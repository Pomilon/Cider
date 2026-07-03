#ifndef CIDER_REG_ALLOC_H
#define CIDER_REG_ALLOC_H

#include "cider_ir.h"
#include <map>
#include <set>
#include <vector>
#include <string>
#include <iostream>
#include <algorithm>

namespace Cider {

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
            std::map<std::string, LiveInterval> intervals;
            int index = 0;

            // First pass: build initial live intervals
            for (auto& block : func.getBlocks()) {
                for (auto& instr : block->getInstructions()) {
                    if (instr.dest && instr.dest->kind == IRValue::REGISTER) {
                        updateInterval(intervals, instr.dest->name, index, instr.dest->type);
                    }
                    for (auto& arg : instr.args) {
                        if (arg.kind == IRValue::REGISTER) {
                            updateInterval(intervals, arg.name, index, arg.type);
                        }
                    }
                    for (auto& pair : instr.asmMap) {
                        if (pair.second.kind == IRValue::REGISTER) {
                            updateInterval(intervals, pair.second.name, index, pair.second.type);
                        }
                    }
                    index++;
                }
            }

            // Second pass: detect back-edges (loops) and extend intervals across them
            index = 0;
            std::map<std::string, int> labelToIndex;
            for (auto& block : func.getBlocks()) {
                labelToIndex[block->getName()] = index;
                index += (int)block->getInstructions().size();
            }
            index = 0;
            for (auto& block : func.getBlocks()) {
                for (auto& instr : block->getInstructions()) {
                    if (instr.op == IROp::BR || instr.op == IROp::COND_BR) {
                        for (auto& arg : instr.args) {
                            if (arg.kind == IRValue::LABEL) {
                                auto it = labelToIndex.find(arg.name);
                                if (it != labelToIndex.end() && it->second < index) {
                                    // Backward branch: extend intervals live across loop boundary
                                    for (auto& pair : intervals) {
                                        if (pair.second.start < index && pair.second.end >= it->second) {
                                            pair.second.end = index + 1;
                                        }
                                    }
                                }
                            }
                        }
                    }
                    index++;
                }
            }

            std::vector<LiveInterval> unhandled;
            for (auto& pair : intervals) {
                unhandled.push_back(pair.second);
            }
            std::sort(unhandled.begin(), unhandled.end());

            // Callee-saved registers only (preserved across calls, avoids ASM sizing issues)
            std::vector<std::string> freeIntRegs = {"rbx", "r12", "r13", "r14", "r15"};
            std::vector<std::string> freeFloatRegs = {"xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"};

            std::vector<LiveInterval> active;
            int currentStackOffset = 0;

            for (auto& interval : unhandled) {
                // Expire old intervals
                for (auto it = active.begin(); it != active.end(); ) {
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
                    currentStackOffset += 8;
                    allocation[interval.varName] = {true, "", -currentStackOffset};
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

    private:
        void updateInterval(std::map<std::string, LiveInterval>& intervals, const std::string& name, int index, IRType type) {
            if (intervals.find(name) == intervals.end()) {
                intervals[name] = {name, index, index, type == IRType::F32 || type == IRType::F64};
            } else {
                intervals[name].end = index;
            }
        }
    };

}

#endif
