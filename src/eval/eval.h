// eval.h — one constant folder for source expressions. LAYERS.md §3.4.
//
// Array lengths, `const` initialisers, enumerator values, bitfield widths,
// `sizeof` and every "provable" in SEMANTICS.md all need the same thing: the
// value of an expression known at compile time. Three places wanted it — the
// resolver, the checker and the optimiser — which is the two-authorities shape
// the layers exist to prevent. So there is one function here, shared by all
// three, and the only one that folds source.
//
// evalConstInt answers an integer or an error naming why not. It never traps,
// never hangs on a `const` cycle (those are rejected naming the cycle), and
// never reads a value that is not a constant: a name that is not a `const` is
// an error, not a zero.
//
// The environment is three callables, so the resolver, the checker and the
// optimiser each supply their own const table and their own resolver without
// this file depending on any of them.

#ifndef CIDER_EVAL_EVAL_H
#define CIDER_EVAL_EVAL_H

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>

#include "ast/ast.h"
#include "types/type.h"

namespace cider {

struct EvalEnv {
    TypeContext& types;
    // The initialiser of a qualified constant (`mod.name`), or null when no
    // such constant exists. Non-owning.
    std::function<const Expr*(const std::string& qualified)> constantInit;
    // The resolved type of a spelling, or Error. Used only for `sizeof`.
    std::function<TypeRef(const TypeExpr* spelling)> resolve;
    // Qualifies a bare name in the current module (`x` -> `mod.x`).
    std::function<std::string(const std::string& bare)> qualify;
};

struct EvalResult {
    bool ok = false;
    int64_t value = 0;
    std::string error;
};

// Folds an integer constant expression. `active` holds the qualified constants
// already being folded above this call; a name that re-enters is a cycle and
// is rejected naming it.
EvalResult evalConstInt(const Expr& expr, EvalEnv& env,
                        std::unordered_set<std::string>& active);

}  // namespace cider

#endif  // CIDER_EVAL_EVAL_H
