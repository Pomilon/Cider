// signature.cpp — one comparison for signatures.
//
// Signatures hold interned handles, so comparing two signatures is pointer
// comparison pairwise, plus ABI. No walk, no second rule.

#include <utility>

#include "types/signature.h"
#include "types/type.h"

namespace cider {

bool sameParameter(const Parameter& lhs, const Parameter& rhs) {
    return lhs.type == rhs.type && lhs.isMutable == rhs.isMutable;
}

Signature::Signature(std::vector<Parameter> parameters, const Type* returnType, Abi abi)
    : parameters_(std::move(parameters)), returnType_(returnType), abi_(abi) {}

bool sameSignature(const Signature& lhs, const Signature& rhs) {
    if (lhs.returnType() != rhs.returnType()) {
        return false;
    }
    if (lhs.abi() != rhs.abi()) {
        return false;
    }
    const std::vector<Parameter>& a = lhs.parameters();
    const std::vector<Parameter>& b = rhs.parameters();
    if (a.size() != b.size()) {
        return false;
    }
    for (uint64_t i = 0; i < static_cast<uint64_t>(a.size()); ++i) {
        if (!sameParameter(a[i], b[i])) {
            return false;
        }
    }
    return true;
}

}  // namespace cider
