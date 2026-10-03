// opt.cpp — fold, then dead-code elimination.
//
// Folding first exposes more dead code; the structural check first means
// neither pass reads garbage. Each pass preserves the seven rows of IR.md §7,
// and each row below says how.

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
            unsigned __int128 ua = static_cast<unsigned __int128>(a);
            unsigned __int128 ub = static_cast<unsigned __int128>(b);
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
        if (a == static_cast<__int128>(INT64_MIN) && b == -1) {
            return std::nullopt;
        }
        out = instr.op == ir::Op::Div ? a / b : a % b;
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
        if (instr.op == ir::Op::Shl) {
            out = a << static_cast<int>(b);
        } else {
            return static_cast<int64_t>(a >> static_cast<int>(b));
        }
        done = true;
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
                }
                const ir::Terminator& term = block.term;
                if (term.value.has_value()) {
                    used.insert(term.value->id);
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
            bool indexed = false;
            for (const ir::Block& scan : fn.blocks) {
                for (const ir::Instr& seen : scan.body) {
                    if (seen.op == ir::Op::Gep) {
                        indexed = true;
                    }
                }
            }
            std::unordered_set<uint32_t> readSlots;
            for (const ir::Block& scan : fn.blocks) {
                for (const ir::Instr& seen : scan.body) {
                    if (seen.op == ir::Op::Load) {
                        readSlots.insert(seen.addressBase.id);
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
                    bool deadStore = !indexed && instr.op == ir::Op::Store &&
                                     !instr.isVolatile &&
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
    result.folded = foldConsts(module);
    result.removed = deadCode(module);
    return result;
}

}  // namespace opt
}  // namespace cider
