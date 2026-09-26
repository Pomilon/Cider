#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "lexer.h"
#include "module_resolver.h"
#include "parser.h"
#include "symbol_collector.h"
#include "symbols.h"
#include "type_checker.h"

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

std::vector<std::string> checkSrc(const std::string& src) {
    cider::ErrorReporter rep;
    cider::Lexer lex(src, rep);
    cider::Parser parser(lex, rep);
    auto prog = parser.parseProgram();
    cider::SymbolTable table;
    cider::ModuleResolver resolver;
    cider::SymbolCollector collector(table, rep, resolver);
    collector.collect(*prog);
    rep.clear();
    cider::TypeChecker checker(table, rep);
    checker.check(*prog);
    std::vector<std::string> out;
    for (auto& e : rep.errors()) out.push_back(e.message);
    return out;
}

// CID-005: same pipeline as checkSrc plus the target-driven checker flags
// (`--target uefi` wires both from main.cpp).
std::vector<std::string> checkSrcOpts(const std::string& src, bool defaultAbiMs, bool allowUefiEntry) {
    cider::ErrorReporter rep;
    cider::Lexer lex(src, rep);
    cider::Parser parser(lex, rep);
    auto prog = parser.parseProgram();
    cider::SymbolTable table;
    cider::ModuleResolver resolver;
    cider::SymbolCollector collector(table, rep, resolver);
    collector.collect(*prog);
    rep.clear();
    cider::TypeChecker checker(table, rep);
    checker.setDefaultAbiMs(defaultAbiMs);
    checker.setUefiEntryAllowed(allowUefiEntry);
    checker.check(*prog);
    std::vector<std::string> out;
    for (auto& e : rep.errors()) out.push_back(e.message);
    return out;
}

std::vector<std::string> checkWithDefaultAbiMs(const std::string& src) {
    return checkSrcOpts(src, /*defaultAbiMs=*/true, /*allowUefiEntry=*/false);
}

bool hasError(const std::vector<std::string>& errors, const std::string& sub) {
    for (auto& e : errors) {
        if (e.find(sub) != std::string::npos) return true;
    }
    return false;
}

int run() {
    // Clean program with main.
    auto ok = checkSrc("func main() -> void { var x: int = 1; }\n");
    CHECK(ok.empty());

    // String -> int mismatch.
    auto e1 = checkSrc("func main() -> void { var x: int = \"str\"; }\n");
    CHECK(hasError(e1, "Type mismatch in declaration of 'x'"));

    // int -> char* mismatch.
    auto e2 = checkSrc("func main() -> void { var x: int = 1; var y: char* = x; }\n");
    CHECK(hasError(e2, "Type mismatch in declaration of 'y'"));

    // int -> float widening is OK.
    auto ok2 = checkSrc("func main() -> void { var x: float = 1; }\n");
    CHECK(ok2.empty());

    // CID-003: f32 is an alias of float; f64 is a real type.
    auto okF64 = checkSrc(
        "func main() -> int {\n"
        "  var a: f32 = 1.5;\n"
        "  var b: f64 = 2.5;\n"
        "  var c: f64 = 7;\n"
        "  var d: float = b;\n"
        "  var e: f64 = a + b;\n"
        "  return 0;\n"
        "}\n");
    if (!okF64.empty()) {
        std::fprintf(stderr, "FAIL f64 declarations: %s\n", okF64[0].c_str());
        return 1;
    }

    // CID-003 review: bitwise operators reject float-family operands even
    // though arithmetic promotes them.
    auto eBit1 = checkSrc(
        "func main() -> void { var a: float = 1.0; var b: float = 2.0; var c: float = a & b; }\n");
    CHECK(hasError(eBit1, "Bitwise operator '&' requires integer operands"));
    auto eBit2 = checkSrc(
        "func main() -> void { var a: f64 = 1.0; var b: f64 = 2.0; var c: f64 = a | b; }\n");
    CHECK(hasError(eBit2, "Bitwise operator '|' requires integer operands"));
    auto eBit3 = checkSrc(
        "func main() -> void { var a: f64 = 1.0; var b: f64 = 2.0; var c: f64 = a << b; }\n");
    CHECK(hasError(eBit3, "Bitwise operator '<<' requires integer operands"));
    auto eBit4 = checkSrc(
        "func main() -> void { var a: int = 1; var b: f64 = 2.0; var c: int = a & b; }\n");
    CHECK(hasError(eBit4, "Bitwise operator '&' requires integer operands"));

    // Integer bitwise operations are unaffected.
    auto okBit = checkSrc(
        "func main() -> void { var a: int = 6; var b: int = 3; var c: int = a & b; var d: int = a << b; }\n");
    CHECK(okBit.empty());

    // CID-003 follow-up (W5): compound assignments with no float lowering
    // are rejected like their binary forms; the checker used to accept them
    // and codegen emitted divsd while the VM threw at runtime.
    auto eModAssign = checkSrc(
        "func main() -> void { var a: f64 = 5.0; a %= 2; }\n");
    CHECK(hasError(eModAssign, "Modulo operator '%' requires integer operands"));
    auto eModAssignRhs = checkSrc(
        "func main() -> void { var a: int = 5; a %= 2.5; }\n");
    CHECK(hasError(eModAssignRhs, "Modulo operator '%' requires integer operands"));
    auto eAndAssign = checkSrc(
        "func main() -> void { var a: float = 5.0; a &= 2; }\n");
    CHECK(hasError(eAndAssign, "Bitwise operator '&' requires integer operands"));
    auto eOrAssign = checkSrc(
        "func main() -> void { var a: f64 = 5.0; a |= 2; }\n");
    CHECK(hasError(eOrAssign, "Bitwise operator '|' requires integer operands"));
    auto eXorAssign = checkSrc(
        "func main() -> void { var a: float = 5.0; a ^= 2; }\n");
    CHECK(hasError(eXorAssign, "Bitwise operator '^' requires integer operands"));
    // Integer compound operations and float arithmetic compounds still work.
    auto okCompound = checkSrc(
        "func main() -> void { var i: int = 7; i %= 3; i &= 2; i |= 1; i ^= 4;\n"
        "  var f: float = 1.0; f += 1; f *= 2; f -= 3; f /= 4; }\n");
    CHECK(okCompound.empty());

    // Return values are typed but NOT checked against the function's
    // return type (parity with main — the IR phase enforces it).
    auto e3 = checkSrc("func f() -> int { return \"s\"; }\nfunc main() -> void { }\n");
    CHECK(e3.empty());

    // Undefined identifier.
    auto e4 = checkSrc("func main() -> void { var x: int = nope; }\n");
    CHECK(hasError(e4, "Undefined identifier 'nope'"));

    // Assignment to undeclared variable.
    auto e5 = checkSrc("func main() -> void { undeclared = 5; }\n");
    CHECK(hasError(e5, "Undefined identifier 'undeclared'"));

    // Const assignment.
    auto e6 = checkSrc("func main() -> void { const C: int = 1; C = 2; }\n");
    CHECK(hasError(e6, "Cannot assign to constant 'C'"));

    // CID-003 follow-up (W5): non-constant float global initializers have
    // no runtime initializer and silently read 0.0 on every backend.
    auto eFloatGlobal = checkSrc(
        "var g0: int = 7;\nvar g: f64 = g0;\nfunc main() -> void { }\n");
    CHECK(hasError(eFloatGlobal,
                   "Global float initializer in declaration of 'g' must use constant expressions"));
    auto eFloatGlobalCall = checkSrc(
        "func give() -> f64 { return 1.0; }\nvar g: float = give();\nfunc main() -> void { }\n");
    CHECK(hasError(eFloatGlobalCall,
                   "Global float initializer in declaration of 'g' must use constant expressions"));
    // Constant shapes and initializer-less globals still pass.
    auto okFloatGlobals = checkSrc(
        "var a: f64 = 5;\n"
        "var b: f64 = 1.0 + 2.0;\n"
        "var c: float = -0.1;\n"
        "var d: f64 = 7 as f64;\n"
        "var e: f64 = 5 / 2;\n"
        "var f: f64;\n"
        "func main() -> void { }\n");
    CHECK(okFloatGlobals.empty());

    // CID-003 follow-up (W6): type aliases resolve to their underlying
    // float type before the check, so `type Real = f64` cannot bypass it.
    auto eFloatGlobalAlias = checkSrc(
        "type Real = f64;\n"
        "func give() -> f64 { return 1.0; }\n"
        "var g: Real = give();\n"
        "func main() -> void { }\n");
    CHECK(hasError(eFloatGlobalAlias,
                   "Global float initializer in declaration of 'g' must use constant expressions"));
    // Aliased constants stay legal, as do non-float and pointer aliases.
    auto okFloatGlobalAlias = checkSrc(
        "type Real = f64;\n"
        "type Whole = int;\n"
        "type RealPtr = f64*;\n"
        "var a: Real = 2.5;\n"
        "var b: Real = 5;\n"
        "var c: Whole = 7;\n"
        "var p: RealPtr = nilPtr;\n"
        "func main() -> void { }\n");
    CHECK(okFloatGlobalAlias.empty());

    // Class pointer covariance.
    auto ok3 = checkSrc(
        "class Base { var a: int; }\n"
        "class Derived extends Base { var b: int; }\n"
        "func main() -> void { var p: Base* = d; var d: Derived* = nilPtr; }\n");
    // Note: d must be declared before use; reorder:
    ok3 = checkSrc(
        "class Base { var a: int; }\n"
        "class Derived extends Base { var b: int; }\n"
        "func main() -> void { var d: Derived* = nilPtr; var p: Base* = d; }\n");
    CHECK(ok3.empty());

    // Reversed covariance -> error.
    auto e7 = checkSrc(
        "class Base { var a: int; }\n"
        "class Derived extends Base { var b: int; }\n"
        "func main() -> void { var b: Base* = nilPtr; var d: Derived* = b; }\n");
    CHECK(hasError(e7, "Type mismatch"));

    // Missing entry point.
    auto e8 = checkSrc("func f() -> void { }\n");
    CHECK(hasError(e8, "Missing entry point"));

    // Undefined type in declaration.
    auto e9 = checkSrc("func main() -> void { var x: Nope = nilPtr; }\n");
    CHECK(e9.size() >= 1);

    // Interface field assignment works when satisfied.
    auto ok4 = checkSrc(
        "interface I { func m() -> void; }\n"
        "class C { func m() -> void { } }\n"
        "func main() -> void { var i: I = c; var c: C* = nilPtr; }\n");
    ok4 = checkSrc(
        "interface I { func m() -> void; }\n"
        "class C { func m() -> void { } }\n"
        "func main() -> void { var c: C* = nilPtr; var i: I = c; }\n");
    CHECK(ok4.empty());

    // CID-006: fn(...) typing and checking.
    auto fnOk = checkSrc(
        "func add(a: int, b: int) -> int { return a + b; }\n"
        "func main() -> int {\n"
        "  var f: fn(int, int) -> int = add;\n"
        "  var r: int = f(1, 2);\n"
        "  var n: fn(int, int) -> int = nilPtr;\n"
        "  var v: *void = add;\n"
        "  var back: fn(int, int) -> int = v as fn(int, int) -> int;\n"
        "  return r;\n"
        "}\n");
    CHECK(fnOk.empty());

    auto fnSig = checkSrc(
        "func inc(a: int) -> int { return a + 1; }\n"
        "func main() -> int { var f: fn(int, int) -> int = inc; return 0; }\n");
    CHECK(hasError(fnSig, "Type mismatch in declaration of 'f'. Expected fn(int, int) -> int, got fn(int) -> int"));

    auto fnArity = checkSrc(
        "func inc(a: int) -> int { return a + 1; }\n"
        "func main() -> int { var f: fn(int) -> int = inc; var r: int = f(1, 2); return r; }\n");
    CHECK(hasError(fnArity, "Function 'fn(int) -> int' expects 1 arguments, got 2"));

    auto fnArg = checkSrc(
        "func inc(a: int) -> int { return a + 1; }\n"
        "func main() -> int { var f: fn(int) -> int = inc; var r: int = f(\"x\"); return r; }\n");
    CHECK(hasError(fnArg, "Argument 1 to 'fn(int) -> int' type mismatch. Expected int, got string"));

    auto fnNonFn = checkSrc(
        "func main() -> int { var f: fn(int) -> int = 42; return 0; }\n");
    CHECK(hasError(fnNonFn, "Type mismatch in declaration of 'f'. Expected fn(int) -> int, got int"));

    auto fnAssign = checkSrc(
        "func inc(a: int) -> int { return a + 1; }\n"
        "func noret(a: int) -> void { }\n"
        "func main() -> int { var f: fn(int) -> int = inc; f = noret; return 0; }\n");
    CHECK(hasError(fnAssign, "Type mismatch in assignment. Expected fn(int) -> int, got fn(int) -> void"));

    auto fnGlobalNonConst = checkSrc(
        "func inc(a: int) -> int { return a + 1; }\n"
        "var local: fn(int) -> int = inc;\n"
        "var bad: fn(int) -> int = local;\n"
        "func main() -> int { return 0; }\n");
    CHECK(hasError(fnGlobalNonConst, "Global function pointer initializer in declaration of 'bad' must be a function name or nilPtr"));

    auto fnStruct = checkSrc(
        "struct P { var x: int; }\n"
        "func take(p: P) -> int { return p.x; }\n"
        "func main() -> int { var f: fn(P) -> int = take; var r: int = f(0); return r; }\n");
    CHECK(hasError(fnStruct, "do not support struct/class by-value"));

    // CID-006 fix (F1): aliases of fn(...) types must reach the same checks
    // as the spelled-out type — valid use, indirect-call arity/argument
    // checks, params, and the global-constant rule.
    auto fnAliasOk = checkSrc(
        "type Cb = fn(int) -> int;\n"
        "func inc(a: int) -> int { return a + 1; }\n"
        "var g: Cb = inc;\n"
        "func main() -> int {\n"
        "  var f: Cb = inc;\n"
        "  var r: int = f(1);\n"
        "  var n: Cb = nilPtr;\n"
        "  return r;\n"
        "}\n");
    CHECK(fnAliasOk.empty());

    auto fnAliasArity = checkSrc(
        "type Cb = fn(int) -> int;\n"
        "func inc(a: int) -> int { return a + 1; }\n"
        "func main() -> int { var f: Cb = inc; var r: int = f(1, 2); return r; }\n");
    CHECK(hasError(fnAliasArity, "Function 'fn(int) -> int' expects 1 arguments, got 2"));

    auto fnAliasArg = checkSrc(
        "type Cb = fn(int) -> int;\n"
        "func inc(a: int) -> int { return a + 1; }\n"
        "func main() -> int { var f: Cb = inc; var r: int = f(\"x\"); return r; }\n");
    CHECK(hasError(fnAliasArg, "Argument 1 to 'fn(int) -> int' type mismatch. Expected int, got string"));

    auto fnAliasParam = checkSrc(
        "type Cb = fn(int) -> int;\n"
        "func apply(cb: Cb) -> int { return cb(1, 2); }\n"
        "func main() -> int { return 0; }\n");
    CHECK(hasError(fnAliasParam, "Function 'fn(int) -> int' expects 1 arguments, got 2"));

    auto fnAliasGlobalNonConst = checkSrc(
        "type Cb = fn(int) -> int;\n"
        "func inc(a: int) -> int { return a + 1; }\n"
        "var local: Cb = inc;\n"
        "var bad: Cb = local;\n"
        "func main() -> int { return 0; }\n");
    CHECK(hasError(fnAliasGlobalNonConst,
                   "Global function pointer initializer in declaration of 'bad' must be a function name or nilPtr"));

    // CID-006 fix round 2: cyclic aliases must terminate in the checker too.
    // Comparing an initializer through resolveType used to recurse forever
    // (check exit 139 on `var x: B = 5`); it must reach the normal mismatch.
    auto cyclicAlias = checkSrc(
        "type A = B;\n"
        "type B = A;\n"
        "func main() -> int {\n"
        "  var x: B = 5;\n"
        "  return 0;\n"
        "}\n");
    CHECK(hasError(cyclicAlias, "Type mismatch in declaration of 'x'"));

    // CID-006 fix round 3: uses of a cyclic alias — assignment, same-typed
    // copy and call parameter — must be accepted without recursion
    // (checking these crashed before the round-2 visited set).
    auto cyclicAliasUses = checkSrc(
        "type A = B;\n"
        "type B = A;\n"
        "func use(v: B) -> int { return 0; }\n"
        "func main() -> int {\n"
        "  var x: B;\n"
        "  x = x;\n"
        "  var y: B = x;\n"
        "  return use(y);\n"
        "}\n");
    CHECK(cyclicAliasUses.empty());

    // CID-005: ABI identity and target-default typing.
    auto abiMismatch = checkSrc(
        "func inc(a: int) -> int { return a + 1; }\n"
        "func main() -> int {\n"
        "  var a: fn(int) -> int = inc;\n"
        "  var b: msabi fn(int) -> int = a;\n"
        "  return 0;\n"
        "}\n");
    CHECK(hasError(abiMismatch, "Type mismatch in declaration of 'b'. Expected msabi fn(int) -> int, got fn(int) -> int"));

    auto abiOk = checkSrc(
        "msabi func inc(a: int) -> int { return a + 1; }\n"
        "func main() -> int {\n"
        "  var f: msabi fn(int) -> int = inc;\n"
        "  return f(1);\n"
        "}\n");
    CHECK(abiOk.empty());

    auto abiDefaultUefi = checkWithDefaultAbiMs(
        "func inc(a: int) -> int { return a + 1; }\n"
        "func main() -> int { var f: msabi fn(int) -> int = inc; return f(1); }\n");
    CHECK(abiDefaultUefi.empty());

    // CID-005: `--target uefi` allows `efi_main` instead of main/_start.
    auto uefiEntry = checkSrcOpts(
        "msabi func efi_main(image: *void, st: *void) -> u64 { return 0; }\n",
        /*defaultAbiMs=*/true, /*allowUefiEntry=*/true);
    CHECK(uefiEntry.empty());
    auto uefiEntryMissing = checkSrcOpts(
        "msabi func helper() -> u64 { return 0; }\n",
        /*defaultAbiMs=*/true, /*allowUefiEntry=*/true);
    CHECK(hasError(uefiEntryMissing, "Missing entry point"));

    // CID-005/T5: wide literals type as [u16; N] (incl. NUL) and u'X' as
    // u16; u16 arrays decay to *u16 (and *void) but never to *u8.
    auto wideOk = checkSrc(
        "func take(p: *u16) -> int { return p[0]; }\n"
        "func main() -> int {\n"
        "  var s: [u16; 3] = u\"AB\";\n"
        "  var p: *u16 = u\"CD\";\n"
        "  var c: u16 = u'X';\n"
        "  return take(u\"EF\") + s[0] + p[1] + c;\n"
        "}\n");
    CHECK(wideOk.empty());
    auto wideBad = checkSrc(
        "func main() -> int { var p: *u8 = u\"AB\"; return 0; }\n");
    CHECK(!wideBad.empty());  // u16 array never decays to u8*

    // CID-005/T5 (F1): a function-pointer return must match the declared
    // signature exactly, ABI included (the broad return-statement gap is
    // out of scope).
    auto retAbiBad = checkSrc(
        "func g(a: int) -> int { return a; }\n"
        "func f() -> msabi fn(int) -> int { return g; }\n"
        "func main() -> int { return 0; }\n");
    CHECK(hasError(retAbiBad,
                   "Return type mismatch. Expected msabi fn(int) -> int, got fn(int) -> int"));
    auto retAbiOk = checkSrc(
        "msabi func g(a: int) -> int { return a; }\n"
        "func f() -> msabi fn(int) -> int { return g; }\n"
        "func main() -> int { return 0; }\n");
    CHECK(retAbiOk.empty());
    auto retSigBad = checkSrc(
        "func g(a: int, b: int) -> int { return a + b; }\n"
        "func f() -> fn(int) -> int { return g; }\n"
        "func main() -> int { return 0; }\n");
    CHECK(hasError(retSigBad,
                   "Return type mismatch. Expected fn(int) -> int, got fn(int, int) -> int"));
    // Under --target uefi an unmarked function is MS-ABI, so the same
    // source flips which of the pair is the mismatch.
    auto retDefaultMsOk = checkWithDefaultAbiMs(
        "func g(a: int) -> int { return a; }\n"
        "func f() -> msabi fn(int) -> int { return g; }\n"
        "func main() -> int { return 0; }\n");
    CHECK(retDefaultMsOk.empty());
    auto retDefaultMsBad = checkWithDefaultAbiMs(
        "func g(a: int) -> int { return a; }\n"
        "func f() -> fn(int) -> int { return g; }\n"
        "func main() -> int { return 0; }\n");
    CHECK(hasError(retDefaultMsBad,
                   "Return type mismatch. Expected fn(int) -> int, got msabi fn(int) -> int"));

    // CID-005/T5 (F2): an interface implementation's method symbol is pinned
    // to SysV by the collector, so a method VALUE types as plain `fn` under
    // --target uefi (previously it typed as `msabi fn`, disagreeing with its
    // SysV definition).
    auto ifaceMethodValue = checkWithDefaultAbiMs(
        "interface I { func m() -> int; }\n"
        "class C { func m() -> int { return 7; } }\n"
        "func main() -> int {\n"
        "  var c: C;\n"
        "  var p: fn(*C) -> int = C_m;\n"
        "  return p(&c);\n"
        "}\n");
    CHECK(ifaceMethodValue.empty());
    auto ifaceMethodValueMs = checkWithDefaultAbiMs(
        "interface I { func m() -> int; }\n"
        "class C { func m() -> int { return 7; } }\n"
        "func main() -> int {\n"
        "  var c: C;\n"
        "  var p: msabi fn(*C) -> int = C_m;\n"
        "  return p(&c);\n"
        "}\n");
    CHECK(hasError(ifaceMethodValueMs,
                   "Type mismatch in declaration of 'p'. Expected msabi fn(*C) -> int, got fn(*C) -> int"));

    // CID-005/T5 fix round 1 (Q1): a fixed-size array initialised from a
    // wide literal must match the literal's [u16; N+1] size exactly.
    // areTypesCompatible's integer fallback is size-blind, so a short or
    // long literal used to be accepted and then silently over-read (array
    // MEMCPY of the declared size from the smaller data block) or truncated.
    auto wideSizeOk = checkSrc(
        "func main() -> int { var s: [u16; 3] = u\"AB\"; return s[0]; }\n");
    CHECK(wideSizeOk.empty());
    auto wideSizeLong = checkSrc(
        "func main() -> int { var s: [u16; 4] = u\"AB\"; return 0; }\n");
    CHECK(hasError(wideSizeLong,
                   "Array initializer size mismatch in declaration of 's': expected 4, got 3"));
    auto wideSizeShort = checkSrc(
        "func main() -> int { var s: [u16; 2] = u\"AB\"; return 0; }\n");
    CHECK(hasError(wideSizeShort,
                   "Array initializer size mismatch in declaration of 's': expected 2, got 3"));
    // Globals take the same declaration path.
    auto wideSizeGlobal = checkSrc(
        "var g: [u16; 4] = u\"AB\";\n"
        "func main() -> int { return 0; }\n");
    CHECK(hasError(wideSizeGlobal,
                   "Array initializer size mismatch in declaration of 'g': expected 4, got 3"));
    // The broader array-size rule in areTypesCompatible also rejects
    // assigning a mismatched wide literal to an existing fixed array.
    auto wideAssignBad = checkSrc(
        "func main() -> int { var s: [u16; 4]; s = u\"AB\"; return 0; }\n");
    CHECK(hasError(wideAssignBad, "Type mismatch in assignment"));

    // CID-005/T5 fix round 1 (Q3): a return whose expression already failed
    // must not cascade into a bogus F1 fn-signature mismatch.
    auto retFailedExpr = checkSrc(
        "func f() -> fn(int) -> int { return missing; }\n"
        "func main() -> int { return 0; }\n");
    CHECK(hasError(retFailedExpr, "Undefined identifier 'missing'"));
    CHECK(!hasError(retFailedExpr, "Return type mismatch"));

    // CID-005/T7: `type` aliases are STRUCT-kind type providers in the
    // symbol table, so the indirect-call by-value gate must resolve the
    // alias chain before its kind test (mirroring ir_gen::isStructValueType)
    // or every fn-pointer spelled with a UEFI typedef (`-> EFI_STATUS`,
    // `EFI_MEMORY_TYPE` parameters) is misreported as a struct-by-value call.
    auto aliasFnSig = checkSrc(
        "type EFI_STATUS = u64;\n"
        "type EFI_MEMORY_TYPE = u32;\n"
        "struct P { var f: msabi fn(*P, EFI_MEMORY_TYPE) -> EFI_STATUS; }\n"
        "msabi func cb(p: *P, t: EFI_MEMORY_TYPE) -> EFI_STATUS { return 0; }\n"
        "msabi func g(p: *P) -> EFI_STATUS { p->f(p, 7); return cb(p, 7); }\n"
        "func main() -> int { return 0; }\n");
    CHECK(aliasFnSig.empty());

    // Genuine by-value aggregates through fn pointers stay rejected, both
    // direct and behind an alias.
    auto aggFnSig = checkSrc(
        "struct S { var x: int; }\n"
        "struct Q { var f: msabi fn(*Q, S) -> u64; }\n"
        "msabi func g(q: *Q, s: S) -> u64 { q->f(q, s); return 0; }\n"
        "func main() -> int { return 0; }\n");
    CHECK(hasError(aggFnSig, "do not support struct/class by-value"));
    auto aggAliasFnSig = checkSrc(
        "struct S { var x: int; }\n"
        "type SAlias = S;\n"
        "struct Q { var f: msabi fn(*Q, SAlias) -> u64; }\n"
        "msabi func g(q: *Q, s: SAlias) -> u64 { q->f(q, s); return 0; }\n"
        "func main() -> int { return 0; }\n");
    CHECK(hasError(aggAliasFnSig, "do not support struct/class by-value"));

    // CID-005/T7 fix round 1 (F3): fn-pointer signature equality must
    // alias-resolve the nested signature (parameters AND return), not just
    // the outer spelling: `msabi fn(*P) -> EFI_STATUS` and
    // `msabi fn(*P) -> u64` are the same type. Everything else (primitive
    // width, ABI) stays exact.
    auto aliasFnRet = checkSrc(
        "type EFI_STATUS = u64;\n"
        "struct P { var x: u64; }\n"
        "msabi func f(p: *P) -> EFI_STATUS { return 0; }\n"
        "func main() -> int {\n"
        "  var h: msabi fn(*P) -> u64 = f;\n"
        "  return 0;\n"
        "}\n");
    CHECK(aliasFnRet.empty());
    auto aliasFnRetReverse = checkSrc(
        "type EFI_STATUS = u64;\n"
        "struct P { var x: u64; }\n"
        "msabi func f(p: *P) -> u64 { return 0; }\n"
        "func main() -> int {\n"
        "  var h: msabi fn(*P) -> EFI_STATUS = f;\n"
        "  return 0;\n"
        "}\n");
    CHECK(aliasFnRetReverse.empty());
    // An outer fn-type alias is interchangeable with the expanded spelling
    // in declarations, argument passing and returns.
    auto aliasFnOuter = checkSrc(
        "type EFI_STATUS = u64;\n"
        "struct P { var x: u64; }\n"
        "type Cb = msabi fn(*P) -> u64;\n"
        "msabi func f(p: *P) -> EFI_STATUS { return 0; }\n"
        "msabi func pass(h: Cb) -> void { h(nilPtr); }\n"
        "msabi func pick() -> Cb { return f; }\n"
        "func main() -> int {\n"
        "  var h: Cb = f;\n"
        "  pass(h);\n"
        "  return 0;\n"
        "}\n");
    CHECK(aliasFnOuter.empty());
    // Alias parameter spelling against an expanded signature at an
    // argument site and through an indirect call.
    auto aliasFnParam = checkSrc(
        "type EFI_MEMORY_TYPE = u32;\n"
        "struct P { var f: msabi fn(*P, u32) -> u64; }\n"
        "msabi func take(h: msabi fn(*P, EFI_MEMORY_TYPE) -> u64) -> void { }\n"
        "msabi func g(p: *P, t: u32) -> u64 { p->f(p, t); return 0; }\n"
        "func main() -> int { take(g); return 0; }\n");
    CHECK(aliasFnParam.empty());
    // A raw function argument whose alias-return type meets an expanded
    // parameter signature (the finding's argument-passing repro).
    auto aliasFnArg = checkSrc(
        "type EFI_STATUS = u64;\n"
        "struct P { var x: u64; }\n"
        "msabi func f(p: *P) -> EFI_STATUS { return 0; }\n"
        "msabi func take(h: msabi fn(*P) -> u64) -> void { h(nilPtr); }\n"
        "func main() -> int { take(f); return 0; }\n");
    CHECK(aliasFnArg.empty());
    // Genuine mismatch stays rejected: the alias resolves to `u64` while
    // the other spelling is `int`.
    auto aliasFnRetBad = checkSrc(
        "type EFI_STATUS = u64;\n"
        "struct P { var x: u64; }\n"
        "msabi func f(p: *P) -> int { return 0; }\n"
        "func main() -> int { var h: msabi fn(*P) -> EFI_STATUS = f; return 0; }\n");
    CHECK(hasError(aliasFnRetBad, "Type mismatch in declaration of 'h'"));

    // CID-005/T7 fix round 1 (F4): an alias whose target does not resolve
    // (`type NopeAlias = Nope`) must stay rejected by the by-value gate
    // instead of silently widening it.
    auto unresolvedAliasCall = checkSrc(
        "type NopeAlias = Nope;\n"
        "struct Q { var f: msabi fn(*Q, NopeAlias) -> u64; }\n"
        "msabi func g(q: *Q, s: NopeAlias) -> u64 { q->f(q, s); return 0; }\n"
        "func main() -> int { return 0; }\n");
    CHECK(hasError(unresolvedAliasCall, "do not support struct/class by-value"));

    // CID-005/T7 fix round 2: self-referential and mutually cyclic fn-type
    // aliases must terminate signature-alias resolution. Before the visited
    // set, `resolveSignatureAliases` and `resolveType` recursed into each
    // other and every use of the alias below overflowed the compiler stack
    // (check exit 139). Native behavior matches the pre-fix parent: the
    // alias expands to the same signature as the raw function, so the
    // declaration is accepted.
    auto cyclicFnSelfRef = checkSrc(
        "type F = fn(F) -> void;\n"
        "func foo(x: F) -> void { }\n"
        "func main() -> int { var h: F = foo; return 0; }\n");
    CHECK(cyclicFnSelfRef.empty());
    // Under `--target uefi` the alias's stored SysV signature and the
    // target-default msabi function genuinely differ: the parent's normal
    // mismatch diagnostic, not a crash.
    auto cyclicFnSelfRefUefi = checkWithDefaultAbiMs(
        "type F = fn(F) -> void;\n"
        "func foo(x: F) -> void { }\n"
        "func main() -> int { var h: F = foo; return 0; }\n");
    CHECK(hasError(cyclicFnSelfRefUefi,
                   "Type mismatch in declaration of 'h'. Expected F, got msabi fn(F) -> void"));
    // The mutual-cycle shape (`type A = B; type B = fn(A) -> void;`), a
    // previously accepted program that the round-1 fix crashed on.
    auto cyclicFnMutual = checkSrc(
        "type A = B;\n"
        "type B = fn(A) -> void;\n"
        "func foo(x: A) -> void { }\n"
        "func main() -> int { var h: A = foo; return 0; }\n");
    CHECK(cyclicFnMutual.empty());
    auto cyclicFnMutualUefi = checkWithDefaultAbiMs(
        "type A = B;\n"
        "type B = fn(A) -> void;\n"
        "func foo(x: A) -> void { }\n"
        "func main() -> int { var h: A = foo; return 0; }\n");
    CHECK(hasError(cyclicFnMutualUefi,
                   "Type mismatch in declaration of 'h'. Expected A, got msabi fn(A) -> void"));
    // Full use surface: declaration, assignment, argument passing (aliased
    // and raw fn values) and an indirect call whose parameter is the cyclic
    // alias. Each of these funnels through areTypesCompatible.
    auto cyclicFnUses = checkSrc(
        "type F = fn(F) -> void;\n"
        "func noop(x: F) -> void { }\n"
        "func take(h: F) -> void { }\n"
        "func main() -> int {\n"
        "  var h: F = noop;\n"
        "  h = noop;\n"
        "  take(h);\n"
        "  take(noop);\n"
        "  h(noop);\n"
        "  return 0;\n"
        "}\n");
    CHECK(cyclicFnUses.empty());
    // A genuine mismatch through the cyclic alias still reports normally.
    auto cyclicFnBad = checkSrc(
        "type F = fn(F) -> void;\n"
        "func bad() -> void { }\n"
        "func main() -> int { var h: F = bad; return 0; }\n");
    CHECK(hasError(cyclicFnBad, "Type mismatch in declaration of 'h'. Expected F, got fn() -> void"));
    // The cycle guard must not poison sibling positions: an alias used for
    // both a parameter and the return type resolves fully in each position
    // (a visited set that never pops would leave the second spelling
    // unresolved and reject this valid declaration).
    auto aliasFnRepeat = checkSrc(
        "type EFI_STATUS = u64;\n"
        "msabi func f(p: EFI_STATUS) -> u64 { return 0; }\n"
        "func main() -> int {\n"
        "  var h: msabi fn(EFI_STATUS) -> EFI_STATUS = f;\n"
        "  return 0;\n"
        "}\n");
    CHECK(aliasFnRepeat.empty());

    // CID-005 final-review (F1): signature alias expansion must be bounded.
    // A branched fn-type alias DAG doubles the work per level
    // (`type A0 = fn(A1, A1) -> void; type A1 = fn(A2, A2) -> void; ...`),
    // so the pre-fix compiler spent >60 s on 22 levels (the base compiler
    // rejected the same source in ~7 ms). The expansion budget must make
    // this finish in milliseconds and fall back to the unexpanded spelling
    // comparison, keeping the base diagnostic.
    {
        std::string dag;
        for (int i = 0; i < 22; ++i) {
            dag += "type A" + std::to_string(i) + " = fn(A" + std::to_string(i + 1) +
                   ", A" + std::to_string(i + 1) + ") -> void;\n";
        }
        dag += "type A22 = fn(u64, u64) -> void;\n";
        dag += "func foo(a: u64, b: u64) -> void { }\n";
        dag += "func main() -> int { var h: A0 = foo; return 0; }\n";
        auto dagStart = std::chrono::steady_clock::now();
        auto dagErrors = checkSrc(dag);
        double dagSeconds = std::chrono::duration<double>(
                                std::chrono::steady_clock::now() - dagStart).count();
        // Generous bound: the pre-fix compiler needs >60 s here, the fix
        // milliseconds; a wall-clock assert catches any reintroduced
        // exponential blow-up even on slow CI machines.
        if (dagSeconds > 5.0) {
            std::fprintf(stderr, "FAIL branched alias DAG took %.3fs\n", dagSeconds);
            return 1;
        }
        CHECK(hasError(dagErrors, "Type mismatch in declaration of 'h'"));
    }

    // The budget must not truncate expansions a real signature can reach:
    // a 10-level branched DAG is ~2^11 alias expansions per side, expands
    // fully, and the alias spelling stays interchangeable with a function
    // whose parameters carry the alias.
    {
        std::string dag;
        for (int i = 0; i < 10; ++i) {
            dag += "type B" + std::to_string(i) + " = fn(B" + std::to_string(i + 1) +
                   ", B" + std::to_string(i + 1) + ") -> void;\n";
        }
        dag += "type B10 = fn(u64, u64) -> void;\n";
        dag += "func bar(a: B1, b: B1) -> void { }\n";
        dag += "func main() -> int { var h: B0 = bar; return 0; }\n";
        CHECK(checkSrc(dag).empty());
    }
    return 0;
}

int main() { return run(); }
