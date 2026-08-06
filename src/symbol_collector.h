#pragma once

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "ast.h"
#include "errors.h"
#include "symbols.h"

namespace cider {

class SymbolCollector : public ASTVisitor {
public:
    SymbolCollector(SymbolTable& symbolTable, ErrorReporter& reporter)
        : symbolTable_(symbolTable), reporter_(reporter), currentProgram_(nullptr) {}

    void collect(AstProgram& program) {
        currentProgram_ = &program;
        program.accept(*this);
    }

    const std::map<std::string, std::shared_ptr<TemplateSymbol>>& getTemplateTable() const {
        return templateTable_;
    }

    // Visitor methods
    void visit(AstProgram& program) override {
        for (auto& stmt : program.getStatements()) {
            stmt->accept(*this);
        }
        // Post-processing: check which structs/classes implement which interfaces
        for (auto& ifaceName : interfaceNames_) {
            auto ifaceSym = symbolTable_.resolve(ifaceName);
            if (!ifaceSym || ifaceSym->kind != SymbolKind::INTERFACE) continue;
            auto iface = std::static_pointer_cast<InterfaceSymbol>(ifaceSym);
            for (auto& structName : structClassNames_) {
                auto structSym = symbolTable_.resolve(structName);
                if (!structSym) continue;
                if (structSym->kind != SymbolKind::STRUCT && structSym->kind != SymbolKind::CLASS) continue;
                auto ss = std::static_pointer_cast<StructSymbol>(structSym);
                bool satisfies = true;
                for (auto& im : iface->methods) {
                    bool found = false;
                    for (auto& sm : ss->methods) {
                        if (sm.name != im.name) continue;
                        if (sm.parameterTypes.size() != im.parameterTypes.size()) continue;
                        bool paramsMatch = true;
                        for (size_t pi = 0; pi < sm.parameterTypes.size(); ++pi) {
                            if (!(sm.parameterTypes[pi] == im.parameterTypes[pi])) {
                                paramsMatch = false;
                                break;
                            }
                        }
                        if (!paramsMatch) continue;
                        if (!(sm.returnType == im.returnType)) continue;
                        found = true;
                        break;
                    }
                    if (!found) {
                        satisfies = false;
                        break;
                    }
                }
                if (satisfies && !iface->methods.empty()) {
                    interfaceImpls_[ifaceName].push_back(structName);
                }
            }
        }
    }

    const std::map<std::string, std::vector<std::string>>& getInterfaceImpls() const {
        return interfaceImpls_;
    }

    void visit(VarDeclStmt& stmt) override {
        auto typeInfo = TypeInfo::fromAST(stmt.getTypeAST());
        auto sym = std::make_shared<Symbol>();
        sym->kind = SymbolKind::VARIABLE;
        sym->name = stmt.getName();
        sym->type = typeInfo;
        sym->line = stmt.getLine();
        sym->col = stmt.getColumn();
        sym->isConst = stmt.isConst();

        if (symbolTable_.getCurrentScope()->resolveLocal(sym->name)) {
            reporter_.error("Redeclaration of variable '" + sym->name + "'", sym->line, sym->col);
        } else {
            symbolTable_.define(sym);
        }
    }

    void visit(FunctionDeclStmt& stmt) override {
        // Check for template function declaration
        if (stmt.hasTemplateParams()) {
            auto tmplSym = std::make_shared<TemplateSymbol>();
            tmplSym->kind = SymbolKind::TEMPLATE;
            tmplSym->name = stmt.getName();
            tmplSym->typeParams = stmt.getTemplateParams();
            templateTable_[stmt.getName()] = tmplSym;
            return;
        }

        std::string prevClass = currentClassName_;

        auto sym = std::make_shared<FunctionSymbol>();
        sym->kind = SymbolKind::FUNCTION;
        sym->name = stmt.getName();
        if (!currentClassName_.empty()) {
            sym->name = currentClassName_ + "_" + sym->name;
        }
        sym->type = {sym->name, 0};
        sym->line = stmt.getLine();
        sym->col = stmt.getColumn();
        sym->returnType = TypeInfo::fromAST(stmt.getReturnType());
        sym->isExtern = stmt.isExtern();
        sym->isExported = inExport_;

        for (auto& param : stmt.getParams()) {
            TypeInfo paramType = TypeInfo::fromAST(param.second.get());
            if (!paramType.isInterface && paramType.pointerLevel == 0) {
                auto typeSym = symbolTable_.resolve(paramType.name);
                if (typeSym && typeSym->kind == SymbolKind::INTERFACE) {
                    paramType.isInterface = true;
                }
            }
            sym->parameterTypes.push_back(paramType);
        }

        if (symbolTable_.getCurrentScope()->resolveLocal(sym->name)) {
            // Ignore if it's already defined
        } else {
            symbolTable_.define(sym);
        }

        symbolTable_.enterScope();

        // Define 'this' for methods
        if (!currentClassName_.empty()) {
            auto thisSym = std::make_shared<Symbol>();
            thisSym->kind = SymbolKind::VARIABLE;
            thisSym->name = "this";
            thisSym->type.name = currentClassName_;
            thisSym->type.pointerLevel = 1;
            thisSym->type.isUserDefined = true;
            symbolTable_.define(thisSym);
        }

        for (auto& param : stmt.getParams()) {
            auto paramSym = std::make_shared<Symbol>();
            paramSym->kind = SymbolKind::VARIABLE;
            paramSym->name = param.first;
            paramSym->type = TypeInfo::fromAST(param.second.get());
            paramSym->line = stmt.getLine();
            paramSym->col = stmt.getColumn();
            symbolTable_.define(paramSym);
        }

        if (!stmt.isExtern()) {
            for (auto& s : stmt.getBody()) {
                s->accept(*this);
            }
        }
        symbolTable_.exitScope();
        currentClassName_ = prevClass;
    }

    void visit(StructDeclStmt& stmt) override {
        // Check for template struct declaration
        if (stmt.hasTemplateParams()) {
            auto tmplSym = std::make_shared<TemplateSymbol>();
            tmplSym->kind = SymbolKind::TEMPLATE;
            tmplSym->name = stmt.getName();
            tmplSym->typeParams = stmt.getTemplateParams();
            templateTable_[stmt.getName()] = tmplSym;
            return;
        }

        auto sym = std::make_shared<StructSymbol>();
        sym->kind = SymbolKind::STRUCT;
        sym->name = stmt.getName();
        sym->line = stmt.getLine();
        sym->col = stmt.getColumn();

        if (stmt.isUnion()) {
            int maxSize = 0;
            for (auto& field : stmt.getFields()) {
                StructSymbol::Field f;
                f.name = field.first;
                f.type = TypeInfo::fromAST(field.second.get());
                f.offset = 0;

                int size = getFieldSize(f.type);
                if (size > maxSize) maxSize = size;

                sym->fields.push_back(f);
            }
            sym->totalSize = maxSize;
        } else {
            int currentOffset = 0;
            for (auto& field : stmt.getFields()) {
                StructSymbol::Field f;
                f.name = field.first;
                f.type = TypeInfo::fromAST(field.second.get());
                f.offset = currentOffset;

                int size = getFieldSize(f.type);

                currentOffset += size;
                sym->fields.push_back(f);
            }
            sym->totalSize = currentOffset;
        }

        if (symbolTable_.getCurrentScope()->resolveLocal(sym->name)) {
            reporter_.error("Redeclaration of type '" + sym->name + "'", sym->line, sym->col);
        } else {
            symbolTable_.define(sym);
        }
        structClassNames_.push_back(stmt.getName());
    }

    void visit(InterfaceDeclStmt& stmt) override {
        auto sym = std::make_shared<InterfaceSymbol>();
        sym->kind = SymbolKind::INTERFACE;
        sym->name = stmt.getName();
        sym->type = {stmt.getName(), 0};
        sym->type.isInterface = true;
        sym->line = stmt.getLine();
        sym->col = stmt.getColumn();
        for (auto& m : stmt.getMethods()) {
            StructSymbol::Method method;
            method.name = m.name;
            method.mangledName = m.name;
            method.returnType = TypeInfo::fromAST(m.returnType.get());
            for (auto& p : m.params) {
                method.parameterTypes.push_back(TypeInfo::fromAST(p.second.get()));
            }
            sym->methods.push_back(method);
        }
        if (symbolTable_.getCurrentScope()->resolveLocal(sym->name)) {
            reporter_.error("Redeclaration of type '" + sym->name + "'", sym->line, sym->col);
        } else {
            symbolTable_.define(sym);
        }
        interfaceNames_.push_back(stmt.getName());
    }

    void visit(EnumDeclStmt& stmt) override {
        auto sym = std::make_shared<Symbol>();
        sym->kind = SymbolKind::ENUM;
        sym->name = stmt.getName();
        sym->type = {"int", 0};
        sym->line = stmt.getLine();
        sym->col = stmt.getColumn();

        if (symbolTable_.getCurrentScope()->resolveLocal(sym->name)) {
            reporter_.error("Redeclaration of enum '" + sym->name + "'", sym->line, sym->col);
        } else {
            symbolTable_.define(sym);
        }

        for (size_t i = 0; i < stmt.getVariants().size(); i++) {
            auto variant = std::make_shared<Symbol>();
            variant->kind = SymbolKind::VARIABLE;
            variant->name = stmt.getVariants()[i];
            variant->type = {"int", 0};
            variant->line = stmt.getLine();
            variant->col = stmt.getColumn();
            variant->isConst = true;
            variant->constValue = (double)i;
            if (symbolTable_.getCurrentScope()->resolveLocal(variant->name)) {
                reporter_.error("Redeclaration of variant '" + variant->name + "'", variant->line, variant->col);
            } else {
                symbolTable_.define(variant);
            }
        }
    }

    void visit(TypeAliasStmt& stmt) override {
        auto sym = std::make_shared<Symbol>();
        sym->kind = SymbolKind::STRUCT;  // Treat alias as a type provider
        sym->name = stmt.getName();
        sym->type = TypeInfo::fromAST(stmt.getTargetType());
        sym->line = stmt.getLine();
        sym->col = stmt.getColumn();

        if (symbolTable_.getCurrentScope()->resolveLocal(sym->name)) {
            reporter_.error("Redeclaration of type '" + sym->name + "'", sym->line, sym->col);
        } else {
            symbolTable_.define(sym);
        }
    }

    void visit(BlockStmt& stmt) override {
        symbolTable_.enterScope();
        for (auto& s : stmt.getStatements()) {
            s->accept(*this);
        }
        symbolTable_.exitScope();
    }

    void visit(IfStmt& stmt) override {
        symbolTable_.enterScope();
        for (auto& s : stmt.getThenBranch()) s->accept(*this);
        symbolTable_.exitScope();
        if (!stmt.getElseBranch().empty()) {
            symbolTable_.enterScope();
            for (auto& s : stmt.getElseBranch()) s->accept(*this);
            symbolTable_.exitScope();
        }
    }

    void visit(WhileStmt& stmt) override {
        symbolTable_.enterScope();
        for (auto& s : stmt.getBody()) s->accept(*this);
        symbolTable_.exitScope();
    }

    void visit(ForStmt& stmt) override {
        symbolTable_.enterScope();
        if (stmt.getInitializer()) stmt.getInitializer()->accept(*this);
        for (auto& s : stmt.getBody()) s->accept(*this);
        symbolTable_.exitScope();
    }

    void visit(ForEachStmt& stmt) override {
        symbolTable_.enterScope();
        auto loopVar = std::make_shared<Symbol>();
        loopVar->kind = SymbolKind::VARIABLE;
        loopVar->name = stmt.getVarName();
        loopVar->type = {"var", 0};
        symbolTable_.define(loopVar);
        for (auto& s : stmt.getBody()) s->accept(*this);
        symbolTable_.exitScope();
    }

    void visit(ImportStmt& stmt) override {
        // Module resolution arrives in Phase 4; imports are collected
        // as statements and skipped here for now.
        (void)stmt;
    }

    // Stubs
    void visit(NumberExpr&) override {}
    void visit(StringExpr&) override {}
    void visit(CharExpr&) override {}
    void visit(IdentifierExpr&) override {}
    void visit(BooleanExpr&) override {}
    void visit(NilExpr&) override {}
    void visit(NilPtrExpr&) override {}
    void visit(NewExpr&) override {}
    void visit(ThisExpr&) override {}
    void visit(BinaryExpr&) override {}
    void visit(UnaryExpr&) override {}
    void visit(CallExpr&) override {}
    void visit(MemberAccessExpr&) override {}
    void visit(ListExpr& expr) override {
        for (auto& e : expr.getElements()) e->accept(*this);
    }
    void visit(TableExpr& expr) override {
        for (auto& e : expr.getEntries()) {
            e.first->accept(*this);
            e.second->accept(*this);
        }
    }
    void visit(IndexExpr&) override {}
    void visit(SliceExpr&) override {}
    void visit(TernaryExpr&) override {}
    void visit(GroupingExpr& expr) override { expr.getExpression()->accept(*this); }
    void visit(FunctionExpr&) override {}
    void visit(SuperExpr&) override {}
    void visit(AwaitExpr&) override {}
    void visit(StructLiteralExpr&) override {}
    void visit(SizeofExpr&) override {}
    void visit(CastExpr&) override {}
    void visit(ArrayTypeAST&) override {}
    void visit(TypeAST&) override {}
    void visit(AssignStmt&) override {}
    void visit(DeleteStmt& stmt) override { stmt.getTarget()->accept(*this); }
    void visit(BreakStmt&) override {}
    void visit(ContinueStmt&) override {}
    void visit(ThrowStmt&) override {}
    void visit(TryCatchStmt& stmt) override {
        symbolTable_.enterScope();
        for (auto& s : stmt.getTryBlock()) s->accept(*this);
        symbolTable_.exitScope();

        symbolTable_.enterScope();
        // Define catch variable
        auto catchVarSym = std::make_shared<Symbol>();
        catchVarSym->kind = SymbolKind::VARIABLE;
        catchVarSym->name = stmt.getCatchVar();
        catchVarSym->type = {"var", 0};
        symbolTable_.define(catchVarSym);
        for (auto& s : stmt.getCatchBlock()) s->accept(*this);
        symbolTable_.exitScope();
    }

    void visit(DeferStmt& stmt) override {
        stmt.getDeferredStmt()->accept(*this);
    }

    void visit(AsmStmt&) override {}

    void visit(MatchStmt& stmt) override {
        for (auto& arm : stmt.getArms()) {
            symbolTable_.enterScope();
            arm.body->accept(*this);
            symbolTable_.exitScope();
        }
    }

    void visit(ReturnStmt&) override {}
    void visit(ExpressionStmt&) override {}

    void visit(ClassDeclStmt& stmt) override {
        auto sym = std::make_shared<StructSymbol>();
        sym->kind = SymbolKind::CLASS;
        sym->name = stmt.getName();
        sym->line = stmt.getLine();
        sym->col = stmt.getColumn();
        sym->parentName = stmt.getParentName();

        int currentOffset = 0;
        bool hasVTable = false;

        // Inherit from parent if it exists
        if (!sym->parentName.empty()) {
            auto parent = symbolTable_.resolve(sym->parentName);
            if (parent && parent->kind == SymbolKind::CLASS) {
                auto parentStruct = std::static_pointer_cast<StructSymbol>(parent);
                sym->fields = parentStruct->fields;
                sym->methods = parentStruct->methods;
                sym->vtable = parentStruct->vtable;
                currentOffset = parentStruct->totalSize;
                if (!sym->vtable.empty()) hasVTable = true;
            }
        }

        // First pass: identify if this class introduces new virtual methods
        for (auto& member : stmt.getMembers()) {
            if (auto method = dynamic_cast<FunctionDeclStmt*>(member.get())) {
                if (method->isVirtual() || method->isOverride()) {
                    hasVTable = true;
                }
            }
        }

        // If this is the root of a virtual hierarchy, reserve space for vptr
        if (hasVTable && sym->vtable.empty()) {
            if (currentOffset == 0) {
                currentOffset = 8;  // Size of vptr
            } else {
                for (auto& f : sym->fields) f.offset += 8;
                currentOffset += 8;
            }
        }

        for (auto& member : stmt.getMembers()) {
            if (auto field = dynamic_cast<VarDeclStmt*>(member.get())) {
                StructSymbol::Field f;
                f.name = field->getName();
                f.type = TypeInfo::fromAST(field->getTypeAST());
                f.offset = currentOffset;
                currentOffset += getFieldSize(f.type);
                sym->fields.push_back(f);
            } else if (auto method = dynamic_cast<FunctionDeclStmt*>(member.get())) {
                if (method->isDestructor()) {
                    sym->destructorName = sym->name + "_" + method->getName();
                    auto funcSym = std::make_shared<FunctionSymbol>();
                    funcSym->kind = SymbolKind::FUNCTION;
                    funcSym->name = sym->destructorName;
                    funcSym->returnType = {"void", 0};
                    funcSym->parameterTypes = {{sym->name, 1}};
                    symbolTable_.define(funcSym);
                    continue;
                }
                StructSymbol::Method m;
                m.name = method->getName();
                m.mangledName = sym->name + "_" + m.name;
                m.returnType = TypeInfo::fromAST(method->getReturnType());
                m.isVirtual = method->isVirtual();
                m.isOverride = method->isOverride();

                for (auto& param : method->getParams()) {
                    m.parameterTypes.push_back(TypeInfo::fromAST(param.second.get()));
                }

                if (m.isOverride) {
                    for (size_t i = 0; i < sym->vtable.size(); ++i) {
                        if (sym->methods[i].name == m.name) {
                            sym->vtable[i] = m.mangledName;
                            m.vtableIndex = (int)i;
                            break;
                        }
                    }
                    for (auto& em : sym->methods) {
                        if (em.name == m.name) {
                            em = m;
                            break;
                        }
                    }
                } else if (m.isVirtual) {
                    m.vtableIndex = (int)sym->vtable.size();
                    sym->vtable.push_back(m.mangledName);
                    sym->methods.push_back(m);
                } else {
                    sym->methods.push_back(m);
                }

                auto funcSym = std::make_shared<FunctionSymbol>();
                funcSym->kind = SymbolKind::FUNCTION;
                funcSym->name = m.mangledName;
                funcSym->returnType = m.returnType;
                funcSym->parameterTypes = m.parameterTypes;
                funcSym->parameterTypes.insert(funcSym->parameterTypes.begin(), {sym->name, 1});
                symbolTable_.define(funcSym);
            }
        }
        std::string prevClass = currentClassName_;
        currentClassName_ = stmt.getName();
        for (auto& member : stmt.getMembers()) {
            member->accept(*this);
        }
        currentClassName_ = prevClass;

        if (currentOffset == 0) currentOffset = 8;
        sym->totalSize = currentOffset;
        symbolTable_.define(sym);
        structClassNames_.push_back(stmt.getName());
    }

    void visit(FromImportStmt& stmt) override {
        (void)stmt;
    }

    void visit(ExportStmt& stmt) override {
        inExport_ = true;
        stmt.getStmt()->accept(*this);
        inExport_ = false;
    }

    void visit(ExportExpressionStmt&) override {}

    void addStructClassName(const std::string& name) {
        structClassNames_.push_back(name);
    }

    int getFieldSize(const TypeInfo& type) {
        if (type.pointerLevel > 0) return 8;
        if (type.isInterface) return 16;
        if (type.isArray) {
            TypeInfo elemType = type;
            elemType.isArray = false;
            elemType.arraySize = 0;
            return type.arraySize * getFieldSize(elemType);
        }
        if (type.name == "u8" || type.name == "i8" || type.name == "char" || type.name == "byte" || type.name == "void") return 1;
        if (type.name == "u16" || type.name == "i16") return 2;
        if (type.name == "u32" || type.name == "i32" || type.name == "int" || type.name == "float" || type.name == "bool") return 4;
        if (type.name == "u64" || type.name == "i64") return 8;
        auto sym = symbolTable_.resolve(type.name);
        if (sym && (sym->kind == SymbolKind::STRUCT || sym->kind == SymbolKind::CLASS)) {
            return std::static_pointer_cast<StructSymbol>(sym)->totalSize;
        }
        return 8;
    }

private:
    SymbolTable& symbolTable_;
    ErrorReporter& reporter_;
    std::string currentClassName_;
    AstProgram* currentProgram_;
    std::vector<std::string> interfaceNames_;
    std::vector<std::string> structClassNames_;
    std::map<std::string, std::vector<std::string>> interfaceImpls_;
    std::map<std::string, std::shared_ptr<TemplateSymbol>> templateTable_;
    bool inExport_ = false;
};

}  // namespace cider
