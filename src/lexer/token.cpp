// token.cpp — one name per kind.
//
// Exhaustive with no `default:`, so a new TokenKind breaks the build here
// rather than printing nothing in a diagnostic. Gate T1 plus -Werror=switch.

#include "lexer/token.h"

namespace cider {

const char* tokenName(TokenKind kind) {
    switch (kind) {
        case TokenKind::End: return "end of file";
        case TokenKind::Error: return "error";
        case TokenKind::Ident: return "identifier";
        case TokenKind::IntLit: return "integer literal";
        case TokenKind::FloatLit: return "float literal";
        case TokenKind::CharLit: return "character literal";
        case TokenKind::StringLit: return "string literal";
        case TokenKind::WideStringLit: return "wide string literal";
        case TokenKind::WideCharLit: return "wide character literal";
        case TokenKind::KwFunc: return "func";
        case TokenKind::KwExtern: return "extern";
        case TokenKind::KwStruct: return "struct";
        case TokenKind::KwClass: return "class";
        case TokenKind::KwInterface: return "interface";
        case TokenKind::KwUnion: return "union";
        case TokenKind::KwEnum: return "enum";
        case TokenKind::KwVar: return "var";
        case TokenKind::KwConst: return "const";
        case TokenKind::KwType: return "type";
        case TokenKind::KwImport: return "import";
        case TokenKind::KwFrom: return "from";
        case TokenKind::KwExport: return "export";
        case TokenKind::KwReturn: return "return";
        case TokenKind::KwIf: return "if";
        case TokenKind::KwElse: return "else";
        case TokenKind::KwWhile: return "while";
        case TokenKind::KwFor: return "for";
        case TokenKind::KwIn: return "in";
        case TokenKind::KwMatch: return "match";
        case TokenKind::KwDefault: return "default";
        case TokenKind::KwBreak: return "break";
        case TokenKind::KwContinue: return "continue";
        case TokenKind::KwDefer: return "defer";
        case TokenKind::KwTry: return "try";
        case TokenKind::KwCatch: return "catch";
        case TokenKind::KwThrow: return "throw";
        case TokenKind::KwDelete: return "delete";
        case TokenKind::KwNew: return "new";
        case TokenKind::KwAs: return "as";
        case TokenKind::KwAnd: return "and";
        case TokenKind::KwOr: return "or";
        case TokenKind::KwNot: return "not";
        case TokenKind::KwThis: return "this";
        case TokenKind::KwSelf: return "self";
        case TokenKind::KwSuper: return "super";
        case TokenKind::KwOverride: return "override";
        case TokenKind::KwInit: return "init";
        case TokenKind::KwDeinit: return "deinit";
        case TokenKind::KwVirtual: return "virtual";
        case TokenKind::KwPrivate: return "private";
        case TokenKind::KwPublic: return "public";
        case TokenKind::KwTrue: return "true";
        case TokenKind::KwFalse: return "false";
        case TokenKind::KwNilPtr: return "nilPtr";
        case TokenKind::KwUninit: return "uninit";
        case TokenKind::KwFini: return "fini";
        case TokenKind::KwSizeof: return "sizeof";
        case TokenKind::KwAlignof: return "alignof";
        case TokenKind::KwAsm: return "asm";
        case TokenKind::KwVolatile: return "volatile";
        case TokenKind::KwExperimental: return "experimental";
        case TokenKind::KwInt: return "int";
        case TokenKind::KwI8: return "i8";
        case TokenKind::KwI16: return "i16";
        case TokenKind::KwI32: return "i32";
        case TokenKind::KwI64: return "i64";
        case TokenKind::KwU8: return "u8";
        case TokenKind::KwU16: return "u16";
        case TokenKind::KwU32: return "u32";
        case TokenKind::KwU64: return "u64";
        case TokenKind::KwF32: return "f32";
        case TokenKind::KwF64: return "f64";
        case TokenKind::KwBool: return "bool";
        case TokenKind::KwChar: return "char";
        case TokenKind::KwByte: return "byte";
        case TokenKind::KwString: return "string";
        case TokenKind::KwVoid: return "void";
        case TokenKind::LParen: return "(";
        case TokenKind::RParen: return ")";
        case TokenKind::LBrace: return "{";
        case TokenKind::RBrace: return "}";
        case TokenKind::LBracket: return "[";
        case TokenKind::RBracket: return "]";
        case TokenKind::Comma: return ",";
        case TokenKind::Colon: return ":";
        case TokenKind::Semi: return ";";
        case TokenKind::Dot: return ".";
        case TokenKind::Question: return "?";
        case TokenKind::Plus: return "+";
        case TokenKind::Minus: return "-";
        case TokenKind::Star: return "*";
        case TokenKind::Slash: return "/";
        case TokenKind::Percent: return "%";
        case TokenKind::Amp: return "&";
        case TokenKind::Pipe: return "|";
        case TokenKind::Caret: return "^";
        case TokenKind::Tilde: return "~";
        case TokenKind::Bang: return "!";
        case TokenKind::Lt: return "<";
        case TokenKind::Gt: return ">";
        case TokenKind::Eq: return "=";
        case TokenKind::AndAnd: return "&&";
        case TokenKind::OrOr: return "||";
        case TokenKind::EqEq: return "==";
        case TokenKind::BangEq: return "!=";
        case TokenKind::LtEq: return "<=";
        case TokenKind::GtEq: return ">=";
        case TokenKind::LtLt: return "<<";
        case TokenKind::GtGt: return ">>";
        case TokenKind::PlusPlus: return "++";
        case TokenKind::MinusMinus: return "--";
        case TokenKind::PlusEq: return "+=";
        case TokenKind::MinusEq: return "-=";
        case TokenKind::StarEq: return "*=";
        case TokenKind::SlashEq: return "/=";
        case TokenKind::PercentEq: return "%=";
        case TokenKind::AmpEq: return "&=";
        case TokenKind::PipeEq: return "|=";
        case TokenKind::CaretEq: return "^=";
        case TokenKind::LtLtEq: return "<<=";
        case TokenKind::GtGtEq: return ">>=";
        case TokenKind::At: return "@";
        case TokenKind::Arrow: return "->";
        case TokenKind::FatArrow: return "=>";
        case TokenKind::ColonColon: return "::";
        case TokenKind::Ellipsis: return "...";
        case TokenKind::Hash: return "#";
    }
    return "unreachable";
}

}  // namespace cider
