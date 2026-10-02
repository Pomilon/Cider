// lexer.cpp — one character in, one token out, with a position.
//
// Maximal munch everywhere: `==` before `=`, `::` before `:`, `...` before
// `.`. There is no `->`: `p->f` lexes as `-` then `>`, and the parser rejects
// it, because a token the language does not have must fail loudly at the layer
// that knows the grammar rather than silently here.

#include "lexer/lexer.h"

#include <unordered_map>

namespace cider {

namespace {

bool isAlpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool isDigit(char c) {
    return c >= '0' && c <= '9';
}

bool isHex(char c) {
    return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool isBin(char c) {
    return c == '0' || c == '1';
}

const std::unordered_map<std::string, TokenKind>& keywords() {
    static const std::unordered_map<std::string, TokenKind> table = {
        {"func", TokenKind::KwFunc}, {"extern", TokenKind::KwExtern},
        {"struct", TokenKind::KwStruct}, {"class", TokenKind::KwClass},
        {"interface", TokenKind::KwInterface}, {"union", TokenKind::KwUnion},
        {"enum", TokenKind::KwEnum}, {"var", TokenKind::KwVar},
        {"const", TokenKind::KwConst}, {"type", TokenKind::KwType},
        {"import", TokenKind::KwImport}, {"from", TokenKind::KwFrom},
        {"export", TokenKind::KwExport}, {"return", TokenKind::KwReturn},
        {"if", TokenKind::KwIf}, {"else", TokenKind::KwElse},
        {"while", TokenKind::KwWhile}, {"for", TokenKind::KwFor},
        {"in", TokenKind::KwIn}, {"match", TokenKind::KwMatch},
        {"default", TokenKind::KwDefault}, {"break", TokenKind::KwBreak},
        {"continue", TokenKind::KwContinue}, {"defer", TokenKind::KwDefer},
        {"try", TokenKind::KwTry}, {"catch", TokenKind::KwCatch},
        {"throw", TokenKind::KwThrow}, {"delete", TokenKind::KwDelete},
        {"new", TokenKind::KwNew}, {"as", TokenKind::KwAs},
        {"and", TokenKind::KwAnd}, {"or", TokenKind::KwOr},
        {"not", TokenKind::KwNot}, {"self", TokenKind::KwSelf},
        {"super", TokenKind::KwSuper}, {"init", TokenKind::KwInit},
        {"deinit", TokenKind::KwDeinit}, {"virtual", TokenKind::KwVirtual},
        {"private", TokenKind::KwPrivate}, {"public", TokenKind::KwPublic},
        {"true", TokenKind::KwTrue}, {"false", TokenKind::KwFalse},
        {"nilPtr", TokenKind::KwNilPtr}, {"fini", TokenKind::KwFini},
        {"sizeof", TokenKind::KwSizeof}, {"asm", TokenKind::KwAsm},
        {"int", TokenKind::KwInt}, {"i8", TokenKind::KwI8},
        {"i16", TokenKind::KwI16}, {"i32", TokenKind::KwI32},
        {"i64", TokenKind::KwI64}, {"u8", TokenKind::KwU8},
        {"u16", TokenKind::KwU16}, {"u32", TokenKind::KwU32},
        {"u64", TokenKind::KwU64}, {"f32", TokenKind::KwF32},
        {"f64", TokenKind::KwF64}, {"bool", TokenKind::KwBool},
        {"char", TokenKind::KwChar}, {"byte", TokenKind::KwByte},
        {"string", TokenKind::KwString}, {"void", TokenKind::KwVoid},
    };
    return table;
}

// The value of an escape, or -1 for a letter that escapes nothing. Anything
// else after a backslash is a diagnostic, not a guess.
int escapeValue(char c) {
    switch (c) {
        case 'n': return 10;
        case 't': return 9;
        case 'r': return 13;
        case '\\': return 92;
        case '\'': return 39;
        case '"': return 34;
        case '0': return 0;
        case '{': return 123;
        case '}': return 125;
        default: return -1;
    }
}

}  // namespace

Lexer::Lexer(std::string file, std::string source)
    : file_(std::move(file)), source_(std::move(source)) {}

char Lexer::peek() const {
    return atEnd() ? 0 : source_[cursor_];
}

char Lexer::peekAt(uint64_t n) const {
    uint64_t want = cursor_ + n;
    if (want < source_.size()) {
        return source_[want];
    }
    return 0;
}

char Lexer::advance() {
    char c = source_[cursor_];
    ++cursor_;
    if (c == '\n') {
        ++line_;
        column_ = 1;
    } else {
        ++column_;
    }
    return c;
}

bool Lexer::atEnd() const {
    return cursor_ >= source_.size();
}

bool Lexer::match(char want) {
    if (!atEnd() && source_[cursor_] == want) {
        advance();
        return true;
    }
    return false;
}

void Lexer::fail(Position pos, const std::string& message) {
    errors_.push_back(LexError{pos, message});
}

void Lexer::skipSpaceAndComments() {
    for (;;) {
        while (!atEnd() && (peek() == ' ' || peek() == '\t' || peek() == '\r' || peek() == '\n')) {
            advance();
        }
        if (peek() == '/' && peekAt(1) == '/') {
            while (!atEnd() && peek() != '\n') {
                advance();
            }
            continue;
        }
        return;
    }
}

std::vector<Token> Lexer::lex() {
    std::vector<Token> out;
    for (;;) {
        skipSpaceAndComments();
        if (atEnd()) {
            break;
        }
        Token token = nextToken();
        bool done = token.kind == TokenKind::End;
        out.push_back(std::move(token));
        if (done) {
            return out;
        }
    }
    Token end;
    end.kind = TokenKind::End;
    end.pos = Position{file_, line_, column_};
    out.push_back(std::move(end));
    return out;
}

Token Lexer::nextToken() {
    Position start{file_, line_, column_};
    uint64_t begin = cursor_;
    char c = advance();
    Token token;
    token.pos = start;

    if (isAlpha(c)) {
        while (!atEnd() && (isAlpha(peek()) || isDigit(peek()))) {
            advance();
        }
        token.text = source_.substr(begin, cursor_ - begin);
        auto found = keywords().find(token.text);
        token.kind = found == keywords().end() ? TokenKind::Ident : found->second;
        return token;
    }
    if (isDigit(c)) {
        // Step back: number() reads from the first digit itself.
        --cursor_;
        --column_;
        return number();
    }
    switch (c) {
        case '\'': {
            --cursor_;
            --column_;
            return character();
        }
        case '"': {
            --cursor_;
            --column_;
            return string();
        }
        case '(': token.kind = TokenKind::LParen; break;
        case ')': token.kind = TokenKind::RParen; break;
        case '{': token.kind = TokenKind::LBrace; break;
        case '}': token.kind = TokenKind::RBrace; break;
        case '[': token.kind = TokenKind::LBracket; break;
        case ']': token.kind = TokenKind::RBracket; break;
        case ',': token.kind = TokenKind::Comma; break;
        case ';': token.kind = TokenKind::Semi; break;
        case '?': token.kind = TokenKind::Question; break;
        case '~': token.kind = TokenKind::Tilde; break;
        case '#': token.kind = TokenKind::Hash; break;
        case ':':
            token.kind = match(':') ? TokenKind::ColonColon : TokenKind::Colon;
            break;
        case '.':
            if (peek() == '.' && peekAt(1) == '.') {
                advance();
                advance();
                token.kind = TokenKind::Ellipsis;
            } else {
                token.kind = TokenKind::Dot;
            }
            break;
        case '+':
            token.kind = match('=') ? TokenKind::PlusEq : TokenKind::Plus;
            break;
        case '-':
            token.kind = match('=') ? TokenKind::MinusEq : TokenKind::Minus;
            break;
        case '*':
            token.kind = match('=') ? TokenKind::StarEq : TokenKind::Star;
            break;
        case '/':
            token.kind = match('=') ? TokenKind::SlashEq : TokenKind::Slash;
            break;
        case '%':
            token.kind = match('=') ? TokenKind::PercentEq : TokenKind::Percent;
            break;
        case '&':
            if (match('&')) {
                token.kind = TokenKind::AndAnd;
            } else {
                token.kind = match('=') ? TokenKind::AmpEq : TokenKind::Amp;
            }
            break;
        case '|':
            if (match('|')) {
                token.kind = TokenKind::OrOr;
            } else {
                token.kind = match('=') ? TokenKind::PipeEq : TokenKind::Pipe;
            }
            break;
        case '^':
            token.kind = match('=') ? TokenKind::CaretEq : TokenKind::Caret;
            break;
        case '!':
            token.kind = match('=') ? TokenKind::BangEq : TokenKind::Bang;
            break;
        case '=':
            if (match('=')) {
                token.kind = TokenKind::EqEq;
            } else {
                token.kind = match('>') ? TokenKind::FatArrow : TokenKind::Eq;
            }
            break;
        case '<':
            if (match('<')) {
                token.kind = match('=') ? TokenKind::LtLtEq : TokenKind::LtLt;
            } else {
                token.kind = match('=') ? TokenKind::LtEq : TokenKind::Lt;
            }
            break;
        case '>':
            if (match('>')) {
                token.kind = match('=') ? TokenKind::GtGtEq : TokenKind::GtGt;
            } else {
                token.kind = match('=') ? TokenKind::GtEq : TokenKind::Gt;
            }
            break;
        default: {
            token.kind = TokenKind::Error;
            fail(start, std::string("unexpected character '") + c + "'");
            break;
        }
    }
    token.text = source_.substr(begin, cursor_ - begin);
    return token;
}

Token Lexer::number() {
    Position start{file_, line_, column_};
    Token token;
    token.kind = TokenKind::IntLit;
    token.pos = start;
    uint64_t begin = cursor_;

    uint8_t base = 10;
    if (peek() == '0' && (peekAt(1) == 'x' || peekAt(1) == 'X')) {
        base = 16;
        advance();
        advance();
    } else if (peek() == '0' && (peekAt(1) == 'b' || peekAt(1) == 'B')) {
        base = 2;
        advance();
        advance();
    }
    uint64_t digitsBegin = cursor_;
    while (!atEnd() && (isHex(peek()) || peek() == '_')) {
        advance();
    }
    std::string raw = source_.substr(digitsBegin, cursor_ - digitsBegin);
    std::string digits;
    for (char d : raw) {
        if (d != '_') {
            digits.push_back(d);
        }
    }
    bool valid = !digits.empty();
    for (char d : digits) {
        bool ok = base == 16 ? isHex(d) : (base == 2 ? isBin(d) : isDigit(d));
        if (!ok) {
            valid = false;
        }
    }
    // A `.` followed by a digit makes it a float; otherwise the dots belong to
    // whatever follows.
    if (valid && base == 10 && peek() == '.' && isDigit(peekAt(1))) {
        advance();
        while (!atEnd() && (isDigit(peek()) || peek() == '_')) {
            advance();
        }
        if ((peek() == 'e' || peek() == 'E') &&
            (isDigit(peekAt(1)) || peekAt(1) == '+' || peekAt(1) == '-')) {
            advance();
            if (peek() == '+' || peek() == '-') {
                advance();
            }
            while (!atEnd() && (isDigit(peek()) || peek() == '_')) {
                advance();
            }
        }
        std::string spelling = source_.substr(begin, cursor_ - begin);
        std::string clean;
        for (char d : spelling) {
            if (d != '_') {
                clean.push_back(d);
            }
        }
        token.kind = TokenKind::FloatLit;
        token.text = spelling;
        token.lit.floatDigits = clean;
        return token;
    }
    token.text = source_.substr(begin, cursor_ - begin);
    if (!valid) {
        token.kind = TokenKind::Error;
        fail(start, "invalid digit for base");
        return token;
    }
    token.lit.intBase = base;
    token.lit.intDigits = digits;
    return token;
}

Token Lexer::character() {
    Position start{file_, line_, column_};
    Token token;
    token.kind = TokenKind::CharLit;
    token.pos = start;
    advance();  // the opening quote
    if (atEnd() || peek() == '\n') {
        token.kind = TokenKind::Error;
        fail(start, "unterminated character literal");
        return token;
    }
    uint32_t value = 0;
    char c = advance();
    if (c == '\\') {
        if (atEnd()) {
            token.kind = TokenKind::Error;
            fail(start, "unterminated character literal");
            return token;
        }
        int escaped = escapeValue(advance());
        if (escaped < 0) {
            token.kind = TokenKind::Error;
            fail(start, "unknown escape");
            while (!atEnd() && peek() != '\'' && peek() != '\n') {
                advance();
            }
            if (!atEnd() && peek() == '\'') {
                advance();
            }
            return token;
        }
        value = static_cast<uint32_t>(escaped);
    } else {
        value = static_cast<uint32_t>(static_cast<unsigned char>(c));
    }
    if (!match('\'')) {
        token.kind = TokenKind::Error;
        fail(start, "a character literal holds exactly one character");
        return token;
    }
    token.text = std::string("'") + (value < 128 ? std::string(1, static_cast<char>(value)) : "?") + "'";
    token.lit.charValue = value;
    return token;
}

Token Lexer::string() {
    Position start{file_, line_, column_};
    Token token;
    token.kind = TokenKind::StringLit;
    token.pos = start;
    advance();  // the opening quote
    std::string spelling = "\"";
    std::string text;
    auto flushText = [&](Position) {
        if (!text.empty()) {
            StringPart part;
            part.isExpr = false;
            part.text = text;
            part.pos = start;
            token.lit.parts.push_back(std::move(part));
            text.clear();
        }
    };
    for (;;) {
        if (atEnd() || peek() == '\n') {
            token.kind = TokenKind::Error;
            fail(start, "unterminated string literal");
            token.text = spelling;
            return token;
        }
        char c = peek();
        if (c == '"') {
            advance();
            spelling += "\"";
            flushText(start);
            token.text = spelling;
            return token;
        }
        if (c == '{') {
            Position brace{file_, line_, column_};
            advance();
            spelling += "{";
            uint64_t inner = cursor_;
            std::vector<Token> expr = braced(0);
            std::string innerText = source_.substr(inner, cursor_ - inner - 1);
            spelling += innerText;
            spelling += "}";
            flushText(start);
            StringPart part;
            part.isExpr = true;
            part.text = innerText;
            part.expr = std::move(expr);
            part.pos = brace;
            token.lit.parts.push_back(std::move(part));
            continue;
        }
        if (c == '\\') {
            advance();
            spelling += "\\";
            if (atEnd()) {
                break;
            }
            char e = advance();
            spelling += e;
            int escaped = escapeValue(e);
            if (escaped < 0) {
                token.kind = TokenKind::Error;
                fail(start, "unknown escape");
                continue;
            }
            text.push_back(static_cast<char>(escaped));
            continue;
        }
        advance();
        spelling += c;
        text.push_back(c);
    }
    token.kind = TokenKind::Error;
    fail(start, "unterminated string literal");
    token.text = spelling;
    return token;
}

std::vector<Token> Lexer::braced(int depth) {
    std::vector<Token> out;
    for (;;) {
        skipSpaceAndComments();
        if (atEnd()) {
            Position here{file_, line_, column_};
            fail(here, "unterminated interpolation");
            return out;
        }
        if (peek() == '}' && depth == 0) {
            advance();
            return out;
        }
        Token token = nextToken();
        if (token.kind == TokenKind::End) {
            Position here{file_, line_, column_};
            fail(here, "unterminated interpolation");
            return out;
        }
        if (token.kind == TokenKind::LBrace) {
            ++depth;
        }
        if (token.kind == TokenKind::RBrace) {
            --depth;
        }
        out.push_back(std::move(token));
    }
}

}  // namespace cider
