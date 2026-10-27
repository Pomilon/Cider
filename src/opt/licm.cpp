// licm.cpp — loop-invariant code motion over the real phi IR.
//
// A natural loop is found from its back edges: an edge `latch -> header` where
// the header dominates the latch. The loop's block set is the header plus every
// reachable block that can reach a latch without going through the header.
// Several latches feeding one header belong to one loop, and loops are
// processed innermost first (smallest block set, then reverse-postorder
// header), so a value hoisted out of an inner loop can be hoisted again by the
// outer loop that still contains its new preheader.
//
// A loop is only rewritten when it already has a preheader: exactly one
// predecessor of the header lies outside the loop and that predecessor's only
// successor is the header. This slice never creates or splits a block, so a
// loop without one is counted as skipped and left untouched. The preheader may
// be the function entry. Hoisted instructions are appended to the end of the
// preheader body, before its terminator, in dependency order: a candidate is
// moved only after every operand is outside the loop, and the repeated scan
// that makes that true also appends the operand before its user.
//
// The candidate policy is the GVN allowlist, because moving code needs the
// same "pure and cannot fault" answer:
//
//   Const, Copy, Add, Sub, Mul, And, Or, Xor, Shl, Shr, Neg, Not, BitNot,
//   Cmp, Gep
//
// An instruction must also be pure, non-volatile, non-trapping, have exactly
// one non-void result, and have no float width or float language type on its
// result or operands. Load, Store, Call, Phi, Extract, Insert, Cast, DataAddr,
// FnAddr, Trap and Asm are absent from the allowlist: they read or write
// memory, depend on the edge, or have semantics this pass must not reason
// about. Integer Div/Rem are absent because they may trap; float operations
// are excluded entirely so NaN payloads, signed zero and rounding stay out of
// the pass. A Gep with an invariant base and index is pointer arithmetic and
// moves like the rest.
//
// Speculation is deliberate. A loop body may execute zero times, so hoisting a
// pure operation evaluates it on a path that did not before. None of the
// allowed operations can fault or observe memory, and an unused result has no
// effect, so the extra evaluation is unobservable. This is also why a result
// used by a phi needs no edge-specific rule: moving the definition to the
// preheader makes it dominate every incoming edge, and the phi still selects
// the value on the edge that names it. Instructions that can trap or observe
// memory are never speculated.
//
// Moves preserve the CFG exactly: no terminator, block, label or order
// changes. Only body entries change, and every operand of a moved instruction
// is defined in a block that dominates the preheader, so every use the old
// definition dominated is still dominated by its new one. The pipeline runs
// the structural checker again after this pass and reports a malformed rewrite
// as an `after licm` diagnostic.
//
// Counters: `loops` counts natural loops detected (one per header, not one per
// back edge), `hoisted` counts instruction moves (a nested invariant moved
// twice counts twice), and `skipped` counts loops with no usable preheader.

#include "opt/opt.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "opt/dominance.h"

namespace cider {
namespace opt {

namespace {

constexpr uint64_t kOneResult = 1;
constexpr uint64_t kOneBlock = 1;
// A value defined outside any loop, and the marker for a parameter. No real
// block carries this label, so membership tests read it as "outside".
constexpr uint32_t kOutside = 0xFFFFFFFFu;

bool isFloatWidth(ir::IRType width) {
    return width == ir::IRType::F32 || width == ir::IRType::F64;
}

bool isFloatType(const TypeRef& type) {
    return !type.isError() && type.type->kind() == Kind::Basic &&
           basicIsFloat(*type.type->basic());
}

// The GVN allowlist. Keep the two lists in step: a new opcode is safe here
// only after the same purity argument GVN makes for it.
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
        case ir::Op::Cast:
        case ir::Op::Trap:
        case ir::Op::Asm:
            return false;
    }
    return false;
}

struct NaturalLoop {
    uint32_t header = 0;
    std::vector<uint32_t> latches;
    std::unordered_set<uint32_t> blocks;
    uint32_t preheader = kOutside;
};

bool eligible(const ir::Instr& instr) {
    if (!instr.isPure || instr.isVolatile || instr.mayTrap) {
        return false;
    }
    if (instr.results.size() != kOneResult) {
        return false;
    }
    if (!allowedOp(instr.op)) {
        return false;
    }
    const ir::Value& result = instr.results[0];
    if (result.width == ir::IRType::Void || instr.width == ir::IRType::Void) {
        return false;
    }
    if (isFloatWidth(instr.width) || isFloatWidth(result.width) ||
        isFloatType(instr.type) || isFloatType(result.type)) {
        return false;
    }
    for (const ir::Value& operand : instr.operands) {
        if (isFloatWidth(operand.width) || isFloatType(operand.type)) {
            return false;
        }
    }
    return true;
}

// A pure instruction may move when every value it reads is already outside the
// loop. `defBlock` is updated as instructions move, so a value hoisted earlier
// in this same loop (or by an inner loop) already answers "outside".
bool operandsAvailable(const ir::Instr& instr,
                       const std::unordered_set<uint32_t>& blocks,
                       const std::unordered_map<uint32_t, uint32_t>& defBlock) {
    auto outside = [&](const ir::Value& value) {
        auto found = defBlock.find(value.id);
        if (found == defBlock.end()) {
            return false;
        }
        return blocks.find(found->second) == blocks.end();
    };
    for (const ir::Value& operand : instr.operands) {
        if (!outside(operand)) {
            return false;
        }
    }
    // A Gep names its base in addressBase rather than operands; a global
    // label is resolved by the linker and is not a value use. This mirrors
    // the structural checker and GVN.
    if (instr.op == ir::Op::Gep && instr.globalLabel.empty() &&
        !outside(instr.addressBase)) {
        return false;
    }
    return true;
}

}  // namespace

LicmResult loopInvariantCodeMotion(ir::Module& module) {
    LicmResult result;
    for (ir::Function& fn : module.functions) {
        if (fn.blocks.empty()) {
            continue;
        }
        const DominatorInfo dominators = computeDominators(fn);
        if (dominators.reachable.empty()) {
            continue;
        }
        std::unordered_map<uint32_t, ir::Block*> blockAt;
        for (ir::Block& block : fn.blocks) {
            blockAt[block.label] = &block;
        }

        // Definition block for every SSA value. Parameters are defined outside
        // every loop. A duplicate id (the pre-mem2reg Alloca-parameter shape)
        // simply takes the last definition and is conservatively treated as
        // loop-defined if that definition is inside the loop.
        std::unordered_map<uint32_t, uint32_t> defBlock;
        for (const ir::Value& param : fn.params) {
            defBlock[param.id] = kOutside;
        }
        for (const ir::Block& block : fn.blocks) {
            for (const ir::Instr& instr : block.body) {
                for (const ir::Value& value : instr.results) {
                    defBlock[value.id] = block.label;
                }
            }
        }

        // One natural loop per back-edge header. `rpo` is deterministic, and
        // so is the block-label order inside each predecessor list, so both
        // the loop list and the preheader choice are stable.
        std::vector<NaturalLoop> loops;
        std::unordered_map<uint32_t, uint64_t> loopAt;
        for (uint32_t label : dominators.rpo) {
            auto successors = dominators.successors.find(label);
            if (successors == dominators.successors.end()) {
                continue;
            }
            for (uint32_t target : successors->second) {
                if (!dominators.dominates(target, label)) {
                    continue;
                }
                auto found = loopAt.find(target);
                uint64_t at = 0;
                if (found == loopAt.end()) {
                    at = loops.size();
                    loopAt[target] = at;
                    NaturalLoop loop;
                    loop.header = target;
                    loops.push_back(std::move(loop));
                } else {
                    at = found->second;
                }
                loops[at].latches.push_back(label);
            }
        }

        for (NaturalLoop& loop : loops) {
            // The header is always in its own natural loop.
            loop.blocks.insert(loop.header);

            // Grow from every latch that names this header through the
            // predecessor edges, stopping at the header. Restricting the walk
            // to reachable blocks keeps an unreachable edge into a latch from
            // joining the loop. A duplicate in the work list is harmless: the
            // set guards re-expansion.
            std::vector<uint32_t> work = loop.latches;
            while (!work.empty()) {
                uint32_t label = work.back();
                work.pop_back();
                if (label == loop.header) {
                    continue;
                }
                if (!loop.blocks.insert(label).second) {
                    continue;
                }
                auto preds = dominators.predecessors.find(label);
                if (preds == dominators.predecessors.end()) {
                    continue;
                }
                for (uint32_t pred : preds->second) {
                    if (dominators.reachable.find(pred) ==
                        dominators.reachable.end()) {
                        continue;
                    }
                    if (loop.blocks.find(pred) == loop.blocks.end()) {
                        work.push_back(pred);
                    }
                }
            }

            // An existing preheader only: one predecessor outside the loop
            // whose sole successor is the header. The entry block qualifies.
            std::vector<uint32_t> outside;
            auto preds = dominators.predecessors.find(loop.header);
            if (preds != dominators.predecessors.end()) {
                for (uint32_t pred : preds->second) {
                    if (loop.blocks.find(pred) == loop.blocks.end()) {
                        outside.push_back(pred);
                    }
                }
            }
            const bool oneOutside = outside.size() == kOneBlock;
            if (oneOutside) {
                const uint32_t candidate = outside[0];
                auto successors = dominators.successors.find(candidate);
                if (successors != dominators.successors.end() &&
                    successors->second.size() == kOneBlock &&
                    successors->second[0] == loop.header) {
                    loop.preheader = candidate;
                }
            }
        }

        // Innermost first: a nested loop's block set is a proper subset of the
        // containing loop, so its smaller size is the deterministic order.
        // Equal sizes are disjoint; reverse postorder breaks that tie.
        std::sort(loops.begin(), loops.end(),
                  [&](const NaturalLoop& lhs, const NaturalLoop& rhs) {
                      if (lhs.blocks.size() != rhs.blocks.size()) {
                          return lhs.blocks.size() < rhs.blocks.size();
                      }
                      auto left = dominators.rpoIndex.find(lhs.header);
                      auto right = dominators.rpoIndex.find(rhs.header);
                      const uint32_t leftIndex =
                          left == dominators.rpoIndex.end() ? 0 : left->second;
                      const uint32_t rightIndex =
                          right == dominators.rpoIndex.end() ? 0 : right->second;
                      return leftIndex < rightIndex;
                  });

        for (NaturalLoop& loop : loops) {
            ++result.loops;
            if (loop.preheader == kOutside) {
                ++result.skipped;
                continue;
            }
            auto preheaderAt = blockAt.find(loop.preheader);
            if (preheaderAt == blockAt.end()) {
                ++result.skipped;
                continue;
            }
            ir::Block& preheader = *preheaderAt->second;

            // Repeat until a full scan moves nothing. The scan is in reverse
            // postorder and each body in definition order, so a user is
            // normally moved after its operand in the same pass; the repeat
            // also covers a chain whose definition sits in a later block.
            bool changed = true;
            while (changed) {
                changed = false;
                std::vector<ir::Instr> moved;
                for (uint32_t label : dominators.rpo) {
                    if (loop.blocks.find(label) == loop.blocks.end()) {
                        continue;
                    }
                    auto at = blockAt.find(label);
                    if (at == blockAt.end()) {
                        continue;
                    }
                    ir::Block& block = *at->second;
                    uint64_t index = 0;
                    while (index < block.body.size()) {
                        ir::Instr& instr = block.body[index];
                        if (!eligible(instr) ||
                            !operandsAvailable(instr, loop.blocks, defBlock)) {
                            ++index;
                            continue;
                        }
                        for (const ir::Value& value : instr.results) {
                            defBlock[value.id] = loop.preheader;
                        }
                        moved.push_back(std::move(instr));
                        block.body.erase(
                            block.body.begin() +
                            static_cast<std::ptrdiff_t>(index));
                        ++result.hoisted;
                        changed = true;
                    }
                }
                if (!moved.empty()) {
                    preheader.body.insert(
                        preheader.body.end(),
                        std::make_move_iterator(moved.begin()),
                        std::make_move_iterator(moved.end()));
                }
            }
        }
    }
    return result;
}

}  // namespace opt
}  // namespace cider
