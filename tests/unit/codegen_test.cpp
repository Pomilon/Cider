#include <cstdio>
#include <fstream>
#include <functional>
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

std::string genAsmTarget(const std::string& src, const std::string& target) {
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

    std::string path = "<tmp>/codegen_target_out.asm";
    cider::CodeGenerator cg(target);
    CHECKSTR(cg.generate(module, path));
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Like genAsmTarget, but lets the test patch the module's IR before codegen.
// CID-005 review carry-over 1 uses it to put an explicit Ms `callAbi` on a
// direct call to an interface-dispatched (post-accept SysV-pinned) method:
// class-body `msabi` modifiers do not parse yet, so the mismatching symbol
// state is simulated at the IR level.
std::string genAsmTargetPatched(const std::string& src, const std::string& target,
                                const std::function<void(cider::ModuleIR&)>& patch) {
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
    patch(module);

    std::string path = "<tmp>/codegen_target_patched_out.asm";
    cider::CodeGenerator cg(target);
    CHECKSTR(cg.generate(module, path));
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int run() {
    // CID-005: the target string selects the default ABI descriptor.
    CHECK(cider::targetAbiFor("native") == cider::Abi::SysV);
    CHECK(cider::targetAbiFor("pomeos") == cider::Abi::SysV);
    CHECK(cider::targetAbiFor("uefi") == cider::Abi::Ms);

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

    // CID-006: global function pointer -> data cell with the code label.
    std::string fnglob = genAsm(
        "func add(a: int, b: int) -> int { return a + b; }\n"
        "var g: fn(int, int) -> int = add;\n"
        "func main() -> int { return g(1, 2); }\n");
    CHECK(fnglob.find("g: dq add") != std::string::npos);

    // CID-006 interop: the legacy `*void` global gets the real cell too
    // (native used to read a constant zero).
    std::string voidglob = genAsm(
        "func add(a: int, b: int) -> int { return a + b; }\n"
        "var v: *void = add;\n"
        "func main() -> int { return 0; }\n");
    CHECK(voidglob.find("v: dq add") != std::string::npos);

    // CID-006 carry-over (Task 2 F1): an alias-spelled global fn emits the
    // same data cell and never a BSS slot named after the alias.
    std::string fnalias = genAsm(
        "func inc(a: int) -> int { return a + 1; }\n"
        "type Cb = fn(int) -> int;\n"
        "var g: Cb = inc;\n"
        "func main() -> int { return g(1); }\n");
    CHECK(fnalias.find("g: dq inc") != std::string::npos);
    CHECK(fnalias.find("g: resb") == std::string::npos);

    // CID-006 fix round 1 (F1): the review repro's two-hop alias chain emits
    // the same data cell and never a by-value copy of the label (the local
    // copy used to lower to `rep movsb` from the value-as-address).
    std::string fnchain = genAsm(
        "type B = fn(int) -> int;\n"
        "type A = B;\n"
        "func inc(a: int) -> int { return a + 1; }\n"
        "var g: A = inc;\n"
        "func main() -> int {\n"
        "  var f: fn(int) -> int = g;\n"
        "  return f(10) + 42;\n"
        "}\n");
    CHECK(fnchain.find("g: dq inc") != std::string::npos);
    CHECK(fnchain.find("rep movsb") == std::string::npos);

    // CID-006 fix round 1 (F3 pin): an alias of `*void` initialized from a
    // function name reaches the declAlias branch and emits the real cell.
    std::string pvalias = genAsm(
        "func add(a: int, b: int) -> int { return a + b; }\n"
        "type PV = *void;\n"
        "var g: PV = add;\n"
        "func main() -> int { return 0; }\n");
    CHECK(pvalias.find("g: dq add") != std::string::npos);

    // CID-005: `--target uefi` definitions use MS-ABI param registers.
    std::string msDef = genAsmTarget(
        "func add(a: int, b: int) -> int { return a + b; }\n"
        "func mix(x: int, f: float, p: *u8, g: f64) -> int { return x + p[0]; }\n"
        "func main() -> int { return add(1, 2); }\n",
        "uefi");
    CHECK(msDef.find("global main") != std::string::npos);
    // Definition side reads *from* the MS argument registers (`mov rcx, ...`
    // as a destination is the MS call-site form, owned by Task 3). Win64
    // slots are positional per design §2: argument N uses the Nth slot of
    // its class, so x -> rcx (slot 0), f -> xmm1 (slot 1), p -> r8 (slot 2),
    // g -> xmm3 (slot 3).
    CHECK(msDef.find(", rcx\n") != std::string::npos);   // arg 0 int -> rcx
    CHECK(msDef.find(", xmm1") != std::string::npos);    // arg 1 float -> xmm1
    CHECK(msDef.find(", r8\n") != std::string::npos);    // arg 2 pointer -> r8
    CHECK(msDef.find(", xmm3") != std::string::npos);    // arg 3 f64 -> xmm3
    CHECK(msDef.find("movq xmm") != std::string::npos);  // float params read via movq
    CHECK(msDef.find("mov rbx, rdi") == std::string::npos);  // no SysV rdi spill-in
    // CID-005 fix round 1: positional mixed signature. `pos(float, int,
    // float, int)` -> XMM0, RDX, XMM2, R9 (the per-class cursor model would
    // give XMM0, RCX, XMM1, RDX); the independent int/float cursors must not
    // be read.
    std::string msPos = genAsmTarget(
        "func pos(a: float, b: int, c: float, d: int) -> int { return b + d; }\n"
        "func main() -> int { return 0; }\n",
        "uefi");
    CHECK(msPos.find(", xmm0\n") != std::string::npos);  // arg 0 float -> xmm0
    CHECK(msPos.find(", rdx\n") != std::string::npos);   // arg 1 int -> rdx
    CHECK(msPos.find(", xmm2\n") != std::string::npos);  // arg 2 float -> xmm2
    CHECK(msPos.find(", r9\n") != std::string::npos);    // arg 3 int -> r9
    CHECK(msPos.find(", rcx\n") == std::string::npos);   // no independent int cursor
    CHECK(msPos.find(", xmm1\n") == std::string::npos);  // no independent float cursor
    // Positional slots beyond 3 go on the stack in source order: e (float,
    // position 4) at [rbp + 48], f (int, position 5) at [rbp + 56]. The
    // per-class cursor model would have put them in xmm2/r8 instead.
    std::string msPosStack = genAsmTarget(
        "func pos6(a: float, b: int, c: float, d: int, e: float, f: int) -> int { return b + d + f; }\n"
        "func main() -> int { return 0; }\n",
        "uefi");
    CHECK(msPosStack.find("[rbp + 48]") != std::string::npos);
    CHECK(msPosStack.find("[rbp + 56]") != std::string::npos);
    // Stack overflow args start at [rbp + 48] (5th int); the SysV stack-arg
    // base [rbp + 16] must not appear in the function at all.
    std::string msStack = genAsmTarget(
        "func six(a: int, b: int, c: int, d: int, e: int, f: int) -> int { return a + b + c + d + e + f; }\n"
        "func main() -> int { return six(1, 2, 3, 4, 5, 6); }\n",
        "uefi");
    CHECK(msStack.find("[rbp + 48]") != std::string::npos);
    CHECK(msStack.find("[rbp + 56]") != std::string::npos);
    CHECK(msStack.find("[rbp + 16]") == std::string::npos);
    // Callee-saved XMM8-15: a live float value forces the save/restore pair.
    // (bump uses four float values; the MS prologue renumbers the used part
    // of the allocator's xmm15-down pool to a dense xmm8.. run.)
    std::string msXmm = genAsmTarget(
        "func bump(f: float) -> float { var x: float = f; x = x + 1.0; return x; }\n"
        "func main() -> int { return 0; }\n",
        "uefi");
    // CID-005 fix round 3: each XMM save occupies a full 16-byte slot
    // (`sub rsp, 16` + `movdqu`), so the upper 64 bits survive too. The
    // old `movq` pair saved only half the register and a caller's upper
    // halves came back zeroed (review repro).
    CHECK(msXmm.find("sub rsp, 16") != std::string::npos);
    CHECK(msXmm.find("movdqu [rsp], xmm8") != std::string::npos);
    CHECK(msXmm.find("movdqu xmm8, [rsp]") != std::string::npos);
    CHECK(msXmm.find("movq [rsp], xmm8") == std::string::npos);
    CHECK(msXmm.find("movq xmm8, [rsp]") == std::string::npos);
    // The restore reads the slot the prologue saved: the frame must be
    // rewound to the xmm saves first — 7 GP * 8 + 4 xmm * 16 = 120.
    {
        size_t rewind = msXmm.find("lea rsp, [rbp - 120]");
        size_t restore = msXmm.find("movdqu xmm8, [rsp]");
        CHECK(rewind != std::string::npos);
        CHECK(restore != std::string::npos);
        CHECK(rewind < restore);
    }
    // An integer-only MS definition never saves an XMM register.
    CHECK(msStack.find("movq [rsp], xmm") == std::string::npos);
    // CID-005 fix round 2: MEMCPY lowers to `rep movsb`, which writes
    // RDI/RSI; Win64 makes both callee-saved, so an MS definition holding
    // a struct copy must save them in the prologue and restore them in the
    // epilogue (a bare rbx/r12-r15 save set leaked the caller's values).
    std::string msMemcpy = genAsmTarget(
        "struct S { var a: int; var b: int; var c: int; var d: int; }\n"
        "func cp(x: int) -> int {\n"
        "  var p: S;\n"
        "  p.a = x; p.b = 2; p.c = 3; p.d = 4;\n"
        "  var q: S = p;\n"
        "  return q.a + q.d;\n"
        "}\n"
        "func main() -> int { return cp(1); }\n",
        "uefi");
    {
        // Slice cp's body so main's own prologue/epilogue cannot satisfy
        // the ordering checks.
        size_t cpStart = msMemcpy.find("cp:");
        size_t cpEnd = msMemcpy.find("global main");
        CHECK(cpStart != std::string::npos);
        CHECK(cpEnd != std::string::npos);
        CHECK(cpStart < cpEnd);
        std::string cpAsm = msMemcpy.substr(cpStart, cpEnd - cpStart);
        CHECK(cpAsm.find("rep movsb") != std::string::npos);
        size_t pushRdi = cpAsm.find("push rdi");
        size_t pushRsi = cpAsm.find("push rsi");
        size_t copy = cpAsm.find("rep movsb");
        size_t popRsi = cpAsm.find("pop rsi");
        size_t popRdi = cpAsm.find("pop rdi");
        CHECK(pushRdi != std::string::npos);
        CHECK(pushRsi != std::string::npos);
        CHECK(popRsi != std::string::npos);
        CHECK(popRdi != std::string::npos);
        // Saved before the copy runs, restored after it (reverse order).
        CHECK(pushRdi < copy && pushRsi < copy);
        CHECK(copy < popRsi && popRsi < popRdi);
    }
    // The save is ABI-driven, not MEMCPY-driven: an MS definition with no
    // struct copy saves RDI/RSI too.
    std::string msNoCopy = genAsmTarget(
        "func add(a: int, b: int) -> int { return a + b; }\n"
        "func main() -> int { return add(1, 2); }\n",
        "uefi");
    CHECK(msNoCopy.find("push rdi") != std::string::npos);
    CHECK(msNoCopy.find("push rsi") != std::string::npos);
    CHECK(msNoCopy.find("pop rdi") != std::string::npos);
    CHECK(msNoCopy.find("pop rsi") != std::string::npos);
    // CID-005 carry-over: virtual/override definitions are pinned SysV in
    // the IR, so they keep the SysV rdi/rsi marshaling under --target uefi.
    std::string msVirt = genAsmTarget(
        "class Base {\n"
        "  virtual func f(x: int) -> int { return x + 1; }\n"
        "}\n"
        "func main() -> int { return 0; }\n",
        "uefi");
    CHECK(msVirt.find("push rdi") != std::string::npos);
    CHECK(msVirt.find(", rcx\n") == std::string::npos);
    CHECK(msVirt.find(", rdx\n") == std::string::npos);

    // CID-005 T3 (carry-over): RSP is 16-byte aligned at every call
    // (design §2). Cider's body sits at rsp ≡ 8 mod 16, so a call with an
    // even number of stack words needs an 8-byte pad while an odd number
    // is self-aligning: `one(1)` (no stack words) gets the pad,
    // `seven(...)` (one stack word) does not.
    std::string alignMix = genAsm(
        "func one(a: int) -> int { return a; }\n"
        "func seven(a: int, b: int, c: int, d: int, e: int, f: int, g: int) -> int { return a + g; }\n"
        "func main() -> int { return one(1) + seven(1, 2, 3, 4, 5, 6, 7); }\n");
    {
        size_t mainPos = alignMix.find("global main");
        CHECK(mainPos != std::string::npos);
        std::string mainAsm = alignMix.substr(mainPos);
        size_t pads = 0;
        for (size_t p = mainAsm.find("sub rsp, 8"); p != std::string::npos;
             p = mainAsm.find("sub rsp, 8", p + 1)) {
            pads++;
        }
        CHECK(pads == 1);
        CHECK(mainAsm.find("call one") != std::string::npos);
        CHECK(mainAsm.find("call seven") != std::string::npos);
        CHECK(mainAsm.find("push 7") != std::string::npos);  // one stack word
    }
    // The SysV `_start` kernel entry has no return address (rbp ≡ 8 mod 16),
    // so its pad is the mirror image: an odd stack word needs the pad there.
    std::string startAsm = genAsm(
        "func seven(a: int, b: int, c: int, d: int, e: int, f: int, g: int) -> int { return a + g; }\n"
        "func _start() -> void { seven(1, 2, 3, 4, 5, 6, 7); }\n");
    {
        size_t s = startAsm.find("_start:");
        CHECK(s != std::string::npos);
        std::string body = startAsm.substr(s);  // _start is the last function
        CHECK(body.find("push 7") != std::string::npos);
        size_t pads = 0;
        for (size_t p = body.find("sub rsp, 8"); p != std::string::npos;
             p = body.find("sub rsp, 8", p + 1)) {
            pads++;
        }
        CHECK(pads == 1);
    }

    // CID-005 T3: MS-ABI calls. The caller reserves 32 bytes of shadow
    // space and keeps RSP 16-byte aligned at the call; the body sits at
    // rsp ≡ 8 mod 16, so the no-stack-arg case adds an 8-byte pad
    // (32 + 8 = 40). Arguments ride the positional Win64 slots.
    std::string msCall = genAsmTarget(
        "func add(a: int, b: int) -> int { return a + b; }\n"
        "func main() -> int { return add(1, 2); }\n",
        "uefi");
    CHECK(msCall.find("sub rsp, 40") != std::string::npos);
    CHECK(msCall.find("add rsp, 40") != std::string::npos);
    CHECK(msCall.find("mov rcx, 1") != std::string::npos);
    CHECK(msCall.find("mov rdx, 2") != std::string::npos);
    CHECK(msCall.find("call add") != std::string::npos);
    // The same call must not take the SysV register shuffle. (The MS
    // definition's own prologue/epilogue push/pop rdi, so slice the call
    // sequence: pad-sub through pad-add.)
    {
        size_t seqStart = msCall.find("sub rsp, 40");
        size_t seqEnd = msCall.find("add rsp, 40", seqStart);
        CHECK(seqStart != std::string::npos && seqEnd != std::string::npos);
        std::string seq = msCall.substr(seqStart, seqEnd - seqStart);
        CHECK(seq.find("push 1") == std::string::npos);
        CHECK(seq.find("pop rdi") == std::string::npos);
    }

    // Float args ride xmm0-3, not the GP path.
    std::string msFloat = genAsmTarget(
        "func fadd(a: float, b: f64) -> f64 { return b; }\n"
        "func main() -> int { return fadd(1.5, 2.5) as int; }\n",
        "uefi");
    CHECK(msFloat.find("movss xmm0,") != std::string::npos);
    CHECK(msFloat.find("movsd xmm1,") != std::string::npos);

    // Stack args go to [rsp + 32 + 8k] above the shadow space; the slots
    // are positional, so e (arg 4) is [rsp + 32] and f (arg 5) is
    // [rsp + 40]. rcx/rdx/r8/r9 take args 0-3.
    std::string msCallStack = genAsmTarget(
        "func six(a: int, b: int, c: int, d: int, e: int, f: int) -> int { return e + f; }\n"
        "func main() -> int { return six(1, 2, 3, 4, 5, 6); }\n",
        "uefi");
    CHECK(msCallStack.find("mov r8, 3") != std::string::npos);
    CHECK(msCallStack.find("mov r9, 4") != std::string::npos);
    CHECK(msCallStack.find("[rsp + 32]") != std::string::npos);
    CHECK(msCallStack.find("[rsp + 40]") != std::string::npos);

    // A single stack word (odd count) is self-aligning: 32 + 8 = 40 and no
    // extra pad, with the argument at [rsp + 32].
    std::string msCallStackOdd = genAsmTarget(
        "func five(a: int, b: int, c: int, d: int, e: int) -> int { return e; }\n"
        "func main() -> int { return five(1, 2, 3, 4, 5); }\n",
        "uefi");
    CHECK(msCallStackOdd.find("sub rsp, 40") != std::string::npos);
    CHECK(msCallStackOdd.find("[rsp + 32]") != std::string::npos);
    CHECK(msCallStackOdd.find("[rsp + 40]") == std::string::npos);

    // A float stack argument must not clobber an earlier float argument
    // register: the stack store moves bits through rax, not xmm0 (which
    // already holds arg 0). a-d ride xmm0-3; e (arg 4) goes to [rsp + 32].
    std::string msFloatStack = genAsmTarget(
        "func mix(a: float, b: float, c: float, d: float, e: float) -> float {\n"
        "  return a + b + c + d + e;\n"
        "}\n"
        "func main() -> int {\n"
        "  var a: float = 1.0;\n"
        "  var b: float = 2.0;\n"
        "  var c: float = 3.0;\n"
        "  var d: float = 4.0;\n"
        "  var e: float = 5.0;\n"
        "  return mix(a, b, c, d, e) as int;\n"
        "}\n",
        "uefi");
    {
        size_t mainPos = msFloatStack.find("global main");
        CHECK(mainPos != std::string::npos);
        std::string mainAsm = msFloatStack.substr(mainPos);
        size_t seqStart = mainAsm.find("sub rsp, 40");
        size_t call = mainAsm.find("call mix");
        CHECK(seqStart != std::string::npos && call != std::string::npos &&
              seqStart < call);
        std::string seq = mainAsm.substr(seqStart, call - seqStart);
        CHECK(seq.find("movss xmm0, ") != std::string::npos);
        CHECK(seq.find("movss xmm1, ") != std::string::npos);
        CHECK(seq.find("movss xmm2, ") != std::string::npos);
        CHECK(seq.find("movss xmm3, ") != std::string::npos);
        CHECK(seq.find("[rsp + 32]") != std::string::npos);
        CHECK(seq.find("movq rax, xmm") != std::string::npos);
        size_t xmm0Count = 0;
        for (size_t p = seq.find("movss xmm0, "); p != std::string::npos;
             p = seq.find("movss xmm0, ", p + 1)) {
            xmm0Count++;
        }
        CHECK(xmm0Count == 1);  // the arg-0 load only
    }

    // Indirect through an msabi fn value uses r11 and the same frame.
    std::string msInd = genAsmTarget(
        "msabi func twice(x: int) -> int { return x * 2; }\n"
        "func main() -> int {\n"
        "  var p: msabi fn(int) -> int = twice;\n"
        "  return p(21);\n"
        "}\n",
        "uefi");
    CHECK(msInd.find("call r11") != std::string::npos);
    CHECK(msInd.find("mov rcx, 21") != std::string::npos);
    CHECK(msInd.find("sub rsp, 40") != std::string::npos);

    // __sret: the hidden result pointer is positional argument 0 (RCX); the
    // user argument follows in RDX. A cursor that skipped the hidden
    // pointer would put x in RCX.
    std::string msSret = genAsmTarget(
        "struct S { var a: int; var b: int; }\n"
        "func mk(x: int) -> S { var s: S; s.a = x; s.b = 2; return s; }\n"
        "func main() -> int { var s: S = mk(1); return s.a; }\n",
        "uefi");
    {
        size_t mainPos = msSret.find("global main");
        CHECK(mainPos != std::string::npos);
        std::string mainAsm = msSret.substr(mainPos);
        size_t call = mainAsm.find("call mk");
        CHECK(call != std::string::npos);
        size_t rcx = mainAsm.find("mov rcx, ");
        size_t rdx = mainAsm.find("mov rdx, 1");
        CHECK(rcx != std::string::npos && rcx < call);
        CHECK(rdx != std::string::npos && rdx < call);
        CHECK(mainAsm.find("mov rcx, 1") == std::string::npos);
    }

    // Interface implementations are pinned SysV after accept; a direct call
    // to one is lowered during accept (symbol ABI still Default), so it must
    // resolve the callee FunctionIR (SysV) instead of the target descriptor
    // (MS) — otherwise it marshals MS against a SysV prologue.
    std::string msIface = genAsmTarget(
        "interface I { func m() -> int; }\n"
        "class C { func m() -> int { return 7; } }\n"
        "func callDirect(c: *C) -> int { return c.m(); }\n"
        "func main() -> int { return 0; }\n",
        "uefi");
    {
        size_t start = msIface.find("callDirect:");
        size_t end = msIface.find("global main");
        CHECK(start != std::string::npos && end != std::string::npos && start < end);
        std::string cd = msIface.substr(start, end - start);
        CHECK(cd.find("call C_m") != std::string::npos);
        CHECK(cd.find("pop rdi") != std::string::npos);      // SysV arg 0
        CHECK(cd.find("mov rcx, ") == std::string::npos);    // not MS arg 0
        // The pinned definition keeps the SysV prologue (no MS rdi/rsi save).
        size_t def = msIface.find("C_m:");
        size_t entry = msIface.find(".entry:", def);
        CHECK(def != std::string::npos && entry != std::string::npos);
        CHECK(msIface.substr(def, entry - def).find("push rdi") == std::string::npos);
    }

    // CID-005 review carry-over: an explicit Ms `callAbi` on a direct call
    // must not beat the callee definition's pinned ABI. Interface
    // implementations are pinned SysV after accept; an `msabi` marker on
    // such a method would leave Ms on its symbol (class-body modifiers are
    // not yet parseable, so patch the CALL to simulate it). The call must
    // still marshal SysV against the pinned definition.
    std::string msIfaceExplicit = genAsmTargetPatched(
        "interface I { func m() -> int; }\n"
        "class C { func m() -> int { return 7; } }\n"
        "func callDirect(c: *C) -> int { return c.m(); }\n"
        "func main() -> int { return 0; }\n",
        "uefi",
        [](cider::ModuleIR& m) {
            for (auto& fn : m.getFunctions()) {
                for (auto& b : fn->getBlocks()) {
                    for (auto& in : b->getInstructions()) {
                        if (in.op == cider::IROp::CALL && !in.args.empty() &&
                            in.args[0].kind == cider::IRValue::LABEL &&
                            in.args[0].name.find("C_m") != std::string::npos) {
                            in.callAbi = cider::Abi::Ms;
                        }
                    }
                }
            }
        });
    {
        size_t start = msIfaceExplicit.find("callDirect:");
        size_t end = msIfaceExplicit.find("global main");
        CHECK(start != std::string::npos && end != std::string::npos && start < end);
        std::string cd = msIfaceExplicit.substr(start, end - start);
        CHECK(cd.find("call C_m") != std::string::npos);
        CHECK(cd.find("pop rdi") != std::string::npos);    // SysV arg 0 wins
        CHECK(cd.find("mov rcx, ") == std::string::npos);  // not MS arg 0
    }

    // super.method() on a plain (non-virtual) parent method: the definition
    // is Default -> MS under uefi, so the super call must use the resolved
    // callee ABI (MS), not the SysV typed-indirect default. A virtual parent
    // method is pinned SysV and keeps the SysV call.
    std::string msSuper = genAsmTarget(
        "class Base {\n"
        "  func plain(x: int) -> int { return x + 1; }\n"
        "  virtual func virt(x: int) -> int { return x + 2; }\n"
        "}\n"
        "class Derived extends Base {\n"
        "  override func virt(x: int) -> int { return x + 3; }\n"
        "  func test(x: int) -> int { return super.plain(x) + super.virt(x); }\n"
        "}\n"
        "func main() -> int { return 0; }\n",
        "uefi");
    {
        size_t start = msSuper.find("Derived_test:");
        size_t end = msSuper.find("global main");
        CHECK(start != std::string::npos && end != std::string::npos && start < end);
        std::string t = msSuper.substr(start, end - start);
        size_t plain = t.find("call Base_plain");
        size_t virt = t.find("call Base_virt");
        CHECK(plain != std::string::npos);
        CHECK(virt != std::string::npos);
        size_t rcx = t.find("rcx, ");       // MS marshaling for `plain`
        CHECK(rcx != std::string::npos && rcx < plain);
        size_t rdi = t.find("pop rdi");     // SysV marshaling for `virt`
        CHECK(rdi != std::string::npos && rdi > plain && rdi < virt);
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

    // CID-003 follow-up (W5): the constant CVT_ITOF immediate-range test
    // `bits <= INT32_MAX || (int64_t)bits >= INT32_MIN` is true for nearly
    // every 64-bit value, so out-of-range constants took the mov-imm32
    // decimal branch instead of the full 64-bit hex form. (Both encode the
    // same movabs; this pins the intended branch.)
    {
        cider::ModuleIR mod;
        auto fn = std::make_unique<cider::FunctionIR>("main", cider::IRType::I32);
        auto blk = std::make_unique<cider::BasicBlock>("entry");
        blk->addInstruction({cider::IROp::CVT_ITOF,
                             cider::IRValue::Register("big", cider::IRType::F64),
                             {cider::IRValue::Constant((int64_t)5000000000LL, cider::IRType::I64)}});
        blk->addInstruction({cider::IROp::CVT_ITOF,
                             cider::IRValue::Register("neg", cider::IRType::F64),
                             {cider::IRValue::Constant((int64_t)-100, cider::IRType::I64)}});
        blk->addInstruction({cider::IROp::RET, std::nullopt,
                             {cider::IRValue::Register("big", cider::IRType::F64)}});
        fn->addBlock(std::move(blk));
        mod.addFunction(std::move(fn));
        std::string path = "<tmp>/codegen_cvtimm.asm";
        cider::CodeGenerator cg;
        CHECK(cg.generate(mod, path));
        std::ifstream in(path);
        std::ostringstream ss;
        ss << in.rdbuf();
        std::string asmText = ss.str();
        CHECK(asmText.find("mov rax, 0x12A05F200") != std::string::npos);  // 5000000000
        CHECK(asmText.find("mov rax, 5000000000") == std::string::npos);
        CHECK(asmText.find("mov rax, -100") != std::string::npos);        // in-range stays decimal
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

    // CID-003 follow-up (M6): the float-constant pool must key by exact
    // IEEE-754 bits. std::pair<double, IRType> compares with operator<,
    // which treats -0.0 and +0.0 as equivalent, so a module holding both
    // shared one label and one sign. No Cider source reaches the pool with
    // -0.0 today: unary minus lowers to `0.0 - x`, +0.0 - +0.0 is +0.0,
    // and the AST ConstantFolder is not wired into any pipeline — so this
    // pin constructs the IR directly.
    {
        cider::ModuleIR mod;
        auto fn = std::make_unique<cider::FunctionIR>("main", cider::IRType::I32);
        auto blk = std::make_unique<cider::BasicBlock>("entry");
        blk->addInstruction({cider::IROp::MOV, cider::IRValue::Register("p64", cider::IRType::F64),
                             {cider::IRValue::Constant(0.0, cider::IRType::F64)}});
        blk->addInstruction({cider::IROp::MOV, cider::IRValue::Register("n64", cider::IRType::F64),
                             {cider::IRValue::Constant(-0.0, cider::IRType::F64)}});
        blk->addInstruction({cider::IROp::MOV, cider::IRValue::Register("p32", cider::IRType::F32),
                             {cider::IRValue::Constant(0.0, cider::IRType::F32)}});
        blk->addInstruction({cider::IROp::MOV, cider::IRValue::Register("n32", cider::IRType::F32),
                             {cider::IRValue::Constant(-0.0, cider::IRType::F32)}});
        fn->addBlock(std::move(blk));
        mod.addFunction(std::move(fn));
        std::string path = "<tmp>/codegen_negzero.asm";
        cider::CodeGenerator cg;
        CHECK(cg.generate(mod, path));
        std::ifstream in(path);
        std::ostringstream ss;
        ss << in.rdbuf();
        std::string asmText = ss.str();
        CHECK(asmText.find("dq 0x0") != std::string::npos);              // +0.0 f64
        CHECK(asmText.find("dq 0x8000000000000000") != std::string::npos);  // -0.0 f64
        CHECK(asmText.find("dd 0x0") != std::string::npos);              // +0.0 f32
        CHECK(asmText.find("dd 0x80000000") != std::string::npos);       // -0.0 f32
    }

    // CID-005/T5: wide string constants become `dw` units (uppercase hex,
    // zero-padded) preceded by `align 2`.
    {
        std::string wideAsm = genAsmTarget(
            "func main() -> int { var p: *u16 = u\"AB\"; return p[0]; }\n", "native");
        CHECK(wideAsm.find("dw 0x0041, 0x0042") != std::string::npos);
        CHECK(wideAsm.find("align 2") != std::string::npos);
    }

    // CID-005: UEFI emission differences.
    {
        std::string uefiAsm = genAsmTarget(
            "msabi func efi_main(image: *void, st: *void) -> u64 { return 0; }\n"
            "func main() -> int { return 0; }\n",
            "uefi");
        CHECK(uefiAsm.find("global efi_main") != std::string::npos);
        CHECK(uefiAsm.find("global main") != std::string::npos);
        CHECK(uefiAsm.find("extern malloc") == std::string::npos);
        CHECK(uefiAsm.find("extern free") == std::string::npos);
        CHECK(uefiAsm.find("extern _setjmp") == std::string::npos);
        CHECK(uefiAsm.find("extern longjmp") == std::string::npos);
        CHECK(uefiAsm.find("default rel") != std::string::npos);
        // The pomeos post-pass (`global _setjmp` + stub) must not run for uefi.
        CHECK(uefiAsm.find("global _setjmp") == std::string::npos);
        // User externs still emitted.
        std::string uefiExt = genAsmTarget(
            "msabi extern func puts(s: *u16) -> u64;\n"
            "msabi func efi_main(i: *void, s: *void) -> u64 { return puts(nilPtr); }\n",
            "uefi");
        CHECK(uefiExt.find("extern puts") != std::string::npos);
        // `_start` is a normal function: no kernel-stack [rbp + 8] argc load.
        std::string uefiStart = genAsmTarget(
            "msabi func _start(argc: int, argv: *u8) -> void { }\n",
            "uefi");
        CHECK(uefiStart.find("global _start") != std::string::npos);
        // Definition side reads *from* the MS argument registers (`mov r15,
        // rcx` here); the existing T2 tests pin the same `", rcx\n"` form.
        // `mov rcx, <src>` is the call-site form, which an empty body never
        // emits.
        CHECK(uefiStart.find(", rcx\n") != std::string::npos);    // MS arg reg
        CHECK(uefiStart.find("[rbp + 8]") == std::string::npos);  // no kernel convention
        // Carry-over 3 (T4 repro): an explicit `sysvabi func _start` under
        // uefi is still an ordinary call-entered function, not the native
        // kernel entry — the kernel argc/argv window must not appear.
        std::string uefiSysvStart = genAsmTarget(
            "sysvabi func _start(argc: int, argv: *u8) -> void { }\n",
            "uefi");
        CHECK(uefiSysvStart.find("global _start") != std::string::npos);
        CHECK(uefiSysvStart.find("[rbp + 8]") == std::string::npos);
        CHECK(uefiSysvStart.find("[rbp + 16]") == std::string::npos);
    }
    return 0;
}

int main() { return run(); }
