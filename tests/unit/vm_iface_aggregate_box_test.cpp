// CIDER-BUG-212 regression: the heap boxes behind the interface values a
// returned AGGREGATE embeds (a bare interface field, an interface array
// field, or either nested deeper) are caller-owned temporaries, released —
// after the boxed values' destructors — at the exit of the scope enclosing
// the call.
//
// This is the embedded-field twin of CIDER-BUG-202's per-element array boxes
// (vm_iface_array_box_test.cpp) and of CIDER-BUG-184's whole-value box
// (vm_iface_box_test.cpp). The callee copies each embedded leaf into a box
// with the same two-word header (release thunk + ownership token, token 0 =
// the reserved "transferred" marker), and the call site registers a claim and
// a release per embedded leaf SLOT. Pre-fix the sret copy moved the fat
// pointers verbatim, so every call left the leaf pointing into the dead
// callee frame and dispatch read a zeroed slot — there was no heap growth to
// measure, because no box was ever allocated; the leak this pins is the one
// the fix could have introduced, i.e. a box per embedded leaf per call that
// nobody frees. It also pins that the caller's release walk descends the
// aggregate's FIELDS, not just its array dimensions: a registration that
// missed a field would leak that field's box once per call.
//
// The metric is the post-run LIVE set (heapBytes() - freeListBytes()): the
// permanent allocations (globals, vtables, strings) plus anything the run
// failed to free. A leak-free run's live set is its permanent allocations
// whatever the iteration count, so one iteration and many must agree within
// the VM first-fit splitter's slack (it does not coalesce, so chunk splitting
// can extend the heap by a whole frame chunk). kAllocatorSlack covers that
// absorbed remainder and is far below one leaked box.
//
// Shapes are pinned:
//   * one bare interface field, a class WITH a destructor: the release frees
//     the box and runs the finalizer exactly once per call (the exit code
//     checks the count: the callee's own local and the boxed copy, so 2 per
//     call);
//   * a class carrying an EMBEDDED class value with its own finalizer, so the
//     box release must run the whole embedded destructor walk (CIDER-BUG-088
//     order), not just the outer one — 4 per call, and a release that only
//     ran the outer finalizer would grow the live set once per call;
//   * an interface ARRAY field, and a struct field holding another struct's
//     interface field: the walk descends fields and array dimensions, so each
//     of these leaks one box per field per call if a leaf is missed;
//   * two fields of DIFFERENT concrete types: each leaf's box takes its own
//     size and thunk from its own vtable, so one wrong thunk or one missed
//     registration fails here;
//   * a pass-through (`pass(fill())`): one box per leaf, two caller
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
    // returning a struct/class embedding an interface field is exactly the
    // shape TypeChecker::hasInterfaceBoxing now reports), so the flag is
    // passed directly, like vm_iface_array_box_test does.
    gen.setInterfaceBoxing(ifaceBoxing);
    cider::ModuleIR module = gen.generate(*prog);
    if (rep.hasErrors()) {
        std::fprintf(stderr, "gen errors: %s\n", rep.errors()[0].message.c_str());
    }
    return module;
}

// The common preamble: one interface with two implementors of DIFFERENT field
// layouts, so each leaf's box must take its size and thunk from its OWN
// vtable rather than from one shared shape.
std::string preamble() {
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
           "class E {\n"
           "  var v: int;\n"
           "  var tag: int;\n"
           "  var pad: [u64; 64];\n"
           "  func ~E() { drops = drops + 1; }\n"
           "  func label() -> int { return this.v; }\n"
           "}\n";
}

// One or more bare interface fields in a returned struct. `drops` accounting
// per call, per field: the callee's own local implementor's finalizer, then
// the boxed copy's at the caller's scope exit — 2 per field.
std::string fieldProgram(int iterations, int fields) {
    std::string decl;
    std::string body;
    std::string checks;
    for (int i = 0; i < fields; ++i) {
        std::string name(1, static_cast<char>('a' + i));
        decl += "  var " + name + ": I;\n";
        body += "  var d" + name + ": D; d" + name + ".v = " + std::to_string(i) + ";\n";
        body += "  h." + name + " = d" + name + ";\n";
        checks += "    if (h." + name + ".label() != " + std::to_string(i) + ") { return 1; }\n";
    }
    return preamble() +
        "struct H {\n" + decl + "}\n"
        "func fill() -> H {\n"
        "  var h: H;\n" + body +
        "  return h;\n"
        "}\n"
        "func main() -> int {\n"
        "  var i: int = 0;\n"
        "  while (i < " + std::to_string(iterations) + ") {\n"
        "    var h: H = fill();\n" + checks +
        "    i = i + 1;\n"
        "  }\n"
        // Per call, per field: the callee's own local finalizer and the
        // boxed copy's, so 2 per field.
        "  if (drops != " + std::to_string(iterations * fields * 2) + ") { return 2; }\n"
        "  return 0;\n"
        "}\n";
}

// The `perFieldDtors` shape needs its own implementor declaration, because the
// embedded finalizer lives on the class the box holds.
std::string embeddedFieldProgram(int iterations) {
    return "var drops: int = 0;\n"
           "interface I {\n"
           "  func label() -> int;\n"
           "}\n"
           "class M {\n"
           "  var z: int;\n"
           "  func ~M() { drops = drops + 1; }\n"
           "}\n"
           "class D {\n"
           "  var v: int;\n"
           "  var m: M;\n"
           "  var pad: [u64; 64];\n"
           "  func ~D() { drops = drops + 1; }\n"
           "  func label() -> int { return this.v; }\n"
           "}\n"
           "class E {\n"
           "  var v: int;\n"
           "  var tag: int;\n"
           "  var pad: [u64; 64];\n"
           "  func ~E() { drops = drops + 1; }\n"
           "  func label() -> int { return this.v; }\n"
           "}\n"
           "struct H {\n"
           "  var a: I;\n"
           "}\n"
           "func fill() -> H {\n"
           "  var h: H;\n"
           "  var d: D; d.v = 3; d.m.z = 1;\n"
           "  h.a = d;\n"
           "  return h;\n"
           "}\n"
           "func main() -> int {\n"
           "  var i: int = 0;\n"
           "  while (i < " + std::to_string(iterations) + ") {\n"
           "    var h: H = fill();\n"
           "    if (h.a.label() != 3) { return 1; }\n"
           "    i = i + 1;\n"
           "  }\n"
           // The callee's local D (own + embedded M) and the boxed copy's
           // (own + embedded M): 4 finalizers per call.
           "  if (drops != " + std::to_string(iterations * 4) + ") { return 2; }\n"
           "  return 0;\n"
           "}\n";
}

// An interface ARRAY field in a returned struct: two leaves in one field, so
// the walk must descend the array dimensions of a FIELD (not only of the
// returned type itself). `nested` additionally wraps the array field in a
// second struct, so the field-of-a-field descent is pinned too.
std::string arrayFieldProgram(int iterations, bool nested) {
    std::string types =
        "struct A {\n"
        "  var arr: [I; 2];\n"
        "}\n";
    std::string decl = "A";
    std::string prefix;
    std::string checks;
    if (nested) {
        types += "struct B {\n"
                 "  var a: A;\n"
                 "}\n";
        decl = "B";
        prefix = "  b.a.";
        checks =
            "    if (b.a.arr[0].label() != 1) { return 1; }\n"
            "    if (b.a.arr[1].label() != 2) { return 1; }\n";
    } else {
        prefix = "  b.";
        checks =
            "    if (b.arr[0].label() != 1) { return 1; }\n"
            "    if (b.arr[1].label() != 2) { return 1; }\n";
    }
    return preamble() + types +
        "func fill() -> " + decl + " {\n"
        "  var b: " + decl + ";\n"
        "  var d0: D; d0.v = 1;\n"
        "  var d1: E; d1.v = 2;\n"
        + prefix + "arr[0] = d0;\n"
        + prefix + "arr[1] = d1;\n"
        "  return b;\n"
        "}\n"
        "func main() -> int {\n"
        "  var i: int = 0;\n"
        "  while (i < " + std::to_string(iterations) + ") {\n"
        "    var b: " + decl + " = fill();\n" + checks +
        "    i = i + 1;\n"
        "  }\n"
        // Two leaves, each contributing the callee's local finalizer and the
        // boxed copy's: 4 per call.
        "  if (drops != " + std::to_string(iterations * 4) + ") { return 2; }\n"
        "  return 0;\n"
        "}\n";
}

// A pass-through: one set of boxes, two caller temporaries (the call's own
// sret slot and the local's). The last claimant releases, so each box is
// freed exactly once; a double release corrupts the allocator and fails the
// run.
std::string passProgram(int iterations) {
    return preamble() +
        "struct H {\n"
        "  var a: I;\n"
        "}\n"
        "func fill() -> H {\n"
        "  var h: H;\n"
        "  var d: D; d.v = 5;\n"
        "  h.a = d;\n"
        "  return h;\n"
        "}\n"
        "func pass(x: H) -> H { return x; }\n"
        "func main() -> int {\n"
        "  var i: int = 0;\n"
        "  while (i < " + std::to_string(iterations) + ") {\n"
        "    var h: H = pass(fill());\n"
        "    if (h.a.label() != 5) { return 1; }\n"
        "    i = i + 1;\n"
        "  }\n"
        "  if (drops != " + std::to_string(iterations * 2) + ") { return 2; }\n"
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
    // One bare interface field, destructor-bearing: the box is freed and the
    // finalizer runs exactly once per call (the exit code checks drops).
    {
        size_t one = runLiveBytes(fieldProgram(1, 1), 0);
        size_t many = runLiveBytes(fieldProgram(200, 1), 0);
        CHECK(one != (size_t)-1 && many != (size_t)-1);
        CHECK(bounded(one, many));
    }

    // Two fields of different concrete types: each leaf's box picks its own
    // size and thunk, and both must be released.
    {
        size_t one = runLiveBytes(fieldProgram(1, 2), 0);
        size_t many = runLiveBytes(fieldProgram(200, 2), 0);
        CHECK(one != (size_t)-1 && many != (size_t)-1);
        CHECK(bounded(one, many));
    }

    // An embedded class value inside the boxed class: the release must run
    // the whole embedded destructor walk, not just the outer finalizer.
    {
        size_t one = runLiveBytes(embeddedFieldProgram(1), 0);
        size_t many = runLiveBytes(embeddedFieldProgram(200), 0);
        CHECK(one != (size_t)-1 && many != (size_t)-1);
        CHECK(bounded(one, many));
    }

    // An interface ARRAY field, and the same field wrapped in a second
    // struct: the walk must descend field array dimensions AND fields of
    // fields. A registration that missed either leaf would leak one box per
    // leaf per call.
    {
        size_t one = runLiveBytes(arrayFieldProgram(1, false), 0);
        size_t many = runLiveBytes(arrayFieldProgram(200, false), 0);
        CHECK(one != (size_t)-1 && many != (size_t)-1);
        CHECK(bounded(one, many));
    }
    {
        size_t one = runLiveBytes(arrayFieldProgram(1, true), 0);
        size_t many = runLiveBytes(arrayFieldProgram(200, true), 0);
        CHECK(one != (size_t)-1 && many != (size_t)-1);
        CHECK(bounded(one, many));
    }

    // Pass-through: one box per leaf, two caller temporaries, released
    // exactly once by the last holder.
    {
        size_t one = runLiveBytes(passProgram(1), 0);
        size_t many = runLiveBytes(passProgram(200), 0);
        CHECK(one != (size_t)-1 && many != (size_t)-1);
        CHECK(bounded(one, many));
    }

    std::printf("vm_iface_aggregate_box_test: ok\n");
    return 0;
}

int main() { return run(); }
