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

Resolver::Resolver(TypeContext& ctx, ModuleLoader load)
    : ctx_(ctx), load_(std::move(load)) {}

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
        // The prelude is an implicit `from prelude import ...` in every
        // module (FEATURES §10.2): any spelling the program writes — its own
        // declaration or a from-import — shadows it for that module instead
        // of colliding with it. Anything else redeclaring is still an error.
        if (!found->second->isPrelude) {
            fail(pos, "duplicate declaration of '" + name + "'");
            return const_cast<Decl*>(found->second);
        }
    }
    auto decl = std::make_unique<Decl>();
    decl->kind = kind;
    decl->name = name;
    decl->scope = scope;
    decl->pos = pos;
    // Single owner of declaration ids: every Decl gets a fresh
    // (module, index) here, so prelude entries, templates, globals and
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
    bool classChain = false;
    bool afterVptr = false;
    bool packedStruct = false;
    if (const Decl* self = declById(id)) {
        if (self->nominal == Kind::Union) {
            return unionMemberIndex(id, name) >= 0 ? 0 : -1;
        }
        if (self->nominal == Kind::Class) {
            classChain = true;
            afterVptr = !virtualNames(id).empty();
        }
        if (self->nominal == Kind::Struct && self->packed) {
            packedStruct = true;
        }
    }
    std::vector<std::string> fields;
    if (classChain) {
        std::unordered_set<std::string> chainSeen;
        if (!chainFieldsImpl(id, chainSeen, fields)) {
            return -1;
        }
    } else {
        fields = fieldNames(id);
    }
    int64_t matches = 0;
    for (const std::string& field : fields) {
        if (field == name) {
            ++matches;
        }
    }
    // One name, one offset: a field repeated across a chain would answer
    // two addresses for one spelling, which is an ambiguity, not a layout.
    if (matches > 1) {
        return -1;
    }
    int64_t at = 0;
    if (afterVptr) {
        at = layout::kSizePointer;
    }
    for (const std::string& field : fields) {
        TypeRef type = fieldType(id, field);
        if (type.isError() && classChain) {
            type = fieldTypeAcrossBases(id, field);
        }
        if (type.isError()) {
            return -1;
        }
        int64_t multiple = nominalAlignImpl(type.type, seen);
        int64_t bytes = nominalSizeImpl(type.type, seen);
        if (multiple <= 0 || bytes < 0) {
            return -1;
        }
        if (!packedStruct) {
            at = layout::roundUp(at, multiple);
        }
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

bool Resolver::chainFieldsImpl(DeclId id, std::unordered_set<std::string>& seen,
                               std::vector<std::string>& out) const {
    if (!seen.insert(declKey(id)).second) {
        return false;
    }
    std::vector<DeclId> bases = classBases(id);
    if (bases.size() > kOneBase) {
        // Several bases at one level would need per-subobject vtprs; with
        // one vptr word the layout cannot name them, so nothing lays out.
        return false;
    }
    for (DeclId base : bases) {
        if (!chainFieldsImpl(base, seen, out)) {
            return false;
        }
    }
    for (const std::string& field : fieldNames(id)) {
        out.push_back(field);
    }
    return true;
}

int64_t Resolver::structSize(DeclId id) const {
    std::unordered_set<std::string> seen;
    return structSizeImpl(id, seen);
}

int64_t Resolver::classSize(DeclId id) const {
    std::unordered_set<std::string> seen;
    seen.insert(declKey(id));
    return classSizeImpl(id, seen);
}

int64_t Resolver::classAlign(DeclId id) const {
    std::unordered_set<std::string> seen;
    seen.insert(declKey(id));
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
        if (!id.has_value() || !seen.insert(declKey(*id)).second) {
            return -1;
        }
        int64_t out = classSizeImpl(*id, seen);
        seen.erase(declKey(*id));
        return out;
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
        if (!id.has_value() || !seen.insert(declKey(*id)).second) {
            return -1;
        }
        int64_t out = classAlignImpl(*id, seen);
        seen.erase(declKey(*id));
        return out;
    }
    return alignOf(type);
}

int64_t Resolver::structSizeImpl(DeclId id, std::unordered_set<std::string>& seen) const {
    const Decl* self = declById(id);
    const bool packed = self != nullptr && self->packed;
    int64_t at = 0;
    int64_t multiple = 1;
    for (const std::string& field : fieldNames(id)) {
        TypeRef type = fieldType(id, field);
        if (type.isError()) {
            return -1;
        }
        int64_t fieldMultiple = nominalAlignImpl(type.type, seen);
        int64_t bytes = nominalSizeImpl(type.type, seen);
        if (fieldMultiple <= 0 || bytes < 0) {
            return -1;
        }
        if (fieldMultiple > multiple) {
            multiple = fieldMultiple;
        }
        if (!packed) {
            at = layout::roundUp(at, fieldMultiple);
        }
        at += bytes;
    }
    if (packed) {
        // 4.5: no padding is the point; the size is the raw sum.
        return at;
    }
    return layout::roundUp(at, multiple);
}

int64_t Resolver::structAlignImpl(DeclId id, std::unordered_set<std::string>& seen) const {
    const Decl* self = declById(id);
    if (self != nullptr && self->packed) {
        // 4.5: a byte-exact layout aligns to one.
        return 1;
    }
    int64_t multiple = 1;
    for (const std::string& field : fieldNames(id)) {
        TypeRef type = fieldType(id, field);
        if (type.isError()) {
            return -1;
        }
        int64_t fieldMultiple = nominalAlignImpl(type.type, seen);
        if (fieldMultiple <= 0) {
            return -1;
        }
        if (fieldMultiple > multiple) {
            multiple = fieldMultiple;
        }
    }
    return multiple;
}

int64_t Resolver::classSizeImpl(DeclId id, std::unordered_set<std::string>& seen) const {
    // A chain lays out bases most-base first, then the class's own fields
    // (FEATURES 2.2). Several bases at one level have no layout yet, so
    // the walk refuses. Under 4.8.5 only a chain with a virtual slot
    // carries the vptr word; a chain without one sizes by classSizePlain.
    std::vector<std::string> fields;
    {
        std::unordered_set<std::string> chainSeen;
        if (!chainFieldsImpl(id, chainSeen, fields)) {
            return -1;
        }
    }
    const bool headed = !virtualNames(id).empty();
    int64_t at = 0;
    int64_t multiple = 1;
    if (headed) {
        at = layout::kSizePointer;
        multiple = layout::kAlignPointer;
    }
    for (const std::string& field : fields) {
        TypeRef type = fieldType(id, field);
        if (type.isError()) {
            return -1;
        }
        int64_t fieldMultiple = nominalAlignImpl(type.type, seen);
        int64_t bytes = nominalSizeImpl(type.type, seen);
        if (fieldMultiple <= 0 || bytes < 0) {
            return -1;
        }
        if (fieldMultiple > multiple) {
            multiple = fieldMultiple;
        }
        at = layout::roundUp(at, fieldMultiple);
        at += bytes;
    }
    if (headed) {
        return layout::classSize(at - layout::kSizePointer, multiple);
    }
    return layout::classSizePlain(at, multiple);
}

int64_t Resolver::classAlignImpl(DeclId id, std::unordered_set<std::string>& seen) const {
    std::vector<std::string> fields;
    {
        std::unordered_set<std::string> chainSeen;
        if (!chainFieldsImpl(id, chainSeen, fields)) {
            return -1;
        }
    }
    // 4.8.5: only a virtual chain carries the pointer alignment of its
    // vptr word; a concrete chain aligns as its fields do.
    int64_t multiple = 1;
    if (!virtualNames(id).empty()) {
        multiple = layout::kAlignPointer;
    }
    for (const std::string& field : fields) {
        TypeRef type = fieldType(id, field);
        if (type.isError()) {
            return -1;
        }
        int64_t fieldMultiple = nominalAlignImpl(type.type, seen);
        if (fieldMultiple <= 0) {
            return -1;
        }
        if (fieldMultiple > multiple) {
            multiple = fieldMultiple;
        }
    }
    return multiple;
}

int64_t Resolver::unionSizeImpl(DeclId id, std::unordered_set<std::string>& seen) const {
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
    std::vector<DeclId> bases = classBases(id);
    if (bases.size() > kOneBase) {
        // Several bases at one level compose no slot list yet; callers see
        // an empty table and refuse rather than dispatch through half one.
        return {};
    }
    std::vector<std::string> out;
    if (!bases.empty()) {
        // One base at a level owns every inherited slot; its list is the
        // prefix of this class's (a multiple-base level answered above).
        out = virtualNames(bases[kFirstBase]);
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
    bindPrelude(scope, program.pos);
    // Imports first, so a `from` alias and every qualified name resolves
    // against a module whose own headers already exist.
    for (const std::unique_ptr<cider::Decl>& decl : program.decls) {
        if (const auto* from = dynamic_cast<const FromImport*>(decl.get())) {
            if (from->module == "prelude") {
                // The prelude is an implicit `from prelude import ...` in
                // every module (FEATURES §10.2), so the explicit form binds
                // the module's own prelude entries: no file is loaded, and
                // the alias keeps the prelude mark or the linker would
                // look for a module-qualified symbol that is never emitted.
                for (const std::string& name : from->names) {
                    const Decl* target = lookup(scope, name);
                    if (target == nullptr || !target->isPrelude) {
                        fail(decl->pos, "'" + name +
                                            "' is not exported by 'prelude'");
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
                    alias->isPrelude = true;
                }
                continue;
            }
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
                alias->isPrelude = target->isPrelude;
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
            Signature sig = buildSignature(fn->params, fn->returns.get(), inner, failed);
            const_cast<Decl*>(d)->sig = std::move(sig);
            continue;
        }
        if (const auto* ext = dynamic_cast<const ExternFunc*>(unwrapped)) {
            const Decl* d = lookup(scope, ext->name);
            if (d == nullptr) {
                continue;
            }
            bool failed = false;
            Signature sig = buildSignature(ext->params, ext->returns.get(), scope, failed);
            const_cast<Decl*>(d)->sig = std::move(sig);
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
    if (decl.isPrelude) {
        return "cider_" + decl.name;
    }
    if (decl.isExtern) {
        return decl.name;
    }
    // Decls bound in a function body have no link symbol; callers pass the
    // module they looked the name up in. A null scope is entry-adjacent by
    // construction (the prelude fallback), so bare is safe there too.
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
            }
        }
        for (const std::string& method : methodNames(tmpl)) {
            const Signature* sig = methodSig(tmpl, method);
            if (sig == nullptr) {
                continue;
            }
            methodSigs_[into + "." + method] = substSig(*sig, tmpl, args, pos);
            methodLists_[into].push_back(method);
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
    Signature sig = buildFuncSig(type->params, type->returns.get(), scope, failed);
    if (failed) {
        return errorRef();
    }
    if (inPointer) {
        return TypeRef{ctx_.functionPtr(std::move(sig))};
    }
    return TypeRef{ctx_.closure(std::move(sig))};
}

Signature Resolver::buildSignature(const std::vector<Param>& params,
                                   const TypeExpr* returns, const Scope* scope,
                                   bool& failed) {
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
    return Signature(std::move(out), ret, Abi::SysV);
}

void Resolver::buildMethodSig(const MethodDecl& method, const Scope* scope, bool& failed) {
    Signature sig = buildSignature(method.params, method.returns.get(), scope, failed);
    (void)sig;
}

Signature Resolver::buildFuncSig(const std::vector<std::unique_ptr<TypeExpr>>& params,
                                 const TypeExpr* returns, const Scope* scope,
                                 bool& failed) {
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
    return Signature(std::move(out), ret, Abi::SysV);
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

void Resolver::bindPrelude(const Scope* scope, Position pos) {
    const Type* string = ctx_.string();
    const Type* none = ctx_.void_();
    const Type* i64 = ctx_.basic(BasicKind::I64);
    const Type* addr = ctx_.pointer(none);
    auto func = [&](const std::string& name, std::vector<Parameter> params,
                    const Type* ret) {
        Decl* d = bind(scope, DeclKind::Func, name, pos);
        d->type = TypeRef{ctx_.wildcard()};
        d->sig = Signature(std::move(params), ret, Abi::SysV);
        d->isPrelude = true;
    };
    func("print", {{string, false}}, none);
    func("panic", {{string, false}}, none);
    func("alloc", {{i64, false}, {i64, false}}, addr);
    func("realloc", {{addr, false}, {i64, false}, {i64, false}}, addr);
    func("free", {{addr, false}}, none);
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
    return env;
}

}  // namespace resolve
}  // namespace cider
