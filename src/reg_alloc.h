#pragma once

#include <algorithm>
#include <cstdint>
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

// CIDER-BUG-172: compact id-numbered liveness sets. Liveness used to be
// materialised as std::set<std::string> per block, so every union in the
// fixpoint and every per-value block history paid a string comparison and
// a set-node allocation: allocate() was O(blocks x live values) with a
// large constant (2.3 s of codegen at N=400 on the destructor chain).
// Values are now numbered once per function; a block's live set is a
// fixed-width vector of uint64 words and union/difference are word
// operations. The data-flow equations (CID-019, CIDER-BUG-166) are
// untouched and the least fixed point is unique, so the live sets — and
// every register/slot decision derived from them — are exactly the ones
// the string sets produced (byte-identical asm).
struct LiveBits {
    std::vector<uint64_t> words;

    void reset(size_t numBits) { words.assign((numBits + 63) / 64, 0); }
    void set(size_t bit) { words[bit >> 6] |= (uint64_t(1) << (bit & 63)); }
};

// Visit every set bit in ascending order. The consumers below only keep
// per-value first/last block numbers, so the visit order cannot change a
// decision; ascending order keeps the pass deterministic and easy to
// compare with the old per-value std::set<int> histories.
template <typename F>
inline void forEachSetBit(const std::vector<uint64_t>& words, F&& f) {
    for (size_t w = 0; w < words.size(); w++) {
        uint64_t bits = words[w];
        while (bits) {
            unsigned bit = (unsigned)__builtin_ctzll(bits);
            f((int)(w * 64 + bit));
            bits &= bits - 1;
        }
    }
}

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

        // CIDER-BUG-172: number every register value once, in scan order.
        // The ids are only a key for the compact live sets; every decision
        // below is still keyed and ordered by value name, as before.
        // `isFloat` keeps the old last-occurrence-wins typing because the
        // scan visits args, asmMap, phiArgs and dest in the same order the
        // string-keyed map was written in.
        std::map<std::string, int> valueId;
        std::vector<std::string> valueNames;
        std::vector<char> isFloat;
        for (int b = 0; b < numBlocks; b++) {
            auto& instrs = func.getBlocks()[b]->getInstructions();
            for (auto& instr : instrs) {
                auto note = [&](const IRValue& v) {
                    if (v.kind != IRValue::REGISTER) return;
                    auto ins = valueId.emplace(v.name, (int)valueNames.size());
                    if (ins.second) {
                        valueNames.push_back(v.name);
                        isFloat.push_back(0);
                    }
                    isFloat[ins.first->second] = (v.type == IRType::F32 || v.type == IRType::F64);
                };
                for (auto& arg : instr.args) note(arg);
                for (auto& pair : instr.asmMap) note(pair.second);
                for (auto& kv : instr.phiArgs) note(kv.second);
                if (instr.dest) note(*instr.dest);
            }
        }
        const int numValues = (int)valueNames.size();
        const size_t numWords = ((size_t)numValues + 63) / 64;

        // Compute USE[b] and DEF[b]
        std::vector<LiveBits> USE(numBlocks);
        std::vector<LiveBits> DEF(numBlocks);
        for (int b = 0; b < numBlocks; b++) {
            USE[b].reset(numValues);
            DEF[b].reset(numValues);
        }

        // PHI edge uses (CID-019). A value consumed by a PHI in successor s
        // along the edge b -> s is read when the PHI runs (the VM looks up
        // phiArgs[previousBlock]), so it is live-out of b. Tracked per edge
        // because the same PHI takes a different value from each
        // predecessor. Without this, a value defined in b and used only by
        // a PHI in s would claim to die in b before the edge is taken.
        std::vector<std::map<int, LiveBits>> PHI_EDGE_USE(numBlocks);

        std::vector<int> definedInBlock(numValues, -1);
        for (int b = 0; b < numBlocks; b++) {
            auto& instrs = func.getBlocks()[b]->getInstructions();
            for (auto& instr : instrs) {
                for (auto& arg : instr.args) {
                    if (arg.kind != IRValue::REGISTER) continue;
                    int id = valueId.at(arg.name);
                    if (definedInBlock[id] != b) USE[b].set(id);
                }
                for (auto& pair : instr.asmMap) {
                    if (pair.second.kind != IRValue::REGISTER) continue;
                    int id = valueId.at(pair.second.name);
                    if (definedInBlock[id] != b) USE[b].set(id);
                }
                for (auto& [phiBlock, val] : instr.phiArgs) {
                    if (val.kind != IRValue::REGISTER) continue;
                    int id = valueId.at(val.name);
                    if (definedInBlock[id] != b) USE[b].set(id);
                    auto pit = blockIndex.find(phiBlock);
                    if (pit != blockIndex.end()) {
                        LiveBits& edge = PHI_EDGE_USE[pit->second][b];
                        if (edge.words.empty()) edge.words.assign(numWords, 0);
                        edge.set(id);
                    }
                }
                if (instr.dest && instr.dest->kind == IRValue::REGISTER) {
                    int id = valueId.at(instr.dest->name);
                    DEF[b].set(id);
                    definedInBlock[id] = b;
                }
            }
        }

        // Solve backward data-flow equations (CID-019). LIVE_OUT[b] is
        // every value live entering one of b's successors (plus the PHI
        // operands that successor reads along the edge), and LIVE_IN[b]
        // adds b's own upward-exposed uses, killed by b's definitions.
        std::vector<LiveBits> LIVE_IN(numBlocks);
        std::vector<LiveBits> LIVE_OUT(numBlocks);
        for (int b = 0; b < numBlocks; b++) {
            LIVE_IN[b].words = USE[b].words;
            LIVE_OUT[b].reset(numValues);
        }

        std::vector<uint64_t> newOut(numWords, 0);
        std::vector<uint64_t> newIn(numWords, 0);
        bool changed = true;
        while (changed) {
            changed = false;
            // CIDER-BUG-166: liveness flows backward, so process blocks in
            // reverse order — a sweep then reads each block's already-final
            // successors. The old forward order needed one sweep per block
            // on straight-line code (each sweep moved liveness back a single
            // block: O(B^2) set merges); reverse order reaches the same least
            // fixed point in one pass plus the confirming pass for acyclic
            // CFGs. CIDER-BUG-172: the merges are now word ORs, so a sweep
            // is O(blocks x live-set words) instead of O(blocks x live
            // values x log) with string allocations. The fixed point, and
            // therefore every register decision, is unchanged.
            for (int b = numBlocks - 1; b >= 0; b--) {
                std::fill(newOut.begin(), newOut.end(), 0);
                for (int s : successors[b]) {
                    const auto& succIn = LIVE_IN[s].words;
                    for (size_t i = 0; i < numWords; i++) newOut[i] |= succIn[i];
                    auto edgeUse = PHI_EDGE_USE[b].find(s);
                    if (edgeUse != PHI_EDGE_USE[b].end()) {
                        const auto& edge = edgeUse->second.words;
                        for (size_t i = 0; i < edge.size(); i++) newOut[i] |= edge[i];
                    }
                }
                if (newOut != LIVE_OUT[b].words) {
                    changed = true;
                    LIVE_OUT[b].words = newOut;
                }

                for (size_t i = 0; i < numWords; i++) {
                    newIn[i] = USE[b].words[i] | (LIVE_OUT[b].words[i] & ~DEF[b].words[i]);
                }
                if (newIn != LIVE_IN[b].words) {
                    changed = true;
                    LIVE_IN[b].words = newIn;
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
        // CIDER-BUG-172: regFirst/regLast per value replace the old
        // map<string, set<int>> histories (same min/max results).
        std::vector<int> regFirst(numValues, numBlocks);
        std::vector<int> regLast(numValues, -1);
        for (int b = 0; b < numBlocks; b++) {
            auto noteBlock = [&](int id) {
                if (b < regFirst[id]) regFirst[id] = b;
                if (b > regLast[id]) regLast[id] = b;
            };
            forEachSetBit(USE[b].words, noteBlock);
            forEachSetBit(DEF[b].words, noteBlock);
        }
        for (int src = 0; src < numBlocks; src++) {
            for (int tgt : successors[src]) {
                if (tgt < src) {  // back edge: src -> tgt
                    for (int t = tgt; t <= src; ++t) {
                        forEachSetBit(LIVE_IN[tgt].words, [&](int id) {
                            if (t < regFirst[id]) regFirst[id] = t;
                            if (t > regLast[id]) regLast[id] = t;
                        });
                    }
                }
            }
        }

        std::map<std::string, LiveInterval> intervals;
        for (int id = 0; id < numValues; id++) {
            if (regLast[id] < 0) continue;
            intervals[valueNames[id]] = {valueNames[id], regFirst[id], regLast[id], isFloat[id] != 0};
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
            std::vector<int> slotFirst(numValues, numBlocks);
            std::vector<int> slotLast(numValues, -1);
            for (int b = 0; b < numBlocks; b++) {
                auto noteBlock = [&](int id) {
                    if (b < slotFirst[id]) slotFirst[id] = b;
                    if (b > slotLast[id]) slotLast[id] = b;
                };
                forEachSetBit(USE[b].words, noteBlock);
                forEachSetBit(DEF[b].words, noteBlock);
                forEachSetBit(LIVE_IN[b].words, noteBlock);
                forEachSetBit(LIVE_OUT[b].words, noteBlock);
            }
            for (int id = 0; id < numValues; id++) {
                if (slotLast[id] < 0) continue;
                slotRanges[valueNames[id]] = {valueNames[id], slotFirst[id], slotLast[id], isFloat[id] != 0};
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
