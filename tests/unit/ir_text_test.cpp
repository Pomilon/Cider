#include <cstdio>
#include <cstring>
#include <string>
#include "ir.h"
#include "ir_text.h"

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

int run() {
    cider::Program p = cider::parseIR(
        "func main:\n"
        "  %0 = const 1\n"
        "  %1 = str \"Hello, world!\\n\"\n"
        "  write %0, %1, 14\n"
        "  exit 0\n");
    CHECK(p.functions.size() == 1);
    CHECK(p.functions[0].name == "main");
    CHECK(p.functions[0].instrs.size() == 4);
    CHECK(p.strings.size() == 1 && p.strings[0] == "Hello, world!\n");

    const auto& i0 = p.functions[0].instrs[0];
    CHECK(i0.op == cider::Op::CONST && i0.dest == "%0" && i0.args[0].imm == 1);

    const auto& i1 = p.functions[0].instrs[1];
    CHECK(i1.op == cider::Op::STR && i1.dest == "%1" && i1.args[0].imm == 0);

    const auto& i2 = p.functions[0].instrs[2];
    CHECK(i2.op == cider::Op::WRITE && i2.args.size() == 3);
    CHECK(i2.args[0].isReg && i2.args[0].reg == "%0");
    CHECK(!i2.args[2].isReg && i2.args[2].imm == 14);

    const auto& i3 = p.functions[0].instrs[3];
    CHECK(i3.op == cider::Op::EXIT && !i3.args[0].isReg && i3.args[0].imm == 0);

    // Comment handling.
    cider::Program c = cider::parseIR(
        "func main:   ; entry\n"
        "  %0 = const 1 ; the fd\n"
        "  exit %0\n");
    CHECK(c.functions[0].instrs.size() == 2);
    CHECK(c.functions[0].instrs[1].args[0].isReg);

    // Missing operand -> ParseError with line number.
    bool threw = false;
    try {
        cider::parseIR("func main:\n  %0 = const\n");
    } catch (const cider::ParseError& e) {
        threw = true;
        CHECK(e.line == 2);
    }
    CHECK(threw);

    // Instruction outside a function.
    threw = false;
    try {
        cider::parseIR("  %0 = const 1\n");
    } catch (const cider::ParseError&) {
        threw = true;
    }
    CHECK(threw);
    // Round-trip: parse -> print -> parse -> print must be stable.
    const std::string src =
        "func main:\n"
        "  %0 = const 1\n"
        "  %1 = str \"Hello, world!\\n\"\n"
        "  write %0, %1, 14\n"
        "  exit 0\n";
    cider::Program a = cider::parseIR(src);
    std::string printed = cider::printIR(a);
    cider::Program b = cider::parseIR(printed);
    CHECK(cider::printIR(b) == printed);
    CHECK(printed.find("  %0 = const 1\n") != std::string::npos);
    CHECK(printed.find("  write %0, %1, 14\n") != std::string::npos);
    return 0;
}

int main() { return run(); }
