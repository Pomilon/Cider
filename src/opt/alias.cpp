// alias.cpp — per-slot escape and GEP-range facts for one function.
//
// This is the per-slot counterpart of memory.cpp's whole-function bail-out.
// An Alloca stays in `nonEscaping` only when its address is used by direct
// Load/Store field accesses (or a constant-index GEP) with no call, phi,
// stored-value, terminator, volatile or asm use. Frame cells are adjacent,
// so a GEP displacement that can reach a neighbour removes every Alloca in
// the function: a per-slot answer alone cannot see the neighbour.

#include "opt/opt.h"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace cider {
namespace opt {
namespace {

constexpr uint64_t kMoreThanOne = 2;

// The four instructions whose addressBase names a pointer-like base.
bool namesBase(ir::Op op) {
    return op == ir::Op::Load || op == ir::Op::Store ||
           op == ir::Op::Extract || op == ir::Op::Insert ||
           op == ir::Op::Gep;
}

}  // namespace

SlotFacts analyzeMemory(const ir::Function& fn) {
    SlotFacts facts;

    // Every Alloca result is a candidate slot. Repeated result ids are not
    // expected (the structural checker rejects them for non-parameters), but
    // the set de-duplicates them for safety.
    std::unordered_set<uint32_t> allocas;
    for (const ir::Block& block : fn.blocks) {
        for (const ir::Instr& instr : block.body) {
            if (instr.op == ir::Op::Alloca) {
                for (const ir::Value& result : instr.results) {
                    allocas.insert(result.id);
                }
            }
        }
    }

    // SSA constants let a GEP's index operand be folded into its displacement.
    // A non-constant index escapes the base: the address it computes is not a
    // field this analysis can bound.
    std::unordered_map<uint32_t, int64_t> consts;
    for (const ir::Block& block : fn.blocks) {
        for (const ir::Instr& instr : block.body) {
            if (instr.op == ir::Op::Const && !instr.results.empty()) {
                consts[instr.results[0].id] = instr.constantBits;
            }
        }
    }

    std::unordered_set<uint32_t> escaped;
    bool neighbourHazard = false;

    auto note = [&](uint32_t id) {
        if (allocas.find(id) != allocas.end()) {
            escaped.insert(id);
        }
    };

    for (const ir::Block& block : fn.blocks) {
        for (const ir::Instr& instr : block.body) {
            if (instr.op == ir::Op::Asm) {
                // Asm can name frame storage by register without an operand,
                // so no IR use reveals the escape.
                facts.asmPresent = true;
                continue;
            }
            if (instr.op == ir::Op::Alloca) {
                continue;
            }

            // Ordinary uses: operands, argument words, phi incomings, the
            // value a Store writes, an indirect callee. Any slot id here is an
            // escape rather than a field access.
            for (const ir::Value& operand : instr.operands) {
                note(operand.id);
            }
            for (const ir::Argument& arg : instr.arguments) {
                for (const ir::Value& word : arg.words) {
                    note(word.id);
                }
            }
            for (const ir::PhiIncoming& incoming : instr.phiIncoming) {
                note(incoming.value.id);
            }
            if (instr.op == ir::Op::Store || instr.op == ir::Op::Insert) {
                note(instr.storedValue.id);
            }
            if (instr.op == ir::Op::Call && instr.call.indirect) {
                note(instr.calleeValue.id);
            }

            if (namesBase(instr.op)) {
                // A link label names the label, not the default zero base.
                const bool labelBase =
                    (instr.op == ir::Op::Load || instr.op == ir::Op::Store) &&
                    !instr.globalLabel.empty();
                if (!labelBase &&
                    allocas.find(instr.addressBase.id) != allocas.end()) {
                    const uint32_t base = instr.addressBase.id;
                    if (instr.op == ir::Op::Load || instr.op == ir::Op::Store) {
                        // Volatile reads/writes are observable and must never
                        // be removed. A direct access at a constant
                        // displacement is a field access, not an escape; the
                        // fields of one aggregate are promoted together.
                        if (instr.isVolatile) {
                            note(base);
                        }
                        if (instr.displacement != 0 &&
                            allocas.size() > kMoreThanOne) {
                            neighbourHazard = true;
                        }
                    } else if (instr.op == ir::Op::Gep) {
                        int64_t displacement = instr.displacement;
                        if (!instr.operands.empty()) {
                            auto found = consts.find(instr.operands[0].id);
                            if (found == consts.end()) {
                                note(base);  // runtime index: unbounded range
                            } else {
                                displacement += found->second;
                            }
                        }
                        facts.gepDisplacement[base] = displacement;
                        // A GEP yields a computed address. Even a zero
                        // displacement can be added to a runtime offset later
                        // (the lowerer does exactly that for `this->field`),
                        // and that address can reach an adjacent cell. Any GEP
                        // in the function is therefore a whole-function
                        // hazard, not just a non-zero displacement one.
                        neighbourHazard = true;
                    } else {
                        // Extract/Insert widen or reinterpret the pointer.
                        note(base);
                    }
                }
            }
        }

        const ir::Terminator& term = block.term;
        for (const ir::Value& word : term.values) {
            note(word.id);
        }
        if (term.cond.has_value()) {
            note(term.cond->id);
        }
        if (term.term == ir::Term::Switch) {
            note(term.switchValue.id);
            for (const ir::Case& arm : term.cases) {
                note(arm.value.id);
            }
        }
    }

    // Asm and a neighbour-reaching displacement are whole-function hazards:
    // neither leaves any slot provably isolated.
    if (facts.asmPresent || neighbourHazard) {
        return facts;
    }

    for (uint32_t id : allocas) {
        if (escaped.find(id) == escaped.end()) {
            facts.nonEscaping.insert(id);
        }
    }
    return facts;
}

}  // namespace opt
}  // namespace cider
