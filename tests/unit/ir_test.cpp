#include <cstdio>
#include <memory>
#include <string>
#include "ir.h"

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

int run() {
    // IRValue forms.
    auto r = cider::IRValue::Register("0", cider::IRType::I32);
    CHECK(r.toString() == "%0" && r.type == cider::IRType::I32);
    auto c = cider::IRValue::Constant(5, cider::IRType::I32);
    CHECK(c.toString() == "5" && c.constValue == 5 && c.intValue == 5);
    auto l = cider::IRValue::Label("entry");
    CHECK(l.toString() == "entry" && l.type == cider::IRType::VOID);

    // Instruction toString.
    cider::IRInstruction add;
    add.op = cider::IROp::ADD;
    add.dest = cider::IRValue::Register("1", cider::IRType::I32);
    add.args = {cider::IRValue::Constant(1, cider::IRType::I32), r};
    CHECK(add.toString() == "%1 = add 1, %0");

    // PHI with phiArgs.
    cider::IRInstruction phi;
    phi.op = cider::IROp::PHI;
    phi.dest = cider::IRValue::Register("2", cider::IRType::I32);
    phi.phiArgs["then"] = cider::IRValue::Constant(1, cider::IRType::I32);
    phi.phiArgs["else"] = cider::IRValue::Constant(2, cider::IRType::I32);
    CHECK(phi.toString() == "%2 = phi [2, else], [1, then]");

    // ASM with asmMap.
    cider::IRInstruction asmInstr;
    asmInstr.op = cider::IROp::ASM;
    asmInstr.args = {cider::IRValue::Constant(1, cider::IRType::I32)};
    asmInstr.asmMap["{code}"] = cider::IRValue::Register("3", cider::IRType::I32);
    CHECK(asmInstr.toString() == "asm 1");

    // Blocks + function assembly.
    auto block = std::make_unique<cider::BasicBlock>("entry");
    block->addInstruction(add);
    auto fn = std::make_unique<cider::FunctionIR>("main", cider::IRType::VOID);
    fn->addParam("a");
    fn->addBlock(std::move(block));
    CHECK(fn->getBlocks().size() == 1);
    CHECK(fn->getBlocks()[0]->getName() == "entry");
    CHECK(fn->getParams().size() == 1 && fn->getParams()[0] == "a");
    CHECK(fn->getReturnType() == cider::IRType::VOID);

    // ModuleIR adders.
    cider::ModuleIR module;
    module.addFunction(std::move(fn));
    module.addGlobalVar("g", cider::IRType::I32, 5);
    module.addGlobalVar("arr", cider::IRType::I32, 0, 100);
    module.addStringData("str0: db 'hi', 0");
    module.addVTable("A", {"A_m"});
    module.addExternFunction("puts");
    module.addGlobalAsm("global _start");
    CHECK(module.getFunctions().size() == 1);
    CHECK(module.getGlobalVars().size() == 2);
    CHECK(module.getGlobalVars()[1].arraySize == 100);
    CHECK(module.getVTables().at("A")[0] == "A_m");
    CHECK(module.getExternFunctions()[0] == "puts");
    CHECK(module.getGlobalAsm()[0] == "global _start");
    return 0;
}

int main() { return run(); }
