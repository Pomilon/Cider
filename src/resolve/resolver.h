// resolver.h — the header pass. LAYERS.md §3.3.
//
// Produces Decl (every declaration with its resolved Type), Scopes (a tree of
// name-to-Decl bindings), and resolve (the total function from TYPES.md §2.4).
// Two ordered phases inside one visitor class: headers of every module first,
// so a global or function may be used above its declaration, then bodies —
// which belong to the checker, not here.
//
// Every name a program mentions is bound to exactly one Decl, or the program
// is rejected with a diagnostic at the use. There is no "unresolved but
// tolerated" state. Imports resolve recursively: a module's own imports are
// resolved when it is, so `main -> a -> b` resolves and checks `b`. An import
// cycle names the cycle instead of hanging.

#ifndef CIDER_RESOLVE_RESOLVER_H
#define CIDER_RESOLVE_RESOLVER_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast/ast.h"
#include "eval/eval.h"
#include "resolve/scope.h"

namespace cider {
namespace resolve {

struct ResolveError {
    Position pos;
    std::string message;
};

class Resolver {
public:
    // Parses nothing itself: given a module name, returns its AST, or null
    // when no such module exists. The driver owns file lookup and parsing;
    // this class owns recursion, cycle detection and checking.
    using ModuleLoader =
        std::function<std::unique_ptr<Program>(const std::string& name)>;

    explicit Resolver(TypeContext& ctx, ModuleLoader load = nullptr);

    // Runs the header pass over one program as `module`, resolving its
    // imports recursively first. False when any diagnostic was recorded, in
    // this module or one it reached.
    bool resolveProgram(Program& program, const std::string& module);

    // The entry module's scope: the first module resolveProgram ran on.
    const Scope* moduleScope() const { return entry_; }
    // The entry module's name: definitions here keep their spellings, and
    // every other module's link names mangle under their own.
    std::string entryModule() const {
        return entry_ != nullptr ? entry_->module : std::string();
    }
    // Every resolved module in the order it started: the entry first, then
    // each import depth-first. The driver checks and lowers each in turn.
    const std::vector<std::string>& moduleOrder() const { return order_; }
    // The loaded program for a resolved module. Null when unknown: the
    // entry program is the caller's, only imports are owned here.
    Program* programOf(const std::string& module) const {
        auto found = programOf_.find(module);
        return found == programOf_.end() ? nullptr : found->second;
    }
    const Scope* scopeOf(const std::string& module) const;
    const Decl* lookup(const Scope* scope, const std::string& name) const;

    // TYPES.md §2.4, the total function: a spelling in a scope, answered with
    // a fully-resolved Type or Error. Error answers record one diagnostic at
    // the spelling and never cascade a second for the same use.
    TypeRef resolveType(const TypeExpr* spelling, const Scope* scope);

    // An enumerator's value, assigned C-style (0, then previous plus one).
    // -1 when no such member exists.
    int64_t enumValue(const std::string& qualifiedType, const std::string& member) const;
    // Whether the qualified enum type names that member (-1 is a legal
    // value, so presence needs its own question).
    bool isEnumMember(const std::string& qualifiedType, const std::string& member) const;
    // Every enumerator of one enum declaration, in declaration order with
    // its value: the membership check an int-to-enum cast lowers and the
    // case list enum exhaustiveness counts. Keyed by declaration, not by
    // spelling, so two same-named enums never share one. Empty when the id
    // names no enum this table holds.
    std::vector<std::pair<std::string, int64_t>> enumMembers(DeclId id) const;
    // The single enum of this scope with that member, or null when none
    // or several name it; `count` tells the two apart so diagnostics stay
    // honest. Only enums declared in this module answer; imported ones
    // keep their qualified spelling.
    const Decl* lookupEnumMember(const Scope* scope, const std::string& member,
                                 int& count) const;

    // The resolved type of a node the header pass already answered (field and
    // parameter types, alias targets). The checker adds expression types to the
    // same table; one table, two writers in order, never two tables.
    TypeRef typeOf(const Node* node) const;
    void recordType(const Node* node, TypeRef type);
    // A field or member type by owning declaration and name. Error when no
    // such field exists.
    TypeRef fieldType(DeclId id, const std::string& name) const;
    // 4.8.4: a field's visibility and the module that declares it. Both
    // walk bases the way fieldType does; the declaring base answers.
    bool fieldIsPrivate(DeclId id, const std::string& name) const;
    std::string fieldModule(DeclId id, const std::string& name) const;
    // A method signature by owning declaration and name. Null when absent.
    const Signature* methodSig(DeclId id, const std::string& name) const;
    // Every method name of a struct, class or interface, in declaration
    // order. Empty for anything else.
    std::vector<std::string> methodNames(DeclId id) const;
    // Every field or member name of a struct, class or union, in declaration
    // order. Empty for anything else.
    std::vector<std::string> fieldNames(DeclId id) const;
    // A class's whole chain's fields in physical order (bases first), which
    // is also the set a `new C { ... }` literal must name. Structs and
    // unions answer their own names; a chain the layout cannot name answers
    // empty and construction refuses.
    std::vector<std::string> chainFieldNames(DeclId id) const;
    // The byte offset of a field within its struct, laid out in declaration
    // order with each field aligned to its own alignment (TYPES.md 3.4). -1
    // when the struct, the field, or any size on the way there is unknown.
    int64_t fieldOffset(DeclId id, const std::string& name) const;    // A struct's total size: fields plus padding, rounded to its own
    // alignment, with no padding beyond that (TYPES.md 3.3). -1 when unknown.
    // Only structs: classes and interfaces are refused elsewhere.
    int64_t structSize(DeclId id) const;
    // A class's total size: its chain's fields in most-base-first order,
    // with the leading vptr word only when the chain has virtual slots
    // (4.8.5), rounded to the combined alignment (TYPES.md 4.2). -1 when
    // the chain is unsized (several bases at one level, unknown fields).
    int64_t classSize(DeclId id) const;
    // A class's alignment: the maximum of its vptr word (only when the
    // chain has virtual slots, 4.8.5) and its fields' and bases'.
    // -1 when unknown.
    int64_t classAlign(DeclId id) const;
    // 4.8: the offset of a base subobject inside a derived object's layout,
    // composed through every path. Null when `base` is not an ancestor or
    // the path is ambiguous (two subobjects spell one name).
    std::optional<int64_t> baseOffset(DeclId derived, DeclId base) const;
    // 4.8: the byte adjustment a class-pointer conversion needs. A derived
    // pointer to a base adds the base's offset; a base pointer to a derived
    // subtracts it; an unrelated pair answers null (a plain reinterpreting
    // cast). Ambiguity answers null too: two subobjects would need a choice.
    std::optional<int64_t> classCastAdjust(DeclId from, DeclId to) const;
    // True when `base` is reachable through more than one base path.
    bool baseAmbiguous(DeclId derived, DeclId base) const;
    // 4.8: a name two bases both provide is an ambiguity, not a pick. An
    // own declaration hides every base's, so it is never ambiguous.
    bool methodAmbiguous(DeclId id, const std::string& name) const;
    bool fieldAmbiguous(DeclId id, const std::string& name) const;
    // 4.1: a bitfield's position inside its storage unit — the unit's byte
    // offset comes from fieldOffset; this answers the bit offset and width.
    // False when the field is not a bitfield or the name is unknown.
    bool bitField(DeclId id, const std::string& name, int64_t& bitOffset,
                  int64_t& width) const;
    // A union member's index in declaration order: the tag value written on
    // construction and compared on a checked read (TYPES.md 4.6). -1 when
    // the union or the member is unknown.
    int64_t unionMemberIndex(DeclId id, const std::string& name) const;
    // The byte offset of a union's tag: the payload padded to its own
    // alignment (TYPES.md 3.4a, layout::unionTagOffset). -1 when unknown.
    int64_t unionTagOffset(DeclId id) const;
    // The member a hasM name asks about: "hasI" names member "i"
    // (TYPES.md 4.6). Empty when the name is not a hasM spelling of a
    // member. One function so the checker and the lowerer cannot disagree
    // about the spelling.
    std::string unionHasMember(DeclId id, const std::string& hasName) const;
    // A type's size when the type may name a struct, class or union: plain
    // types answer from the type itself, a nominal answers from its
    // declaration. -1 when unknown. Interfaces are not sized here — their
    // layouts belong to later slices, and a guess would miscompile.
    int64_t nominalSize(const Type* type) const;
    // The same recursion for alignment.
    int64_t nominalAlign(const Type* type) const;
    // A class's direct bases, as declaration ids. Empty when none.
    std::vector<DeclId> classBases(DeclId id) const;
    // The virtual slots of a class, base-first: an override keeps its base
    // slot, a new `virtual` appends (FEATURES 2.3). Computed lazily from
    // the declaration flags, so declaration order inside a module cannot
    // matter. A chain with several bases at one level has no vtable yet
    // and answers empty; a name that is not virtual answers -1 from the
    // index. A non-empty list gains the destructor slot first (4.8.3), so
    // `delete` through a base pointer dispatches like any virtual call.
    std::vector<std::string> virtualNames(DeclId id) const;
    int64_t virtualIndexOf(DeclId id, const std::string& name) const;
    // The class that declares the method, walking the chain: a virtual call
    // and a static call both spell the declarer's link name for inherited
    // slots, and the override's class answers for its own redeclaration.
    std::optional<DeclId> methodHome(DeclId id, const std::string& name) const;
    EvalEnv evalEnv(const Scope* scope);

    const std::vector<ResolveError>& errors() const { return errors_; }

    // Builds a signature from parameter declarations. Public because the
    // checker rebuilds function signatures with it when walking bodies — one
    // function, called from two phases, never two rules.
    Signature buildSignature(const std::vector<Param>& params, const TypeExpr* returns,
                             const Scope* scope, bool& failed, Abi abi = Abi::SysV);
    // Binds one type parameter into a scope, for the checker's generic body
    // walks. The resolver owns every Decl; the checker borrows them.
    const Decl* bindTypeParam(const Scope* scope, const std::string& name,
                              DeclId templateId, uint32_t index, Position pos);
    // A child scope for one generic instantiation's parameters. Public for
    // the same reason: the checker walks template bodies under it.
    const Scope* typeParamScope(const Scope* parent, const std::string& module,
                                const std::vector<std::string>& names, DeclId templateId,
                                Position pos);
    // Specializes a template: struct, class, alias or function. Answers a
    // fresh declaration id whose member tables carry substituted types, and
    // caches it so one argument list means one specialization. Guards its own
    // recursion: self-applied templates name the depth instead of hanging.
    std::optional<DeclId> instantiate(DeclId tmpl, const std::vector<TypeRef>& args,
                                    Position pos);
    // A specialized function's substituted signature, by declaration id.
    // Null when the id names no specialization this table holds.
    const Signature* funcSig(DeclId id) const;
    // Where a specialization came from: template id plus argument types.
    // Empty when the id names no specialization this table holds.
    std::optional<std::pair<DeclId, std::vector<TypeRef>>> instantiationOf(
        DeclId spec) const;
    // True when the declaration id names a template instantiation.
    bool isSpecialization(DeclId id) const;
    // The declaration for an id, by scan. The table is small and lookups are
    // rare (templates and specializations only).
    const Decl* declById(DeclId id) const;
    // The link symbol a function declaration assembles under. The prelude
    // keeps its C name (`cider_print`), an extern keeps its bare spelling
    // (FEATURES §4.4: the linker defines it), an entry-module definition
    // keeps its spelling, and every other module's definition mangles under
    // its module (`b__aval` for `b`'s `aval`). One owner, one spelling:
    // the checker and every lowerer call site ask here instead of building
    // names locally.
    std::string linkName(const Decl& decl) const;
    // The link symbol for an instantiated generic function: the template's
    // home module flattened, its name, then the sanitized argument type
    // names (`id__S__int`, `box__id__S__int`). Specs own no Decl entries,
    // so linkName cannot see them; every emitter and call site asks here
    // instead. Empty when the id names no function specialization.
    std::string specLinkName(DeclId spec) const;
    // Replaces every TypeParam of one template with its argument types.
    // Nominal specializations re-originate: `Box<T>` inside `C<T>` becomes
    // `Box<int>` when C does, through specOrigin_ rather than spellings.
    TypeRef substitute(TypeRef type, DeclId tmpl, const std::vector<TypeRef>& args,
                       Position pos);
    Signature substSig(const Signature& sig, DeclId tmpl,
                       const std::vector<TypeRef>& args, Position pos);

private:
    void fail(Position pos, const std::string& message);
    // 8.4: maps an `msabi`/`sysv` spelling to its Abi. An unknown spelling is
    // an error, never a silent default.
    Abi abiOf(const std::string& spelling, bool& failed, Position pos);
    const Scope* makeScope(const Scope* parent, const std::string& module);
    Decl* bind(const Scope* scope, DeclKind kind, const std::string& name, Position pos);
    bool resolveModule(const std::string& name);
    void declareModule(const std::string& name, Program& program, const Scope* scope);
    void resolveDeclTypes(const std::string& name, Program& program, const Scope* scope);
    TypeRef resolveNamed(const NamedType* type, const Scope* scope);
    TypeRef resolveSignature(const FuncSigType* type, const Scope* scope, bool inPointer);
    // The slot list before the destructor rides: base-first class virtuals
    // only, so a chain with no user virtuals answers empty and forms no
    // table at all.
    std::vector<std::string> virtualSlots(DeclId id) const;
    // Validates one method's signature. The checker rebuilds it with this same
    // function when it walks the body — one function, called twice, never two
    // rules.
    void buildMethodSig(const MethodDecl& method, const Scope* scope, bool& failed);
    Signature buildFuncSig(const std::vector<std::unique_ptr<TypeExpr>>& params,
                           const TypeExpr* returns, const Scope* scope, bool& failed,
                           Abi abi = Abi::SysV);
    void bindPrelude(const Scope* scope, Position pos);
    // Size/alignment with a visited set: a struct naming itself (directly or
    // through friends) is -1, not a stack overflow. The checker rejects
    // uninhabitable types; the resolver still must not hang if one arrives.
    int64_t nominalSizeImpl(const Type* type, std::unordered_set<std::string>& seen) const;
    int64_t nominalAlignImpl(const Type* type, std::unordered_set<std::string>& seen) const;
    int64_t structSizeImpl(DeclId id, std::unordered_set<std::string>& seen) const;
    // 4.1: one walk owns a struct's storage layout — sizes, alignment,
    // field offsets and bit slots all read from it, so the rules cannot
    // drift apart. `slots` are in declaration order.
    struct StructSlot {
        std::string name;
        int64_t at = 0;
        int64_t bytes = 0;
        int64_t bitAt = -1;
        int64_t width = 0;
    };
    bool structSlots(DeclId id, std::unordered_set<std::string>& seen,
                     std::vector<StructSlot>& slots, int64_t& total,
                     int64_t& align) const;
    int64_t structAlignImpl(DeclId id, std::unordered_set<std::string>& seen) const;
    int64_t classSizeImpl(DeclId id, std::unordered_set<std::string>& seen) const;
    int64_t classAlignImpl(DeclId id, std::unordered_set<std::string>& seen) const;
    // 4.8: one walk owns a class's layout. Bases lay out in declaration
    // order, each at its own alignment; the first base shares the object's
    // vptr word when both carry tables, and otherwise starts after it. A
    // class's own fields follow its bases. `slots` are absolute offsets in
    // this class's view, each naming the class that declares it.
    struct ClassSlot {
        DeclId owner;
        std::string name;
        TypeRef type;
        int64_t at = 0;
    };
    struct ClassLayout {
        std::vector<ClassSlot> slots;
        std::vector<std::pair<DeclId, int64_t>> bases;
        int64_t multiple = 1;
        // The last byte the class's own fields occupy, before the final
        // rounding: a base subobject ends here in its parent's layout, so a
        // single base keeps the flat chain's packing (4.8).
        int64_t packed = 0;
        int64_t bytes = 0;
    };
    std::optional<ClassLayout> classLayoutImpl(
        DeclId id, std::unordered_set<std::string>& seen) const;
    // Walks every base path to `base`, summing offsets and counting paths.
    bool baseRelationImpl(DeclId derived, DeclId base, int64_t& at,
                          int& paths) const;
    int fieldPathsImpl(DeclId id, const std::string& name,
                       std::vector<std::string>& path) const;
    int methodPathsImpl(DeclId id, const std::string& name,
                        std::vector<std::string>& path) const;
    // The physical field order of a class with bases: most-base first, then
    // each class's own fields (FEATURES 2.2). A chain the layout cannot
    // name answers false and the size queries refuse rather than lay out
    // half an object.
    bool chainFieldsImpl(DeclId id, std::unordered_set<std::string>& seen,
                         std::vector<std::string>& out) const;
    // A field declared by a base: the chain walk names it, but fieldType
    // only holds a class's own fields, so the declaring class answers.
    TypeRef fieldTypeAcrossBases(DeclId id, const std::string& name) const;
    // The class in the chain that declares the field; none when no base
    // does. Visibility and the home module both hang off this owner.
    std::optional<DeclId> fieldOwner(DeclId id, const std::string& name) const;
    // A union's payload is its widest member, every member at offset 0, the
    // tag after the payload padded to the payload's alignment (TYPES.md
    // 3.4a). Same visited-set rule as structs: recursive unions are -1.
    int64_t unionSizeImpl(DeclId id, std::unordered_set<std::string>& seen) const;
    int64_t unionAlignImpl(DeclId id, std::unordered_set<std::string>& seen) const;
    static std::vector<const TypeExpr*> rawBases(const ClassDecl* decl);
    std::string qualify(const std::string& bare) const;

    TypeContext& ctx_;
    ModuleLoader load_;
    std::vector<std::unique_ptr<Decl>> decls_;
    std::vector<std::unique_ptr<Scope>> scopes_;
    std::unordered_map<std::string, const Scope*> modules_;
    std::unordered_map<std::string, uint32_t> moduleIds_;
    std::unordered_map<uint32_t, std::string> moduleNames_;
    // Fresh declaration ids per module, shared by declarations and template
    // specializations so the two can never collide.
    std::unordered_map<std::string, uint32_t> nextIndex_;
    // Where a specialization came from: template id plus argument types.
    // Substitution composes through this table instead of re-resolving
    // spellings the header pass no longer holds.
    std::unordered_map<std::string, std::pair<DeclId, std::vector<TypeRef>>> specOrigin_;
    // One argument list means one specialization.
    std::unordered_map<std::string, DeclId> specCache_;
    // Substituted signatures of specialized functions, by declaration id.
    std::unordered_map<std::string, Signature> funcSigs_;
    int instDepth_ = 0;
    std::unordered_map<std::string, std::unique_ptr<Program>> programs_;
    std::unordered_map<std::string, Program*> programOf_;
    std::unordered_map<const Node*, TypeRef> nodeTypes_;
    std::unordered_set<std::string> aliasActive_;
    std::unordered_map<std::string, const Expr*> constInits_;
    std::unordered_map<std::string, int64_t> enumValues_;
    std::unordered_map<std::string, std::vector<std::pair<std::string, int64_t>>> enumOrder_;
    std::unordered_map<std::string, TypeRef> fieldTypes_;
    // 4.8.4: only private fields get an entry; public is the default.
    std::unordered_set<std::string> privateFields_;
    std::unordered_map<std::string, Signature> methodSigs_;
    std::unordered_map<std::string, std::vector<std::string>> methodLists_;
    // Declared methods that carry `virtual`; the composed slot list is
    // virtualNames', and an override needs no entry here (it inherits its
    // base slot by name).
    std::unordered_map<std::string, bool> methodVirtual_;
    std::unordered_map<std::string, std::vector<std::string>> fieldLists_;
    // 4.1: a field's declared bit width ("a: int:3" — 3). Absent means a
    // plain field; -1 means the width expression did not fold.
    std::unordered_map<std::string, int64_t> fieldBits_;
    std::unordered_map<std::string, std::vector<DeclId>> baseTable_;
    std::vector<std::string> stack_;
    std::vector<ResolveError> errors_;
    std::vector<std::string> order_;
    const Scope* entry_ = nullptr;
    bool failed_ = false;
};

}  // namespace resolve
}  // namespace cider

#endif  // CIDER_RESOLVE_RESOLVER_H
