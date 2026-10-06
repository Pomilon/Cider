// parser_test.cpp — syntax alone, proved on text.
//
// The parser accepts or rejects on syntax alone and never resolves a name.
// Each test below feeds source text and asserts on the tree's shape, or on the
// diagnostic a bad form produces. Nothing here names a type, a module, or a
// value — those do not exist yet at this layer.

#include <cstdio>
#include <string>

#include "parse/parser.h"
#include "lexer/lexer.h"

namespace {

int failures = 0;
int checks = 0;

void check(bool holds, const char* what) {
    ++checks;
    if (holds) {
        std::printf("  ok   %s\n", what);
    } else {
        std::printf("  FAIL %s\n", what);
        ++failures;
    }
}

std::unique_ptr<cider::Program> parse(const std::string& text, bool& ok) {
    cider::Lexer lexer("test.cid", text);
    std::vector<cider::Token> tokens = lexer.lex();
    cider::Parser parser("test.cid", std::move(tokens));
    std::unique_ptr<cider::Program> program = parser.parse();
    ok = parser.ok() && lexer.errors().empty();
    if (!parser.ok()) {
        for (const cider::ParseError& e : parser.errors()) {
            std::printf("    diagnostic %u:%u %s\n", e.pos.line, e.pos.column,
                        e.message.c_str());
        }
    }
    return program;
}

// `var g: int = 1 + 2 * 3;` binds looser at `+` than at `*`.
void testPrecedence() {
    std::printf("precedence\n");
    bool ok = false;
    std::unique_ptr<cider::Program> program = parse("var g: int = 1 + 2 * 3;", ok);
    check(ok, "parses");
    check(program->globals.size() == 1, "one global");
    const cider::Binary& add =
        static_cast<const cider::Binary&>(*program->globals[0]->init);
    check(add.op == cider::BinOp::Add, "addition at the top");
    const cider::Binary& mul = static_cast<const cider::Binary&>(*add.rhs);
    check(mul.op == cider::BinOp::Mul, "multiplication inside");
    check(program->globals[0]->pos.column == 1, "position rides along");
}

// `and`/`or` are the word spellings of `&&`/`||`: same BinOp, so the check
// and short-circuit layers never see a difference (FEATURES §8.1).
void testWordLogic() {
    std::printf("word logic\n");
    bool ok = false;
    std::unique_ptr<cider::Program> program = parse("var g: bool = a and b or c;", ok);
    check(ok, "parses");
    if (!ok || program->globals.empty()) {
        return;
    }
    const cider::Binary& top =
        static_cast<const cider::Binary&>(*program->globals[0]->init);
    check(top.op == cider::BinOp::OrOr, "or loosest");
    const cider::Binary& inner =
        static_cast<const cider::Binary&>(*top.lhs);
    check(inner.op == cider::BinOp::AndAnd, "and tighter");
}

// `as` binds looser than unary: `&x as *void` is `(&x) as *void`.
void testAsBelowUnary() {
    std::printf("as below unary\n");
    bool ok = false;
    std::unique_ptr<cider::Program> program = parse("var p: *void = &x as *void;", ok);
    check(ok, "parses");
    const cider::Cast& cast =
        static_cast<const cider::Cast&>(*program->globals[0]->init);
    const cider::Unary& addr = static_cast<const cider::Unary&>(*cast.expr);
    check(addr.op == cider::UnOp::Addr, "the address is taken first");
}

// `-128` is one literal, and `a->b` is two tokens the parser rejects.
void testMinusAndArrow() {
    std::printf("unary minus folds, the arrow fails\n");
    bool ok = false;
    std::unique_ptr<cider::Program> program = parse("var x: i8 = -128;", ok);
    check(ok, "parses");
    const cider::IntLit& lit =
        static_cast<const cider::IntLit&>(*program->globals[0]->init);
    check(lit.negative && lit.digits == "128", "one negative literal");
    bool ok2 = true;
    parse("func f(p: *Point) -> int { return p->x; }", ok2);
    check(!ok2, "p->x is rejected");
}

// `a += b` is one Assign carrying its operator; `a = b` carries none.
void testCompound() {
    std::printf("compound assignment\n");
    bool ok = false;
    std::unique_ptr<cider::Program> program =
        parse("func f() -> void { a += 1; b = 2; }", ok);
    check(ok, "parses");
    const cider::FuncDecl& fn = static_cast<const cider::FuncDecl&>(*program->decls[0]);
    const cider::ExprStmt& first =
        static_cast<const cider::ExprStmt&>(*fn.body->stmts[0]);
    const cider::Assign& add = static_cast<const cider::Assign&>(*first.expr);
    check(add.op.has_value() && *add.op == cider::BinOp::Add, "carries Add");
    const cider::ExprStmt& second =
        static_cast<const cider::ExprStmt&>(*fn.body->stmts[1]);
    const cider::Assign& plain = static_cast<const cider::Assign&>(*second.expr);
    check(!plain.op.has_value(), "plain assignment carries none");
}

// A missing initialiser is a diagnostic, and parsing continues after it.
void testMissingInit() {
    std::printf("errors are diagnostics, and parsing continues\n");
    bool ok = true;
    std::unique_ptr<cider::Program> program =
        parse("var bad: int;\nvar good: int = 1;", ok);
    check(!ok, "the program fails");
    check(program->globals.size() == 1, "the good global survived");
    check(program->globals[0]->name == "good", "it is the second one");
}

// `++i` prefixes, `i++` postfixes, `*p++` is `*(p++)`, `a--b` is `(a--)-b`.
void testIncrDecr() {
    std::printf("increment and decrement\n");
    bool ok = false;
    std::unique_ptr<cider::Program> program = parse(
        "func f() -> void { ++i; j--; --*p; (*q)++; a-- - b; }", ok);
    check(ok, "parses");
    const cider::FuncDecl& fn = static_cast<const cider::FuncDecl&>(*program->decls[0]);
    const cider::IncrDecr& pre =
        static_cast<const cider::IncrDecr&>(
            *static_cast<const cider::ExprStmt&>(*fn.body->stmts[0]).expr);
    check(pre.isIncr && pre.isPrefix, "++i is prefix increment");
    const cider::IncrDecr& post =
        static_cast<const cider::IncrDecr&>(
            *static_cast<const cider::ExprStmt&>(*fn.body->stmts[1]).expr);
    check(!post.isIncr && !post.isPrefix, "j-- is postfix decrement");
    const cider::IncrDecr& through =
        static_cast<const cider::IncrDecr&>(
            *static_cast<const cider::ExprStmt&>(*fn.body->stmts[2]).expr);
    check(through.isPrefix &&
              dynamic_cast<const cider::Unary*>(through.operand.get()) != nullptr,
          "--*p dereferences first");
    const cider::IncrDecr& grouped =
        static_cast<const cider::IncrDecr&>(
            *static_cast<const cider::ExprStmt&>(*fn.body->stmts[3]).expr);
    check(!grouped.isPrefix, "(*q)++ increments through the group");
    const cider::Binary& rest =
        static_cast<const cider::Binary&>(
            *static_cast<const cider::ExprStmt&>(*fn.body->stmts[4]).expr);
    check(rest.op == cider::BinOp::Sub &&
              dynamic_cast<const cider::IncrDecr*>(rest.lhs.get()) != nullptr,
          "a--b is (a--) - b");
}

// `max::<int>(3, 9)` carries its arguments on the call.
void testGenericCall() {
    std::printf("explicit type arguments\n");
    bool ok = false;
    std::unique_ptr<cider::Program> program =
        parse("var m: int = max::<int>(3, 9);", ok);
    check(ok, "parses");
    const cider::Call& call =
        static_cast<const cider::Call&>(*program->globals[0]->init);
    check(call.typeArgs.size() == 1, "one type argument");
    check(call.args.size() == 2, "two value arguments");
}

// `match` arms, `try`/`catch`, and `for-in` parse to their nodes.
void testControlForms() {
    std::printf("control forms\n");
    bool ok = false;
    std::unique_ptr<cider::Program> program = parse(
        "func f(v: Value) -> string {"
        " match v { Value { i: x } => { return \"i\"; } default => { return \"?\"; } }"
        " try { risky(); } catch e: MyError { report(e); }"
        " for (x in xs) { print(x); }"
        "}",
        ok);
    check(ok, "parses");
}

// `func init` names the constructor: `init` lexes as a keyword, so the
// method parser accepts it explicitly rather than as an identifier.
void testMethodInit() {
    std::printf("method init\n");
    bool ok = false;
    std::unique_ptr<cider::Program> program = parse(
        "class Counter { var v: int; func init(n: int) { (*self).v = n; } }", ok);
    check(ok, "parses");
    const cider::ClassDecl& cls =
        static_cast<const cider::ClassDecl&>(*program->decls[0]);
    check(cls.init != nullptr, "constructor is stored as init");
    check(cls.init->params.size() == 1, "one init parameter");
    check(cls.methods.empty(), "no ordinary methods");
}

// `asm { "line" "line" }` joins the strings into one text with a newline
// between them and records each `{placeholder}` body raw, suffix included.
// `export var hits: int = 0;` lands in globals: every global binds
// cross-module already (no export enforcement exists), so the marker is a
// no-op there rather than a second global form.
void testExportVar() {
    std::printf("export var\n");
    bool ok = false;
    std::unique_ptr<cider::Program> program =
        parse("export var hits: int = 0;\n", ok);
    check(ok, "parses");
    check(program->globals.size() == 1, "one global");
    check(program->globals[0]->name == "hits", "the name survives");
}

void testDotQualifiedType() {
    std::printf("dot-qualified types\n");
    bool ok = false;
    std::unique_ptr<cider::Program> program = parse("type V = da.S;\n", ok);
    check(ok, "parses");
    if (!ok) {
        return;
    }
    const cider::TypeAlias* alias = nullptr;
    for (const std::unique_ptr<cider::Decl>& decl : program->decls) {
        if (const auto* al = dynamic_cast<const cider::TypeAlias*>(decl.get())) {
            alias = al;
        }
    }
    check(alias != nullptr, "the declaration is a TypeAlias");
    if (alias == nullptr) {
        return;
    }
    const auto* named =
        dynamic_cast<const cider::NamedType*>(alias->target.get());
    check(named != nullptr, "the target is a named type");
    if (named == nullptr) {
        return;
    }
    check(named->qualifier == "da", "the dot names the module");
    check(named->name == "S", "the name after the dot survives");
}

void testAsm() {
    std::printf("asm blocks\n");
    bool ok = false;
    std::unique_ptr<cider::Program> program = parse(
        "func f(sum: int) -> void {\n"
        "    asm {\n"
        "        \"mov rax, 60\"\n"
        "        \"mov edi, {sum}\"\n"
        "        \"mov {fd:q}, r15\"\n"
        "        \"syscall\"\n"
        "    }\n"
        "}\n", ok);
    check(ok, "parses");
    if (!ok) {
        return;
    }
    const cider::FuncDecl& fn =
        static_cast<const cider::FuncDecl&>(*program->decls[0]);
    const cider::AsmStmt* block = nullptr;
    for (const std::unique_ptr<cider::Stmt>& stmt : fn.body->stmts) {
        if (const auto* as = dynamic_cast<const cider::AsmStmt*>(stmt.get())) {
            block = as;
        }
    }
    check(block != nullptr, "the statement is an AsmStmt");
    if (block == nullptr) {
        return;
    }
    std::string joined = "lines join with newlines: got [" + block->text + "]";
    check(block->text == "mov rax, 60\nmov edi, {sum}\nmov {fd:q}, r15\nsyscall",
          joined.c_str());
    check(block->names.size() == 2, "one name per placeholder");
    if (block->names.size() == 2) {
        check(block->names[0] == "sum", "a bare name is kept");
        check(block->names[1] == "fd:q", "a width suffix stays raw");
    }
}

// `new [u8; 2] {[5, 6]}` is the list-literal element form of `new`
// (FEATURES §1.8): the bracketed elements are the initializers, exactly
// as if written without the brackets. `[0, ...]` stays the repeat form.
void testListLiteralNew() {
    std::printf("list-literal new\n");
    bool ok = false;
    std::unique_ptr<cider::Program> program =
        parse("var g: [u8; 2] = new [u8; 2] {[5, 6]};", ok);
    check(ok, "parses");
    if (program->globals.empty() || program->globals[0]->init == nullptr) {
        return;
    }
    const cider::NewArray& made =
        static_cast<const cider::NewArray&>(*program->globals[0]->init);
    check(!made.repeat, "a bracketed list is not the repeat form");
    check(made.inits.size() == 2, "both elements are initializers");
    program = parse("var h: [u8; 3] = new [u8; 3] {[0, ...]};", ok);
    check(ok, "the repeat form still parses");
    if (!program->globals.empty() && program->globals[0]->init != nullptr) {
        const cider::NewArray& rep =
            static_cast<const cider::NewArray&>(*program->globals[0]->init);
        check(rep.repeat, "ellipsis keeps the repeat form");
    }
}

}  // namespace

int main() {
    std::printf("parser_test — syntax alone\n\n");
    testPrecedence();
    testWordLogic();
    testAsBelowUnary();
    testMinusAndArrow();
    testCompound();
    testMissingInit();
    testGenericCall();
    testControlForms();
    testMethodInit();
    testExportVar();
    testDotQualifiedType();
    testIncrDecr();
    testAsm();
    testListLiteralNew();
    std::printf("\n%d checks, %s\n", checks, failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
