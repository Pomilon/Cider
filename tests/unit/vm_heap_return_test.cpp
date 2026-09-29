// CIDER-BUG-156 regression: the malloc buffer backing a non-sret virtual
// aggregate return is a caller-owned copy. CIDER-BUG-137 already ran the
// copy's destructors at the caller's enclosing-scope exit; the buffer
// itself must be released right after them, or every call leaks a block
// (pre-fix every case below grew by one block per extra loop iteration).
//
// The accounting runs on the unoptimized IR: a heap copy whose allocation
// escaped through the callee's RET stays on the heap, so every completed
// call must put its block back on the VM's free list. The metric is the
// post-run LIVE set (heapBytes() - freeListBytes(): globals, vtables,
// strings and anything the program failed to free), because heapBytes()
// alone is allocator-dependent — the VM's first-fit free list does not
// coalesce, so chunk splitting can extend the heap by a whole frame
// chunk. The live set of a leak-free run is exactly its permanent
// allocations whatever the iteration count. The VM splitter also absorbs
// remainders up to its 16-byte header, a few bytes per heap extension, so
// the assertion allows kAllocatorSlack; one leaked block is ~528 bytes
// (the classes below carry a 512-byte payload), orders of magnitude above
// that slack.
//
// Both shapes are pinned: a class with a destructor (the CIDER-BUG-137
// path), a destructor-free class (the release must not be gated on the
// finalizer walk finding something) and a virtual method returning
// another non-sret virtual call's result (inner and outer buffers are
// released at different scopes).

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
// far below one leaked block and far above the accumulated drift.
static constexpr size_t kAllocatorSlack = 1024;

// Mirrors tests/unit/ir_gen_test.cpp's genModuleSrc.
cider::ModuleIR genModule(const std::string& src) {
    cider::ErrorReporter rep;
    cider::Lexer lex(src, rep);
    cider::Parser parser(lex, rep);
    auto prog = parser.parseProgram();
    cider::SymbolTable table;
    cider::ModuleResolver resolver;
    cider::SymbolCollector collector(table, rep, resolver);
    collector.collect(*prog);
    cider::IRGenerator gen(table);
    cider::ModuleIR module = gen.generate(*prog);
    if (rep.hasErrors()) {
        std::fprintf(stderr, "gen errors: %s\n", rep.errors()[0].message.c_str());
    }
    return module;
}

// A virtual method returning a class by value rides the legacy malloc
// path (VIRTUAL_CALL carries no sret slot). The loop body pins all three
// caller-owned forms the CIDER-BUG-137 rule covers: assignment into an
// outer local, initialization of a per-iteration local, and a bare
// discarded temporary. `drops` counts every finalizer (the callee's own
// returned local, the caller's heap-copy temporary, and the declared
// locals), so a missing destructor or a duplicated free-time destroy is
// caught through the exit code.
std::string loopProgram(int iterations, bool withDtor) {
    std::string dtor = withDtor ? "  func ~D() -> void { drops = drops + 1; }\n" : "";
    std::string dropsDecl = withDtor ? "var drops: int = 0;\n" : "";
    std::string expect = withDtor
        ? "    if (drops != " + std::to_string(iterations * 7) + ") { return 2; }\n"
        : "";
    return dropsDecl +
        "class D {\n"
        "  var label: int;\n"
        "  var pad: [u64; 64];\n" + dtor +
        "}\n"
        "class Base {\n"
        "  virtual func make(n: int) -> D {\n"
        "    var d: D;\n"
        "    d.label = n;\n"
        "    return d;\n"
        "  }\n"
        "}\n"
        "class Derived extends Base {\n"
        "  override func make(n: int) -> D {\n"
        "    var d: D;\n"
        "    d.label = n + 1;\n"
        "    return d;\n"
        "  }\n"
        "}\n"
        "func main() -> int {\n"
        "  var b: Derived;\n"
        "  var r: D;\n"
        "  var i: int = 0;\n"
        "  while (i < " + std::to_string(iterations) + ") {\n"
        "    r = b.make(i);\n"
        "    if (r.label != i + 1) { return 1; }\n"
        "    var q: D = b.make(i);\n"
        "    if (q.label != i + 1) { return 1; }\n"
        "    b.make(i);\n"
        "    i = i + 1;\n"
        "  }\n" + expect +
        "  return 0;\n"
        "}\n";
}

// A nested shape: a virtual method returns the result of another non-sret
// virtual aggregate call. The inner buffer is released inside the outer
// method (right after the value is copied into the outer buffer); the
// caller releases the outer one. A double release or a release before the
// copy would corrupt the allocator and fail the run.
std::string nestedProgram(int iterations) {
    return
        "class D {\n"
        "  var label: int;\n"
        "  var pad: [u64; 64];\n"
        "}\n"
        "class Base {\n"
        "  virtual func make(n: int) -> D {\n"
        "    var d: D;\n"
        "    d.label = n;\n"
        "    return d;\n"
        "  }\n"
        "  virtual func remake(n: int) -> D {\n"
        "    return this.make(n);\n"
        "  }\n"
        "}\n"
        "class Derived extends Base {\n"
        "  override func make(n: int) -> D {\n"
        "    var d: D;\n"
        "    d.label = n + 10;\n"
        "    return d;\n"
        "  }\n"
        "  override func remake(n: int) -> D {\n"
        "    return this.make(n + 100);\n"
        "  }\n"
        "}\n"
        "func main() -> int {\n"
        "  var b: Derived;\n"
        "  var i: int = 0;\n"
        "  while (i < " + std::to_string(iterations) + ") {\n"
        "    var r: D = b.remake(i);\n"
        "    if (r.label != i + 110) { return 1; }\n"
        "    i = i + 1;\n"
        "  }\n"
        "  return 0;\n"
        "}\n";
}

// Runs `src` in a fresh VM and returns the bytes still live after the run
// (heap minus free list).
size_t runLiveBytes(const std::string& src, int expectedExit) {
    cider::VM vm;
    cider::ModuleIR module = genModule(src);
    cider::VMResult r = vm.run(module);
    if (r.exitCode != expectedExit) {
        std::fprintf(stderr, "unexpected exit %d (want %d)\n", r.exitCode, expectedExit);
        return (size_t)-1;
    }
    return vm.heapBytes() - vm.freeListBytes();
}

// Pre-fix every extra iteration kept one block per call live; the
// assertion is one-sided, so allocator slack can only make it stricter
// than intended, never hide a leak bigger than kAllocatorSlack.
bool bounded(size_t one, size_t many) {
    if (one == (size_t)-1 || many == (size_t)-1) return false;
    return many <= one + kAllocatorSlack;
}

int run() {
    // Destructible element: 200 iterations must not keep the three heap
    // copies of each iteration live.
    {
        size_t one = runLiveBytes(loopProgram(1, true), 0);
        size_t many = runLiveBytes(loopProgram(200, true), 0);
        CHECK(bounded(one, many));
    }

    // Destructor-free element: the release rides the ownership rule, not
    // the finalizer walk.
    {
        size_t one = runLiveBytes(loopProgram(1, false), 0);
        size_t many = runLiveBytes(loopProgram(256, false), 0);
        CHECK(bounded(one, many));
    }

    // Nested non-sret virtual returns: inner buffer released inside the
    // outer method, outer one at the caller's scope exit.
    {
        size_t one = runLiveBytes(nestedProgram(1), 0);
        size_t many = runLiveBytes(nestedProgram(1000), 0);
        CHECK(bounded(one, many));
    }

    std::printf("vm_heap_return_test: ok\n");
    return 0;
}

int main() { return run(); }
