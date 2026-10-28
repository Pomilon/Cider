// context.cpp — interning decides equality once, at construction.
//
// Every factory validates its arguments, looks for an existing Type that
// matches, and only builds when none does. Sub-handles compare by pointer:
// they are interned already, so pointer equality is structural equality and no
// walk is needed. TYPES.md §2.3.

#include "types/context.h"

namespace cider {

namespace {

bool payloadMatches(Kind kind, const TypePayload& a, const TypePayload& b) {
    if (kind == Kind::Void || kind == Kind::String || kind == Kind::Error ||
        kind == Kind::Wildcard || kind == Kind::FunctionItem ||
        kind == Kind::VaList) {
        return true;
    }
    if (kind == Kind::Basic) {
        return std::get<BasicPayload>(a).basic == std::get<BasicPayload>(b).basic;
    }
    if (kind == Kind::Pointer) {
        return std::get<PointerPayload>(a).pointee == std::get<PointerPayload>(b).pointee;
    }
    if (kind == Kind::Array) {
        const ArrayPayload& x = std::get<ArrayPayload>(a);
        const ArrayPayload& y = std::get<ArrayPayload>(b);
        return x.element == y.element && x.length == y.length;
    }
    if (kind == Kind::Slice) {
        return std::get<SlicePayload>(a).element == std::get<SlicePayload>(b).element;
    }
    if (kind == Kind::Struct || kind == Kind::Class || kind == Kind::Interface ||
        kind == Kind::Union || kind == Kind::Enum) {
        return sameDecl(std::get<NominalPayload>(a).decl, std::get<NominalPayload>(b).decl);
    }
    if (kind == Kind::Closure || kind == Kind::FunctionPtr) {
        return sameSignature(std::get<SignaturePayload>(a).signature,
                             std::get<SignaturePayload>(b).signature);
    }
    if (kind == Kind::TypeParam) {
        const TypeParamPayload& x = std::get<TypeParamPayload>(a);
        const TypeParamPayload& y = std::get<TypeParamPayload>(b);
        return sameDecl(x.decl, y.decl) && x.index == y.index;
    }
    if (kind == Kind::Alias) {
        return sameDecl(std::get<AliasPayload>(a).target, std::get<AliasPayload>(b).target);
    }
    return false;
}

bool nominalKind(Kind kind) {
    return kind == Kind::Struct || kind == Kind::Class || kind == Kind::Interface ||
           kind == Kind::Union || kind == Kind::Enum;
}

}  // namespace

const Type* TypeContext::find(Kind kind, const TypePayload& payload) const {
    for (const auto& item : owned_) {
        if (item->kind() == kind && payloadMatches(kind, item->payload(), payload)) {
            return item.get();
        }
    }
    return nullptr;
}

const Type* TypeContext::store(Type* type) {
    owned_.emplace_back(type);
    return owned_.back().get();
}

const Type* TypeContext::void_() {
    VoidPayload p;
    if (const Type* f = find(Kind::Void, TypePayload(p))) {
        return f;
    }
    return store(new Type(p));
}

const Type* TypeContext::string() {
    StringPayload p;
    if (const Type* f = find(Kind::String, TypePayload(p))) {
        return f;
    }
    return store(new Type(p));
}

const Type* TypeContext::basic(BasicKind b) {
    BasicPayload p;
    p.basic = b;
    if (const Type* f = find(Kind::Basic, TypePayload(p))) {
        return f;
    }
    return store(new Type(p));
}

const Type* TypeContext::pointer(const Type* pointee) {
    if (pointee == nullptr || pointee->isError()) {
        return &Type::poison();
    }
    PointerPayload p;
    p.pointee = pointee;
    if (const Type* f = find(Kind::Pointer, TypePayload(p))) {
        return f;
    }
    return store(new Type(p));
}

const Type* TypeContext::array(const Type* element, int64_t length) {
    if (element == nullptr || element->isError() || length < 0) {
        return &Type::poison();
    }
    ArrayPayload p;
    p.element = element;
    p.length = length;
    if (const Type* f = find(Kind::Array, TypePayload(p))) {
        return f;
    }
    return store(new Type(p));
}

const Type* TypeContext::slice(const Type* element) {
    if (element == nullptr || element->isError()) {
        return &Type::poison();
    }
    SlicePayload p;
    p.element = element;
    if (const Type* f = find(Kind::Slice, TypePayload(p))) {
        return f;
    }
    return store(new Type(p));
}

const Type* TypeContext::nominal(Kind kind, DeclId decl) {
    if (!nominalKind(kind)) {
        return &Type::poison();
    }
    NominalPayload p;
    p.decl = decl;
    if (const Type* f = find(kind, TypePayload(p))) {
        return f;
    }
    return store(new Type(p, kind));
}

const Type* TypeContext::vaList() {
    VaListPayload p;
    if (const Type* f = find(Kind::VaList, TypePayload(p))) {
        return f;
    }
    return store(new Type(p));
}

const Type* TypeContext::functionPtr(Signature signature) {
    SignaturePayload p;
    p.signature = std::move(signature);
    if (const Type* f = find(Kind::FunctionPtr, TypePayload(p))) {
        return f;
    }
    return store(new Type(p, Kind::FunctionPtr));
}

const Type* TypeContext::closure(Signature signature) {
    SignaturePayload p;
    p.signature = std::move(signature);
    if (const Type* f = find(Kind::Closure, TypePayload(p))) {
        return f;
    }
    return store(new Type(p, Kind::Closure));
}

const Type* TypeContext::typeParam(DeclId decl, uint32_t index) {
    TypeParamPayload p;
    p.decl = decl;
    p.index = index;
    if (const Type* f = find(Kind::TypeParam, TypePayload(p))) {
        return f;
    }
    return store(new Type(p));
}

const Type* TypeContext::alias(DeclId target) {
    AliasPayload p;
    p.target = target;
    if (const Type* f = find(Kind::Alias, TypePayload(p))) {
        return f;
    }
    return store(new Type(p));
}

const Type* TypeContext::wildcard() {
    WildcardPayload p;
    if (const Type* f = find(Kind::Wildcard, TypePayload(p))) {
        return f;
    }
    return store(new Type(p));
}

const Type* TypeContext::error() {
    return &Type::poison();
}

const Type* TypeContext::functionItem() {
    FunctionItemPayload p;
    if (const Type* f = find(Kind::FunctionItem, TypePayload(p))) {
        return f;
    }
    return store(new Type(p));
}

uint64_t TypeContext::count() const {
    return static_cast<uint64_t>(owned_.size());
}

}  // namespace cider
