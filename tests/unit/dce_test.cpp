// dce_test.cpp — whole-program function pruning.
//
// The pass is deliberately small: it removes only function definitions the
// entry cannot reach, and it keeps globals/rodata and extern declarations.
// These tests build IR directly so each root kind can be isolated.

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "ir/ir.h"
#include "opt/opt.h"

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

cider::ir::Function makeFunction(const std::string& name, bool isExtern = false) {
    cider::ir::Function fn;
    fn.name = name;
    fn.isExtern = isExtern;
    if (!isExtern) {
        cider::ir::Block block;
        block.label = 1;
        block.term.term = cider::ir::Term::Ret;
        fn.blocks.push_back(std::move(block));
    }
    return fn;
}

cider::ir::Instr makeCall(const std::string& callee, bool indirect = false) {
    cider::ir::Instr call;
    call.op = cider::ir::Op::Call;
    call.callee = callee;
    call.call.indirect = indirect;
    return call;
}

cider::ir::Instr makeFnAddr(const std::string& label) {
    cider::ir::Instr addr;
    addr.op = cider::ir::Op::FnAddr;
    addr.globalLabel = label;
    addr.results.push_back(cider::ir::Value{1, cider::ir::IRType::Ptr, {}});
    return addr;
}

cider::ir::Instr makeAsm(const std::string& text) {
    cider::ir::Instr asmInstr;
    asmInstr.op = cider::ir::Op::Asm;
    asmInstr.asmText = text;
    return asmInstr;
}

bool hasFunction(const cider::ir::Module& module, const std::string& name) {
    for (const cider::ir::Function& fn : module.functions) {
        if (fn.name == name) {
            return true;
        }
    }
    return false;
}

std::size_t functionCount(const cider::ir::Module& module) {
    return module.functions.size();
}

void testReachableChain() {
    std::printf("reachable chain\n");
    cider::ir::Module module;
    module.name = "main";

    cider::ir::Function entry = makeFunction("_start");
    entry.blocks[0].body.push_back(makeCall("helper"));
    module.functions.push_back(std::move(entry));

    cider::ir::Function helper = makeFunction("helper");
    helper.blocks[0].body.push_back(makeCall("leaf"));
    module.functions.push_back(std::move(helper));

    cider::ir::Function leaf = makeFunction("leaf");
    module.functions.push_back(std::move(leaf));

    cider::ir::Function dead = makeFunction("dead");
    dead.blocks[0].body.push_back(makeCall("orphan"));
    module.functions.push_back(std::move(dead));

    module.functions.push_back(makeFunction("orphan"));
    module.functions.push_back(makeFunction("unused"));

    cider::opt::FunctionPruneResult result =
        cider::opt::pruneFunctions(module, "_start");
    check(!result.skipped, "the entry is present");
    check(hasFunction(module, "_start"), "the entry survives");
    check(hasFunction(module, "helper"), "a directly called function survives");
    check(hasFunction(module, "leaf"), "a transitive callee survives");
    check(!hasFunction(module, "dead"), "an unreachable definition is removed");
    check(!hasFunction(module, "orphan"),
          "a callee reachable only from a dead function is removed");
    check(!hasFunction(module, "unused"), "an isolated definition is removed");
    check(result.functionsRemoved == 3, "the counter reports three removals");
}

void testAddressAndAssemblyRoots() {
    std::printf("address and assembly roots\n");
    cider::ir::Module module;
    module.name = "main";
    module.moduleAsm = "call module_helper\n";

    cider::ir::Function entry = makeFunction("_start");
    entry.blocks[0].body.push_back(makeFnAddr("addressed"));
    entry.blocks[0].body.push_back(makeAsm("call asm_helper\n"));
    entry.blocks[0].body.push_back(makeCall("indirect_target", true));
    module.functions.push_back(std::move(entry));

    module.functions.push_back(makeFunction("addressed"));
    module.functions.push_back(makeFunction("asm_helper"));
    module.functions.push_back(makeFunction("module_helper"));
    module.functions.push_back(makeFunction("vtable_fn"));
    module.functions.push_back(makeFunction("reloc_fn"));
    module.functions.push_back(makeFunction("forwarder"));
    module.functions.push_back(makeFunction("external", true));
    module.functions.push_back(makeFunction("indirect_target"));
    module.functions.push_back(makeFunction("dead"));

    cider::ir::Global global;
    global.label = "_ZTV1C";
    global.initLabel = "forwarder";
    global.initLabels.push_back("vtable_fn");
    global.relocWords.push_back({0, "reloc_fn"});
    module.globals.push_back(std::move(global));

    cider::opt::FunctionPruneResult result =
        cider::opt::pruneFunctions(module, "_start");
    check(!result.skipped, "the entry is present");
    check(hasFunction(module, "addressed"), "a FnAddr target survives");
    check(hasFunction(module, "asm_helper"),
          "a function named by inline asm survives");
    check(hasFunction(module, "module_helper"),
          "a function named by module asm survives");
    check(hasFunction(module, "vtable_fn"),
          "a global initializer label survives");
    check(hasFunction(module, "reloc_fn"),
          "a relocated global word survives");
    check(hasFunction(module, "forwarder"),
          "a function-valued global's forwarding thunk survives");
    check(hasFunction(module, "external"), "an extern declaration survives");
    check(!hasFunction(module, "indirect_target"),
          "an indirect call does not root a guessed callee");
    check(!hasFunction(module, "dead"), "an unrelated definition is removed");
}

void testMissingEntryKeepsEverything() {
    std::printf("missing entry\n");
    cider::ir::Module module;
    module.name = "main";
    module.functions.push_back(makeFunction("one"));
    module.functions.push_back(makeFunction("two"));

    cider::opt::FunctionPruneResult result =
        cider::opt::pruneFunctions(module, "_start");
    check(result.skipped, "a missing entry skips pruning");
    check(functionCount(module) == 2, "the module is left unchanged");
    check(result.functionsRemoved == 0, "nothing is counted as removed");
}

void testDuplicateNamesStayTogether() {
    std::printf("duplicate names\n");
    cider::ir::Module module;
    module.name = "main";

    cider::ir::Function entry = makeFunction("_start");
    entry.blocks[0].body.push_back(makeCall("both"));
    module.functions.push_back(std::move(entry));

    module.functions.push_back(makeFunction("both"));
    module.functions.push_back(makeFunction("both"));
    module.functions.push_back(makeFunction("unused"));

    cider::opt::FunctionPruneResult result =
        cider::opt::pruneFunctions(module, "_start");
    std::size_t both = 0;
    for (const cider::ir::Function& fn : module.functions) {
        if (fn.name == "both") {
            ++both;
        }
    }
    check(both == 2, "both definitions sharing a live name survive");
    check(!hasFunction(module, "unused"), "the unrelated definition is removed");
    check(result.functionsRemoved == 1, "one definition is counted");
}

}  // namespace

int main() {
    std::printf("dce_test — whole-program function pruning\n\n");
    testReachableChain();
    testAddressAndAssemblyRoots();
    testMissingEntryKeepsEverything();
    testDuplicateNamesStayTogether();
    std::printf("\n%d checks, %s\n", checks,
                failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
