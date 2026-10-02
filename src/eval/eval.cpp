// eval.cpp — one constant folder for source expressions.
//
// Everything here is exact: intermediate arithmetic runs in 128 bits and is
// range-checked, so folding can never wrap silently. A fold that would exceed
// int64 is a rejection naming the expression, not a wrap — the compiler
// refuses to guess what the programmer meant by a number that does not fit.

#include "eval/eval.h"

#include "types/type.h"

namespace cider {

namespace {

EvalResult fail(const std::string& error) {
    return EvalResult{false, 0, error};
}

// The magnitude of an integer literal. Digits are canonical (no `_`, no
// prefix), so this is a pure base conversion with a range check.
EvalResult literalValue(uint8_t base, const std::string& digits, bool negative) {
    unsigned __int128 magnitude = 0;
    for (char d : digits) {
        uint64_t digit = 0;
        if (d >= '0' && d <= '9') {
            digit = static_cast<uint64_t>(d - '0');
        } else if (d >= 'a' && d <= 'f') {
            digit = static_cast<uint64_t>(d - 'a') + 10;
        } else if (d >= 'A' && d <= 'F') {
            digit = static_cast<uint64_t>(d - 'A') + 10;
        } else {
            return fail("invalid digit in integer literal");
        }
        magnitude = magnitude * base + digit;
    }
    if (negative) {
        unsigned __int128 limit =
            static_cast<unsigned __int128>(static_cast<uint64_t>(INT64_MAX)) + 1;
        if (magnitude > limit) {
            return fail("integer literal does not fit");
        }
        if (magnitude == limit) {
            return EvalResult{true, INT64_MIN, ""};
        }
        return EvalResult{true, -static_cast<int64_t>(magnitude), ""};
    }
    if (magnitude > static_cast<unsigned __int128>(static_cast<uint64_t>(INT64_MAX))) {
        return fail("integer literal does not fit");
    }
    return EvalResult{true, static_cast<int64_t>(magnitude), ""};
}

EvalResult foldBinary(BinOp op, int64_t lhs, int64_t rhs) {
    __int128 a = lhs;
    __int128 b = rhs;
    auto inRange = [](__int128 v) {
        return v >= static_cast<__int128>(INT64_MIN) &&
               v <= static_cast<__int128>(INT64_MAX);
    };
    switch (op) {
        case BinOp::Add:
        case BinOp::Sub:
        case BinOp::Mul: {
            __int128 out = op == BinOp::Add ? a + b : (op == BinOp::Sub ? a - b : a * b);
            if (!inRange(out)) {
                return fail("constant arithmetic overflows");
            }
            return EvalResult{true, static_cast<int64_t>(out), ""};
        }
        case BinOp::Div:
        case BinOp::Rem:
            if (b == 0) {
                return fail("division by zero in constant expression");
            }
            if (a == static_cast<__int128>(INT64_MIN) && b == -1) {
                return fail("constant arithmetic overflows");
            }
            return EvalResult{true,
                             static_cast<int64_t>(op == BinOp::Div ? a / b : a % b), ""};
        case BinOp::And:
            return EvalResult{true, lhs & rhs, ""};
        case BinOp::Or:
            return EvalResult{true, lhs | rhs, ""};
        case BinOp::Xor:
            return EvalResult{true, lhs ^ rhs, ""};
        case BinOp::Shl:
        case BinOp::Shr: {
            if (b < 0 || b >= 64) {
                return fail("shift count out of range in constant expression");
            }
            if (op == BinOp::Shl) {
                __int128 out = a << static_cast<int>(b);
                if (!inRange(out)) {
                    return fail("constant arithmetic overflows");
                }
                return EvalResult{true, static_cast<int64_t>(out), ""};
            }
            return EvalResult{true, lhs >> static_cast<int>(b), ""};
        }
        case BinOp::Eq:
            return EvalResult{true, lhs == rhs ? 1 : 0, ""};
        case BinOp::Ne:
            return EvalResult{true, lhs != rhs ? 1 : 0, ""};
        case BinOp::Lt:
            return EvalResult{true, lhs < rhs ? 1 : 0, ""};
        case BinOp::Le:
            return EvalResult{true, lhs <= rhs ? 1 : 0, ""};
        case BinOp::Gt:
            return EvalResult{true, lhs > rhs ? 1 : 0, ""};
        case BinOp::Ge:
            return EvalResult{true, lhs >= rhs ? 1 : 0, ""};
        case BinOp::AndAnd:
            return EvalResult{true, (lhs != 0 && rhs != 0) ? 1 : 0, ""};
        case BinOp::OrOr:
            return EvalResult{true, (lhs != 0 || rhs != 0) ? 1 : 0, ""};
    }
    return fail("not a constant expression");
}

}  // namespace

EvalResult evalConstInt(const Expr& expr, EvalEnv& env,
                        std::unordered_set<std::string>& active) {
    if (const auto* lit = dynamic_cast<const IntLit*>(&expr)) {
        return literalValue(lit->base, lit->digits, lit->negative);
    }
    if (const auto* lit = dynamic_cast<const CharLit*>(&expr)) {
        return EvalResult{true, static_cast<int64_t>(lit->value), ""};
    }
    if (const auto* lit = dynamic_cast<const BoolLit*>(&expr)) {
        return EvalResult{true, lit->value ? 1 : 0, ""};
    }
    if (dynamic_cast<const FloatLit*>(&expr) != nullptr ||
        dynamic_cast<const StringLit*>(&expr) != nullptr ||
        dynamic_cast<const NilPtr*>(&expr) != nullptr) {
        return fail("not an integer constant");
    }
    if (const auto* ident = dynamic_cast<const Ident*>(&expr)) {
        std::string qualified = env.qualify(ident->name);
        if (active.find(qualified) != active.end()) {
            std::string cycle = qualified;
            for (const std::string& name : active) {
                cycle += " -> " + name;
            }
            return fail("constant cycle: " + cycle);
        }
        const Expr* init = env.constantInit(qualified);
        if (init == nullptr) {
            return fail(ident->name + " is not a constant");
        }
        active.insert(qualified);
        EvalResult folded = evalConstInt(*init, env, active);
        active.erase(qualified);
        return folded;
    }
    if (const auto* unary = dynamic_cast<const Unary*>(&expr)) {
        EvalResult inner = evalConstInt(*unary->operand, env, active);
        if (!inner.ok) {
            return inner;
        }
        if (unary->op == UnOp::Neg) {
            if (inner.value == INT64_MIN) {
                return fail("constant arithmetic overflows");
            }
            return EvalResult{true, -inner.value, ""};
        }
        if (unary->op == UnOp::Not) {
            return EvalResult{true, inner.value == 0 ? 1 : 0, ""};
        }
        if (unary->op == UnOp::BitNot) {
            return EvalResult{true, ~inner.value, ""};
        }
        return fail("not a constant expression");
    }
    if (const auto* binary = dynamic_cast<const Binary*>(&expr)) {
        EvalResult lhs = evalConstInt(*binary->lhs, env, active);
        if (!lhs.ok) {
            return lhs;
        }
        EvalResult rhs = evalConstInt(*binary->rhs, env, active);
        if (!rhs.ok) {
            return rhs;
        }
        return foldBinary(binary->op, lhs.value, rhs.value);
    }
    if (const auto* ternary = dynamic_cast<const Ternary*>(&expr)) {
        EvalResult cond = evalConstInt(*ternary->cond, env, active);
        if (!cond.ok) {
            return cond;
        }
        // Only the taken branch folds: the other may not be constant, and at
        // runtime it never runs — the same reason `and`/`or` skip their right
        // side, applied to folding instead of evaluating.
        if (cond.value != 0) {
            return evalConstInt(*ternary->then, env, active);
        }
        return evalConstInt(*ternary->els, env, active);
    }
    if (const auto* cast = dynamic_cast<const Cast*>(&expr)) {
        EvalResult inner = evalConstInt(*cast->expr, env, active);
        if (!inner.ok) {
            return inner;
        }
        TypeRef target = env.resolve(cast->target.get());
        if (target.isError()) {
            return fail("not a constant expression");
        }
        if (target.type->kind() != Kind::Basic) {
            return fail("not an integer constant");
        }
        std::optional<BasicKind> basic = target.type->basic();
        int64_t width = 0;
        bool isSigned = true;
        if (*basic == BasicKind::I8 || *basic == BasicKind::U8) {
            width = 8;
            isSigned = *basic == BasicKind::I8;
        } else if (*basic == BasicKind::I16 || *basic == BasicKind::U16) {
            width = 16;
            isSigned = *basic == BasicKind::I16;
        } else if (*basic == BasicKind::I32 || *basic == BasicKind::U32) {
            width = 32;
            isSigned = *basic == BasicKind::I32;
        } else if (*basic == BasicKind::I64 || *basic == BasicKind::U64) {
            width = 64;
            isSigned = *basic == BasicKind::I64;
        } else if (*basic == BasicKind::Bool || *basic == BasicKind::Char) {
            return EvalResult{true, inner.value == 0 ? 0 : 1, ""};
        } else {
            return fail("not an integer constant");
        }
        uint64_t mask = width == 64 ? ~static_cast<uint64_t>(0)
                                    : (static_cast<uint64_t>(1) << width) - 1;
        uint64_t kept = static_cast<uint64_t>(inner.value) & mask;
        if (!isSigned) {
            return EvalResult{true, static_cast<int64_t>(kept), ""};
        }
        uint64_t sign = static_cast<uint64_t>(1) << (width - 1);
        if ((kept & sign) != 0) {
            kept |= ~mask;
        }
        return EvalResult{true, static_cast<int64_t>(kept), ""};
    }
    if (const auto* sz = dynamic_cast<const SizeofType*>(&expr)) {
        TypeRef target = env.resolve(sz->type.get());
        if (target.isError()) {
            return fail("not a constant expression");
        }
        int64_t bytes = typeSize(target.type);
        if (bytes < 0) {
            return fail("type has no size");
        }
        return EvalResult{true, bytes, ""};
    }
    return fail("not a constant expression");
}

}  // namespace cider
