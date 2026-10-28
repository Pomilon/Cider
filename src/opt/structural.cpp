// structural.cpp — gate L4, executable.
//
// "Every block terminated, every target valid" is checked at the boundary,
// before any pass runs. Five properties, each naming what failed so the
// diagnostic points at the lowering bug rather than at the pass that tripped
// over it:
//
//   1. every block has a terminator — a block still holding the default
//      Unreachable is one the lowerer forgot to terminate, and codegen
//      would trap on it at runtime instead of failing the compile
//   2. every branch target names a block of the same function — a dangling
//      target reads as a label defined later, or never
//   3. every use is defined and its definition dominates the use, or is a
//      function parameter — otherwise a pass reads garbage
//   4. every value is defined exactly once — otherwise SSA is a claim, and
//      two definitions of one id is the shape a miswired emitter makes
//   5. every phi has one incoming per reachable predecessor, those
//      predecessors are real, and each incoming value dominates the edge;
//      phis lead their block and never sit in the entry block
//
// What this deliberately does *not* assert: a block's immediate dominator is
// not required to have a lower index than the block it dominates. That was
// never a property to check — v2 assumed it and crashed with `map::at`.

#include "opt/structural.h"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "opt/dominance.h"

namespace cider {
namespace opt {

namespace {

constexpr uint64_t kOneResult = 1;
constexpr uint64_t kOneOperand = 1;

bool namesBase(ir::Op op) {
    return op == ir::Op::Load || op == ir::Op::Store ||
           op == ir::Op::Extract || op == ir::Op::Insert ||
           op == ir::Op::Gep;
}

struct Definition {
    uint32_t block = 0;
    int64_t index = -1;  // -1 for a function parameter, else the body index
};

using Definitions = std::unordered_map<uint32_t, Definition>;

void checkTerminated(const ir::Function& fn, std::vector<StructuralError>& errors) {
    for (const ir::Block& block : fn.blocks) {
        if (block.term.term == ir::Term::Unreachable) {
            errors.push_back(StructuralError{
                fn.name, block.label,
                "block " + std::to_string(block.label) + " is unterminated"});
        }
    }
}

void checkTargets(const ir::Function& fn, const std::unordered_set<uint32_t>& labels,
                  std::vector<StructuralError>& errors) {
    auto missing = [&](const std::string& what, uint32_t label, uint32_t target) {
        errors.push_back(StructuralError{
            fn.name, label, what + " targets missing block " + std::to_string(target)});
    };
    for (const ir::Block& block : fn.blocks) {
        const ir::Terminator& term = block.term;
        if (term.term == ir::Term::Br && labels.find(term.target) == labels.end()) {
            missing("br", block.label, term.target);
        }
        if (term.term == ir::Term::CondBr) {
            if (labels.find(term.thenTarget) == labels.end()) {
                missing("branch", block.label, term.thenTarget);
            }
            if (labels.find(term.elseTarget) == labels.end()) {
                missing("branch", block.label, term.elseTarget);
            }
        }
        if (term.term == ir::Term::Switch) {
            for (const ir::Case& arm : term.cases) {
                if (labels.find(arm.target) == labels.end()) {
                    missing("switch", block.label, arm.target);
                }
            }
            if (labels.find(term.defaultTarget) == labels.end()) {
                missing("switch", block.label, term.defaultTarget);
            }
        }
    }
}

// Collect the single definition of every value. Parameters are defined before
// the entry block's first instruction; an Alloca may repeat a parameter id by
// design, because the slot is the parameter's home until mem2reg promotes it.
void collectDefinitions(const ir::Function& fn, Definitions& defs,
                        std::vector<StructuralError>& errors) {
    std::unordered_set<uint32_t> params;
    const uint32_t entry = fn.blocks.empty() ? 0 : fn.blocks.front().label;
    for (const ir::Value& param : fn.params) {
        params.insert(param.id);
        defs[param.id] = Definition{entry, -1};
    }
    for (const ir::Block& block : fn.blocks) {
        for (uint64_t index = 0; index < block.body.size(); ++index) {
            const ir::Instr& instr = block.body[index];
            for (const ir::Value& result : instr.results) {
                if (instr.op == ir::Op::Alloca &&
                    params.find(result.id) != params.end()) {
                    continue;
                }
                auto found = defs.find(result.id);
                if (found != defs.end()) {
                    errors.push_back(
                        StructuralError{fn.name, block.label,
                                        "value %" + std::to_string(result.id) +
                                            " defined twice"});
                    continue;
                }
                defs[result.id] = Definition{
                    block.label, static_cast<int64_t>(index)};
            }
        }
    }
}

void checkUse(const ir::Function& fn, const Definitions& defs,
              const DominatorInfo& dominators, uint32_t id, uint32_t block,
              uint64_t index, std::vector<StructuralError>& errors) {
    if (id == 0xFFFFFFFFu) {
        return;
    }
    auto found = defs.find(id);
    if (found == defs.end()) {
        errors.push_back(StructuralError{
            fn.name, block, "use of undefined value %" + std::to_string(id)});
        return;
    }
    const Definition& def = found->second;
    const bool reachable =
        dominators.reachable.find(block) != dominators.reachable.end();
    if (!reachable) {
        // Unreachable code is still checked for definition order in its own
        // block, but dominance has no meaning without a path from entry.
        if (def.block == block && def.index >= 0 &&
            static_cast<uint64_t>(def.index) >= index) {
            errors.push_back(StructuralError{
                fn.name, block,
                "value %" + std::to_string(id) + " used before its definition"});
        }
        return;
    }
    if (def.block == block) {
        if (def.index >= 0 &&
            static_cast<uint64_t>(def.index) >= index) {
            errors.push_back(StructuralError{
                fn.name, block,
                "value %" + std::to_string(id) + " used before its definition"});
        }
        return;
    }
    if (!dominators.dominates(def.block, block)) {
        errors.push_back(StructuralError{
            fn.name, block,
            "value %" + std::to_string(id) +
                " is used in a block its definition does not dominate"});
    }
}

void checkInstructionUses(const ir::Function& fn, const Definitions& defs,
                          const DominatorInfo& dominators,
                          std::vector<StructuralError>& errors) {
    for (const ir::Block& block : fn.blocks) {
        for (uint64_t index = 0; index < block.body.size(); ++index) {
            const ir::Instr& instr = block.body[index];
            for (const ir::Value& operand : instr.operands) {
                checkUse(fn, defs, dominators, operand.id, block.label, index,
                         errors);
            }
            for (const ir::Argument& arg : instr.arguments) {
                for (const ir::Value& word : arg.words) {
                    checkUse(fn, defs, dominators, word.id, block.label, index,
                             errors);
                }
            }
            if (namesBase(instr.op) && instr.globalLabel.empty()) {
                checkUse(fn, defs, dominators, instr.addressBase.id, block.label,
                         index, errors);
            }
            if (instr.op == ir::Op::Store || instr.op == ir::Op::Insert) {
                checkUse(fn, defs, dominators, instr.storedValue.id, block.label,
                         index, errors);
            }
            if (instr.op == ir::Op::Call && instr.call.indirect) {
                checkUse(fn, defs, dominators, instr.calleeValue.id, block.label,
                         index, errors);
            }
        }
        const uint64_t afterBody = block.body.size();
        const ir::Terminator& term = block.term;
        for (const ir::Value& value : term.values) {
            checkUse(fn, defs, dominators, value.id, block.label, afterBody,
                     errors);
        }
        if (term.cond.has_value()) {
            checkUse(fn, defs, dominators, term.cond->id, block.label, afterBody,
                     errors);
        }
        if (term.term == ir::Term::Switch) {
            checkUse(fn, defs, dominators, term.switchValue.id, block.label,
                     afterBody, errors);
            for (const ir::Case& arm : term.cases) {
                checkUse(fn, defs, dominators, arm.value.id, block.label,
                         afterBody, errors);
            }
        }
    }
}

// VARIADICS.md §8: the two va operations have one legal shape each, and a
// malformed one is a lowering bug that the target would otherwise read as
// plausible bytes. VaStart lives only in a variadic function, initializes a
// cursor, and answers nothing; VaArg reads one scalar value, so it names
// exactly one result at a non-void width. Both take their `*va_list` in
// `operands`, which is also what keeps the address's definition dominating
// the use (checkInstructionUses above walks it).
bool scalarResultKind(Kind kind) {
    return kind == Kind::Basic || kind == Kind::Pointer ||
           kind == Kind::FunctionPtr || kind == Kind::Enum;
}

void checkVaOperations(const ir::Function& fn,
                       std::vector<StructuralError>& errors) {
    for (const ir::Block& block : fn.blocks) {
        for (const ir::Instr& instr : block.body) {
            if (instr.op != ir::Op::VaStart && instr.op != ir::Op::VaArg) {
                continue;
            }
            const std::string name = ir::opName(instr.op);
            const bool pointerOperand =
                instr.operands.size() == kOneOperand &&
                instr.operands[0].width == ir::IRType::Ptr &&
                !instr.operands[0].type.isError() &&
                (instr.operands[0].type.type->kind() == Kind::Pointer ||
                 instr.operands[0].type.type->kind() == Kind::FunctionPtr);
            if (!pointerOperand) {
                errors.push_back(StructuralError{
                    fn.name, block.label,
                    name + " needs exactly one pointer operand"});
            }
            if (instr.op == ir::Op::VaStart) {
                if (!fn.signature.variadic()) {
                    errors.push_back(StructuralError{
                        fn.name, block.label,
                        "va_start outside a variadic function"});
                }
                if (!instr.results.empty()) {
                    errors.push_back(StructuralError{
                        fn.name, block.label, "va_start has no result"});
                }
                continue;
            }
            if (instr.results.size() != kOneResult) {
                errors.push_back(StructuralError{
                    fn.name, block.label, "va_arg needs exactly one result"});
                continue;
            }
            if (instr.width == ir::IRType::Void ||
                instr.results[0].width != instr.width) {
                errors.push_back(StructuralError{
                    fn.name, block.label,
                    "va_arg needs a non-void result width"});
            }
            if (instr.type.isError() ||
                instr.results[0].type != instr.type ||
                !scalarResultKind(instr.type.type->kind())) {
                errors.push_back(StructuralError{
                    fn.name, block.label,
                    "va_arg needs one scalar result type"});
            }
        }
    }
}

void checkPhis(const ir::Function& fn, const Definitions& defs,
               const DominatorInfo& dominators,
               std::vector<StructuralError>& errors) {
    if (fn.blocks.empty()) {
        return;
    }
    const uint32_t entry = fn.blocks.front().label;
    std::unordered_map<uint32_t, uint64_t> blockIndex;
    for (uint64_t i = 0; i < fn.blocks.size(); ++i) {
        blockIndex[fn.blocks[i].label] = i;
    }
    for (const ir::Block& block : fn.blocks) {
        bool nonPhiSeen = false;
        for (const ir::Instr& instr : block.body) {
            if (instr.op != ir::Op::Phi) {
                nonPhiSeen = true;
                continue;
            }
            if (nonPhiSeen) {
                errors.push_back(StructuralError{
                    fn.name, block.label,
                    "a phi follows a non-phi instruction"});
            }
            if (block.label == entry) {
                errors.push_back(StructuralError{
                    fn.name, block.label, "a phi sits in the entry block"});
            }
            if (dominators.reachable.find(block.label) ==
                dominators.reachable.end()) {
                errors.push_back(StructuralError{
                    fn.name, block.label, "a phi sits in an unreachable block"});
            }
            if (instr.results.size() != kOneResult) {
                errors.push_back(StructuralError{
                    fn.name, block.label,
                    "a phi must have exactly one result"});
                continue;
            }
            const ir::Value& result = instr.results[0];
            if (result.width != instr.width || !(result.type == instr.type)) {
                errors.push_back(StructuralError{
                    fn.name, block.label,
                    "a phi result has the wrong width or type"});
            }
            std::unordered_set<uint32_t> expected;
            auto preds = dominators.predecessors.find(block.label);
            if (preds != dominators.predecessors.end()) {
                for (uint32_t pred : preds->second) {
                    if (dominators.reachable.find(pred) !=
                        dominators.reachable.end()) {
                        expected.insert(pred);
                    }
                }
            }
            std::unordered_set<uint32_t> seen;
            for (const ir::PhiIncoming& incoming : instr.phiIncoming) {
                if (!seen.insert(incoming.block).second) {
                    errors.push_back(StructuralError{
                        fn.name, block.label,
                        "a phi names predecessor " +
                            std::to_string(incoming.block) + " twice"});
                }
                if (expected.find(incoming.block) == expected.end()) {
                    errors.push_back(StructuralError{
                        fn.name, block.label,
                        "a phi names block " +
                            std::to_string(incoming.block) +
                            ", which is not a reachable predecessor"});
                }
                auto predAt = blockIndex.find(incoming.block);
                const uint64_t predBody =
                    predAt == blockIndex.end()
                        ? 0
                        : fn.blocks[predAt->second].body.size();
                checkUse(fn, defs, dominators, incoming.value.id,
                         incoming.block, predBody, errors);
                if (incoming.value.width != instr.width ||
                    !(incoming.value.type == instr.type)) {
                    errors.push_back(StructuralError{
                        fn.name, block.label,
                        "a phi incoming value has the wrong width or type"});
                }
            }
            if (seen.size() != expected.size()) {
                errors.push_back(StructuralError{
                    fn.name, block.label,
                    "a phi is missing an incoming for a reachable predecessor"});
            }
        }
    }
}

void checkDefs(const ir::Function& fn, const DominatorInfo& dominators,
               std::vector<StructuralError>& errors) {
    Definitions defs;
    collectDefinitions(fn, defs, errors);
    checkInstructionUses(fn, defs, dominators, errors);
    checkPhis(fn, defs, dominators, errors);
}

}  // namespace

std::vector<StructuralError> checkModule(const ir::Module& module) {
    std::vector<StructuralError> errors;
    for (const ir::Function& fn : module.functions) {
        if (fn.blocks.empty()) {
            errors.push_back(StructuralError{fn.name, 0, "a function has no blocks"});
            continue;
        }
        std::unordered_set<uint32_t> labels;
        for (const ir::Block& block : fn.blocks) {
            labels.insert(block.label);
        }
        checkTargets(fn, labels, errors);
        checkTerminated(fn, errors);
        const DominatorInfo dominators = computeDominators(fn);
        checkDefs(fn, dominators, errors);
        checkVaOperations(fn, errors);
    }
    return errors;
}

}  // namespace opt
}  // namespace cider
