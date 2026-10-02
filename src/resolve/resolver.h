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

    const std::vector<ResolveError>& errors() const { return errors_; }

private:
    void fail(Position pos, const std::string& message);
    const Scope* makeScope(const Scope* parent, const std::string& module);
    Decl* bind(const Scope* scope, DeclKind kind, const std::string& name, Position pos);
    bool resolveModule(const std::string& name);
    void declareModule(const std::string& name, Program& program, const Scope* scope);
    void resolveDeclTypes(const std::string& name, Program& program, const Scope* scope);
    TypeRef resolveNamed(const NamedType* type, const Scope* scope);
    TypeRef resolveSignature(const FuncSigType* type, const Scope* scope, bool inPointer);
    Signature buildSignature(const std::vector<Param>& params, const TypeExpr* returns,
                             const Scope* scope, bool& failed);
    // Validates one method's signature. The checker rebuilds it with this same
    // function when it walks the body — one function, called twice, never two
    // rules.
    void buildMethodSig(const MethodDecl& method, const Scope* scope, bool& failed);
    Signature buildFuncSig(const std::vector<std::unique_ptr<TypeExpr>>& params,
                           const TypeExpr* returns, const Scope* scope, bool& failed);
    void bindPrelude(const Scope* scope, Position pos);
    const Scope* typeParamScope(const Scope* parent, const std::string& module,
                                const std::vector<std::string>& names, DeclId templateId,
                                Position pos);
    static std::vector<const TypeExpr*> rawBases(const ClassDecl* decl);
    EvalEnv evalEnv(const Scope* scope);
    std::string qualify(const std::string& bare) const;

    TypeContext& ctx_;
    ModuleLoader load_;
    std::vector<std::unique_ptr<Decl>> decls_;
    std::vector<std::unique_ptr<Scope>> scopes_;
    std::unordered_map<std::string, const Scope*> modules_;
    std::unordered_map<std::string, uint32_t> moduleIds_;
    std::unordered_map<std::string, std::unique_ptr<Program>> programs_;
    std::unordered_map<std::string, Program*> programOf_;
    std::unordered_map<const Node*, TypeRef> nodeTypes_;
    std::unordered_set<std::string> aliasActive_;
    std::unordered_map<std::string, const Expr*> constInits_;
    std::unordered_map<std::string, int64_t> enumValues_;
    std::vector<std::string> stack_;
    std::vector<ResolveError> errors_;
    const Scope* entry_ = nullptr;
    bool failed_ = false;
};

}  // namespace resolve
}  // namespace cider

#endif  // CIDER_RESOLVE_RESOLVER_H
