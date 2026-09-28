#pragma once

#include <memory>
#include <vector>

#include "ast.h"
#include "errors.h"
#include "lexer.h"

namespace cider {

class Parser {
public:
    explicit Parser(Lexer& lexer, ErrorReporter& reporter);
    std::unique_ptr<AstProgram> parseProgram();

    enum Precedence {
        LOWEST,
        ASSIGN,       // =
        TERNARY,      // ? :
        LOGICAL_OR,   // or
        LOGICAL_AND,  // and
        BIT_OR,       // |
        BIT_XOR,      // ^
        BIT_AND,      // &
        EQUALS,       // == !=
        LESSGREATER,  // < > <= >=
        SHIFT,        // << >>
        SUM,          // + -
        PRODUCT,      // * / %
        EXPONENT,     // ^
        PREFIX,       // - ! ~ & * (unary)
        CALL,         // ()
        MEMBER_ACCESS // . -> (access)
    };

private:
    Lexer& lexer_;
    ErrorReporter& reporter_;
    Token currentToken_;
    Token peekToken_;
    Token peekToken2_;

    void nextToken();
    bool expect(TokenType type);
    bool match(TokenType type);
    void error(const std::string& message);

    std::unique_ptr<Expression> parseExpression(bool allowStructLiteral = true);
    std::unique_ptr<Expression> parseExpression(Precedence precedence, bool allowStructLiteral = true);
    std::unique_ptr<Expression> parsePrimaryExpression();
    std::unique_ptr<Expression> parseCallExpression(std::unique_ptr<Expression> callee);
    std::unique_ptr<Expression> parsePrefixExpression();
    std::unique_ptr<Expression> parseInfixExpression(std::unique_ptr<Expression> left);
    std::unique_ptr<Expression> parseMemberAccessExpression(std::unique_ptr<Expression> object, bool isArrow = false);
    std::unique_ptr<Expression> parseIndexExpression(std::unique_ptr<Expression> object);
    std::unique_ptr<Expression> parseStructLiteralExpression(std::unique_ptr<Expression> object);
    std::unique_ptr<Expression> parseCastExpression(std::unique_ptr<Expression> expr);
    std::unique_ptr<Expression> parseSizeofExpression();
    std::unique_ptr<Expression> parseTernaryExpression(std::unique_ptr<Expression> condition);
    std::unique_ptr<Expression> parseFunctionExpression(bool isAsync = false);
    std::unique_ptr<Expression> parseAwaitExpression();
    std::unique_ptr<Expression> parseListLiteral();
    std::unique_ptr<Expression> parseTableLiteral();

    Precedence getPrecedence(TokenType type);

    std::unique_ptr<Statement> parseStatement();
    std::unique_ptr<Statement> parseAbiModifiedFunction();
    std::unique_ptr<Statement> parseVarDeclarationStatement(bool isExtern = false);
    std::unique_ptr<Statement> parseAssignmentStatement(std::unique_ptr<Expression> target);
    std::unique_ptr<Statement> parseIfStatement();
    std::unique_ptr<Statement> parseWhileStatement();
    std::unique_ptr<Statement> parseForStatement();
    std::unique_ptr<Statement> parseDeleteStatement();
    std::unique_ptr<Statement> parseReturnStatement();
    std::unique_ptr<Statement> parseBreakStatement();
    std::unique_ptr<Statement> parseContinueStatement();
    std::unique_ptr<Statement> parseThrowStatement();
    std::unique_ptr<Statement> parseTryCatchStatement();
    std::unique_ptr<Statement> parseDeferStatement();
    std::unique_ptr<Statement> parseAsmStatement();
    std::unique_ptr<Statement> parseMatchStatement();
    std::unique_ptr<Statement> parseExpressionStatement();
    std::unique_ptr<Statement> parseFunctionDeclaration(bool isAsync = false, bool isVirtual = false, bool isOverride = false, bool isExtern = false);
    std::unique_ptr<Statement> parseStructDeclaration(bool isUnion = false);
    std::unique_ptr<Statement> parseEnumDeclaration();
    std::unique_ptr<Statement> parseTypeAliasStatement();
    std::unique_ptr<Statement> parseClassDeclaration();
    std::unique_ptr<Statement> parseInterfaceDeclaration();
    std::unique_ptr<Statement> parseImportStatement();
    std::unique_ptr<Statement> parseFromImportStatement();
    std::unique_ptr<Statement> parseExportStatement();

    std::unique_ptr<TypeAST> parseType(bool castContext = false);
    // CID-006 `fn(...) -> R`; CIDER-BUG-063 reuses it for the legacy
    // `func(...)`/`function(...)` closure spelling (kindName carries the
    // written keyword, basePointerLevel the legacy +1 object-pointer level).
    std::unique_ptr<TypeAST> parseFunctionPointerType(int line, int col, bool isVolatile, Abi abi,
                                                      const std::string& kindName = "fn",
                                                      int basePointerLevel = 0);
    std::vector<std::unique_ptr<Statement>> parseBlockStatement();
    std::vector<std::string> parseTemplateParams();

    bool isTypeToken(TokenType type) const;
};

}  // namespace cider
