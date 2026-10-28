// variadic.h — the C default argument promotions (VARIADICS.md §4).
//
// A variadic call's arguments past the named parameters are not declared by
// anything, so the language converts each one the way C does before the ABI
// ever sees it. This is one rule stated once: the checker asks it, and no
// other layer re-derives which source types may cross the C variadic tail.

#ifndef CIDER_TYPES_VARIADIC_H
#define CIDER_TYPES_VARIADIC_H

#include <optional>

#include "types/type.h"

namespace cider {

class TypeContext;

// The type a variadic argument is passed as, or nothing when the type has no
// C variadic representation in this slice: bool, char, i8, i16, u8 and u16
// widen to i32; f32 widens to f64; i32, u32, i64, u64 and f64 pass
// unchanged; data and function pointers pass unchanged; an enum passes as its
// i32 value. Strings, arrays, structs, classes, unions, closures and va_list
// answer nothing, because the first slice carries scalar values only
// (VARIADICS.md §4).
std::optional<TypeRef> promoteVariadicArgument(TypeRef type, TypeContext& ctx);

}  // namespace cider

#endif  // CIDER_TYPES_VARIADIC_H
