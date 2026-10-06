// lower.h — checked expressions to IR. LAYERS.md §3.5, IR.md.
//
// ## Why this has no visitor of its own
//
// LAYERS.md §3.5.1: "The AST is walked once, in the resolver / checker.
// Lowering **queries** that result; it does not re-walk." So this is not an
// ASTVisitor — that is the checker's, and gate T5 counts exactly one. The
// Checker calls into this while it walks, which is what "one traversal" means
// when a phase has to emit as it goes: the type is recorded and the IR is
// emitted on the same visit.
//
// The slice is scalars. Integer and float arithmetic, comparisons, casts,
// calls, returns, if/else, while, locals, parameters and global initialisers.
// Anything else is **refused with a diagnostic** rather than approximated,
// because a phase that receives an Error stops (TYPES.md §7) and emitting a
// plausible instruction for a construct nobody designed is how a wrong answer
// becomes a shipped one.

#ifndef CIDER_LOWER_LOWER_H
#define CIDER_LOWER_LOWER_H

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "check/checker.h"
#include "ir/ir.h"

namespace cider {
namespace lower {

// Where a value lives while a function is being built. A local or parameter
// that is still live is an SSA value; anything needing an address becomes a
// stack slot, which is MEMORY.md §4's territory and not this slice's.
// A local is a stack slot, never an SSA value: a variable assigned in a
// loop and read across the back edge cannot be one version per store without
// phi nodes, so every named place is memory and only temporaries are SSA.
// This is MEMORY.md section 4, and it is what makes `while` correct.
struct LocalBinding {
    // One slot id per word, in order. Scalars hold one; a string holds two.
    // Separate ids rather than base-plus-index: nothing here may assume two
    // allocations land consecutively.
    std::vector<ir::Value> slots;
    TypeRef type;
    bool isMutable = false;
};

struct GlobalBinding {
    std::string name;
    std::string label;
    // The module that defined it: two modules may define one name, and the
    // label is what keeps them apart. Use sites resolve through the
    // current module's scope, never by first match.
    std::string module;
    TypeRef type;
    int64_t initial = 0;
    // Zeroed image words for a zero-repeat array global: the global holds
    // this many `.quad 0` entries instead of one `.quad initial`.
    uint64_t zeroWords = 0;
    // A read-only bytes object this global points at, empty when none. Set
    // for string globals, whose 16 bytes are (rodata address, length).
    std::string initLabel;
    bool isVolatile = false;
};

class Lowerer : public check::LowerSink {
public:
    Lowerer(TypeContext& ctx, resolve::Resolver& resolver, check::Checker& checker);

    // Runs the checker's walk with this lowerer attached, so types are
    // recorded and IR is emitted on the same visit. True unless the program
    // failed any layer or hit a construct not yet lowered. The module stays
    // in the lowerer: lowerInto accumulates imports into it and takeModule
    // moves it out when every module is in.
    bool lowerModule(const std::string& name, Program& program);
    // Lowers one more program into the module lowerModule started: imports
    // first, entry last, so link names resolve in one IR module and rodata
    // labels never collide. Returns false when this module failed.
    bool lowerInto(const std::string& name, Program& program);
    // Moves the accumulated module out. Null when nothing lowered.
    std::unique_ptr<ir::Module> takeModule() { return std::move(module_); }

    const std::vector<check::CheckError>& errors() const { return errors_; }

    // --- called by the Checker during its walk, through check::LowerSink ---
    void onProgram(Program& n) override;
    void onFuncDecl(FuncDecl& n) override;
    void onMethodDecl(const std::string& owner, const std::string& name,
                      const std::vector<Param>& params, const Signature& sig,
                      TypeRef self, Block& body) override;
    void onGlobal(VarDecl& n);
    void onVarDecl(VarDecl& n) override;
    void onExprStmt(ExprStmt& n) override;
    void onReturn(Return& n) override;
    void onBreak(Break& n) override;
    void onContinue(Continue& n) override;
    void onIf(IfStmt& n) override;
    void onWhile(WhileStmt& n) override;
    void onFor(ForStmt& n) override;
    void onForIn(ForInStmt& n) override;
    void onDelete(DeleteStmt& n) override;
    void onAsmStmt(AsmStmt& n) override;
    void onBlockEnter(Block& n) override;
    void onBlockExit(Block& n) override;
    // A closure literal, after its body checked. Emits the hidden function
    // and records where, so lowering the literal itself answers the
    // two-word value (code address, captures pointer).
    void onFuncLit(FuncLit& lit, const std::vector<check::Captured>& captures,
                   const Signature& sig) override;
    ir::Value onBinary(Binary& n);
    // `&&` and `||` with short-circuit control flow: the deciding side
    // leaves its value in a cell, and only the taken arm writes the answer.
    ir::Value lowerShortCircuit(const Binary& n);
    // `?:` with branch control flow: the checker proved both arms share one
    // type, so each arm stores its words into the cell and the join loads
    // them back. The untaken arm never evaluates (SEMANTICS: single-eval).
    std::vector<ir::Value> lowerTernary(const Ternary& n);
    // Prefix/postfix `++` / `--`: the checker proved the operand is an
    // integer place, so the operand loads, steps by one through the same
    // operation `+= 1` would emit, and stores back. Postfix answers the
    // loaded words, prefix the stored one.
    std::vector<ir::Value> lowerIncrDecr(const IncrDecr& n);
    ir::Value onCall(Call& n);
    ir::Value onIdent(Ident& n);
    ir::Value onCast(Cast& n);
    // The slice's expression dispatcher. Returns undefined() and records a
    // diagnostic for a construct this slice does not lower, so nothing is ever
    // approximated.
    ir::Value onIdentOrExpr(const Expr* expr, TypeRef expected);
    // One pinned stack slot for a named place. The result id names the slot;
    // rsp+id*8 is its address by construction, so loads and stores name it
    // through addressBase and the slot never needs materialising.
    ir::Value allocaSlot(TypeRef type, Position pos);
    std::vector<ir::Value> allocaSlots(TypeRef type, Position pos, uint64_t words);
    // The only path from a type to fresh slots: refuses interfaces and
    // anything wordCount cannot size, so no caller casts a -1 count.
    std::vector<ir::Value> allocaFor(TypeRef type, Position pos);
    TypeRef wordType(TypeRef aggregate, uint64_t word);
    std::vector<ir::Value> loadLocal(Position pos, const LocalBinding& binding);
    std::vector<ir::Value> loadGlobal(Position pos, const GlobalBinding& global);
    // A string literal answers its address, its length, and where its bytes
    // live. Expression positions use the words; globals use the label and
    // length for their image. One function so the two cannot disagree.
    struct LoweredString {
        std::vector<ir::Value> words;
        std::string label;
        int64_t len = 0;
    };
    LoweredString lowerString(const StringLit& lit);
    // An interpolation `"a{x}b"` builds its string on the heap: each part
    // answers an address word and a length word, the lengths sum to a
    // total, one cider_alloc answers the buffer, and every part's bytes
    // copy into it in order. Text answers rodata; a string answers its
    // words; a bool selects rodata through a branch; a char stores one
    // byte to scratch; an integer converts through a decimal helper. A
    // float refuses: no buffer spelling exists yet, and guessing one is
    // exactly what the rewrite forbids.
    struct PartBytes {
        ir::Value ptr;
        ir::Value len;
    };
    LoweredString lowerInterp(const StringLit& lit);
    PartBytes rodataPair(const std::string& text, Position pos);
    void copyBytes(ir::Value dst, ir::Value src, ir::Value len, Position pos);
    // How many machine words a value occupies: one for scalars and pointers,
    // two for the 16-byte aggregates (strings today). -1 for anything larger,
    // which this slice does not lower.
    int wordCount(TypeRef type);
    // Lowers any expression to its words. Scalars answer one value; a string
    // literal answers its address and length. Anything larger is refused.
    std::vector<ir::Value> lowerWords(const Expr* expr);
    // `&f` where f names a plain function answers its code address as one
    // word. Every other address refuses: `&x` on values is FEAT-038.
    std::vector<ir::Value> lowerAddrOf(const Unary& n);
    std::vector<ir::Value> lowerElementAddr(const Index& n);
    // Stores every word into the place an identifier names: consecutive
    // slots for a local, the label plus displacement for a global. Answers
    // the stored words, so an assignment is still an expression. Anything
    // else is refused, because fields and elements need aggregates and this
    // slice has none.
    std::vector<ir::Value> storeToIdent(const Ident& target,
                                        const std::vector<ir::Value>& words);
    // A place something lives: local slots or one global label, plus a byte
    // offset into it for fields and elements. Nested field chains accumulate
    // the offset while the slots stay the base value's.
    //
    // A store through a union member also writes the union's tag: every union
    // crossed on the way to the member records its tag offset (from the
    // base) and the member's index, outermost first. A read ignores these;
    // a store writes each tag after the payload, because a write sets the
    // tag as a consequence (TYPES.md 4.6).
    struct TagWrite {
        int64_t off = 0;
        int64_t index = 0;
    };
    struct Place {
        std::vector<ir::Value> slots;
        std::string global;
        TypeRef type;
        int64_t off = 0;
        bool isVolatile = false;
        std::vector<TagWrite> tagWrites;
        // Set when the object sits behind a loaded pointer: the address as
        // a value, not a slot. Slot-precise logic must not read slots then.
        bool hasIndirect = false;
        ir::Value indirect;
    };
    // Names a place: identifiers, and field chains over them. Anything else
    // (a call result, arithmetic) has no address and is refused.
    bool resolvePlace(const Expr* expr, Place& out);
    // One scalar load/store at a place plus offset. The bytes must sit inside
    // a single word; anything straddling words is refused, not split, because
    // a partial-word write that guessed its neighbors would corrupt them.
    // A struct/array field that fits one word is addressed by offset and
    // size, never by scalar type: the byte count names an unsigned width and
    // the loaded value takes that width's type, so width and type never
    // disagree downstream. Only whole widths exist; any other byte count
    // refuses rather than splits.
    bool aggregateWord(int64_t bytes, ir::IRType& width, TypeRef& type);
    // A runtime address for slot bytes the slot grid cannot name: the slot
    // base plus a constant byte offset, added at runtime. The result names
    // no slot, so slot-precise optimizations treat it conservatively.
    ir::Value computedBase(ir::Value slot, int64_t byteOff, Position pos);
    std::vector<ir::Value> loadRange(Position pos, const Place& place, TypeRef fieldType);
    void storeRange(Position pos, const Place& place,
                    const std::vector<ir::Value>& words, TypeRef fieldType);
    // Construction into given slots, field by field (element by element). A
    // nested value lands word-aligned or is refused: unaligned multi-word
    // copies guess neighbor bytes.
    void constructStruct(const std::vector<ir::Value>& slots, const NewStruct& n);
    void constructArray(const std::vector<ir::Value>& slots, const NewArray& n);
    // Zero-repeat support for construction: a scalar-zero filler over an
    // aggregate element zeroes every scalar instead of lowering one value.
    bool isZeroRepeatFiller(const Expr& filler, TypeRef element);
    // A class constructs its fields after a null vptr word; methods wire
    // the vptr in a later slice. A class with bases is refused: inherited
    // fields have no layout yet.
    void constructClass(const std::vector<ir::Value>& slots, const NewStruct& n);
    // A union constructs its one live member at offset 0 and writes the
    // member's index to the tag after the payload (TYPES.md 4.6).
    void constructUnion(const std::vector<ir::Value>& slots, const NewStruct& n);
    // Loads a union's tag and compares it to the expected member index. The
    // answer is one boolean word: true when the member is live.
    ir::Value emitTagCheck(Position pos, const Place& base, int64_t tagOff, int64_t index);
    // Branches to a union-tag trap when `ok` is false, continuing in a fresh
    // block that the caller owns: instructions the caller emits next land
    // past the check, never on the trap path.
    void emitTagTrap(Position pos, ir::Value ok);
    // Branches to a trap of the given reason when `ok` is false, continuing
    // in a fresh block the caller owns. emitTagTrap is the union-tag shape
    // of this; slice bounds use it with IndexOutOfBounds (SEMANTICS 8).
    void emitCheckTrap(Position pos, ir::Value ok, ir::TrapReason reason);
    // A signed i64 comparison answering one boolean word, for bounds checks
    // the source types already fixed at i64 (SEMANTICS 2.3).
    ir::Value emitI64Cmp(Position pos, BinOp op, ir::Value a, ir::Value b);
    // A slice expression answers its two words, pointer then length: the
    // base address advanced past the low bound, and high minus low. Bounds
    // the compiler cannot prove emit runtime checks; provable violations
    // refuse instead of trapping (SEMANTICS 8).
    std::vector<ir::Value> lowerSlice(const SliceExpr& n);
    // Folds one bound of a slice or index: true with the value when the
    // compiler names it, false when it is a runtime value.
    bool foldBound(const Expr* expr, int64_t& value);
    // Reads one element through a slice or string base: the pointer plus a
    // scaled index, guarded below by zero and above by the runtime length.
    std::vector<ir::Value> lowerSliceIndex(const Index& n, const Place& base,
                                           TypeRef baseType);
    // Names the element's address through a slice or string base: guards
    // first, then the pointer plus a scaled index. Shared by the read and
    // the store so the two cannot disagree about what is in bounds.
    bool sliceElementPlace(const Index& n, const Place& base, TypeRef baseType,
                           TypeRef& element, Place& at);
    // Writes one element through a slice base: the same guards and address
    // as the read, then an indirect store. Strings never arrive here; the
    // checker rejects their stores first.
    void storeSliceIndex(const Index& target, const Place& base,
                         TypeRef baseType, const std::vector<ir::Value>& words);
    // The address of one array element for a COMPUTED index: base plus
    // index times bytes, guarded both sides (SEMANTICS §8 checks every
    // access). Reads, stores, and address-of share it so the guards
    // cannot drift apart. Folded indexes never arrive: the direct slot
    // route owns those.
    bool arrayElementPlace(const Index& n, const Place& base,
                           TypeRef arrayType, const char* what, Place& at);
    std::vector<ir::Value> lowerNewStruct(const NewStruct& n);
    std::vector<ir::Value> lowerNewArray(const NewArray& n);
    std::vector<ir::Value> lowerNewClass(const NewClass& n);
    // Answers a closure literal's two words: the hidden function's address
    // and the captures allocation, filled from the enclosing locals. Never
    // invents a layout; the checker hands the captures over with the hook.
    std::vector<ir::Value> lowerFuncLit(const FuncLit& lit);
    // A bare function name in value position answers a closure pair, but a
    // plain function takes no receiver while every closure call passes one.
    // The forwarder is a hidden function with the receiver-first shape that
    // drops the captures word and calls through, so the call site stays
    // uniform and never learns which kind of code it holds.
    std::string forwardFunction(const std::string& link, const Signature& sig,
                                Position pos);
    std::vector<ir::Value> lowerField(const Field& n);
    // A union member read checks the tag first: mismatch traps, and only the
    // live member's bytes load (TYPES.md 4.6). The tag is never statically
    // known here, so every read checks; static tracking is a later slice.
    std::vector<ir::Value> lowerUnionField(const Field& n, const Place& base);
    std::vector<ir::Value> lowerIndex(const Index& n);
    void storeIndex(const Index& target, const std::vector<ir::Value>& words);
    // One arithmetic operation, shared by Binary and compound assignment so
    // the two cannot disagree about what `+` emits.
    ir::Value emitArith(BinOp op, ir::Value a, ir::Value b, TypeRef result,
                        Position pos);
    // Assignments evaluate the value (computing first for a compound form)
    // and store it into the place the target names.
    ir::Value lowerAssign(Assign& n);
    // The statement form stores without answering a value, so several words
    // are fine where the expression form must refuse.
    void lowerAssignStmt(Assign& n);
    // The target stores shared by both forms. True when the store emitted.
    bool storeAssignTarget(Assign& n, const std::vector<ir::Value>& value);

    // Emitted by the expression hooks above, in visit order. The Checker calls
    // one of these for each expression it types.
    void emit(ir::Instr instr);
    ir::Value newValue(ir::IRType width, TypeRef type, Position pos);

    // The `wrap` flag is per file and inlining crosses files, so it travels on
    // each instruction (IR.md §3.1).
    bool wrapEnabled() const { return wrap_; }
    void setWrap(bool on) { wrap_ = on; }

    // The signature of a function by name, for direct calls.
    const Signature* functionSig(const std::string& name) const;

private:
    void refuse(Position pos, const std::string& what);
    void emitTerminator(const ir::Terminator& term);
    ir::Value constantI64(int64_t value, TypeRef type, Position pos);
    // Classify the register class of every parameter word once the slots
    // are placed; the code generator reads it straight from the function.
    void recordParamClasses();
    // A float constant: the IEEE bits for the literal's own width, stored
    // in `constantBits` exactly as an integer constant stores its value.
    ir::Value constantFBits(uint64_t bits, TypeRef type, Position pos);
    ir::Value lowerFloatLit(const FloatLit& lit);
    ir::Value lowerNeg(const Unary& n);
    // Usual float conversion (SEMANTICS §2.3): an integer or narrower
    // float value converts to the target float width; anything else
    // passes through unchanged.
    ir::Value widenFloat(ir::Value v, TypeRef from, TypeRef to, Position pos);
    // The compound operation shared by the expression and statement
    // assignment paths (the right-hand side converts to the target's
    // float width first); refuses in place on a malformed compound.
    bool compoundValue(Assign& n, std::vector<ir::Value>& value);
    ir::IRType widthOf(TypeRef type, Position pos);

    uint32_t emitBlock(Position pos);
    void startBlock(uint32_t label);
    void sealBlock(uint32_t label);
    uint32_t current() const { return current_; }
    ir::Value undefined() const;

    TypeContext& ctx_;
    resolve::Resolver& resolver_;
    check::Checker& checker_;
    std::unique_ptr<ir::Module> module_;
    ir::Function* function_ = nullptr;
    bool enabled_ = false;
    bool wrap_ = false;
    bool failed_ = false;
    uint32_t valueSeq_ = 0;
    uint32_t labelSeq_ = 0;
    // The innermost enclosing loop's two jump targets: `continue` goes to
    // the head (the condition is re-evaluated), `break` to the exit. Nested
    // loops push their own frame, so an inner break leaves only the inner
    // loop. The checker already rejects a break outside every loop, so a
    // jump with an empty stack is a lowerer bug and refuses.
    struct LoopTargets {
        uint32_t head;
        uint32_t exit;
    };
    std::vector<LoopTargets> loops_;
    uint32_t rodataSeq_ = 0;
    uint32_t current_ = 0;
    bool terminated_ = false;
    std::vector<uint32_t> blockOrder_;
    std::vector<std::unordered_map<std::string, LocalBinding>> locals_;
    // One entry per entered block, recording the locals depth from before
    // the block: the exit truncates to it, so a refusal mid-block (or a
    // block entered already-failed) can neither leak frames nor steal the
    // enclosing function's frame. Enter and exit are always paired by the
    // checker, so this stack can never underflow while locals_ can.
    std::vector<uint64_t> scopeMarks_;
    std::vector<GlobalBinding> globals_;
    // The binding a bare global name means here: resolved through the
    // current module's scope to the declaration's home module, never by
    // whichever module lowered first. Null when no binding with that home
    // exists yet (a module the walk has not reached is a loud refusal at
    // the use, not a silent first match).
    const GlobalBinding* findGlobal(const std::string& name);
    std::unordered_map<std::string, std::string> globalNames_;
    std::vector<check::CheckError> errors_;
    // Hidden closure functions, minted in first-sight order. The site that
    // lowers a literal reads its entry back for the code word and layout.
    struct ClosureSite {
        std::string hidden;
        std::vector<check::Captured> captures;
    };
    std::unordered_map<const FuncLit*, ClosureSite> closures_;
    uint64_t closureSeq_ = 0;
};

}  // namespace lower
}  // namespace cider

#endif  // CIDER_LOWER_LOWER_H
