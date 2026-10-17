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

bool isOct(char c) {
    return c >= '0' && c <= '7';
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
        {"not", TokenKind::KwNot}, {"self", TokenKind::KwSelf}, {"this", TokenKind::KwThis},
        {"super", TokenKind::KwSuper}, {"init", TokenKind::KwInit},
        {"deinit", TokenKind::KwDeinit}, {"virtual", TokenKind::KwVirtual},
        {"private", TokenKind::KwPrivate}, {"public", TokenKind::KwPublic},
        {"true", TokenKind::KwTrue}, {"false", TokenKind::KwFalse},
        {"nil", TokenKind::KwNilPtr}, {"nilPtr", TokenKind::KwNilPtr}, {"uninit", TokenKind::KwUninit}, {"fini", TokenKind::KwFini},
        {"sizeof", TokenKind::KwSizeof}, {"asm", TokenKind::KwAsm},
        {"volatile", TokenKind::KwVolatile}, {"experimental", TokenKind::KwExperimental},
        {"int", TokenKind::KwInt}, {"i8", TokenKind::KwI8},
        {"i16", TokenKind::KwI16}, {"i32", TokenKind::KwI32},
        {"i64", TokenKind::KwI64}, {"u8", TokenKind::KwU8},
        {"u16", TokenKind::KwU16}, {"u32", TokenKind::KwU32},
        {"u64", TokenKind::KwU64}, {"f32", TokenKind::KwF32},
        {"f64", TokenKind::KwF64},
        // `float` is the f32 alias (TYPES.md §10): same token, same type,
        // so nothing downstream ever sees a separate spelling.
        {"float", TokenKind::KwF32}, {"bool", TokenKind::KwBool},
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
        // A `u` glued to a quote opens a wide literal (`u"AB"`, `u'Z'`):
        // anything else keeps the ordinary identifier path, so a lone `u`
        // or `u` before a space stays a name.
        if (token.text == "u" && (peek() == '"' || peek() == '\'')) {
            char quote = peek();
            cursor_ = begin;
            column_ = start.column;
            if (quote == '"') {
                return wideString();
            }
            return wideChar();
        }
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
            if (match('+')) {
                token.kind = TokenKind::PlusPlus;
            } else {
                token.kind = match('=') ? TokenKind::PlusEq : TokenKind::Plus;
            }
            break;
        case '-':
            if (match('>')) {
                token.kind = TokenKind::Arrow;
            } else if (match('-')) {
                token.kind = TokenKind::MinusMinus;
            } else {
                token.kind = match('=') ? TokenKind::MinusEq : TokenKind::Minus;
            }
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
    } else if (peek() == '0' && (peekAt(1) == 'o' || peekAt(1) == 'O')) {
        base = 8;
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
        bool ok = base == 16 ? isHex(d)
                             : (base == 2 ? isBin(d) : (base == 8 ? isOct(d) : isDigit(d)));
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
        int escaped = -1;
        if (!atEnd() && peek() == 'x' && isHex(peekAt(1)) && isHex(peekAt(2))) {
            advance();
            int hi = advance();
            int lo = advance();
            hi = hi <= '9' ? hi - '0' : (hi <= 'F' ? hi - 'A' + 10 : hi - 'a' + 10);
            lo = lo <= '9' ? lo - '0' : (lo <= 'F' ? lo - 'A' + 10 : lo - 'a' + 10);
            escaped = hi * 16 + lo;
        } else if (!atEnd() && peek() != 'x') {
            escaped = escapeValue(advance());
        }
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
            // FEATURES §1.4: `{{` is a literal brace, and a `{` that cannot
            // open an interpolation (nothing after it but the closing
            // quote, a newline, or the file) is literal too — no expression
            // could start there, so there is nothing to guess about.
            if (!atEnd() && peek() == '{') {
                advance();
                spelling += "{";
                text.push_back('{');
                continue;
            }
            if (atEnd() || peek() == '"' || peek() == '\n') {
                text.push_back('{');
                continue;
            }
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
        if (c == '}') {
            // FEATURES §1.4: `}}` is a literal `}`, and a lone `}` was
            // always literal — the closer only closes inside an
            // interpolation, never in plain text.
            advance();
            spelling += "}";
            if (!atEnd() && peek() == '}') {
                advance();
                spelling += "}";
            }
            text.push_back('}');
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
            if (e == 'x') {
                // FEATURES §1.4: exactly two hex digits name a byte; anything
                // else is the same unknown-escape diagnostic as any other
                // bad letter after a backslash.
                auto digit = [](char h) -> int {
                    if (h >= '0' && h <= '9') {
                        return h - '0';
                    }
                    if (h >= 'a' && h <= 'f') {
                        return h - 'a' + 10;
                    }
                    if (h >= 'A' && h <= 'F') {
                        return h - 'A' + 10;
                    }
                    return -1;
                };
                int hi = !atEnd() ? digit(peek()) : -1;
                int lo = (hi >= 0 && cursor_ + 1 < source_.size())
                             ? digit(source_[cursor_ + 1])
                             : -1;
                if (hi < 0 || lo < 0) {
                    token.kind = TokenKind::Error;
                    fail(start, "unknown escape");
                    continue;
                }
                spelling += peek();
                spelling += source_[cursor_ + 1];
                advance();
                advance();
                text.push_back(static_cast<char>(hi * 16 + lo));
                continue;
            }
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

Token Lexer::wideString() {
    Position start{file_, line_, column_};
    Token token;
    token.kind = TokenKind::WideStringLit;
    token.pos = start;
    advance();  // the u
    advance();  // the opening quote
    std::string spelling = "u\"";
    auto digit = [](char h) -> int {
        if (h >= '0' && h <= '9') {
            return h - '0';
        }
        if (h >= 'a' && h <= 'f') {
            return h - 'a' + 10;
        }
        if (h >= 'A' && h <= 'F') {
            return h - 'A' + 10;
        }
        return -1;
    };
    // One UTF-8 sequence becomes one code point (astral planes arrive as a
    // surrogate pair, the only honest UTF-16 spelling of them). Anything
    // that is not well-formed UTF-8 is a diagnostic, never a guess.
    auto pushUtf8 = [&](unsigned char first) -> bool {
        uint32_t point = 0;
        int rest = 0;
        if ((first & 0xE0) == 0xC0) {
            point = first & 0x1F;
            rest = 1;
        } else if ((first & 0xF0) == 0xE0) {
            point = first & 0x0F;
            rest = 2;
        } else if ((first & 0xF8) == 0xF0) {
            point = first & 0x07;
            rest = 3;
        } else {
            return false;
        }
        spelling += static_cast<char>(first);
        for (int i = 0; i < rest; ++i) {
            if (atEnd() || (peek() & 0xC0) != 0x80) {
                return false;
            }
            unsigned char cont =
                static_cast<unsigned char>(advance());
            spelling += static_cast<char>(cont);
            point = (point << 6) | (cont & 0x3F);
        }
        if (point > 0x10FFFF || (rest == 1 && point < 0x80) ||
            (rest == 2 && point < 0x800) || (rest == 3 && point < 0x10000) ||
            (point >= 0xD800 && point <= 0xDFFF)) {
            return false;
        }
        if (point < 0x10000) {
            token.lit.wideUnits.push_back(static_cast<uint16_t>(point));
        } else {
            point -= 0x10000;
            token.lit.wideUnits.push_back(
                static_cast<uint16_t>(0xD800 + (point >> 10)));
            token.lit.wideUnits.push_back(
                static_cast<uint16_t>(0xDC00 + (point & 0x3FF)));
        }
        return true;
    };
    auto failHere = [&](const std::string& message) {
        token.kind = TokenKind::Error;
        fail(start, message);
    };
    for (;;) {
        if (atEnd() || peek() == '\n') {
            failHere("unterminated string literal");
            token.text = spelling;
            return token;
        }
        char c = peek();
        if (c == '"') {
            advance();
            spelling += "\"";
            token.text = spelling;
            return token;
        }
        if (c == '{') {
            advance();
            spelling += "{";
            // `{{` spells one brace; narrow strings use the same two
            // characters for it. A lone `{` would interpolate, which wide
            // literals do not do.
            if (!atEnd() && peek() == '{') {
                advance();
                spelling += "{";
                token.lit.wideUnits.push_back('{');
                continue;
            }
            failHere("wide strings do not interpolate");
            continue;
        }
        if (c == '}') {
            // `}}` spells one `}`, and a lone `}` was always literal —
            // the same rule narrow strings follow.
            advance();
            spelling += "}";
            if (!atEnd() && peek() == '}') {
                advance();
                spelling += "}";
            }
            token.lit.wideUnits.push_back('}');
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
            if (e == 'x') {
                int hi = !atEnd() ? digit(peek()) : -1;
                int lo = (hi >= 0 && cursor_ + 1 < source_.size())
                             ? digit(source_[cursor_ + 1])
                             : -1;
                if (hi < 0 || lo < 0) {
                    failHere("unknown escape");
                    continue;
                }
                spelling += peek();
                spelling += source_[cursor_ + 1];
                advance();
                advance();
                token.lit.wideUnits.push_back(
                    static_cast<uint16_t>(hi * 16 + lo));
                continue;
            }
            if (e == 'u') {
                // `\uHHHH` names one raw unit: even a lone surrogate
                // passes through unpaired and unvalidated past four hex
                // digits, so UTF-16 round-trips exactly.
                uint32_t unit = 0;
                for (int i = 0; i < 4; ++i) {
                    if (atEnd()) {
                        unit = 0xFFFFFFFF;
                        break;
                    }
                    int d = digit(peek());
                    if (d < 0) {
                        unit = 0xFFFFFFFF;
                        break;
                    }
                    spelling += peek();
                    advance();
                    unit = unit * 16 + static_cast<uint32_t>(d);
                }
                if (unit > 0xFFFF) {
                    failHere("unknown escape");
                    continue;
                }
                token.lit.wideUnits.push_back(static_cast<uint16_t>(unit));
                continue;
            }
            int escaped = escapeValue(e);
            if (escaped < 0) {
                failHere("unknown escape");
                continue;
            }
            token.lit.wideUnits.push_back(static_cast<uint16_t>(escaped));
            continue;
        }
        if (static_cast<unsigned char>(c) < 0x80) {
            advance();
            spelling += c;
            token.lit.wideUnits.push_back(static_cast<uint16_t>(
                static_cast<unsigned char>(c)));
            continue;
        }
        unsigned char first = static_cast<unsigned char>(advance());
        if (!pushUtf8(first)) {
            failHere("invalid UTF-8 in wide string");
            continue;
        }
    }
    failHere("unterminated string literal");
    token.text = spelling;
    return token;
}

Token Lexer::wideChar() {
    Position start{file_, line_, column_};
    Token token;
    token.kind = TokenKind::WideCharLit;
    token.pos = start;
    advance();  // the u
    advance();  // the opening quote
    if (atEnd() || peek() == '\n') {
        token.kind = TokenKind::Error;
        fail(start, "unterminated character literal");
        return token;
    }
    uint32_t value = 0;
    bool have = false;
    char c = advance();
    if (c == '\\') {
        if (atEnd()) {
            token.kind = TokenKind::Error;
            fail(start, "unterminated character literal");
            return token;
        }
        int escaped = -1;
        if (peek() == 'x' && isHex(peekAt(1)) && isHex(peekAt(2))) {
            advance();
            int hi = advance();
            int lo = advance();
            hi = hi <= '9' ? hi - '0' : (hi <= 'F' ? hi - 'A' + 10 : hi - 'a' + 10);
            lo = lo <= '9' ? lo - '0' : (lo <= 'F' ? lo - 'A' + 10 : lo - 'a' + 10);
            escaped = hi * 16 + lo;
        } else if (peek() == 'u' && isHex(peekAt(1)) && isHex(peekAt(2)) &&
                   isHex(peekAt(3)) && isHex(peekAt(4))) {
            advance();
            uint32_t unit = 0;
            for (int i = 0; i < 4; ++i) {
                char h = advance();
                unit = unit * 16 +
                       static_cast<uint32_t>(h <= '9' ? h - '0'
                                             : (h <= 'F' ? h - 'A' + 10
                                                        : h - 'a' + 10));
            }
            escaped = static_cast<int>(unit);
        } else if (!atEnd() && peek() != 'u') {
            escaped = escapeValue(advance());
        }
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
        have = true;
    } else if (static_cast<unsigned char>(c) < 0x80) {
        value = static_cast<uint32_t>(static_cast<unsigned char>(c));
        have = true;
    } else {
        // A multibyte source character decodes like any wide unit, but the
        // literal holds exactly one: astral planes do not fit a u16.
        unsigned char first = static_cast<unsigned char>(c);
        uint32_t point = 0;
        int rest = (first & 0xE0) == 0xC0   ? 1
                   : (first & 0xF0) == 0xE0 ? 2
                   : (first & 0xF8) == 0xF0 ? 3
                                            : -1;
        bool valid = rest > 0;
        point = first & (rest == 1   ? 0x1F
                         : rest == 2 ? 0x0F
                                     : 0x07);
        for (int i = 0; valid && i < rest; ++i) {
            if (atEnd() || (peek() & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            point = (point << 6) |
                    (static_cast<unsigned char>(advance()) & 0x3F);
        }
        if (valid && (point > 0xFFFF || (rest == 1 && point < 0x80) ||
                      (rest == 2 && point < 0x800) ||
                      (point >= 0xD800 && point <= 0xDFFF))) {
            valid = false;
        }
        if (!valid) {
            token.kind = TokenKind::Error;
            fail(start, "invalid UTF-8 in wide string");
            return token;
        }
        value = point;
        have = true;
    }
    if (!have || !match('\'')) {
        token.kind = TokenKind::Error;
        fail(start, "a wide character holds exactly one unit");
        return token;
    }
    if (value > 0xFFFF) {
        token.kind = TokenKind::Error;
        fail(start, "a wide character holds exactly one unit");
        return token;
    }
    token.text = std::string("u'") +
                 (value < 128 ? std::string(1, static_cast<char>(value)) : "?") +
                 "'";
    token.lit.charValue = value;
    return token;
}

std::vector<Token> Lexer::braced(int depth) {    std::vector<Token> out;
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
