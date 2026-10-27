// blocks.cpp — empty-block elimination and single-predecessor block merging.
//
// This pass runs after constant control-flow simplification and before value
// numbering. Simplification leaves two shapes this pass can clean up without
// reasoning about values: a reachable block whose whole body is `Br target`,
// and a block with one predecessor whose terminator is that predecessor's
// unconditional `Br`.
//
// Both transformations delete one block and redirect edges. They never touch
// the entry block, never create a self-loop, and never guess at a phi. The
// rules are deliberately narrower than a general CFG simplifier:
//
//   - Empty block E (non-entry, empty body, terminator `Br T`): redirect every
//     edge into E to T and delete E. E has no phis because it has no body. If T
//     has phis, the rewrite is allowed only when E has exactly one predecessor
//     P and P is not already a predecessor of T: then T's phi entry for E
//     becomes an entry for P, preserving one incoming per distinct predecessor.
//     With several predecessors, duplicating T's phi value under each new
//     predecessor is not justified here, so the block is left alone.
//   - Merge block B (non-entry, exactly one predecessor P, `P: Br B`, B != P)
//     into P. B must have no phis, so there is no B-entry phi to collapse.
//     B's body and terminator move to the end of P; every successor phi entry
//     for B is relabelled P. The merge is refused when a successor is P (it
//     would create a self-loop) or when a successor already names P as a
//     predecessor (two edges from one predecessor cannot both have a phi
//     value).
//
// The transformations are monotone (each removes one block and preserves the
// surviving order), so a function is cleaned to a fixpoint. Blocks are scanned
// in module order and candidates are chosen in that order, so the result does
// not depend on hash iteration.
//
// Assembly can name a code-generator block label (`.L_<function>_<label>`) as
// plain text. A function whose module-level or inline assembly spells that
// prefix is left entirely alone: deleting or merging a block would remove a
// label the assembler still needs. A block whose body contains an `Asm`,
// `Trap`, or volatile instruction is never merged, and a block ending in a
// `Trap` terminator is never merged either, because this pass promises not to
// move those effects. Empty-block elimination only moves edges: its candidate
// has no body and a plain `Br` terminator.
//
// Traps, volatile operations, calls and stores in surviving blocks are neither
// deleted nor reordered: merging appends B's body after P's body on P's only
// successor edge, which is exactly where it already executed.

#include "opt/opt.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cider {
namespace opt {

namespace {

using Predecessors = std::unordered_map<uint32_t, std::vector<uint32_t>>;

constexpr uint64_t kOnePredecessor = 1;
constexpr uint64_t kFirstBlock = 0;

bool hasPhi(const ir::Block& block) {
    for (const ir::Instr& instr : block.body) {
        if (instr.op == ir::Op::Phi) {
            return true;
        }
    }
    return false;
}

// A block whose effects the merge rule refuses to relocate. A phi never
// appears here because merge candidates are required to have none.
bool hasPinnedEffect(const ir::Block& block) {
    for (const ir::Instr& instr : block.body) {
        if (instr.op == ir::Op::Asm || instr.op == ir::Op::Trap ||
            instr.isVolatile) {
            return true;
        }
    }
    return false;
}

bool contains(const std::vector<uint32_t>& labels, uint32_t label) {
    return std::find(labels.begin(), labels.end(), label) != labels.end();
}

void addOnce(std::vector<uint32_t>& labels, uint32_t label) {
    if (!contains(labels, label)) {
        labels.push_back(label);
    }
}

void removeOne(std::vector<uint32_t>& labels, uint32_t label) {
    labels.erase(std::remove(labels.begin(), labels.end(), label),
                 labels.end());
}

// Distinct successors of a terminator, in edge order. Used by the merge rule
// to touch each successor's phis exactly once even when a Switch names one
// target from several cases.
std::vector<uint32_t> successors(const ir::Terminator& term) {
    std::vector<uint32_t> out;
    switch (term.term) {
        case ir::Term::Br:
            addOnce(out, term.target);
            break;
        case ir::Term::CondBr:
            addOnce(out, term.thenTarget);
            addOnce(out, term.elseTarget);
            break;
        case ir::Term::Switch:
            for (const ir::Case& arm : term.cases) {
                addOnce(out, arm.target);
            }
            addOnce(out, term.defaultTarget);
            break;
        case ir::Term::Ret:
        case ir::Term::Unreachable:
        case ir::Term::Trap:
            break;
    }
    return out;
}

void redirectEdges(ir::Terminator& term, uint32_t from, uint32_t to) {
    switch (term.term) {
        case ir::Term::Br:
            if (term.target == from) {
                term.target = to;
            }
            break;
        case ir::Term::CondBr:
            if (term.thenTarget == from) {
                term.thenTarget = to;
            }
            if (term.elseTarget == from) {
                term.elseTarget = to;
            }
            break;
        case ir::Term::Switch:
            for (ir::Case& arm : term.cases) {
                if (arm.target == from) {
                    arm.target = to;
                }
            }
            if (term.defaultTarget == from) {
                term.defaultTarget = to;
            }
            break;
        case ir::Term::Ret:
        case ir::Term::Unreachable:
        case ir::Term::Trap:
            break;
    }
}

// Every distinct predecessor of every block, in block order. Only terminators
// are read: the map is the one mutable CFG fact this pass maintains as it
// rewrites edges, so a later candidate sees the graph the earlier one made.
Predecessors buildPredecessors(const ir::Function& fn) {
    Predecessors preds;
    std::unordered_set<uint32_t> labels;
    for (const ir::Block& block : fn.blocks) {
        labels.insert(block.label);
    }
    for (const ir::Block& block : fn.blocks) {
        for (uint32_t target : successors(block.term)) {
            if (labels.find(target) == labels.end()) {
                continue;
            }
            addOnce(preds[target], block.label);
        }
    }
    return preds;
}

ir::Block* findBlock(ir::Function& fn, uint32_t label) {
    for (ir::Block& block : fn.blocks) {
        if (block.label == label) {
            return &block;
        }
    }
    return nullptr;
}

// Relabel one phi incoming per phi word that names `from`. Entries are matched
// by block label, never by position, so two words of one aggregate stay
// aligned even when their incoming lists are ordered differently. Callers
// prove the destination does not already name the new predecessor, so no
// duplicate incoming is created.
void rewritePhiIncoming(ir::Block& block, uint32_t from, uint32_t to) {
    for (ir::Instr& instr : block.body) {
        if (instr.op != ir::Op::Phi) {
            continue;
        }
        for (ir::PhiIncoming& incoming : instr.phiIncoming) {
            if (incoming.block == from) {
                incoming.block = to;
            }
        }
    }
}

// Rule 1. `block` is fn.blocks[index]. On success it is erased and `index`
// addresses the next block.
bool eliminateEmpty(ir::Function& fn, size_t index, Predecessors& preds,
                    uint64_t& removed) {
    const uint32_t entry = fn.blocks.front().label;
    ir::Block& block = fn.blocks[index];
    if (block.label == entry || !block.body.empty() ||
        block.term.term != ir::Term::Br) {
        return false;
    }
    const uint32_t target = block.term.target;
    if (target == block.label) {
        return false;
    }
    ir::Block* destination = findBlock(fn, target);
    if (destination == nullptr) {
        return false;
    }
    std::vector<uint32_t> incoming;
    auto found = preds.find(block.label);
    if (found != preds.end()) {
        incoming = found->second;
    }
    // An edge from the destination back into the candidate would become a
    // destination self-loop after the redirect.
    if (contains(incoming, target)) {
        return false;
    }
    if (hasPhi(*destination)) {
        if (incoming.size() != kOnePredecessor) {
            return false;
        }
        const uint32_t pred = incoming[0];
        auto destinationPreds = preds.find(target);
        if (destinationPreds != preds.end() &&
            contains(destinationPreds->second, pred)) {
            // The predecessor already reaches the destination directly, so
            // relabelling the candidate's incoming entry would give the
            // destination two entries for one predecessor.
            return false;
        }
    }

    for (uint32_t pred : incoming) {
        ir::Block* predecessor = findBlock(fn, pred);
        if (predecessor != nullptr) {
            redirectEdges(predecessor->term, block.label, target);
        }
    }
    if (hasPhi(*destination)) {
        rewritePhiIncoming(*destination, block.label, incoming[0]);
    }
    // The removed block itself was a predecessor of the destination through
    // its `Br target`; drop that stale entry before adding the redirected
    // predecessors, so a later candidate sees the real graph.
    auto destinationPreds = preds.find(target);
    if (destinationPreds != preds.end()) {
        removeOne(destinationPreds->second, block.label);
    }
    for (uint32_t pred : incoming) {
        addOnce(preds[target], pred);
    }
    preds.erase(block.label);
    fn.blocks.erase(fn.blocks.begin() + static_cast<std::ptrdiff_t>(index));
    ++removed;
    return true;
}

// Rule 2. `block` is fn.blocks[index]. On success it is erased.
bool mergeIntoPredecessor(ir::Function& fn, size_t index,
                          Predecessors& preds, uint64_t& merged) {
    const uint32_t entry = fn.blocks.front().label;
    ir::Block& block = fn.blocks[index];
    if (block.label == entry || hasPhi(block) || hasPinnedEffect(block) ||
        block.term.term == ir::Term::Trap) {
        return false;
    }
    auto found = preds.find(block.label);
    if (found == preds.end() || found->second.size() != kOnePredecessor) {
        return false;
    }
    const uint32_t predLabel = found->second[0];
    if (predLabel == block.label) {
        return false;
    }
    ir::Block* pred = findBlock(fn, predLabel);
    if (pred == nullptr || pred->term.term != ir::Term::Br ||
        pred->term.target != block.label) {
        return false;
    }

    const std::vector<uint32_t> next = successors(block.term);
    for (uint32_t target : next) {
        if (target == block.label || target == predLabel) {
            // The candidate would either vanish from its own terminator or
            // leave the predecessor branching to itself.
            return false;
        }
        ir::Block* destination = findBlock(fn, target);
        if (destination == nullptr) {
            return false;
        }
        if (hasPhi(*destination)) {
            auto destinationPreds = preds.find(target);
            if (destinationPreds != preds.end() &&
                contains(destinationPreds->second, predLabel)) {
                // After the merge the destination would have two edges from
                // one predecessor block, which one phi value cannot describe.
                return false;
            }
        }
    }

    for (uint32_t target : next) {
        ir::Block* destination = findBlock(fn, target);
        if (hasPhi(*destination)) {
            rewritePhiIncoming(*destination, block.label, predLabel);
        }
        auto destinationPreds = preds.find(target);
        if (destinationPreds != preds.end()) {
            removeOne(destinationPreds->second, block.label);
            addOnce(destinationPreds->second, predLabel);
        }
    }
    preds.erase(block.label);

    for (ir::Instr& instr : block.body) {
        pred->body.push_back(std::move(instr));
    }
    pred->term = block.term;
    fn.blocks.erase(fn.blocks.begin() + static_cast<std::ptrdiff_t>(index));
    ++merged;
    return true;
}

bool asmNamesBlockLabel(const std::string& functionName,
                        const std::string& moduleAsm,
                        const std::vector<std::string>& inlineAsm) {
    // Codegen spells a block label `.L_` + function name + `_` + label.
    // Matching the whole prefix is conservative: a near-name false positive
    // only leaves that function unoptimized.
    const std::string prefix = ".L_" + functionName + "_";
    if (moduleAsm.find(prefix) != std::string::npos) {
        return true;
    }
    for (const std::string& text : inlineAsm) {
        if (text.find(prefix) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void cleanupFunction(ir::Function& fn, BlocksResult& result) {
    if (fn.blocks.empty()) {
        return;
    }
    Predecessors preds = buildPredecessors(fn);
    for (;;) {
        bool changed = false;
        uint64_t index = kFirstBlock;
        while (index < fn.blocks.size()) {
            if (eliminateEmpty(fn, index, preds, result.emptyBlocksRemoved)) {
                changed = true;
                continue;
            }
            if (mergeIntoPredecessor(fn, index, preds, result.blocksMerged)) {
                changed = true;
                continue;
            }
            ++index;
        }
        if (!changed) {
            return;
        }
    }
}

}  // namespace

BlocksResult cleanupBlocks(ir::Module& module) {
    BlocksResult result;
    // Collect assembly text once: an inline block may legally spell a label
    // of any function in this object, and module-wide assembly is emitted in
    // the same file.
    std::vector<std::string> inlineAsm;
    for (const ir::Function& fn : module.functions) {
        for (const ir::Block& block : fn.blocks) {
            for (const ir::Instr& instr : block.body) {
                if (instr.op == ir::Op::Asm) {
                    inlineAsm.push_back(instr.asmText);
                }
            }
        }
    }
    for (ir::Function& fn : module.functions) {
        if (asmNamesBlockLabel(fn.name, module.moduleAsm, inlineAsm)) {
            continue;
        }
        cleanupFunction(fn, result);
    }
    return result;
}

}  // namespace opt
}  // namespace cider
