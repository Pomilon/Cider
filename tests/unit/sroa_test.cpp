// sroa_test.cpp — escape/GEP-range analysis and aggregate SROA.
//
// Each case builds a small valid function, asks analyzeMemory for its per-slot
// facts and runs sroa, then checks the exact counters and that the rewritten
// module is still structurally valid. The neighbour case pins Review Focus #5:
// an escaped address must never let a GEP scalarize an adjacent allocation.

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

int countOp(const cider::ir::Module& module, cider::ir::Op op) {
    int n = 0;
    for (const cider::ir::Function& fn : module.functions) {
        n += countOp(fn, op);
    }
    return n;
}

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
        newFunction("f");
    }

    void newFunction(const std::string& name) {
        cider::ir::Function function;
        function.name = name;
        cider::ir::Block block;
        block.label = 0;
        function.blocks.push_back(block);
        module.functions.push_back(std::move(function));
        fn = &module.functions.back();
    }

    cider::TypeRef basic(cider::BasicKind kind) {
        return cider::TypeRef{types.basic(kind)};
    }
    cider::TypeRef i64() { return basic(cider::BasicKind::I64); }
    cider::TypeRef i32() { return basic(cider::BasicKind::I32); }
    cider::TypeRef u8() { return basic(cider::BasicKind::U8); }

    cider::TypeRef ptrType() { return cider::TypeRef{types.pointer(u8().type)}; }

    cider::ir::Value value(uint32_t id, cider::ir::IRType width,
                           cider::TypeRef type) {
        cider::ir::Value out;
        out.id = id;
        out.width = width;
        out.type = type;
        return out;
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

    cider::ir::Instr allocaSlot(uint32_t id, cider::TypeRef pointee) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Alloca;
        instr.width = cider::ir::IRType::Ptr;
        instr.type = cider::TypeRef{types.pointer(pointee.type)};
        instr.pointee = pointee;
        instr.results.push_back(value(id, instr.width, instr.type));
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

    cider::ir::Instr gep(uint32_t result, uint32_t base, int64_t displacement) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Gep;
        instr.width = cider::ir::IRType::Ptr;
        instr.type = ptrType();
        instr.isPure = true;
        instr.addressBase = value(base, cider::ir::IRType::Ptr, ptrType());
        instr.displacement = displacement;
        instr.results.push_back(value(result, instr.width, instr.type));
        return instr;
    }

    cider::ir::Instr store(uint32_t base, cider::ir::Value stored,
                           cider::ir::IRType width, cider::TypeRef type,
                           int64_t displacement = 0, bool isVolatile = false,
                           const std::string& label = "") {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Store;
        instr.width = width;
        instr.type = type;
        instr.addressBase = value(base, cider::ir::IRType::Ptr, ptrType());
        instr.pointee = type;
        instr.displacement = displacement;
        instr.storedValue = stored;
        instr.isVolatile = isVolatile;
        instr.globalLabel = label;
        return instr;
    }

    cider::ir::Instr load(uint32_t result, uint32_t base,
                          cider::ir::IRType width, cider::TypeRef type,
                          int64_t displacement = 0, bool isVolatile = false) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Load;
        instr.width = width;
        instr.type = type;
        instr.addressBase = value(base, cider::ir::IRType::Ptr, ptrType());
        instr.pointee = type;
        instr.displacement = displacement;
        instr.isVolatile = isVolatile;
        instr.results.push_back(value(result, width, type));
        return instr;
    }

    cider::ir::Instr callWithSlot(uint32_t base, const std::string& callee) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Call;
        instr.callee = callee;
        cider::ir::Argument arg;
        arg.words.push_back(value(base, cider::ir::IRType::Ptr, ptrType()));
        instr.arguments.push_back(arg);
        return instr;
    }

    void finishRet(uint32_t id, cider::ir::IRType width, cider::TypeRef type) {
        cider::ir::Terminator term;
        term.term = cider::ir::Term::Ret;
        term.values.push_back(value(id, width, type));
        fn->blocks.back().term = term;
    }
};

void testAdjacentAllocaNeighbour() {
    std::printf("a GEP reaching a neighbour cell keeps both slots in memory\n");
    Ir ir;
    cider::TypeRef t = ir.i64();
    ir.emit(ir.allocaSlot(1, t));  // A
    ir.emit(ir.allocaSlot(2, t));  // B, adjacent cell
    ir.emit(ir.constant(3, 42, cider::ir::IRType::I64, t));
    ir.emit(ir.gep(4, 1, 8));  // %4 = A + 8, i.e. B's cell
    ir.emit(ir.store(4, ir.value(3, cider::ir::IRType::I64, t),
                     cider::ir::IRType::I64, t));
    ir.finishRet(3, cider::ir::IRType::I64, t);

    cider::opt::SlotFacts facts = cider::opt::analyzeMemory(*ir.fn);
    check(facts.nonEscaping.count(1) == 0, "slot A is not non-escaping");
    check(facts.nonEscaping.count(2) == 0, "neighbour slot B is not non-escaping");
    check(facts.gepDisplacement.find(1) != facts.gepDisplacement.end() &&
              facts.gepDisplacement.at(1) == 8,
          "A's constant GEP displacement is recorded");
    cider::opt::SroaResult result = cider::opt::sroa(ir.module);
    check(result.slotsScalarized == 0, "no slot is scalarized");
    check(countOp(*ir.fn, cider::ir::Op::Alloca) == 2,
          "both Allocas remain in the module");
    check(structurallyValid(ir.module), "the result is structurally valid");
}

void testEscapeViaCallAsmVolatile() {
    std::printf("a call, an asm block and a volatile access each force memory\n");
    Ir ir;
    cider::TypeRef t = ir.i32();

    // (a) &slot reaches a Call argument word.
    ir.emit(ir.allocaSlot(1, t));
    ir.emit(ir.constant(2, 7, cider::ir::IRType::I32, t));
    ir.emit(ir.callWithSlot(1, "sink"));
    ir.finishRet(2, cider::ir::IRType::I32, t);

    // (b) inline asm can name frame storage without an operand.
    ir.newFunction("with_asm");
    ir.emit(ir.allocaSlot(1, t));
    ir.emit(ir.constant(2, 8, cider::ir::IRType::I32, t));
    cider::ir::Instr asmInstr;
    asmInstr.op = cider::ir::Op::Asm;
    asmInstr.asmText = "nop";
    ir.emit(asmInstr);
    ir.finishRet(2, cider::ir::IRType::I32, t);

    // (c) a volatile Store of the slot.
    ir.newFunction("with_volatile");
    ir.emit(ir.allocaSlot(1, t));
    ir.emit(ir.constant(2, 9, cider::ir::IRType::I32, t));
    ir.emit(ir.store(1, ir.value(2, cider::ir::IRType::I32, t),
                     cider::ir::IRType::I32, t, 0, true));
    ir.finishRet(2, cider::ir::IRType::I32, t);

    cider::opt::SlotFacts byCall = cider::opt::analyzeMemory(ir.module.functions[0]);
    cider::opt::SlotFacts byAsm = cider::opt::analyzeMemory(ir.module.functions[1]);
    cider::opt::SlotFacts byVolatile = cider::opt::analyzeMemory(ir.module.functions[2]);
    check(byCall.nonEscaping.empty(), "the call argument escapes its slot");
    check(byVolatile.nonEscaping.empty(), "the volatile access escapes its slot");
    check(byAsm.nonEscaping.empty(), "asm empties the non-escaping set");
    check(byAsm.asmPresent, "the asm function reports assembly present");
    cider::opt::SroaResult result = cider::opt::sroa(ir.module);
    check(result.slotsScalarized == 0, "no slot is scalarized");
    check(countOp(ir.module, cider::ir::Op::Alloca) == 3,
          "all three Allocas remain");
    check(structurallyValid(ir.module), "the result is structurally valid");
}

void testScalarizeStructFields() {
    std::printf("three distinct field stores and loads are promoted to SSA\n");
    Ir ir;
    cider::TypeRef t = ir.i64();
    ir.emit(ir.constant(1, 11, cider::ir::IRType::I64, t));
    ir.emit(ir.constant(2, 22, cider::ir::IRType::I64, t));
    ir.emit(ir.constant(3, 33, cider::ir::IRType::I64, t));
    ir.emit(ir.allocaSlot(4, t));
    ir.emit(ir.store(4, ir.value(1, cider::ir::IRType::I64, t),
                     cider::ir::IRType::I64, t, 0));
    ir.emit(ir.store(4, ir.value(2, cider::ir::IRType::I64, t),
                     cider::ir::IRType::I64, t, 8));
    ir.emit(ir.store(4, ir.value(3, cider::ir::IRType::I64, t),
                     cider::ir::IRType::I64, t, 16));
    ir.emit(ir.load(5, 4, cider::ir::IRType::I64, t, 0));
    ir.emit(ir.load(6, 4, cider::ir::IRType::I64, t, 8));
    ir.emit(ir.load(7, 4, cider::ir::IRType::I64, t, 16));
    ir.emit(ir.binary(cider::ir::Op::Add, 8,
                      ir.value(5, cider::ir::IRType::I64, t),
                      ir.value(6, cider::ir::IRType::I64, t),
                      cider::ir::IRType::I64, t));
    ir.emit(ir.binary(cider::ir::Op::Add, 9,
                      ir.value(8, cider::ir::IRType::I64, t),
                      ir.value(7, cider::ir::IRType::I64, t),
                      cider::ir::IRType::I64, t));
    ir.finishRet(9, cider::ir::IRType::I64, t);

    cider::opt::SroaResult result = cider::opt::sroa(ir.module);
    check(result.slotsScalarized == 1, "one slot is scalarized");
    check(result.fieldsPromoted == 3, "three fields are promoted");
    check(result.loadsRemoved == 3 && result.storesRemoved == 3,
          "three loads and three stores are removed");
    check(countOp(*ir.fn, cider::ir::Op::Alloca) == 0 &&
              countOp(*ir.fn, cider::ir::Op::Load) == 0 &&
              countOp(*ir.fn, cider::ir::Op::Store) == 0,
          "no Alloca, Load or Store remains");
    const cider::ir::Instr* first = instrResult(*ir.fn, 8);
    const cider::ir::Instr* second = instrResult(*ir.fn, 9);
    check(first != nullptr && first->operands.size() == 2 &&
              first->operands[0].id == 1 && first->operands[1].id == 2,
          "the first Add uses the stored values directly");
    check(second != nullptr && second->operands.size() == 2 &&
              second->operands[0].id == 8 && second->operands[1].id == 3,
          "the second Add uses the third stored value directly");
    check(structurallyValid(ir.module), "the result is structurally valid");
}

void testWidthMismatchKeepsMemory() {
    std::printf("a store/load width mismatch keeps the field in memory\n");
    Ir ir;
    cider::TypeRef wide = ir.i64();
    cider::TypeRef narrow = ir.i32();
    ir.emit(ir.constant(1, 42, cider::ir::IRType::I64, wide));
    ir.emit(ir.allocaSlot(2, wide));
    ir.emit(ir.store(2, ir.value(1, cider::ir::IRType::I64, wide),
                     cider::ir::IRType::I64, wide, 0));
    ir.emit(ir.load(3, 2, cider::ir::IRType::I32, narrow, 0));
    ir.finishRet(3, cider::ir::IRType::I32, narrow);

    cider::opt::SlotFacts facts = cider::opt::analyzeMemory(*ir.fn);
    check(facts.nonEscaping.count(2) == 1,
          "the mismatched slot did not escape, it is merely ineligible");
    cider::opt::SroaResult result = cider::opt::sroa(ir.module);
    check(result.fieldsPromoted == 0 && result.slotsScalarized == 0,
          "the mismatched field is not promoted");
    check(countOp(*ir.fn, cider::ir::Op::Store) == 1 &&
              countOp(*ir.fn, cider::ir::Op::Load) == 1 &&
              countOp(*ir.fn, cider::ir::Op::Alloca) == 1,
          "the Store, Load and Alloca all remain");
    check(structurallyValid(ir.module), "the result is structurally valid");
}

void testGlobalStoreNeverRemoved() {
    std::printf("a global-labelled Store is never removed by SROA\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.emit(ir.constant(1, 5, cider::ir::IRType::I32, t));
    ir.emit(ir.store(0, ir.value(1, cider::ir::IRType::I32, t),
                     cider::ir::IRType::I32, t, 0, false, "global_counter"));
    ir.finishRet(1, cider::ir::IRType::I32, t);

    // A clean control slot proves the pass actually ran on this module while
    // the global Store above is left alone.
    ir.newFunction("control");
    ir.emit(ir.constant(1, 7, cider::ir::IRType::I32, t));
    ir.emit(ir.allocaSlot(2, t));
    ir.emit(ir.store(2, ir.value(1, cider::ir::IRType::I32, t),
                     cider::ir::IRType::I32, t, 0));
    ir.emit(ir.load(3, 2, cider::ir::IRType::I32, t, 0));
    ir.finishRet(3, cider::ir::IRType::I32, t);

    cider::opt::SroaResult result = cider::opt::sroa(ir.module);
    check(result.storesRemoved == 1,
          "only the control slot's Store is removed; the global Store is kept");
    check(result.slotsScalarized == 1,
          "the clean control slot is still scalarized");
    check(countOp(ir.module.functions[0], cider::ir::Op::Store) == 1,
          "the global-labelled Store remains");
    check(structurallyValid(ir.module), "the result is structurally valid");
}

}  // namespace

int main() {
    std::printf("sroa_test — escape/GEP-range facts and aggregate SROA\n\n");
    testAdjacentAllocaNeighbour();
    testEscapeViaCallAsmVolatile();
    testScalarizeStructFields();
    testWidthMismatchKeepsMemory();
    testGlobalStoreNeverRemoved();
    std::printf("\n%d checks, %s\n", checks,
                failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
