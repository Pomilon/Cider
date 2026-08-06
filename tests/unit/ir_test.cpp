#include <cstdio>
#include <cstring>
#include "ir.h"

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

int run() {
    CHECK(std::strcmp(cider::opName(cider::Op::CONST), "const") == 0);
    CHECK(std::strcmp(cider::opName(cider::Op::STR), "str") == 0);
    CHECK(std::strcmp(cider::opName(cider::Op::WRITE), "write") == 0);
    CHECK(std::strcmp(cider::opName(cider::Op::EXIT), "exit") == 0);

    cider::Operand reg;
    reg.isReg = true;
    reg.reg = "%1";
    cider::Operand imm;
    imm.isReg = false;
    imm.imm = 14;
    CHECK(reg.isReg && !imm.isReg && imm.imm == 14);

    cider::Instruction instr;
    instr.op = cider::Op::EXIT;
    instr.args.push_back(imm);
    CHECK(instr.args.size() == 1 && instr.args[0].imm == 14);

    cider::Program prog;
    cider::Function fn;
    fn.name = "main";
    fn.instrs.push_back(instr);
    prog.functions.push_back(fn);
    CHECK(prog.functions.size() == 1 && prog.functions[0].name == "main");
    return 0;
}

int main() { return run(); }
