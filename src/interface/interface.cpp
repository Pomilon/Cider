// interface.cpp — render and read the `.cidm` declaration surface.

#include "interface/interface.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <sstream>

#include "types/kind.h"

namespace cider {
namespace interfacefmt {
namespace {

std::string intPrefix(uint8_t base) {
    switch (base) {
        case 16: return "0x";
        case 2: return "0b";
        case 8: return "0o";
        default: return "";
    }
}

std::string escapeChar(uint32_t value) {
    switch (value) {
        case 0: return "\\0";
        case '\n': return "\\n";
        case '\t': return "\\t";
        case '\r': return "\\r";
        case '\\': return "\\\\";
        case '\'': return "\\'";
        default: break;
    }
    if (value >= 32 && value < 127) {
        return std::string(1, static_cast<char>(value));
    }
    std::ostringstream out;
    out << "\\x";
    const char* digits = "0123456789abcdef";
    out << digits[(value >> 4) & 0xf] << digits[value & 0xf];
    return out.str();
}

std::string escapeText(const std::string& text) {
    std::string out;
    for (unsigned char c : text) {
        switch (c) {
            case 0: out += "\\0"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            default:
                if (c >= 32 && c < 127) {
                    out += static_cast<char>(c);
                } else {
                    const char* digits = "0123456789abcdef";
                    out += "\\x";
                    out += digits[(c >> 4) & 0xf];
                    out += digits[c & 0xf];
                }
                break;
        }
    }
    return out;
}

bool renderType(const TypeExpr& type, std::string& out);
bool renderExpr(const Expr& expr, std::string& out);

bool renderTypeList(const std::vector<std::unique_ptr<TypeExpr>>& types,
                    std::string& out) {
    bool first = true;
    for (const std::unique_ptr<TypeExpr>& type : types) {
        if (!first) {
            out += ", ";
        }
        first = false;
        if (type == nullptr || !renderType(*type, out)) {
            return false;
        }
    }
    return true;
}

bool renderType(const TypeExpr& type, std::string& out) {
    if (const auto* basic = dynamic_cast<const BasicType*>(&type)) {
        out += basicKindName(basic->basic);
        return true;
    }
    if (const auto* pointer = dynamic_cast<const PointerType*>(&type)) {
        out += "*";
        if (pointer->pointeeVolatile) {
            out += "volatile ";
        }
        return pointer->pointee != nullptr &&
               renderType(*pointer->pointee, out);
    }
    if (const auto* array = dynamic_cast<const ArrayType*>(&type)) {
        out += "[";
        if (array->element == nullptr || !renderType(*array->element, out)) {
            return false;
        }
        out += "; ";
        if (array->length == nullptr || !renderExpr(*array->length, out)) {
            return false;
        }
        out += "]";
        return true;
    }
    if (const auto* slice = dynamic_cast<const SliceType*>(&type)) {
        out += "[]";
        return slice->element != nullptr && renderType(*slice->element, out);
    }
    if (const auto* named = dynamic_cast<const NamedType*>(&type)) {
        if (!named->qualifier.empty()) {
            out += named->qualifier;
            out += ".";
        }
        out += named->name;
        if (!named->args.empty()) {
            out += "<";
            if (!renderTypeList(named->args, out)) {
                return false;
            }
            out += ">";
        }
        return true;
    }
    if (const auto* sig = dynamic_cast<const FuncSigType*>(&type)) {
        if (sig->abi == "msabi") {
            out += "msabi ";
        } else if (sig->abi == "sysv") {
            out += "sysv ";
        }
        out += "func(";
        if (!renderTypeList(sig->params, out)) {
            return false;
        }
        if (sig->variadic) {
            out += ", ...";
        }
        out += ")";
        if (sig->returns != nullptr) {
            out += " -> ";
            if (!renderType(*sig->returns, out)) {
                return false;
            }
        }
        return true;
    }
    return false;
}

std::string binOpText(BinOp op) {
    switch (op) {
        case BinOp::Add: return "+";
        case BinOp::Sub: return "-";
        case BinOp::Mul: return "*";
        case BinOp::Div: return "/";
        case BinOp::Rem: return "%";
        case BinOp::And: return "&";
        case BinOp::Or: return "|";
        case BinOp::Xor: return "^";
        case BinOp::Shl: return "<<";
        case BinOp::Shr: return ">>";
        case BinOp::Eq: return "==";
        case BinOp::Ne: return "!=";
        case BinOp::Lt: return "<";
        case BinOp::Le: return "<=";
        case BinOp::Gt: return ">";
        case BinOp::Ge: return ">=";
        case BinOp::AndAnd: return "&&";
        case BinOp::OrOr: return "||";
    }
    return "";
}

std::string unOpText(UnOp op) {
    switch (op) {
        case UnOp::Neg: return "-";
        case UnOp::Not: return "!";
        case UnOp::BitNot: return "~";
        case UnOp::Deref: return "*";
        case UnOp::Addr: return "&";
    }
    return "";
}

bool renderExpr(const Expr& expr, std::string& out) {
    if (const auto* lit = dynamic_cast<const IntLit*>(&expr)) {
        if (lit->negative) {
            out += "-";
        }
        out += intPrefix(lit->base);
        out += lit->digits;
        return true;
    }
    if (const auto* lit = dynamic_cast<const FloatLit*>(&expr)) {
        if (lit->negative) {
            out += "-";
        }
        out += lit->text;
        if (lit->narrow) {
            out += "f";
        }
        return true;
    }
    if (const auto* lit = dynamic_cast<const CharLit*>(&expr)) {
        out += "'";
        out += escapeChar(lit->value);
        out += "'";
        return true;
    }
    if (const auto* lit = dynamic_cast<const WideChar*>(&expr)) {
        out += "u'";
        out += escapeChar(lit->value);
        out += "'";
        return true;
    }
    if (const auto* lit = dynamic_cast<const StringLit*>(&expr)) {
        std::string text;
        for (const StringPartAst& part : lit->parts) {
            if (part.isExpr) {
                return false;
            }
            text += part.text;
        }
        out += "\"";
        out += escapeText(text);
        out += "\"";
        return true;
    }
    if (const auto* lit = dynamic_cast<const WideLit*>(&expr)) {
        out += "u\"";
        for (uint16_t unit : lit->units) {
            out += escapeChar(unit);
        }
        out += "\"";
        return true;
    }
    if (const auto* lit = dynamic_cast<const BoolLit*>(&expr)) {
        out += lit->value ? "true" : "false";
        return true;
    }
    if (dynamic_cast<const NilPtr*>(&expr) != nullptr) {
        out += "nilPtr";
        return true;
    }
    if (dynamic_cast<const UninitExpr*>(&expr) != nullptr) {
        out += "uninit";
        return true;
    }
    if (const auto* ident = dynamic_cast<const Ident*>(&expr)) {
        out += ident->name;
        return true;
    }
    if (const auto* binary = dynamic_cast<const Binary*>(&expr)) {
        std::string lhs;
        std::string rhs;
        if (!renderExpr(*binary->lhs, lhs) || !renderExpr(*binary->rhs, rhs)) {
            return false;
        }
        out += "(";
        out += lhs;
        out += " ";
        out += binOpText(binary->op);
        out += " ";
        out += rhs;
        out += ")";
        return true;
    }
    if (const auto* unary = dynamic_cast<const Unary*>(&expr)) {
        std::string operand;
        if (!renderExpr(*unary->operand, operand)) {
            return false;
        }
        out += "(";
        out += unOpText(unary->op);
        out += operand;
        out += ")";
        return true;
    }
    if (const auto* cast = dynamic_cast<const Cast*>(&expr)) {
        std::string inner;
        std::string type;
        if (!renderExpr(*cast->expr, inner) || !renderType(*cast->target, type)) {
            return false;
        }
        out += "(";
        out += inner;
        out += " as ";
        out += type;
        out += ")";
        return true;
    }
    if (const auto* ternary = dynamic_cast<const Ternary*>(&expr)) {
        std::string cond;
        std::string then;
        std::string els;
        if (!renderExpr(*ternary->cond, cond) ||
            !renderExpr(*ternary->then, then) ||
            !renderExpr(*ternary->els, els)) {
            return false;
        }
        out += "(";
        out += cond;
        out += " ? ";
        out += then;
        out += " : ";
        out += els;
        out += ")";
        return true;
    }
    if (const auto* field = dynamic_cast<const Field*>(&expr)) {
        std::string object;
        if (!renderExpr(*field->object, object)) {
            return false;
        }
        out += object;
        out += field->isArrow ? "->" : ".";
        out += field->name;
        return true;
    }
    if (const auto* index = dynamic_cast<const Index*>(&expr)) {
        std::string array;
        std::string at;
        if (!renderExpr(*index->array, array) || !renderExpr(*index->index, at)) {
            return false;
        }
        out += array;
        out += "[";
        out += at;
        out += "]";
        return true;
    }
    if (const auto* call = dynamic_cast<const Call*>(&expr)) {
        std::string callee;
        if (!renderExpr(*call->callee, callee)) {
            return false;
        }
        out += callee;
        if (!call->typeArgs.empty()) {
            out += "::<";
            if (!renderTypeList(call->typeArgs, out)) {
                return false;
            }
            out += ">";
        }
        out += "(";
        bool first = true;
        for (const std::unique_ptr<Expr>& arg : call->args) {
            if (!first) {
                out += ", ";
            }
            first = false;
            if (arg == nullptr || !renderExpr(*arg, out)) {
                return false;
            }
        }
        out += ")";
        return true;
    }
    if (const auto* size = dynamic_cast<const SizeofType*>(&expr)) {
        std::string type;
        if (!renderType(*size->type, type)) {
            return false;
        }
        out += "sizeof(";
        out += type;
        out += ")";
        return true;
    }
    if (const auto* size = dynamic_cast<const SizeofExpr*>(&expr)) {
        std::string inner;
        if (!renderExpr(*size->operand, inner)) {
            return false;
        }
        out += "sizeof(";
        out += inner;
        out += ")";
        return true;
    }
    if (const auto* align = dynamic_cast<const AlignofType*>(&expr)) {
        std::string type;
        if (!renderType(*align->type, type)) {
            return false;
        }
        out += "alignof(";
        out += type;
        out += ")";
        return true;
    }
    if (const auto* array = dynamic_cast<const ArrayLit*>(&expr)) {
        out += "[";
        bool first = true;
        for (const std::unique_ptr<Expr>& init : array->inits) {
            if (!first) {
                out += ", ";
            }
            first = false;
            if (init == nullptr || !renderExpr(*init, out)) {
                return false;
            }
        }
        if (array->repeat) {
            out += ", ...";
        }
        out += "]";
        return true;
    }
    if (const auto* construct = dynamic_cast<const NewStruct*>(&expr)) {
        if (construct->heap || construct->tryNew) {
            return false;
        }
        std::string type;
        if (!renderType(*construct->type, type)) {
            return false;
        }
        out += type;
        out += " { ";
        bool first = true;
        for (const FieldInit& field : construct->fields) {
            if (!first) {
                out += ", ";
            }
            first = false;
            out += field.name;
            out += ": ";
            if (field.value == nullptr || !renderExpr(*field.value, out)) {
                return false;
            }
        }
        out += " }";
        return true;
    }
    return false;
}

bool renderParams(const std::vector<Param>& params, bool variadic,
                  std::string& out) {
    out += "(";
    bool first = true;
    for (const Param& param : params) {
        if (!first) {
            out += ", ";
        }
        first = false;
        if (param.isMutable) {
            out += "var ";
        }
        out += param.name;
        out += ": ";
        if (param.type == nullptr || !renderType(*param.type, out)) {
            return false;
        }
    }
    // VARIADICS.md §9: a trailing `...` is part of the declaration surface.
    if (variadic) {
        if (!first) {
            out += ", ";
        }
        out += "...";
    }
    out += ")";
    return true;
}

bool renderFunctionTail(const std::vector<Param>& params,
                        const TypeExpr* returns, bool variadic,
                        std::string& out) {
    if (!renderParams(params, variadic, out)) {
        return false;
    }
    if (returns != nullptr) {
        out += " -> ";
        if (!renderType(*returns, out)) {
            return false;
        }
    }
    return true;
}

bool renderField(const FieldDecl& field, std::string& out) {
    if (field.isPrivate) {
        out += "private ";
    }
    out += field.name;
    out += ": ";
    if (field.isVolatile) {
        out += "volatile ";
    }
    if (field.type == nullptr || !renderType(*field.type, out)) {
        return false;
    }
    if (field.bitWidth != nullptr) {
        out += ": ";
        if (!renderExpr(*field.bitWidth, out)) {
            return false;
        }
    }
    if (field.init != nullptr) {
        out += " = ";
        if (!renderExpr(*field.init, out)) {
            return false;
        }
    }
    out += ";\n";
    return true;
}

bool renderMethod(const MethodDecl& method, std::string& out) {
    if (method.name == "init" || method.name == "deinit") {
        return false;
    }
    if (method.isPrivate) {
        out += "private ";
    }
    if (method.isVirtual) {
        out += "virtual ";
    }
    if (method.isOverride) {
        out += "override ";
    }
    out += "func ";
    out += method.name;
    if (!renderParams(method.params, false, out)) {
        return false;
    }
    if (method.returns != nullptr) {
        out += " -> ";
        if (!renderType(*method.returns, out)) {
            return false;
        }
    }
    out += ";\n";
    return true;
}

struct Renderer {
    std::ostringstream body;
    std::string requiresSource;

    void skip(const std::string& reason) {
        if (requiresSource.empty()) {
            requiresSource = reason;
        }
    }

    void renderImport(const Decl& decl) {
        if (const auto* import = dynamic_cast<const ImportDecl*>(&decl)) {
            body << "import " << import->module << ";\n";
            return;
        }
        if (const auto* from = dynamic_cast<const FromImport*>(&decl)) {
            body << "from " << from->module << " import ";
            bool first = true;
            for (const std::string& name : from->names) {
                if (!first) {
                    body << ", ";
                }
                first = false;
                body << name;
            }
            body << ";\n";
        }
    }

    void renderDecl(const Decl& decl) {
        if (const auto* exported = dynamic_cast<const ExportDecl*>(&decl)) {
            if (exported->decl != nullptr) {
                renderDecl(*exported->decl);
            }
            return;
        }
        if (dynamic_cast<const ImportDecl*>(&decl) != nullptr ||
            dynamic_cast<const FromImport*>(&decl) != nullptr ||
            dynamic_cast<const ModuleAsm*>(&decl) != nullptr) {
            return;
        }
        if (const auto* fn = dynamic_cast<const FuncDecl*>(&decl)) {
            if (!fn->typeParams.empty()) {
                skip("generic functions");
                return;
            }
            std::string text;
            if (fn->abi == "msabi") {
                text += "msabi ";
            } else if (fn->abi == "sysv") {
                text += "sysv ";
            }
            text += "func ";
            text += fn->name;
            if (!renderFunctionTail(fn->params, fn->returns.get(),
                                    fn->variadic, text)) {
                skip("function signature");
                return;
            }
            text += ";\n";
            body << text;
            return;
        }
        if (const auto* ext = dynamic_cast<const ExternFunc*>(&decl)) {
            std::string text;
            if (ext->abi == "msabi") {
                text += "msabi ";
            } else if (ext->abi == "sysv") {
                text += "sysv ";
            }
            text += "extern func ";
            text += ext->name;
            if (!renderParams(ext->params, ext->variadic, text)) {
                skip("extern signature");
                return;
            }
            if (ext->returns != nullptr) {
                text += " -> ";
                if (!renderType(*ext->returns, text)) {
                    skip("extern signature");
                    return;
                }
            }
            text += ";\n";
            body << text;
            return;
        }
        if (const auto* ext = dynamic_cast<const ExternVar*>(&decl)) {
            std::string type;
            if (ext->type == nullptr || !renderType(*ext->type, type)) {
                skip("extern variable type");
                return;
            }
            body << "extern var " << ext->name << ": " << type << ";\n";
            return;
        }
        if (const auto* constant = dynamic_cast<const ConstDecl*>(&decl)) {
            std::string text = "const ";
            text += constant->name;
            if (constant->type != nullptr) {
                std::string type;
                if (!renderType(*constant->type, type)) {
                    skip("constant type");
                    return;
                }
                text += ": ";
                text += type;
            }
            std::string value;
            if (constant->init == nullptr || !renderExpr(*constant->init, value)) {
                skip("constant initialiser");
                return;
            }
            text += " = ";
            text += value;
            text += ";\n";
            body << text;
            return;
        }
        if (const auto* structure = dynamic_cast<const StructDecl*>(&decl)) {
            if (!structure->typeParams.empty()) {
                skip("generic structs");
                return;
            }
            if (!structure->interfaces.empty()) {
                skip("interface implementations");
                return;
            }
            std::string text;
            if (structure->packed) {
                text += "@packed ";
            }
            text += "struct ";
            text += structure->name;
            text += " {\n";
            for (const FieldDecl& field : structure->fields) {
                if (!renderField(field, text)) {
                    skip("struct field");
                    return;
                }
            }
            for (const MethodDecl& method : structure->methods) {
                if (method.isVirtual) {
                    skip("virtual methods");
                    return;
                }
                if (!renderMethod(method, text)) {
                    skip("struct method");
                    return;
                }
            }
            text += "}\n";
            body << text;
            return;
        }
        if (dynamic_cast<const ClassDecl*>(&decl) != nullptr) {
            skip("classes");
            return;
        }
        if (dynamic_cast<const InterfaceDecl*>(&decl) != nullptr) {
            skip("interfaces");
            return;
        }
        if (dynamic_cast<const UnionDecl*>(&decl) != nullptr) {
            skip("unions");
            return;
        }
        if (const auto* enumeration = dynamic_cast<const EnumDecl*>(&decl)) {
            std::string text = "enum ";
            text += enumeration->name;
            text += " {\n";
            for (const EnumMember& member : enumeration->members) {
                text += "    ";
                text += member.name;
                if (member.value != nullptr) {
                    std::string value;
                    if (!renderExpr(*member.value, value)) {
                        skip("enumerator value");
                        return;
                    }
                    text += " = ";
                    text += value;
                }
                text += ",\n";
            }
            text += "}\n";
            body << text;
            return;
        }
        if (const auto* alias = dynamic_cast<const TypeAlias*>(&decl)) {
            if (!alias->typeParams.empty()) {
                skip("generic aliases");
                return;
            }
            std::string target;
            if (alias->target == nullptr || !renderType(*alias->target, target)) {
                skip("alias target");
                return;
            }
            body << "type " << alias->name << " = " << target << ";\n";
            return;
        }
        skip("unrecognised declaration");
    }

    void renderGlobal(const VarDecl& global) {
        std::string type;
        if (global.type == nullptr || !renderType(*global.type, type)) {
            skip("global type");
            return;
        }
        body << "var " << global.name << ": ";
        if (global.isVolatile) {
            body << "volatile ";
        }
        body << type << ";\n";
    }
};

// --- SHA-256 -----------------------------------------------------------------

constexpr uint32_t rotr(uint32_t value, uint32_t bits) {
    return (value >> bits) | (value << (32 - bits));
}

constexpr std::array<uint32_t, 64> kRound = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

void sha256Block(const uint8_t* block, std::array<uint32_t, 8>& state) {
    std::array<uint32_t, 64> words{};
    for (uint32_t i = 0; i < 16; ++i) {
        words[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
                   (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(block[i * 4 + 3]);
    }
    for (uint32_t i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(words[i - 15], 7) ^ rotr(words[i - 15], 18) ^
                      (words[i - 15] >> 3);
        uint32_t s1 = rotr(words[i - 2], 17) ^ rotr(words[i - 2], 19) ^
                      (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }
    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];
    uint32_t f = state[5];
    uint32_t g = state[6];
    uint32_t h = state[7];
    for (uint32_t i = 0; i < 64; ++i) {
        uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t choice = (e & f) ^ (~e & g);
        uint32_t temp1 = h + s1 + choice + kRound[i] + words[i];
        uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

}  // namespace

std::string sha256(const std::string& text) {
    std::array<uint32_t, 8> state = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };
    constexpr uint64_t kBitsPerByte = 8;
    constexpr uint64_t kWordBytes = 4;
    constexpr uint64_t kBlockBytes = 64;
    uint64_t bitLength = static_cast<uint64_t>(text.size()) * kBitsPerByte;
    uint64_t fullBlocks = static_cast<uint64_t>(text.size()) / kBlockBytes;
    for (uint64_t block = 0; block < fullBlocks; ++block) {
        sha256Block(
            reinterpret_cast<const uint8_t*>(text.data() + block * kBlockBytes),
            state);
    }
    uint64_t remainder = static_cast<uint64_t>(text.size()) % kBlockBytes;
    std::array<uint8_t, kBlockBytes> tail{};
    std::copy_n(reinterpret_cast<const uint8_t*>(text.data()) +
                    fullBlocks * kBlockBytes,
                static_cast<size_t>(remainder), tail.begin());
    tail[remainder] = 0x80;
    if (remainder + 1 + 2 * kWordBytes > kBlockBytes) {
        sha256Block(tail.data(), state);
        tail.fill(0);
    }
    for (uint32_t i = 0; i < kWordBytes; ++i) {
        tail[kBlockBytes - 1 - i] =
            static_cast<uint8_t>((bitLength >> (i * 8)) & 0xff);
    }
    sha256Block(tail.data(), state);
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (uint32_t word : state) {
        for (int shift = 28; shift >= 0; shift -= 4) {
            out += kHex[(word >> shift) & 0xf];
        }
    }
    return out;
}

std::string modulePath(const std::string& module) {
    std::string out = module;
    for (char& c : out) {
        if (c == '.') {
            c = '/';
        }
    }
    return out;
}

RenderResult render(const std::string& module, const Program& program,
                    const std::string& sourceHash,
                    const std::vector<Dependency>& deps) {
    Renderer renderer;
    for (const std::unique_ptr<Decl>& decl : program.decls) {
        if (dynamic_cast<const ImportDecl*>(decl.get()) != nullptr ||
            dynamic_cast<const FromImport*>(decl.get()) != nullptr) {
            renderer.renderImport(*decl);
        }
    }
    for (const std::unique_ptr<Decl>& decl : program.decls) {
        renderer.renderDecl(*decl);
    }
    for (const std::unique_ptr<VarDecl>& global : program.globals) {
        renderer.renderGlobal(*global);
    }
    std::ostringstream out;
    out << "// cider-interface: " << kFormatVersion << "\n";
    out << "// cider-interface-compiler: " << kCompilerTag << "\n";
    out << "// cider-interface-module: " << module << "\n";
    out << "// cider-interface-source: " << sourceHash << "\n";
    for (const Dependency& dep : deps) {
        out << "// cider-interface-dep: " << dep.module << " "
            << dep.hash << "\n";
    }
    if (!renderer.requiresSource.empty()) {
        out << "// cider-interface-requires-source: "
            << renderer.requiresSource << "\n";
    }
    out << renderer.body.str();
    RenderResult result;
    result.text = out.str();
    result.requiresSource = renderer.requiresSource;
    return result;
}

bool parseHeader(const std::string& text, Header& header, std::string& error) {
    const std::string prefix = "// cider-interface";
    constexpr auto kStart = 0;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.compare(kStart, prefix.size(), prefix) != kStart) {
            break;
        }
        auto valueAfter = [&](const std::string& key) {
            return line.substr(prefix.size() + key.size());
        };
        auto startsWith = [&](const std::string& key) {
            return line.compare(kStart, prefix.size() + key.size(),
                                prefix + key) == kStart;
        };
        if (startsWith(":")) {
            std::string value = valueAfter(":");
            while (!value.empty() &&
                   (value.front() == ' ' || value.front() == '\t')) {
                value.erase(value.begin());
            }
            try {
                header.format = std::stoi(value);
            } catch (...) {
                error = "malformed interface version line";
                return false;
            }
            continue;
        }
        if (startsWith("-compiler: ")) {
            header.compiler = valueAfter("-compiler: ");
            continue;
        }
        if (startsWith("-module: ")) {
            header.module = valueAfter("-module: ");
            continue;
        }
        if (startsWith("-source: ")) {
            header.sourceHash = valueAfter("-source: ");
            continue;
        }
        if (startsWith("-dep: ")) {
            std::istringstream fields(valueAfter("-dep: "));
            Dependency dep;
            fields >> dep.module;
            fields >> dep.hash;
            if (!dep.module.empty()) {
                header.deps.push_back(std::move(dep));
            }
            continue;
        }
        if (startsWith("-requires-source: ")) {
            header.requiresSource = valueAfter("-requires-source: ");
            continue;
        }
    }
    if (header.format == 0) {
        error = "missing interface version";
        return false;
    }
    if (header.module.empty()) {
        error = "missing interface module name";
        return false;
    }
    if (header.sourceHash.empty()) {
        error = "missing interface source hash";
        return false;
    }
    return true;
}

}  // namespace interfacefmt
}  // namespace cider
