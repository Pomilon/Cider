// regalloc.cpp — global liveness + local linear scan over the lowered IR.
//
// The input is the lowered, non-phi IR codegen receives. The pass has two
// cooperating halves:
//
// Global (cross-block) allocation. A value is a global candidate only when
// all of these hold:
//
//   - it has exactly one definition in the whole function (phi-lowered ids
//     have one definition per incoming edge and are excluded);
//   - at least one use is in a different block from its definition;
//   - its result is a single machine word and not a float;
//   - neither it nor its producer is one of the instructions this slice
//     refuses to re-home (division, variable shifts, byte multiply, calls'
//     argument words, inline asm, volatile accesses, Alloca, phi).
//
// Liveness is a standard may-analysis over the CFG (live-in/live-out fixpoint)
// and interference comes from pairwise cliques of the live values at every
// instruction and terminator. That models branch merges, loops, back edges and
// values that are live through a block without being named there. A value is
// marked barrier-crossing when it is live after a call, asm, trap or volatile
// instruction it did not just define; barrier-crossing values may only use a
// callee-saved home.
//
// Local allocation. Values wholly inside one block keep exactly the old
// block-local linear scan. It runs over the same candidate rules, skips ids
// the global pass homed, and treats every register a global home occupies in
// the block as reserved for the whole block. That keeps the old register
// choices for local values while making them safe beside global homes.
//
// Refusal is always "keep the stack slot", never a new allocation decision.
// That is what makes the allocator safe to add after the fact: the old slot
// path remains complete for every value it declines.

#include "codegen/regalloc.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cider {
namespace codegen {

namespace {

constexpr uint8_t kNoRegister = static_cast<uint8_t>(Register::None);
constexpr uint32_t kInvalidValue = 0xFFFFFFFFu;
constexpr uint8_t kRegisterCount = static_cast<uint8_t>(Register::Rbx) + 1;
constexpr uint64_t kOneDefinition = 1;
// A live set wider than this is not turned into a pairwise interference
// clique. Its values keep their frames instead: the clique's quadratic edge
// count would dominate compile time on large array copies, where thousands of
// element values can be live at one point. Normal Cider functions sit far
// below this line.
constexpr uint32_t kCliqueLimit = 64;
constexpr uint32_t kNoEdges = 0;
// Above this many cross-block candidates, the function keeps every value in
// its slot. The liveness sets in a giant array copy are themselves huge, and
// the local fallback already handles those values; spending seconds or
// gigabytes to prove that a 20,000-element copy has no register to spare is
// not a useful trade.
constexpr uint32_t kGlobalCandidateLimit = 1024;

// Caller-saved rows. r10 is volatile in both ABIs and is not fixed scratch;
// rdi/rsi are volatile only under SysV, so the msabi row stops before them.
constexpr Register kSysVPool[] = {Register::R8, Register::R9, Register::R10,
                                  Register::Rdi, Register::Rsi};
constexpr Register kMsAbiPool[] = {Register::R8, Register::R9, Register::R10};

// Callee-saved homes. rbx is nonvolatile under both ABIs and is not touched
// by any emitter path (call spill staging borrows rax/r10/r11/r12-r15, asm
// staging pushes and pops r12-r15/rbx, and fixed scratch is rax/rcx/rdx/r11).
constexpr Register kCalleePool[] = {Register::Rbx};

bool isFloatWidth(ir::IRType width) {
    return width == ir::IRType::F32 || width == ir::IRType::F64;
}

bool isByteWidth(ir::IRType width) {
    return width == ir::IRType::I8 || width == ir::IRType::U8;
}

// True when the emitter uses a fixed sub-register or special register for
// this instruction and therefore should keep its operands and result in
// memory for this slice.
bool isFixedScratchInstruction(const ir::Instr& instr) {
    if (instr.op == ir::Op::Div || instr.op == ir::Op::Rem ||
        instr.op == ir::Op::Shl || instr.op == ir::Op::Shr ||
        instr.op == ir::Op::VaStart || instr.op == ir::Op::VaArg) {
        return true;
    }
    return instr.op == ir::Op::Mul && isByteWidth(instr.width);
}

// True when the instruction ends a register live range: the emitter may
// clobber caller-saved registers, or the language requires the access to stay
// in memory. Values live across one of these keep their slot unless a
// callee-saved home can carry them.
bool isBarrier(const ir::Instr& instr) {
    if (instr.op == ir::Op::Call || instr.op == ir::Op::Asm ||
        instr.op == ir::Op::Trap || instr.op == ir::Op::VaStart ||
        instr.op == ir::Op::VaArg) {
        return true;
    }
    return (instr.op == ir::Op::Load || instr.op == ir::Op::Store) &&
           instr.isVolatile;
}

void noteValue(uint32_t id, uint32_t& top) {
    if (id != kInvalidValue && id > top) {
        top = id;
    }
}

void noteInstrValues(const ir::Instr& instr, uint32_t& top) {
    for (const ir::Value& result : instr.results) {
        noteValue(result.id, top);
    }
    for (const ir::Value& operand : instr.operands) {
        noteValue(operand.id, top);
    }
    for (const ir::PhiIncoming& incoming : instr.phiIncoming) {
        noteValue(incoming.value.id, top);
    }
    if (instr.op == ir::Op::Load || instr.op == ir::Op::Store ||
        instr.op == ir::Op::Extract || instr.op == ir::Op::Insert ||
        instr.op == ir::Op::Gep) {
        noteValue(instr.addressBase.id, top);
    }
    if (instr.op == ir::Op::Store || instr.op == ir::Op::Insert) {
        noteValue(instr.storedValue.id, top);
    }
    if (instr.op == ir::Op::Call && instr.call.indirect) {
        noteValue(instr.calleeValue.id, top);
    }
}

uint32_t maxValueId(const ir::Function& fn) {
    uint32_t top = 0;
    for (const ir::Value& param : fn.params) {
        noteValue(param.id, top);
    }
    for (const ir::Block& block : fn.blocks) {
        for (const ir::Instr& instr : block.body) {
            noteInstrValues(instr, top);
            if (instr.op == ir::Op::Call) {
                for (const ir::Argument& arg : instr.arguments) {
                    for (const ir::Value& word : arg.words) {
                        noteValue(word.id, top);
                    }
                }
            }
        }
        for (const ir::Value& value : block.term.values) {
            noteValue(value.id, top);
        }
        if (block.term.cond.has_value()) {
            noteValue(block.term.cond->id, top);
        }
        if (block.term.term == ir::Term::Switch) {
            noteValue(block.term.switchValue.id, top);
            for (const ir::Case& arm : block.term.cases) {
                noteValue(arm.value.id, top);
            }
        }
    }
    return top;
}

// Every value an instruction reads, in one place: the emitter's slots and the
// allocator's liveness must agree on what a use is. The caller filters ids.
void appendInstrUses(const ir::Instr& instr, std::vector<uint32_t>& out) {
    auto add = [&](const ir::Value& value) {
        if (value.id != kInvalidValue) {
            out.push_back(value.id);
        }
    };
    for (const ir::Value& operand : instr.operands) {
        add(operand);
    }
    for (const ir::PhiIncoming& incoming : instr.phiIncoming) {
        add(incoming.value);
    }
    if (instr.op == ir::Op::Load || instr.op == ir::Op::Store ||
        instr.op == ir::Op::Extract || instr.op == ir::Op::Insert ||
        instr.op == ir::Op::Gep) {
        add(instr.addressBase);
    }
    if (instr.op == ir::Op::Store || instr.op == ir::Op::Insert) {
        add(instr.storedValue);
    }
    if (instr.op == ir::Op::Call) {
        for (const ir::Argument& arg : instr.arguments) {
            for (const ir::Value& word : arg.words) {
                add(word);
            }
        }
        if (instr.call.indirect) {
            add(instr.calleeValue);
        }
    }
}

void appendTermUses(const ir::Terminator& term, std::vector<uint32_t>& out) {
    auto add = [&](const ir::Value& value) {
        if (value.id != kInvalidValue) {
            out.push_back(value.id);
        }
    };
    for (const ir::Value& value : term.values) {
        add(value);
    }
    if (term.cond.has_value()) {
        add(*term.cond);
    }
    if (term.term == ir::Term::Switch) {
        add(term.switchValue);
        for (const ir::Case& arm : term.cases) {
            add(arm.value);
        }
    }
}

void exclude(std::unordered_set<uint32_t>& out, const ir::Value& value) {
    if (value.id != kInvalidValue) {
        out.insert(value.id);
    }
}

// Values whose producer, consumer or storage shape this slice does not model.
// Exclusion is function-wide, so the same id is never half-promoted in one
// block and read from a slot in another.
void collectExclusions(const ir::Function& fn,
                       std::unordered_set<uint32_t>& excluded) {
    for (const ir::Block& block : fn.blocks) {
        for (const ir::Instr& instr : block.body) {
            if (isFixedScratchInstruction(instr)) {
                for (const ir::Value& operand : instr.operands) {
                    exclude(excluded, operand);
                }
                for (const ir::Value& result : instr.results) {
                    exclude(excluded, result);
                }
            }
            if (instr.op == ir::Op::Phi) {
                for (const ir::Value& result : instr.results) {
                    exclude(excluded, result);
                }
                for (const ir::Value& operand : instr.operands) {
                    exclude(excluded, operand);
                }
                for (const ir::PhiIncoming& incoming : instr.phiIncoming) {
                    exclude(excluded, incoming.value);
                }
            }
            if (instr.op == ir::Op::Alloca) {
                for (const ir::Value& result : instr.results) {
                    exclude(excluded, result);
                }
            }
            if (instr.op == ir::Op::Extract || instr.op == ir::Op::Insert) {
                for (const ir::Value& result : instr.results) {
                    exclude(excluded, result);
                }
                for (const ir::Value& operand : instr.operands) {
                    exclude(excluded, operand);
                }
                exclude(excluded, instr.addressBase);
                exclude(excluded, instr.storedValue);
            }
            if (instr.op == ir::Op::Call) {
                for (const ir::Argument& arg : instr.arguments) {
                    for (const ir::Value& word : arg.words) {
                        exclude(excluded, word);
                    }
                }
                if (instr.call.indirect) {
                    exclude(excluded, instr.calleeValue);
                }
            }
            if (instr.op == ir::Op::Asm) {
                for (const ir::Value& operand : instr.operands) {
                    exclude(excluded, operand);
                }
                for (const ir::Value& result : instr.results) {
                    exclude(excluded, result);
                }
            }
            if ((instr.op == ir::Op::Load || instr.op == ir::Op::Store) &&
                instr.isVolatile) {
                if (instr.op == ir::Op::Load) {
                    for (const ir::Value& result : instr.results) {
                        exclude(excluded, result);
                    }
                }
                if (instr.op == ir::Op::Store) {
                    exclude(excluded, instr.storedValue);
                }
                exclude(excluded, instr.addressBase);
            }
        }
    }
}

struct Site {
    uint32_t block = 0;
    uint32_t instr = 0;
};

struct Interval {
    uint32_t start = 0;
    uint32_t end = 0;
    uint32_t id = 0;
};

struct Active {
    uint32_t end = 0;
    uint8_t reg = kNoRegister;
};

struct GlobalPlan {
    std::unordered_map<uint32_t, uint8_t> home;
    std::vector<std::unordered_set<uint32_t>> liveIn;
    std::vector<std::unordered_set<uint32_t>> liveOut;
    std::unordered_set<uint32_t> candidates;
    std::unordered_set<uint32_t> crossing;
    std::unordered_set<uint32_t> forcedSpill;
    uint64_t allocated = 0;
    uint64_t spilled = 0;
    bool usesRbx = false;
};

bool spansBarrier(uint32_t start, uint32_t end,
                  const std::vector<uint32_t>& barriers) {
    for (uint32_t barrier : barriers) {
        // A definition exactly at a barrier (a call result, a volatile load
        // result) begins after it; only a use on the far side is a crossing.
        if (start < barrier && end >= barrier) {
            return true;
        }
    }
    return false;
}

uint8_t registerBit(Register reg) {
    return static_cast<uint8_t>(1u << static_cast<uint8_t>(reg));
}

bool isCalleeSaved(Register reg) {
    for (Register candidate : kCalleePool) {
        if (candidate == reg) {
            return true;
        }
    }
    return false;
}

using Adjacency = std::unordered_map<uint32_t, std::unordered_set<uint32_t>>;

void addClique(const std::vector<uint32_t>& ids, Adjacency& adj) {
    for (auto a = ids.begin(); a != ids.end(); ++a) {
        for (auto b = a + 1; b != ids.end(); ++b) {
            if (*a == *b) {
                continue;
            }
            adj[*a].insert(*b);
            adj[*b].insert(*a);
        }
    }
}

// A wide live set is not materialised as a clique. Every value at that point
// is marked to keep its slot instead; a value that occupies no register needs
// no interference edges, and correctness is unchanged (the slot path is the
// oracle).
void recordClique(const std::vector<uint32_t>& point, GlobalPlan& plan,
                  Adjacency& adj) {
    if (point.size() > kCliqueLimit) {
        plan.forcedSpill.insert(point.begin(), point.end());
        return;
    }
    addClique(point, adj);
}

void addCandidate(std::vector<uint32_t>& out, uint32_t id,
                  const std::unordered_set<uint32_t>& candidates) {
    if (candidates.find(id) != candidates.end()) {
        out.push_back(id);
    }
}

// Function-wide may-liveness over candidate ids, then interference cliques at
// every instruction and terminator. Candidate ids are the only values that can
// occupy a register, so non-candidates need no nodes in the graph.
GlobalPlan planGlobals(const ir::Function& fn,
                       const std::unordered_set<uint32_t>& excluded,
                       const std::unordered_map<uint32_t, std::vector<Site>>& uses,
                       const std::unordered_map<uint32_t, std::vector<Site>>& defs) {
    GlobalPlan plan;
    const size_t blockCount = fn.blocks.size();
    plan.liveIn.resize(blockCount);
    plan.liveOut.resize(blockCount);

    std::unordered_map<uint32_t, uint32_t> blockAt;
    for (uint32_t b = 0; b < blockCount; ++b) {
        blockAt[fn.blocks[b].label] = b;
    }

    std::vector<std::vector<uint32_t>> successors(blockCount);
    auto addTarget = [&](std::vector<uint32_t>& out, uint32_t label) {
        auto found = blockAt.find(label);
        if (found != blockAt.end()) {
            out.push_back(found->second);
        }
    };
    for (uint32_t b = 0; b < blockCount; ++b) {
        const ir::Terminator& term = fn.blocks[b].term;
        switch (term.term) {
            case ir::Term::Br:
                addTarget(successors[b], term.target);
                break;
            case ir::Term::CondBr:
                addTarget(successors[b], term.thenTarget);
                addTarget(successors[b], term.elseTarget);
                break;
            case ir::Term::Switch:
                for (const ir::Case& arm : term.cases) {
                    addTarget(successors[b], arm.target);
                }
                addTarget(successors[b], term.defaultTarget);
                break;
            case ir::Term::Ret:
            case ir::Term::Unreachable:
            case ir::Term::Trap:
                break;
        }
    }

    // A global range starts at a single definition and reaches past its block.
    for (const auto& entry : defs) {
        const uint32_t id = entry.first;
        const size_t definitionCount = entry.second.size();
        if (definitionCount != kOneDefinition ||
            excluded.find(id) != excluded.end()) {
            continue;
        }
        const Site& def = entry.second.front();
        if (def.block >= blockCount || def.instr >= fn.blocks[def.block].body.size()) {
            continue;
        }
        const ir::Instr& producer = fn.blocks[def.block].body[def.instr];
        const ir::Value* value = nullptr;
        for (const ir::Value& result : producer.results) {
            if (result.id == id) {
                value = &result;
                break;
            }
        }
        if (value == nullptr || value->width == ir::IRType::Void ||
            isFloatWidth(value->width)) {
            continue;
        }
        auto sites = uses.find(id);
        if (sites == uses.end() || sites->second.empty()) {
            continue;
        }
        bool crossBlock = false;
        for (const Site& use : sites->second) {
            if (use.block != def.block) {
                crossBlock = true;
                break;
            }
        }
        if (crossBlock) {
            plan.candidates.insert(id);
        }
    }

    if (plan.candidates.empty()) {
        return plan;
    }
    if (plan.candidates.size() > kGlobalCandidateLimit) {
        plan.forcedSpill.insert(plan.candidates.begin(),
                                plan.candidates.end());
        plan.spilled = plan.candidates.size();
        return plan;
    }

    std::vector<std::unordered_set<uint32_t>> blockDefs(blockCount);
    std::vector<std::unordered_set<uint32_t>> upUse(blockCount);
    for (uint32_t b = 0; b < blockCount; ++b) {
        const ir::Block& block = fn.blocks[b];
        std::unordered_set<uint32_t> defined;
        std::vector<uint32_t> scratch;
        for (const ir::Instr& instr : block.body) {
            scratch.clear();
            appendInstrUses(instr, scratch);
            for (uint32_t id : scratch) {
                if (plan.candidates.find(id) != plan.candidates.end() &&
                    defined.find(id) == defined.end()) {
                    upUse[b].insert(id);
                }
            }
            for (const ir::Value& result : instr.results) {
                if (plan.candidates.find(result.id) != plan.candidates.end()) {
                    defined.insert(result.id);
                    blockDefs[b].insert(result.id);
                }
            }
        }
        scratch.clear();
        appendTermUses(block.term, scratch);
        for (uint32_t id : scratch) {
            if (plan.candidates.find(id) != plan.candidates.end() &&
                defined.find(id) == defined.end()) {
                upUse[b].insert(id);
            }
        }
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t b = 0; b < blockCount; ++b) {
            std::unordered_set<uint32_t> out;
            for (uint32_t successor : successors[b]) {
                out.insert(plan.liveIn[successor].begin(),
                           plan.liveIn[successor].end());
            }
            std::unordered_set<uint32_t> in = upUse[b];
            for (uint32_t id : out) {
                if (blockDefs[b].find(id) == blockDefs[b].end()) {
                    in.insert(id);
                }
            }
            if (out != plan.liveOut[b] || in != plan.liveIn[b]) {
                plan.liveOut[b] = std::move(out);
                plan.liveIn[b] = std::move(in);
                changed = true;
            }
        }
    }

    Adjacency adj;
    for (uint32_t b = 0; b < blockCount; ++b) {
        // A block whose live sets are already too wide for a clique needs no
        // instruction-by-instruction interference: every cross-block value
        // that touches it keeps its slot, and the local scan can still home
        // values wholly inside the block.
        if (plan.liveIn[b].size() > kCliqueLimit ||
            plan.liveOut[b].size() > kCliqueLimit) {
            plan.forcedSpill.insert(plan.liveIn[b].begin(),
                                    plan.liveIn[b].end());
            plan.forcedSpill.insert(plan.liveOut[b].begin(),
                                    plan.liveOut[b].end());
            continue;
        }
        const ir::Block& block = fn.blocks[b];
        // Terminator uses are live at every instruction in the block: a call
        // earlier in the body can clobber a caller-saved register that the
        // terminator reads. Seeding the backward scan with them is what lets
        // the barrier test mark such a value as crossing; leaving them only
        // in the interference clique below gave a terminator-only value a
        // caller-saved home and a call then overwrote it.
        std::vector<uint32_t> termUses;
        std::vector<uint32_t> scratch;
        scratch.clear();
        appendTermUses(block.term, scratch);
        for (uint32_t id : scratch) {
            addCandidate(termUses, id, plan.candidates);
        }
        std::unordered_set<uint32_t> live = plan.liveOut[b];
        for (uint32_t id : termUses) {
            live.insert(id);
        }
        for (auto at = block.body.rbegin(); at != block.body.rend(); ++at) {
            const ir::Instr& instr = *at;
            // A trap is terminal: the instructions after it never execute, and
            // nothing before it is live across it.
            if (instr.op == ir::Op::Trap) {
                live.clear();
                continue;
            }

            std::vector<uint32_t> instrUses;
            scratch.clear();
            appendInstrUses(instr, scratch);
            for (uint32_t id : scratch) {
                addCandidate(instrUses, id, plan.candidates);
            }
            std::unordered_set<uint32_t> instrDefs;
            for (const ir::Value& result : instr.results) {
                if (plan.candidates.find(result.id) != plan.candidates.end()) {
                    instrDefs.insert(result.id);
                }
            }

            if (isBarrier(instr)) {
                for (uint32_t id : live) {
                    if (instrDefs.find(id) == instrDefs.end()) {
                        plan.crossing.insert(id);
                    }
                }
            }

            std::unordered_set<uint32_t> before = live;
            for (uint32_t id : instrDefs) {
                before.erase(id);
            }
            for (uint32_t id : instrUses) {
                before.insert(id);
            }
            std::vector<uint32_t> point(before.begin(), before.end());
            for (uint32_t id : instrDefs) {
                point.push_back(id);
            }
            recordClique(point, plan, adj);
            live = std::move(before);
        }

        // Values used by the terminator and values live out of the block are
        // simultaneously needed: a condition, a switch word or a return word
        // must not share a home with the value it chooses or returns.
        std::unordered_set<uint32_t> atTerm = plan.liveOut[b];
        for (uint32_t id : termUses) {
            atTerm.insert(id);
        }
        std::vector<uint32_t> point(atTerm.begin(), atTerm.end());
        recordClique(point, plan, adj);
    }

    const bool ms = fn.signature.abi() == Abi::MsAbi;
    std::vector<Register> pool;
    if (ms) {
        pool.insert(pool.end(), std::begin(kMsAbiPool), std::end(kMsAbiPool));
    } else {
        pool.insert(pool.end(), std::begin(kSysVPool), std::end(kSysVPool));
    }
    pool.insert(pool.end(), std::begin(kCalleePool), std::end(kCalleePool));

    std::vector<uint32_t> order(plan.candidates.begin(), plan.candidates.end());
    auto degreeOf = [&](uint32_t id) {
        auto found = adj.find(id);
        if (found == adj.end()) {
            return static_cast<std::size_t>(kNoEdges);
        }
        return found->second.size();
    };
    std::sort(order.begin(), order.end(),
              [&](uint32_t lhs, uint32_t rhs) {
                  if (degreeOf(lhs) != degreeOf(rhs)) {
                      return degreeOf(lhs) > degreeOf(rhs);
                  }
                  return lhs < rhs;
              });

    std::unordered_map<uint32_t, uint8_t> assigned;
    for (uint32_t id : order) {
        if (plan.forcedSpill.find(id) != plan.forcedSpill.end()) {
            ++plan.spilled;
            continue;
        }
        uint8_t taken = 0;
        auto neighbors = adj.find(id);
        if (neighbors != adj.end()) {
            for (uint32_t neighbor : neighbors->second) {
                auto found = assigned.find(neighbor);
                if (found != assigned.end()) {
                    taken |= registerBit(static_cast<Register>(found->second));
                }
            }
        }
        const bool crossing =
            plan.crossing.find(id) != plan.crossing.end();
        uint8_t chosen = kNoRegister;
        for (Register candidate : pool) {
            if (crossing && !isCalleeSaved(candidate)) {
                continue;
            }
            if ((taken & registerBit(candidate)) == 0) {
                chosen = static_cast<uint8_t>(candidate);
                break;
            }
        }
        if (chosen == kNoRegister) {
            ++plan.spilled;
            continue;
        }
        assigned[id] = chosen;
        plan.home[id] = chosen;
        if (static_cast<Register>(chosen) == Register::Rbx) {
            plan.usesRbx = true;
        }
        ++plan.allocated;
    }
    return plan;
}

}  // namespace

const char* registerName(Register reg) {
    switch (reg) {
        case Register::None:
            return "none";
        case Register::R8:
            return "r8";
        case Register::R9:
            return "r9";
        case Register::R10:
            return "r10";
        case Register::Rdi:
            return "rdi";
        case Register::Rsi:
            return "rsi";
        case Register::Rbx:
            return "rbx";
    }
    return "none";
}

FunctionAllocation allocateFunction(const ir::Function& fn) {
    FunctionAllocation result;
    result.blocks.resize(fn.blocks.size());
    const uint32_t top = maxValueId(fn);
    for (BlockAllocation& block : result.blocks) {
        block.home.assign(static_cast<uint64_t>(top) + 1, kNoRegister);
    }

    std::unordered_set<uint32_t> excluded;
    collectExclusions(fn, excluded);
    std::unordered_map<uint32_t, std::vector<Site>> uses;
    std::unordered_map<uint32_t, std::vector<Site>> defs;
    for (uint32_t b = 0; b < fn.blocks.size(); ++b) {
        const ir::Block& block = fn.blocks[b];
        for (uint32_t i = 0; i < block.body.size(); ++i) {
            const ir::Instr& instr = block.body[i];
            for (const ir::Value& result : instr.results) {
                if (result.id != kInvalidValue) {
                    defs[result.id].push_back(Site{b, i});
                }
            }
            for (const ir::Value& operand : instr.operands) {
                uses[operand.id].push_back(Site{b, i});
            }
            for (const ir::PhiIncoming& incoming : instr.phiIncoming) {
                uses[incoming.value.id].push_back(Site{b, i});
            }
            if (instr.op == ir::Op::Load || instr.op == ir::Op::Store ||
                instr.op == ir::Op::Extract || instr.op == ir::Op::Insert ||
                instr.op == ir::Op::Gep) {
                uses[instr.addressBase.id].push_back(Site{b, i});
            }
            if (instr.op == ir::Op::Store || instr.op == ir::Op::Insert) {
                uses[instr.storedValue.id].push_back(Site{b, i});
            }
            if (instr.op == ir::Op::Call) {
                for (const ir::Argument& arg : instr.arguments) {
                    for (const ir::Value& word : arg.words) {
                        uses[word.id].push_back(Site{b, i});
                    }
                }
                if (instr.call.indirect) {
                    uses[instr.calleeValue.id].push_back(Site{b, i});
                }
            }
        }
        const ir::Terminator& term = block.term;
        const uint32_t termAt = static_cast<uint32_t>(block.body.size());
        for (const ir::Value& value : term.values) {
            uses[value.id].push_back(Site{b, termAt});
        }
        if (term.cond.has_value()) {
            uses[term.cond->id].push_back(Site{b, termAt});
        }
        if (term.term == ir::Term::Switch) {
            uses[term.switchValue.id].push_back(Site{b, termAt});
            for (const ir::Case& arm : term.cases) {
                uses[arm.value.id].push_back(Site{b, termAt});
            }
        }
    }

    // Global homes first: cross-block liveness and interference.
    GlobalPlan global = planGlobals(fn, excluded, uses, defs);
    for (const auto& entry : global.home) {
        const uint32_t id = entry.first;
        const uint8_t reg = entry.second;
        auto def = defs.find(id);
        if (def != defs.end()) {
            for (const Site& site : def->second) {
                result.blocks[site.block].home[id] = reg;
            }
        }
        auto use = uses.find(id);
        if (use != uses.end()) {
            for (const Site& site : use->second) {
                result.blocks[site.block].home[id] = reg;
            }
        }
        for (uint32_t b = 0; b < fn.blocks.size(); ++b) {
            if (global.liveIn[b].find(id) != global.liveIn[b].end() ||
                global.liveOut[b].find(id) != global.liveOut[b].end()) {
                result.blocks[b].home[id] = reg;
            }
        }
    }
    result.globalValues = global.allocated;
    result.globalSpilled = global.spilled;
    result.valuesAllocated += global.allocated;
    result.valuesSpilled += global.spilled;
    if (global.usesRbx) {
        result.calleeSaved.push_back(Register::Rbx);
    }

    // A global home owns its register for the whole block it is live in, so
    // the local scan cannot reuse it anywhere inside that block.
    std::vector<uint8_t> reserved(fn.blocks.size());
    for (const auto& entry : global.home) {
        const uint32_t id = entry.first;
        const uint8_t bit = registerBit(static_cast<Register>(entry.second));
        auto def = defs.find(id);
        if (def != defs.end()) {
            for (const Site& site : def->second) {
                reserved[site.block] |= bit;
            }
        }
        auto use = uses.find(id);
        if (use != uses.end()) {
            for (const Site& site : use->second) {
                reserved[site.block] |= bit;
            }
        }
        for (uint32_t b = 0; b < fn.blocks.size(); ++b) {
            if (global.liveIn[b].find(id) != global.liveIn[b].end() ||
                global.liveOut[b].find(id) != global.liveOut[b].end()) {
                reserved[b] |= bit;
            }
        }
    }

    // One local use list per block: building last/first use then costs only
    // that block's own uses, not the whole function's, for every block.
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> blockUses(
        fn.blocks.size());
    for (const auto& entry : uses) {
        for (const Site& site : entry.second) {
            blockUses[site.block].push_back({entry.first, site.instr});
        }
    }

    const bool ms = fn.signature.abi() == Abi::MsAbi;
    const Register* pool = ms ? kMsAbiPool : kSysVPool;
    const uint32_t poolSize = ms
                                  ? static_cast<uint32_t>(std::size(kMsAbiPool))
                                  : static_cast<uint32_t>(std::size(kSysVPool));

    for (uint32_t b = 0; b < fn.blocks.size(); ++b) {
        const ir::Block& block = fn.blocks[b];
        BlockAllocation& out = result.blocks[b];
        std::unordered_map<uint32_t, uint32_t> defAt;
        std::unordered_map<uint32_t, uint32_t> lastUse;
        std::unordered_map<uint32_t, uint32_t> firstUse;
        std::unordered_set<uint32_t> twiceDefined;
        std::vector<uint32_t> barriers;
        for (uint32_t i = 0; i < block.body.size(); ++i) {
            const ir::Instr& instr = block.body[i];
            for (const ir::Value& defined : instr.results) {
                if (defAt.find(defined.id) != defAt.end()) {
                    twiceDefined.insert(defined.id);
                }
                defAt[defined.id] = i;
            }
            if (isBarrier(instr)) {
                barriers.push_back(i);
            }
        }
        if (block.term.term == ir::Term::Trap) {
            barriers.push_back(static_cast<uint32_t>(block.body.size()));
        }
        result.barrierCount += static_cast<uint64_t>(barriers.size());

        for (const auto& use : blockUses[b]) {
            const uint32_t id = use.first;
            const uint32_t at = use.second;
            auto found = lastUse.find(id);
            if (found == lastUse.end() || at > found->second) {
                lastUse[id] = at;
            }
            auto first = firstUse.find(id);
            if (first == firstUse.end() || at < first->second) {
                firstUse[id] = at;
            }
        }

        std::vector<Interval> intervals;
        for (const auto& entry : defAt) {
            const uint32_t id = entry.first;
            if (excluded.find(id) != excluded.end() ||
                twiceDefined.find(id) != twiceDefined.end() ||
                global.home.find(id) != global.home.end()) {
                continue;
            }
            const ir::Value* value = nullptr;
            for (const ir::Value& resultValue : block.body[entry.second].results) {
                if (resultValue.id == id) {
                    value = &resultValue;
                    break;
                }
            }
            if (value == nullptr || value->width == ir::IRType::Void ||
                isFloatWidth(value->width)) {
                continue;
            }
            auto used = lastUse.find(id);
            auto first = firstUse.find(id);
            if (used == lastUse.end() || first == firstUse.end()) {
                continue;
            }
            // Every use must be in this block. A def in another block does
            // not matter: this block's path defines its own home, and only
            // this block can read it.
            bool local = true;
            auto sites = uses.find(id);
            if (sites != uses.end()) {
                for (const Site& site : sites->second) {
                    if (site.block != b) {
                        local = false;
                        break;
                    }
                }
            }
            if (!local || first->second < entry.second ||
                used->second <= entry.second) {
                continue;
            }
            if (spansBarrier(entry.second, used->second, barriers)) {
                continue;
            }
            intervals.push_back(Interval{entry.second, used->second, id});
        }

        std::sort(intervals.begin(), intervals.end(),
                  [](const Interval& lhs, const Interval& rhs) {
                      if (lhs.start != rhs.start) {
                          return lhs.start < rhs.start;
                      }
                      if (lhs.end != rhs.end) {
                          return lhs.end > rhs.end;
                      }
                      return lhs.id < rhs.id;
                  });

        std::vector<Active> active;
        for (const Interval& interval : intervals) {
            for (auto it = active.begin(); it != active.end();) {
                if (it->end < interval.start) {
                    it = active.erase(it);
                } else {
                    ++it;
                }
            }
            bool taken[kRegisterCount] = {};
            for (uint8_t reg = 1; reg < kRegisterCount; ++reg) {
                if ((reserved[b] & registerBit(static_cast<Register>(reg))) != 0) {
                    taken[reg] = true;
                }
            }
            for (const Active& held : active) {
                taken[held.reg] = true;
            }
            uint8_t chosen = kNoRegister;
            for (uint32_t p = 0; p < poolSize; ++p) {
                const uint8_t candidate = static_cast<uint8_t>(pool[p]);
                if (!taken[candidate]) {
                    chosen = candidate;
                    break;
                }
            }
            if (chosen == kNoRegister) {
                // Spill-on-pressure: the current interval keeps its existing
                // stack slot. No active value is evicted, so every assigned
                // register really does carry its value until its last use.
                ++result.valuesSpilled;
                continue;
            }
            out.home[interval.id] = chosen;
            active.push_back(Active{interval.end, chosen});
            ++result.valuesAllocated;
        }
    }
    return result;
}

}  // namespace codegen
}  // namespace cider
