#pragma once

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <functional>
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
#include "nasm_reserved.h"
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
    // CIDER-BUG-095: accepted module-qualified template spellings
    // (`m.id<int>` -> shared bare `id<int>`), from the collector. A
    // qualified call resolves through this map when the module scope does
    // not hold the shared instantiation (mirrors the checker). Empty by
    // default: existing programs are untouched.
    void setQualifiedTemplateInstantiations(const std::map<std::string, std::string>& map) {
        qualifiedTemplateInstantiations_ = map;
    }
    // CIDER-BUG-136: per-owner struct-template instantiation identities
    // recorded for separate compilation, keyed by
    // (spelling context module, spelling) — see templateSpellingKey. A bare
    // `Box<int>` resolves to its own module's instantiation instead of the
    // first same-named one. Empty in whole-program mode (nothing changes).
    void setTemplateInstanceIdentities(const std::map<std::string, std::string>& map) {
        templateInstanceIdentities_ = map;
    }

    // CIDER-BUG-119: function/block-local aggregate declarations the
    // collector registered, keyed by their AST node (mirrors
    // TypeChecker::setLocalAggregates). Visiting the declaration defines
    // the finalized StructSymbol in the generator's live scope, so later
    // type-size and member-offset lookups resolve it.
    void setLocalAggregates(
        const std::map<const Statement*, std::shared_ptr<StructSymbol>>& map) {
        localAggregates_ = map;
    }

    // CID-005: `--target uefi` types unmarked function identifiers as
    // MS-ABI fn values (mirrors the checker's functionPointerTypeOf).
    void setDefaultAbiMs(bool v) { defaultAbiMs_ = v; }

    // CIDER-ISS-001: opt-in `--checks` mode. When on, an IndexExpr whose
    // base is a fixed-size array value emits a runtime bounds guard against
    // the array's static length. When off (the default) the generated IR is
    // unchanged, so default output stays byte-identical.
    void setChecks(bool v) { checks_ = v; }

    // CIDER-FEAT-006: separate-compilation mode. Compiler-synthesized data
    // labels (vtables, zero buffers, global string payloads) get a
    // deterministic per-object prefix so two objects linked together do not
    // collide; empty (the default) keeps whole-program output byte-identical.
    void setSeparateCompilation(bool v) { separateCompilation_ = v; }

    ModuleIR generate(AstProgram& program) {
        // CIDER-FEAT-006: token derived from this object's entry source
        // path. Deterministic (identical rebuilds stay byte-identical) and
        // distinct per compilation unit.
        separateToken_.clear();
        if (separateCompilation_) {
            uint64_t h = 1469598103934665603ULL;
            for (unsigned char c : currentSourcePath_) {
                h ^= c;
                h *= 1099511628211ULL;
            }
            char buf[32];
            std::snprintf(buf, sizeof(buf), "sep_%016llx_", (unsigned long long)h);
            separateToken_ = buf;
        }
        visitedPrograms_.clear();
        classSyms_.clear();
        structSyms_.clear();
        ifaceSyms_.clear();
        globalValues_.clear();
        pendingGlobalInits_.clear();
        entryFunctions_.clear();
        exceptionGlobalsEnsured_ = false;
        interpHelpersEnsured_ = false;
        stringEqHelpersEnsured_ = false;
        stringCmpHelpersEnsured_ = false;
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
                // CIDER-BUG-119: a local class's own entries resolve through
                // the Method record (see localMethodDef), so the vtable
                // references the same module-mangled label as the
                // definition.
                if (!methodDef.first) methodDef = localMethodDef(classSym, e);
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
                                // CIDER-BUG-119: a function/block-local
                                // implementation's symbol is rebuilt from its
                                // record (see localMethodDef), so the entry
                                // label matches the definition's mangling.
                                if (!methodDef.first) methodDef = localMethodDef(ss, sm.mangledName);
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

        // CIDER-BUG-087: run embedded class-value field initializers for
        // module-scope aggregates from a synthesized startup function (the
        // entry calls it). No-op unless such a global exists.
        emitGlobalFieldInitializers(program);

        // CIDER-BUG-137: run global destructors at program exit from a
        // synthesized fini function (every entry return calls it). Mirrors
        // the startup pass; no-op unless a global has embedded class
        // destructors.
        emitGlobalFieldFinalizers(program);

        return std::move(module_);
    }

    void startBlock(const std::string& name) {
        if (currentBlock_) {
            currentFunction_->addBlock(std::move(currentBlock_));
        }
        currentBlock_ = std::make_unique<BasicBlock>(name);
        // CIDER-FEAT-001: a block inherits the source line of the statement
        // that opened it; codegen -g turns that into a line-table row.
        currentBlock_->setSourceLine(currentSourceLine_);
    }

    // CIDER-FEAT-001 (compile -g): source positions are tracked at
    // statement granularity. `visitStatement` records which blocks and
    // instructions exist before the statement runs and stamps the new
    // instructions with the statement's line afterwards. Nested statements
    // stamp first; the outer pass only fills in still-unknown (0) lines, so
    // the innermost statement wins. Instructions synthesized later by
    // optimizer passes keep line 0 and emit no row.
    struct StatementMark {
        FunctionIR* func = nullptr;
        size_t blockCount = 0;
        BasicBlock* block = nullptr;
        size_t instrCount = 0;
    };

    StatementMark beginStatementMark() {
        StatementMark m;
        m.func = currentFunction_.get();
        if (m.func) m.blockCount = m.func->getBlocks().size();
        m.block = currentBlock_.get();
        if (m.block) m.instrCount = m.block->getInstructions().size();
        return m;
    }

    void endStatementMark(const StatementMark& m, int line) {
        if (!m.func || line <= 0) return;
        auto stamp = [&](BasicBlock* b, size_t from) {
            if (!b) return;
            auto& ins = b->getInstructions();
            for (size_t i = from; i < ins.size(); ++i) {
                if (ins[i].sourceLine == 0) ins[i].sourceLine = line;
            }
        };
        // Blocks appended while the statement ran (branch/loop bodies and
        // merges) plus anything appended to the block that was current.
        auto& blocks = m.func->getBlocks();
        for (size_t i = m.blockCount; i < blocks.size(); ++i) {
            stamp(blocks[i].get(), 0);
        }
        stamp(m.block, m.instrCount);
    }

    void visitStatement(Statement* s) {
        if (!s) return;
        StatementMark m = beginStatementMark();
        int prevLine = currentSourceLine_;
        currentSourceLine_ = s->getLine();
        s->accept(*this);
        endStatementMark(m, s->getLine());
        currentSourceLine_ = prevLine;
    }

    struct DestructorInfo {
        std::string mangledName;
        IRValue varAddr;
        // CIDER-BUG-088: byte offset of the object within `varAddr`. Direct
        // class locals register offset 0 (byte-identical IR); an embedded
        // class-value subobject registers its accumulated field/element
        // offset and gets its address computed at scope exit.
        int offset = 0;
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
                visitStatement(s);
            }
        }
    }

    void emitDefers() { emitDefers(0); }

    void emitDestructors(size_t firstScope) {
        for (size_t i = destructorStack_.size(); i > firstScope; --i) {
            for (size_t j = destructorStack_[i - 1].size(); j > 0; --j) {
                const DestructorInfo& d = destructorStack_[i - 1][j - 1];
                // CIDER-BUG-088: an embedded subobject's address is computed
                // here (lazily) instead of at declaration, so no pointer for
                // it stays live through the enclosing scope. The direct
                // class-local case keeps its existing address (offset 0).
                IRValue addr = d.varAddr;
                if (d.offset != 0) {
                    addr = IRValue::Register(nextTemp(), IRType::PTR);
                    currentBlock_->addInstruction(
                        {IROp::ADD, addr,
                         {d.varAddr, IRValue::Constant((int64_t)d.offset, IRType::I32)}});
                }
                IRInstruction call;
                call.op = IROp::CALL;
                call.dest = std::nullopt;
                call.args.push_back(IRValue::Label(d.mangledName));
                call.args.push_back(addr);
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
        // CIDER-FEAT-002: an enum lowers as its underlying integer
        // (int-sized); bare, dotted and canonical spellings all resolve.
        auto typeSym = resolveNominalType(name);
        if (typeSym && typeSym->kind == SymbolKind::ENUM) return IRType::I32;
        if (typeSym && typeSym->type.pointerLevel == 0 && !typeSym->type.name.empty()) {
            std::string targetName = typeSym->type.name;
            if (targetName != name) {
                // CID-020/021/CIDER-BUG-029 rule: an alias target spelling
                // belongs to the alias's defining module. Re-root it before
                // the recursive lookup so a cross-module `type X = Y` maps
                // Y's real width (an enum target stays I32) instead of
                // falling through to PTR.
                TypeInfo target;
                target.name = targetName;
                std::string rooted =
                    canonicalizeNominalInModule(target, definingModuleFor(typeSym, name)).name;
                IRType resolved = mapTypeImpl(rooted, visiting);
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
        if (!sym && !defModule.empty()) sym = resolveNominalInModule(t.name, defModule);
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
        // CIDER-BUG-061: `volatile` written at the use site (`var x: volatile
        // V` with `type V = int`) must survive alias resolution exactly like
        // the qualifier on the alias target itself.
        resolved.isVolatile = resolved.isVolatile || t.isVolatile;
        // CIDER-BUG-068: the same rule for an inferred closure
        // representation on an alias-spelled parameter (`type Cb = fn(...)`;
        // the checker marks the spelling, the alias target is plain fn).
        resolved.isClosure = resolved.isClosure || t.isClosure;
        // CIDER-BUG-078: and for an interface flag resolved on the alias
        // spelling (`type S = Shape`): the fat-pointer representation
        // belongs to the target, so losing it here would treat the value as
        // a scalar at every resolved read/store site.
        resolved.isInterface = resolved.isInterface || t.isInterface;
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
        if (!sym && !defModule.empty()) sym = resolveNominalInModule(resolved.name, defModule);
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
            return emitFunctionName(definingModuleFor(sym, id->getName()), sym->name,
                                    sym->isExtern);
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
                                                    target->name, target->isExtern);
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
        if (!sym && !defModule.empty()) sym = resolveNominalInModule(resolved.name, defModule);
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
    // CIDER-BUG-174: an ARRAY of fn values retains the element's
    // isFunctionPointer flag (TypeInfo::fromAST copies the element signature
    // onto the array wrapper so indexing recovers it), so the scalar-fn
    // rejection must exclude arrays. Without `!t.isArray` such an array was
    // a "scalar fn" here: callers passed its address but the callee stored
    // that pointer as the first element instead of memcpying the array.
    bool isByValueAggregate(const TypeInfo& t, const std::string& defModule = "") {
        if (t.pointerLevel > 0 || t.isSlice || t.isInterface ||
            (t.isFunctionPointer && !t.isArray)) {
            return false;
        }
        TypeInfo resolved = resolveTypeAlias(t, defModule);
        if (resolved.pointerLevel > 0 || resolved.isSlice || resolved.isInterface ||
            (resolved.isFunctionPointer && !resolved.isArray)) {
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
            visitStatement(stmt.get());
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
            // CIDER-FEAT-004: pointer type arguments put '*' into mangled
            // names (`id<Node*>`); keep it out of assembler symbols.
            else if (c == '*') result += "_p_";
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
    // CIDER-BUG-072: main-file definitions additionally escape names that
    // collide with a NASM reserved word (see nasm_reserved.h); `extern`
    // references pass isExtern and keep the linker's spelling untouched.
    static bool isNeverMangled(const std::string& bare) {
        return bare == "main" || bare == "_start";
    }

    static std::string emitFunctionName(const std::string& defModule, const std::string& bare,
                                        bool isExtern = false) {
        if (isExtern) return bare;
        if (defModule.empty() || isNeverMangled(bare)) return escapeNasmReservedWord(bare);
        return sanitizeAsmName(mangleQualifiedName(defModule, bare));
    }

    // CID-020/021 (spec §5): global IR names mangle by DEFINING module,
    // same rule as functions (T6 probe G1: same-named globals in two
    // modules shared one bare label and qualified reads emitted garbage).
    // Main-file globals keep legacy bare names, which keeps import-free IR
    // byte-identical. `main`/`_start` are never mangled (spec Review Focus);
    // `extern` references resolve through definingModuleFor (bare) and never
    // reach emission (early return at the emission site).
    // CIDER-BUG-072: main-file definitions escape NASM reserved words (the
    // module-mangled form always starts with `mod_`, so it cannot collide);
    // `extern` references keep the linker's spelling untouched.
    static std::string emitGlobalName(const std::string& defModule, const std::string& bare,
                                      bool isExtern = false) {
        if (isExtern) return bare;
        if (defModule.empty() || isNeverMangled(bare)) return escapeNasmReservedWord(bare);
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

    // CIDER-BUG-102: resolve a signature/field/target spelling as written in
    // `defModule` (bare names keep the module-scope fallback above; a
    // qualified spelling re-roots its qualifier there first), so a callee's
    // `lib.Shape` resolves even when the use site does not import lib. Null
    // when the spelling does not resolve in that module.
    std::shared_ptr<Symbol> resolveNominalInModule(const std::string& name,
                                                   const std::string& defModule) {
        if (name.empty() || defModule.empty()) return nullptr;
        std::string rooted = name;
        if (name.find("::") == std::string::npos && name.find('<') == std::string::npos &&
            name.find('.') != std::string::npos) {
            TypeInfo t;
            t.name = name;
            rooted = canonicalizeNominalInModule(t, defModule).name;
            if (rooted == name) return nullptr;  // qualifier not visible there
        }
        if (rooted.find("::") != std::string::npos) return resolveNominalType(rooted);
        return resolveCalleeNominal(rooted, defModule);
    }

    // Mangled global key for a resolved symbol: bind-time provenance first
    // (from-aliases share the target object, so the origin module wins),
    // then the scope-identity search, then the same-module rule — the same
    // definingModuleFor chain as function calls.
    std::string globalRefName(const std::shared_ptr<Symbol>& sym, const std::string& name) {
        if (!sym) return name;
        return emitGlobalName(definingModuleFor(sym, name), name, sym->isExtern);
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

    // CIDER-BUG-102: resolve a name as a spelling written in `logical`'s
    // module sees it (that module's scope, then root — never the use
    // site's). Mirrors the checker's resolveScopedInModule; "" keeps the
    // legacy current-scope walk so main-file spellings are unchanged.
    std::shared_ptr<Symbol> resolveInModule(const std::string& name, const std::string& logical) {
        if (logical.empty()) return symbolTable_.resolve(name);
        Scope* ms = scopeForModule(logical);
        if (!ms) return symbolTable_.resolve(name);
        if (auto sym = ms->resolveLocal(name)) return sym;
        Scope* root = symbolTable_.getRootScope();
        if (root && root != ms) return root->resolveLocal(name);
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
        // CIDER-BUG-136: separate compilation gives a spelling a per-owner
        // instantiation identity; re-root the lookup through it before the
        // legacy paths (mirrors the checker's resolveScopedRaw). The context
        // is the module being generated.
        if (name.find('<') != std::string::npos && name.find("::") == std::string::npos) {
            std::string ident = templateInstanceIdentity(name, currentModule_);
            if (!ident.empty() && ident != name) return resolveNominalType(ident);
        }
        // CIDER-BUG-073: a module-qualified template instantiation spelling
        // (`m.Box<int>`) resolves to the instantiation registered under the
        // bare spelling. In separate mode that is the OWNER's scope
        // (CIDER-BUG-136); whole-program keeps the legacy root-scope lookup.
        // The dot must precede the '<': dots inside argument lists
        // (`Box<m.Thing>`) are not qualifiers.
        auto lt = name.find('<');
        if (lt != std::string::npos) {
            auto canonSep = name.find("::");
            if (canonSep != std::string::npos && canonSep < lt) {
                // CIDER-BUG-136 canonical identity (`a::Box<int>`): the bare
                // part lives in the owner module's scope.
                std::string logical = name.substr(0, canonSep);
                std::string bare = name.substr(canonSep + 2);
                for (const auto& [path, scope] : moduleScopes_) {
                    if (scope && logicalForScope(scope) == logical) {
                        auto mem = scope->resolveLocal(bare);
                        if (mem) return mem;
                    }
                }
                return nullptr;
            }
            auto dot = name.rfind('.', lt);
            if (dot == std::string::npos) return nullptr;
            auto modSym = symbolTable_.resolve(name.substr(0, dot));
            if (!modSym || modSym->kind != SymbolKind::MODULE) return nullptr;
            // Owner module scope first (per-owner instantiation, separate
            // compilation), then root (legacy shared instantiation).
            Scope* ms = std::static_pointer_cast<ModuleSymbol>(modSym)->scope;
            if (ms) {
                auto owned = ms->resolveLocal(name.substr(dot + 1));
                if (owned && (owned->kind == SymbolKind::STRUCT || owned->kind == SymbolKind::CLASS))
                    return owned;
            }
            Scope* root = symbolTable_.getRootScope();
            if (!root) return nullptr;
            auto inst = root->resolveLocal(name.substr(dot + 1));
            if (inst && (inst->kind == SymbolKind::STRUCT || inst->kind == SymbolKind::CLASS))
                return inst;
            return nullptr;
        }
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

    // CIDER-BUG-043/029: a nominal spelling belongs to the module where it
    // was written (a field's type in the struct's module, an alias target
    // in the alias's module). Re-root a bare nominal in that module so
    // downstream lookups resolve without an explicit import — e.g. member
    // access on a transitively reached pointee type, or a cross-module
    // `type X = othermod.S`. Mirrors the checker's canonicalizeInModule;
    // main-module types keep bare names (canonical == bare there), so
    // import-free IR stays identical.
    // CIDER-BUG-102: a DOTTED spelling gets the same treatment — its
    // qualifier is re-rooted in the spelling's module (`lib.Shape` written
    // in module mid becomes `lib::Shape`), so a use site that does not
    // import lib still resolves the alias target or signature nominal.
    TypeInfo canonicalizeNominalInModule(TypeInfo t, const std::string& ownerMod) {
        if (t.name.empty() || t.name.find("::") != std::string::npos) return t;
        // CIDER-BUG-136: a template instantiation spelling re-roots through
        // the per-owner identity recorded for this spelling context
        // (separate compilation). Whole-program records nothing and returns
        // the spelling untouched, exactly as before.
        if (t.name.find('<') != std::string::npos) {
            std::string ident = templateInstanceIdentity(t.name, ownerMod);
            if (!ident.empty()) t.name = ident;
            return t;
        }
        if (t.name.find('.') != std::string::npos) {
            auto dot = t.name.rfind('.');
            if (dot == 0 || dot + 1 >= t.name.size()) return t;
            auto modSym = resolveInModule(t.name.substr(0, dot), ownerMod);
            if (modSym && modSym->kind == SymbolKind::MODULE) {
                auto ms = std::static_pointer_cast<ModuleSymbol>(modSym);
                t.name = canonicalTypeName(t.name.substr(dot + 1), ms->logicalName);
            }
            return t;
        }
        if (ownerMod.empty()) return t;
        auto resolved = resolveNominalType(ownerMod + "::" + t.name);
        if (resolved && (resolved->kind == SymbolKind::STRUCT || resolved->kind == SymbolKind::CLASS ||
                         resolved->kind == SymbolKind::ENUM || resolved->kind == SymbolKind::INTERFACE)) {
            t.name = ownerMod + "::" + t.name;
        }
        return t;
    }

    // CIDER-BUG-090: module a field's type spelling belongs to ("" == main).
    // An inherited field keeps its declaring class's module, which the
    // derived class's module may not see; field type resolution must use
    // this, not the accessed class's module. `ownerMod` covers symbols
    // built outside the collector (unit tests), where the field's module is
    // the aggregate's.
    static std::string fieldTypeModule(const StructSymbol::Field& f, const std::string& ownerMod) {
        return f.typeModule.empty() ? ownerMod : f.typeModule;
    }

    // CIDER-BUG-090: module a method's signature spellings belong to; an
    // inherited method keeps its declaring class's module. `ownerMod`
    // covers symbols built outside the collector (unit tests).
    static std::string methodModule(const StructSymbol::Method& m, const std::string& ownerMod) {
        return m.declaringModule.empty() ? ownerMod : m.declaringModule;
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
        if (t.name.find('<') != std::string::npos) return t;
        std::string defMod = definingModuleFor(sym, name);
        // CIDER-BUG-102: a qualified spelling belongs to the symbol's
        // defining module, so re-root its qualifier there (same rule as
        // canonicalizeNominalInModule).
        if (t.name.find('.') != std::string::npos) return canonicalizeNominalInModule(t, defMod);
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

    // CIDER-BUG-119: FunctionSymbol for a method of a function/block-local
    // class. The collector defined it in its transient local scope, so it is
    // unreachable after collection; the Method record still carries the full
    // signature. Rebuilding it keeps the call convention (return type, sret,
    // ABI) decisions identical to module-scope methods.
    std::shared_ptr<FunctionSymbol> localMethodSymbol(
        const std::shared_ptr<StructSymbol>& classSym, const StructSymbol::Method& m) {
        auto fs = std::make_shared<FunctionSymbol>();
        fs->kind = SymbolKind::FUNCTION;
        fs->name = m.mangledName;
        fs->returnType = m.returnType;
        fs->parameterTypes = m.parameterTypes;
        fs->parameterTypes.insert(fs->parameterTypes.begin(), {classSym->name, 1});
        fs->abi = (m.isVirtual || m.isOverride) ? Abi::SysV : Abi::Default;
        return fs;
    }

    // CIDER-BUG-119: (FunctionSymbol, defining module) for a method of a
    // function/block-local class, by collector-composed mangled name. The
    // collector defined the symbol in its transient local scope, so
    // resolveFunctionDef misses it; rebuilding from the Method record (see
    // localMethodSymbol) and attributing it to the class's defining module
    // makes vtable entries and call sites compose the same label as the
    // method definition. Null for an unknown name (module classes and
    // inherited module methods resolve through resolveFunctionDef first).
    std::pair<std::shared_ptr<FunctionSymbol>, std::string> localMethodDef(
        const std::shared_ptr<StructSymbol>& classSym, const std::string& mangledName) {
        if (!classSym) return {nullptr, ""};
        for (const auto& m : classSym->methods) {
            if (m.mangledName != mangledName) continue;
            return {localMethodSymbol(classSym, m),
                    methodModule(m, definingModuleFor(classSym, classSym->name))};
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
        // CIDER-BUG-119: a function/block-local class keys its vtable under
        // the unique mangling prefix, so same-named local classes in
        // different functions cannot collide.
        std::string base = classSym->manglePrefix.empty() ? classSym->name : classSym->manglePrefix;
        // CIDER-FEAT-006: per-object prefix in separate compilation (the
        // vtable is emitted by each object that uses the class; references
        // are intra-object, so distinct names link cleanly).
        return separateToken_ + emitGlobalName(definingModuleFor(classSym, classSym->name), base);
    }

    // CID-020/021 (spec §6): vtable-map key joining an interface and its
    // implementing type, each part mangled by its own defining module.
    // Bare legacy "Iface_Class" when both are main-file.
    std::string vtableIfaceKey(const std::shared_ptr<Symbol>& ifaceSym, const std::string& ifaceName,
                               const std::shared_ptr<Symbol>& scSym, const std::string& scName) {
        std::string im = ifaceSym ? definingModuleFor(ifaceSym, ifaceSym->name) : "";
        std::string sm = scSym ? definingModuleFor(scSym, scSym->name) : "";
        // CIDER-BUG-119: a function/block-local implementing class keys
        // under its unique mangling prefix (mirrors vtableKeyForClass), so
        // same-named local classes in different functions never share a
        // vtable.
        std::string scBase = scName;
        if (scSym && (scSym->kind == SymbolKind::STRUCT || scSym->kind == SymbolKind::CLASS)) {
            auto ss = std::static_pointer_cast<StructSymbol>(scSym);
            if (!ss->manglePrefix.empty()) scBase = ss->manglePrefix;
        }
        return separateToken_ + emitGlobalName(im, ifaceName) + "_" + emitGlobalName(sm, scBase);
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
        // CIDER-BUG-078: an alias spelling (`type S = Shape`, `type T = Sq`)
        // names the same definition; resolve the chain so the label matches
        // the vtable generate() emits for the direct spelling.
        TypeInfo ifaceT;
        ifaceT.name = ifaceName;
        TypeInfo ifaceResolved = resolveTypeAlias(ifaceT);
        auto ifaceSym = resolveNominalType(ifaceResolved.name);
        if (!ifaceSym || ifaceSym->kind != SymbolKind::INTERFACE) {
            return "vtable_" + separateToken_ + ifaceName + "_" + concreteName;
        }
        TypeInfo concreteT;
        concreteT.name = concreteName;
        TypeInfo concreteResolved = resolveTypeAlias(concreteT);
        auto concSym = resolveNominalType(concreteResolved.name);
        if (!concSym) concSym = resolveUniqueModuleNominal(concreteResolved.name);
        if (!concSym || (concSym->kind != SymbolKind::STRUCT && concSym->kind != SymbolKind::CLASS)) {
            return "vtable_" + separateToken_ + ifaceName + "_" + concreteName;
        }
        return "vtable_" + vtableIfaceKey(ifaceSym, ifaceSym->name, concSym, concSym->name);
    }

    // CIDER-BUG-054: materialise an interface (fat pointer) value at
    // `addr` — the data pointer at +0, the interface/concrete vtable
    // pointer at +8. A concrete value contributes the object's address, a
    // concrete pointer its pointer value; an interface source copies both
    // words (the checker admits only the same interface, whose method
    // order the stored vtable follows). Shared by declaration-time
    // interface initialization and reassignment so the two lower
    // identically on every backend.
    // CIDER-BUG-115: because a concrete value contributes its ADDRESS
    // (aliasing, like a C++ reference), a by-value source that dies with
    // the current frame leaves the stored data pointer dangling once the
    // frame returns. The checker reports the statically detectable shapes
    // under --warn (warnDanglingInterfaceStore) and the VM makes frame
    // reuse observable (VM::retireFrame), matching native's next call
    // overwriting the dead stack slots.
    // CIDER-BUG-079: `isVolatile` is the target's volatile flag, sourced
    // exactly like the scalar/aggregate store paths source it (a volatile
    // declared type, field or pointee). Both fat-pointer words are then
    // observable writes under CIDER-BUG-061.
    void emitInterfaceStore(const TypeInfo& ifaceType, IRValue addr, Expression* value,
                            bool isVolatile = false) {
        value->accept(*this);
        emitInterfaceStoreAt(ifaceType, addr, lastValue_, lastExprType_, isVolatile);
        lastExprType_ = ifaceType;
    }

    // CIDER-BUG-077: the store half of emitInterfaceStore, for callers
    // that already evaluated the source (the hidden-sret return slot).
    // `value` is the source value and `initType` its type.
    // CIDER-BUG-079: `isVolatile` marks the two target stores volatile.
    // CIDER-BUG-101: the SOURCE carries its own flag — copying FROM a
    // volatile interface object reads both fat-pointer words through
    // volatile loads (CIDER-BUG-061 model), independent of the target.
    void emitInterfaceStoreAt(const TypeInfo& ifaceType, IRValue addr, IRValue value,
                              TypeInfo initType, bool isVolatile = false) {
        // The interface flag can be missing on spellings that predate
        // declaration lowering (e.g. a module global read through a
        // from-alias): resolve the name before classifying; CIDER-BUG-078:
        // an alias spelling resolves through its chain.
        initType = resolveInterfaceNominal(initType);
        const bool sourceIsVolatile = initType.isVolatile;
        // A global target is a label: materialise its address so the two
        // stores honour the +0/+8 offsets (label stores are sized by the
        // global's declared type and ignore the offset).
        if (addr.kind == IRValue::LABEL) {
            IRValue addrReg = IRValue::Register(nextTemp(), IRType::PTR);
            currentBlock_->addInstruction({IROp::MOV, addrReg, {addr}});
            addr = addrReg;
        }
        if (initType.isInterface) {
            // Fat-pointer copy: both words, no vtable relabelling.
            IRValue dataPtr = IRValue::Register(nextTemp(), IRType::PTR);
            currentBlock_->addInstruction({IROp::LOAD, dataPtr, {value, IRValue::Constant(0, IRType::I32)}, sourceIsVolatile});
            IRValue vtablePtr = IRValue::Register(nextTemp(), IRType::PTR);
            currentBlock_->addInstruction({IROp::LOAD, vtablePtr, {value, IRValue::Constant(8, IRType::I32)}, sourceIsVolatile});
            currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, dataPtr, IRValue::Constant(0, IRType::I32)}, isVolatile});
            IRValue addr8 = IRValue::Register(nextTemp(), IRType::PTR);
            currentBlock_->addInstruction({IROp::ADD, addr8, {addr, IRValue::Constant(8, IRType::I32)}});
            currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr8, vtablePtr, IRValue::Constant(0, IRType::I32)}, isVolatile});
        } else {
            currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, value, IRValue::Constant(0, IRType::I32)}, isVolatile});
            IRValue addr8 = IRValue::Register(nextTemp(), IRType::PTR);
            currentBlock_->addInstruction({IROp::ADD, addr8, {addr, IRValue::Constant(8, IRType::I32)}});
            std::string concreteTypeName = initType.name;
            if (initType.pointerLevel > 0) {
                TypeInfo base = initType;
                base.pointerLevel--;
                concreteTypeName = base.name;
            }
            std::string vtableLabel = interfaceVtableLabel(ifaceType.name, concreteTypeName);
            currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr8, IRValue::Label(vtableLabel), IRValue::Constant(0, IRType::I32)}, isVolatile});
        }
    }

    // Name for ir_gen-inserted runtime calls (`new`, `delete`, closures,
    // the interpolation helpers): the visible malloc/free definition's
    // mangled name, so the hardened std allocator — not the VM builtin /
    // libc — serves programs that import it. Bare when nothing is in
    // scope (legacy VM-builtin/libc fallback).
    std::string runtimeHelperName(const std::string& bare) {
        auto sym = symbolTable_.resolve(bare);
        if (sym && sym->kind == SymbolKind::FUNCTION)
            return emitFunctionName(definingModuleFor(sym, bare), bare, sym->isExtern);
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
        // CIDER-BUG-095: a qualified function-template call (`m.id<int>(x)`)
        // uses the shared bare instantiation when the module scope does not
        // hold it (mirrors the checker's accepted-spelling fallback).
        if (!target) {
            auto qti = qualifiedTemplateInstantiations_.find(
                base->getName() + "." + mem->getMember());
            if (qti != qualifiedTemplateInstantiations_.end())
                target = symbolTable_.resolve(qti->second);
        }
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
        funcName = emitFunctionName(defMod, sanitizeAsmName(mem->getMember()), target->isExtern);
        return true;
    }

    void visit(FunctionDeclStmt& stmt) override {
        // Skip template function declarations (uninstantiated)
        if (stmt.hasTemplateParams()) return;

        std::string funcName = sanitizeAsmName(stmt.getName());
        if (!currentClassName_.empty()) {
            // CIDER-BUG-119: a function/block-local class carries a unique
            // mangling prefix (the collector assigned it before composing
            // the Method records); resolve the live class symbol so the
            // definition matches `m.mangledName`. Module-scope classes have
            // an empty prefix and keep the legacy `Class_method` name.
            std::string classBase = currentClassName_;
            auto classSym = symbolTable_.resolve(currentClassName_);
            if (classSym && classSym->kind == SymbolKind::CLASS) {
                auto cs = std::static_pointer_cast<StructSymbol>(classSym);
                if (!cs->manglePrefix.empty()) classBase = cs->manglePrefix;
            }
            funcName = classBase + "_" + funcName;
        }

        if (stmt.isExtern()) {
            // `extern` names are never mangled (spec Review Focus): they
            // must match the linker-provided symbol.
            module_.addExternFunction(funcName);
            return;
        }
        // CIDER-FEAT-006: a declaration loaded from a `.cider-meta` snapshot
        // has no body here — the defining module's object carries it. Emit
        // the mangled name as an extern reference.
        if (stmt.isMetaDecl()) {
            module_.addExternFunction(emitFunctionName(currentModule_, funcName));
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
        // CIDER-BUG-077: an interface-typed return is a 16-byte fat pointer
        // and rides the hidden-sret convention, exactly like a by-value
        // struct/array return. Resolve the flag here (the definition AST
        // carries the bare spelling) so the prologue and return sites agree.
        funcRetType = resolveInterfaceNominal(resolveTypeAlias(funcRetType));
        // Virtual methods stay on the legacy malloc path so VIRTUAL_CALL
        // callers (which don't pass sret) keep matching the callee.
        bool isVirtMethod = stmt.isVirtual() || stmt.isOverride();
        bool funcIsSret = (isByValueAggregate(funcRetType) || funcRetType.isInterface) && !isVirtMethod;
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
        // CIDER-FEAT-006: a template instantiation cloned for a use site in
        // separate-compilation mode carries the use-site file so per-file
        // codegen emits it locally; every other definition follows the
        // enclosing program's source path.
        const std::string& emitPath = stmt.getEmitSourcePath();
        currentFunction_->setSourcePath(emitPath.empty() ? currentSourcePath_ : emitPath);
        // A use-site instantiation may be emitted by every object that uses
        // it (Phase 1 accepts duplicates instead of COMDAT dedup), so the
        // definition is weak: the linker keeps one shared copy.
        if (separateCompilation_ && !emitPath.empty()) currentFunction_->setWeak(true);
        currentFunction_->setSourceLine(stmt.getLine());
        // CIDER-BUG-087: a top-level entry function runs the synthesized
        // global field-initializer pass before any user code (see
        // emitGlobalFieldInitializers). Nested functions are not entries.
        if (!prevFunction && (stmt.getName() == "main" || stmt.getName() == "_start" ||
                              stmt.getName() == "efi_main")) {
            entryFunctions_.push_back(currentFunction_.get());
        }
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
            // CIDER-BUG-054: an interface-typed parameter is the same
            // 16-byte fat pointer as a local interface variable. Resolve
            // the flag here as visit(VarDeclStmt) does, so the alloca has
            // the right size and dispatch/assignment see the interface.
            // CIDER-BUG-070: resolve through the nominal lookup so a
            // module-qualified spelling (`lib.Shape`) is seen too.
            sym->type = resolveInterfaceNominal(sym->type);
            // CIDER-BUG-101: a volatile parameter's materialisation copy is
            // the same observable-access family as a volatile declaration
            // (CIDER-BUG-061): the local slot's two stores (and, for an
            // interface, the source words they read) are marked volatile.
            const bool paramIsVolatile = resolveTypeAlias(sym->type).isVolatile;
            symbolTable_.define(sym);

            IRType paramIRType = (sym->type.pointerLevel > 0) ? IRType::PTR : mapType(sym->type.name);
            std::string paramRegName = "arg_" + param.first;
            currentFunction_->addParam(paramRegName, paramIRType);
            IRValue paramReg = IRValue::Register(paramRegName, paramIRType);
            IRValue addr = IRValue::Register(nextTemp(), IRType::PTR);
            currentBlock_->addInstruction({IROp::ALLOCA, addr, {IRValue::Constant(getTypeSize(sym->type), IRType::I32)}});
            if (sym->type.isInterface) {
                // Interface values are address-like: callers pass the
                // address of their fat pointer. Copy the two words into the
                // parameter's own slot so method dispatch and reassignment
                // use it directly, exactly like a declared local.
                IRValue dataPtr = IRValue::Register(nextTemp(), IRType::PTR);
                currentBlock_->addInstruction({IROp::LOAD, dataPtr, {paramReg, IRValue::Constant(0, IRType::I32)}, paramIsVolatile});
                IRValue vtablePtr = IRValue::Register(nextTemp(), IRType::PTR);
                currentBlock_->addInstruction({IROp::LOAD, vtablePtr, {paramReg, IRValue::Constant(8, IRType::I32)}, paramIsVolatile});
                currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, dataPtr, IRValue::Constant(0, IRType::I32)}, paramIsVolatile});
                IRValue addr8 = IRValue::Register(nextTemp(), IRType::PTR);
                currentBlock_->addInstruction({IROp::ADD, addr8, {addr, IRValue::Constant(8, IRType::I32)}});
                currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr8, vtablePtr, IRValue::Constant(0, IRType::I32)}, paramIsVolatile});
            } else if (sym->type.pointerLevel == 0 && isByValueAggregate(sym->type)) {
                // By-value aggregate params (struct/class/fixed-size array)
                // arrive as a caller pointer: copy the pointee into the
                // local slot instead of storing the pointer value as the
                // first field (CID-022; CIDER-BUG-045 for arrays).
                currentBlock_->addInstruction({IROp::MEMCPY, std::nullopt, {addr, paramReg, IRValue::Constant(getTypeSize(sym->type), IRType::I32)},
                                               paramIsVolatile});
                // CIDER-BUG-089: the parameter is a declared class value, so
                // re-stamp its class's vptr after the copy (`v: V` receiving
                // `*p` from a dynamically-typed pointer slices to V).
                // CIDER-BUG-133: a volatile parameter's re-stamp is an
                // observable write, marked like the copy above.
                emitClassVptrStoreForType(sym->type, addr, currentModule_, paramIsVolatile);
            } else {
                currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, paramReg, IRValue::Constant(0, IRType::I32)},
                                               paramIsVolatile});
            }
            variableValues_[param.first] = addr;
            variableValues_[paramRegName] = addr;  // Support both for lookup convenience
            paramNames_.insert(param.first);
            // CIDER-BUG-137: a by-value aggregate parameter is the callee's
            // OWN copy — callers pass their source address and the prologue
            // memcpys it into this slot. It therefore finalizes exactly like
            // a declared local: at function-scope exit, after every later-
            // registered local (LIFO), with the CIDER-BUG-088 order (own
            // destructor first, embedded values in reverse construction
            // order). Params register in declaration order, so the reverse
            // walk destroys the last parameter first. Scalars, pointers,
            // slices and interfaces carry no embedded class destructors and
            // register nothing.
            registerTempDestructors(sym->type, addr, currentModule_);
        }
        // Pre-pass: params assigned anywhere in the body must not read-through.
        assignedParams_.clear();
        for (auto& s : stmt.getBody()) collectAssignedParams(s.get());

        for (auto& s : stmt.getBody()) {
            visitStatement(s.get());
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

            // CIDER-BUG-077: interface-typed return. The hidden-sret slot
            // receives the 16-byte fat pointer — a copied data/vtable pair
            // for an interface source, or a pair built from a satisfying
            // concrete value/single pointer. No scalar conversion applies:
            // the checker admits only interface or struct/class sources.
            if (currentIsSret_ && currentReturnType_.isInterface) {
                TypeInfo valIface = resolveInterfaceNominal(retType);
                if (!valIface.isInterface && !valIface.isArray && valIface.pointerLevel == 0 &&
                    isStructValueType(valIface)) {
                    // A concrete struct/class VALUE is boxed: the fat
                    // pointer's data word must not address the callee's
                    // frame, which dies on return. Copy the object into a
                    // fresh heap slot (the same allocation `new` uses) so
                    // the caller's interface stays valid; a concrete pointer
                    // or interface source passes its address through.
                    int objSize = getTypeSize(valIface);
                    IRValue box = IRValue::Register(nextTemp(), IRType::PTR);
                    IRInstruction alloc;
                    alloc.op = IROp::CALL;
                    alloc.dest = box;
                    alloc.args.push_back(IRValue::Label(runtimeHelperName("malloc")));
                    alloc.args.push_back(IRValue::Constant(objSize, IRType::I32));
                    currentBlock_->addInstruction(alloc);
                    // CIDER-BUG-133: the box copy reads the (possibly
                    // volatile) source VALUE, so its volatility marks the
                    // MEMCPY; the heap destination is a compiler temp.
                    currentBlock_->addInstruction({IROp::MEMCPY, std::nullopt,
                                                   {box, val, IRValue::Constant(objSize, IRType::I32)},
                                                   valIface.isVolatile});
                    val = box;
                }
                IRValue sretAddr = variableValues_.count("__sret")
                    ? variableValues_["__sret"]
                    : IRValue::Register("__sret", IRType::PTR);
                IRValue sretPtr = IRValue::Register(nextTemp(), IRType::PTR);
                currentBlock_->addInstruction({IROp::LOAD, sretPtr, {sretAddr, IRValue::Constant(0, IRType::I32)}});
                // CIDER-BUG-133: a volatile interface return type marks the
                // two fat-pointer target stores (the source's own flag is
                // handled inside via retType).
                emitInterfaceStoreAt(currentReturnType_, sretPtr, val, retType,
                                     currentReturnType_.isVolatile);
                lastExprType_ = currentReturnType_;
                emitDefers();
                emitDestructors();
                emitExceptionUnwind(0);
                currentBlock_->addInstruction({IROp::RET, std::nullopt, {}});
                startBlock(nextLabel("dead"));
                return;
            }

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
                // CIDER-BUG-133: an aggregate return copy is observable
                // when either end is volatile — the declared return type or
                // the returned VALUE (including volatility inferred from a
                // volatile enclosing object), mirroring the declaration and
                // assignment copies.
                currentBlock_->addInstruction({IROp::MEMCPY, std::nullopt, {sretPtr, val, IRValue::Constant(structSize, IRType::I32)},
                                               retType.isVolatile || currentReturnType_.isVolatile});
                // CIDER-BUG-089: the returned value's declared class owns
                // the vptr word, so `return *p;` slices to the return type.
                // CIDER-BUG-133: a volatile return target marks the stamp.
                emitClassVptrStoreForType(currentReturnType_, sretPtr, currentModule_,
                                          currentReturnType_.isVolatile);
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
                // CIDER-BUG-133: same source/target rule as the sret copy.
                currentBlock_->addInstruction({IROp::MEMCPY, std::nullopt, {localPtr, val, IRValue::Constant(structSize, IRType::I32)},
                                               retType.isVolatile || currentReturnType_.isVolatile});
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
                                            sanitizeAsmName(lookupName), sym->isExtern);
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

            // CIDER-BUG-070: recover a missing interface flag from the
            // nominal spelling before choosing the dispatch path, so a
            // module-qualified interface value (`sh: lib.Shape`) dispatches
            // through its vtable exactly like an unqualified one.
            objType = resolveInterfaceNominal(objType);

            // Check if object is an interface type -> interface method call
            if (objType.isInterface) {
                // CIDER-BUG-101: reads of the interface object are observable
                // when the object — or, for a pointer-to-interface, the
                // pointee — is volatile. This is the read twin of the
                // CIDER-BUG-079 store rule: both fat-pointer words are
                // marked, so `(*p).area()` through a volatile pointee is
                // never elided or reordered by LICM/GVN/DCE.
                const bool objIsVolatile = objType.isVolatile;
                // CIDER-BUG-078: the isInterface flag may come from an alias
                // spelling (`type S = Shape`); resolve the chain before the
                // symbol lookup so the vtable index comes from the target
                // interface, exactly like the direct spelling.
                auto ifaceSym = resolveNominalType(resolveTypeAlias(objType).name);
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
                            currentBlock_->addInstruction({IROp::LOAD, dataPtrReg, {objPtr, IRValue::Constant(0, IRType::I32)}, objIsVolatile});
                            // Load vtable_ptr from [objPtr + 8]
                            IRValue vtablePtrReg = IRValue::Register(nextTemp(), IRType::PTR);
                            currentBlock_->addInstruction({IROp::LOAD, vtablePtrReg, {objPtr, IRValue::Constant(8, IRType::I32)}, objIsVolatile});
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
                            // CIDER-BUG-080: arguments flow through the same
                            // interface conversion, so a concrete value can
                            // be passed to an interface parameter of an
                            // interface method and the vtable of a concrete
                            // receiver's method sees a fat pointer.
                            std::vector<TypeInfo> argTypes;
                            // CIDER-BUG-165: the method's declared parameter
                            // shapes are known here, so a list argument is
                            // materialised against its array parameter shape
                            // (the checker validates the same shape).
                            std::vector<TypeInfo> ifaceParams =
                                rootCallParams(m.parameterTypes,
                                               definingModuleFor(ifaceSym, ifaceSym->name));
                            for (size_t ai = 0; ai < expr.getArgs().size(); ++ai) {
                                const TypeInfo* paramT = ai < ifaceParams.size()
                                                             ? &ifaceParams[ai]
                                                             : nullptr;
                                args.push_back(
                                    evaluateCallArg(expr.getArgs()[ai].get(), paramT));
                                argTypes.push_back(lastExprType_);
                            }
                            convertCallArgs(args, 2, ifaceParams, 0, &argTypes);
                            // CIDER-BUG-077: a method returning an interface
                            // returns through the hidden-sret convention (its
                            // implementation is a non-virtual method, which
                            // ir_gen compiles as sret). The 16-byte slot goes
                            // right after the method address, so the callee's
                            // prologue sees [__sret, this, ...].
                            TypeInfo methodRet =
                                resolveInterfaceNominal(canonicalizeNominalInModule(
                                    m.returnType, definingModuleFor(ifaceSym, ifaceSym->name)));
                            bool ifaceRet = methodRet.isInterface;
                            IRValue retBuf;
                            IRValue dest;
                            IRType retIRType = IRType::I32;
                            if (ifaceRet) {
                                retBuf = IRValue::Register(nextTemp(), IRType::PTR);
                                currentBlock_->addInstruction({IROp::ALLOCA, retBuf, {IRValue::Constant(16, IRType::I32)}});
                                args.insert(args.begin() + 1, retBuf);
                            } else {
                                retIRType = (m.returnType.pointerLevel > 0) ? IRType::PTR : mapType(m.returnType.name);
                                dest = IRValue::Register(nextTemp(), retIRType);
                            }
                            IRInstruction callInstr;
                            callInstr.op = IROp::CALL;
                            // CID-005: interface dispatch is SysV (the
                            // implementations are pinned SysV by generate()).
                            callInstr.callAbi = Abi::SysV;
                            callInstr.dest = ifaceRet ? std::nullopt : std::optional<IRValue>(dest);
                            callInstr.args = std::move(args);
                            currentBlock_->addInstruction(callInstr);
                            lastValue_ = ifaceRet ? retBuf : dest;
                            lastExprType_ = ifaceRet ? methodRet : m.returnType;
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
                        if (!methodDef.first) methodDef = localMethodDef(classSym, m.mangledName);
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
            auto sym = resolvedFuncSym;
            if (!sym) {
                auto rawSym = symbolTable_.resolve(funcName);
                if (rawSym && rawSym->kind == SymbolKind::FUNCTION)
                    sym = std::static_pointer_cast<FunctionSymbol>(rawSym);
            }
            TypeInfo calleeRet = {"int", 0};
            std::string calleeDefMod;
            // CIDER-BUG-165: the callee's declared parameter shapes are
            // resolved BEFORE the arguments are evaluated, so a list-literal
            // argument can be materialised against its parameter's array
            // shape (evaluateCallArg); the legacy loop had no parameter
            // information until after the argument temporaries were built.
            std::vector<TypeInfo> calleeParams;
            size_t paramBase = 0;
            if (sym && sym->kind == SymbolKind::FUNCTION) {
                auto funcSym = std::static_pointer_cast<FunctionSymbol>(sym);
                calleeRet = funcSym->returnType;
                // CID-020/021: bare signature nominals belong to the callee;
                // scope the sret/size queries to its defining module so a
                // same-named type in the caller (or no visible type at all)
                // cannot change the call convention.
                calleeDefMod = definingModuleFor(sym, funcName);
                // CID-003: method symbols carry `this` as parameter 0, so
                // shift past it when an object is passed as the hidden first
                // argument.
                if ((objPtr.kind != IRValue::CONSTANT || objPtr.intValue != 0) &&
                    !funcSym->parameterTypes.empty()) {
                    paramBase = 1;
                }
                // CIDER-BUG-080: root the signature's interface spellings in
                // the callee's module before conversion, so an imported
                // callee's bare `Shape` parameter materialises the vtable
                // label that module generated.
                calleeParams = rootCallParams(funcSym->parameterTypes, calleeDefMod);
            }

            std::vector<IRValue> args;
            // CIDER-BUG-080: each evaluated argument's type drives the
            // interface conversion below (concrete value/pointer -> fat
            // pointer); collected here because lastExprType_ is overwritten
            // by later arguments.
            std::vector<TypeInfo> argTypes;
            for (size_t ai = 0; ai < expr.getArgs().size(); ++ai) {
                const TypeInfo* paramT = (paramBase + ai < calleeParams.size())
                                             ? &calleeParams[paramBase + ai]
                                             : nullptr;
                args.push_back(evaluateCallArg(expr.getArgs()[ai].get(), paramT));
                argTypes.push_back(lastExprType_);
            }

            if (sym && sym->kind == SymbolKind::FUNCTION) {
                // CIDER-BUG-090: present the result spelling rooted in the
                // callee's module (mirroring the checker), so member access
                // on the result resolves a return type the caller's module
                // may not see — e.g. an inherited method returning the base
                // module's private type.
                lastExprType_ =
                    canonicalizeNominalInModule(resolveTypeAlias(calleeRet, calleeDefMod), calleeDefMod);
                // CID-003: the parameter's declared width drives the
                // conversion of float-family arguments (a literal in a
                // float context keeps its parsed double).
                convertCallArgs(args, 0, calleeParams, paramBase, &argTypes);
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
            TypeInfo calleeIfaceRet = callInterfaceParamType(calleeRet, calleeDefMod);
            bool calleeIsIfaceRet = calleeIfaceRet.isInterface;
            bool calleeIsSret = isByValueAggregate(calleeRet, calleeDefMod) || calleeIsIfaceRet;
            // Virtual callees are never sret (they stay on the malloc path;
            // an interface-typed virtual return is rejected by the checker,
            // because VIRTUAL_CALL cannot carry the hidden slot); direct
            // CALLs here are free functions / non-virtual methods.
            if (calleeIsSret && !(isVirtual && vtableIndex != -1)) {
                int retSize = calleeIsIfaceRet ? 16 : getTypeSize(calleeRet, calleeDefMod);
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
                // CIDER-BUG-137: the sret slot is the caller's OWN copy of
                // the returned class/struct value (the callee memcpys into
                // it). The caller therefore finalizes it at the end of its
                // enclosing scope, after every use of the result, with the
                // same CIDER-BUG-088 order a declared local gets. Interface
                // results (fat pointers) own no embedded class values and
                // register nothing.
                registerTempDestructors(calleeRet, retBuf, calleeDefMod);
                if (calleeIsIfaceRet) {
                    // CIDER-BUG-077: the result is the address of the
                    // caller's 16-byte fat pointer; the interface flag lets
                    // assignment/dispatch/argument paths treat it as one.
                    lastExprType_ = calleeIfaceRet;
                } else {
                    // CIDER-BUG-090: keep the callee-module-rooted spelling.
                    lastExprType_.isUserDefined = true;
                }
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
            // CIDER-BUG-137: a virtual (or otherwise non-sret) by-value
            // aggregate return comes back as a fresh heap copy (the legacy
            // malloc path in visit(ReturnStmt)); it is the caller's owned
            // temporary just like an sret slot, so register its finalizers
            // at the caller's enclosing scope. `lastExprType_` still holds
            // the callee-rooted return spelling from above.
            if (isVirtual && vtableIndex != -1 &&
                isByValueAggregate(lastExprType_, calleeDefMod)) {
                registerTempDestructors(lastExprType_, dest, calleeDefMod);
            }
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
            // CIDER-BUG-080: evaluated argument types for the interface
            // conversion (see materializeInterfaceArg).
            std::vector<TypeInfo> argTypes;
            for (auto& arg : expr.getArgs()) {
                arg->accept(*this);
                args.push_back(lastValue_);
                argTypes.push_back(lastExprType_);
            }

            if (calleeType.isClosure) {
                // Closure call: the value is a closure object {fnptr,
                // captures...} — load the fnptr and pass the object first.
                IRValue obj = fnPtr;
                fnPtr = IRValue::Register(nextTemp(), IRType::PTR);
                currentBlock_->addInstruction({IROp::LOAD, fnPtr, {obj, IRValue::Constant(0, IRType::I32)}});
                // CIDER-BUG-063: the declared `func(...)` signature (now
                // parsed instead of discarded) drives argument conversion and
                // the result type exactly like the typed fn(...) path below;
                // an untyped legacy marker keeps the assumed-int result.
                TypeInfo retType{"int", 0};
                if (calleeType.fnReturnType) retType = *calleeType.fnReturnType;
                if (calleeType.fnParamTypes) {
                    convertCallArgs(args, 0, *calleeType.fnParamTypes, 0, &argTypes);
                }
                IRType destIR = (retType.pointerLevel > 0 || retType.isFunctionPointer)
                                    ? IRType::PTR
                                    : mapType(retType.name);
                if (destIR == IRType::VOID) destIR = IRType::I32;
                IRValue dest = IRValue::Register(nextTemp(), destIR);
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
                lastExprType_ = retType;
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
                    convertCallArgs(args, 0, *calleeType.fnParamTypes, 0, &argTypes);
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

    // CIDER-BUG-065: record `n` as a capture when it is an outer local that
    // is not shadowed by a parameter or a nearer binding (the catch variable,
    // a loop variable, a nested literal's parameter).
    void noteCapture(const std::string& n, const std::set<std::string>& shadowed,
                     std::vector<std::string>& out) {
        if (!shadowed.count(n) && variableValues_.count(n) &&
            std::find(out.begin(), out.end(), n) == out.end()) {
            out.push_back(n);
        }
    }

    void collectCapturesExpr(Expression* e, const std::set<std::string>& params,
                             std::vector<std::string>& out) {
        if (!e) return;
        if (auto id = dynamic_cast<IdentifierExpr*>(e)) {
            noteCapture(id->getName(), params, out);
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
        } else if (auto list = dynamic_cast<ListExpr*>(e)) {
            for (auto& el : list->getElements()) collectCapturesExpr(el.get(), params, out);
        } else if (auto table = dynamic_cast<TableExpr*>(e)) {
            for (auto& entry : table->getEntries()) {
                collectCapturesExpr(entry.first.get(), params, out);
                collectCapturesExpr(entry.second.get(), params, out);
            }
        } else if (auto slice = dynamic_cast<SliceExpr*>(e)) {
            collectCapturesExpr(slice->getObject(), params, out);
            collectCapturesExpr(slice->getStart(), params, out);
            collectCapturesExpr(slice->getEnd(), params, out);
        } else if (auto aw = dynamic_cast<AwaitExpr*>(e)) {
            collectCapturesExpr(aw->getValue(), params, out);
        } else if (auto lit = dynamic_cast<StructLiteralExpr*>(e)) {
            for (auto& field : lit->getFields()) collectCapturesExpr(field.second.get(), params, out);
        } else if (auto fn = dynamic_cast<FunctionExpr*>(e)) {
            // CIDER-BUG-065: a nested literal's uses must also become captures
            // of this closure, so the chain passes outer variables through;
            // the nested literal collects them again from this closure's
            // frame when its own body is generated.
            std::set<std::string> nestedParams = params;
            for (auto& p : fn->getParams()) nestedParams.insert(p.first);
            for (auto& b : fn->getBody()) collectCapturesStmt(b.get(), nestedParams, out);
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
                // CIDER-BUG-065: `for (var i = ...)` shadows an outer capture
                // of the same name in the condition, increment and body.
                std::set<std::string> loopParams = params;
                if (auto initDecl = dynamic_cast<VarDeclStmt*>(fs->getInitializer())) {
                    loopParams.insert(initDecl->getName());
                }
                if (fs->getCondition()) collectCapturesExpr(fs->getCondition(), loopParams, out);
                if (fs->getIncrement()) collectCapturesStmt(fs->getIncrement(), loopParams, out);
                for (auto& b : fs->getBody()) collectCapturesStmt(b.get(), loopParams, out);
                break;
            }
            case ASTNode::FOR_EACH_STMT: {
                auto fe = dynamic_cast<ForEachStmt*>(s);
                collectCapturesExpr(fe->getIterable(), params, out);
                // CIDER-BUG-065: the loop variable shadows an outer capture of
                // the same name inside the body.
                std::set<std::string> bodyParams = params;
                if (!fe->getVarName().empty()) bodyParams.insert(fe->getVarName());
                for (auto& b : fe->getBody()) collectCapturesStmt(b.get(), bodyParams, out);
                break;
            }
            case ASTNode::DELETE_STMT:
                collectCapturesExpr(dynamic_cast<DeleteStmt*>(s)->getTarget(), params, out);
                break;
            case ASTNode::THROW_STMT:
                collectCapturesExpr(dynamic_cast<ThrowStmt*>(s)->getValue(), params, out);
                break;
            case ASTNode::TRY_CATCH_STMT: {
                auto ts = dynamic_cast<TryCatchStmt*>(s);
                for (auto& b : ts->getTryBlock()) collectCapturesStmt(b.get(), params, out);
                // CIDER-BUG-065: the catch binding is not a capture, and it
                // shadows an outer variable of the same name in the body.
                std::set<std::string> catchParams = params;
                if (!ts->getCatchVar().empty()) catchParams.insert(ts->getCatchVar());
                for (auto& b : ts->getCatchBlock()) collectCapturesStmt(b.get(), catchParams, out);
                break;
            }
            case ASTNode::DEFER_STMT:
                collectCapturesStmt(dynamic_cast<DeferStmt*>(s)->getDeferredStmt(), params, out);
                break;
            case ASTNode::ASM_STMT: {
                // CIDER-BUG-065: `{var}` placeholders are uses of the named
                // variable; mirror visit(AsmStmt)'s `*`/`:width` stripping.
                auto asmStmt = dynamic_cast<AsmStmt*>(s);
                for (const auto& entry : asmStmt->localVariables) {
                    std::string name = entry.first.empty() ? entry.second : entry.first;
                    if (!name.empty() && name[0] == '*') name = name.substr(1);
                    size_t colon = name.find(':');
                    if (colon != std::string::npos) name = name.substr(0, colon);
                    if (!name.empty()) noteCapture(name, params, out);
                }
                break;
            }
            case ASTNode::MATCH_STMT: {
                auto ms = dynamic_cast<MatchStmt*>(s);
                collectCapturesExpr(ms->getExpression(), params, out);
                for (auto& arm : ms->getArms()) {
                    collectCapturesExpr(arm.pattern.get(), params, out);
                    collectCapturesStmt(arm.body.get(), params, out);
                }
                break;
            }
            case ASTNode::EXPORT_STMT:
                collectCapturesStmt(dynamic_cast<ExportStmt*>(s)->getStmt(), params, out);
                break;
            case ASTNode::EXPORT_EXPRESSION_STMT:
                collectCapturesExpr(dynamic_cast<ExportExpressionStmt*>(s)->getExpression(), params, out);
                break;
            default:
                break;
        }
    }

    void visit(FunctionExpr& expr) override {
        if (!currentFunction_) {
            lastValue_ = IRValue::Constant(0, IRType::PTR);
            // CIDER-BUG-063: closure-object representation marker.
            lastExprType_ = {"func", 1};
            lastExprType_.isClosure = true;
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
        currentFunction_->setSourceLine(expr.getLine());
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
            visitStatement(s.get());
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
        // CIDER-BUG-063: closure-object representation marker plus the
        // literal's own signature, so a direct literal call converts
        // arguments and types its result like the checker's literal type.
        TypeInfo closureType;
        closureType.name = "func";
        closureType.pointerLevel = 1;
        closureType.isFunctionPointer = true;
        closureType.isClosure = true;
        closureType.fnParamTypes = std::make_shared<std::vector<TypeInfo>>();
        for (auto& p : expr.getParams()) {
            closureType.fnParamTypes->push_back(p.second ? TypeInfo::fromAST(p.second.get())
                                                         : TypeInfo{"int", 0});
        }
        closureType.fnReturnType = std::make_shared<TypeInfo>(retType);
        lastExprType_ = closureType;
    }

    // CIDER-FEAT-002: the EnumSymbol a `Enum` / `lib.Enum` qualifier names
    // (or a `type Alias = Enum` provider resolving to one). `constContext`
    // binds names like the const fold does (the declaration scope first);
    // otherwise the ordinary use-site scope walk applies.
    std::shared_ptr<Symbol> resolveEnumQualifierSymbol(Expression* object, bool constContext) {
        auto resolveName = [&](const std::string& n) {
            return constContext ? resolveConstIdentifier(n) : symbolTable_.resolve(n);
        };
        auto asEnum = [&](const std::shared_ptr<Symbol>& sym) -> std::shared_ptr<Symbol> {
            if (sym && sym->kind == SymbolKind::ENUM) return sym;
            // `type Alias = Enum`: follow the provider chain rooted at the
            // alias's own canonical name, so its bare target spelling
            // resolves in the alias's defining module (mirrors the
            // checker's enumSymbolOfTypeName).
            if (sym && sym->isTypeAlias) {
                TypeInfo rooted;
                rooted.name = canonicalTypeName(sym->name, definingModuleFor(sym, sym->name));
                TypeInfo target = resolveTypeAlias(rooted);
                auto resolved = resolveNominalType(target.name);
                if (resolved && resolved->kind == SymbolKind::ENUM) return resolved;
            }
            return nullptr;
        };
        if (auto* base = dynamic_cast<IdentifierExpr*>(object)) {
            return asEnum(resolveName(base->getName()));
        }
        if (auto* inner = dynamic_cast<MemberAccessExpr*>(object)) {
            if (inner->isArrow()) return nullptr;
            if (auto* base = dynamic_cast<IdentifierExpr*>(inner->getObject())) {
                auto baseSym = resolveName(base->getName());
                if (baseSym && baseSym->kind == SymbolKind::MODULE) {
                    Scope* ms = std::static_pointer_cast<ModuleSymbol>(baseSym)->scope;
                    auto target = ms ? ms->resolveLocal(inner->getMember()) : nullptr;
                    return asEnum(target);
                }
            }
        }
        return nullptr;
    }

    // CIDER-FEAT-002: fold `Enum.VARIANT` / `lib.Enum.VARIANT`. The enum
    // name is a type qualifier, so the variant's constant is emitted
    // directly (no storage, no object value). Returns true when `expr` is a
    // qualified enum variant reference.
    bool tryFoldEnumVariant(MemberAccessExpr& expr) {
        if (expr.isArrow()) return false;
        auto enumSym = resolveEnumQualifierSymbol(expr.getObject(), /*constContext=*/false);
        if (!enumSym) return false;
        auto esym = std::static_pointer_cast<EnumSymbol>(enumSym);
        int64_t value = 0;
        if (!esym->findVariant(expr.getMember(), value)) return false;
        lastValue_ = IRValue::Constant((double)value, IRType::I32);
        lastExprType_ = {canonicalTypeName(esym->name, definingModuleFor(esym, esym->name)), 0};
        return true;
    }

    void visit(MemberAccessExpr& expr) override {
        // CIDER-FEAT-002: `Enum.VARIANT` / `lib.Enum.VARIANT` has no object
        // value; fold the variant constant directly. Checked before the
        // module-const paths so a qualified enum type never evaluates as an
        // expression.
        if (tryFoldEnumVariant(expr)) return;
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
                        definingModuleFor(target, expr.getMember()), target->name,
                        target->isExtern));
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
                    // CIDER-FEAT-002: a module-qualified enum variant
                    // (`lib.RED`) has no storage — fold its constant. A
                    // non-variant `const` keeps its global load (the
                    // collector never evaluates its initializer into
                    // constValue), so folding is limited to variants.
                    if (target->isEnumVariant) {
                        lastValue_ = IRValue::Constant(target->constValue, mapType(target->type.name));
                        lastExprType_ = canonicalizeSymbolType(target, expr.getMember());
                        return;
                    }
                    std::string gname = emitGlobalName(
                        definingModuleFor(target, expr.getMember()), target->name,
                        target->isExtern);
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
                    // CIDER-BUG-061: a volatile symbol read is observable.
                    currentBlock_->addInstruction({IROp::LOAD, dest, {IRValue::Label(gname)},
                                                   lastExprType_.isVolatile});
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
                    // CIDER-BUG-090: an inherited field keeps its own
                    // declaring module, which the accessed class's module
                    // may not see.
                    std::string fieldMod = fieldTypeModule(field, ownerMod);
                    TypeInfo fieldType =
                        canonicalizeNominalInModule(resolveTypeAlias(field.type, fieldMod), fieldMod);
                    // CIDER-BUG-069: an interface-typed field is a 16-byte
                    // fat pointer; reads present its address (like an
                    // interface variable) so dispatch sees both words.
                    fieldType = resolveInterfaceNominal(fieldType);
                    bool fieldIsStruct = (fieldType.pointerLevel == 0);
                    if (fieldIsStruct) {
                        auto fieldTypeSym = resolveNominalType(fieldType.name);
                        if (!fieldTypeSym && !fieldMod.empty()) {
                            fieldTypeSym = resolveNominalInModule(fieldType.name, fieldMod);
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
                    // CIDER-BUG-061: the field type may be volatile itself,
                    // and every member of a volatile object is volatile.
                    bool fieldVolatile = fieldType.isVolatile || objType.isVolatile;
                    // CIDER-BUG-133: present that same flag on the read's
                    // type, so consumers that classify by the EXPRESSION's
                    // type (interface dispatch reads, aggregate copies) keep
                    // the enclosing-object volatility that the load above
                    // already carries. Without it `h.shape.area()` through a
                    // volatile Holder read both fat-pointer words plainly.
                    fieldType.isVolatile = fieldVolatile;
                    if (field.bitWidth > 0 && !isAddress_) {
                        bool isSigned = (fieldType.name == "int" || fieldType.name == "i8" ||
                                         fieldType.name == "i16" || fieldType.name == "i32");
                        lastValue_ = emitBitfieldLoad(addr, field.bitOffset, field.bitWidth, isSigned,
                                                      fieldVolatile);
                    } else if (isAddress_ || fieldIsStruct || fieldIsArray || fieldType.isInterface) {
                        lastValue_ = addr;
                    } else {
                        IRType fieldIRType = (fieldType.pointerLevel > 0) ? IRType::PTR : mapType(fieldType.name);
                        IRValue dest = IRValue::Register(nextTemp(), fieldIRType);
                        currentBlock_->addInstruction({IROp::LOAD, dest, {addr, IRValue::Constant(0, IRType::I32)},
                                                       fieldVolatile});
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
        int structSize = getTypeSize({expr.getStructName(), 0});
        currentBlock_->addInstruction({IROp::ALLOCA, dest, {IRValue::Constant(structSize, IRType::I32)}});
        // CIDER-BUG-067: a literal that omits fields must leave them zero,
        // not whatever the shared alloca slot held.
        emitZeroInstance(dest, structSize);

        // CIDER-BUG-095: a module-qualified template literal
        // (`m.Box<int>{...}`) resolves through the same nominal lookup the
        // type-position spelling uses (CIDER-BUG-073), so field offsets
        // come from the shared instantiation.
        auto sym = resolveNominalType(expr.getStructName());
        if (sym && (sym->kind == SymbolKind::STRUCT || sym->kind == SymbolKind::CLASS)) {
            auto structSym = std::static_pointer_cast<StructSymbol>(sym);
            std::string structMod = definingModuleFor(structSym, structSym->name);
            // CIDER-BUG-087: fields the literal omits must run the field
            // initializers of the class values they embed, exactly like a
            // declared local. Explicit fields copy their slot wholesale
            // (below), so the omitted ones are initialised first.
            {
                std::set<std::string> explicitFields;
                for (auto& fieldInit : expr.getFields()) {
                    explicitFields.insert(fieldInit.first);
                }
                std::set<const StructSymbol*> visiting;
                for (const auto& f : structSym->fields) {
                    if (explicitFields.count(f.name)) continue;
                    // CIDER-BUG-090: per-field declaring module (templates).
                    emitEmbeddedInitializers(f.type, dest, f.offset,
                                             fieldTypeModule(f, structMod), visiting);
                }
            }
            for (auto& fieldInit : expr.getFields()) {
                int offset = 0;
                TypeInfo fieldType;
                for (const auto& f : structSym->fields) {
                    if (f.name == fieldInit.first) {
                        offset = f.offset;
                        // CIDER-BUG-113: classify the field by its
                        // alias-resolved shape like the member-read and
                        // member-store paths do; an alias spelling kept
                        // `isArray` false, so a small alias-declared array
                        // field took the scalar STORE below (writing the
                        // source address over the field and its neighbor)
                        // instead of copying the element bytes.
                        fieldType = canonicalizeNominalInModule(
                            resolveTypeAlias(f.type, structMod), structMod);
                        break;
                    }
                }
                fieldType = resolveInterfaceNominal(fieldType);
                if (fieldType.isInterface) {
                    // CIDER-BUG-069: an interface-typed field of a struct
                    // literal stores the same 16-byte fat pointer a direct
                    // field assignment does.
                    IRValue fieldAddr = IRValue::Register(nextTemp(), IRType::PTR);
                    currentBlock_->addInstruction({IROp::ADD, fieldAddr,
                                                   {dest, IRValue::Constant(offset, IRType::I32)}});
                    emitInterfaceStore(fieldType, fieldAddr, fieldInit.second.get(),
                                       fieldType.isVolatile);
                    continue;
                }
                // CIDER-BUG-165: a list literal for a fixed-array field is
                // materialised against the field's declared shape
                // (CIDER-BUG-163's emitListLiteralTemp) exactly like a
                // declaration-time array; the aggregate branch below then
                // copies the whole array into the field. visit(ListExpr)'s
                // context-free inference would have stored pointer words for
                // nested rows / word-sized elements for narrow ones.
                if (auto* list = dynamic_cast<ListExpr*>(fieldInit.second.get());
                    list && fieldType.isArray && fieldType.pointerLevel == 0 &&
                    fieldType.arraySize > 0) {
                    lastValue_ = emitListLiteralTemp(*list, fieldType, false);
                } else {
                    fieldInit.second->accept(*this);
                }
                IRValue val = lastValue_;

                // CID-003: an aggregate literal's field stores at the
                // declared width, exactly like a variable store (a literal
                // f32 into an f64 field must not write 4 bytes).
                val = convertValueToType(val, fieldType);

                // CIDER-BUG-081: a by-value struct/class (or array) field
                // takes the same MEMCPY rule as a direct member store — the
                // field is classified by its own RESOLVED declared type, and
                // the RHS value of an aggregate is its source ADDRESS. The
                // scalar STORE below would write that address into the field
                // (4/8 bytes) instead of copying the value bytes.
                if (fieldType.pointerLevel == 0 && !fieldType.isInterface && !fieldType.isSlice &&
                    (fieldType.isArray || isStructValueType(fieldType) || getTypeSize(fieldType) > 8)) {
                    IRValue fieldAddr = IRValue::Register(nextTemp(), IRType::PTR);
                    currentBlock_->addInstruction({IROp::ADD, fieldAddr,
                                                   {dest, IRValue::Constant(offset, IRType::I32)}});
                    // CIDER-BUG-133: the copy from a volatile field-init
                    // source (e.g. a member of a volatile enclosing object)
                    // is an observable read. CIDER-BUG-146: a
                    // volatile-qualified field is itself a user-qualified
                    // volatile object even inside the literal temp, so its
                    // destination flag marks the copy exactly like the
                    // direct member store (`h.inner = src`); an unqualified
                    // field of an unqualified temp still contributes nothing.
                    currentBlock_->addInstruction({IROp::MEMCPY, std::nullopt,
                                                   {fieldAddr, val, IRValue::Constant(getTypeSize(fieldType), IRType::I32)},
                                                   fieldType.isVolatile || lastExprType_.isVolatile});
                    // CIDER-BUG-089: an explicit class-valued field copy
                    // re-stamps the field's declared class vptr, so
                    // `S{ inner: *p }` slices to Inner like every other copy.
                    // CIDER-BUG-146: the stamp follows the destination field's
                    // volatile flag, like every other copy-site re-stamp.
                    emitClassVptrStoreForType(fieldType, fieldAddr,
                                              definingModuleFor(structSym, structSym->name),
                                              fieldType.isVolatile);
                } else {
                    // CIDER-BUG-146: an explicit field store into a
                    // volatile-qualified field is an observable write,
                    // mirroring the direct member store's field rule
                    // (memberAccessIsVolatile). The literal's temp is not
                    // itself volatile, so only the field's own flag marks
                    // the store.
                    currentBlock_->addInstruction({IROp::STORE, std::nullopt, {dest, val, IRValue::Constant(offset, IRType::I32)},
                                                   fieldType.isVolatile});
                }
            }
        }
        lastValue_ = dest;
        // CIDER-BUG-095: report the resolved symbol's spelling, so a
        // qualified literal (`m.Box<int>`) carries the shared bare
        // instantiation name like every other use.
        lastExprType_ = {sym ? sym->name : expr.getStructName(), 0};
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
                // CIDER-BUG-100: a pointer-to-interface dereferences to the
                // 16-byte fat pointer, so `*p` presents its address (like an
                // interface variable) for the interface copy and dispatch
                // read paths; a scalar LOAD would hand the data word to
                // dispatch as if it were the fat pointer's address. Recover
                // the flag by name first: the `*Shape` declaration spelling
                // cannot carry it.
                pointee = resolveInterfaceNominal(pointee);
                bool pointeeIsAggregate = (pointee.pointerLevel == 0) &&
                    (pointee.isArray || pointee.isInterface ||
                     isStructValueType(pointee) ||
                     isUniqueModuleAggregate(pointee.name));
                if (pointeeIsAggregate) {
                    lastValue_ = operand;
                    type = pointee;
                    lastExprType_ = type;
                    return;
                }
                IRType derefType = (type.pointerLevel > 1) ? IRType::PTR : mapType(type.name);
                IRValue dest = IRValue::Register(nextTemp(), derefType);
                // CIDER-BUG-061: a load through `volatile T*` is observable.
                currentBlock_->addInstruction({IROp::LOAD, dest, {operand, IRValue::Constant(0, IRType::I32)},
                                               type.isVolatile});
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
            // CIDER-BUG-061: `++`/`--` on a volatile object is a volatile
            // read-modify-write.
            currentBlock_->addInstruction({IROp::LOAD, current, {addr, IRValue::Constant(0, IRType::I32)},
                                           operandType.isVolatile});

            IRValue one = IRValue::Constant(1, irType);
            IRValue result = IRValue::Register(nextTemp(), irType);
            currentBlock_->addInstruction({expr.getOperator() == "++" ? IROp::ADD : IROp::SUB, result, {current, one}});

            if (addr.kind == IRValue::LABEL) {
                currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, result},
                                               operandType.isVolatile});
            } else {
                currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, result, IRValue::Constant(0, IRType::I32)},
                                               operandType.isVolatile});
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
        // CIDER-FEAT-006: a module-level variable loaded from a
        // `.cider-meta` snapshot. The defining module's object owns the
        // storage; register it as extern DATA and let reads/writes resolve
        // through the mangled label. The collector's symbol (with its
        // `constInit` for folding) stays in the scope untouched.
        if (stmt.isMetaDecl()) {
            std::string gname = emitGlobalName(currentModule_, stmt.getName());
            module_.addExternVariable(gname);
            globalValues_[gname] = IRValue::Label(gname);
            return;
        }
        auto sym = std::make_shared<Symbol>();
        sym->kind = SymbolKind::VARIABLE;
        sym->name = stmt.getName();
        sym->type = TypeInfo::fromAST(stmt.getTypeAST());
        // CIDER-BUG-070: resolve the interface flag through the nominal
        // lookup (bare, dotted `lib.Shape`, canonical `lib::Shape`) so a
        // module-qualified declaration is a 16-byte fat pointer exactly
        // like an unqualified one.
        sym->type = resolveInterfaceNominal(sym->type);
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
            // CIDER-BUG-063: a legacy `func(...)` global initialized with a
            // named function stores a raw code pointer in its .data cell
            // (see the function-pointer path below). Present the symbol as
            // the plain representation, so reads and calls take the typed
            // indirect path instead of the closure-object path. nilPtr and
            // closure initializers keep the closure marker.
            TypeInfo declaredFn = resolveFunctionPointerAlias(sym->type);
            if (declaredFn.isClosure &&
                !functionPointerInitTarget(stmt.getInitializer()).empty()) {
                sym->type = declaredFn;
                sym->type.isClosure = false;
            }
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
            // CIDER-BUG-092: an alias-declared fixed array (`type W =
            // [u16;3]`) carries the alias spelling in sym->type, so isArray
            // is false until the chain resolves. The array-shaped
            // initializer paths below (wide literal, list literal) decide by
            // shape, so resolve once and use the resolved form.
            TypeInfo declArray = resolveTypeAlias(sym->type);
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
                if (declArray.isArray && declArray.pointerLevel == 0) {
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
            // through to the legacy BSS path below. CIDER-BUG-092: use the
            // alias-resolved shape so an alias spelling lands here too.
            if (declArray.isArray && declArray.pointerLevel == 0 && declArray.arraySize > 0) {
                if (auto list = dynamic_cast<ListExpr*>(stmt.getInitializer())) {
                    auto& elems = list->getElements();
                    std::string dataLine = gname + ": db ";
                    bool firstByte = true;
                    bool allConst = ((int)elems.size() == declArray.arraySize);
                    // CIDER-BUG-163: append one element's constant bytes in
                    // declaration order. A nested list literal for a
                    // fixed-array row recurses against the declared row
                    // shape; the bytes of consecutive rows are contiguous,
                    // so the flat concatenation stays the array image.
                    std::function<bool(Expression*, TypeInfo)> appendConstBytes =
                        [&](Expression* e, TypeInfo et) -> bool {
                        // CIDER-BUG-163: the row element may be an alias
                        // spelling of an array (`type Row = [u8;2]`), so
                        // resolve before the shape test.
                        et = resolveTypeAlias(et);
                        if (et.isArray && et.pointerLevel == 0 && et.arraySize > 0) {
                            auto* inner = dynamic_cast<ListExpr*>(e);
                            if (!inner || (int)inner->getElements().size() != et.arraySize) {
                                return false;
                            }
                            TypeInfo innerElem = et.elementType();
                            for (auto& ie : inner->getElements()) {
                                if (!appendConstBytes(ie.get(), innerElem)) return false;
                            }
                            return true;
                        }
                        int elemSize = getTypeSize(et);
                        IRType elemIR = mapType(et.name);
                        uint64_t v = 0;
                        if (elemIR == IRType::F32 || elemIR == IRType::F64) {
                            // CID-003 follow-up (M3c): a float-typed element
                            // converts its constant to the element width's
                            // IEEE-754 bits. An integer element used to emit
                            // its raw integer bits (a denormal when read as
                            // f32/f64); float literal elements are rejected
                            // by the checker before this point.
                            double fv = 0;
                            if (!tryEvalConstF64(e, fv)) return false;
                            if (elemIR == IRType::F32) {
                                float f = (float)fv;
                                uint32_t b32 = 0;
                                std::memcpy(&b32, &f, sizeof(b32));
                                v = b32;
                            } else {
                                std::memcpy(&v, &fv, sizeof(v));
                            }
                        } else if (!tryEvalConstU64(e, v)) {
                            return false;
                        }
                        for (int b = 0; b < elemSize; b++) {
                            if (!firstByte) dataLine += ", ";
                            firstByte = false;
                            dataLine += std::to_string((v >> (8 * b)) & 0xFFULL);
                        }
                        return true;
                    };
                    for (size_t ei = 0; allConst && ei < elems.size(); ei++) {
                        if (!appendConstBytes(elems[ei].get(), declArray.elementType())) {
                            allConst = false;
                            break;
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
                module_.addGlobalVar(gname, IRType::PTR, 0, 0, 8, false, currentSourcePath_);
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
            // CIDER-BUG-113: classify the aggregate by its alias-resolved
            // shape, like the literal paths above (CIDER-BUG-092). An
            // alias-declared fixed array (`type Arr = [Inner;3]`) carries
            // the alias spelling in sym->type, where isArray is false and
            // isStructValueType rejects the array shape, so the global was
            // never queued and its embedded class initializers were skipped.
            std::string aggMod = definingModuleFor(sym, stmt.getName());
            TypeInfo declAgg = resolveTypeAlias(sym->type, aggMod);
            // CIDER-BUG-087/089: a module-scope aggregate whose embedded
            // class values need formation stores (field initializers or a
            // vptr) stays zero in .bss, so queue its pass for the
            // synthesized startup function (see emitGlobalFieldInitializers).
            // The predicate filters out scalars and class-free aggregates,
            // which emit nothing.
            if (declAgg.pointerLevel == 0 &&
                (declAgg.isArray || isStructValueType(declAgg))) {
                // CIDER-BUG-144: carry the declared (alias-resolved)
                // volatility into the synthesized startup pass.
                pendingGlobalInits_.push_back({declAgg, aggMod, IRValue::Label(gname),
                                               declAgg.isVolatile});
            }
            module_.addGlobalVar(gname, (declAgg.pointerLevel > 0) ? IRType::PTR : mapType(declAgg.name), initVal, declAgg.isArray ? declAgg.arraySize : 0, totalBytes, initIsFloat, currentSourcePath_);
            globalValues_[gname] = IRValue::Label(gname);
            return;
        }

        IRValue addr = IRValue::Register(nextTemp(), IRType::PTR);
        currentBlock_->addInstruction({IROp::ALLOCA, addr, {IRValue::Constant(getTypeSize(sym->type), IRType::I32)}});
        variableValues_[stmt.getName()] = addr;

        // Check if this type has a destructor
        // CIDER-BUG-113: classify the declaration by its alias-resolved
        // shape, mirroring CIDER-BUG-092's declArray below. An
        // alias-to-class spelling otherwise took the struct branch (the
        // alias provider is STRUCT-kind), skipping the class instance's
        // zero-fill, and an alias-declared array only reached the array
        // branch incidentally through aggregateHasInitializers' own
        // resolution.
        TypeInfo varType = resolveTypeAlias(sym->type, currentModule_);
        // CIDER-BUG-088: scope-exit finalizers. A local's destructor set is
        // the class's own destructor (if any) plus every class value it
        // embeds by value — class fields, nested structs and fixed arrays of
        // class values included. Registration is the same recursion the
        // initializer fix uses (registerEmbeddedDestructors, gated by
        // aggregateHasDestructors), so aggregates without embedded class
        // destructors register exactly what they did before. Registration is
        // independent of the initializer: an aggregate initialized by a
        // copy/literal still owns subobjects to destroy. Push order is
        // construction order, so the stack's reverse walk destroys the class
        // body first and then embedded values in reverse declaration order.
        {
            std::set<const StructSymbol*> visiting;
            if (aggregateHasDestructors(varType, currentModule_, visiting)) {
                registerEmbeddedDestructors(varType, addr, 0, currentModule_, visiting);
            }
        }
        if (varType.pointerLevel == 0 && !varType.isArray) {
            auto typeSym = resolveNominalType(varType.name);
            if (typeSym && typeSym->kind == SymbolKind::CLASS) {
                auto classSym = std::static_pointer_cast<StructSymbol>(typeSym);
                // CIDER-BUG-049: a value-class instance without an explicit
                // initializer runs the class's field initializers. With one
                // (`var v: C = other;`) the copy/memcpy below copies the
                // source object wholesale, so the source state wins.
                // CIDER-BUG-089: the same pass stamps the class's vptr (part
                // of emitClassFieldInitializersImpl), so a value local of a
                // virtual class dispatches through its declared class.
                if (!stmt.getInitializer()) {
                    // CIDER-BUG-067: zero the whole value-class slot first,
                    // then run the constant field initializers on top.
                    // CIDER-BUG-144: a volatile destination's formation
                    // stores are observable accesses (alias-resolved flag,
                    // like every copy site).
                    emitZeroInstance(addr, getTypeSize(varType), varType.isVolatile);
                    emitClassFieldInitializers(classSym, addr, varType.isVolatile);
                }
            } else if (!stmt.getInitializer()) {
                // CIDER-BUG-087: a struct value embeds its class-valued
                // fields by value, so a fresh local must run those classes'
                // field initializers at the fields' addresses. Gated on the
                // predicate so structs without embedded class initializers
                // keep their existing (unzeroed) emission.
                std::set<const StructSymbol*> visiting;
                if (aggregateNeedsInit(varType, currentModule_, visiting)) {
                    emitZeroInstance(addr, getTypeSize(varType), varType.isVolatile);
                    emitEmbeddedInitializers(varType, addr, 0, currentModule_, visiting,
                                             varType.isVolatile);
                }
            }
        } else if (varType.pointerLevel == 0 && varType.isArray && !stmt.getInitializer()) {
            // CIDER-BUG-087: a fixed array of class values embeds each
            // element as a subobject; zero the whole array so unset fields
            // read zero, then run every element's initializers. Struct
            // elements recurse through their own class-valued fields.
            std::set<const StructSymbol*> visiting;
            if (aggregateNeedsInit(varType, currentModule_, visiting)) {
                emitZeroInstance(addr, getTypeSize(varType), varType.isVolatile);
                emitEmbeddedInitializers(varType, addr, 0, currentModule_, visiting,
                                         varType.isVolatile);
            }
        }

        if (stmt.getInitializer()) {
            // CIDER-BUG-054: interface initialization stores the fat
            // pointer (data + vtable). Reassignment shares this lowering
            // (see visit(AssignStmt)), so the two paths cannot diverge.
            if (sym->type.isInterface) {
                // CIDER-BUG-133: the target flag comes from the
                // alias-resolved type, so a volatile declaration through an
                // alias (`type VS = volatile Shape`) marks both words
                // exactly like the direct spelling.
                emitInterfaceStore(sym->type, addr, stmt.getInitializer(),
                                   varType.isVolatile);
                return;
            }
            // Non-interface initializers keep the legacy scalar/aggregate
            // store path.
            {
                // CIDER-BUG-092: an alias-declared fixed array (`type Row =
                // [u8;3]`) carries the alias spelling in sym->type, so
                // isArray is false until the chain resolves. The array-shaped
                // paths below (list literal, slice copy) decide by shape, so
                // resolve once and use the resolved form, mirroring the
                // declaration-site checks.
                TypeInfo declArray = resolveTypeAlias(sym->type);
                // Fixed-size array from a list literal (CID-007): store
                // each element at its own offset with the element width
                // (a MEMCPY of the literal temp would copy I32-wide
                // elements into narrow slots).
                if (declArray.isArray && declArray.pointerLevel == 0 && declArray.arraySize > 0) {
                    if (auto list = dynamic_cast<ListExpr*>(stmt.getInitializer())) {
                        auto& elems = list->getElements();
                        TypeInfo elemType = declArray.elementType();
                        // CIDER-BUG-052: an element alias resolves to its
                        // real shape (array elements need the MEMCPY below).
                        elemType = resolveTypeAlias(elemType);
                        // CIDER-BUG-135: an interface-typed element is the
                        // 16-byte fat pointer, exactly like a non-list
                        // interface target; recover the flag for a spelling
                        // stripped from the array's bare element name.
                        elemType = resolveInterfaceNominal(elemType);
                        int elemSize = getTypeSize(elemType);
                        // CIDER-BUG-052: an array-typed element is
                        // address-like; copy its bytes into the slot instead
                        // of storing the address itself. CIDER-BUG-104: a
                        // by-value struct/class element (at ANY size — an
                        // exactly-8-byte struct missed the `> 8` fallback)
                        // and any other value wider than a machine word
                        // (interfaces) are address-like too: the element
                        // expression yields the source ADDRESS, so the legacy
                        // scalar STORE wrote pointer words into the element.
                        // Mirrors CIDER-BUG-083's element-store rule.
                        bool elemIsAggregate = isListAggregateElement(elemType);
                        for (size_t ei = 0; ei < elems.size(); ei++) {
                            // CIDER-BUG-163: a nested list literal for a
                            // fixed-array row is materialised against the
                            // declared row shape and copied as an aggregate
                            // (CIDER-BUG-104's store rule), instead of
                            // letting visit(ListExpr) type it `*T` and store
                            // a pointer word into the row slot.
                            if (auto* inner = dynamic_cast<ListExpr*>(elems[ei].get());
                                inner && elemType.isArray && elemType.pointerLevel == 0 &&
                                elemType.arraySize > 0) {
                                IRValue innerAddr = emitListLiteralTemp(*inner, elemType,
                                                                       varType.isVolatile);
                                emitListElementStore(addr, (int64_t)(ei * (size_t)elemSize),
                                                     innerAddr, elemType, elemSize, true,
                                                     varType.isVolatile, false);
                                continue;
                            }
                            elems[ei]->accept(*this);
                            // CIDER-BUG-135: an interface-typed element
                            // accepts a concrete implementor with the normal
                            // interface conversion (object address + vtable),
                            // exactly like `var i: Shape = <value>`; an
                            // interface-valued element keeps the existing
                            // fat-pointer copy below.
                            if (elemType.isInterface &&
                                !resolveInterfaceNominal(lastExprType_).isInterface) {
                                emitInterfaceStoreAt(
                                    elemType, listElementAddress(addr, ei, elemSize),
                                    lastValue_, lastExprType_, varType.isVolatile);
                                continue;
                            }
                            // CID-003: elements store at the declared width
                            // (an f32 literal in an f64 element must not
                            // write 4 bytes; an int element converts).
                            IRValue eval = convertValueToType(lastValue_, elemType);
                            // CIDER-BUG-133: both flags flow into the copy —
                            // the declared array's volatile target and the
                            // element expression's own volatile source
                            // (e.g. a member of a volatile enclosing object).
                            emitListElementStore(addr, (int64_t)(ei * (size_t)elemSize), eval,
                                                 elemType, elemSize, elemIsAggregate,
                                                 varType.isVolatile, lastExprType_.isVolatile);
                        }
                        lastExprType_ = sym->type;
                        return;
                    }
                }
                stmt.getInitializer()->accept(*this);
                IRValue val = lastValue_;
                // CIDER-BUG-133: remember the initializer's own volatile
                // flag before the target-specific conversions below can
                // overwrite lastExprType_ (a slice source is replaced by the
                // declared array type at CIDER-BUG-057's step). An aggregate
                // copy reads this flag into its MEMCPY.
                const bool initIsVolatile = lastExprType_.isVolatile;
                // CIDER-BUG-063: a legacy `func(...)`-typed declaration
                // initialized with a named function stores a raw code
                // pointer. Retype the symbol to the plain fn value (same
                // signature, closure marker cleared), so this variable's
                // calls take the typed indirect path; an initializer that
                // really is a closure object keeps the closure marker.
                // CIDER-BUG-068: the mirror direction. A `fn(...)`-typed
                // declaration initialized with a closure-valued source
                // (literal, closure-returning call, or closure-typed
                // variable/parameter) stores the closure object pointer, so
                // retype the symbol to the closure representation and calls
                // through it take the closure path. Both directions require
                // an fn-typed source; the checker has already enforced the
                // exact signature/ABI match.
                TypeInfo declaredFn = resolveFunctionPointerAlias(sym->type);
                TypeInfo initFn = resolveFunctionPointerAlias(lastExprType_);
                if (declaredFn.isFunctionPointer && !declaredFn.isArray &&
                    initFn.isFunctionPointer && !initFn.isArray &&
                    declaredFn.isClosure != initFn.isClosure) {
                    sym->type = lastExprType_;
                }
                // CIDER-BUG-057: `var s: [T; N] = base[start:end]` copies
                // the slice's elements. Use the descriptor's data pointer so
                // the MEMCPY below starts at base+start; the copy length is
                // the target's element count (the type checker rejects a
                // statically-known mismatch and accepts a dynamic length).
                // CIDER-BUG-071: declArray (resolved above) also covers the
                // alias spelling, so the descriptor pointer is followed by
                // the aggregate MEMCPY rather than stored as a scalar.
                if (lastExprType_.isSlice && lastExprType_.pointerLevel == 0 &&
                    declArray.isArray && declArray.pointerLevel == 0) {
                    val = sliceDataPointer(val);
                    lastExprType_ = declArray;
                }
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
                    // CIDER-BUG-133: an aggregate copy is observable when
                    // EITHER end is volatile — the target's declared flag
                    // (CIDER-BUG-061/092; alias-resolved, so `type V =
                    // volatile S` marks too) or the source's flag, which may
                    // be inferred from a volatile enclosing object (a member
                    // read) rather than spelled on the initializer's type.
                    currentBlock_->addInstruction({IROp::MEMCPY, std::nullopt, {addr, val, IRValue::Constant(getTypeSize(sym->type), IRType::I32)},
                                                   varType.isVolatile || initIsVolatile});
                    // CIDER-BUG-089: a copy into a class value re-stamps the
                    // declared class's vptr, so `var v: V = *p;` slices to V
                    // even when the pointee's dynamic type is derived.
                    // CIDER-BUG-133: a volatile destination's vptr stamp is
                    // an observable write too.
                    emitClassVptrStoreForType(varType, addr, currentModule_, varType.isVolatile);
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
                    // CIDER-BUG-061: initialization of a volatile local is
                    // itself an observable write. CIDER-BUG-133: the flag
                    // comes from the alias-resolved declared type, so
                    // `type VI = volatile int; var x: VI = ...` marks too.
                    currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, val, IRValue::Constant(0, IRType::I32)},
                                                   varType.isVolatile});
                }
            }
        }
    }

    int getTypeSize(TypeInfo type, const std::string& defModule = "") {
        if (type.pointerLevel > 0) return 8;
        if (type.isArray) {
            // CIDER-BUG-052: elementType() yields the inner dimension for a
            // nested fixed array, so the product covers every dimension.
            return type.arraySize * getTypeSize(type.elementType(), defModule);
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
        if (!sym && !defModule.empty()) sym = resolveNominalInModule(type.name, defModule);
        // CIDER-FEAT-002: an enum value is its int-sized underlying
        // integer, never the legacy 8-byte unknown-nominal default.
        if (sym && sym->kind == SymbolKind::ENUM) return 4;
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

    // CIDER-BUG-135: address of element `index` in a list-literal target
    // buffer. Element 0 reuses the base — every caller passes a register or
    // an already-materialised global label, the shapes the interface store
    // accepts — and the rest form base + index*elemSize, exactly the offset
    // the element-store path computes for itself.
    IRValue listElementAddress(IRValue base, size_t index, int elemSize) {
        if (index == 0) return base;
        IRValue addr = IRValue::Register(nextTemp(), IRType::PTR);
        currentBlock_->addInstruction(
            {IROp::ADD, addr,
             {base, IRValue::Constant((int64_t)(index * (size_t)elemSize), IRType::I32)}});
        return addr;
    }

    // CIDER-BUG-104: store one element of a list-literal buffer. Scalar
    // element values keep the legacy scalar STORE; a by-value aggregate
    // (array, struct/class or a wider-than-a-word value such as an
    // interface) is presented as its source ADDRESS, so the slot must
    // MEMCPY the element bytes. The shared rule keeps the declaration and
    // expression list paths from diverging (mirrors CIDER-BUG-083's
    // element-store classification).
    bool isListAggregateElement(const TypeInfo& t) {
        TypeInfo r = resolveTypeAlias(t);
        // Slices evaluate to their descriptor pointer — a scalar — even when
        // the element type is a struct, so they are never MEMCPY'd.
        if (r.pointerLevel > 0 || r.isSlice) return false;
        return r.isArray || isStructValueType(r) || getTypeSize(r) > 8;
    }

    // CIDER-BUG-163: materialise a (possibly nested) fixed-array list
    // literal into a stack temp shaped by the DECLARED array type. The flat
    // rule (CIDER-BUG-104/112) stores one element per slot; an element that
    // is itself a list literal for a fixed-array row recurses against the
    // declared row shape and is copied through emitListElementStore as an
    // aggregate. Without this, visit(ListExpr)'s context-free inference
    // typed the inner literal `*T` and the slot received a pointer word
    // instead of the row bytes. Returns the temp base; sets lastExprType_ to
    // the declared shape.
    IRValue emitListLiteralTemp(ListExpr& list, TypeInfo arrayType, bool targetIsVolatile) {
        auto& elems = list.getElements();
        TypeInfo elemType = resolveTypeAlias(arrayType.elementType());
        // CIDER-BUG-135: an interface-typed row element is the 16-byte fat
        // pointer, exactly like a non-list interface target.
        elemType = resolveInterfaceNominal(elemType);
        int elemSize = getTypeSize(elemType);
        bool elemIsAggregate = isListAggregateElement(elemType);
        IRValue addr = IRValue::Register(nextTemp(), IRType::PTR);
        currentBlock_->addInstruction(
            {IROp::ALLOCA, addr,
             {IRValue::Constant(elemSize * (int)elems.size(), IRType::I32)}});
        for (size_t ei = 0; ei < elems.size(); ei++) {
            if (auto* inner = dynamic_cast<ListExpr*>(elems[ei].get());
                inner && elemType.isArray && elemType.pointerLevel == 0 &&
                elemType.arraySize > 0) {
                IRValue innerAddr = emitListLiteralTemp(*inner, elemType, targetIsVolatile);
                emitListElementStore(addr, (int64_t)(ei * (size_t)elemSize), innerAddr,
                                     elemType, elemSize, true, targetIsVolatile, false);
                continue;
            }
            elems[ei]->accept(*this);
            // CIDER-BUG-135: a concrete implementor element of an
            // interface-typed row converts through the shared interface
            // store (object address + vtable), exactly like the declaration
            // path; an interface-valued element keeps the fat-pointer copy.
            if (elemType.isInterface &&
                !resolveInterfaceNominal(lastExprType_).isInterface) {
                emitInterfaceStoreAt(elemType, listElementAddress(addr, ei, elemSize),
                                     lastValue_, lastExprType_, targetIsVolatile);
                continue;
            }
            IRValue eval = convertValueToType(lastValue_, elemType);
            emitListElementStore(addr, (int64_t)(ei * (size_t)elemSize), eval,
                                 elemType, elemSize, elemIsAggregate,
                                 targetIsVolatile, lastExprType_.isVolatile);
        }
        lastExprType_ = arrayType;
        return addr;
    }

    void emitListElementStore(IRValue base, int64_t offset, IRValue val,
                              const TypeInfo& elemType, int elemSize, bool aggregate,
                              bool isVolatile = false, bool sourceIsVolatile = false) {
        if (!aggregate) {
            currentBlock_->addInstruction({IROp::STORE, std::nullopt,
                                           {base, val, IRValue::Constant(offset, IRType::I32)},
                                           isVolatile});
            return;
        }
        IRValue dst = IRValue::Register(nextTemp(), IRType::PTR);
        currentBlock_->addInstruction({IROp::ADD, dst, {base, IRValue::Constant(offset, IRType::I32)}});
        // CIDER-BUG-133: the MEMCPY is an aggregate copy, so either end's
        // volatile flag makes it an observable access (the read when the
        // element VALUE comes from a volatile source, CIDER-BUG-061's twin
        // of the interface rule in emitInterfaceStoreAt).
        currentBlock_->addInstruction({IROp::MEMCPY, std::nullopt,
                                       {dst, val, IRValue::Constant(elemSize, IRType::I32)},
                                       isVolatile || sourceIsVolatile});
        // CIDER-BUG-089: a class-valued element is a declared class slot,
        // so an explicit copy (`[*p]`) re-stamps the element's class vptr;
        // CIDER-BUG-133: a volatile destination makes that stamp volatile.
        emitClassVptrStoreForType(elemType, dst, currentModule_, isVolatile);
    }

    // CIDER-BUG-080: materialise a call-site interface argument. An
    // interface argument already evaluates to the address of its 16-byte
    // fat pointer (declaration/reassignment/parameter materialisation), so
    // it passes through. A satisfying concrete value or pointer evaluates
    // to the object's address; build the fat pointer in a caller-frame
    // temp — data at +0, the interface/concrete vtable at +8 — and pass
    // that address, exactly the shape emitInterfaceStore materialises for
    // `var i: I = <value>` (without re-evaluating the argument, whose code
    // is already emitted).
    IRValue materializeInterfaceArg(const TypeInfo& ifaceType, const TypeInfo& argType,
                                    IRValue val) {
        if (resolveInterfaceNominal(argType).isInterface) return val;
        IRValue slot = IRValue::Register(nextTemp(), IRType::PTR);
        currentBlock_->addInstruction({IROp::ALLOCA, slot, {IRValue::Constant(16, IRType::I32)}});
        currentBlock_->addInstruction({IROp::STORE, std::nullopt,
                                       {slot, val, IRValue::Constant(0, IRType::I32)}});
        IRValue slot8 = IRValue::Register(nextTemp(), IRType::PTR);
        currentBlock_->addInstruction({IROp::ADD, slot8, {slot, IRValue::Constant(8, IRType::I32)}});
        std::string concreteName = argType.name;
        if (argType.pointerLevel > 0) {
            TypeInfo base = argType;
            base.pointerLevel--;
            concreteName = base.name;
        }
        currentBlock_->addInstruction({IROp::STORE, std::nullopt,
                                       {slot8, IRValue::Label(interfaceVtableLabel(ifaceType.name, concreteName)),
                                        IRValue::Constant(0, IRType::I32)}});
        return slot;
    }

    // CIDER-BUG-080: a bare interface spelling in a callee signature
    // belongs to the callee's defining module, which the caller may not
    // import. Root it there before the vtable-label lookup; an unrooted
    // bare name would fall back to the legacy label and the interface call
    // would fail at the first dispatch. Non-interface parameters are
    // returned unchanged.
    TypeInfo callInterfaceParamType(TypeInfo t, const std::string& defMod) {
        return resolveInterfaceNominal(canonicalizeNominalInModule(t, defMod));
    }

    std::vector<TypeInfo> rootCallParams(const std::vector<TypeInfo>& params,
                                         const std::string& defMod) {
        std::vector<TypeInfo> out;
        out.reserve(params.size());
        for (const auto& pt : params) out.push_back(callInterfaceParamType(pt, defMod));
        return out;
    }

    // CIDER-BUG-165: evaluate one call argument against its declared
    // parameter shape. A list literal passed to a fixed-array parameter
    // must be materialised against that DECLARED shape (CIDER-BUG-163's
    // emitListLiteralTemp), exactly like a declaration or assignment
    // target; letting visit(ListExpr) infer the context-free flat `*T`
    // shape stored pointer words for nested rows and word-sized elements
    // for narrow ones. The materialisation temp is caller storage, so its
    // element stores carry no volatile flag: a volatile parameter is
    // marked by the callee's own parameter copy (CIDER-BUG-101), and a
    // volatile element SOURCE still marks its read inside
    // emitListLiteralTemp. A null paramType (an unresolved indirect
    // callee) keeps the legacy expression evaluation.
    IRValue evaluateCallArg(Expression* arg, const TypeInfo* paramType) {
        if (paramType) {
            TypeInfo paramArray = resolveTypeAlias(*paramType);
            if (auto* list = dynamic_cast<ListExpr*>(arg);
                list && paramArray.isArray && paramArray.pointerLevel == 0 &&
                paramArray.arraySize > 0) {
                return emitListLiteralTemp(*list, paramArray, false);
            }
        }
        arg->accept(*this);
        return lastValue_;
    }

    // CID-003: convert call arguments to the callee's declared parameter
    // types. `argBase` skips leading call-construction entries (an
    // interface call's method address and data pointer); `paramBase`
    // skips a hidden leading parameter (a method's `this`).
    // CIDER-BUG-080: `argTypes`, when given, holds each evaluated
    // argument's type aligned with `args[argBase..]`; an interface
    // parameter then materialises a fat pointer for a concrete argument
    // instead of passing the raw object address.
    // CIDER-BUG-133 (investigated, documented decision): a by-value
    // aggregate/interface argument is passed as its source ADDRESS and the
    // CALLEE materialises it from the callee parameter's own volatile flag
    // (CIDER-BUG-101). A volatile caller-side source therefore copies
    // plainly inside a plain parameter. That is by design: the callee is
    // shared by every call site, IR CALL carries no per-argument
    // volatility, and inserting a caller-side temp copy would change the
    // calling shape rather than mark an access ir_gen already emits. The
    // volatile parameter spelling (`p: volatile S`) is the supported way to
    // mark the callee-side copy; see the parameter materialisation in
    // visit(FunctionDeclStmt).
    void convertCallArgs(std::vector<IRValue>& args, size_t argBase,
                         const std::vector<TypeInfo>& paramTypes,
                         size_t paramBase = 0,
                         const std::vector<TypeInfo>* argTypes = nullptr) {
        for (size_t i = 0; argBase + i < args.size() && paramBase + i < paramTypes.size(); ++i) {
            TypeInfo paramT = resolveInterfaceNominal(paramTypes[paramBase + i]);
            if (paramT.isInterface && argTypes && i < argTypes->size()) {
                args[argBase + i] = materializeInterfaceArg(paramT, (*argTypes)[i], args[argBase + i]);
            } else {
                args[argBase + i] = convertValueToType(args[argBase + i], paramT);
            }
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
            // CIDER-BUG-137: the short-circuit RHS evaluates only on this
            // path, so its temporaries must finalize here — before the merge
            // — or a scope-exit destructor would read a slot the other path
            // never wrote.
            deferStack_.push_back({});
            destructorStack_.push_back({});
            expr.getRight()->accept(*this);
            IRValue rightVal = lastValue_;
            TypeInfo rightType = lastExprType_;
            emitDestructors(destructorStack_.size() - 1);
            destructorStack_.pop_back();
            deferStack_.pop_back();
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
            // CIDER-BUG-137: same per-path temp scope as `&&`.
            deferStack_.push_back({});
            destructorStack_.push_back({});
            expr.getRight()->accept(*this);
            IRValue rightVal = lastValue_;
            TypeInfo rightType = lastExprType_;
            emitDestructors(destructorStack_.size() - 1);
            destructorStack_.pop_back();
            deferStack_.pop_back();
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

        // CIDER-BUG-056: `==`/`!=` between two `string` values compares
        // the pointed-to bytes, not the addresses. `string` is the one
        // type with string semantics (TypeInfo name "string", pointer
        // level 0); explicit pointer types (`*u8`/`char*`, nilPtr) keep
        // identity comparison, so a cast stays the escape hatch for
        // pointer identity. Identical literal spellings are the same
        // bytes and fold here; everything else calls the _streq helper.
        // CIDER-ISS-012 extends the same split to `<`/`>`/`<=`/`>=`.
        bool bothStrings = (leftType.name == "string" && leftType.pointerLevel == 0 &&
                             rightType.name == "string" && rightType.pointerLevel == 0);
        if (bothStrings && (sOp == "==" || sOp == "!=")) {
            auto isStringLiteral = [](const IRValue& v) {
                return v.kind == IRValue::CONSTANT && v.type == IRType::PTR &&
                       !v.name.empty() && v.name.front() == '"';
            };
            if (isStringLiteral(left) && isStringLiteral(right) &&
                left.name == right.name) {
                lastValue_ = IRValue::Constant((sOp == "==") ? 1 : 0, IRType::BOOL);
            } else {
                ensureStringEqualityHelpers();
                IRValue eq = emitCall("_streq", {left, right});
                IRValue dest = IRValue::Register(nextTemp(), IRType::BOOL);
                currentBlock_->addInstruction({sOp == "==" ? IROp::CMP_NE : IROp::CMP_EQ,
                                               dest, {eq, IRValue::Constant(0, IRType::I32)}});
                lastValue_ = dest;
            }
            lastExprType_ = {"bool", 0};
            return;
        }

        // CIDER-ISS-012: ordered comparisons between two `string` values
        // compare contents lexicographically (strcmp sign), not addresses.
        // The `_strcmp` helper is plain IR built once per module — like
        // _streq — so the VM (optimized and unoptimized) and the native
        // backend run identical code. nil sorts before every non-nil
        // string and is never dereferenced. Explicit pointer types keep
        // address ordering; identical literal spellings fold to the known
        // equality result so a common case needs no helper call.
        if (bothStrings && (sOp == "<" || sOp == ">" || sOp == "<=" || sOp == ">=")) {
            auto isStringLiteral = [](const IRValue& v) {
                return v.kind == IRValue::CONSTANT && v.type == IRType::PTR &&
                       !v.name.empty() && v.name.front() == '"';
            };
            if (isStringLiteral(left) && isStringLiteral(right) &&
                left.name == right.name) {
                bool le = (sOp == "<=" || sOp == ">=");
                lastValue_ = IRValue::Constant(le ? 1 : 0, IRType::BOOL);
            } else {
                ensureStringComparisonHelpers();
                IRValue cmp = emitCall("_strcmp", {left, right}, IRType::I32);
                IROp cmpOp = (sOp == "<") ? IROp::CMP_LT
                           : (sOp == ">") ? IROp::CMP_GT
                           : (sOp == "<=") ? IROp::CMP_LE : IROp::CMP_GE;
                IRValue dest = IRValue::Register(nextTemp(), IRType::BOOL);
                currentBlock_->addInstruction({cmpOp, dest,
                                               {cmp, IRValue::Constant(0, IRType::I32)}});
                lastValue_ = dest;
            }
            lastExprType_ = {"bool", 0};
            return;
        }

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
        // CIDER-BUG-061: a `const volatile` symbol (a read-only MMIO
        // register) must still be read at each use — never folded.
        // CIDER-BUG-133: resolve alias chains first, so a volatile
        // declaration through an alias (`type VI = volatile int`) is not
        // folded away.
        if (sym && sym->isConst && !resolveTypeAlias(sym->type).isVolatile &&
            !globalValues_.count(globalRefName(sym, expr.getName()))) {
            IRType irType = (sym->type.pointerLevel > 0) ? IRType::PTR : mapType(sym->type.name);
            lastValue_ = IRValue::Constant(sym->constValue, irType);
            return;
        }

        if (sym && sym->kind == SymbolKind::FUNCTION) {
            // CID-020/021 (spec §5): function values label the mangled
            // definition name. CID-006: the value's type is the fn
            // signature (labels and mangling are unchanged).
            lastValue_ = IRValue::Label(emitFunctionName(definingModuleFor(sym, expr.getName()),
                                                          sym->name, sym->isExtern));
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
                // CIDER-BUG-061: reading a volatile local is observable.
                currentBlock_->addInstruction({IROp::LOAD, dest, {addr, IRValue::Constant(0, IRType::I32)},
                                               lastExprType_.isVolatile});
                lastValue_ = dest;
            }
        } else if (globalValues_.count(globalRefName(sym, expr.getName()))) {
            lastValue_ = globalValues_[globalRefName(sym, expr.getName())];
            if (!isAddress_) {
                IRType irType = (lastExprType_.pointerLevel > 0) ? IRType::PTR : mapType(lastExprType_.name);
                IRValue dest = IRValue::Register(nextTemp(), irType);
                // CIDER-BUG-061: reading a volatile global is observable.
                currentBlock_->addInstruction({IROp::LOAD, dest, {lastValue_},
                                               lastExprType_.isVolatile});
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
            if (sym) {
                // CIDER-BUG-015/019/052: an alias-typed value presents its
                // resolved shape (visit(IdentifierExpr) does the same), so
                // chained indexing and assignment-target typing see the real
                // array/struct shape instead of the alias spelling.
                return resolveTypeAlias(sym->type);
            }
        } else if (auto mem = dynamic_cast<MemberAccessExpr*>(expr)) {
            // CID-020/021 §3: `mod.g` names a module-scope global; resolve
            // its declared type from the module's own scope (the same symbol
            // visit(MemberAccessExpr) reads). Without this, a field of a
            // module-qualified global struct has no inferred type, so
            // CIDER-BUG-069's interface-target detection cannot see the
            // fat pointer and the store would write only the data word.
            if (!mem->isArrow()) {
                if (auto base = dynamic_cast<IdentifierExpr*>(mem->getObject())) {
                    auto baseSym = symbolTable_.resolve(base->getName());
                    if (baseSym && baseSym->kind == SymbolKind::MODULE) {
                        auto modSym = std::static_pointer_cast<ModuleSymbol>(baseSym);
                        auto target = modSym->scope ? modSym->scope->resolveLocal(mem->getMember())
                                                    : nullptr;
                        if (target && target->kind == SymbolKind::VARIABLE) {
                            std::string memberName = mem->getMember();
                            std::string defMod = definingModuleFor(target, memberName);
                            TypeInfo t = canonicalizeSymbolType(target, memberName);
                            if (t.pointerLevel == 0) t = resolveTypeAlias(t, defMod);
                            return resolveInterfaceNominal(t);
                        }
                        return TypeInfo{};
                    }
                }
            }
            TypeInfo objType = inferExprType(mem->getObject());
            if (objType.pointerLevel > 0) objType.pointerLevel--;
            // CIDER-BUG-015: alias-safe struct lookup and field typing.
            auto sym = resolveStructOrClassSymbol(objType);
            if (sym) {
                auto ss = std::static_pointer_cast<StructSymbol>(sym);
                std::string ownerMod = definingModuleFor(sym, ss->name);
                for (const auto& f : ss->fields) {
                    if (f.name == mem->getMember()) {
                        // CIDER-BUG-090: resolve in the declaring class's
                        // module (inherited fields keep theirs).
                        std::string fieldMod = fieldTypeModule(f, ownerMod);
                        TypeInfo fieldType =
                            canonicalizeNominalInModule(resolveTypeAlias(f.type, fieldMod), fieldMod);
                        // CIDER-BUG-133: every member of a volatile object
                        // is volatile. Present the enclosing flag on the
                        // inferred field type, so target classification of
                        // NESTED member accesses (`o.inner.shape = ...`)
                        // sees the volatility that the direct store paths
                        // already honour for one level.
                        if (objType.isVolatile) fieldType.isVolatile = true;
                        return fieldType;
                    }
                }
            }
        } else if (auto idx = dynamic_cast<IndexExpr*>(expr)) {
            // Array/pointer element type (Issue #22): without this,
            // chained indexing like g_tasks[0].name[i] resolved to an
            // empty type, so the store path "widened" the char value to
            // a pointer width and emitted mov qword over the neighbors.
            // CIDER-BUG-052: elementType() keeps the inner dimension of a
            // nested fixed array.
            TypeInfo baseType = inferExprType(idx->getObject());
            if (baseType.isArray) {
                baseType = baseType.elementType();
            } else if (baseType.pointerLevel > 0) {
                baseType.pointerLevel--;
            } else if (baseType.name == "string") {
                baseType.name = "char";
            }
            // CIDER-BUG-052: an element alias resolves to its real shape.
            return resolveTypeAlias(baseType);
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

    // CIDER-BUG-069: interface-typed fields/elements carry the interface
    // flag once the collector resolves it; recover it by name for shapes
    // that reach IR generation without it (an array element stripped from a
    // bare element name, an alias spelling), mirroring the declaration-time
    // resolution so stores, reads and dispatch agree. Arrays are never
    // interface values themselves: only scalar elements are flagged.
    // CIDER-BUG-070: the name goes through resolveNominalType, so
    // module-qualified spellings (`lib.Shape`) recover the flag too.
    // CIDER-BUG-078: resolve `type S = Shape` chains first, so an alias
    // spelling presents the same 16-byte fat pointer as the direct name.
    TypeInfo resolveInterfaceNominal(TypeInfo t) {
        if (!t.isInterface && t.pointerLevel == 0 && !t.isArray) {
            TypeInfo resolved = resolveTypeAlias(t);
            // CIDER-BUG-112: an alias spelling can resolve to an aggregate
            // whose innermost element names an interface (`type Shapes2 =
            // [Shape; 2]`). Only the scalar shape is the 16-byte fat
            // pointer; flagging the aggregate spelling would size it as one
            // interface and route its copies through emitInterfaceStore.
            if (resolved.pointerLevel == 0 && !resolved.isArray) {
                auto typeSym = resolveNominalType(resolved.name);
                if (typeSym && typeSym->kind == SymbolKind::INTERFACE) t.isInterface = true;
            }
        }
        return t;
    }

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
                // CIDER-BUG-090: an inherited field resolves in its own
                // declaring module.
                std::string fieldMod = fieldTypeModule(f, ownerMod);
                out = canonicalizeNominalInModule(resolveTypeAlias(f.type, fieldMod), fieldMod);
                // CIDER-BUG-069: present an interface field as the 16-byte
                // fat pointer it is (name fallback for unflagged shapes).
                out = resolveInterfaceNominal(out);
                return true;
            }
        }
        return false;
    }

    // CIDER-BUG-061: a member access is volatile when the member itself is
    // `volatile`-qualified or when the object it sits in is (every member of
    // a volatile object is volatile, as in C). Module-qualified globals
    // (`mod.reg`) resolve through the module scope.
    bool memberAccessIsVolatile(MemberAccessExpr* mem) {
        if (!mem) return false;
        if (auto base = dynamic_cast<IdentifierExpr*>(mem->getObject())) {
            auto baseSym = symbolTable_.resolve(base->getName());
            if (baseSym && baseSym->kind == SymbolKind::MODULE) {
                auto modSym = std::static_pointer_cast<ModuleSymbol>(baseSym);
                auto target = modSym->scope ? modSym->scope->resolveLocal(mem->getMember())
                                            : nullptr;
                if (target && resolveTypeAlias(target->type).isVolatile) return true;
            }
        }
        TypeInfo objType = inferExprType(mem->getObject());
        if (objType.pointerLevel > 0) objType.pointerLevel--;
        if (resolveTypeAlias(objType).isVolatile) return true;
        TypeInfo fieldType;
        return getMemberFieldType(mem, fieldType) && fieldType.isVolatile;
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
            // CIDER-FEAT-002: `Enum.VARIANT` / `lib.Enum.VARIANT` folds to
            // the variant's integer value (the qualifier is a type, not a
            // value). A module-qualified bare variant (`lib.RED`) is an
            // ordinary const symbol and folds through the path below.
            if (auto enumSym = resolveEnumQualifierSymbol(mem->getObject(), /*constContext=*/true)) {
                int64_t enumValue = 0;
                if (std::static_pointer_cast<EnumSymbol>(enumSym)->findVariant(mem->getMember(), enumValue)) {
                    out = (uint64_t)enumValue;
                    return true;
                }
            }
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

    IRValue emitBitfieldLoad(IRValue addr, int bitOffset, int bitWidth, bool isSigned,
                             bool isVolatile = false) {
        IRValue word = IRValue::Register(nextTemp(), IRType::I32);
        currentBlock_->addInstruction({IROp::LOAD, word, {addr, IRValue::Constant(0, IRType::I32)},
                                       isVolatile});
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

    void emitBitfieldStore(IRValue addr, IRValue val, int bitOffset, int bitWidth,
                           bool isVolatile = false) {
        IRValue word = IRValue::Register(nextTemp(), IRType::I32);
        currentBlock_->addInstruction({IROp::LOAD, word, {addr, IRValue::Constant(0, IRType::I32)},
                                       isVolatile});
        IRValue vm = IRValue::Register(nextTemp(), IRType::I32);
        currentBlock_->addInstruction({IROp::BIT_AND, vm, {val, IRValue::Constant(bitfieldMask(bitWidth), IRType::I32)}});
        IRValue shifted = IRValue::Register(nextTemp(), IRType::I32);
        currentBlock_->addInstruction({IROp::LSHIFT, shifted, {vm, IRValue::Constant(bitOffset, IRType::I32)}});
        IRValue cleared = IRValue::Register(nextTemp(), IRType::I32);
        int64_t clearMask = ~(bitfieldMask(bitWidth) << bitOffset);
        currentBlock_->addInstruction({IROp::BIT_AND, cleared, {word, IRValue::Constant(clearMask, IRType::I32)}});
        IRValue nw = IRValue::Register(nextTemp(), IRType::I32);
        currentBlock_->addInstruction({IROp::BIT_OR, nw, {cleared, shifted}});
        currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, nw, IRValue::Constant(0, IRType::I32)},
                                       isVolatile});
    }

    // CIDER-BUG-057: a slice value is a pointer to a {dataPtr, len}
    // descriptor (see visit(SliceExpr)). Copying a slice into a fixed array
    // must read the elements through the descriptor's data pointer at
    // offset 0 — copying the descriptor's own bytes would copy the start
    // address word instead of the elements.
    IRValue sliceDataPointer(IRValue sliceDesc) {
        IRValue dataPtr = IRValue::Register(nextTemp(), IRType::PTR);
        currentBlock_->addInstruction({IROp::LOAD, dataPtr, {sliceDesc, IRValue::Constant(0, IRType::I32)}});
        return dataPtr;
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

        // CIDER-BUG-061: whether this assignment writes a volatile object.
        // Each target shape reports the object being written: an identifier
        // its declared type, a deref its pointee, an index its element, a
        // member its field (or a volatile enclosing object), a module-global
        // member its symbol.
        bool targetIsVolatile = false;
        if (auto ident = dynamic_cast<IdentifierExpr*>(stmt.getTarget())) {
            auto sym = symbolTable_.resolve(ident->getName());
            if (sym) targetIsVolatile = resolveTypeAlias(sym->type).isVolatile;
        } else if (auto mem = dynamic_cast<MemberAccessExpr*>(stmt.getTarget())) {
            targetIsVolatile = memberAccessIsVolatile(mem);
        } else if (auto idx = dynamic_cast<IndexExpr*>(stmt.getTarget())) {
            TypeInfo elemType = inferExprType(idx->getObject());
            if (elemType.isArray) {
                elemType = elemType.elementType();
            } else if (elemType.pointerLevel > 0) {
                elemType.pointerLevel--;
            } else if (elemType.name == "string") {
                elemType.name = "char";
            }
            targetIsVolatile = resolveTypeAlias(elemType).isVolatile;
        } else if (hasDerefTarget) {
            targetIsVolatile = derefPointeeType.isVolatile;
        } else {
            targetIsVolatile = inferExprType(stmt.getTarget()).isVolatile;
        }

        if (compoundOp.empty()) {
            // CIDER-BUG-054: an interface variable target stores the fat
            // pointer (data + vtable) through the same helper the
            // declaration path uses, so reassignment dispatches exactly
            // like a declaration-time assignment.
            // CIDER-BUG-069: interface-typed fields and elements are the
            // same 16-byte fat pointer (the collector sizes them that way
            // and the read paths present their address for dispatch), so
            // `s.field = v` and `a[i] = v` take the same store path.
            TypeInfo targetIface;
            bool haveInterfaceTarget = false;
            if (auto ident = dynamic_cast<IdentifierExpr*>(stmt.getTarget())) {
                auto sym = symbolTable_.resolve(ident->getName());
                if (sym) {
                    targetIface = sym->type;
                    haveInterfaceTarget = true;
                }
            } else if (auto mem = dynamic_cast<MemberAccessExpr*>(stmt.getTarget())) {
                haveInterfaceTarget = getMemberFieldType(mem, targetIface);
            } else if (auto idx = dynamic_cast<IndexExpr*>(stmt.getTarget())) {
                TypeInfo baseType = inferExprType(idx->getObject());
                if (baseType.isArray) {
                    targetIface = baseType.elementType();
                } else if (baseType.pointerLevel > 0) {
                    targetIface = baseType;
                    targetIface.pointerLevel--;
                } else if (baseType.name == "string") {
                    targetIface = {"char", 0};
                }
                haveInterfaceTarget = true;
            } else if (hasDerefTarget) {
                // CIDER-BUG-100: `*p` through a pointer-to-interface names
                // the same 16-byte fat-pointer slot `p[i]` does, so it
                // takes the shared interface store too. derefPointeeType is
                // the declared pointee, so a `volatile Shape` pointee keeps
                // its volatile flag for the two target stores (CIDER-BUG-079).
                targetIface = derefPointeeType;
                haveInterfaceTarget = true;
            }
            if (haveInterfaceTarget) {
                targetIface = resolveInterfaceNominal(targetIface);
                if (targetIface.isInterface) {
                    emitInterfaceStore(targetIface, addr, stmt.getValue(),
                                       targetIsVolatile);
                    return;
                }
            }
            // CIDER-BUG-112: a list literal reassigned into an existing
            // fixed array (`s = [1,2,3]`, direct or alias spelling) mirrors
            // the declaration-time lowering: each element stores at its
            // declared width/offset, and aggregate elements (CIDER-BUG-104)
            // are copied through emitListElementStore. The literal is never
            // materialised as a temp buffer, exactly like a declaration.
            // A label target (a global) is materialised into a register
            // first: the label STORE path has no per-element offset. The
            // checker has already enforced the size and element types.
            if (auto list = dynamic_cast<ListExpr*>(stmt.getValue())) {
                TypeInfo assignArray = resolveTypeAlias(inferExprType(stmt.getTarget()));
                if (assignArray.isArray && assignArray.pointerLevel == 0 &&
                    assignArray.arraySize > 0) {
                    TypeInfo elemType = resolveTypeAlias(assignArray.elementType());
                    // CIDER-BUG-135: an interface-typed element is the
                    // 16-byte fat pointer, exactly like the declaration
                    // path; recover the flag for an unflagged element
                    // spelling.
                    elemType = resolveInterfaceNominal(elemType);
                    int elemSize = getTypeSize(elemType);
                    bool elemIsAggregate = isListAggregateElement(elemType);
                    IRValue storeBase = addr;
                    if (storeBase.kind == IRValue::LABEL) {
                        storeBase = IRValue::Register(nextTemp(), IRType::PTR);
                        currentBlock_->addInstruction({IROp::ADD, storeBase,
                                                       {addr, IRValue::Constant(0, IRType::I32)}});
                    }
                    auto& elems = list->getElements();
                    for (size_t ei = 0; ei < elems.size(); ei++) {
                        // CIDER-BUG-163: same nested-row materialisation as
                        // the declaration path, shared through
                        // emitListLiteralTemp.
                        if (auto* inner = dynamic_cast<ListExpr*>(elems[ei].get());
                            inner && elemType.isArray && elemType.pointerLevel == 0 &&
                            elemType.arraySize > 0) {
                            IRValue innerAddr = emitListLiteralTemp(*inner, elemType,
                                                                    targetIsVolatile);
                            emitListElementStore(storeBase, (int64_t)(ei * (size_t)elemSize),
                                                 innerAddr, elemType, elemSize, true,
                                                 targetIsVolatile, false);
                            continue;
                        }
                        elems[ei]->accept(*this);
                        // CIDER-BUG-135: a concrete implementor element of
                        // an interface-typed array converts through the
                        // shared interface store (object address + vtable),
                        // exactly like the declaration path and a non-list
                        // interface assignment; an interface-valued element
                        // keeps the existing fat-pointer copy.
                        if (elemType.isInterface &&
                            !resolveInterfaceNominal(lastExprType_).isInterface) {
                            emitInterfaceStoreAt(
                                elemType, listElementAddress(storeBase, ei, elemSize),
                                lastValue_, lastExprType_, targetIsVolatile);
                            continue;
                        }
                        // CID-003: elements store at the declared width,
                        // exactly like the declaration path.
                        IRValue eval = convertValueToType(lastValue_, elemType);
                        // CIDER-BUG-133: the element copy carries both the
                        // target array's flag and the element source's own
                        // flag (mirrors the declaration list path).
                        emitListElementStore(storeBase, (int64_t)(ei * (size_t)elemSize), eval,
                                             elemType, elemSize, elemIsAggregate,
                                             targetIsVolatile, lastExprType_.isVolatile);
                    }
                    lastExprType_ = assignArray;
                    lastValue_ = storeBase;
                    return;
                }
            }
            stmt.getValue()->accept(*this);
            IRValue val = lastValue_;
            // CIDER-BUG-057: remember that the RHS is a slice before the
            // target-specific conversions below overwrite lastExprType_.
            bool valueIsSlice = lastExprType_.isSlice && lastExprType_.pointerLevel == 0;
            // CIDER-BUG-133: remember the RHS's own volatile flag before the
            // target-specific branches overwrite lastExprType_ with the
            // TARGET type. An aggregate MEMCPY below reads this flag: the
            // copy is observable when the source is volatile even though the
            // target is not (e.g. `h.inner` of a volatile enclosing object).
            const bool valueIsVolatile = lastExprType_.isVolatile;

            // CIDER-BUG-116: classify the RHS value by its own RESOLVED
            // shape, not by flags the producing expression happened to set.
            // A by-value struct/class value of any size is an aggregate whose
            // value is already its source ADDRESS (locals, derefs, member
            // reads, ternaries and sret call results all present struct
            // values by address), so the store below must MEMCPY the value
            // bytes. A deref source (`*r`) carries no isUserDefined marker
            // (visit(UnaryExpr) only recovers the pointee spelling), so an
            // exactly-8-byte (or narrower) pointee missed both the flag and
            // the `getTypeSize(...) > 8` fallback: `*p = *r` lowered to a
            // scalar STORE of the source address instead of copying the
            // object. Marking the value here mirrors CIDER-BUG-081/083's
            // classification of member and element targets; the
            // target-specific branches below still take precedence for the
            // shapes they classify themselves.
            if (isStructValueType(lastExprType_)) lastExprType_.isUserDefined = true;

            // Bitfield targets use the read-modify-write sequence.
            if (auto mem = dynamic_cast<MemberAccessExpr*>(stmt.getTarget())) {
                int bitOffset = 0, bitWidth = 0;
                if (getMemberBitfield(mem, bitOffset, bitWidth)) {
                    emitBitfieldStore(addr, val, bitOffset, bitWidth, targetIsVolatile);
                    return;
                }
                TypeInfo fieldType;
                if (getMemberFieldType(mem, fieldType)) {
                    // CIDER-BUG-081: classify the field by its own RESOLVED
                    // shape, not by the RHS value's flags. A by-value struct/
                    // class field of any size is an aggregate target whose RHS
                    // value is already the source ADDRESS (locals, member/
                    // element reads, struct literals and sret call results all
                    // present struct values by address), so the store below
                    // must MEMCPY the value bytes. Marking the field here
                    // mirrors what visit(IdentifierExpr) does for struct
                    // values and what CIDER-BUG-083 does for element targets.
                    // Without it an exactly-8-byte (or narrower) struct field
                    // missed both the isUserDefined flag and the
                    // `getTypeSize(...) > 8` fallback, and the store lowered
                    // to a scalar STORE of the source address. The
                    // StructLiteralExpr field-init path takes the same rule.
                    if (isStructValueType(fieldType)) fieldType.isUserDefined = true;
                    // CID-003: the field's declared type drives the stored
                    // representation (int narrowing, int<->float, widths).
                    val = convertValueToType(val, fieldType);
                    lastExprType_ = fieldType;
                }
            } else if (auto idx = dynamic_cast<IndexExpr*>(stmt.getTarget())) {
                TypeInfo baseType = inferExprType(idx->getObject());
                TypeInfo elemType = baseType;
                if (elemType.isArray) {
                    elemType = elemType.elementType();
                } else if (elemType.pointerLevel > 0) {
                    elemType.pointerLevel--;
                } else if (elemType.name == "string") {
                    elemType.name = "char";
                }
                // CIDER-BUG-052: an element alias resolves to its real shape
                // (array elements need the MEMCPY decision below).
                elemType = resolveTypeAlias(elemType);
                // CIDER-BUG-083: classify the element by its own RESOLVED
                // shape, not by the RHS value's flags. A by-value struct/
                // class element of any size is an aggregate target whose RHS
                // value is already the source ADDRESS (locals, element reads,
                // struct fields and sret call results all present struct
                // values by address), so the store below must MEMCPY the
                // value bytes. Marking the element here mirrors what
                // visit(IdentifierExpr) does for struct values. Without it an
                // exactly-8-byte struct missed both the isUserDefined flag
                // and the `getTypeSize(...) > 8` fallback, and the store
                // lowered to a scalar STORE of the source address.
                if (isStructValueType(elemType)) elemType.isUserDefined = true;
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

            // CIDER-BUG-057: `arr = base[start:end]` copies the slice's
            // elements into the fixed array — resolve the descriptor's data
            // pointer first, then let the aggregate MEMCPY below copy the
            // target's element count (a slice-typed RHS is never a scalar
            // store). The declaration path uses the same rule.
            if (valueIsSlice) {
                TypeInfo assignTargetType = inferExprType(stmt.getTarget());
                if (assignTargetType.isArray && assignTargetType.pointerLevel == 0) {
                    val = sliceDataPointer(val);
                    lastExprType_ = assignTargetType;
                }
            }

            // CIDER-BUG-081/083 classified member and element aggregate
            // targets; CIDER-BUG-089 closes the identifier shape for virtual
            // class values: a value assigned through a deref (`v = *p`)
            // carries no isUserDefined marker, so an exactly-8-byte virtual
            // class (vptr only) fell to the scalar STORE and wrote the
            // source ADDRESS into the value. The added test is restricted to
            // virtual classes so every non-virtual aggregate keeps its
            // existing emission.
            bool rhsVirtualClass = virtualClassForType(lastExprType_, currentModule_) != nullptr;
            if (lastExprType_.pointerLevel == 0 &&
                (lastExprType_.isArray || lastExprType_.isUserDefined ||
                 rhsVirtualClass || getTypeSize(lastExprType_) > 8)) {
                // CIDER-BUG-133: either end's volatile flag marks the copy
                // (the source flag was captured before the target-specific
                // conversions above overwrote lastExprType_).
                currentBlock_->addInstruction({IROp::MEMCPY, std::nullopt, {addr, val, IRValue::Constant(getTypeSize(lastExprType_), IRType::I32)},
                                               targetIsVolatile || valueIsVolatile});
                // CIDER-BUG-089: `v = *p;` is a copy like a declaration
                // initializer, so the target's declared class re-stamps its
                // vptr (the checker admits aggregate assignments only
                // between equal static types, so the declared class is the
                // value's class).
                // CIDER-BUG-133: the stamp follows the DESTINATION's
                // volatile flag (the vptr label is constant, not a read).
                emitClassVptrStoreForType(lastExprType_, addr, currentModule_, targetIsVolatile);
            } else {
                if (addr.kind == IRValue::LABEL) {
                    currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, val}, targetIsVolatile});
                } else {
                    currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, val, IRValue::Constant(0, IRType::I32)},
                                                   targetIsVolatile});
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
                current = emitBitfieldLoad(addr, bitOffset, bitWidth, true, targetIsVolatile);
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
                        elemType = elemType.elementType();
                    } else if (elemType.pointerLevel > 0) {
                        elemType.pointerLevel--;
                    } else if (elemType.name == "string") {
                        elemType.name = "char";
                    }
                    // CIDER-BUG-052: resolve an element alias to its shape.
                    elemType = resolveTypeAlias(elemType);
                    targetIR = (elemType.pointerLevel > 0) ? IRType::PTR : mapType(elemType.name);
                } else if (hasDerefTarget) {
                    // CID-003: `*p += v` loads/stores at the pointee width.
                    targetIR = (derefPointeeType.pointerLevel > 0) ? IRType::PTR : mapType(derefPointeeType.name);
                }
                current = IRValue::Register(nextTemp(), targetIR);
                // CIDER-BUG-061: compound assignment reads the volatile
                // object before writing it back.
                if (addr.kind == IRValue::LABEL) {
                    currentBlock_->addInstruction({IROp::LOAD, current, {addr}, targetIsVolatile});
                } else {
                    currentBlock_->addInstruction({IROp::LOAD, current, {addr, IRValue::Constant(0, IRType::I32)},
                                                   targetIsVolatile});
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
                emitBitfieldStore(addr, result, bitOffset, bitWidth, targetIsVolatile);
                return;
            }
            if (widenedFloat && targetFloatWidth == IRType::F32) {
                // The f64 computation rounds once on the way back into the
                // f32 target slot.
                result = convertFloatWidth(result, IRType::F32);
            }
            if (addr.kind == IRValue::LABEL) {
                currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, result}, targetIsVolatile});
            } else {
                currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, result, IRValue::Constant(0, IRType::I32)},
                                               targetIsVolatile});
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
        for (auto& s : stmt.getThenBranch()) visitStatement(s.get());
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
        for (auto& s : stmt.getElseBranch()) visitStatement(s.get());
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
        // CIDER-BUG-137: the condition re-evaluates on every iteration, so
        // temporaries it creates (a class-valued call's sret slot) must
        // finalize at the end of each evaluation — registering them in the
        // enclosing scope would only reach the last iteration's slot. The
        // pushed scope is balanced before the body starts, so it never
        // affects unwindDepths_.
        deferStack_.push_back({});
        destructorStack_.push_back({});
        stmt.getCondition()->accept(*this);
        IRValue cond = lastValue_;
        emitDestructors(destructorStack_.size() - 1);
        destructorStack_.pop_back();
        deferStack_.pop_back();
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
        for (auto& s : stmt.getBody()) visitStatement(s.get());
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
        // CIDER-BUG-108: the header's declarations own the for statement's
        // scope, so their finalizers run at the for-scope exit (the exit
        // block below), not at the enclosing block's exit. deferStack_ moves
        // in lockstep (the header grammar only admits expressions, so these
        // entries stay empty) because unwindDepths_ indexes both stacks and
        // break/continue must stop above this scope, not inside it.
        deferStack_.push_back({});
        destructorStack_.push_back({});
        if (stmt.getInitializer()) stmt.getInitializer()->accept(*this);

        std::string headerName = nextLabel("for_header");
        std::string bodyName = nextLabel("for_body");
        std::string incName = nextLabel("for_inc");
        std::string exitName = nextLabel("for_exit");

        currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(headerName)}});

        startBlock(headerName);
        if (stmt.getCondition()) {
            // CIDER-BUG-137: per-evaluation temp scope, exactly like the
            // while header (the condition re-evaluates every iteration).
            deferStack_.push_back({});
            destructorStack_.push_back({});
            stmt.getCondition()->accept(*this);
            IRValue condVal = lastValue_;
            emitDestructors(destructorStack_.size() - 1);
            destructorStack_.pop_back();
            deferStack_.pop_back();
            currentBlock_->addInstruction({IROp::COND_BR, std::nullopt, {condVal, IRValue::Label(bodyName), IRValue::Label(exitName)}});
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
        for (auto& s : stmt.getBody()) visitStatement(s.get());
        if (currentBlock_ && (currentBlock_->getInstructions().empty() || (currentBlock_->getInstructions().back().op != IROp::BR && currentBlock_->getInstructions().back().op != IROp::RET))) {
            emitDefers(deferStack_.size() - 1);
            emitDestructors(destructorStack_.size() - 1);
            currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(incName)}});
        }

        startBlock(incName);
        // CIDER-BUG-137: per-evaluation temp scope, like the condition: the
        // increment runs on every iteration, so its temporaries finalize
        // right after each evaluation.
        deferStack_.push_back({});
        destructorStack_.push_back({});
        if (stmt.getIncrement()) stmt.getIncrement()->accept(*this);
        emitDestructors(destructorStack_.size() - 1);
        destructorStack_.pop_back();
        deferStack_.pop_back();
        currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(headerName)}});

        destructorStack_.pop_back();
        deferStack_.pop_back();
        unwindDepths_.pop_back();
        continueLabels_.pop_back();
        breakLabels_.pop_back();
        symbolTable_.exitScope();

        // CIDER-BUG-108: run the header scope's finalizers exactly once on
        // the way out (condition false or `break`), before the next
        // statement. `continue` jumps to the increment block inside the
        // loop, so the scope survives across iterations.
        startBlock(exitName);
        emitDefers(deferStack_.size() - 1);
        emitDestructors(destructorStack_.size() - 1);
        destructorStack_.pop_back();
        deferStack_.pop_back();
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
                } else if (auto sym = symbolTable_.resolve(name)) {
                    // CIDER-BUG-072: globals key by their escaped/mangled
                    // spelling, so look the address up the same way the
                    // reference above did.
                    auto git = globalValues_.find(globalRefName(sym, name));
                    if (git != globalValues_.end()) {
                        varAddr = git->second;
                        found = true;
                    }
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
            // CIDER-BUG-142: file-scope asm is user code/labels/data; keep it
            // in its own list so codegen can emit it in .text instead of the
            // .bss context used for compiler-generated global asm.
            module_.addUserAsm(stmt.getCode());
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
            // CIDER-BUG-052: one dimension; for a nested array the loop
            // variable is the inner array (by-value aggregate).
            elemType = elemType.elementType();
        }
        // CIDER-BUG-052: an element alias resolves to its real shape, or a
        // `[Row; N]` loop variable would be treated as a scalar slot.
        elemType = resolveTypeAlias(elemType);
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
            bool iterRetKnown = false;
            int iterStructSize = 8;
            TypeInfo iterRetType = iterableType;
            {
                auto iterSym = symbolTable_.resolve(iterFn);
                if (iterSym && iterSym->kind == SymbolKind::FUNCTION) {
                    auto fnSym = std::static_pointer_cast<FunctionSymbol>(iterSym);
                    if (isStructValueType(fnSym->returnType)) {
                        iterIsSret = true;
                        iterStructSize = getTypeSize(fnSym->returnType);
                        iterRetType = fnSym->returnType;
                        iterRetKnown = true;
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
                // CIDER-BUG-137: `iter()`'s sret slot is a caller-owned
                // temporary (the iterator object) that lives for the whole
                // loop; register its finalizers in the enclosing scope, so
                // it is destroyed after the last iteration like any other
                // returned temporary. Only a resolved return type may
                // register: the unresolved fallback sizes the slot from the
                // iterable, which is not the iterator's own type.
                if (iterRetKnown) registerTempDestructors(iterRetType, itBuf, currentModule_);
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
            // CIDER-BUG-133: the element type may be volatile (an array of
            // `volatile T`, or an iterator whose next() yields a volatile
            // pointee); the copy READS the iterable's storage, so it is
            // observable. The loop-variable slot itself is a fresh temp, so
            // only the source access carries the flag.
            if (elemType.pointerLevel == 0 &&
                (elemType.isArray || isStructValueType(elemType) || getTypeSize(elemType) > 8)) {
                currentBlock_->addInstruction({IROp::MEMCPY, std::nullopt, {varAddr, p, IRValue::Constant(getTypeSize(elemType), IRType::I32)},
                                               elemType.isVolatile});
            } else {
                IRType irType = (elemType.pointerLevel > 0) ? IRType::PTR : mapType(elemType.name);
                IRValue elemVal = IRValue::Register(nextTemp(), irType);
                currentBlock_->addInstruction({IROp::LOAD, elemVal, {p, IRValue::Constant(0, IRType::I32)},
                                               elemType.isVolatile});
                currentBlock_->addInstruction({IROp::STORE, std::nullopt, {varAddr, elemVal, IRValue::Constant(0, IRType::I32)}});
            }
            // CIDER-BUG-108: the copied element finalizes at the end of this
            // iteration (see registerLoopVarDestructors).
            registerLoopVarDestructors(elemType, varAddr);

            for (auto& s : stmt.getBody()) {
                visitStatement(s.get());
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
            // CIDER-BUG-133: this load reads the COMPILER-GENERATED slice
            // descriptor's length word (see visit(SliceExpr)), not the
            // user's object. Slices are expression-internal descriptors —
            // the language has no volatile-qualified slice spelling — so the
            // descriptor has no volatile model and the read stays plain; the
            // element accesses below carry the element type's actual flag.
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
        // CIDER-BUG-133: the element read carries the element type's
        // volatile flag (a `[volatile T; N]` array, or the element type of a
        // slice expression over one); the loop-variable temp store stays
        // plain. The descriptor words read above remain plain — the
        // descriptor is compiler-generated, not a volatile object.
        if (elemType.pointerLevel == 0 &&
            (elemType.isArray || isStructValueType(elemType) || getTypeSize(elemType) > 8)) {
            currentBlock_->addInstruction({IROp::MEMCPY, std::nullopt, {varAddr, elemAddr, IRValue::Constant(getTypeSize(elemType), IRType::I32)},
                                           elemType.isVolatile});
        } else {
            IRType irType = (elemType.pointerLevel > 0) ? IRType::PTR : mapType(elemType.name);
            IRValue elemVal = IRValue::Register(nextTemp(), irType);
            currentBlock_->addInstruction({IROp::LOAD, elemVal, {elemAddr, IRValue::Constant(0, IRType::I32)},
                                           elemType.isVolatile});
            currentBlock_->addInstruction({IROp::STORE, std::nullopt, {varAddr, elemVal, IRValue::Constant(0, IRType::I32)}});
        }
        // CIDER-BUG-108: the copied element finalizes at the end of this
        // iteration (see registerLoopVarDestructors).
        registerLoopVarDestructors(elemType, varAddr);

        for (auto& s : stmt.getBody()) {
            visitStatement(s.get());
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

            // CIDER-ISS-010: a null active-handler pointer means no try is
            // in scope — a plain uncaught throw, or a rethrow from the
            // outermost catch (which reinstalls the enclosing, possibly
            // null, handler before its body runs). Longjmping to null is
            // undefined and crashed the native backend, so branch to the
            // defined uncaught-exception path instead.
            std::string handlerBlockName = nextLabel("throw_handler");
            std::string uncaughtBlockName = nextLabel("uncaught");
            currentBlock_->addInstruction({IROp::COND_BR, std::nullopt,
                                           {bufPtr, IRValue::Label(handlerBlockName),
                                            IRValue::Label(uncaughtBlockName)}});

            startBlock(handlerBlockName);
            IRInstruction call;
            call.op = IROp::CALL;
            call.dest = std::nullopt;
            call.args.push_back(IRValue::Label("longjmp"));
            call.args.push_back(bufPtr);
            call.args.push_back(IRValue::Constant(1, IRType::I32));
            currentBlock_->addInstruction(call);

            startBlock(uncaughtBlockName);
            // The VM implements this label as a builtin that raises
            // `vm error: uncaught exception` and exits 2; codegen defines a
            // terminating stub with byte-identical stderr and exit code.
            IRInstruction uncaught;
            uncaught.op = IROp::CALL;
            uncaught.dest = std::nullopt;
            uncaught.args.push_back(IRValue::Label("_uncaught_exception"));
            currentBlock_->addInstruction(uncaught);

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
                visitStatement(s.get());
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
                visitStatement(s.get());
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
            for (auto& s : stmt.getTryBlock()) visitStatement(s.get());
            symbolTable_.exitScope();
            symbolTable_.enterScope();
            for (auto& s : stmt.getCatchBlock()) visitStatement(s.get());
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

    IRValue emitCall(const std::string& callee, std::vector<IRValue> args,
                     IRType returnType = IRType::PTR) {
        IRValue dest = IRValue::Register(nextTemp(), returnType);
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

    // CIDER-BUG-056: `_streq(a: char*, b: char*) -> bool` — byte-wise
    // content equality with strcmp semantics (both operands are
    // NUL-terminated). Like the interpolation helpers above this is plain
    // IR built once per module, so the VM (optimized and unoptimized) and
    // the native backend run the same code. 1 = equal, 0 = not equal.
    // nil is handled, not dereferenced: null vs null is equal, null vs a
    // real string is unequal.
    void ensureStringEqualityHelpers() {
        if (stringEqHelpersEnsured_) return;
        stringEqHelpersEnsured_ = true;

        auto fn = std::make_unique<FunctionIR>("_streq", IRType::BOOL);
        fn->addParam("arg_a");
        fn->addParam("arg_b");

        auto entry = std::make_unique<BasicBlock>("entry");
        auto loop = std::make_unique<BasicBlock>("loop");
        auto checkB = std::make_unique<BasicBlock>("check_b");
        auto bothNull = std::make_unique<BasicBlock>("both_null");
        auto content = std::make_unique<BasicBlock>("content");
        auto check = std::make_unique<BasicBlock>("check");
        auto advance = std::make_unique<BasicBlock>("advance");
        auto equal = std::make_unique<BasicBlock>("equal");
        auto differ = std::make_unique<BasicBlock>("differ");

        IRValue pa = IRValue::Register(nextTemp(), IRType::PTR);
        entry->addInstruction({IROp::ALLOCA, pa, {IRValue::Constant(8, IRType::I32)}});
        entry->addInstruction({IROp::STORE, std::nullopt,
                               {pa, IRValue::Register("arg_a", IRType::PTR),
                                IRValue::Constant(0, IRType::I32)}});
        IRValue pb = IRValue::Register(nextTemp(), IRType::PTR);
        entry->addInstruction({IROp::ALLOCA, pb, {IRValue::Constant(8, IRType::I32)}});
        entry->addInstruction({IROp::STORE, std::nullopt,
                               {pb, IRValue::Register("arg_b", IRType::PTR),
                                IRValue::Constant(0, IRType::I32)}});
        entry->addInstruction({IROp::BR, std::nullopt, {IRValue::Label("loop")}});

        IRValue va = IRValue::Register(nextTemp(), IRType::PTR);
        loop->addInstruction({IROp::LOAD, va, {pa, IRValue::Constant(0, IRType::I32)}});
        IRValue vb = IRValue::Register(nextTemp(), IRType::PTR);
        loop->addInstruction({IROp::LOAD, vb, {pb, IRValue::Constant(0, IRType::I32)}});
        IRValue aNull = IRValue::Register(nextTemp(), IRType::BOOL);
        loop->addInstruction({IROp::CMP_EQ, aNull, {va, IRValue::Constant(0, IRType::I32)}});
        loop->addInstruction({IROp::COND_BR, std::nullopt,
                              {aNull, IRValue::Label("both_null"), IRValue::Label("check_b")}});

        // Left non-null: a nil right side must not be dereferenced.
        IRValue bNullR = IRValue::Register(nextTemp(), IRType::BOOL);
        checkB->addInstruction({IROp::CMP_EQ, bNullR, {vb, IRValue::Constant(0, IRType::I32)}});
        checkB->addInstruction({IROp::COND_BR, std::nullopt,
                                {bNullR, IRValue::Label("differ"), IRValue::Label("content")}});

        IRValue bNull = IRValue::Register(nextTemp(), IRType::BOOL);
        bothNull->addInstruction({IROp::CMP_EQ, bNull, {vb, IRValue::Constant(0, IRType::I32)}});
        bothNull->addInstruction({IROp::COND_BR, std::nullopt,
                                  {bNull, IRValue::Label("equal"), IRValue::Label("differ")}});

        IRValue ca = IRValue::Register(nextTemp(), IRType::U8);
        content->addInstruction({IROp::LOAD, ca, {va, IRValue::Constant(0, IRType::I32)}});
        IRValue cb = IRValue::Register(nextTemp(), IRType::U8);
        content->addInstruction({IROp::LOAD, cb, {vb, IRValue::Constant(0, IRType::I32)}});
        IRValue ne = IRValue::Register(nextTemp(), IRType::BOOL);
        content->addInstruction({IROp::CMP_NE, ne, {ca, cb}});
        content->addInstruction({IROp::COND_BR, std::nullopt,
                                 {ne, IRValue::Label("differ"), IRValue::Label("check")}});

        IRValue atEnd = IRValue::Register(nextTemp(), IRType::BOOL);
        check->addInstruction({IROp::CMP_EQ, atEnd, {ca, IRValue::Constant(0, IRType::I32)}});
        check->addInstruction({IROp::COND_BR, std::nullopt,
                               {atEnd, IRValue::Label("equal"), IRValue::Label("advance")}});

        // Advance both cursors. Reload from the slots so this block does
        // not depend on the loop block's registers staying live.
        IRValue va2 = IRValue::Register(nextTemp(), IRType::PTR);
        advance->addInstruction({IROp::LOAD, va2, {pa, IRValue::Constant(0, IRType::I32)}});
        IRValue na = IRValue::Register(nextTemp(), IRType::PTR);
        advance->addInstruction({IROp::ADD, na, {va2, IRValue::Constant(1, IRType::I32)}});
        advance->addInstruction({IROp::STORE, std::nullopt,
                                 {pa, na, IRValue::Constant(0, IRType::I32)}});
        IRValue vb2 = IRValue::Register(nextTemp(), IRType::PTR);
        advance->addInstruction({IROp::LOAD, vb2, {pb, IRValue::Constant(0, IRType::I32)}});
        IRValue nb = IRValue::Register(nextTemp(), IRType::PTR);
        advance->addInstruction({IROp::ADD, nb, {vb2, IRValue::Constant(1, IRType::I32)}});
        advance->addInstruction({IROp::STORE, std::nullopt,
                                 {pb, nb, IRValue::Constant(0, IRType::I32)}});
        advance->addInstruction({IROp::BR, std::nullopt, {IRValue::Label("loop")}});

        equal->addInstruction({IROp::RET, std::nullopt, {IRValue::Constant(1, IRType::BOOL)}});
        differ->addInstruction({IROp::RET, std::nullopt, {IRValue::Constant(0, IRType::BOOL)}});

        fn->addBlock(std::move(entry));
        fn->addBlock(std::move(loop));
        fn->addBlock(std::move(checkB));
        fn->addBlock(std::move(bothNull));
        fn->addBlock(std::move(content));
        fn->addBlock(std::move(check));
        fn->addBlock(std::move(advance));
        fn->addBlock(std::move(equal));
        fn->addBlock(std::move(differ));
        module_.addFunction(std::move(fn));
    }

    // CIDER-ISS-012: `_strcmp(a: char*, b: char*) -> int` — lexicographic
    // content comparison with strcmp's sign convention (both operands are
    // NUL-terminated): <0 if a sorts before b, 0 if equal, >0 if a sorts
    // after b. Like _streq above this is plain IR built once per module,
    // so the VM (optimized and unoptimized) and the native backend run the
    // same code rather than delegating to a backend-specific libc strcmp.
    // nil is defined and never dereferenced: nil sorts before every
    // non-nil string, including "", and two nils compare equal. Bytes
    // compare as unsigned char, matching C strcmp.
    void ensureStringComparisonHelpers() {
        if (stringCmpHelpersEnsured_) return;
        stringCmpHelpersEnsured_ = true;

        auto fn = std::make_unique<FunctionIR>("_strcmp", IRType::I32);
        fn->addParam("arg_a");
        fn->addParam("arg_b");

        auto entry = std::make_unique<BasicBlock>("entry");
        auto loop = std::make_unique<BasicBlock>("loop");
        auto checkB = std::make_unique<BasicBlock>("check_b");
        auto aNull = std::make_unique<BasicBlock>("a_null");
        auto content = std::make_unique<BasicBlock>("content");
        auto check = std::make_unique<BasicBlock>("check");
        auto advance = std::make_unique<BasicBlock>("advance");
        auto differ = std::make_unique<BasicBlock>("differ");
        auto equal = std::make_unique<BasicBlock>("equal");
        auto less = std::make_unique<BasicBlock>("less");
        auto greater = std::make_unique<BasicBlock>("greater");

        IRValue pa = IRValue::Register(nextTemp(), IRType::PTR);
        entry->addInstruction({IROp::ALLOCA, pa, {IRValue::Constant(8, IRType::I32)}});
        entry->addInstruction({IROp::STORE, std::nullopt,
                               {pa, IRValue::Register("arg_a", IRType::PTR),
                                IRValue::Constant(0, IRType::I32)}});
        IRValue pb = IRValue::Register(nextTemp(), IRType::PTR);
        entry->addInstruction({IROp::ALLOCA, pb, {IRValue::Constant(8, IRType::I32)}});
        entry->addInstruction({IROp::STORE, std::nullopt,
                               {pb, IRValue::Register("arg_b", IRType::PTR),
                                IRValue::Constant(0, IRType::I32)}});
        entry->addInstruction({IROp::BR, std::nullopt, {IRValue::Label("loop")}});

        IRValue va = IRValue::Register(nextTemp(), IRType::PTR);
        loop->addInstruction({IROp::LOAD, va, {pa, IRValue::Constant(0, IRType::I32)}});
        IRValue vb = IRValue::Register(nextTemp(), IRType::PTR);
        loop->addInstruction({IROp::LOAD, vb, {pb, IRValue::Constant(0, IRType::I32)}});
        IRValue aIsNull = IRValue::Register(nextTemp(), IRType::BOOL);
        loop->addInstruction({IROp::CMP_EQ, aIsNull, {va, IRValue::Constant(0, IRType::I32)}});
        loop->addInstruction({IROp::COND_BR, std::nullopt,
                              {aIsNull, IRValue::Label("a_null"), IRValue::Label("check_b")}});

        // Left non-nil: a nil right side sorts after every string.
        IRValue bNullRight = IRValue::Register(nextTemp(), IRType::BOOL);
        checkB->addInstruction({IROp::CMP_EQ, bNullRight, {vb, IRValue::Constant(0, IRType::I32)}});
        checkB->addInstruction({IROp::COND_BR, std::nullopt,
                                {bNullRight, IRValue::Label("greater"), IRValue::Label("content")}});

        // Left nil: equal to nil, less than every non-nil string.
        IRValue bNullLeft = IRValue::Register(nextTemp(), IRType::BOOL);
        aNull->addInstruction({IROp::CMP_EQ, bNullLeft, {vb, IRValue::Constant(0, IRType::I32)}});
        aNull->addInstruction({IROp::COND_BR, std::nullopt,
                               {bNullLeft, IRValue::Label("equal"), IRValue::Label("less")}});

        IRValue ca = IRValue::Register(nextTemp(), IRType::U8);
        content->addInstruction({IROp::LOAD, ca, {va, IRValue::Constant(0, IRType::I32)}});
        IRValue cb = IRValue::Register(nextTemp(), IRType::U8);
        content->addInstruction({IROp::LOAD, cb, {vb, IRValue::Constant(0, IRType::I32)}});
        // The first differing byte decides; CMP on two U8 regs is an
        // unsigned byte compare in both backends (matching strcmp).
        IRValue ne = IRValue::Register(nextTemp(), IRType::BOOL);
        content->addInstruction({IROp::CMP_NE, ne, {ca, cb}});
        IRValue lt = IRValue::Register(nextTemp(), IRType::BOOL);
        content->addInstruction({IROp::CMP_LT, lt, {ca, cb}});
        content->addInstruction({IROp::COND_BR, std::nullopt,
                                 {ne, IRValue::Label("differ"), IRValue::Label("check")}});

        IRValue atEnd = IRValue::Register(nextTemp(), IRType::BOOL);
        check->addInstruction({IROp::CMP_EQ, atEnd, {ca, IRValue::Constant(0, IRType::I32)}});
        check->addInstruction({IROp::COND_BR, std::nullopt,
                               {atEnd, IRValue::Label("equal"), IRValue::Label("advance")}});

        // Advance both cursors. Reload from the slots so this block does
        // not depend on the loop block's registers staying live.
        IRValue va2 = IRValue::Register(nextTemp(), IRType::PTR);
        advance->addInstruction({IROp::LOAD, va2, {pa, IRValue::Constant(0, IRType::I32)}});
        IRValue na = IRValue::Register(nextTemp(), IRType::PTR);
        advance->addInstruction({IROp::ADD, na, {va2, IRValue::Constant(1, IRType::I32)}});
        advance->addInstruction({IROp::STORE, std::nullopt,
                                 {pa, na, IRValue::Constant(0, IRType::I32)}});
        IRValue vb2 = IRValue::Register(nextTemp(), IRType::PTR);
        advance->addInstruction({IROp::LOAD, vb2, {pb, IRValue::Constant(0, IRType::I32)}});
        IRValue nb = IRValue::Register(nextTemp(), IRType::PTR);
        advance->addInstruction({IROp::ADD, nb, {vb2, IRValue::Constant(1, IRType::I32)}});
        advance->addInstruction({IROp::STORE, std::nullopt,
                                 {pb, nb, IRValue::Constant(0, IRType::I32)}});
        advance->addInstruction({IROp::BR, std::nullopt, {IRValue::Label("loop")}});

        differ->addInstruction({IROp::COND_BR, std::nullopt,
                                {lt, IRValue::Label("less"), IRValue::Label("greater")}});

        equal->addInstruction({IROp::RET, std::nullopt, {IRValue::Constant(0, IRType::I32)}});
        less->addInstruction({IROp::RET, std::nullopt, {IRValue::Constant(-1, IRType::I32)}});
        greater->addInstruction({IROp::RET, std::nullopt, {IRValue::Constant(1, IRType::I32)}});

        fn->addBlock(std::move(entry));
        fn->addBlock(std::move(loop));
        fn->addBlock(std::move(checkB));
        fn->addBlock(std::move(aNull));
        fn->addBlock(std::move(content));
        fn->addBlock(std::move(check));
        fn->addBlock(std::move(advance));
        fn->addBlock(std::move(differ));
        fn->addBlock(std::move(equal));
        fn->addBlock(std::move(less));
        fn->addBlock(std::move(greater));
        module_.addFunction(std::move(fn));
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
        std::string strLabel = "_gs" + separateToken_ + std::to_string(globalStrCount_++);
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

    // CIDER-BUG-067: zero a freshly created instance. Every field must read
    // as zero until an initializer stores it. Scalar fields were already
    // covered field-by-field by CIDER-BUG-049, but aggregate fields (struct,
    // class-value, fixed array, 16-byte interface fat pointer) were not, and
    // escape analysis can rewrite `new` to a stack slot whose previous
    // contents are backend-dependent. The zero bytes come from a lazily
    // created static block of `size` zero bytes: the native backend emits it
    // in .bss (zero by definition at load), the VM zero-fills global storage
    // when materialising labels. Sharing one block per distinct size keeps
    // the data small; MEMCPY/MOV are understood by every backend and pass.
    // CIDER-BUG-155: the label lookup is split out for sites that must emit
    // the zeroing copy into a block other than the current one (the ternary
    // merge slot lives in the dominating condition block).
    std::string zeroBufferLabel(int size) {
        auto it = zeroBuffers_.find(size);
        if (it == zeroBuffers_.end()) {
            std::string label = "_cider_zero_" + separateToken_ + std::to_string(size);
            module_.addGlobalVar(label, IRType::U8, 0, 0, size, false);
            it = zeroBuffers_.emplace(size, label).first;
        }
        return it->second;
    }

    // CIDER-BUG-144: `isVolatile` is the destination's volatile flag (the
    // same flag every copy/declaration site sources from its alias-resolved
    // type), so zeroing a volatile aggregate is itself an observable MEMCPY.
    void emitZeroInstance(IRValue addr, int size, bool isVolatile = false) {
        if (size <= 0) return;
        IRValue src = IRValue::Register(nextTemp(), IRType::PTR);
        currentBlock_->addInstruction({IROp::MOV, src, {IRValue::Label(zeroBufferLabel(size))}});
        currentBlock_->addInstruction({IROp::MEMCPY, std::nullopt,
                                       {addr, src, IRValue::Constant(size, IRType::I32)},
                                       isVolatile});
    }

    // CIDER-BUG-049: materialise a class's field initializers as constant
    // stores at `objAddr`. The checker admits only foldable initializer shapes
    // (literals, const references, constant arithmetic, string literals,
    // function names), so no runtime initializer pass is needed. Inherited
    // fields lead `classSym->fields` (the collector copies them), so base
    // initializers run before derived ones. Field LAYOUT is untouched: only
    // values are stored, at the offsets the collector already assigned.
    // CIDER-BUG-067: callers zero the whole instance first (emitZeroInstance),
    // so fields without an initializer need no store here; this runs after
    // the zero and therefore overrides it.
    // CIDER-BUG-082: the recursion into embedded class-value fields lives in
    // emitClassFieldInitializersImpl.
    // CIDER-BUG-144: `isVolatile` is the destination's volatile flag, so a
    // volatile value local / `new` instance / global stamps its vptr and
    // field initializers through observable stores.
    void emitClassFieldInitializers(const std::shared_ptr<StructSymbol>& classSym,
                                    IRValue objAddr, bool isVolatile = false) {
        std::set<const StructSymbol*> visiting;
        emitClassFieldInitializersImpl(classSym, objAddr, visiting, isVolatile);
    }

    // CIDER-BUG-089: a class value's first word is its vptr (the collector
    // reserves offset 0 for every class whose hierarchy has a vtable).
    // Stamp the DECLARED class's vtable there — never a dynamic type's — so
    // a value instance is a C++-style slice of its static class, while
    // `new` pointers and interfaces keep dynamic dispatch. A class without
    // virtual methods reserves no vptr and emits nothing, keeping programs
    // without virtual value instances byte-identical.
    // CIDER-BUG-133: `isVolatile` marks the write when `objAddr` is a
    // volatile destination (CIDER-BUG-061/079 model), so a volatile class
    // value's re-stamp after a copy is itself an observable access.
    void emitClassVptrStore(const std::shared_ptr<StructSymbol>& classSym, IRValue objAddr,
                            bool isVolatile = false) {
        if (!classSym || classSym->kind != SymbolKind::CLASS || classSym->vtable.empty()) return;
        currentBlock_->addInstruction(
            {IROp::STORE, std::nullopt,
             {objAddr, IRValue::Label("vtable_" + vtableKeyForClass(classSym)),
              IRValue::Constant(0, IRType::I32)}, isVolatile});
    }

    // CIDER-BUG-089: resolve `t` to the class whose values reserve a vptr
    // (offset 0) — a CLASS with a non-empty vtable. Null for pointers,
    // interfaces, slices, arrays, structs, non-virtual classes and
    // unresolvable spellings, so every CIDER-BUG-089 path can test
    // "virtual class value" without emitting anything.
    std::shared_ptr<StructSymbol> virtualClassForType(TypeInfo t, const std::string& defModule = "") {
        if (t.pointerLevel > 0 || t.isArray || t.isSlice || t.isInterface || t.isFunctionPointer) return nullptr;
        t = canonicalizeNominalInModule(resolveTypeAlias(t, defModule), defModule);
        auto sym = resolveStructOrClassSymbol(t, defModule);
        if (!sym || sym->kind != SymbolKind::CLASS) return nullptr;
        auto classSym = std::static_pointer_cast<StructSymbol>(sym);
        if (classSym->vtable.empty()) return nullptr;
        return classSym;
    }

    // CIDER-BUG-089: the copy-site twin of emitClassVptrStore. Resolves an
    // alias or module-qualified spelling to its class before stamping, so a
    // copy into a declared class value re-asserts the static vptr instead
    // of smuggling the pointee's dynamic vptr into the value.
    // CIDER-BUG-133: `isVolatile` is the destination's volatile flag (the
    // same flag the copy's MEMCPY carries), marking the re-stamp.
    void emitClassVptrStoreForType(TypeInfo t, IRValue objAddr, const std::string& defModule = "",
                                   bool isVolatile = false) {
        emitClassVptrStore(virtualClassForType(t, defModule), objAddr, isVolatile);
    }

    // CIDER-BUG-087/089: true when an embedded class value under `t` needs
    // formation-time stores — a field initializer to run, or a vptr word to
    // stamp (any class with a vtable). Mirrors emitEmbeddedInitializers so
    // the two cannot disagree about when IR is emitted: aggregates with
    // neither stay byte-identical (no address computation, no stores).
    // `visiting` is the by-value layout cycle guard (a repeated class or
    // struct means `class A { var a: A; }` / `struct S { var s: S; }`):
    // stop instead of recursing without bound.
    bool aggregateNeedsInit(TypeInfo t, const std::string& defModule,
                            std::set<const StructSymbol*>& visiting) {
        t = canonicalizeNominalInModule(resolveTypeAlias(t, defModule), defModule);
        if (t.pointerLevel > 0 || t.isSlice || t.isInterface || t.isFunctionPointer) return false;
        if (t.isArray) {
            if (t.arraySize <= 0) return false;
            return aggregateNeedsInit(t.elementType(), defModule, visiting);
        }
        auto sym = resolveStructOrClassSymbol(t, defModule);
        if (!sym) return false;
        auto aggSym = std::static_pointer_cast<StructSymbol>(sym);
        if (!visiting.insert(aggSym.get()).second) return false;
        // CIDER-BUG-089: a virtual class's own vptr stamp is formation work.
        bool found = aggSym->kind == SymbolKind::CLASS && !aggSym->vtable.empty();
        if (!found) {
            std::string aggMod = definingModuleFor(aggSym, aggSym->name);
            for (const auto& field : aggSym->fields) {
                // CIDER-BUG-049: only class fields carry initializers; the
                // parser rejects them on struct fields.
                if (field.initializer) {
                    found = true;
                    break;
                }
                // CIDER-BUG-090: an inherited field's type resolves in its
                // declaring class's module, not the accessed class's.
                if (aggregateNeedsInit(field.type, fieldTypeModule(field, aggMod), visiting)) {
                    found = true;
                    break;
                }
            }
        }
        visiting.erase(aggSym.get());
        return found;
    }

    // CIDER-BUG-087/089: run the formation stores (field initializers and
    // vptr words) of every embedded class value under `base + offset`. The
    // caller established via aggregateNeedsInit that at least one exists, so
    // no IR is emitted for all-zero aggregates (byte-identity) — the address
    // is computed lazily here for the same reason. Pointer, slice, interface
    // and fn-pointer values are not embedded aggregates: pointers handle
    // their target where it is constructed, slices hold descriptor storage,
    // and an interface is a fat pointer.
    // CIDER-BUG-144: `isVolatile` is the destination aggregate's volatile
    // flag (or an enclosing volatile object's, through the field/array
    // recursion). Every formation store beneath it is an observable access
    // (CIDER-BUG-061's "every member of a volatile object is volatile"), and
    // a volatile-qualified field/element makes its own subobject volatile
    // even under a plain destination, so `t.isVolatile` merges into the flag
    // for this recursion level.
    void emitEmbeddedInitializers(TypeInfo t, IRValue base, int offset,
                                  const std::string& defModule,
                                  std::set<const StructSymbol*>& visiting,
                                  bool isVolatile = false) {
        t = canonicalizeNominalInModule(resolveTypeAlias(t, defModule), defModule);
        isVolatile = isVolatile || t.isVolatile;
        if (t.pointerLevel > 0 || t.isSlice || t.isInterface || t.isFunctionPointer) return;
        if (!aggregateNeedsInit(t, defModule, visiting)) return;
        IRValue addr = IRValue::Register(nextTemp(), IRType::PTR);
        currentBlock_->addInstruction(
            {IROp::ADD, addr, {base, IRValue::Constant((int64_t)offset, IRType::I32)}});
        if (t.isArray) {
            // CIDER-BUG-087: a fixed array of class values embeds every
            // element as a subobject; run them in index order.
            TypeInfo elem = canonicalizeNominalInModule(resolveTypeAlias(t.elementType(), defModule),
                                                        defModule);
            int elemSize = getTypeSize(elem);
            for (int i = 0; i < t.arraySize; ++i) {
                emitEmbeddedInitializers(elem, addr, i * elemSize, defModule, visiting, isVolatile);
            }
            return;
        }
        auto sym = resolveStructOrClassSymbol(t, defModule);
        if (!sym) return;
        auto aggSym = std::static_pointer_cast<StructSymbol>(sym);
        if (aggSym->kind == SymbolKind::CLASS) {
            emitClassFieldInitializersImpl(aggSym, addr, visiting, isVolatile);
        } else {
            emitStructFieldInitializersImpl(aggSym, addr, visiting, isVolatile);
        }
    }

    // CIDER-BUG-087: structs have no field initializers of their own (the
    // parser rejects them), so this only recurses into the class values
    // their fields embed, at the field's address. `visiting` carries the
    // layout cycle guard for struct-in-struct by-value nesting.
    // CIDER-BUG-144: `isVolatile` carries the enclosing destination's flag;
    // a volatile-qualified field is merged in emitEmbeddedInitializers.
    void emitStructFieldInitializersImpl(const std::shared_ptr<StructSymbol>& structSym,
                                         IRValue objAddr,
                                         std::set<const StructSymbol*>& visiting,
                                         bool isVolatile = false) {
        if (!structSym || !currentFunction_) return;
        if (!visiting.insert(structSym.get()).second) return;
        std::string structMod = definingModuleFor(structSym, structSym->name);
        for (const auto& field : structSym->fields) {
            // CIDER-BUG-090: per-field declaring module (templates).
            emitEmbeddedInitializers(field.type, objAddr, field.offset,
                                     fieldTypeModule(field, structMod), visiting, isVolatile);
        }
        visiting.erase(structSym.get());
    }

    // CIDER-BUG-088: the finalizer twin of aggregateNeedsInit. True
    // when `t` is a class carrying a destructor or embeds (through class
    // fields, struct fields and fixed arrays of class/struct values) a class
    // value that carries one. Mirrors registerEmbeddedDestructors so the two
    // cannot disagree about when IR is emitted: aggregates without embedded
    // class destructors register nothing and stay byte-identical. `visiting`
    // is the same by-value layout cycle guard as the initializer walk.
    bool aggregateHasDestructors(TypeInfo t, const std::string& defModule,
                                 std::set<const StructSymbol*>& visiting) {
        t = canonicalizeNominalInModule(resolveTypeAlias(t, defModule), defModule);
        if (t.pointerLevel > 0 || t.isSlice || t.isInterface || t.isFunctionPointer) return false;
        if (t.isArray) {
            if (t.arraySize <= 0) return false;
            return aggregateHasDestructors(t.elementType(), defModule, visiting);
        }
        auto sym = resolveStructOrClassSymbol(t, defModule);
        if (!sym) return false;
        auto aggSym = std::static_pointer_cast<StructSymbol>(sym);
        if (!visiting.insert(aggSym.get()).second) return false;
        // A class's own destructor is registered by the walk too (last, so
        // it runs first at scope exit).
        bool found = aggSym->kind == SymbolKind::CLASS && !aggSym->destructorName.empty();
        if (!found) {
            std::string aggMod = definingModuleFor(aggSym, aggSym->name);
            for (const auto& field : aggSym->fields) {
                // CIDER-BUG-090: per-field declaring module.
                if (aggregateHasDestructors(field.type, fieldTypeModule(field, aggMod), visiting)) {
                    found = true;
                    break;
                }
            }
        }
        visiting.erase(aggSym.get());
        return found;
    }

    // CIDER-BUG-088: register scope-exit destructor calls for every class
    // value embedded under `base + offset`, mirroring the
    // emitEmbeddedInitializers traversal (structs, classes and fixed arrays;
    // pointers/slices/interfaces/fn-pointers are not embedded aggregates).
    // Push order is construction order — embedded fields first, the class's
    // own destructor last — so the scope-exit emission, which iterates the
    // stack in reverse, destroys the class body first (a destructor may
    // still read its subobjects) and then the embedded values in reverse
    // declaration/element order.
    void registerEmbeddedDestructors(TypeInfo t, IRValue base, int offset,
                                     const std::string& defModule,
                                     std::set<const StructSymbol*>& visiting) {
        t = canonicalizeNominalInModule(resolveTypeAlias(t, defModule), defModule);
        if (t.pointerLevel > 0 || t.isSlice || t.isInterface || t.isFunctionPointer) return;
        if (t.isArray) {
            TypeInfo elem = canonicalizeNominalInModule(resolveTypeAlias(t.elementType(), defModule),
                                                        defModule);
            int elemSize = getTypeSize(elem);
            for (int i = 0; i < t.arraySize; ++i) {
                registerEmbeddedDestructors(elem, base, offset + i * elemSize, defModule, visiting);
            }
            return;
        }
        auto sym = resolveStructOrClassSymbol(t, defModule);
        if (!sym) return;
        auto aggSym = std::static_pointer_cast<StructSymbol>(sym);
        if (!visiting.insert(aggSym.get()).second) return;
        std::string aggMod = definingModuleFor(aggSym, aggSym->name);
        for (const auto& field : aggSym->fields) {
            // CIDER-BUG-090: per-field declaring module.
            registerEmbeddedDestructors(field.type, base, offset + field.offset,
                                        fieldTypeModule(field, aggMod), visiting);
        }
        if (aggSym->kind == SymbolKind::CLASS && !aggSym->destructorName.empty()) {
            // CID-020/021 (spec §5/§6): destructors mangle with the class's
            // defining module, matching the definition (same rule as the
            // direct class-local registration in visit(VarDeclStmt)).
            auto dtorDef = resolveFunctionDef(aggSym->destructorName,
                                              definingScopeFor(aggSym, aggSym->name));
            if (!dtorDef.first && !aggSym->manglePrefix.empty()) {
                // CIDER-BUG-119: a function/block-local class's destructor
                // symbol is unreachable after collection; attribute the
                // label to the class's defining module directly (the
                // definition is emitted from the class body under it).
                dtorDef.second = definingModuleFor(aggSym, aggSym->name);
            }
            destructorStack_.back().push_back(
                {emitFunctionName(dtorDef.second, aggSym->destructorName), base, offset});
        }
        visiting.erase(aggSym.get());
    }

    // CIDER-BUG-108: an iteration variable is a fresh copy of the element
    // (both foreach paths below copy it into its own slot), so it owns
    // embedded class values exactly like a declared local and finalizes when
    // the per-iteration scope exits — break/continue included, because
    // unwindDepths_ indexes that scope. Gated by aggregateHasDestructors so
    // scalar and class-free elements register nothing (byte-identical).
    void registerLoopVarDestructors(TypeInfo elemType, IRValue varAddr) {
        std::set<const StructSymbol*> visiting;
        if (aggregateHasDestructors(elemType, currentModule_, visiting)) {
            registerEmbeddedDestructors(elemType, varAddr, 0, currentModule_, visiting);
        }
    }

    // CIDER-BUG-137: an owned compiler temporary finalizes at the exit of
    // the scope that owns it, exactly like a declared local. The current
    // cases are a by-value parameter copy (registered at function scope) and
    // a caller-side by-value return slot (registered at the caller's
    // enclosing scope after the call). Registration reuses the CIDER-BUG-088
    // recursion and ordering — own destructor first, then embedded class
    // values in reverse construction order, LIFO with the surrounding
    // locals — so temporaries cannot disagree with locals about depth or
    // order. `t` is a declaration spelling; `defModule` roots its nominal
    // names like every type query at the site. Aggregates without embedded
    // class destructors register nothing (byte-identical IR).
    void registerTempDestructors(TypeInfo t, IRValue addr, const std::string& defModule) {
        if (!currentFunction_ || destructorStack_.empty()) return;
        std::set<const StructSymbol*> visiting;
        if (aggregateHasDestructors(t, defModule, visiting)) {
            registerEmbeddedDestructors(t, addr, 0, defModule, visiting);
        }
    }

    // CIDER-BUG-087: module-scope class/struct values are zero in .bss (and
    // in the VM's zero-filled global storage), so their embedded class
    // values need the same formation stores a local gets (CIDER-BUG-089:
    // field initializers and vptrs). There is no runtime pass for globals,
    // so synthesize an init function and call it from every entry function
    // before user code; the body replays the same emitEmbeddedInitializers
    // machinery at each global's label. Programs with no such globals emit
    // nothing (byte-identical).
    //
    // CIDER-BUG-106: under separate compilation a module object has no entry
    // function to hang the call off, and the entry object's own .bss does
    // not cover the library's globals. Each object therefore emits a
    // per-object init: a `--emit-meta` module exports a deterministic
    // symbol and every object chains the init of each module it imported
    // (the collector appends imported ASTs dependency-first, so the calls
    // replay the whole-program declaration order). Whole-program builds
    // keep the single local `_cider_global_init`.
    void emitGlobalFieldInitializers(AstProgram& program) {
        // The root file's logical module: non-empty only for a `--emit-meta`
        // module object (the main/entry file keeps "").
        std::string rootModule;
        auto rootIt = programModules_.find(&program);
        if (rootIt != programModules_.end()) rootModule = rootIt->second;

        // CIDER-BUG-106: init functions this object calls before its own
        // globals. Empty in whole-program mode (one combined pass).
        std::vector<std::string> depInits;
        if (separateCompilation_) {
            for (const auto& imported : program.getImportedAsts()) {
                auto it = programModules_.find(imported.get());
                if (it == programModules_.end()) continue;
                if (it->second.empty() || it->second == rootModule) continue;
                std::string depName = moduleGlobalInitFunctionName(it->second);
                if (std::find(depInits.begin(), depInits.end(), depName) == depInits.end()) {
                    depInits.push_back(depName);
                }
            }
        }

        std::set<const StructSymbol*> visiting;
        bool any = false;
        for (const auto& pending : pendingGlobalInits_) {
            TypeInfo t = canonicalizeNominalInModule(
                resolveTypeAlias(pending.type, pending.defModule), pending.defModule);
            if (aggregateNeedsInit(t, pending.defModule, visiting)) {
                any = true;
                break;
            }
        }

        // Whole-program: emit only when there is something to initialise and
        // an entry to call it. Separate compilation: a module object always
        // exports its init (importers call it unconditionally; its body may
        // be empty); an entry object emits one as soon as it has its own
        // globals or an imported module to chain.
        if (!separateCompilation_) {
            if (!any || entryFunctions_.empty()) return;
        } else if (rootModule.empty() && !any && depInits.empty()) {
            return;
        }

        const bool exported = separateCompilation_ && !rootModule.empty();
        const std::string initName =
            exported ? moduleGlobalInitFunctionName(rootModule) : globalInitFunctionName();

        // Build the helper in a fresh function context: field initializers
        // fold consts in their declaring module and allocate temps, exactly
        // as at a local creation site.
        auto prevFunction = std::move(currentFunction_);
        auto prevBlock = std::move(currentBlock_);
        auto prevVariableValues = variableValues_;
        int prevTempCount = tempCount_;
        int prevLabelCount = labelCount_;
        BasicBlock* prevEntryBlock = functionEntryBlock_;
        TypeInfo prevReturnType = currentReturnType_;
        bool prevIsSret = currentIsSret_;

        currentFunction_ = std::make_unique<FunctionIR>(initName, IRType::VOID);
        if (exported) {
            // A source path marks the definition for per-file codegen: the
            // module's own object emits and exports the symbol.
            currentFunction_->setSourcePath(currentSourcePath_);
        }
        currentBlock_ = std::make_unique<BasicBlock>("entry");
        functionEntryBlock_ = currentBlock_.get();
        variableValues_.clear();
        tempCount_ = 0;
        labelCount_ = 0;
        currentReturnType_ = {"void", 0};
        currentIsSret_ = false;

        // CIDER-BUG-106: chain imported modules first, mirroring the
        // whole-program declaration order (dependencies precede the module).
        for (const auto& dep : depInits) {
            currentBlock_->addInstruction({IROp::CALL, std::nullopt, {IRValue::Label(dep)}});
            module_.addExternFunction(dep);
        }

        for (const auto& pending : pendingGlobalInits_) {
            TypeInfo t = canonicalizeNominalInModule(
                resolveTypeAlias(pending.type, pending.defModule), pending.defModule);
            if (!aggregateNeedsInit(t, pending.defModule, visiting)) continue;
            IRValue base = IRValue::Register(nextTemp(), IRType::PTR);
            currentBlock_->addInstruction({IROp::MOV, base, {pending.addr}});
            // CIDER-BUG-144: a volatile global's formation stores are
            // observable accesses, exactly like a volatile local's.
            emitEmbeddedInitializers(t, base, 0, pending.defModule, visiting, pending.isVolatile);
        }
        currentBlock_->addInstruction({IROp::RET, std::nullopt, {}});
        currentFunction_->addBlock(std::move(currentBlock_));
        currentBlock_ = nullptr;
        module_.addFunction(std::move(currentFunction_));

        currentFunction_ = std::move(prevFunction);
        currentBlock_ = std::move(prevBlock);
        variableValues_ = prevVariableValues;
        tempCount_ = prevTempCount;
        labelCount_ = prevLabelCount;
        functionEntryBlock_ = prevEntryBlock;
        currentReturnType_ = prevReturnType;
        currentIsSret_ = prevIsSret;

        // The call runs before any user code. A program with both `main`
        // and `_start` gets it in each; the pass is idempotent, so a shared
        // entry cannot observe a half-initialised global.
        IRInstruction call;
        call.op = IROp::CALL;
        call.dest = std::nullopt;
        call.args.push_back(IRValue::Label(initName));
        for (FunctionIR* entry : entryFunctions_) {
            if (!entry) continue;
            auto& blocks = entry->getBlocks();
            if (blocks.empty()) continue;
            auto& instructions = blocks[0]->getInstructions();
            instructions.insert(instructions.begin(), call);
        }
    }

    // CIDER-BUG-087: fixed helper name, matching the runtime-helper style
    // (`_strlen`, `_streq`). Synthesized (empty source path), so codegen
    // emits it in the object that generated it and only that object's entry
    // references it.
    static std::string globalInitFunctionName() { return "_cider_global_init"; }

    // CIDER-BUG-106: exported per-object init symbol of a separate-
    // compilation module object. Derived from the logical module name with
    // the same injective mangling as every other module symbol, so an
    // importing object can name it without extra metadata.
    static std::string moduleGlobalInitFunctionName(const std::string& module) {
        return mangleQualifiedName(module, "_cider_global_init");
    }

    // CIDER-BUG-137: module-scope globals with embedded class destructors
    // finalize at program exit, in reverse construction order. The startup
    // pass (CIDER-BUG-087/106) constructed them dependency-first in the same
    // order the whole-program IR walk produces; the fini undoes exactly that
    // order: this object's own globals last-declared first, then every
    // transitively imported module's fini, each exactly once, in reverse
    // dependency order.
    //
    // Whole-program builds get one local `_cider_global_fini`, called from
    // every entry function (`main`/`_start`/`efi_main`) just before each
    // return, so the VM entry return and the native entry return both run
    // it. CIDER-BUG-153 extends the same call to a terminal inline-asm exit
    // syscall visible in the entry (a native `_start` cannot return at all);
    // see emitFiniBeforeAsmExits. Programs with no destructible globals emit
    // nothing (byte-identical). Separate compilation mirrors CIDER-FEAT-006:
    // every module object exports a mangled fini that destroys ONLY its own
    // globals. The entry object owns the ordering: it walks its transitive
    // import graph in the same dependency-first order the init chain uses,
    // reverses it, and calls each module's fini once. Chaining finis per
    // module was rejected because a diamond import would destroy a shared
    // module's globals twice and a per-object chain would destroy them in
    // graph order, not reverse construction order. A module object has no
    // entry, so its fini only runs when an entry object calls it; the
    // unusual `--emit-meta` object that also defines an entry still does not
    // chain its own dependencies (an importer's walk reaches them; a
    // standalone run of such an object leaves its dependency globals
    // unfinalized).
    void emitGlobalFieldFinalizers(AstProgram& program) {
        std::string rootModule;
        auto rootIt = programModules_.find(&program);
        if (rootIt != programModules_.end()) rootModule = rootIt->second;

        // This object's own destructible globals, in construction order.
        // Under separate compilation an imported source AST (the corrupt-
        // cache fallback) can also contribute pending globals; those belong
        // to the imported module's object, whose fini destroys them, so keep
        // only the root module's own. Whole-program mode has one combined
        // pass and no dep chain, so every pending global is ours.
        std::vector<const PendingGlobalInit*> own;
        for (const auto& pending : pendingGlobalInits_) {
            if (separateCompilation_ && pending.defModule != rootModule) continue;
            std::set<const StructSymbol*> visiting;
            if (aggregateHasDestructors(pending.type, pending.defModule, visiting)) {
                own.push_back(&pending);
            }
        }

        // Transitive module dependencies in construction order: the exact
        // order `generate` first initializes their globals (imports before
        // the importing module, siblings in import order, each module once).
        // Reversed below into destruction order. A module object does not
        // chain its dependencies: the entry object's single ordered list
        // makes the reverse construction order exact and diamond-safe.
        std::vector<std::string> depFinis;
        const bool exported = separateCompilation_ && !rootModule.empty();
        // A module object never chains its dependencies: the entry object
        // owns the single ordered list, which is the only diamond-safe and
        // exact-reverse-construction way. An imported module that happens to
        // define an entry therefore leaves its own dependencies to the
        // importing entry object's walk (documented limitation for the
        // unusual module-with-main build).
        if (separateCompilation_ && !exported) {
            std::set<std::string> seen;
            std::vector<std::string> order;
            std::function<void(AstProgram*)> walk = [&](AstProgram* p) {
                if (!p) return;
                auto it = programModules_.find(p);
                std::string mod = (it != programModules_.end()) ? it->second : std::string();
                if (mod == rootModule) return;   // the root is handled as `own`
                if (!mod.empty() && !seen.insert(mod).second) return;  // diamond
                for (const auto& imp : p->getImportedAsts()) walk(imp.get());
                if (!mod.empty()) order.push_back(mod);
            };
            for (const auto& imported : program.getImportedAsts()) walk(imported.get());
            std::reverse(order.begin(), order.end());
            for (const auto& mod : order) {
                depFinis.push_back(moduleGlobalFiniFunctionName(mod));
            }
        }

        if (!separateCompilation_) {
            if (own.empty() || entryFunctions_.empty()) return;
        } else if (rootModule.empty() && own.empty() && depFinis.empty()) {
            return;
        }

        const std::string finiName =
            exported ? moduleGlobalFiniFunctionName(rootModule) : globalFiniFunctionName();

        // Build the helper in a fresh function context, mirroring
        // emitGlobalFieldInitializers. Destruction registration uses the
        // ordinary local machinery (registerEmbeddedDestructors), pushed in
        // construction order so the reverse emission runs the last-built
        // object's own destructor first, then its embedded values in
        // reverse, and so on down to the first global.
        auto prevFunction = std::move(currentFunction_);
        auto prevBlock = std::move(currentBlock_);
        auto prevVariableValues = variableValues_;
        int prevTempCount = tempCount_;
        int prevLabelCount = labelCount_;
        BasicBlock* prevEntryBlock = functionEntryBlock_;
        TypeInfo prevReturnType = currentReturnType_;
        bool prevIsSret = currentIsSret_;
        auto prevDeferStack = deferStack_;
        auto prevDestructorStack = destructorStack_;

        currentFunction_ = std::make_unique<FunctionIR>(finiName, IRType::VOID);
        if (exported) {
            // A source path marks the definition for per-file codegen: the
            // module's own object emits and exports the symbol.
            currentFunction_->setSourcePath(currentSourcePath_);
        }
        currentBlock_ = std::make_unique<BasicBlock>("entry");
        functionEntryBlock_ = currentBlock_.get();
        variableValues_.clear();
        tempCount_ = 0;
        labelCount_ = 0;
        currentReturnType_ = {"void", 0};
        currentIsSret_ = false;
        deferStack_.clear();
        destructorStack_.clear();
        deferStack_.push_back({});
        destructorStack_.push_back({});

        for (const PendingGlobalInit* pending : own) {
            IRValue base = IRValue::Register(nextTemp(), IRType::PTR);
            currentBlock_->addInstruction({IROp::MOV, base, {pending->addr}});
            std::set<const StructSymbol*> visiting;
            registerEmbeddedDestructors(pending->type, base, 0, pending->defModule, visiting);
        }
        emitDestructors();

        // Imported modules destroy their own globals, each exactly once,
        // after this object's; the entry's reverse construction order is
        // carried by this list (module objects have an empty one).
        for (const auto& dep : depFinis) {
            currentBlock_->addInstruction({IROp::CALL, std::nullopt, {IRValue::Label(dep)}});
            module_.addExternFunction(dep);
        }

        currentBlock_->addInstruction({IROp::RET, std::nullopt, {}});
        currentFunction_->addBlock(std::move(currentBlock_));
        currentBlock_ = nullptr;
        module_.addFunction(std::move(currentFunction_));

        currentFunction_ = std::move(prevFunction);
        currentBlock_ = std::move(prevBlock);
        variableValues_ = prevVariableValues;
        tempCount_ = prevTempCount;
        labelCount_ = prevLabelCount;
        functionEntryBlock_ = prevEntryBlock;
        currentReturnType_ = prevReturnType;
        currentIsSret_ = prevIsSret;
        deferStack_ = std::move(prevDeferStack);
        destructorStack_ = std::move(prevDestructorStack);

        // Call the fini from every exit of every entry function. The entry
        // runs once per program, and one RET is taken, so the fini runs
        // exactly once; the return value stays live across the call (IR
        // registers survive calls on every backend).
        IRInstruction finiCall;
        finiCall.op = IROp::CALL;
        finiCall.dest = std::nullopt;
        finiCall.args.push_back(IRValue::Label(finiName));
        for (FunctionIR* entry : entryFunctions_) {
            if (!entry) continue;
            for (auto& block : entry->getBlocks()) {
                auto& instructions = block->getInstructions();
                for (size_t i = instructions.size(); i > 0; --i) {
                    if (instructions[i - 1].op == IROp::RET) {
                        instructions.insert(instructions.begin() + (i - 1), finiCall);
                    }
                }
            }
            // CIDER-BUG-153: an inline-asm exit syscall never returns, so
            // the RET-site call above is unreachable on that path. Where the
            // syscall is compiler-visible in the entry's own asm, call the
            // fini in front of it instead.
            emitFiniBeforeAsmExits(entry, finiCall);
        }
    }

    // CIDER-BUG-153: a native `_start` cannot return (the kernel enters it
    // with no return address), so bare-metal programs end in an inline-asm
    // exit syscall; that never reaches the RET-site fini call CIDER-BUG-137
    // injects, while the VM's `_start` return path does run it. Where the
    // exit syscall is directly visible in an entry's own asm, run the fini
    // there so both backends agree.
    //
    // The call goes immediately BEFORE the exit sequence's register setup,
    // never between the setup and the syscall: the fini runs destructors
    // (arbitrary user code) and both backends lose the raw syscall state
    // across it (a native call clobbers caller-saved registers; the VM's
    // single asm register file is shared with the callee's asm). Writing
    // rax/rdi after the call re-establishes the exit state on both.
    //
    // The recognition is deliberately conservative:
    //   * only the entry's own inline asm is inspected, never a function it
    //     calls (an asm exit inside a helper keeps the documented gap);
    //   * rax must be provably the exit(2) (60) or exit_group(2) (231)
    //     number from a `mov rax/eax, <imm>` or `xor rax, rax` in the same
    //     basic block, and the exit-status register rdi must be written at
    //     or after the insertion point; a call, a non-asm instruction, an
    //     unrecognized rax/rdi mention or another syscall clears the
    //     candidate, so a `syscall` that is not provably an exit
    //     (write/read/mmap/...) is never touched;
    //   * the insertion point is the first write of the setup region that
    //     ends at the syscall, so earlier work in a multi-line block (a
    //     write syscall, a print) still runs first.
    void emitFiniBeforeAsmExits(FunctionIR* entry, const IRInstruction& finiCall) {
        if (!entry) return;

        // Only a full-width write establishes an input: `rax`/`eax` (32-bit
        // writes zero-extend) and `rdi`/`edi`. Partial writes (`ax`, `al`,
        // `di`, `dil`) leave the rest of the register unknown and are treated
        // as conservative clobbers by the mention checks below.
        auto isFullRax = [](const std::string& t) {
            return t == "rax" || t == "eax";
        };
        auto isFullRdi = [](const std::string& t) {
            return t == "rdi" || t == "edi";
        };
        // Word-boundary scan for any spelling in a raw asm line.
        auto mentionsAny = [](const std::string& text, const char* const* names, size_t count) {
            auto isWordChar = [](char c) {
                return std::isalnum((unsigned char)c) || c == '_';
            };
            for (size_t n = 0; n < count; ++n) {
                size_t len = std::strlen(names[n]);
                size_t pos = 0;
                while ((pos = text.find(names[n], pos)) != std::string::npos) {
                    bool leftOk = (pos == 0) || !isWordChar(text[pos - 1]);
                    bool rightOk = (pos + len >= text.size()) || !isWordChar(text[pos + len]);
                    if (leftOk && rightOk) return true;
                    pos += len;
                }
            }
            return false;
        };
        auto mentionsRax = [&](const std::string& t) {
            static const char* const kNames[] = {"rax", "eax", "ax", "al", "ah"};
            return mentionsAny(t, kNames, 5);
        };
        auto mentionsRdi = [&](const std::string& t) {
            static const char* const kNames[] = {"rdi", "edi", "di", "dil"};
            return mentionsAny(t, kNames, 4);
        };
        auto trim = [](const std::string& s) {
            size_t b = s.find_first_not_of(" \t\r");
            if (b == std::string::npos) return std::string();
            size_t e = s.find_last_not_of(" \t\r");
            return s.substr(b, e - b + 1);
        };
        // A bare integer operand (decimal or 0x...). Anything else (a
        // placeholder, register, symbol, expression) is not statically known.
        auto parseImm = [](const std::string& s, int64_t& out) {
            if (s.empty()) return false;
            char* end = nullptr;
            long long v = std::strtoll(s.c_str(), &end, 0);
            if (end == s.c_str() || *end != '\0') return false;
            out = v;
            return true;
        };
        // First whitespace-delimited token: the operand proper, with any
        // trailing comment dropped.
        auto firstToken = [](const std::string& s) {
            size_t b = s.find_first_not_of(" \t");
            if (b == std::string::npos) return std::string();
            size_t e = s.find_first_of(" \t", b);
            return s.substr(b, e == std::string::npos ? std::string::npos : e - b);
        };

        // One linear walk per block: the rax value, the rdi write and the
        // start of the setup region carry across consecutive asm blocks (the
        // `asm{"mov rax, 60"} asm{"mov rdi, 0"} asm{"syscall"}` idiom).
        // Anything else -- a call, a non-asm instruction, an unrecognized
        // rax/rdi mention or another syscall -- ends the region.
        for (auto& block : entry->getBlocks()) {
            auto& instrs = block->getInstructions();
            bool raxKnown = false;
            int64_t raxVal = 0;
            bool rdiKnown = false;
            bool inRegion = false;
            size_t setupInstr = 0;   // instruction holding the region's first write
            size_t setupOffset = 0;  // byte offset of that line in its asm text
            std::map<size_t, std::vector<size_t>> insertions;

            auto clearRegion = [&]() {
                raxKnown = false;
                rdiKnown = false;
                inRegion = false;
            };
            auto beginRegion = [&](size_t instrIdx, size_t off) {
                if (!inRegion) {
                    inRegion = true;
                    setupInstr = instrIdx;
                    setupOffset = off;
                }
            };

            for (size_t i = 0; i < instrs.size(); ++i) {
                if (instrs[i].op != IROp::ASM || instrs[i].args.empty() ||
                    instrs[i].args[0].kind != IRValue::LABEL) {
                    clearRegion();
                    continue;
                }
                const std::string& code = instrs[i].args[0].name;
                size_t pos = 0;
                while (pos <= code.size()) {
                    size_t nl = code.find('\n', pos);
                    std::string line = trim(code.substr(
                        pos, nl == std::string::npos ? std::string::npos : nl - pos));
                    if (!line.empty()) {
                        size_t sp = line.find_first_of(" \t");
                        std::string mnem = (sp == std::string::npos) ? line : line.substr(0, sp);
                        if (mnem == "syscall") {
                            if (raxKnown && (raxVal == 60 || raxVal == 231) && rdiKnown &&
                                inRegion) {
                                insertions[setupInstr].push_back(setupOffset);
                            }
                            // The syscall returns its result in rax (or does
                            // not return at all); either way the state is
                            // consumed and rdi must be set up again.
                            clearRegion();
                        } else if (mnem == "mov" || mnem == "xor" || mnem == "movsxd" ||
                                   mnem == "lea") {
                            size_t p = (sp == std::string::npos)
                                           ? std::string::npos
                                           : line.find_first_not_of(" \t", sp);
                            std::string rest = (p == std::string::npos) ? std::string()
                                                                        : line.substr(p);
                            size_t comma = rest.find(',');
                            std::string dst = trim(comma == std::string::npos
                                                       ? rest
                                                       : rest.substr(0, comma));
                            std::string src = firstToken(comma == std::string::npos
                                                             ? std::string()
                                                             : rest.substr(comma + 1));
                            if (isFullRax(dst)) {
                                int64_t v = 0;
                                if (mnem == "xor" && src == dst) {
                                    raxKnown = true;
                                    raxVal = 0;
                                } else if (mnem == "mov" && parseImm(src, v)) {
                                    raxKnown = true;
                                    raxVal = v;
                                } else {
                                    raxKnown = false;
                                }
                                beginRegion(i, pos);
                            } else if (isFullRdi(dst)) {
                                rdiKnown = true;
                                beginRegion(i, pos);
                            } else if (mentionsRax(line) || mentionsRdi(line)) {
                                clearRegion();
                            }
                        } else if (mnem == "call" || mentionsRax(line) || mentionsRdi(line)) {
                            // A call may return into rax/rdi; any other
                            // mention is conservatively a possible write.
                            clearRegion();
                        }
                    }
                    if (nl == std::string::npos) break;
                    pos = nl + 1;
                }
            }
            if (insertions.empty()) continue;

            std::vector<IRInstruction> rebuilt;
            rebuilt.reserve(instrs.size() + insertions.size() * 2);
            for (size_t i = 0; i < instrs.size(); ++i) {
                auto it = insertions.find(i);
                if (it == insertions.end()) {
                    rebuilt.push_back(instrs[i]);
                    continue;
                }
                std::vector<size_t>& offsets = it->second;
                std::sort(offsets.begin(), offsets.end());
                offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
                const std::string& code = instrs[i].args[0].name;
                size_t start = 0;
                for (size_t off : offsets) {
                    std::string chunk = code.substr(start, off - start);
                    if (chunk.find_first_not_of(" \t\r\n") != std::string::npos) {
                        IRInstruction piece = instrs[i];
                        piece.args[0] = IRValue::Label(chunk);
                        rebuilt.push_back(std::move(piece));
                    }
                    rebuilt.push_back(finiCall);
                    start = off;
                }
                // The setup region and everything after it (including the
                // syscall) stay as their own asm block, with the placeholder
                // map preserved.
                IRInstruction piece = instrs[i];
                piece.args[0] = IRValue::Label(code.substr(start));
                rebuilt.push_back(std::move(piece));
            }
            instrs = std::move(rebuilt);
        }
    }

    // CIDER-BUG-137: fixed whole-program exit-pass name, styled like
    // globalInitFunctionName. Synthesized (empty source path), so codegen
    // emits it in the object that generated it and only that object's entry
    // references it.
    static std::string globalFiniFunctionName() { return "_cider_global_fini"; }

    // CIDER-BUG-137: exported per-object exit-pass symbol of a separate-
    // compilation module object (the fini twin of moduleGlobalInitFunctionName).
    static std::string moduleGlobalFiniFunctionName(const std::string& module) {
        return mangleQualifiedName(module, "_cider_global_fini");
    }

    // CIDER-BUG-082/087: `visiting` is the recursion path for embedded
    // class-value fields, keyed by aggregate identity. A repeated class or
    // struct means a by-value layout cycle (`class A { var a: A; }`,
    // `struct S { var s: S; }`, which the collector sizes with a
    // fallback): stop instead of recursing without bound. It is a path
    // set, not a global one, so two sibling fields of one class both run.
    // CIDER-BUG-144: `isVolatile` is the destination's volatile flag (the
    // value local / `new` / global site, or an enclosing volatile object),
    // marking the vptr stamp, each field-initializer store and the embedded
    // recursion. A volatile-qualified field marks its own stores too.
    void emitClassFieldInitializersImpl(const std::shared_ptr<StructSymbol>& classSym,
                                        IRValue objAddr,
                                        std::set<const StructSymbol*>& visiting,
                                        bool isVolatile = false) {
        if (!classSym || !currentFunction_) return;
        if (!visiting.insert(classSym.get()).second) return;
        // CIDER-BUG-089: the class's first word is its vptr, so stamp it
        // before the field stores (the same position `new` used to emit it
        // from). Nested class values recurse below and stamp their own
        // addresses, including through struct fields and fixed arrays.
        emitClassVptrStore(classSym, objAddr, isVolatile);
        std::string classMod = definingModuleFor(classSym, classSym->name);
        for (const auto& field : classSym->fields) {
            Expression* init = field.initializer;
            // CIDER-BUG-090: an inherited field's type spelling belongs to
            // the base's module; resolving it in the derived class's module
            // loses the layout (and the embedded initializers) entirely.
            std::string fieldMod = fieldTypeModule(field, classMod);
            TypeInfo fieldType =
                canonicalizeNominalInModule(resolveTypeAlias(field.type, fieldMod), fieldMod);
            // CIDER-BUG-144: a volatile destination marks every member, and
            // a volatile-qualified field is itself a volatile object.
            const bool fieldVolatile = isVolatile || fieldType.isVolatile;
            // CIDER-BUG-082/087: an embedded class value lives in a class
            // field, a struct field or a fixed array (any nesting), so run
            // the inner initializers at the field's address before this
            // class's own stores — matching a direct `var inner: Inner;`
            // local / `new Inner`. No IR is emitted when the nested
            // aggregate is statically all-zero.
            emitEmbeddedInitializers(fieldType, objAddr, field.offset, fieldMod, visiting,
                                     fieldVolatile);
            if (!init) continue;  // default zero already stored by emitZeroInstance
            IRType fieldIR = (fieldType.pointerLevel > 0) ? IRType::PTR : mapType(fieldType.name);
            bool ptrLike = fieldType.pointerLevel > 0 || fieldType.isFunctionPointer ||
                           fieldType.name == "string";
            auto storeField = [&](IRValue val) {
                currentBlock_->addInstruction(
                    {IROp::STORE, std::nullopt,
                     {objAddr, val, IRValue::Constant(field.offset, IRType::I32)}, fieldVolatile});
            };
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
        visiting.erase(classSym.get());
    }

    // CIDER-FEAT-003: run a class's `func init(...)` constructor on a fresh
    // object. Constructors are never virtual (the collector rejects
    // virtual/override `init`), so this is a direct call with `this` as the
    // hidden first argument. Called after the zero/field-initializer stores
    // so the ordering is: zero defaults -> 049 field initializers -> body.
    //
    // CIDER-BUG-146 (investigated, documented decision): `new volatile C(...)`
    // marks every caller-side formation store (the zero, vptr and field
    // initializers — CIDER-BUG-144) but the constructor BODY still writes
    // `this` fields plainly. The constructor is ONE shared callee: its
    // hidden `this` parameter is a plain `C*` in the generated function
    // (visit(FunctionDeclStmt)), and IR CALL carries no per-argument
    // volatility — the same architecture limit the CIDER-BUG-133 note in
    // convertCallArgs records for by-value arguments. Supporting it would
    // require either emitting a per-call-site volatile-`this` clone of every
    // constructor (code duplication plus a dispatch decision for every
    // call shape) or a new ABI/parameter flag on the hidden `this`
    // (explicitly out of scope; there is no user syntax to declare it).
    // `volatile` on the constructor's own parameters already marks the
    // writes to those parameters; `this` has no such spelling. Decision:
    // document, do not thread — no ABI flag, no cloning.
    void emitConstructorCall(const std::shared_ptr<StructSymbol>& classSym, IRValue objPtr,
                             const std::vector<std::unique_ptr<Expression>>& argExprs) {
        auto ctorDef = resolveFunctionDef(classSym->constructorName,
                                          definingScopeFor(classSym, classSym->name));
        if (!ctorDef.first && !classSym->manglePrefix.empty()) {
            // CIDER-BUG-119: a function/block-local class's constructor
            // FunctionSymbol was defined in the collector's transient local
            // scope; rebuild it from the recorded signature (the definition
            // itself is still emitted from the class body).
            auto fs = std::make_shared<FunctionSymbol>();
            fs->kind = SymbolKind::FUNCTION;
            fs->name = classSym->constructorName;
            fs->returnType = {"void", 0};
            fs->parameterTypes = classSym->constructorParamTypes;
            fs->parameterTypes.insert(fs->parameterTypes.begin(), {classSym->name, 1});
            ctorDef = {fs, definingModuleFor(classSym, classSym->name)};
        }
        if (!ctorDef.first) {
            if (reporter_) {
                reporter_->error("Constructor '" + classSym->constructorName +
                                     "' has no definition",
                                 classSym->line);
            }
            return;
        }
        std::vector<IRValue> args;
        args.push_back(objPtr);
        // CIDER-BUG-080: evaluated argument types drive interface argument
        // conversion (a concrete value becomes a call-site fat pointer).
        // CIDER-BUG-165: the constructor's declared parameter shapes are
        // rooted here first, so a list argument is materialised against its
        // parameter's array shape.
        std::vector<TypeInfo> argTypes;
        std::vector<TypeInfo> ctorParams =
            rootCallParams(ctorDef.first->parameterTypes, ctorDef.second);
        for (size_t ai = 0; ai < argExprs.size(); ++ai) {
            const TypeInfo* paramT = (1 + ai < ctorParams.size()) ? &ctorParams[1 + ai] : nullptr;
            args.push_back(evaluateCallArg(argExprs[ai].get(), paramT));
            argTypes.push_back(lastExprType_);
        }
        // parameterTypes[0] is the hidden `this`; args[0] is its value, so
        // conversion starts at the first real parameter on both sides.
        convertCallArgs(args, 1, ctorParams, 1, &argTypes);
        IRInstruction call;
        call.op = IROp::CALL;
        call.dest = std::nullopt;
        call.callAbi = ctorDef.first->abi;
        call.args.push_back(
            IRValue::Label(emitFunctionName(ctorDef.second, classSym->constructorName)));
        for (auto& a : args) call.args.push_back(a);
        currentBlock_->addInstruction(call);
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

        // CIDER-BUG-067: start from an all-zero instance before the vptr and
        // the field initializers, so aggregate fields (struct / class-value /
        // array / interface) read as zero even when escape analysis rewrites
        // this malloc to a stack slot.
        // CIDER-BUG-144: `new volatile V` allocates a volatile object, so
        // its formation stores are observable accesses. The flag comes from
        // the alias-resolved `new` type (mirroring the checker and the copy
        // sites), covering `type VV = volatile V; new VV`.
        // CIDER-BUG-146: the constructor BODY is out of this flag's reach —
        // see the documented decision in emitConstructorCall.
        const bool newIsVolatile = resolveTypeAlias(type, currentModule_).isVolatile;
        emitZeroInstance(dest, size, newIsVolatile);

        // CIDER-FEAT-003: resolve alias-aware (matching the checker), so
        // `type A = C; new A(...)` gets the same vptr, field initializers
        // and constructor as `new C(...)` instead of silently skipping them.
        // CIDER-BUG-089: the class's vptr store is part of the shared
        // formation pass (emitClassFieldInitializersImpl), so `new` and
        // value locals take the same path instead of duplicating the store.
        auto sym = resolveStructOrClassSymbol(type);
        if (sym && sym->kind == SymbolKind::CLASS) {
            auto classSym = std::static_pointer_cast<StructSymbol>(sym);
            // CIDER-BUG-049: run the class's field initializers (inherited
            // fields included, in layout order).
            emitClassFieldInitializers(classSym, dest, newIsVolatile);
            // CIDER-FEAT-003: the constructor body runs last, with `this`
            // bound to the fresh object.
            if (!classSym->constructorName.empty()) {
                emitConstructorCall(classSym, dest, expr.getArgs());
            }
        } else if (sym && sym->kind == SymbolKind::STRUCT) {
            // CIDER-BUG-087: `new S` is a fresh struct value, so its
            // embedded class-valued fields run their initializers on the
            // zeroed allocation, exactly like a declared local.
            std::set<const StructSymbol*> visiting;
            if (aggregateNeedsInit(type, "", visiting)) {
                emitEmbeddedInitializers(type, dest, 0, "", visiting, newIsVolatile);
            }
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
            // CIDER-BUG-108: a heap target runs the same finalizer set a
            // scope-exit local of its type does — the class's own destructor
            // body first, then every class value it embeds by value in
            // reverse construction order (the CIDER-BUG-088 ordering), and
            // for a struct target its embedded class values. A transient
            // destructor scope reuses registerEmbeddedDestructors /
            // emitDestructors, so the heap and scope paths cannot disagree;
            // a class carrying only its own destructor emits exactly the old
            // single offset-0 call. CID-020/021: resolution is module-aware
            // through resolveStructOrClassSymbol, so a qualified-typed
            // object still finds its module class's destructor.
            std::set<const StructSymbol*> visiting;
            if (aggregateHasDestructors(baseType, currentModule_, visiting)) {
                destructorStack_.push_back({});
                registerEmbeddedDestructors(baseType, ptr, 0, currentModule_, visiting);
                emitDestructors(destructorStack_.size() - 1);
                destructorStack_.pop_back();
            }
        }
        IRInstruction call;
        call.op = IROp::CALL;
        call.dest = std::nullopt;
        call.args.push_back(IRValue::Label(runtimeHelperName("free")));
        call.args.push_back(ptr);
        currentBlock_->addInstruction(call);
    }
    // CIDER-ISS-001: `--checks` bounds guard for a fixed-size array index.
    // Emitted for an array VALUE with a statically known length (single or
    // nested dimension): `idx < 0 || idx >= count` branches to a call of
    // the `_bounds_fail` runtime routine; the normal load/store then
    // proceeds in a fresh block. Pointer indexing is deliberately NOT
    // checked — the pointee extent is not known, so no static bound exists.
    // A compile-time constant index in range costs nothing; an out-of-range
    // constant routes straight to the trap. `idx` is evaluated exactly once
    // by the caller. Comparisons use 64-bit constants so every index width
    // widens consistently on both backends (see codegen widenCmpOp).
    void emitBoundsCheck(const IRValue& idx, int64_t count) {
        if (!checks_ || count <= 0) return;
        IRType it = idx.type;
        if (it == IRType::F32 || it == IRType::F64) return;  // not an integer index
        bool idxUnsigned = (it == IRType::U8 || it == IRType::U16 ||
                            it == IRType::U32 || it == IRType::U64 ||
                            it == IRType::PTR || it == IRType::BOOL);
        auto failCall = [&]() {
            IRInstruction call;
            call.op = IROp::CALL;
            call.dest = std::nullopt;
            call.args.push_back(IRValue::Label("_bounds_fail"));
            currentBlock_->addInstruction(call);
        };
        if (idx.kind == IRValue::CONSTANT) {
            bool bad = idxUnsigned ? (uint64_t)idx.intValue >= (uint64_t)count
                                   : (idx.intValue < 0 || idx.intValue >= count);
            if (!bad) return;
            // Known out of range: unconditional trap, then a fresh block so
            // the normal indexing sequence still has a well-formed block
            // (only reachable if a user-defined _bounds_fail returns).
            std::string resumeName = nextLabel("bounds_ok");
            failCall();
            currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(resumeName)}});
            startBlock(resumeName);
            return;
        }
        std::string failName = nextLabel("bounds_fail");
        std::string okName = nextLabel("bounds_ok");
        if (!idxUnsigned) {
            std::string loOkName = nextLabel("bounds_lo_ok");
            IRValue below = IRValue::Register(nextTemp(), IRType::BOOL);
            currentBlock_->addInstruction(
                {IROp::CMP_LT, below, {idx, IRValue::Constant(0, IRType::I64)}});
            currentBlock_->addInstruction({IROp::COND_BR, std::nullopt,
                                           {below, IRValue::Label(failName),
                                            IRValue::Label(loOkName)}});
            startBlock(loOkName);
        }
        IRValue over = IRValue::Register(nextTemp(), IRType::BOOL);
        currentBlock_->addInstruction(
            {IROp::CMP_GE, over, {idx, IRValue::Constant(count, IRType::I64)}});
        currentBlock_->addInstruction({IROp::COND_BR, std::nullopt,
                                       {over, IRValue::Label(failName),
                                        IRValue::Label(okName)}});
        startBlock(failName);
        failCall();
        // The routine never returns on VM/native; the branch keeps the CFG
        // well-formed (and resumes the normal path if a user-defined
        // _bounds_fail returns).
        currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(okName)}});
        startBlock(okName);
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
        // CIDER-ISS-001: --checks guards an index into a fixed-size array
        // VALUE against the array's static length before the scaled address
        // is formed. A pointer base (pointerLevel > 0) stays unchecked: its
        // pointee extent is unknown; so do slices and strings.
        if (baseType.isArray && baseType.pointerLevel == 0 && baseType.arraySize > 0) {
            emitBoundsCheck(idx, baseType.arraySize);
        }
        TypeInfo elemType = baseType;
        if (elemType.isArray) {
            // CIDER-BUG-052: elementType() keeps the inner dimension of a
            // nested fixed array (its size is the indexing stride) and
            // yields the flat element for a single-dimension array.
            elemType = elemType.elementType();
        } else if (elemType.pointerLevel > 0) {
            elemType.pointerLevel--;
        } else if (elemType.name == "string") {
            elemType.name = "char";
        }
        elemType = resolveTypeAlias(elemType);
        // CIDER-BUG-069: an element of an interface-typed array/pointer is
        // the same 16-byte fat pointer as an interface variable.
        elemType = resolveInterfaceNominal(elemType);

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
        // CIDER-BUG-052: an inner array element is address-like, exactly
        // like a top-level array value — loading it would read the first
        // element's bytes as a pointer. CIDER-BUG-069: an interface element
        // is address-like too (the fat pointer's address feeds dispatch).
        if (isAddress_ || elemIsStruct || elemType.isArray || elemType.isInterface) {
            lastValue_ = addr;
        } else {
            IRType irType = (elemType.pointerLevel > 0 || elemType.isFunctionPointer)
                                ? IRType::PTR
                                : mapType(elemType.name);
            IRValue dest = IRValue::Register(nextTemp(), irType);
            // CIDER-BUG-061: an element of a volatile array/pointer read is
            // observable.
            currentBlock_->addInstruction({IROp::LOAD, dest, {addr, IRValue::Constant(0, IRType::I32)},
                                           elemType.isVolatile});
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

        // CIDER-BUG-068: a cast between function types reinterprets the
        // pointer but not its runtime shape. A closure object cast to a plain
        // `fn(...)` spelling (direct or through a `type` alias) stays a
        // closure, so calls through it take the closure path (the checker's
        // representation inference follows the same source-shape rule).
        TypeInfo resolvedTarget = resolveFunctionPointerAlias(targetType);
        if (!resolvedTarget.isClosure && resolvedTarget.isFunctionPointer &&
            !resolvedTarget.isArray && !resolvedTarget.isMsAbi && lastExprType_.isClosure) {
            targetType.isClosure = true;
        }
        lastExprType_ = targetType;
    }

    void visit(MatchStmt& stmt) override {
        stmt.getExpression()->accept(*this);
        IRValue matchVal = lastValue_;
        // CIDER-BUG-055: the scrutinee type decides whether a pattern is
        // compared by content; captured before any pattern is evaluated.
        TypeInfo matchType = lastExprType_;
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
                TypeInfo patternType = lastExprType_;
                IRValue cond = IRValue::Register(nextTemp(), IRType::BOOL);
                // CIDER-BUG-055: `match` over a `string` (name "string",
                // pointer level 0) must test the pointed-to bytes, not the
                // addresses; pattern arms used to compare LITERAL addresses
                // and silently fell through to `default`. Reuse the
                // CIDER-BUG-056 `_streq` helper, so arm selection and `==`
                // agree. Explicit pointer patterns (`*u8`/`char*`) keep
                // their identity comparison, exactly as in `==`.
                if (matchType.name == "string" && matchType.pointerLevel == 0 &&
                    patternType.name == "string" && patternType.pointerLevel == 0) {
                    ensureStringEqualityHelpers();
                    IRValue eq = emitCall("_streq", {matchVal, patternVal});
                    currentBlock_->addInstruction({IROp::CMP_NE, cond, {eq, IRValue::Constant(0, IRType::I32)}});
                } else {
                    currentBlock_->addInstruction({IROp::CMP_EQ, cond, {matchVal, patternVal}});
                }
                currentBlock_->addInstruction({IROp::COND_BR, std::nullopt, {cond, IRValue::Label(armName), IRValue::Label(nextCheckName)}});
            }
            startBlock(armName);
            // CIDER-BUG-050: an arm body is its own scope (an implicit block
            // when written as a single statement), so its defers run at that
            // arm's end rather than at the enclosing block's exit.
            deferStack_.push_back({});
            destructorStack_.push_back({});
            visitStatement(arm.body.get());
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
        for (auto& s : stmt.getStatements()) visitStatement(s.get());
        for (auto it = deferStack_.back().rbegin(); it != deferStack_.back().rend(); ++it) visitStatement(*it);
        // CIDER-BUG-088: share emitDestructors so embedded-subobject
        // addresses (offset != 0) are handled identically at every exit site.
        emitDestructors(destructorStack_.size() - 1);
        destructorStack_.pop_back();
        deferStack_.pop_back();
        symbolTable_.exitScope();
    }
    void visit(ExpressionStmt& stmt) override { stmt.getExpression()->accept(*this); }
    void visit(ClassDeclStmt& stmt) override {
        // CIDER-BUG-119: install a function/block-local class in the live
        // scope first (see visit(StructDeclStmt)). Skipped when
        // unresolvable (the checker rejects such programs first).
        std::shared_ptr<Symbol> csym;
        if (auto la = localAggregates_.find(&stmt); la != localAggregates_.end()) {
            csym = la->second;
            symbolTable_.define(csym);
        } else {
            csym = symbolTable_.resolve(stmt.getName());
        }
        if (csym && (csym->kind == SymbolKind::STRUCT || csym->kind == SymbolKind::CLASS))
            classSyms_.push_back(std::static_pointer_cast<StructSymbol>(csym));
        std::string oldClass = currentClassName_;
        currentClassName_ = stmt.getName();
        for (auto& member : stmt.getMembers()) {
            if (dynamic_cast<FunctionDeclStmt*>(member.get())) visitStatement(member.get());
        }
        currentClassName_ = oldClass;
    }
    void visit(ExportStmt& stmt) override { if (auto inner = stmt.getStmt()) visitStatement(inner); }
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
        // CIDER-BUG-104: classify the element like the fixed-array
        // declaration path. A by-value aggregate (array/struct/class) or any
        // value wider than a machine word is presented as its source
        // ADDRESS, so the buffer must MEMCPY the element bytes; the legacy
        // scalar STORE put pointer words into the slot. Mirrors
        // CIDER-BUG-083's element-store rule.
        bool elemIsAggregate = isListAggregateElement(elemType);

        // Allocate stack space for all elements
        int totalSize = elemSize * (int)elements.size();
        IRValue addr = IRValue::Register(nextTemp(), IRType::PTR);
        currentBlock_->addInstruction({IROp::ALLOCA, addr, {IRValue::Constant(totalSize, IRType::I32)}});

        // Store first element
        // CIDER-BUG-133: mirror the declaration/assignment list paths — the
        // element source's volatile flag marks an aggregate copy's read.
        emitListElementStore(addr, 0, firstVal, elemType, elemSize, elemIsAggregate,
                             false, lastExprType_.isVolatile);

        // Store remaining elements
        for (size_t i = 1; i < elements.size(); i++) {
            elements[i]->accept(*this);
            if (lastExprType_.name != elemType.name ||
                lastExprType_.pointerLevel != elemType.pointerLevel) {
                lastValue_ = IRValue::Constant(0, IRType::PTR);
                lastExprType_ = {"error", 0};
                return;
            }
            emitListElementStore(addr, (int64_t)i * elemSize, lastValue_,
                                 elemType, elemSize, elemIsAggregate,
                                 false, lastExprType_.isVolatile);
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
            elemType = elemType.elementType();
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
        // CIDER-BUG-155: the block holding the COND_BR dominates both arms.
        // An aggregate-valued result allocates (and zeroes) its merge slot
        // here, before the branch, so both arms copy into one slot and the
        // slot register is set on every path.
        BasicBlock* condBlock = currentBlock_.get();

        startBlock(thenName);
        // CIDER-BUG-137: an arm evaluates on one path only, so a temporary
        // it creates must finalize before the merge or a scope-exit
        // destructor would read a slot the other path never wrote.
        deferStack_.push_back({});
        destructorStack_.push_back({});
        expr.getThenExpr()->accept(*this);
        IRValue thenVal = lastValue_;
        TypeInfo thenType = lastExprType_;
        // CIDER-BUG-155: a by-value aggregate arm yields the address of an
        // arm-scoped temporary. Copying that value into the dominating
        // merge slot makes the arm temporary dead at the arm's end (it can
        // finalize per-arm like a scalar arm's temps, CIDER-BUG-137) while
        // the merged temporary owns the value for the enclosing scope. The
        // slot is zeroed like a declared aggregate local, so a destructor
        // reached after the arm threw (an enclosing catch continues the
        // scope) finalizes zeroed storage, never stack garbage.
        bool aggregateMerge = isByValueAggregate(thenType, currentModule_);
        IRValue mergeSlot;
        int mergeSize = 0;
        if (aggregateMerge) {
            mergeSize = getTypeSize(thenType);
            mergeSlot = IRValue::Register(nextTemp(), IRType::PTR);
            IRValue zeroSrc = IRValue::Register(nextTemp(), IRType::PTR);
            auto& condInstrs = condBlock->getInstructions();
            condInstrs.insert(condInstrs.end() - 1,
                              IRInstruction{IROp::ALLOCA, mergeSlot,
                                            {IRValue::Constant(mergeSize, IRType::I32)}});
            if (mergeSize > 0) {
                condInstrs.insert(condInstrs.end() - 1,
                                  IRInstruction{IROp::MOV, zeroSrc,
                                                {IRValue::Label(zeroBufferLabel(mergeSize))}});
                condInstrs.insert(condInstrs.end() - 1,
                                  IRInstruction{IROp::MEMCPY, std::nullopt,
                                                {mergeSlot, zeroSrc, IRValue::Constant(mergeSize, IRType::I32)}});
            }
            currentBlock_->addInstruction({IROp::MEMCPY, std::nullopt,
                                           {mergeSlot, thenVal, IRValue::Constant(mergeSize, IRType::I32)},
                                           thenType.isVolatile});
        }
        if (aggregateMerge || !isListAggregateElement(thenType)) {
            emitDestructors(destructorStack_.size() - 1);
        }
        destructorStack_.pop_back();
        deferStack_.pop_back();
        BasicBlock* thenBlock = currentBlock_.get();

        startBlock(elseName);
        // Same per-arm temp scope as the then arm.
        deferStack_.push_back({});
        destructorStack_.push_back({});
        expr.getElseExpr()->accept(*this);
        IRValue elseVal = lastValue_;
        TypeInfo elseType = lastExprType_;
        if (aggregateMerge) {
            currentBlock_->addInstruction({IROp::MEMCPY, std::nullopt,
                                           {mergeSlot, elseVal, IRValue::Constant(mergeSize, IRType::I32)},
                                           elseType.isVolatile});
        }
        if (aggregateMerge || !isListAggregateElement(elseType)) {
            emitDestructors(destructorStack_.size() - 1);
        }
        destructorStack_.pop_back();
        deferStack_.pop_back();
        BasicBlock* elseBlock = currentBlock_.get();

        if (aggregateMerge) {
            // CIDER-BUG-155: the merged temporary is the caller's OWN copy
            // (CIDER-BUG-137's ownership rule): it survives the arm that
            // produced it and finalizes at the exit of the enclosing scope,
            // LIFO with the surrounding locals/temporaries. Both arms wrote
            // the one slot, so the result is that slot directly — no PHI of
            // arm addresses (and no dangling alias) is needed. Registration
            // happens after the arm scopes are gone, so it lands in the
            // enclosing scope; aggregates without embedded class destructors
            // register nothing.
            registerTempDestructors(thenType, mergeSlot, currentModule_);
        }

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
        if (!aggregateMerge && floatMerge) {
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
        if (aggregateMerge) {
            lastValue_ = mergeSlot;
            lastExprType_ = thenType;
            return;
        }
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
        // CIDER-BUG-124: the base spelling may be dotted (`lib.Base`) or an
        // alias; resolve it like any other type position (dotted qualifiers
        // through the module registry, alias chains to the class they end
        // at).
        TypeInfo parentType;
        parentType.name = cs->parentName;
        auto parentSym = resolveStructOrClassSymbol(parentType);
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
                // CIDER-BUG-090: an inherited method's return spelling
                // belongs to the class that declared it.
                std::string methodMod =
                    methodModule(m, definingModuleFor(parentSym, ps->name));
                lastExprType_ =
                    canonicalizeNominalInModule(resolveTypeAlias(m.returnType, methodMod), methodMod);
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
        // CIDER-BUG-119: a function/block-local declaration is installed in
        // the live scope from the collector registry first.
        std::shared_ptr<Symbol> ssym;
        if (auto la = localAggregates_.find(&stmt); la != localAggregates_.end()) {
            ssym = la->second;
            symbolTable_.define(ssym);
        } else {
            ssym = symbolTable_.resolve(stmt.getName());
        }
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
            // CIDER-FEAT-006: separate objects must share one handler chain
            // (a try in one object catches a throw from another). `common`
            // symbols are merged by the linker and every reference goes
            // through a relocation, so the slot is genuinely shared; a
            // weak definition in each object would bind locally instead.
            if (separateCompilation_) {
                module_.addGlobalAsm("common _exception_buf_ptr 8");
                module_.addGlobalAsm("common _exception_value 8");
            } else {
                module_.addGlobalAsm("global _exception_buf_ptr");
                module_.addGlobalAsm("_exception_buf_ptr: resq 1");
                module_.addGlobalAsm("global _exception_value");
                module_.addGlobalAsm("_exception_value: resq 1");
            }
        }
    }

private:
    ModuleIR module_;
    SymbolTable& symbolTable_;
    std::unique_ptr<FunctionIR> currentFunction_;
    std::unique_ptr<BasicBlock> currentBlock_;
    std::string currentClassName_;
    std::string currentSourcePath_;
    // CIDER-FEAT-006: separate-compilation mode + per-object data label
    // prefix ("" in whole-program mode keeps output byte-identical).
    bool separateCompilation_ = false;
    std::string separateToken_;
    // CIDER-FEAT-001: line of the statement currently being lowered; stamped
    // on blocks/instructions so `compile -g` can emit DWARF line tables.
    int currentSourceLine_ = 0;
    // CIDER-BUG-024: optional diagnostic sink (nullptr in unit tests).
    ErrorReporter* reporter_ = nullptr;
    // CID-020: resolved-path -> module scope registry (from the collector).
    std::map<std::string, Scope*> moduleScopes_;
    // CID-020/021: imported AST -> logical module name (from the collector).
    std::map<const AstProgram*, std::string> programModules_;
    // CID-020/021: defined symbol -> defining module logical (bind-time
    // provenance from the collector; "" == main).
    std::map<uint64_t, std::string> symbolModules_;
    // CIDER-BUG-095: accepted module-qualified template spellings -> shared
    // bare instantiation name (from the collector; see setter).
    std::map<std::string, std::string> qualifiedTemplateInstantiations_;
    // CIDER-BUG-136: (spelling context module, spelling) -> per-owner
    // canonical instantiation identity (from the collector; see setter).
    std::map<std::string, std::string> templateInstanceIdentities_;
    // CIDER-BUG-119: function/block-local aggregate AST -> collector symbol
    // (see setter).
    std::map<const Statement*, std::shared_ptr<StructSymbol>> localAggregates_;
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
    bool stringEqHelpersEnsured_ = false;  // CIDER-BUG-056 _streq helper
    bool stringCmpHelpersEnsured_ = false;  // CIDER-ISS-012 _strcmp helper
    bool currentIsSret_ = false;
    bool defaultAbiMs_ = false;  // CID-005: --target uefi function-value typing
    bool checks_ = false;        // CIDER-ISS-001: opt-in runtime bounds checks
    TypeInfo currentReturnType_ = {"void", 0};
    int tempCount_ = 0;
    int globalStrCount_ = 0;
    int labelCount_ = 0;
    int anonFuncCount_ = 0;
    int closureCount_ = 0;
    // CIDER-BUG-067: static all-zero blocks keyed by byte size, the source of
    // the whole-instance zeroing at each creation site (one per distinct size).
    std::map<int, std::string> zeroBuffers_;
    // CIDER-BUG-087: module-scope aggregates (class/struct values and fixed
    // arrays of them) whose embedded class-valued fields need a startup
    // pass. Their storage is zero in .bss / the VM's zero-filled globals,
    // so a synthesized function replays the same field-initializer stores a
    // local gets; the program entry calls it before any user code.
    struct PendingGlobalInit {
        TypeInfo type;
        std::string defModule;
        IRValue addr;
        // CIDER-BUG-144: the global's alias-resolved volatile flag, so the
        // startup pass marks its formation stores like a volatile local's.
        bool isVolatile = false;
    };
    std::vector<PendingGlobalInit> pendingGlobalInits_;
    // CIDER-BUG-087: top-level entry functions (main/_start/efi_main)
    // generated in this unit; each runs the global initializer pass.
    std::vector<FunctionIR*> entryFunctions_;
    std::string nextTemp() { return "v" + std::to_string(tempCount_++); }
    std::string nextLabel(const std::string& prefix) {
        return (currentFunction_ ? currentFunction_->getName() + "_" : "") + prefix + std::to_string(labelCount_++);
    }
};

}  // namespace cider
