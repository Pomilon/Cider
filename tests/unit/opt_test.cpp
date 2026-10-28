// opt_test.cpp — malformed IR is refused, and passes preserve meaning.
//
// IR.md §7 lists what every pass must preserve, and each row below pins one:
// a pass may not delete a trapping instruction, move a volatile one, change
// overflow, or drop a source location. The structural check runs before any
// pass — gate L4 — so no pass ever runs on a malformed module.

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "check/checker.h"
#include "ir/ir.h"
#include "lexer/lexer.h"
#include "lower/lower.h"
#include "opt/opt.h"
#include "opt/structural.h"
#include "parse/parser.h"
#include "types/context.h"

namespace {

int failures = 0;
int checks = 0;

void check(bool holds, const std::string& what) {
    ++checks;
    if (holds) {
        std::printf("  ok   %s\n", what.c_str());
    } else {
        std::printf("  FAIL %s\n", what.c_str());
        ++failures;
    }
}

struct Piped {
    bool ok = false;
    std::string stage;
    std::unique_ptr<cider::TypeContext> types;
    std::unique_ptr<cider::ir::Module> module;
    cider::opt::OptResult opt;
};

// modules/std/varargs.cid, embedded: a pipeline test imports the public
// surface without depending on a working directory (VARIADICS.md §5).
const char* kVarargsSource =
    "export type VaList = va_list;\n"
    "\n"
    "extern func va_start(ap: *VaList) -> void;\n"
    "extern func va_copy(dst: *VaList, src: *VaList) -> void;\n"
    "extern func va_end(ap: *VaList) -> void;\n"
    "\n"
    "extern func va_arg_i32(ap: *VaList) -> i32;\n"
    "extern func va_arg_i64(ap: *VaList) -> i64;\n"
    "extern func va_arg_u64(ap: *VaList) -> u64;\n"
    "extern func va_arg_f64(ap: *VaList) -> f64;\n"
    "extern func va_arg_ptr(ap: *VaList) -> *void;\n";

cider::resolve::Resolver::ModuleLoader varargsLoader() {
    return [](const std::string& name) -> std::unique_ptr<cider::Program> {
        if (name != "std.varargs") {
            return nullptr;
        }
        cider::Lexer lexer("varargs.cid", kVarargsSource);
        cider::Parser parser("varargs.cid", lexer.lex());
        return parser.parse();
    };
}

Piped pipe(const std::string& text) {
    Piped out;
    out.types = std::make_unique<cider::TypeContext>();
    cider::Lexer lexer("test.cid", text);
    std::vector<cider::Token> tokens = lexer.lex();
    if (!lexer.errors().empty()) {
        out.stage = "lex";
        return out;
    }
    cider::Parser parser("test.cid", std::move(tokens));
    std::unique_ptr<cider::Program> program = parser.parse();
    if (!parser.ok()) {
        out.stage = "parse";
        return out;
    }
    auto resolver = std::make_unique<cider::resolve::Resolver>(*out.types,
                                                               varargsLoader());
    if (!resolver->resolveProgram(*program, "main")) {
        out.stage = "resolve";
        return out;
    }
    auto checker = std::make_unique<cider::check::Checker>(*out.types, *resolver);
    cider::lower::Lowerer lowerer(*out.types, *resolver, *checker);
    if (!lowerer.lowerModule("main", *program)) {
        out.module = nullptr;
        out.stage = "lower";
        return out;
    }
    out.module = lowerer.takeModule();
    out.opt = cider::opt::run(*out.module);
    if (!out.opt.ok) {
        out.stage = "opt:" + out.opt.error;
        return out;
    }
    out.ok = true;
    return out;
}

int countOp(const cider::ir::Function& fn, cider::ir::Op op) {
    int n = 0;
    for (const cider::ir::Block& block : fn.blocks) {
        for (const cider::ir::Instr& instr : block.body) {
            if (instr.op == op) {
                ++n;
            }
        }
    }
    return n;
}

const cider::ir::Function* find(const cider::ir::Module* module, const std::string& name) {
    if (module == nullptr) {
        return nullptr;
    }
    for (const cider::ir::Function& fn : module->functions) {
        if (fn.name == name) {
            return &fn;
        }
    }
    return nullptr;
}

const cider::ir::Instr* firstInstr(const cider::ir::Function& fn,
                                   cider::ir::Op op) {
    for (const cider::ir::Block& block : fn.blocks) {
        for (const cider::ir::Instr& instr : block.body) {
            if (instr.op == op) {
                return &instr;
            }
        }
    }
    return nullptr;
}

// A small hand-built function for the slot pass. Source lowering leaves a
// Store's width unset for scalar locals, so the helpers let each test choose
// the exact width/type pair it wants to exercise.
struct Ir {
    cider::TypeContext types;
    cider::ir::Module module;
    cider::ir::Function* fn = nullptr;

    Ir() {
        module.name = "test";
        cider::ir::Function function;
        function.name = "f";
        cider::ir::Block block;
        block.label = 0;
        function.blocks.push_back(block);
        module.functions.push_back(std::move(function));
        fn = &module.functions.back();
    }

    cider::TypeRef i32() { return cider::TypeRef{types.basic(cider::BasicKind::I32)}; }
    cider::TypeRef u32() { return cider::TypeRef{types.basic(cider::BasicKind::U32)}; }
    cider::TypeRef i64() { return cider::TypeRef{types.basic(cider::BasicKind::I64)}; }
    cider::TypeRef boolean() { return cider::TypeRef{types.basic(cider::BasicKind::Bool)}; }
    cider::TypeRef ptr() {
        return cider::TypeRef{types.pointer(types.basic(cider::BasicKind::U8))};
    }

    cider::ir::Value value(uint32_t id, cider::ir::IRType width,
                           cider::TypeRef type) {
        cider::ir::Value out;
        out.id = id;
        out.width = width;
        out.type = type;
        return out;
    }

    void emit(cider::ir::Instr instr) {
        fn->blocks.back().body.push_back(std::move(instr));
    }

    void addBlock(uint32_t label) {
        cider::ir::Block block;
        block.label = label;
        fn->blocks.push_back(std::move(block));
    }

    cider::ir::Instr allocaSlot(uint32_t id, cider::TypeRef type) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Alloca;
        instr.width = cider::ir::IRType::Ptr;
        instr.type = cider::TypeRef{types.pointer(type.type)};
        instr.results.push_back(value(id, instr.width, instr.type));
        return instr;
    }

    cider::ir::Instr constant(uint32_t id, int64_t bits,
                              cider::ir::IRType width, cider::TypeRef type) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Const;
        instr.width = width;
        instr.type = type;
        instr.constantBits = bits;
        instr.constantText = std::to_string(bits);
        instr.isPure = true;
        instr.results.push_back(value(id, width, type));
        return instr;
    }

    cider::ir::Instr store(uint32_t base, cider::ir::Value stored,
                           cider::ir::IRType width, cider::TypeRef type,
                           bool isVolatile = false) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Store;
        instr.width = width;
        instr.type = type;
        instr.addressBase = value(base, cider::ir::IRType::Ptr, ptr());
        instr.storedValue = stored;
        instr.isVolatile = isVolatile;
        return instr;
    }

    cider::ir::Instr load(uint32_t result, uint32_t base,
                          cider::ir::IRType width, cider::TypeRef type,
                          bool isVolatile = false) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Load;
        instr.width = width;
        instr.type = type;
        instr.addressBase = value(base, cider::ir::IRType::Ptr, ptr());
        instr.isPure = true;
        instr.isVolatile = isVolatile;
        instr.results.push_back(value(result, width, type));
        return instr;
    }

    cider::ir::Instr add(uint32_t result, cider::ir::Value lhs,
                         cider::ir::Value rhs, cider::TypeRef type) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Add;
        instr.width = cider::ir::IRType::I32;
        instr.type = type;
        instr.isPure = true;
        instr.results.push_back(value(result, instr.width, type));
        instr.operands.push_back(lhs);
        instr.operands.push_back(rhs);
        return instr;
    }

    cider::ir::Instr phi(uint32_t result, cider::ir::IRType width,
                         cider::TypeRef type,
                         const std::vector<std::pair<uint32_t, uint32_t>>& in) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Phi;
        instr.width = width;
        instr.type = type;
        instr.isPure = true;
        instr.results.push_back(value(result, width, type));
        for (const auto& pair : in) {
            cider::ir::PhiIncoming incoming;
            incoming.value = value(pair.first, width, type);
            incoming.block = pair.second;
            instr.phiIncoming.push_back(incoming);
        }
        return instr;
    }

    cider::ir::Instr gep(uint32_t result, uint32_t base,
                         cider::ir::Value offset) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Gep;
        instr.width = cider::ir::IRType::Ptr;
        instr.type = ptr();
        instr.addressBase = value(base, cider::ir::IRType::Ptr, ptr());
        instr.isPure = true;
        instr.results.push_back(value(result, instr.width, instr.type));
        instr.operands.push_back(offset);
        return instr;
    }

    cider::ir::Instr call(const std::string& callee) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Call;
        instr.width = cider::ir::IRType::Void;
        instr.callee = callee;
        instr.mayTrap = true;
        return instr;
    }

    cider::ir::Instr callWith(const std::string& callee,
                              cider::ir::Value word) {
        cider::ir::Instr instr = call(callee);
        cider::ir::Argument arg;
        arg.words.push_back(word);
        arg.width = word.width;
        instr.arguments.push_back(std::move(arg));
        return instr;
    }

    cider::ir::Instr asmBlock() {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Asm;
        instr.width = cider::ir::IRType::Void;
        instr.asmText = "nop";
        return instr;
    }

    void finishRet(size_t block, uint32_t id, cider::ir::IRType width,
                   cider::TypeRef type) {
        cider::ir::Terminator term;
        term.term = cider::ir::Term::Ret;
        term.values.push_back(value(id, width, type));
        fn->blocks[block].term = term;
    }

    void finishBr(size_t block, uint32_t target) {
        cider::ir::Terminator term;
        term.term = cider::ir::Term::Br;
        term.target = target;
        fn->blocks[block].term = term;
    }

    void finishCondBr(size_t block, uint32_t condId, uint32_t thenTarget,
                      uint32_t elseTarget) {
        cider::ir::Terminator term;
        term.term = cider::ir::Term::CondBr;
        term.cond = value(condId, cider::ir::IRType::Bool, boolean());
        term.thenTarget = thenTarget;
        term.elseTarget = elseTarget;
        fn->blocks[block].term = term;
    }
};

// Well-formed modules pass the structural check, with no diagnostics.
void testWellFormed() {
    std::printf("well-formed modules pass\n");
    Piped r = pipe("func f(a: int, b: int) -> int { return a + b; }\n");
    check(r.ok, "checks: " + r.stage);
}

// Constants fold: `1 + 2` becomes one const, and the add is gone.
void testFold() {
    std::printf("constants fold\n");
    Piped r = pipe("func f() -> int { return 1 + 2 * 3; }\n");
    check(r.ok, "lowers and optimises: " + r.stage);
    const cider::ir::Function* fn = find(r.module.get(), "f");
    check(fn != nullptr, "the function is there");
    if (fn == nullptr) {
        return;
    }
    check(countOp(*fn, cider::ir::Op::Add) == 0, "no add remains");
    check(countOp(*fn, cider::ir::Op::Mul) == 0, "no mul remains");
    check(countOp(*fn, cider::ir::Op::Const) == 1, "one const remains");
}

// An indirect call keeps its callee chain: the address, the store, and the
// load all survive, because the call reads its target like an operand.
void testIndirectKept() {
    std::printf("indirect callees survive\n");
    Piped r = pipe(
        "func add(a: int, b: int) -> int { return a + b; }\n"
        "func main() -> int {\n"
        "    var f: *func(int, int) -> int = &add;\n"
        "    return (*f)(20, 22);\n"
        "}\n");
    check(r.ok, "lowers and optimises: " + r.stage);
    const cider::ir::Function* fn = find(r.module.get(), "main");
    check(fn != nullptr && countOp(*fn, cider::ir::Op::FnAddr) == 1,
          "the address survives");
    // Slot forwarding may replace the local's load and delete its store, but
    // the indirect call must still name the FnAddr value itself. Deleting the
    // address instead was the v3 bug this test was born from.
    const cider::ir::Instr* address = fn != nullptr
        ? firstInstr(*fn, cider::ir::Op::FnAddr) : nullptr;
    const cider::ir::Instr* call = fn != nullptr
        ? firstInstr(*fn, cider::ir::Op::Call) : nullptr;
    check(address != nullptr && call != nullptr && call->call.indirect &&
              !address->results.empty() &&
              call->calleeValue.id == address->results[0].id,
          "the indirect call keeps the address");
}

// An unused pure definition is deleted.
void testDeadCode() {
    std::printf("dead code is deleted\n");
    Piped r = pipe("func f(a: int) -> int { var t: int = a + 1; return a; }\n");
    check(r.ok, "lowers and optimises: " + r.stage);
    const cider::ir::Function* fn = find(r.module.get(), "f");
    check(fn != nullptr && countOp(*fn, cider::ir::Op::Add) == 0, "the dead add is gone");
}

// A division that may trap is never deleted, used or not.
void testTrapKept() {
    std::printf("a trapping instruction is never deleted\n");
    Piped r = pipe("func f(a: int, b: int) -> int { var t: int = a / b; return a; }\n");
    check(r.ok, "lowers and optimises: " + r.stage);
    const cider::ir::Function* fn = find(r.module.get(), "f");
    check(fn != nullptr && countOp(*fn, cider::ir::Op::Div) == 1, "the div stays");
}

// A dangling branch target names its block.
void testDanglingTarget() {
    std::printf("dangling targets name their block\n");
    cider::ir::Module module;
    module.name = "bad";
    cider::ir::Function fn;
    fn.name = "f";
    cider::ir::Block block;
    block.label = 0;
    cider::ir::Terminator term;
    term.term = cider::ir::Term::Br;
    term.target = 99;
    block.term = term;
    fn.blocks.push_back(block);
    module.functions.push_back(fn);
    std::vector<cider::opt::StructuralError> errors = cider::opt::checkModule(module);
    check(errors.size() == 1, "one diagnostic");
    check(!errors.empty() && errors[0].message.find("99") != std::string::npos,
          "naming the missing block");
}

// A block with no terminator names its block: the default Terminator is
// Unreachable, which is also the shape a lowering bug makes when it forgets
// to terminate a block (gate L4, BUG-260).
void testUnterminatedBlock() {
    std::printf("unterminated blocks name their block\n");
    cider::ir::Module module;
    module.name = "bad";
    cider::ir::Function fn;
    fn.name = "f";
    cider::ir::Block block;
    block.label = 3;
    fn.blocks.push_back(block);
    module.functions.push_back(fn);
    std::vector<cider::opt::StructuralError> errors = cider::opt::checkModule(module);
    check(errors.size() == 1, "one diagnostic");
    check(!errors.empty() && errors[0].message.find("3") != std::string::npos,
          "naming the unterminated block");
}

// An operand with no definition names the value.
void testUndefinedOperand() {
    std::printf("undefined operands name the value\n");
    cider::ir::Module module;
    module.name = "bad";
    cider::ir::Function fn;
    fn.name = "f";
    cider::ir::Block block;
    block.label = 0;
    cider::ir::Instr instr;
    instr.op = cider::ir::Op::Add;
    instr.width = cider::ir::IRType::I32;
    instr.isPure = true;
    cider::ir::Value result;
    result.id = 7;
    result.width = cider::ir::IRType::I32;
    instr.results.push_back(result);
    cider::ir::Value ghost;
    ghost.id = 4242;
    ghost.width = cider::ir::IRType::I32;
    instr.operands.push_back(ghost);
    instr.operands.push_back(ghost);
    block.body.push_back(instr);
    cider::ir::Terminator term;
    term.term = cider::ir::Term::Ret;
    term.values.push_back(result);
    block.term = term;
    fn.blocks.push_back(block);
    module.functions.push_back(fn);
    std::vector<cider::opt::StructuralError> errors = cider::opt::checkModule(module);
    check(!errors.empty(), "a diagnostic");
}

// A value defined twice names the value.
void testDoubleDefine() {
    std::printf("a value defined twice names the value\n");
    cider::ir::Module module;
    module.name = "bad";
    cider::ir::Function fn;
    fn.name = "f";
    cider::ir::Block block;
    block.label = 0;
    for (int i = 0; i < 2; ++i) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Const;
        instr.width = cider::ir::IRType::I32;
        instr.isPure = true;
        cider::ir::Value result;
        result.id = 3;
        result.width = cider::ir::IRType::I32;
        instr.results.push_back(result);
        block.body.push_back(instr);
    }
    cider::ir::Terminator term;
    term.term = cider::ir::Term::Unreachable;
    block.term = term;
    fn.blocks.push_back(block);
    module.functions.push_back(fn);
    std::vector<cider::opt::StructuralError> errors = cider::opt::checkModule(module);
    check(!errors.empty(), "a diagnostic");
}

// The pipeline refuses a malformed module before any pass runs on it.
void testPipelineRefuses() {
    std::printf("the pipeline refuses malformed modules\n");
    cider::ir::Module module;
    module.name = "bad";
    cider::ir::Function fn;
    fn.name = "f";
    cider::ir::Block block;
    block.label = 0;
    cider::ir::Terminator term;
    term.term = cider::ir::Term::Br;
    term.target = 99;
    block.term = term;
    fn.blocks.push_back(block);
    module.functions.push_back(fn);
    cider::opt::OptResult result = cider::opt::run(module);
    check(!result.ok, "run fails");
    check(!result.error.empty(), "with a reason");
}

// Narrow constants fold at their own width: `2000000000 + 2000000000`
// is an i32 wrap, not an i64 sum, and a 32-bit MIN/-1 keeps its
// division for the hardware trap instead of folding.
void testFoldWidths() {
    std::printf("narrow constants fold at their width\n");
    Piped r = pipe("func f() -> int { return 2000000000 + 2000000000; }\n");
    check(r.ok, "lowers and optimises: " + r.stage);
    const cider::ir::Function* fn = find(r.module.get(), "f");
    check(fn != nullptr, "the function is there");
    if (fn != nullptr) {
        check(countOp(*fn, cider::ir::Op::Add) == 0, "no add remains");
        int64_t folded = 0;
        bool seen = false;
        for (const cider::ir::Block& block : fn->blocks) {
            for (const cider::ir::Instr& instr : block.body) {
                if (instr.op == cider::ir::Op::Const && !instr.results.empty()) {
                    folded = instr.constantBits;
                    seen = true;
                }
            }
        }
        check(seen && folded == -294967296, "the const wraps at 32 bits");
    }
    Piped t = pipe(
        "func f(a: int) -> int {\n"
        "    var m: int = -2147483648;\n"
        "    return m / -1;\n"
        "}\n");
    check(t.ok, "lowers and optimises: " + t.stage);
    const cider::ir::Function* div = find(t.module.get(), "f");
    check(div != nullptr && countOp(*div, cider::ir::Op::Div) == 1,
          "a 32-bit MIN/-1 keeps its division");

    // mem2reg exposes u64 constants to the folder. A right shift of a u64 is
    // logical, and a u64 division is unsigned; folding either as signed gave
    // the wrong answer once the stack traffic was gone.
    Piped shifted = pipe(
        "func f() -> u64 {\n"
        "    var x: u64 = 0xFFFF800000000000;\n"
        "    return x >> 32;\n"
        "}\n");
    check(shifted.ok, "lowers and optimises: " + shifted.stage);
    const cider::ir::Function* shr = find(shifted.module.get(), "f");
    check(shr != nullptr && countOp(*shr, cider::ir::Op::Shr) == 0,
          "the u64 shift folded");
    bool shiftedValue = false;
    if (shr != nullptr) {
        for (const cider::ir::Block& block : shr->blocks) {
            for (const cider::ir::Instr& instr : block.body) {
                if (instr.op == cider::ir::Op::Const &&
                    instr.constantBits == 0xFFFF8000) {
                    shiftedValue = true;
                }
            }
        }
    }
    check(shiftedValue, "the u64 shift is logical");
    Piped divided = pipe(
        "func f() -> u64 { return 0xFFFFFFFFFFFFFFFF / 3; }\n");
    check(divided.ok, "lowers and optimises: " + divided.stage);
    const cider::ir::Function* dv = find(divided.module.get(), "f");
    check(dv != nullptr && countOp(*dv, cider::ir::Op::Div) == 0,
          "the u64 division folded");
}

// An if/else join becomes a phi and the slot traffic disappears.
void testMem2RegJoin() {
    std::printf("mem2reg builds a phi at an if/else join\n");
    Piped r = pipe(
        "func f(c: bool, a: int, b: int) -> int {\n"
        "    var x: int = 0;\n"
        "    if (c) { x = a; } else { x = b; }\n"
        "    return x + 1;\n"
        "}\n");
    check(r.ok, "lowers and optimises: " + r.stage);
    const cider::ir::Function* fn = find(r.module.get(), "f");
    check(fn != nullptr, "the function is there");
    if (fn == nullptr) {
        return;
    }
    check(r.opt.promoted >= 4, "every scalar slot was promoted");
    check(r.opt.phis >= 1, "the join has a phi");
    check(countOp(*fn, cider::ir::Op::Phi) >= 1, "the phi is in the IR");
    check(countOp(*fn, cider::ir::Op::Alloca) == 0, "no Alloca remains");
    check(countOp(*fn, cider::ir::Op::Load) == 0, "no Load remains");
    check(countOp(*fn, cider::ir::Op::Store) == 0, "no Store remains");
}

// Nested if/else joins produce phis at each join.
void testMem2RegNestedJoins() {
    std::printf("mem2reg handles nested joins\n");
    Piped r = pipe(
        "func f(c: bool, d: bool, a: int, b: int) -> int {\n"
        "    var x: int = 0;\n"
        "    if (c) {\n"
        "        if (d) { x = a; } else { x = b; }\n"
        "    } else {\n"
        "        if (d) { x = a + 1; } else { x = b + 1; }\n"
        "    }\n"
        "    return x;\n"
        "}\n");
    check(r.ok, "lowers and optimises: " + r.stage);
    const cider::ir::Function* fn = find(r.module.get(), "f");
    check(fn != nullptr, "the function is there");
    if (fn == nullptr) {
        return;
    }
    check(r.opt.phis >= 3, "each join has a phi");
    check(countOp(*fn, cider::ir::Op::Alloca) == 0, "no Alloca remains");
    check(countOp(*fn, cider::ir::Op::Load) == 0, "no Load remains");
    check(countOp(*fn, cider::ir::Op::Store) == 0, "no Store remains");
}

// A loop header and its back edge use real phis.
void testMem2RegLoop() {
    std::printf("mem2reg builds loop-header phis\n");
    Piped r = pipe(
        "func f(n: int) -> int {\n"
        "    var s: int = 0;\n"
        "    var i: int = 0;\n"
        "    while (i < n) { s = s + i; i = i + 1; }\n"
        "    return s;\n"
        "}\n");
    check(r.ok, "lowers and optimises: " + r.stage);
    const cider::ir::Function* fn = find(r.module.get(), "f");
    check(fn != nullptr, "the function is there");
    if (fn == nullptr) {
        return;
    }
    check(r.opt.phis >= 2, "the accumulator and induction variable have phis");
    check(countOp(*fn, cider::ir::Op::Alloca) == 0, "no Alloca remains");
    check(countOp(*fn, cider::ir::Op::Load) == 0, "no Load remains");
    check(countOp(*fn, cider::ir::Op::Store) == 0, "no Store remains");
}

// Repeated definitions in one path need no phi.
void testMem2RegMultipleDefs() {
    std::printf("mem2reg handles repeated definitions\n");
    Piped r = pipe(
        "func f(a: int) -> int {\n"
        "    var x: int = a;\n"
        "    x = 1;\n"
        "    x = 2;\n"
        "    return x;\n"
        "}\n");
    check(r.ok, "lowers and optimises: " + r.stage);
    const cider::ir::Function* fn = find(r.module.get(), "f");
    check(fn != nullptr, "the function is there");
    if (fn == nullptr) {
        return;
    }
    check(r.opt.promoted == 2, "the parameter and local are promoted");
    check(r.opt.phis == 0, "a straight path needs no phi");
    check(countOp(*fn, cider::ir::Op::Alloca) == 0, "no Alloca remains");
    check(countOp(*fn, cider::ir::Op::Load) == 0, "no Load remains");
    check(countOp(*fn, cider::ir::Op::Store) == 0, "no Store remains");
}

// A parameter slot is initialized by the ABI; its loads become the parameter
// value itself and the parameter metadata becomes the value's type.
void testMem2RegParameter() {
    std::printf("mem2reg promotes parameter slots\n");
    Piped r = pipe(
        "func f(a: int) -> int {\n"
        "    a = a + 1;\n"
        "    return a;\n"
        "}\n");
    check(r.ok, "lowers and optimises: " + r.stage);
    const cider::ir::Function* fn = find(r.module.get(), "f");
    check(fn != nullptr, "the function is there");
    if (fn == nullptr) {
        return;
    }
    check(r.opt.promoted == 1, "the parameter slot was promoted");
    check(r.opt.phis == 0, "no phi is needed");
    check(countOp(*fn, cider::ir::Op::Alloca) == 0, "no Alloca remains");
    check(countOp(*fn, cider::ir::Op::Load) == 0, "no Load remains");
    check(countOp(*fn, cider::ir::Op::Store) == 0, "no Store remains");
    check(fn->params.size() == 1 &&
              fn->params[0].width == cider::ir::IRType::I32 &&
              fn->params[0].type.type != nullptr &&
              fn->params[0].type.type->kind() == cider::Kind::Basic,
          "the parameter value carries the scalar type");
}

// The inliner now removes this small call before mem2reg; the scalar slots
// around it still promote, so the pipeline result is a call-free function.
void testMem2RegCall() {
    std::printf("mem2reg promotes around a call the inliner removes\n");
    Piped r = pipe(
        "func g(a: int) -> int { return a + 1; }\n"
        "func f(a: int) -> int {\n"
        "    var x: int = g(a);\n"
        "    x = x + 1;\n"
        "    return x;\n"
        "}\n");
    check(r.ok, "lowers and optimises: " + r.stage);
    const cider::ir::Function* fn = find(r.module.get(), "f");
    check(fn != nullptr, "the function is there");
    if (fn == nullptr) {
        return;
    }
    check(r.opt.inlined == 1, "the small call was inlined");
    check(r.opt.promoted >= 2, "the parameter and local are promoted");
    check(countOp(*fn, cider::ir::Op::Call) == 0, "the call is gone");
    check(countOp(*fn, cider::ir::Op::Alloca) == 0, "no Alloca remains");
    check(countOp(*fn, cider::ir::Op::Load) == 0, "no Load remains");
    check(countOp(*fn, cider::ir::Op::Store) == 0, "no Store remains");
}

// Address-taking escapes the frame; nothing in the function is promoted.
void testMem2RegEscape() {
    std::printf("mem2reg leaves an escaped frame in memory\n");
    Piped r = pipe(
        "func f(a: int) -> int {\n"
        "    var x: int = a;\n"
        "    var p: *int = &x;\n"
        "    *p = 2;\n"
        "    return x;\n"
        "}\n");
    check(r.ok, "lowers and optimises: " + r.stage);
    const cider::ir::Function* fn = find(r.module.get(), "f");
    check(fn != nullptr, "the function is there");
    if (fn == nullptr) {
        return;
    }
    check(r.opt.promoted == 0, "the escaped frame is not promoted");
    check(countOp(*fn, cider::ir::Op::Alloca) >= 1, "the Alloca stays");
    check(countOp(*fn, cider::ir::Op::Load) >= 1, "the load stays");
    check(countOp(*fn, cider::ir::Op::Store) >= 1, "the store stays");
}

// A function containing asm is skipped even when its locals look direct.
void testMem2RegAsm() {
    std::printf("mem2reg skips a function containing asm\n");
    Piped r = pipe(
        "func f(a: int) -> int {\n"
        "    var x: int = a;\n"
        "    asm { \"nop\" }\n"
        "    return x;\n"
        "}\n");
    check(r.ok, "lowers and optimises: " + r.stage);
    const cider::ir::Function* fn = find(r.module.get(), "f");
    check(fn != nullptr, "the function is there");
    if (fn == nullptr) {
        return;
    }
    check(r.opt.promoted == 0, "the asm function is not promoted");
    check(countOp(*fn, cider::ir::Op::Alloca) >= 1, "the Alloca stays");
    check(countOp(*fn, cider::ir::Op::Load) >= 1, "the load stays");
    check(countOp(*fn, cider::ir::Op::Store) >= 1, "the store stays");
}

// `= uninit` stores nothing. A load that can see that state stays a memory
// read; promotion never invents a zero.
void testMem2RegUninit() {
    std::printf("mem2reg preserves uninitialized loads\n");
    Piped r = pipe(
        "func f() -> int {\n"
        "    var x: int = uninit;\n"
        "    return x;\n"
        "}\n");
    check(r.ok, "lowers and optimises: " + r.stage);
    const cider::ir::Function* fn = find(r.module.get(), "f");
    check(fn != nullptr, "the function is there");
    if (fn == nullptr) {
        return;
    }
    check(r.opt.promoted == 0, "the uninitialized slot is not promoted");
    check(countOp(*fn, cider::ir::Op::Alloca) == 1, "the Alloca stays");
    check(countOp(*fn, cider::ir::Op::Load) == 1, "the uninitialized load stays");
    check(countOp(*fn, cider::ir::Op::Phi) == 0, "no value is invented");
}

void testMem2RegUninitBranch() {
    std::printf("mem2reg keeps a partially initialized slot in memory\n");
    Piped r = pipe(
        "func f(c: bool) -> int {\n"
        "    var x: int = uninit;\n"
        "    if (c) { x = 1; }\n"
        "    return x;\n"
        "}\n");
    check(r.ok, "lowers and optimises: " + r.stage);
    const cider::ir::Function* fn = find(r.module.get(), "f");
    check(fn != nullptr, "the function is there");
    if (fn == nullptr) {
        return;
    }
    check(r.opt.promoted == 1, "only the parameter is promoted");
    check(countOp(*fn, cider::ir::Op::Alloca) == 1, "the local Alloca stays");
    check(countOp(*fn, cider::ir::Op::Load) >= 1, "the join load stays");
}

// Aggregate word slots are not single scalar slots; their field offsets keep
// the function in memory.
void testMem2RegAggregate() {
    std::printf("mem2reg leaves aggregate slots in memory\n");
    Piped r = pipe(
        "struct Pair { var lo: int; var hi: int; }\n"
        "func f(a: int) -> int {\n"
        "    var p: Pair = Pair { lo: a, hi: 0 };\n"
        "    p.lo = a + 1;\n"
        "    return p.lo;\n"
        "}\n");
    check(r.ok, "lowers and optimises: " + r.stage);
    const cider::ir::Function* fn = find(r.module.get(), "f");
    check(fn != nullptr, "the function is there");
    if (fn == nullptr) {
        return;
    }
    check(r.opt.promoted == 0, "the aggregate frame is not promoted");
    check(countOp(*fn, cider::ir::Op::Alloca) >= 1, "the Alloca stays");
    check(countOp(*fn, cider::ir::Op::Load) >= 1, "the field load stays");
}

// A store followed by a load of the same slot answers the load from the
// stored value, and the load is gone.
void testForwardLoad() {
    std::printf("a stored value forwards to a later load\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.emit(ir.constant(0, 7, cider::ir::IRType::I32, t));
    ir.emit(ir.allocaSlot(1, t));
    ir.emit(ir.store(1, ir.value(0, cider::ir::IRType::I32, t),
                     cider::ir::IRType::Void, t));
    ir.emit(ir.load(2, 1, cider::ir::IRType::I32, t));
    ir.emit(ir.add(3, ir.value(2, cider::ir::IRType::I32, t),
                   ir.value(0, cider::ir::IRType::I32, t), t));
    ir.finishRet(0, 3, cider::ir::IRType::I32, t);
    cider::opt::MemoryResult result = cider::opt::forwardMemory(ir.module);
    check(result.forwarded == 1, "one load forwarded");
    check(result.storesRemoved == 0, "the read keeps its store");
    check(countOp(*ir.fn, cider::ir::Op::Load) == 0, "the load is gone");
    check(countOp(*ir.fn, cider::ir::Op::Store) == 1, "the store stays");
    const cider::ir::Instr* add = firstInstr(*ir.fn, cider::ir::Op::Add);
    check(add != nullptr && add->operands.size() == 2 &&
              add->operands[0].id == 0 && add->operands[1].id == 0,
          "the load's uses name the stored value");
}

// A store overwritten before any read is dead; the surviving store feeds the
// later load.
void testOverwrittenStore() {
    std::printf("an overwritten store is removed\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.emit(ir.constant(0, 1, cider::ir::IRType::I32, t));
    ir.emit(ir.allocaSlot(1, t));
    ir.emit(ir.store(1, ir.value(0, cider::ir::IRType::I32, t),
                     cider::ir::IRType::Void, t));
    ir.emit(ir.constant(2, 2, cider::ir::IRType::I32, t));
    ir.emit(ir.store(1, ir.value(2, cider::ir::IRType::I32, t),
                     cider::ir::IRType::Void, t));
    ir.emit(ir.load(3, 1, cider::ir::IRType::I32, t));
    ir.finishRet(0, 3, cider::ir::IRType::I32, t);
    cider::opt::MemoryResult result = cider::opt::forwardMemory(ir.module);
    check(result.storesRemoved == 1, "the first store is removed");
    check(result.forwarded == 1, "the load forwards the second value");
    check(countOp(*ir.fn, cider::ir::Op::Store) == 1, "one store remains");
    check(countOp(*ir.fn, cider::ir::Op::Load) == 0, "the load is gone");
    const cider::ir::Instr* store = firstInstr(*ir.fn, cider::ir::Op::Store);
    check(store != nullptr && store->storedValue.id == 2,
          "the surviving store is the later one");
    const cider::ir::Terminator& ret = ir.fn->blocks[0].term;
    check(ret.values.size() == 1 && ret.values[0].id == 2,
          "the return names the forwarded value");
}

// A unique-predecessor edge carries the value; a store-only straight-line
// chain also carries the dead-store proof.
void testCrossBlockForwarding() {
    std::printf("unique-predecessor edges carry forwarding state\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.emit(ir.constant(0, 9, cider::ir::IRType::I32, t));
    ir.emit(ir.allocaSlot(1, t));
    ir.emit(ir.store(1, ir.value(0, cider::ir::IRType::I32, t),
                     cider::ir::IRType::Void, t));
    ir.finishBr(0, 1);
    ir.addBlock(1);
    ir.emit(ir.load(2, 1, cider::ir::IRType::I32, t));
    ir.finishRet(1, 2, cider::ir::IRType::I32, t);
    cider::opt::MemoryResult result = cider::opt::forwardMemory(ir.module);
    check(result.forwarded == 1, "the successor's load forwards");
    check(countOp(*ir.fn, cider::ir::Op::Load) == 0, "the load is gone");
    check(ir.fn->blocks[1].term.values.size() == 1 &&
              ir.fn->blocks[1].term.values[0].id == 0,
          "the successor returns the stored value");
}

void testCrossBlockDeadStore() {
    std::printf("a straight-line successor removes an overwritten store\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.emit(ir.constant(0, 1, cider::ir::IRType::I32, t));
    ir.emit(ir.allocaSlot(1, t));
    ir.emit(ir.store(1, ir.value(0, cider::ir::IRType::I32, t),
                     cider::ir::IRType::Void, t));
    ir.finishBr(0, 1);
    ir.addBlock(1);
    ir.emit(ir.constant(2, 2, cider::ir::IRType::I32, t));
    ir.emit(ir.store(1, ir.value(2, cider::ir::IRType::I32, t),
                     cider::ir::IRType::Void, t));
    ir.finishBr(1, 2);
    ir.addBlock(2);
    ir.emit(ir.load(3, 1, cider::ir::IRType::I32, t));
    ir.finishRet(2, 3, cider::ir::IRType::I32, t);
    cider::opt::MemoryResult result = cider::opt::forwardMemory(ir.module);
    check(result.storesRemoved == 1, "the entry store is removed");
    check(result.forwarded == 1, "the second load forwards");
    check(countOp(*ir.fn, cider::ir::Op::Store) == 1, "one store remains");
    const cider::ir::Instr* store = firstInstr(*ir.fn, cider::ir::Op::Store);
    check(store != nullptr && store->storedValue.id == 2,
          "the second block's store survives");
}

// A branch with two successors means one path could read the store. The
// overwrite in one successor must not delete it.
void testMultiSuccessorStoreGuard() {
    std::printf("a multi-successor block does not remove its store\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.emit(ir.constant(0, 1, cider::ir::IRType::I32, t));
    ir.emit(ir.allocaSlot(1, t));
    ir.emit(ir.store(1, ir.value(0, cider::ir::IRType::I32, t),
                     cider::ir::IRType::Void, t));
    ir.emit(ir.constant(2, 1, cider::ir::IRType::Bool, ir.boolean()));
    ir.finishCondBr(0, 2, 1, 2);
    ir.addBlock(1);
    ir.emit(ir.constant(3, 2, cider::ir::IRType::I32, t));
    ir.emit(ir.store(1, ir.value(3, cider::ir::IRType::I32, t),
                     cider::ir::IRType::Void, t));
    ir.finishRet(1, 3, cider::ir::IRType::I32, t);
    ir.addBlock(2);
    ir.emit(ir.load(4, 1, cider::ir::IRType::I32, t));
    ir.finishRet(2, 4, cider::ir::IRType::I32, t);
    cider::opt::MemoryResult result = cider::opt::forwardMemory(ir.module);
    check(result.storesRemoved == 0, "the entry store is kept");
    check(result.forwarded == 1, "the other successor still forwards");
    check(countOp(*ir.fn, cider::ir::Op::Store) == 2, "both stores remain");
}

// Storing the Alloca's address somewhere escapes the slot: another path can
// now name its cell.
void testEscapingAddress() {
    std::printf("a stored Alloca address escapes its slot\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    cider::TypeRef pointer = cider::TypeRef{ir.types.pointer(t.type)};
    ir.emit(ir.constant(0, 3, cider::ir::IRType::I32, t));
    ir.emit(ir.allocaSlot(1, t));
    ir.emit(ir.store(1, ir.value(0, cider::ir::IRType::I32, t),
                     cider::ir::IRType::Void, t));
    ir.emit(ir.allocaSlot(4, pointer));
    ir.emit(ir.store(4, ir.value(1, cider::ir::IRType::Ptr, pointer),
                     cider::ir::IRType::Ptr, pointer));
    ir.emit(ir.load(2, 1, cider::ir::IRType::I32, t));
    ir.finishRet(0, 2, cider::ir::IRType::I32, t);
    cider::opt::MemoryResult result = cider::opt::forwardMemory(ir.module);
    check(result.forwarded == 0 && result.storesRemoved == 0,
          "nothing about the escaped slot is changed");
    check(countOp(*ir.fn, cider::ir::Op::Load) == 1, "the load stays");
}

// A Gep over the slot is an escaping use even with a zero index.
void testGepEscapes() {
    std::printf("a Gep use escapes its slot\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    cider::TypeRef i64 = ir.i64();
    ir.emit(ir.constant(0, 8, cider::ir::IRType::I64, i64));
    ir.emit(ir.allocaSlot(1, t));
    ir.emit(ir.gep(2, 1, ir.value(0, cider::ir::IRType::I64, i64)));
    ir.emit(ir.constant(3, 4, cider::ir::IRType::I32, t));
    ir.emit(ir.store(1, ir.value(3, cider::ir::IRType::I32, t),
                     cider::ir::IRType::Void, t));
    ir.emit(ir.load(4, 1, cider::ir::IRType::I32, t));
    ir.finishRet(0, 4, cider::ir::IRType::I32, t);
    cider::opt::MemoryResult result = cider::opt::forwardMemory(ir.module);
    check(result.forwarded == 0 && result.storesRemoved == 0,
          "the Gep'd slot is untouched");
    check(countOp(*ir.fn, cider::ir::Op::Load) == 1 &&
              countOp(*ir.fn, cider::ir::Op::Gep) == 1,
          "the load and the Gep stay");
}

// Alloca cells share the frame. One escaped address can reach a neighbour by
// displacement, so a Gep on one slot makes every slot in the function
// ineligible even when the other slots look direct-only.
void testNeighbourEscapeTaintsFunction() {
    std::printf("one escaping slot taints the function's frame\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    cider::TypeRef i64 = ir.i64();
    ir.emit(ir.constant(0, 8, cider::ir::IRType::I64, i64));
    ir.emit(ir.allocaSlot(1, t));
    ir.emit(ir.gep(2, 1, ir.value(0, cider::ir::IRType::I64, i64)));
    ir.emit(ir.allocaSlot(3, t));
    ir.emit(ir.constant(4, 5, cider::ir::IRType::I32, t));
    ir.emit(ir.store(3, ir.value(4, cider::ir::IRType::I32, t),
                     cider::ir::IRType::Void, t));
    ir.emit(ir.load(5, 3, cider::ir::IRType::I32, t));
    ir.finishRet(0, 5, cider::ir::IRType::I32, t);
    cider::opt::MemoryResult result = cider::opt::forwardMemory(ir.module);
    check(result.forwarded == 0 && result.storesRemoved == 0,
          "the neighbouring slot is not treated as private");
    check(countOp(*ir.fn, cider::ir::Op::Load) == 1, "the load stays");
}

// Calls are barriers even when they do not name the slot, and naming the slot
// in an argument escapes it.
void testCallBarrier() {
    std::printf("calls are barriers and call arguments escape\n");
    {
        Ir ir;
        cider::TypeRef t = ir.i32();
        ir.emit(ir.constant(0, 5, cider::ir::IRType::I32, t));
        ir.emit(ir.allocaSlot(1, t));
        ir.emit(ir.store(1, ir.value(0, cider::ir::IRType::I32, t),
                         cider::ir::IRType::Void, t));
        ir.emit(ir.call("g"));
        ir.emit(ir.load(2, 1, cider::ir::IRType::I32, t));
        ir.finishRet(0, 2, cider::ir::IRType::I32, t);
        cider::opt::MemoryResult result = cider::opt::forwardMemory(ir.module);
        check(result.forwarded == 0 && result.storesRemoved == 0,
              "no value crosses a call");
        check(countOp(*ir.fn, cider::ir::Op::Load) == 1, "the load stays");
    }
    {
        Ir ir;
        cider::TypeRef t = ir.i32();
        cider::TypeRef pointer = cider::TypeRef{ir.types.pointer(t.type)};
        ir.emit(ir.constant(0, 5, cider::ir::IRType::I32, t));
        ir.emit(ir.allocaSlot(1, t));
        ir.emit(ir.store(1, ir.value(0, cider::ir::IRType::I32, t),
                         cider::ir::IRType::Void, t));
        ir.emit(ir.callWith("g", ir.value(1, cider::ir::IRType::Ptr, pointer)));
        ir.emit(ir.load(2, 1, cider::ir::IRType::I32, t));
        ir.finishRet(0, 2, cider::ir::IRType::I32, t);
        cider::opt::MemoryResult result = cider::opt::forwardMemory(ir.module);
        check(result.forwarded == 0 && result.storesRemoved == 0,
              "a call argument escapes the slot");
        check(countOp(*ir.fn, cider::ir::Op::Load) == 1, "the load stays");
    }
    {
        // A call after the access is a barrier, not a reason to leave the
        // whole function alone.
        Ir ir;
        cider::TypeRef t = ir.i32();
        ir.emit(ir.constant(0, 5, cider::ir::IRType::I32, t));
        ir.emit(ir.allocaSlot(1, t));
        ir.emit(ir.store(1, ir.value(0, cider::ir::IRType::I32, t),
                         cider::ir::IRType::Void, t));
        ir.emit(ir.load(2, 1, cider::ir::IRType::I32, t));
        ir.emit(ir.call("g"));
        ir.finishRet(0, 2, cider::ir::IRType::I32, t);
        cider::opt::MemoryResult result = cider::opt::forwardMemory(ir.module);
        check(result.forwarded == 1, "an earlier load still forwards");
        check(countOp(*ir.fn, cider::ir::Op::Load) == 0, "the load is gone");
    }
}

// Asm is a barrier with no operand, and an operand names the slot and escapes
// it.
void testAsmBarrier() {
    std::printf("asm is a barrier and asm operands escape\n");
    {
        Ir ir;
        cider::TypeRef t = ir.i32();
        ir.emit(ir.constant(0, 6, cider::ir::IRType::I32, t));
        ir.emit(ir.allocaSlot(1, t));
        ir.emit(ir.store(1, ir.value(0, cider::ir::IRType::I32, t),
                         cider::ir::IRType::Void, t));
        ir.emit(ir.asmBlock());
        ir.emit(ir.load(2, 1, cider::ir::IRType::I32, t));
        ir.finishRet(0, 2, cider::ir::IRType::I32, t);
        cider::opt::MemoryResult result = cider::opt::forwardMemory(ir.module);
        check(result.forwarded == 0 && result.storesRemoved == 0,
              "no value crosses an asm block");
        check(countOp(*ir.fn, cider::ir::Op::Load) == 1, "the load stays");
    }
    {
        Ir ir;
        cider::TypeRef t = ir.i32();
        cider::TypeRef pointer = cider::TypeRef{ir.types.pointer(t.type)};
        ir.emit(ir.constant(0, 6, cider::ir::IRType::I32, t));
        ir.emit(ir.allocaSlot(1, t));
        ir.emit(ir.store(1, ir.value(0, cider::ir::IRType::I32, t),
                         cider::ir::IRType::Void, t));
        cider::ir::Instr asmInstr = ir.asmBlock();
        asmInstr.operands.push_back(
            ir.value(1, cider::ir::IRType::Ptr, pointer));
        ir.emit(std::move(asmInstr));
        ir.emit(ir.load(2, 1, cider::ir::IRType::I32, t));
        ir.finishRet(0, 2, cider::ir::IRType::I32, t);
        cider::opt::MemoryResult result = cider::opt::forwardMemory(ir.module);
        check(result.forwarded == 0 && result.storesRemoved == 0,
              "an asm operand escapes the slot");
        check(countOp(*ir.fn, cider::ir::Op::Load) == 1, "the load stays");
    }
    {
        // An asm block can name the frame by register even with no operand.
        // The whole function keeps its memory shape, including a load that
        // appears before the asm.
        Ir ir;
        cider::TypeRef t = ir.i32();
        ir.emit(ir.constant(0, 6, cider::ir::IRType::I32, t));
        ir.emit(ir.allocaSlot(1, t));
        ir.emit(ir.store(1, ir.value(0, cider::ir::IRType::I32, t),
                         cider::ir::IRType::Void, t));
        ir.emit(ir.load(2, 1, cider::ir::IRType::I32, t));
        ir.emit(ir.asmBlock());
        ir.finishRet(0, 2, cider::ir::IRType::I32, t);
        cider::opt::MemoryResult result = cider::opt::forwardMemory(ir.module);
        check(result.forwarded == 0 && result.storesRemoved == 0,
              "asm disables the function's earlier accesses too");
        check(countOp(*ir.fn, cider::ir::Op::Load) == 1, "the earlier load stays");
    }
}

// Volatile accesses keep their own shape and disable the slot.
void testVolatileUntouched() {
    std::printf("volatile slot accesses are untouched\n");
    {
        Ir ir;
        cider::TypeRef t = ir.i32();
        ir.emit(ir.constant(0, 7, cider::ir::IRType::I32, t));
        ir.emit(ir.allocaSlot(1, t));
        ir.emit(ir.store(1, ir.value(0, cider::ir::IRType::I32, t),
                         cider::ir::IRType::Void, t, true));
        ir.emit(ir.load(2, 1, cider::ir::IRType::I32, t));
        ir.finishRet(0, 2, cider::ir::IRType::I32, t);
        cider::opt::MemoryResult result = cider::opt::forwardMemory(ir.module);
        check(result.forwarded == 0 && result.storesRemoved == 0,
              "a volatile store disables the slot");
        const cider::ir::Instr* store = firstInstr(*ir.fn, cider::ir::Op::Store);
        check(store != nullptr && store->isVolatile, "the volatile store stays");
        check(countOp(*ir.fn, cider::ir::Op::Load) == 1, "the load stays");
    }
    {
        Ir ir;
        cider::TypeRef t = ir.i32();
        ir.emit(ir.constant(0, 7, cider::ir::IRType::I32, t));
        ir.emit(ir.allocaSlot(1, t));
        ir.emit(ir.store(1, ir.value(0, cider::ir::IRType::I32, t),
                         cider::ir::IRType::Void, t));
        ir.emit(ir.load(2, 1, cider::ir::IRType::I32, t, true));
        ir.finishRet(0, 2, cider::ir::IRType::I32, t);
        cider::opt::MemoryResult result = cider::opt::forwardMemory(ir.module);
        check(result.forwarded == 0 && result.storesRemoved == 0,
              "a volatile load disables the slot");
        const cider::ir::Instr* load = firstInstr(*ir.fn, cider::ir::Op::Load);
        check(load != nullptr && load->isVolatile, "the volatile load stays");
    }
}

// Width or language-type mismatches never forward, and do not let an overwrite
// delete a store whose bytes the new store does not fully cover.
void testMismatchedAccess() {
    std::printf("mismatched widths and types are untouched\n");
    {
        Ir ir;
        cider::TypeRef t = ir.i32();
        cider::TypeRef wide = ir.i64();
        ir.emit(ir.constant(0, 1, cider::ir::IRType::I32, t));
        ir.emit(ir.allocaSlot(1, t));
        ir.emit(ir.store(1, ir.value(0, cider::ir::IRType::I32, t),
                         cider::ir::IRType::Void, t));
        ir.emit(ir.load(2, 1, cider::ir::IRType::I64, wide));
        ir.finishRet(0, 2, cider::ir::IRType::I64, wide);
        cider::opt::MemoryResult result = cider::opt::forwardMemory(ir.module);
        check(result.forwarded == 0, "a width mismatch does not forward");
        check(countOp(*ir.fn, cider::ir::Op::Load) == 1, "the load stays");
    }
    {
        Ir ir;
        cider::TypeRef t = ir.i32();
        cider::TypeRef unsigned32 = ir.u32();
        ir.emit(ir.constant(0, 1, cider::ir::IRType::I32, t));
        ir.emit(ir.allocaSlot(1, t));
        ir.emit(ir.store(1, ir.value(0, cider::ir::IRType::I32, t),
                         cider::ir::IRType::Void, t));
        ir.emit(ir.load(2, 1, cider::ir::IRType::U32, unsigned32));
        ir.finishRet(0, 2, cider::ir::IRType::U32, unsigned32);
        cider::opt::MemoryResult result = cider::opt::forwardMemory(ir.module);
        check(result.forwarded == 0, "a type mismatch does not forward");
        check(countOp(*ir.fn, cider::ir::Op::Load) == 1, "the load stays");
    }
    {
        Ir ir;
        cider::TypeRef t = ir.i32();
        cider::TypeRef wide = ir.i64();
        ir.emit(ir.constant(0, 1, cider::ir::IRType::I32, t));
        ir.emit(ir.allocaSlot(1, t));
        ir.emit(ir.store(1, ir.value(0, cider::ir::IRType::I32, t),
                         cider::ir::IRType::Void, t));
        ir.emit(ir.constant(2, 2, cider::ir::IRType::I64, wide));
        ir.emit(ir.store(1, ir.value(2, cider::ir::IRType::I64, wide),
                         cider::ir::IRType::Void, wide));
        ir.emit(ir.load(3, 1, cider::ir::IRType::I64, wide));
        ir.finishRet(0, 3, cider::ir::IRType::I64, wide);
        cider::opt::MemoryResult result = cider::opt::forwardMemory(ir.module);
        check(result.storesRemoved == 0,
              "a differently typed overwrite is not removed");
        check(result.forwarded == 1, "the matching load still forwards");
        check(countOp(*ir.fn, cider::ir::Op::Store) == 2, "both stores stay");
    }
    {
        Ir ir;
        cider::TypeRef t = ir.i32();
        ir.emit(ir.constant(0, 1, cider::ir::IRType::I32, t));
        ir.emit(ir.allocaSlot(1, t));
        ir.emit(ir.store(1, ir.value(0, cider::ir::IRType::I32, t),
                         cider::ir::IRType::Void, t));
        ir.emit(ir.constant(2, 2, cider::ir::IRType::I32, t));
        ir.emit(ir.store(1, ir.value(2, cider::ir::IRType::I32, t),
                         cider::ir::IRType::I32, t));
        ir.emit(ir.load(3, 1, cider::ir::IRType::I32, t));
        ir.finishRet(0, 3, cider::ir::IRType::I32, t);
        cider::opt::MemoryResult result = cider::opt::forwardMemory(ir.module);
        check(result.storesRemoved == 0,
              "a narrower overwrite does not cover the first store");
        check(result.forwarded == 1, "the matching load still forwards");
        check(countOp(*ir.fn, cider::ir::Op::Store) == 2, "both stores stay");
    }
}

// A volatile access removes the slot from mem2reg eligibility.
void testMem2RegVolatile() {
    std::printf("mem2reg leaves volatile slots in memory\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.emit(ir.constant(0, 7, cider::ir::IRType::I32, t));
    ir.emit(ir.allocaSlot(1, t));
    ir.emit(ir.store(1, ir.value(0, cider::ir::IRType::I32, t),
                     cider::ir::IRType::Void, t, true));
    ir.emit(ir.load(2, 1, cider::ir::IRType::I32, t));
    ir.finishRet(0, 2, cider::ir::IRType::I32, t);
    cider::opt::PromotionResult result = cider::opt::mem2reg(ir.module);
    check(result.promotedSlots == 0, "the volatile slot is not promoted");
    check(countOp(*ir.fn, cider::ir::Op::Alloca) == 1, "the Alloca stays");
    check(countOp(*ir.fn, cider::ir::Op::Load) == 1, "the Load stays");
    check(countOp(*ir.fn, cider::ir::Op::Store) == 1, "the Store stays");
}

// Width and language-type mismatches keep the slot in memory.
void testMem2RegMismatched() {
    std::printf("mem2reg leaves mismatched slots in memory\n");
    {
        Ir ir;
        cider::TypeRef t = ir.i32();
        cider::TypeRef wide = ir.i64();
        ir.emit(ir.constant(0, 1, cider::ir::IRType::I32, t));
        ir.emit(ir.allocaSlot(1, t));
        ir.emit(ir.store(1, ir.value(0, cider::ir::IRType::I32, t),
                         cider::ir::IRType::Void, t));
        ir.emit(ir.load(2, 1, cider::ir::IRType::I64, wide));
        ir.finishRet(0, 2, cider::ir::IRType::I64, wide);
        cider::opt::PromotionResult result = cider::opt::mem2reg(ir.module);
        check(result.promotedSlots == 0, "a width mismatch is not promoted");
        check(countOp(*ir.fn, cider::ir::Op::Load) == 1, "the Load stays");
    }
    {
        Ir ir;
        cider::TypeRef t = ir.i32();
        cider::TypeRef unsigned32 = ir.u32();
        ir.emit(ir.constant(0, 1, cider::ir::IRType::I32, t));
        ir.emit(ir.allocaSlot(1, t));
        ir.emit(ir.store(1, ir.value(0, cider::ir::IRType::I32, t),
                         cider::ir::IRType::Void, t));
        ir.emit(ir.load(2, 1, cider::ir::IRType::U32, unsigned32));
        ir.finishRet(0, 2, cider::ir::IRType::U32, unsigned32);
        cider::opt::PromotionResult result = cider::opt::mem2reg(ir.module);
        check(result.promotedSlots == 0, "a type mismatch is not promoted");
        check(countOp(*ir.fn, cider::ir::Op::Load) == 1, "the Load stays");
    }
    {
        // A store that writes fewer bytes than the value it carries cannot be
        // promoted: the whole value is not in the cell.
        Ir ir;
        cider::TypeRef t = ir.i32();
        ir.emit(ir.constant(0, 1, cider::ir::IRType::I32, t));
        ir.emit(ir.allocaSlot(1, t));
        ir.emit(ir.store(1, ir.value(0, cider::ir::IRType::I32, t),
                         cider::ir::IRType::I16, t));
        ir.emit(ir.load(2, 1, cider::ir::IRType::I32, t));
        ir.finishRet(0, 2, cider::ir::IRType::I32, t);
        cider::opt::PromotionResult result = cider::opt::mem2reg(ir.module);
        check(result.promotedSlots == 0, "a partial store is not promoted");
        check(countOp(*ir.fn, cider::ir::Op::Load) == 1, "the Load stays");
    }
}

// A valid diamond phi passes the structural checker.
void testPhiStructureValid() {
    std::printf("a valid phi passes the structural checker\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    cider::TypeRef b = ir.boolean();
    ir.emit(ir.constant(0, 1, cider::ir::IRType::Bool, b));
    ir.emit(ir.constant(1, 11, cider::ir::IRType::I32, t));
    ir.emit(ir.constant(2, 22, cider::ir::IRType::I32, t));
    ir.finishCondBr(0, 0, 1, 2);
    ir.addBlock(1);
    ir.finishBr(1, 3);
    ir.addBlock(2);
    ir.finishBr(2, 3);
    ir.addBlock(3);
    ir.emit(ir.phi(4, cider::ir::IRType::I32, t, {{1, 1}, {2, 2}}));
    ir.finishRet(3, 4, cider::ir::IRType::I32, t);
    std::vector<cider::opt::StructuralError> errors =
        cider::opt::checkModule(ir.module);
    check(errors.empty(), "the diamond is well formed");
}

// Phi arity, predecessor membership and incoming dominance are all checked.
void testPhiStructureErrors() {
    std::printf("the structural checker validates phi edges\n");
    auto diamond = [](bool missing, bool wrongPred, bool badDominance,
                      bool duplicate, bool afterNonPhi, bool inEntry) {
        Ir ir;
        cider::TypeRef t = ir.i32();
        cider::TypeRef b = ir.boolean();
        ir.emit(ir.constant(0, 1, cider::ir::IRType::Bool, b));
        ir.emit(ir.constant(1, 11, cider::ir::IRType::I32, t));
        ir.emit(ir.constant(2, 22, cider::ir::IRType::I32, t));
        ir.finishCondBr(0, 0, 1, 2);
        ir.addBlock(1);
        ir.finishBr(1, 3);
        ir.addBlock(2);
        ir.finishBr(2, 3);
        ir.addBlock(3);
        if (afterNonPhi) {
            ir.emit(ir.constant(5, 5, cider::ir::IRType::I32, t));
        }
        std::vector<std::pair<uint32_t, uint32_t>> incoming;
        if (!missing) {
            incoming.push_back({1, 1});
        }
        if (wrongPred) {
            incoming.push_back({2, 99});
        } else if (badDominance) {
            incoming.push_back({2, 1});
            incoming.push_back({1, 2});
        } else {
            incoming.push_back({2, 2});
        }
        if (duplicate) {
            incoming.push_back({2, 1});
        }
        ir.emit(ir.phi(4, cider::ir::IRType::I32, t, incoming));
        ir.finishRet(3, 4, cider::ir::IRType::I32, t);
        if (inEntry) {
            // Move the phi to the entry block; the diamond above is then
            // intentionally inconsistent and must be refused.
            ir.fn->blocks[0].body.push_back(ir.fn->blocks[3].body[0]);
            ir.fn->blocks[3].body.erase(ir.fn->blocks[3].body.begin());
        }
        return cider::opt::checkModule(ir.module);
    };
    check(!diamond(true, false, false, false, false, false).empty(),
          "a missing incoming is refused");
    check(!diamond(false, true, false, false, false, false).empty(),
          "an unknown predecessor is refused");
    check(!diamond(false, false, true, false, false, false).empty(),
          "a non-dominating incoming value is refused");
    check(!diamond(false, false, false, true, false, false).empty(),
          "a duplicate predecessor is refused");
    check(!diamond(false, false, false, false, true, false).empty(),
          "a phi after a non-phi is refused");
    check(!diamond(false, false, false, false, false, true).empty(),
          "a phi in the entry block is refused");
}

// VARIADICS.md §8:VaStart takes one `*va_list` and answers nothing; VaArg
// takes one `*va_list` and answers exactly one non-void value. The shapes are
// checked before any pass runs, so a malformed lowering never reaches codegen.
void testVaOperationStructure() {
    std::printf("the structural checker validates va operations\n");
    struct Shape {
        bool startResult = false;
        bool startBadOperand = false;
        bool startBadType = false;
        bool startNonVariadic = false;
        bool argNoResult = false;
        bool argVoidWidth = false;
        bool argBadOperand = false;
        bool argBadType = false;
    };
    auto validate = [](Shape shape) {
        Ir ir;
        cider::TypeRef t = ir.i32();
        cider::TypeRef p = ir.ptr();
        // A function signature is where va_start's context lives: the shape
        // is legal only in a variadic body (VARIADICS.md §5).
        ir.fn->signature = cider::Signature(
            {}, t.type, cider::Abi::SysV, !shape.startNonVariadic);
        ir.emit(ir.constant(10, 0, cider::ir::IRType::Ptr, p));
        ir.emit(ir.constant(12, 7, cider::ir::IRType::I32, t));
        cider::ir::Instr start;
        start.op = cider::ir::Op::VaStart;
        start.operands.push_back(ir.value(
            10, cider::ir::IRType::Ptr, shape.startBadType ? t : p));
        if (shape.startBadOperand) {
            start.operands.push_back(ir.value(10, cider::ir::IRType::Ptr, p));
        }
        if (shape.startResult) {
            start.results.push_back(
                ir.value(13, cider::ir::IRType::I32, t));
        }
        ir.emit(start);
        cider::ir::Instr read;
        read.op = cider::ir::Op::VaArg;
        read.width = shape.argVoidWidth ? cider::ir::IRType::Void
                                        : cider::ir::IRType::I32;
        read.type = t;
        read.operands.push_back(ir.value(10, cider::ir::IRType::Ptr, p));
        if (shape.argBadOperand) {
            read.operands.push_back(
                ir.value(10, cider::ir::IRType::Ptr, p));
        }
        if (!shape.argNoResult) {
            read.results.push_back(ir.value(
                11, read.width,
                shape.argBadType ? ir.boolean() : t));
        }
        ir.emit(read);
        // A value the return can name in every shape: without it the
        // no-result case failed for an undefined use, not for its own shape.
        ir.finishRet(0, shape.argNoResult ? 12 : 11,
                     cider::ir::IRType::I32, t);
        return cider::opt::checkModule(ir.module);
    };
    check(validate(Shape{}).empty(), "a well-formed va_start/va_arg passes");
    check(!validate(Shape{.startResult = true}).empty(),
          "va_start with a result is refused");
    check(!validate(Shape{.startBadOperand = true}).empty(),
          "va_start needs exactly one pointer operand");
    check(!validate(Shape{.startBadType = true}).empty(),
          "va_start needs a pointer-typed operand");
    check(!validate(Shape{.startNonVariadic = true}).empty(),
          "va_start outside a variadic function is refused");
    check(!validate(Shape{.argNoResult = true}).empty(),
          "va_arg without a result is refused");
    check(!validate(Shape{.argVoidWidth = true}).empty(),
          "va_arg with a void width is refused");
    check(!validate(Shape{.argBadOperand = true}).empty(),
          "va_arg needs exactly one pointer operand");
    check(!validate(Shape{.argBadType = true}).empty(),
          "va_arg needs one scalar result type");
}

// Phi lowering removes phis and leaves ordinary copies; a critical edge gets
// a dedicated edge block first.
void testPhiLowering() {
    std::printf("phi lowering splits critical edges and emits copies\n");
    {
        Ir ir;
        cider::TypeRef t = ir.i32();
        cider::TypeRef b = ir.boolean();
        ir.emit(ir.constant(0, 1, cider::ir::IRType::Bool, b));
        ir.emit(ir.constant(1, 11, cider::ir::IRType::I32, t));
        ir.emit(ir.constant(2, 22, cider::ir::IRType::I32, t));
        ir.finishCondBr(0, 0, 1, 2);
        ir.addBlock(1);
        ir.finishBr(1, 3);
        ir.addBlock(2);
        ir.finishBr(2, 3);
        ir.addBlock(3);
        ir.emit(ir.phi(4, cider::ir::IRType::I32, t, {{1, 1}, {2, 2}}));
        ir.finishRet(3, 4, cider::ir::IRType::I32, t);
        cider::opt::PhiLowerResult result = cider::opt::lowerPhis(ir.module);
        check(result.ok, "lowering succeeds: " + result.error);
        check(result.phisLowered == 1, "one phi is lowered");
        check(countOp(*ir.fn, cider::ir::Op::Phi) == 0, "no phi remains");
        check(countOp(*ir.fn, cider::ir::Op::Copy) >= 2,
              "both incoming edges get a copy");
    }
    {
        // block1 has two successors and block3 has two predecessors, so the
        // block1 -> block3 edge is critical.
        Ir ir;
        cider::TypeRef t = ir.i32();
        cider::TypeRef b = ir.boolean();
        ir.emit(ir.constant(0, 1, cider::ir::IRType::Bool, b));
        ir.finishBr(0, 1);
        ir.addBlock(1);
        ir.emit(ir.constant(1, 1, cider::ir::IRType::Bool, b));
        ir.emit(ir.constant(5, 33, cider::ir::IRType::I32, t));
        ir.finishCondBr(1, 1, 2, 3);
        ir.addBlock(2);
        ir.emit(ir.constant(2, 22, cider::ir::IRType::I32, t));
        ir.finishBr(2, 3);
        ir.addBlock(3);
        ir.emit(ir.phi(4, cider::ir::IRType::I32, t, {{5, 1}, {2, 2}}));
        ir.finishRet(3, 4, cider::ir::IRType::I32, t);
        const uint64_t before = ir.fn->blocks.size();
        cider::opt::PhiLowerResult result = cider::opt::lowerPhis(ir.module);
        check(result.ok, "critical-edge lowering succeeds: " + result.error);
        check(result.splitBlocks >= 1, "the critical edge was split");
        check(ir.fn->blocks.size() > before, "an edge block was added");
        check(countOp(*ir.fn, cider::ir::Op::Phi) == 0, "no phi remains");
        check(countOp(*ir.fn, cider::ir::Op::Copy) >= 1,
              "the edge block carries a copy");
    }
}

cider::ir::Rodata dedupBlob(const std::string& label,
                            const std::string& bytes) {
    cider::ir::Rodata blob;
    blob.label = label;
    blob.bytes = bytes;
    return blob;
}

cider::ir::Instr dedupDataAddr(const std::string& label, uint32_t id) {
    cider::ir::Instr instr;
    instr.op = cider::ir::Op::DataAddr;
    instr.globalLabel = label;
    instr.width = cider::ir::IRType::Ptr;
    instr.isPure = true;
    instr.results.push_back(
        cider::ir::Value{id, cider::ir::IRType::Ptr, {}});
    return instr;
}

// Two literals share bytes, one is distinct, and references arrive through a
// DataAddr, a global's initLabel, a vtable-style initLabels list, and a
// relocated image word.
cider::ir::Module dedupFixture() {
    cider::ir::Module module;
    module.name = "main";

    cider::ir::Function entry;
    entry.name = "_start";
    cider::ir::Block block;
    block.label = 0;
    block.body.push_back(dedupDataAddr("s_1", 0));
    block.body.push_back(dedupDataAddr("s_3", 1));
    block.term.term = cider::ir::Term::Ret;
    entry.blocks.push_back(std::move(block));
    module.functions.push_back(std::move(entry));

    cider::ir::Global first;
    first.label = "g_first";
    first.initLabel = "s_1";
    module.globals.push_back(std::move(first));

    cider::ir::Global second;
    second.label = "g_second";
    second.initLabels.push_back("s_3");
    second.relocWords.push_back({0, "s_3"});
    module.globals.push_back(std::move(second));

    module.rodata.push_back(dedupBlob("s_0", std::string("same\0", 5)));
    module.rodata.push_back(dedupBlob("s_1", std::string("same\0", 5)));
    module.rodata.push_back(dedupBlob("s_2", std::string("other\0", 6)));
    module.rodata.push_back(dedupBlob("s_3", std::string("same\0", 5)));
    return module;
}

void testRodataDedup() {
    std::printf("identical rodata bytes share the first label\n");
    cider::ir::Module module = dedupFixture();
    cider::opt::RodataDedupResult result =
        cider::opt::deduplicateRodata(module);

    std::string labels;
    for (const cider::ir::Rodata& blob : module.rodata) {
        labels += blob.label + ",";
    }
    check(labels == "s_0,s_2,", "the first label survives in module order");
    check(module.rodata[0].bytes == std::string("same\0", 5),
          "the canonical blob keeps its exact bytes");
    check(module.rodata[1].bytes == std::string("other\0", 6),
          "a distinct byte string is not merged");
    check(result.blobsMerged == 2 && result.bytesRemoved == 10,
          "both duplicate blobs and their bytes are counted");
    check(result.referencesRewritten == 5,
          "every instruction and global reference is rewritten");

    const cider::ir::Block& block = module.functions[0].blocks[0];
    check(block.body[0].globalLabel == "s_0" &&
              block.body[1].globalLabel == "s_0",
          "DataAddr references name the canonical label");
    check(module.globals[0].initLabel == "s_0",
          "a global initLabel is rewritten");
    check(module.globals[1].initLabels.size() == 1 &&
              module.globals[1].initLabels[0] == "s_0",
          "a vtable/initializer label list is rewritten");
    check(module.globals[1].relocWords.size() == 1 &&
              module.globals[1].relocWords[0].second == "s_0",
          "a relocated image word is rewritten");
}

void testRodataDedupPinsAssembly() {
    std::printf("assembly-mentioned labels stay defined\n");
    {
        cider::ir::Module module;
        module.name = "main";
        module.moduleAsm = "  .quad s_1\n";
        module.rodata.push_back(dedupBlob("s_0", std::string("same\0", 5)));
        module.rodata.push_back(dedupBlob("s_1", std::string("same\0", 5)));

        cider::opt::RodataDedupResult result =
            cider::opt::deduplicateRodata(module);
        check(module.rodata.size() == 2,
              "module asm pins the duplicate it names");
        check(result.blobsMerged == 0 && result.asmPinned == 1,
              "the pinned blob is reported and nothing is merged");
    }
    {
        cider::ir::Module module;
        module.name = "main";
        cider::ir::Function entry;
        entry.name = "_start";
        cider::ir::Block block;
        block.label = 0;
        cider::ir::Instr asmInstr;
        asmInstr.op = cider::ir::Op::Asm;
        asmInstr.asmText = "lea rax, [rip + s_1]";
        block.body.push_back(std::move(asmInstr));
        block.term.term = cider::ir::Term::Ret;
        entry.blocks.push_back(std::move(block));
        module.functions.push_back(std::move(entry));
        module.rodata.push_back(dedupBlob("s_0", std::string("same\0", 5)));
        module.rodata.push_back(dedupBlob("s_1", std::string("same\0", 5)));

        cider::opt::RodataDedupResult result =
            cider::opt::deduplicateRodata(module);
        check(module.rodata.size() == 2,
              "inline asm pins the duplicate it names");
        check(result.blobsMerged == 0 && result.asmPinned == 1,
              "inline-asm pinning is reported");
        check(module.functions[0].blocks[0].body[0].asmText ==
                  "lea rax, [rip + s_1]",
              "assembly text is never rewritten");
    }
}

void testRodataDedupAddressIdentity() {
    std::printf("pointer-converted literals keep distinct addresses\n");
    cider::ir::Module module;
    module.name = "main";
    cider::ir::Rodata pointer = dedupBlob("s_0", std::string("hi\0", 3));
    pointer.addressIdentity = true;
    module.rodata.push_back(std::move(pointer));
    module.rodata.push_back(dedupBlob("s_1", std::string("hi\0", 3)));

    cider::opt::RodataDedupResult result =
        cider::opt::deduplicateRodata(module);
    check(module.rodata.size() == 2,
          "an address-identity blob is never merged");
    check(result.blobsMerged == 0 && result.identityPinned == 1,
          "the identity pin is reported and nothing is merged");

    cider::ir::Module strings;
    strings.name = "main";
    strings.rodata.push_back(dedupBlob("s_0", std::string("hi\0", 3)));
    strings.rodata.push_back(dedupBlob("s_1", std::string("hi\0", 3)));
    cider::opt::RodataDedupResult merged =
        cider::opt::deduplicateRodata(strings);
    check(strings.rodata.size() == 1 && merged.blobsMerged == 1,
          "the same bytes still merge when no address is observed");
}

void testRodataDedupDeterministic() {
    std::printf("rodata dedup is deterministic\n");
    cider::ir::Module first = dedupFixture();
    cider::ir::Module second = dedupFixture();
    cider::opt::RodataDedupResult firstResult =
        cider::opt::deduplicateRodata(first);
    cider::opt::RodataDedupResult secondResult =
        cider::opt::deduplicateRodata(second);
    check(firstResult.blobsMerged == secondResult.blobsMerged &&
              firstResult.bytesRemoved == secondResult.bytesRemoved &&
              firstResult.referencesRewritten ==
                  secondResult.referencesRewritten,
          "two identical modules report identical counters");
    check(cider::ir::dumpModule(first) == cider::ir::dumpModule(second),
          "two identical modules deduplicate byte-identically");

    cider::ir::Module single;
    single.name = "main";
    single.rodata.push_back(dedupBlob("s_0", std::string("only\0", 5)));
    cider::opt::RodataDedupResult singleResult =
        cider::opt::deduplicateRodata(single);
    check(single.rodata.size() == 1 && singleResult.blobsMerged == 0,
          "a single blob is untouched");
}

// A non-zero displacement use marks the slot escaping, even though it still
// names the same Alloca.
void testDisplacementEscapes() {
    std::printf("a displaced access escapes its slot\n");
    Ir ir;
    cider::TypeRef t = ir.i32();
    ir.emit(ir.constant(0, 1, cider::ir::IRType::I32, t));
    ir.emit(ir.allocaSlot(1, t));
    ir.emit(ir.store(1, ir.value(0, cider::ir::IRType::I32, t),
                     cider::ir::IRType::Void, t));
    cider::ir::Instr load = ir.load(2, 1, cider::ir::IRType::I32, t);
    load.displacement = 4;
    ir.emit(std::move(load));
    ir.finishRet(0, 2, cider::ir::IRType::I32, t);
    cider::opt::MemoryResult result = cider::opt::forwardMemory(ir.module);
    check(result.forwarded == 0 && result.storesRemoved == 0,
          "the displaced slot is untouched");
    check(countOp(*ir.fn, cider::ir::Op::Load) == 1, "the load stays");
}

}  // namespace

// VARIADICS.md §8: the va operations are opaque to the optimiser. VaCopy's
// word stores initialize a cursor no Load reads, and a variadic callee must
// stay a call: cloning one would leave va_start's saved argument state behind.
void testVariadicPipeline() {
    std::printf("variadic IR survives the optimiser\n");
    Piped r = pipe(
        "from std.varargs import VaList, va_start, va_copy, va_arg_i32, va_end;\n"
        "func sum(n: int, ...) -> int {\n"
        "  var ap: VaList = uninit; var copy: VaList = uninit;\n"
        "  va_start(&ap); va_copy(&copy, &ap);\n"
        "  var v: i32 = va_arg_i32(&copy);\n"
        "  va_end(&ap); va_end(&copy); return n + v;\n"
        "}\n"
        "func tag(n: int, ...) -> int {\n"
        "  var ap: VaList = uninit; va_start(&ap); va_end(&ap); return n;\n"
        "}\n"
        "func main() -> int { return sum(1) + tag(2); }\n");
    check(r.ok, "the variadic program optimises: " + r.stage);
    if (!r.ok) {
        return;
    }
    const cider::ir::Function* sum = nullptr;
    for (const cider::ir::Function& fn : r.module->functions) {
        if (fn.name == "sum") {
            sum = &fn;
        }
    }
    check(sum != nullptr, "the variadic callee survives");
    check(sum != nullptr &&
              countOp(*sum, cider::ir::Op::VaStart) == 1 &&
              countOp(*sum, cider::ir::Op::VaArg) == 1,
          "va_start and va_arg survive");
    check(sum != nullptr && countOp(*sum, cider::ir::Op::Load) >= 3 &&
              countOp(*sum, cider::ir::Op::Store) >= 3,
          "va_copy's cursor stores are not dead");
    const cider::ir::Function* main = nullptr;
    for (const cider::ir::Function& fn : r.module->functions) {
        if (fn.name == "main") {
            main = &fn;
        }
    }
    check(main != nullptr && countOp(*main, cider::ir::Op::Call) >= 1,
          "a variadic call is not inlined (inline count " +
              std::to_string(r.opt.inlined) + ")");
    bool tagCall = false;
    if (main != nullptr) {
        for (const cider::ir::Block& block : main->blocks) {
            for (const cider::ir::Instr& instr : block.body) {
                if (instr.op == cider::ir::Op::Call && instr.callee == "tag") {
                    tagCall = true;
                }
            }
        }
    }
    check(tagCall, "the small variadic callee stays a call");
    check(main != nullptr && countOp(*main, cider::ir::Op::VaStart) == 0,
          "no saved-argument state leaks into the caller");
}

int main() {
    std::printf("opt_test — malformed IR is refused, passes preserve meaning\n\n");
    testWellFormed();
    testFold();
    testFoldWidths();
    testDeadCode();
    testIndirectKept();
    testTrapKept();
    testMem2RegJoin();
    testMem2RegNestedJoins();
    testMem2RegLoop();
    testMem2RegMultipleDefs();
    testMem2RegParameter();
    testMem2RegCall();
    testMem2RegEscape();
    testMem2RegAsm();
    testMem2RegUninit();
    testMem2RegUninitBranch();
    testMem2RegAggregate();
    testForwardLoad();
    testOverwrittenStore();
    testCrossBlockForwarding();
    testCrossBlockDeadStore();
    testMultiSuccessorStoreGuard();
    testEscapingAddress();
    testGepEscapes();
    testNeighbourEscapeTaintsFunction();
    testCallBarrier();
    testAsmBarrier();
    testVolatileUntouched();
    testMismatchedAccess();
    testMem2RegVolatile();
    testMem2RegMismatched();
    testPhiStructureValid();
    testPhiStructureErrors();
    testVaOperationStructure();
    testVariadicPipeline();
    testPhiLowering();
    testRodataDedup();
    testRodataDedupPinsAssembly();
    testRodataDedupAddressIdentity();
    testRodataDedupDeterministic();
    testDisplacementEscapes();
    testDanglingTarget();
    testUnterminatedBlock();
    testUndefinedOperand();
    testDoubleDefine();
    testPipelineRefuses();
    std::printf("\n%d checks, %s\n", checks, failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
