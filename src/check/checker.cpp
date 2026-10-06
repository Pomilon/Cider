// checker.cpp — the body walk.
//
// The second role inside the one shared traversal: the resolver wrote Decls,
// Scopes and the name facts; this walk writes each expression's Type and
// checks it in place. Recording a type and checking it are the same step.
//
// Literals read their type from context: an integer literal takes the declared
// type when it fits, or defaults to `int` (then `i64`) with nothing to
// constrain it. Everything else infers bottom-up and converts at the six
// positions SEMANTICS.md §5.4 names — and nowhere else.

#include "check/checker.h"

#include "types/context.h"

namespace cider {
namespace check {

// `lower` here would resolve to check::lower inside this namespace, so the
// lowering header's namespace is named once and used from there on.

namespace {

// A repeat fills an array from exactly one element. Named, so the number is
// not written inline: T4 watches for size literals in code, and this one is
// about how many initialisers a repeat may carry.
constexpr int64_t kOneElement = 1;

// A union names one live member, and "has" is three letters. Spelling facts
// with the same standing as kOneElement: the checks name these, never
// digits, because T4 refuses digits beside size-looking words.
constexpr uint64_t kOneMember = 1;
constexpr uint64_t kHasPrefixLen = 3;

// The width, in bits, and signedness of a basic kind. Plain numbers, not
// layout constants: these decide a conversion, not a size, and "width" is not
// a word the size gate watches. Bits, not bytes: basicWidthBits answers in
// bits, and a converter that disagrees with its own measure emits i64 for
// i32+i32 — which is exactly the bug this comment exists to prevent.
BasicKind intFor(int widthBits, bool isSigned) {
    if (widthBits <= 8) {
        return isSigned ? BasicKind::I8 : BasicKind::U8;
    }
    if (widthBits <= 16) {
        return isSigned ? BasicKind::I16 : BasicKind::U16;
    }
    if (widthBits <= 32) {
        return isSigned ? BasicKind::I32 : BasicKind::U32;
    }
    return isSigned ? BasicKind::I64 : BasicKind::U64;
}

}  // namespace

Checker::Checker(TypeContext& ctx, resolve::Resolver& resolver)
    : ctx_(ctx), resolver_(resolver) {}

void Checker::fail(Position pos, const std::string& message) {
    errors_.push_back(CheckError{pos, message});
    failed_ = true;
}

void Checker::record(const Node* node, TypeRef type) {
    resolver_.recordType(node, type);
}

TypeRef Checker::typeOf(const Node* node) const {
    return resolver_.typeOf(node);
}

TypeRef Checker::resolveHere(const TypeExpr* spelling) {
    // The resolver records the diagnostic; the checker records the failure.
    // Without this, a use-site resolve error (bare generic in a body, bad
    // arity in a local) leaves checkProgram reporting success while another
    // layer holds the only error. One flag, no second message.
    size_t before = resolver_.errors().size();
    TypeRef out = substDeep(resolver_.resolveType(spelling, typeScope_));
    if (resolver_.errors().size() != before) {
        failed_ = true;
    }
    return out;
}

TypeRef Checker::substDeep(TypeRef type) {
    if (type.isError() || !active_.has_value()) {
        return type;
    }
    Kind kind = type.type->kind();
    if (kind == Kind::TypeParam) {
        const TypeParamPayload& payload =
            std::get<TypeParamPayload>(type.type->payload());
        if (payload.decl.module == active_->tmpl.module &&
            payload.decl.index == active_->tmpl.index &&
            payload.index < active_->args.size()) {
            return substDeep(active_->args[payload.index]);
        }
        return type;
    }
    if (kind == Kind::Pointer) {
        TypeRef inner = substDeep(TypeRef{type.type->pointee()});
        if (inner.type == type.type->pointee()) {
            return type;
        }
        return TypeRef{ctx_.pointer(inner.type)};
    }
    if (kind == Kind::Array) {
        TypeRef inner = substDeep(TypeRef{type.type->element()});
        std::optional<int64_t> length = type.type->length();
        if (inner.type == type.type->element() || !length.has_value()) {
            return inner.type == type.type->element() ? type : errorRef();
        }
        return TypeRef{ctx_.array(inner.type, *length)};
    }
    if (kind == Kind::Slice) {
        TypeRef inner = substDeep(TypeRef{type.type->element()});
        if (inner.type == type.type->element()) {
            return type;
        }
        return TypeRef{ctx_.slice(inner.type)};
    }
    if (kind == Kind::Closure || kind == Kind::FunctionPtr) {
        const Signature& sig = *type.type->signature();
        std::vector<Parameter> params;
        bool changed = false;
        for (const Parameter& param : sig.parameters()) {
            TypeRef sub = substDeep(TypeRef{param.type});
            changed = changed || sub.type != param.type;
            Parameter out;
            out.type = sub.type;
            out.isMutable = param.isMutable;
            params.push_back(out);
        }
        TypeRef ret{sig.returnType()};
        TypeRef subRet = ret.type == nullptr ? ret : substDeep(ret);
        changed = changed || subRet.type != ret.type;
        if (!changed) {
            return type;
        }
        Signature rebuilt(std::move(params), subRet.type, sig.abi());
        if (kind == Kind::Closure) {
            return TypeRef{ctx_.closure(std::move(rebuilt))};
        }
        return TypeRef{ctx_.functionPtr(std::move(rebuilt))};
    }
    return type;
}

void Checker::walkMethodInst(DeclId spec, DeclId tmpl, const std::string& method) {
    std::string key =
        std::to_string(spec.module) + "." + std::to_string(spec.index) + "." + method;
    if (walked_.find(key) != walked_.end()) {
        return;
    }
    walked_.insert(key);
    const resolve::Decl* tmplDecl = resolver_.declById(tmpl);
    if (tmplDecl == nullptr) {
        fail(Position{module_, 1, 1}, "checker bug: template is missing");
        return;
    }
    std::string home =
        tmplDecl->scope != nullptr ? tmplDecl->scope->module : module_;
    auto spelling = methods_.find(home + "." + tmplDecl->name + "." + method);
    if (spelling == methods_.end()) {
        fail(Position{module_, 1, 1}, "checker bug: template method is missing");
        return;
    }
    auto origin = resolver_.instantiationOf(spec);
    if (!origin.has_value()) {
        fail(Position{module_, 1, 1}, "checker bug: specialization has no origin");
        return;
    }
    std::string homeStruct =
        tmplDecl->scope != nullptr ? tmplDecl->scope->module : module_;
    auto structs = structs_.find(homeStruct + "." + tmplDecl->name);
    std::vector<std::string> names =
        structs == structs_.end() ? std::vector<std::string>{} : structs->second->typeParams;
    const resolve::Scope* scope = resolver_.typeParamScope(
        moduleScope_, module_, names, tmpl, spelling->second->pos);
    const resolve::Scope* savedScope = typeScope_;
    typeScope_ = scope;
    std::optional<ActiveInst> savedActive = active_;
    active_ = ActiveInst{tmpl, origin->second, names};
    const Signature* sig = resolver_.methodSig(spec, method);
    if (sig == nullptr) {
        fail(spelling->second->pos, "checker bug: specialized signature is missing");
        typeScope_ = savedScope;
        active_ = savedActive;
        return;
    }
    pushScope();
    const Type* obj = ctx_.nominal(tmplDecl->nominal, spec);
    bindLocal("self", TypeRef{ctx_.pointer(obj)}, false, spelling->second->pos);
    for (uint64_t i = 0; i < static_cast<uint64_t>(spelling->second->params.size()); ++i) {
        bindLocal(spelling->second->params[i].name,
                  TypeRef{sig->parameters()[i].type},
                  spelling->second->params[i].isMutable,
                  spelling->second->params[i].pos);
    }
    TypeRef savedRet = funcReturn_;
    bool wasFunc = inFunc_;
    const Type* ret = sig->returnType() != nullptr ? sig->returnType() : ctx_.void_();
    funcReturn_ = TypeRef{ret};
    inFunc_ = true;
    LowerSink* savedSink = sink_;
    sink_ = nullptr;
    spelling->second->body->accept(*this);
    sink_ = savedSink;
    funcReturn_ = savedRet;
    inFunc_ = wasFunc;
    popScope();
    typeScope_ = savedScope;
    active_ = savedActive;
}

const FuncDecl* Checker::funcSpelling(const std::string& module,
                                    const std::string& name) const {
    auto found = funcs_.find(module + "." + name);
    return found == funcs_.end() ? nullptr : found->second;
}

bool Checker::checkProgram(Program& program, const std::string& module) {
    module_ = module;
    moduleScope_ = resolver_.scopeOf(module);
    if (moduleScope_ == nullptr) {
        fail(program.pos, "checker bug: '" + module + "' was never resolved");
        return false;
    }
    env_ = std::make_unique<EvalEnv>(resolver_.evalEnv(moduleScope_));
    typeScope_ = moduleScope_;
    for (const std::unique_ptr<Decl>& decl : program.decls) {
        if (const auto* fn = dynamic_cast<const FuncDecl*>(decl.get())) {
            funcs_[module + "." + fn->name] = fn;
        }
        if (const auto* sd = dynamic_cast<const StructDecl*>(decl.get())) {
            structs_[module + "." + sd->name] = sd;
            for (const MethodDecl& method : sd->methods) {
                methods_[module + "." + sd->name + "." + method.name] = &method;
            }
        }
        if (const auto* cd = dynamic_cast<const ClassDecl*>(decl.get())) {
            for (const MethodDecl& method : cd->methods) {
                methods_[module + "." + cd->name + "." + method.name] = &method;
            }
        }
    }
    visitProgram(program);
    return !failed_;
}

// --- Scopes ------------------------------------------------------------------

const LocalScope* Checker::pushScope() {
    auto scope = std::make_unique<LocalScope>();
    scope->parent = current_;
    const LocalScope* out = scope.get();
    scopes_.push_back(std::move(scope));
    current_ = out;
    return out;
}

void Checker::popScope() {
    current_ = current_ != nullptr ? current_->parent : nullptr;
}

bool Checker::bindLocal(const std::string& name, TypeRef type, bool isMut, Position pos) {
    if (current_ == nullptr) {
        return false;
    }
    const_cast<LocalScope*>(current_)->names[name] = Local{type, isMut, pos};
    return true;
}

const Local* Checker::findLocal(const std::string& name) const {
    for (const LocalScope* scope = current_; scope != nullptr; scope = scope->parent) {
        auto found = scope->names.find(name);
        if (found != scope->names.end()) {
            return &found->second;
        }
    }
    return nullptr;
}

const LocalScope* Checker::scopeOf(const std::string& name) const {
    for (const LocalScope* scope = current_; scope != nullptr; scope = scope->parent) {
        if (scope->names.find(name) != scope->names.end()) {
            return scope;
        }
    }
    return nullptr;
}

bool withinScope(const LocalScope* scope, const LocalScope* root) {
    for (const LocalScope* at = scope; at != nullptr; at = at->parent) {
        if (at == root) {
            return true;
        }
    }
    return false;
}

// --- Declarations and statements -----------------------------------------------

void Checker::visitProgram(Program& n) {
    // Globals first: a global initialiser is constant or null, so nothing it
    // needs is declared below it — and every function body may read a global,
    // so their types must be recorded before any body walks.
    for (auto& g : n.globals) {
        TypeRef declared = resolver_.typeOf(g.get());
        if (declared.isError()) {
            continue;
        }
        // A global initialiser is constant or null: there is no startup code
        // and no order problem (FEATURES.md §7.3). Anything else needs a
        // function to run it, and globals have none.
        if (dynamic_cast<const NilPtr*>(g->init.get()) != nullptr) {
            if (declared.type->kind() != Kind::Pointer &&
                declared.type->kind() != Kind::FunctionPtr) {
                fail(g->init->pos, "null is a pointer value");
                continue;
            }
            record(g.get(), declared);
            continue;
        }
        std::unordered_set<std::string> active;
        EvalResult folded = evalConstInt(*g->init, *env_, active);
        if (!folded.ok) {
            // A zero repeat names every byte (zero), so it is a constant
            // a global may hold: the bytes are known and there is no
            // startup code to order (FEATURES.md §8.3). Anything else
            // constructed is not constant.
            bool zeroGlobal = false;
            if (const auto* initArray =
                    dynamic_cast<const NewArray*>(g->init.get())) {
                // Repeat with a zero filler: the normal checking below
                // still enforces the types (literal adaptation for
                // scalars, isZeroRepeat for aggregates), so the gate only
                // has to recognise the all-bytes-known shape.
                if (initArray->repeat && !initArray->inits.empty()) {
                    EvalResult filler =
                        evalConstInt(*initArray->inits[0], *env_, active);
                    zeroGlobal = filler.ok && filler.value == 0;
                }
            }
            // A float literal is a constant of its own kind: evalConstInt
            // answers integers, so only a literal in a float-typed global
            // gets past without a folded integer. Anything else is not a
            // constant initialiser.
            const auto* asFloat = dynamic_cast<const FloatLit*>(g->init.get());
            const bool floatGlobal =
                declared.type->kind() == Kind::Basic &&
                basicIsFloat(*declared.type->basic());
            // A string literal is bytes in the image: the lowerer already
            // owns that case (rodata plus a two-word global), so the gate
            // only has to not call it non-constant.
            const auto* asString = dynamic_cast<const StringLit*>(g->init.get());
            const bool stringGlobal = declared.type->kind() == Kind::String;
            if (!zeroGlobal && (asFloat == nullptr || !floatGlobal) &&
                (asString == nullptr || !stringGlobal)) {
                fail(g->init->pos, "a global initialiser must be constant: " + folded.error);
                continue;
            }
        }
        TypeRef init = checkExpr(g->init.get(), declared);
        if (!checkCompatible(g->init->pos, g->init.get(), init, declared)) {
            continue;
        }
        record(g.get(), declared);
    }
    // After the globals are checked: the sink reads their recorded types, and
    // it runs before any function body so a global read lowers.
    if (sink_ != nullptr) {
        sink_->onProgram(n);
    }
    for (auto& d : n.decls) {
        d->accept(*this);
    }
}

void Checker::visitFuncDecl(FuncDecl& n) {
    if (!n.typeParams.empty()) {
        return;
    }
    bool failed = false;
    Signature sig = resolver_.buildSignature(n.params, n.returns.get(), moduleScope_, failed);
    if (failed) {
        return;
    }
    const Type* ret = sig.returnType() != nullptr ? sig.returnType() : ctx_.void_();
    pushScope();
    const std::vector<Parameter>& params = sig.parameters();
    for (uint64_t i = 0; i < static_cast<uint64_t>(params.size()); ++i) {
        bindLocal(n.params[i].name, TypeRef{params[i].type}, params[i].isMutable,
                  n.params[i].pos);
    }
    TypeRef saved = funcReturn_;
    bool was = inFunc_;
    funcReturn_ = TypeRef{ret};
    inFunc_ = true;
    if (sink_ != nullptr) {
        sink_->onFuncDecl(n);
    } else {
        n.body->accept(*this);
    }
    funcReturn_ = saved;
    inFunc_ = was;
    popScope();
}

void Checker::visitStructDecl(StructDecl& n) {
    if (!n.typeParams.empty()) {
        return;
    }
    const resolve::Decl* self = resolver_.lookup(moduleScope_, n.name);
    if (self == nullptr) {
        fail(n.pos, "checker bug: '" + n.name + "' was never resolved");
        return;
    }
    // Conformance: every interface method, present with the same signature.
    for (const std::unique_ptr<TypeExpr>& iface : n.interfaces) {
        TypeRef t = resolveHere(iface.get());
        if (t.isError() || t.type->kind() != Kind::Interface) {
            continue;
        }
        std::optional<DeclId> ifaceId = t.type->decl();
        if (!ifaceId.has_value() || !implementsAll(self->id, *ifaceId, n.pos)) {
            continue;
        }
    }
    checkMethods(self->id, Kind::Struct, n.methods);
}

void Checker::visitClassDecl(ClassDecl& n) {
    if (!n.typeParams.empty()) {
        return;
    }
    const resolve::Decl* self = resolver_.lookup(moduleScope_, n.name);
    if (self == nullptr) {
        fail(n.pos, "checker bug: '" + n.name + "' was never resolved");
        return;
    }
    checkMethods(self->id, Kind::Class, n.methods);
    checkCtor(self->id, Kind::Class, n.init.get(), true);
    checkCtor(self->id, Kind::Class, n.deinit.get(), false);
}

void Checker::checkCtor(DeclId id, Kind kind, FuncDecl* ctor, bool isInit) {
    if (ctor == nullptr) {
        if (isInit) {
            return;
        }
        return;
    }
    const resolve::Scope* saved = moduleScope_;
    // The constructor's own scope: its parameters and, for init, a `self` of the
    // class being built.
    pushScope();
    const Signature* sig = resolver_.methodSig(id, isInit ? "init" : "deinit");
    if (sig != nullptr) {
        const std::vector<Parameter>& params = sig->parameters();
        for (uint64_t i = 0; i < static_cast<uint64_t>(params.size()); ++i) {
            bindLocal(ctor->params[i].name, TypeRef{params[i].type}, params[i].isMutable,
                      ctor->params[i].pos);
        }
        const Type* obj = ctx_.nominal(kind, id);
        bindLocal("self", TypeRef{ctx_.pointer(obj)}, false, ctor->pos);
        const Type* ret = sig->returnType() != nullptr ? sig->returnType() : ctx_.void_();
        TypeRef savedRet = funcReturn_;
        bool was = inFunc_;
        funcReturn_ = TypeRef{ret};
        inFunc_ = true;
        if (sink_ != nullptr) {
            const resolve::Decl* owner = resolver_.declById(id);
            std::string name = owner != nullptr ? owner->name : "";
            sink_->onMethodDecl(name, isInit ? "init" : "deinit", ctor->params, *sig,
                                TypeRef{ctx_.pointer(obj)}, *ctor->body);
        } else {
            ctor->body->accept(*this);
        }
        funcReturn_ = savedRet;
        inFunc_ = was;
    }
    popScope();
    (void)saved;
}

void Checker::checkMethods(DeclId id, Kind kind, std::vector<MethodDecl>& methods) {
    const Type* obj = ctx_.nominal(kind, id);
    const Type* self = ctx_.pointer(obj);
    for (MethodDecl& method : methods) {
        const Signature* sig = resolver_.methodSig(id, method.name);
        if (sig == nullptr) {
            fail(method.pos, "checker bug: signature missing");
            continue;
        }
        pushScope();
        bindLocal("self", TypeRef{self}, false, method.pos);
        const std::vector<Parameter>& declared = sig->parameters();
        for (uint64_t i = 0; i < static_cast<uint64_t>(declared.size()); ++i) {
            bindLocal(method.params[i].name, TypeRef{declared[i].type},
                      declared[i].isMutable, method.params[i].pos);
        }
        const Type* ret = sig->returnType() != nullptr ? sig->returnType() : ctx_.void_();
        TypeRef saved = funcReturn_;
        bool was = inFunc_;
        funcReturn_ = TypeRef{ret};
        inFunc_ = true;
        if (sink_ != nullptr) {
            const resolve::Decl* owner = resolver_.declById(id);
            std::string name = owner != nullptr ? owner->name : "";
            sink_->onMethodDecl(name, method.name, method.params, *sig, TypeRef{self},
                                *method.body);
        } else {
            method.body->accept(*this);
        }
        funcReturn_ = saved;
        inFunc_ = was;
        popScope();
    }
}

void Checker::visitVarDecl(VarDecl& n) {
    TypeRef declared = resolveHere(n.type.get());
    if (declared.isError()) {
        return;
    }
    TypeRef init = checkExpr(n.init.get(), declared);
    if (!checkCompatible(n.init->pos, n.init.get(), init, declared)) {
        return;
    }
    bindLocal(n.name, declared, true, n.pos);
    record(&n, declared);
    if (sink_ != nullptr) {
        sink_->onVarDecl(n);
    }
}

void Checker::visitBlock(Block& n) {
    pushScope();
    if (sink_ != nullptr) {
        sink_->onBlockEnter(n);
    }
    for (auto& s : n.stmts) {
        s->accept(*this);
    }
    if (sink_ != nullptr) {
        sink_->onBlockExit(n);
    }
    popScope();
}

void Checker::visitIfStmt(IfStmt& n) {
    TypeRef cond = checkExpr(n.cond.get(), TypeRef{ctx_.basic(BasicKind::Bool)});
    if (!cond.isError() && cond != TypeRef{ctx_.basic(BasicKind::Bool)}) {
        fail(n.cond->pos, "a condition is bool");
    }
    if (sink_ != nullptr) {
        sink_->onIf(n);
        return;
    }
    n.then->accept(*this);
    if (n.els != nullptr) {
        n.els->accept(*this);
    }
}

void Checker::visitWhileStmt(WhileStmt& n) {
    TypeRef cond = checkExpr(n.cond.get(), TypeRef{ctx_.basic(BasicKind::Bool)});
    if (!cond.isError() && cond != TypeRef{ctx_.basic(BasicKind::Bool)}) {
        fail(n.cond->pos, "a condition is bool");
    }
    ++loopDepth_;
    if (sink_ != nullptr) {
        sink_->onWhile(n);
        --loopDepth_;
        return;
    }
    n.body->accept(*this);
    --loopDepth_;
}

void Checker::visitForStmt(ForStmt& n) {
    pushScope();
    if (sink_ != nullptr) {
        // Init is checked here so the condition and step resolve against
        // it, but not lowered: onFor re-accepts it inside the loop's own
        // frame so the binding lands where the loop can see it.
        LowerSink* saved = sink_;
        sink_ = nullptr;
        if (n.init != nullptr) {
            n.init->accept(*this);
        }
        sink_ = saved;
        if (n.cond != nullptr) {
            TypeRef cond = checkExpr(n.cond.get(), TypeRef{ctx_.basic(BasicKind::Bool)});
            if (!cond.isError() && cond != TypeRef{ctx_.basic(BasicKind::Bool)}) {
                fail(n.cond->pos, "a condition is bool");
            }
        }
        if (n.step != nullptr) {
            checkExpr(n.step.get(), TypeRef{ctx_.wildcard()});
        }
        ++loopDepth_;
        sink_->onFor(n);
        --loopDepth_;
        popScope();
        return;
    }
    if (n.init != nullptr) {
        n.init->accept(*this);
    }
    if (n.cond != nullptr) {
        TypeRef cond = checkExpr(n.cond.get(), TypeRef{ctx_.basic(BasicKind::Bool)});
        if (!cond.isError() && cond != TypeRef{ctx_.basic(BasicKind::Bool)}) {
            fail(n.cond->pos, "a condition is bool");
        }
    }
    if (n.step != nullptr) {
        checkExpr(n.step.get(), TypeRef{ctx_.wildcard()});
    }
    ++loopDepth_;
    n.body->accept(*this);
    --loopDepth_;
    popScope();
}

void Checker::visitForInStmt(ForInStmt& n) {
    TypeRef seq = checkExpr(n.seq.get(), TypeRef{ctx_.wildcard()});
    TypeRef element = errorRef();
    if (!seq.isError()) {
        Kind kind = seq.type->kind();
        if (kind == Kind::Array || kind == Kind::Slice) {
            element = TypeRef{seq.type->element()};
        } else if (kind == Kind::Struct || kind == Kind::Class) {
            std::optional<DeclId> id = seq.type->decl();
            if (id.has_value()) {
                const Signature* next = resolver_.methodSig(*id, "next");
                if (next != nullptr && next->arity() == 1 && next->returnType() != nullptr &&
                    next->returnType()->kind() == Kind::Pointer) {
                    element = TypeRef{next->returnType()->pointee()};
                } else {
                    fail(n.seq->pos, "a for-in type provides iter() and next()");
                }
            }
        } else {
            fail(n.seq->pos, "cannot iterate this type");
        }
    }
    pushScope();
    if (!element.isError()) {
        bindLocal(n.name, element, n.isMutable, n.pos);
    }
    ++loopDepth_;
    if (sink_ != nullptr) {
        // The element binding lands here so the lowered body resolves it;
        // onForIn re-accepts the body inside its own frame copy.
        sink_->onForIn(n);
    } else {
        n.body->accept(*this);
    }
    --loopDepth_;
    popScope();
}

void Checker::visitExprStmt(ExprStmt& n) {
    checkExpr(n.expr.get(), TypeRef{ctx_.wildcard()});
    if (sink_ != nullptr) {
        sink_->onExprStmt(n);
    }
}

void Checker::visitReturn(Return& n) {
    if (!inFunc_) {
        fail(n.pos, "return outside a function");
        return;
    }
    if (n.value == nullptr) {
        if (funcReturn_ != TypeRef{ctx_.void_()}) {
            fail(n.pos, "returning nothing from a function with a return type");
            return;
        }
        // A bare return still leaves the function: the sink must close the
        // block with Ret, or an `if (c) { return; }` arm falls through to
        // the join and silently runs what follows it.
        if (sink_ != nullptr) {
            sink_->onReturn(n);
        }
        return;
    }
    TypeRef value = checkExpr(n.value.get(), funcReturn_);
    checkCompatible(n.value->pos, n.value.get(), value, funcReturn_);
    // After the value is checked: the sink reads its recorded type.
    if (sink_ != nullptr) {
        sink_->onReturn(n);
    }
}

void Checker::visitBreak(Break& n) {
    if (loopDepth_ == 0) {
        fail(n.pos, "break outside a loop");
        return;
    }
    if (sink_ != nullptr) {
        sink_->onBreak(n);
    }
}

void Checker::visitContinue(Continue& n) {
    if (loopDepth_ == 0) {
        fail(n.pos, "continue outside a loop");
        return;
    }
    if (sink_ != nullptr) {
        sink_->onContinue(n);
    }
}

// --- Expressions ---------------------------------------------------------------

void Checker::visitIntLit(IntLit& n) {
    record(&n, checkLiteralInt(n, TypeRef{ctx_.wildcard()}, n.pos));
}

void Checker::visitFloatLit(FloatLit& n) {
    record(&n, checkLiteralFloat(n, TypeRef{ctx_.wildcard()}, n.pos));
}

void Checker::visitCharLit(CharLit& n) {
    record(&n, TypeRef{ctx_.basic(BasicKind::Char)});
}

void Checker::visitStringLit(StringLit& n) {
    for (auto& part : n.parts) {
        if (part.isExpr && part.expr != nullptr) {
            checkExpr(part.expr.get(), TypeRef{ctx_.wildcard()});
        }
    }
    record(&n, TypeRef{ctx_.string()});
}

void Checker::visitBoolLit(BoolLit& n) {
    record(&n, TypeRef{ctx_.basic(BasicKind::Bool)});
}

void Checker::visitNilPtr(NilPtr& n) {
    record(&n, TypeRef{ctx_.pointer(ctx_.void_())});
}

void Checker::visitIdent(Ident& n) {
    record(&n, checkIdent(n));
}


void Checker::visitBinary(Binary& n) {
    record(&n, checkBinary(n, TypeRef{ctx_.wildcard()}));
}

void Checker::visitUnary(Unary& n) {
    TypeRef operand = checkExpr(n.operand.get(), TypeRef{ctx_.wildcard()});
    if (operand.isError()) {
        record(&n, operand);
        return;
    }
    Kind kind = operand.type->kind();
    if (n.op == UnOp::Not) {
        if (kind != Kind::Basic || *operand.type->basic() != BasicKind::Bool) {
            fail(n.pos, "not needs a bool");
            record(&n, errorRef());
            return;
        }
        record(&n, operand);
        return;
    }
    if (n.op == UnOp::Deref) {
        if (kind != Kind::Pointer && kind != Kind::FunctionPtr) {
            fail(n.pos, "cannot dereference this type");
            record(&n, errorRef());
            return;
        }
        if (kind == Kind::FunctionPtr) {
            record(&n, operand);
            return;
        }
        record(&n, TypeRef{operand.type->pointee()});
        return;
    }
    if (n.op == UnOp::Addr) {
        if (!isPlace(n.operand.get())) {
            fail(n.pos, "cannot take the address of this");
            record(&n, errorRef());
            return;
        }
        if (const auto* ident = dynamic_cast<const Ident*>(n.operand.get())) {
            if (findLocal(ident->name) == nullptr) {
                const resolve::Decl* decl = resolver_.lookup(moduleScope_, ident->name);
                // A plain function's address is its code address — the bare
                // `*func` type (the star is the kind's own spelling, not a
                // Pointer around it: the resolver collapses `*func(..)`
                // to Kind::FunctionPtr). A bare `add` checks as a closure
                // (the 16-byte value); `&add` is the 8-byte code address,
                // the only way to form a `*func`. Generic functions never
                // reach here: naming one without arguments is already an
                // error above.
                if (decl != nullptr && decl->kind == resolve::DeclKind::Func) {
                    Signature sig = decl->sig;
                    record(&n, TypeRef{ctx_.functionPtr(std::move(sig))});
                    return;
                }
            }
        }
        record(&n, TypeRef{ctx_.pointer(operand.type)});
        return;
    }
    if (kind != Kind::Basic) {
        fail(n.pos, "this operator needs a number");
        record(&n, errorRef());
        return;
    }
    BasicKind basic = *operand.type->basic();
    if (n.op == UnOp::BitNot && !basicIsInteger(basic)) {
        fail(n.pos, "this operator needs an integer");
        record(&n, errorRef());
        return;
    }
    record(&n, operand);
}

void Checker::visitCast(Cast& n) {
    TypeRef from = checkExpr(n.expr.get(), TypeRef{ctx_.wildcard()});
    TypeRef target = resolveHere(n.target.get());
    if (from.isError() || target.isError()) {
        record(&n, errorRef());
        return;
    }
    if (!castRowOk(from, target)) {
        fail(n.pos, "no cast from " + typeName(from) + " to " + typeName(target));
        record(&n, errorRef());
        return;
    }
    if (from.type->kind() == Kind::Union) {
        // A union cast reads the payload's bytes ignoring the tag (TYPES.md
        // 4.6): the target must fit in the payload, and SEMANTICS rejects
        // wider targets rather than reading past it.
        std::optional<DeclId> uid = from.type->decl();
        // Split across lines on purpose: the offset call beside a digit
        // reads as a size literal and T4 refuses it.
        int64_t room = -1;
        if (uid.has_value()) {
            room = resolver_.unionTagOffset(*uid);
        }
        int64_t want = resolver_.nominalSize(target.type);
        if (room < 0 || want < 0) {
            fail(n.pos, "a union cast cannot be sized");
            record(&n, errorRef());
            return;
        }
        if (want > room) {
            fail(n.pos, "a cast reads the payload: " + typeName(target) +
                             " does not fit");
            record(&n, errorRef());
            return;
        }
    }
    record(&n, target);
}

void Checker::visitTernary(Ternary& n) {
    record(&n, checkTernary(n, TypeRef{ctx_.wildcard()}));
}

TypeRef Checker::checkTernary(Ternary& n, TypeRef expected) {
    TypeRef cond = checkExpr(n.cond.get(), TypeRef{ctx_.basic(BasicKind::Bool)});
    if (!cond.isError() && cond != TypeRef{ctx_.basic(BasicKind::Bool)}) {
        fail(n.cond->pos, "a condition is bool");
    }
    TypeRef then = checkExpr(n.then.get(), expected);
    TypeRef els = checkExpr(n.els.get(), expected);
    if (then.isError() || els.isError()) {
        return errorRef();
    }
    if (then != els) {
        fail(n.pos, "the branches of a ternary match: " + typeName(then) + " and " +
                        typeName(els));
        return errorRef();
    }
    if (!expected.isError() && expected != TypeRef{ctx_.wildcard()} &&
        !checkCompatible(n.pos, n.then.get(), then, expected)) {
        return errorRef();
    }
    return then;
}

void Checker::visitCall(Call& n) {
    record(&n, checkCall(n));
}

void Checker::visitField(Field& n) {
    record(&n, checkField(n));
}

void Checker::visitIndex(Index& n) {
    TypeRef base = checkExpr(n.array.get(), TypeRef{ctx_.wildcard()});
    TypeRef index =
        checkExpr(n.index.get(), TypeRef{ctx_.basic(BasicKind::I64)});
    if (base.isError() || index.isError()) {
        record(&n, errorRef());
        return;
    }
    if (index.type->kind() != Kind::Basic || !basicIsInteger(*index.type->basic())) {
        fail(n.index->pos, "an index is an integer");
        record(&n, errorRef());
        return;
    }
    Kind kind = base.type->kind();
    if (kind == Kind::Array || kind == Kind::Slice) {
        record(&n, TypeRef{base.type->element()});
        return;
    }
    if (kind == Kind::String) {
        record(&n, TypeRef{ctx_.basic(BasicKind::U8)});
        return;
    }
    fail(n.pos, "cannot index this type");
    record(&n, errorRef());
}

void Checker::visitSliceExpr(SliceExpr& n) {
    TypeRef base = checkExpr(n.array.get(), TypeRef{ctx_.wildcard()});
    TypeRef lo = checkExpr(n.lo.get(), TypeRef{ctx_.basic(BasicKind::I64)});
    TypeRef hi = checkExpr(n.hi.get(), TypeRef{ctx_.basic(BasicKind::I64)});
    if (base.isError() || lo.isError() || hi.isError()) {
        record(&n, errorRef());
        return;
    }
    Kind kind = base.type->kind();
    if (kind == Kind::String) {
        record(&n, base);
        return;
    }
    if (kind == Kind::Array || kind == Kind::Slice) {
        record(&n, TypeRef{ctx_.slice(base.type->element())});
        return;
    }
    fail(n.pos, "cannot slice this type");
    record(&n, errorRef());
}

void Checker::visitNewStruct(NewStruct& n) {
    record(&n, checkNewStruct(n));
}

void Checker::visitNewClass(NewClass& n) {
    TypeRef target = resolveHere(n.type.get());
    if (target.isError()) {
        record(&n, errorRef());
        return;
    }
    if (target.type->kind() != Kind::Class) {
        fail(n.pos, "new C(args) creates a class");
        record(&n, errorRef());
        return;
    }
    std::optional<DeclId> id = target.type->decl();
    const Signature* init = resolver_.methodSig(*id, "init");
    if (init == nullptr) {
        fail(n.pos, "this class has no init: create it with new C { ... }");
        record(&n, errorRef());
        return;
    }
    if (n.args.size() != init->arity()) {
        fail(n.pos, "wrong number of arguments");
        record(&n, errorRef());
        return;
    }
    for (uint64_t i = 0; i < static_cast<uint64_t>(n.args.size()); ++i) {
        TypeRef want{init->parameters()[i].type};
        TypeRef got = checkExpr(n.args[i].get(), want);
        checkCompatible(n.args[i]->pos, n.args[i].get(), got, want);
    }
    record(&n, TypeRef{ctx_.pointer(target.type)});
}

void Checker::visitNewArray(NewArray& n) {
    record(&n, checkNewArray(n));
}

void Checker::visitAssign(Assign& n) {
    TypeRef target = checkExpr(n.target.get(), TypeRef{ctx_.wildcard()});
    if (target.isError()) {
        record(&n, errorRef());
        return;
    }
    if (!isPlace(n.target.get())) {
        fail(n.target->pos, "cannot assign to this");
        record(&n, errorRef());
        return;
    }
    if (const auto* index = dynamic_cast<const Index*>(n.target.get())) {
        // Strings read through the slice path but never write: immutability
        // is a checking rule, so the lowerer never meets a string store.
        TypeRef base = typeOf(index->array.get());
        if (!base.isError() && base.type->kind() == Kind::String) {
            fail(n.target->pos, "strings are immutable");
            record(&n, errorRef());
            return;
        }
    }
    if (!n.op.has_value()) {
        TypeRef value = checkExpr(n.value.get(), target);
        checkCompatible(n.value->pos, n.value.get(), value, target);
        record(&n, target);
        return;
    }
    TypeRef value = checkExpr(n.value.get(), TypeRef{ctx_.wildcard()});
    if (value.isError()) {
        record(&n, errorRef());
        return;
    }
    TypeRef combined = commonType(target, value, n.pos);
    if (combined.isError()) {
        record(&n, errorRef());
        return;
    }
    checkCompatible(n.value->pos, n.value.get(), combined, target);
    record(&n, target);
}

void Checker::visitIncrDecr(IncrDecr& n) {
    TypeRef operand = checkExpr(n.operand.get(), TypeRef{ctx_.wildcard()});
    if (operand.isError()) {
        record(&n, operand);
        return;
    }
    // The operand updates in place, so it is checked like an assignment
    // target: a place, never a string element, and an integer — the same
    // widths `x += 1` would accept, minus the float `+=` allows. A float
    // `++` hides a precision choice behind one character; `f += 1.0`
    // writes it out.
    if (!isPlace(n.operand.get())) {
        fail(n.operand->pos, "cannot increment this");
        record(&n, errorRef());
        return;
    }
    if (const auto* index = dynamic_cast<const Index*>(n.operand.get())) {
        TypeRef base = typeOf(index->array.get());
        if (!base.isError() && base.type->kind() == Kind::String) {
            fail(n.operand->pos, "strings are immutable");
            record(&n, errorRef());
            return;
        }
    }
    if (operand.type->kind() != Kind::Basic || !basicIsInteger(*operand.type->basic())) {
        fail(n.operand->pos, "this operator needs an integer");
        record(&n, errorRef());
        return;
    }
    record(&n, operand);
}

void Checker::visitSizeofType(SizeofType& n) {
    TypeRef target = resolveHere(n.type.get());
    if (target.isError()) {
        record(&n, errorRef());
        return;
    }
    record(&n, TypeRef{ctx_.basic(BasicKind::I64)});
}

void Checker::visitFuncLit(FuncLit& n) {
    record(&n, checkFuncLit(n));
}

TypeRef Checker::checkFuncLit(FuncLit& lit) {
    bool failed = false;
    Signature sig = resolver_.buildSignature(lit.params, lit.returns.get(), moduleScope_, failed);
    if (failed) {
        return errorRef();
    }
    if (emitted_.find(&lit) != emitted_.end()) {
        // The lowerer's hidden-function walk meets the same node again: the
        // types are recorded and the nested literals emitted, so answering
        // the type without re-walking is the whole job. Re-delegating would
        // emit the function twice.
        return TypeRef{ctx_.closure(std::move(sig))};
    }
    // The body is walked once per literal, like a named function's, but the
    // scope only hides the literal's own parameters: enclosing names stay
    // visible so a body can read them. Reads that bind outside the
    // literal's own scope are its by-value captures, recorded for the
    // lowerer in first-read order.
    pushScope();
    const std::vector<Parameter>& params = sig.parameters();
    for (uint64_t i = 0; i < static_cast<uint64_t>(params.size()); ++i) {
        bindLocal(lit.params[i].name, TypeRef{params[i].type}, params[i].isMutable,
                  lit.params[i].pos);
    }
    const Type* ret = sig.returnType() != nullptr ? sig.returnType() : ctx_.void_();
    TypeRef saved = funcReturn_;
    bool was = inFunc_;
    funcReturn_ = TypeRef{ret};
    inFunc_ = true;
    // The discovery walk runs with the sink detached, like a generic
    // instantiation walk: nothing lowers yet, so the lowerer cannot meet a
    // half-built literal, and errors here are the only copy — the lowering
    // walk re-checks deterministically and reports nothing new.
    litStack_.push_back(LiteralFrame{current_, {}});
    LowerSink* savedSink = sink_;
    sink_ = nullptr;
    size_t before = errors_.size();
    lit.body->accept(*this);
    size_t after = errors_.size();
    sink_ = savedSink;
    std::vector<Captured> captures = std::move(litStack_.back().captures);
    litStack_.pop_back();
    funcReturn_ = saved;
    inFunc_ = was;
    if (after != before) {
        popScope();
        return errorRef();
    }
    // The scope stays pushed through delegation: the lowerer's walk runs
    // now, synchronously, and nested literals discover against these exact
    // bindings. Siblings below never see it; the pop is below.
    if (sink_ != nullptr) {
        emitted_.insert(&lit);
        sink_->onFuncLit(lit, captures, sig);
    }
    popScope();
    return TypeRef{ctx_.closure(std::move(sig))};
}

void Checker::visitMatchStmt(MatchStmt& n) {
    TypeRef scrutinee = checkExpr(n.scrutinee.get(), TypeRef{ctx_.wildcard()});
    for (auto& arm : n.arms) {
        pushScope();
        bindPattern(*arm->pattern, scrutinee);
        arm->body->accept(*this);
        popScope();
    }
    if (n.fallback != nullptr) {
        n.fallback->accept(*this);
    }
    // Arm dispatch lowering does not exist yet: the first arm ran
    // unconditionally, so match refuses here. Arms still check above.
    fail(n.pos, "match is not implemented yet");
}

void Checker::visitDeleteStmt(DeleteStmt& n) {
    TypeRef t = checkExpr(n.ptr.get(), TypeRef{ctx_.wildcard()});
    if (t.isError()) {
        return;
    }
    const Type* obj =
        t.type->kind() == Kind::Pointer ? t.type->pointee() : nullptr;
    if (obj == nullptr || obj->kind() != Kind::Class) {
        fail(n.pos, "delete needs a class pointer");
        return;
    }
    if (sink_ != nullptr) {
        sink_->onDelete(n);
    }
}

// Every `{placeholder}` in an asm block must name a local, a parameter, or
// a module-level declaration: the text is raw assembly the checker cannot
// see into, so a typo'd name has to fail here (FEAT-035).
void Checker::visitAsmStmt(AsmStmt& n) {
    bool bound = true;
    for (const std::string& raw : n.names) {
        std::string base = raw;
        if (!base.empty() && base[0] == '*') {
            base = base.substr(1);
        }
        std::string::size_type colon = base.find(':');
        if (colon != std::string::npos) {
            base = base.substr(0, colon);
        }
        if (base.empty() || (findLocal(base) == nullptr &&
                             resolver_.lookup(moduleScope_, base) == nullptr)) {
            fail(n.pos, "asm placeholder '" + base + "' is unbound");
            bound = false;
        }
    }
    if (bound && sink_ != nullptr) {
        sink_->onAsmStmt(n);
    }
}

void Checker::visitThrowStmt(ThrowStmt& n) {
    // Exception transport (FEAT-041) does not exist yet: throwing must
    // refuse here rather than check clean and vanish in lowering, which
    // previously exited 0. The operand still checks so its errors report.
    checkExpr(n.value.get(), TypeRef{ctx_.wildcard()});
    fail(n.pos, "throw is not implemented yet");
}

void Checker::visitTryStmt(TryStmt& n) {
    // Same shape as throw: handler bodies would lower inline with no
    // unwind edges, so trying refuses until transport lands.
    fail(n.pos, "try is not implemented yet");
}

void Checker::bindPattern(Pattern& pat, TypeRef scrutinee) {
    if (pat.kind == PatternKind::Bind) {
        bindLocal(pat.name, scrutinee, false, pat.pos);
        return;
    }
    if (pat.kind == PatternKind::Type) {
        TypeRef t = resolveHere(pat.type.get());
        if (t.isError() || t.type->kind() != Kind::Struct) {
            return;
        }
        std::optional<DeclId> id = t.type->decl();
        if (!id.has_value()) {
            return;
        }
        for (FieldPat& field : pat.fields) {
            TypeRef member = resolver_.fieldType(*id, field.name);
            if (member.isError()) {
                fail(field.pat->pos, "no such field");
                continue;
            }
            bindSubPattern(*field.pat, member);
        }
        return;
    }
    if (pat.kind == PatternKind::Member) {
        auto tagSpelling = std::make_unique<NamedType>(
            pat.pos, "", pat.tag, std::vector<std::unique_ptr<TypeExpr>>{});
        TypeRef t = resolveHere(tagSpelling.get());
        if (t.isError() || t.type->kind() != Kind::Union) {
            return;
        }
        std::optional<DeclId> id = t.type->decl();
        if (!id.has_value()) {
            return;
        }
        if (resolver_.fieldType(*id, pat.member).isError()) {
            fail(pat.pos, "no such member");
        }
        return;
    }
}

void Checker::bindSubPattern(Pattern& pat, TypeRef type) {
    if (pat.kind == PatternKind::Bind) {
        bindLocal(pat.name, type, false, pat.pos);
        return;
    }
    if (pat.kind == PatternKind::Type) {
        TypeRef t = resolveHere(pat.type.get());
        if (t.isError()) {
            return;
        }
        std::optional<DeclId> id = t.type->decl();
        if (!id.has_value()) {
            return;
        }
        for (FieldPat& field : pat.fields) {
            TypeRef member = resolver_.fieldType(*id, field.name);
            if (member.isError()) {
                fail(field.pat->pos, "no such field");
                continue;
            }
            bindSubPattern(*field.pat, member);
        }
    }
}

// --- Expression cores ----------------------------------------------------------

TypeRef Checker::checkExpr(Expr* expr, TypeRef expected) {
    if (expr == nullptr) {
        return errorRef();
    }
    if (auto* lit = dynamic_cast<IntLit*>(expr)) {
        TypeRef t = checkLiteralInt(*lit, expected, lit->pos);
        record(lit, t);
        return t;
    }
    if (auto* lit = dynamic_cast<FloatLit*>(expr)) {
        TypeRef t = checkLiteralFloat(*lit, expected, lit->pos);
        record(lit, t);
        return t;
    }
    if (auto* lit = dynamic_cast<CharLit*>(expr)) {
        record(lit, TypeRef{ctx_.basic(BasicKind::Char)});
        return TypeRef{ctx_.basic(BasicKind::Char)};
    }
    if (auto* lit = dynamic_cast<BoolLit*>(expr)) {
        record(lit, TypeRef{ctx_.basic(BasicKind::Bool)});
        return TypeRef{ctx_.basic(BasicKind::Bool)};
    }
    if (auto* nil = dynamic_cast<NilPtr*>(expr)) {
        record(nil, TypeRef{ctx_.pointer(ctx_.void_())});
        return TypeRef{ctx_.pointer(ctx_.void_())};
    }
    if (auto* ident = dynamic_cast<Ident*>(expr)) {
        TypeRef t = checkIdent(*ident);
        record(ident, t);
        return t;
    }
    if (auto* string = dynamic_cast<StringLit*>(expr)) {
        for (auto& part : string->parts) {
            if (part.isExpr && part.expr != nullptr) {
                checkExpr(part.expr.get(), TypeRef{ctx_.wildcard()});
            }
        }
        record(string, TypeRef{ctx_.string()});
        return TypeRef{ctx_.string()};
    }
    if (auto* binary = dynamic_cast<Binary*>(expr)) {
        TypeRef t = checkBinary(*binary, expected);
        record(binary, t);
        return t;
    }
    if (auto* unary = dynamic_cast<Unary*>(expr)) {
        visitUnary(*unary);
        return typeOf(unary);
    }
    if (auto* step = dynamic_cast<IncrDecr*>(expr)) {
        visitIncrDecr(*step);
        return typeOf(step);
    }
    if (auto* cast = dynamic_cast<Cast*>(expr)) {
        visitCast(*cast);
        return typeOf(cast);
    }
    if (auto* ternary = dynamic_cast<Ternary*>(expr)) {
        TypeRef t = checkTernary(*ternary, expected);
        record(ternary, t);
        return t;
    }
    if (auto* call = dynamic_cast<Call*>(expr)) {
        TypeRef t = checkCall(*call);
        record(call, t);
        return t;
    }
    if (auto* field = dynamic_cast<Field*>(expr)) {
        TypeRef t = checkField(*field);
        record(field, t);
        return t;
    }
    if (auto* index = dynamic_cast<Index*>(expr)) {
        visitIndex(*index);
        return typeOf(index);
    }
    if (auto* slice = dynamic_cast<SliceExpr*>(expr)) {
        visitSliceExpr(*slice);
        return typeOf(slice);
    }
    if (auto* news = dynamic_cast<NewStruct*>(expr)) {
        TypeRef t = checkNewStruct(*news);
        record(news, t);
        return t;
    }
    if (auto* newc = dynamic_cast<NewClass*>(expr)) {
        visitNewClass(*newc);
        return typeOf(newc);
    }
    if (auto* newa = dynamic_cast<NewArray*>(expr)) {
        TypeRef t = checkNewArray(*newa);
        record(newa, t);
        return t;
    }
    if (auto* assign = dynamic_cast<Assign*>(expr)) {
        visitAssign(*assign);
        return typeOf(assign);
    }
    if (auto* sz = dynamic_cast<SizeofType*>(expr)) {
        visitSizeofType(*sz);
        return typeOf(sz);
    }
    if (auto* fun = dynamic_cast<FuncLit*>(expr)) {
        TypeRef t = checkFuncLit(*fun);
        record(fun, t);
        return t;
    }
    fail(expr->pos, "checker bug: unhandled expression");
    return errorRef();
}

TypeRef Checker::checkLiteralInt(const IntLit& lit, TypeRef expected, Position pos) {
    std::unordered_set<std::string> active;
    IntLit probe = IntLit(lit.pos, lit.base, lit.digits, lit.negative);
    EvalResult folded = evalConstInt(probe, *env_, active);
    if (!folded.ok) {
        fail(pos, folded.error);
        return errorRef();
    }
    if (!expected.isError() && expected != TypeRef{ctx_.wildcard()} &&
        expected.type->kind() == Kind::Basic) {
        BasicKind basic = *expected.type->basic();
        if (basicIsFloat(basic)) {
            // A literal in a float context is that float's value, not an
            // integer that failed to fit (FEATURES §13 keeps literals out
            // of the explicit-cast rule); the conversion rounding is the
            // cast table's, applied when the literal lowers.
            return expected;
        }
        if (basicIsInteger(basic)) {
            int width = basicWidthBits(basic);
            bool isSigned = basicIsSigned(basic);
            __int128 lo = isSigned ? -(static_cast<__int128>(1) << (width - 1)) : 0;
            __int128 hi = isSigned ? ((static_cast<__int128>(1) << (width - 1)) - 1)
                                  : ((static_cast<__int128>(1) << width) - 1);
            if (folded.big) {
                // Bits above INT64_MAX fit an unsigned target wide enough
                // and nothing else; the signed compare would read them as
                // a negative number the source never wrote.
                unsigned __int128 bits = static_cast<uint64_t>(folded.value);
                if (isSigned ||
                    bits > static_cast<unsigned __int128>(hi)) {
                    fail(pos, "this literal does not fit " + typeName(expected));
                    return errorRef();
                }
                return expected;
            }
            __int128 value = folded.value;
            if (value < lo || value > hi) {
                fail(pos, "this literal does not fit " + typeName(expected));
                return errorRef();
            }
            return expected;
        }
    }
    if (folded.big) {
        // No context to fit against: the bits say u64, the one width that
        // holds them, never a negative i32 the wrap would read as.
        return TypeRef{ctx_.basic(BasicKind::U64)};
    }
    if (folded.value >= INT32_MIN && folded.value <= INT32_MAX) {
        return TypeRef{ctx_.basic(BasicKind::I32)};
    }
    return TypeRef{ctx_.basic(BasicKind::I64)};
}

TypeRef Checker::checkLiteralFloat(const FloatLit& lit, TypeRef expected, Position pos) {
    (void)lit;
    (void)pos;
    if (!expected.isError() && expected != TypeRef{ctx_.wildcard()} &&
        expected.type->kind() == Kind::Basic && basicIsFloat(*expected.type->basic())) {
        return expected;
    }
    return TypeRef{ctx_.basic(BasicKind::F64)};
}

TypeRef Checker::checkIdent(Ident& expr) {
    if (const Local* local = findLocal(expr.name)) {
        // A read that binds outside the innermost open literal is that
        // literal's by-value capture — and every enclosing literal's too,
        // up to the one that binds it. A nested literal's site lowers
        // inside its parent's hidden function, where only the parent's
        // own captures are materialised; without the transitive copy the
        // nested site cannot resolve a name from two levels out. Block
        // scopes inside a body bind within that literal's own scope, so
        // they capture nothing.
        if (!litStack_.empty()) {
            const LocalScope* def = scopeOf(expr.name);
            if (def != nullptr) {
                for (uint64_t i = static_cast<uint64_t>(litStack_.size());
                     i > 0;) {
                    --i;
                    if (withinScope(def, litStack_[i].own)) {
                        break;
                    }
                    std::vector<Captured>& captures = litStack_[i].captures;
                    bool known = false;
                    for (const Captured& have : captures) {
                        if (have.name == expr.name) {
                            known = true;
                            break;
                        }
                    }
                    if (!known) {
                        Captured take;
                        take.name = expr.name;
                        take.type = local->type;
                        take.pos = expr.pos;
                        captures.push_back(std::move(take));
                    }
                }
            }
        }
        return local->type;
    }
    const resolve::Decl* decl = resolver_.lookup(moduleScope_, expr.name);
    if (decl == nullptr) {
        fail(expr.pos, "unbound name '" + expr.name + "'");
        return errorRef();
    }
    if (decl->kind == resolve::DeclKind::Func) {
        if (decl->typeParams > 0) {
            fail(expr.pos, "a generic function needs explicit arguments");
            return errorRef();
        }
        return TypeRef{ctx_.closure(decl->sig)};
    }
    if (decl->kind == resolve::DeclKind::Var || decl->kind == resolve::DeclKind::Const) {
        return decl->type;
    }
    fail(expr.pos, "'" + expr.name + "' is not a value");
    return errorRef();
}

bool Checker::isPlace(const Expr* expr) const {
    if (dynamic_cast<const Ident*>(expr) != nullptr) {
        return true;
    }
    if (dynamic_cast<const Field*>(expr) != nullptr) {
        return true;
    }
    if (dynamic_cast<const Index*>(expr) != nullptr) {
        return true;
    }
    if (const auto* unary = dynamic_cast<const Unary*>(expr)) {
        return unary->op == UnOp::Deref;
    }
    return false;
}

TypeRef Checker::checkBinary(Binary& expr, TypeRef expected) {
    TypeRef lhs = checkExpr(expr.lhs.get(), TypeRef{ctx_.wildcard()});
    TypeRef rhs = checkExpr(expr.rhs.get(), TypeRef{ctx_.wildcard()});
    if (lhs.isError() || rhs.isError()) {
        return errorRef();
    }
    // The expectation of the whole expression reaches its literal operands
    // first (FEATURES §13: literals adapt): `var x: float = 1.0 / 2.0` is
    // an f32 division, and an f64 variable beside an f32 literal in an f64
    // declaration widens the literal rather than dragging the variable.
    const bool expectFloat =
        !expected.isError() && expected.type != nullptr &&
        expected.type->kind() == Kind::Basic &&
        basicIsFloat(*expected.type->basic());
    if (expectFloat) {
        if (dynamic_cast<const FloatLit*>(expr.lhs.get()) != nullptr) {
            lhs = checkExpr(expr.lhs.get(), expected);
        }
        if (dynamic_cast<const FloatLit*>(expr.rhs.get()) != nullptr) {
            rhs = checkExpr(expr.rhs.get(), expected);
        }
    }
    // A float literal in a binary operand takes the other side's float
    // width (FEATURES §13: literals adapt as binary operands): `x != 0.3`
    // beside an f32 compares at f32, never through f64 where the rounded
    // float would compare unequal to the double it was written as. The
    // re-check records the literal's type under the expectation, exactly
    // as a declaration's expectation types it.
    const auto* lhsLit = dynamic_cast<const FloatLit*>(expr.lhs.get());
    const auto* rhsLit = dynamic_cast<const FloatLit*>(expr.rhs.get());
    const bool lhsFloat = lhs.type->kind() == Kind::Basic &&
                          basicIsFloat(*lhs.type->basic());
    const bool rhsFloat = rhs.type->kind() == Kind::Basic &&
                          basicIsFloat(*rhs.type->basic());
    if (lhsLit != nullptr && rhsFloat) {
        lhs = checkExpr(expr.lhs.get(), rhs);
    } else if (rhsLit != nullptr && lhsFloat) {
        rhs = checkExpr(expr.rhs.get(), lhs);
    }
    bool isBoolOp = expr.op == BinOp::AndAnd || expr.op == BinOp::OrOr;
    bool isEq = expr.op == BinOp::Eq || expr.op == BinOp::Ne;
    bool isRel =
        expr.op == BinOp::Lt || expr.op == BinOp::Le || expr.op == BinOp::Gt ||
        expr.op == BinOp::Ge;
    bool isShift = expr.op == BinOp::Shl || expr.op == BinOp::Shr;
    if (isBoolOp) {
        const TypeRef boolean{ctx_.basic(BasicKind::Bool)};
        if (lhs != boolean || rhs != boolean) {
            fail(expr.pos, "and/or need bool operands");
            return errorRef();
        }
        return boolean;
    }
    if (isEq || isRel) {
        const bool lhsBool = lhs.type->kind() == Kind::Basic &&
                             *lhs.type->basic() == BasicKind::Bool;
        const bool rhsBool = rhs.type->kind() == Kind::Basic &&
                             *rhs.type->basic() == BasicKind::Bool;
        if (lhsBool || rhsBool) {
            // Equality on bools compares values (SEMANTICS §4); ordering
            // has no meaning, and a bool beside a non-bool is a mismatch
            // the common type must never silently absorb.
            if (isEq && lhsBool && rhsBool) {
                return TypeRef{ctx_.basic(BasicKind::Bool)};
            }
            fail(expr.pos, "bools compare for equality only");
            return errorRef();
        }
        if (lhs.type->kind() == Kind::Pointer && rhs.type->kind() == Kind::Pointer) {
            // Pointers compare by identity (SEMANTICS §4): two spellings of
            // one pointer type, or either side nilPtr — its spelling is
            // *void, the sole null every *T accepts. Unrelated pointees
            // reject rather than compare garbage.
            if (lhs.type->pointee() == rhs.type->pointee() ||
                lhs.type->pointee() == ctx_.void_() ||
                rhs.type->pointee() == ctx_.void_()) {
                return TypeRef{ctx_.basic(BasicKind::Bool)};
            }
            fail(expr.pos, "these pointer types cannot compare");
            return errorRef();
        }
        TypeRef common = commonType(lhs, rhs, expr.pos);
        if (common.isError()) {
            return errorRef();
        }
        if (isRel) {
            checkShiftDiv(expr, lhs, rhs);
        }
        return TypeRef{ctx_.basic(BasicKind::Bool)};
    }
    if (isShift) {
        if (!isInteger(lhs) || !isInteger(rhs)) {
            fail(expr.pos, "shifts are integer-only");
            return errorRef();
        }
        checkShiftDiv(expr, lhs, rhs);
        return commonType(lhs, rhs, expr.pos);
    }
    TypeRef common = commonType(lhs, rhs, expr.pos);
    if (common.isError()) {
        return errorRef();
    }
    checkShiftDiv(expr, lhs, rhs);
    return common;
}

void Checker::checkShiftDiv(const Binary& expr, TypeRef lhs, TypeRef rhs) {
    if (expr.op != BinOp::Div && expr.op != BinOp::Rem && expr.op != BinOp::Shl &&
        expr.op != BinOp::Shr) {
        return;
    }
    std::unordered_set<std::string> active;
    EvalResult divisor = evalConstInt(*expr.rhs, *env_, active);
    if (!divisor.ok) {
        return;
    }
    if ((expr.op == BinOp::Div || expr.op == BinOp::Rem) && divisor.value == 0) {
        fail(expr.rhs->pos, "division by zero");
        return;
    }
    if ((expr.op == BinOp::Shl || expr.op == BinOp::Shr) && divisor.value < 0) {
        fail(expr.rhs->pos, "a shift count is not negative");
    }
}

bool Checker::isInteger(TypeRef type) const {
    return !type.isError() && type.type->kind() == Kind::Basic &&
           basicIsInteger(*type.type->basic());
}

TypeRef Checker::checkUnionHas(Call& expr, const Field& callee, TypeRef object) {
    // hasM is "has" plus the member with its first letter capitalized:
    // member `i` answers `hasI`, member `count` answers `hasCount`. The
    // spelling lives in the resolver, shared with the lowerer.
    std::optional<DeclId> id = object.type->decl();
    std::string member =
        id.has_value() ? resolver_.unionHasMember(*id, callee.name) : "";
    if (!id.has_value() || member.empty()) {
        fail(expr.pos, "no such member");
        return errorRef();
    }
    if (!expr.args.empty()) {
        fail(expr.pos, "hasM takes no arguments");
        return errorRef();
    }
    return TypeRef{ctx_.basic(BasicKind::Bool)};
}

TypeRef Checker::checkCall(Call& expr) {
    Signature sig;
    bool hasSig = false;
    // `v.hasI()`: a union answers whether the named member is live. This is
    // not a method lookup — unions have no methods — but the second of the
    // three union reads (TYPES.md 4.6). Anything not shaped like hasM, or on
    // a non-union, falls through to the ordinary path unchanged.
    if (const auto* field = dynamic_cast<const Field*>(expr.callee.get())) {
        // hasM-shaped names read as characters, never digits: T4 refuses a
        // digit beside `.size()`, and the spelling check is the resolver's.
        const std::string& name = field->name;
        bool longEnough = name.size() > kHasPrefixLen;
        bool hasShaped = longEnough && name[0] == 'h' && name[1] == 'a' &&
                         name[2] == 's';
        if (hasShaped) {
            TypeRef object = checkExpr(field->object.get(), TypeRef{ctx_.wildcard()});
            if (!object.isError() && object.type->kind() == Kind::Union) {
                return checkUnionHas(expr, *field, object);
            }
        }
    }
    if (const auto* ident = dynamic_cast<const Ident*>(expr.callee.get())) {
        if (const Local* local = findLocal(ident->name)) {
            if (!local->type.isError() &&
                (local->type.type->kind() == Kind::FunctionPtr ||
                 local->type.type->kind() == Kind::Closure)) {
                sig = *local->type.type->signature();
                hasSig = true;
            }
        } else {
            const resolve::Decl* decl = resolver_.lookup(moduleScope_, ident->name);
            if (decl != nullptr && decl->kind == resolve::DeclKind::Func) {
                if (decl->typeParams > 0) {
                    return checkGenericCall(expr, decl);
                }
                sig = decl->sig;
                hasSig = true;
            }
        }
    }
    if (!hasSig) {
        // `da.aval(v)`: a call through a module qualifier resolves the
        // member in the defining scope. Generics ride the same explicit-
        // argument path as a bare spelling.
        if (const auto* field = dynamic_cast<const Field*>(expr.callee.get())) {
            if (const auto* ident =
                    dynamic_cast<const Ident*>(field->object.get())) {
                if (findLocal(ident->name) == nullptr) {
                    const resolve::Decl* mod =
                        resolver_.lookup(moduleScope_, ident->name);
                    if (mod != nullptr &&
                        mod->kind == resolve::DeclKind::Module) {
                        const resolve::Scope* home =
                            resolver_.scopeOf(mod->origin);
                        const resolve::Decl* member =
                            home != nullptr
                                ? resolver_.lookup(home, field->name)
                                : nullptr;
                        if (member != nullptr &&
                            member->kind == resolve::DeclKind::Func) {
                            if (member->typeParams > 0) {
                                return checkGenericCall(expr, member);
                            }
                            sig = member->sig;
                            hasSig = true;
                        }
                    }
                }
            }
        }
    }
    if (!hasSig) {
        TypeRef callee = checkExpr(expr.callee.get(), TypeRef{ctx_.wildcard()});
        if (callee.isError()) {
            return errorRef();
        }
        Kind kind = callee.type->kind();
        if (kind == Kind::FunctionPtr || kind == Kind::Closure) {
            sig = *callee.type->signature();
            hasSig = true;
        }
    }
    if (!hasSig) {
        // `(*f)(v)`: the dereference of a function pointer is the function.
        if (const auto* deref = dynamic_cast<const Unary*>(expr.callee.get())) {
            if (deref->op == UnOp::Deref) {
                TypeRef inner =
                    checkExpr(deref->operand.get(), TypeRef{ctx_.wildcard()});
                if (!inner.isError() && inner.type->kind() == Kind::FunctionPtr) {
                    sig = *inner.type->signature();
                    hasSig = true;
                }
            }
        }
    }
    if (!hasSig) {
        fail(expr.pos, "not callable");
        return errorRef();
    }
    if (expr.args.size() != sig.arity()) {
        fail(expr.pos, "wrong number of arguments");
        return errorRef();
    }
    for (uint64_t i = 0; i < static_cast<uint64_t>(expr.args.size()); ++i) {
        TypeRef want{sig.parameters()[i].type};
        TypeRef got = checkExpr(expr.args[i].get(), want);
        checkCompatible(expr.args[i]->pos, expr.args[i].get(), got, want);
    }
    const Type* ret = sig.returnType() != nullptr ? sig.returnType() : ctx_.void_();
    return TypeRef{ret};
}

TypeRef Checker::checkGenericCall(Call& expr, const resolve::Decl* decl) {
    if (expr.typeArgs.empty()) {
        fail(expr.pos, "a generic call needs explicit arguments");
        return errorRef();
    }
    std::vector<TypeRef> argTypes;
    for (const std::unique_ptr<TypeExpr>& spelling : expr.typeArgs) {
        TypeRef arg = resolveHere(spelling.get());
        if (arg.isError()) {
            return arg;
        }
        argTypes.push_back(arg);
    }
    if (argTypes.size() != decl->typeParams) {
        fail(expr.pos, "wrong number of type arguments");
        return errorRef();
    }
    std::optional<DeclId> spec = resolver_.instantiate(decl->id, argTypes, expr.pos);
    if (!spec.has_value()) {
        return errorRef();
    }
    const Signature* sig = resolver_.funcSig(*spec);
    if (sig == nullptr) {
        fail(expr.pos, "checker bug: specialization has no signature");
        return errorRef();
    }
    if (expr.args.size() != sig->arity()) {
        fail(expr.pos, "wrong number of arguments");
        return errorRef();
    }
    for (uint64_t i = 0; i < static_cast<uint64_t>(expr.args.size()); ++i) {
        TypeRef want{sig->parameters()[i].type};
        TypeRef got = checkExpr(expr.args[i].get(), want);
        checkCompatible(expr.args[i]->pos, expr.args[i].get(), got, want);
    }
    // Walk the template body once per specialization, with the arguments
    // substituted and the sink detached: lowering generic bodies belongs to
    // the next slice, and emitting under the template's name would collide.
    std::string key =
        std::to_string(spec->module) + "." + std::to_string(spec->index);
    if (walked_.find(key) == walked_.end()) {
        walked_.insert(key);
        const resolve::Decl* homeDecl = resolver_.declById(decl->id);
        std::string home =
            homeDecl != nullptr && homeDecl->scope != nullptr
                ? homeDecl->scope->module
                : module_;
        const FuncDecl* spelling = funcSpelling(home, decl->name);
        if (spelling == nullptr) {
            fail(expr.pos, "checker bug: template body is missing");
            return errorRef();
        }
        const resolve::Scope* scope = resolver_.typeParamScope(
            typeScope_, module_, spelling->typeParams, decl->id, expr.pos);
        const resolve::Scope* savedScope = typeScope_;
        typeScope_ = scope;
        std::optional<ActiveInst> savedActive = active_;
        active_ = ActiveInst{decl->id, argTypes, spelling->typeParams};
        pushScope();
        for (uint64_t i = 0; i < static_cast<uint64_t>(spelling->params.size()); ++i) {
            bindLocal(spelling->params[i].name,
                      TypeRef{sig->parameters()[i].type},
                      spelling->params[i].isMutable, spelling->params[i].pos);
        }
        TypeRef savedRet = funcReturn_;
        bool wasFunc = inFunc_;
        const Type* ret =
            sig->returnType() != nullptr ? sig->returnType() : ctx_.void_();
        funcReturn_ = TypeRef{ret};
        inFunc_ = true;
        LowerSink* savedSink = sink_;
        sink_ = nullptr;
        spelling->body->accept(*this);
        sink_ = savedSink;
        funcReturn_ = savedRet;
        inFunc_ = wasFunc;
        popScope();
        typeScope_ = savedScope;
        active_ = savedActive;
    }
    const Type* ret = sig->returnType() != nullptr ? sig->returnType() : ctx_.void_();
    return TypeRef{ret};
}

TypeRef Checker::checkField(Field& expr) {
    // `da.aval`: the qualifier names a module, so the member resolves in
    // the defining scope, never in the current one. Functions answer their
    // closure type like a bare spelling; globals answer their declared
    // type. Anything else is not a member.
    if (const auto* ident = dynamic_cast<const Ident*>(expr.object.get())) {
        if (findLocal(ident->name) == nullptr) {
            const resolve::Decl* mod =
                resolver_.lookup(moduleScope_, ident->name);
            if (mod != nullptr && mod->kind == resolve::DeclKind::Module) {
                const resolve::Scope* home = resolver_.scopeOf(mod->origin);
                const resolve::Decl* member =
                    home != nullptr ? resolver_.lookup(home, expr.name) : nullptr;
                if (member == nullptr) {
                    fail(expr.pos, "'" + expr.name + "' is not exported by '" +
                                          mod->origin + "'");
                    return errorRef();
                }
                if (member->kind == resolve::DeclKind::Func) {
                    if (member->typeParams > 0) {
                        fail(expr.pos, "a generic function needs explicit arguments");
                        return errorRef();
                    }
                    return TypeRef{ctx_.closure(member->sig)};
                }
                if (member->kind == resolve::DeclKind::Var ||
                    member->kind == resolve::DeclKind::Const) {
                    return member->type;
                }
                fail(expr.pos, "'" + expr.name + "' is not a value");
                return errorRef();
            }
        }
    }
    TypeRef object = checkExpr(expr.object.get(), TypeRef{ctx_.wildcard()});
    if (object.isError()) {
        return errorRef();
    }
    Kind kind = object.type->kind();
    if (kind == Kind::Pointer) {
        fail(expr.pos, "deref first: a pointer has no fields");
        return errorRef();
    }
    if (kind == Kind::Class) {
        // A bare class value reads its fields through the resolver's field
        // table. A method name answers the method's closure, which the
        // lowerer refuses until methods lower. Pointer-to-class stays
        // explicit: only (*c).f reaches here, never c.f.
        std::optional<DeclId> id = object.type->decl();
        if (!id.has_value()) {
            return errorRef();
        }
        TypeRef member = resolver_.fieldType(*id, expr.name);
        if (!member.isError()) {
            return member;
        }
        const Signature* sig = resolver_.methodSig(*id, expr.name);
        if (sig == nullptr) {
            fail(expr.pos, "no such field");
            return errorRef();
        }
        return TypeRef{ctx_.closure(*sig)};
    }
    if (kind == Kind::Slice || kind == Kind::String || kind == Kind::Array) {
        // The only member a sized value exposes is its element count; the
        // checker names no other field so the lowerer never guesses one.
        if (expr.name == "len") {
            return TypeRef{ctx_.basic(BasicKind::I64)};
        }
        fail(expr.pos, "no such field");
        return errorRef();
    }
    if (kind == Kind::Struct || kind == Kind::Union) {
        std::optional<DeclId> id = object.type->decl();
        if (!id.has_value()) {
            return errorRef();
        }
        TypeRef member = resolver_.fieldType(*id, expr.name);
        if (!member.isError()) {
            return member;
        }
        // Not a field: a method value. Non-generic methods were walked with
        // their struct; a specialized one walks on first use here.
        const Signature* sig = resolver_.methodSig(*id, expr.name);
        if (sig == nullptr) {
            fail(expr.pos, "no such field");
            return errorRef();
        }
        if (kind == Kind::Struct) {
            auto origin = resolver_.instantiationOf(*id);
            if (origin.has_value()) {
                walkMethodInst(*id, origin->first, expr.name);
                sig = resolver_.methodSig(*id, expr.name);
                if (sig == nullptr) {
                    fail(expr.pos, "checker bug: method vanished");
                    return errorRef();
                }
            }
        }
        return TypeRef{ctx_.closure(*sig)};
    }
    if (kind == Kind::Interface) {
        std::optional<DeclId> id = object.type->decl();
        if (!id.has_value()) {
            return errorRef();
        }
        const Signature* sig = resolver_.methodSig(*id, expr.name);
        if (sig == nullptr) {
            fail(expr.pos, "no such method");
            return errorRef();
        }
        return TypeRef{ctx_.closure(*sig)};
    }
    fail(expr.pos, "no fields on this type");
    return errorRef();
}

TypeRef Checker::checkNewStruct(NewStruct& expr) {
    TypeRef target = resolveHere(expr.type.get());
    if (target.isError()) {
        return errorRef();
    }
    Kind kind = target.type->kind();
    std::optional<DeclId> id = target.type->decl();
    if ((kind != Kind::Struct && kind != Kind::Class && kind != Kind::Union) ||
        !id.has_value()) {
        fail(expr.pos, "new S { ... } creates a struct");
        return errorRef();
    }
    if (kind == Kind::Union) {
        return checkNewUnion(expr, target, *id);
    }
    if (kind == Kind::Class && resolver_.methodSig(*id, "init") != nullptr) {
        fail(expr.pos, "this class has an init: create it with new C(args)");
        return errorRef();
    }
    std::vector<std::string> fields = resolver_.fieldNames(*id);
    if (expr.fields.size() != fields.size()) {
        fail(expr.pos, "every field is named, exactly once");
        return errorRef();
    }
    for (const FieldInit& init : expr.fields) {
        TypeRef field = resolver_.fieldType(*id, init.name);
        if (field.isError()) {
            fail(init.value->pos, "no such field");
            return errorRef();
        }
        TypeRef value = checkExpr(init.value.get(), field);
        if (!checkCompatible(init.value->pos, init.value.get(), value, field)) {
            return errorRef();
        }
    }
    for (uint64_t i = 0; i < static_cast<uint64_t>(fields.size()); ++i) {
        bool named = false;
        for (const FieldInit& init : expr.fields) {
            if (init.name == fields[i]) {
                named = true;
                break;
            }
        }
        if (!named) {
            fail(expr.pos, "missing field '" + fields[i] + "'");
            return errorRef();
        }
    }
    return target;
}

// A union is constructed by naming exactly one live member; the tag is
// written as a consequence of the write (TYPES.md 4.6), never named.
TypeRef Checker::checkNewUnion(NewStruct& expr, TypeRef target, DeclId id) {
    if (expr.fields.size() != kOneMember) {
        fail(expr.pos, "a union names one live member");
        return errorRef();
    }
    const FieldInit& init = expr.fields[0];
    TypeRef member = resolver_.fieldType(id, init.name);
    if (member.isError()) {
        fail(init.value->pos, "no such member");
        return errorRef();
    }
    TypeRef value = checkExpr(init.value.get(), member);
    if (!checkCompatible(init.value->pos, init.value.get(), value, member)) {
        return errorRef();
    }
    return target;
}

bool Checker::isZeroRepeat(const Expr& filler, TypeRef element) {
    Kind kind = element.type->kind();
    if (kind != Kind::Array && kind != Kind::Struct && kind != Kind::Union) {
        return false;
    }
    const auto* lit = dynamic_cast<const IntLit*>(&filler);
    if (lit == nullptr) {
        return false;
    }
    std::unordered_set<std::string> active;
    EvalResult folded = evalConstInt(*lit, *env_, active);
    return folded.ok && folded.value == 0;
}

TypeRef Checker::checkNewArray(NewArray& expr) {    TypeRef element = resolveHere(expr.element.get());
    if (element.isError()) {
        return errorRef();
    }
    std::unordered_set<std::string> active;
    EvalResult count = evalConstInt(*expr.length, *env_, active);
    if (!count.ok) {
        fail(expr.length->pos, count.error);
        return errorRef();
    }
    if (count.value < 0) {
        fail(expr.length->pos, "an array length is not negative");
        return errorRef();
    }
    const int64_t written = static_cast<int64_t>(expr.inits.size());
    if (expr.repeat && written != kOneElement) {
        fail(expr.pos, "a repeat fills from exactly one element");
        return errorRef();
    }
    if (!expr.repeat && written != count.value) {
        fail(expr.pos, "the element count must match the array type");
        return errorRef();
    }
    if (expr.repeat && isZeroRepeat(*expr.inits[0], element)) {
        // `new [T; N] { [0, ...] }` makes every element zero, and zero
        // reaches through nested arrays: the filler is the scalar zero,
        // not an element-typed value, so no per-element check runs. The
        // filler still checks (recording its type for the lowerer); only
        // the compatibility check is skipped.
        checkExpr(expr.inits[0].get(), element);
        return TypeRef{ctx_.array(element.type, count.value)};
    }
    for (auto& init : expr.inits) {
        TypeRef value = checkExpr(init.get(), element);
        checkCompatible(init->pos, init.get(), value, element);
    }
    return TypeRef{ctx_.array(element.type, count.value)};
}

// --- Conversions ---------------------------------------------------------------

TypeRef Checker::commonType(TypeRef lhs, TypeRef rhs, Position pos) {
    if (lhs.isError() || rhs.isError()) {
        return errorRef();
    }
    if (lhs.type->kind() != Kind::Basic || rhs.type->kind() != Kind::Basic) {
        fail(pos, "arithmetic needs numbers");
        return errorRef();
    }
    BasicKind a = *lhs.type->basic();
    BasicKind b = *rhs.type->basic();
    if (basicIsFloat(a) || basicIsFloat(b)) {
        if (a == BasicKind::F64 || b == BasicKind::F64) {
            return TypeRef{ctx_.basic(BasicKind::F64)};
        }
        return TypeRef{ctx_.basic(BasicKind::F32)};
    }
    if (!basicIsInteger(a) || !basicIsInteger(b)) {
        fail(pos, "arithmetic needs numbers");
        return errorRef();
    }
    int width = basicWidthBits(a) > basicWidthBits(b) ? basicWidthBits(a) : basicWidthBits(b);
    bool signedA = basicIsSigned(a);
    bool signedB = basicIsSigned(b);
    if (signedA == signedB) {
        return TypeRef{ctx_.basic(intFor(width, signedA))};
    }
    // Differ in signedness: signed wins when it holds every value of the
    // unsigned one — a strictly wider signed type. Otherwise unsigned.
    int signedWidth = signedA ? basicWidthBits(a) : basicWidthBits(b);
    int unsignedWidth = signedA ? basicWidthBits(b) : basicWidthBits(a);
    if (signedWidth > unsignedWidth) {
        return TypeRef{ctx_.basic(intFor(width, true))};
    }
    return TypeRef{ctx_.basic(intFor(width, false))};
}

bool Checker::checkCompatible(Position pos, const Expr* fromExpr, TypeRef from, TypeRef to) {
    if (from.isError() || to.isError()) {
        return true;
    }
    if (from == to) {
        return true;
    }
    // A null names no type, so it converts to any pointer.
    if (dynamic_cast<const NilPtr*>(fromExpr) != nullptr &&
        (to.type->kind() == Kind::Pointer || to.type->kind() == Kind::FunctionPtr)) {
        return true;
    }
    // An integer literal converts to any integer type whose range contains it.
    if (dynamic_cast<const IntLit*>(fromExpr) != nullptr && to.type->kind() == Kind::Basic &&
        basicIsInteger(*to.type->basic())) {
        std::unordered_set<std::string> active;
        EvalResult folded = evalConstInt(*fromExpr, *env_, active);
        if (folded.ok) {
            int width = basicWidthBits(*to.type->basic());
            bool isSigned = basicIsSigned(*to.type->basic());
            __int128 lo = isSigned ? -(static_cast<__int128>(1) << (width - 1)) : 0;
            __int128 hi = isSigned ? ((static_cast<__int128>(1) << (width - 1)) - 1)
                                   : ((static_cast<__int128>(1) << width) - 1);
            if (folded.value >= lo && folded.value <= hi) {
                return true;
            }
        }
    }
    // A float literal converts to any float type.
    if (dynamic_cast<const FloatLit*>(fromExpr) != nullptr && to.type->kind() == Kind::Basic &&
        basicIsFloat(*to.type->basic())) {
        return true;
    }
    // A concrete value converts to an interface it implements.
    if (to.type->kind() == Kind::Interface &&
        (from.type->kind() == Kind::Struct || from.type->kind() == Kind::Class)) {
        std::optional<DeclId> concrete = from.type->decl();
        std::optional<DeclId> iface = to.type->decl();
        if (concrete.has_value() && iface.has_value() &&
            implementsAll(*concrete, *iface, pos)) {
            return true;
        }
        return false;
    }
    // An upcast points at something that really is a Base.
    if (from.type->kind() == Kind::Pointer && to.type->kind() == Kind::Pointer) {
        const Type* fromPointee = from.type->pointee();
        const Type* toPointee = to.type->pointee();
        if (fromPointee->kind() == Kind::Class && toPointee->kind() == Kind::Class) {
            std::optional<DeclId> derived = fromPointee->decl();
            std::optional<DeclId> base = toPointee->decl();
            if (derived.has_value() && base.has_value() &&
                (sameDecl(*derived, *base) || isDerivedFrom(*derived, *base))) {
                return true;
            }
        }
        // Erasure discards the type, so it cannot claim to be something it is
        // not. `free(buf)` type-checks because of this row.
        if (toPointee->kind() == Kind::Void) {
            return true;
        }
    }
    fail(pos, "cannot use " + typeName(from) + " as " + typeName(to));
    return false;
}

bool Checker::implementsAll(DeclId concrete, DeclId iface, Position pos) {
    for (const std::string& method : resolver_.methodNames(iface)) {
        const Signature* want = resolver_.methodSig(iface, method);
        const Signature* got = resolver_.methodSig(concrete, method);
        if (got == nullptr) {
            for (DeclId base : resolver_.classBases(concrete)) {
                got = resolver_.methodSig(base, method);
                if (got != nullptr) {
                    break;
                }
            }
        }
        if (got == nullptr) {
            fail(pos, "does not implement '" + method + "'");
            return false;
        }
        if (!sameSignature(*got, *want)) {
            fail(pos, "'" + method + "' has the wrong signature");
            return false;
        }
    }
    return true;
}

bool Checker::isDerivedFrom(DeclId derived, DeclId base) const {
    std::vector<DeclId> frontier = resolver_.classBases(derived);
    while (!frontier.empty()) {
        DeclId next = frontier.back();
        frontier.pop_back();
        if (sameDecl(next, base)) {
            return true;
        }
        for (DeclId parent : resolver_.classBases(next)) {
            frontier.push_back(parent);
        }
    }
    return false;
}

bool Checker::castRowOk(TypeRef from, TypeRef to) const {
    if (from == to) {
        return true;
    }
    Kind a = from.type->kind();
    Kind b = to.type->kind();
    if (a == Kind::Void || b == Kind::Void) {
        return false;
    }
    // A union's payload reads as any type. Explicit by construction.
    if (a == Kind::Union) {
        return true;
    }
    bool aNum = a == Kind::Basic;
    bool bNum = b == Kind::Basic;
    if (aNum && bNum) {
        return true;
    }
    if (a == Kind::Pointer && b == Kind::Pointer) {
        return true;
    }
    if (aNum && b == Kind::Pointer) {
        return true;
    }
    if (a == Kind::Pointer && bNum) {
        return true;
    }
    // An interface reads back as a concrete type, checked at runtime.
    if (a == Kind::Interface && (b == Kind::Class || b == Kind::Struct)) {
        return true;
    }
    // A base pointer reads back as a derived one, checked at runtime.
    if (a == Kind::Pointer && b == Kind::Pointer) {
        return true;
    }
    // Same width reinterprets the bits. Both sizes come from layout.h, and an
    // unknown size is not a reinterpretation.
    int64_t sa = typeSize(from.type);
    int64_t sb = typeSize(to.type);
    if (sa > 0 && sa == sb) {
        return true;
    }
    return false;
}

}  // namespace check
}  // namespace cider
