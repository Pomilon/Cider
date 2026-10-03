// ir.h — the instruction set, the aggregate representation and the ABI.
// IR.md, and the hand-off between lowering and the optimiser.
//
// Three flags are per-instruction because real optimisations depend on them,
// and v2 lost the same information three different ways:
//
//   mayTrap   dead-code elimination, LICM and inlining all need to know
//             whether an instruction can fault. Without it DCE deletes a
//             bounds check, or LICM hoists a division out of a loop and
//             changes *which input* faults.
//   volatile  FEATURES.md §1.1c promises a volatile access is never
//             eliminated, merged, hoisted or reordered.
//   overflow  `# experimental(wrap)` is a *file* property and inlining
//             crosses files, so it travels on the instruction.
//
// SSA is the shape lowering emits, not a later pass: every value is defined
// once and joins use phi, so the definition dominates its uses — the property
// every pass here relies on.
//
// An aggregate is never one value. An IRType is what a *register* holds, and a
// struct is not that; so an aggregate is N values and N comes from layout.h
// (IR.md §1). An `Value` carries both halves of what it is: `width`, the
// machine type of the slot, and `type`, the language type it came from.
// Neither is derivable from the other.

#ifndef CIDER_IR_IR_H
#define CIDER_IR_IR_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "lexer/token.h"
#include "types/signature.h"
#include "types/type.h"

namespace cider {
namespace ir {

// Thirteen machine types and no others. `layout.h` derives width and
// alignment from these, and nothing derives a width from an IRType twice.
enum class IRType {
    I8, U8, I16, U16, I32, U32, I64, U64, F32, F64, Bool, Ptr, Void,
};

const char* irTypeName(IRType type);

// One machine word of one IRType. `type` is the language type it came from:
// `width` alone cannot say whether an I32 slot holds an i32, a u32 or a
// bitfield's bits, and `type` alone cannot say how many slots there are.
struct Value {
    uint32_t id = 0;
    IRType width = IRType::Void;
    TypeRef type;
};

// The signed and unsigned comparison operations are separate opcodes, not a
// flag: SEMANTICS §2.3 decides signedness once and the instruction records it.
// An implementer who emits `jb` where `jl` was needed gets a different answer
// for every negative number.
enum class CmpOp {
    Eq, Ne, Lt, Le, Gt, Ge,      // signed
    Ult, Ule, Ugt, Uge,          // unsigned
};

const char* cmpOpName(CmpOp op);

enum class TrapReason {
    IndexOutOfBounds,
    DivByZero,
    MinDivNegOne,
    BitfieldOverflow,
    UnionTag,
    Downcast,
    Null,
    Explicit,
};

const char* trapReasonName(TrapReason reason);

enum class OverflowMode {
    Undefined,   // the default: overflow is undefined (SEMANTICS §2.1)
    Wrap,        // `# experimental(wrap)`: defined to wrap at the result width
};

enum class Op {
    Const, Copy,
    Add, Sub, Mul, Div, Rem,
    And, Or, Xor, Shl, Shr,
    Neg, Not, BitNot,
    FAdd, FSub, FMul, FDiv, FRem,
    Cmp,
    Load, Store, Alloca,
    Extract, Insert, Gep,
    Call, Cast, Trap, Asm,
};

const char* opName(Op op);

// A `call` carries its landing pad rather than `throw` being an instruction:
// unwinding is then the same machinery as every other branch, and a throw is a
// branch to a block the programmer named (IR.md §5).
struct CallInfo {
    bool indirect = false;
    std::optional<uint32_t> landingPad;
};

// One argument of a call. Its own type rather than signature.h's Parameter,
// because an argument here is already-checked and already-typed: it is a value,
// not a declaration.
struct Argument {
    Value value;
};

struct Instr {
    Op op = Op::Const;
    IRType width = IRType::Void;
    TypeRef type;
    Position loc{"", 1, 1};

    // The three flags, and `pure` because "no side effect, so it may move or
    // be deleted" is what a pass reads to decide either.
    bool mayTrap = false;
    bool isVolatile = false;
    bool isPure = false;
    OverflowMode overflow = OverflowMode::Undefined;

    std::vector<Value> results;
    std::vector<Value> operands;

    // Op-specific payload, all inert unless the Op uses them, so the
    // instruction set stays closed and readable rather than becoming a variant
    // per operation.
    int64_t constantBits = 0;        // Const: the value, in `width`
    std::string constantText;        // Const: the spelling, for an IR dump
    Value addressBase;               // Load/Store/Extract/Insert/Gep
    TypeRef pointee;                 // the same
    // A byte displacement added to the base. Named `displacement`
    // rather than `offset`: it is one word of an instruction, not a layout
    // fact, and T4 watches the other word.
    int64_t displacement = 0;
    uint32_t field = 0;              // Extract/Insert: by index, never by name
    Value storedValue;               // Store/Insert
    CmpOp cmp = CmpOp::Eq;           // Cmp
    std::string callee;              // Call: a function name
    std::string globalLabel;         // Load/Store: the global's link label
    Value calleeValue;               // Call: indirect
    std::vector<Argument> arguments; // Call
    CallInfo call;                   // Call
    TrapReason reason = TrapReason::Explicit;  // Trap
    std::string asmText;             // Asm
    std::vector<std::string> asmNames;
};

// Every block ends in exactly one terminator, and every branch target names a
// block of this function. Those are the two structural rules gate L4 checks.
enum class Term {
    Ret,
    Br,
    CondBr,
    Switch,
    Unreachable,
    Trap,
};

struct Case {
    Value value;
    uint32_t target = 0;
};

struct Terminator {
    Term term = Term::Unreachable;
    std::optional<Value> value;      // Ret
    uint32_t target = 0;             // Br
    std::optional<Value> cond;       // CondBr
    uint32_t thenTarget = 0;
    uint32_t elseTarget = 0;
    Value switchValue;               // Switch
    std::vector<Case> cases;
    uint32_t defaultTarget = 0;      // Switch
    TrapReason reason = TrapReason::Explicit;  // Trap
    Position loc{"", 1, 1};
};

struct Block {
    uint32_t label = 0;
    std::vector<Instr> body;         // no terminator in here
    Terminator term;                 // exactly one
    Position loc{"", 1, 1};
};

struct Function {
    std::string name;
    Signature signature;
    // One value per signature parameter, in order. Parameters are SSA values:
    // still live, so they live in a register. The structural check treats
    // these as defined, which is what "or is a function parameter" means.
    std::vector<Value> params;
    bool isExtern = false;
    std::vector<Block> blocks;
    Position loc{"", 1, 1};

    // Set by lowering, read by the structural check and by codegen: a call is
    // mayTrap only if the callee can throw, which is known here rather than
    // guessed later.
    bool canThrow = false;
};

// A module global: its link label, its type, and its initial bytes. The
// initial value is constant (FEATURES.md 7.3 allows nothing else), so there
// is no startup code and no order problem — the image carries the bytes.
struct Global {
    std::string label;
    TypeRef type;
    int64_t initial = 0;
};

struct Module {
    std::string name;
    std::vector<Function> functions;
    std::vector<Global> globals;
};

}  // namespace ir
}  // namespace cider

#endif  // CIDER_IR_IR_H
