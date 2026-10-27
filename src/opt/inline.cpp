// inline.cpp — conservative direct-call inlining, before mem2reg.
//
// The lowerer emits a stack-slot form: every parameter is one Alloca result,
// one per incoming machine word, and the prologue stores the argument word
// into that slot. This pass clones a small, same-module callee at one call
// site and gives every callee parameter its own fresh slot plus an explicit
// Store of the corresponding flattened call-argument word. Mem2reg then sees
// ordinary stores and loads and can promote the cloned parameter traffic,
// including joins inside the cloned body.
//
// The refusal list is the safety boundary, not a heuristic:
//   - direct, same-module calls only; indirect, missing and extern callees
//     stay calls;
//   - a callee that calls itself directly stays a call, so one level of
//     inlining can never unfold recursion;
//   - a callee containing asm stays a call: asm can name frame storage the
//     IR does not describe;
//   - a callee over 24 instructions or 16 blocks stays a call;
//   - beyond the tiny tier, a callee with more than four call sites stays a
//     call: cloning a medium body into many callers grows the image faster
//     than removing the call overhead saves;
//   - caller and callee must agree on the calling convention;
//   - all reachable callee blocks are cloned. A callee with unreachable
//     blocks is refused: cloning into an unreachable caller region would
//     otherwise move a reachable phi into an unreachable block;
//   - one level per call site. Calls inside cloned bodies are not visited
//     again; calls in the caller's original suffix still are. A callee that
//     has itself been a caller is cloned from the body it arrived with, so a
//     clone never carries a second level of inlined code.
//
// Return values use the SSA shape directly: one Ret block substitutes its
// mapped words for the call results, several Ret blocks prepend one Phi per
// result word to the continuation. Void and sret calls have no result words,
// so their cloned Refs become plain branches and a body that stores through
// the hidden pointer keeps working unchanged.

#include "opt/opt.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "opt/dominance.h"

namespace cider {
namespace opt {

namespace {

constexpr uint32_t kInvalidValue = 0xFFFFFFFFu;
constexpr uint32_t kLastFreshId = 0xFFFFFFFEu;
constexpr uint64_t kOneResult = 1;
constexpr uint64_t kMaxCalleeInstrs = 24;
constexpr uint64_t kMaxCalleeBlocks = 16;
// A tiny helper is cheap enough to duplicate at every call. A larger body is
// only inline-worthy while it is not spread across the module: cloning a
// twenty-instruction function into many callers grows the image faster than
// it removes call overhead.
constexpr uint64_t kTinyCalleeInstrs = 8;
constexpr uint64_t kMaxCalleeSites = 4;
constexpr uint64_t kMaxSitesPerFunction = 40;
constexpr uint64_t kMaxClonedInstrsPerFunction = 4000;

bool namesBase(ir::Op op) {
    return op == ir::Op::Load || op == ir::Op::Store ||
           op == ir::Op::Extract || op == ir::Op::Insert ||
           op == ir::Op::Gep;
}

uint32_t maxValueId(const ir::Function& fn) {
    uint32_t top = 0;
    auto note = [&](uint32_t id) {
        if (id > top && id != kInvalidValue) {
            top = id;
        }
    };
    for (const ir::Value& param : fn.params) {
        note(param.id);
    }
    for (const ir::Block& block : fn.blocks) {
        for (const ir::Instr& instr : block.body) {
            for (const ir::Value& result : instr.results) {
                note(result.id);
            }
            for (const ir::Value& operand : instr.operands) {
                note(operand.id);
            }
            for (const ir::PhiIncoming& incoming : instr.phiIncoming) {
                note(incoming.value.id);
            }
            if (namesBase(instr.op)) {
                note(instr.addressBase.id);
            }
            if (instr.op == ir::Op::Store || instr.op == ir::Op::Insert) {
                note(instr.storedValue.id);
            }
            if (instr.op == ir::Op::Call && instr.call.indirect) {
                note(instr.calleeValue.id);
            }
        }
        const ir::Terminator& term = block.term;
        for (const ir::Value& value : term.values) {
            note(value.id);
        }
        if (term.cond.has_value()) {
            note(term.cond->id);
        }
        if (term.term == ir::Term::Switch) {
            note(term.switchValue.id);
            for (const ir::Case& arm : term.cases) {
                note(arm.value.id);
            }
        }
    }
    return top;
}

uint32_t maxLabelId(const ir::Function& fn) {
    uint32_t top = 0;
    for (const ir::Block& block : fn.blocks) {
        if (block.label > top) {
            top = block.label;
        }
    }
    return top;
}

// The same semantic use set the structural checker reads. A field that is
// inert for its opcode is not remapped: a direct call's calleeValue default
// and a Load's global-label addressBase are not uses.
bool remapValue(ir::Value& value,
                const std::unordered_map<uint32_t, ir::Value>& map) {
    if (value.id == kInvalidValue) {
        return true;
    }
    auto found = map.find(value.id);
    if (found == map.end()) {
        return false;
    }
    value = found->second;
    return true;
}

bool remapInstrUses(ir::Instr& instr,
                    const std::unordered_map<uint32_t, ir::Value>& map) {
    for (ir::Value& operand : instr.operands) {
        if (!remapValue(operand, map)) {
            return false;
        }
    }
    for (ir::Argument& arg : instr.arguments) {
        for (ir::Value& word : arg.words) {
            if (!remapValue(word, map)) {
                return false;
            }
        }
    }
    for (ir::PhiIncoming& incoming : instr.phiIncoming) {
        if (!remapValue(incoming.value, map)) {
            return false;
        }
    }
    if (namesBase(instr.op) && instr.globalLabel.empty()) {
        if (!remapValue(instr.addressBase, map)) {
            return false;
        }
    }
    if (instr.op == ir::Op::Store || instr.op == ir::Op::Insert) {
        if (!remapValue(instr.storedValue, map)) {
            return false;
        }
    }
    if (instr.op == ir::Op::Call && instr.call.indirect) {
        if (!remapValue(instr.calleeValue, map)) {
            return false;
        }
    }
    return true;
}

bool remapTerminatorUses(ir::Terminator& term,
                         const std::unordered_map<uint32_t, ir::Value>& map) {
    for (ir::Value& value : term.values) {
        if (!remapValue(value, map)) {
            return false;
        }
    }
    if (term.cond.has_value()) {
        if (!remapValue(*term.cond, map)) {
            return false;
        }
    }
    if (term.term == ir::Term::Switch) {
        if (!remapValue(term.switchValue, map)) {
            return false;
        }
        for (ir::Case& arm : term.cases) {
            if (!remapValue(arm.value, map)) {
                return false;
            }
        }
    }
    return true;
}

// Rewrite every semantic use of a value id. Definitions are untouched. This
// is the field set mem2reg and slot forwarding share.
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
        for (ir::Value& value : term.values) {
            remap(value);
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

const ir::Function* findFunction(const std::vector<ir::Function>& functions,
                                 const std::string& name) {
    for (const ir::Function& fn : functions) {
        if (fn.name == name) {
            return &fn;
        }
    }
    return nullptr;
}

bool hasAsm(const ir::Function& fn) {
    for (const ir::Block& block : fn.blocks) {
        for (const ir::Instr& instr : block.body) {
            if (instr.op == ir::Op::Asm) {
                return true;
            }
        }
    }
    return false;
}

bool callsItself(const ir::Function& fn) {
    for (const ir::Block& block : fn.blocks) {
        for (const ir::Instr& instr : block.body) {
            if (instr.op == ir::Op::Call && !instr.call.indirect &&
                instr.callee == fn.name) {
                return true;
            }
        }
    }
    return false;
}

uint64_t instructionCount(const ir::Function& fn) {
    uint64_t count = 0;
    for (const ir::Block& block : fn.blocks) {
        count += block.body.size();
    }
    return count;
}

// Distinct successor labels, in terminator order, so the rewrite below is
// deterministic and visits an edge once.
std::vector<uint32_t> successorLabels(const ir::Terminator& term) {
    std::vector<uint32_t> out;
    auto add = [&](uint32_t label) {
        for (uint32_t seen : out) {
            if (seen == label) {
                return;
            }
        }
        out.push_back(label);
    };
    switch (term.term) {
        case ir::Term::Br:
            add(term.target);
            break;
        case ir::Term::CondBr:
            add(term.thenTarget);
            add(term.elseTarget);
            break;
        case ir::Term::Switch:
            for (const ir::Case& arm : term.cases) {
                add(arm.target);
            }
            add(term.defaultTarget);
            break;
        case ir::Term::Ret:
        case ir::Term::Unreachable:
        case ir::Term::Trap:
            break;
    }
    return out;
}

uint64_t blockIndex(const ir::Function& fn, uint32_t label) {
    for (uint64_t i = 0; i < fn.blocks.size(); ++i) {
        if (fn.blocks[i].label == label) {
            return i;
        }
    }
    return fn.blocks.size();
}

// Everything one successful inline adds, built off to the side so a refusal
// part-way through leaves the caller untouched.
struct Plan {
    bool ok = false;
    uint32_t entryLabel = 0;
    uint32_t continuationLabel = 0;
    std::vector<ir::Block> clones;
    std::vector<ir::Instr> prologue;
    std::unordered_map<uint32_t, ir::Value> resultSubst;
    std::vector<ir::Instr> returnPhis;
    uint64_t addedInstrs = 0;
};

// Build the clone for one direct call. `callee` is const; nothing in `caller`
// is read except its current maximum ids, which the caller passes in.
Plan planInline(const ir::Function& callee, const ir::Instr& call,
                uint32_t maxValue, uint32_t maxLabel) {
    Plan plan;
    if (callee.blocks.empty()) {
        return plan;
    }

    // Parameter slots are the only values whose mapping is not their own
    // result id: every callee param becomes a fresh Alloca result.
    std::unordered_set<uint32_t> paramIds;
    std::unordered_map<uint32_t, ir::Value> valueMap;
    uint64_t nextValue = static_cast<uint64_t>(maxValue) + 1;
    auto freshValue = [&](ir::IRType width, TypeRef type) {
        ir::Value value;
        value.id = static_cast<uint32_t>(nextValue++);
        value.width = width;
        value.type = type;
        return value;
    };
    for (const ir::Value& param : callee.params) {
        if (param.id == kInvalidValue || param.width != ir::IRType::Ptr ||
            nextValue > static_cast<uint64_t>(kLastFreshId) ||
            !paramIds.insert(param.id).second) {
            return plan;
        }
        valueMap[param.id] = freshValue(param.width, param.type);
    }

    // Every other definition gets a fresh id preserving width and type. A
    // param's entry Alloca is the one allowed repeat and is not cloned: the
    // fresh slot above replaces it.
    for (const ir::Block& block : callee.blocks) {
        for (const ir::Instr& instr : block.body) {
            if (instr.op == ir::Op::Alloca && !instr.results.empty() &&
                paramIds.find(instr.results[0].id) != paramIds.end()) {
                if (instr.results.size() != kOneResult) {
                    return plan;
                }
                continue;
            }
            for (const ir::Value& result : instr.results) {
                if (paramIds.find(result.id) != paramIds.end() ||
                    valueMap.find(result.id) != valueMap.end() ||
                    nextValue > static_cast<uint64_t>(kLastFreshId)) {
                    return plan;
                }
                valueMap[result.id] = freshValue(result.width, result.type);
            }
        }
    }

    // One fresh label per callee block, then the continuation.
    std::unordered_map<uint32_t, uint32_t> labelMap;
    uint64_t nextLabel = static_cast<uint64_t>(maxLabel) + 1;
    auto freshLabel = [&]() {
        return static_cast<uint32_t>(nextLabel++);
    };
    for (const ir::Block& block : callee.blocks) {
        if (labelMap.find(block.label) != labelMap.end() ||
            nextLabel > static_cast<uint64_t>(kLastFreshId)) {
            return plan;
        }
        labelMap[block.label] = freshLabel();
    }
    if (nextLabel > static_cast<uint64_t>(kLastFreshId)) {
        return plan;
    }
    plan.entryLabel = labelMap[callee.blocks.front().label];
    plan.continuationLabel = freshLabel();

    // Flatten the call arguments in word order. The first word belongs to
    // the first param, and so on; a shortfall or a surplus is a shape this
    // slice does not claim to understand, so it stays a call.
    std::vector<ir::Value> words;
    for (const ir::Argument& arg : call.arguments) {
        for (const ir::Value& word : arg.words) {
            words.push_back(word);
        }
    }
    if (words.size() != callee.params.size()) {
        return plan;
    }

    // The fresh parameter slots and their stores go into the caller prefix.
    for (uint64_t i = 0; i < callee.params.size(); ++i) {
        ir::Value slot = valueMap[callee.params[i].id];
        ir::Instr alloca;
        alloca.op = ir::Op::Alloca;
        alloca.loc = call.loc;
        alloca.width = slot.width;
        alloca.type = slot.type;
        alloca.isPure = false;
        alloca.results.push_back(slot);
        plan.prologue.push_back(std::move(alloca));

        ir::Value stored = words[i];
        ir::Instr store;
        store.op = ir::Op::Store;
        store.loc = call.loc;
        store.width = stored.width == ir::IRType::Void ? slot.width
                                                       : stored.width;
        store.type = stored.type;
        if (stored.width == ir::IRType::Void) {
            stored.width = store.width;
        }
        store.storedValue = stored;
        store.addressBase = slot;
        plan.prologue.push_back(std::move(store));
    }
    plan.addedInstrs += plan.prologue.size();

    // Return validation happens before a single clone is built. A single
    // Ret substitutes directly; several need one phi per result word.
    std::vector<uint64_t> retBlocks;
    for (uint64_t i = 0; i < callee.blocks.size(); ++i) {
        if (callee.blocks[i].term.term == ir::Term::Ret) {
            retBlocks.push_back(i);
        }
    }
    if (retBlocks.empty()) {
        return plan;
    }
    if (!call.results.empty()) {
        for (uint64_t index : retBlocks) {
            const ir::Terminator& ret = callee.blocks[index].term;
            if (ret.values.size() != call.results.size()) {
                return plan;
            }
            for (uint64_t i = 0; i < ret.values.size(); ++i) {
                if (ret.values[i].width != call.results[i].width ||
                    !(ret.values[i].type == call.results[i].type)) {
                    return plan;
                }
            }
        }
    }

    std::vector<ir::Value> phiResults;
    if (!call.results.empty() && retBlocks.size() > kOneResult) {
        for (const ir::Value& result : call.results) {
            if (nextValue > static_cast<uint64_t>(kLastFreshId)) {
                return plan;
            }
            phiResults.push_back(freshValue(result.width, result.type));
        }
    }

    // Clone every block and instruction. Param Allocas are the only dropped
    // instructions; their slots were emitted above.
    for (const ir::Block& source : callee.blocks) {
        ir::Block clone;
        clone.label = labelMap[source.label];
        clone.loc = source.loc;
        for (const ir::Instr& instr : source.body) {
            if (instr.op == ir::Op::Alloca && !instr.results.empty() &&
                paramIds.find(instr.results[0].id) != paramIds.end()) {
                continue;
            }
            ir::Instr copy = instr;
            for (ir::Value& result : copy.results) {
                auto found = valueMap.find(result.id);
                if (found == valueMap.end()) {
                    return plan;
                }
                result = found->second;
            }
            if (!remapInstrUses(copy, valueMap)) {
                return plan;
            }
            // Phi predecessor labels are CFG references, not values: they
            // move through the block map, not the value map. A callee that
            // was itself a caller may already carry return phis from an
            // earlier inline, and those labels must move with it.
            for (ir::PhiIncoming& incoming : copy.phiIncoming) {
                auto found = labelMap.find(incoming.block);
                if (found == labelMap.end()) {
                    return plan;
                }
                incoming.block = found->second;
            }
            clone.body.push_back(std::move(copy));
            ++plan.addedInstrs;
        }

        const ir::Terminator& sourceTerm = source.term;
        if (sourceTerm.term == ir::Term::Ret) {
            // Every return goes to the continuation; its values were
            // validated above and are read through the map below.
            ir::Terminator br;
            br.term = ir::Term::Br;
            br.loc = sourceTerm.loc;
            br.target = plan.continuationLabel;
            clone.term = br;
            plan.clones.push_back(std::move(clone));
            continue;
        }
        ir::Terminator copy = sourceTerm;
        if (!remapTerminatorUses(copy, valueMap)) {
            return plan;
        }
        auto remapTarget = [&](uint32_t& target) {
            auto found = labelMap.find(target);
            if (found == labelMap.end()) {
                return false;
            }
            target = found->second;
            return true;
        };
        switch (copy.term) {
            case ir::Term::Br:
                if (!remapTarget(copy.target)) {
                    return plan;
                }
                break;
            case ir::Term::CondBr:
                if (!remapTarget(copy.thenTarget) ||
                    !remapTarget(copy.elseTarget)) {
                    return plan;
                }
                break;
            case ir::Term::Switch:
                for (ir::Case& arm : copy.cases) {
                    if (!remapTarget(arm.target)) {
                        return plan;
                    }
                }
                if (!remapTarget(copy.defaultTarget)) {
                    return plan;
                }
                break;
            case ir::Term::Ret:
            case ir::Term::Unreachable:
            case ir::Term::Trap:
                break;
        }
        clone.term = std::move(copy);
        plan.clones.push_back(std::move(clone));
    }

    // Return values. Single Ret: direct substitution keeps the caller's
    // uses on the value the callee actually returned. Several paths: one
    // phi per result word, with one incoming per cloned Ret block.
    if (!call.results.empty()) {
        if (retBlocks.size() == kOneResult) {
            const ir::Terminator& ret = callee.blocks[retBlocks[0]].term;
            for (uint64_t i = 0; i < call.results.size(); ++i) {
                auto found = valueMap.find(ret.values[i].id);
                if (found == valueMap.end()) {
                    return plan;
                }
                plan.resultSubst[call.results[i].id] = found->second;
            }
        } else {
            for (uint64_t i = 0; i < call.results.size(); ++i) {
                ir::Instr phi;
                phi.op = ir::Op::Phi;
                phi.width = call.results[i].width;
                phi.type = call.results[i].type;
                phi.isPure = true;
                phi.results.push_back(phiResults[i]);
                for (uint64_t index : retBlocks) {
                    const ir::Terminator& ret = callee.blocks[index].term;
                    auto found = valueMap.find(ret.values[i].id);
                    if (found == valueMap.end()) {
                        return plan;
                    }
                    ir::PhiIncoming incoming;
                    incoming.value = found->second;
                    incoming.block = labelMap[callee.blocks[index].label];
                    phi.phiIncoming.push_back(incoming);
                }
                plan.resultSubst[call.results[i].id] = phiResults[i];
                plan.returnPhis.push_back(std::move(phi));
                ++plan.addedInstrs;
            }
        }
    }

    plan.ok = true;
    return plan;
}

// A call is eligible only when every refusal above says yes. This is a pure
// predicate; the counters are updated by the caller.
bool eligibleCall(const std::vector<ir::Function>& callees,
                  const std::unordered_map<std::string, uint64_t>& callSites,
                  const ir::Function& caller, const ir::Instr& call,
                  const ir::Function*& callee) {
    callee = nullptr;
    if (call.op != ir::Op::Call || call.call.indirect || call.callee.empty()) {
        return false;
    }
    callee = findFunction(callees, call.callee);
    if (callee == nullptr || callee == &caller || callee->isExtern ||
        callee->blocks.empty() || callee->name == caller.name) {
        return false;
    }
    if (hasAsm(*callee) || callsItself(*callee)) {
        return false;
    }
    const uint64_t size = instructionCount(*callee);
    if (callee->blocks.size() > kMaxCalleeBlocks ||
        size > kMaxCalleeInstrs) {
        return false;
    }
    if (size > kTinyCalleeInstrs) {
        auto found = callSites.find(callee->name);
        if (found != callSites.end() && found->second > kMaxCalleeSites) {
            return false;
        }
    }
    if (callee->signature.abi() != caller.signature.abi() ||
        call.call.msabi != (callee->signature.abi() == Abi::MsAbi)) {
        // The words are passed explicitly, so a mismatched convention is
        // very likely harmless; refusing it costs an inline and keeps this
        // slice from having to prove the ABI row is irrelevant.
        return false;
    }
    // Every callee block must be reachable: the clone's phis would otherwise
    // land in an unreachable region and the structural checker refuses them.
    const DominatorInfo doms = computeDominators(*callee);
    if (doms.reachable.size() != callee->blocks.size()) {
        return false;
    }
    return true;
}

}  // namespace

InlineResult inlineCalls(ir::Module& module) {
    InlineResult result;
    // Clone from the bodies as they arrived. A callee that has itself been a
    // caller would otherwise carry its own inlined clones into this caller,
    // turning a chain of small calls into nested copies and doubling large
    // programs. One level per call site means one original body per site.
    const std::vector<ir::Function> callees = module.functions;
    std::unordered_map<std::string, uint64_t> callSites;
    for (const ir::Function& fn : callees) {
        for (const ir::Block& block : fn.blocks) {
            for (const ir::Instr& instr : block.body) {
                if (instr.op == ir::Op::Call && !instr.call.indirect &&
                    !instr.callee.empty()) {
                    ++callSites[instr.callee];
                }
            }
        }
    }
    for (uint64_t fi = 0; fi < module.functions.size(); ++fi) {
        ir::Function& fn = module.functions[fi];
        if (fn.blocks.empty()) {
            continue;
        }
        // Reachability of the caller is fixed before any split. A call in an
        // unreachable block stays a call, for the same phi reason above.
        const DominatorInfo doms = computeDominators(fn);
        std::unordered_set<uint32_t> reachable = doms.reachable;

        // Fresh block labels along with cloned instructions are appended at
        // the end of the function. They must never be scanned for another
        // inline: that is the one-level rule.
        std::unordered_set<uint32_t> clonedBlocks;
        uint64_t sites = 0;
        uint64_t clonedInstrs = 0;

        uint64_t bi = 0;
        while (bi < fn.blocks.size()) {
            const uint32_t label = fn.blocks[bi].label;
            if (clonedBlocks.find(label) != clonedBlocks.end()) {
                ++bi;
                continue;
            }
            if (reachable.find(label) == reachable.end()) {
                ++bi;
                continue;
            }

            bool inlinedHere = false;
            uint64_t index = 0;
            while (index < fn.blocks[bi].body.size()) {
                ir::Instr& instr = fn.blocks[bi].body[index];
                if (instr.op != ir::Op::Call) {
                    ++index;
                    continue;
                }
                const ir::Function* callee = nullptr;
                if (!eligibleCall(callees, callSites, fn, instr, callee)) {
                    ++result.skipped;
                    ++index;
                    continue;
                }
                if (sites >= kMaxSitesPerFunction) {
                    ++result.skipped;
                    ++index;
                    continue;
                }
                Plan plan = planInline(*callee, instr, maxValueId(fn),
                                       maxLabelId(fn));
                if (!plan.ok) {
                    ++result.skipped;
                    ++index;
                    continue;
                }
                if (clonedInstrs + plan.addedInstrs >
                    kMaxClonedInstrsPerFunction) {
                    ++result.skipped;
                    ++index;
                    continue;
                }

                // Success. Capture what the split needs before moving the
                // instruction vectors around.
                const ir::Instr callCopy = instr;
                ir::Block& block = fn.blocks[bi];
                const ir::Terminator originalTerm = block.term;

                std::vector<ir::Instr> prefix;
                prefix.reserve(index + plan.prologue.size());
                for (uint64_t i = 0; i < index; ++i) {
                    prefix.push_back(std::move(block.body[i]));
                }
                for (ir::Instr& slot : plan.prologue) {
                    prefix.push_back(std::move(slot));
                }
                std::vector<ir::Instr> suffix;
                suffix.reserve(block.body.size() - index - kOneResult);
                for (uint64_t i = index + kOneResult; i < block.body.size(); ++i) {
                    suffix.push_back(std::move(block.body[i]));
                }

                ir::Terminator toClone;
                toClone.term = ir::Term::Br;
                toClone.loc = callCopy.loc;
                toClone.target = plan.entryLabel;
                block.body = std::move(prefix);
                block.term = toClone;

                ir::Block continuation;
                continuation.label = plan.continuationLabel;
                continuation.loc = callCopy.loc;
                continuation.body = std::move(plan.returnPhis);
                for (ir::Instr& rest : suffix) {
                    continuation.body.push_back(std::move(rest));
                }
                continuation.term = originalTerm;

                // The continuation now owns the original terminator's edges.
                // Every phi in a successor that named the split block must
                // name the continuation instead, or the phi's predecessor
                // set no longer matches the CFG.
                for (uint32_t target : successorLabels(originalTerm)) {
                    uint64_t at = blockIndex(fn, target);
                    if (at == fn.blocks.size()) {
                        continue;
                    }
                    for (ir::Instr& phi : fn.blocks[at].body) {
                        if (phi.op != ir::Op::Phi) {
                            continue;
                        }
                        for (ir::PhiIncoming& incoming : phi.phiIncoming) {
                            if (incoming.block == label) {
                                incoming.block = continuation.label;
                            }
                        }
                    }
                }

                // Keep the vector order close to control-flow order:
                // prefix, cloned body, continuation, then the original
                // successors. Slot forwarding walks blocks in this order and
                // only carries state across an edge whose producer came
                // first; appending the clones after the continuation made
                // the prefix's state unreachable to its own clone and let a
                // forwarded load chain through a deleted one.
                uint64_t insertAt = bi + kOneResult;
                for (ir::Block& clone : plan.clones) {
                    clonedBlocks.insert(clone.label);
                    fn.blocks.insert(fn.blocks.begin() + insertAt,
                                     std::move(clone));
                    ++insertAt;
                }
                fn.blocks.insert(fn.blocks.begin() + insertAt,
                                 std::move(continuation));
                replaceUses(fn, plan.resultSubst);

                reachable.insert(plan.continuationLabel);
                ++sites;
                clonedInstrs += plan.addedInstrs;
                ++result.inlined;
                result.clonedInstructions += plan.addedInstrs;
                inlinedHere = true;
                break;
            }

            if (inlinedHere) {
                // The continuation sits at bi+1: process its suffix next,
                // which is how a second call in the original block is
                // reached without ever revisiting the cloned body.
                ++bi;
                continue;
            }
            ++bi;
        }
    }
    return result;
}

}  // namespace opt
}  // namespace cider
