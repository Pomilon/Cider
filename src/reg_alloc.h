#pragma once

#include <algorithm>
#include <iostream>
#include <map>
#include <set>
#include <string>
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

        // Map block name to index
        std::map<std::string, int> blockIndex;
        for (int i = 0; i < numBlocks; i++) {
            blockIndex[func.getBlocks()[i]->getName()] = i;
        }

        // Build successor lists
        std::vector<std::vector<int>> successors(numBlocks);
        for (int b = 0; b < numBlocks; b++) {
            auto& instrs = func.getBlocks()[b]->getInstructions();
            for (auto& instr : instrs) {
                if (instr.op == IROp::BR) {
                    if (!instr.args.empty() && instr.args[0].kind == IRValue::LABEL) {
                        auto it = blockIndex.find(instr.args[0].name);
                        if (it != blockIndex.end()) {
                            successors[b].push_back(it->second);
                        }
                    }
                } else if (instr.op == IROp::COND_BR) {
                    if (instr.args.size() > 1 && instr.args[1].kind == IRValue::LABEL) {
                        auto it = blockIndex.find(instr.args[1].name);
                        if (it != blockIndex.end()) {
                            successors[b].push_back(it->second);
                        }
                    }
                    if (instr.args.size() > 2 && instr.args[2].kind == IRValue::LABEL) {
                        auto it = blockIndex.find(instr.args[2].name);
                        if (it != blockIndex.end()) {
                            successors[b].push_back(it->second);
                        }
                    }
                }
            }
        }

        // Track register types
        std::map<std::string, bool> isFloat;

        // Compute USE[b] and DEF[b]
        std::vector<std::set<std::string>> USE(numBlocks);
        std::vector<std::set<std::string>> DEF(numBlocks);

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
                    }
                }
                if (instr.dest && instr.dest->kind == IRValue::REGISTER) {
                    DEF[b].insert(instr.dest->name);
                    definedInBlock.insert(instr.dest->name);
                    isFloat[instr.dest->name] = (instr.dest->type == IRType::F32 || instr.dest->type == IRType::F64);
                }
            }
        }

        // Solve data-flow equations
        std::vector<std::set<std::string>> LIVE_IN(numBlocks);
        std::vector<std::set<std::string>> LIVE_OUT(numBlocks);

        for (int b = 0; b < numBlocks; b++) {
            LIVE_IN[b] = USE[b];
        }

        bool changed = true;
        while (changed) {
            changed = false;
            for (int b = 0; b < numBlocks; b++) {
                std::set<std::string> newOut;
                for (int s : successors[b]) {
                    newOut.insert(LIVE_IN[s].begin(), LIVE_IN[s].end());
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

        // Build intervals from liveness. USE/DEF alone is NOT enough: a value
        // used in a loop header is re-used on every back edge, so it is live
        // across the whole loop body even though its USE/DEF blocks only span
        // [preheader, header]. Without extending those intervals the linear
        // scan hands the value's register (or spill slot) to a body value,
        // clobbering the loop-carried operand. LIVE_IN/LIVE_OUT can't be
        // added wholesale — they also mark values that merely flow through a
        // block boundary, which would stretch intervals into blocks where
        // the value is not used (unnecessary spills). So only extend along
        // back edges: every value live into a back-edge target (loop header)
        // must keep its register through all blocks of the loop body.
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

        // Linear scan using block indices
        std::vector<LiveInterval> unhandled;
        for (auto& pair : intervals) {
            unhandled.push_back(pair.second);
        }
        std::sort(unhandled.begin(), unhandled.end());

        std::vector<std::string> freeIntRegs = {"rbx", "r12", "r13", "r14", "r15"};
        std::vector<std::string> freeFloatRegs = {"xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"};

        std::vector<LiveInterval> active;
        int currentStackOffset = 0;
        // Reusable spill slots: negative offsets, deepest first.
        std::vector<int> freeSpillSlots;

        for (auto& interval : unhandled) {
            for (auto it = active.begin(); it != active.end();) {
                if (it->end < interval.start) {
                    auto& res = allocation[it->varName];
                    if (!res.isSpilled) {
                        if (it->isFloat) freeFloatRegs.push_back(res.reg);
                        else freeIntRegs.push_back(res.reg);
                    } else {
                        freeSpillSlots.push_back(res.stackOffset);
                    }
                    it = active.erase(it);
                } else {
                    ++it;
                }
            }

            auto& freeRegs = interval.isFloat ? freeFloatRegs : freeIntRegs;
            if (freeRegs.empty()) {
                if (!freeSpillSlots.empty()) {
                    // Reuse the deepest free slot (most negative offset).
                    std::sort(freeSpillSlots.begin(), freeSpillSlots.end());
                    int slot = freeSpillSlots.front();
                    freeSpillSlots.erase(freeSpillSlots.begin());
                    allocation[interval.varName] = {true, "", slot};
                } else {
                    currentStackOffset += 8;
                    allocation[interval.varName] = {true, "", -currentStackOffset};
                }
                // Track spilled intervals too: their slots return to the pool
                // when the interval expires.
                active.push_back(interval);
            } else {
                std::string reg = freeRegs.back();
                freeRegs.pop_back();
                allocation[interval.varName] = {false, reg, 0};
                active.push_back(interval);
            }
        }

        finalStackSize = currentStackOffset;
        return allocation;
    }
};

}  // namespace cider
