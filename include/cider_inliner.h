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
            callCounts_.clear();
            containsIndirectCall_.clear();

            for (const auto& func : module.getFunctions()) {
                const std::string& name = func->getName();
                edges_[name] = {};
                for (const auto& block : func->getBlocks()) {
                    for (const auto& instr : block->getInstructions()) {
                        if ((instr.op == IROp::CALL || instr.op == IROp::VIRTUAL_CALL) && !instr.args.empty()) {
                            if (instr.args[0].kind == IRValue::LABEL) {
                                edges_[name].push_back(instr.args[0].name);
                                callCounts_[instr.args[0].name]++;
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

        int callSites(const std::string& callee) const {
            auto it = callCounts_.find(callee);
            return it != callCounts_.end() ? it->second : 0;
        }

        bool containsIndirectCall(const std::string& func) const {
            return containsIndirectCall_.find(func) != containsIndirectCall_.end();
        }

    private:
        std::map<std::string, std::vector<std::string>> edges_;
        std::map<std::string, int> sccId_;
        std::map<std::string, int> callCounts_;
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
        static constexpr int CALL_OVERHEAD = 5;  // cycles saved per eliminated call site
        static constexpr int BLOAT_CAP = 40;     // hard instruction cap regardless of cycle math

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

} // namespace Cider

#endif
