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

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast/ast.h"
#include "eval/eval.h"
#include "resolve/resolver.h"

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
    virtual void onIf(IfStmt& n) { (void)n; }
    virtual void onWhile(WhileStmt& n) { (void)n; }
    // `delete p;` frees a class object. Checked first (the pointer type is
    // known), then lowered here.
    virtual void onDelete(DeleteStmt& n) { (void)n; }
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

class Checker : public ASTVisitor {
public:
    Checker(TypeContext& ctx, resolve::Resolver& resolver);

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

    TypeRef typeOf(const Node* node) const;
    const std::vector<CheckError>& errors() const { return errors_; }
    size_t errorCount() const { return errors_.size(); }

    // Declarations and statements with rules.
    void visitProgram(Program& n) override;
    void visitFuncDecl(FuncDecl& n) override;
    void visitStructDecl(StructDecl& n) override;
    void visitClassDecl(ClassDecl& n) override;
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
    void visitThrowStmt(ThrowStmt& n) override;
    void visitTryStmt(TryStmt& n) override;

    // Every expression records its type.
    void visitIntLit(IntLit& n) override;
    void visitFloatLit(FloatLit& n) override;
    void visitCharLit(CharLit& n) override;
    void visitStringLit(StringLit& n) override;
    void visitBoolLit(BoolLit& n) override;
    void visitNilPtr(NilPtr& n) override;
    void visitIdent(Ident& n) override;
    void visitBinary(Binary& n) override;
    void visitUnary(Unary& n) override;
    void visitCast(Cast& n) override;
    void visitTernary(Ternary& n) override;
    void visitCall(Call& n) override;
    void visitField(Field& n) override;
    void visitIndex(Index& n) override;
    void visitSliceExpr(SliceExpr& n) override;
    void visitNewStruct(NewStruct& n) override;
    void visitNewClass(NewClass& n) override;
    void visitNewArray(NewArray& n) override;
    void visitAssign(Assign& n) override;
    void visitSizeofType(SizeofType& n) override;
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
    bool isPlace(const Expr* expr) const;
    bool isInteger(TypeRef type) const;
    TypeRef checkBinary(Binary& expr);
    void checkShiftDiv(const Binary& expr, TypeRef lhs, TypeRef rhs);
    TypeRef checkCall(Call& expr);
    TypeRef checkField(Field& expr);
    TypeRef checkNewStruct(NewStruct& expr);
    TypeRef checkNewUnion(NewStruct& expr, TypeRef target, DeclId id);
    TypeRef checkUnionHas(Call& expr, const Field& callee, TypeRef object);
    TypeRef checkNewArray(NewArray& expr);
    TypeRef checkTernary(Ternary& expr, TypeRef expected);
    void checkMethods(DeclId id, Kind kind, std::vector<MethodDecl>& methods);
    void checkCtor(DeclId id, Kind kind, FuncDecl* ctor, bool isInit);
    void bindPattern(Pattern& pat, TypeRef scrutinee);
    void bindSubPattern(Pattern& pat, TypeRef type);

    // SEMANTICS.md §2.3: the common type of two operands, stated as a type.
    TypeRef commonType(TypeRef lhs, TypeRef rhs, Position pos);
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
    const FuncDecl* funcSpelling(const std::string& name) const;
    bool implementsAll(DeclId concrete, DeclId iface, Position pos);
    bool isDerivedFrom(DeclId derived, DeclId base) const;
    bool castRowOk(TypeRef from, TypeRef to) const;

    // Templates by name, for instantiation body walks. Non-owning; the
    // Program outlives every use.
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
    // Types resolve here rather than at module scope during generic walks,
    // where the template's parameters are bound.
    const resolve::Scope* typeScope_ = nullptr;

    TypeContext& ctx_;
    resolve::Resolver& resolver_;
    const resolve::Scope* moduleScope_ = nullptr;
    std::string module_;
    // Held by pointer: EvalEnv holds a reference to the TypeContext, and a
    // class with a reference member cannot be assigned. One env per program,
    // built in checkProgram once the module scope is known.
    std::unique_ptr<EvalEnv> env_;
    const LocalScope* current_ = nullptr;
    std::vector<std::unique_ptr<LocalScope>> scopes_;
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
    TypeRef funcReturn_;
    bool inFunc_ = false;
    uint64_t loopDepth_ = 0;
    std::vector<CheckError> errors_;
    bool failed_ = false;
    LowerSink* sink_ = nullptr;
};

}  // namespace check
}  // namespace cider

#endif  // CIDER_CHECK_CHECKER_H
