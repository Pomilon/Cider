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
};

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
    auto resolver = std::make_unique<cider::resolve::Resolver>(*out.types);
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
    cider::opt::OptResult result = cider::opt::run(*out.module);
    if (!result.ok) {
        out.stage = "opt:" + result.error;
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

int main() {
    std::printf("opt_test — malformed IR is refused, passes preserve meaning\n\n");
    testWellFormed();
    testFold();
    testFoldWidths();
    testDeadCode();
    testIndirectKept();
    testTrapKept();
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
    testDisplacementEscapes();
    testDanglingTarget();
    testUnterminatedBlock();
    testUndefinedOperand();
    testDoubleDefine();
    testPipelineRefuses();
    std::printf("\n%d checks, %s\n", checks, failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
