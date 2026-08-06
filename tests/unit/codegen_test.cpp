#include <cstdio>
#include <string>
#include "ir.h"
#include "ir_text.h"
#include "codegen.h"

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

int run() {
    cider::Program p = cider::parseIR(
        "func main:\n"
        "  %0 = const 1\n"
        "  %1 = str \"Hello, world!\\n\"\n"
        "  write %0, %1, 14\n"
        "  exit 0\n");
    std::string asmText = cider::generateAsm(p);

    CHECK(asmText.find("section .data") != std::string::npos);
    CHECK(asmText.find("section .text") != std::string::npos);
    CHECK(asmText.find("global _start") != std::string::npos);
    CHECK(asmText.find("_start:") != std::string::npos);
    CHECK(asmText.find("mov rax, 1") != std::string::npos);          // write syscall number
    CHECK(asmText.find("syscall") != std::string::npos);
    CHECK(asmText.find("mov rax, 60") != std::string::npos);          // exit syscall number

    // The literal must appear in .data.
    CHECK(asmText.find("'H', 'e', 'l', 'l', 'o'") != std::string::npos);

    // No main -> CodegenError.
    bool threw = false;
    try { cider::generateAsm(cider::parseIR("func other:\n  exit 0\n")); }
    catch (const cider::CodegenError&) { threw = true; }
    CHECK(threw);
    return 0;
}

int main() { return run(); }
