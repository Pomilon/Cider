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
    if (magnitude > static_cast<unsigned __int128>(UINT64_MAX)) {
        return fail("integer literal does not fit");
    }
    if (magnitude > static_cast<unsigned __int128>(static_cast<uint64_t>(INT64_MAX))) {
        // u64 territory: the bits ride wrapped, flagged for the fit check
        // that knows whether the context can hold them.
        EvalResult out;
        out.ok = true;
        out.value = static_cast<int64_t>(static_cast<uint64_t>(magnitude));
        out.big = true;
        return out;
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
        int64_t bytes = env.sizeOf ? env.sizeOf(target.type) : typeSize(target.type);
        if (bytes < 0) {
            return fail("type has no size");
        }
        return EvalResult{true, bytes, ""};
    }
    if (const auto* al = dynamic_cast<const AlignofType*>(&expr)) {
        TypeRef target = env.resolve(al->type.get());
        if (target.isError()) {
            return fail("not a constant expression");
        }
        int64_t unit =
            env.alignOf ? env.alignOf(target.type) : alignOf(target.type);
        if (unit < 0) {
            return fail("type has no alignment");
        }
        return EvalResult{true, unit, ""};
    }
    return fail("not a constant expression");
}

namespace {

FloatResult floatFail(const std::string& error) {
    return FloatResult{false, 0.0, error};
}

// A literal decimal in double: one rounding, the same one the local path
// applies when it parses the literal at f64.
FloatResult floatLiteral(const std::string& text, bool negative) {
    double parsed = 0.0;
    try {
        parsed = std::stod(text);
    } catch (...) {
        return floatFail("not a float constant");
    }
    return FloatResult{true, negative ? -parsed : parsed, ""};
}

}  // namespace

FloatResult evalConstFloat(const Expr& expr, EvalEnv& env,
                           std::unordered_set<std::string>& active) {
    if (const auto* lit = dynamic_cast<const FloatLit*>(&expr)) {
        return floatLiteral(lit->text, lit->negative);
    }
    if (const auto* lit = dynamic_cast<const IntLit*>(&expr)) {
        EvalResult magnitude = literalValue(lit->base, lit->digits, lit->negative);
        if (!magnitude.ok || magnitude.big) {
            return floatFail("not a float constant");
        }
        return FloatResult{true, static_cast<double>(magnitude.value), ""};
    }
    if (dynamic_cast<const CharLit*>(&expr) != nullptr ||
        dynamic_cast<const BoolLit*>(&expr) != nullptr ||
        dynamic_cast<const StringLit*>(&expr) != nullptr ||
        dynamic_cast<const NilPtr*>(&expr) != nullptr) {
        return floatFail("not a float constant");
    }
    if (const auto* ident = dynamic_cast<const Ident*>(&expr)) {
        std::string qualified = env.qualify(ident->name);
        if (active.find(qualified) != active.end()) {
            return floatFail("constant cycle: " + qualified);
        }
        const Expr* init = env.constantInit(qualified);
        if (init == nullptr) {
            return floatFail(ident->name + " is not a constant");
        }
        active.insert(qualified);
        FloatResult folded = evalConstFloat(*init, env, active);
        if (!folded.ok) {
            EvalResult asInt = evalConstInt(*init, env, active);
            active.erase(qualified);
            if (!asInt.ok || asInt.big) {
                return floatFail(folded.error);
            }
            return FloatResult{true, static_cast<double>(asInt.value), ""};
        }
        active.erase(qualified);
        return folded;
    }
    if (const auto* unary = dynamic_cast<const Unary*>(&expr)) {
        // Integer semantics first: `-5` is the integer -5 (then -5.0),
        // matching the local path where the unary binds before any
        // conversion.
        EvalResult asInt = evalConstInt(expr, env, active);
        if (asInt.ok && !asInt.big) {
            return FloatResult{true, static_cast<double>(asInt.value), ""};
        }
        if (unary->op != UnOp::Neg) {
            return floatFail("not a float constant");
        }
        FloatResult inner = evalConstFloat(*unary->operand, env, active);
        if (!inner.ok) {
            return inner;
        }
        return FloatResult{true, -inner.value, ""};
    }
    if (const auto* binary = dynamic_cast<const Binary*>(&expr)) {
        // Same integer-first rule as the unary case: `5 / 2` divides as
        // integers (2, then 2.0) because both sides are integers, exactly
        // as the local path types the operation before converting.
        EvalResult asInt = evalConstInt(expr, env, active);
        if (asInt.ok && !asInt.big) {
            return FloatResult{true, static_cast<double>(asInt.value), ""};
        }
        if (binary->op != BinOp::Add && binary->op != BinOp::Sub &&
            binary->op != BinOp::Mul && binary->op != BinOp::Div) {
            return floatFail("not a float constant");
        }
        FloatResult lhs = evalConstFloat(*binary->lhs, env, active);
        if (!lhs.ok) {
            return lhs;
        }
        FloatResult rhs = evalConstFloat(*binary->rhs, env, active);
        if (!rhs.ok) {
            return rhs;
        }
        double out = 0.0;
        switch (binary->op) {
            case BinOp::Add:
                out = lhs.value + rhs.value;
                break;
            case BinOp::Sub:
                out = lhs.value - rhs.value;
                break;
            case BinOp::Mul:
                out = lhs.value * rhs.value;
                break;
            default:
                // Division is defined on floats (SEMANTICS): a zero divisor
                // is the IEEE answer, not a refusal.
                out = lhs.value / rhs.value;
                break;
        }
        return FloatResult{true, out, ""};
    }
    if (const auto* ternary = dynamic_cast<const Ternary*>(&expr)) {
        EvalResult cond = evalConstInt(*ternary->cond, env, active);
        if (!cond.ok) {
            return floatFail("not a float constant");
        }
        if (cond.value != 0) {
            return evalConstFloat(*ternary->then, env, active);
        }
        return evalConstFloat(*ternary->els, env, active);
    }
    if (const auto* cast = dynamic_cast<const Cast*>(&expr)) {
        TypeRef target = env.resolve(cast->target.get());
        if (target.isError() || target.type->kind() != Kind::Basic) {
            return floatFail("not a float constant");
        }
        std::optional<BasicKind> basic = target.type->basic();
        if (!basic.has_value() || !basicIsFloat(*basic)) {
            return floatFail("not a float constant");
        }
        EvalResult asInt = evalConstInt(*cast->expr, env, active);
        double operand = 0.0;
        if (asInt.ok && !asInt.big) {
            operand = static_cast<double>(asInt.value);
        } else {
            FloatResult inner = evalConstFloat(*cast->expr, env, active);
            if (!inner.ok) {
                return inner;
            }
            operand = inner.value;
        }
        // An `as float` rounds once to binary32 here, in the image's bits:
        // the global then matches the local path's runtime round exactly.
        if (*basic == BasicKind::F32) {
            operand = static_cast<double>(static_cast<float>(operand));
        }
        return FloatResult{true, operand, ""};
    }
    return floatFail("not a float constant");
}

}  // namespace cider
