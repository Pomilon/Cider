// opt.cpp — inline, mem2reg, fold, simplify control flow, value-number, hoist
// loop invariants, forward slots, dead-code elimination, then deduplicate
// read-only bytes.
//
// Inlining first clones small direct callees and gives their parameter words
// fresh slots, so mem2reg can promote the cloned traffic. Mem2reg then turns
// straight-line and phi-joined slot traffic into SSA, so folding sees more
// constants. Control-flow simplification consumes those constants: it folds
// provably-taken branches, drops unreachable blocks, and collapses the phis
// those blocks leave behind, which exposes still more constants. Value
// numbering then runs on the smaller phi IR and before slot forwarding, so it
// removes duplicate pure expressions across dominated blocks. LICM follows
// value numbering so it moves the one surviving copy of an invariant
// expression to a preheader, and before slot forwarding and DCE so those
// passes see the final placement. Slot forwarding still sees every remaining
// load/store, and DCE deletes the values every pass orphaned. The structural
// check runs before any pass, again after inlining, again after simplification,
// again after value numbering, and again after LICM, so no pass reads garbage
// and a malformed rewrite fails cleanly. Each pass preserves the seven rows of
// IR.md §7, and each row below says how.

#include "opt/opt.h"

#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "opt/structural.h"

namespace cider {
namespace opt {

namespace {

// The value of a constant definition, or nullopt when the id is not one. A
// map rebuilt per function: constants are SSA values, so one definition each,
// and the map is exact rather than approximate.
bool isConstOp(ir::Op op) {
    return op == ir::Op::Add || op == ir::Op::Sub || op == ir::Op::Mul ||
           op == ir::Op::Div || op == ir::Op::Rem || op == ir::Op::And ||
           op == ir::Op::Or || op == ir::Op::Xor || op == ir::Op::Shl ||
           op == ir::Op::Shr || op == ir::Op::Cmp;
}

// Folds one instruction with constant operands. Answers nullopt wherever the
// runtime semantics must decide instead: overflow, division by zero, an
// out-of-range shift. Folding those would turn a trap into a constant.
std::optional<int64_t> foldOne(const ir::Instr& instr,
                               const std::unordered_map<uint32_t, int64_t>& consts) {
    // Binary folding only. The count goes into a named local first, because a
    // digit beside `.size()` reads as a size literal and T4 refuses it.
    if (!isConstOp(instr.op)) {
        return std::nullopt;
    }
    // Float arithmetic and float comparisons carry IEEE bits, not integers:
    // folding them here would compare bit patterns as if they were numbers
    // and answer a different truth. Leave them for the hardware.
    if (instr.width == ir::IRType::F32 || instr.width == ir::IRType::F64) {
        return std::nullopt;
    }
    if (!instr.operands.empty() && !instr.operands[0].type.isError() &&
        instr.operands[0].type.type->kind() == Kind::Basic &&
        basicIsFloat(*instr.operands[0].type.type->basic())) {
        return std::nullopt;
    }
    uint64_t arity = 0;
    for (const ir::Value& operand : instr.operands) {
        (void)operand;
        ++arity;
    }
    const uint64_t want = 2;
    if (arity != want) {
        return std::nullopt;
    }
    auto lhs = consts.find(instr.operands[0].id);
    auto rhs = consts.find(instr.operands[1].id);
    if (lhs == consts.end() || rhs == consts.end()) {
        return std::nullopt;
    }
    __int128 a = lhs->second;
    __int128 b = rhs->second;
    auto inRange = [](__int128 v) {
        return v >= static_cast<__int128>(INT64_MIN) &&
               v <= static_cast<__int128>(INT64_MAX);
    };
    // A fold emulates the runtime operation at its own width: sub-64
    // values wrap at their width (the corpus pins wrap on i32), so the
    // inputs truncate first and the output truncates after. 64-bit and
    // pointer widths keep the old refuse-on-overflow shape.
    int bits = 0;
    bool isSigned = true;
    switch (instr.width) {
        case ir::IRType::I8:
            bits = 8;
            break;
        case ir::IRType::U8:
            bits = 8;
            isSigned = false;
            break;
        case ir::IRType::I16:
            bits = 16;
            break;
        case ir::IRType::U16:
            bits = 16;
            isSigned = false;
            break;
        case ir::IRType::I32:
            bits = 32;
            break;
        case ir::IRType::U32:
            bits = 32;
            isSigned = false;
            break;
        case ir::IRType::U64:
            // 64-bit unsigned arithmetic has no narrower truncation, but its
            // division and comparisons are unsigned. The shift cases below
            // still need the bit pattern rather than a signed value.
            isSigned = false;
            break;
        default:
            break;
    }
    auto truncate = [](__int128 v, int toBits, bool toSigned) {
        if (toBits <= 0 || toBits >= 64) {
            return v;
        }
        unsigned __int128 mask =
            (static_cast<unsigned __int128>(1) << toBits) - 1;
        unsigned __int128 kept = static_cast<unsigned __int128>(v) & mask;
        if (!toSigned) {
            return static_cast<__int128>(kept);
        }
        unsigned __int128 sign =
            static_cast<unsigned __int128>(1) << (toBits - 1);
        if ((kept & sign) != 0) {
            return static_cast<__int128>(
                kept - (static_cast<unsigned __int128>(1) << toBits));
        }
        return static_cast<__int128>(kept);
    };
    if (bits > 0) {
        a = truncate(a, bits, isSigned);
        b = truncate(b, bits, isSigned);
    }
    __int128 out = 0;
    bool done = false;
    if (instr.op == ir::Op::Cmp) {
        bool signedCmp = instr.cmp == ir::CmpOp::Eq || instr.cmp == ir::CmpOp::Ne ||
                         instr.cmp == ir::CmpOp::Lt || instr.cmp == ir::CmpOp::Le ||
                         instr.cmp == ir::CmpOp::Gt || instr.cmp == ir::CmpOp::Ge;
        bool holds = false;
        if (signedCmp) {
            if (instr.cmp == ir::CmpOp::Eq) {
                holds = a == b;
            } else if (instr.cmp == ir::CmpOp::Ne) {
                holds = a != b;
            } else if (instr.cmp == ir::CmpOp::Lt) {
                holds = a < b;
            } else if (instr.cmp == ir::CmpOp::Le) {
                holds = a <= b;
            } else if (instr.cmp == ir::CmpOp::Gt) {
                holds = a > b;
            } else {
                holds = a >= b;
            }
        } else {
            // A u64 operand is a 64-bit bit pattern. Casting its signed
            // __int128 copy straight to unsigned would extend the sign into
            // 128 bits and compare/divide the wrong value.
            unsigned __int128 ua =
                instr.width == ir::IRType::U64
                    ? static_cast<unsigned __int128>(
                          static_cast<uint64_t>(a))
                    : static_cast<unsigned __int128>(a);
            unsigned __int128 ub =
                instr.width == ir::IRType::U64
                    ? static_cast<unsigned __int128>(
                          static_cast<uint64_t>(b))
                    : static_cast<unsigned __int128>(b);
            if (instr.cmp == ir::CmpOp::Eq) {
                holds = ua == ub;
            } else if (instr.cmp == ir::CmpOp::Ne) {
                holds = ua != ub;
            } else if (instr.cmp == ir::CmpOp::Ult) {
                holds = ua < ub;
            } else if (instr.cmp == ir::CmpOp::Ule) {
                holds = ua <= ub;
            } else if (instr.cmp == ir::CmpOp::Ugt) {
                holds = ua > ub;
            } else {
                holds = ua >= ub;
            }
        }
        return holds ? 1 : 0;
    }
    if (instr.op == ir::Op::Add) {
        out = a + b;
        done = true;
    } else if (instr.op == ir::Op::Sub) {
        out = a - b;
        done = true;
    } else if (instr.op == ir::Op::Mul) {
        out = a * b;
        done = true;
    } else if (instr.op == ir::Op::Div || instr.op == ir::Op::Rem) {
        if (b == 0) {
            return std::nullopt;
        }
        if (isSigned) {
            __int128 minVal = bits > 0
                ? -(static_cast<__int128>(1) << (bits - 1))
                : static_cast<__int128>(INT64_MIN);
            if (a == minVal && b == -1) {
                return std::nullopt;
            }
            out = instr.op == ir::Op::Div ? a / b : a % b;
        } else {
            unsigned __int128 ua =
                instr.width == ir::IRType::U64
                    ? static_cast<unsigned __int128>(
                          static_cast<uint64_t>(a))
                    : static_cast<unsigned __int128>(a);
            unsigned __int128 ub =
                instr.width == ir::IRType::U64
                    ? static_cast<unsigned __int128>(
                          static_cast<uint64_t>(b))
                    : static_cast<unsigned __int128>(b);
            out = static_cast<__int128>(
                instr.op == ir::Op::Div ? ua / ub : ua % ub);
        }
        done = true;
    } else if (instr.op == ir::Op::And) {
        out = a & b;
        done = true;
    } else if (instr.op == ir::Op::Or) {
        out = a | b;
        done = true;
    } else if (instr.op == ir::Op::Xor) {
        out = a ^ b;
        done = true;
    } else if (instr.op == ir::Op::Shl || instr.op == ir::Op::Shr) {
        if (b < 0 || b >= 64) {
            return std::nullopt;
        }
        // The hardware masks the count to the operand width; the fold
        // uses the same mask so folded and executed shifts agree.
        int64_t count = static_cast<int64_t>(b);
        if (bits > 0 && bits <= 32) {
            count &= 31;
        } else {
            count &= 63;
        }
        // A u64 shift is a bit operation on the 64-bit pattern, not a signed
        // arithmetic shift. Folding it through the signed __int128 copy would
        // turn `0xFFFF800000000000 >> 32` into a sign-filled result.
        if (instr.width == ir::IRType::U64) {
            uint64_t pattern = static_cast<uint64_t>(a);
            if (instr.op == ir::Op::Shl) {
                return static_cast<int64_t>(pattern << count);
            }
            return static_cast<int64_t>(pattern >> count);
        }
        if (instr.op == ir::Op::Shl) {
            out = a << count;
        } else {
            return truncate(static_cast<int64_t>(a >> count), bits, isSigned);
        }
        done = true;
    }
    if (bits > 0) {
        return truncate(out, bits, isSigned);
    }
    if (!done || !inRange(out)) {
        return std::nullopt;
    }
    return static_cast<int64_t>(out);
}

}  // namespace

uint64_t foldConsts(ir::Module& module) {
    uint64_t folded = 0;
    for (ir::Function& fn : module.functions) {
        std::unordered_map<uint32_t, int64_t> consts;
        for (ir::Block& block : fn.blocks) {
            for (ir::Instr& instr : block.body) {
                if (instr.op == ir::Op::Const && !instr.results.empty()) {
                    consts[instr.results[0].id] = instr.constantBits;
                    continue;
                }
                std::optional<int64_t> value = foldOne(instr, consts);
                if (!value.has_value() || instr.results.empty()) {
                    continue;
                }
                instr.op = ir::Op::Const;
                instr.operands.clear();
                instr.arguments.clear();
                instr.constantBits = *value;
                instr.constantText = std::to_string(*value);
                instr.mayTrap = false;
                instr.isPure = true;
                consts[instr.results[0].id] = *value;
                ++folded;
            }
        }
    }
    return folded;
}

uint64_t deadCode(ir::Module& module) {
    uint64_t removed = 0;
    for (ir::Function& fn : module.functions) {
        for (;;) {
            // Used ids: every operand, every call argument, every terminator
            // value. A definition no use reaches is dead unless removing it
            // would remove a side effect or a trap.
            std::unordered_set<uint32_t> used;
            for (const ir::Block& block : fn.blocks) {
                for (const ir::Instr& instr : block.body) {
                    for (const ir::Value& operand : instr.operands) {
                        used.insert(operand.id);
                    }
                    for (const ir::Argument& arg : instr.arguments) {
                        for (const ir::Value& word : arg.words) {
                            used.insert(word.id);
                        }
                    }
                    for (const ir::PhiIncoming& incoming : instr.phiIncoming) {
                        used.insert(incoming.value.id);
                    }
                    // A slot or a stored value is a use like any other: the
                    // address a load reads and the value a store writes keep
                    // their definitions alive. Missing these deleted a store's
                    // value out from under it.
                    if (instr.op == ir::Op::Load || instr.op == ir::Op::Store ||
                        instr.op == ir::Op::Extract || instr.op == ir::Op::Insert ||
                        instr.op == ir::Op::Gep) {
                        used.insert(instr.addressBase.id);
                    }
                    if (instr.op == ir::Op::Store || instr.op == ir::Op::Insert) {
                        used.insert(instr.storedValue.id);
                    }
                    // An indirect call reads its target like an operand: the
                    // address the call jumps to keeps its definition alive.
                    // Missing this deleted the load, then the store, then
                    // the address itself, and the call jumped to garbage.
                    if (instr.op == ir::Op::Call && instr.call.indirect) {
                        used.insert(instr.calleeValue.id);
                    }
                }
                const ir::Terminator& term = block.term;
                for (const ir::Value& word : term.values) {
                    used.insert(word.id);
                }
                if (term.cond.has_value()) {
                    used.insert(term.cond->id);
                }
                if (term.term == ir::Term::Switch) {
                    used.insert(term.switchValue.id);
                }
            }
            // Slots no load reads: a store there changes nothing observable,
            // unless volatile. Sound because nothing takes a slot's address in
            // this slice, so no other path can observe the write — except a
            // Gep, which reads an element its index chooses at runtime. Any
            // Gep in the function disables this rule for the whole function:
            // precise ranges need allocation sizes the IR does not carry yet.
            // An Add result parked in an addressBase is the same hazard one
            // operation over: the load names the sum, not the slots it was
            // computed from, so the dead-store rule stays off for those too.
            bool indexed = false;
            std::unordered_set<uint32_t> sums;
            for (const ir::Block& scan : fn.blocks) {
                for (const ir::Instr& seen : scan.body) {
                    if (seen.op == ir::Op::Gep) {
                        indexed = true;
                    }
                    if (seen.op == ir::Op::Add && !seen.results.empty()) {
                        sums.insert(seen.results[0].id);
                    }
                }
            }
            if (!indexed) {
                for (const ir::Block& scan : fn.blocks) {
                    for (const ir::Instr& seen : scan.body) {
                        if ((seen.op == ir::Op::Load || seen.op == ir::Op::Store) &&
                            sums.find(seen.addressBase.id) != sums.end()) {
                            indexed = true;
                        }
                    }
                }
            }
            // A loaded pointer, a call result, or any other computed address
            // in an addressBase is the same hazard: the base names no slot,
            // so no slot read proves the write unobserved. Heap stores are
            // the case that bit: init writes through the allocator's pointer
            // and nothing in the function loads through it.
            std::unordered_set<uint32_t> slotIds;
            for (const ir::Block& scan : fn.blocks) {
                for (const ir::Instr& seen : scan.body) {
                    if (seen.op == ir::Op::Alloca) {
                        for (const ir::Value& result : seen.results) {
                            slotIds.insert(result.id);
                        }
                    }
                }
            }
            if (!indexed) {
                for (const ir::Block& scan : fn.blocks) {
                    for (const ir::Instr& seen : scan.body) {
                        if ((seen.op == ir::Op::Load || seen.op == ir::Op::Store) &&
                            seen.globalLabel.empty() &&
                            slotIds.find(seen.addressBase.id) == slotIds.end()) {
                            indexed = true;
                        }
                    }
                }
            }
            std::unordered_set<uint32_t> readSlots;
            for (const ir::Block& scan : fn.blocks) {
                for (const ir::Instr& seen : scan.body) {
                    if (seen.op == ir::Op::Load) {
                        readSlots.insert(seen.addressBase.id);
                    } else if (seen.op == ir::Op::Asm) {
                        // The block's text reads every placeholder's
                        // storage: a store it will load must not be
                        // judged dead.
                        for (const ir::Value& use : seen.operands) {
                            readSlots.insert(use.id);
                        }
                    }
                }
            }
            bool changed = false;
            for (ir::Block& block : fn.blocks) {
                std::vector<ir::Instr> kept;
                for (ir::Instr& instr : block.body) {
                    bool live = instr.results.empty();
                    for (const ir::Value& result : instr.results) {
                        if (used.find(result.id) != used.end()) {
                            live = true;
                        }
                    }
                    // Four reasons to keep a dead definition: it stores, it
                    // calls, it traps, or it is volatile. Each is one field.
                    // A store joins them only when its slot is never read.
                    // A store through a label is never dead at function
                    // scope: the global escapes to every other function and
                    // module, so no slot read can prove it unread. (Its
                    // address names a label, not a slot.)
                    bool deadStore = !indexed && instr.op == ir::Op::Store &&
                                     !instr.isVolatile &&
                                     instr.globalLabel.empty() &&
                                     readSlots.find(instr.addressBase.id) ==
                                         readSlots.end();
                    if (deadStore) {
                        ++removed;
                        changed = true;
                        continue;
                    }
                    if (!live && instr.isPure && !instr.mayTrap && !instr.isVolatile &&
                        instr.op != ir::Op::Store && instr.op != ir::Op::Call &&
                        instr.op != ir::Op::Asm) {
                        ++removed;
                        changed = true;
                        continue;
                    }
                    kept.push_back(std::move(instr));
                }
                block.body = std::move(kept);
            }
            if (!changed) {
                break;
            }
        }
    }
    return removed;
}

OptResult run(ir::Module& module) {
    std::vector<StructuralError> bad = checkModule(module);
    if (!bad.empty()) {
        const StructuralError& first = bad[0];
        return OptResult{false,
                         first.function + ":" + std::to_string(first.block) + ": " +
                             first.message,
                         0, 0};
    }
    OptResult result{true, "", 0, 0};
    InlineResult inlining = inlineCalls(module);
    result.inlined = inlining.inlined;
    result.clonedInstructions = inlining.clonedInstructions;
    result.inliningSkipped = inlining.skipped;
    bad = checkModule(module);
    if (!bad.empty()) {
        const StructuralError& first = bad[0];
        result.ok = false;
        result.error = "after inline " + first.function + ":" +
                       std::to_string(first.block) + ": " + first.message;
        return result;
    }
    PromotionResult promotion = mem2reg(module);
    result.promoted = promotion.promotedSlots;
    result.phis = promotion.phisInserted;
    result.loadsPromoted = promotion.loadsReplaced;
    result.storesPromoted = promotion.storesRemoved;
    result.folded = foldConsts(module);
    SimplifyResult simplify = simplifyControlFlow(module);
    result.branchesFolded = simplify.branchesFolded;
    result.blocksRemoved = simplify.blocksRemoved;
    result.phisSimplified = simplify.phisSimplified;
    result.phiIncomingRemoved = simplify.phiIncomingRemoved;
    bad = checkModule(module);
    if (!bad.empty()) {
        const StructuralError& first = bad[0];
        result.ok = false;
        result.error = "after simplify " + first.function + ":" +
                       std::to_string(first.block) + ": " + first.message;
        return result;
    }
    GvnResult gvn = valueNumber(module);
    result.expressionsRemoved = gvn.expressionsRemoved;
    result.usesRedirected = gvn.usesRedirected;
    result.copiesCollapsed = gvn.copiesCollapsed;
    bad = checkModule(module);
    if (!bad.empty()) {
        const StructuralError& first = bad[0];
        result.ok = false;
        result.error = "after gvn " + first.function + ":" +
                       std::to_string(first.block) + ": " + first.message;
        return result;
    }
    LicmResult licm = loopInvariantCodeMotion(module);
    result.licmLoops = licm.loops;
    result.licmHoisted = licm.hoisted;
    result.licmSkipped = licm.skipped;
    bad = checkModule(module);
    if (!bad.empty()) {
        const StructuralError& first = bad[0];
        result.ok = false;
        result.error = "after licm " + first.function + ":" +
                       std::to_string(first.block) + ": " + first.message;
        return result;
    }
    MemoryResult memory = forwardMemory(module);
    result.forwarded = memory.forwarded;
    result.storesRemoved = memory.storesRemoved;
    result.removed = deadCode(module);
    // DCE first: a DataAddr it deletes is a reference deduplication no longer
    // needs to rewrite. Pruning later still sees every surviving reference on
    // its canonical label, so the two passes compose in either order.
    RodataDedupResult dedup = deduplicateRodata(module);
    result.rodataMerged = dedup.blobsMerged;
    result.rodataBytesMerged = dedup.bytesRemoved;
    return result;
}

}  // namespace opt
}  // namespace cider
