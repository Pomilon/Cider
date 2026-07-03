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
            auto operand = expr.takeOperand();
            auto folded = foldExpression(std::move(operand));
            if (auto num = dynamic_cast<NumberExpr*>(folded.get())) {
                double v = num->getValue();
                std::string op = expr.getOperator();
                if (op == "-") {
                    lastFoldedExpr_ = std::make_unique<NumberExpr>(-v, expr.getLine(), expr.getColumn());
                    return;
                } else if (op == "!") {
                    lastFoldedExpr_ = std::make_unique<NumberExpr>(v == 0 ? 1.0 : 0.0, expr.getLine(), expr.getColumn());
                    return;
                }
            }
            expr.setOperand(std::move(folded));
        }
        void visit(CallExpr&) override {}
        void visit(MemberAccessExpr&) override {}
        void visit(ListExpr&) override {}
        void visit(TableExpr&) override {}
        void visit(IndexExpr&) override {}
        void visit(SliceExpr&) override {}
        void visit(TernaryExpr& expr) override {
            auto cond = expr.takeCondition();
            auto folded = foldExpression(std::move(cond));
            if (auto num = dynamic_cast<NumberExpr*>(folded.get())) {
                if (num->getValue() != 0) {
                    lastFoldedExpr_ = expr.takeThenExpr();
                } else {
                    lastFoldedExpr_ = expr.takeElseExpr();
                }
                if (lastFoldedExpr_) lastFoldedExpr_ = foldExpression(std::move(lastFoldedExpr_));
                return;
            }
            expr.setCondition(std::move(folded));
        }
        void visit(GroupingExpr& expr) override {
            auto inner = expr.takeExpression();
            lastFoldedExpr_ = foldExpression(std::move(inner));
        }
        void visit(FunctionExpr&) override {}
        void visit(SuperExpr&) override {}
        void visit(AwaitExpr&) override {}
        void visit(StructLiteralExpr&) override {}
        void visit(SizeofExpr& expr) override {
            // Will be handled in IR generation. For now just leave it.
        }
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
            auto expr = stmt.takeExpression();
            stmt.setExpression(foldExpression(std::move(expr)));
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
        void visit(InterfaceDeclStmt&) override {}

    private:
        std::unique_ptr<Expression> lastFoldedExpr_ = nullptr;
    };

} // namespace Cider

#endif
