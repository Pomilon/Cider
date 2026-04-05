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
        int start = 0;
        int end = 0;
        bool isFloat = false;
        
        bool operator<(const LiveInterval& other) const {
            return start < other.start;
        }
    };

    struct AllocationResult {
        bool isSpilled;
        std::string reg; 
        int stackOffset; 
    };

    class RegisterAllocator {
    public:
        std::map<std::string, AllocationResult> allocate(FunctionIR& func, int& finalStackSize) {
            std::map<std::string, AllocationResult> allocation;
            std::map<std::string, LiveInterval> intervals;
            int index = 0;
            
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
                    for (auto& [name, val] : instr.asmMap) {
                        if (val.kind == IRValue::REGISTER) {
                            updateInterval(intervals, val.name, index, val.type);
                        }
                    }
                    index++;
                }
            }

            // Conservatively extend all intervals to the end of the function
            // to ensure registers aren't reused across loop back-edges.
            for (auto& [name, interval] : intervals) {
                interval.end = index;
            }
            
            std::vector<LiveInterval> unhandled;
            for (auto& [name, interval] : intervals) {
                unhandled.push_back(interval);
            }
            std::sort(unhandled.begin(), unhandled.end());
            
            std::vector<std::string> freeIntRegs = {"rbx", "r12", "r13", "r14", "r15"};
            std::vector<std::string> freeFloatRegs = {"xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"};
            
            std::vector<LiveInterval> active;
            int currentStackOffset = 0;
            
            for (auto& interval : unhandled) {
                // Expire old intervals
                for (auto it = active.begin(); it != active.end(); ) {
                    if (it->end >= interval.start) {
                        ++it;
                        continue;
                    }
                    auto alloc = allocation[it->varName];
                    if (!alloc.isSpilled) {
                        if (it->isFloat) freeFloatRegs.push_back(alloc.reg);
                        else freeIntRegs.push_back(alloc.reg);
                    }
                    it = active.erase(it);
                }
                
                auto& freeRegs = interval.isFloat ? freeFloatRegs : freeIntRegs;
                
                if (freeRegs.empty()) {
                    // Spill
                    currentStackOffset += 8;
                    allocation[interval.varName] = {true, "", -currentStackOffset};
                } else {
                    // Allocate register
                    std::string reg = freeRegs.back();
                    freeRegs.pop_back();
                    allocation[interval.varName] = {false, reg, 0};
                    active.push_back(interval);
                }
            }
            
            finalStackSize = currentStackOffset;
            if (finalStackSize % 16 != 0) {
                finalStackSize += 16 - (finalStackSize % 16);
            }
            
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
