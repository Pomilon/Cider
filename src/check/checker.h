// checker.h — the body walk. LAYERS.md §3.4.
//
// The second role inside the one shared traversal: the resolver wrote Decls,
// Scopes and the name facts; the checker writes each expression's Type and
// checks it in place. Recording an expression's type and checking it are the
// same step — there is no moment at which types are recorded but unchecked.
//
// This is the one visitor class in src/, and gates T5/L1 count it. Every other
// walk in the compiler goes through these methods or does not happen.

#ifndef CIDER_CHECK_CHECKER_H
#define CIDER_CHECK_CHECKER_H

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast/ast.h"
#include "eval/eval.h"
#include "resolve/resolver.h"
#include "target/target.h"

namespace cider {

// Forward declaration in the real namespace. Declaring it inside `check` would
// make `lower::Lowerer` resolve to check::lower from every member below, and
// that type is only ever a pointer here.
namespace lower {
class Lowerer;
}

namespace check {

// Where lowering listens, defined here rather than in the lowering header so
// the checker does not depend on the layer that calls it: a static library
// cannot include its own caller. Lowering implements this; the checker emits
// through it while it walks.
// One by-value capture of a closure literal: the name as the body spells it
// and the type the enclosing scope gives it. The checker discovers these
// while it walks the body; the lowerer lays them out behind the captures
// word in first-read order.
struct Captured {
    std::string name;
    TypeRef type;
    Position pos;
};

class LowerSink {
public:
    virtual ~LowerSink() = default;
    virtual void onProgram(Program& n) { (void)n; }
    virtual void onBlockEnter(Block& n) { (void)n; }
    virtual void onBlockExit(Block& n) { (void)n; }
    virtual void onFuncDecl(FuncDecl& n) { (void)n; }
    // 7.10: one module-level finalizer block. The sink lowers it into the
    // generated finalizer function.
    virtual void onFiniDecl(FiniDecl& n) { (void)n; }
    // One instantiated generic function body, emitted under its own link
    // name. The spelling holds parameter names and the template body; the
    // signature holds the substituted parameter and return types. Fires
    // from the checker's spec walk, never from the program walk.
    virtual void onSpecFunc(DeclId spec, const FuncDecl& spelling,
                            const Signature& sig) {
        (void)spec;
        (void)spelling;
        (void)sig;
    }
    // A method or constructor body, emitted as its own function. The owner
    // names the mangling; the signature holds the declared parameters and
    // self arrives separately because no source spelling names it.
    virtual void onMethodDecl(const std::string& owner, const std::string& name,
                              const std::vector<Param>& params, const Signature& sig,
                              TypeRef self, Block& body) {
        (void)owner;
        (void)name;
        (void)params;
        (void)sig;
        (void)self;
        (void)body;
    }
    virtual void onVarDecl(VarDecl& n) { (void)n; }
    // An expression statement is evaluated for effect and its value dropped:
    // calls and assignments live here, and without this hook they vanish.
    virtual void onExprStmt(ExprStmt& n) { (void)n; }
    virtual void onReturn(Return& n) { (void)n; }
    // `break` and `continue` leave the innermost loop: checked to be inside
    // one, then lowered as jumps to its exit and its head. Without this hook
    // they vanish and the loop runs to completion.
    virtual void onBreak(Break& n) { (void)n; }
    virtual void onContinue(Continue& n) { (void)n; }
    // A defer statement, after its body checked in the enclosing scope.
    // The lowerer registers the body for every exit of the innermost
    // open scope; nothing runs here.
    virtual void onDeferStmt(DeferStmt& n) { (void)n; }
    // The checker owns the exception shape; these hooks let lowering emit
    // the handler around the already-checked body and catches.
    virtual void onTryEnter(TryStmt& n) { (void)n; }
    virtual void onTryBodyDone(TryStmt& n) { (void)n; }
    virtual void onCatchEnter(TryStmt& n, CatchClause& c) {
        (void)n;
        (void)c;
    }
    virtual void onCatchExit(TryStmt& n, CatchClause& c) {
        (void)n;
        (void)c;
    }
    virtual void onTryExit(TryStmt& n) { (void)n; }
    virtual void onThrowStmt(ThrowStmt& n) { (void)n; }
    virtual void onIf(IfStmt& n) { (void)n; }
    // A match arm, after its pattern validated against the scrutinee type
    // and exhaustiveness decided (a default arm, or both bool constants).
    // Without this hook the first arm would run unconditionally.
    virtual void onMatch(MatchStmt& n) { (void)n; }
    virtual void onWhile(WhileStmt& n) { (void)n; }
    virtual void onFor(ForStmt& n) { (void)n; }
    virtual void onForIn(ForInStmt& n) { (void)n; }
    // `delete p;` frees a class object. Checked first (the pointer type is
    // known), then lowered here.
    virtual void onDelete(DeleteStmt& n) { (void)n; }
    // An asm block, after every `{placeholder}` name resolved against scope.
    virtual void onAsmStmt(AsmStmt& n) { (void)n; }
    // A closure literal, after its body checked. The captures are the
    // enclosing locals the body reads, in first-read order; the signature
    // holds the declared parameters. The lowerer emits the hidden function
    // and answers the two-word value when it lowers the literal.
    virtual void onFuncLit(FuncLit& lit, const std::vector<Captured>& captures,
                           const Signature& sig) {
        (void)lit;
        (void)captures;
        (void)sig;
    }
};

struct CheckError {
    Position pos;
    std::string message;
};

struct Local {
    TypeRef type;
    bool isMut = false;
    Position pos{"", 1, 1};
};

struct LocalScope {
    const LocalScope* parent = nullptr;
    std::unordered_map<std::string, Local> names;
};

// Provenance of address-carrying storage (T10): whether a pointer-sized
// value known to hold a closure address may cast back to a closure
// spelling. Raw accepts no such cast; Neutral (nilPtr) casts to the null
// closure value. Anything unproven is Raw: laundering refuses loudly.
enum class Prov { Raw, Closure, Neutral };

class Checker : public ASTVisitor {
public:
    Checker(TypeContext& ctx, resolve::Resolver& resolver,
            target::Kind target = target::Kind::Native);

    // Lowering is driven from this walk, not from a second one: LAYERS.md
    // §3.5.1 says lowering queries the walk's result rather than re-walking the
    // AST, which is also why this is the only visitor class in src/ (T5/L1).
    // Null until lowerModule attaches one.
    void setSink(LowerSink* sink) { sink_ = sink; }
    LowerSink* sink() const { return sink_; }

    // Checks every body of one module. The module must be resolved first;
    // unknown names here are a checker bug, not a program error, and are
    // reported as such.
    bool checkProgram(Program& program, const std::string& module);
    // Whether the validator proved a match arm can never match (FEATURES
    // §8.4 dead-pattern rule). The lowerer chains straight through those.
    bool isDeadPattern(const Pattern* pat) const;
    // The module the last checkProgram walked: the lowerer names
    // definitions by the module being walked, not the entry.
    const std::string& module() const { return module_; }

    TypeRef typeOf(const Node* node) const;
    const std::vector<CheckError>& errors() const { return errors_; }
    size_t errorCount() const { return errors_.size(); }
    // The specialization a generic call instantiated, by call site and
    // recording context. Empty context while walking top-level code;
    // otherwise the spec whose body walk recorded it. Template bodies are
    // shared across specs, so one site records one spec per context; the
    // lowerer resolves each site under its own context instead of
    // trusting the last write (which order-dependent overwrites lose).
    std::optional<DeclId> specOf(const Call* call,
                                 const std::optional<DeclId>& ctx) const;
    // The operand type a sizeof spelled, by node. Recorded during checking
    // with the active substitution applied, so spec emissions read the
    // concrete type instead of re-resolving past type parameters.
    TypeRef sizeofOperand(const SizeofType* node) const;
    TypeRef sizeofExprOperand(const SizeofExpr* node) const;
    TypeRef alignofOperand(const AlignofType* node) const;
    // Every recorded generic call site and its spec, for the lowerer's
    // emission worklist. Checking already computed the transitive closure:
    // detached body walks still instantiate nested calls.
    // Name provenance for the lowerer (call shapes): the same answer
    // provOf gives an identifier, without needing its node.
    Prov provOfName(const std::string& name) const;
    // Whether the checker saw this expression as a volatile place: a
    // deref through a `*volatile T` name. The lowerer marks the loads and
    // stores it emits through the place with this answer.
    bool placeIsVolatile(const Expr* expr) const;
    // The type a typed catch binds. A bare catch has no entry; lowering
    // keys that case on CatchClause::type being null.
    TypeRef catchType(const CatchClause& clause) const;
    // Names a declaration `*volatile T` so derefs through it are volatile
    // places; silent for every other spelling.
    void noteVolatilePointer(const std::string& name, const TypeExpr* type);
    const std::map<std::pair<const Call*, std::string>, DeclId>& callSpecs() const {
        return callSpecs_;
    }
    // Re-walks one instantiated generic function body with the sink
    // attached, so the lowerer emits it. Setup repeats the checking walk
    // (type-param scope, substituted binds, return type); the body was
    // already proven clean, so the deterministic re-walk reports nothing
    // new. No-op when the id names no function specialization.
    void walkSpecForEmit(DeclId spec);
    // Collects call-site argument provenance for every function before
    // any body checks (public: the driver runs it after resolving and
    // before lowering every module).
    void prescanProvenance(const std::vector<std::string>& modules);

    // SEMANTICS.md §2.3: the common type of two operands, stated as a type.
    // Public so lowering emits a comparison in the same domain the checker
    // proved — that domain must not be re-decided downstream.
    TypeRef commonType(TypeRef lhs, TypeRef rhs, Position pos);

    // Declarations and statements with rules.
    void visitProgram(Program& n) override;
    void visitFuncDecl(FuncDecl& n) override;
    void visitStructDecl(StructDecl& n) override;
    void visitClassDecl(ClassDecl& n) override;
    void visitUnionDecl(UnionDecl& n) override;
    void visitSizeofExpr(SizeofExpr& n) override;
    void visitModuleAsm(ModuleAsm& n) override;
    void visitVarDecl(VarDecl& n) override;
    void visitBlock(Block& n) override;
    void visitIfStmt(IfStmt& n) override;
    void visitWhileStmt(WhileStmt& n) override;
    void visitForStmt(ForStmt& n) override;
    void visitForInStmt(ForInStmt& n) override;
    void visitExprStmt(ExprStmt& n) override;
    void visitReturn(Return& n) override;
    void visitBreak(Break& n) override;
    void visitContinue(Continue& n) override;
    void visitMatchStmt(MatchStmt& n) override;
    void visitDeleteStmt(DeleteStmt& n) override;
    void visitAsmStmt(AsmStmt& n) override;
    void visitThrowStmt(ThrowStmt& n) override;
    void visitTryStmt(TryStmt& n) override;
    void visitDeferStmt(DeferStmt& n) override;
    void visitFiniDecl(FiniDecl& n) override;

    // Every expression records its type.
    void visitIntLit(IntLit& n) override;
    void visitFloatLit(FloatLit& n) override;
    void visitCharLit(CharLit& n) override;
    void visitStringLit(StringLit& n) override;
    void visitBoolLit(BoolLit& n) override;
    void visitNilPtr(NilPtr& n) override;
    void visitUninitExpr(UninitExpr& n) override;
    void visitSuper(Super& n) override;
    void visitSuperInit(SuperInit& n) override;
    void visitIdent(Ident& n) override;
    void visitBinary(Binary& n) override;
    void visitUnary(Unary& n) override;
    void visitCast(Cast& n) override;
    void visitTernary(Ternary& n) override;
    void visitIncrDecr(IncrDecr& n) override;
    void visitCall(Call& n) override;
    void visitField(Field& n) override;
    void visitIndex(Index& n) override;
    void visitSliceExpr(SliceExpr& n) override;
    void visitNewStruct(NewStruct& n) override;
    void visitNewClass(NewClass& n) override;
    void visitNewArray(NewArray& n) override;
    void visitArrayLit(ArrayLit& n) override;
    void visitAssign(Assign& n) override;
    void visitSizeofType(SizeofType& n) override;
    void visitAlignofType(AlignofType& n) override;
    void visitFuncLit(FuncLit& n) override;
    TypeRef checkFuncLit(FuncLit& lit);

private:
    void fail(Position pos, const std::string& message);
    void record(const Node* node, TypeRef type);

    // Scopes: locals first, then the module scope. Shadowing is allowed and
    // silent; an inner binding hides the outer one.
    const LocalScope* pushScope();
    void popScope();
    bool bindLocal(const std::string& name, TypeRef type, bool isMut, Position pos);
    const Local* findLocal(const std::string& name) const;
    // The scope a name resolves in, or null. Closure capture discovery asks
    // where a read binds rather than only what it binds to.
    const LocalScope* scopeOf(const std::string& name) const;

    // Expressions. `expected` is the type the context demands, or a wildcard
    // when the expression must speak for itself (literals default from it).
    TypeRef checkExpr(Expr* expr, TypeRef expected);
    TypeRef checkLiteralInt(const IntLit& lit, TypeRef expected, Position pos);
    TypeRef checkLiteralFloat(const FloatLit& lit, TypeRef expected, Position pos);
    TypeRef checkIdent(Ident& expr);
    // The representation an expression carries for closure round trips:
    // literals and tracked storage answer above, everything else is Raw.
    Prov provOf(const Expr* expr);
    // Records a local's provenance in the innermost scope; shadowing
    // scopes hide it again on pop, like the names themselves.
    void recordFlow(const std::string& name, Prov prov);
    // Names one syntactic storage chain for place flow, or empty when no
    // chain proves (calls, literals beyond nil, unkeyable shapes).
    std::string placeKey(const Expr* expr);
    bool isPlace(const Expr* expr) const;
    bool isInteger(TypeRef type) const;
    TypeRef checkBinary(Binary& expr, TypeRef expected);
    void checkShiftDiv(const Binary& expr, TypeRef lhs, TypeRef rhs);
    TypeRef checkCall(Call& expr);
    TypeRef checkField(Field& expr);
    // 4.8.4: a private field is visible strictly within the module that
    // declares its class or struct. Fails and answers false elsewhere.
    bool fieldVisible(DeclId id, const std::string& name, Position pos);
    // 4.8.4: a private method is visible strictly within the module that
    // declares its type. Fails and answers false elsewhere.
    bool methodVisible(DeclId id, const std::string& name, Position pos);
    // v2 CIDER-BUG-049: every field default is checked against its field
    // type in the declaring module, so the lowerer can replay it.
    void checkFieldDefaults(DeclId id, const std::vector<FieldDecl>& fields);
    TypeRef checkNewStruct(NewStruct& expr);
    TypeRef checkNewUnion(NewStruct& expr, TypeRef target, DeclId id);
    TypeRef checkUnionHas(Call& expr, const Field& callee, TypeRef object);
    TypeRef checkNewArray(NewArray& expr);
    TypeRef checkArrayLit(ArrayLit& expr, TypeRef declared);
    // A repeat filler that is the scalar zero over an aggregate element:
    // `new [[u8; 2]; 3] { [0, ...] }` zeroes every scalar, so the filler
    // never checks against the element type.
    bool isZeroRepeat(const Expr& filler, TypeRef element);
    // Every scalar leaf of an aggregate construction folds to an integer:
    // `new [u8; 3] {1, 2, 3}` names every image byte, so a global may hold
    // it with no startup code to order (FEATURES §8.3). The type checking
    // below still enforces every element, so this only answers constness.
    bool constAggregateInit(const Expr& init);
    TypeRef checkTernary(Ternary& expr, TypeRef expected);
    void checkMethods(DeclId id, Kind kind, std::vector<MethodDecl>& methods);
    // A redeclaration of a base method is an override when the base's slot
    // is virtual — with a compatible signature — and an error otherwise
    // (SEMANTICS 10: neither an override nor a shadow exists).
    void checkOverrides(const resolve::Decl& self, const ClassDecl& n);
    void checkCtor(DeclId id, Kind kind, FuncDecl* ctor, bool isInit);
    // BUG-274: a by-value cycle has no finite size, so the declaration
    // itself is refused. Only inline layouts embed — struct, union, class
    // and array — while pointers, interfaces, strings, slices and scalars
    // are fixed-size and stop the walk (`*A` stays legal).
    void checkNoSelfEmbed(DeclId self, const std::string& kind,
                          const std::string& declName,
                          const std::vector<FieldDecl>& fields,
                          const std::string& partWord);
    bool typeEmbedsSelf(const Type* type, DeclId self,
                        std::set<uint64_t>& seen) const;
    // TYPES.md §9 definite assignment for init bodies: the fields assigned
    // on every path through a statement. Straight lines accumulate
    // `(*self).f = ...`; branches intersect arms; loops contribute nothing
    // (they may run zero times); value-opacity — calls inside values and
    // conditions — bails the check but keeps the recorded stores, which
    // stand on their own syntax. Statement-position unknowns — a call
    // statement, a non-self store, asm, throw, try, defer, anything
    // unlisted — set unknownFlow instead: they may assign fields
    // invisibly, so an init containing one is refused, never assumed
    // (TYPES.md §9.2: unprovable is rejected, not accepted).
    struct InitAssign {
        std::unordered_set<std::string> assigned;
        bool bailed = false;
        bool unknownFlow = false;
    };
    InitAssign assignedThrough(const Stmt* stmt, InitAssign state,
                               const std::vector<std::string>& fields);
    bool initExprOpaque(const Expr* expr);
    bool isSelfFieldStore(const Expr* target, std::string& field);
    void requireInitAssigned(Position pos, const std::vector<std::string>& fields,
                             const InitAssign& state);
    void checkInitAssign(DeclId id, const FuncDecl& ctor);
    void bindPattern(const Pattern& pat, TypeRef scrutinee);
    void bindSubPattern(const Pattern& pat, TypeRef type);
    // Match validation (FEATURES §8.4): a pattern Name that resolves to a
    // value in scope compares by that type's == (the Erlang rule); only an
    // unresolvable name binds fresh. Classification never binds, so both
    // the pure-mode walk and the sink-mode validation share it.
    bool resolveMatchValue(const std::string& name, TypeRef& bound);
    bool matchComparable(TypeRef scrutinee, TypeRef bound);
    bool checkMatchConst(const Pattern& pat, TypeRef scrutinee);
    bool checkMatchRange(const Pattern& pat, TypeRef scrutinee);
    bool checkMatchPattern(const Pattern& pat, TypeRef scrutinee);
    bool checkMatchType(const Pattern& pat, TypeRef scrutinee);
    bool checkMatchMember(const Pattern& pat, TypeRef scrutinee);
    // Resolves a member pattern's tag to its union declaration, or nothing
    // when the tag is unknown or not a union. Never fails itself: the
    // classifier above already diagnosed those shapes, so the
    // exhaustiveness walk reuses the answer without doubling the error.
    std::optional<DeclId> matchMemberUnion(const Pattern& pat);
    std::optional<DeclId> matchMemberEnum(const Pattern& pat);
    void bindMatchFresh(const Pattern& pat, TypeRef scrutinee);
    // Binds one arm's every alternative: the primary, then each `|` rest.
    // An arm with alternatives binds no fresh names — the body cannot know
    // which alternative's value it would hold — so any alternative that
    // grows the scope fails here instead of binding half a meaning.
    void bindMatchArm(MatchArm& arm, TypeRef scrutinee);
    // Checks the arm's `if` guard in the arm's scope, after the pattern's
    // binds, so it reads bound names like the body does. A missing guard
    // checks nothing; a present one must be a bool, like any condition.
    void checkMatchGuard(MatchArm& arm);
    // Counts the names bound in the innermost scope: the before/after
    // answers the alternation rule above without naming any one shape.
    size_t localCount() const;

    // SEMANTICS.md §2.3: the common type of two operands, stated as a type.
    // (Public; declared beside typeOf. Lowering queries it for comparisons.)
    // Resolves a spelling in the current type scope, then substitutes the
    // active instantiation through it. Outside a generic body this is
    // resolveType; inside one, `T` and `Box<T>` answer concrete types.
    TypeRef resolveHere(const TypeExpr* spelling);
    // Replaces TypeParam payloads of one template with its arguments.
    // Anything else passes through unchanged (same pointer, no rebuild).
    TypeRef substDeep(TypeRef type);
    // SEMANTICS.md §5.4: the six implicit conversions, and nothing else.
    bool checkCompatible(Position pos, const Expr* fromExpr, TypeRef from, TypeRef to);
    // Checks a generic call: resolves arguments, instantiates, checks them
    // against the substituted signature, and walks the template body once
    // with the arguments substituted. Answers the substituted return type.
    TypeRef checkGenericCall(Call& expr, const resolve::Decl* decl);
    // Walks one specialized method body with its arguments substituted.
    // No-op when this specialization already walked. Generic init bodies
    // wait with the rest of init checking: construction checks arity and
    // types through the tables, which is what this slice promises.
    void walkMethodInst(DeclId spec, DeclId tmpl, const std::string& method);
    // The template AST behind a function name, for instantiation walks.
    // Keyed by module and name: two modules may template the same spelling.
    const FuncDecl* funcSpelling(const std::string& module,
                                 const std::string& name) const;
    bool implementsAll(DeclId concrete, DeclId iface, Position pos);
    bool isDerivedFrom(DeclId derived, DeclId base) const;
    bool castRowOk(TypeRef from, TypeRef to) const;

    // Spellings by module and name: every checked module registers here,
    // and a bare name would collide across modules. Non-owning; the
    // Programs outlive every use.
    std::unordered_map<std::string, const FuncDecl*> funcs_;
    std::unordered_map<std::string, const MethodDecl*> methods_;
    std::unordered_map<std::string, const StructDecl*> structs_;
    // The active generic instantiation, if any. Single frame: the AST has no
    // nested generic functions, so one is enough.
    struct ActiveInst {
        DeclId tmpl;
        std::vector<TypeRef> args;
        std::vector<std::string> names;
    };
    std::optional<ActiveInst> active_;
    // Instantiations already body-walked, by specialization key. A recursive
    // call into the same instantiation checks against its signature without
    // re-walking; anything else would loop the compiler on recursive code.
    std::unordered_set<std::string> walked_;
    // Generic call sites and the specialization each instantiated, keyed
    // by site and recording context (the spec whose body walk recorded
    // it, or empty at top level). One site keeps one spec per context so
    // order-dependent overwrites cannot lose specializations.
    std::map<std::pair<const Call*, std::string>, DeclId> callSpecs_;
    // The spec whose body is walking, if any: the recording context for
    // nested generic calls. Saved and restored like the active
    // instantiation, for the same nesting reasons.
    std::optional<DeclId> walkSpec_;
    // Sizeof operand types by node, recorded with substitution applied.
    std::unordered_map<const SizeofType*, TypeRef> sizeofOperands_;
    std::unordered_map<const SizeofExpr*, TypeRef> sizeofExprOperands_;
    std::unordered_map<const AlignofType*, TypeRef> alignofOperands_;
    // Types resolve here rather than at module scope during generic walks,
    // where the template's parameters are bound.
    const resolve::Scope* typeScope_ = nullptr;

    TypeContext& ctx_;
    resolve::Resolver& resolver_;
    target::Kind target_;
    const resolve::Scope* moduleScope_ = nullptr;
    std::string module_;
    // Held by pointer: EvalEnv holds a reference to the TypeContext, and a
    // class with a reference member cannot be assigned. One env per program,
    // built in checkProgram once the module scope is known.
    std::unique_ptr<EvalEnv> env_;
    const LocalScope* current_ = nullptr;
    std::vector<std::unique_ptr<LocalScope>> scopes_;
    // Provenance per local scope, parallel to the scopes above: pushed
    // and popped with them, so shadowing hides outer entries the way it
    // hides names. Globals persist in their own table, cleared on
    // function entry (a read sees only stores from its own function:
    // conservative across functions, exact within one).
    std::vector<std::unordered_map<std::string, Prov>> flowStack_;
    std::unordered_map<std::string, Prov> globalProv_;
    // Provenance per place (fields, elements, derefs), parallel to the
    // scopes above: same push/pop discipline, so shadowing behaves. Keys
    // name the syntactic chain (`s.v` vs `(*ps).v` never merge: aliasing
    // chains stay unproven, documented); unkeyable shapes read Raw.
    std::vector<std::unordered_map<std::string, Prov>> placeFlow_;
    // Names declared as `*volatile T`, per scope: a deref through one is a
    // volatile access (FEATURES §1.1c), and the lowerer asks which deref
    // expressions the checker saw that way.
    std::vector<std::unordered_set<std::string>> volatilePtrs_;
    std::unordered_set<const Expr*> volatilePlaces_;
    // Global provenance inside the prescan only: seeded from declaration
    // initializers, updated by stores in module order. Best-effort across
    // modules (reads see earlier writes); exact within one function.
    std::unordered_map<std::string, Prov> prescanGlobals_;
    // Parameter provenance from the whole-program prescan below: a *void
    // parameter is proven when every known call site passes a closure.
    // Keyed by module, function and index; absent means unproven.
    struct ParamSite {
        bool seen = false;
        bool allClosure = true;
        bool allNeutral = true;
    };
    std::unordered_map<std::string, ParamSite> paramSites_;
    // Set while a prescan round promotes any site.
    bool prescanMoved_ = false;
    // The module a prescan walk belongs to, for call-site resolution.
    std::string prescanModule_;
    // Reads the table above with the corpus rule: unanimous closure (or
    // neutrality) proves, anything else (including no sites) refuses.
    Prov paramProv(const std::string& key) const;
    // One prescan round over all functions; true when any table moved.
    bool prescanRound(const std::vector<std::string>& modules);
    // Walks one function body for call sites, with function-local flow.
    void prescanFunction(const std::string& mod, const FuncDecl& fn);
    // Statement and expression walkers for the prescan: statements update
    // the flow maps, every expression is scanned for calls.
    void prescanStmt(const Stmt* stmt,
                     std::vector<std::unordered_map<std::string, Prov>>& scopes,
                     const std::unordered_map<std::string, bool>& closureParams);
    void prescanExpr(const Expr* expr,
                     std::vector<std::unordered_map<std::string, Prov>>& scopes,
                     const std::unordered_map<std::string, bool>& closureParams);
    // Merges a speculative copy back: only entries the walk left exactly
    // as found survive, so zero-trip paths never prove.
    void mergePrescan(std::unordered_map<std::string, Prov>& into,
                      const std::unordered_map<std::string, Prov>& before);
    // Syntactic provenance inside the prescan (no checker state): literals
    // answer directly, names read the flow, casts preserve, calls and
    // places read Raw (places arrive with the next slice).
    Prov prescanArg(const Expr* expr,
                    std::vector<std::unordered_map<std::string, Prov>>& scopes,
                    const std::unordered_map<std::string, bool>& closureParams);
    // Records one call site's argument against the callee's parameter.
    void prescanSite(const std::string& mod, const Call& call,
                     std::vector<std::unordered_map<std::string, Prov>>& scopes,
                     const std::unordered_map<std::string, bool>& closureParams);
    // Closure literals being discovered, innermost last. A read that binds
    // outside the innermost literal's own scope is that literal's capture.
    struct LiteralFrame {
        const LocalScope* own = nullptr;
        std::vector<Captured> captures;
    };
    std::vector<LiteralFrame> litStack_;
    // Literals already handed to the sink. The lowerer's hidden-function
    // walk meets the same node again and must answer the type without
    // re-walking, or delegation would recurse forever.
    std::unordered_set<const FuncLit*> emitted_;
    // Match arms the validator proved can never match (a type mismatch the
    // corpus requires to compile and skip, FEATURES §8.4). The lowerer
    // chains the test edge straight through them instead of comparing.
    std::unordered_set<const Pattern*> deadPatterns_;
    TypeRef funcReturn_;
    bool inFunc_ = false;
    // 7.10: inside a fini block the storage checks like a function body,
    // but a return would leave the finalizer half-run, so it refuses.
    bool inFini_ = false;
    // 4.8.3: an init body may call `super_init(...)`; anywhere else the
    // spelling is an ordinary name.
    bool inInit_ = false;
    // 7.2: the one expression (a whole statement or a C-style for step)
    // where a stand-alone `++`/`--` is legal. Null everywhere else, so an
    // embedded increment refuses.
    const Expr* statementExpr_ = nullptr;
    // The class and self type of the method body being walked, so
    // `super` resolves against the right base (Unified Standard 4.8.3).
    std::optional<DeclId> currentClass_;
    TypeRef currentSelf_;
    uint64_t loopDepth_ = 0;
    // Catch bodies being walked, so a bare `throw;` knows it has a current
    // failure to forward. Lowering uses the recorded type per clause.
    int catchDepth_ = 0;
    std::unordered_map<const CatchClause*, TypeRef> catchTypes_;
    // Defer bodies being walked, for the exit-jump ban: a return, break or
    // continue inside one would leave its scope twice.
    int inDefer_ = 0;
    std::vector<CheckError> errors_;
    bool failed_ = false;
    LowerSink* sink_ = nullptr;
};

}  // namespace check
}  // namespace cider

#endif  // CIDER_CHECK_CHECKER_H
