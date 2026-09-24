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
    return 0;
}

int main() { return run(); }
