#pragma once

// cider bundle: amalgamates a multi-file program into a single monolithic
// .cid file that compiles identically. Self-contained: it only uses the
// public Lexer/Parser/AST/ModuleResolver APIs and changes no existing
// compiler behavior.
//
// Method: parse the entry file, resolve its imports exactly like the
// symbol collector does, recurse depth-first (each file once, cycles are
// an error), blank out the import statements, and concatenate the rest
// with per-module banners. Whole-program compilation is order
// independent (symbols are collected before checking), so emitting
// dependencies first and the entry last is semantics-preserving.

#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "ast.h"
#include "errors.h"
#include "lexer.h"
#include "module_resolver.h"
#include "parser.h"

namespace cider {

struct BundleResult {
    bool ok = false;
    std::string output;  // bundled source, valid when ok
    std::string error;   // human-readable reason, valid when !ok
};

class Bundler {
public:
    BundleResult bundle(const std::string& entryPath) {
        BundleResult result;
        ModuleResolver resolver;
        std::map<std::string, std::string> texts;  // path -> full text
        std::vector<std::string> order;            // post-order paths
        std::set<std::string> done;
        std::set<std::string> active;
        std::string error;
        if (!visit(entryPath, entryPath, resolver, texts, order, done, active, error)) {
            result.error = error;
            return result;
        }
        std::ostringstream out;
        out << "// Bundled by `cider bundle " << entryPath << "` (" << order.size()
            << " modules). Diagnostics refer to this file's layout.\n";
        for (const auto& path : order) {
            out << "\n// ===== bundled from " << path << " =====\n";
            out << stripImports(texts[path], importLines_[path]);
            if (!texts[path].empty() && texts[path].back() != '\n') out << "\n";
        }
        result.ok = true;
        result.output = out.str();
        return result;
    }

private:
    std::map<std::string, std::vector<int>> importLines_;

    static bool readText(const std::string& path, std::string& text, std::string& error) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            error = "cannot open file: " + path;
            return false;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        text = ss.str();
        return true;
    }

    // Blank out the import statements starting at the given 1-based lines.
    // An import only contains a module path and identifiers, so the first
    // ';' at or after the start line always terminates it (even when the
    // statement spans lines). Everything else is copied verbatim.
    static std::string stripImports(const std::string& text, const std::vector<int>& lines) {
        std::vector<std::string> split;
        {
            std::istringstream ss(text);
            std::string line;
            while (std::getline(ss, line)) split.push_back(line);
        }
        for (int start : lines) {
            for (size_t i = (start >= 1 ? (size_t)(start - 1) : 0); i < split.size(); ++i) {
                bool done = split[i].find(';') != std::string::npos;
                split[i].clear();
                if (done) break;
            }
        }
        std::ostringstream out;
        for (const auto& line : split) out << line << "\n";
        return out.str();
    }

    bool visit(const std::string& path, const std::string& entryPath, ModuleResolver& resolver,
               std::map<std::string, std::string>& texts, std::vector<std::string>& order,
               std::set<std::string>& done, std::set<std::string>& active, std::string& error) {
        if (done.count(path)) return true;
        if (active.count(path)) {
            error = "Circular import detected for module '" + path + "'";
            return false;
        }
        std::string text;
        if (!readText(path, text, error)) return false;

        ErrorReporter rep;
        rep.setFile(path);
        Lexer lexer(text, rep);
        Parser parser(lexer, rep);
        auto prog = parser.parseProgram();
        if (rep.hasErrors() || !prog) {
            error = path + ": parse error: " +
                    (rep.hasErrors() ? rep.errors().front().message : "unknown");
            return false;
        }

        // Collect direct imports before recursing so diagnostics match the
        // compiler's order (unresolved modules reported at the import site).
        struct Dep {
            std::string name;
            int line;
        };
        std::vector<Dep> deps;
        std::vector<int> lines;
        for (const auto& stmt : prog->getStatements()) {
            if (stmt->getType() == ASTNode::IMPORT_STMT) {
                auto* imp = static_cast<ImportStmt*>(stmt.get());
                deps.push_back({imp->getModuleName(), imp->getLine()});
                lines.push_back(imp->getLine());
            } else if (stmt->getType() == ASTNode::FROM_IMPORT_STMT) {
                auto* imp = static_cast<FromImportStmt*>(stmt.get());
                deps.push_back({imp->getModuleName(), imp->getLine()});
                lines.push_back(imp->getLine());
            }
        }

        active.insert(path);
        for (const auto& dep : deps) {
            // Same origin as the collector: every import resolves against
            // the entry file's directory first, not the importing file's.
            auto resolved = resolver.resolve(dep.name, entryPath);
            std::string depPath;
            if (resolved.type == ModuleType::CIDER_SOURCE_FILE) {
                depPath = resolved.path;
            } else if (resolved.type == ModuleType::CIDER_PACKAGE_DIR) {
                depPath = resolved.path + "/__init__.cid";
            } else {
                error = path + ":" + std::to_string(dep.line) +
                        ": Could not resolve module '" + dep.name + "'";
                active.erase(path);
                return false;
            }
            if (!visit(depPath, entryPath, resolver, texts, order, done, active, error)) {
                active.erase(path);
                return false;
            }
        }
        active.erase(path);

        texts[path] = text;
        importLines_[path] = lines;
        done.insert(path);
        order.push_back(path);
        return true;
    }
};

}  // namespace cider
