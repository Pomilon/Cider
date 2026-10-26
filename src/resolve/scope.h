// scope.h — names and the scopes that hold them. LAYERS.md §3.3.
//
// A Scope is a tree of name-to-Decl bindings. A Decl is a declaration with its
// resolved Type: a variable, function, type, constant or module. The resolver
// owns every Decl and every Scope it makes; everyone else borrows.

#ifndef CIDER_RESOLVE_SCOPE_H
#define CIDER_RESOLVE_SCOPE_H

#include <string>
#include <unordered_map>

#include "ast/ast.h"
#include "lexer/token.h"
#include "types/signature.h"
#include "types/type.h"

namespace cider {
namespace resolve {

struct Scope;

enum class DeclKind {
    Var,
    Func,
    Type,
    Alias,
    Const,
    Module,
    TypeParam,
};

struct Decl {
    DeclKind kind = DeclKind::Var;
    std::string name;
    TypeRef type;
    Signature sig;
    DeclId id;
    const Scope* scope = nullptr;
    Position pos{"", 1, 1};
    // Extern only: declared in Cider, defined by the linker. Calls keep
    // the bare spelling on every backend (FEATURES §4.4): mangling one
    // would point at a symbol nothing defines.
    bool isExtern = false;
    // Module only: the full dotted path the import named (`a.b` even when
    // bound as `b`), so qualified uses find the defining scope.
    std::string origin;
    // Const only: the initialiser, for evalConst. Non-owning; the AST
    // outlives every use the resolver makes of it.
    const Expr* init = nullptr;
    // Type only: how many explicit arguments an instantiation takes.
    uint64_t typeParams = 0;
    // The parameter names, in order, for templates that name them (aliases
    // need them to bind arguments; structs and functions keep their AST).
    std::vector<std::string> paramNames;
    // Type only: which nominal Kind (Struct, Class, Interface, Union, Enum).
    Kind nominal = Kind::Error;
    // Struct only: 4.5's @packed spelling — no padding between fields.
    bool packed = false;
    // Alias only: the spelling it names. Non-owning; the AST outlives use.
    const TypeExpr* aliasTarget = nullptr;
    // Alias only, from-import: the defining module the target lives in
    // (`lib` for `from lib import A`). The resolver answers the target
    // there, not in the use scope — target names mean the defining
    // module's members. Empty for same-module aliases.
    std::string homeModule;
    // TypeParam only: the template it belongs to, and its position in that
    // template's parameter list.
    DeclId templateId;
    uint32_t paramIndex = 0;
};

struct Scope {
    const Scope* parent = nullptr;
    std::string module;
    std::unordered_map<std::string, const Decl*> bindings;
};

}  // namespace resolve
}  // namespace cider

#endif  // CIDER_RESOLVE_SCOPE_H
