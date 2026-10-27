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
// Optional strength reductions are deliberately absent. `Sub 0,x -> Neg` and
// `Xor x,-1 -> BitNot` would change the 64-bit word representation at sub-64
// widths, because the emitter and VM implement Neg/BitNot without re-extending
// the answer to the value's canonical width word while Sub/Xor do extend.
// Consumers re-extend on read, but this pass does not rely on that wider
// argument. `Mul x,2^k -> Shl x,k` is absent as well: the 8-bit multiply path
// computes a sixteen-bit product before extension, so the wrap shape differs
// from an 8-bit shift at that width. A checked, width-covering proof is future
// work.
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

#include <cstdint>
#include <unordered_map>

namespace cider {
namespace opt {

namespace {

constexpr uint64_t kOneResult = 1;
constexpr uint64_t kOneOperand = 1;
constexpr uint64_t kTwoOperands = 2;

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

// All constants the current IR proves. Direct Const definitions are recorded
// first, then a same-width/same-type Copy chain is followed to a fixed point.
// A Copy is resolved only when the value it moves carries exactly its own
// width and type, which is the same rule simplification and value numbering
// use before substituting a copy. Never iterates the table, so the map is
// deterministic.
std::unordered_map<uint32_t, Constant> resolveConstants(const ir::Function& fn) {
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
                if (instr.op != ir::Op::Copy ||
                    instr.results.size() != kOneResult ||
                    instr.operands.size() != kOneOperand || instr.isVolatile ||
                    instr.mayTrap || !instr.isPure) {
                    continue;
                }
                const ir::Value& result = instr.results[0];
                const ir::Value& source = instr.operands[0];
                if (source.width != result.width || !(source.type == result.type) ||
                    known.find(result.id) != known.end()) {
                    continue;
                }
                auto found = known.find(source.id);
                if (found == known.end()) {
                    continue;
                }
                known.emplace(result.id, found->second);
                changed = true;
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

// One instruction. Returns true when it was rewritten. The guard clauses are
// the whole safety policy: integer width, pure, non-volatile, single result,
// two operands all at the result's exact width/type, and no trapping operation
// other than Div/Rem whose divisor is about to be proved one.
bool rewriteOne(ir::Instr& instr,
                const std::unordered_map<uint32_t, Constant>& known,
                StrengthResult& result) {
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
        case ir::Op::Rem:
            if (rhs != nullptr && isOne(*rhs)) {
                return zero();
            }
            return false;
        default:
            return false;
    }
}

}  // namespace

StrengthResult strengthReduce(ir::Module& module) {
    StrengthResult result;
    for (ir::Function& fn : module.functions) {
        if (fn.blocks.empty()) {
            continue;
        }
        for (;;) {
            const std::unordered_map<uint32_t, Constant> known =
                resolveConstants(fn);
            bool changed = false;
            for (ir::Block& block : fn.blocks) {
                for (ir::Instr& instr : block.body) {
                    if (rewriteOne(instr, known, result)) {
                        changed = true;
                    }
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
