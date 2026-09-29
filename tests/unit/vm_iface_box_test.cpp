// CIDER-BUG-184 regression: the heap box behind an interface-typed return
// of a concrete VALUE is a caller-owned temporary. The callee copies the
// value into a fresh block (its frame address dies on return), the caller's
// interface-result slot holds the box through a tag bit, and the box is
// released — after the boxed value's destructors — at the exit of the
// scope enclosing the call, exactly like the CIDER-BUG-137/156
// caller-owned return temporaries. Pre-fix nobody freed the block (and the
// embedded destructors never ran): every call leaked one box (~536 bytes
// for the class below), measured as ~77 bytes/iteration average in the
// original report.
//
// The accounting runs on the unoptimized IR, mirroring
// tests/unit/vm_heap_return_test.cpp: the metric is the post-run LIVE set
// (heapBytes() - freeListBytes(): globals, vtables, strings and anything
// the program failed to free), because heapBytes() alone is
// allocator-dependent — the VM's first-fit free list does not coalesce,
// so chunk splitting can extend the heap by a whole frame chunk. The live
// set of a leak-free run is exactly its permanent allocations whatever the
// iteration count; kAllocatorSlack covers the splitter's absorbed
// remainders, and is orders of magnitude below one leaked box.
//
// Three shapes are pinned:
//   * a class WITH a destructor: the release must free the box and run the
//     destructor exactly once per call (wrong drops fail the run);
//   * a destructor-free class: the release rides the ownership rule, not
//     the finalizer walk finding something;
//   * a chain of interface returns (`id(make(i))`): one box, two caller
//     temporaries, released exactly once by the last holder.
//
// The legacy path (setInterfaceBoxing(false), the pre-fix representation)
// is run as a negative control on the destructor-free program: it must
// still grow, so the accounting above is proven sensitive to this leak.

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

// VM first-fit splitter: a remainder of kHeaderSize (16) bytes or less is
// absorbed by the allocation (vm.cpp VM::malloc), so a completed free can
// leave at most that many bytes unreturned per heap extension. 1 KiB is
// far below one leaked box (16 header + 520 object bytes) and far above
// the accumulated drift of a leak-free run.
static constexpr size_t kAllocatorSlack = 1024;

// Mirrors tests/unit/ir_gen_test.cpp's genModule, plus the driver's
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
    // CIDER-BUG-184: mirrors main.cpp's `gen.setInterfaceBoxing(
    // checker.hasInterfaceBoxing())`. This test skips the type checker
    // (the file is checker-valid: `return d;` for the interface-typed
    // return is exactly the shape TypeChecker::checkInterfaceValue flags),
    // so the flag is passed directly. False keeps the pre-fix legacy
    // representation for the leak-reproduction control.
    gen.setInterfaceBoxing(ifaceBoxing);
    cider::ModuleIR module = gen.generate(*prog);
    if (rep.hasErrors()) {
        std::fprintf(stderr, "gen errors: %s\n", rep.errors()[0].message.c_str());
    }
    return module;
}

// The boxed-value loop: the call site sits in the while body, so each
// iteration's box is released at the iteration scope exit — after the
// dispatch on it. `chain` adds a second caller temporary (id's sret slot)
// over the same box, which must be released exactly once.
// `drops` counts every finalizer: the callee's own returned local and the
// boxed copy (2 per call with a destructor), so a missing or duplicated
// release is caught through the exit code.
std::string loopProgram(int iterations, bool withDtor, bool chain) {
    std::string dtor = withDtor ? "  func ~D() -> void { drops = drops + 1; }\n" : "";
    std::string dropsDecl = withDtor ? "var drops: int = 0;\n" : "";
    std::string expect = withDtor
        ? "    if (drops != " + std::to_string(iterations * 2) + ") { return 2; }\n"
        : "";
    std::string make = chain ? "id(make(i))" : "make(i)";
    return dropsDecl +
        "interface I {\n"
        "  func label() -> int;\n"
        "}\n"
        "class D {\n"
        "  var v: int;\n"
        "  var pad: [u64; 64];\n" + dtor +
        "  func label() -> int { return this.v; }\n"
        "}\n"
        "func make(n: int) -> I {\n"
        "  var d: D;\n"
        "  d.v = n;\n"
        "  return d;\n"
        "}\n"
        "func id(x: I) -> I { return x; }\n"
        "func main() -> int {\n"
        "  var i: int = 0;\n"
        "  while (i < " + std::to_string(iterations) + ") {\n"
        "    var s: I = " + make + ";\n"
        "    if (s.label() != i) { return 1; }\n"
        "    i = i + 1;\n"
        "  }\n" + expect +
        "  return 0;\n"
        "}\n";
}

// Runs `src` in a fresh VM with the given boxing representation and
// returns the bytes still live after the run (heap minus free list).
size_t runLiveBytes(const std::string& src, int expectedExit, bool ifaceBoxing) {
    cider::VM vm;
    cider::ModuleIR module = genModule(src, ifaceBoxing);
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
    // CIDER-BUG-184 negative control: the pre-fix legacy representation
    // (no header, no release) must still leak one box per call, proving
    // the accounting detects this exact leak.
    {
        size_t one = runLiveBytes(loopProgram(1, false, false), 0, false);
        size_t many = runLiveBytes(loopProgram(200, false, false), 0, false);
        CHECK(one != (size_t)-1 && many != (size_t)-1);
        CHECK(many > one + kAllocatorSlack);
    }

    // The fixed caller-owned release: destructor-free box, 200 iterations
    // must not keep any block live.
    {
        size_t one = runLiveBytes(loopProgram(1, false, false), 0, true);
        size_t many = runLiveBytes(loopProgram(200, false, false), 0, true);
        CHECK(bounded(one, many));
    }

    // Destructible box: the release also runs the boxed value's finalizer
    // exactly once per call (drops == 2 * iterations pins local + box), and
    // still frees the block every iteration.
    {
        size_t one = runLiveBytes(loopProgram(1, true, false), 0, true);
        size_t many = runLiveBytes(loopProgram(200, true, false), 0, true);
        CHECK(bounded(one, many));
    }

    // Chained interface returns: one box through two caller temporaries,
    // released exactly once (a double release corrupts the allocator and
    // fails the run; a zero release shows up in the live set). 200
    // iterations keep the VM splitter's unreclaimed remainders (see
    // kAllocatorSlack) far below one leaked box.
    {
        size_t one = runLiveBytes(loopProgram(1, true, true), 0, true);
        size_t many = runLiveBytes(loopProgram(200, true, true), 0, true);
        CHECK(bounded(one, many));
    }

    std::printf("vm_iface_box_test: ok\n");
    return 0;
}

int main() { return run(); }
