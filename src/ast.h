#pragma once

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cider {

class ASTVisitor;
class TypeAST;

// ---------------------------------------------------------------- nodes

class ASTNode {
public:
    enum NodeType {
        EXPRESSION, STATEMENT, PROGRAM,
        NUMBER_EXPR, STRING_EXPR, CHAR_EXPR, BOOLEAN_EXPR, NEW_EXPR, NIL_EXPR,
        NIL_PTR_EXPR, IDENTIFIER_EXPR, BINARY_EXPR, UNARY_EXPR, CALL_EXPR,
        MEMBER_ACCESS_EXPR, LIST_EXPR, TABLE_EXPR, INDEX_EXPR, SLICE_EXPR,
        TERNARY_EXPR, GROUPING_EXPR, THIS_EXPR, SUPER_EXPR, FUNCTION_EXPR,
        AWAIT_EXPR, STRUCT_LITERAL_EXPR, SIZEOF_EXPR, CAST_EXPR, ARRAY_TYPE,
        VAR_DECL_STMT, ASSIGN_STMT, IF_STMT, WHILE_STMT, FOR_STMT,
        FOR_EACH_STMT, DELETE_STMT, BREAK_STMT, CONTINUE_STMT, THROW_STMT,
        TRY_CATCH_STMT, DEFER_STMT, ASM_STMT, MATCH_STMT, BLOCK_STMT,
        RETURN_STMT, EXPRESSION_STMT, FUNCTION_DECL_STMT, CLASS_DECL_STMT,
        STRUCT_DECL_STMT, ENUM_DECL_STMT, TYPE_ALIAS_STMT, IMPORT_STMT,
        FROM_IMPORT_STMT, EXPORT_STMT, EXPORT_EXPRESSION_STMT,
        INTERFACE_DECL_STMT, TYPE_AST
    };

    ASTNode(NodeType type, int line, int col) : type_(type), line_(line), col_(col) {}
    virtual ~ASTNode() = default;

    NodeType getType() const { return type_; }
    int getLine() const { return line_; }
    int getColumn() const { return col_; }

    virtual void dump(std::string& out, int depth) const = 0;
    virtual void accept(ASTVisitor& visitor);

private:
    NodeType type_;
    int line_;
    int col_;
};

namespace detail {
inline void indent(std::string& out, int depth) { out.append(static_cast<size_t>(depth) * 2, ' '); }
inline void dumpNumber(std::string& out, double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%g", v);
    out += buf;
}
inline void dumpString(std::string& out, const std::string& s) {
    out += '"';
    for (char c : s) {
        if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else out += c;
    }
    out += '"';
}
inline void dumpChar(std::string& out, char c) {
    out += '\'';
    if (c == '\n') out += "\\n";
    else if (c == '\t') out += "\\t";
    else if (c == '\'') out += "\\'";
    else if (c == '\\') out += "\\\\";
    else out += c;
    out += '\'';
}
}  // namespace detail

class Expression : public ASTNode {
public:
    Expression(NodeType type, int line, int col) : ASTNode(type, line, col) {}
};

class Statement : public ASTNode {
public:
    Statement(NodeType type, int line, int col) : ASTNode(type, line, col) {}
};

// ---------------------------------------------------------------- types

class TypeAST : public ASTNode {
public:
    TypeAST(const std::string& name, int pointerLevel, int line, int col, bool isVolatile = false)
        : ASTNode(TYPE_AST, line, col), name_(name), pointerLevel_(pointerLevel), isVolatile_(isVolatile) {}

    const std::string& getName() const { return name_; }
    void setName(const std::string& name) { name_ = name; }
    int getPointerLevel() const { return pointerLevel_; }
    bool isVolatile() const { return isVolatile_; }

    void dump(std::string& out, int /*depth*/) const override {
        out += name_;
        out.append(static_cast<size_t>(pointerLevel_), '*');
    }

private:
    std::string name_;
    int pointerLevel_;
    bool isVolatile_;
};

class ArrayTypeAST : public TypeAST {
public:
    ArrayTypeAST(std::unique_ptr<TypeAST> baseType, int size, int line, int col)
        : TypeAST(baseType->getName() + "[" + std::to_string(size) + "]", baseType->getPointerLevel(), line, col),
          baseType_(std::move(baseType)), size_(size) {}

    TypeAST* getBaseType() const { return baseType_.get(); }
    int getSize() const { return size_; }

    void dump(std::string& out, int /*depth*/) const override {
        baseType_->dump(out, 0);
        out += '[' + std::to_string(size_) + ']';
    }

private:
    std::unique_ptr<TypeAST> baseType_;
    int size_;
};

// ---------------------------------------------------------------- expressions

class NumberExpr : public Expression {
public:
    explicit NumberExpr(double value, int line, int col) : Expression(NUMBER_EXPR, line, col), value_(value) {}
    double getValue() const { return value_; }
    void dump(std::string& out, int /*depth*/) const override {
        out += "(Number ";
        detail::dumpNumber(out, value_);
        out += ")";
    }
private:
    double value_;
};

class StringExpr : public Expression {
public:
    explicit StringExpr(const std::string& value, int line, int col) : Expression(STRING_EXPR, line, col), value_(value) {}
    const std::string& getValue() const { return value_; }
    void dump(std::string& out, int /*depth*/) const override {
        out += "(String ";
        detail::dumpString(out, value_);
        out += ")";
    }
private:
    std::string value_;
};

class CharExpr : public Expression {
public:
    explicit CharExpr(char value, int line, int col) : Expression(CHAR_EXPR, line, col), value_(value) {}
    char getValue() const { return value_; }
    void dump(std::string& out, int /*depth*/) const override {
        out += "(Char ";
        detail::dumpChar(out, value_);
        out += ")";
    }
private:
    char value_;
};

class BooleanExpr : public Expression {
public:
    explicit BooleanExpr(bool value, int line, int col) : Expression(BOOLEAN_EXPR, line, col), value_(value) {}
    bool getValue() const { return value_; }
    void dump(std::string& out, int /*depth*/) const override {
        out += value_ ? "(Bool true)" : "(Bool false)";
    }
private:
    bool value_;
};

class NilExpr : public Expression {
public:
    explicit NilExpr(int line, int col) : Expression(NIL_EXPR, line, col) {}
    void dump(std::string& out, int /*depth*/) const override { out += "(Nil)"; }
};

class NilPtrExpr : public Expression {
public:
    NilPtrExpr(int line, int col) : Expression(NIL_PTR_EXPR, line, col) {}
    void dump(std::string& out, int /*depth*/) const override { out += "(NilPtr)"; }
};

class NewExpr : public Expression {
public:
    NewExpr(std::unique_ptr<TypeAST> type, int line, int col)
        : Expression(NEW_EXPR, line, col), type_(std::move(type)) {}
    TypeAST* getNewType() const { return type_.get(); }
    void dump(std::string& out, int depth) const override {
        out += "(New ";
        type_->dump(out, depth);
        out += ")";
    }
private:
    std::unique_ptr<TypeAST> type_;
};

class IdentifierExpr : public Expression {
public:
    explicit IdentifierExpr(const std::string& name, int line, int col) : Expression(IDENTIFIER_EXPR, line, col), name_(name) {}
    const std::string& getName() const { return name_; }
    void dump(std::string& out, int /*depth*/) const override {
        out += "(Ident " + name_ + ")";
    }
private:
    std::string name_;
};

class SuperExpr : public Expression {
public:
    SuperExpr(const std::string& member, int line, int col) : Expression(SUPER_EXPR, line, col), member_(member) {}
    const std::string& getMember() const { return member_; }
    void dump(std::string& out, int /*depth*/) const override {
        out += "(Super " + member_ + ")";
    }
private:
    std::string member_;
};

class ThisExpr : public Expression {
public:
    ThisExpr(int line, int col) : Expression(THIS_EXPR, line, col) {}
    void dump(std::string& out, int /*depth*/) const override { out += "(This)"; }
};

class BinaryExpr : public Expression {
public:
    BinaryExpr(std::unique_ptr<Expression> left, const std::string& op, std::unique_ptr<Expression> right, int line, int col)
        : Expression(BINARY_EXPR, line, col), left_(std::move(left)), op_(op), right_(std::move(right)) {}
    Expression* getLeft() const { return left_.get(); }
    const std::string& getOperator() const { return op_; }
    Expression* getRight() const { return right_.get(); }
    void setLeft(std::unique_ptr<Expression> left) { left_ = std::move(left); }
    void setRight(std::unique_ptr<Expression> right) { right_ = std::move(right); }
    std::unique_ptr<Expression> takeLeft() { return std::move(left_); }
    std::unique_ptr<Expression> takeRight() { return std::move(right_); }
    void dump(std::string& out, int depth) const override {
        out += "(Binary " + op_ + " ";
        left_->dump(out, depth);
        out += " ";
        right_->dump(out, depth);
        out += ")";
    }
private:
    std::unique_ptr<Expression> left_;
    std::string op_;
    std::unique_ptr<Expression> right_;
};

class UnaryExpr : public Expression {
public:
    UnaryExpr(const std::string& op, std::unique_ptr<Expression> operand, int line, int col)
        : Expression(UNARY_EXPR, line, col), op_(op), operand_(std::move(operand)) {}
    const std::string& getOperator() const { return op_; }
    Expression* getOperand() const { return operand_.get(); }
    std::unique_ptr<Expression> takeOperand() { return std::move(operand_); }
    void setOperand(std::unique_ptr<Expression> operand) { operand_ = std::move(operand); }
    void dump(std::string& out, int depth) const override {
        out += "(Unary " + op_ + " ";
        operand_->dump(out, depth);
        out += ")";
    }
private:
    std::string op_;
    std::unique_ptr<Expression> operand_;
};

class CallExpr : public Expression {
public:
    CallExpr(std::unique_ptr<Expression> callee, std::vector<std::unique_ptr<Expression>> args, int line, int col)
        : Expression(CALL_EXPR, line, col), callee_(std::move(callee)), args_(std::move(args)) {}
    Expression* getCallee() const { return callee_.get(); }
    const std::vector<std::unique_ptr<Expression>>& getArgs() const { return args_; }
    void dump(std::string& out, int depth) const override {
        out += "(Call ";
        callee_->dump(out, depth);
        for (const auto& a : args_) {
            out += " ";
            a->dump(out, depth);
        }
        out += ")";
    }
private:
    std::unique_ptr<Expression> callee_;
    std::vector<std::unique_ptr<Expression>> args_;
};

class MemberAccessExpr : public Expression {
public:
    MemberAccessExpr(std::unique_ptr<Expression> object, const std::string& member, int line, int col, bool isArrow = false)
        : Expression(MEMBER_ACCESS_EXPR, line, col), object_(std::move(object)), member_(member), isArrow_(isArrow) {}
    Expression* getObject() const { return object_.get(); }
    const std::string& getMember() const { return member_; }
    bool isArrow() const { return isArrow_; }
    void dump(std::string& out, int depth) const override {
        out += isArrow_ ? "(Arrow " : "(Member ";
        object_->dump(out, depth);
        out += " " + member_ + ")";
    }
private:
    std::unique_ptr<Expression> object_;
    std::string member_;
    bool isArrow_;
};

class ListExpr : public Expression {
public:
    explicit ListExpr(std::vector<std::unique_ptr<Expression>> elements, int line, int col)
        : Expression(LIST_EXPR, line, col), elements_(std::move(elements)) {}
    const std::vector<std::unique_ptr<Expression>>& getElements() const { return elements_; }
    void dump(std::string& out, int depth) const override {
        out += "(List";
        for (const auto& e : elements_) {
            out += " ";
            e->dump(out, depth);
        }
        out += ")";
    }
private:
    std::vector<std::unique_ptr<Expression>> elements_;
};

class TableExpr : public Expression {
public:
    using Entry = std::pair<std::unique_ptr<Expression>, std::unique_ptr<Expression>>;
    explicit TableExpr(std::vector<Entry> entries, int line, int col)
        : Expression(TABLE_EXPR, line, col), entries_(std::move(entries)) {}
    const std::vector<Entry>& getEntries() const { return entries_; }
    void dump(std::string& out, int depth) const override {
        out += "(Table";
        for (const auto& e : entries_) {
            out += " ";
            e.first->dump(out, depth);
            out += "=";
            e.second->dump(out, depth);
        }
        out += ")";
    }
private:
    std::vector<Entry> entries_;
};

class IndexExpr : public Expression {
public:
    IndexExpr(std::unique_ptr<Expression> object, std::unique_ptr<Expression> index, int line, int col)
        : Expression(INDEX_EXPR, line, col), object_(std::move(object)), index_(std::move(index)) {}
    Expression* getObject() const { return object_.get(); }
    Expression* getIndex() const { return index_.get(); }
    void dump(std::string& out, int depth) const override {
        out += "(Index ";
        object_->dump(out, depth);
        out += " ";
        index_->dump(out, depth);
        out += ")";
    }
private:
    std::unique_ptr<Expression> object_;
    std::unique_ptr<Expression> index_;
};

class SliceExpr : public Expression {
public:
    SliceExpr(std::unique_ptr<Expression> object, std::unique_ptr<Expression> start, std::unique_ptr<Expression> end, int line, int col)
        : Expression(SLICE_EXPR, line, col), object_(std::move(object)), start_(std::move(start)), end_(std::move(end)) {}
    Expression* getObject() const { return object_.get(); }
    Expression* getStart() const { return start_.get(); }
    Expression* getEnd() const { return end_.get(); }
    void dump(std::string& out, int depth) const override {
        out += "(Slice ";
        object_->dump(out, depth);
        if (start_) {
            out += " ";
            start_->dump(out, depth);
        }
        if (end_) {
            out += " ";
            end_->dump(out, depth);
        }
        out += ")";
    }
private:
    std::unique_ptr<Expression> object_;
    std::unique_ptr<Expression> start_;
    std::unique_ptr<Expression> end_;
};

class TernaryExpr : public Expression {
public:
    TernaryExpr(std::unique_ptr<Expression> condition, std::unique_ptr<Expression> thenExpr, std::unique_ptr<Expression> elseExpr, int line, int col)
        : Expression(TERNARY_EXPR, line, col), condition_(std::move(condition)), thenExpr_(std::move(thenExpr)), elseExpr_(std::move(elseExpr)) {}
    Expression* getCondition() const { return condition_.get(); }
    Expression* getThenExpr() const { return thenExpr_.get(); }
    Expression* getElseExpr() const { return elseExpr_.get(); }
    std::unique_ptr<Expression> takeCondition() { return std::move(condition_); }
    std::unique_ptr<Expression> takeThenExpr() { return std::move(thenExpr_); }
    std::unique_ptr<Expression> takeElseExpr() { return std::move(elseExpr_); }
    void setCondition(std::unique_ptr<Expression> expr) { condition_ = std::move(expr); }
    void dump(std::string& out, int depth) const override {
        out += "(Ternary ";
        condition_->dump(out, depth);
        out += " ";
        thenExpr_->dump(out, depth);
        out += " ";
        elseExpr_->dump(out, depth);
        out += ")";
    }
private:
    std::unique_ptr<Expression> condition_;
    std::unique_ptr<Expression> thenExpr_;
    std::unique_ptr<Expression> elseExpr_;
};

class GroupingExpr : public Expression {
public:
    GroupingExpr(std::unique_ptr<Expression> expression, int line, int col)
        : Expression(GROUPING_EXPR, line, col), expression_(std::move(expression)) {}
    Expression* getExpression() const { return expression_.get(); }
    std::unique_ptr<Expression> takeExpression() { return std::move(expression_); }
    void dump(std::string& out, int depth) const override {
        out += "(Group ";
        expression_->dump(out, depth);
        out += ")";
    }
private:
    std::unique_ptr<Expression> expression_;
};

class FunctionExpr : public Expression {
public:
    FunctionExpr(const std::string& name, std::vector<std::string> params,
                 std::vector<std::unique_ptr<Statement>> body, int line, int col, bool isAsync = false)
        : Expression(FUNCTION_EXPR, line, col), name_(name), params_(std::move(params)), body_(std::move(body)), isAsync_(isAsync) {}
    const std::string& getName() const { return name_; }
    const std::vector<std::string>& getParams() const { return params_; }
    const std::vector<std::unique_ptr<Statement>>& getBody() const { return body_; }
    bool isAsync() const { return isAsync_; }
    void dump(std::string& out, int depth) const override;
private:
    std::string name_;
    std::vector<std::string> params_;
    std::vector<std::unique_ptr<Statement>> body_;
    bool isAsync_;
};

class AwaitExpr : public Expression {
public:
    AwaitExpr(std::unique_ptr<Expression> value, int line, int col)
        : Expression(AWAIT_EXPR, line, col), value_(std::move(value)) {}
    Expression* getValue() const { return value_.get(); }
    void dump(std::string& out, int depth) const override {
        out += "(Await ";
        value_->dump(out, depth);
        out += ")";
    }
private:
    std::unique_ptr<Expression> value_;
};

class StructLiteralExpr : public Expression {
public:
    using FieldInit = std::pair<std::string, std::unique_ptr<Expression>>;
    StructLiteralExpr(const std::string& structName, std::vector<FieldInit> fields, int line, int col)
        : Expression(STRUCT_LITERAL_EXPR, line, col), structName_(structName), fields_(std::move(fields)) {}
    const std::string& getStructName() const { return structName_; }
    const std::vector<FieldInit>& getFields() const { return fields_; }
    void dump(std::string& out, int depth) const override {
        out += "(Struct " + structName_;
        for (const auto& f : fields_) {
            out += " (" + f.first + " ";
            f.second->dump(out, depth);
            out += ")";
        }
        out += ")";
    }
private:
    std::string structName_;
    std::vector<FieldInit> fields_;
};

class SizeofExpr : public Expression {
public:
    SizeofExpr(std::unique_ptr<TypeAST> targetType, int line, int col)
        : Expression(SIZEOF_EXPR, line, col), targetType_(std::move(targetType)) {}
    TypeAST* getTargetType() const { return targetType_.get(); }
    void dump(std::string& out, int depth) const override {
        out += "(Sizeof ";
        targetType_->dump(out, depth);
        out += ")";
    }
private:
    std::unique_ptr<TypeAST> targetType_;
};

class CastExpr : public Expression {
public:
    CastExpr(std::unique_ptr<Expression> expr, std::unique_ptr<TypeAST> targetType, int line, int col)
        : Expression(CAST_EXPR, line, col), expr_(std::move(expr)), targetType_(std::move(targetType)) {}
    Expression* getExpression() const { return expr_.get(); }
    TypeAST* getTargetType() const { return targetType_.get(); }
    void dump(std::string& out, int depth) const override {
        out += "(Cast ";
        expr_->dump(out, depth);
        out += " -> ";
        targetType_->dump(out, depth);
        out += ")";
    }
private:
    std::unique_ptr<Expression> expr_;
    std::unique_ptr<TypeAST> targetType_;
};

// ---------------------------------------------------------------- statements

class VarDeclStmt : public Statement {
public:
    VarDeclStmt(const std::string& name, std::unique_ptr<Expression> initializer, std::unique_ptr<TypeAST> type, int line, int col, bool isConst = false)
        : Statement(VAR_DECL_STMT, line, col), name_(name), initializer_(std::move(initializer)), type_(std::move(type)), isConst_(isConst) {}
    const std::string& getName() const { return name_; }
    Expression* getInitializer() const { return initializer_.get(); }
    void setInitializer(std::unique_ptr<Expression> initializer) { initializer_ = std::move(initializer); }
    std::unique_ptr<Expression> takeInitializer() { return std::move(initializer_); }
    TypeAST* getTypeAST() const { return type_.get(); }
    bool isConst() const { return isConst_; }
    void dump(std::string& out, int depth) const override;
private:
    std::string name_;
    std::unique_ptr<Expression> initializer_;
    std::unique_ptr<TypeAST> type_;
    bool isConst_;
};

class AssignStmt : public Statement {
public:
    AssignStmt(std::unique_ptr<Expression> target, std::unique_ptr<Expression> value, int line, int col, std::string op = "")
        : Statement(ASSIGN_STMT, line, col), target_(std::move(target)), value_(std::move(value)), op_(std::move(op)) {}
    Expression* getTarget() const { return target_.get(); }
    Expression* getValue() const { return value_.get(); }
    const std::string& getOp() const { return op_; }
    void dump(std::string& out, int depth) const override;
private:
    std::unique_ptr<Expression> target_;
    std::unique_ptr<Expression> value_;
    std::string op_;
};

class IfStmt : public Statement {
public:
    IfStmt(std::unique_ptr<Expression> condition, std::vector<std::unique_ptr<Statement>> thenBranch,
           std::vector<std::unique_ptr<Statement>> elseBranch, int line, int col)
        : Statement(IF_STMT, line, col), condition_(std::move(condition)),
          thenBranch_(std::move(thenBranch)), elseBranch_(std::move(elseBranch)) {}
    Expression* getCondition() const { return condition_.get(); }
    const std::vector<std::unique_ptr<Statement>>& getThenBranch() const { return thenBranch_; }
    const std::vector<std::unique_ptr<Statement>>& getElseBranch() const { return elseBranch_; }
    void dump(std::string& out, int depth) const override;
private:
    std::unique_ptr<Expression> condition_;
    std::vector<std::unique_ptr<Statement>> thenBranch_;
    std::vector<std::unique_ptr<Statement>> elseBranch_;
};

class WhileStmt : public Statement {
public:
    WhileStmt(std::unique_ptr<Expression> condition, std::vector<std::unique_ptr<Statement>> body, int line, int col)
        : Statement(WHILE_STMT, line, col), condition_(std::move(condition)), body_(std::move(body)) {}
    Expression* getCondition() const { return condition_.get(); }
    const std::vector<std::unique_ptr<Statement>>& getBody() const { return body_; }
    void dump(std::string& out, int depth) const override;
private:
    std::unique_ptr<Expression> condition_;
    std::vector<std::unique_ptr<Statement>> body_;
};

class BlockStmt : public Statement {
public:
    explicit BlockStmt(std::vector<std::unique_ptr<Statement>> statements, int line, int col)
        : Statement(BLOCK_STMT, line, col), statements_(std::move(statements)) {}
    const std::vector<std::unique_ptr<Statement>>& getStatements() const { return statements_; }
    void dump(std::string& out, int depth) const override;
private:
    std::vector<std::unique_ptr<Statement>> statements_;
};

class ForStmt : public Statement {
public:
    ForStmt(std::unique_ptr<Statement> initializer, std::unique_ptr<Expression> condition,
            std::unique_ptr<Statement> increment, std::vector<std::unique_ptr<Statement>> body, int line, int col)
        : Statement(FOR_STMT, line, col), initializer_(std::move(initializer)),
          condition_(std::move(condition)), increment_(std::move(increment)), body_(std::move(body)) {}
    Statement* getInitializer() const { return initializer_.get(); }
    Expression* getCondition() const { return condition_.get(); }
    Statement* getIncrement() const { return increment_.get(); }
    const std::vector<std::unique_ptr<Statement>>& getBody() const { return body_; }
    void dump(std::string& out, int depth) const override;
private:
    std::unique_ptr<Statement> initializer_;
    std::unique_ptr<Expression> condition_;
    std::unique_ptr<Statement> increment_;
    std::vector<std::unique_ptr<Statement>> body_;
};

class ForEachStmt : public Statement {
public:
    ForEachStmt(const std::string& varName, std::unique_ptr<Expression> iterable, std::vector<std::unique_ptr<Statement>> body, int line, int col)
        : Statement(FOR_EACH_STMT, line, col), varName_(varName), iterable_(std::move(iterable)), body_(std::move(body)) {}
    const std::string& getVarName() const { return varName_; }
    Expression* getIterable() const { return iterable_.get(); }
    const std::vector<std::unique_ptr<Statement>>& getBody() const { return body_; }
    void dump(std::string& out, int depth) const override;
private:
    std::string varName_;
    std::unique_ptr<Expression> iterable_;
    std::vector<std::unique_ptr<Statement>> body_;
};

class DeleteStmt : public Statement {
public:
    DeleteStmt(std::unique_ptr<Expression> target, int line, int col)
        : Statement(DELETE_STMT, line, col), target_(std::move(target)) {}
    Expression* getTarget() const { return target_.get(); }
    void dump(std::string& out, int depth) const override;
private:
    std::unique_ptr<Expression> target_;
};

class BreakStmt : public Statement {
public:
    explicit BreakStmt(int line, int col) : Statement(BREAK_STMT, line, col) {}
    void dump(std::string& out, int /*depth*/) const override { out += "(Break)"; }
};

class ContinueStmt : public Statement {
public:
    explicit ContinueStmt(int line, int col) : Statement(CONTINUE_STMT, line, col) {}
    void dump(std::string& out, int /*depth*/) const override { out += "(Continue)"; }
};

class ThrowStmt : public Statement {
public:
    explicit ThrowStmt(std::unique_ptr<Expression> value, int line, int col)
        : Statement(THROW_STMT, line, col), value_(std::move(value)) {}
    Expression* getValue() const { return value_.get(); }
    void dump(std::string& out, int depth) const override;
private:
    std::unique_ptr<Expression> value_;
};

class TryCatchStmt : public Statement {
public:
    TryCatchStmt(std::vector<std::unique_ptr<Statement>> tryBlock,
                 const std::string& catchVar,
                 std::vector<std::unique_ptr<Statement>> catchBlock,
                 int line, int col)
        : Statement(TRY_CATCH_STMT, line, col), tryBlock_(std::move(tryBlock)), catchVar_(catchVar), catchBlock_(std::move(catchBlock)) {}
    const std::vector<std::unique_ptr<Statement>>& getTryBlock() const { return tryBlock_; }
    const std::string& getCatchVar() const { return catchVar_; }
    const std::vector<std::unique_ptr<Statement>>& getCatchBlock() const { return catchBlock_; }
    void dump(std::string& out, int depth) const override;
private:
    std::vector<std::unique_ptr<Statement>> tryBlock_;
    std::string catchVar_;
    std::vector<std::unique_ptr<Statement>> catchBlock_;
};

class DeferStmt : public Statement {
public:
    DeferStmt(std::unique_ptr<Statement> deferredStmt, int line, int col)
        : Statement(DEFER_STMT, line, col), deferredStmt_(std::move(deferredStmt)) {}
    Statement* getDeferredStmt() const { return deferredStmt_.get(); }
    void dump(std::string& out, int depth) const override;
private:
    std::unique_ptr<Statement> deferredStmt_;
};

class AsmStmt : public Statement {
public:
    AsmStmt(const std::string& code, int line, int col)
        : Statement(ASM_STMT, line, col), code_(code) {}
    const std::string& getCode() const { return code_; }
    std::map<std::string, std::string> localVariables;
    void dump(std::string& out, int /*depth*/) const override {
        out += "(Asm ";
        detail::dumpString(out, code_);
        out += ")";
    }
private:
    std::string code_;
};

class MatchStmt : public Statement {
public:
    struct Arm {
        std::unique_ptr<Expression> pattern;
        std::unique_ptr<Statement> body;
    };
    MatchStmt(std::unique_ptr<Expression> expression, std::vector<Arm> arms, int line, int col)
        : Statement(MATCH_STMT, line, col), expression_(std::move(expression)), arms_(std::move(arms)) {}
    Expression* getExpression() const { return expression_.get(); }
    const std::vector<Arm>& getArms() const { return arms_; }
    void dump(std::string& out, int depth) const override;
private:
    std::unique_ptr<Expression> expression_;
    std::vector<Arm> arms_;
};

class ReturnStmt : public Statement {
public:
    explicit ReturnStmt(std::unique_ptr<Expression> value, int line, int col)
        : Statement(RETURN_STMT, line, col), value_(std::move(value)) {}
    Expression* getValue() const { return value_.get(); }
    void dump(std::string& out, int depth) const override;
private:
    std::unique_ptr<Expression> value_;
};

class ExpressionStmt : public Statement {
public:
    explicit ExpressionStmt(std::unique_ptr<Expression> expr, int line, int col)
        : Statement(EXPRESSION_STMT, line, col), expr_(std::move(expr)) {}
    Expression* getExpression() const { return expr_.get(); }
    std::unique_ptr<Expression> takeExpression() { return std::move(expr_); }
    void setExpression(std::unique_ptr<Expression> expr) { expr_ = std::move(expr); }
    void dump(std::string& out, int depth) const override;
private:
    std::unique_ptr<Expression> expr_;
};

class FunctionDeclStmt : public Statement {
public:
    using Parameter = std::pair<std::string, std::unique_ptr<TypeAST>>;
    FunctionDeclStmt(const std::string& name, std::vector<Parameter> params,
                     std::vector<std::unique_ptr<Statement>> body, std::unique_ptr<TypeAST> returnType, int line, int col,
                     bool isAsync = false, bool isVirtual = false, bool isOverride = false, bool isExtern = false,
                     bool isDestructor = false, std::vector<std::string> templateParams = {})
        : Statement(FUNCTION_DECL_STMT, line, col), name_(name), params_(std::move(params)), body_(std::move(body)),
          returnType_(std::move(returnType)), isAsync_(isAsync), isVirtual_(isVirtual), isOverride_(isOverride),
          isExtern_(isExtern), isDestructor_(isDestructor), templateParams_(std::move(templateParams)) {}
    const std::string& getName() const { return name_; }
    const std::vector<Parameter>& getParams() const { return params_; }
    const std::vector<std::unique_ptr<Statement>>& getBody() const { return body_; }
    TypeAST* getReturnType() const { return returnType_.get(); }
    bool isAsync() const { return isAsync_; }
    bool isVirtual() const { return isVirtual_; }
    bool isOverride() const { return isOverride_; }
    bool isExtern() const { return isExtern_; }
    bool isDestructor() const { return isDestructor_; }
    const std::vector<std::string>& getTemplateParams() const { return templateParams_; }
    bool hasTemplateParams() const { return !templateParams_.empty(); }
    void dump(std::string& out, int depth) const override;
private:
    std::string name_;
    std::vector<Parameter> params_;
    std::vector<std::unique_ptr<Statement>> body_;
    std::unique_ptr<TypeAST> returnType_;
    bool isAsync_;
    bool isVirtual_;
    bool isOverride_;
    bool isExtern_;
    bool isDestructor_;
    std::vector<std::string> templateParams_;
};

class StructDeclStmt : public Statement {
public:
    using Field = std::pair<std::string, std::unique_ptr<TypeAST>>;
    StructDeclStmt(const std::string& name, std::vector<Field> fields, int line, int col,
                   bool isUnion = false, std::vector<std::string> templateParams = {})
        : Statement(STRUCT_DECL_STMT, line, col), name_(name), fields_(std::move(fields)), isUnion_(isUnion), templateParams_(std::move(templateParams)) {}
    const std::string& getName() const { return name_; }
    const std::vector<Field>& getFields() const { return fields_; }
    bool isUnion() const { return isUnion_; }
    const std::vector<std::string>& getTemplateParams() const { return templateParams_; }
    bool hasTemplateParams() const { return !templateParams_.empty(); }
    void dump(std::string& out, int depth) const override;
private:
    std::string name_;
    std::vector<Field> fields_;
    bool isUnion_;
    std::vector<std::string> templateParams_;
};

class EnumDeclStmt : public Statement {
public:
    EnumDeclStmt(const std::string& name, std::vector<std::string> variants, int line, int col)
        : Statement(ENUM_DECL_STMT, line, col), name_(name), variants_(std::move(variants)) {}
    const std::string& getName() const { return name_; }
    const std::vector<std::string>& getVariants() const { return variants_; }
    void dump(std::string& out, int depth) const override;
private:
    std::string name_;
    std::vector<std::string> variants_;
};

class TypeAliasStmt : public Statement {
public:
    TypeAliasStmt(const std::string& name, std::unique_ptr<TypeAST> targetType, int line, int col)
        : Statement(TYPE_ALIAS_STMT, line, col), name_(name), targetType_(std::move(targetType)) {}
    const std::string& getName() const { return name_; }
    TypeAST* getTargetType() const { return targetType_.get(); }
    void dump(std::string& out, int depth) const override;
private:
    std::string name_;
    std::unique_ptr<TypeAST> targetType_;
};

class ClassDeclStmt : public Statement {
public:
    ClassDeclStmt(const std::string& name, const std::string& superclassName, std::vector<std::unique_ptr<Statement>> members, int line, int col)
        : Statement(CLASS_DECL_STMT, line, col), name_(name), superclassName_(superclassName), members_(std::move(members)) {}
    const std::string& getName() const { return name_; }
    const std::string& getParentName() const { return superclassName_; }
    const std::vector<std::unique_ptr<Statement>>& getMembers() const { return members_; }
    void dump(std::string& out, int depth) const override;
private:
    std::string name_;
    std::string superclassName_;
    std::vector<std::unique_ptr<Statement>> members_;
};

class ImportStmt : public Statement {
public:
    explicit ImportStmt(const std::string& moduleName, int line, int col) : Statement(IMPORT_STMT, line, col), moduleName_(moduleName) {}
    const std::string& getModuleName() const { return moduleName_; }
    void dump(std::string& out, int /*depth*/) const override {
        out += "(Import ";
        detail::dumpString(out, moduleName_);
        out += ")";
    }
private:
    std::string moduleName_;
};

class FromImportStmt : public Statement {
public:
    FromImportStmt(const std::string& moduleName, const std::vector<std::string>& symbols, int line, int col)
        : Statement(FROM_IMPORT_STMT, line, col), moduleName_(moduleName), symbols_(symbols) {}
    const std::string& getModuleName() const { return moduleName_; }
    const std::vector<std::string>& getSymbols() const { return symbols_; }
    void dump(std::string& out, int depth) const override;
private:
    std::string moduleName_;
    std::vector<std::string> symbols_;
};

class ExportStmt : public Statement {
public:
    explicit ExportStmt(std::unique_ptr<Statement> stmt, int line, int col)
        : Statement(EXPORT_STMT, line, col), stmt_(std::move(stmt)) {}
    Statement* getStmt() const { return stmt_.get(); }
    void dump(std::string& out, int depth) const override;
private:
    std::unique_ptr<Statement> stmt_;
};

class ExportExpressionStmt : public Statement {
public:
    explicit ExportExpressionStmt(std::unique_ptr<Expression> expr, int line, int col)
        : Statement(EXPORT_EXPRESSION_STMT, line, col), expr_(std::move(expr)) {}
    Expression* getExpression() const { return expr_.get(); }
    void dump(std::string& out, int depth) const override;
private:
    std::unique_ptr<Expression> expr_;
};

struct InterfaceMethod {
    std::string name;
    std::vector<std::pair<std::string, std::unique_ptr<TypeAST>>> params;
    std::unique_ptr<TypeAST> returnType;
    int line;
    int col;
    InterfaceMethod(const std::string& name,
                    std::vector<std::pair<std::string, std::unique_ptr<TypeAST>>> params,
                    std::unique_ptr<TypeAST> returnType, int line, int col)
        : name(name), params(std::move(params)), returnType(std::move(returnType)), line(line), col(col) {}
};

class InterfaceDeclStmt : public Statement {
public:
    InterfaceDeclStmt(const std::string& name, std::vector<InterfaceMethod> methods, int line, int col)
        : Statement(INTERFACE_DECL_STMT, line, col), name_(name), methods_(std::move(methods)) {}
    const std::string& getName() const { return name_; }
    const std::vector<InterfaceMethod>& getMethods() const { return methods_; }
    void dump(std::string& out, int depth) const override;
private:
    std::string name_;
    std::vector<InterfaceMethod> methods_;
};

class AstProgram : public ASTNode {
public:
    AstProgram() : ASTNode(PROGRAM, 1, 1) {}
    void addStatement(std::unique_ptr<Statement> stmt) { statements_.push_back(std::move(stmt)); }
    const std::vector<std::unique_ptr<Statement>>& getStatements() const { return statements_; }
    bool isStrict = false;
    void setSourcePath(const std::string& p) { sourcePath_ = p; }
    const std::string& getSourcePath() const { return sourcePath_; }
    void addImportedAst(std::unique_ptr<AstProgram> ast) { importedAsts_.push_back(std::move(ast)); }
    const std::vector<std::unique_ptr<AstProgram>>& getImportedAsts() const { return importedAsts_; }
    void dump(std::string& out, int depth) const override;
private:
    std::vector<std::unique_ptr<Statement>> statements_;
    std::vector<std::unique_ptr<AstProgram>> importedAsts_;
    std::string sourcePath_;
};

// ---------------------------------------------------------------- visitor

class ASTVisitor {
public:
    virtual ~ASTVisitor() = default;
    virtual void visit(NumberExpr&) {}
    virtual void visit(StringExpr&) {}
    virtual void visit(CharExpr&) {}
    virtual void visit(IdentifierExpr&) {}
    virtual void visit(BooleanExpr&) {}
    virtual void visit(NilExpr&) {}
    virtual void visit(NilPtrExpr&) {}
    virtual void visit(NewExpr&) {}
    virtual void visit(ThisExpr&) {}
    virtual void visit(BinaryExpr&) {}
    virtual void visit(UnaryExpr&) {}
    virtual void visit(CallExpr&) {}
    virtual void visit(MemberAccessExpr&) {}
    virtual void visit(ListExpr&) {}
    virtual void visit(TableExpr&) {}
    virtual void visit(IndexExpr&) {}
    virtual void visit(SliceExpr&) {}
    virtual void visit(TernaryExpr&) {}
    virtual void visit(GroupingExpr&) {}
    virtual void visit(FunctionExpr&) {}
    virtual void visit(SuperExpr&) {}
    virtual void visit(AwaitExpr&) {}
    virtual void visit(StructLiteralExpr&) {}
    virtual void visit(SizeofExpr&) {}
    virtual void visit(CastExpr&) {}
    virtual void visit(ArrayTypeAST&) {}
    virtual void visit(TypeAST&) {}
    virtual void visit(VarDeclStmt&) {}
    virtual void visit(AssignStmt&) {}
    virtual void visit(IfStmt&) {}
    virtual void visit(WhileStmt&) {}
    virtual void visit(ForStmt&) {}
    virtual void visit(ForEachStmt&) {}
    virtual void visit(DeleteStmt&) {}
    virtual void visit(BreakStmt&) {}
    virtual void visit(ContinueStmt&) {}
    virtual void visit(ThrowStmt&) {}
    virtual void visit(TryCatchStmt&) {}
    virtual void visit(DeferStmt&) {}
    virtual void visit(AsmStmt&) {}
    virtual void visit(MatchStmt&) {}
    virtual void visit(BlockStmt&) {}
    virtual void visit(ReturnStmt&) {}
    virtual void visit(ExpressionStmt&) {}
    virtual void visit(FunctionDeclStmt&) {}
    virtual void visit(StructDeclStmt&) {}
    virtual void visit(EnumDeclStmt&) {}
    virtual void visit(TypeAliasStmt&) {}
    virtual void visit(ClassDeclStmt&) {}
    virtual void visit(ImportStmt&) {}
    virtual void visit(FromImportStmt&) {}
    virtual void visit(ExportStmt&) {}
    virtual void visit(ExportExpressionStmt&) {}
    virtual void visit(InterfaceDeclStmt&) {}
    virtual void visit(AstProgram&) {}
};

// ---------------------------------------------------------------- dump impls

inline void FunctionExpr::dump(std::string& out, int depth) const {
    out += "(FunctionExpr " + name_;
    if (!params_.empty()) {
        out += " (params";
        for (const auto& p : params_) out += " " + p;
        out += ")";
    }
    if (!body_.empty()) {
        out += " (body";
        for (const auto& s : body_) {
            out += " ";
            s->dump(out, depth);
        }
        out += ")";
    }
    out += ")";
}

inline void VarDeclStmt::dump(std::string& out, int depth) const {
    out += isConst_ ? "(ConstDecl " : "(VarDecl ";
    out += name_ + " ";
    if (type_) type_->dump(out, depth);
    if (initializer_) {
        out += " = ";
        initializer_->dump(out, depth);
    }
    out += ")";
}

inline void AssignStmt::dump(std::string& out, int depth) const {
    out += "(Assign " + (op_.empty() ? "=" : op_) + " ";
    target_->dump(out, depth);
    out += " ";
    value_->dump(out, depth);
    out += ")";
}

inline void BlockStmt::dump(std::string& out, int depth) const {
    out += "(Block";
    for (const auto& s : statements_) {
        out += " ";
        s->dump(out, depth);
    }
    out += ")";
}

inline void IfStmt::dump(std::string& out, int depth) const {
    out += "(If ";
    condition_->dump(out, depth);
    out += " (then";
    for (const auto& s : thenBranch_) {
        out += " ";
        s->dump(out, depth);
    }
    out += ")";
    if (!elseBranch_.empty()) {
        out += " (else";
        for (const auto& s : elseBranch_) {
            out += " ";
            s->dump(out, depth);
        }
        out += ")";
    }
    out += ")";
}

inline void WhileStmt::dump(std::string& out, int depth) const {
    out += "(While ";
    condition_->dump(out, depth);
    for (const auto& s : body_) {
        out += " ";
        s->dump(out, depth);
    }
    out += ")";
}

inline void ForStmt::dump(std::string& out, int depth) const {
    out += "(For";
    if (initializer_) {
        out += " ";
        initializer_->dump(out, depth);
    }
    if (condition_) {
        out += " ";
        condition_->dump(out, depth);
    }
    if (increment_) {
        out += " ";
        increment_->dump(out, depth);
    }
    for (const auto& s : body_) {
        out += " ";
        s->dump(out, depth);
    }
    out += ")";
}

inline void ForEachStmt::dump(std::string& out, int depth) const {
    out += "(ForEach " + varName_ + " ";
    iterable_->dump(out, depth);
    for (const auto& s : body_) {
        out += " ";
        s->dump(out, depth);
    }
    out += ")";
}

inline void DeleteStmt::dump(std::string& out, int depth) const {
    out += "(Delete ";
    target_->dump(out, depth);
    out += ")";
}

inline void ThrowStmt::dump(std::string& out, int depth) const {
    out += "(Throw ";
    value_->dump(out, depth);
    out += ")";
}

inline void TryCatchStmt::dump(std::string& out, int depth) const {
    out += "(TryCatch " + catchVar_ + " (try";
    for (const auto& s : tryBlock_) {
        out += " ";
        s->dump(out, depth);
    }
    out += ") (catch";
    for (const auto& s : catchBlock_) {
        out += " ";
        s->dump(out, depth);
    }
    out += "))";
}

inline void DeferStmt::dump(std::string& out, int depth) const {
    out += "(Defer ";
    deferredStmt_->dump(out, depth);
    out += ")";
}

inline void MatchStmt::dump(std::string& out, int depth) const {
    out += "(Match ";
    expression_->dump(out, depth);
    for (const auto& arm : arms_) {
        out += " (arm ";
        arm.pattern->dump(out, depth);
        out += " ";
        arm.body->dump(out, depth);
        out += ")";
    }
    out += ")";
}

inline void ReturnStmt::dump(std::string& out, int depth) const {
    if (value_) {
        out += "(Return ";
        value_->dump(out, depth);
        out += ")";
    } else {
        out += "(Return)";
    }
}

inline void ExpressionStmt::dump(std::string& out, int depth) const {
    out += "(ExprStmt ";
    expr_->dump(out, depth);
    out += ")";
}

inline void FunctionDeclStmt::dump(std::string& out, int depth) const {
    out += "(FunctionDecl " + name_;
    if (isVirtual_) out += " virtual";
    if (isOverride_) out += " override";
    if (isExtern_) out += " extern";
    if (isAsync_) out += " async";
    if (!templateParams_.empty()) {
        out += " <";
        for (size_t i = 0; i < templateParams_.size(); ++i) {
            if (i) out += ", ";
            out += templateParams_[i];
        }
        out += ">";
    }
    if (!params_.empty()) {
        out += " (params";
        for (const auto& p : params_) {
            out += " (" + p.first + " ";
            p.second->dump(out, depth);
            out += ")";
        }
        out += ")";
    }
    if (returnType_) {
        out += " (returns ";
        returnType_->dump(out, depth);
        out += ")";
    }
    out += " (body";
    for (const auto& s : body_) {
        out += " ";
        s->dump(out, depth);
    }
    out += ")";
    out += ")";
}

inline void StructDeclStmt::dump(std::string& out, int depth) const {
    out += isUnion_ ? "(Union " : "(Struct ";
    out += name_;
    if (!templateParams_.empty()) {
        out += " <";
        for (size_t i = 0; i < templateParams_.size(); ++i) {
            if (i) out += ", ";
            out += templateParams_[i];
        }
        out += ">";
    }
    for (const auto& f : fields_) {
        out += " (" + f.first + " ";
        f.second->dump(out, depth);
        out += ")";
    }
    out += ")";
}

inline void EnumDeclStmt::dump(std::string& out, int /*depth*/) const {
    out += "(Enum " + name_;
    for (const auto& v : variants_) out += " " + v;
    out += ")";
}

inline void TypeAliasStmt::dump(std::string& out, int depth) const {
    out += "(TypeAlias " + name_ + " ";
    targetType_->dump(out, depth);
    out += ")";
}

inline void ClassDeclStmt::dump(std::string& out, int depth) const {
    out += "(Class " + name_;
    if (!superclassName_.empty()) out += " extends " + superclassName_;
    for (const auto& m : members_) {
        out += " ";
        m->dump(out, depth);
    }
    out += ")";
}

inline void FromImportStmt::dump(std::string& out, int /*depth*/) const {
    out += "(FromImport ";
    detail::dumpString(out, moduleName_);
    for (const auto& s : symbols_) out += " " + s;
    out += ")";
}

inline void ExportStmt::dump(std::string& out, int depth) const {
    out += "(Export ";
    stmt_->dump(out, depth);
    out += ")";
}

inline void ExportExpressionStmt::dump(std::string& out, int depth) const {
    out += "(ExportExpr ";
    expr_->dump(out, depth);
    out += ")";
}

inline void InterfaceDeclStmt::dump(std::string& out, int depth) const {
    out += "(Interface " + name_;
    for (const auto& m : methods_) {
        out += " (method " + m.name + " (params";
        for (const auto& p : m.params) {
            out += " (" + p.first + " ";
            p.second->dump(out, depth);
            out += ")";
        }
        out += ")";
        if (m.returnType) {
            out += " (returns ";
            m.returnType->dump(out, depth);
            out += ")";
        }
        out += ")";
    }
    out += ")";
}

inline void AstProgram::dump(std::string& out, int depth) const {
    out += "(Program";
    if (isStrict) out += " strict";
    for (const auto& s : statements_) {
        out += "\n  ";
        s->dump(out, depth + 1);
    }
    out += "\n)";
}

inline std::string dump(const AstProgram& prog) {
    std::string out;
    prog.dump(out, 0);
    return out;
}

// ---------------------------------------------------------------- accept impls

inline void ASTNode::accept(ASTVisitor& v) {
    if (auto* n = dynamic_cast<NumberExpr*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<StringExpr*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<CharExpr*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<BooleanExpr*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<NilExpr*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<NilPtrExpr*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<NewExpr*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<IdentifierExpr*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<ThisExpr*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<SuperExpr*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<BinaryExpr*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<UnaryExpr*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<CallExpr*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<MemberAccessExpr*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<ListExpr*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<TableExpr*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<IndexExpr*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<SliceExpr*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<TernaryExpr*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<GroupingExpr*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<FunctionExpr*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<AwaitExpr*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<StructLiteralExpr*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<SizeofExpr*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<CastExpr*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<ArrayTypeAST*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<TypeAST*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<VarDeclStmt*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<AssignStmt*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<IfStmt*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<WhileStmt*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<ForStmt*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<ForEachStmt*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<DeleteStmt*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<BlockStmt*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<BreakStmt*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<ContinueStmt*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<ThrowStmt*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<TryCatchStmt*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<DeferStmt*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<AsmStmt*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<MatchStmt*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<ReturnStmt*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<ExpressionStmt*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<FunctionDeclStmt*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<StructDeclStmt*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<EnumDeclStmt*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<TypeAliasStmt*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<ClassDeclStmt*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<ImportStmt*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<FromImportStmt*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<ExportStmt*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<ExportExpressionStmt*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<InterfaceDeclStmt*>(this)) { v.visit(*n); return; }
    if (auto* n = dynamic_cast<AstProgram*>(this)) { v.visit(*n); return; }
}

}  // namespace cider
