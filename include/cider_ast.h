#ifndef CIDER_AST_H
#define CIDER_AST_H

#include <vector>
#include <memory>
#include <string>

namespace Cider
{

    class ASTVisitor;

    class ASTNode
    {
    public:
        enum NodeType
        {
            EXPRESSION,
            STATEMENT,
            PROGRAM,
            NUMBER_EXPR,
            STRING_EXPR,
            CHAR_EXPR,
            BOOLEAN_EXPR,
            NEW_EXPR,
            NIL_EXPR,
            NIL_PTR_EXPR,
            IDENTIFIER_EXPR,
            BINARY_EXPR,
            UNARY_EXPR,
            CALL_EXPR,
            MEMBER_ACCESS_EXPR,
            LIST_EXPR,
            TABLE_EXPR,
            INDEX_EXPR,
            SLICE_EXPR,
            TERNARY_EXPR,
            GROUPING_EXPR,
            THIS_EXPR,
            SUPER_EXPR,
            FUNCTION_EXPR,
            AWAIT_EXPR,
            STRUCT_LITERAL_EXPR,
            SIZEOF_EXPR,
            CAST_EXPR,
            ARRAY_TYPE,
            VAR_DECL_STMT,
            ASSIGN_STMT,
            IF_STMT,
            WHILE_STMT,
            FOR_STMT,
            FOR_EACH_STMT,
            DELETE_STMT,
            BREAK_STMT,
            CONTINUE_STMT,
            THROW_STMT,
            TRY_CATCH_STMT,
            DEFER_STMT,
            ASM_STMT,
            MATCH_STMT,
            BLOCK_STMT,
            RETURN_STMT,
            EXPRESSION_STMT,
            FUNCTION_DECL_STMT,
            CLASS_DECL_STMT,
            STRUCT_DECL_STMT,
            ENUM_DECL_STMT,
            TYPE_ALIAS_STMT,
            IMPORT_STMT,
            FROM_IMPORT_STMT,
            EXPORT_STMT,
            EXPORT_EXPRESSION_STMT,
            TYPE_AST
        };

        ASTNode(NodeType type, int line, int col) : type_(type), line_(line), col_(col) {}
        virtual ~ASTNode() = default;

        NodeType getType() const { return type_; }
        int getLine() const { return line_; }
        int getColumn() const { return col_; }

        virtual void accept(ASTVisitor &visitor) = 0;

    private:
        NodeType type_;
        int line_;
        int col_;
    };

    class TypeAST : public ASTNode
    {
    public:
        TypeAST(const std::string &name, int pointerLevel, int line, int col)
            : ASTNode(TYPE_AST, line, col), name_(name), pointerLevel_(pointerLevel) {}
        
        const std::string &getName() const { return name_; }
        int getPointerLevel() const { return pointerLevel_; }
        
        void accept(ASTVisitor &visitor) override;

    private:
        std::string name_;
        int pointerLevel_;
    };

    class Expression : public ASTNode
    {
    public:
        Expression(NodeType type, int line, int col) : ASTNode(type, line, col) {}
    };

    class Statement : public ASTNode
    {
    public:
        Statement(NodeType type, int line, int col) : ASTNode(type, line, col) {}
    };

    class NumberExpr : public Expression
    {
    public:
        explicit NumberExpr(double value, int line, int col) : Expression(NUMBER_EXPR, line, col), value_(value) {}
        double getValue() const { return value_; }
        void accept(ASTVisitor &visitor) override;
    private:
        double value_;
    };

    class StringExpr : public Expression
    {
    public:
        explicit StringExpr(const std::string &value, int line, int col) : Expression(STRING_EXPR, line, col), value_(value) {}
        const std::string &getValue() const { return value_; }
        void accept(ASTVisitor &visitor) override;
    private:
        std::string value_;
    };

    class CharExpr : public Expression
    {
    public:
        explicit CharExpr(char value, int line, int col) : Expression(CHAR_EXPR, line, col), value_(value) {}
        char getValue() const { return value_; }
        void accept(ASTVisitor &visitor) override;
    private:
        char value_;
    };

    class BooleanExpr : public Expression
    {
    public:
        explicit BooleanExpr(bool value, int line, int col) : Expression(BOOLEAN_EXPR, line, col), value_(value) {}
        bool getValue() const { return value_; }
        void accept(ASTVisitor &visitor) override;
    private:
        bool value_;
    };

    class NilExpr : public Expression
    {
    public:
        explicit NilExpr(int line, int col) : Expression(NIL_EXPR, line, col) {}
        void accept(ASTVisitor &visitor) override;
    };

    class NilPtrExpr : public Expression
    {
    public:
        NilPtrExpr(int line, int col) : Expression(NIL_PTR_EXPR, line, col) {}
        void accept(ASTVisitor &visitor) override;
    };

    class NewExpr : public Expression
    {
    public:
        NewExpr(std::unique_ptr<TypeAST> type, int line, int col)
            : Expression(NEW_EXPR, line, col), type_(std::move(type)) {}
        TypeAST* getNewType() const { return type_.get(); }
        void accept(ASTVisitor &visitor) override;
    private:
        std::unique_ptr<TypeAST> type_;
    };

    class IdentifierExpr : public Expression
    {
    public:
        explicit IdentifierExpr(const std::string &name, int line, int col) : Expression(IDENTIFIER_EXPR, line, col), name_(name) {}
        const std::string &getName() const { return name_; }
        void accept(ASTVisitor &visitor) override;
    private:
        std::string name_;
    };

    class SuperExpr : public Expression
    {
    public:
        SuperExpr(const std::string& member, int line, int col) : Expression(SUPER_EXPR, line, col), member_(member) {}
        const std::string& getMember() const { return member_; }
        void accept(ASTVisitor &visitor) override;
    private:
        std::string member_;
    };

    class ThisExpr : public Expression
    {
    public:
        ThisExpr(int line, int col) : Expression(THIS_EXPR, line, col) {}
        void accept(ASTVisitor &visitor) override;
    };

    class BinaryExpr : public Expression
    {
    public:
        BinaryExpr(std::unique_ptr<Expression> left, const std::string &op, std::unique_ptr<Expression> right, int line, int col)
            : Expression(BINARY_EXPR, line, col), left_(std::move(left)), op_(op), right_(std::move(right)) {}
        Expression *getLeft() const { return left_.get(); }
        const std::string &getOperator() const { return op_; }
        Expression *getRight() const { return right_.get(); }
        void setLeft(std::unique_ptr<Expression> left) { left_ = std::move(left); }
        void setRight(std::unique_ptr<Expression> right) { right_ = std::move(right); }
        std::unique_ptr<Expression> takeLeft() { return std::move(left_); }
        std::unique_ptr<Expression> takeRight() { return std::move(right_); }
        void accept(ASTVisitor &visitor) override;
    private:
        std::unique_ptr<Expression> left_;
        std::string op_;
        std::unique_ptr<Expression> right_;
    };

    class UnaryExpr : public Expression
    {
    public:
        UnaryExpr(const std::string &op, std::unique_ptr<Expression> operand, int line, int col)
            : Expression(UNARY_EXPR, line, col), op_(op), operand_(std::move(operand)) {}
        const std::string &getOperator() const { return op_; }
        Expression *getOperand() const { return operand_.get(); }
        void accept(ASTVisitor &visitor) override;
    private:
        std::string op_;
        std::unique_ptr<Expression> operand_;
    };

    class CallExpr : public Expression
    {
    public:
        CallExpr(std::unique_ptr<Expression> callee, std::vector<std::unique_ptr<Expression>> args, int line, int col)
            : Expression(CALL_EXPR, line, col), callee_(std::move(callee)), args_(std::move(args)) {}
        Expression *getCallee() const { return callee_.get(); }
        const std::vector<std::unique_ptr<Expression>> &getArgs() const { return args_; }
        void accept(ASTVisitor &visitor) override;
    private:
        std::unique_ptr<Expression> callee_;
        std::vector<std::unique_ptr<Expression>> args_;
    };

    class MemberAccessExpr : public Expression
    {
    public:
        MemberAccessExpr(std::unique_ptr<Expression> object, const std::string &member, int line, int col, bool isArrow = false)
            : Expression(MEMBER_ACCESS_EXPR, line, col), object_(std::move(object)), member_(member), isArrow_(isArrow) {}
        Expression *getObject() const { return object_.get(); }
        const std::string &getMember() const { return member_; }
        bool isArrow() const { return isArrow_; }
        void accept(ASTVisitor &visitor) override;
    private:
        std::unique_ptr<Expression> object_;
        std::string member_;
        bool isArrow_;
    };

    class ListExpr : public Expression
    {
    public:
        explicit ListExpr(std::vector<std::unique_ptr<Expression>> elements, int line, int col)
            : Expression(LIST_EXPR, line, col), elements_(std::move(elements)) {}
        const std::vector<std::unique_ptr<Expression>> &getElements() const { return elements_; }
        void accept(ASTVisitor &visitor) override;
    private:
        std::vector<std::unique_ptr<Expression>> elements_;
    };

    class TableExpr : public Expression
    {
    public:
        using Entry = std::pair<std::unique_ptr<Expression>, std::unique_ptr<Expression>>;
        explicit TableExpr(std::vector<Entry> entries, int line, int col)
            : Expression(TABLE_EXPR, line, col), entries_(std::move(entries)) {}
        const std::vector<Entry> &getEntries() const { return entries_; }
        void accept(ASTVisitor &visitor) override;
    private:
        std::vector<Entry> entries_;
    };

    class IndexExpr : public Expression
    {
    public:
        IndexExpr(std::unique_ptr<Expression> object, std::unique_ptr<Expression> index, int line, int col)
            : Expression(INDEX_EXPR, line, col), object_(std::move(object)), index_(std::move(index)) {}
        Expression *getObject() const { return object_.get(); }
        Expression *getIndex() const { return index_.get(); }
        void accept(ASTVisitor &visitor) override;
    private:
        std::unique_ptr<Expression> object_;
        std::unique_ptr<Expression> index_;
    };

    class SliceExpr : public Expression
    {
    public:
        SliceExpr(std::unique_ptr<Expression> object, std::unique_ptr<Expression> start, std::unique_ptr<Expression> end, int line, int col)
            : Expression(SLICE_EXPR, line, col), object_(std::move(object)), start_(std::move(start)), end_(std::move(end)) {}
        Expression *getObject() const { return object_.get(); }
        Expression *getStart() const { return start_.get(); }
        Expression *getEnd() const { return end_.get(); }
        void accept(ASTVisitor &visitor) override;
    private:
        std::unique_ptr<Expression> object_;
        std::unique_ptr<Expression> start_;
        std::unique_ptr<Expression> end_;
    };

    class TernaryExpr : public Expression
    {
    public:
        TernaryExpr(std::unique_ptr<Expression> condition, std::unique_ptr<Expression> thenExpr, std::unique_ptr<Expression> elseExpr, int line, int col)
            : Expression(TERNARY_EXPR, line, col), condition_(std::move(condition)), thenExpr_(std::move(thenExpr)), elseExpr_(std::move(elseExpr)) {}
        Expression *getCondition() const { return condition_.get(); }
        Expression *getThenExpr() const { return thenExpr_.get(); }
        Expression *getElseExpr() const { return elseExpr_.get(); }
        void accept(ASTVisitor &visitor) override;
    private:
        std::unique_ptr<Expression> condition_;
        std::unique_ptr<Expression> thenExpr_;
        std::unique_ptr<Expression> elseExpr_;
    };

    class GroupingExpr : public Expression
    {
    public:
        GroupingExpr(std::unique_ptr<Expression> expression, int line, int col)
            : Expression(GROUPING_EXPR, line, col), expression_(std::move(expression)) {}
        Expression *getExpression() const { return expression_.get(); }
        void accept(ASTVisitor &visitor) override;
    private:
        std::unique_ptr<Expression> expression_;
    };

    class FunctionExpr : public Expression
    {
    public:
        FunctionExpr(const std::string &name, std::vector<std::string> params,
                     std::vector<std::unique_ptr<Statement>> body, int line, int col, bool isAsync = false)
            : Expression(FUNCTION_EXPR, line, col), name_(name), params_(std::move(params)), body_(std::move(body)), isAsync_(isAsync) {}
        const std::string &getName() const { return name_; }
        const std::vector<std::string> &getParams() const { return params_; }
        const std::vector<std::unique_ptr<Statement>> &getBody() const { return body_; }
        bool isAsync() const { return isAsync_; }
        void accept(ASTVisitor &visitor) override;
    private:
        std::string name_;
        std::vector<std::string> params_;
        std::vector<std::unique_ptr<Statement>> body_;
        bool isAsync_;
    };

    class AwaitExpr : public Expression
    {
    public:
        AwaitExpr(std::unique_ptr<Expression> value, int line, int col)
            : Expression(AWAIT_EXPR, line, col), value_(std::move(value)) {}
        Expression *getValue() const { return value_.get(); }
        void accept(ASTVisitor &visitor) override;
    private:
        std::unique_ptr<Expression> value_;
    };

    class StructLiteralExpr : public Expression
    {
    public:
        using FieldInit = std::pair<std::string, std::unique_ptr<Expression>>;
        StructLiteralExpr(const std::string &structName, std::vector<FieldInit> fields, int line, int col)
            : Expression(STRUCT_LITERAL_EXPR, line, col), structName_(structName), fields_(std::move(fields)) {}
        const std::string &getStructName() const { return structName_; }
        const std::vector<FieldInit> &getFields() const { return fields_; }
        void accept(ASTVisitor &visitor) override;
    private:
        std::string structName_;
        std::vector<FieldInit> fields_;
    };

    class SizeofExpr : public Expression
    {
    public:
        SizeofExpr(std::unique_ptr<TypeAST> targetType, int line, int col)
            : Expression(SIZEOF_EXPR, line, col), targetType_(std::move(targetType)) {}
        TypeAST *getTargetType() const { return targetType_.get(); }
        void accept(ASTVisitor &visitor) override;
    private:
        std::unique_ptr<TypeAST> targetType_;
    };

    class CastExpr : public Expression
    {
    public:
        CastExpr(std::unique_ptr<Expression> expr, std::unique_ptr<TypeAST> targetType, int line, int col)
            : Expression(CAST_EXPR, line, col), expr_(std::move(expr)), targetType_(std::move(targetType)) {}
        Expression *getExpression() const { return expr_.get(); }
        TypeAST *getTargetType() const { return targetType_.get(); }
        void accept(ASTVisitor &visitor) override;
    private:
        std::unique_ptr<Expression> expr_;
        std::unique_ptr<TypeAST> targetType_;
    };

    class ArrayTypeAST : public TypeAST
    {
    public:
        ArrayTypeAST(std::unique_ptr<TypeAST> baseType, int size, int line, int col)
            : TypeAST(baseType->getName() + "[" + std::to_string(size) + "]", baseType->getPointerLevel(), line, col), 
              baseType_(std::move(baseType)), size_(size) {}
        TypeAST *getBaseType() const { return baseType_.get(); }
        int getSize() const { return size_; }
        void accept(ASTVisitor &visitor) override;
    private:
        std::unique_ptr<TypeAST> baseType_;
        int size_;
    };

    class VarDeclStmt : public Statement
    {
    public:
        VarDeclStmt(const std::string &name, std::unique_ptr<Expression> initializer, std::unique_ptr<TypeAST> type, int line, int col, bool isConst = false)
            : Statement(VAR_DECL_STMT, line, col), name_(name), initializer_(std::move(initializer)), type_(std::move(type)), isConst_(isConst) {}
        const std::string &getName() const { return name_; }
        Expression *getInitializer() const { return initializer_.get(); }
        void setInitializer(std::unique_ptr<Expression> initializer) { initializer_ = std::move(initializer); }
        std::unique_ptr<Expression> takeInitializer() { return std::move(initializer_); }
        TypeAST *getTypeAST() const { return type_.get(); }
        bool isConst() const { return isConst_; }
        void accept(ASTVisitor &visitor) override;
    private:
        std::string name_;
        std::unique_ptr<Expression> initializer_;
        std::unique_ptr<TypeAST> type_;
        bool isConst_;
    };

    class AssignStmt : public Statement
    {
    public:
        AssignStmt(std::unique_ptr<Expression> target, std::unique_ptr<Expression> value, int line, int col, std::string op = "")
            : Statement(ASSIGN_STMT, line, col), target_(std::move(target)), value_(std::move(value)), op_(std::move(op)) {}
        Expression *getTarget() const { return target_.get(); }
        Expression *getValue() const { return value_.get(); }
        const std::string& getOp() const { return op_; }
        void accept(ASTVisitor &visitor) override;
    private:
        std::unique_ptr<Expression> target_;
        std::unique_ptr<Expression> value_;
        std::string op_;
    };

    class IfStmt : public Statement
    {
    public:
        IfStmt(std::unique_ptr<Expression> condition, std::vector<std::unique_ptr<Statement>> thenBranch,
               std::vector<std::unique_ptr<Statement>> elseBranch, int line, int col)
            : Statement(IF_STMT, line, col), condition_(std::move(condition)),
              thenBranch_(std::move(thenBranch)), elseBranch_(std::move(elseBranch)) {}
        Expression *getCondition() const { return condition_.get(); }
        const std::vector<std::unique_ptr<Statement>> &getThenBranch() const { return thenBranch_; }
        const std::vector<std::unique_ptr<Statement>> &getElseBranch() const { return elseBranch_; }
        void accept(ASTVisitor &visitor) override;
    private:
        std::unique_ptr<Expression> condition_;
        std::vector<std::unique_ptr<Statement>> thenBranch_;
        std::vector<std::unique_ptr<Statement>> elseBranch_;
    };

    class WhileStmt : public Statement
    {
    public:
        WhileStmt(std::unique_ptr<Expression> condition, std::vector<std::unique_ptr<Statement>> body, int line, int col)
            : Statement(WHILE_STMT, line, col), condition_(std::move(condition)), body_(std::move(body)) {}
        Expression *getCondition() const { return condition_.get(); }
        const std::vector<std::unique_ptr<Statement>> &getBody() const { return body_; }
        void accept(ASTVisitor &visitor) override;
    private:
        std::unique_ptr<Expression> condition_;
        std::vector<std::unique_ptr<Statement>> body_;
    };

    class BlockStmt : public Statement
    {
    public:
        explicit BlockStmt(std::vector<std::unique_ptr<Statement>> statements, int line, int col)
            : Statement(BLOCK_STMT, line, col), statements_(std::move(statements)) {}
        const std::vector<std::unique_ptr<Statement>> &getStatements() const { return statements_; }
        void accept(ASTVisitor &visitor) override;
    private:
        std::vector<std::unique_ptr<Statement>> statements_;
    };

    class ForStmt : public Statement
    {
    public:
        ForStmt(std::unique_ptr<Statement> initializer, std::unique_ptr<Expression> condition,
                std::unique_ptr<Statement> increment, std::vector<std::unique_ptr<Statement>> body, int line, int col)
            : Statement(FOR_STMT, line, col), initializer_(std::move(initializer)),
              condition_(std::move(condition)), increment_(std::move(increment)),
              body_(std::move(body)) {}
        Statement *getInitializer() const { return initializer_.get(); }
        Expression *getCondition() const { return condition_.get(); }
        Statement *getIncrement() const { return increment_.get(); }
        const std::vector<std::unique_ptr<Statement>> &getBody() const { return body_; }
        void accept(ASTVisitor &visitor) override;
    private:
        std::unique_ptr<Statement> initializer_;
        std::unique_ptr<Expression> condition_;
        std::unique_ptr<Statement> increment_;
        std::vector<std::unique_ptr<Statement>> body_;
    };

    class ForEachStmt : public Statement
    {
    public:
        ForEachStmt(const std::string &varName, std::unique_ptr<Expression> iterable, std::vector<std::unique_ptr<Statement>> body, int line, int col)
            : Statement(FOR_EACH_STMT, line, col), varName_(varName), iterable_(std::move(iterable)), body_(std::move(body)) {}
        const std::string &getVarName() const { return varName_; }
        Expression *getIterable() const { return iterable_.get(); }
        const std::vector<std::unique_ptr<Statement>> &getBody() const { return body_; }
        void accept(ASTVisitor &visitor) override;
    private:
        std::string varName_;
        std::unique_ptr<Expression> iterable_;
        std::vector<std::unique_ptr<Statement>> body_;
    };

    class DeleteStmt : public Statement
    {
    public:
        DeleteStmt(std::unique_ptr<Expression> target, int line, int col)
            : Statement(DELETE_STMT, line, col), target_(std::move(target)) {}
        Expression *getTarget() const { return target_.get(); }
        void accept(ASTVisitor &visitor) override;
    private:
        std::unique_ptr<Expression> target_;
    };

    class BreakStmt : public Statement
    {
    public:
        explicit BreakStmt(int line, int col) : Statement(BREAK_STMT, line, col) {}
        void accept(ASTVisitor &visitor) override;
    };

    class ContinueStmt : public Statement
    {
    public:
        explicit ContinueStmt(int line, int col) : Statement(CONTINUE_STMT, line, col) {}
        void accept(ASTVisitor &visitor) override;
    };

    class ThrowStmt : public Statement
    {
    public:
        explicit ThrowStmt(std::unique_ptr<Expression> value, int line, int col)
            : Statement(THROW_STMT, line, col), value_(std::move(value)) {}
        Expression *getValue() const { return value_.get(); }
        void accept(ASTVisitor &visitor) override;
    private:
        std::unique_ptr<Expression> value_;
    };

    class TryCatchStmt : public Statement
    {
    public:
        TryCatchStmt(std::vector<std::unique_ptr<Statement>> tryBlock,
                     const std::string& catchVar,
                     std::vector<std::unique_ptr<Statement>> catchBlock,
                     int line, int col)
            : Statement(TRY_CATCH_STMT, line, col), tryBlock_(std::move(tryBlock)), catchVar_(catchVar), catchBlock_(std::move(catchBlock)) {}
        const std::vector<std::unique_ptr<Statement>> &getTryBlock() const { return tryBlock_; }
        const std::string &getCatchVar() const { return catchVar_; }
        const std::vector<std::unique_ptr<Statement>> &getCatchBlock() const { return catchBlock_; }
        void accept(ASTVisitor &visitor) override;
    private:
        std::vector<std::unique_ptr<Statement>> tryBlock_;
        std::string catchVar_;
        std::vector<std::unique_ptr<Statement>> catchBlock_;
    };

    class DeferStmt : public Statement
    {
    public:
        DeferStmt(std::unique_ptr<Statement> deferredStmt, int line, int col)
            : Statement(DEFER_STMT, line, col), deferredStmt_(std::move(deferredStmt)) {}
        Statement *getDeferredStmt() const { return deferredStmt_.get(); }
        void accept(ASTVisitor &visitor) override;
    private:
        std::unique_ptr<Statement> deferredStmt_;
    };

    class AsmStmt : public Statement
    {
    public:
        AsmStmt(const std::string &code, int line, int col)
            : Statement(ASM_STMT, line, col), code_(code) {}
        const std::string &getCode() const { return code_; }
        void accept(ASTVisitor &visitor) override;
        
        // Maps variable name used in {name} to actual identifier
        std::map<std::string, std::string> localVariables;
    private:
        std::string code_;
    };

    class MatchStmt : public Statement
    {
    public:
        struct Arm {
            std::unique_ptr<Expression> pattern;
            std::unique_ptr<Statement> body;
        };
        MatchStmt(std::unique_ptr<Expression> expression, std::vector<Arm> arms, int line, int col)
            : Statement(MATCH_STMT, line, col), expression_(std::move(expression)), arms_(std::move(arms)) {}
        Expression *getExpression() const { return expression_.get(); }
        const std::vector<Arm> &getArms() const { return arms_; }
        void accept(ASTVisitor &visitor) override;
    private:
        std::unique_ptr<Expression> expression_;
        std::vector<Arm> arms_;
    };

    class ReturnStmt : public Statement
    {
    public:
        explicit ReturnStmt(std::unique_ptr<Expression> value, int line, int col)
            : Statement(RETURN_STMT, line, col), value_(std::move(value)) {}
        Expression *getValue() const { return value_.get(); }
        void accept(ASTVisitor &visitor) override;
    private:
        std::unique_ptr<Expression> value_;
    };

    class ExpressionStmt : public Statement
    {
    public:
        explicit ExpressionStmt(std::unique_ptr<Expression> expr, int line, int col)
            : Statement(EXPRESSION_STMT, line, col), expr_(std::move(expr)) {}
        Expression *getExpression() const { return expr_.get(); }
        void accept(ASTVisitor &visitor) override;
    private:
        std::unique_ptr<Expression> expr_;
    };

    class FunctionDeclStmt : public Statement
    {
    public:
        using Parameter = std::pair<std::string, std::unique_ptr<TypeAST>>;
        FunctionDeclStmt(const std::string &name, std::vector<Parameter> params,
                         std::vector<std::unique_ptr<Statement>> body, std::unique_ptr<TypeAST> returnType, int line, int col, bool isAsync = false)
            : Statement(FUNCTION_DECL_STMT, line, col), name_(name), params_(std::move(params)), body_(std::move(body)), returnType_(std::move(returnType)), isAsync_(isAsync) {}
        const std::string &getName() const { return name_; }
        const std::vector<Parameter> &getParams() const { return params_; }
        const std::vector<std::unique_ptr<Statement>> &getBody() const { return body_; }
        TypeAST *getReturnType() const { return returnType_.get(); }
        bool isAsync() const { return isAsync_; }
        void accept(ASTVisitor &visitor) override;
    private:
        std::string name_;
        std::vector<Parameter> params_;
        std::vector<std::unique_ptr<Statement>> body_;
        std::unique_ptr<TypeAST> returnType_;
        bool isAsync_;
    };

    class StructDeclStmt : public Statement
    {
    public:
        using Field = std::pair<std::string, std::unique_ptr<TypeAST>>;
        StructDeclStmt(const std::string &name, std::vector<Field> fields, int line, int col)
            : Statement(STRUCT_DECL_STMT, line, col), name_(name), fields_(std::move(fields)) {}
        const std::string &getName() const { return name_; }
        const std::vector<Field> &getFields() const { return fields_; }
        void accept(ASTVisitor &visitor) override;
    private:
        std::string name_;
        std::vector<Field> fields_;
    };

    class EnumDeclStmt : public Statement
    {
    public:
        EnumDeclStmt(const std::string &name, std::vector<std::string> variants, int line, int col)
            : Statement(ENUM_DECL_STMT, line, col), name_(name), variants_(std::move(variants)) {}
        const std::string &getName() const { return name_; }
        const std::vector<std::string> &getVariants() const { return variants_; }
        void accept(ASTVisitor &visitor) override;
    private:
        std::string name_;
        std::vector<std::string> variants_;
    };

    class TypeAliasStmt : public Statement
    {
    public:
        TypeAliasStmt(const std::string &name, std::unique_ptr<TypeAST> targetType, int line, int col)
            : Statement(TYPE_ALIAS_STMT, line, col), name_(name), targetType_(std::move(targetType)) {}
        const std::string &getName() const { return name_; }
        TypeAST *getTargetType() const { return targetType_.get(); }
        void accept(ASTVisitor &visitor) override;
    private:
        std::string name_;
        std::unique_ptr<TypeAST> targetType_;
    };

    class ClassDeclStmt : public Statement
    {
    public:
        ClassDeclStmt(const std::string &name, const std::string &superclassName, std::vector<std::unique_ptr<Statement>> members, int line, int col)
            : Statement(CLASS_DECL_STMT, line, col), name_(name), superclassName_(superclassName), members_(std::move(members)) {}
        const std::string &getName() const { return name_; }
        const std::string &getParentName() const { return superclassName_; }
        const std::vector<std::unique_ptr<Statement>> &getMembers() const { return members_; }
        void accept(ASTVisitor &visitor) override;
    private:
        std::string name_;
        std::string superclassName_;
        std::vector<std::unique_ptr<Statement>> members_;
    };

    class ImportStmt : public Statement
    {
    public:
        explicit ImportStmt(const std::string &moduleName, int line, int col) : Statement(IMPORT_STMT, line, col), moduleName_(moduleName) {}
        const std::string &getModuleName() const { return moduleName_; }
        void accept(ASTVisitor &visitor) override;
    private:
        std::string moduleName_;
    };

    class FromImportStmt : public Statement
    {
    public:
        FromImportStmt(const std::string &moduleName, const std::vector<std::string> &symbols, int line, int col)
            : Statement(FROM_IMPORT_STMT, line, col), moduleName_(moduleName), symbols_(symbols) {}
        const std::string &getModuleName() const { return moduleName_; }
        const std::vector<std::string> &getSymbols() const { return symbols_; }
        void accept(ASTVisitor &visitor) override;
    private:
        std::string moduleName_;
        std::vector<std::string> symbols_;
    };

    class ExportStmt : public Statement
    {
    public:
        explicit ExportStmt(std::unique_ptr<Statement> stmt, int line, int col)
            : Statement(EXPORT_STMT, line, col), stmt_(std::move(stmt)) {}
        Statement *getStmt() const { return stmt_.get(); }
        void accept(ASTVisitor &visitor) override;
    private:
        std::unique_ptr<Statement> stmt_;
    };

    class ExportExpressionStmt : public Statement
    {
    public:
        explicit ExportExpressionStmt(std::unique_ptr<Expression> expr, int line, int col)
            : Statement(EXPORT_EXPRESSION_STMT, line, col), expr_(std::move(expr)) {}
        Expression *getExpression() const { return expr_.get(); }
        void accept(ASTVisitor &visitor) override;
    private:
        std::unique_ptr<Expression> expr_;
    };

    class Program : public ASTNode
    {
    public:
        Program() : ASTNode(PROGRAM, 1, 1) {}
        void addStatement(std::unique_ptr<Statement> stmt) { statements_.push_back(std::move(stmt)); }
        const std::vector<std::unique_ptr<Statement>> &getStatements() const { return statements_; }
        void accept(ASTVisitor &visitor) override;
        bool isStrict = false;
        
        void addImportedAst(std::unique_ptr<Program> ast) { importedAsts_.push_back(std::move(ast)); }
        const std::vector<std::unique_ptr<Program>>& getImportedAsts() const { return importedAsts_; }
    private:
        std::vector<std::unique_ptr<Statement>> statements_;
        std::vector<std::unique_ptr<Program>> importedAsts_;
    };

    class ASTVisitor
    {
    public:
        virtual ~ASTVisitor() = default;
        virtual void visit(NumberExpr &expr) = 0;
        virtual void visit(StringExpr &expr) = 0;
        virtual void visit(CharExpr &expr) = 0;
        virtual void visit(IdentifierExpr &expr) = 0;
        virtual void visit(BooleanExpr &expr) = 0;
        virtual void visit(NilExpr &expr) = 0;
        virtual void visit(NilPtrExpr &expr) = 0;
        virtual void visit(NewExpr &expr) = 0;
        virtual void visit(ThisExpr &expr) = 0;
        virtual void visit(BinaryExpr &expr) = 0;
        virtual void visit(UnaryExpr &expr) = 0;
        virtual void visit(CallExpr &expr) = 0;
        virtual void visit(MemberAccessExpr &expr) = 0;
        virtual void visit(ListExpr &expr) = 0;
        virtual void visit(TableExpr &expr) = 0;
        virtual void visit(IndexExpr &expr) = 0;
        virtual void visit(SliceExpr &expr) = 0;
        virtual void visit(TernaryExpr &expr) = 0;
        virtual void visit(GroupingExpr &expr) = 0;
        virtual void visit(FunctionExpr &expr) = 0;
        virtual void visit(SuperExpr &expr) = 0;
        virtual void visit(AwaitExpr &expr) = 0;
        virtual void visit(StructLiteralExpr &expr) = 0;
        virtual void visit(SizeofExpr &expr) = 0;
        virtual void visit(CastExpr &expr) = 0;
        virtual void visit(ArrayTypeAST &node) = 0;
        virtual void visit(TypeAST &node) = 0;
        virtual void visit(VarDeclStmt &stmt) = 0;
        virtual void visit(AssignStmt &stmt) = 0;
        virtual void visit(IfStmt &stmt) = 0;
        virtual void visit(WhileStmt &stmt) = 0;
        virtual void visit(ForStmt &stmt) = 0;
        virtual void visit(ForEachStmt &stmt) = 0;
        virtual void visit(DeleteStmt &stmt) = 0;
        virtual void visit(BreakStmt &stmt) = 0;
        virtual void visit(ContinueStmt &stmt) = 0;
        virtual void visit(ThrowStmt &stmt) = 0;
        virtual void visit(TryCatchStmt &stmt) = 0;
        virtual void visit(DeferStmt &stmt) = 0;
        virtual void visit(AsmStmt &stmt) = 0;
        virtual void visit(MatchStmt &stmt) = 0;
        virtual void visit(BlockStmt &stmt) = 0;
        virtual void visit(ReturnStmt &stmt) = 0;
        virtual void visit(ExpressionStmt &stmt) = 0;
        virtual void visit(FunctionDeclStmt &stmt) = 0;
        virtual void visit(StructDeclStmt &stmt) = 0;
        virtual void visit(EnumDeclStmt &stmt) = 0;
        virtual void visit(TypeAliasStmt &stmt) = 0;
        virtual void visit(ClassDeclStmt &stmt) = 0;
        virtual void visit(ImportStmt &stmt) = 0;
        virtual void visit(FromImportStmt &stmt) = 0;
        virtual void visit(ExportStmt &stmt) = 0;
        virtual void visit(ExportExpressionStmt &stmt) = 0;
        virtual void visit(Program &program) = 0;
    };

    inline void NumberExpr::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void StringExpr::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void CharExpr::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void BooleanExpr::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void NilExpr::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void NilPtrExpr::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void NewExpr::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void IdentifierExpr::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void ThisExpr::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void SuperExpr::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void BinaryExpr::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void UnaryExpr::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void CallExpr::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void MemberAccessExpr::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void ListExpr::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void TableExpr::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void IndexExpr::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void SliceExpr::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void TernaryExpr::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void GroupingExpr::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void FunctionExpr::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void AwaitExpr::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void StructLiteralExpr::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void SizeofExpr::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void CastExpr::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void ArrayTypeAST::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void TypeAST::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void VarDeclStmt::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void AssignStmt::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void IfStmt::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void WhileStmt::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void ForStmt::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void ForEachStmt::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void DeleteStmt::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void BlockStmt::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void BreakStmt::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void ContinueStmt::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void ThrowStmt::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void TryCatchStmt::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void DeferStmt::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void AsmStmt::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void MatchStmt::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void ReturnStmt::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void ExpressionStmt::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void FunctionDeclStmt::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void StructDeclStmt::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void EnumDeclStmt::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void TypeAliasStmt::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void ClassDeclStmt::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void ImportStmt::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void FromImportStmt::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void ExportStmt::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void ExportExpressionStmt::accept(ASTVisitor &visitor) { visitor.visit(*this); }
    inline void Program::accept(ASTVisitor &visitor) { visitor.visit(*this); }

}

#endif
