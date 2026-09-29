#pragma once

#include <iostream>
#include <map>
#include <utility>
#include <vector>

#include "ir.h"

namespace cider {

/**
 * SSA Deconstructor that handles the "Parallel Move" problem
 * by using the stack (PUSH/POP) to safely shuffle registers.
 *
 * CIDER-BUG-204: NATIVE-ONLY pass. Its output is the physical-register form
 * codegen consumes, NOT an interchangeable IR form:
 *
 *   - The parallel-move problem only exists because x86 has a finite set of
 *     physical registers, so a phi edge may need several values moved at once
 *     and the sources/destinations can overlap. PUSH-all-then-POP-in-reverse
 *     is the standard fix, and codegen lowers PUSH/POP to real `push r*` /
 *     `pop r*` on allocated registers.
 *   - The VM has an unbounded map of virtual registers, so the same edge is a
 *     plain copy. The VM therefore runs the PHI form of the IR and its
 *     interpreter rejects PUSH/POP (see the IROp::PHI case in vm.cpp).
 *
 * `cider run` must NOT call this pass. The two backends executing different
 * IR shapes is expected; tests/run_tests.py pins that they still agree on
 * stdout/stderr/exit. Wiring this pass into the run path is the regression
 * this class documents.
 */
class SSADeconstructor {
public:
    void process(ModuleIR& module) {
        for (auto& func : module.getFunctions()) {
            // For each block, we collect the PHI moves coming FROM it
            std::map<BasicBlock*, std::vector<std::pair<IRValue, IRValue>>> predecessorMoves;

            for (auto& block : func->getBlocks()) {
                auto& instrs = block->getInstructions();
                std::vector<IRInstruction> newInstrs;

                for (auto& instr : instrs) {
                    if (instr.op == IROp::PHI) {
                        for (const auto& [predName, val] : instr.phiArgs) {
                            BasicBlock* predBlock = nullptr;
                            for (auto& b : func->getBlocks()) {
                                if (b->getName() == predName) {
                                    predBlock = b.get();
                                    break;
                                }
                            }

                            if (predBlock) {
                                // Move val -> instr.dest
                                predecessorMoves[predBlock].push_back({*instr.dest, val});
                            }
                        }
                    } else {
                        newInstrs.push_back(instr);
                    }
                }
                instrs = std::move(newInstrs);
            }

            // Now insert the moves into the predecessors
            for (auto& [block, moves] : predecessorMoves) {
                if (moves.empty()) continue;

                std::vector<IRInstruction> deconstructed;

                // To handle parallel moves (like swaps or chains) without trashing registers:
                // 1. PUSH all source values onto the stack
                for (const auto& move : moves) {
                    IRInstruction pushInstr;
                    pushInstr.op = IROp::PUSH;
                    pushInstr.args.push_back(move.second);
                    deconstructed.push_back(pushInstr);
                }

                // 2. POP them into the destinations in REVERSE order
                for (auto it = moves.rbegin(); it != moves.rend(); ++it) {
                    IRInstruction popInstr;
                    popInstr.op = IROp::POP;
                    popInstr.dest = it->first;
                    deconstructed.push_back(popInstr);
                }

                auto& instrs = block->getInstructions();
                // Insert before the terminator (BR, COND_BR, or RET)
                if (!instrs.empty() && (instrs.back().op == IROp::BR || instrs.back().op == IROp::COND_BR || instrs.back().op == IROp::RET)) {
                    instrs.insert(instrs.end() - 1, deconstructed.begin(), deconstructed.end());
                } else {
                    instrs.insert(instrs.end(), deconstructed.begin(), deconstructed.end());
                }
            }
        }
    }
};

}  // namespace cider
