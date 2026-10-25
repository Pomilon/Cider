// operator.h — the canonical names and spellings of overloadable operators.
//
// The parser stores one link-safe identifier per operator declaration
// (`operator_add`), and the checker maps a Binary node's BinOp back to the
// same spelling. Keeping both directions in one place means a spelling can
// never drift between declaration and use.

#ifndef CIDER_AST_OPERATOR_H
#define CIDER_AST_OPERATOR_H

#include <string>

#include "ast/ast.h"

namespace cider {

// The canonical identifier for a binary operator, or null when the operator
// is built-in only (`&&`, `||`).
inline const char* binOpOperatorName(BinOp op) {
    switch (op) {
        case BinOp::Add: return "operator_add";
        case BinOp::Sub: return "operator_sub";
        case BinOp::Mul: return "operator_mul";
        case BinOp::Div: return "operator_div";
        case BinOp::Rem: return "operator_rem";
        case BinOp::And: return "operator_bitand";
        case BinOp::Or: return "operator_bitor";
        case BinOp::Xor: return "operator_xor";
        case BinOp::Shl: return "operator_shl";
        case BinOp::Shr: return "operator_shr";
        case BinOp::Lt: return "operator_lt";
        case BinOp::Le: return "operator_le";
        case BinOp::Gt: return "operator_gt";
        case BinOp::Ge: return "operator_ge";
        case BinOp::Eq: return "operator_eq";
        case BinOp::Ne: return "operator_ne";
        case BinOp::AndAnd:
        case BinOp::OrOr:
            return nullptr;
    }
    return nullptr;
}

// The source spelling, for diagnostics.
inline const char* binOpSpelling(BinOp op) {
    switch (op) {
        case BinOp::Add: return "+";
        case BinOp::Sub: return "-";
        case BinOp::Mul: return "*";
        case BinOp::Div: return "/";
        case BinOp::Rem: return "%";
        case BinOp::And: return "&";
        case BinOp::Or: return "|";
        case BinOp::Xor: return "^";
        case BinOp::Shl: return "<<";
        case BinOp::Shr: return ">>";
        case BinOp::Lt: return "<";
        case BinOp::Le: return "<=";
        case BinOp::Gt: return ">";
        case BinOp::Ge: return ">=";
        case BinOp::Eq: return "==";
        case BinOp::Ne: return "!=";
        case BinOp::AndAnd: return "&&";
        case BinOp::OrOr: return "||";
    }
    return "";
}

// The canonical identifier for a compound assignment operator, or null when
// the spelling has no compound form (`==`, `<`, `&&`, ...).
inline const char* compoundAssignOperatorName(BinOp op) {
    switch (op) {
        case BinOp::Add: return "operator_add_assign";
        case BinOp::Sub: return "operator_sub_assign";
        case BinOp::Mul: return "operator_mul_assign";
        case BinOp::Div: return "operator_div_assign";
        case BinOp::Rem: return "operator_rem_assign";
        case BinOp::And: return "operator_bitand_assign";
        case BinOp::Or: return "operator_bitor_assign";
        case BinOp::Xor: return "operator_xor_assign";
        case BinOp::Shl: return "operator_shl_assign";
        case BinOp::Shr: return "operator_shr_assign";
        case BinOp::Lt:
        case BinOp::Le:
        case BinOp::Gt:
        case BinOp::Ge:
        case BinOp::Eq:
        case BinOp::Ne:
        case BinOp::AndAnd:
        case BinOp::OrOr:
            return nullptr;
    }
    return nullptr;
}

// The source spelling, for diagnostics.
inline const char* compoundAssignSpelling(BinOp op) {
    switch (op) {
        case BinOp::Add: return "+=";
        case BinOp::Sub: return "-=";
        case BinOp::Mul: return "*=";
        case BinOp::Div: return "/=";
        case BinOp::Rem: return "%=";
        case BinOp::And: return "&=";
        case BinOp::Or: return "|=";
        case BinOp::Xor: return "^=";
        case BinOp::Shl: return "<<=";
        case BinOp::Shr: return ">>=";
        case BinOp::Lt:
        case BinOp::Le:
        case BinOp::Gt:
        case BinOp::Ge:
        case BinOp::Eq:
        case BinOp::Ne:
        case BinOp::AndAnd:
        case BinOp::OrOr:
            return "";
    }
    return "";
}

// Whether the canonical name denotes `=` or one of its compound forms.
inline bool isAssignmentOperatorName(const std::string& name) {
    return name == "operator_assign" ||
           name == "operator_add_assign" ||
           name == "operator_sub_assign" ||
           name == "operator_mul_assign" ||
           name == "operator_div_assign" ||
           name == "operator_rem_assign" ||
           name == "operator_bitand_assign" ||
           name == "operator_bitor_assign" ||
           name == "operator_xor_assign" ||
           name == "operator_shl_assign" ||
           name == "operator_shr_assign";
}

// The canonical identifier for a unary operator, or null when the operator
// is built-in only (`!`) or is not an operator expression at all (`*`, `&`).
inline const char* unOpOperatorName(UnOp op) {
    switch (op) {
        case UnOp::Neg: return "operator_neg";
        case UnOp::BitNot: return "operator_bitnot";
        case UnOp::Not:
        case UnOp::Deref:
        case UnOp::Addr:
            return nullptr;
    }
    return nullptr;
}

// The source spelling of a unary operator, for diagnostics.
inline const char* unOpSpelling(UnOp op) {
    switch (op) {
        case UnOp::Neg: return "-";
        case UnOp::Not: return "!";
        case UnOp::BitNot: return "~";
        case UnOp::Deref: return "*";
        case UnOp::Addr: return "&";
    }
    return "";
}

inline bool isOperatorName(const std::string& name) {
    return name.rfind("operator_", 0) == 0;
}

}  // namespace cider

#endif  // CIDER_AST_OPERATOR_H
