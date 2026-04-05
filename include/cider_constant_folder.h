#ifndef CIDER_CONSTANT_FOLDER_H
#define CIDER_CONSTANT_FOLDER_H

#include "cider_ast.h"
#include <memory>
#include <iostream>

namespace Cider {

    class ConstantFolder : public ASTVisitor {
    public:
        ConstantFolder() {}

        void fold(Program& program) {
            program.accept(*this);
        }

        std::unique_ptr<Expression> foldExpression(std::unique_ptr<Expression> expr) {
            if (!expr) return nullptr;
            expr->accept(*this);
            if (lastFoldedExpr_) {
                auto res = std::move(lastFoldedExpr_);
                lastFoldedExpr_ = nullptr;
                return res;
            }
            return expr;
        }

        void visit(Program& program) override {
            for (auto& imported : program.getImportedAsts()) {
                imported->accept(*this);
            }
            for (auto& stmt : program.getStatements()) {
                stmt->accept(*this);
            }
        }

        void visit(VarDeclStmt& stmt) override {
            if (stmt.getInitializer()) {
                auto init = stmt.takeInitializer();
                stmt.setInitializer(foldExpression(std::move(init)));
            }
        }

        void visit(BinaryExpr& expr) override {
            auto left = expr.takeLeft();
            auto right = expr.takeRight();
            
            auto foldedLeft = foldExpression(std::move(left));
            auto foldedRight = foldExpression(std::move(right));
            
            auto leftNum = dynamic_cast<NumberExpr*>(foldedLeft.get());
            auto rightNum = dynamic_cast<NumberExpr*>(foldedRight.get());
            
            if (leftNum && rightNum) {
                double l = leftNum->getValue();
                double r = rightNum->getValue();
                std::string op = expr.getOperator();
                
                std::unique_ptr<NumberExpr> res = nullptr;
                if (op == "+") res = std::make_unique<NumberExpr>(l + r, expr.getLine(), expr.getColumn());
                else if (op == "-") res = std::make_unique<NumberExpr>(l - r, expr.getLine(), expr.getColumn());
                else if (op == "*") res = std::make_unique<NumberExpr>(l * r, expr.getLine(), expr.getColumn());
                else if (op == "/") {
                    if (r != 0) res = std::make_unique<NumberExpr>(l / r, expr.getLine(), expr.getColumn());
                }
                
                if (res) {
                    std::cout << "Constant Folding: " << l << " " << op << " " << r << " -> " << res->getValue() << "\n";
                    lastFoldedExpr_ = std::move(res);
                    return;
                }
            }
            
            // Re-attach folded (or original) children
            expr.setLeft(std::move(foldedLeft));
            expr.setRight(std::move(foldedRight));
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
        void visit(UnaryExpr& expr) override {
            auto operand = expr.getOperand(); // Can't take operand easily yet, skip for now
        }
        void visit(CallExpr&) override {}
        void visit(MemberAccessExpr&) override {}
        void visit(ListExpr&) override {}
        void visit(TableExpr&) override {}
        void visit(IndexExpr&) override {}
        void visit(SliceExpr&) override {}
        void visit(TernaryExpr&) override {}
        void visit(GroupingExpr& expr) override {
            expr.getExpression()->accept(*this);
            if (lastFoldedExpr_) {
                // Should replace inner expr? For now just propagate lastFoldedExpr
            }
        }
        void visit(FunctionExpr&) override {}
        void visit(SuperExpr&) override {}
        void visit(AwaitExpr&) override {}
        void visit(StructLiteralExpr&) override {}
        void visit(SizeofExpr&) override {}
        void visit(CastExpr&) override {}
        void visit(ArrayTypeAST&) override {}
        void visit(TypeAST&) override {}
        void visit(AssignStmt&) override {}
        void visit(IfStmt&) override {}
        void visit(WhileStmt&) override {}
        void visit(ForStmt&) override {}
        void visit(ForEachStmt&) override {}
        void visit(DeleteStmt&) override {}
        void visit(BreakStmt&) override {}
        void visit(ContinueStmt&) override {}
        void visit(ThrowStmt&) override {}
        void visit(TryCatchStmt&) override {}
        void visit(DeferStmt& stmt) override {
            stmt.getDeferredStmt()->accept(*this);
        }
        void visit(AsmStmt&) override {}
        void visit(MatchStmt& stmt) override {
            stmt.getExpression()->accept(*this);
            for (auto& arm : stmt.getArms()) arm.body->accept(*this);
        }
        void visit(BlockStmt&) override {}
        void visit(ReturnStmt&) override {}
        void visit(ExpressionStmt& stmt) override {
            // Need a way to take expression from ExpressionStmt
        }
        void visit(FunctionDeclStmt& stmt) override {
            for (auto& s : stmt.getBody()) s->accept(*this);
        }
        void visit(StructDeclStmt&) override {}
        void visit(EnumDeclStmt&) override {}
        void visit(TypeAliasStmt&) override {}
        void visit(ClassDeclStmt&) override {}
        void visit(ImportStmt&) override {}
        void visit(FromImportStmt&) override {}
        void visit(ExportStmt& stmt) override { stmt.getStmt()->accept(*this); }
        void visit(ExportExpressionStmt&) override {}

    private:
        std::unique_ptr<Expression> lastFoldedExpr_ = nullptr;
    };

} // namespace Cider

#endif
