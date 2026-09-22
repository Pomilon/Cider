#pragma once

#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "ast.h"
#include "errors.h"
#include "lexer.h"
#include "module_resolver.h"
#include "module_scope.h"
#include "parser.h"
#include "symbols.h"

namespace cider {

class SymbolCollector : public ASTVisitor {
public:
    SymbolCollector(SymbolTable& symbolTable, ErrorReporter& reporter,
                    ModuleResolver& resolver, const std::string& originPath = "")
        : symbolTable_(symbolTable), resolver_(resolver), reporter_(reporter),
          currentProgram_(nullptr), originPath_(originPath) {}

    void collect(AstProgram& program) {
        currentProgram_ = &program;
        program.accept(*this);
        // Post-processing: scan for function template call instantiations.
        // Index-based loop avoids iterator invalidation from addStatement.
        for (size_t si = 0; si < program.getStatements().size(); si++) {
            visitStmtForTemplateCalls(program.getStatements()[si].get());
        }
        for (auto& imported : program.getImportedAsts()) {
            for (size_t si = 0; si < imported->getStatements().size(); si++) {
                visitStmtForTemplateCalls(imported->getStatements()[si].get());
            }
        }
    }

    const std::map<std::string, std::shared_ptr<TemplateSymbol>>& getTemplateTable() const {
        return templateTable_;
    }

    // --- module-namespace context (CID-020/021) ---
    // Logical name of the module currently being collected ("" == main file).
    std::string getCurrentModule() const {
        return moduleStack_.empty() ? std::string() : moduleStack_.back();
    }
    // Module scopes keyed by RESOLVED FILE PATH (spec §1).
    const std::map<std::string, Scope*>& getModuleScopes() const { return moduleScopes_; }
    // Qualifier -> competing MODULE candidates for the checker's
    // ambiguous-module error (spec §1). The scope keeps the first binding.
    const std::map<std::string, std::vector<std::shared_ptr<ModuleSymbol>>>& getAmbiguousModules() const {
        return ambiguousModules_;
    }
    // Imported AST root -> logical module name, for checker/ir_gen re-entry.
    const std::map<const AstProgram*, std::string>& getProgramModules() const { return programModules_; }

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
        // Check for struct template instantiation (e.g., Vec<int>).
        if (!typeInfo.isArray && typeInfo.pointerLevel == 0) {
            checkAndInstantiateStructTemplate(typeInfo.name);
        }
        auto sym = std::make_shared<Symbol>();
        sym->kind = SymbolKind::VARIABLE;
        sym->name = stmt.getName();
        sym->type = typeInfo;
        sym->line = stmt.getLine();
        sym->col = stmt.getColumn();
        sym->isConst = stmt.isConst();
        sym->isExtern = stmt.isExtern();

        if (symbolTable_.getCurrentScope()->resolveLocal(sym->name)) {
            reporter_.error("Redeclaration of variable '" + sym->name + "'", sym->line, sym->col);
        } else {
            symbolTable_.define(sym);
        }
    }

    static bool isKnownTypeName(const std::string& n) {
        static const std::set<std::string> builtins = {
            "int", "bool", "char", "byte", "float", "string", "void",
            "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64"};
        if (builtins.count(n)) return true;
        return n.find('<') != std::string::npos;  // Vec<int> is a concrete type
    }

    void visit(FunctionDeclStmt& stmt) override {
        // Check for template function declaration. If every parameter is a
        // concrete type (int, user struct, Vec<int>, ...), this is an
        // explicit specialization, keyed by its mangled name.
        if (stmt.hasTemplateParams()) {
            auto tmplSym = std::make_shared<TemplateSymbol>();
            tmplSym->kind = SymbolKind::TEMPLATE;
            tmplSym->name = stmt.getName();
            tmplSym->typeParams = stmt.getTemplateParams();
            bool specialization = true;
            for (const auto& p : stmt.getTemplateParams()) {
                if (!isKnownTypeName(p)) { specialization = false; break; }
            }
            if (specialization) {
                std::string key = stmt.getName() + "<";
                for (size_t i = 0; i < stmt.getTemplateParams().size(); ++i) {
                    if (i > 0) key += ",";
                    key += stmt.getTemplateParams()[i];
                }
                key += ">";
                templateTable_[key] = tmplSym;
            } else {
                templateTable_[stmt.getName()] = tmplSym;
            }
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
        sym->isAsync = stmt.isAsync();
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
        // Check for template struct declaration (explicit specializations
        // are keyed by their mangled name, like function templates).
        if (stmt.hasTemplateParams()) {
            auto tmplSym = std::make_shared<TemplateSymbol>();
            tmplSym->kind = SymbolKind::TEMPLATE;
            tmplSym->name = stmt.getName();
            tmplSym->typeParams = stmt.getTemplateParams();
            bool specialization = true;
            for (const auto& p : stmt.getTemplateParams()) {
                if (!isKnownTypeName(p)) { specialization = false; break; }
            }
            if (specialization) {
                std::string key = stmt.getName() + "<";
                for (size_t i = 0; i < stmt.getTemplateParams().size(); ++i) {
                    if (i > 0) key += ",";
                    key += stmt.getTemplateParams()[i];
                }
                key += ">";
                templateTable_[key] = tmplSym;
            } else {
                templateTable_[stmt.getName()] = tmplSym;
            }
            return;
        }

        auto sym = std::make_shared<StructSymbol>();
        sym->kind = SymbolKind::STRUCT;
        sym->name = stmt.getName();
        sym->line = stmt.getLine();
        sym->col = stmt.getColumn();

        if (stmt.isUnion()) {
            int maxSize = 0;
            int maxAlign = 1;
            for (auto& field : stmt.getFields()) {
                StructSymbol::Field f;
                f.name = field.name;
                f.type = TypeInfo::fromAST(field.type.get());
                f.offset = 0;
                f.bits = field.bits;

                int size = getFieldSize(f.type);
                if (size > maxSize) maxSize = size;
                int a = (field.bits > 0) ? 4 : getFieldAlignment(f.type);
                if (a > maxAlign) maxAlign = a;

                sym->fields.push_back(f);
            }
            sym->totalSize = alignUp(maxSize, maxAlign);
        } else {
            int currentOffset = 0;
            int maxAlign = 1;
            int bitPos = 0;  // bit position within the current word
            for (auto& field : stmt.getFields()) {
                StructSymbol::Field f;
                f.name = field.name;
                f.type = TypeInfo::fromAST(field.type.get());
                f.bits = field.bits;

                if (field.bits > 0) {
                    // Bitfield: align the containing word to 4, then pack
                    // into the current 32-bit word if it fits.
                    if (bitPos == 0) {
                        currentOffset = alignUp(currentOffset, 4);
                        if (4 > maxAlign) maxAlign = 4;
                    }
                    if (bitPos + field.bits > 32) {
                        currentOffset += 4;
                        bitPos = 0;
                    }
                    f.offset = currentOffset;
                    f.bitOffset = bitPos;
                    f.bitWidth = field.bits;
                    bitPos += field.bits;
                } else {
                    // Regular field: close any partially used bitfield word,
                    // then align to its natural alignment.
                    if (bitPos > 0) {
                        currentOffset += 4;
                        bitPos = 0;
                    }
                    int a = getFieldAlignment(f.type);
                    currentOffset = alignUp(currentOffset, a);
                    if (a > maxAlign) maxAlign = a;
                    f.offset = currentOffset;
                    currentOffset += getFieldSize(f.type);
                }
                sym->fields.push_back(f);
            }
            if (bitPos > 0) currentOffset += 4;
            sym->totalSize = alignUp(currentOffset, maxAlign);
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
        importModule(stmt.getModuleName(), stmt.getLine(), stmt.getColumn());
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
        int classMaxAlign = 1;
        for (auto& f : sym->fields) {
            int a = getFieldAlignment(f.type);
            if (a > classMaxAlign) classMaxAlign = a;
        }
        if (hasVTable && sym->vtable.empty()) {
            if (currentOffset == 0) {
                currentOffset = 8;  // Size of vptr
            } else {
                for (auto& f : sym->fields) f.offset += 8;
                currentOffset += 8;
            }
            if (8 > classMaxAlign) classMaxAlign = 8;
        }

        for (auto& member : stmt.getMembers()) {
            if (auto field = dynamic_cast<VarDeclStmt*>(member.get())) {
                StructSymbol::Field f;
                f.name = field->getName();
                f.type = TypeInfo::fromAST(field->getTypeAST());
                int a = getFieldAlignment(f.type);
                currentOffset = alignUp(currentOffset, a);
                if (a > classMaxAlign) classMaxAlign = a;
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

        if (currentOffset == 0) {
            currentOffset = 8;
            if (8 > classMaxAlign) classMaxAlign = 8;
        }
        sym->totalSize = alignUp(currentOffset, classMaxAlign);
        symbolTable_.define(sym);
        structClassNames_.push_back(stmt.getName());
    }

    void visit(FromImportStmt& stmt) override {
        Scope* modScope = importModule(stmt.getModuleName(), stmt.getLine(), stmt.getColumn());
        if (modScope) bindFromAliases(stmt, modScope);
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

    int getFieldAlignment(const TypeInfo& type, std::set<std::string>* visiting = nullptr) {
        if (type.pointerLevel > 0) return 8;
        if (type.isInterface) return 8;
        if (type.isArray) {
            TypeInfo elemType = type;
            elemType.isArray = false;
            elemType.arraySize = 0;
            return getFieldAlignment(elemType, visiting);
        }
        if (type.name == "u8" || type.name == "i8" || type.name == "char" || type.name == "byte" || type.name == "void") return 1;
        if (type.name == "u16" || type.name == "i16") return 2;
        if (type.name == "u32" || type.name == "i32" || type.name == "int" || type.name == "float" || type.name == "bool") return 4;
        if (type.name == "u64" || type.name == "i64") return 8;
        bool ownSet = false;
        if (!visiting) {
            visiting = new std::set<std::string>();
            ownSet = true;
        }
        int result = 8;
        if (visiting->count(type.name)) {
            result = 8;
        } else {
            visiting->insert(type.name);
            auto sym = symbolTable_.resolve(type.name);
            if (sym && (sym->kind == SymbolKind::STRUCT || sym->kind == SymbolKind::CLASS)) {
                auto ss = std::static_pointer_cast<StructSymbol>(sym);
                int maxA = 1;
                for (auto& f : ss->fields) {
                    int a = getFieldAlignment(f.type, visiting);
                    if (a > maxA) maxA = a;
                }
                result = maxA;
            } else {
                result = 8;
            }
            visiting->erase(type.name);
        }
        if (ownSet) delete visiting;
        return result;
    }

    static int alignUp(int offset, int align) {
        if (align <= 1) return offset;
        return (offset + align - 1) & ~(align - 1);
    }

    // Define the MODULE symbol for an imported module in the importing scope
    // under both the full logical name and its last-segment qualifier (spec
    // §1). A qualifier already bound to a DIFFERENT module is left in place
    // and both candidates are recorded for the checker's ambiguous-module
    // error — never silently overwritten, never an error here. A qualifier
    // shadowed by a value keeps the value (spec §3: values shadow modules).
    void registerModuleSymbol(Scope* importingScope, const std::string& logicalName,
                              Scope* modScope, int line, int col) {
        auto makeModule = [&](const std::string& name) {
            auto sym = std::make_shared<ModuleSymbol>();
            sym->kind = SymbolKind::MODULE;
            sym->name = name;
            sym->logicalName = logicalName;
            sym->scope = modScope;
            sym->line = line;
            sym->col = col;
            return sym;
        };
        // Full logical names contain dots, so they cannot collide with user
        // symbols; only a same-name re-import can find one here.
        if (!importingScope->resolveLocal(logicalName)) {
            importingScope->define(makeModule(logicalName));
        }
        std::string qualifier = qualifierForModule(logicalName);
        auto existing = importingScope->resolveLocal(qualifier);
        if (!existing) {
            importingScope->define(makeModule(qualifier));
            return;
        }
        if (existing->kind != SymbolKind::MODULE) return;  // value shadows qualifier
        auto existingMod = std::static_pointer_cast<ModuleSymbol>(existing);
        if (existingMod->scope == modScope) return;  // same module re-imported
        auto& candidates = ambiguousModules_[qualifier];
        auto hasScope = [&](Scope* s) {
            for (auto& c : candidates)
                if (c->scope == s) return true;
            return false;
        };
        if (!hasScope(existingMod->scope)) candidates.push_back(existingMod);
        if (!hasScope(modScope)) candidates.push_back(makeModule(qualifier));
    }

    // Bind `from m import a, b` aliases in the importing scope. Each alias
    // points at the SAME symbol object in the module's scope (spec §4), so
    // later phases resolve them exactly like the pre-namespace flattening.
    void bindFromAliases(FromImportStmt& stmt, Scope* modScope) {
        Scope* importingScope = symbolTable_.getCurrentScope();
        for (const auto& name : stmt.getSymbols()) {
            auto target = modScope->resolve(name);
            if (!target) {
                // Function/struct templates live in the template table, not
                // in any scope; as with the legacy flattening, importing one
                // is a no-op and use sites instantiate on demand.
                if (templateTable_.count(name)) continue;
                reporter_.error("module '" + stmt.getModuleName() + "' has no symbol '" + name + "'",
                                stmt.getLine(), stmt.getColumn());
                continue;
            }
            if (importingScope->resolveLocal(name)) {
                reporter_.error("Redeclaration of '" + name + "' (from module '" +
                                stmt.getModuleName() + "')", stmt.getLine(), stmt.getColumn());
                continue;
            }
            importingScope->define(target);
        }
    }

    // Collect an imported module's symbols in its own child scope (spec §1).
    // The scope registry is keyed by RESOLVED FILE PATH: two logical names
    // resolving to one file share one scope and are defined once; diamonds
    // reuse it. Returns the module scope, or nullptr when resolution or
    // parsing failed. MODULE symbol registration in the importing scope
    // happens for every import, including scope reuses.
    Scope* importModule(const std::string& moduleName, int line, int col) {
        auto result = resolver_.resolve(moduleName, originPath_);
        if (result.type == ModuleType::NOT_FOUND) {
            reporter_.error("Could not resolve module '" + moduleName + "'", line, col);
            return nullptr;
        }

        // Cycle detection
        if (importingPaths_.count(result.path)) {
            reporter_.error("Circular import detected for module '" + moduleName + "'", line, col);
            return nullptr;
        }

        // The scope that issued this import: root for main-file imports, the
        // importer's own module scope for nested ones.
        Scope* importingScope = symbolTable_.getCurrentScope();

        auto scopeIt = moduleScopes_.find(result.path);
        if (scopeIt != moduleScopes_.end()) {
            // Diamond / repeat import: share the existing scope (defined
            // once), but still register this logical name for the importer.
            importedModuleNames_.insert(moduleName);
            registerModuleSymbol(importingScope, moduleName, scopeIt->second, line, col);
            return scopeIt->second;
        }

        importedModuleNames_.insert(moduleName);
        importingPaths_.insert(result.path);

        std::ifstream file(result.path);
        if (!file) {
            importingPaths_.erase(result.path);
            return nullptr;
        }
        std::stringstream buffer;
        buffer << file.rdbuf();

        // Attribute nested diagnostics to the imported file (Issue #16):
        // the shared reporter otherwise stamps them with the main file.
        // Covers both the nested parse and the symbol collection below.
        std::string prevFile = reporter_.getFile();
        reporter_.setFile(result.path);
        Lexer lexer(buffer.str(), reporter_);
        Parser parser(lexer, reporter_);
        auto importedAst = parser.parseProgram();

        Scope* modScope = nullptr;
        if (importedAst) {
            importedAst->setSourcePath(result.path);
            symbolTable_.enterModuleScope();
            modScope = symbolTable_.getCurrentScope();
            moduleScopes_[result.path] = modScope;
            moduleStack_.push_back(moduleName);
            importedAst->accept(*this);
            moduleStack_.pop_back();
            symbolTable_.exitScope();
            if (currentProgram_) {
                programModules_[importedAst.get()] = moduleName;
                currentProgram_->addImportedAst(std::move(importedAst));
            }
        }
        reporter_.setFile(prevFile);

        importingPaths_.erase(result.path);
        if (!modScope) return nullptr;  // parse failed; errors already reported

        registerModuleSymbol(importingScope, moduleName, modScope, line, col);
        return modScope;
    }

    // --- template instantiation helpers ---

    static std::pair<std::string, std::vector<std::string>> parseMangledTemplateName(const std::string& mangled) {
        size_t pos = mangled.find('<');
        if (pos == std::string::npos) return {mangled, {}};
        std::string baseName = mangled.substr(0, pos);
        std::string argsStr = mangled.substr(pos + 1, mangled.size() - pos - 2);
        std::vector<std::string> args;
        size_t start = 0;
        while (start < argsStr.size()) {
            size_t comma = argsStr.find(',', start);
            if (comma == std::string::npos) {
                args.push_back(argsStr.substr(start));
                break;
            }
            args.push_back(argsStr.substr(start, comma - start));
            start = comma + 1;
        }
        return {baseName, args};
    }

    static std::string substituteTypeName(const std::string& name,
                                          const std::vector<std::string>& tmplParams,
                                          const std::vector<std::string>& tmplArgs) {
        for (size_t i = 0; i < tmplParams.size(); i++) {
            if (name == tmplParams[i]) return tmplArgs[i];
        }
        // Recursively substitute within template type arguments like Result<T,E>.
        size_t pos = name.find('<');
        if (pos != std::string::npos) {
            std::string baseName = name.substr(0, pos);
            std::string argsStr = name.substr(pos + 1, name.size() - pos - 2);
            std::vector<std::string> args;
            size_t start = 0;
            while (start < argsStr.size()) {
                size_t comma = argsStr.find(',', start);
                if (comma == std::string::npos) {
                    args.push_back(argsStr.substr(start));
                    break;
                }
                args.push_back(argsStr.substr(start, comma - start));
                start = comma + 1;
            }
            std::string result = baseName + "<";
            for (size_t i = 0; i < args.size(); i++) {
                if (i > 0) result += ",";
                result += substituteTypeName(args[i], tmplParams, tmplArgs);
            }
            result += ">";
            return result;
        }
        return name;
    }

    void checkAndInstantiateStructTemplate(const std::string& typeName) {
        size_t pos = typeName.find('<');
        if (pos == std::string::npos) return;
        std::string baseName = typeName.substr(0, pos);
        auto it = templateTable_.find(baseName);
        if (it == templateTable_.end()) return;
        auto tmplSym = it->second;
        if (tmplSym->kind != SymbolKind::TEMPLATE) return;

        StructDeclStmt* tmplStruct = nullptr;
        auto findStruct = [&](const std::vector<std::unique_ptr<Statement>>& stmts) -> StructDeclStmt* {
            for (auto& s : stmts) {
                Statement* inner = s.get();
                if (auto es = dynamic_cast<ExportStmt*>(inner)) inner = es->getStmt();
                if (auto sd = dynamic_cast<StructDeclStmt*>(inner)) {
                    if (sd->getName() == baseName) return sd;
                }
            }
            return nullptr;
        };
        tmplStruct = findStruct(currentProgram_->getStatements());
        if (!tmplStruct) {
            for (auto& imported : currentProgram_->getImportedAsts()) {
                tmplStruct = findStruct(imported->getStatements());
                if (tmplStruct) break;
            }
        }
        if (!tmplStruct) return;

        auto parsed = parseMangledTemplateName(typeName);
        if (parsed.second.size() != tmplSym->typeParams.size()) {
            reporter_.error("Template '" + baseName + "' expects " +
                std::to_string(tmplSym->typeParams.size()) + " type arguments, got " +
                std::to_string(parsed.second.size()), 0, 0);
            return;
        }

        auto structSym = std::make_shared<StructSymbol>();
        structSym->kind = SymbolKind::STRUCT;
        structSym->name = typeName;
        structSym->line = 0;
        structSym->col = 0;

        int currentOffset = 0;
        int maxAlign = 1;
        int bitPos = 0;
        for (auto& field : tmplStruct->getFields()) {
            StructSymbol::Field f;
            f.name = field.name;
            f.type = TypeInfo::fromAST(field.type.get());
            f.type.name = substituteTypeName(f.type.name, tmplSym->typeParams, parsed.second);
            f.bits = field.bits;
            if (field.bits > 0) {
                if (bitPos == 0) {
                    currentOffset = alignUp(currentOffset, 4);
                    if (4 > maxAlign) maxAlign = 4;
                }
                if (bitPos + field.bits > 32) {
                    currentOffset += 4;
                    bitPos = 0;
                }
                f.offset = currentOffset;
                f.bitOffset = bitPos;
                f.bitWidth = field.bits;
                bitPos += field.bits;
            } else {
                if (bitPos > 0) {
                    currentOffset += 4;
                    bitPos = 0;
                }
                int a = getFieldAlignment(f.type);
                currentOffset = alignUp(currentOffset, a);
                if (a > maxAlign) maxAlign = a;
                f.offset = currentOffset;
                currentOffset += getFieldSize(f.type);
            }
            structSym->fields.push_back(f);
        }
        if (bitPos > 0) currentOffset += 4;
        structSym->totalSize = alignUp(currentOffset, maxAlign);

        auto globalScope = symbolTable_.getCurrentScope();
        while (globalScope->getParent()) globalScope = globalScope->getParent();
        if (!globalScope->resolveLocal(typeName)) {
            globalScope->define(structSym);
            structClassNames_.push_back(typeName);
        }
    }

    void visitStmtForTemplateCalls(Statement* stmt) {
        if (!stmt) return;
        switch (stmt->getType()) {
            case ASTNode::FUNCTION_DECL_STMT:
                if (auto fd = dynamic_cast<FunctionDeclStmt*>(stmt)) {
                    for (auto& s : fd->getBody()) visitStmtForTemplateCalls(s.get());
                }
                break;
            case ASTNode::EXPRESSION_STMT:
                if (auto es = dynamic_cast<ExpressionStmt*>(stmt))
                    visitExprForTemplateCalls(es->getExpression());
                break;
            case ASTNode::RETURN_STMT:
                if (auto rs = dynamic_cast<ReturnStmt*>(stmt)) {
                    if (rs->getValue()) visitExprForTemplateCalls(rs->getValue());
                }
                break;
            case ASTNode::VAR_DECL_STMT:
                if (auto vs = dynamic_cast<VarDeclStmt*>(stmt)) {
                    if (vs->getInitializer()) visitExprForTemplateCalls(vs->getInitializer());
                }
                break;
            case ASTNode::ASSIGN_STMT:
                if (auto as = dynamic_cast<AssignStmt*>(stmt)) {
                    visitExprForTemplateCalls(as->getTarget());
                    visitExprForTemplateCalls(as->getValue());
                }
                break;
            case ASTNode::IF_STMT:
                if (auto is = dynamic_cast<IfStmt*>(stmt)) {
                    visitExprForTemplateCalls(is->getCondition());
                    for (auto& s : is->getThenBranch()) visitStmtForTemplateCalls(s.get());
                    for (auto& s : is->getElseBranch()) visitStmtForTemplateCalls(s.get());
                }
                break;
            case ASTNode::WHILE_STMT:
                if (auto ws = dynamic_cast<WhileStmt*>(stmt)) {
                    visitExprForTemplateCalls(ws->getCondition());
                    for (auto& s : ws->getBody()) visitStmtForTemplateCalls(s.get());
                }
                break;
            case ASTNode::BLOCK_STMT:
                if (auto bs = dynamic_cast<BlockStmt*>(stmt)) {
                    for (auto& s : bs->getStatements()) visitStmtForTemplateCalls(s.get());
                }
                break;
            default:
                break;
        }
    }

    // Minimal type inference for template argument deduction: returns the
    // concrete type name of a literal/simple argument expression.
    std::string inferArgType(Expression* arg) {
        if (!arg) return "";
        if (dynamic_cast<NumberExpr*>(arg)) return "int";
        if (dynamic_cast<StringExpr*>(arg) || dynamic_cast<InterpolatedStringExpr*>(arg)) return "string";
        if (dynamic_cast<BooleanExpr*>(arg)) return "bool";
        if (dynamic_cast<CharExpr*>(arg)) return "char";
        if (auto id = dynamic_cast<IdentifierExpr*>(arg)) {
            auto sym = symbolTable_.resolve(id->getName());
            if (sym) return sym->type.name;
        }
        if (auto cast = dynamic_cast<CastExpr*>(arg)) {
            if (cast->getTargetType()) return cast->getTargetType()->getName();
        }
        if (auto un = dynamic_cast<UnaryExpr*>(arg)) {
            return inferArgType(un->getOperand());
        }
        return "";
    }

    void deduceAndInstantiate(Expression* callee, const std::vector<std::unique_ptr<Expression>>& args) {
        auto id = dynamic_cast<IdentifierExpr*>(callee);
        if (!id) return;
        const std::string& nm = id->getName();
        if (nm.find('<') != std::string::npos) return;  // already explicit
        auto it = templateTable_.find(nm);
        if (it == templateTable_.end()) return;
        // Only primary templates (params are generic names) deduce.
        bool primary = true;
        for (const auto& p : it->second->typeParams) {
            if (isKnownTypeName(p)) { primary = false; break; }
        }
        if (!primary) return;
        // Single type parameter: it is unified with the first argument's
        // type (max<T>(a: T, b: T)). Multiple params map in order
        // (pair<T,U> <- args 0,1).
        const auto& typeParams = it->second->typeParams;
        std::string mangled = nm + "<";
        for (size_t i = 0; i < typeParams.size(); ++i) {
            size_t argIdx = (typeParams.size() == 1) ? 0 : i;
            if (argIdx >= args.size()) return;
            std::string t = inferArgType(args[argIdx].get());
            if (t.empty()) return;  // cannot deduce — leave it
            if (i > 0) mangled += ",";
            mangled += t;
        }
        mangled += ">";
        instantiateFunctionTemplate(mangled);
        // Rewrite the call site to the mangled name so the type checker and
        // ir-gen resolve it as a plain function.
        id->setName(mangled);
    }

    void visitExprForTemplateCalls(Expression* expr) {
        if (!expr) return;
        if (auto id = dynamic_cast<IdentifierExpr*>(expr)) {
            const std::string& name = id->getName();
            size_t pos = name.find('<');
            if (pos != std::string::npos) {
                auto parsed = parseMangledTemplateName(name);
                auto it = templateTable_.find(parsed.first);
                if (it != templateTable_.end()) {
                    instantiateFunctionTemplate(name);
                }
            }
        } else if (auto call = dynamic_cast<CallExpr*>(expr)) {
            deduceAndInstantiate(call->getCallee(), call->getArgs());
            visitExprForTemplateCalls(call->getCallee());
            for (auto& arg : call->getArgs()) visitExprForTemplateCalls(arg.get());
        } else if (auto bin = dynamic_cast<BinaryExpr*>(expr)) {
            visitExprForTemplateCalls(bin->getLeft());
            visitExprForTemplateCalls(bin->getRight());
        } else if (auto unary = dynamic_cast<UnaryExpr*>(expr)) {
            visitExprForTemplateCalls(unary->getOperand());
        } else if (auto ternary = dynamic_cast<TernaryExpr*>(expr)) {
            visitExprForTemplateCalls(ternary->getCondition());
            visitExprForTemplateCalls(ternary->getThenExpr());
            visitExprForTemplateCalls(ternary->getElseExpr());
        } else if (auto grouping = dynamic_cast<GroupingExpr*>(expr)) {
            visitExprForTemplateCalls(grouping->getExpression());
        } else if (auto member = dynamic_cast<MemberAccessExpr*>(expr)) {
            visitExprForTemplateCalls(member->getObject());
        } else if (auto index = dynamic_cast<IndexExpr*>(expr)) {
            visitExprForTemplateCalls(index->getObject());
            visitExprForTemplateCalls(index->getIndex());
        }
    }

    std::unique_ptr<TypeAST> cloneTypeWithSubst(TypeAST* type,
                                                const std::vector<std::string>& tmplParams,
                                                const std::vector<std::string>& tmplArgs) {
        if (!type) return nullptr;
        if (auto arr = dynamic_cast<ArrayTypeAST*>(type)) {
            return std::make_unique<ArrayTypeAST>(
                cloneTypeWithSubst(arr->getBaseType(), tmplParams, tmplArgs),
                arr->getSize(), arr->getLine(), arr->getColumn());
        }
        std::string name = substituteTypeName(type->getName(), tmplParams, tmplArgs);
        return std::make_unique<TypeAST>(name, type->getPointerLevel(), type->getLine(), type->getColumn(),
                                         type->isVolatile());
    }

    std::unique_ptr<Statement> cloneStmtWithSubst(Statement* stmt,
                                                  const std::vector<std::string>& tmplParams,
                                                  const std::vector<std::string>& tmplArgs) {
        if (!stmt) return nullptr;
        switch (stmt->getType()) {
            case ASTNode::VAR_DECL_STMT: {
                auto vs = dynamic_cast<VarDeclStmt*>(stmt);
                auto clonedInit = vs->getInitializer()
                    ? cloneExprWithSubst(vs->getInitializer(), tmplParams, tmplArgs) : nullptr;
                return std::make_unique<VarDeclStmt>(vs->getName(), std::move(clonedInit),
                    cloneTypeWithSubst(vs->getTypeAST(), tmplParams, tmplArgs),
                    vs->getLine(), vs->getColumn(), vs->isConst());
            }
            case ASTNode::RETURN_STMT: {
                auto rs = dynamic_cast<ReturnStmt*>(stmt);
                auto clonedVal = rs->getValue()
                    ? cloneExprWithSubst(rs->getValue(), tmplParams, tmplArgs) : nullptr;
                return std::make_unique<ReturnStmt>(std::move(clonedVal), rs->getLine(), rs->getColumn());
            }
            case ASTNode::EXPRESSION_STMT: {
                auto es = dynamic_cast<ExpressionStmt*>(stmt);
                return std::make_unique<ExpressionStmt>(
                    cloneExprWithSubst(es->getExpression(), tmplParams, tmplArgs),
                    es->getLine(), es->getColumn());
            }
            case ASTNode::BLOCK_STMT: {
                auto bs = dynamic_cast<BlockStmt*>(stmt);
                std::vector<std::unique_ptr<Statement>> clonedStmts;
                for (auto& s : bs->getStatements()) {
                    clonedStmts.push_back(cloneStmtWithSubst(s.get(), tmplParams, tmplArgs));
                }
                return std::make_unique<BlockStmt>(std::move(clonedStmts), bs->getLine(), bs->getColumn());
            }
            case ASTNode::ASSIGN_STMT: {
                auto as = dynamic_cast<AssignStmt*>(stmt);
                return std::make_unique<AssignStmt>(
                    cloneExprWithSubst(as->getTarget(), tmplParams, tmplArgs),
                    cloneExprWithSubst(as->getValue(), tmplParams, tmplArgs),
                    as->getLine(), as->getColumn(), as->getOp());
            }
            case ASTNode::IF_STMT: {
                auto is = dynamic_cast<IfStmt*>(stmt);
                std::vector<std::unique_ptr<Statement>> clonedThen;
                for (auto& s : is->getThenBranch()) {
                    clonedThen.push_back(cloneStmtWithSubst(s.get(), tmplParams, tmplArgs));
                }
                std::vector<std::unique_ptr<Statement>> clonedElse;
                for (auto& s : is->getElseBranch()) {
                    clonedElse.push_back(cloneStmtWithSubst(s.get(), tmplParams, tmplArgs));
                }
                return std::make_unique<IfStmt>(
                    cloneExprWithSubst(is->getCondition(), tmplParams, tmplArgs),
                    std::move(clonedThen), std::move(clonedElse),
                    is->getLine(), is->getColumn());
            }
            case ASTNode::WHILE_STMT: {
                auto ws = dynamic_cast<WhileStmt*>(stmt);
                std::vector<std::unique_ptr<Statement>> clonedBody;
                for (auto& s : ws->getBody()) {
                    clonedBody.push_back(cloneStmtWithSubst(s.get(), tmplParams, tmplArgs));
                }
                return std::make_unique<WhileStmt>(
                    cloneExprWithSubst(ws->getCondition(), tmplParams, tmplArgs),
                    std::move(clonedBody), ws->getLine(), ws->getColumn());
            }
            default:
                return nullptr;
        }
    }

    std::unique_ptr<Expression> cloneExprWithSubst(Expression* expr,
                                                   const std::vector<std::string>& tmplParams,
                                                   const std::vector<std::string>& tmplArgs) {
        if (!expr) return nullptr;
        switch (expr->getType()) {
            case ASTNode::IDENTIFIER_EXPR:
                return std::make_unique<IdentifierExpr>(
                    substituteTypeName(dynamic_cast<IdentifierExpr*>(expr)->getName(), tmplParams, tmplArgs),
                    expr->getLine(), expr->getColumn());
            case ASTNode::NUMBER_EXPR: {
                auto num = dynamic_cast<NumberExpr*>(expr);
                if (num->isFloat())
                    return std::make_unique<NumberExpr>(num->getFloatValue(), expr->getLine(), expr->getColumn());
                return std::make_unique<NumberExpr>(num->getUIntValue(), expr->getLine(), expr->getColumn());
            }
            case ASTNode::BOOLEAN_EXPR:
                return std::make_unique<BooleanExpr>(
                    dynamic_cast<BooleanExpr*>(expr)->getValue(), expr->getLine(), expr->getColumn());
            case ASTNode::STRING_EXPR:
                return std::make_unique<StringExpr>(
                    dynamic_cast<StringExpr*>(expr)->getValue(), expr->getLine(), expr->getColumn());
            case ASTNode::CHAR_EXPR:
                return std::make_unique<CharExpr>(
                    dynamic_cast<CharExpr*>(expr)->getValue(), expr->getLine(), expr->getColumn());
            case ASTNode::BINARY_EXPR: {
                auto be = dynamic_cast<BinaryExpr*>(expr);
                return std::make_unique<BinaryExpr>(
                    cloneExprWithSubst(be->getLeft(), tmplParams, tmplArgs),
                    be->getOperator(),
                    cloneExprWithSubst(be->getRight(), tmplParams, tmplArgs),
                    be->getLine(), be->getColumn());
            }
            case ASTNode::UNARY_EXPR: {
                auto ue = dynamic_cast<UnaryExpr*>(expr);
                return std::make_unique<UnaryExpr>(
                    ue->getOperator(),
                    cloneExprWithSubst(ue->getOperand(), tmplParams, tmplArgs),
                    ue->getLine(), ue->getColumn(), ue->isPostfix());
            }
            case ASTNode::TERNARY_EXPR: {
                auto te = dynamic_cast<TernaryExpr*>(expr);
                return std::make_unique<TernaryExpr>(
                    cloneExprWithSubst(te->getCondition(), tmplParams, tmplArgs),
                    cloneExprWithSubst(te->getThenExpr(), tmplParams, tmplArgs),
                    cloneExprWithSubst(te->getElseExpr(), tmplParams, tmplArgs),
                    te->getLine(), te->getColumn());
            }
            case ASTNode::GROUPING_EXPR:
                return std::make_unique<GroupingExpr>(
                    cloneExprWithSubst(dynamic_cast<GroupingExpr*>(expr)->getExpression(), tmplParams, tmplArgs),
                    expr->getLine(), expr->getColumn());
            case ASTNode::CALL_EXPR: {
                auto ce = dynamic_cast<CallExpr*>(expr);
                std::vector<std::unique_ptr<Expression>> clonedArgs;
                for (auto& arg : ce->getArgs()) {
                    clonedArgs.push_back(cloneExprWithSubst(arg.get(), tmplParams, tmplArgs));
                }
                return std::make_unique<CallExpr>(
                    cloneExprWithSubst(ce->getCallee(), tmplParams, tmplArgs),
                    std::move(clonedArgs), ce->getLine(), ce->getColumn());
            }
            case ASTNode::MEMBER_ACCESS_EXPR: {
                auto ma = dynamic_cast<MemberAccessExpr*>(expr);
                return std::make_unique<MemberAccessExpr>(
                    cloneExprWithSubst(ma->getObject(), tmplParams, tmplArgs),
                    ma->getMember(), ma->getLine(), ma->getColumn(), ma->isArrow());
            }
            case ASTNode::NIL_EXPR:
                return std::make_unique<NilExpr>(expr->getLine(), expr->getColumn());
            case ASTNode::NIL_PTR_EXPR:
                return std::make_unique<NilPtrExpr>(expr->getLine(), expr->getColumn());
            case ASTNode::THIS_EXPR:
                return std::make_unique<ThisExpr>(expr->getLine(), expr->getColumn());
            case ASTNode::SIZEOF_EXPR: {
                auto se = dynamic_cast<SizeofExpr*>(expr);
                return std::make_unique<SizeofExpr>(
                    cloneTypeWithSubst(se->getTargetType(), tmplParams, tmplArgs),
                    se->getLine(), se->getColumn());
            }
            case ASTNode::CAST_EXPR: {
                auto ce = dynamic_cast<CastExpr*>(expr);
                return std::make_unique<CastExpr>(
                    cloneExprWithSubst(ce->getExpression(), tmplParams, tmplArgs),
                    cloneTypeWithSubst(ce->getTargetType(), tmplParams, tmplArgs),
                    ce->getLine(), ce->getColumn());
            }
            case ASTNode::INDEX_EXPR: {
                auto ie = dynamic_cast<IndexExpr*>(expr);
                return std::make_unique<IndexExpr>(
                    cloneExprWithSubst(ie->getObject(), tmplParams, tmplArgs),
                    cloneExprWithSubst(ie->getIndex(), tmplParams, tmplArgs),
                    ie->getLine(), ie->getColumn());
            }
            default:
                return nullptr;
        }
    }

    void instantiateFunctionTemplate(const std::string& mangledName) {
        auto parsed = parseMangledTemplateName(mangledName);
        // An explicit specialization (keyed by the mangled name) wins over
        // the primary template.
        auto exact = templateTable_.find(mangledName);
        auto it = (exact != templateTable_.end()) ? exact : templateTable_.find(parsed.first);
        if (it == templateTable_.end()) return;

        // Already instantiated (asm-safe mangled name avoids conflicts).
        if (symbolTable_.resolve(mangledName)) return;

        auto tmplSym = it->second;
        if (parsed.second.size() != tmplSym->typeParams.size()) {
            reporter_.error("Template '" + parsed.first + "' expects " +
                std::to_string(tmplSym->typeParams.size()) + " type arguments, got " +
                std::to_string(parsed.second.size()), 0, 0);
            return;
        }

        FunctionDeclStmt* tmplFunc = nullptr;
        bool isExact = (exact != templateTable_.end());
        auto findTemplateFunc = [&](const std::vector<std::unique_ptr<Statement>>& stmts) -> FunctionDeclStmt* {
            for (auto& s : stmts) {
                Statement* inner = s.get();
                if (auto es = dynamic_cast<ExportStmt*>(inner)) inner = es->getStmt();
                if (auto fd = dynamic_cast<FunctionDeclStmt*>(inner)) {
                    if (fd->getName() != parsed.first || !fd->hasTemplateParams()) continue;
                    if (isExact) {
                        // Explicit specialization: the template parameter
                        // list must match the requested concrete args.
                        if (fd->getTemplateParams() == parsed.second) return fd;
                    } else {
                        return fd;  // the primary template
                    }
                }
            }
            return nullptr;
        };
        tmplFunc = findTemplateFunc(currentProgram_->getStatements());
        AstProgram* definingProg = currentProgram_;
        if (!tmplFunc) {
            for (auto& imported : currentProgram_->getImportedAsts()) {
                tmplFunc = findTemplateFunc(imported->getStatements());
                if (tmplFunc) { definingProg = imported.get(); break; }
            }
        }
        if (!tmplFunc) return;

        // CID-020: a template from an imported module instantiates into that
        // module's scope (its body resolves module-internal names) and the
        // clone is appended to the module's AST. The same symbol object is
        // also defined in the root scope so call sites anywhere keep
        // resolving it exactly like the legacy flat table.
        Scope* rootScope = symbolTable_.getRootScope();
        Scope* definingScope = rootScope;
        if (definingProg && definingProg != currentProgram_ &&
            !definingProg->getSourcePath().empty()) {
            auto scopeIt = moduleScopes_.find(definingProg->getSourcePath());
            if (scopeIt != moduleScopes_.end()) definingScope = scopeIt->second;
        }

        // Build the concrete function symbol.
        auto funcSym = std::make_shared<FunctionSymbol>();
        funcSym->kind = SymbolKind::FUNCTION;
        funcSym->name = mangledName;
        funcSym->type = {mangledName, 0};
        funcSym->line = tmplFunc->getLine();
        funcSym->col = tmplFunc->getColumn();

        auto substType = [&](TypeAST* typeAST) -> TypeInfo {
            TypeInfo info = TypeInfo::fromAST(typeAST);
            info.name = substituteTypeName(info.name, tmplSym->typeParams, parsed.second);
            return info;
        };

        funcSym->returnType = substType(tmplFunc->getReturnType());
        for (auto& param : tmplFunc->getParams()) {
            funcSym->parameterTypes.push_back(substType(param.second.get()));
        }

        Scope* prevScope = symbolTable_.getCurrentScope();
        symbolTable_.setCurrentScope(definingScope);
        symbolTable_.define(funcSym);
        if (definingScope != rootScope && !rootScope->resolveLocal(mangledName)) {
            rootScope->define(funcSym);
        }

        // Clone the full function with type substitution and add to the
        // defining program (main or the template's module).
        std::vector<FunctionDeclStmt::Parameter> clonedParams;
        for (auto& param : tmplFunc->getParams()) {
            clonedParams.push_back({param.first,
                cloneTypeWithSubst(param.second.get(), tmplSym->typeParams, parsed.second)});
        }
        auto clonedReturnType = cloneTypeWithSubst(tmplFunc->getReturnType(), tmplSym->typeParams, parsed.second);
        std::vector<std::unique_ptr<Statement>> clonedBody;
        for (auto& s : tmplFunc->getBody()) {
            auto cloned = cloneStmtWithSubst(s.get(), tmplSym->typeParams, parsed.second);
            if (cloned) clonedBody.push_back(std::move(cloned));
        }
        auto clonedFunc = std::make_unique<FunctionDeclStmt>(
            mangledName, std::move(clonedParams), std::move(clonedBody), std::move(clonedReturnType),
            tmplFunc->getLine(), tmplFunc->getColumn());
        FunctionDeclStmt* rawPtr = clonedFunc.get();

        // Process the cloned function through the collector (defines its
        // params and body symbols) in the defining scope, then scan its body
        // for nested template calls, then append it to the defining program
        // for the IR generator.
        visit(*rawPtr);
        visitStmtForTemplateCalls(rawPtr);
        symbolTable_.setCurrentScope(prevScope);
        definingProg->addStatement(std::move(clonedFunc));
    }

private:
    SymbolTable& symbolTable_;
    ModuleResolver& resolver_;
    ErrorReporter& reporter_;
    std::string currentClassName_;
    AstProgram* currentProgram_;
    std::string originPath_;
    std::vector<std::string> interfaceNames_;
    std::vector<std::string> structClassNames_;
    std::map<std::string, std::vector<std::string>> interfaceImpls_;
    std::map<std::string, std::shared_ptr<TemplateSymbol>> templateTable_;
    bool inExport_ = false;
    std::set<std::string> importedModuleNames_;
    std::set<std::string> importingPaths_;
    std::map<std::string, Scope*> moduleScopes_;  // resolved path -> module scope (spec §1)
    std::map<std::string, std::vector<std::shared_ptr<ModuleSymbol>>> ambiguousModules_;
    std::map<const AstProgram*, std::string> programModules_;  // imported AST -> logical name
    std::vector<std::string> moduleStack_;  // current-module context; empty == main file
};

}  // namespace cider
