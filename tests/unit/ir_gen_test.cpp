#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "ir_gen.h"
#include "lexer.h"
#include "module_resolver.h"
#include "parser.h"
#include "symbol_collector.h"
#include "symbols.h"

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

std::string genSrc(const std::string& src, std::vector<std::string>* errors = nullptr) {
    cider::ErrorReporter rep;
    cider::Lexer lex(src, rep);
    cider::Parser parser(lex, rep);
    auto prog = parser.parseProgram();
    cider::SymbolTable table;
    cider::ModuleResolver resolver;
    cider::SymbolCollector collector(table, rep, resolver);
    collector.collect(*prog);
    if (errors) {
        for (auto& e : rep.errors()) errors->push_back(e.message);
    }
    cider::IRGenerator gen(table);
    auto module = gen.generate(*prog);
    std::ostringstream ss;
    std::streambuf* old = std::cout.rdbuf(ss.rdbuf());
    module.print();
    std::cout.rdbuf(old);
    return ss.str();
}

// Like genSrc, but hands back the module so tests can assert IR types
// (the printed text does not show operand/result types).
cider::ModuleIR genModuleSrc(const std::string& src) {
    cider::ErrorReporter rep;
    cider::Lexer lex(src, rep);
    cider::Parser parser(lex, rep);
    auto prog = parser.parseProgram();
    cider::SymbolTable table;
    cider::ModuleResolver resolver;
    cider::SymbolCollector collector(table, rep, resolver);
    collector.collect(*prog);
    cider::IRGenerator gen(table);
    return gen.generate(*prog);
}

std::string genSrcWithProvenance(const std::string& src, const std::string& symName,
                                   const std::string& mod) {
    cider::ErrorReporter rep;
    cider::Lexer lex(src, rep);
    cider::Parser parser(lex, rep);
    auto prog = parser.parseProgram();
    cider::SymbolTable table;
    cider::ModuleResolver resolver;
    cider::SymbolCollector collector(table, rep, resolver);
    collector.collect(*prog);
    cider::IRGenerator gen(table);
    // Fake a defining module for one symbol: with module provenance and no
    // exemption these would all emit mod_<m>__ names.
    auto sym = table.resolve(symName);
    if (sym) gen.setSymbolModules({{sym.get(), mod}});
    auto module = gen.generate(*prog);
    std::ostringstream ss;
    std::streambuf* old = std::cout.rdbuf(ss.rdbuf());
    module.print();
    std::cout.rdbuf(old);
    return ss.str();
}

int run() {
    // Function with typed arithmetic.
    std::string d = genSrc("func f(a: int) -> int {\n  return a + 1;\n}\n");
    CHECK(d.find("define f() {") != std::string::npos);
    CHECK(d.find("entry:") != std::string::npos);
    CHECK(d.find("%v0 = alloca 4") != std::string::npos);
    CHECK(d.find("add") != std::string::npos);
    CHECK(d.find("ret %v") != std::string::npos);

    // Global variable.
    d = genSrc("var g: int = 5;\nfunc main() -> void { }\n");
    CHECK(d.find("define main() {") != std::string::npos);

    // Local var + assignment: alloca/store/load sequence.
    d = genSrc("func main() -> void {\n  var x: int = 1;\n  x = x + 1;\n}\n");
    CHECK(d.find("alloca") != std::string::npos);
    CHECK(d.find("store") != std::string::npos);
    CHECK(d.find("load") != std::string::npos);

    // if/else produces two successor blocks with a merge.
    d = genSrc("func main() -> void {\n  if (c) { a(); } else { b(); }\n}\n");
    CHECK(d.find("br ") != std::string::npos);
    CHECK(d.find("main_then") != std::string::npos);
    CHECK(d.find("main_else") != std::string::npos);
    CHECK(d.find("main_ifcont") != std::string::npos);

    // Method call on a class -> mangled target.
    d = genSrc(
        "class K {\n  func go() -> void { }\n}\n"
        "func main() -> void {\n  var k: K* = nilPtr;\n  k.go();\n}\n");
    CHECK(d.find("call K_go") != std::string::npos);

    // Virtual method -> vcall with vtable index.
    d = genSrc(
        "class V {\n  virtual func m() -> void { }\n}\n"
        "func main() -> void {\n  var v: V* = nilPtr;\n  v.m();\n}\n");
    CHECK(d.find("vcall") != std::string::npos);

    // Struct field access loads at the field offset.
    d = genSrc(
        "struct P {\n  var x: int;\n  var y: char*;\n}\n"
        "func main() -> void {\n  var p: P* = nilPtr;\n  var a: int = p.x;\n}\n");
    CHECK(d.find("add %v") != std::string::npos);
    CHECK(d.find("load") != std::string::npos);

    // While loop structure.
    d = genSrc("func main() -> void {\n  while (x < 10) { x = x + 1; }\n}\n");
    CHECK(d.find("main_while_header") != std::string::npos);
    CHECK(d.find("main_while_body") != std::string::npos);
    CHECK(d.find("main_while_exit") != std::string::npos);

    // CID-020/021 never-mangle exemptions (spec Review Focus), exercised
    // through injected defining-module provenance: without the guards each
    // of these would emit a mod_m__ name.

    // `extern` references stay bare even with module provenance.
    d = genSrcWithProvenance(
        "extern func putchar(c: int) -> int;\n"
        "func main() -> int {\n  return putchar(65);\n}\n",
        "putchar", "m");
    CHECK(d.find("call putchar") != std::string::npos);
    CHECK(d.find("mod_m__putchar") == std::string::npos);

    // `main` calls stay bare even with module provenance.
    d = genSrcWithProvenance("func main() -> int {\n  return main();\n}\n", "main", "m");
    CHECK(d.find("define main() {") != std::string::npos);
    CHECK(d.find("call main") != std::string::npos);
    CHECK(d.find("mod_m__main") == std::string::npos);

    // `_start` calls stay bare even with module provenance.
    d = genSrcWithProvenance(
        "func _start() -> void { }\n"
        "func main() -> int {\n  _start();\n  return 0;\n}\n",
        "_start", "m");
    CHECK(d.find("call _start") != std::string::npos);
    CHECK(d.find("mod_m__") == std::string::npos);

    // A global literally named `main` is referenced bare even with module
    // provenance (definition and reference agree on the bare label). NOTE:
    // this locks end-to-end shape only — ir_gen re-defines globals during
    // generation, so injected provenance is shadowed here; the exemption
    // itself is pinned by the direct emitGlobalName checks below.
    d = genSrcWithProvenance(
        "var main: int = 7;\n"
        "func getmain() -> int {\n  return main;\n}\n",
        "main", "m");
    CHECK(d.find("load main") != std::string::npos);
    CHECK(d.find("mod_m__main") == std::string::npos);

    // Control: an ordinary function with module provenance still mangles at
    // reference sites, so the exemptions above did not over-broaden.
    d = genSrcWithProvenance(
        "func helper() -> int {\n  return 1;\n}\n"
        "func main() -> int {\n  return helper();\n}\n",
        "helper", "m");
    CHECK(d.find("call mod_m__helper") != std::string::npos);

    // Direct helper checks (all public in ir_gen.h, no harness needed).
    CHECK(cider::IRGenerator::isNeverMangled("main"));
    CHECK(cider::IRGenerator::isNeverMangled("_start"));
    CHECK(!cider::IRGenerator::isNeverMangled("helper"));
    CHECK(!cider::IRGenerator::isNeverMangled("putchar"));
    CHECK(!cider::IRGenerator::isNeverMangled(""));
    CHECK(cider::IRGenerator::emitGlobalName("m", "main") == "main");
    CHECK(cider::IRGenerator::emitGlobalName("m", "_start") == "_start");
    CHECK(cider::IRGenerator::emitGlobalName("m", "g") == "mod_m__g");
    CHECK(cider::IRGenerator::emitGlobalName("", "g") == "g");
    CHECK(cider::IRGenerator::emitFunctionName("m", "main") == "main");
    CHECK(cider::IRGenerator::emitFunctionName("m", "_start") == "_start");
    CHECK(cider::IRGenerator::emitFunctionName("m", "f") == "mod_m__f");
    {
        // definingModuleFor: extern collapses to main-file (bare) even with
        // module provenance; ordinary symbols keep the recorded module.
        cider::SymbolTable table;
        auto ext = std::make_shared<cider::FunctionSymbol>();
        ext->kind = cider::SymbolKind::FUNCTION;
        ext->name = "putchar";
        ext->isExtern = true;
        table.define(ext);
        auto helper = std::make_shared<cider::FunctionSymbol>();
        helper->kind = cider::SymbolKind::FUNCTION;
        helper->name = "helper";
        table.define(helper);
        cider::IRGenerator gen(table);
        gen.setSymbolModules({{ext.get(), "m"}, {helper.get(), "m"}});
        CHECK(gen.definingModuleFor(ext, "putchar") == "");
        CHECK(gen.definingModuleFor(helper, "helper") == "m");
        std::shared_ptr<cider::Symbol> nullSym;
        CHECK(gen.definingModuleFor(nullSym, "x") == "");
    }

    // CID-003: mixed f32/f64 operands promote to f64.
    {
        const std::string src =
            "func main() -> int { var a: f64 = 2.5; var b: float = 1.5; var c: f64 = a + b; return 0; }";
        std::string ir = genSrc(src);
        CHECK(ir.find("add %") != std::string::npos);
        // the F32 operand is widened before the add
        CHECK(ir.find("cvt") != std::string::npos || ir.find("%v") != std::string::npos);
        CHECK(ir.find("cvt_f32f64") != std::string::npos);

        // The checker's float<->f64 mutual compatibility cannot pin the
        // promotion result, and the text dump hides types, so also assert
        // the emitted width conversion and the add's operand/result types
        // directly (Task 1 deferred minor).
        auto module = genModuleSrc(src);
        bool sawWidenToF64 = false;
        bool addIsF64 = false;
        for (const auto& fn : module.getFunctions()) {
            for (const auto& block : fn->getBlocks()) {
                for (const auto& in : block->getInstructions()) {
                    if (in.op == cider::IROp::CVT_F32F64 && in.dest &&
                        in.dest->type == cider::IRType::F64) {
                        sawWidenToF64 = true;
                    }
                    if (in.op == cider::IROp::ADD && in.dest &&
                        in.dest->type == cider::IRType::F64 && in.args.size() == 2 &&
                        in.args[0].type == cider::IRType::F64 &&
                        in.args[1].type == cider::IRType::F64) {
                        addIsF64 = true;
                    }
                }
            }
        }
        CHECK(sawWidenToF64);
        CHECK(addIsF64);
    }
    return 0;
}

int main() { return run(); }
