// type.h — what a type actually is. TYPES.md §2.1 and §2.2.
//
// The organising rule:
//
//     A Kind says how a thing is TREATED. A Type says what that thing IS.
//
// Two things share a Kind when the compiler handles them identically, and are
// different Types when they are not the same thing. `u8` and `i32` are both
// Kind::Basic and different Types. `Shape` and `Circle` are both Kind::Class
// and different Types. `*Shape` and `Shape` differ in Kind, because reading
// through one needs a dereference and reading the other does not.
//
// So a Kind is not a name. `kindName(Kind::Basic)` is "basic", which names the
// treatment and nothing else. `typeName` below is the single naming authority.
//
// ## Why interning
//
// A Type must contain another Type (`type Node { var next: *Node; }`), so a
// Type stored by value inside its own payload is infinitely large and cannot
// compile — indirection is forced. Gate T3 forbids shared ownership in
// src/types/. The answer is interning: a TypeContext owns every Type and hands
// out `const Type*` handles, and two handles name the same type exactly when
// they are the same pointer. Structural identity falls out of construction,
// which is what TYPES.md §2.3 asks for, and the checker never walks a type
// twice to compare it.
//
// A Type is immutable once built: nothing in the language mutates a type, so
// the resolver builds a new one and whatever already refers to the old one is
// unaffected.
//
// `volatile` is deliberately not a field here. FEATURES.md §1.1c makes it a
// property of a declaration — how a place is accessed — and folding it into the
// payload would make one type two, with interning keyed on access rules.

#ifndef CIDER_TYPES_TYPE_H
#define CIDER_TYPES_TYPE_H

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

#include "types/kind.h"
#include "types/signature.h"

namespace cider {

class TypeContext;

// Identifies a nominal type: a struct, class, interface, union or enum. A
// handle, not a pointer to a definition, so a Type never owns its declaration.
// TYPES.md §2.4 requires a resolved Type to contain no Alias and no TypeParam,
// so neither may appear as a DeclId here.
struct DeclId {
    uint32_t module = 0;
    uint32_t index = 0;
};

// True when two handles name the same declaration. The one comparison for
// declarations, written once here.
bool sameDecl(DeclId lhs, DeclId rhs);

// --- The payloads ------------------------------------------------------------
//
// Each alternative holds exactly the fields its Kind has, and no others. Every
// recursive reference is a `const Type*` handle, which is also what makes each
// payload a complete type so they can all live in one variant.

struct VoidPayload {};

struct StringPayload {};

struct BasicPayload {
    BasicKind basic = BasicKind::I32;
};

struct PointerPayload {
    const Type* pointee = nullptr;
};

struct ArrayPayload {
    const Type* element = nullptr;
    int64_t length = 0;
};

struct SlicePayload {
    const Type* element = nullptr;
};

// One payload serves Struct, Class, Interface, Union and Enum: those five are
// treated alike by every layout rule and differ only in which DeclId they name.
// One payload means a size cannot depend on which of the five a code path
// happened to spell.
struct NominalPayload {
    DeclId decl;
};

struct SignaturePayload {
    Signature signature;
};

struct TypeParamPayload {
    DeclId decl;
    uint32_t index = 0;
};

// AliasPayload.target names what `type X = Y` names, for diagnostics only. A
// resolved Type never keeps one (TYPES.md §2.4).
struct AliasPayload {
    DeclId target;
};

struct WildcardPayload {};

struct ErrorPayload {};

// FunctionItem carries no payload: a function name is not a type, and its type
// is always the FunctionPtr `&f` produces or the Closure that naming `f`
// without `&` produces. It exists so a name has a type while being checked,
// and is never stored in a variable (TYPES.md §10 row 5).
struct FunctionItemPayload {};

using TypePayload = std::variant<
    VoidPayload,         // Kind::Void
    StringPayload,       // Kind::String
    BasicPayload,        // Kind::Basic
    PointerPayload,      // Kind::Pointer
    ArrayPayload,        // Kind::Array
    SlicePayload,        // Kind::Slice
    NominalPayload,      // Kind::Struct, Class, Interface, Union, Enum
    SignaturePayload,    // Kind::FunctionPtr, Closure
    TypeParamPayload,    // Kind::TypeParam
    AliasPayload,        // Kind::Alias
    WildcardPayload,     // Kind::Wildcard
    ErrorPayload,        // Kind::Error
    FunctionItemPayload  // Kind::FunctionItem
>;

// --- The type ----------------------------------------------------------------

// Built only by a TypeContext, which owns it. Never copied, never mutated.
// The constructor takes the payload and nothing else: Kind is derived from it,
// so there is nothing that could disagree.
class Type {
public:
    // The default is the poison value: TYPES.md §7 says there is exactly one,
    // and a Type that starts life as anything else would let a bug construct a
    // valid type by accident.
    Type() = default;

    // How it is treated, derived from the payload.
    Kind kind() const { return kind_; }

    // What it is.
    const TypePayload& payload() const { return payload_; }

    // Scalar accessors. Each answers only when the Kind holds that field;
    // otherwise empty. No default is invented for the wrong Kind, because a
    // default that looks like a real field is how a bug reads a length off a
    // pointer and keeps going.
    std::optional<BasicKind> basic() const;
    std::optional<int64_t> length() const;
    std::optional<DeclId> decl() const;

    // Handle accessors. Each answers the poison Type when asked of a Kind that
    // does not hold it, rather than null — TYPES.md §7 says an Error stays an
    // Error, and null would be a second poison value that callers forget to
    // check.
    const Type* pointee() const;
    const Type* element() const;

    // The signature of a function pointer or closure. Null when the Kind holds
    // none: a Signature has no poison value, so absence is a null pointer and
    // callers check it.
    const Signature* signature() const;

    // True for Kind::Error. A phase that receives one stops rather than
    // substituting anything (TYPES.md §7).
    bool isError() const { return kind_ == Kind::Error; }

    // Whether a size is known. TYPES.md §3.2 requires sizeOf to be total: it
    // answers for every resolved Type and reports the three Kinds that are not
    // a type yet. Asked once here rather than re-derived by every caller.
    bool hasResolvedLayout() const { return kindHasResolvedLayout(kind_); }

    // The one poison value. Function-local static, so exactly one exists.
    static const Type& poison();

private:
    friend class TypeContext;

    // One constructor per payload. Each sets the Kind that owns it, so the
    // pairing is fixed at compile time and no call can mix them.
    explicit Type(VoidPayload payload);
    explicit Type(StringPayload payload);
    explicit Type(BasicPayload payload);
    explicit Type(PointerPayload payload);
    explicit Type(ArrayPayload payload);
    explicit Type(SlicePayload payload);
    explicit Type(NominalPayload payload, Kind kind);
    explicit Type(SignaturePayload payload, Kind kind);
    explicit Type(TypeParamPayload payload);
    explicit Type(AliasPayload payload);
    explicit Type(WildcardPayload payload);
    explicit Type(ErrorPayload payload);
    explicit Type(FunctionItemPayload payload);

    Kind kind_ = Kind::Error;
    TypePayload payload_ = ErrorPayload{};
};

// --- Handles -----------------------------------------------------------------

// What every layer holds. Two handles name the same type exactly when they are
// the same pointer, because the intern table has already made that true.
struct TypeRef {
    const Type* type = nullptr;

    // The single equality for types in src/types/; the gates require exactly
    // one, and this is it. Anything else that needs comparing — declarations,
    // signatures, parameters — has its own named predicate above, so no second
    // definition can drift from this one.
    friend bool operator==(const TypeRef& lhs, const TypeRef& rhs) = default;

    bool isError() const { return type == nullptr || type->isError(); }
};

// The poison handle. Never null-and-therefore-forgotten, which is what a bare
// nullptr would be (TYPES.md §7).
TypeRef errorRef();

// --- Naming ------------------------------------------------------------------

// The single naming authority: resolves the whole identity, so a diagnostic
// never prints a treatment name twice. `u8`, `i32`, `*Shape`, `[]u8`, `[u8; 5]`.
std::string typeName(TypeRef ref);

// The size in bytes, from layout.h and nothing else. Answers -1 where no size
// is known: the three unresolved Kinds, FunctionItem (never stored, so never
// asked except by a bug), and nominal types whose fields are not modelled yet.
// -1 rather than a default, because TYPES.md §3.2 forbids defaults.
int64_t typeSize(const Type* type);

// The alignment of a type, from the same table as typeSize. Answers -1 where
// no alignment is known: nominals without field layout, and everything
// typeSize answers -1 for. The resolver walks fields with this plus roundUp;
// nobody else lays anything out.
int64_t alignOf(const Type* type);

}  // namespace cider

#endif  // CIDER_TYPES_TYPE_H
