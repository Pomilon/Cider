#include "cider_lexer.h"
#include <iostream>
#include <map>

namespace Cider
{

    /**
     * Static helper to convert TokenType to string
     */
    std::string Token::toString(TokenType type)
    {
        static const std::map<TokenType, std::string> tokenTypeNames = {
            {TokenType::FUNCTION, "FUNCTION"}, {TokenType::FUNC, "FUNC"}, {TokenType::IF, "IF"}, {TokenType::ELSE, "ELSE"}, {TokenType::WHILE, "WHILE"}, {TokenType::FOR, "FOR"}, {TokenType::RETURN, "RETURN"}, {TokenType::TRUE, "TRUE"}, {TokenType::FALSE, "FALSE"}, {TokenType::NIL, "NIL"}, {TokenType::NIL_PTR, "NIL_PTR"}, {TokenType::IMPORT, "IMPORT"}, {TokenType::FROM, "FROM"}, {TokenType::EXPORT, "EXPORT"}, {TokenType::VAR, "VAR"}, {TokenType::CLASS, "CLASS"}, {TokenType::STRUCT, "STRUCT"},         {TokenType::ENUM, "ENUM"}, {TokenType::EXTERN, "EXTERN"}, {TokenType::EXTENDS, "EXTENDS"}, {TokenType::NEW, "NEW"}, {TokenType::THIS, "THIS"}, {TokenType::STRICT, "STRICT"},
            {TokenType::BREAK, "BREAK"}, {TokenType::CONTINUE, "CONTINUE"}, {TokenType::TRY, "TRY"}, {TokenType::CATCH, "CATCH"}, {TokenType::THROW, "THROW"}, {TokenType::SUPER, "SUPER"}, {TokenType::AS, "AS"}, {TokenType::SIZEOF, "SIZEOF"}, {TokenType::CONST, "CONST"}, {TokenType::TYPE, "TYPE"}, {TokenType::DEFER, "DEFER"}, {TokenType::ASM, "ASM"}, {TokenType::MATCH, "MATCH"}, {TokenType::DELETE, "DELETE"},             {TokenType::VIRTUAL, "VIRTUAL"}, {TokenType::OVERRIDE, "OVERRIDE"}, {TokenType::VOLATILE, "VOLATILE"},

            {TokenType::INT, "INT"},
            {TokenType::FLOAT, "FLOAT"},
            {TokenType::BOOL_TYPE, "BOOL_TYPE"},
            {TokenType::STRING_TYPE, "STRING_TYPE"},
            {TokenType::VOID, "VOID"},
            {TokenType::U8, "U8"}, {TokenType::I8, "I8"},
            {TokenType::U16, "U16"}, {TokenType::I16, "I16"},
            {TokenType::U32, "U32"}, {TokenType::I32, "I32"},
            {TokenType::U64, "U64"}, {TokenType::I64, "I64"},
            {TokenType::CHAR_TYPE, "CHAR_TYPE"},
            {TokenType::BYTE_TYPE, "BYTE_TYPE"},

            {TokenType::PLUS, "PLUS"},
            {TokenType::MINUS, "MINUS"},
            {TokenType::MULTIPLY, "MULTIPLY"},
            {TokenType::DIVIDE, "DIVIDE"},
            {TokenType::MODULO, "MODULO"},
            {TokenType::ASSIGN, "ASSIGN"},
            {TokenType::PLUS_ASSIGN, "PLUS_ASSIGN"},
            {TokenType::MINUS_ASSIGN, "MINUS_ASSIGN"},
            {TokenType::MULT_ASSIGN, "MULT_ASSIGN"},
            {TokenType::DIV_ASSIGN, "DIV_ASSIGN"},
            {TokenType::MOD_ASSIGN, "MOD_ASSIGN"},
            {TokenType::EQ, "EQ"},
            {TokenType::NE, "NE"},
            {TokenType::LT, "LT"},
            {TokenType::LE, "LE"},
            {TokenType::GT, "GT"},
            {TokenType::GE, "GE"},
            {TokenType::AND, "AND"},
            {TokenType::OR, "OR"},
            {TokenType::NOT, "NOT"},
            {TokenType::QUESTION, "QUESTION"},
            {TokenType::CARET, "CARET"},
            {TokenType::AMPERSAND, "AMPERSAND"},
            {TokenType::ARROW, "ARROW"}, {TokenType::FAT_ARROW, "FAT_ARROW"}, {TokenType::BIT_OR, "BIT_OR"},
            {TokenType::BIT_XOR, "BIT_XOR"},
            {TokenType::BIT_NOT, "BIT_NOT"},
            {TokenType::LSHIFT, "LSHIFT"},
            {TokenType::RSHIFT, "RSHIFT"},

            {TokenType::LPAREN, "LPAREN"},
            {TokenType::RPAREN, "RPAREN"},
            {TokenType::LBRACE, "LBRACE"},
            {TokenType::RBRACE, "RBRACE"},
            {TokenType::LBRACKET, "LBRACKET"},
            {TokenType::RBRACKET, "RBRACKET"},
            {TokenType::COMMA, "COMMA"},
            {TokenType::DOT, "DOT"},
            {TokenType::COLON, "COLON"},
            {TokenType::SEMICOLON, "SEMICOLON"},

            {TokenType::IDENTIFIER, "IDENTIFIER"},
            {TokenType::NUMBER, "NUMBER"},
            {TokenType::STRING, "STRING"},
            {TokenType::CHAR_LITERAL, "CHAR_LITERAL"},

            {TokenType::END_OF_FILE, "END_OF_FILE"},
            {TokenType::UNKNOWN, "UNKNOWN"}};

        auto it = tokenTypeNames.find(type);
        if (it != tokenTypeNames.end())
        {
            return it->second;
        }
        return "UNKNOWN";
    }

    /**
     * Member function to get a detailed string representation of the token
     */
    std::string Token::debugString() const
    {
        return "Type: " + Token::toString(type) + ", Value: '" + value + "', Line: " + std::to_string(line) + ", Col: " + std::to_string(column);
    }

    Lexer::Lexer(const std::string &source, ErrorReporter &reporter)
        : source(source), reporter_(reporter), currentPos(0), currentLine(1), currentCol(1) {}

    char Lexer::peek()
    {
        if (currentPos >= source.length())
        {
            return '\0';
        }
        return source[currentPos];
    }

    char Lexer::advance()
    {
        if (currentPos >= source.length())
        {
            return '\0';
        }
        char c = source[currentPos];
        currentPos++;
        currentCol++;
        return c;
    }

    void Lexer::skipWhitespace()
    {
        for (;;)
        {
            while (currentPos < source.length() && (isspace(peek()) || peek() == '\r'))
            {
                if (peek() == '\n')
                {
                    currentLine++;
                    currentCol = 1;
                }
                advance();
            }

            if (peek() == '/' && currentPos + 1 < source.length() && source[currentPos + 1] == '/')
            {
                while (currentPos < source.length() && peek() != '\n' && peek() != '\r')
                {
                    advance();
                }
                continue;
            }

            if (peek() == '/' && currentPos + 1 < source.length() && source[currentPos + 1] == '*')
            {
                advance();
                advance();
                bool commentEnded = false;
                while (currentPos < source.length())
                {
                    if (peek() == '*' && currentPos + 1 < source.length() && source[currentPos + 1] == '/')
                    {
                        advance();
                        advance();
                        commentEnded = true;
                        break;
                    }
                    if (peek() == '\n')
                    {
                        currentLine++;
                        currentCol = 1;
                    }
                    advance();
                }
                if (!commentEnded)
                {
                    break;
                }
                continue;
            }

            break;
        }
    }

    Token Lexer::readIdentifier()
    {
        size_t start = currentPos;
        while (currentPos < source.length() && (isalnum(peek()) || peek() == '_'))
        {
            advance();
        }
        std::string value = source.substr(start, currentPos - start);

        static const std::map<std::string, TokenType> keywords = {
            {"function", TokenType::FUNCTION}, {"func", TokenType::FUNC}, {"if", TokenType::IF}, {"else", TokenType::ELSE}, {"while", TokenType::WHILE}, {"for", TokenType::FOR}, {"return", TokenType::RETURN}, {"true", TokenType::TRUE}, {"false", TokenType::FALSE}, {"nil", TokenType::NIL}, {"nilPtr", TokenType::NIL_PTR}, {"import", TokenType::IMPORT}, {"from", TokenType::FROM}, {"export", TokenType::EXPORT}, {"var", TokenType::VAR}, {"class", TokenType::CLASS},         {"struct", TokenType::STRUCT}, {"enum", TokenType::ENUM}, {"extern", TokenType::EXTERN}, {"extends", TokenType::EXTENDS}, {"new", TokenType::NEW}, {"delete", TokenType::DELETE}, {"this", TokenType::THIS},
            {"break", TokenType::BREAK}, {"continue", TokenType::CONTINUE}, {"try", TokenType::TRY}, {"catch", TokenType::CATCH}, {"throw", TokenType::THROW}, {"super", TokenType::SUPER},
            {"async", TokenType::ASYNC}, {"await", TokenType::AWAIT}, {"as", TokenType::AS}, {"sizeof", TokenType::SIZEOF}, {"const", TokenType::CONST}, {"type", TokenType::TYPE}, {"defer", TokenType::DEFER}, {"asm", TokenType::ASM}, {"match", TokenType::MATCH},             {"virtual", TokenType::VIRTUAL}, {"override", TokenType::OVERRIDE}, {"volatile", TokenType::VOLATILE},
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
        if (it != keywords.end())
        {
            type = it->second;
        }
        return makeToken(type, value);
    }

    Token Lexer::readNumber()
    {
        size_t start = currentPos;
        while (currentPos < source.length() && isdigit(peek()))
        {
            advance();
        }
        if (peek() == '.' && (currentPos + 1 < source.length() && isdigit(source[currentPos + 1])))
        {
            advance();
            while (currentPos < source.length() && isdigit(peek()))
            {
                advance();
            }
        }
        if (currentPos < source.length() && (peek() == 'e' || peek() == 'E'))
        {
            advance();
            if (currentPos < source.length() && (peek() == '+' || peek() == '-'))
            {
                advance();
            }
            if (currentPos < source.length() && isdigit(peek()))
            {
                while (currentPos < source.length() && isdigit(peek()))
                {
                    advance();
                }
            }
        }
        std::string value = source.substr(start, currentPos - start);
        return makeToken(TokenType::NUMBER, value);
    }

    Token Lexer::readString()
    {
        size_t start = currentPos;
        std::string raw_value;

        while (peek() != '"' && peek() != '\0')
        {
            if (peek() == '\n')
            {
                reporter_.error("Unterminated string literal (newline encountered)", currentLine, currentCol);
                return makeToken(TokenType::UNKNOWN, "");
            }

            if (peek() == '\\')
            {
                advance();
                char escapedChar = peek();
                switch (escapedChar)
                {
                case '"':
                    raw_value += '"';
                    break;
                case '\\':
                    raw_value += '\\';
                    break;
                case 'n':
                    raw_value += '\n';
                    break;
                case 't':
                    raw_value += '\t';
                    break;
                case 'r':
                    raw_value += '\r';
                    break;
                default:
                    raw_value += '\\';
                    raw_value += escapedChar;
                    break;
                }
                advance();
            }
            else
            {
                raw_value += peek();
                advance();
            }
        }

        if (peek() == '\0')
        {
            reporter_.error("Unterminated string literal (EOF)", currentLine, currentCol);
            return makeToken(TokenType::UNKNOWN, "");
        }

        advance();
        return makeToken(TokenType::STRING, raw_value);
    }

    Token Lexer::readCharLiteral()
    {
        advance(); // Skip opening quote
        if (peek() == '\0')
        {
            reporter_.error("Unterminated character literal (EOF)", currentLine, currentCol);
            return makeToken(TokenType::UNKNOWN, "");
        }

        std::string raw_value;
        if (peek() == '\\')
        {
            advance();
            char escapedChar = peek();
            switch (escapedChar)
            {
            case 'n':
                raw_value = "\n";
                break;
            case 't':
                raw_value = "\t";
                break;
            case 'r':
                raw_value = "\r";
                break;
            case '\\':
                raw_value = "\\";
                break;
            case '\'':
                raw_value = "'";
                break;
            default:
                raw_value = escapedChar;
                break;
            }
        }
        else
        {
            raw_value = peek();
        }
        advance();

        if (peek() != '\'')
        {
            reporter_.error("Unterminated character literal, expected '", currentLine, currentCol);
            return makeToken(TokenType::UNKNOWN, "");
        }
        advance(); // Skip closing quote

        return makeToken(TokenType::CHAR_LITERAL, raw_value);
    }

    Token Lexer::makeToken(TokenType type, const std::string &value)
    {
        return {type, value, currentLine, currentCol - static_cast<int>(value.length())};
    }

    Token Lexer::getNextToken()
    {
        skipWhitespace();

        if (currentPos >= source.length())
        {
            return makeToken(TokenType::END_OF_FILE, "");
        }

        char c = peek();
        Token token;
        if (isalpha(c) || c == '_')
        {
            token = readIdentifier();
        }
        else if (isdigit(c))
        {
            token = readNumber();
        }
        else if (c == '"')
        {
            advance();
            token = readString();
        }
        else if (c == '\'')
        {
            token = readCharLiteral();
        }
        else
            switch (c)
            {
            case '+':
                advance();
                if (peek() == '+') {
                    advance();
                    token = makeToken(TokenType::INCREMENT, "++");
                } else if (peek() == '=') {
                    advance();
                    token = makeToken(TokenType::PLUS_ASSIGN, "+=");
                } else {
                    token = makeToken(TokenType::PLUS, "+");
                }
                break;
            case '-':
                advance();
                if (peek() == '-') {
                    advance();
                    token = makeToken(TokenType::DECREMENT, "--");
                } else if (peek() == '=') {
                    advance();
                    token = makeToken(TokenType::MINUS_ASSIGN, "-=");
                } else if (peek() == '>') {
                    advance();
                    token = makeToken(TokenType::ARROW, "->");
                } else {
                    token = makeToken(TokenType::MINUS, "-");
                }
                break;
            case '&':
                advance();
                if (peek() == '&') {
                    advance();
                    token = makeToken(TokenType::AND, "&&");
                } else {
                    token = makeToken(TokenType::AMPERSAND, "&");
                }
                break;
            case '|':
                advance();
                if (peek() == '|') {
                    advance();
                    token = makeToken(TokenType::OR, "||");
                } else {
                    token = makeToken(TokenType::BIT_OR, "|");
                }
                break;            case '~':
                advance();
                token = makeToken(TokenType::BIT_NOT, "~");
                break;
            case '^':
                advance();
                token = makeToken(TokenType::BIT_XOR, "^");
                break;
            case '%':
                advance();
                if (peek() == '=') {
                    advance();
                    token = makeToken(TokenType::MOD_ASSIGN, "%=");
                } else {
                    token = makeToken(TokenType::MODULO, "%");
                }
                break;
            case '*':
                advance();
                if (peek() == '=') {
                    advance();
                    token = makeToken(TokenType::MULT_ASSIGN, "*=");
                } else {
                    token = makeToken(TokenType::MULTIPLY, "*");
                }
                break;
            case '=':
            {
                advance();
                if (peek() == '=')
                {
                    advance();
                    token = makeToken(TokenType::EQ, "==");
                }
                else if (peek() == '>')
                {
                    advance();
                    token = makeToken(TokenType::FAT_ARROW, "=>");
                }
                else
                {
                    token = makeToken(TokenType::ASSIGN, "=");
                }
                break;
            }
            case '!':
            {
                advance();
                if (peek() == '=')
                {
                    advance();
                    token = makeToken(TokenType::NE, "!=");
                }
                else
                {
                    token = makeToken(TokenType::NOT, "!");
                }
                break;
            }
            case '<':
            {
                advance();
                if (peek() == '=')
                {
                    advance();
                    token = makeToken(TokenType::LE, "<=");
                }
                else if (peek() == '<')
                {
                    advance();
                    token = makeToken(TokenType::LSHIFT, "<<");
                }
                else
                {
                    token = makeToken(TokenType::LT, "<");
                }
                break;
            }
            case '>':
            {
                advance();
                if (peek() == '=')
                {
                    advance();
                    token = makeToken(TokenType::GE, ">=");
                }
                else if (peek() == '>')
                {
                    advance();
                    token = makeToken(TokenType::RSHIFT, ">>");
                }
                else
                {
                    token = makeToken(TokenType::GT, ">");
                }
                break;
            }
            case '(':
                advance();
                token = makeToken(TokenType::LPAREN, "(");
                break;
            case ')':
                advance();
                token = makeToken(TokenType::RPAREN, ")");
                break;
            case '{':
                advance();
                token = makeToken(TokenType::LBRACE, "{");
                break;
            case '}':
                advance();
                token = makeToken(TokenType::RBRACE, "}");
                break;
            case '[':
                advance();
                token = makeToken(TokenType::LBRACKET, "[");
                break;
            case ']':
                advance();
                token = makeToken(TokenType::RBRACKET, "]");
                break;
            case ',':
                advance();
                token = makeToken(TokenType::COMMA, ",");
                break;
            case '.':
                advance();
                token = makeToken(TokenType::DOT, ".");
                break;
            case ':':
                advance();
                token = makeToken(TokenType::COLON, ":");
                break;
            case ';':
                advance();
                token = makeToken(TokenType::SEMICOLON, ";");
                break;
            case '?':
                advance();
                token = makeToken(TokenType::QUESTION, "?");
                break;
            case '/':
            {
                advance();
                if (peek() == '/' || peek() == '*')
                {
                    token = makeToken(TokenType::DIVIDE, "/");
                }
                else if (peek() == '=')
                {
                    advance();
                    token = makeToken(TokenType::DIV_ASSIGN, "/=");
                }
                else
                {
                    token = makeToken(TokenType::DIVIDE, "/");
                }
                break;
            }
            default:
                std::string unknownChar(1, advance());
                token = makeToken(TokenType::UNKNOWN, unknownChar);
                break;
            }
        return token;
    }

} // namespace Cider
