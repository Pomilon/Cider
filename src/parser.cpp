#include "parser.h"

#include <map>
#include <stdexcept>

namespace cider {

namespace {

// True if a token can begin an expression (used to disambiguate a trailing
// '*' in a cast target type: `y as u64 * p` must keep `*` as multiply, while
// `msg as char*` (followed by ';'/')'/`,`) keeps it as a pointer).
static bool canStartExpression(TokenType t) {
    switch (t) {
    case TokenType::IDENTIFIER:
    case TokenType::NUMBER:
    case TokenType::STRING:
    case TokenType::CHAR_LITERAL:
    case TokenType::TRUE:
    case TokenType::FALSE:
    case TokenType::NIL:
    case TokenType::NIL_PTR:
    case TokenType::THIS:
    case TokenType::SUPER:
    case TokenType::NEW:
    case TokenType::LPAREN:
    case TokenType::LBRACKET:
    case TokenType::LBRACE:
    case TokenType::FUNCTION:
    case TokenType::FUNC:
    case TokenType::ASYNC:
    case TokenType::AWAIT:
    case TokenType::SIZEOF:
    case TokenType::INCREMENT:
    case TokenType::DECREMENT:
    case TokenType::MINUS:
    case TokenType::NOT:
    case TokenType::MULTIPLY:
    case TokenType::AMPERSAND:
    case TokenType::BIT_NOT:
    case TokenType::TILDE:
        return true;
    default:
        return false;
    }
}

// Parse a NUMBER token text (decimal, 0x hex, 0b binary, 0o octal, float
// with '.'/'e', all allowing '_' separators) into a NumberExpr.
std::unique_ptr<NumberExpr> makeNumberExpr(const std::string& text, int line, int col,
                                           std::string* errOut = nullptr) {
    std::string s;
    s.reserve(text.size());
    for (char c : text) {
        if (c != '_') s += c;
    }
    try {
        if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            if (s.size() == 2) throw std::invalid_argument("empty hex literal");
            uint64_t v = std::stoull(s.substr(2), nullptr, 16);
            return std::make_unique<NumberExpr>(v, line, col);
        }
        if (s.size() > 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
            if (s.size() == 2) throw std::invalid_argument("empty binary literal");
            uint64_t v = 0;
            for (size_t i = 2; i < s.size(); ++i) {
                if (s[i] != '0' && s[i] != '1') throw std::invalid_argument("bad binary digit");
                v = (v << 1) | (uint64_t)(s[i] - '0');
            }
            return std::make_unique<NumberExpr>(v, line, col);
        }
        if (s.size() > 2 && s[0] == '0' && (s[1] == 'o' || s[1] == 'O')) {
            if (s.size() == 2) throw std::invalid_argument("empty octal literal");
            uint64_t v = std::stoull(s.substr(2), nullptr, 8);
            return std::make_unique<NumberExpr>(v, line, col);
        }
        bool isFloat = (s.find('.') != std::string::npos ||
                        s.find('e') != std::string::npos ||
                        s.find('E') != std::string::npos);
        if (isFloat) {
            double v = std::stod(s);
            return std::make_unique<NumberExpr>(v, line, col);
        }
        uint64_t v = std::stoull(s, nullptr, 10);
        return std::make_unique<NumberExpr>(v, line, col);
    } catch (const std::exception& e) {
        if (errOut) *errOut = e.what();
        return nullptr;
    }
}

// Parse any integer literal token (decimal/hex/binary/octal, '_' allowed)
// for contexts requiring an int (array sizes, bit widths).
bool parseIntToken(const std::string& text, uint64_t& out) {
    std::string s;
    s.reserve(text.size());
    for (char c : text) {
        if (c != '_') s += c;
    }
    try {
        if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            out = std::stoull(s.substr(2), nullptr, 16);
            return true;
        }
        if (s.size() > 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
            uint64_t v = 0;
            for (size_t i = 2; i < s.size(); ++i) {
                if (s[i] != '0' && s[i] != '1') return false;
                v = (v << 1) | (uint64_t)(s[i] - '0');
            }
            out = v;
            return true;
        }
        if (s.size() > 2 && s[0] == '0' && (s[1] == 'o' || s[1] == 'O')) {
            out = std::stoull(s.substr(2), nullptr, 8);
            return true;
        }
        out = std::stoull(s, nullptr, 10);
        return true;
    } catch (...) {
        return false;
    }
}

std::map<TokenType, Parser::Precedence> precedences = {
    {TokenType::EQ, Parser::EQUALS},
    {TokenType::NE, Parser::EQUALS},
    {TokenType::LT, Parser::LESSGREATER},
    {TokenType::LE, Parser::LESSGREATER},
    {TokenType::GT, Parser::LESSGREATER},
    {TokenType::GE, Parser::LESSGREATER},
    {TokenType::LSHIFT, Parser::SHIFT},
    {TokenType::RSHIFT, Parser::SHIFT},
    {TokenType::AMPERSAND, Parser::BIT_AND},
    {TokenType::BIT_XOR, Parser::BIT_XOR},
    {TokenType::BIT_OR, Parser::BIT_OR},
    {TokenType::PLUS, Parser::SUM},
    {TokenType::MINUS, Parser::SUM},
    {TokenType::MULTIPLY, Parser::PRODUCT},
    {TokenType::DIVIDE, Parser::PRODUCT},
    {TokenType::MODULO, Parser::PRODUCT},
    {TokenType::CARET, Parser::EXPONENT},
    {TokenType::AS, Parser::PREFIX},
    {TokenType::INCREMENT, Parser::CALL},
    {TokenType::DECREMENT, Parser::CALL},
    {TokenType::DOT, Parser::MEMBER_ACCESS},
    {TokenType::ARROW, Parser::MEMBER_ACCESS},
    {TokenType::LBRACKET, Parser::MEMBER_ACCESS},
    {TokenType::LBRACE, Parser::CALL},
    {TokenType::QUESTION, Parser::TERNARY},
    {TokenType::AND, Parser::LOGICAL_AND},
    {TokenType::OR, Parser::LOGICAL_OR},
    {TokenType::LPAREN, Parser::CALL},
};

}  // namespace

Parser::Parser(Lexer& lexer, ErrorReporter& reporter) : lexer_(lexer), reporter_(reporter) {
    nextToken();
    nextToken();
    nextToken();
}

void Parser::nextToken() {
    currentToken_ = peekToken_;
    peekToken_ = peekToken2_;
    peekToken2_ = lexer_.getNextToken();
}

bool Parser::expect(TokenType type) {
    if (peekToken_.type == type) {
        nextToken();
        return true;
    }
    error("Expected " + Token::toString(type) + ", got " + peekToken_.debugString());
    return false;
}

bool Parser::match(TokenType type) {
    if (currentToken_.type == type) {
        nextToken();
        return true;
    }
    return false;
}

void Parser::error(const std::string& message) {
    reporter_.error(message, currentToken_.line, currentToken_.column);
}

Parser::Precedence Parser::getPrecedence(TokenType type) {
    auto it = precedences.find(type);
    if (it != precedences.end()) {
        return it->second;
    }
    return LOWEST;
}

std::unique_ptr<AstProgram> Parser::parseProgram() {
    auto program = std::make_unique<AstProgram>();

    if (currentToken_.type == TokenType::STRICT) {
        nextToken();
        if (currentToken_.type == TokenType::IDENTIFIER && currentToken_.value == "cider") {
        } else {
            error("Expected 'cider' after 'strict'");
        }
        if (peekToken_.type != TokenType::SEMICOLON) {
            error("Expected ';' after strict statement.");
        } else {
            nextToken();  // Move to ;
            nextToken();  // Consume ;
        }
        program->isStrict = true;
    }

    while (currentToken_.type != TokenType::END_OF_FILE) {
        auto stmt = parseStatement();
        if (stmt) {
            program->addStatement(std::move(stmt));
        } else if (reporter_.hasErrors()) {
            // Error recovery: skip to next sync token
            while (currentToken_.type != TokenType::END_OF_FILE &&
                   currentToken_.type != TokenType::SEMICOLON &&
                   currentToken_.type != TokenType::RBRACE &&
                   currentToken_.type != TokenType::FUNCTION &&
                   currentToken_.type != TokenType::FUNC &&
                   currentToken_.type != TokenType::VAR &&
                   currentToken_.type != TokenType::IF &&
                   currentToken_.type != TokenType::WHILE &&
                   currentToken_.type != TokenType::FOR &&
                   currentToken_.type != TokenType::RETURN &&
                   currentToken_.type != TokenType::CLASS &&
                   currentToken_.type != TokenType::STRUCT &&
                   currentToken_.type != TokenType::ENUM &&
                   currentToken_.type != TokenType::IMPORT &&
                   currentToken_.type != TokenType::FROM &&
                   currentToken_.type != TokenType::EXPORT &&
                   currentToken_.type != TokenType::INTERFACE) {
                nextToken();
            }
            if (currentToken_.type == TokenType::SEMICOLON) nextToken();
            if (currentToken_.type == TokenType::RBRACE) break;
        } else {
            nextToken();
        }
    }
    return program;
}

std::unique_ptr<Expression> Parser::parseExpression(bool allowStructLiteral) {
    return parseExpression(LOWEST, allowStructLiteral);
}

std::unique_ptr<Expression> Parser::parseExpression(Precedence precedence, bool allowStructLiteral) {
    auto leftExpr = parsePrefixExpression();
    if (!leftExpr) return nullptr;

    while (getPrecedence(currentToken_.type) > precedence) {
        if (currentToken_.type == TokenType::LBRACE && !allowStructLiteral) {
            break;
        }

        if (currentToken_.type == TokenType::LPAREN) {
            leftExpr = parseCallExpression(std::move(leftExpr));
        } else if (currentToken_.type == TokenType::INCREMENT ||
                   currentToken_.type == TokenType::DECREMENT) {
            int line = currentToken_.line;
            int col = currentToken_.column;
            std::string op = currentToken_.value;
            nextToken();
            leftExpr = std::make_unique<UnaryExpr>(op, std::move(leftExpr), line, col, true);
        } else if (currentToken_.type == TokenType::DOT) {
            leftExpr = parseMemberAccessExpression(std::move(leftExpr));
        } else if (currentToken_.type == TokenType::ARROW) {
            leftExpr = parseMemberAccessExpression(std::move(leftExpr), true);
        } else if (currentToken_.type == TokenType::LBRACKET) {
            leftExpr = parseIndexExpression(std::move(leftExpr));
        } else if (currentToken_.type == TokenType::LBRACE) {
            leftExpr = parseStructLiteralExpression(std::move(leftExpr));
        } else if (currentToken_.type == TokenType::AS) {
            leftExpr = parseCastExpression(std::move(leftExpr));
        } else if (currentToken_.type == TokenType::QUESTION) {
            leftExpr = parseTernaryExpression(std::move(leftExpr));
        } else {
            leftExpr = parseInfixExpression(std::move(leftExpr));
        }
        if (!leftExpr) return nullptr;
    }
    return leftExpr;
}

std::unique_ptr<Expression> Parser::parsePrimaryExpression() {
    int line = currentToken_.line;
    int col = currentToken_.column;

    switch (currentToken_.type) {
    case TokenType::IDENTIFIER: {
        std::string val = currentToken_.value;
        nextToken();

        // Check for template arguments: funcName<Type1,Type2>
        if (currentToken_.type == TokenType::LT &&
            isTypeToken(peekToken_.type) &&
            (peekToken2_.type == TokenType::GT || peekToken2_.type == TokenType::COMMA)) {
            nextToken();  // consume <
            val += "<";
            while (true) {
                if (!isTypeToken(currentToken_.type)) {
                    error("Expected type in template argument.");
                    return nullptr;
                }
                val += currentToken_.value;
                nextToken();
                if (currentToken_.type == TokenType::COMMA) {
                    val += ",";
                    nextToken();
                } else if (currentToken_.type == TokenType::GT) {
                    val += ">";
                    nextToken();  // consume >
                    break;
                } else {
                    error("Expected ',' or '>' in template arguments.");
                    return nullptr;
                }
            }
        }

        return std::make_unique<IdentifierExpr>(val, line, col);
    }
    case TokenType::THIS: {
        nextToken();
        return std::make_unique<ThisExpr>(line, col);
    }
    case TokenType::SUPER: {
        nextToken();  // Consume super
        if (currentToken_.type != TokenType::DOT) {
            error("Expected '.' after 'super'.");
            return nullptr;
        }
        nextToken();  // Consume .
        if (currentToken_.type != TokenType::IDENTIFIER) {
            error("Expected identifier after 'super.'.");
            return nullptr;
        }
        std::string member = currentToken_.value;
        nextToken();  // Consume member
        return std::make_unique<SuperExpr>(member, line, col);
    }
    case TokenType::NUMBER: {
        std::string raw = currentToken_.value;
        auto node = makeNumberExpr(raw, line, col);
        if (!node) {
            error("Invalid number literal: " + raw);
            return nullptr;
        }
        nextToken();
        return node;
    }
    case TokenType::STRING: {
        std::string val = currentToken_.value;
        int line = currentToken_.line;
        int col = currentToken_.column;
        nextToken();
        // String interpolation: "x = {y}" — scan for unescaped braces.
        // {{ and }} produce literal braces; {expr} is parsed by re-lexing
        // the segment text with a fresh parser.
        std::vector<InterpolatedStringExpr::Part> parts;
        bool interpolated = false;
        auto collapseEscaped = [](const std::string& raw) {
            std::string out;
            out.reserve(raw.size());
            for (size_t k = 0; k < raw.size(); ++k) {
                if (raw[k] == '{' && k + 1 < raw.size() && raw[k + 1] == '{') {
                    out += '{';
                    k++;
                } else if (raw[k] == '}' && k + 1 < raw.size() && raw[k + 1] == '}') {
                    out += '}';
                    k++;
                } else {
                    out += raw[k];
                }
            }
            return out;
        };
        size_t i = 0;
        size_t partStart = 0;
        while (i < val.size()) {
            if (val[i] == '{') {
                if (i + 1 < val.size() && val[i + 1] == '{') {
                    i += 2;
                    continue;  // literal { — kept in the raw text
                }
                // A brace followed by whitespace is a literal brace (CSS,
                // JSON, code snippets in strings) — never interpolation.
                if (i + 1 < val.size() &&
                    (val[i + 1] == ' ' || val[i + 1] == '\t' || val[i + 1] == '\n')) {
                    i++;
                    continue;
                }
                size_t exprStart = i + 1;
                size_t depth = 1;
                size_t j = exprStart;
                bool closed = false;
                while (j < val.size()) {
                    if (val[j] == '{') depth++;
                    else if (val[j] == '}') {
                        depth--;
                        if (depth == 0) { closed = true; break; }
                    }
                    j++;
                }
                if (!closed) {
                    error("Unclosed '{' in interpolated string.");
                    return nullptr;
                }
                std::string seg = val.substr(exprStart, j - exprStart);
                // Parse the segment as a full expression with a fresh parser.
                // If it fails, the whole {..} is literal text (backwards
                // compatible with pre-interpolation strings).
                ErrorReporter subRep;
                Lexer subLex(seg, subRep);
                Parser subParser(subLex, subRep);
                auto expr = subParser.parseExpression();
                if (!expr || subRep.hasErrors() ||
                    subLex.getNextToken().type != TokenType::END_OF_FILE) {
                    i = j + 1;  // keep the whole segment as literal text
                    continue;
                }
                if (i > partStart) {
                    parts.push_back({false, collapseEscaped(val.substr(partStart, i - partStart)), nullptr});
                }
                parts.push_back({true, "", std::move(expr)});
                interpolated = true;
                i = j + 1;
                partStart = i;
            } else if (val[i] == '}') {
                if (i + 1 < val.size() && val[i + 1] == '}') {
                    i += 2;
                    continue;  // literal } — collapsed in the raw text
                }
                i++;  // lone } is literal text (backwards compatible)
            } else {
                i++;
            }
        }
        if (!interpolated) {
            return std::make_unique<StringExpr>(val, line, col);
        }
        if (partStart < val.size()) parts.push_back({false, collapseEscaped(val.substr(partStart)), nullptr});
        return std::make_unique<InterpolatedStringExpr>(std::move(parts), line, col);
    }
    case TokenType::CHAR_LITERAL: {
        char val = currentToken_.value[0];
        nextToken();
        return std::make_unique<CharExpr>(val, line, col);
    }
    case TokenType::TRUE: {
        nextToken();
        return std::make_unique<BooleanExpr>(true, line, col);
    }
    case TokenType::FALSE: {
        nextToken();
        return std::make_unique<BooleanExpr>(false, line, col);
    }
    case TokenType::NIL: {
        nextToken();
        return std::make_unique<NilExpr>(line, col);
    }
    case TokenType::NIL_PTR: {
        nextToken();
        return std::make_unique<NilPtrExpr>(line, col);
    }
    case TokenType::NEW: {
        nextToken();  // Consume 'new'
        auto type = parseType();
        if (!type) return nullptr;
        return std::make_unique<NewExpr>(std::move(type), line, col);
    }
    case TokenType::LPAREN: {
        nextToken();  // Consume '('
        auto expr = parseExpression();
        if (!expr) return nullptr;
        if (currentToken_.type != TokenType::RPAREN) {
            error("Expected ')' after expression.");
            return nullptr;
        }
        nextToken();  // Consume ')'
        return std::make_unique<GroupingExpr>(std::move(expr), line, col);
    }
    case TokenType::LBRACKET: {
        return parseListLiteral();
    }
    case TokenType::LBRACE: {
        return parseTableLiteral();
    }
    case TokenType::ASYNC: {
        nextToken();
        if (currentToken_.type != TokenType::FUNCTION && currentToken_.type != TokenType::FUNC) {
            error("Expected 'func' after 'async'.");
            return nullptr;
        }
        return parseFunctionExpression(true);
    }
    case TokenType::AWAIT: {
        return parseAwaitExpression();
    }
    case TokenType::FUNCTION:
    case TokenType::FUNC: {
        return parseFunctionExpression();
    }
    case TokenType::SIZEOF: {
        return parseSizeofExpression();
    }
    default:
        error("Unexpected token in expression: " + currentToken_.debugString());
        return nullptr;
    }
}

std::unique_ptr<Expression> Parser::parseAwaitExpression() {
    int line = currentToken_.line;
    int col = currentToken_.column;
    nextToken();
    auto value = parseExpression(Precedence::PREFIX);
    if (!value) return nullptr;
    return std::make_unique<AwaitExpr>(std::move(value), line, col);
}

std::unique_ptr<Expression> Parser::parseCallExpression(std::unique_ptr<Expression> callee) {
    int line = currentToken_.line;
    int col = currentToken_.column;
    nextToken();  // Consume '('

    std::vector<std::unique_ptr<Expression>> args;
    if (currentToken_.type != TokenType::RPAREN) {
        while (true) {
            auto arg = parseExpression();
            if (!arg) return nullptr;
            args.push_back(std::move(arg));

            if (currentToken_.type == TokenType::COMMA) {
                nextToken();
            } else {
                break;
            }
        }
    }
    if (currentToken_.type != TokenType::RPAREN) {
        error("Expected ')' after call arguments.");
        return nullptr;
    }
    nextToken();  // Consume ')'
    return std::make_unique<CallExpr>(std::move(callee), std::move(args), line, col);
}

std::unique_ptr<Expression> Parser::parsePrefixExpression() {
    int line = currentToken_.line;
    int col = currentToken_.column;
    if (currentToken_.type == TokenType::INCREMENT || currentToken_.type == TokenType::DECREMENT ||
        currentToken_.type == TokenType::MINUS || currentToken_.type == TokenType::NOT ||
        currentToken_.type == TokenType::MULTIPLY || currentToken_.type == TokenType::AMPERSAND ||
        currentToken_.type == TokenType::BIT_NOT || currentToken_.type == TokenType::TILDE) {
        std::string op = currentToken_.value;
        nextToken();
        auto right = parseExpression(PREFIX);
        if (!right) return nullptr;
        return std::make_unique<UnaryExpr>(op, std::move(right), line, col);
    }
    return parsePrimaryExpression();
}

std::unique_ptr<Expression> Parser::parseInfixExpression(std::unique_ptr<Expression> left) {
    int line = currentToken_.line;
    int col = currentToken_.column;
    TokenType operatorType = currentToken_.type;
    std::string op = currentToken_.value;
    Precedence precedence = getPrecedence(operatorType);
    nextToken();

    auto right = parseExpression(precedence);
    if (!right) return nullptr;

    return std::make_unique<BinaryExpr>(std::move(left), op, std::move(right), line, col);
}

std::unique_ptr<Expression> Parser::parseMemberAccessExpression(std::unique_ptr<Expression> object, bool isArrow) {
    int line = currentToken_.line;
    int col = currentToken_.column;
    nextToken();  // Consume . or ->

    if (currentToken_.type != TokenType::IDENTIFIER) {
        error("Expected identifier after access operator.");
        return nullptr;
    }
    std::string memberName = currentToken_.value;
    nextToken();  // Consume member name
    return std::make_unique<MemberAccessExpr>(std::move(object), memberName, line, col, isArrow);
}

std::unique_ptr<Expression> Parser::parseIndexExpression(std::unique_ptr<Expression> object) {
    int line = currentToken_.line;
    int col = currentToken_.column;
    nextToken();  // Consume '['

    std::unique_ptr<Expression> start = nullptr;
    std::unique_ptr<Expression> end = nullptr;
    bool isSlice = false;

    if (currentToken_.type == TokenType::COLON) {
        isSlice = true;
        nextToken();  // Consume ':'
        if (currentToken_.type != TokenType::RBRACKET) {
            end = parseExpression();
        }
    } else {
        start = parseExpression();
        if (!start) return nullptr;

        if (currentToken_.type == TokenType::COLON) {
            isSlice = true;
            nextToken();  // Consume ':'

            if (currentToken_.type != TokenType::RBRACKET) {
                end = parseExpression();
                if (!end) return nullptr;
            }
        }
    }

    if (currentToken_.type != TokenType::RBRACKET) {
        error("Expected ']' after index.");
        return nullptr;
    }
    nextToken();  // Consume ']'

    if (isSlice) {
        return std::make_unique<SliceExpr>(std::move(object), std::move(start), std::move(end), line, col);
    }
    return std::make_unique<IndexExpr>(std::move(object), std::move(start), line, col);
}

std::unique_ptr<Expression> Parser::parseStructLiteralExpression(std::unique_ptr<Expression> object) {
    int line = currentToken_.line;
    int col = currentToken_.column;

    IdentifierExpr* id = dynamic_cast<IdentifierExpr*>(object.get());
    if (!id) {
        error("Struct literal must follow a struct name (identifier).");
        return nullptr;
    }
    std::string structName = id->getName();

    std::vector<StructLiteralExpr::FieldInit> fields;
    nextToken();  // Consume '{'

    if (currentToken_.type != TokenType::RBRACE) {
        while (true) {
            if (currentToken_.type != TokenType::IDENTIFIER) {
                error("Expected field name in struct literal.");
                return nullptr;
            }
            std::string fieldName = currentToken_.value;
            nextToken();  // Consume field name
            if (currentToken_.type != TokenType::COLON) {
                error("Expected ':' after field name in struct literal.");
                return nullptr;
            }
            nextToken();  // Consume ':'
            auto value = parseExpression();
            fields.push_back(std::make_pair(fieldName, std::move(value)));

            if (currentToken_.type == TokenType::COMMA) {
                nextToken();
            } else {
                break;
            }
        }
    }

    if (currentToken_.type != TokenType::RBRACE) {
        error("Expected '}' after struct literal.");
        return nullptr;
    }
    nextToken();  // Consume '}'

    return std::make_unique<StructLiteralExpr>(structName, std::move(fields), line, col);
}

std::unique_ptr<Expression> Parser::parseCastExpression(std::unique_ptr<Expression> expr) {
    int line = currentToken_.line;
    int col = currentToken_.column;
    nextToken();  // Consume 'as'
    auto targetType = parseType(true);
    return std::make_unique<CastExpr>(std::move(expr), std::move(targetType), line, col);
}

std::unique_ptr<Expression> Parser::parseSizeofExpression() {
    int line = currentToken_.line;
    int col = currentToken_.column;
    nextToken();  // Consume 'sizeof'
    if (currentToken_.type != TokenType::LPAREN) {
        error("Expected '(' after 'sizeof'.");
        return nullptr;
    }
    nextToken();  // Consume '('
    auto targetType = parseType();
    if (currentToken_.type != TokenType::RPAREN) {
        error("Expected ')' after sizeof type.");
        return nullptr;
    }
    nextToken();  // Consume ')'
    return std::make_unique<SizeofExpr>(std::move(targetType), line, col);
}

std::unique_ptr<Expression> Parser::parseTernaryExpression(std::unique_ptr<Expression> condition) {
    int line = currentToken_.line;
    int col = currentToken_.column;
    nextToken();  // Consume '?'
    auto thenExpr = parseExpression(LOWEST);
    if (!thenExpr) return nullptr;

    if (currentToken_.type != TokenType::COLON) {
        error("Expected ':' in ternary expression.");
        return nullptr;
    }
    nextToken();  // Consume ':'
    auto elseExpr = parseExpression(LOWEST);
    if (!elseExpr) return nullptr;

    return std::make_unique<TernaryExpr>(std::move(condition), std::move(thenExpr), std::move(elseExpr), line, col);
}

std::unique_ptr<Expression> Parser::parseFunctionExpression(bool isAsync) {
    int line = currentToken_.line;
    int col = currentToken_.column;
    nextToken();  // Consume function/func

    std::string funcName = "";
    if (currentToken_.type == TokenType::IDENTIFIER) {
        funcName = currentToken_.value;
        nextToken();
    }

    if (currentToken_.type != TokenType::LPAREN) {
        error("Expected '(' after function keyword.");
        return nullptr;
    }
    nextToken();

    std::vector<FunctionExpr::Parameter> params;
    if (currentToken_.type != TokenType::RPAREN) {
        while (true) {
            if (currentToken_.type != TokenType::IDENTIFIER) {
                error("Expected parameter name.");
                return nullptr;
            }
            std::string paramName = currentToken_.value;
            nextToken();
            std::unique_ptr<TypeAST> paramType;
            if (currentToken_.type == TokenType::COLON) {
                nextToken();
                paramType = parseType();
            }
            params.push_back({paramName, std::move(paramType)});

            if (currentToken_.type == TokenType::COMMA) {
                nextToken();
            } else {
                break;
            }
        }
    }

    if (currentToken_.type != TokenType::RPAREN) {
        error("Expected ')' after parameters.");
        return nullptr;
    }
    nextToken();

    std::unique_ptr<TypeAST> returnType;
    if (currentToken_.type == TokenType::ARROW) {
        nextToken();
        returnType = parseType();
    }

    if (currentToken_.type != TokenType::LBRACE) {
        error("Expected '{' for function body.");
        return nullptr;
    }
    nextToken();

    std::vector<std::unique_ptr<Statement>> body = parseBlockStatement();

    if (currentToken_.type != TokenType::RBRACE) {
        error("Expected '}' after function body.");
        return nullptr;
    }
    nextToken();

    return std::make_unique<FunctionExpr>(funcName, std::move(params), std::move(body),
                                          std::move(returnType), line, col, isAsync);
}

std::unique_ptr<Expression> Parser::parseListLiteral() {
    int line = currentToken_.line;
    int col = currentToken_.column;
    std::vector<std::unique_ptr<Expression>> elements;
    nextToken();  // Consume '['

    if (currentToken_.type != TokenType::RBRACKET) {
        while (true) {
            auto element = parseExpression();
            if (!element) return nullptr;
            elements.push_back(std::move(element));

            if (currentToken_.type == TokenType::COMMA) {
                nextToken();
            } else {
                break;
            }
        }
    }

    if (currentToken_.type != TokenType::RBRACKET) {
        error("Expected ']' after list.");
        return nullptr;
    }
    nextToken();

    return std::make_unique<ListExpr>(std::move(elements), line, col);
}

std::unique_ptr<Expression> Parser::parseTableLiteral() {
    int line = currentToken_.line;
    int col = currentToken_.column;
    std::vector<TableExpr::Entry> entries;
    nextToken();  // Consume '{'

    if (currentToken_.type != TokenType::RBRACE) {
        while (true) {
            std::unique_ptr<Expression> key;
            if (currentToken_.type == TokenType::IDENTIFIER || currentToken_.type == TokenType::STRING ||
                currentToken_.type == TokenType::NUMBER) {
                if (currentToken_.type == TokenType::STRING) {
                    key = std::make_unique<StringExpr>(currentToken_.value, currentToken_.line, currentToken_.column);
                } else if (currentToken_.type == TokenType::NUMBER) {
                    auto numNode = makeNumberExpr(currentToken_.value, currentToken_.line, currentToken_.column);
                    if (!numNode) {
                        error("Invalid number literal: " + currentToken_.value);
                        return nullptr;
                    }
                    key = std::move(numNode);
                } else {
                    key = std::make_unique<StringExpr>(currentToken_.value, currentToken_.line, currentToken_.column);
                }
                nextToken();
            } else {
                error("Expected table key.");
                return nullptr;
            }

            if (currentToken_.type != TokenType::COLON) {
                error("Expected ':' after table key.");
                return nullptr;
            }
            nextToken();  // Consume ':'

            auto value = parseExpression();
            if (!value) return nullptr;

            entries.push_back({std::move(key), std::move(value)});

            if (currentToken_.type == TokenType::COMMA) {
                nextToken();
            } else {
                break;
            }
        }
    }

    if (currentToken_.type != TokenType::RBRACE) {
        error("Expected '}' after table.");
        return nullptr;
    }
    nextToken();

    return std::make_unique<TableExpr>(std::move(entries), line, col);
}

std::unique_ptr<Statement> Parser::parseStatement() {
    switch (currentToken_.type) {
    case TokenType::VAR:
    case TokenType::CONST:
        return parseVarDeclarationStatement();
    case TokenType::TYPE:
        return parseTypeAliasStatement();
    case TokenType::IF:
        return parseIfStatement();
    case TokenType::WHILE:
        return parseWhileStatement();
    case TokenType::FOR:
        return parseForStatement();
    case TokenType::DELETE:
        return parseDeleteStatement();
    case TokenType::RETURN:
        return parseReturnStatement();
    case TokenType::BREAK:
        return parseBreakStatement();
    case TokenType::CONTINUE:
        return parseContinueStatement();
    case TokenType::THROW:
        return parseThrowStatement();
    case TokenType::TRY:
        return parseTryCatchStatement();
    case TokenType::DEFER:
        return parseDeferStatement();
    case TokenType::ASM:
        return parseAsmStatement();
    case TokenType::MATCH:
        return parseMatchStatement();
    case TokenType::STRUCT:
        return parseStructDeclaration();
    case TokenType::UNION:
        return parseStructDeclaration(true);
    case TokenType::ENUM:
        return parseEnumDeclaration();
    case TokenType::ASYNC: {
        nextToken();
        if (currentToken_.type == TokenType::FUNCTION || currentToken_.type == TokenType::FUNC) {
            return parseFunctionDeclaration(true);
        }
        error("Expected 'func' after 'async'.");
        return nullptr;
    }
    case TokenType::VIRTUAL: {
        nextToken();
        bool isAsync = false;
        if (currentToken_.type == TokenType::ASYNC) {
            isAsync = true;
            nextToken();
        }
        if (currentToken_.type == TokenType::FUNCTION || currentToken_.type == TokenType::FUNC) {
            return parseFunctionDeclaration(isAsync, true, false);
        }
        error("Expected 'func' after 'virtual'.");
        return nullptr;
    }
    case TokenType::OVERRIDE: {
        nextToken();
        bool isAsync = false;
        if (currentToken_.type == TokenType::ASYNC) {
            isAsync = true;
            nextToken();
        }
        if (currentToken_.type == TokenType::FUNCTION || currentToken_.type == TokenType::FUNC) {
            return parseFunctionDeclaration(isAsync, false, true);
        }
        error("Expected 'func' after 'override'.");
        return nullptr;
    }
    case TokenType::EXTERN: {
        nextToken();
        if (currentToken_.type == TokenType::FUNCTION || currentToken_.type == TokenType::FUNC) {
            return parseFunctionDeclaration(false, false, false, true);
        }
        error("Expected 'func' after 'extern'.");
        return nullptr;
    }
    case TokenType::FUNCTION:
    case TokenType::FUNC:
        return parseFunctionDeclaration();
    case TokenType::CLASS:
        return parseClassDeclaration();
    case TokenType::INTERFACE:
        return parseInterfaceDeclaration();
    case TokenType::IMPORT:
        return parseImportStatement();
    case TokenType::FROM:
        return parseFromImportStatement();
    case TokenType::EXPORT:
        return parseExportStatement();
    case TokenType::LBRACE: {
        int line = currentToken_.line;
        int col = currentToken_.column;
        nextToken();
        auto body = parseBlockStatement();
        if (currentToken_.type != TokenType::RBRACE) {
            error("Expected '}' after block statement.");
            return nullptr;
        }
        nextToken();
        return std::make_unique<BlockStmt>(std::move(body), line, col);
    }
    default:
        return parseExpressionStatement();
    }
}

std::unique_ptr<TypeAST> Parser::parseType(bool castContext) {
    int line = currentToken_.line;
    int col = currentToken_.column;
    bool isVolatile = false;

    if (currentToken_.type == TokenType::VOLATILE) {
        isVolatile = true;
        nextToken();
    }

    if (currentToken_.type == TokenType::LBRACKET) {
        nextToken();
        auto baseType = parseType(castContext);
        if (currentToken_.type != TokenType::SEMICOLON) {
            error("Expected ';' after base type in array declaration.");
            return nullptr;
        }
        nextToken();
        if (currentToken_.type != TokenType::NUMBER) {
            error("Expected array size.");
            return nullptr;
        }
        uint64_t sz64 = 0;
        if (!parseIntToken(currentToken_.value, sz64)) {
            error("Invalid array size: " + currentToken_.value);
            return nullptr;
        }
        int size = (int)sz64;
        nextToken();
        if (currentToken_.type != TokenType::RBRACKET) {
            error("Expected ']' after array size.");
            return nullptr;
        }
        nextToken();
        return std::make_unique<ArrayTypeAST>(std::move(baseType), size, line, col);
    }

    int pointerLevel = 0;
    while (currentToken_.type == TokenType::MULTIPLY) {
        pointerLevel++;
        nextToken();
    }

    // Function type: func(int) -> int — the parameters and return type are
    // parsed and discarded (the type carries only the func marker; the
    // closure's own literal carries the full signature).
    if (currentToken_.type == TokenType::FUNCTION || currentToken_.type == TokenType::FUNC) {
        std::string typeName = currentToken_.value;
        nextToken();
        if (currentToken_.type == TokenType::LPAREN) {
            int depth = 1;
            nextToken();
            while (depth > 0 && currentToken_.type != TokenType::END_OF_FILE) {
                if (currentToken_.type == TokenType::LPAREN) depth++;
                else if (currentToken_.type == TokenType::RPAREN) depth--;
                nextToken();
            }
        }
        if (currentToken_.type == TokenType::ARROW) {
            nextToken();
            // consume the return type's tokens
            int depth = 0;
            while (currentToken_.type != TokenType::END_OF_FILE) {
                if (currentToken_.type == TokenType::LBRACE || currentToken_.type == TokenType::SEMICOLON ||
                    currentToken_.type == TokenType::COMMA || currentToken_.type == TokenType::RPAREN ||
                    currentToken_.type == TokenType::RBRACE || currentToken_.type == TokenType::EQ ||
                    currentToken_.type == TokenType::ASSIGN) {
                    break;
                }
                if (currentToken_.type == TokenType::LT || currentToken_.type == TokenType::LPAREN) depth++;
                else if (currentToken_.type == TokenType::GT || currentToken_.type == TokenType::RPAREN) depth--;
                if (depth < 0) break;
                nextToken();
            }
        }
        return std::make_unique<TypeAST>(typeName, pointerLevel + 1, line, col, isVolatile);
    }

    if (currentToken_.type == TokenType::IDENTIFIER ||
        currentToken_.type == TokenType::INT ||
        currentToken_.type == TokenType::FLOAT ||
        currentToken_.type == TokenType::BOOL_TYPE ||
        currentToken_.type == TokenType::STRING_TYPE ||
        currentToken_.type == TokenType::VOID ||
        currentToken_.type == TokenType::U8 || currentToken_.type == TokenType::I8 ||
        currentToken_.type == TokenType::U16 || currentToken_.type == TokenType::I16 ||
        currentToken_.type == TokenType::U32 || currentToken_.type == TokenType::I32 ||
        currentToken_.type == TokenType::U64 || currentToken_.type == TokenType::I64 ||
        currentToken_.type == TokenType::CHAR_TYPE || currentToken_.type == TokenType::BYTE_TYPE) {
        std::string typeName = currentToken_.value;
        nextToken();

        // Check for template type arguments: Vec<int> or Vec<int, float>
        if (currentToken_.type == TokenType::LT) {
            nextToken();  // consume <
            typeName += "<";
            while (true) {
                if (!isTypeToken(currentToken_.type)) {
                    error("Expected type in template argument.");
                    return nullptr;
                }
                typeName += currentToken_.value;
                nextToken();
                if (currentToken_.type == TokenType::COMMA) {
                    typeName += ",";
                    nextToken();
                } else if (currentToken_.type == TokenType::GT) {
                    typeName += ">";
                    nextToken();  // consume >
                    break;
                } else {
                    error("Expected ',' or '>' in template arguments.");
                    return nullptr;
                }
            }
        }

        // Trailing '*'s: in declarations they are always pointers (greedy).
        // In cast targets (`y as u64 * p`) the last '*' may be a binary
        // multiply — leave it unconsumed when the token after it can start
        // an expression (the multiply RHS). `msg as char*` (star followed by
        // ';'/')'/`,`) stays a pointer.
        if (castContext) {
            while (currentToken_.type == TokenType::MULTIPLY) {
                if (peekToken_.type == TokenType::MULTIPLY) {
                    pointerLevel++;
                    nextToken();
                } else if (canStartExpression(peekToken_.type)) {
                    break;
                } else {
                    pointerLevel++;
                    nextToken();
                }
            }
        } else {
            while (currentToken_.type == TokenType::MULTIPLY) {
                pointerLevel++;
                nextToken();
            }
        }
        return std::make_unique<TypeAST>(typeName, pointerLevel, line, col, isVolatile);
    }
    error("Expected type name.");
    return nullptr;
}

bool Parser::isTypeToken(TokenType type) const {
    return type == TokenType::IDENTIFIER ||
           type == TokenType::INT || type == TokenType::FLOAT ||
           type == TokenType::BOOL_TYPE || type == TokenType::STRING_TYPE ||
           type == TokenType::VOID ||
           type == TokenType::U8 || type == TokenType::I8 ||
           type == TokenType::U16 || type == TokenType::I16 ||
           type == TokenType::U32 || type == TokenType::I32 ||
           type == TokenType::U64 || type == TokenType::I64 ||
           type == TokenType::CHAR_TYPE || type == TokenType::BYTE_TYPE;
}

std::vector<std::string> Parser::parseTemplateParams() {
    std::vector<std::string> params;
    if (currentToken_.type != TokenType::LT) return params;
    nextToken();  // consume <
    while (true) {
        // Type tokens (int, bool, user types as identifiers) are allowed:
        // explicit specializations write func max<int>(...).
        if (currentToken_.type != TokenType::IDENTIFIER && !isTypeToken(currentToken_.type)) {
            error("Expected template parameter name.");
            return params;
        }
        params.push_back(currentToken_.value);
        nextToken();
        if (currentToken_.type == TokenType::COMMA) {
            nextToken();
        } else if (currentToken_.type == TokenType::GT) {
            nextToken();  // consume >
            break;
        } else {
            error("Expected ',' or '>' in template parameter list.");
            return params;
        }
    }
    return params;
}

std::unique_ptr<Statement> Parser::parseVarDeclarationStatement() {
    int line = currentToken_.line;
    int col = currentToken_.column;
    bool isConst = (currentToken_.type == TokenType::CONST);
    nextToken();  // Consume 'var' or 'const'

    if (currentToken_.type != TokenType::IDENTIFIER) {
        error("Expected identifier after declaration keyword.");
        return nullptr;
    }
    std::string varName = currentToken_.value;
    nextToken();

    std::unique_ptr<TypeAST> type = nullptr;
    if (currentToken_.type == TokenType::COLON) {
        nextToken();
        type = parseType();
    } else {
        error("Expected ':' and type after variable name in Cider.");
        return nullptr;
    }

    std::unique_ptr<Expression> initializer = nullptr;
    if (currentToken_.type == TokenType::ASSIGN) {
        nextToken();  // Consume '='
        initializer = parseExpression();
    } else if (isConst) {
        error("Constants must be initialized.");
        return nullptr;
    }

    if (currentToken_.type != TokenType::SEMICOLON) {
        error("Expected ';' after variable declaration.");
        return nullptr;
    }
    nextToken();  // Consume ';'

    return std::make_unique<VarDeclStmt>(varName, std::move(initializer), std::move(type), line, col, isConst);
}

std::unique_ptr<Statement> Parser::parseAssignmentStatement(std::unique_ptr<Expression> target) {
    int line = target->getLine();
    int col = target->getColumn();

    TokenType opType = currentToken_.type;
    nextToken();  // Consume operator

    auto value = parseExpression(LOWEST);
    if (!value) return nullptr;
    if (currentToken_.type != TokenType::SEMICOLON) {
        error("Expected ';' after assignment.");
        return nullptr;
    }
    nextToken();  // Consume ';'

    if (opType == TokenType::ASSIGN) {
        return std::make_unique<AssignStmt>(std::move(target), std::move(value), line, col);
    }

    std::string binaryOp;
    switch (opType) {
    case TokenType::PLUS_ASSIGN: binaryOp = "+"; break;
    case TokenType::MINUS_ASSIGN: binaryOp = "-"; break;
    case TokenType::MULT_ASSIGN: binaryOp = "*"; break;
    case TokenType::DIV_ASSIGN: binaryOp = "/"; break;
    case TokenType::MOD_ASSIGN: binaryOp = "%"; break;
    case TokenType::AMPERSAND_ASSIGN: binaryOp = "&"; break;
    case TokenType::BIT_OR_ASSIGN: binaryOp = "|"; break;
    case TokenType::BIT_XOR_ASSIGN: binaryOp = "^"; break;
    default: break;
    }
    return std::make_unique<AssignStmt>(std::move(target), std::move(value), line, col, binaryOp);
}

std::unique_ptr<Statement> Parser::parseIfStatement() {
    int line = currentToken_.line;
    int col = currentToken_.column;
    nextToken();  // Consume 'if'

    if (currentToken_.type != TokenType::LPAREN) {
        error("Expected '(' after 'if'.");
        return nullptr;
    }
    nextToken();  // Consume '('

    auto condition = parseExpression(false);
    if (!condition) return nullptr;

    if (currentToken_.type != TokenType::RPAREN) {
        error("Expected ')' after condition.");
        return nullptr;
    }
    nextToken();  // Consume ')'

    std::vector<std::unique_ptr<Statement>> thenBranch;
    if (currentToken_.type == TokenType::LBRACE) {
        nextToken();
        thenBranch = parseBlockStatement();
        if (currentToken_.type != TokenType::RBRACE) {
            error("Expected '}' after 'then' block.");
            return nullptr;
        }
        nextToken();
    } else {
        auto stmt = parseStatement();
        if (!stmt) return nullptr;
        thenBranch.push_back(std::move(stmt));
    }

    std::vector<std::unique_ptr<Statement>> elseBranch;
    if (currentToken_.type == TokenType::ELSE) {
        nextToken();
        if (currentToken_.type == TokenType::IF) {
            auto nestedIf = parseIfStatement();
            if (!nestedIf) return nullptr;
            elseBranch.push_back(std::move(nestedIf));
        } else if (currentToken_.type == TokenType::LBRACE) {
            nextToken();
            elseBranch = parseBlockStatement();
            if (currentToken_.type != TokenType::RBRACE) {
                error("Expected '}' after 'else' block.");
                return nullptr;
            }
            nextToken();
        } else {
            auto stmt = parseStatement();
            if (!stmt) return nullptr;
            elseBranch.push_back(std::move(stmt));
        }
    }

    return std::make_unique<IfStmt>(std::move(condition), std::move(thenBranch), std::move(elseBranch), line, col);
}

std::unique_ptr<Statement> Parser::parseWhileStatement() {
    int line = currentToken_.line;
    int col = currentToken_.column;
    nextToken();  // Consume 'while'
    if (currentToken_.type != TokenType::LPAREN) {
        error("Expected '(' after 'while'.");
        return nullptr;
    }
    nextToken();  // Consume '('

    auto condition = parseExpression(false);
    if (!condition) return nullptr;
    if (currentToken_.type != TokenType::RPAREN) {
        error("Expected ')' after condition.");
        return nullptr;
    }
    nextToken();  // Consume ')'

    std::vector<std::unique_ptr<Statement>> body;
    if (currentToken_.type == TokenType::LBRACE) {
        nextToken();
        body = parseBlockStatement();
        if (currentToken_.type != TokenType::RBRACE) {
            error("Expected '}' after 'while' block.");
            return nullptr;
        }
        nextToken();
    } else {
        auto stmt = parseStatement();
        if (!stmt) return nullptr;
        body.push_back(std::move(stmt));
    }

    return std::make_unique<WhileStmt>(std::move(condition), std::move(body), line, col);
}

std::unique_ptr<Statement> Parser::parseForStatement() {
    int line = currentToken_.line;
    int col = currentToken_.column;
    nextToken();

    if (currentToken_.type != TokenType::LPAREN) {
        error("Expected '(' after 'for'.");
        return nullptr;
    }
    nextToken();

    if (currentToken_.type == TokenType::VAR) {
        nextToken();
        if (currentToken_.type != TokenType::IDENTIFIER) {
            error("Expected identifier after 'var'.");
            return nullptr;
        }
        std::string varName = currentToken_.value;
        nextToken();

        if (currentToken_.type == TokenType::IDENTIFIER && currentToken_.value == "in") {
            nextToken();
            auto iterable = parseExpression(false);
            if (!iterable) return nullptr;

            if (currentToken_.type != TokenType::RPAREN) {
                error("Expected ')' after iterable.");
                return nullptr;
            }
            nextToken();  // Consume ')'

            std::vector<std::unique_ptr<Statement>> body;
            if (currentToken_.type == TokenType::LBRACE) {
                nextToken();
                body = parseBlockStatement();
                if (currentToken_.type != TokenType::RBRACE) {
                    error("Expected '}' after for loop body.");
                    return nullptr;
                }
                nextToken();
            } else {
                auto stmt = parseStatement();
                if (!stmt) return nullptr;
                body.push_back(std::move(stmt));
            }

            return std::make_unique<ForEachStmt>(varName, std::move(iterable), std::move(body), line, col);
        } else {
            std::unique_ptr<TypeAST> type = nullptr;
            if (currentToken_.type == TokenType::COLON) {
                nextToken();
                type = parseType();
            }

            std::unique_ptr<Expression> initExpr = nullptr;
            if (currentToken_.type == TokenType::ASSIGN) {
                nextToken();
                initExpr = parseExpression();
            }
            if (currentToken_.type != TokenType::SEMICOLON) {
                error("Expected ';' after initializer.");
                return nullptr;
            }
            auto initializer = std::make_unique<VarDeclStmt>(varName, std::move(initExpr), std::move(type), line, col);
            nextToken();

            std::unique_ptr<Expression> condition = nullptr;
            if (currentToken_.type != TokenType::SEMICOLON) {
                condition = parseExpression(false);
            }
            if (currentToken_.type != TokenType::SEMICOLON) {
                error("Expected ';' after for condition.");
                return nullptr;
            }
            nextToken();

            std::unique_ptr<Statement> increment = nullptr;
            if (currentToken_.type != TokenType::RPAREN) {
                auto expr = parseExpression(false);
                if (currentToken_.type == TokenType::ASSIGN ||
                    currentToken_.type == TokenType::PLUS_ASSIGN || currentToken_.type == TokenType::MINUS_ASSIGN ||
                    currentToken_.type == TokenType::MULT_ASSIGN || currentToken_.type == TokenType::DIV_ASSIGN ||
                    currentToken_.type == TokenType::MOD_ASSIGN) {
                    TokenType opType = currentToken_.type;
                    nextToken();
                    auto value = parseExpression(LOWEST);
                    std::string binaryOp = "";
                    switch (opType) {
                    case TokenType::PLUS_ASSIGN: binaryOp = "+"; break;
                    case TokenType::MINUS_ASSIGN: binaryOp = "-"; break;
                    case TokenType::MULT_ASSIGN: binaryOp = "*"; break;
                    case TokenType::DIV_ASSIGN: binaryOp = "/"; break;
                    case TokenType::MOD_ASSIGN: binaryOp = "%"; break;
                    default: break;
                    }
                    increment = std::make_unique<AssignStmt>(std::move(expr), std::move(value), line, col, binaryOp);
                } else {
                    increment = std::make_unique<ExpressionStmt>(std::move(expr), line, col);
                }
            }

            if (currentToken_.type != TokenType::RPAREN) {
                error("Expected ')' after for header.");
                return nullptr;
            }
            nextToken();

            std::vector<std::unique_ptr<Statement>> body;
            if (currentToken_.type == TokenType::LBRACE) {
                nextToken();
                body = parseBlockStatement();
                if (currentToken_.type != TokenType::RBRACE) {
                    error("Expected '}' after block.");
                    return nullptr;
                }
                nextToken();
            } else {
                auto stmt = parseStatement();
                if (!stmt) return nullptr;
                body.push_back(std::move(stmt));
            }

            return std::make_unique<ForStmt>(std::move(initializer), std::move(condition), std::move(increment), std::move(body), line, col);
        }
    }

    std::unique_ptr<Statement> initializer = nullptr;
    if (currentToken_.type == TokenType::SEMICOLON) {
        nextToken();
    } else {
        initializer = parseExpressionStatement();
    }

    std::unique_ptr<Expression> condition = nullptr;
    if (currentToken_.type != TokenType::SEMICOLON) {
        condition = parseExpression(false);
    }
    if (currentToken_.type != TokenType::SEMICOLON) {
        error("Expected ';' after for condition.");
        return nullptr;
    }
    nextToken();

    std::unique_ptr<Statement> increment = nullptr;
    if (currentToken_.type != TokenType::RPAREN) {
        auto expr = parseExpression(false);
        if (currentToken_.type == TokenType::ASSIGN) {
            nextToken();
            auto value = parseExpression(LOWEST);
            increment = std::make_unique<AssignStmt>(std::move(expr), std::move(value), line, col);
        } else {
            increment = std::make_unique<ExpressionStmt>(std::move(expr), line, col);
        }
    }

    if (currentToken_.type != TokenType::RPAREN) {
        error("Expected ')' after for header.");
        return nullptr;
    }
    nextToken();

    std::vector<std::unique_ptr<Statement>> body;
    if (currentToken_.type == TokenType::LBRACE) {
        nextToken();
        body = parseBlockStatement();
        if (currentToken_.type != TokenType::RBRACE) {
            error("Expected '}' after block.");
            return nullptr;
        }
        nextToken();
    } else {
        auto stmt = parseStatement();
        if (!stmt) return nullptr;
        body.push_back(std::move(stmt));
    }

    return std::make_unique<ForStmt>(std::move(initializer), std::move(condition), std::move(increment), std::move(body), line, col);
}

std::unique_ptr<Statement> Parser::parseDeleteStatement() {
    int line = currentToken_.line;
    int col = currentToken_.column;
    nextToken();  // Consume 'delete'

    auto target = parseExpression();

    if (currentToken_.type == TokenType::SEMICOLON) {
        nextToken();
    }

    return std::make_unique<DeleteStmt>(std::move(target), line, col);
}

std::unique_ptr<Statement> Parser::parseReturnStatement() {
    int line = currentToken_.line;
    int col = currentToken_.column;
    nextToken();  // Consume 'return'

    std::unique_ptr<Expression> returnValue = nullptr;
    if (currentToken_.type == TokenType::SEMICOLON) {
        nextToken();
    } else {
        returnValue = parseExpression();
        if (!returnValue) return nullptr;
        if (currentToken_.type != TokenType::SEMICOLON) {
            error("Expected ';' after return value.");
            return nullptr;
        }
        nextToken();  // Consume ';'
    }
    return std::make_unique<ReturnStmt>(std::move(returnValue), line, col);
}

std::unique_ptr<Statement> Parser::parseExpressionStatement() {
    auto expr = parseExpression();
    if (!expr) return nullptr;

    if (currentToken_.type == TokenType::ASSIGN ||
        currentToken_.type == TokenType::PLUS_ASSIGN || currentToken_.type == TokenType::MINUS_ASSIGN ||
        currentToken_.type == TokenType::MULT_ASSIGN || currentToken_.type == TokenType::DIV_ASSIGN ||
        currentToken_.type == TokenType::MOD_ASSIGN || currentToken_.type == TokenType::AMPERSAND_ASSIGN ||
        currentToken_.type == TokenType::BIT_OR_ASSIGN || currentToken_.type == TokenType::BIT_XOR_ASSIGN) {
        return parseAssignmentStatement(std::move(expr));
    } else {
        if (currentToken_.type != TokenType::RBRACE && currentToken_.type != TokenType::END_OF_FILE) {
            if (currentToken_.type != TokenType::SEMICOLON) {
                error("Expected ';' after expression statement.");
                return nullptr;
            }
            nextToken();  // Consume ';'
        }
        return std::make_unique<ExpressionStmt>(std::move(expr), expr->getLine(), expr->getColumn());
    }
}

std::unique_ptr<Statement> Parser::parseFunctionDeclaration(bool isAsync, bool isVirtual, bool isOverride, bool isExtern) {
    int line = currentToken_.line;
    int col = currentToken_.column;
    bool isDestructor = false;
    std::string funcName;
    nextToken();  // Consume function/func

    if (currentToken_.type == TokenType::TILDE) {
        isDestructor = true;
        nextToken();  // Consume ~
        if (currentToken_.type != TokenType::IDENTIFIER) {
            error("Expected class name after '~' in destructor.");
            return nullptr;
        }
        funcName = "~" + currentToken_.value;
        nextToken();
    } else {
        if (currentToken_.type != TokenType::IDENTIFIER) {
            error("Expected function name.");
            return nullptr;
        }
        funcName = currentToken_.value;
        nextToken();
    }

    std::vector<std::string> templateParams;
    if (currentToken_.type == TokenType::LT) {
        templateParams = parseTemplateParams();
    }

    if (currentToken_.type != TokenType::LPAREN) {
        error("Expected '(' after function name.");
        return nullptr;
    }
    nextToken();

    std::vector<FunctionDeclStmt::Parameter> params;
    if (currentToken_.type != TokenType::RPAREN) {
        while (true) {
            if (currentToken_.type != TokenType::IDENTIFIER) {
                error("Expected parameter name.");
                return nullptr;
            }
            std::string paramName = currentToken_.value;
            nextToken();  // Consume param name

            std::unique_ptr<TypeAST> paramType = nullptr;
            if (currentToken_.type == TokenType::COLON) {
                nextToken();  // Consume ':'
                paramType = parseType();
            } else {
                error("Expected ':' and type for parameter in Cider.");
                return nullptr;
            }
            params.push_back(std::make_pair(paramName, std::move(paramType)));

            if (currentToken_.type == TokenType::COMMA) {
                nextToken();  // Consume ','
            } else {
                break;
            }
        }
    }

    if (currentToken_.type != TokenType::RPAREN) {
        error("Expected ')' after parameters.");
        return nullptr;
    }
    nextToken();  // Consume ')'

    std::unique_ptr<TypeAST> returnType = nullptr;
    if (currentToken_.type == TokenType::ARROW) {
        nextToken();
        returnType = parseType();
    } else {
        returnType = std::make_unique<TypeAST>("void", 0, line, col);
    }

    std::vector<std::unique_ptr<Statement>> body;
    if (isExtern) {
        if (currentToken_.type != TokenType::SEMICOLON) {
            error("Expected ';' after extern function declaration.");
            return nullptr;
        }
        nextToken();
    } else {
        if (currentToken_.type != TokenType::LBRACE) {
            error("Expected '{' for function body.");
            return nullptr;
        }
        nextToken();  // Consume '{'

        body = parseBlockStatement();

        if (currentToken_.type != TokenType::RBRACE) {
            error("Expected '}' after function body.");
            return nullptr;
        }
        nextToken();
    }

    return std::make_unique<FunctionDeclStmt>(funcName, std::move(params), std::move(body), std::move(returnType), line, col, isAsync, isVirtual, isOverride, isExtern, isDestructor, std::move(templateParams));
}

std::unique_ptr<Statement> Parser::parseStructDeclaration(bool isUnion) {
    int line = currentToken_.line;
    int col = currentToken_.column;
    nextToken();  // struct

    if (currentToken_.type != TokenType::IDENTIFIER) {
        error("Expected struct name.");
        return nullptr;
    }
    std::string structName = currentToken_.value;
    nextToken();

    std::vector<std::string> templateParams;
    if (currentToken_.type == TokenType::LT) {
        templateParams = parseTemplateParams();
    }

    if (currentToken_.type != TokenType::LBRACE) {
        error("Expected '{' for struct body.");
        return nullptr;
    }
    nextToken();

    std::vector<StructDeclStmt::Field> fields;
    while (currentToken_.type != TokenType::RBRACE && currentToken_.type != TokenType::END_OF_FILE) {
        if (currentToken_.type == TokenType::VAR) {
            nextToken();
            if (currentToken_.type != TokenType::IDENTIFIER) {
                error("Expected field name.");
                return nullptr;
            }
            std::string fieldName = currentToken_.value;
            nextToken();
            if (currentToken_.type != TokenType::COLON) {
                error("Expected ':' after field name.");
                return nullptr;
            }
            nextToken();
            auto fieldType = parseType();
            int fieldBits = 0;
            if (currentToken_.type == TokenType::COLON) {
                // Bitfield: var a: int:3;
                nextToken();
                if (currentToken_.type != TokenType::NUMBER) {
                    error("Expected bit width after ':'.");
                    return nullptr;
                }
                uint64_t bw = 0;
                if (!parseIntToken(currentToken_.value, bw)) {
                    error("Invalid bit width: " + currentToken_.value);
                    return nullptr;
                }
                fieldBits = (int)bw;
                nextToken();
            }
            StructDeclStmt::Field fld;
            fld.name = fieldName;
            fld.type = std::move(fieldType);
            fld.bits = fieldBits;
            fields.push_back(std::move(fld));
            nextToken();  // Move past type
            if (currentToken_.type == TokenType::SEMICOLON) nextToken();
        } else {
            error("Expected 'var' for struct field.");
            return nullptr;
        }
    }
    if (currentToken_.type != TokenType::RBRACE) {
        error("Expected '}' after struct body.");
        return nullptr;
    }
    nextToken();

    return std::make_unique<StructDeclStmt>(structName, std::move(fields), line, col, isUnion, std::move(templateParams));
}

std::unique_ptr<Statement> Parser::parseEnumDeclaration() {
    int line = currentToken_.line;
    int col = currentToken_.column;
    nextToken();  // enum

    if (currentToken_.type != TokenType::IDENTIFIER) {
        error("Expected enum name.");
        return nullptr;
    }
    std::string enumName = currentToken_.value;
    nextToken();

    if (currentToken_.type != TokenType::LBRACE) {
        error("Expected '{' for enum body.");
        return nullptr;
    }
    nextToken();

    std::vector<std::string> variants;
    while (currentToken_.type != TokenType::RBRACE && currentToken_.type != TokenType::END_OF_FILE) {
        if (currentToken_.type == TokenType::IDENTIFIER) {
            variants.push_back(currentToken_.value);
            nextToken();
            if (currentToken_.type == TokenType::COMMA) nextToken();
        } else {
            error("Expected variant name in enum.");
            return nullptr;
        }
    }
    if (currentToken_.type != TokenType::RBRACE) {
        error("Expected '}' after enum body.");
        return nullptr;
    }
    nextToken();

    return std::make_unique<EnumDeclStmt>(enumName, std::move(variants), line, col);
}

std::unique_ptr<Statement> Parser::parseTypeAliasStatement() {
    int line = currentToken_.line;
    int col = currentToken_.column;
    nextToken();  // Consume 'type'

    if (currentToken_.type != TokenType::IDENTIFIER) {
        error("Expected identifier after 'type'.");
        return nullptr;
    }
    std::string aliasName = currentToken_.value;
    nextToken();

    if (currentToken_.type != TokenType::ASSIGN) {
        error("Expected '=' after type alias name.");
        return nullptr;
    }
    nextToken();  // Consume '='

    auto targetType = parseType();

    if (currentToken_.type != TokenType::SEMICOLON) {
        error("Expected ';' after type alias.");
        return nullptr;
    }
    nextToken();  // Consume ';'

    return std::make_unique<TypeAliasStmt>(aliasName, std::move(targetType), line, col);
}

std::unique_ptr<Statement> Parser::parseDeferStatement() {
    int line = currentToken_.line;
    int col = currentToken_.column;
    nextToken();  // Consume 'defer'

    auto stmt = parseStatement();
    return std::make_unique<DeferStmt>(std::move(stmt), line, col);
}

std::unique_ptr<Statement> Parser::parseAsmStatement() {
    int line = currentToken_.line;
    int col = currentToken_.column;
    nextToken();  // Consume 'asm'

    if (currentToken_.type != TokenType::LBRACE) {
        error("Expected '{' after 'asm'.");
        return nullptr;
    }
    nextToken();  // Consume '{'

    std::string code = "";
    while (currentToken_.type == TokenType::STRING) {
        if (!code.empty()) code += "\n";
        code += currentToken_.value;
        nextToken();
    }

    if (code.empty()) {
        error("Expected at least one assembly string literal.");
        return nullptr;
    }

    if (currentToken_.type != TokenType::RBRACE) {
        error("Expected '}' after assembly code.");
        return nullptr;
    }
    nextToken();  // Consume '}'

    auto stmt = std::make_unique<AsmStmt>(code, line, col);

    // Extract {var} placeholders
    size_t pos = 0;
    while ((pos = code.find('{', pos)) != std::string::npos) {
        size_t end = code.find('}', pos);
        if (end != std::string::npos) {
            std::string varName = code.substr(pos + 1, end - pos - 1);
            stmt->localVariables[varName] = varName;
            pos = end + 1;
        } else {
            break;
        }
    }

    return stmt;
}

std::unique_ptr<Statement> Parser::parseMatchStatement() {
    int line = currentToken_.line;
    int col = currentToken_.column;
    nextToken();  // Consume 'match'

    if (currentToken_.type != TokenType::LPAREN) {
        error("Expected '(' after 'match'.");
        return nullptr;
    }
    nextToken();  // Consume '('
    auto expr = parseExpression(false);
    if (currentToken_.type != TokenType::RPAREN) {
        error("Expected ')' after match expression.");
        return nullptr;
    }
    nextToken();  // Consume ')'

    if (currentToken_.type != TokenType::LBRACE) {
        error("Expected '{' for match body.");
        return nullptr;
    }
    nextToken();  // Consume '{'

    std::vector<MatchStmt::Arm> arms;
    while (currentToken_.type != TokenType::RBRACE && currentToken_.type != TokenType::END_OF_FILE) {
        auto pattern = parseExpression(false);
        if (currentToken_.type != TokenType::FAT_ARROW) {
            error("Expected '=>' after match pattern.");
            return nullptr;
        }
        nextToken();  // Consume '=>'
        auto body = parseStatement();
        arms.push_back({std::move(pattern), std::move(body)});

        if (currentToken_.type == TokenType::COMMA) nextToken();
    }

    if (currentToken_.type != TokenType::RBRACE) {
        error("Expected '}' after match arms.");
        return nullptr;
    }
    nextToken();  // Consume '}'

    return std::make_unique<MatchStmt>(std::move(expr), std::move(arms), line, col);
}

std::unique_ptr<Statement> Parser::parseClassDeclaration() {
    int line = currentToken_.line;
    int col = currentToken_.column;
    nextToken();

    if (currentToken_.type != TokenType::IDENTIFIER) {
        error("Expected class name.");
        return nullptr;
    }
    std::string className = currentToken_.value;
    std::string superclassName = "";

    if (peekToken_.type == TokenType::EXTENDS) {
        nextToken();
        if (!expect(TokenType::IDENTIFIER)) return nullptr;
        superclassName = currentToken_.value;
    }

    if (!expect(TokenType::LBRACE)) return nullptr;
    nextToken();

    std::vector<std::unique_ptr<Statement>> members;
    while (currentToken_.type != TokenType::RBRACE && currentToken_.type != TokenType::END_OF_FILE) {
        if (currentToken_.type == TokenType::FUNCTION || currentToken_.type == TokenType::FUNC) {
            members.push_back(parseFunctionDeclaration());
        } else if (currentToken_.type == TokenType::ASYNC) {
            nextToken();
            if (currentToken_.type == TokenType::FUNCTION || currentToken_.type == TokenType::FUNC) {
                members.push_back(parseFunctionDeclaration(true));
            } else {
                error("Expected 'func' after 'async'.");
            }
        } else if (currentToken_.type == TokenType::VIRTUAL) {
            nextToken();
            bool isAsync = false;
            if (currentToken_.type == TokenType::ASYNC) {
                isAsync = true;
                nextToken();
            }
            if (currentToken_.type == TokenType::FUNCTION || currentToken_.type == TokenType::FUNC) {
                members.push_back(parseFunctionDeclaration(isAsync, true, false));
            } else {
                error("Expected 'func' after 'virtual'.");
            }
        } else if (currentToken_.type == TokenType::OVERRIDE) {
            nextToken();
            bool isAsync = false;
            if (currentToken_.type == TokenType::ASYNC) {
                isAsync = true;
                nextToken();
            }
            if (currentToken_.type == TokenType::FUNCTION || currentToken_.type == TokenType::FUNC) {
                members.push_back(parseFunctionDeclaration(isAsync, false, true));
            } else {
                error("Expected 'func' after 'override'.");
            }
        } else if (currentToken_.type == TokenType::VAR) {
            members.push_back(parseVarDeclarationStatement());
        } else {
            error("Only methods and fields are supported in classes.");
            return nullptr;
        }
    }

    if (currentToken_.type != TokenType::RBRACE) {
        error("Expected '}' after class body.");
        return nullptr;
    }
    nextToken();

    return std::make_unique<ClassDeclStmt>(className, superclassName, std::move(members), line, col);
}

std::unique_ptr<Statement> Parser::parseImportStatement() {
    int line = currentToken_.line;
    int col = currentToken_.column;
    nextToken();

    std::string moduleName;
    if (currentToken_.type == TokenType::DOT) {
        moduleName += ".";
        nextToken();
    }

    if (currentToken_.type != TokenType::IDENTIFIER && !isTypeToken(currentToken_.type)) {
        error("Expected module name.");
        return nullptr;
    }
    moduleName += currentToken_.value;
    nextToken();

    while (currentToken_.type == TokenType::DOT) {
        moduleName += ".";
        nextToken();
        if (currentToken_.type != TokenType::IDENTIFIER && !isTypeToken(currentToken_.type)) {
            error("Expected identifier after '.' in module name.");
            return nullptr;
        }
        moduleName += currentToken_.value;
        nextToken();
    }

    if (currentToken_.type != TokenType::SEMICOLON) {
        error("Expected SEMICOLON.");
        return nullptr;
    }
    nextToken();

    return std::make_unique<ImportStmt>(moduleName, line, col);
}

std::unique_ptr<Statement> Parser::parseFromImportStatement() {
    int line = currentToken_.line;
    int col = currentToken_.column;
    nextToken();

    std::string moduleName;
    if (currentToken_.type == TokenType::DOT) {
        moduleName += ".";
        nextToken();
    }

    if (currentToken_.type != TokenType::IDENTIFIER && !isTypeToken(currentToken_.type)) {
        error("Expected module name.");
        return nullptr;
    }
    moduleName += currentToken_.value;
    nextToken();

    while (currentToken_.type == TokenType::DOT) {
        moduleName += ".";
        nextToken();
        if (currentToken_.type != TokenType::IDENTIFIER && !isTypeToken(currentToken_.type)) {
            error("Expected identifier after '.' in module name.");
            return nullptr;
        }
        moduleName += currentToken_.value;
        nextToken();
    }

    if (currentToken_.type != TokenType::IMPORT) {
        error("Expected 'import'.");
        return nullptr;
    }
    nextToken();

    std::vector<std::string> symbols;
    while (true) {
        if (currentToken_.type != TokenType::IDENTIFIER) {
            error("Expected symbol name.");
            return nullptr;
        }
        symbols.push_back(currentToken_.value);
        nextToken();

        if (currentToken_.type == TokenType::COMMA) {
            nextToken();
        } else {
            break;
        }
    }

    if (currentToken_.type != TokenType::SEMICOLON) {
        error("Expected SEMICOLON.");
        return nullptr;
    }
    nextToken();

    return std::make_unique<FromImportStmt>(moduleName, symbols, line, col);
}

std::unique_ptr<Statement> Parser::parseExportStatement() {
    int line = currentToken_.line;
    int col = currentToken_.column;
    nextToken();

    if (currentToken_.type == TokenType::LBRACE) {
        nextToken();
        std::vector<std::unique_ptr<Statement>> exports;
        while (true) {
            if (currentToken_.type != TokenType::IDENTIFIER) {
                error("Expected identifier in export list.");
                return nullptr;
            }
            auto identExpr = std::make_unique<IdentifierExpr>(currentToken_.value, currentToken_.line, currentToken_.column);
            nextToken();
            exports.push_back(std::make_unique<ExportExpressionStmt>(std::move(identExpr), line, col));

            if (currentToken_.type == TokenType::COMMA) {
                nextToken();
            } else {
                break;
            }
        }
        if (currentToken_.type != TokenType::RBRACE) return nullptr;
        nextToken();
        if (currentToken_.type != TokenType::SEMICOLON) return nullptr;
        nextToken();
        return std::make_unique<BlockStmt>(std::move(exports), line, col);
    }

    std::unique_ptr<Statement> stmt = nullptr;
    if (currentToken_.type == TokenType::VAR)
        stmt = parseVarDeclarationStatement();
    else if (currentToken_.type == TokenType::CONST)
        stmt = parseVarDeclarationStatement();
    else if (currentToken_.type == TokenType::FUNCTION || currentToken_.type == TokenType::FUNC)
        stmt = parseFunctionDeclaration();
    else if (currentToken_.type == TokenType::STRUCT)
        stmt = parseStructDeclaration();
    else if (currentToken_.type == TokenType::UNION)
        stmt = parseStructDeclaration(true);
    else if (currentToken_.type == TokenType::ENUM)
        stmt = parseEnumDeclaration();
    else if (currentToken_.type == TokenType::CLASS)
        stmt = parseClassDeclaration();
    else if (currentToken_.type == TokenType::IDENTIFIER || currentToken_.type == TokenType::THIS) {
        auto expr = parseExpression(LOWEST);
        if (!expr) return nullptr;
        if (currentToken_.type != TokenType::SEMICOLON) {
            error("Expected ';' after export expression.");
            return nullptr;
        }
        nextToken();
        return std::make_unique<ExportExpressionStmt>(std::move(expr), line, col);
    } else {
        error("Unexpected token after 'export'.");
        return nullptr;
    }

    return std::make_unique<ExportStmt>(std::move(stmt), line, col);
}

std::unique_ptr<Statement> Parser::parseBreakStatement() {
    int line = currentToken_.line;
    int col = currentToken_.column;
    nextToken();
    if (currentToken_.type == TokenType::SEMICOLON) nextToken();
    return std::make_unique<BreakStmt>(line, col);
}

std::unique_ptr<Statement> Parser::parseContinueStatement() {
    int line = currentToken_.line;
    int col = currentToken_.column;
    nextToken();
    if (currentToken_.type == TokenType::SEMICOLON) nextToken();
    return std::make_unique<ContinueStmt>(line, col);
}

std::unique_ptr<Statement> Parser::parseThrowStatement() {
    int line = currentToken_.line;
    int col = currentToken_.column;
    nextToken();
    auto expr = parseExpression();
    if (!expr) return nullptr;
    if (currentToken_.type == TokenType::SEMICOLON) nextToken();
    return std::make_unique<ThrowStmt>(std::move(expr), line, col);
}

std::unique_ptr<Statement> Parser::parseTryCatchStatement() {
    int line = currentToken_.line;
    int col = currentToken_.column;
    nextToken();

    if (currentToken_.type != TokenType::LBRACE) return nullptr;
    nextToken();
    auto tryBlock = parseBlockStatement();
    if (currentToken_.type != TokenType::RBRACE) return nullptr;
    nextToken();

    if (currentToken_.type != TokenType::CATCH) return nullptr;
    nextToken();

    if (currentToken_.type != TokenType::LPAREN) return nullptr;
    nextToken();

    if (currentToken_.type != TokenType::IDENTIFIER) return nullptr;
    std::string catchVar = currentToken_.value;
    nextToken();

    if (currentToken_.type != TokenType::RPAREN) return nullptr;
    nextToken();

    if (currentToken_.type != TokenType::LBRACE) return nullptr;
    nextToken();
    auto catchBlock = parseBlockStatement();
    if (currentToken_.type != TokenType::RBRACE) return nullptr;
    nextToken();

    return std::make_unique<TryCatchStmt>(std::move(tryBlock), catchVar, std::move(catchBlock), line, col);
}

std::vector<std::unique_ptr<Statement>> Parser::parseBlockStatement() {
    std::vector<std::unique_ptr<Statement>> statements;
    while (currentToken_.type != TokenType::RBRACE && currentToken_.type != TokenType::END_OF_FILE) {
        auto stmt = parseStatement();
        if (stmt) statements.push_back(std::move(stmt));
        else nextToken();
    }
    return statements;
}

std::unique_ptr<Statement> Parser::parseInterfaceDeclaration() {
    nextToken();  // consume 'interface'
    int line = currentToken_.line;
    int col = currentToken_.column;
    if (currentToken_.type != TokenType::IDENTIFIER) { error("Expected interface name."); return nullptr; }
    std::string name = currentToken_.value;
    nextToken();
    if (currentToken_.type != TokenType::LBRACE) { error("Expected '{' after interface name."); return nullptr; }
    nextToken();
    std::vector<InterfaceMethod> methods;
    while (currentToken_.type != TokenType::RBRACE && currentToken_.type != TokenType::END_OF_FILE) {
        bool isFunc = currentToken_.type == TokenType::FUNCTION || currentToken_.type == TokenType::FUNC;
        if (!isFunc) { error("Expected method declaration."); return nullptr; }
        nextToken();
        int mLine = currentToken_.line;
        int mCol = currentToken_.column;
        if (currentToken_.type != TokenType::IDENTIFIER) { error("Expected method name."); return nullptr; }
        std::string mName = currentToken_.value;
        nextToken();
        if (currentToken_.type != TokenType::LPAREN) { error("Expected '('."); return nullptr; }
        nextToken();
        std::vector<std::pair<std::string, std::unique_ptr<TypeAST>>> params;
        while (currentToken_.type != TokenType::RPAREN) {
            if (currentToken_.type != TokenType::IDENTIFIER) { error("Expected param name."); return nullptr; }
            std::string pName = currentToken_.value;
            nextToken();
            if (currentToken_.type != TokenType::COLON) { error("Expected ':'."); return nullptr; }
            nextToken();
            auto pType = parseType();
            params.push_back({pName, std::move(pType)});
            if (currentToken_.type == TokenType::COMMA) nextToken();
        }
        nextToken();  // ')'
        std::unique_ptr<TypeAST> retType = std::make_unique<TypeAST>("void", 0, mLine, mCol);
        if (currentToken_.type == TokenType::ARROW) {
            nextToken();
            retType = parseType();
        }
        if (currentToken_.type != TokenType::SEMICOLON) { error("Expected ';'."); return nullptr; }
        nextToken();
        methods.push_back(InterfaceMethod(mName, std::move(params), std::move(retType), mLine, mCol));
    }
    if (currentToken_.type != TokenType::RBRACE) { error("Expected '}'."); return nullptr; }
    nextToken();
    return std::make_unique<InterfaceDeclStmt>(name, std::move(methods), line, col);
}

}  // namespace cider
