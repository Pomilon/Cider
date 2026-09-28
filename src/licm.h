#pragma once

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "dominators.h"
#include "ir.h"

namespace cider {

// A LOAD is loop-invariant only if the memory it reads is not modified inside
// the loop. Cider stores locals in allocas, so the LOAD's address register is
// typically an alloca address. We only hoist LOADs whose address is an ALLOCA
// and where no STORE in the loop body writes to that same alloca.
//
// Issue #15: the old version scanned the whole function for the def and the
// whole loop for STOREs on EVERY LOAD (quadratic). Callers now build the
// def map + loop STORE set once per hoistLoop and reuse them here (O(1)).
struct LicmCaches {
    std::map<std::string, const IRInstruction*> defMap;
    std::set<std::string> multiDef;
    std::set<std::string> loopStores;
    // CIDER-BUG-061: a volatile store in the loop is an observable write
    // that may alias any escaped object (the compiler has no alias oracle),
    // so no load may be hoisted across it.
    bool loopHasVolatileStore = false;
};
static LicmCaches buildLicmCaches(const FunctionIR& func,
                                  const std::set<std::string>& loopBlocks) {
    LicmCaches c;
    for (const auto& b : func.getBlocks()) {
        for (const auto& si : b->getInstructions()) {
            if (si.dest && si.dest->kind == IRValue::REGISTER) {
                auto it = c.defMap.find(si.dest->name);
                if (it == c.defMap.end()) {
                    c.defMap[si.dest->name] = &si;
                } else {
                    c.multiDef.insert(si.dest->name);
                }
            }
            if (loopBlocks.find(b->getName()) != loopBlocks.end() &&
                si.op == IROp::STORE) {
                if (si.isVolatile) c.loopHasVolatileStore = true;
                if (!si.args.empty() && si.args[0].kind == IRValue::REGISTER) {
                    c.loopStores.insert(si.args[0].name);
                }
            }
        }
    }
    return c;
}
static bool isLoadInvariantCached(const IRInstruction& instr,
                                 const LicmCaches& c) {
    if (instr.op != IROp::LOAD || instr.args.empty() || instr.args[0].kind != IRValue::REGISTER)
        return false;
    // CIDER-BUG-061: a volatile load must execute once per iteration, in
    // place — never hoisted. A volatile store anywhere in the loop may alias
    // the loaded object, so it conservatively blocks every load hoist.
    if (instr.isVolatile || c.loopHasVolatileStore) return false;
    const std::string& reg = instr.args[0].name;
    if (c.multiDef.count(reg)) return false;
    auto it = c.defMap.find(reg);
    if (it == c.defMap.end()) return false;
    if (it->second->op != IROp::ALLOCA) return false;  // non-alloca: conservative
    if (c.loopStores.count(reg)) return false;
    return true;
}

class SSALicmPass {
public:
    bool run(ModuleIR& module);

private:
    DominatorTree dt_;
    NaturalLoopFinder loopFinder_;

    bool runOnFunction(FunctionIR& func);
    bool hoistLoop(FunctionIR& func, const Loop& loop, const std::string& preheader,
                   const DominatorTree& dt);
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

    // Issue #15: batch preheader insertion. The old code rebuilt the
    // Lengauer-Tarjan dominator tree + loop forest after EVERY loop
    // (O(L*(N+E))), hanging on kernel-size functions. Preheaders don't need
    // up-to-date DT (insertPreheader ignores it), so insert all first, then
    // rebuild once and hoist.
    std::map<std::string, std::string> preheaders;
    // Snapshot loop bodies: insertion mutates func blocks, which would
    // invalidate live Loop* pointers.
    std::map<std::string, Loop> loopSnap;
    for (const std::string& hdr : headers) {
        const Loop* l = loopFinder_.loopForHeader(hdr);
        if (l) loopSnap[hdr] = *l;
    }
    for (const std::string& hdr : headers) {
        auto it = loopSnap.find(hdr);
        if (it == loopSnap.end()) continue;
        std::string phName = NaturalLoopFinder::insertPreheader(func, it->second, dt_);
        if (!phName.empty()) preheaders[hdr] = phName;
    }
    if (preheaders.empty()) return false;

    dt_.build(func);
    loopFinder_.build(func, dt_);
    bool changed = false;
    for (const auto& [hdr, phName] : preheaders) {
        const Loop* updatedLoop = loopFinder_.loopForHeader(hdr);
        if (!updatedLoop) continue;
        if (hoistLoop(func, *updatedLoop, phName, dt_)) changed = true;
    }
    return changed;
}

bool SSALicmPass::hoistLoop(FunctionIR& func, const Loop& loop, const std::string& preheader,
                            const DominatorTree& dt) {
    std::set<std::string> loopBlocks = loop.blocks;
    // Cached def/store info (Issue #15): one scan per loop, not per LOAD.
    LicmCaches licmCache = buildLicmCaches(func, loopBlocks);

    std::set<std::string> invariantRegs;
    for (const auto& b : func.getBlocks()) {
        if (loopBlocks.find(b->getName()) != loopBlocks.end()) continue;
        // A register is only loop-invariant if its def is executed on every
        // path reaching the loop, i.e. its block dominates the preheader.
        // Defs in blocks after the loop (or on sibling paths) are NOT valid
        // operands while the loop runs — hoisting a use of one reads a
        // not-yet-defined (garbage) register.
        if (!dt.dominates(b->getName(), preheader)) continue;
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

                // CIDER-BUG-061: volatile accesses never move — they must
                // execute exactly where written.
                if (instr.isVolatile) continue;

                // CIDER-ISS-012 support: a register with more than one
                // definition is not SSA. Hoisting one definition to the
                // preheader (and dropping it from the loop) lets the other
                // definitions — and the stale preheader value — reach uses
                // that used to be path-dependent. The inliner creates
                // exactly this shape: every cloned RET does
                // `MOV callDest, retval`. Registers were unique before
                // inlining, so this was never guarded.
                if (instr.dest && instr.dest->kind == IRValue::REGISTER &&
                    licmCache.multiDef.count(instr.dest->name)) {
                    continue;
                }

                if (instr.op == IROp::STORE || instr.op == IROp::CALL ||
                    instr.op == IROp::VIRTUAL_CALL || instr.op == IROp::RET ||
                    instr.op == IROp::COND_BR || instr.op == IROp::BR ||
                    instr.op == IROp::ASM ||
                    instr.op == IROp::PHI) continue;

                // For LOAD: require the memory read to be truly loop-invariant.
                if (instr.op == IROp::LOAD && !isLoadInvariantCached(instr, licmCache)) continue;

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
    bool hoistLoop(FunctionIR& func, const Loop& loop, const std::string& preheader,
                   const DominatorTree& dt);
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

    // Same batching as SSA mode (Issue #15): one DT+loop rebuild total.
    std::map<std::string, std::string> preheaders;
    std::map<std::string, Loop> loopSnap;
    for (const std::string& hdr : headers) {
        const Loop* l = loopFinder_.loopForHeader(hdr);
        if (l) loopSnap[hdr] = *l;
    }
    for (const std::string& hdr : headers) {
        auto it = loopSnap.find(hdr);
        if (it == loopSnap.end()) continue;
        std::string phName = NaturalLoopFinder::insertPreheader(func, it->second, dt_);
        if (!phName.empty()) preheaders[hdr] = phName;
    }
    if (preheaders.empty()) return false;

    dt_.build(func);
    loopFinder_.build(func, dt_);
    bool changed = false;
    for (const auto& [hdr, phName] : preheaders) {
        const Loop* updatedLoop = loopFinder_.loopForHeader(hdr);
        if (!updatedLoop) continue;
        if (hoistLoop(func, *updatedLoop, phName, dt_)) changed = true;
    }
    return changed;
}

bool PostLicmPass::hoistLoop(FunctionIR& func, const Loop& loop, const std::string& preheader,
                             const DominatorTree& dt) {
    std::set<std::string> loopBlocks = loop.blocks;
    // Cached def/store info (Issue #15): one scan per loop, not per LOAD.
    LicmCaches licmCache = buildLicmCaches(func, loopBlocks);

    std::set<std::string> invariantRegs;
    for (const auto& b : func.getBlocks()) {
        if (loopBlocks.find(b->getName()) != loopBlocks.end()) continue;
        // See SSALicmPass::hoistLoop: an operand is invariant only if its def
        // block dominates the preheader (executed on every path to the loop).
        if (!dt.dominates(b->getName(), preheader)) continue;
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

                // CIDER-BUG-061: volatile accesses never move — they must
                // execute exactly where written.
                if (instr.isVolatile) continue;

                // CIDER-ISS-012 support: see SSALicmPass::hoistLoop — a
                // multi-defined register is not SSA, and hoisting one of its
                // definitions corrupts the other paths. The inliner's
                // multi-RET `MOV callDest, retval` shape hits this.
                if (instr.dest && instr.dest->kind == IRValue::REGISTER &&
                    licmCache.multiDef.count(instr.dest->name)) {
                    continue;
                }

                // Post-deconstruction: exclude same ops as SSA mode plus PUSH, POP.
                if (instr.op == IROp::STORE || instr.op == IROp::CALL ||
                    instr.op == IROp::VIRTUAL_CALL || instr.op == IROp::RET ||
                    instr.op == IROp::COND_BR || instr.op == IROp::BR ||
                    instr.op == IROp::ASM ||
                    instr.op == IROp::PHI || instr.op == IROp::PUSH ||
                    instr.op == IROp::POP) continue;

                // For LOAD: require the memory read to be truly loop-invariant.
                if (instr.op == IROp::LOAD && !isLoadInvariantCached(instr, licmCache)) continue;

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

    // Recompute the insert position each iteration: inserting invalidates
    // iterators at/after the insertion point, and a stale iterator would
    // both be UB and scramble the hoisted order (dependents before their
    // operands). Inserting before the terminator preserves worklist order.
    for (const auto* instr : invariantInstrs) {
        auto insertPos = preInstrs.empty() ? preInstrs.end() : preInstrs.end() - 1;
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

}  // namespace cider
