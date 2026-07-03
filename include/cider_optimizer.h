#ifndef CIDER_OPTIMIZER_H
#define CIDER_OPTIMIZER_H

#include "cider_ir.h"
#include <set>
#include <map>

namespace Cider {

    class Optimizer {
    public:
        // Dead Code Elimination: remove instructions whose results are never used
        static void dce(FunctionIR& func) {
            bool changed = true;
            while (changed) {
                changed = false;
                std::set<std::string> used;

                // Collect all used register names
                for (auto& block : func.getBlocks()) {
                    for (auto& instr : block->getInstructions()) {
                        for (auto& arg : instr.args) {
                            if (arg.kind == IRValue::REGISTER) {
                                used.insert(arg.name);
                            }
                        }
                        for (auto& [phiBlock, val] : instr.phiArgs) {
                            if (val.kind == IRValue::REGISTER) {
                                used.insert(val.name);
                            }
                        }
                    }
                }

                // Remove unused instructions
                for (auto& block : func.getBlocks()) {
                    auto& insts = block->getInstructions();
                    for (auto it = insts.begin(); it != insts.end(); ) {
                        if (it->dest && it->dest->kind == IRValue::REGISTER) {
                            if (used.find(it->dest->name) == used.end()) {
                                // Don't remove stores, calls, branches, returns, or asm
                                if (it->op != IROp::STORE && it->op != IROp::CALL &&
                                    it->op != IROp::VIRTUAL_CALL && it->op != IROp::BR &&
                                    it->op != IROp::COND_BR && it->op != IROp::RET &&
                                    it->op != IROp::ASM && it->op != IROp::ALLOCA) {
                                    it = insts.erase(it);
                                    changed = true;
                                    continue;
                                }
                            }
                        }
                        ++it;
                    }
                }
            }
        }

        // Simple inlining: inline functions called only once with small bodies
        static bool inlineFunctions(ModuleIR& module) {
            // Count call sites per function
            std::map<std::string, int> callCounts;
            for (auto& func : module.getFunctions()) {
                for (auto& block : func->getBlocks()) {
                    for (auto& instr : block->getInstructions()) {
                        if (instr.op == IROp::CALL && instr.args.size() > 0) {
                            if (instr.args[0].kind == IRValue::LABEL) {
                                callCounts[instr.args[0].name]++;
                            }
                        }
                    }
                }
            }

            bool inlined = false;
            // For each function called at most once with small body
            for (auto& [name, count] : callCounts) {
                if (count != 1) continue;
                if (name.find("__anon_") == 0) continue; // Skip anonymous functions

                // Find the function
                FunctionIR* callee = nullptr;
                for (auto& f : module.getFunctions()) {
                    if (f->getName() == name) { callee = f.get(); break; }
                }
                if (!callee) continue;

                // Only inline tiny functions (1-3 instructions in body, no branches)
                int bodySize = 0;
                for (auto& block : callee->getBlocks()) {
                    bodySize += (int)block->getInstructions().size();
                }
                if (bodySize > 5 || callee->getBlocks().size() > 2) continue;

                // Find the single call site
                for (auto& caller : module.getFunctions()) {
                    for (auto& block : caller->getBlocks()) {
                        auto& insts = block->getInstructions();
                        for (auto it = insts.begin(); it != insts.end(); ++it) {
                            if (it->op == IROp::CALL && it->args.size() > 0 &&
                                it->args[0].kind == IRValue::LABEL && it->args[0].name == name) {
                                // TODO: Full inlining requires renaming SSA variables,
                                // parameter substitution, and return value rewriting.
                                // For now, mark as inline candidate.
                                (void)it;
                                break;
                            }
                        }
                    }
                }
            }
            return inlined;
        }
    };

}

#endif
