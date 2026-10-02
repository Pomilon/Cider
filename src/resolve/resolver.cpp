// resolver.cpp — the header pass.
//
// Headers of every module first, so a global or a function may be used above
// its declaration and two functions may call each other. Bodies belong to the
// checker. One visitor class and two ordered phases is what "one traversal"
// honestly means when module scope is order-independent (LAYERS.md §3.5.1).

#include "resolve/resolver.h"

#include "types/context.h"

namespace cider {
namespace resolve {

Resolver::Resolver(TypeContext& ctx, ModuleLoader load)
    : ctx_(ctx), load_(std::move(load)) {}

void Resolver::fail(Position pos, const std::string& message) {
    errors_.push_back(ResolveError{pos, message});
    failed_ = true;
}

const Scope* Resolver::makeScope(const Scope* parent, const std::string& module) {
    auto scope = std::make_unique<Scope>();
    scope->parent = parent;
    scope->module = module;
    const Scope* out = scope.get();
    scopes_.push_back(std::move(scope));
    return out;
}

Decl* Resolver::bind(const Scope* scope, DeclKind kind, const std::string& name,
                     Position pos) {
    auto found = scope->bindings.find(name);
    if (found != scope->bindings.end()) {
        fail(pos, "duplicate declaration of '" + name + "'");
        return const_cast<Decl*>(found->second);
    }
    auto decl = std::make_unique<Decl>();
    decl->kind = kind;
    decl->name = name;
    decl->scope = scope;
    decl->pos = pos;
    Decl* out = decl.get();
    decls_.push_back(std::move(decl));
    const_cast<Scope*>(scope)->bindings[name] = out;
    return out;
}

const Scope* Resolver::scopeOf(const std::string& module) const {
    auto found = modules_.find(module);
    return found == modules_.end() ? nullptr : found->second;
}

const Decl* Resolver::lookup(const Scope* scope, const std::string& name) const {
    for (const Scope* current = scope; current != nullptr; current = current->parent) {
        auto found = current->bindings.find(name);
        if (found != current->bindings.end()) {
            return found->second;
        }
    }
    return nullptr;
}

TypeRef Resolver::typeOf(const Node* node) const {
    auto found = nodeTypes_.find(node);
    if (found == nodeTypes_.end()) {
        return errorRef();
    }
    return found->second;
}

void Resolver::recordType(const Node* node, TypeRef type) {
    nodeTypes_[node] = type;
}

std::string declKey(DeclId id) {
    return std::to_string(id.module) + "." + std::to_string(id.index);
}

TypeRef Resolver::fieldType(DeclId id, const std::string& name) const {
    auto found = fieldTypes_.find(declKey(id) + "." + name);
    if (found == fieldTypes_.end()) {
        return errorRef();
    }
    return found->second;
}

std::vector<std::string> Resolver::fieldNames(DeclId id) const {
    auto found = fieldLists_.find(declKey(id));
    if (found == fieldLists_.end()) {
        return {};
    }
    return found->second;
}

const Signature* Resolver::methodSig(DeclId id, const std::string& name) const {
    auto found = methodSigs_.find(declKey(id) + "." + name);
    return found == methodSigs_.end() ? nullptr : &found->second;
}

std::vector<std::string> Resolver::methodNames(DeclId id) const {
    auto found = methodLists_.find(declKey(id));
    if (found == methodLists_.end()) {
        return {};
    }
    return found->second;
}

std::vector<DeclId> Resolver::classBases(DeclId id) const {
    auto found = baseTable_.find(declKey(id));
    if (found == baseTable_.end()) {
        return {};
    }
    return found->second;
}

int64_t Resolver::enumValue(const std::string& qualifiedType,
                            const std::string& member) const {
    auto found = enumValues_.find(qualifiedType + "." + member);
    return found == enumValues_.end() ? -1 : found->second;
}

std::string Resolver::qualify(const std::string& bare) const {
    if (stack_.empty()) {
        return bare;
    }
    return stack_.back() + "." + bare;
}

// --- Modules ---------------------------------------------------------------

bool Resolver::resolveProgram(Program& program, const std::string& module) {
    if (modules_.find(module) != modules_.end()) {
        return !failed_;
    }
    for (const std::string& active : stack_) {
        if (active == module) {
            std::string cycle = module;
            bool started = false;
            for (const std::string& name : stack_) {
                if (name == module) {
                    started = true;
                }
                if (started) {
                    cycle += " -> " + name;
                }
            }
            cycle += " -> " + module;
            fail(program.pos, "import cycle: " + cycle);
            return false;
        }
    }
    if (moduleIds_.find(module) == moduleIds_.end()) {
        uint64_t id = moduleIds_.size();
        moduleIds_[module] = static_cast<uint32_t>(id);
    }
    stack_.push_back(module);
    programOf_[module] = &program;
    const Scope* scope = makeScope(nullptr, module);
    modules_[module] = scope;
    if (entry_ == nullptr) {
        entry_ = scope;
    }
    bindPrelude(scope, program.pos);
    // Imports first, so a `from` alias and every qualified name resolves
    // against a module whose own headers already exist.
    for (const std::unique_ptr<cider::Decl>& decl : program.decls) {
        if (const auto* from = dynamic_cast<const FromImport*>(decl.get())) {
            if (!resolveModule(from->module)) {
                continue;
            }
            const Scope* other = scopeOf(from->module);
            for (const std::string& name : from->names) {
                const Decl* target = lookup(other, name);
                if (target == nullptr) {
                    fail(decl->pos,
                         "'" + name + "' is not exported by '" + from->module + "'");
                    continue;
                }
                Decl* alias = bind(scope, target->kind, name, decl->pos);
                alias->type = target->type;
                alias->sig = target->sig;
                alias->id = target->id;
                alias->nominal = target->nominal;
                alias->typeParams = target->typeParams;
                alias->aliasTarget = target->aliasTarget;
                alias->init = target->init;
                if (target->kind == DeclKind::Const && target->init != nullptr) {
                    constInits_[module + "." + name] = target->init;
                }
            }
        } else if (const auto* single = dynamic_cast<const ImportDecl*>(decl.get())) {
            if (!resolveModule(single->module)) {
                continue;
            }
            std::string last = single->module;
            std::string::size_type dot = last.rfind('.');
            if (dot != std::string::npos) {
                last = last.substr(dot + 1);
            }
            Decl* mod = bind(scope, DeclKind::Module, last, decl->pos);
            (void)mod;
        }
    }
    declareModule(module, program, scope);
    resolveDeclTypes(module, program, scope);
    stack_.pop_back();
    return !failed_;
}

bool Resolver::resolveModule(const std::string& name) {
    if (modules_.find(name) != modules_.end()) {
        return true;
    }
    if (load_ == nullptr) {
        fail(Position{name, 1, 1}, "cannot find module '" + name + "'");
        return false;
    }
    std::unique_ptr<Program> program = load_(name);
    if (program == nullptr) {
        fail(Position{name, 1, 1}, "cannot find module '" + name + "'");
        return false;
    }
    Program* raw = program.get();
    programs_[name] = std::move(program);
    return resolveProgram(*raw, name);
}

// --- The header pass ---------------------------------------------------------

void Resolver::declareModule(const std::string& module, Program& program,
                             const Scope* scope) {
    uint32_t modId = moduleIds_[module];
    uint64_t next = 0;
    for (const std::unique_ptr<cider::Decl>& decl : program.decls) {
        if (const auto* fn = dynamic_cast<const FuncDecl*>(decl.get())) {
            Decl* d = bind(scope, DeclKind::Func, fn->name, fn->pos);
            d->type = TypeRef{ctx_.wildcard()};
            d->id = DeclId{modId, static_cast<uint32_t>(next)};
            ++next;
            continue;
        }
        if (const auto* ext = dynamic_cast<const ExternFunc*>(decl.get())) {
            Decl* d = bind(scope, DeclKind::Func, ext->name, ext->pos);
            d->type = TypeRef{ctx_.wildcard()};
            continue;
        }
        if (const auto* cn = dynamic_cast<const ConstDecl*>(decl.get())) {
            Decl* d = bind(scope, DeclKind::Const, cn->name, cn->pos);
            d->type = TypeRef{ctx_.wildcard()};
            d->init = cn->init.get();
            constInits_[module + "." + cn->name] = cn->init.get();
            continue;
        }
        const StructDecl* sd = dynamic_cast<const StructDecl*>(decl.get());
        const ClassDecl* cd = dynamic_cast<const ClassDecl*>(decl.get());
        const InterfaceDecl* id = dynamic_cast<const InterfaceDecl*>(decl.get());
        const UnionDecl* ud = dynamic_cast<const UnionDecl*>(decl.get());
        const EnumDecl* ed = dynamic_cast<const EnumDecl*>(decl.get());
        const TypeAlias* al = dynamic_cast<const TypeAlias*>(decl.get());
        if (sd == nullptr && cd == nullptr && id == nullptr && ud == nullptr &&
            ed == nullptr && al == nullptr) {
            continue;
        }
        std::string name;
        Kind nominal = Kind::Error;
        uint64_t tps = 0;
        if (sd != nullptr) {
            name = sd->name;
            nominal = Kind::Struct;
            tps = sd->typeParams.size();
        } else if (cd != nullptr) {
            name = cd->name;
            nominal = Kind::Class;
            tps = 0;
        } else if (id != nullptr) {
            name = id->name;
            nominal = Kind::Interface;
        } else if (ud != nullptr) {
            name = ud->name;
            nominal = Kind::Union;
        } else if (ed != nullptr) {
            name = ed->name;
            nominal = Kind::Enum;
        }
        if (al != nullptr) {
            Decl* d = bind(scope, DeclKind::Alias, al->name, al->pos);
            d->type = TypeRef{ctx_.wildcard()};
            d->typeParams = al->typeParams.size();
            d->aliasTarget = al->target.get();
            d->id = DeclId{modId, static_cast<uint32_t>(next)};
            ++next;
            continue;
        }
        Decl* d = bind(scope, DeclKind::Type, name, decl->pos);
        d->nominal = nominal;
        d->id = DeclId{modId, static_cast<uint32_t>(next)};
        ++next;
        d->typeParams = tps;
        d->type = TypeRef{ctx_.nominal(nominal, d->id)};
    }
    for (const std::unique_ptr<VarDecl>& global : program.globals) {
        Decl* d = bind(scope, DeclKind::Var, global->name, global->pos);
        d->type = TypeRef{ctx_.wildcard()};
    }
}

void Resolver::resolveDeclTypes(const std::string& module, Program& program,
                                const Scope* scope) {
    EvalEnv env = evalEnv(scope);
    for (const std::unique_ptr<cider::Decl>& decl : program.decls) {
        if (const auto* fn = dynamic_cast<const FuncDecl*>(decl.get())) {
            const Decl* d = lookup(scope, fn->name);
            if (d == nullptr) {
                continue;
            }
            const Scope* inner = scope;
            if (!fn->typeParams.empty()) {
                inner = typeParamScope(scope, module, fn->typeParams, d->id, fn->pos);
            }
            bool failed = false;
            Signature sig = buildSignature(fn->params, fn->returns.get(), inner, failed);
            const_cast<Decl*>(d)->sig = std::move(sig);
            continue;
        }
        if (const auto* ext = dynamic_cast<const ExternFunc*>(decl.get())) {
            const Decl* d = lookup(scope, ext->name);
            if (d == nullptr) {
                continue;
            }
            bool failed = false;
            Signature sig = buildSignature(ext->params, ext->returns.get(), scope, failed);
            const_cast<Decl*>(d)->sig = std::move(sig);
            continue;
        }
        if (const auto* cn = dynamic_cast<const ConstDecl*>(decl.get())) {
            if (cn->type != nullptr) {
                TypeRef t = resolveType(cn->type.get(), scope);
                const Decl* d = lookup(scope, cn->name);
                if (d != nullptr && !t.isError()) {
                    const_cast<Decl*>(d)->type = t;
                }
            }
            std::unordered_set<std::string> active;
            EvalResult folded = evalConstInt(*cn->init, env, active);
            if (!folded.ok) {
                fail(cn->init->pos, folded.error);
            }
            continue;
        }
        if (const auto* sd = dynamic_cast<const StructDecl*>(decl.get())) {
            const Decl* self = lookup(scope, sd->name);
            const Scope* inner = scope;
            if (!sd->typeParams.empty() && self != nullptr) {
                inner = typeParamScope(scope, module, sd->typeParams, self->id, sd->pos);
            }
            for (const FieldDecl& field : sd->fields) {
                TypeRef t = resolveType(field.type.get(), inner);
                nodeTypes_[&field] = t;
                if (self != nullptr && !t.isError()) {
                    std::string fkey = declKey(self->id);
                    fieldTypes_[fkey + "." + field.name] = t;
                    fieldLists_[fkey].push_back(field.name);
                }
            }
            for (const auto& iface : sd->interfaces) {
                TypeRef t = resolveType(iface.get(), inner);
                if (!t.isError() && t.type->kind() != Kind::Interface) {
                    fail(iface->pos, "a struct implements interfaces");
                }
            }
            for (const MethodDecl& method : sd->methods) {
                bool failed = false;
                Signature sig =
                    buildSignature(method.params, method.returns.get(), inner, failed);
                if (self != nullptr && !failed) {
                    std::string key = declKey(self->id);
                    methodSigs_[key + "." + method.name] = std::move(sig);
                    methodLists_[key].push_back(method.name);
                }
            }
            continue;
        }
        if (const auto* cd = dynamic_cast<const ClassDecl*>(decl.get())) {
            const Decl* self = lookup(scope, cd->name);
            const Scope* inner = scope;
            if (!cd->typeParams.empty() && self != nullptr) {
                inner = typeParamScope(scope, module, cd->typeParams, self->id, cd->pos);
            }
            const Decl* cself = lookup(scope, cd->name);
            for (const TypeExpr* base : rawBases(cd)) {
                TypeRef t = resolveType(base, inner);
                if (t.isError()) {
                    continue;
                }
                if (t.type->kind() != Kind::Class) {
                    fail(base->pos, "a class inherits classes");
                    continue;
                }
                if (cself != nullptr) {
                    std::optional<DeclId> baseId = t.type->decl();
                    if (baseId.has_value()) {
                        baseTable_[declKey(cself->id)].push_back(*baseId);
                    }
                }
            }
            for (const FieldDecl& field : cd->fields) {
                TypeRef t = resolveType(field.type.get(), inner);
                nodeTypes_[&field] = t;
                if (cself != nullptr && !t.isError()) {
                    std::string fkey = declKey(cself->id);
                    fieldTypes_[fkey + "." + field.name] = t;
                    fieldLists_[fkey].push_back(field.name);
                }
            }
            for (const MethodDecl& method : cd->methods) {
                bool failed = false;
                Signature sig =
                    buildSignature(method.params, method.returns.get(), inner, failed);
                if (cself != nullptr && !failed) {
                    std::string key = declKey(cself->id);
                    methodSigs_[key + "." + method.name] = std::move(sig);
                    methodLists_[key].push_back(method.name);
                }
            }
            continue;
        }
        if (const auto* id = dynamic_cast<const InterfaceDecl*>(decl.get())) {
            const Decl* iself = lookup(scope, id->name);
            for (const MethodSig& method : id->methods) {
                bool failed = false;
                Signature sig =
                    buildSignature(method.params, method.returns.get(), scope, failed);
                if (iself != nullptr && !failed) {
                    std::string key = declKey(iself->id);
                    methodSigs_[key + "." + method.name] = std::move(sig);
                    methodLists_[key].push_back(method.name);
                }
            }
            continue;
        }
        if (const auto* ud = dynamic_cast<const UnionDecl*>(decl.get())) {
            const Decl* uself = lookup(scope, ud->name);
            for (const FieldDecl& member : ud->members) {
                TypeRef t = resolveType(member.type.get(), scope);
                nodeTypes_[&member] = t;
                if (uself != nullptr && !t.isError()) {
                    std::string fkey = declKey(uself->id);
                    fieldTypes_[fkey + "." + member.name] = t;
                    fieldLists_[fkey].push_back(member.name);
                }
            }
            continue;
        }
        if (const auto* ed = dynamic_cast<const EnumDecl*>(decl.get())) {
            int64_t next = 0;
            bool first = true;
            for (const EnumMember& member : ed->members) {
                int64_t value = 0;
                if (member.value != nullptr) {
                    std::unordered_set<std::string> active;
                    EvalResult folded = evalConstInt(*member.value, env, active);
                    if (!folded.ok) {
                        fail(member.value->pos, folded.error);
                        continue;
                    }
                    value = folded.value;
                } else if (first) {
                    value = 0;
                } else {
                    value = next;
                }
                first = false;
                next = value + 1;
                enumValues_[module + "." + ed->name + "." + member.name] = value;
            }
            continue;
        }
        if (const auto* al = dynamic_cast<const TypeAlias*>(decl.get())) {
            const Decl* d = lookup(scope, al->name);
            const Scope* inner = scope;
            if (!al->typeParams.empty() && d != nullptr) {
                inner = typeParamScope(scope, module, al->typeParams, d->id, al->pos);
            }
            TypeRef t = resolveType(al->target.get(), inner);
            nodeTypes_[al] = t;
            continue;
        }
    }
    for (const std::unique_ptr<VarDecl>& global : program.globals) {
        TypeRef t = resolveType(global->type.get(), scope);
        const Decl* d = lookup(scope, global->name);
        if (d != nullptr && !t.isError()) {
            const_cast<Decl*>(d)->type = t;
        }
        nodeTypes_[global.get()] = t;
    }
}

// The bases of a class, as spellings. A helper because ClassDecl holds them
// behind unique_ptrs and the loop above wants raw spellings.
std::vector<const TypeExpr*> Resolver::rawBases(const ClassDecl* decl) {
    std::vector<const TypeExpr*> out;
    for (const std::unique_ptr<TypeExpr>& base : decl->bases) {
        out.push_back(base.get());
    }
    return out;
}

// --- resolve: the total function ------------------------------------------------

TypeRef Resolver::resolveType(const TypeExpr* spelling, const Scope* scope) {
    if (spelling == nullptr) {
        return errorRef();
    }
    if (const auto* basic = dynamic_cast<const BasicType*>(spelling)) {
        return TypeRef{ctx_.basic(basic->basic)};
    }
    if (const auto* ptr = dynamic_cast<const PointerType*>(spelling)) {
        if (dynamic_cast<const FuncSigType*>(ptr->pointee.get()) != nullptr) {
            const auto* sig = static_cast<const FuncSigType*>(ptr->pointee.get());
            return resolveSignature(sig, scope, true);
        }
        TypeRef inner = resolveType(ptr->pointee.get(), scope);
        if (inner.isError()) {
            return inner;
        }
        return TypeRef{ctx_.pointer(inner.type)};
    }
    if (const auto* arr = dynamic_cast<const ArrayType*>(spelling)) {
        TypeRef element = resolveType(arr->element.get(), scope);
        if (element.isError()) {
            return element;
        }
        EvalEnv env = evalEnv(scope);
        std::unordered_set<std::string> active;
        EvalResult length = evalConstInt(*arr->length, env, active);
        if (!length.ok) {
            fail(arr->length->pos, length.error);
            return errorRef();
        }
        if (length.value < 0) {
            fail(arr->length->pos, "an array length is not negative");
            return errorRef();
        }
        return TypeRef{ctx_.array(element.type, length.value)};
    }
    if (const auto* slice = dynamic_cast<const SliceType*>(spelling)) {
        TypeRef element = resolveType(slice->element.get(), scope);
        if (element.isError()) {
            return element;
        }
        return TypeRef{ctx_.slice(element.type)};
    }
    if (const auto* named = dynamic_cast<const NamedType*>(spelling)) {
        return resolveNamed(named, scope);
    }
    if (const auto* sig = dynamic_cast<const FuncSigType*>(spelling)) {
        return resolveSignature(sig, scope, false);
    }
    fail(spelling->pos, "not a type spelling");
    return errorRef();
}

TypeRef Resolver::resolveNamed(const NamedType* type, const Scope* scope) {
    const Scope* home = scope;
    if (!type->qualifier.empty()) {
        home = scopeOf(type->qualifier);
        if (home == nullptr) {
            fail(type->pos, "unknown module '" + type->qualifier + "'");
            return errorRef();
        }
    }
    if (type->qualifier.empty() && type->args.empty()) {
        if (type->name == "string") {
            return TypeRef{ctx_.string()};
        }
        if (type->name == "void") {
            return TypeRef{ctx_.void_()};
        }
    }
    const Decl* decl = lookup(home, type->name);
    if (decl == nullptr) {
        fail(type->pos, "unknown type '" + type->name + "'");
        return errorRef();
    }
    if (decl->kind == DeclKind::TypeParam) {
        if (!type->args.empty()) {
            fail(type->pos, "a type parameter takes no arguments");
            return errorRef();
        }
        return TypeRef{ctx_.typeParam(decl->templateId, decl->paramIndex)};
    }
    if (decl->kind == DeclKind::Alias) {
        std::string qualified = home->module + "." + decl->name;
        if (aliasActive_.find(qualified) != aliasActive_.end()) {
            fail(type->pos, "type alias cycle: " + qualified);
            return errorRef();
        }
        aliasActive_.insert(qualified);
        TypeRef target = resolveType(decl->aliasTarget, home);
        aliasActive_.erase(qualified);
        return target;
    }
    if (decl->kind != DeclKind::Type) {
        fail(type->pos, "'" + type->name + "' is not a type");
        return errorRef();
    }
    if (decl->typeParams > 0) {
        if (type->args.size() != decl->typeParams) {
            fail(type->pos,
                 "'" + type->name + "' takes " + std::to_string(decl->typeParams) +
                     " arguments");
            return errorRef();
        }
        for (const std::unique_ptr<TypeExpr>& arg : type->args) {
            TypeRef resolved = resolveType(arg.get(), scope);
            if (resolved.isError()) {
                return resolved;
            }
        }
        return TypeRef{ctx_.wildcard()};
    }
    if (!type->args.empty()) {
        fail(type->pos, "'" + type->name + "' takes no arguments");
        return errorRef();
    }
    return TypeRef{ctx_.nominal(decl->nominal, decl->id)};
}

TypeRef Resolver::resolveSignature(const FuncSigType* type, const Scope* scope,
                                  bool inPointer) {
    bool failed = false;
    Signature sig = buildFuncSig(type->params, type->returns.get(), scope, failed);
    if (failed) {
        return errorRef();
    }
    if (inPointer) {
        return TypeRef{ctx_.functionPtr(std::move(sig))};
    }
    return TypeRef{ctx_.closure(std::move(sig))};
}

Signature Resolver::buildSignature(const std::vector<Param>& params,
                                   const TypeExpr* returns, const Scope* scope,
                                   bool& failed) {
    std::vector<Parameter> out;
    for (const Param& param : params) {
        TypeRef t = resolveType(param.type.get(), scope);
        if (t.isError()) {
            failed = true;
            continue;
        }
        Parameter p;
        p.type = t.type;
        p.isMutable = param.isMutable;
        out.push_back(p);
    }
    const Type* ret = ctx_.void_();
    if (returns != nullptr) {
        TypeRef t = resolveType(returns, scope);
        if (t.isError()) {
            failed = true;
        } else {
            ret = t.type;
        }
    }
    return Signature(std::move(out), ret, Abi::SysV);
}

void Resolver::buildMethodSig(const MethodDecl& method, const Scope* scope, bool& failed) {
    Signature sig = buildSignature(method.params, method.returns.get(), scope, failed);
    (void)sig;
}

Signature Resolver::buildFuncSig(const std::vector<std::unique_ptr<TypeExpr>>& params,
                                 const TypeExpr* returns, const Scope* scope,
                                 bool& failed) {
    std::vector<Parameter> out;
    for (const std::unique_ptr<TypeExpr>& param : params) {
        TypeRef t = resolveType(param.get(), scope);
        if (t.isError()) {
            failed = true;
            continue;
        }
        Parameter p;
        p.type = t.type;
        out.push_back(p);
    }
    const Type* ret = ctx_.void_();
    if (returns != nullptr) {
        TypeRef t = resolveType(returns, scope);
        if (t.isError()) {
            failed = true;
        } else {
            ret = t.type;
        }
    }
    return Signature(std::move(out), ret, Abi::SysV);
}

const Scope* Resolver::typeParamScope(const Scope* parent, const std::string& module,
                                        const std::vector<std::string>& names,
                                        DeclId templateId, Position pos) {
    const Scope* scope = makeScope(parent, module);
    uint32_t index = 0;
    for (const std::string& name : names) {
        Decl* d = bind(scope, DeclKind::TypeParam, name, pos);
        d->templateId = templateId;
        d->paramIndex = index;
        ++index;
    }
    return scope;
}

void Resolver::bindPrelude(const Scope* scope, Position pos) {
    const Type* string = ctx_.string();
    const Type* none = ctx_.void_();
    const Type* i64 = ctx_.basic(BasicKind::I64);
    const Type* addr = ctx_.pointer(none);
    auto func = [&](const std::string& name, std::vector<Parameter> params,
                    const Type* ret) {
        Decl* d = bind(scope, DeclKind::Func, name, pos);
        d->type = TypeRef{ctx_.wildcard()};
        d->sig = Signature(std::move(params), ret, Abi::SysV);
        d->isPrelude = true;
    };
    func("print", {{string, false}}, none);
    func("panic", {{string, false}}, none);
    func("alloc", {{i64, false}, {i64, false}}, addr);
    func("realloc", {{addr, false}, {i64, false}, {i64, false}}, addr);
    func("free", {{addr, false}}, none);
}

EvalEnv Resolver::evalEnv(const Scope* scope) {
    // Built field by field: EvalEnv holds a reference and cannot be copied, so
    // it is constructed and filled rather than aggregate-initialised.
    EvalEnv env(ctx_);
    env.constantInit = [this](const std::string& qualified) -> const Expr* {
        auto found = constInits_.find(qualified);
        return found == constInits_.end() ? nullptr : found->second;
    };
    env.resolve = [this, scope](const TypeExpr* spelling) -> TypeRef {
        return resolveType(spelling, scope);
    };
    // The scope's own module, not the resolver's stack: by the time a body is
    // checked the header pass has unwound, and `main.x` must mean main's x.
    env.qualify = [scope](const std::string& bare) -> std::string {
        return scope->module + "." + bare;
    };
    return env;
}

}  // namespace resolve
}  // namespace cider
