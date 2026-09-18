#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "constant_folder.h"
#include "dominators.h"
#include "gvn.h"
#include "inliner.h"
#include "ir_gen.h"
#include "lexer.h"
#include "licm.h"
#include "module_resolver.h"
#include "optimizer.h"
#include "parser.h"
#include "symbol_collector.h"
#include "symbols.h"

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

struct ModuleResult {
    cider::ModuleIR module;
    std::string print() {
        std::ostringstream ss;
        std::streambuf* old = std::cout.rdbuf(ss.rdbuf());
        module.print();
        std::cout.rdbuf(old);
        return ss.str();
    }
};

ModuleResult genModule(const std::string& src) {
    cider::ErrorReporter rep;
    cider::Lexer lex(src, rep);
    cider::Parser parser(lex, rep);
    auto prog = parser.parseProgram();
    cider::SymbolTable table;
    cider::ModuleResolver resolver;
    cider::SymbolCollector collector(table, rep, resolver);
    collector.collect(*prog);
    cider::ConstantFolder folder;
    folder.fold(*prog);
    cider::IRGenerator gen(table);
    ModuleResult r;
    r.module = gen.generate(*prog);
    return r;
}

int testDominators() {
    // entry -> A/B (cond) -> merge -> exit; plus a back edge merge -> A.
    auto fn = std::make_unique<cider::FunctionIR>("f", cider::IRType::VOID);
    auto entry = std::make_unique<cider::BasicBlock>("entry");
    cider::IRInstruction br1;
    br1.op = cider::IROp::COND_BR;
    br1.args = {cider::IRValue::Constant(1, cider::IRType::I32),
                cider::IRValue::Label("A"), cider::IRValue::Label("B")};
    entry->addInstruction(br1);
    auto a = std::make_unique<cider::BasicBlock>("A");
    cider::IRInstruction br2;
    br2.op = cider::IROp::BR;
    br2.args = {cider::IRValue::Label("merge")};
    a->addInstruction(br2);
    auto b = std::make_unique<cider::BasicBlock>("B");
    cider::IRInstruction brToA;
    brToA.op = cider::IROp::BR;
    brToA.args = {cider::IRValue::Label("A")};
    b->addInstruction(brToA);
    auto merge = std::make_unique<cider::BasicBlock>("merge");
    cider::IRInstruction br3;
    br3.op = cider::IROp::COND_BR;
    br3.args = {cider::IRValue::Constant(0, cider::IRType::I32),
                cider::IRValue::Label("A"), cider::IRValue::Label("exit")};
    merge->addInstruction(br3);
    auto exit = std::make_unique<cider::BasicBlock>("exit");
    fn->addBlock(std::move(entry));
    fn->addBlock(std::move(a));
    fn->addBlock(std::move(b));
    fn->addBlock(std::move(merge));
    fn->addBlock(std::move(exit));

    cider::DominatorTree dt;
    dt.build(*fn);
    CHECK(dt.dominates("entry", "A"));
    CHECK(dt.dominates("entry", "merge"));
    CHECK(!dt.dominates("A", "B"));
    CHECK(!dt.dominates("B", "A"));
    CHECK(dt.dominates("merge", "exit"));
    CHECK(!dt.dominates("exit", "merge"));

    // Loop detection on merge -> A -> merge.
    cider::NaturalLoopFinder loops;
    loops.build(*fn, dt);
    const auto& ls = loops.getLoops();
    CHECK(!ls.empty());
    bool found = false;
    for (const auto& l : ls) {
        if (l.header == "A") {
            found = true;
            CHECK(l.blocks.count("merge") == 1);
        }
    }
    CHECK(found);
    return 0;
}

int testDce() {
    auto fn = std::make_unique<cider::FunctionIR>("f", cider::IRType::VOID);
    auto block = std::make_unique<cider::BasicBlock>("entry");
    cider::IRInstruction dead;
    dead.op = cider::IROp::ADD;
    dead.dest = cider::IRValue::Register("d", cider::IRType::I32);
    dead.args = {cider::IRValue::Constant(1, cider::IRType::I32),
                 cider::IRValue::Constant(2, cider::IRType::I32)};
    block->addInstruction(dead);
    cider::IRInstruction live;
    live.op = cider::IROp::MOV;
    live.dest = cider::IRValue::Register("l", cider::IRType::I32);
    live.args = {cider::IRValue::Constant(5, cider::IRType::I32)};
    block->addInstruction(live);
    cider::IRInstruction ret;
    ret.op = cider::IROp::RET;
    ret.args = {cider::IRValue::Register("l", cider::IRType::I32)};
    block->addInstruction(ret);
    fn->addBlock(std::move(block));
    cider::Optimizer::dce(*fn);
    const auto& instrs = fn->getBlocks()[0]->getInstructions();
    CHECK(instrs.size() == 2);  // dead add removed
    CHECK(instrs[0].dest->name == "l");
    return 0;
}

int testConstFold() {
    // var x: int = 2 + 3 * 4;  -> folded at the AST level before ir-gen.
    auto r = genModule("func main() -> void {\n  var x: int = 2 + 3 * 4;\n}\n");
    std::string d = r.print();
    CHECK(d.find("store %v0, 14") != std::string::npos);
    return 0;
}

int testGvn() {
    auto fn = std::make_unique<cider::FunctionIR>("f", cider::IRType::I32);
    auto block = std::make_unique<cider::BasicBlock>("entry");
    auto addInstr = [](const std::string& dest) {
        cider::IRInstruction in;
        in.op = cider::IROp::ADD;
        in.dest = cider::IRValue::Register(dest, cider::IRType::I32);
        in.args = {cider::IRValue::Register("x", cider::IRType::I32),
                   cider::IRValue::Constant(1, cider::IRType::I32)};
        return in;
    };
    block->addInstruction(addInstr("a"));
    block->addInstruction(addInstr("b"));
    cider::IRInstruction ret;
    ret.op = cider::IROp::RET;
    ret.args = {cider::IRValue::Register("b", cider::IRType::I32)};
    block->addInstruction(ret);
    fn->addBlock(std::move(block));
    cider::ModuleIR mod;
    mod.addFunction(std::move(fn));
    cider::GvnPass gvn;
    CHECK(gvn.run(mod));
    const auto& instrs = mod.getFunctions()[0]->getBlocks()[0]->getInstructions();
    CHECK(instrs[0].op == cider::IROp::ADD);  // first kept
    CHECK(instrs[1].op == cider::IROp::MOV);  // second deduped to a mov
    CHECK(instrs[1].args[0].name == "a");
    return 0;
}

int testLicm() {
    // while (i < 10) { j = 2 * 3; }  -> the mul hoists to the preheader.
    auto r = genModule(
        "func main() -> void {\n"
        "  var i: int = 0;\n"
        "  var j: int = 0;\n"
        "  while (i < 10) {\n"
        "    j = 2 * 3;\n"
        "    i = i + 1;\n"
        "  }\n"
        "}\n");
    std::string before = r.print();
    cider::SSALicmPass licm;
    CHECK(licm.run(r.module));
    std::string after = r.print();
    CHECK(after.find("mul") != std::string::npos);            // still present
    CHECK(after.find("_preheader") != std::string::npos);     // preheader created
    return 0;
}

int testInliner() {
    // A small helper gets inlined; recursion does not.
    auto r = genModule(
        "func helper(a: int) -> int {\n  return a + 1;\n}\n"
        "func main() -> int {\n  return helper(41);\n}\n");
    std::string before = r.print();
    cider::Inliner<> inliner;
    CHECK(inliner.run(r.module));
    std::string after = r.print();
    CHECK(after.find("call helper") == std::string::npos);  // call gone
    CHECK(after.find("helper_") != std::string::npos);      // cloned blocks present
    return 0;
}

int run() {
    CHECK(testDominators() == 0);
    CHECK(testDce() == 0);
    CHECK(testConstFold() == 0);
    CHECK(testGvn() == 0);
    CHECK(testLicm() == 0);
    CHECK(testInliner() == 0);
    return 0;
}

int main() { return run(); }
