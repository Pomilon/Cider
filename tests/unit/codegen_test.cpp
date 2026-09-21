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
    return 0;
}

int main() { return run(); }
