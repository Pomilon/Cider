#pragma once

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ir.h"

namespace cider {

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
            if (it == idom_.end() || it->second == cur) return false;
            cur = it->second;
        }
        return true;
    }

    bool strictlyDominates(const std::string& a, const std::string& b) const {
        return a != b && dominates(a, b);
    }

    // CIDER-BUG-259: total lookup. A block the tree knows nothing about — one
    // unreachable from the entry, or one whose dominator the analysis could
    // not establish — reports the empty name instead of throwing
    // std::out_of_range out of the optimizer. Every caller only ever compares
    // the result against a block name, and "dominates nothing" is the answer
    // a block outside the tree deserves.
    const std::string& idom(const std::string& block) const {
        static const std::string kNoDominator;
        auto it = idom_.find(block);
        return it == idom_.end() ? kNoDominator : it->second;
    }

private:
    std::map<std::string, std::string> idom_;  // block → immediate dominator
    std::map<std::string, int> depth_;         // depth in dom tree (entry = 0)

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

    // CIDER-BUG-259: block names are the CFG's only keys, so a duplicated
    // name makes an edge ambiguous. The first occurrence wins (the same
    // choice RegisterAllocator::allocate makes); a function with duplicate
    // block names is malformed either way, and neither analysis can say
    // anything trustworthy about it.
    for (size_t i = 0; i < blocks.size(); ++i) {
        cfg.blockNames.push_back(blocks[i]->getName());
        nameToIdx.emplace(blocks[i]->getName(), (int)i);
    }

    int n = (int)blocks.size();
    cfg.preds.resize(n);
    cfg.succs.resize(n);
    cfg.entry = nameToIdx[blocks.front()->getName()];

    for (size_t i = 0; i < blocks.size(); ++i) {
        int u = (int)i;
        const auto& instrs = blocks[i]->getInstructions();
        if (instrs.empty()) continue;

        // CIDER-BUG-259: an edge is recorded only for a terminator that
        // names a block of THIS function. A BR/COND_BR whose operand is not
        // a label, or whose target is not a block here, is malformed IR —
        // the edge is then unknown, and an unknown edge is left out instead
        // of throwing std::map::at out of the optimizer (the same
        // conservative rule RegisterAllocator::allocate applies to a
        // malformed terminator).
        auto addEdge = [&](const IRValue& target) {
            if (target.kind != IRValue::LABEL) return;
            auto it = nameToIdx.find(target.name);
            if (it == nameToIdx.end()) return;
            int v = it->second;
            cfg.succs[u].push_back(v);
            cfg.preds[v].push_back(u);
        };

        const auto& term = instrs.back();
        if (term.op == IROp::BR) {
            if (!term.args.empty()) addEdge(term.args[0]);
        } else if (term.op == IROp::COND_BR && term.args.size() >= 3) {
            addEdge(term.args[1]);
            addEdge(term.args[2]);
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
    int n = (int)cfg.blockNames.size();

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
        bucket[semi[w]].push_back(w);

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
        if (idomIdx[w] != semi[w]) {
            idomIdx[w] = idomIdx[idomIdx[w]];
        }
    }
    idomIdx[cfg.entry] = cfg.entry;

    // CIDER-BUG-259: record the dominator TREE by walking it from the entry,
    // so a block's parent is always filled before the block itself. The old
    // code filled idom_/depth_ by walking the blocks in LAYOUT-INDEX order
    // and reading the parent's depth with std::map::at, so it silently
    // required every immediate dominator to have a LOWER block index than
    // the block it dominates. Nothing in the IR requires that ordering — the
    // landing-pad pass's first block layout (see ir_gen.h) violated it, and a
    // legal CFG like `entry -> header; header -> body` with `body` numbered
    // before `header` threw a bare std::out_of_range out of the optimizer.
    // Depth is now read off the tree, so the result depends only on the CFG.
    std::vector<std::vector<int>> domChildren(n);
    for (int i = 0; i < n; ++i) {
        if (dfn[i] == -1 || i == cfg.entry) continue;
        int p = idomIdx[i];
        // A self-parent or a parent the DFS never reached is not a tree edge:
        // such a block is left out of the maps entirely, so dominates()
        // answers "no" for it and idom() reports no dominator — both safe
        // answers, and the only ones the tree does not support.
        if (p < 0 || p == i || dfn[p] == -1) continue;
        domChildren[p].push_back(i);
    }

    std::vector<int> depthIdx(n, -1);
    const std::string& entryName = cfg.blockNames[cfg.entry];
    idom_[entryName] = entryName;
    depthIdx[cfg.entry] = 0;
    std::vector<int> stack;
    stack.push_back(cfg.entry);
    while (!stack.empty()) {
        int v = stack.back();
        stack.pop_back();
        for (int c : domChildren[v]) {
            idom_[cfg.blockNames[c]] = cfg.blockNames[v];
            depthIdx[c] = depthIdx[v] + 1;
            stack.push_back(c);
        }
    }
    for (int i = 0; i < n; ++i) {
        if (depthIdx[i] < 0) continue;
        depth_[cfg.blockNames[i]] = depthIdx[i];
    }
}

// --- Loop detection ---

struct Loop {
    std::string header;
    std::set<std::string> blocks;
    std::vector<Loop*> children;
    std::vector<std::string> exits;
    int depth = 0;
};

class NaturalLoopFinder {
public:
    void build(const FunctionIR& func, const DominatorTree& dt);

    const std::vector<Loop>& getLoops() const { return loops_; }
    const Loop* loopForHeader(const std::string& header) const;

    static std::string insertPreheader(FunctionIR& func, const Loop& loop,
                                       const DominatorTree& dt);

private:
    std::vector<Loop> loops_;
    std::map<std::string, int> headerToIdx_;

    std::set<std::string> findBody(const std::string& header,
                                   const std::string& latch,
                                   const FunctionIR& func,
                                   const DominatorTree& /*dt*/) const;
    void buildNesting();
    void computeExits(const FunctionIR& func);
    void computeDepths();
};

// --- NaturalLoopFinder implementation ---

std::set<std::string> NaturalLoopFinder::findBody(
    const std::string& header, const std::string& latch,
    const FunctionIR& func, const DominatorTree& /*dt*/) const {
    // Build predecessor map once (Issue #15): the old code scanned every
    // block for every worklist item — O(N^2) per back edge. With the map,
    // each edge is visited once.
    std::map<std::string, std::vector<std::string>> preds;
    for (const auto& b : func.getBlocks()) {
        const auto& instrs = b->getInstructions();
        if (instrs.empty()) continue;
        const auto& term = instrs.back();
        auto addPred = [&](const std::string& target) {
            preds[target].push_back(b->getName());
        };
        if (term.op == IROp::BR && !term.args.empty() && term.args[0].kind == IRValue::LABEL) {
            addPred(term.args[0].name);
        } else if (term.op == IROp::COND_BR && term.args.size() >= 3) {
            if (term.args[1].kind == IRValue::LABEL) addPred(term.args[1].name);
            if (term.args[2].kind == IRValue::LABEL) addPred(term.args[2].name);
        }
    }
    std::set<std::string> body;
    body.insert(header);
    std::vector<std::string> worklist;
    worklist.push_back(latch);
    while (!worklist.empty()) {
        std::string v = worklist.back();
        worklist.pop_back();
        if (body.find(v) != body.end()) continue;
        body.insert(v);
        auto it = preds.find(v);
        if (it == preds.end()) continue;
        for (const auto& p : it->second) {
            if (body.find(p) == body.end()) worklist.push_back(p);
        }
    }
    return body;
}

void NaturalLoopFinder::build(const FunctionIR& func, const DominatorTree& dt) {
    loops_.clear();
    headerToIdx_.clear();

    std::map<std::string, std::set<std::string>> successors;
    for (const auto& b : func.getBlocks()) {
        const auto& instrs = b->getInstructions();
        if (instrs.empty()) continue;
        const auto& term = instrs.back();
        if (term.op == IROp::BR && !term.args.empty() && term.args[0].kind == IRValue::LABEL) {
            successors[b->getName()].insert(term.args[0].name);
        } else if (term.op == IROp::COND_BR && term.args.size() >= 3) {
            if (term.args[1].kind == IRValue::LABEL) successors[b->getName()].insert(term.args[1].name);
            if (term.args[2].kind == IRValue::LABEL) successors[b->getName()].insert(term.args[2].name);
        }
    }

    for (const auto& [from, toSet] : successors) {
        for (const std::string& to : toSet) {
            if (dt.dominates(to, from)) {
                auto it = headerToIdx_.find(to);
                if (it != headerToIdx_.end()) {
                    auto additional = findBody(to, from, func, dt);
                    loops_[it->second].blocks.insert(additional.begin(), additional.end());
                } else {
                    Loop loop;
                    loop.header = to;
                    loop.blocks = findBody(to, from, func, dt);
                    headerToIdx_[to] = (int)loops_.size();
                    loops_.push_back(std::move(loop));
                }
            }
        }
    }

    buildNesting();
    computeExits(func);
    computeDepths();
}

void NaturalLoopFinder::buildNesting() {
    for (size_t i = 0; i < loops_.size(); ++i) {
        for (size_t j = 0; j < loops_.size(); ++j) {
            if (i == j) continue;
            if (loops_[i].blocks.size() > loops_[j].blocks.size()
                && loops_[i].blocks.find(loops_[j].header) != loops_[i].blocks.end()) {
                loops_[i].children.push_back(&loops_[j]);
            }
        }
    }
    for (auto& loop : loops_) {
        for (auto it = loop.children.begin(); it != loop.children.end();) {
            bool isTransitive = false;
            for (auto* other : loop.children) {
                if (other != *it && other->blocks.find((*it)->header) != other->blocks.end()) {
                    isTransitive = true;
                    break;
                }
            }
            if (isTransitive) it = loop.children.erase(it);
            else ++it;
        }
    }
}

void NaturalLoopFinder::computeExits(const FunctionIR& func) {
    for (auto& loop : loops_) {
        loop.exits.clear();
        for (const auto& b : func.getBlocks()) {
            if (loop.blocks.find(b->getName()) == loop.blocks.end()) continue;
            const auto& instrs = b->getInstructions();
            if (instrs.empty()) continue;
            const auto& term = instrs.back();
            auto addExitIfOutside = [&](const std::string& target) {
                if (loop.blocks.find(target) == loop.blocks.end()) {
                    loop.exits.push_back(target);
                }
            };
            // CIDER-BUG-259: the BR arm read args[0] without checking that
            // the terminator has any operand at all (the COND_BR arm did) —
            // a BR with no operand read past the end of the vector.
            if (term.op == IROp::BR && !term.args.empty() &&
                term.args[0].kind == IRValue::LABEL) {
                addExitIfOutside(term.args[0].name);
            } else if (term.op == IROp::COND_BR && term.args.size() >= 3) {
                if (term.args[1].kind == IRValue::LABEL) addExitIfOutside(term.args[1].name);
                if (term.args[2].kind == IRValue::LABEL) addExitIfOutside(term.args[2].name);
            }
        }
    }
}

void NaturalLoopFinder::computeDepths() {
    for (auto& loop : loops_) loop.depth = 0;

    std::set<Loop*> isChild;
    for (auto& l : loops_) {
        for (auto* child : l.children) {
            isChild.insert(child);
        }
    }

    std::function<void(Loop&, int)> setDepth;
    setDepth = [&](Loop& l, int d) {
        l.depth = d;
        for (auto* child : l.children) {
            setDepth(*child, d + 1);
        }
    };

    for (auto& l : loops_) {
        if (isChild.find(&l) == isChild.end()) {
            setDepth(l, 0);
        }
    }
}

const Loop* NaturalLoopFinder::loopForHeader(const std::string& header) const {
    auto it = headerToIdx_.find(header);
    if (it != headerToIdx_.end()) return &loops_[it->second];
    return nullptr;
}

std::string NaturalLoopFinder::insertPreheader(FunctionIR& func, const Loop& loop,
                                                const DominatorTree& /*dt*/) {
    std::string preheaderName = loop.header + "_preheader";

    for (const auto& b : func.getBlocks()) {
        if (b->getName() == preheaderName) return preheaderName;
    }

    std::vector<std::string> outsidePreds;
    for (const auto& b : func.getBlocks()) {
        if (b->getName() == loop.header) continue;
        const auto& instrs = b->getInstructions();
        if (instrs.empty()) continue;
        const auto& term = instrs.back();
        auto targetsHeader = [&](const IRInstruction& t) -> bool {
            if (t.op == IROp::BR && !t.args.empty() && t.args[0].kind == IRValue::LABEL
                && t.args[0].name == loop.header) return true;
            if (t.op == IROp::COND_BR && t.args.size() >= 3) {
                if (t.args[1].kind == IRValue::LABEL && t.args[1].name == loop.header) return true;
                if (t.args[2].kind == IRValue::LABEL && t.args[2].name == loop.header) return true;
            }
            return false;
        };
        if (targetsHeader(term) && loop.blocks.find(b->getName()) == loop.blocks.end()) {
            outsidePreds.push_back(b->getName());
        }
    }

    if (outsidePreds.empty()) return "";

    auto& blocks = func.getBlocks();

    struct PHIMerge {
        std::map<std::string, IRValue> entries;
        std::string tempReg;
        IRType valType;
    };
    std::vector<PHIMerge> merges;
    for (auto& b : func.getBlocks()) {
        if (b->getName() != loop.header) continue;
        for (auto& instr : b->getInstructions()) {
            if (instr.op == IROp::PHI) {
                PHIMerge merge;
                for (const auto& [pred, val] : instr.phiArgs) {
                    if (loop.blocks.find(pred) == loop.blocks.end()) {
                        merge.entries[pred] = val;
                    }
                }
                if (!merge.entries.empty()) {
                    merge.valType = merge.entries.begin()->second.type;
                    static int phiCounter = 0;
                    merge.tempReg = "phi_merge_" + loop.header + "_" + std::to_string(phiCounter++);
                    merges.push_back(std::move(merge));
                }
            }
        }
        break;
    }

    auto preheader = std::make_unique<BasicBlock>(preheaderName);
    for (auto& merge : merges) {
        IRInstruction phi;
        phi.op = IROp::PHI;
        phi.dest = IRValue::Register(merge.tempReg, merge.valType);
        phi.phiArgs = merge.entries;
        preheader->addInstruction(phi);
    }
    IRInstruction br;
    br.op = IROp::BR;
    br.args.push_back(IRValue::Label(loop.header));
    preheader->addInstruction(br);

    auto headerIt = std::find_if(blocks.begin(), blocks.end(),
                                 [&](const auto& b) { return b->getName() == loop.header; });
    blocks.insert(headerIt, std::move(preheader));

    for (auto& b : func.getBlocks()) {
        if (std::find(outsidePreds.begin(), outsidePreds.end(), b->getName()) == outsidePreds.end())
            continue;
        for (auto& instr : b->getInstructions()) {
            auto retarget = [&](IRValue& arg) {
                if (arg.kind == IRValue::LABEL && arg.name == loop.header) {
                    arg.name = preheaderName;
                }
            };
            if (instr.op == IROp::BR && !instr.args.empty()) {
                retarget(instr.args[0]);
            } else if (instr.op == IROp::COND_BR && instr.args.size() >= 3) {
                retarget(instr.args[1]);
                retarget(instr.args[2]);
            }
        }
    }

    headerIt = std::find_if(blocks.begin(), blocks.end(),
                            [&](const auto& b) { return b->getName() == loop.header; });
    size_t mergeIdx = 0;
    for (auto& instr : (*headerIt)->getInstructions()) {
        if (instr.op == IROp::PHI && mergeIdx < merges.size()) {
            for (const auto& [pred, _] : merges[mergeIdx].entries) {
                instr.phiArgs.erase(pred);
            }
            instr.phiArgs[preheaderName] = IRValue::Register(merges[mergeIdx].tempReg, merges[mergeIdx].valType);
            ++mergeIdx;
        }
    }

    return preheaderName;
}

}  // namespace cider
