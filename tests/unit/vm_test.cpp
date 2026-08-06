#include <cstdio>
#include <string>
#include "ir.h"
#include "ir_text.h"
#include "vm.h"

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

int run() {
    cider::Program p = cider::parseIR(
        "func main:\n"
        "  %0 = const 1\n"
        "  %1 = str \"Hello, world!\\n\"\n"
        "  write %0, %1, 14\n"
        "  exit 0\n");
    cider::VMResult r = cider::runVM(p);
    CHECK(r.exitCode == 0);
    CHECK(r.stdoutText == "Hello, world!\n");
    CHECK(r.stderrText.empty());

    // Exit code path with an immediate.
    cider::Program q = cider::parseIR("func main:\n  exit 7\n");
    CHECK(cider::runVM(q).exitCode == 7);

    // Exit code from a register.
    cider::Program q2 = cider::parseIR("func main:\n  %0 = const 9\n  exit %0\n");
    CHECK(cider::runVM(q2).exitCode == 9);

    // Bad fd -> VMError.
    cider::Program b = cider::parseIR(
        "func main:\n"
        "  %0 = const 9\n"
        "  %1 = str \"x\"\n"
        "  write %0, %1, 1\n"
        "  exit 0\n");
    bool threw = false;
    try { cider::runVM(b); } catch (const cider::VMError&) { threw = true; }
    CHECK(threw);

    // Missing main -> VMError.
    threw = false;
    try { cider::runVM(cider::parseIR("func other:\n  exit 0\n")); }
    catch (const cider::VMError&) { threw = true; }
    CHECK(threw);
    return 0;
}

int main() { return run(); }
