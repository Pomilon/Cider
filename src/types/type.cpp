// type.cpp — what a type is, and its one name.
//
// Kind is derived at construction and never stored beside a payload it could
// disagree with. Every switch over Kind below is exhaustive with no `default:`,
// so a new enumerator breaks the build here rather than silently answering
// wrong — gate T1 plus -Werror=switch.

#include <utility>

#include "types/type.h"

#include "layout.h"
#include "types/context.h"

namespace cider {

bool sameDecl(DeclId lhs, DeclId rhs) {
    return lhs.module == rhs.module && lhs.index == rhs.index;
}

// --- Construction: one constructor per payload, each fixing its Kind --------

Type::Type(VoidPayload payload) : kind_(Kind::Void), payload_(payload) {}
Type::Type(StringPayload payload) : kind_(Kind::String), payload_(payload) {}
Type::Type(BasicPayload payload) : kind_(Kind::Basic), payload_(payload) {}
Type::Type(PointerPayload payload) : kind_(Kind::Pointer), payload_(payload) {}
Type::Type(ArrayPayload payload) : kind_(Kind::Array), payload_(payload) {}
Type::Type(SlicePayload payload) : kind_(Kind::Slice), payload_(payload) {}
Type::Type(NominalPayload payload, Kind kind) : kind_(kind), payload_(payload) {}
Type::Type(SignaturePayload payload, Kind kind) : kind_(kind), payload_(payload) {}
Type::Type(TypeParamPayload payload) : kind_(Kind::TypeParam), payload_(payload) {}
Type::Type(AliasPayload payload) : kind_(Kind::Alias), payload_(payload) {}
Type::Type(WildcardPayload payload) : kind_(Kind::Wildcard), payload_(payload) {}
Type::Type(ErrorPayload payload) : kind_(Kind::Error), payload_(payload) {}
Type::Type(FunctionItemPayload payload) : kind_(Kind::FunctionItem), payload_(payload) {}

const Type& Type::poison() {
    static const Type only(ErrorPayload{});
    return only;
}

// --- Accessors: answer only their own Kind -----------------------------------

std::optional<BasicKind> Type::basic() const {
    if (const auto* p = std::get_if<BasicPayload>(&payload_)) {
        return p->basic;
    }
    return std::nullopt;
}

std::optional<int64_t> Type::length() const {
    if (const auto* p = std::get_if<ArrayPayload>(&payload_)) {
        return p->length;
    }
    return std::nullopt;
}

std::optional<DeclId> Type::decl() const {
    if (const auto* p = std::get_if<NominalPayload>(&payload_)) {
        return p->decl;
    }
    if (const auto* p = std::get_if<TypeParamPayload>(&payload_)) {
        return p->decl;
    }
    if (const auto* p = std::get_if<AliasPayload>(&payload_)) {
        return p->target;
    }
    return std::nullopt;
}

const Type* Type::pointee() const {
    if (const auto* p = std::get_if<PointerPayload>(&payload_)) {
        return p->pointee;
    }
    return &poison();
}

const Type* Type::element() const {
    if (const auto* p = std::get_if<ArrayPayload>(&payload_)) {
        return p->element;
    }
    if (const auto* p = std::get_if<SlicePayload>(&payload_)) {
        return p->element;
    }
    return &poison();
}

const Signature* Type::signature() const {
    if (const auto* p = std::get_if<SignaturePayload>(&payload_)) {
        return &p->signature;
    }
    return nullptr;
}

// --- Handles -----------------------------------------------------------------

TypeRef errorRef() {
    return TypeRef{&Type::poison()};
}

// --- Naming: the single authority --------------------------------------------

namespace {

std::string sigText(const Signature& sig, bool isPointer) {
    std::string out = isPointer ? "*func(" : "func(";
    bool first = true;
    for (const Parameter& param : sig.parameters()) {
        if (!first) {
            out += ", ";
        }
        first = false;
        out += typeName(TypeRef{param.type});
        if (param.isMutable) {
            out += " var";
        }
    }
    out += ")";
    if (sig.returnType() != nullptr) {
        out += " -> ";
        out += typeName(TypeRef{sig.returnType()});
    }
    return out;
}

}  // namespace

std::string typeName(TypeRef ref) {
    const Type* t = ref.type;
    if (t == nullptr || t->isError()) {
        return "error";
    }
    switch (t->kind()) {
        case Kind::Error:
            return "error";
        case Kind::Void:
            return "void";
        case Kind::String:
            return "string";
        case Kind::Basic:
            return basicKindName(*t->basic());
        case Kind::Pointer:
            return "*" + typeName(TypeRef{t->pointee()});
        case Kind::Array: {
            std::string out = "[";
            out += typeName(TypeRef{t->element()});
            out += "; ";
            out += std::to_string(*t->length());
            out += "]";
            return out;
        }
        case Kind::Slice:
            return "[]" + typeName(TypeRef{t->element()});
        case Kind::Struct:
        case Kind::Class:
        case Kind::Interface:
        case Kind::Union:
        case Kind::Enum: {
            const DeclId d = *t->decl();
            std::string out = kindName(t->kind());
            out += "(";
            out += std::to_string(d.module);
            out += ":";
            out += std::to_string(d.index);
            out += ")";
            return out;
        }
        case Kind::Closure:
            return sigText(*t->signature(), false);
        case Kind::FunctionPtr:
            return sigText(*t->signature(), true);
        case Kind::FunctionItem:
            return "function";
        case Kind::TypeParam: {
            const DeclId d = *t->decl();
            std::string out = "T";
            out += std::to_string(d.index);
            return out;
        }
        case Kind::Alias: {
            const DeclId d = *t->decl();
            std::string out = "alias(";
            out += std::to_string(d.module);
            out += ":";
            out += std::to_string(d.index);
            out += ")";
            return out;
        }
        case Kind::Wildcard:
            return "?";
    }
    return "unreachable";
}

// --- Size: every number from layout.h, and -1 where none is known ------------

int64_t typeSize(const Type* t) {
    if (t == nullptr || t->isError()) {
        return -1;
    }
    switch (t->kind()) {
        case Kind::Error:
        case Kind::Alias:
        case Kind::TypeParam:
        case Kind::Wildcard:
        case Kind::FunctionItem:
            return -1;
        case Kind::Void:
            return 0;
        case Kind::String:
            return layout::kSizeString;
        case Kind::Basic:
            switch (*t->basic()) {
                case BasicKind::I8:
                    return layout::kSizeI8;
                case BasicKind::I16:
                    return layout::kSizeI16;
                case BasicKind::I32:
                    return layout::kSizeI32;
                case BasicKind::I64:
                    return layout::kSizeI64;
                case BasicKind::U8:
                    return layout::kSizeI8;
                case BasicKind::U16:
                    return layout::kSizeI16;
                case BasicKind::U32:
                    return layout::kSizeI32;
                case BasicKind::U64:
                    return layout::kSizeI64;
                case BasicKind::F32:
                    return layout::kSizeF32;
                case BasicKind::F64:
                    return layout::kSizeF64;
                case BasicKind::Bool:
                    return layout::kSizeBool;
                case BasicKind::Char:
                    return layout::kSizeChar;
            }
            return -1;
        case Kind::Pointer:
        case Kind::FunctionPtr:
            return layout::kSizePointer;
        case Kind::Slice:
            return layout::kSizeSlice;
        case Kind::Interface:
            return layout::kSizeInterface;
        case Kind::Closure:
            return layout::kSizeClosure;
        case Kind::Enum:
            return layout::kSizeEnum;
        case Kind::Array: {
            const int64_t per = typeSize(t->element());
            if (per < 0) {
                return -1;
            }
            std::optional<int64_t> len = t->length();
            if (!len) {
                return -1;
            }
            return per * *len;
        }
        case Kind::Struct:
        case Kind::Class:
        case Kind::Union:
            return -1;
    }
    return -1;
}

}  // namespace cider
