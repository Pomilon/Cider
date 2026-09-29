#pragma once

#include <algorithm>
#include <functional>
#include <iostream>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ir.h"

namespace cider {

struct LiveInterval {
    std::string varName;
    int start;
    int end;
    bool isFloat;

    bool operator<(const LiveInterval& other) const {
        return start < other.start;
    }
};

struct RegisterAllocation {
    bool isSpilled;
    std::string reg;
    int stackOffset;
};

typedef RegisterAllocation AllocationResult;

class RegisterAllocator {
public:
    std::map<std::string, AllocationResult> allocate(FunctionIR& func, int& finalStackSize) {
        std::map<std::string, AllocationResult> allocation;

        int numBlocks = (int)func.getBlocks().size();
        if (numBlocks == 0) {
            finalStackSize = 0;
            return allocation;
        }

        // Map block name to index. Duplicate names make edges ambiguous;
        // treat the function as malformed (conservative fallback below).
        // The first occurrence wins where baseline kept the last one: that
        // choice only decides which target a malformed edge records, and
        // duplicate names already force the conservative path, so no safe
        // register/slot decision depends on it.
        std::map<std::string, int> blockIndex;
        bool duplicateBlockName = false;
        for (int i = 0; i < numBlocks; i++) {
            if (!blockIndex.emplace(func.getBlocks()[i]->getName(), i).second) {
                duplicateBlockName = true;
            }
        }

        // Build successor lists. A block whose terminator cannot be fully
        // resolved (no terminator, or a branch to a label that is not in
        // the function) is malformed: its successors are unknown, so the
        // CFG analysis must not claim anything about values flowing
        // through it.
        std::vector<std::vector<int>> successors(numBlocks);
        std::vector<bool> malformed(numBlocks, duplicateBlockName);
        for (int b = 0; b < numBlocks; b++) {
            auto& instrs = func.getBlocks()[b]->getInstructions();
            if (instrs.empty() ||
                (instrs.back().op != IROp::BR && instrs.back().op != IROp::COND_BR &&
                 instrs.back().op != IROp::RET)) {
                malformed[b] = true;
            }
            for (auto& instr : instrs) {
                if (instr.op == IROp::BR) {
                    if (!instr.args.empty() && instr.args[0].kind == IRValue::LABEL) {
                        auto it = blockIndex.find(instr.args[0].name);
                        if (it != blockIndex.end()) {
                            successors[b].push_back(it->second);
                        } else {
                            malformed[b] = true;
                        }
                    } else {
                        malformed[b] = true;
                    }
                } else if (instr.op == IROp::COND_BR) {
                    if (instr.args.size() > 1 && instr.args[1].kind == IRValue::LABEL) {
                        auto it = blockIndex.find(instr.args[1].name);
                        if (it != blockIndex.end()) {
                            successors[b].push_back(it->second);
                        } else {
                            malformed[b] = true;
                        }
                    } else {
                        malformed[b] = true;
                    }
                    if (instr.args.size() > 2 && instr.args[2].kind == IRValue::LABEL) {
                        auto it = blockIndex.find(instr.args[2].name);
                        if (it != blockIndex.end()) {
                            successors[b].push_back(it->second);
                        } else {
                            malformed[b] = true;
                        }
                    } else {
                        malformed[b] = true;
                    }
                }
            }
        }

        // Track register types
        std::map<std::string, bool> isFloat;

        // Compute USE[b] and DEF[b]
        std::vector<std::set<std::string>> USE(numBlocks);
        std::vector<std::set<std::string>> DEF(numBlocks);

        // PHI edge uses (CID-019). A value consumed by a PHI in successor s
        // along the edge b -> s is read when the PHI runs (the VM looks up
        // phiArgs[previousBlock]), so it is live-out of b. Tracked per edge
        // because the same PHI takes a different value from each
        // predecessor. Without this, a value defined in b and used only by
        // a PHI in s would claim to die in b before the edge is taken.
        std::vector<std::map<int, std::set<std::string>>> PHI_EDGE_USE(numBlocks);

        for (int b = 0; b < numBlocks; b++) {
            std::set<std::string> definedInBlock;
            auto& instrs = func.getBlocks()[b]->getInstructions();
            for (auto& instr : instrs) {
                for (auto& arg : instr.args) {
                    if (arg.kind == IRValue::REGISTER) {
                        if (definedInBlock.find(arg.name) == definedInBlock.end()) {
                            USE[b].insert(arg.name);
                        }
                        isFloat[arg.name] = (arg.type == IRType::F32 || arg.type == IRType::F64);
                    }
                }
                for (auto& pair : instr.asmMap) {
                    if (pair.second.kind == IRValue::REGISTER) {
                        if (definedInBlock.find(pair.second.name) == definedInBlock.end()) {
                            USE[b].insert(pair.second.name);
                        }
                        isFloat[pair.second.name] = (pair.second.type == IRType::F32 || pair.second.type == IRType::F64);
                    }
                }
                for (auto& [phiBlock, val] : instr.phiArgs) {
                    if (val.kind == IRValue::REGISTER) {
                        if (definedInBlock.find(val.name) == definedInBlock.end()) {
                            USE[b].insert(val.name);
                        }
                        isFloat[val.name] = (val.type == IRType::F32 || val.type == IRType::F64);
                        auto pit = blockIndex.find(phiBlock);
                        if (pit != blockIndex.end()) {
                            PHI_EDGE_USE[pit->second][b].insert(val.name);
                        }
                    }
                }
                if (instr.dest && instr.dest->kind == IRValue::REGISTER) {
                    DEF[b].insert(instr.dest->name);
                    definedInBlock.insert(instr.dest->name);
                    isFloat[instr.dest->name] = (instr.dest->type == IRType::F32 || instr.dest->type == IRType::F64);
                }
            }
        }

        // Solve backward data-flow equations (CID-019). LIVE_OUT[b] is
        // every value live entering one of b's successors (plus the PHI
        // operands that successor reads along the edge), and LIVE_IN[b]
        // adds b's own upward-exposed uses, killed by b's definitions.
        std::vector<std::set<std::string>> LIVE_IN(numBlocks);
        std::vector<std::set<std::string>> LIVE_OUT(numBlocks);

        for (int b = 0; b < numBlocks; b++) {
            LIVE_IN[b] = USE[b];
        }

        bool changed = true;
        while (changed) {
            changed = false;
            // CIDER-BUG-166: liveness flows backward, so process blocks in
            // reverse order — a sweep then reads each block's already-final
            // successors. The old forward order needed one sweep per block
            // on straight-line code (each sweep moved liveness back a single
            // block: O(B^2) set merges); reverse order reaches the same least
            // fixed point in one pass plus the confirming pass for acyclic
            // CFGs. The fixed point, and therefore every register decision,
            // is unchanged.
            for (int b = numBlocks - 1; b >= 0; b--) {
                std::set<std::string> newOut;
                for (int s : successors[b]) {
                    newOut.insert(LIVE_IN[s].begin(), LIVE_IN[s].end());
                    auto edgeUse = PHI_EDGE_USE[b].find(s);
                    if (edgeUse != PHI_EDGE_USE[b].end()) {
                        newOut.insert(edgeUse->second.begin(), edgeUse->second.end());
                    }
                }
                if (newOut != LIVE_OUT[b]) {
                    changed = true;
                    LIVE_OUT[b] = newOut;
                }

                std::set<std::string> newIn = USE[b];
                for (auto& v : LIVE_OUT[b]) {
                    if (DEF[b].find(v) == DEF[b].end()) {
                        newIn.insert(v);
                    }
                }
                if (newIn != LIVE_IN[b]) {
                    changed = true;
                    LIVE_IN[b] = newIn;
                }
            }
        }

        // Register intervals. CID-019 deliberately keeps this scheme (and
        // every register decision the linear scan makes from it) unchanged;
        // spill slots use the CFG-liveness ranges computed below instead.
        // USE/DEF alone is NOT enough: a value used in a loop header is
        // re-used on every back edge, so it is live across the whole loop
        // body even though its USE/DEF blocks only span [preheader,
        // header]. Without extending those intervals the linear scan hands
        // the value's register to a body value, clobbering the loop-carried
        // operand. LIVE_IN/LIVE_OUT can't be added wholesale — they also
        // mark values that merely flow through a block boundary, which
        // would stretch intervals into blocks where the value is not used
        // (unnecessary spills). So only extend along back edges: every
        // value live into a back-edge target (loop header) must keep its
        // register through all blocks of the loop body.
        std::map<std::string, std::set<int>> blocksForReg;
        for (int b = 0; b < numBlocks; b++) {
            for (auto& v : USE[b]) blocksForReg[v].insert(b);
            for (auto& v : DEF[b]) blocksForReg[v].insert(b);
        }
        for (int src = 0; src < numBlocks; src++) {
            for (int tgt : successors[src]) {
                if (tgt < src) {  // back edge: src -> tgt
                    for (int t = tgt; t <= src; ++t) {
                        for (auto& v : LIVE_IN[tgt]) {
                            blocksForReg[v].insert(t);
                        }
                    }
                }
            }
        }

        std::map<std::string, LiveInterval> intervals;
        for (auto& pair : blocksForReg) {
            int firstBlock = *pair.second.begin();
            int lastBlock = *pair.second.rbegin();
            intervals[pair.first] = {pair.first, firstBlock, lastBlock, isFloat[pair.first]};
        }

        // Extend LOAD address register intervals
        for (int b = 0; b < numBlocks; b++) {
            auto& instrs = func.getBlocks()[b]->getInstructions();
            for (auto& instr : instrs) {
                if (instr.op == IROp::LOAD && !instr.args.empty() && instr.args[0].kind == IRValue::REGISTER) {
                    std::string addrReg = instr.args[0].name;
                    auto it = intervals.find(addrReg);
                    if (it != intervals.end()) {
                        it->second.end = std::max(it->second.end, std::min(b + 1, numBlocks - 1));
                    }
                }
            }
        }

        // Slot live ranges (CID-019). Unlike the register intervals above,
        // these come straight from the CFG data-flow equations: a value is
        // live in any block where it is used, defined, live-in or live-out.
        // [first live block, last live block] over-approximates the exact
        // live set (the span covers numeric gaps), which is safe for slot
        // reuse: two ranges that do not overlap at block granularity can
        // never be live at the same time, so they may share a slot. The
        // block gap only costs reuse opportunities, never correctness.
        std::map<std::string, LiveInterval> slotRanges;
        {
            std::map<std::string, std::set<int>> liveBlocks;
            for (int b = 0; b < numBlocks; b++) {
                for (auto& v : USE[b]) liveBlocks[v].insert(b);
                for (auto& v : DEF[b]) liveBlocks[v].insert(b);
                for (auto& v : LIVE_IN[b]) liveBlocks[v].insert(b);
                for (auto& v : LIVE_OUT[b]) liveBlocks[v].insert(b);
            }
            for (auto& pair : liveBlocks) {
                int firstBlock = *pair.second.begin();
                int lastBlock = *pair.second.rbegin();
                slotRanges[pair.first] = {pair.first, firstBlock, lastBlock, isFloat[pair.first]};
            }
        }

        // Conservative fallback (Issue #10). Reachability from the entry
        // and terminator well-formedness are preconditions for trusting
        // the equations; blocks that fail them (dead code, branches with
        // no target, calls that do not return) may still be executed in
        // ways the CFG does not show, so values appearing in them keep a
        // fresh, never-recycled slot exactly as before CID-019. If a
        // *reachable* block is malformed its unknown successors can also
        // be reached at run time, so reuse is disabled for the whole
        // function; an unreachable malformed block (dead code) only makes
        // the values that appear in it conservative.
        std::vector<bool> reachable(numBlocks, false);
        {
            std::vector<int> work;
            work.push_back(0);
            reachable[0] = true;
            while (!work.empty()) {
                int b = work.back();
                work.pop_back();
                for (int s : successors[b]) {
                    if (!reachable[s]) {
                        reachable[s] = true;
                        work.push_back(s);
                    }
                }
            }
        }
        bool reachableMalformed = false;
        for (int b = 0; b < numBlocks; b++) {
            if (reachable[b] && malformed[b]) reachableMalformed = true;
        }
        std::set<std::string> conservativeValues;
        for (int b = 0; b < numBlocks; b++) {
            if (reachable[b] && !malformed[b]) continue;
            for (auto& instr : func.getBlocks()[b]->getInstructions()) {
                for (auto& arg : instr.args) {
                    if (arg.kind == IRValue::REGISTER) conservativeValues.insert(arg.name);
                }
                for (auto& pair : instr.asmMap) {
                    if (pair.second.kind == IRValue::REGISTER) conservativeValues.insert(pair.second.name);
                }
                for (auto& [phiBlock, val] : instr.phiArgs) {
                    if (val.kind == IRValue::REGISTER) conservativeValues.insert(val.name);
                }
                if (instr.dest && instr.dest->kind == IRValue::REGISTER) {
                    conservativeValues.insert(instr.dest->name);
                }
            }
        }

        // Linear scan using block indices. Register decisions are the same
        // as before CID-019 (same intervals, same order, same register
        // files); spilled values leave their slot assignment to the pass
        // below.
        std::vector<LiveInterval> unhandled;
        for (auto& pair : intervals) {
            unhandled.push_back(pair.second);
        }
        std::sort(unhandled.begin(), unhandled.end());

        std::vector<std::string> freeIntRegs = {"rbx", "r12", "r13", "r14", "r15"};
        std::vector<std::string> freeFloatRegs = {"xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"};

        std::vector<LiveInterval> active;
        for (auto& interval : unhandled) {
            for (auto it = active.begin(); it != active.end();) {
                if (it->end < interval.start) {
                    auto& res = allocation[it->varName];
                    if (!res.isSpilled) {
                        if (it->isFloat) freeFloatRegs.push_back(res.reg);
                        else freeIntRegs.push_back(res.reg);
                    }
                    it = active.erase(it);
                } else {
                    ++it;
                }
            }

            auto& freeRegs = interval.isFloat ? freeFloatRegs : freeIntRegs;
            if (freeRegs.empty()) {
                // Slot assigned by the reuse pass below.
                allocation[interval.varName] = {true, "", 0};
                active.push_back(interval);
            } else {
                std::string reg = freeRegs.back();
                freeRegs.pop_back();
                allocation[interval.varName] = {false, reg, 0};
                active.push_back(interval);
            }
        }

        // Spill-slot assignment (CID-019). Slots are 8 bytes each. A slot
        // is recycled for a new value when every value already assigned to
        // it is dead before the new value is born (previous range end <
        // new range start) — the disjointness rule. Ranges are processed
        // by start, so a min-heap keyed by end finds every recyclable slot.
        // Conservative values bypass the pool: fresh slot, never shared.
        int slotsUsed = 0;
        {
            std::vector<LiveInterval> spillIntervals;
            std::vector<std::string> conservativeSpills;
            for (auto& [name, res] : allocation) {
                if (!res.isSpilled) continue;
                auto it = slotRanges.find(name);
                if (reachableMalformed || conservativeValues.count(name) || it == slotRanges.end()) {
                    conservativeSpills.push_back(name);
                } else {
                    spillIntervals.push_back(it->second);
                }
            }
            std::sort(spillIntervals.begin(), spillIntervals.end(),
                      [](const LiveInterval& a, const LiveInterval& b) {
                          if (a.start != b.start) return a.start < b.start;
                          return a.end < b.end;
                      });

            typedef std::pair<int, int> SlotEnd;  // (range end, slot index)
            std::priority_queue<SlotEnd, std::vector<SlotEnd>, std::greater<SlotEnd>> inUse;
            std::vector<int> freeSlots;
            for (auto& interval : spillIntervals) {
                while (!inUse.empty() && inUse.top().first < interval.start) {
                    freeSlots.push_back(inUse.top().second);
                    inUse.pop();
                }
                int slot;
                if (!freeSlots.empty()) {
                    slot = freeSlots.back();
                    freeSlots.pop_back();
                } else {
                    slot = ++slotsUsed;
                }
                allocation[interval.varName].stackOffset = -8 * slot;
                inUse.push({interval.end, slot});
            }
            for (auto& name : conservativeSpills) {
                allocation[name].stackOffset = -8 * (++slotsUsed);
            }
        }

        finalStackSize = slotsUsed * 8;
        return allocation;
    }
};

}  // namespace cider
