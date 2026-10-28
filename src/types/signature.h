// signature.h — the parameter and return types of a function, pointer or closure.
//
// A Signature is shared by three Kinds (FunctionPtr, Closure, FunctionItem) and
// is part of what a Type is rather than how it is treated, so it is held by
// value inside SignaturePayload.
//
// A Signature holds `const Type*` handles and nothing owns anything: the
// TypeContext owns every Type, and a Signature only names them. No deep copy,
// which the checker would otherwise pay for on every signature it compared.

#ifndef CIDER_TYPES_SIGNATURE_H
#define CIDER_TYPES_SIGNATURE_H

#include <cstdint>
#include <vector>

namespace cider {

class Type;

// The calling convention an `extern` declaration states. FEATURES.md §4.4:
// "The calling convention is stated explicitly where it differs from the
// default." An extern function is a FunctionPtr carrying an explicit ABI, so
// there is no separate Extern Kind and this sits on the signature.
enum class Abi {
    SysV,   // the native and vm targets' default
    MsAbi,  // the UEFI target's default
};

// One parameter: its type, and whether the callee may reassign it
// (FEATURES.md §4.1, `func f(var a: T)`).
struct Parameter {
    const Type* type = nullptr;   // interned handle; never null once built
    bool isMutable = false;
};

// True when two parameters name the same type and agree on mutability.
// Parameter types are interned handles, so naming the same type means holding
// the same pointer — no deep walk.
bool sameParameter(const Parameter& lhs, const Parameter& rhs);

// What a function, function pointer or closure is: its parameters in order,
// its return type, its ABI, and whether it takes a C variadic tail
// (VARIADICS.md §3). A null return type means `void`, which is how
// `func f()` and `func f() -> void` end up one spelling rather than two.
class Signature {
public:
    Signature() = default;
    Signature(std::vector<Parameter> parameters, const Type* returnType, Abi abi = Abi::SysV,
              bool variadic = false);

    const std::vector<Parameter>& parameters() const { return parameters_; }
    const Type* returnType() const { return returnType_; }
    Abi abi() const { return abi_; }
    bool variadic() const { return variadic_; }
    uint64_t arity() const { return static_cast<uint64_t>(parameters_.size()); }

private:
    std::vector<Parameter> parameters_;
    const Type* returnType_ = nullptr;   // null means void
    Abi abi_ = Abi::SysV;
    bool variadic_ = false;
};

// True when two signatures take the same parameters pairwise in order, return
// the same type, agree on ABI, and agree on the variadic bit (VARIADICS.md §2:
// a variadic pointer is not compatible with a non-variadic pointer). Written
// once here; every site that compares
// two signatures calls it. TYPES.md §2.3 needs it for interface identity: two
// interfaces with identical method names and different signatures are different
// interfaces, and that test is a signature comparison.
bool sameSignature(const Signature& lhs, const Signature& rhs);

}  // namespace cider

#endif  // CIDER_TYPES_SIGNATURE_H
