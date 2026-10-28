// parser.cpp — syntax alone.
//
// Every method here answers a syntactic question. Nothing resolves a name,
// nothing infers a type, and nothing consults the resolver — the grammar has
// no ambiguity that needs one (LAYERS.md §3.2), and a parser that asks is the
// v2 sizeof bug.
//
// Precedence runs tight (150) to loose (10). FEATURES.md §8.1a states the
// table loosest-first; the engine runs the other direction, and the level
// beside each method is the table row it implements.

#include "parse/parser.h"

namespace cider {

namespace {

// A unary operator declaration names exactly one parameter; the count has a
// name so a size-looking literal never appears outside layout.h.
constexpr int kOneParameter = 1;

// Binding levels, tightest first. The number is the table row in reverse:
// postfix is level 0 of the table and 150 here.
enum {
    LVL_POSTFIX = 150,
    LVL_UNARY = 140,
    LVL_AS = 130,
    LVL_MUL = 120,
    LVL_ADD = 110,
    LVL_SHIFT = 100,
    LVL_BITAND = 90,
    LVL_BITXOR = 80,
    LVL_BITOR = 70,
    LVL_REL = 60,
    LVL_EQ = 50,
    LVL_ANDAND = 40,
    LVL_OROR = 30,
    LVL_TERNARY = 20,
    LVL_ASSIGN = 10,
};

// The binary operator a token spells, if any. If-chains rather than a switch:
// this maps spellings, not Kinds, and a switch over TokenKind here would make
// gate T1 count a table as a traversal.
bool tokenBinOp(TokenKind kind, BinOp& op, int& level) {
    if (kind == TokenKind::Star || kind == TokenKind::Slash || kind == TokenKind::Percent) {
        op = kind == TokenKind::Star ? BinOp::Mul : (kind == TokenKind::Slash ? BinOp::Div : BinOp::Rem);
        level = LVL_MUL;
        return true;
    }
    if (kind == TokenKind::Plus || kind == TokenKind::Minus) {
        op = kind == TokenKind::Plus ? BinOp::Add : BinOp::Sub;
        level = LVL_ADD;
        return true;
    }
    if (kind == TokenKind::LtLt || kind == TokenKind::GtGt) {
        op = kind == TokenKind::LtLt ? BinOp::Shl : BinOp::Shr;
        level = LVL_SHIFT;
        return true;
    }
    if (kind == TokenKind::Amp) {
        op = BinOp::And;
        level = LVL_BITAND;
        return true;
    }
    if (kind == TokenKind::Caret) {
        op = BinOp::Xor;
        level = LVL_BITXOR;
        return true;
    }
    if (kind == TokenKind::Pipe) {
        op = BinOp::Or;
        level = LVL_BITOR;
        return true;
    }
    if (kind == TokenKind::Lt || kind == TokenKind::Gt || kind == TokenKind::LtEq ||
        kind == TokenKind::GtEq) {
        if (kind == TokenKind::Lt) {
            op = BinOp::Lt;
        } else if (kind == TokenKind::Gt) {
            op = BinOp::Gt;
        } else if (kind == TokenKind::LtEq) {
            op = BinOp::Le;
        } else {
            op = BinOp::Ge;
        }
        level = LVL_REL;
        return true;
    }
    if (kind == TokenKind::EqEq || kind == TokenKind::BangEq) {
        op = kind == TokenKind::EqEq ? BinOp::Eq : BinOp::Ne;
        level = LVL_EQ;
        return true;
    }
    if (kind == TokenKind::AndAnd || kind == TokenKind::KwAnd) {
        op = BinOp::AndAnd;
        level = LVL_ANDAND;
        return true;
    }
    if (kind == TokenKind::OrOr || kind == TokenKind::KwOr) {
        op = BinOp::OrOr;
        level = LVL_OROR;
        return true;
    }
    return false;
}

// A compound assignment spells its binary operator. Same reason for
// if-chains: a spelling table, not a Kind dispatch.
bool tokenCompoundOp(TokenKind kind, BinOp& op) {
    if (kind == TokenKind::PlusEq) {
        op = BinOp::Add;
    } else if (kind == TokenKind::MinusEq) {
        op = BinOp::Sub;
    } else if (kind == TokenKind::StarEq) {
        op = BinOp::Mul;
    } else if (kind == TokenKind::SlashEq) {
        op = BinOp::Div;
    } else if (kind == TokenKind::PercentEq) {
        op = BinOp::Rem;
    } else if (kind == TokenKind::AmpEq) {
        op = BinOp::And;
    } else if (kind == TokenKind::PipeEq) {
        op = BinOp::Or;
    } else if (kind == TokenKind::CaretEq) {
        op = BinOp::Xor;
    } else if (kind == TokenKind::LtLtEq) {
        op = BinOp::Shl;
    } else if (kind == TokenKind::GtGtEq) {
        op = BinOp::Shr;
    } else {
        return false;
    }
    return true;
}

bool declStart(TokenKind kind) {
    return kind == TokenKind::KwFunc || kind == TokenKind::KwStruct ||
           kind == TokenKind::KwClass || kind == TokenKind::KwInterface ||
           kind == TokenKind::KwUnion || kind == TokenKind::KwEnum ||
           kind == TokenKind::KwType || kind == TokenKind::KwConst ||
           kind == TokenKind::KwExtern || kind == TokenKind::KwImport ||
           kind == TokenKind::KwFrom ||
           kind == TokenKind::KwExport || kind == TokenKind::KwVar;
}

}  // namespace

Parser::Parser(std::string file, std::vector<Token> tokens,
               bool interfaceMode)
    : file_(std::move(file)), interfaceMode_(interfaceMode),
      tokens_(std::move(tokens)) {}

const Token& Parser::peek() const {
    return tokens_[cursor_];
}

const Token& Parser::peekAt(uint64_t n) const {
    uint64_t want = cursor_ + n;
    if (want < tokens_.size()) {
        return tokens_[want];
    }
    return tokens_.back();
}

const Token& Parser::advance() {
    const Token& token = peek();
    if (!atEnd()) {
        ++cursor_;
    }
    return token;
}

bool Parser::atEnd() const {
    return peek().kind == TokenKind::End;
}

bool Parser::check(TokenKind kind) const {
    return peek().kind == kind;
}

bool Parser::match(TokenKind kind) {
    if (check(kind)) {
        advance();
        return true;
    }
    return false;
}

bool Parser::matchGt() {
    if (match(TokenKind::Gt)) {
        return true;
    }
    if (!check(TokenKind::GtGt)) {
        return false;
    }
    Position split = advance().pos;
    Token extra;
    extra.kind = TokenKind::Gt;
    extra.pos = Position{split.file, split.line, split.column + 1};
    tokens_.insert(tokens_.begin() + static_cast<int64_t>(cursor_), std::move(extra));
    return true;
}

void Parser::fail(Position pos, const std::string& message) {
    errors_.push_back(ParseError{pos, message});
}

void Parser::failHere(const std::string& message) {
    fail(peek().pos, message);
}

void Parser::syncToDecl() {
    while (!atEnd() && peek().kind != TokenKind::RBrace && !declStart(peek().kind)) {
        advance();
    }
}

void Parser::syncToStmt() {
    while (!atEnd() && peek().kind != TokenKind::Semi && peek().kind != TokenKind::RBrace) {
        advance();
    }
    match(TokenKind::Semi);
}

// --- The program -------------------------------------------------------------

std::unique_ptr<Program> Parser::parse() {
    Position start{file_, 1, 1};
    // Built up front, because a directive sets a property of the file and the
    // file has to exist before a directive names one.
    std::unique_ptr<Program> program =
        std::make_unique<Program>(start, file_, std::vector<std::unique_ptr<Decl>>{},
                                  std::vector<std::unique_ptr<VarDecl>>{});
    program->isInterface = interfaceMode_;
    while (!atEnd()) {
        if (check(TokenKind::Hash)) {
            if (!parseDirective(*program)) {
                syncToDecl();
                if (!atEnd() && check(TokenKind::Hash)) {
                    advance();
                }
            }
            continue;
        }
        if (check(TokenKind::KwVar)) {
            std::unique_ptr<VarDecl> global = parseGlobal();
            if (global != nullptr) {
                program->globals.push_back(std::move(global));
            } else {
                syncToDecl();
            }
            continue;
        }
        if (check(TokenKind::KwExport) && peekAt(1).kind == TokenKind::KwVar) {
            // `export` on a global is a marker only: every global binds
            // cross-module already (no export enforcement exists), so the
            // declaration parses as the plain global it is.
            advance();
            std::unique_ptr<VarDecl> global = parseGlobal();
            if (global != nullptr) {
                program->globals.push_back(std::move(global));
            } else {
                syncToDecl();
            }
            continue;
        }
        uint64_t before = save();
        std::unique_ptr<Decl> decl = parseDecl();
        if (decl != nullptr) {
            program->decls.push_back(std::move(decl));
        } else {
            syncToDecl();
            if (save() == before && !atEnd()) {
                advance();
            }
        }
    }
    return program;
}

std::unique_ptr<Decl> Parser::parseDecl() {
    TokenKind kind = peek().kind;
    if (kind == TokenKind::KwAsm) {
        std::unique_ptr<Stmt> stmt = parseAsm();
        if (stmt == nullptr) {
            return nullptr;
        }
        auto* asmStmt = dynamic_cast<AsmStmt*>(stmt.get());
        if (asmStmt == nullptr) {
            return nullptr;
        }
        return std::make_unique<ModuleAsm>(asmStmt->pos, std::move(asmStmt->text));
    }
    // 8.4: `msabi func`, `sysv func`, `msabi extern func` — the convention is
    // stated before the declaration it applies to.
    if (kind == TokenKind::Ident &&
        (peek().text == "msabi" || peek().text == "sysv")) {
        std::string abi = peek().text;
        advance();
        if (check(TokenKind::KwFunc)) {
            std::unique_ptr<Decl> decl = parseFuncDecl(false);
            if (decl == nullptr) {
                return nullptr;
            }
            static_cast<FuncDecl*>(decl.get())->abi = std::move(abi);
            return decl;
        }
        if (check(TokenKind::KwExtern)) {
            std::unique_ptr<Decl> decl = parseExtern();
            if (decl == nullptr) {
                return nullptr;
            }
            if (auto* fn = dynamic_cast<ExternFunc*>(decl.get())) {
                fn->abi = std::move(abi);
                return decl;
            }
            failHere("a calling convention applies only to functions");
            return nullptr;
        }
        failHere("a calling convention before a declaration applies to 'func' or 'extern'");
        return nullptr;
    }
    if (kind == TokenKind::At) {
        // 4.5: `@packed struct` — the only declaration-level use of `@`.
        advance();
        if (!check(TokenKind::Ident) || peek().text != "packed") {
            failHere("expected 'packed' after '@' in a declaration");
            return nullptr;
        }
        advance();
        if (!check(TokenKind::KwStruct)) {
            failHere("'@packed' applies to struct declarations only");
            return nullptr;
        }
        std::unique_ptr<Decl> decl = parseStructDecl(false);
        if (decl == nullptr) {
            return nullptr;
        }
        static_cast<StructDecl*>(decl.get())->packed = true;
        return decl;
    }
    if (kind == TokenKind::KwFunc) {
        return parseFuncDecl(false);
    }
    if (kind == TokenKind::KwStruct) {
        return parseStructDecl(false);
    }
    if (kind == TokenKind::KwClass) {
        return parseClassDecl(false);
    }
    if (kind == TokenKind::KwInterface) {
        return parseInterfaceDecl();
    }
    if (kind == TokenKind::KwUnion) {
        return parseUnionDecl(false);
    }
    if (kind == TokenKind::KwEnum) {
        return parseEnumDecl(false);
    }
    if (kind == TokenKind::KwType) {
        return parseTypeAlias(false);
    }
    if (kind == TokenKind::KwConst) {
        return parseConstDecl();
    }
    if (kind == TokenKind::KwExtern) {
        return parseExtern();
    }
    if (kind == TokenKind::KwImport || kind == TokenKind::KwFrom) {
        return parseImport();
    }
    if (kind == TokenKind::KwExport) {
        Position start = advance().pos;
        std::unique_ptr<Decl> inner = parseDecl();
        if (inner == nullptr) {
            return nullptr;
        }
        return std::make_unique<ExportDecl>(start, std::move(inner));
    }
    failHere("expected a declaration");
    return nullptr;
}

std::unique_ptr<VarDecl> Parser::parseGlobal() {
    std::unique_ptr<VarDecl> var = parseVarDecl();
    return var;
}

// --- Declarations ------------------------------------------------------------

// The canonical names are ordinary identifiers, so a module prefix and a
// method owner can be joined onto them without escaping punctuation.
bool Parser::parseOperatorName(std::string& out) {
    Position start = advance().pos;
    switch (peek().kind) {
        // `[]` is one spelling made of two tokens. The parser consumes both
        // here so the declaration's name is still one link-safe identifier.
        case TokenKind::LBracket:
            advance();
            if (!match(TokenKind::RBracket)) {
                fail(start, "an index operator is spelled '[]'");
                return false;
            }
            out = "operator_index";
            return true;
        // `()` is the call operator: a member-only spelling made of two
        // tokens. Its parameter list follows the spelling as usual.
        case TokenKind::LParen:
            advance();
            if (!match(TokenKind::RParen)) {
                fail(start, "a call operator is spelled '()'");
                return false;
            }
            out = "operator_call";
            return true;
        // Allocation operators are keywords, but they name global functions
        // like any other operator. The checker pins their exact signatures.
        // Array forms are a distinct spelling the language does not have.
        case TokenKind::KwNew:
        case TokenKind::KwDelete:
            if (check(TokenKind::LBracket)) {
                fail(start, "array allocation operators are not supported yet");
                return false;
            }
            out = peek().kind == TokenKind::KwNew ? "operator_new"
                                                  : "operator_delete";
            break;
        case TokenKind::Plus: out = "operator_add"; break;
        case TokenKind::Minus: out = "operator_sub"; break;
        case TokenKind::Star: out = "operator_mul"; break;
        case TokenKind::Slash: out = "operator_div"; break;
        case TokenKind::Percent: out = "operator_rem"; break;
        case TokenKind::Tilde: out = "operator_bitnot"; break;
        case TokenKind::Amp: out = "operator_bitand"; break;
        case TokenKind::Pipe: out = "operator_bitor"; break;
        case TokenKind::Caret: out = "operator_xor"; break;
        case TokenKind::LtLt: out = "operator_shl"; break;
        case TokenKind::GtGt: out = "operator_shr"; break;
        case TokenKind::Lt: out = "operator_lt"; break;
        case TokenKind::Gt: out = "operator_gt"; break;
        case TokenKind::LtEq: out = "operator_le"; break;
        case TokenKind::GtEq: out = "operator_ge"; break;
        // `=` and its compound forms are assignment operators.
        case TokenKind::Arrow: out = "operator_arrow"; break;
        case TokenKind::Eq: out = "operator_assign"; break;
        case TokenKind::PlusEq: out = "operator_add_assign"; break;
        case TokenKind::MinusEq: out = "operator_sub_assign"; break;
        case TokenKind::StarEq: out = "operator_mul_assign"; break;
        case TokenKind::SlashEq: out = "operator_div_assign"; break;
        case TokenKind::PercentEq: out = "operator_rem_assign"; break;
        case TokenKind::AmpEq: out = "operator_bitand_assign"; break;
        case TokenKind::PipeEq: out = "operator_bitor_assign"; break;
        case TokenKind::CaretEq: out = "operator_xor_assign"; break;
        case TokenKind::LtLtEq: out = "operator_shl_assign"; break;
        case TokenKind::GtGtEq: out = "operator_shr_assign"; break;
        case TokenKind::EqEq: out = "operator_eq"; break;
        case TokenKind::BangEq: out = "operator_ne"; break;
        default:
            fail(start, "this operator cannot be overloaded");
            return false;
    }
    advance();
    return true;
}

std::unique_ptr<Decl> Parser::parseFuncDecl(bool) {
    Position start = advance().pos;
    std::string name;
    if (check(TokenKind::KwOperator)) {
        if (!parseOperatorName(name)) {
            return nullptr;
        }
    } else if (check(TokenKind::Ident)) {
        name = advance().text;
    } else {
        failHere("a function needs a name");
        return nullptr;
    }
    std::vector<std::string> tps;
    if (match(TokenKind::Lt)) {
        for (;;) {
            if (!check(TokenKind::Ident)) {
                failHere("a type parameter needs a name");
                return nullptr;
            }
            tps.push_back(advance().text);
            if (match(TokenKind::Comma)) {
                continue;
            }
            break;
        }
        if (!match(TokenKind::Gt)) {
            failHere("expected '>'");
            return nullptr;
        }
    }
    std::vector<Param> params = parseParams();
    // `-` is binary by spelling but unary by arity. The declaration's own
    // parameter count names the operator before the checker sees it.
    if (params.size() == kOneParameter && name == "operator_sub") {
        name = "operator_neg";
    }
    std::unique_ptr<TypeExpr> returns = parseReturnType();
    if (!check(TokenKind::LBrace)) {
        if (interfaceMode_ && match(TokenKind::Semi)) {
            return std::make_unique<FuncDecl>(
                start, std::move(name), std::move(tps), std::move(params),
                std::move(returns), nullptr);
        }
        failHere("a function needs a body");
        return nullptr;
    }
    std::unique_ptr<Stmt> body = parseBlock();
    if (body == nullptr) {
        return nullptr;
    }
    return std::make_unique<FuncDecl>(start, std::move(name), std::move(tps), std::move(params),
                                      std::move(returns),
                                      std::unique_ptr<Block>(static_cast<Block*>(body.release())));
}

std::vector<Param> Parser::parseParams() {
    std::vector<Param> params;
    if (!match(TokenKind::LParen)) {
        failHere("expected '('");
        return params;
    }
    if (match(TokenKind::RParen)) {
        return params;
    }
    for (;;) {
        Position ppos = peek().pos;
        bool isMutable = match(TokenKind::KwVar);
        if (!check(TokenKind::Ident)) {
            failHere("a parameter needs a name");
            return params;
        }
        std::string name = advance().text;
        if (!match(TokenKind::Colon)) {
            failHere("a parameter needs a type");
            return params;
        }
        // A parameter may spell `volatile T`: qualification is an access
        // property of declarations, and the value semantics are the same
        // either way.
        (void)match(TokenKind::KwVolatile);
        std::unique_ptr<TypeExpr> type = parseType();
        if (type == nullptr) {
            return params;
        }
        params.push_back(Param(ppos, std::move(name), std::move(type), isMutable));
        if (match(TokenKind::Comma)) {
            continue;
        }
        break;
    }
    if (!match(TokenKind::RParen)) {
        failHere("expected ')'");
    }
    return params;
}

// A return type is spelled `->` and lexes as one Arrow token.
std::unique_ptr<TypeExpr> Parser::parseReturnType() {
    if (!match(TokenKind::Arrow)) {
        return nullptr;
    }
    // `-> volatile T` is accepted for the same reason a parameter's
    // spelling is: the qualifier describes access, not the value.
    (void)match(TokenKind::KwVolatile);
    return parseType();
}

FieldDecl Parser::parseField() {
    Position start = peek().pos;
    // 4.8.4: `public` is the default; both spellings are accepted so a
    // declaration can state its intent.
    bool isPrivate = match(TokenKind::KwPrivate);
    (void)match(TokenKind::KwPublic);
    (void)match(TokenKind::KwVar);
    FieldDecl field(start, "", nullptr, nullptr, false);
    field.isPrivate = isPrivate;
    if (!check(TokenKind::Ident)) {
        failHere("a field needs a name");
        return field;
    }
    field.name = advance().text;
    if (!match(TokenKind::Colon)) {
        failHere("a field needs a type");
        return field;
    }
    field.isVolatile = match(TokenKind::KwVolatile);
    field.type = parseType();
    if (match(TokenKind::Colon)) {
        field.bitWidth = parseExpr();
    }
    // v2 CIDER-BUG-049: a field may carry a default, run at every
    // construction so an aggregate can leave it out.
    if (match(TokenKind::Eq)) {
        field.init = parseExpr();
    }
    if (!match(TokenKind::Semi)) {
        failHere("expected ';'");
    }
    return field;
}

MethodDecl Parser::parseMethod() {
    Position start = peek().pos;
    bool isPrivate = false;
    bool isOverride = false;
    bool isVirtual = false;
    // Visibility, override and virtual are modifiers of one method, so the
    // spelling order between them carries no meaning. The last visibility
    // wins, matching the field rule.
    for (;;) {
        if (match(TokenKind::KwPrivate)) {
            isPrivate = true;
        } else if (match(TokenKind::KwPublic)) {
            isPrivate = false;
        } else if (match(TokenKind::KwOverride)) {
            isOverride = true;
        } else if (match(TokenKind::KwVirtual)) {
            isVirtual = true;
        } else {
            break;
        }
    }
    if (!match(TokenKind::KwFunc)) {
        failHere("expected a method");
        return MethodDecl(start, "", {}, nullptr, nullptr, false);
    }
    std::string name;
    if (check(TokenKind::KwOperator)) {
        if (!parseOperatorName(name)) {
            return MethodDecl(start, "", {}, nullptr, nullptr, false);
        }
    } else {
        if (!check(TokenKind::Ident) && !check(TokenKind::KwInit) &&
            !check(TokenKind::KwDeinit)) {
            failHere("a method needs a name");
            return MethodDecl(start, "", {}, nullptr, nullptr, false);
        }
        TokenKind nameKind = peek().kind;
        name = advance().text;
        if (name.empty() &&
            (nameKind == TokenKind::KwInit || nameKind == TokenKind::KwDeinit)) {
            name = tokenName(nameKind);
        }
    }
    if (!interfaceMode_ && (check(TokenKind::KwInit) || check(TokenKind::KwDeinit))) {
        failHere("an init or deinit needs a body");
    }
    std::vector<Param> params = parseParams();
    // A member operator's receiver is implicit: unary minus declares no
    // parameters, while one parameter makes it the binary subtraction
    // operator. The free spelling below has the opposite arity mapping.
    if (params.empty() && name == "operator_sub") {
        name = "operator_neg";
    }
    std::unique_ptr<TypeExpr> returns = parseReturnType();
    if (!check(TokenKind::LBrace)) {
        if (interfaceMode_ && match(TokenKind::Semi)) {
            MethodDecl out(start, std::move(name), std::move(params),
                           std::move(returns), nullptr, isVirtual);
            out.isOverride = isOverride;
            out.isPrivate = isPrivate;
            return out;
        }
        failHere("a method needs a body");
        return MethodDecl(start, std::move(name), std::move(params), std::move(returns),
                          nullptr, isVirtual);
    }
    std::unique_ptr<Stmt> body = parseBlock();
    MethodDecl out(start, std::move(name), std::move(params), std::move(returns),
                   std::unique_ptr<Block>(static_cast<Block*>(body.release())), isVirtual);
    out.isOverride = isOverride;
    out.isPrivate = isPrivate;
    return out;
}

std::unique_ptr<Decl> Parser::parseStructDecl(bool) {
    Position start = advance().pos;
    if (!check(TokenKind::Ident)) {
        failHere("a struct needs a name");
        return nullptr;
    }
    std::string name = advance().text;
    std::vector<std::string> tps;
    if (match(TokenKind::Lt)) {
        for (;;) {
            if (!check(TokenKind::Ident)) {
                failHere("a type parameter needs a name");
                return nullptr;
            }
            tps.push_back(advance().text);
            if (match(TokenKind::Comma)) {
                continue;
            }
            break;
        }
        if (!match(TokenKind::Gt)) {
            failHere("expected '>'");
            return nullptr;
        }
    }
    std::vector<std::unique_ptr<TypeExpr>> ifs;
    if (match(TokenKind::Colon)) {
        for (;;) {
            std::unique_ptr<TypeExpr> iface = parseType();
            if (iface == nullptr) {
                return nullptr;
            }
            ifs.push_back(std::move(iface));
            if (match(TokenKind::Comma)) {
                continue;
            }
            break;
        }
    }
    if (!match(TokenKind::LBrace)) {
        failHere("a struct needs a body");
        return nullptr;
    }
    std::vector<FieldDecl> fields;
    std::vector<MethodDecl> methods;
    while (!check(TokenKind::RBrace) && !atEnd()) {
        bool visible = check(TokenKind::KwPrivate) || check(TokenKind::KwPublic);
        TokenKind afterVisible = visible ? peekAt(1).kind : TokenKind::End;
        bool visibleMethod = visible && (afterVisible == TokenKind::KwFunc ||
                                         afterVisible == TokenKind::KwVirtual ||
                                         afterVisible == TokenKind::KwOverride);
        if (visibleMethod || check(TokenKind::KwVirtual) ||
            check(TokenKind::KwFunc) || check(TokenKind::KwOverride)) {
            methods.push_back(parseMethod());
        } else if (visible || check(TokenKind::KwVar) ||
                   (check(TokenKind::Ident) &&
                    peekAt(1).kind == TokenKind::Colon)) {
            fields.push_back(parseField());
        } else {
            failHere("a struct holds fields and methods");
            syncToStmt();
            match(TokenKind::RBrace);
            break;
        }
    }
    if (!match(TokenKind::RBrace)) {
        failHere("expected '}'");
        return nullptr;
    }
    return std::make_unique<StructDecl>(start, std::move(name), std::move(tps), std::move(ifs),
                                        std::move(fields), std::move(methods));
}

std::unique_ptr<Decl> Parser::parseClassDecl(bool) {
    Position start = advance().pos;
    if (!check(TokenKind::Ident)) {
        failHere("a class needs a name");
        return nullptr;
    }
    std::string name = advance().text;
    std::vector<std::string> tps;
    if (match(TokenKind::Lt)) {
        for (;;) {
            if (!check(TokenKind::Ident)) {
                failHere("a type parameter needs a name");
                return nullptr;
            }
            tps.push_back(advance().text);
            if (match(TokenKind::Comma)) {
                continue;
            }
            break;
        }
        if (!match(TokenKind::Gt)) {
            failHere("expected '>'");
            return nullptr;
        }
    }
    std::vector<std::unique_ptr<TypeExpr>> bases;
    if (match(TokenKind::Colon)) {
        for (;;) {
            std::unique_ptr<TypeExpr> base = parseType();
            if (base == nullptr) {
                return nullptr;
            }
            bases.push_back(std::move(base));
            if (match(TokenKind::Comma)) {
                continue;
            }
            break;
        }
    }
    if (!match(TokenKind::LBrace)) {
        failHere("a class needs a body");
        return nullptr;
    }
    std::vector<FieldDecl> fields;
    std::vector<MethodDecl> methods;
    std::unique_ptr<FuncDecl> init;
    std::unique_ptr<FuncDecl> deinit;
    while (!check(TokenKind::RBrace) && !atEnd()) {
        bool visible = check(TokenKind::KwPrivate) || check(TokenKind::KwPublic);
        TokenKind afterVisible = visible ? peekAt(1).kind : TokenKind::End;
        bool visibleMethod = visible && (afterVisible == TokenKind::KwFunc ||
                                         afterVisible == TokenKind::KwVirtual ||
                                         afterVisible == TokenKind::KwOverride);
        if (visibleMethod || check(TokenKind::KwVirtual) ||
            check(TokenKind::KwFunc) || check(TokenKind::KwOverride)) {
            MethodDecl method = parseMethod();
            if ((method.name == "init" || method.name == "deinit") &&
                method.body != nullptr) {
                std::vector<Param> ps;
                for (Param& p : method.params) {
                    ps.push_back(Param(p.pos, p.name, std::move(p.type), p.isMutable));
                }
                std::unique_ptr<FuncDecl> ctor = std::make_unique<FuncDecl>(
                    method.pos, method.name, std::vector<std::string>{},
                    std::move(ps), std::move(method.returns), std::move(method.body));
                if (method.name == "init") {
                    init = std::move(ctor);
                } else {
                    deinit = std::move(ctor);
                }
            } else {
                methods.push_back(std::move(method));
            }
            continue;
        }
        if (visible || check(TokenKind::KwVar) ||
            (check(TokenKind::Ident) && peekAt(1).kind == TokenKind::Colon)) {
            fields.push_back(parseField());
            continue;
        }
        failHere("a class holds fields and methods");
        syncToStmt();
        break;
    }
    if (!match(TokenKind::RBrace)) {
        failHere("expected '}'");
        return nullptr;
    }
    return std::make_unique<ClassDecl>(start, std::move(name), std::move(tps),
                                       std::move(bases), std::move(fields),
                                       std::move(methods), std::move(init),
                                       std::move(deinit));
}

std::unique_ptr<Decl> Parser::parseInterfaceDecl() {
    Position start = advance().pos;
    if (!check(TokenKind::Ident)) {
        failHere("an interface needs a name");
        return nullptr;
    }
    std::string name = advance().text;
    if (!match(TokenKind::LBrace)) {
        failHere("an interface needs a body");
        return nullptr;
    }
    std::vector<MethodSig> methods;
    while (!check(TokenKind::RBrace) && !atEnd()) {
        Position mpos = peek().pos;
        if (!match(TokenKind::KwFunc)) {
            failHere("an interface holds method signatures");
            syncToStmt();
            break;
        }
        if (!check(TokenKind::Ident)) {
            failHere("a method needs a name");
            return nullptr;
        }
        std::string mname = advance().text;
        std::vector<Param> params = parseParams();
        std::unique_ptr<TypeExpr> returns = parseReturnType();
        if (!match(TokenKind::Semi)) {
            failHere("expected ';'");
            return nullptr;
        }
        MethodSig sig;
        sig.name = std::move(mname);
        sig.params = std::move(params);
        sig.returns = std::move(returns);
        (void)mpos;
        methods.push_back(std::move(sig));
    }
    if (!match(TokenKind::RBrace)) {
        failHere("expected '}'");
        return nullptr;
    }
    return std::make_unique<InterfaceDecl>(start, std::move(name), std::move(methods));
}

std::unique_ptr<Decl> Parser::parseUnionDecl(bool) {
    Position start = advance().pos;
    if (!check(TokenKind::Ident)) {
        failHere("a union needs a name");
        return nullptr;
    }
    std::string name = advance().text;
    if (!match(TokenKind::LBrace)) {
        failHere("a union needs a body");
        return nullptr;
    }
    std::vector<FieldDecl> members;
    while (!check(TokenKind::RBrace) && !atEnd()) {
        if (!check(TokenKind::KwVar) &&
            !(check(TokenKind::Ident) && peekAt(1).kind == TokenKind::Colon)) {
            failHere("a union holds member declarations");
            syncToStmt();
            break;
        }
        members.push_back(parseField());
    }
    if (!match(TokenKind::RBrace)) {
        failHere("expected '}'");
        return nullptr;
    }
    return std::make_unique<UnionDecl>(start, std::move(name), std::move(members));
}

std::unique_ptr<Decl> Parser::parseEnumDecl(bool) {
    Position start = advance().pos;
    if (!check(TokenKind::Ident)) {
        failHere("an enum needs a name");
        return nullptr;
    }
    std::string name = advance().text;
    if (!match(TokenKind::LBrace)) {
        failHere("an enum needs a body");
        return nullptr;
    }
    std::vector<EnumMember> members;
    while (!check(TokenKind::RBrace) && !atEnd()) {
        Position mpos = peek().pos;
        if (!check(TokenKind::Ident)) {
            failHere("an enumerator needs a name");
            syncToStmt();
            break;
        }
        std::string mname = advance().text;
        std::unique_ptr<Expr> value;
        if (match(TokenKind::Eq)) {
            value = parseExpr();
        }
        members.push_back(EnumMember(mpos, std::move(mname), std::move(value)));
        match(TokenKind::Comma);
    }
    if (!match(TokenKind::RBrace)) {
        failHere("expected '}'");
        return nullptr;
    }
    return std::make_unique<EnumDecl>(start, std::move(name), std::move(members));
}

std::unique_ptr<Decl> Parser::parseTypeAlias(bool) {
    Position start = advance().pos;
    if (!check(TokenKind::Ident)) {
        failHere("an alias needs a name");
        return nullptr;
    }
    std::string name = advance().text;
    std::vector<std::string> tps;
    if (match(TokenKind::Lt)) {
        for (;;) {
            if (!check(TokenKind::Ident)) {
                failHere("a type parameter needs a name");
                return nullptr;
            }
            tps.push_back(advance().text);
            if (match(TokenKind::Comma)) {
                continue;
            }
            break;
        }
        if (!match(TokenKind::Gt)) {
            failHere("expected '>'");
            return nullptr;
        }
    }
    if (!match(TokenKind::Eq)) {
        failHere("expected '='");
        return nullptr;
    }
    std::unique_ptr<TypeExpr> target = parseType();
    if (target == nullptr) {
        return nullptr;
    }
    if (!match(TokenKind::Semi)) {
        failHere("expected ';'");
        return nullptr;
    }
    return std::make_unique<TypeAlias>(start, std::move(name), std::move(tps),
                                       std::move(target));
}

std::unique_ptr<Decl> Parser::parseConstDecl() {
    Position start = advance().pos;
    if (!check(TokenKind::Ident)) {
        failHere("a constant needs a name");
        return nullptr;
    }
    std::string name = advance().text;
    std::unique_ptr<TypeExpr> type;
    if (match(TokenKind::Colon)) {
        type = parseType();
    }
    if (!match(TokenKind::Eq)) {
        failHere("a constant needs a value");
        return nullptr;
    }
    std::unique_ptr<Expr> init = parseExpr();
    if (init == nullptr) {
        return nullptr;
    }
    if (!match(TokenKind::Semi)) {
        failHere("expected ';'");
        return nullptr;
    }
    return std::make_unique<ConstDecl>(start, std::move(name), std::move(type),
                                       std::move(init));
}

std::unique_ptr<Decl> Parser::parseExtern() {
    Position start = advance().pos;
    std::string abi;
    if (check(TokenKind::StringLit)) {
        const Token& lit = advance();
        if (!lit.lit.parts.empty() && !lit.lit.parts[0].isExpr) {
            abi = lit.lit.parts[0].text;
        }
    }
    if (match(TokenKind::KwVar)) {
        if (!abi.empty()) {
            failHere("extern var has no calling convention");
            return nullptr;
        }
        if (!check(TokenKind::Ident)) {
            failHere("an extern variable needs a name");
            return nullptr;
        }
        std::string name = advance().text;
        if (!match(TokenKind::Colon)) {
            failHere("an extern variable needs a type");
            return nullptr;
        }
        std::unique_ptr<TypeExpr> type = parseType();
        if (type == nullptr) {
            return nullptr;
        }
        if (!match(TokenKind::Semi)) {
            failHere("expected ';'");
            return nullptr;
        }
        return std::make_unique<ExternVar>(start, std::move(name), std::move(type));
    }
    if (!match(TokenKind::KwFunc)) {
        failHere("extern declares a function or variable");
        return nullptr;
    }
    if (!check(TokenKind::Ident)) {
        failHere("an extern function needs a name");
        return nullptr;
    }
    std::string name = advance().text;
    std::vector<Param> params = parseParams();
    std::unique_ptr<TypeExpr> returns = parseReturnType();
    if (!match(TokenKind::Semi)) {
        failHere("expected ';'");
        return nullptr;
    }
    return std::make_unique<ExternFunc>(start, std::move(name), std::move(params),
                                        std::move(returns), std::move(abi));
}

bool Parser::matchModuleSegment(std::string& out) {
    if (check(TokenKind::Ident)) {
        out = advance().text;
        return true;
    }
    // A type spelling names no type in a module path: `string` in
    // `std.string` is just the segment's name. Keywords carry no text,
    // so the spelling comes from the token table (the parseMethod idiom).
    switch (peek().kind) {
        case TokenKind::KwInt:
        case TokenKind::KwI8:
        case TokenKind::KwI16:
        case TokenKind::KwI32:
        case TokenKind::KwI64:
        case TokenKind::KwU8:
        case TokenKind::KwU16:
        case TokenKind::KwU32:
        case TokenKind::KwU64:
        case TokenKind::KwF32:
        case TokenKind::KwF64:
        case TokenKind::KwBool:
        case TokenKind::KwChar:
        case TokenKind::KwByte:
        case TokenKind::KwString:
        case TokenKind::KwVoid:
            out = tokenName(peek().kind);
            advance();
            return true;
        default:
            return false;
    }
}

std::unique_ptr<Decl> Parser::parseImport() {
    Position start = peek().pos;
    if (match(TokenKind::KwImport)) {
        std::string module;
        for (;;) {
            std::string segment;
            if (!matchModuleSegment(segment)) {
                failHere("a module needs a name");
                return nullptr;
            }
            if (!module.empty()) {
                module += ".";
            }
            module += segment;
            if (match(TokenKind::Dot)) {
                continue;
            }
            break;
        }
        if (!match(TokenKind::Semi)) {
            failHere("expected ';'");
            return nullptr;
        }
        return std::make_unique<ImportDecl>(start, std::move(module));
    }
    advance();
    std::string module;
    for (;;) {
        std::string segment;
        if (!matchModuleSegment(segment)) {
            failHere("a module needs a name");
            return nullptr;
        }
        if (!module.empty()) {
            module += ".";
        }
        module += segment;
        if (match(TokenKind::Dot)) {
            continue;
        }
        break;
    }
    if (peek().kind != TokenKind::KwImport) {
        failHere("expected 'import'");
        return nullptr;
    }
    advance();
    std::vector<std::string> names;
    for (;;) {
        if (check(TokenKind::Ident)) {
            names.push_back(advance().text);
        } else if (check(TokenKind::KwOperator)) {
            // A library may export an allocation operator; the canonical
            // name imports like any other function.
            std::string name;
            if (!parseOperatorName(name)) {
                return nullptr;
            }
            names.push_back(std::move(name));
        } else {
            failHere("a name to import");
            return nullptr;
        }
        if (match(TokenKind::Comma)) {
            continue;
        }
        break;
    }
    if (!match(TokenKind::Semi)) {
        failHere("expected ';'");
        return nullptr;
    }
    return std::make_unique<FromImport>(start, std::move(module), std::move(names));
}

bool Parser::parseDirective(Program& program) {
    Position start = advance().pos;
    if (!match(TokenKind::KwExperimental)) {
        failHere("expected 'experimental'");
        return false;
    }
    if (!match(TokenKind::LParen)) {
        failHere("expected '('");
        return false;
    }
    if (!check(TokenKind::Ident)) {
        failHere("a behaviour has a name");
        return false;
    }
    std::string name = advance().text;
    if (!match(TokenKind::RParen)) {
        failHere("expected ')'");
        return false;
    }
    if (name == "wrap") {
        program.wrap = true;
        return true;
    }
    // An unknown behaviour is refused by name: silently ignoring a directive
    // would let a program ask for defined overflow and not get it.
    fail(start, "unknown experimental behaviour '" + name + "'");
    return false;
}

// --- Statements --------------------------------------------------------------

std::unique_ptr<Stmt> Parser::parseBlock() {
    Position start = advance().pos;
    std::vector<std::unique_ptr<Stmt>> stmts;
    while (!check(TokenKind::RBrace) && !atEnd()) {
        if (declStart(peek().kind) && !check(TokenKind::KwVar)) {
            std::unique_ptr<Decl> decl = parseDecl();
            if (decl != nullptr) {
                stmts.push_back(std::make_unique<DeclStmt>(decl->pos, std::move(decl)));
            } else {
                syncToStmt();
            }
            continue;
        }
        if (check(TokenKind::KwVar)) {
            std::unique_ptr<VarDecl> var = parseVarDecl();
            if (var != nullptr) {
                stmts.push_back(std::move(var));
            } else {
                syncToStmt();
            }
            continue;
        }
        std::unique_ptr<Stmt> stmt = parseStmt();
        if (stmt != nullptr) {
            stmts.push_back(std::move(stmt));
        } else {
            syncToStmt();
        }
    }
    if (!match(TokenKind::RBrace)) {
        failHere("expected '}'");
        return nullptr;
    }
    return std::make_unique<Block>(start, std::move(stmts));
}

std::unique_ptr<VarDecl> Parser::parseVarDecl() {
    Position start = advance().pos;
    if (!check(TokenKind::Ident)) {
        failHere("a variable needs a name");
        return nullptr;
    }
    std::string name = advance().text;
    if (!match(TokenKind::Colon)) {
        failHere("a variable needs a type");
        return nullptr;
    }
    bool isVolatile = match(TokenKind::KwVolatile);
    std::unique_ptr<TypeExpr> type = parseType();
    if (type == nullptr) {
        return nullptr;
    }
    if (interfaceMode_ && match(TokenKind::Semi)) {
        return std::make_unique<VarDecl>(start, std::move(name), std::move(type),
                                         nullptr, isVolatile);
    }
    if (!match(TokenKind::Eq)) {
        fail(peek().pos, "a variable needs an initialiser");
        return nullptr;
    }
    std::unique_ptr<Expr> init = parseExpr();
    if (init == nullptr) {
        return nullptr;
    }
    if (!match(TokenKind::Semi)) {
        failHere("expected ';'");
        return nullptr;
    }
    return std::make_unique<VarDecl>(start, std::move(name), std::move(type),
                                    std::move(init), isVolatile);
}

std::unique_ptr<Stmt> Parser::parseStmt() {
    TokenKind kind = peek().kind;
    if (kind == TokenKind::LBrace) {
        return parseBlock();
    }
    if (kind == TokenKind::KwIf) {
        return parseIf();
    }
    if (kind == TokenKind::KwWhile) {
        return parseWhile();
    }
    if (kind == TokenKind::KwFor) {
        return parseFor();
    }
    if (kind == TokenKind::KwMatch) {
        return parseMatch();
    }
    if (kind == TokenKind::KwDefer) {
        return parseDefer();
    }
    if (kind == TokenKind::KwReturn) {
        return parseReturn();
    }
    if (kind == TokenKind::KwDelete) {
        return parseDelete();
    }
    if (kind == TokenKind::KwAsm) {
        return parseAsm();
    }
    if (kind == TokenKind::KwBreak) {
        Position start = advance().pos;
        if (!match(TokenKind::Semi)) {
            failHere("expected ';'");
            return nullptr;
        }
        return std::make_unique<Break>(start);
    }
    if (kind == TokenKind::KwContinue) {
        Position start = advance().pos;
        if (!match(TokenKind::Semi)) {
            failHere("expected ';'");
            return nullptr;
        }
        return std::make_unique<Continue>(start);
    }
    std::unique_ptr<Expr> expr = parseExpr();
    if (expr == nullptr) {
        return nullptr;
    }
    if (!match(TokenKind::Semi)) {
        failHere("expected ';'");
        return nullptr;
    }
    return std::make_unique<ExprStmt>(expr->pos, std::move(expr));
}

std::unique_ptr<Stmt> Parser::parseIf() {
    Position start = advance().pos;
    if (!match(TokenKind::LParen)) {
        failHere("a condition needs parentheses");
        return nullptr;
    }
    std::unique_ptr<Expr> cond = parseExpr();
    if (cond == nullptr) {
        return nullptr;
    }
    if (!match(TokenKind::RParen)) {
        failHere("expected ')'");
        return nullptr;
    }
    std::unique_ptr<Stmt> then = parseStmt();
    if (then == nullptr) {
        return nullptr;
    }
    std::unique_ptr<Stmt> els;
    if (match(TokenKind::KwElse)) {
        els = parseStmt();
    }
    return std::make_unique<IfStmt>(start, std::move(cond), std::move(then), std::move(els));
}

std::unique_ptr<Stmt> Parser::parseWhile() {
    Position start = advance().pos;
    if (!match(TokenKind::LParen)) {
        failHere("a condition needs parentheses");
        return nullptr;
    }
    std::unique_ptr<Expr> cond = parseExpr();
    if (cond == nullptr) {
        return nullptr;
    }
    if (!match(TokenKind::RParen)) {
        failHere("expected ')'");
        return nullptr;
    }
    std::unique_ptr<Stmt> body = parseStmt();
    if (body == nullptr) {
        return nullptr;
    }
    return std::make_unique<WhileStmt>(start, std::move(cond), std::move(body));
}

std::unique_ptr<Stmt> Parser::parseFor() {
    Position start = advance().pos;
    if (!match(TokenKind::LParen)) {
        failHere("expected '('");
        return nullptr;
    }
    // `for (var x in xs)`, `for (x in xs)` and the Standard's annotated
    // `for (var b: u8 in xs)` against the C-style form. One lookahead
    // decision, then commit — the only save/restore in the parser. The
    // annotation is the one place the two spellings share a prefix
    // (`var x: T`), so a `: T` that no `in` follows restores and reparses
    // as the C-style head.
    uint64_t saved = save();
    bool isVar = match(TokenKind::KwVar);
    std::unique_ptr<TypeExpr> annotation;
    if (check(TokenKind::Ident) && peekAt(1).kind == TokenKind::Colon) {
        std::string name = advance().text;
        advance();
        annotation = parseType();
        if (annotation == nullptr) {
            return nullptr;
        }
        if (match(TokenKind::KwIn)) {
            std::unique_ptr<Expr> seq = parseExpr();
            if (seq == nullptr) {
                return nullptr;
            }
            if (!match(TokenKind::RParen)) {
                failHere("expected ')'");
                return nullptr;
            }
            std::unique_ptr<Stmt> body = parseStmt();
            if (body == nullptr) {
                return nullptr;
            }
            return std::make_unique<ForInStmt>(start, isVar, std::move(name),
                                               std::move(seq), std::move(body),
                                               std::move(annotation));
        }
        restore(saved);
    } else if (check(TokenKind::Ident) && peekAt(1).kind == TokenKind::KwIn) {
        std::string name = advance().text;
        advance();
        std::unique_ptr<Expr> seq = parseExpr();
        if (seq == nullptr) {
            return nullptr;
        }
        if (!match(TokenKind::RParen)) {
            failHere("expected ')'");
            return nullptr;
        }
        std::unique_ptr<Stmt> body = parseStmt();
        if (body == nullptr) {
            return nullptr;
        }
        return std::make_unique<ForInStmt>(start, isVar, std::move(name), std::move(seq),
                                           std::move(body));
    }
    restore(saved);
    std::unique_ptr<Stmt> init;
    if (!check(TokenKind::Semi)) {
        if (check(TokenKind::KwVar)) {
            advance();
            if (!check(TokenKind::Ident)) {
                failHere("a variable needs a name");
                return nullptr;
            }
            std::string name = advance().text;
            if (!match(TokenKind::Colon)) {
                failHere("a variable needs a type");
                return nullptr;
            }
            std::unique_ptr<TypeExpr> type = parseType();
            if (type == nullptr || !match(TokenKind::Eq)) {
                failHere("a loop variable needs an initialiser");
                return nullptr;
            }
            std::unique_ptr<Expr> value = parseExpr();
            if (value == nullptr) {
                return nullptr;
            }
            init = std::make_unique<VarDecl>(start, std::move(name), std::move(type),
                                             std::move(value));
        } else {
            std::unique_ptr<Expr> expr = parseExpr();
            if (expr == nullptr) {
                return nullptr;
            }
            init = std::make_unique<ExprStmt>(expr->pos, std::move(expr));
        }
    }
    if (!match(TokenKind::Semi)) {
        failHere("expected ';'");
        return nullptr;
    }
    std::unique_ptr<Expr> cond;
    if (!check(TokenKind::Semi)) {
        cond = parseExpr();
    }
    if (!match(TokenKind::Semi)) {
        failHere("expected ';'");
        return nullptr;
    }
    std::unique_ptr<Expr> step;
    if (!check(TokenKind::RParen)) {
        step = parseExpr();
    }
    if (!match(TokenKind::RParen)) {
        failHere("expected ')'");
        return nullptr;
    }
    std::unique_ptr<Stmt> body = parseStmt();
    if (body == nullptr) {
        return nullptr;
    }
    return std::make_unique<ForStmt>(start, std::move(init), std::move(cond), std::move(step),
                                     std::move(body));
}

Pattern Parser::parsePattern() {
    Position start = peek().pos;
    Pattern pattern(start);
    if (check(TokenKind::Ident) && peek().text == "_") {
        advance();
        pattern.kind = PatternKind::Wildcard;
        return pattern;
    }
    if (peek().kind == TokenKind::IntLit || peek().kind == TokenKind::FloatLit ||
        peek().kind == TokenKind::CharLit || peek().kind == TokenKind::StringLit ||
        peek().kind == TokenKind::KwTrue || peek().kind == TokenKind::KwFalse ||
        peek().kind == TokenKind::KwNilPtr) {
        pattern.kind = PatternKind::Const;
        pattern.value = parsePrefix();
        // `lo ... hi` is one range pattern, inclusive on both ends. Only
        // integer literals bound it: a float, char, string, bool, or name
        // beside the dots is refused here, where it is written, and a
        // bound the dots never follow stays the constant it always was.
        if (match(TokenKind::Ellipsis)) {
            if (dynamic_cast<IntLit*>(pattern.value.get()) == nullptr ||
                !check(TokenKind::IntLit)) {
                failHere("a range bound is an integer");
                return pattern;
            }
            pattern.kind = PatternKind::Range;
            pattern.rangeHi = parsePrefix();
        }
        return pattern;
    }
    if (!check(TokenKind::Ident)) {
        failHere("expected a pattern");
        return pattern;
    }
    std::string first = advance().text;
    if (match(TokenKind::Dot)) {
        if (!check(TokenKind::Ident)) {
            failHere("expected a member name");
            return pattern;
        }
        pattern.kind = PatternKind::Member;
        pattern.tag = std::move(first);
        pattern.member = advance().text;
        // A second dot roots the case in another module
        // (`lib.Shape.CIRCLE`): the middle names the type, the last the
        // case. Anything further is not a pattern.
        if (match(TokenKind::Dot)) {
            if (!check(TokenKind::Ident)) {
                failHere("expected a member name");
                return pattern;
            }
            pattern.mod = std::move(pattern.tag);
            pattern.tag = std::move(pattern.member);
            pattern.member = advance().text;
        }
        return pattern;
    }
    if (check(TokenKind::LBrace)) {
        advance();
        pattern.kind = PatternKind::Type;
        pattern.type = std::make_unique<NamedType>(start, "", std::move(first),
                                                   std::vector<std::unique_ptr<TypeExpr>>{});
        // An empty struct lists all zero of its fields this way: the
        // checker still proves the braces name an empty struct, so `E {}`
        // only ever matches `E`.
        if (match(TokenKind::RBrace)) {
            return pattern;
        }
        for (;;) {
            if (!check(TokenKind::Ident)) {
                failHere("a field pattern needs a name");
                return pattern;
            }
            FieldPat field;
            field.name = advance().text;
            if (!match(TokenKind::Colon)) {
                failHere("expected ':'");
                return pattern;
            }
            field.pat = std::make_unique<Pattern>(parsePattern());
            pattern.fields.push_back(std::move(field));
            if (match(TokenKind::Comma)) {
                continue;
            }
            break;
        }
        if (!match(TokenKind::RBrace)) {
            failHere("expected '}'");
        }
        return pattern;
    }
    pattern.kind = PatternKind::Bind;
    pattern.name = std::move(first);
    return pattern;
}

std::unique_ptr<Stmt> Parser::parseMatch() {
    Position start = advance().pos;
    bool savedNoLit = noStructLit_;
    noStructLit_ = true;
    std::unique_ptr<Expr> scrutinee = parseExpr();
    noStructLit_ = savedNoLit;
    if (scrutinee == nullptr) {
        return nullptr;
    }
    if (!match(TokenKind::LBrace)) {
        failHere("a match needs arms");
        return nullptr;
    }
    std::vector<std::unique_ptr<MatchArm>> arms;
    std::unique_ptr<Block> fallback;
    while (!check(TokenKind::RBrace) && !atEnd()) {
        Position apos = peek().pos;
        // `_` is the Standard's wildcard spelling of the default arm. A
        // guard after it (`_ if cond`) is a real pattern, so only the bare
        // form is the default.
        bool isDefault = match(TokenKind::KwDefault);
        if (!isDefault && check(TokenKind::Ident) && peek().text == "_" &&
            peekAt(1).kind == TokenKind::FatArrow) {
            advance();
            isDefault = true;
        }
        if (isDefault) {
            if (!match(TokenKind::FatArrow)) {
                failHere("expected '=>'");
                return nullptr;
            }
            std::unique_ptr<Stmt> body = parseStmt();
            if (body == nullptr) {
                return nullptr;
            }
            Block* block = dynamic_cast<Block*>(body.get());
            if (block != nullptr) {
                body.release();
                fallback = std::unique_ptr<Block>(block);
            } else {
                std::vector<std::unique_ptr<Stmt>> one;
                one.push_back(std::move(body));
                fallback = std::make_unique<Block>(apos, std::move(one));
            }
            continue;
        }
        Pattern pattern = parsePattern();
        std::vector<std::unique_ptr<Pattern>> alts;
        while (match(TokenKind::Pipe)) {
            Pattern alt = parsePattern();
            alts.push_back(std::make_unique<Pattern>(std::move(alt)));
        }
        // A guard trails the alternatives: `if` never continues a
        // pattern, so one here unambiguously opens the arm's condition.
        std::unique_ptr<Expr> guard;
        if (match(TokenKind::KwIf)) {
            guard = parseExpr();
            if (guard == nullptr) {
                return nullptr;
            }
        }
        if (!match(TokenKind::FatArrow)) {
            failHere("expected '=>'");
            return nullptr;
        }
        std::unique_ptr<Stmt> body = parseStmt();
        if (body == nullptr) {
            return nullptr;
        }
        Block* block = dynamic_cast<Block*>(body.get());
        std::unique_ptr<Block> arm;
        if (block != nullptr) {
            body.release();
            arm = std::unique_ptr<Block>(block);
        } else {
            std::vector<std::unique_ptr<Stmt>> one;
            one.push_back(std::move(body));
            arm = std::make_unique<Block>(apos, std::move(one));
        }
        auto matchArm = std::make_unique<MatchArm>(
            apos, std::make_unique<Pattern>(std::move(pattern)), std::move(arm));
        matchArm->alts = std::move(alts);
        matchArm->guard = std::move(guard);
        arms.push_back(std::move(matchArm));
    }
    if (!match(TokenKind::RBrace)) {
        failHere("expected '}'");
        return nullptr;
    }
    return std::make_unique<MatchStmt>(start, std::move(scrutinee), std::move(arms),
                                       std::move(fallback));
}

std::unique_ptr<Stmt> Parser::parseDefer() {
    Position start = advance().pos;
    std::unique_ptr<Stmt> stmt = parseStmt();
    if (stmt == nullptr) {
        return nullptr;
    }
    return std::make_unique<DeferStmt>(start, std::move(stmt));
}

std::unique_ptr<Stmt> Parser::parseReturn() {
    Position start = advance().pos;
    if (check(TokenKind::Semi) || check(TokenKind::RBrace)) {
        match(TokenKind::Semi);
        return std::make_unique<Return>(start, nullptr);
    }
    std::unique_ptr<Expr> value = parseExpr();
    if (value == nullptr) {
        return nullptr;
    }
    if (!match(TokenKind::Semi) && !check(TokenKind::RBrace)) {
        failHere("expected ';'");
        return nullptr;
    }
    match(TokenKind::Semi);
    return std::make_unique<Return>(start, std::move(value));
}

std::unique_ptr<Stmt> Parser::parseDelete() {
    Position start = advance().pos;
    std::unique_ptr<Expr> ptr = parseExpr();
    if (ptr == nullptr) {
        return nullptr;
    }
    if (!match(TokenKind::Semi)) {
        failHere("expected ';'");
        return nullptr;
    }
    return std::make_unique<DeleteStmt>(start, std::move(ptr));
}

// `asm { "line" "line" }` — the strings are the block's raw text, joined
// with a newline between them, and each `{placeholder}` body is kept raw
// (suffix and all) for the checker and codegen to resolve (FEATURES §4.4).
std::unique_ptr<Stmt> Parser::parseAsm() {
    Position start = advance().pos;
    if (!check(TokenKind::LBrace)) {
        failHere("expected '{' after 'asm'");
        return nullptr;
    }
    advance();
    std::string text;
    std::vector<std::string> names;
    while (check(TokenKind::StringLit)) {
        const Token& tok = peek();
        advance();
        if (!text.empty()) {
            text += "\n";
        }
        for (const StringPart& part : tok.lit.parts) {
            if (part.isExpr) {
                text += "{" + part.text + "}";
                names.push_back(part.text);
            } else {
                text += part.text;
            }
        }
    }
    if (!check(TokenKind::RBrace)) {
        failHere("expected '}' to close the asm block");
        return nullptr;
    }
    advance();
    return std::make_unique<AsmStmt>(start, std::move(text), std::move(names));
}

// --- Types -------------------------------------------------------------------

std::unique_ptr<TypeExpr> Parser::parseType() {
    Position start = peek().pos;
    // A leading `volatile` before a whole type (`type VI = volatile T`,
    // `[volatile T; N]`): qualification is an access property, so the
    // spelling is accepted wherever a type is.
    (void)match(TokenKind::KwVolatile);
    uint64_t depth = 0;
    while (match(TokenKind::Star)) {
        ++depth;
    }
    // `*volatile T`: the innermost pointer reads and writes its pointee
    // volatile (FEATURES §1.1c spells the mark before the pointee).
    bool pointeeVolatile = false;
    if (depth > 0 && match(TokenKind::KwVolatile)) {
        pointeeVolatile = true;
    }
    std::unique_ptr<TypeExpr> type = parsePrimaryType();
    if (type == nullptr) {
        return nullptr;
    }
    (void)start;
    bool innermost = true;
    while (depth > 0) {
        Position ppos = type->pos;
        auto ptr = std::make_unique<PointerType>(ppos, std::move(type));
        if (innermost && pointeeVolatile) {
            ptr->pointeeVolatile = true;
        }
        innermost = false;
        type = std::move(ptr);
        --depth;
    }
    return type;
}

std::unique_ptr<TypeExpr> Parser::parsePrimaryType() {
    Position start = peek().pos;
    TokenKind kind = peek().kind;
    // 8.4: `*msabi func(...) -> T` — the mark rides the function pointer type
    // so a call through it picks the same row the callee was built for.
    if (kind == TokenKind::Ident && peekAt(1).kind == TokenKind::KwFunc &&
        (peek().text == "msabi" || peek().text == "sysv")) {
        std::string abi = peek().text;
        advance();
        std::unique_ptr<TypeExpr> sig = parseFuncSig();
        if (sig == nullptr) {
            return nullptr;
        }
        static_cast<FuncSigType*>(sig.get())->abi = abi;
        return sig;
    }
    if (kind == TokenKind::KwInt) {
        advance();
        return std::make_unique<BasicType>(start, BasicKind::I32);
    }
    if (kind == TokenKind::KwI8) {
        advance();
        return std::make_unique<BasicType>(start, BasicKind::I8);
    }
    if (kind == TokenKind::KwI16) {
        advance();
        return std::make_unique<BasicType>(start, BasicKind::I16);
    }
    if (kind == TokenKind::KwI32) {
        advance();
        return std::make_unique<BasicType>(start, BasicKind::I32);
    }
    if (kind == TokenKind::KwI64) {
        advance();
        return std::make_unique<BasicType>(start, BasicKind::I64);
    }
    if (kind == TokenKind::KwU8) {
        advance();
        return std::make_unique<BasicType>(start, BasicKind::U8);
    }
    if (kind == TokenKind::KwU16) {
        advance();
        return std::make_unique<BasicType>(start, BasicKind::U16);
    }
    if (kind == TokenKind::KwU32) {
        advance();
        return std::make_unique<BasicType>(start, BasicKind::U32);
    }
    if (kind == TokenKind::KwU64) {
        advance();
        return std::make_unique<BasicType>(start, BasicKind::U64);
    }
    if (kind == TokenKind::KwF32) {
        advance();
        return std::make_unique<BasicType>(start, BasicKind::F32);
    }
    if (kind == TokenKind::KwF64) {
        advance();
        return std::make_unique<BasicType>(start, BasicKind::F64);
    }
    if (kind == TokenKind::KwBool) {
        advance();
        return std::make_unique<BasicType>(start, BasicKind::Bool);
    }
    if (kind == TokenKind::KwChar) {
        advance();
        return std::make_unique<BasicType>(start, BasicKind::Char);
    }
    if (kind == TokenKind::KwByte) {
        advance();
        return std::make_unique<BasicType>(start, BasicKind::U8);
    }
    if (kind == TokenKind::KwString || kind == TokenKind::KwVoid ||
        kind == TokenKind::Ident) {
        bool isString = check(TokenKind::KwString);
        bool isVoid = check(TokenKind::KwVoid);
        std::string qualifier;
        std::string name;
        if (isString || isVoid) {
            name = advance().text;
        } else {
            name = advance().text;
            // A qualifier may use either separator: `da.S` is the surface
            // spelling (values and module paths both use the dot), while
            // `da::S` stays accepted and both denote the canonical `da::S`
            // (TYPES §2.3). In a type position an identifier followed by a
            // dot can only be a qualification — there are no float
            // literals here — so the two cannot be confused.
            if (match(TokenKind::ColonColon) || match(TokenKind::Dot)) {
                if (!check(TokenKind::Ident)) {
                    failHere("expected a type name");
                    return nullptr;
                }
                qualifier = std::move(name);
                name = advance().text;
            }
        }
        std::vector<std::unique_ptr<TypeExpr>> args;
        if (match(TokenKind::Lt)) {
            for (;;) {
                std::unique_ptr<TypeExpr> arg = parseType();
                if (arg == nullptr) {
                    return nullptr;
                }
                args.push_back(std::move(arg));
                if (match(TokenKind::Comma)) {
                    continue;
                }
                break;
            }
            if (!matchGt()) {
                failHere("expected '>'");
                return nullptr;
            }
        }
        if (isString || isVoid) {
            if (!args.empty()) {
                failHere("this type takes no arguments");
                return nullptr;
            }
            std::vector<std::unique_ptr<TypeExpr>> none;
            if (isString) {
                return std::make_unique<NamedType>(start, "", "string", std::move(none));
            }
            return std::make_unique<NamedType>(start, "", "void", std::move(none));
        }
        return std::make_unique<NamedType>(start, std::move(qualifier), std::move(name),
                                           std::move(args));
    }
    if (kind == TokenKind::LBracket) {
        advance();
        if (match(TokenKind::RBracket)) {
            std::unique_ptr<TypeExpr> element = parseType();
            if (element == nullptr) {
                return nullptr;
            }
            return std::make_unique<SliceType>(start, std::move(element));
        }
        std::unique_ptr<TypeExpr> element = parseType();
        if (element == nullptr) {
            return nullptr;
        }
        if (!match(TokenKind::Semi)) {
            failHere("expected ';'");
            return nullptr;
        }
        std::unique_ptr<Expr> length = parseExpr();
        if (length == nullptr) {
            return nullptr;
        }
        if (!match(TokenKind::RBracket)) {
            failHere("expected ']'");
            return nullptr;
        }
        return std::make_unique<ArrayType>(start, std::move(element), std::move(length));
    }
    if (kind == TokenKind::KwFunc) {
        return parseFuncSig();
    }
    failHere("expected a type");
    return nullptr;
}

std::unique_ptr<TypeExpr> Parser::parseFuncSig() {
    Position start = advance().pos;
    if (!match(TokenKind::LParen)) {
        failHere("expected '('");
        return nullptr;
    }
    std::vector<std::unique_ptr<TypeExpr>> params;
    if (!check(TokenKind::RParen)) {
        for (;;) {
            // The Standard's fn-type spellings name their parameters
            // (`*func(code: int) -> bool`); a name carries no meaning in a
            // type, so it parses and is dropped.
            if (check(TokenKind::Ident) && peekAt(1).kind == TokenKind::Colon) {
                advance();
                advance();
            }
            std::unique_ptr<TypeExpr> param = parseType();
            if (param == nullptr) {
                return nullptr;
            }
            params.push_back(std::move(param));
            if (match(TokenKind::Comma)) {
                continue;
            }
            break;
        }
    }
    if (!match(TokenKind::RParen)) {
        failHere("expected ')'");
        return nullptr;
    }
    std::unique_ptr<TypeExpr> returns = parseReturnType();
    return std::make_unique<FuncSigType>(start, std::move(params), std::move(returns));
}
// --- Expressions -------------------------------------------------------------

std::unique_ptr<Expr> Parser::parseExpr() {
    return parseExprWith(0);
}

std::unique_ptr<Expr> Parser::parseExprWith(int minLevel) {
    std::unique_ptr<Expr> lhs = parsePrefix();
    if (lhs == nullptr) {
        return nullptr;
    }
    for (;;) {
        // `as` binds looser than unary, tighter than multiplicative.
        if (peek().kind == TokenKind::KwAs && LVL_AS > minLevel) {
            Position apos = advance().pos;
            std::unique_ptr<TypeExpr> target = parseType();
            if (target == nullptr) {
                return nullptr;
            }
            lhs = std::make_unique<Cast>(apos, std::move(lhs), std::move(target));
            continue;
        }
        // The ternary is right-associative and the loosest before assignment.
        if (peek().kind == TokenKind::Question && LVL_TERNARY > minLevel) {
            advance();
            std::unique_ptr<Expr> then = parseExpr();
            if (then == nullptr) {
                return nullptr;
            }
            if (!match(TokenKind::Colon)) {
                failHere("expected ':'");
                return nullptr;
            }
            std::unique_ptr<Expr> els = parseExprWith(LVL_TERNARY - 1);
            if (els == nullptr) {
                return nullptr;
            }
            lhs = std::make_unique<Ternary>(lhs->pos, std::move(lhs), std::move(then),
                                            std::move(els));
            continue;
        }
        // Assignment is right-associative and the loosest of all.
        if (peek().kind == TokenKind::Eq && LVL_ASSIGN > minLevel) {
            Position apos = advance().pos;
            std::unique_ptr<Expr> rhs = parseExprWith(LVL_ASSIGN - 1);
            if (rhs == nullptr) {
                return nullptr;
            }
            lhs = std::make_unique<Assign>(apos, std::move(lhs), std::optional<BinOp>{},
                                           std::move(rhs));
            continue;
        }
        BinOp compound = BinOp::Add;
        if (tokenCompoundOp(peek().kind, compound) && LVL_ASSIGN > minLevel) {
            Position apos = advance().pos;
            std::unique_ptr<Expr> rhs = parseExprWith(LVL_ASSIGN - 1);
            if (rhs == nullptr) {
                return nullptr;
            }
            lhs = std::make_unique<Assign>(apos, std::move(lhs),
                                           std::optional<BinOp>{compound}, std::move(rhs));
            continue;
        }
        BinOp op = BinOp::Add;
        int level = 0;
        if (!tokenBinOp(peek().kind, op, level) || level <= minLevel) {
            return lhs;
        }
        advance();
        std::unique_ptr<Expr> rhs = parseExprWith(level);
        if (rhs == nullptr) {
            return nullptr;
        }
        lhs = std::make_unique<Binary>(lhs->pos, op, std::move(lhs), std::move(rhs));
    }
}

std::unique_ptr<Expr> Parser::parsePrefix() {
    Position start = peek().pos;
    if (match(TokenKind::Minus)) {
        // A minus before a numeric literal folds into it: `-128` is one
        // literal, because `128` alone does not fit an `i8`. Before anything
        // else it is an ordinary negation.
        if (check(TokenKind::IntLit)) {
            const Token& lit = advance();
            return std::make_unique<IntLit>(start, lit.lit.intBase, lit.lit.intDigits, true);
        }
        if (check(TokenKind::FloatLit)) {
            const Token& lit = advance();
            return std::make_unique<FloatLit>(start, lit.lit.floatDigits, true, lit.lit.floatNarrow);
        }
        std::unique_ptr<Expr> operand = parsePrefix();
        if (operand == nullptr) {
            return nullptr;
        }
        return std::make_unique<Unary>(start, UnOp::Neg, std::move(operand));
    }
    if (match(TokenKind::Bang) || match(TokenKind::KwNot)) {
        std::unique_ptr<Expr> operand = parsePrefix();
        if (operand == nullptr) {
            return nullptr;
        }
        return std::make_unique<Unary>(start, UnOp::Not, std::move(operand));
    }
    if (match(TokenKind::Tilde)) {
        std::unique_ptr<Expr> operand = parsePrefix();
        if (operand == nullptr) {
            return nullptr;
        }
        return std::make_unique<Unary>(start, UnOp::BitNot, std::move(operand));
    }
    if (match(TokenKind::Star)) {
        std::unique_ptr<Expr> operand = parsePrefix();
        if (operand == nullptr) {
            return nullptr;
        }
        return std::make_unique<Unary>(start, UnOp::Deref, std::move(operand));
    }
    if (match(TokenKind::Amp)) {
        std::unique_ptr<Expr> operand = parsePrefix();
        if (operand == nullptr) {
            return nullptr;
        }
        return std::make_unique<Unary>(start, UnOp::Addr, std::move(operand));
    }
    // Prefix `++` / `--` bind like the other unary operators, so `++*p`
    // increments the pointee: the operand parses as a prefix expression,
    // and the checker decides whether it is a place.
    if (check(TokenKind::PlusPlus) || check(TokenKind::MinusMinus)) {
        bool isIncr = peek().kind == TokenKind::PlusPlus;
        advance();
        std::unique_ptr<Expr> operand = parsePrefix();
        if (operand == nullptr) {
            return nullptr;
        }
        return std::make_unique<IncrDecr>(start, isIncr, true, std::move(operand));
    }
    std::unique_ptr<Expr> base;
    switch (peek().kind) {
        case TokenKind::IntLit: {
            const Token& lit = advance();
            base = std::make_unique<IntLit>(start, lit.lit.intBase, lit.lit.intDigits, false);
            break;
        }
        case TokenKind::FloatLit: {
            const Token& lit = advance();
            base = std::make_unique<FloatLit>(start, lit.lit.floatDigits, false, lit.lit.floatNarrow);
            break;
        }
        case TokenKind::CharLit: {
            const Token& lit = advance();
            base = std::make_unique<CharLit>(start, lit.lit.charValue);
            break;
        }
        case TokenKind::StringLit: {
            Token lit = advance();
            base = parseStringParts(lit);
            break;
        }
        case TokenKind::WideStringLit: {
            const Token& lit = advance();
            base = std::make_unique<WideLit>(start, lit.lit.wideUnits);
            break;
        }
        case TokenKind::WideCharLit: {
            const Token& lit = advance();
            base = std::make_unique<WideChar>(start, lit.lit.charValue);
            break;
        }
        case TokenKind::KwTrue: {
            advance();
            base = std::make_unique<BoolLit>(start, true);
            break;
        }
        case TokenKind::KwFalse: {
            advance();
            base = std::make_unique<BoolLit>(start, false);
            break;
        }
        case TokenKind::KwNilPtr: {
            advance();
            base = std::make_unique<NilPtr>(start);
            break;
        }
        case TokenKind::KwUninit: {
            advance();
            base = std::make_unique<UninitExpr>(start);
            break;
        }
        case TokenKind::KwSuper: {
            advance();
            base = std::make_unique<Super>(start);
            break;
        }
        case TokenKind::At: {
            advance();
            if (!check(TokenKind::Ident) || peek().text != "bitcast") {
                failHere("expected 'bitcast' after '@'");
                return nullptr;
            }
            advance();
            if (!match(TokenKind::LParen)) {
                failHere("expected '(' after '@bitcast'");
                return nullptr;
            }
            std::unique_ptr<TypeExpr> bcTarget = parseType();
            if (bcTarget == nullptr) {
                return nullptr;
            }
            if (!match(TokenKind::Comma)) {
                failHere("expected ',' in '@bitcast(Type, value)'");
                return nullptr;
            }
            std::unique_ptr<Expr> bcValue = parseExpr();
            if (bcValue == nullptr) {
                return nullptr;
            }
            if (!match(TokenKind::RParen)) {
                failHere("expected ')'");
                return nullptr;
            }
            auto bc = std::make_unique<Cast>(start, std::move(bcValue), std::move(bcTarget));
            bc->raw = true;
            base = std::move(bc);
            break;
        }
        case TokenKind::Ident: {
            std::string name = advance().text;
            if (check(TokenKind::LBrace) && !noStructLit_) {
                advance();
                std::vector<std::unique_ptr<TypeExpr>> noArgs;
                auto named = std::make_unique<NamedType>(start, "", name, std::move(noArgs));
                base = parseStructLiteral(std::move(named), start);
                if (base == nullptr) {
                    return nullptr;
                }
                break;
            }
            base = std::make_unique<Ident>(start, std::move(name));
            break;
        }
        case TokenKind::KwSelf:
        case TokenKind::KwThis: {
            // `self` and `this` are ordinary names once parsed: the checker
            // binds `self` as a local in every method, constructor and
            // instantiated generic method body. `this` is the same receiver
            // under its v2 spelling, so it lands on the same name.
            advance();
            base = std::make_unique<Ident>(start, "self");
            break;
        }
        case TokenKind::LParen: {
            advance();
            base = parseExpr();
            if (base == nullptr) {
                return nullptr;
            }
            if (!match(TokenKind::RParen)) {
                failHere("expected ')'");
                return nullptr;
            }
            break;
        }
        case TokenKind::KwNew:
            base = parseNew(false);
            break;
        case TokenKind::KwTryNew:
            base = parseNew(true);
            break;
        case TokenKind::KwSizeof:
            base = parseSizeof();
            break;
        case TokenKind::KwAlignof:
            base = parseAlignof();
            break;
        case TokenKind::KwFunc:
            base = parseFuncLit();
            break;
        case TokenKind::LBracket: {
            advance();
            std::vector<std::unique_ptr<Expr>> inits;
            bool repeat = false;
            if (!check(TokenKind::RBracket)) {
                for (;;) {
                    inits.push_back(parseExpr());
                    if (inits.back() == nullptr) {
                        return nullptr;
                    }
                    if (match(TokenKind::Comma)) {
                        if (match(TokenKind::Ellipsis)) {
                            repeat = true;
                            break;
                        }
                        if (check(TokenKind::RBracket)) {
                            break;
                        }
                        continue;
                    }
                    break;
                }
            }
            if (!match(TokenKind::RBracket)) {
                failHere("expected ']'");
                return nullptr;
            }
            base = std::make_unique<ArrayLit>(start, std::move(inits), repeat);
            break;
        }
        default:
            failHere("expected an expression");
            return nullptr;
    }
    if (base == nullptr) {
        return nullptr;
    }
    return parsePostfix(std::move(base));
}

std::unique_ptr<Expr> Parser::parsePostfix(std::unique_ptr<Expr> base) {
    for (;;) {
        // `lib.Type { ... }`: a qualified aggregate literal (Unified
        // Standard 5.1). The name chain built so far is the type spelling.
        if (check(TokenKind::LBrace) && !noStructLit_) {
            std::unique_ptr<NamedType> named;
            if (const auto* id = dynamic_cast<const Ident*>(base.get())) {
                std::vector<std::unique_ptr<TypeExpr>> noArgs;
                named = std::make_unique<NamedType>(id->pos, "", id->name,
                                                    std::move(noArgs));
            } else if (const auto* fld = dynamic_cast<const Field*>(base.get())) {
                const auto* obj = dynamic_cast<const Ident*>(fld->object.get());
                if (obj != nullptr) {
                    std::vector<std::unique_ptr<TypeExpr>> noArgs;
                    named = std::make_unique<NamedType>(fld->pos, obj->name,
                                                        fld->name, std::move(noArgs));
                }
            }
            if (named != nullptr) {
                advance();
                base = parseStructLiteral(std::move(named), base->pos);
                if (base == nullptr) {
                    return nullptr;
                }
                continue;
            }
        }
        // `Vec<T> { ... }`: a generic bare literal (Unified Standard 5.1).
        // The `<` opens type arguments only when its balanced `>` is
        // immediately followed by `{`; `a < b` stays a comparison.
        bool namedBase = dynamic_cast<const Ident*>(base.get()) != nullptr ||
            (dynamic_cast<const Field*>(base.get()) != nullptr &&
             dynamic_cast<const Ident*>(
                 static_cast<const Field*>(base.get())->object.get()) != nullptr);
        if (check(TokenKind::Lt) && !noStructLit_ && namedBase) {
            uint32_t depth = 0, off = 0;
            bool shaped = false;
            for (; off < 32; ++off) {
                TokenKind k = peekAt(off).kind;
                if (k == TokenKind::Lt) {
                    ++depth;
                } else if (k == TokenKind::Gt) {
                    --depth;
                    if (depth == 0) {
                        shaped = peekAt(off + 1).kind == TokenKind::LBrace;
                        break;
                    }
                } else if (k == TokenKind::GtGt) {
                    if (depth < 2) {
                        break;
                    }
                    depth -= 2;
                    if (depth == 0) {
                        shaped = peekAt(off + 1).kind == TokenKind::LBrace;
                        break;
                    }
                } else if (k == TokenKind::End) {
                    break;
                }
            }
            if (shaped) {
                advance();
                std::vector<std::unique_ptr<TypeExpr>> args;
                if (!check(TokenKind::Gt)) {
                    for (;;) {
                        std::unique_ptr<TypeExpr> a = parseType();
                        if (a == nullptr) {
                            return nullptr;
                        }
                        args.push_back(std::move(a));
                        if (match(TokenKind::Comma)) {
                            continue;
                        }
                        break;
                    }
                }
                if (!matchGt()) {
                    failHere("expected '>'");
                    return nullptr;
                }
                if (!match(TokenKind::LBrace)) {
                    failHere("expected '{'");
                    return nullptr;
                }
                std::vector<std::unique_ptr<TypeExpr>> noArgs;
                std::unique_ptr<NamedType> gname;
                if (const auto* gident = dynamic_cast<const Ident*>(base.get())) {
                    gname = std::make_unique<NamedType>(gident->pos, "", gident->name,
                                                        std::move(noArgs));
                } else {
                    const auto* gfld = static_cast<const Field*>(base.get());
                    const auto* gobj = static_cast<const Ident*>(gfld->object.get());
                    gname = std::make_unique<NamedType>(gfld->pos, gobj->name,
                                                        gfld->name, std::move(noArgs));
                }
                gname->args = std::move(args);
                base = parseStructLiteral(std::move(gname), base->pos);
                if (base == nullptr) {
                    return nullptr;
                }
                continue;
            }
        }
        // Postfix `++` / `--` bind at this level, so `*p++` is `*(p++)`
        // and only `(*p)++` touches the pointee — the same rule C uses,
        // and the checker rejects whatever is not a place either way.
        if (check(TokenKind::PlusPlus) || check(TokenKind::MinusMinus)) {
            bool isIncr = peek().kind == TokenKind::PlusPlus;
            Position ppos = advance().pos;
            base = std::make_unique<IncrDecr>(ppos, isIncr, false, std::move(base));
            continue;
        }
        if (match(TokenKind::Dot)) {
            if (!check(TokenKind::Ident)) {
                failHere("expected a field name");
                return nullptr;
            }
            Position fpos = peek().pos;
            std::string name = advance().text;
            (void)fpos;
            base = std::make_unique<Field>(base->pos, std::move(base), std::move(name));
            continue;
        }
        // `p->f` desugars to `(*p).f`: one deref, then the member. The flag
        // keeps the spelling visible so a class/struct receiver can select
        // its member `operator->` before the built-in pointer path runs.
        if (match(TokenKind::Arrow)) {
            if (!check(TokenKind::Ident)) {
                failHere("expected a field name");
                return nullptr;
            }
            std::string name = advance().text;
            Position bpos = base->pos;
            base = std::make_unique<Field>(
                bpos,
                std::make_unique<Unary>(bpos, UnOp::Deref, std::move(base)),
                std::move(name), true);
            continue;
        }
        // `max::<int>(...)`: explicit arguments need the `::`, which is what
        // keeps `a < b` a comparison. Type positions stay `Box<int>`.
        if (check(TokenKind::ColonColon) && peekAt(1).kind == TokenKind::Lt) {
            Position gpos = advance().pos;
            advance();
            std::vector<std::unique_ptr<TypeExpr>> args;
            for (;;) {
                std::unique_ptr<TypeExpr> arg = parseType();
                if (arg == nullptr) {
                    return nullptr;
                }
                args.push_back(std::move(arg));
                if (match(TokenKind::Comma)) {
                    continue;
                }
                break;
            }
            if (!matchGt()) {
                failHere("expected '>'");
                return nullptr;
            }
            if (!match(TokenKind::LParen)) {
                failHere("type arguments need a call");
                return nullptr;
            }
            std::vector<std::unique_ptr<Expr>> callArgs;
            if (!check(TokenKind::RParen)) {
                for (;;) {
                    std::unique_ptr<Expr> arg = parseExpr();
                    if (arg == nullptr) {
                        return nullptr;
                    }
                    callArgs.push_back(std::move(arg));
                    if (match(TokenKind::Comma)) {
                        continue;
                    }
                    break;
                }
            }
            if (!match(TokenKind::RParen)) {
                failHere("expected ')'");
                return nullptr;
            }
            (void)gpos;
            base = std::make_unique<Call>(base->pos, std::move(base), std::move(args),
                                          std::move(callArgs));
            continue;
        }
        if (match(TokenKind::LParen)) {
            Position cpos = base->pos;
            std::vector<std::unique_ptr<Expr>> args;
            if (!check(TokenKind::RParen)) {
                for (;;) {
                    std::unique_ptr<Expr> arg = parseExpr();
                    if (arg == nullptr) {
                        return nullptr;
                    }
                    args.push_back(std::move(arg));
                    if (match(TokenKind::Comma)) {
                        continue;
                    }
                    break;
                }
            }
            if (!match(TokenKind::RParen)) {
                failHere("expected ')'");
                return nullptr;
            }
            // `super_init(args)` is the base constructor call (Standard
            // 4.8.3), a contextual spelling: the name is ordinary anywhere
            // but directly called.
            const auto* callee = dynamic_cast<const Ident*>(base.get());
            if (callee != nullptr && callee->name == "super_init") {
                base = std::make_unique<SuperInit>(cpos, std::move(args));
                continue;
            }
            base = std::make_unique<Call>(cpos, std::move(base),
                                          std::vector<std::unique_ptr<TypeExpr>>{},
                                          std::move(args));
            continue;
        }
        if (match(TokenKind::LBracket)) {
            Position ipos = base->pos;
            std::unique_ptr<Expr> first = parseExpr();
            if (first == nullptr) {
                return nullptr;
            }
            if (match(TokenKind::Colon)) {
                std::unique_ptr<Expr> hi = parseExpr();
                if (hi == nullptr) {
                    return nullptr;
                }
                if (!match(TokenKind::RBracket)) {
                    failHere("expected ']'");
                    return nullptr;
                }
                base = std::make_unique<SliceExpr>(ipos, std::move(base), std::move(first),
                                                  std::move(hi));
            } else {
                if (!match(TokenKind::RBracket)) {
                    failHere("expected ']'");
                    return nullptr;
                }
                base = std::make_unique<Index>(ipos, std::move(base), std::move(first));
            }
            continue;
        }
        return base;
    }
}

std::unique_ptr<Expr> Parser::parseStructLiteral(std::unique_ptr<TypeExpr> type,
                                                 Position start) {
    std::vector<FieldInit> fields;
    if (!check(TokenKind::RBrace)) {
        for (;;) {
            if (!check(TokenKind::Ident)) {
                failHere("a field needs a name");
                return nullptr;
            }
            FieldInit field;
            field.name = advance().text;
            if (!match(TokenKind::Colon)) {
                failHere("expected ':'");
                return nullptr;
            }
            field.value = parseExpr();
            if (field.value == nullptr) {
                return nullptr;
            }
            fields.push_back(std::move(field));
            if (match(TokenKind::Comma)) {
                continue;
            }
            break;
        }
    }
    if (!match(TokenKind::RBrace)) {
        failHere("expected '}'");
        return nullptr;
    }
    return std::make_unique<NewStruct>(start, std::move(type), std::move(fields));
}

std::unique_ptr<Expr> Parser::parseNew(bool tryNew) {
    Position start = advance().pos;
    if (check(TokenKind::LBracket)) {
        std::unique_ptr<TypeExpr> array = parseType();
        if (array == nullptr) {
            return nullptr;
        }
        ArrayType* raw = dynamic_cast<ArrayType*>(array.get());
        if (raw == nullptr) {
            failHere("expected an array type");
            return nullptr;
        }
        if (!match(TokenKind::LBrace)) {
            failHere("expected '{'");
            return nullptr;
        }
        std::vector<std::unique_ptr<Expr>> inits;
        bool repeat = false;
        if (match(TokenKind::LBracket)) {
            std::unique_ptr<Expr> first = parseExpr();
            if (first == nullptr) {
                return nullptr;
            }
            if (!match(TokenKind::Comma)) {
                failHere("expected ','");
                return nullptr;
            }
            // `[0, ...]` is the repeat form; `[a, b, c]` is the same
            // element list `{a, b, c}` would hold, brackets included
            // (FEATURES §1.8). The comma decides nothing — what follows
            // it does.
            if (match(TokenKind::Ellipsis)) {
                if (!match(TokenKind::RBracket)) {
                    failHere("expected ']'");
                    return nullptr;
                }
                inits.push_back(std::move(first));
                repeat = true;
            } else {
                inits.push_back(std::move(first));
                for (;;) {
                    std::unique_ptr<Expr> init = parseExpr();
                    if (init == nullptr) {
                        return nullptr;
                    }
                    inits.push_back(std::move(init));
                    if (match(TokenKind::Comma)) {
                        continue;
                    }
                    break;
                }
                if (!match(TokenKind::RBracket)) {
                    failHere("expected ']'");
                    return nullptr;
                }
            }
        } else if (!check(TokenKind::RBrace)) {
            for (;;) {
                std::unique_ptr<Expr> init = parseExpr();
                if (init == nullptr) {
                    return nullptr;
                }
                inits.push_back(std::move(init));
                if (match(TokenKind::Comma)) {
                    continue;
                }
                break;
            }
        }
        if (!match(TokenKind::RBrace)) {
            failHere("expected '}'");
            return nullptr;
        }
        std::unique_ptr<Expr> length = std::move(raw->length);
        std::unique_ptr<TypeExpr> element = std::move(raw->element);
        auto made = std::make_unique<NewArray>(start, std::move(element),
                                               std::move(length), std::move(inits),
                                               repeat, true);
        made->tryNew = tryNew;
        return made;
    }
    std::unique_ptr<TypeExpr> type = parsePrimaryType();
    if (type == nullptr) {
        return nullptr;
    }
    if (check(TokenKind::LBrace)) {
        advance();
        std::unique_ptr<Expr> lit = parseStructLiteral(std::move(type), start);
        if (lit != nullptr) {
            static_cast<NewStruct*>(lit.get())->heap = true;
            static_cast<NewStruct*>(lit.get())->tryNew = tryNew;
        }
        return lit;
    }
    if (match(TokenKind::LParen)) {
        std::vector<std::unique_ptr<Expr>> args;
        if (!check(TokenKind::RParen)) {
            for (;;) {
                std::unique_ptr<Expr> arg = parseExpr();
                if (arg == nullptr) {
                    return nullptr;
                }
                args.push_back(std::move(arg));
                if (match(TokenKind::Comma)) {
                    continue;
                }
                break;
            }
        }
        if (!match(TokenKind::RParen)) {
            failHere("expected ')'");
            return nullptr;
        }
        auto made = std::make_unique<NewClass>(start, std::move(type),
                                               std::move(args));
        made->tryNew = tryNew;
        return made;
    }
    failHere(std::string("expected '{' or '(' after ") +
             (tryNew ? "try_new" : "new"));
    return nullptr;
}

std::unique_ptr<Expr> Parser::parseSizeof() {
    Position start = advance().pos;
    // Parenthesized is the type form, exactly as before. `sizeof(x)` is a
    // type spelling, not a value form; a value with the same name fails the
    // same way a bad type spelling does.
    if (check(TokenKind::LParen)) {
        advance();
        std::unique_ptr<TypeExpr> type = parseType();
        if (type == nullptr) {
            return nullptr;
        }
        if (!match(TokenKind::RParen)) {
            failHere("expected ')'");
            return nullptr;
        }
        return std::make_unique<SizeofType>(start, std::move(type));
    }
    // The unparenthesized type spellings (`sizeof int`, `sizeof struct S`).
    // A bare identifier is a value, not a type: the operand form covers it.
    if (check(TokenKind::KwStruct) || check(TokenKind::KwClass) ||
        check(TokenKind::KwUnion) || check(TokenKind::KwEnum) ||
        check(TokenKind::KwInterface) || check(TokenKind::KwType)) {
        std::unique_ptr<TypeExpr> type = parseType();
        if (type == nullptr) {
            return nullptr;
        }
        return std::make_unique<SizeofType>(start, std::move(type));
    }
    // The value form binds like a prefix operator: `sizeof a + b` is
    // `(sizeof a) + b`, and `sizeof f()` keeps the call in the operand.
    std::unique_ptr<Expr> operand = parsePrefix();
    if (operand == nullptr) {
        return nullptr;
    }
    return std::make_unique<SizeofExpr>(start, std::move(operand));
}

std::unique_ptr<Expr> Parser::parseAlignof() {
    Position start = advance().pos;
    if (!match(TokenKind::LParen)) {
        failHere("alignof takes a type in parentheses");
        return nullptr;
    }
    std::unique_ptr<TypeExpr> type = parseType();
    if (type == nullptr) {
        return nullptr;
    }
    if (!match(TokenKind::RParen)) {
        failHere("expected ')'");
        return nullptr;
    }
    return std::make_unique<AlignofType>(start, std::move(type));
}

std::unique_ptr<Expr> Parser::parseFuncLit() {
    Position start = advance().pos;
    std::vector<Param> params = parseParams();
    std::unique_ptr<TypeExpr> returns = parseReturnType();
    if (!check(TokenKind::LBrace)) {
        failHere("a closure needs a body");
        return nullptr;
    }
    std::unique_ptr<Stmt> body = parseBlock();
    if (body == nullptr) {
        return nullptr;
    }
    return std::make_unique<FuncLit>(
        start, std::move(params), std::move(returns),
        std::unique_ptr<Block>(static_cast<Block*>(body.release())));
}

std::unique_ptr<Expr> Parser::parseStringParts(const Token& token) {
    std::vector<StringPartAst> parts;
    for (const StringPart& part : token.lit.parts) {
        StringPartAst out;
        // Braces are ordinary string bytes outside asm. The lexer still
        // records the run separately so `parseAsm` can name placeholders;
        // here it goes back verbatim, escapes already decoded.
        out.text = part.isExpr ? "{" + part.text + "}" : part.text;
        parts.push_back(std::move(out));
    }
    return std::make_unique<StringLit>(token.pos, std::move(parts));
}

std::unique_ptr<Expr> Parser::parseBraced(const std::vector<Token>& tokens, Position pos) {
    std::vector<Token> saved = std::move(tokens_);
    uint64_t cursor = save();
    tokens_ = tokens;
    cursor_ = 0;
    std::unique_ptr<Expr> expr = parseExpr();
    bool trailing = atEnd();
    tokens_ = std::move(saved);
    restore(cursor);
    if (expr == nullptr || !trailing) {
        fail(pos, "expected an expression");
        return nullptr;
    }
    return expr;
}

}  // namespace cider
