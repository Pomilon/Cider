// context.h — the owner of every Type. TYPES.md §2.1, §2.3, §2.4.
//
// One context owns all types built while checking one program. It hands out
// `const Type*` handles, and two handles name the same type exactly when they
// are the same pointer: interning decides equality once, at construction, so
// no caller ever walks two types to compare them.
//
// Invalid construction is answered with the poison Type, never with null and
// never with a default that looks real: a null pointee, a negative array
// length, a nominal Kind that is not one of the five nominal Kinds. TYPES.md
// §7 says an Error stays an Error, so a caller that receives one stops.

#ifndef CIDER_TYPES_CONTEXT_H
#define CIDER_TYPES_CONTEXT_H

#include <cstdint>
#include <memory>
#include <vector>

#include "types/kind.h"
#include "types/signature.h"
#include "types/type.h"

namespace cider {

class Type;

class TypeContext {
public:
    TypeContext() = default;
    TypeContext(const TypeContext&) = delete;
    TypeContext& operator=(const TypeContext&) = delete;

    // Every factory below returns an owned handle that stays valid for the
    // life of the context. Calling one twice with equal arguments returns the
    // same pointer both times.
    const Type* void_();
    const Type* string();
    const Type* basic(BasicKind basic);
    const Type* pointer(const Type* pointee);
    const Type* array(const Type* element, int64_t length);
    const Type* slice(const Type* element);
    // kind must be Struct, Class, Interface, Union or Enum; anything else is
    // answered with poison, because a nominal of another Kind is not a thing.
    const Type* nominal(Kind kind, DeclId decl);
    const Type* functionPtr(Signature signature);
    const Type* closure(Signature signature);
    const Type* typeParam(DeclId decl, uint32_t index);
    const Type* alias(DeclId target);
    const Type* wildcard();
    const Type* error();
    const Type* functionItem();

    // How many distinct Types are interned. For tests, which assert that
    // interning reuses rather than duplicates.
    uint64_t count() const;

private:
    // Finds an existing Type whose payload matches, or null. Sub-handles
    // compare by pointer: they are interned already, so pointer equality is
    // structural equality.
    const Type* find(Kind kind, const TypePayload& payload) const;
    const Type* store(Type* type);

    std::vector<std::unique_ptr<Type>> owned_;
};

}  // namespace cider

#endif  // CIDER_TYPES_CONTEXT_H
