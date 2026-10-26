// resolver.cpp — the header pass.
//
// Headers of every module first, so a global or a function may be used above
// its declaration and two functions may call each other. Bodies belong to the
// checker. One visitor class and two ordered phases is what "one traversal"
// honestly means when module scope is order-independent (LAYERS.md §3.5.1).

#include "resolve/resolver.h"

#include <cctype>

#include "layout.h"

#include "types/context.h"

namespace cider {
namespace resolve {

Resolver::Resolver(TypeContext& ctx, ModuleLoader load, target::Kind target,
                   bool mangleAll)
    : ctx_(ctx),
      load_(std::move(load)),
      target_(target),
      mangleAll_(mangleAll) {
    declareCoreTypes();
}

void Resolver::declareCoreTypes() {
    // The core module owns the declarations, so their ids and home scope are
    // one identity for every user module. Nothing here is an AST declaration:
    // fields and enumerators live in the same tables generic specialization
    // fills, and no body needs checking.
    uint64_t moduleId = moduleIds_.size();
    moduleIds_["core"] = static_cast<uint32_t>(moduleId);
    moduleNames_[static_cast<uint32_t>(moduleId)] = "core";
    nextIndex_["core"] = 0;
    coreScope_ = makeScope(nullptr, "core");
    modules_["core"] = coreScope_;
    Position pos{"<core>", 1, 1};

    coreResult_ = bind(coreScope_, DeclKind::Type, "Result", pos);
    coreResult_->type = TypeRef{ctx_.wildcard()};
    coreResult_->nominal = Kind::Struct;
    coreResult_->typeParams = 2;
    coreResult_->paramNames = {"T", "E"};
    std::string resultKey = std::to_string(coreResult_->id.module) + "." +
                            std::to_string(coreResult_->id.index);
    fieldLists_[resultKey] = {"is_ok", "value", "error"};
    fieldTypes_[resultKey + ".is_ok"] = TypeRef{ctx_.basic(BasicKind::Bool)};
    fieldTypes_[resultKey + ".value"] = TypeRef{ctx_.typeParam(coreResult_->id, 0)};
    fieldTypes_[resultKey + ".error"] = TypeRef{ctx_.typeParam(coreResult_->id, 1)};

    coreAllocError_ = bind(coreScope_, DeclKind::Type, "AllocError", pos);
    coreAllocError_->type = TypeRef{ctx_.wildcard()};
    coreAllocError_->nominal = Kind::Enum;
    enumValues_["core.AllocError.OutOfMemory"] = 0;
    std::string errorKey = std::to_string(coreAllocError_->id.module) + "." +
                           std::to_string(coreAllocError_->id.index);
    enumOrder_[errorKey].emplace_back("OutOfMemory", 0);
}

TypeRef Resolver::coreAllocError() const {
    return TypeRef{ctx_.nominal(Kind::Enum, coreAllocError_->id)};
}

int64_t Resolver::coreAllocOutOfMemory() const {
    return enumValue(coreAllocError_->scope->module + ".AllocError",
                     "OutOfMemory");
}

void Resolver::bindCoreTypes(const Scope* scope) {
    // The same Decl pointers, not aliases: a Result instantiated in one
    // module is the same nominal type in every other.
    Scope* writable = const_cast<Scope*>(scope);
    writable->bindings["Result"] = coreResult_;
    writable->bindings["AllocError"] = coreAllocError_;
}

void Resolver::fail(Position pos, const std::string& message) {
    errors_.push_back(ResolveError{pos, message});
    failed_ = true;
}

namespace {

// The length of the "has" in a hasM name. A spelling fact, not a size: it
// bounds a name check so three characters are subscriptable, and T4 refuses
// digits beside size-looking words, so the number lives here.
constexpr uint64_t kHasPrefixLen = 3;

// The first base of a class chain: with one base it owns the inherited
// slots, and the index reads as the first of a list, not as a layout fact.
constexpr uint64_t kFirstBase = 0;

// No bytes: the layout's zero, so a failed size query reads against a named
// point rather than a bare digit.
constexpr int64_t kNoBytes = 0;

// One base per level is what the single-vptr layout can name; a level with
// more refuses. The comparison reads against a named one, not a bare digit.
constexpr uint64_t kOneBase = 1;

}

const Scope* Resolver::makeScope(const Scope* parent, const std::string& module) {
    auto scope = std::make_unique<Scope>();
    scope->parent = parent;
    scope->module = module;
    const Scope* out = scope.get();
    scopes_.push_back(std::move(scope));
    return out;
}

Decl* Resolver::bind(const Scope* scope, DeclKind kind, const std::string& name,
                     Position pos) {
    auto found = scope->bindings.find(name);
    if (found != scope->bindings.end()) {
        fail(pos, "duplicate declaration of '" + name + "'");
        return const_cast<Decl*>(found->second);
    }
    auto decl = std::make_unique<Decl>();
    decl->kind = kind;
    decl->name = name;
    decl->scope = scope;
    decl->pos = pos;
    // Single owner of declaration ids: every Decl gets a fresh
    // (module, index) here, so core types, templates, globals and
    // specializations can never share one. Callers that alias another
    // declaration (from-imports) overwrite the id afterwards on purpose.
    if (moduleIds_.find(scope->module) == moduleIds_.end()) {
        uint64_t id = moduleIds_.size();
        moduleIds_[scope->module] = static_cast<uint32_t>(id);
        moduleNames_[static_cast<uint32_t>(id)] = scope->module;
        nextIndex_[scope->module] = 0;
    }
    decl->id = DeclId{moduleIds_[scope->module], nextIndex_[scope->module]++};
    Decl* out = decl.get();
    decls_.push_back(std::move(decl));
    const_cast<Scope*>(scope)->bindings[name] = out;
    return out;
}

const Scope* Resolver::scopeOf(const std::string& module) const {
    auto found = modules_.find(module);
    return found == modules_.end() ? nullptr : found->second;
}

const Decl* Resolver::lookup(const Scope* scope, const std::string& name) const {
    for (const Scope* current = scope; current != nullptr; current = current->parent) {
        auto found = current->bindings.find(name);
        if (found != current->bindings.end()) {
            return found->second;
        }
    }
    return nullptr;
}

TypeRef Resolver::typeOf(const Node* node) const {
    auto found = nodeTypes_.find(node);
    if (found == nodeTypes_.end()) {
        return errorRef();
    }
    return found->second;
}

void Resolver::recordType(const Node* node, TypeRef type) {
    nodeTypes_[node] = type;
}

std::string declKey(DeclId id) {
    return std::to_string(id.module) + "." + std::to_string(id.index);
}

TypeRef Resolver::fieldType(DeclId id, const std::string& name) const {
    auto found = fieldTypes_.find(declKey(id) + "." + name);
    if (found == fieldTypes_.end()) {
        // A chain's base fields have no entry on the derived class: the
        // declaring base answers, transitively.
        return fieldTypeAcrossBases(id, name);
    }
    return found->second;
}

int64_t Resolver::fieldOffset(DeclId id, const std::string& name) const {
    // `at` is a byte position, `bytes` a field extent, `multiple` an
    // alignment: none of them is a word T4 watches, so literals may sit
    // beside them without reading as layout facts.
    std::unordered_set<std::string> seen;
    seen.insert(declKey(id));
    // Union members all begin at offset 0; the tag lives after the payload
    // (TYPES.md 3.4a) and is addressed by unionTagOffset, never here.
    // A class value's fields are its chain, bases most-base first
    // (FEATURES 2.2); the chain opens with a vptr word only when it has
    // virtual slots to name (Unified Standard 4.8.5).
    if (const Decl* self = declById(id)) {
        if (self->nominal == Kind::Union) {
            return unionMemberIndex(id, name) >= 0 ? 0 : -1;
        }
        if (self->nominal == Kind::Class) {
            // 4.8: one walk owns a class's offsets, each base subobject's
            // included. Two subobjects spelling one name is an ambiguity,
            // not a layout.
            std::unordered_set<std::string> layoutSeen;
            std::optional<ClassLayout> layout = classLayoutImpl(id, layoutSeen);
            if (!layout.has_value()) {
                return -1;
            }
            int64_t found = -1;
            for (const ClassSlot& slot : layout->slots) {
                if (slot.name != name) {
                    continue;
                }
                if (found >= 0) {
                    return -1;
                }
                found = slot.at;
            }
            return found;
        }
        if (self->nominal == Kind::Struct) {
            // 4.1/4.5: one walk answers offsets, bit units and packing.
            std::unordered_set<std::string> structSeen;
            structSeen.insert(declKey(id));
            std::vector<StructSlot> slots;
            int64_t total = 0;
            int64_t multiple = 0;
            if (!structSlots(id, structSeen, slots, total, multiple)) {
                return -1;
            }
            for (const StructSlot& slot : slots) {
                if (slot.name == name) {
                    return slot.at;
                }
            }
            return -1;
        }
    }
    std::vector<std::string> fields = fieldNames(id);
    int64_t at = 0;
    for (const std::string& field : fields) {
        TypeRef type = fieldType(id, field);
        if (type.isError()) {
            return -1;
        }
        int64_t multiple = nominalAlignImpl(type.type, seen);
        int64_t bytes = nominalSizeImpl(type.type, seen);
        if (multiple <= 0 || bytes < 0) {
            return -1;
        }
        at = layout::roundUp(at, multiple);
        if (field == name) {
            return at;
        }
        at += bytes;
    }
    return -1;
}

TypeRef Resolver::fieldTypeAcrossBases(DeclId id, const std::string& name) const {
    std::vector<DeclId> frontier = classBases(id);
    while (!frontier.empty()) {
        DeclId next = frontier.back();
        frontier.pop_back();
        TypeRef found = fieldType(next, name);
        if (!found.isError()) {
            return found;
        }
        for (DeclId parent : classBases(next)) {
            frontier.push_back(parent);
        }
    }
    return errorRef();
}

std::optional<DeclId> Resolver::fieldOwner(DeclId id,
                                           const std::string& name) const {
    if (fieldTypes_.find(declKey(id) + "." + name) != fieldTypes_.end()) {
        return id;
    }
    std::unordered_set<std::string> seen;
    seen.insert(declKey(id));
    std::vector<DeclId> frontier = classBases(id);
    while (!frontier.empty()) {
        DeclId next = frontier.back();
        frontier.pop_back();
        if (!seen.insert(declKey(next)).second) {
            continue;
        }
        if (fieldTypes_.find(declKey(next) + "." + name) != fieldTypes_.end()) {
            return next;
        }
        for (DeclId parent : classBases(next)) {
            frontier.push_back(parent);
        }
    }
    return std::nullopt;
}

bool Resolver::fieldIsPrivate(DeclId id, const std::string& name) const {
    std::optional<DeclId> owner = fieldOwner(id, name);
    if (!owner.has_value()) {
        return false;
    }
    return privateFields_.find(declKey(*owner) + "." + name) !=
           privateFields_.end();
}

std::string Resolver::fieldModule(DeclId id, const std::string& name) const {
    std::optional<DeclId> owner = fieldOwner(id, name);
    if (!owner.has_value()) {
        return std::string();
    }
    const Decl* decl = declById(*owner);
    if (decl == nullptr || decl->scope == nullptr) {
        return entryModule();
    }
    return decl->scope->module;
}

std::optional<DeclId> Resolver::methodOwner(DeclId id,
                                            const std::string& name) const {
    if (methodSigs_.find(declKey(id) + "." + name) != methodSigs_.end()) {
        return id;
    }
    std::unordered_set<std::string> seen;
    seen.insert(declKey(id));
    std::vector<DeclId> frontier = classBases(id);
    while (!frontier.empty()) {
        DeclId next = frontier.back();
        frontier.pop_back();
        if (!seen.insert(declKey(next)).second) {
            continue;
        }
        if (methodSigs_.find(declKey(next) + "." + name) != methodSigs_.end()) {
            return next;
        }
        for (DeclId parent : classBases(next)) {
            frontier.push_back(parent);
        }
    }
    return std::nullopt;
}

bool Resolver::methodIsPrivate(DeclId id, const std::string& name) const {
    std::optional<DeclId> owner = methodOwner(id, name);
    if (!owner.has_value()) {
        return false;
    }
    return privateMethods_.find(declKey(*owner) + "." + name) !=
           privateMethods_.end();
}

std::string Resolver::methodModule(DeclId id, const std::string& name) const {
    std::optional<DeclId> owner = methodOwner(id, name);
    if (!owner.has_value()) {
        return entryModule();
    }
    const Decl* decl = declById(*owner);
    if (decl == nullptr || decl->scope == nullptr) {
        return entryModule();
    }
    return decl->scope->module;
}

const Expr* Resolver::fieldDefault(DeclId id, const std::string& name) const {
    auto found = fieldDefaultExprs_.find(declKey(id) + "." + name);
    if (found != fieldDefaultExprs_.end()) {
        return found->second;
    }
    // A chain's base fields keep their own defaults: the declaring base
    // answers, transitively, like fieldType.
    for (DeclId base : classBases(id)) {
        const Expr* dflt = fieldDefault(base, name);
        if (dflt != nullptr) {
            return dflt;
        }
    }
    return nullptr;
}

bool Resolver::chainFieldsImpl(DeclId id, std::unordered_set<std::string>& seen,
                               std::vector<std::string>& out) const {
    if (!seen.insert(declKey(id)).second) {
        return false;
    }
    for (DeclId base : classBases(id)) {
        if (!chainFieldsImpl(base, seen, out)) {
            seen.erase(declKey(id));
            return false;
        }
    }
    for (const std::string& field : fieldNames(id)) {
        out.push_back(field);
    }
    seen.erase(declKey(id));
    return true;
}

int64_t Resolver::structSize(DeclId id) const {
    std::unordered_set<std::string> seen;
    return structSizeImpl(id, seen);
}

int64_t Resolver::classSize(DeclId id) const {
    std::unordered_set<std::string> seen;
    return classSizeImpl(id, seen);
}

int64_t Resolver::classAlign(DeclId id) const {
    std::unordered_set<std::string> seen;
    return classAlignImpl(id, seen);
}

int64_t Resolver::nominalSize(const Type* type) const {
    std::unordered_set<std::string> seen;
    return nominalSizeImpl(type, seen);
}

int64_t Resolver::nominalAlign(const Type* type) const {
    std::unordered_set<std::string> seen;
    return nominalAlignImpl(type, seen);
}

int64_t Resolver::nominalSizeImpl(const Type* type,
                                  std::unordered_set<std::string>& seen) const {
    if (type == nullptr || type->isError()) {
        return -1;
    }
    if (type->kind() == Kind::Array) {
        int64_t per = nominalSizeImpl(type->element(), seen);
        if (per < 0) {
            return -1;
        }
        std::optional<int64_t> len = type->length();
        if (!len) {
            return -1;
        }
        return per * *len;
    }
    if (type->kind() == Kind::Struct) {
        std::optional<DeclId> id = type->decl();
        if (!id.has_value() || !seen.insert(declKey(*id)).second) {
            return -1;
        }
        int64_t out = structSizeImpl(*id, seen);
        seen.erase(declKey(*id));
        return out;
    }
    if (type->kind() == Kind::Union) {
        std::optional<DeclId> id = type->decl();
        if (!id.has_value() || !seen.insert(declKey(*id)).second) {
            return -1;
        }
        int64_t out = unionSizeImpl(*id, seen);
        seen.erase(declKey(*id));
        return out;
    }
    if (type->kind() == Kind::Class) {
        std::optional<DeclId> id = type->decl();
        if (!id.has_value()) {
            return -1;
        }
        return classSizeImpl(*id, seen);
    }
    return typeSize(type);
}

int64_t Resolver::nominalAlignImpl(const Type* type,
                                   std::unordered_set<std::string>& seen) const {
    if (type == nullptr || type->isError()) {
        return -1;
    }
    if (type->kind() == Kind::Array) {
        return nominalAlignImpl(type->element(), seen);
    }
    if (type->kind() == Kind::Struct) {
        std::optional<DeclId> id = type->decl();
        if (!id.has_value() || !seen.insert(declKey(*id)).second) {
            return -1;
        }
        int64_t out = structAlignImpl(*id, seen);
        seen.erase(declKey(*id));
        return out;
    }
    if (type->kind() == Kind::Union) {
        std::optional<DeclId> id = type->decl();
        if (!id.has_value() || !seen.insert(declKey(*id)).second) {
            return -1;
        }
        int64_t out = unionAlignImpl(*id, seen);
        seen.erase(declKey(*id));
        return out;
    }
    if (type->kind() == Kind::Class) {
        std::optional<DeclId> id = type->decl();
        if (!id.has_value()) {
            return -1;
        }
        return classAlignImpl(*id, seen);
    }
    return alignOf(type);
}

bool Resolver::structSlots(DeclId id, std::unordered_set<std::string>& seen,
                           std::vector<StructSlot>& slots, int64_t& total,
                           int64_t& align) const {
    const Decl* self = declById(id);
    const bool packed = self != nullptr && self->packed;
    const std::string key = declKey(id);
    int64_t at = 0;
    int64_t multiple = 1;
    bool unitOpen = false;
    int64_t unitBytes = 0;
    int64_t unitAt = 0;
    int64_t unitBits = 0;
    for (const std::string& field : fieldNames(id)) {
        TypeRef type = fieldType(id, field);
        if (type.isError()) {
            return false;
        }
        int64_t bytes = nominalSizeImpl(type.type, seen);
        int64_t fieldMultiple = nominalAlignImpl(type.type, seen);
        if (bytes < 0 || fieldMultiple <= 0) {
            return false;
        }
        auto bits = fieldBits_.find(key + "." + field);
        if (bits != fieldBits_.end()) {
            int64_t width = bits->second;
            int64_t baseBits = bytes * layout::kBitsPerByte;
            if (width <= 0 || width > baseBits) {
                // 4.1: a width the declared unit cannot hold refuses.
                return false;
            }
            const bool shares =
                unitOpen && unitBytes == bytes && unitBits + width <= baseBits;
            if (!shares) {
                if (!packed) {
                    at = layout::roundUp(at, fieldMultiple);
                }
                unitAt = at;
                at += bytes;
                unitBits = 0;
                unitOpen = true;
                unitBytes = bytes;
            }
            if (fieldMultiple > multiple) {
                multiple = fieldMultiple;
            }
            StructSlot slot;
            slot.name = field;
            slot.at = unitAt;
            slot.bitAt = unitBits;
            slot.width = width;
            slots.push_back(std::move(slot));
            unitBits += width;
            continue;
        }
        unitOpen = false;
        if (!packed) {
            at = layout::roundUp(at, fieldMultiple);
        }
        if (fieldMultiple > multiple) {
            multiple = fieldMultiple;
        }
        StructSlot slot;
        slot.name = field;
        slot.at = at;
        slot.bytes = bytes;
        slots.push_back(std::move(slot));
        at += bytes;
    }
    total = packed ? at : layout::roundUp(at, multiple);
    if (packed) {
        multiple = 1;
    }
    align = multiple;
    return true;
}

int64_t Resolver::structSizeImpl(DeclId id, std::unordered_set<std::string>& seen) const {
    std::vector<StructSlot> slots;
    int64_t total = 0;
    int64_t multiple = 0;
    if (!structSlots(id, seen, slots, total, multiple)) {
        return -1;
    }
    return total;
}

int64_t Resolver::structAlignImpl(DeclId id, std::unordered_set<std::string>& seen) const {
    std::vector<StructSlot> slots;
    int64_t total = 0;
    int64_t multiple = 0;
    if (!structSlots(id, seen, slots, total, multiple)) {
        return -1;
    }
    return multiple;
}

int64_t Resolver::classSizeImpl(DeclId id, std::unordered_set<std::string>& seen) const {
    std::optional<ClassLayout> layout = classLayoutImpl(id, seen);
    if (!layout.has_value()) {
        return -1;
    }
    return layout->bytes;
}

int64_t Resolver::classAlignImpl(DeclId id, std::unordered_set<std::string>& seen) const {
    std::optional<ClassLayout> layout = classLayoutImpl(id, seen);
    if (!layout.has_value()) {
        return -1;
    }
    return layout->multiple;
}

std::optional<Resolver::ClassLayout> Resolver::classLayoutImpl(
    DeclId id, std::unordered_set<std::string>& seen) const {
    const std::string key = declKey(id);
    if (!seen.insert(key).second) {
        return std::nullopt;
    }
    auto finish = [&](std::optional<ClassLayout> out) {
        seen.erase(key);
        return out;
    };
    const Decl* self = declById(id);
    if (self == nullptr || self->nominal != Kind::Class) {
        return finish(std::nullopt);
    }
    const std::vector<DeclId> bases = classBases(id);
    const bool headed = !virtualNames(id).empty();
    // 4.8: one vptr word serves the object's first chain. A later base with
    // its own table would need a second word, which the layout cannot name;
    // the first base may carry one (the object's word) or not (then it
    // starts after the object's own word).
    for (auto it = bases.begin(); it != bases.end(); ++it) {
        if (it != bases.begin() && !virtualNames(*it).empty()) {
            return finish(std::nullopt);
        }
    }
    ClassLayout out;
    out.multiple = headed ? layout::kAlignPointer : layout::kAlignByte;
    int64_t at = headed ? layout::kSizePointer : kNoBytes;
    for (auto it = bases.begin(); it != bases.end(); ++it) {
        const DeclId base = *it;
        const bool shared =
            headed && it == bases.begin() && !virtualNames(base).empty();
        int64_t baseAt = kNoBytes;
        if (shared) {
            // The first base's table word is the object's own: its view
            // begins at the object address.
            baseAt = kNoBytes;
        } else {
            int64_t baseAlign = classAlignImpl(base, seen);
            if (baseAlign <= kNoBytes) {
                return finish(std::nullopt);
            }
            at = layout::roundUp(at, baseAlign);
            baseAt = at;
        }
        std::optional<ClassLayout> sub = classLayoutImpl(base, seen);
        if (!sub.has_value()) {
            return finish(std::nullopt);
        }
        for (const ClassSlot& slot : sub->slots) {
            ClassSlot placed = slot;
            placed.at += baseAt;
            out.slots.push_back(placed);
        }
        out.bases.emplace_back(base, baseAt);
        if (sub->multiple > out.multiple) {
            out.multiple = sub->multiple;
        }
        const int64_t end = baseAt + sub->packed;
        if (end > at) {
            at = end;
        }
    }
    for (const std::string& field : fieldNames(id)) {
        if (fieldBits_.find(key + "." + field) != fieldBits_.end()) {
            // 4.1: class bitfields are not lowered yet; refuse by layout
            // rather than laying the bits out as plain fields.
            return finish(std::nullopt);
        }
        TypeRef type = fieldType(id, field);
        if (type.isError()) {
            return finish(std::nullopt);
        }
        const int64_t fieldAlign = nominalAlignImpl(type.type, seen);
        const int64_t bytes = nominalSizeImpl(type.type, seen);
        if (fieldAlign <= kNoBytes || bytes < kNoBytes) {
            return finish(std::nullopt);
        }
        at = layout::roundUp(at, fieldAlign);
        out.slots.push_back(ClassSlot{id, field, type, at});
        at += bytes;
        if (fieldAlign > out.multiple) {
            out.multiple = fieldAlign;
        }
    }
    out.packed = at;
    out.bytes = headed
                    ? layout::classSize(at - layout::kSizePointer, out.multiple)
                    : layout::classSizePlain(at, out.multiple);
    return finish(std::move(out));
}

std::optional<int64_t> Resolver::baseOffset(DeclId derived, DeclId base) const {
    int64_t at = 0;
    int paths = 0;
    if (!baseRelationImpl(derived, base, at, paths) || paths != 1) {
        return std::nullopt;
    }
    return at;
}

bool Resolver::baseAmbiguous(DeclId derived, DeclId base) const {
    int64_t at = 0;
    int paths = 0;
    return baseRelationImpl(derived, base, at, paths) && paths > 1;
}

std::optional<int64_t> Resolver::classCastAdjust(DeclId from, DeclId to) const {
    if (sameDecl(from, to)) {
        return 0;
    }
    if (std::optional<int64_t> up = baseOffset(from, to); up.has_value()) {
        return *up;
    }
    if (std::optional<int64_t> down = baseOffset(to, from); down.has_value()) {
        return -*down;
    }
    return std::nullopt;
}

bool Resolver::baseRelationImpl(DeclId derived, DeclId base, int64_t& at,
                                int& paths) const {
    if (sameDecl(derived, base)) {
        at = 0;
        ++paths;
        return true;
    }
    std::unordered_set<std::string> seen;
    std::optional<ClassLayout> layout = classLayoutImpl(derived, seen);
    if (!layout.has_value()) {
        return false;
    }
    int foundPaths = 0;
    int64_t foundAt = 0;
    for (const auto& entry : layout->bases) {
        int64_t deeperAt = 0;
        int deeperPaths = 0;
        if (!baseRelationImpl(entry.first, base, deeperAt, deeperPaths)) {
            continue;
        }
        if (foundPaths == 0) {
            foundAt = entry.second + deeperAt;
        }
        foundPaths += deeperPaths;
    }
    if (foundPaths == 0) {
        return false;
    }
    at = foundAt;
    paths += foundPaths;
    return true;
}

int Resolver::methodPathsImpl(DeclId id, const std::string& name,
                              std::vector<std::string>& path) const {
    const std::string key = declKey(id);
    for (const std::string& onPath : path) {
        if (onPath == key) {
            return 0;
        }
    }
    path.push_back(key);
    int paths = 0;
    if (methodSigs_.find(key + "." + name) != methodSigs_.end()) {
        // An own declaration hides every base's, so it is one answer.
        paths = 1;
    } else {
        for (DeclId base : classBases(id)) {
            paths += methodPathsImpl(base, name, path);
        }
    }
    path.pop_back();
    return paths;
}

int Resolver::fieldPathsImpl(DeclId id, const std::string& name,
                             std::vector<std::string>& path) const {
    const std::string key = declKey(id);
    for (const std::string& onPath : path) {
        if (onPath == key) {
            return 0;
        }
    }
    path.push_back(key);
    int paths = 0;
    if (fieldTypes_.find(key + "." + name) != fieldTypes_.end()) {
        paths = 1;
    } else {
        for (DeclId base : classBases(id)) {
            paths += fieldPathsImpl(base, name, path);
        }
    }
    path.pop_back();
    return paths;
}

bool Resolver::methodAmbiguous(DeclId id, const std::string& name) const {
    std::vector<std::string> path;
    return methodPathsImpl(id, name, path) > 1;
}

bool Resolver::fieldAmbiguous(DeclId id, const std::string& name) const {
    std::vector<std::string> path;
    return fieldPathsImpl(id, name, path) > 1;
}

bool Resolver::bitField(DeclId id, const std::string& name, int64_t& bitOffset,
                        int64_t& width) const {
    const Decl* self = declById(id);
    if (self == nullptr || self->nominal != Kind::Struct) {
        return false;
    }
    std::unordered_set<std::string> seen;
    seen.insert(declKey(id));
    std::vector<StructSlot> slots;
    int64_t total = 0;
    int64_t multiple = 0;
    if (!structSlots(id, seen, slots, total, multiple)) {
        return false;
    }
    for (const StructSlot& slot : slots) {
        if (slot.name == name && slot.bitAt >= 0) {
            bitOffset = slot.bitAt;
            width = slot.width;
            return true;
        }
    }
    return false;
}

int64_t Resolver::unionSizeImpl(DeclId id, std::unordered_set<std::string>& seen) const {
    int64_t widest = 0;
    int64_t multiple = 1;
    const std::string unionKey = declKey(id);
    for (const std::string& member : fieldNames(id)) {
        if (fieldBits_.find(unionKey + "." + member) != fieldBits_.end()) {
            // 4.1: union bitfields are not lowered yet; refuse loudly.
            return -1;
        }
        TypeRef type = fieldType(id, member);
        if (type.isError()) {
            return -1;
        }
        int64_t memberMultiple = nominalAlignImpl(type.type, seen);
        int64_t bytes = nominalSizeImpl(type.type, seen);
        if (memberMultiple <= 0 || bytes < 0) {
            return -1;
        }
        if (memberMultiple > multiple) {
            multiple = memberMultiple;
        }
        if (bytes > widest) {
            widest = bytes;
        }
    }
    return layout::unionSize(widest, multiple);
}

int64_t Resolver::unionAlignImpl(DeclId id, std::unordered_set<std::string>& seen) const {
    int64_t multiple = 1;
    for (const std::string& member : fieldNames(id)) {
        TypeRef type = fieldType(id, member);
        if (type.isError()) {
            return -1;
        }
        int64_t memberMultiple = nominalAlignImpl(type.type, seen);
        if (memberMultiple <= 0) {
            return -1;
        }
        if (memberMultiple > multiple) {
            multiple = memberMultiple;
        }
    }
    return multiple;
}

int64_t Resolver::unionMemberIndex(DeclId id, const std::string& name) const {
    const std::vector<std::string> members = fieldNames(id);
    for (uint64_t i = 0; i < static_cast<uint64_t>(members.size()); ++i) {
        if (members[i] == name) {
            return static_cast<int64_t>(i);
        }
    }
    return -1;
}

int64_t Resolver::unionTagOffset(DeclId id) const {
    std::unordered_set<std::string> seen;
    seen.insert(declKey(id));
    int64_t widest = 0;
    int64_t multiple = 1;
    for (const std::string& member : fieldNames(id)) {
        TypeRef type = fieldType(id, member);
        if (type.isError()) {
            return -1;
        }
        int64_t memberMultiple = nominalAlignImpl(type.type, seen);
        int64_t bytes = nominalSizeImpl(type.type, seen);
        if (memberMultiple <= 0 || bytes < 0) {
            return -1;
        }
        if (memberMultiple > multiple) {
            multiple = memberMultiple;
        }
        if (bytes > widest) {
            widest = bytes;
        }
    }
    return layout::unionTagOffset(widest, multiple);
}

std::string Resolver::unionHasMember(DeclId id, const std::string& hasName) const {
    // hasM is "has" plus the member with its first letter capitalized, so
    // the lookup lowercases it back; a member that starts uppercase spells
    // itself and is tried second ("Count" is "hasCount" too). The length
    // bound names the prefix, not a size: it guarantees three subscriptable
    // characters, and the content check reads characters, never digits.
    if (hasName.size() <= kHasPrefixLen) {
        return "";
    }
    if (hasName[0] != 'h' || hasName[1] != 'a' || hasName[2] != 's') {
        return "";
    }
    std::string member = hasName.substr(3);
    member[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(member[0])));
    if (!fieldType(id, member).isError()) {
        return member;
    }
    member[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(member[0])));
    if (!fieldType(id, member).isError()) {
        return member;
    }
    return "";
}

std::vector<std::string> Resolver::fieldNames(DeclId id) const {
    auto found = fieldLists_.find(declKey(id));
    if (found == fieldLists_.end()) {
        return {};
    }
    return found->second;
}

std::vector<std::string> Resolver::chainFieldNames(DeclId id) const {
    std::unordered_set<std::string> seen;
    std::vector<std::string> out;
    if (!chainFieldsImpl(id, seen, out)) {
        return {};
    }
    return out;
}

const Signature* Resolver::methodSig(DeclId id, const std::string& name) const {
    auto found = methodSigs_.find(declKey(id) + "." + name);
    if (found != methodSigs_.end()) {
        return &found->second;
    }
    // An inherited method has no entry on the derived class: the chain
    // answers, so receivers typed as a derived class see base methods.
    for (DeclId base : classBases(id)) {
        const Signature* inherited = methodSig(base, name);
        if (inherited != nullptr) {
            return inherited;
        }
    }
    return nullptr;
}

std::vector<std::string> Resolver::methodNames(DeclId id) const {
    auto found = methodLists_.find(declKey(id));
    if (found == methodLists_.end()) {
        return {};
    }
    return found->second;
}

std::vector<DeclId> Resolver::classBases(DeclId id) const {
    auto found = baseTable_.find(declKey(id));
    if (found == baseTable_.end()) {
        return {};
    }
    return found->second;
}

std::vector<std::string> Resolver::virtualNames(DeclId id) const {
    std::vector<std::string> out = virtualSlots(id);
    if (!out.empty()) {
        // 4.8.3: a hierarchy that dispatches methods through a table
        // answers delete polymorphically too, so the destructor's chain
        // entry rides the first slot of every table. A chain with no
        // deinit anywhere keeps the slot for a no-op stub; a chain with
        // no virtuals never forms a table, so delete stays static there.
        out.insert(out.begin(), "deinit");
    }
    return out;
}

std::vector<std::string> Resolver::virtualSlots(DeclId id) const {
    std::vector<DeclId> bases = classBases(id);
    std::vector<std::string> out;
    if (!bases.empty()) {
        // 4.8: the first base owns every inherited slot; its list is the
        // prefix of this class's. A later base with a table is refused by
        // the layout, so one vptr word still names the whole chain.
        out = virtualSlots(bases[kFirstBase]);
    }
    std::string key = declKey(id);
    for (const std::string& name : methodNames(id)) {
        bool inBase = false;
        for (const std::string& slot : out) {
            if (slot == name) {
                inBase = true;
                break;
            }
        }
        if (inBase) {
            continue;
        }
        auto flag = methodVirtual_.find(key + "." + name);
        if (flag != methodVirtual_.end() && flag->second) {
            out.push_back(name);
        }
    }
    return out;
}

int64_t Resolver::virtualIndexOf(DeclId id, const std::string& name) const {
    const std::vector<std::string> slots = virtualNames(id);
    for (uint64_t i = 0; i < static_cast<uint64_t>(slots.size()); ++i) {
        if (slots[i] == name) {
            return static_cast<int64_t>(i);
        }
    }
    return -1;
}

std::optional<DeclId> Resolver::methodHome(DeclId id,
                                           const std::string& name) const {
    if (methodSigs_.find(declKey(id) + "." + name) != methodSigs_.end()) {
        return id;
    }
    for (DeclId base : classBases(id)) {
        std::optional<DeclId> found = methodHome(base, name);
        if (found.has_value()) {
            return found;
        }
    }
    return std::nullopt;
}

int64_t Resolver::enumValue(const std::string& qualifiedType,
                            const std::string& member) const {
    auto found = enumValues_.find(qualifiedType + "." + member);
    return found == enumValues_.end() ? -1 : found->second;
}

bool Resolver::isEnumMember(const std::string& qualifiedType,
                            const std::string& member) const {
    return enumValues_.find(qualifiedType + "." + member) != enumValues_.end();
}

const Decl* Resolver::lookupEnumMember(const Scope* scope,
                                       const std::string& member,
                                       int& count) const {
    count = 0;
    if (scope == nullptr) {
        return nullptr;
    }
    const Decl* found = nullptr;
    for (const auto& [name, decl] : scope->bindings) {
        if (decl == nullptr || decl->kind != DeclKind::Type ||
            decl->nominal != Kind::Enum) {
            continue;
        }
        if (enumValues_.find(scope->module + "." + name + "." + member) ==
            enumValues_.end()) {
            continue;
        }
        ++count;
        found = decl;
    }
    return count == 1 ? found : nullptr;
}

std::vector<std::pair<std::string, int64_t>> Resolver::enumMembers(DeclId id) const {
    auto ordered = enumOrder_.find(declKey(id));
    if (ordered == enumOrder_.end()) {
        return {};
    }
    return ordered->second;
}

std::string Resolver::qualify(const std::string& bare) const {
    if (stack_.empty()) {
        return bare;
    }
    return stack_.back() + "." + bare;
}

// --- Modules ---------------------------------------------------------------

bool Resolver::resolveProgram(Program& program, const std::string& module) {
    if (modules_.find(module) != modules_.end()) {
        return !failed_;
    }
    for (const std::string& active : stack_) {
        if (active == module) {
            std::string cycle = module;
            bool started = false;
            for (const std::string& name : stack_) {
                if (name == module) {
                    started = true;
                }
                if (started) {
                    cycle += " -> " + name;
                }
            }
            cycle += " -> " + module;
            fail(program.pos, "import cycle: " + cycle);
            return false;
        }
    }
    if (moduleIds_.find(module) == moduleIds_.end()) {
        uint64_t id = moduleIds_.size();
        moduleIds_[module] = static_cast<uint32_t>(id);
        moduleNames_[static_cast<uint32_t>(id)] = module;
        nextIndex_[module] = 0;
    }
    stack_.push_back(module);
    programOf_[module] = &program;
    const Scope* scope = makeScope(nullptr, module);
    modules_[module] = scope;
    if (entry_ == nullptr) {
        entry_ = scope;
    }
    bindCoreTypes(scope);
    // Imports first, so a `from` alias and every qualified name resolves
    // against a module whose own headers already exist.
    for (const std::unique_ptr<cider::Decl>& decl : program.decls) {
        if (const auto* from = dynamic_cast<const FromImport*>(decl.get())) {
            if (!resolveModule(from->module)) {
                continue;
            }
            const Scope* other = scopeOf(from->module);
            for (const std::string& name : from->names) {
                const Decl* target = lookup(other, name);
                if (target == nullptr) {
                    fail(decl->pos,
                         "'" + name + "' is not exported by '" + from->module + "'");
                    continue;
                }
                Decl* alias = bind(scope, target->kind, name, decl->pos);
                alias->type = target->type;
                alias->sig = target->sig;
                alias->id = target->id;
                alias->nominal = target->nominal;
                alias->typeParams = target->typeParams;
                alias->aliasTarget = target->aliasTarget;
                alias->init = target->init;
                alias->isExtern = target->isExtern;
                if (alias->kind == DeclKind::Alias) {
                    alias->homeModule = from->module;
                }
                if (target->kind == DeclKind::Const && target->init != nullptr) {
                    constInits_[module + "." + name] = target->init;
                }
            }
        } else if (const auto* single = dynamic_cast<const ImportDecl*>(decl.get())) {
            if (!resolveModule(single->module)) {
                continue;
            }
            std::string last = single->module;
            std::string::size_type dot = last.rfind('.');
            if (dot != std::string::npos) {
                last = last.substr(dot + 1);
            }
            Decl* mod = bind(scope, DeclKind::Module, last, decl->pos);
            mod->origin = single->module;
        }
    }
    declareModule(module, program, scope);
    resolveDeclTypes(module, program, scope);
    resolveLocalAggregates(program, scope, module);
    stack_.pop_back();
    // Post-order: dependents lower after their dependencies, so every
    // name a body uses is already registered when its module lowers.
    // (Cycle detection reads the stack, never this list.)
    order_.push_back(module);
    return !failed_;
}

bool Resolver::resolveModule(const std::string& name) {
    if (modules_.find(name) != modules_.end()) {
        return true;
    }
    if (load_ == nullptr) {
        fail(Position{name, 1, 1}, "cannot find module '" + name + "'");
        return false;
    }
    std::unique_ptr<Program> program = load_(name);
    if (program == nullptr) {
        fail(Position{name, 1, 1}, "cannot find module '" + name + "'");
        return false;
    }
    Program* raw = program.get();
    programs_[name] = std::move(program);
    return resolveProgram(*raw, name);
}

// --- The header pass ---------------------------------------------------------

void Resolver::declareModule(const std::string& module, Program& program,
                             const Scope* scope) {
    // Ids come from bind(), the single owner: no manual assignment here.
    // `export` marks visibility; it never changes what the declaration
    // binds, so the wrapper comes off before the shape check.
    for (const std::unique_ptr<cider::Decl>& decl : program.decls) {
        const cider::Decl* unwrapped = decl.get();
        if (const auto* exp = dynamic_cast<const ExportDecl*>(decl.get())) {
            unwrapped = exp->decl.get();
        }
        if (const auto* fn = dynamic_cast<const FuncDecl*>(unwrapped)) {
            Decl* d = bind(scope, DeclKind::Func, fn->name, fn->pos);
            d->type = TypeRef{ctx_.wildcard()};
            d->typeParams = fn->typeParams.size();
            continue;
        }
        if (const auto* ext = dynamic_cast<const ExternFunc*>(unwrapped)) {
            Decl* d = bind(scope, DeclKind::Func, ext->name, ext->pos);
            d->type = TypeRef{ctx_.wildcard()};
            d->isExtern = true;
            continue;
        }
        if (const auto* ext = dynamic_cast<const ExternVar*>(unwrapped)) {
            Decl* d = bind(scope, DeclKind::Var, ext->name, ext->pos);
            d->type = TypeRef{ctx_.wildcard()};
            d->isExtern = true;
            continue;
        }
        if (const auto* cn = dynamic_cast<const ConstDecl*>(unwrapped)) {
            Decl* d = bind(scope, DeclKind::Const, cn->name, cn->pos);
            d->type = TypeRef{ctx_.wildcard()};
            d->init = cn->init.get();
            constInits_[module + "." + cn->name] = cn->init.get();
            continue;
        }
        const StructDecl* sd = dynamic_cast<const StructDecl*>(unwrapped);
        const ClassDecl* cd = dynamic_cast<const ClassDecl*>(unwrapped);
        const InterfaceDecl* id = dynamic_cast<const InterfaceDecl*>(unwrapped);
        const UnionDecl* ud = dynamic_cast<const UnionDecl*>(unwrapped);
        const EnumDecl* ed = dynamic_cast<const EnumDecl*>(unwrapped);
        const TypeAlias* al = dynamic_cast<const TypeAlias*>(unwrapped);
        if (sd == nullptr && cd == nullptr && id == nullptr && ud == nullptr &&
            ed == nullptr && al == nullptr) {
            continue;
        }
        std::string name;
        Kind nominal = Kind::Error;
        uint64_t tps = 0;
        if (sd != nullptr) {
            name = sd->name;
            nominal = Kind::Struct;
            tps = sd->typeParams.size();
        } else if (cd != nullptr) {
            name = cd->name;
            nominal = Kind::Class;
            tps = cd->typeParams.size();
        } else if (id != nullptr) {
            name = id->name;
            nominal = Kind::Interface;
        } else if (ud != nullptr) {
            name = ud->name;
            nominal = Kind::Union;
        } else if (ed != nullptr) {
            name = ed->name;
            nominal = Kind::Enum;
        }
        if (al != nullptr) {
            Decl* d = bind(scope, DeclKind::Alias, al->name, al->pos);
            d->type = TypeRef{ctx_.wildcard()};
            d->typeParams = al->typeParams.size();
            d->paramNames = al->typeParams;
            d->aliasTarget = al->target.get();
            continue;
        }
        Decl* d = bind(scope, DeclKind::Type, name, decl->pos);
        d->nominal = nominal;
        d->typeParams = tps;
        d->packed = sd != nullptr && sd->packed;
        d->type = TypeRef{ctx_.nominal(nominal, d->id)};
    }
    for (const std::unique_ptr<VarDecl>& global : program.globals) {
        Decl* d = bind(scope, DeclKind::Var, global->name, global->pos);
        d->type = TypeRef{ctx_.wildcard()};
    }
}

const Scope* Resolver::localTypeScopeFor(const Block* block) const {
    auto found = localTypeScopes_.find(block);
    return found == localTypeScopes_.end() ? nullptr : found->second;
}

void Resolver::resolveLocalAggregates(Program& program, const Scope* moduleScope,
                                      const std::string& module) {
    for (const std::unique_ptr<cider::Decl>& declaration : program.decls) {
        cider::Decl* unwrapped = declaration.get();
        if (auto* exported = dynamic_cast<ExportDecl*>(unwrapped)) {
            unwrapped = exported->decl.get();
        }
        if (auto* fn = dynamic_cast<FuncDecl*>(unwrapped)) {
            if (fn->body != nullptr) {
                scanLocalAggregateBlock(*fn->body, moduleScope, module);
            }
            continue;
        }
        if (dynamic_cast<StructDecl*>(unwrapped) != nullptr ||
            dynamic_cast<ClassDecl*>(unwrapped) != nullptr) {
            scanLocalAggregateBodies(*unwrapped, moduleScope, module);
        }
    }
}

void Resolver::scanLocalAggregateBodies(cider::Decl& declaration,
                                        const Scope* parent,
                                        const std::string& module) {
    if (auto* sd = dynamic_cast<StructDecl*>(&declaration)) {
        for (MethodDecl& method : sd->methods) {
            if (method.body != nullptr) {
                scanLocalAggregateBlock(*method.body, parent, module);
            }
        }
        return;
    }
    if (auto* cd = dynamic_cast<ClassDecl*>(&declaration)) {
        for (MethodDecl& method : cd->methods) {
            if (method.body != nullptr) {
                scanLocalAggregateBlock(*method.body, parent, module);
            }
        }
        if (cd->init != nullptr && cd->init->body != nullptr) {
            scanLocalAggregateBlock(*cd->init->body, parent, module);
        }
        if (cd->deinit != nullptr && cd->deinit->body != nullptr) {
            scanLocalAggregateBlock(*cd->deinit->body, parent, module);
        }
    }
}

void Resolver::scanLocalAggregateStmt(Stmt& stmt, const Scope* parent,
                                      const std::string& module) {
    if (auto* block = dynamic_cast<Block*>(&stmt)) {
        scanLocalAggregateBlock(*block, parent, module);
        return;
    }
    if (auto* branch = dynamic_cast<IfStmt*>(&stmt)) {
        if (branch->then != nullptr) {
            scanLocalAggregateStmt(*branch->then, parent, module);
        }
        if (branch->els != nullptr) {
            scanLocalAggregateStmt(*branch->els, parent, module);
        }
        return;
    }
    if (auto* loop = dynamic_cast<WhileStmt*>(&stmt)) {
        if (loop->body != nullptr) {
            scanLocalAggregateStmt(*loop->body, parent, module);
        }
        return;
    }
    if (auto* loop = dynamic_cast<ForStmt*>(&stmt)) {
        if (loop->init != nullptr) {
            scanLocalAggregateStmt(*loop->init, parent, module);
        }
        if (loop->body != nullptr) {
            scanLocalAggregateStmt(*loop->body, parent, module);
        }
        return;
    }
    if (auto* loop = dynamic_cast<ForInStmt*>(&stmt)) {
        if (loop->body != nullptr) {
            scanLocalAggregateStmt(*loop->body, parent, module);
        }
        return;
    }
    if (auto* match = dynamic_cast<MatchStmt*>(&stmt)) {
        for (const std::unique_ptr<MatchArm>& arm : match->arms) {
            if (arm->body != nullptr) {
                scanLocalAggregateBlock(*arm->body, parent, module);
            }
        }
        if (match->fallback != nullptr) {
            scanLocalAggregateBlock(*match->fallback, parent, module);
        }
        return;
    }
    if (auto* deferred = dynamic_cast<DeferStmt*>(&stmt)) {
        if (deferred->stmt != nullptr) {
            scanLocalAggregateStmt(*deferred->stmt, parent, module);
        }
        return;
    }
}

void Resolver::scanLocalAggregateBlock(Block& block, const Scope* parent,
                                       const std::string& module) {
    if (localTypeScopes_.find(&block) != localTypeScopes_.end()) {
        return;
    }
    const Scope* moduleScope = scopeOf(module);

    Program local(block.pos, "", {}, {});
    std::vector<DeclStmt*> holders;
    std::vector<std::string> sourceNames;
    std::vector<std::string> hiddenNames;

    for (std::unique_ptr<Stmt>& statement : block.stmts) {
        auto* holder = dynamic_cast<DeclStmt*>(statement.get());
        if (holder == nullptr || holder->decl == nullptr) {
            continue;
        }
        cider::Decl* node = holder->decl.get();
        std::string source;
        uint64_t typeParams = 0;
        if (auto* sd = dynamic_cast<StructDecl*>(node)) {
            source = sd->name;
            typeParams = sd->typeParams.size();
        } else if (auto* cd = dynamic_cast<ClassDecl*>(node)) {
            source = cd->name;
            typeParams = cd->typeParams.size();
        } else if (auto* ud = dynamic_cast<UnionDecl*>(node)) {
            source = ud->name;
        } else {
            continue;
        }
        if (typeParams != 0) {
            fail(node->pos, "a function-local aggregate cannot be generic");
            continue;
        }
        std::string hidden;
        do {
            hidden = "__cider_local_" + std::to_string(localTypeSeq_++);
        } while (moduleScope != nullptr &&
                 moduleScope->bindings.find(hidden) !=
                     moduleScope->bindings.end());
        if (auto* sd = dynamic_cast<StructDecl*>(node)) {
            sd->name = hidden;
        } else if (auto* cd = dynamic_cast<ClassDecl*>(node)) {
            cd->name = hidden;
        } else if (auto* ud = dynamic_cast<UnionDecl*>(node)) {
            ud->name = hidden;
        }
        holders.push_back(holder);
        sourceNames.push_back(source);
        hiddenNames.push_back(hidden);
        local.decls.push_back(std::move(holder->decl));
    }

    if (local.decls.empty()) {
        for (std::unique_ptr<Stmt>& statement : block.stmts) {
            scanLocalAggregateStmt(*statement, parent, module);
        }
        return;
    }

    const Scope* typeScope = makeScope(parent, module);
    localTypeScopes_[&block] = typeScope;
    {
        declareModule(module, local, typeScope);
        uint64_t aliasIndex = 0;
        for (const std::string& hidden : hiddenNames) {
            const std::string& source = sourceNames[aliasIndex];
            const Decl* resolved = lookup(typeScope, hidden);
            if (resolved == nullptr) {
                ++aliasIndex;
                continue;
            }
            if (typeScope->bindings.find(source) != typeScope->bindings.end()) {
                fail(resolved->pos, "duplicate declaration of '" + source + "'");
            } else {
                const_cast<Scope*>(typeScope)->bindings[source] = resolved;
            }
            ++aliasIndex;
        }
        resolveDeclTypes(module, local, typeScope);
        // Lowerer owner lookup starts from the module scope. The source
        // spelling still lives only in the block scope; only the hidden
        // link name is visible module-wide.
        for (const std::string& hidden : hiddenNames) {
            const Decl* resolved = lookup(typeScope, hidden);
            if (resolved != nullptr && moduleScope != nullptr) {
                const_cast<Scope*>(moduleScope)->bindings[hidden] = resolved;
            }
        }
        uint64_t holderIndex = 0;
        for (DeclStmt* holder : holders) {
            holder->decl = std::move(local.decls[holderIndex]);
            ++holderIndex;
        }
        for (DeclStmt* holder : holders) {
            if (holder->decl != nullptr) {
                scanLocalAggregateBodies(*holder->decl, typeScope, module);
            }
        }
    }

    for (std::unique_ptr<Stmt>& statement : block.stmts) {
        scanLocalAggregateStmt(*statement, typeScope, module);
    }
}

void Resolver::resolveDeclTypes(const std::string& module, Program& program,
                                const Scope* scope) {
    EvalEnv env = evalEnv(scope);
    // Member recording precedes every instantiation: an alias, signature
    // or global can name `Wrap<One<int>>` before either struct is
    // declared, and instantiating against unrecorded fields caches an
    // empty, poisoned spec. Recording all struct/class fields first makes
    // declaration order irrelevant for instantiation. (A field whose own
    // type names a later template still needs order today; that deeper
    // forward reference is a separate limitation, not a silent rule.)
    std::unordered_set<std::string> fieldsDone;
    for (const std::unique_ptr<cider::Decl>& decl : program.decls) {
        const cider::Decl* unwrapped = decl.get();
        if (const auto* exp = dynamic_cast<const ExportDecl*>(unwrapped)) {
            unwrapped = exp->decl.get();
        }
        const StructDecl* sdn = dynamic_cast<const StructDecl*>(unwrapped);
        const ClassDecl* cdn = dynamic_cast<const ClassDecl*>(unwrapped);
        if (sdn == nullptr && cdn == nullptr) {
            continue;
        }
        const Decl* self = nullptr;
        const Scope* inner = scope;
        std::string name;
        if (sdn != nullptr) {
            self = lookup(scope, sdn->name);
            name = sdn->name;
            if (!sdn->typeParams.empty() && self != nullptr) {
                inner = typeParamScope(scope, module, sdn->typeParams, self->id,
                                       sdn->pos);
            }
        } else {
            self = lookup(scope, cdn->name);
            name = cdn->name;
            if (!cdn->typeParams.empty() && self != nullptr) {
                inner = typeParamScope(scope, module, cdn->typeParams, self->id,
                                       cdn->pos);
            }
        }
        if (self == nullptr) {
            continue;
        }
        std::string fkey = declKey(self->id);
        if (fieldsDone.find(fkey) != fieldsDone.end()) {
            continue;
        }
        fieldsDone.insert(fkey);
        const std::vector<FieldDecl>& fields =
            sdn != nullptr ? sdn->fields : cdn->fields;
        for (const FieldDecl& field : fields) {
            TypeRef t = resolveType(field.type.get(), inner);
            nodeTypes_[&field] = t;
            if (!t.isError()) {
                fieldTypes_[fkey + "." + field.name] = t;
                fieldLists_[fkey].push_back(field.name);
                if (field.init != nullptr) {
                    fieldDefaultExprs_[fkey + "." + field.name] = field.init.get();
                }
                if (field.isPrivate) {
                    privateFields_.insert(fkey + "." + field.name);
                }
            }
            if (field.bitWidth != nullptr) {
                std::unordered_set<std::string> active;
                EvalResult bits = evalConstInt(*field.bitWidth, env, active);
                fieldBits_[fkey + "." + field.name] = bits.ok ? bits.value : -1;
            }
        }
    }
    for (const std::unique_ptr<cider::Decl>& decl : program.decls) {
        const cider::Decl* unwrapped = decl.get();
        if (const auto* exp = dynamic_cast<const ExportDecl*>(unwrapped)) {
            unwrapped = exp->decl.get();
        }
        if (const auto* fn = dynamic_cast<const FuncDecl*>(unwrapped)) {
            const Decl* d = lookup(scope, fn->name);
            if (d == nullptr) {
                continue;
            }
            const Scope* inner = scope;
            if (!fn->typeParams.empty()) {
                inner = typeParamScope(scope, module, fn->typeParams, d->id, fn->pos);
            }
            bool failed = false;
            Signature sig = buildSignature(fn->params, fn->returns.get(), inner, failed,
                                           abiOf(fn->abi, failed, fn->pos));
            const_cast<Decl*>(d)->sig = std::move(sig);
            continue;
        }
        if (const auto* ext = dynamic_cast<const ExternFunc*>(unwrapped)) {
            const Decl* d = lookup(scope, ext->name);
            if (d == nullptr) {
                continue;
            }
            bool failed = false;
            Signature sig = buildSignature(ext->params, ext->returns.get(), scope, failed,
                                           abiOf(ext->abi, failed, ext->pos));
            const_cast<Decl*>(d)->sig = std::move(sig);
            continue;
        }
        if (const auto* ext = dynamic_cast<const ExternVar*>(unwrapped)) {
            const Decl* d = lookup(scope, ext->name);
            if (d == nullptr) {
                continue;
            }
            TypeRef type = resolveType(ext->type.get(), scope);
            if (!type.isError()) {
                const_cast<Decl*>(d)->type = type;
            }
            continue;
        }
        if (const auto* cn = dynamic_cast<const ConstDecl*>(unwrapped)) {
            if (cn->type != nullptr) {
                TypeRef t = resolveType(cn->type.get(), scope);
                const Decl* d = lookup(scope, cn->name);
                if (d != nullptr && !t.isError()) {
                    const_cast<Decl*>(d)->type = t;
                }
            }
            std::unordered_set<std::string> active;
            EvalResult folded = evalConstInt(*cn->init, env, active);
            if (!folded.ok) {
                fail(cn->init->pos, folded.error);
            }
            continue;
        }
        if (const auto* sd = dynamic_cast<const StructDecl*>(unwrapped)) {
            const Decl* self = lookup(scope, sd->name);
            const Scope* inner = scope;
            if (!sd->typeParams.empty() && self != nullptr) {
                inner = typeParamScope(scope, module, sd->typeParams, self->id, sd->pos);
            }
            for (const FieldDecl& field : sd->fields) {
                TypeRef t = resolveType(field.type.get(), inner);
                nodeTypes_[&field] = t;
                // Fields were already recorded by the pre-pass above; the
                // guard keeps the member lists exactly-once.
                if (self != nullptr && !t.isError() &&
                    fieldsDone.find(declKey(self->id)) == fieldsDone.end()) {
                    std::string fkey = declKey(self->id);
                    fieldTypes_[fkey + "." + field.name] = t;
                    fieldLists_[fkey].push_back(field.name);
                    if (field.init != nullptr) {
                        fieldDefaultExprs_[fkey + "." + field.name] =
                            field.init.get();
                    }
                    if (field.isPrivate) {
                        privateFields_.insert(fkey + "." + field.name);
                    }
                }
            }
            for (const auto& iface : sd->interfaces) {
                TypeRef t = resolveType(iface.get(), inner);
                if (!t.isError() && t.type->kind() != Kind::Interface) {
                    fail(iface->pos, "a struct implements interfaces");
                }
            }
            for (const MethodDecl& method : sd->methods) {
                bool failed = false;
                Signature sig =
                    buildSignature(method.params, method.returns.get(), inner, failed);
                if (self != nullptr && !failed) {
                    std::string key = declKey(self->id);
                    methodSigs_[key + "." + method.name] = std::move(sig);
                    methodLists_[key].push_back(method.name);
                    if (method.isPrivate) {
                        privateMethods_.insert(key + "." + method.name);
                    }
                }
            }
            continue;
        }
        if (const auto* cd = dynamic_cast<const ClassDecl*>(unwrapped)) {
            const Decl* self = lookup(scope, cd->name);
            const Scope* inner = scope;
            if (!cd->typeParams.empty() && self != nullptr) {
                inner = typeParamScope(scope, module, cd->typeParams, self->id, cd->pos);
            }
            const Decl* cself = lookup(scope, cd->name);
            for (const TypeExpr* base : rawBases(cd)) {
                TypeRef t = resolveType(base, inner);
                if (t.isError()) {
                    continue;
                }
                if (t.type->kind() != Kind::Class) {
                    fail(base->pos, "a class inherits classes");
                    continue;
                }
                if (cself != nullptr) {
                    std::optional<DeclId> baseId = t.type->decl();
                    if (baseId.has_value()) {
                        baseTable_[declKey(cself->id)].push_back(*baseId);
                    }
                }
            }
            for (const FieldDecl& field : cd->fields) {
                TypeRef t = resolveType(field.type.get(), inner);
                nodeTypes_[&field] = t;
                // Fields were already recorded by the pre-pass above; the
                // guard keeps the member lists exactly-once.
                if (cself != nullptr && !t.isError() &&
                    fieldsDone.find(declKey(cself->id)) == fieldsDone.end()) {
                    std::string fkey = declKey(cself->id);
                    fieldTypes_[fkey + "." + field.name] = t;
                    fieldLists_[fkey].push_back(field.name);
                    if (field.init != nullptr) {
                        fieldDefaultExprs_[fkey + "." + field.name] =
                            field.init.get();
                    }
                    if (field.isPrivate) {
                        privateFields_.insert(fkey + "." + field.name);
                    }
                }
            }
            for (const MethodDecl& method : cd->methods) {
                bool failed = false;
                Signature sig =
                    buildSignature(method.params, method.returns.get(), inner, failed);
                if (cself != nullptr && !failed) {
                    std::string key = declKey(cself->id);
                    methodSigs_[key + "." + method.name] = std::move(sig);
                    methodLists_[key].push_back(method.name);
                    methodVirtual_[key + "." + method.name] = method.isVirtual;
                    if (method.isPrivate) {
                        privateMethods_.insert(key + "." + method.name);
                    }
                }
            }
            // `init`/`deinit` parse to their own declarations, not methods,
            // but everything downstream (checkCtor, `new C(args)`) looks them
            // up as methods — so they are recorded as methods here.
            const FuncDecl* ctors[2] = {cd->init.get(), cd->deinit.get()};
            const char* names[2] = {"init", "deinit"};
            for (int i = 0; i < 2; ++i) {
                if (ctors[i] == nullptr || cself == nullptr) {
                    continue;
                }
                bool failed = false;
                Signature sig = buildSignature(ctors[i]->params,
                                               ctors[i]->returns.get(), inner, failed);
                if (!failed) {
                    std::string key = declKey(cself->id);
                    methodSigs_[key + "." + names[i]] = std::move(sig);
                    methodLists_[key].push_back(names[i]);
                }
            }
            continue;
        }
        if (const auto* id = dynamic_cast<const InterfaceDecl*>(unwrapped)) {
            const Decl* iself = lookup(scope, id->name);
            for (const MethodSig& method : id->methods) {
                bool failed = false;
                Signature sig =
                    buildSignature(method.params, method.returns.get(), scope, failed);
                if (iself != nullptr && !failed) {
                    std::string key = declKey(iself->id);
                    methodSigs_[key + "." + method.name] = std::move(sig);
                    methodLists_[key].push_back(method.name);
                }
            }
            continue;
        }
        if (const auto* ud = dynamic_cast<const UnionDecl*>(unwrapped)) {
            const Decl* uself = lookup(scope, ud->name);
            for (const FieldDecl& member : ud->members) {
                TypeRef t = resolveType(member.type.get(), scope);
                nodeTypes_[&member] = t;
                if (uself != nullptr && !t.isError()) {
                    std::string fkey = declKey(uself->id);
                    fieldTypes_[fkey + "." + member.name] = t;
                    fieldLists_[fkey].push_back(member.name);
                    if (member.init != nullptr) {
                        fieldDefaultExprs_[fkey + "." + member.name] =
                            member.init.get();
                    }
                    if (member.isPrivate) {
                        privateFields_.insert(fkey + "." + member.name);
                    }
                }
            }
            continue;
        }
        if (const auto* ed = dynamic_cast<const EnumDecl*>(unwrapped)) {
            int64_t next = 0;
            bool first = true;
            const Decl* eself = lookup(scope, ed->name);
            for (const EnumMember& member : ed->members) {
                int64_t value = 0;
                if (member.value != nullptr) {
                    std::unordered_set<std::string> active;
                    EvalResult folded = evalConstInt(*member.value, env, active);
                    if (!folded.ok) {
                        fail(member.value->pos, folded.error);
                        continue;
                    }
                    value = folded.value;
                } else if (first) {
                    value = 0;
                } else {
                    value = next;
                }
                first = false;
                next = value + 1;
                enumValues_[module + "." + ed->name + "." + member.name] = value;
                if (eself != nullptr) {
                    enumOrder_[declKey(eself->id)].emplace_back(member.name, value);
                }
            }
            continue;
        }
        if (const auto* al = dynamic_cast<const TypeAlias*>(unwrapped)) {
            const Decl* d = lookup(scope, al->name);
            const Scope* inner = scope;
            if (!al->typeParams.empty() && d != nullptr) {
                inner = typeParamScope(scope, module, al->typeParams, d->id, al->pos);
            }
            TypeRef t = resolveType(al->target.get(), inner);
            nodeTypes_[al] = t;
            continue;
        }
    }
    for (const std::unique_ptr<VarDecl>& global : program.globals) {
        TypeRef t = resolveType(global->type.get(), scope);
        const Decl* d = lookup(scope, global->name);
        if (d != nullptr && !t.isError()) {
            const_cast<Decl*>(d)->type = t;
        }
        nodeTypes_[global.get()] = t;
    }
}

// The bases of a class, as spellings. A helper because ClassDecl holds them
// behind unique_ptrs and the loop above wants raw spellings.
std::vector<const TypeExpr*> Resolver::rawBases(const ClassDecl* decl) {
    std::vector<const TypeExpr*> out;
    for (const std::unique_ptr<TypeExpr>& base : decl->bases) {
        out.push_back(base.get());
    }
    return out;
}

// --- resolve: the total function ------------------------------------------------

TypeRef Resolver::resolveType(const TypeExpr* spelling, const Scope* scope) {
    if (spelling == nullptr) {
        return errorRef();
    }
    if (const auto* basic = dynamic_cast<const BasicType*>(spelling)) {
        return TypeRef{ctx_.basic(basic->basic)};
    }
    if (const auto* ptr = dynamic_cast<const PointerType*>(spelling)) {
        if (dynamic_cast<const FuncSigType*>(ptr->pointee.get()) != nullptr) {
            const auto* sig = static_cast<const FuncSigType*>(ptr->pointee.get());
            return resolveSignature(sig, scope, true);
        }
        TypeRef inner = resolveType(ptr->pointee.get(), scope);
        if (inner.isError()) {
            return inner;
        }
        return TypeRef{ctx_.pointer(inner.type)};
    }
    if (const auto* arr = dynamic_cast<const ArrayType*>(spelling)) {
        TypeRef element = resolveType(arr->element.get(), scope);
        if (element.isError()) {
            return element;
        }
        EvalEnv env = evalEnv(scope);
        std::unordered_set<std::string> active;
        EvalResult length = evalConstInt(*arr->length, env, active);
        if (!length.ok) {
            fail(arr->length->pos, length.error);
            return errorRef();
        }
        if (length.value < 0) {
            fail(arr->length->pos, "an array length is not negative");
            return errorRef();
        }
        return TypeRef{ctx_.array(element.type, length.value)};
    }
    if (const auto* slice = dynamic_cast<const SliceType*>(spelling)) {
        TypeRef element = resolveType(slice->element.get(), scope);
        if (element.isError()) {
            return element;
        }
        return TypeRef{ctx_.slice(element.type)};
    }
    if (const auto* named = dynamic_cast<const NamedType*>(spelling)) {
        return resolveNamed(named, scope);
    }
    if (const auto* sig = dynamic_cast<const FuncSigType*>(spelling)) {
        return resolveSignature(sig, scope, false);
    }
    fail(spelling->pos, "not a type spelling");
    return errorRef();
}

TypeRef Resolver::resolveNamed(const NamedType* type, const Scope* scope) {
    const Scope* home = scope;
    if (!type->qualifier.empty()) {
        // A qualifier names a module through its local binding when one
        // is in scope (`util` for `import b.util`), or a full module
        // path otherwise (`b.util`, `std.io`). Calls already resolve
        // the binding first; types follow the same rule so `util.Foo`
        // and `util.mval()` mean the same module.
        home = nullptr;
        const Decl* bound = lookup(scope, type->qualifier);
        if (bound != nullptr && bound->kind == DeclKind::Module) {
            home = scopeOf(bound->origin);
        }
        if (home == nullptr) {
            home = scopeOf(type->qualifier);
        }
        if (home == nullptr) {
            fail(type->pos, "unknown module '" + type->qualifier + "'");
            return errorRef();
        }
    }
    if (type->qualifier.empty() && type->args.empty()) {
        if (type->name == "string") {
            return TypeRef{ctx_.string()};
        }
        if (type->name == "void") {
            return TypeRef{ctx_.void_()};
        }
    }
    const Decl* decl = lookup(home, type->name);
    if (decl == nullptr) {
        fail(type->pos, "unknown type '" + type->name + "'");
        return errorRef();
    }
    if (decl->kind == DeclKind::TypeParam) {
        if (!type->args.empty()) {
            fail(type->pos, "a type parameter takes no arguments");
            return errorRef();
        }
        return TypeRef{ctx_.typeParam(decl->templateId, decl->paramIndex)};
    }
    if (decl->kind == DeclKind::Alias) {
        if (!type->args.empty() && type->args.size() != decl->typeParams) {
            fail(type->pos,
                 "'" + type->name + "' takes " + std::to_string(decl->typeParams) +
                     " arguments");
            return errorRef();
        }
        if (decl->typeParams > 0 && type->args.empty()) {
            fail(type->pos, "'" + type->name + "' takes " +
                                 std::to_string(decl->typeParams) + " arguments");
            return errorRef();
        }
        std::string qualified = home->module + "." + decl->name;
        if (aliasActive_.find(qualified) != aliasActive_.end()) {
            fail(type->pos, "type alias cycle: " + qualified);
            return errorRef();
        }
        aliasActive_.insert(qualified);
        const Scope* inner = home;
        // A from-imported alias names its target in the defining
        // module: answer it there, not in the use scope. Same-module
        // aliases carry no home and resolve where they always did.
        if (!decl->homeModule.empty()) {
            const Scope* origin = scopeOf(decl->homeModule);
            if (origin != nullptr) {
                inner = origin;
            }
        }
        if (!decl->paramNames.empty()) {
            inner = typeParamScope(inner, inner->module, decl->paramNames, decl->id,
                                   type->pos);
        }
        TypeRef target = resolveType(decl->aliasTarget, inner);
        aliasActive_.erase(qualified);
        if (target.isError() || type->args.empty()) {
            return target;
        }
        std::vector<TypeRef> argTypes;
        for (const std::unique_ptr<TypeExpr>& arg : type->args) {
            TypeRef resolved = resolveType(arg.get(), scope);
            if (resolved.isError()) {
                return resolved;
            }
            argTypes.push_back(resolved);
        }
        return substitute(target, decl->id, argTypes, type->pos);
    }
    if (decl->kind != DeclKind::Type) {
        fail(type->pos, "'" + type->name + "' is not a type");
        return errorRef();
    }
    if (decl->typeParams > 0) {
        if (type->args.size() != decl->typeParams) {
            fail(type->pos,
                 "'" + type->name + "' takes " + std::to_string(decl->typeParams) +
                     " arguments");
            return errorRef();
        }
        std::vector<TypeRef> argTypes;
        for (const std::unique_ptr<TypeExpr>& arg : type->args) {
            TypeRef resolved = resolveType(arg.get(), scope);
            if (resolved.isError()) {
                return resolved;
            }
            argTypes.push_back(resolved);
        }
        std::optional<DeclId> spec = instantiate(decl->id, argTypes, type->pos);
        if (!spec.has_value()) {
            return errorRef();
        }
        return TypeRef{ctx_.nominal(decl->nominal, *spec)};
    }
    if (!type->args.empty()) {
        fail(type->pos, "'" + type->name + "' takes no arguments");
        return errorRef();
    }
    return TypeRef{ctx_.nominal(decl->nominal, decl->id)};
}

namespace {

// Self-applied templates name the depth instead of hanging: `Box<Box<T>>`
// nests one level per application, and depth is the only honest bound.
constexpr int kMaxInstDepth = 16;

std::string specKey(DeclId tmpl, const std::vector<TypeRef>& args) {
    std::string key =
        std::to_string(tmpl.module) + "." + std::to_string(tmpl.index) + "<";
    bool first = true;
    for (TypeRef arg : args) {
        if (!first) {
            key += ",";
        }
        first = false;
        key += typeName(arg);
    }
    return key + ">";
}

}  // namespace

const Decl* Resolver::bindTypeParam(const Scope* scope, const std::string& name,
                                      DeclId templateId, uint32_t index, Position pos) {
    Decl* d = bind(scope, DeclKind::TypeParam, name, pos);
    d->templateId = templateId;
    d->paramIndex = index;
    return d;
}

const Decl* Resolver::declById(DeclId id) const {
    for (const std::unique_ptr<Decl>& decl : decls_) {
        if (decl->id.module == id.module && decl->id.index == id.index) {
            return decl.get();
        }
    }
    return nullptr;
}

std::string Resolver::linkName(const Decl& decl) const {
    if (decl.isExtern) {
        return decl.name;
    }
    // Decls bound in a function body have no link symbol; callers pass the
    // module they looked the name up in. A null scope is entry-adjacent by
    // construction, so bare is safe there too.
    std::string home = decl.scope != nullptr ? decl.scope->module : entryModule();
    if (home.empty() || home == entryModule()) {
        return decl.name;
    }
    // Dots cannot survive the assembler: `a.b`'s `aval` links as `a__b__aval`.
    std::string flat = home;
    for (char& c : flat) {
        if (c == '.') {
            c = '_';
        }
    }
    return flat + "__" + decl.name;
}

std::string Resolver::specLinkName(DeclId spec) const {
    auto origin = instantiationOf(spec);
    if (!origin.has_value()) {
        return "";
    }
    const Decl* tmpl = declById(origin->first);
    if (tmpl == nullptr || tmpl->kind != DeclKind::Func) {
        return "";
    }
    std::string home =
        tmpl->scope != nullptr ? tmpl->scope->module : entryModule();
    std::string flat = home;
    for (char& c : flat) {
        if (c != '_' && (c < 'a' || c > 'z') && (c < 'A' || c > 'Z') &&
            (c < '0' || c > '9')) {
            c = '_';
        }
    }
    // Specs always mangle, even at entry: the bare name belongs to the
    // template itself (which emits nothing), so `id` and `id__S__int`
    // never collide.
    std::string out = flat + "__" + tmpl->name + "__S";
    for (TypeRef arg : origin->second) {
        out += "__";
        for (char c : typeName(arg)) {
            if (c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9')) {
                out += c;
            } else {
                out += '_';
            }
        }
    }
    return out;
}

TypeRef Resolver::substitute(TypeRef type, DeclId tmpl, const std::vector<TypeRef>& args,
                             Position pos) {
    if (type.isError()) {
        return type;
    }
    Kind kind = type.type->kind();
    if (kind == Kind::TypeParam) {
        const TypeParamPayload& payload = std::get<TypeParamPayload>(type.type->payload());
        if (payload.decl.module == tmpl.module && payload.decl.index == tmpl.index &&
            payload.index < args.size()) {
            // A bare parameter reference needs no substitution within this
            // frame: if it names this template's parameter the mapping
            // already selected it, and another template's parameter is an
            // outer frame's business. Recursing here ping-pongs on
            // param-to-param mappings (`Swapped<U, T>` swaps T and U
            // forever) and segfaulted the compiler; returning the
            // reference answers every case, including identity.
            const TypeRef& arg = args[payload.index];
            if (!arg.isError() && arg.type->kind() == Kind::TypeParam) {
                return arg;
            }
            return substitute(arg, tmpl, args, pos);
        }
        return type;
    }
    if (kind == Kind::Pointer) {
        const Type* inner = type.type->pointee();
        TypeRef sub = substitute(TypeRef{inner}, tmpl, args, pos);
        if (sub.isError()) {
            return sub;
        }
        return TypeRef{ctx_.pointer(sub.type)};
    }
    if (kind == Kind::Array) {
        const Type* inner = type.type->element();
        TypeRef sub = substitute(TypeRef{inner}, tmpl, args, pos);
        if (sub.isError()) {
            return sub;
        }
        std::optional<int64_t> length = type.type->length();
        if (!length.has_value()) {
            return errorRef();
        }
        return TypeRef{ctx_.array(sub.type, *length)};
    }
    if (kind == Kind::Slice) {
        const Type* inner = type.type->element();
        TypeRef sub = substitute(TypeRef{inner}, tmpl, args, pos);
        if (sub.isError()) {
            return sub;
        }
        return TypeRef{ctx_.slice(sub.type)};
    }
    if (kind == Kind::Closure || kind == Kind::FunctionPtr) {
        const Signature& sig = *type.type->signature();
        Signature out = substSig(sig, tmpl, args, pos);
        if (kind == Kind::Closure) {
            return TypeRef{ctx_.closure(std::move(out))};
        }
        return TypeRef{ctx_.functionPtr(std::move(out))};
    }
    if (kind == Kind::Struct || kind == Kind::Class || kind == Kind::Interface ||
        kind == Kind::Union || kind == Kind::Enum) {
        std::optional<DeclId> id = type.type->decl();
        if (!id.has_value()) {
            return errorRef();
        }
        std::string key =
            std::to_string(id->module) + "." + std::to_string(id->index);
        auto origin = specOrigin_.find(key);
        if (origin == specOrigin_.end()) {
            return type;
        }
        std::vector<TypeRef> subArgs;
        for (TypeRef arg : origin->second.second) {
            TypeRef sub = substitute(arg, tmpl, args, pos);
            if (sub.isError()) {
                return sub;
            }
            subArgs.push_back(sub);
        }
        std::optional<DeclId> spec = instantiate(origin->second.first, subArgs, pos);
        if (!spec.has_value()) {
            return errorRef();
        }
        return TypeRef{ctx_.nominal(kind, *spec)};
    }
    return type;
}

Signature Resolver::substSig(const Signature& sig, DeclId tmpl,
                             const std::vector<TypeRef>& args, Position pos) {
    std::vector<Parameter> params;
    for (const Parameter& param : sig.parameters()) {
        TypeRef sub = substitute(TypeRef{param.type}, tmpl, args, pos);
        Parameter out;
        out.type = sub.isError() ? nullptr : sub.type;
        out.isMutable = param.isMutable;
        params.push_back(out);
    }
    const Type* ret = sig.returnType();
    TypeRef subRet{ret};
    if (ret != nullptr) {
        subRet = substitute(TypeRef{ret}, tmpl, args, pos);
    }
    return Signature(std::move(params), subRet.isError() ? nullptr : subRet.type,
                     sig.abi());
}

const Signature* Resolver::funcSig(DeclId id) const {
    auto found = funcSigs_.find(std::to_string(id.module) + "." + std::to_string(id.index));
    return found == funcSigs_.end() ? nullptr : &found->second;
}

std::optional<std::pair<DeclId, std::vector<TypeRef>>> Resolver::instantiationOf(
    DeclId spec) const {
    auto found = specOrigin_.find(std::to_string(spec.module) + "." +
                                  std::to_string(spec.index));
    if (found == specOrigin_.end()) {
        return std::nullopt;
    }
    return found->second;
}

bool Resolver::isSpecialization(DeclId id) const {
    return specOrigin_.find(std::to_string(id.module) + "." + std::to_string(id.index)) !=
           specOrigin_.end();
}

std::optional<DeclId> Resolver::instantiate(DeclId tmpl, const std::vector<TypeRef>& args,
                                            Position pos) {
    std::string key = specKey(tmpl, args);
    auto cached = specCache_.find(key);
    if (cached != specCache_.end()) {
        return cached->second;
    }
    if (instDepth_ >= kMaxInstDepth) {
        fail(pos, "instantiation too deep");
        return std::nullopt;
    }
    const Decl* templateDecl = declById(tmpl);
    if (templateDecl == nullptr) {
        fail(pos, "checker bug: template was never declared");
        return std::nullopt;
    }
    auto modName = moduleNames_.find(tmpl.module);
    if (modName == moduleNames_.end()) {
        fail(pos, "checker bug: template module is unknown");
        return std::nullopt;
    }
    DeclId id{tmpl.module, nextIndex_[modName->second]++};
    // Cached before populating: a self-application re-enters here and must
    // find this id rather than recurse forever.
    specCache_[key] = id;
    specOrigin_[std::to_string(id.module) + "." + std::to_string(id.index)] =
        std::make_pair(tmpl, args);
    ++instDepth_;
    if (templateDecl->kind == DeclKind::Type &&
        (templateDecl->nominal == Kind::Struct || templateDecl->nominal == Kind::Class)) {
        std::string from = std::to_string(tmpl.module) + "." + std::to_string(tmpl.index);
        std::string into = std::to_string(id.module) + "." + std::to_string(id.index);
        for (const std::string& field : fieldNames(tmpl)) {
            TypeRef member = fieldType(tmpl, field);
            TypeRef sub = substitute(member, tmpl, args, pos);
            if (!sub.isError()) {
                fieldTypes_[into + "." + field] = sub;
                fieldLists_[into].push_back(field);
                const Expr* dflt = fieldDefault(tmpl, field);
                if (dflt != nullptr) {
                    fieldDefaultExprs_[into + "." + field] = dflt;
                }
                if (fieldIsPrivate(tmpl, field)) {
                    privateFields_.insert(into + "." + field);
                }
            }
        }
        for (const std::string& method : methodNames(tmpl)) {
            const Signature* sig = methodSig(tmpl, method);
            if (sig == nullptr) {
                continue;
            }
            methodSigs_[into + "." + method] = substSig(*sig, tmpl, args, pos);
            methodLists_[into].push_back(method);
            if (privateMethods_.find(declKey(tmpl) + "." + method) !=
                privateMethods_.end()) {
                privateMethods_.insert(into + "." + method);
            }
        }
        // Bases substitute like members: a base recorded as nominal of an
        // earlier specialization re-originates through specOrigin_ inside
        // substitute, so `B<T>` becomes `B<int>` when C does.
        auto bases = baseTable_.find(from);
        if (bases != baseTable_.end()) {
            for (DeclId baseId : bases->second) {
                TypeRef sub = substitute(TypeRef{ctx_.nominal(Kind::Class, baseId)}, tmpl,
                                         args, pos);
                if (sub.isError()) {
                    continue;
                }
                std::optional<DeclId> subId = sub.type->decl();
                if (subId.has_value()) {
                    baseTable_[into].push_back(*subId);
                }
            }
        }
    }
    if (templateDecl->kind == DeclKind::Func) {
        funcSigs_[std::to_string(id.module) + "." + std::to_string(id.index)] =
            substSig(templateDecl->sig, tmpl, args, pos);
    }
    --instDepth_;
    return id;
}

TypeRef Resolver::resolveSignature(const FuncSigType* type, const Scope* scope,
                                  bool inPointer) {
    bool failed = false;
    Signature sig = buildFuncSig(type->params, type->returns.get(), scope, failed,
                                 abiOf(type->abi, failed, type->pos));
    if (failed) {
        return errorRef();
    }
    if (inPointer) {
        return TypeRef{ctx_.functionPtr(std::move(sig))};
    }
    return TypeRef{ctx_.closure(std::move(sig))};
}

Abi Resolver::abiOf(const std::string& spelling, bool& failed, Position pos) {
    if (spelling.empty()) {
        return target::info(target_).abi == target::Abi::MsAbi ? Abi::MsAbi
                                                               : Abi::SysV;
    }
    if (spelling == "sysv") {
        return Abi::SysV;
    }
    if (spelling == "msabi") {
        return Abi::MsAbi;
    }
    fail(pos, "unknown calling convention '" + spelling + "'");
    failed = true;
    return Abi::SysV;
}

Signature Resolver::buildSignature(const std::vector<Param>& params,
                                   const TypeExpr* returns, const Scope* scope,
                                   bool& failed, Abi abi) {
    std::vector<Parameter> out;
    for (const Param& param : params) {
        TypeRef t = resolveType(param.type.get(), scope);
        if (t.isError()) {
            failed = true;
            continue;
        }
        Parameter p;
        p.type = t.type;
        p.isMutable = param.isMutable;
        out.push_back(p);
    }
    const Type* ret = ctx_.void_();
    if (returns != nullptr) {
        TypeRef t = resolveType(returns, scope);
        if (t.isError()) {
            failed = true;
        } else {
            ret = t.type;
        }
    }
    return Signature(std::move(out), ret, abi);
}

void Resolver::buildMethodSig(const MethodDecl& method, const Scope* scope, bool& failed) {
    Signature sig = buildSignature(method.params, method.returns.get(), scope, failed);
    (void)sig;
}

Signature Resolver::buildFuncSig(const std::vector<std::unique_ptr<TypeExpr>>& params,
                                 const TypeExpr* returns, const Scope* scope,
                                 bool& failed, Abi abi) {
    std::vector<Parameter> out;
    for (const std::unique_ptr<TypeExpr>& param : params) {
        TypeRef t = resolveType(param.get(), scope);
        if (t.isError()) {
            failed = true;
            continue;
        }
        Parameter p;
        p.type = t.type;
        out.push_back(p);
    }
    const Type* ret = ctx_.void_();
    if (returns != nullptr) {
        TypeRef t = resolveType(returns, scope);
        if (t.isError()) {
            failed = true;
        } else {
            ret = t.type;
        }
    }
    return Signature(std::move(out), ret, abi);
}

const Scope* Resolver::typeParamScope(const Scope* parent, const std::string& module,
                                        const std::vector<std::string>& names,
                                        DeclId templateId, Position pos) {
    const Scope* scope = makeScope(parent, module);
    uint32_t index = 0;
    for (const std::string& name : names) {
        Decl* d = bind(scope, DeclKind::TypeParam, name, pos);
        d->templateId = templateId;
        d->paramIndex = index;
        ++index;
    }
    return scope;
}

EvalEnv Resolver::evalEnv(const Scope* scope) {
    // Built field by field: EvalEnv holds a reference and cannot be copied, so
    // it is constructed and filled rather than aggregate-initialised.
    EvalEnv env(ctx_);
    env.constantInit = [this](const std::string& qualified) -> const Expr* {
        auto found = constInits_.find(qualified);
        return found == constInits_.end() ? nullptr : found->second;
    };
    env.resolve = [this, scope](const TypeExpr* spelling) -> TypeRef {
        return resolveType(spelling, scope);
    };
    // The scope's own module, not the resolver's stack: by the time a body is
    // checked the header pass has unwound, and `main.x` must mean main's x.
    env.qualify = [scope](const std::string& bare) -> std::string {
        return scope->module + "." + bare;
    };
    env.sizeOf = [this](const Type* type) -> int64_t {
        return nominalSize(type);
    };
    env.alignOf = [this](const Type* type) -> int64_t {
        return nominalAlign(type);
    };
    return env;
}

}  // namespace resolve
}  // namespace cider
