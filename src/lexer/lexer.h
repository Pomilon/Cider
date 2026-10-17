// lexer.h — the first layer. LAYERS.md §3.1.
//
// Owns character positions, literal decoding, escape sequences, comment
// handling, and the spelling of every token. May read the source text and
// nothing else. Never decides what a token means: it does not know whether an
// identifier is a variable or a type, and it does not resolve names.
//
// Comments are `//` to the end of the line, and only that. One spelling for a
// comment, like everything else in this language.
//
// Escapes, in both character and string literals:
//
//     \n \t \r \\ \' \" \0 \{ \}
//
// The last two exist because interpolation uses braces: `\{` is a brace that
// stays text. Anything else after a backslash is a diagnostic, not a guess.

#ifndef CIDER_LEXER_LEXER_H
#define CIDER_LEXER_LEXER_H

#include <string>
#include <vector>

#include "lexer/token.h"

namespace cider {

class Lexer {
public:
    Lexer(std::string file, std::string source);

    // Lexes the whole source. The last token is always End. Malformed input
    // becomes Error tokens with an entry in errors(), and lexing continues
    // after each one — LAYERS.md §3.1's contract.
    std::vector<Token> lex();

    const std::vector<LexError>& errors() const { return errors_; }

private:
    char peek() const;
    // The character `n` ahead, or 0 past the end. Bounds-checked, so callers
    // never index the source directly for lookahead.
    char peekAt(uint64_t n) const;
    char advance();
    bool atEnd() const;
    bool match(char want);

    void skipSpaceAndComments();
    Token nextToken();
    Token number();
    Token character();
    Token string();
    // Wide literals: `u"AB"` decodes to UTF-16 units, `u'Z'` to one code
    // point. No interpolation — a lone `{` is a diagnostic, `{{` spells a
    // brace; narrow strings give the pair the identical meaning.
    Token wideString();
    Token wideChar();
    // Lexes the tokens of one `{...}` interpolation, stopping after the
    // closing brace. The caller records the spelling; these are the meaning.
    std::vector<Token> braced(int depth);

    void fail(Position pos, const std::string& message);

    std::string file_;
    std::string source_;
    uint64_t cursor_ = 0;
    uint32_t line_ = 1;
    uint32_t column_ = 1;
    std::vector<LexError> errors_;
};

}  // namespace cider

#endif  // CIDER_LEXER_LEXER_H
