#ifndef CIDER_ESCAPE_H
#define CIDER_ESCAPE_H

#include "cider_ir.h"
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace Cider {

class EscapeAnalysis {
public:
    void run(ModuleIR& module) {
        for (auto& func : module.getFunctions()) {
            escapingParams_[func->getName()] = {};
        }
        computeSummaries(module);
        for (auto& func : module.getFunctions()) {
            analyzeAndRewrite(*func);
        }
    }

private:
    // func name → set of param indices that escape
    std::map<std::string, std::set<int>> escapingParams_;

    // Per-analysis state (rebuilt per call):
    std::map<std::string, const IRInstruction*> defs_;  // reg → defining instruction
    std::set<std::string> values_;                      // regs holding the tracked pointer
    std::set<std::string> locals_;                      // alloca slots holding the tracked pointer
    std::vector<std::pair<BasicBlock*, size_t>> freeSites_;  // (block, instr index) of call free

    // --- helpers ---

    static bool isMallocCall(const IRInstruction& instr) {
        return instr.op == IROp::CALL && !instr.args.empty() &&
               instr.args[0].kind == IRValue::LABEL && instr.args[0].name == "malloc";
    }

    static bool isFreeCall(const IRInstruction& instr) {
        return instr.op == IROp::CALL && !instr.args.empty() &&
               instr.args[0].kind == IRValue::LABEL && instr.args[0].name == "free";
    }

    // Does the callee's param at 0-based position `paramIdx` escape?
    // Unknown functions and indirect calls default to escaping (conservative).
    bool calleeParamEscapes(const IRInstruction& call, size_t paramIdx) {
        if (call.args.empty() || call.args[0].kind != IRValue::LABEL) return true;
        const std::string& callee = call.args[0].name;
        if (callee == "free") return false;  // free never retains its pointer
        auto it = escapingParams_.find(callee);
        if (it == escapingParams_.end()) return true;
        return it->second.count((int)paramIdx) > 0;
    }

    void buildDefs(FunctionIR& func) {
        defs_.clear();
        for (auto& block : func.getBlocks()) {
            for (auto& instr : block->getInstructions()) {
                if (instr.dest && instr.dest->kind == IRValue::REGISTER) {
                    defs_[instr.dest->name] = &instr;
                }
            }
        }
    }

    // Trace a register to its base: returns the local ALLOCA register name it
    // ultimately derives from, or "" for anything else (param, heap pointer,
    // loaded value, unknown).
    std::string baseOf(const std::string& reg) {
        std::set<std::string> seen;
        std::string cur = reg;
        while (!cur.empty() && !seen.count(cur)) {
            seen.insert(cur);
            auto it = defs_.find(cur);
            if (it == defs_.end()) return "";
            const IRInstruction* def = it->second;
            if (def->op == IROp::ALLOCA) return cur;
            if (def->op == IROp::ADD || def->op == IROp::MOV) {
                if (!def->args.empty() && def->args[0].kind == IRValue::REGISTER) {
                    cur = def->args[0].name;
                    continue;
                }
                return "";
            }
            return "";
        }
        return "";
    }

    bool isLocalAlloca(const std::string& reg) {
        auto it = defs_.find(reg);
        return it != defs_.end() && it->second->op == IROp::ALLOCA;
    }

    // --- core: does the value in rootReg escape? ---
    //
    // Propagates the tracked pointer through MOV/PHI/CVT/LOAD and taints local
    // alloca slots that store it. Returns true if any escape trigger fires.
    bool valueEscapes(FunctionIR& func, const std::string& rootReg) {
        buildDefs(func);
        values_.clear();
        locals_.clear();
        freeSites_.clear();
        values_.insert(rootReg);

        // Propagate tracked values and taint locals (fixpoint over instructions).
        bool changed = true;
        while (changed) {
            changed = false;
            for (auto& block : func.getBlocks()) {
                for (auto& instr : block->getInstructions()) {
                    if (instr.dest && instr.dest->kind == IRValue::REGISTER) {
                        const std::string& dst = instr.dest->name;
                        if (values_.count(dst)) continue;
                        if (instr.op == IROp::MOV || instr.op == IROp::CVT_ITOF ||
                            instr.op == IROp::CVT_FTOI) {
                            if (!instr.args.empty() && instr.args[0].kind == IRValue::REGISTER &&
                                values_.count(instr.args[0].name)) {
                                values_.insert(dst);
                                changed = true;
                            }
                        } else if (instr.op == IROp::PHI) {
                            for (const auto& [pred, val] : instr.phiArgs) {
                                if (val.kind == IRValue::REGISTER && values_.count(val.name)) {
                                    values_.insert(dst);
                                    changed = true;
                                    break;
                                }
                            }
                        } else if (instr.op == IROp::LOAD) {
                            if (!instr.args.empty() && instr.args[0].kind == IRValue::REGISTER) {
                                std::string base = baseOf(instr.args[0].name);
                                if (!base.empty() && locals_.count(base)) {
                                    values_.insert(dst);
                                    changed = true;
                                }
                            }
                        }
                    }
                    if (instr.op == IROp::STORE && instr.args.size() > 1 &&
                        instr.args[1].kind == IRValue::REGISTER && values_.count(instr.args[1].name)) {
                        if (!instr.args.empty() && instr.args[0].kind == IRValue::REGISTER) {
                            std::string base = baseOf(instr.args[0].name);
                            if (!base.empty() && isLocalAlloca(base) && !locals_.count(base)) {
                                locals_.insert(base);
                                changed = true;
                            }
                        }
                    }
                }
            }
        }

        // Escape checks.
        for (auto& block : func.getBlocks()) {
            auto& instrs = block->getInstructions();
            for (size_t ii = 0; ii < instrs.size(); ++ii) {
                const auto& instr = instrs[ii];
                switch (instr.op) {
                    case IROp::RET: {
                        if (!instr.args.empty() && instr.args[0].kind == IRValue::REGISTER) {
                            const std::string& v = instr.args[0].name;
                            if (values_.count(v)) return true;
                            std::string b = baseOf(v);
                            if (!b.empty() && locals_.count(b)) return true;
                        }
                        break;
                    }
                    case IROp::STORE: {
                        if (instr.args.size() < 2) break;
                        // Case 1: storing a tracked pointer value.
                        if (instr.args[1].kind == IRValue::REGISTER && values_.count(instr.args[1].name)) {
                            if (instr.args[0].kind == IRValue::LABEL) return true;  // global
                            if (instr.args[0].kind == IRValue::REGISTER) {
                                std::string base = baseOf(instr.args[0].name);
                                bool isLocal = !base.empty() && isLocalAlloca(base);
                                bool isSelf = values_.count(base);
                                if (!isLocal && !isSelf) return true;  // into another heap object
                            }
                        }
                        // Case 2: storing the ADDRESS of a tainted local.
                        if (instr.args[1].kind == IRValue::REGISTER) {
                            std::string vbase = baseOf(instr.args[1].name);
                            if (!vbase.empty() && locals_.count(vbase)) {
                                if (instr.args[0].kind == IRValue::LABEL) return true;
                                if (instr.args[0].kind == IRValue::REGISTER) {
                                    std::string base = baseOf(instr.args[0].name);
                                    if (base.empty() || !isLocalAlloca(base)) return true;
                                }
                            }
                        }
                        break;
                    }
                    case IROp::CALL:
                    case IROp::VIRTUAL_CALL: {
                        for (size_t i = 1; i < instr.args.size(); ++i) {
                            if (instr.args[i].kind != IRValue::REGISTER) continue;
                            const std::string& arg = instr.args[i].name;
                            bool tracked = values_.count(arg);
                            if (!tracked) {
                                std::string b = baseOf(arg);
                                tracked = !b.empty() && locals_.count(b);
                            }
                            if (!tracked) continue;
                            if (isFreeCall(instr)) {
                                freeSites_.push_back({block.get(), ii});  // record for removal
                                continue;
                            }
                            if (calleeParamEscapes(instr, i - 1)) return true;
                        }
                        break;
                    }
                    default:
                        break;
                }
            }
        }
        return false;
    }

    // --- interprocedural summaries (monotone fixpoint) ---

    void computeSummaries(ModuleIR& module) {
        bool changed = true;
        while (changed) {
            changed = false;
            for (auto& func : module.getFunctions()) {
                const auto& params = func->getParams();
                std::set<int> newEsc;
                for (int i = 0; i < (int)params.size(); ++i) {
                    if (valueEscapes(*func, params[i])) newEsc.insert(i);
                }
                if (newEsc != escapingParams_[func->getName()]) {
                    escapingParams_[func->getName()] = newEsc;
                    changed = true;
                }
            }
        }
    }

    // --- intraprocedural rewrite ---

    void analyzeAndRewrite(FunctionIR& func) {
        // Collect free sites discovered during each allocation's analysis.
        std::vector<std::pair<BasicBlock*, size_t>> toRemove;

        for (auto& block : func.getBlocks()) {
            auto& instrs = block->getInstructions();
            for (size_t i = 0; i < instrs.size(); ++i) {
                auto& instr = instrs[i];
                if (!isMallocCall(instr) || !instr.dest ||
                    instr.dest->kind != IRValue::REGISTER) continue;
                if (instr.args.size() < 2 || instr.args[1].kind != IRValue::CONSTANT) continue;

                std::string allocReg = instr.dest->name;
                if (valueEscapes(func, allocReg)) continue;  // stays on heap

                // Stack-eligible: malloc → ALLOCA.
                int size = (int)instr.args[1].constValue;
                instr.op = IROp::ALLOCA;
                instr.args = {IRValue::Constant(size, IRType::I32)};

                // Record the matching free sites for removal.
                for (auto& site : freeSites_) toRemove.push_back(site);
            }
        }

        // Remove free sites (dedup first, then descending index order per block).
        std::set<std::pair<BasicBlock*, size_t>> uniqueSites(toRemove.begin(), toRemove.end());
        std::map<BasicBlock*, std::vector<size_t>> byBlock;
        for (auto& [block, idx] : uniqueSites) byBlock[block].push_back(idx);
        for (auto& [block, idxs] : byBlock) {
            std::sort(idxs.begin(), idxs.end(), std::greater<size_t>());
            auto& instrs = block->getInstructions();
            for (size_t idx : idxs) {
                if (idx < instrs.size()) instrs.erase(instrs.begin() + idx);
            }
        }
    }
};

} // namespace Cider

#endif
