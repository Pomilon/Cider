#pragma once

#include <set>
#include <vector>

#include "ir.h"

namespace cider {

class Optimizer {
public:
    static void dce(FunctionIR& func) {
        bool changed = true;
        while (changed) {
            changed = false;
            std::set<std::string> used;
            for (auto& block : func.getBlocks()) {
                for (auto& instr : block->getInstructions()) {
                    for (auto& arg : instr.args) {
                        if (arg.kind == IRValue::REGISTER) used.insert(arg.name);
                    }
                    for (auto& [phiBlock, val] : instr.phiArgs) {
                        if (val.kind == IRValue::REGISTER) used.insert(val.name);
                    }
                }
            }
            for (auto& block : func.getBlocks()) {
                auto& insts = block->getInstructions();
                for (auto it = insts.begin(); it != insts.end();) {
                    if (it->dest && it->dest->kind == IRValue::REGISTER) {
                        if (used.find(it->dest->name) == used.end()) {
                            // CIDER-BUG-061: a volatile access is observable
                            // even when its result is unused (a status-register
                            // read clears an IRQ); it must never be dropped.
                            if (!it->isVolatile && it->op != IROp::STORE && it->op != IROp::CALL &&
                                it->op != IROp::VIRTUAL_CALL && it->op != IROp::BR &&
                                it->op != IROp::COND_BR && it->op != IROp::RET &&
                                it->op != IROp::ASM && it->op != IROp::ALLOCA &&
                                it->op != IROp::MOV && it->op != IROp::LOAD &&
                                it->op != IROp::PUSH && it->op != IROp::POP) {
                                it = insts.erase(it);
                                changed = true;
                                continue;
                            }
                        }
                    }
                    ++it;
                }
            }
        }
    }
};

}  // namespace cider
