#pragma once

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ir.h"

namespace cider {

// Escape analysis: converts non-escaping malloc calls into stack ALLOCAs and
// removes the matching free calls. The interprocedural summaries (which
// parameters escape) are computed as a monotone fixpoint; every analysis is
// conservative (unknown functions and indirect calls escape).
class EscapeAnalysis {
public:
    static constexpr int kMaxStackAllocSize = 1024 * 1024;  // 1 MiB cap

    void run(ModuleIR& module) {
        for (auto& func : module.getFunctions()) {
            escapingParams_[func->getName()] = {};
        }
        computeSummaries(module);
        zeroBuffers_.clear();
        zeroFillTempCount_ = 0;
        for (auto& func : module.getFunctions()) {
            analyzeAndRewrite(module, *func);
        }
    }

private:
    // func name -> set of param indices that escape
    std::map<std::string, std::set<int>> escapingParams_;

    // CIDER-BUG-221: a promoted block must present the same BYTES the heap
    // block presented, not just the same address class. Shared zero buffers
    // for the fill (size -> label) and the temp-name counter for the MOV that
    // materializes each buffer's address. Both reset per run().
    std::map<int, std::string> zeroBuffers_;
    int zeroFillTempCount_ = 0;

    // Per-analysis state (rebuilt per call):
    std::map<std::string, const IRInstruction*> defs_;  // reg -> defining instruction
    std::set<std::string> values_;                       // regs holding the tracked pointer
    std::set<std::string> locals_;                       // alloca slots holding the tracked pointer
    std::vector<std::pair<BasicBlock*, size_t>> freeSites_;  // (block, instr index) of call free
    bool recordFreeSites_ = false;

    // CID-020/021 (spec §5): the allocator may live in a module, in which
    // case its IR name is mangled (`mod_std__alloc__malloc`). Recognize the
    // bare names plus any `mod_...__malloc` / `mod_...__free` spelling, so
    // escape analysis keeps firing for imported allocators. A main-file
    // function would have to deliberately mimic the `mod_` prefix with a
    // `__` separator to collide (same rationale as module_scope.h: the
    // `_u` escape keeps real manglings unambiguous, and neither "malloc"
    // nor "free" contains an underscore).
    static bool isHeapCall(const std::string& name, const char* base) {
        if (name == base) return true;
        const std::string prefix = "mod_";
        const std::string suffix = std::string("__") + base;
        if (name.size() <= prefix.size() + suffix.size()) return false;
        if (name.compare(0, prefix.size(), prefix) != 0) return false;
        if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
        return true;
    }

    static bool isMallocCall(const IRInstruction& instr) {
        return instr.op == IROp::CALL && !instr.args.empty() &&
               instr.args[0].kind == IRValue::LABEL && isHeapCall(instr.args[0].name, "malloc");
    }

    static bool isFreeCall(const IRInstruction& instr) {
        return instr.op == IROp::CALL && !instr.args.empty() &&
               instr.args[0].kind == IRValue::LABEL && isHeapCall(instr.args[0].name, "free");
    }

    // Does the callee's param at 0-based position `paramIdx` escape?
    bool calleeParamEscapes(const IRInstruction& call, size_t paramIdx) {
        if (call.args.empty() || call.args[0].kind != IRValue::LABEL) return true;
        const std::string& callee = call.args[0].name;
        if (isHeapCall(callee, "free")) return false;
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

    // Propagates the tracked pointer through MOV/PHI/CVT/LOAD and taints local
    // alloca slots that store it. Returns true if any escape trigger fires.
    bool valueEscapes(FunctionIR& func, const std::string& rootReg) {
        buildDefs(func);
        values_.clear();
        locals_.clear();
        freeSites_.clear();
        values_.insert(rootReg);

        bool changed = true;
        while (changed) {
            changed = false;
            for (auto& block : func.getBlocks()) {
                for (auto& instr : block->getInstructions()) {
                    if (instr.dest && instr.dest->kind == IRValue::REGISTER) {
                        const std::string& dst = instr.dest->name;
                        if (values_.count(dst)) continue;
                        if (instr.op == IROp::MOV || instr.op == IROp::CVT_ITOF ||
                            instr.op == IROp::CVT_FTOI || instr.op == IROp::ADD) {
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
                            } else if (base.empty() || !isLocalAlloca(base)) {
                                // Storing the tracked pointer into the heap
                                // (a loaded field, a global, an address that
                                // isn't a local slot) makes it observable
                                // beyond the function — it escapes.
                                return true;
                            }
                        }
                    }
                    if (instr.op == IROp::MEMCPY && instr.args.size() >= 2 &&
                        instr.args[1].kind == IRValue::REGISTER) {
                        std::string srcBase = baseOf(instr.args[1].name);
                        std::string dstBase =
                            (instr.args[0].kind == IRValue::REGISTER) ? baseOf(instr.args[0].name) : "";
                        bool srcTainted = !srcBase.empty() && locals_.count(srcBase);
                        bool dstLocal = !dstBase.empty() && isLocalAlloca(dstBase);
                        if (srcTainted && !dstLocal) {
                            return true;
                        }
                        if (srcTainted && dstLocal && !locals_.count(dstBase)) {
                            locals_.insert(dstBase);
                            changed = true;
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
                    case IROp::CMP_EQ:
                    case IROp::CMP_NE: {
                        // Pointer identity is observable: comparing a tracked
                        // pointer against ANOTHER pointer (a freed-and-
                        // reallocated block's address, a second allocation)
                        // can only be answered by the real heap — stack slots
                        // would change the result (no reuse, distinct
                        // addresses). Null checks (vs a constant) are safe.
                        for (size_t ai = 0; ai < instr.args.size(); ++ai) {
                            const auto& arg = instr.args[ai];
                            if (arg.kind != IRValue::REGISTER) continue;
                            bool tracked = values_.count(arg.name);
                            if (!tracked) {
                                std::string b = baseOf(arg.name);
                                tracked = !b.empty() && locals_.count(b);
                            }
                            if (!tracked) continue;
                            bool otherIsPointer = false;
                            for (size_t aj = 0; aj < instr.args.size(); ++aj) {
                                if (aj == ai) continue;
                                const auto& other = instr.args[aj];
                                if (other.kind == IRValue::REGISTER) {
                                    otherIsPointer = true;
                                    break;
                                }
                                if (other.kind == IRValue::LABEL) {
                                    otherIsPointer = true;
                                    break;
                                }
                            }
                            if (otherIsPointer) return true;
                        }
                        break;
                    }
                    case IROp::CALL:
                    case IROp::VIRTUAL_CALL: {
                        size_t startIdx = (instr.op == IROp::VIRTUAL_CALL) ? 0 : 1;
                        for (size_t i = startIdx; i < instr.args.size(); ++i) {
                            if (instr.args[i].kind != IRValue::REGISTER) continue;
                            const std::string& arg = instr.args[i].name;
                            bool tracked = values_.count(arg);
                            if (!tracked) {
                                std::string b = baseOf(arg);
                                tracked = !b.empty() && locals_.count(b);
                            }
                            if (!tracked) continue;
                            if (isFreeCall(instr)) {
                                if (recordFreeSites_) {
                                    freeSites_.push_back({block.get(), ii});
                                    continue;
                                }
                                return true;
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

    void computeSummaries(ModuleIR& module) {
        bool changed = true;
        while (changed) {
            changed = false;
            for (auto& func : module.getFunctions()) {
                const auto& params = func->getParams();
                std::set<int> newEsc;
                for (int i = 0; i < (int)params.size(); ++i) {
                    recordFreeSites_ = false;
                    if (valueEscapes(*func, params[i])) newEsc.insert(i);
                }
                if (newEsc != escapingParams_[func->getName()]) {
                    escapingParams_[func->getName()] = newEsc;
                    changed = true;
                }
            }
        }
    }

    // CIDER-BUG-221: a lazily created static block of `size` zero bytes, the
    // same trick ir_gen's emitZeroInstance uses for `new C`. Native emits it
    // in .bss (zero at load by definition) and the VM zero-fills global
    // storage, so the copy source reads 0 on every backend. One block per
    // distinct size keeps the data small; the label has no source path, so
    // codegen keeps it a local symbol and separate-compilation objects cannot
    // collide on it (see codegen's global loop). The prefix differs from
    // ir_gen's `_cider_zero_` so one module never defines the same label twice.
    std::string zeroBufferLabel(ModuleIR& module, int size) {
        auto it = zeroBuffers_.find(size);
        if (it == zeroBuffers_.end()) {
            std::string label = "_cider_zeroblock_" + std::to_string(size);
            module.addGlobalVar(label, IRType::U8, 0, 0, size, false);
            it = zeroBuffers_.emplace(size, label).first;
        }
        return it->second;
    }

    // CIDER-BUG-221: a promoted malloc must hand the program the same bytes the
    // heap block did. CIDER-BUG-215 made "fresh memory reads 0" the language's
    // explicit policy on all three backends, so a bare ALLOCA silently replaced
    // that guarantee with whatever the native stack happened to hold -- the VM
    // zeroes its frame region (CIDER-BUG-115) and therefore kept agreeing with
    // the heap, while native served frame residue. CIDER-BUG-067 already zeroes
    // the `new` path for exactly this reason; the fill belongs here too, and it
    // is emitted right after the ALLOCA so the slot reads 0 from its first use.
    // MOV/MEMCPY are understood by every backend and by every pass, so no new
    // IR op is needed.
    void insertZeroFill(ModuleIR& module, std::vector<IRInstruction>& instrs,
                        size_t at, const std::string& allocReg, int slotSize) {
        if (at > instrs.size() || slotSize <= 0) return;
        IRValue src = IRValue::Register("ciderzerofill" + std::to_string(zeroFillTempCount_++),
                                        IRType::PTR);
        instrs.insert(instrs.begin() + at,
                      IRInstruction{IROp::MOV, src, {IRValue::Label(zeroBufferLabel(module, slotSize))}});
        instrs.insert(instrs.begin() + at + 1,
                      IRInstruction{IROp::MEMCPY, std::nullopt,
                                    {IRValue::Register(allocReg, IRType::PTR), src,
                                     IRValue::Constant(slotSize, IRType::I32)}});
    }

    // A pending rewrite of one block instruction. The index is in the space
    // the block has while the pass walks it: converting a CALL to an ALLOCA
    // rewrites the instruction in place, and nothing is inserted or erased
    // until every block has been walked, so one space covers the whole block.
    struct Edit {
        size_t index;
        bool insertFill;   // true: MOV + MEMCPY after `index`
        int slotSize;      // fill only: bytes to zero
        std::string allocReg;
    };

    void analyzeAndRewrite(ModuleIR& module, FunctionIR& func) {
        std::map<BasicBlock*, std::vector<Edit>> edits;

        for (auto& block : func.getBlocks()) {
            auto& instrs = block->getInstructions();
            for (size_t i = 0; i < instrs.size(); ++i) {
                auto& instr = instrs[i];
                if (!isMallocCall(instr) || !instr.dest ||
                    instr.dest->kind != IRValue::REGISTER) continue;
                if (instr.args.size() < 2 || instr.args[1].kind != IRValue::CONSTANT) continue;

                std::string allocReg = instr.dest->name;
                recordFreeSites_ = true;
                if (valueEscapes(func, allocReg)) continue;  // stays on heap

                // Stack-eligible: malloc -> ALLOCA.
                // Pad the size: the backend emits a 64-bit store for every
                // field, so a 4-byte int field at the tail of an N-byte object
                // writes [N-4, N+4) — 4 bytes past the object. Heap chunks
                // tolerate this (allocator slack); packed stack slots don't,
                // and the overrun would clobber the adjacent local slot.
                // Large allocations stay on the heap: a multi-megabyte frame
                // risks the stack guard and gains nothing. A negative size has
                // no stack image at all, so it stays on the heap too.
                int size = (int)instr.args[1].intValue;
                if (size > kMaxStackAllocSize || size < 0) continue;
                // CIDER-BUG-221: the padded slot is what the fill covers, so
                // the 4 bytes of slack an over-wide tail store can touch are
                // zero too and the slot's whole window is deterministic.
                int slotSize = size + 8;
                instr.op = IROp::ALLOCA;
                instr.args = {IRValue::Constant(slotSize, IRType::I32)};

                if (slotSize > 0) {
                    edits[block.get()].push_back({i, true, slotSize, allocReg});
                }
                for (auto& site : freeSites_) {
                    edits[site.first].push_back({site.second, false, 0, ""});
                }
            }
        }

        // Apply each block's edits from the highest index down. Descending
        // order is what keeps the recorded indices valid: the edits applied so
        // far all sit ABOVE the current one, so none of them has shifted it,
        // and everything still to be applied sits below it and cannot shift it
        // either. A `free` index is erased at most once even if two promotions
        // recorded the same site.
        for (auto& [block, blockEdits] : edits) {
            auto& instrs = block->getInstructions();
            std::sort(blockEdits.begin(), blockEdits.end(),
                      [](const Edit& a, const Edit& b) { return a.index > b.index; });
            std::set<size_t> erased;
            for (const auto& e : blockEdits) {
                if (e.index >= instrs.size()) continue;
                if (e.insertFill) {
                    insertZeroFill(module, instrs, e.index + 1, e.allocReg, e.slotSize);
                } else if (erased.insert(e.index).second) {
                    instrs.erase(instrs.begin() + e.index);
                }
            }
        }
    }
};

}  // namespace cider
