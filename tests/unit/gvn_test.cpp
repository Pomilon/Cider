// gvn_test.cpp — dominance-scoped value numbering over the phi IR.
//
// Each case builds a small valid function, runs the pass directly, and then
// asks the structural checker to verify the rewrite. The cases pin the scope
// rule (a leader must dominate its duplicate), the opcode refusals, the key
// fields, commutative canonicalisation, Copy collapse, phi/call/terminator
// use rewriting, counters, and idempotence.

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

const cider::ir::Instr* firstInstr(const cider::ir::Function& fn,
                                   cider::ir::Op op) {
    for (const cider::ir::Block& block : fn.blocks) {
        for (const cider::ir::Instr& instr : block.body) {
            if (instr.op == op) {
                return &instr;
            }
        }
    }
    return nullptr;
}

const cider::ir::Instr* instrIn(const cider::ir::Function& fn, uint32_t block,
                                cider::ir::Op op) {
    for (const cider::ir::Block& at : fn.blocks) {
        if (at.label != block) {
            continue;
        }
        for (const cider::ir::Instr& instr : at.body) {
            if (instr.op == op) {
                return &instr;
            }
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

    cider::TypeRef i32() { return cider::TypeRef{types.basic(cider::BasicKind::I32)}; }
    cider::TypeRef u32() { return cider::TypeRef{types.basic(cider::BasicKind::U32)}; }
    cider::TypeRef i64() { return cider::TypeRef{types.basic(cider::BasicKind::I64)}; }
    cider::TypeRef f64() { return cider::TypeRef{types.basic(cider::BasicKind::F64)}; }
    cider::TypeRef boolean() { return cider::TypeRef{types.basic(cider::BasicKind::Bool)}; }
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

    cider::ir::Instr add(uint32_t result, cider::ir::Value lhs,
                         cider::ir::Value rhs, cider::TypeRef type) {
        return binary(cider::ir::Op::Add, result, lhs, rhs,
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

    cider::ir::Instr call(const std::string& callee) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Call;
        instr.width = cider::ir::IRType::Void;
        instr.callee = callee;
        instr.mayTrap = true;
        return instr;
    }

    cider::ir::Instr callIndirect(cider::ir::Value callee) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Call;
        instr.width = cider::ir::IRType::Void;
        instr.call.indirect = true;
        instr.calleeValue = callee;
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

    cider::ir::Instr asmBlock() {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Asm;
        instr.width = cider::ir::IRType::Void;
        instr.asmText = "nop";
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

    void finishSwitch(size_t block, uint32_t valueId, uint32_t caseValueId,
                      uint32_t target, uint32_t defaultTarget) {
        cider::ir::Terminator term;
        term.term = cider::ir::Term::Switch;
        term.switchValue = value(valueId, cider::ir::IRType::I32, i32());
        cider::ir::Case arm;
        arm.value = value(caseValueId, cider::ir::IRType::I32, i32());
        arm.target = target;
        term.cases.push_back(arm);
        term.defaultTarget = defaultTarget;
        fn->blocks[block].term = term;
    }
};

void testDuplicateInBlock() {
    std::printf("duplicate arithmetic in one block\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::I32, t);
    ir.param(1, cider::ir::IRType::I32, t);
    ir.emit(ir.add(2, ir.value(0, cider::ir::IRType::I32, t),
                   ir.value(1, cider::ir::IRType::I32, t), t));
    ir.emit(ir.add(3, ir.value(0, cider::ir::IRType::I32, t),
                   ir.value(1, cider::ir::IRType::I32, t), t));
    ir.emit(ir.add(4, ir.value(2, cider::ir::IRType::I32, t),
                   ir.value(3, cider::ir::IRType::I32, t), t));
    ir.finishRet(0, 4, cider::ir::IRType::I32, t);

    cider::opt::GvnResult result = cider::opt::valueNumber(ir.module);
    check(result.expressionsRemoved == 1, "one expression removed");
    check(result.usesRedirected == 1, "one use redirected");
    check(countOp(*ir.fn, cider::ir::Op::Add) == 2, "two adds remain");
    const cider::ir::Instr* consumer =
        instrIn(*ir.fn, 0, cider::ir::Op::Add);
    const cider::ir::Instr* second = nullptr;
    int seen = 0;
    for (const cider::ir::Instr& instr : ir.fn->blocks[0].body) {
        if (instr.op != cider::ir::Op::Add) {
            continue;
        }
        if (++seen == 2) {
            second = &instr;
        }
    }
    check(consumer != nullptr && second != nullptr &&
              second->results.size() == 1 &&
              second->results[0].id == 4 && second->operands.size() == 2 &&
              second->operands[0].id == 2 && second->operands[1].id == 2,
          "the duplicate's use names the leader");
    check(structurallyValid(ir.module), "the rewrite is structurally valid");
}

void testDominatedChildAndPhiUse() {
    std::printf("a dominated child reuses its ancestor and phi uses update\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::I32, t);
    ir.param(1, cider::ir::IRType::I32, t);
    ir.param(2, cider::ir::IRType::Bool, ir.boolean());
    ir.emit(ir.add(3, ir.value(0, cider::ir::IRType::I32, t),
                   ir.value(1, cider::ir::IRType::I32, t), t));
    ir.finishCondBr(0, 2, 1, 2);
    ir.addBlock(1);
    ir.emit(ir.add(4, ir.value(0, cider::ir::IRType::I32, t),
                   ir.value(1, cider::ir::IRType::I32, t), t));
    ir.finishBr(1, 3);
    ir.addBlock(2);
    ir.finishBr(2, 3);
    ir.addBlock(3);
    ir.emit(ir.phi(5, cider::ir::IRType::I32, t, {{4, 1}, {3, 2}}));
    ir.finishRet(3, 5, cider::ir::IRType::I32, t);

    cider::opt::GvnResult result = cider::opt::valueNumber(ir.module);
    check(result.expressionsRemoved == 1, "the child's duplicate is removed");
    check(countOp(*ir.fn, cider::ir::Op::Add) == 1, "one add remains");
    const cider::ir::Instr* phi = firstInstr(*ir.fn, cider::ir::Op::Phi);
    check(phi != nullptr && phi->phiIncoming.size() == 2 &&
              phi->phiIncoming[0].value.id == 3,
          "the phi incoming names the dominating leader");
    check(structurallyValid(ir.module), "the rewrite is structurally valid");
}

void testSiblingNotReused() {
    std::printf("a sibling branch's expression is not reused\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::I32, t);
    ir.param(1, cider::ir::IRType::I32, t);
    ir.param(2, cider::ir::IRType::Bool, ir.boolean());
    ir.finishCondBr(0, 2, 1, 2);
    ir.addBlock(1);
    ir.emit(ir.add(3, ir.value(0, cider::ir::IRType::I32, t),
                   ir.value(1, cider::ir::IRType::I32, t), t));
    ir.finishBr(1, 3);
    ir.addBlock(2);
    ir.emit(ir.add(4, ir.value(0, cider::ir::IRType::I32, t),
                   ir.value(1, cider::ir::IRType::I32, t), t));
    ir.finishBr(2, 3);
    ir.addBlock(3);
    ir.emit(ir.phi(5, cider::ir::IRType::I32, t, {{3, 1}, {4, 2}}));
    ir.finishRet(3, 5, cider::ir::IRType::I32, t);

    cider::opt::GvnResult result = cider::opt::valueNumber(ir.module);
    check(result.expressionsRemoved == 0, "nothing is commoned across siblings");
    check(countOp(*ir.fn, cider::ir::Op::Add) == 2, "both adds remain");
    check(structurallyValid(ir.module), "the module is structurally valid");
}

void testJoinCannotUseSibling() {
    std::printf("a join does not reuse an expression from one branch\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::I32, t);
    ir.param(1, cider::ir::IRType::I32, t);
    ir.param(2, cider::ir::IRType::Bool, ir.boolean());
    ir.finishCondBr(0, 2, 1, 2);
    ir.addBlock(1);
    ir.emit(ir.add(3, ir.value(0, cider::ir::IRType::I32, t),
                   ir.value(1, cider::ir::IRType::I32, t), t));
    ir.finishBr(1, 3);
    ir.addBlock(2);
    ir.emit(ir.add(4, ir.value(0, cider::ir::IRType::I32, t),
                   ir.value(1, cider::ir::IRType::I32, t), t));
    ir.finishBr(2, 3);
    ir.addBlock(3);
    ir.emit(ir.add(5, ir.value(0, cider::ir::IRType::I32, t),
                   ir.value(1, cider::ir::IRType::I32, t), t));
    ir.finishRet(3, 5, cider::ir::IRType::I32, t);

    cider::opt::GvnResult result = cider::opt::valueNumber(ir.module);
    check(result.expressionsRemoved == 0, "the join keeps its own expression");
    check(countOp(*ir.fn, cider::ir::Op::Add) == 3, "all three adds remain");
    check(structurallyValid(ir.module), "the module is structurally valid");
}

void testJoinUsesDominatingEntry() {
    std::printf("a join reuses an expression that dominates every path\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::I32, t);
    ir.param(1, cider::ir::IRType::I32, t);
    ir.param(2, cider::ir::IRType::Bool, ir.boolean());
    ir.emit(ir.add(3, ir.value(0, cider::ir::IRType::I32, t),
                   ir.value(1, cider::ir::IRType::I32, t), t));
    ir.finishCondBr(0, 2, 1, 2);
    ir.addBlock(1);
    ir.finishBr(1, 3);
    ir.addBlock(2);
    ir.finishBr(2, 3);
    ir.addBlock(3);
    ir.emit(ir.add(4, ir.value(0, cider::ir::IRType::I32, t),
                   ir.value(1, cider::ir::IRType::I32, t), t));
    ir.finishRet(3, 4, cider::ir::IRType::I32, t);

    cider::opt::GvnResult result = cider::opt::valueNumber(ir.module);
    check(result.expressionsRemoved == 1, "the join's duplicate is removed");
    check(countOp(*ir.fn, cider::ir::Op::Add) == 1, "the entry leader remains");
    const cider::ir::Terminator& ret = ir.fn->blocks[3].term;
    check(ret.values.size() == 1 && ret.values[0].id == 3,
          "the return names the dominating leader");
    check(structurallyValid(ir.module), "the module is structurally valid");
}

void testKeyFields() {
    std::printf("width, type and predicate are part of the key\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    cider::TypeRef u = ir.u32();
    cider::TypeRef w = ir.i64();
    ir.param(0, cider::ir::IRType::I32, t);
    ir.param(1, cider::ir::IRType::I32, t);
    ir.emit(ir.add(2, ir.value(0, cider::ir::IRType::I32, t),
                   ir.value(1, cider::ir::IRType::I32, t), t));
    ir.emit(ir.binary(cider::ir::Op::Add, 3,
                      ir.value(0, cider::ir::IRType::I32, t),
                      ir.value(1, cider::ir::IRType::I32, t),
                      cider::ir::IRType::U32, u));
    ir.emit(ir.binary(cider::ir::Op::Add, 4,
                      ir.value(0, cider::ir::IRType::I32, t),
                      ir.value(1, cider::ir::IRType::I32, t),
                      cider::ir::IRType::I64, w));
    ir.emit(ir.cmp(5, cider::ir::CmpOp::Eq,
                   ir.value(0, cider::ir::IRType::I32, t),
                   ir.value(1, cider::ir::IRType::I32, t)));
    ir.emit(ir.cmp(6, cider::ir::CmpOp::Ne,
                   ir.value(0, cider::ir::IRType::I32, t),
                   ir.value(1, cider::ir::IRType::I32, t)));
    ir.emit(ir.cmp(7, cider::ir::CmpOp::Lt,
                   ir.value(0, cider::ir::IRType::I32, t),
                   ir.value(1, cider::ir::IRType::I32, t)));
    ir.emit(ir.cmp(8, cider::ir::CmpOp::Ult,
                   ir.value(0, cider::ir::IRType::I32, t),
                   ir.value(1, cider::ir::IRType::I32, t)));
    ir.finishRet(0, 2, cider::ir::IRType::I32, t);

    cider::opt::GvnResult result = cider::opt::valueNumber(ir.module);
    check(result.expressionsRemoved == 0, "no mismatched expression merges");
    check(countOp(*ir.fn, cider::ir::Op::Add) == 3, "all three adds remain");
    check(countOp(*ir.fn, cider::ir::Op::Cmp) == 4, "all four cmps remain");
    check(structurallyValid(ir.module), "the module is structurally valid");
}

void testCommutative() {
    std::printf("commutative operands canonicalise by value id\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::I32, t);
    ir.param(1, cider::ir::IRType::I32, t);
    ir.emit(ir.add(2, ir.value(0, cider::ir::IRType::I32, t),
                   ir.value(1, cider::ir::IRType::I32, t), t));
    ir.emit(ir.add(3, ir.value(1, cider::ir::IRType::I32, t),
                   ir.value(0, cider::ir::IRType::I32, t), t));
    ir.emit(ir.binary(cider::ir::Op::Mul, 4,
                      ir.value(0, cider::ir::IRType::I32, t),
                      ir.value(1, cider::ir::IRType::I32, t),
                      cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::Mul, 5,
                      ir.value(1, cider::ir::IRType::I32, t),
                      ir.value(0, cider::ir::IRType::I32, t),
                      cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::And, 6,
                      ir.value(0, cider::ir::IRType::I32, t),
                      ir.value(1, cider::ir::IRType::I32, t),
                      cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::And, 7,
                      ir.value(1, cider::ir::IRType::I32, t),
                      ir.value(0, cider::ir::IRType::I32, t),
                      cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::Or, 8,
                      ir.value(0, cider::ir::IRType::I32, t),
                      ir.value(1, cider::ir::IRType::I32, t),
                      cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::Or, 9,
                      ir.value(1, cider::ir::IRType::I32, t),
                      ir.value(0, cider::ir::IRType::I32, t),
                      cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::Xor, 10,
                      ir.value(0, cider::ir::IRType::I32, t),
                      ir.value(1, cider::ir::IRType::I32, t),
                      cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::Xor, 11,
                      ir.value(1, cider::ir::IRType::I32, t),
                      ir.value(0, cider::ir::IRType::I32, t),
                      cider::ir::IRType::I32, t));
    ir.emit(ir.cmp(12, cider::ir::CmpOp::Eq,
                   ir.value(0, cider::ir::IRType::I32, t),
                   ir.value(1, cider::ir::IRType::I32, t)));
    ir.emit(ir.cmp(13, cider::ir::CmpOp::Eq,
                   ir.value(1, cider::ir::IRType::I32, t),
                   ir.value(0, cider::ir::IRType::I32, t)));
    ir.emit(ir.cmp(14, cider::ir::CmpOp::Ne,
                   ir.value(0, cider::ir::IRType::I32, t),
                   ir.value(1, cider::ir::IRType::I32, t)));
    ir.emit(ir.cmp(15, cider::ir::CmpOp::Ne,
                   ir.value(1, cider::ir::IRType::I32, t),
                   ir.value(0, cider::ir::IRType::I32, t)));
    ir.emit(ir.binary(cider::ir::Op::Sub, 16,
                      ir.value(0, cider::ir::IRType::I32, t),
                      ir.value(1, cider::ir::IRType::I32, t),
                      cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::Sub, 17,
                      ir.value(1, cider::ir::IRType::I32, t),
                      ir.value(0, cider::ir::IRType::I32, t),
                      cider::ir::IRType::I32, t));
    ir.emit(ir.cmp(18, cider::ir::CmpOp::Lt,
                   ir.value(0, cider::ir::IRType::I32, t),
                   ir.value(1, cider::ir::IRType::I32, t)));
    ir.emit(ir.cmp(19, cider::ir::CmpOp::Lt,
                   ir.value(1, cider::ir::IRType::I32, t),
                   ir.value(0, cider::ir::IRType::I32, t)));
    ir.finishRet(0, 17, cider::ir::IRType::I32, t);

    cider::opt::GvnResult result = cider::opt::valueNumber(ir.module);
    check(result.expressionsRemoved == 7, "seven commutative duplicates merge");
    check(countOp(*ir.fn, cider::ir::Op::Add) == 1, "one add remains");
    check(countOp(*ir.fn, cider::ir::Op::Mul) == 1, "one mul remains");
    check(countOp(*ir.fn, cider::ir::Op::And) == 1, "one and remains");
    check(countOp(*ir.fn, cider::ir::Op::Or) == 1, "one or remains");
    check(countOp(*ir.fn, cider::ir::Op::Xor) == 1, "one xor remains");
    check(countOp(*ir.fn, cider::ir::Op::Sub) == 2,
          "subtraction is not commutative");
    check(countOp(*ir.fn, cider::ir::Op::Cmp) == 4,
          "Eq/Ne merge, Lt does not swap");
    check(structurallyValid(ir.module), "the module is structurally valid");
}

void testGep() {
    std::printf("Gep keys include base, index and displacement\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    cider::TypeRef i64 = ir.i64();
    ir.emit(ir.allocaSlot(0, t));
    ir.emit(ir.allocaSlot(1, t));
    ir.emit(ir.constant(2, 0, cider::ir::IRType::I64, i64));
    ir.emit(ir.constant(3, 8, cider::ir::IRType::I64, i64));
    ir.emit(ir.gep(4, 0, ir.value(2, cider::ir::IRType::I64, i64)));
    ir.emit(ir.gep(5, 0, ir.value(2, cider::ir::IRType::I64, i64)));
    ir.emit(ir.gep(6, 0, ir.value(2, cider::ir::IRType::I64, i64), 8));
    ir.emit(ir.gep(7, 0, ir.value(3, cider::ir::IRType::I64, i64)));
    ir.emit(ir.gep(8, 1, ir.value(2, cider::ir::IRType::I64, i64)));
    ir.finishRet(0, 4, cider::ir::IRType::Ptr, ir.ptr());

    cider::opt::GvnResult result = cider::opt::valueNumber(ir.module);
    check(result.expressionsRemoved == 1, "only the identical Gep merges");
    check(countOp(*ir.fn, cider::ir::Op::Gep) == 4, "four distinct Geps remain");
    check(structurallyValid(ir.module), "the module is structurally valid");
}

void testCopyCollapse() {
    std::printf("Copy chains collapse to their source\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::I32, t);
    ir.emit(ir.copy(1, ir.value(0, cider::ir::IRType::I32, t),
                    cider::ir::IRType::I32, t));
    ir.emit(ir.copy(2, ir.value(1, cider::ir::IRType::I32, t),
                    cider::ir::IRType::I32, t));
    ir.emit(ir.add(3, ir.value(2, cider::ir::IRType::I32, t),
                   ir.value(0, cider::ir::IRType::I32, t), t));
    ir.finishRet(0, 3, cider::ir::IRType::I32, t);

    cider::opt::GvnResult result = cider::opt::valueNumber(ir.module);
    check(result.copiesCollapsed == 2, "both copy moves collapse");
    check(result.expressionsRemoved == 2, "both copies count as removed");
    check(result.usesRedirected == 2, "the chain and the add are redirected");
    check(countOp(*ir.fn, cider::ir::Op::Copy) == 0, "no copy remains");
    const cider::ir::Instr* add = firstInstr(*ir.fn, cider::ir::Op::Add);
    check(add != nullptr && add->operands[0].id == 0 &&
              add->operands[1].id == 0,
          "the add names the parameter directly");
    check(structurallyValid(ir.module), "the module is structurally valid");
}

void testCopyTypeMismatchKept() {
    std::printf("a copy with a mismatched width or type does not collapse\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    cider::TypeRef u = ir.u32();
    ir.param(0, cider::ir::IRType::I32, t);
    ir.emit(ir.copy(1, ir.value(0, cider::ir::IRType::I32, t),
                    cider::ir::IRType::U32, u));
    ir.finishRet(0, 0, cider::ir::IRType::I32, t);

    cider::opt::GvnResult result = cider::opt::valueNumber(ir.module);
    check(result.copiesCollapsed == 0, "the mismatched copy is kept");
    check(countOp(*ir.fn, cider::ir::Op::Copy) == 1, "the copy remains");
    check(structurallyValid(ir.module), "the module is structurally valid");
}

void testNoCommoning() {
    std::printf("loads, stores, calls, volatile, trapping, asm and extract stay\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.emit(ir.allocaSlot(0, t));
    ir.emit(ir.constant(1, 1, cider::ir::IRType::I32, t));
    ir.emit(ir.store(0, ir.value(1, cider::ir::IRType::I32, t),
                     cider::ir::IRType::Void, t));
    ir.emit(ir.load(2, 0, cider::ir::IRType::I32, t));
    ir.emit(ir.load(3, 0, cider::ir::IRType::I32, t));
    ir.emit(ir.load(4, 0, cider::ir::IRType::I32, t, true));
    ir.emit(ir.load(5, 0, cider::ir::IRType::I32, t, true));
    ir.emit(ir.call("g"));
    ir.emit(ir.call("g"));
    ir.emit(ir.div(6, ir.value(1, cider::ir::IRType::I32, t),
                   ir.value(1, cider::ir::IRType::I32, t), t));
    ir.emit(ir.div(7, ir.value(1, cider::ir::IRType::I32, t),
                   ir.value(1, cider::ir::IRType::I32, t), t));
    ir.emit(ir.extract(8, 0, t));
    ir.emit(ir.extract(9, 0, t));
    ir.emit(ir.asmBlock());
    ir.emit(ir.asmBlock());
    cider::ir::Instr trap;
    trap.op = cider::ir::Op::Trap;
    trap.reason = cider::ir::TrapReason::Explicit;
    ir.emit(trap);
    ir.emit(trap);
    ir.finishRet(0, 2, cider::ir::IRType::I32, t);

    cider::opt::GvnResult result = cider::opt::valueNumber(ir.module);
    check(result.expressionsRemoved == 0, "nothing unsafe is commoned");
    check(countOp(*ir.fn, cider::ir::Op::Load) == 4, "all four loads remain");
    check(countOp(*ir.fn, cider::ir::Op::Store) == 1, "the store remains");
    check(countOp(*ir.fn, cider::ir::Op::Call) == 2, "both calls remain");
    check(countOp(*ir.fn, cider::ir::Op::Div) == 2, "both trapping divs remain");
    check(countOp(*ir.fn, cider::ir::Op::Extract) == 2, "both extracts remain");
    check(countOp(*ir.fn, cider::ir::Op::Asm) == 2, "both asm blocks remain");
    check(countOp(*ir.fn, cider::ir::Op::Trap) == 2, "both explicit traps remain");
    check(structurallyValid(ir.module), "the module is structurally valid");
}

void testPhiNotCommoned() {
    std::printf("phi instructions are never commoned\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::I32, t);
    ir.param(1, cider::ir::IRType::Bool, ir.boolean());
    ir.finishCondBr(0, 1, 1, 2);
    ir.addBlock(1);
    ir.finishBr(1, 3);
    ir.addBlock(2);
    ir.finishBr(2, 3);
    ir.addBlock(3);
    ir.emit(ir.phi(2, cider::ir::IRType::I32, t, {{0, 1}, {0, 2}}));
    ir.emit(ir.phi(3, cider::ir::IRType::I32, t, {{0, 1}, {0, 2}}));
    ir.finishRet(3, 3, cider::ir::IRType::I32, t);

    cider::opt::GvnResult result = cider::opt::valueNumber(ir.module);
    check(result.expressionsRemoved == 0, "no phi is removed");
    check(countOp(*ir.fn, cider::ir::Op::Phi) == 2, "both phis remain");
    check(structurallyValid(ir.module), "the module is structurally valid");
}

void testCrossCall() {
    std::printf("a pure expression may cross a call with unchanged operands\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::I32, t);
    ir.param(1, cider::ir::IRType::I32, t);
    ir.emit(ir.add(2, ir.value(0, cider::ir::IRType::I32, t),
                   ir.value(1, cider::ir::IRType::I32, t), t));
    ir.emit(ir.call("g"));
    ir.emit(ir.add(3, ir.value(1, cider::ir::IRType::I32, t),
                   ir.value(0, cider::ir::IRType::I32, t), t));
    ir.finishRet(0, 3, cider::ir::IRType::I32, t);

    cider::opt::GvnResult result = cider::opt::valueNumber(ir.module);
    check(result.expressionsRemoved == 1, "the post-call duplicate is removed");
    check(countOp(*ir.fn, cider::ir::Op::Add) == 1, "one add remains");
    check(countOp(*ir.fn, cider::ir::Op::Call) == 1, "the call remains");
    const cider::ir::Terminator& ret = ir.fn->blocks[0].term;
    check(ret.values.size() == 1 && ret.values[0].id == 2,
          "the return uses the pre-call leader");
    check(structurallyValid(ir.module), "the module is structurally valid");
}

void testOtherUseFields() {
    std::printf("stored values, address bases and indirect callees update\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    cider::TypeRef i64 = ir.i64();
    ir.emit(ir.allocaSlot(0, t));
    ir.emit(ir.constant(1, 1, cider::ir::IRType::I32, t));
    ir.emit(ir.constant(2, 1, cider::ir::IRType::I32, t));
    ir.emit(ir.constant(9, 0, cider::ir::IRType::I64, i64));
    ir.emit(ir.gep(3, 0, ir.value(9, cider::ir::IRType::I64, i64)));
    ir.emit(ir.gep(4, 0, ir.value(9, cider::ir::IRType::I64, i64)));
    ir.emit(ir.store(4, ir.value(2, cider::ir::IRType::I32, t),
                     cider::ir::IRType::Void, t));
    ir.emit(ir.load(5, 4, cider::ir::IRType::I32, t));
    ir.emit(ir.constant(10, 0, cider::ir::IRType::Ptr, ir.ptr()));
    ir.emit(ir.constant(11, 0, cider::ir::IRType::Ptr, ir.ptr()));
    ir.emit(ir.callIndirect(ir.value(11, cider::ir::IRType::Ptr, ir.ptr())));
    ir.finishRet(0, 5, cider::ir::IRType::I32, t);

    cider::opt::GvnResult result = cider::opt::valueNumber(ir.module);
    check(result.expressionsRemoved == 3,
          "the const, pointer const and Gep duplicates merge");
    check(result.usesRedirected == 4,
          "store base/value, load base and callee are redirected");
    const cider::ir::Instr* store =
        firstInstr(*ir.fn, cider::ir::Op::Store);
    check(store != nullptr && store->addressBase.id == 3 &&
              store->storedValue.id == 1,
          "the store names the leader Gep and value");
    const cider::ir::Instr* load = firstInstr(*ir.fn, cider::ir::Op::Load);
    check(load != nullptr && load->addressBase.id == 3,
          "the load names the leader Gep");
    const cider::ir::Instr* call = firstInstr(*ir.fn, cider::ir::Op::Call);
    check(call != nullptr && call->call.indirect &&
              call->calleeValue.id == 10,
          "the indirect callee names the leader");
    check(structurallyValid(ir.module), "the rewrite is structurally valid");
}

void testTerminatorUses() {
    std::printf("condition and switch-case values update\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::I32, t);
    ir.param(1, cider::ir::IRType::I32, t);
    ir.emit(ir.cmp(2, cider::ir::CmpOp::Lt,
                   ir.value(0, cider::ir::IRType::I32, t),
                   ir.value(1, cider::ir::IRType::I32, t)));
    ir.emit(ir.cmp(3, cider::ir::CmpOp::Lt,
                   ir.value(0, cider::ir::IRType::I32, t),
                   ir.value(1, cider::ir::IRType::I32, t)));
    ir.finishCondBr(0, 3, 1, 2);
    ir.addBlock(1);
    ir.emit(ir.add(4, ir.value(0, cider::ir::IRType::I32, t),
                   ir.value(1, cider::ir::IRType::I32, t), t));
    ir.emit(ir.add(5, ir.value(0, cider::ir::IRType::I32, t),
                   ir.value(1, cider::ir::IRType::I32, t), t));
    ir.finishSwitch(1, 4, 5, 3, 3);
    ir.addBlock(2);
    ir.finishBr(2, 3);
    ir.addBlock(3);
    ir.finishRet(3, 0, cider::ir::IRType::I32, t);

    cider::opt::GvnResult result = cider::opt::valueNumber(ir.module);
    check(result.expressionsRemoved == 2,
          "the duplicate compare and add are removed");
    check(result.usesRedirected == 2,
          "the condition and switch case are redirected");
    const cider::ir::Terminator& branch = ir.fn->blocks[0].term;
    check(branch.cond.has_value() && branch.cond->id == 2,
          "the branch condition names the leader");
    const cider::ir::Terminator& sw = ir.fn->blocks[1].term;
    check(sw.term == cider::ir::Term::Switch && sw.cases.size() == 1 &&
              sw.cases[0].value.id == 4,
          "the switch case names the leader");
    check(structurallyValid(ir.module), "the rewrite is structurally valid");
}

void testFloatsExcluded() {
    std::printf("float arithmetic and comparisons are never commoned\n");
    Ir ir;
    cider::TypeRef f = ir.f64();
    ir.param(0, cider::ir::IRType::F64, f);
    ir.param(1, cider::ir::IRType::F64, f);
    ir.emit(ir.binary(cider::ir::Op::Add, 2,
                      ir.value(0, cider::ir::IRType::F64, f),
                      ir.value(1, cider::ir::IRType::F64, f),
                      cider::ir::IRType::F64, f));
    ir.emit(ir.binary(cider::ir::Op::Add, 3,
                      ir.value(0, cider::ir::IRType::F64, f),
                      ir.value(1, cider::ir::IRType::F64, f),
                      cider::ir::IRType::F64, f));
    ir.emit(ir.cmp(4, cider::ir::CmpOp::Eq,
                   ir.value(0, cider::ir::IRType::F64, f),
                   ir.value(1, cider::ir::IRType::F64, f)));
    ir.emit(ir.cmp(5, cider::ir::CmpOp::Eq,
                   ir.value(0, cider::ir::IRType::F64, f),
                   ir.value(1, cider::ir::IRType::F64, f)));
    ir.finishRet(0, 3, cider::ir::IRType::F64, f);

    cider::opt::GvnResult result = cider::opt::valueNumber(ir.module);
    check(result.expressionsRemoved == 0, "no float expression is commoned");
    check(countOp(*ir.fn, cider::ir::Op::Add) == 2, "both float adds remain");
    check(countOp(*ir.fn, cider::ir::Op::Cmp) == 2, "both float cmps remain");
    check(structurallyValid(ir.module), "the module is structurally valid");
}

void testConstantsAndIdempotence() {
    std::printf("constants merge and a second run changes nothing\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::I32, t);
    ir.emit(ir.constant(1, 7, cider::ir::IRType::I32, t));
    ir.emit(ir.constant(2, 7, cider::ir::IRType::I32, t));
    ir.emit(ir.add(3, ir.value(0, cider::ir::IRType::I32, t),
                   ir.value(1, cider::ir::IRType::I32, t), t));
    ir.emit(ir.add(4, ir.value(0, cider::ir::IRType::I32, t),
                   ir.value(2, cider::ir::IRType::I32, t), t));
    ir.emit(ir.add(5, ir.value(3, cider::ir::IRType::I32, t),
                   ir.value(4, cider::ir::IRType::I32, t), t));
    ir.finishRet(0, 5, cider::ir::IRType::I32, t);

    cider::opt::GvnResult first = cider::opt::valueNumber(ir.module);
    check(first.expressionsRemoved == 2,
          "the duplicate const and the add naming it merge");
    check(countOp(*ir.fn, cider::ir::Op::Const) == 1, "one const remains");
    check(countOp(*ir.fn, cider::ir::Op::Add) == 2, "two adds remain");
    check(structurallyValid(ir.module), "the rewrite is structurally valid");

    const std::string before = cider::ir::dumpModule(ir.module);
    cider::opt::GvnResult second = cider::opt::valueNumber(ir.module);
    check(second.expressionsRemoved == 0, "a second run removes nothing");
    check(second.usesRedirected == 0, "a second run redirects nothing");
    check(cider::ir::dumpModule(ir.module) == before,
          "a second run leaves the dump unchanged");
}

void testPipelineWiring() {
    std::printf("the pipeline runs GVN and reports its counters\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::I32, t);
    ir.param(1, cider::ir::IRType::I32, t);
    ir.emit(ir.add(2, ir.value(0, cider::ir::IRType::I32, t),
                   ir.value(1, cider::ir::IRType::I32, t), t));
    ir.emit(ir.add(3, ir.value(0, cider::ir::IRType::I32, t),
                   ir.value(1, cider::ir::IRType::I32, t), t));
    ir.emit(ir.add(4, ir.value(2, cider::ir::IRType::I32, t),
                   ir.value(3, cider::ir::IRType::I32, t), t));
    ir.finishRet(0, 4, cider::ir::IRType::I32, t);

    cider::opt::OptResult result = cider::opt::run(ir.module);
    check(result.ok, "the pipeline succeeds: " + result.error);
    check(result.expressionsRemoved == 1, "the GVN counter reaches OptResult");
    check(result.usesRedirected == 1, "the redirect counter reaches OptResult");
    check(countOp(*ir.fn, cider::ir::Op::Add) == 2, "the duplicate is gone");
    check(structurallyValid(ir.module), "the pipeline output is valid");
}

}  // namespace

int main() {
    std::printf("gvn_test — dominance-scoped value numbering\n\n");
    testDuplicateInBlock();
    testDominatedChildAndPhiUse();
    testSiblingNotReused();
    testJoinCannotUseSibling();
    testJoinUsesDominatingEntry();
    testKeyFields();
    testCommutative();
    testGep();
    testCopyCollapse();
    testCopyTypeMismatchKept();
    testNoCommoning();
    testPhiNotCommoned();
    testCrossCall();
    testOtherUseFields();
    testTerminatorUses();
    testFloatsExcluded();
    testConstantsAndIdempotence();
    testPipelineWiring();
    std::printf("\n%d checks, %s\n", checks,
                failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
