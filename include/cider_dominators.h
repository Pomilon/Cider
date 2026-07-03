#ifndef CIDER_DOMINATORS_H
#define CIDER_DOMINATORS_H

#include "cider_ir.h"
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace Cider {

class DominatorTree {
public:
    void build(const FunctionIR& func);

    bool dominates(const std::string& a, const std::string& b) const {
        if (a == b) return true;
        auto itA = depth_.find(a);
        auto itB = depth_.find(b);
        if (itA == depth_.end() || itB == depth_.end()) return false;
        if (itA->second > itB->second) return false;
        std::string cur = b;
        while (cur != a) {
            auto it = idom_.find(cur);
            if (it == idom_.end()) return false;
            cur = it->second;
        }
        return true;
    }

    bool strictlyDominates(const std::string& a, const std::string& b) const {
        return a != b && dominates(a, b);
    }

    const std::string& idom(const std::string& block) const {
        return idom_.at(block);
    }

private:
    std::map<std::string, std::string> idom_;   // block → immediate dominator
    std::map<std::string, int> depth_;           // depth in dom tree (entry = 0)

    struct CFG {
        std::vector<std::vector<int>> preds;
        std::vector<std::vector<int>> succs;
        int entry;
        std::vector<std::string> blockNames;
    };
    CFG buildCFG(const FunctionIR& func) const;

    void dfs(int v, const std::vector<std::vector<int>>& succs,
             std::vector<int>& dfn, std::vector<int>& vertex,
             std::vector<int>& parent, int& count);
    int eval(int v, const std::vector<int>& dfn,
             const std::vector<int>& semi, std::vector<int>& label,
             std::vector<int>& ancestor) const;
    void compress(int v, const std::vector<int>& dfn,
                  const std::vector<int>& semi, std::vector<int>& label,
                  std::vector<int>& ancestor) const;
};

// --- DominatorTree implementation ---

DominatorTree::CFG DominatorTree::buildCFG(const FunctionIR& func) const {
    const auto& blocks = func.getBlocks();
    CFG cfg;
    cfg.blockNames.reserve(blocks.size());
    std::map<std::string, int> nameToIdx;

    for (size_t i = 0; i < blocks.size(); ++i) {
        cfg.blockNames.push_back(blocks[i]->getName());
        nameToIdx[blocks[i]->getName()] = i;
    }

    int n = blocks.size();
    cfg.preds.resize(n);
    cfg.succs.resize(n);
    cfg.entry = nameToIdx[blocks.front()->getName()];

    for (size_t i = 0; i < blocks.size(); ++i) {
        int u = i;
        const auto& instrs = blocks[i]->getInstructions();
        if (instrs.empty()) continue;

        const auto& term = instrs.back();
        if (term.op == IROp::BR && !term.args.empty() && term.args[0].kind == IRValue::LABEL) {
            int v = nameToIdx[term.args[0].name];
            cfg.succs[u].push_back(v);
            cfg.preds[v].push_back(u);
        } else if (term.op == IROp::COND_BR && term.args.size() >= 3) {
            int v1 = nameToIdx[term.args[1].name];
            int v2 = nameToIdx[term.args[2].name];
            cfg.succs[u].push_back(v1); cfg.succs[u].push_back(v2);
            cfg.preds[v1].push_back(u); cfg.preds[v2].push_back(u);
        }
    }
    return cfg;
}

void DominatorTree::dfs(int v, const std::vector<std::vector<int>>& succs,
                        std::vector<int>& dfn, std::vector<int>& vertex,
                        std::vector<int>& parent, int& count) {
    dfn[v] = count;
    vertex[count] = v;
    count++;
    for (int w : succs[v]) {
        if (dfn[w] == -1) {
            parent[w] = v;
            dfs(w, succs, dfn, vertex, parent, count);
        }
    }
}

int DominatorTree::eval(int v, const std::vector<int>& dfn,
                        const std::vector<int>& semi,
                        std::vector<int>& label,
                        std::vector<int>& ancestor) const {
    if (ancestor[v] == -1) return v;
    compress(v, dfn, semi, label, ancestor);
    if (dfn[semi[label[ancestor[v]]]] < dfn[semi[label[v]]]) {
        return label[ancestor[v]];
    } else {
        return label[v];
    }
}

void DominatorTree::compress(int v, const std::vector<int>& dfn,
                             const std::vector<int>& semi,
                             std::vector<int>& label,
                             std::vector<int>& ancestor) const {
    if (ancestor[ancestor[v]] != -1) {
        compress(ancestor[v], dfn, semi, label, ancestor);
        if (dfn[semi[label[ancestor[v]]]] < dfn[semi[label[v]]]) {
            label[v] = label[ancestor[v]];
        }
        ancestor[v] = ancestor[ancestor[v]];
    }
}

void DominatorTree::build(const FunctionIR& func) {
    idom_.clear();
    depth_.clear();

    if (func.getBlocks().empty()) return;

    CFG cfg = buildCFG(func);
    int n = cfg.blockNames.size();

    std::vector<int> dfn(n, -1), vertex(n), parent(n, -1);
    std::vector<int> semi(n), label(n), ancestor(n, -1);
    std::vector<int> idomIdx(n, -1);
    std::vector<std::vector<int>> bucket(n);

    for (int i = 0; i < n; ++i) {
        label[i] = i;
        semi[i] = i;
    }

    int dfsCount = 0;
    dfs(cfg.entry, cfg.succs, dfn, vertex, parent, dfsCount);

    if (dfsCount == 0) return;

    for (int i = dfsCount - 1; i > 0; --i) {
        int w = vertex[i];
        for (int v : cfg.preds[w]) {
            if (dfn[v] == -1) continue;
            int u = eval(v, dfn, semi, label, ancestor);
            if (dfn[semi[u]] < dfn[semi[w]]) {
                semi[w] = semi[u];
            }
        }
        bucket[vertex[semi[w]]].push_back(w);

        int p = parent[w];
        if (p == -1) continue;
        ancestor[w] = p;

        for (int v : bucket[p]) {
            int u = eval(v, dfn, semi, label, ancestor);
            if (dfn[semi[u]] < dfn[semi[v]]) {
                idomIdx[v] = u;
            } else {
                idomIdx[v] = p;
            }
        }
        bucket[p].clear();
    }

    for (int i = 1; i < dfsCount; ++i) {
        int w = vertex[i];
        if (idomIdx[w] != vertex[semi[w]]) {
            idomIdx[w] = idomIdx[idomIdx[w]];
        }
    }
    idomIdx[cfg.entry] = cfg.entry;

    for (int i = 0; i < n; ++i) {
        if (dfn[i] == -1) continue;
        const std::string& name = cfg.blockNames[i];
        if (i == cfg.entry) {
            idom_[name] = name;
            depth_[name] = 0;
        } else {
            const std::string& parentName = cfg.blockNames[idomIdx[i]];
            idom_[name] = parentName;
            depth_[name] = depth_.at(parentName) + 1;
        }
    }
}

} // namespace Cider

#endif
