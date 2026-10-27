// Whole-program dead-definition elimination.
//
// This pass removes function, global, and rodata definitions that the program
// entry cannot reach. It is intentionally limited to whole-program builds: a
// split-module object must keep every definition because another object may
// reference it. The driver decides when that promise holds.
//
// Functions, globals, and rodata are one reachability problem, not three:
//   - a live function may name another function (`Call`, `FnAddr`, or inline
//     assembly), a global (`Load`/`Store`/`DataAddr`/`Gep`), or rodata (a
//     `DataAddr` naming a string blob);
//   - a live global may name a function (`initLabels`, or a forwarding thunk
//     in `initLabel`), another global (a class vtable in `relocWords`), or
//     rodata (a string or wide literal in `initLabel`);
//   - rodata names nothing.
//
// Resolving them together is what keeps a dead cycle dead. A global can point
// at a function and that function back at the global; treating either side as
// a root independently would keep both alive forever. Only the entry,
// module-level assembly, and references from symbols already proved live are
// roots.
//
// Assembly is opaque to the IR, so a symbol named there is kept even if no IR
// operand mentions it. Matching is symbol-boundary aware: `call helper` roots
// `helper`, but `helper2` and `foo_helper` do not. Extern declarations are
// always kept; they have no body and are harmless. Any reference the analysis
// cannot classify is left alone rather than guessed at.

#include "opt/opt.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cider::opt {
namespace {

bool isSymbolChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

// Visits each maximal symbol-shaped word in `text`. GAS separates symbols
// with everything outside this alphabet, so one word at a time is exactly the
// boundary-aware match this pass documents: `helper2` does not mention
// `helper`, and `foo_helper` does not either.
template <typename Visit>
void forEachSymbolWord(const std::string& text, Visit visit) {
    std::uint64_t at = 0;
    while (at < text.size()) {
        if (!isSymbolChar(text[at])) {
            ++at;
            continue;
        }
        const std::uint64_t start = at;
        while (at < text.size() && isSymbolChar(text[at])) {
            ++at;
        }
        visit(text.substr(static_cast<std::size_t>(start),
                          static_cast<std::size_t>(at - start)));
    }
}

struct PruneStats {
    uint64_t functionsRemoved = 0;
    uint64_t instructionsRemoved = 0;
    uint64_t globalsRemoved = 0;
    uint64_t rodataRemoved = 0;
    uint64_t rodataBytesRemoved = 0;
    bool skipped = false;
};

// The one engine behind both entry points. When `pruneData` is false every
// global and rodata entry starts live and stays in the module, which is the
// old function-only contract: their initializer labels still root functions.
PruneStats prune(ir::Module& module, const std::string& entry, bool pruneData) {
    PruneStats stats;

    std::unordered_map<std::string, std::vector<const ir::Function*>> functions;
    for (const ir::Function& fn : module.functions) {
        functions[fn.name].push_back(&fn);
    }
    if (functions.find(entry) == functions.end()) {
        // `run` and whole-program `compile` require an entry before this
        // pass; a missing one means the caller is not in the mode this
        // whole-program pass is allowed to prune. Leave the module alone.
        stats.skipped = true;
        return stats;
    }

    std::unordered_map<std::string, std::vector<const ir::Global*>> globals;
    for (const ir::Global& global : module.globals) {
        globals[global.label].push_back(&global);
    }
    std::unordered_set<std::string> rodataNames;
    for (const ir::Rodata& blob : module.rodata) {
        rodataNames.insert(blob.label);
    }

    std::unordered_set<std::string> liveFunctions;
    std::unordered_set<std::string> liveGlobals;
    std::unordered_set<std::string> liveRodata;
    std::vector<std::string> functionWork;
    std::vector<std::string> globalWork;

    auto root = [&](const std::string& name) {
        if (name.empty()) {
            return;
        }
        if (functions.find(name) != functions.end() &&
            liveFunctions.insert(name).second) {
            functionWork.push_back(name);
        }
        if (globals.find(name) != globals.end() &&
            liveGlobals.insert(name).second) {
            globalWork.push_back(name);
        }
        if (rodataNames.find(name) != rodataNames.end()) {
            liveRodata.insert(name);
        }
    };

    auto rootAssembly = [&](const std::string& text) {
        forEachSymbolWord(text, [&](const std::string& word) {
            root(word);
            // Codegen appends one `_` to a function whose spelling GAS would
            // read as a register (`call cs` names the segment register). The
            // emitted spelling is therefore `cs_`, and assembly written
            // against it names the source symbol with that suffix. Rooting
            // the prefix too is conservative: it can only keep a definition
            // that might otherwise disappear.
            if (word.length() > 1 && word.back() == '_') {
                root(word.substr(0, word.length() - 1));
            }
        });
    };

    root(entry);

    // Module-level assembly is always emitted, so every symbol it names is a
    // root even if no function body is reachable. Inline assembly is scanned
    // only when its containing function is live.
    rootAssembly(module.moduleAsm);

    if (!pruneData) {
        for (const ir::Global& global : module.globals) {
            if (liveGlobals.insert(global.label).second) {
                globalWork.push_back(global.label);
            }
        }
        for (const ir::Rodata& blob : module.rodata) {
            liveRodata.insert(blob.label);
        }
    }

    while (!functionWork.empty() || !globalWork.empty()) {
        if (!functionWork.empty()) {
            const std::string name = std::move(functionWork.back());
            functionWork.pop_back();
            const auto found = functions.find(name);
            if (found == functions.end()) {
                continue;
            }
            for (const ir::Function* fn : found->second) {
                for (const ir::Block& block : fn->blocks) {
                    for (const ir::Instr& instr : block.body) {
                        if (instr.op == ir::Op::Call &&
                            !instr.call.indirect) {
                            root(instr.callee);
                        }
                        // One field names a linker-resolved symbol for every
                        // op that has one: FnAddr for code, DataAddr/Load/
                        // Store/Gep for globals and rodata.
                        if (!instr.globalLabel.empty()) {
                            root(instr.globalLabel);
                        }
                        if (instr.op == ir::Op::Asm) {
                            rootAssembly(instr.asmText);
                        }
                    }
                }
            }
            continue;
        }

        const std::string name = std::move(globalWork.back());
        globalWork.pop_back();
        const auto found = globals.find(name);
        if (found == globals.end()) {
            continue;
        }
        for (const ir::Global* global : found->second) {
            // A function-valued global points at a forwarding thunk through
            // `initLabel`; a string or wide global points at rodata there.
            root(global->initLabel);
            for (const std::string& label : global->initLabels) {
                root(label);
            }
            for (const auto& reloc : global->relocWords) {
                root(reloc.second);
            }
        }
    }

    std::vector<ir::Function> keptFunctions;
    keptFunctions.reserve(module.functions.size());
    for (ir::Function& fn : module.functions) {
        if (fn.isExtern || liveFunctions.find(fn.name) != liveFunctions.end()) {
            keptFunctions.push_back(std::move(fn));
            continue;
        }
        ++stats.functionsRemoved;
        for (const ir::Block& block : fn.blocks) {
            stats.instructionsRemoved += block.body.size();
        }
    }
    module.functions = std::move(keptFunctions);

    if (!pruneData) {
        return stats;
    }

    std::vector<ir::Global> keptGlobals;
    keptGlobals.reserve(module.globals.size());
    for (ir::Global& global : module.globals) {
        if (liveGlobals.find(global.label) != liveGlobals.end()) {
            keptGlobals.push_back(std::move(global));
            continue;
        }
        ++stats.globalsRemoved;
    }
    module.globals = std::move(keptGlobals);

    std::vector<ir::Rodata> keptRodata;
    keptRodata.reserve(module.rodata.size());
    for (ir::Rodata& blob : module.rodata) {
        if (liveRodata.find(blob.label) != liveRodata.end()) {
            keptRodata.push_back(std::move(blob));
            continue;
        }
        ++stats.rodataRemoved;
        stats.rodataBytesRemoved += blob.bytes.size();
    }
    module.rodata = std::move(keptRodata);
    return stats;
}

}  // namespace

FunctionPruneResult pruneFunctions(ir::Module& module,
                                   const std::string& entry) {
    const PruneStats stats = prune(module, entry, false);
    FunctionPruneResult result;
    result.functionsRemoved = stats.functionsRemoved;
    result.instructionsRemoved = stats.instructionsRemoved;
    result.skipped = stats.skipped;
    return result;
}

WholeProgramPruneResult pruneWholeProgram(ir::Module& module,
                                          const std::string& entry) {
    const PruneStats stats = prune(module, entry, true);
    WholeProgramPruneResult result;
    result.functionsRemoved = stats.functionsRemoved;
    result.instructionsRemoved = stats.instructionsRemoved;
    result.globalsRemoved = stats.globalsRemoved;
    result.rodataRemoved = stats.rodataRemoved;
    result.rodataBytesRemoved = stats.rodataBytesRemoved;
    result.skipped = stats.skipped;
    return result;
}

}  // namespace cider::opt
