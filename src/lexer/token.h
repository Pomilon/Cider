// token.h — what the lexer produces. LAYERS.md §3.1.
//
// One character in, one token out, with a position. A Token carries its
// spelling for diagnostics and its decoded value for everyone else, so nothing
// downstream re-reads literal text to find out what a number is.
//
// The decoded value lives beside the spelling rather than instead of it:
// `1_000_000` keeps its underscores in `text` and `1000000` in the payload.
// A diagnostic prints what the programmer wrote; the checker reads what it
// means. One decoder, one answer.

#ifndef CIDER_LEXER_TOKEN_H
#define CIDER_LEXER_TOKEN_H

#include <cstdint>
#include <string>
#include <vector>

namespace cider {

// Where a token starts: the file it came from, 1-based line and column.
// Columns count bytes, not characters: the source is UTF-8 and the lexer does
// not decode it beyond ASCII structure.
struct Position {
    std::string file;
    uint32_t line = 1;
    uint32_t column = 1;
};

// Every spelling the language has. Keywords are distinct kinds rather than
// identifiers with meaning attached, so the parser can switch on them
// exhaustively instead of comparing strings at every match.
enum class TokenKind {
    End,      // past the last character; always the final token
    Error,    // a malformed literal or character; lexing continues after it
    Ident,    // a name that is not a keyword
    IntLit,   // `42`, `0xFF`, `1_000_000` — value in Literal, no type
    FloatLit, // `3.14`, `1.0e-9` — text in Literal, no type
    CharLit,  // `'x'` — a code point in Literal
    StringLit,// `"text"`, `"x = {e}"` — parts in Literal
    WideStringLit,// `u"AB"` — UTF-16 units in Literal, no interpolation
    WideCharLit, // `u'Z'` — a code point in Literal, typed u16

    KwFunc, KwExtern, KwStruct, KwClass, KwInterface, KwUnion, KwEnum,
    KwVar, KwConst, KwType, KwImport, KwFrom, KwExport, KwReturn,
    KwIf, KwElse, KwWhile, KwFor, KwIn, KwMatch, KwDefault,
    KwBreak, KwContinue, KwDefer, KwTry, KwCatch, KwThrow,
    KwDelete, KwNew, KwAs, KwAnd, KwOr, KwNot,
    KwSelf, KwThis, KwSuper, KwInit, KwDeinit, KwVirtual, KwPrivate, KwPublic,
    KwTrue, KwFalse, KwNilPtr, KwUninit, KwFini, KwSizeof, KwAsm, KwVolatile, KwExperimental,

    KwInt, KwI8, KwI16, KwI32, KwI64,
    KwU8, KwU16, KwU32, KwU64,
    KwF32, KwF64, KwBool, KwChar, KwByte, KwString, KwVoid,

    LParen, RParen, LBrace, RBrace, LBracket, RBracket,
    Comma, Colon, Semi, Dot, Question,
    Plus, Minus, Star, Slash, Percent, Amp, Pipe, Caret, Tilde, Bang,
    Lt, Gt, Eq,
    AndAnd, OrOr, EqEq, BangEq, LtEq, GtEq, LtLt, GtGt,
    PlusPlus, MinusMinus,
    PlusEq, MinusEq, StarEq, SlashEq, PercentEq,
    AmpEq, PipeEq, CaretEq, LtLtEq, GtGtEq,
    Arrow, FatArrow, ColonColon, Ellipsis, Hash,
};

// One fragment of a string literal. Text parts hold bytes; expression parts
// hold the tokens between the braces, lexed by the same lexer, so the parser
// never re-reads the literal's characters to find out what `{e}` means.
struct StringPart {
    bool isExpr = false;
    std::string text;
    std::vector<struct Token> expr;
    Position pos;
};

// The decoded value of a literal token. Only the field matching `kind` is
// live; the rest are empty. `intBase` is 0 for every non-integer token.
struct Literal {
    uint8_t intBase = 0;          // IntLit: 2, 10 or 16
    std::string intDigits;        // IntLit: canonical digits, no `_`, no prefix
    std::string floatDigits;      // FloatLit: exact text
    uint32_t charValue = 0;       // CharLit: the code point
    std::vector<StringPart> parts;// StringLit: text and expression fragments
    std::vector<uint16_t> wideUnits;  // WideStringLit: decoded UTF-16 units
};

struct Token {
    TokenKind kind = TokenKind::End;
    std::string text;             // the spelling, for diagnostics
    Position pos;                 // where it starts
    Literal lit;                  // the decoded value, for everyone else
};

// A malformed literal or character: what it was and where. The lexer records
// one per problem and keeps going, so a file with three bad literals reports
// three diagnostics rather than stopping at the first.
struct LexError {
    Position pos;
    std::string message;
};

// The spelling of a kind, for diagnostics and tests. One function, so a kind
// has one name everywhere it is printed.
const char* tokenName(TokenKind kind);

}  // namespace cider

#endif  // CIDER_LEXER_TOKEN_H
