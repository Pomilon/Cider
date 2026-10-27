// simplify_test.cpp — constant control-flow folding and unreachable blocks.
//
// The pass runs after constant folding in the pipeline, so these tests build
// IR directly: a CondBr or Switch over a constant, the phis a removed edge
// leaves behind, and the values it must refuse to guess at. Every transform is
// followed by the structural checker, which is the same gate the pipeline runs
// after this pass.

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "ir/ir.h"
#include "opt/opt.h"
#include "opt/structural.h"
#include "types/context.h"

namespace {

int failures = 0;
int checks = 0;

void check(bool holds, const std::string& what) {
    ++checks;
    if (holds) {
        std::printf("  ok   %s\n", what.c_str());
    } else {
        std::printf("  FAIL %s\n", what.c_str());
        ++failures;
    }
}

bool structurallyValid(const cider::ir::Module& module) {
    return cider::opt::checkModule(module).empty();
}

const cider::ir::Block* blockWithLabel(const cider::ir::Function& fn,
                                       uint32_t label) {
    for (const cider::ir::Block& block : fn.blocks) {
        if (block.label == label) {
            return &block;
        }
    }
    return nullptr;
}

int countOp(const cider::ir::Function& fn, cider::ir::Op op) {
    int n = 0;
    for (const cider::ir::Block& block : fn.blocks) {
        for (const cider::ir::Instr& instr : block.body) {
            if (instr.op == op) {
                ++n;
            }
        }
    }
    return n;
}

std::string blockLabels(const cider::ir::Function& fn) {
    std::string labels;
    for (const cider::ir::Block& block : fn.blocks) {
        labels += std::to_string(block.label) + ",";
    }
    return labels;
}

// A stable description of the shape the structural dump does not print:
// block order, terminators, and phi incoming lists. Type pointers are never
// printed, so two independently built modules can be compared.
std::string describe(const cider::ir::Function& fn) {
    std::string out;
    for (const cider::ir::Block& block : fn.blocks) {
        out += "block " + std::to_string(block.label) + " term " +
               std::to_string(static_cast<int>(block.term.term)) + " tgt " +
               std::to_string(block.term.target) + " then " +
               std::to_string(block.term.thenTarget) + " else " +
               std::to_string(block.term.elseTarget) + " default " +
               std::to_string(block.term.defaultTarget) + "\n";
        for (const cider::ir::Instr& instr : block.body) {
            out += "  " + std::string(cider::ir::opName(instr.op));
            for (const cider::ir::Value& result : instr.results) {
                out += " r" + std::to_string(result.id);
            }
            for (const cider::ir::PhiIncoming& incoming : instr.phiIncoming) {
                out += " [" + std::to_string(incoming.value.id) + "," +
                       std::to_string(incoming.block) + "]";
            }
            out += "\n";
        }
        for (const cider::ir::Value& word : block.term.values) {
            out += "  ret " + std::to_string(word.id) + "\n";
        }
        if (block.term.cond.has_value()) {
            out += "  cond " + std::to_string(block.term.cond->id) + "\n";
        }
        if (block.term.term == cider::ir::Term::Switch) {
            out += "  switch " + std::to_string(block.term.switchValue.id) +
                   "\n";
            for (const cider::ir::Case& arm : block.term.cases) {
                out += "  case " + std::to_string(arm.value.id) + " -> " +
                       std::to_string(arm.target) + "\n";
            }
        }
    }
    return out;
}

struct Ir {
    cider::TypeContext types;
    cider::ir::Module module;
    cider::ir::Function* fn = nullptr;

    Ir() {
        module.name = "test";
        cider::ir::Function function;
        function.name = "f";
        cider::ir::Block block;
        block.label = 0;
        function.blocks.push_back(block);
        module.functions.push_back(std::move(function));
        fn = &module.functions.back();
    }

    cider::TypeRef i32() {
        return cider::TypeRef{types.basic(cider::BasicKind::I32)};
    }
    cider::TypeRef i64() {
        return cider::TypeRef{types.basic(cider::BasicKind::I64)};
    }
    cider::TypeRef boolean() {
        return cider::TypeRef{types.basic(cider::BasicKind::Bool)};
    }
    cider::TypeRef ptr() {
        return cider::TypeRef{types.pointer(types.basic(cider::BasicKind::U8))};
    }

    cider::ir::Value value(uint32_t id, cider::ir::IRType width,
                           cider::TypeRef type) {
        cider::ir::Value out;
        out.id = id;
        out.width = width;
        out.type = type;
        return out;
    }

    void param(uint32_t id, cider::ir::IRType width, cider::TypeRef type) {
        fn->params.push_back(value(id, width, type));
    }

    cider::ir::Block& addBlock(uint32_t label) {
        cider::ir::Block block;
        block.label = label;
        fn->blocks.push_back(std::move(block));
        return fn->blocks.back();
    }

    cider::ir::Block* blockAt(uint32_t label) {
        for (cider::ir::Block& block : fn->blocks) {
            if (block.label == label) {
                return &block;
            }
        }
        return nullptr;
    }

    void emit(cider::ir::Instr instr) {
        fn->blocks.back().body.push_back(std::move(instr));
    }

    cider::ir::Instr constant(uint32_t id, int64_t bits,
                              cider::ir::IRType width, cider::TypeRef type) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Const;
        instr.width = width;
        instr.type = type;
        instr.constantBits = bits;
        instr.constantText = std::to_string(bits);
        instr.isPure = true;
        instr.results.push_back(value(id, width, type));
        return instr;
    }

    cider::ir::Instr copy(uint32_t id, cider::ir::Value source,
                          cider::ir::IRType width, cider::TypeRef type) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Copy;
        instr.width = width;
        instr.type = type;
        instr.isPure = true;
        instr.results.push_back(value(id, width, type));
        instr.operands.push_back(source);
        return instr;
    }

    cider::ir::Instr phi(
        uint32_t id, cider::ir::IRType width, cider::TypeRef type,
        const std::vector<std::pair<uint32_t, uint32_t>>& incoming) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Phi;
        instr.width = width;
        instr.type = type;
        instr.isPure = true;
        instr.results.push_back(value(id, width, type));
        for (const auto& pair : incoming) {
            cider::ir::PhiIncoming entry;
            entry.value = value(pair.first, width, type);
            entry.block = pair.second;
            instr.phiIncoming.push_back(entry);
        }
        return instr;
    }

    cider::ir::Instr binary(cider::ir::Op op, uint32_t id,
                            cider::ir::Value lhs, cider::ir::Value rhs,
                            cider::ir::IRType width, cider::TypeRef type) {
        cider::ir::Instr instr;
        instr.op = op;
        instr.width = width;
        instr.type = type;
        instr.isPure = true;
        instr.results.push_back(value(id, width, type));
        instr.operands.push_back(lhs);
        instr.operands.push_back(rhs);
        return instr;
    }

    cider::ir::Instr allocaSlot(uint32_t id, cider::TypeRef type) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Alloca;
        instr.width = cider::ir::IRType::Ptr;
        instr.type = cider::TypeRef{types.pointer(type.type)};
        instr.results.push_back(value(id, instr.width, instr.type));
        return instr;
    }

    cider::ir::Instr store(uint32_t base, cider::ir::Value stored,
                           cider::ir::IRType width, cider::TypeRef type,
                           bool isVolatile = false) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Store;
        instr.width = width;
        instr.type = type;
        instr.addressBase = value(base, cider::ir::IRType::Ptr, ptr());
        instr.storedValue = stored;
        instr.isVolatile = isVolatile;
        return instr;
    }

    cider::ir::Instr load(uint32_t id, uint32_t base, cider::ir::IRType width,
                          cider::TypeRef type) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Load;
        instr.width = width;
        instr.type = type;
        instr.addressBase = value(base, cider::ir::IRType::Ptr, ptr());
        instr.isPure = true;
        instr.results.push_back(value(id, width, type));
        return instr;
    }

    cider::ir::Instr asmBlock() {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Asm;
        instr.width = cider::ir::IRType::Void;
        instr.asmText = "nop";
        return instr;
    }

    cider::ir::Instr trap() {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Trap;
        instr.width = cider::ir::IRType::Void;
        instr.reason = cider::ir::TrapReason::Explicit;
        instr.mayTrap = true;
        return instr;
    }

    void finishRet(uint32_t blockLabel, uint32_t id, cider::ir::IRType width,
                   cider::TypeRef type) {
        cider::ir::Terminator term;
        term.term = cider::ir::Term::Ret;
        term.values.push_back(value(id, width, type));
        blockAt(blockLabel)->term = term;
    }

    void finishBr(uint32_t blockLabel, uint32_t target) {
        cider::ir::Terminator term;
        term.term = cider::ir::Term::Br;
        term.target = target;
        blockAt(blockLabel)->term = term;
    }

    void finishCondBr(uint32_t blockLabel, cider::ir::Value cond,
                      uint32_t thenTarget, uint32_t elseTarget) {
        cider::ir::Terminator term;
        term.term = cider::ir::Term::CondBr;
        term.cond = cond;
        term.thenTarget = thenTarget;
        term.elseTarget = elseTarget;
        blockAt(blockLabel)->term = term;
    }

    void finishSwitch(uint32_t blockLabel, cider::ir::Value value,
                      const std::vector<std::pair<cider::ir::Value, uint32_t>>&
                          cases,
                      uint32_t defaultTarget) {
        cider::ir::Terminator term;
        term.term = cider::ir::Term::Switch;
        term.switchValue = value;
        for (const auto& pair : cases) {
            cider::ir::Case arm;
            arm.value = pair.first;
            arm.target = pair.second;
            term.cases.push_back(arm);
        }
        term.defaultTarget = defaultTarget;
        blockAt(blockLabel)->term = term;
    }
};

void testConstantTrueCond() {
    std::printf("constant true branch folds to the then edge\n");
    Ir ir;
    cider::TypeRef b = ir.boolean();
    cider::TypeRef t = ir.i32();
    ir.emit(ir.constant(1, 1, cider::ir::IRType::Bool, b));
    ir.finishCondBr(0, ir.value(1, cider::ir::IRType::Bool, b), 1, 2);
    ir.addBlock(1);
    ir.emit(ir.constant(2, 7, cider::ir::IRType::I32, t));
    ir.finishRet(1, 2, cider::ir::IRType::I32, t);
    ir.addBlock(2);
    ir.emit(ir.constant(3, 9, cider::ir::IRType::I32, t));
    ir.finishRet(2, 3, cider::ir::IRType::I32, t);

    cider::opt::SimplifyResult result = cider::opt::simplifyControlFlow(ir.module);
    check(structurallyValid(ir.module), "the result is structurally valid");
    check(blockLabels(*ir.fn) == "0,1,", "the untaken block is removed");
    check(ir.fn->blocks[0].term.term == cider::ir::Term::Br &&
              ir.fn->blocks[0].term.target == 1,
          "the branch becomes Br on the then edge");
    check(result.branchesFolded == 1, "one branch is counted");
    check(result.blocksRemoved == 1, "one block is counted");
    check(result.phisSimplified == 0 && result.phiIncomingRemoved == 0,
          "no phi counter moves without a phi");
}

void testConstantFalseCond() {
    std::printf("constant false branch folds to the else edge\n");
    Ir ir;
    cider::TypeRef b = ir.boolean();
    cider::TypeRef t = ir.i32();
    ir.emit(ir.constant(1, 0, cider::ir::IRType::Bool, b));
    ir.finishCondBr(0, ir.value(1, cider::ir::IRType::Bool, b), 1, 2);
    ir.addBlock(1);
    ir.emit(ir.constant(2, 7, cider::ir::IRType::I32, t));
    ir.finishRet(1, 2, cider::ir::IRType::I32, t);
    ir.addBlock(2);
    ir.emit(ir.constant(3, 9, cider::ir::IRType::I32, t));
    ir.finishRet(2, 3, cider::ir::IRType::I32, t);

    cider::opt::SimplifyResult result = cider::opt::simplifyControlFlow(ir.module);
    check(structurallyValid(ir.module), "the result is structurally valid");
    check(blockLabels(*ir.fn) == "0,2,", "the untaken then block is removed");
    check(ir.fn->blocks[0].term.term == cider::ir::Term::Br &&
              ir.fn->blocks[0].term.target == 2,
          "the branch becomes Br on the else edge");
    check(result.branchesFolded == 1 && result.blocksRemoved == 1,
          "the counters report one fold and one removal");
}

void testSwitchMatch() {
    std::printf("switch selects the matching case\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.emit(ir.constant(1, 2, cider::ir::IRType::I32, t));
    ir.emit(ir.constant(2, 1, cider::ir::IRType::I32, t));
    ir.emit(ir.constant(3, 3, cider::ir::IRType::I32, t));
    ir.emit(ir.constant(4, 2, cider::ir::IRType::I32, t));
    // Deliberately not in numeric order: first match wins, and that is case 4.
    ir.finishSwitch(0, ir.value(1, cider::ir::IRType::I32, t),
                    {{ir.value(2, cider::ir::IRType::I32, t), 1},
                     {ir.value(3, cider::ir::IRType::I32, t), 3},
                     {ir.value(4, cider::ir::IRType::I32, t), 2}},
                    4);
    for (uint32_t label = 1; label <= 4; ++label) {
        ir.addBlock(label);
        ir.emit(ir.constant(10 + label, 10 + label,
                            cider::ir::IRType::I32, t));
        ir.finishRet(label, 10 + label, cider::ir::IRType::I32, t);
    }

    cider::opt::SimplifyResult result = cider::opt::simplifyControlFlow(ir.module);
    check(structurallyValid(ir.module), "the result is structurally valid");
    check(blockLabels(*ir.fn) == "0,2,", "only the matching case target survives");
    check(ir.fn->blocks[0].term.term == cider::ir::Term::Br &&
              ir.fn->blocks[0].term.target == 2,
          "the switch becomes Br on the matching case");
    check(result.branchesFolded == 1 && result.blocksRemoved == 3,
          "the counters report one fold and three removals");
}

void testSwitchDefault() {
    std::printf("switch without a matching case takes the default\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.emit(ir.constant(1, 9, cider::ir::IRType::I32, t));
    ir.emit(ir.constant(2, 1, cider::ir::IRType::I32, t));
    ir.emit(ir.constant(3, 2, cider::ir::IRType::I32, t));
    ir.finishSwitch(0, ir.value(1, cider::ir::IRType::I32, t),
                    {{ir.value(2, cider::ir::IRType::I32, t), 1},
                     {ir.value(3, cider::ir::IRType::I32, t), 2}},
                    3);
    for (uint32_t label = 1; label <= 3; ++label) {
        ir.addBlock(label);
        ir.emit(ir.constant(10 + label, 10 + label,
                            cider::ir::IRType::I32, t));
        ir.finishRet(label, 10 + label, cider::ir::IRType::I32, t);
    }

    cider::opt::SimplifyResult result = cider::opt::simplifyControlFlow(ir.module);
    check(structurallyValid(ir.module), "the result is structurally valid");
    check(blockLabels(*ir.fn) == "0,3,", "only the default target survives");
    check(ir.fn->blocks[0].term.term == cider::ir::Term::Br &&
              ir.fn->blocks[0].term.target == 3,
          "the switch becomes Br on the default");
    check(result.branchesFolded == 1 && result.blocksRemoved == 2,
          "the counters report one fold and two removals");
}

void testNonconstantSwitchUnchanged() {
    std::printf("switch over a nonconstant value is untouched\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::I32, t);
    ir.emit(ir.constant(1, 1, cider::ir::IRType::I32, t));
    ir.finishSwitch(0, ir.value(0, cider::ir::IRType::I32, t),
                    {{ir.value(1, cider::ir::IRType::I32, t), 1}}, 2);
    for (uint32_t label = 1; label <= 2; ++label) {
        ir.addBlock(label);
        ir.emit(ir.constant(10 + label, 10 + label,
                            cider::ir::IRType::I32, t));
        ir.finishRet(label, 10 + label, cider::ir::IRType::I32, t);
    }

    cider::opt::SimplifyResult result = cider::opt::simplifyControlFlow(ir.module);
    check(structurallyValid(ir.module), "the result is structurally valid");
    check(ir.fn->blocks[0].term.term == cider::ir::Term::Switch,
          "the switch stays a switch");
    check(ir.fn->blocks.size() == 3, "no block is removed");
    check(result.branchesFolded == 0 && result.blocksRemoved == 0,
          "no counter moves");
}

void testUnknownEarlierCaseRefuses() {
    std::printf("an unknown earlier case refuses a switch fold\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::I32, t);
    ir.emit(ir.constant(1, 2, cider::ir::IRType::I32, t));
    ir.emit(ir.constant(2, 2, cider::ir::IRType::I32, t));
    // The first case is the parameter: it could equal 2, so the matching
    // constant case cannot be chosen safely.
    ir.finishSwitch(0, ir.value(1, cider::ir::IRType::I32, t),
                    {{ir.value(0, cider::ir::IRType::I32, t), 1},
                     {ir.value(2, cider::ir::IRType::I32, t), 2}},
                    3);
    for (uint32_t label = 1; label <= 3; ++label) {
        ir.addBlock(label);
        ir.emit(ir.constant(10 + label, 10 + label,
                            cider::ir::IRType::I32, t));
        ir.finishRet(label, 10 + label, cider::ir::IRType::I32, t);
    }

    cider::opt::SimplifyResult result = cider::opt::simplifyControlFlow(ir.module);
    check(structurallyValid(ir.module), "the result is structurally valid");
    check(ir.fn->blocks[0].term.term == cider::ir::Term::Switch,
          "the switch is left alone");
    check(ir.fn->blocks.size() == 4, "no block is removed");
    check(result.branchesFolded == 0 && result.blocksRemoved == 0,
          "no counter moves");
}

void testNestedConstantBranches() {
    std::printf("nested constant branches fold to a fixpoint\n");
    Ir ir;
    cider::TypeRef b = ir.boolean();
    cider::TypeRef t = ir.i32();
    ir.emit(ir.constant(1, 1, cider::ir::IRType::Bool, b));
    ir.finishCondBr(0, ir.value(1, cider::ir::IRType::Bool, b), 1, 2);
    ir.addBlock(1);
    ir.emit(ir.constant(2, 0, cider::ir::IRType::Bool, b));
    ir.finishCondBr(1, ir.value(2, cider::ir::IRType::Bool, b), 3, 4);
    ir.addBlock(2);
    ir.finishBr(2, 5);
    ir.addBlock(3);
    ir.finishBr(3, 5);
    ir.addBlock(4);
    ir.finishBr(4, 5);
    ir.addBlock(5);
    ir.emit(ir.constant(3, 7, cider::ir::IRType::I32, t));
    ir.finishRet(5, 3, cider::ir::IRType::I32, t);

    cider::opt::SimplifyResult result = cider::opt::simplifyControlFlow(ir.module);
    check(structurallyValid(ir.module), "the result is structurally valid");
    check(blockLabels(*ir.fn) == "0,1,4,5,",
          "blocks 2 and 3 are unreachable and removed");
    check(ir.fn->blocks[0].term.term == cider::ir::Term::Br &&
              ir.fn->blocks[0].term.target == 1,
          "the outer branch takes the then edge");
    check(ir.fn->blocks[1].term.term == cider::ir::Term::Br &&
              ir.fn->blocks[1].term.target == 4,
          "the inner branch takes the else edge");
    check(result.branchesFolded == 2 && result.blocksRemoved == 2,
          "both branches and both dead blocks are counted");
}

void testUnreachableBlockRemoved() {
    std::printf("an already-unreachable block is removed\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.emit(ir.constant(1, 5, cider::ir::IRType::I32, t));
    ir.finishRet(0, 1, cider::ir::IRType::I32, t);
    ir.addBlock(1);
    ir.emit(ir.trap());
    ir.emit(ir.asmBlock());
    ir.finishBr(1, 1);

    cider::opt::SimplifyResult result = cider::opt::simplifyControlFlow(ir.module);
    check(structurallyValid(ir.module), "the result is structurally valid");
    check(blockLabels(*ir.fn) == "0,", "the unreachable block is gone");
    check(result.branchesFolded == 0 && result.blocksRemoved == 1,
          "one block is counted and no branch changed");
}

void testReachableSideEffectsSurvive() {
    std::printf("reachable traps, volatile stores and asm stay put\n");
    Ir ir;
    cider::TypeRef b = ir.boolean();
    cider::TypeRef t = ir.i32();
    ir.emit(ir.allocaSlot(1, t));
    ir.emit(ir.constant(2, 3, cider::ir::IRType::I32, t));
    ir.emit(ir.store(1, ir.value(2, cider::ir::IRType::I32, t),
                    cider::ir::IRType::I32, t, true));
    ir.emit(ir.trap());
    ir.emit(ir.asmBlock());
    ir.emit(ir.constant(3, 1, cider::ir::IRType::Bool, b));
    ir.finishCondBr(0, ir.value(3, cider::ir::IRType::Bool, b), 1, 2);
    ir.addBlock(1);
    ir.emit(ir.constant(4, 7, cider::ir::IRType::I32, t));
    ir.finishRet(1, 4, cider::ir::IRType::I32, t);
    ir.addBlock(2);
    ir.emit(ir.constant(5, 9, cider::ir::IRType::I32, t));
    ir.finishRet(2, 5, cider::ir::IRType::I32, t);

    cider::opt::SimplifyResult result = cider::opt::simplifyControlFlow(ir.module);
    check(structurallyValid(ir.module), "the result is structurally valid");
    check(result.branchesFolded == 1 && result.blocksRemoved == 1,
          "the constant branch and its dead block are the only changes");
    check(countOp(*ir.fn, cider::ir::Op::Asm) == 1,
          "the reachable asm instruction survives");
    check(countOp(*ir.fn, cider::ir::Op::Trap) == 1,
          "the reachable trap instruction survives");
    check(countOp(*ir.fn, cider::ir::Op::Store) == 1,
          "the reachable volatile store survives");
    check(ir.fn->blocks[0].body.size() == 6,
          "no instruction in the kept block was deleted");
}

void testPhiIncomingAndSingleIncoming() {
    std::printf("a removed predecessor prunes phis and a lone phi collapses\n");
    Ir ir;
    cider::TypeRef b = ir.boolean();
    cider::TypeRef t = ir.i32();
    ir.emit(ir.constant(1, 1, cider::ir::IRType::Bool, b));
    ir.finishCondBr(0, ir.value(1, cider::ir::IRType::Bool, b), 1, 2);
    ir.addBlock(1);
    ir.emit(ir.constant(2, 10, cider::ir::IRType::I32, t));
    ir.finishBr(1, 3);
    ir.addBlock(2);
    ir.emit(ir.constant(3, 20, cider::ir::IRType::I32, t));
    ir.finishBr(2, 3);
    ir.addBlock(3);
    ir.emit(ir.phi(4, cider::ir::IRType::I32, t, {{2, 1}, {3, 2}}));
    ir.finishRet(3, 4, cider::ir::IRType::I32, t);

    cider::opt::SimplifyResult result = cider::opt::simplifyControlFlow(ir.module);
    check(structurallyValid(ir.module), "the result is structurally valid");
    check(blockLabels(*ir.fn) == "0,1,3,", "the untaken predecessor is gone");
    check(ir.fn->blocks[2].body.empty(), "the single-incoming phi is deleted");
    check(ir.fn->blocks[2].term.values.size() == 1 &&
              ir.fn->blocks[2].term.values[0].id == 2,
          "the return now names the surviving constant");
    check(result.phisSimplified == 1, "one phi is counted");
    check(result.phiIncomingRemoved == 1, "one incoming entry is counted");
}

void testMultiwordPhisAlignByLabel() {
    std::printf("multiword phis align incoming entries by block label\n");
    Ir ir;
    cider::TypeRef b = ir.boolean();
    cider::TypeRef t = ir.i64();
    ir.emit(ir.constant(1, 1, cider::ir::IRType::Bool, b));
    ir.finishCondBr(0, ir.value(1, cider::ir::IRType::Bool, b), 1, 2);
    ir.addBlock(1);
    ir.emit(ir.constant(2, 10, cider::ir::IRType::I64, t));
    ir.emit(ir.constant(3, 11, cider::ir::IRType::I64, t));
    ir.finishBr(1, 3);
    ir.addBlock(2);
    ir.emit(ir.constant(4, 20, cider::ir::IRType::I64, t));
    ir.emit(ir.constant(5, 21, cider::ir::IRType::I64, t));
    ir.finishBr(2, 3);
    ir.addBlock(3);
    // The two word phis deliberately list their predecessors in opposite
    // orders; pruning must follow the label, not the position.
    ir.emit(ir.phi(6, cider::ir::IRType::I64, t, {{2, 1}, {4, 2}}));
    ir.emit(ir.phi(7, cider::ir::IRType::I64, t, {{5, 2}, {3, 1}}));
    ir.emit(ir.binary(cider::ir::Op::Add, 8,
                      ir.value(6, cider::ir::IRType::I64, t),
                      ir.value(7, cider::ir::IRType::I64, t),
                      cider::ir::IRType::I64, t));
    ir.finishRet(3, 8, cider::ir::IRType::I64, t);

    cider::opt::SimplifyResult result = cider::opt::simplifyControlFlow(ir.module);
    check(structurallyValid(ir.module), "the result is structurally valid");
    check(blockLabels(*ir.fn) == "0,1,3,", "the untaken predecessor is gone");
    const cider::ir::Block* join = blockWithLabel(*ir.fn, 3);
    check(join != nullptr && join->body.size() == 1 &&
              join->body[0].op == cider::ir::Op::Add,
          "both phi words are deleted and the add remains");
    check(join != nullptr && join->body[0].operands.size() == 2 &&
              join->body[0].operands[0].id == 2 &&
              join->body[0].operands[1].id == 3,
          "the add names the first predecessor's two words, not block 2's");
    check(result.phisSimplified == 2, "both phi words are counted");
    check(result.phiIncomingRemoved == 2,
          "one incoming entry per word is counted");
}

void testNonconstantCondUnchanged() {
    std::printf("a nonconstant condition is untouched\n");
    Ir ir;
    cider::TypeRef b = ir.boolean();
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::Bool, b);
    ir.finishCondBr(0, ir.value(0, cider::ir::IRType::Bool, b), 1, 2);
    ir.addBlock(1);
    ir.emit(ir.constant(1, 7, cider::ir::IRType::I32, t));
    ir.finishRet(1, 1, cider::ir::IRType::I32, t);
    ir.addBlock(2);
    ir.emit(ir.constant(2, 9, cider::ir::IRType::I32, t));
    ir.finishRet(2, 2, cider::ir::IRType::I32, t);

    cider::opt::SimplifyResult result = cider::opt::simplifyControlFlow(ir.module);
    check(structurallyValid(ir.module), "the result is structurally valid");
    check(ir.fn->blocks[0].term.term == cider::ir::Term::CondBr,
          "the branch stays conditional");
    check(ir.fn->blocks.size() == 3, "no block is removed");
    check(result.branchesFolded == 0 && result.blocksRemoved == 0,
          "no counter moves");
}

void testLoadConditionRefused() {
    std::printf("a loaded condition is not guessed at\n");
    Ir ir;
    cider::TypeRef b = ir.boolean();
    cider::TypeRef t = ir.i32();
    ir.emit(ir.allocaSlot(1, b));
    ir.emit(ir.constant(2, 1, cider::ir::IRType::Bool, b));
    ir.emit(ir.store(1, ir.value(2, cider::ir::IRType::Bool, b),
                    cider::ir::IRType::Bool, b));
    ir.emit(ir.load(3, 1, cider::ir::IRType::Bool, b));
    ir.finishCondBr(0, ir.value(3, cider::ir::IRType::Bool, b), 1, 2);
    ir.addBlock(1);
    ir.emit(ir.constant(4, 7, cider::ir::IRType::I32, t));
    ir.finishRet(1, 4, cider::ir::IRType::I32, t);
    ir.addBlock(2);
    ir.emit(ir.constant(5, 9, cider::ir::IRType::I32, t));
    ir.finishRet(2, 5, cider::ir::IRType::I32, t);

    cider::opt::SimplifyResult result = cider::opt::simplifyControlFlow(ir.module);
    check(structurallyValid(ir.module), "the result is structurally valid");
    check(ir.fn->blocks[0].term.term == cider::ir::Term::CondBr,
          "the branch stays conditional through the load");
    check(ir.fn->blocks.size() == 3, "no block is removed");
    check(result.branchesFolded == 0, "the load is not resolved");
}

void testConstantThroughCopy() {
    std::printf("a constant survives a same-width Copy\n");
    Ir ir;
    cider::TypeRef b = ir.boolean();
    cider::TypeRef t = ir.i32();
    ir.emit(ir.constant(1, 1, cider::ir::IRType::Bool, b));
    ir.emit(ir.copy(2, ir.value(1, cider::ir::IRType::Bool, b),
                    cider::ir::IRType::Bool, b));
    ir.finishCondBr(0, ir.value(2, cider::ir::IRType::Bool, b), 1, 2);
    ir.addBlock(1);
    ir.emit(ir.constant(3, 7, cider::ir::IRType::I32, t));
    ir.finishRet(1, 3, cider::ir::IRType::I32, t);
    ir.addBlock(2);
    ir.emit(ir.constant(4, 9, cider::ir::IRType::I32, t));
    ir.finishRet(2, 4, cider::ir::IRType::I32, t);

    cider::opt::SimplifyResult result = cider::opt::simplifyControlFlow(ir.module);
    check(structurallyValid(ir.module), "the result is structurally valid");
    check(ir.fn->blocks[0].term.term == cider::ir::Term::Br &&
              ir.fn->blocks[0].term.target == 1,
          "the Copy chain reaches the constant");
    check(result.branchesFolded == 1 && result.blocksRemoved == 1,
          "the branch folds and the dead block is removed");
}

void testWidthChangingCopyRefused() {
    std::printf("a width-changing Copy is not resolved\n");
    Ir ir;
    cider::TypeRef b = ir.boolean();
    cider::TypeRef t = ir.i32();
    ir.emit(ir.constant(1, 1, cider::ir::IRType::I32, t));
    ir.emit(ir.copy(2, ir.value(1, cider::ir::IRType::I32, t),
                    cider::ir::IRType::Bool, b));
    ir.finishCondBr(0, ir.value(2, cider::ir::IRType::Bool, b), 1, 2);
    ir.addBlock(1);
    ir.emit(ir.constant(3, 7, cider::ir::IRType::I32, t));
    ir.finishRet(1, 3, cider::ir::IRType::I32, t);
    ir.addBlock(2);
    ir.emit(ir.constant(4, 9, cider::ir::IRType::I32, t));
    ir.finishRet(2, 4, cider::ir::IRType::I32, t);

    cider::opt::SimplifyResult result = cider::opt::simplifyControlFlow(ir.module);
    check(structurallyValid(ir.module), "the result is structurally valid");
    check(ir.fn->blocks[0].term.term == cider::ir::Term::CondBr,
          "the branch stays conditional");
    check(result.branchesFolded == 0 && result.blocksRemoved == 0,
          "no counter moves");
}

void testConstantThroughPhi() {
    std::printf("agreeing phi incoming values fold a branch\n");
    Ir ir;
    cider::TypeRef b = ir.boolean();
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::Bool, b);
    ir.finishCondBr(0, ir.value(0, cider::ir::IRType::Bool, b), 1, 2);
    ir.addBlock(1);
    ir.emit(ir.constant(10, 5, cider::ir::IRType::I32, t));
    ir.finishBr(1, 3);
    ir.addBlock(2);
    ir.emit(ir.constant(11, 5, cider::ir::IRType::I32, t));
    ir.finishBr(2, 3);
    ir.addBlock(3);
    ir.emit(ir.phi(12, cider::ir::IRType::I32, t, {{10, 1}, {11, 2}}));
    ir.finishCondBr(3, ir.value(12, cider::ir::IRType::I32, t), 4, 5);
    ir.addBlock(4);
    ir.emit(ir.constant(13, 1, cider::ir::IRType::I32, t));
    ir.finishRet(4, 13, cider::ir::IRType::I32, t);
    ir.addBlock(5);
    ir.emit(ir.constant(14, 2, cider::ir::IRType::I32, t));
    ir.finishRet(5, 14, cider::ir::IRType::I32, t);

    cider::opt::SimplifyResult result = cider::opt::simplifyControlFlow(ir.module);
    check(structurallyValid(ir.module), "the result is structurally valid");
    check(blockLabels(*ir.fn) == "0,1,2,3,4,",
          "only the not-taken successor of the constant phi branch is gone");
    check(ir.fn->blocks[3].term.term == cider::ir::Term::Br &&
              ir.fn->blocks[3].term.target == 4,
          "the phi branch becomes Br on the then edge");
    check(result.branchesFolded == 1 && result.blocksRemoved == 1,
          "the fold and removal are counted");
    check(result.phisSimplified == 0,
          "the phi still has two live predecessors");
}

void testDivergentPhiRefused() {
    std::printf("disagreeing phi incoming values do not fold\n");
    Ir ir;
    cider::TypeRef b = ir.boolean();
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::Bool, b);
    ir.finishCondBr(0, ir.value(0, cider::ir::IRType::Bool, b), 1, 2);
    ir.addBlock(1);
    ir.emit(ir.constant(10, 5, cider::ir::IRType::I32, t));
    ir.finishBr(1, 3);
    ir.addBlock(2);
    ir.emit(ir.constant(11, 6, cider::ir::IRType::I32, t));
    ir.finishBr(2, 3);
    ir.addBlock(3);
    ir.emit(ir.phi(12, cider::ir::IRType::I32, t, {{10, 1}, {11, 2}}));
    ir.finishCondBr(3, ir.value(12, cider::ir::IRType::I32, t), 4, 5);
    ir.addBlock(4);
    ir.emit(ir.constant(13, 1, cider::ir::IRType::I32, t));
    ir.finishRet(4, 13, cider::ir::IRType::I32, t);
    ir.addBlock(5);
    ir.emit(ir.constant(14, 2, cider::ir::IRType::I32, t));
    ir.finishRet(5, 14, cider::ir::IRType::I32, t);

    cider::opt::SimplifyResult result = cider::opt::simplifyControlFlow(ir.module);
    check(structurallyValid(ir.module), "the result is structurally valid");
    check(ir.fn->blocks[3].term.term == cider::ir::Term::CondBr,
          "the phi branch stays conditional");
    check(ir.fn->blocks.size() == 6, "no block is removed");
    check(result.branchesFolded == 0 && result.blocksRemoved == 0,
          "no counter moves");
}

// Fill a module with nested constant branches and a two-word phi. Used twice
// so determinism is measured against an independently constructed module.
void buildShapeModule(Ir& ir) {
    cider::TypeRef b = ir.boolean();
    cider::TypeRef t = ir.i64();
    ir.emit(ir.constant(1, 1, cider::ir::IRType::Bool, b));
    ir.finishCondBr(0, ir.value(1, cider::ir::IRType::Bool, b), 1, 2);
    ir.addBlock(1);
    ir.emit(ir.constant(2, 10, cider::ir::IRType::I64, t));
    ir.emit(ir.constant(3, 11, cider::ir::IRType::I64, t));
    ir.finishBr(1, 3);
    ir.addBlock(2);
    ir.emit(ir.constant(4, 20, cider::ir::IRType::I64, t));
    ir.emit(ir.constant(5, 21, cider::ir::IRType::I64, t));
    ir.finishBr(2, 3);
    ir.addBlock(3);
    ir.emit(ir.phi(6, cider::ir::IRType::I64, t, {{2, 1}, {4, 2}}));
    ir.emit(ir.phi(7, cider::ir::IRType::I64, t, {{5, 2}, {3, 1}}));
    ir.emit(ir.binary(cider::ir::Op::Add, 8,
                      ir.value(6, cider::ir::IRType::I64, t),
                      ir.value(7, cider::ir::IRType::I64, t),
                      cider::ir::IRType::I64, t));
    ir.finishRet(3, 8, cider::ir::IRType::I64, t);
}

void testDeterminism() {
    std::printf("two identical modules simplify identically\n");
    Ir first;
    buildShapeModule(first);
    Ir second;
    buildShapeModule(second);
    cider::opt::SimplifyResult firstResult =
        cider::opt::simplifyControlFlow(first.module);
    cider::opt::SimplifyResult secondResult =
        cider::opt::simplifyControlFlow(second.module);
    check(structurallyValid(first.module), "the first result is valid");
    check(structurallyValid(second.module), "the second result is valid");
    check(describe(*first.fn) == describe(*second.fn),
          "the two functions have the same shape");
    check(cider::ir::dumpModule(first.module) ==
              cider::ir::dumpModule(second.module),
          "two identical modules dump byte-identically");
    check(firstResult.branchesFolded == secondResult.branchesFolded &&
              firstResult.blocksRemoved == secondResult.blocksRemoved &&
              firstResult.phisSimplified == secondResult.phisSimplified &&
              firstResult.phiIncomingRemoved ==
                  secondResult.phiIncomingRemoved,
          "the counters do not depend on insertion order");

    cider::opt::SimplifyResult again =
        cider::opt::simplifyControlFlow(first.module);
    check(again.branchesFolded == 0 && again.blocksRemoved == 0 &&
              again.phisSimplified == 0 && again.phiIncomingRemoved == 0,
          "a second run is a no-op");
}

void testPipelineResult() {
    std::printf("the pipeline reports the pass counters\n");
    Ir ir;
    cider::TypeRef b = ir.boolean();
    cider::TypeRef t = ir.i32();
    ir.emit(ir.constant(1, 1, cider::ir::IRType::Bool, b));
    ir.finishCondBr(0, ir.value(1, cider::ir::IRType::Bool, b), 1, 2);
    ir.addBlock(1);
    ir.emit(ir.constant(2, 7, cider::ir::IRType::I32, t));
    ir.finishRet(1, 2, cider::ir::IRType::I32, t);
    ir.addBlock(2);
    ir.emit(ir.constant(3, 9, cider::ir::IRType::I32, t));
    ir.finishRet(2, 3, cider::ir::IRType::I32, t);

    cider::opt::OptResult result = cider::opt::run(ir.module);
    check(result.ok, "the pipeline succeeds: " + result.error);
    check(structurallyValid(ir.module), "the pipeline result is valid");
    check(result.branchesFolded == 1,
          "the branch-fold counter reaches OptResult");
    check(result.blocksRemoved == 1,
          "the block-removal counter reaches OptResult");
}

}  // namespace

int main() {
    std::printf("simplify_test — constant control flow and unreachable blocks\n\n");
    testConstantTrueCond();
    testConstantFalseCond();
    testSwitchMatch();
    testSwitchDefault();
    testNonconstantSwitchUnchanged();
    testUnknownEarlierCaseRefuses();
    testNestedConstantBranches();
    testUnreachableBlockRemoved();
    testReachableSideEffectsSurvive();
    testPhiIncomingAndSingleIncoming();
    testMultiwordPhisAlignByLabel();
    testNonconstantCondUnchanged();
    testLoadConditionRefused();
    testConstantThroughCopy();
    testWidthChangingCopyRefused();
    testConstantThroughPhi();
    testDivergentPhiRefused();
    testDeterminism();
    testPipelineResult();
    std::printf("\n%d checks, %s\n", checks,
                failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
