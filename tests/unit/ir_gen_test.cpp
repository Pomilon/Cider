#include <cstdio>
#include <cstring>
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

    // CID-006 F1: `extern var` is linker-provided DATA. ir_gen registers it
    // via addExternVariable — not addExternFunction — so the VM never turns
    // a variable read into a function VALUE.
    {
        cider::ModuleIR m = genModuleSrc(
            "extern var errno: int;\n"
            "extern func putchar(c: char) -> int;\n"
            "func main() -> int {\n  return errno;\n}\n");
        CHECK(m.getExternVariables().size() == 1);
        CHECK(m.getExternVariables()[0] == "errno");
        CHECK(m.getExternFunctions().size() == 1);
        CHECK(m.getExternFunctions()[0] == "putchar");
    }

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

    // CID-003: an f64 literal keeps its double value (no float round-trip).
    {
        std::string ir = genSrc("func main() -> int { var x: f64 = 16777217.0; return 0; }");
        CHECK(ir.find("16777217") != std::string::npos);

        // The dump shows the constant's parsed double even for the buggy F32
        // path, so also pin the emitted IR: the declaration must produce an
        // F64 constant directly, not cvt_f32f64 from an F32 constant (the
        // F32 pool entry rounds 16777217 to 16777216 on native).
        auto module = genModuleSrc(
            "func main() -> int { var x: f64 = 16777217.0; return 0; }");
        bool sawExactF64Const = false;
        bool sawF32ToF64Cvt = false;
        for (const auto& fn : module.getFunctions()) {
            for (const auto& block : fn->getBlocks()) {
                for (const auto& in : block->getInstructions()) {
                    if (in.op == cider::IROp::CVT_F32F64) sawF32ToF64Cvt = true;
                    for (const auto& a : in.args) {
                        if (a.kind == cider::IRValue::CONSTANT &&
                            a.type == cider::IRType::F64 && a.constValue == 16777217.0) {
                            sawExactF64Const = true;
                        }
                    }
                }
            }
        }
        CHECK(sawExactF64Const);
        CHECK(!sawF32ToF64Cvt);
    }

    // CID-003 follow-up (I2): every integer width/sign converts to float
    // through CVT_ITOF (previously only I32/I64 did; u32/u64/narrow
    // sources stored raw bits or truncated on native).
    {
        const std::string src =
            "func main() -> int {\n"
            "  var a: f64 = 3000000000;\n"        // u32 literal
            "  var b: u64 = 5000000000;\n"
            "  var c: f64 = b;\n"                  // u64 variable
            "  var s: i8 = 0 - 5;\n"
            "  var d: f64 = s;\n"                  // narrow signed
            "  var e: f64 = 3000000000 as f64;\n"  // u32 cast
            "  return 0;\n"
            "}";
        auto module = genModuleSrc(src);
        bool sawU32 = false;
        bool sawU64 = false;
        bool sawI8 = false;
        for (const auto& fn : module.getFunctions()) {
            for (const auto& block : fn->getBlocks()) {
                for (const auto& in : block->getInstructions()) {
                    if (in.op != cider::IROp::CVT_ITOF || !in.dest ||
                        in.dest->type != cider::IRType::F64 || in.args.size() != 1) {
                        continue;
                    }
                    if (in.args[0].type == cider::IRType::U32) sawU32 = true;
                    if (in.args[0].type == cider::IRType::U64) sawU64 = true;
                    if (in.args[0].type == cider::IRType::I8) sawI8 = true;
                }
            }
        }
        CHECK(sawU32);
        CHECK(sawU64);
        CHECK(sawI8);
    }

    // CID-003 follow-up (I3): an integer compound-assignment RHS converts
    // to the target's float width before the arithmetic.
    {
        auto module = genModuleSrc(
            "func main() -> int { var d: f64 = 1.0; d += 1; d *= 2; return 0; }");
        bool addIsF64 = false;
        bool mulIsF64 = false;
        for (const auto& fn : module.getFunctions()) {
            for (const auto& block : fn->getBlocks()) {
                for (const auto& in : block->getInstructions()) {
                    if ((in.op == cider::IROp::ADD || in.op == cider::IROp::MUL) &&
                        in.dest && in.dest->type == cider::IRType::F64 &&
                        in.args.size() == 2 && in.args[0].type == cider::IRType::F64 &&
                        in.args[1].type == cider::IRType::F64) {
                        if (in.op == cider::IROp::ADD) addIsF64 = true;
                        if (in.op == cider::IROp::MUL) mulIsF64 = true;
                    }
                }
            }
        }
        CHECK(addIsF64);
        CHECK(mulIsF64);
    }

    // CID-003 follow-up (I4): every float PHI arg has the PHI's common
    // width, in both arm orders (the type used to come from the then-arm
    // only, so `c ? d : 1.5` merged an F32 value into an F64 PHI).
    {
        const char* sources[] = {
            "func main() -> int { var c: bool = true; var d: f64 = 2.5;"
            " var e: f64 = c ? d : 1.5; return 0; }",
            "func main() -> int { var c: bool = true; var d: f64 = 2.5;"
            " var e: f64 = c ? 1.5 : d; return 0; }",
            "func main() -> int { var c: bool = true; var a: float = 1.5;"
            " var b: float = 2.5; var f: float = c ? a : b; return 0; }",
        };
        for (const char* src : sources) {
            auto module = genModuleSrc(src);
            bool sawFloatPhi = false;
            bool allArgsMatch = true;
            for (const auto& fn : module.getFunctions()) {
                for (const auto& block : fn->getBlocks()) {
                    for (const auto& in : block->getInstructions()) {
                        if (in.op != cider::IROp::PHI || !in.dest) continue;
                        if (in.dest->type != cider::IRType::F32 &&
                            in.dest->type != cider::IRType::F64) {
                            continue;
                        }
                        sawFloatPhi = true;
                        for (const auto& [pred, val] : in.phiArgs) {
                            if (val.type != in.dest->type) allArgsMatch = false;
                        }
                    }
                }
            }
            CHECK(sawFloatPhi);
            CHECK(allArgsMatch);
        }
    }

    // CID-003 follow-up (M3): global float initializers fold integer
    // literals and constant expressions into the declared width's
    // IEEE-754 bits (initialIsFloat) instead of silently BSS-zeroing.
    {
        auto bitsOfF64 = [](double v) -> int64_t {
            uint64_t bits = 0;
            std::memcpy(&bits, &v, sizeof(bits));
            return (int64_t)bits;
        };
        auto bitsOfF32 = [](double v) -> int64_t {
            float f = (float)v;
            uint32_t bits = 0;
            std::memcpy(&bits, &f, sizeof(bits));
            return (int64_t)bits;
        };
        auto module = genModuleSrc(
            "var g: f64 = 5;\n"
            "var h: f64 = 1.0 + 2.0;\n"
            "var f: float = 7;\n"
            "var a: [f64; 2] = [1, 2];\n"
            "func main() -> int { return 0; }\n");
        int found = 0;
        for (const auto& var : module.getGlobalVars()) {
            if (var.name == "g") {
                CHECK(var.initialIsFloat);
                CHECK(var.type == cider::IRType::F64);
                CHECK(var.initialValue == bitsOfF64(5.0));
                found++;
            } else if (var.name == "h") {
                CHECK(var.initialIsFloat);
                CHECK(var.initialValue == bitsOfF64(3.0));
                found++;
            } else if (var.name == "f") {
                CHECK(var.initialIsFloat);
                CHECK(var.type == cider::IRType::F32);
                CHECK(var.initialValue == bitsOfF32(7.0));
                found++;
            }
        }
        CHECK(found == 3);
        // The global float array's byte list is the elements' exact f64 bit
        // patterns (int elements used to emit raw int bits).
        uint64_t vals[2] = {0, 0};
        double f64vals[2] = {1.0, 2.0};
        for (int k = 0; k < 2; k++) std::memcpy(&vals[k], &f64vals[k], 8);
        std::ostringstream expect;
        expect << "a: db ";
        for (int k = 0; k < 2; k++) {
            for (int b = 0; b < 8; b++) {
                if (k > 0 || b > 0) expect << ", ";
                expect << ((vals[k] >> (8 * b)) & 0xFFULL);
            }
        }
        bool sawArr = false;
        for (const auto& line : module.getStringData()) {
            if (line == expect.str()) sawArr = true;
        }
        CHECK(sawArr);
    }

    // CID-003 follow-up (M2): -0.1 in an f64 global keeps the nearest
    // double, not the f32 literal widened to f64.
    {
        auto module = genModuleSrc(
            "var b: f64 = -0.1;\nfunc main() -> int { return 0; }\n");
        bool sawB = false;
        for (const auto& var : module.getGlobalVars()) {
            if (var.name != "b") continue;
            sawB = true;
            CHECK(var.initialIsFloat);
            uint64_t bits = 0;
            double expected = -0.1;
            std::memcpy(&bits, &expected, sizeof(bits));
            CHECK(var.initialValue == (int64_t)bits);
        }
        CHECK(sawB);
    }

    // CID-003 follow-up (M5): a mixed signed/unsigned comparison converts
    // both operands to one common unsigned type before the CMP.
    {
        auto module = genModuleSrc(
            "func main() -> int {\n"
            "  var a: i32 = 0 - 1;\n"
            "  var b: u32 = 1;\n"
            "  if (a < b) { return 1; }\n"
            "  return 0;\n}\n");
        bool sawCmp = false;
        for (const auto& fn : module.getFunctions()) {
            for (const auto& block : fn->getBlocks()) {
                for (const auto& in : block->getInstructions()) {
                    if (in.op != cider::IROp::CMP_LT) continue;
                    sawCmp = true;
                    CHECK(in.args.size() == 2);
                    CHECK(in.args[0].type == cider::IRType::U32);
                    CHECK(in.args[1].type == cider::IRType::U32);
                }
            }
        }
        CHECK(sawCmp);
    }

    // CID-006: calls through fn values use the stored return type.
    {
        auto module = genModuleSrc(
            "func add(a: int, b: int) -> int { return a + b; }\n"
            "func fadd(a: int, b: int) -> float { return 1.0; }\n"
            "func main() -> int {\n"
            "  var f: fn(int, int) -> int = add;\n"
            "  var r: int = f(1, 2);\n"
            "  var g: fn(int, int) -> float = fadd;\n"
            "  var s: float = g(1, 2);\n"
            "  return r;\n"
            "}\n");
        bool sawLabelStore = false;
        bool sawIntCall = false;
        bool sawFloatCall = false;
        for (const auto& fn : module.getFunctions()) {
            for (const auto& block : fn->getBlocks()) {
                for (const auto& in : block->getInstructions()) {
                    if (in.op == cider::IROp::STORE && in.args.size() > 1 &&
                        in.args[1].kind == cider::IRValue::LABEL && in.args[1].name == "add") {
                        sawLabelStore = true;
                    }
                    if (in.op == cider::IROp::CALL && !in.args.empty() &&
                        in.args[0].kind == cider::IRValue::REGISTER && in.dest) {
                        if (in.dest->type == cider::IRType::I32) sawIntCall = true;
                        if (in.dest->type == cider::IRType::F32) sawFloatCall = true;
                    }
                }
            }
        }
        CHECK(sawLabelStore);
        CHECK(sawIntCall);
        CHECK(sawFloatCall);
    }

    // CID-006: a global fn initialized from a named function becomes a
    // .data cell holding the function label.
    {
        auto module = genModuleSrc(
            "func add(a: int, b: int) -> int { return a + b; }\n"
            "var g: fn(int, int) -> int = add;\n"
            "func main() -> int { return g(1, 2); }\n");
        bool sawDq = false;
        for (const auto& line : module.getStringData()) {
            if (line.find("g: dq add") != std::string::npos) sawDq = true;
        }
        CHECK(sawDq);
    }

    // CID-006 interop: the legacy `*void g = f` global gets the same cell
    // (was a silent constant zero on native).
    {
        auto module = genModuleSrc(
            "func add(a: int, b: int) -> int { return a + b; }\n"
            "var g: *void = add;\n"
            "func main() -> int { return 0; }\n");
        bool sawDq = false;
        for (const auto& line : module.getStringData()) {
            if (line.find("g: dq add") != std::string::npos) sawDq = true;
        }
        CHECK(sawDq);
    }

    // CID-006 carry-over (Task 2 F1): alias-spelled fn globals get the same
    // .data cell (they used to fall through to a BSS slot sized by the
    // alias name), and an alias-spelled nilPtr global gets the 8-byte zero
    // cell.
    {
        auto module = genModuleSrc(
            "func inc(a: int) -> int { return a + 1; }\n"
            "type Cb = fn(int) -> int;\n"
            "var g: Cb = inc;\n"
            "var h: Cb = nilPtr;\n"
            "func main() -> int { return 0; }\n");
        bool sawDq = false;
        for (const auto& line : module.getStringData()) {
            if (line.find("g: dq inc") != std::string::npos) sawDq = true;
        }
        CHECK(sawDq);
        bool sawH = false;
        for (const auto& var : module.getGlobalVars()) {
            if (var.name != "h") continue;
            sawH = true;
            CHECK(var.type == cider::IRType::PTR);
            CHECK(var.totalBytes == 8);
            CHECK(var.initialValue == 0);
        }
        CHECK(sawH);
    }

    // CID-006 carry-over (Task 2 F1): calls through alias-spelled fn values
    // type the result from the stored signature and convert arguments
    // through the stored parameter types; an alias-spelled local sizes its
    // slot at 8 bytes (the alias name used to reach the struct-size path).
    {
        auto module = genModuleSrc(
            "func idf(a: float) -> float { return a; }\n"
            "type Cb = fn(float) -> float;\n"
            "func main() -> int {\n"
            "  var f: Cb = idf;\n"
            "  var s: float = f(2);\n"
            "  return 0;\n"
            "}\n");
        bool sawFnSlot = false;
        bool sawF32Call = false;
        bool sawF32Arg = false;
        for (const auto& fn : module.getFunctions()) {
            for (const auto& block : fn->getBlocks()) {
                for (const auto& in : block->getInstructions()) {
                    if (in.op == cider::IROp::ALLOCA && in.args.size() == 1 &&
                        in.args[0].kind == cider::IRValue::CONSTANT &&
                        in.args[0].intValue == 8 && in.dest &&
                        in.dest->type == cider::IRType::PTR) {
                        sawFnSlot = true;
                    }
                    if (in.op == cider::IROp::CALL && in.dest &&
                        in.dest->type == cider::IRType::F32 && in.args.size() == 2) {
                        sawF32Call = true;
                        if (in.args[1].type == cider::IRType::F32) sawF32Arg = true;
                    }
                }
            }
        }
        CHECK(sawFnSlot);
        CHECK(sawF32Call);
        CHECK(sawF32Arg);
    }

    // CID-006 fix round 1 (F1): `type A = B` alias CHAINS must be resolved
    // to a fixed point before the struct test. A two-hop chain ending in a
    // fn(...) target kept the STRUCT-kind alias symbol, so the local copy of
    // the value lowered to a MEMCPY from the value-as-address (native
    // SIGSEGV). This is the review repro verbatim.
    {
        auto module = genModuleSrc(
            "type B = fn(int) -> int;\n"
            "type A = B;\n"
            "func inc(a: int) -> int { return a + 1; }\n"
            "var g: A = inc;\n"
            "func main() -> int {\n"
            "  var f: fn(int) -> int = g;\n"
            "  return f(10) + 42;\n"
            "}\n");
        bool sawDq = false;
        for (const auto& line : module.getStringData()) {
            if (line.find("g: dq inc") != std::string::npos) sawDq = true;
        }
        CHECK(sawDq);
        bool sawMemcpy = false;
        bool sawFnSlot = false;
        bool sawIntCall = false;
        for (const auto& fn : module.getFunctions()) {
            for (const auto& block : fn->getBlocks()) {
                for (const auto& in : block->getInstructions()) {
                    if (in.op == cider::IROp::MEMCPY) sawMemcpy = true;
                    if (in.op == cider::IROp::ALLOCA && in.args.size() == 1 &&
                        in.args[0].kind == cider::IRValue::CONSTANT &&
                        in.args[0].intValue == 8 && in.dest &&
                        in.dest->type == cider::IRType::PTR) {
                        sawFnSlot = true;
                    }
                    if (in.op == cider::IROp::CALL && !in.args.empty() &&
                        in.args[0].kind == cider::IRValue::REGISTER && in.dest &&
                        in.dest->type == cider::IRType::I32) {
                        sawIntCall = true;
                    }
                }
            }
        }
        CHECK(!sawMemcpy);
        CHECK(sawFnSlot);
        CHECK(sawIntCall);
    }

    // CID-006 fix round 1 (F1): a non-fn two-hop alias chain behaves exactly
    // like the direct spelling — no by-value MEMCPY, 4-byte slot, plain
    // constant store. (The chain used to take the MEMCPY path and read a
    // garbage size off the alias symbol.)
    {
        auto chain = genModuleSrc(
            "type B = int;\n"
            "type A = B;\n"
            "func main() -> int {\n"
            "  var x: A = 5;\n"
            "  var y: int = 5;\n"
            "  return x + y;\n"
            "}\n");
        auto direct = genModuleSrc(
            "func main() -> int {\n"
            "  var x: int = 5;\n"
            "  var y: int = 5;\n"
            "  return x + y;\n"
            "}\n");
        auto irShape = [](const cider::ModuleIR& m) {
            std::string s;
            for (const auto& fn : m.getFunctions()) {
                s += fn->getName() + "{";
                for (const auto& block : fn->getBlocks()) {
                    s += block->getName() + ":";
                    for (const auto& in : block->getInstructions()) {
                        s += std::to_string((int)in.op) + "(";
                        if (in.dest) {
                            s += std::to_string((int)in.dest->kind) + "/" + in.dest->name + "/" +
                                 std::to_string((int)in.dest->type) + ";";
                        }
                        for (const auto& a : in.args) {
                            s += std::to_string((int)a.kind) + "/" + a.name + "/" +
                                 std::to_string(a.intValue) + "/" + std::to_string((int)a.type) + ",";
                        }
                        s += ")|";
                    }
                }
                s += "}";
            }
            return s;
        };
        CHECK(irShape(chain) == irShape(direct));
    }

    // CID-006 fix round 1 (F3 pin): an alias of `*void` initialized from a
    // function name reaches the `declAlias` branch and emits the real cell.
    {
        auto module = genModuleSrc(
            "func add(a: int, b: int) -> int { return a + b; }\n"
            "type PV = *void;\n"
            "var g: PV = add;\n"
            "func main() -> int { return 0; }\n");
        bool sawDq = false;
        for (const auto& line : module.getStringData()) {
            if (line.find("g: dq add") != std::string::npos) sawDq = true;
        }
        CHECK(sawDq);
    }

    // CID-006 fix round 2: checker-accepted cyclic aliases must terminate.
    // The depth cap in resolveTypeAlias returned the other alias, so
    // getTypeSize's re-resolution never converged (compiler stack overflow).
    // A mutual cycle, a lasso into a cycle, and a self-alias must all
    // compile to a deterministic 8-byte slot.
    {
        auto module = genModuleSrc(
            "type A = B;\n"
            "type B = A;\n"
            "type C = A;\n"
            "type Self = Self;\n"
            "func main() -> int {\n"
            "  var x: B;\n"
            "  var y: C;\n"
            "  var z: Self;\n"
            "  return 0;\n"
            "}\n");
        int allocas = 0;
        int sized8 = 0;
        for (const auto& fn : module.getFunctions()) {
            for (const auto& block : fn->getBlocks()) {
                for (const auto& in : block->getInstructions()) {
                    if (in.op == cider::IROp::ALLOCA && in.args.size() == 1 &&
                        in.args[0].kind == cider::IRValue::CONSTANT) {
                        allocas++;
                        if (in.args[0].intValue == 8) sized8++;
                    }
                }
            }
        }
        CHECK(allocas == 3);
        CHECK(sized8 == 3);
    }

    // CID-006 fix round 3: the mapType alias walk must terminate on cycles
    // too. Assignments, same-typed copies and parameter uses of a cyclic
    // alias used to recurse ~65k frames deep in mapType (compiler SIGSEGV)
    // instead of falling back to the unknown-type PTR.
    {
        auto module = genModuleSrc(
            "type A = B;\n"
            "type B = A;\n"
            "func use(v: B) -> int { return 0; }\n"
            "func main() -> int {\n"
            "  var x: B;\n"
            "  x = x;\n"
            "  var y: B = x;\n"
            "  return use(y);\n"
            "}\n");
        int allocas8 = 0;
        for (const auto& fn : module.getFunctions()) {
            for (const auto& block : fn->getBlocks()) {
                for (const auto& in : block->getInstructions()) {
                    if (in.op == cider::IROp::ALLOCA && in.args.size() == 1 &&
                        in.args[0].kind == cider::IRValue::CONSTANT &&
                        in.args[0].intValue == 8) {
                        allocas8++;
                    }
                }
            }
        }
        CHECK(allocas8 == 3);  // x, y and the parameter slot
    }
    // CID-005 fix round 1: definitions reached through mechanisms whose
    // call sites T3 pins to SysV (VIRTUAL_CALL, the interface branch, and
    // closure calls) must themselves be SysV under every target. An
    // unmarked interface implementation would otherwise emit an MS prologue
    // under `--target uefi` while every interface call marshals SysV.
    {
        auto module = genModuleSrc(
            "interface I {\n"
            "  func m() -> int;\n"
            "}\n"
            "class C {\n"
            "  virtual func v() -> int { return 1; }\n"
            "  func m() -> int { return 2; }\n"
            "}\n"
            "func makeClosure() -> func(int) -> int {\n"
            "  return func(x: int) -> int { return x; };\n"
            "}\n"
            "func main() -> int {\n"
            "  var f: func(int) -> int = makeClosure();\n"
            "  return f(0);\n"
            "}\n");
        bool sawVirtual = false;
        bool sawIfaceImpl = false;
        bool sawClosure = false;
        for (const auto& fn : module.getFunctions()) {
            if (fn->getName() == "C_v") {
                sawVirtual = true;
                CHECK(fn->getAbi() == cider::Abi::SysV);
            }
            if (fn->getName() == "C_m") {
                sawIfaceImpl = true;
                CHECK(fn->getAbi() == cider::Abi::SysV);
            }
            if (fn->getName().rfind("_closure_", 0) == 0) {
                sawClosure = true;
                CHECK(fn->getAbi() == cider::Abi::SysV);
            }
        }
        CHECK(sawVirtual);
        CHECK(sawIfaceImpl);
        CHECK(sawClosure);
    }

    // CID-005 fix round 1: closure FunctionIRs carry real parameter IR
    // types (hidden closure object PTR, typed params), not addParam's I32
    // default the MS/SysV classifiers would misread.
    {
        auto module = genModuleSrc(
            "func makeClosure() -> func(f64) -> f64 {\n"
            "  return func(x: f64) -> f64 { return x; };\n"
            "}\n"
            "func main() -> int {\n"
            "  var f: func(f64) -> f64 = makeClosure();\n"
            "  return f(1.0) as int;\n"
            "}\n");
        bool sawClosure = false;
        for (const auto& fn : module.getFunctions()) {
            if (fn->getName().rfind("_closure_", 0) != 0) continue;
            sawClosure = true;
            const auto& pt = fn->getParamTypes();
            CHECK(pt.size() == 2);
            CHECK(pt[0] == cider::IRType::PTR);
            CHECK(pt[1] == cider::IRType::F64);
        }
        CHECK(sawClosure);
    }

    // CID-005 fix round 1: an interface implementation inherited from a
    // parent class is reached through the child's interface vtable; the
    // parent's definition must be pinned too (the analysis resolves the
    // inherited mangled name, not just the child's own methods).
    {
        auto module = genModuleSrc(
            "interface I {\n"
            "  func a() -> int;\n"
            "  func b() -> int;\n"
            "}\n"
            "class Base {\n"
            "  func a() -> int { return 1; }\n"
            "}\n"
            "class Derived extends Base {\n"
            "  func b() -> int { return 2; }\n"
            "}\n"
            "func main() -> int { return 0; }\n");
        bool sawBaseA = false;
        bool sawDerivedB = false;
        for (const auto& fn : module.getFunctions()) {
            if (fn->getName() == "Base_a") {
                sawBaseA = true;
                CHECK(fn->getAbi() == cider::Abi::SysV);
            }
            if (fn->getName() == "Derived_b") {
                sawDerivedB = true;
                CHECK(fn->getAbi() == cider::Abi::SysV);
            }
        }
        CHECK(sawBaseA);
        CHECK(sawDerivedB);
    }

    // CID-005 fix round 1: the FunctionSymbols T3's direct-call lowering
    // reads must agree with the pinned definitions, or a direct call to an
    // interface implementation / virtual method would marshal MS under uefi
    // against a SysV prologue.
    {
        cider::ErrorReporter rep;
        cider::Lexer lex(
            "interface I {\n"
            "  func m() -> int;\n"
            "}\n"
            "class C {\n"
            "  virtual func v() -> int { return 1; }\n"
            "  func m() -> int { return 2; }\n"
            "}\n"
            "func main() -> int { return 0; }\n",
            rep);
        cider::Parser parser(lex, rep);
        auto prog = parser.parseProgram();
        cider::SymbolTable table;
        cider::ModuleResolver resolver;
        cider::SymbolCollector collector(table, rep, resolver);
        collector.collect(*prog);
        cider::IRGenerator gen(table);
        gen.generate(*prog);
        auto symOf = [&](const std::string& name)
            -> std::shared_ptr<cider::FunctionSymbol> {
            auto s = table.resolve(name);
            return (s && s->kind == cider::SymbolKind::FUNCTION)
                       ? std::static_pointer_cast<cider::FunctionSymbol>(s)
                       : nullptr;
        };
        auto cv = symOf("C_v");
        CHECK(cv && cv->abi == cider::Abi::SysV);
        auto cm = symOf("C_m");
        CHECK(cm && cm->abi == cider::Abi::SysV);
    }

    // CID-005 T3: every CallExpr lowering records its calling convention.
    // A direct call carries the callee FunctionSymbol's ABI (`Default` when
    // unmarked; codegen resolves that from the target or from the pinned
    // definition). An indirect call through a typed fn value carries the
    // type's ABI (`Ms` iff `isMsAbi`). T4 supplies the real `msabi`
    // spellings, so both the symbol and the value type come from source.
    {
        cider::ErrorReporter rep;
        cider::Lexer lex(
            "msabi func f(x: int) -> int { return x; }\n"
            "var gp: msabi fn(int) -> int = f;\n"
            "func main() -> int { return f(2) + gp(3); }\n",
            rep);
        cider::Parser parser(lex, rep);
        auto prog = parser.parseProgram();
        cider::SymbolTable table;
        cider::ModuleResolver resolver;
        cider::SymbolCollector collector(table, rep, resolver);
        collector.collect(*prog);
        cider::IRGenerator gen(table);
        auto module = gen.generate(*prog);
        bool sawMsDirect = false, sawMsIndirect = false;
        for (const auto& fn : module.getFunctions()) {
            for (const auto& block : fn->getBlocks()) {
                for (const auto& in : block->getInstructions()) {
                    if (in.op != cider::IROp::CALL) continue;
                    if (in.args[0].kind == cider::IRValue::LABEL) {
                        if (in.callAbi == cider::Abi::Ms) sawMsDirect = true;
                    } else if (in.callAbi == cider::Abi::Ms) {
                        sawMsIndirect = true;
                    }
                }
            }
        }
        CHECK(sawMsDirect);
        CHECK(sawMsIndirect);
    }

    // CID-005: an instantiated `msabi` template's fast-path symbol carries
    // the template ABI, so direct calls marshal MS against the clone's MS
    // definition (the clone's own collector visit is first-wins and skips
    // this symbol).
    {
        cider::ErrorReporter rep;
        cider::Lexer lex(
            "msabi func id<T>(x: T) -> T { return x; }\n"
            "func main() -> int { return id(7); }\n",
            rep);
        cider::Parser parser(lex, rep);
        auto prog = parser.parseProgram();
        cider::SymbolTable table;
        cider::ModuleResolver resolver;
        cider::SymbolCollector collector(table, rep, resolver);
        collector.collect(*prog);
        auto sym = table.resolve("id<int>");
        CHECK(sym && sym->kind == cider::SymbolKind::FUNCTION);
        auto fsym = std::static_pointer_cast<cider::FunctionSymbol>(sym);
        CHECK(fsym->abi == cider::Abi::Ms);
    }

    // CID-005 T3: unmarked direct calls carry Default (resolved by codegen);
    // indirect calls through plain fn values, closures, interface dispatch
    // and virtual dispatch carry SysV, matching the definitions T1 pinned.
    {
        auto module = genModuleSrc(
            "interface I { func m() -> int; }\n"
            "class C {\n"
            "  virtual func v() -> int { return 1; }\n"
            "  func m() -> int { return 2; }\n"
            "}\n"
            "func id(x: int) -> int { return x; }\n"
            "func makeClosure() -> func(int) -> int {\n"
            "  return func(x: int) -> int { return x; };\n"
            "}\n"
            "func callVirt(c: C) -> int { return c.v(); }\n"
            "func callIface() -> int {\n"
            "  var c: C;\n"
            "  var i: I = c;\n"
            "  return i.m();\n"
            "}\n"
            "func main() -> int {\n"
            "  var p: fn(int) -> int = id;\n"
            "  var q: func(int) -> int = makeClosure();\n"
            "  return id(5) + p(3) + q(4);\n"
            "}\n");
        bool sawDefaultDirect = false, sawSysvIndirect = false;
        bool sawVirtual = false, sawIface = false;
        for (const auto& fn : module.getFunctions()) {
            for (const auto& block : fn->getBlocks()) {
                for (const auto& in : block->getInstructions()) {
                    if (in.op == cider::IROp::VIRTUAL_CALL) {
                        sawVirtual = true;
                        CHECK(in.callAbi == cider::Abi::SysV);
                        continue;
                    }
                    if (in.op != cider::IROp::CALL) continue;
                    if (in.args[0].kind == cider::IRValue::LABEL) {
                        sawDefaultDirect = true;
                        CHECK(in.callAbi == cider::Abi::Default);
                    } else {
                        sawSysvIndirect = true;
                        CHECK(in.callAbi == cider::Abi::SysV);
                        if (fn->getName() == "callIface") sawIface = true;
                    }
                }
            }
        }
        CHECK(sawDefaultDirect);
        CHECK(sawSysvIndirect);
        CHECK(sawVirtual);
        CHECK(sawIface);
    }
    return 0;
}

int main() { return run(); }
