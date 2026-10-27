// dce_test.cpp — whole-program definition pruning.
//
// The pass removes unreachable function, global, and rodata definitions. It
// keeps extern declarations and resolves the three kinds together, because a
// live global can name a function and a function can name a global. These
// tests build IR directly so each root kind and each dead cycle can be
// isolated.

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

cider::ir::Instr makeDataAddr(const std::string& label) {
    cider::ir::Instr addr;
    addr.op = cider::ir::Op::DataAddr;
    addr.globalLabel = label;
    addr.results.push_back(cider::ir::Value{1, cider::ir::IRType::Ptr, {}});
    return addr;
}

cider::ir::Instr makeLoadGlobal(const std::string& label) {
    cider::ir::Instr load;
    load.op = cider::ir::Op::Load;
    load.globalLabel = label;
    load.width = cider::ir::IRType::I64;
    load.isPure = true;
    load.results.push_back(cider::ir::Value{1, cider::ir::IRType::I64, {}});
    return load;
}

cider::ir::Instr makeStoreGlobal(const std::string& label) {
    cider::ir::Instr store;
    store.op = cider::ir::Op::Store;
    store.globalLabel = label;
    store.width = cider::ir::IRType::I64;
    store.storedValue = cider::ir::Value{1, cider::ir::IRType::I64, {}};
    return store;
}

cider::ir::Instr makeAsm(const std::string& text) {
    cider::ir::Instr asmInstr;
    asmInstr.op = cider::ir::Op::Asm;
    asmInstr.asmText = text;
    return asmInstr;
}

cider::ir::Global makeGlobal(const std::string& label) {
    cider::ir::Global global;
    global.label = label;
    return global;
}

cider::ir::Rodata makeRodata(const std::string& label,
                             const std::string& bytes) {
    cider::ir::Rodata blob;
    blob.label = label;
    // The lowerer NUL-terminates every literal; mirror that here so byte
    // counters include the terminator exactly as the real module does.
    blob.bytes = bytes + '\0';
    return blob;
}

bool hasFunction(const cider::ir::Module& module, const std::string& name) {
    for (const cider::ir::Function& fn : module.functions) {
        if (fn.name == name) {
            return true;
        }
    }
    return false;
}

bool hasGlobal(const cider::ir::Module& module, const std::string& label) {
    for (const cider::ir::Global& global : module.globals) {
        if (global.label == label) {
            return true;
        }
    }
    return false;
}

bool hasRodata(const cider::ir::Module& module, const std::string& label) {
    for (const cider::ir::Rodata& blob : module.rodata) {
        if (blob.label == label) {
            return true;
        }
    }
    return false;
}

std::string globalLabels(const cider::ir::Module& module) {
    std::string labels;
    for (const cider::ir::Global& global : module.globals) {
        labels += global.label + ",";
    }
    return labels;
}

std::string rodataLabels(const cider::ir::Module& module) {
    std::string labels;
    for (const cider::ir::Rodata& blob : module.rodata) {
        labels += blob.label + ",";
    }
    return labels;
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
    module.moduleAsm = "call module_helper\ncall cs_\n";

    cider::ir::Function entry = makeFunction("_start");
    entry.blocks[0].body.push_back(makeFnAddr("addressed"));
    entry.blocks[0].body.push_back(makeAsm("call asm_helper\n"));
    entry.blocks[0].body.push_back(makeCall("indirect_target", true));
    module.functions.push_back(std::move(entry));

    module.functions.push_back(makeFunction("addressed"));
    module.functions.push_back(makeFunction("asm_helper"));
    module.functions.push_back(makeFunction("module_helper"));
    module.functions.push_back(makeFunction("cs"));
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
    check(hasFunction(module, "cs"),
          "an emitted register-spelled name roots its source function");
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

void testWholeProgramMissingEntryKeepsData() {
    std::printf("whole-program missing entry\n");
    cider::ir::Module module;
    module.name = "main";
    module.functions.push_back(makeFunction("one"));
    module.globals.push_back(makeGlobal("g_one"));
    module.rodata.push_back(makeRodata("s_one", "one"));

    cider::opt::WholeProgramPruneResult result =
        cider::opt::pruneWholeProgram(module, "_start");
    check(result.skipped, "a missing entry skips whole-program pruning");
    check(hasFunction(module, "one") && hasGlobal(module, "g_one") &&
              hasRodata(module, "s_one"),
          "functions, globals, and rodata are all left unchanged");
    check(result.functionsRemoved == 0 && result.globalsRemoved == 0 &&
              result.rodataRemoved == 0,
          "no whole-program counter moves when pruning is refused");
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

void testDirectDataLiveness() {
    std::printf("direct data liveness\n");
    cider::ir::Module module;
    module.name = "main";

    cider::ir::Function entry = makeFunction("_start");
    entry.blocks[0].body.push_back(makeDataAddr("g_direct"));
    entry.blocks[0].body.push_back(makeLoadGlobal("g_loaded"));
    entry.blocks[0].body.push_back(makeStoreGlobal("g_stored"));
    entry.blocks[0].body.push_back(makeDataAddr("s_used"));
    module.functions.push_back(std::move(entry));
    module.functions.push_back(makeFunction("external", true));

    module.globals.push_back(makeGlobal("g_direct"));
    module.globals.push_back(makeGlobal("g_loaded"));
    module.globals.push_back(makeGlobal("g_stored"));
    module.globals.push_back(makeGlobal("g_unused"));
    module.rodata.push_back(makeRodata("s_used", "used"));
    module.rodata.push_back(makeRodata("s_unused", "unused"));

    cider::opt::WholeProgramPruneResult result =
        cider::opt::pruneWholeProgram(module, "_start");
    check(!result.skipped, "the entry is present");
    check(hasGlobal(module, "g_direct"), "a DataAddr target survives");
    check(hasGlobal(module, "g_loaded"), "a loaded global survives");
    check(hasGlobal(module, "g_stored"), "a stored global survives");
    check(!hasGlobal(module, "g_unused"), "an unreferenced global is removed");
    check(hasRodata(module, "s_used"), "a DataAddr rodata target survives");
    check(!hasRodata(module, "s_unused"), "unreferenced rodata is removed");
    check(hasFunction(module, "external"), "an extern declaration survives");
    check(result.globalsRemoved == 1, "one global is counted");
    check(result.rodataRemoved == 1, "one rodata blob is counted");
    check(result.rodataBytesRemoved == 7, "removed rodata bytes are counted");
    check(result.functionsRemoved == 0, "no function is removed");
    check(globalLabels(module) == "g_direct,g_loaded,g_stored,",
          "global order is preserved");
    check(rodataLabels(module) == "s_used,", "rodata order is preserved");
}

void testGlobalDependencyChain() {
    std::printf("global dependency chain\n");
    cider::ir::Module module;
    module.name = "main";

    cider::ir::Function entry = makeFunction("_start");
    entry.blocks[0].body.push_back(makeDataAddr("g_root"));
    module.functions.push_back(std::move(entry));

    cider::ir::Function vtableFn = makeFunction("fn_vtable");
    vtableFn.blocks[0].body.push_back(makeDataAddr("g_from_function"));
    module.functions.push_back(std::move(vtableFn));
    module.functions.push_back(makeFunction("fn_method"));
    module.functions.push_back(makeFunction("fn_dead"));

    cider::ir::Global root = makeGlobal("g_root");
    root.initLabel = "s_string";
    root.initLabels.push_back("fn_vtable");
    root.relocWords.push_back({0, "vtable_Class"});
    module.globals.push_back(std::move(root));

    cider::ir::Global vtable = makeGlobal("vtable_Class");
    vtable.initLabel = "s_nested";
    vtable.initLabels.push_back("fn_method");
    module.globals.push_back(std::move(vtable));

    cider::ir::Global fromFunction = makeGlobal("g_from_function");
    fromFunction.initLabel = "s_deep";
    module.globals.push_back(std::move(fromFunction));
    module.globals.push_back(makeGlobal("g_unused"));

    module.rodata.push_back(makeRodata("s_string", "str"));
    module.rodata.push_back(makeRodata("s_nested", "nested"));
    module.rodata.push_back(makeRodata("s_deep", "deep"));
    module.rodata.push_back(makeRodata("s_unused", "unused"));

    cider::opt::WholeProgramPruneResult result =
        cider::opt::pruneWholeProgram(module, "_start");
    check(!result.skipped, "the entry is present");
    check(hasGlobal(module, "g_root"), "a directly referenced global survives");
    check(hasGlobal(module, "vtable_Class"),
          "a vtable referenced only by a live global survives");
    check(hasGlobal(module, "g_from_function"),
          "a global referenced only by a live function survives");
    check(!hasGlobal(module, "g_unused"), "the unrelated global is removed");
    check(hasFunction(module, "fn_vtable"),
          "a function in a live global's initLabels survives");
    check(hasFunction(module, "fn_method"),
          "a function reached through the vtable survives");
    check(!hasFunction(module, "fn_dead"), "the unrelated function is removed");
    check(hasRodata(module, "s_string"),
          "rodata named by a live global's initLabel survives");
    check(hasRodata(module, "s_nested"),
          "rodata named by a live vtable's initLabel survives");
    check(hasRodata(module, "s_deep"),
          "rodata named inside an initializer chain survives");
    check(!hasRodata(module, "s_unused"), "the unrelated rodata is removed");
    check(result.functionsRemoved == 1, "one function is counted");
    check(result.globalsRemoved == 1, "one global is counted");
    check(result.rodataRemoved == 1, "one rodata blob is counted");
}

void testDeadFunctionGlobalCycle() {
    std::printf("dead function/global cycle\n");
    cider::ir::Module module;
    module.name = "main";

    cider::ir::Function entry = makeFunction("_start");
    entry.blocks[0].body.push_back(makeDataAddr("g_alive"));
    entry.blocks[0].body.push_back(makeDataAddr("s_alive"));
    module.functions.push_back(std::move(entry));

    cider::ir::Function orphan = makeFunction("orphan_fn");
    orphan.blocks[0].body.push_back(makeCall("fn_other"));
    orphan.blocks[0].body.push_back(makeLoadGlobal("g_back"));
    module.functions.push_back(std::move(orphan));

    cider::ir::Function other = makeFunction("fn_other");
    other.blocks[0].body.push_back(makeDataAddr("s_fn"));
    module.functions.push_back(std::move(other));

    cider::ir::Global dead = makeGlobal("g_dead");
    dead.initLabel = "s_dead";
    dead.initLabels.push_back("orphan_fn");
    module.globals.push_back(std::move(dead));

    cider::ir::Global back = makeGlobal("g_back");
    back.initLabel = "s_back";
    back.relocWords.push_back({0, "g_dead"});
    module.globals.push_back(std::move(back));
    module.globals.push_back(makeGlobal("g_alive"));

    module.rodata.push_back(makeRodata("s_dead", "dead"));
    module.rodata.push_back(makeRodata("s_back", "back"));
    module.rodata.push_back(makeRodata("s_fn", "fn"));
    module.rodata.push_back(makeRodata("s_alive", "alive"));

    cider::opt::WholeProgramPruneResult result =
        cider::opt::pruneWholeProgram(module, "_start");
    check(!result.skipped, "the entry is present");
    check(!hasFunction(module, "orphan_fn"),
          "a function reachable only from a dead global is removed");
    check(!hasFunction(module, "fn_other"),
          "a callee reachable only through that dead function is removed");
    check(!hasGlobal(module, "g_dead"), "the dead global is removed");
    check(!hasGlobal(module, "g_back"),
          "a global in a dead cycle is removed, not self-sustaining");
    check(!hasRodata(module, "s_dead"), "the dead global's rodata is removed");
    check(!hasRodata(module, "s_back"), "the cycle's rodata is removed");
    check(!hasRodata(module, "s_fn"), "the dead function's rodata is removed");
    check(hasGlobal(module, "g_alive"), "a live global is untouched");
    check(hasRodata(module, "s_alive"), "live rodata is untouched");
    check(result.functionsRemoved == 2, "two functions are counted");
    check(result.instructionsRemoved == 3, "every removed instruction is counted");
    check(result.globalsRemoved == 2, "two globals are counted");
    check(result.rodataRemoved == 3, "three rodata blobs are counted");
    check(result.rodataBytesRemoved == 13, "their bytes are counted");
}

void testAssemblyRootsForData() {
    std::printf("assembly roots for data\n");
    cider::ir::Module module;
    module.name = "main";
    module.moduleAsm =
        "  call asm_module_fn\n"
        "  .quad g_asm_module\n"
        "  .quad s_asm_module\n";

    cider::ir::Function entry = makeFunction("_start");
    entry.blocks[0].body.push_back(makeAsm(
        "mov rax, [rip + g_asm_inline]\n"
        "mov rbx, [rip + s_asm_inline]\n"
        "call asm_inline_fn\n"));
    module.functions.push_back(std::move(entry));

    cider::ir::Function dead = makeFunction("dead_asm_fn");
    dead.blocks[0].body.push_back(makeAsm(
        "mov rax, [rip + g_dead_asm]\n"
        "mov rbx, [rip + s_dead_asm]\n"
        "call dead_asm_target\n"));
    module.functions.push_back(std::move(dead));
    module.functions.push_back(makeFunction("dead_asm_target"));
    module.functions.push_back(makeFunction("asm_module_fn"));
    module.functions.push_back(makeFunction("asm_inline_fn"));

    module.globals.push_back(makeGlobal("g_asm_module"));
    module.globals.push_back(makeGlobal("g_asm_inline"));
    module.globals.push_back(makeGlobal("g_dead_asm"));
    module.globals.push_back(makeGlobal("g_unused"));
    module.rodata.push_back(makeRodata("s_asm_module", "module"));
    module.rodata.push_back(makeRodata("s_asm_inline", "inline"));
    module.rodata.push_back(makeRodata("s_dead_asm", "dead"));
    module.rodata.push_back(makeRodata("s_unused", "unused"));
    module.rodata.push_back(makeRodata("s_asm_module_extra", "extra"));

    cider::opt::WholeProgramPruneResult result =
        cider::opt::pruneWholeProgram(module, "_start");
    check(!result.skipped, "the entry is present");
    check(hasGlobal(module, "g_asm_module"),
          "module asm roots a global");
    check(hasRodata(module, "s_asm_module"),
          "module asm roots rodata");
    check(hasFunction(module, "asm_module_fn"),
          "module asm roots a function");
    check(hasGlobal(module, "g_asm_inline"),
          "inline asm roots a global");
    check(hasRodata(module, "s_asm_inline"),
          "inline asm roots rodata");
    check(hasFunction(module, "asm_inline_fn"),
          "inline asm roots a function");
    check(!hasGlobal(module, "g_dead_asm"),
          "asm inside a dead function roots nothing");
    check(!hasRodata(module, "s_dead_asm"),
          "rodata named only by dead asm is removed");
    check(!hasFunction(module, "dead_asm_target"),
          "a function named only by dead asm is removed");
    check(!hasGlobal(module, "g_unused"), "the unrelated global is removed");
    check(!hasRodata(module, "s_unused"), "the unrelated rodata is removed");
    check(!hasRodata(module, "s_asm_module_extra"),
          "a symbol-boundary near match does not root the wrong blob");
    check(result.functionsRemoved == 2, "two dead asm functions are counted");
    check(result.globalsRemoved == 2, "two asm-dead globals are counted");
    check(result.rodataRemoved == 3, "three asm-dead blobs are counted");
}

void testFunctionOnlyKeepsData() {
    std::printf("function-only path keeps data\n");
    cider::ir::Module module;
    module.name = "main";

    cider::ir::Function entry = makeFunction("_start");
    module.functions.push_back(std::move(entry));
    module.functions.push_back(makeFunction("dead_fn"));
    module.functions.push_back(makeFunction("fn_from_global"));

    cider::ir::Global unused = makeGlobal("g_unused");
    unused.initLabel = "s_unused";
    unused.initLabels.push_back("fn_from_global");
    module.globals.push_back(std::move(unused));
    module.rodata.push_back(makeRodata("s_unused", "keep"));

    cider::opt::FunctionPruneResult result =
        cider::opt::pruneFunctions(module, "_start");
    check(!result.skipped, "the entry is present");
    check(!hasFunction(module, "dead_fn"), "the dead function is removed");
    check(hasFunction(module, "fn_from_global"),
          "a global's initializer label still roots a function");
    check(hasGlobal(module, "g_unused"),
          "the function-only path keeps an unreferenced global");
    check(hasRodata(module, "s_unused"),
          "the function-only path keeps unreferenced rodata");
    check(result.functionsRemoved == 1, "one function is counted");
}

cider::ir::Module makeOrderModule() {
    cider::ir::Module module;
    module.name = "main";

    cider::ir::Function entry = makeFunction("_start");
    entry.blocks[0].body.push_back(makeDataAddr("g_b"));
    entry.blocks[0].body.push_back(makeDataAddr("g_d"));
    entry.blocks[0].body.push_back(makeDataAddr("s_b"));
    entry.blocks[0].body.push_back(makeDataAddr("s_d"));
    module.functions.push_back(std::move(entry));

    module.globals.push_back(makeGlobal("g_a"));
    module.globals.push_back(makeGlobal("g_b"));
    module.globals.push_back(makeGlobal("g_c"));
    module.globals.push_back(makeGlobal("g_d"));
    module.rodata.push_back(makeRodata("s_a", "a"));
    module.rodata.push_back(makeRodata("s_b", "b"));
    module.rodata.push_back(makeRodata("s_c", "c"));
    module.rodata.push_back(makeRodata("s_d", "d"));
    return module;
}

void testDeterministicOrder() {
    std::printf("deterministic order\n");
    cider::ir::Module first = makeOrderModule();
    cider::ir::Module second = makeOrderModule();

    cider::opt::WholeProgramPruneResult firstResult =
        cider::opt::pruneWholeProgram(first, "_start");
    cider::opt::WholeProgramPruneResult secondResult =
        cider::opt::pruneWholeProgram(second, "_start");
    check(globalLabels(first) == "g_b,g_d,",
          "live globals keep their original relative order");
    check(rodataLabels(first) == "s_b,s_d,",
          "live rodata keeps its original relative order");
    check(firstResult.globalsRemoved == 2 && firstResult.rodataRemoved == 2,
          "both runs remove the same definitions");
    check(firstResult.globalsRemoved == secondResult.globalsRemoved &&
              firstResult.rodataRemoved == secondResult.rodataRemoved,
          "counters do not depend on insertion order");
    check(cider::ir::dumpModule(first) == cider::ir::dumpModule(second),
          "two identical modules prune byte-identically");
}

}  // namespace

int main() {
    std::printf("dce_test — whole-program definition pruning\n\n");
    testReachableChain();
    testAddressAndAssemblyRoots();
    testMissingEntryKeepsEverything();
    testWholeProgramMissingEntryKeepsData();
    testDuplicateNamesStayTogether();
    testDirectDataLiveness();
    testGlobalDependencyChain();
    testDeadFunctionGlobalCycle();
    testAssemblyRootsForData();
    testFunctionOnlyKeepsData();
    testDeterministicOrder();
    std::printf("\n%d checks, %s\n", checks,
                failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
