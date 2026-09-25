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
    return 0;
}

int main() { return run(); }
