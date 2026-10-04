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
    const Scope* scopeOf(const std::string& module) const;
    const Decl* lookup(const Scope* scope, const std::string& name) const;

    // TYPES.md §2.4, the total function: a spelling in a scope, answered with
    // a fully-resolved Type or Error. Error answers record one diagnostic at
    // the spelling and never cascade a second for the same use.
    TypeRef resolveType(const TypeExpr* spelling, const Scope* scope);

    // An enumerator's value, assigned C-style (0, then previous plus one).
    // -1 when no such member exists.
    int64_t enumValue(const std::string& qualifiedType, const std::string& member) const;

    // The resolved type of a node the header pass already answered (field and
    // parameter types, alias targets). The checker adds expression types to the
    // same table; one table, two writers in order, never two tables.
    TypeRef typeOf(const Node* node) const;
    void recordType(const Node* node, TypeRef type);
    // A field or member type by owning declaration and name. Error when no
    // such field exists.
    TypeRef fieldType(DeclId id, const std::string& name) const;
    // A method signature by owning declaration and name. Null when absent.
    const Signature* methodSig(DeclId id, const std::string& name) const;
    // Every method name of a struct, class or interface, in declaration
    // order. Empty for anything else.
    std::vector<std::string> methodNames(DeclId id) const;
    // Every field or member name of a struct, class or union, in declaration
    // order. Empty for anything else.
    std::vector<std::string> fieldNames(DeclId id) const;
    // The byte offset of a field within its struct, laid out in declaration
    // order with each field aligned to its own alignment (TYPES.md 3.4). -1
    // when the struct, the field, or any size on the way there is unknown.
    int64_t fieldOffset(DeclId id, const std::string& name) const;
    // A struct's total size: fields plus padding, rounded to its own
    // alignment, with no padding beyond that (TYPES.md 3.3). -1 when unknown.
    // Only structs: classes, unions and interfaces are refused elsewhere.
    int64_t structSize(DeclId id) const;
    // A class's direct bases, as declaration ids. Empty when none.
    std::vector<DeclId> classBases(DeclId id) const;
    EvalEnv evalEnv(const Scope* scope);

    const std::vector<ResolveError>& errors() const { return errors_; }

    // Builds a signature from parameter declarations. Public because the
    // checker rebuilds function signatures with it when walking bodies — one
    // function, called from two phases, never two rules.
    Signature buildSignature(const std::vector<Param>& params, const TypeExpr* returns,
                             const Scope* scope, bool& failed);
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
    // Replaces every TypeParam of one template with its argument types.
    // Nominal specializations re-originate: `Box<T>` inside `C<T>` becomes
    // `Box<int>` when C does, through specOrigin_ rather than spellings.
    TypeRef substitute(TypeRef type, DeclId tmpl, const std::vector<TypeRef>& args,
                       Position pos);
    Signature substSig(const Signature& sig, DeclId tmpl,
                       const std::vector<TypeRef>& args, Position pos);

private:
    void fail(Position pos, const std::string& message);
    const Scope* makeScope(const Scope* parent, const std::string& module);
    Decl* bind(const Scope* scope, DeclKind kind, const std::string& name, Position pos);
    bool resolveModule(const std::string& name);
    void declareModule(const std::string& name, Program& program, const Scope* scope);
    void resolveDeclTypes(const std::string& name, Program& program, const Scope* scope);
    TypeRef resolveNamed(const NamedType* type, const Scope* scope);
    TypeRef resolveSignature(const FuncSigType* type, const Scope* scope, bool inPointer);
    // Validates one method's signature. The checker rebuilds it with this same
    // function when it walks the body — one function, called twice, never two
    // rules.
    void buildMethodSig(const MethodDecl& method, const Scope* scope, bool& failed);
    Signature buildFuncSig(const std::vector<std::unique_ptr<TypeExpr>>& params,
                           const TypeExpr* returns, const Scope* scope, bool& failed);
    void bindPrelude(const Scope* scope, Position pos);
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
    std::unordered_map<std::string, TypeRef> fieldTypes_;
    std::unordered_map<std::string, Signature> methodSigs_;
    std::unordered_map<std::string, std::vector<std::string>> methodLists_;
    std::unordered_map<std::string, std::vector<std::string>> fieldLists_;
    std::unordered_map<std::string, std::vector<DeclId>> baseTable_;
    std::vector<std::string> stack_;
    std::vector<ResolveError> errors_;
    const Scope* entry_ = nullptr;
    bool failed_ = false;
};

}  // namespace resolve
}  // namespace cider

#endif  // CIDER_RESOLVE_RESOLVER_H
