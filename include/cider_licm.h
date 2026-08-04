#ifndef CIDER_LICM_H
#define CIDER_LICM_H

#include "cider_ir.h"
#include "cider_dominators.h"
#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace Cider {

class SSALicmPass {
public:
    bool run(ModuleIR& module);

private:
    DominatorTree dt_;
    NaturalLoopFinder loopFinder_;

    bool runOnFunction(FunctionIR& func);
    bool hoistLoop(FunctionIR& func, const Loop& loop, const std::string& preheader);
};

bool SSALicmPass::run(ModuleIR& module) {
    bool changed = false;
    for (auto& func : module.getFunctions()) {
        if (runOnFunction(*func)) changed = true;
    }
    return changed;
}

bool SSALicmPass::runOnFunction(FunctionIR& func) {
    dt_.build(func);
    loopFinder_.build(func, dt_);
    if (loopFinder_.getLoops().empty()) return false;

    std::vector<std::string> headers;
    for (const auto& loop : loopFinder_.getLoops()) headers.push_back(loop.header);
    std::sort(headers.begin(), headers.end(),
        [&](const std::string& a, const std::string& b) {
            const auto* la = loopFinder_.loopForHeader(a);
            const auto* lb = loopFinder_.loopForHeader(b);
            if (!la || !lb) return false;
            return la->depth > lb->depth;
        });

    bool changed = false;
    for (const std::string& hdr : headers) {
        const Loop* loop = loopFinder_.loopForHeader(hdr);
        if (!loop) continue;

        std::string phName = NaturalLoopFinder::insertPreheader(func, *loop, dt_);
        if (phName.empty()) continue;

        dt_.build(func);
        loopFinder_.build(func, dt_);
        const Loop* updatedLoop = loopFinder_.loopForHeader(hdr);
        if (!updatedLoop) continue;
        if (hoistLoop(func, *updatedLoop, phName)) changed = true;
    }
    return changed;
}

bool SSALicmPass::hoistLoop(FunctionIR& func, const Loop& loop, const std::string& preheader) {
    std::set<std::string> loopBlocks = loop.blocks;

    std::set<std::string> invariantRegs;
    for (const auto& b : func.getBlocks()) {
        if (loopBlocks.find(b->getName()) != loopBlocks.end()) continue;
        if (b->getName() == preheader) continue;
        for (const auto& instr : b->getInstructions()) {
            if (instr.dest && instr.dest->kind == IRValue::REGISTER) {
                invariantRegs.insert(instr.dest->name);
            }
        }
    }

    std::vector<const IRInstruction*> invariantInstrs;
    std::set<std::pair<std::string, size_t>> invariantInsts;
    bool changed = true;

    while (changed) {
        changed = false;
        for (const auto& b : func.getBlocks()) {
            if (loopBlocks.find(b->getName()) == loopBlocks.end()) continue;
            const auto& instrs = b->getInstructions();
            for (size_t ii = 0; ii < instrs.size(); ++ii) {
                const auto& instr = instrs[ii];
                if (invariantInsts.find({b->getName(), ii}) != invariantInsts.end()) continue;

                if (instr.op == IROp::STORE || instr.op == IROp::CALL ||
                    instr.op == IROp::VIRTUAL_CALL || instr.op == IROp::RET ||
                    instr.op == IROp::COND_BR || instr.op == IROp::BR ||
                    instr.op == IROp::ASM || instr.op == IROp::LOAD ||
                    instr.op == IROp::PHI) continue;

                bool allInvariant = true;
                for (const auto& arg : instr.args) {
                    if (arg.kind == IRValue::REGISTER &&
                        invariantRegs.find(arg.name) == invariantRegs.end()) {
                        allInvariant = false;
                        break;
                    }
                }
                for (const auto& [_, val] : instr.asmMap) {
                    if (val.kind == IRValue::REGISTER &&
                        invariantRegs.find(val.name) == invariantRegs.end()) {
                        allInvariant = false;
                        break;
                    }
                }

                if (allInvariant) {
                    invariantInsts.insert({b->getName(), ii});
                    invariantInstrs.push_back(&instr);
                    if (instr.dest && instr.dest->kind == IRValue::REGISTER) {
                        invariantRegs.insert(instr.dest->name);
                    }
                    changed = true;
                }
            }
        }
    }

    if (invariantInstrs.empty()) return false;

    auto* preheaderBlock = [&]() -> BasicBlock* {
        for (auto& b : func.getBlocks()) {
            if (b->getName() == preheader) return b.get();
        }
        return nullptr;
    }();
    if (!preheaderBlock) return false;

    auto& preInstrs = preheaderBlock->getInstructions();
    for (const auto* instr : invariantInstrs) {
        auto pos = preInstrs.empty() ? preInstrs.end() : preInstrs.end() - 1;
        preInstrs.insert(pos, *instr);
    }

    for (auto& b : func.getBlocks()) {
        if (loopBlocks.find(b->getName()) == loopBlocks.end()) continue;
        auto& instrs = b->getInstructions();
        std::vector<size_t> toRemove;
        for (size_t ii = 0; ii < instrs.size(); ++ii) {
            if (invariantInsts.find({b->getName(), ii}) != invariantInsts.end()) {
                toRemove.push_back(ii);
            }
        }
        for (auto ri = toRemove.rbegin(); ri != toRemove.rend(); ++ri) {
            instrs.erase(instrs.begin() + *ri);
        }
    }

    return true;
}

class PostLicmPass {
public:
    bool run(ModuleIR& module);

private:
    DominatorTree dt_;
    NaturalLoopFinder loopFinder_;

    bool runOnFunction(FunctionIR& func);
    bool hoistLoop(FunctionIR& func, const Loop& loop, const std::string& preheader);
};

bool PostLicmPass::run(ModuleIR& module) {
    bool changed = false;
    for (auto& func : module.getFunctions()) {
        if (runOnFunction(*func)) changed = true;
    }
    return changed;
}

bool PostLicmPass::runOnFunction(FunctionIR& func) {
    dt_.build(func);
    loopFinder_.build(func, dt_);
    if (loopFinder_.getLoops().empty()) return false;

    std::vector<std::string> headers;
    for (const auto& loop : loopFinder_.getLoops()) headers.push_back(loop.header);
    std::sort(headers.begin(), headers.end(),
        [&](const std::string& a, const std::string& b) {
            const auto* la = loopFinder_.loopForHeader(a);
            const auto* lb = loopFinder_.loopForHeader(b);
            if (!la || !lb) return false;
            return la->depth > lb->depth;
        });

    bool changed = false;
    for (const std::string& hdr : headers) {
        const Loop* loop = loopFinder_.loopForHeader(hdr);
        if (!loop) continue;

        std::string phName = NaturalLoopFinder::insertPreheader(func, *loop, dt_);
        if (phName.empty()) continue;

        dt_.build(func);
        loopFinder_.build(func, dt_);
        const Loop* updatedLoop = loopFinder_.loopForHeader(hdr);
        if (!updatedLoop) continue;
        if (hoistLoop(func, *updatedLoop, phName)) changed = true;
    }
    return changed;
}

bool PostLicmPass::hoistLoop(FunctionIR& func, const Loop& loop, const std::string& preheader) {
    std::set<std::string> loopBlocks = loop.blocks;

    std::set<std::string> invariantRegs;
    for (const auto& b : func.getBlocks()) {
        if (loopBlocks.find(b->getName()) != loopBlocks.end()) continue;
        if (b->getName() == preheader) continue;
        for (const auto& instr : b->getInstructions()) {
            if (instr.dest && instr.dest->kind == IRValue::REGISTER) {
                invariantRegs.insert(instr.dest->name);
            }
        }
    }

    std::vector<const IRInstruction*> invariantInstrs;
    std::set<std::pair<std::string, size_t>> invariantInsts;
    bool changed = true;

    while (changed) {
        changed = false;
        for (const auto& b : func.getBlocks()) {
            if (loopBlocks.find(b->getName()) == loopBlocks.end()) continue;
            const auto& instrs = b->getInstructions();
            for (size_t ii = 0; ii < instrs.size(); ++ii) {
                const auto& instr = instrs[ii];
                if (invariantInsts.find({b->getName(), ii}) != invariantInsts.end()) continue;

                // Post-deconstruction: exclude same ops as SSA mode plus PUSH, POP.
                if (instr.op == IROp::STORE || instr.op == IROp::CALL ||
                    instr.op == IROp::VIRTUAL_CALL || instr.op == IROp::RET ||
                    instr.op == IROp::COND_BR || instr.op == IROp::BR ||
                    instr.op == IROp::ASM || instr.op == IROp::LOAD ||
                    instr.op == IROp::PHI || instr.op == IROp::PUSH ||
                    instr.op == IROp::POP) continue;

                bool allInvariant = true;
                for (const auto& arg : instr.args) {
                    if (arg.kind == IRValue::REGISTER &&
                        invariantRegs.find(arg.name) == invariantRegs.end()) {
                        allInvariant = false;
                        break;
                    }
                }
                for (const auto& [_, val] : instr.asmMap) {
                    if (val.kind == IRValue::REGISTER &&
                        invariantRegs.find(val.name) == invariantRegs.end()) {
                        allInvariant = false;
                        break;
                    }
                }

                if (allInvariant) {
                    invariantInsts.insert({b->getName(), ii});
                    invariantInstrs.push_back(&instr);
                    if (instr.dest && instr.dest->kind == IRValue::REGISTER) {
                        invariantRegs.insert(instr.dest->name);
                    }
                    changed = true;
                }
            }
        }
    }

    if (invariantInstrs.empty()) return false;

    auto* preheaderBlock = [&]() -> BasicBlock* {
        for (auto& b : func.getBlocks()) {
            if (b->getName() == preheader) return b.get();
        }
        return nullptr;
    }();

    if (!preheaderBlock) return false;

    auto& preInstrs = preheaderBlock->getInstructions();
    auto insertPos = preInstrs.end();
    if (!preInstrs.empty()) insertPos = preInstrs.end() - 1;

    for (const auto* instr : invariantInstrs) {
        preInstrs.insert(insertPos, *instr);
    }

    for (auto& b : func.getBlocks()) {
        if (loopBlocks.find(b->getName()) == loopBlocks.end()) continue;
        auto& instrs = b->getInstructions();
        std::vector<size_t> toRemove;
        for (size_t ii = 0; ii < instrs.size(); ++ii) {
            if (invariantInsts.find({b->getName(), ii}) != invariantInsts.end()) {
                toRemove.push_back(ii);
            }
        }
        for (auto ri = toRemove.rbegin(); ri != toRemove.rend(); ++ri) {
            instrs.erase(instrs.begin() + *ri);
        }
    }

    return true;
}

} // namespace Cider

#endif
