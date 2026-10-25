// ast.h — the tree holds every form FEATURES.md §14 can spell. LAYERS.md §3.2.
//
// One node per form, positions on everything, and no type information anywhere:
// types live in the checker's side table, not in the tree, so lowering looks
// them up instead of re-deriving them. A lowering site that infers a type
// instead of looking one up is what gate L2 exists to catch.
//
// Traversal is node-driven and exactly once. Each `accept` calls its visit
// method, and the base visit method walks the children — so an override that
// wants the subtree calls `ASTVisitor::visitX(n)` and one that does not simply
// doesn't. Nobody reimplements recursion, because four hand-rolled walkers
// doing it inside one file is what v2 did and gate T5/L1 forbids.

#ifndef CIDER_AST_AST_H
#define CIDER_AST_AST_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "lexer/token.h"
#include "types/kind.h"

namespace cider {

struct ASTVisitor;

// --- The root of everything --------------------------------------------------

struct Node {
    Position pos;
    explicit Node(Position p) : pos(std::move(p)) {}
    virtual ~Node() = default;
    virtual void accept(ASTVisitor& visitor) = 0;
};

struct Expr : Node {
    using Node::Node;
};

struct Stmt : Node {
    using Node::Node;
};

struct Decl : Node {
    using Node::Node;
};

struct TypeExpr : Node {
    using Node::Node;
};

// --- Operators ---------------------------------------------------------------

enum class BinOp {
    Add, Sub, Mul, Div, Rem,
    And, Or, Xor, Shl, Shr,
    Eq, Ne, Lt, Le, Gt, Ge,
    AndAnd, OrOr,
};

enum class UnOp {
    Neg, Not, BitNot, Deref, Addr,
};

// --- Type spellings ----------------------------------------------------------

struct BasicType : TypeExpr {
    BasicKind basic;
    BasicType(Position p, BasicKind b) : TypeExpr(std::move(p)), basic(b) {}
    void accept(ASTVisitor& v) override;
};

struct PointerType : TypeExpr {
    std::unique_ptr<TypeExpr> pointee;
    // `*volatile T`: the pointee's memory is accessed volatile through
    // this pointer (FEATURES §1.1c). The mark rides the spelling; the
    // resolved Type stays one type, because volatility describes access,
    // not the value.
    bool pointeeVolatile = false;
    PointerType(Position p, std::unique_ptr<TypeExpr> e)
        : TypeExpr(std::move(p)), pointee(std::move(e)) {}
    void accept(ASTVisitor& v) override;
};

struct ArrayType : TypeExpr {
    std::unique_ptr<TypeExpr> element;
    std::unique_ptr<Expr> length;
    ArrayType(Position p, std::unique_ptr<TypeExpr> e, std::unique_ptr<Expr> n)
        : TypeExpr(std::move(p)), element(std::move(e)), length(std::move(n)) {}
    void accept(ASTVisitor& v) override;
};

struct SliceType : TypeExpr {
    std::unique_ptr<TypeExpr> element;
    SliceType(Position p, std::unique_ptr<TypeExpr> e)
        : TypeExpr(std::move(p)), element(std::move(e)) {}
    void accept(ASTVisitor& v) override;
};

// A named type with an optional qualifier and explicit arguments:
// `Shape`, `lib.Shape`, `Box<int>`. Never deduced, never bare of arguments
// where the declaration takes them (FEATURES.md §3.1).
struct NamedType : TypeExpr {
    std::string qualifier;
    std::string name;
    std::vector<std::unique_ptr<TypeExpr>> args;
    NamedType(Position p, std::string q, std::string n,
              std::vector<std::unique_ptr<TypeExpr>> a)
        : TypeExpr(std::move(p)), qualifier(std::move(q)), name(std::move(n)),
          args(std::move(a)) {}
    void accept(ASTVisitor& v) override;
};

// `func(A, B) -> R`. A null return is `void`. The same spelling is the closure
// value type; `*func(...)` is a PointerType around one of these.
struct FuncSigType : TypeExpr {
    std::vector<std::unique_ptr<TypeExpr>> params;
    std::unique_ptr<TypeExpr> returns;
    // 8.4: "msabi"/"sysv"; empty means the platform default (SysV).
    std::string abi;
    FuncSigType(Position p, std::vector<std::unique_ptr<TypeExpr>> ps,
                std::unique_ptr<TypeExpr> r)
        : TypeExpr(std::move(p)), params(std::move(ps)), returns(std::move(r)) {}
    void accept(ASTVisitor& v) override;
};

// --- Literals and names ------------------------------------------------------

struct IntLit : Expr {
    uint8_t base;
    std::string digits;
    bool negative;
    IntLit(Position p, uint8_t b, std::string d, bool neg)
        : Expr(std::move(p)), base(b), digits(std::move(d)), negative(neg) {}
    void accept(ASTVisitor& v) override;
};

struct FloatLit : Expr {
    std::string text;
    bool negative;
    bool narrow = false;
    FloatLit(Position p, std::string t, bool neg, bool narrowFlag = false)
        : Expr(std::move(p)), text(std::move(t)), negative(neg), narrow(narrowFlag) {}
    void accept(ASTVisitor& v) override;
};

struct CharLit : Expr {
    uint32_t value;
    CharLit(Position p, uint32_t v) : Expr(std::move(p)), value(v) {}
    void accept(ASTVisitor& v) override;
};

struct StringPartAst {
    bool isExpr = false;
    std::string text;
    std::unique_ptr<Expr> expr;
};

struct StringLit : Expr {
    std::vector<StringPartAst> parts;
    StringLit(Position p, std::vector<StringPartAst> ps)
        : Expr(std::move(p)), parts(std::move(ps)) {}
    void accept(ASTVisitor& v) override;
};

// A wide string literal (`u"AB"`): decoded UTF-16 units without the
// terminator. No interpolation — the lexer rejects a lone `{` — so the
// units are the whole value and lowering appends one zero unit.
struct WideLit : Expr {
    std::vector<uint16_t> units;
    WideLit(Position p, std::vector<uint16_t> us)
        : Expr(std::move(p)), units(std::move(us)) {}
    void accept(ASTVisitor& v) override;
};

// A wide character literal (`u'Z'`): one code point, typed u16.
struct WideChar : Expr {
    uint32_t value;
    WideChar(Position p, uint32_t v) : Expr(std::move(p)), value(v) {}
    void accept(ASTVisitor& v) override;
};

struct BoolLit : Expr {
    bool value;
    BoolLit(Position p, bool v) : Expr(std::move(p)), value(v) {}
    void accept(ASTVisitor& v) override;
};

struct NilPtr : Expr {
    using Expr::Expr;
    void accept(ASTVisitor& v) override;
};

struct UninitExpr : Expr {
    using Expr::Expr;
    void accept(ASTVisitor& v) override;
};

// The receiver inside a derived method: the same object, resolved at the
// BASE of the chain (Unified Standard 4.8.3). Only legal in a method of a
// class that has a base.
struct Super : Expr {
    using Expr::Expr;
    void accept(ASTVisitor& v) override;
};

// `super_init(args)` inside an init body: the base class constructor runs
// on the same receiver (Unified Standard 4.8.3). Spelled contextually: an
// identifier `super_init` directly called is this node, anywhere else it
// is an ordinary name.
struct SuperInit : Expr {
    std::vector<std::unique_ptr<Expr>> args;
    SuperInit(Position p, std::vector<std::unique_ptr<Expr>> a)
        : Expr(std::move(p)), args(std::move(a)) {}
    void accept(ASTVisitor& v) override;
};

struct Ident : Expr {
    std::string name;
    Ident(Position p, std::string n) : Expr(std::move(p)), name(std::move(n)) {}
    void accept(ASTVisitor& v) override;
};

// --- Composite expressions ---------------------------------------------------

struct Binary : Expr {
    BinOp op;
    std::unique_ptr<Expr> lhs;
    std::unique_ptr<Expr> rhs;
    Binary(Position p, BinOp o, std::unique_ptr<Expr> l, std::unique_ptr<Expr> r)
        : Expr(std::move(p)), op(o), lhs(std::move(l)), rhs(std::move(r)) {}
    void accept(ASTVisitor& v) override;
};

struct Unary : Expr {
    UnOp op;
    std::unique_ptr<Expr> operand;
    Unary(Position p, UnOp o, std::unique_ptr<Expr> e)
        : Expr(std::move(p)), op(o), operand(std::move(e)) {}
    void accept(ASTVisitor& v) override;
};

struct Cast : Expr {
    std::unique_ptr<Expr> expr;
    std::unique_ptr<TypeExpr> target;
    bool raw = false;
    Cast(Position p, std::unique_ptr<Expr> e, std::unique_ptr<TypeExpr> t)
        : Expr(std::move(p)), expr(std::move(e)), target(std::move(t)) {}
    void accept(ASTVisitor& v) override;
};

struct Ternary : Expr {
    std::unique_ptr<Expr> cond;
    std::unique_ptr<Expr> then;
    std::unique_ptr<Expr> els;
    Ternary(Position p, std::unique_ptr<Expr> c, std::unique_ptr<Expr> t,
            std::unique_ptr<Expr> e)
        : Expr(std::move(p)), cond(std::move(c)), then(std::move(t)), els(std::move(e)) {}
    void accept(ASTVisitor& v) override;
};

// Prefix or postfix `++` / `--` on a place: `i++` answers the old value
// and adds one, `++i` answers the new value. The operand is checked like
// an assignment target and must be an integer; the update itself rides the
// same operation the matching `+= 1` would emit, so overflow behaves like
// `+` (undefined by default, wrapping under the experimental flag).
struct IncrDecr : Expr {
    bool isIncr;
    bool isPrefix;
    std::unique_ptr<Expr> operand;
    IncrDecr(Position p, bool incr, bool prefix, std::unique_ptr<Expr> e)
        : Expr(std::move(p)), isIncr(incr), isPrefix(prefix), operand(std::move(e)) {}
    void accept(ASTVisitor& v) override;
};

struct Call : Expr {
    std::unique_ptr<Expr> callee;
    std::vector<std::unique_ptr<TypeExpr>> typeArgs;
    std::vector<std::unique_ptr<Expr>> args;
    Call(Position p, std::unique_ptr<Expr> c,
         std::vector<std::unique_ptr<TypeExpr>> t, std::vector<std::unique_ptr<Expr>> a)
        : Expr(std::move(p)), callee(std::move(c)), typeArgs(std::move(t)),
          args(std::move(a)) {}
    void accept(ASTVisitor& v) override;
};

struct Field : Expr {
    std::unique_ptr<Expr> object;
    std::string name;
    // True only for the `->` spelling. The object still carries the built-in
    // dereference so pointer access keeps one lowering path; the flag lets
    // the checker try a smart pointer's member `operator->` first.
    bool isArrow;
    Field(Position p, std::unique_ptr<Expr> o, std::string n, bool arrow = false)
        : Expr(std::move(p)), object(std::move(o)), name(std::move(n)),
          isArrow(arrow) {}
    void accept(ASTVisitor& v) override;
};

struct Index : Expr {
    std::unique_ptr<Expr> array;
    std::unique_ptr<Expr> index;
    Index(Position p, std::unique_ptr<Expr> a, std::unique_ptr<Expr> i)
        : Expr(std::move(p)), array(std::move(a)), index(std::move(i)) {}
    void accept(ASTVisitor& v) override;
};

struct SliceExpr : Expr {
    std::unique_ptr<Expr> array;
    std::unique_ptr<Expr> lo;
    std::unique_ptr<Expr> hi;
    SliceExpr(Position p, std::unique_ptr<Expr> a, std::unique_ptr<Expr> l,
              std::unique_ptr<Expr> h)
        : Expr(std::move(p)), array(std::move(a)), lo(std::move(l)), hi(std::move(h)) {}
    void accept(ASTVisitor& v) override;
};

struct FieldInit {
    std::string name;
    std::unique_ptr<Expr> value;
};

struct NewStruct : Expr {
    std::unique_ptr<TypeExpr> type;
    std::vector<FieldInit> fields;
    bool heap = false;
    NewStruct(Position p, std::unique_ptr<TypeExpr> t, std::vector<FieldInit> f)
        : Expr(std::move(p)), type(std::move(t)), fields(std::move(f)) {}
    void accept(ASTVisitor& v) override;
};

struct NewClass : Expr {
    std::unique_ptr<TypeExpr> type;
    std::vector<std::unique_ptr<Expr>> args;
    NewClass(Position p, std::unique_ptr<TypeExpr> t, std::vector<std::unique_ptr<Expr>> a)
        : Expr(std::move(p)), type(std::move(t)), args(std::move(a)) {}
    void accept(ASTVisitor& v) override;
};

struct NewArray : Expr {
    std::unique_ptr<TypeExpr> element;
    std::unique_ptr<Expr> length;
    std::vector<std::unique_ptr<Expr>> inits;
    bool repeat;
    bool heap;
    NewArray(Position p, std::unique_ptr<TypeExpr> e, std::unique_ptr<Expr> n,
             std::vector<std::unique_ptr<Expr>> i, bool r, bool h = false)
        : Expr(std::move(p)), element(std::move(e)), length(std::move(n)),
          inits(std::move(i)), repeat(r), heap(h) {}
    void accept(ASTVisitor& v) override;
};

// `[a, b, c]` or `[x, ...]` — a bare value array literal
// (Unified Standard 6.3). The declared type supplies element and count.
struct ArrayLit : Expr {
    std::vector<std::unique_ptr<Expr>> inits;
    bool repeat;
    ArrayLit(Position p, std::vector<std::unique_ptr<Expr>> i, bool r)
        : Expr(std::move(p)), inits(std::move(i)), repeat(r) {}
    void accept(ASTVisitor& v) override;
};

struct SizeofType : Expr {
    std::unique_ptr<TypeExpr> type;
    SizeofType(Position p, std::unique_ptr<TypeExpr> t)
        : Expr(std::move(p)), type(std::move(t)) {}
    void accept(ASTVisitor& v) override;
};

// `sizeof expr` — the operand is typed but never evaluated.
struct SizeofExpr : Expr {
    std::unique_ptr<Expr> operand;
    SizeofExpr(Position p, std::unique_ptr<Expr> e)
        : Expr(std::move(p)), operand(std::move(e)) {}
    void accept(ASTVisitor& v) override;
};

// `alignof(T)` — the alignment as a compile-time integer (Standard 11).
struct AlignofType : Expr {
    std::unique_ptr<TypeExpr> type;
    AlignofType(Position p, std::unique_ptr<TypeExpr> t)
        : Expr(std::move(p)), type(std::move(t)) {}
    void accept(ASTVisitor& v) override;
};

struct Assign : Expr {
    std::unique_ptr<Expr> target;
    std::optional<BinOp> op;
    std::unique_ptr<Expr> value;
    Assign(Position p, std::unique_ptr<Expr> t, std::optional<BinOp> o,
           std::unique_ptr<Expr> v)
        : Expr(std::move(p)), target(std::move(t)), op(o), value(std::move(v)) {}
    void accept(ASTVisitor& v) override;
};

// --- Statements --------------------------------------------------------------

struct VarDecl : Stmt {
    std::string name;
    std::unique_ptr<TypeExpr> type;
    std::unique_ptr<Expr> init;
    bool isVolatile;
    VarDecl(Position p, std::string n, std::unique_ptr<TypeExpr> t, std::unique_ptr<Expr> i,
            bool vol = false)
        : Stmt(std::move(p)), name(std::move(n)), type(std::move(t)), init(std::move(i)),
          isVolatile(vol) {}
    void accept(ASTVisitor& v) override;
};

struct ExprStmt : Stmt {
    std::unique_ptr<Expr> expr;
    ExprStmt(Position p, std::unique_ptr<Expr> e)
        : Stmt(std::move(p)), expr(std::move(e)) {}
    void accept(ASTVisitor& v) override;
};

struct Return : Stmt {
    std::unique_ptr<Expr> value;
    Return(Position p, std::unique_ptr<Expr> v) : Stmt(std::move(p)), value(std::move(v)) {}
    void accept(ASTVisitor& v) override;
};

struct Break : Stmt {
    using Stmt::Stmt;
    void accept(ASTVisitor& v) override;
};

struct Continue : Stmt {
    using Stmt::Stmt;
    void accept(ASTVisitor& v) override;
};

struct Block : Stmt {
    std::vector<std::unique_ptr<Stmt>> stmts;
    Block(Position p, std::vector<std::unique_ptr<Stmt>> s = {})
        : Stmt(std::move(p)), stmts(std::move(s)) {}
    void accept(ASTVisitor& v) override;
};

struct IfStmt : Stmt {
    std::unique_ptr<Expr> cond;
    std::unique_ptr<Stmt> then;
    std::unique_ptr<Stmt> els;
    IfStmt(Position p, std::unique_ptr<Expr> c, std::unique_ptr<Stmt> t,
           std::unique_ptr<Stmt> e)
        : Stmt(std::move(p)), cond(std::move(c)), then(std::move(t)), els(std::move(e)) {}
    void accept(ASTVisitor& v) override;
};

struct WhileStmt : Stmt {
    std::unique_ptr<Expr> cond;
    std::unique_ptr<Stmt> body;
    WhileStmt(Position p, std::unique_ptr<Expr> c, std::unique_ptr<Stmt> b)
        : Stmt(std::move(p)), cond(std::move(c)), body(std::move(b)) {}
    void accept(ASTVisitor& v) override;
};

struct ForStmt : Stmt {
    std::unique_ptr<Stmt> init;
    std::unique_ptr<Expr> cond;
    std::unique_ptr<Expr> step;
    std::unique_ptr<Stmt> body;
    ForStmt(Position p, std::unique_ptr<Stmt> i, std::unique_ptr<Expr> c,
            std::unique_ptr<Expr> s, std::unique_ptr<Stmt> b)
        : Stmt(std::move(p)), init(std::move(i)), cond(std::move(c)),
          step(std::move(s)), body(std::move(b)) {}
    void accept(ASTVisitor& v) override;
};

struct ForInStmt : Stmt {
    bool isMutable;
    std::string name;
    std::unique_ptr<Expr> seq;
    std::unique_ptr<Stmt> body;
    // The Standard's optional annotation (`for (var b: u8 in xs)`); null
    // when the element's own type names the variable.
    std::unique_ptr<TypeExpr> annotation;
    ForInStmt(Position p, bool m, std::string n, std::unique_ptr<Expr> s,
              std::unique_ptr<Stmt> b, std::unique_ptr<TypeExpr> a = nullptr)
        : Stmt(std::move(p)), isMutable(m), name(std::move(n)), seq(std::move(s)),
          body(std::move(b)), annotation(std::move(a)) {}
    void accept(ASTVisitor& v) override;
};

enum class PatternKind {
    Bind, Wildcard, Type, Const, Member, Range,
};

struct FieldPat {
    std::string name;
    std::unique_ptr<struct Pattern> pat;
};

struct Pattern : Node {
    PatternKind kind = PatternKind::Wildcard;
    std::string name;
    std::unique_ptr<TypeExpr> type;
    std::vector<FieldPat> fields;
    std::unique_ptr<Expr> value;
    // A range keeps its lower bound in `value` and its upper bound here:
    // both are integer literals, and the arm matches when lo <= x <= hi.
    std::unique_ptr<Expr> rangeHi;
    std::string tag;
    std::string member;
    // A module-qualified case (`lib.Shape.CIRCLE`) roots its type in
    // another module: empty for the local `Tag.Member` spelling.
    std::string mod;
    explicit Pattern(Position p) : Node(std::move(p)) {}
    void accept(ASTVisitor& v) override;
};

struct MatchArm : Node {
    std::unique_ptr<Pattern> pattern;
    // `p1 | p2 => body` lists every alternative but the first here: each
    // one is checked like its own arm, and the body runs when any fits.
    std::vector<std::unique_ptr<Pattern>> alts;
    // `pat if cond => body` keeps the guard here, null without `if`: the
    // pattern runs first, and a false guard falls to the next arm.
    std::unique_ptr<Expr> guard;
    std::unique_ptr<Block> body;
    MatchArm(Position p, std::unique_ptr<Pattern> pat, std::unique_ptr<Block> b)
        : Node(std::move(p)), pattern(std::move(pat)), body(std::move(b)) {}
    void accept(ASTVisitor& v) override;
};

struct MatchStmt : Stmt {
    std::unique_ptr<Expr> scrutinee;
    std::vector<std::unique_ptr<MatchArm>> arms;
    std::unique_ptr<Block> fallback;
    MatchStmt(Position p, std::unique_ptr<Expr> s, std::vector<std::unique_ptr<MatchArm>> a,
              std::unique_ptr<Block> d)
        : Stmt(std::move(p)), scrutinee(std::move(s)), arms(std::move(a)),
          fallback(std::move(d)) {}
    void accept(ASTVisitor& v) override;
};

struct DeferStmt : Stmt {
    std::unique_ptr<Stmt> stmt;
    DeferStmt(Position p, std::unique_ptr<Stmt> s)
        : Stmt(std::move(p)), stmt(std::move(s)) {}
    void accept(ASTVisitor& v) override;
};

struct DeleteStmt : Stmt {
    std::unique_ptr<Expr> ptr;
    DeleteStmt(Position p, std::unique_ptr<Expr> e)
        : Stmt(std::move(p)), ptr(std::move(e)) {}
    void accept(ASTVisitor& v) override;
};

struct AsmStmt : Stmt {
    std::string text;
    std::vector<std::string> names;
    AsmStmt(Position p, std::string t, std::vector<std::string> n)
        : Stmt(std::move(p)), text(std::move(t)), names(std::move(n)) {}
    void accept(ASTVisitor& v) override;
};

// Module-scope asm is raw text. Unlike AsmStmt, no `{name}` substitution is
// performed: the bytes are the program's, not the compiler's.
struct ModuleAsm : Decl {
    std::string text;
    ModuleAsm(Position p, std::string t)
        : Decl(std::move(p)), text(std::move(t)) {}
    void accept(ASTVisitor& v) override;
};

// --- Declarations ------------------------------------------------------------

struct Param : Node {
    std::string name;
    std::unique_ptr<TypeExpr> type;
    bool isMutable;
    Param(Position p, std::string n, std::unique_ptr<TypeExpr> t, bool m)
        : Node(std::move(p)), name(std::move(n)), type(std::move(t)), isMutable(m) {}
    void accept(ASTVisitor& v) override;
};

struct FieldDecl : Node {
    std::string name;
    std::unique_ptr<TypeExpr> type;
    std::unique_ptr<Expr> bitWidth;
    bool isVolatile;
    // 4.8.4: public is the default; `private` restricts every read and
    // store of the field to the module that declares its class or struct.
    bool isPrivate = false;
    // v2 CIDER-BUG-049: an optional default runs at every construction
    // before the constructor body, so an aggregate may omit the field.
    std::unique_ptr<Expr> init;
    FieldDecl(Position p, std::string n, std::unique_ptr<TypeExpr> t,
              std::unique_ptr<Expr> w, bool vol, bool priv = false)
        : Node(std::move(p)), name(std::move(n)), type(std::move(t)),
          bitWidth(std::move(w)), isVolatile(vol), isPrivate(priv) {}
    void accept(ASTVisitor& v) override;
};

struct MethodDecl : Node {
    std::string name;
    std::vector<Param> params;
    std::unique_ptr<TypeExpr> returns;
    std::unique_ptr<Block> body;
    bool isVirtual;
    bool isOverride = false;
    // 4.8.4: public is the default; `private` restricts calls to the module
    // that declares the method's type.
    bool isPrivate = false;
    MethodDecl(Position p, std::string n, std::vector<Param> ps,
               std::unique_ptr<TypeExpr> r, std::unique_ptr<Block> b, bool virt)
        : Node(std::move(p)), name(std::move(n)), params(std::move(ps)),
          returns(std::move(r)), body(std::move(b)), isVirtual(virt) {}
    void accept(ASTVisitor& v) override;
};

struct FuncLit : Expr {
    std::vector<Param> params;
    std::unique_ptr<TypeExpr> returns;
    std::unique_ptr<Block> body;
    FuncLit(Position p, std::vector<Param> ps, std::unique_ptr<TypeExpr> r,
            std::unique_ptr<Block> b)
        : Expr(std::move(p)), params(std::move(ps)), returns(std::move(r)),
          body(std::move(b)) {}
    void accept(ASTVisitor& v) override;
};

struct FuncDecl : Decl {
    std::string name;
    std::vector<std::string> typeParams;
    std::vector<Param> params;
    std::unique_ptr<TypeExpr> returns;
    std::unique_ptr<Block> body;
    // 8.4: "msabi"/"sysv"; empty means the platform default (SysV).
    std::string abi;
    FuncDecl(Position p, std::string n, std::vector<std::string> tps,
             std::vector<Param> ps, std::unique_ptr<TypeExpr> r, std::unique_ptr<Block> b)
        : Decl(std::move(p)), name(std::move(n)), typeParams(std::move(tps)),
          params(std::move(ps)), returns(std::move(r)), body(std::move(b)) {}
    void accept(ASTVisitor& v) override;
};

struct StructDecl : Decl {
    std::string name;
    std::vector<std::string> typeParams;
    std::vector<std::unique_ptr<TypeExpr>> interfaces;
    std::vector<FieldDecl> fields;
    std::vector<MethodDecl> methods;
    // 4.5: a @packed struct drops every padding byte between its fields.
    bool packed = false;
    StructDecl(Position p, std::string n, std::vector<std::string> tps,
               std::vector<std::unique_ptr<TypeExpr>> ifs, std::vector<FieldDecl> f,
               std::vector<MethodDecl> m)
        : Decl(std::move(p)), name(std::move(n)), typeParams(std::move(tps)),
          interfaces(std::move(ifs)), fields(std::move(f)), methods(std::move(m)) {}
    void accept(ASTVisitor& v) override;
};

struct ClassDecl : Decl {
    std::string name;
    std::vector<std::string> typeParams;
    std::vector<std::unique_ptr<TypeExpr>> bases;
    std::vector<FieldDecl> fields;
    std::vector<MethodDecl> methods;
    std::unique_ptr<FuncDecl> init;
    std::unique_ptr<FuncDecl> deinit;
    ClassDecl(Position p, std::string n, std::vector<std::string> tps,
              std::vector<std::unique_ptr<TypeExpr>> b, std::vector<FieldDecl> f,
              std::vector<MethodDecl> m, std::unique_ptr<FuncDecl> i,
              std::unique_ptr<FuncDecl> d)
        : Decl(std::move(p)), name(std::move(n)), typeParams(std::move(tps)),
          bases(std::move(b)), fields(std::move(f)), methods(std::move(m)),
          init(std::move(i)), deinit(std::move(d)) {}
    void accept(ASTVisitor& v) override;
};

struct MethodSig {
    std::string name;
    std::vector<Param> params;
    std::unique_ptr<TypeExpr> returns;
};

struct InterfaceDecl : Decl {
    std::string name;
    std::vector<MethodSig> methods;
    InterfaceDecl(Position p, std::string n, std::vector<MethodSig> m)
        : Decl(std::move(p)), name(std::move(n)), methods(std::move(m)) {}
    void accept(ASTVisitor& v) override;
};

struct UnionDecl : Decl {
    std::string name;
    std::vector<FieldDecl> members;
    UnionDecl(Position p, std::string n, std::vector<FieldDecl> m)
        : Decl(std::move(p)), name(std::move(n)), members(std::move(m)) {}
    void accept(ASTVisitor& v) override;
};

struct EnumMember : Node {
    std::string name;
    std::unique_ptr<Expr> value;
    EnumMember(Position p, std::string n, std::unique_ptr<Expr> v)
        : Node(std::move(p)), name(std::move(n)), value(std::move(v)) {}
    void accept(ASTVisitor& v) override;
};

struct EnumDecl : Decl {
    std::string name;
    std::vector<EnumMember> members;
    EnumDecl(Position p, std::string n, std::vector<EnumMember> m)
        : Decl(std::move(p)), name(std::move(n)), members(std::move(m)) {}
    void accept(ASTVisitor& v) override;
};

struct TypeAlias : Decl {
    std::string name;
    std::vector<std::string> typeParams;
    std::unique_ptr<TypeExpr> target;
    TypeAlias(Position p, std::string n, std::vector<std::string> tps,
              std::unique_ptr<TypeExpr> t)
        : Decl(std::move(p)), name(std::move(n)), typeParams(std::move(tps)),
          target(std::move(t)) {}
    void accept(ASTVisitor& v) override;
};

struct ConstDecl : Decl {
    std::string name;
    std::unique_ptr<TypeExpr> type;
    std::unique_ptr<Expr> init;
    ConstDecl(Position p, std::string n, std::unique_ptr<TypeExpr> t,
              std::unique_ptr<Expr> i)
        : Decl(std::move(p)), name(std::move(n)), type(std::move(t)), init(std::move(i)) {}
    void accept(ASTVisitor& v) override;
};

struct ExternFunc : Decl {
    std::string name;
    std::vector<Param> params;
    std::unique_ptr<TypeExpr> returns;
    std::string abi;
    ExternFunc(Position p, std::string n, std::vector<Param> ps,
               std::unique_ptr<TypeExpr> r, std::string a)
        : Decl(std::move(p)), name(std::move(n)), params(std::move(ps)),
          returns(std::move(r)), abi(std::move(a)) {}
    void accept(ASTVisitor& v) override;
};

// `extern var name: T;` — a linker-owned data symbol with no storage here.
struct ExternVar : Decl {
    std::string name;
    std::unique_ptr<TypeExpr> type;
    ExternVar(Position p, std::string n, std::unique_ptr<TypeExpr> t)
        : Decl(std::move(p)), name(std::move(n)), type(std::move(t)) {}
    void accept(ASTVisitor& v) override;
};

struct ImportDecl : Decl {
    std::string module;
    ImportDecl(Position p, std::string m) : Decl(std::move(p)), module(std::move(m)) {}
    void accept(ASTVisitor& v) override;
};

struct FromImport : Decl {
    std::string module;
    std::vector<std::string> names;
    FromImport(Position p, std::string m, std::vector<std::string> n)
        : Decl(std::move(p)), module(std::move(m)), names(std::move(n)) {}
    void accept(ASTVisitor& v) override;
};

struct DeclStmt : Stmt {
    std::unique_ptr<Decl> decl;
    DeclStmt(Position p, std::unique_ptr<Decl> d)
        : Stmt(std::move(p)), decl(std::move(d)) {}
    void accept(ASTVisitor& v) override;
};

struct ExportDecl : Decl {
    std::unique_ptr<Decl> decl;
    ExportDecl(Position p, std::unique_ptr<Decl> d)
        : Decl(std::move(p)), decl(std::move(d)) {}
    void accept(ASTVisitor& v) override;
};

struct Program : Node {
    std::string file;
    std::vector<std::unique_ptr<Decl>> decls;
    std::vector<std::unique_ptr<VarDecl>> globals;
    // `# experimental(wrap)` for this file (FEATURES.md §11.5). It is a
    // property of the *file*, which is why lowering puts it on each
    // instruction rather than in a compiler flag: inlining crosses files.
    bool wrap = false;
    Program(Position p, std::string f, std::vector<std::unique_ptr<Decl>> d,
            std::vector<std::unique_ptr<VarDecl>> g)
        : Node(std::move(p)), file(std::move(f)), decls(std::move(d)),
          globals(std::move(g)) {}
    void accept(ASTVisitor& v) override;
};

// --- The one visitor ---------------------------------------------------------
//
// Every visit method walks the node's children by default. An override that
// wants the subtree calls the base method; one that does not, doesn't. Either
// way the recursion lives here exactly once.

struct ASTVisitor {
    virtual ~ASTVisitor() = default;
    virtual void visitProgram(Program& n);
    virtual void visitBasicType(BasicType& n);
    virtual void visitPointerType(PointerType& n);
    virtual void visitArrayType(ArrayType& n);
    virtual void visitSliceType(SliceType& n);
    virtual void visitNamedType(NamedType& n);
    virtual void visitFuncSigType(FuncSigType& n);
    virtual void visitIntLit(IntLit& n);
    virtual void visitFloatLit(FloatLit& n);
    virtual void visitCharLit(CharLit& n);
    virtual void visitStringLit(StringLit& n);
    virtual void visitWideLit(WideLit& n);
    virtual void visitWideChar(WideChar& n);
    virtual void visitBoolLit(BoolLit& n);
    virtual void visitNilPtr(NilPtr& n);
    virtual void visitUninitExpr(UninitExpr& n);
    virtual void visitSuper(Super& n);
    virtual void visitSuperInit(SuperInit& n);
    virtual void visitIdent(Ident& n);
    virtual void visitBinary(Binary& n);
    virtual void visitUnary(Unary& n);
    virtual void visitCast(Cast& n);
    virtual void visitTernary(Ternary& n);
    virtual void visitIncrDecr(IncrDecr& n);
    virtual void visitCall(Call& n);
    virtual void visitField(Field& n);
    virtual void visitIndex(Index& n);
    virtual void visitSliceExpr(SliceExpr& n);
    virtual void visitNewStruct(NewStruct& n);
    virtual void visitNewClass(NewClass& n);
    virtual void visitNewArray(NewArray& n);
    virtual void visitArrayLit(ArrayLit& n);
    virtual void visitAssign(Assign& n);
    virtual void visitSizeofType(SizeofType& n);
    virtual void visitSizeofExpr(SizeofExpr& n);
    virtual void visitAlignofType(AlignofType& n);
    virtual void visitFuncLit(FuncLit& n);
    virtual void visitDeclStmt(DeclStmt& n);
    virtual void visitExportDecl(ExportDecl& n);
    virtual void visitVarDecl(VarDecl& n);
    virtual void visitExprStmt(ExprStmt& n);
    virtual void visitReturn(Return& n);
    virtual void visitBreak(Break& n);
    virtual void visitContinue(Continue& n);
    virtual void visitBlock(Block& n);
    virtual void visitIfStmt(IfStmt& n);
    virtual void visitWhileStmt(WhileStmt& n);
    virtual void visitForStmt(ForStmt& n);
    virtual void visitForInStmt(ForInStmt& n);
    virtual void visitPattern(Pattern& n);
    virtual void visitMatchArm(MatchArm& n);
    virtual void visitMatchStmt(MatchStmt& n);
    virtual void visitDeferStmt(DeferStmt& n);
    virtual void visitDeleteStmt(DeleteStmt& n);
    virtual void visitAsmStmt(AsmStmt& n);
    virtual void visitModuleAsm(ModuleAsm& n);
    virtual void visitParam(Param& n);
    virtual void visitFieldDecl(FieldDecl& n);
    virtual void visitMethodDecl(MethodDecl& n);
    virtual void visitFuncDecl(FuncDecl& n);
    virtual void visitStructDecl(StructDecl& n);
    virtual void visitClassDecl(ClassDecl& n);
    virtual void visitInterfaceDecl(InterfaceDecl& n);
    virtual void visitUnionDecl(UnionDecl& n);
    virtual void visitEnumMember(EnumMember& n);
    virtual void visitEnumDecl(EnumDecl& n);
    virtual void visitTypeAlias(TypeAlias& n);
    virtual void visitConstDecl(ConstDecl& n);
    virtual void visitExternFunc(ExternFunc& n);
    virtual void visitExternVar(ExternVar& n);
    virtual void visitImportDecl(ImportDecl& n);
    virtual void visitFromImport(FromImport& n);
};

// --- accept: one line per node, each naming its visit ------------------------

inline void BasicType::accept(ASTVisitor& v) { v.visitBasicType(*this); }
inline void PointerType::accept(ASTVisitor& v) { v.visitPointerType(*this); }
inline void ArrayType::accept(ASTVisitor& v) { v.visitArrayType(*this); }
inline void SliceType::accept(ASTVisitor& v) { v.visitSliceType(*this); }
inline void NamedType::accept(ASTVisitor& v) { v.visitNamedType(*this); }
inline void FuncSigType::accept(ASTVisitor& v) { v.visitFuncSigType(*this); }
inline void IntLit::accept(ASTVisitor& v) { v.visitIntLit(*this); }
inline void FloatLit::accept(ASTVisitor& v) { v.visitFloatLit(*this); }
inline void CharLit::accept(ASTVisitor& v) { v.visitCharLit(*this); }
inline void StringLit::accept(ASTVisitor& v) { v.visitStringLit(*this); }
inline void WideLit::accept(ASTVisitor& v) { v.visitWideLit(*this); }
inline void WideChar::accept(ASTVisitor& v) { v.visitWideChar(*this); }
inline void BoolLit::accept(ASTVisitor& v) { v.visitBoolLit(*this); }
inline void NilPtr::accept(ASTVisitor& v) { v.visitNilPtr(*this); }
inline void UninitExpr::accept(ASTVisitor& v) { v.visitUninitExpr(*this); }
inline void Super::accept(ASTVisitor& v) { v.visitSuper(*this); }
inline void SuperInit::accept(ASTVisitor& v) { v.visitSuperInit(*this); }
inline void Ident::accept(ASTVisitor& v) { v.visitIdent(*this); }
inline void Binary::accept(ASTVisitor& v) { v.visitBinary(*this); }
inline void Unary::accept(ASTVisitor& v) { v.visitUnary(*this); }
inline void Cast::accept(ASTVisitor& v) { v.visitCast(*this); }
inline void Ternary::accept(ASTVisitor& v) { v.visitTernary(*this); }
inline void IncrDecr::accept(ASTVisitor& v) { v.visitIncrDecr(*this); }
inline void Call::accept(ASTVisitor& v) { v.visitCall(*this); }
inline void Field::accept(ASTVisitor& v) { v.visitField(*this); }
inline void Index::accept(ASTVisitor& v) { v.visitIndex(*this); }
inline void SliceExpr::accept(ASTVisitor& v) { v.visitSliceExpr(*this); }
inline void NewStruct::accept(ASTVisitor& v) { v.visitNewStruct(*this); }
inline void NewClass::accept(ASTVisitor& v) { v.visitNewClass(*this); }
inline void NewArray::accept(ASTVisitor& v) { v.visitNewArray(*this); }
inline void ArrayLit::accept(ASTVisitor& v) { v.visitArrayLit(*this); }
inline void Assign::accept(ASTVisitor& v) { v.visitAssign(*this); }
inline void SizeofType::accept(ASTVisitor& v) { v.visitSizeofType(*this); }
inline void SizeofExpr::accept(ASTVisitor& v) { v.visitSizeofExpr(*this); }
inline void FuncLit::accept(ASTVisitor& v) { v.visitFuncLit(*this); }
inline void DeclStmt::accept(ASTVisitor& v) { v.visitDeclStmt(*this); }
inline void ExportDecl::accept(ASTVisitor& v) { v.visitExportDecl(*this); }
inline void VarDecl::accept(ASTVisitor& v) { v.visitVarDecl(*this); }
inline void ExprStmt::accept(ASTVisitor& v) { v.visitExprStmt(*this); }
inline void Return::accept(ASTVisitor& v) { v.visitReturn(*this); }
inline void Break::accept(ASTVisitor& v) { v.visitBreak(*this); }
inline void Continue::accept(ASTVisitor& v) { v.visitContinue(*this); }
inline void Block::accept(ASTVisitor& v) { v.visitBlock(*this); }
inline void IfStmt::accept(ASTVisitor& v) { v.visitIfStmt(*this); }
inline void WhileStmt::accept(ASTVisitor& v) { v.visitWhileStmt(*this); }
inline void ForStmt::accept(ASTVisitor& v) { v.visitForStmt(*this); }
inline void ForInStmt::accept(ASTVisitor& v) { v.visitForInStmt(*this); }
inline void Pattern::accept(ASTVisitor& v) { v.visitPattern(*this); }
inline void MatchArm::accept(ASTVisitor& v) { v.visitMatchArm(*this); }
inline void MatchStmt::accept(ASTVisitor& v) { v.visitMatchStmt(*this); }
inline void DeferStmt::accept(ASTVisitor& v) { v.visitDeferStmt(*this); }
inline void DeleteStmt::accept(ASTVisitor& v) { v.visitDeleteStmt(*this); }
inline void AsmStmt::accept(ASTVisitor& v) { v.visitAsmStmt(*this); }
inline void ModuleAsm::accept(ASTVisitor& v) { v.visitModuleAsm(*this); }
inline void Param::accept(ASTVisitor& v) { v.visitParam(*this); }
inline void FieldDecl::accept(ASTVisitor& v) { v.visitFieldDecl(*this); }
inline void MethodDecl::accept(ASTVisitor& v) { v.visitMethodDecl(*this); }
inline void FuncDecl::accept(ASTVisitor& v) { v.visitFuncDecl(*this); }
inline void StructDecl::accept(ASTVisitor& v) { v.visitStructDecl(*this); }
inline void ClassDecl::accept(ASTVisitor& v) { v.visitClassDecl(*this); }
inline void InterfaceDecl::accept(ASTVisitor& v) { v.visitInterfaceDecl(*this); }
inline void UnionDecl::accept(ASTVisitor& v) { v.visitUnionDecl(*this); }
inline void EnumMember::accept(ASTVisitor& v) { v.visitEnumMember(*this); }
inline void EnumDecl::accept(ASTVisitor& v) { v.visitEnumDecl(*this); }
inline void TypeAlias::accept(ASTVisitor& v) { v.visitTypeAlias(*this); }
inline void ConstDecl::accept(ASTVisitor& v) { v.visitConstDecl(*this); }
inline void ExternFunc::accept(ASTVisitor& v) { v.visitExternFunc(*this); }
inline void ExternVar::accept(ASTVisitor& v) { v.visitExternVar(*this); }
inline void ImportDecl::accept(ASTVisitor& v) { v.visitImportDecl(*this); }
inline void FromImport::accept(ASTVisitor& v) { v.visitFromImport(*this); }
inline void Program::accept(ASTVisitor& v) { v.visitProgram(*this); }

// --- The default walk: every child, in source order --------------------------

inline void ASTVisitor::visitProgram(Program& n) {
    for (auto& d : n.decls) d->accept(*this);
    for (auto& g : n.globals) g->accept(*this);
}
inline void ASTVisitor::visitBasicType(BasicType&) {}
inline void ASTVisitor::visitPointerType(PointerType& n) { n.pointee->accept(*this); }
inline void ASTVisitor::visitArrayType(ArrayType& n) {
    n.element->accept(*this);
    if (n.length != nullptr) n.length->accept(*this);
}
inline void ASTVisitor::visitSliceType(SliceType& n) { n.element->accept(*this); }
inline void ASTVisitor::visitNamedType(NamedType& n) {
    for (auto& a : n.args) a->accept(*this);
}
inline void ASTVisitor::visitFuncSigType(FuncSigType& n) {
    for (auto& p : n.params) p->accept(*this);
    if (n.returns != nullptr) n.returns->accept(*this);
}
inline void ASTVisitor::visitIntLit(IntLit&) {}
inline void ASTVisitor::visitFloatLit(FloatLit&) {}
inline void ASTVisitor::visitCharLit(CharLit&) {}
inline void ASTVisitor::visitWideLit(WideLit&) {}
inline void ASTVisitor::visitWideChar(WideChar&) {}
inline void ASTVisitor::visitStringLit(StringLit& n) {
    for (auto& part : n.parts) {
        if (part.isExpr && part.expr != nullptr) part.expr->accept(*this);
    }
}
inline void ASTVisitor::visitBoolLit(BoolLit&) {}
inline void ASTVisitor::visitNilPtr(NilPtr&) {}
inline void ASTVisitor::visitUninitExpr(UninitExpr&) {}
inline void ASTVisitor::visitSuper(Super&) {}
inline void ASTVisitor::visitSuperInit(SuperInit& n) {
    for (auto& a : n.args) a->accept(*this);
}
inline void ASTVisitor::visitIdent(Ident&) {}
inline void ASTVisitor::visitBinary(Binary& n) {
    n.lhs->accept(*this);
    n.rhs->accept(*this);
}
inline void ASTVisitor::visitUnary(Unary& n) { n.operand->accept(*this); }
inline void ASTVisitor::visitCast(Cast& n) {
    n.expr->accept(*this);
    n.target->accept(*this);
}
inline void ASTVisitor::visitTernary(Ternary& n) {
    n.cond->accept(*this);
    n.then->accept(*this);
    n.els->accept(*this);
}
inline void ASTVisitor::visitIncrDecr(IncrDecr& n) { n.operand->accept(*this); }
inline void ASTVisitor::visitCall(Call& n) {
    n.callee->accept(*this);
    for (auto& t : n.typeArgs) t->accept(*this);
    for (auto& a : n.args) a->accept(*this);
}
inline void ASTVisitor::visitField(Field& n) { n.object->accept(*this); }
inline void ASTVisitor::visitIndex(Index& n) {
    n.array->accept(*this);
    n.index->accept(*this);
}
inline void ASTVisitor::visitSliceExpr(SliceExpr& n) {
    n.array->accept(*this);
    n.lo->accept(*this);
    n.hi->accept(*this);
}
inline void ASTVisitor::visitNewStruct(NewStruct& n) {
    n.type->accept(*this);
    for (auto& f : n.fields) f.value->accept(*this);
}
inline void ASTVisitor::visitNewClass(NewClass& n) {
    n.type->accept(*this);
    for (auto& a : n.args) a->accept(*this);
}
inline void ASTVisitor::visitNewArray(NewArray& n) {
    n.element->accept(*this);
    if (n.length != nullptr) n.length->accept(*this);
    for (auto& i : n.inits) i->accept(*this);
}
inline void ASTVisitor::visitArrayLit(ArrayLit& n) {
    for (auto& i : n.inits) i->accept(*this);
}
inline void ASTVisitor::visitSizeofType(SizeofType& n) { n.type->accept(*this); }
inline void ASTVisitor::visitSizeofExpr(SizeofExpr& n) { n.operand->accept(*this); }
inline void AlignofType::accept(ASTVisitor& v) { v.visitAlignofType(*this); }
inline void ASTVisitor::visitAlignofType(AlignofType& n) { n.type->accept(*this); }
inline void ASTVisitor::visitAssign(Assign& n) {
    n.target->accept(*this);
    n.value->accept(*this);
}
inline void ASTVisitor::visitFuncLit(FuncLit& n) {
    for (auto& p : n.params) p.accept(*this);
    if (n.returns != nullptr) n.returns->accept(*this);
    n.body->accept(*this);
}
inline void ASTVisitor::visitDeclStmt(DeclStmt& n) { n.decl->accept(*this); }
inline void ASTVisitor::visitExportDecl(ExportDecl& n) { n.decl->accept(*this); }
inline void ASTVisitor::visitVarDecl(VarDecl& n) {
    n.type->accept(*this);
    n.init->accept(*this);
}
inline void ASTVisitor::visitExprStmt(ExprStmt& n) { n.expr->accept(*this); }
inline void ASTVisitor::visitReturn(Return& n) {
    if (n.value != nullptr) n.value->accept(*this);
}
inline void ASTVisitor::visitBreak(Break&) {}
inline void ASTVisitor::visitContinue(Continue&) {}
inline void ASTVisitor::visitBlock(Block& n) {
    for (auto& s : n.stmts) s->accept(*this);
}
inline void ASTVisitor::visitIfStmt(IfStmt& n) {
    n.cond->accept(*this);
    n.then->accept(*this);
    if (n.els != nullptr) n.els->accept(*this);
}
inline void ASTVisitor::visitWhileStmt(WhileStmt& n) {
    n.cond->accept(*this);
    n.body->accept(*this);
}
inline void ASTVisitor::visitForStmt(ForStmt& n) {
    if (n.init != nullptr) n.init->accept(*this);
    if (n.cond != nullptr) n.cond->accept(*this);
    if (n.step != nullptr) n.step->accept(*this);
    n.body->accept(*this);
}
inline void ASTVisitor::visitForInStmt(ForInStmt& n) {
    n.seq->accept(*this);
    n.body->accept(*this);
}
inline void ASTVisitor::visitPattern(Pattern& n) {
    if (n.type != nullptr) n.type->accept(*this);
    for (auto& f : n.fields) f.pat->accept(*this);
    if (n.value != nullptr) n.value->accept(*this);
    if (n.rangeHi != nullptr) n.rangeHi->accept(*this);
}
inline void ASTVisitor::visitMatchArm(MatchArm& n) {
    n.pattern->accept(*this);
    if (n.guard != nullptr) {
        n.guard->accept(*this);
    }
    n.body->accept(*this);
}
inline void ASTVisitor::visitMatchStmt(MatchStmt& n) {
    n.scrutinee->accept(*this);
    for (auto& a : n.arms) a->accept(*this);
    if (n.fallback != nullptr) n.fallback->accept(*this);
}
inline void ASTVisitor::visitDeferStmt(DeferStmt& n) { n.stmt->accept(*this); }
inline void ASTVisitor::visitDeleteStmt(DeleteStmt& n) { n.ptr->accept(*this); }
inline void ASTVisitor::visitAsmStmt(AsmStmt&) {}
inline void ASTVisitor::visitModuleAsm(ModuleAsm&) {}
inline void ASTVisitor::visitParam(Param& n) { n.type->accept(*this); }
inline void ASTVisitor::visitFieldDecl(FieldDecl& n) {
    n.type->accept(*this);
    if (n.bitWidth != nullptr) {
        n.bitWidth->accept(*this);
    }
    if (n.init != nullptr) {
        n.init->accept(*this);
    }
}
inline void ASTVisitor::visitMethodDecl(MethodDecl& n) {
    for (auto& p : n.params) p.accept(*this);
    if (n.returns != nullptr) n.returns->accept(*this);
    n.body->accept(*this);
}
inline void ASTVisitor::visitFuncDecl(FuncDecl& n) {
    for (auto& p : n.params) p.accept(*this);
    if (n.returns != nullptr) n.returns->accept(*this);
    n.body->accept(*this);
}
inline void ASTVisitor::visitStructDecl(StructDecl& n) {
    for (auto& i : n.interfaces) i->accept(*this);
    for (auto& f : n.fields) f.accept(*this);
    for (auto& m : n.methods) m.accept(*this);
}
inline void ASTVisitor::visitClassDecl(ClassDecl& n) {
    for (auto& b : n.bases) b->accept(*this);
    for (auto& f : n.fields) f.accept(*this);
    for (auto& m : n.methods) m.accept(*this);
    if (n.init != nullptr) n.init->accept(*this);
    if (n.deinit != nullptr) n.deinit->accept(*this);
}
inline void ASTVisitor::visitInterfaceDecl(InterfaceDecl& n) {
    for (auto& m : n.methods) {
        for (auto& p : m.params) p.accept(*this);
        if (m.returns != nullptr) m.returns->accept(*this);
    }
}
inline void ASTVisitor::visitUnionDecl(UnionDecl& n) {
    for (auto& m : n.members) m.accept(*this);
}
inline void ASTVisitor::visitEnumMember(EnumMember& n) {
    if (n.value != nullptr) n.value->accept(*this);
}
inline void ASTVisitor::visitEnumDecl(EnumDecl& n) {
    for (auto& m : n.members) m.accept(*this);
}
inline void ASTVisitor::visitTypeAlias(TypeAlias& n) { n.target->accept(*this); }
inline void ASTVisitor::visitConstDecl(ConstDecl& n) {
    if (n.type != nullptr) n.type->accept(*this);
    n.init->accept(*this);
}
inline void ASTVisitor::visitExternFunc(ExternFunc& n) {
    for (auto& p : n.params) p.accept(*this);
    if (n.returns != nullptr) n.returns->accept(*this);
}
inline void ASTVisitor::visitExternVar(ExternVar& n) {
    if (n.type != nullptr) n.type->accept(*this);
}
inline void ASTVisitor::visitImportDecl(ImportDecl&) {}
inline void ASTVisitor::visitFromImport(FromImport&) {}

}  // namespace cider

#endif  // CIDER_AST_AST_H
