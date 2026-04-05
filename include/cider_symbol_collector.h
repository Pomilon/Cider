#ifndef CIDER_SYMBOL_COLLECTOR_H
#define CIDER_SYMBOL_COLLECTOR_H

#include "cider_ast.h"
#include "cider_symbols.h"
#include "cider_module_resolver.h"
#include "cider_lexer.h"
#include "cider_parser.h"
#include "cider_errors.h"
#include <iostream>
#include <fstream>
#include <sstream>

namespace Cider {

    class SymbolCollector : public ASTVisitor {
    public:
        SymbolCollector(SymbolTable& symbolTable, ModuleResolver& resolver, ErrorReporter& reporter) 
            : symbolTable_(symbolTable), resolver_(resolver), reporter_(reporter), currentProgram_(nullptr) {}

        void collect(Program& program) {
            currentProgram_ = &program;
            program.accept(*this);
        }

        // Visitor methods
        void visit(Program& program) override {
            for (auto& stmt : program.getStatements()) {
                stmt->accept(*this);
            }
        }

        void visit(VarDeclStmt& stmt) override {
            auto sym = std::make_shared<Symbol>();
            sym->kind = SymbolKind::VARIABLE;
            sym->name = stmt.getName();
            sym->type = TypeInfo::fromAST(stmt.getTypeAST());
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
            auto sym = std::make_shared<FunctionSymbol>();
            sym->kind = SymbolKind::FUNCTION;
            sym->name = stmt.getName();
            sym->type = {stmt.getName(), 0}; // Use name as type for resolution
            sym->line = stmt.getLine();
            sym->col = stmt.getColumn();
            sym->returnType = TypeInfo::fromAST(stmt.getReturnType());

            for (auto& param : stmt.getParams()) {
                sym->parameterTypes.push_back(TypeInfo::fromAST(param.second.get()));
            }

            if (symbolTable_.getCurrentScope()->resolveLocal(sym->name)) {
                reporter_.error("Redeclaration of function '" + sym->name + "'", sym->line, sym->col);
            } else {
                symbolTable_.define(sym);
            }

            symbolTable_.enterScope();
            for (auto& param : stmt.getParams()) {
                auto paramSym = std::make_shared<Symbol>();
                paramSym->kind = SymbolKind::VARIABLE;
                paramSym->name = param.first;
                paramSym->type = TypeInfo::fromAST(param.second.get());
                paramSym->line = stmt.getLine();
                paramSym->col = stmt.getColumn();
                symbolTable_.define(paramSym);
            }

            for (auto& s : stmt.getBody()) {
                s->accept(*this);
            }
            symbolTable_.exitScope();
        }
        void visit(StructDeclStmt& stmt) override {
            auto sym = std::make_shared<StructSymbol>();
            sym->kind = SymbolKind::STRUCT;
            sym->name = stmt.getName();
            sym->line = stmt.getLine();
            sym->col = stmt.getColumn();

            int currentOffset = 0;
            for (auto& field : stmt.getFields()) {
                StructSymbol::Field f;
                f.name = field.first;
                f.type = TypeInfo::fromAST(field.second.get());
                f.offset = currentOffset;

                // Foundation: Calculate size (simplified to 8-byte alignment for now)
                int size = 8; 
                if (f.type.isArray) size = 8 * f.type.arraySize;

                currentOffset += size;
                sym->fields.push_back(f);
            }
            sym->totalSize = currentOffset;

            if (symbolTable_.getCurrentScope()->resolveLocal(sym->name)) {
                reporter_.error("Redeclaration of type '" + sym->name + "'", sym->line, sym->col);
            } else {
                symbolTable_.define(sym);
            }
        }

        void visit(EnumDeclStmt& stmt) override {
            auto sym = std::make_shared<Symbol>();
            sym->kind = SymbolKind::ENUM;
            sym->name = stmt.getName();
            sym->line = stmt.getLine();
            sym->col = stmt.getColumn();

            if (symbolTable_.getCurrentScope()->resolveLocal(sym->name)) {
                reporter_.error("Redeclaration of enum '" + sym->name + "'", sym->line, sym->col);
            } else {
                symbolTable_.define(sym);
            }
        }

        void visit(TypeAliasStmt& stmt) override {
            auto sym = std::make_shared<Symbol>();
            sym->kind = SymbolKind::STRUCT; // Treat alias as a type provider
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
            auto result = resolver_.resolve(stmt.getModuleName());
            if (result.type == ModuleType::NOT_FOUND) {
                reporter_.error("Could not resolve module '" + stmt.getModuleName() + "'", stmt.getLine(), stmt.getColumn());
                return;
            }

            std::ifstream file(result.path);
            if (!file) return;
            std::stringstream buffer;
            buffer << file.rdbuf();
            
            Lexer lexer(buffer.str());
            Parser parser(lexer, reporter_);
            auto importedAst = parser.parseProgram();
            
            if (importedAst) {
                importedAst->accept(*this);
                if (currentProgram_) {
                    currentProgram_->addImportedAst(std::move(importedAst));
                }
            }
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
        void visit(ListExpr&) override {}
        void visit(TableExpr&) override {}
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
            // Inherit from parent if it exists (Foundation: Layout Engine)
            if (!sym->parentName.empty()) {
                auto parent = symbolTable_.resolve(sym->parentName);
                if (parent && parent->kind == SymbolKind::CLASS) {
                    auto parentStruct = std::static_pointer_cast<StructSymbol>(parent);
                    sym->fields = parentStruct->fields;
                    sym->methods = parentStruct->methods;
                    currentOffset = parentStruct->totalSize;
                }
            }

            for (auto& member : stmt.getMembers()) {
                if (auto field = dynamic_cast<VarDeclStmt*>(member.get())) {
                    StructSymbol::Field f;
                    f.name = field->getName();
                    f.type = TypeInfo::fromAST(field->getTypeAST());
                    f.offset = currentOffset;
                    currentOffset += 8; // Simplified alignment
                    sym->fields.push_back(f);
                } else if (auto method = dynamic_cast<FunctionDeclStmt*>(member.get())) {
                    StructSymbol::Method m;
                    m.name = method->getName();
                    for (auto& p : method->getParams()) {
                        m.parameterTypes.push_back(TypeInfo::fromAST(p.second.get()));
                    }
                    m.returnType = TypeInfo::fromAST(method->getReturnType());
                    m.vtableIndex = sym->methods.size();
                    sym->methods.push_back(m);
                }
            }
            sym->totalSize = currentOffset;
            symbolTable_.define(sym);
        }
        void visit(FromImportStmt& stmt) override {
            auto result = resolver_.resolve(stmt.getModuleName());
            if (result.type == ModuleType::NOT_FOUND) {
                reporter_.error("Could not resolve module '" + stmt.getModuleName() + "'", stmt.getLine(), stmt.getColumn());
                return;
            }

            std::ifstream file(result.path);
            if (!file) return;
            std::stringstream buffer;
            buffer << file.rdbuf();
            
            Lexer lexer(buffer.str());
            Parser parser(lexer, reporter_);
            auto importedAst = parser.parseProgram();
            
            if (importedAst) {
                importedAst->accept(*this);
                if (currentProgram_) {
                    currentProgram_->addImportedAst(std::move(importedAst));
                }
            }
        }
        void visit(ExportStmt& stmt) override { stmt.getStmt()->accept(*this); }
        void visit(ExportExpressionStmt&) override {}

    private:
        SymbolTable& symbolTable_;
        ModuleResolver& resolver_;
        ErrorReporter& reporter_;
        Program* currentProgram_;
    };

} // namespace Cider

#endif
