// simplify.cpp — constant control-flow folding and unreachable-block removal.
//
// This pass runs after constant folding and before value numbering. Folding
// can prove a branch or switch takes one edge; that can make a whole block
// unreachable, which in turn can leave a successor phi with fewer incoming
// values, or exactly one. The pass repeats those steps to a fixpoint because
// simplifying a phi can expose a constant that folds another branch.
//
// What it will resolve: a value defined by `Const`, by a `Copy` whose result
// has the same width and type as its source, or by a `Phi` every one of whose
// incoming values resolves to the same constant bits. Nothing else. A load,
// call, asm block, cast or arithmetic result is not guessed at, even when a
// previous pass could have folded it: arithmetic folding belongs to
// `foldConsts`, and memory is not readable at this point.
//
// Width handling: a narrow `Const` is taken exactly as the 64-bit word its
// producer stored. `foldConsts` already truncates narrow arithmetic results to
// their result width. `Copy` and `Phi` are only resolved when their result
// width and type match every value they carry, so this pass never invents a
// truncation or extension the IR did not already perform.
//
// Removal is deliberately whole-block only. An instruction in a reachable
// block is never deleted or moved, so a reachable trap, volatile access or asm
// block is untouched. A block unreachable from the entry cannot run, so it is
// discarded with its contents.
//
// Phis: when a block is removed, or when a branch no longer names one of its
// targets, every phi in a surviving block loses the incoming entry for that
// predecessor. Entries are matched by block label, never by position, so two
// word phis of one aggregate may list their predecessors in different orders.
// A phi left with one incoming is replaced everywhere by that incoming and
// deleted. The replacement is only made when the incoming value has the phi's
// width and type, which the structural checker also requires.

#include "opt/opt.h"

#include <cstdint>
#include <optional>
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
constexpr uint64_t kOneIncoming = 1;

// One resolved constant word. Width is carried so a caller can insist two
// values were produced at the same width before comparing them; the bits are
// the exact 64-bit word the producer stored.
struct Constant {
    uint64_t bits = 0;
    ir::IRType width = ir::IRType::Void;
};

bool isFloatWidth(ir::IRType width) {
    return width == ir::IRType::F32 || width == ir::IRType::F64;
}

// Resolve one definition using only definitions already proved constant.
// Nullopt is the conservative answer: unknown, mismatched in width/type, a
// float bit pattern, or a phi whose incoming values disagree or are unknown.
std::optional<Constant> resolveOne(
    const ir::Instr& instr,
    const std::unordered_map<uint32_t, Constant>& known) {
    if (instr.results.size() != kOneResult) {
        return std::nullopt;
    }
    const ir::Value& result = instr.results[0];
    if (result.width == ir::IRType::Void || isFloatWidth(result.width)) {
        return std::nullopt;
    }
    if (instr.op == ir::Op::Const) {
        // A Const is a word, not a mathematical integer: the producer already
        // chose its representation at this width. Take the bits verbatim.
        Constant value;
        value.bits = static_cast<uint64_t>(instr.constantBits);
        value.width = result.width;
        return value;
    }
    if (instr.op == ir::Op::Copy) {
        if (instr.operands.size() != kOneOperand) {
            return std::nullopt;
        }
        const ir::Value& source = instr.operands[0];
        if (source.width != result.width || !(source.type == result.type)) {
            return std::nullopt;
        }
        auto found = known.find(source.id);
        if (found == known.end()) {
            return std::nullopt;
        }
        return found->second;
    }
    if (instr.op == ir::Op::Phi) {
        // A phi with no incoming is in an unreachable block (or malformed);
        // it has no value to prove. Every incoming must be a constant of the
        // phi's own width and type, and they must all agree bit-for-bit.
        if (instr.phiIncoming.empty()) {
            return std::nullopt;
        }
        std::optional<Constant> value;
        for (const ir::PhiIncoming& incoming : instr.phiIncoming) {
            if (incoming.value.width != result.width ||
                !(incoming.value.type == result.type)) {
                return std::nullopt;
            }
            auto found = known.find(incoming.value.id);
            if (found == known.end()) {
                return std::nullopt;
            }
            if (!value.has_value()) {
                value = found->second;
            } else if (value->bits != found->second.bits) {
                return std::nullopt;
            }
        }
        return value;
    }
    return std::nullopt;
}

// Every constant this function's definitions prove, in one fixed point. The
// iteration is bounded by the number of definitions: every pass either adds a
// value or stops. Ordering is by block and body position, so the result does
// not depend on hash-table iteration.
std::unordered_map<uint32_t, Constant> resolveConstants(const ir::Function& fn) {
    std::unordered_map<uint32_t, Constant> known;
    for (;;) {
        bool changed = false;
        for (const ir::Block& block : fn.blocks) {
            for (const ir::Instr& instr : block.body) {
                if (instr.results.size() != kOneResult) {
                    continue;
                }
                const uint32_t id = instr.results[0].id;
                if (known.find(id) != known.end()) {
                    continue;
                }
                std::optional<Constant> value = resolveOne(instr, known);
                if (!value.has_value()) {
                    continue;
                }
                known.emplace(id, *value);
                changed = true;
            }
        }
        if (!changed) {
            return known;
        }
    }
}

// Rewrite every semantic use of a replacement. Definitions are never touched:
// the map only names results of phi instructions this pass deletes. The use
// sites listed below are every place the structural checker and value
// numbering also rewrite.
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
            // A global-labelled access names the label, not addressBase; the
            // default zero base must not be rewritten.
            if ((instr.op == ir::Op::Load || instr.op == ir::Op::Store ||
                 instr.op == ir::Op::Extract || instr.op == ir::Op::Insert ||
                 instr.op == ir::Op::Gep) &&
                instr.globalLabel.empty()) {
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

// Fold a reachable terminator whose tested value resolves to a constant.
// Returns true when the terminator changed. A nonconstant value, a float, or
// a switch with an unknown earlier case is left exactly as it was.
bool foldTerminator(ir::Block& block,
                    const std::unordered_map<uint32_t, Constant>& known,
                    SimplifyResult& result) {
    ir::Terminator& term = block.term;
    if (term.term == ir::Term::CondBr) {
        if (!term.cond.has_value()) {
            return false;
        }
        auto found = known.find(term.cond->id);
        if (found == known.end() ||
            found->second.width != term.cond->width) {
            return false;
        }
        // The runtime tests the whole stored word: nonzero takes the then
        // edge, zero the else edge.
        const uint32_t target =
            found->second.bits != 0 ? term.thenTarget : term.elseTarget;
        const Position loc = term.loc;
        term = ir::Terminator{};
        term.term = ir::Term::Br;
        term.target = target;
        term.loc = loc;
        ++result.branchesFolded;
        return true;
    }
    if (term.term == ir::Term::Switch) {
        auto value = known.find(term.switchValue.id);
        if (value == known.end() ||
            value->second.width != term.switchValue.width) {
            return false;
        }
        // First matching case wins; no match takes the default. A case whose
        // value is not a provable constant could match, so an unknown case
        // refuses the whole fold rather than guessing it is unequal. Case
        // values must be the switch value's width: a width-changing compare
        // needs a coercion rule this slice does not have.
        uint32_t chosen = term.defaultTarget;
        for (const ir::Case& arm : term.cases) {
            if (arm.value.width != term.switchValue.width) {
                return false;
            }
            auto found = known.find(arm.value.id);
            if (found == known.end() ||
                found->second.width != arm.value.width) {
                return false;
            }
            if (found->second.bits == value->second.bits) {
                chosen = arm.target;
                break;
            }
        }
        const Position loc = term.loc;
        term = ir::Terminator{};
        term.term = ir::Term::Br;
        term.target = chosen;
        term.loc = loc;
        ++result.branchesFolded;
        return true;
    }
    return false;
}

// Remove incoming entries whose predecessor is no longer a predecessor of the
// phi's block. Rebuilt from the current terminators for every block, so an
// edge that a fold removed from a still-reachable block is dropped as well as
// an edge whose whole predecessor block was deleted. Entries are matched by
// label, not index.
uint64_t prunePhiIncoming(ir::Block& block,
                          const std::unordered_set<uint32_t>& predecessors) {
    uint64_t removed = 0;
    for (ir::Instr& instr : block.body) {
        if (instr.op != ir::Op::Phi) {
            continue;
        }
        std::vector<ir::PhiIncoming> kept;
        kept.reserve(instr.phiIncoming.size());
        for (const ir::PhiIncoming& incoming : instr.phiIncoming) {
            if (predecessors.find(incoming.block) != predecessors.end()) {
                kept.push_back(incoming);
            } else {
                ++removed;
            }
        }
        instr.phiIncoming = std::move(kept);
    }
    return removed;
}

// Replace every single-incoming phi in a surviving block by its incoming
// value. Chains are resolved before any use is rewritten, so a phi that names
// another simplified phi ends at the real value. A cycle (only possible in
// malformed or unreachable IR) is left alone rather than deleting a
// definition still in use.
uint64_t simplifySingleIncomingPhis(ir::Function& fn) {
    if (fn.blocks.empty()) {
        return 0;
    }
    const uint32_t entry = fn.blocks.front().label;
    std::unordered_map<uint32_t, ir::Value> raw;
    for (const ir::Block& block : fn.blocks) {
        if (block.label == entry) {
            continue;
        }
        for (const ir::Instr& instr : block.body) {
            if (instr.op != ir::Op::Phi ||
                instr.results.size() != kOneResult ||
                instr.phiIncoming.size() != kOneIncoming) {
                continue;
            }
            const ir::Value& result = instr.results[0];
            const ir::Value& incoming = instr.phiIncoming[0].value;
            if (incoming.id == result.id ||
                incoming.width != result.width ||
                !(incoming.type == result.type)) {
                continue;
            }
            raw.emplace(result.id, incoming);
        }
    }
    if (raw.empty()) {
        return 0;
    }

    std::unordered_map<uint32_t, ir::Value> subst;
    subst.reserve(raw.size());
    for (const auto& entryPair : raw) {
        const uint32_t id = entryPair.first;
        ir::Value resolved = entryPair.second;
        std::unordered_set<uint32_t> seen;
        seen.insert(id);
        bool cycle = false;
        while (true) {
            auto next = raw.find(resolved.id);
            if (next == raw.end()) {
                break;
            }
            if (!seen.insert(resolved.id).second) {
                cycle = true;
                break;
            }
            resolved = next->second;
        }
        if (cycle || resolved.id == id) {
            continue;
        }
        subst.emplace(id, resolved);
    }
    if (subst.empty()) {
        return 0;
    }

    replaceUses(fn, subst);
    uint64_t removed = 0;
    for (ir::Block& block : fn.blocks) {
        std::vector<ir::Instr> kept;
        kept.reserve(block.body.size());
        for (ir::Instr& instr : block.body) {
            const bool drop =
                instr.op == ir::Op::Phi &&
                instr.results.size() == kOneResult &&
                subst.find(instr.results[0].id) != subst.end();
            if (drop) {
                ++removed;
            } else {
                kept.push_back(std::move(instr));
            }
        }
        block.body = std::move(kept);
    }
    return removed;
}

// One function, to a fixpoint. Folding only removes edges; phi simplification
// only deletes a value whose incoming is now unique; block removal only drops
// blocks the entry cannot reach. Each step is monotone, so the loop finishes
// when no step changes anything.
void simplifyFunction(ir::Function& fn, SimplifyResult& result) {
    if (fn.blocks.empty()) {
        return;
    }
    for (;;) {
        bool changed = false;
        const std::unordered_map<uint32_t, Constant> known = resolveConstants(fn);

        // Fold only reachable terminators: an unreachable block is removed
        // wholesale below, and counting a fold there would describe work the
        // output never sees.
        DominatorInfo reachable = computeDominators(fn);
        for (ir::Block& block : fn.blocks) {
            if (reachable.reachable.find(block.label) ==
                reachable.reachable.end()) {
                continue;
            }
            if (foldTerminator(block, known, result)) {
                changed = true;
            }
        }

        // Reachability after this round's folds. The first block is the entry
        // by the same convention lowering, codegen and dominance use.
        const DominatorInfo current = computeDominators(fn);
        std::vector<ir::Block> kept;
        kept.reserve(fn.blocks.size());
        for (ir::Block& block : fn.blocks) {
            if (current.reachable.find(block.label) ==
                current.reachable.end()) {
                ++result.blocksRemoved;
                changed = true;
                continue;
            }
            kept.push_back(std::move(block));
        }
        fn.blocks = std::move(kept);

        // Rebuild the predecessor sets from the surviving terminators. This is
        // what makes phi pruning follow an edge removed by a fold, not only a
        // block removed by reachability.
        const DominatorInfo pruned = computeDominators(fn);
        for (ir::Block& block : fn.blocks) {
            std::unordered_set<uint32_t> predecessors;
            auto found = pruned.predecessors.find(block.label);
            if (found != pruned.predecessors.end()) {
                for (uint32_t pred : found->second) {
                    predecessors.insert(pred);
                }
            }
            const uint64_t dropped = prunePhiIncoming(block, predecessors);
            if (dropped != 0) {
                result.phiIncomingRemoved += dropped;
                changed = true;
            }
        }

        const uint64_t simplified = simplifySingleIncomingPhis(fn);
        if (simplified != 0) {
            result.phisSimplified += simplified;
            changed = true;
        }

        if (!changed) {
            return;
        }
    }
}

}  // namespace

SimplifyResult simplifyControlFlow(ir::Module& module) {
    SimplifyResult result;
    for (ir::Function& fn : module.functions) {
        simplifyFunction(fn, result);
    }
    return result;
}

}  // namespace opt
}  // namespace cider
