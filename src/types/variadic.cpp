// variadic.cpp — the C default argument promotions (VARIADICS.md §4).
//
// The table is the document's: the narrow integers widen to i32, f32 widens
// to f64, and everything already ABI-shaped passes through. A type outside
// the table has no variadic spelling yet, so this answers nothing rather than
// inventing a representation the lowerer cannot carry.

#include "types/variadic.h"

#include "types/context.h"

namespace cider {

std::optional<TypeRef> promoteVariadicArgument(TypeRef type, TypeContext& ctx) {
    if (type.isError()) {
        return std::nullopt;
    }
    switch (type.type->kind()) {
        // A data pointer and a function pointer are one code/data word on
        // the supported ABI, and C passes both through unchanged.
        case Kind::Pointer:
        case Kind::FunctionPtr:
            return type;
        // C enums are ints: the value crosses the tail as its i32 member
        // (the enum's own layout is 4 bytes, layout.h's kSizeEnum).
        case Kind::Enum:
            return TypeRef{ctx.basic(BasicKind::I32)};
        case Kind::Basic:
            break;
        // Everything else is an aggregate, a nominal, a closure or an
        // unresolved spelling: no C variadic representation in this slice.
        case Kind::Error:
        case Kind::Void:
        case Kind::String:
        case Kind::Array:
        case Kind::Slice:
        case Kind::Struct:
        case Kind::Class:
        case Kind::Interface:
        case Kind::Union:
        case Kind::VaList:
        case Kind::Closure:
        case Kind::FunctionItem:
        case Kind::TypeParam:
        case Kind::Alias:
        case Kind::Wildcard:
            return std::nullopt;
    }
    switch (*type.type->basic()) {
        case BasicKind::Bool:
        case BasicKind::Char:
        case BasicKind::I8:
        case BasicKind::I16:
        case BasicKind::U8:
        case BasicKind::U16:
            // The integer promotions widen every narrower-than-i32 integer to
            // i32, signed or not: the value reaches the callee the way C
            // passes it.
            return TypeRef{ctx.basic(BasicKind::I32)};
        case BasicKind::F32:
            return TypeRef{ctx.basic(BasicKind::F64)};
        case BasicKind::I32:
        case BasicKind::U32:
        case BasicKind::I64:
        case BasicKind::U64:
        case BasicKind::F64:
            return type;
    }
    return std::nullopt;
}

}  // namespace cider
