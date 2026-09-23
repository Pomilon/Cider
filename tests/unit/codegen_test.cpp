#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "codegen.h"
#include "ir_gen.h"
#include "lexer.h"
#include "module_resolver.h"
#include "parser.h"
#include "ssa_deconstructor.h"
#include "symbol_collector.h"
#include "symbols.h"

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)
#define CHECKSTR(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return ""; } } while (0)

std::string genAsm(const std::string& src) {
    cider::ErrorReporter rep;
    cider::Lexer lex(src, rep);
    cider::Parser parser(lex, rep);
    auto prog = parser.parseProgram();
    cider::SymbolTable table;
    cider::ModuleResolver resolver;
    cider::SymbolCollector collector(table, rep, resolver);
    collector.collect(*prog);
    cider::IRGenerator gen(table);
    auto module = gen.generate(*prog);
    cider::SSADeconstructor decon;
    decon.process(module);

    std::string path = "<tmp>/codegen_out.asm";
    cider::CodeGenerator cg;
    CHECKSTR(cg.generate(module, path));
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int run() {
    // Arithmetic + branch function.
    std::string asmText = genAsm(
        "func main() -> int {\n"
        "  var x: int = 6;\n"
        "  var y: int = 7;\n"
        "  if (x < y) { return x * y; }\n"
        "  return 0;\n"
        "}\n");
    CHECK(asmText.find("section .text") != std::string::npos);
    CHECK(asmText.find("section .data") != std::string::npos);
    CHECK(asmText.find("section .bss") != std::string::npos);
    CHECK(asmText.find("global main") != std::string::npos);
    CHECK(asmText.find("main:") != std::string::npos);
    CHECK(asmText.find("push rbp") != std::string::npos);
    CHECK(asmText.find("sub rsp") != std::string::npos);
    CHECK(asmText.find("imul") != std::string::npos);
    CHECK(asmText.find("jne") != std::string::npos);
    CHECK(asmText.find("pop rbp") != std::string::npos);
    CHECK(asmText.find("ret") != std::string::npos);

    // _start-named function gets a global declaration.
    std::string start = genAsm(
        "func _start() -> void {\n"
        "  asm { \"mov rax, 60\" }\n"
        "  asm { \"mov rdi, 0\" }\n"
        "  asm { \"syscall\" }\n"
        "}\n");
    CHECK(start.find("global _start") != std::string::npos);

    // Asm placeholder substitution ({code} etc).
    std::string asmProg = genAsm(
        "func main() -> void {\n"
        "  var code: int = 7;\n"
        "  asm { \"mov rax, 60\" }\n"
        "  asm { \"mov rdi, {code}\" }\n"
        "  asm { \"syscall\" }\n"
        "}\n");
    CHECK(asmProg.find("mov rdi, rbx") != std::string::npos ||
          asmProg.find("mov rdi,") != std::string::npos);

    // CID-029: float global initializers keep IEEE-754 bits (10.0 is
    // 0x41200000, not integer `dq 10`), and float->int casts lower to
    // cvttss2si (never a bare `mov dword [mem], xmm`).
    std::string fglob = genAsm(
        "var g_adv: float = 10.0;\n"
        "func main() -> int {\n"
        "  var scale: float = 4.0 * 0.5;\n"
        "  return (g_adv as float * scale) as int;\n"
        "}\n");
    CHECK(fglob.find("g_adv: dd 0x41200000") != std::string::npos);
    CHECK(fglob.find("cvttss2si") != std::string::npos);
    // No bare GP<->XMM mov: every xmm mention must be movss/movsd/movq/
    // cvt*/mulss/addss/etc or a valid xmm-to-xmm movss.
    {
        std::istringstream lines(fglob);
        std::string ln;
        while (std::getline(lines, ln)) {
            if (ln.find("xmm") == std::string::npos) continue;
            bool ok = (ln.find("movss") != std::string::npos ||
                       ln.find("movsd") != std::string::npos ||
                       ln.find("movq") != std::string::npos ||
                       ln.find("cvtt") != std::string::npos ||
                       ln.find("cvtsi") != std::string::npos ||
                       ln.find("mulss") != std::string::npos ||
                       ln.find("addss") != std::string::npos ||
                       ln.find("subss") != std::string::npos ||
                       ln.find("divss") != std::string::npos ||
                       ln.find("ucomiss") != std::string::npos ||
                       ln.find("xorps") != std::string::npos ||
                       ln.find("movaps") != std::string::npos);
            CHECK(ok);
        }
    }

    // CID-003: float width conversions. (CHECKSTR returns "" and is only
    // usable in string-returning helpers; run() uses CHECK/.find like the
    // rest of its assertions.) Sources are variables so both conversions
    // stay covered: literals in an f64 declaration fold to F64 constants
    // and no longer detour through cvtss2sd.
    {
        std::string asmText = genAsm(
            "func main() -> int { var s: float = 2.5; var a: f64 = s; var b: float = a; return 0; }");
        CHECK(asmText.find("cvtss2sd") != std::string::npos);   // f32 variable -> f64 widening
        CHECK(asmText.find("cvtsd2ss") != std::string::npos);   // f64 variable -> f32 narrowing
        CHECK(asmText.find("movsd") != std::string::npos);      // f64 destinations use 8-byte moves
    }

    // CID-003: float constants emit exact IEEE-754 bit patterns as hex,
    // never decimal text (std::to_string(double) cannot round-trip an f64
    // literal). The 0.3 pair also pins uppercase hex output.
    {
        std::string asmD = genAsm("func main() -> int { var d: f64 = 2.5; return 0; }");
        CHECK(asmD.find("dq 0x4004000000000000") != std::string::npos);  // double 2.5
        CHECK(asmD.find("2.500000") == std::string::npos);               // no decimal pool text
        std::string asmF = genAsm("func main() -> int { var f: float = 2.5; return 0; }");
        CHECK(asmF.find("dd 0x40200000") != std::string::npos);          // float 2.5
        std::string asmR = genAsm(
            "func main() -> int { var f: float = 0.3; var d: f64 = 0.3; return 0; }");
        CHECK(asmR.find("dd 0x3E99999A") != std::string::npos);          // float 0.3
        CHECK(asmR.find("dq 0x3FD3333333333333") != std::string::npos);  // double 0.3
    }

    // CID-003 follow-up (I2): integer->float conversion is width/sign
    // aware. The old path always read eax, truncating i64 sources and
    // treating u32/u64 as signed.
    {
        std::string asmText = genAsm(
            "func main() -> int {\n"
            "  var big: i64 = 5000000000;\n"
            "  var d: f64 = big;\n"
            "  var u: u32 = 3000000000;\n"
            "  var e: f64 = u;\n"
            "  var q: u64 = 5000000000;\n"
            "  var g: f64 = q;\n"
            "  var s: i8 = 0 - 5;\n"
            "  var h: f64 = s;\n"
            "  return 0;\n"
            "}");
        CHECK(asmText.find("cvtsi2sd xmm0, rax") != std::string::npos);   // i64/u32 from rax
        CHECK(asmText.find("cvtsi2sd xmm0, eax") == std::string::npos);  // never truncate
        CHECK(asmText.find("movsx rax, ") != std::string::npos);         // i8 sign-extends
        // u64 converts unsigned: the >= 2^63 path doubles the shifted value.
        CHECK(asmText.find("addsd xmm0, xmm0") != std::string::npos);
    }

    // CID-003 follow-up (I3): an integer RHS in a float compound
    // assignment converts before the arithmetic (`addsd xmm0, 1` does not
    // assemble).
    {
        std::string asmText = genAsm(
            "func main() -> int {\n"
            "  var d: f64 = 1.0;\n"
            "  d += 1;\n"
            "  d *= 2;\n"
            "  var f: float = 1.5;\n"
            "  f += 1;\n"
            "  f *= 2;\n"
            "  return 0;\n"
            "}");
        CHECK(asmText.find("addsd xmm0, 1") == std::string::npos);
        CHECK(asmText.find("mulsd xmm0, 2") == std::string::npos);
        CHECK(asmText.find("addss xmm0, 1") == std::string::npos);
        CHECK(asmText.find("cvtsi2sd") != std::string::npos);
        CHECK(asmText.find("cvtsi2ss") != std::string::npos);
    }

    // CID-003 follow-up (I4): ternary merges move float values through GP
    // registers (`push xmm`/`pop xmm` are not instructions) and convert
    // mixed arm widths to the common float width.
    {
        std::string asmText = genAsm(
            "func main() -> int {\n"
            "  var c: bool = true;\n"
            "  var a32: float = 1.25;\n"
            "  var b32: float = 3.75;\n"
            "  var f: float = c ? a32 : b32;\n"
            "  var d: f64 = 2.5;\n"
            "  var e: f64 = c ? d : a32;\n"
            "  var g: f64 = c ? a32 : d;\n"
            "  var h: float = c ? d : a32;\n"
            "  return 0;\n"
            "}");
        CHECK(asmText.find("push xmm") == std::string::npos);
        CHECK(asmText.find("pop xmm") == std::string::npos);
        CHECK(asmText.find("movq rax, xmm") != std::string::npos);       // float push
        CHECK(asmText.find("movq xmm") != std::string::npos);            // float pop
        CHECK(asmText.find("cvtss2sd") != std::string::npos);            // f32 arm -> f64 merge
        CHECK(asmText.find("cvtsd2ss") != std::string::npos);            // f64 arm -> f32 merge
    }
    return 0;
}

int main() { return run(); }
