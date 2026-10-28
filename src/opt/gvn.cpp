// gvn.cpp — dominance-scoped global value numbering (CSE) over the phi IR.
//
// The table maps one expression key to the first value that computed it. The
// table is scoped by the dominator tree: an entry inserted in a block is
// visible to that block's dominator subtree and erased when the walk leaves
// it. A sibling branch therefore never sees the other branch's expressions,
// and a join sees only what every path into it has already computed.
//
// Only instructions that are pure, non-volatile and unable to trap may be
// commoned. The allowlist is explicit rather than "everything not on a block
// list", because a new opcode must be examined before GVN treats it as safe:
//
//   Const, Copy, Add, Sub, Mul, And, Or, Xor, Shl, Shr, Neg, Not, BitNot,
//   Cmp, Gep
//
// Load, Store, Call, Phi, Extract, Insert, Cast, DataAddr, FnAddr, Trap and
// Asm stay untouched. Division and remainder are absent because integer
// forms may trap; float forms are excluded by the float rule below. Loads and
// calls read or write memory, Phi depends on its edge, and asm is opaque.
//
// Floats are excluded entirely: an instruction is eligible only when neither
// its result nor an operand is an F32/F64 width or a float basic type. That
// keeps NaN payloads, signed zero, rounding and comparison flag semantics out
// of the pass instead of reasoning about them. Gep is pointer arithmetic, so
// its Ptr result and integer index remain eligible even when the pointee is
// a float type; the pointee is part of the key and is never read here.
//
// Commutative operands (Add, Mul, And, Or, Xor, and Eq/Ne comparisons) are
// ordered by value id before the key is built, so `a + b` and `b + a` are one
// expression. The key also names the op, result width, result type identity,
// compare predicate, overflow mode, constant bits, Gep base/displacement/
// pointee, and the operand ids. Two values that differ in any of those are
// different expressions.
//
// A duplicate is replaced by a leader that dominates it, and every semantic
// use of the duplicate result is rewritten: operands, call argument words,
// phi incoming values, addressBase, storedValue, an indirect callee, and
// terminator values/conditions/switch cases. Copy chains collapse when the
// copied value has the same width and type; that is a pure value move, not an
// operation whose repetition could matter.
//
// A call does not clear the table. Every eligible instruction consumes SSA
// values, which a call cannot change, and Gep computes an address rather than
// reading the memory behind it. A pure expression may therefore be reused
// across a call when its operands are unchanged. Trapping, volatile and asm
// instructions are never removed or reordered by this pass.

#include "opt/opt.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "opt/dominance.h"

namespace cider {
namespace opt {

namespace {

constexpr uint64_t kOneResult = 1;
constexpr uint64_t kOneOperand = 1;
constexpr uint64_t kTwoOperands = 2;

bool namesBase(ir::Op op) {
    return op == ir::Op::Load || op == ir::Op::Store ||
           op == ir::Op::Extract || op == ir::Op::Insert ||
           op == ir::Op::Gep;
}

bool isFloatWidth(ir::IRType width) {
    return width == ir::IRType::F32 || width == ir::IRType::F64;
}

bool isFloatType(const TypeRef& type) {
    return !type.isError() && type.type->kind() == Kind::Basic &&
           basicIsFloat(*type.type->basic());
}

// True for an instruction GVN is allowed to common. The callers have already
// checked purity and the flags; this list is the opcode policy.
bool allowedOp(ir::Op op) {
    switch (op) {
        case ir::Op::Const:
        case ir::Op::Copy:
        case ir::Op::Add:
        case ir::Op::Sub:
        case ir::Op::Mul:
        case ir::Op::And:
        case ir::Op::Or:
        case ir::Op::Xor:
        case ir::Op::Shl:
        case ir::Op::Shr:
        case ir::Op::Neg:
        case ir::Op::Not:
        case ir::Op::BitNot:
        case ir::Op::Cmp:
        case ir::Op::Gep:
            return true;
        case ir::Op::Phi:
        case ir::Op::Div:
        case ir::Op::Rem:
        case ir::Op::FAdd:
        case ir::Op::FSub:
        case ir::Op::FMul:
        case ir::Op::FDiv:
        case ir::Op::FRem:
        case ir::Op::Load:
        case ir::Op::Store:
        case ir::Op::Alloca:
        case ir::Op::DataAddr:
        case ir::Op::FnAddr:
        case ir::Op::Extract:
        case ir::Op::Insert:
        case ir::Op::Call:
        case ir::Op::VaStart:
        case ir::Op::VaArg:
        case ir::Op::Cast:
        case ir::Op::Trap:
        case ir::Op::Asm:
            return false;
    }
    return false;
}

bool commutative(const ir::Instr& instr) {
    switch (instr.op) {
        case ir::Op::Add:
        case ir::Op::Mul:
        case ir::Op::And:
        case ir::Op::Or:
        case ir::Op::Xor:
            return true;
        case ir::Op::Cmp:
            return instr.cmp == ir::CmpOp::Eq || instr.cmp == ir::CmpOp::Ne;
        default:
            return false;
    }
}

// One expression, with every field that can change its answer. `type` and
// `pointee` are interned Type handles, so equality is identity and the hash
// uses the handle only; no decision below depends on pointer ordering.
struct ExprKey {
    ir::Op op = ir::Op::Const;
    ir::IRType width = ir::IRType::Void;
    const Type* type = nullptr;
    ir::OverflowMode overflow = ir::OverflowMode::Undefined;
    ir::CmpOp cmp = ir::CmpOp::Eq;
    int64_t constantBits = 0;
    int64_t displacement = 0;
    uint32_t field = 0;
    uint32_t base = 0;
    const Type* pointee = nullptr;
    std::vector<uint32_t> operands;

    friend bool operator==(const ExprKey& lhs, const ExprKey& rhs) = default;
};

void hashCombine(uint64_t& seed, uint64_t value) {
    seed ^= value + 0x9E3779B97F4A7C15ull + (seed << 6) + (seed >> 2);
}

struct ExprKeyHash {
    uint64_t operator()(const ExprKey& key) const {
        uint64_t seed = static_cast<uint64_t>(key.op);
        hashCombine(seed, static_cast<uint64_t>(key.width));
        hashCombine(seed, reinterpret_cast<uintptr_t>(key.type));
        hashCombine(seed, static_cast<uint64_t>(key.overflow));
        hashCombine(seed, static_cast<uint64_t>(key.cmp));
        hashCombine(seed, static_cast<uint64_t>(key.constantBits));
        hashCombine(seed, static_cast<uint64_t>(key.displacement));
        hashCombine(seed, key.field);
        hashCombine(seed, key.base);
        hashCombine(seed, reinterpret_cast<uintptr_t>(key.pointee));
        for (uint32_t id : key.operands) {
            hashCombine(seed, id);
        }
        return seed;
    }
};

struct Leader {
    ir::Value value;
    uint32_t block = 0;
};

ExprKey makeKey(const ir::Instr& instr,
                const std::unordered_map<uint32_t, ir::Value>& subst) {
    ExprKey key;
    key.op = instr.op;
    key.width = instr.width;
    key.type = instr.type.type;
    key.overflow = instr.overflow;
    key.cmp = instr.cmp;
    key.constantBits = instr.constantBits;
    key.displacement = instr.displacement;
    key.field = instr.field;
    auto base = subst.find(instr.addressBase.id);
    key.base = base != subst.end() ? base->second.id : instr.addressBase.id;
    key.pointee = instr.pointee.type;
    key.operands.reserve(instr.operands.size());
    for (const ir::Value& operand : instr.operands) {
        auto found = subst.find(operand.id);
        key.operands.push_back(found != subst.end() ? found->second.id
                                                    : operand.id);
    }
    if (commutative(instr) && key.operands.size() == kTwoOperands &&
        key.operands[0] > key.operands[1]) {
        std::swap(key.operands[0], key.operands[1]);
    }
    return key;
}

ir::Value canonicalValue(const ir::Value& value,
                         const std::unordered_map<uint32_t, ir::Value>& subst) {
    auto found = subst.find(value.id);
    return found != subst.end() ? found->second : value;
}

// Rewrite every semantic use of a substituted result. Definitions are never
// touched: the map only names results of instructions this pass deletes.
// Returns how many value slots changed, for the counter a test can assert.
uint64_t replaceUses(ir::Function& fn,
                     const std::unordered_map<uint32_t, ir::Value>& subst) {
    if (subst.empty()) {
        return 0;
    }
    uint64_t redirected = 0;
    auto remap = [&](ir::Value& value) {
        auto found = subst.find(value.id);
        if (found != subst.end()) {
            value = found->second;
            ++redirected;
        }
    };
    for (ir::Block& block : fn.blocks) {
        for (ir::Instr& instr : block.body) {
            for (ir::Value& operand : instr.operands) {
                remap(operand);
            }
            for (ir::Argument& arg : instr.arguments) {
                for (ir::Value& word : arg.words) {
                    remap(word);
                }
            }
            for (ir::PhiIncoming& incoming : instr.phiIncoming) {
                remap(incoming.value);
            }
            // A global-labelled Load/Store names the label, not addressBase;
            // mirroring the structural checker and mem2reg keeps the default
            // zero base out of the rewrite.
            if (namesBase(instr.op) && instr.globalLabel.empty()) {
                remap(instr.addressBase);
            }
            if (instr.op == ir::Op::Store || instr.op == ir::Op::Insert) {
                remap(instr.storedValue);
            }
            if (instr.op == ir::Op::Call && instr.call.indirect) {
                remap(instr.calleeValue);
            }
        }
        ir::Terminator& term = block.term;
        for (ir::Value& word : term.values) {
            remap(word);
        }
        if (term.cond.has_value()) {
            remap(*term.cond);
        }
        if (term.term == ir::Term::Switch) {
            remap(term.switchValue);
            for (ir::Case& arm : term.cases) {
                remap(arm.value);
            }
        }
    }
    return redirected;
}

}  // namespace

GvnResult valueNumber(ir::Module& module) {
    GvnResult result;
    for (ir::Function& fn : module.functions) {
        if (fn.blocks.empty()) {
            continue;
        }
        const DominatorInfo dominators = computeDominators(fn);
        if (dominators.reachable.empty()) {
            continue;
        }
        std::unordered_map<uint32_t, const ir::Block*> blockAt;
        for (const ir::Block& block : fn.blocks) {
            blockAt[block.label] = &block;
        }
        std::unordered_map<ExprKey, Leader, ExprKeyHash> available;
        std::vector<ExprKey> scopeLog;
        std::unordered_map<uint32_t, ir::Value> subst;
        std::unordered_set<uint32_t> removed;

        std::function<void(uint32_t)> walk = [&](uint32_t label) {
            auto at = blockAt.find(label);
            if (at == blockAt.end()) {
                return;
            }
            const size_t mark = scopeLog.size();
            for (const ir::Instr& instr : at->second->body) {
                if (!instr.isPure || instr.isVolatile || instr.mayTrap ||
                    instr.results.size() != kOneResult ||
                    !allowedOp(instr.op)) {
                    continue;
                }
                // Floats stay out of the pass entirely, including a float
                // comparison whose result width is Bool.
                if (isFloatWidth(instr.width) || isFloatType(instr.type)) {
                    continue;
                }
                bool floatOperand = false;
                for (const ir::Value& operand : instr.operands) {
                    if (isFloatWidth(operand.width) ||
                        isFloatType(operand.type)) {
                        floatOperand = true;
                        break;
                    }
                }
                if (floatOperand) {
                    continue;
                }
                const ir::Value resultValue = instr.results[0];
                if (resultValue.width == ir::IRType::Void) {
                    continue;
                }

                // A Copy whose value is the same width and type is a move,
                // not an operation: name the source directly and remove the
                // copy. Chains collapse because the source is already
                // canonicalised.
                if (instr.op == ir::Op::Copy &&
                    instr.operands.size() == kOneOperand) {
                    ir::Value source =
                        canonicalValue(instr.operands[0], subst);
                    if (source.id != resultValue.id &&
                        source.width == resultValue.width &&
                        source.type == resultValue.type) {
                        subst[resultValue.id] = source;
                        removed.insert(resultValue.id);
                        ++result.copiesCollapsed;
                        ++result.expressionsRemoved;
                        continue;
                    }
                }

                ExprKey key = makeKey(instr, subst);
                auto found = available.find(key);
                if (found != available.end()) {
                    // The table is dominator-scoped, so this holds by
                    // construction; checking it keeps a future scoping bug
                    // from deleting an instruction whose leader is not
                    // available on every path.
                    if (!dominators.dominates(found->second.block, label)) {
                        continue;
                    }
                    subst[resultValue.id] = found->second.value;
                    removed.insert(resultValue.id);
                    ++result.expressionsRemoved;
                    continue;
                }
                Leader leader;
                leader.value = resultValue;
                leader.block = label;
                available[key] = leader;
                scopeLog.push_back(std::move(key));
            }
            auto children = dominators.children.find(label);
            if (children != dominators.children.end()) {
                for (uint32_t child : children->second) {
                    walk(child);
                }
            }
            while (scopeLog.size() > mark) {
                available.erase(scopeLog.back());
                scopeLog.pop_back();
            }
        };
        walk(fn.blocks.front().label);

        result.usesRedirected += replaceUses(fn, subst);
        if (removed.empty()) {
            continue;
        }
        for (ir::Block& block : fn.blocks) {
            std::vector<ir::Instr> kept;
            for (ir::Instr& instr : block.body) {
                const bool drop = !instr.results.empty() &&
                                  removed.find(instr.results[0].id) !=
                                      removed.end();
                if (!drop) {
                    kept.push_back(std::move(instr));
                }
            }
            block.body = std::move(kept);
        }
    }
    return result;
}

}  // namespace opt
}  // namespace cider
