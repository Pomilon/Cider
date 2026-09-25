#include "lexer.h"

#include <cctype>
#include <map>

namespace cider {

std::string Token::toString(TokenType type) {
    static const std::map<TokenType, std::string> tokenTypeNames = {
        {TokenType::FUNCTION, "FUNCTION"}, {TokenType::FUNC, "FUNC"}, {TokenType::IF, "IF"}, {TokenType::ELSE, "ELSE"}, {TokenType::WHILE, "WHILE"}, {TokenType::FOR, "FOR"}, {TokenType::RETURN, "RETURN"}, {TokenType::TRUE, "TRUE"}, {TokenType::FALSE, "FALSE"}, {TokenType::NIL, "NIL"}, {TokenType::NIL_PTR, "NIL_PTR"}, {TokenType::IMPORT, "IMPORT"}, {TokenType::FROM, "FROM"}, {TokenType::EXPORT, "EXPORT"}, {TokenType::VAR, "VAR"}, {TokenType::CLASS, "CLASS"},
        {TokenType::STRUCT, "STRUCT"}, {TokenType::UNION, "UNION"}, {TokenType::ENUM, "ENUM"}, {TokenType::EXTERN, "EXTERN"}, {TokenType::EXTENDS, "EXTENDS"}, {TokenType::NEW, "NEW"}, {TokenType::THIS, "THIS"}, {TokenType::STRICT, "STRICT"},
        {TokenType::BREAK, "BREAK"}, {TokenType::CONTINUE, "CONTINUE"}, {TokenType::TRY, "TRY"}, {TokenType::CATCH, "CATCH"}, {TokenType::THROW, "THROW"}, {TokenType::SUPER, "SUPER"}, {TokenType::AS, "AS"}, {TokenType::SIZEOF, "SIZEOF"}, {TokenType::CONST, "CONST"}, {TokenType::TYPE, "TYPE"}, {TokenType::DEFER, "DEFER"}, {TokenType::ASM, "ASM"}, {TokenType::MATCH, "MATCH"}, {TokenType::DELETE, "DELETE"},
        {TokenType::VIRTUAL, "VIRTUAL"}, {TokenType::OVERRIDE, "OVERRIDE"}, {TokenType::VOLATILE, "VOLATILE"}, {TokenType::INTERFACE, "INTERFACE"},

        {TokenType::INT, "INT"}, {TokenType::FLOAT, "FLOAT"}, {TokenType::BOOL_TYPE, "BOOL_TYPE"}, {TokenType::STRING_TYPE, "STRING_TYPE"}, {TokenType::VOID, "VOID"},
        {TokenType::U8, "U8"}, {TokenType::I8, "I8"}, {TokenType::U16, "U16"}, {TokenType::I16, "I16"}, {TokenType::U32, "U32"}, {TokenType::I32, "I32"}, {TokenType::U64, "U64"}, {TokenType::I64, "I64"},
        {TokenType::CHAR_TYPE, "CHAR_TYPE"}, {TokenType::BYTE_TYPE, "BYTE_TYPE"},

        {TokenType::PLUS, "PLUS"}, {TokenType::MINUS, "MINUS"}, {TokenType::MULTIPLY, "MULTIPLY"}, {TokenType::DIVIDE, "DIVIDE"}, {TokenType::MODULO, "MODULO"}, {TokenType::ASSIGN, "ASSIGN"},
        {TokenType::PLUS_ASSIGN, "PLUS_ASSIGN"}, {TokenType::MINUS_ASSIGN, "MINUS_ASSIGN"}, {TokenType::MULT_ASSIGN, "MULT_ASSIGN"}, {TokenType::DIV_ASSIGN, "DIV_ASSIGN"}, {TokenType::MOD_ASSIGN, "MOD_ASSIGN"},
        {TokenType::EQ, "EQ"}, {TokenType::NE, "NE"}, {TokenType::LT, "LT"}, {TokenType::LE, "LE"}, {TokenType::GT, "GT"}, {TokenType::GE, "GE"}, {TokenType::AND, "AND"}, {TokenType::OR, "OR"}, {TokenType::NOT, "NOT"}, {TokenType::QUESTION, "QUESTION"}, {TokenType::CARET, "CARET"}, {TokenType::AMPERSAND, "AMPERSAND"},
        {TokenType::ARROW, "ARROW"}, {TokenType::FAT_ARROW, "FAT_ARROW"}, {TokenType::BIT_OR, "BIT_OR"}, {TokenType::BIT_XOR, "BIT_XOR"}, {TokenType::BIT_NOT, "BIT_NOT"}, {TokenType::TILDE, "TILDE"}, {TokenType::LSHIFT, "LSHIFT"}, {TokenType::RSHIFT, "RSHIFT"},

        {TokenType::LPAREN, "LPAREN"}, {TokenType::RPAREN, "RPAREN"}, {TokenType::LBRACE, "LBRACE"}, {TokenType::RBRACE, "RBRACE"}, {TokenType::LBRACKET, "LBRACKET"}, {TokenType::RBRACKET, "RBRACKET"},
        {TokenType::COMMA, "COMMA"}, {TokenType::DOT, "DOT"}, {TokenType::COLON, "COLON"}, {TokenType::SEMICOLON, "SEMICOLON"},

        {TokenType::IDENTIFIER, "IDENTIFIER"}, {TokenType::NUMBER, "NUMBER"}, {TokenType::STRING, "STRING"}, {TokenType::CHAR_LITERAL, "CHAR_LITERAL"},
        {TokenType::WIDE_STRING, "WIDE_STRING"}, {TokenType::WIDE_CHAR_LITERAL, "WIDE_CHAR_LITERAL"},

        {TokenType::END_OF_FILE, "END_OF_FILE"}, {TokenType::UNKNOWN, "UNKNOWN"}};

    auto it = tokenTypeNames.find(type);
    if (it != tokenTypeNames.end()) {
        return it->second;
    }
    return "UNKNOWN";
}

std::string Token::debugString() const {
    return "Type: " + Token::toString(type) + ", Value: '" + value + "', Line: " + std::to_string(line) + ", Col: " + std::to_string(column);
}

Lexer::Lexer(const std::string& source, ErrorReporter& reporter)
    : source(source), reporter_(reporter), currentPos(0), currentLine(1), currentCol(1) {}

char Lexer::peek() {
    if (currentPos >= source.length()) {
        return '\0';
    }
    return source[currentPos];
}

char Lexer::advance() {
    if (currentPos >= source.length()) {
        return '\0';
    }
    char c = source[currentPos];
    currentPos++;
    if (c == '\n') {
        currentLine++;
        currentCol = 1;
    } else {
        currentCol++;
    }
    return c;
}

void Lexer::skipWhitespace() {
    for (;;) {
        while (currentPos < source.length() && (std::isspace(static_cast<unsigned char>(peek())) || peek() == '\r')) {
            advance();
        }

        if (peek() == '/' && currentPos + 1 < source.length() && source[currentPos + 1] == '/') {
            while (currentPos < source.length() && peek() != '\n' && peek() != '\r') {
                advance();
            }
            continue;
        }

        if (peek() == '/' && currentPos + 1 < source.length() && source[currentPos + 1] == '*') {
            advance();
            advance();
            bool commentEnded = false;
            while (currentPos < source.length()) {
                if (peek() == '*' && currentPos + 1 < source.length() && source[currentPos + 1] == '/') {
                    advance();
                    advance();
                    commentEnded = true;
                    break;
                }
                advance();
            }
            if (!commentEnded) {
                break;
            }
            continue;
        }

        break;
    }
}

Token Lexer::readIdentifier() {
    size_t start = currentPos;
    while (currentPos < source.length() && (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_')) {
        advance();
    }
    std::string value = source.substr(start, currentPos - start);

    static const std::map<std::string, TokenType> keywords = {
        {"function", TokenType::FUNCTION}, {"func", TokenType::FUNC}, {"if", TokenType::IF}, {"else", TokenType::ELSE}, {"while", TokenType::WHILE}, {"for", TokenType::FOR}, {"return", TokenType::RETURN}, {"true", TokenType::TRUE}, {"false", TokenType::FALSE}, {"nil", TokenType::NIL}, {"nilPtr", TokenType::NIL_PTR}, {"import", TokenType::IMPORT}, {"from", TokenType::FROM}, {"export", TokenType::EXPORT}, {"var", TokenType::VAR}, {"class", TokenType::CLASS},
        {"struct", TokenType::STRUCT}, {"union", TokenType::UNION}, {"enum", TokenType::ENUM}, {"extern", TokenType::EXTERN}, {"extends", TokenType::EXTENDS}, {"new", TokenType::NEW}, {"delete", TokenType::DELETE}, {"this", TokenType::THIS},
        {"break", TokenType::BREAK}, {"continue", TokenType::CONTINUE}, {"try", TokenType::TRY}, {"catch", TokenType::CATCH}, {"throw", TokenType::THROW}, {"super", TokenType::SUPER},
        {"async", TokenType::ASYNC}, {"await", TokenType::AWAIT}, {"as", TokenType::AS}, {"sizeof", TokenType::SIZEOF}, {"const", TokenType::CONST}, {"type", TokenType::TYPE}, {"defer", TokenType::DEFER}, {"asm", TokenType::ASM}, {"match", TokenType::MATCH},
        {"virtual", TokenType::VIRTUAL}, {"override", TokenType::OVERRIDE}, {"volatile", TokenType::VOLATILE}, {"interface", TokenType::INTERFACE},
        {"strict", TokenType::STRICT}, {"and", TokenType::AND},
        {"or", TokenType::OR},
        {"not", TokenType::NOT},
        {"int", TokenType::INT},
        {"float", TokenType::FLOAT},
        {"bool", TokenType::BOOL_TYPE},
        {"string", TokenType::STRING_TYPE},
        {"void", TokenType::VOID},
        {"u8", TokenType::U8}, {"i8", TokenType::I8},
        {"u16", TokenType::U16}, {"i16", TokenType::I16},
        {"u32", TokenType::U32}, {"i32", TokenType::I32},
        {"u64", TokenType::U64}, {"i64", TokenType::I64},
        {"char", TokenType::CHAR_TYPE},
        {"byte", TokenType::BYTE_TYPE}};

    TokenType type = TokenType::IDENTIFIER;
    auto it = keywords.find(value);
    if (it != keywords.end()) {
        type = it->second;
    }
    return makeToken(type, value);
}

Token Lexer::readNumber() {
    size_t start = currentPos;
    // Hex (0x...), binary (0b...), octal (0o...) prefixes. Underscores are
    // allowed as visual separators (0xFF_FF, 1_000_000) and preserved in the
    // token text; the parser strips them during conversion.
    if (peek() == '0' && currentPos + 1 < source.length()) {
        char nxt = source[currentPos + 1];
        if (nxt == 'x' || nxt == 'X') {
            advance();
            advance();
            while (currentPos < source.length() &&
                   (std::isxdigit(static_cast<unsigned char>(peek())) || peek() == '_')) {
                advance();
            }
            std::string value = source.substr(start, currentPos - start);
            return makeToken(TokenType::NUMBER, value);
        }
        if (nxt == 'b' || nxt == 'B') {
            advance();
            advance();
            while (currentPos < source.length() &&
                   (peek() == '0' || peek() == '1' || peek() == '_')) {
                advance();
            }
            std::string value = source.substr(start, currentPos - start);
            return makeToken(TokenType::NUMBER, value);
        }
        if (nxt == 'o' || nxt == 'O') {
            advance();
            advance();
            while (currentPos < source.length() &&
                   ((peek() >= '0' && peek() <= '7') || peek() == '_')) {
                advance();
            }
            std::string value = source.substr(start, currentPos - start);
            return makeToken(TokenType::NUMBER, value);
        }
    }
    while (currentPos < source.length() &&
           (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '_')) {
        advance();
    }
    if (peek() == '.' && (currentPos + 1 < source.length() && std::isdigit(static_cast<unsigned char>(source[currentPos + 1])))) {
        advance();
        while (currentPos < source.length() &&
               (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '_')) {
            advance();
        }
    }
    if (currentPos < source.length() && (peek() == 'e' || peek() == 'E')) {
        size_t save = currentPos;
        advance();
        if (currentPos < source.length() && (peek() == '+' || peek() == '-')) {
            advance();
        }
        bool anyDigit = false;
        size_t digitsStart = currentPos;
        while (currentPos < source.length() &&
               (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '_')) {
            if (std::isdigit(static_cast<unsigned char>(peek()))) anyDigit = true;
            advance();
        }
        if (!anyDigit) {
            // Not a valid exponent (e.g. hex-adjacent ident); rewind to before 'e'.
            currentPos = save;
        } else {
            (void)digitsStart;
        }
    }
    std::string value = source.substr(start, currentPos - start);
    return makeToken(TokenType::NUMBER, value);
}

Token Lexer::readString() {
    std::string raw_value;

    while (peek() != '"' && peek() != '\0') {
        if (peek() == '\n') {
            reporter_.error("Unterminated string literal (newline encountered)", currentLine, currentCol);
            return makeToken(TokenType::UNKNOWN, "");
        }

        if (peek() == '\\') {
            advance();
            char escapedChar = peek();
            switch (escapedChar) {
            case '"': raw_value += '"'; break;
            case '\\': raw_value += '\\'; break;
            case 'n': raw_value += '\n'; break;
            case 't': raw_value += '\t'; break;
            case 'r': raw_value += '\r'; break;
            case 'b': raw_value += '\b'; break;
            case '0': raw_value += '\0'; break;
            case 'x': {
                // CID-030: \xNN hex escape (one or two hex digits).
                advance(); // consume 'x'
                int value = 0;
                int digits = 0;
                while (digits < 2) {
                    char h = peek();
                    int d = -1;
                    if (h >= '0' && h <= '9') d = h - '0';
                    else if (h >= 'a' && h <= 'f') d = h - 'a' + 10;
                    else if (h >= 'A' && h <= 'F') d = h - 'A' + 10;
                    if (d < 0) break;
                    value = value * 16 + d;
                    digits++;
                    advance();
                }
                if (digits == 0) {
                    reporter_.error("\\x escape requires at least one hex digit", currentLine, currentCol);
                    raw_value += 'x';
                } else {
                    raw_value += static_cast<char>(value);
                }
                continue; // digits (and 'x') already consumed
            }
            default:
                raw_value += '\\';
                raw_value += escapedChar;
                break;
            }
            advance();
        } else {
            raw_value += peek();
            advance();
        }
    }

    if (peek() == '\0') {
        reporter_.error("Unterminated string literal (EOF)", currentLine, currentCol);
        return makeToken(TokenType::UNKNOWN, "");
    }

    advance();
    return makeToken(TokenType::STRING, raw_value);
}

// CID-005: `u"..."` — UTF-16LE code units, including a terminating NUL
// unit, stored as raw little-endian bytes in the token value. The caller
// has already consumed the opening quote.
Token Lexer::readWideString() {
    std::u16string units;

    while (peek() != '"' && peek() != '\0') {
        if (peek() == '\n') {
            reporter_.error("Unterminated string literal (newline encountered)", currentLine, currentCol);
            return makeToken(TokenType::UNKNOWN, "");
        }

        if (peek() == '\\') {
            advance();
            char escapedChar = peek();
            switch (escapedChar) {
            case '"': units.push_back(u'"'); break;
            case '\\': units.push_back(u'\\'); break;
            case 'n': units.push_back(u'\n'); break;
            case 't': units.push_back(u'\t'); break;
            case 'r': units.push_back(u'\r'); break;
            case 'b': units.push_back(u'\b'); break;
            case '0': units.push_back(u'\0'); break;
            case 'x': {
                advance();  // consume 'x'
                int value = 0;
                int digits = 0;
                while (digits < 2 && std::isxdigit(static_cast<unsigned char>(peek()))) {
                    char h = peek();
                    value = value * 16 + (h <= '9' ? h - '0' : (std::tolower(static_cast<unsigned char>(h)) - 'a' + 10));
                    digits++;
                    advance();
                }
                if (digits == 0) {
                    reporter_.error("\\x escape requires at least one hex digit", currentLine, currentCol);
                }
                units.push_back((char16_t)value);
                continue;  // digits (and 'x') already consumed
            }
            case 'u': {
                advance();  // consume 'u'
                int value = 0;
                for (int d = 0; d < 4; ++d) {
                    char h = peek();
                    if (!std::isxdigit(static_cast<unsigned char>(h))) {
                        reporter_.error("\\u escape requires four hex digits", currentLine, currentCol);
                        break;
                    }
                    value = value * 16 + (h <= '9' ? h - '0' : (std::tolower(static_cast<unsigned char>(h)) - 'a' + 10));
                    advance();
                }
                units.push_back((char16_t)value);
                continue;  // digits already consumed
            }
            default:
                units.push_back(u'\\');
                units.push_back((char16_t)(unsigned char)escapedChar);
                break;
            }
            advance();
        } else {
            // Raw UTF-8 input: copy bytes as Latin-1 code units (ASCII is
            // the supported source case; `\uXXXX` covers the rest).
            units.push_back((char16_t)(unsigned char)peek());
            advance();
        }
    }

    if (peek() == '\0') {
        reporter_.error("Unterminated string literal (EOF)", currentLine, currentCol);
        return makeToken(TokenType::UNKNOWN, "");
    }

    advance();  // closing quote
    units.push_back(u'\0');
    std::string bytes;
    bytes.reserve(units.size() * 2);
    for (char16_t u : units) {
        bytes.push_back((char)(u & 0xFF));
        bytes.push_back((char)((u >> 8) & 0xFF));
    }
    return makeToken(TokenType::WIDE_STRING, bytes);
}

// CID-005: `u'X'` — exactly one UTF-16 code unit, two raw little-endian
// bytes in the token value, no NUL. The caller has already consumed the
// opening quote.
Token Lexer::readWideCharLiteral() {
    std::u16string units;

    if (peek() == '\0') {
        reporter_.error("Unterminated character literal (EOF)", currentLine, currentCol);
        return makeToken(TokenType::UNKNOWN, "");
    }

    if (peek() == '\\') {
        advance();
        char escapedChar = peek();
        switch (escapedChar) {
        case '\'': units.push_back(u'\''); break;
        case '"': units.push_back(u'"'); break;
        case '\\': units.push_back(u'\\'); break;
        case 'n': units.push_back(u'\n'); break;
        case 't': units.push_back(u'\t'); break;
        case 'r': units.push_back(u'\r'); break;
        case 'b': units.push_back(u'\b'); break;
        case '0': units.push_back(u'\0'); break;
        case 'x': {
            advance();  // consume 'x'
            int value = 0;
            int digits = 0;
            while (digits < 2 && std::isxdigit(static_cast<unsigned char>(peek()))) {
                char h = peek();
                value = value * 16 + (h <= '9' ? h - '0' : (std::tolower(static_cast<unsigned char>(h)) - 'a' + 10));
                digits++;
                advance();
            }
            if (digits == 0) {
                reporter_.error("\\x escape requires at least one hex digit", currentLine, currentCol);
            }
            units.push_back((char16_t)value);
            break;  // digits (and 'x') already consumed; no extra advance
        }
        case 'u': {
            advance();  // consume 'u'
            int value = 0;
            for (int d = 0; d < 4; ++d) {
                char h = peek();
                if (!std::isxdigit(static_cast<unsigned char>(h))) {
                    reporter_.error("\\u escape requires four hex digits", currentLine, currentCol);
                    break;
                }
                value = value * 16 + (h <= '9' ? h - '0' : (std::tolower(static_cast<unsigned char>(h)) - 'a' + 10));
                advance();
            }
            units.push_back((char16_t)value);
            break;  // digits already consumed; no extra advance
        }
        default:
            units.push_back((char16_t)(unsigned char)escapedChar);
            break;
        }
        if (escapedChar != 'x' && escapedChar != 'u') advance();
    } else {
        units.push_back((char16_t)(unsigned char)peek());
        advance();
    }

    if (peek() != '\'') {
        reporter_.error("Unterminated character literal, expected '", currentLine, currentCol);
        return makeToken(TokenType::UNKNOWN, "");
    }
    advance();  // closing quote

    std::string bytes;
    bytes.push_back((char)(units[0] & 0xFF));
    bytes.push_back((char)((units[0] >> 8) & 0xFF));
    return makeToken(TokenType::WIDE_CHAR_LITERAL, bytes);
}

Token Lexer::readCharLiteral() {
    advance();  // Skip opening quote
    if (peek() == '\0') {
        reporter_.error("Unterminated character literal (EOF)", currentLine, currentCol);
        return makeToken(TokenType::UNKNOWN, "");
    }

    std::string raw_value;
    bool escapeConsumedDigits = false;
    if (peek() == '\\') {
        advance();
        char escapedChar = peek();
        switch (escapedChar) {
        case 'n': raw_value = "\n"; break;
        case 't': raw_value = "\t"; break;
        case 'r': raw_value = "\r"; break;
        case 'b': raw_value = "\b"; break;
        case '0': raw_value = std::string("\0", 1); break;
        case '\\': raw_value = "\\"; break;
        case '\'': raw_value = "'"; break;
        case 'x': {
            // CID-030: \xNN hex escape (one or two hex digits).
            advance(); // consume 'x'
            int value = 0;
            int digits = 0;
            while (digits < 2) {
                char h = peek();
                int d = -1;
                if (h >= '0' && h <= '9') d = h - '0';
                else if (h >= 'a' && h <= 'f') d = h - 'a' + 10;
                else if (h >= 'A' && h <= 'F') d = h - 'A' + 10;
                if (d < 0) break;
                value = value * 16 + d;
                digits++;
                advance();
            }
            if (digits == 0) {
                reporter_.error("\\x escape requires at least one hex digit", currentLine, currentCol);
                raw_value = "x";
            } else {
                raw_value = std::string(1, static_cast<char>(value));
            }
            escapeConsumedDigits = true;
            break;
        }
        default: raw_value = escapedChar; break;
        }
    } else {
        raw_value = peek();
    }
    if (!escapeConsumedDigits) advance();

    if (peek() != '\'') {
        reporter_.error("Unterminated character literal, expected '", currentLine, currentCol);
        return makeToken(TokenType::UNKNOWN, "");
    }
    advance();  // Skip closing quote

    return makeToken(TokenType::CHAR_LITERAL, raw_value);
}

Token Lexer::makeToken(TokenType type, const std::string& value) {
    return {type, value, currentLine, currentCol - static_cast<int>(value.length())};
}

Token Lexer::getNextToken() {
    skipWhitespace();

    if (currentPos >= source.length()) {
        return makeToken(TokenType::END_OF_FILE, "");
    }

    char c = peek();
    Token token;
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
        token = readIdentifier();
        // CID-005: `u"..."` / `u'X'` are wide literals only when the
        // quote is immediately adjacent; `u` stays an identifier otherwise.
        if (token.type == TokenType::IDENTIFIER && token.value == "u") {
            if (peek() == '"') {
                advance();
                token = readWideString();
            } else if (peek() == '\'') {
                advance();
                token = readWideCharLiteral();
            }
        }
    } else if (std::isdigit(static_cast<unsigned char>(c))) {
        token = readNumber();
    } else if (c == '"') {
        advance();
        token = readString();
    } else if (c == '\'') {
        token = readCharLiteral();
    } else {
        switch (c) {
        case '+':
            advance();
            if (peek() == '+') { advance(); token = makeToken(TokenType::INCREMENT, "++"); }
            else if (peek() == '=') { advance(); token = makeToken(TokenType::PLUS_ASSIGN, "+="); }
            else { token = makeToken(TokenType::PLUS, "+"); }
            break;
        case '-':
            advance();
            if (peek() == '-') { advance(); token = makeToken(TokenType::DECREMENT, "--"); }
            else if (peek() == '=') { advance(); token = makeToken(TokenType::MINUS_ASSIGN, "-="); }
            else if (peek() == '>') { advance(); token = makeToken(TokenType::ARROW, "->"); }
            else { token = makeToken(TokenType::MINUS, "-"); }
            break;
        case '&':
            advance();
            if (peek() == '=') { advance(); token = makeToken(TokenType::AMPERSAND_ASSIGN, "&="); }
            else if (peek() == '&') { advance(); token = makeToken(TokenType::AND, "&&"); }
            else { token = makeToken(TokenType::AMPERSAND, "&"); }
            break;
        case '|':
            advance();
            if (peek() == '=') { advance(); token = makeToken(TokenType::BIT_OR_ASSIGN, "|="); }
            else if (peek() == '|') { advance(); token = makeToken(TokenType::OR, "||"); }
            else { token = makeToken(TokenType::BIT_OR, "|"); }
            break;
        case '~':
            advance();
            token = makeToken(TokenType::TILDE, "~");
            break;
        case '^':
            advance();
            if (peek() == '=') { advance(); token = makeToken(TokenType::BIT_XOR_ASSIGN, "^="); }
            else { token = makeToken(TokenType::BIT_XOR, "^"); }
            break;
        case '%':
            advance();
            if (peek() == '=') { advance(); token = makeToken(TokenType::MOD_ASSIGN, "%="); }
            else { token = makeToken(TokenType::MODULO, "%"); }
            break;
        case '*':
            advance();
            if (peek() == '=') { advance(); token = makeToken(TokenType::MULT_ASSIGN, "*="); }
            else { token = makeToken(TokenType::MULTIPLY, "*"); }
            break;
        case '=':
            advance();
            if (peek() == '=') { advance(); token = makeToken(TokenType::EQ, "=="); }
            else if (peek() == '>') { advance(); token = makeToken(TokenType::FAT_ARROW, "=>"); }
            else { token = makeToken(TokenType::ASSIGN, "="); }
            break;
        case '!':
            advance();
            if (peek() == '=') { advance(); token = makeToken(TokenType::NE, "!="); }
            else { token = makeToken(TokenType::NOT, "!"); }
            break;
        case '<':
            advance();
            if (peek() == '=') { advance(); token = makeToken(TokenType::LE, "<="); }
            else if (peek() == '<') { advance(); token = makeToken(TokenType::LSHIFT, "<<"); }
            else { token = makeToken(TokenType::LT, "<"); }
            break;
        case '>':
            advance();
            if (peek() == '=') { advance(); token = makeToken(TokenType::GE, ">="); }
            else if (peek() == '>') { advance(); token = makeToken(TokenType::RSHIFT, ">>"); }
            else { token = makeToken(TokenType::GT, ">"); }
            break;
        case '(': advance(); token = makeToken(TokenType::LPAREN, "("); break;
        case ')': advance(); token = makeToken(TokenType::RPAREN, ")"); break;
        case '{': advance(); token = makeToken(TokenType::LBRACE, "{"); break;
        case '}': advance(); token = makeToken(TokenType::RBRACE, "}"); break;
        case '[': advance(); token = makeToken(TokenType::LBRACKET, "["); break;
        case ']': advance(); token = makeToken(TokenType::RBRACKET, "]"); break;
        case ',': advance(); token = makeToken(TokenType::COMMA, ","); break;
        case '.': advance(); token = makeToken(TokenType::DOT, "."); break;
        case ':': advance(); token = makeToken(TokenType::COLON, ":"); break;
        case ';': advance(); token = makeToken(TokenType::SEMICOLON, ";"); break;
        case '?': advance(); token = makeToken(TokenType::QUESTION, "?"); break;
        case '/':
            advance();
            if (peek() == '=') { advance(); token = makeToken(TokenType::DIV_ASSIGN, "/="); }
            else { token = makeToken(TokenType::DIVIDE, "/"); }
            break;
        default:
            std::string unknownChar(1, advance());
            token = makeToken(TokenType::UNKNOWN, unknownChar);
            break;
        }
    }
    return token;
}

}  // namespace cider
