#pragma once

#include <algorithm>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <stack>
#include <string>
#include <vector>

#include "ir.h"

namespace cider {

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
            {IROp::ALLOCA, 0}, {IROp::PUSH, 1}, {IROp::POP, 1},
            {IROp::CMP_EQ, 1}, {IROp::CMP_NE, 1}, {IROp::CMP_LT, 1},
            {IROp::CMP_GT, 1}, {IROp::CMP_LE, 1}, {IROp::CMP_GE, 1},
            {IROp::LOAD, 1}, {IROp::STORE, 1}, {IROp::MEMCPY, 4},
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
        if (nSites <= 0) return false;  // no gain without live call sites
        return (CALL_OVERHEAD * nSites) >= bodyCost;
    }
};

template <typename CostModel = HybridCycleCostModel>
class Inliner {
public:
    bool run(ModuleIR& module) {
        graph_.build(module);
        functionMap_.clear();
        for (auto& func : module.getFunctions()) {
            functionMap_[func->getName()] = func.get();
        }
        // CIDER-BUG-162: capture the static call-site counts exactly once,
        // before any inlining, and only consume them from then on. Later
        // fixpoint rounds run over IR that already contains clones of
        // previously inlined bodies; re-counting those clones would feed
        // compiler-generated sites back into the cost model's gain term
        // (`CALL_OVERHEAD * nSites`), unlocking ever-larger callees whose
        // bodies contain yet more calls. That multiplies the site count on
        // every round and grows the IR exponentially, which then swamps the
        // downstream passes (N call sites: ~5x compile time per +2 sites).
        // Tying the budget to the source program keeps total inlining work
        // proportional to the real call sites.
        if (!sitesCaptured_) {
            for (auto& func : module.getFunctions()) {
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
            sitesCaptured_ = true;
        }
        bool changed = false;
        for (auto& func : module.getFunctions()) {
            if (inlineFunction(*func)) changed = true;
        }
        return changed;
    }

private:
    CallGraph graph_;
    CostModel costModel_;
    int siteCounter_ = 0;
    bool sitesCaptured_ = false;
    std::map<std::string, FunctionIR*> functionMap_;
    std::map<std::string, int> remainingSites_;
    // CIDER-BUG-173: predecessor block name -> blocks with a PHI keyed by it.
    // BasicBlock objects are owned by unique_ptr and never freed by the
    // inliner, so raw pointers stay valid for the duration of one
    // inlineFunction() pass.
    std::map<std::string, std::vector<BasicBlock*>> phiPredIndex_;

    // Register every PHI predecessor key of `block`. Repeated keys for the
    // same block are collapsed (pushes for one block are contiguous).
    void indexPhiPredecessors(BasicBlock& block) {
        for (auto& ci : block.getInstructions()) {
            if (ci.op != IROp::PHI) continue;
            for (const auto& kv : ci.phiArgs) {
                auto& v = phiPredIndex_[kv.first];
                if (v.empty() || v.back() != &block) v.push_back(&block);
            }
        }
    }

    // CIDER-BUG-064: every PHI edge keyed `from` now leaves from `to`
    // instead. Only the indexed PHIs can match, so the rewrite is
    // proportional to the affected edges.
    void rewritePhiEdges(const std::string& from, const std::string& to) {
        auto predIt = phiPredIndex_.find(from);
        if (predIt == phiPredIndex_.end()) return;
        std::vector<BasicBlock*> affected = std::move(predIt->second);
        phiPredIndex_.erase(predIt);
        for (BasicBlock* b : affected) {
            for (auto& ci : b->getInstructions()) {
                if (ci.op != IROp::PHI) continue;
                auto phiIt = ci.phiArgs.find(from);
                if (phiIt == ci.phiArgs.end()) continue;
                IRValue incoming = phiIt->second;
                ci.phiArgs.erase(phiIt);
                ci.phiArgs[to] = incoming;
            }
            auto& v = phiPredIndex_[to];
            if (v.empty() || v.back() != b) v.push_back(b);
        }
    }

    // One accepted call site: where it is and the already-materialised clone
    // blocks that replace it.
    struct PlannedSite {
        size_t pos = 0;
        std::string afterName;
        std::string entryName;
        std::vector<std::unique_ptr<BasicBlock>> clones;
    };

    bool inlineFunction(FunctionIR& func) {
        bool changed = false;
        // CIDER-BUG-173: index which blocks hold a PHI keyed by each
        // predecessor. A split only has to rewrite the PHIs that list the
        // split block as an incoming edge; without this index every inline
        // site rescanned every block (and instruction) of the caller, which
        // is quadratic in the number of call sites on large straight-line
        // functions.
        phiPredIndex_.clear();
        for (auto& block : func.getBlocks()) indexPhiPredecessors(*block);
        for (size_t bi = 0; bi < func.getBlocks().size(); ++bi) {
            BasicBlock& block = *func.getBlocks()[bi];
            // Collect every call site of this block that the (fixed) budget
            // admits, in program order. Mutating the block per site used to
            // move the whole remaining tail each time; see splitAtSites.
            std::vector<PlannedSite> sites;
            const auto& insts = block.getInstructions();
            for (size_t ii = 0; ii < insts.size(); ++ii) {
                const IRInstruction& instr = insts[ii];
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
                sites.push_back(planSite(*callee, instr, ii));
                remainingSites_[calleeName]--;
            }
            if (sites.empty()) continue;
            changed = true;
            size_t inserted = splitAtSites(func, bi, block, sites);
            // CIDER-BUG-162: the blocks just created hold the call sites that
            // were handled above plus compiler-generated clones of them; the
            // budget does not re-count either, so skip them for this pass.
            bi += inserted;
        }
        return changed;
    }

    // Materialises the clone blocks for one call site without touching the
    // caller. `pos` is the call's index in the caller block; `entryName` and
    // `afterName` match the names the previous per-site splitter generated.
    PlannedSite planSite(FunctionIR& callee, const IRInstruction& callInstr, size_t pos) {
        PlannedSite site;
        site.pos = pos;
        std::string prefix = callee.getName() + "_" + std::to_string(siteCounter_++) + "_";
        site.afterName = prefix + "after";
        std::string entryName = callee.getBlocks().front()->getName();
        site.entryName = prefix + entryName;

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
                for (const auto& kv : ci.phiArgs) {
                    if (kv.second.kind == IRValue::REGISTER && paramMap.find(kv.second.name) == paramMap.end()) {
                        regRename[kv.second.name] = prefix + kv.second.name;
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
                        std::string newBlock = blockRename.at(kv.first);
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
                    br.args.push_back(IRValue::Label(site.afterName));
                    clone->addInstruction(br);
                } else {
                    clone->addInstruction(cloned);
                }
            }
            site.clones.push_back(std::move(clone));
        }
        return site;
    }

    // Splits `block` at every planned site and inserts, after it, each
    // site's clones followed by the `after` segment that holds the code past
    // the replaced call. Returns the number of inserted blocks.
    //
    // The segments are cut from the last site backwards: each cut moves only
    // the (small) segment after one call, and the final tail once, so the
    // whole block is split in linear time. Cutting forwards moved the entire
    // remaining tail per site, which is quadratic on large straight-line
    // blocks (CIDER-BUG-173).
    size_t splitAtSites(FunctionIR& func, size_t blockIdx, BasicBlock& block,
                        std::vector<PlannedSite>& sites) {
        // CIDER-BUG-064: the original terminator ends up in the last tail
        // segment, so every successor PHI that listed the split block as the
        // predecessor of that edge must key its incoming value on that
        // segment instead. Rewrite before the instructions move, while the
        // index's pointer to `block` is still accurate; the per-site chain
        // collapses to this one rewrite because the intermediate segment
        // names are fresh and cannot be referenced yet.
        const std::string splitBlockName = block.getName();
        const std::string& lastAfterName = sites.back().afterName;
        rewritePhiEdges(splitBlockName, lastAfterName);

        auto& insts = block.getInstructions();
        std::vector<std::unique_ptr<BasicBlock>> tails(sites.size());
        for (size_t j = sites.size(); j-- > 0;) {
            auto tail = std::make_unique<BasicBlock>(sites[j].afterName);
            auto& tailInsts = tail->getInstructions();
            tailInsts.reserve(insts.size() - sites[j].pos - 1);
            tailInsts.insert(tailInsts.end(),
                             std::make_move_iterator(insts.begin() + sites[j].pos + 1),
                             std::make_move_iterator(insts.end()));
            insts.resize(sites[j].pos);  // drop the call and the moved tail
            tails[j] = std::move(tail);
            IRInstruction br;
            br.op = IROp::BR;
            br.args.push_back(IRValue::Label(sites[j].entryName));
            insts.push_back(br);
        }

        std::vector<std::unique_ptr<BasicBlock>> newBlocks;
        for (size_t j = 0; j < sites.size(); ++j) {
            for (auto& clone : sites[j].clones) newBlocks.push_back(std::move(clone));
            newBlocks.push_back(std::move(tails[j]));
        }
        size_t inserted = newBlocks.size();
        auto& blocks = func.getBlocks();
        blocks.insert(blocks.begin() + blockIdx + 1,
                      std::make_move_iterator(newBlocks.begin()),
                      std::make_move_iterator(newBlocks.end()));
        // The new blocks can be split targets of later sites in this pass
        // (tails) or hold PHIs of their own (clones).
        for (size_t ni = blockIdx + 1; ni <= blockIdx + inserted; ++ni) {
            indexPhiPredecessors(*blocks[ni]);
        }
        return inserted;
    }
};

}  // namespace cider
