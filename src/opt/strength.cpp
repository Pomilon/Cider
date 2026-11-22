// strength.cpp — bit-exact integer strength reduction and peephole identities.
//
// This pass runs after constant folding and before control-flow simplification.
// Constant folding has already replaced every operation whose operands are all
// known; what remains is the shape where one side is a constant and the other
// is a runtime value. The rules below rewrite only the operations whose answer
// is exactly the surviving operand's word, or exactly zero, at the
// instruction's own width and signedness:
//
//   Add x,0 / 0,x          -> Copy x
//   Sub x,0                -> Copy x
//   Or  x,0 / 0,x          -> Copy x
//   Xor x,0 / 0,x          -> Copy x
//   Shl x,0, Shr x,0       -> Copy x
//   Mul x,1 / 1,x          -> Copy x
//   And x,all-ones / ...   -> Copy x   (all-ones at the exact integer width)
//   Div x,1                -> Copy x
//   Rem x,1                -> Const 0
//   Mul x,0 / 0,x          -> Const 0
//
// Why each is exact. Integer addition, subtraction, bitwise or, xor, and
// shift by zero are the identity at every width. Multiplication by one is the
// identity. A bitwise and with every bit of the width set keeps every bit of
// the value. Division by one is the identity for both signed and unsigned
// forms, and no division by one can trap: the only signed division overflow is
// the width's minimum divided by minus one, which a divisor of one cannot
// reach; a zero divisor is the other trap and one is not zero. Remainder by one
// is zero for every dividend, again without a trap. Multiplication by zero is
// zero at every integer width, including the 8-bit path whose product lives in
// sixteen bits before extension: zero times anything is zero at either width.
//
// Evaluation and traps are unchanged. `x` is an SSA operand, already defined
// before this instruction; replacing the operation never deletes, moves or
// duplicates the definition of `x`. A later dead-code pass may delete a value
// that is now unused, but it refuses to delete anything trapping or volatile.
// A launch of a trapping operation therefore stays exactly where it was. Only
// the replaced instruction loses a flag: a Div/Rem that this pass proves
// cannot trap becomes a Copy/Const with `mayTrap` clear, because the
// replacement itself cannot fault. `isVolatile` and trapping instructions are
// never rewritten here.
//
// Constants are matched exactly. A direct `Const` is recognised, or a `Copy`
// chain whose every result has the source's width and language type. The
// constant and both operands must have the instruction result's exact
// `IRType` and `TypeRef`; a same-bit constant at another width or language
// type is refused rather than coerced. Only the eight integer widths I8/U8,
// I16/U16, I32/U32 and I64/U64 participate: floats, Bool, Ptr and Void are
// outside the pass.
//
// Two more exact rules are applied. At an unsigned integer width whose divisor
// is a single set bit, `Rem x,2^k -> And x,2^k-1`: every unsigned value equals
// its low-bit mask, so the remainder is the same word and cannot trap. Signed
// remainders are not proven non-negative and stay trapping Rem instructions; a
// zero divisor is not a power of two and stays too. For I32/U32/I64/U64,
// `Mul x,2^k -> Shl x,k` is exact: at those widths the product wraps on exactly
// the bits the shift keeps. A constant with more than one use is never mutated,
// so a divisor read elsewhere keeps its original spelling.
//
// A single set-bit divisor can reach a Rem (or a Mul) through a value-preserving
// integer Cast of a Const as well as directly — e.g. `cast u64 (const i32 64)`
// from a named capacity shared by other users. Such a single-use Cast is
// resolved to the constant it carries (only widening, non-negative values, so
// sign and zero extension agree) and is rewritten into a Const of the new
// operand in place. When the divisor is a Const that several instructions read,
// mutating it would corrupt those other readers, so a fresh Const is inserted
// immediately before the rewritten instruction and only that operand is
// repointed. Both paths keep the single-use Const mutation for the common case.
//
// The remaining optional strength reductions are deliberately absent. `Sub 0,x
// -> Neg` and `Xor x,-1 -> BitNot` would change the 64-bit word representation
// at sub-64 widths, because the emitter and VM implement Neg/BitNot without
// re-extending the answer to the value's canonical width word while Sub/Xor do
// extend. Consumers re-extend on read, but this pass does not rely on that
// wider argument. `Mul x,2^k -> Shl x,k` is absent at I8/U8/I16/U16: those
// multiply paths compute a wider product before extension, so the wrap shape
// differs from a shift at that width. A checked, width-covering proof for the
// narrow paths is future work.
//
// Rewrites preserve the result Value (id, width, type), the instruction's
// location and overflow mode. The replacement is built from an empty Instr so
// every payload field the old operation used — call arguments, asm text, phi
// incoming, address base, stored value and the rest — is cleared. The pass
// repeats to a fixpoint because a rewritten `Const` can make a later
// instruction's operand provably zero or one; each rewrite turns one
// operation into Copy or Const, which no rule matches again, so the loop is
// monotone and bounded by the instruction count. Scans follow block and body
// order; hash tables are only looked up by id, never iterated for a decision.

#include "opt/opt.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace cider {
namespace opt {

namespace {

constexpr uint64_t kOneResult = 1;
constexpr uint64_t kOneOperand = 1;
constexpr uint64_t kTwoOperands = 2;
constexpr uint32_t kLastFreshId = 0xFFFFFFFEu;  // 0xFFFFFFFF is "no value".

// A fresh mask Const requested while rewriting a multi-use divisor. The caller
// inserts it just before the rewritten instruction so def-before-use holds.
struct NewConst {
    uint32_t id = 0;
    int64_t bits = 0;
    ir::IRType width = ir::IRType::Void;
    TypeRef type;
    Position loc;
};

// One integer constant as the defining Const produced it. `width` and `type`
// travel with the bits so every match can demand the operation's exact
// width and language type rather than comparing bit patterns across a
// coercion the IR never performed.
struct Constant {
    uint64_t bits = 0;
    ir::IRType width = ir::IRType::Void;
    TypeRef type;
};

bool isIntegerWidth(ir::IRType width) {
    switch (width) {
        case ir::IRType::I8:
        case ir::IRType::U8:
        case ir::IRType::I16:
        case ir::IRType::U16:
        case ir::IRType::I32:
        case ir::IRType::U32:
        case ir::IRType::I64:
        case ir::IRType::U64:
            return true;
        case ir::IRType::F32:
        case ir::IRType::F64:
        case ir::IRType::Bool:
        case ir::IRType::Ptr:
        case ir::IRType::Void:
            return false;
    }
    return false;
}

int widthBits(ir::IRType width) {
    switch (width) {
        case ir::IRType::I8:
        case ir::IRType::U8:
            return 8;
        case ir::IRType::I16:
        case ir::IRType::U16:
            return 16;
        case ir::IRType::I32:
        case ir::IRType::U32:
            return 32;
        case ir::IRType::I64:
        case ir::IRType::U64:
            return 64;
        default:
            return 0;
    }
}

// The low bits a value of this width owns. The IR carries a 64-bit word and a
// narrow value's high bits are not part of its value, so zero, one and
// all-ones are compared at the declared width.
uint64_t widthMask(ir::IRType width) {
    const int bits = widthBits(width);
    if (bits <= 0 || bits >= 64) {
        return ~uint64_t{0};
    }
    return (uint64_t{1} << bits) - 1;
}

bool isZero(const Constant& constant) {
    return (constant.bits & widthMask(constant.width)) == 0;
}

bool isOne(const Constant& constant) {
    return (constant.bits & widthMask(constant.width)) == 1;
}

bool isAllOnes(const Constant& constant) {
    const uint64_t mask = widthMask(constant.width);
    return (constant.bits & mask) == mask;
}

bool sameAsResult(const ir::Value& value, const ir::Instr& instr) {
    return value.width == instr.width && value.type == instr.type;
}

// Largest value id anywhere in the function, including operands and
// terminators. Fresh ids are handed out above every id in the module so a new
// Const can never collide with an existing definition or use.
uint32_t maxValueId(const ir::Function& fn) {
    uint32_t top = 0;
    auto note = [&](uint32_t id) {
        if (id > top && id != 0xFFFFFFFFu) {
            top = id;
        }
    };
    for (const ir::Value& param : fn.params) {
        note(param.id);
    }
    for (const ir::Block& block : fn.blocks) {
        for (const ir::Instr& instr : block.body) {
            for (const ir::Value& value : instr.results) note(value.id);
            for (const ir::Value& value : instr.operands) note(value.id);
            for (const ir::PhiIncoming& incoming : instr.phiIncoming) {
                note(incoming.value.id);
            }
            if (instr.op == ir::Op::Load || instr.op == ir::Op::Store ||
                instr.op == ir::Op::Extract || instr.op == ir::Op::Insert ||
                instr.op == ir::Op::Gep) {
                note(instr.addressBase.id);
            }
            if (instr.op == ir::Op::Store || instr.op == ir::Op::Insert) {
                note(instr.storedValue.id);
            }
            if (instr.op == ir::Op::Call && instr.call.indirect) {
                note(instr.calleeValue.id);
            }
            for (const ir::Argument& arg : instr.arguments) {
                for (const ir::Value& value : arg.words) note(value.id);
            }
        }
        for (const ir::Value& value : block.term.values) note(value.id);
        if (block.term.cond.has_value()) note(block.term.cond->id);
        if (block.term.term == ir::Term::Switch) {
            note(block.term.switchValue.id);
            for (const ir::Case& arm : block.term.cases) note(arm.value.id);
        }
    }
    return top;
}

ir::Instr newConstInstruction(const NewConst& request) {
    ir::Instr instr;
    instr.op = ir::Op::Const;
    instr.width = request.width;
    instr.type = request.type;
    instr.loc = request.loc;
    instr.isPure = true;
    instr.results.push_back(
        ir::Value{request.id, request.width, request.type});
    instr.constantBits = request.bits;
    instr.constantText = std::to_string(request.bits);
    return instr;
}

// Snapshot of every semantic use count in the function. Taken once per
// fixpoint iteration so a rewrite that repoints one reader cannot make a
// still-shared constant look single-use to a later rewrite in the same scan.
std::unordered_map<uint32_t, uint64_t> computeUseCounts(
    const ir::Function& fn);

// All constants the current IR proves. Direct Const definitions are recorded
// first, then same-width/same-type Copy chains and single-use value-preserving
// integer Casts of known constants are followed to a fixed point. A Copy is
// resolved only when the value it moves carries exactly its own width and
// type, which is the same rule simplification and value numbering use before
// substituting a copy. A Cast is resolved only when it has one use and its
// source bits survive the conversion unchanged, so rewriting the Cast in place
// cannot surprise another reader. Never iterates the table, so the map is
// deterministic.
std::unordered_map<uint32_t, Constant> resolveConstants(
    const ir::Function& fn,
    const std::unordered_map<uint32_t, uint64_t>& uses) {
    std::unordered_map<uint32_t, Constant> known;
    for (const ir::Block& block : fn.blocks) {
        for (const ir::Instr& instr : block.body) {
            if (instr.op != ir::Op::Const || instr.results.size() != kOneResult ||
                instr.isVolatile || !instr.isPure) {
                continue;
            }
            const ir::Value& result = instr.results[0];
            if (!isIntegerWidth(result.width)) {
                continue;
            }
            Constant constant;
            constant.bits = static_cast<uint64_t>(instr.constantBits);
            constant.width = result.width;
            constant.type = result.type;
            known[result.id] = constant;
        }
    }
    for (;;) {
        bool changed = false;
        for (const ir::Block& block : fn.blocks) {
            for (const ir::Instr& instr : block.body) {
                if (instr.results.size() != kOneResult || instr.isVolatile ||
                    instr.mayTrap) {
                    continue;
                }
                const ir::Value& result = instr.results[0];
                const ir::Value& source = instr.operands.empty()
                                              ? result
                                              : instr.operands[0];
                if (instr.op == ir::Op::Copy) {
                    if (!instr.isPure ||
                        instr.operands.size() != kOneOperand) continue;
                    if (source.width != result.width ||
                        !(source.type == result.type) ||
                        known.find(result.id) != known.end()) {
                        continue;
                    }
                    auto found = known.find(source.id);
                    if (found == known.end()) continue;
                    known.emplace(result.id, found->second);
                    changed = true;
                } else if (instr.op == ir::Op::Cast) {
                    // The lowerer's `as` path does not set `isPure` on a Cast
                    // even though it is a side-effect-free numeric conversion,
                    // so only the volatile/trap flags gate this recognition.
                    if (instr.operands.size() != kOneOperand ||
                        !isIntegerWidth(result.width) ||
                        !isIntegerWidth(source.width) ||
                        !sameAsResult(result, instr) ||
                        known.find(result.id) != known.end()) {
                        continue;
                    }
                    auto found = known.find(source.id);
                    if (found == known.end()) continue;
                    const Constant& from = found->second;
                    auto counted = uses.find(result.id);
                    if (from.width != source.width ||
                        !(from.type == source.type) ||
                        counted == uses.end() || counted->second != 1) {
                        continue;
                    }
                    // Only widen, and only when the source value's sign bit is
                    // clear: then zero and sign extension produce the same
                    // bits, so the Const we infer is the value the Cast carries.
                    const int fromBits = widthBits(from.width);
                    const int toBits = widthBits(result.width);
                    if (fromBits <= 0 || toBits <= 0 || fromBits > toBits) {
                        continue;
                    }
                    const uint64_t bits = from.bits & widthMask(from.width);
                    if ((bits >> (fromBits - 1)) != 0) continue;
                    Constant resized;
                    resized.bits = bits;
                    resized.width = result.width;
                    resized.type = result.type;
                    known.emplace(result.id, resized);
                    changed = true;
                }
            }
        }
        if (!changed) {
            return known;
        }
    }
}

// One constant operand of `instr`, or nullptr when it is not a direct constant
// at the instruction's exact width and language type. The operand Value must
// carry that same width/type too; a Const with the right result but an operand
// spelling another width is malformed and refused rather than guessed at.
const Constant* lookup(const std::unordered_map<uint32_t, Constant>& known,
                       const ir::Value& operand, const ir::Instr& instr) {
    if (operand.width != instr.width || !(operand.type == instr.type)) {
        return nullptr;
    }
    auto found = known.find(operand.id);
    if (found == known.end()) {
        return nullptr;
    }
    if (found->second.width != instr.width ||
        !(found->second.type == instr.type)) {
        return nullptr;
    }
    return &found->second;
}

// Replace `instr` with a clean `Copy` of `source`, keeping the result Value
// and the source location. Built from an empty Instr, so no payload or flag
// of the old arithmetic survives. Overflow is preserved: the identity holds
// under both Undefined and Wrap, and the field is inert on a Copy.
void makeCopy(ir::Instr& instr, const ir::Value& source) {
    const ir::Value result = instr.results[0];
    const ir::IRType width = instr.width;
    const TypeRef type = instr.type;
    const Position loc = instr.loc;
    const ir::OverflowMode overflow = instr.overflow;
    instr = ir::Instr{};
    instr.op = ir::Op::Copy;
    instr.width = width;
    instr.type = type;
    instr.loc = loc;
    instr.isPure = true;
    instr.overflow = overflow;
    instr.results.push_back(result);
    instr.operands.push_back(source);
}

// Replace `instr` with a clean integer `Const` zero at the same result width
// and type. Used for Rem x,1 and Mul x,0; both answers are exact zero.
void makeZero(ir::Instr& instr) {
    const ir::Value result = instr.results[0];
    const ir::IRType width = instr.width;
    const TypeRef type = instr.type;
    const Position loc = instr.loc;
    const ir::OverflowMode overflow = instr.overflow;
    instr = ir::Instr{};
    instr.op = ir::Op::Const;
    instr.width = width;
    instr.type = type;
    instr.loc = loc;
    instr.isPure = true;
    instr.overflow = overflow;
    instr.results.push_back(result);
    instr.constantBits = 0;
    instr.constantText = "0";
}

// Unsigned integer types only: a negative signed value is not equal to its
// low-bit mask, so signed Rem must stay a trapping Rem.
bool unsignedAt(ir::IRType width, const TypeRef& type) {
    if (type.isError() || type.type == nullptr ||
        !type.type->basic().has_value()) {
        return false;
    }
    switch (*type.type->basic()) {
        case cider::BasicKind::U8:  return width == ir::IRType::U8;
        case cider::BasicKind::U16: return width == ir::IRType::U16;
        case cider::BasicKind::U32: return width == ir::IRType::U32;
        case cider::BasicKind::U64: return width == ir::IRType::U64;
        default: return false;
    }
}

// The position of the single set bit, or -1 for zero or a multi-bit value.
int log2SingleBit(uint64_t bits) {
    if (bits == 0 || (bits & (bits - 1)) != 0) {
        return -1;
    }
    int n = 0;
    while ((bits >> n) != 1) {
        ++n;
    }
    return n;
}

// Count every semantic use of every id. A value's own definition is not a use,
// so a count of one means only the instruction being rewritten reads it.
std::unordered_map<uint32_t, uint64_t> computeUseCounts(
    const ir::Function& fn) {
    std::unordered_map<uint32_t, uint64_t> counts;
    auto note = [&](const ir::Value& v) {
        if (v.id != 0xFFFFFFFFu) ++counts[v.id];
    };
    for (const ir::Block& b : fn.blocks) {
        for (const ir::Instr& in : b.body) {
            for (const ir::Value& v : in.operands) note(v);
            for (const ir::Argument& a : in.arguments) {
                for (const ir::Value& v : a.words) note(v);
            }
            for (const ir::PhiIncoming& p : in.phiIncoming) note(p.value);
            if (in.op == ir::Op::Load || in.op == ir::Op::Store ||
                in.op == ir::Op::Extract || in.op == ir::Op::Insert ||
                in.op == ir::Op::Gep) {
                note(in.addressBase);
            }
            if (in.op == ir::Op::Store || in.op == ir::Op::Insert) {
                note(in.storedValue);
            }
            if (in.op == ir::Op::Call && in.call.indirect) note(in.calleeValue);
        }
        for (const ir::Value& v : b.term.values) note(v);
        if (b.term.cond.has_value()) note(*b.term.cond);
        if (b.term.term == ir::Term::Switch) {
            note(b.term.switchValue);
            for (const ir::Case& c : b.term.cases) note(c.value);
        }
    }
    return counts;
}

// Change the opcode in place and give its divisor operand the new value
// (`bits` is the mask for Rem, the shift amount for Mul). Three shapes are
// supported, all keeping def-before-use and never changing a shared constant:
//   * a single-use Const is mutated in place, where it already sits;
//   * a single-use integer Cast is replaced in place by a Const of `bits` —
//     its only reader is this instruction, so nothing else sees the change;
//   * a multi-use Const is left alone and a fresh Const is queued for the
//     caller to insert just before `instr`, with only this operand repointed.
bool rewriteDivisor(ir::Function& fn, ir::Instr& instr, ir::Op op,
                    int64_t bits,
                    const std::unordered_map<uint32_t, uint64_t>& uses,
                    std::vector<NewConst>& pending, uint64_t& nextFresh) {
    const uint32_t id = instr.operands[1].id;
    const auto counted = uses.find(id);
    const uint64_t count = counted == uses.end() ? 0 : counted->second;
    for (ir::Block& b : fn.blocks) {
        for (ir::Instr& in : b.body) {
            if (in.results.empty() || in.results[0].id != id) {
                continue;
            }
            if (in.op == ir::Op::Const) {
                if (count == 1) {
                    in.constantBits = bits;
                    in.constantText = std::to_string(bits);
                    instr.op = op;
                    instr.mayTrap = false;
                    instr.isPure = true;
                    return true;
                }
                if (nextFresh > kLastFreshId) return false;
                const uint32_t fresh = static_cast<uint32_t>(nextFresh++);
                pending.push_back(
                    NewConst{fresh, bits, instr.width, instr.type, instr.loc});
                instr.operands[1] = ir::Value{fresh, instr.width, instr.type};
                instr.op = op;
                instr.mayTrap = false;
                instr.isPure = true;
                return true;
            }
            if (in.op == ir::Op::Cast) {
                if (count != 1 || in.isVolatile || in.mayTrap ||
                    !isIntegerWidth(in.width) || in.width != instr.width ||
                    !(in.type == instr.type)) {
                    return false;
                }
                ir::Instr replacement;
                replacement.op = ir::Op::Const;
                replacement.width = instr.width;
                replacement.type = instr.type;
                replacement.loc = in.loc;
                replacement.isPure = true;
                replacement.overflow = in.overflow;
                replacement.results.push_back(
                    ir::Value{id, instr.width, instr.type});
                replacement.constantBits = bits;
                replacement.constantText = std::to_string(bits);
                in = std::move(replacement);
                instr.op = op;
                instr.mayTrap = false;
                instr.isPure = true;
                return true;
            }
            return false;
        }
    }
    return false;
}

// One instruction. Returns true when it was rewritten. The guard clauses are
// the whole safety policy: integer width, pure, non-volatile, single result,
// two operands all at the result's exact width/type, and no trapping operation
// other than Div/Rem whose divisor is about to be proved one.
bool rewriteOne(ir::Function& fn, ir::Instr& instr,
                const std::unordered_map<uint32_t, Constant>& known,
                const std::unordered_map<uint32_t, uint64_t>& uses,
                StrengthResult& result, std::vector<NewConst>& pending,
                uint64_t& nextFresh) {
    if (!instr.isPure || instr.isVolatile ||
        instr.results.size() != kOneResult ||
        instr.operands.size() != kTwoOperands ||
        !isIntegerWidth(instr.width) ||
        !sameAsResult(instr.results[0], instr)) {
        return false;
    }
    switch (instr.op) {
        case ir::Op::Add:
        case ir::Op::Sub:
        case ir::Op::Mul:
        case ir::Op::Div:
        case ir::Op::Rem:
        case ir::Op::And:
        case ir::Op::Or:
        case ir::Op::Xor:
        case ir::Op::Shl:
        case ir::Op::Shr:
            break;
        default:
            return false;
    }
    if (instr.op != ir::Op::Div && instr.op != ir::Op::Rem &&
        instr.mayTrap) {
        return false;
    }
    if (!sameAsResult(instr.operands[0], instr) ||
        !sameAsResult(instr.operands[1], instr)) {
        return false;
    }
    const Constant* lhs = lookup(known, instr.operands[0], instr);
    const Constant* rhs = lookup(known, instr.operands[1], instr);

    auto copy = [&](const ir::Value& source) {
        const ir::Value kept = source;
        makeCopy(instr, kept);
        ++result.reduced;
        ++result.copies;
        return true;
    };
    auto zero = [&]() {
        makeZero(instr);
        ++result.reduced;
        ++result.constants;
        return true;
    };

    switch (instr.op) {
        case ir::Op::Add:
        case ir::Op::Or:
        case ir::Op::Xor:
            if (rhs != nullptr && isZero(*rhs)) {
                return copy(instr.operands[0]);
            }
            if (lhs != nullptr && isZero(*lhs)) {
                return copy(instr.operands[1]);
            }
            return false;
        case ir::Op::Sub:
            if (rhs != nullptr && isZero(*rhs)) {
                return copy(instr.operands[0]);
            }
            return false;
        case ir::Op::Shl:
        case ir::Op::Shr:
            if (rhs != nullptr && isZero(*rhs)) {
                return copy(instr.operands[0]);
            }
            return false;
        case ir::Op::Mul:
            if ((lhs != nullptr && isZero(*lhs)) ||
                (rhs != nullptr && isZero(*rhs))) {
                return zero();
            }
            if (rhs != nullptr && isOne(*rhs)) {
                return copy(instr.operands[0]);
            }
            if (lhs != nullptr && isOne(*lhs)) {
                return copy(instr.operands[1]);
            }
            if (rhs != nullptr &&
                (instr.width == ir::IRType::I32 || instr.width == ir::IRType::U32 ||
                 instr.width == ir::IRType::I64 || instr.width == ir::IRType::U64)) {
                const uint64_t value = rhs->bits & widthMask(instr.width);
                const int k = log2SingleBit(value);
                if (k >= 0 && k < widthBits(instr.width) &&
                    rewriteDivisor(fn, instr, ir::Op::Shl, k, uses, pending,
                                   nextFresh)) {
                    ++result.reduced;
                    return true;
                }
            }
            return false;
        case ir::Op::And:
            if (rhs != nullptr && isAllOnes(*rhs)) {
                return copy(instr.operands[0]);
            }
            if (lhs != nullptr && isAllOnes(*lhs)) {
                return copy(instr.operands[1]);
            }
            return false;
        case ir::Op::Div:
            if (rhs != nullptr && isOne(*rhs)) {
                return copy(instr.operands[0]);
            }
            return false;
        case ir::Op::Rem: {
            if (rhs != nullptr && isOne(*rhs)) {
                return zero();
            }
            if (rhs != nullptr && unsignedAt(instr.width, instr.type)) {
                const uint64_t value = rhs->bits & widthMask(instr.width);
                const int k = log2SingleBit(value);
                if (k >= 0 && k < widthBits(instr.width) &&
                    rewriteDivisor(fn, instr, ir::Op::And,
                                   static_cast<int64_t>(value - 1), uses,
                                   pending, nextFresh)) {
                    ++result.reduced;
                    return true;
                }
            }
            return false;
        }
        default:
            return false;
    }
}

}  // namespace

StrengthResult strengthReduce(ir::Module& module) {
    StrengthResult result;
    // Fresh ids come from above every id in the module, so a Const inserted for
    // a multi-use divisor cannot collide with a definition anywhere else.
    uint64_t nextFresh = 1;
    for (const ir::Function& fn : module.functions) {
        const uint64_t top = static_cast<uint64_t>(maxValueId(fn)) + 1;
        if (top > nextFresh) nextFresh = top;
    }
    for (ir::Function& fn : module.functions) {
        if (fn.blocks.empty()) {
            continue;
        }
        for (;;) {
            const std::unordered_map<uint32_t, uint64_t> uses =
                computeUseCounts(fn);
            const std::unordered_map<uint32_t, Constant> known =
                resolveConstants(fn, uses);
            bool changed = false;
            for (ir::Block& block : fn.blocks) {
                // Indexed walk: a multi-use divisor queues a fresh Const that
                // is inserted before the rewritten instruction, shifting it.
                uint64_t index = 0;
                while (index < block.body.size()) {
                    std::vector<NewConst> pending;
                    if (rewriteOne(fn, block.body[index], known, uses, result,
                                   pending, nextFresh)) {
                        changed = true;
                    }
                    if (!pending.empty()) {
                        for (const NewConst& request : pending) {
                            block.body.insert(
                                block.body.begin() +
                                    static_cast<std::ptrdiff_t>(index),
                                newConstInstruction(request));
                        }
                        index += pending.size();
                    }
                    ++index;
                }
            }
            if (!changed) {
                break;
            }
        }
    }
    return result;
}

}  // namespace opt
}  // namespace cider
