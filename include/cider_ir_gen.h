#ifndef CIDER_IR_GEN_H
#define CIDER_IR_GEN_H

#include "cider_ast.h"
#include "cider_ir.h"
#include "cider_symbols.h"
#include <map>
#include <set>

namespace Cider {

    class IRGenerator : public ASTVisitor {
    public:
        IRGenerator(SymbolTable& syms) : symbolTable_(syms) {}

        ModuleIR generate(Program& program) {
            visitedPrograms_.clear();
            globalValues_.clear();
            program.accept(*this);

            // Populate V-Tables in module
            // We need to find all StructSymbols that are classes
            // This is a bit inefficient but works for now: we can't easily iterate all symbols in SymbolTable
            // So we'll have ClassDeclStmt visitor register them or search the global scope.
            // Let's assume they are in the global scope.
            auto globalScope = symbolTable_.getCurrentScope();
            while (globalScope->getParent()) globalScope = globalScope->getParent();
            
            // We need a way to get all symbols. Let's add a getter to Scope or SymbolTable if needed.
            // For now, let's just use ClassDeclStmt to register them into a list in IRGenerator.
            for (const auto& className : classNames_) {
                auto sym = symbolTable_.resolve(className);
                if (sym && sym->kind == SymbolKind::CLASS) {
                    auto classSym = std::static_pointer_cast<StructSymbol>(sym);
                    if (!classSym->vtable.empty()) {
                        module_.addVTable(className, classSym->vtable);
                    }
                }
            }

            return std::move(module_);
        }

        void startBlock(const std::string& name) {
            if (currentBlock_) {
                currentFunction_->addBlock(std::move(currentBlock_));
            }
            currentBlock_ = std::make_unique<BasicBlock>(name);
        }

        void emitDefers() {
            // Emit all defers in reverse order of scopes, and reverse order within scope
            for (auto it = deferStack_.rbegin(); it != deferStack_.rend(); ++it) {
                for (auto s_it = it->rbegin(); s_it != it->rend(); ++s_it) {
                    (*s_it)->accept(*this);
                }
            }
        }

        IRType mapType(const std::string& name) {
            if (name == "i8") return IRType::I8;
            if (name == "u8" || name == "byte" || name == "char") return IRType::U8;
            if (name == "i16") return IRType::I16;
            if (name == "u16") return IRType::U16;
            if (name == "i32" || name == "int") return IRType::I32;
            if (name == "u32") return IRType::U32;
            if (name == "i64") return IRType::I64;
            if (name == "u64") return IRType::U64;
            if (name == "float") return IRType::F32;
            if (name == "bool") return IRType::BOOL;
            if (name == "void") return IRType::VOID;
            return IRType::PTR;
        }

        // Visitor methods
        void visit(Program& program) override {
            if (visitedPrograms_.count(&program)) return;
            visitedPrograms_.insert(&program);

            for (auto& imported : program.getImportedAsts()) {
                imported->accept(*this);
            }
            for (auto& stmt : program.getStatements()) {
                stmt->accept(*this);
            }
        }

        void visit(FunctionDeclStmt& stmt) override {
            std::string funcName = stmt.getName();
            if (!currentClassName_.empty()) {
                funcName = currentClassName_ + "_" + funcName;
            }

            // Save state for nested generation
            auto prevFunction = std::move(currentFunction_);
            auto prevBlock = std::move(currentBlock_);
            auto prevVariableValues = variableValues_;
            int prevTempCount = tempCount_;
            int prevLabelCount = labelCount_;
            auto prevBreakLabels = breakLabels_;
            auto prevContinueLabels = continueLabels_;
            auto prevDeferStack = deferStack_;

            currentFunction_ = std::make_unique<FunctionIR>(funcName, mapType(TypeInfo::fromAST(stmt.getReturnType()).name));
            currentBlock_ = std::make_unique<BasicBlock>("entry");

            variableValues_.clear();
            tempCount_ = 0;
            labelCount_ = 0;
            breakLabels_.clear();
            continueLabels_.clear();
            deferStack_.clear();
            deferStack_.push_back({}); // Function scope level

            symbolTable_.enterScope();

            // Handle 'this' pointer for methods
            if (!currentClassName_.empty()) {
                std::string paramName = "this";
                IRType paramIRType = IRType::PTR;
                currentFunction_->addParam(paramName);
                
                auto thisSym = std::make_shared<Symbol>();
                thisSym->kind = SymbolKind::VARIABLE;
                thisSym->name = "this";
                thisSym->type.name = currentClassName_;
                thisSym->type.pointerLevel = 1;
                thisSym->type.isUserDefined = true;
                symbolTable_.define(thisSym);

                IRValue paramReg = IRValue::Register(paramName, paramIRType);
                IRValue addr = IRValue::Register(nextTemp(), IRType::PTR);
                currentBlock_->addInstruction({IROp::ALLOCA, addr, {IRValue::Constant(8, IRType::I32)}});
                currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, paramReg, IRValue::Constant(0, IRType::I32)}});
                variableValues_[paramName] = addr;
            }
            for (auto& param : stmt.getParams()) {
                auto sym = std::make_shared<Symbol>();
                sym->kind = SymbolKind::VARIABLE;
                sym->name = param.first;
                sym->type = TypeInfo::fromAST(param.second.get());
                symbolTable_.define(sym);

                IRType paramIRType = (sym->type.pointerLevel > 0) ? IRType::PTR : mapType(sym->type.name);
                std::string paramRegName = "arg_" + param.first;
                currentFunction_->addParam(paramRegName);
                IRValue paramReg = IRValue::Register(paramRegName, paramIRType);
                IRValue addr = IRValue::Register(nextTemp(), IRType::PTR);
                currentBlock_->addInstruction({IROp::ALLOCA, addr, {IRValue::Constant(getTypeSize(sym->type), IRType::I32)}});
                currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, paramReg, IRValue::Constant(0, IRType::I32)}});
                variableValues_[param.first] = addr;
                variableValues_[paramRegName] = addr; // Support both for lookup convenience
            }
            for (auto& s : stmt.getBody()) {
                s->accept(*this);
            }
            
            if (currentBlock_ && (currentBlock_->getInstructions().empty() || currentBlock_->getInstructions().back().op != IROp::RET)) {
                emitDefers();
                currentBlock_->addInstruction({IROp::RET, std::nullopt, {}});
            }

            if (currentBlock_) {
                currentFunction_->addBlock(std::move(currentBlock_));
                currentBlock_ = nullptr;
            }
            module_.addFunction(std::move(currentFunction_));
            symbolTable_.exitScope();
            deferStack_.pop_back();

            // Restore state
            currentFunction_ = std::move(prevFunction);
            currentBlock_ = std::move(prevBlock);
            variableValues_ = prevVariableValues;
            tempCount_ = prevTempCount;
            labelCount_ = prevLabelCount;
            breakLabels_ = prevBreakLabels;
            continueLabels_ = prevContinueLabels;
            deferStack_ = prevDeferStack;
        }

        void visit(ReturnStmt& stmt) override {
            if (stmt.getValue()) {
                stmt.getValue()->accept(*this);
                IRValue val = lastValue_;
                emitDefers();
                currentBlock_->addInstruction({IROp::RET, std::nullopt, {val}});
            } else {
                emitDefers();
                currentBlock_->addInstruction({IROp::RET, std::nullopt, {}});
            }
            startBlock(nextLabel("dead"));
        }

        void visit(CallExpr& expr) override {
            auto calleeExpr = expr.getCallee();
            std::string funcName = "";
            bool isVirtual = false;
            int vtableIndex = -1;
            IRValue objPtr = IRValue::Constant(0, IRType::PTR);

            if (auto id = dynamic_cast<IdentifierExpr*>(calleeExpr)) {
                funcName = id->getName();
            } else if (auto mem = dynamic_cast<MemberAccessExpr*>(calleeExpr)) {
                // Method call: obj.method() or obj->method()
                bool wasAddress = isAddress_;
                isAddress_ = false;
                mem->getObject()->accept(*this);
                objPtr = lastValue_;
                isAddress_ = wasAddress;

                TypeInfo objType = lastExprType_;
                if (objType.pointerLevel > 0) objType.pointerLevel--;
                
                auto sym = symbolTable_.resolve(objType.name);
                if (sym && (sym->kind == SymbolKind::CLASS || sym->kind == SymbolKind::STRUCT)) {
                    auto classSym = std::static_pointer_cast<StructSymbol>(sym);
                    for (const auto& m : classSym->methods) {
                        if (m.name == mem->getMember()) {
                            funcName = m.mangledName;
                            isVirtual = m.isVirtual || m.isOverride;
                            vtableIndex = m.vtableIndex;
                            break;
                        }
                    }
                }
            }

            if (!funcName.empty()) {
                std::vector<IRValue> args;
                for (auto& arg : expr.getArgs()) {
                    arg->accept(*this);
                    args.push_back(lastValue_);
                }
                
                auto sym = symbolTable_.resolve(funcName);
                IRType retIRType = IRType::I32;
                if (sym && sym->kind == SymbolKind::FUNCTION) {
                    auto funcSym = std::static_pointer_cast<FunctionSymbol>(sym);
                    retIRType = (funcSym->returnType.pointerLevel > 0) ? IRType::PTR : mapType(funcSym->returnType.name);
                    lastExprType_ = funcSym->returnType;
                }

                IRValue dest = IRValue::Register(nextTemp(), retIRType);
                
                IRInstruction callInstr;
                if (isVirtual && vtableIndex != -1) {
                    callInstr.op = IROp::VIRTUAL_CALL;
                    callInstr.args.push_back(objPtr);
                    callInstr.args.push_back(IRValue::Constant(vtableIndex, IRType::I32));
                    for (auto& a : args) callInstr.args.push_back(a);
                } else {
                    callInstr.op = IROp::CALL;
                    callInstr.args.push_back(IRValue::Label(funcName));
                    if (objPtr.kind != IRValue::CONSTANT || objPtr.constValue != 0) {
                        callInstr.args.push_back(objPtr);
                    }
                    for (auto& a : args) callInstr.args.push_back(a);
                }
                
                callInstr.dest = dest;
                currentBlock_->addInstruction(callInstr);
                lastValue_ = dest;
            }
        }

        void visit(MemberAccessExpr& expr) override {
            bool wasAddress = isAddress_;
            isAddress_ = false;
            expr.getObject()->accept(*this);
            IRValue obj = lastValue_;
            isAddress_ = wasAddress;

            TypeInfo objType = lastExprType_;
            if (objType.pointerLevel > 0) objType.pointerLevel--;
            
            auto sym = symbolTable_.resolve(objType.name);
            if (sym && (sym->kind == SymbolKind::STRUCT || sym->kind == SymbolKind::CLASS)) {
                auto structSym = std::static_pointer_cast<StructSymbol>(sym);
                for (const auto& field : structSym->fields) {
                    if (field.name == expr.getMember()) {
                        IRValue addr = IRValue::Register(nextTemp(), IRType::PTR);
                        currentBlock_->addInstruction({IROp::ADD, addr, {obj, IRValue::Constant(field.offset, IRType::I32)}});
                        
                        if (isAddress_) {
                            lastValue_ = addr;
                        } else {
                            IRType fieldIRType = (field.type.pointerLevel > 0) ? IRType::PTR : mapType(field.type.name);
                            IRValue dest = IRValue::Register(nextTemp(), fieldIRType);
                            // Correct LOAD: LOAD dest, [addr + 0]
                            currentBlock_->addInstruction({IROp::LOAD, dest, {addr, IRValue::Constant(0, IRType::I32)}});
                            lastValue_ = dest;
                        }
                        lastExprType_ = field.type;
                        return;
                    }
                }
            }
            
            IRValue dest = IRValue::Register(nextTemp(), IRType::I32);
            currentBlock_->addInstruction({IROp::LOAD, dest, {obj, IRValue::Label(expr.getMember())}});
            lastValue_ = dest;
        }

        void visit(StructLiteralExpr& expr) override {
            IRValue dest = IRValue::Register(nextTemp(), IRType::PTR);
            currentBlock_->addInstruction({IROp::ALLOCA, dest, {IRValue::Constant(getTypeSize({expr.getStructName(), 0}), IRType::I32)}});
            
            auto sym = symbolTable_.resolve(expr.getStructName());
            if (sym && (sym->kind == SymbolKind::STRUCT || sym->kind == SymbolKind::CLASS)) {
                auto structSym = std::static_pointer_cast<StructSymbol>(sym);
                for (auto& fieldInit : expr.getFields()) {
                    fieldInit.second->accept(*this);
                    IRValue val = lastValue_;
                    
                    int offset = 0;
                    for (const auto& f : structSym->fields) {
                        if (f.name == fieldInit.first) {
                            offset = f.offset;
                            break;
                        }
                    }
                    currentBlock_->addInstruction({IROp::STORE, std::nullopt, {dest, val, IRValue::Constant(offset, IRType::I32)}});
                }
            }
            lastValue_ = dest;
            lastExprType_ = {expr.getStructName(), 0};
        }

        IRValue getAddress(Expression* expr) {
            bool wasAddress = isAddress_;
            isAddress_ = true;
            expr->accept(*this);
            IRValue addr = lastValue_;
            isAddress_ = wasAddress;
            return addr;
        }

        void visit(UnaryExpr& expr) override {
            TypeInfo type = lastExprType_;

            if (expr.getOperator() == "&") {
                isAddress_ = true;
                expr.getOperand()->accept(*this);
                isAddress_ = false;
                type = lastExprType_;
                type.pointerLevel++;
            } else if (expr.getOperator() == "*") {
                expr.getOperand()->accept(*this);
                IRValue operand = lastValue_;
                type = lastExprType_;
                if (isAddress_) {
                    lastValue_ = operand;
                } else {
                    IRType derefType = (type.pointerLevel > 1) ? IRType::PTR : mapType(type.name);
                    IRValue dest = IRValue::Register(nextTemp(), derefType);
                    currentBlock_->addInstruction({IROp::LOAD, dest, {operand, IRValue::Constant(0, IRType::I32)}});
                    lastValue_ = dest;
                }
                if (type.pointerLevel > 0) type.pointerLevel--;
            } else {
                expr.getOperand()->accept(*this);
                IRValue operand = lastValue_;
                type = lastExprType_;
                
                if (expr.getOperator() == "~") {
                    IRType irType = (type.pointerLevel > 0) ? IRType::PTR : mapType(type.name);
                    IRValue dest = IRValue::Register(nextTemp(), irType);
                    currentBlock_->addInstruction({IROp::BIT_NOT, dest, {operand}});
                    lastValue_ = dest;
                } else if (expr.getOperator() == "!") {
                    IRValue dest = IRValue::Register(nextTemp(), IRType::BOOL);
                    currentBlock_->addInstruction({IROp::BIT_XOR, dest, {operand, IRValue::Constant(1, IRType::BOOL)}});
                    lastValue_ = dest;
                    type = {"bool", 0};
                } else if (expr.getOperator() == "-") {
                    IRType irType = (type.pointerLevel > 0) ? IRType::PTR : mapType(type.name);
                    IRValue dest = IRValue::Register(nextTemp(), irType);
                    currentBlock_->addInstruction({IROp::SUB, dest, {IRValue::Constant(0, irType), operand}});
                    lastValue_ = dest;
                } else {
                    lastValue_ = operand;
                }
            }
            lastExprType_ = type;
        }

        void visit(VarDeclStmt& stmt) override {
            auto sym = std::make_shared<Symbol>();
            sym->kind = SymbolKind::VARIABLE;
            sym->name = stmt.getName();
            sym->type = TypeInfo::fromAST(stmt.getTypeAST());
            symbolTable_.define(sym);

            if (!currentFunction_) {
                double initVal = 0;
                if (stmt.getInitializer()) {
                    if (auto num = dynamic_cast<NumberExpr*>(stmt.getInitializer())) {
                        initVal = num->getValue();
                    }
                }
                module_.addGlobalVar(stmt.getName(), (sym->type.pointerLevel > 0) ? IRType::PTR : mapType(sym->type.name), initVal);
                globalValues_[stmt.getName()] = IRValue::Label(stmt.getName());
                return;
            }

            IRValue addr = IRValue::Register(nextTemp(), IRType::PTR);
            currentBlock_->addInstruction({IROp::ALLOCA, addr, {IRValue::Constant(getTypeSize(sym->type), IRType::I32)}});
            variableValues_[stmt.getName()] = addr;

            if (stmt.getInitializer()) {
                stmt.getInitializer()->accept(*this);
                IRValue val = lastValue_;
                if (sym->type.isArray || sym->type.isUserDefined || getTypeSize(sym->type) > 8) {
                    currentBlock_->addInstruction({IROp::MEMCPY, std::nullopt, {addr, val, IRValue::Constant(getTypeSize(sym->type), IRType::I32)}});
                } else {
                    currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, val, IRValue::Constant(0, IRType::I32)}});
                }
            }
        }

        int getTypeSize(TypeInfo type) {
            if (type.pointerLevel > 0) return 8;
            if (type.isArray) {
                TypeInfo elemType = type;
                elemType.isArray = false;
                elemType.arraySize = 0;
                return type.arraySize * getTypeSize(elemType);
            }
            if (type.name == "u8" || type.name == "i8" || type.name == "char" || type.name == "byte" || type.name == "void") return 1;
            if (type.name == "u16" || type.name == "i16") return 2;
            if (type.name == "u32" || type.name == "i32" || type.name == "int" || type.name == "float" || type.name == "bool") return 4;
            if (type.name == "u64" || type.name == "i64") return 8;

            auto sym = symbolTable_.resolve(type.name);
            if (sym && (sym->kind == SymbolKind::STRUCT || sym->kind == SymbolKind::CLASS)) {
                return std::static_pointer_cast<StructSymbol>(sym)->totalSize;
            }
            return 8; // Default
        }

        void visit(BinaryExpr& expr) override {
            expr.getLeft()->accept(*this);
            IRValue left = lastValue_;
            TypeInfo leftType = lastExprType_;

            std::string sOp = expr.getOperator();

            if (sOp == "&&") {
                IRValue condReg = lastValue_;
                std::string scTrueName = nextLabel("sc_true");
                std::string scFalseName = nextLabel("sc_false");
                std::string scMergeName = nextLabel("sc_merge");
                currentBlock_->addInstruction({IROp::COND_BR, std::nullopt, {condReg, IRValue::Label(scTrueName), IRValue::Label(scFalseName)}});
                startBlock(scTrueName);
                expr.getRight()->accept(*this);
                IRValue rightVal = lastValue_;
                TypeInfo rightType = lastExprType_;
                currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(scMergeName)}});
                startBlock(scFalseName);
                IRValue falseVal = IRValue::Constant(0, IRType::BOOL);
                currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(scMergeName)}});
                startBlock(scMergeName);
                IRValue phiDest = IRValue::Register(nextTemp(), IRType::BOOL);
                IRInstruction phiInstr;
                phiInstr.op = IROp::PHI;
                phiInstr.dest = phiDest;
                phiInstr.phiArgs[scTrueName] = rightVal;
                phiInstr.phiArgs[scFalseName] = falseVal;
                currentBlock_->addInstruction(phiInstr);
                lastValue_ = phiDest;
                lastExprType_ = rightType.pointerLevel > 0 ? rightType : TypeInfo{"bool", 0};
                return;
            }

            if (sOp == "||") {
                IRValue condReg = lastValue_;
                std::string scTrueName = nextLabel("sc_true");
                std::string scFalseName = nextLabel("sc_false");
                std::string scMergeName = nextLabel("sc_merge");
                currentBlock_->addInstruction({IROp::COND_BR, std::nullopt, {condReg, IRValue::Label(scTrueName), IRValue::Label(scFalseName)}});
                startBlock(scTrueName);
                IRValue trueVal = IRValue::Constant(1, IRType::BOOL);
                currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(scMergeName)}});
                startBlock(scFalseName);
                expr.getRight()->accept(*this);
                IRValue rightVal = lastValue_;
                TypeInfo rightType = lastExprType_;
                currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(scMergeName)}});
                startBlock(scMergeName);
                IRValue phiDest = IRValue::Register(nextTemp(), IRType::BOOL);
                IRInstruction phiInstr;
                phiInstr.op = IROp::PHI;
                phiInstr.dest = phiDest;
                phiInstr.phiArgs[scTrueName] = trueVal;
                phiInstr.phiArgs[scFalseName] = rightVal;
                currentBlock_->addInstruction(phiInstr);
                lastValue_ = phiDest;
                lastExprType_ = rightType.pointerLevel > 0 ? rightType : TypeInfo{"bool", 0};
                return;
            }

            expr.getRight()->accept(*this);
            IRValue right = lastValue_;
            TypeInfo rightType = lastExprType_;

            IROp op;
            
            if (leftType.pointerLevel > 0 && (sOp == "+" || sOp == "-")) {
                TypeInfo baseType = leftType;
                baseType.pointerLevel--;
                int size = getTypeSize(baseType);
                if (size > 1) {
                    IRValue scaled = IRValue::Register(nextTemp(), right.type);
                    currentBlock_->addInstruction({IROp::MUL, scaled, {right, IRValue::Constant(size, IRType::I32)}});
                    right = scaled;
                }
            } else if (rightType.pointerLevel > 0 && sOp == "+") {
                TypeInfo baseType = rightType;
                baseType.pointerLevel--;
                int size = getTypeSize(baseType);
                if (size > 1) {
                    IRValue scaled = IRValue::Register(nextTemp(), left.type);
                    currentBlock_->addInstruction({IROp::MUL, scaled, {left, IRValue::Constant(size, IRType::I32)}});
                    left = scaled;
                }
            }

            if (sOp == "+") op = IROp::ADD;
            else if (sOp == "-") op = IROp::SUB;
            else if (sOp == "*") op = IROp::MUL;
            else if (sOp == "/") op = IROp::DIV;
            else if (sOp == "%") op = IROp::MOD;
            else if (sOp == "==") op = IROp::CMP_EQ;
            else if (sOp == "!=") op = IROp::CMP_NE;
            else if (sOp == "<") op = IROp::CMP_LT;
            else if (sOp == ">") op = IROp::CMP_GT;
            else if (sOp == "<=") op = IROp::CMP_LE;
            else if (sOp == ">=") op = IROp::CMP_GE;
            else if (sOp == "&") op = IROp::BIT_AND;
            else if (sOp == "|") op = IROp::BIT_OR;
            else if (sOp == "^") op = IROp::BIT_XOR;
            else if (sOp == "<<") op = IROp::LSHIFT;
            else if (sOp == ">>") op = IROp::RSHIFT;
            else return;

            IRValue dest = IRValue::Register(nextTemp(), left.type);
            currentBlock_->addInstruction({op, dest, {left, right}});
            lastValue_ = dest;
        }

        void visit(NumberExpr& expr) override {
            lastValue_ = IRValue::Constant(expr.getValue(), IRType::I32);
        }

        void visit(CharExpr& expr) override {
            lastValue_ = IRValue::Constant(expr.getValue(), IRType::U8);
        }

        void visit(IdentifierExpr& expr) override {
            if (expr.getName() == "_") {
                lastValue_ = IRValue::Constant(1, IRType::BOOL);
                lastExprType_ = {"bool", 0};
                return;
            }
            
            auto sym = symbolTable_.resolve(expr.getName());
            if (sym) {
                lastExprType_ = sym->type;
                // Only treat as user-defined (struct/class) if it's NOT a pointer
                if (lastExprType_.pointerLevel == 0) {
                    auto typeSym = symbolTable_.resolve(lastExprType_.name);
                    if (typeSym && (typeSym->kind == SymbolKind::STRUCT || typeSym->kind == SymbolKind::CLASS)) {
                        lastExprType_.isUserDefined = true;
                    }
                } else {
                    lastExprType_.isUserDefined = false;
                }
            }

            if (isAddress_) {
                if (variableValues_.count(expr.getName())) {
                    lastValue_ = variableValues_[expr.getName()];
                    return;
                }
            }

            if (lastExprType_.isArray || lastExprType_.isUserDefined) {
                lastValue_ = getAddress(&expr);
                return;
            }

            if (variableValues_.count(expr.getName())) {
                IRValue addr = variableValues_[expr.getName()];
                IRType irType = (lastExprType_.pointerLevel > 0) ? IRType::PTR : mapType(lastExprType_.name);
                IRValue dest = IRValue::Register(nextTemp(), irType);
                currentBlock_->addInstruction({IROp::LOAD, dest, {addr, IRValue::Constant(0, IRType::I32)}});
                lastValue_ = dest;
            } else if (globalValues_.count(expr.getName())) {
                lastValue_ = globalValues_[expr.getName()];
                if (!isAddress_) {
                    IRType irType = (lastExprType_.pointerLevel > 0) ? IRType::PTR : mapType(lastExprType_.name);
                    IRValue dest = IRValue::Register(nextTemp(), irType);
                    currentBlock_->addInstruction({IROp::LOAD, dest, {lastValue_}});
                    lastValue_ = dest;
                }
            } else {
                lastValue_ = IRValue::Register(expr.getName(), IRType::I32);
                lastExprType_ = {"int", 0};
            }
        }

        void visit(AssignStmt& stmt) override {
            stmt.getValue()->accept(*this);
            IRValue val = lastValue_;
            
            IRValue addr = IRValue::Constant(0, IRType::PTR);
            if (auto ident = dynamic_cast<IdentifierExpr*>(stmt.getTarget())) {
                addr = variableValues_.count(ident->getName()) ? variableValues_[ident->getName()] : globalValues_[ident->getName()];
            } else if (auto mem = dynamic_cast<MemberAccessExpr*>(stmt.getTarget())) {
                addr = getAddress(mem);
            } else if (auto idx = dynamic_cast<IndexExpr*>(stmt.getTarget())) {
                addr = getAddress(idx);
            } else if (auto unary = dynamic_cast<UnaryExpr*>(stmt.getTarget())) {
                if (unary->getOperator() == "*") {
                    unary->getOperand()->accept(*this);
                    addr = lastValue_;
                }
            } else {
                addr = getAddress(stmt.getTarget());
            }

            if (lastExprType_.isArray || lastExprType_.isUserDefined || getTypeSize(lastExprType_) > 8) {
                currentBlock_->addInstruction({IROp::MEMCPY, std::nullopt, {addr, val, IRValue::Constant(getTypeSize(lastExprType_), IRType::I32)}});
            } else {
                if (addr.kind == IRValue::LABEL) {
                    currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, val}});
                } else {
                    currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, val, IRValue::Constant(0, IRType::I32)}});
                }
            }
        }

        void visit(IfStmt& stmt) override {
            stmt.getCondition()->accept(*this);
            IRValue cond = lastValue_;

            std::string thenName = nextLabel("then");
            std::string elseName = nextLabel("else");
            std::string mergeName = nextLabel("ifcont");

            currentBlock_->addInstruction({IROp::COND_BR, std::nullopt, {cond, IRValue::Label(thenName), IRValue::Label(elseName)}});
            
            startBlock(thenName);
            symbolTable_.enterScope();
            for (auto& s : stmt.getThenBranch()) s->accept(*this);
            if (currentBlock_ && (currentBlock_->getInstructions().empty() || (currentBlock_->getInstructions().back().op != IROp::BR && currentBlock_->getInstructions().back().op != IROp::RET))) {
                currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(mergeName)}});
            }
            symbolTable_.exitScope();

            startBlock(elseName);
            symbolTable_.enterScope();
            for (auto& s : stmt.getElseBranch()) s->accept(*this);
            if (currentBlock_ && (currentBlock_->getInstructions().empty() || (currentBlock_->getInstructions().back().op != IROp::BR && currentBlock_->getInstructions().back().op != IROp::RET))) {
                currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(mergeName)}});
            }
            symbolTable_.exitScope();

            startBlock(mergeName);
        }

        void visit(WhileStmt& stmt) override {
            std::string headerName = nextLabel("while_header");
            std::string bodyName = nextLabel("while_body");
            std::string exitName = nextLabel("while_exit");

            currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(headerName)}});

            startBlock(headerName);
            stmt.getCondition()->accept(*this);
            IRValue cond = lastValue_;
            currentBlock_->addInstruction({IROp::COND_BR, std::nullopt, {cond, IRValue::Label(bodyName), IRValue::Label(exitName)}});

            continueLabels_.push_back(headerName);
            breakLabels_.push_back(exitName);
            deferStack_.push_back({});
            symbolTable_.enterScope();

            startBlock(bodyName);
            for (auto& s : stmt.getBody()) s->accept(*this);
            if (currentBlock_ && (currentBlock_->getInstructions().empty() || (currentBlock_->getInstructions().back().op != IROp::BR && currentBlock_->getInstructions().back().op != IROp::RET))) {
                currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(headerName)}});
            }
            symbolTable_.exitScope();
            deferStack_.pop_back();
            continueLabels_.pop_back();
            breakLabels_.pop_back();

            startBlock(exitName);
        }

        void visit(ForStmt& stmt) override {
            symbolTable_.enterScope();
            if (stmt.getInitializer()) stmt.getInitializer()->accept(*this);

            std::string headerName = nextLabel("for_header");
            std::string bodyName = nextLabel("for_body");
            std::string incName = nextLabel("for_inc");
            std::string exitName = nextLabel("for_exit");

            currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(headerName)}});

            startBlock(headerName);
            if (stmt.getCondition()) {
                stmt.getCondition()->accept(*this);
                currentBlock_->addInstruction({IROp::COND_BR, std::nullopt, {lastValue_, IRValue::Label(bodyName), IRValue::Label(exitName)}});
            } else {
                currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(bodyName)}});
            }

            continueLabels_.push_back(incName);
            breakLabels_.push_back(exitName);
            deferStack_.push_back({});

            startBlock(bodyName);
            for (auto& s : stmt.getBody()) s->accept(*this);
            if (currentBlock_ && (currentBlock_->getInstructions().empty() || (currentBlock_->getInstructions().back().op != IROp::BR && currentBlock_->getInstructions().back().op != IROp::RET))) {
                currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(incName)}});
            }

            startBlock(incName);
            if (stmt.getIncrement()) stmt.getIncrement()->accept(*this);
            currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(headerName)}});

            deferStack_.pop_back();
            continueLabels_.pop_back();
            breakLabels_.pop_back();
            symbolTable_.exitScope();

            startBlock(exitName);
        }

        void visit(AsmStmt& stmt) override {
            if (currentBlock_) {
                IRInstruction instr;
                instr.op = IROp::ASM;
                instr.args.push_back(IRValue::Label(stmt.getCode()));

                std::string code = stmt.getCode();
                std::set<std::string> mentionedVars;
                size_t pos = 0;
                while ((pos = code.find("{", pos)) != std::string::npos) {
                    size_t end = code.find("}", pos);
                    if (end != std::string::npos) {
                        std::string var = code.substr(pos + 1, end - pos - 1);
                        if (!var.empty() && var[0] == '*') var = var.substr(1);
                        if (!var.empty()) mentionedVars.insert(var);
                        pos = end + 1;
                    } else {
                        pos++;
                    }
                }

                bool prevIsAddress = isAddress_;
                isAddress_ = false;
                for (const auto& name : mentionedVars) {
                    auto sym = symbolTable_.resolve(name);
                    if (sym) {
                        IdentifierExpr ident(name, 0, 0);
                        ident.accept(*this);
                        instr.asmMap[name] = lastValue_;
                    }
                }
                isAddress_ = prevIsAddress;

                currentBlock_->addInstruction(instr);

                for (const auto& name : mentionedVars) {
                    bool found = false;
                    IRValue varAddr;
                    if (variableValues_.count(name)) {
                        varAddr = variableValues_[name];
                        found = true;
                    } else if (globalValues_.count(name)) {
                        varAddr = globalValues_[name];
                        found = true;
                    }
                    
                    if (found) {
                        IRValue val = instr.asmMap[name];
                        if (varAddr.kind == IRValue::LABEL) {
                            currentBlock_->addInstruction({IROp::STORE, std::nullopt, {varAddr, val}});
                        } else {
                            currentBlock_->addInstruction({IROp::STORE, std::nullopt, {varAddr, val, IRValue::Constant(0, IRType::I32)}});
                        }
                    }
                }
            } else {
                module_.addGlobalAsm(stmt.getCode());
            }
        }

        void visit(BreakStmt&) override {
            if (!breakLabels_.empty()) {
                emitDefers();
                currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(breakLabels_.back())}});
                startBlock(nextLabel("dead"));
            }
        }
        void visit(ContinueStmt&) override {
            if (!continueLabels_.empty()) {
                emitDefers();
                currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(continueLabels_.back())}});
                startBlock(nextLabel("dead"));
            }
        }
        void visit(DeferStmt& stmt) override {
            deferStack_.back().push_back(stmt.getDeferredStmt());
        }
        void visit(ForEachStmt&) override {}
        void visit(ThrowStmt&) override {}
        void visit(TryCatchStmt&) override {}

        void visit(StringExpr& expr) override {
            IRValue strVal;
            strVal.kind = IRValue::CONSTANT;
            strVal.name = "\"" + expr.getValue() + "\"";
            strVal.type = IRType::PTR;
            strVal.constValue = 0;
            lastValue_ = strVal;
            lastExprType_ = {"string", 0};
        }
        void visit(BooleanExpr& expr) override {
            lastValue_ = IRValue::Constant(expr.getValue() ? 1 : 0, IRType::BOOL);
            lastExprType_ = {"bool", 0};
        }
        void visit(NilExpr&) override { 
            lastValue_ = IRValue::Constant(0, IRType::PTR); 
            lastExprType_ = {"nil", 0};
        }
        void visit(NilPtrExpr&) override { 
            lastValue_ = IRValue::Constant(0, IRType::PTR); 
            lastExprType_ = {"void", 1};
        }
        void visit(NewExpr& expr) override {
            TypeInfo type = TypeInfo::fromAST(expr.getNewType());
            int size = getTypeSize(type);
            IRValue dest = IRValue::Register(nextTemp(), IRType::PTR);
            IRInstruction call;
            call.op = IROp::CALL;
            call.dest = dest;
            call.args.push_back(IRValue::Label("malloc"));
            call.args.push_back(IRValue::Constant(size, IRType::I32));
            currentBlock_->addInstruction(call);

            // Initialize vptr if class is virtual
            auto sym = symbolTable_.resolve(type.name);
            if (sym && sym->kind == SymbolKind::CLASS) {
                auto classSym = std::static_pointer_cast<StructSymbol>(sym);
                if (!classSym->vtable.empty()) {
                    IRValue vptrVal = IRValue::Label("vtable_" + type.name);
                    currentBlock_->addInstruction({IROp::STORE, std::nullopt, {dest, vptrVal, IRValue::Constant(0, IRType::I32)}});
                }
            }

            lastValue_ = dest;
            TypeInfo resType = type; resType.pointerLevel++; lastExprType_ = resType;
        }
        void visit(DeleteStmt& stmt) override {
            stmt.getTarget()->accept(*this);
            IRValue ptr = lastValue_;
            IRInstruction call;
            call.op = IROp::CALL;
            call.dest = std::nullopt;
            call.args.push_back(IRValue::Label("free"));
            call.args.push_back(ptr);
            currentBlock_->addInstruction(call);
        }
        void visit(IndexExpr& expr) override {
            bool wasAddress = isAddress_;
            isAddress_ = false;
            expr.getObject()->accept(*this);
            IRValue obj = lastValue_;
            TypeInfo baseType = lastExprType_;
            
            expr.getIndex()->accept(*this);
            IRValue idx = lastValue_;
            isAddress_ = wasAddress;

            TypeInfo elemType = baseType;
            if (elemType.pointerLevel > 0) elemType.pointerLevel--;
            else if (elemType.isArray) elemType.isArray = false;

            int elemSize = getTypeSize(elemType);
            IRValue scaledIdx = IRValue::Register(nextTemp(), IRType::I32);
            currentBlock_->addInstruction({IROp::MUL, scaledIdx, {idx, IRValue::Constant(elemSize, IRType::I32)}});
            IRValue addr = IRValue::Register(nextTemp(), IRType::PTR);
            currentBlock_->addInstruction({IROp::ADD, addr, {obj, scaledIdx}});
            
            lastExprType_ = elemType;
            if (isAddress_) {
                lastValue_ = addr;
            } else {
                IRType irType = (elemType.pointerLevel > 0) ? IRType::PTR : mapType(elemType.name);
                IRValue dest = IRValue::Register(nextTemp(), irType);
                currentBlock_->addInstruction({IROp::LOAD, dest, {addr, IRValue::Constant(0, IRType::I32)}});
                lastValue_ = dest;
            }
        }
        void visit(GroupingExpr& expr) override { expr.getExpression()->accept(*this); }
        void visit(SizeofExpr& expr) override {
            TypeInfo targetType = TypeInfo::fromAST(expr.getTargetType());
            int size = getTypeSize(targetType);
            lastValue_ = IRValue::Constant(size, IRType::I32);
        }
        void visit(CastExpr& expr) override {
            expr.getExpression()->accept(*this);
            TypeInfo targetType = TypeInfo::fromAST(expr.getTargetType());
            IRType irType = (targetType.pointerLevel > 0) ? IRType::PTR : mapType(targetType.name);
            
            IRValue dest = IRValue::Register(nextTemp(), irType);
            
            // Explicitly truncate if casting to a smaller type
            if (irType == IRType::I8 || irType == IRType::U8) {
                currentBlock_->addInstruction({IROp::BIT_AND, dest, {lastValue_, IRValue::Constant(0xFF, IRType::I32)}});
            } else if (irType == IRType::I16 || irType == IRType::U16) {
                currentBlock_->addInstruction({IROp::BIT_AND, dest, {lastValue_, IRValue::Constant(0xFFFF, IRType::I32)}});
            } else {
                currentBlock_->addInstruction({IROp::MOV, dest, {lastValue_}});
            }
            
            lastValue_ = dest;
            lastExprType_ = targetType;
        }
        void visit(MatchStmt& stmt) override {
            stmt.getExpression()->accept(*this);
            IRValue matchVal = lastValue_;
            std::string exitName = nextLabel("match_exit");
            for (auto& arm : stmt.getArms()) {
                std::string armName = nextLabel("arm");
                std::string nextCheckName = nextLabel("next_check");
                arm.pattern->accept(*this);
                IRValue patternVal = lastValue_;
                IRValue cond = IRValue::Register(nextTemp(), IRType::BOOL);
                currentBlock_->addInstruction({IROp::CMP_EQ, cond, {matchVal, patternVal}});
                currentBlock_->addInstruction({IROp::COND_BR, std::nullopt, {cond, IRValue::Label(armName), IRValue::Label(nextCheckName)}});
                startBlock(armName);
                arm.body->accept(*this);
                if (currentBlock_ && (currentBlock_->getInstructions().empty() || currentBlock_->getInstructions().back().op != IROp::BR)) {
                    currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(exitName)}});
                }
                startBlock(nextCheckName);
            }
            if (currentBlock_) currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(exitName)}});
            startBlock(exitName);
        }
        void visit(BlockStmt& stmt) override {
            symbolTable_.enterScope();
            deferStack_.push_back({});
            for (auto& s : stmt.getStatements()) s->accept(*this);
            for (auto it = deferStack_.back().rbegin(); it != deferStack_.back().rend(); ++it) (*it)->accept(*this);
            deferStack_.pop_back();
            symbolTable_.exitScope();
        }
        void visit(ExpressionStmt& stmt) override { stmt.getExpression()->accept(*this); }
        void visit(ClassDeclStmt& stmt) override {
            classNames_.push_back(stmt.getName());
            std::string oldClass = currentClassName_;
            currentClassName_ = stmt.getName();
            for (auto& member : stmt.getMembers()) {
                if (dynamic_cast<FunctionDeclStmt*>(member.get())) member->accept(*this);
            }
            currentClassName_ = oldClass;
        }
        void visit(ExportStmt& stmt) override { stmt.getStmt()->accept(*this); }
        void visit(ThisExpr& expr) override {
            if (variableValues_.count("this")) {
                IRValue addr = variableValues_["this"];
                IRValue dest = IRValue::Register(nextTemp(), IRType::PTR);
                currentBlock_->addInstruction({IROp::LOAD, dest, {addr, IRValue::Constant(0, IRType::I32)}});
                lastValue_ = dest;
                lastExprType_ = {currentClassName_, 1};
                lastExprType_.isUserDefined = true;
            } else {
                // Handle error or default
                lastValue_ = IRValue::Constant(0, IRType::PTR);
            }
        }
        void visit(ListExpr&) override {}
        void visit(TableExpr&) override {}
        void visit(SliceExpr&) override {}
        void visit(TernaryExpr& expr) override {
            expr.getCondition()->accept(*this);
            IRValue cond = lastValue_;

            std::string thenName = nextLabel("tern_then");
            std::string elseName = nextLabel("tern_else");
            std::string mergeName = nextLabel("tern_end");

            currentBlock_->addInstruction({IROp::COND_BR, std::nullopt, {cond, IRValue::Label(thenName), IRValue::Label(elseName)}});

            startBlock(thenName);
            expr.getThenExpr()->accept(*this);
            IRValue thenVal = lastValue_;
            TypeInfo thenType = lastExprType_;
            currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(mergeName)}});

            startBlock(elseName);
            expr.getElseExpr()->accept(*this);
            IRValue elseVal = lastValue_;
            TypeInfo elseType = lastExprType_;
            currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(mergeName)}});

            startBlock(mergeName);
            IRType phiType = thenType.pointerLevel > 0 ? IRType::PTR : mapType(thenType.name);
            IRValue phiDest = IRValue::Register(nextTemp(), phiType);
            IRInstruction phiInstr;
            phiInstr.op = IROp::PHI;
            phiInstr.dest = phiDest;
            phiInstr.phiArgs[thenName] = thenVal;
            phiInstr.phiArgs[elseName] = elseVal;
            currentBlock_->addInstruction(phiInstr);
            lastValue_ = phiDest;
            lastExprType_ = thenType;
        }
        void visit(FunctionExpr&) override {}
        void visit(SuperExpr&) override {}
        void visit(AwaitExpr&) override {}
        void visit(ArrayTypeAST&) override {}
        void visit(TypeAST&) override {}
        void visit(StructDeclStmt&) override {}
        void visit(EnumDeclStmt&) override {}
        void visit(TypeAliasStmt&) override {}
        void visit(ImportStmt&) override {}
        void visit(FromImportStmt&) override {}
        void visit(ExportExpressionStmt&) override {}

    private:
        ModuleIR module_;
        SymbolTable& symbolTable_;
        std::unique_ptr<FunctionIR> currentFunction_;
        std::unique_ptr<BasicBlock> currentBlock_;
        std::string currentClassName_;
        IRValue lastValue_ = IRValue::Constant(0, IRType::I32);
        TypeInfo lastExprType_;
        bool isAddress_ = false;
        std::map<std::string, IRValue> variableValues_;
        std::map<std::string, IRValue> globalValues_;
        std::vector<std::string> classNames_;
        std::set<Program*> visitedPrograms_;
        std::vector<std::string> breakLabels_;
        std::vector<std::string> continueLabels_;
        std::vector<std::vector<Statement*>> deferStack_;
        int tempCount_ = 0;
        int labelCount_ = 0;
        std::string nextTemp() { return "v" + std::to_string(tempCount_++); }
        std::string nextLabel(const std::string& prefix) { 
            return (currentFunction_ ? currentFunction_->getName() + "_" : "") + prefix + std::to_string(labelCount_++); 
        }
    };

} // namespace Cider

#endif
