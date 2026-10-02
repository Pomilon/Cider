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
    bool isPrelude = false;
    // Const only: the initialiser, for evalConst. Non-owning; the AST
    // outlives every use the resolver makes of it.
    const Expr* init = nullptr;
    // Type only: how many explicit arguments an instantiation takes.
    uint64_t typeParams = 0;
    // Type only: which nominal Kind (Struct, Class, Interface, Union, Enum).
    Kind nominal = Kind::Error;
    // Alias only: the spelling it names. Non-owning; the AST outlives use.
    const TypeExpr* aliasTarget = nullptr;
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
