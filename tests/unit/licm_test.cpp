// licm_test.cpp — loop-invariant code motion over the real phi IR.
//
// Each case builds a small valid function, runs the pass directly, and asks the
// structural checker to verify the rewrite. The cases pin the natural-loop and
// preheader rules, the candidate allowlist, dependency ordering through nested
// loops, the deliberate speculation of a pure phi input, counters, and
// determinism.

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

int countOpInBlock(const cider::ir::Function& fn, uint32_t label,
                   cider::ir::Op op) {
    for (const cider::ir::Block& block : fn.blocks) {
        if (block.label != label) {
            continue;
        }
        int n = 0;
        for (const cider::ir::Instr& instr : block.body) {
            if (instr.op == op) {
                ++n;
            }
        }
        return n;
    }
    return -1;
}

const cider::ir::Block* blockAt(const cider::ir::Function& fn, uint32_t label) {
    for (const cider::ir::Block& block : fn.blocks) {
        if (block.label == label) {
            return &block;
        }
    }
    return nullptr;
}

constexpr uint32_t kNoBlock = 0xFFFFFFFFu;

uint32_t blockOfValue(const cider::ir::Function& fn, uint32_t id) {
    for (const cider::ir::Block& block : fn.blocks) {
        for (const cider::ir::Instr& instr : block.body) {
            for (const cider::ir::Value& result : instr.results) {
                if (result.id == id) {
                    return block.label;
                }
            }
        }
    }
    return kNoBlock;
}

const cider::ir::Instr* instrIn(const cider::ir::Function& fn, uint32_t label,
                                cider::ir::Op op) {
    const cider::ir::Block* block = blockAt(fn, label);
    if (block == nullptr) {
        return nullptr;
    }
    for (const cider::ir::Instr& instr : block->body) {
        if (instr.op == op) {
            return &instr;
        }
    }
    return nullptr;
}

bool structurallyValid(const cider::ir::Module& module) {
    const std::vector<cider::opt::StructuralError> errors =
        cider::opt::checkModule(module);
    for (const cider::opt::StructuralError& error : errors) {
        std::printf("       %s:%u: %s\n", error.function.c_str(),
                    error.block, error.message.c_str());
    }
    return errors.empty();
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

    cider::TypeRef f64() {
        return cider::TypeRef{types.basic(cider::BasicKind::F64)};
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

    cider::ir::Value i32v(uint32_t id) {
        return value(id, cider::ir::IRType::I32, i32());
    }

    void param(uint32_t id, cider::ir::IRType width, cider::TypeRef type) {
        fn->params.push_back(value(id, width, type));
    }

    void addBlock(uint32_t label) {
        cider::ir::Block block;
        block.label = label;
        fn->blocks.push_back(std::move(block));
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

    cider::ir::Instr binary(cider::ir::Op op, uint32_t result,
                            cider::ir::Value lhs, cider::ir::Value rhs,
                            cider::ir::IRType width, cider::TypeRef type) {
        cider::ir::Instr instr;
        instr.op = op;
        instr.width = width;
        instr.type = type;
        instr.isPure = true;
        instr.results.push_back(value(result, width, type));
        instr.operands.push_back(lhs);
        instr.operands.push_back(rhs);
        return instr;
    }

    cider::ir::Instr copy(uint32_t result, cider::ir::Value source,
                          cider::ir::IRType width, cider::TypeRef type) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Copy;
        instr.width = width;
        instr.type = type;
        instr.isPure = true;
        instr.results.push_back(value(result, width, type));
        instr.operands.push_back(source);
        return instr;
    }

    cider::ir::Instr add(uint32_t result, cider::ir::Value lhs,
                         cider::ir::Value rhs, cider::TypeRef type) {
        return binary(cider::ir::Op::Add, result, lhs, rhs,
                      cider::ir::IRType::I32, type);
    }

    cider::ir::Instr mul(uint32_t result, cider::ir::Value lhs,
                         cider::ir::Value rhs, cider::TypeRef type) {
        return binary(cider::ir::Op::Mul, result, lhs, rhs,
                      cider::ir::IRType::I32, type);
    }

    cider::ir::Instr cmp(uint32_t result, cider::ir::CmpOp op,
                         cider::ir::Value lhs, cider::ir::Value rhs) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Cmp;
        instr.width = cider::ir::IRType::Bool;
        instr.type = boolean();
        instr.cmp = op;
        instr.isPure = true;
        instr.results.push_back(value(result, instr.width, instr.type));
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

    cider::ir::Instr gep(uint32_t result, uint32_t base,
                         cider::ir::Value index, int64_t displacement = 0) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Gep;
        instr.width = cider::ir::IRType::Ptr;
        instr.type = ptr();
        instr.addressBase = value(base, cider::ir::IRType::Ptr, ptr());
        instr.displacement = displacement;
        instr.isPure = true;
        instr.results.push_back(value(result, instr.width, instr.type));
        instr.operands.push_back(index);
        return instr;
    }

    cider::ir::Instr load(uint32_t result, uint32_t base,
                          cider::ir::IRType width, cider::TypeRef type,
                          bool isVolatile = false) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Load;
        instr.width = width;
        instr.type = type;
        instr.addressBase = value(base, cider::ir::IRType::Ptr, ptr());
        instr.isPure = true;
        instr.isVolatile = isVolatile;
        instr.results.push_back(value(result, width, type));
        return instr;
    }

    cider::ir::Instr store(uint32_t base, cider::ir::Value stored,
                           cider::ir::IRType width, cider::TypeRef type) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Store;
        instr.width = width;
        instr.type = type;
        instr.addressBase = value(base, cider::ir::IRType::Ptr, ptr());
        instr.storedValue = stored;
        return instr;
    }

    cider::ir::Instr call(const std::string& callee) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Call;
        instr.width = cider::ir::IRType::Void;
        instr.callee = callee;
        instr.mayTrap = true;
        return instr;
    }

    cider::ir::Instr div(uint32_t result, cider::ir::Value lhs,
                         cider::ir::Value rhs, cider::TypeRef type) {
        cider::ir::Instr instr = binary(cider::ir::Op::Div, result, lhs, rhs,
                                        cider::ir::IRType::I32, type);
        instr.mayTrap = true;
        return instr;
    }

    cider::ir::Instr rem(uint32_t result, cider::ir::Value lhs,
                         cider::ir::Value rhs, cider::TypeRef type) {
        cider::ir::Instr instr = binary(cider::ir::Op::Rem, result, lhs, rhs,
                                        cider::ir::IRType::I32, type);
        instr.mayTrap = true;
        return instr;
    }

    cider::ir::Instr asmBlock() {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Asm;
        instr.width = cider::ir::IRType::Void;
        instr.asmText = "nop";
        return instr;
    }

    cider::ir::Instr extract(uint32_t result, uint32_t base,
                             cider::TypeRef type) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Extract;
        instr.width = cider::ir::IRType::I32;
        instr.type = type;
        instr.addressBase = value(base, cider::ir::IRType::Ptr, ptr());
        instr.field = 0;
        instr.isPure = true;
        instr.results.push_back(value(result, instr.width, type));
        return instr;
    }

    cider::ir::Instr insert(uint32_t result, uint32_t base,
                            cider::ir::Value stored, cider::TypeRef type) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Insert;
        instr.width = cider::ir::IRType::I32;
        instr.type = type;
        instr.addressBase = value(base, cider::ir::IRType::Ptr, ptr());
        instr.storedValue = stored;
        instr.field = 0;
        instr.isPure = true;
        instr.results.push_back(value(result, instr.width, type));
        return instr;
    }

    cider::ir::Instr phi(uint32_t result, cider::ir::IRType width,
                         cider::TypeRef type,
                         const std::vector<std::pair<uint32_t, uint32_t>>& in) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Phi;
        instr.width = width;
        instr.type = type;
        instr.isPure = true;
        instr.results.push_back(value(result, width, type));
        for (const auto& pair : in) {
            cider::ir::PhiIncoming incoming;
            incoming.value = value(pair.first, width, type);
            incoming.block = pair.second;
            instr.phiIncoming.push_back(incoming);
        }
        return instr;
    }

    void finishRet(size_t block, uint32_t id, cider::ir::IRType width,
                   cider::TypeRef type) {
        cider::ir::Terminator term;
        term.term = cider::ir::Term::Ret;
        term.values.push_back(value(id, width, type));
        fn->blocks[block].term = term;
    }

    void finishBr(size_t block, uint32_t target) {
        cider::ir::Terminator term;
        term.term = cider::ir::Term::Br;
        term.target = target;
        fn->blocks[block].term = term;
    }

    void finishCondBr(size_t block, uint32_t condId, uint32_t thenTarget,
                      uint32_t elseTarget) {
        cider::ir::Terminator term;
        term.term = cider::ir::Term::CondBr;
        term.cond = value(condId, cider::ir::IRType::Bool, boolean());
        term.thenTarget = thenTarget;
        term.elseTarget = elseTarget;
        fn->blocks[block].term = term;
    }

    bool lastResultIn(uint32_t label, uint32_t id) {
        const cider::ir::Block* block = blockAt(*fn, label);
        if (block == nullptr || block->body.empty()) {
            return false;
        }
        const cider::ir::Instr& instr = block->body.back();
        return instr.results.size() == 1 && instr.results[0].id == id;
    }
};

void testSimpleHoist() {
    std::printf("an invariant arithmetic result moves to the entry preheader\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::I32, t);
    ir.param(1, cider::ir::IRType::I32, t);
    ir.param(2, cider::ir::IRType::I32, t);
    ir.emit(ir.constant(3, 0, cider::ir::IRType::I32, t));
    ir.emit(ir.constant(4, 1, cider::ir::IRType::I32, t));
    ir.finishBr(0, 1);

    ir.addBlock(1);
    ir.emit(ir.phi(5, cider::ir::IRType::I32, t, {{3, 0}, {6, 3}}));
    ir.emit(ir.cmp(7, cider::ir::CmpOp::Lt, ir.i32v(5), ir.i32v(0)));
    ir.finishCondBr(1, 7, 2, 4);

    ir.addBlock(2);
    ir.emit(ir.add(8, ir.i32v(1), ir.i32v(2), t));
    ir.emit(ir.add(9, ir.i32v(8), ir.i32v(5), t));
    ir.finishBr(2, 3);

    ir.addBlock(3);
    ir.emit(ir.add(6, ir.i32v(5), ir.i32v(4), t));
    ir.finishBr(3, 1);

    ir.addBlock(4);
    ir.finishRet(4, 5, cider::ir::IRType::I32, t);

    cider::opt::LicmResult result =
        cider::opt::loopInvariantCodeMotion(ir.module);
    check(result.loops == 1, "one natural loop is found");
    check(result.hoisted == 1, "one invariant is hoisted");
    check(result.skipped == 0, "no loop is skipped");
    check(ir.lastResultIn(0, 8), "the add now ends the entry preheader body");
    check(countOpInBlock(*ir.fn, 0, cider::ir::Op::Add) == 1,
          "the entry has exactly the hoisted add");
    check(countOpInBlock(*ir.fn, 2, cider::ir::Op::Add) == 1,
          "the body keeps the add that reads the loop phi");
    check(countOp(*ir.fn, cider::ir::Op::Add) == 3,
          "hoisting does not duplicate or delete an instruction");
    check(structurallyValid(ir.module), "the rewrite is structurally valid");
}

void testConstAndCopyStay() {
    std::printf("constants and copies stay where they are\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::Bool, ir.boolean());
    ir.finishBr(0, 1);

    ir.addBlock(1);
    ir.emit(ir.constant(2, 7, cider::ir::IRType::I32, t));
    ir.emit(ir.copy(3, ir.i32v(2), cider::ir::IRType::I32, t));
    ir.finishCondBr(1, 0, 2, 1);

    ir.addBlock(2);
    ir.finishRet(2, 3, cider::ir::IRType::I32, t);

    cider::opt::LicmResult result =
        cider::opt::loopInvariantCodeMotion(ir.module);
    check(result.loops == 1, "one natural loop is found");
    check(result.hoisted == 0, "a constant and its copy do not move");
    check(countOpInBlock(*ir.fn, 1, cider::ir::Op::Const) == 1,
          "the constant stays in the loop");
    check(countOpInBlock(*ir.fn, 1, cider::ir::Op::Copy) == 1,
          "the copy stays in the loop");
    check(countOpInBlock(*ir.fn, 0, cider::ir::Op::Const) == 0,
          "the preheader gains no constant");
    check(structurallyValid(ir.module), "the loop is structurally valid");
}

void testOperandDefinedInLoop() {
    std::printf("an operand defined in the loop prevents hoisting\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::I32, t);
    ir.param(1, cider::ir::IRType::I32, t);
    ir.emit(ir.constant(2, 0, cider::ir::IRType::I32, t));
    ir.emit(ir.allocaSlot(3, t));
    ir.emit(ir.constant(4, 1, cider::ir::IRType::I32, t));
    ir.finishBr(0, 1);

    ir.addBlock(1);
    ir.emit(ir.phi(5, cider::ir::IRType::I32, t, {{2, 0}, {9, 3}}));
    ir.emit(ir.cmp(6, cider::ir::CmpOp::Lt, ir.i32v(5), ir.i32v(0)));
    ir.finishCondBr(1, 6, 2, 4);

    ir.addBlock(2);
    ir.emit(ir.load(7, 3, cider::ir::IRType::I32, t));
    ir.emit(ir.add(8, ir.i32v(7), ir.i32v(1), t));
    ir.finishBr(2, 3);

    ir.addBlock(3);
    ir.emit(ir.add(9, ir.i32v(5), ir.i32v(4), t));
    ir.finishBr(3, 1);

    ir.addBlock(4);
    ir.finishRet(4, 5, cider::ir::IRType::I32, t);

    cider::opt::LicmResult result =
        cider::opt::loopInvariantCodeMotion(ir.module);
    check(result.loops == 1, "one natural loop is found");
    check(result.hoisted == 0, "the loop-defined load keeps its user in place");
    check(instrIn(*ir.fn, 2, cider::ir::Op::Add) != nullptr,
          "the add still sits in the body");
    check(structurallyValid(ir.module), "the module is structurally valid");
}

void testLoopCarriedPhi() {
    std::printf("a loop-carried phi prevents hoisting\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::I32, t);
    ir.param(1, cider::ir::IRType::I32, t);
    ir.emit(ir.constant(2, 0, cider::ir::IRType::I32, t));
    ir.emit(ir.constant(7, 1, cider::ir::IRType::I32, t));
    ir.finishBr(0, 1);

    ir.addBlock(1);
    ir.emit(ir.phi(3, cider::ir::IRType::I32, t, {{2, 0}, {4, 3}}));
    ir.emit(ir.cmp(5, cider::ir::CmpOp::Lt, ir.i32v(3), ir.i32v(0)));
    ir.finishCondBr(1, 5, 2, 4);

    ir.addBlock(2);
    ir.emit(ir.add(6, ir.i32v(3), ir.i32v(1), t));
    ir.finishBr(2, 3);

    ir.addBlock(3);
    ir.emit(ir.add(4, ir.i32v(3), ir.i32v(7), t));
    ir.finishBr(3, 1);

    ir.addBlock(4);
    ir.finishRet(4, 3, cider::ir::IRType::I32, t);

    cider::opt::LicmResult result =
        cider::opt::loopInvariantCodeMotion(ir.module);
    check(result.hoisted == 0, "no arithmetic using the phi moves");
    check(countOpInBlock(*ir.fn, 2, cider::ir::Op::Add) == 1,
          "the body add remains");
    check(countOpInBlock(*ir.fn, 3, cider::ir::Op::Add) == 1,
          "the latch add remains");
    check(structurallyValid(ir.module), "the module is structurally valid");
}

void testNestedThroughBothLevels() {
    std::printf("a nested invariant leaves the inner and outer loop\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::I32, t);
    ir.param(1, cider::ir::IRType::I32, t);
    ir.param(2, cider::ir::IRType::I32, t);
    ir.emit(ir.constant(3, 0, cider::ir::IRType::I32, t));
    ir.emit(ir.constant(4, 1, cider::ir::IRType::I32, t));
    ir.finishBr(0, 1);

    ir.addBlock(1);
    ir.emit(ir.phi(5, cider::ir::IRType::I32, t, {{3, 0}, {13, 5}}));
    ir.emit(ir.cmp(7, cider::ir::CmpOp::Lt, ir.i32v(5), ir.i32v(2)));
    ir.finishCondBr(1, 7, 2, 6);

    ir.addBlock(2);
    ir.finishBr(2, 3);

    ir.addBlock(3);
    ir.emit(ir.phi(8, cider::ir::IRType::I32, t, {{3, 2}, {12, 4}}));
    ir.emit(ir.cmp(9, cider::ir::CmpOp::Lt, ir.i32v(8), ir.i32v(2)));
    ir.finishCondBr(3, 9, 4, 5);

    ir.addBlock(4);
    ir.emit(ir.mul(10, ir.i32v(0), ir.i32v(1), t));
    ir.emit(ir.add(11, ir.i32v(10), ir.i32v(8), t));
    ir.emit(ir.add(12, ir.i32v(8), ir.i32v(4), t));
    ir.finishBr(4, 3);

    ir.addBlock(5);
    ir.emit(ir.add(13, ir.i32v(5), ir.i32v(4), t));
    ir.finishBr(5, 1);

    ir.addBlock(6);
    ir.finishRet(6, 5, cider::ir::IRType::I32, t);

    cider::opt::LicmResult result =
        cider::opt::loopInvariantCodeMotion(ir.module);
    check(result.loops == 2, "two nested natural loops are found");
    check(result.hoisted == 2, "the invariant moves once per loop level");
    check(ir.lastResultIn(0, 10), "the mul reaches the outer preheader");
    check(countOpInBlock(*ir.fn, 2, cider::ir::Op::Mul) == 0,
          "the inner preheader no longer holds it");
    check(countOpInBlock(*ir.fn, 4, cider::ir::Op::Mul) == 0,
          "the inner body no longer holds it");
    check(countOp(*ir.fn, cider::ir::Op::Mul) == 1,
          "the instruction is moved, never duplicated");
    check(structurallyValid(ir.module), "the rewrite is structurally valid");
}

void testNoPreheaderSkips() {
    std::printf("a loop with two outside predecessors is skipped\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::I32, t);
    ir.param(1, cider::ir::IRType::I32, t);
    ir.param(2, cider::ir::IRType::Bool, ir.boolean());
    ir.finishCondBr(0, 2, 1, 2);

    ir.addBlock(1);
    ir.finishBr(1, 3);
    ir.addBlock(2);
    ir.finishBr(2, 3);

    ir.addBlock(3);
    ir.finishCondBr(3, 2, 4, 5);

    ir.addBlock(4);
    ir.emit(ir.add(6, ir.i32v(0), ir.i32v(1), t));
    ir.finishBr(4, 3);

    ir.addBlock(5);
    ir.finishRet(5, 0, cider::ir::IRType::I32, t);

    cider::opt::LicmResult result =
        cider::opt::loopInvariantCodeMotion(ir.module);
    check(result.loops == 1, "the loop is still detected");
    check(result.skipped == 1, "the missing preheader is counted");
    check(result.hoisted == 0, "nothing is hoisted");
    check(countOpInBlock(*ir.fn, 4, cider::ir::Op::Add) == 1,
          "the invariant remains in the body");
    check(structurallyValid(ir.module), "the module is structurally valid");
}

void testMultipleLatches() {
    std::printf("two back edges to one header form one loop\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::I32, t);
    ir.param(1, cider::ir::IRType::I32, t);
    ir.param(2, cider::ir::IRType::I32, t);
    ir.param(6, cider::ir::IRType::Bool, ir.boolean());
    ir.emit(ir.constant(3, 0, cider::ir::IRType::I32, t));
    ir.emit(ir.constant(4, 1, cider::ir::IRType::I32, t));
    ir.finishBr(0, 1);

    ir.addBlock(1);
    ir.emit(ir.phi(5, cider::ir::IRType::I32, t,
                   {{3, 0}, {10, 3}, {11, 4}}));
    ir.emit(ir.cmp(8, cider::ir::CmpOp::Lt, ir.i32v(5), ir.i32v(0)));
    ir.finishCondBr(1, 8, 2, 5);

    ir.addBlock(2);
    ir.emit(ir.add(9, ir.i32v(1), ir.i32v(2), t));
    ir.finishCondBr(2, 6, 3, 4);

    ir.addBlock(3);
    ir.emit(ir.add(10, ir.i32v(5), ir.i32v(4), t));
    ir.finishBr(3, 1);

    ir.addBlock(4);
    ir.emit(ir.add(11, ir.i32v(5), ir.i32v(4), t));
    ir.finishBr(4, 1);

    ir.addBlock(5);
    ir.finishRet(5, 5, cider::ir::IRType::I32, t);

    cider::opt::LicmResult result =
        cider::opt::loopInvariantCodeMotion(ir.module);
    check(result.loops == 1, "both back edges count as one natural loop");
    check(result.hoisted == 1, "the shared invariant moves once");
    check(result.skipped == 0, "the loop has one preheader");
    check(ir.lastResultIn(0, 9), "the add reaches the shared preheader");
    check(countOpInBlock(*ir.fn, 2, cider::ir::Op::Add) == 0,
          "the split body no longer holds it");
    check(countOpInBlock(*ir.fn, 3, cider::ir::Op::Add) == 1 &&
              countOpInBlock(*ir.fn, 4, cider::ir::Op::Add) == 1,
          "both latches keep their phi-dependent increments");
    check(structurallyValid(ir.module), "the rewrite is structurally valid");
}

void testMultipleExits() {
    std::printf("a loop with several exits still hoists once\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::I32, t);
    ir.param(1, cider::ir::IRType::I32, t);
    ir.param(2, cider::ir::IRType::I32, t);
    ir.param(3, cider::ir::IRType::Bool, ir.boolean());
    ir.emit(ir.constant(4, 0, cider::ir::IRType::I32, t));
    ir.emit(ir.constant(5, 1, cider::ir::IRType::I32, t));
    ir.finishBr(0, 1);

    ir.addBlock(1);
    ir.emit(ir.phi(6, cider::ir::IRType::I32, t, {{4, 0}, {13, 3}}));
    ir.emit(ir.cmp(7, cider::ir::CmpOp::Lt, ir.i32v(6), ir.i32v(2)));
    ir.finishCondBr(1, 7, 2, 4);

    ir.addBlock(2);
    ir.emit(ir.add(8, ir.i32v(0), ir.i32v(1), t));
    ir.emit(ir.add(9, ir.i32v(8), ir.i32v(6), t));
    ir.finishCondBr(2, 3, 5, 3);

    ir.addBlock(3);
    ir.emit(ir.add(13, ir.i32v(6), ir.i32v(5), t));
    ir.finishBr(3, 1);

    ir.addBlock(4);
    ir.finishRet(4, 0, cider::ir::IRType::I32, t);
    ir.addBlock(5);
    ir.finishRet(5, 0, cider::ir::IRType::I32, t);

    cider::opt::LicmResult result =
        cider::opt::loopInvariantCodeMotion(ir.module);
    check(result.loops == 1, "the loop is detected despite two exits");
    check(result.hoisted == 1, "the invariant moves exactly once");
    check(ir.lastResultIn(0, 8), "the add reaches the preheader");
    check(countOpInBlock(*ir.fn, 2, cider::ir::Op::Add) == 1,
          "the body keeps only the phi-dependent add");
    check(structurallyValid(ir.module), "the rewrite is structurally valid");
}

void testRefusals() {
    std::printf("loads, stores, calls, trapping, float, asm and casts stay\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    cider::TypeRef f = ir.f64();
    ir.param(0, cider::ir::IRType::I32, t);
    ir.param(1, cider::ir::IRType::I32, t);
    ir.param(2, cider::ir::IRType::I32, t);
    ir.param(3, cider::ir::IRType::F64, f);
    ir.emit(ir.constant(4, 0, cider::ir::IRType::I32, t));
    ir.emit(ir.constant(5, 1, cider::ir::IRType::I32, t));
    ir.emit(ir.allocaSlot(6, t));
    ir.finishBr(0, 1);

    ir.addBlock(1);
    ir.emit(ir.phi(9, cider::ir::IRType::I32, t, {{4, 0}, {10, 3}}));
    ir.emit(ir.cmp(11, cider::ir::CmpOp::Lt, ir.i32v(9), ir.i32v(0)));
    ir.finishCondBr(1, 11, 2, 4);

    ir.addBlock(2);
    ir.emit(ir.load(12, 6, cider::ir::IRType::I32, t));
    ir.emit(ir.load(13, 6, cider::ir::IRType::I32, t, true));
    ir.emit(ir.store(6, ir.i32v(5), cider::ir::IRType::Void, t));
    ir.emit(ir.call("g"));
    ir.emit(ir.div(14, ir.i32v(1), ir.i32v(2), t));
    ir.emit(ir.rem(15, ir.i32v(1), ir.i32v(2), t));
    ir.emit(ir.binary(cider::ir::Op::FAdd, 16, ir.value(3, cider::ir::IRType::F64, f),
                      ir.value(3, cider::ir::IRType::F64, f),
                      cider::ir::IRType::F64, f));
    ir.emit(ir.binary(cider::ir::Op::Add, 17, ir.i32v(1), ir.i32v(2),
                      cider::ir::IRType::I32, t));
    ir.fn->blocks.back().body.back().mayTrap = true;
    ir.emit(ir.binary(cider::ir::Op::Add, 18, ir.i32v(1), ir.i32v(2),
                      cider::ir::IRType::I32, t));
    ir.fn->blocks.back().body.back().isVolatile = true;
    ir.emit(ir.asmBlock());
    ir.emit(ir.extract(19, 6, t));
    ir.emit(ir.insert(20, 6, ir.i32v(1), t));
    cider::ir::Instr cast;
    cast.op = cider::ir::Op::Cast;
    cast.width = cider::ir::IRType::Ptr;
    cast.type = ir.ptr();
    cast.isPure = true;
    cast.results.push_back(ir.value(21, cast.width, cast.type));
    cast.operands.push_back(ir.i32v(1));
    ir.emit(cast);
    cider::ir::Instr data;
    data.op = cider::ir::Op::DataAddr;
    data.width = cider::ir::IRType::Ptr;
    data.type = ir.ptr();
    data.globalLabel = "gvar";
    data.isPure = true;
    data.results.push_back(ir.value(22, data.width, data.type));
    ir.emit(data);
    cider::ir::Instr fnAddr;
    fnAddr.op = cider::ir::Op::FnAddr;
    fnAddr.width = cider::ir::IRType::Ptr;
    fnAddr.type = ir.ptr();
    fnAddr.globalLabel = "gfun";
    fnAddr.isPure = true;
    fnAddr.results.push_back(ir.value(23, fnAddr.width, fnAddr.type));
    ir.emit(fnAddr);
    cider::ir::Instr trap;
    trap.op = cider::ir::Op::Trap;
    trap.reason = cider::ir::TrapReason::Explicit;
    trap.mayTrap = true;
    ir.emit(trap);
    ir.emit(ir.allocaSlot(24, t));
    ir.finishBr(2, 3);

    ir.addBlock(3);
    ir.emit(ir.add(10, ir.i32v(9), ir.i32v(5), t));
    ir.finishBr(3, 1);

    ir.addBlock(4);
    ir.finishRet(4, 9, cider::ir::IRType::I32, t);

    cider::opt::LicmResult result =
        cider::opt::loopInvariantCodeMotion(ir.module);
    check(result.loops == 1, "one natural loop is found");
    check(result.hoisted == 0, "no refused instruction moves");
    check(countOp(*ir.fn, cider::ir::Op::Load) == 2, "both loads remain");
    check(countOp(*ir.fn, cider::ir::Op::Store) == 1, "the store remains");
    check(countOp(*ir.fn, cider::ir::Op::Call) == 1, "the call remains");
    check(countOp(*ir.fn, cider::ir::Op::Div) == 1, "the div remains");
    check(countOp(*ir.fn, cider::ir::Op::Rem) == 1, "the rem remains");
    check(countOp(*ir.fn, cider::ir::Op::FAdd) == 1, "the float add remains");
    check(countOp(*ir.fn, cider::ir::Op::Asm) == 1, "the asm block remains");
    check(countOp(*ir.fn, cider::ir::Op::Extract) == 1, "the extract remains");
    check(countOp(*ir.fn, cider::ir::Op::Insert) == 1, "the insert remains");
    check(countOp(*ir.fn, cider::ir::Op::Cast) == 1, "the cast remains");
    check(countOp(*ir.fn, cider::ir::Op::DataAddr) == 1,
          "the data address remains");
    check(countOp(*ir.fn, cider::ir::Op::FnAddr) == 1,
          "the function address remains");
    check(countOp(*ir.fn, cider::ir::Op::Trap) == 1, "the trap remains");
    check(countOp(*ir.fn, cider::ir::Op::Alloca) == 2,
          "both allocas remain in place");
    check(countOpInBlock(*ir.fn, 2, cider::ir::Op::Add) == 2,
          "the may-trap and volatile adds stay");
    check(structurallyValid(ir.module), "the module is structurally valid");
}

void testGep() {
    std::printf("a Gep with invariant base and index is hoisted\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    cider::TypeRef w = ir.i64();
    ir.param(0, cider::ir::IRType::I32, t);
    ir.emit(ir.allocaSlot(1, t));
    ir.emit(ir.allocaSlot(2, t));
    ir.emit(ir.constant(3, 0, cider::ir::IRType::I64, w));
    ir.emit(ir.constant(4, 1, cider::ir::IRType::I32, t));
    ir.emit(ir.constant(5, 0, cider::ir::IRType::I32, t));
    ir.finishBr(0, 1);

    ir.addBlock(1);
    ir.emit(ir.phi(6, cider::ir::IRType::I32, t, {{5, 0}, {7, 3}}));
    ir.emit(ir.cmp(8, cider::ir::CmpOp::Lt, ir.i32v(6), ir.i32v(0)));
    ir.finishCondBr(1, 8, 2, 4);

    ir.addBlock(2);
    ir.emit(ir.gep(9, 1, ir.value(3, cider::ir::IRType::I64, w)));
    ir.emit(ir.add(10, ir.i32v(6), ir.i32v(6), t));
    ir.emit(ir.gep(11, 1, ir.i32v(10)));
    ir.finishBr(2, 3);

    ir.addBlock(3);
    ir.emit(ir.add(7, ir.i32v(6), ir.i32v(4), t));
    ir.finishBr(3, 1);

    ir.addBlock(4);
    ir.finishRet(4, 6, cider::ir::IRType::I32, t);

    cider::opt::LicmResult result =
        cider::opt::loopInvariantCodeMotion(ir.module);
    check(result.hoisted == 1, "only the invariant Gep moves");
    check(ir.lastResultIn(0, 9), "the invariant Gep ends the entry body");
    check(countOpInBlock(*ir.fn, 2, cider::ir::Op::Gep) == 1,
          "the Gep with an in-loop index stays");
    check(countOp(*ir.fn, cider::ir::Op::Gep) == 2,
          "both Geps remain, one in each block");
    const cider::ir::Instr* gep = instrIn(*ir.fn, 0, cider::ir::Op::Gep);
    check(gep != nullptr && gep->addressBase.id == 1,
          "the moved Gep keeps its invariant base");
    check(structurallyValid(ir.module), "the rewrite is structurally valid");
}

void testPhiUseSpeculation() {
    std::printf("a pure phi input may be hoisted and speculated\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::I32, t);
    ir.param(1, cider::ir::IRType::I32, t);
    ir.param(2, cider::ir::IRType::I32, t);
    ir.emit(ir.constant(3, 0, cider::ir::IRType::I32, t));
    ir.emit(ir.constant(4, 1, cider::ir::IRType::I32, t));
    ir.finishBr(0, 1);

    ir.addBlock(1);
    ir.emit(ir.phi(5, cider::ir::IRType::I32, t, {{3, 0}, {6, 3}}));
    ir.emit(ir.phi(7, cider::ir::IRType::I32, t, {{3, 0}, {8, 3}}));
    ir.emit(ir.cmp(9, cider::ir::CmpOp::Lt, ir.i32v(7), ir.i32v(0)));
    ir.finishCondBr(1, 9, 2, 4);

    ir.addBlock(2);
    ir.finishBr(2, 3);

    ir.addBlock(3);
    ir.emit(ir.add(6, ir.i32v(1), ir.i32v(2), t));
    ir.emit(ir.add(8, ir.i32v(7), ir.i32v(4), t));
    ir.finishBr(3, 1);

    ir.addBlock(4);
    ir.finishRet(4, 5, cider::ir::IRType::I32, t);

    cider::opt::LicmResult result =
        cider::opt::loopInvariantCodeMotion(ir.module);
    check(result.hoisted == 1, "the pure phi input is hoisted");
    check(ir.lastResultIn(0, 6), "the add ends the preheader body");
    const cider::ir::Block* header = blockAt(*ir.fn, 1);
    check(header != nullptr && header->body[0].phiIncoming.size() == 2 &&
              header->body[0].phiIncoming[1].block == 3 &&
              header->body[0].phiIncoming[1].value.id == 6,
          "the phi still names the hoisted value on the latch edge");
    check(countOpInBlock(*ir.fn, 3, cider::ir::Op::Add) == 1,
          "the loop-carried add stays in the latch");
    check(structurallyValid(ir.module),
          "the speculated value dominates every use");
}

void testCountersAndDeterminism() {
    std::printf("counters are exact and a second run changes nothing\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::I32, t);
    ir.param(1, cider::ir::IRType::I32, t);
    ir.param(2, cider::ir::IRType::I32, t);
    ir.emit(ir.constant(3, 0, cider::ir::IRType::I32, t));
    ir.emit(ir.constant(4, 1, cider::ir::IRType::I32, t));
    ir.finishBr(0, 1);

    ir.addBlock(1);
    ir.emit(ir.phi(5, cider::ir::IRType::I32, t, {{3, 0}, {12, 3}}));
    ir.emit(ir.cmp(6, cider::ir::CmpOp::Lt, ir.i32v(5), ir.i32v(0)));
    ir.finishCondBr(1, 6, 2, 4);

    ir.addBlock(2);
    ir.emit(ir.add(7, ir.i32v(1), ir.i32v(2), t));
    ir.emit(ir.mul(8, ir.i32v(7), ir.i32v(1), t));
    ir.emit(ir.cmp(9, cider::ir::CmpOp::Lt, ir.i32v(1), ir.i32v(2)));
    ir.emit(ir.add(10, ir.i32v(8), ir.i32v(5), t));
    ir.finishBr(2, 3);

    ir.addBlock(3);
    ir.emit(ir.add(12, ir.i32v(5), ir.i32v(4), t));
    ir.finishBr(3, 1);

    ir.addBlock(4);
    ir.finishRet(4, 5, cider::ir::IRType::I32, t);

    cider::opt::LicmResult first =
        cider::opt::loopInvariantCodeMotion(ir.module);
    check(first.loops == 1, "one loop counted");
    check(first.hoisted == 3, "three invariants counted");
    check(first.skipped == 0, "nothing skipped");
    check(countOpInBlock(*ir.fn, 0, cider::ir::Op::Add) == 1,
          "the first hoisted add is in the preheader");
    check(countOpInBlock(*ir.fn, 0, cider::ir::Op::Mul) == 1,
          "the dependent mul follows it to the preheader");
    check(countOpInBlock(*ir.fn, 0, cider::ir::Op::Cmp) == 1,
          "the invariant compare also moves");
    check(ir.lastResultIn(0, 9),
          "the compare is appended after the arithmetic it does not use");
    const cider::ir::Block* entry = blockAt(*ir.fn, 0);
    check(entry != nullptr && entry->body.size() >= 4 &&
              entry->body[2].results[0].id == 7 &&
              entry->body[3].results[0].id == 8,
          "the add precedes the mul that reads it");

    const std::string before = cider::ir::dumpModule(ir.module);
    cider::opt::LicmResult second =
        cider::opt::loopInvariantCodeMotion(ir.module);
    check(second.loops == 1, "the loop is still detected");
    check(second.hoisted == 0, "a second run hoists nothing");
    check(second.skipped == 0, "a second run skips nothing");
    check(cider::ir::dumpModule(ir.module) == before,
          "a second run leaves the dump byte-identical");
    check(structurallyValid(ir.module), "the rewrite is structurally valid");
}

void testPipelineWiring() {
    std::printf("the pipeline runs LICM and reports its counters\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::I32, t);
    ir.param(1, cider::ir::IRType::I32, t);
    ir.param(2, cider::ir::IRType::I32, t);
    ir.emit(ir.constant(3, 0, cider::ir::IRType::I32, t));
    ir.emit(ir.constant(4, 1, cider::ir::IRType::I32, t));
    ir.finishBr(0, 1);

    ir.addBlock(1);
    ir.emit(ir.phi(5, cider::ir::IRType::I32, t, {{3, 0}, {6, 3}}));
    ir.finishBr(1, 2);

    ir.addBlock(2);
    ir.emit(ir.add(8, ir.i32v(1), ir.i32v(2), t));
    ir.emit(ir.cmp(7, cider::ir::CmpOp::Lt, ir.i32v(5), ir.i32v(0)));
    ir.finishCondBr(2, 7, 4, 3);

    ir.addBlock(3);
    ir.emit(ir.add(6, ir.i32v(5), ir.i32v(4), t));
    ir.finishBr(3, 1);

    ir.addBlock(4);
    ir.finishRet(4, 8, cider::ir::IRType::I32, t);

    cider::opt::OptResult result = cider::opt::run(ir.module);
    check(result.ok, "the pipeline succeeds: " + result.error);
    check(result.licmLoops == 1, "the LICM loop counter reaches OptResult");
    check(result.licmHoisted == 1, "the LICM hoist counter reaches OptResult");
    check(result.licmSkipped == 0, "the LICM skip counter reaches OptResult");
    // Block cleanup may merge the preheader's successor into another block,
    // so follow the value ids rather than a label whose number the CFG
    // cleanup is allowed to remove.
    check(blockOfValue(*ir.fn, 8) == 0,
          "the invariant add now sits in the entry");
    check(blockOfValue(*ir.fn, 6) != 0 &&
              blockOfValue(*ir.fn, 6) != kNoBlock,
          "the loop-carried add stays out of the entry");
    check(structurallyValid(ir.module), "the pipeline output is valid");
}

}  // namespace

int main() {
    std::printf("licm_test — loop-invariant code motion\n\n");
    testSimpleHoist();
    testConstAndCopyStay();
    testOperandDefinedInLoop();
    testLoopCarriedPhi();
    testNestedThroughBothLevels();
    testNoPreheaderSkips();
    testMultipleLatches();
    testMultipleExits();
    testRefusals();
    testGep();
    testPhiUseSpeculation();
    testCountersAndDeterminism();
    testPipelineWiring();
    std::printf("\n%d checks, %s\n", checks,
                failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
