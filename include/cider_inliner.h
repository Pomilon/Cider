#ifndef CIDER_INLINER_H
#define CIDER_INLINER_H

#include "cider_ir.h"
#include <algorithm>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <stack>
#include <string>
#include <vector>

namespace Cider {

    class CallGraph {
    public:
        void build(const ModuleIR& module) {
            edges_.clear();
            sccId_.clear();
            containsIndirectCall_.clear();

            for (const auto& func : module.getFunctions()) {
                const std::string& name = func->getName();
                edges_[name] = {};
                for (const auto& block : func->getBlocks()) {
                    for (const auto& instr : block->getInstructions()) {
                        if ((instr.op == IROp::CALL || instr.op == IROp::VIRTUAL_CALL) && !instr.args.empty()) {
                            if (instr.args[0].kind == IRValue::LABEL) {
                                edges_[name].push_back(instr.args[0].name);
                            } else {
                                containsIndirectCall_.insert(name);
                            }
                        }
                    }
                }
            }
            tarjan();
        }

        bool sameSCC(const std::string& a, const std::string& b) const {
            auto ia = sccId_.find(a);
            auto ib = sccId_.find(b);
            return ia != sccId_.end() && ib != sccId_.end() && ia->second == ib->second;
        }

        bool containsIndirectCall(const std::string& func) const {
            return containsIndirectCall_.find(func) != containsIndirectCall_.end();
        }

    private:
        std::map<std::string, std::vector<std::string>> edges_;
        std::map<std::string, int> sccId_;
        std::set<std::string> containsIndirectCall_;

        void tarjan() {
            std::map<std::string, int> index;
            std::map<std::string, int> lowlink;
            std::stack<std::string> S;
            std::set<std::string> onStack;
            int idx = 0;
            int scc = 0;

            std::function<void(const std::string&)> strongconnect = [&](const std::string& v) {
                index[v] = idx;
                lowlink[v] = idx;
                idx++;
                S.push(v);
                onStack.insert(v);

                auto it = edges_.find(v);
                if (it != edges_.end()) {
                    for (const std::string& w : it->second) {
                        if (index.find(w) == index.end()) {
                            strongconnect(w);
                            lowlink[v] = std::min(lowlink[v], lowlink[w]);
                        } else if (onStack.find(w) != onStack.end()) {
                            lowlink[v] = std::min(lowlink[v], index[w]);
                        }
                    }
                }

                if (lowlink[v] == index[v]) {
                    while (true) {
                        std::string w = S.top();
                        S.pop();
                        onStack.erase(w);
                        sccId_[w] = scc;
                        if (w == v) break;
                    }
                    scc++;
                }
            };

            for (const auto& kv : edges_) {
                if (index.find(kv.first) == index.end()) {
                    strongconnect(kv.first);
                }
            }
        }
    };

    struct HybridCycleCostModel {
        static constexpr int CALL_OVERHEAD = 5;
        static constexpr int BLOAT_CAP = 40;

        bool shouldInline(const FunctionIR& callee, int nSites, const CallGraph& graph) const {
            if (graph.containsIndirectCall(callee.getName())) return false;

            static const std::map<IROp, int> latency = {
                {IROp::MOV, 1}, {IROp::ADD, 1}, {IROp::SUB, 1},
                {IROp::BIT_AND, 1}, {IROp::BIT_OR, 1}, {IROp::BIT_XOR, 1}, {IROp::BIT_NOT, 1},
                {IROp::LSHIFT, 1}, {IROp::RSHIFT, 1}, {IROp::BR, 1}, {IROp::RET, 1},
                {IROp::ALLOCA, 1}, {IROp::PUSH, 1}, {IROp::POP, 1},
                {IROp::CMP_EQ, 1}, {IROp::CMP_NE, 1}, {IROp::CMP_LT, 1},
                {IROp::CMP_GT, 1}, {IROp::CMP_LE, 1}, {IROp::CMP_GE, 1},
                {IROp::LOAD, 4}, {IROp::STORE, 4}, {IROp::MEMCPY, 4},
                {IROp::MUL, 3}, {IROp::DIV, 20}, {IROp::MOD, 20},
                {IROp::CALL, 20}, {IROp::VIRTUAL_CALL, 20}, {IROp::COND_BR, 2},
                {IROp::CVT_ITOF, 3}, {IROp::CVT_FTOI, 3}, {IROp::ASM, 10},
                {IROp::PHI, 0}
            };

            int bodyInstrs = 0;
            int bodyCost = 0;
            for (const auto& block : callee.getBlocks()) {
                for (const auto& instr : block->getInstructions()) {
                    bodyInstrs++;
                    auto it = latency.find(instr.op);
                    bodyCost += (it != latency.end()) ? it->second : 4;
                }
            }

            if (bodyInstrs > BLOAT_CAP) return false;
            return (CALL_OVERHEAD * nSites) >= bodyCost;
        }
    };

    template <typename CostModel = HybridCycleCostModel>
    class Inliner {
    public:
        bool run(ModuleIR& module) {
            module_ = &module;
            graph_.build(module);
            functionMap_.clear();
            remainingSites_.clear();
            for (auto& func : module.getFunctions()) {
                functionMap_[func->getName()] = func.get();
                for (auto& block : func->getBlocks()) {
                    for (auto& instr : block->getInstructions()) {
                        if ((instr.op == IROp::CALL || instr.op == IROp::VIRTUAL_CALL) && !instr.args.empty()) {
                            if (instr.args[0].kind == IRValue::LABEL) {
                                remainingSites_[instr.args[0].name]++;
                            }
                        }
                    }
                }
            }
            bool changed = false;
            for (auto& func : module.getFunctions()) {
                if (inlineFunction(*func)) changed = true;
            }
            return changed;
        }

    private:
        ModuleIR* module_ = nullptr;
        CallGraph graph_;
        CostModel costModel_;
        int siteCounter_ = 0;
        std::map<std::string, FunctionIR*> functionMap_;
        std::map<std::string, int> remainingSites_;

        bool inlineFunction(FunctionIR& func) {
            bool changed = false;
            for (size_t bi = 0; bi < func.getBlocks().size(); ++bi) {
                auto& block = *func.getBlocks()[bi];
                auto& insts = block.getInstructions();
                for (size_t ii = 0; ii < insts.size(); ++ii) {
                    auto& instr = insts[ii];
                    if (instr.op != IROp::CALL || instr.args.empty() || instr.args[0].kind != IRValue::LABEL) {
                        continue;
                    }
                    std::string calleeName = instr.args[0].name;
                    auto fit = functionMap_.find(calleeName);
                    if (fit == functionMap_.end()) continue;
                    FunctionIR* callee = fit->second;
                    if (graph_.sameSCC(func.getName(), calleeName)) continue;
                    if (instr.args.size() != callee->getParams().size() + 1) continue;
                    if (callee->getBlocks().empty()) continue;
                    if (!costModel_.shouldInline(*callee, remainingSites_[calleeName], graph_)) continue;

                    size_t numCloned = inlineAtSite(func, bi, ii, *callee);
                    remainingSites_[calleeName]--;
                    changed = true;
                    bi += numCloned;
                    break;
                }
            }
            return changed;
        }

        size_t inlineAtSite(FunctionIR& caller, size_t blockIdx, size_t pos, FunctionIR& callee) {
            auto& block = *caller.getBlocks()[blockIdx];
            std::string prefix = callee.getName() + "_" + std::to_string(siteCounter_++) + "_";
            const IRInstruction callInstr = block.getInstructions()[pos];

            std::map<std::string, std::string> blockRename;
            for (const auto& cb : callee.getBlocks()) {
                blockRename[cb->getName()] = prefix + cb->getName();
            }

            std::map<std::string, IRValue> paramMap;
            for (size_t pi = 0; pi < callee.getParams().size() && pi + 1 < callInstr.args.size(); ++pi) {
                paramMap[callee.getParams()[pi]] = callInstr.args[pi + 1];
            }

            std::map<std::string, std::string> regRename;
            for (const auto& cb : callee.getBlocks()) {
                for (const auto& ci : cb->getInstructions()) {
                    if (ci.dest && ci.dest->kind == IRValue::REGISTER) {
                        if (paramMap.find(ci.dest->name) == paramMap.end()) {
                            regRename[ci.dest->name] = prefix + ci.dest->name;
                        }
                    }
                    for (const auto& arg : ci.args) {
                        if (arg.kind == IRValue::REGISTER && paramMap.find(arg.name) == paramMap.end()) {
                            regRename[arg.name] = prefix + arg.name;
                        }
                    }
                    for (const auto& kv : ci.asmMap) {
                        if (kv.second.kind == IRValue::REGISTER && paramMap.find(kv.second.name) == paramMap.end()) {
                            regRename[kv.second.name] = prefix + kv.second.name;
                        }
                    }
                }
            }

            auto remapValue = [&](const IRValue& v) -> IRValue {
                if (v.kind == IRValue::REGISTER) {
                    auto pit = paramMap.find(v.name);
                    if (pit != paramMap.end()) return pit->second;
                    auto rit = regRename.find(v.name);
                    if (rit != regRename.end()) return IRValue::Register(rit->second, v.type);
                } else if (v.kind == IRValue::LABEL) {
                    auto bit = blockRename.find(v.name);
                    if (bit != blockRename.end()) return IRValue::Label(bit->second);
                }
                return v;
            };

            std::string entryName = callee.getBlocks().front()->getName();
            std::string afterName = prefix + "after";

            auto& insts = block.getInstructions();
            std::vector<IRInstruction> afterInsts(insts.begin() + pos + 1, insts.end());
            insts.erase(insts.begin() + pos, insts.end());
            IRInstruction entryBr;
            entryBr.op = IROp::BR;
            entryBr.args.push_back(IRValue::Label(blockRename[entryName]));
            insts.push_back(entryBr);

            std::vector<std::unique_ptr<BasicBlock>> newBlocks;
            for (const auto& cb : callee.getBlocks()) {
                auto clone = std::make_unique<BasicBlock>(blockRename[cb->getName()]);
                for (const auto& ci : cb->getInstructions()) {
                    IRInstruction cloned = ci;

                    if (cloned.dest && cloned.dest->kind == IRValue::REGISTER) {
                        *cloned.dest = remapValue(*cloned.dest);
                    }
                    for (auto& arg : cloned.args) {
                        arg = remapValue(arg);
                    }
                    for (auto& kv : cloned.asmMap) {
                        kv.second = remapValue(kv.second);
                    }
                    {
                        std::map<std::string, IRValue> remappedPhi;
                        for (const auto& kv : cloned.phiArgs) {
                            std::string newBlock = blockRename[kv.first];
                            remappedPhi[newBlock] = remapValue(kv.second);
                        }
                        cloned.phiArgs = std::move(remappedPhi);
                    }

                    if (cloned.op == IROp::RET) {
                        if (callInstr.dest && !cloned.args.empty()) {
                            IRInstruction mov;
                            mov.op = IROp::MOV;
                            mov.dest = *callInstr.dest;
                            mov.args.push_back(cloned.args[0]);
                            clone->addInstruction(mov);
                        }
                        IRInstruction br;
                        br.op = IROp::BR;
                        br.args.push_back(IRValue::Label(afterName));
                        clone->addInstruction(br);
                    } else {
                        clone->addInstruction(cloned);
                    }
                }
                newBlocks.push_back(std::move(clone));
            }

            auto afterBlock = std::make_unique<BasicBlock>(afterName);
            afterBlock->getInstructions() = std::move(afterInsts);
            newBlocks.push_back(std::move(afterBlock));

            auto& blocks = caller.getBlocks();
            size_t numCloned = callee.getBlocks().size();
            blocks.insert(blocks.begin() + blockIdx + 1,
                          std::make_move_iterator(newBlocks.begin()),
                          std::make_move_iterator(newBlocks.end()));
            return numCloned;
        }
    };

} // namespace Cider

#endif
