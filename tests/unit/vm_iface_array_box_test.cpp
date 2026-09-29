// CIDER-BUG-202 regression: the heap boxes behind the interface ELEMENTS of
// a returned fixed array are caller-owned temporaries, released — after the
// boxed values' destructors — at the exit of the scope enclosing the call.
//
// This is the array-element twin of CIDER-BUG-184's whole-value box
// (vm_iface_box_test.cpp): the callee copies each element into a box with
// the same two-word header (release thunk + ownership token, token 0 = the
// reserved "transferred" marker), and the call site registers a claim and a
// release per ELEMENT slot. Pre-fix the sret copy moved the fat pointers
// verbatim, so every call leaked the element's frame address' worth of
// nothing at all (the element pointed into the dead callee frame) and
// dispatch read a zeroed slot — so there was no heap growth to measure; the
// leak this pins is the one the fix could have introduced, i.e. a box per
// element per call that nobody frees.
//
// The metric is the post-run LIVE set (heapBytes() - freeListBytes()): the
// permanent allocations (globals, vtables, strings) plus anything the run
// failed to free. A leak-free run's live set is its permanent allocations
// whatever the iteration count, so one iteration and many must agree within
// the VM first-fit splitter's slack (it does not coalesce, so chunk
// splitting can extend the heap by a whole frame chunk). kAllocatorSlack
// covers that absorbed remainder and is far below one leaked box.
//
// Four shapes are pinned:
//   * one element, a class WITH a destructor: the release frees the box and
//     runs the finalizer exactly once per call (the exit code checks the
//     count: the callee's own local and the boxed copy, so 2 per call);
//   * two elements of DIFFERENT concrete types: the per-element box picks
//     each element's own size and thunk from its own vtable, so one leaked
//     box or one wrong thunk fails here;
//   * a destructor-free class: the release rides the ownership rule, not
//     the finalizer walk finding something to destroy;
//   * a pass-through array return (`pass(f())`): one box, two caller
//     temporaries, released exactly once by the last holder — a double
//     release corrupts the allocator and fails the run.

#include <cstdio>
#include <string>

#include "ir_gen.h"
#include "lexer.h"
#include "module_resolver.h"
#include "parser.h"
#include "symbol_collector.h"
#include "symbols.h"
#include "vm.h"

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

// Mirrors tests/unit/vm_iface_box_test.cpp's slack: the VM's first-fit
// splitter absorbs a remainder of kHeaderSize (16) bytes or less into the
// allocation, so a completed free can leave at most that many bytes
// unreturned per heap extension. 1 KiB is far above the drift of a
// leak-free run and far below one leaked box (16-byte header + the object).
static constexpr size_t kAllocatorSlack = 1024;

// Mirrors tests/unit/ir_gen_test.cpp's genModule plus the driver's
// interface-boxing flag.
cider::ModuleIR genModule(const std::string& src, bool ifaceBoxing) {
    cider::ErrorReporter rep;
    cider::Lexer lex(src, rep);
    cider::Parser parser(lex, rep);
    auto prog = parser.parseProgram();
    cider::SymbolTable table;
    cider::ModuleResolver resolver;
    cider::SymbolCollector collector(table, rep, resolver);
    collector.collect(*prog);
    cider::IRGenerator gen(table);
    // This test skips the type checker (every file here is checker-valid:
    // returning a `[I; N]` for an interface-element array is exactly the
    // shape TypeChecker::hasInterfaceBoxing now reports), so the flag is
    // passed directly, like vm_iface_box_test does.
    gen.setInterfaceBoxing(ifaceBoxing);
    cider::ModuleIR module = gen.generate(*prog);
    if (rep.hasErrors()) {
        std::fprintf(stderr, "gen errors: %s\n", rep.errors()[0].message.c_str());
    }
    return module;
}

// `drops` counts every finalizer: each element contributes the callee's own
// local implementor plus the boxed copy, so `2 * width` per call with
// destructors. `width` is the returned array's element count; `withDtor`
// picks finalizer-bearing classes.
//
// `E` is a second implementor of the same interface with a different field
// layout, so the per-element box has to take each element's size and thunk
// from its OWN vtable rather than from one shared shape. Even elements hold
// D and odd ones hold E, so a two-element array carries two different
// concrete types behind the same interface.
std::string arrayLoopProgram(int iterations, int width, bool withDtor) {
    std::string dtorD = withDtor ? "  func ~D() { drops = drops + 1; }\n" : "";
    std::string dtorE = withDtor ? "  func ~E() { drops = drops + 1; }\n" : "";
    std::string dropsDecl = withDtor ? "var drops: int = 0;\n" : "";
    std::string expect = withDtor
        ? "    if (drops != " + std::to_string(iterations * 2 * width) + ") { return 2; }\n"
        : "";
    // Even elements hold D and odd ones hold E, so a two-element array
    // carries two different concrete types behind the same interface.
    std::string body;
    std::string checks;
    for (int i = 0; i < width; ++i) {
        char name = (i % 2 == 0) ? 'd' : 'e';
        body += std::string("    var ") + name + std::to_string(i) + ": " +
                (i % 2 == 0 ? "D" : "E") + "; " + name + std::to_string(i) + ".v = " +
                std::to_string(i) + ";\n";
        body += "    a[" + std::to_string(i) + "] = " + name + std::to_string(i) + ";\n";
        checks += "    if (a[" + std::to_string(i) + "].label() != " + std::to_string(i) +
                  ") { return 1; }\n";
    }
    return dropsDecl +
        "interface I {\n"
        "  func label() -> int;\n"
        "}\n"
        "class D {\n"
        "  var v: int;\n"
        "  var pad: [u64; 64];\n" + dtorD +
        "  func label() -> int { return this.v; }\n"
        "}\n"
        "class E {\n"
        "  var v: int;\n"
        "  var tag: int;\n"
        "  var pad: [u64; 64];\n" + dtorE +
        "  func label() -> int { return this.v; }\n"
        "}\n"
        "func fill() -> [I; " + std::to_string(width) + "] {\n"
        "  var a: [I; " + std::to_string(width) + "];\n" + body +
        "  return a;\n"
        "}\n"
        "func main() -> int {\n"
        "  var i: int = 0;\n"
        "  while (i < " + std::to_string(iterations) + ") {\n"
        "    var a: [I; " + std::to_string(width) + "] = fill();\n" + checks +
        "    i = i + 1;\n"
        "  }\n" + expect +
        "  return 0;\n"
        "}\n";
}

// A pass-through: one set of boxes, two caller temporaries (the call's own
// sret slot and `a`'s). The last claimant releases, so each box is freed
// exactly once; a double release corrupts the allocator and fails the run.
std::string passProgram(int iterations) {
    return "var drops: int = 0;\n"
        "interface I {\n"
        "  func label() -> int;\n"
        "}\n"
        "class D {\n"
        "  var v: int;\n"
        "  var pad: [u64; 64];\n"
        "  func ~D() { drops = drops + 1; }\n"
        "  func label() -> int { return this.v; }\n"
        "}\n"
        "func fill() -> [I; 2] {\n"
        "  var a: [I; 2];\n"
        "  var k: int = 0;\n"
        "  while (k < 2) {\n"
        "    var d: D; d.v = k;\n"
        "    a[k] = d;\n"
        "    k = k + 1;\n"
        "  }\n"
        "  return a;\n"
        "}\n"
        "func pass(x: [I; 2]) -> [I; 2] { return x; }\n"
        "func main() -> int {\n"
        "  var i: int = 0;\n"
        "  while (i < " + std::to_string(iterations) + ") {\n"
        "    var a: [I; 2] = pass(fill());\n"
        "    if (a[0].label() != 0 || a[1].label() != 1) { return 1; }\n"
        "    i = i + 1;\n"
        "  }\n"
        "  if (drops != " + std::to_string(iterations * 4) + ") { return 2; }\n"
        "  return 0;\n"
        "}\n";
}

// Runs `src` in a fresh VM and returns the bytes still live after the run
// (heap minus free list).
size_t runLiveBytes(const std::string& src, int expectedExit) {
    cider::VM vm;
    cider::ModuleIR module = genModule(src, true);
    cider::VMResult r = vm.run(module);
    if (r.exitCode != expectedExit) {
        std::fprintf(stderr, "unexpected exit %d (want %d)\n", r.exitCode, expectedExit);
        return (size_t)-1;
    }
    return vm.heapBytes() - vm.freeListBytes();
}

// One-sided leak bound: allocator slack can only make the assertion
// stricter than intended, never hide a leak bigger than kAllocatorSlack.
bool bounded(size_t one, size_t many) {
    if (one == (size_t)-1 || many == (size_t)-1) return false;
    if (many > one + kAllocatorSlack) {
        std::fprintf(stderr, "live bytes: one=%zu many=%zu (growth %zu > slack %zu)\n",
                     one, many, many - one, kAllocatorSlack);
        return false;
    }
    return true;
}

int run() {
    // One element, destructor-bearing: the box is freed and the finalizer
    // runs exactly once per call (the exit code checks drops == 2 * iters).
    {
        size_t one = runLiveBytes(arrayLoopProgram(1, 1, true), 0);
        size_t many = runLiveBytes(arrayLoopProgram(200, 1, true), 0);
        CHECK(one != (size_t)-1 && many != (size_t)-1);
        CHECK(bounded(one, many));
    }

    // Two elements: one box per element, both released. A release that
    // freed only the first would grow the live set by one box per call and
    // fail the bound below.
    {
        size_t one = runLiveBytes(arrayLoopProgram(1, 2, true), 0);
        size_t many = runLiveBytes(arrayLoopProgram(200, 2, true), 0);
        CHECK(one != (size_t)-1 && many != (size_t)-1);
        CHECK(bounded(one, many));
    }

    // Destructor-free: the release rides the ownership rule, not the
    // finalizer walk finding something to destroy.
    {
        size_t one = runLiveBytes(arrayLoopProgram(1, 2, false), 0);
        size_t many = runLiveBytes(arrayLoopProgram(200, 2, false), 0);
        CHECK(one != (size_t)-1 && many != (size_t)-1);
        CHECK(bounded(one, many));
    }

    // Pass-through: one box per element, two caller temporaries, released
    // exactly once by the last holder.
    {
        size_t one = runLiveBytes(passProgram(1), 0);
        size_t many = runLiveBytes(passProgram(200), 0);
        CHECK(one != (size_t)-1 && many != (size_t)-1);
        CHECK(bounded(one, many));
    }

    std::printf("vm_iface_array_box_test: ok\n");
    return 0;
}

int main() { return run(); }
