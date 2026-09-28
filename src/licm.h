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
//
// CIDER-BUG-086: a CALL/VIRTUAL_CALL/ASM in the loop is a memory clobber —
// the callee may write any memory reachable from the pointers it is given —
// and a MEMCPY is a bulk write like STORE. Two consequences:
//   * MEMCPY is never hoisted (see the op exclusion in both hoistLoop
//     implementations): moving the write out of the loop freezes the first
//     iteration's bytes, and its source may be call-written.
//   * a LOAD from an alloca whose address escaped the function (passed to a
//     callee, stored into memory, returned, or substituted into an ASM)
//     cannot be hoisted across a clobber: the callee can write that alloca.
//     Allocas whose address never escapes are unreachable from any callee,
//     so their loads keep hoisting.
struct LicmCaches {
    std::map<std::string, const IRInstruction*> defMap;
    std::set<std::string> multiDef;
    std::set<std::string> loopStores;
    // CIDER-BUG-061: a volatile store in the loop is an observable write
    // that may alias any escaped object (the compiler has no alias oracle),
    // so no load may be hoisted across it.
    bool loopHasVolatileStore = false;
    // CIDER-BUG-086: a CALL/VIRTUAL_CALL/ASM in the loop may write any
    // memory reachable from its arguments.
    bool loopHasClobber = false;
    // Allocas whose address can reach a callee: passed as a CALL/VIRTUAL_CALL/
    // ASM argument (directly or through ADD/MOV/PHI), stored as a value, or
    // returned. Only these can be written by a clobber.
    std::set<std::string> escapedAllocas;
    // A multi-defined register (the inliner writes one `MOV dest, ret` per
    // cloned return) can carry any of several pointers; if one reaches a
    // callee, every alloca is conservatively treated as escaped.
    bool escapedUnknown = false;
};

// Follows ADD/MOV/PHI chains and collects every ALLOCA whose address `reg`
// may carry. Unknown provenance (a LOAD result, a parameter, ...) yields
// nothing: an address stored into memory is already marked when the STORE
// that stored it is scanned.
static void collectAllocaBases(const std::string& reg, const LicmCaches& c,
                               std::set<std::string>& out,
                               std::set<std::string>& visiting) {
    if (reg.empty() || !visiting.insert(reg).second) return;
    auto it = c.defMap.find(reg);
    if (it == c.defMap.end()) return;
    const IRInstruction* def = it->second;
    if (def->op == IROp::ALLOCA) {
        out.insert(reg);
        return;
    }
    if ((def->op == IROp::ADD || def->op == IROp::MOV) && !def->args.empty() &&
        def->args[0].kind == IRValue::REGISTER) {
        collectAllocaBases(def->args[0].name, c, out, visiting);
        return;
    }
    if (def->op == IROp::PHI) {
        for (const auto& [_, val] : def->phiArgs) {
            if (val.kind == IRValue::REGISTER) {
                collectAllocaBases(val.name, c, out, visiting);
            }
        }
    }
}

// Marks the allocas an escape-triggering operand may point at.
static void markEscapedValue(const IRValue& v, LicmCaches& c) {
    if (v.kind != IRValue::REGISTER) return;
    if (c.multiDef.count(v.name)) {
        c.escapedUnknown = true;
        return;
    }
    std::set<std::string> bases, visiting;
    collectAllocaBases(v.name, c, bases, visiting);
    c.escapedAllocas.insert(bases.begin(), bases.end());
}

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
            const bool inLoop = loopBlocks.find(b->getName()) != loopBlocks.end();
            if (inLoop && (si.op == IROp::STORE || si.op == IROp::MEMCPY)) {
                if (si.isVolatile) c.loopHasVolatileStore = true;
                if (!si.args.empty() && si.args[0].kind == IRValue::REGISTER) {
                    c.loopStores.insert(si.args[0].name);
                }
            }
            if (inLoop && (si.op == IROp::CALL || si.op == IROp::VIRTUAL_CALL ||
                           si.op == IROp::ASM)) {
                c.loopHasClobber = true;
            }
        }
    }
    // Escape sites are resolved after the whole def map exists: the block
    // order is not topological, so a use may precede its def here.
    // They are function-wide: a callee that received the address earlier can
    // be asked to write through it by an in-loop call (a stored address makes
    // the alloca reachable from whatever later holds it).
    for (const auto& b : func.getBlocks()) {
        for (const auto& si : b->getInstructions()) {
            if (si.op == IROp::CALL || si.op == IROp::VIRTUAL_CALL || si.op == IROp::ASM) {
                for (const auto& arg : si.args) markEscapedValue(arg, c);
                for (const auto& [_, val] : si.asmMap) markEscapedValue(val, c);
            } else if (si.op == IROp::STORE) {
                if (si.args.size() > 1) markEscapedValue(si.args[1], c);
            } else if (si.op == IROp::RET) {
                if (!si.args.empty()) markEscapedValue(si.args[0], c);
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
    // CIDER-BUG-086: a clobber in the loop can only write an alloca whose
    // address escaped the function; non-escaping slots are callee-unreachable.
    if (c.loopHasClobber && (c.escapedUnknown || c.escapedAllocas.count(reg))) return false;
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

                // CIDER-BUG-086: MEMCPY is a bulk memory write like STORE —
                // never hoist it. Its source may be written by an in-loop
                // call (the sret slot), and moving the write to the
                // preheader freezes the first iteration's bytes.
                if (instr.op == IROp::STORE || instr.op == IROp::MEMCPY ||
                    instr.op == IROp::CALL ||
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

                // Post-deconstruction: exclude same ops as SSA mode plus PUSH,
                // POP (and MEMCPY, see CIDER-BUG-086 above).
                if (instr.op == IROp::STORE || instr.op == IROp::MEMCPY ||
                    instr.op == IROp::CALL ||
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
