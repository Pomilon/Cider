#pragma once

// CIDER-FEAT-006: `.cider-meta` module snapshots (separate compilation).
//
// Implements the text format from
// docs/superpowers/specs/2026-07-03-separate-compilation-design.md:
//
//   module std.io
//   dep std.string
//   export func strlen(s: *char) -> int { }
//   template<T> export func new_vec<T>() -> Vec<T> { ... }
//   template<T> export struct Vec<T> { data: *T; len: int; cap: int }
//
// The header lines (`module`, `dep`) are the only non-Cider syntax. The
// declaration text is ordinary Cider source, so the loader parses it with
// the standard Lexer/Parser and collects it with the standard
// SymbolCollector. Function declarations carry an empty `{ }` body and are
// marked as meta declarations (Statement::setMetaDecl) so IR generation
// emits an `extern` reference instead of a body; template declarations
// keep their real bodies because instantiation happens at the use site.
//
// The emitter writes every top-level declaration (the exported ones with
// an `export` prefix, private ones without) so that exported signatures
// can reference private helper types and constant folding keeps resolving
// private consts. Module privacy is unchanged: the loader preserves each
// declaration's exported flag.

#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

#include "ast.h"
#include "symbols.h"

namespace cider {

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4). The spec writes `<module>.hash` with a SHA256 of the
// source file for change detection; hash-based invalidation itself is Phase 1
// out-of-scope, so the compiler only records the digest.
// ---------------------------------------------------------------------------

namespace meta_detail {

struct Sha256 {
    uint32_t state[8];
    uint64_t bitlen = 0;
    uint8_t buffer[64];
    size_t buflen = 0;

    static uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

    void init() {
        static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                       0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        std::memcpy(state, iv, sizeof(iv));
        bitlen = 0;
        buflen = 0;
    }

    void transform(const uint8_t* chunk) {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
            0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
            0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
            0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
            0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
            0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
            0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
            0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t(chunk[i * 4]) << 24) | (uint32_t(chunk[i * 4 + 1]) << 16) |
                   (uint32_t(chunk[i * 4 + 2]) << 8) | uint32_t(chunk[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = h + s1 + ch + k[i] + w[i];
            uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = s0 + maj;
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }

    void update(const uint8_t* data, size_t len) {
        for (size_t i = 0; i < len; ++i) {
            buffer[buflen++] = data[i];
            if (buflen == 64) {
                transform(buffer);
                bitlen += 512;
                buflen = 0;
            }
        }
    }

    void final(uint8_t out[32]) {
        uint64_t bits = bitlen + (uint64_t)buflen * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0;
        while (buflen != 56) update(&zero, 1);
        uint8_t lenBytes[8];
        for (int i = 0; i < 8; ++i) lenBytes[i] = (uint8_t)(bits >> (56 - i * 8));
        update(lenBytes, 8);
        for (int i = 0; i < 8; ++i) {
            out[i * 4] = (uint8_t)(state[i] >> 24);
            out[i * 4 + 1] = (uint8_t)(state[i] >> 16);
            out[i * 4 + 2] = (uint8_t)(state[i] >> 8);
            out[i * 4 + 3] = (uint8_t)state[i];
        }
    }
};

inline std::string trimCopy(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) b++;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) e--;
    return s.substr(b, e - b);
}

inline std::string dirNameOf(const std::string& path) {
    auto slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

}  // namespace meta_detail

inline std::string sha256Hex(const std::string& data) {
    meta_detail::Sha256 sha;
    sha.init();
    sha.update(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    uint8_t digest[32];
    sha.final(digest);
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (int i = 0; i < 32; ++i) {
        out += hex[digest[i] >> 4];
        out += hex[digest[i] & 0xF];
    }
    return out;
}

// Canonical absolute path (falls back to the input when it does not exist).
inline std::string canonicalPath(const std::string& path) {
    char buf[PATH_MAX];
    if (realpath(path.c_str(), buf) != nullptr) return std::string(buf);
    return path;
}

// Directory of the running binary, for the module-root candidates that mirror
// ModuleResolver's binary-relative search paths.
inline std::string metaBinaryDir() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return "";
    buf[n] = '\0';
    std::string exe(buf);
    size_t slash = exe.find_last_of('/');
    return (slash == std::string::npos) ? "" : exe.substr(0, slash + 1);
}

// CIDER-FEAT-006: logical module name for a file passed to `--emit-meta`.
// Mirrors ModuleResolver's search roots: a file under a `modules/` root is
// named by its dotted path relative to that root (`modules/std/io.cid` ->
// `std.io`), a package `__init__.cid` drops the file part, and files outside
// every root fall back to the project root / the file's own directory. The
// `<module>.cider-meta` key the importer checks is this same name.
inline std::string deriveModuleName(const std::string& inputPath) {
    std::string abs = canonicalPath(inputPath);
    std::string dir = meta_detail::dirNameOf(inputPath);

    // Module roots first: these are the directories importers resolve
    // logical names against, so they win over the file's own directory.
    std::vector<std::string> roots;
    roots.push_back(dir + "/modules/");
    roots.push_back("./modules/");
    roots.push_back("./Cider/modules/");
    std::string binDir = metaBinaryDir();
    if (!binDir.empty()) {
        roots.push_back(binDir + "../modules/");
        roots.push_back(binDir + "../../modules/");
    }
    // Fallbacks: the project root and the file's own directory.
    std::vector<std::string> fallbacks;
    fallbacks.push_back("./");
    fallbacks.push_back(dir + "/");

    auto logicalFromRoot = [&](const std::string& root) -> std::string {
        std::string absRoot = canonicalPath(root);
        if (absRoot.empty() || absRoot.back() != '/') absRoot += "/";
        if (abs.compare(0, absRoot.size(), absRoot) != 0) return "";
        std::string rel = abs.substr(absRoot.size());
        if (rel.empty()) return "";
        const std::string ext = ".cid";
        if (rel.size() > ext.size() && rel.compare(rel.size() - ext.size(), ext.size(), ext) == 0) {
            rel = rel.substr(0, rel.size() - ext.size());
        } else {
            return "";
        }
        const std::string initSuffix = "/__init__";
        if (rel.size() >= initSuffix.size() &&
            rel.compare(rel.size() - initSuffix.size(), initSuffix.size(), initSuffix) == 0) {
            rel = rel.substr(0, rel.size() - initSuffix.size());
        }
        std::string logical = rel;
        for (char& c : logical) {
            if (c == '/') c = '.';
        }
        return logical;
    };

    auto bestFrom = [&](const std::vector<std::string>& candidates) -> std::string {
        std::string best;
        size_t bestLen = 0;
        for (const auto& root : candidates) {
            std::string logical = logicalFromRoot(root);
            if (logical.empty()) continue;
            size_t len = canonicalPath(root).size();
            if (len > bestLen) {
                best = logical;
                bestLen = len;
            }
        }
        return best;
    };

    std::string best = bestFrom(roots);
    if (best.empty()) best = bestFrom(fallbacks);
    if (!best.empty()) return best;
    std::string base = abs;
    auto slash = base.find_last_of('/');
    if (slash != std::string::npos) base = base.substr(slash + 1);
    if (base.size() > 4 && base.compare(base.size() - 4, 4, ".cid") == 0) base = base.substr(0, base.size() - 4);
    return base;
}

// Cache file name for a logical module name: `<module>.cider-meta`. Slashes
// and anything outside the safe set are escaped so nested package names stay
// one flat file.
inline std::string metaFileName(const std::string& logicalName, const char* suffix) {
    std::string safe;
    for (char c : logicalName) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '-') {
            safe += c;
        } else {
            safe += '_';
        }
    }
    if (safe.empty()) safe = "module";
    return safe + suffix;
}

// ---------------------------------------------------------------------------
// Meta text: parsing headers, translating template declarations, extracting
// declaration source spans.
// ---------------------------------------------------------------------------

struct MetaFile {
    std::string moduleName;
    std::vector<std::string> deps;
    std::string declarations;
};

// Parse `module <name>` / `dep <name>` header lines followed by declaration
// source. Returns false when the mandatory `module` line is missing.
inline bool parseMetaText(const std::string& text, MetaFile& out) {
    std::istringstream ss(text);
    std::string line;
    bool sawModule = false;
    bool inDecls = false;
    while (std::getline(ss, line)) {
        std::string t = meta_detail::trimCopy(line);
        if (!inDecls) {
            if (!sawModule) {
                if (t.empty()) continue;  // tolerate leading blank lines
                const std::string prefix = "module";
                if (t.compare(0, prefix.size(), prefix) != 0 ||
                    (t.size() > prefix.size() && t[prefix.size()] != ' ' && t[prefix.size()] != '\t')) {
                    return false;
                }
                out.moduleName = meta_detail::trimCopy(t.substr(prefix.size()));
                if (out.moduleName.empty()) return false;
                sawModule = true;
                continue;
            }
            if (t.compare(0, 4, "dep ") == 0 || t.compare(0, 4, "dep\t") == 0) {
                std::string dep = meta_detail::trimCopy(t.substr(4));
                if (!dep.empty()) out.deps.push_back(dep);
                continue;
            }
            if (t.empty()) continue;
            inDecls = true;
        }
        out.declarations += line;
        out.declarations += '\n';
    }
    return sawModule;
}

// Translate spec-style `template<T> export ...` lines into plain Cider
// source (`export ...`); the `<T>` list is already part of the declaration
// name. Lines that do not start with `template<` pass through untouched.
inline std::string translateMetaDeclarations(const std::string& declarations) {
    std::istringstream ss(declarations);
    std::string line;
    std::string out;
    while (std::getline(ss, line)) {
        size_t start = 0;
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) start++;
        if (line.compare(start, 9, "template<") == 0) {
            int depth = 0;
            size_t i = start + 8;  // at '<'
            for (; i < line.size(); ++i) {
                if (line[i] == '<') depth++;
                else if (line[i] == '>') {
                    if (--depth == 0) break;
                }
            }
            if (i < line.size() && depth == 0) {
                size_t rest = i + 1;
                while (rest < line.size() && (line[rest] == ' ' || line[rest] == '\t')) rest++;
                out += line.substr(rest);
                out += '\n';
                continue;
            }
        }
        out += line;
        out += '\n';
    }
    return out;
}

// Byte offset of a 1-based (line, col) position, or npos.
inline size_t metaOffsetFor(const std::string& src, int line, int col) {
    size_t off = 0;
    int cur = 1;
    while (cur < line) {
        size_t nl = src.find('\n', off);
        if (nl == std::string::npos) return std::string::npos;
        off = nl + 1;
        cur++;
    }
    off += (size_t)(col > 0 ? col - 1 : 0);
    return off >= src.size() ? std::string::npos : off;
}

// Extract the source text of a declaration starting at (line, col). The scan
// stops at the matching top-level '}' or at a top-level ';'. When `stripBody`
// is set (plain function declarations), the body block is replaced by `{ }`
// so the snapshot stays declaration-only. Strings, chars and comments are
// skipped, so braces inside them never affect the depth.
inline std::string extractDeclSource(const std::string& src, int line, int col,
                                     bool stripBody) {
    size_t start = metaOffsetFor(src, line, col);
    if (start == std::string::npos) return "";
    size_t i = start;
    int depth = 0;
    size_t bodyOpen = std::string::npos;
    size_t end = std::string::npos;
    while (i < src.size()) {
        char c = src[i];
        if (c == '/' && i + 1 < src.size() && src[i + 1] == '/') {
            size_t nl = src.find('\n', i);
            i = (nl == std::string::npos) ? src.size() : nl;
            continue;
        }
        if (c == '/' && i + 1 < src.size() && src[i + 1] == '*') {
            size_t close = src.find("*/", i + 2);
            i = (close == std::string::npos) ? src.size() : close + 2;
            continue;
        }
        if (c == '"' || c == '\'') {
            char quote = c;
            i++;
            while (i < src.size()) {
                if (src[i] == '\\' && i + 1 < src.size()) {
                    i += 2;
                    continue;
                }
                if (src[i] == quote) {
                    i++;
                    break;
                }
                i++;
            }
            continue;
        }
        if (c == '(' || c == '[' || c == '{') {
            if (c == '{' && depth == 0 && bodyOpen == std::string::npos) bodyOpen = i;
            depth++;
            i++;
            continue;
        }
        if (c == ')' || c == ']' || c == '}') {
            depth--;
            i++;
            if (depth == 0 && c == '}') {
                end = i;
                break;
            }
            continue;
        }
        if (c == ';' && depth == 0) {
            i++;
            end = i;
            break;
        }
        i++;
    }
    if (end == std::string::npos) end = i;
    if (stripBody && bodyOpen != std::string::npos && bodyOpen < end) {
        std::string head = src.substr(start, bodyOpen - start);
        return head + " { }";
    }
    return src.substr(start, end - start);
}

// ---------------------------------------------------------------------------
// Emitter: build the `.cider-meta` text for one compiled module.
// ---------------------------------------------------------------------------

struct MetaEmitResult {
    std::string text;  // `.cider-meta` content
    std::string hash;  // SHA-256 of the module source
};

class MetaEmitter {
public:
    // `moduleScope` holds the module's own symbols (root scope for a main
    // file); `templates` is the collector's template table. Every top-level
    // declaration is written, with `export` preserved from the symbol flags.
    static MetaEmitResult emit(
        AstProgram& program, const std::string& sourceText, const std::string& moduleName,
        Scope* moduleScope,
        const std::map<std::string, std::shared_ptr<TemplateSymbol>>& templates) {
        MetaEmitResult result;
        result.hash = sha256Hex(sourceText);

        std::vector<std::string> deps;
        std::vector<std::string> decls;
        for (auto& stmt : program.getStatements()) {
            Statement* inner = stmt.get();
            if (auto es = dynamic_cast<ExportStmt*>(inner)) inner = es->getStmt();
            if (!inner) continue;
            switch (inner->getType()) {
                case ASTNode::IMPORT_STMT: {
                    auto* imp = static_cast<ImportStmt*>(inner);
                    addDep(deps, imp->getModuleName());
                    break;
                }
                case ASTNode::FROM_IMPORT_STMT: {
                    auto* imp = static_cast<FromImportStmt*>(inner);
                    addDep(deps, imp->getModuleName());
                    decls.push_back(extractDeclSource(sourceText, inner->getLine(),
                                                      inner->getColumn(), false));
                    break;
                }
                case ASTNode::EXPORT_EXPRESSION_STMT:
                case ASTNode::BLOCK_STMT:
                    break;
                case ASTNode::FUNCTION_DECL_STMT: {
                    auto* fd = static_cast<FunctionDeclStmt*>(inner);
                    if (!fd->hasTemplateParams() && fd->getName().find('<') != std::string::npos) {
                        break;  // instantiation clone: the template snapshot covers it
                    }
                    bool isTemplate = fd->hasTemplateParams();
                    bool exported = isTemplate
                                        ? templateIsExported(templates, moduleName, fd->getName(),
                                                             fd->getTemplateParams())
                                        : symbolIsExported(moduleScope, fd->getName());
                    if (!exported && !isTemplate && !symbolExists(moduleScope, fd->getName())) {
                        break;
                    }
                    // Template bodies are kept: instantiation happens at the
                    // use site. Plain declarations are body-less.
                    std::string text = extractDeclSource(sourceText, inner->getLine(),
                                                         inner->getColumn(),
                                                         !fd->isExtern() && !isTemplate);
                    if (fd->isExtern()) {
                        decls.push_back(text);
                    } else if (isTemplate) {
                        decls.push_back("template<" + joinParams(fd->getTemplateParams()) +
                                        "> " + (exported ? "export " : "") + text);
                    } else {
                        decls.push_back((exported ? "export " : "") + text);
                    }
                    break;
                }
                case ASTNode::INTERFACE_DECL_STMT: {
                    // The parser records an interface's span at its name, so
                    // the `interface` keyword is re-added here.
                    std::string name = declName(inner);
                    bool exported = symbolIsExported(moduleScope, name);
                    std::string text = "interface " + extractDeclSource(
                        sourceText, inner->getLine(), inner->getColumn(), false);
                    decls.push_back((exported ? "export " : "") + text);
                    break;
                }
                case ASTNode::STRUCT_DECL_STMT:
                case ASTNode::CLASS_DECL_STMT:
                case ASTNode::ENUM_DECL_STMT:
                case ASTNode::TYPE_ALIAS_STMT:
                case ASTNode::VAR_DECL_STMT: {
                    std::string name = declName(inner);
                    if (name.empty()) break;
                    if (name.find('<') != std::string::npos) break;  // instantiation clone
                    bool isTemplate = false;
                    if (auto* sd = dynamic_cast<StructDeclStmt*>(inner)) {
                        isTemplate = sd->hasTemplateParams();
                    }
                    bool exported = isTemplate
                                        ? templateIsExported(templates, moduleName, name,
                                                             templateParamsOf(inner))
                                        : symbolIsExported(moduleScope, name);
                    std::string text = extractDeclSource(sourceText, inner->getLine(),
                                                         inner->getColumn(), false);
                    if (isTemplate) {
                        decls.push_back("template<" + joinParams(templateParamsOf(inner)) +
                                        "> " + (exported ? "export " : "") + text);
                    } else {
                        decls.push_back((exported ? "export " : "") + text);
                    }
                    break;
                }
                default:
                    break;
            }
        }

        std::ostringstream out;
        out << "module " << moduleName << "\n";
        for (const auto& dep : deps) out << "dep " << dep << "\n";
        for (const auto& d : decls) {
            if (d.empty()) continue;
            out << d;
            if (d.back() != '\n') out << "\n";
        }
        result.text = out.str();
        return result;
    }

private:
    static void addDep(std::vector<std::string>& deps, const std::string& name) {
        if (name.empty()) return;
        for (const auto& d : deps) {
            if (d == name) return;
        }
        deps.push_back(name);
    }

    static std::string joinParams(const std::vector<std::string>& params) {
        std::string out;
        for (size_t i = 0; i < params.size(); ++i) {
            if (i) out += ", ";
            out += params[i];
        }
        return out;
    }

    static std::vector<std::string> templateParamsOf(Statement* stmt) {
        if (auto* fd = dynamic_cast<FunctionDeclStmt*>(stmt)) return fd->getTemplateParams();
        if (auto* sd = dynamic_cast<StructDeclStmt*>(stmt)) return sd->getTemplateParams();
        return {};
    }

    static std::string declName(Statement* stmt) {
        if (auto* fd = dynamic_cast<FunctionDeclStmt*>(stmt)) return fd->getName();
        if (auto* sd = dynamic_cast<StructDeclStmt*>(stmt)) return sd->getName();
        if (auto* cd = dynamic_cast<ClassDeclStmt*>(stmt)) return cd->getName();
        if (auto* ed = dynamic_cast<EnumDeclStmt*>(stmt)) return ed->getName();
        if (auto* id = dynamic_cast<InterfaceDeclStmt*>(stmt)) return id->getName();
        if (auto* td = dynamic_cast<TypeAliasStmt*>(stmt)) return td->getName();
        if (auto* vd = dynamic_cast<VarDeclStmt*>(stmt)) return vd->getName();
        return "";
    }

    static bool symbolExists(Scope* scope, const std::string& name) {
        return scope && scope->resolveLocal(name) != nullptr;
    }

    static bool symbolIsExported(Scope* scope, const std::string& name) {
        if (!scope) return false;
        auto sym = scope->resolveLocal(name);
        return sym && (sym->isExported || sym->isExtern);
    }

    static bool templateIsExported(
        const std::map<std::string, std::shared_ptr<TemplateSymbol>>& templates,
        const std::string& moduleName, const std::string& name,
        const std::vector<std::string>& params) {
        std::vector<std::string> keys;
        auto specialized = [&]() {
            std::string key = name + "<";
            for (size_t i = 0; i < params.size(); ++i) {
                if (i) key += ",";
                key += params[i];
            }
            key += ">";
            return key;
        };
        if (!params.empty()) keys.push_back(specialized());
        keys.push_back(name);
        for (const auto& key : keys) {
            if (!moduleName.empty()) {
                auto it = templates.find(moduleName + "::" + key);
                if (it != templates.end()) return it->second && it->second->isExported;
            }
            auto it = templates.find(key);
            if (it != templates.end()) return it->second && it->second->isExported;
        }
        return false;
    }
};

}  // namespace cider
