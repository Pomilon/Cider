// strength_test.cpp — integer strength reduction and peephole identities.
//
// Each case builds a small valid function, runs the pass directly, and asks
// the structural checker to verify the rewrite. The cases pin every rule at
// every integer width and signedness, the exact-width/type constant match,
// Copy-chain constant recognition, the nonconstant/float/handled-flag
// refusals, the deliberately skipped optional reductions, fixpoint ordering,
// clean replacement fields, determinism and the pipeline counter.

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
    const std::vector<cider::opt::StructuralError> errors =
        cider::opt::checkModule(module);
    for (const cider::opt::StructuralError& error : errors) {
        std::printf("       %s:%u: %s\n", error.function.c_str(),
                    error.block, error.message.c_str());
    }
    return errors.empty();
}

const cider::ir::Instr* instructionWithResult(const cider::ir::Function& fn,
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

struct WidthCase {
    cider::ir::IRType width;
    cider::BasicKind kind;
    int64_t allOnes;
};

const std::vector<WidthCase>& integerWidths() {
    static const std::vector<WidthCase> widths = {
        {cider::ir::IRType::I8, cider::BasicKind::I8, -1},
        {cider::ir::IRType::U8, cider::BasicKind::U8, 255},
        {cider::ir::IRType::I16, cider::BasicKind::I16, -1},
        {cider::ir::IRType::U16, cider::BasicKind::U16, 65535},
        {cider::ir::IRType::I32, cider::BasicKind::I32, -1},
        {cider::ir::IRType::U32, cider::BasicKind::U32, 4294967295LL},
        {cider::ir::IRType::I64, cider::BasicKind::I64, -1},
        {cider::ir::IRType::U64, cider::BasicKind::U64, -1},
    };
    return widths;
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

    cider::TypeRef basic(cider::BasicKind kind) {
        return cider::TypeRef{types.basic(kind)};
    }
    cider::TypeRef i32() { return basic(cider::BasicKind::I32); }
    cider::TypeRef u32() { return basic(cider::BasicKind::U32); }
    cider::TypeRef f32() { return basic(cider::BasicKind::F32); }
    cider::TypeRef f64() { return basic(cider::BasicKind::F64); }

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

    cider::ir::Instr& last() { return fn->blocks.back().body.back(); }

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

    void finishRet(uint32_t blockLabel, uint32_t id, cider::ir::IRType width,
                   cider::TypeRef type) {
        cider::ir::Terminator term;
        term.term = cider::ir::Term::Ret;
        term.values.push_back(value(id, width, type));
        for (cider::ir::Block& block : fn->blocks) {
            if (block.label == blockLabel) {
                block.term = term;
                return;
            }
        }
    }

    void finishBr(uint32_t blockLabel, uint32_t target) {
        cider::ir::Terminator term;
        term.term = cider::ir::Term::Br;
        term.target = target;
        for (cider::ir::Block& block : fn->blocks) {
            if (block.label == blockLabel) {
                block.term = term;
                return;
            }
        }
    }
};

void checkCopy(const cider::ir::Function& fn, uint32_t resultId,
               uint32_t sourceId, cider::ir::IRType width,
               cider::TypeRef type, const std::string& what) {
    const cider::ir::Instr* instr = instructionWithResult(fn, resultId);
    check(instr != nullptr && instr->op == cider::ir::Op::Copy &&
              instr->results.size() == 1 && instr->operands.size() == 1 &&
              instr->operands[0].id == sourceId &&
              instr->results[0].width == width &&
              instr->results[0].type == type && instr->width == width &&
              instr->type == type && instr->isPure && !instr->mayTrap &&
              !instr->isVolatile,
          what);
}

void checkZero(const cider::ir::Function& fn, uint32_t resultId,
               cider::ir::IRType width, cider::TypeRef type,
               const std::string& what) {
    const cider::ir::Instr* instr = instructionWithResult(fn, resultId);
    check(instr != nullptr && instr->op == cider::ir::Op::Const &&
              instr->results.size() == 1 && instr->operands.empty() &&
              instr->constantBits == 0 && instr->constantText == "0" &&
              instr->results[0].width == width &&
              instr->results[0].type == type && instr->width == width &&
              instr->type == type && instr->isPure && !instr->mayTrap &&
              !instr->isVolatile,
          what);
}

// Every rule at its own width, for every integer width and signedness.
void testAllWidths() {
    std::printf("every rule is exact at every integer width/signedness\n");
    Ir ir;
    uint32_t nextId = 1000;
    struct ResultIds {
        cider::ir::IRType width;
        cider::TypeRef type;
        uint32_t param = 0;
        uint32_t add = 0;
        uint32_t sub = 0;
        uint32_t orZero = 0;
        uint32_t xorZero = 0;
        uint32_t shlZero = 0;
        uint32_t shrZero = 0;
        uint32_t mulOne = 0;
        uint32_t andOnes = 0;
        uint32_t divOne = 0;
        uint32_t remOne = 0;
        uint32_t mulZero = 0;
    };
    std::vector<ResultIds> cases;
    for (const WidthCase& width : integerWidths()) {
        cider::TypeRef type = ir.basic(width.kind);
        ResultIds ids;
        ids.width = width.width;
        ids.type = type;
        ids.param = nextId++;
        ids.add = nextId++;
        ids.sub = nextId++;
        ids.orZero = nextId++;
        ids.xorZero = nextId++;
        ids.shlZero = nextId++;
        ids.shrZero = nextId++;
        ids.mulOne = nextId++;
        ids.andOnes = nextId++;
        ids.divOne = nextId++;
        ids.remOne = nextId++;
        ids.mulZero = nextId++;
        const uint32_t zero = nextId++;
        const uint32_t one = nextId++;
        const uint32_t allOnes = nextId++;

        ir.param(ids.param, width.width, type);
        ir.emit(ir.constant(zero, 0, width.width, type));
        ir.emit(ir.constant(one, 1, width.width, type));
        ir.emit(ir.constant(allOnes, width.allOnes, width.width, type));
        const cider::ir::Value x = ir.value(ids.param, width.width, type);
        ir.emit(ir.binary(cider::ir::Op::Add, ids.add, x,
                          ir.value(zero, width.width, type), width.width,
                          type));
        ir.emit(ir.binary(cider::ir::Op::Sub, ids.sub, x,
                          ir.value(zero, width.width, type), width.width,
                          type));
        ir.emit(ir.binary(cider::ir::Op::Or, ids.orZero,
                          ir.value(zero, width.width, type), x, width.width,
                          type));
        ir.emit(ir.binary(cider::ir::Op::Xor, ids.xorZero, x,
                          ir.value(zero, width.width, type), width.width,
                          type));
        ir.emit(ir.binary(cider::ir::Op::Shl, ids.shlZero, x,
                          ir.value(zero, width.width, type), width.width,
                          type));
        ir.emit(ir.binary(cider::ir::Op::Shr, ids.shrZero, x,
                          ir.value(zero, width.width, type), width.width,
                          type));
        ir.emit(ir.binary(cider::ir::Op::Mul, ids.mulOne, x,
                          ir.value(one, width.width, type), width.width,
                          type));
        ir.emit(ir.binary(cider::ir::Op::And, ids.andOnes, x,
                          ir.value(allOnes, width.width, type), width.width,
                          type));
        ir.emit(ir.binary(cider::ir::Op::Div, ids.divOne, x,
                          ir.value(one, width.width, type), width.width,
                          type));
        ir.last().mayTrap = true;
        ir.emit(ir.binary(cider::ir::Op::Rem, ids.remOne, x,
                          ir.value(one, width.width, type), width.width,
                          type));
        ir.last().mayTrap = true;
        ir.emit(ir.binary(cider::ir::Op::Mul, ids.mulZero, x,
                          ir.value(zero, width.width, type), width.width,
                          type));
        cases.push_back(ids);
    }
    ir.finishRet(0, cases.back().param, cases.back().width, cases.back().type);

    cider::opt::StrengthResult result = cider::opt::strengthReduce(ir.module);
    check(structurallyValid(ir.module), "the result is structurally valid");
    check(result.reduced == cases.size() * 11 &&
              result.copies == cases.size() * 9 &&
              result.constants == cases.size() * 2,
          "every width contributes nine copies and two constants");
    for (const ResultIds& ids : cases) {
        const std::string at = std::string(cider::ir::irTypeName(ids.width));
        checkCopy(*ir.fn, ids.add, ids.param, ids.width, ids.type,
                  "Add x,0 -> Copy x at " + at);
        checkCopy(*ir.fn, ids.sub, ids.param, ids.width, ids.type,
                  "Sub x,0 -> Copy x at " + at);
        checkCopy(*ir.fn, ids.orZero, ids.param, ids.width, ids.type,
                  "Or 0,x -> Copy x at " + at);
        checkCopy(*ir.fn, ids.xorZero, ids.param, ids.width, ids.type,
                  "Xor x,0 -> Copy x at " + at);
        checkCopy(*ir.fn, ids.shlZero, ids.param, ids.width, ids.type,
                  "Shl x,0 -> Copy x at " + at);
        checkCopy(*ir.fn, ids.shrZero, ids.param, ids.width, ids.type,
                  "Shr x,0 -> Copy x at " + at);
        checkCopy(*ir.fn, ids.mulOne, ids.param, ids.width, ids.type,
                  "Mul x,1 -> Copy x at " + at);
        checkCopy(*ir.fn, ids.andOnes, ids.param, ids.width, ids.type,
                  "And x,all-ones -> Copy x at " + at);
        checkCopy(*ir.fn, ids.divOne, ids.param, ids.width, ids.type,
                  "Div x,1 -> Copy x at " + at);
        checkZero(*ir.fn, ids.remOne, ids.width, ids.type,
                  "Rem x,1 -> Const 0 at " + at);
        checkZero(*ir.fn, ids.mulZero, ids.width, ids.type,
                  "Mul x,0 -> Const 0 at " + at);
    }
}

// Both operand orders for the commutative rules, plus the nonconstant refusals.
void testOperandOrdersAndNonConstants() {
    std::printf("commutative identities match both sides; nonconstants stay\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::I32, t);
    ir.param(1, cider::ir::IRType::I32, t);
    const cider::ir::Value x = ir.value(0, cider::ir::IRType::I32, t);
    const cider::ir::Value y = ir.value(1, cider::ir::IRType::I32, t);
    ir.emit(ir.constant(2, 0, cider::ir::IRType::I32, t));
    ir.emit(ir.constant(3, 1, cider::ir::IRType::I32, t));
    ir.emit(ir.constant(4, -1, cider::ir::IRType::I32, t));
    const cider::ir::Value zero = ir.value(2, cider::ir::IRType::I32, t);
    const cider::ir::Value one = ir.value(3, cider::ir::IRType::I32, t);
    const cider::ir::Value allOnes = ir.value(4, cider::ir::IRType::I32, t);
    ir.emit(ir.binary(cider::ir::Op::Add, 10, x, zero,
                      cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::Add, 11, zero, x,
                      cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::Or, 12, x, zero,
                      cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::Or, 13, zero, x,
                      cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::Xor, 14, x, zero,
                      cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::Xor, 15, zero, x,
                      cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::Mul, 16, x, one,
                      cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::Mul, 17, one, x,
                      cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::And, 18, x, allOnes,
                      cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::And, 19, allOnes, x,
                      cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::Mul, 20, x, zero,
                      cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::Mul, 21, zero, x,
                      cider::ir::IRType::I32, t));

    ir.emit(ir.binary(cider::ir::Op::Add, 30, x, y,
                      cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::Sub, 31, x, y,
                      cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::Or, 32, x, y,
                      cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::Xor, 33, x, y,
                      cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::Mul, 34, x, y,
                      cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::And, 35, x, y,
                      cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::Shl, 36, x, y,
                      cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::Shr, 37, x, y,
                      cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::Div, 38, x, y,
                      cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::Rem, 39, x, y,
                      cider::ir::IRType::I32, t));
    ir.finishRet(0, 39, cider::ir::IRType::I32, t);

    cider::opt::StrengthResult result = cider::opt::strengthReduce(ir.module);
    check(structurallyValid(ir.module), "the result is structurally valid");
    check(result.reduced == 12 && result.copies == 10 &&
              result.constants == 2,
          "both operand orders are rewritten and no others are");
    for (uint32_t id = 10; id <= 19; ++id) {
        const cider::ir::Instr* instr = instructionWithResult(*ir.fn, id);
        check(instr != nullptr && instr->op == cider::ir::Op::Copy,
              "commutative identity " + std::to_string(id) + " is a copy");
    }
    for (uint32_t id = 20; id <= 21; ++id) {
        const cider::ir::Instr* instr = instructionWithResult(*ir.fn, id);
        check(instr != nullptr && instr->op == cider::ir::Op::Const &&
                  instr->constantBits == 0,
              "mul by zero " + std::to_string(id) + " is constant zero");
    }
    for (uint32_t id = 30; id <= 39; ++id) {
        const cider::ir::Instr* instr = instructionWithResult(*ir.fn, id);
        check(instr != nullptr && instr->op != cider::ir::Op::Copy &&
                  instr->op != cider::ir::Op::Const,
              "nonconstant operand " + std::to_string(id) + " is untouched");
    }
}

void testAndNotAllOnes() {
    std::printf("an all-ones test is exact at the width, not a wide mask\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::I32, t);
    const cider::ir::Value x = ir.value(0, cider::ir::IRType::I32, t);
    ir.emit(ir.constant(1, 0x7FFFFFFF, cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::And, 2, x,
                      ir.value(1, cider::ir::IRType::I32, t),
                      cider::ir::IRType::I32, t));
    ir.finishRet(0, 2, cider::ir::IRType::I32, t);
    cider::opt::StrengthResult result = cider::opt::strengthReduce(ir.module);
    check(structurallyValid(ir.module), "the result is structurally valid");
    const cider::ir::Instr* instr = instructionWithResult(*ir.fn, 2);
    check(instr != nullptr && instr->op == cider::ir::Op::And,
          "a mask with one clear bit stays an And");
    check(result.reduced == 0, "no counter moves");
}

void testFloatUntouched() {
    std::printf("float arithmetic is never treated as integer arithmetic\n");
    Ir ir;
    const cider::ir::IRType f32 = cider::ir::IRType::F32;
    const cider::ir::IRType f64 = cider::ir::IRType::F64;
    ir.param(0, f32, ir.f32());
    ir.param(1, f64, ir.f64());
    ir.emit(ir.constant(2, 0, f32, ir.f32()));
    ir.emit(ir.constant(3, 1, f64, ir.f64()));
    ir.emit(ir.binary(cider::ir::Op::Add, 10, ir.value(0, f32, ir.f32()),
                      ir.value(2, f32, ir.f32()), f32, ir.f32()));
    ir.emit(ir.binary(cider::ir::Op::Mul, 11, ir.value(1, f64, ir.f64()),
                      ir.value(3, f64, ir.f64()), f64, ir.f64()));
    ir.emit(ir.binary(cider::ir::Op::Div, 12, ir.value(1, f64, ir.f64()),
                      ir.value(3, f64, ir.f64()), f64, ir.f64()));
    ir.emit(ir.binary(cider::ir::Op::Rem, 13, ir.value(1, f64, ir.f64()),
                      ir.value(3, f64, ir.f64()), f64, ir.f64()));
    ir.finishRet(0, 13, f64, ir.f64());
    cider::opt::StrengthResult result = cider::opt::strengthReduce(ir.module);
    check(structurallyValid(ir.module), "the result is structurally valid");
    check(result.reduced == 0, "no float instruction is rewritten");
    for (uint32_t id = 10; id <= 13; ++id) {
        const cider::ir::Instr* instr = instructionWithResult(*ir.fn, id);
        check(instr != nullptr && instr->op != cider::ir::Op::Copy &&
                  instr->op != cider::ir::Op::Const,
              "float instruction " + std::to_string(id) + " is untouched");
    }
}

void testFlagsAndTraps() {
    std::printf("volatile and trapping shapes are refused, safe divide is not\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::I32, t);
    const cider::ir::Value x = ir.value(0, cider::ir::IRType::I32, t);
    ir.emit(ir.constant(1, 0, cider::ir::IRType::I32, t));
    ir.emit(ir.constant(2, 1, cider::ir::IRType::I32, t));
    ir.emit(ir.constant(3, 2, cider::ir::IRType::I32, t));
    const cider::ir::Value zero = ir.value(1, cider::ir::IRType::I32, t);
    const cider::ir::Value one = ir.value(2, cider::ir::IRType::I32, t);
    const cider::ir::Value two = ir.value(3, cider::ir::IRType::I32, t);

    ir.emit(ir.binary(cider::ir::Op::Add, 10, x, zero,
                      cider::ir::IRType::I32, t));
    ir.last().isVolatile = true;
    ir.emit(ir.binary(cider::ir::Op::Mul, 11, x, one,
                      cider::ir::IRType::I32, t));
    ir.last().mayTrap = true;
    ir.emit(ir.binary(cider::ir::Op::Add, 12, x, zero,
                      cider::ir::IRType::I32, t));
    ir.last().mayTrap = true;
    ir.emit(ir.binary(cider::ir::Op::Div, 13, x, two,
                      cider::ir::IRType::I32, t));
    ir.last().mayTrap = true;
    ir.emit(ir.binary(cider::ir::Op::Rem, 14, x, two,
                      cider::ir::IRType::I32, t));
    ir.last().mayTrap = true;
    ir.emit(ir.binary(cider::ir::Op::Div, 15, x, one,
                      cider::ir::IRType::I32, t));
    ir.last().mayTrap = true;
    ir.emit(ir.binary(cider::ir::Op::Rem, 16, x, one,
                      cider::ir::IRType::I32, t));
    ir.last().mayTrap = true;
    ir.finishRet(0, 16, cider::ir::IRType::I32, t);

    cider::opt::StrengthResult result = cider::opt::strengthReduce(ir.module);
    check(structurallyValid(ir.module), "the result is structurally valid");
    check(result.reduced == 2 && result.copies == 1 && result.constants == 1,
          "only Div/Rem by one are rewritten");
    check(instructionWithResult(*ir.fn, 10)->op == cider::ir::Op::Add,
          "a volatile Add x,0 is untouched");
    check(instructionWithResult(*ir.fn, 11)->op == cider::ir::Op::Mul,
          "a trapping Mul x,1 is untouched");
    check(instructionWithResult(*ir.fn, 12)->op == cider::ir::Op::Add,
          "a trapping Add x,0 is untouched");
    check(instructionWithResult(*ir.fn, 13)->op == cider::ir::Op::Div,
          "Div x,2 stays");
    check(instructionWithResult(*ir.fn, 14)->op == cider::ir::Op::Rem,
          "Rem x,2 stays");
    const cider::ir::Instr* div = instructionWithResult(*ir.fn, 15);
    check(div != nullptr && div->op == cider::ir::Op::Copy && !div->mayTrap,
          "Div x,1 becomes a non-trapping copy");
    const cider::ir::Instr* rem = instructionWithResult(*ir.fn, 16);
    check(rem != nullptr && rem->op == cider::ir::Op::Const &&
              !rem->mayTrap,
          "Rem x,1 becomes a non-trapping constant");
}

void testWidthAndTypeMismatch() {
    std::printf("constants and operands must match the exact width/type\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    cider::TypeRef u = ir.u32();
    ir.param(0, cider::ir::IRType::I32, t);
    ir.param(1, cider::ir::IRType::U32, u);
    const cider::ir::Value x = ir.value(0, cider::ir::IRType::I32, t);
    const cider::ir::Value wrong = ir.value(1, cider::ir::IRType::U32, u);
    ir.emit(ir.constant(2, 0, cider::ir::IRType::U32, u));
    ir.emit(ir.constant(3, 1, cider::ir::IRType::U32, u));
    ir.emit(ir.constant(4, 4294967295LL, cider::ir::IRType::U32, u));
    const cider::ir::Value zero = ir.value(2, cider::ir::IRType::U32, u);
    const cider::ir::Value one = ir.value(3, cider::ir::IRType::U32, u);
    const cider::ir::Value ones = ir.value(4, cider::ir::IRType::U32, u);

    ir.emit(ir.binary(cider::ir::Op::Add, 10, x, zero,
                      cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::Mul, 11, x, one,
                      cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::And, 12, x, ones,
                      cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::Div, 13, x, one,
                      cider::ir::IRType::I32, t));
    ir.last().mayTrap = true;
    ir.emit(ir.binary(cider::ir::Op::Rem, 14, x, one,
                      cider::ir::IRType::I32, t));
    ir.last().mayTrap = true;
    ir.emit(ir.binary(cider::ir::Op::Mul, 15, x, zero,
                      cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::Add, 16, wrong, ir.value(2,
                      cider::ir::IRType::I32, t), cider::ir::IRType::I32, t));
    ir.finishRet(0, 16, cider::ir::IRType::I32, t);

    cider::opt::StrengthResult result = cider::opt::strengthReduce(ir.module);
    check(structurallyValid(ir.module), "the result is structurally valid");
    check(result.reduced == 0, "no mismatched constant or operand is coerced");
    for (uint32_t id = 10; id <= 16; ++id) {
        const cider::ir::Instr* instr = instructionWithResult(*ir.fn, id);
        check(instr != nullptr && instr->op != cider::ir::Op::Copy &&
                  instr->op != cider::ir::Op::Const,
              "mismatched shape " + std::to_string(id) + " is untouched");
    }
}

void testCopyChainConstants() {
    std::printf("a same-width Copy chain exposes its constant\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::I32, t);
    const cider::ir::Value x = ir.value(0, cider::ir::IRType::I32, t);
    ir.emit(ir.constant(1, 1, cider::ir::IRType::I32, t));
    ir.emit(ir.copy(2, ir.value(1, cider::ir::IRType::I32, t),
                    cider::ir::IRType::I32, t));
    ir.emit(ir.copy(3, ir.value(2, cider::ir::IRType::I32, t),
                    cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::Mul, 4, x,
                      ir.value(3, cider::ir::IRType::I32, t),
                      cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::Div, 5, x,
                      ir.value(3, cider::ir::IRType::I32, t),
                      cider::ir::IRType::I32, t));
    ir.last().mayTrap = true;
    ir.finishRet(0, 5, cider::ir::IRType::I32, t);
    cider::opt::StrengthResult result = cider::opt::strengthReduce(ir.module);
    check(structurallyValid(ir.module), "the result is structurally valid");
    check(result.reduced == 2 && result.copies == 2,
          "both identity users see the copied constant");
    check(instructionWithResult(*ir.fn, 4)->op == cider::ir::Op::Copy &&
              instructionWithResult(*ir.fn, 4)->operands[0].id == 0,
          "Mul x, copy(1) becomes Copy x");
    check(instructionWithResult(*ir.fn, 5)->op == cider::ir::Op::Copy &&
              instructionWithResult(*ir.fn, 5)->operands[0].id == 0,
          "Div x, copy(1) becomes Copy x");
}

void testFixpoint() {
    std::printf("a chain in one block reaches a fixpoint\n");
    {
        Ir ir;
        cider::TypeRef t = ir.i32();
        ir.param(0, cider::ir::IRType::I32, t);
        const cider::ir::Value x = ir.value(0, cider::ir::IRType::I32, t);
        ir.emit(ir.constant(1, 0, cider::ir::IRType::I32, t));
        ir.emit(ir.constant(2, 1, cider::ir::IRType::I32, t));
        ir.emit(ir.binary(cider::ir::Op::Add, 10, x,
                          ir.value(1, cider::ir::IRType::I32, t),
                          cider::ir::IRType::I32, t));
        ir.emit(ir.binary(cider::ir::Op::Sub, 11,
                          ir.value(10, cider::ir::IRType::I32, t),
                          ir.value(1, cider::ir::IRType::I32, t),
                          cider::ir::IRType::I32, t));
        ir.emit(ir.binary(cider::ir::Op::Mul, 12,
                          ir.value(11, cider::ir::IRType::I32, t),
                          ir.value(2, cider::ir::IRType::I32, t),
                          cider::ir::IRType::I32, t));
        ir.emit(ir.binary(cider::ir::Op::Div, 13,
                          ir.value(12, cider::ir::IRType::I32, t),
                          ir.value(2, cider::ir::IRType::I32, t),
                          cider::ir::IRType::I32, t));
        ir.last().mayTrap = true;
        ir.finishRet(0, 13, cider::ir::IRType::I32, t);
        cider::opt::StrengthResult result =
            cider::opt::strengthReduce(ir.module);
        check(structurallyValid(ir.module), "the chain result is valid");
        check(result.reduced == 4 && result.copies == 4,
              "all four chained identities are rewritten");
        for (uint32_t id = 10; id <= 13; ++id) {
            const uint32_t source = id == 10 ? 0 : id - 1;
            checkCopy(*ir.fn, id, source, cider::ir::IRType::I32, t,
                      "chained identity " + std::to_string(id) +
                          " copies its rewritten operand");
        }
    }
    // A constant defined in a block that appears later in module order but
    // dominates the user earlier in the scan. The first scan rewrites the Rem;
    // the second must see it to fold the Mul that was scanned first.
    {
        Ir ir;
        cider::TypeRef t = ir.i32();
        ir.param(0, cider::ir::IRType::I32, t);
        ir.param(1, cider::ir::IRType::I32, t);
        const cider::ir::Value x = ir.value(0, cider::ir::IRType::I32, t);
        const cider::ir::Value y = ir.value(1, cider::ir::IRType::I32, t);
        ir.emit(ir.constant(2, 1, cider::ir::IRType::I32, t));
        ir.finishBr(0, 3);
        ir.addBlock(2);
        ir.emit(ir.binary(cider::ir::Op::Mul, 20, y,
                          ir.value(21, cider::ir::IRType::I32, t),
                          cider::ir::IRType::I32, t));
        ir.finishRet(2, 20, cider::ir::IRType::I32, t);
        ir.addBlock(3);
        ir.emit(ir.binary(cider::ir::Op::Rem, 21, x,
                          ir.value(2, cider::ir::IRType::I32, t),
                          cider::ir::IRType::I32, t));
        ir.last().mayTrap = true;
        ir.finishBr(3, 2);

        cider::opt::StrengthResult result =
            cider::opt::strengthReduce(ir.module);
        check(structurallyValid(ir.module),
              "the cross-block chain result is valid");
        check(result.reduced == 2 && result.constants == 2,
              "the second scan folds the Mul left of the Rem");
        checkZero(*ir.fn, 21, cider::ir::IRType::I32, t,
                  "the dominating Rem x,1 becomes zero");
        checkZero(*ir.fn, 20, cider::ir::IRType::I32, t,
                  "the earlier-scanned Mul y,zero becomes zero");
    }
}

void testOptionalRulesSkipped() {
    std::printf("the deliberately skipped optional rules stay in place\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::I32, t);
    const cider::ir::Value x = ir.value(0, cider::ir::IRType::I32, t);
    ir.emit(ir.constant(1, 0, cider::ir::IRType::I32, t));
    ir.emit(ir.constant(2, -1, cider::ir::IRType::I32, t));
    ir.emit(ir.constant(3, 2, cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::Sub, 10,
                      ir.value(1, cider::ir::IRType::I32, t), x,
                      cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::Xor, 11, x,
                      ir.value(2, cider::ir::IRType::I32, t),
                      cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::Mul, 12, x,
                      ir.value(3, cider::ir::IRType::I32, t),
                      cider::ir::IRType::I32, t));
    ir.finishRet(0, 12, cider::ir::IRType::I32, t);
    cider::opt::StrengthResult result = cider::opt::strengthReduce(ir.module);
    check(structurallyValid(ir.module), "the result is structurally valid");
    check(result.reduced == 0, "no optional rule fires");
    check(instructionWithResult(*ir.fn, 10)->op == cider::ir::Op::Sub,
          "Sub 0,x is not rewritten to Neg");
    check(instructionWithResult(*ir.fn, 11)->op == cider::ir::Op::Xor,
          "Xor x,-1 is not rewritten to BitNot");
    check(instructionWithResult(*ir.fn, 12)->op == cider::ir::Op::Mul,
          "Mul x,2 is not rewritten to a shift");
}

void testFieldsCleared() {
    std::printf("a replacement has no stale payload or trapping flag\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::I32, t);
    const cider::ir::Value x = ir.value(0, cider::ir::IRType::I32, t);
    ir.emit(ir.constant(1, 1, cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::Div, 10, x,
                      ir.value(1, cider::ir::IRType::I32, t),
                      cider::ir::IRType::I32, t));
    cider::ir::Instr& div = ir.last();
    div.mayTrap = true;
    div.overflow = cider::ir::OverflowMode::Wrap;
    div.constantBits = 99;
    div.constantText = "junk";
    div.addressBase = x;
    div.displacement = 7;
    div.field = 3;
    div.storedValue = x;
    div.cmp = cider::ir::CmpOp::Ge;
    div.callee = "callee";
    div.globalLabel = "global";
    div.calleeValue = x;
    div.reason = cider::ir::TrapReason::DivByZero;
    div.asmText = "nop";
    div.asmNames.push_back("name");
    div.call.indirect = true;
    div.phiIncoming.push_back(cider::ir::PhiIncoming{x, 0});
    ir.finishRet(0, 10, cider::ir::IRType::I32, t);

    cider::opt::StrengthResult result = cider::opt::strengthReduce(ir.module);
    check(structurallyValid(ir.module), "the result is structurally valid");
    check(result.reduced == 1, "the division is rewritten");
    const cider::ir::Instr* instr = instructionWithResult(*ir.fn, 10);
    check(instr != nullptr && instr->op == cider::ir::Op::Copy &&
              instr->results.size() == 1 &&
              instr->results[0].id == 10 &&
              instr->results[0].width == cider::ir::IRType::I32 &&
              instr->results[0].type == t && instr->operands.size() == 1 &&
              instr->operands[0].id == 0,
          "the result id, width and type and the copied operand survive");
    check(instr != nullptr && instr->overflow == cider::ir::OverflowMode::Wrap,
          "the equivalent overflow mode is preserved");
    check(instr != nullptr && instr->mayTrap == false &&
              instr->isVolatile == false && instr->isPure,
          "the replacement is pure, non-volatile and non-trapping");
    check(instr != nullptr && instr->constantBits == 0 &&
              instr->constantText.empty() && instr->addressBase.id == 0 &&
              instr->displacement == 0 && instr->field == 0 &&
              instr->storedValue.id == 0 &&
              instr->cmp == cider::ir::CmpOp::Eq && instr->callee.empty() &&
              instr->globalLabel.empty() && instr->calleeValue.id == 0 &&
              instr->reason == cider::ir::TrapReason::Explicit &&
              instr->asmText.empty() && instr->asmNames.empty() &&
              !instr->call.indirect && instr->phiIncoming.empty() &&
              instr->arguments.empty(),
          "every old payload field is cleared");
}

void testTrappingOperandSurvives() {
    std::printf("zero/remainder rewrites keep a trapping operand definition\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::I32, t);
    const cider::ir::Value x = ir.value(0, cider::ir::IRType::I32, t);
    ir.emit(ir.constant(1, 1, cider::ir::IRType::I32, t));
    ir.emit(ir.constant(2, 2, cider::ir::IRType::I32, t));
    ir.emit(ir.constant(3, 0, cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::Div, 10, x,
                      ir.value(2, cider::ir::IRType::I32, t),
                      cider::ir::IRType::I32, t));
    ir.last().mayTrap = true;
    ir.emit(ir.binary(cider::ir::Op::Mul, 11,
                      ir.value(10, cider::ir::IRType::I32, t),
                      ir.value(3, cider::ir::IRType::I32, t),
                      cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::Rem, 12,
                      ir.value(10, cider::ir::IRType::I32, t),
                      ir.value(1, cider::ir::IRType::I32, t),
                      cider::ir::IRType::I32, t));
    ir.last().mayTrap = true;
    ir.finishRet(0, 12, cider::ir::IRType::I32, t);
    cider::opt::StrengthResult result = cider::opt::strengthReduce(ir.module);
    check(structurallyValid(ir.module), "the result is structurally valid");
    check(result.constants == 2, "both zero-producing rules fire");
    const cider::ir::Instr* division = instructionWithResult(*ir.fn, 10);
    check(division != nullptr && division->op == cider::ir::Op::Div &&
              division->mayTrap,
          "the trapping Div x,2 definition is still present and still traps");
    checkZero(*ir.fn, 11, cider::ir::IRType::I32, t,
              "Mul (Div x,2),0 becomes zero");
    checkZero(*ir.fn, 12, cider::ir::IRType::I32, t,
              "Rem (Div x,2),1 becomes zero");
}

void testDeterminism() {
    std::printf("identical modules reduce identically and idempotently\n");
    Ir first;
    Ir second;
    auto build = [](Ir& ir) {
        cider::TypeRef t = ir.i32();
        ir.param(0, cider::ir::IRType::I32, t);
        const cider::ir::Value x = ir.value(0, cider::ir::IRType::I32, t);
        ir.emit(ir.constant(1, 0, cider::ir::IRType::I32, t));
        ir.emit(ir.constant(2, 1, cider::ir::IRType::I32, t));
        ir.emit(ir.constant(3, -1, cider::ir::IRType::I32, t));
        ir.emit(ir.binary(cider::ir::Op::Add, 10, x,
                          ir.value(1, cider::ir::IRType::I32, t),
                          cider::ir::IRType::I32, t));
        ir.emit(ir.binary(cider::ir::Op::Mul, 11,
                          ir.value(10, cider::ir::IRType::I32, t),
                          ir.value(2, cider::ir::IRType::I32, t),
                          cider::ir::IRType::I32, t));
        ir.emit(ir.binary(cider::ir::Op::And, 12,
                          ir.value(11, cider::ir::IRType::I32, t),
                          ir.value(3, cider::ir::IRType::I32, t),
                          cider::ir::IRType::I32, t));
        ir.emit(ir.binary(cider::ir::Op::Rem, 13,
                          ir.value(12, cider::ir::IRType::I32, t),
                          ir.value(2, cider::ir::IRType::I32, t),
                          cider::ir::IRType::I32, t));
        ir.last().mayTrap = true;
        ir.finishRet(0, 13, cider::ir::IRType::I32, t);
    };
    build(first);
    build(second);
    cider::opt::StrengthResult one = cider::opt::strengthReduce(first.module);
    cider::opt::StrengthResult two = cider::opt::strengthReduce(second.module);
    check(structurallyValid(first.module), "the first result is valid");
    check(structurallyValid(second.module), "the second result is valid");
    check(cider::ir::dumpModule(first.module) ==
              cider::ir::dumpModule(second.module),
          "two identical modules dump byte-identically");
    check(one.reduced == two.reduced && one.copies == two.copies &&
              one.constants == two.constants,
          "the counters do not depend on construction history");
    const std::string once = cider::ir::dumpModule(first.module);
    cider::opt::StrengthResult again = cider::opt::strengthReduce(first.module);
    check(again.reduced == 0 && again.copies == 0 && again.constants == 0,
          "a second run is a no-op");
    check(cider::ir::dumpModule(first.module) == once,
          "the second run does not change the dump");
}

void testPipelineResult() {
    std::printf("the pipeline reports strength-reduction counters\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.param(0, cider::ir::IRType::I32, t);
    const cider::ir::Value x = ir.value(0, cider::ir::IRType::I32, t);
    ir.emit(ir.constant(1, 0, cider::ir::IRType::I32, t));
    ir.emit(ir.binary(cider::ir::Op::Add, 10, x,
                      ir.value(1, cider::ir::IRType::I32, t),
                      cider::ir::IRType::I32, t));
    ir.finishRet(0, 10, cider::ir::IRType::I32, t);
    cider::opt::OptResult result = cider::opt::run(ir.module);
    check(result.ok, "the pipeline succeeds: " + result.error);
    check(structurallyValid(ir.module), "the pipeline result is valid");
    check(result.strengthReduced == 1 && result.strengthCopies == 1 &&
              result.strengthConstants == 0,
          "the strength counters reach OptResult");
    check(instructionWithResult(*ir.fn, 10) == nullptr ||
              instructionWithResult(*ir.fn, 10)->op == cider::ir::Op::Copy,
          "the identity is gone from the optimized function");
}

}  // namespace

int main() {
    std::printf("strength_test — integer identities and strength reduction\n\n");
    testAllWidths();
    testOperandOrdersAndNonConstants();
    testAndNotAllOnes();
    testFloatUntouched();
    testFlagsAndTraps();
    testWidthAndTypeMismatch();
    testCopyChainConstants();
    testFixpoint();
    testOptionalRulesSkipped();
    testFieldsCleared();
    testTrappingOperandSurvives();
    testDeterminism();
    testPipelineResult();
    std::printf("\n%d checks, %s\n", checks,
                failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
