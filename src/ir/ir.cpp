// ir.cpp — one name per IRType, Op, CmpOp and TrapReason.
//
// Exhaustive with no `default:` everywhere, so a new enumerator breaks the
// build here rather than emitting an empty name in an IR dump. An IR dump that
// prints nothing for an instruction the compiler just emitted is worse than
// no dump.

#include "ir/ir.h"

namespace cider {
namespace ir {

const char* irTypeName(IRType type) {
    switch (type) {
        case IRType::I8: return "i8";
        case IRType::U8: return "u8";
        case IRType::I16: return "i16";
        case IRType::U16: return "u16";
        case IRType::I32: return "i32";
        case IRType::U32: return "u32";
        case IRType::I64: return "i64";
        case IRType::U64: return "u64";
        case IRType::F32: return "f32";
        case IRType::F64: return "f64";
        case IRType::Bool: return "bool";
        case IRType::Ptr: return "ptr";
        case IRType::Void: return "void";
    }
    return "unreachable";
}

const char* cmpOpName(CmpOp op) {
    switch (op) {
        case CmpOp::Eq: return "eq";
        case CmpOp::Ne: return "ne";
        case CmpOp::Lt: return "lt";
        case CmpOp::Le: return "le";
        case CmpOp::Gt: return "gt";
        case CmpOp::Ge: return "ge";
        case CmpOp::Ult: return "ult";
        case CmpOp::Ule: return "ule";
        case CmpOp::Ugt: return "ugt";
        case CmpOp::Uge: return "uge";
    }
    return "unreachable";
}

const char* trapReasonName(TrapReason reason) {
    switch (reason) {
        case TrapReason::IndexOutOfBounds: return "index_out_of_bounds";
        case TrapReason::DivByZero: return "div_by_zero";
        case TrapReason::MinDivNegOne: return "min_div_neg_one";
        case TrapReason::BitfieldOverflow: return "bitfield_overflow";
        case TrapReason::UnionTag: return "union_tag";
        case TrapReason::Downcast: return "downcast";
        case TrapReason::Null: return "null";
        case TrapReason::Explicit: return "explicit";
    }
    return "unreachable";
}

const char* opName(Op op) {
    switch (op) {
        case Op::Const: return "const";
        case Op::Copy: return "copy";
        case Op::Add: return "add";
        case Op::Sub: return "sub";
        case Op::Mul: return "mul";
        case Op::Div: return "div";
        case Op::Rem: return "rem";
        case Op::And: return "and";
        case Op::Or: return "or";
        case Op::Xor: return "xor";
        case Op::Shl: return "shl";
        case Op::Shr: return "shr";
        case Op::Neg: return "neg";
        case Op::Not: return "not";
        case Op::BitNot: return "bitnot";
        case Op::FAdd: return "fadd";
        case Op::FSub: return "fsub";
        case Op::FMul: return "fmul";
        case Op::FDiv: return "fdiv";
        case Op::FRem: return "frem";
        case Op::Cmp: return "cmp";
        case Op::Load: return "load";
        case Op::Store: return "store";
        case Op::Alloca: return "alloca";
        case Op::Extract: return "extract";
        case Op::Insert: return "insert";
        case Op::Gep: return "gep";
        case Op::Call: return "call";
        case Op::Cast: return "cast";
        case Op::Trap: return "trap";
        case Op::Asm: return "asm";
    }
    return "unreachable";
}

}  // namespace ir
}  // namespace cider
