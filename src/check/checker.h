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
#include <string>
#include <unordered_map>
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
class LowerSink {
public:
    virtual ~LowerSink() = default;
    virtual void onProgram(Program& n) { (void)n; }
    virtual void onBlockEnter(Block& n) { (void)n; }
    virtual void onBlockExit(Block& n) { (void)n; }
    virtual void onFuncDecl(FuncDecl& n) { (void)n; }
    virtual void onVarDecl(VarDecl& n) { (void)n; }
    // An expression statement is evaluated for effect and its value dropped:
    // calls and assignments live here, and without this hook they vanish.
    virtual void onExprStmt(ExprStmt& n) { (void)n; }
    virtual void onReturn(Return& n) { (void)n; }
    virtual void onIf(IfStmt& n) { (void)n; }
    virtual void onWhile(WhileStmt& n) { (void)n; }
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

private:
    void fail(Position pos, const std::string& message);
    void record(const Node* node, TypeRef type);

    // Scopes: locals first, then the module scope. Shadowing is allowed and
    // silent; an inner binding hides the outer one.
    const LocalScope* pushScope();
    void popScope();
    bool bindLocal(const std::string& name, TypeRef type, bool isMut, Position pos);
    const Local* findLocal(const std::string& name) const;

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
    TypeRef checkNewArray(NewArray& expr);
    TypeRef checkTernary(Ternary& expr, TypeRef expected);
    void checkMethods(DeclId id, Kind kind, std::vector<MethodDecl>& methods);
    void checkCtor(DeclId id, Kind kind, FuncDecl* ctor, bool isInit);
    void bindPattern(Pattern& pat, TypeRef scrutinee);
    void bindSubPattern(Pattern& pat, TypeRef type);

    // SEMANTICS.md §2.3: the common type of two operands, stated as a type.
    TypeRef commonType(TypeRef lhs, TypeRef rhs, Position pos);
    // SEMANTICS.md §5.4: the six implicit conversions, and nothing else.
    bool checkCompatible(Position pos, const Expr* fromExpr, TypeRef from, TypeRef to);
    bool implementsAll(DeclId concrete, DeclId iface, Position pos);
    bool isDerivedFrom(DeclId derived, DeclId base) const;
    bool castRowOk(TypeRef from, TypeRef to) const;

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
