// sccp.cpp — sparse conditional constant propagation over the phi IR.
//
// A lattice value is Unknown (optimistic top), Const(bits), or Overdefined.
// A branch marks only the edges its condition proves executable; a phi meets
// only over executable incoming edges and is revisited whenever the lattice
// of an incoming value changes. That is what makes a loop-carried constant
// converge instead of being assumed on the first visit: a back edge that is
// still Unknown contributes nothing, but once the latch's value is proved the
// phi is re-evaluated and can move from Const to Overdefined if the two sides
// disagree.
//
// Nothing is rewritten until the lattice/edge fixpoint is stable. Rewrites
// keep the result id, so uses never need redirecting; the only substitution
// is for a phi collapsed after its predecessor edge disappears. All-constant
// arithmetic is owned by foldConsts (which runs earlier in the pipeline):
// this pass only re-evaluates allowed pure ops when a phi-derived constant
// makes an operand provably constant, and mirrors foldConsts' width mask and
// overflow refusals. Cast, Div, Rem, Load, Store, Call, Asm, Trap, floats and
// volatile instructions are always Overdefined. An unreachable block is
// removed only when it holds no trapping, volatile, trap or asm instruction.

#include "opt/opt.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "opt/dominance.h"

namespace cider {
namespace opt {
namespace {

constexpr uint64_t kOneResult = 1;
constexpr uint64_t kOneOperand = 1;
constexpr uint64_t kOneIncoming = 1;
constexpr uint64_t kOneInstr = 1;
constexpr uint64_t kTwoOperands = 2;
constexpr uint64_t kTwo = 2;
constexpr uint64_t kFour = 4;

uint64_t edgeKey(uint32_t from, uint32_t to) {
    return (static_cast<uint64_t>(from) << 32) | static_cast<uint64_t>(to);
}

enum class State { Unknown, Const, Overdefined };

struct Lattice {
    State state = State::Unknown;
    int64_t bits = 0;
};

using LatticeMap = std::unordered_map<uint32_t, Lattice>;

int rank(State state) {
    switch (state) {
        case State::Unknown:      return 0;
        case State::Const:        return 1;
        case State::Overdefined:  return 2;
    }
    return 0;
}

Lattice overdefined() { return Lattice{State::Overdefined, 0}; }
Lattice constantValue(int64_t bits) { return Lattice{State::Const, bits}; }

bool isFloatWidth(ir::IRType width) {
    return width == ir::IRType::F32 || width == ir::IRType::F64;
}

bool isFloatType(const TypeRef& type) {
    return !type.isError() && type.type->kind() == Kind::Basic &&
           basicIsFloat(*type.type->basic());
}

bool isIntegerWidth(ir::IRType width) {
    switch (width) {
        case ir::IRType::I8:
        case ir::IRType::U8:
        case ir::IRType::I16:
        case ir::IRType::U16:
        case ir::IRType::I32:
        case ir::IRType::U32:
        case ir::IRType::I64:
        case ir::IRType::U64:
        case ir::IRType::Bool:
            return true;
        default:
            return false;
    }
}

bool sameShape(const ir::Value& a, const ir::Value& b) {
    return a.width == b.width && a.type == b.type;
}

bool hasForbiddenEffect(const ir::Instr& instr) {
    return instr.mayTrap || instr.isVolatile || instr.op == ir::Op::Trap ||
           instr.op == ir::Op::Asm;
}

bool hasForbiddenEffect(const ir::Block& block) {
    for (const ir::Instr& instr : block.body) {
        if (hasForbiddenEffect(instr)) {
            return true;
        }
    }
    return false;
}

bool foldedOp(ir::Op op) {
    switch (op) {
        case ir::Op::Copy:
        case ir::Op::Phi:
        case ir::Op::Add:
        case ir::Op::Sub:
        case ir::Op::Mul:
        case ir::Op::And:
        case ir::Op::Or:
        case ir::Op::Xor:
        case ir::Op::Shl:
        case ir::Op::Shr:
        case ir::Op::Cmp:
            return true;
        default:
            return false;
    }
}

// Signed predicates read the stored word as int64_t, unsigned predicates
// compare the raw bits. Mirrors compareConstants in simplify.cpp; the
// narrowing/signed-negative widening Cast shapes never reach it because a
// Cast's result is always Overdefined.
std::optional<bool> compareConstants(const ir::Instr& instr, int64_t a,
                                     int64_t b) {
    const uint64_t ua = static_cast<uint64_t>(a);
    const uint64_t ub = static_cast<uint64_t>(b);
    switch (instr.cmp) {
        case ir::CmpOp::Eq:  return a == b;
        case ir::CmpOp::Ne:  return a != b;
        case ir::CmpOp::Lt:  return a < b;
        case ir::CmpOp::Le:  return a <= b;
        case ir::CmpOp::Gt:  return a > b;
        case ir::CmpOp::Ge:  return a >= b;
        case ir::CmpOp::Ult: return ua < ub;
        case ir::CmpOp::Ule: return ua <= ub;
        case ir::CmpOp::Ugt: return ua > ub;
        case ir::CmpOp::Uge: return ua >= ub;
    }
    return std::nullopt;
}

// Mirror foldOne's width mask and overflow refusals for the pure integer ops
// this pass may re-evaluate. Div/Rem are deliberately absent: a trapping
// division is never folded, even with a proved nonzero divisor.
std::optional<int64_t> foldArithmetic(const ir::Instr& instr, int64_t lhs,
                                      int64_t rhs) {
    __int128 a = lhs;
    __int128 b = rhs;
    int bits = 0;
    bool isSigned = true;
    switch (instr.width) {
        case ir::IRType::I8:  bits = 8;  break;
        case ir::IRType::U8:  bits = 8;  isSigned = false; break;
        case ir::IRType::I16: bits = 16; break;
        case ir::IRType::U16: bits = 16; isSigned = false; break;
        case ir::IRType::I32: bits = 32; break;
        case ir::IRType::U32: bits = 32; isSigned = false; break;
        case ir::IRType::U64: isSigned = false; break;
        case ir::IRType::Bool: break;
        default: return std::nullopt;
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
        unsigned __int128 sign = static_cast<unsigned __int128>(1)
                                 << (toBits - 1);
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
    switch (instr.op) {
        case ir::Op::Add: out = a + b; break;
        case ir::Op::Sub: out = a - b; break;
        case ir::Op::Mul: out = a * b; break;
        case ir::Op::And: out = a & b; break;
        case ir::Op::Or:  out = a | b; break;
        case ir::Op::Xor: out = a ^ b; break;
        case ir::Op::Shl:
        case ir::Op::Shr: {
            if (b < 0 || b >= 64) {
                return std::nullopt;
            }
            int64_t count = static_cast<int64_t>(b);
            count &= (bits > 0 && bits <= 32) ? 31 : 63;
            if (instr.width == ir::IRType::U64) {
                const uint64_t pattern = static_cast<uint64_t>(a);
                if (instr.op == ir::Op::Shl) {
                    return static_cast<int64_t>(pattern << count);
                }
                return static_cast<int64_t>(pattern >> count);
            }
            if (instr.op == ir::Op::Shl) {
                out = a << count;
            } else {
                return truncate(static_cast<int64_t>(a >> count), bits,
                                isSigned);
            }
            break;
        }
        default: return std::nullopt;
    }
    if (bits > 0) {
        return static_cast<int64_t>(truncate(out, bits, isSigned));
    }
    if (out < static_cast<__int128>(std::numeric_limits<int64_t>::min()) ||
        out > static_cast<__int128>(std::numeric_limits<int64_t>::max())) {
        return std::nullopt;
    }
    return static_cast<int64_t>(out);
}

// One instruction's lattice value, given the current facts. Unknown means
// "not decided yet"; it is never a reason to fold a dependent overdefined.
Lattice evaluate(const ir::Instr& instr, const LatticeMap& values,
                 const std::unordered_set<uint64_t>& edges,
                 uint32_t blockLabel) {
    if (instr.results.size() != kOneResult) {
        return Lattice{};
    }
    const ir::Value& result = instr.results[0];
    if (result.width == ir::IRType::Void || isFloatWidth(result.width) ||
        isFloatType(result.type) || instr.mayTrap || instr.isVolatile) {
        return overdefined();
    }
    switch (instr.op) {
        case ir::Op::Const:
            return constantValue(instr.constantBits);
        case ir::Op::Copy: {
            if (instr.operands.size() != kOneOperand ||
                !sameShape(instr.operands[0], result)) {
                return overdefined();
            }
            auto found = values.find(instr.operands[0].id);
            return found == values.end() ? Lattice{} : found->second;
        }
        case ir::Op::Phi: {
            if (instr.phiIncoming.empty()) {
                return overdefined();
            }
            bool sawConst = false;
            bool sawUnknown = false;
            int64_t bits = 0;
            for (const ir::PhiIncoming& incoming : instr.phiIncoming) {
                if (edges.find(edgeKey(incoming.block, blockLabel)) ==
                    edges.end()) {
                    continue;
                }
                if (!sameShape(incoming.value, result)) {
                    return overdefined();
                }
                auto found = values.find(incoming.value.id);
                if (found == values.end() ||
                    found->second.state == State::Unknown) {
                    // Top is the identity of meet: the incoming may still
                    // become a constant, so do not decide the phi yet, but
                    // do not poison it either.
                    sawUnknown = true;
                    continue;
                }
                if (found->second.state == State::Overdefined) {
                    return overdefined();
                }
                if (!sawConst) {
                    bits = found->second.bits;
                    sawConst = true;
                } else if (bits != found->second.bits) {
                    return overdefined();
                }
            }
            (void)sawUnknown;  // documented above; Unknown stays optimistic
            return sawConst ? constantValue(bits) : Lattice{};
        }
        case ir::Op::Add:
        case ir::Op::Sub:
        case ir::Op::Mul:
        case ir::Op::And:
        case ir::Op::Or:
        case ir::Op::Xor:
        case ir::Op::Shl:
        case ir::Op::Shr: {
            if (instr.operands.size() != kTwoOperands ||
                !isIntegerWidth(instr.width) ||
                !sameShape(instr.operands[0], result) ||
                !sameShape(instr.operands[1], result)) {
                return overdefined();
            }
            auto lhs = values.find(instr.operands[0].id);
            auto rhs = values.find(instr.operands[1].id);
            if (lhs == values.end() || rhs == values.end() ||
                lhs->second.state == State::Unknown ||
                rhs->second.state == State::Unknown) {
                return Lattice{};
            }
            if (lhs->second.state == State::Overdefined ||
                rhs->second.state == State::Overdefined) {
                return overdefined();
            }
            auto folded = foldArithmetic(instr, lhs->second.bits,
                                         rhs->second.bits);
            return folded.has_value() ? constantValue(*folded) : overdefined();
        }
        case ir::Op::Cmp: {
            if (instr.operands.size() != kTwoOperands ||
                !isIntegerWidth(instr.operands[0].width) ||
                !isIntegerWidth(instr.operands[1].width) ||
                isFloatType(instr.operands[0].type) ||
                isFloatType(instr.operands[1].type) ||
                !sameShape(instr.operands[0], instr.operands[1])) {
                return overdefined();
            }
            auto lhs = values.find(instr.operands[0].id);
            auto rhs = values.find(instr.operands[1].id);
            if (lhs == values.end() || rhs == values.end() ||
                lhs->second.state == State::Unknown ||
                rhs->second.state == State::Unknown) {
                return Lattice{};
            }
            if (lhs->second.state == State::Overdefined ||
                rhs->second.state == State::Overdefined) {
                return overdefined();
            }
            auto holds = compareConstants(instr, lhs->second.bits,
                                          rhs->second.bits);
            return holds.has_value() ? constantValue(*holds ? 1 : 0)
                                     : overdefined();
        }
        default:
            return overdefined();
    }
}

struct Analysis {
    LatticeMap values;
    std::unordered_set<uint64_t> edges;
    std::unordered_set<uint32_t> executable;
};

void successorsOf(const ir::Terminator& term, std::vector<uint32_t>& out) {
    switch (term.term) {
        case ir::Term::Br:
            out.push_back(term.target);
            break;
        case ir::Term::CondBr:
            out.push_back(term.thenTarget);
            out.push_back(term.elseTarget);
            break;
        case ir::Term::Switch:
            for (const ir::Case& arm : term.cases) {
                out.push_back(arm.target);
            }
            out.push_back(term.defaultTarget);
            break;
        case ir::Term::Ret:
        case ir::Term::Unreachable:
        case ir::Term::Trap:
            break;
    }
}

void seed(ir::Function& fn, Analysis& analysis) {
    for (const ir::Value& param : fn.params) {
        analysis.values[param.id] = overdefined();
    }
    for (const ir::Block& block : fn.blocks) {
        for (const ir::Instr& instr : block.body) {
            if (instr.op == ir::Op::Const && instr.results.size() == kOneResult) {
                analysis.values[instr.results[0].id] =
                    constantValue(instr.constantBits);
            }
        }
    }
    analysis.executable.insert(fn.blocks.front().label);
}

// Mark every edge a proved terminator can take. Edges are monotone: once an
// edge is executable it stays so, which is exactly what a condition that
// later becomes Overdefined needs (both edges are then present).
bool markEdges(const ir::Function& fn, Analysis& analysis) {
    bool changed = false;
    auto addEdge = [&](uint32_t from, uint32_t to) {
        if (analysis.edges.insert(edgeKey(from, to)).second) {
            changed = true;
        }
        if (analysis.executable.insert(to).second) {
            changed = true;
        }
    };
    auto latticeFor = [&](uint32_t id) -> const Lattice* {
        auto found = analysis.values.find(id);
        return found == analysis.values.end() ? nullptr : &found->second;
    };
    for (const ir::Block& block : fn.blocks) {
        if (analysis.executable.find(block.label) ==
            analysis.executable.end()) {
            continue;
        }
        const ir::Terminator& term = block.term;
        if (term.term == ir::Term::Br) {
            addEdge(block.label, term.target);
        } else if (term.term == ir::Term::CondBr && term.cond.has_value()) {
            const Lattice* cond = latticeFor(term.cond->id);
            if (cond != nullptr && cond->state == State::Const) {
                addEdge(block.label,
                        cond->bits != 0 ? term.thenTarget : term.elseTarget);
            } else if (cond != nullptr && cond->state == State::Overdefined) {
                addEdge(block.label, term.thenTarget);
                addEdge(block.label, term.elseTarget);
            }
        } else if (term.term == ir::Term::Switch) {
            const Lattice* value = latticeFor(term.switchValue.id);
            if (value == nullptr || value->state == State::Unknown) {
                continue;
            }
            if (value->state == State::Overdefined) {
                std::vector<uint32_t> all;
                successorsOf(term, all);
                for (uint32_t target : all) {
                    addEdge(block.label, target);
                }
                continue;
            }
            bool decided = false;
            for (const ir::Case& arm : term.cases) {
                if (arm.value.width != term.switchValue.width) {
                    // A width-changing compare has no rule here: assume any
                    // case may match.
                    std::vector<uint32_t> all;
                    successorsOf(term, all);
                    for (uint32_t target : all) {
                        addEdge(block.label, target);
                    }
                    decided = true;
                    break;
                }
                const Lattice* label = latticeFor(arm.value.id);
                if (label == nullptr || label->state != State::Const) {
                    // An unproved earlier case could match; assume every
                    // remaining case and the default are executable.
                    std::vector<uint32_t> rest;
                    for (const ir::Case& later : term.cases) {
                        rest.push_back(later.target);
                    }
                    rest.push_back(term.defaultTarget);
                    for (uint32_t target : rest) {
                        addEdge(block.label, target);
                    }
                    decided = true;
                    break;
                }
                if (label->bits == value->bits) {
                    addEdge(block.label, arm.target);
                    decided = true;
                    break;
                }
            }
            if (!decided) {
                addEdge(block.label, term.defaultTarget);
            }
        }
    }
    return changed;
}

// The bounded lattice/edge fixpoint. Each round either promotes a lattice
// value or adds an edge, so the instruction/edge-count budget is sufficient.
void solve(ir::Function& fn, Analysis& analysis) {
    uint64_t values = 0;
    uint64_t edges = 0;
    for (const ir::Block& block : fn.blocks) {
        values += static_cast<uint64_t>(block.body.size());
        for (const ir::Instr& instr : block.body) {
            (void)instr;
            ++edges;
        }
    }
    const uint64_t budget =
        kTwo * values + edges + fn.blocks.size() + kFour;
    for (uint64_t round = 0; round <= budget; ++round) {
        bool changed = markEdges(fn, analysis);
        for (const ir::Block& block : fn.blocks) {
            if (analysis.executable.find(block.label) ==
                analysis.executable.end()) {
                continue;
            }
            for (const ir::Instr& instr : block.body) {
                if (instr.results.size() != kOneResult) {
                    continue;
                }
                const uint32_t id = instr.results[0].id;
                const Lattice next =
                    evaluate(instr, analysis.values, analysis.edges,
                             block.label);
                auto found = analysis.values.find(id);
                if (found == analysis.values.end()) {
                    if (next.state != State::Unknown) {
                        analysis.values.emplace(id, next);
                        changed = true;
                    }
                } else if (rank(next.state) > rank(found->second.state)) {
                    found->second = next;
                    changed = true;
                }
            }
        }
        if (!changed) {
            return;
        }
    }
}

// Rewrite every proved result in an executable block to a Const carrying the
// same result id, width, type and overflow mode, built from an empty Instr so
// no stale payload survives. Uses keep naming the id, so nothing is
// redirected here.
bool rewriteValues(ir::Function& fn, const Analysis& analysis,
                   SccpResult& result) {
    bool changed = false;
    for (ir::Block& block : fn.blocks) {
        if (analysis.executable.find(block.label) ==
            analysis.executable.end()) {
            continue;
        }
        for (ir::Instr& instr : block.body) {
            if (instr.results.size() != kOneResult || instr.op == ir::Op::Const ||
                !foldedOp(instr.op) || hasForbiddenEffect(instr)) {
                continue;
            }
            auto found = analysis.values.find(instr.results[0].id);
            if (found == analysis.values.end() ||
                found->second.state != State::Const) {
                continue;
            }
            const ir::Value saved = instr.results[0];
            const Position loc = instr.loc;
            const ir::OverflowMode overflow = instr.overflow;
            const int64_t bits = found->second.bits;
            instr = ir::Instr{};
            instr.op = ir::Op::Const;
            instr.width = saved.width;
            instr.type = saved.type;
            instr.loc = loc;
            instr.overflow = overflow;
            instr.isPure = true;
            instr.constantBits = bits;
            instr.constantText = std::to_string(bits);
            instr.results.push_back(saved);
            ++result.constantsReplaced;
            changed = true;
        }
    }
    return changed;
}

bool rewriteTerminators(ir::Function& fn, const Analysis& analysis,
                        SccpResult& result) {
    bool changed = false;
    for (ir::Block& block : fn.blocks) {
        if (analysis.executable.find(block.label) ==
            analysis.executable.end()) {
            continue;
        }
        ir::Terminator& term = block.term;
        uint32_t target = 0;
        bool fold = false;
        if (term.term == ir::Term::CondBr && term.cond.has_value()) {
            auto found = analysis.values.find(term.cond->id);
            if (found != analysis.values.end() &&
                found->second.state == State::Const) {
                target = found->second.bits != 0 ? term.thenTarget
                                                 : term.elseTarget;
                fold = true;
            }
        } else if (term.term == ir::Term::Switch) {
            auto value = analysis.values.find(term.switchValue.id);
            if (value != analysis.values.end() &&
                value->second.state == State::Const) {
                // First matching case wins; an unproved earlier case refuses
                // the fold, exactly as simplifyControlFlow does.
                target = term.defaultTarget;
                fold = true;
                for (const ir::Case& arm : term.cases) {
                    if (arm.value.width != term.switchValue.width) {
                        fold = false;
                        break;
                    }
                    auto found = analysis.values.find(arm.value.id);
                    if (found == analysis.values.end() ||
                        found->second.state != State::Const) {
                        fold = false;
                        break;
                    }
                    if (found->second.bits == value->second.bits) {
                        target = arm.target;
                        break;
                    }
                }
            }
        }
        if (!fold) {
            continue;
        }
        const Position loc = term.loc;
        term = ir::Terminator{};
        term.term = ir::Term::Br;
        term.target = target;
        term.loc = loc;
        ++result.branchesFolded;
        changed = true;
    }
    return changed;
}

// Use-field set shared with gvn.cpp's replaceUses. Only collapsed phis need
// redirection; a Const rewrite keeps the result id.
bool namesBase(ir::Op op) {
    return op == ir::Op::Load || op == ir::Op::Store ||
           op == ir::Op::Extract || op == ir::Op::Insert ||
           op == ir::Op::Gep;
}

void replaceUses(ir::Function& fn,
                 const std::unordered_map<uint32_t, ir::Value>& subst) {
    if (subst.empty()) {
        return;
    }
    auto remap = [&](ir::Value& value) {
        auto found = subst.find(value.id);
        if (found != subst.end()) {
            value = found->second;
        }
    };
    for (ir::Block& block : fn.blocks) {
        for (ir::Instr& instr : block.body) {
            for (ir::Value& operand : instr.operands) {
                remap(operand);
            }
            for (ir::Argument& arg : instr.arguments) {
                for (ir::Value& word : arg.words) {
                    remap(word);
                }
            }
            for (ir::PhiIncoming& incoming : instr.phiIncoming) {
                remap(incoming.value);
            }
            if (namesBase(instr.op) && instr.globalLabel.empty()) {
                remap(instr.addressBase);
            }
            if (instr.op == ir::Op::Store || instr.op == ir::Op::Insert) {
                remap(instr.storedValue);
            }
            if (instr.op == ir::Op::Call && instr.call.indirect) {
                remap(instr.calleeValue);
            }
        }
        ir::Terminator& term = block.term;
        for (ir::Value& word : term.values) {
            remap(word);
        }
        if (term.cond.has_value()) {
            remap(*term.cond);
        }
        if (term.term == ir::Term::Switch) {
            remap(term.switchValue);
            for (ir::Case& arm : term.cases) {
                remap(arm.value);
            }
        }
    }
}

// Collapse single-incoming phis as simplifyControlFlow does. Cycles are
// refused rather than deleting a definition still in use.
uint64_t simplifySingleIncomingPhis(ir::Function& fn) {
    if (fn.blocks.empty()) {
        return 0;
    }
    const uint32_t entry = fn.blocks.front().label;
    std::unordered_map<uint32_t, ir::Value> raw;
    for (const ir::Block& block : fn.blocks) {
        if (block.label == entry) {
            continue;
        }
        for (const ir::Instr& instr : block.body) {
            if (instr.op != ir::Op::Phi ||
                instr.results.size() != kOneResult ||
                instr.phiIncoming.size() != kOneIncoming) {
                continue;
            }
            const ir::Value& result = instr.results[0];
            const ir::Value& incoming = instr.phiIncoming[0].value;
            if (incoming.id == result.id ||
                incoming.width != result.width ||
                !(incoming.type == result.type)) {
                continue;
            }
            raw.emplace(result.id, incoming);
        }
    }
    if (raw.empty()) {
        return 0;
    }
    std::unordered_map<uint32_t, ir::Value> subst;
    for (const auto& item : raw) {
        const uint32_t id = item.first;
        ir::Value resolved = item.second;
        std::unordered_set<uint32_t> seen;
        seen.insert(id);
        bool cycle = false;
        for (;;) {
            auto next = raw.find(resolved.id);
            if (next == raw.end()) {
                break;
            }
            if (!seen.insert(resolved.id).second) {
                cycle = true;
                break;
            }
            resolved = next->second;
        }
        if (!cycle && resolved.id != id) {
            subst.emplace(id, resolved);
        }
    }
    if (subst.empty()) {
        return 0;
    }
    replaceUses(fn, subst);
    uint64_t removed = 0;
    for (ir::Block& block : fn.blocks) {
        std::vector<ir::Instr> kept;
        kept.reserve(block.body.size());
        for (ir::Instr& instr : block.body) {
            const bool drop =
                instr.op == ir::Op::Phi &&
                instr.results.size() == kOneResult &&
                subst.find(instr.results[0].id) != subst.end();
            if (drop) {
                ++removed;
            } else {
                kept.push_back(std::move(instr));
            }
        }
        block.body = std::move(kept);
    }
    return removed;
}

// Remove only blocks outside the executable region. A non-executable block
// with a forbidden effect is kept, and its CFG successors are kept too so a
// surviving phi never loses an incoming for a predecessor that still exists.
bool cleanup(ir::Function& fn, const Analysis& analysis, SccpResult& result) {
    std::unordered_map<uint32_t, const ir::Block*> blockAt;
    for (const ir::Block& block : fn.blocks) {
        blockAt[block.label] = &block;
    }
    std::unordered_set<uint32_t> roots = analysis.executable;
    for (const ir::Block& block : fn.blocks) {
        if (analysis.executable.find(block.label) ==
                analysis.executable.end() &&
            hasForbiddenEffect(block)) {
            roots.insert(block.label);
        }
    }
    std::unordered_set<uint32_t> kept;
    std::vector<uint32_t> work(roots.begin(), roots.end());
    while (!work.empty()) {
        const uint32_t label = work.back();
        work.pop_back();
        if (!kept.insert(label).second) {
            continue;
        }
        auto at = blockAt.find(label);
        if (at == blockAt.end()) {
            continue;
        }
        std::vector<uint32_t> next;
        successorsOf(at->second->term, next);
        for (uint32_t target : next) {
            work.push_back(target);
        }
    }
    bool changed = false;
    std::vector<ir::Block> survivors;
    survivors.reserve(fn.blocks.size());
    for (ir::Block& block : fn.blocks) {
        if (kept.find(block.label) == kept.end()) {
            ++result.blocksRemoved;
            changed = true;
        } else {
            survivors.push_back(std::move(block));
        }
    }
    fn.blocks = std::move(survivors);

    // Rebuild predecessor sets from the surviving terminators so a phi loses
    // the incoming for an edge that a fold removed, not only for a whole
    // block that was deleted. Entries are matched by label, not position.
    const DominatorInfo dominators = computeDominators(fn);
    for (ir::Block& block : fn.blocks) {
        std::unordered_set<uint32_t> predecessors;
        auto found = dominators.predecessors.find(block.label);
        if (found != dominators.predecessors.end()) {
            for (uint32_t pred : found->second) {
                predecessors.insert(pred);
            }
        }
        for (ir::Instr& instr : block.body) {
            if (instr.op != ir::Op::Phi) {
                continue;
            }
            std::vector<ir::PhiIncoming> pruned;
            pruned.reserve(instr.phiIncoming.size());
            for (const ir::PhiIncoming& incoming : instr.phiIncoming) {
                if (predecessors.find(incoming.block) != predecessors.end()) {
                    pruned.push_back(incoming);
                } else {
                    changed = true;
                }
            }
            instr.phiIncoming = std::move(pruned);
        }
    }
    const uint64_t simplified = simplifySingleIncomingPhis(fn);
    result.phisSimplified += simplified;
    if (simplified != 0) {
        changed = true;
    }
    return changed;
}

void add(SccpResult& total, const SccpResult& round) {
    total.constantsReplaced += round.constantsReplaced;
    total.branchesFolded += round.branchesFolded;
    total.blocksRemoved += round.blocksRemoved;
    total.phisSimplified += round.phisSimplified;
}

}  // namespace

SccpResult sccp(ir::Module& module) {
    SccpResult total;
    for (ir::Function& fn : module.functions) {
        if (fn.blocks.empty()) {
            continue;
        }
        uint64_t instructions = 0;
        for (const ir::Block& block : fn.blocks) {
            instructions += static_cast<uint64_t>(block.body.size()) + kOneInstr;
        }
        const uint64_t budget = instructions + fn.blocks.size() + kFour;
        for (uint64_t iteration = 0; iteration < budget; ++iteration) {
            Analysis analysis;
            seed(fn, analysis);
            solve(fn, analysis);
            SccpResult round;
            bool changed = rewriteValues(fn, analysis, round);
            changed = rewriteTerminators(fn, analysis, round) || changed;
            changed = cleanup(fn, analysis, round) || changed;
            add(total, round);
            if (!changed) {
                break;
            }
        }
    }
    return total;
}

}  // namespace opt
}  // namespace cider
