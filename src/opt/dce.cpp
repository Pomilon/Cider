// Whole-program dead-function elimination.
//
// This pass removes only function definitions, never globals or rodata. It is
// intentionally limited to whole-program builds: a split-module object must
// keep every definition because another object may reference it. The driver
// decides when that promise holds.
//
// The entry point is the only required root. Everything else is discovered by
// following the references that survive code generation:
//   - direct calls (`Instr::callee`)
//   - function addresses (`Op::FnAddr` / `Instr::globalLabel`)
//   - global initializer and vtable function labels
//   - relocation words that name functions
//   - symbols named by module-level or reachable inline assembly
//
// Assembly is opaque to the IR, so a function named there is kept even if no
// IR operand mentions it. The match is symbol-boundary aware: `call helper`
// roots `helper`, but `add rax, rbx` does not root a function named `add`.
// Extern declarations are always kept; they have no body and are harmless.

#include "opt/opt.h"

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

bool mentionsSymbol(const std::string& text, const std::string& symbol) {
    if (symbol.empty()) {
        return false;
    }
    std::uint64_t at = 0;
    while ((at = text.find(symbol, at)) != std::string::npos) {
        const bool starts =
            at == 0 || !isSymbolChar(text[at - 1]);
        const std::size_t end = at + symbol.size();
        const bool ends =
            end == text.size() || !isSymbolChar(text[end]);
        if (starts && ends) {
            return true;
        }
        at = end;
    }
    return false;
}

}  // namespace

FunctionPruneResult pruneFunctions(ir::Module& module,
                                   const std::string& entry) {
    FunctionPruneResult result;

    std::unordered_map<std::string, const ir::Function*> byName;
    for (const ir::Function& fn : module.functions) {
        byName.emplace(fn.name, &fn);
    }
    if (byName.find(entry) == byName.end()) {
        // `run` and whole-program `compile` require an entry before this
        // pass; a missing one means the caller is not in the mode this
        // whole-program pass is allowed to prune. Leave the module alone.
        result.skipped = true;
        return result;
    }

    std::unordered_set<std::string> live;
    std::vector<std::string> work;

    auto root = [&](const std::string& name) {
        if (byName.find(name) != byName.end() &&
            live.insert(name).second) {
            work.push_back(name);
        }
    };

    auto rootAssembly = [&](const std::string& text) {
        if (text.empty()) {
            return;
        }
        for (const auto& [name, fn] : byName) {
            if (!fn->isExtern && mentionsSymbol(text, name)) {
                root(name);
            }
        }
    };

    root(entry);

    // Module-level assembly is always emitted, so every symbol it names is a
    // root even if no function body is reachable.
    rootAssembly(module.moduleAsm);

    // Vtable slots live in `initLabels`; other function pointers show up as
    // relocation words in a global's initializer. A function-valued global
    // points at a forwarding thunk through `initLabel`, so that thunk is a
    // root too.
    for (const ir::Global& global : module.globals) {
        root(global.initLabel);
        for (const std::string& label : global.initLabels) {
            root(label);
        }
        for (const auto& reloc : global.relocWords) {
            root(reloc.second);
        }
    }

    while (!work.empty()) {
        const std::string name = std::move(work.back());
        work.pop_back();

        const auto found = byName.find(name);
        if (found == byName.end()) {
            continue;
        }
        const ir::Function& fn = *found->second;
        for (const ir::Block& block : fn.blocks) {
            for (const ir::Instr& instr : block.body) {
                if (instr.op == ir::Op::Call && !instr.call.indirect &&
                    !instr.callee.empty()) {
                    root(instr.callee);
                } else if (instr.op == ir::Op::FnAddr &&
                           !instr.globalLabel.empty()) {
                    root(instr.globalLabel);
                } else if (instr.op == ir::Op::Asm) {
                    rootAssembly(instr.asmText);
                }
            }
        }
    }

    std::vector<ir::Function> kept;
    kept.reserve(module.functions.size());
    for (ir::Function& fn : module.functions) {
        if (fn.isExtern || live.find(fn.name) != live.end()) {
            kept.push_back(std::move(fn));
            continue;
        }
        ++result.functionsRemoved;
        for (const ir::Block& block : fn.blocks) {
            result.instructionsRemoved += block.body.size();
        }
    }
    module.functions = std::move(kept);
    return result;
}

}  // namespace cider::opt
