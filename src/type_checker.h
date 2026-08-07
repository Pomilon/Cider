#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast.h"
#include "errors.h"
#include "symbols.h"

namespace cider {

class TypeChecker : public ASTVisitor {
public:
    TypeChecker(SymbolTable& symbolTable, ErrorReporter& reporter, bool moduleMode = false)
        : symbolTable_(symbolTable), reporter_(reporter), moduleMode_(moduleMode) {}

    void check(AstProgram& program) {
        program.accept(*this);
    }

    TypeInfo resolveType(TypeInfo info) {
        if (info.pointerLevel > 0) return info;
        auto sym = symbolTable_.resolve(info.name);
        if (sym && sym->kind == SymbolKind::STRUCT && sym->type.name != info.name && !sym->type.name.empty()) {
            TypeInfo resolved = resolveType(sym->type);
            resolved.pointerLevel += info.pointerLevel;  // Should be 0 here
            return resolved;
        }
        return info;
    }

    bool areTypesCompatible(TypeInfo t1, TypeInfo t2) {
        if (t1.name == "wildcard" || t2.name == "wildcard") return true;
        t1 = resolveType(t1);
        t2 = resolveType(t2);

        if (t1 == t2) return true;

        auto isInteger = [](const std::string& name) {
            return name == "int" || name == "u8" || name == "i8" || name == "u16" || name == "i16" ||
                   name == "u32" || name == "i32" || name == "u64" || name == "i64" || name == "char" || name == "byte";
        };

        if (t1.name == "float" && isInteger(t2.name) && t1.pointerLevel == 0 && t2.pointerLevel == 0) return true;
        if (isInteger(t1.name) && isInteger(t2.name) && t1.pointerLevel == 0 && t2.pointerLevel == 0) return true;
        // C-style conversions: any T* <-> void* (both directions, any
        // pointer depth). The reference only allowed T* -> void*, which
        // rejected nilPtr initializations (*void -> T*) and the sieve
        // benchmark's free(*bool).
        if ((t1.name == "void" && t1.pointerLevel >= 1 && t2.pointerLevel >= t1.pointerLevel) ||
            (t2.name == "void" && t2.pointerLevel >= 1 && t1.pointerLevel >= t2.pointerLevel)) return true;

        // Pointer covariance for classes
        if (t1.pointerLevel > 0 && t2.pointerLevel > 0 && t1.pointerLevel == t2.pointerLevel) {
            auto sym2 = symbolTable_.resolve(t2.name);
            if (sym2 && sym2->kind == SymbolKind::CLASS) {
                auto classSym2 = std::static_pointer_cast<StructSymbol>(sym2);
                std::string parent = classSym2->parentName;
                while (!parent.empty()) {
                    if (parent == t1.name) return true;
                    auto parentSym = symbolTable_.resolve(parent);
                    if (parentSym && parentSym->kind == SymbolKind::CLASS) {
                        parent = std::static_pointer_cast<StructSymbol>(parentSym)->parentName;
                    } else {
                        break;
                    }
                }
            }
        }

        return false;
    }

    // Visitor methods
    void visit(AstProgram& program) override {
        for (auto& stmt : program.getStatements()) {
            auto type = stmt->getType();
            if (type != ASTNode::VAR_DECL_STMT &&
                type != ASTNode::FUNCTION_DECL_STMT &&
                type != ASTNode::STRUCT_DECL_STMT &&
                type != ASTNode::ENUM_DECL_STMT &&
                type != ASTNode::CLASS_DECL_STMT &&
                type != ASTNode::TYPE_ALIAS_STMT &&
                type != ASTNode::IMPORT_STMT &&
                type != ASTNode::FROM_IMPORT_STMT &&
                type != ASTNode::EXPORT_STMT &&
                type != ASTNode::ASM_STMT &&
                type != ASTNode::INTERFACE_DECL_STMT) {
                error("Executable statements are not allowed at the global scope", stmt->getLine());
            }
            stmt->accept(*this);
        }

        auto mainSym = symbolTable_.resolve("main");
        auto startSym = symbolTable_.resolve("_start");

        bool hasMain = mainSym && mainSym->kind == SymbolKind::FUNCTION;
        bool hasStart = startSym && startSym->kind == SymbolKind::FUNCTION;

        if (!moduleMode_ && !hasMain && !hasStart) {
            reporter_.error("Missing entry point: 'main' or '_start' function is required.", 1, 1);
        }
    }

    void visit(VarDeclStmt& stmt) override {
        TypeInfo declaredType = TypeInfo::fromAST(stmt.getTypeAST());
        // Resolve interface type
        if (!declaredType.isInterface && declaredType.pointerLevel == 0) {
            auto typeSym = symbolTable_.resolve(declaredType.name);
            if (typeSym && typeSym->kind == SymbolKind::INTERFACE) {
                declaredType.isInterface = true;
            }
        }

        if (stmt.getInitializer()) {
            stmt.getInitializer()->accept(*this);
            TypeInfo initType = lastExprType_;

            if (declaredType.isInterface) {
                auto ifaceSym = symbolTable_.resolve(declaredType.name);
                if (ifaceSym && ifaceSym->kind == SymbolKind::INTERFACE) {
                    auto iface = std::static_pointer_cast<InterfaceSymbol>(ifaceSym);
                    TypeInfo concreteType = initType;
                    if (concreteType.pointerLevel > 0) concreteType.pointerLevel--;
                    auto concreteSym = symbolTable_.resolve(concreteType.name);
                    if (!concreteSym || (concreteSym->kind != SymbolKind::STRUCT && concreteSym->kind != SymbolKind::CLASS)) {
                        error("Cannot assign non-struct/class type to interface '" + declaredType.name + "'", stmt.getLine());
                    } else {
                        auto ss = std::static_pointer_cast<StructSymbol>(concreteSym);
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
                                error("Type '" + concreteType.name + "' does not implement interface '" +
                                      declaredType.name + "': missing method '" + im.name + "'", stmt.getLine());
                            }
                        }
                    }
                }
            } else if (!areTypesCompatible(declaredType, initType)) {
                error("Type mismatch in declaration of '" + stmt.getName() +
                          "'. Expected " + declaredType.toString() + ", got " + initType.toString(),
                      stmt.getLine());
            }
        }

        auto sym = std::make_shared<Symbol>();
        sym->kind = SymbolKind::VARIABLE;
        sym->name = stmt.getName();
        sym->type = declaredType;
        sym->line = stmt.getLine();
        sym->col = stmt.getColumn();
        sym->isConst = stmt.isConst();
        symbolTable_.define(sym);
    }

    void visit(AssignStmt& stmt) override {
        stmt.getTarget()->accept(*this);
        TypeInfo targetType = lastExprType_;

        auto target = dynamic_cast<IdentifierExpr*>(stmt.getTarget());
        if (target) {
            auto sym = symbolTable_.resolve(target->getName());
            if (sym && sym->isConst) {
                error("Cannot assign to constant '" + target->getName() + "'", stmt.getLine());
            }
        }

        stmt.getValue()->accept(*this);
        TypeInfo valueType = lastExprType_;

        if (!areTypesCompatible(targetType, valueType)) {
            error("Type mismatch in assignment. Expected " + targetType.toString() +
                      ", got " + valueType.toString(),
                  stmt.getLine());
        }
    }

    void visit(BinaryExpr& expr) override {
        expr.getLeft()->accept(*this);
        TypeInfo leftType = lastExprType_;

        expr.getRight()->accept(*this);
        TypeInfo rightType = lastExprType_;

        auto isInteger = [](const std::string& name) {
            return name == "int" || name == "u8" || name == "i8" || name == "u16" || name == "i16" ||
                   name == "u32" || name == "i32" || name == "u64" || name == "i64" || name == "char" || name == "byte";
        };

        std::string op = expr.getOperator();

        bool isPtrMath = (leftType.pointerLevel > 0 && isInteger(rightType.name)) ||
                         (rightType.pointerLevel > 0 && isInteger(leftType.name) && op == "+");

        bool isMixedNumeric = (isInteger(leftType.name) && rightType.name == "float") ||
                              (leftType.name == "float" && isInteger(rightType.name));

        if (!areTypesCompatible(leftType, rightType) && !isPtrMath && !isMixedNumeric) {
            error("Type mismatch in binary expression '" + expr.getOperator() +
                      "'. " + leftType.toString() + " vs " + rightType.toString(),
                  expr.getLine());
        }

        if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=") {
            lastExprType_ = {"bool", 0};
        } else if (isPtrMath) {
            lastExprType_ = (leftType.pointerLevel > 0) ? leftType : rightType;
        } else if (isMixedNumeric) {
            lastExprType_ = {"float", 0};
        } else if (op == "|" || op == "&" || op == "^" || op == "<<" || op == ">>") {
            TypeInfo resolved = resolveType(leftType);
            if ((resolved.name != "int" && resolved.name.find('u') != 0 && resolved.name.find('i') != 0) || resolved.pointerLevel > 0) {
                error("Bitwise operator '" + op + "' requires integer operands", expr.getLine());
            }
            lastExprType_ = leftType;
        } else {
            lastExprType_ = leftType;
        }
    }

    void visit(UnaryExpr& expr) override {
        expr.getOperand()->accept(*this);
        TypeInfo operandType = lastExprType_;

        if (expr.getOperator() == "&") {
            lastExprType_ = operandType;
            lastExprType_.pointerLevel++;
        } else if (expr.getOperator() == "*") {
            if (operandType.pointerLevel == 0) {
                error("Cannot dereference non-pointer type " + operandType.toString(), expr.getLine());
            } else {
                lastExprType_ = operandType;
                lastExprType_.pointerLevel--;
            }
        } else if (expr.getOperator() == "!") {
            if (resolveType(operandType).name != "bool" || operandType.pointerLevel > 0) {
                error("Expected bool for '!' operator, got " + operandType.toString(), expr.getLine());
            }
            lastExprType_ = {"bool", 0};
        } else if (expr.getOperator() == "~") {
            TypeInfo resolved = resolveType(operandType);
            if ((resolved.name != "int" && resolved.name.find('u') != 0 && resolved.name.find('i') != 0) || operandType.pointerLevel > 0) {
                error("Expected int for '~' operator, got " + operandType.toString(), expr.getLine());
            }
            lastExprType_ = operandType;
        } else if (expr.getOperator() == "-") {
            if (operandType.pointerLevel > 0) {
                error("Cannot apply '-' to pointer type " + operandType.toString(), expr.getLine());
            }
            lastExprType_ = operandType;
        } else if (expr.getOperator() == "++" || expr.getOperator() == "--") {
            if (operandType.pointerLevel > 0) {
                error("Cannot " + expr.getOperator() + " a pointer type", expr.getLine());
            }
            lastExprType_ = operandType;
        }
    }

    void visit(IdentifierExpr& expr) override {
        if (expr.getName() == "_") {
            lastExprType_ = {"wildcard", 0};
            return;
        }
        auto sym = symbolTable_.resolve(expr.getName());
        if (!sym) {
            error("Undefined identifier '" + expr.getName() + "'", expr.getLine());
            lastExprType_ = {"error", 0};
        } else {
            if (sym->kind == SymbolKind::FUNCTION) {
                lastExprType_ = {"void", 1};
            } else {
                lastExprType_ = sym->type;
                // Resolve interface type
                if (!lastExprType_.isInterface && lastExprType_.pointerLevel == 0) {
                    auto typeSym = symbolTable_.resolve(lastExprType_.name);
                    if (typeSym && typeSym->kind == SymbolKind::INTERFACE) {
                        lastExprType_.isInterface = true;
                    }
                }
            }
        }
    }

    void visit(NumberExpr& expr) override {
        double val = expr.getValue();
        if (val == (long long)val) {
            lastExprType_ = {"int", 0};
        } else {
            lastExprType_ = {"float", 0};
        }
    }

    void visit(CharExpr&) override { lastExprType_ = {"char", 0}; }
    void visit(StringExpr&) override { lastExprType_ = {"string", 0}; }

    void visit(InterpolatedStringExpr& expr) override {
        for (const auto& part : expr.getParts()) {
            if (!part.isExpr) continue;
            part.expr->accept(*this);
            const std::string& t = lastExprType_.name;
            if (t == "int" || t == "bool" || t == "string" || t == "char") continue;
            if (t == "float") {
                error("Interpolation of float is not supported yet; cast to int first.", expr.getLine());
            } else {
                error("Interpolation requires int, bool, char or string, got " + t, expr.getLine());
            }
        }
        lastExprType_ = {"string", 0};
    }
    void visit(BooleanExpr&) override { lastExprType_ = {"bool", 0}; }
    void visit(NilExpr&) override { lastExprType_ = {"nil", 0}; }
    void visit(NilPtrExpr&) override { lastExprType_ = {"void", 1}; }

    void visit(NewExpr& expr) override {
        TypeInfo type = TypeInfo::fromAST(expr.getNewType());
        type.pointerLevel++;  // new T returns T*
        lastExprType_ = type;
    }

    void visit(MemberAccessExpr& expr) override {
        expr.getObject()->accept(*this);
        TypeInfo objType = lastExprType_;

        if (expr.isArrow()) {
            if (objType.pointerLevel == 0) {
                error("Cannot use '->' on non-pointer type " + objType.toString(), expr.getLine());
                lastExprType_ = {"error", 0};
                return;
            }
            objType.pointerLevel--;
        }

        // Check if object type is an interface
        auto ifaceSym = symbolTable_.resolve(objType.name);
        if (ifaceSym && ifaceSym->kind == SymbolKind::INTERFACE) {
            auto iface = std::static_pointer_cast<InterfaceSymbol>(ifaceSym);
            for (const auto& method : iface->methods) {
                if (method.name == expr.getMember()) {
                    lastExprType_ = method.returnType;
                    return;
                }
            }
            error("Method '" + expr.getMember() + "' not found in interface " + objType.name, expr.getLine());
            lastExprType_ = {"error", 0};
            return;
        }

        auto sym = symbolTable_.resolve(objType.name);
        if (!sym || (sym->kind != SymbolKind::STRUCT && sym->kind != SymbolKind::CLASS)) {
            error("Type " + objType.toString() + " is not a struct or class", expr.getLine());
            lastExprType_ = {"error", 0};
            return;
        }

        auto structSym = std::static_pointer_cast<StructSymbol>(sym);
        bool found = false;
        for (const auto& field : structSym->fields) {
            if (field.name == expr.getMember()) {
                lastExprType_ = field.type;
                found = true;
                break;
            }
        }

        if (!found) {
            for (const auto& method : structSym->methods) {
                if (method.name == expr.getMember()) {
                    lastExprType_ = method.returnType;
                    found = true;
                    break;
                }
            }
        }

        if (!found) {
            error("Member '" + expr.getMember() + "' not found in " + objType.name, expr.getLine());
            lastExprType_ = {"error", 0};
        }
    }

    void visit(StructLiteralExpr& expr) override {
        auto sym = symbolTable_.resolve(expr.getStructName());
        if (!sym || sym->kind != SymbolKind::STRUCT) {
            error("Unknown struct type '" + expr.getStructName() + "'", expr.getLine());
            lastExprType_ = {"error", 0};
            return;
        }

        auto structSym = std::static_pointer_cast<StructSymbol>(sym);
        for (auto& fieldInit : expr.getFields()) {
            bool fieldFound = false;
            for (const auto& fieldDef : structSym->fields) {
                if (fieldDef.name == fieldInit.first) {
                    fieldFound = true;
                    fieldInit.second->accept(*this);
                    if (!areTypesCompatible(fieldDef.type, lastExprType_)) {
                        error("Type mismatch for field '" + fieldDef.name +
                                  "'. Expected " + fieldDef.type.toString() + ", got " + lastExprType_.toString(),
                              expr.getLine());
                    }
                    break;
                }
            }
            if (!fieldFound) {
                error("Struct '" + expr.getStructName() + "' has no field '" + fieldInit.first + "'", expr.getLine());
            }
        }
        lastExprType_ = {expr.getStructName(), 0};
    }

    void visit(SizeofExpr& /*expr*/) override { lastExprType_ = {"int", 0}; }

    void visit(CastExpr& expr) override {
        expr.getExpression()->accept(*this);
        lastExprType_ = TypeInfo::fromAST(expr.getTargetType());
    }

    void visit(ArrayTypeAST&) override {}

    void visit(CallExpr& expr) override {
        auto callee = expr.getCallee();
        std::shared_ptr<FunctionSymbol> funcSym = nullptr;

        if (auto id = dynamic_cast<IdentifierExpr*>(callee)) {
            auto sym = symbolTable_.resolve(id->getName());
            if (sym && sym->kind == SymbolKind::FUNCTION) {
                funcSym = std::static_pointer_cast<FunctionSymbol>(sym);
            }
        } else if (auto mem = dynamic_cast<MemberAccessExpr*>(callee)) {
            mem->getObject()->accept(*this);
            TypeInfo objType = lastExprType_;
            if (objType.pointerLevel > 0) objType.pointerLevel--;

            auto sym = symbolTable_.resolve(objType.name);
            if (sym && (sym->kind == SymbolKind::CLASS || sym->kind == SymbolKind::STRUCT)) {
                auto classSym = std::static_pointer_cast<StructSymbol>(sym);
                for (const auto& m : classSym->methods) {
                    if (m.name == mem->getMember()) {
                        // Found the method
                        funcSym = std::make_shared<FunctionSymbol>();
                        funcSym->name = m.name;
                        funcSym->parameterTypes = m.parameterTypes;
                        funcSym->returnType = m.returnType;
                        break;
                    }
                }
            }
        }

        if (!funcSym) {
            // Function pointer call: accept callee and args generically
            callee->accept(*this);
            for (auto& arg : expr.getArgs()) {
                arg->accept(*this);
            }
            lastExprType_ = {"int", 0};
            return;
        }

        if (expr.getArgs().size() != funcSym->parameterTypes.size()) {
            error("Function '" + funcSym->name + "' expects " + std::to_string(funcSym->parameterTypes.size()) +
                      " arguments, got " + std::to_string(expr.getArgs().size()),
                  expr.getLine());
        } else {
            for (size_t i = 0; i < expr.getArgs().size(); ++i) {
                expr.getArgs()[i]->accept(*this);
                if (!areTypesCompatible(funcSym->parameterTypes[i], lastExprType_)) {
                    error("Argument " + std::to_string(i + 1) + " to '" + funcSym->name +
                              "' type mismatch. Expected " + funcSym->parameterTypes[i].toString() +
                              ", got " + lastExprType_.toString(),
                          expr.getLine());
                }
            }
        }
        lastExprType_ = funcSym->returnType;
    }

    void visit(FunctionDeclStmt& stmt) override {
        // Skip template function bodies (uninstantiated)
        if (stmt.hasTemplateParams()) return;

        symbolTable_.enterScope();

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
            if (!paramSym->type.isInterface && paramSym->type.pointerLevel == 0) {
                auto typeSym = symbolTable_.resolve(paramSym->type.name);
                if (typeSym && typeSym->kind == SymbolKind::INTERFACE) {
                    paramSym->type.isInterface = true;
                }
            }
            symbolTable_.define(paramSym);
        }
        if (!stmt.isExtern()) {
            for (auto& s : stmt.getBody()) s->accept(*this);
        }
        symbolTable_.exitScope();
    }

    void visit(BlockStmt& stmt) override {
        symbolTable_.enterScope();
        for (auto& s : stmt.getStatements()) s->accept(*this);
        symbolTable_.exitScope();
    }

    void visit(IfStmt& stmt) override {
        stmt.getCondition()->accept(*this);
        if (resolveType(lastExprType_).name != "bool" || lastExprType_.pointerLevel > 0) {
            error("If condition must be a boolean, got " + lastExprType_.toString(), stmt.getLine());
        }
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
        stmt.getCondition()->accept(*this);
        if (resolveType(lastExprType_).name != "bool" || lastExprType_.pointerLevel > 0) {
            error("While condition must be a boolean, got " + lastExprType_.toString(), stmt.getLine());
        }
        symbolTable_.enterScope();
        for (auto& s : stmt.getBody()) s->accept(*this);
        symbolTable_.exitScope();
    }

    void visit(ForStmt& stmt) override {
        symbolTable_.enterScope();
        if (stmt.getInitializer()) stmt.getInitializer()->accept(*this);
        if (stmt.getCondition()) {
            stmt.getCondition()->accept(*this);
            if (resolveType(lastExprType_).name != "bool" || lastExprType_.pointerLevel > 0) {
                error("For condition must be a boolean", stmt.getLine());
            }
        }
        if (stmt.getIncrement()) stmt.getIncrement()->accept(*this);
        for (auto& s : stmt.getBody()) s->accept(*this);
        symbolTable_.exitScope();
    }

    void visit(ReturnStmt& stmt) override {
        if (stmt.getValue()) stmt.getValue()->accept(*this);
        else lastExprType_ = {"void", 0};
    }

    void visit(ExpressionStmt& stmt) override { stmt.getExpression()->accept(*this); }

    void visit(ThisExpr& expr) override {
        auto sym = symbolTable_.resolve("this");
        if (sym) {
            lastExprType_ = sym->type;
        } else {
            error("'this' used outside of a class method", expr.getLine());
            lastExprType_ = {"error", 0};
        }
    }

    void visit(SuperExpr& expr) override {
        if (currentClassName_.empty()) {
            error("'super' can only be used inside a class", expr.getLine());
            lastExprType_ = {"error", 0};
            return;
        }
        auto classSym = symbolTable_.resolve(currentClassName_);
        if (!classSym || classSym->kind != SymbolKind::CLASS) {
            error("Current class '" + currentClassName_ + "' not found", expr.getLine());
            lastExprType_ = {"error", 0};
            return;
        }
        auto cs = std::static_pointer_cast<StructSymbol>(classSym);
        if (cs->parentName.empty()) {
            error("Class '" + currentClassName_ + "' has no parent class", expr.getLine());
            lastExprType_ = {"error", 0};
            return;
        }
        auto parentSym = symbolTable_.resolve(cs->parentName);
        if (!parentSym || (parentSym->kind != SymbolKind::CLASS && parentSym->kind != SymbolKind::STRUCT)) {
            error("Parent class '" + cs->parentName + "' not found", expr.getLine());
            lastExprType_ = {"error", 0};
            return;
        }
        auto ps = std::static_pointer_cast<StructSymbol>(parentSym);
        for (auto& m : ps->methods) {
            if (m.name == expr.getMember()) {
                lastExprType_ = m.returnType;
                return;
            }
        }
        error("Method '" + expr.getMember() + "' not found in parent class '" + cs->parentName + "'", expr.getLine());
        lastExprType_ = {"error", 0};
    }

    void visit(AwaitExpr& expr) override {
        // await expr — the awaited expression is usually a call to an async
        // function. Check it; the result type is the awaited value's type.
        expr.getValue()->accept(*this);
        // The awaited call must resolve to an async function.
        if (auto call = dynamic_cast<CallExpr*>(expr.getValue())) {
            if (auto id = dynamic_cast<IdentifierExpr*>(call->getCallee())) {
                auto sym = symbolTable_.resolve(id->getName());
                if (sym && sym->kind == SymbolKind::FUNCTION) {
                    auto fs = std::static_pointer_cast<FunctionSymbol>(sym);
                    if (!fs->isAsync) {
                        error("Can only await async functions, '" + id->getName() + "' is not async.", expr.getLine());
                    }
                }
            }
        }
    }

    void visit(ListExpr& expr) override {
        auto& elements = expr.getElements();
        if (elements.empty()) {
            lastExprType_ = {"void", 1};
            return;
        }
        elements[0]->accept(*this);
        TypeInfo elemType = lastExprType_;
        for (size_t i = 1; i < elements.size(); i++) {
            elements[i]->accept(*this);
            if (lastExprType_.name != elemType.name ||
                lastExprType_.pointerLevel != elemType.pointerLevel) {
                error("List element type mismatch: expected " +
                          elemType.toString() + ", got " + lastExprType_.toString(),
                      expr.getLine());
                lastExprType_ = {"error", 0};
                return;
            }
        }
        lastExprType_ = elemType;
        lastExprType_.pointerLevel++;
    }

    void visit(TableExpr& expr) override {
        auto& entries = expr.getEntries();
        if (entries.empty()) {
            lastExprType_ = {"void", 1};
            return;
        }
        entries[0].first->accept(*this);
        TypeInfo keyType = lastExprType_;
        entries[0].second->accept(*this);
        TypeInfo valType = lastExprType_;
        for (size_t i = 1; i < entries.size(); i++) {
            entries[i].first->accept(*this);
            if (lastExprType_.name != keyType.name || lastExprType_.pointerLevel != keyType.pointerLevel) {
                error("Table key type mismatch at entry " + std::to_string(i), expr.getLine());
                lastExprType_ = {"error", 0};
                return;
            }
            entries[i].second->accept(*this);
            if (lastExprType_.name != valType.name || lastExprType_.pointerLevel != valType.pointerLevel) {
                error("Table value type mismatch at entry " + std::to_string(i), expr.getLine());
                lastExprType_ = {"error", 0};
                return;
            }
        }
        lastExprType_ = {"void", 1};  // pointer to KV pairs
    }

    void visit(IndexExpr& expr) override {
        expr.getObject()->accept(*this);
        TypeInfo objType = lastExprType_;
        expr.getIndex()->accept(*this);
        TypeInfo idxType = lastExprType_;

        if (idxType.name != "int" && idxType.name != "u32" && idxType.name != "u64" && idxType.name != "i32" && idxType.name != "i64") {
            error("Array index must be an integer, got " + idxType.toString(), expr.getLine());
        }

        if (objType.isArray) {
            lastExprType_ = objType;
            lastExprType_.isArray = false;
            lastExprType_.arraySize = 0;
        } else if (objType.pointerLevel > 0 || objType.name == "string") {
            lastExprType_ = objType;
            if (objType.pointerLevel > 0) lastExprType_.pointerLevel--;
            else lastExprType_ = {"char", 0};
        } else {
            error("Cannot index non-array and non-pointer type " + objType.toString(), expr.getLine());
            lastExprType_ = {"error", 0};
        }
    }

    void visit(SliceExpr& expr) override {
        expr.getObject()->accept(*this);
        TypeInfo objType = lastExprType_;

        if (objType.isArray) {
            lastExprType_ = objType;
            lastExprType_.isArray = false;
            lastExprType_.isSlice = true;
            lastExprType_.arraySize = 0;
        } else {
            error("Cannot slice non-array type " + objType.toString(), expr.getLine());
            lastExprType_ = {"error", 0};
        }
    }

    void visit(TernaryExpr& expr) override {
        expr.getCondition()->accept(*this);
        if (resolveType(lastExprType_).name != "bool" || lastExprType_.pointerLevel > 0) {
            error("Ternary condition must be a boolean, got " + lastExprType_.toString(), expr.getLine());
        }
        expr.getThenExpr()->accept(*this);
        TypeInfo thenType = lastExprType_;
        expr.getElseExpr()->accept(*this);
        if (!areTypesCompatible(thenType, lastExprType_)) {
            error("Ternary branches must have compatible types, got " + thenType.toString() +
                      " and " + lastExprType_.toString(),
                  expr.getLine());
        }
        lastExprType_ = thenType;
    }

    void visit(GroupingExpr& expr) override {
        expr.getExpression()->accept(*this);
    }

    void visit(FunctionExpr& expr) override {
        // Closure literal: check the body in a fresh scope with the params,
        // then type it as a function pointer. Captured identifiers resolve
        // in the enclosing scopes (they are read at the closure site).
        symbolTable_.enterScope();
        for (const auto& p : expr.getParams()) {
            auto sym = std::make_shared<Symbol>();
            sym->kind = SymbolKind::VARIABLE;
            sym->name = p.first;
            if (p.second) sym->type = TypeInfo::fromAST(p.second.get());
            symbolTable_.define(sym);
        }
        for (auto& s : expr.getBody()) {
            s->accept(*this);
        }
        symbolTable_.exitScope();
        lastExprType_ = {"func", 1};
    }
    void visit(TypeAST&) override {}
    void visit(StructDeclStmt&) override {}
    void visit(EnumDeclStmt&) override {}
    void visit(TypeAliasStmt&) override {}

    void visit(ClassDeclStmt& stmt) override {
        std::string prevClass = currentClassName_;
        currentClassName_ = stmt.getName();
        for (auto& member : stmt.getMembers()) {
            member->accept(*this);
        }
        currentClassName_ = prevClass;
    }

    void visit(ImportStmt&) override {}
    void visit(FromImportStmt&) override {}
    void visit(ExportStmt& stmt) override { stmt.getStmt()->accept(*this); }
    void visit(ExportExpressionStmt&) override {}
    void visit(InterfaceDeclStmt&) override {}

    void visit(ForEachStmt& stmt) override {
        stmt.getIterable()->accept(*this);
        TypeInfo iterableType = lastExprType_;

        TypeInfo elemType;
        if (iterableType.isArray) {
            elemType = iterableType;
            elemType.isArray = false;
            elemType.arraySize = 0;
        } else if (iterableType.isSlice) {
            elemType = iterableType;
            elemType.isSlice = false;
        } else if (iterableType.pointerLevel == 0 &&
                   (symbolTable_.resolve(iterableType.name) &&
                    (symbolTable_.resolve(iterableType.name)->kind == SymbolKind::STRUCT ||
                     symbolTable_.resolve(iterableType.name)->kind == SymbolKind::CLASS))) {
            // Custom iterator protocol: iter() -> It, next(self: It) -> *T.
            auto sym = symbolTable_.resolve(iterableType.name);
            auto ss = std::static_pointer_cast<StructSymbol>(sym);
            const StructSymbol::Method* iterM = nullptr;
            const StructSymbol::Method* nextM = nullptr;
            for (const auto& m : ss->methods) {
                if (m.name == "iter") iterM = &m;
                else if (m.name == "next") nextM = &m;
            }
            if (!iterM || !nextM) {
                error("For-in over '" + iterableType.name + "' requires iter() and next() methods.", stmt.getLine());
                return;
            }
            if (nextM->returnType.pointerLevel < 1) {
                error("Iterator's next() must return a pointer, got " + nextM->returnType.toString(), stmt.getLine());
                return;
            }
            elemType = nextM->returnType;
            elemType.pointerLevel--;
            // The pointee's user-defined flag must reflect reality: an int
            // iterator element is not a struct (the flag would route the
            // loop variable through getAddress instead of a value load).
            if (elemType.pointerLevel == 0 && !elemType.isArray) {
                auto ts = symbolTable_.resolve(elemType.name);
                if (!ts || (ts->kind != SymbolKind::STRUCT && ts->kind != SymbolKind::CLASS)) {
                    elemType.isUserDefined = false;
                }
            }
        } else {
            error("For-in iterable must be an array, slice, or a type with iter()/next(), got " + iterableType.toString(), stmt.getLine());
            return;
        }

        symbolTable_.enterScope();
        auto sym = std::make_shared<Symbol>();
        sym->kind = SymbolKind::VARIABLE;
        sym->name = stmt.getVarName();
        sym->type = elemType;
        sym->line = stmt.getLine();
        sym->col = stmt.getColumn();
        symbolTable_.define(sym);

        for (auto& s : stmt.getBody()) {
            s->accept(*this);
        }
        symbolTable_.exitScope();
    }

    void visit(DeleteStmt& stmt) override {
        stmt.getTarget()->accept(*this);
        if (lastExprType_.pointerLevel == 0 && lastExprType_.name != "wildcard") {
            error("Cannot delete non-pointer type " + lastExprType_.toString(), stmt.getLine());
        }
    }

    void visit(BreakStmt&) override {}
    void visit(ContinueStmt&) override {}

    void visit(ThrowStmt& stmt) override {
        stmt.getValue()->accept(*this);
    }

    void visit(TryCatchStmt& stmt) override {
        symbolTable_.enterScope();
        for (auto& s : stmt.getTryBlock()) {
            s->accept(*this);
        }
        symbolTable_.exitScope();

        symbolTable_.enterScope();
        auto catchVarSym = std::make_shared<Symbol>();
        catchVarSym->kind = SymbolKind::VARIABLE;
        catchVarSym->name = stmt.getCatchVar();
        catchVarSym->type = {"int", 0};
        symbolTable_.define(catchVarSym);
        for (auto& s : stmt.getCatchBlock()) {
            s->accept(*this);
        }
        symbolTable_.exitScope();
    }

    void visit(DeferStmt& stmt) override {
        stmt.getDeferredStmt()->accept(*this);
    }

    void visit(AsmStmt&) override {}

    void visit(MatchStmt& stmt) override {
        stmt.getExpression()->accept(*this);
        TypeInfo matchType = lastExprType_;

        for (auto& arm : stmt.getArms()) {
            // The `default` arm matches everything.
            bool isDefault = false;
            if (auto pid = dynamic_cast<IdentifierExpr*>(arm.pattern.get())) {
                isDefault = (pid->getName() == "default");
            }
            if (!isDefault) {
                arm.pattern->accept(*this);
                if (!areTypesCompatible(matchType, lastExprType_)) {
                    error("Match pattern type mismatch. Expected " + matchType.toString() +
                              ", got " + lastExprType_.toString(),
                          arm.pattern->getLine());
                }
            }

            symbolTable_.enterScope();
            arm.body->accept(*this);
            symbolTable_.exitScope();
        }
    }

private:
    SymbolTable& symbolTable_;
    ErrorReporter& reporter_;
    TypeInfo lastExprType_ = {"void", 0};
    std::string currentClassName_;
    bool moduleMode_ = false;

    void error(const std::string& msg, int line) {
        reporter_.error(msg, line);
    }
};

}  // namespace cider
