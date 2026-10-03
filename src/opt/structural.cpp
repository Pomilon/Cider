// structural.cpp — gate L4, executable.
//
// "Every block terminated, every target valid" is checked at the boundary,
// before any pass runs. Three properties, each naming what failed so the
// diagnostic points at the lowering bug rather than at the pass that tripped
// over it:
//
//   1. every branch target names a block of the same function — a dangling
//      target reads as a label defined later, or never
//   2. every operand is defined before use, or is a function parameter —
//      otherwise a pass reads garbage
//   3. every value is defined exactly once — otherwise SSA is a claim, and
//      two definitions of one id is the shape a miswired emitter makes
//
// What this deliberately does *not* assert: a block's immediate dominator is
// not required to have a lower index than the block it dominates. That was
// never a property to check — v2 assumed it and crashed with `map::at`.

#include "opt/structural.h"

#include <unordered_map>
#include <unordered_set>

namespace cider {
namespace opt {

namespace {

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

void checkDefs(const ir::Function& fn, std::vector<StructuralError>& errors) {
    // Defined before use, in linear order: parameters first, then each result
    // as its instruction appears. A use of an id with no definition is a pass
    // reading garbage; two definitions of one id is not SSA.
    std::unordered_set<uint32_t> defined;
    std::unordered_set<uint32_t> slots;
    for (const ir::Value& param : fn.params) {
        defined.insert(param.id);
        slots.insert(param.id);
    }
    auto use = [&](uint32_t block, uint32_t id) {
        if (defined.find(id) == defined.end() && id != 0xFFFFFFFFu) {
            errors.push_back(StructuralError{
                fn.name, block, "use of undefined value %" + std::to_string(id)});
        }
    };
    for (const ir::Block& block : fn.blocks) {
        for (const ir::Instr& instr : block.body) {
            for (const ir::Value& operand : instr.operands) {
                use(block.label, operand.id);
            }
            for (const ir::Argument& arg : instr.arguments) {
                use(block.label, arg.value.id);
            }
            for (const ir::Value& result : instr.results) {
                // A slot comes into existence twice by design: once as a
                // function parameter and once as its Alloca, and both name
                // the same id. Anything else defined twice is not SSA.
                bool slot = instr.op == ir::Op::Alloca &&
                            slots.find(result.id) != slots.end();
                if (!slot && defined.find(result.id) != defined.end()) {
                    errors.push_back(
                        StructuralError{fn.name, block.label,
                                        "value %" + std::to_string(result.id) +
                                            " defined twice"});
                }
                defined.insert(result.id);
            }
        }
        const ir::Terminator& term = block.term;
        if (term.value.has_value()) {
            use(block.label, term.value->id);
        }
        if (term.cond.has_value()) {
            use(block.label, term.cond->id);
        }
        if (term.term == ir::Term::Switch) {
            use(block.label, term.switchValue.id);
        }
    }
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
        checkDefs(fn, errors);
    }
    return errors;
}

}  // namespace opt
}  // namespace cider
