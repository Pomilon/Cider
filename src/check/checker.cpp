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

#include "ast/operator.h"
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
constexpr uint64_t kNoParameters = 0;
constexpr uint64_t kOneParameter = 1;
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

Checker::Checker(TypeContext& ctx, resolve::Resolver& resolver, target::Kind target)
    : ctx_(ctx), resolver_(resolver), target_(target) {}

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

const resolve::Decl* Checker::operatorDecl(const Binary* expr) const {
    auto found = operatorDecls_.find(expr);
    return found == operatorDecls_.end() ? nullptr : found->second;
}

const resolve::Decl* Checker::operatorDecl(const Unary* expr) const {
    auto found = unaryOperatorDecls_.find(expr);
    return found == unaryOperatorDecls_.end() ? nullptr : found->second;
}

bool Checker::isMemberOperator(const Binary* expr) const {
    return memberOperatorCalls_.find(expr) != memberOperatorCalls_.end();
}

bool Checker::isMemberOperator(const Unary* expr) const {
    return memberUnaryOperatorCalls_.find(expr) != memberUnaryOperatorCalls_.end();
}

bool Checker::isOperatorIndex(const Index* expr) const {
    return operatorIndexes_.find(expr) != operatorIndexes_.end();
}

bool Checker::isOperatorIndexAssign(const Assign* expr) const {
    return operatorIndexAssigns_.find(expr) != operatorIndexAssigns_.end();
}

bool Checker::isOperatorCall(const Call* expr) const {
    return operatorCalls_.find(expr) != operatorCalls_.end();
}

bool Checker::isOperatorAssign(const Assign* expr) const {
    return operatorAssigns_.find(expr) != operatorAssigns_.end();
}

bool Checker::isOperatorArrow(const Field* expr) const {
    return operatorArrows_.find(expr) != operatorArrows_.end();
}

TypeRef Checker::operatorArrowTarget(const Field* expr) const {
    auto found = operatorArrows_.find(expr);
    return found != operatorArrows_.end() ? found->second : TypeRef{ctx_.error()};
}

std::optional<DeclId> Checker::specOf(const Call* call,
                                              const std::optional<DeclId>& ctx) const {
    std::string key;
    if (ctx.has_value()) {
        key = std::to_string(ctx->module) + "." + std::to_string(ctx->index);
    }
    auto found = callSpecs_.find(std::make_pair(call, key));
    return found == callSpecs_.end() ? std::nullopt
                                     : std::optional<DeclId>(found->second);
}

Prov Checker::paramProv(const std::string& key) const {
    auto found = paramSites_.find(key);
    if (found == paramSites_.end() || !found->second.seen) {
        return Prov::Raw;
    }
    if (found->second.allClosure) {
        return Prov::Closure;
    }
    if (found->second.allNeutral) {
        return Prov::Neutral;
    }
    return Prov::Raw;
}

void Checker::prescanProvenance(const std::vector<std::string>& modules) {
    // Fixpoint over parameter sites: promotions only settle entries toward
    // their final unanimous value, so an unchanged round ends it.
    while (prescanRound(modules)) {
    }
}

bool Checker::prescanRound(const std::vector<std::string>& modules) {
    prescanMoved_ = false;
    for (const std::string& mod : modules) {
        Program* program = resolver_.programOf(mod);
        if (program == nullptr) {
            continue;
        }
        // Global initializers seed prescan globals (constants only in
        // practice); stores during function walks update them in order.
        for (const auto& decl : program->decls) {
            const VarDecl* global = nullptr;
            if (const auto* vd = dynamic_cast<const VarDecl*>(decl.get())) {
                global = vd;
            } else if (const auto* exp = dynamic_cast<const ExportDecl*>(decl.get())) {
                global = dynamic_cast<const VarDecl*>(exp->decl.get());
            }
            if (global != nullptr && global->init != nullptr) {
                if (dynamic_cast<const NilPtr*>(global->init.get()) != nullptr) {
                    prescanGlobals_[global->name] = Prov::Neutral;
                } else if (dynamic_cast<const FuncLit*>(global->init.get()) != nullptr) {
                    prescanGlobals_[global->name] = Prov::Closure;
                }
            }
        }
        for (const auto& decl : program->decls) {
            const FuncDecl* fn = nullptr;
            if (const auto* direct = dynamic_cast<const FuncDecl*>(decl.get())) {
                fn = direct;
            } else if (const auto* exp = dynamic_cast<const ExportDecl*>(decl.get())) {
                fn = dynamic_cast<const FuncDecl*>(exp->decl.get());
            }
            if (fn == nullptr || !fn->typeParams.empty() || fn->body == nullptr) {
                continue;
            }
            prescanFunction(mod, *fn);
        }
    }
    return prescanMoved_;
}

void Checker::prescanFunction(const std::string& mod, const FuncDecl& fn) {
    prescanModule_ = mod;
    // Closure-typed parameters prove by spelling (no call needed); the
    // rest start Raw and earn proof from call sites below.
    std::unordered_map<std::string, bool> closureParams;
    const resolve::Scope* home = resolver_.scopeOf(mod);
    for (const Param& param : fn.params) {
        if (param.type != nullptr && home != nullptr) {
            TypeRef spelled = resolver_.resolveType(param.type.get(), home);
            if (!spelled.isError() &&
                spelled.type->kind() == Kind::Closure) {
                closureParams[param.name] = true;
            }
        }
    }
    std::vector<std::unordered_map<std::string, Prov>> scopes(1);
    prescanStmt(fn.body.get(), scopes, closureParams);
}

Prov Checker::prescanArg(const Expr* expr,
                          std::vector<std::unordered_map<std::string, Prov>>& scopes,
                          const std::unordered_map<std::string, bool>& closureParams) {
    if (expr == nullptr) {
        return Prov::Raw;
    }
    if (dynamic_cast<const NilPtr*>(expr) != nullptr) {
        return Prov::Neutral;
    }
    if (dynamic_cast<const FuncLit*>(expr) != nullptr) {
        return Prov::Closure;
    }
    if (const auto* ident = dynamic_cast<const Ident*>(expr)) {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto found = it->find(ident->name);
            if (found != it->end()) {
                return found->second;
            }
        }
        auto global = prescanGlobals_.find(ident->name);
        if (global != prescanGlobals_.end()) {
            return global->second;
        }
        if (closureParams.find(ident->name) != closureParams.end()) {
            return Prov::Closure;
        }
        return Prov::Raw;
    }
    if (const auto* cast = dynamic_cast<const Cast*>(expr)) {
        // Casts preserve: a proven address stays proven through any
        // spelling change, and an unproven one stays unproven.
        return prescanArg(cast->expr.get(), scopes, closureParams);
    }
    return Prov::Raw;
}

void Checker::prescanSite(const std::string& mod, const Call& call,
                            std::vector<std::unordered_map<std::string, Prov>>& scopes,
                            const std::unordered_map<std::string, bool>& closureParams) {
    // Only free-function callees name a parameter table: methods,
    // generics and indirect calls contribute no sites (documented).
    const resolve::Scope* home = resolver_.scopeOf(mod);
    if (home == nullptr) {
        return;
    }
    const resolve::Decl* decl = nullptr;
    if (const auto* ident = dynamic_cast<const Ident*>(call.callee.get())) {
        decl = resolver_.lookup(home, ident->name);
    } else if (const auto* field = dynamic_cast<const Field*>(call.callee.get())) {
        const auto* modName = dynamic_cast<const Ident*>(field->object.get());
        if (modName == nullptr) {
            return;
        }
        const resolve::Decl* moduleDecl = resolver_.lookup(home, modName->name);
        if (moduleDecl == nullptr ||
            moduleDecl->kind != resolve::DeclKind::Module) {
            return;
        }
        const resolve::Scope* target = resolver_.scopeOf(moduleDecl->origin);
        decl = target != nullptr ? resolver_.lookup(target, field->name) : nullptr;
    } else {
        return;
    }
    if (decl == nullptr || decl->kind != resolve::DeclKind::Func ||
        decl->typeParams > 0 || decl->scope == nullptr) {
        return;
    }
    std::string base =
        decl->scope->module + "." + decl->name + "#";
    for (uint64_t i = 0; i < static_cast<uint64_t>(call.args.size()); ++i) {
        // Places and returns read Raw here (next slices); names, casts,
        // literals and nil answer above.
        std::string key =
            base + std::to_string(static_cast<unsigned long long>(i));
        Prov arg = prescanArg(call.args[i].get(), scopes, closureParams);
        ParamSite& site = paramSites_[key];
        bool keepC = site.allClosure && (arg == Prov::Closure);
        bool keepN = site.allNeutral && (arg == Prov::Neutral);
        if (!site.seen || keepC != site.allClosure || keepN != site.allNeutral) {
            site.seen = true;
            site.allClosure = keepC;
            site.allNeutral = keepN;
            prescanMoved_ = true;
        }
    }
}

void Checker::prescanStmt(const Stmt* stmt,
                          std::vector<std::unordered_map<std::string, Prov>>& scopes,
                          const std::unordered_map<std::string, bool>& closureParams) {
    if (stmt == nullptr) {
        return;
    }
    if (const auto* vd = dynamic_cast<const VarDecl*>(stmt)) {
        if (vd->init != nullptr) {
            // Scan first (nested calls record their sites), then classify.
            prescanExpr(vd->init.get(), scopes, closureParams);
            scopes.back()[vd->name] =
                prescanArg(vd->init.get(), scopes, closureParams);
        }
        return;
    }
    if (const auto* es = dynamic_cast<const ExprStmt*>(stmt)) {
        prescanExpr(es->expr.get(), scopes, closureParams);
        return;
    }
    if (const auto* ret = dynamic_cast<const Return*>(stmt)) {
        prescanExpr(ret->value.get(), scopes, closureParams);
        return;
    }
    if (const auto* block = dynamic_cast<const Block*>(stmt)) {
        scopes.emplace_back();
        for (const auto& inner : block->stmts) {
            prescanStmt(inner.get(), scopes, closureParams);
        }
        scopes.pop_back();
        return;
    }
    if (const auto* defer = dynamic_cast<const DeferStmt*>(stmt)) {
        // Exit-time values may exceed registration-time ones; walking in
        // place under-proves (refuses) rather than over-proves.
        prescanStmt(defer->stmt.get(), scopes, closureParams);
        return;
    }
    if (const auto* cond = dynamic_cast<const IfStmt*>(stmt)) {
        prescanExpr(cond->cond.get(), scopes, closureParams);
        // Branches may disagree: keep what both prove, Raw otherwise. A
        // name one branch never touches keeps the incoming value.
        auto merge = [&](std::unordered_map<std::string, Prov> branch) {
            for (const auto& [name, prov] : branch) {
                auto base = scopes.back().find(name);
                Prov before = base != scopes.back().end()
                                  ? base->second
                                  : Prov::Raw;
                if (prov != before) {
                    scopes.back()[name] = Prov::Raw;
                } else {
                    scopes.back()[name] = prov;
                }
            }
        };
        auto then = scopes.back();
        prescanStmt(cond->then.get(), scopes, closureParams);
        std::unordered_map<std::string, Prov> thenOut = scopes.back();
        scopes.back() = then;
        if (cond->els != nullptr) {
            prescanStmt(cond->els.get(), scopes, closureParams);
        }
        std::unordered_map<std::string, Prov> elseOut = scopes.back();
        scopes.back() = then;
        merge(thenOut);
        // Else-side: only demote where it disagrees with the merged value.
        for (const auto& [name, prov] : elseOut) {
            auto base = then.find(name);
            Prov before = base != then.end() ? base->second : Prov::Raw;
            if (prov != before) {
                scopes.back()[name] = Prov::Raw;
            } else if (scopes.back().find(name) == scopes.back().end()) {
                scopes.back()[name] = prov;
            }
        }
        return;
    }
    if (const auto* loop = dynamic_cast<const WhileStmt*>(stmt)) {
        prescanExpr(loop->cond.get(), scopes, closureParams);
        // One speculative pass on a copy: loop-carried writes stay
        // unproven (a zero-trip loop runs nothing), but sites inside
        // still record.
        auto body = scopes.back();
        std::vector<std::unordered_map<std::string, Prov>> inner{body};
        prescanStmt(loop->body.get(), inner, closureParams);
        mergePrescan(scopes.back(), body);
        return;
    }
    if (const auto* loop = dynamic_cast<const ForStmt*>(stmt)) {
        prescanStmt(loop->init.get(), scopes, closureParams);
        prescanExpr(loop->cond.get(), scopes, closureParams);
        prescanExpr(loop->step.get(), scopes, closureParams);
        auto body = scopes.back();
        std::vector<std::unordered_map<std::string, Prov>> inner{body};
        prescanStmt(loop->body.get(), inner, closureParams);
        mergePrescan(scopes.back(), body);
        return;
    }
    if (const auto* loop = dynamic_cast<const ForInStmt*>(stmt)) {
        prescanExpr(loop->seq.get(), scopes, closureParams);
        auto body = scopes.back();
        std::vector<std::unordered_map<std::string, Prov>> inner{body};
        prescanStmt(loop->body.get(), inner, closureParams);
        mergePrescan(scopes.back(), body);
        return;
    }
    if (const auto* match = dynamic_cast<const MatchStmt*>(stmt)) {
        prescanExpr(match->scrutinee.get(), scopes, closureParams);
        // Arms run at most one: keep what every arm agrees on. Each arm
        // still records its own sites.
        bool first = true;
        std::unordered_map<std::string, Prov> agreed;
        auto takeArm = [&](const Block* body) {
            if (body == nullptr) {
                return;
            }
            auto copy = scopes.back();
            std::vector<std::unordered_map<std::string, Prov>> inner{copy};
            prescanStmt(body, inner, closureParams);
            if (first) {
                agreed = inner.back();
                first = false;
                return;
            }
            for (auto it = agreed.begin(); it != agreed.end();) {
                auto found = inner.back().find(it->first);
                if (found == inner.back().end() || found->second != it->second) {
                    it = agreed.erase(it);
                } else {
                    ++it;
                }
            }
        };
        for (const auto& arm : match->arms) {
            if (arm->guard != nullptr) {
                prescanExpr(arm->guard.get(), scopes, closureParams);
            }
            takeArm(arm->body.get());
        }
        if (match->fallback != nullptr) {
            takeArm(match->fallback.get());
        }
        if (!first) {
            scopes.back() = agreed;
        }
        return;
    }
}

void Checker::mergePrescan(std::unordered_map<std::string, Prov>& into,
                           const std::unordered_map<std::string, Prov>& before) {
    // Loop-carried writes stay unproven (see above): only entries the
    // body left exactly as found survive.
    for (auto it = into.begin(); it != into.end();) {
        auto found = before.find(it->first);
        if (found == before.end() || found->second != it->second) {
            it = into.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto& [name, prov] : before) {
        if (into.find(name) == into.end()) {
            into[name] = prov;
        }
    }
}

void Checker::prescanExpr(const Expr* expr,
                          std::vector<std::unordered_map<std::string, Prov>>& scopes,
                          const std::unordered_map<std::string, bool>& closureParams) {
    if (expr == nullptr) {
        return;
    }
    // The module is recovered per call below; prescanSite resolves it.
    if (const auto* call = dynamic_cast<const Call*>(expr)) {
        prescanSite(prescanModule_, *call, scopes, closureParams);
        for (const auto& arg : call->args) {
            prescanExpr(arg.get(), scopes, closureParams);
        }
        return;
    }
    if (const auto* assign = dynamic_cast<const Assign*>(expr)) {
        prescanExpr(assign->value.get(), scopes, closureParams);
        if (const auto* ident = dynamic_cast<const Ident*>(assign->target.get())) {
            if (!assign->op.has_value()) {
                Prov proven = prescanArg(assign->value.get(), scopes, closureParams);
                // A declared local takes it in flow; otherwise the name is
                // a global and records persistently. Undeclared uses stay
                // loud in checking; here they read Raw.
                bool bound = false;
                for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
                    if (it->find(ident->name) != it->end()) {
                        bound = true;
                        break;
                    }
                }
                if (bound) {
                    scopes.back()[ident->name] = proven;
                } else {
                    prescanGlobals_[ident->name] = proven;
                }
            }
        } else {
            prescanExpr(assign->target.get(), scopes, closureParams);
        }
        return;
    }
    if (const auto* binary = dynamic_cast<const Binary*>(expr)) {
        prescanExpr(binary->lhs.get(), scopes, closureParams);
        prescanExpr(binary->rhs.get(), scopes, closureParams);
        return;
    }
    if (const auto* unary = dynamic_cast<const Unary*>(expr)) {
        prescanExpr(unary->operand.get(), scopes, closureParams);
        return;
    }
    if (const auto* cast = dynamic_cast<const Cast*>(expr)) {
        prescanExpr(cast->expr.get(), scopes, closureParams);
        return;
    }
    if (const auto* ternary = dynamic_cast<const Ternary*>(expr)) {
        prescanExpr(ternary->cond.get(), scopes, closureParams);
        prescanExpr(ternary->then.get(), scopes, closureParams);
        prescanExpr(ternary->els.get(), scopes, closureParams);
        return;
    }
    if (const auto* index = dynamic_cast<const Index*>(expr)) {
        prescanExpr(index->array.get(), scopes, closureParams);
        prescanExpr(index->index.get(), scopes, closureParams);
        return;
    }
    if (const auto* field = dynamic_cast<const Field*>(expr)) {
        prescanExpr(field->object.get(), scopes, closureParams);
        return;
    }
    if (const auto* slice = dynamic_cast<const SliceExpr*>(expr)) {
        prescanExpr(slice->array.get(), scopes, closureParams);
        prescanExpr(slice->lo.get(), scopes, closureParams);
        prescanExpr(slice->hi.get(), scopes, closureParams);
        return;
    }
    if (const auto* inc = dynamic_cast<const IncrDecr*>(expr)) {
        prescanExpr(inc->operand.get(), scopes, closureParams);
        return;
    }
    if (const auto* news = dynamic_cast<const NewStruct*>(expr)) {
        for (const auto& field : news->fields) {
            prescanExpr(field.value.get(), scopes, closureParams);
        }
        return;
    }
    if (const auto* newa = dynamic_cast<const NewArray*>(expr)) {
        for (const auto& init : newa->inits) {
            prescanExpr(init.get(), scopes, closureParams);
        }
        return;
    }
    if (const auto* newc = dynamic_cast<const NewClass*>(expr)) {
        for (const auto& arg : newc->args) {
            prescanExpr(arg.get(), scopes, closureParams);
        }
        return;
    }
    if (const auto* si = dynamic_cast<const SuperInit*>(expr)) {
        for (const auto& arg : si->args) {
            prescanExpr(arg.get(), scopes, closureParams);
        }
        return;
    }
}

TypeRef Checker::sizeofOperand(const SizeofType* node) const {
    auto found = sizeofOperands_.find(node);
    return found == sizeofOperands_.end() ? errorRef() : found->second;
}

TypeRef Checker::sizeofExprOperand(const SizeofExpr* node) const {
    auto found = sizeofExprOperands_.find(node);
    return found == sizeofExprOperands_.end() ? errorRef() : found->second;
}

TypeRef Checker::alignofOperand(const AlignofType* node) const {
    auto found = alignofOperands_.find(node);
    return found == alignofOperands_.end() ? errorRef() : found->second;
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
    if (kind == Kind::Struct || kind == Kind::Class || kind == Kind::Interface ||
        kind == Kind::Union || kind == Kind::Enum) {
        // A nested instantiation re-originates: `R<T, E>` inside `ok<T, E>`
        // names the spec of bare parameters, but the body means the spec
        // of this walk's arguments. Substitute the origin's arguments and
        // re-instantiate, the way the resolver's own substitution does;
        // an unchanged list keeps the same spec. A depth failure refuses
        // instead of hanging (see the resolver's bound).
        std::optional<DeclId> id = type.type->decl();
        if (!id.has_value()) {
            return errorRef();
        }
        std::optional<std::pair<DeclId, std::vector<TypeRef>>> origin =
            resolver_.instantiationOf(*id);
        if (!origin.has_value()) {
            return type;
        }
        std::vector<TypeRef> subArgs;
        bool changed = false;
        for (TypeRef arg : origin->second) {
            TypeRef sub = substDeep(arg);
            if (sub.isError()) {
                return sub;
            }
            changed = changed || sub.type != arg.type;
            subArgs.push_back(sub);
        }
        if (!changed) {
            return type;
        }
        std::optional<DeclId> spec = resolver_.instantiate(
            origin->first, subArgs, Position{module_, 1, 1});
        if (!spec.has_value()) {
            return errorRef();
        }
        return TypeRef{ctx_.nominal(kind, *spec)};
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

// v4's entry is source-defined. The driver owns the command-level decision
// to require one; this method owns the target shape so `check` and `compile`
// cannot disagree about what a valid entry is.
bool Checker::checkEntry(Program& program) {
    const bool uefi = target_ == target::Kind::Uefi;
    const bool pomeos = target_ == target::Kind::Pomeos;
    const std::string expected = uefi ? "efi_main" : "_start";
    const FuncDecl* entry = nullptr;
    for (const std::unique_ptr<Decl>& decl : program.decls) {
        const Decl* node = decl.get();
        if (const auto* exported = dynamic_cast<const ExportDecl*>(node)) {
            node = exported->decl.get();
        }
        const auto* fn = dynamic_cast<const FuncDecl*>(node);
        if (fn != nullptr && fn->name == expected) {
            entry = fn;
            break;
        }
    }
    if (entry == nullptr) {
        if (uefi) {
            fail(program.pos,
                 "no entry function for uefi: define msabi func "
                 "efi_main(image_handle: *void, system_table: *void) -> int");
        } else {
            fail(program.pos, "no entry function: define func _start() -> void");
        }
        return false;
    }
    if (!entry->typeParams.empty()) {
        fail(entry->pos, expected + " cannot be generic");
        return false;
    }
    const resolve::Decl* binding =
        resolver_.moduleScope() != nullptr
            ? resolver_.lookup(resolver_.moduleScope(), expected)
            : nullptr;
    if (binding == nullptr || binding->kind != resolve::DeclKind::Func) {
        fail(entry->pos, "checker bug: " + expected + " was never resolved");
        return false;
    }
    const Signature& sig = binding->sig;
    if (uefi) {
        if (entry->abi != "msabi") {
            fail(entry->pos, "efi_main must be declared msabi");
            return false;
        }
        if (sig.arity() != 2) {
            fail(entry->pos,
                 "efi_main takes exactly two parameters: "
                 "image_handle and system_table");
            return false;
        }
        for (uint64_t i = 0; i < sig.arity(); ++i) {
            const Type* type = sig.parameters()[i].type;
            if (type == nullptr || type->kind() != Kind::Pointer ||
                type->pointee()->kind() != Kind::Void) {
                const std::string name =
                    i < entry->params.size() ? entry->params[i].name
                                             : std::to_string(i + 1);
                fail(entry->pos,
                     "efi_main parameter '" + name + "' must be *void");
                return false;
            }
        }
        const Type* ret = sig.returnType();
        if (ret == nullptr || ret->kind() != Kind::Basic ||
            *ret->basic() != BasicKind::I32) {
            fail(entry->pos, "efi_main must return int");
            return false;
        }
        if (sig.abi() != Abi::MsAbi) {
            fail(entry->pos, "efi_main must use the msabi calling convention");
            return false;
        }
        return true;
    }
    if (!entry->params.empty()) {
        if (pomeos) {
            // PomeOS enters `_start` with argc in rdi and argv in rsi. A
            // program that does not need them may declare a parameterless
            // entry; one that does declares the reference signature. Both
            // are SysV, so the emitter needs no argument shuffling.
            if (sig.arity() != 2) {
                fail(entry->pos,
                     "_start takes no parameters or (argc: int, argv: **char)");
                return false;
            }
            const Type* argcType = sig.parameters()[0].type;
            if (argcType == nullptr || argcType->kind() != Kind::Basic ||
                *argcType->basic() != BasicKind::I32) {
                fail(entry->pos, "_start argc parameter must be int");
                return false;
            }
            const Type* argvType = sig.parameters()[1].type;
            const Type* pointer = argvType != nullptr &&
                                          argvType->kind() == Kind::Pointer
                                      ? argvType->pointee()
                                      : nullptr;
            const Type* character = pointer != nullptr &&
                                            pointer->kind() == Kind::Pointer
                                        ? pointer->pointee()
                                        : nullptr;
            if (character == nullptr || character->kind() != Kind::Basic ||
                *character->basic() != BasicKind::Char) {
                fail(entry->pos, "_start argv parameter must be **char");
                return false;
            }
        } else {
            fail(entry->pos, "_start takes no parameters");
            return false;
        }
    }
    const Type* ret = sig.returnType();
    if (ret == nullptr || ret->kind() != Kind::Void) {
        fail(entry->pos, "_start must return void");
        return false;
    }
    if (sig.abi() != Abi::SysV) {
        fail(entry->pos, "_start must use the sysv calling convention");
        return false;
    }
    return true;
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
        // Export wraps the spelling: unwrap it so exported templates
        // register their bodies like local ones do. Visit order already
        // sees through the wrapper; only this table missed it.
        const Decl* inner = decl.get();
        if (const auto* exp = dynamic_cast<const ExportDecl*>(inner)) {
            inner = exp->decl.get();
        }
        if (const auto* fn = dynamic_cast<const FuncDecl*>(inner)) {
            funcs_[module + "." + fn->name] = fn;
        }
        if (const auto* sd = dynamic_cast<const StructDecl*>(inner)) {
            structs_[module + "." + sd->name] = sd;
            for (const MethodDecl& method : sd->methods) {
                methods_[module + "." + sd->name + "." + method.name] = &method;
            }
        }
        if (const auto* cd = dynamic_cast<const ClassDecl*>(inner)) {
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
    flowStack_.push_back({});
    placeFlow_.push_back({});
    volatilePtrs_.push_back({});
    return out;
}

// A declaration spelled `*volatile T` marks its name so derefs through it
// are volatile accesses; anything else is silent.
void Checker::noteVolatilePointer(const std::string& name, const TypeExpr* type) {
    const auto* ptr = dynamic_cast<const PointerType*>(type);
    if (ptr == nullptr || !ptr->pointeeVolatile) {
        return;
    }
    if (!volatilePtrs_.empty()) {
        volatilePtrs_.back().insert(name);
    }
}

bool Checker::placeIsVolatile(const Expr* expr) const {
    return expr != nullptr && volatilePlaces_.find(expr) != volatilePlaces_.end();
}

void Checker::popScope() {
    current_ = current_ != nullptr ? current_->parent : nullptr;
    if (!flowStack_.empty()) {
        flowStack_.pop_back();
    }
    if (!placeFlow_.empty()) {
        placeFlow_.pop_back();
    }
    if (!volatilePtrs_.empty()) {
        volatilePtrs_.pop_back();
    }
}

void Checker::recordFlow(const std::string& name, Prov prov) {
    if (flowStack_.empty()) {
        return;
    }
    flowStack_.back()[name] = prov;
}

Prov Checker::provOf(const Expr* expr) {
    if (expr == nullptr) {
        return Prov::Raw;
    }
    // nilPtr is representation-neutral: it casts to the null value of any
    // carrier spelling without proving anything about storage.
    if (dynamic_cast<const NilPtr*>(expr) != nullptr) {
        return Prov::Neutral;
    }
    // A closure literal is a closure object wherever it flows.
    if (dynamic_cast<const FuncLit*>(expr) != nullptr) {
        return Prov::Closure;
    }
    if (const auto* ident = dynamic_cast<const Ident*>(expr)) {
        return provOfName(ident->name);
    }
    if (const auto* cast = dynamic_cast<const Cast*>(expr)) {
        // Casts preserve: a proven address stays proven through any
        // spelling change, and an unproven one stays unproven.
        return provOf(cast->expr.get());
    }
    if (const auto* ternary = dynamic_cast<const Ternary*>(expr)) {
        // Branches join: neutrality is transparent (a nil arm proves
        // nothing either way), agreement keeps the value, anything else
        // refuses by falling to Raw.
        Prov then = provOf(ternary->then.get());
        Prov els = provOf(ternary->els.get());
        if (then == els) {
            return then;
        }
        if (then == Prov::Neutral) {
            return els;
        }
        if (els == Prov::Neutral) {
            return then;
        }
        return Prov::Raw;
    }
    // Places answer their tracked chain, or Raw when no store proved it.
    std::string key = placeKey(expr);
    if (!key.empty()) {
        for (auto it = placeFlow_.rbegin(); it != placeFlow_.rend(); ++it) {
            auto found = it->find(key);
            if (found != it->end()) {
                return found->second;
            }
        }
    }
    return Prov::Raw;
}

std::string Checker::placeKey(const Expr* expr) {
    if (const auto* ident = dynamic_cast<const Ident*>(expr)) {
        return "v:" + ident->name;
    }
    if (const auto* deref = dynamic_cast<const Unary*>(expr)) {
        if (deref->op != UnOp::Deref) {
            return "";
        }
        std::string inner = placeKey(deref->operand.get());
        if (inner.empty()) {
            return "";
        }
        return "d(" + inner + ")";
    }
    if (const auto* field = dynamic_cast<const Field*>(expr)) {
        std::string inner = placeKey(field->object.get());
        if (inner.empty()) {
            return "";
        }
        return "f(" + inner + "," + field->name + ")";
    }
    if (const auto* index = dynamic_cast<const Index*>(expr)) {
        std::string inner = placeKey(index->array.get());
        if (inner.empty()) {
            return "";
        }
        // Only a folded index names a cell: a computed index could name
        // any element, so the whole Chain stays unproven.
        std::unordered_set<std::string> active;
        EvalResult folded = evalConstInt(*index->index, *env_, active);
        if (!folded.ok) {
            return "";
        }
        return "i(" + inner + "," + std::to_string(folded.value) + ")";
    }
    return "";
}

Prov Checker::provOfName(const std::string& name) const {
    for (auto it = flowStack_.rbegin(); it != flowStack_.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) {
            return found->second;
        }
    }
    auto global = globalProv_.find(name);
    if (global != globalProv_.end()) {
        return global->second;
    }
    // Untracked names default by type: a closure-typed name is its own
    // proof (nothing else inhabits the type), while carriers start Raw
    // until a tracked store proves them.
    if (const Local* local = findLocal(name)) {
        if (!local->type.isError() &&
            local->type.type->kind() == Kind::Closure) {
            return Prov::Closure;
        }
    }
    return Prov::Raw;
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
        // `gc.MK2`: a module-qualified const folds through its defining
        // module — the table holds every module's consts, but names inside
        // only resolve in their own scope, so the fold runs there. Anything
        // that is not a const member falls through and fails loudly below,
        // exactly as an unresolvable name would.
        EvalResult folded{false, 0, ""};
        const Field* qualField =
            dynamic_cast<const Field*>(g->init.get());
        const Ident* qualMod =
            qualField != nullptr
                ? dynamic_cast<const Ident*>(qualField->object.get())
                : nullptr;
        const resolve::Decl* qualMember = nullptr;
        const resolve::Scope* qualHome = nullptr;
        if (qualMod != nullptr) {
            const resolve::Decl* modDecl =
                resolver_.lookup(moduleScope_, qualMod->name);
            if (modDecl != nullptr &&
                modDecl->kind == resolve::DeclKind::Module) {
                qualHome = resolver_.scopeOf(modDecl->origin);
                const resolve::Decl* target =
                    qualHome != nullptr
                        ? resolver_.lookup(qualHome, qualField->name)
                        : nullptr;
                if (target != nullptr &&
                    target->kind == resolve::DeclKind::Const &&
                    target->init != nullptr) {
                    qualMember = target;
                }
            }
        }
        if (qualMember != nullptr && qualHome != nullptr) {
            EvalEnv homeEnv = resolver_.evalEnv(qualHome);
            folded = evalConstInt(*qualMember->init, homeEnv, active);
        } else {
            folded = evalConstInt(*g->init, *env_, active);
        }
        if (!folded.ok) {
            // An aggregate construction whose every scalar leaf folds to
            // an integer names every image byte, so a global may hold it:
            // the bytes are known and there is no startup code to order
            // (FEATURES.md §8.3). A zero repeat is the all-zero case of
            // the same rule. Anything else constructed is not constant.
            // The normal checking below still enforces the types
            // (literal adaptation for scalars, isZeroRepeat for
            // aggregates), so the gate only recognises the
            // all-bytes-known shape.
            bool constGlobal = false;
            if (dynamic_cast<const NewArray*>(g->init.get()) != nullptr ||
                dynamic_cast<const ArrayLit*>(g->init.get()) != nullptr ||
                (dynamic_cast<const NewStruct*>(g->init.get()) != nullptr &&
                 (declared.type->kind() == Kind::Struct ||
                  declared.type->kind() == Kind::Class))) {
                // A struct value folds only into a struct or class
                // global: an interface global holds a pair, which a bare
                // value cannot build — that spelling stays refused below
                // rather than accepted here and unbuildable later.
                constGlobal = constAggregateInit(*g->init);
            }
            // `b.inc`: a module-qualified function names its code
            // address, which is a link-time constant — the lowerer
            // materialises the closure pair, so the gate only
            // recognises the spelling. Anything else falls through
            // to fail loudly below.
            bool funcGlobal = false;
            if (const auto* field = dynamic_cast<const Field*>(g->init.get())) {
                if (const auto* mod = dynamic_cast<const Ident*>(field->object.get())) {
                    const resolve::Decl* modDecl = resolver_.lookup(moduleScope_, mod->name);
                    if (modDecl != nullptr && modDecl->kind == resolve::DeclKind::Module) {
                        const resolve::Scope* home = resolver_.scopeOf(modDecl->origin);
                        const resolve::Decl* member = home != nullptr ? resolver_.lookup(home, field->name) : nullptr;
                        if (member != nullptr && member->kind == resolve::DeclKind::Func && member->typeParams == 0) {
                            funcGlobal = true;
                        }
                    }
                }
            }
            // `&dbl`: a same-module function names its code address,
            // which is a link-time constant — the lowerer emits the
            // address word, so the gate only recognises the spelling.
            // Anything else falls through to fail loudly below.
            if (!funcGlobal) {
                if (const auto* addr = dynamic_cast<const Unary*>(g->init.get())) {
                    if (addr->op == UnOp::Addr) {
                        if (const auto* ident = dynamic_cast<const Ident*>(addr->operand.get())) {
                            const resolve::Decl* decl = resolver_.lookup(moduleScope_, ident->name);
                            if (decl != nullptr && decl->kind == resolve::DeclKind::Func && decl->typeParams == 0) {
                                funcGlobal = true;
                            }
                        }
                    }
                }
            }
            // A bare `dbl` names the same link-time code as `&dbl` for a
            // closure global: the value is the forwarder pair either way.
            if (!funcGlobal) {
                if (const auto* ident = dynamic_cast<const Ident*>(g->init.get())) {
                    const resolve::Decl* decl = resolver_.lookup(moduleScope_, ident->name);
                    if (decl != nullptr && decl->kind == resolve::DeclKind::Func && decl->typeParams == 0) {
                        funcGlobal = true;
                    }
                }
            }
            // `&add as *void`: a cast of a function address to a void
            // pointer is the same link-time constant. Anything else
            // falls through to fail loudly below.
            if (!funcGlobal) {
                if (const auto* cast = dynamic_cast<const Cast*>(g->init.get())) {
                    if (const auto* addr = dynamic_cast<const Unary*>(cast->expr.get())) {
                        if (addr->op == UnOp::Addr) {
                            if (const auto* ident = dynamic_cast<const Ident*>(addr->operand.get())) {
                                const resolve::Decl* decl = resolver_.lookup(moduleScope_, ident->name);
                                TypeRef target = resolver_.resolveType(cast->target.get(), resolver_.scopeOf(module_));
                                if (decl != nullptr && decl->kind == resolve::DeclKind::Func && decl->typeParams == 0 &&
                                    !target.isError() && target.type->kind() == Kind::Pointer &&
                                    target.type->pointee() == ctx_.void_() &&
                                    declared.type->kind() == Kind::Pointer &&
                                    declared.type->pointee() == ctx_.void_()) {
                                    funcGlobal = true;
                                }
                            }
                        }
                    }
                }
            }
            // `nilPtr as I`: the null interface is sixteen zero bytes in
            // the image — a null data word beside a null vtable word — so
            // it is constant the way a null pointer is. The target
            // spelling must name an interface, and the declaration must be
            // one: anything else falls through to the normal path, which
            // fails it loudly. The normal checking below still enforces
            // the types, so the gate only recognises the all-bytes-known
            // shape.
            if (const auto* nilCast = dynamic_cast<const Cast*>(g->init.get())) {
                TypeRef spelled = resolver_.resolveType(nilCast->target.get(),
                                                        resolver_.scopeOf(module_));
                if (!spelled.isError() &&
                    spelled.type->kind() == Kind::Interface &&
                    declared.type->kind() == Kind::Interface &&
                    dynamic_cast<const NilPtr*>(nilCast->expr.get()) != nullptr) {
                    constGlobal = true;
                }
            }
            // A float literal is a constant of its own kind: evalConstInt
            // answers integers, so only a literal in a float-typed global
            // gets past without a folded integer. A float expression over
            // constants folds the same way (integer subtrees first, so
            // `5 / 2` is 2, then 2.0): the image then matches the local
            // path bit for bit. Anything else is not a constant
            // initialiser.
            const auto* asFloat = dynamic_cast<const FloatLit*>(g->init.get());
            const bool floatGlobal =
                declared.type->kind() == Kind::Basic &&
                basicIsFloat(*declared.type->basic());
            FloatResult floatFolded{false, 0.0, ""};
            if (floatGlobal && asFloat == nullptr) {
                floatFolded = evalConstFloat(*g->init, *env_, active);
            }
            // A string literal is bytes in the image: the lowerer already
            // owns that case (rodata plus a two-word global), so the gate
            // only has to not call it non-constant.
            const auto* asString = dynamic_cast<const StringLit*>(g->init.get());
            const bool stringGlobal = declared.type->kind() == Kind::String;
            // An enumerator spelling is four known bytes in the image: the
            // lowerer folds every spelling the expression checker accepts
            // (bare, qualified, alias, module-qualified), so the gate only
            // admits the shapes and the checking below validates them.
            // Anything else constructed is not constant.
            bool enumGlobal = false;
            if (!constGlobal && declared.type->kind() == Kind::Enum &&
                (dynamic_cast<const Ident*>(g->init.get()) != nullptr ||
                 dynamic_cast<const Field*>(g->init.get()) != nullptr)) {
                enumGlobal = true;
            }
            // A wide literal is known image bytes (units plus the NUL unit)
            // or a rodata address: the lowerer folds both spellings, so
            // the gate only admits the shape and the checking below
            // validates the fit.
            bool wideGlobal = false;
            if (!constGlobal &&
                dynamic_cast<const WideLit*>(g->init.get()) != nullptr &&
                (declared.type->kind() == Kind::Array ||
                 declared.type->kind() == Kind::Pointer)) {
                wideGlobal = true;
            }
            if (!constGlobal && !funcGlobal && !enumGlobal && !wideGlobal &&
                (asFloat == nullptr || !floatGlobal) &&
                !(floatGlobal && floatFolded.ok) &&
                (asString == nullptr || !stringGlobal)) {
                const std::string why =
                    (floatGlobal && asFloat == nullptr) ? floatFolded.error
                                                        : folded.error;
                fail(g->init->pos, "a global initialiser must be constant: " + why);
                continue;
            }
        }
        TypeRef init = checkExpr(g->init.get(), declared);
        if (!checkCompatible(g->init->pos, g->init.get(), init, declared)) {
            continue;
        }
        // A folded integer names image bytes: when it does not fit the
        // declared width the program is provably overflowing, which rejects
        // — unless wrap defines the value, in which case the lowerer keeps
        // the low bits (SEMANTICS §2.3, FEATURES §11.5). Literals never
        // reach here: their own range check fails first, above.
        if (folded.ok && declared.type->kind() == Kind::Basic) {
            const std::optional<BasicKind> basic = declared.type->basic();
            if (basic.has_value() && basicIsInteger(*basic) &&
                basicWidthBits(*basic) < basicWidthBits(BasicKind::I64) &&
                dynamic_cast<const IntLit*>(g->init.get()) == nullptr) {
                const int width = basicWidthBits(*basic);
                const bool isSigned = basicIsSigned(*basic);
                __int128 lo = isSigned ? -(static_cast<__int128>(1) << (width - 1)) : 0;
                __int128 hi = isSigned ? ((static_cast<__int128>(1) << (width - 1)) - 1)
                                       : ((static_cast<__int128>(1) << width) - 1);
                if (folded.value < lo || folded.value > hi) {
                    if (!n.wrap) {
                        fail(g->init->pos, "a constant does not fit its type");
                        continue;
                    }
                }
            }
        }
        record(g.get(), declared);
        // A global's declaration proves its storage for later reads.
        // Stores from function bodies update the same entry; functions
        // clear it on entry, so a read sees only its own function's
        // stores.
        globalProv_[g->name] = provOf(g->init.get());
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
        if (isOperatorName(n.name)) {
            fail(n.pos, "an operator cannot be generic");
        }
        return;
    }
    // Globals read only their own function's stores: a callee cannot
    // observe a caller's global writes before they happen, and checking
    // runs callers after callees, so cross-function global flow refuses
    // rather than guessing order.
    globalProv_.clear();
    bool failed = false;
    Signature sig = resolver_.buildSignature(n.params, n.returns.get(), moduleScope_, failed);
    if (failed) {
        return;
    }
    if (n.name == "operator_new" || n.name == "operator_delete") {
        const Type* pv = ctx_.pointer(ctx_.void_());
        if (n.name == "operator_new") {
            const Type* i64 = ctx_.basic(BasicKind::I64);
            if (sig.arity() != 2 ||
                sig.parameters()[0].type != i64 ||
                sig.parameters()[1].type != i64 ||
                sig.returnType() != pv) {
                fail(n.pos,
                     "operator new must be (size: i64, align: i64) -> *void");
                return;
            }
        } else if (sig.arity() != 1 ||
                   sig.parameters()[0].type != pv ||
                   (sig.returnType() != nullptr &&
                    sig.returnType() != ctx_.void_())) {
            fail(n.pos, "operator delete must be (ptr: *void) -> void");
            return;
        }
    }
    if (isOperatorName(n.name) && n.name != "operator_new" &&
        n.name != "operator_delete") {
        if (n.name == "operator_index") {
            fail(n.pos, "operator '[]' must be a member");
            return;
        }
        if (n.name == "operator_call") {
            fail(n.pos, "operator '()' must be a member");
            return;
        }
        if (n.name == "operator_arrow") {
            fail(n.pos, "operator '->' must be a member");
            return;
        }
        if (isAssignmentOperatorName(n.name)) {
            fail(n.pos, "an assignment operator must be a member");
            return;
        }
        const bool unary =
            n.name == "operator_neg" || n.name == "operator_bitnot";
        if (unary && sig.arity() != 1) {
            fail(n.pos, "a unary operator takes exactly one parameter");
            return;
        }
        if (!unary && sig.arity() != 2) {
            fail(n.pos, "a binary operator takes exactly two parameters");
            return;
        }
        const auto nominal = [](const Type* type) {
            Kind kind = type->kind();
            return kind == Kind::Struct || kind == Kind::Class ||
                   kind == Kind::Enum;
        };
        const auto& params = sig.parameters();
        bool anyNominal = false;
        for (const auto& parameter : params) {
            anyNominal = anyNominal || nominal(parameter.type);
        }
        if (!anyNominal) {
            fail(n.pos, "an operator needs a class, struct or enum operand");
            return;
        }
    }
    const Type* ret = sig.returnType() != nullptr ? sig.returnType() : ctx_.void_();
    pushScope();
    const std::vector<Parameter>& params = sig.parameters();
    for (uint64_t i = 0; i < static_cast<uint64_t>(params.size()); ++i) {
        bindLocal(n.params[i].name, TypeRef{params[i].type}, params[i].isMutable,
                  n.params[i].pos);
        // Prescan proofs seed the flow: a proven *void parameter reads
        // proven from its first use, like any store would.
        std::string key = module_ + "." + n.name + "#" +
                          std::to_string(static_cast<unsigned long long>(i));
        Prov proven = paramProv(key);
        if (proven != Prov::Raw) {
            recordFlow(n.params[i].name, proven);
        }
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
    checkNoSelfEmbed(self->id, "struct", n.name, n.fields, "field");
    checkFieldDefaults(self->id, n.fields);
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
    // 4.8: several bases lay out one after another. The first may carry the
    // table; a later headed base would need a second vptr word the layout
    // does not have, so it refuses by name. The same name twice is a
    // duplicate, not a chain.
    std::vector<DeclId> bases = resolver_.classBases(self->id);
    for (auto it = bases.begin(); it != bases.end(); ++it) {
        const resolve::Decl* baseDecl = resolver_.declById(*it);
        std::string baseName =
            baseDecl != nullptr && !baseDecl->name.empty() ? baseDecl->name : "?";
        for (auto jt = bases.begin(); jt != it; ++jt) {
            if (sameDecl(*jt, *it)) {
                fail(n.pos, "base class '" + baseName + "' is named twice");
                return;
            }
        }
        if (it != bases.begin() && !resolver_.virtualNames(*it).empty()) {
            fail(n.pos, "base class '" + baseName +
                            "' carries a table after the first base");
            return;
        }
    }
    checkNoSelfEmbed(self->id, "class", n.name, n.fields, "field");
    checkFieldDefaults(self->id, n.fields);
    checkOverrides(*self, n);
    checkMethods(self->id, Kind::Class, n.methods);
    checkCtor(self->id, Kind::Class, n.init.get(), true);
    checkCtor(self->id, Kind::Class, n.deinit.get(), false);
}

void Checker::visitUnionDecl(UnionDecl& n) {
    const resolve::Decl* self = resolver_.lookup(moduleScope_, n.name);
    if (self == nullptr) {
        fail(n.pos, "checker bug: '" + n.name + "' was never resolved");
        return;
    }
    checkNoSelfEmbed(self->id, "union", n.name, n.members, "member");
    // The default walk: each member's type expression, as before. v2
    // CIDER-BUG-049 defaults have no single member to belong to, so a
    // defaulted union member refuses by name.
    for (auto& m : n.members) {
        if (m.init != nullptr) {
            fail(m.init->pos, "a union member cannot carry a default");
        }
        m.accept(*this);
    }
}

bool Checker::typeEmbedsSelf(const Type* type, DeclId self,
                             std::set<uint64_t>& seen) const {
    if (type == nullptr || type->isError()) {
        return false;
    }
    if (type->kind() == Kind::Array) {
        // A fixed repetition of inline values embeds what it repeats.
        return typeEmbedsSelf(type->element(), self, seen);
    }
    if (type->kind() != Kind::Struct && type->kind() != Kind::Union &&
        type->kind() != Kind::Class) {
        return false;
    }
    std::optional<DeclId> id = type->decl();
    if (!id.has_value()) {
        return false;
    }
    if (sameDecl(*id, self)) {
        return true;
    }
    uint64_t key = (static_cast<uint64_t>(id->module) << 32) | id->index;
    if (!seen.insert(key).second) {
        // Some other cycle, not one through self: finite or not, it is not
        // this declaration's doing.
        return false;
    }
    for (const std::string& field : resolver_.fieldNames(*id)) {
        TypeRef nested = resolver_.fieldType(*id, field);
        if (typeEmbedsSelf(nested.type, self, seen)) {
            return true;
        }
    }
    seen.erase(key);
    return false;
}

void Checker::checkNoSelfEmbed(DeclId self, const std::string& kind,
                               const std::string& declName,
                               const std::vector<FieldDecl>& fields,
                               const std::string& partWord) {
    for (const FieldDecl& field : fields) {
        TypeRef t = resolver_.fieldType(self, field.name);
        if (t.isError()) {
            continue;
        }
        std::set<uint64_t> seen;
        if (typeEmbedsSelf(t.type, self, seen)) {
            fail(field.pos, kind + " '" + declName + "' holds itself by value in " +
                                partWord + " '" + field.name + "'");
        }
    }
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
        std::optional<DeclId> savedClass = currentClass_;
        bool wasInit = inInit_;
        // The constructor walks as a method of its class: `self` names the
        // object and, for init, `super_init` may name the base (4.8.3).
        currentClass_ = id;
        currentSelf_ = TypeRef{ctx_.pointer(obj)};
        inInit_ = isInit;
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
        currentClass_ = savedClass;
        inInit_ = wasInit;
    }
    popScope();
    (void)saved;
    if (isInit && sig != nullptr) {
        checkInitAssign(id, *ctor);
    }
}

bool Checker::initExprOpaque(const Expr* expr) {
    if (expr == nullptr) {
        return false;
    }
    // Anything that can run or reach beyond the statement: calls (a callee
    // holding self could assign), construction that runs init, closures
    // (assigned through only when called, which is itself a call), and
    // anything unlisted (fail-closed). Reads, names and literals are
    // transparent; stores are judged at the statement that owns them.
    if (dynamic_cast<const Call*>(expr) != nullptr ||
        dynamic_cast<const NewClass*>(expr) != nullptr ||
        dynamic_cast<const FuncLit*>(expr) != nullptr) {
        return true;
    }
    if (const auto* un = dynamic_cast<const Unary*>(expr)) {
        return initExprOpaque(un->operand.get());
    }
    if (const auto* field = dynamic_cast<const Field*>(expr)) {
        return initExprOpaque(field->object.get());
    }
    if (const auto* index = dynamic_cast<const Index*>(expr)) {
        return initExprOpaque(index->array.get()) || initExprOpaque(index->index.get());
    }
    if (const auto* slice = dynamic_cast<const SliceExpr*>(expr)) {
        return initExprOpaque(slice->array.get()) || initExprOpaque(slice->lo.get()) ||
               initExprOpaque(slice->hi.get());
    }
    if (const auto* tern = dynamic_cast<const Ternary*>(expr)) {
        return initExprOpaque(tern->cond.get()) || initExprOpaque(tern->then.get()) ||
               initExprOpaque(tern->els.get());
    }
    if (const auto* cast = dynamic_cast<const Cast*>(expr)) {
        return initExprOpaque(cast->expr.get());
    }
    if (const auto* binary = dynamic_cast<const Binary*>(expr)) {
        return initExprOpaque(binary->lhs.get()) || initExprOpaque(binary->rhs.get());
    }
    if (dynamic_cast<const IncrDecr*>(expr) != nullptr) {
        // A nested step bails: only a statement-level `(*self).f++`
        // counts, and anything else relying on one is too tangled to
        // credit without risking a wrongful rejection.
        return true;
    }
    if (dynamic_cast<const Assign*>(expr) != nullptr) {
        // Same for a nested assignment: the statement walker credits
        // only top-level self-field stores.
        return true;
    }
    if (const auto* built = dynamic_cast<const NewStruct*>(expr)) {
        for (const FieldInit& field : built->fields) {
            if (initExprOpaque(field.value.get())) {
                return true;
            }
        }
        return false;
    }
    if (const auto* listed = dynamic_cast<const NewArray*>(expr)) {
        if (initExprOpaque(listed->length.get())) {
            return true;
        }
        for (const std::unique_ptr<Expr>& init : listed->inits) {
            if (initExprOpaque(init.get())) {
                return true;
            }
        }
        return false;
    }
    if (const auto* lit = dynamic_cast<const StringLit*>(expr)) {
        // Interpolated parts evaluate: a call hiding in one bails like
        // any other call.
        for (const auto& part : lit->parts) {
            if (part.isExpr && initExprOpaque(part.expr.get())) {
                return true;
            }
        }
        return false;
    }
    if (dynamic_cast<const Ident*>(expr) != nullptr ||
        dynamic_cast<const IntLit*>(expr) != nullptr ||
        dynamic_cast<const FloatLit*>(expr) != nullptr ||
        dynamic_cast<const CharLit*>(expr) != nullptr ||
        dynamic_cast<const BoolLit*>(expr) != nullptr ||
        dynamic_cast<const NilPtr*>(expr) != nullptr ||
        dynamic_cast<const SizeofType*>(expr) != nullptr) {
        return false;
    }
    return true;
}

bool Checker::isSelfFieldStore(const Expr* target, std::string& field) {
    // `(*self).f`: the only store the analysis credits. checkCtor binds
    // `self` after the parameters, so the name always means the object
    // under construction here.
    const auto* into = dynamic_cast<const Field*>(target);
    if (into == nullptr) {
        return false;
    }
    const auto* deref = dynamic_cast<const Unary*>(into->object.get());
    if (deref == nullptr || deref->op != UnOp::Deref) {
        return false;
    }
    const auto* self = dynamic_cast<const Ident*>(deref->operand.get());
    if (self == nullptr || self->name != "self") {
        return false;
    }
    field = into->name;
    return true;
}

void Checker::requireInitAssigned(Position pos, const std::vector<std::string>& fields,
                                  const InitAssign& state) {
    // Every own field carries a default, so there is nothing the init body
    // must prove: unknown flow only matters when a field still needs a
    // store the analysis cannot see (CIDER-BUG-049 fields are exempt).
    if (fields.empty()) {
        return;
    }
    if (state.unknownFlow) {
        fail(pos, "init cannot be proven to assign every field");
        return;
    }
    if (state.bailed) {
        return;
    }
    for (const std::string& field : fields) {
        if (state.assigned.find(field) == state.assigned.end()) {
            fail(pos, "init does not assign '" + field + "'");
            return;
        }
    }
}

Checker::InitAssign Checker::assignedThrough(const Stmt* stmt, InitAssign state,
                                             const std::vector<std::string>& fields) {
    if (stmt == nullptr) {
        return state;
    }
    if (const auto* block = dynamic_cast<const Block*>(stmt)) {
        for (const std::unique_ptr<Stmt>& s : block->stmts) {
            state = assignedThrough(s.get(), state, fields);
        }
        return state;
    }
    if (const auto* expr = dynamic_cast<const ExprStmt*>(stmt)) {
        if (const auto* assign = dynamic_cast<const Assign*>(expr->expr.get())) {
            std::string field;
            if (isSelfFieldStore(assign->target.get(), field)) {
                state.assigned.insert(field);
                if (initExprOpaque(assign->value.get())) {
                    state.bailed = true;
                }
                return state;
            }
            state.bailed = true;
            state.unknownFlow = true;
            return state;
        }
        if (const auto* step = dynamic_cast<const IncrDecr*>(expr->expr.get())) {
            std::string field;
            if (isSelfFieldStore(step->operand.get(), field)) {
                state.assigned.insert(field);
                return state;
            }
            state.bailed = true;
            state.unknownFlow = true;
            return state;
        }
        if (dynamic_cast<const SuperInit*>(expr->expr.get()) != nullptr) {
            // The base constructor runs against the base's fields and
            // cannot name this class's own fields, so coverage is
            // unaffected and the call is not opaque (Standard 4.8.3).
            return state;
        }
        if (initExprOpaque(expr->expr.get())) {
            state.bailed = true;
            state.unknownFlow = true;
        }
        return state;
    }
    if (const auto* decl = dynamic_cast<const VarDecl*>(stmt)) {
        if (decl->init != nullptr && initExprOpaque(decl->init.get())) {
            state.bailed = true;
        }
        return state;
    }
    if (const auto* iff = dynamic_cast<const IfStmt*>(stmt)) {
        if (initExprOpaque(iff->cond.get())) {
            state.bailed = true;
        }
        InitAssign thenState = assignedThrough(iff->then.get(), state, fields);
        if (iff->els == nullptr) {
            // No else: the skipped path assigns nothing, so only the
            // entry set survives — but anything the arm ran still bails.
            thenState.assigned = state.assigned;
            return thenState;
        }
        InitAssign elseState = assignedThrough(iff->els.get(), state, fields);
        InitAssign joined;
        joined.bailed = thenState.bailed || elseState.bailed;
        joined.unknownFlow = thenState.unknownFlow || elseState.unknownFlow;
        for (const std::string& field : thenState.assigned) {
            if (elseState.assigned.find(field) != elseState.assigned.end()) {
                joined.assigned.insert(field);
            }
        }
        return joined;
    }
    if (const auto* loop = dynamic_cast<const WhileStmt*>(stmt)) {
        if (initExprOpaque(loop->cond.get())) {
            state.bailed = true;
        }
        InitAssign body = assignedThrough(loop->body.get(), state, fields);
        // A loop may run zero times, so its adds never stand — but its
        // returns were already checked and its effects still bail.
        state.bailed = body.bailed;
        state.unknownFlow = state.unknownFlow || body.unknownFlow;
        return state;
    }
    if (const auto* loop = dynamic_cast<const ForStmt*>(stmt)) {
        InitAssign head = state;
        if (loop->init != nullptr) {
            head = assignedThrough(loop->init.get(), head, fields);
        }
        if (loop->cond != nullptr && initExprOpaque(loop->cond.get())) {
            head.bailed = true;
        }
        if (loop->step != nullptr && initExprOpaque(loop->step.get())) {
            head.bailed = true;
        }
        InitAssign body = assignedThrough(loop->body.get(), head, fields);
        head.bailed = body.bailed;
        head.unknownFlow = head.unknownFlow || body.unknownFlow;
        return head;
    }
    if (const auto* loop = dynamic_cast<const ForInStmt*>(stmt)) {
        if (initExprOpaque(loop->seq.get())) {
            state.bailed = true;
        }
        InitAssign body = assignedThrough(loop->body.get(), state, fields);
        state.bailed = body.bailed;
        state.unknownFlow = state.unknownFlow || body.unknownFlow;
        return state;
    }
    if (const auto* match = dynamic_cast<const MatchStmt*>(stmt)) {
        if (initExprOpaque(match->scrutinee.get())) {
            state.bailed = true;
        }
        bool first = true;
        InitAssign joined = state;
        for (const std::unique_ptr<MatchArm>& arm : match->arms) {
            InitAssign one = assignedThrough(arm->body.get(), state, fields);
            if (first) {
                joined = one;
                first = false;
            } else {
                InitAssign narrower;
                narrower.bailed = joined.bailed || one.bailed;
                narrower.unknownFlow = joined.unknownFlow || one.unknownFlow;
                for (const std::string& field : joined.assigned) {
                    if (one.assigned.find(field) != one.assigned.end()) {
                        narrower.assigned.insert(field);
                    }
                }
                joined = narrower;
            }
        }
        if (match->fallback != nullptr) {
            InitAssign last = assignedThrough(match->fallback.get(), state, fields);
            InitAssign narrower;
            narrower.bailed = joined.bailed || last.bailed;
            narrower.unknownFlow = joined.unknownFlow || last.unknownFlow;
            for (const std::string& field : joined.assigned) {
                if (last.assigned.find(field) != last.assigned.end()) {
                    narrower.assigned.insert(field);
                }
            }
            joined = narrower;
        }
        return joined;
    }
    if (const auto* ret = dynamic_cast<const Return*>(stmt)) {
        // A path reaching a return with a field unassigned is rejected
        // naming the field (TYPES.md §9.2); the set itself is unchanged.
        requireInitAssigned(ret->pos, fields, state);
        return state;
    }
    if (dynamic_cast<const Break*>(stmt) != nullptr ||
        dynamic_cast<const Continue*>(stmt) != nullptr) {
        return state;
    }
    // Anything else (delete, asm, throw, try, defer, unknown) bails: it
    // may reach beyond the statement in ways syntax cannot show.
    state.bailed = true;
    state.unknownFlow = true;
    return state;
}

void Checker::checkFieldDefaults(DeclId id, const std::vector<FieldDecl>& fields) {
    for (const FieldDecl& field : fields) {
        if (field.init == nullptr) {
            continue;
        }
        TypeRef want = resolver_.fieldType(id, field.name);
        if (want.isError()) {
            continue;
        }
        TypeRef got = checkExpr(field.init.get(), want);
        checkCompatible(field.init->pos, field.init.get(), got, want);
    }
}

void Checker::checkInitAssign(DeclId id, const FuncDecl& ctor) {
    // TYPES.md §9.2: a path through init that reaches the end, or reaches
    // a return, having left a field unassigned is a compile-time rejection
    // naming the field. Runs over the body independent of the checking
    // walk, so check and lowering modes share one analysis.
    std::vector<std::string> fields;
    for (const std::string& name : resolver_.fieldNames(id)) {
        // v2 CIDER-BUG-049: a defaulted field is exempt from the proof:
        // its default runs before the constructor body either way.
        if (resolver_.fieldDefault(id, name) == nullptr) {
            fields.push_back(name);
        }
    }
    if (ctor.body == nullptr) {
        return;
    }
    InitAssign state;
    state = assignedThrough(ctor.body.get(), state, fields);
    requireInitAssigned(ctor.body->pos, fields, state);
}

void Checker::checkOverrides(const resolve::Decl& self, const ClassDecl& n) {
    for (const MethodDecl& method : n.methods) {
        const Signature* own = resolver_.methodSig(self.id, method.name);
        bool sawBaseVirtual = false;
        for (DeclId baseId : resolver_.classBases(self.id)) {
            const Signature* baseSig = resolver_.methodSig(baseId, method.name);
            if (baseSig == nullptr) {
                continue;
            }
            if (resolver_.virtualIndexOf(baseId, method.name) < 0) {
                fail(method.pos, "a method with a base signature that is not " +
                                     std::string("virtual is neither an override nor a shadow: '") +
                                     method.name + "'");
                continue;
            }
            sawBaseVirtual = true;
            if (own != nullptr && !sameSignature(*own, *baseSig)) {
                fail(method.pos, "'" + method.name +
                                     "' overrides with an incompatible signature");
            }
        }
        if (method.isOverride && !sawBaseVirtual) {
            fail(method.pos, "'" + method.name +
                                 "' overrides but no base declares a virtual of that name");
        }
    }
}

void Checker::checkMethods(DeclId id, Kind kind, std::vector<MethodDecl>& methods) {
    const Type* obj = ctx_.nominal(kind, id);
    const Type* self = ctx_.pointer(obj);
    // One label per method name: v3 mangles a method as `<type>__<name>`
    // with no overload suffix, so a second declaration of the same name
    // (plain and virtual, or two signatures) would collide at codegen.
    std::unordered_set<std::string> seen;
    for (MethodDecl& method : methods) {
        if (!seen.insert(method.name).second) {
            fail(method.pos, "duplicate method '" + method.name + "'");
            continue;
        }
        if (method.name == "operator_new" ||
            method.name == "operator_delete") {
            fail(method.pos, "allocation operators must be global functions");
            continue;
        }
        globalProv_.clear();
        const Signature* sig = resolver_.methodSig(id, method.name);
        if (sig == nullptr) {
            fail(method.pos, "checker bug: signature missing");
            continue;
        }
        if (method.name == "operator_index" &&
            sig->arity() != kOneParameter) {
            fail(method.pos, "an index operator takes exactly one parameter");
            continue;
        }
        if (method.name == "operator_arrow") {
            if (sig->arity() != kNoParameters) {
                fail(method.pos, "an arrow operator takes no parameters");
                continue;
            }
            const Type* answer = sig->returnType();
            if (answer == nullptr || answer->kind() != Kind::Pointer ||
                answer->pointee() == nullptr) {
                fail(method.pos, "an arrow operator must return a pointer");
                continue;
            }
        }
        const bool unaryOperator =
            method.name == "operator_neg" || method.name == "operator_bitnot";
        if (unaryOperator && sig->arity() != kNoParameters) {
            fail(method.pos,
                 "a unary operator takes no parameters in a member declaration");
            continue;
        }
        if (isOperatorName(method.name) && !unaryOperator &&
            method.name != "operator_index" &&
            method.name != "operator_call" &&
            method.name != "operator_arrow" &&
            sig->arity() != kOneParameter) {
            fail(method.pos,
                 "a binary operator takes exactly one parameter in a member declaration");
            continue;
        }
        if (isAssignmentOperatorName(method.name) &&
            sig->returnType() != nullptr &&
            sig->returnType() != ctx_.void_()) {
            fail(method.pos, "an assignment operator returns void");
            continue;
        }
        pushScope();
        bindLocal("self", TypeRef{self}, false, method.pos);
        const std::vector<Parameter>& declared = sig->parameters();
        for (uint64_t i = 0; i < static_cast<uint64_t>(declared.size()); ++i) {
            bindLocal(method.params[i].name, TypeRef{declared[i].type},
                      declared[i].isMutable, method.params[i].pos);
            noteVolatilePointer(method.params[i].name,
                                method.params[i].type.get());
        }
        const Type* ret = sig->returnType() != nullptr ? sig->returnType() : ctx_.void_();
        TypeRef saved = funcReturn_;
        bool was = inFunc_;
        std::optional<DeclId> savedClass = currentClass_;
        TypeRef savedSelf = currentSelf_;
        funcReturn_ = TypeRef{ret};
        inFunc_ = true;
        currentClass_ = id;
        currentSelf_ = TypeRef{self};
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
        currentClass_ = savedClass;
        currentSelf_ = savedSelf;
        popScope();
    }
}

void Checker::visitVarDecl(VarDecl& n) {
    TypeRef declared = resolveHere(n.type.get());
    if (declared.isError()) {
        return;
    }
    if (dynamic_cast<UninitExpr*>(n.init.get()) != nullptr) {
        // Unified Standard 3.2: the storage exists, nothing initializes it.
        record(n.init.get(), declared);
        bindLocal(n.name, declared, true, n.pos);
        noteVolatilePointer(n.name, n.type.get());
        record(&n, declared);
        recordFlow(n.name, Prov::Raw);
        if (sink_ != nullptr) {
            sink_->onVarDecl(n);
        }
        return;
    }
    TypeRef init = checkExpr(n.init.get(), declared);
    if (!checkCompatible(n.init->pos, n.init.get(), init, declared)) {
        return;
    }
    bindLocal(n.name, declared, true, n.pos);
    noteVolatilePointer(n.name, n.type.get());
    record(&n, declared);
    // A declaration proves its storage: later reads of the name answer
    // what this initializer carried.
    recordFlow(n.name, provOf(n.init.get()));
    if (sink_ != nullptr) {
        sink_->onVarDecl(n);
    }
}

void Checker::visitBlock(Block& n) {
    const resolve::Scope* savedTypeScope = typeScope_;
    if (const resolve::Scope* local = resolver_.localTypeScopeFor(&n)) {
        typeScope_ = local;
    }
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
    typeScope_ = savedTypeScope;
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
            const Expr* savedStep = statementExpr_;
            statementExpr_ = dynamic_cast<IncrDecr*>(n.step.get()) != nullptr
                                 ? n.step.get()
                                 : nullptr;
            checkExpr(n.step.get(), TypeRef{ctx_.wildcard()});
            statementExpr_ = savedStep;
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
        const Expr* savedStep = statementExpr_;
        statementExpr_ = dynamic_cast<IncrDecr*>(n.step.get()) != nullptr
                             ? n.step.get()
                             : nullptr;
        checkExpr(n.step.get(), TypeRef{ctx_.wildcard()});
        statementExpr_ = savedStep;
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
                // The iteration protocol is two methods (FEATURES 8.2):
                // iter() answers the iterator value once, next() takes a
                // pointer to that same iterator and answers the next
                // element or nilPtr. One without the other does not
                // iterate, and next through any other pointer is not the
                // protocol.
                const Signature* iter = resolver_.methodSig(*id, "iter");
                const Signature* next = resolver_.methodSig(*id, "next");
                bool shapesOk = false;
                if (iter != nullptr && next != nullptr && iter->arity() == 0 &&
                    next->arity() == 1) {
                    const Type* iterRet = iter->returnType();
                    const Type* nextParam = next->parameters()[0].type;
                    const Type* nextRet = next->returnType();
                    if (iterRet != nullptr && nextParam != nullptr &&
                        nextRet != nullptr &&
                        (iterRet->kind() == Kind::Struct ||
                         iterRet->kind() == Kind::Class ||
                         iterRet->kind() == Kind::Union)) {
                        std::optional<DeclId> iterId = iterRet->decl();
                        const Type* held =
                            nextParam->kind() == Kind::Pointer
                                ? nextParam->pointee()
                                : nullptr;
                        const Type* elem =
                            nextRet->kind() == Kind::Pointer
                                ? nextRet->pointee()
                                : nullptr;
                        if (iterId.has_value() && held != nullptr &&
                            !held->isError() && elem != nullptr &&
                            !elem->isError() &&
                            elem->kind() != Kind::Void) {
                            std::optional<DeclId> heldId = held->decl();
                            if (heldId.has_value() &&
                                sameDecl(*iterId, *heldId)) {
                                element = TypeRef{elem};
                                shapesOk = true;
                            }
                        }
                    }
                }
                if (!shapesOk) {
                    fail(n.seq->pos, "a for-in type provides iter() and next()");
                }
            }
        } else {
            fail(n.seq->pos, "cannot iterate this type");
        }
    }
    pushScope();
    if (n.annotation != nullptr && !element.isError()) {
        TypeRef annotated = resolveHere(n.annotation.get());
        if (!annotated.isError() && annotated.type != element.type) {
            fail(n.annotation->pos,
                 "the loop variable is " + typeName(element) + ", not " +
                     typeName(annotated));
        }
    }
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
    const Expr* saved = statementExpr_;
    statementExpr_ =
        dynamic_cast<IncrDecr*>(n.expr.get()) != nullptr ? n.expr.get() : nullptr;
    checkExpr(n.expr.get(), TypeRef{ctx_.wildcard()});
    statementExpr_ = saved;
    if (sink_ != nullptr) {
        sink_->onExprStmt(n);
    }
}

void Checker::visitReturn(Return& n) {
    if (!inFunc_) {
        fail(n.pos, "return outside a function");
        return;
    }
    // FEATURES §5.2: a return inside a defer would leave the scope twice
    // and re-run every defer in it, including itself.
    if (inDefer_ > 0) {
        fail(n.pos, "return inside a defer");
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
    // TYPES §4.3: a class value boxes by address, so returning a frame
    // value (a local or parameter name) as an interface would dangle the
    // dead frame. Small structs inline and stay legal; pointers and
    // dereferences keep their owner's address under the usual pointer
    // rules. Class values only spell as names today (bare construction is
    // rejected, dereferences are Deref nodes), so a name here is
    // provably frame-dead: heap-allocate or return the pointer instead.
    if (!value.isError() && !funcReturn_.isError() &&
        funcReturn_.type->kind() == Kind::Interface &&
        value.type->kind() == Kind::Class &&
        dynamic_cast<const Ident*>(n.value.get()) != nullptr) {
        fail(n.value->pos,
             "a class value cannot return as an interface: it would dangle");
    }
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
    // FEATURES §5.2: a break inside a defer would leave the scope twice
    // and re-run every defer in it, including itself.
    if (inDefer_ > 0) {
        fail(n.pos, "break inside a defer");
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
    if (inDefer_ > 0) {
        fail(n.pos, "continue inside a defer");
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

void Checker::visitUninitExpr(UninitExpr& n) {
    // Unified Standard 3.2: `uninit` is a declaration's opt-out, not a
    // value. visitVarDecl lets it through by name; anywhere else it fails.
    fail(n.pos, "uninit is only allowed as a variable's initializer");
    record(&n, TypeRef{ctx_.error()});
}

void Checker::visitSuper(Super& n) {
    if (!inFunc_ || !currentClass_.has_value()) {
        fail(n.pos, "super is only a receiver inside a method");
        record(&n, errorRef());
        return;
    }
    if (resolver_.classBases(*currentClass_).empty()) {
        fail(n.pos, "super needs a base class");
        record(&n, errorRef());
        return;
    }
    if (resolver_.classBases(*currentClass_).begin() + 1 !=
        resolver_.classBases(*currentClass_).end()) {
        // 4.8: `super` names one base; with several there is no single one.
        fail(n.pos, "super needs exactly one base class");
        record(&n, errorRef());
        return;
    }
    record(&n, currentSelf_);
}

void Checker::visitSuperInit(SuperInit& n) {
    // Unified Standard 4.8.3: `super_init(args)` runs the direct base's
    // constructor on this object. It is meaningful only inside an init of
    // a class that has exactly one base with an init.
    if (!inInit_ || !currentClass_.has_value()) {
        fail(n.pos, "super_init is only usable inside an init body");
        record(&n, errorRef());
        return;
    }
    const std::vector<DeclId> bases = resolver_.classBases(*currentClass_);
    if (bases.empty()) {
        fail(n.pos, "this class has no base class for super_init");
        record(&n, errorRef());
        return;
    }
    if (bases.begin() + 1 != bases.end()) {
        fail(n.pos, "super_init is not lowered for several base classes");
        record(&n, errorRef());
        return;
    }
    const Signature* init = resolver_.methodSig(bases[0], "init");
    if (init == nullptr) {
        fail(n.pos, "the base class has no init");
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
    record(&n, TypeRef{ctx_.void_()});
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
    if (n.op == UnOp::Neg || n.op == UnOp::BitNot) {
        TypeRef overloaded;
        if (checkOperatorOverload(n, operand, overloaded)) {
            record(&n, overloaded);
            return;
        }
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
        // A deref through a `*volatile T` name is a volatile access.
        if (const auto* src = dynamic_cast<const Ident*>(n.operand.get())) {
            for (auto frame = volatilePtrs_.rbegin();
                 frame != volatilePtrs_.rend(); ++frame) {
                if (frame->find(src->name) != frame->end()) {
                    volatilePlaces_.insert(&n);
                    break;
                }
            }
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
        if (const auto* field = dynamic_cast<const Field*>(n.operand.get())) {
            // `&lib.areaOf`: the qualifier names a module, so the member is
            // a plain function of that module. Its address is the same bare
            // code address a local `&name` answers, not a pointer around
            // the closure spelling a field read would give.
            const auto* qualifier = dynamic_cast<const Ident*>(field->object.get());
            if (qualifier != nullptr && findLocal(qualifier->name) == nullptr) {
                const resolve::Decl* mod =
                    resolver_.lookup(moduleScope_, qualifier->name);
                if (mod != nullptr && mod->kind == resolve::DeclKind::Module) {
                    const resolve::Scope* home = resolver_.scopeOf(mod->origin);
                    const resolve::Decl* member =
                        home != nullptr ? resolver_.lookup(home, field->name) : nullptr;
                    if (member != nullptr && member->kind == resolve::DeclKind::Func) {
                        if (member->typeParams > 0) {
                            fail(n.pos, "a generic function needs explicit arguments");
                            record(&n, errorRef());
                            return;
                        }
                        Signature sig = member->sig;
                        record(&n, TypeRef{ctx_.functionPtr(std::move(sig))});
                        return;
                    }
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
    if (n.raw) {
        // `@bitcast(T, x)`: bits move unchanged, never a numeric
        // conversion; both sides are basics of one width (13.5).
        std::optional<BasicKind> bf =
            from.type->kind() == Kind::Basic ? from.type->basic() : std::nullopt;
        std::optional<BasicKind> bt =
            target.type->kind() == Kind::Basic ? target.type->basic() : std::nullopt;
        if (!bf.has_value() || !bt.has_value() ||
            basicWidthBits(*bf) != basicWidthBits(*bt)) {
            fail(n.pos, "@bitcast needs equal-width scalar types");
            record(&n, errorRef());
            return;
        }
        record(&n, target);
        return;
    }
    if (dynamic_cast<const NilPtr*>(n.expr.get()) != nullptr &&
        target.type->kind() == Kind::String) {
        // A null names no type, so it converts to a string the same way
        // it converts to any pointer: the null string is (nil, 0), which
        // reads empty and compares equal to another null without ever
        // dereferencing. Only the literal converts — a *void variable
        // beside a string stays a mismatch the source must spell.
        record(&n, target);
        return;
    }
    if (dynamic_cast<const NilPtr*>(n.expr.get()) != nullptr &&
        target.type->kind() == Kind::Interface) {
        // A null names no interface either: the null interface is
        // (nil, nil), whose vtable word answers every null check without
        // ever dispatching. Only the literal converts — a *void variable
        // beside an interface stays a mismatch the source must spell.
        record(&n, target);
        return;
    }
    // Closure round trips gate on provenance before the shape table: a
    // *void (or deeper carrier) holding a proven closure address casts to
    // a closure spelling or a fn spelling, and nilPtr casts to the null
    // closure value. Anything else holding the address refuses — reading
    // a pair through it would be a guess. A closure casts to *void by
    // address (checked below by shape).
    auto isVoidCarrier = [](const TypeRef& t) -> bool {
        if (t.type->kind() == Kind::Pointer && t.type->pointee() != nullptr) {
            Kind inner = t.type->pointee()->kind();
            return inner == Kind::Void || inner == Kind::Pointer;
        }
        return false;
    };
    // A byte view carries the same address a void carrier does: only a
    // proven closure address answers as a pair through it.
    auto isByteView = [](const TypeRef& t) -> bool {
        if (t.type->kind() != Kind::Pointer || t.type->pointee() == nullptr) {
            return false;
        }
        const Type* inner = t.type->pointee();
        return inner->kind() == Kind::Basic && inner->basic().has_value() &&
               *inner->basic() == BasicKind::U8;
    };
    bool toClosureSpelling =
        target.type->kind() == Kind::Closure ||
        target.type->kind() == Kind::FunctionPtr;
    // A closure value casts onward by shape below; a fn spelling holding
    // a proven closure still gates (it adopts the representation here).
    bool fromClosureValue =
        !from.isError() && from.type->kind() == Kind::Closure;
    if (toClosureSpelling && !fromClosureValue && !from.isError()) {
        if (dynamic_cast<const NilPtr*>(n.expr.get()) != nullptr) {
            record(&n, target);
            return;
        }
        // One function pointer casts to another by its single code word: the
        // row check below owns that pair, and 8.4 blesses an explicit cast
        // across conventions. Adopting the pair (a closure target) still
        // needs proven provenance, and so does any cast out of a void
        // carrier or byte view.
        const bool pointerToPointer =
            from.type->kind() == Kind::FunctionPtr &&
            target.type->kind() == Kind::FunctionPtr;
        // A fn spelling adopts here too: only a proven closure address
        // answers as a pair, never a raw code pointer.
        if (!pointerToPointer &&
            (isVoidCarrier(from) || isByteView(from) ||
             from.type->kind() == Kind::FunctionPtr)) {
            Prov proven = provOf(n.expr.get());
            if (proven != Prov::Closure && proven != Prov::Neutral) {
                fail(n.pos, "this *void is not proven to hold a closure");
                record(&n, errorRef());
                return;
            }
            record(&n, target);
            return;
        }
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
    if (base.isError()) {
        record(&n, errorRef());
        return;
    }
    Kind kind = base.type->kind();
    if (kind == Kind::Struct || kind == Kind::Class) {
        // `[]` is member-only: the receiver's own type supplies the method,
        // and the index must match its parameter exactly. A struct or class
        // has no built-in indexing rule to fall back to.
        std::optional<DeclId> id = base.type->decl();
        const Signature* sig =
            id.has_value() ? resolver_.methodSig(*id, "operator_index") : nullptr;
        if (sig == nullptr || sig->arity() != kOneParameter) {
            fail(n.pos, "no matching operator '[]'");
            record(&n, errorRef());
            return;
        }
        TypeRef want{sig->parameters()[0].type};
        TypeRef index = checkExpr(n.index.get(), want);
        if (index.isError()) {
            record(&n, errorRef());
            return;
        }
        if (index != want) {
            fail(n.pos, "no matching operator '[]'");
            record(&n, errorRef());
            return;
        }
        operatorIndexes_.insert(&n);
        const Type* ret =
            sig->returnType() != nullptr ? sig->returnType() : ctx_.void_();
        record(&n, TypeRef{ret});
        return;
    }
    TypeRef index =
        checkExpr(n.index.get(), TypeRef{ctx_.basic(BasicKind::I64)});
    if (index.isError()) {
        record(&n, errorRef());
        return;
    }
    if (index.type->kind() != Kind::Basic || !basicIsInteger(*index.type->basic())) {
        fail(n.index->pos, "an index is an integer");
        record(&n, errorRef());
        return;
    }
    if (kind == Kind::Array || kind == Kind::Slice) {
        record(&n, TypeRef{base.type->element()});
        return;
    }
    if (kind == Kind::String) {
        // 2.4: a string's element is a char, so `&s[0]` is the *char a C
        // callee takes. It is still one byte; only the interpretation differs
        // from a byte buffer's u8.
        record(&n, TypeRef{ctx_.basic(BasicKind::Char)});
        return;
    }
    if (kind == Kind::Pointer) {
        // `p[i]` is `*(p + i)` (SEMANTICS §8.2): the element of a pointer
        // with a size. A void pointee has no element, so no *void answer.
        if (base.type->pointee() == ctx_.void_()) {
            fail(n.pos, "cannot index this type");
            record(&n, errorRef());
            return;
        }
        record(&n, TypeRef{base.type->pointee()});
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
    std::optional<DeclId> initHome = resolver_.methodHome(*id, "init");
    if (!initHome.has_value() || !sameDecl(*initHome, *id)) {
        fail(n.pos, "this class has no init: create it with new C { ... }");
        record(&n, errorRef());
        return;
    }
    const Signature* init = resolver_.methodSig(*id, "init");
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
    TypeRef ptr{ctx_.pointer(target.type)};
    if (n.tryNew) {
        if (!requireAllocOperator(true, n.pos)) {
            record(&n, errorRef());
            return;
        }
        record(&n, checkResultOf(ptr, n.pos));
        return;
    }
    if (!requireAllocOperator(true, n.pos)) {
        record(&n, errorRef());
        return;
    }
    record(&n, ptr);
}

void Checker::visitNewArray(NewArray& n) {
    record(&n, checkNewArray(n));
}

void Checker::visitArrayLit(ArrayLit& n) {
    // The real check reaches ArrayLit through checkExpr with the declared
    // type (visitVarDecl's init path); a bare walk has nothing to answer.
    (void)n;
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
        if (isOperatorIndex(index)) {
            // Assignment through a member operator[]: the operator is called
            // once and must answer a pointer to the stored element. A
            // non-pointer result is a check error, never a fallback to
            // built-in indexing. The value matches the pointee exactly for
            // `=`; a compound form uses the built-in compound rule on that
            // pointee type. The store itself goes through the pointer, so a
            // nil answer behaves like any other store through nil.
            std::optional<DeclId> id = base.isError() ? std::nullopt : base.type->decl();
            const Signature* sig =
                id.has_value() ? resolver_.methodSig(*id, "operator_index") : nullptr;
            const Type* ret = sig != nullptr ? sig->returnType() : nullptr;
            if (ret == nullptr || ret->kind() != Kind::Pointer ||
                ret->pointee() == nullptr) {
                fail(n.target->pos,
                     "assignment through operator '[]' needs a pointer result");
                record(&n, errorRef());
                return;
            }
            TypeRef element{ret->pointee()};
            if (element.type->kind() == Kind::Void) {
                fail(n.target->pos,
                     "cannot assign through operator '[]' returning *void");
                record(&n, errorRef());
                return;
            }
            if (!n.op.has_value()) {
                TypeRef value = checkExpr(n.value.get(), element);
                if (value.isError()) {
                    record(&n, errorRef());
                    return;
                }
                if (value != element) {
                    fail(n.value->pos,
                         "assignment through operator '[]' needs " +
                             typeName(element) + ", got " + typeName(value));
                    record(&n, errorRef());
                    return;
                }
                operatorIndexAssigns_.insert(&n);
                record(&n, element);
                return;
            }
            TypeRef value = checkExpr(n.value.get(), TypeRef{ctx_.wildcard()});
            if (value.isError()) {
                record(&n, errorRef());
                return;
            }
            // The same built-in compound shapes the ordinary target rule
            // accepts: pointer += integer scales, everything else combines
            // to a common numeric type and converts back to the pointee.
            if (element.type->kind() == Kind::Pointer &&
                element.type->pointee() != ctx_.void_() && isInteger(value) &&
                (n.op == BinOp::Add || n.op == BinOp::Sub)) {
                operatorIndexAssigns_.insert(&n);
                record(&n, element);
                return;
            }
            TypeRef combined = commonType(element, value, n.pos);
            if (combined.isError()) {
                record(&n, errorRef());
                return;
            }
            checkCompatible(n.value->pos, n.value.get(), combined, element);
            operatorIndexAssigns_.insert(&n);
            record(&n, element);
            return;
        }
    }
    if (target.type->kind() == Kind::Struct || target.type->kind() == Kind::Class) {
        // `a = b` and `a += b` on a nominal value use the matching member
        // assignment operator when one is declared. The declaration owns
        // the store; built-in aggregate assignment remains the fallback for
        // types that do not declare the operator.
        const char* opName = n.op.has_value()
                                 ? compoundAssignOperatorName(*n.op)
                                 : "operator_assign";
        if (opName != nullptr) {
            std::optional<DeclId> id = target.type->decl();
            const Signature* assignSig =
                id.has_value() ? resolver_.methodSig(*id, opName) : nullptr;
            if (assignSig != nullptr) {
                const char* spelling = n.op.has_value()
                                           ? compoundAssignSpelling(*n.op)
                                           : "=";
                if (assignSig->arity() != kOneParameter) {
                    fail(n.pos, std::string("no matching operator '") +
                                    spelling + "'");
                    record(&n, errorRef());
                    return;
                }
                TypeRef want{assignSig->parameters()[0].type};
                TypeRef value = checkExpr(n.value.get(), want);
                if (value.isError()) {
                    record(&n, errorRef());
                    return;
                }
                if (value != want) {
                    fail(n.value->pos, std::string("no matching operator '") +
                                           spelling + "'");
                    record(&n, errorRef());
                    return;
                }
                operatorAssigns_.insert(&n);
                record(&n, TypeRef{ctx_.void_()});
                return;
            }
        }
    }
    if (!n.op.has_value()) {
        TypeRef value = checkExpr(n.value.get(), target);
        checkCompatible(n.value->pos, n.value.get(), value, target);
        // A store proves its target: later reads answer what this value
        // carried. Bare names record in their scope (globals persistently);
        // other chains record by place key. Anything unkeyable stays Raw.
        if (const auto* ident = dynamic_cast<const Ident*>(n.target.get())) {
            Prov proven = provOf(n.value.get());
            if (findLocal(ident->name) != nullptr) {
                recordFlow(ident->name, proven);
            } else {
                globalProv_[ident->name] = proven;
            }
        } else {
            std::string key = placeKey(n.target.get());
            if (!key.empty() && !placeFlow_.empty()) {
                placeFlow_.back()[key] = provOf(n.value.get());
            }
        }
        record(&n, target);
        return;
    }
    TypeRef value = checkExpr(n.value.get(), TypeRef{ctx_.wildcard()});
    if (value.isError()) {
        record(&n, errorRef());
        return;
    }
    // `p += n` is `p = p + n` (FEATURES §1.7): the same shapes the binary
    // rule answers, with the target as the combined type. Anything else a
    // pointer reaches falls below to the common type's loud rejection.
    if (target.type->kind() == Kind::Pointer &&
        target.type->pointee() != ctx_.void_() && isInteger(value) &&
        (n.op == BinOp::Add || n.op == BinOp::Sub)) {
        record(&n, target);
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
    // 7.2: the Go model makes ++/-- statements. A whole statement or a
    // C-style for step is the whole of what the Standard blesses; a value
    // position would hide a store inside a read.
    if (statementExpr_ != &n) {
        fail(n.pos, "increment and decrement are statements, not expressions");
        record(&n, errorRef());
        return;
    }
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
    sizeofOperands_[&n] = target;
    record(&n, TypeRef{ctx_.basic(BasicKind::I64)});
}

void Checker::visitSizeofExpr(SizeofExpr& n) {
    if (n.operand == nullptr) {
        record(&n, errorRef());
        return;
    }
    // Type the operand only. Lowering reads the recorded type and never
    // emits a call, store or trap for the operand itself.
    TypeRef operand = checkExpr(n.operand.get(), TypeRef{});
    if (operand.isError()) {
        record(&n, errorRef());
        return;
    }
    sizeofExprOperands_[&n] = operand;
    record(&n, TypeRef{ctx_.basic(BasicKind::I64)});
}

void Checker::visitAlignofType(AlignofType& n) {
    TypeRef target = resolveHere(n.type.get());
    if (target.isError()) {
        record(&n, errorRef());
        return;
    }
    alignofOperands_[&n] = target;
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
    if (after != before) {
        funcReturn_ = saved;
        inFunc_ = was;
        popScope();
        return errorRef();
    }
    if (!captures.empty()) {
        fail(captures.front().pos, "captures are not supported");
        funcReturn_ = saved;
        inFunc_ = was;
        popScope();
        return errorRef();
    }
    // The scope and the return type stay in place through delegation: the
    // lowerer's walk re-checks the body deterministically, and a return
    // inside the literal answers the literal's type, not the enclosing
    // function's. Restoring first re-binds inner returns to the outer
    // type and rejects them loudly for the wrong reason.
    if (sink_ != nullptr) {
        emitted_.insert(&lit);
        sink_->onFuncLit(lit, captures, sig);
    }
    funcReturn_ = saved;
    inFunc_ = was;
    popScope();
    return TypeRef{ctx_.closure(std::move(sig))};
}

void Checker::visitMatchStmt(MatchStmt& n) {
    TypeRef scrutinee = checkExpr(n.scrutinee.get(), TypeRef{ctx_.wildcard()});
    if (!scrutinee.isError()) {
        for (auto& arm : n.arms) {
            checkMatchPattern(*arm->pattern, scrutinee);
            for (auto& alt : arm->alts) {
                checkMatchPattern(*alt, scrutinee);
            }
        }
        // Exhaustiveness is decided by the type, at compile time: a
        // default arm, or a finite cover over a bool, a union, or an
        // enum. Anything else is a rejection naming the statement, never
        // a runtime miss.
        bool exhaustive = n.fallback != nullptr;
        if (!exhaustive && scrutinee.type->kind() == Kind::Basic &&
            scrutinee.type->basic().has_value() &&
            *scrutinee.type->basic() == BasicKind::Bool) {
            bool hasTrue = false;
            bool hasFalse = false;
            for (auto& arm : n.arms) {
                // A guard may fail at runtime, so a guarded arm covers
                // nothing: only a guard-free `true` and `false` close
                // the bool.
                if (arm->guard != nullptr) {
                    continue;
                }
                // Every alternative covers on its own: `true | false`
                // in one arm names both cases, as two arms would.
                auto consider = [&](const Pattern& pat) {
                    if (pat.kind != PatternKind::Const) {
                        return;
                    }
                    const auto* flag =
                        dynamic_cast<const BoolLit*>(pat.value.get());
                    if (flag == nullptr) {
                        return;
                    }
                    if (flag->value) {
                        hasTrue = true;
                    } else {
                        hasFalse = true;
                    }
                };
                consider(*arm->pattern);
                for (auto& alt : arm->alts) {
                    consider(*alt);
                }
            }
            exhaustive = hasTrue && hasFalse;
        }
        if (!exhaustive && scrutinee.type->kind() == Kind::Union) {
            std::optional<DeclId> sid = scrutinee.type->decl();
            if (sid.has_value()) {
                // A union with every member listed is exhaustive
                // (FEATURES §8.4): only a live member arm for this same
                // union covers, so a dead arm for another union counts
                // for nothing and a duplicate counts once.
                std::unordered_set<std::string> covered;
                for (auto& arm : n.arms) {
                    // Guards may fail, so a guarded arm covers no member.
                    if (arm->guard != nullptr) {
                        continue;
                    }
                    auto consider = [&](const Pattern& pat) {
                        if (pat.kind != PatternKind::Member || isDeadPattern(&pat)) {
                            return;
                        }
                        std::optional<DeclId> armId = matchMemberUnion(pat);
                        if (armId.has_value() && sameDecl(*sid, *armId)) {
                            covered.insert(pat.member);
                        }
                    };
                    consider(*arm->pattern);
                    for (auto& alt : arm->alts) {
                        consider(*alt);
                    }
                }
                exhaustive = true;
                for (const std::string& name : resolver_.fieldNames(*sid)) {
                    if (covered.count(name) == 0) {
                        exhaustive = false;
                        break;
                    }
                }
            }
        }
        if (!exhaustive && scrutinee.type->kind() == Kind::Enum) {
            std::optional<DeclId> sid = scrutinee.type->decl();
            if (sid.has_value()) {
                // An enumerator with every case listed is exhaustive
                // (FEATURES §8.4): only a live case arm for this same
                // enum covers, so a dead arm for another enum counts
                // for nothing and a duplicate counts once. A raw number
                // never names a case, so only enumerator arms cover.
                std::unordered_set<std::string> covered;
                for (auto& arm : n.arms) {
                    // Guards may fail, so a guarded arm covers no case.
                    if (arm->guard != nullptr) {
                        continue;
                    }
                    auto consider = [&](const Pattern& pat) {
                        if (pat.kind != PatternKind::Member || isDeadPattern(&pat)) {
                            return;
                        }
                        std::optional<DeclId> armId = matchMemberEnum(pat);
                        if (armId.has_value() && sameDecl(*sid, *armId)) {
                            covered.insert(pat.member);
                        }
                    };
                    consider(*arm->pattern);
                    for (auto& alt : arm->alts) {
                        consider(*alt);
                    }
                }
                exhaustive = true;
                for (const auto& member : resolver_.enumMembers(*sid)) {
                    if (covered.count(member.first) == 0) {
                        exhaustive = false;
                        break;
                    }
                }
            }
        }
        if (!exhaustive) {
            fail(n.pos, "match needs a default arm");
        }
    }
    if (sink_ != nullptr) {
        // Fresh bindings stay pushed while the lowerer walks the bodies:
        // onMatch re-accepts each arm synchronously, so these scopes are
        // alive for exactly the delegation (mirrors visitForInStmt, where
        // the element binding outlives the sink call the same way).
        for (auto& arm : n.arms) {
            pushScope();
            bindMatchArm(*arm, scrutinee);
            checkMatchGuard(*arm);
        }
        sink_->onMatch(n);
        for (auto& arm : n.arms) {
            (void)arm;
            popScope();
        }
        return;
    }
    for (auto& arm : n.arms) {
        pushScope();
        bindMatchArm(*arm, scrutinee);
        checkMatchGuard(*arm);
        arm->body->accept(*this);
        popScope();
    }
    if (n.fallback != nullptr) {
        n.fallback->accept(*this);
    }
}

bool Checker::resolveMatchValue(const std::string& name, TypeRef& bound) {
    if (const Local* local = findLocal(name)) {
        bound = local->type;
        return true;
    }
    const resolve::Decl* decl = resolver_.lookup(moduleScope_, name);
    if (decl == nullptr) {
        // A bare enumerator compares by value: exactly one enum in this
        // module may own the name, or the name still binds fresh below.
        // Imported enums keep their qualified spelling in patterns.
        int owners = 0;
        const resolve::Decl* owner =
            resolver_.lookupEnumMember(moduleScope_, name, owners);
        if (owner != nullptr) {
            bound = TypeRef{ctx_.nominal(Kind::Enum, owner->id)};
            return true;
        }
        return false;
    }
    if (decl->kind == resolve::DeclKind::Var ||
        decl->kind == resolve::DeclKind::Const) {
        bound = decl->type;
        return true;
    }
    return false;
}

bool Checker::matchComparable(TypeRef scrutinee, TypeRef bound) {
    if (scrutinee.isError() || bound.isError()) {
        return false;
    }
    Kind sk = scrutinee.type->kind();
    Kind bk = bound.type->kind();
    if (sk == Kind::String && bk == Kind::String) {
        return true;
    }
    if (sk == Kind::Pointer && bk == Kind::Pointer) {
        return true;
    }
    // Enums compare by underlying value: the same enum, or either side
    // an integer (TYPES §2.6). Different enums never compare; the arm
    // dies instead of matching garbage.
    if (sk == Kind::Enum || bk == Kind::Enum) {
        auto idOf = [](const TypeRef& t) -> std::optional<DeclId> {
            return t.type->kind() == Kind::Enum ? t.type->decl() : std::nullopt;
        };
        auto isInt = [](const TypeRef& t) -> bool {
            return t.type->kind() == Kind::Basic &&
                   basicIsInteger(*t.type->basic());
        };
        std::optional<DeclId> s = idOf(scrutinee);
        std::optional<DeclId> b = idOf(bound);
        if (s.has_value() && b.has_value()) {
            return sameDecl(*s, *b);
        }
        return (s.has_value() && isInt(bound)) ||
               (b.has_value() && isInt(scrutinee));
    }
    if (sk == Kind::Basic && bk == Kind::Basic) {
        auto s = scrutinee.type->basic();
        auto b = bound.type->basic();
        if (!s.has_value() || !b.has_value()) {
            return false;
        }
        // Patterns stay strict: the same spelling on both sides. A bound
        // i32 beside an i64 scrutinee is a loud mismatch, not a guess.
        return *s == *b;
    }
    return false;
}

bool Checker::checkMatchConst(const Pattern& pat, TypeRef scrutinee) {
    const Expr* value = pat.value.get();
    // A mismatched pattern is dead, not wrong: it compiles and never
    // matches, so control reaches the default (FEATURES §8.4). Only
    // genuinely unlowerable shapes stay loud.
    auto dead = [&] {
        deadPatterns_.insert(&pat);
        return true;
    };
    Kind sk = scrutinee.type->kind();
    if (dynamic_cast<const NilPtr*>(value) != nullptr) {
        if (sk != Kind::String && sk != Kind::Pointer) {
            return dead();
        }
        return true;
    }
    if (dynamic_cast<const StringLit*>(value) != nullptr) {
        if (sk != Kind::String) {
            return dead();
        }
        return true;
    }
    if (dynamic_cast<const FloatLit*>(value) != nullptr) {
        fail(value->pos, "this pattern is not supported yet");
        return false;
    }
    if (sk == Kind::Enum) {
        // An enum answers only to its named cases: every other spelling
        // beside an enum is dead the way an int beside a bool is. A float
        // never reaches here — it stays loud above, unlowerable anywhere.
        return dead();
    }
    if (sk != Kind::Basic) {
        return dead();
    }
    auto b = scrutinee.type->basic();
    if (!b.has_value()) {
        fail(value->pos, "a constant pattern needs an integer, a bool, or a char");
        return false;
    }
    // The constant keeps its own spelling beside the scrutinee's: a bool
    // beside an int is a dead arm, not a 0/1 comparison.
    if (*b == BasicKind::Bool && dynamic_cast<const BoolLit*>(value) == nullptr) {
        return dead();
    }
    if (*b == BasicKind::Char && dynamic_cast<const CharLit*>(value) == nullptr) {
        return dead();
    }
    if (basicIsInteger(*b) && dynamic_cast<const IntLit*>(value) == nullptr) {
        return dead();
    }
    if (!basicIsInteger(*b) && *b != BasicKind::Bool && *b != BasicKind::Char) {
        return dead();
    }
    std::unordered_set<std::string> active;
    EvalResult folded = evalConstInt(*value, *env_, active);
    if (!folded.ok) {
        fail(value->pos, folded.error);
        return false;
    }
    return true;
}

bool Checker::checkMatchRange(const Pattern& pat, TypeRef scrutinee) {
    // A range orders its scrutinee, so only an integer answers: every
    // other shape is dead the way a mistyped constant is — it compiles
    // and never matches, and an inverted range is empty for the same
    // reason. A bound above INT64_MAX is meaningful only beside an
    // unsigned type; beside a signed one the wrapped bits would read as
    // a negative number the source never wrote, so it fails loudly like
    // the literal fit check it mirrors.
    auto dead = [&] {
        deadPatterns_.insert(&pat);
        return true;
    };
    std::unordered_set<std::string> active;
    EvalResult lo = evalConstInt(*pat.value, *env_, active);
    if (!lo.ok) {
        fail(pat.value->pos, lo.error);
        return false;
    }
    EvalResult hi = evalConstInt(*pat.rangeHi, *env_, active);
    if (!hi.ok) {
        fail(pat.rangeHi->pos, hi.error);
        return false;
    }
    if (scrutinee.type->kind() != Kind::Basic || !scrutinee.type->basic().has_value() ||
        !basicIsInteger(*scrutinee.type->basic())) {
        return dead();
    }
    // Both bounds are non-negative literals, so the wrapped magnitude is
    // exact with or without the big flag: there is no sign to lose.
    uint64_t loMag = static_cast<uint64_t>(lo.value);
    uint64_t hiMag = static_cast<uint64_t>(hi.value);
    if (loMag > hiMag) {
        return dead();
    }
    if ((lo.big || hi.big) && basicIsSigned(*scrutinee.type->basic())) {
        fail(pat.rangeHi->pos, "this range bound does not fit " + typeName(scrutinee));
        return false;
    }
    return true;
}

bool Checker::checkMatchPattern(const Pattern& pat, TypeRef scrutinee) {
    switch (pat.kind) {
        case PatternKind::Wildcard:
            return true;
        case PatternKind::Bind: {
            TypeRef bound = errorRef();
            if (resolveMatchValue(pat.name, bound) &&
                !matchComparable(scrutinee, bound)) {
                deadPatterns_.insert(&pat);
            }
            return true;
        }
        case PatternKind::Const:
            return checkMatchConst(pat, scrutinee);
        case PatternKind::Range:
            return checkMatchRange(pat, scrutinee);
        case PatternKind::Type:
            return checkMatchType(pat, scrutinee);
        case PatternKind::Member:
            return checkMatchMember(pat, scrutinee);
    }
    return false;
}

bool Checker::checkMatchType(const Pattern& pat, TypeRef scrutinee) {
    // A struct pattern names its type and lists every field (FEATURES
    // §8.4): a missing field is a rejection, never a silent don't-care.
    // A pattern for another type is dead, not wrong — it compiles and
    // never matches — except over an interface, where the boxed value
    // could hold the named struct at runtime and only a dynamic check
    // could answer: refusing stays honest until that check lands.
    TypeRef t = resolveHere(pat.type.get());
    if (t.isError()) {
        return false;
    }
    if (t.type->kind() != Kind::Struct) {
        fail(pat.pos, "this pattern needs a struct type");
        return false;
    }
    std::optional<DeclId> id = t.type->decl();
    if (!id.has_value()) {
        fail(pat.pos, "this pattern needs a struct type");
        return false;
    }
    std::unordered_set<std::string> listed;
    for (const FieldPat& field : pat.fields) {
        if (!listed.insert(field.name).second) {
            fail(field.pat->pos, "a struct pattern lists a field twice");
            return false;
        }
        if (resolver_.fieldType(*id, field.name).isError()) {
            fail(field.pat->pos, "no such field");
            return false;
        }
    }
    for (const std::string& name : resolver_.fieldNames(*id)) {
        if (listed.count(name) == 0) {
            fail(pat.pos, "a struct pattern lists every field");
            return false;
        }
    }
    if (scrutinee.type->kind() == Kind::Interface) {
        fail(pat.pos, "this pattern is not supported yet");
        return false;
    }
    bool same = false;
    if (scrutinee.type->kind() == Kind::Struct) {
        std::optional<DeclId> sid = scrutinee.type->decl();
        same = sid.has_value() && sameDecl(*sid, *id);
    }
    if (!same) {
        deadPatterns_.insert(&pat);
    }
    // Every field still classifies against its own type, so a constant
    // of the wrong shape dies the same death a top-level one would —
    // and a dead field sinks the whole arm with it.
    bool ok = true;
    for (const FieldPat& field : pat.fields) {
        TypeRef member = resolver_.fieldType(*id, field.name);
        if (member.isError() || !checkMatchPattern(*field.pat, member)) {
            ok = false;
            continue;
        }
        if (isDeadPattern(field.pat.get())) {
            deadPatterns_.insert(&pat);
        }
    }
    return ok;
}

std::optional<DeclId> Checker::matchMemberUnion(const Pattern& pat) {
    auto tagSpelling = std::make_unique<NamedType>(
        pat.pos, pat.mod, pat.tag, std::vector<std::unique_ptr<TypeExpr>>{});
    TypeRef t = resolveHere(tagSpelling.get());
    if (t.isError() || t.type->kind() != Kind::Union) {
        return std::nullopt;
    }
    return t.type->decl();
}

std::optional<DeclId> Checker::matchMemberEnum(const Pattern& pat) {
    auto tagSpelling = std::make_unique<NamedType>(
        pat.pos, pat.mod, pat.tag, std::vector<std::unique_ptr<TypeExpr>>{});
    TypeRef t = resolveHere(tagSpelling.get());
    if (t.isError() || t.type->kind() != Kind::Enum) {
        return std::nullopt;
    }
    return t.type->decl();
}

bool Checker::checkMatchMember(const Pattern& pat, TypeRef scrutinee) {
    // A member pattern names the live union member or the enum case
    // (FEATURES §8.4): the tag answers at runtime, so only the same union
    // or the same enum stays live. Any other shape can never hold that tag
    // and dies; an interface could box either, which still needs its
    // dynamic check to land.
    auto tagSpelling = std::make_unique<NamedType>(
        pat.pos, pat.mod, pat.tag, std::vector<std::unique_ptr<TypeExpr>>{});
    TypeRef t = resolveHere(tagSpelling.get());
    if (t.isError()) {
        return false;
    }
    if (t.type->kind() != Kind::Union && t.type->kind() != Kind::Enum) {
        fail(pat.pos, "this pattern needs a union or an enum type");
        return false;
    }
    std::optional<DeclId> id = t.type->decl();
    if (!id.has_value()) {
        fail(pat.pos, "this pattern needs a union or an enum type");
        return false;
    }
    if (t.type->kind() == Kind::Enum) {
        bool known = false;
        for (const auto& member : resolver_.enumMembers(*id)) {
            if (member.first == pat.member) {
                known = true;
                break;
            }
        }
        if (!known) {
            fail(pat.pos, "no such member");
            return false;
        }
        if (scrutinee.type->kind() == Kind::Interface) {
            fail(pat.pos, "this pattern is not supported yet");
            return false;
        }
        bool same = false;
        if (scrutinee.type->kind() == Kind::Enum) {
            std::optional<DeclId> sid = scrutinee.type->decl();
            same = sid.has_value() && sameDecl(*sid, *id);
        }
        if (!same) {
            deadPatterns_.insert(&pat);
        }
        return true;
    }
    if (resolver_.fieldType(*id, pat.member).isError()) {
        fail(pat.pos, "no such member");
        return false;
    }
    if (scrutinee.type->kind() == Kind::Interface) {
        fail(pat.pos, "this pattern is not supported yet");
        return false;
    }
    bool same = false;
    if (scrutinee.type->kind() == Kind::Union) {
        std::optional<DeclId> sid = scrutinee.type->decl();
        same = sid.has_value() && sameDecl(*sid, *id);
    }
    if (!same) {
        deadPatterns_.insert(&pat);
    }
    return true;
}

bool Checker::isDeadPattern(const Pattern* pat) const {
    return deadPatterns_.count(pat) != 0;
}

void Checker::bindMatchFresh(const Pattern& pat, TypeRef scrutinee) {
    if (pat.kind == PatternKind::Bind || pat.kind == PatternKind::Type ||
        pat.kind == PatternKind::Member) {
        bindPattern(pat, scrutinee);
    }
}

size_t Checker::localCount() const {
    return current_ == nullptr ? 0 : current_->names.size();
}

void Checker::bindMatchArm(MatchArm& arm, TypeRef scrutinee) {
    // One alternative binding fresh is already the whole body owning the
    // value; with several, no alternative may, so each one is measured
    // on its own — the primary included, since `x | 1` breaks the body
    // exactly as `1 | x` does.
    if (arm.alts.empty()) {
        bindMatchFresh(*arm.pattern, scrutinee);
        return;
    }
    size_t before = localCount();
    bindMatchFresh(*arm.pattern, scrutinee);
    if (localCount() != before) {
        fail(arm.pattern->pos, "an alternation arm binds no names");
    }
    for (auto& alt : arm.alts) {
        before = localCount();
        bindMatchFresh(*alt, scrutinee);
        if (localCount() != before) {
            fail(alt->pos, "an alternation arm binds no names");
        }
    }
}

void Checker::checkMatchGuard(MatchArm& arm) {
    if (arm.guard == nullptr) {
        return;
    }
    TypeRef cond = checkExpr(arm.guard.get(), TypeRef{ctx_.basic(BasicKind::Bool)});
    if (!cond.isError() && cond != TypeRef{ctx_.basic(BasicKind::Bool)}) {
        fail(arm.guard->pos, "a guard is bool");
    }
}

void Checker::visitDeleteStmt(DeleteStmt& n) {
    TypeRef t = checkExpr(n.ptr.get(), TypeRef{ctx_.wildcard()});
    if (t.isError()) {
        return;
    }
    const Type* obj =
        t.type->kind() == Kind::Pointer ? t.type->pointee() : nullptr;
    // Classes run their destructor chain before release. Structs and unions
    // have no destructor (no deinit) and simply return their storage to the
    // library operator. Every other pointee has no defined deletion: a
    // scalar, pointer, function pointer or interface is refused loudly.
    if (obj == nullptr || (obj->kind() != Kind::Class &&
                           obj->kind() != Kind::Struct &&
                           obj->kind() != Kind::Union)) {
        fail(n.pos, "delete needs a class, struct or union pointer");
        return;
    }
    if (!requireAllocOperator(false, n.pos)) {
        return;
    }
    if (sink_ != nullptr) {
        sink_->onDelete(n);
    }
}

// Every `{placeholder}` in an asm block must name a local, a parameter, or
// a module-level declaration: the text is raw assembly the checker cannot
// see into, so a typo'd name has to fail here (FEAT-035).
void Checker::visitModuleAsm(ModuleAsm& n) {
    const target::Info& info = target::info(target_);
    // Module-scope asm is raw text for an assembler. A target that emits
    // assembly can pass it through; a PE target and the interpreter cannot
    // execute it, and must reject it rather than silently drop the block.
    if (!info.allowsAsm || info.output != target::Output::Asm) {
        fail(n.pos, std::string("module-scope asm is not supported on ") +
                        info.name);
    }
}

void Checker::visitAsmStmt(AsmStmt& n) {
    if (!target::info(target_).allowsAsm) {
        fail(n.pos, std::string("asm blocks are not supported on ") +
                        target::info(target_).name);
        return;
    }
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

void Checker::visitDeferStmt(DeferStmt& n) {
    // A defer names no storage: without a function there is no scope exit
    // to run it at, so it refuses where it is written.
    if (!inFunc_) {
        fail(n.pos, "defer outside a function");
        return;
    }
    // FEATURES §5.2: the body runs at scope exit with exit-time values, so
    // checking walks it in place (names resolve in the enclosing scope)
    // while lowering replays it at every exit. The sink detaches for the
    // check walk: attached, the body would lower inline at the defer point
    // and run twice. Returns, breaks and continues inside refuse above
    // through inDefer_.
    ++inDefer_;
    LowerSink* savedSink = sink_;
    sink_ = nullptr;
    n.stmt->accept(*this);
    sink_ = savedSink;
    --inDefer_;
    if (failed_) {
        return;
    }
    if (sink_ != nullptr) {
        sink_->onDeferStmt(n);
    }
}

void Checker::bindPattern(const Pattern& pat, TypeRef scrutinee) {
    // Silent by design: checkMatchPattern already reported every shape
    // problem, so binding only introduces names, never a second error.
    if (pat.kind == PatternKind::Bind) {
        TypeRef bound = errorRef();
        if (!resolveMatchValue(pat.name, bound)) {
            bindLocal(pat.name, scrutinee, false, pat.pos);
        }
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
        for (const FieldPat& field : pat.fields) {
            TypeRef member = resolver_.fieldType(*id, field.name);
            if (member.isError()) {
                continue;
            }
            bindSubPattern(*field.pat, member);
        }
        return;
    }
    if (pat.kind == PatternKind::Member) {
        auto tagSpelling = std::make_unique<NamedType>(
            pat.pos, pat.mod, pat.tag, std::vector<std::unique_ptr<TypeExpr>>{});
        TypeRef t = resolveHere(tagSpelling.get());
        if (t.isError() || t.type->kind() != Kind::Union) {
            return;
        }
        std::optional<DeclId> id = t.type->decl();
        if (!id.has_value()) {
            return;
        }
        if (resolver_.fieldType(*id, pat.member).isError()) {
            return;
        }
        return;
    }
}

void Checker::bindSubPattern(const Pattern& pat, TypeRef type) {
    if (pat.kind == PatternKind::Bind) {
        // The Erlang rule, one level down: a name already in scope
        // compares by value (checkMatchPattern proved it comparable or
        // marked the arm dead); only an unresolvable name binds fresh.
        TypeRef bound = errorRef();
        if (!resolveMatchValue(pat.name, bound)) {
            bindLocal(pat.name, type, false, pat.pos);
        }
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
        for (const FieldPat& field : pat.fields) {
            TypeRef member = resolver_.fieldType(*id, field.name);
            if (member.isError()) {
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
    if (auto* wide = dynamic_cast<WideLit*>(expr)) {
        // N units plus the NUL terminator: the array spelling that fits
        // exactly. Pointer positions convert through checkCompatible.
        const Type* elem = ctx_.basic(BasicKind::U16);
        size_t units = wide->units.size();
        int64_t cells = static_cast<int64_t>(units) + 1;
        TypeRef t{ctx_.array(elem, cells)};
        record(wide, t);
        return t;
    }
    if (auto* wideChar = dynamic_cast<WideChar*>(expr)) {
        if (wideChar->value > 0xFFFF) {
            fail(wideChar->pos, "a wide character holds exactly one unit");
            record(wideChar, errorRef());
            return errorRef();
        }
        record(wideChar, TypeRef{ctx_.basic(BasicKind::U16)});
        return TypeRef{ctx_.basic(BasicKind::U16)};
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
    if (auto* alit = dynamic_cast<ArrayLit*>(expr)) {
        TypeRef t = checkArrayLit(*alit, expected);
        record(alit, t);
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
    if (auto* sx = dynamic_cast<SizeofExpr*>(expr)) {
        visitSizeofExpr(*sx);
        return typeOf(sx);
    }
    if (auto* al = dynamic_cast<AlignofType*>(expr)) {
        visitAlignofType(*al);
        return typeOf(al);
    }
    if (auto* fun = dynamic_cast<FuncLit*>(expr)) {
        TypeRef t = checkFuncLit(*fun);
        record(fun, t);
        return t;
    }
    if (auto* un = dynamic_cast<UninitExpr*>(expr)) {
        visitUninitExpr(*un);
        return typeOf(un);
    }
    if (auto* si = dynamic_cast<SuperInit*>(expr)) {
        visitSuperInit(*si);
        return typeOf(si);
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
    (void)pos;
    if (!expected.isError() && expected != TypeRef{ctx_.wildcard()} &&
        expected.type->kind() == Kind::Basic && basicIsFloat(*expected.type->basic())) {
        return expected;
    }
    // `3.14f` names f32 on its own; a bare float literal stays f64
    // (Unified Standard 2.2).
    if (lit.narrow) {
        return TypeRef{ctx_.basic(BasicKind::F32)};
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
        // A bare enumerator names its enum's member: exactly one enum in
        // this module may claim it. Imported enums keep their qualified
        // spelling.
        int owners = 0;
        const resolve::Decl* owner =
            resolver_.lookupEnumMember(moduleScope_, expr.name, owners);
        if (owner != nullptr) {
            return TypeRef{ctx_.nominal(Kind::Enum, owner->id)};
        }
        if (owners > 1) {
            fail(expr.pos, "ambiguous enumerator '" + expr.name + "'");
            return errorRef();
        }
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

bool Checker::checkOperatorOverload(Binary& expr, TypeRef lhs, TypeRef rhs,
                                    TypeRef& result) {
    const char* name = binOpOperatorName(expr.op);
    if (name == nullptr) {
        return false;
    }
    const auto nominal = [](TypeRef type, Kind kind) {
        return type.type->kind() == kind;
    };
    const bool lhsNeeds = nominal(lhs, Kind::Struct) || nominal(lhs, Kind::Class);
    const bool rhsNeeds = nominal(rhs, Kind::Struct) || nominal(rhs, Kind::Class);
    const bool lhsEnum = nominal(lhs, Kind::Enum);
    const bool rhsEnum = nominal(rhs, Kind::Enum);
    if (!lhsNeeds && !rhsNeeds && !lhsEnum && !rhsEnum) {
        return false;
    }

    // A member operator lives on the left operand's own type. It is tried
    // first; when it is absent or its parameter is not an exact match, the
    // free declaration below is still a candidate.
    if (lhsNeeds) {
        std::optional<DeclId> id = lhs.type->decl();
        const Signature* sig =
            id.has_value() ? resolver_.methodSig(*id, name) : nullptr;
        if (sig != nullptr && sig->arity() == kOneParameter) {
            TypeRef parameter{sig->parameters()[0].type};
            if (parameter == rhs) {
                memberOperatorCalls_.insert(&expr);
                const Type* ret = sig->returnType();
                result = TypeRef{ret != nullptr ? ret : ctx_.void_()};
                return true;
            }
        }
    }

    const resolve::Decl* decl = resolver_.lookup(moduleScope_, name);
    if (decl != nullptr && decl->kind == resolve::DeclKind::Func &&
        decl->typeParams == 0 && decl->sig.arity() == 2) {
        TypeRef first{decl->sig.parameters()[0].type};
        TypeRef second{decl->sig.parameters()[1].type};
        if (first == lhs && second == rhs) {
            operatorDecls_[&expr] = decl;
            const Type* ret = decl->sig.returnType();
            result = TypeRef{ret != nullptr ? ret : ctx_.void_()};
            return true;
        }
    }

    // A class or struct has no built-in arithmetic or comparison, so silence
    // here would only become a confusing "arithmetic needs numbers" later.
    // Enums keep their built-in comparison rules when no exact operator match
    // exists; an enum operator is selected only on an exact pair.
    if (lhsNeeds || rhsNeeds) {
        fail(expr.pos, std::string("no matching operator '") +
                          binOpSpelling(expr.op) + "'");
        result = errorRef();
        return true;
    }
    return false;
}

bool Checker::checkOperatorOverload(Unary& expr, TypeRef operand,
                                    TypeRef& result) {
    const char* name = unOpOperatorName(expr.op);
    if (name == nullptr) {
        return false;
    }
    const Kind kind = operand.type->kind();
    const bool nominal =
        kind == Kind::Struct || kind == Kind::Class || kind == Kind::Enum;
    if (!nominal) {
        return false;
    }

    // A member unary operator takes no parameters; the receiver is the
    // operand. It wins over a free declaration when both would be exact.
    if (kind == Kind::Struct || kind == Kind::Class) {
        std::optional<DeclId> id = operand.type->decl();
        const Signature* sig =
            id.has_value() ? resolver_.methodSig(*id, name) : nullptr;
        if (sig != nullptr && sig->arity() == kNoParameters) {
            memberUnaryOperatorCalls_.insert(&expr);
            const Type* ret = sig->returnType();
            result = TypeRef{ret != nullptr ? ret : ctx_.void_()};
            return true;
        }
    }

    const resolve::Decl* decl = resolver_.lookup(moduleScope_, name);
    if (decl != nullptr && decl->kind == resolve::DeclKind::Func &&
        decl->typeParams == 0 && decl->sig.arity() == 1) {
        TypeRef parameter{decl->sig.parameters()[0].type};
        if (parameter == operand) {
            unaryOperatorDecls_[&expr] = decl;
            const Type* ret = decl->sig.returnType();
            result = TypeRef{ret != nullptr ? ret : ctx_.void_()};
            return true;
        }
    }

    fail(expr.pos, std::string("no matching operator '") +
                      unOpSpelling(expr.op) + "'");
    result = errorRef();
    return true;
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
    TypeRef overloaded;
    if (checkOperatorOverload(expr, lhs, rhs, overloaded)) {
        return overloaded;
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
        const bool lhsChar = lhs.type->kind() == Kind::Basic &&
                             *lhs.type->basic() == BasicKind::Char;
        const bool rhsChar = rhs.type->kind() == Kind::Basic &&
                             *rhs.type->basic() == BasicKind::Char;
        if (lhsChar || rhsChar) {
            // Chars compare by byte value: equality and ordering alike
            // (Unified Standard 2.3). A char beside a non-char still
            // needs an explicit `as` — a bare 66 is an int, not a char.
            if (lhsChar && rhsChar) {
                return TypeRef{ctx_.basic(BasicKind::Bool)};
            }
            fail(expr.pos, "a char compares with a char, not a number");
            return errorRef();
        }
        const bool lhsString = lhs.type->kind() == Kind::String;
        const bool rhsString = rhs.type->kind() == Kind::String;
        if (lhsString || rhsString) {
            // Equality on strings compares content (SEMANTICS §4, the
            // same rule as the other value types): ordering has no
            // meaning, and a string beside a non-string is a mismatch —
            // identity through a *u8 needs spelling, not silence.
            if (isEq && lhsString && rhsString) {
                return TypeRef{ctx_.basic(BasicKind::Bool)};
            }
            // A null names no type, so it checks against a string the
            // same way it checks against any pointer: the null string
            // is empty, and the comparison below reads no bytes for
            // one. Only the nilPtr literal — a *void variable beside a
            // string stays a mismatch, like the cast rule.
            const bool lhsNil = dynamic_cast<const NilPtr*>(expr.lhs.get()) != nullptr;
            const bool rhsNil = dynamic_cast<const NilPtr*>(expr.rhs.get()) != nullptr;
            if (isEq && ((lhsString && rhsNil) || (rhsString && lhsNil))) {
                return TypeRef{ctx_.basic(BasicKind::Bool)};
            }
            fail(expr.pos, "strings compare for equality only");
            return errorRef();
        }
        const bool lhsEnum = lhs.type->kind() == Kind::Enum;
        const bool rhsEnum = rhs.type->kind() == Kind::Enum;
        if (lhsEnum || rhsEnum) {
            // Enums compare by underlying value: two spellings of one
            // enum, or either side an integer. Different enums never
            // compare, and ordering answers the integer order.
            auto enumId = [](const TypeRef& t) -> std::optional<DeclId> {
                return t.type->kind() == Kind::Enum ? t.type->decl()
                                                    : std::nullopt;
            };
            auto isInt = [](const TypeRef& t) -> bool {
                return t.type->kind() == Kind::Basic &&
                       basicIsInteger(*t.type->basic());
            };
            std::optional<DeclId> l = enumId(lhs);
            std::optional<DeclId> r = enumId(rhs);
            if (l.has_value() && r.has_value()) {
                if (!sameDecl(*l, *r)) {
                    fail(expr.pos, "these enum types cannot compare");
                    return errorRef();
                }
                return TypeRef{ctx_.basic(BasicKind::Bool)};
            }
            if ((l.has_value() && isInt(rhs)) ||
                (r.has_value() && isInt(lhs))) {
                return TypeRef{ctx_.basic(BasicKind::Bool)};
            }
            fail(expr.pos, "an enum compares against its own type or an integer");
            return errorRef();
        }
        const bool lhsIface = lhs.type->kind() == Kind::Interface;
        const bool rhsIface = rhs.type->kind() == Kind::Interface;
        if (lhsIface || rhsIface) {
            // Equality on an interface is a null check: a null interface
            // carries no vtable, so it compares against nilPtr only.
            // Anything else beside an interface is a mismatch the source
            // must spell — dispatch first, compare after.
            const bool lhsNil = dynamic_cast<const NilPtr*>(expr.lhs.get()) != nullptr;
            const bool rhsNil = dynamic_cast<const NilPtr*>(expr.rhs.get()) != nullptr;
            if (isEq && ((lhsIface && rhsNil) || (rhsIface && lhsNil))) {
                return TypeRef{ctx_.basic(BasicKind::Bool)};
            }
            fail(expr.pos, "interfaces compare against nilPtr only");
            return errorRef();
        }
        const bool lhsClosure = lhs.type->kind() == Kind::Closure;
        const bool rhsClosure = rhs.type->kind() == Kind::Closure;
        if (lhsClosure || rhsClosure) {
            // Equality on a closure is a null check: a null closure
            // carries no code, so it compares against nilPtr only.
            // Anything else beside a closure is a mismatch the source
            // must spell — closures compare by address, not by value.
            const bool lhsNil = dynamic_cast<const NilPtr*>(expr.lhs.get()) != nullptr;
            const bool rhsNil = dynamic_cast<const NilPtr*>(expr.rhs.get()) != nullptr;
            if (isEq && ((lhsClosure && rhsNil) || (rhsClosure && lhsNil))) {
                return TypeRef{ctx_.basic(BasicKind::Bool)};
            }
            fail(expr.pos, "closures compare against nilPtr only");
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
        if (lhs.type->kind() == Kind::FunctionPtr ||
            rhs.type->kind() == Kind::FunctionPtr) {
            // Function pointers compare by address (FEATURES §4.2):
            // same-signature pairs, or either side nil — nilPtr spells
            // *void, which every address accepts. Anything else beside
            // a function pointer rejects rather than comparing garbage.
            bool lhsVoid = lhs.type->kind() == Kind::Pointer &&
                           lhs.type->pointee() == ctx_.void_();
            bool rhsVoid = rhs.type->kind() == Kind::Pointer &&
                           rhs.type->pointee() == ctx_.void_();
            const Signature* l = lhs.type->kind() == Kind::FunctionPtr
                                     ? lhs.type->signature()
                                     : nullptr;
            const Signature* r = rhs.type->kind() == Kind::FunctionPtr
                                     ? rhs.type->signature()
                                     : nullptr;
            if (l != nullptr && r != nullptr) {
                if (!sameSignature(*l, *r)) {
                    fail(expr.pos, "these function pointer types cannot compare");
                    return errorRef();
                }
                return TypeRef{ctx_.basic(BasicKind::Bool)};
            }
            if ((l != nullptr && rhsVoid) || (r != nullptr && lhsVoid)) {
                return TypeRef{ctx_.basic(BasicKind::Bool)};
            }
            fail(expr.pos, "these function pointer types cannot compare");
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
    // FEATURES §1.7 / SEMANTICS §8.2: `p ± n` answers the same *T, scaled
    // by the element's size; `p - q` over one *T answers an i64 count.
    // A pointer whose pointee is void has no element size, so no *void
    // arithmetic answers; anything else a non-Basic reaches falls below
    // to the common type's loud rejection.
    const bool lhsPtr = lhs.type->kind() == Kind::Pointer;
    const bool rhsPtr = rhs.type->kind() == Kind::Pointer;
    if (lhsPtr || rhsPtr) {
        const bool lhsVoid = lhsPtr && lhs.type->pointee() == ctx_.void_();
        const bool rhsVoid = rhsPtr && rhs.type->pointee() == ctx_.void_();
        if (lhsPtr && !lhsVoid && isInteger(rhs) &&
            (expr.op == BinOp::Add || expr.op == BinOp::Sub)) {
            return lhs;
        }
        if (rhsPtr && !rhsVoid && isInteger(lhs) && expr.op == BinOp::Add) {
            return rhs;
        }
        if (lhsPtr && rhsPtr && !lhsVoid && !rhsVoid && expr.op == BinOp::Sub &&
            lhs.type->pointee() == rhs.type->pointee()) {
            return TypeRef{ctx_.basic(BasicKind::I64)};
        }
        fail(expr.pos, "these pointer operands do not combine");
        return errorRef();
    }
    // SEMANTICS §8: `s1 + s2` is a new string with s1's bytes then
    // s2's. Anything else beside a string refuses — a number is not a
    // string, in either order, with no implicit conversion.
    if (expr.op == BinOp::Add &&
        (lhs.type->kind() == Kind::String || rhs.type->kind() == Kind::String)) {
        if (lhs.type->kind() == Kind::String && rhs.type->kind() == Kind::String) {
            return lhs;
        }
        fail(expr.pos, "a string concatenates only with a string");
        return errorRef();
    }
    // Unified Standard 2.3: char beside char stays char through + and -,
    // so `c - 'a' + 'A'` is char all the way and needs no casts.
    const bool lhsCh = lhs.type->kind() == Kind::Basic &&
                       *lhs.type->basic() == BasicKind::Char;
    const bool rhsCh = rhs.type->kind() == Kind::Basic &&
                       *rhs.type->basic() == BasicKind::Char;
    if (lhsCh && rhsCh && (expr.op == BinOp::Add || expr.op == BinOp::Sub)) {
        return lhs;
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
    if (expr.op == BinOp::Shl || expr.op == BinOp::Shr) {
        if (divisor.value < 0) {
            fail(expr.rhs->pos, "a shift count is not negative");
            return;
        }
        // Standard §3: a constant count at or past the operand's width is
        // refused; only a runtime count masks. The width is in bits, not
        // bytes, so the size gate never watches this line.
        if (lhs.type != nullptr && lhs.type->kind() == Kind::Basic &&
            basicIsInteger(*lhs.type->basic()) &&
            divisor.value >= basicWidthBits(*lhs.type->basic())) {
            fail(expr.rhs->pos, "a shift count is too large");
        }
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
    // `super.m(args)`: the base's method, resolved statically — never the
    // derived override and never a vtable slot (Unified Standard 4.8.3).
    if (const auto* field = dynamic_cast<const Field*>(expr.callee.get())) {
        if (dynamic_cast<const Super*>(field->object.get()) != nullptr) {
            if (!currentClass_.has_value()) {
                fail(expr.pos, "super is only a receiver inside a method");
                return errorRef();
            }
            const Signature* baseSig = nullptr;
            std::vector<DeclId> superBases = resolver_.classBases(*currentClass_);
            if (superBases.begin() + 1 != superBases.end()) {
                // 4.8: `super` names one base; with several there is no
                // single method to resolve.
                fail(expr.pos, "super needs exactly one base class");
                return errorRef();
            }
            for (DeclId baseId : superBases) {
                baseSig = resolver_.methodSig(baseId, field->name);
                if (baseSig != nullptr) {
                    break;
                }
            }
            if (baseSig == nullptr) {
                fail(expr.pos, "no base declares a method named '" + field->name + "'");
                return errorRef();
            }
            record(field, TypeRef{ctx_.closure(*baseSig)});
            if (expr.args.size() != baseSig->arity()) {
                fail(expr.pos, "wrong number of arguments");
                return errorRef();
            }
            for (uint64_t i = 0; i < static_cast<uint64_t>(expr.args.size()); ++i) {
                TypeRef want{baseSig->parameters()[i].type};
                TypeRef got = checkExpr(expr.args[i].get(), want);
                checkCompatible(expr.args[i]->pos, expr.args[i].get(), got, want);
            }
            const Type* ret =
                baseSig->returnType() != nullptr ? baseSig->returnType() : ctx_.void_();
            return TypeRef{ret};
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
        if (kind == Kind::Struct || kind == Kind::Class) {
            // `v(args)`: the object is callable only through its member
            // `operator()`. There is no built-in call rule for a nominal
            // value, and no free fallback: `()` is member-only.
            std::optional<DeclId> id = callee.type->decl();
            const Signature* callSig =
                id.has_value() ? resolver_.methodSig(*id, "operator_call")
                               : nullptr;
            if (callSig == nullptr || expr.args.size() != callSig->arity()) {
                fail(expr.pos, "no matching operator '()'");
                return errorRef();
            }
            for (uint64_t i = 0; i < static_cast<uint64_t>(expr.args.size()); ++i) {
                TypeRef want{callSig->parameters()[i].type};
                TypeRef got = checkExpr(expr.args[i].get(), want);
                if (got.isError()) {
                    return errorRef();
                }
                if (got != want) {
                    fail(expr.pos, "no matching operator '()'");
                    return errorRef();
                }
            }
            operatorCalls_.insert(&expr);
            const Type* ret = callSig->returnType() != nullptr
                                  ? callSig->returnType()
                                  : ctx_.void_();
            return TypeRef{ret};
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
    // Context-keyed: a shared template body records one spec per walking
    // context, so a later walk for other arguments cannot clobber this one.
    std::string ctxKey;
    if (walkSpec_.has_value()) {
        ctxKey = std::to_string(walkSpec_->module) + "." +
                 std::to_string(walkSpec_->index);
    }
    callSpecs_[std::make_pair(&expr, ctxKey)] = *spec;
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
        // A template body names its defining module: helpers the body
        // calls resolve there, never in the caller's module. Same-module
        // calls pass their own scope back, so nothing changes for them.
        const resolve::Scope* homeScope = resolver_.scopeOf(home);
        const resolve::Scope* scope = resolver_.typeParamScope(
            homeScope != nullptr ? homeScope : typeScope_, module_,
            spelling->typeParams, decl->id, expr.pos);
        const resolve::Scope* savedScope = typeScope_;
        typeScope_ = scope;
        const resolve::Scope* savedModuleScope = moduleScope_;
        if (homeScope != nullptr) {
            moduleScope_ = homeScope;
        }
        std::optional<ActiveInst> savedActive = active_;
        active_ = ActiveInst{decl->id, argTypes, spelling->typeParams};
        std::optional<DeclId> savedWalk = walkSpec_;
        walkSpec_ = spec;
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
        moduleScope_ = savedModuleScope;
        active_ = savedActive;
        walkSpec_ = savedWalk;
    }
    const Type* ret = sig->returnType() != nullptr ? sig->returnType() : ctx_.void_();
    return TypeRef{ret};
}

void Checker::walkSpecForEmit(DeclId spec) {
    // The lowerer's emission walk: same setup as the checking walk above,
    // but the sink stays attached so statements lower, and no walked_ gate
    // (the lowerer emits each spec once; a recursive call into the same
    // spec checks against its signature through walked_ without re-walking).
    // Module and type scopes stay whatever the last module left: the
    // checking walks ran under the caller's scopes too, so names resolve
    // exactly as proven. The body was proven clean, so this re-walk
    // reports nothing new — unless it is a lowering gap, failed loudly.
    auto origin = resolver_.instantiationOf(spec);
    if (!origin.has_value()) {
        return;
    }
    const resolve::Decl* tmplDecl = resolver_.declById(origin->first);
    if (tmplDecl == nullptr || tmplDecl->kind != resolve::DeclKind::Func) {
        return;
    }
    std::string home = tmplDecl->scope != nullptr
                           ? tmplDecl->scope->module
                           : module_;
    const FuncDecl* spelling = funcSpelling(home, tmplDecl->name);
    const Signature* sig = resolver_.funcSig(spec);
    if (spelling == nullptr || sig == nullptr) {
        fail(Position{module_, 1, 1}, "checker bug: template body is missing");
        return;
    }
    // Home scopes, like the checking walk: helpers resolve in the
    // defining module so emission answers what checking proved.
    const resolve::Scope* homeScope = resolver_.scopeOf(home);
    const resolve::Scope* scope = resolver_.typeParamScope(
        homeScope != nullptr ? homeScope : typeScope_, module_,
        spelling->typeParams, origin->first, spelling->pos);
    const resolve::Scope* savedScope = typeScope_;
    typeScope_ = scope;
    const resolve::Scope* savedModuleScope = moduleScope_;
    if (homeScope != nullptr) {
        moduleScope_ = homeScope;
    }
    std::optional<ActiveInst> savedActive = active_;
    active_ = ActiveInst{origin->first, origin->second, spelling->typeParams};
    std::optional<DeclId> savedWalk = walkSpec_;
    walkSpec_ = spec;
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
    if (sink_ != nullptr) {
        sink_->onSpecFunc(spec, *spelling, *sig);
    }
    funcReturn_ = savedRet;
    inFunc_ = wasFunc;
    popScope();
    typeScope_ = savedScope;
    moduleScope_ = savedModuleScope;
    active_ = savedActive;
    walkSpec_ = savedWalk;
}

bool Checker::fieldVisible(DeclId id, const std::string& name, Position pos) {
    // 4.8.4: public is the default; the resolver only marks private
    // fields. The home module is the declaring class's, so an inherited
    // field answers its base's module rather than the derived one's.
    if (!resolver_.fieldIsPrivate(id, name)) {
        return true;
    }
    std::string home = resolver_.fieldModule(id, name);
    if (home == module_) {
        return true;
    }
    fail(pos, "'" + name + "' is private to module '" + home + "'");
    return false;
}

bool Checker::methodVisible(DeclId id, const std::string& name, Position pos) {
    // 4.8.4: methods follow fields. Public is the default; an inherited
    // private method answers its declaring module, not the derived one.
    if (!resolver_.methodIsPrivate(id, name)) {
        return true;
    }
    std::string home = resolver_.methodModule(id, name);
    if (home == module_) {
        return true;
    }
    fail(pos, "'" + name + "' is private to module '" + home + "'");
    return false;
}

TypeRef Checker::checkField(Field& expr) {
    if (expr.isArrow) {
        auto* deref = dynamic_cast<Unary*>(expr.object.get());
        if (deref == nullptr || deref->op != UnOp::Deref) {
            fail(expr.pos, "operator '->' needs a pointer");
            return errorRef();
        }
        TypeRef receiver = checkExpr(deref->operand.get(),
                                     TypeRef{ctx_.wildcard()});
        if (receiver.isError()) {
            return errorRef();
        }
        Kind receiverKind = receiver.type->kind();
        if (receiverKind == Kind::Pointer) {
            TypeRef target{receiver.type->pointee()};
            record(deref, target);
            return checkFieldType(target, expr);
        }
        if (receiverKind == Kind::Class || receiverKind == Kind::Struct) {
            std::optional<DeclId> id = receiver.type->decl();
            if (!id.has_value()) {
                return errorRef();
            }
            const Signature* sig = resolver_.methodSig(*id, "operator_arrow");
            if (sig == nullptr) {
                fail(expr.pos, "no matching operator '->'");
                return errorRef();
            }
            if (!methodVisible(*id, "operator_arrow", expr.pos)) {
                return errorRef();
            }
            if (sig->arity() != kNoParameters) {
                fail(expr.pos, "an arrow operator takes no parameters");
                return errorRef();
            }
            const Type* target = sig->returnType();
            if (target == nullptr || target->kind() != Kind::Pointer ||
                target->pointee() == nullptr) {
                fail(expr.pos, "an arrow operator must return a pointer");
                return errorRef();
            }
            TypeRef pointee{target->pointee()};
            operatorArrows_[&expr] = pointee;
            return checkFieldType(pointee, expr);
        }
        fail(expr.pos, "operator '->' needs a pointer or a class/struct");
        return errorRef();
    }
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
                    // A bare exported enumerator (`lib.CIRCLE`) names its
                    // enum's member: exactly one enum in that module may
                    // own it, or the name stays unexported. Direct members
                    // already missed above, so this branch owns nothing
                    // they could name.
                    if (home != nullptr) {
                        const resolve::Decl* owner = nullptr;
                        int owners = 0;
                        for (const auto& [name, decl] : home->bindings) {
                            if (decl == nullptr ||
                                decl->kind != resolve::DeclKind::Type ||
                                decl->nominal != Kind::Enum) {
                                continue;
                            }
                            bool found = false;
                            for (const auto& entry :
                                 resolver_.enumMembers(decl->id)) {
                                if (entry.first == expr.name) {
                                    found = true;
                                    break;
                                }
                            }
                            if (found) {
                                ++owners;
                                owner = decl;
                            }
                        }
                        if (owner != nullptr && owners == 1) {
                            return TypeRef{ctx_.nominal(Kind::Enum, owner->id)};
                        }
                        if (owners > 1) {
                            fail(expr.pos, "ambiguous enumerator '" +
                                              expr.name + "'");
                            return errorRef();
                        }
                    }
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
            // `Color.RED`: the qualifier names an enum type, so the member
            // is an enumerator and denotes its integer value (TYPES §2.6).
            // A bare `RED` stays unbound: enumerators never enter scope.
            const resolve::Decl* named =
                resolver_.lookup(moduleScope_, ident->name);
            if (named != nullptr &&
                named->kind == resolve::DeclKind::Type &&
                named->nominal == Kind::Enum) {
                std::string home = named->scope != nullptr
                                       ? named->scope->module
                                       : module_;
                if (!resolver_.isEnumMember(home + "." + named->name,
                                            expr.name)) {
                    fail(expr.pos, "'" + expr.name + "' is not an enumerator of '" +
                                      named->name + "'");
                    return errorRef();
                }
                // Enumerators check as their enum: conversions and
                // equality key off the type, while lowering still flows
                // the underlying i32 (see widthOf).
                return TypeRef{ctx_.nominal(Kind::Enum, named->id)};
            }
            // `Shade.GREEN`: the qualifier aliases an enum, so the member
            // is that enum's enumerator. The alias spells the same type
            // the enum does; anything else here is not a value.
            if (named != nullptr &&
                named->kind == resolve::DeclKind::Alias &&
                named->aliasTarget != nullptr) {
                TypeRef target = resolver_.resolveType(named->aliasTarget,
                                                        moduleScope_);
                if (!target.isError() && target.type->kind() == Kind::Enum) {
                    std::optional<DeclId> aliasId = target.type->decl();
                    if (aliasId.has_value()) {
                        bool found = false;
                        for (const auto& entry :
                             resolver_.enumMembers(*aliasId)) {
                            if (entry.first == expr.name) {
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            fail(expr.pos, "'" + expr.name +
                                              "' is not an enumerator of '" +
                                              ident->name + "'");
                            return errorRef();
                        }
                        return TypeRef{ctx_.nominal(Kind::Enum, *aliasId)};
                    }
                }
            }
        }
    }
    if (const auto* inner = dynamic_cast<const Field*>(expr.object.get())) {
        // `mid.Col.GREEN`: the object names an enum through a module
        // member — either the enum itself or an alias of it — so the
        // field names one of its enumerators (TYPES §2.6). A bare
        // `mid.Col` stays unbound: only the three-part spelling is a
        // value, and anything else falls through to fail loudly below.
        if (const auto* mod = dynamic_cast<const Ident*>(inner->object.get())) {
            if (findLocal(mod->name) == nullptr) {
                const resolve::Decl* modDecl =
                    resolver_.lookup(moduleScope_, mod->name);
                if (modDecl != nullptr &&
                    modDecl->kind == resolve::DeclKind::Module) {
                    const resolve::Scope* home =
                        resolver_.scopeOf(modDecl->origin);
                    const resolve::Decl* member =
                        home != nullptr ? resolver_.lookup(home, inner->name)
                                        : nullptr;
                    TypeRef target;
                    std::optional<DeclId> enumId;
                    if (member != nullptr &&
                        member->kind == resolve::DeclKind::Type &&
                        member->nominal == Kind::Enum) {
                        enumId = member->id;
                    } else if (member != nullptr &&
                               member->kind == resolve::DeclKind::Alias &&
                               member->aliasTarget != nullptr) {
                        target = resolver_.resolveType(member->aliasTarget, home);
                        if (!target.isError() &&
                            target.type->kind() == Kind::Enum) {
                            enumId = target.type->decl();
                        }
                    }
                    if (enumId.has_value()) {
                        bool found = false;
                        for (const auto& entry : resolver_.enumMembers(*enumId)) {
                            if (entry.first == expr.name) {
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            fail(expr.pos, "'" + expr.name +
                                                  "' is not an enumerator of '" +
                                                  inner->name + "'");
                            return errorRef();
                        }
                        // Enumerators check as their enum: conversions and
                        // equality key off the type, while lowering still
                        // flows the underlying i32 (see widthOf).
                        return TypeRef{ctx_.nominal(Kind::Enum, *enumId)};
                    }
                }
            }
        }
    }
    TypeRef object = checkExpr(expr.object.get(), TypeRef{ctx_.wildcard()});
    if (object.isError()) {
        return errorRef();
    }
    return checkFieldType(object, expr);
}
TypeRef Checker::checkFieldType(TypeRef object, Field& expr) {
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
        if (resolver_.fieldAmbiguous(*id, expr.name)) {
            // 4.8: two bases declare the field, so no single slot is the
            // answer. The name refuses instead of picking one.
            fail(expr.pos, "field '" + expr.name +
                               "' is ambiguous: several base classes declare it");
            return errorRef();
        }
        TypeRef member = resolver_.fieldType(*id, expr.name);
        if (!member.isError()) {
            if (!fieldVisible(*id, expr.name, expr.pos)) {
                return errorRef();
            }
            return member;
        }
        const Signature* sig = resolver_.methodSig(*id, expr.name);
        if (sig == nullptr) {
            fail(expr.pos, "no such field");
            return errorRef();
        }
        if (resolver_.methodAmbiguous(*id, expr.name)) {
            fail(expr.pos, "method '" + expr.name +
                               "' is ambiguous: several base classes declare it");
            return errorRef();
        }
        if (!methodVisible(*id, expr.name, expr.pos)) {
            return errorRef();
        }
        return TypeRef{ctx_.closure(*sig)};
    }
    if (kind == Kind::Slice || kind == Kind::String || kind == Kind::Array) {
        // The only member a sized value exposes is its element count; the
        // checker names no other field so the lowerer never guesses one.
        // 2.4 / 6.2: the field is an int, the Standard's `len` spelling.
        if (expr.name == "len") {
            return TypeRef{ctx_.basic(BasicKind::I32)};
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
            if (!fieldVisible(*id, expr.name, expr.pos)) {
                return errorRef();
            }
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
        if (!methodVisible(*id, expr.name, expr.pos)) {
            return errorRef();
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
    std::optional<DeclId> initHome = resolver_.methodHome(*id, "init");
    if (kind == Kind::Class && initHome.has_value() &&
        sameDecl(*initHome, *id)) {
        fail(expr.pos, "this class has an init: create it with new C(args)");
        return errorRef();
    }
    std::vector<std::string> fields = resolver_.chainFieldNames(*id);
    std::unordered_set<std::string> named;
    for (const FieldInit& init : expr.fields) {
        TypeRef field = resolver_.fieldType(*id, init.name);
        if (field.isError()) {
            fail(init.value->pos, "no such field");
            return errorRef();
        }
        // v2 CIDER-BUG-049: one name, one store; a repeated name would
        // quietly overwrite the store before it.
        if (!named.insert(init.name).second) {
            fail(init.value->pos, "field '" + init.name + "' is named twice");
            return errorRef();
        }
        if (!fieldVisible(*id, init.name, init.value->pos)) {
            return errorRef();
        }
        TypeRef value = checkExpr(init.value.get(), field);
        if (!checkCompatible(init.value->pos, init.value.get(), value, field)) {
            return errorRef();
        }
    }
    for (const std::string& name : fields) {
        // A defaulted field may be left out: its default runs first.
        if (resolver_.fieldDefault(*id, name) != nullptr) {
            continue;
        }
        if (named.find(name) == named.end()) {
            fail(expr.pos, "missing field '" + name + "'");
            return errorRef();
        }
    }
    // Honest `new` (Unified Standard 5): a paren or `new` form allocates on
    // the heap and answers `*T`. A bare literal is the stack value.
    // `try_new` answers Result<*T, AllocError> and only constructs when the
    // allocation operator succeeds.
    if (expr.tryNew) {
        if (!requireAllocOperator(true, expr.pos)) {
            return errorRef();
        }
        return checkResultOf(TypeRef{ctx_.pointer(target.type)}, expr.pos);
    }
    if (expr.heap) {
        if (!requireAllocOperator(true, expr.pos)) {
            return errorRef();
        }
        return TypeRef{ctx_.pointer(target.type)};
    }
    return target;
}

TypeRef Checker::checkResultOf(TypeRef value, Position pos) {
    std::vector<TypeRef> args{value, resolver_.coreAllocError()};
    std::optional<DeclId> spec =
        resolver_.instantiate(resolver_.coreResultTemplate(), args, pos);
    if (!spec.has_value()) {
        return errorRef();
    }
    return TypeRef{ctx_.nominal(Kind::Struct, *spec)};
}

bool Checker::requireAllocOperator(bool allocation, Position pos) {
    const char* name = allocation ? "operator_new" : "operator_delete";
    const resolve::Decl* decl = resolver_.lookup(moduleScope_, name);
    if (decl == nullptr || decl->kind != resolve::DeclKind::Func) {
        fail(pos, allocation
                     ? "allocation needs a global operator new(size: i64, align: i64) -> *void"
                     : "deletion needs a global operator delete(ptr: *void) -> void");
        return false;
    }
    return true;
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
    if (!fieldVisible(id, init.name, init.value->pos)) {
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

bool Checker::constAggregateInit(const Expr& init) {
    if (const auto* made = dynamic_cast<const NewArray*>(&init)) {
        for (const auto& element : made->inits) {
            if (!constAggregateInit(*element)) {
                return false;
            }
        }
        return !made->inits.empty();
    }
    if (const auto* lit = dynamic_cast<const ArrayLit*>(&init)) {
        for (const auto& element : lit->inits) {
            if (!constAggregateInit(*element)) {
                return false;
            }
        }
        return !lit->inits.empty();
    }
    if (const auto* made = dynamic_cast<const NewStruct*>(&init)) {
        // A struct or class value with every field constant folds to
        // image bytes. Coverage is the checker's own rule elsewhere, so
        // const-ness alone answers here; a paren-form `new C(args)`
        // runs its constructor and is a different node that falls to
        // the fold below, which fails it loudly.
        for (const auto& field : made->fields) {
            if (!constAggregateInit(*field.value)) {
                return false;
            }
        }
        return true;
    }
    if (const auto* cast = dynamic_cast<const Cast*>(&init)) {
        // `nilPtr as I` is constant (sixteen zero bytes in the image):
        // the declaration site accepts the same null spelling for a
        // whole interface global, so array elements accept it too.
        if (dynamic_cast<const NilPtr*>(cast->expr.get()) != nullptr) {
            TypeRef spelled = resolver_.resolveType(cast->target.get(),
                                                     resolver_.scopeOf(module_));
            if (!spelled.isError() &&
                spelled.type->kind() == Kind::Interface) {
                return true;
            }
        }
    }
    // A bare nil pointer is the zero word whatever it fills: the emission
    // writes the field's own width of zeros, so const-ness needs no type.
    if (dynamic_cast<const NilPtr*>(&init) != nullptr) {
        return true;
    }
    std::unordered_set<std::string> active;
    if (evalConstInt(init, *env_, active).ok) {
        return true;
    }
    // A float element folds to a double the same way: const-ness is
    // folding to a number, and the emission already rounds float
    // elements at width. Anything else stays refused below.
    return evalConstFloat(init, *env_, active).ok;
}

TypeRef Checker::checkNewArray(NewArray& expr) {
    if (expr.heap || expr.tryNew) {
        fail(expr.pos, "array allocation is not supported yet");
        return errorRef();
    }
    TypeRef element = resolveHere(expr.element.get());
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
    TypeRef arr = TypeRef{ctx_.array(element.type, count.value)};
    if (expr.heap) {
        return TypeRef{ctx_.pointer(arr.type)};
    }
    return arr;
}

// A bare array literal `[a, b, c]` (or spread `[0, ...]`) answers the
// declared array type — never a heap pointer. The declared type owns the
// element type and the count; the literal only supplies the values.
TypeRef Checker::checkArrayLit(ArrayLit& expr, TypeRef declared) {
    if (declared.isError() || declared.type->kind() != Kind::Array) {
        fail(expr.pos, "an array literal needs a declared array type");
        return errorRef();
    }
    TypeRef element{declared.type->element()};
    if (element.isError()) {
        return errorRef();
    }
    std::optional<int64_t> declared_count = declared.type->length();
    if (!declared_count.has_value()) {
        fail(expr.pos, "an array literal needs a fixed declared length");
        return errorRef();
    }
    int64_t count = *declared_count;
    const int64_t written = static_cast<int64_t>(expr.inits.size());
    if (expr.repeat && written != kOneElement) {
        fail(expr.pos, "a spread fills from exactly one element");
        return errorRef();
    }
    if (!expr.repeat && written != count) {
        fail(expr.pos, "the element count must match the array type");
        return errorRef();
    }
    if (expr.repeat) {
        // The spread's filler still checks (recording its type for the
        // lowerer); the compatibility check runs per-element in the
        // lowerer, since the scalar zero is not an element-typed value.
        checkExpr(expr.inits[0].get(), element);
        return declared;
    }
    for (auto& init : expr.inits) {
        TypeRef value = checkExpr(init.get(), element);
        checkCompatible(init->pos, init.get(), value, element);
    }
    return declared;
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
    // A null converts to the null closure value, like nilPtr as func spells.
    if (dynamic_cast<const NilPtr*>(fromExpr) != nullptr &&
        to.type->kind() == Kind::Closure) {
        return true;
    }
    // A string literal converts to a byte pointer: the literal
    // materializes its own bytes, so word zero is their address. Only
    // the literal converts — a string variable beside a *u8 stays a
    // mismatch, spelled `&s[0]`. A char is one byte, so *char rides the
    // same rodata-lifetime justification as *u8 (FEATURES §1.4). *void
    // accepts any address, so the literal's word zero converts there too.
    if (dynamic_cast<const StringLit*>(fromExpr) != nullptr &&
        to.type->kind() == Kind::Pointer) {
        TypeRef pointee{to.type->pointee()};
        if (pointee.type->kind() == Kind::Void) {
            return true;
        }
        if (pointee.type->kind() == Kind::Basic) {
            BasicKind basic = *pointee.type->basic();
            if (basic == BasicKind::U8 || basic == BasicKind::I8 ||
                basic == BasicKind::Char) {
                return true;
            }
        }
    }
    // A wide literal converts to a u16 pointer: the literal materializes
    // its own units in rodata, so word zero is their address. Only the
    // literal converts — a [u16; N] variable beside a *u16 stays a
    // mismatch, spelled `&a[0]`. *void accepts any address, so the
    // literal's word zero converts there too.
    if (dynamic_cast<const WideLit*>(fromExpr) != nullptr &&
        to.type->kind() == Kind::Pointer) {
        TypeRef pointee{to.type->pointee()};
        if (pointee.type->kind() == Kind::Void) {
            return true;
        }
        if (pointee.type->kind() == Kind::Basic &&
            *pointee.type->basic() == BasicKind::U16) {
            return true;
        }
    }
    // A closure converts to a void pointer by address: the literal
    // materializes its own pair, so word zero is the pair's address. Only
    // a proven closure converts — a raw code address beside a *void stays
    // a mismatch, laundering refuses. *u8 rides the same address
    // justification (the pair's bytes); anything else beside a *void from
    // a closure stays unspelled.
    auto isVoidPtr = [](const TypeRef& t) -> bool {
        return t.type->kind() == Kind::Pointer &&
               t.type->pointee() != nullptr &&
               t.type->pointee()->kind() == Kind::Void;
    };
    if (from.type->kind() == Kind::Closure && isVoidPtr(to)) {
        if (provOf(fromExpr) != Prov::Closure) {
            fail(pos, "only a closure converts to *void");
            return false;
        }
        return true;
    }
    // Deeper carriers (*void storage through another pointer) answer the
    // same single address word the cast gate above shapes.
    auto isDeepVoidPtr = [](const TypeRef& t) -> bool {
        if (t.type->kind() == Kind::Pointer && t.type->pointee() != nullptr &&
            t.type->pointee()->kind() == Kind::Pointer &&
            t.type->pointee()->pointee() != nullptr) {
            return t.type->pointee()->pointee()->kind() == Kind::Void;
        }
        return false;
    };
    if (from.type->kind() == Kind::Closure && isDeepVoidPtr(to)) {
        if (provOf(fromExpr) != Prov::Closure) {
            fail(pos, "only a closure converts to *void");
            return false;
        }
        return true;
    }
    // A proven-closure *void converts back to a closure spelling: the
    // address names the pair the store observed. An unproven carrier
    // refuses — reading a pair through it would be a guess. A null
    // converts to the null closure value, like nilPtr as func spells.
    if (isVoidPtr(from) && to.type->kind() == Kind::Closure) {
        Prov proven = provOf(fromExpr);
        if (proven != Prov::Closure && proven != Prov::Neutral) {
            fail(pos, "this *void is not proven to hold a closure");
            return false;
        }
        return true;
    }
    // A proven-closure *void adopts the fn spelling: the declaration
    // carries the closure representation from here on.
    if (isVoidPtr(from) && to.type->kind() == Kind::FunctionPtr) {
        if (provOf(fromExpr) != Prov::Closure) {
            fail(pos, "this *void is not proven to hold a closure");
            return false;
        }
        return true;
    }
    // A slice copies into an array of the same element type: the copy
    // checks the length where it happens, so a mismatch traps instead of
    // truncating (SEMANTICS §5, slice-to-array row). Interned types make
    // this a pointer comparison — same spelling is the same type.
    if (from.type->kind() == Kind::Slice && to.type->kind() == Kind::Array &&
        from.type->element() == to.type->element()) {
        return true;
    }
    // A compile-time integer constant converts to any integer type whose
    // range contains it: a literal, a named constant, a size, an alignment
    // and arithmetic over them bind by value (Standard 11's
    // `var node_bytes: u64 = sizeof(Node);` needs exactly this). A value
    // that is not constant converts nothing — an `int` variable beside a u8
    // stays the mismatch it is.
    if (from.type->kind() == Kind::Basic &&
        basicIsInteger(*from.type->basic()) &&
        to.type->kind() == Kind::Basic && basicIsInteger(*to.type->basic())) {
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
    // An enum value flows to any integer target as its underlying value
    // (TYPES §2.6); the reverse needs an explicit `as`, so integers never
    // land in an enum by accident. The width is the target's: an
    // enumerator fits every integer spelling.
    if (from.type->kind() == Kind::Enum && to.type->kind() == Kind::Basic &&
        basicIsInteger(*to.type->basic())) {
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
    // A class reference converts to an interface its class implements: the
    // reference becomes the pair's data word (TYPES.md §4.3). Bare Class
    // stays above — only a reference is a value a variable can hold.
    if (to.type->kind() == Kind::Interface && from.type->kind() == Kind::Pointer &&
        from.type->pointee() != nullptr &&
        from.type->pointee()->kind() == Kind::Class) {
        std::optional<DeclId> concrete = from.type->pointee()->decl();
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
    // A closure casts to a void pointer by address (provenance rides the
    // type: only closure-typed spellings arrive here). A byte view spells
    // the same address through *u8. Deeper carriers stay refused until
    // lowering shapes them.
    auto isU8Ptr = [](const Type* t) -> bool {
        return t != nullptr && t->kind() == Kind::Basic &&
               t->basic().has_value() && *t->basic() == BasicKind::U8;
    };
    if (a == Kind::Closure && b == Kind::Pointer &&
        to.type->pointee() != nullptr &&
        (to.type->pointee()->kind() == Kind::Void ||
         isU8Ptr(to.type->pointee()))) {
        return true;
    }
    // An enum casts to and from any integer width: the value is the
    // underlying i32 widened or narrowed like any integer conversion,
    // and the membership check the int-to-enum lowering emits guards
    // the way in (TYPES §2.6). Same-width pairs already passed through
    // the size rule below; this row owns the wide ones.
    auto isIntKind = [](Kind k, const Type* t) -> bool {
        return k == Kind::Basic && basicIsInteger(*t->basic());
    };
    if ((a == Kind::Enum && isIntKind(b, to.type)) ||
        (isIntKind(a, from.type) && b == Kind::Enum)) {
        return true;
    }
    // An interface reads back as a concrete struct, checked at runtime.
    // A bare class is not a value type (TYPES §4.2: only `*C` is), so it
    // is refused here; the pointer-to-class row below covers `*C`.
    if (a == Kind::Interface && b == Kind::Struct) {
        return true;
    }
    // An interface reads back as a class reference it holds, checked at
    // runtime like the concrete types above: the data word already is
    // the reference, so the check is the whole conversion.
    if (a == Kind::Interface && b == Kind::Pointer &&
        to.type->pointee() != nullptr &&
        to.type->pointee()->kind() == Kind::Class) {
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
