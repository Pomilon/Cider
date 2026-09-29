#pragma once

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "ast.h"
#include "errors.h"
#include "module_scope.h"
#include "symbols.h"
#include "warnings.h"

namespace cider {

class TypeChecker : public ASTVisitor {
public:
    TypeChecker(SymbolTable& symbolTable, ErrorReporter& reporter, bool moduleMode = false)
        : symbolTable_(symbolTable), reporter_(reporter), warnings_(reporter), moduleMode_(moduleMode) {}

    void check(AstProgram& program) {
        program.accept(*this);
        // CIDER-BUG-068: resolve the fn/closure representation inference
        // once the whole program (imports included) has been walked; the
        // annotations it writes are read by ir_gen.
        resolveFnRepresentations();
    }

    // CIDER-BUG-184: true when some interface-typed return in the program
    // boxes a concrete struct/class value. ir_gen reads this via
    // setInterfaceBoxing so the boxing program uses one consistent
    // representation (tagged boxes + caller-side release); programs that
    // never box keep their byte-identical IR.
    bool hasInterfaceBoxing() const { return interfaceBoxing_; }

    // CID-020/021 module context, wired from the collector (mirrors
    // IRGenerator::setModuleScopes). Until set, the checker behaves exactly
    // as before (all lookups are plain resolves in the root scope).
    void setModuleScopes(const std::map<std::string, Scope*>& scopes) { moduleScopes_ = scopes; }
    void setProgramModules(const std::map<const AstProgram*, std::string>& programModules) {
        programModules_ = programModules;
    }
    void setAmbiguousModules(
        const std::map<std::string, std::vector<std::shared_ptr<ModuleSymbol>>>& ambiguous) {
        ambiguousModules_ = ambiguous;
    }
    // Bind-time defining-module provenance from the collector (mirrors
    // IRGenerator::setSymbolModules): lets nominal identity resolve by the
    // type's owner, not the file being checked. Unset (unit tests) falls
    // back to the legacy current-module rule.
    void setSymbolModules(const std::map<uint64_t, std::string>& symbolModules) {
        symbolModules_ = symbolModules;
    }
    // CIDER-BUG-073: qualified template spellings the collector accepted,
    // mapped to the shared bare instantiation spelling. Unset (unit tests)
    // keeps the legacy behavior: names containing '<' are never
    // canonicalized (T6).
    void setQualifiedTemplateInstantiations(const std::map<std::string, std::string>& map) {
        qualifiedTemplateInstantiations_ = map;
    }
    // CIDER-BUG-136: per-owner struct-template instantiation identities
    // recorded for separate compilation, keyed by
    // (spelling context module, spelling) — see templateSpellingKey. Lets a
    // bare `Box<int>` resolve to the identity of ITS module's template
    // instead of the first same-named instantiation registered. Empty in
    // whole-program mode (nothing changes there).
    void setTemplateInstanceIdentities(const std::map<std::string, std::string>& map) {
        templateInstanceIdentities_ = map;
    }
    // CIDER-BUG-119: function/block-local aggregate declarations the
    // collector registered, keyed by their AST node (see the getter in
    // SymbolCollector). Visiting the declaration installs the finalized
    // StructSymbol into the live scope; without this, the symbol only ever
    // lived in the collector's transient local scope and every use fell
    // through to "Type X is not a struct or class".
    void setLocalAggregates(
        const std::map<const Statement*, std::shared_ptr<StructSymbol>>& map) {
        localAggregates_ = map;
    }
    // CIDER-BUG-096: base names of the templates each module declares
    // ("" == main file). A qualified template spelling the collector did
    // not accept is reported as an unknown template only when its module
    // declares no such template; export/arity rejections already carry the
    // collector's diagnostic. Unset (unit tests) keeps the legacy silence.
    void setModuleTemplateNames(const std::map<std::string, std::set<std::string>>& names) {
        moduleTemplateNames_ = names;
        moduleTemplateNamesSet_ = true;
    }

    // CID-005: target ABI defaults. `--target uefi` sets defaultAbiMs, which
    // types unmarked functions as MS-ABI in fn-value position; plain `fn`
    // type spellings stay SysV (spec §3). uefiEntryAllowed permits
    // `efi_main` in place of main/_start.
    void setDefaultAbiMs(bool v) { defaultAbiMs_ = v; }
    void setUefiEntryAllowed(bool v) { uefiEntryAllowed_ = v; }

    // CIDER-FEAT-005: opt-in warnings (--warn). Disabled by default so the
    // default pipeline stays byte-identical; when enabled the checker feeds
    // the WarningTracker (unused variables/imports, shadowing,
    // read-before-assignment) whose diagnostics go to reporter_.warning()
    // and never affect hasErrors() or exit codes. Only the root program is
    // analyzed: imported module bodies are error-checked, never warned
    // about.
    void setWarningsEnabled(bool v) { warningsEnabled_ = v; }

    // CIDER-FEAT-005: every successful name resolution counts as a use for
    // the unused-import bookkeeping — identifiers, type spellings, callees
    // and every other path that binds a name flow through here. Speculative
    // lookups can only mark a binding used, so this stays conservative.
    std::shared_ptr<Symbol> resolveScoped(const std::string& name) {
        auto sym = resolveScopedRaw(name);
        if (warningsActive_ && sym) warnings_.markImportUsed(sym);
        return sym;
    }

    // CIDER-BUG-136: identity a template spelling resolves to when written
    // in `context` ("" == main file): the collector's per-owner map first
    // (separate compilation), then the legacy accepted-qualified-spelling
    // map. Empty when the spelling has no recorded instantiation.
    std::string templateInstanceIdentity(const std::string& spelling, const std::string& context) {
        auto it = templateInstanceIdentities_.find(templateSpellingKey(context, spelling));
        if (it != templateInstanceIdentities_.end()) return it->second;
        auto qti = qualifiedTemplateInstantiations_.find(spelling);
        if (qti != qualifiedTemplateInstantiations_.end()) return qti->second;
        return std::string();
    }

    // CID-020/021 §2: strict current-module-then-root lookup. The collector
    // parents module scopes under the importer (nested enterScope), so a
    // naive resolve() would leak importer symbols here; instead walk from
    // the current scope up to (and including) the module scope, then fall
    // back to the root scope only. With no module current this is exactly
    // symbolTable_.resolve().
    std::shared_ptr<Symbol> resolveScopedRaw(const std::string& name) {
        if (name.find("::") != std::string::npos) return resolveCanonical(name);
        // CIDER-BUG-136: a bare template instantiation re-roots through its
        // per-owner identity (separate compilation) before the scope walk.
        if (name.find('<') != std::string::npos) {
            std::string ident = templateInstanceIdentity(name, currentModule_);
            if (!ident.empty() && ident != name) return resolveScopedRaw(ident);
        }
        if (!currentModuleScope_) return symbolTable_.resolve(name);
        bool underModule = false;
        for (Scope* s = symbolTable_.getCurrentScope(); s; s = s->getParent()) {
            if (s == currentModuleScope_) {
                underModule = true;
                break;
            }
        }
        if (underModule) {
            for (Scope* s = symbolTable_.getCurrentScope(); s; s = s->getParent()) {
                if (auto sym = s->resolveLocal(name)) return sym;
                if (s == currentModuleScope_) break;
            }
        }
        Scope* root = symbolTable_.getRootScope();
        return root ? root->resolveLocal(name) : nullptr;
    }

    // CIDER-BUG-119: true when `sym` is bound to `name` by a scope BELOW
    // the current module/root scope — i.e. a function- or block-local
    // declaration. Such a binding has no module-qualified canonical
    // spelling: its identity is lexical, so callers must keep the written
    // name and resolve it through the live scope stack.
    bool isScopeLocalBinding(const std::string& name, const std::shared_ptr<Symbol>& sym) {
        for (Scope* s = symbolTable_.getCurrentScope(); s; s = s->getParent()) {
            if (s == currentModuleScope_ || s == symbolTable_.getRootScope()) return false;
            if (s->resolveLocal(name) == sym) return true;
        }
        return false;
    }

    // CIDER-BUG-102: resolve a name as a spelling WRITTEN in `logical`'s
    // module sees it — that module's scope, then root, never the use site's
    // scope. Dotted spellings and alias targets pass their defining module
    // here, so `lib.Shape` written in module mid resolves against mid's
    // imports even when the ultimate use site does not import lib. A no-op
    // for the current module (and for unregistered modules), which keeps
    // ordinary lookups unchanged.
    std::shared_ptr<Symbol> resolveScopedInModule(const std::string& name, const std::string& logical) {
        if (logical == currentModule_) return resolveScoped(name);
        Scope* ms = scopeForModule(logical);
        if (!ms) return resolveScoped(name);
        Scope* prevScope = symbolTable_.getCurrentScope();
        Scope* prevModuleScope = currentModuleScope_;
        std::string prevModule = currentModule_;
        currentModule_ = logical;
        currentModuleScope_ = ms;
        symbolTable_.setCurrentScope(ms);
        auto sym = resolveScoped(name);
        symbolTable_.setCurrentScope(prevScope);
        currentModule_ = prevModule;
        currentModuleScope_ = prevModuleScope;
        return sym;
    }

    // A canonical "logical::Name" (spec §1 identity) resolves its bare part
    // in that module's scope.
    std::shared_ptr<Symbol> resolveCanonical(const std::string& name) {
        auto pos = name.find("::");
        std::string logical = name.substr(0, pos);
        std::string bare = name.substr(pos + 2);
        for (const auto& [prog, mod] : programModules_) {
            if (mod != logical || !prog) continue;
            auto scIt = moduleScopes_.find(prog->getSourcePath());
            if (scIt != moduleScopes_.end() && scIt->second) {
                if (auto sym = scIt->second->resolveLocal(bare)) return sym;
            }
        }
        return nullptr;
    }

    // CID-020/021 §1: struct/class/enum/interface identity is the
    // DEFINING module's full logical path ("nsa::Event"); main-file types
    // keep legacy bare names. Resolution is by symbol provenance
    // (bind-time, alias-safe), not by the file being checked — so a value
    // of type b.Event differs from a.Event in every scope. Primitives,
    // template instantiations and unresolvable names are unaffected.
    // Dotted names (`nsa.Event`, `a.util.Foo`) resolve their qualifier
    // through the module registry (spec §3). `line` attributes qualifier
    // errors; line < 0 leaves failing dotted names untouched so the outer
    // comparison fails safely instead of accepting.
    std::string definingModuleOf(const std::shared_ptr<Symbol>& sym) {
        if (sym) {
            auto it = symbolModules_.find(sym->id);
            if (it != symbolModules_.end()) return it->second;
        }
        return currentModule_;
    }

    // CIDER-BUG-090: module a method's signature spellings were written in
    // ("" == main). An inherited method keeps its declaring class's module,
    // which may be the only module where a parameter/return type is visible.
    // `fallback` covers methods built without the collector (unit tests).
    static std::string methodModuleOf(const StructSymbol::Method& m,
                                      const std::string& fallback) {
        return m.declaringModule.empty() ? fallback : m.declaringModule;
    }

    // CID-006: the fn(...) type of a named function, with every nominal
    // canonicalized in the function's defining module (same rule as call
    // signatures, spec §5). `line` attributes dotted-name errors the same
    // way the call-site signature canonicalization does.
    TypeInfo functionPointerTypeOf(const std::shared_ptr<FunctionSymbol>& fsym, int line = -1) {
        TypeInfo ft;
        ft.name = "fn";
        ft.isFunctionPointer = true;
        ft.isMsAbi = (fsym->abi == Abi::Ms) || (fsym->abi == Abi::Default && defaultAbiMs_);
        ft.fnParamTypes = std::make_shared<std::vector<TypeInfo>>();
        std::string defMod = definingModuleOf(fsym);
        for (const auto& p : fsym->parameterTypes) {
            TypeInfo paramT = canonicalizeInModule(p, defMod, line);
            // CIDER-BUG-085: the named function's signature is the reference
            // spelling AST-derived fn types must match; resolve its nested
            // interface flags here so both sides carry them.
            resolveInterfaceFlag(paramT);
            ft.fnParamTypes->push_back(paramT);
        }
        ft.fnReturnType = std::make_shared<TypeInfo>(canonicalizeInModule(fsym->returnType, defMod, line));
        resolveInterfaceFlag(*ft.fnReturnType);
        return ft;
    }

    // Module scope for a logical name ("" == root scope). Null when unknown.
    Scope* scopeForModule(const std::string& logical) {
        if (logical.empty()) return symbolTable_.getRootScope();
        for (const auto& [prog, mod] : programModules_) {
            if (!prog || mod != logical) continue;
            auto it = moduleScopes_.find(prog->getSourcePath());
            if (it != moduleScopes_.end()) return it->second;
        }
        return nullptr;
    }

    bool isAmbiguousQualifier(const std::string& qualifier) {
        auto it = ambiguousModules_.find(qualifier);
        return it != ambiguousModules_.end() && it->second.size() >= 2;
    }

    // CIDER-BUG-060: cross-module visibility. A module member is reachable
    // only when exported; extern declarations are linker-provided and stay
    // accessible without an export. References from inside the owning module
    // (a self-qualified `m.x` inside m, or canonicalization that re-enters
    // m's own scope) are unrestricted.
    bool isVisibleMember(const std::shared_ptr<Symbol>& mem, const std::string& ownerModule) {
        if (!mem) return true;
        // A nested-module qualifier is registration syntax, not a member
        // declaration: its own kind error (if any) is reported elsewhere.
        if (mem->kind == SymbolKind::MODULE) return true;
        if (mem->isExported || mem->isExtern) return true;
        return !ownerModule.empty() && ownerModule == currentModule_;
    }

    // Canonicalize a dotted `qualifier.Name` (qualifier = last segment or
    // full logical path) to `logical::Name`. The qualifier resolves as a
    // spelling written in `spellingModule` sees it (CIDER-BUG-102), so a
    // cross-module signature/alias target written in its own module does
    // not depend on the ultimate use site's imports.
    TypeInfo canonicalizeDotted(TypeInfo t, int line, const std::string& spellingModule) {
        auto dot = t.name.rfind('.');
        std::string prefix = t.name.substr(0, dot);
        std::string bare = t.name.substr(dot + 1);
        auto modSym = resolveScopedInModule(prefix, spellingModule);
        bool isModule = modSym && modSym->kind == SymbolKind::MODULE;
        std::shared_ptr<Symbol> mem;
        std::string logical;
        if (isModule && !isAmbiguousQualifier(prefix)) {
            auto ms = std::static_pointer_cast<ModuleSymbol>(modSym);
            logical = ms->logicalName;
            if (ms->scope) mem = ms->scope->resolveLocal(bare);
        }
        // CIDER-BUG-060: a qualified type is usable only when the member is
        // exported (externs exempt). line < 0 marks silent pre-check calls
        // that must keep failing safely instead of emitting a diagnostic.
        // CIDER-BUG-176: the spelling belongs to `spellingModule`
        // (CIDER-BUG-102 re-rooting), so a self-qualified target written in
        // its own module (`export type S = lib.Internal;` inside lib) stays
        // visible when an alias chain re-resolves it from a foreign use
        // site; a foreign spelling of the same private target still errors.
        bool selfSpelling = !logical.empty() && spellingModule == logical;
        if (mem && !selfSpelling && !isVisibleMember(mem, logical)) {
            if (line < 0) return t;
            error("symbol '" + bare + "' is not exported from module '" + prefix + "'", line);
            return t;
        }
        bool ok = isModule && mem &&
                  (mem->kind == SymbolKind::STRUCT || mem->kind == SymbolKind::CLASS ||
                   mem->kind == SymbolKind::ENUM || mem->kind == SymbolKind::INTERFACE);
        if (!ok) {
            if (line < 0) return t;
            if (!reportAmbiguousQualifier(prefix, line)) {
                if (isModule) {
                    error("module '" + prefix + "' has no symbol '" + bare + "'", line);
                } else if (modSym) {
                    error("'" + prefix + "' is not a module", line);
                } else {
                    error("Undefined identifier '" + prefix + "'", line);
                }
            }
            return t;
        }
        t.name = canonicalTypeName(bare, logical);
        return t;
    }

    // Canonicalize a nominal in a NON-current module's context: strict
    // module-scope-local lookup, then root (spec §2 order), never the
    // caller's scope — so a callee signature means the same at every use
    // site. Falls back to the untouched name when unresolvable.
    TypeInfo canonicalizeInModule(TypeInfo t, const std::string& logical, int line = -1) {
        // CID-006: signature nominals belong to the same context as the
        // fn type itself.
        if (t.isFunctionPointer) {
            TypeInfo r = t;
            if (t.fnParamTypes) {
                r.fnParamTypes = std::make_shared<std::vector<TypeInfo>>();
                for (const auto& p : *t.fnParamTypes) {
                    r.fnParamTypes->push_back(canonicalizeInModule(p, logical, line));
                }
            }
            if (t.fnReturnType) {
                r.fnReturnType = std::make_shared<TypeInfo>(
                    canonicalizeInModule(*t.fnReturnType, logical, line));
            }
            return r;
        }
        // CIDER-BUG-052: canonicalize a nested array's inner dimensions too;
        // the flattened name below only covers the innermost element.
        if (t.arrayElement) {
            t.arrayElement = std::make_shared<TypeInfo>(
                canonicalizeInModule(*t.arrayElement, logical, line));
        }
        if (t.name.empty() || t.name.find("::") != std::string::npos) return t;
        // CIDER-BUG-136: a template instantiation spelling resolves to the
        // per-owner identity recorded for THIS spelling context (separate
        // compilation), so a bare `Box<int>` written in two modules keeps
        // each module's own template. Whole-program mode records nothing
        // and falls through to the legacy shared lookup.
        if (t.name.find('<') != std::string::npos) {
            std::string ident = templateInstanceIdentity(t.name, logical);
            if (!ident.empty()) {
                t.name = ident;
                return t;
            }
        }
        // CIDER-BUG-073: a qualified template instantiation (`m.Box<int>`)
        // resolves to the shared bare instantiation the collector
        // registered, exactly like the from-import spelling. Bare template
        // instantiations stay untouched (T6).
        auto qti = qualifiedTemplateInstantiations_.find(t.name);
        if (qti != qualifiedTemplateInstantiations_.end()) {
            t.name = qti->second;
            return t;
        }
        if (t.name.find('<') != std::string::npos) return t;  // template instantiation (T6)
        if (t.name.find('.') != std::string::npos) return canonicalizeDotted(t, line, logical);
        Scope* ms = scopeForModule(logical);
        std::shared_ptr<Symbol> sym;
        if (ms) sym = ms->resolveLocal(t.name);
        if (!sym && ms && ms != symbolTable_.getRootScope()) {
            Scope* root = symbolTable_.getRootScope();
            if (root) sym = root->resolveLocal(t.name);
        }
        if (sym && (sym->kind == SymbolKind::STRUCT || sym->kind == SymbolKind::CLASS ||
                    sym->kind == SymbolKind::ENUM || sym->kind == SymbolKind::INTERFACE)) {
            t.name = canonicalTypeName(t.name, definingModuleOf(sym));
        }
        return t;
    }

    TypeInfo canonicalizeNominal(TypeInfo t, int line = -1) {
        // CID-006: signature nominals belong to the same context as the
        // fn type itself.
        if (t.isFunctionPointer) {
            TypeInfo r = t;
            if (t.fnParamTypes) {
                r.fnParamTypes = std::make_shared<std::vector<TypeInfo>>();
                for (const auto& p : *t.fnParamTypes) {
                    r.fnParamTypes->push_back(canonicalizeNominal(p, line));
                }
            }
            if (t.fnReturnType) {
                r.fnReturnType = std::make_shared<TypeInfo>(
                    canonicalizeNominal(*t.fnReturnType, line));
            }
            return r;
        }
        // CIDER-BUG-052: canonicalize a nested array's inner dimensions too;
        // the flattened name below only covers the innermost element.
        if (t.arrayElement) {
            t.arrayElement = std::make_shared<TypeInfo>(canonicalizeNominal(*t.arrayElement, line));
        }
        if (t.name.empty() || t.name.find("::") != std::string::npos) return t;  // already canonical
        // CIDER-BUG-136: per-owner instantiation identity for this spelling
        // context (separate compilation); see canonicalizeInModule.
        if (t.name.find('<') != std::string::npos) {
            std::string ident = templateInstanceIdentity(t.name, currentModule_);
            if (!ident.empty()) {
                t.name = ident;
                return t;
            }
        }
        // CIDER-BUG-073: see canonicalizeInModule — accepted qualified
        // template spellings resolve to the shared bare instantiation.
        auto qti = qualifiedTemplateInstantiations_.find(t.name);
        if (qti != qualifiedTemplateInstantiations_.end()) {
            t.name = qti->second;
            return t;
        }
        // CIDER-BUG-096: a qualified template spelling the collector did not
        // accept (ambiguous qualifier / unknown template) is diagnosed here
        // instead of bailing out to the generic member-access error.
        if (t.name.find('<') != std::string::npos) return canonicalizeDottedTemplate(t, line, currentModule_);
        if (t.name.find('.') != std::string::npos) return canonicalizeDotted(t, line, currentModule_);
        auto sym = resolveScoped(t.name);
        if (sym && (sym->kind == SymbolKind::STRUCT || sym->kind == SymbolKind::CLASS ||
                    sym->kind == SymbolKind::ENUM || sym->kind == SymbolKind::INTERFACE)) {
            // CIDER-BUG-119: a function-local binding keeps its written
            // spelling; qualifying it with the module would make later
            // lookups miss the lexical scope (and local types of two
            // functions in one module must stay distinct).
            if (isScopeLocalBinding(t.name, sym)) return t;
            t.name = canonicalTypeName(t.name, definingModuleOf(sym));
        }
        return t;
    }

    // CIDER-BUG-128: canonical identity of a TYPE SPELLING written in
    // `logical`'s module, for the signature comparisons that decide
    // interface satisfaction (checkInterfaceValue). Collected signatures
    // keep the spellings as written — bare, dotted or through a `type`
    // alias, each in its own declaring module — so comparing them raw
    // conflates two same-named types declared in different modules (a
    // look-alike class silently "implements" the interface) and splits one
    // type spelled two ways (a genuine cross-module implementor is
    // rejected). The walk mirrors resolveType + canonicalizeInModule: a
    // `type` alias hop resolves in the ALIAS's defining module
    // (CIDER-BUG-102) and the nominal that ends the chain canonicalizes
    // where it was written (CID-020/021), so `Param`, `a.Param` and an
    // alias of either written in module a are one identity — while a bare
    // `Param` written in module b is another. Nested fn/array spellings
    // recurse in the same module. An unresolvable spelling keeps its
    // written name and line < 0 marks the silent pre-check the convention
    // already uses: the outer comparison then fails safely instead of
    // accepting (or of reporting a second, stray diagnostic).
    TypeInfo canonicalSignatureType(const TypeInfo& t, const std::string& logical,
                                    int line = -1) {
        if (t.isFunctionPointer) {
            TypeInfo r = t;
            if (t.fnParamTypes) {
                r.fnParamTypes = std::make_shared<std::vector<TypeInfo>>();
                for (const auto& p : *t.fnParamTypes) {
                    r.fnParamTypes->push_back(canonicalSignatureType(p, logical, line));
                }
            }
            if (t.fnReturnType) {
                r.fnReturnType = std::make_shared<TypeInfo>(
                    canonicalSignatureType(*t.fnReturnType, logical, line));
            }
            return r;
        }
        if (t.arrayElement) {
            // CIDER-BUG-052: name/pointerLevel mirror the innermost element,
            // so the canonical element spelling carries both with it (the
            // element walk below already resolved its alias chain).
            TypeInfo elem = canonicalSignatureType(*t.arrayElement, logical, line);
            TypeInfo r = t;
            r.arrayElement = std::make_shared<TypeInfo>(elem);
            r.name = elem.name;
            r.pointerLevel = elem.pointerLevel;
            return r;
        }
        if (t.name.empty()) return t;
        // The written spelling canonicalizes first (dotted qualifiers,
        // templates, bare nominals), then each `type` hop in its own
        // module; `visiting` bounds a cyclic chain exactly like
        // resolveTypeImpl.
        TypeInfo cur = canonicalizeInModule(t, logical, line);
        std::set<std::string> visiting;
        std::string mod = logical;
        while (!cur.name.empty() && visiting.insert(cur.name).second) {
            auto sym = resolveScopedInModule(cur.name, mod);
            if (!sym || !sym->isTypeAlias || sym->type.name.empty() ||
                sym->type.name == cur.name) {
                break;
            }
            std::string hopMod = definingModuleOf(sym);
            TypeInfo target = canonicalizeInModule(sym->type, hopMod, line);
            // The written wrapper/markings belong to the spelling, not to
            // how the alias spells its target (resolveTypeImpl rewraps the
            // array the same way; the flags keep the representation the
            // collector recorded — CIDER-BUG-078/080).
            if (cur.isArray) {
                if (!target.isArray) {
                    target.isArray = true;
                    target.arraySize = cur.arraySize;
                    target.arrayElement = cur.arrayElement;
                } else {
                    TypeInfo outer = target;
                    outer.arraySize = cur.arraySize;
                    outer.arrayElement = std::make_shared<TypeInfo>(target);
                    target = outer;
                }
            }
            target.pointerLevel += cur.pointerLevel;
            target.isInterface = target.isInterface || cur.isInterface;
            target.isVolatile = target.isVolatile || cur.isVolatile;
            target.isClosure = target.isClosure || cur.isClosure;
            target.isSlice = target.isSlice || cur.isSlice;
            cur = target;
            mod = hopMod;
        }
        return cur;
    }

    // Spec §1: two imports sharing a last segment make the bare qualifier an
    // ambiguity error listing both candidates, never a silent pick. Returns
    // true when an error was emitted.
    bool reportAmbiguousQualifier(const std::string& qualifier, int line) {
        auto it = ambiguousModules_.find(qualifier);
        if (it == ambiguousModules_.end() || it->second.size() < 2) return false;
        std::string msg = "Ambiguous module qualifier '" + qualifier + "' (candidates:";
        for (size_t i = 0; i < it->second.size(); ++i) {
            if (i) msg += ",";
            msg += " '" + it->second[i]->logicalName + "'";
        }
        msg += "; use a from-import)";
        error(msg, line);
        return true;
    }

    // CIDER-BUG-096: a qualified template spelling (`m.Box<int>`) that the
    // collector did not accept. Accepted spellings were rewritten through
    // qualifiedTemplateInstantiations_ before this runs, and export/arity
    // rejections were already reported by the collector; the remaining
    // failures are an ambiguous qualifier or a module that declares no such
    // template. Both must surface here: the collector instantiates lazily
    // from annotations, so an unused declaration never reached the checker's
    // member lookups and used to stay silent (or fell through to the generic
    // "is not a struct or class"). A dot after the '<' belongs to an
    // argument list (`Box<m.Thing>`) and is not a qualifier. line < 0 marks
    // silent pre-check calls that must keep failing safely.
    TypeInfo canonicalizeDottedTemplate(TypeInfo t, int line, const std::string& spellingModule) {
        auto lt = t.name.find('<');
        if (lt == std::string::npos) return t;
        auto dot = t.name.rfind('.', lt);
        if (dot == std::string::npos) {  // bare instantiation
            // CIDER-BUG-134: unlike qualified spellings, a bare template
            // name has no qualifier to check against; report it when no
            // known template carries its base name.
            checkBareTemplate(t, line);
            return t;
        }
        if (line < 0) return t;
        std::string prefix = t.name.substr(0, dot);
        std::string bare = t.name.substr(dot + 1, lt - dot - 1);
        if (reportAmbiguousQualifier(prefix, line)) return t;
        auto modSym = resolveScopedInModule(prefix, spellingModule);
        if (modSym && modSym->kind == SymbolKind::MODULE) {
            auto ms = std::static_pointer_cast<ModuleSymbol>(modSym);
            auto namesIt = moduleTemplateNames_.find(ms->logicalName);
            if (namesIt != moduleTemplateNames_.end() && namesIt->second.count(bare)) {
                return t;  // declared: collector already accepted or reported it
            }
            error("unknown template '" + bare + "' in module '" + prefix + "'", line);
            return t;
        }
        // Not a module qualifier: the same diagnostics a plain dotted
        // spelling gets (the member's argument list does not obscure it).
        if (modSym) error("'" + prefix + "' is not a module", line);
        else error("Undefined identifier '" + prefix + "'", line);
        return t;
    }

    // CIDER-BUG-134: declaration-site diagnostic for a BARE template
    // spelling (`type X = Nope<int>;`) that names no known template. The
    // collector instantiates lazily from annotations, so an UNUSED bare
    // spelling used to stay silent and only a later use failed with the
    // generic "is not a struct or class". A bare spelling the collector
    // accepted always has its base name in moduleTemplateNames_ (and, in
    // separate compilation, a per-owner identity); export/arity rejections
    // already carry the collector's own diagnostic, so nothing more is
    // reported for those. Only a name no module declares is unknown. line
    // < 0 marks silent pre-check calls; without a collector-supplied
    // template table (unit tests) the legacy silence is kept.
    void checkBareTemplate(const TypeInfo& t, int line) {
        if (line < 0 || !moduleTemplateNamesSet_) return;
        std::string base = t.name.substr(0, t.name.find('<'));
        for (const auto& [module, names] : moduleTemplateNames_) {
            (void)module;
            if (names.count(base)) return;
        }
        error("unknown template '" + base + "'", line);
    }

    TypeInfo resolveType(TypeInfo info) {
        std::set<std::string> visiting;
        return resolveTypeImpl(info, visiting);
    }

    // CIDER-BUG-015: `type X = T` providers are STRUCT-kind plain Symbols
    // (`isTypeAlias`), not StructSymbols, so a kind test alone admits them
    // and the StructSymbol cast is undefined behavior. Follow the alias
    // chain (cycle-safe, via resolveType) and return the symbol only when
    // it is a genuine struct/class definition; nullptr for alias
    // providers, primitives, cyclic stops and unresolvable spellings.
    // A pointer level does not change the nominal identity here (dot
    // access auto-derefs; arrow access decremented before the call), so
    // strip it before resolving — this mirrors the legacy lookup, which
    // was by name only.
    std::shared_ptr<Symbol> resolveStructOrClass(TypeInfo t) {
        t.pointerLevel = 0;
        TypeInfo resolved = resolveType(t);
        auto sym = resolveScoped(resolved.name);
        if (!sym || (sym->kind != SymbolKind::STRUCT && sym->kind != SymbolKind::CLASS)) return nullptr;
        if (!sym->type.name.empty()) return nullptr;  // alias provider / self-alias stop
        return sym;
    }

    // CIDER-BUG-124: resolve a class's `extends` spelling to the class it
    // names. Dotted spellings canonicalize through the same defining-module
    // rules as every other type position (CID-020/021, CIDER-BUG-102; a
    // `lib.Base` written in a module resolves its qualifier in that module),
    // and `type` alias chains resolve to the class they end at. `line` >= 0
    // attributes dotted-name/visibility diagnostics; line < 0 stays silent
    // for speculative walks. Null when the spelling names no class.
    std::shared_ptr<Symbol> resolveParentClass(const std::string& name, int line = -1) {
        if (name.empty()) return nullptr;
        TypeInfo t;
        t.name = name;
        t = canonicalizeNominal(t, line);
        return resolveStructOrClass(t);
    }

    // CID-006 fix round 2: cycle-safe recursion (mirrors ir_gen's alias
    // resolver). Aliases are checker-accepted even when cyclic; stopping at
    // the repeated spelling keeps areTypesCompatible and every other caller
    // terminating instead of overflowing the stack.
    TypeInfo resolveTypeImpl(TypeInfo info, std::set<std::string>& visiting) {
        if (info.pointerLevel > 0 || info.isFunctionPointer) return info;
        if (info.name.empty() || !visiting.insert(info.name).second) return info;
        auto sym = resolveScoped(info.name);
        if (sym && sym->kind == SymbolKind::STRUCT && sym->type.name != info.name && !sym->type.name.empty()) {
            // CIDER-BUG-029: the target spelling belongs to the alias's
            // defining module; canonicalize it there so a cross-module
            // `type X = othermod.S` resolves (and compares) at use sites
            // that do not import S.
            TypeInfo target = canonicalizeInModule(sym->type, definingModuleOf(sym), -1);
            TypeInfo resolved = resolveTypeImpl(target, visiting);
            resolved.pointerLevel += info.pointerLevel;  // Should be 0 here
            // CIDER-BUG-019: an alias spelling can be an ELEMENT type inside
            // an array (`[Cb; N]`); keep the outer array wrapper, or the
            // shape is lost and indexing is rejected / sizes compare equal.
            // CIDER-BUG-052: when the alias target is itself an array, the
            // spelling adds an outer dimension around it — the flat
            // representation used to drop that wrapper entirely.
            if (info.isArray) {
                if (!resolved.isArray) {
                    resolved.isArray = true;
                    resolved.arraySize = info.arraySize;
                    resolved.arrayElement = info.arrayElement;
                } else {
                    TypeInfo outer = resolved;
                    outer.isArray = true;
                    outer.arraySize = info.arraySize;
                    outer.arrayElement = std::make_shared<TypeInfo>(resolved);
                    resolved = outer;
                }
            }
            return resolved;
        }
        return info;
    }

    // CID-006 fix (F1): `type Cb = fn(int) -> int;` stores the ALIAS NAME in
    // every symbol typed with it, so every fn-specific rule must resolve the
    // alias first or it silently misses alias-typed values (indirect calls,
    // global initializers). Non-fn aliases keep their declared spelling so
    // diagnostics are unchanged; arrays are left untouched because
    // resolveType would drop the isArray/arraySize wrapper. Task 3's ir_gen
    // gate must apply the same resolution to `sym->type` before testing
    // isFunctionPointer (ir_gen::mapType already walks the alias chain).
    TypeInfo resolveFunctionPointerAlias(TypeInfo t) {
        // CIDER-BUG-085: this helper feeds both sides of every explicit
        // signatureEquals comparison. Resolve interface flags through the
        // signature here as well, so a symbol-stored `fn(I) -> int` and an
        // AST-derived spelling of the same type compare with identical
        // flags even when the type reached the comparison without passing a
        // collection/declaration boundary.
        if (t.isFunctionPointer || t.isArray) {
            resolveInterfaceFlag(t);
            return t;
        }
        TypeInfo resolved = resolveType(t);
        if (resolved.isFunctionPointer) {
            resolveInterfaceFlag(resolved);
            return resolved;
        }
        return t;
    }

    // ------------------------------------------------------------------
    // CIDER-BUG-068: closure/plain representation inference for `fn(...)`
    // storage.
    //
    // A closure literal produces a heap object {fnptr, captures...} whose
    // call passes the object as a hidden first argument; a named function
    // is a raw code pointer. A single `fn(...)` variable or parameter can
    // hold only one of the two shapes, so the checker infers each
    // parameter's shape from every argument it receives (and every value
    // copied into a fn-typed local that is later passed on). The inference
    // is a union-find over storage slots with two polarities:
    //   * closure values and plain fn values constrain a slot directly,
    //   * an identifier naming a fn-typed parameter/local unions its slot
    //     with the target, so forwarding chains resolve transitively.
    // Conflicting polarities are reported at the second use instead of
    // silently miscompiling. After the walk, resolved closure parameters
    // are marked on their TypeAST; TypeInfo::fromAST (and therefore
    // ir_gen) then emits the closure call path for them.
    // CIDER-BUG-118: Mixed is a *void provenance slot that received both
    // shapes over the program. It is reported only where the closure shape is
    // required; a fn(...) storage never resolves to Mixed (a real storage
    // conflict is an error).
    enum class FnRepr : uint8_t { Unknown = 0, Plain = 1, Closure = 2, Mixed = 3 };

    static std::string fnParamSlotKey(const std::shared_ptr<Symbol>& fnSym, size_t index) {
        return "p:" + std::to_string(fnSym->id) + ":" + std::to_string(index);
    }

    int fnSlot(const std::string& key, const std::string& label, int line) {
        auto it = fnSlotIndex_.find(key);
        if (it != fnSlotIndex_.end()) return it->second;
        int idx = (int)fnSlotParent_.size();
        fnSlotIndex_.emplace(key, idx);
        fnSlotParent_.push_back(idx);
        fnSlotPolarity_.push_back(FnRepr::Unknown);
        fnSlotLabel_.push_back(label);
        fnSlotLine_.push_back(line);
        fnSlotKey_.push_back(key);
        fnSlotVoid_.push_back(0);
        fnSlotRigid_.push_back(0);
        fnSlotMustProve_.push_back(0);
        fnSlotVoidSource_.push_back(0);
        fnSlotSourceSpelling_.push_back("");
        return idx;
    }

    int fnSlotForSymbol(const std::shared_ptr<Symbol>& sym, const std::string& label, int line) {
        int idx = fnSlot("v:" + std::to_string(sym->id), label, line);
        // A fn-typed declaration fixes its storage representation for the
        // whole program; mixing shapes there is a conflict, never the
        // recoverable *void ambiguity (CIDER-BUG-118).
        fnSlotRigid_[fnFind(idx)] = 1;
        return idx;
    }

    int fnParamSlot(const std::shared_ptr<FunctionSymbol>& fnSym, size_t index, int line) {
        return fnSlot(fnParamSlotKey(fnSym, index),
                      "parameter " + std::to_string(index + 1) + " of '" + fnSym->name + "'", line);
    }

    // CIDER-BUG-118: `*void` storage carries fn values without fixing a
    // representation, so its slot is provenance-only: mixing shapes in it is
    // recorded as Mixed (never a storage conflict) and the post-pass never
    // annotates its TypeAST.
    int fnVoidSlot(const std::string& key, const std::string& label, int line) {
        int idx = fnSlot(key, label, line);
        fnSlotVoid_[idx] = 1;
        return idx;
    }

    int fnSlotForVoidSymbol(const std::shared_ptr<Symbol>& sym, const std::string& label, int line) {
        return fnVoidSlot("v:" + std::to_string(sym->id), label, line);
    }

    int fnVoidParamSlot(const std::shared_ptr<FunctionSymbol>& fnSym, size_t index, int line) {
        return fnVoidSlot(fnParamSlotKey(fnSym, index),
                          "parameter " + std::to_string(index + 1) + " of '" + fnSym->name + "'", line);
    }

    // The untyped carrier whose runtime shape the inference must prove: a
    // `*void`/`**void` spelling or an alias chain resolving to one. Only this
    // family keeps unchecked cast semantics into the closure spelling
    // (CIDER-BUG-118); fn(...) spellings are handled by the signature rules.
    bool isVoidCarrier(const TypeInfo& t) {
        if (t.isFunctionPointer || t.isArray) return false;
        TypeInfo r = t.pointerLevel > 0 ? t : resolveType(t);
        if (r.isFunctionPointer || r.isArray) return false;
        return r.name == "void" && r.pointerLevel >= 1;
    }

    // A nilPtr source (through any grouping) is representation-neutral:
    // either shape fits, exactly like the nil initializer of a func(...)
    // declaration.
    static bool isNilSource(Expression* expr) {
        while (auto* g = dynamic_cast<GroupingExpr*>(expr)) expr = g->getExpression();
        return dynamic_cast<NilPtrExpr*>(expr) != nullptr;
    }

    // CIDER-BUG-140: canonical storage identity for the indirect *void
    // carriers (fields, array elements, pointees) so a store and a later
    // load/cast of the same location share one provenance slot. Only
    // identifier-rooted chains are identified: `p[i]` is keyed by its base
    // (elements of one array share a slot, so mixing shapes across them
    // resolves to Mixed and is rejected), and pointer arithmetic, `this`,
    // module and call-result bases return an empty key (their provenance
    // stays unknown, so a closure cast from them is conservatively
    // rejected). Nested chains compose (`s.inner.f`, `(*p).f` and `p->f`
    // all reach one key).
    //
    // The key follows the syntactic chain, not alias analysis: two distinct
    // chains that name the same runtime location (an object's `s.f` versus a
    // pointer's `p->f`, a base-class and a derived-class view) keep separate
    // slots, so a read through the other chain is unproven and rejected.
    // That is the CIDER-BUG-140 conservative fallback, and it is what keeps
    // the verdict sound without interprocedural alias tracking.
    std::string fnStorageKey(Expression* expr) {
        if (auto* g = dynamic_cast<GroupingExpr*>(expr)) {
            return fnStorageKey(g->getExpression());
        }
        if (auto* id = dynamic_cast<IdentifierExpr*>(expr)) {
            auto sym = resolveScoped(id->getName());
            if (sym && sym->kind == SymbolKind::VARIABLE) {
                return "v:" + std::to_string(sym->id);
            }
            return "";
        }
        if (auto* mem = dynamic_cast<MemberAccessExpr*>(expr)) {
            std::string base = fnStorageKey(mem->getObject());
            if (base.empty()) return "";
            if (mem->isArrow()) base = "d:" + base;
            return "f:" + base + "." + mem->getMember();
        }
        if (auto* ix = dynamic_cast<IndexExpr*>(expr)) {
            std::string base = fnStorageKey(ix->getObject());
            if (base.empty()) return "";
            return "e:" + base;
        }
        if (auto* un = dynamic_cast<UnaryExpr*>(expr)) {
            if (un->getOperator() == "*") {
                std::string base = fnStorageKey(un->getOperand());
                if (base.empty()) return "";
                return "d:" + base;
            }
        }
        return "";
    }

    // CIDER-BUG-140: the provenance slot of a *void field/element/pointee.
    // Returns -1 when the storage chain is not identifiable (the cast side
    // then conservatively requires proof before producing a closure).
    int fnCarrierSlot(Expression* expr, int line) {
        std::string key = fnStorageKey(expr);
        if (key.empty()) return -1;
        return fnVoidSlot(key, "a *void storage location", line);
    }

    int fnFind(int x) {
        while (fnSlotParent_[x] != x) {
            fnSlotParent_[x] = fnSlotParent_[fnSlotParent_[x]];
            x = fnSlotParent_[x];
        }
        return x;
    }

    void fnConflictError(const std::string& label, int line) {
        std::string what = label.empty() ? std::string("an fn(...) value") : label;
        error("Closure representation conflict for " + what +
                  ": a closure object and a plain fn(...) value are both used with it, but one "
                  "storage holds one representation (CIDER-BUG-068)",
              line);
    }

    void fnConstrain(int slot, FnRepr repr, int line, const std::string& label) {
        if (repr == FnRepr::Unknown) return;
        int r = fnFind(slot);
        if (fnSlotPolarity_[r] == FnRepr::Unknown) {
            fnSlotPolarity_[r] = repr;
            fnSlotLine_[r] = line;
            return;
        }
        if (fnSlotPolarity_[r] != repr) {
            // CIDER-BUG-118: *void provenance has no fixed representation,
            // so both shapes may flow through it over the program. Record
            // the ambiguity for a later closure cast instead of reporting a
            // storage conflict no program site is responsible for. A
            // component with a fn(...) storage (rigid) keeps the
            // CIDER-BUG-068 conflict.
            if (!fnSlotRigid_[r]) {
                fnSlotPolarity_[r] = FnRepr::Mixed;
                return;
            }
            fnConflictError(label.empty() ? fnSlotLabel_[r] : label, line);
        }
    }

    void fnUnion(int a, int b, int line) {
        a = fnFind(a);
        b = fnFind(b);
        if (a == b) return;
        FnRepr pa = fnSlotPolarity_[a];
        FnRepr pb = fnSlotPolarity_[b];
        if (pa != FnRepr::Unknown && pb != FnRepr::Unknown && pa != pb) {
            // CIDER-BUG-118: merging the two shapes through *void provenance
            // is not a storage conflict; resolve the component to Mixed and
            // leave the verdict to the closure-cast site. A fn(...) storage
            // (rigid) in either component keeps the CIDER-BUG-068 conflict.
            if (fnSlotRigid_[a] || fnSlotRigid_[b]) {
                std::string la = fnSlotLabel_[a].empty() ? std::string("one fn(...) storage") : fnSlotLabel_[a];
                std::string lb = fnSlotLabel_[b].empty() ? std::string("another fn(...) storage") : fnSlotLabel_[b];
                error("Closure representation conflict between " + la + " and " + lb +
                          ": a closure object and a plain fn(...) value are both used with them, but one "
                          "storage holds one representation (CIDER-BUG-068)",
                      line);
                return;
            }
            pa = FnRepr::Mixed;
            fnSlotPolarity_[a] = FnRepr::Mixed;
        }
        fnSlotParent_[b] = a;
        fnSlotRigid_[a] = fnSlotRigid_[a] || fnSlotRigid_[b];
        if (pa == FnRepr::Unknown) {
            fnSlotPolarity_[a] = pb;
            if (pb != FnRepr::Unknown) fnSlotLine_[a] = line;
        }
    }

    // Constrain/union `slot` from a fn-typed expression's runtime shape.
    // Returns true when the expression's representation was tied to the slot
    // (a direct constraint or a slot edge); false for representation-neutral
    // shapes (nilPtr) and shapes whose unchecked semantics are unchanged.
    // CIDER-BUG-118: `*void` sources carry a fn value without a spelling, so
    // they are followed through the same edges as fn(...) values. When
    // `viaVoid` is given it is set as soon as the recorded shape passed
    // through a *void carrier, so a fn(...) declaration can annotate its
    // TypeAST for ir_gen (whose own retyping cannot see through *void).
    bool fnRecordExpr(int slot, Expression* expr, TypeInfo type, int line,
                      bool* viaVoid = nullptr) {
        TypeInfo t = resolveFunctionPointerAlias(type);
        bool fnTyped = t.isFunctionPointer && !t.isArray;
        bool voidCarrier = isVoidCarrier(t);
        // CIDER-BUG-140: follow grouping/cast chains and either ternary branch
        // before the spelling test, so a proof can be traced even through
        // intermediate typed pointers (`(c as *u8) as func(...)` is a closure
        // object, `(add1 as *u8) as func(...)` is a raw code pointer) and
        // parenthesized wrappers.
        if (auto* c = dynamic_cast<CastExpr*>(expr)) {
            auto it = castSourceTypes_.find(c);
            if (it == castSourceTypes_.end()) return false;
            return fnRecordExpr(slot, c->getExpression(), it->second, line, viaVoid);
        }
        if (auto* g = dynamic_cast<GroupingExpr*>(expr)) {
            return fnRecordExpr(slot, g->getExpression(), type, line, viaVoid);
        }
        if (auto* t3 = dynamic_cast<TernaryExpr*>(expr)) {
            // Either branch can supply the runtime value. CIDER-BUG-140: each
            // branch is recorded on its own child slot first, so a branch that
            // only contributes a union edge to an unproven *void storage (an
            // identifier whose component is Unknown) is not mistaken for a
            // proven shape; the children then merge into the caller's slot.
            TypeInfo thenType = type;
            TypeInfo elseType = type;
            auto branchIt = ternaryBranchTypes_.find(t3);
            if (branchIt != ternaryBranchTypes_.end()) {
                thenType = branchIt->second.first;
                elseType = branchIt->second.second;
            }
            int thenSlot = fnSlot("b:" + std::to_string(fnBranchCheckCount_++), "", line);
            int elseSlot = fnSlot("b:" + std::to_string(fnBranchCheckCount_++), "", line);
            bool thenRec = fnRecordExpr(thenSlot, t3->getThenExpr(), thenType, line, viaVoid);
            FnRepr thenPol = fnSlotPolarity_[fnFind(thenSlot)];
            bool elseRec = fnRecordExpr(elseSlot, t3->getElseExpr(), elseType, line, viaVoid);
            FnRepr elsePol = fnSlotPolarity_[fnFind(elseSlot)];
            fnUnion(slot, thenSlot, line);
            fnUnion(slot, elseSlot, line);
            bool thenOk = isNilSource(t3->getThenExpr()) ||
                          (thenRec && thenPol != FnRepr::Unknown);
            bool elseOk = isNilSource(t3->getElseExpr()) ||
                          (elseRec && elsePol != FnRepr::Unknown);
            // An unproven branch (not nil) leaves the runtime shape unknown,
            // so the recorded branch alone must not prove the closure shape.
            // Both-unproven stays Unknown and is judged by the source rule.
            if (thenOk != elseOk) fnTaintUnproven(slot, line);
            return thenRec || elseRec;
        }
        if (!fnTyped && !voidCarrier) return false;
        if (voidCarrier && viaVoid) *viaVoid = true;
        if (fnTyped && t.isClosure) {
            fnConstrain(slot, FnRepr::Closure, line, fnSlotLabel_[fnFind(slot)]);
            return true;
        }
        if (!expr || dynamic_cast<NilPtrExpr*>(expr)) return false;  // nil: either shape
        if (auto* id = dynamic_cast<IdentifierExpr*>(expr)) {
            auto sym = resolveScoped(id->getName());
            if (sym && sym->kind == SymbolKind::FUNCTION) {
                // A named function is a raw code pointer even when it is
                // laundered through *void storage (CIDER-BUG-118).
                fnConstrain(slot, FnRepr::Plain, line, fnSlotLabel_[fnFind(slot)]);
                return true;
            }
            if (sym && sym->kind == SymbolKind::VARIABLE) {
                auto pk = fnParamSymKey_.find(sym->id);
                if (pk != fnParamSymKey_.end()) {
                    // The argument is a parameter of the enclosing function:
                    // unify the two parameters instead of pinning a shape.
                    fnUnion(slot, fnSlot(pk->second, "", line), line);
                    return true;
                }
                TypeInfo symFn = resolveFunctionPointerAlias(sym->type);
                if (symFn.isFunctionPointer && !symFn.isArray) {
                    fnUnion(slot, fnSlotForSymbol(sym, "variable '" + sym->name + "'", line), line);
                    return true;
                }
                if (isVoidCarrier(sym->type)) {
                    // Forward the *void storage's provenance (CIDER-BUG-118).
                    fnUnion(slot, fnSlotForVoidSymbol(sym, "variable '" + sym->name + "'", line), line);
                    return true;
                }
            }
            return false;
        }
        if (fnTyped && dynamic_cast<CallExpr*>(expr)) {
            // Only fn(...)-returning calls reach here; `func(...)`-returning
            // calls are closure-typed and handled above. Returning a closure
            // where `fn(...)` is declared is rejected, so a plain fn result
            // is definitely a raw pointer. A *void-returning call stays
            // unconstrained: the value could be either shape.
            fnConstrain(slot, FnRepr::Plain, line, fnSlotLabel_[fnFind(slot)]);
            return true;
        }
        if (fnTyped && (dynamic_cast<MemberAccessExpr*>(expr) || dynamic_cast<IndexExpr*>(expr))) {
            // A fn-typed struct/class field or array element can only ever
            // hold raw fn values: storing a closure there is rejected (the
            // field/array storage has no representation refinement), so a
            // module-qualified `m.fn` and `arr[i]` are definitely plain. A
            // *void-typed field/element keeps its unchecked semantics.
            fnConstrain(slot, FnRepr::Plain, line, fnSlotLabel_[fnFind(slot)]);
            return true;
        }
        if (voidCarrier) {
            // CIDER-BUG-140: a *void field/element/pointee forwards its
            // storage slot's provenance, so a store and a later cast share
            // one verdict. An unidentified chain stays unconstrained and the
            // cast side rejects it conservatively.
            if (dynamic_cast<MemberAccessExpr*>(expr) || dynamic_cast<IndexExpr*>(expr) ||
                (dynamic_cast<UnaryExpr*>(expr) && static_cast<UnaryExpr*>(expr)->getOperator() == "*")) {
                int carrier = fnCarrierSlot(expr, line);
                if (carrier < 0) return false;
                fnUnion(slot, carrier, line);
                return true;
            }
        }
        // Any other shape keeps its pre-existing unchecked semantics; leave
        // the slot unconstrained (plain default).
        return false;
    }

    // CIDER-BUG-140: a cast-free conversion into a closure-typed destination
    // (declaration, assignment, argument, return) takes the same proof
    // obligation as an explicit cast: the *void source must resolve to a
    // closure object. The check slot is provenance-only and is judged by the
    // cast-verdict pass below with conversion wording. Returns true when a
    // check slot was recorded.
    bool fnCheckClosureConversion(TypeInfo dstType, Expression* value, TypeInfo valueType,
                                  int line) {
        TypeInfo dst = resolveFunctionPointerAlias(dstType);
        if (!(dst.isFunctionPointer && !dst.isArray && dst.isClosure)) return false;
        if (!isVoidCarrier(valueType) || isNilSource(value)) return false;
        int slot = fnVoidSlot("q:" + std::to_string(fnConvertCheckCount_++), dst.toString(), line);
        fnSlotVoidSource_[slot] = 1;
        fnSlotMustProve_[slot] = 1;
        fnRecordExpr(slot, value, valueType, line);
        return true;
    }

    // CIDER-BUG-140: true when any slot in `root`'s component is a *void
    // provenance slot. A closure cast whose value flowed through *void must
    // resolve to Closure even when the immediate spelling is fn(...): the
    // *void provenance is the only evidence of the runtime shape.
    bool fnComponentHasVoid(int root) {
        for (size_t i = 1; i < fnSlotParent_.size(); ++i) {
            if (fnSlotVoid_[i] && fnFind((int)i) == root) return true;
        }
        return false;
    }

    // CIDER-BUG-140: record that a component's runtime shape is unproven (an
    // unrecorded ternary branch): mark it Mixed so the verdict/storage rules
    // report instead of silently adopting the recorded branch's shape. No
    // immediate conflict diagnostic: the component may legitimately be a
    // *void provenance slot that no site consumes.
    void fnTaintUnproven(int slot, int line) {
        int r = fnFind(slot);
        if (fnSlotPolarity_[r] != FnRepr::Mixed) {
            fnSlotPolarity_[r] = FnRepr::Mixed;
            fnSlotLine_[r] = line;
        }
    }

    // Resolve the union-find after the whole program has been checked and
    // mark every closure-represented parameter on its TypeAST. Variable
    // slots whose declaration had no representation-bearing initializer
    // (an uninitialized `fn(...)` local/global later assigned a closure)
    // are annotated too; the other variable slots are re-derived by ir_gen
    // from their initializer/assignment shapes. Parameter slots with no
    // registration belong to a callee whose representation cannot be
    // inferred (a method or an extern function): report instead of emitting
    // a call through the wrong convention.
    void resolveFnRepresentations() {
        for (size_t i = 1; i < fnSlotParent_.size(); ++i) (void)fnFind((int)i);
        // CIDER-BUG-140: a root judged by a cast/conversion check site (a
        // c:/q: slot) reports there; the storage-level Mixed rule stays quiet
        // for those roots so one problem produces one diagnostic.
        std::set<int> checkRoots;
        for (size_t i = 1; i < fnSlotParent_.size(); ++i) {
            const std::string& key = fnSlotKey_[i];
            if (!key.empty() && (key[0] == 'c' || key[0] == 'q')) {
                checkRoots.insert(fnFind((int)i));
            }
        }
        for (size_t i = 1; i < fnSlotParent_.size(); ++i) {
            int root = fnFind((int)i);
            FnRepr pol = fnSlotPolarity_[root];
            const std::string& key = fnSlotKey_[i];
            // CIDER-BUG-140: storage whose component received both shapes
            // through *void resolves to Mixed; a real fn(...) storage cannot
            // commit to either representation (a *void provenance slot may
            // legitimately be Mixed).
            if (pol == FnRepr::Mixed && !fnSlotVoid_[i] && !key.empty()) {
                if (key[0] == 's') {
                    error("A closure value may be stored into " +
                              (fnSlotLabel_[i].empty() ? std::string("a fn(...) storage location")
                                                       : fnSlotLabel_[i]) +
                              ", which keeps the plain fn(...) representation; the value received "
                              "both a closure object and a plain fn(...) value through *void, so "
                              "the store cannot be proven safe (CIDER-BUG-140)",
                          fnSlotLine_[i]);
                    continue;
                }
                if (key[0] == 'x') {
                    error("A closure value may be returned from " +
                              (fnSlotLabel_[i].empty() ? std::string("a function") : fnSlotLabel_[i]) +
                              ", which is declared with a plain fn(...) return type; the value "
                              "received both a closure object and a plain fn(...) value through "
                              "*void, so the return cannot be proven safe (CIDER-BUG-140: use the "
                              "func(...) return spelling)",
                          fnSlotLine_[i]);
                    continue;
                }
                if ((key[0] == 'v' || key[0] == 'p') && fnSlotRigid_[root] &&
                    !checkRoots.count(root)) {
                    error("Closure representation conflict for " +
                              (fnSlotLabel_[i].empty() ? std::string("an fn(...) storage")
                                                       : fnSlotLabel_[i]) +
                              ": the value received both a closure object and a plain fn(...) "
                              "value through *void, but the storage holds one representation "
                              "(CIDER-BUG-140)",
                          fnSlotLine_[i]);
                    continue;
                }
            }
            if (pol != FnRepr::Closure) continue;
            // CIDER-BUG-118: a *void provenance slot has no declaration/ABI
            // to refine (the *void TypeAST stays untyped); only real
            // fn(...) storage is annotated. A fn(...) member of the same
            // component is still annotated below (its own flag is clear).
            if (fnSlotVoid_[i]) continue;
            // CIDER-BUG-093: cast-check slots are not storage; they only
            // carry the source shape. A closure-resolved one is the allowed
            // `closure as func(...)` reinterpretation, so nothing to mark.
            // CIDER-BUG-140: the same holds for cast-free conversion checks
            // and for the per-branch ternary child slots.
            if (!key.empty() &&
                (key[0] == 'c' || key[0] == 'q' || key[0] == 'b')) {
                continue;
            }
            auto astIt = fnParamAst_.find(key);
            if (astIt != fnParamAst_.end()) {
                if (astIt->second) astIt->second->setClosureRepr(true);
                continue;
            }
            if (!key.empty() && key[0] == 'v') {
                auto varIt = fnVarAst_.find(key);
                if (varIt != fnVarAst_.end() && varIt->second) {
                    varIt->second->setClosureRepr(true);
                }
                continue;
            }
            if (!key.empty() && key[0] == 'x') {
                error("A closure value is returned from " +
                          (fnSlotLabel_[i].empty() ? std::string("a function") : fnSlotLabel_[i]) +
                          ", which is declared with a plain fn(...) return type; returning a "
                          "closure object where fn(...) is declared is not supported "
                          "(CIDER-BUG-068: use the func(...) return spelling)",
                      fnSlotLine_[i]);
                continue;
            }
            if (!key.empty() && key[0] == 's') {
                error("A closure value is stored into " +
                          (fnSlotLabel_[i].empty() ? std::string("a fn(...) storage location")
                                                   : fnSlotLabel_[i]) +
                          ", which keeps the plain fn(...) representation; storing a closure "
                          "object into a field, array element or pointee is not supported "
                          "(CIDER-BUG-068)",
                      fnSlotLine_[i]);
                continue;
            }
            std::string label = fnSlotLabel_[i].empty() ? "an fn(...) parameter" : fnSlotLabel_[i];
            error("A closure value is passed to " + label +
                      ", but that parameter's closure representation cannot be inferred "
                      "(CIDER-BUG-068 supports fn(...) parameters of free functions)",
                  fnSlotLine_[i]);
        }
        // CIDER-BUG-093: a cast to the closure spelling func(...) whose
        // source resolves to a plain fn pointer would leave a raw code
        // pointer in a closure-typed value — every later call would read
        // the function's first bytes as the closure object and pass the
        // pointer as the hidden object argument. Report it instead of
        // emitting the misread call. A source that resolves Closure (or
        // stays Unknown: nilPtr and other unchecked fn(...) shapes) is
        // untouched.
        // CIDER-BUG-118: a cast whose source flows through *void must prove
        // the closure shape. A proven closure object is the allowed
        // reinterpretation; a source that resolves to a plain raw pointer is
        // reported by the rule above, and an unproven *void (opaque pointer,
        // extern storage, uninferable parameter) is reported here too — the
        // pre-118 leniency for unproven sources was exactly the launder.
        std::set<int> reportedCastRoots;
        for (size_t i = 1; i < fnSlotParent_.size(); ++i) {
            const std::string& key = fnSlotKey_[i];
            if (key.empty() || (key[0] != 'c' && key[0] != 'q')) continue;
            int root = fnFind((int)i);
            FnRepr pol = fnSlotPolarity_[root];
            if (pol == FnRepr::Closure) continue;
            // The pre-118 rule for fn(...) sources: only a resolved plain
            // pointer is reported. A source chain that reached a tracked
            // *void storage location must prove closure instead
            // (CIDER-BUG-118); CIDER-BUG-140 extends that to every
            // conversion slot and to a fn(...) source whose component picked
            // up *void provenance.
            if (pol == FnRepr::Unknown && !fnSlotMustProve_[i] && !fnComponentHasVoid(root)) {
                continue;
            }
            if (!reportedCastRoots.insert(root).second) continue;
            bool conversion = key[0] == 'q';
            bool voidSource = fnSlotVoidSource_[i];
            const std::string& label = fnSlotLabel_[i];
            if (pol == FnRepr::Plain) {
                if (conversion) {
                    error("A plain fn(...) value cannot be converted to the closure spelling. "
                          "Expected " + label +
                              ", got a plain fn(...) value: a raw code pointer is not a closure "
                              "object {fnptr, captures...} whose calls pass the object as a hidden "
                              "first argument (CIDER-BUG-140). Use the fn(...) spelling, or store "
                              "the function in a func(...) variable (CIDER-BUG-063 adapts it to "
                              "the raw pointer representation).",
                          fnSlotLine_[i]);
                } else {
                    error("Function cast cannot produce the closure spelling. Expected " +
                              label +
                              ", got a plain fn(...) value: a raw code pointer is not a closure "
                              "object {fnptr, captures...} whose calls pass the object as a hidden "
                              "first argument (CIDER-BUG-093). Use the fn(...) spelling, or store "
                              "the function in a func(...) variable (CIDER-BUG-063 adapts it to "
                              "the raw pointer representation).",
                          fnSlotLine_[i]);
                }
            } else if (pol == FnRepr::Mixed) {
                if (conversion) {
                    error("The closure representation of " + label +
                              " cannot be inferred: its *void source received both a closure "
                              "object and a plain fn(...) value, so its fn representation cannot "
                              "be proven (CIDER-BUG-140). Use an explicit func(...) cast where "
                              "the value is known to be a closure object, or the fn(...) "
                              "spelling.",
                          fnSlotLine_[i]);
                } else if (voidSource) {
                    error("Function cast cannot produce the closure spelling. Expected " +
                              label +
                              ", but the *void source received both a closure object and a plain "
                              "fn(...) value, so its fn representation cannot be proven "
                              "(CIDER-BUG-118). Use the fn(...) spelling, or store the function "
                              "in a func(...) variable (CIDER-BUG-063 adapts it to the raw "
                              "pointer representation).",
                          fnSlotLine_[i]);
                } else {
                    error("Function cast cannot produce the closure spelling. Expected " +
                              label + ", but the source value of type " +
                              fnSlotSourceSpelling_[i] +
                              " flows from a *void component that received both a closure object "
                              "and a plain fn(...) value, so its fn representation cannot be "
                              "proven (CIDER-BUG-140). Use the fn(...) spelling, or store the "
                              "function in a func(...) variable (CIDER-BUG-063 adapts it to the "
                              "raw pointer representation).",
                          fnSlotLine_[i]);
                }
            } else {
                if (conversion) {
                    error("The closure representation of " + label +
                              " cannot be inferred: its *void source's fn representation is not "
                              "known to be a closure object, and a raw code pointer would be "
                              "read as {fnptr, captures...} and passed as the hidden first "
                              "argument of every call (CIDER-BUG-140). Store a closure object in "
                              "the *void first, or use the fn(...) spelling.",
                          fnSlotLine_[i]);
                } else if (voidSource) {
                    error("Function cast cannot produce the closure spelling. Expected " +
                              label +
                              ", got a *void value whose fn representation is not known to be a "
                              "closure object: a raw code pointer would be read as {fnptr, "
                              "captures...} and passed as the hidden first argument of every "
                              "call (CIDER-BUG-118). Store a closure object in the *void first, "
                              "or use the fn(...) spelling.",
                          fnSlotLine_[i]);
                } else {
                    error("Function cast cannot produce the closure spelling. Expected " +
                              label + ", got a value of type " + fnSlotSourceSpelling_[i] +
                              " whose fn representation is not known to be a closure object: "
                              "only a *void source whose provenance proves a closure object can "
                              "feed the closure spelling (CIDER-BUG-140). Store the closure in a "
                              "*void the inference can see (a local, global, parameter, or a "
                              "*void field, array element or pointee it can trace), or use the "
                              "fn(...) spelling.",
                          fnSlotLine_[i]);
                }
            }
        }
    }

    // CID-005/T7 fix round 1 (F3): TypeInfo::signatureEquals compares the
    // nested parameter/return TypeInfos exactly, so a fn value spelled
    // `msabi fn(*P) -> EFI_STATUS` did not match `msabi fn(*P) -> u64`
    // (likewise an outer `type Cb = ...` spelling against the expanded
    // one). Resolve the alias chain on every nested type with the same
    // cycle-safe resolveType used everywhere else, cloning the signature
    // vectors instead of mutating the shared ones. The ABI marker and the
    // exact comparison then stay untouched: `u64` vs `int` still differs.
    // Arrays keep their declared spelling (resolveType would drop the
    // wrapper and its size, mirroring resolveFunctionPointerAlias).
    //
    // CID-005/T7 fix round 2: a cyclic fn-type alias (`type F = fn(F) ->
    // void;`, or `type A = B; type B = fn(A) -> void;`) makes this walk and
    // resolveType recurse into each other with no fixed point — resolveType's
    // visited set is per-call, and it stops at the fn-typed alias *value*
    // whose nested parameter is the alias spelling again, which this walk
    // then re-resolves. Guard the walk with the set of spellings on the
    // active expansion path (insert before resolving, erase on the way out,
    // the recursive-walk analogue of resolveTypeImpl's stop): a spelling
    // repeated up the path stops there, while the same spelling in sibling
    // positions (e.g. `fn(EFI_STATUS) -> EFI_STATUS`) still resolves
    // independently. Cyclic aliases are checker-accepted
    // (`tests/programs/cyclic_alias_*`), so every walk must terminate.
    //
    // CID-005 final-review (F1): the walk above re-expands every alias
    // occurrence, so a *branched* DAG (`type A0 = fn(A1, A1) -> void;
    // type A1 = fn(A2, A2) -> void; ...`) doubles the work per level: the
    // 22-level DAG took ~100 s on the branch tip while the base compiler
    // rejected the same source in ~7 ms. Bound the total number of
    // expansions per resolution; once the budget is spent the remaining
    // spellings stay unresolved and the comparison keeps both unexpanded
    // types, which is the pre-alias path (every F3/ABI/cyclic result below
    // the budget is unchanged). A memoized DAG would share subtrees, but
    // TypeInfo equality walks them without a visited set, so the bound must
    // count repeated sibling expansions — as it does here.
    static constexpr int kMaxSignatureAliasExpansions = 8192;

    TypeInfo resolveSignatureAliases(TypeInfo t, bool* exhausted = nullptr) {
        std::set<std::string> visiting;
        int budget = kMaxSignatureAliasExpansions;
        bool hitBudget = false;
        TypeInfo resolved = resolveSignatureAliasesImpl(t, visiting, budget, hitBudget);
        // CIDER-BUG-022: OR into the caller's flag — resolveSignatureAliasPair
        // resolves side 1 then side 2 under one flag, and side 2 must not
        // clobber side 1's exhaustion (a partial expansion produced a ~46 KB
        // return-type diagnostic).
        if (exhausted) *exhausted = *exhausted || hitBudget;
        return resolved;
    }

    // Resolves both sides of a comparison under one exhaustion flag: when
    // either side runs out of budget neither is replaced, so the caller
    // compares the unexpanded spellings instead of a partial expansion.
    void resolveSignatureAliasPair(TypeInfo& t1, TypeInfo& t2) {
        bool exhausted = false;
        TypeInfo r1 = resolveSignatureAliases(t1, &exhausted);
        TypeInfo r2 = resolveSignatureAliases(t2, &exhausted);
        if (!exhausted) {
            t1 = r1;
            t2 = r2;
        }
    }

    TypeInfo resolveSignatureAliasesImpl(TypeInfo t, std::set<std::string>& visiting,
                                         int& budget, bool& exhausted) {
        if (t.isFunctionPointer) {
            if (t.fnParamTypes) {
                auto params = std::make_shared<std::vector<TypeInfo>>();
                params->reserve(t.fnParamTypes->size());
                for (const auto& p : *t.fnParamTypes) {
                    params->push_back(resolveSignatureAliasesImpl(p, visiting, budget, exhausted));
                }
                t.fnParamTypes = params;
            }
            if (t.fnReturnType) {
                t.fnReturnType = std::make_shared<TypeInfo>(
                    resolveSignatureAliasesImpl(*t.fnReturnType, visiting, budget, exhausted));
            }
            return t;
        }
        if (t.isArray) return t;
        if (t.name.empty() || !visiting.insert(t.name).second) return t;
        if (budget <= 0) {
            exhausted = true;
            visiting.erase(t.name);
            return t;
        }
        --budget;
        TypeInfo resolved = resolveType(t);
        if (resolved.isFunctionPointer) {
            resolved = resolveSignatureAliasesImpl(resolved, visiting, budget, exhausted);
        }
        visiting.erase(t.name);
        return resolved;
    }

    // The language's builtin value spellings are never symbol-table
    // entries, so an unresolved name that matches one is a primitive and
    // not a candidate by-value aggregate (used by the call gate below).
    static bool isBuiltinTypeName(const std::string& n) {
        return n == "void" || n == "bool" || n == "string" || n == "char" || n == "byte" ||
               n == "int" || n == "float" || n == "f32" || n == "f64" ||
               n == "u8" || n == "i8" || n == "u16" || n == "i16" ||
               n == "u32" || n == "i32" || n == "u64" || n == "i64";
    }

    // CID-005/T7 fix round 1 (F4): classify a fn-pointer parameter/return
    // for the by-value indirect-call gate. Mirrors ir_gen::isStructValueType:
    // alias-resolve to a fixed point, skip resolved pointers/arrays/fn
    // values, and fall back to the spelling's defining module for a bare
    // nominal that does not resolve at the use site (qualified `mod::X`
    // goes through resolveCanonical). An alias whose resolved target does
    // not resolve either is rejected conservatively: the pre-alias gate saw
    // the STRUCT-kind alias provider and rejected it, so e.g.
    // `type NopeAlias = Nope` must not silently widen the gate. A direct
    // unresolved spelling (e.g. a template instantiation name) never
    // reached an alias provider and keeps its previous accept path.
    bool isByValueAggregateAtCall(const TypeInfo& t) {
        // CIDER-BUG-174: mirror ir_gen::isByValueAggregate — an array of fn
        // values carries the element's isFunctionPointer flag, so only a
        // scalar fn value is excluded here; the array falls through to the
        // fixed-size-array rule below (indirect calls cannot pass it,
        // exactly like any other array).
        // CIDER-BUG-157: an array descriptor carries its element's pointer
        // level, so the array test comes before the pointer rejections
        // (mirrors ir_gen::isByValueAggregate for closure-element arrays).
        TypeInfo resolved = resolveType(t);
        if (resolved.isArray) return resolved.arraySize > 0;
        if (t.pointerLevel > 0 || (t.isFunctionPointer && !t.isArray)) return false;
        if (resolved.pointerLevel > 0 || (resolved.isFunctionPointer && !resolved.isArray)) {
            return false;
        }
        // CIDER-BUG-045: fixed-size arrays are by-value aggregates (params
        // copy, returns use sret), which indirect calls do not pass.
        if (t.isArray) return t.arraySize > 0;
        if (isBuiltinTypeName(resolved.name)) return false;
        std::shared_ptr<Symbol> s;
        if (resolved.name.find("::") != std::string::npos) {
            s = resolveCanonical(resolved.name);
            if (!s) return true;  // unresolvable qualified nominal: stay conservative
        } else {
            s = resolveScoped(resolved.name);
            if (!s) {
                if (resolved.name == t.name) return false;  // no alias followed
                // Same fallback ir_gen::isStructValueType applies with the
                // callee's defining module: try the module that owns the
                // alias spelling before giving up.
                auto spellingSym = resolveScoped(t.name);
                std::string ownerMod = spellingSym ? definingModuleOf(spellingSym) : "";
                Scope* ms = scopeForModule(ownerMod);
                if (ms) s = ms->resolveLocal(resolved.name);
                if (!s) return true;  // dangling alias target: stay conservative
            }
        }
        return s->kind == SymbolKind::STRUCT || s->kind == SymbolKind::CLASS;
    }

    // CID-003: the float-family spellings, shared by areTypesCompatible
    // and visit(BinaryExpr) so the two copies cannot drift.
    static bool isFloatName(const std::string& n) { return n == "float" || n == "f64"; }

    // CIDER-BUG-048: integer scalar spellings that ir_gen materialises as an
    // integer .data/.bss slot (mapType). `bool` is included: it maps to I32
    // storage and a bool literal must not be silently dropped to zero.
    static bool isIntegerName(const std::string& n) {
        return n == "int" || n == "u8" || n == "i8" || n == "u16" || n == "i16" ||
               n == "u32" || n == "i32" || n == "u64" || n == "i64" ||
               n == "char" || n == "byte" || n == "bool";
    }

    // CIDER-FEAT-002: the EnumSymbol a nominal type spelling names, or null.
    // Handles bare (`Color`), dotted (`lib.Color`) and canonical
    // (`lib::Color`) spellings through the normal nominal lookup. Pointers,
    // arrays, fn types and interfaces are never enums, and neither is an
    // alias that does not resolve to one.
    std::shared_ptr<EnumSymbol> resolveEnumSymbol(TypeInfo t) {
        if (t.pointerLevel > 0 || t.isArray || t.isFunctionPointer || t.isInterface) return nullptr;
        if (t.name.empty() || t.name == "error" || t.name == "wildcard") return nullptr;
        TypeInfo resolved = resolveType(canonicalizeNominal(t, -1));
        if (resolved.pointerLevel > 0 || resolved.isArray || resolved.isFunctionPointer) return nullptr;
        auto sym = resolveScoped(resolved.name);
        if (!sym || sym->kind != SymbolKind::ENUM) return nullptr;
        return std::static_pointer_cast<EnumSymbol>(sym);
    }

    // CIDER-FEAT-002: canonical nominal type of an enum definition.
    TypeInfo enumTypeOf(const std::shared_ptr<EnumSymbol>& sym) {
        TypeInfo t;
        t.name = canonicalTypeName(sym->name, definingModuleOf(sym));
        return t;
    }

    // CIDER-FEAT-002: the enum a type-provider symbol names — a direct
    // enum, or a `type Alias = Enum` provider whose chain ends in one.
    std::shared_ptr<EnumSymbol> enumSymbolOfTypeName(const std::shared_ptr<Symbol>& sym) {
        if (!sym) return nullptr;
        if (sym->kind == SymbolKind::ENUM) return std::static_pointer_cast<EnumSymbol>(sym);
        if (sym->isTypeAlias) {
            // Root the chain at the alias's own canonical name, so its bare
            // target spelling resolves in the alias's defining module
            // (`type A = Shape` inside module m means m.Shape here).
            TypeInfo rooted;
            rooted.name = canonicalTypeName(sym->name, definingModuleOf(sym));
            TypeInfo target = resolveType(rooted);
            auto resolved = resolveScoped(target.name);
            if (resolved && resolved->kind == SymbolKind::ENUM) {
                return std::static_pointer_cast<EnumSymbol>(resolved);
            }
        }
        return nullptr;
    }

    // CIDER-FEAT-002: the enum a QUALIFIER expression names (`Color`,
    // `lib.Color` or an alias spelling). Used where the qualifier is not a
    // value — the object of `Enum.VARIANT` must not be evaluated through
    // visit(IdentifierExpr)'s value paths.
    std::shared_ptr<EnumSymbol> enumSymbolFromQualifier(Expression* expr) {
        if (!expr) return nullptr;
        if (auto id = dynamic_cast<IdentifierExpr*>(expr)) {
            return enumSymbolOfTypeName(resolveScoped(id->getName()));
        }
        if (auto mem = dynamic_cast<MemberAccessExpr*>(expr)) {
            if (mem->isArrow()) return nullptr;
            if (auto base = dynamic_cast<IdentifierExpr*>(mem->getObject())) {
                // An ambiguous qualifier stays the module-access path's
                // diagnostic (spec §1), never a silent pick here.
                if (isAmbiguousQualifier(base->getName())) return nullptr;
                auto baseSym = resolveScoped(base->getName());
                if (baseSym && baseSym->kind == SymbolKind::MODULE) {
                    auto modSym = std::static_pointer_cast<ModuleSymbol>(baseSym);
                    Scope* ms = modSym->scope;
                    auto target = ms ? ms->resolveLocal(mem->getMember()) : nullptr;
                    return enumSymbolOfTypeName(target);
                }
            }
        }
        return nullptr;
    }

    // CIDER-FEAT-002: fold `Color.RED` / `lib.Color.RED` to the variant's
    // integer value for constant-expression contexts. A module-qualified
    // bare variant (`lib.RED`) is an ordinary const symbol and is folded by
    // the existing module-const paths instead.
    bool resolveEnumVariantConst(Expression* expr, int64_t& value) {
        auto mem = dynamic_cast<MemberAccessExpr*>(expr);
        if (!mem || mem->isArrow()) return false;
        auto enumSym = enumSymbolFromQualifier(mem->getObject());
        if (!enumSym) return false;
        return enumSym->findVariant(mem->getMember(), value);
    }

    // CIDER-FEAT-002: an enum-typed scalar takes the same "no runtime
    // initializer" route as an integer (its underlying representation is
    // int-sized), so global/class-field constants rules apply to it too.
    bool isIntegerLikeType(TypeInfo t) {
        t = resolveType(t);
        if (t.pointerLevel > 0 || t.isArray) return false;
        return isIntegerName(t.name) || resolveEnumSymbol(t) != nullptr;
    }

    // CIDER-BUG-167: the volatile mark is a property of the object an
    // access goes through, not of the value being stored. Reading a
    // volatile object yields a plain value and a plain value may
    // initialize/assign a volatile-qualified destination; the aggregate
    // copy marks the observable accesses either way (CIDER-BUG-133/144).
    // Scalars already compare equal through the numeric fallback below and
    // interfaces through checkInterfaceValue, so only aggregates (structs,
    // classes, arrays) were left with the exact identity test and its
    // embedded `isVolatile` comparison. `withoutVolatileMarks` builds the
    // value shape with every mark cleared for that comparison; pointer
    // types keep the exact rule, because there the flag describes the
    // pointee and dropping it would silently widen what the pointer
    // permits.
    static TypeInfo withoutVolatileMarks(TypeInfo t) {
        t.isVolatile = false;
        if (t.arrayElement) {
            // arrayElement descriptors are shared between TypeInfo copies,
            // so clear the flag on a clone instead of in place.
            t.arrayElement =
                std::make_shared<TypeInfo>(withoutVolatileMarks(*t.arrayElement));
        }
        return t;
    }

    bool areTypesCompatible(TypeInfo t1, TypeInfo t2) {
        if (t1.name == "wildcard" || t2.name == "wildcard") return true;
        t1 = resolveType(t1);
        t2 = resolveType(t2);
        // CIDER-BUG-085: every spelling of a nominal interface carries the
        // same flag. A signature that reached this comparison without a
        // collection/declaration boundary (a cast target, a synthesized
        // signature copy) may still have an unresolved nested interface
        // param; resolve both sides with the same spelling-derived rule.
        // This cannot mask a real mismatch: the flag is only ever SET from
        // the resolved kind of the spelling's own name and is never cleared,
        // so `fn(int) -> int` vs `fn(I) -> int` and `fn(I) -> int` vs
        // `fn(J) -> int` still differ by name, while the two spellings of
        // the one type stop disagreeing about internal representation.
        resolveInterfaceFlag(t1);
        resolveInterfaceFlag(t2);
        // CID-005/T7 fix round 1 (F3): resolve alias chains inside fn
        // signatures too, so a typedef spelling and its expansion are
        // interchangeable in every fn-value position (declaration,
        // assignment, argument passing). The comparison itself stays
        // exact, including the ABI marker. F1: the resolution is bounded,
        // and on budget exhaustion both sides stay unexpanded.
        resolveSignatureAliasPair(t1, t2);
        // CID-020/021 §1: compare nominal type identities canonically, so
        // same-named types in different modules differ.
        t1 = canonicalizeNominal(t1);
        t2 = canonicalizeNominal(t2);

        // CIDER-BUG-100: the interface flag is a value-level marker (this
        // type is a 16-byte fat pointer). On a pointer type it instead
        // records the pointee's kind, and only one spelling carries it:
        // `&t` inherits it from the interface operand, while the straight
        // declaration `*Shape` comes from the type AST, which cannot
        // resolve interface symbols. Two pointers to the same interface are
        // the same type, so compare nominal identity and level without the
        // inherited marker. The flag is only ever SET from the resolved
        // kind of the spelling's own name, never semantic on a pointer, so
        // this cannot mask a real mismatch: `*I` vs `*J`, `*I` vs `*int`
        // and `*I` vs `**I` still differ by name/level.
        if (t1.pointerLevel > 0 && t2.pointerLevel > 0) {
            t1.isInterface = false;
            t2.isInterface = false;
        }

        if (t1 == t2) return true;

        // CIDER-BUG-167: same value shape with the volatile marks dropped
        // is compatible (see withoutVolatileMarks). Checked before the
        // array-shape rules below, whose sameArrayElement comparison would
        // otherwise reject a nested array that differs only by its marks.
        // Top-level pointers are excluded: there isVolatile qualifies the
        // pointee, not the value being copied.
        if (t1.pointerLevel == 0 && t2.pointerLevel == 0 &&
            withoutVolatileMarks(t1) == withoutVolatileMarks(t2)) {
            return true;
        }

        // CIDER-FEAT-002: enum conversion rules (t1 = target/expected,
        // t2 = source/actual).
        //  * two enums interconvert only when they are the SAME enum;
        //  * an enum value flows implicitly to an integer-family target
        //    (the legacy bare spelling assigns to `int`);
        //  * an integer does NOT flow to an enum target without an explicit
        //    `value as Enum` cast.
        auto enum1 = resolveEnumSymbol(t1);
        auto enum2 = resolveEnumSymbol(t2);
        if (enum1 || enum2) {
            if (enum1 && enum2) return enum1 == enum2;
            if (enum2) return isIntegerName(t1.name) && t1.pointerLevel == 0 && !t1.isArray;
            return false;
        }

        // CID-005 fix round 1 (Q1 follow-up): two array values must also
        // agree on their element count. The integer fallback below ignores
        // isArray/arraySize, so `[u16; 4] = u"AB"` and `s = u"AB"` into an
        // existing [u16; 4] were accepted and then copied 8 bytes from a
        // 6-byte data block (or silently truncated). Array/pointer mixes
        // keep their existing rules (including the u16 decay branch below).
        // CIDER-BUG-052: nested arrays must also agree on their inner shape
        // — the flattened name made `[[u8; 2]; 3]` and `[[u8; 3]; 3]`
        // compatible through the numeric fallback.
        if (t1.isArray && t2.isArray &&
            (t1.arraySize != t2.arraySize || !t1.sameArrayElement(t2))) {
            return false;
        }

        // CIDER-BUG-052: an array value and a scalar are never compatible.
        // The numeric fallback below is size-blind and reads the flattened
        // (innermost) element name, so `[u8; 2] = 5` — and, for a nested
        // array element, `m[0] = 5` — passed and ir_gen then MEMCPY'd from
        // the scalar as if it were an address.
        if (t1.isArray != t2.isArray && t1.pointerLevel == 0 && t2.pointerLevel == 0) return false;

        // CID-006: two fn types match only via the exact signature test
        // above. One fn + one *void (nilPtr, legacy untyped references)
        // stays compatible both ways; fn arrays never decay to *void.
        if (t1.isFunctionPointer || t2.isFunctionPointer) {
            if (t1.isFunctionPointer && t2.isFunctionPointer) return false;
            if (t1.isArray || t2.isArray) return false;
            const TypeInfo& other = t1.isFunctionPointer ? t2 : t1;
            return other.name == "void" && other.pointerLevel >= 1;
        }

        auto isInteger = [](const std::string& name) {
            return name == "int" || name == "u8" || name == "i8" || name == "u16" || name == "i16" ||
                   name == "u32" || name == "i32" || name == "u64" || name == "i64" || name == "char" || name == "byte";
        };

        if (isFloatName(t1.name) && isInteger(t2.name) && t1.pointerLevel == 0 && t2.pointerLevel == 0) return true;
        if (isFloatName(t1.name) && isFloatName(t2.name) && t1.pointerLevel == 0 && t2.pointerLevel == 0) return true;
        if (isInteger(t1.name) && isInteger(t2.name) && t1.pointerLevel == 0 && t2.pointerLevel == 0) return true;
        // C-style conversions: any T* <-> void* (both directions, any
        // pointer depth). The reference only allowed T* -> void*, which
        // rejected nilPtr initializations (*void -> T*) and the sieve
        // benchmark's free(*bool).
        if ((t1.name == "void" && t1.pointerLevel >= 1 && t2.pointerLevel >= t1.pointerLevel) ||
            (t2.name == "void" && t2.pointerLevel >= 1 && t1.pointerLevel >= t2.pointerLevel)) return true;

        // String literals decay to byte pointers (C `char*`): `foo("hi")`
        // where `foo(p: char*)`, `var p: char* = "hi"`, etc. Also covers
        // `u8*`/`i8*`/`byte*` (framebuffer, serial) and `void*`.
        auto isBytePtr = [](const TypeInfo& t) {
            return t.pointerLevel >= 1 &&
                   (t.name == "char" || t.name == "u8" || t.name == "i8" ||
                    t.name == "byte" || t.name == "void");
        };
        if (t1.name == "string" && t1.pointerLevel == 0 && isBytePtr(t2)) return true;
        if (t2.name == "string" && t2.pointerLevel == 0 && isBytePtr(t1)) return true;

        // CID-005: `u"..."` is a [u16; N] array that decays to *u16
        // (never to *u8). Only u16 arrays decay; all other arrays keep
        // their previous strict behavior.
        if (t1.name == "u16" && t1.isArray && !t2.isArray && t2.pointerLevel == 1 &&
            (t2.name == "u16" || t2.name == "void")) return true;
        if (t2.name == "u16" && t2.isArray && !t1.isArray && t1.pointerLevel == 1 &&
            (t1.name == "u16" || t1.name == "void")) return true;

        // Pointer covariance for classes
        if (t1.pointerLevel > 0 && t2.pointerLevel > 0 && t1.pointerLevel == t2.pointerLevel) {
            auto sym2 = resolveScoped(t2.name);
            if (sym2 && sym2->kind == SymbolKind::CLASS) {
                auto classSym2 = std::static_pointer_cast<StructSymbol>(sym2);
                std::string parent = classSym2->parentName;
                while (!parent.empty()) {
                    // CIDER-BUG-124: the stored parent spelling may be
                    // dotted (`lib.Base`) or an alias. Compare the written
                    // spelling canonically first (as before), then the
                    // resolved class's canonical identity (spec §1) so a
                    // hierarchy spelled either way keeps matching.
                    TypeInfo parentType;
                    parentType.name = parent;
                    parentType.pointerLevel = 0;
                    if (canonicalizeNominal(parentType).name == t1.name) return true;
                    auto parentSym = resolveParentClass(parent);
                    if (!parentSym || parentSym->kind != SymbolKind::CLASS) break;
                    if (canonicalTypeName(parentSym->name, definingModuleOf(parentSym)) ==
                        t1.name) {
                        return true;
                    }
                    parent = std::static_pointer_cast<StructSymbol>(parentSym)->parentName;
                }
            }
        }

        return false;
    }

    // Visitor methods
    void visit(AstProgram& program) override {
        // Attribute diagnostics to the program's own file when known
        // (Issue #16): imported ASTs carry their module path.
        std::string prevFile = reporter_.getFile();
        if (!program.getSourcePath().empty()) reporter_.setFile(program.getSourcePath());
        // CIDER-FEAT-006: module mode (`--emit-meta`) collects the root file
        // as a named module. Install the same module context the imported
        // path uses, so definitions resolve with their own module current.
        std::string prevModule = currentModule_;
        Scope* prevModuleScope = currentModuleScope_;
        Scope* prevScope = symbolTable_.getCurrentScope();
        auto pmIt = programModules_.find(&program);
        if (pmIt != programModules_.end()) {
            currentModule_ = pmIt->second;
            currentModuleScope_ = nullptr;
            if (!program.getSourcePath().empty()) {
                auto scIt = moduleScopes_.find(program.getSourcePath());
                if (scIt != moduleScopes_.end()) currentModuleScope_ = scIt->second;
            }
            if (currentModuleScope_) symbolTable_.setCurrentScope(currentModuleScope_);
        }
        // CIDER-FEAT-005: warning analysis is active for the root program
        // only (this visitor runs exactly once, for the root AST).
        bool prevWarningsActive = warningsActive_;
        warningsActive_ = warningsEnabled_;
        checkTopLevelStatements(program);
        warningsActive_ = prevWarningsActive;

        auto mainSym = resolveScoped("main");
        auto startSym = resolveScoped("_start");

        bool hasMain = mainSym && mainSym->kind == SymbolKind::FUNCTION;
        bool hasStart = startSym && startSym->kind == SymbolKind::FUNCTION;

        auto uefiSym = resolveScoped("efi_main");
        bool hasUefiEntry = uefiEntryAllowed_ && uefiSym && uefiSym->kind == SymbolKind::FUNCTION;
        if (!moduleMode_ && !hasMain && !hasStart && !hasUefiEntry) {
            reporter_.error("Missing entry point: 'main' or '_start' function is required.", 1, 1);
        }

        // CID-020/021 (spec §6.4): check each imported module's body with
        // that module as current, so unqualified names bind per-module and
        // latent errors surface as file-attributed diagnostics at check
        // time. The main/_start presence check above stays main-only.
        // CIDER-FEAT-006: `.cider-meta` snapshots were checked when their
        // module was compiled; re-checking the declaration-only AST would
        // duplicate that work and reject bodies the snapshot omits.
        for (auto& imported : program.getImportedAsts()) {
            if (imported->isMeta()) continue;
            checkImportedProgram(*imported);
        }
        // CIDER-FEAT-005: imports are declared before any use, so they are
        // reported once the whole root program has been checked.
        if (warningsEnabled_) warnings_.reportUnusedImports();
        reporter_.setFile(prevFile);
        symbolTable_.setCurrentScope(prevScope);
        currentModule_ = prevModule;
        currentModuleScope_ = prevModuleScope;
    }

    void checkTopLevelStatements(AstProgram& program) {
        for (auto& stmt : program.getStatements()) {
            auto type = stmt->getType();
            if (type != ASTNode::VAR_DECL_STMT &&
                type != ASTNode::FUNCTION_DECL_STMT &&
                type != ASTNode::STRUCT_DECL_STMT &&
                type != ASTNode::ENUM_DECL_STMT &&
                type != ASTNode::CLASS_DECL_STMT &&
                type != ASTNode::TYPE_ALIAS_STMT &&
                type != ASTNode::IMPORT_STMT &&
                type != ASTNode::FROM_IMPORT_STMT &&
                type != ASTNode::EXPORT_STMT &&
                type != ASTNode::EXPORT_EXPRESSION_STMT &&
                type != ASTNode::ASM_STMT &&
                type != ASTNode::INTERFACE_DECL_STMT) {
                error("Executable statements are not allowed at the global scope", stmt->getLine());
            }
            stmt->accept(*this);
        }
    }

    void checkImportedProgram(AstProgram& imported) {
        std::string prevFile = reporter_.getFile();
        if (!imported.getSourcePath().empty()) reporter_.setFile(imported.getSourcePath());
        std::string prevModule = currentModule_;
        Scope* prevModuleScope = currentModuleScope_;
        Scope* prevScope = symbolTable_.getCurrentScope();
        auto pmIt = programModules_.find(&imported);
        currentModule_ = (pmIt != programModules_.end()) ? pmIt->second : std::string();
        currentModuleScope_ = nullptr;
        if (!imported.getSourcePath().empty()) {
            auto scIt = moduleScopes_.find(imported.getSourcePath());
            if (scIt != moduleScopes_.end()) currentModuleScope_ = scIt->second;
        }
        if (currentModuleScope_) symbolTable_.setCurrentScope(currentModuleScope_);
        checkTopLevelStatements(imported);
        symbolTable_.setCurrentScope(prevScope);
        currentModule_ = prevModule;
        currentModuleScope_ = prevModuleScope;
        reporter_.setFile(prevFile);
    }

    // CIDER-BUG-048: while folding a const's initializer, identifiers must
    // resolve in the declaration's own module, not at the use site (a module
    // const may reference an unexported const next to it). Null at top level
    // means "resolve in the current scope".
    Scope* constEvalScope_ = nullptr;

    std::shared_ptr<Symbol> resolveConstIdentifier(const std::string& name) {
        if (!constEvalScope_) return resolveScoped(name);
        if (auto sym = constEvalScope_->resolveLocal(name)) return sym;
        Scope* root = symbolTable_.getRootScope();
        return root ? root->resolveLocal(name) : nullptr;
    }

    // Shape check for compile-time integer constants (CID-007). Paired
    // with IRGenerator::tryEvalConstU64, which evaluates the same shapes;
    // anything rejected here would otherwise silently zero-initialise.
    // CIDER-BUG-048: identifiers naming `const` symbols recurse into the
    // declaration's initializer, so `var g: int = SOME_CONST;` and const
    // chains keep their value (order-independently, with cycle detection).
    bool isConstIntExpr(Expression* expr) {
        std::set<uint64_t> visiting;
        return isConstIntExpr(expr, visiting);
    }

    bool isConstIntExpr(Expression* expr, std::set<uint64_t>& visiting) {
        if (!expr) return false;
        if (auto num = dynamic_cast<NumberExpr*>(expr)) return !num->isFloat();
        if (dynamic_cast<CharExpr*>(expr)) return true;
        if (dynamic_cast<BooleanExpr*>(expr)) return true;
        if (auto grp = dynamic_cast<GroupingExpr*>(expr)) return isConstIntExpr(grp->getExpression(), visiting);
        if (auto un = dynamic_cast<UnaryExpr*>(expr)) {
            return (un->getOperator() == "-" || un->getOperator() == "+" ||
                    un->getOperator() == "~") &&
                   isConstIntExpr(un->getOperand(), visiting);
        }
        if (auto bin = dynamic_cast<BinaryExpr*>(expr)) {
            const std::string& op = bin->getOperator();
            bool supported = (op == "+" || op == "-" || op == "*" || op == "/" || op == "%" ||
                              op == "<<" || op == ">>" || op == "&" || op == "|" || op == "^");
            return supported && isConstIntExpr(bin->getLeft(), visiting) &&
                   isConstIntExpr(bin->getRight(), visiting);
        }
        if (auto cast = dynamic_cast<CastExpr*>(expr)) {
            return isConstIntExpr(cast->getExpression(), visiting);
        }
        if (auto id = dynamic_cast<IdentifierExpr*>(expr)) {
            return isConstSymbolIntExpr(resolveConstIdentifier(id->getName()), visiting);
        }
        // CIDER-FEAT-002: `Color.RED` / `lib.Color.RED` is an enum constant.
        if (auto mem = dynamic_cast<MemberAccessExpr*>(expr)) {
            if (mem->isArrow()) return false;
            int64_t enumValue = 0;
            if (resolveEnumVariantConst(expr, enumValue)) return true;
        }
        // CIDER-BUG-048: a module-qualified const (`mc.K`) is the same
        // constant as a from-import alias; resolve the member in the
        // module's own scope (mirrors isConstantFunctionPointerInit).
        if (auto mem = dynamic_cast<MemberAccessExpr*>(expr)) {
            if (mem->isArrow()) return false;
            auto base = dynamic_cast<IdentifierExpr*>(mem->getObject());
            if (!base) return false;
            auto baseSym = resolveConstIdentifier(base->getName());
            if (!baseSym || baseSym->kind != SymbolKind::MODULE) return false;
            auto modSym = std::static_pointer_cast<ModuleSymbol>(baseSym);
            auto target = modSym->scope ? modSym->scope->resolveLocal(mem->getMember()) : nullptr;
            return isConstSymbolIntExpr(target, visiting);
        }
        return false;
    }

    // CIDER-BUG-048: foldable `const` symbol for the integer shape check.
    // Recurse into the declaration's initializer (in its own scope, cycle-
    // checked). Enum variants and initializer-less consts are constants by
    // construction.
    bool isConstSymbolIntExpr(const std::shared_ptr<Symbol>& sym,
                              std::set<uint64_t>& visiting) {
        if (!sym || !sym->isConst || sym->kind != SymbolKind::VARIABLE) return false;
        if (!sym->constInit) return true;
        if (!visiting.insert(sym->id).second) return false;
        Scope* prevScope = constEvalScope_;
        if (Scope* declScope = scopeForModule(definingModuleOf(sym))) constEvalScope_ = declScope;
        bool ok = isConstIntExpr(sym->constInit, visiting);
        constEvalScope_ = prevScope;
        visiting.erase(sym->id);
        return ok;
    }

    // Shape check for compile-time constant float initializers (CID-003
    // follow-up W5). Mirrors IRGenerator::tryEvalConstF64's accepted shapes;
    // anything rejected here would otherwise silently BSS-zero a float
    // global. Numeric literals of either kind are fine: integer literals
    // and integer-only subexpressions fold exactly as they do locally.
    static bool isConstFloatExpr(Expression* expr) {
        if (!expr) return false;
        if (dynamic_cast<NumberExpr*>(expr)) return true;
        if (dynamic_cast<CharExpr*>(expr)) return true;
        if (auto grp = dynamic_cast<GroupingExpr*>(expr)) return isConstFloatExpr(grp->getExpression());
        if (auto un = dynamic_cast<UnaryExpr*>(expr)) {
            return (un->getOperator() == "-" || un->getOperator() == "+" ||
                    un->getOperator() == "~") &&
                   isConstFloatExpr(un->getOperand());
        }
        if (auto cast = dynamic_cast<CastExpr*>(expr)) return isConstFloatExpr(cast->getExpression());
        if (auto bin = dynamic_cast<BinaryExpr*>(expr)) {
            const std::string& op = bin->getOperator();
            bool supported = (op == "+" || op == "-" || op == "*" || op == "/" || op == "%" ||
                              op == "<<" || op == ">>" || op == "&" || op == "|" || op == "^");
            return supported && isConstFloatExpr(bin->getLeft()) && isConstFloatExpr(bin->getRight());
        }
        return false;
    }

    // CID-006: the only global fn initializers ir_gen can fold into a
    // .data cell are `nilPtr` and an identifier naming a function
    // (grouping allowed). Everything else would silently read 0.
    bool isConstantFunctionPointerInit(Expression* expr) {
        // CIDER-BUG-024: unwrap grouping and casts — `add as *void` /
        // `add as fn(...)` are constant spellings of the function name
        // (ir_gen's functionPointerInitTarget folds the same shape).
        while (true) {
            if (auto grp = dynamic_cast<GroupingExpr*>(expr)) { expr = grp->getExpression(); continue; }
            if (auto cast = dynamic_cast<CastExpr*>(expr)) { expr = cast->getExpression(); continue; }
            break;
        }
        if (!expr) return false;
        if (dynamic_cast<NilPtrExpr*>(expr)) return true;
        if (auto id = dynamic_cast<IdentifierExpr*>(expr)) {
            auto sym = resolveScoped(id->getName());
            return sym && sym->kind == SymbolKind::FUNCTION;
        }
        // CIDER-BUG-023: a module-qualified function value (`b.inc`) is
        // the same constant; the ir_gen fold resolves the member in the
        // module scope and emits the mangled label.
        if (auto mem = dynamic_cast<MemberAccessExpr*>(expr)) {
            if (!mem->isArrow()) {
                if (auto base = dynamic_cast<IdentifierExpr*>(mem->getObject())) {
                    auto baseSym = resolveScoped(base->getName());
                    if (baseSym && baseSym->kind == SymbolKind::MODULE) {
                        auto modSym = std::static_pointer_cast<ModuleSymbol>(baseSym);
                        Scope* ms = modSym->scope;
                        auto target = ms ? ms->resolveLocal(mem->getMember()) : nullptr;
                        return target && target->kind == SymbolKind::FUNCTION;
                    }
                }
            }
        }
        return false;
    }

    // CIDER-BUG-157: a fixed-array descriptor copies its element's own
    // pointer level (a closure object or a pointer element carries one), so
    // an array of closure-typed elements has pointerLevel > 0 even though
    // the VALUE is the array object, not a pointer. The language has no
    // pointer-to-array spelling (`*[T; N]` is rejected by the parser), so
    // `isArray` alone identifies the array value; the list/element rules
    // below key on this helper instead of `pointerLevel == 0`.
    static bool isFixedArrayValue(const TypeInfo& t) {
        return t.isArray && t.arraySize > 0;
    }

    // CIDER-BUG-212: true when a by-value aggregate EMBEDS an interface value
    // anywhere in its layout — a bare interface field, a fixed array of
    // interfaces, or a struct/class field that (transitively) embeds one.
    //
    // CIDER-BUG-202 established this for a returned fixed array whose LEAVES
    // are interface elements, walking only ARRAY dimensions. A returned
    // struct or class carrying an interface field was invisible to it: the
    // program kept the untagged representation, the sret copy moved the fat
    // pointer verbatim, and dispatch after return read the callee's dead
    // frame slot (a nil slot reads as a zeroed object, so `area()` returned 0
    // instead of 9). This walks the whole by-value layout instead.
    //
    // This is the same by-value layout walk the IR generator's boxing uses
    // (`registerEmbeddedDestructors`' traversal, shared rule), so the two
    // cannot disagree about which programs need the tagged boxing
    // representation. `defModule` is the module the spelling was written in,
    // threaded the way ir_gen's `fieldTypeModule` threads a field's own:
    // an inherited field keeps its declaring class's module, which the
    // aggregate's module may not be able to resolve the spelling in.
    // The `depth` bound is the one hasInterfaceArrayElement already used: a
    // cyclic row alias (`type T = [T;2]`) and a cyclic field chain both
    // terminate.
    bool hasEmbeddedInterfaceValue(const TypeInfo& t, const std::string& defModule, int depth) {
        if (depth > 8) return false;
        TypeInfo resolved = resolveType(t);
        if (resolved.pointerLevel > 0 || resolved.isSlice || resolved.isFunctionPointer) {
            return false;
        }
        if (isFixedArrayValue(resolved)) {
            return hasEmbeddedInterfaceValue(resolved.elementType(), defModule, depth + 1);
        }
        // A bare interface leaf: the 16-byte fat pointer itself. The flag is
        // resolved by name so an alias spelling of the interface counts too.
        TypeInfo leaf = resolved;
        if (resolveInterfaceSymbolInModule(leaf, defModule)) return true;
        // Otherwise a struct/class value may embed one through its fields. An
        // alias provider (a `type X = Y` spelling) is not a by-value nominal:
        // resolveType already followed the chain above, so a symbol still
        // carrying a target name is a spelling the layout walk cannot read
        // fields from.
        auto sym = resolveScopedInModule(resolved.name, defModule);
        if (!sym || (sym->kind != SymbolKind::STRUCT && sym->kind != SymbolKind::CLASS)) {
            return false;
        }
        if (!sym->type.name.empty()) return false;
        auto aggSym = std::static_pointer_cast<StructSymbol>(sym);
        std::string ownerMod = definingModuleOf(sym);
        for (const auto& field : aggSym->fields) {
            // CIDER-BUG-090: per-field declaring module.
            std::string fieldMod = field.typeModule.empty() ? ownerMod : field.typeModule;
            if (hasEmbeddedInterfaceValue(field.type, fieldMod, depth + 1)) return true;
        }
        return false;
    }

    // Fixed-size array elements from a list literal (CID-007): element
    // count and per-element types. Shared by the declaration-time
    // initializer and CIDER-BUG-112's reassignment so the two sites cannot
    // drift; `where` names the site in diagnostics ("declaration of 'x'" /
    // "assignment to 'x'"). Returns false after reporting.
    // CIDER-BUG-092: declaredType is the alias-resolved shape (an alias
    // spelling carries isArray=false until its chain resolves).
    bool checkFixedArrayElements(const std::string& where, const TypeInfo& declaredType,
                                 ListExpr& list, int line) {
        auto& elems = list.getElements();
        if ((int)elems.size() != declaredType.arraySize) {
            error("Array initializer size mismatch in " + where + ": expected " +
                      std::to_string(declaredType.arraySize) + ", got " +
                      std::to_string(elems.size()), line);
            return false;
        }
        TypeInfo elemDecl = declaredType.elementType();
        // CIDER-BUG-135: an interface-typed element is the same 16-byte fat
        // pointer as a non-list interface target, so a concrete implementor
        // element must take the normal interface conversion instead of the
        // exact compatibility test. Recover the flag on the scalar element
        // (a spelling stripped from the array can reach here unflagged),
        // mirroring the declaration-time resolution.
        resolveInterfaceFlag(elemDecl);
        // CIDER-BUG-163: a row element spelled as an alias of an array
        // (`type Row = [u8;2]`; `[Row;3]`) carries no isArray flag until
        // its chain resolves, so resolve the shape used for the nested
        // literal test. The generic comparison below keeps the declared
        // spelling for its diagnostics.
        TypeInfo elemArray = resolveType(elemDecl);
        for (auto& e : elems) {
            // CIDER-BUG-163: an element that is itself a list literal for a
            // fixed-array row is validated against the inner shape
            // recursively. visit(ListExpr) infers a flat `*T` for every list
            // literal, so the generic compatibility test below saw `*int`
            // for `[1,2]` where the declared row is `[int;2]` and rejected
            // the declaration.
            if (auto* inner = dynamic_cast<ListExpr*>(e.get());
                inner && isFixedArrayValue(elemArray)) {
                if (!checkFixedArrayElements(where, elemArray, *inner, line)) return false;
                continue;
            }
            e->accept(*this);
            if (elemDecl.isInterface) {
                // CIDER-BUG-135: the shared rule reports the same clear
                // errors as `var i: Shape = <value>` (a non-implementor
                // names the missing method; a different interface is
                // rejected), and stops the remaining element checks.
                if (!checkInterfaceValue(elemDecl, lastExprType_, line)) return false;
            } else if (!areTypesCompatible(elemDecl, lastExprType_)) {
                error("Array element type mismatch in " + where + ": expected " +
                          elemDecl.toString() + ", got " + lastExprType_.toString(), line);
                return false;
            }
            // CIDER-BUG-068: a fixed plain fn(...) array element keeps the
            // plain raw-pointer shape, so a statically-plain element that
            // resolves to a closure (a closure-valued parameter/local) is
            // reported in the post-pass instead of being stored as an object
            // pointer.
            // CIDER-BUG-157: a closure-typed (func(...)) element already has
            // the closure representation, so a direct closure value is the
            // matching shape and needs no rejection slot; only a *void value
            // takes the cast-free conversion proof, exactly like a direct
            // field/element assignment (a plain fn(...) value is rejected by
            // the compatibility test above).
            TypeInfo elemFn = resolveFunctionPointerAlias(elemDecl);
            if (elemFn.isFunctionPointer && !elemFn.isArray) {
                if (elemFn.isClosure) {
                    if (isVoidCarrier(lastExprType_)) {
                        fnCheckClosureConversion(elemDecl, e.get(), lastExprType_, line);
                    }
                } else {
                    int slot = fnSlot("s:" + std::to_string(fnStoreCheckCount_++),
                                      "an array element", line);
                    fnRecordExpr(slot, e.get(), lastExprType_, line);
                }
            }
        }
        return true;
    }

    // Fixed-size array from a list literal (CID-007): `var t: [u8; 4] =
    // [10, 20, 30, 40]`. Globals have no runtime initialiser, so their
    // elements must be constant (a clear error otherwise, never silent
    // BSS-zero). Only called for fixed-size array declarations;
    // pointer-from-list (`var l: *int = [...]`) keeps the legacy path.
    void checkFixedArrayInit(VarDeclStmt& stmt, const TypeInfo& declaredType) {
        auto* list = static_cast<ListExpr*>(stmt.getInitializer());
        std::string where = "declaration of '" + stmt.getName() + "'";
        if (!checkFixedArrayElements(where, declaredType, *list, stmt.getLine())) return;
        // CID-020/021 (spec §6): scopeDepth skips module scopes via the
        // scope-kind flag, so a module-level array still counts as global.
        if (symbolTable_.scopeDepth() == 0 && !isConstArrayLiteral(*list, declaredType)) {
            error("Global array initializer in declaration of '" + stmt.getName() +
                      "' must use constant expressions", stmt.getLine());
            return;
        }
        lastExprType_ = declaredType;
    }

    // CIDER-BUG-163: the global constant rule for a (possibly nested)
    // fixed-array list literal. An inner list recurses with its declared row
    // shape; a scalar element keeps the existing isConstIntExpr shape check
    // (paired with ir_gen's constant-byte emission). A non-list inner
    // expression is never constant-foldable here, matching the flat rule.
    bool isConstArrayLiteral(ListExpr& list, const TypeInfo& arrayType) {
        // CIDER-BUG-163: the row element may be an alias spelling of an
        // array (`type Row = [u8;2]`; `[Row;3]`), so resolve before the
        // shape test or the nested literal is treated as a scalar.
        TypeInfo elemDecl = resolveType(arrayType.elementType());
        for (auto& e : list.getElements()) {
            if (auto* inner = dynamic_cast<ListExpr*>(e.get());
                inner && isFixedArrayValue(elemDecl)) {
                if (!isConstArrayLiteral(*inner, elemDecl)) return false;
            } else if (!isConstIntExpr(e.get())) {
                return false;
            }
        }
        return true;
    }

    // CIDER-BUG-112: reassigning a list literal into an existing fixed
    // array (`s = [1,2,3]`, direct or alias spelling) takes the same
    // size/element checks as the declaration-time initializer. Unlike a
    // module-scope declaration, the assignment is a runtime site even for
    // a global target, so the elements may be runtime expressions; the
    // shared helper visits each element exactly once.
    void checkFixedArrayAssign(const std::string& where, const TypeInfo& declaredType,
                               ListExpr& list, int line) {
        if (!checkFixedArrayElements(where, declaredType, list, line)) return;
        lastExprType_ = declaredType;
    }

    // CIDER-BUG-018: strict array element identity. The numeric fallback in
    // areTypesCompatible is size-blind (`[i32;3] = u"AB"` passed), so array
    // copies that must be byte-exact compare the resolved, canonicalized
    // element types instead.
    bool sameArrayElementType(TypeInfo a, TypeInfo b) {
        a = a.elementType();
        b = b.elementType();
        a = canonicalizeNominal(resolveType(a));
        b = canonicalizeNominal(resolveType(b));
        return a == b;
    }

    // CID-005 fix round 1 (Q1): a fixed-size array initialised from a wide
    // literal. The literal's checker type is [u16; units+1], but
    // areTypesCompatible's integer fallback is size-blind, so the declared
    // size must be compared explicitly (mirrors checkFixedArrayInit).
    // Without this, `[u16; 4] = u"AB"` passed and the array MEMCPY read two
    // bytes past the data block; `[u16; 2] = u"AB"` silently truncated.
    // CIDER-BUG-092: declaredType is the alias-resolved shape, mirroring
    // checkFixedArrayInit.
    void checkWideArrayInit(VarDeclStmt& stmt, const TypeInfo& declaredType,
                            WideStringExpr& init) {
        int expected = (int)init.getValue().size() + 1;  // units + NUL unit
        if (declaredType.arraySize != expected) {
            error("Array initializer size mismatch in declaration of '" + stmt.getName() +
                      "': expected " + std::to_string(declaredType.arraySize) +
                      ", got " + std::to_string(expected), stmt.getLine());
            return;
        }
        TypeInfo elemDecl = declaredType.elementType();
        elemDecl = resolveType(elemDecl);
        // CIDER-BUG-018: the payload is u16 units; the numeric fallback in
        // areTypesCompatible is size-blind, so `[i32;3] = u"AB"` and
        // `[f64;3] = u"AB"` passed and copied 12 bytes from a 6-byte block.
        if (elemDecl.pointerLevel > 0 || elemDecl.isArray || elemDecl.isFunctionPointer ||
            elemDecl.name != "u16") {
            error("Array element type mismatch in declaration of '" + stmt.getName() +
                      "': expected " + elemDecl.toString() + ", got u16", stmt.getLine());
            return;
        }
        lastExprType_ = declaredType;
    }

    // CIDER-BUG-057: fold a slice bound to a compile-time element index.
    // Mirrors the expression shapes isConstIntExpr accepts (literals,
    // grouping/unary/constant arithmetic, `const` references); anything
    // else is a dynamic bound and skips the static element-count check.
    bool evalConstIntBound(Expression* expr, int64_t& out) {
        std::set<uint64_t> visiting;
        return evalConstIntBound(expr, out, visiting);
    }

    bool evalConstIntBound(Expression* expr, int64_t& out, std::set<uint64_t>& visiting) {
        if (!expr) return false;
        if (auto num = dynamic_cast<NumberExpr*>(expr)) {
            if (num->isFloat()) return false;
            out = num->getIntValue();
            return true;
        }
        if (auto ch = dynamic_cast<CharExpr*>(expr)) {
            out = (int64_t)ch->getValue();
            return true;
        }
        if (auto grp = dynamic_cast<GroupingExpr*>(expr)) {
            return evalConstIntBound(grp->getExpression(), out, visiting);
        }
        if (auto un = dynamic_cast<UnaryExpr*>(expr)) {
            int64_t v = 0;
            if (!evalConstIntBound(un->getOperand(), v, visiting)) return false;
            const std::string& op = un->getOperator();
            if (op == "-") out = -v;
            else if (op == "+") out = v;
            else if (op == "~") out = ~v;
            else return false;
            return true;
        }
        if (auto bin = dynamic_cast<BinaryExpr*>(expr)) {
            int64_t l = 0, r = 0;
            if (!evalConstIntBound(bin->getLeft(), l, visiting) ||
                !evalConstIntBound(bin->getRight(), r, visiting)) {
                return false;
            }
            const std::string& op = bin->getOperator();
            if (op == "+") out = l + r;
            else if (op == "-") out = l - r;
            else if (op == "*") out = l * r;
            else if (op == "/") { if (r == 0) return false; out = l / r; }
            else if (op == "%") { if (r == 0) return false; out = l % r; }
            else if (op == "<<") { if (r < 0 || r >= 64) return false; out = (int64_t)((uint64_t)l << r); }
            else if (op == ">>") { if (r < 0 || r >= 64) return false; out = l >> r; }
            else if (op == "&") out = l & r;
            else if (op == "|") out = l | r;
            else if (op == "^") out = l ^ r;
            else return false;
            return true;
        }
        if (auto id = dynamic_cast<IdentifierExpr*>(expr)) {
            return evalConstBoundSymbol(resolveConstIdentifier(id->getName()), out, visiting);
        }
        // CIDER-FEAT-002: an enum variant spelling folds to its value.
        if (auto mem = dynamic_cast<MemberAccessExpr*>(expr)) {
            if (mem->isArrow()) return false;
            if (resolveEnumVariantConst(expr, out)) return true;
        }
        // Module-qualified const (`mc.K`), same resolution as isConstIntExpr.
        if (auto mem = dynamic_cast<MemberAccessExpr*>(expr)) {
            if (mem->isArrow()) return false;
            auto base = dynamic_cast<IdentifierExpr*>(mem->getObject());
            if (!base) return false;
            auto baseSym = resolveConstIdentifier(base->getName());
            if (!baseSym || baseSym->kind != SymbolKind::MODULE) return false;
            auto modSym = std::static_pointer_cast<ModuleSymbol>(baseSym);
            auto target = modSym->scope ? modSym->scope->resolveLocal(mem->getMember()) : nullptr;
            return evalConstBoundSymbol(target, out, visiting);
        }
        return false;
    }

    bool evalConstBoundSymbol(const std::shared_ptr<Symbol>& sym, int64_t& out,
                              std::set<uint64_t>& visiting) {
        if (!sym || !sym->isConst || sym->kind != SymbolKind::VARIABLE) return false;
        if (!sym->constInit) {
            out = (int64_t)sym->constValue;
            return true;
        }
        if (!visiting.insert(sym->id).second) return false;
        Scope* prevScope = constEvalScope_;
        if (Scope* declScope = scopeForModule(definingModuleOf(sym))) constEvalScope_ = declScope;
        bool ok = evalConstIntBound(sym->constInit, out, visiting);
        constEvalScope_ = prevScope;
        visiting.erase(sym->id);
        return ok;
    }

    // CIDER-BUG-057: `var s: [T; N] = base[start:end]` and
    // `s = base[start:end]` copy the slice's elements into the fixed array.
    // The element types must match exactly (areTypesCompatible's integer
    // fallback is size-blind), and a statically-known slice length must
    // equal N: a longer slice would silently truncate and a shorter one
    // would copy elements past the slice's end (the copy always runs for
    // the target's element count, starting at the slice's start address).
    // A dynamic length copies N elements from the start address; the
    // target size governs, and ir_gen's MEMCPY does exactly that.
    void checkSliceArrayCopy(const std::string& where, const TypeInfo& declaredType,
                             const TypeInfo& initType, SliceExpr* slice, int line) {
        TypeInfo elemDecl = canonicalizeNominal(resolveType(declaredType.elementType()));
        TypeInfo elemInit = canonicalizeNominal(resolveType(initType));
        elemInit.isSlice = false;
        if (!(elemDecl == elemInit)) {
            error("Slice element type mismatch in " + where + ": expected " +
                      elemDecl.toString() + ", got " + elemInit.toString(),
                  line);
            return;
        }
        if (!slice) return;  // e.g. a slice-typed ternary: length is dynamic
        int64_t startVal = 0;
        if (slice->getStart() && !evalConstIntBound(slice->getStart(), startVal)) return;
        int64_t endVal = 0;
        if (slice->getEnd()) {
            if (!evalConstIntBound(slice->getEnd(), endVal)) return;
        } else if (lastSliceBaseSize_ >= 0) {
            endVal = lastSliceBaseSize_;
        } else {
            return;
        }
        int64_t len = endVal - startVal;
        if (len != declaredType.arraySize) {
            error("Slice size mismatch in " + where + ": expected " +
                      std::to_string(declaredType.arraySize) + ", got " + std::to_string(len),
                  line);
        }
    }

    // CIDER-BUG-078: the interface symbol behind a type spelling, resolved
    // through `type S = Shape` chains. An alias declaration stores the ALIAS
    // NAME in every declaration/annotation, so the name-keyed interface
    // tests used to miss it (mirroring CIDER-BUG-071's shape resolution).
    // Resolve once through resolveType (cycle-safe) and key off the resolved
    // nominal. CIDER-BUG-080: dotted spellings (`lib.Shape`) resolve their
    // member in the qualifier's module scope, mirroring the 070 collector
    // fix. Null for non-interface shapes.
    std::shared_ptr<Symbol> resolveInterfaceSymbol(TypeInfo t) {
        TypeInfo resolved = resolveType(t);
        // CIDER-BUG-112: an alias spelling can resolve to an aggregate whose
        // innermost element names an interface (`type Shapes2 = [Shape; 2]`).
        // Only a scalar shape is a fat pointer; without this guard the
        // name-keyed lookup below sees the array's element name and flags
        // the whole aggregate as one interface (wrong storage size and
        // store path). Callers strip an array/pointer first when they mean
        // an element.
        if (resolved.pointerLevel > 0 || resolved.isArray) return nullptr;
        auto typeSym = resolveScoped(resolved.name);
        if (!typeSym && resolved.name.find('.') != std::string::npos) {
            auto dot = resolved.name.rfind('.');
            auto modSym = resolveScoped(resolved.name.substr(0, dot));
            if (modSym && modSym->kind == SymbolKind::MODULE) {
                auto ms = std::static_pointer_cast<ModuleSymbol>(modSym)->scope;
                if (ms) typeSym = ms->resolveLocal(resolved.name.substr(dot + 1));
            }
        }
        if (typeSym && typeSym->kind == SymbolKind::INTERFACE) return typeSym;
        return nullptr;
    }

    // CIDER-BUG-212: resolveInterfaceSymbol rooted in the module the spelling
    // was WRITTEN in, not the module currently being checked. The embedded
    // interface walk descends into a field's type, and an inherited field
    // keeps its declaring class's module (CIDER-BUG-090) — which the use
    // site's scope may not resolve the spelling in. A canonical `mod::Name`
    // resolves through resolveScopedRaw's canonical path either way, so this
    // only re-roots a BARE name, which is exactly the case a field's raw
    // spelling is.
    std::shared_ptr<Symbol> resolveInterfaceSymbolInModule(TypeInfo t, const std::string& defModule) {
        if (defModule.empty() || defModule == currentModule_) return resolveInterfaceSymbol(t);
        Scope* ms = scopeForModule(defModule);
        if (!ms) return resolveInterfaceSymbol(t);
        Scope* prevScope = symbolTable_.getCurrentScope();
        Scope* prevModuleScope = currentModuleScope_;
        std::string prevModule = currentModule_;
        currentModule_ = defModule;
        currentModuleScope_ = ms;
        symbolTable_.setCurrentScope(ms);
        auto sym = resolveInterfaceSymbol(t);
        symbolTable_.setCurrentScope(prevScope);
        currentModule_ = prevModule;
        currentModuleScope_ = prevModuleScope;
        return sym;
    }

    // CIDER-BUG-069: resolve the interface flag on a captured field/element
    // type the same way visit(VarDeclStmt) resolves a declaration — a plain
    // interface name is a fat pointer even when the flag was not set when
    // the type was captured. Arrays keep their wrapper unflagged; callers
    // resolve the element after elementType().
    // CIDER-BUG-078: follow `type S = Shape` aliases too, so an alias-typed
    // declaration/field/parameter is the same 16-byte fat pointer as the
    // direct spelling.
    // CIDER-BUG-085: recurse through fn(...) signatures. A `fn(I) -> int`
    // spelling's parameters/return are TypeInfos built by
    // TypeInfo::fromAST, which cannot resolve symbols, so an interface
    // nested in the signature starts unflagged while the same position in a
    // named function's signature carries the flag from the collector. Walks
    // the same shape canonicalizeNominal walks, so every spelling of one
    // type resolves to the same flags. Arrays keep their wrapper unflagged
    // (CIDER-BUG-069); a fn-typed element still resolves its signature.
    void resolveInterfaceFlag(TypeInfo& t) {
        if (t.isFunctionPointer) {
            if (t.fnParamTypes) {
                for (auto& p : *t.fnParamTypes) resolveInterfaceFlag(p);
            }
            if (t.fnReturnType) resolveInterfaceFlag(*t.fnReturnType);
            return;
        }
        if (t.isInterface || t.pointerLevel > 0 || t.isArray) return;
        if (resolveInterfaceSymbol(t)) t.isInterface = true;
    }

    // CIDER-ISS-011: a source interface can be projected to a target whose
    // methods are an exact prefix of the source's method order. Interface
    // vtables are plain method-pointer arrays in declaration order, so this
    // is the largest subset conversion that can copy the fat pointer without
    // changing its vtable. Each projected signature must match canonically.
    bool interfaceProjectionCompatible(const TypeInfo& targetType,
                                       const TypeInfo& sourceType) {
        auto targetSymbol = resolveInterfaceSymbol(targetType);
        auto sourceSymbol = resolveInterfaceSymbol(sourceType);
        if (!targetSymbol || !sourceSymbol) return false;
        if (targetSymbol == sourceSymbol) return true;

        auto target = std::static_pointer_cast<InterfaceSymbol>(targetSymbol);
        auto source = std::static_pointer_cast<InterfaceSymbol>(sourceSymbol);
        if (target->methods.size() > source->methods.size()) return false;

        std::string targetModule = definingModuleOf(targetSymbol);
        std::string sourceModule = definingModuleOf(sourceSymbol);
        for (size_t i = 0; i < target->methods.size(); ++i) {
            const auto& targetMethod = target->methods[i];
            const auto& sourceMethod = source->methods[i];
            if (targetMethod.name != sourceMethod.name ||
                targetMethod.parameterTypes.size() != sourceMethod.parameterTypes.size()) {
                return false;
            }

            std::string targetMethodModule = methodModuleOf(targetMethod, targetModule);
            std::string sourceMethodModule = methodModuleOf(sourceMethod, sourceModule);
            for (size_t p = 0; p < targetMethod.parameterTypes.size(); ++p) {
                if (!(canonicalSignatureType(targetMethod.parameterTypes[p], targetMethodModule) ==
                      canonicalSignatureType(sourceMethod.parameterTypes[p], sourceMethodModule))) {
                    return false;
                }
            }
            if (!(canonicalSignatureType(targetMethod.returnType, targetMethodModule) ==
                  canonicalSignatureType(sourceMethod.returnType, sourceMethodModule))) {
                return false;
            }
        }
        return true;
    }

    // The interface conversion rule, shared by declaration-time assignment,
    // reassignment, parameters, and returns so the sites cannot drift.
    // Accepts a struct/class value, a pointer to one, or a same-layout
    // interface projection. Every concrete interface method must be
    // implemented with a matching signature; anything else is rejected.
    // CIDER-BUG-135: returns whether the conversion is accepted (false
    // after reporting), so the list-element check can stop like every other
    // site; callers that only need the diagnostic may ignore it.
    // CIDER-BUG-184: `boxesConcrete` (when non-null) reports that an
    // accepted value is a concrete struct/class VALUE, the one shape ir_gen
    // must box (copy to the heap) because the interface result outlives the
    // frame that produced it. An interface source is a fat-pointer copy and
    // a single concrete pointer passes through; neither allocates.
    bool checkInterfaceValue(const TypeInfo& ifaceType, const TypeInfo& valueType, int line,
                             bool* boxesConcrete = nullptr) {
        auto ifaceSym = resolveInterfaceSymbol(ifaceType);
        if (!ifaceSym) return true;
        auto iface = std::static_pointer_cast<InterfaceSymbol>(ifaceSym);
        if (valueType.isInterface) {
            if (interfaceProjectionCompatible(ifaceType, valueType)) return true;
            error("Cannot assign interface '" + valueType.name + "' to interface '" +
                      ifaceType.name +
                      "': source must provide target methods in the same order with matching signatures",
                  line);
            return false;
        }
        TypeInfo concreteType = valueType;
        if (concreteType.pointerLevel > 0) concreteType.pointerLevel--;
        // CIDER-BUG-080: only a single indirection (class/struct value or
        // one pointer to it) is an object address. A pointer-to-pointer or
        // array value presents the address of a pointer/array, which the
        // fat pointer would then hand to method dispatch as if it were the
        // object — reject instead of silently corrupting. nilPtr (`*void`)
        // and function values are rejected by the same rule.
        if (concreteType.pointerLevel > 0 || concreteType.isArray) {
            error("Cannot assign " + valueType.toString() + " to interface '" +
                      ifaceType.name + "': expected a struct/class value or pointer", line);
            return false;
        }
        // CIDER-BUG-078: an alias spelling of a struct/class (`type T = Sq`)
        // resolves to the definition before the StructSymbol cast, exactly
        // like the direct spelling; an unresolvable/cyclic provider is not a
        // struct and keeps the rejection (alias providers are plain Symbols,
        // never StructSymbols — mirror resolveStructOrClass).
        TypeInfo concreteResolved = resolveType(concreteType);
        auto concreteSym = resolveScoped(concreteResolved.name);
        if (!concreteSym || concreteSym->isTypeAlias ||
            (concreteSym->kind != SymbolKind::STRUCT && concreteSym->kind != SymbolKind::CLASS)) {
            error("Cannot assign non-struct/class type to interface '" + ifaceType.name + "'", line);
            return false;
        }
        auto ss = std::static_pointer_cast<StructSymbol>(concreteSym);
        // CIDER-BUG-184: a value source (no indirection) is the boxing
        // shape; a single concrete pointer aliases its pointee instead.
        if (boxesConcrete && valueType.pointerLevel == 0 && !valueType.isArray &&
            !valueType.isInterface) {
            *boxesConcrete = true;
        }
        bool ok = true;
        // CIDER-BUG-128: every signature below is compared by its CANONICAL
        // identity — each spelling resolved in the module that declared it
        // (the interface method's for the interface side, the implementing
        // method's for the class side), never by raw equality. Same-spelled
        // types from two modules are then different types, and one type
        // spelled bare, dotted or through an alias is the same type.
        std::string ifaceMod = definingModuleOf(ifaceSym);
        std::string valueMod = definingModuleOf(concreteSym);
        for (auto& im : iface->methods) {
            std::string imMod = methodModuleOf(im, ifaceMod);
            std::vector<TypeInfo> imParams;
            imParams.reserve(im.parameterTypes.size());
            for (const auto& pt : im.parameterTypes) {
                imParams.push_back(canonicalSignatureType(pt, imMod));
            }
            TypeInfo imRet = canonicalSignatureType(im.returnType, imMod);
            bool found = false;
            for (auto& sm : ss->methods) {
                if (sm.name != im.name) continue;
                if (sm.parameterTypes.size() != imParams.size()) continue;
                std::string smMod = methodModuleOf(sm, valueMod);
                bool paramsMatch = true;
                for (size_t pi = 0; pi < sm.parameterTypes.size(); ++pi) {
                    if (!(canonicalSignatureType(sm.parameterTypes[pi], smMod) == imParams[pi])) {
                        paramsMatch = false;
                        break;
                    }
                }
                if (!paramsMatch) continue;
                if (!(canonicalSignatureType(sm.returnType, smMod) == imRet)) continue;
                found = true;
                break;
            }
            if (!found) {
                error("Type '" + concreteType.name + "' does not implement interface '" +
                          ifaceType.name + "': missing method '" + im.name + "'", line);
                ok = false;
            }
        }
        return ok;
    }

    void visit(VarDeclStmt& stmt) override {
        TypeInfo declaredType = TypeInfo::fromAST(stmt.getTypeAST());
        // CIDER-BUG-068: initializer type captured for the parameter
        // representation inference recorded after the symbol is defined.
        TypeInfo declInitType;
        bool hasDeclInitType = false;
        // CID-020/021 §1: store the canonical nominal identity so later
        // uses compare by defining module, not by the using file. Done
        // before the interface check so dotted spellings resolve too.
        declaredType = canonicalizeNominal(declaredType, stmt.getLine());
        // Resolve interface type; CIDER-BUG-078: through `type S = Shape`
        // alias chains too, so an alias spelling is the same fat pointer as
        // the direct spelling.
        if (!declaredType.isInterface && declaredType.pointerLevel == 0 &&
            resolveInterfaceSymbol(declaredType)) {
            declaredType.isInterface = true;
        }
        // CIDER-BUG-071: an alias declaration (`type Row = [u8;3]`) stores
        // the alias spelling in declaredType, so isArray is false until the
        // chain is resolved. The initializer rules below decide by shape, so
        // resolve here and use the resolved form wherever isArray picks the
        // rule; resolveType keeps the outer array wrapper (CIDER-BUG-019/052).
        // CIDER-BUG-092: the list- and wide-literal rules below take the
        // same resolved shape, so an alias spelling reaches the element and
        // size checks instead of the generic mismatch.
        TypeInfo declaredArray = resolveType(declaredType);

        if (stmt.getInitializer()) {
            auto* wideInit = dynamic_cast<WideStringExpr*>(stmt.getInitializer());
            if (dynamic_cast<ListExpr*>(stmt.getInitializer()) &&
                isFixedArrayValue(declaredArray)) {
                checkFixedArrayInit(stmt, declaredArray);
            } else if (wideInit && declaredArray.isArray &&
                       declaredArray.pointerLevel == 0 && declaredArray.arraySize > 0) {
                checkWideArrayInit(stmt, declaredArray, *wideInit);
            } else {
                stmt.getInitializer()->accept(*this);
                TypeInfo initType = lastExprType_;
                declInitType = initType;
                hasDeclInitType = true;

                // CIDER-BUG-063: a legacy closure-typed declaration
                // (`func(...)`/`function(...)`, direct or through a `type`
                // alias) initialized with a named function stores the raw
                // code pointer. Accept the same-signature conversion and
                // refine the declared type to the plain representation, so
                // later uses take the fn(...) paths and a raw pointer can
                // never be fed to a closure call.
                TypeInfo declaredFn = resolveFunctionPointerAlias(declaredType);
                TypeInfo initFn = resolveFunctionPointerAlias(initType);
                bool closureFromPlainFn =
                    !inClassFieldDecl_ &&
                    declaredFn.isFunctionPointer && declaredFn.isClosure && !declaredFn.isArray &&
                    initFn.isFunctionPointer && !initFn.isClosure && !initFn.isArray &&
                    declaredFn.signatureEquals(initFn);
                // CIDER-BUG-068: the mirror direction. A `fn(...)`-typed
                // declaration (local, global assignment, or through a `type`
                // alias) initialized with a closure object adopts the
                // closure representation, exactly like writing the legacy
                // `func(...)` spelling; the signature (and ABI) must match
                // exactly. Class fields keep their existing rejection (their
                // constant-init rule cannot materialise a closure object).
                bool fnFromClosure =
                    !inClassFieldDecl_ &&
                    declaredFn.isFunctionPointer && !declaredFn.isClosure && !declaredFn.isArray &&
                    initFn.isFunctionPointer && initFn.isClosure && !initFn.isArray &&
                    declaredFn.signatureEquals(initFn);

                if (declaredType.isInterface) {
                    // CIDER-BUG-054: the conversion rule lives in
                    // checkInterfaceValue so declaration and reassignment
                    // cannot drift.
                    checkInterfaceValue(declaredType, initType, stmt.getLine());
                } else if (initType.isSlice && initType.pointerLevel == 0 &&
                           declaredArray.isArray && declaredArray.pointerLevel == 0 &&
                           symbolTable_.scopeDepth() > 0) {
                    // CIDER-BUG-057: `var s: [T; N] = base[start:end]` copies
                    // the slice's elements. Locals only: globals have no
                    // runtime initializer and keep the type-mismatch
                    // rejection, so a slice cannot silently BSS-zero them.
                    // CIDER-BUG-071: declaredArray also covers the alias
                    // spelling (`var s: Row = a[1:4]`).
                    checkSliceArrayCopy("declaration of '" + stmt.getName() + "'",
                                        declaredArray, initType,
                                        dynamic_cast<SliceExpr*>(stmt.getInitializer()),
                                        stmt.getLine());
                    lastExprType_ = declaredType;
                } else if (!closureFromPlainFn && !fnFromClosure &&
                           !areTypesCompatible(declaredType, initType)) {
                    // CIDER-BUG-092: an array-shaped alias reports its
                    // resolved shape (direct-array spellings already print
                    // it); non-array aliases keep their declared spelling
                    // in diagnostics.
                    TypeInfo diagDeclared = declaredArray.isArray ? declaredArray : declaredType;
                    error("Type mismatch in declaration of '" + stmt.getName() +
                              "'. Expected " + canonicalizeNominal(diagDeclared).toString() + ", got " +
                              canonicalizeNominal(initType).toString(),
                          stmt.getLine());
                }
                if (closureFromPlainFn) {
                    // Keep the declared signature; store the plain value
                    // representation (the checker defines the symbol below).
                    declaredType = declaredFn;
                    declaredType.isClosure = false;
                }
                if (fnFromClosure) {
                    // Keep the declared signature; store the closure-object
                    // representation so later calls take the closure path.
                    declaredType = declaredFn;
                    declaredType.isClosure = true;
                }
                // CIDER-BUG-140: a `*void` initializer converted into a
                // closure-typed declaration is a cast-free conversion and
                // must prove the closure object, exactly like
                // `v as func(...)`. Class fields keep their constant-init
                // rule instead.
                if (!inClassFieldDecl_ && isVoidCarrier(declInitType)) {
                    fnCheckClosureConversion(declaredType, stmt.getInitializer(), declInitType,
                                             stmt.getLine());
                }
            }
        }

        // CIDER-BUG-049: class field initializers have no runtime
        // initializer pass — ir_gen materialises them as constant stores at
        // each object creation site (new C, value-class locals). Mirror
        // CIDER-BUG-048's global-scalar policy: accept exactly the shapes the
        // constant evaluator folds (literals, const references, arithmetic)
        // and report the rest instead of silently leaving the field zero.
        if (inClassFieldDecl_ && stmt.getInitializer()) {
            TypeInfo resolvedField = resolveType(declaredType);
            TypeInfo resolvedFn = resolveFunctionPointerAlias(declaredType);
            Expression* fieldInit = stmt.getInitializer();
            bool stringLiteral = dynamic_cast<StringExpr*>(fieldInit) != nullptr;
            if (resolvedFn.isFunctionPointer && !resolvedFn.isArray) {
                if (!isConstantFunctionPointerInit(fieldInit)) {
                    error("Class function pointer field initializer in declaration of '" +
                              stmt.getName() + "' must be a function name or nilPtr",
                          stmt.getLine());
                }
            } else if (resolvedField.pointerLevel > 0 || resolvedField.name == "string") {
                if (!isConstantFunctionPointerInit(fieldInit) && !stringLiteral) {
                    error("Class pointer field initializer in declaration of '" + stmt.getName() +
                              "' must be nilPtr, a string literal, or a function name",
                          stmt.getLine());
                }
            } else if (!resolvedField.isArray && isFloatName(resolvedField.name)) {
                if (!isConstFloatExpr(fieldInit)) {
                    error("Class field initializer in declaration of '" + stmt.getName() +
                              "' must use constant expressions", stmt.getLine());
                }
            } else if (isIntegerLikeType(resolvedField)) {
                if (!isConstIntExpr(fieldInit)) {
                    error("Class field initializer in declaration of '" + stmt.getName() +
                              "' must use constant expressions", stmt.getLine());
                }
            } else {
                // Aggregate (array/struct/class/slice/interface) fields have
                // no constant materialisation at all.
                error("Class field initializer in declaration of '" + stmt.getName() +
                          "' must be a scalar constant expression", stmt.getLine());
            }
        }

        if (inClassFieldDecl_) return;  // see visit(ClassDeclStmt)

        // CID-003 follow-up (W5/W6): float-typed globals have no runtime
        // initializer; a non-constant initializer silently reads 0.0 on
        // every backend (the IR folds only constant shapes). Mirror the
        // global-array rule. Module scopes count as global (scopeDepth 0).
        // Resolve the declared type through the symbol table first, as
        // ir_gen's mapType does, so an alias (`type Real = f64`) cannot
        // bypass the check and BSS-zero the slot.
        if (stmt.getInitializer() && symbolTable_.scopeDepth() == 0 &&
            declaredType.pointerLevel == 0 && !declaredType.isArray) {
            TypeInfo resolvedType = resolveType(declaredType);
            if (resolvedType.pointerLevel == 0 && !resolvedType.isArray &&
                isFloatName(resolvedType.name) &&
                !isConstFloatExpr(stmt.getInitializer())) {
                error("Global float initializer in declaration of '" + stmt.getName() +
                          "' must use constant expressions", stmt.getLine());
            }
        }

        // CID-006: global function pointers have no runtime initializer
        // either. Named functions (a .data cell holding the label) and
        // nilPtr (zero) are the only supported shapes. CID-006 fix (F1):
        // resolve `type Cb = fn(...)` aliases so an alias spelling cannot
        // bypass the rule and silently BSS-zero the slot.
        TypeInfo resolvedFnDecl = resolveFunctionPointerAlias(declaredType);
        if (stmt.getInitializer() && symbolTable_.scopeDepth() == 0 &&
            resolvedFnDecl.isFunctionPointer && !resolvedFnDecl.isArray &&
            !isConstantFunctionPointerInit(stmt.getInitializer())) {
            error("Global function pointer initializer in declaration of '" + stmt.getName() +
                      "' must be a function name or nilPtr",
                  stmt.getLine());
        }

        // CIDER-BUG-048: integer scalars have no runtime initializer either.
        // `var g: int = get();` and `var a: int = b + 1;` (b a non-const
        // global) used to type-check and silently read 0 from .bss on every
        // backend. Only shapes ir_gen's tryEvalConstU64 folds are accepted —
        // literals, constant expressions and `const` references resolved
        // recursively — so an accepted declaration always lands in .data.
        if (stmt.getInitializer() && !stmt.isExtern() && symbolTable_.scopeDepth() == 0 &&
            declaredType.pointerLevel == 0 && !declaredType.isArray) {
            // CIDER-FEAT-002: enum-typed globals take the same route.
            TypeInfo resolvedType = resolveType(declaredType);
            if (isIntegerLikeType(resolvedType) && !isConstIntExpr(stmt.getInitializer())) {
                error("Global integer initializer in declaration of '" + stmt.getName() +
                          "' must use constant expressions", stmt.getLine());
            }
        }

        // CIDER-BUG-048: pointer and string scalars share the problem —
        // without a runtime initializer ir_gen can only materialise nilPtr
        // (zero), a string/wide-string literal (the early string paths) or
        // a function-name shape (a .data cell holding the label). Addresses
        // (`&x`) and calls used to silently BSS-zero; they are rejected
        // now. Function-pointer spellings have their own, stricter gate.
        if (stmt.getInitializer() && !stmt.isExtern() && symbolTable_.scopeDepth() == 0 &&
            !declaredType.isArray) {
            TypeInfo resolvedPtr = resolveType(declaredType);
            bool scalarPtr = !resolvedPtr.isArray &&
                             (resolvedPtr.pointerLevel > 0 || resolvedPtr.name == "string");
            Expression* init = stmt.getInitializer();
            if (scalarPtr && !resolvedFnDecl.isFunctionPointer &&
                !isConstantFunctionPointerInit(init) &&
                !dynamic_cast<StringExpr*>(init) &&
                !dynamic_cast<WideStringExpr*>(init)) {
                error("Global pointer initializer in declaration of '" + stmt.getName() +
                          "' must be nilPtr, a string literal, or a function name",
                      stmt.getLine());
            }
        }

        auto sym = std::make_shared<Symbol>();
        sym->kind = SymbolKind::VARIABLE;
        sym->name = stmt.getName();
        sym->type = declaredType;
        sym->line = stmt.getLine();
        sym->col = stmt.getColumn();
        sym->isConst = stmt.isConst();
        // CIDER-BUG-048: checker-created symbols replace the collector's in
        // the shared scope; carry the const initializer so later constant
        // folding (and uses checked after this declaration) sees it, and
        // keep the defining-module provenance so folding a reference still
        // resolves the initializer in its own module.
        if (sym->isConst) sym->constInit = stmt.getInitializer();
        if (auto prev = symbolTable_.getCurrentScope()->resolveLocal(sym->name)) {
            // CIDER-FEAT-006: the replacement must keep the collector's
            // module-visibility flags — the meta emitter reads them after
            // checking to decide which declarations are exported.
            sym->isExported = prev->isExported;
            sym->isExtern = prev->isExtern;
            auto pit = symbolModules_.find(prev->id);
            if (pit != symbolModules_.end()) symbolModules_[sym->id] = pit->second;
        }
        symbolTable_.define(sym);
        // CIDER-BUG-068: feed the fn/closure representation inference with
        // this declaration (locals, globals and class fields alike; class
        // fields were rejected above, and extern storage keeps its external
        // raw-pointer shape). The slot follows the symbol identity, so later
        // uses and assignments unify with it. When the declaration has no
        // representation-bearing initializer (none, nilPtr, or an unchecked
        // shape), remember its TypeAST: if a later assignment from a
        // closure-valued parameter/local resolves the slot to closure,
        // ir_gen cannot re-derive that from the initializer, so the declared
        // type is annotated instead.
        // CIDER-BUG-118: *void declarations get a provenance-only slot
        // (their untyped storage fixes nothing) so casts back to a closure
        // spelling can be proven or rejected.
        if (!inClassFieldDecl_ && !stmt.isExtern()) {
            TypeInfo slotType = resolveFunctionPointerAlias(declaredType);
            bool fnStorage = slotType.isFunctionPointer && !slotType.isArray;
            bool voidStorage = isVoidCarrier(declaredType);
            if (fnStorage || voidStorage) {
                int slot = fnStorage
                               ? fnSlotForSymbol(sym, "variable '" + sym->name + "'", stmt.getLine())
                               : fnSlotForVoidSymbol(sym, "variable '" + sym->name + "'",
                                                     stmt.getLine());
                bool viaVoid = false;
                bool recorded = hasDeclInitType &&
                                fnRecordExpr(slot, stmt.getInitializer(), declInitType,
                                             stmt.getLine(), &viaVoid);
                if (fnStorage && (!recorded || viaVoid)) fnVarAst_[fnSlotKey_[slot]] = stmt.getTypeAST();
            }
        }
        if (warningsActive_) {
            // CIDER-FEAT-005: only function-local declarations are unused-
            // warned; globals are recorded so locals shadowing them warn.
            bool isLocal = symbolTable_.scopeDepth() > 0;
            warnings_.declare(symbolTable_.getCurrentScope(), sym, stmt.getLine(),
                              stmt.getColumn(), isLocal && !stmt.isExtern(), isLocal);
            // CIDER-ISS-002: a local without an initializer starts
            // unassigned; reads before the first assignment warn. Externs
            // are backed by storage the checker cannot see (like globals),
            // and value-class locals run the class's field initializers /
            // zero-default stores at the declaration site (CIDER-BUG-049).
            if (isLocal && !stmt.isExtern() && !stmt.getInitializer() &&
                !isValueClassType(declaredType)) {
                warnings_.markUninitialized(sym);
            }
            // CIDER-BUG-115: a function-local declaration is current-frame
            // storage; a local pointer initialized with `&local` (or a copy
            // of one) stays provably frame-local until it is reassigned.
            if (isLocal) {
                frameLocalSymbols_.insert(sym->id);
                if (resolveType(declaredType).pointerLevel > 0)
                    recordFrameLocalPointer(sym, stmt.getInitializer());
            }
        }
    }

    void visit(AssignStmt& stmt) override {
        // CIDER-FEAT-005: `x = v` writes x without reading it, so it must
        // not clear the unused-variable warning. Compound assignments
        // (`x += v`) read the target and are not suppressed.
        bool prevSuppressRead = suppressIdentRead_;
        if (stmt.getOp().empty() && dynamic_cast<IdentifierExpr*>(stmt.getTarget())) {
            suppressIdentRead_ = true;
        }
        // CIDER-ISS-002: a store through an element/member chain
        // (`arr[i] = v`, `s.f = v`) initializes that storage lexically.
        // Mark the storage base before the target walk so its base read is
        // not reported (conservative: any element/member store counts for
        // the whole variable). A bare identifier target keeps the ordering
        // rule below (`x = x + 1` must still read x), and a pointer indexed
        // target (`p[i] = v`) genuinely reads p, so it is left to the walk.
        if (warningsActive_) {
            bool indexed = false;
            if (const std::string* base = addressBaseName(stmt.getTarget(), indexed)) {
                auto baseSym = resolveScopedRaw(*base);
                bool pointerIndex = indexed && indexesPointerValue(baseSym);
                if (!dynamic_cast<IdentifierExpr*>(stmt.getTarget()) && !pointerIndex) {
                    warnings_.markAssigned(baseSym);
                }
            }
        }
        stmt.getTarget()->accept(*this);
        suppressIdentRead_ = prevSuppressRead;
        TypeInfo targetType = lastExprType_;

        auto target = dynamic_cast<IdentifierExpr*>(stmt.getTarget());
        if (target) {
            auto sym = resolveScoped(target->getName());
            if (sym && sym->isConst) {
                error("Cannot assign to constant '" + target->getName() + "'", stmt.getLine());
            }
        }

        // CIDER-BUG-054 / CIDER-BUG-071: the target's interface and array
        // shapes are needed before the value walk so CIDER-BUG-112's
        // list-literal path can run the declaration-time element checks in
        // place of the generic compatibility test. Both are derived from
        // targetType only, exactly as before.
        //
        // CIDER-BUG-054: reassigning an interface variable uses the same
        // conversion rule as its declaration — any satisfying concrete
        // value, or a copy of the same interface. The flag can be missing
        // on spellings collected before lowering (e.g. a global interface
        // variable), so resolve the declared name too; CIDER-BUG-078: an
        // alias spelling resolves through its chain like the direct one.
        TypeInfo ifaceTargetType = targetType;
        if (dynamic_cast<IdentifierExpr*>(stmt.getTarget()) &&
            !ifaceTargetType.isInterface && ifaceTargetType.pointerLevel == 0 &&
            resolveInterfaceSymbol(ifaceTargetType)) {
            ifaceTargetType.isInterface = true;
        }

        // CIDER-BUG-071: an alias-typed target (`type Row = [u8;3]`;
        // `s = base[1:4]`, or a field/member of alias type) carries the
        // alias spelling in targetType; resolve it so the slice-copy and
        // list-literal rules see the array shape, exactly like the direct
        // spelling.
        TypeInfo targetArray = resolveType(targetType);

        // CIDER-BUG-112: a list literal reassigned into a fixed array
        // (`s = [1,2,3]`, direct or alias spelling) takes the same
        // size/element checks as the declaration-time initializer, with
        // each element visited exactly once (the declaration path also
        // skips visit(ListExpr) for array targets). Plain assignments only:
        // `s += [1,2,3]` keeps the existing mismatch rejection.
        TypeInfo valueType;
        auto* listValue = stmt.getOp().empty() ? dynamic_cast<ListExpr*>(stmt.getValue())
                                               : nullptr;
        if (listValue && !ifaceTargetType.isInterface && isFixedArrayValue(targetArray)) {
            std::string where = target ? ("assignment to '" + target->getName() + "'")
                                       : std::string("array assignment");
            checkFixedArrayAssign(where, targetArray, *listValue, stmt.getLine());
            valueType = lastExprType_;
        } else {
            stmt.getValue()->accept(*this);
            valueType = lastExprType_;

            if (ifaceTargetType.isInterface) {
                checkInterfaceValue(ifaceTargetType, valueType, stmt.getLine());
                // CIDER-BUG-115: warn when the store aliases a by-value
                // object that dies with this function into storage that is
                // not provably in the same frame.
                warnDanglingInterfaceStore(stmt.getValue(), stmt.getTarget(), valueType,
                                           stmt.getLine());
            } else if (valueType.isSlice && valueType.pointerLevel == 0 &&
                       targetArray.isArray && targetArray.pointerLevel == 0) {
                // CIDER-BUG-057: `arr = base[start:end]` copies the slice's
                // elements into the existing array, with the same size and
                // element rules as the declaration-time copy above.
                std::string where = target ? ("assignment to '" + target->getName() + "'")
                                           : std::string("array assignment");
                checkSliceArrayCopy(where, targetArray, valueType,
                                    dynamic_cast<SliceExpr*>(stmt.getValue()), stmt.getLine());
            } else if (!stmt.getOp().empty() && dynamic_cast<ListExpr*>(stmt.getValue())) {
                // CIDER-BUG-112: a list literal has no compound-assignment
                // lowering — the element-wise copy rule applies to a plain
                // assignment only — so report that instead of the generic
                // type mismatch below.
                error("A list literal cannot be used with compound assignment '" +
                          stmt.getOp() + "='", stmt.getLine());
            } else if (!areTypesCompatible(targetType, valueType)) {
                error("Type mismatch in assignment. Expected " + canonicalizeNominal(targetType).toString() +
                          ", got " + canonicalizeNominal(valueType).toString(),
                      stmt.getLine());
            } else {
                // CIDER-BUG-068: the assignment is representation-consistent
                // (the compatibility test above rejects closure/plain mixes on
                // one storage), so feed the inference with the value's
                // representation for later argument-passing decisions. A
                // fn-typed field/element/pointee store keeps the plain raw-pointer
                // shape (that storage has no representation refinement), so its
                // site is recorded and a closure value is reported in the
                // post-pass instead of being miscompiled.
                // CIDER-BUG-118: a *void target has no fixed representation;
                // record the value's fn shape as provenance so a later cast
                // back to a closure spelling can be proven or rejected.
                // CIDER-BUG-140: a *void field/element/pointee records on a
                // storage slot shared with its later casts, a fn(...) target
                // receiving a *void value records the provenance (a mixed
                // component is reported), and a closure-typed target takes
                // the cast-free conversion verdict.
                TypeInfo targetFn = resolveFunctionPointerAlias(targetType);
                TypeInfo valueFn = resolveFunctionPointerAlias(valueType);
                bool fnTarget = targetFn.isFunctionPointer && !targetFn.isArray;
                bool fnValue = valueFn.isFunctionPointer && !valueFn.isArray;
                bool fnClosureTarget = fnTarget && targetFn.isClosure;
                bool voidTarget = isVoidCarrier(targetType);
                bool voidValue = isVoidCarrier(valueType);
                // CIDER-BUG-157: a closure-typed target already holds the
                // closure representation, so a direct closure value (or the
                // cast-free *void conversion checked below) needs no store
                // verdict; only a plain fn(...) storage site records the
                // value shape for the post-pass rejection. A plain fn(...)
                // value against a closure target is rejected by the
                // compatibility test above.
                if ((fnTarget && !fnClosureTarget && fnValue) ||
                    (fnTarget && !fnClosureTarget && voidValue) || voidTarget) {
                    if (target) {
                        auto sym = resolveScoped(target->getName());
                        if (sym && sym->kind == SymbolKind::VARIABLE) {
                            int slot = fnTarget
                                           ? fnSlotForSymbol(sym, "variable '" + sym->name + "'",
                                                             stmt.getLine())
                                           : fnSlotForVoidSymbol(sym, "variable '" + sym->name + "'",
                                                                 stmt.getLine());
                            fnRecordExpr(slot, stmt.getValue(), valueType, stmt.getLine());
                        }
                    } else if (voidTarget) {
                        int slot = fnCarrierSlot(stmt.getTarget(), stmt.getLine());
                        if (slot >= 0) {
                            fnRecordExpr(slot, stmt.getValue(), valueType, stmt.getLine());
                        }
                    } else {
                        std::string siteLabel = "a fn(...) storage location";
                        if (auto* mem = dynamic_cast<MemberAccessExpr*>(stmt.getTarget())) {
                            siteLabel = "field '" + mem->getMember() + "'";
                        } else if (dynamic_cast<IndexExpr*>(stmt.getTarget())) {
                            siteLabel = "an array element";
                        } else if (auto* un = dynamic_cast<UnaryExpr*>(stmt.getTarget())) {
                            if (un->getOperator() == "*") siteLabel = "a pointer target";
                        }
                        int slot = fnSlot("s:" + std::to_string(fnStoreCheckCount_++), siteLabel,
                                          stmt.getLine());
                        fnRecordExpr(slot, stmt.getValue(), valueType, stmt.getLine());
                    }
                }
                if (fnClosureTarget && voidValue) {
                    fnCheckClosureConversion(targetType, stmt.getValue(), valueType,
                                             stmt.getLine());
                }
            }
        }

        // CIDER-ISS-002: the target is initialized from here on (simple and
        // compound forms both count). Marked after the value walk so
        // `x = x + 1` on an unassigned x still reports the value read.
        if (warningsActive_ && target) {
            warnings_.markAssigned(resolveScoped(target->getName()));
        }

        // CIDER-BUG-115: keep a local pointer's `&local` provenance in step
        // with its current value. A plain assignment adopts the RHS's
        // provenance (kept for `&local`/copies, dropped for everything
        // else); a compound assignment always drops it.
        if (warningsActive_) {
            if (auto* tid = dynamic_cast<IdentifierExpr*>(stmt.getTarget())) {
                auto tsym = resolveScopedRaw(tid->getName());
                if (tsym && tsym->kind == SymbolKind::VARIABLE &&
                    frameLocalSymbols_.count(tsym->id) > 0) {
                    if (stmt.getOp().empty()) {
                        recordFrameLocalPointer(tsym, stmt.getValue());
                    } else {
                        frameLocalPointers_.erase(tsym->id);
                    }
                }
            }
        }

        // CID-003 follow-up (W5): compound assignments mirror the binary
        // operator rules — `%=` and the bitwise operators have no float
        // lowering, so float-family operands are rejected here instead of
        // reaching codegen (silent divsd) or the VM (runtime throw).
        const std::string& op = stmt.getOp();
        if (!op.empty()) {
            bool targetFloat = isFloatName(targetType.name) && targetType.pointerLevel == 0;
            bool valueFloat = isFloatName(valueType.name) && valueType.pointerLevel == 0;
            if (op == "%" && (targetFloat || valueFloat)) {
                error("Modulo operator '%' requires integer operands", stmt.getLine());
            }
            if ((op == "&" || op == "|" || op == "^") && (targetFloat || valueFloat)) {
                error("Bitwise operator '" + op + "' requires integer operands", stmt.getLine());
            }
            // CIDER-BUG-056: `s += t` is the same unsupported concatenation
            // shape as `s + t`; without this it stored the sum of two
            // addresses into `s`.
            bool targetString = targetType.name == "string" && targetType.pointerLevel == 0;
            bool valueString = valueType.name == "string" && valueType.pointerLevel == 0;
            if (op == "+" && targetString && valueString) {
                error("String concatenation with '+=' is not supported; "
                      "use std.string.strcat into a buffer",
                      stmt.getLine());
            }
            // CIDER-ISS-012: the other compound forms on two `string`
            // values are equally meaningless — the generic path would
            // combine the two addresses. Compound string ± integer
            // (`s += n`, `s -= n`) stays a supported interior-pointer
            // operation, and explicit `*u8`/`char*` targets are untouched.
            if (targetString && valueString && op != "+" &&
                (op == "-" || op == "*" || op == "/" || op == "%" ||
                 op == "&" || op == "|" || op == "^")) {
                error("Operator '" + op + "=' is not supported on two string values; "
                      "use std.string", stmt.getLine());
            }
        }
    }

    void visit(BinaryExpr& expr) override {
        expr.getLeft()->accept(*this);
        TypeInfo leftType = lastExprType_;

        expr.getRight()->accept(*this);
        TypeInfo rightType = lastExprType_;

        auto isInteger = [](const std::string& name) {
            return name == "int" || name == "u8" || name == "i8" || name == "u16" || name == "i16" ||
                   name == "u32" || name == "i32" || name == "u64" || name == "i64" || name == "char" || name == "byte";
        };

        std::string op = expr.getOperator();

        bool isPtrMath = (leftType.pointerLevel > 0 && isInteger(rightType.name)) ||
                         (rightType.pointerLevel > 0 && isInteger(leftType.name) && op == "+");

        // Strings are byte pointers (CID-002): `s + 2` / `s - 2` form an
        // interior pointer exactly like `&s[2]`. Scaling is 1, so IRGen
        // needs no change beyond accepting the types here.
        bool leftIsString = (leftType.name == "string" && leftType.pointerLevel == 0);
        bool rightIsString = (rightType.name == "string" && rightType.pointerLevel == 0);
        bool isStringMath = (leftIsString && isInteger(rightType.name) && (op == "+" || op == "-")) ||
                            (rightIsString && isInteger(leftType.name) && op == "+");

        // CIDER-BUG-056: `+` on two string values has no concatenation
        // lowering — IRGen's generic ADD would add the two addresses and
        // produce garbage that compares unequal to a real "ab" forever.
        // Reject it and steer to the buffer helpers in std.string. String
        // plus/minus an integer (`s + 2`, `1 + s`, `s + 5 - 2`) stays a
        // supported interior-pointer operation, and explicit char*/*u8
        // arithmetic is untouched.
        if (leftIsString && rightIsString && op == "+") {
            error("String concatenation with '+' is not supported; "
                  "use std.string.strcat into a buffer",
                  expr.getLine());
        }

        // CIDER-ISS-012: `<`/`>`/`<=`/`>=` between two `string` values now
        // compare contents (lexicographic, strcmp sign) via the IR-built
        // `_strcmp` helper. The remaining arithmetic/bitwise operators have
        // no string-content lowering — IRGen's generic path would combine
        // the two byte addresses — so reject them here the same way as the
        // 056 concatenation. `s + n` / `s - n` interior-pointer arithmetic
        // is unaffected (an integer is not a string), and explicit
        // `*u8`/`char*` operands never reach this branch.
        bool stringOpUnsupported = (op == "-" || op == "*" || op == "/" || op == "%" ||
                                    op == "&" || op == "|" || op == "^" ||
                                    op == "<<" || op == ">>");
        if (leftIsString && rightIsString && stringOpUnsupported) {
            error("Operator '" + op + "' is not supported on two string values; "
                  "use std.string", expr.getLine());
            // Keep the (invalid) expression typed as string so the enclosing
            // assignment/return checks do not report a second, misleading
            // type-mismatch diagnostic.
            lastExprType_ = leftType;
            return;
        }

        bool isMixedNumeric = (isInteger(leftType.name) && isFloatName(rightType.name)) ||
                              (isFloatName(leftType.name) && isInteger(rightType.name)) ||
                              (isFloatName(leftType.name) && isFloatName(rightType.name));

        // CIDER-FEAT-002: enum operands compare against the same enum or an
        // integer in EITHER position without a cast (`c == Color.GREEN`,
        // `0 != c`); the comparison lowers on the underlying integer.
        // Ordering across two different enums stays rejected. Arithmetic on
        // an enum still requires an explicit `c as int`.
        bool isComparison = (op == "==" || op == "!=" || op == "<" || op == ">" ||
                             op == "<=" || op == ">=");
        bool enumComparable = false;
        if (isComparison) {
            auto leftEnum = resolveEnumSymbol(leftType);
            auto rightEnum = resolveEnumSymbol(rightType);
            bool leftInt = isInteger(leftType.name) && leftType.pointerLevel == 0;
            bool rightInt = isInteger(rightType.name) && rightType.pointerLevel == 0;
            enumComparable = (leftEnum && rightEnum && leftEnum == rightEnum) ||
                             (leftEnum && rightInt) || (rightEnum && leftInt);
        }

        if (!areTypesCompatible(leftType, rightType) && !isPtrMath && !isStringMath &&
            !isMixedNumeric && !enumComparable) {
            error("Type mismatch in binary expression '" + expr.getOperator() +
                      "'. " + leftType.toString() + " vs " + rightType.toString(),
                  expr.getLine());
        }

        // CID-003 review fix: bitwise operators have no float lowering, so
        // reject float-family operands here — before the isMixedNumeric
        // promotion branch below can silently validate them. Both operand
        // positions are checked (`float & int`, `int & f64`, `f64 << f64`).
        bool isBitwise = (op == "|" || op == "&" || op == "^" || op == "<<" || op == ">>");
        if (isBitwise && ((isFloatName(leftType.name) && leftType.pointerLevel == 0) ||
                          (isFloatName(rightType.name) && rightType.pointerLevel == 0))) {
            error("Bitwise operator '" + op + "' requires integer operands", expr.getLine());
        }

        // CID-003 follow-up (M4): `%` has no float lowering either — the VM
        // threw `float op not supported` at runtime and codegen silently
        // emitted `divsd`. Reject float-family operands before the
        // isMixedNumeric promotion branch can validate them, like the
        // bitwise operators above; both operand positions are checked.
        if (op == "%" && ((isFloatName(leftType.name) && leftType.pointerLevel == 0) ||
                          (isFloatName(rightType.name) && rightType.pointerLevel == 0))) {
            error("Modulo operator '" + op + "' requires integer operands", expr.getLine());
        }

        if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=") {
            lastExprType_ = {"bool", 0};
        } else if (isStringMath) {
            lastExprType_ = leftIsString ? leftType : rightType;
        } else if (isPtrMath) {
            lastExprType_ = (leftType.pointerLevel > 0) ? leftType : rightType;
        } else if (isMixedNumeric) {
            lastExprType_ = (leftType.name == "f64" || rightType.name == "f64")
                                ? TypeInfo{"f64", 0} : TypeInfo{"float", 0};
        } else if (op == "|" || op == "&" || op == "^" || op == "<<" || op == ">>") {
            TypeInfo resolved = resolveType(leftType);
            if ((resolved.name != "int" && resolved.name.find('u') != 0 && resolved.name.find('i') != 0) || resolved.pointerLevel > 0) {
                error("Bitwise operator '" + op + "' requires integer operands", expr.getLine());
            }
            lastExprType_ = leftType;
        } else {
            lastExprType_ = leftType;
        }
    }

    // ++/-- stores through its operand, so the operand must name storage:
    // a variable, member, element, or dereference. Without this, a literal
    // parses (`(--0.5)` is prefix `--` on 0.5) and reaches IRGen's address
    // path, which stores through a constant address — the VM aborts with
    // heap corruption and native segfaults. Constants are rejected too
    // (not modifiable, mirroring visit(AssignStmt)). Members, elements and
    // dereferences are accepted by shape; deeper cases (`foo().x++`) are
    // pre-existing and unchanged.
    bool isIncrementTarget(Expression* expr) {
        if (!expr) return false;
        if (auto grp = dynamic_cast<GroupingExpr*>(expr)) {
            return isIncrementTarget(grp->getExpression());
        }
        if (auto id = dynamic_cast<IdentifierExpr*>(expr)) {
            if (id->getName() == "_") return false;
            auto sym = resolveScoped(id->getName());
            return sym && sym->kind == SymbolKind::VARIABLE && !sym->isConst;
        }
        if (dynamic_cast<MemberAccessExpr*>(expr)) return true;
        if (dynamic_cast<IndexExpr*>(expr)) return true;
        if (auto un = dynamic_cast<UnaryExpr*>(expr)) return un->getOperator() == "*";
        return false;
    }

    // CIDER-ISS-002: indexing a pointer value (`p[i]`, `p[i].f`) reads p's
    // value; indexing fixed-array storage does not. Array-of-pointer types
    // carry pointerLevel too, so isArray disambiguates. Slices index their
    // descriptor, which is a value read.
    bool indexesPointerValue(const std::shared_ptr<Symbol>& sym) const {
        return sym && !sym->type.isArray &&
               (sym->type.pointerLevel > 0 || sym->type.isSlice);
    }

    // CIDER-ISS-002: a value-class local runs the class's field
    // initializers and zero-default stores at its declaration site even
    // without an explicit initializer (CIDER-BUG-049), so it is not an
    // "unassigned" local. Interfaces and structs have no such pass.
    bool isValueClassType(const TypeInfo& type) {
        if (type.pointerLevel > 0 || type.isArray || type.isInterface) return false;
        TypeInfo resolved = resolveType(type);  // follow `type X = C` aliases
        if (resolved.pointerLevel > 0 || resolved.isArray || resolved.isInterface) return false;
        auto sym = resolveScopedRaw(resolved.name);
        return sym && sym->kind == SymbolKind::CLASS;
    }

    // CIDER-ISS-002: the identifier whose storage `&operand` denotes (`x`
    // for `&x`, `&x[i]`, `&x.f`). Returns null when the operand is not
    // based on a bare identifier (calls, literals, dereferences), or when
    // the chain crosses `->`, which reads a pointer's value. `indexed`
    // reports whether an `[]` was crossed so the caller can distinguish
    // fixed-array storage from indexing a pointer (which reads its value).
    const std::string* addressBaseName(Expression* expr, bool& indexed) {
        indexed = false;
        for (Expression* cur = expr; cur != nullptr;) {
            if (auto* id = dynamic_cast<IdentifierExpr*>(cur)) return &id->getName();
            if (auto* grp = dynamic_cast<GroupingExpr*>(cur)) {
                cur = grp->getExpression();
                continue;
            }
            if (auto* ma = dynamic_cast<MemberAccessExpr*>(cur)) {
                if (ma->isArrow()) return nullptr;
                cur = ma->getObject();
                continue;
            }
            if (auto* ix = dynamic_cast<IndexExpr*>(cur)) {
                indexed = true;
                cur = ix->getObject();
                continue;
            }
            return nullptr;
        }
        return nullptr;
    }

    // ------------------------------------------------------------------
    // CIDER-BUG-115: interface stores and frame lifetime.
    //
    // An interface built from a concrete struct/class VALUE stores that
    // object's address in the fat pointer (aliasing, like a C++
    // reference), so a by-value object local to the current function
    // leaves the interface dangling once the frame dies. The checker
    // cannot prove whether the interface outlives the function, so it
    // reports the detectable shapes as an opt-in (--warn) may-dangle
    // diagnostic: the source must be provably current-frame storage and
    // the target must NOT be provably current-frame storage. The VM
    // additionally makes frame reuse observable (a chunk claimed by a
    // later frame is zeroed, like native's next call overwriting the dead
    // stack slots), so an undetectable case cannot read the old object
    // back once its frame has been reused.
    // ------------------------------------------------------------------

    // Storage that certainly dies with the current frame: a local/parameter
    // object, a member/element of one, or a pointee reached through a
    // frame-local pointer. Globals, module members, heap objects and
    // parameter pointees are not provable, so they return false.
    bool denotesFrameLocalStorage(Expression* expr) {
        if (!expr) return false;
        if (auto* grp = dynamic_cast<GroupingExpr*>(expr))
            return denotesFrameLocalStorage(grp->getExpression());
        if (auto* id = dynamic_cast<IdentifierExpr*>(expr)) {
            auto sym = resolveScopedRaw(id->getName());
            return sym && sym->kind == SymbolKind::VARIABLE &&
                   frameLocalSymbols_.count(sym->id) > 0;
        }
        if (auto* ma = dynamic_cast<MemberAccessExpr*>(expr)) {
            if (ma->isArrow()) return pointerValueIsFrameLocal(ma->getObject());
            return denotesFrameLocalStorage(ma->getObject());
        }
        if (auto* ix = dynamic_cast<IndexExpr*>(expr)) {
            bool indexed = false;
            const std::string* base = addressBaseName(expr, indexed);
            if (base) {
                auto sym = resolveScopedRaw(*base);
                if (!sym || frameLocalSymbols_.count(sym->id) == 0) return false;
                // Indexing a pointer reads the pointer value: the element
                // storage is frame-local only through `&local` provenance.
                if (indexed && indexesPointerValue(sym))
                    return pointerValueIsFrameLocal(ix->getObject());
                return true;
            }
            // Chain crossed `->` or a dereference: classify at the crossing.
            return denotesFrameLocalStorage(ix->getObject());
        }
        if (auto* un = dynamic_cast<UnaryExpr*>(expr)) {
            if (un->getOperator() == "*") return pointerValueIsFrameLocal(un->getOperand());
        }
        return false;
    }

    // The pointer VALUE `expr` is known to address current-frame storage:
    // `&<frame-local lvalue>` or a local pointer with recorded provenance.
    bool pointerValueIsFrameLocal(Expression* expr) {
        if (!expr) return false;
        if (auto* grp = dynamic_cast<GroupingExpr*>(expr))
            return pointerValueIsFrameLocal(grp->getExpression());
        if (auto* id = dynamic_cast<IdentifierExpr*>(expr)) {
            auto sym = resolveScopedRaw(id->getName());
            return sym && sym->kind == SymbolKind::VARIABLE &&
                   frameLocalPointers_.count(sym->id) > 0;
        }
        if (auto* un = dynamic_cast<UnaryExpr*>(expr)) {
            if (un->getOperator() == "&") return denotesFrameLocalStorage(un->getOperand());
        }
        return false;
    }

    // Adopt/drop the frame-local provenance of a local pointer variable from
    // its new value. `&local`, `&local.field`, `&arr[i]` and copies of an
    // already-proven pointer keep the flag; every other value (a call, a
    // parameter, `new`, a global) drops it. Non-pointer locals are ignored.
    void recordFrameLocalPointer(const std::shared_ptr<Symbol>& sym, Expression* value) {
        if (!sym || sym->kind != SymbolKind::VARIABLE) return;
        if (!frameLocalSymbols_.count(sym->id)) return;
        if (resolveType(sym->type).pointerLevel == 0) {
            frameLocalPointers_.erase(sym->id);
            return;
        }
        if (pointerValueIsFrameLocal(value)) {
            frameLocalPointers_.insert(sym->id);
        } else {
            frameLocalPointers_.erase(sym->id);
        }
    }

    // The opt-in may-dangle diagnostic. Only a concrete by-value source
    // (not a pointer, interface or array) that is provably current-frame
    // storage, stored into a target that is NOT provably current-frame
    // storage, reports: exactly the shapes where the stored data pointer
    // dies with the function while the interface may outlive it. Same-frame
    // stores (local targets, local-object members/elements, proven `&local`
    // pointer targets) stay silent.
    void warnDanglingInterfaceStore(Expression* source, Expression* target,
                                    const TypeInfo& sourceType, int line) {
        if (!warningsActive_ || !source || !target) return;
        if (sourceType.name == "error") return;  // no cascade on a failed source
        if (sourceType.isInterface || sourceType.isArray || sourceType.pointerLevel > 0)
            return;
        if (denotesFrameLocalStorage(source) && !denotesFrameLocalStorage(target)) {
            reporter_.warning(
                "interface store of a by-value object that dies at the end of the "
                "current function; the stored pointer dangles if the interface "
                "outlives it",
                line);
        }
    }

    void visit(UnaryExpr& expr) override {
        // CIDER-ISS-002: `&x` (`&x[i]`, `&x.f`) does not read x's value.
        // Mark the storage base address-taken BEFORE the operand walk so
        // the walk does not report a read-before-assignment for it. A
        // pointer indexed under `&` (`&p[i]` is p + i) does read p's value,
        // so it is left to the normal walk, as is any `*p` base.
        if (warningsActive_ && expr.getOperator() == "&") {
            bool indexed = false;
            if (const std::string* base = addressBaseName(expr.getOperand(), indexed)) {
                auto baseSym = resolveScopedRaw(*base);
                if (!(indexed && indexesPointerValue(baseSym))) {
                    warnings_.markAddressTaken(baseSym);
                    // CIDER-BUG-115: an address-taken local pointer may be
                    // repointed through the taken address, so its `&local`
                    // provenance stops being provable.
                    if (!indexed && baseSym && baseSym->kind == SymbolKind::VARIABLE &&
                        resolveType(baseSym->type).pointerLevel > 0) {
                        frameLocalPointers_.erase(baseSym->id);
                    }
                }
            }
        }
        expr.getOperand()->accept(*this);
        TypeInfo operandType = lastExprType_;

        if (expr.getOperator() == "&") {
            lastExprType_ = operandType;
            lastExprType_.pointerLevel++;
        } else if (expr.getOperator() == "*") {
            if (operandType.pointerLevel == 0) {
                error("Cannot dereference non-pointer type " + operandType.toString(), expr.getLine());
            } else {
                lastExprType_ = operandType;
                lastExprType_.pointerLevel--;
                // CIDER-BUG-100: `*p` through a pointer-to-interface names
                // the interface value itself (a 16-byte fat pointer), so
                // recover the flag a declaration spelling (`*Shape`) cannot
                // carry, exactly like visit(IndexExpr) does for `p[i]`.
                // Without this the deref target classified as a plain
                // nominal, so `*p = c` and `*p = <interface>` were rejected
                // as shape mismatches instead of taking the interface store
                // rule.
                resolveInterfaceFlag(lastExprType_);
            }
        } else if (expr.getOperator() == "!" || expr.getOperator() == "not") {
            // CIDER-BUG-046: `not` is the word spelling of `!`; same bool
            // operand requirement and bool result.
            if (resolveType(operandType).name != "bool" || operandType.pointerLevel > 0) {
                error("Expected bool for '" + expr.getOperator() + "' operator, got " + operandType.toString(), expr.getLine());
            }
            lastExprType_ = {"bool", 0};
        } else if (expr.getOperator() == "~") {
            TypeInfo resolved = resolveType(operandType);
            if ((resolved.name != "int" && resolved.name.find('u') != 0 && resolved.name.find('i') != 0) || operandType.pointerLevel > 0) {
                error("Expected int for '~' operator, got " + operandType.toString(), expr.getLine());
            }
            lastExprType_ = operandType;
        } else if (expr.getOperator() == "-") {
            if (operandType.pointerLevel > 0) {
                error("Cannot apply '-' to pointer type " + operandType.toString(), expr.getLine());
            }
            lastExprType_ = operandType;
        } else if (expr.getOperator() == "++" || expr.getOperator() == "--") {
            if (operandType.pointerLevel > 0) {
                error("Cannot " + expr.getOperator() + " a pointer type", expr.getLine());
            }
            if (!isIncrementTarget(expr.getOperand())) {
                error("Operand of '" + expr.getOperator() +
                          "' must be a variable, member, element, or dereference",
                      expr.getLine());
            }
            // CIDER-ISS-002: `x++` stores back, so x counts as assigned
            // after the operand's read (already reported above when x had
            // never been assigned).
            if (warningsActive_) {
                Expression* inc = expr.getOperand();
                while (auto* grp = dynamic_cast<GroupingExpr*>(inc)) inc = grp->getExpression();
                if (auto* id = dynamic_cast<IdentifierExpr*>(inc)) {
                    warnings_.markAssigned(resolveScopedRaw(id->getName()));
                }
            }
            lastExprType_ = operandType;
        }
    }

    void visit(IdentifierExpr& expr) override {
        if (expr.getName() == "_") {
            lastExprType_ = {"wildcard", 0};
            return;
        }
        auto sym = resolveScoped(expr.getName());
        // CIDER-FEAT-005: a bare assignment target is a write; everything
        // else that reaches an identifier is a read. CIDER-ISS-002: the
        // read position locates a read-before-assignment warning.
        if (warningsActive_ && !suppressIdentRead_) {
            warnings_.markRead(sym, expr.getLine(), expr.getColumn());
        }
        if (!sym) {
            // Inside a method, a name matching one of the class's fields is
            // almost always a missing `this.` — point at it instead of the
            // generic undefined error.
            std::string fieldOwner;
            if (!currentClassName_.empty()) {
                auto classSym = resolveScoped(currentClassName_);
                if (classSym && classSym->kind == SymbolKind::CLASS) {
                    auto classStruct = std::static_pointer_cast<StructSymbol>(classSym);
                    for (const auto& field : classStruct->fields) {
                        if (field.name == expr.getName()) {
                            fieldOwner = currentClassName_;
                            break;
                        }
                    }
                }
            }
            if (!fieldOwner.empty()) {
                error("Field '" + expr.getName() + "' of class '" + fieldOwner +
                          "' must be accessed through 'this'", expr.getLine());
            } else {
                error("Undefined identifier '" + expr.getName() + "'", expr.getLine());
            }
            lastExprType_ = {"error", 0};
        } else if (sym->kind == SymbolKind::MODULE) {
            // A bare module qualifier is not a value (spec §3). Values
            // shadow qualifiers via strict lookup, so reaching here means no
            // shadowing value exists in scope.
            if (!reportAmbiguousQualifier(expr.getName(), expr.getLine())) {
                error("'" + expr.getName() + "' is a module, not a value", expr.getLine());
            }
            lastExprType_ = {"error", 0};
        } else if (sym->kind == SymbolKind::ENUM) {
            // CIDER-FEAT-002: an enum name is a type, not a value; qualified
            // variant access (`Color.RED`) is intercepted by
            // visit(MemberAccessExpr) before this path runs.
            error("'" + expr.getName() + "' is a type, not a value", expr.getLine());
            lastExprType_ = {"error", 0};
        } else {
            if (sym->kind == SymbolKind::FUNCTION) {
                // CID-006: a bare function name is a value of its fn
                // signature type; it stays assignable to *void through
                // areTypesCompatible, so the legacy untyped idiom works.
                lastExprType_ = functionPointerTypeOf(
                    std::static_pointer_cast<FunctionSymbol>(sym), expr.getLine());
            } else {
                lastExprType_ = sym->type;
                // CID-020/021 §1: a symbol bound in another module (a
                // module-scope variable read via from-alias) carries that
                // module's bare nominals; re-root them in the owner's
                // context so comparisons use canonical identity.
                std::string ownerMod = definingModuleOf(sym);
                if (ownerMod != currentModule_) {
                    lastExprType_ = canonicalizeInModule(sym->type, ownerMod, expr.getLine());
                }
                // CID-006 fix (F1): an alias-typed value must present its
                // underlying fn signature to the call/global rules.
                lastExprType_ = resolveFunctionPointerAlias(lastExprType_);
                // Resolve interface type; CIDER-BUG-078: alias spellings
                // (`type S = Shape`) resolve through their chain.
                if (!lastExprType_.isInterface && lastExprType_.pointerLevel == 0 &&
                    resolveInterfaceSymbol(lastExprType_)) {
                    lastExprType_.isInterface = true;
                }
            }
        }
    }

    void visit(NumberExpr& expr) override {
        if (expr.isFloat()) {
            lastExprType_ = {"float", 0};
        } else {
            lastExprType_ = {"int", 0};
        }
    }

    void visit(CharExpr&) override { lastExprType_ = {"char", 0}; }
    void visit(StringExpr&) override { lastExprType_ = {"string", 0}; }

    // CID-005: `u"AB"` is `[u16; 3]` — the AST value excludes the NUL the
    // type (and the emitted data) include.
    void visit(WideStringExpr& expr) override {
        TypeInfo t;
        t.name = "u16";
        t.isArray = true;
        t.arraySize = (int)expr.getValue().size() + 1;
        lastExprType_ = t;
    }
    void visit(WideCharExpr&) override { lastExprType_ = {"u16", 0}; }

    void visit(InterpolatedStringExpr& expr) override {
        for (const auto& part : expr.getParts()) {
            if (!part.isExpr) continue;
            part.expr->accept(*this);
            const std::string& t = lastExprType_.name;
            if (t == "int" || t == "bool" || t == "string" || t == "char") continue;
            if (t == "float" || t == "f64") {
                error("Interpolation of float is not supported yet; cast to int first.", expr.getLine());
            } else {
                error("Interpolation requires int, bool, char or string, got " + t, expr.getLine());
            }
        }
        lastExprType_ = {"string", 0};
    }
    void visit(BooleanExpr&) override { lastExprType_ = {"bool", 0}; }
    void visit(NilExpr&) override { lastExprType_ = {"nil", 0}; }
    void visit(NilPtrExpr&) override { lastExprType_ = {"void", 1}; }

    // CIDER-FEAT-003: a class without its own constructor reached from
    // `new C(...)`. If an ancestor declares one, explain why it cannot serve
    // as C's constructor: constructors are not INHERITED, so `new` still
    // requires an own `func init(...)`. CIDER-BUG-123: a base constructor
    // declaring no parameters now runs as part of the chain (before the
    // derived one) — it initializes the base subobject, it is not this
    // class's constructor, and it cannot take the arguments written at `new`.
    std::string missingConstructorHint(const std::shared_ptr<StructSymbol>& classSym) {
        std::string parent = classSym->parentName;
        while (!parent.empty()) {
            // CIDER-BUG-124: dotted/alias parent spellings resolve like any
            // other type position so the ancestor walk keeps working.
            auto parentSym = resolveParentClass(parent);
            if (!parentSym || parentSym->kind != SymbolKind::CLASS) return "";
            auto ps = std::static_pointer_cast<StructSymbol>(parentSym);
            if (!ps->constructorName.empty()) {
                return " (base-class constructors are not inherited; "
                       "declare 'func init(...)' in '" + classSym->name + "')";
            }
            parent = ps->parentName;
        }
        return "";
    }

    void visit(NewExpr& expr) override {
        TypeInfo type = canonicalizeNominal(TypeInfo::fromAST(expr.getNewType()), expr.getLine());
        // CIDER-FEAT-003: resolve the constructed class alias-aware, exactly
        // like the IR generator, so a constructor is never validated here
        // and then silently skipped through a `type A = C` spelling.
        std::shared_ptr<StructSymbol> classSym = nullptr;
        if (type.pointerLevel == 0) {
            auto sym = resolveStructOrClass(type);
            if (sym && sym->kind == SymbolKind::CLASS) {
                classSym = std::static_pointer_cast<StructSymbol>(sym);
            }
        }
        bool hasCtor = classSym && !classSym->constructorName.empty();
        if (expr.hasArgList()) {
            if (!classSym) {
                error("'new " + type.toString() +
                          "(...)' requires a class type with a constructor",
                      expr.getLine());
            } else if (!hasCtor) {
                error("Class '" + type.name + "' has no constructor" +
                          missingConstructorHint(classSym),
                      expr.getLine());
            } else if (expr.getArgs().size() != classSym->constructorParamTypes.size()) {
                error("Constructor of class '" + type.name + "' expects " +
                          std::to_string(classSym->constructorParamTypes.size()) +
                          " arguments, got " + std::to_string(expr.getArgs().size()),
                      expr.getLine());
            } else {
                // Same argument rules as a method call whose first parameter
                // is the hidden `this` (arity, then per-argument
                // compatibility with the callee's canonical parameter types).
                std::string defMod = definingModuleOf(classSym);
                for (size_t i = 0; i < expr.getArgs().size(); ++i) {
                    TypeInfo paramT = canonicalizeInModule(
                        classSym->constructorParamTypes[i], defMod, expr.getLine());
                    // CIDER-BUG-080: an interface-typed constructor
                    // parameter accepts a satisfying concrete value or the
                    // same interface, like every other interface position.
                    resolveInterfaceFlag(paramT);
                    // CIDER-BUG-165: a list literal passed to a fixed-array
                    // constructor parameter takes the same declaration-time
                    // checks as every other call argument (the IR generator
                    // materialises the declared shape).
                    TypeInfo paramArray = resolveType(paramT);
                    if (auto* list = dynamic_cast<ListExpr*>(expr.getArgs()[i].get());
                        list && !paramT.isInterface && isFixedArrayValue(paramArray)) {
                        checkFixedArrayElements(
                            "argument " + std::to_string(i + 1) +
                                " to constructor of class '" + type.name + "'",
                            paramArray, *list, expr.getLine());
                        continue;
                    }
                    expr.getArgs()[i]->accept(*this);
                    if (paramT.isInterface) {
                        if (lastExprType_.name != "error") {
                            checkInterfaceValue(paramT, lastExprType_, expr.getLine());
                        }
                    } else if (!areTypesCompatible(paramT, lastExprType_)) {
                        error("Argument " + std::to_string(i + 1) +
                                  " to constructor of class '" + type.name +
                                  "' type mismatch. Expected " + paramT.toString() +
                                  ", got " + canonicalizeNominal(lastExprType_).toString(),
                              expr.getLine());
                    }
                }
            }
        } else if (hasCtor && !classSym->constructorParamTypes.empty()) {
            // A constructor with parameters called without them (`new C`).
            error("Constructor of class '" + type.name + "' expects " +
                      std::to_string(classSym->constructorParamTypes.size()) +
                      " arguments, got 0",
                  expr.getLine());
        }
        type.pointerLevel++;  // new T returns T*
        lastExprType_ = type;
    }

    void visit(MemberAccessExpr& expr) override {
        // CIDER-FEAT-002: `Enum.VARIANT` and `lib.Enum.VARIANT`. The enum
        // name is a type qualifier, not a value, so it is resolved directly
        // (never evaluated through the expression paths). The variant's type
        // is the enum itself.
        if (!expr.isArrow()) {
            if (auto enumSym = enumSymbolFromQualifier(expr.getObject())) {
                int64_t variantValue = 0;
                if (enumSym->findVariant(expr.getMember(), variantValue)) {
                    lastExprType_ = enumTypeOf(enumSym);
                } else {
                    error("Enum '" + enumSym->name + "' has no variant '" +
                              expr.getMember() + "'", expr.getLine());
                    lastExprType_ = {"error", 0};
                }
                return;
            }
        }
        // CID-020/021 §3: `b.name` where b is a module qualifier resolves
        // the member in that module's scope. Values shadow qualifiers
        // (strict lookup finds the nearer value first), so a MODULE result
        // here means no shadowing. Arrow access never qualifies.
        if (!expr.isArrow()) {
            if (auto base = dynamic_cast<IdentifierExpr*>(expr.getObject())) {
                auto baseSym = resolveScoped(base->getName());
                if (baseSym && baseSym->kind == SymbolKind::MODULE) {
                    visitModuleMemberAccess(expr, base->getName(),
                                            std::static_pointer_cast<ModuleSymbol>(baseSym));
                    return;
                }
            }
        }
        expr.getObject()->accept(*this);
        TypeInfo objType = lastExprType_;

        if (expr.isArrow()) {
            if (objType.pointerLevel == 0) {
                error("Cannot use '->' on non-pointer type " + objType.toString(), expr.getLine());
                lastExprType_ = {"error", 0};
                return;
            }
            objType.pointerLevel--;
        }

        // Check if object type is an interface; CIDER-BUG-078: an alias
        // spelling (`type S = Shape`) resolves through its chain.
        auto ifaceSym = resolveInterfaceSymbol(objType);
        if (ifaceSym) {
            auto iface = std::static_pointer_cast<InterfaceSymbol>(ifaceSym);
            for (const auto& method : iface->methods) {
                if (method.name == expr.getMember()) {
                    lastExprType_ = canonicalizeInModule(
                        method.returnType, methodModuleOf(method, definingModuleOf(ifaceSym)),
                        expr.getLine());
                    // CIDER-BUG-077: an interface method returning an
                    // interface yields the fat pointer (caller sret slot).
                    resolveInterfaceFlag(lastExprType_);
                    return;
                }
            }
            error("Method '" + expr.getMember() + "' not found in interface " + objType.name, expr.getLine());
            lastExprType_ = {"error", 0};
            return;
        }

        // CIDER-BUG-015: `type X = T` providers are STRUCT-kind plain
        // Symbols; resolve the alias chain before the StructSymbol cast
        // (alias-to-struct member access used to be undefined behavior).
        auto sym = resolveStructOrClass(objType);
        if (!sym) {
            error("Type " + objType.toString() + " is not a struct or class", expr.getLine());
            lastExprType_ = {"error", 0};
            return;
        }

        auto structSym = std::static_pointer_cast<StructSymbol>(sym);
        bool found = false;
        for (const auto& field : structSym->fields) {
            if (field.name == expr.getMember()) {
                // CID-020/021 §1: field nominals belong to the struct's
                // owner module; canonicalize there, not at the use site.
                // CIDER-BUG-090: an inherited field's spelling belongs to
                // the class that declared it, not to the derived class's
                // module (which may not see the field's type at all).
                lastExprType_ = canonicalizeInModule(field.type, field.typeModule, expr.getLine());
                // CIDER-BUG-069: an interface-typed field is a fat pointer;
                // resolve the flag by name as a fallback (the collector sets
                // it for module-local declarations).
                resolveInterfaceFlag(lastExprType_);
                found = true;
                break;
            }
        }

        if (!found) {
            for (const auto& method : structSym->methods) {
                if (method.name == expr.getMember()) {
                    // CIDER-BUG-090: an inherited method's return spelling
                    // belongs to the class that declared it.
                    lastExprType_ = canonicalizeInModule(
                        method.returnType, methodModuleOf(method, definingModuleOf(sym)),
                        expr.getLine());
                    // CIDER-BUG-077: class methods may return interfaces.
                    resolveInterfaceFlag(lastExprType_);
                    found = true;
                    break;
                }
            }
        }

        if (!found) {
            error("Member '" + expr.getMember() + "' not found in " + objType.name, expr.getLine());
            lastExprType_ = {"error", 0};
        }
    }

    // Resolve `qualifier.member` inside the qualifier's module scope (spec
    // §3). Members are looked up locally: falling through to the module
    // scope's parent would leak the importer's symbols (the T2 advisory).
    void visitModuleMemberAccess(MemberAccessExpr& expr, const std::string& qualifier,
                                 const std::shared_ptr<ModuleSymbol>& modSym) {
        if (reportAmbiguousQualifier(qualifier, expr.getLine())) {
            lastExprType_ = {"error", 0};
            return;
        }
        Scope* modScope = modSym->scope;
        std::shared_ptr<Symbol> mem = modScope ? modScope->resolveLocal(expr.getMember()) : nullptr;
        if (!mem) {
            error("module '" + qualifier + "' has no symbol '" + expr.getMember() + "'", expr.getLine());
            lastExprType_ = {"error", 0};
            return;
        }
        // CIDER-BUG-060: qualified access to a non-exported member is a
        // visibility error (values, consts, enum variants, functions-as-
        // values). Externs stay accessible; the owning module's own
        // self-qualified references are unrestricted. Fall through with the
        // member's real type after reporting: one visibility error should
        // not cascade into a bogus type-mismatch error.
        if (!isVisibleMember(mem, modSym->logicalName)) {
            error("symbol '" + mem->name + "' is not exported from module '" + qualifier + "'",
                  expr.getLine());
        }
        if (mem->kind == SymbolKind::ENUM) {
            // CIDER-FEAT-002: `lib.Color` alone is a type qualifier, not a
            // value; `lib.Color.RED` is intercepted before the object here
            // is evaluated.
            error("'" + qualifier + "." + mem->name + "' is a type, not a value", expr.getLine());
            lastExprType_ = {"error", 0};
            return;
        }
        if (mem->kind == SymbolKind::FUNCTION) {
            // CIDER-BUG-023: a module-qualified function value carries its
            // fn signature, not *void (the *void spelling made indirect
            // calls fall back to the generic int-result path and rejected
            // `var g: fn(...) = mod.fn;` global initializers).
            lastExprType_ = functionPointerTypeOf(
                std::static_pointer_cast<FunctionSymbol>(mem), expr.getLine());
            return;
        }
        if (mem->kind == SymbolKind::MODULE) {
            error("'" + qualifier + "." + expr.getMember() + "' is a module, not a value", expr.getLine());
            lastExprType_ = {"error", 0};
            return;
        }
        lastExprType_ = canonicalizeInModule(mem->type, modSym->logicalName, expr.getLine());
    }

    void visit(StructLiteralExpr& expr) override {
        // CIDER-BUG-095: an accepted module-qualified template spelling
        // (`m.Box<int>{...}`) resolves to the instantiation the collector
        // registered (CIDER-BUG-073), exactly like the type-position
        // spelling. CIDER-BUG-136: a bare spelling resolves through its
        // per-owner identity in separate compilation. Bare names with no
        // recorded identity stay untouched (T6).
        std::string structName = expr.getStructName();
        std::string ident = templateInstanceIdentity(structName, currentModule_);
        if (!ident.empty()) structName = ident;
        // CIDER-BUG-175: a qualified non-template spelling (`m.Holder{...}`)
        // resolves through the same nominal machinery as the type-position
        // spelling (CIDER-BUG-090): the qualifier selects the module, the
        // member must be exported (CIDER-BUG-060), and the canonical
        // identity matches every other use of the struct. canonicalize-
        // Nominal reports a failed dotted resolution itself (unknown member,
        // not exported, not a module), so no second generic error is added.
        bool dotted = structName.find('.') != std::string::npos;
        std::shared_ptr<Symbol> sym;
        std::string lookupName = structName;
        if (dotted && structName.find('<') == std::string::npos) {
            TypeInfo canon = canonicalizeNominal({structName, 0}, expr.getLine());
            if (canon.name == structName) {
                lastExprType_ = {"error", 0};
                return;
            }
            lookupName = canon.name;
        }
        sym = resolveScoped(lookupName);
        // CIDER-BUG-179: aliases are plain STRUCT-kind Symbols, not
        // StructSymbols. Resolve their chain before the literal's field
        // lookup, retaining the original spelling for diagnostics.
        if (sym && sym->isTypeAlias) {
            TypeInfo resolved = resolveType({lookupName, 0});
            sym = resolveScoped(resolved.name);
        }
        if (!sym || sym->kind != SymbolKind::STRUCT || sym->isTypeAlias) {
            error("Unknown struct type '" + expr.getStructName() + "'", expr.getLine());
            lastExprType_ = {"error", 0};
            return;
        }

        auto structSym = std::static_pointer_cast<StructSymbol>(sym);
        for (auto& fieldInit : expr.getFields()) {
            bool fieldFound = false;
            for (const auto& fieldDef : structSym->fields) {
                if (fieldDef.name == fieldInit.first) {
                    fieldFound = true;
                    // CIDER-BUG-090: resolve in the field's declaring
                    // module, like member access does.
                    TypeInfo expT = canonicalizeInModule(fieldDef.type, fieldDef.typeModule,
                                                         expr.getLine());
                    // CIDER-BUG-069: an interface-typed field in a struct
                    // literal accepts any satisfying concrete value, exactly
                    // like a direct assignment to the field.
                    resolveInterfaceFlag(expT);
                    // CIDER-BUG-165: a list literal for a fixed-array field
                    // takes the declaration-time checks against the declared
                    // shape, exactly like a declaration or assignment target
                    // (CIDER-BUG-112). visit(ListExpr) infers the context-free
                    // flat `*T` and rejected every list field; the shared
                    // helper also recurses into nested rows (CIDER-BUG-163)
                    // and converts interface elements (CIDER-BUG-135).
                    TypeInfo expArray = resolveType(expT);
                    if (auto* list = dynamic_cast<ListExpr*>(fieldInit.second.get());
                        list && !expT.isInterface && isFixedArrayValue(expArray)) {
                        checkFixedArrayElements("field '" + fieldDef.name + "'", expArray,
                                                *list, expr.getLine());
                        lastExprType_ = expT;
                        break;
                    }
                    fieldInit.second->accept(*this);
                    if (expT.isInterface) {
                        checkInterfaceValue(expT, lastExprType_, expr.getLine());
                    } else if (!areTypesCompatible(expT, lastExprType_)) {
                        error("Type mismatch for field '" + fieldDef.name +
                                  "'. Expected " + expT.toString() + ", got " +
                                  canonicalizeNominal(lastExprType_).toString(),
                              expr.getLine());
                    }
                    // CIDER-BUG-140: a fn(...) field storing a *void value
                    // keeps the plain representation, so a component that
                    // received both shapes is reported; a closure-typed field
                    // takes the cast-free conversion verdict.
                    TypeInfo expFn = resolveFunctionPointerAlias(expT);
                    if (expFn.isFunctionPointer && !expFn.isArray && !expFn.isClosure &&
                        isVoidCarrier(lastExprType_)) {
                        int slot = fnSlot("s:" + std::to_string(fnStoreCheckCount_++),
                                          "field '" + fieldDef.name + "'", expr.getLine());
                        fnRecordExpr(slot, fieldInit.second.get(), lastExprType_, expr.getLine());
                    }
                    if (expFn.isFunctionPointer && !expFn.isArray && expFn.isClosure &&
                        isVoidCarrier(lastExprType_)) {
                        fnCheckClosureConversion(expT, fieldInit.second.get(), lastExprType_,
                                                 expr.getLine());
                    }
                    break;
                }
            }
            if (!fieldFound) {
                error("Struct '" + expr.getStructName() + "' has no field '" + fieldInit.first + "'", expr.getLine());
            }
        }
        lastExprType_ = canonicalizeNominal({expr.getStructName(), 0}, expr.getLine());
    }

    void visit(SizeofExpr& expr) override {
        // CIDER-FEAT-005: `sizeof(T)` names T without reading it; note it.
        noteTypeUse(expr.getTargetType());
        lastExprType_ = {"int", 0};
    }

    void visit(CastExpr& expr) override {
        expr.getExpression()->accept(*this);
        TypeInfo srcType = lastExprType_;
        resolveInterfaceFlag(srcType);
        // CIDER-BUG-068: an unchecked cast does not change the runtime shape;
        // remember the source type so the representation inference can follow
        // a closure object through `closure as fn(...)`.
        castSourceTypes_[&expr] = srcType;
        lastExprType_ = canonicalizeNominal(TypeInfo::fromAST(expr.getTargetType()), expr.getLine());
        resolveInterfaceFlag(lastExprType_);
        // CIDER-BUG-018: an array-to-array cast is a layout reinterpretation
        // whose copy uses the TARGET size; require the same element type and
        // count, or `u"AB" as [u16;8]` copies 16 bytes from a 6-byte block.
        // CIDER-BUG-157: the array test must not key on `pointerLevel == 0`
        // — a closure/pointer-element array descriptor carries the element's
        // level, and without the check a `[*int;2] as [*int;3]` read past
        // the source (the shape the widened list support now reaches).
        TypeInfo target = resolveType(lastExprType_);
        TypeInfo src = resolveType(srcType);
        // CIDER-ISS-011: an interface cast is only a vtable-preserving
        // projection. Reject incompatible method order/signatures instead of
        // reinterpreting the source fat pointer as an unrelated interface.
        if (target.isInterface && !src.isInterface) {
            error("Cannot cast non-interface type '" + srcType.name + "' to interface '" +
                      lastExprType_.name + "'",
                  expr.getLine());
        } else if (target.isInterface && src.isInterface &&
                   !interfaceProjectionCompatible(target, src)) {
            error("Cannot cast interface '" + srcType.name + "' to interface '" +
                      lastExprType_.name +
                      "': source must provide target methods in the same order with matching signatures",
                  expr.getLine());
        }
        if (isFixedArrayValue(target) && isFixedArrayValue(src) &&
            (target.arraySize != src.arraySize || !sameArrayElementType(target, src))) {
            error("Array cast size/type mismatch. Expected " + target.toString() +
                      ", got " + src.toString(), expr.getLine());
        }
        // CIDER-BUG-074: a cast of a function value to a fn(...)/func(...)
        // type fixes the signature every later call uses, so it must not
        // invent one. `add1 as fn(int, int) -> int` type-checked for a
        // one-parameter function and the callee was then invoked with the
        // target's arity/conventions. Require the exact signature, the same
        // comparison every declaration, assignment, argument and return
        // position uses (params, return type, ABI; aliases resolved on both
        // sides). The closure/plain representation bit stays out of the
        // comparison: an unchecked cast keeps the source runtime shape
        // (CIDER-BUG-068's inference follows `closure as fn(...)`), so the
        // signature is the only thing the target spelling may not change.
        // Arrays are left to the layout rule above; a non-fn source (nilPtr,
        // *void) keeps its existing unchecked cast semantics.
        //
        // CIDER-BUG-093: keeping the source shape also means a cast cannot
        // *gain* one. Casting a plain fn value to the closure spelling
        // func(...) passed the signature check but left the raw code pointer
        // in a value typed as a closure object: every closure call loads a
        // fnptr from the object and passes the object as a hidden first
        // argument, so `f(1)` read instructions as a pointer on the VM and
        // segfaulted natively. A capture-less record cannot bridge the two
        // conventions either — the plain function would receive the record
        // pointer as its own first parameter. Record the source's runtime
        // shape in a cast-check slot exactly like the other representation
        // rules: a source that is really a closure (a closure-typed value,
        // or one reached through `closure as fn(...)`) resolves the slot
        // Closure and the reinterpretation stays allowed, while a source
        // that resolves to a plain pointer is reported in the post-pass.
        // The `fn(...)` spelling and a `func(...)` declaration (which
        // CIDER-BUG-063 adapts to the raw pointer representation) are the
        // supported spellings. The reverse direction is unaffected: a
        // closure cast to `fn(...)` stays a closure and is called through
        // the closure path (CIDER-BUG-068).
        TypeInfo fnTarget = resolveFunctionPointerAlias(target);
        TypeInfo fnSrc = resolveFunctionPointerAlias(src);
        // CIDER-BUG-094: an array of fn pointers and a scalar fn are
        // different layouts, not two spellings of one value. The array rule
        // above only compares array-to-array and the signature rule below
        // explicitly skips arrays ("left to the layout rule"), so a mixed
        // cast used to fall through both: the array's data-block address was
        // then called as a raw code pointer (VM `bad function id`, native
        // segfault), or the code pointer was indexed as a data block. There
        // is no fn-array decay rule in the language, so reject the mixed
        // shapes in both directions — the runtime shapes differ even when
        // the signatures agree (the element count is not part of a scalar
        // fn type at all).
        //
        // CIDER-BUG-130: the rejection must not depend on the array's
        // *element* being a fn pointer. `[int; 2]` is still a data-block
        // address and a scalar fn is still a code pointer; keying on
        // isFunctionPointer on both sides let a non-fn array slip through
        // the layout rule (array-to-array only) and the signature rule
        // (scalar fn on both sides only) and fail identically at runtime.
        // Test each side's array layout against the other side's scalar fn
        // spelling, in both directions, with aliases already resolved.
        bool targetScalarFn = fnTarget.isFunctionPointer && !fnTarget.isArray;
        bool srcScalarFn = fnSrc.isFunctionPointer && !fnSrc.isArray;
        if ((targetScalarFn && fnSrc.isArray) || (srcScalarFn && fnTarget.isArray)) {
            error("Function cast cannot change array layout. Expected " + fnTarget.toString() +
                      ", got " + fnSrc.toString(), expr.getLine());
        }
        if (fnTarget.isFunctionPointer && !fnTarget.isArray &&
            fnSrc.isFunctionPointer && !fnSrc.isArray) {
            resolveInterfaceFlag(fnTarget);
            resolveInterfaceFlag(fnSrc);
            resolveSignatureAliasPair(fnTarget, fnSrc);
            fnTarget = canonicalizeNominal(fnTarget);
            fnSrc = canonicalizeNominal(fnSrc);
            if (!fnTarget.signatureEquals(fnSrc)) {
                error("Function cast signature mismatch. Expected " + fnTarget.toString() +
                          ", got " + fnSrc.toString(), expr.getLine());
            } else if (fnTarget.isClosure) {
                int slot = fnSlot("c:" + std::to_string(fnCastCheckCount_++),
                                  fnTarget.toString(), expr.getLine());
                // CIDER-BUG-118/140: the verdict below is the resolved shape,
                // not the spelling. A fn(...) source whose component picked
                // up *void provenance is judged by the component rule in the
                // post-pass; a pure fn(...) source keeps the pre-118
                // leniency for unresolved shapes.
                fnRecordExpr(slot, expr.getExpression(), srcType, expr.getLine());
            }
        } else if (fnTarget.isFunctionPointer && !fnTarget.isArray && fnTarget.isClosure &&
                   isVoidCarrier(srcType) && !isNilSource(expr.getExpression())) {
            // CIDER-BUG-118/140: a `*void` source has no spelling that fixes
            // the shape, so it must prove the value is a closure object.
            // Follow its provenance through *void storage, assignments,
            // argument passing and nested casts; a proven closure object
            // stays allowed, a plain, mixed or unresolved shape is reported
            // in the post-pass (CIDER-BUG-140: every *void source must prove,
            // an opaque call result included). A direct nilPtr source is
            // representation-neutral, exactly like the func(...)
            // declaration initialized with nilPtr.
            int slot = fnVoidSlot("c:" + std::to_string(fnCastCheckCount_++),
                                  fnTarget.toString(), expr.getLine());
            fnSlotVoidSource_[slot] = 1;
            fnSlotMustProve_[slot] = 1;
            fnRecordExpr(slot, expr.getExpression(), srcType, expr.getLine());
        } else if (fnTarget.isFunctionPointer && !fnTarget.isArray && fnTarget.isClosure &&
                   srcType.name != "error" && !isNilSource(expr.getExpression()) &&
                   !(fnSrc.isFunctionPointer && !fnSrc.isArray)) {
            // CIDER-BUG-140: every other source spelling (a typed pointer
            // `*T`, string, array, integer, ...) carries no tracked
            // representation, so a cast to the closure spelling can only be
            // accepted when fnRecordExpr can trace a closure object through
            // the expression itself (e.g. `c as *u8 as func(...)`); an
            // unresolved source is rejected conservatively.
            int slot = fnSlot("c:" + std::to_string(fnCastCheckCount_++),
                              fnTarget.toString(), expr.getLine());
            fnSlotSourceSpelling_[slot] = srcType.toString();
            fnSlotMustProve_[slot] = 1;
            fnRecordExpr(slot, expr.getExpression(), srcType, expr.getLine());
        }
    }

    void visit(ArrayTypeAST&) override {}

    void visit(CallExpr& expr) override {
        auto callee = expr.getCallee();
        std::shared_ptr<FunctionSymbol> funcSym = nullptr;
        std::string displayName;
        // CIDER-BUG-165: an interface method call has no FunctionSymbol, so
        // its list-literal arguments are checked against the declared
        // signature in the member-access branch below; record those indices
        // so the indirect fallback does not visit the same list a second
        // time (duplicate element diagnostics).
        std::set<size_t> ifaceListArgsChecked;
        // CIDER-BUG-206: the declared return type of a dispatched interface
        // method, recorded alongside the list-argument indices for the same
        // reason — the indirect fallback has no FunctionSymbol to read it
        // from. Left default (empty name) when the callee is not an
        // interface dispatch, which the fallback checks before using it.
        TypeInfo ifaceDispatchRet;

        if (auto id = dynamic_cast<IdentifierExpr*>(callee)) {
            auto sym = resolveScoped(id->getName());
            if (sym && sym->kind == SymbolKind::FUNCTION) {
                funcSym = std::static_pointer_cast<FunctionSymbol>(sym);
            }
        } else if (auto mem = dynamic_cast<MemberAccessExpr*>(callee)) {
            // CID-020/021 §3: `b.fb(...)` calls the function in b's module
            // scope (same-symbol lookup as bindFromAliases, spec §4).
            if (!mem->isArrow()) {
                if (auto base = dynamic_cast<IdentifierExpr*>(mem->getObject())) {
                    auto baseSym = resolveScoped(base->getName());
                    if (baseSym && baseSym->kind == SymbolKind::MODULE) {
                        auto modSym = std::static_pointer_cast<ModuleSymbol>(baseSym);
                        displayName = base->getName() + "." + mem->getMember();
                        if (!reportAmbiguousQualifier(base->getName(), expr.getLine())) {
                            Scope* modScope = modSym->scope;
                            auto target =
                                modScope ? modScope->resolveLocal(mem->getMember()) : nullptr;
                            // CIDER-BUG-095: a qualified function-template
                            // call (`m.id<int>(x)`) resolves to the shared
                            // bare instantiation when the member itself is
                            // not in the module scope (self-qualified
                            // spellings, module-mode roots). The accepted-
                            // spelling map is the gate: a rejected spelling
                            // was never recorded.
                            if (!target) {
                                auto qti = qualifiedTemplateInstantiations_.find(
                                    base->getName() + "." + mem->getMember());
                                if (qti != qualifiedTemplateInstantiations_.end())
                                    target = resolveScoped(qti->second);
                            }
                            if (!target) {
                                error("module '" + base->getName() + "' has no symbol '" +
                                          mem->getMember() + "'",
                                      expr.getLine());
                            } else if (!isVisibleMember(target, modSym->logicalName)) {
                                // CIDER-BUG-060: `m.f()` on a non-exported
                                // function. Leaves funcSym null so the
                                // indirect-call fallback visits the arguments
                                // without cascading errors.
                                error("symbol '" + mem->getMember() +
                                          "' is not exported from module '" + base->getName() + "'",
                                      expr.getLine());
                            } else if (target->kind != SymbolKind::FUNCTION) {
                                error("'" + displayName + "' is not a function", expr.getLine());
                            } else {
                                funcSym = std::static_pointer_cast<FunctionSymbol>(target);
                            }
                        }
                        if (!funcSym) {
                            for (auto& arg : expr.getArgs()) {
                                arg->accept(*this);
                            }
                            lastExprType_ = {"int", 0};
                            return;
                        }
                    }
                }
            }
            if (!funcSym) {
                mem->getObject()->accept(*this);
                TypeInfo objType = lastExprType_;
                if (objType.pointerLevel > 0) objType.pointerLevel--;

                // CIDER-BUG-015: alias-safe lookup (providers are
                // STRUCT-kind plain Symbols, not StructSymbols).
                auto sym = resolveStructOrClass(objType);
                if (sym) {
                    auto classSym = std::static_pointer_cast<StructSymbol>(sym);
                    // CID-020/021 §1: method signature nominals belong to
                    // the class's owner module; canonicalize there so
                    // cross-module method calls compare canonically.
                    std::string ownerMod = definingModuleOf(sym);
                    for (const auto& m : classSym->methods) {
                        if (m.name == mem->getMember()) {
                            // Found the method
                            funcSym = std::make_shared<FunctionSymbol>();
                            funcSym->name = m.name;
                            // CIDER-BUG-090: an inherited method's signature
                            // spellings belong to the class that declared it.
                            std::string methodMod = methodModuleOf(m, ownerMod);
                            for (const auto& pt : m.parameterTypes) {
                                TypeInfo paramT = canonicalizeInModule(pt, methodMod, expr.getLine());
                                // CIDER-BUG-080: class-method interface
                                // parameters carry the fat-pointer flag; the
                                // collector resolves it, this keeps the
                                // fallback for signature copies.
                                resolveInterfaceFlag(paramT);
                                funcSym->parameterTypes.push_back(paramT);
                            }
                            funcSym->returnType =
                                canonicalizeInModule(m.returnType, methodMod, expr.getLine());
                            // CIDER-BUG-077: a method's interface-typed
                            // return is a fat pointer; recover the flag for
                            // signature copies that lost it.
                            resolveInterfaceFlag(funcSym->returnType);
                            break;
                        }
                    }
                }
                // CIDER-BUG-165: interface dispatch is indirect (funcSym
                // stays null), so the shared argument loop below cannot
                // check its arguments. Validate a list literal against the
                // interface method's declared fixed-array parameter here;
                // every other argument spelling keeps the legacy unchecked
                // behavior. The signature lookups mirror the member-access
                // return-type rule above.
                if (auto ifaceSym = resolveInterfaceSymbol(objType)) {
                    auto iface = std::static_pointer_cast<InterfaceSymbol>(ifaceSym);
                    std::string ifaceMod = definingModuleOf(ifaceSym);
                    for (const auto& m : iface->methods) {
                        if (m.name != mem->getMember()) continue;
                        std::string methodMod = methodModuleOf(m, ifaceMod);
                        // CIDER-BUG-206: interface dispatch has no
                        // FunctionSymbol, so this is the only place the
                        // declared return type of a dispatched method is
                        // available. Record it for the indirect fallback
                        // below, which otherwise types the dispatch result as
                        // a bare `int` and so rejected every useful
                        // by-value aggregate result ("Expected P, got int")
                        // while letting the discarded-result shape through to
                        // a miscompile. The spelling is rooted in the
                        // method's defining module, exactly like the parameter
                        // spellings below and the member-access return-type
                        // rule above.
                        ifaceDispatchRet =
                            canonicalizeInModule(m.returnType, methodMod, expr.getLine());
                        // CIDER-BUG-077: an interface-typed return is the
                        // 16-byte fat pointer, not an aggregate.
                        resolveInterfaceFlag(ifaceDispatchRet);
                        for (size_t i = 0;
                             i < expr.getArgs().size() && i < m.parameterTypes.size(); ++i) {
                            auto* list = dynamic_cast<ListExpr*>(expr.getArgs()[i].get());
                            if (!list) continue;
                            TypeInfo paramT = canonicalizeInModule(
                                m.parameterTypes[i], methodMod, expr.getLine());
                            resolveInterfaceFlag(paramT);
                            TypeInfo paramArray = resolveType(paramT);
                            if (paramT.isInterface || !isFixedArrayValue(paramArray)) {
                                continue;
                            }
                            checkFixedArrayElements(
                                "argument " + std::to_string(i + 1) + " to '" + m.name + "'",
                                paramArray, *list, expr.getLine());
                            ifaceListArgsChecked.insert(i);
                        }
                        break;
                    }
                }
            }
        }

        if (!funcSym) {
            // Indirect call: a typed fn(...) value (CID-006) is checked
            // against its signature; the legacy *void / closure fallback
            // keeps accepting generically with an int result.
            callee->accept(*this);
            // CID-006 fix (F1): resolve `type Cb = fn(...)` aliases first,
            // or alias-typed callees silently take the generic fallback.
            lastExprType_ = resolveFunctionPointerAlias(lastExprType_);
            if (lastExprType_.isFunctionPointer && !lastExprType_.isArray &&
                lastExprType_.fnParamTypes && lastExprType_.fnReturnType) {
                TypeInfo fnType = lastExprType_;
                bool unsupportedAggregate = isByValueAggregateAtCall(*fnType.fnReturnType);
                for (const auto& pt : *fnType.fnParamTypes) {
                    if (isByValueAggregateAtCall(pt)) unsupportedAggregate = true;
                }
                // CIDER-BUG-077: a fn(...) value returning an interface would
                // need the hidden sret slot, which indirect calls do not
                // pass; reject instead of misreading the call result.
                TypeInfo fnRetFlag = *fnType.fnReturnType;
                resolveInterfaceFlag(fnRetFlag);
                if (fnRetFlag.isInterface) {
                    error("Calls through '" + fnType.toString() +
                              "' values do not support interface returns",
                          expr.getLine());
                } else if (unsupportedAggregate) {
                    error("Calls through '" + fnType.toString() +
                              "' values do not support struct/class/array by-value parameters or returns",
                          expr.getLine());
                } else if (expr.getArgs().size() != fnType.fnParamTypes->size()) {
                    error("Function '" + fnType.toString() + "' expects " +
                              std::to_string(fnType.fnParamTypes->size()) +
                              " arguments, got " + std::to_string(expr.getArgs().size()),
                          expr.getLine());
                } else {
                    for (size_t i = 0; i < expr.getArgs().size(); ++i) {
                        expr.getArgs()[i]->accept(*this);
                        TypeInfo paramT = (*fnType.fnParamTypes)[i];
                        // CIDER-BUG-080: an interface-typed slot in a
                        // fn(...) signature accepts the same satisfying
                        // concrete values/pointers as a declaration; the IR
                        // generator materialises the fat pointer at the call
                        // site for any callee shape.
                        resolveInterfaceFlag(paramT);
                        if (paramT.isInterface) {
                            if (lastExprType_.name != "error") {
                                checkInterfaceValue(paramT, lastExprType_, expr.getLine());
                            }
                        } else if (!areTypesCompatible(paramT, lastExprType_)) {
                            error("Argument " + std::to_string(i + 1) + " to '" +
                                      fnType.toString() + "' type mismatch. Expected " +
                                      paramT.toString() + ", got " +
                                      canonicalizeNominal(lastExprType_).toString(),
                                  expr.getLine());
                        }
                    }
                }
                lastExprType_ = *fnType.fnReturnType;
                return;
            }
            // CIDER-BUG-077: an interface method call reaches here through
            // the callee member access, which leaves the method's return
            // type in lastExprType_ (the vtable dispatch has no
            // FunctionSymbol). Preserve an interface result — the legacy
            // fallback below assumes int and would drop the fat pointer.
            TypeInfo calleeResultType = lastExprType_;
            for (size_t ai = 0; ai < expr.getArgs().size(); ++ai) {
                // CIDER-BUG-165: an interface method call's list arguments
                // were already visited by the declared-shape check above.
                if (ifaceListArgsChecked.count(ai)) continue;
                expr.getArgs()[ai]->accept(*this);
            }
            if (calleeResultType.isInterface) {
                lastExprType_ = calleeResultType;
                return;
            }
            // CIDER-BUG-206: same rule for a by-value AGGREGATE result
            // (struct, class or fixed array). The implementation is a
            // non-virtual method, so ir_gen compiles it with the hidden sret
            // convention and the dispatch call site allocates and passes the
            // result slot (ir_gen::visit(CallExpr), the interface branch);
            // typing the result as `int` here rejected every useful shape
            // ("Expected P, got int") while the discarded-result shape fell
            // through to the miscompile. `ifaceDispatchRet` is the declared
            // return spelling recorded by the member-access branch above; it
            // is empty for every non-dispatch callee, and an interface result
            // already returned above, so this only fires for aggregates.
            if (!ifaceDispatchRet.name.empty() && isByValueAggregateAtCall(ifaceDispatchRet)) {
                lastExprType_ = ifaceDispatchRet;
                return;
            }
            lastExprType_ = {"int", 0};
            return;
        }

        if (expr.getArgs().size() != funcSym->parameterTypes.size()) {
            const std::string& label = displayName.empty() ? funcSym->name : displayName;
            error("Function '" + label + "' expects " + std::to_string(funcSym->parameterTypes.size()) +
                      " arguments, got " + std::to_string(expr.getArgs().size()),
                  expr.getLine());
        } else {
            // CID-020/021 §1: parameter nominals belong to the callee's
            // defining module; canonicalize there so b.Event passed where
            // a.Event is expected is a type error, not a layout mix-up.
            std::string defMod = definingModuleOf(funcSym);
            const std::string& label = displayName.empty() ? funcSym->name : displayName;
            for (size_t i = 0; i < expr.getArgs().size(); ++i) {
                TypeInfo paramT =
                    canonicalizeInModule(funcSym->parameterTypes[i], defMod, expr.getLine());
                // CIDER-BUG-080: a parameter declared with an interface type
                // accepts a satisfying concrete value/pointer or the same
                // interface, exactly like a declaration-time assignment
                // (checkInterfaceValue is the shared rule). A method's
                // parameter flags are resolved by the collector; this is the
                // fallback for signature copies that lost the flag.
                resolveInterfaceFlag(paramT);
                // CIDER-BUG-165: a list literal passed to a fixed-array
                // parameter takes the declaration-time checks against the
                // declared shape, exactly like a declaration/assignment
                // target (CIDER-BUG-112/163); visit(ListExpr) infers the
                // context-free flat `*T` and rejected every list argument.
                // The IR generator materialises the same declared shape at
                // the call site.
                TypeInfo paramArray = resolveType(paramT);
                if (auto* list = dynamic_cast<ListExpr*>(expr.getArgs()[i].get());
                    list && !paramT.isInterface && isFixedArrayValue(paramArray)) {
                    checkFixedArrayElements(
                        "argument " + std::to_string(i + 1) + " to '" + label + "'",
                        paramArray, *list, expr.getLine());
                    continue;
                }
                expr.getArgs()[i]->accept(*this);
                TypeInfo argT = lastExprType_;
                TypeInfo fnParam = resolveFunctionPointerAlias(paramT);
                TypeInfo fnArg = resolveFunctionPointerAlias(argT);
                // CIDER-BUG-068: a closure object is accepted where a plain
                // `fn(...)` parameter with the exact same signature (ABI
                // included) is expected. The whole-program inference below
                // then decides whether this parameter is closure-represented;
                // a parameter that also receives raw fn values is reported as
                // a representation conflict instead of being miscompiled.
                bool closureToFnParam =
                    fnParam.isFunctionPointer && !fnParam.isClosure && !fnParam.isArray &&
                    fnArg.isFunctionPointer && fnArg.isClosure && !fnArg.isArray &&
                    fnParam.signatureEquals(fnArg);
                if (paramT.isInterface) {
                    // CIDER-BUG-080: interface call arguments. Skip the
                    // conversion when the argument already failed (an
                    // `error` type would only cascade).
                    if (argT.name != "error") checkInterfaceValue(paramT, argT, expr.getLine());
                } else if (!areTypesCompatible(paramT, argT) && !closureToFnParam) {
                    error("Argument " + std::to_string(i + 1) + " to '" + label +
                              "' type mismatch. Expected " + paramT.toString() + ", got " +
                              canonicalizeNominal(lastExprType_).toString(),
                          expr.getLine());
                } else if (fnParam.isFunctionPointer && !fnParam.isArray) {
                    // Record the argument's representation against the
                    // parameter slot (closure/plain constant or a slot edge
                    // when a parameter/local value flows through).
                    int slot = fnParamSlot(funcSym, i, expr.getLine());
                    fnRecordExpr(slot, expr.getArgs()[i].get(), argT, expr.getLine());
                    // CIDER-BUG-140: a *void argument passed to a closure
                    // parameter is a cast-free conversion and must prove the
                    // closure object, exactly like an explicit cast.
                    if (fnParam.isClosure && isVoidCarrier(argT)) {
                        fnCheckClosureConversion(paramT, expr.getArgs()[i].get(), argT,
                                                 expr.getLine());
                    }
                } else if (isVoidCarrier(paramT)) {
                    // CIDER-BUG-118: record the argument's fn representation
                    // against the *void parameter slot, so a cast inside the
                    // callee can prove the closure shape (or report a raw fn
                    // pointer) exactly like an in-body *void storage.
                    int slot = fnVoidParamSlot(funcSym, i, expr.getLine());
                    fnRecordExpr(slot, expr.getArgs()[i].get(), argT, expr.getLine());
                }
            }
        }
        lastExprType_ = canonicalizeInModule(funcSym->returnType, definingModuleOf(funcSym), expr.getLine());
        // CIDER-BUG-077: an interface-typed call result is the address of
        // the caller's 16-byte sret slot; keep the fat-pointer flag so
        // assignment, dispatch, chaining and argument passing all see it.
        resolveInterfaceFlag(lastExprType_);
    }

    void visit(FunctionDeclStmt& stmt) override {
        // Skip template function bodies (uninstantiated). CIDER-FEAT-005:
        // references inside them are not traversed, so unused-import
        // warnings are disabled for a program that declares a template.
        if (stmt.hasTemplateParams()) {
            if (warningsActive_) warnings_.suppressUnusedImports();
            return;
        }

        // CIDER-BUG-115: lifetime bookkeeping is per frame. A nested
        // function body knows only its own locals/params as frame storage;
        // the outer sets are restored when it finishes.
        std::set<uint64_t> prevFrameLocalSymbols;
        std::set<uint64_t> prevFrameLocalPointers;
        if (warningsActive_) {
            prevFrameLocalSymbols = frameLocalSymbols_;
            prevFrameLocalPointers = frameLocalPointers_;
            frameLocalSymbols_.clear();
            frameLocalPointers_.clear();
        }

        // CID-005 (T5/F1): remember the declared return type for the
        // narrow fn-pointer return check; saved/restored so nested
        // declarations cannot leak it.
        TypeInfo prevReturnType = currentDeclaredReturnType_;
        bool prevHasReturnType = hasDeclaredReturnType_;
        // CIDER-BUG-068: the function name labels return-representation
        // diagnostics; saved/restored like the declared return type.
        std::string prevFunctionName = currentFunctionName_;
        // CIDER-BUG-210: a nested function body is its own frame (ir_gen
        // clears deferStack_ for it), so a defer of the OUTER function
        // cannot make a `return` in here illegal.
        int prevDeferBodyDepth = deferBodyDepth_;
        deferBodyDepth_ = 0;
        currentFunctionName_ = stmt.getName();
        currentDeclaredReturnType_ = stmt.getReturnType()
            ? canonicalizeNominal(TypeInfo::fromAST(stmt.getReturnType()), stmt.getLine())
            : TypeInfo{"void", 0};
        // CIDER-BUG-085: a declared `-> fn(I) -> int` return type resolves
        // its signature's interface flags, so the ReturnStmt signature check
        // compares it with a named function's flagged signature consistently.
        // CIDER-BUG-077: an interface-typed return is the 16-byte fat
        // pointer, not a plain nominal; resolve the flag here so the return
        // check and the IR generator's sret decision agree.
        resolveInterfaceFlag(currentDeclaredReturnType_);
        // CIDER-BUG-077: virtual/override methods stay on the legacy
        // pointer-return path (VIRTUAL_CALL cannot carry a hidden sret
        // slot), so an interface-typed virtual return would dispatch with
        // the wrong convention. Reject it loudly instead.
        if (currentDeclaredReturnType_.isInterface &&
            (stmt.isVirtual() || stmt.isOverride())) {
            error("Virtual method '" + stmt.getName() +
                      "' cannot return an interface: virtual dispatch does not support interface returns",
                  stmt.getLine());
        }
        hasDeclaredReturnType_ = true;

        symbolTable_.enterScope();

        if (!currentClassName_.empty()) {
            auto thisSym = std::make_shared<Symbol>();
            thisSym->kind = SymbolKind::VARIABLE;
            thisSym->name = "this";
            thisSym->type.name = currentClassName_;
            thisSym->type.pointerLevel = 1;
            thisSym->type.isUserDefined = true;
            symbolTable_.define(thisSym);
        }

        // CIDER-BUG-068: register this free function's fn-typed parameters
        // with the representation inference, so call sites can constrain
        // them and the resolution can mark their TypeAST. Methods keep the
        // existing rejection (their call sites resolve a synthesized
        // FunctionSymbol, so no parameter slot can be identified).
        std::shared_ptr<FunctionSymbol> fnDeclSym;
        if (currentClassName_.empty() && !stmt.isExtern()) {
            auto declSym = resolveScoped(stmt.getName());
            if (declSym && declSym->kind == SymbolKind::FUNCTION) {
                fnDeclSym = std::static_pointer_cast<FunctionSymbol>(declSym);
            }
        }

        size_t fnParamIndex = 0;
        for (auto& param : stmt.getParams()) {
            auto paramSym = std::make_shared<Symbol>();
            paramSym->kind = SymbolKind::VARIABLE;
            paramSym->name = param.first;
            paramSym->type = TypeInfo::fromAST(param.second.get());
            // CID-020/021 §1: store the canonical nominal identity (dotted
            // spellings resolve here, with the diagnostic at this line).
            paramSym->type = canonicalizeNominal(paramSym->type, stmt.getLine());
            // CIDER-BUG-085: recursive, so a `f: fn(I) -> int` parameter
            // carries the interface flag nested in its signature, matching
            // the signature the collector stored on the function symbol.
            resolveInterfaceFlag(paramSym->type);
            symbolTable_.define(paramSym);
            // CIDER-BUG-115: a parameter's storage dies with this frame.
            if (warningsActive_) frameLocalSymbols_.insert(paramSym->id);
            if (fnDeclSym) {
                TypeInfo paramFn = resolveFunctionPointerAlias(paramSym->type);
                if (paramFn.isFunctionPointer && !paramFn.isArray) {
                    std::string key = fnParamSlotKey(fnDeclSym, fnParamIndex);
                    fnParamAst_[key] = param.second.get();
                    fnParamSymKey_[paramSym->id] = key;
                    int slot = fnSlot(key,
                                      "parameter '" + param.first + "' of '" + stmt.getName() + "'",
                                      stmt.getLine());
                    fnSlotRigid_[fnFind(slot)] = 1;
                } else if (isVoidCarrier(paramSym->type)) {
                    // CIDER-BUG-118: a *void parameter is a provenance-only
                    // slot. Call sites record the arguments' fn shapes here,
                    // and a cast inside the body follows the parameter
                    // symbol into this slot; there is no *void TypeAST
                    // annotation (the storage is untyped).
                    std::string key = fnParamSlotKey(fnDeclSym, fnParamIndex);
                    fnParamSymKey_[paramSym->id] = key;
                    fnVoidSlot(key, "parameter '" + param.first + "' of '" + stmt.getName() + "'",
                               stmt.getLine());
                }
            }
            ++fnParamIndex;
            // CIDER-FEAT-005: parameters are never unused-warned, but a
            // local shadowing one is.
            if (warningsActive_) {
                warnings_.declare(symbolTable_.getCurrentScope(), paramSym, stmt.getLine(),
                                  stmt.getColumn(), /*warnUnused=*/false, /*checkShadow=*/false);
            }
        }
        if (!stmt.isExtern()) {
            for (auto& s : stmt.getBody()) s->accept(*this);
        }
        exitScopeChecked();
        if (warningsActive_) {
            frameLocalSymbols_ = std::move(prevFrameLocalSymbols);
            frameLocalPointers_ = std::move(prevFrameLocalPointers);
        }
        currentDeclaredReturnType_ = prevReturnType;
        hasDeclaredReturnType_ = prevHasReturnType;
        currentFunctionName_ = prevFunctionName;
        deferBodyDepth_ = prevDeferBodyDepth;
    }

    void visit(BlockStmt& stmt) override {
        symbolTable_.enterScope();
        for (auto& s : stmt.getStatements()) s->accept(*this);
        exitScopeChecked();
    }

    void visit(IfStmt& stmt) override {
        stmt.getCondition()->accept(*this);
        if (resolveType(lastExprType_).name != "bool" || lastExprType_.pointerLevel > 0) {
            error("If condition must be a boolean, got " + lastExprType_.toString(), stmt.getLine());
        }
        symbolTable_.enterScope();
        for (auto& s : stmt.getThenBranch()) s->accept(*this);
        exitScopeChecked();
        if (!stmt.getElseBranch().empty()) {
            symbolTable_.enterScope();
            for (auto& s : stmt.getElseBranch()) s->accept(*this);
            exitScopeChecked();
        }
    }

    void visit(WhileStmt& stmt) override {
        stmt.getCondition()->accept(*this);
        if (resolveType(lastExprType_).name != "bool" || lastExprType_.pointerLevel > 0) {
            error("While condition must be a boolean, got " + lastExprType_.toString(), stmt.getLine());
        }
        symbolTable_.enterScope();
        for (auto& s : stmt.getBody()) s->accept(*this);
        exitScopeChecked();
    }

    void visit(ForStmt& stmt) override {
        symbolTable_.enterScope();
        if (stmt.getInitializer()) stmt.getInitializer()->accept(*this);
        if (stmt.getCondition()) {
            stmt.getCondition()->accept(*this);
            if (resolveType(lastExprType_).name != "bool" || lastExprType_.pointerLevel > 0) {
                error("For condition must be a boolean", stmt.getLine());
            }
        }
        if (stmt.getIncrement()) stmt.getIncrement()->accept(*this);
        for (auto& s : stmt.getBody()) s->accept(*this);
        exitScopeChecked();
    }

    void visit(ReturnStmt& stmt) override {
        // CIDER-BUG-210: see rejectDeferTransfer.
        if (rejectDeferTransfer("return", stmt.getLine())) return;
        if (stmt.getValue()) {
            // CIDER-BUG-177: a list literal returned into a fixed-array
            // return type takes the declaration-time checks against the
            // DECLARED shape, exactly like a declaration, an assignment
            // (CIDER-BUG-112) or a call argument (CIDER-BUG-165) target.
            // visit(ListExpr) infers the context-free flat `*T` for every
            // list literal, so the general compatibility test below saw
            // `*int` where the function returns `[int;2]` and rejected
            // every return-position list literal. Nothing else in this
            // visit applies to an array: a declared array is never a
            // function pointer, a non-list interface or a closure, so the
            // checks below would only re-test the declared shape against
            // itself. ir_gen materialises the same declared shape (see
            // emitListLiteralTemp).
            TypeInfo declArray;
            if (hasDeclaredReturnType_) declArray = resolveType(currentDeclaredReturnType_);
            // CIDER-BUG-202: a returned fixed array whose leaves are
            // interface elements carries N fat pointers whose data words
            // may address THIS frame (an element converted from a concrete
            // value stores the object's address, exactly like a non-list
            // interface store). ir_gen boxes every element at the return
            // site — the CIDER-BUG-077/184 whole-value interface rule
            // applied per element — and the caller releases the boxes, so
            // the program needs the tagged boxing representation decided
            // before any function is generated. Record the shape here, like
            // the whole-value case below. Bounded to the declared shape
            // (no dataflow: an element may alias a frame, a global or a
            // heap object, and only the return-site rule covers all three).
            //
            // CIDER-BUG-212: the same holds for an interface value EMBEDDED
            // in the returned aggregate — a struct/class field, at any
            // nesting depth, including an interface array field. The sret
            // copy moves those fat pointers verbatim exactly as it moved a
            // returned array's elements, so they dangle the same way;
            // hasEmbeddedInterfaceValue walks the whole by-value layout, not
            // just array dimensions, so a returned struct carrying an
            // interface field turns the representation on.
            if (hasDeclaredReturnType_ && hasEmbeddedInterfaceValue(declArray, currentModule_, 0)) {
                interfaceBoxing_ = true;
            }
            if (auto* list = dynamic_cast<ListExpr*>(stmt.getValue());
                list && isFixedArrayValue(declArray)) {
                if (checkFixedArrayElements("return of '" + currentFunctionName_ + "'",
                                            declArray, *list, stmt.getLine())) {
                    lastExprType_ = declArray;
                }
                return;
            }
            stmt.getValue()->accept(*this);
            // CID-005 (T5/F1): when a function-pointer type is on either
            // side of a return, require the exact signature (ABI included).
            // Narrow on purpose: the general return-statement type gap is
            // pre-existing and out of scope, but returning a SysV function
            // where an `msabi fn` is declared (or vice versa) used to be
            // silently accepted and then crashed at the indirect call.
            // CID-005 fix round 1 (Q3): if the returned expression already
            // failed, lastExprType_ is `error`; running the F1 signature
            // comparison on it just adds a bogus cascade diagnostic.
            if (hasDeclaredReturnType_ && lastExprType_.name != "error") {
                TypeInfo declType = resolveFunctionPointerAlias(currentDeclaredReturnType_);
                TypeInfo valType = resolveFunctionPointerAlias(lastExprType_);
                // CIDER-BUG-077: an interface-typed return accepts the same
                // satisfying values as every other interface position — the
                // same interface (a fat-pointer copy), or a satisfying
                // concrete struct/class value or single pointer (the fat
                // pointer is built at the return site). Different interfaces
                // and non-object values (nilPtr included) are rejected by
                // the shared rule, exactly like declaration-time assignment
                // and call arguments. ir_gen returns the 16-byte fat pointer
                // through the hidden-sret convention.
                if (declType.isInterface) {
                    // CIDER-BUG-184: record the boxing shape (concrete value
                    // source) so ir_gen can release the box it allocates;
                    // see hasInterfaceBoxing.
                    bool boxesConcrete = false;
                    checkInterfaceValue(declType, valType, stmt.getLine(), &boxesConcrete);
                    if (boxesConcrete) interfaceBoxing_ = true;
                } else {
                // F1: bounded alias resolution; on budget exhaustion both
                // sides keep their unexpanded (pre-resolution) form.
                resolveSignatureAliasPair(declType, valType);
                bool declFn = declType.isFunctionPointer && !declType.isArray;
                bool valFn = valType.isFunctionPointer && !valType.isArray;
                if (declFn && valFn) {
                    // CIDER-BUG-063: a closure object and a raw fn code
                    // pointer are distinct representations even with an
                    // identical signature; returning one where the other is
                    // declared would call through the wrong convention.
                    if (!declType.signatureEquals(valType) ||
                        declType.isClosure != valType.isClosure) {
                        error("Return type mismatch. Expected " + declType.toString() +
                                  ", got " + valType.toString(),
                              stmt.getLine());
                    }
                } else if ((declFn || valFn) && !areTypesCompatible(declType, valType)) {
                    // Non-fn value against a fn declaration (or the reverse):
                    // only the pre-existing *void/nilPtr and wildcard routes
                    // stay allowed.
                    error("Return type mismatch. Expected " + canonicalizeNominal(declType).toString() +
                              ", got " + canonicalizeNominal(valType).toString(),
                          stmt.getLine());
                } else if (!declFn && !valFn && !areTypesCompatible(declType, valType)) {
                    // CIDER-BUG-016: general return-statement check. Same
                    // compatibility rules as declarations and assignments —
                    // int/float and *void/nilPtr routes stay lenient — but
                    // cross-kind returns that would silently reinterpret
                    // (a value from a void function, a struct where a scalar
                    // is declared, int -> pointer) are rejected.
                    error("Return type mismatch. Expected " + canonicalizeNominal(declType).toString() +
                              ", got " + canonicalizeNominal(valType).toString(),
                          stmt.getLine());
                }
                // CIDER-BUG-018: fixed-size array returns (wide strings
                // included) must match in element type and count — the
                // permissive numeric fallback would otherwise accept
                // `return u"AB";` in a `[u16;4]` function and MEMCPY 8
                // bytes from the 6-byte literal.
                TypeInfo declArr = resolveType(currentDeclaredReturnType_);
                TypeInfo valArr = resolveType(lastExprType_);
                if (declArr.isArray && declArr.pointerLevel == 0 && declArr.arraySize > 0 &&
                    valArr.isArray && valArr.pointerLevel == 0 && valArr.arraySize > 0 &&
                    (declArr.arraySize != valArr.arraySize ||
                     !sameArrayElementType(declArr, valArr))) {
                    error("Return type mismatch. Expected " + declArr.toString() +
                              ", got " + valArr.toString(),
                          stmt.getLine());
                }
                // CIDER-BUG-068: a function declared to return a plain
                // `fn(...)` value must not hand a closure object to its
                // caller (callers type the result as a raw code pointer and
                // call it directly). The value is statically a raw pointer
                // here (the exact-match branch above accepted it), but a
                // parameter/local/cast can resolve to the closure
                // representation; record it and report in the post-pass
                // resolution. Returning a closure object where `fn(...)` is
                // declared stays unsupported (use the `func(...)` return
                // spelling); a closure-typed return value is already reported
                // by the mismatch branch above.
                // CIDER-BUG-140: the same verdict applies when the value
                // reaches the fn(...) return through *void (a mixed source is
                // reported in the post-pass), and a *void value returned from
                // a closure-typed function takes the cast-free conversion
                // check.
                if (declType.isFunctionPointer && !declType.isClosure && !declType.isArray &&
                    ((valType.isFunctionPointer && !valType.isClosure && !valType.isArray &&
                      declType.signatureEquals(valType)) ||
                     (isVoidCarrier(lastExprType_) && !isNilSource(stmt.getValue())))) {
                    int slot = fnSlot("x:" + std::to_string(fnReturnCheckCount_++),
                                      "the return value of '" + currentFunctionName_ + "'",
                                      stmt.getLine());
                    fnRecordExpr(slot, stmt.getValue(), lastExprType_, stmt.getLine());
                }
                if (declType.isFunctionPointer && !declType.isArray && declType.isClosure &&
                    isVoidCarrier(lastExprType_)) {
                    fnCheckClosureConversion(declType, stmt.getValue(), lastExprType_,
                                             stmt.getLine());
                }
                }  // !declType.isInterface
            }
        } else {
            // CIDER-BUG-016: a value-less return in a function declared to
            // return something other than void.
            if (hasDeclaredReturnType_ && currentDeclaredReturnType_.name != "error" &&
                !(currentDeclaredReturnType_.name == "void" &&
                  currentDeclaredReturnType_.pointerLevel == 0)) {
                error("Return without a value in function declared to return " +
                          canonicalizeNominal(currentDeclaredReturnType_).toString(),
                      stmt.getLine());
            }
            lastExprType_ = {"void", 0};
        }
    }

    void visit(ExpressionStmt& stmt) override { stmt.getExpression()->accept(*this); }

    void visit(ThisExpr& expr) override {
        auto sym = resolveScoped("this");
        if (sym) {
            lastExprType_ = sym->type;
        } else {
            error("'this' used outside of a class method", expr.getLine());
            lastExprType_ = {"error", 0};
        }
    }

    void visit(SuperExpr& expr) override {
        if (currentClassName_.empty()) {
            error("'super' can only be used inside a class", expr.getLine());
            lastExprType_ = {"error", 0};
            return;
        }
        auto classSym = resolveScoped(currentClassName_);
        if (!classSym || classSym->kind != SymbolKind::CLASS) {
            error("Current class '" + currentClassName_ + "' not found", expr.getLine());
            lastExprType_ = {"error", 0};
            return;
        }
        auto cs = std::static_pointer_cast<StructSymbol>(classSym);
        if (cs->parentName.empty()) {
            error("Class '" + currentClassName_ + "' has no parent class", expr.getLine());
            lastExprType_ = {"error", 0};
            return;
        }
        auto parentSym = resolveParentClass(cs->parentName);
        if (!parentSym || (parentSym->kind != SymbolKind::CLASS && parentSym->kind != SymbolKind::STRUCT)) {
            error("Parent class '" + cs->parentName + "' not found", expr.getLine());
            lastExprType_ = {"error", 0};
            return;
        }
        auto ps = std::static_pointer_cast<StructSymbol>(parentSym);
        for (auto& m : ps->methods) {
            if (m.name == expr.getMember()) {
                // CIDER-BUG-090: resolve in the method's declaring module,
                // which the parent may itself have inherited the method from.
                lastExprType_ = canonicalizeInModule(
                    m.returnType, methodModuleOf(m, definingModuleOf(parentSym)), expr.getLine());
                return;
            }
        }
        error("Method '" + expr.getMember() + "' not found in parent class '" + cs->parentName + "'", expr.getLine());
        lastExprType_ = {"error", 0};
    }

    void visit(AwaitExpr& expr) override {
        // await expr — the awaited expression is usually a call to an async
        // function. Check it; the result type is the awaited value's type.
        expr.getValue()->accept(*this);
        // The awaited call must resolve to an async function.
        if (auto call = dynamic_cast<CallExpr*>(expr.getValue())) {
            if (auto id = dynamic_cast<IdentifierExpr*>(call->getCallee())) {
                auto sym = resolveScoped(id->getName());
                if (sym && sym->kind == SymbolKind::FUNCTION) {
                    auto fs = std::static_pointer_cast<FunctionSymbol>(sym);
                    if (!fs->isAsync) {
                        error("Can only await async functions, '" + id->getName() + "' is not async.", expr.getLine());
                    }
                }
            }
        }
    }

    void visit(ListExpr& expr) override {
        auto& elements = expr.getElements();
        if (elements.empty()) {
            lastExprType_ = {"void", 1};
            return;
        }
        elements[0]->accept(*this);
        TypeInfo elemType = lastExprType_;
        for (size_t i = 1; i < elements.size(); i++) {
            elements[i]->accept(*this);
            if (lastExprType_.name != elemType.name ||
                lastExprType_.pointerLevel != elemType.pointerLevel) {
                error("List element type mismatch: expected " +
                          elemType.toString() + ", got " + lastExprType_.toString(),
                      expr.getLine());
                lastExprType_ = {"error", 0};
                return;
            }
        }
        lastExprType_ = elemType;
        lastExprType_.pointerLevel++;
    }

    void visit(TableExpr& expr) override {
        auto& entries = expr.getEntries();
        if (entries.empty()) {
            lastExprType_ = {"void", 1};
            return;
        }
        entries[0].first->accept(*this);
        TypeInfo keyType = lastExprType_;
        entries[0].second->accept(*this);
        TypeInfo valType = lastExprType_;
        for (size_t i = 1; i < entries.size(); i++) {
            entries[i].first->accept(*this);
            if (lastExprType_.name != keyType.name || lastExprType_.pointerLevel != keyType.pointerLevel) {
                error("Table key type mismatch at entry " + std::to_string(i), expr.getLine());
                lastExprType_ = {"error", 0};
                return;
            }
            entries[i].second->accept(*this);
            if (lastExprType_.name != valType.name || lastExprType_.pointerLevel != valType.pointerLevel) {
                error("Table value type mismatch at entry " + std::to_string(i), expr.getLine());
                lastExprType_ = {"error", 0};
                return;
            }
        }
        lastExprType_ = {"void", 1};  // pointer to KV pairs
    }

    void visit(IndexExpr& expr) override {
        expr.getObject()->accept(*this);
        // CIDER-BUG-019: resolve `type X = [T; N]` aliases before the
        // array/pointer tests, or indexing an alias-typed array is
        // rejected as "non-array and non-pointer type X".
        TypeInfo objType = resolveType(lastExprType_);
        expr.getIndex()->accept(*this);
        TypeInfo idxType = lastExprType_;

        if (idxType.name != "int" && idxType.name != "u32" && idxType.name != "u64" && idxType.name != "i32" && idxType.name != "i64") {
            error("Array index must be an integer, got " + idxType.toString(), expr.getLine());
        }

        if (objType.isArray) {
            lastExprType_ = objType.elementType();
            // CIDER-BUG-069: an element of an interface array is a fat
            // pointer; resolve the flag by element name.
            resolveInterfaceFlag(lastExprType_);
        } else if (objType.pointerLevel > 0 || objType.name == "string") {
            lastExprType_ = objType;
            if (objType.pointerLevel > 0) {
                lastExprType_.pointerLevel--;
                // CIDER-BUG-199: `type A3 = [D; 3]; var a: *A3;` — the
                // language has no `*[T; N]` spelling, so a pointer to a
                // fixed array only appears through an alias. resolveType
                // stops at a pointer level, so the pointee is known to be an
                // array only after the level is dropped; `a[i]` then names
                // the array's ELEMENT, exactly like an array-valued base.
                // Without this the index kept the whole-array type and every
                // use was rejected ("Expected A3, got int"). Only the last
                // level reaches the pointee; a deeper one names a pointer,
                // whose element is the pointer itself.
                if (lastExprType_.pointerLevel == 0) {
                    TypeInfo pointee = resolveType(lastExprType_);
                    if (pointee.isArray) lastExprType_ = pointee.elementType();
                }
                resolveInterfaceFlag(lastExprType_);
            } else lastExprType_ = {"char", 0};
        } else {
            error("Cannot index non-array and non-pointer type " + objType.toString(), expr.getLine());
            lastExprType_ = {"error", 0};
        }
    }

    void visit(SliceExpr& expr) override {
        // CIDER-FEAT-005: slice bounds are not type-checked by the
        // pre-existing traversal (only the object is visited), so a
        // variable used solely as a bound must still count as read. Visit
        // the bounds with diagnostics suppressed and every piece of checker
        // state restored: references are recorded, type-checking is not
        // changed.
        if (warningsActive_ && (expr.getStart() || expr.getEnd())) {
            TypeInfo savedType = lastExprType_;
            int savedSliceBaseSize = lastSliceBaseSize_;
            int savedSuppressErrors = suppressErrors_;
            suppressErrors_ = savedSuppressErrors + 1;
            if (expr.getStart()) expr.getStart()->accept(*this);
            if (expr.getEnd()) expr.getEnd()->accept(*this);
            suppressErrors_ = savedSuppressErrors;
            lastExprType_ = savedType;
            lastSliceBaseSize_ = savedSliceBaseSize;
        }
        expr.getObject()->accept(*this);
        // CIDER-BUG-019: same alias resolution as visit(IndexExpr).
        TypeInfo objType = resolveType(lastExprType_);

        if (objType.isArray) {
            lastExprType_ = objType.elementType();
            lastExprType_.isSlice = true;
            // CIDER-BUG-057: remember the base array's element count so a
            // `base[start:]` slice's statically-known length can be checked
            // against a fixed array target (the end bound is the base size).
            lastSliceBaseSize_ = objType.arraySize;
        } else {
            lastSliceBaseSize_ = -1;
            error("Cannot slice non-array type " + objType.toString(), expr.getLine());
            lastExprType_ = {"error", 0};
        }
    }

    void visit(TernaryExpr& expr) override {
        expr.getCondition()->accept(*this);
        if (resolveType(lastExprType_).name != "bool" || lastExprType_.pointerLevel > 0) {
            error("Ternary condition must be a boolean, got " + lastExprType_.toString(), expr.getLine());
        }
        expr.getThenExpr()->accept(*this);
        TypeInfo thenType = lastExprType_;
        expr.getElseExpr()->accept(*this);
        if (!areTypesCompatible(thenType, lastExprType_)) {
            error("Ternary branches must have compatible types, got " + thenType.toString() +
                      " and " + lastExprType_.toString(),
                  expr.getLine());
        }
        // CIDER-BUG-140: remember each branch's own spelling so the
        // representation inference records the branch shape, not the
        // ternary's (a *void branch next to a closure branch must not be
        // read as the closure spelling).
        ternaryBranchTypes_[&expr] = {thenType, lastExprType_};
        lastExprType_ = thenType;
    }

    void visit(GroupingExpr& expr) override {
        expr.getExpression()->accept(*this);
    }

    void visit(FunctionExpr& expr) override {
        // Closure literal: check the body in a fresh scope with the params,
        // then type it as a function pointer. Captured identifiers resolve
        // in the enclosing scopes (they are read at the closure site).
        // CID-005 (T5/F1): a closure body has no declared return type of its
        // own, so the enclosing function's check must not apply inside it.
        TypeInfo prevReturnType = currentDeclaredReturnType_;
        bool prevHasReturnType = hasDeclaredReturnType_;
        hasDeclaredReturnType_ = false;
        // CIDER-BUG-210: a closure body is its own frame (see
        // visit(FunctionDeclStmt)), so `return` inside it stays legal even
        // when the literal is written in a defer body.
        int prevDeferBodyDepth = deferBodyDepth_;
        deferBodyDepth_ = 0;
        // CIDER-FEAT-005: a closure's declared return type is otherwise
        // never resolved; note it so a type-only import used here counts.
        noteTypeUse(expr.getReturnType());
        symbolTable_.enterScope();
        // CIDER-BUG-063: the literal's own signature, so a declared
        // `func(...)`/`fn(...)` type can check it exactly (and the closure
        // representation marker distinguishes it from a raw fn value).
        auto closureParams = std::make_shared<std::vector<TypeInfo>>();
        for (const auto& p : expr.getParams()) {
            auto sym = std::make_shared<Symbol>();
            sym->kind = SymbolKind::VARIABLE;
            sym->name = p.first;
            if (p.second) {
                sym->type = canonicalizeNominal(TypeInfo::fromAST(p.second.get()), expr.getLine());
                // CIDER-BUG-085: a closure parameter spelled with an
                // interface (or a fn signature containing one) carries the
                // same flags as any other declaration, so the literal's
                // signature compares equal to the declared spelling.
                resolveInterfaceFlag(sym->type);
            } else {
                sym->type = {"int", 0};
            }
            closureParams->push_back(sym->type);
            symbolTable_.define(sym);
            // CIDER-FEAT-005: closure params are parameters, not locals.
            if (warningsActive_) {
                warnings_.declare(symbolTable_.getCurrentScope(), sym, expr.getLine(),
                                  expr.getColumn(), /*warnUnused=*/false, /*checkShadow=*/false);
            }
        }
        for (auto& s : expr.getBody()) {
            s->accept(*this);
        }
        exitScopeChecked();
        currentDeclaredReturnType_ = prevReturnType;
        hasDeclaredReturnType_ = prevHasReturnType;
        deferBodyDepth_ = prevDeferBodyDepth;
        TypeInfo closureType;
        closureType.name = "func";
        closureType.pointerLevel = 1;
        closureType.isFunctionPointer = true;
        closureType.isClosure = true;
        closureType.fnParamTypes = std::move(closureParams);
        closureType.fnReturnType = std::make_shared<TypeInfo>(
            expr.getReturnType()
                ? canonicalizeNominal(TypeInfo::fromAST(expr.getReturnType()), expr.getLine())
                : TypeInfo{"int", 0});
        // CIDER-BUG-085: the closure's return spelling resolves its
        // interface flags like every other signature position.
        resolveInterfaceFlag(*closureType.fnReturnType);
        lastExprType_ = closureType;
    }
    void visit(TypeAST&) override {}
    void visit(StructDeclStmt& stmt) override {
        // CIDER-BUG-119: a function/block-local declaration is re-installed
        // in the checker's own scope here; the collector's symbol (layout
        // already finalized) is keyed by this AST node. Top-level
        // declarations are absent from the map and stay resolvable through
        // the module/root scope, exactly as before.
        if (auto la = localAggregates_.find(&stmt); la != localAggregates_.end())
            symbolTable_.define(la->second);
        // CIDER-FEAT-005: field type spellings are otherwise not resolved by
        // the checker; note them for the import bookkeeping. Template
        // structs disable the unused-import report like template functions.
        if (warningsActive_ && stmt.hasTemplateParams()) warnings_.suppressUnusedImports();
        for (const auto& field : stmt.getFields()) noteTypeUse(field.type.get());
        // CIDER-BUG-111: a plain struct's field annotation naming a
        // qualified template is resolved at its own line, so an unaccepted
        // spelling is diagnosed at the declaration like a body var decl
        // (CIDER-BUG-096). Template structs stay lazy like template
        // functions: their fields are concrete only after substitution.
        if (!stmt.hasTemplateParams()) {
            for (const auto& field : stmt.getFields()) {
                checkDeclaredTemplateType(field.type.get(),
                                          field.type ? field.type->getLine() : stmt.getLine());
            }
        }
    }
    void visit(EnumDeclStmt&) override {}
    void visit(TypeAliasStmt& stmt) override {
        // CIDER-FEAT-005: `type A = ImportedType;` aliases a type the
        // checker never resolves; note it so the import is not "unused".
        noteTypeUse(stmt.getTargetType());
        // CIDER-BUG-111: same declaration-site resolution as struct fields.
        checkDeclaredTemplateType(stmt.getTargetType(), stmt.getLine());
        // CIDER-BUG-138: a non-template target is resolved at its own line
        // too, so a module-qualified target naming a non-exported symbol
        // gets the standard visibility diagnostic here (CIDER-BUG-060/102)
        // instead of surfacing later as the generic "is not a struct or
        // class". Template-mentioning targets keep the CIDER-BUG-111 path
        // above; bare targets resolve silently.
        if (stmt.getTargetType()) {
            TypeInfo target = TypeInfo::fromAST(stmt.getTargetType());
            if (!mentionsTemplateType(target)) {
                (void)canonicalizeNominal(target, stmt.getLine());
            }
        }
    }

    void visit(ClassDeclStmt& stmt) override {
        // CIDER-BUG-119: see visit(StructDeclStmt) — install the
        // collector-finalized local class in the checker's live scope
        // before its members (`this` etc.) are checked.
        if (auto la = localAggregates_.find(&stmt); la != localAggregates_.end())
            symbolTable_.define(la->second);
        std::string prevClass = currentClassName_;
        currentClassName_ = stmt.getName();
        // CIDER-BUG-124: the `extends` spelling is a type use and is
        // canonicalized (and diagnosed) exactly like any other type
        // position: a module-qualified base must be reachable and exported
        // from here, and a misspelled qualifier/member is reported instead
        // of silently yielding a class with no base. Self-qualified bases
        // inside the owner module stay unrestricted (CIDER-BUG-060
        // visibility). This also marks the name used for the CIDER-FEAT-005
        // import bookkeeping.
        if (!stmt.getParentName().empty()) {
            (void)canonicalizeNominal(TypeInfo{stmt.getParentName(), 0}, stmt.getLine());
        }
        for (auto& member : stmt.getMembers()) {
            // Field declarations are validated but must not define symbols
            // in the enclosing scope (pre-existing leak fix): fields live in
            // the class's StructSymbol, so `var tag` in two classes — or in
            // a class and at global scope — never collides, and a bare `tag`
            // in a method cannot resolve to a phantom variable.
            bool prevField = inClassFieldDecl_;
            inClassFieldDecl_ = dynamic_cast<VarDeclStmt*>(member.get()) != nullptr;
            member->accept(*this);
            inClassFieldDecl_ = prevField;
        }
        currentClassName_ = prevClass;
    }

    void visit(ImportStmt&) override {}

    void visit(FromImportStmt& stmt) override {
        // CIDER-FEAT-005: record the from-import aliases. The collector has
        // already bound them in this scope, so they resolve here; the raw
        // lookup avoids marking the binding as used by its own declaration.
        if (!warningsActive_) return;
        for (const auto& name : stmt.getSymbols()) {
            auto sym = resolveScopedRaw(name);
            warnings_.recordImport(symbolTable_.getCurrentScope(), sym, name,
                                   stmt.getModuleName(), stmt.getLine(), stmt.getColumn());
        }
    }

    void visit(ExportStmt& stmt) override { if (auto inner = stmt.getStmt()) inner->accept(*this); }

    void visit(ExportExpressionStmt& stmt) override {
        // CIDER-FEAT-005: `export { x };` references x even though the
        // expression is never read; a re-exported import is not "unused".
        // CIDER-ISS-002: exporting is not a value read either.
        if (!warningsActive_) return;
        if (auto id = dynamic_cast<IdentifierExpr*>(stmt.getExpression())) {
            warnings_.markRead(resolveScopedRaw(id->getName()), stmt.getLine(), stmt.getColumn(),
                               /*checkUninit=*/false);
        }
    }

    void visit(InterfaceDeclStmt& stmt) override {
        // CIDER-FEAT-005: interface method signatures are otherwise not
        // resolved by the checker; note them for the import bookkeeping.
        if (!warningsActive_) return;
        for (const auto& method : stmt.getMethods()) {
            for (const auto& param : method.params) noteTypeUse(param.second.get());
            noteTypeUse(method.returnType.get());
        }
    }

    void visit(ForEachStmt& stmt) override {
        stmt.getIterable()->accept(*this);
        // CIDER-BUG-052: an alias spelling of an array/iterator type must
        // present its resolved shape (the custom-iterator branch below casts
        // the scoped symbol to a StructSymbol, which an alias provider is
        // not).
        TypeInfo iterableType = resolveType(lastExprType_);

        TypeInfo elemType;
        if (iterableType.isArray) {
            elemType = iterableType.elementType();
        } else if (iterableType.isSlice) {
            elemType = iterableType;
            elemType.isSlice = false;
        } else if (iterableType.pointerLevel == 0 &&
                   (resolveScoped(iterableType.name) &&
                    (resolveScoped(iterableType.name)->kind == SymbolKind::STRUCT ||
                     resolveScoped(iterableType.name)->kind == SymbolKind::CLASS))) {
            // Custom iterator protocol: iter() -> It, next(self: It) -> *T.
            auto sym = resolveScoped(iterableType.name);
            auto ss = std::static_pointer_cast<StructSymbol>(sym);
            const StructSymbol::Method* iterM = nullptr;
            const StructSymbol::Method* nextM = nullptr;
            for (const auto& m : ss->methods) {
                if (m.name == "iter") iterM = &m;
                else if (m.name == "next") nextM = &m;
            }
            if (!iterM || !nextM) {
                error("For-in over '" + iterableType.name + "' requires iter() and next() methods.", stmt.getLine());
                return;
            }
            if (nextM->returnType.pointerLevel < 1) {
                error("Iterator's next() must return a pointer, got " + nextM->returnType.toString(), stmt.getLine());
                return;
            }
            elemType = nextM->returnType;
            elemType.pointerLevel--;
            // The pointee's user-defined flag must reflect reality: an int
            // iterator element is not a struct (the flag would route the
            // loop variable through getAddress instead of a value load).
            if (elemType.pointerLevel == 0 && !elemType.isArray) {
                auto ts = resolveScoped(elemType.name);
                if (!ts || (ts->kind != SymbolKind::STRUCT && ts->kind != SymbolKind::CLASS)) {
                    elemType.isUserDefined = false;
                }
            }
        } else {
            error("For-in iterable must be an array, slice, or a type with iter()/next(), got " + iterableType.toString(), stmt.getLine());
            return;
        }

        symbolTable_.enterScope();
        auto sym = std::make_shared<Symbol>();
        sym->kind = SymbolKind::VARIABLE;
        sym->name = stmt.getVarName();
        sym->type = elemType;
        sym->line = stmt.getLine();
        sym->col = stmt.getColumn();
        symbolTable_.define(sym);
        // CIDER-FEAT-005: the loop variable is a local like any other.
        if (warningsActive_) {
            warnings_.declare(symbolTable_.getCurrentScope(), sym, stmt.getLine(),
                              stmt.getColumn(), /*warnUnused=*/true, /*checkShadow=*/true);
        }

        for (auto& s : stmt.getBody()) {
            s->accept(*this);
        }
        exitScopeChecked();
    }

    void visit(DeleteStmt& stmt) override {
        stmt.getTarget()->accept(*this);
        if (lastExprType_.pointerLevel == 0 && lastExprType_.name != "wildcard") {
            error("Cannot delete non-pointer type " + lastExprType_.toString(), stmt.getLine());
        }
    }

    // CIDER-BUG-210: see rejectDeferTransfer. Both are accepted anywhere
    // else (ir_gen drops a break/continue with no enclosing loop), but
    // inside a `defer` body they reach the unwinder and re-enter it.
    void visit(BreakStmt& stmt) override {
        rejectDeferTransfer("break", stmt.getLine());
    }
    void visit(ContinueStmt& stmt) override {
        rejectDeferTransfer("continue", stmt.getLine());
    }

    void visit(ThrowStmt& stmt) override {
        // CIDER-BUG-210: see rejectDeferTransfer.
        if (rejectDeferTransfer("throw", stmt.getLine())) return;
        stmt.getValue()->accept(*this);
    }

    void visit(TryCatchStmt& stmt) override {
        symbolTable_.enterScope();
        for (auto& s : stmt.getTryBlock()) {
            s->accept(*this);
        }
        exitScopeChecked();

        symbolTable_.enterScope();
        auto catchVarSym = std::make_shared<Symbol>();
        catchVarSym->kind = SymbolKind::VARIABLE;
        catchVarSym->name = stmt.getCatchVar();
        catchVarSym->type = {"int", 0};
        symbolTable_.define(catchVarSym);
        // CIDER-FEAT-005: the catch binding is a local declaration too.
        if (warningsActive_) {
            warnings_.declare(symbolTable_.getCurrentScope(), catchVarSym, stmt.getLine(),
                              stmt.getColumn(), /*warnUnused=*/true, /*checkShadow=*/true);
        }
        for (auto& s : stmt.getCatchBlock()) {
            s->accept(*this);
        }
        exitScopeChecked();
    }

    void visit(DeferStmt& stmt) override {
        // CIDER-BUG-210: the body is still inside the defer, so a `defer`
        // written in a defer body does not clear the flag.
        ++deferBodyDepth_;
        stmt.getDeferredStmt()->accept(*this);
        --deferBodyDepth_;
    }

    void visit(AsmStmt& stmt) override {
        // CIDER-FEAT-005: inline asm references locals by name through
        // `{x}` placeholders; mark every mentioned variable as a use so an
        // asm-only variable is not reported unused. IrGen strips the same
        // `*` deref marker and `:width` suffix. CIDER-ISS-002: a placeholder
        // may be an output operand, so asm counts as an escape/possible
        // initialization rather than a value read.
        if (!warningsActive_) return;
        for (const auto& entry : stmt.localVariables) {
            std::string name = entry.first.empty() ? entry.second : entry.first;
            if (!name.empty() && name[0] == '*') name = name.substr(1);
            size_t colon = name.find(':');
            if (colon != std::string::npos) name = name.substr(0, colon);
            if (name.empty()) continue;
            auto sym = resolveScopedRaw(name);
            warnings_.markRead(sym, stmt.getLine(), stmt.getColumn(), /*checkUninit=*/false);
            warnings_.markAssigned(sym);
        }
    }

    void visit(MatchStmt& stmt) override {
        stmt.getExpression()->accept(*this);
        TypeInfo matchType = lastExprType_;

        for (auto& arm : stmt.getArms()) {
            // The `default` arm matches everything.
            bool isDefault = false;
            if (auto pid = dynamic_cast<IdentifierExpr*>(arm.pattern.get())) {
                isDefault = (pid->getName() == "default");
            }
            if (!isDefault) {
                arm.pattern->accept(*this);
                if (!areTypesCompatible(matchType, lastExprType_)) {
                    error("Match pattern type mismatch. Expected " + matchType.toString() +
                              ", got " + lastExprType_.toString(),
                          arm.pattern->getLine());
                }
            }

            symbolTable_.enterScope();
            arm.body->accept(*this);
            exitScopeChecked();
        }

        // CIDER-FEAT-002: opt-in exhaustiveness warning (see helper).
        if (warningsActive_) warnMissingEnumVariants(stmt, matchType);
    }

    // CIDER-FEAT-002: with `--warn`, a match over an enum that has no
    // `default` arm and does not statically cover every variant reports one
    // warning per missing variant. A pattern that is not a compile-time
    // constant (a variable, a call) could match anything, so the analysis
    // stays silent instead of risking a false positive; a `default` arm is
    // exhaustive by construction. Variants are identified by value, so
    // explicit-value enums stay correct once the parser grows them.
    void warnMissingEnumVariants(MatchStmt& stmt, const TypeInfo& matchType) {
        auto enumSym = resolveEnumSymbol(matchType);
        if (!enumSym) return;
        std::set<int64_t> covered;
        for (auto& arm : stmt.getArms()) {
            if (auto pid = dynamic_cast<IdentifierExpr*>(arm.pattern.get())) {
                if (pid->getName() == "default") return;
            }
            int64_t value = 0;
            if (!evalConstIntBound(arm.pattern.get(), value)) return;
            covered.insert(value);
        }
        for (auto& variant : enumSym->variants) {
            if (!covered.count(variant.value)) {
                reporter_.warning("match on enum '" + enumSym->name +
                                      "' does not handle variant '" + variant.name + "'",
                                  stmt.getLine());
            }
        }
    }

private:
    SymbolTable& symbolTable_;
    ErrorReporter& reporter_;
    // CIDER-FEAT-005: opt-in warning bookkeeping (see setWarningsEnabled).
    WarningTracker warnings_;
    bool warningsEnabled_ = false;
    // True while the root program's statements are being visited; imports
    // and module bodies are checked without warning analysis.
    bool warningsActive_ = false;
    // CIDER-BUG-115: lifetime bookkeeping for interface stores. Symbols
    // defined by the function body currently being checked (parameters and
    // locals), and local pointers whose value is known through `&local`
    // provenance to address that same frame. Both are saved/cleared/
    // restored per function (see visit(FunctionDeclStmt)).
    std::set<uint64_t> frameLocalSymbols_;
    std::set<uint64_t> frameLocalPointers_;
    // Set while visiting a bare assignment target (`x = v`), where the
    // identifier is written but not read.
    bool suppressIdentRead_ = false;
    // >0 while walking expressions the checker normally never type-checks
    // (slice bounds): the walk is for warning bookkeeping only, so any
    // diagnostic it would raise must stay silent.
    int suppressErrors_ = 0;
    // CIDER-BUG-210: how many enclosing `defer` bodies the statement being
    // checked sits in. A `return`/`throw`/`break`/`continue` written there
    // transfers control out of the very emission that is running the defer,
    // so ir_gen's unwinder re-entered the statement it was emitting and
    // recursed until the compiler's stack was gone (SIGSEGV). The checker
    // rejects those shapes where it can still print a diagnostic; cleared
    // at every function-body boundary (visit(FunctionDeclStmt),
    // visit(FunctionExpr)) because a nested function or closure is its own
    // frame and ir_gen gives it a fresh defer stack.
    int deferBodyDepth_ = 0;
    TypeInfo lastExprType_ = {"void", 0};
    // CIDER-BUG-057: array size of the object of the most recently checked
    // SliceExpr, used to statically evaluate a `base[start:]` slice's end
    // bound. -1 == no fixed-size base available.
    int lastSliceBaseSize_ = -1;
    // CID-005 (T5/F1): declared return type of the function declaration
    // whose body is being checked (guarded: no function body == no check).
    TypeInfo currentDeclaredReturnType_ = {"void", 0};
    bool hasDeclaredReturnType_ = false;
    std::string currentClassName_;
    bool inClassFieldDecl_ = false;
    bool moduleMode_ = false;

    // CID-020/021 module context (see setters above).
    std::map<std::string, Scope*> moduleScopes_;  // resolved path -> module scope
    std::map<const AstProgram*, std::string> programModules_;  // imported AST -> logical name
    std::map<std::string, std::vector<std::shared_ptr<ModuleSymbol>>> ambiguousModules_;
    std::map<uint64_t, std::string> symbolModules_;  // symbol identity -> defining module
    // CIDER-BUG-073: accepted qualified template spellings -> shared bare
    // instantiation spelling (see setter above).
    std::map<std::string, std::string> qualifiedTemplateInstantiations_;
    // CIDER-BUG-136: (spelling context module, spelling) -> per-owner
    // canonical instantiation identity (see setter above).
    std::map<std::string, std::string> templateInstanceIdentities_;
    // CIDER-BUG-096: logical module ("" == main) -> template base names the
    // module declares (see setter above).
    std::map<std::string, std::set<std::string>> moduleTemplateNames_;
    // CIDER-BUG-134: whether the collector supplied moduleTemplateNames_
    // (unit tests do not). The bare-unknown-template diagnostic needs the
    // table to tell "unknown" from "declared but not accepted"; unset keeps
    // the legacy silence.
    bool moduleTemplateNamesSet_ = false;
    // CIDER-BUG-119: function/block-local aggregate AST -> collector symbol
    // (see setter above).
    std::map<const Statement*, std::shared_ptr<StructSymbol>> localAggregates_;
    std::string currentModule_;      // logical name of the file being checked ("" == main)
    Scope* currentModuleScope_ = nullptr;

    // CID-005 target flags (see setters above).
    bool defaultAbiMs_ = false;
    bool uefiEntryAllowed_ = false;

    void error(const std::string& msg, int line) {
        if (suppressErrors_ > 0) return;
        reporter_.error(msg, line);
    }

    // CIDER-BUG-210: a control transfer written in a `defer` body leaves the
    // defer's own exit sequence unfinished, and ir_gen's unwinder then
    // re-entered the very statement it was emitting (unbounded recursion,
    // see ir_gen.h emitDefers). The rule is lexical: a transfer anywhere
    // inside a defer body is rejected, however deeply nested in blocks,
    // loops or further defers, because every one of those emits through the
    // same unwinder. Returns true when the statement was rejected, so the
    // callers skip the remaining (moot) checks instead of piling on
    // diagnostics about a value that is never used.
    bool rejectDeferTransfer(const char* keyword, int line) {
        if (deferBodyDepth_ == 0) return false;
        error("'" + std::string(keyword) + "' is not allowed in a defer body", line);
        return true;
    }

    // CIDER-BUG-068 representation inference state (slot 0 is a sentinel so
    // a valid slot index is never 0). See the helpers near
    // resolveFunctionPointerAlias for the design.
    std::map<std::string, int> fnSlotIndex_;
    std::vector<int> fnSlotParent_ = {0};
    std::vector<FnRepr> fnSlotPolarity_ = {FnRepr::Unknown};
    std::vector<std::string> fnSlotLabel_ = {""};
    std::vector<int> fnSlotLine_ = {0};
    std::vector<std::string> fnSlotKey_ = {""};
    // CIDER-BUG-118: provenance-only slot (a *void storage/source). Mixing
    // shapes there resolves to Mixed instead of a conflict, and the slot is
    // never annotated on a TypeAST. A rigid slot is a fn(...) declaration or
    // parameter whose representation is fixed for the program: mixing in a
    // component that contains one stays a CIDER-BUG-068 conflict.
    std::vector<uint8_t> fnSlotVoid_ = {0};
    std::vector<uint8_t> fnSlotRigid_ = {0};
    // Cast-check slots whose source chain reaches a tracked *void storage
    // location: the post-pass rejects an unresolved shape for them
    // (CIDER-BUG-118); other cast slots keep the CIDER-BUG-093 leniency.
    std::vector<uint8_t> fnSlotMustProve_ = {0};
    // CIDER-BUG-140: whether a check slot's source was a *void carrier (the
    // CIDER-BUG-118 diagnostic wording) and, for the other sources, the
    // source spelling used by the CIDER-BUG-140 typed-pointer diagnostic.
    std::vector<uint8_t> fnSlotVoidSource_ = {0};
    std::vector<std::string> fnSlotSourceSpelling_ = {""};
    // Parameter slot key -> declared TypeAST (annotated on resolution).
    std::map<std::string, TypeAST*> fnParamAst_;
    // Variable slot key -> declared TypeAST, for declarations with no
    // representation-bearing initializer (annotated when the slot resolves
    // to closure through a later assignment).
    std::map<std::string, TypeAST*> fnVarAst_;
    // Cast expression -> its source type, so the representation inference
    // can follow a closure object through `closure as fn(...)`.
    std::map<const CastExpr*, TypeInfo> castSourceTypes_;
    // CIDER-BUG-140: ternary expression -> its two branch types, so the
    // inference records each branch under its own spelling instead of the
    // ternary's result spelling.
    std::map<const TernaryExpr*, std::pair<TypeInfo, TypeInfo>> ternaryBranchTypes_;
    // Parameter symbol id -> its slot key (forwarding chains).
    std::map<uint64_t, std::string> fnParamSymKey_;
    // Return-site check slots (key prefix "x:"): a return from a function
    // declared to return a plain `fn(...)` value that resolves to a closure
    // is reported after the walk.
    int fnReturnCheckCount_ = 0;
    // Field/element/pointee store sites (key prefix "s:"): that storage
    // keeps raw fn pointers, so a closure value is reported after the walk.
    int fnStoreCheckCount_ = 0;
    // Cast-to-closure sites (key prefix "c:"): the source shape resolves
    // through the same inference; a plain-resolved source is reported after
    // the walk (CIDER-BUG-093), and an unresolved source that reached a
    // tracked *void storage location is reported too (CIDER-BUG-118).
    int fnCastCheckCount_ = 0;
    // CIDER-BUG-140: cast-free conversions into a closure spelling (key
    // prefix "q:"): the same verdict as a cast, reported with conversion
    // wording.
    int fnConvertCheckCount_ = 0;
    // CIDER-BUG-140: per-branch ternary child slots (key prefix "b:"). They
    // carry a branch's shape into the ternary's slot and are not storage.
    int fnBranchCheckCount_ = 0;
    // Name of the function whose body is being checked (diagnostics).
    std::string currentFunctionName_;

    // CIDER-BUG-184: set when an interface-typed return boxes a concrete
    // struct/class value (see hasInterfaceBoxing).
    bool interfaceBoxing_ = false;

    // CIDER-FEAT-005: close a checker scope, reporting its unused locals
    // first. Every symbolTable_.exitScope() in this class goes through here.
    void exitScopeChecked() {
        if (warningsActive_) warnings_.onScopeExit(symbolTable_.getCurrentScope());
        symbolTable_.exitScope();
    }

    // CIDER-FEAT-005: resolve a declared type only to record import usage
    // (silent: line < 0 keeps unresolvable spellings from adding
    // diagnostics). The result is discarded; type-checking is unchanged.
    void noteTypeUse(TypeAST* type) {
        if (!warningsActive_ || !type) return;
        (void)canonicalizeNominal(TypeInfo::fromAST(type));
    }

    // CIDER-BUG-111: true when a resolved annotation mentions an explicit
    // template argument list anywhere (`m.Box<int>`, `*Box<int>`,
    // `[Pair<int, char>; 2]`, `fn(*m.Box<int>) -> void`). Non-template
    // spellings are excluded so struct-field and alias resolution stays as
    // lazy as before for everything else.
    static bool mentionsTemplateType(const TypeInfo& type) {
        if (type.isFunctionPointer) {
            if (type.fnParamTypes) {
                for (const auto& p : *type.fnParamTypes) {
                    if (mentionsTemplateType(p)) return true;
                }
            }
            return type.fnReturnType && mentionsTemplateType(*type.fnReturnType);
        }
        if (type.arrayElement && mentionsTemplateType(*type.arrayElement)) return true;
        return type.name.find('<') != std::string::npos;
    }

    // CIDER-BUG-111: resolve a struct-field or alias annotation at its own
    // line so an unaccepted qualified template spelling (`m.Nope<int>`) is
    // diagnosed at the declaration, exactly like a body var decl
    // (CIDER-BUG-096). Accepted spellings were rewritten through
    // qualifiedTemplateInstantiations_ by the collector's whole-program
    // pass, so this call is silent for them; anything without an argument
    // list keeps its previous (lazy) behavior.
    void checkDeclaredTemplateType(TypeAST* type, int line) {
        if (!type) return;
        TypeInfo t = TypeInfo::fromAST(type);
        if (!mentionsTemplateType(t)) return;
        (void)canonicalizeNominal(t, line);
    }
};

}  // namespace cider
