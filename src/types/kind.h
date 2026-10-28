// kind.h — the tag of the type union. TYPES.md §2.1.
//
// A `Kind` here and a `Kind` in a switch in lowering must be the same list, and
// gate T1 exists to keep them so: `-Werror=switch` makes a missing case a build
// failure, and scripts/gates.py fails the build on a `default:` that would
// swallow one. The two halves are both required because they catch opposite
// mistakes -- a missing case compiles silently *with* a default, and a default
// on an exhaustive switch is invisible to the compiler.
//
// The enumerators are quoted from TYPES.md §2.1 in that order. The document's
// pseudocode is a sketch; this is the real form, and where it adds an
// enumerator the document already names it (§10.5 added `FunctionItem`).

#ifndef CIDER_TYPES_KIND_H
#define CIDER_TYPES_KIND_H

namespace cider {

// The tag of a Type. Every enumerator is meaningful; `Error` is the poison
// value of §7 and `Wildcard` is internal to inference positions.
enum class Kind {
    Error,        // the poison value (§7)
    Void,
    String,       // the immutable byte sequence of FEATURES 1.4 (16 bytes)
    Basic,        // int, u8, float, bool, char, byte ...
    Pointer,      // to anything, including function pointers; never a closure
    Array,        // fixed length
    Slice,        // length + pointer
    Struct,
    Class,
    Interface,
    Union,
    VaList,       // the compiler-owned C variadic cursor (VARIADICS.md §5);
                  // opaque: its fields belong to the target ABI, never to
                  // user code, and its layout comes from layout.h alone
    Enum,
    Closure,      // func(...) - a closure OBJECT (see §4.5)
    FunctionPtr,  // *func(...) -> T
    FunctionItem, // a function *name*, written `f` or `&f`; not a value type
                  // in its own right — its type is FunctionPtr or Closure
    TypeParam,    // a template's T, before instantiation
    Alias,        // type X = Y, before resolution
    Wildcard,     // internal: "any", only in inference positions
};

// The width and signedness of a Kind::Basic. FEATURES.md §1.1 fixes eight
// integer widths; §1.1b adds `char` as a distinct 1-byte type; §1.2 and §1.3
// give float and bool. `byte` and `int` are aliases and deliberately absent --
// an alias is not a distinct basic kind (§10.1), and naming one here would make
// it a second thing that has to agree with u8 and i32.
enum class BasicKind {
    I8,
    I16,
    I32,
    I64,
    U8,
    U16,
    U32,
    U64,
    F32,
    F64,
    Bool,
    Char,
};

// The human-readable name of a kind, for diagnostics. One function, one owner:
// FEATURES.md §11.1 requires that a failure name the thing that failed, and a
// second spelling of a kind in a diagnostic path is how v2 ended up with 25
// sites writing "error" as a type name.
const char* kindName(Kind kind);

// The human-readable name of a basic kind. Same rule.
const char* basicKindName(BasicKind basic);

// What a basic kind is, asked once here rather than re-derived by every
// switch that needs it. Widths are in bits; layout.h owns byte sizes, and
// bits are bytes times eight, which is arithmetic rather than a second
// authority for a number.
bool basicIsSigned(BasicKind basic);
bool basicIsInteger(BasicKind basic);
bool basicIsFloat(BasicKind basic);
int basicWidthBits(BasicKind basic);

// Whether a size is known for this kind. TYPES.md §3.2 requires sizeOf to be
// total: Alias, TypeParam and Wildcard are unresolved by definition, so asking
// for a size is a compile-time rejection rather than a default.
bool kindHasResolvedLayout(Kind kind);

// True for the kinds whose values own heap storage that `delete` releases.
// MEMORY.md §3 lists them; a closure is absent because there is no
// pointer-to-closure type and no `deinit` to run (§3.2 there).
bool kindIsHeapOwned(Kind kind);

}  // namespace cider

#endif  // CIDER_TYPES_KIND_H
