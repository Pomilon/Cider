// sccp_test.cpp — sparse conditional constant propagation over the phi IR.
//
// Each case builds a small valid function, runs the pass directly, and then
// asks the structural checker to verify the rewrite. The cases pin the
// executable-edge phi meet, loop-phi revisit, trapping/volatile survival,
// float/div refusal, and the narrowing / signed-to-unsigned widening Cast
// shapes that the C1/C2 audit miscompiles came from.

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

// The instruction defining a result id, or nullptr when none does.
const cider::ir::Instr* instrResult(const cider::ir::Function& fn,
                                    uint32_t id) {
    for (const cider::ir::Block& block : fn.blocks) {
        for (const cider::ir::Instr& instr : block.body) {
            for (const cider::ir::Value& result : instr.results) {
                if (result.id == id) {
                    return &instr;
                }
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
    cider::TypeRef i8() { return cider::TypeRef{types.basic(cider::BasicKind::I8)}; }
    cider::TypeRef f64() { return cider::TypeRef{types.basic(cider::BasicKind::F64)}; }
    cider::TypeRef boolean() { return cider::TypeRef{types.basic(cider::BasicKind::Bool)}; }

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

    cider::ir::Instr cast(uint32_t result, cider::ir::Value source,
                          cider::ir::IRType width, cider::TypeRef type) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Cast;
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

    cider::ir::Instr store(uint32_t base, cider::ir::Value stored,
                           cider::ir::IRType width, cider::TypeRef type,
                           bool isVolatile = false) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Store;
        instr.width = width;
        instr.type = type;
        instr.addressBase = value(base, cider::ir::IRType::Ptr, ptrType());
        instr.storedValue = stored;
        instr.isVolatile = isVolatile;
        return instr;
    }

    cider::TypeRef ptrType() {
        return cider::TypeRef{types.pointer(types.basic(cider::BasicKind::U8))};
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
};

void testPhiMeetAndBranchFold() {
    std::printf("phi meet over executable edges folds a branch\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    cider::TypeRef b = ir.boolean();
    ir.param(0, cider::ir::IRType::Bool, b);
    ir.emit(ir.constant(1, 7, cider::ir::IRType::I32, t));
    ir.finishCondBr(0, 0, 1, 2);
    ir.addBlock(1);
    ir.emit(ir.constant(2, 7, cider::ir::IRType::I32, t));
    ir.finishBr(1, 3);
    ir.addBlock(2);
    ir.emit(ir.constant(3, 7, cider::ir::IRType::I32, t));
    ir.finishBr(2, 3);
    ir.addBlock(3);
    ir.emit(ir.phi(4, cider::ir::IRType::I32, t, {{2, 1}, {3, 2}}));
    ir.emit(ir.add(5, ir.value(4, cider::ir::IRType::I32, t),
                   ir.value(1, cider::ir::IRType::I32, t), t));
    ir.finishCondBr(3, 4, 4, 5);
    ir.addBlock(4);
    ir.finishRet(4, 5, cider::ir::IRType::I32, t);
    ir.addBlock(5);
    ir.finishRet(5, 5, cider::ir::IRType::I32, t);

    cider::opt::SccpResult first = cider::opt::sccp(ir.module);
    check(first.constantsReplaced >= 1,
          "the phi and the constant add are replaced");
    check(first.branchesFolded == 1, "the branch on the phi folds");
    const cider::ir::Instr* phi = instrResult(*ir.fn, 4);
    check(phi != nullptr && phi->op == cider::ir::Op::Const &&
              phi->constantBits == 7,
          "the phi carries the entry-arm constant");
    const cider::ir::Instr* sum = instrResult(*ir.fn, 5);
    check(sum != nullptr && sum->op == cider::ir::Op::Const &&
              sum->constantBits == 14,
          "arithmetic with a proved phi constant folds");
    check(structurallyValid(ir.module), "the rewrite is structurally valid");

    const std::string before = cider::ir::dumpModule(ir.module);
    cider::opt::SccpResult second = cider::opt::sccp(ir.module);
    check(second.constantsReplaced == 0 && second.branchesFolded == 0 &&
              second.blocksRemoved == 0 && second.phisSimplified == 0,
          "a second run changes no counter");
    check(cider::ir::dumpModule(ir.module) == before,
          "a second run leaves the dump unchanged");
}

void testLoopPhiRevisit() {
    std::printf("a loop phi converges only after the back edge is revisited\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    cider::TypeRef b = ir.boolean();
    ir.param(0, cider::ir::IRType::Bool, b);
    ir.emit(ir.constant(1, 0, cider::ir::IRType::I32, t));
    ir.finishBr(0, 1);
    ir.addBlock(1);
    ir.emit(ir.phi(2, cider::ir::IRType::I32, t, {{1, 0}, {3, 2}}));
    ir.finishCondBr(1, 2, 3, 2);
    ir.addBlock(2);
    ir.emit(ir.copy(3, ir.value(2, cider::ir::IRType::I32, t),
                    cider::ir::IRType::I32, t));
    ir.finishCondBr(2, 0, 1, 3);
    ir.addBlock(3);
    ir.finishRet(3, 2, cider::ir::IRType::I32, t);

    cider::opt::SccpResult result = cider::opt::sccp(ir.module);
    check(result.constantsReplaced >= 1, "the loop phi becomes a constant");
    check(result.branchesFolded == 1, "the loop's branch folds");
    const cider::ir::Instr* phi = instrResult(*ir.fn, 2);
    check(phi != nullptr && phi->op == cider::ir::Op::Const &&
              phi->constantBits == 0,
          "the phi folds to the entry constant, not a first-visit guess");
    const cider::ir::Instr* next = instrResult(*ir.fn, 3);
    check(next != nullptr && next->op == cider::ir::Op::Const &&
              next->constantBits == 0,
          "the back-edge Copy was revisited after the edge turned executable");
    check(structurallyValid(ir.module), "the converged loop is structurally valid");
}

void testTrappingAndVolatileBlocksSurvive() {
    std::printf("a kept unreachable block keeps its trap and volatile store\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.emit(ir.constant(1, 1, cider::ir::IRType::I32, t));
    ir.emit(ir.constant(4, 10, cider::ir::IRType::I32, t));
    ir.emit(ir.constant(5, 0, cider::ir::IRType::I32, t));
    ir.emit(ir.allocaSlot(2, t));
    ir.finishCondBr(0, 1, 1, 2);
    ir.addBlock(1);
    ir.finishRet(1, 1, cider::ir::IRType::I32, t);
    ir.addBlock(2);
    ir.emit(ir.div(3, ir.value(4, cider::ir::IRType::I32, t),
                   ir.value(5, cider::ir::IRType::I32, t), t));
    ir.emit(ir.store(2, ir.value(4, cider::ir::IRType::I32, t),
                     cider::ir::IRType::Void, t, true));
    ir.finishBr(2, 3);
    ir.addBlock(3);
    ir.finishRet(3, 1, cider::ir::IRType::I32, t);

    cider::opt::SccpResult result = cider::opt::sccp(ir.module);
    check(result.branchesFolded == 1, "the entry branch folds");
    check(result.blocksRemoved == 0,
          "the unreachable block is kept because it has forbidden effects");
    const cider::ir::Instr* div = instrResult(*ir.fn, 3);
    check(div != nullptr && div->op == cider::ir::Op::Div && div->mayTrap,
          "the mayTrap Div is still present");
    int volatileStores = 0;
    for (const cider::ir::Block& block : ir.fn->blocks) {
        for (const cider::ir::Instr& instr : block.body) {
            if (instr.op == cider::ir::Op::Store && instr.isVolatile) {
                ++volatileStores;
            }
        }
    }
    check(volatileStores == 1, "the volatile Store is still present");
    check(structurallyValid(ir.module), "the kept block is structurally valid");
}

void testFloatAndDivRemNotFolded() {
    std::printf("float compares and trapping Div/Rem are not folded\n");
    Ir ir;
    cider::TypeRef f = ir.f64();
    cider::TypeRef t = ir.i32();
    cider::TypeRef b = ir.boolean();
    ir.param(0, cider::ir::IRType::F64, f);
    ir.param(1, cider::ir::IRType::F64, f);
    ir.emit(ir.constant(2, 1, cider::ir::IRType::Bool, b));
    ir.emit(ir.cmp(3, cider::ir::CmpOp::Eq,
                   ir.value(0, cider::ir::IRType::F64, f),
                   ir.value(1, cider::ir::IRType::F64, f)));
    ir.emit(ir.cmp(4, cider::ir::CmpOp::Eq,
                   ir.value(0, cider::ir::IRType::F64, f),
                   ir.value(1, cider::ir::IRType::F64, f)));
    ir.emit(ir.constant(5, 7, cider::ir::IRType::I32, t));
    ir.emit(ir.constant(6, 2, cider::ir::IRType::I32, t));
    ir.emit(ir.div(7, ir.value(5, cider::ir::IRType::I32, t),
                   ir.value(6, cider::ir::IRType::I32, t), t));
    ir.emit(ir.rem(8, ir.value(5, cider::ir::IRType::I32, t),
                   ir.value(6, cider::ir::IRType::I32, t), t));
    ir.finishCondBr(0, 2, 1, 2);
    ir.addBlock(1);
    ir.finishRet(1, 2, cider::ir::IRType::Bool, b);
    ir.addBlock(2);
    ir.finishRet(2, 2, cider::ir::IRType::Bool, b);

    cider::opt::SccpResult result = cider::opt::sccp(ir.module);
    check(result.branchesFolded == 1, "the unrelated constant branch folds");
    check(result.constantsReplaced == 0,
          "none of the four guarded results is replaced");
    const cider::ir::Instr* c3 = instrResult(*ir.fn, 3);
    const cider::ir::Instr* c4 = instrResult(*ir.fn, 4);
    const cider::ir::Instr* d7 = instrResult(*ir.fn, 7);
    const cider::ir::Instr* r8 = instrResult(*ir.fn, 8);
    check(c3 != nullptr && c3->op == cider::ir::Op::Cmp &&
              c4 != nullptr && c4->op == cider::ir::Op::Cmp &&
              d7 != nullptr && d7->op == cider::ir::Op::Div &&
              r8 != nullptr && r8->op == cider::ir::Op::Rem,
          "both float Cmps and the Div/Rem keep their opcodes");
    check(countOp(*ir.fn, cider::ir::Op::Cmp) == 2 &&
              countOp(*ir.fn, cider::ir::Op::Div) == 1 &&
              countOp(*ir.fn, cider::ir::Op::Rem) == 1,
          "no compare or trapping division disappeared");
    check(structurallyValid(ir.module), "the module is structurally valid");
}

void testConstantShapesNarrowingAndWidening() {
    std::printf("narrowing and signed-to-unsigned widening Casts are not reused\n");
    Ir ir;
    cider::TypeRef i32 = ir.i32();
    cider::TypeRef i8 = ir.i8();
    cider::TypeRef u32 = ir.u32();
    cider::TypeRef b = ir.boolean();

    // C1: I32(-1) narrowed to I8; adding one must not reuse the untruncated -1.
    ir.emit(ir.constant(1, -1, cider::ir::IRType::I32, i32));
    ir.emit(ir.cast(2, ir.value(1, cider::ir::IRType::I32, i32),
                    cider::ir::IRType::I8, i8));
    ir.emit(ir.constant(3, 1, cider::ir::IRType::I8, i8));
    ir.emit(ir.binary(cider::ir::Op::Add, 4,
                      ir.value(2, cider::ir::IRType::I8, i8),
                      ir.value(3, cider::ir::IRType::I8, i8),
                      cider::ir::IRType::I8, i8));

    // C2: I8(-1) widened to U32; comparing to 0xFFFFFFFF must not read the
    // signed source value.
    ir.emit(ir.constant(5, -1, cider::ir::IRType::I8, i8));
    ir.emit(ir.cast(6, ir.value(5, cider::ir::IRType::I8, i8),
                    cider::ir::IRType::U32, u32));
    ir.emit(ir.constant(7, 0xFFFFFFFFLL, cider::ir::IRType::U32, u32));
    ir.emit(ir.cmp(8, cider::ir::CmpOp::Ult,
                   ir.value(6, cider::ir::IRType::U32, u32),
                   ir.value(7, cider::ir::IRType::U32, u32)));

    // An unrelated constant branch proves the pass actually ran.
    ir.emit(ir.constant(9, 1, cider::ir::IRType::Bool, b));
    ir.finishCondBr(0, 9, 1, 2);
    ir.addBlock(1);
    ir.finishRet(1, 1, cider::ir::IRType::I32, i32);
    ir.addBlock(2);
    ir.finishRet(2, 1, cider::ir::IRType::I32, i32);

    cider::opt::SccpResult result = cider::opt::sccp(ir.module);
    check(result.branchesFolded == 1, "the unrelated constant branch folds");
    check(result.constantsReplaced == 0,
          "no cast-derived result is replaced by a constant");
    const cider::ir::Instr* narrow = instrResult(*ir.fn, 2);
    const cider::ir::Instr* wide = instrResult(*ir.fn, 6);
    const cider::ir::Instr* sum = instrResult(*ir.fn, 4);
    const cider::ir::Instr* cmp = instrResult(*ir.fn, 8);
    check(narrow != nullptr && narrow->op == cider::ir::Op::Cast &&
              wide != nullptr && wide->op == cider::ir::Op::Cast,
          "both Casts remain");
    check(sum != nullptr && sum->op == cider::ir::Op::Add &&
              cmp != nullptr && cmp->op == cider::ir::Op::Cmp,
          "the Add and the Cmp remain unfolded");
    check(structurallyValid(ir.module), "the result is structurally valid");
}

}  // namespace

int main() {
    std::printf("sccp_test — sparse conditional constant propagation\n\n");
    testPhiMeetAndBranchFold();
    testLoopPhiRevisit();
    testTrappingAndVolatileBlocksSurvive();
    testFloatAndDivRemNotFolded();
    testConstantShapesNarrowingAndWidening();
    std::printf("\n%d checks, %s\n", checks,
                failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
