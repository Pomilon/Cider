#ifndef CIDER_LEXER_H
#define CIDER_LEXER_H

#include <string>
#include <vector>
#include <map>
#include "cider_errors.h"

namespace Cider
{

    enum class TokenType
    {
        // Keywords
        FUNCTION,
        FUNC,    // Added 'func'
        IF,
        ELSE,
        WHILE,
        FOR,
        RETURN,
        TRUE,
        FALSE,
        NIL,
        NIL_PTR, // Added 'nilPtr'
        IMPORT,
        FROM,
        EXPORT,
        VAR,
        CLASS,
        STRUCT,  // Added 'struct'
        ENUM,    // Added 'enum'
        EXTERN,  // Added 'extern'
        EXTENDS,
        NEW,
        DELETE,
        THIS,
        STRICT,
        BREAK,
        CONTINUE,
        TRY,
        CATCH,
        THROW,
        SUPER,
        ASYNC,
        AWAIT,
        AS,
        SIZEOF,
        CONST,
        TYPE,
        DEFER,
        ASM,
        MATCH,
        VIRTUAL,
        OVERRIDE,
        VOLATILE,

        // Types
        INT,
        FLOAT,
        BOOL_TYPE,
        STRING_TYPE,
        VOID,
        U8, I8, U16, I16, U32, I32, U64, I64,
        CHAR_TYPE,
        BYTE_TYPE,

        // Operators
        PLUS,
        MINUS,
        INCREMENT,  // ++
        DECREMENT,  // --
        MULTIPLY,
        DIVIDE,
        MODULO,
        ASSIGN,
        PLUS_ASSIGN,
        MINUS_ASSIGN,
        MULT_ASSIGN,
        DIV_ASSIGN,
        MOD_ASSIGN,
        AMPERSAND_ASSIGN,
        BIT_OR_ASSIGN,
        BIT_XOR_ASSIGN,
        EQ,
        NE,
        LT,
        LE,
        GT,
        GE,
        AND,
        OR,
        NOT,
        QUESTION,
        CARET,
        AMPERSAND,
        ARROW,
        FAT_ARROW,
        BIT_OR,
        BIT_XOR,
        BIT_NOT,
        LSHIFT,
        RSHIFT,

        // Delimiters
        LPAREN,
        RPAREN,
        LBRACE,
        RBRACE,
        LBRACKET,
        RBRACKET,
        COMMA,
        DOT,
        COLON,
        SEMICOLON,

        // Literals
        IDENTIFIER,
        NUMBER,
        STRING,
        CHAR_LITERAL,

        // End of File
        END_OF_FILE,
        UNKNOWN
    };

    struct Token
    {
        TokenType type;
        std::string value;
        int line;
        int column;

        static std::string toString(TokenType type);
        std::string debugString() const;
    };

    class Lexer
    {
    public:
        Lexer(const std::string &source, ErrorReporter &reporter);
        Token getNextToken();
        char peek(); 

    private:
        std::string source;
        ErrorReporter &reporter_;
        size_t currentPos;
        int currentLine;
        int currentCol;

        Token readIdentifier();
        Token readNumber();
        Token readString();
        Token readCharLiteral();
        Token makeToken(TokenType type, const std::string &value);
        void skipWhitespace();
        char advance();
    };

} // namespace Cider

#endif // CIDER_LEXER_H
