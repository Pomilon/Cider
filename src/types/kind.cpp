// kind.cpp — the two name tables, and the one predicate over Kind.
//
// Every switch here is over a Kind or a BasicKind and has no `default:`, so
// adding an enumerator breaks the build in three places at once rather than
// silently producing an empty name in a diagnostic. That is the point: a
// diagnostic that prints an empty string for a type the compiler has never heard
// of is worse than no diagnostic.

#include "types/kind.h"

namespace cider {

const char* kindName(Kind kind) {
    switch (kind) {
        case Kind::Error: return "error";
        case Kind::Void: return "void";
        case Kind::String: return "string";
        case Kind::Basic: return "basic";
        case Kind::Pointer: return "pointer";
        case Kind::Array: return "array";
        case Kind::Slice: return "slice";
        case Kind::Struct: return "struct";
        case Kind::Class: return "class";
        case Kind::Interface: return "interface";
        case Kind::Union: return "union";
        case Kind::Enum: return "enum";
        case Kind::Closure: return "closure";
        case Kind::FunctionPtr: return "function pointer";
        case Kind::FunctionItem: return "function";
        case Kind::TypeParam: return "type parameter";
        case Kind::Alias: return "alias";
        case Kind::Wildcard: return "wildcard";
    }
    return "unreachable";
}

const char* basicKindName(BasicKind basic) {
    switch (basic) {
        case BasicKind::I8: return "i8";
        case BasicKind::I16: return "i16";
        case BasicKind::I32: return "i32";
        case BasicKind::I64: return "i64";
        case BasicKind::U8: return "u8";
        case BasicKind::U16: return "u16";
        case BasicKind::U32: return "u32";
        case BasicKind::U64: return "u64";
        case BasicKind::F32: return "f32";
        case BasicKind::F64: return "f64";
        case BasicKind::Bool: return "bool";
        case BasicKind::Char: return "char";
    }
    return "unreachable";
}

bool basicIsSigned(BasicKind basic) {
    switch (basic) {
        case BasicKind::I8:
        case BasicKind::I16:
        case BasicKind::I32:
        case BasicKind::I64:
        case BasicKind::F32:
        case BasicKind::F64:
            return true;
        case BasicKind::U8:
        case BasicKind::U16:
        case BasicKind::U32:
        case BasicKind::U64:
        case BasicKind::Bool:
        case BasicKind::Char:
            return false;
    }
    return false;
}

bool basicIsInteger(BasicKind basic) {
    switch (basic) {
        case BasicKind::F32:
        case BasicKind::F64:
        case BasicKind::Bool:
        case BasicKind::Char:
            return false;
        case BasicKind::I8:
        case BasicKind::I16:
        case BasicKind::I32:
        case BasicKind::I64:
        case BasicKind::U8:
        case BasicKind::U16:
        case BasicKind::U32:
        case BasicKind::U64:
            return true;
    }
    return false;
}

bool basicIsFloat(BasicKind basic) {
    switch (basic) {
        case BasicKind::F32:
        case BasicKind::F64:
            return true;
        case BasicKind::I8:
        case BasicKind::I16:
        case BasicKind::I32:
        case BasicKind::I64:
        case BasicKind::U8:
        case BasicKind::U16:
        case BasicKind::U32:
        case BasicKind::U64:
        case BasicKind::Bool:
        case BasicKind::Char:
            return false;
    }
    return false;
}

int basicWidthBits(BasicKind basic) {
    switch (basic) {
        case BasicKind::I8:
        case BasicKind::U8:
        case BasicKind::Bool:
        case BasicKind::Char:
            return 8;
        case BasicKind::I16:
        case BasicKind::U16:
            return 16;
        case BasicKind::I32:
        case BasicKind::U32:
        case BasicKind::F32:
            return 32;
        case BasicKind::I64:
        case BasicKind::U64:
        case BasicKind::F64:
            return 64;
    }
    return 0;
}

bool kindHasResolvedLayout(Kind kind) {
    switch (kind) {
        case Kind::Alias:
        case Kind::TypeParam:
        case Kind::Wildcard:
            return false;
        case Kind::Error:
        case Kind::Void:
        case Kind::String:
        case Kind::Basic:
        case Kind::Pointer:
        case Kind::Array:
        case Kind::Slice:
        case Kind::Struct:
        case Kind::Class:
        case Kind::Interface:
        case Kind::Union:
        case Kind::Enum:
        case Kind::Closure:
        case Kind::FunctionPtr:
        case Kind::FunctionItem:
            return true;
    }
    return false;
}

bool kindIsHeapOwned(Kind kind) {
    switch (kind) {
        // A class reference is heap storage, released by `delete`. A bare
        // `Class` names the object rather than a reference to it (TYPES.md
        // §4.2), and both spellings lead to the same storage.
        case Kind::Class:
            return true;
        // Fixed arrays, structs and unions reach the heap when built with `new`,
        // and MEMORY.md §3 records "nothing, or `delete`" for each.
        case Kind::Array:
        case Kind::Struct:
        case Kind::Union:
            return true;
        // Not heap-owned, each for a stated reason:
        //   String, Slice and Pointer are 16- or 8-byte values that borrow.
        //   Interface is a fat pointer; it does not own the class it names
        //   (SEMANTICS.md §9.3).
        //   Closure is a value whose captures are never released by the
        //   language (MEMORY.md §3.2).
        case Kind::Error:
        case Kind::Void:
        case Kind::String:
        case Kind::Basic:
        case Kind::Pointer:
        case Kind::Slice:
        case Kind::Interface:
        case Kind::Enum:
        case Kind::Closure:
        case Kind::FunctionPtr:
        case Kind::FunctionItem:
        case Kind::TypeParam:
        case Kind::Alias:
        case Kind::Wildcard:
            return false;
    }
    return false;
}

}  // namespace cider
