// parser.h — syntax alone. LAYERS.md §3.2.
//
// Accepts or rejects on syntax alone. Two `if` arms with different spellings
// are the same node; two spellings of one instantiated type are parsed, not
// resolved. The parser never resolves a name to a declaration, never infers a
// type, and never decides whether a program is well-formed — in particular it
// never consults the resolver, because a parser that does is the v2
// sizeof-parses-as-a-callable bug.
//
// What the parser does own, completely:
//   - the precedence table (FEATURES.md §8.1a), levels 0 through 14
//   - folding a leading `-` into a numeric literal (`var x: i8 = -128`)
//   - desugaring `a op= b` to an Assign carrying the operator, target written
//     once and evaluated once (SEMANTICS.md §2.2)
//   - the `max::<int>(a, b)` expression form (type positions stay `Box<int>`)
//   - reading `->` as Minus followed by Gt in return-type position only,
//     because the member-access arrow does not exist and must not lex as one
//
// Errors are diagnostics with positions, and parsing continues after each one:
// a file with three bad declarations reports three diagnostics. Emitting IR
// for a program with errors is forbidden (LAYERS.md §7), so the driver checks
// ok() and stops — the parser's job is a complete error list, not a partial
// tree that looks usable.

#ifndef CIDER_PARSE_PARSER_H
#define CIDER_PARSE_PARSER_H

#include <memory>
#include <string>
#include <vector>

#include "ast/ast.h"
#include "lexer/token.h"

namespace cider {

struct ParseError {
    Position pos;
    std::string message;
};

class Parser {
public:
    // `interfaceMode` accepts the declaration-only shape of a `.cidm`:
    // bodyless `func`/method declarations and globals without initialisers.
    // Ordinary source keeps requiring bodies, so a missed body stays a
    // diagnostic rather than an accidental extern.
    Parser(std::string file, std::vector<Token> tokens,
           bool interfaceMode = false);

    // Parses the whole file. Never null; if ok() is false the tree is partial
    // and the driver must not use it.
    std::unique_ptr<Program> parse();
    const std::vector<ParseError>& errors() const { return errors_; }
    bool ok() const { return errors_.empty(); }

private:
    // Token movement. `save`/`restore` exist for exactly one decision: `for (`
    // followed by `var x in` or `x in` against the C-style form.
    const Token& peek() const;
    const Token& peekAt(uint64_t n) const;
    const Token& advance();
    bool atEnd() const;
    bool check(TokenKind kind) const;
    bool match(TokenKind kind);
    // Matches `>`, splitting a `>>` in two. `Box<Vec<int>>` lexes its end as
    // one token; in type-argument position that token can only be two closes,
    // so the parser separates them rather than failing.
    bool matchGt();
    uint64_t save() const { return cursor_; }
    void restore(uint64_t cursor) { cursor_ = cursor; }

    // Records a diagnostic and returns an Error-flavoured null: callers that
    // cannot continue return nullptr up to the next synchronization point.
    void fail(Position pos, const std::string& message);
    void failHere(const std::string& message);

    // Synchronization: after an error, skip to the next declaration or
    // statement boundary so one bad form does not hide the rest of the file.
    void syncToDecl();
    void syncToStmt();

    // Declarations and statements.
    std::unique_ptr<Decl> parseDecl();
    std::unique_ptr<VarDecl> parseGlobal();
    std::unique_ptr<Decl> parseFuncDecl(bool exported);
    // Consumes `operator` and one allowed binary operator spelling, storing
    // the canonical link-safe name. Rejects every operator this slice cannot
    // declare yet.
    bool parseOperatorName(std::string& out);
    std::unique_ptr<Decl> parseStructDecl(bool exported);
    std::unique_ptr<Decl> parseClassDecl(bool exported);
    std::unique_ptr<Decl> parseInterfaceDecl();
    std::unique_ptr<Decl> parseUnionDecl(bool exported);
    std::unique_ptr<Decl> parseEnumDecl(bool exported);
    std::unique_ptr<Decl> parseTypeAlias(bool exported);
    std::unique_ptr<Decl> parseConstDecl();
    std::unique_ptr<Decl> parseExtern();
    std::unique_ptr<Decl> parseImport();
    // One segment of a dotted module path: an identifier, or a type
    // spelling (`string` in `std.string`). The path is not a type
    // position, so the keyword is just a name there.
    bool matchModuleSegment(std::string& out);
    // `# experimental(name)`. Returns false for an unknown behaviour, which is a
    // diagnostic naming it rather than a silent no-op.
    bool parseDirective(Program& program);
    std::vector<Param> parseParams();
    std::unique_ptr<TypeExpr> parseReturnType();
    FieldDecl parseField();
    MethodDecl parseMethod();
    std::unique_ptr<Expr> parseStructLiteral(std::unique_ptr<TypeExpr> type,
                                            Position start);
    std::unique_ptr<Stmt> parseStmt();
    std::unique_ptr<Stmt> parseBlock();
    std::unique_ptr<VarDecl> parseVarDecl(bool global = false);
    std::unique_ptr<Stmt> parseIf();
    std::unique_ptr<Stmt> parseWhile();
    std::unique_ptr<Stmt> parseFor();
    std::unique_ptr<Stmt> parseMatch();
    Pattern parsePattern();
    std::unique_ptr<Stmt> parseDefer();
    std::unique_ptr<Stmt> parseReturn();
    std::unique_ptr<Stmt> parseDelete();
    std::unique_ptr<Stmt> parseAsm();

    // Types. `parseType` is the full spelling; `parsePrimaryType` is one
    // without the leading `*`s, for positions where pointer depth is written
    // by the caller.
    std::unique_ptr<TypeExpr> parseType();
    std::unique_ptr<TypeExpr> parsePrimaryType();
    // 8.4: the `func(...) -> T` body, shared by the plain spelling and the
    // `msabi`/`sysv`-marked one.
    std::unique_ptr<TypeExpr> parseFuncSig();

    // Expressions, Pratt-parsed. Levels run tight (150) to loose (10); the
    // table in FEATURES.md §8.1a is stated loosest-first, this engine runs
    // tightest-first, and the mapping is written next to each method.
    std::unique_ptr<Expr> parseExpr();
    std::unique_ptr<Expr> parseExprWith(int minLevel);
    std::unique_ptr<Expr> parsePrefix();
    std::unique_ptr<Expr> parsePostfix(std::unique_ptr<Expr> base);
    std::unique_ptr<Expr> parseNew(bool tryNew);
    std::unique_ptr<Expr> parseSizeof();
    std::unique_ptr<Expr> parseAlignof();
    std::unique_ptr<Expr> parseFuncLit();
    std::unique_ptr<Expr> parseStringParts(const Token& token);
    // Parses one expression out of already-lexed interpolation tokens.
    std::unique_ptr<Expr> parseBraced(const std::vector<Token>& tokens, Position pos);

    std::string file_;
    // A bare `T { ... }` literal is refused where a `{` follows an
    // expression that cannot be a literal head (a match scrutinee).
    bool noStructLit_ = false;
    // `.cidm` declarations-only mode. See the constructor comment.
    bool interfaceMode_ = false;
    std::vector<Token> tokens_;
    uint64_t cursor_ = 0;
    std::vector<ParseError> errors_;
};

}  // namespace cider

#endif  // CIDER_PARSE_PARSER_H
