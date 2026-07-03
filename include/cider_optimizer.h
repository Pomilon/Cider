#ifndef CIDER_OPTIMIZER_H
#define CIDER_OPTIMIZER_H

#include "cider_ir.h"
#include <set>
#include <map>
#include <vector>

namespace Cider {

    class Optimizer {
    public:
        static void dce(FunctionIR& func) {
            bool changed = true;
            while (changed) {
                changed = false;
                std::set<std::string> used;
                for (auto& block : func.getBlocks()) {
                    for (auto& instr : block->getInstructions()) {
                        for (auto& arg : instr.args) {
                            if (arg.kind == IRValue::REGISTER) used.insert(arg.name);
                        }
                        for (auto& [phiBlock, val] : instr.phiArgs) {
                            if (val.kind == IRValue::REGISTER) used.insert(val.name);
                        }
                    }
                }
                for (auto& block : func.getBlocks()) {
                    auto& insts = block->getInstructions();
                    for (auto it = insts.begin(); it != insts.end(); ) {
                        if (it->dest && it->dest->kind == IRValue::REGISTER) {
                            if (used.find(it->dest->name) == used.end()) {
                                if (it->op != IROp::STORE && it->op != IROp::CALL &&
                                    it->op != IROp::VIRTUAL_CALL && it->op != IROp::BR &&
                                    it->op != IROp::COND_BR && it->op != IROp::RET &&
                                    it->op != IROp::ASM && it->op != IROp::ALLOCA &&
                                    it->op != IROp::MOV && it->op != IROp::LOAD &&
                                    it->op != IROp::PUSH && it->op != IROp::POP) {
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

        static bool inlineFunctions(ModuleIR& module) {
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

            bool anyInlined = false;
            for (auto& [name, count] : callCounts) {
                if (count != 1) continue;
                if (name.find("__anon_") == 0) continue;

                FunctionIR* callee = nullptr;
                int calleeIdx = -1;
                for (size_t i = 0; i < module.getFunctions().size(); i++) {
                    if (module.getFunctions()[i]->getName() == name) {
                        callee = module.getFunctions()[i].get();
                        calleeIdx = (int)i;
                        break;
                    }
                }
                if (!callee) continue;

                int bodySize = 0;
                int blockCount = (int)callee->getBlocks().size();
                for (auto& block : callee->getBlocks()) {
                    bodySize += (int)block->getInstructions().size();
                    for (auto& ci : block->getInstructions()) {
                        if (ci.op == IROp::BR || ci.op == IROp::COND_BR) blockCount = 99;
                    }
                }
                if (bodySize > 8 || blockCount > 1) continue;

                for (auto& caller : module.getFunctions()) {
                    if (caller.get() == callee) continue;

                    // Build block rename map (avoid label conflicts)
                    std::map<std::string, std::string> blockRename;
                    for (auto& cb : callee->getBlocks()) {
                        blockRename[cb->getName()] = name + "_" + cb->getName();
                    }

                    for (auto& block : caller->getBlocks()) {
                        auto& insts = block->getInstructions();
                        for (auto it = insts.begin(); it != insts.end(); ++it) {
                            if (it->op == IROp::CALL && it->args.size() > 0 &&
                                it->args[0].kind == IRValue::LABEL && it->args[0].name == name) {

                                std::string prefix = name + "_";
                                std::map<std::string, std::string> renameMap;

                                // Build rename map for callee registers and block labels
                                for (auto& cb : callee->getBlocks()) {
                                    for (auto& ci : cb->getInstructions()) {
                                        if (ci.dest && ci.dest->kind == IRValue::REGISTER) {
                                            if (renameMap.find(ci.dest->name) == renameMap.end()) {
                                                renameMap[ci.dest->name] = prefix + ci.dest->name;
                                            }
                                        }
                                        for (auto& arg : ci.args) {
                                            if (arg.kind == IRValue::REGISTER && renameMap.find(arg.name) == renameMap.end()) {
                                                renameMap[arg.name] = prefix + arg.name;
                                            }
                                            // Rename block labels in branches
                                            if (arg.kind == IRValue::LABEL) {
                                                auto br = blockRename.find(arg.name);
                                                if (br != blockRename.end()) arg.name = br->second;
                                            }
                                        }
                                    }
                                }

                                // Rename actual basic block objects
                                for (auto& cb : callee->getBlocks()) {
                                    auto br = blockRename.find(cb->getName());
                                    if (br != blockRename.end()) cb->setName(br->second);
                                }

                                // Map callee parameters to caller's argument values
                                const auto& calleeParams = callee->getParams();
                                for (size_t pi = 0; pi < calleeParams.size() && pi + 1 < it->args.size(); pi++) {
                                    renameMap[calleeParams[pi]] = it->args[pi + 1].name;
                                }

                                // Clone callee blocks with renamed registers
                                std::vector<IRInstruction> inlinedInsts;
                                std::string retDest;

                                for (auto& cb : callee->getBlocks()) {
                                    for (auto& ci : cb->getInstructions()) {
                                        IRInstruction cloned = ci;
                                        if (cloned.dest && cloned.dest->kind == IRValue::REGISTER) {
                                            auto rn = renameMap.find(cloned.dest->name);
                                            if (rn != renameMap.end()) cloned.dest->name = rn->second;
                                        }
                                        for (auto& arg : cloned.args) {
                                            if (arg.kind == IRValue::REGISTER) {
                                                auto rn = renameMap.find(arg.name);
                                                if (rn != renameMap.end()) arg.name = rn->second;
                                            }
                                            if (arg.kind == IRValue::LABEL) {
                                                auto br = blockRename.find(arg.name);
                                                if (br != blockRename.end()) arg.name = br->second;
                                            }
                                        }
                                        for (auto& [phiBlock, val] : cloned.phiArgs) {
                                            if (val.kind == IRValue::REGISTER) {
                                                auto rn = renameMap.find(val.name);
                                                if (rn != renameMap.end()) val.name = rn->second;
                                            }
                                        }
                                        if (cloned.op == IROp::RET && cloned.args.size() > 0) {
                                            if (cloned.args[0].kind == IRValue::REGISTER) {
                                                retDest = cloned.args[0].name;
                                            }
                                        }
                                        inlinedInsts.push_back(std::move(cloned));
                                    }
                                }

                                auto callDest = it->dest;
                                it = insts.erase(it);

                                for (auto& ci : inlinedInsts) {
                                    if (ci.op == IROp::RET) {
                                        if (callDest && !retDest.empty()) {
                                            IRInstruction mov;
                                            mov.op = IROp::MOV;
                                            mov.dest = *callDest;
                                            mov.args.push_back(IRValue::Register(retDest, callDest->type));
                                            it = insts.insert(it, std::move(mov));
                                            ++it;
                                        }
                                    } else {
                                        it = insts.insert(it, std::move(ci));
                                        ++it;
                                    }
                                }

                                anyInlined = true;
                                goto next_func;
                            }
                        }
                    }
                }
                next_func:;
            }

            return anyInlined;
        }
    };

}

#endif
