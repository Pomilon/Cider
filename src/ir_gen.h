#pragma once

#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include <cstring>

#include "ast.h"
#include "errors.h"
#include <algorithm>
#include "ir.h"
#include "module_scope.h"
#include "symbols.h"

namespace cider {

class IRGenerator : public ASTVisitor {
public:
    IRGenerator(SymbolTable& syms, const std::string& sourcePath = "")
        : symbolTable_(syms), currentSourcePath_(sourcePath) {}

    // CIDER-BUG-024: optional diagnostic sink. The driver passes its
    // reporter so ir_gen can hard-error instead of silently emitting a
    // named load when a nominal type cannot be resolved; unit tests
    // without a reporter keep the legacy path.
    void setReporter(ErrorReporter* rep) { reporter_ = rep; }

    // CID-020: module scopes keyed by resolved file path, from the
    // collector. Lets each imported AST generate with its own scope current
    // (spec §2). Empty by default: unit tests without a collector keep the
    // legacy always-root behavior.
    void setModuleScopes(const std::map<std::string, Scope*>& scopes) { moduleScopes_ = scopes; }
    // CID-020/021: imported AST -> logical module name, from the collector
    // (mirrors TypeChecker::setProgramModules). Lets emission mangle by the
    // DEFINING module. Empty by default: legacy always-bare behavior.
    void setProgramModules(const std::map<const AstProgram*, std::string>& programModules) {
        programModules_ = programModules;
    }
    // CID-020/021: defined symbol -> defining module logical, from the
    // collector (bind-time provenance; aliases share the target object, so
    // chained aliases resolve to the origin module, not path-order luck).
    // Empty by default: legacy scope-identity search behavior.
    void setSymbolModules(const std::map<uint64_t, std::string>& symbolModules) {
        symbolModules_ = symbolModules;
    }

    // CID-005: `--target uefi` types unmarked function identifiers as
    // MS-ABI fn values (mirrors the checker's functionPointerTypeOf).
    void setDefaultAbiMs(bool v) { defaultAbiMs_ = v; }

    ModuleIR generate(AstProgram& program) {
        visitedPrograms_.clear();
        classSyms_.clear();
        structSyms_.clear();
        ifaceSyms_.clear();
        globalValues_.clear();
        exceptionGlobalsEnsured_ = false;
        interpHelpersEnsured_ = false;
        program.accept(*this);

        // Populate V-Tables in module (CID-020/021 spec §6: module-scope
        // classes resolve to their own objects and mangle keys by defining
        // module, so qualified `new` and virtual dispatch find them;
        // same-named classes in two modules mangle apart instead of
        // colliding).
        for (auto& classSym : classSyms_) {
            if (!classSym || classSym->kind != SymbolKind::CLASS) continue;
            if (classSym->vtable.empty()) continue;
            Scope* classScope = definingScopeFor(classSym, classSym->name);
            std::vector<std::string> entries;
            for (const auto& e : classSym->vtable) {
                auto methodDef = resolveFunctionDef(e, classScope);
                entries.push_back(emitFunctionName(methodDef.second, e));
            }
            module_.addVTable(vtableKeyForClass(classSym), entries);
        }

        // Generate interface vtables: for each interface-struct pair.
        // CID-005: interface dispatch lowers to SysV calls (T3 pins the
        // interface branch; MS-ABI interface/virtual dispatch is out of
        // scope), but interface implementations are ordinary unmarked
        // methods. Their definitions must therefore be pinned to SysV, or
        // `--target uefi` would emit an MS prologue while every interface
        // call marshals SysV. The exact set is collected while the vtables
        // are built below, then applied to both the FunctionIR (definition
        // emission) and its FunctionSymbol (direct call sites read `abi`).
        std::set<std::string> ifaceDispatchedNames;
        for (auto& iface : ifaceSyms_) {
            if (!iface) continue;
            // Check all classes/structs that might implement this interface
            std::vector<std::shared_ptr<StructSymbol>> allStructClasses = classSyms_;
            for (auto& sn : structSyms_) {
                bool found = false;
                for (auto& cn : classSyms_) {
                    if (cn == sn) {
                        found = true;
                        break;
                    }
                }
                if (!found) allStructClasses.push_back(sn);
            }
            for (auto& ss : allStructClasses) {
                if (!ss) continue;
                bool satisfies = true;
                for (auto& im : iface->methods) {
                    bool found = false;
                    for (auto& sm : ss->methods) {
                        if (sm.name != im.name) continue;
                        if (sm.parameterTypes.size() != im.parameterTypes.size()) continue;
                        bool paramsMatch = true;
                        for (size_t pi = 0; pi < sm.parameterTypes.size(); ++pi) {
                            if (!(sm.parameterTypes[pi] == im.parameterTypes[pi])) {
                                paramsMatch = false;
                                break;
                            }
                        }
                        if (!paramsMatch) continue;
                        if (!(sm.returnType == im.returnType)) continue;
                        found = true;
                        break;
                    }
                    if (!found) {
                        satisfies = false;
                        break;
                    }
                }
                if (satisfies && !iface->methods.empty()) {
                    std::vector<std::string> vtableMethods;
                    for (auto& im : iface->methods) {
                        for (auto& sm : ss->methods) {
                            if (sm.name == im.name) {
                                // CID-020/021 (spec §5/§6): entries mangle
                                // with the struct's defining module.
                                auto methodDef = resolveFunctionDef(
                                    sm.mangledName, definingScopeFor(ss, ss->name));
                                vtableMethods.push_back(
                                    emitFunctionName(methodDef.second, sm.mangledName));
                                // CID-005: this definition is dispatched
                                // through the SysV-only interface path.
                                ifaceDispatchedNames.insert(vtableMethods.back());
                                if (methodDef.first) methodDef.first->abi = Abi::SysV;
                                break;
                            }
                        }
                    }
                    module_.addVTable(vtableIfaceKey(iface, iface->name, ss, ss->name), vtableMethods);
                }
            }
        }

        // CID-005: apply the interface-dispatch pinning collected above to
        // the generated definitions (see the comment on ifaceDispatchedNames).
        // Definitions may be emitted before or after the vtable analysis, so
        // this is post-accept; changed under native/pomeos only from
        // Default -> SysV, which resolve to the same convention.
        for (auto& fn : module_.getFunctions()) {
            if (ifaceDispatchedNames.count(fn->getName())) fn->setAbi(Abi::SysV);
        }

        return std::move(module_);
    }

    void startBlock(const std::string& name) {
        if (currentBlock_) {
            currentFunction_->addBlock(std::move(currentBlock_));
        }
        currentBlock_ = std::make_unique<BasicBlock>(name);
    }

    struct DestructorInfo {
        std::string mangledName;
        IRValue varAddr;
    };

    // CIDER-BUG-050: a `defer` runs when the lexical block it is written in
    // exits, so emission unwinds from the innermost scope down to (and
    // including) `firstScope`, each scope in reverse order of registration
    // (LIFO). Emitting leaves the scopes on the stack: the construct owning
    // a scope pops it when its block ends. firstScope == 0 unwinds every
    // pending defer, which is what `return` and function fallthrough need.
    void emitDefers(size_t firstScope) {
        for (size_t i = deferStack_.size(); i > firstScope; --i) {
            for (size_t j = deferStack_[i - 1].size(); j > 0; --j) {
                // Re-read the slot on every step: a deferred statement may
                // itself be a block, whose accept pushes/pops a scope (and
                // may reallocate the outer vector).
                Statement* s = deferStack_[i - 1][j - 1];
                s->accept(*this);
            }
        }
    }

    void emitDefers() { emitDefers(0); }

    void emitDestructors(size_t firstScope) {
        for (size_t i = destructorStack_.size(); i > firstScope; --i) {
            for (size_t j = destructorStack_[i - 1].size(); j > 0; --j) {
                const DestructorInfo& d = destructorStack_[i - 1][j - 1];
                IRInstruction call;
                call.op = IROp::CALL;
                call.dest = std::nullopt;
                call.args.push_back(IRValue::Label(d.mangledName));
                call.args.push_back(d.varAddr);
                currentBlock_->addInstruction(call);
            }
        }
    }

    void emitDestructors() { emitDestructors(0); }

    // CID-006 fix round 3: the alias walk is cycle-safe. `visiting` is a
    // per-call visited set; a repeated spelling (the checker accepts cyclic
    // aliases) falls back to PTR, the same result as an unknown name, so
    // `type A = B; type B = A;` uses terminate instead of overflowing the
    // stack. Acyclic spellings resolve exactly as before.
    IRType mapTypeImpl(const std::string& name, std::set<std::string>& visiting) {
        if (name == "i8") return IRType::I8;
        if (name == "u8" || name == "byte" || name == "char") return IRType::U8;
        if (name == "i16") return IRType::I16;
        if (name == "u16") return IRType::U16;
        if (name == "i32" || name == "int") return IRType::I32;
        if (name == "u32") return IRType::U32;
        if (name == "i64") return IRType::I64;
        if (name == "u64") return IRType::U64;
        if (name == "float" || name == "f32") return IRType::F32;
        if (name == "f64") return IRType::F64;
        if (name == "bool") return IRType::I32;
        if (name == "void") return IRType::VOID;
        // Resolve type aliases through symbol table
        if (name.empty() || !visiting.insert(name).second) return IRType::PTR;
        auto typeSym = symbolTable_.resolve(name);
        if (typeSym && typeSym->type.pointerLevel == 0 && !typeSym->type.name.empty()) {
            std::string targetName = typeSym->type.name;
            if (targetName != name) {
                IRType resolved = mapTypeImpl(targetName, visiting);
                if (resolved != IRType::PTR) return resolved;
            }
        }
        return IRType::PTR;
    }

    IRType mapType(const std::string& name) {
        std::set<std::string> visiting;
        return mapTypeImpl(name, visiting);
    }

    // CID-006 (Task 2 F1 carry-over, fix round 2): resolve a type spelling
    // through `type X = Y` aliases to a fixed point, mirroring
    // type_checker::resolveType. The collector stores aliases as STRUCT-kind
    // providers; a fn(...) target keeps its signature payload. Each hop
    // resolves through the nominal lookup (dotted/canonical names) plus the
    // callee defining-module fallback, so chains in callee signatures resolve
    // too. `visiting` is a per-resolution visited set: the checker accepts
    // cyclic aliases, and stopping at the repeated spelling keeps every
    // caller terminating (getTypeSize re-resolves its result, so a cap that
    // returned the other alias made the pair recurse without bound).
    TypeInfo resolveTypeAliasImpl(TypeInfo t, const std::string& defModule,
                                  std::set<std::string>& visiting) {
        if (t.pointerLevel > 0 || t.isFunctionPointer || t.isArray) return t;
        if (t.name.empty() || !visiting.insert(t.name).second) return t;
        auto sym = resolveNominalType(t.name);
        if (!sym && !defModule.empty()) sym = resolveCalleeNominal(t.name, defModule);
        if (!sym || sym->kind != SymbolKind::STRUCT || sym->type.name == t.name ||
            sym->type.name.empty()) {
            return t;
        }
        // CIDER-BUG-029: the target spelling belongs to the alias's
        // defining module; re-root it there so a cross-module
        // `type X = othermod.S` resolves at use sites that do not import S.
        TypeInfo target = canonicalizeNominalInModule(sym->type, definingModuleFor(sym, t.name));
        TypeInfo resolved = resolveTypeAliasImpl(target, defModule, visiting);
        resolved.pointerLevel += t.pointerLevel;
        return resolved;
    }

    TypeInfo resolveTypeAlias(TypeInfo t, const std::string& defModule = "") {
        std::set<std::string> visiting;
        return resolveTypeAliasImpl(t, defModule, visiting);
    }

    // CIDER-BUG-015: `type X = T` providers are STRUCT-kind plain Symbols,
    // not StructSymbols. Every StructSymbol cast must first follow the alias
    // chain (cycle-safe via resolveTypeAlias) and reject providers
    // (`type.name` non-empty), or member access through an alias-typed
    // object is undefined behavior and alias-typed FIELDS get classified
    // as by-value structs (address emitted instead of a typed load).
    // A pointer level does not change the nominal identity (callers
    // decrement for deref contexts; the lookup below is by name), so strip
    // it before resolving — mirroring the legacy by-name lookup.
    std::shared_ptr<Symbol> resolveStructOrClassSymbol(TypeInfo t, const std::string& defModule = "") {
        t.pointerLevel = 0;
        TypeInfo resolved = resolveTypeAlias(t, defModule);
        auto sym = resolveNominalType(resolved.name);
        if (!sym && !defModule.empty()) sym = resolveCalleeNominal(resolved.name, defModule);
        if (!sym || (sym->kind != SymbolKind::STRUCT && sym->kind != SymbolKind::CLASS)) return nullptr;
        if (!sym->type.name.empty()) return nullptr;  // alias provider / self-alias stop
        return sym;
    }

    // CID-006 (Task 2 F1 carry-over): the ir_gen half of the checker's
    // resolveFunctionPointerAlias gate. Returns the resolved fn(...)
    // signature when the spelling aliases one, else the original TypeInfo
    // (non-fn aliases and arrays keep their declared spelling, so legacy
    // paths are untouched). Every fn-specific gate must go through here:
    // alias-typed symbols carry the alias NAME, not the signature.
    TypeInfo resolveFunctionPointerAlias(TypeInfo t) {
        if (t.isFunctionPointer || t.isArray) return t;
        TypeInfo resolved = resolveTypeAlias(t);
        return resolved.isFunctionPointer ? resolved : t;
    }

    // CID-006: the label a global function-pointer initializer folds to.
    // Returns "" for nilPtr / no initializer (the cell stays zero).
    std::string functionPointerInitTarget(Expression* init) {
        // CIDER-BUG-024: `add as *void` / `add as fn(...)` spellings carry
        // the same constant as the bare name; unwrap grouping and casts so
        // the global folds to the label instead of silently BSS-zeroing.
        while (true) {
            if (auto grp = dynamic_cast<GroupingExpr*>(init)) { init = grp->getExpression(); continue; }
            if (auto cast = dynamic_cast<CastExpr*>(init)) { init = cast->getExpression(); continue; }
            break;
        }
        if (auto id = dynamic_cast<IdentifierExpr*>(init)) {
            auto sym = symbolTable_.resolve(id->getName());
            if (!sym || sym->kind != SymbolKind::FUNCTION) return "";
            return emitFunctionName(definingModuleFor(sym, id->getName()), sym->name);
        }
        // CIDER-BUG-023: a module-qualified function value (`b.inc`) folds
        // to the member's mangled label, same as the bare name.
        if (auto mem = dynamic_cast<MemberAccessExpr*>(init)) {
            if (!mem->isArrow()) {
                if (auto base = dynamic_cast<IdentifierExpr*>(mem->getObject())) {
                    auto baseSym = symbolTable_.resolve(base->getName());
                    if (baseSym && baseSym->kind == SymbolKind::MODULE) {
                        auto modSym = std::static_pointer_cast<ModuleSymbol>(baseSym);
                        Scope* ms = modSym->scope;
                        auto target = ms ? ms->resolveLocal(mem->getMember()) : nullptr;
                        if (target && target->kind == SymbolKind::FUNCTION) {
                            return emitFunctionName(definingModuleFor(target, mem->getMember()),
                                                    target->name);
                        }
                    }
                }
            }
        }
        return "";
    }

    // True for struct/class by-value returns (and params decaying likewise).
    // These use the hidden-sret-pointer convention so freestanding targets
    // never need a heap `malloc` for `return someStruct;`. `defModule`
    // scopes bare callee-signature nominals to the callee's defining module
    // when they do not resolve at the use site (empty = legacy walk only,
    // which is what every pre-existing call site passes).
    bool isStructValueType(const TypeInfo& t, const std::string& defModule = "") {
        if (t.pointerLevel > 0) return false;
        if (t.isArray || t.isInterface || t.isSlice || t.isFunctionPointer) return false;
        // CID-006 fix round 1 (F1): resolve `type A = B` alias CHAINS to a
        // fixed point before the STRUCT-kind test. One hop was not enough: a
        // chain ending in a fn(...) target kept the STRUCT-kind alias symbol
        // and classified the value as a by-value struct, so a local copy
        // lowered to a MEMCPY from the value-as-address (native SIGSEGV).
        TypeInfo resolved = resolveTypeAlias(t, defModule);
        if (resolved.isFunctionPointer || resolved.pointerLevel > 0 || resolved.isArray ||
            resolved.isInterface || resolved.isSlice) {
            return false;
        }
        auto sym = resolveNominalType(resolved.name);
        if (!sym && !defModule.empty()) sym = resolveCalleeNominal(resolved.name, defModule);
        if (!sym) return false;
        if (sym->kind != SymbolKind::STRUCT && sym->kind != SymbolKind::CLASS) return false;
        // A STRUCT-kind symbol that still provides another name (alias
        // provider, including a self-alias the fixed point stops on) is not
        // a by-value nominal.
        return sym->type.name.empty();
    }

    // CIDER-BUG-045: fixed-size array values are by-value aggregates too —
    // they return through the same hidden-sret convention (the caller slot
    // outlives the frame). Slices/interfaces/fn values are not.
    bool isByValueAggregate(const TypeInfo& t, const std::string& defModule = "") {
        if (t.pointerLevel > 0 || t.isSlice || t.isInterface || t.isFunctionPointer) return false;
        TypeInfo resolved = resolveTypeAlias(t, defModule);
        if (resolved.pointerLevel > 0 || resolved.isSlice || resolved.isInterface ||
            resolved.isFunctionPointer) {
            return false;
        }
        if (resolved.isArray) return resolved.arraySize > 0;
        return isStructValueType(t, defModule);
    }

    // Visitor methods
    void visit(AstProgram& program) override {
        if (visitedPrograms_.count(&program)) return;
        visitedPrograms_.insert(&program);

        std::string prevPath = currentSourcePath_;
        std::string prevModule = currentModule_;
        if (!program.getSourcePath().empty()) {
            currentSourcePath_ = program.getSourcePath();
        }
        // CID-020/021 (spec §2): the logical module being generated ("" ==
        // main file, which keeps plain names). Definitions emitted under this
        // module mangle with it as the defining module (spec §5).
        auto pmIt = programModules_.find(&program);
        currentModule_ = (pmIt != programModules_.end()) ? pmIt->second : std::string();

        // CID-020 (spec §2): an imported AST generates with its own module
        // scope current, so intra-module references resolve; the main
        // program (empty source path) stays in the root scope.
        Scope* prevScope = symbolTable_.getCurrentScope();
        if (!program.getSourcePath().empty()) {
            auto scopeIt = moduleScopes_.find(program.getSourcePath());
            if (scopeIt != moduleScopes_.end()) {
                symbolTable_.setCurrentScope(scopeIt->second);
            }
        }

        for (auto& imported : program.getImportedAsts()) {
            imported->accept(*this);
        }
        for (auto& stmt : program.getStatements()) {
            stmt->accept(*this);
        }

        symbolTable_.setCurrentScope(prevScope);
        currentModule_ = prevModule;
        currentSourcePath_ = prevPath;
    }

    static std::string sanitizeAsmName(const std::string& name) {
        std::string result;
        for (char c : name) {
            if (c == '<') result += "_l_";
            else if (c == '>') result += "_g_";
            else if (c == ',') result += "_c_";
            else result += c;
        }
        return result;
    }

    // CID-020/021 (spec §5): IR name mangling by DEFINING module. `bare` is
    // the legacy name (free function, or the `Class_method` composition for
    // methods / destructors / vtable entries, so the qualifier always
    // composes outside per spec §6). Main-file symbols and `main`/`_start`
    // keep legacy names, which keeps import-free IR byte-identical;
    // `extern` never reaches here (early return at each emission site).
    static bool isNeverMangled(const std::string& bare) {
        return bare == "main" || bare == "_start";
    }

    static std::string emitFunctionName(const std::string& defModule, const std::string& bare) {
        if (defModule.empty() || isNeverMangled(bare)) return bare;
        return sanitizeAsmName(mangleQualifiedName(defModule, bare));
    }

    // CID-020/021 (spec §5): global IR names mangle by DEFINING module,
    // same rule as functions (T6 probe G1: same-named globals in two
    // modules shared one bare label and qualified reads emitted garbage).
    // Main-file globals keep legacy bare names, which keeps import-free IR
    // byte-identical. `main`/`_start` are never mangled (spec Review Focus);
    // `extern` references resolve through definingModuleFor (bare) and never
    // reach emission (early return at the emission site).
    static std::string emitGlobalName(const std::string& defModule, const std::string& bare) {
        if (defModule.empty() || isNeverMangled(bare)) return bare;
        return mangleQualifiedName(defModule, bare);
    }

    // Resolve a bare callee-signature nominal in the callee's defining
    // module scope (signatures belong to the callee — a bare name that
    // happens not to resolve at the call site still means the same type
    // there). Callers try the legacy scope walk first; this is only the
    // fallback. Null when unresolvable in both.
    std::shared_ptr<Symbol> resolveCalleeNominal(const std::string& name, const std::string& defModule) {
        if (defModule.empty()) return nullptr;
        for (const auto& [path, scope] : moduleScopes_) {
            if (scope && logicalForScope(scope) == defModule) {
                auto mem = scope->resolveLocal(name);
                if (mem) return mem;
            }
        }
        return nullptr;
    }

    // Mangled global key for a resolved symbol: bind-time provenance first
    // (from-aliases share the target object, so the origin module wins),
    // then the scope-identity search, then the same-module rule — the same
    // definingModuleFor chain as function calls.
    std::string globalRefName(const std::shared_ptr<Symbol>& sym, const std::string& name) {
        if (!sym) return name;
        return emitGlobalName(definingModuleFor(sym, name), name);
    }

    // Logical module name for a scope: "" for main/root/unknown (legacy).
    std::string logicalForScope(Scope* s) {
        if (!s) return "";
        if (s == symbolTable_.getRootScope()) return "";
        for (const auto& [prog, logical] : programModules_) {
            if (!prog) continue;
            auto it = moduleScopes_.find(prog->getSourcePath());
            if (it != moduleScopes_.end() && it->second == s) return logical;
        }
        return "";
    }

    // CIDER-BUG-048: scope of a logical module ("" == main/root). Used while
    // folding a const's initializer, whose identifiers bind in the
    // declaration's own module rather than at the use site.
    Scope* scopeForModule(const std::string& logical) {
        if (logical.empty()) return symbolTable_.getRootScope();
        for (const auto& [path, scope] : moduleScopes_) {
            if (scope && logicalForScope(scope) == logical) return scope;
        }
        return nullptr;
    }

    // Scope that binds `name` to this exact symbol object. Module scopes
    // win over root so `from`-aliases (same object as the definition, spec
    // §4) and template clones attribute to the defining module. Null when
    // the symbol has no scope home (locals); callers fall back to the
    // same-module rule.
    Scope* definingScopeFor(const std::shared_ptr<Symbol>& sym, const std::string& name) {
        if (!sym) return nullptr;
        for (const auto& [path, scope] : moduleScopes_) {
            if (scope && scope->resolveLocal(name) == sym) return scope;
        }
        Scope* root = symbolTable_.getRootScope();
        if (root && root->resolveLocal(name) == sym) return root;
        return nullptr;
    }

    // CID-020/021 (spec §3): resolve a nominal type name that may be dotted
    // (`nsa.Event`, `a.util.Foo`) or canonical (`nsa::Event`). Bare names
    // use the legacy scope walk (identical behavior); anything else
    // resolves the qualifier prefix as a module (full logical path or
    // last-segment qualifier) and looks the member up in that module's
    // scope. Null when unresolvable (the checker rejects such programs
    // first, so generation never depends on this path for them).
    std::shared_ptr<Symbol> resolveNominalType(const std::string& name) {
        auto sym = symbolTable_.resolve(name);
        if (sym) return sym;
        std::string qualifier, bare;
        auto canonical = name.find("::");
        if (canonical != std::string::npos) {
            qualifier = name.substr(0, canonical);
            bare = name.substr(canonical + 2);
        } else {
            auto dot = name.rfind('.');
            if (dot == std::string::npos || dot == 0 || dot + 1 >= name.size()) return nullptr;
            qualifier = name.substr(0, dot);
            bare = name.substr(dot + 1);
        }
        if (canonical != std::string::npos) {
            for (const auto& [path, scope] : moduleScopes_) {
                if (scope && logicalForScope(scope) == qualifier) {
                    auto mem = scope->resolveLocal(bare);
                    if (mem) return mem;
                }
            }
            return nullptr;
        }
        auto modSym = symbolTable_.resolve(qualifier);
        if (!modSym || modSym->kind != SymbolKind::MODULE) return nullptr;
        Scope* ms = std::static_pointer_cast<ModuleSymbol>(modSym)->scope;
        if (!ms) return nullptr;
        return ms->resolveLocal(bare);
    }

    // CIDER-BUG-043/029: a nominal spelling belongs to the module where it
    // was written (a field's type in the struct's module, an alias target
    // in the alias's module). Re-root a bare nominal in that module so
    // downstream lookups resolve without an explicit import — e.g. member
    // access on a transitively reached pointee type, or a cross-module
    // `type X = othermod.S`. Mirrors the checker's canonicalizeInModule;
    // main-module types keep bare names (canonical == bare there), so
    // import-free IR stays identical.
    TypeInfo canonicalizeNominalInModule(TypeInfo t, const std::string& ownerMod) {
        if (t.name.empty() || t.name.find("::") != std::string::npos) return t;
        if (t.name.find('<') != std::string::npos || t.name.find('.') != std::string::npos) return t;
        if (ownerMod.empty()) return t;
        auto resolved = resolveNominalType(ownerMod + "::" + t.name);
        if (resolved && (resolved->kind == SymbolKind::STRUCT || resolved->kind == SymbolKind::CLASS ||
                         resolved->kind == SymbolKind::ENUM || resolved->kind == SymbolKind::INTERFACE)) {
            t.name = ownerMod + "::" + t.name;
        }
        return t;
    }

    // CID-031: a symbol declared in another module carries that module's
    // bare nominal type name. Re-root it in the owner's context so layout
    // resolution (field offsets, sizes) matches the checker's canonical
    // identity; without this, `g_percpu.kernel_stack` fell through to the
    // garbage member fallback (base = the loaded struct value) because the
    // bare name was unresolvable in the importing file.
    TypeInfo canonicalizeSymbolType(const std::shared_ptr<Symbol>& sym, const std::string& name) {
        TypeInfo t = sym->type;
        if (t.name.empty() || t.name.find("::") != std::string::npos) return t;
        if (t.name.find('<') != std::string::npos || t.name.find('.') != std::string::npos) return t;
        std::string defMod = definingModuleFor(sym, name);
        if (defMod.empty()) return t;
        auto resolved = resolveNominalType(defMod + "::" + t.name);
        if (resolved && (resolved->kind == SymbolKind::STRUCT || resolved->kind == SymbolKind::CLASS ||
                         resolved->kind == SymbolKind::ENUM || resolved->kind == SymbolKind::INTERFACE)) {
            t.name = defMod + "::" + t.name;
        }
        return t;
    }

    // Defining-module logical name for a resolved symbol ("": main).
    // Bind-time provenance first: it survives chained from-aliases (the
    // alias shares the target object, so identity search alone would pick
    // whichever importing scope sorts first). Falls back to the
    // scope-identity search (unit tests without a collector) and then the
    // same-module rule.
    std::string definingModuleFor(const std::shared_ptr<Symbol>& sym, const std::string& name) {
        // `extern` symbols are never mangled (spec Review Focus): they must
        // match the linker-provided symbol, so every reference site (calls,
        // values, globals, runtime helpers) treats them as main-file names.
        if (sym && sym->isExtern) return "";
        if (sym) {
            auto pit = symbolModules_.find(sym->id);
            if (pit != symbolModules_.end()) return pit->second;
        }
        Scope* ds = definingScopeFor(sym, name);
        if (ds) return logicalForScope(ds);
        return currentModule_;
    }

    // (FunctionSymbol, defining module) for a collector-composed function
    // name (`Class_method`, destructor, vtable entry): the preferred scope
    // (the class's own scope for methods) first, then every module scope,
    // then root. Null symbol when unresolvable (legacy fallback path).
    std::pair<std::shared_ptr<FunctionSymbol>, std::string> resolveFunctionDef(
        const std::string& funcName, Scope* preferred) {
        std::vector<Scope*> order;
        if (preferred) order.push_back(preferred);
        for (const auto& [path, scope] : moduleScopes_) {
            if (scope && scope != preferred) order.push_back(scope);
        }
        Scope* root = symbolTable_.getRootScope();
        if (root && root != preferred) order.push_back(root);
        for (Scope* s : order) {
            auto sym = s->resolveLocal(funcName);
            if (sym && sym->kind == SymbolKind::FUNCTION) {
                auto fsym = std::static_pointer_cast<FunctionSymbol>(sym);
                // Bind-time provenance first (see definingModuleFor):
                // the name search may hit an importer's alias scope.
                auto pit = symbolModules_.find(sym->id);
                std::string mod = (pit != symbolModules_.end()) ? pit->second : logicalForScope(s);
                return {fsym, mod};
            }
        }
        return {nullptr, ""};
    }

    // CID-020/021 (spec §6): vtable-map key for a class/struct symbol —
    // the bare legacy name for main-file types, defining-module mangling
    // otherwise (same rule as method names, so keys stay injective and
    // distinct from function labels). Every reference spells "vtable_"+key
    // (NewExpr, interface stores, VM, native emitter).
    std::string vtableKeyForClass(const std::shared_ptr<StructSymbol>& classSym) {
        if (!classSym) return "";
        return emitGlobalName(definingModuleFor(classSym, classSym->name), classSym->name);
    }

    // CID-020/021 (spec §6): vtable-map key joining an interface and its
    // implementing type, each part mangled by its own defining module.
    // Bare legacy "Iface_Class" when both are main-file.
    std::string vtableIfaceKey(const std::shared_ptr<Symbol>& ifaceSym, const std::string& ifaceName,
                               const std::shared_ptr<Symbol>& scSym, const std::string& scName) {
        std::string im = ifaceSym ? definingModuleFor(ifaceSym, ifaceSym->name) : "";
        std::string sm = scSym ? definingModuleFor(scSym, scSym->name) : "";
        return emitGlobalName(im, ifaceName) + "_" + emitGlobalName(sm, scName);
    }

    // CID-020/021 (spec §6): bare nominal defined by exactly one module
    // scope, for use sites where the defining module was lost (e.g. the
    // callee-signature return type in `var s: Shape = *mod.mkbox()`).
    // Null when absent or ambiguous: the caller keeps the legacy bare
    // spelling, which fails loud rather than guessing a module.
    std::shared_ptr<Symbol> resolveUniqueModuleNominal(const std::string& name) {
        if (name.empty() || name.find('.') != std::string::npos ||
            name.find("::") != std::string::npos) {
            return nullptr;
        }
        std::shared_ptr<Symbol> found;
        for (const auto& [path, scope] : moduleScopes_) {
            if (!scope) continue;
            auto mem = scope->resolveLocal(name);
            if (!mem) continue;
            if (found && found != mem) return nullptr;  // ambiguous: never guess
            found = mem;
        }
        return found;
    }

    // CID-020/021 (spec §6): true when a bare name that does not resolve at
    // the use site names exactly one module's struct/class, so a
    // dereference of it is the aggregate itself (address), not a scalar
    // LOAD. Covers `var s: Shape = *mod.mk()` where the callee-signature
    // return nominal lost its defining module. Ambiguity keeps the legacy
    // scalar path.
    bool isUniqueModuleAggregate(const std::string& name) {
        auto sym = resolveUniqueModuleNominal(name);
        return sym && (sym->kind == SymbolKind::STRUCT || sym->kind == SymbolKind::CLASS);
    }

    // CID-020/021 (spec §6): "vtable_"+key for an interface variable's vptr
    // store, matching generate()'s interface-vtable keys (vtableIfaceKey).
    // Main-file parts resolve in root and keep the legacy "Iface_Class"
    // spelling byte-identical; module parts mangle by defining module.
    // Unresolvable parts keep the legacy bare spelling (same loud
    // unresolved-label failure as before, never a silently wrong vtable).
    std::string interfaceVtableLabel(const std::string& ifaceName, const std::string& concreteName) {
        auto ifaceSym = resolveNominalType(ifaceName);
        if (!ifaceSym || ifaceSym->kind != SymbolKind::INTERFACE) {
            return "vtable_" + ifaceName + "_" + concreteName;
        }
        auto concSym = resolveNominalType(concreteName);
        if (!concSym) concSym = resolveUniqueModuleNominal(concreteName);
        if (!concSym || (concSym->kind != SymbolKind::STRUCT && concSym->kind != SymbolKind::CLASS)) {
            return "vtable_" + ifaceName + "_" + concreteName;
        }
        return "vtable_" + vtableIfaceKey(ifaceSym, ifaceSym->name, concSym, concSym->name);
    }

    // Name for ir_gen-inserted runtime calls (`new`, `delete`, closures,
    // the interpolation helpers): the visible malloc/free definition's
    // mangled name, so the hardened std allocator — not the VM builtin /
    // libc — serves programs that import it. Bare when nothing is in
    // scope (legacy VM-builtin/libc fallback).
    std::string runtimeHelperName(const std::string& bare) {
        auto sym = symbolTable_.resolve(bare);
        if (sym && sym->kind == SymbolKind::FUNCTION)
            return emitFunctionName(definingModuleFor(sym, bare), bare);
        return bare;
    }

    // CID-020 §3: `b.fb(...)` where b is a module qualifier. Mirrors the
    // checker's CallExpr module path: resolve the member in the module's
    // scope and call its mangled name directly. True when handled (the
    // caller skips object evaluation and method lookup). An ambiguous
    // qualifier resolves to the first-registered candidate here, but the
    // checker reports it as an error first, so the driver never reaches
    // this path for ambiguous programs.
    bool tryModuleCall(MemberAccessExpr* mem, std::string& funcName,
                       std::shared_ptr<FunctionSymbol>& outSym) {
        if (mem->isArrow()) return false;
        auto base = dynamic_cast<IdentifierExpr*>(mem->getObject());
        if (!base) return false;
        auto baseSym = symbolTable_.resolve(base->getName());
        if (!baseSym || baseSym->kind != SymbolKind::MODULE) return false;
        auto modSym = std::static_pointer_cast<ModuleSymbol>(baseSym);
        Scope* ms = modSym->scope;
        auto target = ms ? ms->resolveLocal(mem->getMember()) : nullptr;
        if (!target || target->kind != SymbolKind::FUNCTION) return false;
        outSym = std::static_pointer_cast<FunctionSymbol>(target);
        // Mangle by the member's DEFINING module, not the qualifier: the
        // member may itself be a from-alias to another module's function
        // (chained alias). Falls back to the qualifier when no provenance
        // is recorded (unit tests without a collector).
        std::string defMod = modSym->logicalName;
        auto pit = symbolModules_.find(target->id);
        if (pit != symbolModules_.end()) defMod = pit->second;
        // `extern` members are never mangled (spec Review Focus): they must
        // match the linker-provided symbol (same rule as definingModuleFor).
        if (target->isExtern) defMod = "";
        funcName = emitFunctionName(defMod, sanitizeAsmName(mem->getMember()));
        return true;
    }

    void visit(FunctionDeclStmt& stmt) override {
        // Skip template function declarations (uninstantiated)
        if (stmt.hasTemplateParams()) return;

        std::string funcName = sanitizeAsmName(stmt.getName());
        if (!currentClassName_.empty()) {
            funcName = currentClassName_ + "_" + funcName;
        }

        if (stmt.isExtern()) {
            // `extern` names are never mangled (spec Review Focus): they
            // must match the linker-provided symbol.
            module_.addExternFunction(funcName);
            return;
        }
        // CID-020/021 (spec §5): non-main definitions are emitted under the
        // defining module's mangled name (`main`/`_start` stay bare).
        funcName = emitFunctionName(currentModule_, funcName);

        // Save state for nested generation
        auto prevFunction = std::move(currentFunction_);
        auto prevBlock = std::move(currentBlock_);
        auto prevVariableValues = variableValues_;
        int prevTempCount = tempCount_;
        int prevLabelCount = labelCount_;
        auto prevBreakLabels = breakLabels_;
        auto prevContinueLabels = continueLabels_;
        auto prevUnwindDepths = unwindDepths_;
        auto prevDeferStack = deferStack_;
        auto prevDestructorStack = destructorStack_;
        auto prevActiveTryScopes = activeTryScopes_;
        BasicBlock* prevEntryBlock = functionEntryBlock_;
        bool prevIsSret = currentIsSret_;
        TypeInfo prevReturnType = currentReturnType_;

        TypeInfo funcRetType = TypeInfo::fromAST(stmt.getReturnType());
        // Virtual methods stay on the legacy malloc path so VIRTUAL_CALL
        // callers (which don't pass sret) keep matching the callee.
        bool isVirtMethod = stmt.isVirtual() || stmt.isOverride();
        bool funcIsSret = isByValueAggregate(funcRetType) && !isVirtMethod;
        currentIsSret_ = funcIsSret;
        currentReturnType_ = funcRetType;
        IRType funcRetIR = funcIsSret ? IRType::VOID : mapType(funcRetType.name);
        currentFunction_ = std::make_unique<FunctionIR>(funcName, funcRetIR);
        // CID-005: virtual/override definitions are reached through
        // VIRTUAL_CALL, which stays on the SysV path under every target
        // (MS-ABI virtual dispatch is out of scope; T3 pins those call
        // sites to SysV). Emitting them with the target descriptor (MS
        // under `--target uefi`) would silently mismatch every dispatch.
        currentFunction_->setAbi(isVirtMethod ? Abi::SysV : stmt.getAbi());
        currentFunction_->setSourcePath(currentSourcePath_);
        currentBlock_ = std::make_unique<BasicBlock>("entry");
        functionEntryBlock_ = currentBlock_.get();

        variableValues_.clear();
        paramNames_.clear();
        tempCount_ = 0;
        labelCount_ = 0;
        breakLabels_.clear();
        continueLabels_.clear();
        unwindDepths_.clear();
        deferStack_.clear();
        destructorStack_.clear();
        activeTryScopes_.clear();
        deferStack_.push_back({});  // Function scope level
        destructorStack_.push_back({});

        symbolTable_.enterScope();

        // Hidden sret pointer for struct-by-value returns (SysV: rdi).
        // The caller allocates the result slot and passes its address first;
        // `return <struct>` memcpys into it instead of malloc'ing.
        if (funcIsSret) {
            currentFunction_->addParam("__sret", IRType::PTR);
            auto sretSym = std::make_shared<Symbol>();
            sretSym->kind = SymbolKind::VARIABLE;
            sretSym->name = "__sret";
            sretSym->type = {"void", 1};
            symbolTable_.define(sretSym);
            IRValue paramReg = IRValue::Register("__sret", IRType::PTR);
            IRValue addr = IRValue::Register(nextTemp(), IRType::PTR);
            currentBlock_->addInstruction({IROp::ALLOCA, addr, {IRValue::Constant(8, IRType::I32)}});
            currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, paramReg, IRValue::Constant(0, IRType::I32)}});
            variableValues_["__sret"] = addr;
            paramNames_.insert("__sret");
        }

        // Handle 'this' pointer for methods
        if (!currentClassName_.empty()) {
            std::string paramName = "this";
            IRType paramIRType = IRType::PTR;
            currentFunction_->addParam(paramName, paramIRType);

            auto thisSym = std::make_shared<Symbol>();
            thisSym->kind = SymbolKind::VARIABLE;
            thisSym->name = "this";
            thisSym->type.name = currentClassName_;
            thisSym->type.pointerLevel = 1;
            thisSym->type.isUserDefined = true;
            symbolTable_.define(thisSym);

            IRValue paramReg = IRValue::Register(paramName, paramIRType);
            IRValue addr = IRValue::Register(nextTemp(), IRType::PTR);
            currentBlock_->addInstruction({IROp::ALLOCA, addr, {IRValue::Constant(8, IRType::I32)}});
            currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, paramReg, IRValue::Constant(0, IRType::I32)}});
            variableValues_[paramName] = addr;
            paramNames_.insert(paramName);
        }
        for (auto& param : stmt.getParams()) {
            auto sym = std::make_shared<Symbol>();
            sym->kind = SymbolKind::VARIABLE;
            sym->name = param.first;
            sym->type = TypeInfo::fromAST(param.second.get());
            symbolTable_.define(sym);

            IRType paramIRType = (sym->type.pointerLevel > 0) ? IRType::PTR : mapType(sym->type.name);
            std::string paramRegName = "arg_" + param.first;
            currentFunction_->addParam(paramRegName, paramIRType);
            IRValue paramReg = IRValue::Register(paramRegName, paramIRType);
            IRValue addr = IRValue::Register(nextTemp(), IRType::PTR);
            currentBlock_->addInstruction({IROp::ALLOCA, addr, {IRValue::Constant(getTypeSize(sym->type), IRType::I32)}});
            if (sym->type.pointerLevel == 0 && isByValueAggregate(sym->type)) {
                // By-value aggregate params (struct/class/fixed-size array)
                // arrive as a caller pointer: copy the pointee into the
                // local slot instead of storing the pointer value as the
                // first field (CID-022; CIDER-BUG-045 for arrays).
                currentBlock_->addInstruction({IROp::MEMCPY, std::nullopt, {addr, paramReg, IRValue::Constant(getTypeSize(sym->type), IRType::I32)}});
            } else {
                currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, paramReg, IRValue::Constant(0, IRType::I32)}});
            }
            variableValues_[param.first] = addr;
            variableValues_[paramRegName] = addr;  // Support both for lookup convenience
            paramNames_.insert(param.first);
        }
        // Pre-pass: params assigned anywhere in the body must not read-through.
        assignedParams_.clear();
        for (auto& s : stmt.getBody()) collectAssignedParams(s.get());

        for (auto& s : stmt.getBody()) {
            s->accept(*this);
        }

        if (currentBlock_ && (currentBlock_->getInstructions().empty() || currentBlock_->getInstructions().back().op != IROp::RET)) {
            emitDefers();
            emitDestructors();
            currentBlock_->addInstruction({IROp::RET, std::nullopt, {}});
        }

        if (currentBlock_) {
            currentFunction_->addBlock(std::move(currentBlock_));
            currentBlock_ = nullptr;
        }
        module_.addFunction(std::move(currentFunction_));
        symbolTable_.exitScope();
        deferStack_.pop_back();
        destructorStack_.pop_back();

        // Restore state
        currentFunction_ = std::move(prevFunction);
        currentBlock_ = std::move(prevBlock);
        variableValues_ = prevVariableValues;
        tempCount_ = prevTempCount;
        labelCount_ = prevLabelCount;
        breakLabels_ = prevBreakLabels;
        continueLabels_ = prevContinueLabels;
        unwindDepths_ = prevUnwindDepths;
        deferStack_ = prevDeferStack;
        destructorStack_ = prevDestructorStack;
        activeTryScopes_ = std::move(prevActiveTryScopes);
        functionEntryBlock_ = prevEntryBlock;
        currentIsSret_ = prevIsSret;
        currentReturnType_ = prevReturnType;
    }

    void visit(ReturnStmt& stmt) override {
        if (stmt.getValue()) {
            stmt.getValue()->accept(*this);
            IRValue val = lastValue_;
            TypeInfo retType = lastExprType_;

            // CID-003: the function's declared return type drives the
            // returned representation (a returned literal keeps its parsed
            // double; registers convert through the CVT ops; int returns
            // convert too). Pointers and non-float returns pass through.
            val = convertValueToType(val, currentReturnType_);

            // Struct-by-value return via hidden sret pointer: no malloc,
            // freestanding-safe. The caller-provided slot outlives the frame.
            if (currentIsSret_ && isByValueAggregate(currentReturnType_)) {
                int structSize = getTypeSize(currentReturnType_);
                IRValue sretAddr = variableValues_.count("__sret")
                    ? variableValues_["__sret"]
                    : IRValue::Register("__sret", IRType::PTR);
                IRValue sretPtr = IRValue::Register(nextTemp(), IRType::PTR);
                currentBlock_->addInstruction({IROp::LOAD, sretPtr, {sretAddr, IRValue::Constant(0, IRType::I32)}});
                currentBlock_->addInstruction({IROp::MEMCPY, std::nullopt, {sretPtr, val, IRValue::Constant(structSize, IRType::I32)}});
                lastExprType_ = currentReturnType_;
                emitDefers();
                emitDestructors();
                // CIDER-BUG-053: a return from inside a try must restore the
                // enclosing handler for the caller's frame.
                emitExceptionUnwind(0);
                currentBlock_->addInstruction({IROp::RET, std::nullopt, {}});
                startBlock(nextLabel("dead"));
                return;
            }

            // Legacy heap path for struct values returned from non-sret
            // contexts (should not occur for declared struct returns, which
            // are always sret; kept for robustness). The struct test resolves
            // alias chains, so a scalar alias is not copied as a struct.
            if (retType.pointerLevel == 0 && isStructValueType(retType)) {
                int structSize = getTypeSize(retType);
                IRValue localPtr = IRValue::Register(nextTemp(), IRType::PTR);
                currentBlock_->addInstruction({IROp::CALL, localPtr, {IRValue::Label(runtimeHelperName("malloc")), IRValue::Constant(structSize, IRType::I32)}});
                currentBlock_->addInstruction({IROp::MEMCPY, std::nullopt, {localPtr, val, IRValue::Constant(structSize, IRType::I32)}});
                val = localPtr;
                lastExprType_.isUserDefined = true;
            }

            emitDefers();
            emitDestructors();
            emitExceptionUnwind(0);
            currentBlock_->addInstruction({IROp::RET, std::nullopt, {val}});
        } else {
            emitDefers();
            emitDestructors();
            emitExceptionUnwind(0);
            currentBlock_->addInstruction({IROp::RET, std::nullopt, {}});
        }
        startBlock(nextLabel("dead"));
    }

    void visit(CallExpr& expr) override {
        auto calleeExpr = expr.getCallee();
        std::string funcName = "";
        bool isVirtual = false;
        int vtableIndex = -1;
        IRValue objPtr = IRValue::Constant(0, IRType::PTR);
        std::shared_ptr<FunctionSymbol> resolvedFuncSym = nullptr;

        if (auto id = dynamic_cast<IdentifierExpr*>(calleeExpr)) {
            std::string lookupName = id->getName();
            auto sym = symbolTable_.resolve(lookupName);
            if (sym && sym->kind == SymbolKind::FUNCTION) {
                // CID-020/021 (spec §5): calls use the resolved symbol's
                // mangled name, so same-module, from-alias, and template
                // uses all match the definition.
                funcName = emitFunctionName(definingModuleFor(sym, lookupName),
                                            sanitizeAsmName(lookupName));
                resolvedFuncSym = std::static_pointer_cast<FunctionSymbol>(sym);
            }
        } else if (auto mem = dynamic_cast<MemberAccessExpr*>(calleeExpr)) {
            // CID-020/021 §3: `b.fb(...)` calls the function in b's module
            // scope directly (checker already validated the qualifier and
            // member). Skips object evaluation and method lookup.
            if (!tryModuleCall(mem, funcName, resolvedFuncSym)) {
            // Method call: obj.method() or obj->method()
            bool wasAddress = isAddress_;
            isAddress_ = false;
            mem->getObject()->accept(*this);
            objPtr = lastValue_;
            isAddress_ = wasAddress;

            TypeInfo objType = lastExprType_;
            if (objType.pointerLevel > 0) objType.pointerLevel--;

            // Check if object is an interface type -> interface method call
            if (objType.isInterface) {
                auto ifaceSym = resolveNominalType(objType.name);
                if (ifaceSym && ifaceSym->kind == SymbolKind::INTERFACE) {
                    auto iface = std::static_pointer_cast<InterfaceSymbol>(ifaceSym);
                    int methodIdx = 0;
                    bool found = false;
                    for (auto& m : iface->methods) {
                        if (m.name == mem->getMember()) {
                            found = true;
                            // objPtr points to the fat pointer (address of interface variable)
                            // Load data_ptr from [objPtr + 0]
                            IRValue dataPtrReg = IRValue::Register(nextTemp(), IRType::PTR);
                            currentBlock_->addInstruction({IROp::LOAD, dataPtrReg, {objPtr, IRValue::Constant(0, IRType::I32)}});
                            // Load vtable_ptr from [objPtr + 8]
                            IRValue vtablePtrReg = IRValue::Register(nextTemp(), IRType::PTR);
                            currentBlock_->addInstruction({IROp::LOAD, vtablePtrReg, {objPtr, IRValue::Constant(8, IRType::I32)}});
                            // Load method address from vtable[methodIdx]
                            IRValue methodAddrReg = IRValue::Register(nextTemp(), IRType::PTR);
                            currentBlock_->addInstruction({IROp::LOAD, methodAddrReg, {vtablePtrReg, IRValue::Constant(methodIdx * 8, IRType::I32)}});
                            // CALL through method address with data_ptr as 'this'
                            std::vector<IRValue> args;
                            args.push_back(methodAddrReg);
                            args.push_back(dataPtrReg);
                            // CID-003: parameter widths drive argument
                            // conversion here too (m.parameterTypes has no
                            // hidden `this`, which is passed separately).
                            for (auto& arg : expr.getArgs()) {
                                arg->accept(*this);
                                args.push_back(lastValue_);
                            }
                            convertCallArgs(args, 2, m.parameterTypes);
                            IRType retIRType = (m.returnType.pointerLevel > 0) ? IRType::PTR : mapType(m.returnType.name);
                            IRValue dest = IRValue::Register(nextTemp(), retIRType);
                            IRInstruction callInstr;
                            callInstr.op = IROp::CALL;
                            // CID-005: interface dispatch is SysV (the
                            // implementations are pinned SysV by generate()).
                            callInstr.callAbi = Abi::SysV;
                            callInstr.dest = dest;
                            callInstr.args = std::move(args);
                            currentBlock_->addInstruction(callInstr);
                            lastValue_ = dest;
                            lastExprType_ = m.returnType;
                            return;
                        }
                        methodIdx++;
                    }
                    if (!found) {
                        funcName = mem->getMember();
                    }
                    return;
                }
            }

            // CIDER-BUG-015: alias-safe lookup (providers are STRUCT-kind
            // plain Symbols, not StructSymbols).
            auto sym = resolveStructOrClassSymbol(objType);
            if (sym) {
                auto classSym = std::static_pointer_cast<StructSymbol>(sym);
                // CID-020/021 (spec §5/§6): method calls use the defining
                // module's composed name, matching the definition. The
                // method's FunctionSymbol (for the return-type/sret logic
                // below) is resolved in the defining scope.
                Scope* classScope = definingScopeFor(classSym, classSym->name);
                for (const auto& m : classSym->methods) {
                    if (m.name == mem->getMember()) {
                        auto methodDef = resolveFunctionDef(m.mangledName, classScope);
                        funcName = emitFunctionName(methodDef.second, m.mangledName);
                        if (methodDef.first) resolvedFuncSym = methodDef.first;
                        isVirtual = m.isVirtual || m.isOverride;
                        vtableIndex = m.vtableIndex;
                        break;
                    }
                }
            }
            }  // !tryModuleCall: ordinary method call
        }

        if (!funcName.empty()) {
            std::vector<IRValue> args;
            for (auto& arg : expr.getArgs()) {
                arg->accept(*this);
                args.push_back(lastValue_);
            }

            auto sym = resolvedFuncSym;
            if (!sym) {
                auto rawSym = symbolTable_.resolve(funcName);
                if (rawSym && rawSym->kind == SymbolKind::FUNCTION)
                    sym = std::static_pointer_cast<FunctionSymbol>(rawSym);
            }
            TypeInfo calleeRet = {"int", 0};
            std::string calleeDefMod;
            if (sym && sym->kind == SymbolKind::FUNCTION) {
                auto funcSym = std::static_pointer_cast<FunctionSymbol>(sym);
                calleeRet = funcSym->returnType;
                // CID-020/021: bare signature nominals belong to the callee;
                // scope the sret/size queries to its defining module so a
                // same-named type in the caller (or no visible type at all)
                // cannot change the call convention.
                calleeDefMod = definingModuleFor(sym, funcName);
                lastExprType_ = calleeRet;
                // CID-003: the parameter's declared width drives the
                // conversion of float-family arguments (a literal in a
                // float context keeps its parsed double). Method symbols
                // carry `this` as parameter 0, so shift past it when an
                // object is passed as the hidden first argument.
                size_t paramBase = 0;
                if ((objPtr.kind != IRValue::CONSTANT || objPtr.intValue != 0) &&
                    !funcSym->parameterTypes.empty()) {
                    paramBase = 1;
                }
                convertCallArgs(args, 0, funcSym->parameterTypes, paramBase);
            }
            // CID-005: direct calls carry the callee FunctionSymbol's ABI
            // (`Default` when unmarked; codegen resolves that from the target
            // or from the definition's pinned ABI).
            Abi calleeAbi = Abi::Default;
            if (resolvedFuncSym) {
                calleeAbi = resolvedFuncSym->abi;
            } else if (sym && sym->kind == SymbolKind::FUNCTION) {
                calleeAbi = std::static_pointer_cast<FunctionSymbol>(sym)->abi;
            }
            bool calleeIsSret = isByValueAggregate(calleeRet, calleeDefMod);
            // Virtual callees are never sret (they stay on the malloc path);
            // direct CALLs here are free functions / non-virtual methods.
            if (calleeIsSret && !(isVirtual && vtableIndex != -1)) {
                int retSize = getTypeSize(calleeRet, calleeDefMod);
                IRValue retBuf = IRValue::Register(nextTemp(), IRType::PTR);
                currentBlock_->addInstruction({IROp::ALLOCA, retBuf, {IRValue::Constant(retSize, IRType::I32)}});
                IRInstruction callInstr;
                callInstr.op = IROp::CALL;
                callInstr.callAbi = calleeAbi;
                callInstr.dest = std::nullopt;
                callInstr.args.push_back(IRValue::Label(funcName));
                callInstr.args.push_back(retBuf);
                bool hasObj = (objPtr.kind != IRValue::CONSTANT || objPtr.intValue != 0);
                if (hasObj) {
                    callInstr.args.push_back(objPtr);
                }
                for (auto& a : args) callInstr.args.push_back(a);
                currentBlock_->addInstruction(callInstr);
                lastValue_ = retBuf;
                lastExprType_ = calleeRet;
                lastExprType_.isUserDefined = true;
                return;
            }
            IRType retIRType = IRType::I32;
            if (sym && sym->kind == SymbolKind::FUNCTION) {
                auto funcSym = std::static_pointer_cast<FunctionSymbol>(sym);
                retIRType = (funcSym->returnType.pointerLevel > 0) ? IRType::PTR : mapType(funcSym->returnType.name);
            }

            IRValue dest = IRValue::Register(nextTemp(), retIRType);

            IRInstruction callInstr;
            if (isVirtual && vtableIndex != -1) {
                callInstr.op = IROp::VIRTUAL_CALL;
                // CID-005: MS-ABI virtual dispatch is out of scope; the
                // virtual/override definitions are pinned SysV.
                callInstr.callAbi = Abi::SysV;
                callInstr.args.push_back(objPtr);
                callInstr.args.push_back(IRValue::Constant(vtableIndex, IRType::I32));
                for (auto& a : args) callInstr.args.push_back(a);
            } else {
                callInstr.op = IROp::CALL;
                callInstr.callAbi = calleeAbi;
                callInstr.args.push_back(IRValue::Label(funcName));
                if (objPtr.kind != IRValue::CONSTANT || objPtr.intValue != 0) {
                    callInstr.args.push_back(objPtr);
                }
                for (auto& a : args) callInstr.args.push_back(a);
            }

            callInstr.dest = dest;
            currentBlock_->addInstruction(callInstr);
            lastValue_ = dest;
        } else {
            // Function pointer / closure call: callee is an expression.
            bool wasAddress = isAddress_;
            isAddress_ = false;
            calleeExpr->accept(*this);
            isAddress_ = wasAddress;
            IRValue fnPtr = lastValue_;
            // CID-006 (Task 2 F1 carry-over): an alias spelling
            // (`type Cb = fn(...) -> R`) carries the alias name in its
            // TypeInfo, so resolve to the signature before either gate.
            // Closures (`func`, object pointers) are unaffected.
            TypeInfo calleeType = resolveFunctionPointerAlias(lastExprType_);

            std::vector<IRValue> args;
            for (auto& arg : expr.getArgs()) {
                arg->accept(*this);
                args.push_back(lastValue_);
            }

            if (calleeType.name == "func" && calleeType.pointerLevel >= 1) {
                // Closure call: the value is a closure object {fnptr,
                // captures...} — load the fnptr and pass the object first.
                IRValue obj = fnPtr;
                fnPtr = IRValue::Register(nextTemp(), IRType::PTR);
                currentBlock_->addInstruction({IROp::LOAD, fnPtr, {obj, IRValue::Constant(0, IRType::I32)}});
                IRValue dest = IRValue::Register(nextTemp(), IRType::I32);
                IRInstruction callInstr;
                callInstr.op = IROp::CALL;
                // CID-005: closure definitions are pinned SysV (T1 fix
                // round 1), so their call sites are too.
                callInstr.callAbi = Abi::SysV;
                callInstr.args.push_back(fnPtr);
                callInstr.args.push_back(obj);
                for (auto& a : args) callInstr.args.push_back(a);
                callInstr.dest = dest;
                currentBlock_->addInstruction(callInstr);
                lastValue_ = dest;
                lastExprType_ = {"int", 0};
            } else {
                // CID-006: a call through a typed fn(...) value returns the
                // stored return type and converts arguments through the
                // stored parameter types. Untyped *void keeps the legacy
                // assumed-int result.
                TypeInfo retType{"int", 0};
                if (calleeType.isFunctionPointer && !calleeType.isArray &&
                    calleeType.fnReturnType) {
                    retType = *calleeType.fnReturnType;
                }
                if (calleeType.isFunctionPointer && !calleeType.isArray &&
                    calleeType.fnParamTypes) {
                    convertCallArgs(args, 0, *calleeType.fnParamTypes);
                }
                IRType destIR = (retType.pointerLevel > 0 || retType.isFunctionPointer)
                                    ? IRType::PTR
                                    : mapType(retType.name);
                if (destIR == IRType::VOID) destIR = IRType::I32;
                IRValue dest = IRValue::Register(nextTemp(), destIR);
                IRInstruction callInstr;
                callInstr.op = IROp::CALL;
                // CID-005: the fn type carries the convention: `msabi fn`
                // values call MS, plain `fn` values SysV. A label callee
                // (e.g. `super.method()`, whose SuperExpr yields a method
                // label) is a direct call to a known definition, so leave it
                // Default for codegen to resolve from that definition.
                callInstr.callAbi =
                    calleeType.isMsAbi
                        ? Abi::Ms
                        : (fnPtr.kind == IRValue::LABEL ? Abi::Default : Abi::SysV);
                callInstr.args.push_back(fnPtr);
                for (auto& a : args) callInstr.args.push_back(a);
                callInstr.dest = dest;
                currentBlock_->addInstruction(callInstr);
                lastValue_ = dest;
                lastExprType_ = retType;
            }
        }
    }

    // --- closures ---

    void collectCapturesExpr(Expression* e, const std::set<std::string>& params,
                             std::vector<std::string>& out) {
        if (!e) return;
        if (auto id = dynamic_cast<IdentifierExpr*>(e)) {
            const std::string& n = id->getName();
            if (!params.count(n) && variableValues_.count(n) &&
                std::find(out.begin(), out.end(), n) == out.end()) {
                out.push_back(n);
            }
        } else if (auto call = dynamic_cast<CallExpr*>(e)) {
            collectCapturesExpr(call->getCallee(), params, out);
            for (auto& a : call->getArgs()) collectCapturesExpr(a.get(), params, out);
        } else if (auto bin = dynamic_cast<BinaryExpr*>(e)) {
            collectCapturesExpr(bin->getLeft(), params, out);
            collectCapturesExpr(bin->getRight(), params, out);
        } else if (auto un = dynamic_cast<UnaryExpr*>(e)) {
            collectCapturesExpr(un->getOperand(), params, out);
        } else if (auto tern = dynamic_cast<TernaryExpr*>(e)) {
            collectCapturesExpr(tern->getCondition(), params, out);
            collectCapturesExpr(tern->getThenExpr(), params, out);
            collectCapturesExpr(tern->getElseExpr(), params, out);
        } else if (auto group = dynamic_cast<GroupingExpr*>(e)) {
            collectCapturesExpr(group->getExpression(), params, out);
        } else if (auto mem = dynamic_cast<MemberAccessExpr*>(e)) {
            collectCapturesExpr(mem->getObject(), params, out);
        } else if (auto idx = dynamic_cast<IndexExpr*>(e)) {
            collectCapturesExpr(idx->getObject(), params, out);
            collectCapturesExpr(idx->getIndex(), params, out);
        } else if (auto cast = dynamic_cast<CastExpr*>(e)) {
            collectCapturesExpr(cast->getExpression(), params, out);
        } else if (auto interp = dynamic_cast<InterpolatedStringExpr*>(e)) {
            for (const auto& p : interp->getParts()) {
                if (p.isExpr) collectCapturesExpr(p.expr.get(), params, out);
            }
        }
    }

    void collectCapturesStmt(Statement* s, const std::set<std::string>& params,
                             std::vector<std::string>& out) {
        if (!s) return;
        switch (s->getType()) {
            case ASTNode::EXPRESSION_STMT:
                collectCapturesExpr(dynamic_cast<ExpressionStmt*>(s)->getExpression(), params, out);
                break;
            case ASTNode::RETURN_STMT: {
                auto rs = dynamic_cast<ReturnStmt*>(s);
                if (rs->getValue()) collectCapturesExpr(rs->getValue(), params, out);
                break;
            }
            case ASTNode::VAR_DECL_STMT: {
                auto vs = dynamic_cast<VarDeclStmt*>(s);
                if (vs->getInitializer()) collectCapturesExpr(vs->getInitializer(), params, out);
                break;
            }
            case ASTNode::ASSIGN_STMT: {
                auto as = dynamic_cast<AssignStmt*>(s);
                collectCapturesExpr(as->getTarget(), params, out);
                collectCapturesExpr(as->getValue(), params, out);
                break;
            }
            case ASTNode::IF_STMT: {
                auto is = dynamic_cast<IfStmt*>(s);
                collectCapturesExpr(is->getCondition(), params, out);
                for (auto& b : is->getThenBranch()) collectCapturesStmt(b.get(), params, out);
                for (auto& b : is->getElseBranch()) collectCapturesStmt(b.get(), params, out);
                break;
            }
            case ASTNode::WHILE_STMT: {
                auto ws = dynamic_cast<WhileStmt*>(s);
                collectCapturesExpr(ws->getCondition(), params, out);
                for (auto& b : ws->getBody()) collectCapturesStmt(b.get(), params, out);
                break;
            }
            case ASTNode::BLOCK_STMT:
                for (auto& b : dynamic_cast<BlockStmt*>(s)->getStatements()) {
                    collectCapturesStmt(b.get(), params, out);
                }
                break;
            case ASTNode::FOR_STMT: {
                auto fs = dynamic_cast<ForStmt*>(s);
                if (fs->getInitializer()) collectCapturesStmt(fs->getInitializer(), params, out);
                if (fs->getCondition()) collectCapturesExpr(fs->getCondition(), params, out);
                if (fs->getIncrement()) collectCapturesStmt(fs->getIncrement(), params, out);
                for (auto& b : fs->getBody()) collectCapturesStmt(b.get(), params, out);
                break;
            }
            case ASTNode::FOR_EACH_STMT: {
                auto fe = dynamic_cast<ForEachStmt*>(s);
                collectCapturesExpr(fe->getIterable(), params, out);
                for (auto& b : fe->getBody()) collectCapturesStmt(b.get(), params, out);
                break;
            }
            default:
                break;
        }
    }

    void visit(FunctionExpr& expr) override {
        if (!currentFunction_) {
            lastValue_ = IRValue::Constant(0, IRType::PTR);
            lastExprType_ = {"func", 1};
            return;
        }

        std::set<std::string> paramNames;
        for (auto& p : expr.getParams()) paramNames.insert(p.first);
        std::vector<std::string> captures;
        for (auto& s : expr.getBody()) collectCapturesStmt(s.get(), paramNames, captures);

        std::string funcName = "_closure_" + std::to_string(closureCount_++);
        TypeInfo retType = expr.getReturnType() ? TypeInfo::fromAST(expr.getReturnType()) : TypeInfo{"int", 0};
        IRType retIR = (retType.pointerLevel > 0) ? IRType::PTR : mapType(retType.name);

        // Synthesize the closure function: the hidden first parameter is the
        // closure object; captures are read as its fields.
        auto prevFunction = std::move(currentFunction_);
        auto prevBlock = std::move(currentBlock_);
        auto prevVariableValues = variableValues_;
        int prevTempCount = tempCount_;
        int prevLabelCount = labelCount_;
        auto prevBreakLabels = breakLabels_;
        auto prevContinueLabels = continueLabels_;
        auto prevUnwindDepths = unwindDepths_;
        auto prevDeferStack = deferStack_;
        auto prevDestructorStack = destructorStack_;
        auto prevActiveTryScopes = activeTryScopes_;
        BasicBlock* prevEntryBlock = functionEntryBlock_;
        bool prevIsSret = currentIsSret_;
        TypeInfo prevReturnType = currentReturnType_;
        // Closures are invoked via function pointers (non-sret convention),
        // so struct returns inside use the legacy heap path.
        currentIsSret_ = false;
        currentReturnType_ = retType;

        currentFunction_ = std::make_unique<FunctionIR>(funcName, retIR);
        // CID-005: closures are invoked through fn values, which T3 pins to
        // SysV under every target (there is no MS closure convention in
        // scope), so the synthesized body must be SysV too.
        currentFunction_->setAbi(Abi::SysV);
        currentFunction_->setSourcePath(currentSourcePath_);
        currentBlock_ = std::make_unique<BasicBlock>("entry");
        functionEntryBlock_ = currentBlock_.get();
        variableValues_.clear();
        paramNames_.clear();
        tempCount_ = 0;
        labelCount_ = 0;
        breakLabels_.clear();
        continueLabels_.clear();
        unwindDepths_.clear();
        deferStack_.clear();
        destructorStack_.clear();
        activeTryScopes_.clear();
        deferStack_.push_back({});
        destructorStack_.push_back({});

        symbolTable_.enterScope();
        // Hidden closure parameter. CID-005: carry its real IR type (the
        // MS/SysV classifiers read `getParamTypes()`, and the I32 default
        // would misclassify the pointer).
        currentFunction_->addParam("arg__closure", IRType::PTR);
        {
            auto sym = std::make_shared<Symbol>();
            sym->kind = SymbolKind::VARIABLE;
            sym->name = "this";
            sym->type = {"void", 1};
            symbolTable_.define(sym);
            IRValue paramReg = IRValue::Register("arg__closure", IRType::PTR);
            IRValue addr = IRValue::Register(nextTemp(), IRType::PTR);
            currentBlock_->addInstruction({IROp::ALLOCA, addr, {IRValue::Constant(8, IRType::I32)}});
            currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, paramReg, IRValue::Constant(0, IRType::I32)}});
            variableValues_["__closure"] = addr;
            paramNames_.insert("arg__closure");
        }
        // Captured variables are addressed as closure-object fields.
        for (size_t ci = 0; ci < captures.size(); ++ci) {
            IRValue obj = IRValue::Register(nextTemp(), IRType::PTR);
            currentBlock_->addInstruction({IROp::LOAD, obj, {variableValues_["__closure"], IRValue::Constant(0, IRType::I32)}});
            IRValue field = IRValue::Register(nextTemp(), IRType::PTR);
            currentBlock_->addInstruction({IROp::ADD, field, {obj, IRValue::Constant(8 + 8 * (int)ci, IRType::I32)}});
            variableValues_[captures[ci]] = field;
        }
        // Typed parameters.
        for (auto& p : expr.getParams()) {
            TypeInfo pt = p.second ? TypeInfo::fromAST(p.second.get()) : TypeInfo{"int", 0};
            std::string paramName = "arg_" + p.first;
            // CID-005: computed before addParam so the FunctionIR carries
            // the real IR type (see the hidden closure parameter above).
            IRType paramIRType = (pt.pointerLevel > 0) ? IRType::PTR : mapType(pt.name);
            currentFunction_->addParam(paramName, paramIRType);
            auto sym = std::make_shared<Symbol>();
            sym->kind = SymbolKind::VARIABLE;
            sym->name = p.first;
            sym->type = pt;
            symbolTable_.define(sym);
            IRValue paramReg = IRValue::Register(paramName, paramIRType);
            IRValue addr = IRValue::Register(nextTemp(), IRType::PTR);
            currentBlock_->addInstruction({IROp::ALLOCA, addr, {IRValue::Constant(8, IRType::I32)}});
            currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, paramReg, IRValue::Constant(0, IRType::I32)}});
            variableValues_[p.first] = addr;
            paramNames_.insert(paramName);
        }
        for (auto& s : expr.getBody()) {
            s->accept(*this);
        }
        symbolTable_.exitScope();

        currentFunction_->addBlock(std::move(currentBlock_));
        module_.addFunction(std::move(currentFunction_));
        currentFunction_ = std::move(prevFunction);
        currentBlock_ = std::move(prevBlock);
        variableValues_ = std::move(prevVariableValues);
        tempCount_ = prevTempCount;
        labelCount_ = prevLabelCount;
        breakLabels_ = std::move(prevBreakLabels);
        continueLabels_ = std::move(prevContinueLabels);
        unwindDepths_ = std::move(prevUnwindDepths);
        deferStack_ = std::move(prevDeferStack);
        destructorStack_ = std::move(prevDestructorStack);
        activeTryScopes_ = std::move(prevActiveTryScopes);
        functionEntryBlock_ = prevEntryBlock;
        currentIsSret_ = prevIsSret;
        currentReturnType_ = prevReturnType;

        // Closure object: { fnptr, cap_0, ... } — the escape analysis turns
        // the malloc into a stack slot when the closure never leaves.
        int objSize = 8 + 8 * (int)captures.size();
        IRValue obj = IRValue::Register(nextTemp(), IRType::PTR);
        currentBlock_->addInstruction({IROp::CALL, obj, {IRValue::Label(runtimeHelperName("malloc")), IRValue::Constant(objSize, IRType::I32)}});
        currentBlock_->addInstruction({IROp::STORE, std::nullopt, {obj, IRValue::Label(funcName), IRValue::Constant(0, IRType::I32)}});
        for (size_t ci = 0; ci < captures.size(); ++ci) {
            IdentifierExpr ident(captures[ci], 0, 0);
            ident.accept(*this);
            IRValue capVal = lastValue_;
            IRValue capSlot = IRValue::Register(nextTemp(), IRType::PTR);
            currentBlock_->addInstruction({IROp::ADD, capSlot, {obj, IRValue::Constant(8 + 8 * (int)ci, IRType::I32)}});
            currentBlock_->addInstruction({IROp::STORE, std::nullopt, {capSlot, capVal, IRValue::Constant(0, IRType::I32)}});
        }
        lastValue_ = obj;
        lastExprType_ = {"func", 1};
    }

    void visit(MemberAccessExpr& expr) override {
        // CID-020/021 (spec §3/§5): `m.gval` where m is a module qualifier
        // reads the member in m's module scope (T6 probe G1/G3: this used to
        // emit `load %m, gval` garbage). The member may itself be a
        // from-alias into another module, so mangle by the member's
        // DEFINING module — the same rule as tryModuleCall. Address-capable:
        // qualified stores (`m.gval = v`) flow through getAddress and STORE
        // to the label directly, mirroring the IdentifierExpr global paths.
        if (auto base = dynamic_cast<IdentifierExpr*>(expr.getObject())) {
            auto baseSym = symbolTable_.resolve(base->getName());
            if (baseSym && baseSym->kind == SymbolKind::MODULE) {
                auto modSym = std::static_pointer_cast<ModuleSymbol>(baseSym);
                Scope* ms = modSym->scope;
                auto target = ms ? ms->resolveLocal(expr.getMember()) : nullptr;
                if (target && target->kind == SymbolKind::FUNCTION) {
                    lastValue_ = IRValue::Label(emitFunctionName(
                        definingModuleFor(target, expr.getMember()), target->name));
                    // CIDER-BUG-023: carry the fn signature (not *void) so
                    // indirect calls convert args/returns and global
                    // initializers fold; mirrors visit(IdentifierExpr).
                    auto fsym = std::static_pointer_cast<FunctionSymbol>(target);
                    TypeInfo ft;
                    ft.name = "fn";
                    ft.isFunctionPointer = true;
                    ft.isMsAbi = (fsym->abi == Abi::Ms) || (fsym->abi == Abi::Default && defaultAbiMs_);
                    ft.fnParamTypes = std::make_shared<std::vector<TypeInfo>>(fsym->parameterTypes);
                    ft.fnReturnType = std::make_shared<TypeInfo>(fsym->returnType);
                    lastExprType_ = ft;
                    return;
                }
                if (target && target->kind == SymbolKind::VARIABLE) {
                    std::string gname = emitGlobalName(
                        definingModuleFor(target, expr.getMember()), target->name);
                    lastExprType_ = canonicalizeSymbolType(target, expr.getMember());
                    if (lastExprType_.pointerLevel == 0) {
                        // CIDER-BUG-015/019: alias-aware (see visit(IdentifierExpr)).
                        lastExprType_ = resolveTypeAlias(lastExprType_,
                                                         definingModuleFor(target, expr.getMember()));
                        if (resolveStructOrClassSymbol(lastExprType_,
                                                       definingModuleFor(target, expr.getMember()))) {
                            lastExprType_.isUserDefined = true;
                        } else {
                            auto typeSym = resolveNominalType(lastExprType_.name);
                            if (typeSym && (typeSym->kind == SymbolKind::STRUCT ||
                                            typeSym->kind == SymbolKind::CLASS)) {
                                lastExprType_.isUserDefined = typeSym->type.name.empty();
                            }
                        }
                    } else {
                        lastExprType_.isUserDefined = false;
                    }
                    if (isAddress_ || lastExprType_.isArray || lastExprType_.isUserDefined ||
                        lastExprType_.isInterface) {
                        lastValue_ = IRValue::Label(gname);
                        return;
                    }
                    IRType irType = (target->type.pointerLevel > 0) ? IRType::PTR
                                                                   : mapType(target->type.name);
                    IRValue dest = IRValue::Register(nextTemp(), irType);
                    currentBlock_->addInstruction({IROp::LOAD, dest, {IRValue::Label(gname)}});
                    lastValue_ = dest;
                    return;
                }
            }
        }
        bool wasAddress = isAddress_;
        isAddress_ = false;
        expr.getObject()->accept(*this);
        IRValue obj = lastValue_;
        isAddress_ = wasAddress;

        TypeInfo objType = lastExprType_;
        if (objType.pointerLevel > 0) objType.pointerLevel--;

        // CIDER-BUG-015: alias providers are STRUCT-kind plain Symbols;
        // resolve `type X = T` chains before the StructSymbol cast (an
        // alias-typed object used to reach undefined behavior here).
        auto sym = resolveStructOrClassSymbol(objType);
        if (sym) {
            auto structSym = std::static_pointer_cast<StructSymbol>(sym);
            std::string ownerMod = definingModuleFor(sym, structSym->name);
            for (const auto& field : structSym->fields) {
                if (field.name == expr.getMember()) {
                    IRValue addr = IRValue::Register(nextTemp(), IRType::PTR);
                    currentBlock_->addInstruction({IROp::ADD, addr, {obj, IRValue::Constant(field.offset, IRType::I32)}});

                    // The field's declared type may itself be an alias
                    // spelling: resolve the chain before classifying
                    // (struct vs scalar) and before choosing the load
                    // width, or `type W = u64` fields lower as addresses
                    // and `type H = *void` fields as non-pointers. Re-root
                    // bare nominals in the struct's module so a
                    // transitively reached pointee type resolves without
                    // an explicit import (CIDER-BUG-043).
                    TypeInfo fieldType =
                        canonicalizeNominalInModule(resolveTypeAlias(field.type, ownerMod), ownerMod);
                    bool fieldIsStruct = (fieldType.pointerLevel == 0);
                    if (fieldIsStruct) {
                        auto fieldTypeSym = resolveNominalType(fieldType.name);
                        if (!fieldTypeSym && !ownerMod.empty()) {
                            fieldTypeSym = resolveCalleeNominal(fieldType.name, ownerMod);
                        }
                        if (!fieldTypeSym || (fieldTypeSym->kind != SymbolKind::STRUCT && fieldTypeSym->kind != SymbolKind::CLASS) ||
                            !fieldTypeSym->type.name.empty()) {
                            fieldIsStruct = false;
                        }
                    }
                    // Array-typed fields are address-like (like locals):
                    // indexing them must operate on the array's location,
                    // not on a loaded copy of its first element.
                    bool fieldIsArray = fieldType.isArray;
                    if (field.bitWidth > 0 && !isAddress_) {
                        bool isSigned = (fieldType.name == "int" || fieldType.name == "i8" ||
                                         fieldType.name == "i16" || fieldType.name == "i32");
                        lastValue_ = emitBitfieldLoad(addr, field.bitOffset, field.bitWidth, isSigned);
                    } else if (isAddress_ || fieldIsStruct || fieldIsArray) {
                        lastValue_ = addr;
                    } else {
                        IRType fieldIRType = (fieldType.pointerLevel > 0) ? IRType::PTR : mapType(fieldType.name);
                        IRValue dest = IRValue::Register(nextTemp(), fieldIRType);
                        currentBlock_->addInstruction({IROp::LOAD, dest, {addr, IRValue::Constant(0, IRType::I32)}});
                        lastValue_ = dest;
                    }
                    lastExprType_ = fieldType;
                    return;
                }
            }
        }

        // CIDER-BUG-024: this fallback used to emit a named load that
        // codegen lowered as a 32-bit load at offset 0 when a nominal type
        // could not be resolved — silent garbage. The checker rejects
        // unresolved members first, so reaching here is an internal
        // inconsistency: report it (the driver aborts before codegen)
        // instead of emitting a wrong load.
        if (reporter_) {
            reporter_->error("cannot resolve member '" + expr.getMember() +
                                 "' on type " + objType.toString(), expr.getLine());
        }
        IRValue dest = IRValue::Register(nextTemp(), IRType::I32);
        currentBlock_->addInstruction({IROp::LOAD, dest, {obj, IRValue::Label(expr.getMember())}});
        lastValue_ = dest;
    }

    void visit(StructLiteralExpr& expr) override {
        IRValue dest = IRValue::Register(nextTemp(), IRType::PTR);
        currentBlock_->addInstruction({IROp::ALLOCA, dest, {IRValue::Constant(getTypeSize({expr.getStructName(), 0}), IRType::I32)}});

        auto sym = symbolTable_.resolve(expr.getStructName());
        if (sym && (sym->kind == SymbolKind::STRUCT || sym->kind == SymbolKind::CLASS)) {
            auto structSym = std::static_pointer_cast<StructSymbol>(sym);
            for (auto& fieldInit : expr.getFields()) {
                fieldInit.second->accept(*this);
                IRValue val = lastValue_;

                int offset = 0;
                TypeInfo fieldType;
                for (const auto& f : structSym->fields) {
                    if (f.name == fieldInit.first) {
                        offset = f.offset;
                        fieldType = f.type;
                        break;
                    }
                }
                // CID-003: an aggregate literal's field stores at the
                // declared width, exactly like a variable store (a literal
                // f32 into an f64 field must not write 4 bytes).
                val = convertValueToType(val, fieldType);
                currentBlock_->addInstruction({IROp::STORE, std::nullopt, {dest, val, IRValue::Constant(offset, IRType::I32)}});
            }
        }
        lastValue_ = dest;
        lastExprType_ = {expr.getStructName(), 0};
    }

    IRValue getAddress(Expression* expr) {
        bool wasAddress = isAddress_;
        isAddress_ = true;
        expr->accept(*this);
        IRValue addr = lastValue_;
        isAddress_ = wasAddress;
        return addr;
    }

    void visit(UnaryExpr& expr) override {
        TypeInfo type = lastExprType_;

        if (expr.getOperator() == "&") {
            isAddress_ = true;
            expr.getOperand()->accept(*this);
            isAddress_ = false;
            type = lastExprType_;
            type.pointerLevel++;
            lastExprType_ = type;
        } else if (expr.getOperator() == "*") {
            expr.getOperand()->accept(*this);
            IRValue operand = lastValue_;
            type = lastExprType_;
            if (isAddress_) {
                lastValue_ = operand;
            } else {
                // Dereferencing a pointer-to-struct/array yields the object
                // itself, whose address is already `operand`. Emitting a LOAD
                // here would read the first 8 bytes as a pointer and break
                // `var q: T = *p;` (Issue #12). Only scalar/pointer pointee
                // types go through LOAD.
                TypeInfo pointee = type;
                if (pointee.pointerLevel > 0) pointee.pointerLevel--;
                bool pointeeIsAggregate = (pointee.pointerLevel == 0) &&
                    (pointee.isArray || isStructValueType(pointee) ||
                     isUniqueModuleAggregate(pointee.name));
                if (pointeeIsAggregate) {
                    lastValue_ = operand;
                    type = pointee;
                    lastExprType_ = type;
                    return;
                }
                IRType derefType = (type.pointerLevel > 1) ? IRType::PTR : mapType(type.name);
                IRValue dest = IRValue::Register(nextTemp(), derefType);
                currentBlock_->addInstruction({IROp::LOAD, dest, {operand, IRValue::Constant(0, IRType::I32)}});
                lastValue_ = dest;
            }
            if (type.pointerLevel > 0) type.pointerLevel--;
            lastExprType_ = type;
        } else if (expr.getOperator() == "++" || expr.getOperator() == "--") {
            bool isPostfix = expr.isPostfix();
            isAddress_ = true;
            expr.getOperand()->accept(*this);
            IRValue addr = lastValue_;
            isAddress_ = false;
            TypeInfo operandType = lastExprType_;

            IRType irType = (operandType.pointerLevel > 0) ? IRType::PTR : mapType(operandType.name);
            IRValue current = IRValue::Register(nextTemp(), irType);
            currentBlock_->addInstruction({IROp::LOAD, current, {addr, IRValue::Constant(0, IRType::I32)}});

            IRValue one = IRValue::Constant(1, irType);
            IRValue result = IRValue::Register(nextTemp(), irType);
            currentBlock_->addInstruction({expr.getOperator() == "++" ? IROp::ADD : IROp::SUB, result, {current, one}});

            if (addr.kind == IRValue::LABEL) {
                currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, result}});
            } else {
                currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, result, IRValue::Constant(0, IRType::I32)}});
            }

            type = operandType;
            // Postfix yields the original value; prefix yields the new value.
            lastValue_ = isPostfix ? current : result;
        } else {
            expr.getOperand()->accept(*this);
            IRValue operand = lastValue_;
            type = lastExprType_;

            if (expr.getOperator() == "~") {
                IRType irType = (type.pointerLevel > 0) ? IRType::PTR : mapType(type.name);
                IRValue dest = IRValue::Register(nextTemp(), irType);
                currentBlock_->addInstruction({IROp::BIT_NOT, dest, {operand}});
                lastValue_ = dest;
            } else if (expr.getOperator() == "!" || expr.getOperator() == "not") {
                // CIDER-BUG-046: `not` is the word spelling of `!` and
                // must lower to the same boolean negation.
                IRValue dest = IRValue::Register(nextTemp(), IRType::BOOL);
                currentBlock_->addInstruction({IROp::BIT_XOR, dest, {operand, IRValue::Constant(1, IRType::BOOL)}});
                lastValue_ = dest;
                type = {"bool", 0};
            } else if (expr.getOperator() == "-") {
                IRType irType = (type.pointerLevel > 0) ? IRType::PTR : mapType(type.name);
                if ((irType == IRType::F32 || irType == IRType::F64) &&
                    operand.kind == IRValue::CONSTANT) {
                    // CID-003 follow-up (M2): fold -literal to a negative
                    // float constant. `0.0 - 0.1` ran at the literal's f32
                    // width, so a later f64 context could only widen the
                    // f32 result — `var b: f64 = -0.1` was not the nearest
                    // double to -0.1. Keeping it a constant lets
                    // convertFloatWidth carry the parsed double.
                    lastValue_ = IRValue::Constant(-operand.constValue, irType);
                } else {
                    IRValue dest = IRValue::Register(nextTemp(), irType);
                    currentBlock_->addInstruction({IROp::SUB, dest, {IRValue::Constant(0, irType), operand}});
                    lastValue_ = dest;
                }
            } else {
                lastValue_ = operand;
            }
        }
        lastExprType_ = type;
    }

    void visit(VarDeclStmt& stmt) override {
        auto sym = std::make_shared<Symbol>();
        sym->kind = SymbolKind::VARIABLE;
        sym->name = stmt.getName();
        sym->type = TypeInfo::fromAST(stmt.getTypeAST());
        if (!sym->type.isInterface && sym->type.pointerLevel == 0) {
            auto typeSym = symbolTable_.resolve(sym->type.name);
            if (typeSym && typeSym->kind == SymbolKind::INTERFACE) {
                sym->type.isInterface = true;
            }
        }
        // CIDER-BUG-048: a global `const` keeps its isConst marker and
        // initializer AST so tryEvalConstU64 can fold references to it
        // exactly (recursively). Locals are excluded on purpose:
        // visit(IdentifierExpr) folds sym->constValue for any isConst symbol
        // without global storage, and that is 0 for const variables — a
        // local const read must keep loading its stored value.
        if (!currentFunction_ && stmt.isConst()) {
            sym->isConst = true;
            sym->constInit = stmt.getInitializer();
        }
        // CIDER-BUG-048: this fresh symbol replaces the collector's/checker's
        // in the shared scope; keep its bind-time defining-module provenance
        // so const folding (and mangling) still resolves the declaration's
        // own module after replacement.
        if (auto prev = symbolTable_.getCurrentScope()->resolveLocal(sym->name)) {
            auto pit = symbolModules_.find(prev->id);
            if (pit != symbolModules_.end()) symbolModules_[sym->id] = pit->second;
        }
        symbolTable_.define(sym);

        if (stmt.isExtern()) {
            // Linker-provided symbol (CID-001): no storage emitted, just an
            // extern directive; uses address it like a global. The VM has no
            // linker, so running such programs fails there with a clear
            // unresolved-label error. CID-006 F1: variables are registered
            // as extern DATA — treating them as extern functions made the VM
            // fabricate a function VALUE and OOB-read the heap.
            module_.addExternVariable(stmt.getName());
            globalValues_[stmt.getName()] = IRValue::Label(stmt.getName());
            return;
        }

        if (!currentFunction_) {
            // CID-020/021 (spec §5): module-scope globals emit under the
            // defining module's mangled name; main-file globals stay bare.
            std::string gname = emitGlobalName(currentModule_, stmt.getName());
            if (auto strInit = dynamic_cast<StringExpr*>(stmt.getInitializer())) {
                std::string strLabel = emitStringData(strInit->getValue());
                module_.addStringData(gname + ": dq " + strLabel);
                globalValues_[gname] = IRValue::Label(gname);
                return;
            }
            // CID-005: wide string globals. An array global IS the dw data
            // (no NUL wrapper cell); a pointer global gets a `dq` cell
            // pointing at a fresh wide data label.
            if (auto ws = dynamic_cast<WideStringExpr*>(stmt.getInitializer())) {
                auto emitWideData = [&](const std::string& label) {
                    std::string line = label + ": dw ";
                    const auto& units = ws->getValue();
                    for (size_t i = 0; i < units.size(); ++i) {
                        if (i) line += ", ";
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "0x%04X", (unsigned)units[i]);
                        line += buf;
                    }
                    if (!units.empty()) line += ", ";
                    line += "0x0000";
                    module_.addStringData("align 2");
                    module_.addStringData(line);
                };
                if (sym->type.isArray && sym->type.pointerLevel == 0) {
                    emitWideData(gname);
                    globalValues_[gname] = IRValue::Label(gname);
                } else {
                    std::string strLabel = "_gws" + std::to_string(globalStrCount_++);
                    emitWideData(strLabel);
                    module_.addStringData(gname + ": dq " + strLabel);
                    globalValues_[gname] = IRValue::Label(gname);
                }
                return;
            }
            // Fixed-size array from a list literal (CID-007): with all
            // elements constant (guaranteed by the type checker for
            // globals), emit the bytes directly into .data. Otherwise fall
            // through to the legacy BSS path below.
            if (sym->type.isArray && sym->type.pointerLevel == 0 && sym->type.arraySize > 0) {
                if (auto list = dynamic_cast<ListExpr*>(stmt.getInitializer())) {
                    auto& elems = list->getElements();
                    TypeInfo elemType = sym->type;
                    elemType.isArray = false;
                    elemType.arraySize = 0;
                    int elemSize = getTypeSize(elemType);
                    IRType elemIR = mapType(elemType.name);
                    std::string dataLine = gname + ": db ";
                    bool allConst = ((int)elems.size() == sym->type.arraySize);
                    for (size_t ei = 0; allConst && ei < elems.size(); ei++) {
                        uint64_t v = 0;
                        if (elemIR == IRType::F32 || elemIR == IRType::F64) {
                            // CID-003 follow-up (M3c): a float-typed element
                            // converts its constant to the element width's
                            // IEEE-754 bits. An integer element used to emit
                            // its raw integer bits (a denormal when read as
                            // f32/f64); float literal elements are rejected
                            // by the checker before this point.
                            double fv = 0;
                            if (!tryEvalConstF64(elems[ei].get(), fv)) { allConst = false; break; }
                            if (elemIR == IRType::F32) {
                                float f = (float)fv;
                                uint32_t b32 = 0;
                                std::memcpy(&b32, &f, sizeof(b32));
                                v = b32;
                            } else {
                                std::memcpy(&v, &fv, sizeof(v));
                            }
                        } else if (!tryEvalConstU64(elems[ei].get(), v)) {
                            allConst = false;
                            break;
                        }
                        for (int b = 0; b < elemSize; b++) {
                            if (ei > 0 || b > 0) dataLine += ", ";
                            dataLine += std::to_string((v >> (8 * b)) & 0xFFULL);
                        }
                    }
                    if (allConst) {
                        module_.addStringData(dataLine);
                        globalValues_[gname] = IRValue::Label(gname);
                        return;
                    }
                }
            }
            // CID-006: a global function pointer becomes a .data cell holding
            // the function label (native) or its id (VM dq pass). The same
            // cell fixes the legacy `*void g = f;` global, which used to be a
            // silent constant zero on native. nilPtr / uninitialized fn
            // globals stay .bss-zero; the checker rejects every other shape.
            // Task 2 F1 carry-over: alias spellings (`type Cb = ...`) carry
            // the alias name in sym->type, so resolve first — otherwise an
            // alias global falls through to the legacy slot sized by the
            // alias name instead of getting the real cell.
            TypeInfo fnDecl = resolveFunctionPointerAlias(sym->type);
            TypeInfo declAlias = resolveTypeAlias(sym->type);
            std::string fnTarget = functionPointerInitTarget(stmt.getInitializer());
            if (!fnTarget.empty() &&
                (fnDecl.isFunctionPointer || declAlias.pointerLevel > 0)) {
                module_.addStringData(gname + ": dq " + fnTarget);
                globalValues_[gname] = IRValue::Label(gname);
                return;
            }
            if (fnDecl.isFunctionPointer && !fnDecl.isArray) {
                module_.addGlobalVar(gname, IRType::PTR, 0, 0, 8, false);
                globalValues_[gname] = IRValue::Label(gname);
                return;
            }
            int64_t initVal = 0;
            bool initIsFloat = false;
            IRType declaredIR = (sym->type.pointerLevel > 0) ? IRType::PTR : mapType(sym->type.name);
            if (stmt.getInitializer()) {
                if (declaredIR == IRType::F32 || declaredIR == IRType::F64) {
                    // CID-003 follow-up (M3): float globals fold constant
                    // expressions and integer literals like the local path
                    // (`var g: f64 = 5`, `1.0 + 2.0`, `7 as f64`, mixed
                    // widths) and store the declared width's IEEE-754 bits
                    // (CID-029). CIDER-BUG-048: the checker gates
                    // non-constant float initializers; if a checker-accepted
                    // shape still cannot fold, report it instead of silently
                    // BSS-zeroing the slot.
                    double fv = 0;
                    if (tryEvalConstF64(stmt.getInitializer(), fv)) {
                        uint64_t bits = 0;
                        if (declaredIR == IRType::F32) {
                            float f = (float)fv;
                            uint32_t b32 = 0;
                            std::memcpy(&b32, &f, sizeof(b32));
                            bits = b32;
                        } else {
                            std::memcpy(&bits, &fv, sizeof(bits));
                        }
                        initVal = (int64_t)bits;
                        initIsFloat = true;
                    } else if (reporter_) {
                        reporter_->error("Global float initializer in declaration of '" +
                                             stmt.getName() + "' must use constant expressions",
                                         stmt.getLine());
                    }
                } else if (auto num = dynamic_cast<NumberExpr*>(stmt.getInitializer())) {
                    initVal = (int64_t)num->getUIntValue();
                } else {
                    // Constant expressions (CID-014): fold `1 << 4`,
                    // `0x10|0x20`, `~0x10`, ... so they land in .data
                    // instead of silently reading 0 from .bss.
                    uint64_t folded = 0;
                    if (tryEvalConstU64(stmt.getInitializer(), folded)) {
                        initVal = (int64_t)folded;
                    } else if (reporter_) {
                        // CIDER-BUG-048 safety net: the checker accepts only
                        // constant shapes, but a few cannot fold (division by
                        // zero, an oversized shift, a single-precision cast
                        // round-trip) and non-scalar aggregates have no
                        // supported initializer. Report them — never a
                        // silent zero.
                        reporter_->error("Global initializer in declaration of '" +
                                             stmt.getName() + "' must use constant expressions",
                                         stmt.getLine());
                    }
                }
            }
            int totalBytes = getTypeSize(sym->type);
            module_.addGlobalVar(gname, (sym->type.pointerLevel > 0) ? IRType::PTR : mapType(sym->type.name), initVal, sym->type.isArray ? sym->type.arraySize : 0, totalBytes, initIsFloat);
            globalValues_[gname] = IRValue::Label(gname);
            return;
        }

        IRValue addr = IRValue::Register(nextTemp(), IRType::PTR);
        currentBlock_->addInstruction({IROp::ALLOCA, addr, {IRValue::Constant(getTypeSize(sym->type), IRType::I32)}});
        variableValues_[stmt.getName()] = addr;

        // Check if this type has a destructor
        TypeInfo varType = sym->type;
        if (varType.pointerLevel == 0 && !varType.isArray) {
            auto typeSym = resolveNominalType(varType.name);
            if (typeSym && typeSym->kind == SymbolKind::CLASS) {
                auto classSym = std::static_pointer_cast<StructSymbol>(typeSym);
                if (!classSym->destructorName.empty()) {
                    // CID-020/021 (spec §5/§6): destructors mangle with the
                    // class's defining module, matching the definition.
                    auto dtorDef = resolveFunctionDef(classSym->destructorName,
                                                      definingScopeFor(classSym, classSym->name));
                    destructorStack_.back().push_back(
                        {emitFunctionName(dtorDef.second, classSym->destructorName), addr});
                }
                // CIDER-BUG-049: a value-class instance without an explicit
                // initializer runs the class's field initializers. With one
                // (`var v: C = other;`) the copy/memcpy below copies the
                // source object wholesale, so the source state wins.
                if (!stmt.getInitializer()) {
                    emitClassFieldInitializers(classSym, addr);
                }
            }
        }

        if (stmt.getInitializer()) {
            // Handle interface variable initialization: assign concrete type to interface
            bool isInterfaceInit = false;
            TypeInfo initTypeInfo;
            if (sym->type.isInterface) {
                stmt.getInitializer()->accept(*this);
                initTypeInfo = lastExprType_;
                IRValue srcAddr = lastValue_;
                // Store data pointer at [addr + 0]
                currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, srcAddr, IRValue::Constant(0, IRType::I32)}});
                // Compute [addr + 8] in a separate register to avoid corrupting addr
                IRValue addr8 = IRValue::Register(nextTemp(), IRType::PTR);
                currentBlock_->addInstruction({IROp::ADD, addr8, {addr, IRValue::Constant(8, IRType::I32)}});
                // Store vtable pointer at [addr + 8]
                std::string concreteTypeName = initTypeInfo.name;
                if (initTypeInfo.pointerLevel > 0) {
                    TypeInfo base = initTypeInfo;
                    base.pointerLevel--;
                    concreteTypeName = base.name;
                }
                // CID-020/021 (spec §6): must match generate()'s interface
                // vtable key — each part mangles by its own defining module
                // (see interfaceVtableLabel).
                std::string vtableLabel = interfaceVtableLabel(sym->type.name, concreteTypeName);
                currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr8, IRValue::Label(vtableLabel), IRValue::Constant(0, IRType::I32)}});
                lastExprType_ = sym->type;
                isInterfaceInit = true;
            }
            if (!isInterfaceInit) {
                // Fixed-size array from a list literal (CID-007): store
                // each element at its own offset with the element width
                // (a MEMCPY of the literal temp would copy I32-wide
                // elements into narrow slots).
                if (sym->type.isArray && sym->type.pointerLevel == 0 && sym->type.arraySize > 0) {
                    if (auto list = dynamic_cast<ListExpr*>(stmt.getInitializer())) {
                        auto& elems = list->getElements();
                        TypeInfo elemType = sym->type;
                        elemType.isArray = false;
                        elemType.arraySize = 0;
                        int elemSize = getTypeSize(elemType);
                        for (size_t ei = 0; ei < elems.size(); ei++) {
                            elems[ei]->accept(*this);
                            // CID-003: elements store at the declared width
                            // (an f32 literal in an f64 element must not
                            // write 4 bytes; an int element converts).
                            IRValue eval = convertValueToType(lastValue_, elemType);
                            currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, eval, IRValue::Constant((int64_t)(ei * (size_t)elemSize), IRType::I32)}});
                        }
                        lastExprType_ = sym->type;
                        return;
                    }
                }
                stmt.getInitializer()->accept(*this);
                IRValue val = lastValue_;
                if (sym->type.pointerLevel == 0) {
                    IRType declType = mapType(sym->type.name);
                    // Narrow/unsigned/bool float targets had no conversion
                    // at all (CID-025); I32/I64 keep the direct path below.
                    bool declNeedsFloatHelp = (declType == IRType::I8 || declType == IRType::U8 ||
                                               declType == IRType::I16 || declType == IRType::U16 ||
                                               declType == IRType::U32 || declType == IRType::U64 ||
                                               declType == IRType::BOOL);
                    if (isIntIRType(val.type) &&
                        (declType == IRType::F32 || declType == IRType::F64)) {
                        // CID-003 follow-up (I2): every integer width/sign
                        // converts, not just I32/I64.
                        val = convertIntToFloat(val, declType);
                    } else if ((val.type == IRType::F32 || val.type == IRType::F64) && declNeedsFloatHelp) {
                        val = convertFloatToInt(val, sym->type);
                    } else if ((val.type == IRType::F32 || val.type == IRType::F64) &&
                               (declType == IRType::I32 || declType == IRType::I64)) {
                        IRValue converted = IRValue::Register(nextTemp(), declType);
                        currentBlock_->addInstruction({IROp::CVT_FTOI, converted, {val}});
                        val = converted;
                    } else if ((val.type == IRType::F32 && declType == IRType::F64) ||
                               (val.type == IRType::F64 && declType == IRType::F32)) {
                        // CID-003: the declared type drives the width, so
                        // `var a: f64 = 2.5` widens its F32 literal (as an
                        // exact F64 constant) and `var b: float = aF64`
                        // narrows before the store.
                        val = convertFloatWidth(val, declType);
                    }
                }
                bool isStructType = isStructValueType(sym->type);
                bool initIsStruct = (lastExprType_.pointerLevel == 0) && (isStructValueType(lastExprType_) || lastExprType_.isUserDefined ||
                                    lastExprType_.isArray || getTypeSize(lastExprType_) > 8);
                if (sym->type.pointerLevel == 0 && (sym->type.isArray || sym->type.isUserDefined || isStructType || getTypeSize(sym->type) > 8 || initIsStruct)) {
                    currentBlock_->addInstruction({IROp::MEMCPY, std::nullopt, {addr, val, IRValue::Constant(getTypeSize(sym->type), IRType::I32)}});
                } else {
                    // Store at the declared width (same rule as fields and
                    // elements): a narrow initializer in a wide local slot
                    // (all slots are 8 bytes) leaves stale upper bytes that
                    // a later wide load reads as garbage — e.g. a limiting
                    // `var big: i64 = 0 - 5` compared against another i64.
                    if (sym->type.pointerLevel == 0 && !sym->type.isArray) {
                        IRType declIR = mapType(sym->type.name);
                        bool declIsInt = (declIR == IRType::I8 || declIR == IRType::U8 ||
                                          declIR == IRType::I16 || declIR == IRType::U16 ||
                                          declIR == IRType::I32 || declIR == IRType::U32 ||
                                          declIR == IRType::BOOL ||
                                          declIR == IRType::I64 || declIR == IRType::U64);
                        if (declIsInt) val = truncateIntToType(val, sym->type);
                    }
                    currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, val, IRValue::Constant(0, IRType::I32)}});
                }
            }
        }
    }

    int getTypeSize(TypeInfo type, const std::string& defModule = "") {
        if (type.pointerLevel > 0) return 8;
        if (type.isArray) {
            TypeInfo elemType = type;
            elemType.isArray = false;
            elemType.arraySize = 0;
            return type.arraySize * getTypeSize(elemType, defModule);
        }
        // CID-006 (Task 2 F1 carry-over): a fn(...) value is a code pointer
        // (8 bytes), including an alias spelling.
        if (type.isFunctionPointer) return 8;
        // CID-006 fix round 1 (F1): resolve `type X = Y` chains to a fixed
        // point before the nominal lookup. An unresolved alias spelling is a
        // plain Symbol, so the StructSymbol cast below used to read a garbage
        // totalSize off it (`resb 3` / `alloca 4` for alias chains).
        TypeInfo resolved = resolveTypeAlias(type, defModule);
        if (resolved.name != type.name || resolved.pointerLevel != type.pointerLevel ||
            resolved.isArray != type.isArray || resolved.isFunctionPointer != type.isFunctionPointer ||
            resolved.isInterface != type.isInterface || resolved.isSlice != type.isSlice) {
            return getTypeSize(resolved, defModule);
        }
        if (type.name == "u8" || type.name == "i8" || type.name == "char" || type.name == "byte" || type.name == "void") return 1;
        if (type.name == "u16" || type.name == "i16") return 2;
        if (type.name == "u32" || type.name == "i32" || type.name == "int" || type.name == "float" || type.name == "f32" || type.name == "bool") return 4;
        if (type.name == "u64" || type.name == "i64" || type.name == "f64") return 8;

        if (type.isInterface) return 16;
        auto sym = resolveNominalType(type.name);
        if (!sym && !defModule.empty()) sym = resolveCalleeNominal(type.name, defModule);
        if (sym && (sym->kind == SymbolKind::STRUCT || sym->kind == SymbolKind::CLASS)) {
            // Alias providers carry another name in type.name and are plain
            // Symbols, never StructSymbols (cycle / self-alias): sizing one
            // would read unrelated memory.
            if (!sym->type.name.empty()) return 8;
            return std::static_pointer_cast<StructSymbol>(sym)->totalSize;
        }
        if (sym && sym->kind == SymbolKind::INTERFACE) return 16;
        return 8;  // Default
    }

    // C usual arithmetic conversions for mixed-width integer arithmetic
    // (ADD/SUB/MUL/DIV/MOD): when one operand is 64-bit and the other is
    // a narrower integer of a different type, the narrower side is
    // converted to the 64-bit type — unsigned if either side is unsigned
    // — and the operation computes at 64 bits. Without this, `1000 /
    // u64var` typed the division by the I32 left operand, silently
    // truncating the divisor (and codegen's 32-bit DIV path cannot even
    // move a 64-bit source: `mov r11d, r64` is invalid NASM). Same-width
    // mixes and 32-bit-and-under arithmetic are untouched.
    // Returns the common type, or IRType::VOID when no conversion applies.
    static IRType mixedIntCommon(IRType a, IRType b) {
        auto is64 = [](IRType t) { return t == IRType::I64 || t == IRType::U64; };
        auto isNarrowInt = [](IRType t) {
            return t == IRType::I8 || t == IRType::U8 || t == IRType::I16 ||
                   t == IRType::U16 || t == IRType::I32 || t == IRType::U32 ||
                   t == IRType::BOOL;
        };
        auto isUnsigned = [](IRType t) {
            return t == IRType::U8 || t == IRType::U16 || t == IRType::U32 ||
                   t == IRType::U64 || t == IRType::BOOL;
        };
        if (a == b) return IRType::VOID;
        if (!((is64(a) && (isNarrowInt(b) || is64(b))) ||
              (is64(b) && (isNarrowInt(a) || is64(a))))) {
            return IRType::VOID;
        }
        return (isUnsigned(a) || isUnsigned(b)) ? IRType::U64 : IRType::I64;
    }

    // CID-003 follow-up (M5): common comparison domain for mixed
    // signed/unsigned integer operands. Equal widths (or a wider unsigned
    // operand) compare in the unsigned domain — the signed side converts
    // to the unsigned type, wrapping modulo 2^width; a wider signed operand
    // represents every narrower unsigned value, so the unsigned side
    // converts to it and the compare stays signed. This matches C usual
    // arithmetic conversions for 32/64-bit pairs, with one deliberate
    // deviation (recorded in the f64 design decisions log): equal-width
    // narrow pairs (i8/u8, i16/u16) also compare unsigned, where C integer
    // promotion would compare them signed after promotion to int.
    // Pairs with the same signedness return VOID: the existing per-operand
    // widening in codegen/VM already compares them correctly. BOOL counts
    // as signed 32-bit here, matching codegen's setcc selection.
    static IRType mixedCmpCommon(IRType a, IRType b) {
        auto isUnsigned = [](IRType t) {
            return t == IRType::U8 || t == IRType::U16 || t == IRType::U32 ||
                   t == IRType::U64;
        };
        auto widthOf = [](IRType t) -> int {
            if (t == IRType::I8 || t == IRType::U8) return 8;
            if (t == IRType::I16 || t == IRType::U16) return 16;
            if (t == IRType::I32 || t == IRType::U32 || t == IRType::BOOL) return 32;
            return 64;
        };
        if (a == b) return IRType::VOID;
        bool au = isUnsigned(a), bu = isUnsigned(b);
        if (au == bu) return IRType::VOID;
        IRType u = au ? a : b;
        IRType s = au ? b : a;
        return (widthOf(u) >= widthOf(s)) ? u : s;
    }

    // Convert an integer operand to an unsigned comparison type, wrapping
    // modulo 2^width(target) like a C cast to that unsigned type (a
    // narrower signed source sign-extends first, then keeps the low bits).
    // Constants fold; registers get a MOV whose declared type carries the
    // width, which is what codegen/VM widen the compare by.
    IRValue convertIntToUnsigned(IRValue v, IRType target) {
        if (v.type == target) return v;
        int width = (target == IRType::U8) ? 8 : (target == IRType::U16) ? 16
                  : (target == IRType::U32) ? 32 : 64;
        uint64_t mask = (width == 64) ? ~0ULL : ((1ULL << width) - 1);
        if (v.kind == IRValue::CONSTANT) {
            return IRValue::Constant((uint64_t)v.intValue & mask, target);
        }
        IRValue dest = IRValue::Register(nextTemp(), target);
        currentBlock_->addInstruction({IROp::MOV, dest, {v}});
        return dest;
    }

    // Convert an integer operand to the common 64-bit type in place:
    // constants fold (value-preserving extension), anything else goes
    // through MOV (both backends extend correctly on widening moves).
    void convertIntToCommon(IRValue& v, IRType common) {
        if (v.type == common) return;
        if (v.kind == IRValue::CONSTANT) {
            uint64_t u;
            bool srcSigned;
            if (v.type == IRType::I64) {
                u = (uint64_t)v.intValue;
                srcSigned = true;
            } else if (v.type == IRType::U64) {
                u = (uint64_t)v.intValue;
                srcSigned = false;
            } else {
                uint64_t mask = (v.type == IRType::I8 || v.type == IRType::U8) ? 0xFFULL :
                                (v.type == IRType::I16 || v.type == IRType::U16) ? 0xFFFFULL : 0xFFFFFFFFULL;
                u = (uint64_t)v.intValue & mask;
                srcSigned = (v.type == IRType::I8 || v.type == IRType::I16 || v.type == IRType::I32);
                if (srcSigned && (u & ((mask >> 1) + 1))) u |= ~mask;
            }
            if (common == IRType::U64) {
                v = IRValue::Constant(u, common);
            } else {
                v = IRValue::Constant((int64_t)u, common);
            }
        } else {
            IRValue conv = IRValue::Register(nextTemp(), common);
            currentBlock_->addInstruction({IROp::MOV, conv, {v}});
            v = conv;
        }
    }

    // CID-003 follow-up (I2): every integer width and signedness is an
    // int-to-float source. Declaration, cast, binary-expression,
    // argument/return and compound-assignment sites used to gate on
    // I32/I64 only, so u32/u64/narrow sources stored raw bits or were
    // converted as the wrong width on native.
    static bool isIntIRType(IRType t) {
        return t == IRType::I8 || t == IRType::U8 || t == IRType::I16 ||
               t == IRType::U16 || t == IRType::I32 || t == IRType::U32 ||
               t == IRType::I64 || t == IRType::U64 || t == IRType::BOOL;
    }

    // CID-003: convert a value to a float width, preserving constant
    // precision: a literal keeps its parsed double instead of detouring
    // through float (and a narrowing constant rounds once, at compile time).
    // `blk` lets the ternary merge emit a conversion inside the arm's own
    // block (the merge block does not dominate the PHI).
    IRValue convertFloatWidth(IRValue val, IRType target, BasicBlock* blk = nullptr) {
        if (val.type == target) return val;
        if (val.kind == IRValue::CONSTANT) {
            return IRValue::Constant(target == IRType::F32 ? (double)(float)val.constValue
                                                           : val.constValue,
                                     target);
        }
        IRValue converted = IRValue::Register(nextTemp(), target);
        (blk ? blk : currentBlock_.get())
            ->addInstruction({target == IRType::F64 ? IROp::CVT_F32F64
                                                    : IROp::CVT_F64F32,
                              converted, {val}});
        return converted;
    }

    // CID-003 follow-up (I2): integer -> float through CVT_ITOF. The
    // operand's declared width/sign is what codegen and the VM convert by
    // (i64 sign-extends, u32/u64 zero-extend, u64 is unsigned).
    IRValue convertIntToFloat(IRValue val, IRType target, BasicBlock* blk = nullptr) {
        IRValue converted = IRValue::Register(nextTemp(), target);
        (blk ? blk : currentBlock_.get())
            ->addInstruction({IROp::CVT_ITOF, converted, {val}});
        return converted;
    }

    // CID-003: apply a declared scalar type's conversions to a value at an
    // initializer/argument/return/store site, mirroring the declaration
    // path: int -> float via CVT_ITOF, float -> int via CVT_FTOI
    // (narrow/unsigned/bool targets through convertFloatToInt), float
    // widths through convertFloatWidth, integer narrowing through
    // truncateIntToType. Pointers and aggregates pass through untouched.
    IRValue convertValueToType(IRValue val, const TypeInfo& target) {
        if (target.pointerLevel > 0) return val;
        // CIDER-BUG-045: an array target (direct or alias spelling) is
        // address-like — scalar conversion would truncate the data label to
        // a 16-bit constant, so `return u"AB";` memcpy'd from address 0.
        TypeInfo resolvedTarget = resolveTypeAlias(target);
        if (target.isArray || resolvedTarget.isArray) return val;
        IRType targetIR = mapType(target.name);
        bool valIsFloat = (val.type == IRType::F32 || val.type == IRType::F64);
        bool valIsInt = isIntIRType(val.type);
        bool targetIsFloat = (targetIR == IRType::F32 || targetIR == IRType::F64);
        bool targetIsInt = (targetIR == IRType::I8 || targetIR == IRType::U8 ||
                            targetIR == IRType::I16 || targetIR == IRType::U16 ||
                            targetIR == IRType::I32 || targetIR == IRType::U32 ||
                            targetIR == IRType::I64 || targetIR == IRType::U64 ||
                            targetIR == IRType::BOOL);
        if (valIsFloat && targetIsFloat) return convertFloatWidth(val, targetIR);
        if (valIsFloat && targetIsInt) {
            if (targetIR == IRType::I32 || targetIR == IRType::I64) {
                IRValue converted = IRValue::Register(nextTemp(), targetIR);
                currentBlock_->addInstruction({IROp::CVT_FTOI, converted, {val}});
                return converted;
            }
            return convertFloatToInt(val, target);
        }
        if (valIsInt && targetIsFloat) {
            return convertIntToFloat(val, targetIR);
        }
        return truncateIntToType(val, target);
    }

    // CID-003: convert call arguments to the callee's declared parameter
    // types. `argBase` skips leading call-construction entries (an
    // interface call's method address and data pointer); `paramBase`
    // skips a hidden leading parameter (a method's `this`).
    void convertCallArgs(std::vector<IRValue>& args, size_t argBase,
                         const std::vector<TypeInfo>& paramTypes,
                         size_t paramBase = 0) {
        for (size_t i = 0; argBase + i < args.size() && paramBase + i < paramTypes.size(); ++i) {
            args[argBase + i] = convertValueToType(args[argBase + i], paramTypes[paramBase + i]);
        }
    }

    void visit(BinaryExpr& expr) override {
        expr.getLeft()->accept(*this);
        IRValue left = lastValue_;
        TypeInfo leftType = lastExprType_;

        std::string sOp = expr.getOperator();

        // CIDER-BUG-046: the word forms `and`/`or` are synonyms for
        // `&&`/`||`, so they share the short-circuit lowering below.
        if (sOp == "&&" || sOp == "and") {
            IRValue condReg = lastValue_;
            std::string scTrueName = nextLabel("sc_true");
            std::string scFalseName = nextLabel("sc_false");
            std::string scMergeName = nextLabel("sc_merge");
            currentBlock_->addInstruction({IROp::COND_BR, std::nullopt, {condReg, IRValue::Label(scTrueName), IRValue::Label(scFalseName)}});
            startBlock(scTrueName);
            expr.getRight()->accept(*this);
            IRValue rightVal = lastValue_;
            TypeInfo rightType = lastExprType_;
            currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(scMergeName)}});
            std::string trueEndName = currentBlock_->getName();
            startBlock(scFalseName);
            IRValue falseVal = IRValue::Constant(0, IRType::BOOL);
            currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(scMergeName)}});
            std::string falseEndName = currentBlock_->getName();
            startBlock(scMergeName);
            IRValue phiDest = IRValue::Register(nextTemp(), IRType::BOOL);
            IRInstruction phiInstr;
            phiInstr.op = IROp::PHI;
            phiInstr.dest = phiDest;
            phiInstr.phiArgs[trueEndName] = rightVal;
            phiInstr.phiArgs[falseEndName] = falseVal;
            currentBlock_->addInstruction(phiInstr);
            lastValue_ = phiDest;
            lastExprType_ = rightType.pointerLevel > 0 ? rightType : TypeInfo{"bool", 0};
            return;
        }

        if (sOp == "||" || sOp == "or") {
            IRValue condReg = lastValue_;
            std::string scTrueName = nextLabel("sc_true");
            std::string scFalseName = nextLabel("sc_false");
            std::string scMergeName = nextLabel("sc_merge");
            currentBlock_->addInstruction({IROp::COND_BR, std::nullopt, {condReg, IRValue::Label(scTrueName), IRValue::Label(scFalseName)}});
            startBlock(scTrueName);
            IRValue trueVal = IRValue::Constant(1, IRType::BOOL);
            currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(scMergeName)}});
            std::string trueEndName = currentBlock_->getName();
            startBlock(scFalseName);
            expr.getRight()->accept(*this);
            IRValue rightVal = lastValue_;
            TypeInfo rightType = lastExprType_;
            currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(scMergeName)}});
            std::string falseEndName = currentBlock_->getName();
            startBlock(scMergeName);
            IRValue phiDest = IRValue::Register(nextTemp(), IRType::BOOL);
            IRInstruction phiInstr;
            phiInstr.op = IROp::PHI;
            phiInstr.dest = phiDest;
            phiInstr.phiArgs[trueEndName] = trueVal;
            phiInstr.phiArgs[falseEndName] = rightVal;
            currentBlock_->addInstruction(phiInstr);
            lastValue_ = phiDest;
            lastExprType_ = rightType.pointerLevel > 0 ? rightType : TypeInfo{"bool", 0};
            return;
        }

        expr.getRight()->accept(*this);
        IRValue right = lastValue_;
        TypeInfo rightType = lastExprType_;

        IROp op;

        if (leftType.pointerLevel > 0 && (sOp == "+" || sOp == "-")) {
            TypeInfo baseType = leftType;
            baseType.pointerLevel--;
            int size = getTypeSize(baseType);
            if (size > 1) {
                IRValue scaled = IRValue::Register(nextTemp(), right.type);
                currentBlock_->addInstruction({IROp::MUL, scaled, {right, IRValue::Constant(size, IRType::I32)}});
                right = scaled;
            }
        } else if (rightType.pointerLevel > 0 && sOp == "+") {
            TypeInfo baseType = rightType;
            baseType.pointerLevel--;
            int size = getTypeSize(baseType);
            if (size > 1) {
                IRValue scaled = IRValue::Register(nextTemp(), left.type);
                currentBlock_->addInstruction({IROp::MUL, scaled, {left, IRValue::Constant(size, IRType::I32)}});
                left = scaled;
            }
        }

        // Implicit int-to-float conversion for mixed operands (CID-003
        // follow-up I2: every integer width/sign, not just I32/I64).
        if ((left.type == IRType::F32 || left.type == IRType::F64) &&
            isIntIRType(right.type)) {
            right = convertIntToFloat(right, left.type);
        } else if ((right.type == IRType::F32 || right.type == IRType::F64) &&
                   isIntIRType(left.type)) {
            left = convertIntToFloat(left, right.type);
        }

        // CID-003: mixed float widths compute at f64 (C usual conversions).
        // convertFloatWidth folds a constant operand to its exact F64 value:
        // a literal in an f64 context must not detour through the F32
        // constant pool, where 16777217.0 rounds to 16777216.0.
        bool widenedFloats = false;
        if ((left.type == IRType::F32 && right.type == IRType::F64) ||
            (left.type == IRType::F64 && right.type == IRType::F32)) {
            IRValue& narrow = (left.type == IRType::F32) ? left : right;
            narrow = convertFloatWidth(narrow, IRType::F64);
            widenedFloats = true;
        }

        // CID-003 follow-up (M5): mixed signed/unsigned integer
        // comparisons need a common domain (see mixedCmpCommon). Without
        // it, codegen picked setcc from the left operand's type while the
        // VM compared the per-operand-extended values signed, so
        // `-1 < u32(1)` was true everywhere and `i32(-1) != u32(max)`
        // disagreed between backends.
        if ((sOp == "==" || sOp == "!=" || sOp == "<" || sOp == ">" ||
             sOp == "<=" || sOp == ">=") &&
            leftType.pointerLevel == 0 && rightType.pointerLevel == 0) {
            IRType common = mixedCmpCommon(left.type, right.type);
            if (common == IRType::U8 || common == IRType::U16 ||
                common == IRType::U32 || common == IRType::U64) {
                left = convertIntToUnsigned(left, common);
                right = convertIntToUnsigned(right, common);
            } else if (common != IRType::VOID) {
                convertIntToCommon(left, common);
                convertIntToCommon(right, common);
            }
        }

        // Usual arithmetic conversions for mixed-width integer +,-,*,/,%
        // (see mixedIntCommon): compute at 64 bits so narrow operands
        // never truncate a 64-bit peer.
        bool widenedInts = false;
        if ((sOp == "+" || sOp == "-" || sOp == "*" || sOp == "/" || sOp == "%") &&
            leftType.pointerLevel == 0 && rightType.pointerLevel == 0) {
            IRType common = mixedIntCommon(left.type, right.type);
            if (common != IRType::VOID) {
                convertIntToCommon(left, common);
                convertIntToCommon(right, common);
                widenedInts = true;
            }
        }

        if (sOp == "+") op = IROp::ADD;
        else if (sOp == "-") op = IROp::SUB;
        else if (sOp == "*") op = IROp::MUL;
        else if (sOp == "/") op = IROp::DIV;
        else if (sOp == "%") op = IROp::MOD;
        else if (sOp == "==") op = IROp::CMP_EQ;
        else if (sOp == "!=") op = IROp::CMP_NE;
        else if (sOp == "<") op = IROp::CMP_LT;
        else if (sOp == ">") op = IROp::CMP_GT;
        else if (sOp == "<=") op = IROp::CMP_LE;
        else if (sOp == ">=") op = IROp::CMP_GE;
        else if (sOp == "&") op = IROp::BIT_AND;
        else if (sOp == "|") op = IROp::BIT_OR;
        else if (sOp == "^") op = IROp::BIT_XOR;
        else if (sOp == "<<") op = IROp::LSHIFT;
        else if (sOp == ">>") op = IROp::RSHIFT;
        else return;

        IRValue dest = IRValue::Register(nextTemp(), left.type);
        if (op >= IROp::CMP_EQ && op <= IROp::CMP_GE) {
            dest.type = IRType::I32;
        }
        currentBlock_->addInstruction({op, dest, {left, right}});
        lastValue_ = dest;
        if (widenedInts) {
            lastExprType_ = (left.type == IRType::U64) ? TypeInfo{"u64", 0} : TypeInfo{"i64", 0};
        } else if (widenedFloats) {
            lastExprType_ = (op >= IROp::CMP_EQ && op <= IROp::CMP_GE) ? TypeInfo{"bool", 0} : TypeInfo{"f64", 0};
        } else {
            lastExprType_ = (op >= IROp::CMP_EQ && op <= IROp::CMP_GE) ? TypeInfo{"bool", 0} : leftType;
        }
    }

    void visit(NumberExpr& expr) override {
        if (expr.isFloat()) {
            double val = expr.getFloatValue();
            lastExprType_ = {"float", 0};
            lastValue_ = IRValue::Constant(val, IRType::F32);
        } else {
            uint64_t v = expr.getUIntValue();
            // Select the narrowest type that holds the literal: small values
            // stay I32 ("int") for backward compatibility; values with bit 31
            // set become U32 (zero-extended, never sign-extended); larger
            // values become U64/I64 so 64-bit addresses survive.
            if (v <= (uint64_t)INT32_MAX) {
                lastExprType_ = {"int", 0};
                lastValue_ = IRValue::Constant((int64_t)v, IRType::I32);
            } else if (v <= (uint64_t)UINT32_MAX) {
                lastExprType_ = {"u32", 0};
                lastValue_ = IRValue::Constant(v, IRType::U32);
            } else if (v <= (uint64_t)INT64_MAX) {
                lastExprType_ = {"u64", 0};
                lastValue_ = IRValue::Constant((int64_t)v, IRType::I64);
            } else {
                lastExprType_ = {"u64", 0};
                lastValue_ = IRValue::Constant(v, IRType::U64);
            }
        }
    }

    void visit(CharExpr& expr) override {
        lastValue_ = IRValue::Constant(expr.getValue(), IRType::U8);
        lastExprType_ = {"char", 0};
    }

    void visit(IdentifierExpr& expr) override {
        if (expr.getName() == "_") {
            lastValue_ = IRValue::Constant(1, IRType::BOOL);
            lastExprType_ = {"bool", 0};
            return;
        }

        auto sym = symbolTable_.resolve(expr.getName());
        if (sym) {
            lastExprType_ = canonicalizeSymbolType(sym, expr.getName());
            if (lastExprType_.pointerLevel == 0) {
                // CIDER-BUG-015/019: follow `type X = T` chains before
                // classifying. An alias-to-array value is address-like
                // (isArray), an alias-to-struct value is user-defined, and
                // an alias-to-fn value carries its signature; presenting
                // the resolved type keeps indexing and calls consistent.
                // (Non-alias types are returned unchanged.)
                lastExprType_ = resolveTypeAlias(lastExprType_);
                if (resolveStructOrClassSymbol(lastExprType_)) {
                    lastExprType_.isUserDefined = true;
                } else {
                    auto typeSym = resolveNominalType(lastExprType_.name);
                    if (typeSym && (typeSym->kind == SymbolKind::STRUCT || typeSym->kind == SymbolKind::CLASS)) {
                        lastExprType_.isUserDefined = typeSym->type.name.empty();
                    }
                }
            } else {
                lastExprType_.isUserDefined = false;
            }
        }

        // CID-020: a `const` that was emitted as a global (e.g. a module-level
        // const reached here through a from-import alias) reads from storage:
        // the collector never evaluates const initializers into constValue,
        // so folding here would bake in 0. True immediates without storage
        // (enum variants) still fold below. The key mangles by defining
        // module (spec §5), same as every global reference below.
        if (sym && sym->isConst && !globalValues_.count(globalRefName(sym, expr.getName()))) {
            IRType irType = (sym->type.pointerLevel > 0) ? IRType::PTR : mapType(sym->type.name);
            lastValue_ = IRValue::Constant(sym->constValue, irType);
            return;
        }

        if (sym && sym->kind == SymbolKind::FUNCTION) {
            // CID-020/021 (spec §5): function values label the mangled
            // definition name. CID-006: the value's type is the fn
            // signature (labels and mangling are unchanged).
            lastValue_ = IRValue::Label(emitFunctionName(definingModuleFor(sym, expr.getName()), sym->name));
            auto fsym = std::static_pointer_cast<FunctionSymbol>(sym);
            TypeInfo ft;
            ft.name = "fn";
            ft.isFunctionPointer = true;
            ft.isMsAbi = (fsym->abi == Abi::Ms) || (fsym->abi == Abi::Default && defaultAbiMs_);
            ft.fnParamTypes = std::make_shared<std::vector<TypeInfo>>(fsym->parameterTypes);
            ft.fnReturnType = std::make_shared<TypeInfo>(fsym->returnType);
            lastExprType_ = ft;
            return;
        }

        if (isAddress_) {
            if (variableValues_.count(expr.getName())) {
                lastValue_ = variableValues_[expr.getName()];
                return;
            }
            // CID-020/021 (spec §5): globals key by defining module, so a
            // bare use (in-module or via from-alias) hits the mangled name.
            std::string gkey = globalRefName(sym, expr.getName());
            if (globalValues_.count(gkey)) {
                lastValue_ = globalValues_[gkey];
                return;
            }
        }

        if (lastExprType_.isArray || lastExprType_.isUserDefined || lastExprType_.isInterface) {
            lastValue_ = getAddress(&expr);
            return;
        }

        if (variableValues_.count(expr.getName())) {
            if (paramNames_.count(expr.getName()) && !assignedParams_.count(expr.getName())) {
                IRType irType = (lastExprType_.pointerLevel > 0) ? IRType::PTR : mapType(lastExprType_.name);
                lastValue_ = IRValue::Register("arg_" + expr.getName(), irType);
            } else {
                IRValue addr = variableValues_[expr.getName()];
                IRType irType = (lastExprType_.pointerLevel > 0) ? IRType::PTR : mapType(lastExprType_.name);
                IRValue dest = IRValue::Register(nextTemp(), irType);
                currentBlock_->addInstruction({IROp::LOAD, dest, {addr, IRValue::Constant(0, IRType::I32)}});
                lastValue_ = dest;
            }
        } else if (globalValues_.count(globalRefName(sym, expr.getName()))) {
            lastValue_ = globalValues_[globalRefName(sym, expr.getName())];
            if (!isAddress_) {
                IRType irType = (lastExprType_.pointerLevel > 0) ? IRType::PTR : mapType(lastExprType_.name);
                IRValue dest = IRValue::Register(nextTemp(), irType);
                currentBlock_->addInstruction({IROp::LOAD, dest, {lastValue_}});
                lastValue_ = dest;
            }
        } else {
            // CIDER-BUG-044: this fallback used to silently emit a register
            // read named after the identifier; an unresolved global then
            // read a stale temp (the port's `g_my_ip` read the address of a
            // nearby string). The checker rejects undefined identifiers
            // first, so reaching here is an internal inconsistency: report
            // it (the driver aborts before codegen).
            if (reporter_) {
                reporter_->error("cannot resolve identifier '" + expr.getName() +
                                     "' (unresolved value reference)", expr.getLine());
            }
            lastValue_ = IRValue::Register(expr.getName(), IRType::I32);
            lastExprType_ = {"int", 0};
        }
    }

    // Pre-pass: collect every parameter that is assigned ANYWHERE in the
    // function body. Read-through (param as register) is only sound when
    // the param is never written — otherwise a read in an earlier block
    // (e.g. a loop header generated before the assignment) would keep
    // reading the stale original register.
    void collectAssignedParams(Statement* stmt) {
        if (!stmt) return;
        if (auto assign = dynamic_cast<AssignStmt*>(stmt)) {
            if (auto ident = dynamic_cast<IdentifierExpr*>(assign->getTarget())) {
                assignedParams_.insert(ident->getName());
            }
        } else if (auto iff = dynamic_cast<IfStmt*>(stmt)) {
            for (auto& s : iff->getThenBranch()) collectAssignedParams(s.get());
            for (auto& s : iff->getElseBranch()) collectAssignedParams(s.get());
        } else if (auto wh = dynamic_cast<WhileStmt*>(stmt)) {
            for (auto& s : wh->getBody()) collectAssignedParams(s.get());
        } else if (auto blk = dynamic_cast<BlockStmt*>(stmt)) {
            for (auto& s : blk->getStatements()) collectAssignedParams(s.get());
        } else if (auto fr = dynamic_cast<ForStmt*>(stmt)) {
            collectAssignedParams(fr->getInitializer());
            collectAssignedParams(fr->getIncrement());
            for (auto& s : fr->getBody()) collectAssignedParams(s.get());
        } else if (auto fe = dynamic_cast<ForEachStmt*>(stmt)) {
            for (auto& s : fe->getBody()) collectAssignedParams(s.get());
        } else if (auto tc = dynamic_cast<TryCatchStmt*>(stmt)) {
            for (auto& s : tc->getTryBlock()) collectAssignedParams(s.get());
            for (auto& s : tc->getCatchBlock()) collectAssignedParams(s.get());
        } else if (auto df = dynamic_cast<DeferStmt*>(stmt)) {
            collectAssignedParams(df->getDeferredStmt());
        } else if (auto ms = dynamic_cast<MatchStmt*>(stmt)) {
            for (auto& arm : ms->getArms()) collectAssignedParams(arm.body.get());
        } else if (auto ex = dynamic_cast<ExportStmt*>(stmt)) {
            collectAssignedParams(ex->getStmt());
        }
    }

    // --- bitfield access lowering ---

    // Resolve an expression's declared type WITHOUT generating code (no
    // temp counters, no instructions) — for the bitfield lookup helpers.
    TypeInfo inferExprType(Expression* expr) {
        if (auto id = dynamic_cast<IdentifierExpr*>(expr)) {
            auto sym = symbolTable_.resolve(id->getName());
            if (sym) return sym->type;
        } else if (auto mem = dynamic_cast<MemberAccessExpr*>(expr)) {
            TypeInfo objType = inferExprType(mem->getObject());
            if (objType.pointerLevel > 0) objType.pointerLevel--;
            // CIDER-BUG-015: alias-safe struct lookup and field typing.
            auto sym = resolveStructOrClassSymbol(objType);
            if (sym) {
                auto ss = std::static_pointer_cast<StructSymbol>(sym);
                std::string ownerMod = definingModuleFor(sym, ss->name);
                for (const auto& f : ss->fields) {
                    if (f.name == mem->getMember()) {
                        return canonicalizeNominalInModule(resolveTypeAlias(f.type, ownerMod), ownerMod);
                    }
                }
            }
        } else if (auto idx = dynamic_cast<IndexExpr*>(expr)) {
            // Array/pointer element type (Issue #22): without this,
            // chained indexing like g_tasks[0].name[i] resolved to an
            // empty type, so the store path "widened" the char value to
            // a pointer width and emitted mov qword over the neighbors.
            TypeInfo baseType = inferExprType(idx->getObject());
            if (baseType.isArray) {
                baseType.isArray = false;
                baseType.arraySize = 0;
            } else if (baseType.pointerLevel > 0) {
                baseType.pointerLevel--;
            } else if (baseType.name == "string") {
                baseType.name = "char";
            }
            return baseType;
        } else if (auto un = dynamic_cast<UnaryExpr*>(expr)) {
            if (un->getOperator() == "*") {
                TypeInfo t = inferExprType(un->getOperand());
                if (t.pointerLevel > 0) t.pointerLevel--;
                return t;
            }
        } else if (auto group = dynamic_cast<GroupingExpr*>(expr)) {
            return inferExprType(group->getExpression());
        }
        return TypeInfo{};
    }

    bool getMemberBitfield(MemberAccessExpr* mem, int& bitOffset, int& bitWidth) {
        if (!mem) return false;
        // Resolve the object's type WITHOUT generating code: the accept()
        // would advance the temp counters and emit duplicate instructions.
        TypeInfo objType = inferExprType(mem->getObject());
        if (objType.pointerLevel > 0) objType.pointerLevel--;
        auto sym = resolveStructOrClassSymbol(objType);
        if (!sym) return false;
        auto structSym = std::static_pointer_cast<StructSymbol>(sym);
        for (const auto& field : structSym->fields) {
            if (field.name == mem->getMember() && field.bitWidth > 0) {
                bitOffset = field.bitOffset;
                bitWidth = field.bitWidth;
                return true;
            }
        }
        return false;
    }

    static int64_t bitfieldMask(int width) { return (1LL << width) - 1; }

    bool getMemberFieldType(MemberAccessExpr* mem, TypeInfo& out) {
        if (!mem) return false;
        TypeInfo objType = inferExprType(mem->getObject());
        if (objType.pointerLevel > 0) objType.pointerLevel--;
        auto sym = resolveStructOrClassSymbol(objType);
        if (!sym) return false;
        auto ss = std::static_pointer_cast<StructSymbol>(sym);
        std::string ownerMod = definingModuleFor(sym, ss->name);
        for (const auto& f : ss->fields) {
            if (f.name == mem->getMember()) {
                // CIDER-BUG-015/043: report the resolved, module-rooted
                // type so store widths and conversions see through
                // `type X = T` spellings and cross-module pointees.
                out = canonicalizeNominalInModule(resolveTypeAlias(f.type, ownerMod), ownerMod);
                return true;
            }
        }
        return false;
    }

    // Convert an integer IR value to a field/element type so the subsequent
    // STORE uses the field width (never clobbers neighbors, never leaves
    // upper garbage): narrow (`dt.month = 9` -> `mov byte`) via masking,
    // widen (`d.PhysicalStart = 0x80000000` U32 -> U64 -> `mov qword`)
    // via MOV (codegen zero/sign-extends).
    // Evaluate an integer constant expression for global initializers
    // (CID-014): `1 << 4`, `0x10|0x20`, `~0x10`, `(1<<4)`, casts of
    // constants, etc. Only folds what is exactly representable (integers,
    // chars, bools, groups, unary -/+/~, int binary ops with no div-by-zero
    // and shifts < 64). CIDER-BUG-048: an identifier naming a `const`
    // symbol folds by recursing into the declaration's initializer
    // (order-independently, cycle-safe); enum variants fold via constValue.
    // Anything else (floats, calls, non-const globals) returns false — the
    // checker rejects those shapes before IR generation, so no global is
    // silently BSS-zeroed.
    bool tryEvalConstU64(Expression* expr, uint64_t& out) {
        std::set<uint64_t> visiting;
        return tryEvalConstU64(expr, out, visiting);
    }

    bool tryEvalConstU64(Expression* expr, uint64_t& out, std::set<uint64_t>& visiting) {
        if (!expr) return false;
        if (auto num = dynamic_cast<NumberExpr*>(expr)) {
            if (num->isFloat()) return false;
            out = num->getUIntValue();
            return true;
        }
        if (auto ch = dynamic_cast<CharExpr*>(expr)) {
            out = (uint64_t)(unsigned char)ch->getValue();
            return true;
        }
        if (auto b = dynamic_cast<BooleanExpr*>(expr)) {
            out = b->getValue() ? 1 : 0;
            return true;
        }
        if (dynamic_cast<NilPtrExpr*>(expr)) {
            out = 0;
            return true;
        }
        if (auto grp = dynamic_cast<GroupingExpr*>(expr)) {
            return tryEvalConstU64(grp->getExpression(), out, visiting);
        }
        if (auto un = dynamic_cast<UnaryExpr*>(expr)) {
            uint64_t v = 0;
            if (!tryEvalConstU64(un->getOperand(), v, visiting)) return false;
            if (un->getOperator() == "-") { out = 0 - v; return true; }
            if (un->getOperator() == "~") { out = ~v; return true; }
            if (un->getOperator() == "+") { out = v; return true; }
            return false;
        }
        if (auto cast = dynamic_cast<CastExpr*>(expr)) {
            uint64_t v = 0;
            if (!tryEvalConstU64(cast->getExpression(), v, visiting)) return false;
            TypeInfo target = TypeInfo::fromAST(cast->getTargetType());
            if (target.pointerLevel > 0) return false;
            IRType irType = mapType(target.name);
            if (irType == IRType::I8 || irType == IRType::U8) v &= 0xFFULL;
            else if (irType == IRType::I16 || irType == IRType::U16) v &= 0xFFFFULL;
            else if (irType == IRType::I32 || irType == IRType::U32 || irType == IRType::BOOL) v &= 0xFFFFFFFFULL;
            else if (irType != IRType::I64 && irType != IRType::U64 && irType != IRType::PTR) return false;
            out = v;
            return true;
        }
        if (auto id = dynamic_cast<IdentifierExpr*>(expr)) {
            auto sym = resolveConstIdentifier(id->getName());
            return tryEvalConstSymbolU64(sym, out, visiting);
        }
        // CIDER-BUG-048: a module-qualified const (`mc.K`) folds like the
        // bare name (mirrors functionPointerInitTarget's module-member path).
        if (auto mem = dynamic_cast<MemberAccessExpr*>(expr)) {
            if (mem->isArrow()) return false;
            auto base = dynamic_cast<IdentifierExpr*>(mem->getObject());
            if (!base) return false;
            auto baseSym = resolveConstIdentifier(base->getName());
            if (!baseSym || baseSym->kind != SymbolKind::MODULE) return false;
            auto modSym = std::static_pointer_cast<ModuleSymbol>(baseSym);
            auto target = modSym->scope ? modSym->scope->resolveLocal(mem->getMember()) : nullptr;
            return tryEvalConstSymbolU64(target, out, visiting);
        }
        if (auto bin = dynamic_cast<BinaryExpr*>(expr)) {
            uint64_t l = 0, r = 0;
            if (!tryEvalConstU64(bin->getLeft(), l, visiting)) return false;
            if (!tryEvalConstU64(bin->getRight(), r, visiting)) return false;
            const std::string& op = bin->getOperator();
            if (op == "+") out = l + r;
            else if (op == "-") out = l - r;
            else if (op == "*") out = l * r;
            else if (op == "/") {
                if (r == 0) return false;
                out = l / r;
            } else if (op == "%") {
                if (r == 0) return false;
                out = l % r;
            } else if (op == "<<") {
                if (r >= 64) return false;
                out = l << r;
            } else if (op == ">>") {
                if (r >= 64) return false;
                out = l >> r;
            } else if (op == "&") out = l & r;
            else if (op == "|") out = l | r;
            else if (op == "^") out = l ^ r;
            else return false;
            return true;
        }
        return false;
    }

    // CIDER-BUG-048: fold an identifier that names a `const` symbol. A
    // const declaration recurses into its (exact) initializer; enum
    // variants and initializer-less consts carry constValue. `visiting`
    // breaks cycles (`const a = b; const b = a;`), which the checker
    // reports as non-constant before ir_gen runs.
    bool tryEvalConstSymbolU64(const std::shared_ptr<Symbol>& sym, uint64_t& out,
                               std::set<uint64_t>& visiting) {
        if (!sym || !sym->isConst || sym->kind != SymbolKind::VARIABLE) return false;
        if (!sym->constInit) {
            out = (uint64_t)(int64_t)sym->constValue;
            return true;
        }
        if (!visiting.insert(sym->id).second) return false;
        // Resolve the const's own initializer in its declaration scope, so
        // a module const that references an unexported sibling still folds
        // when the use site is another module.
        Scope* prevScope = constEvalScope_;
        if (Scope* declScope = scopeForModule(definingModuleFor(sym, sym->name))) {
            constEvalScope_ = declScope;
        }
        bool ok = tryEvalConstU64(sym->constInit, out, visiting);
        constEvalScope_ = prevScope;
        visiting.erase(sym->id);
        return ok;
    }

    // CIDER-BUG-048: resolve an identifier while folding const
    // initializers. Inside a const declaration, names bind in that
    // declaration scope (then the root); at top level they bind at the
    // use site.
    std::shared_ptr<Symbol> resolveConstIdentifier(const std::string& name) {
        if (!constEvalScope_) return symbolTable_.resolve(name);
        if (auto sym = constEvalScope_->resolveLocal(name)) return sym;
        Scope* root = symbolTable_.getRootScope();
        return root ? root->resolveLocal(name) : nullptr;
    }

    // Constant evaluation for global float initializers (CID-003 follow-up
    // M3). Evaluates the same shapes as tryEvalConstU64 plus float
    // arithmetic, and mirrors the local-variable path so a global and an
    // equivalent local agree bit for bit: integer subexpressions keep
    // integer semantics and wrap at their operand/expression type
    // (`5 / 2` is 2; `(-3000000000)` is the u32 value 1294967296, not a
    // 64-bit one), mixed int/float converts the integer to the float
    // operand's width (CVT_ITOF), and mixed f32/f64 computes at f64. A
    // bare float literal keeps its parsed double until a width context
    // rounds it, exactly like an IR F32 constant (see convertFloatWidth);
    // `literal` tracks that. Returns false for non-constant shapes (calls,
    // ternary, sizeof, struct literals, non-const identifiers); `const`
    // references fold recursively (CIDER-BUG-048). Global scalars cannot
    // materialize the rejected shapes, so the checker rejects them.
    struct ConstEvalValue {
        bool isFloat = false;
        bool isF64 = false;       // float width: false == f32
        bool literal = false;     // float: parsed literal, not yet width-rounded
        bool isUnsigned = false;  // integer signedness
        int intBits = 32;         // integer width for wrapping: 8/16/32/64
        double f = 0;
        uint64_t u = 0;           // canonical integer value (sign-extended if signed)
    };

    static double roundToF32(double v) { return (double)(float)v; }

    static uint64_t intWidthMask(int bits) {
        return (bits >= 64) ? ~0ULL : ((1ULL << bits) - 1);
    }

    // Canonicalize an integer to its declared width/sign the way the VM's
    // narrowInt/extendIntByType round trip leaves a register: keep the low
    // bits, sign-extend signed values. Every ConstEvalValue integer is kept
    // canonical so intConstToDouble and the next operation see the same
    // value a local register would hold.
    static uint64_t canonicalInt(uint64_t v, int bits, bool isUnsigned) {
        uint64_t mask = intWidthMask(bits);
        uint64_t m = v & mask;
        if (!isUnsigned && bits < 64 && (m & (1ULL << (bits - 1)))) m |= ~mask;
        return m;
    }

    static double intConstToDouble(const ConstEvalValue& v) {
        return v.isUnsigned ? (double)v.u : (double)(int64_t)v.u;
    }

    // CVT_ITOF to F32 rounds the integer once, directly to binary32 (not
    // through a binary64 intermediate).
    static double intConstToFloat32(const ConstEvalValue& v) {
        return v.isUnsigned ? (double)(float)(uint64_t)v.u
                            : (double)(float)(int64_t)v.u;
    }

    bool tryEvalConstValue(Expression* expr, ConstEvalValue& out) {
        if (!expr) return false;
        if (auto num = dynamic_cast<NumberExpr*>(expr)) {
            if (num->isFloat()) {
                out.isFloat = true;
                out.isF64 = false;
                out.literal = true;
                out.f = num->getFloatValue();
            } else {
                uint64_t v = num->getUIntValue();
                // Same literal typing as visit(NumberExpr): > INT32_MAX
                // becomes u32, > INT64_MAX becomes u64 (values up to
                // INT64_MAX keep the signed I64 IR type).
                out.isFloat = false;
                out.u = v;
                if (v <= (uint64_t)INT32_MAX) {
                    out.intBits = 32;
                    out.isUnsigned = false;
                } else if (v <= (uint64_t)UINT32_MAX) {
                    out.intBits = 32;
                    out.isUnsigned = true;
                } else if (v <= (uint64_t)INT64_MAX) {
                    out.intBits = 64;
                    out.isUnsigned = false;
                } else {
                    out.intBits = 64;
                    out.isUnsigned = true;
                }
            }
            return true;
        }
        if (auto ch = dynamic_cast<CharExpr*>(expr)) {
            out.isFloat = false;
            out.isUnsigned = true;  // IR type U8, like visit(CharExpr)
            out.intBits = 8;
            out.u = (uint64_t)(unsigned char)ch->getValue();
            return true;
        }
        if (auto grp = dynamic_cast<GroupingExpr*>(expr)) {
            return tryEvalConstValue(grp->getExpression(), out);
        }
        if (auto un = dynamic_cast<UnaryExpr*>(expr)) {
            ConstEvalValue v;
            if (!tryEvalConstValue(un->getOperand(), v)) return false;
            if (un->getOperator() == "-") {
                out = v;
                if (v.isFloat) {
                    // CID-003 follow-up (M2): negation folds to a constant;
                    // a literal stays literal so a widening context keeps
                    // the parsed double (see visit(UnaryExpr)).
                    out.f = -v.f;
                } else {
                    // Local `-x` lowers to `0 - x` at the operand's IR
                    // type, so the subtraction wraps at that width: u32
                    // negation of 3000000000 is 1294967296, not a 64-bit
                    // value.
                    out.u = canonicalInt(0 - v.u, v.intBits, v.isUnsigned);
                }
                return true;
            }
            if (un->getOperator() == "~" && !v.isFloat) {
                out = v;
                out.u = canonicalInt(~v.u, v.intBits, v.isUnsigned);
                return true;
            }
            if (un->getOperator() == "+") { out = v; return true; }
            return false;
        }
        if (auto cast = dynamic_cast<CastExpr*>(expr)) {
            ConstEvalValue v;
            if (!tryEvalConstValue(cast->getExpression(), v)) return false;
            TypeInfo target = TypeInfo::fromAST(cast->getTargetType());
            if (target.pointerLevel > 0) return false;
            IRType irType = mapType(target.name);
            if (irType == IRType::F32 || irType == IRType::F64) {
                out = v;
                out.isFloat = true;
                if (v.isFloat) {
                    if (irType == IRType::F32) {
                        // Any f32 cast rounds once: F64->F32 folds the
                        // constant, same-width goes through MOV, and a
                        // computed value is already rounded.
                        out.f = roundToF32(v.f);
                        out.isF64 = false;
                        out.literal = false;
                    } else {
                        // Widening to f64 keeps a bare f32 literal's parsed
                        // double (CastExpr folds it to an F64 constant); a
                        // computed f32 register widens from its rounded value.
                        out.isF64 = true;
                        out.literal = v.literal;
                    }
                } else {
                    out.isF64 = (irType == IRType::F64);
                    out.f = out.isF64 ? intConstToDouble(v) : intConstToFloat32(v);
                    out.literal = false;
                }
                return true;
            }
            bool tgtInt = (irType == IRType::I8 || irType == IRType::U8 ||
                           irType == IRType::I16 || irType == IRType::U16 ||
                           irType == IRType::I32 || irType == IRType::U32 ||
                           irType == IRType::I64 || irType == IRType::U64);
            if (!tgtInt) return false;  // bool/pointer casts: not folded
            uint64_t bits = v.isFloat ? (uint64_t)(int64_t)v.f  // CVT_FTOI truncates
                                      : v.u;
            int width = (irType == IRType::I8 || irType == IRType::U8) ? 8 :
                        (irType == IRType::I16 || irType == IRType::U16) ? 16 :
                        (irType == IRType::I32 || irType == IRType::U32) ? 32 : 64;
            bool tgtUnsigned = (irType == IRType::U8 || irType == IRType::U16 ||
                                irType == IRType::U32 || irType == IRType::U64);
            out = ConstEvalValue{};
            out.intBits = width;
            out.isUnsigned = tgtUnsigned;
            out.u = canonicalInt(bits, width, tgtUnsigned);
            return true;
        }
        // CIDER-BUG-048: a `const` reference folds through its initializer
        // with full typed semantics (so an i8/u32 const keeps its width and
        // sign instead of being re-typed as a literal). Enum variants and
        // initializer-less consts carry constValue (int). `constEvalVisiting_`
        // breaks cycles; the checker rejects them first.
        if (auto id = dynamic_cast<IdentifierExpr*>(expr)) {
            return tryEvalConstSymbolValue(resolveConstIdentifier(id->getName()), out);
        }
        // CIDER-BUG-048: module-qualified consts fold like bare ones.
        if (auto mem = dynamic_cast<MemberAccessExpr*>(expr)) {
            if (mem->isArrow()) return false;
            auto base = dynamic_cast<IdentifierExpr*>(mem->getObject());
            if (!base) return false;
            auto baseSym = resolveConstIdentifier(base->getName());
            if (!baseSym || baseSym->kind != SymbolKind::MODULE) return false;
            auto modSym = std::static_pointer_cast<ModuleSymbol>(baseSym);
            auto target = modSym->scope ? modSym->scope->resolveLocal(mem->getMember()) : nullptr;
            return tryEvalConstSymbolValue(target, out);
        }
        if (auto bin = dynamic_cast<BinaryExpr*>(expr)) {
            ConstEvalValue l, r;
            if (!tryEvalConstValue(bin->getLeft(), l)) return false;
            if (!tryEvalConstValue(bin->getRight(), r)) return false;
            const std::string& op = bin->getOperator();
            if (l.isFloat || r.isFloat) {
                if (op != "+" && op != "-" && op != "*" && op != "/") return false;
                bool widthF64 = (l.isFloat && l.isF64) || (r.isFloat && r.isF64);
                // An integer operand converts at the float operand's width
                // (CVT_ITOF); then a mixed f32/f64 pair computes at f64.
                auto asDouble = [&](const ConstEvalValue& v) -> double {
                    if (!v.isFloat) {
                        // CVT_ITOF at the float operand's width, rounded once.
                        return widthF64 ? intConstToDouble(v)
                                        : intConstToFloat32(v);
                    }
                    if (widthF64) return v.f;  // widening keeps a literal's parsed double
                    return v.literal ? roundToF32(v.f) : v.f;
                };
                double x = asDouble(l), y = asDouble(r);
                double z;
                if (op == "+") z = x + y;
                else if (op == "-") z = x - y;
                else if (op == "*") z = x * y;
                else z = x / y;
                out = ConstEvalValue{};
                out.isFloat = true;
                out.isF64 = widthF64;
                out.f = widthF64 ? z : roundToF32(z);
                return true;
            }
            // Integer-only arithmetic, wrapped at the result's IR type
            // exactly like the local path (visit(BinaryExpr) + the VM's
            // narrowInt). +,-,*,/,% widen a mixed pair that involves a
            // 64-bit operand to I64/U64 (mixedIntCommon); every other
            // shape computes at the left operand's type. Unlike
            // tryEvalConstU64 this keeps signed division/mod signed (the
            // runtime path does), so `var g: f64 = -7 / 2` is -3.0 like
            // the local form.
            bool arith = (op == "+" || op == "-" || op == "*" ||
                          op == "/" || op == "%");
            bool widen64 = arith && (l.intBits == 64 || r.intBits == 64);
            int destBits = widen64 ? 64 : l.intBits;
            bool destUnsigned = widen64 ? (l.isUnsigned || r.isUnsigned)
                                        : l.isUnsigned;
            uint64_t lv = l.u, rv = r.u;  // canonical: extended by operand type
            uint64_t raw = 0;
            if (op == "+") raw = lv + rv;
            else if (op == "-") raw = lv - rv;
            else if (op == "*") raw = lv * rv;
            else if (op == "/" || op == "%") {
                if (rv == 0) return false;
                // The VM divides unsigned when the destination or either
                // operand is unsigned (CID-028).
                if (destUnsigned || l.isUnsigned || r.isUnsigned) {
                    raw = (op == "/") ? lv / rv : lv % rv;
                } else {
                    int64_t a = (int64_t)lv, b = (int64_t)rv;
                    if (a == INT64_MIN && b == -1) return false;  // would trap
                    raw = (uint64_t)(op == "/" ? a / b : a % b);
                }
            } else if (op == "<<" || op == ">>") {
                if (rv >= 64) return false;
                if (op == "<<") raw = lv << rv;
                else raw = l.isUnsigned ? (lv >> rv) : (uint64_t)((int64_t)lv >> rv);
            } else if (op == "&") raw = lv & rv;
            else if (op == "|") raw = lv | rv;
            else if (op == "^") raw = lv ^ rv;
            else return false;
            out = ConstEvalValue{};
            out.isFloat = false;
            out.intBits = destBits;
            out.isUnsigned = destUnsigned;
            out.u = canonicalInt(raw, destBits, destUnsigned);
            return true;
        }
        return false;
    }

    // CIDER-BUG-048: fold a `const` symbol reference into a typed value.
    // Recurses into the declaration's initializer in its own scope; enum
    // variants and initializer-less consts carry constValue (int).
    bool tryEvalConstSymbolValue(const std::shared_ptr<Symbol>& sym, ConstEvalValue& out) {
        if (!sym || !sym->isConst || sym->kind != SymbolKind::VARIABLE) return false;
        if (!sym->constInit) {
            out = ConstEvalValue{};
            out.isFloat = false;
            out.intBits = 32;
            out.isUnsigned = false;
            out.u = canonicalInt((uint64_t)(int64_t)sym->constValue, 32, false);
            return true;
        }
        if (!constEvalVisiting_.insert(sym->id).second) return false;
        Scope* prevScope = constEvalScope_;
        if (Scope* declScope = scopeForModule(definingModuleFor(sym, sym->name))) {
            constEvalScope_ = declScope;
        }
        bool ok = tryEvalConstValue(sym->constInit, out);
        constEvalScope_ = prevScope;
        constEvalVisiting_.erase(sym->id);
        return ok;
    }

    // Global float initializer entry point: the expression's own value,
    // unconverted. The caller rounds to the declared width (f64 keeps a
    // bare f32 literal's parsed double, matching convertFloatWidth).
    bool tryEvalConstF64(Expression* expr, double& out) {
        ConstEvalValue v;
        if (!tryEvalConstValue(expr, v)) return false;
        out = v.isFloat ? v.f : intConstToDouble(v);
        return true;
    }

    // Float-to-integer cast for any integer target (CID-025): convert
    // with full 64-bit precision, then narrow to the target width, so
    // every cast lowers to cvttss2si/cvttsd2si. (Out-of-int64-range
    // values are C-UB, same as the signed-32 path before.)
    IRValue convertFloatToInt(IRValue val, const TypeInfo& targetType) {
        IRType tgtIR = (targetType.pointerLevel > 0) ? IRType::PTR : mapType(targetType.name);
        IRValue wide = IRValue::Register(nextTemp(), IRType::I64);
        currentBlock_->addInstruction({IROp::CVT_FTOI, wide, {val}});
        if (tgtIR == IRType::I64) return wide;
        IRValue typed = IRValue::Register(nextTemp(), tgtIR);
        currentBlock_->addInstruction({IROp::MOV, typed, {wide}});
        return truncateIntToType(typed, targetType);
    }

    IRValue truncateIntToType(IRValue val, const TypeInfo& target) {
        IRType tgtIR = (target.pointerLevel > 0) ? IRType::PTR : mapType(target.name);
        bool tgtNarrow = (tgtIR == IRType::I8 || tgtIR == IRType::U8 ||
                          tgtIR == IRType::I16 || tgtIR == IRType::U16);
        bool tgtWide32 = (tgtIR == IRType::I32 || tgtIR == IRType::U32 || tgtIR == IRType::BOOL);
        bool tgtWide64 = (tgtIR == IRType::I64 || tgtIR == IRType::U64 || tgtIR == IRType::PTR);
        bool valIsInt = (val.type == IRType::I8 || val.type == IRType::U8 || val.type == IRType::BOOL ||
                         val.type == IRType::I16 || val.type == IRType::U16 ||
                         val.type == IRType::I32 || val.type == IRType::U32 ||
                         val.type == IRType::I64 || val.type == IRType::U64 || val.type == IRType::PTR);
        if (!valIsInt) return val;
        if (val.type == tgtIR) return val;
        if (tgtNarrow) {
            if (val.kind == IRValue::CONSTANT) {
                uint64_t mask = (tgtIR == IRType::I8 || tgtIR == IRType::U8) ? 0xFFULL : 0xFFFFULL;
                uint64_t v = (uint64_t)val.intValue & mask;
                // CID-003 fix round 2: signed narrow targets keep the sign,
                // matching what native sign-extension produces for the same
                // byte (no unsigned masking of negative i8/i16 constants).
                if (tgtIR == IRType::I8) return IRValue::Constant((int64_t)(int8_t)v, tgtIR);
                if (tgtIR == IRType::I16) return IRValue::Constant((int64_t)(int16_t)v, tgtIR);
                return IRValue::Constant(v, tgtIR);
            }
            IRValue dest = IRValue::Register(nextTemp(), tgtIR);
            uint64_t mask = (tgtIR == IRType::I8 || tgtIR == IRType::U8) ? 0xFFULL : 0xFFFFULL;
            currentBlock_->addInstruction({IROp::BIT_AND, dest, {val, IRValue::Constant((int64_t)mask, IRType::I32)}});
            return dest;
        }
        auto width = [](IRType x) -> int {
            if (x == IRType::I8 || x == IRType::U8 || x == IRType::BOOL) return 1;
            if (x == IRType::I16 || x == IRType::U16) return 2;
            if (x == IRType::I32 || x == IRType::U32) return 4;
            return 8;
        };
        if ((tgtWide32 || tgtWide64) && width(val.type) < width(tgtIR)) {
            if (val.kind == IRValue::CONSTANT) {
                return IRValue::Constant((uint64_t)val.intValue, tgtIR);
            }
            IRValue dest = IRValue::Register(nextTemp(), tgtIR);
            currentBlock_->addInstruction({IROp::MOV, dest, {val}});
            return dest;
        }
        return val;
    }

    IRValue emitBitfieldLoad(IRValue addr, int bitOffset, int bitWidth, bool isSigned) {
        IRValue word = IRValue::Register(nextTemp(), IRType::I32);
        currentBlock_->addInstruction({IROp::LOAD, word, {addr, IRValue::Constant(0, IRType::I32)}});
        IRValue shifted = IRValue::Register(nextTemp(), IRType::I32);
        currentBlock_->addInstruction({IROp::RSHIFT, shifted, {word, IRValue::Constant(bitOffset, IRType::I32)}});
        IRValue masked = IRValue::Register(nextTemp(), IRType::I32);
        currentBlock_->addInstruction({IROp::BIT_AND, masked, {shifted, IRValue::Constant(bitfieldMask(bitWidth), IRType::I32)}});
        if (isSigned && bitWidth < 32) {
            // Round-trip the field bits through the top of the word. These
            // temps are U32 (not I32) deliberately: the shifts are bit
            // manipulation, and a signed 32-bit RSHIFT (sar) would sign-fill
            // and corrupt values like 5-in-3-bits that must read back
            // unchanged. Genuine signed arithmetic keeps I32 and sar.
            IRValue sign = IRValue::Register(nextTemp(), IRType::U32);
            currentBlock_->addInstruction({IROp::LSHIFT, sign, {masked, IRValue::Constant(32 - bitWidth, IRType::I32)}});
            IRValue value = IRValue::Register(nextTemp(), IRType::U32);
            currentBlock_->addInstruction({IROp::RSHIFT, value, {sign, IRValue::Constant(32 - bitWidth, IRType::I32)}});
            return value;
        }
        return masked;
    }

    void emitBitfieldStore(IRValue addr, IRValue val, int bitOffset, int bitWidth) {
        IRValue word = IRValue::Register(nextTemp(), IRType::I32);
        currentBlock_->addInstruction({IROp::LOAD, word, {addr, IRValue::Constant(0, IRType::I32)}});
        IRValue vm = IRValue::Register(nextTemp(), IRType::I32);
        currentBlock_->addInstruction({IROp::BIT_AND, vm, {val, IRValue::Constant(bitfieldMask(bitWidth), IRType::I32)}});
        IRValue shifted = IRValue::Register(nextTemp(), IRType::I32);
        currentBlock_->addInstruction({IROp::LSHIFT, shifted, {vm, IRValue::Constant(bitOffset, IRType::I32)}});
        IRValue cleared = IRValue::Register(nextTemp(), IRType::I32);
        int64_t clearMask = ~(bitfieldMask(bitWidth) << bitOffset);
        currentBlock_->addInstruction({IROp::BIT_AND, cleared, {word, IRValue::Constant(clearMask, IRType::I32)}});
        IRValue nw = IRValue::Register(nextTemp(), IRType::I32);
        currentBlock_->addInstruction({IROp::BIT_OR, nw, {cleared, shifted}});
        currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, nw, IRValue::Constant(0, IRType::I32)}});
    }

    void visit(AssignStmt& stmt) override {
        std::string compoundOp = stmt.getOp();

        IRValue addr = IRValue::Constant(0, IRType::PTR);
        // CID-003: for a `*p = v` target the pointee's declared type drives
        // the stored width (see the plain-assignment path below).
        TypeInfo derefPointeeType;
        bool hasDerefTarget = false;
        if (auto ident = dynamic_cast<IdentifierExpr*>(stmt.getTarget())) {
            // CID-020/021 (spec §5): global stores key by defining module.
            auto sym = symbolTable_.resolve(ident->getName());
            addr = variableValues_.count(ident->getName()) ? variableValues_[ident->getName()]
                                                           : globalValues_[globalRefName(sym, ident->getName())];
            // Param read-through must stop once the param is assigned:
            // subsequent reads go through memory (the alloca holds the new value).
            paramNames_.erase(ident->getName());
        } else if (auto mem = dynamic_cast<MemberAccessExpr*>(stmt.getTarget())) {
            addr = getAddress(mem);
        } else if (auto idx = dynamic_cast<IndexExpr*>(stmt.getTarget())) {
            addr = getAddress(idx);
        } else if (auto unary = dynamic_cast<UnaryExpr*>(stmt.getTarget())) {
            if (unary->getOperator() == "*") {
                unary->getOperand()->accept(*this);
                addr = lastValue_;
                // CID-003: remember the pointee so the store uses the
                // declared width (`*p = 2.5` through an f64* must write 8
                // bytes, not 4).
                derefPointeeType = lastExprType_;
                if (derefPointeeType.pointerLevel > 0) derefPointeeType.pointerLevel--;
                hasDerefTarget = true;
            }
        } else {
            addr = getAddress(stmt.getTarget());
        }

        if (compoundOp.empty()) {
            stmt.getValue()->accept(*this);
            IRValue val = lastValue_;

            // Bitfield targets use the read-modify-write sequence.
            if (auto mem = dynamic_cast<MemberAccessExpr*>(stmt.getTarget())) {
                int bitOffset = 0, bitWidth = 0;
                if (getMemberBitfield(mem, bitOffset, bitWidth)) {
                    emitBitfieldStore(addr, val, bitOffset, bitWidth);
                    return;
                }
                TypeInfo fieldType;
                if (getMemberFieldType(mem, fieldType)) {
                    // CID-003: the field's declared type drives the stored
                    // representation (int narrowing, int<->float, widths).
                    val = convertValueToType(val, fieldType);
                    lastExprType_ = fieldType;
                }
            } else if (auto idx = dynamic_cast<IndexExpr*>(stmt.getTarget())) {
                TypeInfo baseType = inferExprType(idx->getObject());
                TypeInfo elemType = baseType;
                if (elemType.isArray) {
                    elemType.isArray = false;
                    elemType.arraySize = 0;
                } else if (elemType.pointerLevel > 0) {
                    elemType.pointerLevel--;
                } else if (elemType.name == "string") {
                    elemType.name = "char";
                }
                val = convertValueToType(val, elemType);
                lastExprType_ = elemType;
            } else if (auto ident = dynamic_cast<IdentifierExpr*>(stmt.getTarget())) {
                // Plain variable stores use the declared width (same rule as
                // fields and elements above): a narrow value in a wide local
                // slot leaves stale upper bytes for later wide loads.
                auto sym = symbolTable_.resolve(ident->getName());
                if (sym && sym->type.pointerLevel == 0 && !sym->type.isArray &&
                    !sym->type.isUserDefined && !isStructValueType(sym->type) &&
                    !sym->type.isInterface && !sym->type.isSlice) {
                    IRType declIR = mapType(sym->type.name);
                    bool declIsInt = (declIR == IRType::I8 || declIR == IRType::U8 ||
                                      declIR == IRType::I16 || declIR == IRType::U16 ||
                                      declIR == IRType::I32 || declIR == IRType::U32 ||
                                      declIR == IRType::BOOL ||
                                      declIR == IRType::I64 || declIR == IRType::U64);
                    if (declIsInt || declIR == IRType::F32 || declIR == IRType::F64) {
                        // CID-003: the declared type drives the stored
                        // representation: integer narrowing, int<->float
                        // (CVT_ITOF/CVT_FTOI) and float widths (`e = 2.5`
                        // into an f64 target folds to the exact F64
                        // constant; `e = 2` converts the int).
                        val = convertValueToType(val, sym->type);
                    }
                }
            } else if (hasDerefTarget) {
                // CID-003: `*p = v` stores at the pointee's declared width
                // (`*p = 2.5` through an f64* must write 8 bytes, an
                // integer pointee narrows, an int value converts).
                val = convertValueToType(val, derefPointeeType);
                bool pointeeScalar = (derefPointeeType.pointerLevel == 0 &&
                                      !derefPointeeType.isArray && !derefPointeeType.isUserDefined &&
                                      !isStructValueType(derefPointeeType) &&
                                      !derefPointeeType.isInterface && !derefPointeeType.isSlice);
                if (pointeeScalar) lastExprType_ = derefPointeeType;
            }

            if (lastExprType_.pointerLevel == 0 && (lastExprType_.isArray || lastExprType_.isUserDefined || getTypeSize(lastExprType_) > 8)) {
                currentBlock_->addInstruction({IROp::MEMCPY, std::nullopt, {addr, val, IRValue::Constant(getTypeSize(lastExprType_), IRType::I32)}});
            } else {
                if (addr.kind == IRValue::LABEL) {
                    currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, val}});
                } else {
                    currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, val, IRValue::Constant(0, IRType::I32)}});
                }
            }
        } else {
            bool isBitfield = false;
            int bitOffset = 0, bitWidth = 0;
            if (auto mem = dynamic_cast<MemberAccessExpr*>(stmt.getTarget())) {
                isBitfield = getMemberBitfield(mem, bitOffset, bitWidth);
            }
            IRValue current;
            if (isBitfield) {
                current = emitBitfieldLoad(addr, bitOffset, bitWidth, true);
            } else {
                // Compound assignment computes at the target's width
                // (CID-028): hardcoding I32 truncated u64 loop counters
                // (temp /= 10) to 32 bits.
                IRType targetIR = IRType::I32;
                if (auto ident = dynamic_cast<IdentifierExpr*>(stmt.getTarget())) {
                    if (auto sym = symbolTable_.resolve(ident->getName())) {
                        targetIR = (sym->type.pointerLevel > 0) ? IRType::PTR : mapType(sym->type.name);
                    }
                } else if (auto mem = dynamic_cast<MemberAccessExpr*>(stmt.getTarget())) {
                    TypeInfo fieldType;
                    if (!getMemberBitfield(mem, bitOffset, bitWidth) && getMemberFieldType(mem, fieldType)) {
                        targetIR = (fieldType.pointerLevel > 0) ? IRType::PTR : mapType(fieldType.name);
                    }
                } else if (auto idx = dynamic_cast<IndexExpr*>(stmt.getTarget())) {
                    TypeInfo baseType = inferExprType(idx->getObject());
                    TypeInfo elemType = baseType;
                    if (elemType.isArray) {
                        elemType.isArray = false;
                        elemType.arraySize = 0;
                    } else if (elemType.pointerLevel > 0) {
                        elemType.pointerLevel--;
                    } else if (elemType.name == "string") {
                        elemType.name = "char";
                    }
                    targetIR = (elemType.pointerLevel > 0) ? IRType::PTR : mapType(elemType.name);
                } else if (hasDerefTarget) {
                    // CID-003: `*p += v` loads/stores at the pointee width.
                    targetIR = (derefPointeeType.pointerLevel > 0) ? IRType::PTR : mapType(derefPointeeType.name);
                }
                current = IRValue::Register(nextTemp(), targetIR);
                if (addr.kind == IRValue::LABEL) {
                    currentBlock_->addInstruction({IROp::LOAD, current, {addr}});
                } else {
                    currentBlock_->addInstruction({IROp::LOAD, current, {addr, IRValue::Constant(0, IRType::I32)}});
                }
            }

            stmt.getValue()->accept(*this);
            IRValue rhs = lastValue_;

            // CID-003: mixed float widths in compound assignments use the
            // same usual conversions as binary expressions — compute at f64
            // so the arithmetic sees same-width operands — then narrow back
            // to the target's width before the store. Without this the ADD
            // ran at F64 with an F32 operand and codegen read 8 bytes from
            // a 4-byte F32 pool constant.
            IRType targetFloatWidth = current.type;
            bool widenedFloat = false;
            // CID-003 follow-up (I3): an integer RHS converts to the target
            // float width first — `d += 1` used to emit `addsd xmm0, 1`,
            // which does not assemble. Runs before the mixed-width
            // promotion so f64/f32 pairs keep their usual conversions.
            if ((current.type == IRType::F32 || current.type == IRType::F64) &&
                isIntIRType(rhs.type)) {
                rhs = convertIntToFloat(rhs, current.type);
            }
            if ((current.type == IRType::F32 && rhs.type == IRType::F64) ||
                (current.type == IRType::F64 && rhs.type == IRType::F32)) {
                IRValue& narrow = (current.type == IRType::F32) ? current : rhs;
                narrow = convertFloatWidth(narrow, IRType::F64);
                widenedFloat = true;
            }

            IROp irOp;
            if (compoundOp == "+") irOp = IROp::ADD;
            else if (compoundOp == "-") irOp = IROp::SUB;
            else if (compoundOp == "*") irOp = IROp::MUL;
            else if (compoundOp == "/") irOp = IROp::DIV;
            else if (compoundOp == "%") irOp = IROp::MOD;
            else if (compoundOp == "&") irOp = IROp::BIT_AND;
            else if (compoundOp == "|") irOp = IROp::BIT_OR;
            else if (compoundOp == "^") irOp = IROp::BIT_XOR;
            else return;

            IRValue result = IRValue::Register(nextTemp(), widenedFloat ? IRType::F64 : current.type);
            if (!widenedFloat && (irOp == IROp::ADD || irOp == IROp::SUB || irOp == IROp::MUL ||
                irOp == IROp::DIV || irOp == IROp::MOD)) {
                // Same usual-arithmetic-conversions rule as binary
                // expressions: compute mixed-width updates at 64 bits;
                // the STORE below narrows back to the LHS width.
                IRType common = mixedIntCommon(current.type, rhs.type);
                if (common != IRType::VOID) {
                    convertIntToCommon(current, common);
                    convertIntToCommon(rhs, common);
                    result.type = common;
                }
            }
            currentBlock_->addInstruction({irOp, result, {current, rhs}});

            if (isBitfield) {
                emitBitfieldStore(addr, result, bitOffset, bitWidth);
                return;
            }
            if (widenedFloat && targetFloatWidth == IRType::F32) {
                // The f64 computation rounds once on the way back into the
                // f32 target slot.
                result = convertFloatWidth(result, IRType::F32);
            }
            if (addr.kind == IRValue::LABEL) {
                currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, result}});
            } else {
                currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, result, IRValue::Constant(0, IRType::I32)}});
            }
        }
    }

    void visit(IfStmt& stmt) override {
        stmt.getCondition()->accept(*this);
        IRValue cond = lastValue_;

        std::string thenName = nextLabel("then");
        std::string elseName = nextLabel("else");
        std::string mergeName = nextLabel("ifcont");

        currentBlock_->addInstruction({IROp::COND_BR, std::nullopt, {cond, IRValue::Label(thenName), IRValue::Label(elseName)}});

        startBlock(thenName);
        symbolTable_.enterScope();
        deferStack_.push_back({});
        destructorStack_.push_back({});
        for (auto& s : stmt.getThenBranch()) s->accept(*this);
        if (currentBlock_ && (currentBlock_->getInstructions().empty() || (currentBlock_->getInstructions().back().op != IROp::BR && currentBlock_->getInstructions().back().op != IROp::RET))) {
            // CIDER-BUG-050: an if branch is a lexical block; its defers run
            // when the branch exits, not at the enclosing block's exit.
            emitDefers(deferStack_.size() - 1);
            emitDestructors(destructorStack_.size() - 1);
            currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(mergeName)}});
        }
        destructorStack_.pop_back();
        deferStack_.pop_back();
        symbolTable_.exitScope();

        startBlock(elseName);
        symbolTable_.enterScope();
        deferStack_.push_back({});
        destructorStack_.push_back({});
        for (auto& s : stmt.getElseBranch()) s->accept(*this);
        if (currentBlock_ && (currentBlock_->getInstructions().empty() || (currentBlock_->getInstructions().back().op != IROp::BR && currentBlock_->getInstructions().back().op != IROp::RET))) {
            emitDefers(deferStack_.size() - 1);
            emitDestructors(destructorStack_.size() - 1);
            currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(mergeName)}});
        }
        destructorStack_.pop_back();
        deferStack_.pop_back();
        symbolTable_.exitScope();

        startBlock(mergeName);
    }

    void visit(WhileStmt& stmt) override {
        std::string headerName = nextLabel("while_header");
        std::string bodyName = nextLabel("while_body");
        std::string exitName = nextLabel("while_exit");

        currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(headerName)}});

        startBlock(headerName);
        stmt.getCondition()->accept(*this);
        IRValue cond = lastValue_;
        currentBlock_->addInstruction({IROp::COND_BR, std::nullopt, {cond, IRValue::Label(bodyName), IRValue::Label(exitName)}});

        continueLabels_.push_back(headerName);
        breakLabels_.push_back(exitName);
        // Index the loop's iteration scope (pushed below, per iteration):
        // break/continue unwind down to it, not past it.
        unwindDepths_.push_back(deferStack_.size());
        symbolTable_.enterScope();

        startBlock(bodyName);
        // CIDER-BUG-050: a fresh scope per iteration, so body defers run at
        // the end of every iteration (and before a break/continue jump).
        deferStack_.push_back({});
        destructorStack_.push_back({});
        for (auto& s : stmt.getBody()) s->accept(*this);
        if (currentBlock_ && (currentBlock_->getInstructions().empty() || (currentBlock_->getInstructions().back().op != IROp::BR && currentBlock_->getInstructions().back().op != IROp::RET))) {
            emitDefers(deferStack_.size() - 1);
            emitDestructors(destructorStack_.size() - 1);
            currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(headerName)}});
        }
        destructorStack_.pop_back();
        deferStack_.pop_back();
        symbolTable_.exitScope();
        unwindDepths_.pop_back();
        continueLabels_.pop_back();
        breakLabels_.pop_back();

        startBlock(exitName);
    }

    void visit(ForStmt& stmt) override {
        symbolTable_.enterScope();
        if (stmt.getInitializer()) stmt.getInitializer()->accept(*this);

        std::string headerName = nextLabel("for_header");
        std::string bodyName = nextLabel("for_body");
        std::string incName = nextLabel("for_inc");
        std::string exitName = nextLabel("for_exit");

        currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(headerName)}});

        startBlock(headerName);
        if (stmt.getCondition()) {
            stmt.getCondition()->accept(*this);
            currentBlock_->addInstruction({IROp::COND_BR, std::nullopt, {lastValue_, IRValue::Label(bodyName), IRValue::Label(exitName)}});
        } else {
            currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(bodyName)}});
        }

        continueLabels_.push_back(incName);
        breakLabels_.push_back(exitName);
        // Index the loop's iteration scope (pushed below): break/continue
        // unwind down to it, not past it.
        unwindDepths_.push_back(deferStack_.size());

        startBlock(bodyName);
        // CIDER-BUG-050: a fresh scope per iteration, so body defers run at
        // the end of every iteration (and before a break/continue jump).
        deferStack_.push_back({});
        destructorStack_.push_back({});
        for (auto& s : stmt.getBody()) s->accept(*this);
        if (currentBlock_ && (currentBlock_->getInstructions().empty() || (currentBlock_->getInstructions().back().op != IROp::BR && currentBlock_->getInstructions().back().op != IROp::RET))) {
            emitDefers(deferStack_.size() - 1);
            emitDestructors(destructorStack_.size() - 1);
            currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(incName)}});
        }

        startBlock(incName);
        if (stmt.getIncrement()) stmt.getIncrement()->accept(*this);
        currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(headerName)}});

        destructorStack_.pop_back();
        deferStack_.pop_back();
        unwindDepths_.pop_back();
        continueLabels_.pop_back();
        breakLabels_.pop_back();
        symbolTable_.exitScope();

        startBlock(exitName);
    }

    void visit(AsmStmt& stmt) override {
        if (currentBlock_) {
            IRInstruction instr;
            instr.op = IROp::ASM;
            instr.args.push_back(IRValue::Label(stmt.getCode()));

            std::string code = stmt.getCode();
            std::set<std::string> mentionedVars;
            size_t pos = 0;
            while ((pos = code.find("{", pos)) != std::string::npos) {
                size_t end = code.find("}", pos);
                if (end != std::string::npos) {
                    std::string var = code.substr(pos + 1, end - pos - 1);
                    if (!var.empty() && var[0] == '*') var = var.substr(1);
                    // Width suffix for codegen (Issue #24): {fd:q} still
                    // refers to variable fd; the suffix only re-sizes the
                    // substituted register.
                    size_t colon = var.find(':');
                    if (colon != std::string::npos) var = var.substr(0, colon);
                    if (!var.empty()) mentionedVars.insert(var);
                    pos = end + 1;
                } else {
                    pos++;
                }
            }

            bool prevIsAddress = isAddress_;
            isAddress_ = false;
            for (const auto& name : mentionedVars) {
                auto sym = symbolTable_.resolve(name);
                if (sym) {
                    IdentifierExpr ident(name, 0, 0);
                    ident.accept(*this);
                    instr.asmMap[name] = lastValue_;
                }
            }
            isAddress_ = prevIsAddress;

            currentBlock_->addInstruction(instr);

            for (const auto& name : mentionedVars) {
                bool found = false;
                IRValue varAddr;
                if (variableValues_.count(name)) {
                    varAddr = variableValues_[name];
                    found = true;
                } else if (globalValues_.count(name)) {
                    varAddr = globalValues_[name];
                    found = true;
                }

                if (found) {
                    IRValue val = instr.asmMap[name];
                    if (varAddr.kind == IRValue::LABEL) {
                        currentBlock_->addInstruction({IROp::STORE, std::nullopt, {varAddr, val}});
                    } else {
                        currentBlock_->addInstruction({IROp::STORE, std::nullopt, {varAddr, val, IRValue::Constant(0, IRType::I32)}});
                    }
                }
            }
        } else {
            module_.addGlobalAsm(stmt.getCode());
        }
    }

    void visit(BreakStmt&) override {
        if (!breakLabels_.empty()) {
            // CIDER-BUG-050: run the defers of the exited blocks (the loop's
            // iteration scope and anything nested inside it) before jumping.
            // Enclosing scopes keep their defers for their own exit, so they
            // are not run twice.
            emitDefers(unwindDepths_.back());
            emitDestructors(unwindDepths_.back());
            // CIDER-BUG-053: same partial unwind for active try handlers.
            emitExceptionUnwind(unwindDepths_.back());
            currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(breakLabels_.back())}});
            startBlock(nextLabel("dead"));
        }
    }
    void visit(ContinueStmt&) override {
        if (!continueLabels_.empty()) {
            // Same partial unwind as break (CIDER-BUG-050).
            emitDefers(unwindDepths_.back());
            emitDestructors(unwindDepths_.back());
            emitExceptionUnwind(unwindDepths_.back());
            currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(continueLabels_.back())}});
            startBlock(nextLabel("dead"));
        }
    }
    void visit(DeferStmt& stmt) override {
        deferStack_.back().push_back(stmt.getDeferredStmt());
    }

    void visit(ForEachStmt& stmt) override {
        if (!currentFunction_) return;

        bool wasAddress = isAddress_;
        isAddress_ = false;
        stmt.getIterable()->accept(*this);
        IRValue iterable = lastValue_;
        TypeInfo iterableType = lastExprType_;
        isAddress_ = wasAddress;

        TypeInfo elemType = iterableType;
        if (elemType.isArray) {
            elemType.isArray = false;
            elemType.arraySize = 0;
        }
        if (elemType.isSlice) {
            elemType.isSlice = false;
        }

        if (!iterableType.isArray && !iterableType.isSlice) {
            // Custom iterator protocol: the element type comes from next()'s
            // return pointee, not the iterable's own type.
            auto typeSym0 = resolveNominalType(iterableType.name);
            if (typeSym0 && (typeSym0->kind == SymbolKind::STRUCT || typeSym0->kind == SymbolKind::CLASS)) {
                auto ss0 = std::static_pointer_cast<StructSymbol>(typeSym0);
                for (const auto& m : ss0->methods) {
                    if (m.name == "next" && m.returnType.pointerLevel >= 1) {
                        elemType = m.returnType;
                        elemType.pointerLevel--;
                        if (elemType.pointerLevel == 0 && !elemType.isArray) {
                            auto ts2 = resolveNominalType(elemType.name);
                            if (!ts2 || (ts2->kind != SymbolKind::STRUCT && ts2->kind != SymbolKind::CLASS)) {
                                elemType.isUserDefined = false;
                            }
                        }
                        break;
                    }
                }
            }
        }

        symbolTable_.enterScope();
        {
            auto loopVarSym = std::make_shared<Symbol>();
            loopVarSym->kind = SymbolKind::VARIABLE;
            loopVarSym->name = stmt.getVarName();
            loopVarSym->type = elemType;
            symbolTable_.define(loopVarSym);
        }
        if (!iterableType.isArray && !iterableType.isSlice) {
            // Custom iterator protocol: it = obj.iter();
            // loop: p = it.next(); if (p == nilPtr) break; x = *p; body.
            std::string typeName = iterableType.name;
            std::string iterFn = sanitizeAsmName(typeName + "_iter");
            std::string nextFn = sanitizeAsmName(typeName + "_next");
            std::string headerName = nextLabel("citer_header");
            std::string bodyName = nextLabel("citer_body");
            std::string exitName = nextLabel("citer_exit");

            IRValue itAddr = IRValue::Register(nextTemp(), IRType::PTR);
            // `iter()` returns the iterator struct by value (sret). Allocate
            // a full struct slot (not a hardcoded 8-byte pointer slot) and
            // pass it as the hidden first argument.
            bool iterIsSret = false;
            int iterStructSize = 8;
            {
                auto iterSym = symbolTable_.resolve(iterFn);
                if (iterSym && iterSym->kind == SymbolKind::FUNCTION) {
                    auto fnSym = std::static_pointer_cast<FunctionSymbol>(iterSym);
                    if (isStructValueType(fnSym->returnType)) {
                        iterIsSret = true;
                        iterStructSize = getTypeSize(fnSym->returnType);
                    }
                } else {
                    // Unresolved (e.g. template instantiation ordering):
                    // fall back to struct-size heuristic via the iterable type.
                    // `iter()` by protocol returns a struct; use its size.
                    iterStructSize = getTypeSize(iterableType);
                    if (iterStructSize > 0) iterIsSret = true;
                }
            }
            IRValue itBuf;
            if (iterIsSret) {
                itBuf = IRValue::Register(nextTemp(), IRType::PTR);
                currentBlock_->addInstruction({IROp::ALLOCA, itBuf, {IRValue::Constant(iterStructSize, IRType::I32)}});
                currentBlock_->addInstruction({IROp::CALL, std::nullopt, {IRValue::Label(iterFn), itBuf, iterable}});
                itAddr = itBuf;
            } else {
                currentBlock_->addInstruction({IROp::ALLOCA, itAddr, {IRValue::Constant(8, IRType::I32)}});
                IRValue itReg = IRValue::Register(nextTemp(), IRType::PTR);
                currentBlock_->addInstruction({IROp::CALL, itReg, {IRValue::Label(iterFn), iterable}});
                currentBlock_->addInstruction({IROp::STORE, std::nullopt, {itAddr, itReg, IRValue::Constant(0, IRType::I32)}});
            }
            currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(headerName)}});

            startBlock(headerName);
            IRValue itv = IRValue::Register(nextTemp(), IRType::PTR);
            if (iterIsSret) {
                itv = itAddr;
            } else {
                currentBlock_->addInstruction({IROp::LOAD, itv, {itAddr, IRValue::Constant(0, IRType::I32)}});
            }
            IRValue p = IRValue::Register(nextTemp(), IRType::PTR);
            currentBlock_->addInstruction({IROp::CALL, p, {IRValue::Label(nextFn), itv}});
            IRValue isNil = IRValue::Register(nextTemp(), IRType::BOOL);
            currentBlock_->addInstruction({IROp::CMP_EQ, isNil, {p, IRValue::Constant(0, IRType::I32)}});
            currentBlock_->addInstruction({IROp::COND_BR, std::nullopt, {isNil, IRValue::Label(exitName), IRValue::Label(bodyName)}});

            continueLabels_.push_back(headerName);
            breakLabels_.push_back(exitName);
            unwindDepths_.push_back(deferStack_.size());

            startBlock(bodyName);
            // CIDER-BUG-050: fresh per-iteration scope for body defers.
            deferStack_.push_back({});
            destructorStack_.push_back({});
            IRValue varAddr = IRValue::Register(nextTemp(), IRType::PTR);
            currentBlock_->addInstruction({IROp::ALLOCA, varAddr, {IRValue::Constant(getTypeSize(elemType), IRType::I32)}});
            variableValues_[stmt.getVarName()] = varAddr;
            // Struct copies only for by-value aggregates (Issue #13): pointers
            // must go through LOAD+STORE, never MEMCPY.
            if (elemType.pointerLevel == 0 &&
                (elemType.isArray || isStructValueType(elemType) || getTypeSize(elemType) > 8)) {
                currentBlock_->addInstruction({IROp::MEMCPY, std::nullopt, {varAddr, p, IRValue::Constant(getTypeSize(elemType), IRType::I32)}});
            } else {
                IRType irType = (elemType.pointerLevel > 0) ? IRType::PTR : mapType(elemType.name);
                IRValue elemVal = IRValue::Register(nextTemp(), irType);
                currentBlock_->addInstruction({IROp::LOAD, elemVal, {p, IRValue::Constant(0, IRType::I32)}});
                currentBlock_->addInstruction({IROp::STORE, std::nullopt, {varAddr, elemVal, IRValue::Constant(0, IRType::I32)}});
            }

            for (auto& s : stmt.getBody()) {
                s->accept(*this);
            }
            if (currentBlock_ && (currentBlock_->getInstructions().empty() ||
                                  (currentBlock_->getInstructions().back().op != IROp::BR &&
                                   currentBlock_->getInstructions().back().op != IROp::RET))) {
                emitDefers(deferStack_.size() - 1);
                emitDestructors(destructorStack_.size() - 1);
                currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(headerName)}});
            }
            destructorStack_.pop_back();
            deferStack_.pop_back();

            startBlock(exitName);
            unwindDepths_.pop_back();
            continueLabels_.pop_back();
            breakLabels_.pop_back();
            symbolTable_.exitScope();
            return;
        }

        std::string headerName = nextLabel("foreach_header");
        std::string bodyName = nextLabel("foreach_body");
        std::string incName = nextLabel("foreach_inc");
        std::string exitName = nextLabel("foreach_exit");

        IRValue iAddr = IRValue::Register(nextTemp(), IRType::PTR);
        currentBlock_->addInstruction({IROp::ALLOCA, iAddr, {IRValue::Constant(4, IRType::I32)}});
        currentBlock_->addInstruction({IROp::STORE, std::nullopt, {iAddr, IRValue::Constant(0, IRType::I32), IRValue::Constant(0, IRType::I32)}});

        currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(headerName)}});

        startBlock(headerName);
        IRValue iVal = IRValue::Register(nextTemp(), IRType::I32);
        currentBlock_->addInstruction({IROp::LOAD, iVal, {iAddr, IRValue::Constant(0, IRType::I32)}});

        IRValue limit;
        if (iterableType.isArray) {
            limit = IRValue::Constant(iterableType.arraySize, IRType::I32);
        } else {
            limit = IRValue::Register(nextTemp(), IRType::I32);
            currentBlock_->addInstruction({IROp::LOAD, limit, {iterable, IRValue::Constant(8, IRType::I32)}});
        }

        IRValue cond = IRValue::Register(nextTemp(), IRType::BOOL);
        currentBlock_->addInstruction({IROp::CMP_LT, cond, {iVal, limit}});
        currentBlock_->addInstruction({IROp::COND_BR, std::nullopt, {cond, IRValue::Label(bodyName), IRValue::Label(exitName)}});

        continueLabels_.push_back(incName);
        breakLabels_.push_back(exitName);
        unwindDepths_.push_back(deferStack_.size());

        startBlock(bodyName);
        // CIDER-BUG-050: fresh per-iteration scope for body defers.
        deferStack_.push_back({});
        destructorStack_.push_back({});
        iVal = IRValue::Register(nextTemp(), IRType::I32);
        currentBlock_->addInstruction({IROp::LOAD, iVal, {iAddr, IRValue::Constant(0, IRType::I32)}});

        int elemSize = getTypeSize(elemType);
        IRValue scaledIdx = IRValue::Register(nextTemp(), IRType::I32);
        currentBlock_->addInstruction({IROp::MUL, scaledIdx, {iVal, IRValue::Constant(elemSize, IRType::I32)}});

        IRValue basePtr;
        if (iterableType.isArray) {
            basePtr = iterable;
        } else {
            basePtr = IRValue::Register(nextTemp(), IRType::PTR);
            currentBlock_->addInstruction({IROp::LOAD, basePtr, {iterable, IRValue::Constant(0, IRType::I32)}});
        }

        IRValue elemAddr = IRValue::Register(nextTemp(), IRType::PTR);
        currentBlock_->addInstruction({IROp::ADD, elemAddr, {basePtr, scaledIdx}});

        IRValue varAddr = IRValue::Register(nextTemp(), IRType::PTR);
        currentBlock_->addInstruction({IROp::ALLOCA, varAddr, {IRValue::Constant(getTypeSize(elemType), IRType::I32)}});
        variableValues_[stmt.getVarName()] = varAddr;

        // Same aggregate-only rule as the iterator path (Issue #13).
        if (elemType.pointerLevel == 0 &&
            (elemType.isArray || isStructValueType(elemType) || getTypeSize(elemType) > 8)) {
            currentBlock_->addInstruction({IROp::MEMCPY, std::nullopt, {varAddr, elemAddr, IRValue::Constant(getTypeSize(elemType), IRType::I32)}});
        } else {
            IRType irType = (elemType.pointerLevel > 0) ? IRType::PTR : mapType(elemType.name);
            IRValue elemVal = IRValue::Register(nextTemp(), irType);
            currentBlock_->addInstruction({IROp::LOAD, elemVal, {elemAddr, IRValue::Constant(0, IRType::I32)}});
            currentBlock_->addInstruction({IROp::STORE, std::nullopt, {varAddr, elemVal, IRValue::Constant(0, IRType::I32)}});
        }

        for (auto& s : stmt.getBody()) {
            s->accept(*this);
        }

        if (currentBlock_ && (currentBlock_->getInstructions().empty() ||
                              (currentBlock_->getInstructions().back().op != IROp::BR && currentBlock_->getInstructions().back().op != IROp::RET))) {
            emitDefers(deferStack_.size() - 1);
            emitDestructors(destructorStack_.size() - 1);
            currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(incName)}});
        }
        destructorStack_.pop_back();
        deferStack_.pop_back();

        startBlock(incName);
        iVal = IRValue::Register(nextTemp(), IRType::I32);
        currentBlock_->addInstruction({IROp::LOAD, iVal, {iAddr, IRValue::Constant(0, IRType::I32)}});
        IRValue one = IRValue::Constant(1, IRType::I32);
        IRValue newI = IRValue::Register(nextTemp(), IRType::I32);
        currentBlock_->addInstruction({IROp::ADD, newI, {iVal, one}});
        currentBlock_->addInstruction({IROp::STORE, std::nullopt, {iAddr, newI, IRValue::Constant(0, IRType::I32)}});
        currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(headerName)}});

        unwindDepths_.pop_back();
        continueLabels_.pop_back();
        breakLabels_.pop_back();
        symbolTable_.exitScope();

        startBlock(exitName);
    }

    void visit(ThrowStmt& stmt) override {
        if (currentFunction_) {
            stmt.getValue()->accept(*this);
            IRValue thrownVal = lastValue_;

            ensureExceptionGlobals();

            IRValue excValAddr = IRValue::Label("_exception_value");
            currentBlock_->addInstruction({IROp::STORE, std::nullopt, {excValAddr, thrownVal}});

            // CIDER-BUG-053: jump to the handler active at this dynamic
            // point, which may live in an ancestor frame. The pointer is
            // swapped at every try entry/exit, so a throw in a callee (or
            // after nested/recursive tries) unwinds to the right catch.
            IRValue bufPtr = IRValue::Register(nextTemp(), IRType::PTR);
            currentBlock_->addInstruction({IROp::LOAD, bufPtr, {IRValue::Label("_exception_buf_ptr")}});

            IRInstruction call;
            call.op = IROp::CALL;
            call.dest = std::nullopt;
            call.args.push_back(IRValue::Label("longjmp"));
            call.args.push_back(bufPtr);
            call.args.push_back(IRValue::Constant(1, IRType::I32));
            currentBlock_->addInstruction(call);

            startBlock(nextLabel("dead"));
        }
    }

    void visit(TryCatchStmt& stmt) override {
        if (currentFunction_) {
            // CIDER-BUG-050: try/catch bodies deliberately do not push a
            // defer/destructor scope. Throws are setjmp/longjmp-based, so a
            // scope here could not run its exits on the throw path; defers
            // written in these bodies stay pending until the enclosing block
            // exits rather than being dropped when the jump skips them.
            ensureExceptionGlobals();

            // CIDER-BUG-053: the try owns a jmp_buf allocated in the
            // function entry block (one per frame invocation, even when the
            // try runs in a loop) and saves the enclosing active handler in
            // a local. Nested tries and throws from callee frames then land
            // in the right handler instead of a shared global buffer.
            IRValue bufAddr = IRValue::Register(nextTemp(), IRType::PTR);
            emitEntryAlloca(bufAddr, kJmpBufSize);
            IRValue prevBuf = IRValue::Register(nextTemp(), IRType::PTR);
            currentBlock_->addInstruction({IROp::LOAD, prevBuf, {IRValue::Label("_exception_buf_ptr")}});
            currentBlock_->addInstruction({IROp::STORE, std::nullopt, {IRValue::Label("_exception_buf_ptr"), bufAddr}});
            activeTryScopes_.push_back({prevBuf, deferStack_.size()});

            IRValue setjmpDest = IRValue::Register(nextTemp(), IRType::I32);
            IRInstruction setjmpCall;
            setjmpCall.op = IROp::CALL;
            setjmpCall.dest = setjmpDest;
            setjmpCall.args.push_back(IRValue::Label("_setjmp"));
            setjmpCall.args.push_back(bufAddr);
            currentBlock_->addInstruction(setjmpCall);

            std::string tryBlockName = nextLabel("try_body");
            std::string catchBlockName = nextLabel("catch_body");
            std::string endName = nextLabel("try_end");

            currentBlock_->addInstruction({IROp::COND_BR, std::nullopt, {setjmpDest, IRValue::Label(catchBlockName), IRValue::Label(tryBlockName)}});

            startBlock(tryBlockName);
            symbolTable_.enterScope();
            for (auto& s : stmt.getTryBlock()) {
                s->accept(*this);
            }
            if (currentBlock_ && (currentBlock_->getInstructions().empty() ||
                                  (currentBlock_->getInstructions().back().op != IROp::BR &&
                                   currentBlock_->getInstructions().back().op != IROp::RET))) {
                // Normal completion: the enclosing handler is active again.
                currentBlock_->addInstruction({IROp::STORE, std::nullopt, {IRValue::Label("_exception_buf_ptr"), prevBuf}});
                currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(endName)}});
            }
            symbolTable_.exitScope();

            startBlock(catchBlockName);
            // The throw path skipped the normal exit: reinstall the
            // enclosing handler before the catch body runs, so a rethrow
            // targets the enclosing try rather than this one.
            currentBlock_->addInstruction({IROp::STORE, std::nullopt, {IRValue::Label("_exception_buf_ptr"), prevBuf}});
            symbolTable_.enterScope();

            IRValue catchAddr = IRValue::Register(nextTemp(), IRType::PTR);
            currentBlock_->addInstruction({IROp::ALLOCA, catchAddr, {IRValue::Constant(8, IRType::I32)}});
            variableValues_[stmt.getCatchVar()] = catchAddr;

            IRValue excVal = IRValue::Register(nextTemp(), IRType::I32);
            currentBlock_->addInstruction({IROp::LOAD, excVal, {IRValue::Label("_exception_value")}});
            currentBlock_->addInstruction({IROp::STORE, std::nullopt, {catchAddr, excVal, IRValue::Constant(0, IRType::I32)}});

            for (auto& s : stmt.getCatchBlock()) {
                s->accept(*this);
            }
            if (currentBlock_ && (currentBlock_->getInstructions().empty() ||
                                  (currentBlock_->getInstructions().back().op != IROp::BR &&
                                   currentBlock_->getInstructions().back().op != IROp::RET))) {
                currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(endName)}});
            }
            symbolTable_.exitScope();

            startBlock(endName);
            activeTryScopes_.pop_back();
        } else {
            symbolTable_.enterScope();
            for (auto& s : stmt.getTryBlock()) s->accept(*this);
            symbolTable_.exitScope();
            symbolTable_.enterScope();
            for (auto& s : stmt.getCatchBlock()) s->accept(*this);
            symbolTable_.exitScope();
        }
    }

    void visit(StringExpr& expr) override {
        IRValue strVal;
        strVal.kind = IRValue::CONSTANT;
        strVal.name = "\"" + expr.getValue() + "\"";
        strVal.type = IRType::PTR;
        strVal.constValue = 0;
        strVal.intValue = 0;
        lastValue_ = strVal;
        lastExprType_ = {"string", 0};
    }

    // CID-005: a wide literal crosses IR as a PTR constant whose name is
    // `L"` + UTF-16LE bytes (NUL unit included) + `"`. Codegen rewrites it
    // into a `dw` data block; the VM materializes the same bytes from the
    // name. Marking it PTR with a constant payload keeps every consumer
    // (stores, MEMCPY, indexing, calls) identical to the narrow string path.
    void visit(WideStringExpr& expr) override {
        std::string bytes;
        bytes.reserve(expr.getValue().size() * 2 + 2);
        for (char16_t u : expr.getValue()) {
            bytes.push_back((char)(u & 0xFF));
            bytes.push_back((char)((u >> 8) & 0xFF));
        }
        bytes.push_back('\0');
        bytes.push_back('\0');
        IRValue v;
        v.kind = IRValue::CONSTANT;
        v.type = IRType::PTR;
        v.name = "L\"" + bytes + "\"";
        lastValue_ = v;
        TypeInfo t;
        t.name = "u16";
        t.isArray = true;
        t.arraySize = (int)expr.getValue().size() + 1;
        lastExprType_ = t;
    }

    void visit(WideCharExpr& expr) override {
        lastValue_ = IRValue::Constant((int64_t)expr.getValue(), IRType::U16);
        lastExprType_ = {"u16", 0};
    }

    // --- string interpolation lowering ---

    static int decodedStringLength(const std::string& raw) {
        int n = 0;
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '\\' && i + 1 < raw.size()) {
                i++;  // escape sequence counts as one byte
            }
            n++;
        }
        return n;
    }

    IRValue emitCall(const std::string& callee, std::vector<IRValue> args) {
        IRValue dest = IRValue::Register(nextTemp(), IRType::PTR);
        IRInstruction call;
        call.op = IROp::CALL;
        call.dest = dest;
        call.args.push_back(IRValue::Label(callee));
        for (auto& a : args) call.args.push_back(a);
        currentBlock_->addInstruction(call);
        return dest;
    }

    IRValue emitAdd(IRValue a, IRValue b) {
        IRValue dest = IRValue::Register(nextTemp(), IRType::I32);
        currentBlock_->addInstruction({IROp::ADD, dest, {a, b}});
        return dest;
    }

    void ensureInterpolationHelpers() {
        if (interpHelpersEnsured_) return;
        interpHelpersEnsured_ = true;

        // _strlen(s: char*) -> int
        {
            auto fn = std::make_unique<FunctionIR>("_strlen", IRType::I32);
            fn->addParam("arg_s");
            auto entry = std::make_unique<BasicBlock>("entry");
            auto loop = std::make_unique<BasicBlock>("loop");
            auto body = std::make_unique<BasicBlock>("body");
            auto exit = std::make_unique<BasicBlock>("exit");
            IRValue s = IRValue::Register(nextTemp(), IRType::PTR);
            entry->addInstruction({IROp::ALLOCA, s, {IRValue::Constant(8, IRType::I32)}});
            entry->addInstruction({IROp::STORE, std::nullopt, {s, IRValue::Register("arg_s", IRType::PTR), IRValue::Constant(0, IRType::I32)}});
            IRValue i = IRValue::Register(nextTemp(), IRType::I32);
            entry->addInstruction({IROp::ALLOCA, i, {IRValue::Constant(4, IRType::I32)}});
            entry->addInstruction({IROp::STORE, std::nullopt, {i, IRValue::Constant(0, IRType::I32), IRValue::Constant(0, IRType::I32)}});
            entry->addInstruction({IROp::BR, std::nullopt, {IRValue::Label("loop")}});
            IRValue p = IRValue::Register(nextTemp(), IRType::PTR);
            loop->addInstruction({IROp::LOAD, p, {s, IRValue::Constant(0, IRType::I32)}});
            IRValue off = IRValue::Register(nextTemp(), IRType::I32);
            loop->addInstruction({IROp::LOAD, off, {i, IRValue::Constant(0, IRType::I32)}});
            IRValue addr = IRValue::Register(nextTemp(), IRType::PTR);
            loop->addInstruction({IROp::ADD, addr, {p, off}});
            IRValue ch = IRValue::Register(nextTemp(), IRType::U8);
            loop->addInstruction({IROp::LOAD, ch, {addr, IRValue::Constant(0, IRType::I32)}});
            IRValue cond = IRValue::Register(nextTemp(), IRType::BOOL);
            loop->addInstruction({IROp::CMP_NE, cond, {ch, IRValue::Constant(0, IRType::I32)}});
            loop->addInstruction({IROp::COND_BR, std::nullopt, {cond, IRValue::Label("body"), IRValue::Label("exit")}});
            IRValue i2 = IRValue::Register(nextTemp(), IRType::I32);
            body->addInstruction({IROp::LOAD, i2, {i, IRValue::Constant(0, IRType::I32)}});
            IRValue i3 = IRValue::Register(nextTemp(), IRType::I32);
            body->addInstruction({IROp::ADD, i3, {i2, IRValue::Constant(1, IRType::I32)}});
            body->addInstruction({IROp::STORE, std::nullopt, {i, i3, IRValue::Constant(0, IRType::I32)}});
            body->addInstruction({IROp::BR, std::nullopt, {IRValue::Label("loop")}});
            IRValue i4 = IRValue::Register(nextTemp(), IRType::I32);
            exit->addInstruction({IROp::LOAD, i4, {i, IRValue::Constant(0, IRType::I32)}});
            exit->addInstruction({IROp::RET, std::nullopt, {i4}});
            fn->addBlock(std::move(entry));
            fn->addBlock(std::move(loop));
            fn->addBlock(std::move(body));
            fn->addBlock(std::move(exit));
            module_.addFunction(std::move(fn));
        }

        // _str_from_int(n: int) -> char*
        {
            auto fn = std::make_unique<FunctionIR>("_str_from_int", IRType::PTR);
            fn->addParam("arg_n");
            auto entry = std::make_unique<BasicBlock>("entry");
            auto neg = std::make_unique<BasicBlock>("neg");
            auto cont0 = std::make_unique<BasicBlock>("cont0");
            auto negchar = std::make_unique<BasicBlock>("negchar");
            auto cont1 = std::make_unique<BasicBlock>("cont1");
            auto loop = std::make_unique<BasicBlock>("loop");
            auto revhdr = std::make_unique<BasicBlock>("revhdr");
            auto revloop = std::make_unique<BasicBlock>("revloop");
            auto done = std::make_unique<BasicBlock>("done");
            IRValue n = IRValue::Register(nextTemp(), IRType::I32);
            entry->addInstruction({IROp::ALLOCA, n, {IRValue::Constant(4, IRType::I32)}});
            entry->addInstruction({IROp::STORE, std::nullopt, {n, IRValue::Register("arg_n", IRType::I32), IRValue::Constant(0, IRType::I32)}});
            IRValue idx = IRValue::Register(nextTemp(), IRType::I32);
            entry->addInstruction({IROp::ALLOCA, idx, {IRValue::Constant(4, IRType::I32)}});
            entry->addInstruction({IROp::STORE, std::nullopt, {idx, IRValue::Constant(0, IRType::I32), IRValue::Constant(0, IRType::I32)}});
            IRValue negf = IRValue::Register(nextTemp(), IRType::I32);
            entry->addInstruction({IROp::ALLOCA, negf, {IRValue::Constant(4, IRType::I32)}});
            entry->addInstruction({IROp::STORE, std::nullopt, {negf, IRValue::Constant(0, IRType::I32), IRValue::Constant(0, IRType::I32)}});
            IRValue nv = IRValue::Register(nextTemp(), IRType::I32);
            entry->addInstruction({IROp::LOAD, nv, {n, IRValue::Constant(0, IRType::I32)}});
            IRValue lt = IRValue::Register(nextTemp(), IRType::BOOL);
            entry->addInstruction({IROp::CMP_LT, lt, {nv, IRValue::Constant(0, IRType::I32)}});
            entry->addInstruction({IROp::COND_BR, std::nullopt, {lt, IRValue::Label("neg"), IRValue::Label("cont0")}});
            IRValue nv2 = IRValue::Register(nextTemp(), IRType::I32);
            neg->addInstruction({IROp::LOAD, nv2, {n, IRValue::Constant(0, IRType::I32)}});
            IRValue negv = IRValue::Register(nextTemp(), IRType::I32);
            neg->addInstruction({IROp::SUB, negv, {IRValue::Constant(0, IRType::I32), nv2}});
            neg->addInstruction({IROp::STORE, std::nullopt, {n, negv, IRValue::Constant(0, IRType::I32)}});
            neg->addInstruction({IROp::STORE, std::nullopt, {negf, IRValue::Constant(1, IRType::I32), IRValue::Constant(0, IRType::I32)}});
            neg->addInstruction({IROp::BR, std::nullopt, {IRValue::Label("cont0")}});
            IRValue nf = IRValue::Register(nextTemp(), IRType::I32);
            cont0->addInstruction({IROp::LOAD, nf, {negf, IRValue::Constant(0, IRType::I32)}});
            IRValue isneg = IRValue::Register(nextTemp(), IRType::BOOL);
            cont0->addInstruction({IROp::CMP_EQ, isneg, {nf, IRValue::Constant(1, IRType::I32)}});
            IRValue buf = IRValue::Register(nextTemp(), IRType::PTR);
            cont0->addInstruction({IROp::CALL, buf, {IRValue::Label(runtimeHelperName("malloc")), IRValue::Constant(16, IRType::I32)}});
            cont0->addInstruction({IROp::COND_BR, std::nullopt, {isneg, IRValue::Label("negchar"), IRValue::Label("cont1")}});
            negchar->addInstruction({IROp::STORE, std::nullopt, {buf, IRValue::Constant(45, IRType::I32), IRValue::Constant(0, IRType::I32)}});
            negchar->addInstruction({IROp::STORE, std::nullopt, {idx, IRValue::Constant(1, IRType::I32), IRValue::Constant(0, IRType::I32)}});
            negchar->addInstruction({IROp::BR, std::nullopt, {IRValue::Label("loop")}});
            cont1->addInstruction({IROp::BR, std::nullopt, {IRValue::Label("loop")}});
            IRValue cur = IRValue::Register(nextTemp(), IRType::I32);
            loop->addInstruction({IROp::LOAD, cur, {n, IRValue::Constant(0, IRType::I32)}});
            IRValue dig = IRValue::Register(nextTemp(), IRType::I32);
            loop->addInstruction({IROp::MOD, dig, {cur, IRValue::Constant(10, IRType::I32)}});
            IRValue chv = IRValue::Register(nextTemp(), IRType::I32);
            loop->addInstruction({IROp::ADD, chv, {dig, IRValue::Constant(48, IRType::I32)}});
            IRValue ii = IRValue::Register(nextTemp(), IRType::I32);
            loop->addInstruction({IROp::LOAD, ii, {idx, IRValue::Constant(0, IRType::I32)}});
            IRValue dest = IRValue::Register(nextTemp(), IRType::PTR);
            loop->addInstruction({IROp::ADD, dest, {buf, ii}});
            loop->addInstruction({IROp::STORE, std::nullopt, {dest, chv, IRValue::Constant(0, IRType::I32)}});
            IRValue cur2 = IRValue::Register(nextTemp(), IRType::I32);
            loop->addInstruction({IROp::LOAD, cur2, {n, IRValue::Constant(0, IRType::I32)}});
            IRValue quot = IRValue::Register(nextTemp(), IRType::I32);
            loop->addInstruction({IROp::DIV, quot, {cur2, IRValue::Constant(10, IRType::I32)}});
            loop->addInstruction({IROp::STORE, std::nullopt, {n, quot, IRValue::Constant(0, IRType::I32)}});
            IRValue ii2 = IRValue::Register(nextTemp(), IRType::I32);
            loop->addInstruction({IROp::LOAD, ii2, {idx, IRValue::Constant(0, IRType::I32)}});
            IRValue ii3 = IRValue::Register(nextTemp(), IRType::I32);
            loop->addInstruction({IROp::ADD, ii3, {ii2, IRValue::Constant(1, IRType::I32)}});
            loop->addInstruction({IROp::STORE, std::nullopt, {idx, ii3, IRValue::Constant(0, IRType::I32)}});
            IRValue cur3 = IRValue::Register(nextTemp(), IRType::I32);
            loop->addInstruction({IROp::LOAD, cur3, {n, IRValue::Constant(0, IRType::I32)}});
            IRValue gt = IRValue::Register(nextTemp(), IRType::BOOL);
            loop->addInstruction({IROp::CMP_GT, gt, {cur3, IRValue::Constant(0, IRType::I32)}});
            loop->addInstruction({IROp::COND_BR, std::nullopt, {gt, IRValue::Label("loop"), IRValue::Label("revhdr")}});
            IRValue lo = IRValue::Register(nextTemp(), IRType::I32);
            revhdr->addInstruction({IROp::ALLOCA, lo, {IRValue::Constant(4, IRType::I32)}});
            IRValue nf2 = IRValue::Register(nextTemp(), IRType::I32);
            revhdr->addInstruction({IROp::LOAD, nf2, {negf, IRValue::Constant(0, IRType::I32)}});
            revhdr->addInstruction({IROp::STORE, std::nullopt, {lo, nf2, IRValue::Constant(0, IRType::I32)}});
            IRValue hi = IRValue::Register(nextTemp(), IRType::I32);
            revhdr->addInstruction({IROp::ALLOCA, hi, {IRValue::Constant(4, IRType::I32)}});
            IRValue ii4 = IRValue::Register(nextTemp(), IRType::I32);
            revhdr->addInstruction({IROp::LOAD, ii4, {idx, IRValue::Constant(0, IRType::I32)}});
            IRValue hi1 = IRValue::Register(nextTemp(), IRType::I32);
            revhdr->addInstruction({IROp::SUB, hi1, {ii4, IRValue::Constant(1, IRType::I32)}});
            revhdr->addInstruction({IROp::STORE, std::nullopt, {hi, hi1, IRValue::Constant(0, IRType::I32)}});
            IRValue lov = IRValue::Register(nextTemp(), IRType::I32);
            revhdr->addInstruction({IROp::LOAD, lov, {lo, IRValue::Constant(0, IRType::I32)}});
            IRValue hiv = IRValue::Register(nextTemp(), IRType::I32);
            revhdr->addInstruction({IROp::LOAD, hiv, {hi, IRValue::Constant(0, IRType::I32)}});
            IRValue revc = IRValue::Register(nextTemp(), IRType::BOOL);
            revhdr->addInstruction({IROp::CMP_LT, revc, {lov, hiv}});
            revhdr->addInstruction({IROp::COND_BR, std::nullopt, {revc, IRValue::Label("revloop"), IRValue::Label("done")}});
            IRValue lov2 = IRValue::Register(nextTemp(), IRType::I32);
            revloop->addInstruction({IROp::LOAD, lov2, {lo, IRValue::Constant(0, IRType::I32)}});
            IRValue pLo = IRValue::Register(nextTemp(), IRType::PTR);
            revloop->addInstruction({IROp::ADD, pLo, {buf, lov2}});
            IRValue tmp = IRValue::Register(nextTemp(), IRType::I32);
            revloop->addInstruction({IROp::LOAD, tmp, {pLo, IRValue::Constant(0, IRType::I32)}});
            IRValue hiv2 = IRValue::Register(nextTemp(), IRType::I32);
            revloop->addInstruction({IROp::LOAD, hiv2, {hi, IRValue::Constant(0, IRType::I32)}});
            IRValue pHi = IRValue::Register(nextTemp(), IRType::PTR);
            revloop->addInstruction({IROp::ADD, pHi, {buf, hiv2}});
            IRValue hiCh = IRValue::Register(nextTemp(), IRType::I32);
            revloop->addInstruction({IROp::LOAD, hiCh, {pHi, IRValue::Constant(0, IRType::I32)}});
            revloop->addInstruction({IROp::STORE, std::nullopt, {pLo, hiCh, IRValue::Constant(0, IRType::I32)}});
            revloop->addInstruction({IROp::STORE, std::nullopt, {pHi, tmp, IRValue::Constant(0, IRType::I32)}});
            IRValue lov3 = IRValue::Register(nextTemp(), IRType::I32);
            revloop->addInstruction({IROp::LOAD, lov3, {lo, IRValue::Constant(0, IRType::I32)}});
            IRValue lo1 = IRValue::Register(nextTemp(), IRType::I32);
            revloop->addInstruction({IROp::ADD, lo1, {lov3, IRValue::Constant(1, IRType::I32)}});
            revloop->addInstruction({IROp::STORE, std::nullopt, {lo, lo1, IRValue::Constant(0, IRType::I32)}});
            IRValue hiv3 = IRValue::Register(nextTemp(), IRType::I32);
            revloop->addInstruction({IROp::LOAD, hiv3, {hi, IRValue::Constant(0, IRType::I32)}});
            IRValue hi2 = IRValue::Register(nextTemp(), IRType::I32);
            revloop->addInstruction({IROp::SUB, hi2, {hiv3, IRValue::Constant(1, IRType::I32)}});
            revloop->addInstruction({IROp::STORE, std::nullopt, {hi, hi2, IRValue::Constant(0, IRType::I32)}});
            IRValue lov4 = IRValue::Register(nextTemp(), IRType::I32);
            revloop->addInstruction({IROp::LOAD, lov4, {lo, IRValue::Constant(0, IRType::I32)}});
            IRValue hiv4 = IRValue::Register(nextTemp(), IRType::I32);
            revloop->addInstruction({IROp::LOAD, hiv4, {hi, IRValue::Constant(0, IRType::I32)}});
            IRValue revc2 = IRValue::Register(nextTemp(), IRType::BOOL);
            revloop->addInstruction({IROp::CMP_LT, revc2, {lov4, hiv4}});
            revloop->addInstruction({IROp::COND_BR, std::nullopt, {revc2, IRValue::Label("revloop"), IRValue::Label("done")}});
            IRValue ii5 = IRValue::Register(nextTemp(), IRType::I32);
            done->addInstruction({IROp::LOAD, ii5, {idx, IRValue::Constant(0, IRType::I32)}});
            IRValue endp = IRValue::Register(nextTemp(), IRType::PTR);
            done->addInstruction({IROp::ADD, endp, {buf, ii5}});
            done->addInstruction({IROp::STORE, std::nullopt, {endp, IRValue::Constant(0, IRType::I32), IRValue::Constant(0, IRType::I32)}});
            done->addInstruction({IROp::RET, std::nullopt, {buf}});
            fn->addBlock(std::move(entry));
            fn->addBlock(std::move(neg));
            fn->addBlock(std::move(cont0));
            fn->addBlock(std::move(negchar));
            fn->addBlock(std::move(cont1));
            fn->addBlock(std::move(loop));
            fn->addBlock(std::move(revhdr));
            fn->addBlock(std::move(revloop));
            fn->addBlock(std::move(done));
            module_.addFunction(std::move(fn));
        }

        // _str_from_bool(v: bool) -> char*  — returns the "true"/"false" constants
        {
            auto fn = std::make_unique<FunctionIR>("_str_from_bool", IRType::PTR);
            fn->addParam("arg_v");
            auto entry = std::make_unique<BasicBlock>("entry");
            auto t = std::make_unique<BasicBlock>("t");
            auto f = std::make_unique<BasicBlock>("f");
            IRValue v = IRValue::Register(nextTemp(), IRType::I32);
            entry->addInstruction({IROp::ALLOCA, v, {IRValue::Constant(4, IRType::I32)}});
            entry->addInstruction({IROp::STORE, std::nullopt, {v, IRValue::Register("arg_v", IRType::I32), IRValue::Constant(0, IRType::I32)}});
            IRValue vv = IRValue::Register(nextTemp(), IRType::I32);
            entry->addInstruction({IROp::LOAD, vv, {v, IRValue::Constant(0, IRType::I32)}});
            IRValue cond = IRValue::Register(nextTemp(), IRType::BOOL);
            entry->addInstruction({IROp::CMP_EQ, cond, {vv, IRValue::Constant(1, IRType::I32)}});
            entry->addInstruction({IROp::COND_BR, std::nullopt, {cond, IRValue::Label("t"), IRValue::Label("f")}});
            IRValue strT;
            strT.kind = IRValue::CONSTANT;
            strT.name = "\"true\"";
            strT.type = IRType::PTR;
            t->addInstruction({IROp::RET, std::nullopt, {strT}});
            IRValue strF;
            strF.kind = IRValue::CONSTANT;
            strF.name = "\"false\"";
            strF.type = IRType::PTR;
            f->addInstruction({IROp::RET, std::nullopt, {strF}});
            fn->addBlock(std::move(entry));
            fn->addBlock(std::move(t));
            fn->addBlock(std::move(f));
            module_.addFunction(std::move(fn));
        }

        // _str_from_char(c: char) -> char* — a 2-byte string
        {
            auto fn = std::make_unique<FunctionIR>("_str_from_char", IRType::PTR);
            fn->addParam("arg_c");
            auto entry = std::make_unique<BasicBlock>("entry");
            IRValue c = IRValue::Register(nextTemp(), IRType::I32);
            entry->addInstruction({IROp::ALLOCA, c, {IRValue::Constant(4, IRType::I32)}});
            entry->addInstruction({IROp::STORE, std::nullopt, {c, IRValue::Register("arg_c", IRType::I32), IRValue::Constant(0, IRType::I32)}});
            IRValue buf = IRValue::Register(nextTemp(), IRType::PTR);
            entry->addInstruction({IROp::CALL, buf, {IRValue::Label(runtimeHelperName("malloc")), IRValue::Constant(2, IRType::I32)}});
            IRValue cv = IRValue::Register(nextTemp(), IRType::I32);
            entry->addInstruction({IROp::LOAD, cv, {c, IRValue::Constant(0, IRType::I32)}});
            entry->addInstruction({IROp::STORE, std::nullopt, {buf, cv, IRValue::Constant(0, IRType::I32)}});
            entry->addInstruction({IROp::STORE, std::nullopt, {buf, IRValue::Constant(0, IRType::I32), IRValue::Constant(1, IRType::I32)}});
            entry->addInstruction({IROp::RET, std::nullopt, {buf}});
            fn->addBlock(std::move(entry));
            module_.addFunction(std::move(fn));
        }
    }

    void visit(InterpolatedStringExpr& expr) override {
        ensureInterpolationHelpers();
        std::vector<IRValue> partVals;
        std::vector<IRValue> partLens;
        int constTotal = 0;
        for (const auto& part : expr.getParts()) {
            if (!part.isExpr) {
                int len = decodedStringLength(part.text);
                constTotal += len;
                IRValue strVal;
                strVal.kind = IRValue::CONSTANT;
                strVal.name = "\"" + part.text + "\"";
                strVal.type = IRType::PTR;
                partVals.push_back(strVal);
                partLens.push_back(IRValue::Constant(len, IRType::I32));
            } else {
                part.expr->accept(*this);
                IRValue val = lastValue_;
                IRValue s;
                const std::string& t = lastExprType_.name;
                if (t == "string") {
                    s = val;
                } else if (t == "int") {
                    s = emitCall("_str_from_int", {val});
                } else if (t == "bool") {
                    s = emitCall("_str_from_bool", {val});
                } else {  // char
                    s = emitCall("_str_from_char", {val});
                }
                partVals.push_back(s);
                partLens.push_back(emitCall("_strlen", {s}));
            }
        }
        // buf = malloc(constTotal + runtime lens)
        IRValue total = IRValue::Constant(constTotal, IRType::I32);
        for (const auto& len : partLens) {
            if (len.kind != IRValue::CONSTANT) total = emitAdd(total, len);
        }
        IRValue buf = emitCall(runtimeHelperName("malloc"), {total});
        // Fill: memcpy(buf + off, part, len); off += len.
        IRValue off = IRValue::Constant(0, IRType::I32);
        for (size_t i = 0; i < partVals.size(); ++i) {
            IRValue dst = IRValue::Register(nextTemp(), IRType::PTR);
            currentBlock_->addInstruction({IROp::ADD, dst, {buf, off}});
            currentBlock_->addInstruction({IROp::MEMCPY, std::nullopt, {dst, partVals[i], partLens[i]}});
            off = emitAdd(off, partLens[i]);
        }
        IRValue endp = IRValue::Register(nextTemp(), IRType::PTR);
        currentBlock_->addInstruction({IROp::ADD, endp, {buf, total}});
        currentBlock_->addInstruction({IROp::STORE, std::nullopt, {endp, IRValue::Constant(0, IRType::I32), IRValue::Constant(0, IRType::I32)}});
        lastValue_ = buf;
        lastExprType_ = {"string", 0};
    }
    void visit(BooleanExpr& expr) override {
        lastValue_ = IRValue::Constant(expr.getValue() ? 1 : 0, IRType::BOOL);
        lastExprType_ = {"bool", 0};
    }
    void visit(NilExpr&) override {
        lastValue_ = IRValue::Constant(0, IRType::PTR);
        lastExprType_ = {"nil", 0};
    }
    void visit(NilPtrExpr&) override {
        lastValue_ = IRValue::Constant(0, IRType::PTR);
        lastExprType_ = {"void", 1};
    }
    // CIDER-BUG-049: emit a byte string as static data and return its label.
    // Shared by string-literal fields and the global StringExpr path.
    // Single-quoted NASM strings process no escapes: '\\' would emit two
    // bytes and a bare apostrophe parses as empty plus an unterminated tail
    // (CID-024); numeric bytes are read identically by NASM and the VM
    // decoder.
    std::string emitStringData(const std::string& strData) {
        std::string strLabel = "_gs" + std::to_string(globalStrCount_++);
        std::string dataLine = strLabel + ": db ";
        for (size_t ci = 0; ci < strData.size(); ++ci) {
            if (ci > 0) dataLine += ", ";
            char c = strData[ci];
            if (c == '\n') dataLine += "10";
            else if (c == '\t') dataLine += "9";
            else if (c == '\\') dataLine += "92";
            else if (c == '\'') dataLine += "39";
            else if (c >= 32 && c < 127) dataLine += "'" + std::string(1, c) + "'";
            else dataLine += std::to_string((unsigned char)c);
        }
        dataLine += ", 0";
        module_.addStringData(dataLine);
        return strLabel;
    }

    // CIDER-BUG-049: folded integer constant typed/truncated for the field's
    // declared width. truncateIntToType does not narrow 64 -> 32 bits, so the
    // mask is applied here (what a register holds after a store of this
    // width). A qword store into a narrower field would clobber its neighbor.
    static IRValue typedFieldConstant(uint64_t v, IRType t) {
        switch (t) {
        case IRType::I8: return IRValue::Constant((int64_t)(int8_t)v, t);
        case IRType::U8: return IRValue::Constant((uint64_t)(v & 0xFFULL), t);
        case IRType::I16: return IRValue::Constant((int64_t)(int16_t)v, t);
        case IRType::U16: return IRValue::Constant((uint64_t)(v & 0xFFFFULL), t);
        case IRType::I32: return IRValue::Constant((int64_t)(int32_t)v, t);
        case IRType::U32: return IRValue::Constant((uint64_t)(v & 0xFFFFFFFFULL), t);
        case IRType::BOOL: return IRValue::Constant(v ? 1 : 0, t);
        default: return IRValue::Constant(v, t);
        }
    }

    // CIDER-BUG-049: materialise a class's field initializers as constant
    // stores at `objAddr`, and value-initialise scalar fields that have none.
    // The checker admits only foldable initializer shapes (literals, const
    // references, constant arithmetic, string literals, function names), so
    // no runtime initializer pass is needed. Inherited fields lead
    // `classSym->fields` (the collector copies them), so base initializers
    // run before derived ones. Field LAYOUT is untouched: only values are
    // stored, at the offsets the collector already assigned.
    void emitClassFieldInitializers(const std::shared_ptr<StructSymbol>& classSym,
                                    IRValue objAddr) {
        if (!classSym || !currentFunction_) return;
        std::string classMod = definingModuleFor(classSym, classSym->name);
        for (const auto& field : classSym->fields) {
            TypeInfo fieldType =
                canonicalizeNominalInModule(resolveTypeAlias(field.type, classMod), classMod);
            IRType fieldIR = (fieldType.pointerLevel > 0) ? IRType::PTR : mapType(fieldType.name);
            bool ptrLike = fieldType.pointerLevel > 0 || fieldType.isFunctionPointer ||
                           fieldType.name == "string";
            auto storeField = [&](IRValue val) {
                currentBlock_->addInstruction(
                    {IROp::STORE, std::nullopt,
                     {objAddr, val, IRValue::Constant(field.offset, IRType::I32)}});
            };
            if (!field.initializer) {
                // An escape-analysed `new` lands on the native stack, so the
                // zero default of an initializer-less scalar field is only
                // deterministic if it is actually stored. Aggregates keep the
                // legacy layout-only treatment.
                if (ptrLike) {
                    storeField(IRValue::Constant(0, IRType::PTR));
                } else if (!fieldType.isArray && !fieldType.isInterface &&
                           !fieldType.isSlice && !isStructValueType(fieldType, classMod)) {
                    if (fieldIR == IRType::F32 || fieldIR == IRType::F64) {
                        storeField(IRValue::Constant(0.0, fieldIR));
                    } else {
                        storeField(typedFieldConstant(0, fieldIR));
                    }
                }
                continue;
            }
            Expression* init = field.initializer;
            // Field initializers are const-like: identifiers bind in the
            // declaring class's module, not at the creation site
            // (CIDER-BUG-048's const-folding rule).
            Scope* prevScope = constEvalScope_;
            if (Scope* declScope = scopeForModule(field.initializerModule)) {
                constEvalScope_ = declScope;
            }
            bool folded = false;
            if (auto strInit = dynamic_cast<StringExpr*>(init)) {
                storeField(IRValue::Label(emitStringData(strInit->getValue())));
                folded = true;
            } else if (ptrLike) {
                std::string fnTarget = functionPointerInitTarget(init);
                uint64_t ptrVal = 0;
                if (!fnTarget.empty()) {
                    storeField(IRValue::Label(fnTarget));
                    folded = true;
                } else if (tryEvalConstU64(init, ptrVal)) {
                    storeField(IRValue::Constant((int64_t)ptrVal, IRType::PTR));
                    folded = true;
                }
            } else if (fieldType.pointerLevel == 0 && !fieldType.isArray) {
                if (fieldIR == IRType::F32 || fieldIR == IRType::F64) {
                    double fv = 0;
                    if (tryEvalConstF64(init, fv)) {
                        storeField(IRValue::Constant(fv, fieldIR));
                        folded = true;
                    }
                } else {
                    uint64_t v = 0;
                    if (tryEvalConstU64(init, v)) {
                        storeField(typedFieldConstant(v, fieldIR));
                        folded = true;
                    }
                }
            }
            if (!folded && reporter_) {
                // Checker-accepted shape that still cannot fold (division by
                // zero, oversized shift, ...): report it — never a silent
                // zero (the CIDER-BUG-048 safety net).
                reporter_->error("Class field initializer in declaration of '" +
                                     field.name + "' must use constant expressions",
                                 classSym->line);
            }
            constEvalScope_ = prevScope;
        }
    }

    void visit(NewExpr& expr) override {
        TypeInfo type = TypeInfo::fromAST(expr.getNewType());
        int size = getTypeSize(type);
        IRValue dest = IRValue::Register(nextTemp(), IRType::PTR);
        IRInstruction call;
        call.op = IROp::CALL;
        call.dest = dest;
        call.args.push_back(IRValue::Label(runtimeHelperName("malloc")));
        call.args.push_back(IRValue::Constant(size, IRType::I32));
        currentBlock_->addInstruction(call);

        // Initialize vptr if class is virtual (CID-020/021 spec §6: the
        // label mangles with the class's defining module, matching the
        // vtable key emitted in generate()).
        auto sym = resolveNominalType(type.name);
        if (sym && sym->kind == SymbolKind::CLASS) {
            auto classSym = std::static_pointer_cast<StructSymbol>(sym);
            if (!classSym->vtable.empty()) {
                IRValue vptrVal = IRValue::Label("vtable_" + vtableKeyForClass(classSym));
                currentBlock_->addInstruction({IROp::STORE, std::nullopt, {dest, vptrVal, IRValue::Constant(0, IRType::I32)}});
            }
            // CIDER-BUG-049: run the class's field initializers (inherited
            // fields included, in layout order).
            emitClassFieldInitializers(classSym, dest);
        }

        lastValue_ = dest;
        TypeInfo resType = type;
        resType.pointerLevel++;
        lastExprType_ = resType;
    }
    void visit(DeleteStmt& stmt) override {
        stmt.getTarget()->accept(*this);
        IRValue ptr = lastValue_;
        // Check if the deleted object's type has a destructor
        TypeInfo deletedType = lastExprType_;
        if (deletedType.pointerLevel > 0) {
            TypeInfo baseType = deletedType;
            baseType.pointerLevel--;
            // CID-020/021: module-aware so `delete` on a qualified-typed
            // object finds a module class's destructor (bare fallback
            // preserves legacy behavior where it resolved).
            auto sym = resolveNominalType(baseType.name);
            if (sym && sym->kind == SymbolKind::CLASS) {
                auto classSym = std::static_pointer_cast<StructSymbol>(sym);
                if (!classSym->destructorName.empty()) {
                    IRInstruction dtorCall;
                    dtorCall.op = IROp::CALL;
                    dtorCall.dest = std::nullopt;
                    // CID-020/021 (spec §5/§6): see the destructor-stack
                    // site; both must match the definition.
                    auto dtorDef = resolveFunctionDef(classSym->destructorName,
                                                      definingScopeFor(classSym, classSym->name));
                    dtorCall.args.push_back(
                        IRValue::Label(emitFunctionName(dtorDef.second, classSym->destructorName)));
                    dtorCall.args.push_back(ptr);
                    currentBlock_->addInstruction(dtorCall);
                }
            }
        }
        IRInstruction call;
        call.op = IROp::CALL;
        call.dest = std::nullopt;
        call.args.push_back(IRValue::Label(runtimeHelperName("free")));
        call.args.push_back(ptr);
        currentBlock_->addInstruction(call);
    }
    void visit(IndexExpr& expr) override {
        bool wasAddress = isAddress_;
        isAddress_ = false;
        expr.getObject()->accept(*this);
        IRValue obj = lastValue_;
        TypeInfo baseType = lastExprType_;

        expr.getIndex()->accept(*this);
        IRValue idx = lastValue_;
        isAddress_ = wasAddress;

        // CIDER-BUG-019: `type X = [T; N]` / `type Cb = fn(...)` element
        // aliases must resolve before the shape strip and the load/address
        // decision. resolveTypeAlias stops at array/fn wrappers, so resolve
        // the base first, strip, then resolve the element.
        baseType = resolveTypeAlias(baseType);
        TypeInfo elemType = baseType;
        if (elemType.isArray) {
            elemType.isArray = false;
            elemType.arraySize = 0;
        } else if (elemType.pointerLevel > 0) {
            elemType.pointerLevel--;
        } else if (elemType.name == "string") {
            elemType.name = "char";
        }
        elemType = resolveTypeAlias(elemType);

        int elemSize = getTypeSize(elemType);
        IRValue scaledIdx = IRValue::Register(nextTemp(), IRType::I32);
        currentBlock_->addInstruction({IROp::MUL, scaledIdx, {idx, IRValue::Constant(elemSize, IRType::I32)}});
        IRValue addr = IRValue::Register(nextTemp(), IRType::PTR);
        currentBlock_->addInstruction({IROp::ADD, addr, {obj, scaledIdx}});

        lastExprType_ = elemType;
        bool elemIsStruct = (elemType.pointerLevel == 0);
        if (elemIsStruct) {
            // CID-031 regression: elemType may carry a canonical defining-module
            // name (`lib::E`) after symbol-type canonicalization; a bare
            // symbolTable_ lookup misses it and the element was then treated as
            // a loaded value instead of an address, producing an extra
            // dereference on `entries[num].field = v`.
            // CIDER-BUG-019: alias providers are STRUCT-kind plain Symbols,
            // so go through the alias-safe resolver (an alias-to-fn element
            // used to stay "struct" and the call then invoked the slot
            // address instead of the loaded function pointer).
            elemIsStruct = resolveStructOrClassSymbol(elemType) != nullptr;
        }
        if (isAddress_ || elemIsStruct) {
            lastValue_ = addr;
        } else {
            IRType irType = (elemType.pointerLevel > 0 || elemType.isFunctionPointer)
                                ? IRType::PTR
                                : mapType(elemType.name);
            IRValue dest = IRValue::Register(nextTemp(), irType);
            currentBlock_->addInstruction({IROp::LOAD, dest, {addr, IRValue::Constant(0, IRType::I32)}});
            lastValue_ = dest;
        }
    }
    void visit(GroupingExpr& expr) override { expr.getExpression()->accept(*this); }
    void visit(SizeofExpr& expr) override {
        TypeInfo targetType = TypeInfo::fromAST(expr.getTargetType());
        int size = getTypeSize(targetType);
        lastValue_ = IRValue::Constant(size, IRType::I32);
    }
    void visit(CastExpr& expr) override {
        expr.getExpression()->accept(*this);
        TypeInfo targetType = TypeInfo::fromAST(expr.getTargetType());
        // CIDER-BUG-018: an array target is a layout reinterpretation, not
        // a scalar conversion — pass the address-like value through (the
        // checker pins size and element-type equality). Without this the
        // cast lowered to a scalar BIT_AND of the data label, which native
        // codegen emitted as a 16-bit immediate (R_X86_64_16 link error).
        if (targetType.isArray) {
            lastExprType_ = targetType;
            return;
        }
        IRType irType = (targetType.pointerLevel > 0) ? IRType::PTR : mapType(targetType.name);

        IRValue dest = IRValue::Register(nextTemp(), irType);

        IRType srcType = lastValue_.type;
        // Detect int<->float conversions. Narrow/unsigned/bool float
        // targets had no conversion at all (CID-025): convert with
        // 64-bit precision first, then narrow. I32/I64 keep the direct
        // path below.
        bool tgtNeedsFloatHelp = (irType == IRType::I8 || irType == IRType::U8 ||
                                  irType == IRType::I16 || irType == IRType::U16 ||
                                  irType == IRType::U32 || irType == IRType::U64 ||
                                  irType == IRType::BOOL);
        if ((srcType == IRType::F32 || srcType == IRType::F64) && tgtNeedsFloatHelp) {
            lastValue_ = convertFloatToInt(lastValue_, targetType);
        } else if (lastValue_.kind == IRValue::CONSTANT &&
                   ((srcType == IRType::F32 && irType == IRType::F64) ||
                    (srcType == IRType::F64 && irType == IRType::F32))) {
            // CID-003 review: a width cast of a literal is a compile-time
            // conversion context. Folding keeps the parsed double for
            // F32->F64 (`16777217.0 as f64` is not representable in f32,
            // so the runtime cvtss2sd via the F32 constant pool truncated
            // it on native) and rounds F64->F32 once through float.
            lastValue_ = IRValue::Constant(irType == IRType::F32
                                               ? (double)(float)lastValue_.constValue
                                               : lastValue_.constValue,
                                           irType);
        } else {
            if ((srcType == IRType::F32 || srcType == IRType::F64) &&
                (irType == IRType::I32 || irType == IRType::I64)) {
                currentBlock_->addInstruction({IROp::CVT_FTOI, dest, {lastValue_}});
            } else if (isIntIRType(srcType) &&
                       (irType == IRType::F32 || irType == IRType::F64)) {
                // CID-003 follow-up (I2): u32/u64/narrow sources convert too
                // (a `3000000000 as f64` used to emit `movsd xmm, imm`).
                currentBlock_->addInstruction({IROp::CVT_ITOF, dest, {lastValue_}});
            } else if ((srcType == IRType::F32 && irType == IRType::F64) ||
                       (srcType == IRType::F64 && irType == IRType::F32)) {
                currentBlock_->addInstruction({srcType == IRType::F32 ? IROp::CVT_F32F64
                                                                      : IROp::CVT_F64F32,
                                               dest, {lastValue_}});
            } else if (irType == IRType::I8 || irType == IRType::U8) {
                currentBlock_->addInstruction({IROp::BIT_AND, dest, {lastValue_, IRValue::Constant(0xFF, IRType::I32)}});
            } else if (irType == IRType::I16 || irType == IRType::U16) {
                currentBlock_->addInstruction({IROp::BIT_AND, dest, {lastValue_, IRValue::Constant(0xFFFF, IRType::I32)}});
            } else {
                currentBlock_->addInstruction({IROp::MOV, dest, {lastValue_}});
            }
            lastValue_ = dest;
        }

        lastExprType_ = targetType;
    }
    void visit(MatchStmt& stmt) override {
        stmt.getExpression()->accept(*this);
        IRValue matchVal = lastValue_;
        std::string exitName = nextLabel("match_exit");
        for (auto& arm : stmt.getArms()) {
            std::string armName = nextLabel("arm");
            std::string nextCheckName = nextLabel("next_check");
            bool isDefault = false;
            if (auto pid = dynamic_cast<IdentifierExpr*>(arm.pattern.get())) {
                isDefault = (pid->getName() == "default");
            }
            if (isDefault) {
                currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(armName)}});
            } else {
                arm.pattern->accept(*this);
                IRValue patternVal = lastValue_;
                IRValue cond = IRValue::Register(nextTemp(), IRType::BOOL);
                currentBlock_->addInstruction({IROp::CMP_EQ, cond, {matchVal, patternVal}});
                currentBlock_->addInstruction({IROp::COND_BR, std::nullopt, {cond, IRValue::Label(armName), IRValue::Label(nextCheckName)}});
            }
            startBlock(armName);
            // CIDER-BUG-050: an arm body is its own scope (an implicit block
            // when written as a single statement), so its defers run at that
            // arm's end rather than at the enclosing block's exit.
            deferStack_.push_back({});
            destructorStack_.push_back({});
            arm.body->accept(*this);
            if (currentBlock_ && (currentBlock_->getInstructions().empty() || currentBlock_->getInstructions().back().op != IROp::BR)) {
                emitDefers(deferStack_.size() - 1);
                emitDestructors(destructorStack_.size() - 1);
                currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(exitName)}});
            }
            destructorStack_.pop_back();
            deferStack_.pop_back();
            startBlock(nextCheckName);
        }
        if (currentBlock_) currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(exitName)}});
        startBlock(exitName);
    }
    void visit(BlockStmt& stmt) override {
        symbolTable_.enterScope();
        deferStack_.push_back({});
        destructorStack_.push_back({});
        for (auto& s : stmt.getStatements()) s->accept(*this);
        for (auto it = deferStack_.back().rbegin(); it != deferStack_.back().rend(); ++it) (*it)->accept(*this);
        for (auto it = destructorStack_.back().rbegin(); it != destructorStack_.back().rend(); ++it) {
            IRInstruction call;
            call.op = IROp::CALL;
            call.dest = std::nullopt;
            call.args.push_back(IRValue::Label(it->mangledName));
            call.args.push_back(it->varAddr);
            currentBlock_->addInstruction(call);
        }
        destructorStack_.pop_back();
        deferStack_.pop_back();
        symbolTable_.exitScope();
    }
    void visit(ExpressionStmt& stmt) override { stmt.getExpression()->accept(*this); }
    void visit(ClassDeclStmt& stmt) override {
        // CID-020/021 (spec §6): stash the resolved class object for vtable
        // emission in generate(). Resolved in the current (own) scope, so a
        // module class keeps its own object and same-named classes in two
        // modules stay distinct entries that mangle apart. Skipped when
        // unresolvable (the checker rejects such programs first).
        auto csym = symbolTable_.resolve(stmt.getName());
        if (csym && (csym->kind == SymbolKind::STRUCT || csym->kind == SymbolKind::CLASS))
            classSyms_.push_back(std::static_pointer_cast<StructSymbol>(csym));
        std::string oldClass = currentClassName_;
        currentClassName_ = stmt.getName();
        for (auto& member : stmt.getMembers()) {
            if (dynamic_cast<FunctionDeclStmt*>(member.get())) member->accept(*this);
        }
        currentClassName_ = oldClass;
    }
    void visit(ExportStmt& stmt) override { if (auto inner = stmt.getStmt()) inner->accept(*this); }
    void visit(ThisExpr& /*expr*/) override {
        if (variableValues_.count("this")) {
            IRValue addr = variableValues_["this"];
            IRValue dest = IRValue::Register(nextTemp(), IRType::PTR);
            currentBlock_->addInstruction({IROp::LOAD, dest, {addr, IRValue::Constant(0, IRType::I32)}});
            lastValue_ = dest;
            lastExprType_ = {currentClassName_, 1};
            lastExprType_.isUserDefined = true;
        } else {
            // Handle error or default
            lastValue_ = IRValue::Constant(0, IRType::PTR);
        }
    }
    void visit(ListExpr& expr) override {
        if (!currentFunction_) return;

        auto& elements = expr.getElements();
        if (elements.empty()) {
            lastValue_ = IRValue::Constant(0, IRType::PTR);
            lastExprType_ = {"void", 1};
            return;
        }

        // Determine element type from first element
        elements[0]->accept(*this);
        IRValue firstVal = lastValue_;
        TypeInfo elemType = lastExprType_;
        int elemSize = getTypeSize(elemType);

        // Allocate stack space for all elements
        int totalSize = elemSize * (int)elements.size();
        IRValue addr = IRValue::Register(nextTemp(), IRType::PTR);
        currentBlock_->addInstruction({IROp::ALLOCA, addr, {IRValue::Constant(totalSize, IRType::I32)}});

        // Store first element
        currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, firstVal, IRValue::Constant(0, IRType::I32)}});

        // Store remaining elements
        for (size_t i = 1; i < elements.size(); i++) {
            elements[i]->accept(*this);
            if (lastExprType_.name != elemType.name ||
                lastExprType_.pointerLevel != elemType.pointerLevel) {
                lastValue_ = IRValue::Constant(0, IRType::PTR);
                lastExprType_ = {"error", 0};
                return;
            }
            IRValue offset = IRValue::Constant(i * elemSize, IRType::I32);
            currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, lastValue_, offset}});
        }

        lastValue_ = addr;
        lastExprType_ = elemType;
        lastExprType_.pointerLevel++;
    }

    void visit(TableExpr& expr) override {
        if (!currentFunction_) return;

        auto& entries = expr.getEntries();
        if (entries.empty()) {
            lastValue_ = IRValue::Constant(0, IRType::PTR);
            lastExprType_ = {"void", 1};
            return;
        }

        // Determine key/value types and sizes from first entry
        entries[0].first->accept(*this);
        IRValue firstKey = lastValue_;
        TypeInfo keyType = lastExprType_;
        int keySize = getTypeSize(keyType);

        entries[0].second->accept(*this);
        IRValue firstVal = lastValue_;
        TypeInfo valType = lastExprType_;
        int valSize = getTypeSize(valType);

        int entrySize = keySize + valSize;
        int totalSize = entrySize * (int)entries.size();
        IRValue addr = IRValue::Register(nextTemp(), IRType::PTR);
        currentBlock_->addInstruction({IROp::ALLOCA, addr, {IRValue::Constant(totalSize, IRType::I32)}});

        // Store first entry
        currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, firstKey, IRValue::Constant(0, IRType::I32)}});
        currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, firstVal, IRValue::Constant(keySize, IRType::I32)}});

        // Store remaining entries
        for (size_t i = 1; i < entries.size(); i++) {
            entries[i].first->accept(*this);
            int kvOffset = i * entrySize;
            currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, lastValue_, IRValue::Constant(kvOffset, IRType::I32)}});
            entries[i].second->accept(*this);
            currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, lastValue_, IRValue::Constant(kvOffset + keySize, IRType::I32)}});
        }

        lastValue_ = addr;
        lastExprType_ = {"void", 1};
    }
    void visit(SliceExpr& expr) override {
        if (!currentFunction_) return;

        bool wasAddress = isAddress_;
        isAddress_ = false;
        expr.getObject()->accept(*this);
        IRValue base = lastValue_;
        TypeInfo baseType = lastExprType_;

        IRValue start;
        if (expr.getStart()) {
            expr.getStart()->accept(*this);
            start = lastValue_;
        } else {
            start = IRValue::Constant(0, IRType::I32);
        }

        IRValue end;
        if (expr.getEnd()) {
            expr.getEnd()->accept(*this);
            end = lastValue_;
        } else {
            end = IRValue::Constant(baseType.arraySize, IRType::I32);
        }
        isAddress_ = wasAddress;

        TypeInfo elemType = baseType;
        if (elemType.isArray) {
            elemType.isArray = false;
            elemType.arraySize = 0;
        }
        int elemSize = getTypeSize(elemType);

        IRValue scaledStart = IRValue::Register(nextTemp(), IRType::I32);
        currentBlock_->addInstruction({IROp::MUL, scaledStart, {start, IRValue::Constant(elemSize, IRType::I32)}});
        IRValue ptr = IRValue::Register(nextTemp(), IRType::PTR);
        currentBlock_->addInstruction({IROp::ADD, ptr, {base, scaledStart}});

        IRValue len = IRValue::Register(nextTemp(), IRType::I32);
        currentBlock_->addInstruction({IROp::SUB, len, {end, start}});

        IRValue sliceAddr = IRValue::Register(nextTemp(), IRType::PTR);
        currentBlock_->addInstruction({IROp::ALLOCA, sliceAddr, {IRValue::Constant(16, IRType::I32)}});

        currentBlock_->addInstruction({IROp::STORE, std::nullopt, {sliceAddr, ptr, IRValue::Constant(0, IRType::I32)}});
        currentBlock_->addInstruction({IROp::STORE, std::nullopt, {sliceAddr, len, IRValue::Constant(8, IRType::I32)}});

        lastValue_ = sliceAddr;
        lastExprType_ = elemType;
        lastExprType_.isSlice = true;
    }
    void visit(TernaryExpr& expr) override {
        expr.getCondition()->accept(*this);
        IRValue cond = lastValue_;

        std::string thenName = nextLabel("tern_then");
        std::string elseName = nextLabel("tern_else");
        std::string mergeName = nextLabel("tern_end");

        currentBlock_->addInstruction({IROp::COND_BR, std::nullopt, {cond, IRValue::Label(thenName), IRValue::Label(elseName)}});

        startBlock(thenName);
        expr.getThenExpr()->accept(*this);
        IRValue thenVal = lastValue_;
        TypeInfo thenType = lastExprType_;
        BasicBlock* thenBlock = currentBlock_.get();

        startBlock(elseName);
        expr.getElseExpr()->accept(*this);
        IRValue elseVal = lastValue_;
        TypeInfo elseType = lastExprType_;
        BasicBlock* elseBlock = currentBlock_.get();

        // CID-003 follow-up (I4): the PHI type is the common type of both
        // arms, and each arm converts inside its own block (a conversion
        // after the merge would not dominate the PHI). The type used to
        // come from the then-arm only, so `c ? d : 1.5` merged an F32
        // value into an F64 PHI (`pop xmm` on native). Float arms promote
        // to f64 when either side is f64, matching binary expressions.
        IRType thenIR = thenType.pointerLevel > 0 ? IRType::PTR : mapType(thenType.name);
        IRType elseIR = elseType.pointerLevel > 0 ? IRType::PTR : mapType(elseType.name);
        bool thenFloat = (thenIR == IRType::F32 || thenIR == IRType::F64);
        bool elseFloat = (elseIR == IRType::F32 || elseIR == IRType::F64);
        bool floatMerge = thenFloat || elseFloat;
        IRType phiType = thenIR;
        if (floatMerge) {
            phiType = (thenIR == IRType::F64 || elseIR == IRType::F64)
                          ? IRType::F64 : IRType::F32;
            if (isIntIRType(thenIR)) {
                thenVal = convertIntToFloat(thenVal, phiType, thenBlock);
            } else if (thenIR != phiType) {
                thenVal = convertFloatWidth(thenVal, phiType, thenBlock);
            }
            if (isIntIRType(elseIR)) {
                elseVal = convertIntToFloat(elseVal, phiType, elseBlock);
            } else if (elseIR != phiType) {
                elseVal = convertFloatWidth(elseVal, phiType, elseBlock);
            }
        }

        thenBlock->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(mergeName)}});
        std::string thenEndName = thenBlock->getName();
        elseBlock->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(mergeName)}});
        std::string elseEndName = elseBlock->getName();

        startBlock(mergeName);
        IRValue phiDest = IRValue::Register(nextTemp(), phiType);
        IRInstruction phiInstr;
        phiInstr.op = IROp::PHI;
        phiInstr.dest = phiDest;
        phiInstr.phiArgs[thenEndName] = thenVal;
        phiInstr.phiArgs[elseEndName] = elseVal;
        currentBlock_->addInstruction(phiInstr);
        lastValue_ = phiDest;
        lastExprType_ = floatMerge
                            ? TypeInfo{phiType == IRType::F64 ? "f64" : "float", 0}
                            : thenType;
    }

    void visit(SuperExpr& expr) override {
        if (currentClassName_.empty() || !currentFunction_) {
            lastValue_ = IRValue::Constant(0, IRType::PTR);
            lastExprType_ = {"error", 0};
            return;
        }
        auto classSym = symbolTable_.resolve(currentClassName_);
        if (!classSym || classSym->kind != SymbolKind::CLASS) {
            lastValue_ = IRValue::Constant(0, IRType::PTR);
            lastExprType_ = {"error", 0};
            return;
        }
        auto cs = std::static_pointer_cast<StructSymbol>(classSym);
        if (cs->parentName.empty()) {
            lastValue_ = IRValue::Constant(0, IRType::PTR);
            lastExprType_ = {"error", 0};
            return;
        }
        auto parentSym = symbolTable_.resolve(cs->parentName);
        if (!parentSym || (parentSym->kind != SymbolKind::CLASS && parentSym->kind != SymbolKind::STRUCT)) {
            lastValue_ = IRValue::Constant(0, IRType::PTR);
            lastExprType_ = {"error", 0};
            return;
        }
        auto ps = std::static_pointer_cast<StructSymbol>(parentSym);
        for (auto& m : ps->methods) {
            if (m.name == expr.getMember()) {
                // CID-020/021 (spec §5/§6): super-call targets mangle like
                // every other method reference.
                auto methodDef = resolveFunctionDef(m.mangledName, definingScopeFor(ps, ps->name));
                lastValue_ = IRValue::Label(emitFunctionName(methodDef.second, m.mangledName));
                lastExprType_ = m.returnType;
                return;
            }
        }
        lastValue_ = IRValue::Constant(0, IRType::PTR);
        lastExprType_ = {"error", 0};
    }
    void visit(AwaitExpr& expr) override {
        // v1 async semantics: the awaited async function runs to completion
        // (cooperative, synchronous). The await lowers to the plain call;
        // the value is the callee's result. The suspension machinery (state
        // machines + scheduler) lands with the stdlib I/O layer.
        if (!currentFunction_) {
            lastValue_ = IRValue::Constant(0, IRType::I32);
            lastExprType_ = {"int", 0};
            return;
        }
        expr.getValue()->accept(*this);
        // The accept emitted the call; its result register is the awaited
        // value.
    }
    void visit(ArrayTypeAST&) override {}
    void visit(TypeAST&) override {}
    void visit(StructDeclStmt& stmt) override {
        // CID-020/021 (spec §6): stash the resolved struct object for the
        // interface-vtable loop in generate(); see visit(ClassDeclStmt).
        auto ssym = symbolTable_.resolve(stmt.getName());
        if (ssym && (ssym->kind == SymbolKind::STRUCT || ssym->kind == SymbolKind::CLASS))
            structSyms_.push_back(std::static_pointer_cast<StructSymbol>(ssym));
        lastExprType_ = {stmt.getName(), 0};
    }
    void visit(EnumDeclStmt&) override {}
    void visit(TypeAliasStmt&) override {
        // Type aliases are resolved at the type level; no runtime code generated
    }
    void visit(ImportStmt&) override {}
    void visit(FromImportStmt&) override {}
    void visit(ExportExpressionStmt&) override {}
    void visit(InterfaceDeclStmt& stmt) override {
        // CID-020/021 (spec §6): stash the resolved interface object for
        // the vtable loop in generate(); see visit(ClassDeclStmt).
        auto isym = symbolTable_.resolve(stmt.getName());
        if (isym && isym->kind == SymbolKind::INTERFACE)
            ifaceSyms_.push_back(std::static_pointer_cast<InterfaceSymbol>(isym));
    }

    // CIDER-BUG-053: a jmp_buf slot. glibc's is 25 qwords (200 bytes); the
    // pomeos runtime only stores 8 but the larger slot is harmless.
    static constexpr int kJmpBufSize = 200;

    // Allocate a try's jmp_buf in the function's entry block so a try that
    // runs in a loop reuses one slot per frame invocation. The ALLOCA has no
    // register operands, so it can go anywhere after the block's leading
    // PHIs (the entry block may already be terminated by then).
    void emitEntryAlloca(IRValue dest, int size) {
        IRInstruction allocaInstr{IROp::ALLOCA, dest, {IRValue::Constant(size, IRType::I32)}};
        if (!functionEntryBlock_ || functionEntryBlock_ == currentBlock_.get()) {
            currentBlock_->addInstruction(std::move(allocaInstr));
            return;
        }
        auto& insts = functionEntryBlock_->getInstructions();
        auto pos = insts.begin();
        while (pos != insts.end() && pos->op == IROp::PHI) ++pos;
        insts.insert(pos, std::move(allocaInstr));
    }

    // Restore _exception_buf_ptr for every try scope entered inside the
    // blocks being unwound. firstDeferDepth is the deferStack_ size at the
    // loop's entry (unwindDepths_.back()): the loop pushes its own scope
    // after recording it, so a try inside the unwound region has a strictly
    // larger depth. Return passes 0 to unwind every active try.
    void emitExceptionUnwind(size_t firstDeferDepth) {
        for (size_t i = activeTryScopes_.size(); i > 0; --i) {
            if (activeTryScopes_[i - 1].deferDepth <= firstDeferDepth) continue;
            currentBlock_->addInstruction({IROp::STORE, std::nullopt,
                                           {IRValue::Label("_exception_buf_ptr"),
                                            activeTryScopes_[i - 1].prevBuf}});
        }
    }

    void ensureExceptionGlobals() {
        if (!exceptionGlobalsEnsured_) {
            exceptionGlobalsEnsured_ = true;
            // CIDER-BUG-053: _exception_buf_ptr holds the jmp_buf of the
            // innermost active try (0 when none). Each try allocates its own
            // buffer, so nested tries, throws after an inner try completed,
            // and throws from callee frames all reach the right handler.
            module_.addGlobalAsm("global _exception_buf_ptr");
            module_.addGlobalAsm("_exception_buf_ptr: resq 1");
            module_.addGlobalAsm("global _exception_value");
            module_.addGlobalAsm("_exception_value: resq 1");
        }
    }

private:
    ModuleIR module_;
    SymbolTable& symbolTable_;
    std::unique_ptr<FunctionIR> currentFunction_;
    std::unique_ptr<BasicBlock> currentBlock_;
    std::string currentClassName_;
    std::string currentSourcePath_;
    // CIDER-BUG-024: optional diagnostic sink (nullptr in unit tests).
    ErrorReporter* reporter_ = nullptr;
    // CID-020: resolved-path -> module scope registry (from the collector).
    std::map<std::string, Scope*> moduleScopes_;
    // CID-020/021: imported AST -> logical module name (from the collector).
    std::map<const AstProgram*, std::string> programModules_;
    // CID-020/021: defined symbol -> defining module logical (bind-time
    // provenance from the collector; "" == main).
    std::map<uint64_t, std::string> symbolModules_;
    // CID-020/021 (spec §2): logical name of the file being generated
    // ("" == main file, which keeps plain names).
    std::string currentModule_;
    IRValue lastValue_ = IRValue::Constant(0, IRType::I32);
    TypeInfo lastExprType_;
    bool isAddress_ = false;
    std::map<std::string, IRValue> variableValues_;
    std::set<std::string> paramNames_;
    std::set<std::string> assignedParams_;
    std::map<std::string, IRValue> globalValues_;
    // CIDER-BUG-048: cycle guard while folding `const` references in
    // tryEvalConstValue (balanced insert/erase around each recursion), and
    // the declaration scope that const-initializer identifiers resolve in
    // (null at top level: resolve at the use site).
    std::set<uint64_t> constEvalVisiting_;
    Scope* constEvalScope_ = nullptr;
    // CID-020/021 (spec §6): resolved class/struct/interface objects for
    // vtable emission in generate(). Objects (not names) so module-scope
    // types keep their identity; same-named types in two modules stay
    // distinct entries. Cleared per generate() alongside visitedPrograms_.
    std::vector<std::shared_ptr<StructSymbol>> classSyms_;
    std::vector<std::shared_ptr<StructSymbol>> structSyms_;
    std::vector<std::shared_ptr<InterfaceSymbol>> ifaceSyms_;
    std::set<AstProgram*> visitedPrograms_;
    std::vector<std::string> breakLabels_;
    std::vector<std::string> continueLabels_;
    // CIDER-BUG-050: for each enclosing loop, the deferStack_/destructorStack_
    // depth above which a break/continue must emit scope exits. That covers
    // the loop's per-iteration scope and every block nested inside it, while
    // leaving enclosing scopes pending until they exit normally. Parallel to
    // breakLabels_/continueLabels_.
    std::vector<size_t> unwindDepths_;
    // CIDER-BUG-053: each try scope active in the function being generated,
    // innermost last. `deferDepth` mirrors unwindDepths_ so return/break/
    // continue restore exactly the handlers they leave.
    struct ActiveTryScope {
        IRValue prevBuf;         // enclosing _exception_buf_ptr value
        size_t deferDepth = 0;   // deferStack_.size() at try entry
    };
    std::vector<ActiveTryScope> activeTryScopes_;
    // Entry block of the function being generated (CIDER-BUG-053): try
    // buffers are allocated there so a try in a loop reuses one slot per
    // frame invocation instead of allocating per iteration.
    BasicBlock* functionEntryBlock_ = nullptr;
    std::vector<std::vector<Statement*>> deferStack_;
    std::vector<std::vector<DestructorInfo>> destructorStack_;
    bool exceptionGlobalsEnsured_ = false;
    bool interpHelpersEnsured_ = false;
    bool currentIsSret_ = false;
    bool defaultAbiMs_ = false;  // CID-005: --target uefi function-value typing
    TypeInfo currentReturnType_ = {"void", 0};
    int tempCount_ = 0;
    int globalStrCount_ = 0;
    int labelCount_ = 0;
    int anonFuncCount_ = 0;
    int closureCount_ = 0;
    std::string nextTemp() { return "v" + std::to_string(tempCount_++); }
    std::string nextLabel(const std::string& prefix) {
        return (currentFunction_ ? currentFunction_->getName() + "_" : "") + prefix + std::to_string(labelCount_++);
    }
};

}  // namespace cider
