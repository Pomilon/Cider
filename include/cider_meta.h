#ifndef CIDER_META_H
#define CIDER_META_H

#include <string>
#include <fstream>
#include <sstream>
#include "cider_ir.h"
#include "cider_symbols.h"
#include "cider_ast.h"

namespace Cider {

    class MetaWriter {
    public:
        static bool write(const std::string& path, const std::string& moduleName,
                          Program* program) {
            std::ofstream out(path);
            if (!out) return false;
            out << "module " << moduleName << "\n";
            writeExports(out, program->getStatements());
            for (auto& imported : program->getImportedAsts()) {
                writeExports(out, imported->getStatements());
            }
            out.close();
            return true;
        }

    private:
        static void writeExports(std::ofstream& out,
                                 const std::vector<std::unique_ptr<Statement>>& stmts) {
            for (auto& s : stmts) {
                if (auto es = dynamic_cast<ExportStmt*>(s.get())) {
                    if (auto fd = dynamic_cast<FunctionDeclStmt*>(es->getStmt())) {
                        out << "export func " << fd->getName() << "(";
                        auto& params = fd->getParams();
                        for (size_t i = 0; i < params.size(); i++) {
                            if (i > 0) out << ", ";
                            out << TypeInfo::fromAST(params[i].second.get()).toString();
                        }
                        out << ") -> ";
                        if (fd->getReturnType())
                            out << TypeInfo::fromAST(fd->getReturnType()).toString();
                        else
                            out << "void";
                        out << "\n";
                    }
                }
            }
        }
    };

    class MetaReader {
    public:
        static bool read(const std::string& path, SymbolTable& syms) {
            std::ifstream in(path);
            if (!in) return false;
            std::string line;
            while (std::getline(in, line)) {
                if (line.empty() || line[0] == '#') continue;
                if (line.find("module ") == 0) continue;
                if (line.find("export func ") == 0) {
                    std::string sig = line.substr(12);
                    size_t paren = sig.find('(');
                    if (paren == std::string::npos) continue;
                    std::string funcName = sig.substr(0, paren);
                    size_t arrow = sig.find(") -> ");
                    std::string params = sig.substr(paren + 1, arrow - paren - 1);
                    std::string retType = sig.substr(arrow + 5);

                    auto funcSym = std::make_shared<FunctionSymbol>();
                    funcSym->kind = SymbolKind::FUNCTION;
                    funcSym->name = funcName;
                    funcSym->returnType = parseTypeString(retType);
                    if (!params.empty()) {
                        std::stringstream ps(params);
                        std::string param;
                        while (std::getline(ps, param, ',')) {
                            size_t colon = param.find(':');
                            if (colon != std::string::npos) {
                                std::string t = param.substr(colon + 1);
                                while (!t.empty() && t[0] == ' ') t = t.substr(1);
                                funcSym->parameterTypes.push_back(parseTypeString(t));
                            }
                        }
                    }
                    syms.define(funcSym);
                }
            }
            return true;
        }

    private:
        static TypeInfo parseTypeString(const std::string& s) {
            TypeInfo t;
            std::string clean = s;
            while (!clean.empty() && clean[0] == ' ') clean = clean.substr(1);
            while (!clean.empty() && clean.back() == ' ') clean.pop_back();
            int ptrLevel = 0;
            while (!clean.empty() && clean[0] == '*') {
                ptrLevel++;
                clean = clean.substr(1);
            }
            t.name = clean;
            t.pointerLevel = ptrLevel;
            return t;
        }
    };

}

#endif
