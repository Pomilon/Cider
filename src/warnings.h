#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "errors.h"
#include "symbols.h"

namespace cider {

// CIDER-FEAT-005: opt-in warning bookkeeping, driven by the type checker
// while `--warn` is on. With warnings disabled the tracker is never fed and
// the default compilation path is byte-identical.
//
// Warnings go through ErrorReporter::warning(), so they never touch
// hasErrors() or the exit code.
//
// Rules (kept deliberately conservative to avoid false positives):
//  * unused local variable: declared in a local scope and never read. A
//    bare assignment target (`x = ...`) is a write, not a read; `x += ...`,
//    `x++`, `x[i] = ...`, `x.f = ...` and `*p = ...` all read the variable.
//    Taking the address (`&x`) suppresses the warning. Parameters, globals,
//    externs and names starting with '_' are never warned.
//  * unused import: a `from m import x` binding that no name resolution
//    (identifier read, type spelling, callee, ...) ever bound to.
//  * shadowing: a local declaration whose name is already bound by an outer
//    local/parameter/global of the same program.
//  * read-before-assignment (CIDER-ISS-002): a local declared without an
//    initializer that is read before any assignment that appears textually
//    earlier in source order. The rule is lexical and deliberately
//    conservative: an assignment anywhere earlier — including a store
//    through an element/member chain (`arr[i] = v`, `s.f = v`), even one
//    guarded by a branch that may not run — suppresses the warning, so this
//    is a "may be read before being assigned" diagnostic with false
//    negatives rather than a definite-assignment proof. Parameters,
//    `for-in` bindings, catch bindings and `_`-prefixed names are exempt
//    like unused-variable analysis; taking the address (`&x`, `&x[i]`) is a
//    possible initialization (the address may escape and be written
//    through), not a read.
class WarningTracker {
public:
    explicit WarningTracker(ErrorReporter& reporter) : reporter_(reporter) {}

    // Record a declaration in `scope`. `warnUnused` covers locals that may
    // be reported unused at scope exit; `checkShadow` enables the shadowing
    // diagnostic (false for parameters, which are tracked so later locals
    // can shadow them but are never warned themselves).
    void declare(const Scope* scope, const std::shared_ptr<Symbol>& sym,
                 int line, int col, bool warnUnused, bool checkShadow) {
        if (!sym || !scope) return;
        if (isSilenced(sym->name)) {
            warnUnused = false;
            checkShadow = false;
        }
        Decl decl;
        decl.name = sym->name;
        decl.line = line > 0 ? line : sym->line;
        decl.col = col > 0 ? col : sym->col;
        decl.warnUnused = warnUnused;
        if (checkShadow) warnShadowed(scope, decl);
        decls_[sym->id] = decl;
        scopeDecls_[scope].push_back(sym->id);
    }

    // Record a `from m import name` binding (the alias shares the target's
    // Symbol object, so identity-keyed reads work across the alias).
    void recordImport(const Scope* scope, const std::shared_ptr<Symbol>& sym,
                      const std::string& name, const std::string& module,
                      int line, int col) {
        if (!sym || !scope || isSilenced(name)) return;
        Decl decl;
        decl.name = name;
        decl.module = module;
        decl.line = line;
        decl.col = col;
        decl.isImport = true;
        decls_[sym->id] = decl;
        scopeDecls_[scope].push_back(sym->id);
        importIds_.push_back(sym->id);
    }

    // A read/reference of a tracked declaration clears its unused status.
    // CIDER-ISS-002: a read of a local that no earlier assignment
    // initialized reports the read-before-assignment warning once per
    // variable. `line`/`col` locate the diagnostic at the read site;
    // `checkUninit=false` keeps bookkeeping-only references (asm operands,
    // re-exports) from warning.
    void markRead(const std::shared_ptr<Symbol>& sym, int line = 0, int col = 0,
                  bool checkUninit = true) {
        auto it = sym ? decls_.find(sym->id) : decls_.end();
        if (it == decls_.end()) return;
        Decl& decl = it->second;
        decl.read = true;
        if (!checkUninit || !decl.warnUninit || decl.assigned || decl.uninitWarned) return;
        decl.uninitWarned = true;
        reporter_.warning("local '" + decl.name + "' may be read before being assigned", line, col);
    }

    // CIDER-ISS-002: a local declared without an initializer starts
    // unassigned and is subject to the read-before-assignment warning until
    // an assignment is seen. `_`-prefixed names stay silent.
    void markUninitialized(const std::shared_ptr<Symbol>& sym) {
        auto it = sym ? decls_.find(sym->id) : decls_.end();
        if (it != decls_.end() && !isSilenced(it->second.name)) it->second.warnUninit = true;
    }

    // CIDER-ISS-002: an assignment (`=`, `+=`, ..., `++`/`--`) initializes
    // the local for the lexical analysis. Writes are not reads, so the
    // unused-variable flag is unaffected.
    void markAssigned(const std::shared_ptr<Symbol>& sym) {
        auto it = sym ? decls_.find(sym->id) : decls_.end();
        if (it != decls_.end()) it->second.assigned = true;
    }

    // A name resolution bound to an imported symbol (identifier, type
    // spelling, callee, ...). Only import bindings are affected: a local
    // resolved speculatively must not be treated as read.
    void markImportUsed(const std::shared_ptr<Symbol>& sym) {
        auto it = sym ? decls_.find(sym->id) : decls_.end();
        if (it != decls_.end() && it->second.isImport) it->second.read = true;
    }

    // `&x` keeps the variable out of the unused report entirely (the
    // address may escape to code the checker cannot see). CIDER-ISS-002:
    // the address may also be written through, so an escape counts as a
    // possible initialization and is never itself read as a value.
    void markAddressTaken(const std::shared_ptr<Symbol>& sym) {
        auto it = sym ? decls_.find(sym->id) : decls_.end();
        if (it != decls_.end()) {
            it->second.addressTaken = true;
            it->second.assigned = true;
        }
    }

    // Report the unused locals of a scope as it closes (the checker calls
    // this before popping the scope from the symbol table).
    void onScopeExit(const Scope* scope) {
        auto it = scopeDecls_.find(scope);
        if (it == scopeDecls_.end()) return;
        for (uint64_t id : it->second) {
            auto decl = decls_.find(id);
            if (decl == decls_.end()) continue;
            if (decl->second.warnUnused && !decl->second.read && !decl->second.addressTaken) {
                reporter_.warning("unused variable '" + decl->second.name +
                                      "' (prefix with '_' to silence)",
                                  decl->second.line, decl->second.col);
            }
            decls_.erase(decl);
        }
        scopeDecls_.erase(it);
    }

    // Conservative bail-out: template bodies/signatures are not fully
    // traversed by the checker, so a binding referenced only from generic
    // code would look unused. Suppress the unused-import report for the
    // whole program when it declares a template (documented trade-off:
    // fewer false positives, a rare missed true positive).
    void suppressUnusedImports() { suppressUnusedImports_ = true; }

    // Report `from m import x` bindings that were never referenced. Called
    // once, after the whole program has been checked.
    void reportUnusedImports() {
        if (suppressUnusedImports_) return;
        for (uint64_t id : importIds_) {
            auto decl = decls_.find(id);
            if (decl == decls_.end() || decl->second.read) continue;
            reporter_.warning("unused import '" + decl->second.name + "' from module '" +
                                  decl->second.module + "'",
                              decl->second.line, decl->second.col);
        }
    }

private:
    struct Decl {
        std::string name;
        std::string module;  // import bindings only
        int line = 0;
        int col = 0;
        bool read = false;
        bool addressTaken = false;
        bool warnUnused = false;
        bool isImport = false;
        // CIDER-ISS-002: read-before-assignment bookkeeping. `warnUninit` is
        // set for locals declared without an initializer; `assigned` records
        // whether an assignment was seen textually earlier in the traversal
        // (lexical source order); `uninitWarned` caps the report at one
        // diagnostic per variable.
        bool warnUninit = false;
        bool assigned = false;
        bool uninitWarned = false;
    };

    // `_`-prefixed names are the documented opt-out (unused/shadowing);
    // empty names cannot be referenced and are never worth a diagnostic.
    static bool isSilenced(const std::string& name) {
        return name.empty() || name[0] == '_';
    }

    // Warn when `decl` re-uses the name of a declaration in an enclosing
    // scope. Imports are not shadow-checked (importing over a name is the
    // checker's business, not a style warning).
    void warnShadowed(const Scope* scope, const Decl& decl) {
        for (const Scope* s = scope->getParent(); s; s = s->getParent()) {
            auto it = scopeDecls_.find(s);
            if (it == scopeDecls_.end()) continue;
            for (uint64_t id : it->second) {
                auto outer = decls_.find(id);
                if (outer == decls_.end() || outer->second.isImport) continue;
                if (outer->second.name != decl.name) continue;
                std::string msg = "declaration of '" + decl.name +
                                  "' shadows an outer declaration";
                if (outer->second.line > 0) {
                    msg += " at line " + std::to_string(outer->second.line);
                }
                reporter_.warning(msg, decl.line, decl.col);
                return;  // nearest shadowed declaration only
            }
        }
    }

    ErrorReporter& reporter_;
    std::map<uint64_t, Decl> decls_;  // symbol id -> declaration state
    std::map<const Scope*, std::vector<uint64_t>> scopeDecls_;
    std::vector<uint64_t> importIds_;  // import order, for stable reporting
    bool suppressUnusedImports_ = false;
};

}  // namespace cider
