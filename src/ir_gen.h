#pragma once

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "ast.h"
#include "errors.h"
#include <algorithm>
#include "ir.h"
#include "symbols.h"

namespace cider {

class IRGenerator : public ASTVisitor {
public:
    IRGenerator(SymbolTable& syms, const std::string& sourcePath = "")
        : symbolTable_(syms), currentSourcePath_(sourcePath) {}

    ModuleIR generate(AstProgram& program) {
        visitedPrograms_.clear();
        globalValues_.clear();
        exceptionGlobalsEnsured_ = false;
        interpHelpersEnsured_ = false;
        program.accept(*this);

        // Populate V-Tables in module
        auto globalScope = symbolTable_.getCurrentScope();
        while (globalScope->getParent()) globalScope = globalScope->getParent();

        for (const auto& className : classNames_) {
            auto sym = symbolTable_.resolve(className);
            if (sym && sym->kind == SymbolKind::CLASS) {
                auto classSym = std::static_pointer_cast<StructSymbol>(sym);
                if (!classSym->vtable.empty()) {
                    module_.addVTable(className, classSym->vtable);
                }
            }
        }

        // Generate interface vtables: for each interface-struct pair
        for (const auto& ifaceName : interfaceNames_) {
            auto ifaceSym = symbolTable_.resolve(ifaceName);
            if (!ifaceSym || ifaceSym->kind != SymbolKind::INTERFACE) continue;
            auto iface = std::static_pointer_cast<InterfaceSymbol>(ifaceSym);
            // Check all classes/structs that might implement this interface
            std::vector<std::string> allStructClasses = classNames_;
            for (const auto& sn : structNames_) {
                bool found = false;
                for (const auto& cn : classNames_) {
                    if (cn == sn) {
                        found = true;
                        break;
                    }
                }
                if (!found) allStructClasses.push_back(sn);
            }
            for (const auto& scName : allStructClasses) {
                auto scSym = symbolTable_.resolve(scName);
                if (!scSym) continue;
                if (scSym->kind != SymbolKind::STRUCT && scSym->kind != SymbolKind::CLASS) continue;
                auto ss = std::static_pointer_cast<StructSymbol>(scSym);
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
                    std::vector<std::string> vtableMethods;
                    for (auto& im : iface->methods) {
                        for (auto& sm : ss->methods) {
                            if (sm.name == im.name) {
                                vtableMethods.push_back(sm.mangledName);
                                break;
                            }
                        }
                    }
                    module_.addVTable(ifaceName + "_" + scName, vtableMethods);
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

    struct DestructorInfo {
        std::string mangledName;
        IRValue varAddr;
    };

    void emitDefers() {
        // Emit all defers in reverse order of scopes, and reverse order within scope
        for (auto it = deferStack_.rbegin(); it != deferStack_.rend(); ++it) {
            for (auto s_it = it->rbegin(); s_it != it->rend(); ++s_it) {
                (*s_it)->accept(*this);
            }
        }
    }

    void emitDestructors() {
        for (auto it = destructorStack_.rbegin(); it != destructorStack_.rend(); ++it) {
            for (auto d_it = it->rbegin(); d_it != it->rend(); ++d_it) {
                IRInstruction call;
                call.op = IROp::CALL;
                call.dest = std::nullopt;
                call.args.push_back(IRValue::Label(d_it->mangledName));
                call.args.push_back(d_it->varAddr);
                currentBlock_->addInstruction(call);
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
        if (name == "bool") return IRType::I32;
        if (name == "void") return IRType::VOID;
        // Resolve type aliases through symbol table
        auto typeSym = symbolTable_.resolve(name);
        if (typeSym && typeSym->type.pointerLevel == 0 && !typeSym->type.name.empty()) {
            std::string targetName = typeSym->type.name;
            if (targetName != name) {
                IRType resolved = mapType(targetName);
                if (resolved != IRType::PTR) return resolved;
            }
        }
        return IRType::PTR;
    }

    // True for struct/class by-value returns (and params decaying likewise).
    // These use the hidden-sret-pointer convention so freestanding targets
    // never need a heap `malloc` for `return someStruct;`.
    bool isStructValueType(const TypeInfo& t) {
        if (t.pointerLevel > 0) return false;
        if (t.isArray || t.isInterface || t.isSlice) return false;
        auto sym = symbolTable_.resolve(t.name);
        if (!sym) return false;
        if (sym->kind != SymbolKind::STRUCT && sym->kind != SymbolKind::CLASS) return false;
        if (sym->type.name != sym->name && !sym->type.name.empty()) {
            auto target = symbolTable_.resolve(sym->type.name);
            if (!target || (target->kind != SymbolKind::STRUCT && target->kind != SymbolKind::CLASS))
                return false;
        }
        return true;
    }

    // Visitor methods
    void visit(AstProgram& program) override {
        if (visitedPrograms_.count(&program)) return;
        visitedPrograms_.insert(&program);

        std::string prevPath = currentSourcePath_;
        if (!program.getSourcePath().empty()) {
            currentSourcePath_ = program.getSourcePath();
        }

        for (auto& imported : program.getImportedAsts()) {
            imported->accept(*this);
        }
        for (auto& stmt : program.getStatements()) {
            stmt->accept(*this);
        }

        currentSourcePath_ = prevPath;
    }

    static std::string sanitizeAsmName(const std::string& name) {
        std::string result;
        for (char c : name) {
            if (c == '<') result += "_l_";
            else if (c == '>') result += "_g_";
            else if (c == ',') result += "_c_";
            else result += c;
        }
        return result;
    }

    void visit(FunctionDeclStmt& stmt) override {
        // Skip template function declarations (uninstantiated)
        if (stmt.hasTemplateParams()) return;

        std::string funcName = sanitizeAsmName(stmt.getName());
        if (!currentClassName_.empty()) {
            funcName = currentClassName_ + "_" + funcName;
        }

        if (stmt.isExtern()) {
            module_.addExternFunction(funcName);
            return;
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
        auto prevDestructorStack = destructorStack_;
        bool prevIsSret = currentIsSret_;
        TypeInfo prevReturnType = currentReturnType_;

        TypeInfo funcRetType = TypeInfo::fromAST(stmt.getReturnType());
        // Virtual methods stay on the legacy malloc path so VIRTUAL_CALL
        // callers (which don't pass sret) keep matching the callee.
        bool isVirtMethod = stmt.isVirtual() || stmt.isOverride();
        bool funcIsSret = isStructValueType(funcRetType) && !isVirtMethod;
        currentIsSret_ = funcIsSret;
        currentReturnType_ = funcRetType;
        IRType funcRetIR = funcIsSret ? IRType::VOID : mapType(funcRetType.name);
        currentFunction_ = std::make_unique<FunctionIR>(funcName, funcRetIR);
        currentFunction_->setSourcePath(currentSourcePath_);
        currentBlock_ = std::make_unique<BasicBlock>("entry");

        variableValues_.clear();
        paramNames_.clear();
        tempCount_ = 0;
        labelCount_ = 0;
        breakLabels_.clear();
        continueLabels_.clear();
        deferStack_.clear();
        destructorStack_.clear();
        deferStack_.push_back({});  // Function scope level
        destructorStack_.push_back({});

        symbolTable_.enterScope();

        // Hidden sret pointer for struct-by-value returns (SysV: rdi).
        // The caller allocates the result slot and passes its address first;
        // `return <struct>` memcpys into it instead of malloc'ing.
        if (funcIsSret) {
            currentFunction_->addParam("__sret");
            auto sretSym = std::make_shared<Symbol>();
            sretSym->kind = SymbolKind::VARIABLE;
            sretSym->name = "__sret";
            sretSym->type = {"void", 1};
            symbolTable_.define(sretSym);
            IRValue paramReg = IRValue::Register("__sret", IRType::PTR);
            IRValue addr = IRValue::Register(nextTemp(), IRType::PTR);
            currentBlock_->addInstruction({IROp::ALLOCA, addr, {IRValue::Constant(8, IRType::I32)}});
            currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, paramReg, IRValue::Constant(0, IRType::I32)}});
            variableValues_["__sret"] = addr;
            paramNames_.insert("__sret");
        }

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
            paramNames_.insert(paramName);
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
            if (sym->type.pointerLevel == 0 && !sym->type.isArray && isStructValueType(sym->type)) {
                // Struct-by-value params arrive as a caller pointer
                // (CID-022): copy the pointee into the local slot instead
                // of storing the pointer value as the first field.
                currentBlock_->addInstruction({IROp::MEMCPY, std::nullopt, {addr, paramReg, IRValue::Constant(getTypeSize(sym->type), IRType::I32)}});
            } else {
                currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, paramReg, IRValue::Constant(0, IRType::I32)}});
            }
            variableValues_[param.first] = addr;
            variableValues_[paramRegName] = addr;  // Support both for lookup convenience
            paramNames_.insert(param.first);
        }
        // Pre-pass: params assigned anywhere in the body must not read-through.
        assignedParams_.clear();
        for (auto& s : stmt.getBody()) collectAssignedParams(s.get());

        for (auto& s : stmt.getBody()) {
            s->accept(*this);
        }

        if (currentBlock_ && (currentBlock_->getInstructions().empty() || currentBlock_->getInstructions().back().op != IROp::RET)) {
            emitDefers();
            emitDestructors();
            currentBlock_->addInstruction({IROp::RET, std::nullopt, {}});
        }

        if (currentBlock_) {
            currentFunction_->addBlock(std::move(currentBlock_));
            currentBlock_ = nullptr;
        }
        module_.addFunction(std::move(currentFunction_));
        symbolTable_.exitScope();
        deferStack_.pop_back();
        destructorStack_.pop_back();

        // Restore state
        currentFunction_ = std::move(prevFunction);
        currentBlock_ = std::move(prevBlock);
        variableValues_ = prevVariableValues;
        tempCount_ = prevTempCount;
        labelCount_ = prevLabelCount;
        breakLabels_ = prevBreakLabels;
        continueLabels_ = prevContinueLabels;
        deferStack_ = prevDeferStack;
        destructorStack_ = prevDestructorStack;
        currentIsSret_ = prevIsSret;
        currentReturnType_ = prevReturnType;
    }

    void visit(ReturnStmt& stmt) override {
        if (stmt.getValue()) {
            stmt.getValue()->accept(*this);
            IRValue val = lastValue_;
            TypeInfo retType = lastExprType_;

            // Struct-by-value return via hidden sret pointer: no malloc,
            // freestanding-safe. The caller-provided slot outlives the frame.
            if (currentIsSret_ && isStructValueType(currentReturnType_)) {
                int structSize = getTypeSize(currentReturnType_);
                IRValue sretAddr = variableValues_.count("__sret")
                    ? variableValues_["__sret"]
                    : IRValue::Register("__sret", IRType::PTR);
                IRValue sretPtr = IRValue::Register(nextTemp(), IRType::PTR);
                currentBlock_->addInstruction({IROp::LOAD, sretPtr, {sretAddr, IRValue::Constant(0, IRType::I32)}});
                currentBlock_->addInstruction({IROp::MEMCPY, std::nullopt, {sretPtr, val, IRValue::Constant(structSize, IRType::I32)}});
                lastExprType_ = currentReturnType_;
                emitDefers();
                emitDestructors();
                currentBlock_->addInstruction({IROp::RET, std::nullopt, {}});
                startBlock(nextLabel("dead"));
                return;
            }

            // Legacy heap path for struct values returned from non-sret
            // contexts (should not occur for declared struct returns, which
            // are always sret; kept for robustness).
            if (retType.pointerLevel == 0) {
                int structSize = getTypeSize(retType);
                auto typeSym = symbolTable_.resolve(retType.name);
                if (typeSym && (typeSym->kind == SymbolKind::STRUCT || typeSym->kind == SymbolKind::CLASS)) {
                    IRValue localPtr = IRValue::Register(nextTemp(), IRType::PTR);
                    currentBlock_->addInstruction({IROp::CALL, localPtr, {IRValue::Label("malloc"), IRValue::Constant(structSize, IRType::I32)}});
                    currentBlock_->addInstruction({IROp::MEMCPY, std::nullopt, {localPtr, val, IRValue::Constant(structSize, IRType::I32)}});
                    val = localPtr;
                    lastExprType_.isUserDefined = true;
                }
            }

            emitDefers();
            emitDestructors();
            currentBlock_->addInstruction({IROp::RET, std::nullopt, {val}});
        } else {
            emitDefers();
            emitDestructors();
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
        std::shared_ptr<FunctionSymbol> resolvedFuncSym = nullptr;

        if (auto id = dynamic_cast<IdentifierExpr*>(calleeExpr)) {
            std::string lookupName = id->getName();
            auto sym = symbolTable_.resolve(lookupName);
            if (sym && sym->kind == SymbolKind::FUNCTION) {
                funcName = sanitizeAsmName(lookupName);
                resolvedFuncSym = std::static_pointer_cast<FunctionSymbol>(sym);
            }
        } else if (auto mem = dynamic_cast<MemberAccessExpr*>(calleeExpr)) {
            // Method call: obj.method() or obj->method()
            bool wasAddress = isAddress_;
            isAddress_ = false;
            mem->getObject()->accept(*this);
            objPtr = lastValue_;
            isAddress_ = wasAddress;

            TypeInfo objType = lastExprType_;
            if (objType.pointerLevel > 0) objType.pointerLevel--;

            // Check if object is an interface type -> interface method call
            if (objType.isInterface) {
                auto ifaceSym = symbolTable_.resolve(objType.name);
                if (ifaceSym && ifaceSym->kind == SymbolKind::INTERFACE) {
                    auto iface = std::static_pointer_cast<InterfaceSymbol>(ifaceSym);
                    int methodIdx = 0;
                    bool found = false;
                    for (auto& m : iface->methods) {
                        if (m.name == mem->getMember()) {
                            found = true;
                            // objPtr points to the fat pointer (address of interface variable)
                            // Load data_ptr from [objPtr + 0]
                            IRValue dataPtrReg = IRValue::Register(nextTemp(), IRType::PTR);
                            currentBlock_->addInstruction({IROp::LOAD, dataPtrReg, {objPtr, IRValue::Constant(0, IRType::I32)}});
                            // Load vtable_ptr from [objPtr + 8]
                            IRValue vtablePtrReg = IRValue::Register(nextTemp(), IRType::PTR);
                            currentBlock_->addInstruction({IROp::LOAD, vtablePtrReg, {objPtr, IRValue::Constant(8, IRType::I32)}});
                            // Load method address from vtable[methodIdx]
                            IRValue methodAddrReg = IRValue::Register(nextTemp(), IRType::PTR);
                            currentBlock_->addInstruction({IROp::LOAD, methodAddrReg, {vtablePtrReg, IRValue::Constant(methodIdx * 8, IRType::I32)}});
                            // CALL through method address with data_ptr as 'this'
                            std::vector<IRValue> args;
                            args.push_back(methodAddrReg);
                            args.push_back(dataPtrReg);
                            for (auto& arg : expr.getArgs()) {
                                arg->accept(*this);
                                args.push_back(lastValue_);
                            }
                            IRType retIRType = (m.returnType.pointerLevel > 0) ? IRType::PTR : mapType(m.returnType.name);
                            IRValue dest = IRValue::Register(nextTemp(), retIRType);
                            IRInstruction callInstr;
                            callInstr.op = IROp::CALL;
                            callInstr.dest = dest;
                            callInstr.args = std::move(args);
                            currentBlock_->addInstruction(callInstr);
                            lastValue_ = dest;
                            lastExprType_ = m.returnType;
                            return;
                        }
                        methodIdx++;
                    }
                    if (!found) {
                        funcName = mem->getMember();
                    }
                    return;
                }
            }

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

            auto sym = resolvedFuncSym;
            if (!sym) {
                auto rawSym = symbolTable_.resolve(funcName);
                if (rawSym && rawSym->kind == SymbolKind::FUNCTION)
                    sym = std::static_pointer_cast<FunctionSymbol>(rawSym);
            }
            TypeInfo calleeRet = {"int", 0};
            if (sym && sym->kind == SymbolKind::FUNCTION) {
                auto funcSym = std::static_pointer_cast<FunctionSymbol>(sym);
                calleeRet = funcSym->returnType;
                lastExprType_ = calleeRet;
            }
            bool calleeIsSret = isStructValueType(calleeRet);
            // Virtual callees are never sret (they stay on the malloc path);
            // direct CALLs here are free functions / non-virtual methods.
            if (calleeIsSret && !(isVirtual && vtableIndex != -1)) {
                int retSize = getTypeSize(calleeRet);
                IRValue retBuf = IRValue::Register(nextTemp(), IRType::PTR);
                currentBlock_->addInstruction({IROp::ALLOCA, retBuf, {IRValue::Constant(retSize, IRType::I32)}});
                IRInstruction callInstr;
                callInstr.op = IROp::CALL;
                callInstr.dest = std::nullopt;
                callInstr.args.push_back(IRValue::Label(funcName));
                callInstr.args.push_back(retBuf);
                bool hasObj = (objPtr.kind != IRValue::CONSTANT || objPtr.intValue != 0);
                if (hasObj) {
                    callInstr.args.push_back(objPtr);
                }
                for (auto& a : args) callInstr.args.push_back(a);
                currentBlock_->addInstruction(callInstr);
                lastValue_ = retBuf;
                lastExprType_ = calleeRet;
                lastExprType_.isUserDefined = true;
                return;
            }
            IRType retIRType = IRType::I32;
            if (sym && sym->kind == SymbolKind::FUNCTION) {
                auto funcSym = std::static_pointer_cast<FunctionSymbol>(sym);
                retIRType = (funcSym->returnType.pointerLevel > 0) ? IRType::PTR : mapType(funcSym->returnType.name);
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
                if (objPtr.kind != IRValue::CONSTANT || objPtr.intValue != 0) {
                    callInstr.args.push_back(objPtr);
                }
                for (auto& a : args) callInstr.args.push_back(a);
            }

            callInstr.dest = dest;
            currentBlock_->addInstruction(callInstr);
            lastValue_ = dest;
        } else {
            // Function pointer / closure call: callee is an expression.
            bool wasAddress = isAddress_;
            isAddress_ = false;
            calleeExpr->accept(*this);
            isAddress_ = wasAddress;
            IRValue fnPtr = lastValue_;
            TypeInfo calleeType = lastExprType_;

            std::vector<IRValue> args;
            for (auto& arg : expr.getArgs()) {
                arg->accept(*this);
                args.push_back(lastValue_);
            }

            if (calleeType.name == "func" && calleeType.pointerLevel >= 1) {
                // Closure call: the value is a closure object {fnptr,
                // captures...} — load the fnptr and pass the object first.
                IRValue obj = fnPtr;
                fnPtr = IRValue::Register(nextTemp(), IRType::PTR);
                currentBlock_->addInstruction({IROp::LOAD, fnPtr, {obj, IRValue::Constant(0, IRType::I32)}});
                IRValue dest = IRValue::Register(nextTemp(), IRType::I32);
                IRInstruction callInstr;
                callInstr.op = IROp::CALL;
                callInstr.args.push_back(fnPtr);
                callInstr.args.push_back(obj);
                for (auto& a : args) callInstr.args.push_back(a);
                callInstr.dest = dest;
                currentBlock_->addInstruction(callInstr);
                lastValue_ = dest;
                lastExprType_ = {"int", 0};
            } else {
                IRValue dest = IRValue::Register(nextTemp(), IRType::I32);
                IRInstruction callInstr;
                callInstr.op = IROp::CALL;
                callInstr.args.push_back(fnPtr);
                for (auto& a : args) callInstr.args.push_back(a);
                callInstr.dest = dest;
                currentBlock_->addInstruction(callInstr);
                lastValue_ = dest;
                lastExprType_ = {"int", 0};
            }
        }
    }

    // --- closures ---

    void collectCapturesExpr(Expression* e, const std::set<std::string>& params,
                             std::vector<std::string>& out) {
        if (!e) return;
        if (auto id = dynamic_cast<IdentifierExpr*>(e)) {
            const std::string& n = id->getName();
            if (!params.count(n) && variableValues_.count(n) &&
                std::find(out.begin(), out.end(), n) == out.end()) {
                out.push_back(n);
            }
        } else if (auto call = dynamic_cast<CallExpr*>(e)) {
            collectCapturesExpr(call->getCallee(), params, out);
            for (auto& a : call->getArgs()) collectCapturesExpr(a.get(), params, out);
        } else if (auto bin = dynamic_cast<BinaryExpr*>(e)) {
            collectCapturesExpr(bin->getLeft(), params, out);
            collectCapturesExpr(bin->getRight(), params, out);
        } else if (auto un = dynamic_cast<UnaryExpr*>(e)) {
            collectCapturesExpr(un->getOperand(), params, out);
        } else if (auto tern = dynamic_cast<TernaryExpr*>(e)) {
            collectCapturesExpr(tern->getCondition(), params, out);
            collectCapturesExpr(tern->getThenExpr(), params, out);
            collectCapturesExpr(tern->getElseExpr(), params, out);
        } else if (auto group = dynamic_cast<GroupingExpr*>(e)) {
            collectCapturesExpr(group->getExpression(), params, out);
        } else if (auto mem = dynamic_cast<MemberAccessExpr*>(e)) {
            collectCapturesExpr(mem->getObject(), params, out);
        } else if (auto idx = dynamic_cast<IndexExpr*>(e)) {
            collectCapturesExpr(idx->getObject(), params, out);
            collectCapturesExpr(idx->getIndex(), params, out);
        } else if (auto cast = dynamic_cast<CastExpr*>(e)) {
            collectCapturesExpr(cast->getExpression(), params, out);
        } else if (auto interp = dynamic_cast<InterpolatedStringExpr*>(e)) {
            for (const auto& p : interp->getParts()) {
                if (p.isExpr) collectCapturesExpr(p.expr.get(), params, out);
            }
        }
    }

    void collectCapturesStmt(Statement* s, const std::set<std::string>& params,
                             std::vector<std::string>& out) {
        if (!s) return;
        switch (s->getType()) {
            case ASTNode::EXPRESSION_STMT:
                collectCapturesExpr(dynamic_cast<ExpressionStmt*>(s)->getExpression(), params, out);
                break;
            case ASTNode::RETURN_STMT: {
                auto rs = dynamic_cast<ReturnStmt*>(s);
                if (rs->getValue()) collectCapturesExpr(rs->getValue(), params, out);
                break;
            }
            case ASTNode::VAR_DECL_STMT: {
                auto vs = dynamic_cast<VarDeclStmt*>(s);
                if (vs->getInitializer()) collectCapturesExpr(vs->getInitializer(), params, out);
                break;
            }
            case ASTNode::ASSIGN_STMT: {
                auto as = dynamic_cast<AssignStmt*>(s);
                collectCapturesExpr(as->getTarget(), params, out);
                collectCapturesExpr(as->getValue(), params, out);
                break;
            }
            case ASTNode::IF_STMT: {
                auto is = dynamic_cast<IfStmt*>(s);
                collectCapturesExpr(is->getCondition(), params, out);
                for (auto& b : is->getThenBranch()) collectCapturesStmt(b.get(), params, out);
                for (auto& b : is->getElseBranch()) collectCapturesStmt(b.get(), params, out);
                break;
            }
            case ASTNode::WHILE_STMT: {
                auto ws = dynamic_cast<WhileStmt*>(s);
                collectCapturesExpr(ws->getCondition(), params, out);
                for (auto& b : ws->getBody()) collectCapturesStmt(b.get(), params, out);
                break;
            }
            case ASTNode::BLOCK_STMT:
                for (auto& b : dynamic_cast<BlockStmt*>(s)->getStatements()) {
                    collectCapturesStmt(b.get(), params, out);
                }
                break;
            case ASTNode::FOR_STMT: {
                auto fs = dynamic_cast<ForStmt*>(s);
                if (fs->getInitializer()) collectCapturesStmt(fs->getInitializer(), params, out);
                if (fs->getCondition()) collectCapturesExpr(fs->getCondition(), params, out);
                if (fs->getIncrement()) collectCapturesStmt(fs->getIncrement(), params, out);
                for (auto& b : fs->getBody()) collectCapturesStmt(b.get(), params, out);
                break;
            }
            case ASTNode::FOR_EACH_STMT: {
                auto fe = dynamic_cast<ForEachStmt*>(s);
                collectCapturesExpr(fe->getIterable(), params, out);
                for (auto& b : fe->getBody()) collectCapturesStmt(b.get(), params, out);
                break;
            }
            default:
                break;
        }
    }

    void visit(FunctionExpr& expr) override {
        if (!currentFunction_) {
            lastValue_ = IRValue::Constant(0, IRType::PTR);
            lastExprType_ = {"func", 1};
            return;
        }

        std::set<std::string> paramNames;
        for (auto& p : expr.getParams()) paramNames.insert(p.first);
        std::vector<std::string> captures;
        for (auto& s : expr.getBody()) collectCapturesStmt(s.get(), paramNames, captures);

        std::string funcName = "_closure_" + std::to_string(closureCount_++);
        TypeInfo retType = expr.getReturnType() ? TypeInfo::fromAST(expr.getReturnType()) : TypeInfo{"int", 0};
        IRType retIR = (retType.pointerLevel > 0) ? IRType::PTR : mapType(retType.name);

        // Synthesize the closure function: the hidden first parameter is the
        // closure object; captures are read as its fields.
        auto prevFunction = std::move(currentFunction_);
        auto prevBlock = std::move(currentBlock_);
        auto prevVariableValues = variableValues_;
        int prevTempCount = tempCount_;
        int prevLabelCount = labelCount_;
        auto prevBreakLabels = breakLabels_;
        auto prevContinueLabels = continueLabels_;
        auto prevDeferStack = deferStack_;
        auto prevDestructorStack = destructorStack_;
        bool prevIsSret = currentIsSret_;
        TypeInfo prevReturnType = currentReturnType_;
        // Closures are invoked via function pointers (non-sret convention),
        // so struct returns inside use the legacy heap path.
        currentIsSret_ = false;
        currentReturnType_ = retType;

        currentFunction_ = std::make_unique<FunctionIR>(funcName, retIR);
        currentFunction_->setSourcePath(currentSourcePath_);
        currentBlock_ = std::make_unique<BasicBlock>("entry");
        variableValues_.clear();
        paramNames_.clear();
        tempCount_ = 0;
        labelCount_ = 0;
        breakLabels_.clear();
        continueLabels_.clear();
        deferStack_.clear();
        destructorStack_.clear();
        deferStack_.push_back({});
        destructorStack_.push_back({});

        symbolTable_.enterScope();
        // Hidden closure parameter.
        currentFunction_->addParam("arg__closure");
        {
            auto sym = std::make_shared<Symbol>();
            sym->kind = SymbolKind::VARIABLE;
            sym->name = "this";
            sym->type = {"void", 1};
            symbolTable_.define(sym);
            IRValue paramReg = IRValue::Register("arg__closure", IRType::PTR);
            IRValue addr = IRValue::Register(nextTemp(), IRType::PTR);
            currentBlock_->addInstruction({IROp::ALLOCA, addr, {IRValue::Constant(8, IRType::I32)}});
            currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, paramReg, IRValue::Constant(0, IRType::I32)}});
            variableValues_["__closure"] = addr;
            paramNames_.insert("arg__closure");
        }
        // Captured variables are addressed as closure-object fields.
        for (size_t ci = 0; ci < captures.size(); ++ci) {
            IRValue obj = IRValue::Register(nextTemp(), IRType::PTR);
            currentBlock_->addInstruction({IROp::LOAD, obj, {variableValues_["__closure"], IRValue::Constant(0, IRType::I32)}});
            IRValue field = IRValue::Register(nextTemp(), IRType::PTR);
            currentBlock_->addInstruction({IROp::ADD, field, {obj, IRValue::Constant(8 + 8 * (int)ci, IRType::I32)}});
            variableValues_[captures[ci]] = field;
        }
        // Typed parameters.
        for (auto& p : expr.getParams()) {
            TypeInfo pt = p.second ? TypeInfo::fromAST(p.second.get()) : TypeInfo{"int", 0};
            std::string paramName = "arg_" + p.first;
            currentFunction_->addParam(paramName);
            auto sym = std::make_shared<Symbol>();
            sym->kind = SymbolKind::VARIABLE;
            sym->name = p.first;
            sym->type = pt;
            symbolTable_.define(sym);
            IRType paramIRType = (pt.pointerLevel > 0) ? IRType::PTR : mapType(pt.name);
            IRValue paramReg = IRValue::Register(paramName, paramIRType);
            IRValue addr = IRValue::Register(nextTemp(), IRType::PTR);
            currentBlock_->addInstruction({IROp::ALLOCA, addr, {IRValue::Constant(8, IRType::I32)}});
            currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, paramReg, IRValue::Constant(0, IRType::I32)}});
            variableValues_[p.first] = addr;
            paramNames_.insert(paramName);
        }
        for (auto& s : expr.getBody()) {
            s->accept(*this);
        }
        symbolTable_.exitScope();

        currentFunction_->addBlock(std::move(currentBlock_));
        module_.addFunction(std::move(currentFunction_));
        currentFunction_ = std::move(prevFunction);
        currentBlock_ = std::move(prevBlock);
        variableValues_ = std::move(prevVariableValues);
        tempCount_ = prevTempCount;
        labelCount_ = prevLabelCount;
        breakLabels_ = std::move(prevBreakLabels);
        continueLabels_ = std::move(prevContinueLabels);
        deferStack_ = std::move(prevDeferStack);
        destructorStack_ = std::move(prevDestructorStack);
        currentIsSret_ = prevIsSret;
        currentReturnType_ = prevReturnType;

        // Closure object: { fnptr, cap_0, ... } — the escape analysis turns
        // the malloc into a stack slot when the closure never leaves.
        int objSize = 8 + 8 * (int)captures.size();
        IRValue obj = IRValue::Register(nextTemp(), IRType::PTR);
        currentBlock_->addInstruction({IROp::CALL, obj, {IRValue::Label("malloc"), IRValue::Constant(objSize, IRType::I32)}});
        currentBlock_->addInstruction({IROp::STORE, std::nullopt, {obj, IRValue::Label(funcName), IRValue::Constant(0, IRType::I32)}});
        for (size_t ci = 0; ci < captures.size(); ++ci) {
            IdentifierExpr ident(captures[ci], 0, 0);
            ident.accept(*this);
            IRValue capVal = lastValue_;
            IRValue capSlot = IRValue::Register(nextTemp(), IRType::PTR);
            currentBlock_->addInstruction({IROp::ADD, capSlot, {obj, IRValue::Constant(8 + 8 * (int)ci, IRType::I32)}});
            currentBlock_->addInstruction({IROp::STORE, std::nullopt, {capSlot, capVal, IRValue::Constant(0, IRType::I32)}});
        }
        lastValue_ = obj;
        lastExprType_ = {"func", 1};
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

                    bool fieldIsStruct = (field.type.pointerLevel == 0);
                    if (fieldIsStruct) {
                        auto fieldTypeSym = symbolTable_.resolve(field.type.name);
                        if (!fieldTypeSym || (fieldTypeSym->kind != SymbolKind::STRUCT && fieldTypeSym->kind != SymbolKind::CLASS)) {
                            fieldIsStruct = false;
                        }
                    }
                    // Array-typed fields are address-like (like locals):
                    // indexing them must operate on the array's location,
                    // not on a loaded copy of its first element.
                    bool fieldIsArray = field.type.isArray;
                    if (field.bitWidth > 0 && !isAddress_) {
                        bool isSigned = (field.type.name == "int" || field.type.name == "i8" ||
                                         field.type.name == "i16" || field.type.name == "i32");
                        lastValue_ = emitBitfieldLoad(addr, field.bitOffset, field.bitWidth, isSigned);
                    } else if (isAddress_ || fieldIsStruct || fieldIsArray) {
                        lastValue_ = addr;
                    } else {
                        IRType fieldIRType = (field.type.pointerLevel > 0) ? IRType::PTR : mapType(field.type.name);
                        IRValue dest = IRValue::Register(nextTemp(), fieldIRType);
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
                TypeInfo fieldType;
                for (const auto& f : structSym->fields) {
                    if (f.name == fieldInit.first) {
                        offset = f.offset;
                        fieldType = f.type;
                        break;
                    }
                }
                val = truncateIntToType(val, fieldType);
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
            lastExprType_ = type;
        } else if (expr.getOperator() == "*") {
            expr.getOperand()->accept(*this);
            IRValue operand = lastValue_;
            type = lastExprType_;
            if (isAddress_) {
                lastValue_ = operand;
            } else {
                // Dereferencing a pointer-to-struct/array yields the object
                // itself, whose address is already `operand`. Emitting a LOAD
                // here would read the first 8 bytes as a pointer and break
                // `var q: T = *p;` (Issue #12). Only scalar/pointer pointee
                // types go through LOAD.
                TypeInfo pointee = type;
                if (pointee.pointerLevel > 0) pointee.pointerLevel--;
                bool pointeeIsAggregate = (pointee.pointerLevel == 0) &&
                    (pointee.isArray || isStructValueType(pointee));
                if (pointeeIsAggregate) {
                    lastValue_ = operand;
                    type = pointee;
                    lastExprType_ = type;
                    return;
                }
                IRType derefType = (type.pointerLevel > 1) ? IRType::PTR : mapType(type.name);
                IRValue dest = IRValue::Register(nextTemp(), derefType);
                currentBlock_->addInstruction({IROp::LOAD, dest, {operand, IRValue::Constant(0, IRType::I32)}});
                lastValue_ = dest;
            }
            if (type.pointerLevel > 0) type.pointerLevel--;
            lastExprType_ = type;
        } else if (expr.getOperator() == "++" || expr.getOperator() == "--") {
            bool isPostfix = expr.isPostfix();
            isAddress_ = true;
            expr.getOperand()->accept(*this);
            IRValue addr = lastValue_;
            isAddress_ = false;
            TypeInfo operandType = lastExprType_;

            IRType irType = (operandType.pointerLevel > 0) ? IRType::PTR : mapType(operandType.name);
            IRValue current = IRValue::Register(nextTemp(), irType);
            currentBlock_->addInstruction({IROp::LOAD, current, {addr, IRValue::Constant(0, IRType::I32)}});

            IRValue one = IRValue::Constant(1, irType);
            IRValue result = IRValue::Register(nextTemp(), irType);
            currentBlock_->addInstruction({expr.getOperator() == "++" ? IROp::ADD : IROp::SUB, result, {current, one}});

            if (addr.kind == IRValue::LABEL) {
                currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, result}});
            } else {
                currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, result, IRValue::Constant(0, IRType::I32)}});
            }

            type = operandType;
            // Postfix yields the original value; prefix yields the new value.
            lastValue_ = isPostfix ? current : result;
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
        if (!sym->type.isInterface && sym->type.pointerLevel == 0) {
            auto typeSym = symbolTable_.resolve(sym->type.name);
            if (typeSym && typeSym->kind == SymbolKind::INTERFACE) {
                sym->type.isInterface = true;
            }
        }
        symbolTable_.define(sym);

        if (!currentFunction_) {
            if (auto strInit = dynamic_cast<StringExpr*>(stmt.getInitializer())) {
                std::string strLabel = "_gs" + std::to_string(globalStrCount_++);
                std::string strData = strInit->getValue();
                std::string dataLine = strLabel + ": db ";
                for (size_t ci = 0; ci < strData.size(); ++ci) {
                    if (ci > 0) dataLine += ", ";
                    char c = strData[ci];
                    if (c == '\n') dataLine += "10";
                    else if (c == '\t') dataLine += "9";
                    else if (c == '\\') dataLine += "'\\\\'";
                    else if (c == '"') dataLine += "'\\\"'";
                    else if (c >= 32 && c < 127) dataLine += "'" + std::string(1, c) + "'";
                    else dataLine += std::to_string((unsigned char)c);
                }
                dataLine += ", 0";
                module_.addStringData(dataLine);
                module_.addStringData(stmt.getName() + ": dq " + strLabel);
                globalValues_[stmt.getName()] = IRValue::Label(stmt.getName());
                return;
            }
            int64_t initVal = 0;
            if (stmt.getInitializer()) {
                if (auto num = dynamic_cast<NumberExpr*>(stmt.getInitializer())) {
                    initVal = (int64_t)num->getUIntValue();
                } else {
                    // Constant expressions (CID-014): fold `1 << 4`,
                    // `0x10|0x20`, `~0x10`, ... so they land in .data
                    // instead of silently reading 0 from .bss.
                    uint64_t folded = 0;
                    if (tryEvalConstU64(stmt.getInitializer(), folded)) {
                        initVal = (int64_t)folded;
                    }
                }
            }
            int totalBytes = getTypeSize(sym->type);
            module_.addGlobalVar(stmt.getName(), (sym->type.pointerLevel > 0) ? IRType::PTR : mapType(sym->type.name), initVal, sym->type.isArray ? sym->type.arraySize : 0, totalBytes);
            globalValues_[stmt.getName()] = IRValue::Label(stmt.getName());
            return;
        }

        IRValue addr = IRValue::Register(nextTemp(), IRType::PTR);
        currentBlock_->addInstruction({IROp::ALLOCA, addr, {IRValue::Constant(getTypeSize(sym->type), IRType::I32)}});
        variableValues_[stmt.getName()] = addr;

        // Check if this type has a destructor
        TypeInfo varType = sym->type;
        if (varType.pointerLevel == 0 && !varType.isArray) {
            auto typeSym = symbolTable_.resolve(varType.name);
            if (typeSym && typeSym->kind == SymbolKind::CLASS) {
                auto classSym = std::static_pointer_cast<StructSymbol>(typeSym);
                if (!classSym->destructorName.empty()) {
                    destructorStack_.back().push_back({classSym->destructorName, addr});
                }
            }
        }

        if (stmt.getInitializer()) {
            // Handle interface variable initialization: assign concrete type to interface
            bool isInterfaceInit = false;
            TypeInfo initTypeInfo;
            if (sym->type.isInterface) {
                stmt.getInitializer()->accept(*this);
                initTypeInfo = lastExprType_;
                IRValue srcAddr = lastValue_;
                // Store data pointer at [addr + 0]
                currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, srcAddr, IRValue::Constant(0, IRType::I32)}});
                // Compute [addr + 8] in a separate register to avoid corrupting addr
                IRValue addr8 = IRValue::Register(nextTemp(), IRType::PTR);
                currentBlock_->addInstruction({IROp::ADD, addr8, {addr, IRValue::Constant(8, IRType::I32)}});
                // Store vtable pointer at [addr + 8]
                std::string concreteTypeName = initTypeInfo.name;
                if (initTypeInfo.pointerLevel > 0) {
                    TypeInfo base = initTypeInfo;
                    base.pointerLevel--;
                    concreteTypeName = base.name;
                }
                std::string vtableLabel = "vtable_" + sym->type.name + "_" + concreteTypeName;
                currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr8, IRValue::Label(vtableLabel), IRValue::Constant(0, IRType::I32)}});
                lastExprType_ = sym->type;
                isInterfaceInit = true;
            }
            if (!isInterfaceInit) {
                stmt.getInitializer()->accept(*this);
                IRValue val = lastValue_;
                if (sym->type.pointerLevel == 0) {
                    IRType declType = mapType(sym->type.name);
                    if ((val.type == IRType::I32 || val.type == IRType::I64) &&
                        (declType == IRType::F32 || declType == IRType::F64)) {
                        IRValue converted = IRValue::Register(nextTemp(), declType);
                        currentBlock_->addInstruction({IROp::CVT_ITOF, converted, {val}});
                        val = converted;
                    } else if ((val.type == IRType::F32 || val.type == IRType::F64) &&
                               (declType == IRType::I32 || declType == IRType::I64)) {
                        IRValue converted = IRValue::Register(nextTemp(), declType);
                        currentBlock_->addInstruction({IROp::CVT_FTOI, converted, {val}});
                        val = converted;
                    }
                }
                bool isStructType = isStructValueType(sym->type);
                bool initIsStruct = (lastExprType_.pointerLevel == 0) && (isStructValueType(lastExprType_) || lastExprType_.isUserDefined ||
                                    lastExprType_.isArray || getTypeSize(lastExprType_) > 8);
                if (sym->type.pointerLevel == 0 && (sym->type.isArray || sym->type.isUserDefined || isStructType || getTypeSize(sym->type) > 8 || initIsStruct)) {
                    currentBlock_->addInstruction({IROp::MEMCPY, std::nullopt, {addr, val, IRValue::Constant(getTypeSize(sym->type), IRType::I32)}});
                } else {
                    // Store at the declared width (same rule as fields and
                    // elements): a narrow initializer in a wide local slot
                    // (all slots are 8 bytes) leaves stale upper bytes that
                    // a later wide load reads as garbage — e.g. a limiting
                    // `var big: i64 = 0 - 5` compared against another i64.
                    if (sym->type.pointerLevel == 0 && !sym->type.isArray) {
                        IRType declIR = mapType(sym->type.name);
                        bool declIsInt = (declIR == IRType::I8 || declIR == IRType::U8 ||
                                          declIR == IRType::I16 || declIR == IRType::U16 ||
                                          declIR == IRType::I32 || declIR == IRType::U32 ||
                                          declIR == IRType::BOOL ||
                                          declIR == IRType::I64 || declIR == IRType::U64);
                        if (declIsInt) val = truncateIntToType(val, sym->type);
                    }
                    currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, val, IRValue::Constant(0, IRType::I32)}});
                }
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

        if (type.isInterface) return 16;
        auto sym = symbolTable_.resolve(type.name);
        if (sym && (sym->kind == SymbolKind::STRUCT || sym->kind == SymbolKind::CLASS)) {
            return std::static_pointer_cast<StructSymbol>(sym)->totalSize;
        }
        if (sym && sym->kind == SymbolKind::INTERFACE) return 16;
        return 8;  // Default
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
            std::string trueEndName = currentBlock_->getName();
            startBlock(scFalseName);
            IRValue falseVal = IRValue::Constant(0, IRType::BOOL);
            currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(scMergeName)}});
            std::string falseEndName = currentBlock_->getName();
            startBlock(scMergeName);
            IRValue phiDest = IRValue::Register(nextTemp(), IRType::BOOL);
            IRInstruction phiInstr;
            phiInstr.op = IROp::PHI;
            phiInstr.dest = phiDest;
            phiInstr.phiArgs[trueEndName] = rightVal;
            phiInstr.phiArgs[falseEndName] = falseVal;
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
            std::string trueEndName = currentBlock_->getName();
            startBlock(scFalseName);
            expr.getRight()->accept(*this);
            IRValue rightVal = lastValue_;
            TypeInfo rightType = lastExprType_;
            currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(scMergeName)}});
            std::string falseEndName = currentBlock_->getName();
            startBlock(scMergeName);
            IRValue phiDest = IRValue::Register(nextTemp(), IRType::BOOL);
            IRInstruction phiInstr;
            phiInstr.op = IROp::PHI;
            phiInstr.dest = phiDest;
            phiInstr.phiArgs[trueEndName] = trueVal;
            phiInstr.phiArgs[falseEndName] = rightVal;
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

        // Implicit int-to-float conversion for mixed operands
        if ((left.type == IRType::F32 || left.type == IRType::F64) &&
            (right.type == IRType::I32 || right.type == IRType::I64)) {
            IRValue converted = IRValue::Register(nextTemp(), left.type);
            currentBlock_->addInstruction({IROp::CVT_ITOF, converted, {right}});
            right = converted;
        } else if ((right.type == IRType::F32 || right.type == IRType::F64) &&
                   (left.type == IRType::I32 || left.type == IRType::I64)) {
            IRValue converted = IRValue::Register(nextTemp(), right.type);
            currentBlock_->addInstruction({IROp::CVT_ITOF, converted, {left}});
            left = converted;
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
        if (op >= IROp::CMP_EQ && op <= IROp::CMP_GE) {
            dest.type = IRType::I32;
        }
        currentBlock_->addInstruction({op, dest, {left, right}});
        lastValue_ = dest;
        lastExprType_ = (op >= IROp::CMP_EQ && op <= IROp::CMP_GE) ? TypeInfo{"bool", 0} : leftType;
    }

    void visit(NumberExpr& expr) override {
        if (expr.isFloat()) {
            double val = expr.getFloatValue();
            lastExprType_ = {"float", 0};
            lastValue_ = IRValue::Constant(val, IRType::F32);
        } else {
            uint64_t v = expr.getUIntValue();
            // Select the narrowest type that holds the literal: small values
            // stay I32 ("int") for backward compatibility; values with bit 31
            // set become U32 (zero-extended, never sign-extended); larger
            // values become U64/I64 so 64-bit addresses survive.
            if (v <= (uint64_t)INT32_MAX) {
                lastExprType_ = {"int", 0};
                lastValue_ = IRValue::Constant((int64_t)v, IRType::I32);
            } else if (v <= (uint64_t)UINT32_MAX) {
                lastExprType_ = {"u32", 0};
                lastValue_ = IRValue::Constant(v, IRType::U32);
            } else if (v <= (uint64_t)INT64_MAX) {
                lastExprType_ = {"u64", 0};
                lastValue_ = IRValue::Constant((int64_t)v, IRType::I64);
            } else {
                lastExprType_ = {"u64", 0};
                lastValue_ = IRValue::Constant(v, IRType::U64);
            }
        }
    }

    void visit(CharExpr& expr) override {
        lastValue_ = IRValue::Constant(expr.getValue(), IRType::U8);
        lastExprType_ = {"char", 0};
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
            if (lastExprType_.pointerLevel == 0) {
                auto typeSym = symbolTable_.resolve(lastExprType_.name);
                if (typeSym && (typeSym->kind == SymbolKind::STRUCT || typeSym->kind == SymbolKind::CLASS)) {
                    lastExprType_.isUserDefined = typeSym->type.name.empty();
                }
            } else {
                lastExprType_.isUserDefined = false;
            }
        }

        if (sym && sym->isConst) {
            IRType irType = (sym->type.pointerLevel > 0) ? IRType::PTR : mapType(sym->type.name);
            lastValue_ = IRValue::Constant(sym->constValue, irType);
            return;
        }

        if (sym && sym->kind == SymbolKind::FUNCTION) {
            lastValue_ = IRValue::Label(sym->name);
            lastExprType_ = {"void", 1};
            return;
        }

        if (isAddress_) {
            if (variableValues_.count(expr.getName())) {
                lastValue_ = variableValues_[expr.getName()];
                return;
            }
            if (globalValues_.count(expr.getName())) {
                lastValue_ = globalValues_[expr.getName()];
                return;
            }
        }

        if (lastExprType_.isArray || lastExprType_.isUserDefined || lastExprType_.isInterface) {
            lastValue_ = getAddress(&expr);
            return;
        }

        if (variableValues_.count(expr.getName())) {
            if (paramNames_.count(expr.getName()) && !assignedParams_.count(expr.getName())) {
                IRType irType = (lastExprType_.pointerLevel > 0) ? IRType::PTR : mapType(lastExprType_.name);
                lastValue_ = IRValue::Register("arg_" + expr.getName(), irType);
            } else {
                IRValue addr = variableValues_[expr.getName()];
                IRType irType = (lastExprType_.pointerLevel > 0) ? IRType::PTR : mapType(lastExprType_.name);
                IRValue dest = IRValue::Register(nextTemp(), irType);
                currentBlock_->addInstruction({IROp::LOAD, dest, {addr, IRValue::Constant(0, IRType::I32)}});
                lastValue_ = dest;
            }
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

    // Pre-pass: collect every parameter that is assigned ANYWHERE in the
    // function body. Read-through (param as register) is only sound when
    // the param is never written — otherwise a read in an earlier block
    // (e.g. a loop header generated before the assignment) would keep
    // reading the stale original register.
    void collectAssignedParams(Statement* stmt) {
        if (!stmt) return;
        if (auto assign = dynamic_cast<AssignStmt*>(stmt)) {
            if (auto ident = dynamic_cast<IdentifierExpr*>(assign->getTarget())) {
                assignedParams_.insert(ident->getName());
            }
        } else if (auto iff = dynamic_cast<IfStmt*>(stmt)) {
            for (auto& s : iff->getThenBranch()) collectAssignedParams(s.get());
            for (auto& s : iff->getElseBranch()) collectAssignedParams(s.get());
        } else if (auto wh = dynamic_cast<WhileStmt*>(stmt)) {
            for (auto& s : wh->getBody()) collectAssignedParams(s.get());
        } else if (auto blk = dynamic_cast<BlockStmt*>(stmt)) {
            for (auto& s : blk->getStatements()) collectAssignedParams(s.get());
        } else if (auto fr = dynamic_cast<ForStmt*>(stmt)) {
            collectAssignedParams(fr->getInitializer());
            collectAssignedParams(fr->getIncrement());
            for (auto& s : fr->getBody()) collectAssignedParams(s.get());
        } else if (auto fe = dynamic_cast<ForEachStmt*>(stmt)) {
            for (auto& s : fe->getBody()) collectAssignedParams(s.get());
        } else if (auto tc = dynamic_cast<TryCatchStmt*>(stmt)) {
            for (auto& s : tc->getTryBlock()) collectAssignedParams(s.get());
            for (auto& s : tc->getCatchBlock()) collectAssignedParams(s.get());
        } else if (auto df = dynamic_cast<DeferStmt*>(stmt)) {
            collectAssignedParams(df->getDeferredStmt());
        } else if (auto ms = dynamic_cast<MatchStmt*>(stmt)) {
            for (auto& arm : ms->getArms()) collectAssignedParams(arm.body.get());
        } else if (auto ex = dynamic_cast<ExportStmt*>(stmt)) {
            collectAssignedParams(ex->getStmt());
        }
    }

    // --- bitfield access lowering ---

    // Resolve an expression's declared type WITHOUT generating code (no
    // temp counters, no instructions) — for the bitfield lookup helpers.
    TypeInfo inferExprType(Expression* expr) {
        if (auto id = dynamic_cast<IdentifierExpr*>(expr)) {
            auto sym = symbolTable_.resolve(id->getName());
            if (sym) return sym->type;
        } else if (auto mem = dynamic_cast<MemberAccessExpr*>(expr)) {
            TypeInfo objType = inferExprType(mem->getObject());
            if (objType.pointerLevel > 0) objType.pointerLevel--;
            auto sym = symbolTable_.resolve(objType.name);
            if (sym && (sym->kind == SymbolKind::STRUCT || sym->kind == SymbolKind::CLASS)) {
                auto ss = std::static_pointer_cast<StructSymbol>(sym);
                for (const auto& f : ss->fields) {
                    if (f.name == mem->getMember()) return f.type;
                }
            }
        } else if (auto idx = dynamic_cast<IndexExpr*>(expr)) {
            // Array/pointer element type (Issue #22): without this,
            // chained indexing like g_tasks[0].name[i] resolved to an
            // empty type, so the store path "widened" the char value to
            // a pointer width and emitted mov qword over the neighbors.
            TypeInfo baseType = inferExprType(idx->getObject());
            if (baseType.isArray) {
                baseType.isArray = false;
                baseType.arraySize = 0;
            } else if (baseType.pointerLevel > 0) {
                baseType.pointerLevel--;
            } else if (baseType.name == "string") {
                baseType.name = "char";
            }
            return baseType;
        } else if (auto un = dynamic_cast<UnaryExpr*>(expr)) {
            if (un->getOperator() == "*") {
                TypeInfo t = inferExprType(un->getOperand());
                if (t.pointerLevel > 0) t.pointerLevel--;
                return t;
            }
        } else if (auto group = dynamic_cast<GroupingExpr*>(expr)) {
            return inferExprType(group->getExpression());
        }
        return TypeInfo{};
    }

    bool getMemberBitfield(MemberAccessExpr* mem, int& bitOffset, int& bitWidth) {
        if (!mem) return false;
        // Resolve the object's type WITHOUT generating code: the accept()
        // would advance the temp counters and emit duplicate instructions.
        TypeInfo objType = inferExprType(mem->getObject());
        if (objType.pointerLevel > 0) objType.pointerLevel--;
        auto sym = symbolTable_.resolve(objType.name);
        if (!sym || (sym->kind != SymbolKind::STRUCT && sym->kind != SymbolKind::CLASS)) return false;
        auto structSym = std::static_pointer_cast<StructSymbol>(sym);
        for (const auto& field : structSym->fields) {
            if (field.name == mem->getMember() && field.bitWidth > 0) {
                bitOffset = field.bitOffset;
                bitWidth = field.bitWidth;
                return true;
            }
        }
        return false;
    }

    static int64_t bitfieldMask(int width) { return (1LL << width) - 1; }

    bool getMemberFieldType(MemberAccessExpr* mem, TypeInfo& out) {
        if (!mem) return false;
        TypeInfo objType = inferExprType(mem->getObject());
        if (objType.pointerLevel > 0) objType.pointerLevel--;
        auto sym = symbolTable_.resolve(objType.name);
        if (!sym || (sym->kind != SymbolKind::STRUCT && sym->kind != SymbolKind::CLASS)) return false;
        auto ss = std::static_pointer_cast<StructSymbol>(sym);
        for (const auto& f : ss->fields) {
            if (f.name == mem->getMember()) {
                out = f.type;
                return true;
            }
        }
        return false;
    }

    // Convert an integer IR value to a field/element type so the subsequent
    // STORE uses the field width (never clobbers neighbors, never leaves
    // upper garbage): narrow (`dt.month = 9` -> `mov byte`) via masking,
    // widen (`d.PhysicalStart = 0x80000000` U32 -> U64 -> `mov qword`)
    // via MOV (codegen zero/sign-extends).
    // Evaluate an integer constant expression for global initializers
    // (CID-014): `1 << 4`, `0x10|0x20`, `~0x10`, `(1<<4)`, casts of
    // constants, etc. Only folds what is exactly representable (integers,
    // chars, groups, unary -/~, int binary ops with no div-by-zero and
    // shifts < 64). Anything else (floats, identifiers, calls) returns
    // false and the global keeps the old BSS-zero behavior.
    bool tryEvalConstU64(Expression* expr, uint64_t& out) {
        if (!expr) return false;
        if (auto num = dynamic_cast<NumberExpr*>(expr)) {
            if (num->isFloat()) return false;
            out = num->getUIntValue();
            return true;
        }
        if (auto ch = dynamic_cast<CharExpr*>(expr)) {
            out = (uint64_t)(unsigned char)ch->getValue();
            return true;
        }
        if (auto grp = dynamic_cast<GroupingExpr*>(expr)) {
            return tryEvalConstU64(grp->getExpression(), out);
        }
        if (auto un = dynamic_cast<UnaryExpr*>(expr)) {
            uint64_t v = 0;
            if (!tryEvalConstU64(un->getOperand(), v)) return false;
            if (un->getOperator() == "-") { out = 0 - v; return true; }
            if (un->getOperator() == "~") { out = ~v; return true; }
            return false;
        }
        if (auto cast = dynamic_cast<CastExpr*>(expr)) {
            uint64_t v = 0;
            if (!tryEvalConstU64(cast->getExpression(), v)) return false;
            TypeInfo target = TypeInfo::fromAST(cast->getTargetType());
            if (target.pointerLevel > 0) return false;
            IRType irType = mapType(target.name);
            if (irType == IRType::I8 || irType == IRType::U8) v &= 0xFFULL;
            else if (irType == IRType::I16 || irType == IRType::U16) v &= 0xFFFFULL;
            else if (irType == IRType::I32 || irType == IRType::U32 || irType == IRType::BOOL) v &= 0xFFFFFFFFULL;
            else if (irType != IRType::I64 && irType != IRType::U64 && irType != IRType::PTR) return false;
            out = v;
            return true;
        }
        if (auto bin = dynamic_cast<BinaryExpr*>(expr)) {
            uint64_t l = 0, r = 0;
            if (!tryEvalConstU64(bin->getLeft(), l)) return false;
            if (!tryEvalConstU64(bin->getRight(), r)) return false;
            const std::string& op = bin->getOperator();
            if (op == "+") out = l + r;
            else if (op == "-") out = l - r;
            else if (op == "*") out = l * r;
            else if (op == "/") {
                if (r == 0) return false;
                out = l / r;
            } else if (op == "%") {
                if (r == 0) return false;
                out = l % r;
            } else if (op == "<<") {
                if (r >= 64) return false;
                out = l << r;
            } else if (op == ">>") {
                if (r >= 64) return false;
                out = l >> r;
            } else if (op == "&") out = l & r;
            else if (op == "|") out = l | r;
            else if (op == "^") out = l ^ r;
            else return false;
            return true;
        }
        return false;
    }

    IRValue truncateIntToType(IRValue val, const TypeInfo& target) {
        IRType tgtIR = (target.pointerLevel > 0) ? IRType::PTR : mapType(target.name);
        bool tgtNarrow = (tgtIR == IRType::I8 || tgtIR == IRType::U8 ||
                          tgtIR == IRType::I16 || tgtIR == IRType::U16);
        bool tgtWide32 = (tgtIR == IRType::I32 || tgtIR == IRType::U32 || tgtIR == IRType::BOOL);
        bool tgtWide64 = (tgtIR == IRType::I64 || tgtIR == IRType::U64 || tgtIR == IRType::PTR);
        bool valIsInt = (val.type == IRType::I8 || val.type == IRType::U8 || val.type == IRType::BOOL ||
                         val.type == IRType::I16 || val.type == IRType::U16 ||
                         val.type == IRType::I32 || val.type == IRType::U32 ||
                         val.type == IRType::I64 || val.type == IRType::U64 || val.type == IRType::PTR);
        if (!valIsInt) return val;
        if (val.type == tgtIR) return val;
        if (tgtNarrow) {
            if (val.kind == IRValue::CONSTANT) {
                uint64_t mask = (tgtIR == IRType::I8 || tgtIR == IRType::U8) ? 0xFFULL : 0xFFFFULL;
                uint64_t v = (uint64_t)val.intValue & mask;
                return IRValue::Constant(v, tgtIR);
            }
            IRValue dest = IRValue::Register(nextTemp(), tgtIR);
            uint64_t mask = (tgtIR == IRType::I8 || tgtIR == IRType::U8) ? 0xFFULL : 0xFFFFULL;
            currentBlock_->addInstruction({IROp::BIT_AND, dest, {val, IRValue::Constant((int64_t)mask, IRType::I32)}});
            return dest;
        }
        auto width = [](IRType x) -> int {
            if (x == IRType::I8 || x == IRType::U8 || x == IRType::BOOL) return 1;
            if (x == IRType::I16 || x == IRType::U16) return 2;
            if (x == IRType::I32 || x == IRType::U32) return 4;
            return 8;
        };
        if ((tgtWide32 || tgtWide64) && width(val.type) < width(tgtIR)) {
            if (val.kind == IRValue::CONSTANT) {
                return IRValue::Constant((uint64_t)val.intValue, tgtIR);
            }
            IRValue dest = IRValue::Register(nextTemp(), tgtIR);
            currentBlock_->addInstruction({IROp::MOV, dest, {val}});
            return dest;
        }
        return val;
    }

    IRValue emitBitfieldLoad(IRValue addr, int bitOffset, int bitWidth, bool isSigned) {
        IRValue word = IRValue::Register(nextTemp(), IRType::I32);
        currentBlock_->addInstruction({IROp::LOAD, word, {addr, IRValue::Constant(0, IRType::I32)}});
        IRValue shifted = IRValue::Register(nextTemp(), IRType::I32);
        currentBlock_->addInstruction({IROp::RSHIFT, shifted, {word, IRValue::Constant(bitOffset, IRType::I32)}});
        IRValue masked = IRValue::Register(nextTemp(), IRType::I32);
        currentBlock_->addInstruction({IROp::BIT_AND, masked, {shifted, IRValue::Constant(bitfieldMask(bitWidth), IRType::I32)}});
        if (isSigned && bitWidth < 32) {
            IRValue sign = IRValue::Register(nextTemp(), IRType::I32);
            currentBlock_->addInstruction({IROp::LSHIFT, sign, {masked, IRValue::Constant(32 - bitWidth, IRType::I32)}});
            IRValue value = IRValue::Register(nextTemp(), IRType::I32);
            currentBlock_->addInstruction({IROp::RSHIFT, value, {sign, IRValue::Constant(32 - bitWidth, IRType::I32)}});
            return value;
        }
        return masked;
    }

    void emitBitfieldStore(IRValue addr, IRValue val, int bitOffset, int bitWidth) {
        IRValue word = IRValue::Register(nextTemp(), IRType::I32);
        currentBlock_->addInstruction({IROp::LOAD, word, {addr, IRValue::Constant(0, IRType::I32)}});
        IRValue vm = IRValue::Register(nextTemp(), IRType::I32);
        currentBlock_->addInstruction({IROp::BIT_AND, vm, {val, IRValue::Constant(bitfieldMask(bitWidth), IRType::I32)}});
        IRValue shifted = IRValue::Register(nextTemp(), IRType::I32);
        currentBlock_->addInstruction({IROp::LSHIFT, shifted, {vm, IRValue::Constant(bitOffset, IRType::I32)}});
        IRValue cleared = IRValue::Register(nextTemp(), IRType::I32);
        int64_t clearMask = ~(bitfieldMask(bitWidth) << bitOffset);
        currentBlock_->addInstruction({IROp::BIT_AND, cleared, {word, IRValue::Constant(clearMask, IRType::I32)}});
        IRValue nw = IRValue::Register(nextTemp(), IRType::I32);
        currentBlock_->addInstruction({IROp::BIT_OR, nw, {cleared, shifted}});
        currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, nw, IRValue::Constant(0, IRType::I32)}});
    }

    void visit(AssignStmt& stmt) override {
        std::string compoundOp = stmt.getOp();

        IRValue addr = IRValue::Constant(0, IRType::PTR);
        if (auto ident = dynamic_cast<IdentifierExpr*>(stmt.getTarget())) {
            addr = variableValues_.count(ident->getName()) ? variableValues_[ident->getName()] : globalValues_[ident->getName()];
            // Param read-through must stop once the param is assigned:
            // subsequent reads go through memory (the alloca holds the new value).
            paramNames_.erase(ident->getName());
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

        if (compoundOp.empty()) {
            stmt.getValue()->accept(*this);
            IRValue val = lastValue_;

            // Bitfield targets use the read-modify-write sequence.
            if (auto mem = dynamic_cast<MemberAccessExpr*>(stmt.getTarget())) {
                int bitOffset = 0, bitWidth = 0;
                if (getMemberBitfield(mem, bitOffset, bitWidth)) {
                    emitBitfieldStore(addr, val, bitOffset, bitWidth);
                    return;
                }
                TypeInfo fieldType;
                if (getMemberFieldType(mem, fieldType)) {
                    val = truncateIntToType(val, fieldType);
                    lastExprType_ = fieldType;
                }
            } else if (auto idx = dynamic_cast<IndexExpr*>(stmt.getTarget())) {
                TypeInfo baseType = inferExprType(idx->getObject());
                TypeInfo elemType = baseType;
                if (elemType.isArray) {
                    elemType.isArray = false;
                    elemType.arraySize = 0;
                } else if (elemType.pointerLevel > 0) {
                    elemType.pointerLevel--;
                } else if (elemType.name == "string") {
                    elemType.name = "char";
                }
                val = truncateIntToType(val, elemType);
                lastExprType_ = elemType;
            } else if (auto ident = dynamic_cast<IdentifierExpr*>(stmt.getTarget())) {
                // Plain variable stores use the declared width (same rule as
                // fields and elements above): a narrow value in a wide local
                // slot leaves stale upper bytes for later wide loads.
                auto sym = symbolTable_.resolve(ident->getName());
                if (sym && sym->type.pointerLevel == 0 && !sym->type.isArray &&
                    !sym->type.isUserDefined && !isStructValueType(sym->type) &&
                    !sym->type.isInterface && !sym->type.isSlice) {
                    IRType declIR = mapType(sym->type.name);
                    bool declIsInt = (declIR == IRType::I8 || declIR == IRType::U8 ||
                                      declIR == IRType::I16 || declIR == IRType::U16 ||
                                      declIR == IRType::I32 || declIR == IRType::U32 ||
                                      declIR == IRType::BOOL ||
                                      declIR == IRType::I64 || declIR == IRType::U64);
                    if (declIsInt) val = truncateIntToType(val, sym->type);
                }
            }

            if (lastExprType_.pointerLevel == 0 && (lastExprType_.isArray || lastExprType_.isUserDefined || getTypeSize(lastExprType_) > 8)) {
                currentBlock_->addInstruction({IROp::MEMCPY, std::nullopt, {addr, val, IRValue::Constant(getTypeSize(lastExprType_), IRType::I32)}});
            } else {
                if (addr.kind == IRValue::LABEL) {
                    currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, val}});
                } else {
                    currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, val, IRValue::Constant(0, IRType::I32)}});
                }
            }
        } else {
            bool isBitfield = false;
            int bitOffset = 0, bitWidth = 0;
            if (auto mem = dynamic_cast<MemberAccessExpr*>(stmt.getTarget())) {
                isBitfield = getMemberBitfield(mem, bitOffset, bitWidth);
            }
            IRValue current;
            if (isBitfield) {
                current = emitBitfieldLoad(addr, bitOffset, bitWidth, true);
            } else {
                current = IRValue::Register(nextTemp(), IRType::I32);
                if (addr.kind == IRValue::LABEL) {
                    currentBlock_->addInstruction({IROp::LOAD, current, {addr}});
                } else {
                    currentBlock_->addInstruction({IROp::LOAD, current, {addr, IRValue::Constant(0, IRType::I32)}});
                }
            }

            stmt.getValue()->accept(*this);
            IRValue rhs = lastValue_;

            IROp irOp;
            if (compoundOp == "+") irOp = IROp::ADD;
            else if (compoundOp == "-") irOp = IROp::SUB;
            else if (compoundOp == "*") irOp = IROp::MUL;
            else if (compoundOp == "/") irOp = IROp::DIV;
            else if (compoundOp == "%") irOp = IROp::MOD;
            else if (compoundOp == "&") irOp = IROp::BIT_AND;
            else if (compoundOp == "|") irOp = IROp::BIT_OR;
            else if (compoundOp == "^") irOp = IROp::BIT_XOR;
            else return;

            IRValue result = IRValue::Register(nextTemp(), IRType::I32);
            currentBlock_->addInstruction({irOp, result, {current, rhs}});

            if (isBitfield) {
                emitBitfieldStore(addr, result, bitOffset, bitWidth);
                return;
            }
            if (addr.kind == IRValue::LABEL) {
                currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, result}});
            } else {
                currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, result, IRValue::Constant(0, IRType::I32)}});
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
        destructorStack_.push_back({});
        symbolTable_.enterScope();

        startBlock(bodyName);
        for (auto& s : stmt.getBody()) s->accept(*this);
        if (currentBlock_ && (currentBlock_->getInstructions().empty() || (currentBlock_->getInstructions().back().op != IROp::BR && currentBlock_->getInstructions().back().op != IROp::RET))) {
            for (auto it = destructorStack_.back().rbegin(); it != destructorStack_.back().rend(); ++it) {
                IRInstruction call;
                call.op = IROp::CALL;
                call.dest = std::nullopt;
                call.args.push_back(IRValue::Label(it->mangledName));
                call.args.push_back(it->varAddr);
                currentBlock_->addInstruction(call);
            }
            currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(headerName)}});
        }
        symbolTable_.exitScope();
        destructorStack_.pop_back();
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
        destructorStack_.push_back({});

        startBlock(bodyName);
        for (auto& s : stmt.getBody()) s->accept(*this);
        if (currentBlock_ && (currentBlock_->getInstructions().empty() || (currentBlock_->getInstructions().back().op != IROp::BR && currentBlock_->getInstructions().back().op != IROp::RET))) {
            for (auto it = destructorStack_.back().rbegin(); it != destructorStack_.back().rend(); ++it) {
                IRInstruction call;
                call.op = IROp::CALL;
                call.dest = std::nullopt;
                call.args.push_back(IRValue::Label(it->mangledName));
                call.args.push_back(it->varAddr);
                currentBlock_->addInstruction(call);
            }
            currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(incName)}});
        }

        startBlock(incName);
        if (stmt.getIncrement()) stmt.getIncrement()->accept(*this);
        currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(headerName)}});

        destructorStack_.pop_back();
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
                    // Width suffix for codegen (Issue #24): {fd:q} still
                    // refers to variable fd; the suffix only re-sizes the
                    // substituted register.
                    size_t colon = var.find(':');
                    if (colon != std::string::npos) var = var.substr(0, colon);
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
            emitDestructors();
            currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(breakLabels_.back())}});
            startBlock(nextLabel("dead"));
        }
    }
    void visit(ContinueStmt&) override {
        if (!continueLabels_.empty()) {
            emitDefers();
            emitDestructors();
            currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(continueLabels_.back())}});
            startBlock(nextLabel("dead"));
        }
    }
    void visit(DeferStmt& stmt) override {
        deferStack_.back().push_back(stmt.getDeferredStmt());
    }

    void visit(ForEachStmt& stmt) override {
        if (!currentFunction_) return;

        bool wasAddress = isAddress_;
        isAddress_ = false;
        stmt.getIterable()->accept(*this);
        IRValue iterable = lastValue_;
        TypeInfo iterableType = lastExprType_;
        isAddress_ = wasAddress;

        TypeInfo elemType = iterableType;
        if (elemType.isArray) {
            elemType.isArray = false;
            elemType.arraySize = 0;
        }
        if (elemType.isSlice) {
            elemType.isSlice = false;
        }

        if (!iterableType.isArray && !iterableType.isSlice) {
            // Custom iterator protocol: the element type comes from next()'s
            // return pointee, not the iterable's own type.
            auto typeSym0 = symbolTable_.resolve(iterableType.name);
            if (typeSym0 && (typeSym0->kind == SymbolKind::STRUCT || typeSym0->kind == SymbolKind::CLASS)) {
                auto ss0 = std::static_pointer_cast<StructSymbol>(typeSym0);
                for (const auto& m : ss0->methods) {
                    if (m.name == "next" && m.returnType.pointerLevel >= 1) {
                        elemType = m.returnType;
                        elemType.pointerLevel--;
                        if (elemType.pointerLevel == 0 && !elemType.isArray) {
                            auto ts2 = symbolTable_.resolve(elemType.name);
                            if (!ts2 || (ts2->kind != SymbolKind::STRUCT && ts2->kind != SymbolKind::CLASS)) {
                                elemType.isUserDefined = false;
                            }
                        }
                        break;
                    }
                }
            }
        }

        symbolTable_.enterScope();
        {
            auto loopVarSym = std::make_shared<Symbol>();
            loopVarSym->kind = SymbolKind::VARIABLE;
            loopVarSym->name = stmt.getVarName();
            loopVarSym->type = elemType;
            symbolTable_.define(loopVarSym);
        }
        deferStack_.push_back({});
        destructorStack_.push_back({});

        if (!iterableType.isArray && !iterableType.isSlice) {
            // Custom iterator protocol: it = obj.iter();
            // loop: p = it.next(); if (p == nilPtr) break; x = *p; body.
            std::string typeName = iterableType.name;
            std::string iterFn = sanitizeAsmName(typeName + "_iter");
            std::string nextFn = sanitizeAsmName(typeName + "_next");
            std::string headerName = nextLabel("citer_header");
            std::string bodyName = nextLabel("citer_body");
            std::string exitName = nextLabel("citer_exit");

            IRValue itAddr = IRValue::Register(nextTemp(), IRType::PTR);
            // `iter()` returns the iterator struct by value (sret). Allocate
            // a full struct slot (not a hardcoded 8-byte pointer slot) and
            // pass it as the hidden first argument.
            bool iterIsSret = false;
            int iterStructSize = 8;
            {
                auto iterSym = symbolTable_.resolve(iterFn);
                if (iterSym && iterSym->kind == SymbolKind::FUNCTION) {
                    auto fnSym = std::static_pointer_cast<FunctionSymbol>(iterSym);
                    if (isStructValueType(fnSym->returnType)) {
                        iterIsSret = true;
                        iterStructSize = getTypeSize(fnSym->returnType);
                    }
                } else {
                    // Unresolved (e.g. template instantiation ordering):
                    // fall back to struct-size heuristic via the iterable type.
                    // `iter()` by protocol returns a struct; use its size.
                    iterStructSize = getTypeSize(iterableType);
                    if (iterStructSize > 0) iterIsSret = true;
                }
            }
            IRValue itBuf;
            if (iterIsSret) {
                itBuf = IRValue::Register(nextTemp(), IRType::PTR);
                currentBlock_->addInstruction({IROp::ALLOCA, itBuf, {IRValue::Constant(iterStructSize, IRType::I32)}});
                currentBlock_->addInstruction({IROp::CALL, std::nullopt, {IRValue::Label(iterFn), itBuf, iterable}});
                itAddr = itBuf;
            } else {
                currentBlock_->addInstruction({IROp::ALLOCA, itAddr, {IRValue::Constant(8, IRType::I32)}});
                IRValue itReg = IRValue::Register(nextTemp(), IRType::PTR);
                currentBlock_->addInstruction({IROp::CALL, itReg, {IRValue::Label(iterFn), iterable}});
                currentBlock_->addInstruction({IROp::STORE, std::nullopt, {itAddr, itReg, IRValue::Constant(0, IRType::I32)}});
            }
            currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(headerName)}});

            startBlock(headerName);
            IRValue itv = IRValue::Register(nextTemp(), IRType::PTR);
            if (iterIsSret) {
                itv = itAddr;
            } else {
                currentBlock_->addInstruction({IROp::LOAD, itv, {itAddr, IRValue::Constant(0, IRType::I32)}});
            }
            IRValue p = IRValue::Register(nextTemp(), IRType::PTR);
            currentBlock_->addInstruction({IROp::CALL, p, {IRValue::Label(nextFn), itv}});
            IRValue isNil = IRValue::Register(nextTemp(), IRType::BOOL);
            currentBlock_->addInstruction({IROp::CMP_EQ, isNil, {p, IRValue::Constant(0, IRType::I32)}});
            currentBlock_->addInstruction({IROp::COND_BR, std::nullopt, {isNil, IRValue::Label(exitName), IRValue::Label(bodyName)}});

            continueLabels_.push_back(headerName);
            breakLabels_.push_back(exitName);

            startBlock(bodyName);
            IRValue varAddr = IRValue::Register(nextTemp(), IRType::PTR);
            currentBlock_->addInstruction({IROp::ALLOCA, varAddr, {IRValue::Constant(getTypeSize(elemType), IRType::I32)}});
            variableValues_[stmt.getVarName()] = varAddr;
            // Struct copies only for by-value aggregates (Issue #13): pointers
            // must go through LOAD+STORE, never MEMCPY.
            if (elemType.pointerLevel == 0 &&
                (elemType.isArray || isStructValueType(elemType) || getTypeSize(elemType) > 8)) {
                currentBlock_->addInstruction({IROp::MEMCPY, std::nullopt, {varAddr, p, IRValue::Constant(getTypeSize(elemType), IRType::I32)}});
            } else {
                IRType irType = (elemType.pointerLevel > 0) ? IRType::PTR : mapType(elemType.name);
                IRValue elemVal = IRValue::Register(nextTemp(), irType);
                currentBlock_->addInstruction({IROp::LOAD, elemVal, {p, IRValue::Constant(0, IRType::I32)}});
                currentBlock_->addInstruction({IROp::STORE, std::nullopt, {varAddr, elemVal, IRValue::Constant(0, IRType::I32)}});
            }

            for (auto& s : stmt.getBody()) {
                s->accept(*this);
            }
            if (currentBlock_ && (currentBlock_->getInstructions().empty() ||
                                  (currentBlock_->getInstructions().back().op != IROp::BR &&
                                   currentBlock_->getInstructions().back().op != IROp::RET))) {
                currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(headerName)}});
            }

            startBlock(exitName);
            destructorStack_.pop_back();
            deferStack_.pop_back();
            continueLabels_.pop_back();
            breakLabels_.pop_back();
            symbolTable_.exitScope();
            return;
        }

        std::string headerName = nextLabel("foreach_header");
        std::string bodyName = nextLabel("foreach_body");
        std::string incName = nextLabel("foreach_inc");
        std::string exitName = nextLabel("foreach_exit");

        IRValue iAddr = IRValue::Register(nextTemp(), IRType::PTR);
        currentBlock_->addInstruction({IROp::ALLOCA, iAddr, {IRValue::Constant(4, IRType::I32)}});
        currentBlock_->addInstruction({IROp::STORE, std::nullopt, {iAddr, IRValue::Constant(0, IRType::I32), IRValue::Constant(0, IRType::I32)}});

        currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(headerName)}});

        startBlock(headerName);
        IRValue iVal = IRValue::Register(nextTemp(), IRType::I32);
        currentBlock_->addInstruction({IROp::LOAD, iVal, {iAddr, IRValue::Constant(0, IRType::I32)}});

        IRValue limit;
        if (iterableType.isArray) {
            limit = IRValue::Constant(iterableType.arraySize, IRType::I32);
        } else {
            limit = IRValue::Register(nextTemp(), IRType::I32);
            currentBlock_->addInstruction({IROp::LOAD, limit, {iterable, IRValue::Constant(8, IRType::I32)}});
        }

        IRValue cond = IRValue::Register(nextTemp(), IRType::BOOL);
        currentBlock_->addInstruction({IROp::CMP_LT, cond, {iVal, limit}});
        currentBlock_->addInstruction({IROp::COND_BR, std::nullopt, {cond, IRValue::Label(bodyName), IRValue::Label(exitName)}});

        continueLabels_.push_back(incName);
        breakLabels_.push_back(exitName);

        startBlock(bodyName);
        iVal = IRValue::Register(nextTemp(), IRType::I32);
        currentBlock_->addInstruction({IROp::LOAD, iVal, {iAddr, IRValue::Constant(0, IRType::I32)}});

        int elemSize = getTypeSize(elemType);
        IRValue scaledIdx = IRValue::Register(nextTemp(), IRType::I32);
        currentBlock_->addInstruction({IROp::MUL, scaledIdx, {iVal, IRValue::Constant(elemSize, IRType::I32)}});

        IRValue basePtr;
        if (iterableType.isArray) {
            basePtr = iterable;
        } else {
            basePtr = IRValue::Register(nextTemp(), IRType::PTR);
            currentBlock_->addInstruction({IROp::LOAD, basePtr, {iterable, IRValue::Constant(0, IRType::I32)}});
        }

        IRValue elemAddr = IRValue::Register(nextTemp(), IRType::PTR);
        currentBlock_->addInstruction({IROp::ADD, elemAddr, {basePtr, scaledIdx}});

        IRValue varAddr = IRValue::Register(nextTemp(), IRType::PTR);
        currentBlock_->addInstruction({IROp::ALLOCA, varAddr, {IRValue::Constant(getTypeSize(elemType), IRType::I32)}});
        variableValues_[stmt.getVarName()] = varAddr;

        // Same aggregate-only rule as the iterator path (Issue #13).
        if (elemType.pointerLevel == 0 &&
            (elemType.isArray || isStructValueType(elemType) || getTypeSize(elemType) > 8)) {
            currentBlock_->addInstruction({IROp::MEMCPY, std::nullopt, {varAddr, elemAddr, IRValue::Constant(getTypeSize(elemType), IRType::I32)}});
        } else {
            IRType irType = (elemType.pointerLevel > 0) ? IRType::PTR : mapType(elemType.name);
            IRValue elemVal = IRValue::Register(nextTemp(), irType);
            currentBlock_->addInstruction({IROp::LOAD, elemVal, {elemAddr, IRValue::Constant(0, IRType::I32)}});
            currentBlock_->addInstruction({IROp::STORE, std::nullopt, {varAddr, elemVal, IRValue::Constant(0, IRType::I32)}});
        }

        for (auto& s : stmt.getBody()) {
            s->accept(*this);
        }

        if (currentBlock_ && (currentBlock_->getInstructions().empty() ||
                              (currentBlock_->getInstructions().back().op != IROp::BR && currentBlock_->getInstructions().back().op != IROp::RET))) {
            currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(incName)}});
        }

        startBlock(incName);
        iVal = IRValue::Register(nextTemp(), IRType::I32);
        currentBlock_->addInstruction({IROp::LOAD, iVal, {iAddr, IRValue::Constant(0, IRType::I32)}});
        IRValue one = IRValue::Constant(1, IRType::I32);
        IRValue newI = IRValue::Register(nextTemp(), IRType::I32);
        currentBlock_->addInstruction({IROp::ADD, newI, {iVal, one}});
        currentBlock_->addInstruction({IROp::STORE, std::nullopt, {iAddr, newI, IRValue::Constant(0, IRType::I32)}});
        currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(headerName)}});

        destructorStack_.pop_back();
        deferStack_.pop_back();
        continueLabels_.pop_back();
        breakLabels_.pop_back();
        symbolTable_.exitScope();

        startBlock(exitName);
    }

    void visit(ThrowStmt& stmt) override {
        if (currentFunction_) {
            stmt.getValue()->accept(*this);
            IRValue thrownVal = lastValue_;

            ensureExceptionGlobals();

            IRValue excValAddr = IRValue::Label("_exception_value");
            currentBlock_->addInstruction({IROp::STORE, std::nullopt, {excValAddr, thrownVal}});

            IRInstruction call;
            call.op = IROp::CALL;
            call.dest = std::nullopt;
            call.args.push_back(IRValue::Label("longjmp"));
            call.args.push_back(IRValue::Label("_exception_jmp_buf"));
            call.args.push_back(IRValue::Constant(1, IRType::I32));
            currentBlock_->addInstruction(call);

            startBlock(nextLabel("dead"));
        }
    }

    void visit(TryCatchStmt& stmt) override {
        if (currentFunction_) {
            ensureExceptionGlobals();

            IRValue setjmpDest = IRValue::Register(nextTemp(), IRType::I32);
            IRInstruction setjmpCall;
            setjmpCall.op = IROp::CALL;
            setjmpCall.dest = setjmpDest;
            setjmpCall.args.push_back(IRValue::Label("_setjmp"));
            setjmpCall.args.push_back(IRValue::Label("_exception_jmp_buf"));
            currentBlock_->addInstruction(setjmpCall);

            std::string tryBlockName = nextLabel("try_body");
            std::string catchBlockName = nextLabel("catch_body");
            std::string endName = nextLabel("try_end");

            currentBlock_->addInstruction({IROp::COND_BR, std::nullopt, {setjmpDest, IRValue::Label(catchBlockName), IRValue::Label(tryBlockName)}});

            startBlock(tryBlockName);
            symbolTable_.enterScope();
            for (auto& s : stmt.getTryBlock()) {
                s->accept(*this);
            }
            if (currentBlock_ && (currentBlock_->getInstructions().empty() ||
                                  (currentBlock_->getInstructions().back().op != IROp::BR &&
                                   currentBlock_->getInstructions().back().op != IROp::RET))) {
                currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(endName)}});
            }
            symbolTable_.exitScope();

            startBlock(catchBlockName);
            symbolTable_.enterScope();

            IRValue catchAddr = IRValue::Register(nextTemp(), IRType::PTR);
            currentBlock_->addInstruction({IROp::ALLOCA, catchAddr, {IRValue::Constant(8, IRType::I32)}});
            variableValues_[stmt.getCatchVar()] = catchAddr;

            IRValue excVal = IRValue::Register(nextTemp(), IRType::I32);
            currentBlock_->addInstruction({IROp::LOAD, excVal, {IRValue::Label("_exception_value")}});
            currentBlock_->addInstruction({IROp::STORE, std::nullopt, {catchAddr, excVal, IRValue::Constant(0, IRType::I32)}});

            for (auto& s : stmt.getCatchBlock()) {
                s->accept(*this);
            }
            if (currentBlock_ && (currentBlock_->getInstructions().empty() ||
                                  (currentBlock_->getInstructions().back().op != IROp::BR &&
                                   currentBlock_->getInstructions().back().op != IROp::RET))) {
                currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(endName)}});
            }
            symbolTable_.exitScope();

            startBlock(endName);
        } else {
            symbolTable_.enterScope();
            for (auto& s : stmt.getTryBlock()) s->accept(*this);
            symbolTable_.exitScope();
            symbolTable_.enterScope();
            for (auto& s : stmt.getCatchBlock()) s->accept(*this);
            symbolTable_.exitScope();
        }
    }

    void visit(StringExpr& expr) override {
        IRValue strVal;
        strVal.kind = IRValue::CONSTANT;
        strVal.name = "\"" + expr.getValue() + "\"";
        strVal.type = IRType::PTR;
        strVal.constValue = 0;
        strVal.intValue = 0;
        lastValue_ = strVal;
        lastExprType_ = {"string", 0};
    }

    // --- string interpolation lowering ---

    static int decodedStringLength(const std::string& raw) {
        int n = 0;
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '\\' && i + 1 < raw.size()) {
                i++;  // escape sequence counts as one byte
            }
            n++;
        }
        return n;
    }

    IRValue emitCall(const std::string& callee, std::vector<IRValue> args) {
        IRValue dest = IRValue::Register(nextTemp(), IRType::PTR);
        IRInstruction call;
        call.op = IROp::CALL;
        call.dest = dest;
        call.args.push_back(IRValue::Label(callee));
        for (auto& a : args) call.args.push_back(a);
        currentBlock_->addInstruction(call);
        return dest;
    }

    IRValue emitAdd(IRValue a, IRValue b) {
        IRValue dest = IRValue::Register(nextTemp(), IRType::I32);
        currentBlock_->addInstruction({IROp::ADD, dest, {a, b}});
        return dest;
    }

    void ensureInterpolationHelpers() {
        if (interpHelpersEnsured_) return;
        interpHelpersEnsured_ = true;

        // _strlen(s: char*) -> int
        {
            auto fn = std::make_unique<FunctionIR>("_strlen", IRType::I32);
            fn->addParam("arg_s");
            auto entry = std::make_unique<BasicBlock>("entry");
            auto loop = std::make_unique<BasicBlock>("loop");
            auto body = std::make_unique<BasicBlock>("body");
            auto exit = std::make_unique<BasicBlock>("exit");
            IRValue s = IRValue::Register(nextTemp(), IRType::PTR);
            entry->addInstruction({IROp::ALLOCA, s, {IRValue::Constant(8, IRType::I32)}});
            entry->addInstruction({IROp::STORE, std::nullopt, {s, IRValue::Register("arg_s", IRType::PTR), IRValue::Constant(0, IRType::I32)}});
            IRValue i = IRValue::Register(nextTemp(), IRType::I32);
            entry->addInstruction({IROp::ALLOCA, i, {IRValue::Constant(4, IRType::I32)}});
            entry->addInstruction({IROp::STORE, std::nullopt, {i, IRValue::Constant(0, IRType::I32), IRValue::Constant(0, IRType::I32)}});
            entry->addInstruction({IROp::BR, std::nullopt, {IRValue::Label("loop")}});
            IRValue p = IRValue::Register(nextTemp(), IRType::PTR);
            loop->addInstruction({IROp::LOAD, p, {s, IRValue::Constant(0, IRType::I32)}});
            IRValue off = IRValue::Register(nextTemp(), IRType::I32);
            loop->addInstruction({IROp::LOAD, off, {i, IRValue::Constant(0, IRType::I32)}});
            IRValue addr = IRValue::Register(nextTemp(), IRType::PTR);
            loop->addInstruction({IROp::ADD, addr, {p, off}});
            IRValue ch = IRValue::Register(nextTemp(), IRType::U8);
            loop->addInstruction({IROp::LOAD, ch, {addr, IRValue::Constant(0, IRType::I32)}});
            IRValue cond = IRValue::Register(nextTemp(), IRType::BOOL);
            loop->addInstruction({IROp::CMP_NE, cond, {ch, IRValue::Constant(0, IRType::I32)}});
            loop->addInstruction({IROp::COND_BR, std::nullopt, {cond, IRValue::Label("body"), IRValue::Label("exit")}});
            IRValue i2 = IRValue::Register(nextTemp(), IRType::I32);
            body->addInstruction({IROp::LOAD, i2, {i, IRValue::Constant(0, IRType::I32)}});
            IRValue i3 = IRValue::Register(nextTemp(), IRType::I32);
            body->addInstruction({IROp::ADD, i3, {i2, IRValue::Constant(1, IRType::I32)}});
            body->addInstruction({IROp::STORE, std::nullopt, {i, i3, IRValue::Constant(0, IRType::I32)}});
            body->addInstruction({IROp::BR, std::nullopt, {IRValue::Label("loop")}});
            IRValue i4 = IRValue::Register(nextTemp(), IRType::I32);
            exit->addInstruction({IROp::LOAD, i4, {i, IRValue::Constant(0, IRType::I32)}});
            exit->addInstruction({IROp::RET, std::nullopt, {i4}});
            fn->addBlock(std::move(entry));
            fn->addBlock(std::move(loop));
            fn->addBlock(std::move(body));
            fn->addBlock(std::move(exit));
            module_.addFunction(std::move(fn));
        }

        // _str_from_int(n: int) -> char*
        {
            auto fn = std::make_unique<FunctionIR>("_str_from_int", IRType::PTR);
            fn->addParam("arg_n");
            auto entry = std::make_unique<BasicBlock>("entry");
            auto neg = std::make_unique<BasicBlock>("neg");
            auto cont0 = std::make_unique<BasicBlock>("cont0");
            auto negchar = std::make_unique<BasicBlock>("negchar");
            auto cont1 = std::make_unique<BasicBlock>("cont1");
            auto loop = std::make_unique<BasicBlock>("loop");
            auto revhdr = std::make_unique<BasicBlock>("revhdr");
            auto revloop = std::make_unique<BasicBlock>("revloop");
            auto done = std::make_unique<BasicBlock>("done");
            IRValue n = IRValue::Register(nextTemp(), IRType::I32);
            entry->addInstruction({IROp::ALLOCA, n, {IRValue::Constant(4, IRType::I32)}});
            entry->addInstruction({IROp::STORE, std::nullopt, {n, IRValue::Register("arg_n", IRType::I32), IRValue::Constant(0, IRType::I32)}});
            IRValue idx = IRValue::Register(nextTemp(), IRType::I32);
            entry->addInstruction({IROp::ALLOCA, idx, {IRValue::Constant(4, IRType::I32)}});
            entry->addInstruction({IROp::STORE, std::nullopt, {idx, IRValue::Constant(0, IRType::I32), IRValue::Constant(0, IRType::I32)}});
            IRValue negf = IRValue::Register(nextTemp(), IRType::I32);
            entry->addInstruction({IROp::ALLOCA, negf, {IRValue::Constant(4, IRType::I32)}});
            entry->addInstruction({IROp::STORE, std::nullopt, {negf, IRValue::Constant(0, IRType::I32), IRValue::Constant(0, IRType::I32)}});
            IRValue nv = IRValue::Register(nextTemp(), IRType::I32);
            entry->addInstruction({IROp::LOAD, nv, {n, IRValue::Constant(0, IRType::I32)}});
            IRValue lt = IRValue::Register(nextTemp(), IRType::BOOL);
            entry->addInstruction({IROp::CMP_LT, lt, {nv, IRValue::Constant(0, IRType::I32)}});
            entry->addInstruction({IROp::COND_BR, std::nullopt, {lt, IRValue::Label("neg"), IRValue::Label("cont0")}});
            IRValue nv2 = IRValue::Register(nextTemp(), IRType::I32);
            neg->addInstruction({IROp::LOAD, nv2, {n, IRValue::Constant(0, IRType::I32)}});
            IRValue negv = IRValue::Register(nextTemp(), IRType::I32);
            neg->addInstruction({IROp::SUB, negv, {IRValue::Constant(0, IRType::I32), nv2}});
            neg->addInstruction({IROp::STORE, std::nullopt, {n, negv, IRValue::Constant(0, IRType::I32)}});
            neg->addInstruction({IROp::STORE, std::nullopt, {negf, IRValue::Constant(1, IRType::I32), IRValue::Constant(0, IRType::I32)}});
            neg->addInstruction({IROp::BR, std::nullopt, {IRValue::Label("cont0")}});
            IRValue nf = IRValue::Register(nextTemp(), IRType::I32);
            cont0->addInstruction({IROp::LOAD, nf, {negf, IRValue::Constant(0, IRType::I32)}});
            IRValue isneg = IRValue::Register(nextTemp(), IRType::BOOL);
            cont0->addInstruction({IROp::CMP_EQ, isneg, {nf, IRValue::Constant(1, IRType::I32)}});
            IRValue buf = IRValue::Register(nextTemp(), IRType::PTR);
            cont0->addInstruction({IROp::CALL, buf, {IRValue::Label("malloc"), IRValue::Constant(16, IRType::I32)}});
            cont0->addInstruction({IROp::COND_BR, std::nullopt, {isneg, IRValue::Label("negchar"), IRValue::Label("cont1")}});
            negchar->addInstruction({IROp::STORE, std::nullopt, {buf, IRValue::Constant(45, IRType::I32), IRValue::Constant(0, IRType::I32)}});
            negchar->addInstruction({IROp::STORE, std::nullopt, {idx, IRValue::Constant(1, IRType::I32), IRValue::Constant(0, IRType::I32)}});
            negchar->addInstruction({IROp::BR, std::nullopt, {IRValue::Label("loop")}});
            cont1->addInstruction({IROp::BR, std::nullopt, {IRValue::Label("loop")}});
            IRValue cur = IRValue::Register(nextTemp(), IRType::I32);
            loop->addInstruction({IROp::LOAD, cur, {n, IRValue::Constant(0, IRType::I32)}});
            IRValue dig = IRValue::Register(nextTemp(), IRType::I32);
            loop->addInstruction({IROp::MOD, dig, {cur, IRValue::Constant(10, IRType::I32)}});
            IRValue chv = IRValue::Register(nextTemp(), IRType::I32);
            loop->addInstruction({IROp::ADD, chv, {dig, IRValue::Constant(48, IRType::I32)}});
            IRValue ii = IRValue::Register(nextTemp(), IRType::I32);
            loop->addInstruction({IROp::LOAD, ii, {idx, IRValue::Constant(0, IRType::I32)}});
            IRValue dest = IRValue::Register(nextTemp(), IRType::PTR);
            loop->addInstruction({IROp::ADD, dest, {buf, ii}});
            loop->addInstruction({IROp::STORE, std::nullopt, {dest, chv, IRValue::Constant(0, IRType::I32)}});
            IRValue cur2 = IRValue::Register(nextTemp(), IRType::I32);
            loop->addInstruction({IROp::LOAD, cur2, {n, IRValue::Constant(0, IRType::I32)}});
            IRValue quot = IRValue::Register(nextTemp(), IRType::I32);
            loop->addInstruction({IROp::DIV, quot, {cur2, IRValue::Constant(10, IRType::I32)}});
            loop->addInstruction({IROp::STORE, std::nullopt, {n, quot, IRValue::Constant(0, IRType::I32)}});
            IRValue ii2 = IRValue::Register(nextTemp(), IRType::I32);
            loop->addInstruction({IROp::LOAD, ii2, {idx, IRValue::Constant(0, IRType::I32)}});
            IRValue ii3 = IRValue::Register(nextTemp(), IRType::I32);
            loop->addInstruction({IROp::ADD, ii3, {ii2, IRValue::Constant(1, IRType::I32)}});
            loop->addInstruction({IROp::STORE, std::nullopt, {idx, ii3, IRValue::Constant(0, IRType::I32)}});
            IRValue cur3 = IRValue::Register(nextTemp(), IRType::I32);
            loop->addInstruction({IROp::LOAD, cur3, {n, IRValue::Constant(0, IRType::I32)}});
            IRValue gt = IRValue::Register(nextTemp(), IRType::BOOL);
            loop->addInstruction({IROp::CMP_GT, gt, {cur3, IRValue::Constant(0, IRType::I32)}});
            loop->addInstruction({IROp::COND_BR, std::nullopt, {gt, IRValue::Label("loop"), IRValue::Label("revhdr")}});
            IRValue lo = IRValue::Register(nextTemp(), IRType::I32);
            revhdr->addInstruction({IROp::ALLOCA, lo, {IRValue::Constant(4, IRType::I32)}});
            IRValue nf2 = IRValue::Register(nextTemp(), IRType::I32);
            revhdr->addInstruction({IROp::LOAD, nf2, {negf, IRValue::Constant(0, IRType::I32)}});
            revhdr->addInstruction({IROp::STORE, std::nullopt, {lo, nf2, IRValue::Constant(0, IRType::I32)}});
            IRValue hi = IRValue::Register(nextTemp(), IRType::I32);
            revhdr->addInstruction({IROp::ALLOCA, hi, {IRValue::Constant(4, IRType::I32)}});
            IRValue ii4 = IRValue::Register(nextTemp(), IRType::I32);
            revhdr->addInstruction({IROp::LOAD, ii4, {idx, IRValue::Constant(0, IRType::I32)}});
            IRValue hi1 = IRValue::Register(nextTemp(), IRType::I32);
            revhdr->addInstruction({IROp::SUB, hi1, {ii4, IRValue::Constant(1, IRType::I32)}});
            revhdr->addInstruction({IROp::STORE, std::nullopt, {hi, hi1, IRValue::Constant(0, IRType::I32)}});
            IRValue lov = IRValue::Register(nextTemp(), IRType::I32);
            revhdr->addInstruction({IROp::LOAD, lov, {lo, IRValue::Constant(0, IRType::I32)}});
            IRValue hiv = IRValue::Register(nextTemp(), IRType::I32);
            revhdr->addInstruction({IROp::LOAD, hiv, {hi, IRValue::Constant(0, IRType::I32)}});
            IRValue revc = IRValue::Register(nextTemp(), IRType::BOOL);
            revhdr->addInstruction({IROp::CMP_LT, revc, {lov, hiv}});
            revhdr->addInstruction({IROp::COND_BR, std::nullopt, {revc, IRValue::Label("revloop"), IRValue::Label("done")}});
            IRValue lov2 = IRValue::Register(nextTemp(), IRType::I32);
            revloop->addInstruction({IROp::LOAD, lov2, {lo, IRValue::Constant(0, IRType::I32)}});
            IRValue pLo = IRValue::Register(nextTemp(), IRType::PTR);
            revloop->addInstruction({IROp::ADD, pLo, {buf, lov2}});
            IRValue tmp = IRValue::Register(nextTemp(), IRType::I32);
            revloop->addInstruction({IROp::LOAD, tmp, {pLo, IRValue::Constant(0, IRType::I32)}});
            IRValue hiv2 = IRValue::Register(nextTemp(), IRType::I32);
            revloop->addInstruction({IROp::LOAD, hiv2, {hi, IRValue::Constant(0, IRType::I32)}});
            IRValue pHi = IRValue::Register(nextTemp(), IRType::PTR);
            revloop->addInstruction({IROp::ADD, pHi, {buf, hiv2}});
            IRValue hiCh = IRValue::Register(nextTemp(), IRType::I32);
            revloop->addInstruction({IROp::LOAD, hiCh, {pHi, IRValue::Constant(0, IRType::I32)}});
            revloop->addInstruction({IROp::STORE, std::nullopt, {pLo, hiCh, IRValue::Constant(0, IRType::I32)}});
            revloop->addInstruction({IROp::STORE, std::nullopt, {pHi, tmp, IRValue::Constant(0, IRType::I32)}});
            IRValue lov3 = IRValue::Register(nextTemp(), IRType::I32);
            revloop->addInstruction({IROp::LOAD, lov3, {lo, IRValue::Constant(0, IRType::I32)}});
            IRValue lo1 = IRValue::Register(nextTemp(), IRType::I32);
            revloop->addInstruction({IROp::ADD, lo1, {lov3, IRValue::Constant(1, IRType::I32)}});
            revloop->addInstruction({IROp::STORE, std::nullopt, {lo, lo1, IRValue::Constant(0, IRType::I32)}});
            IRValue hiv3 = IRValue::Register(nextTemp(), IRType::I32);
            revloop->addInstruction({IROp::LOAD, hiv3, {hi, IRValue::Constant(0, IRType::I32)}});
            IRValue hi2 = IRValue::Register(nextTemp(), IRType::I32);
            revloop->addInstruction({IROp::SUB, hi2, {hiv3, IRValue::Constant(1, IRType::I32)}});
            revloop->addInstruction({IROp::STORE, std::nullopt, {hi, hi2, IRValue::Constant(0, IRType::I32)}});
            IRValue lov4 = IRValue::Register(nextTemp(), IRType::I32);
            revloop->addInstruction({IROp::LOAD, lov4, {lo, IRValue::Constant(0, IRType::I32)}});
            IRValue hiv4 = IRValue::Register(nextTemp(), IRType::I32);
            revloop->addInstruction({IROp::LOAD, hiv4, {hi, IRValue::Constant(0, IRType::I32)}});
            IRValue revc2 = IRValue::Register(nextTemp(), IRType::BOOL);
            revloop->addInstruction({IROp::CMP_LT, revc2, {lov4, hiv4}});
            revloop->addInstruction({IROp::COND_BR, std::nullopt, {revc2, IRValue::Label("revloop"), IRValue::Label("done")}});
            IRValue ii5 = IRValue::Register(nextTemp(), IRType::I32);
            done->addInstruction({IROp::LOAD, ii5, {idx, IRValue::Constant(0, IRType::I32)}});
            IRValue endp = IRValue::Register(nextTemp(), IRType::PTR);
            done->addInstruction({IROp::ADD, endp, {buf, ii5}});
            done->addInstruction({IROp::STORE, std::nullopt, {endp, IRValue::Constant(0, IRType::I32), IRValue::Constant(0, IRType::I32)}});
            done->addInstruction({IROp::RET, std::nullopt, {buf}});
            fn->addBlock(std::move(entry));
            fn->addBlock(std::move(neg));
            fn->addBlock(std::move(cont0));
            fn->addBlock(std::move(negchar));
            fn->addBlock(std::move(cont1));
            fn->addBlock(std::move(loop));
            fn->addBlock(std::move(revhdr));
            fn->addBlock(std::move(revloop));
            fn->addBlock(std::move(done));
            module_.addFunction(std::move(fn));
        }

        // _str_from_bool(v: bool) -> char*  — returns the "true"/"false" constants
        {
            auto fn = std::make_unique<FunctionIR>("_str_from_bool", IRType::PTR);
            fn->addParam("arg_v");
            auto entry = std::make_unique<BasicBlock>("entry");
            auto t = std::make_unique<BasicBlock>("t");
            auto f = std::make_unique<BasicBlock>("f");
            IRValue v = IRValue::Register(nextTemp(), IRType::I32);
            entry->addInstruction({IROp::ALLOCA, v, {IRValue::Constant(4, IRType::I32)}});
            entry->addInstruction({IROp::STORE, std::nullopt, {v, IRValue::Register("arg_v", IRType::I32), IRValue::Constant(0, IRType::I32)}});
            IRValue vv = IRValue::Register(nextTemp(), IRType::I32);
            entry->addInstruction({IROp::LOAD, vv, {v, IRValue::Constant(0, IRType::I32)}});
            IRValue cond = IRValue::Register(nextTemp(), IRType::BOOL);
            entry->addInstruction({IROp::CMP_EQ, cond, {vv, IRValue::Constant(1, IRType::I32)}});
            entry->addInstruction({IROp::COND_BR, std::nullopt, {cond, IRValue::Label("t"), IRValue::Label("f")}});
            IRValue strT;
            strT.kind = IRValue::CONSTANT;
            strT.name = "\"true\"";
            strT.type = IRType::PTR;
            t->addInstruction({IROp::RET, std::nullopt, {strT}});
            IRValue strF;
            strF.kind = IRValue::CONSTANT;
            strF.name = "\"false\"";
            strF.type = IRType::PTR;
            f->addInstruction({IROp::RET, std::nullopt, {strF}});
            fn->addBlock(std::move(entry));
            fn->addBlock(std::move(t));
            fn->addBlock(std::move(f));
            module_.addFunction(std::move(fn));
        }

        // _str_from_char(c: char) -> char* — a 2-byte string
        {
            auto fn = std::make_unique<FunctionIR>("_str_from_char", IRType::PTR);
            fn->addParam("arg_c");
            auto entry = std::make_unique<BasicBlock>("entry");
            IRValue c = IRValue::Register(nextTemp(), IRType::I32);
            entry->addInstruction({IROp::ALLOCA, c, {IRValue::Constant(4, IRType::I32)}});
            entry->addInstruction({IROp::STORE, std::nullopt, {c, IRValue::Register("arg_c", IRType::I32), IRValue::Constant(0, IRType::I32)}});
            IRValue buf = IRValue::Register(nextTemp(), IRType::PTR);
            entry->addInstruction({IROp::CALL, buf, {IRValue::Label("malloc"), IRValue::Constant(2, IRType::I32)}});
            IRValue cv = IRValue::Register(nextTemp(), IRType::I32);
            entry->addInstruction({IROp::LOAD, cv, {c, IRValue::Constant(0, IRType::I32)}});
            entry->addInstruction({IROp::STORE, std::nullopt, {buf, cv, IRValue::Constant(0, IRType::I32)}});
            entry->addInstruction({IROp::STORE, std::nullopt, {buf, IRValue::Constant(0, IRType::I32), IRValue::Constant(1, IRType::I32)}});
            entry->addInstruction({IROp::RET, std::nullopt, {buf}});
            fn->addBlock(std::move(entry));
            module_.addFunction(std::move(fn));
        }
    }

    void visit(InterpolatedStringExpr& expr) override {
        ensureInterpolationHelpers();
        std::vector<IRValue> partVals;
        std::vector<IRValue> partLens;
        int constTotal = 0;
        for (const auto& part : expr.getParts()) {
            if (!part.isExpr) {
                int len = decodedStringLength(part.text);
                constTotal += len;
                IRValue strVal;
                strVal.kind = IRValue::CONSTANT;
                strVal.name = "\"" + part.text + "\"";
                strVal.type = IRType::PTR;
                partVals.push_back(strVal);
                partLens.push_back(IRValue::Constant(len, IRType::I32));
            } else {
                part.expr->accept(*this);
                IRValue val = lastValue_;
                IRValue s;
                const std::string& t = lastExprType_.name;
                if (t == "string") {
                    s = val;
                } else if (t == "int") {
                    s = emitCall("_str_from_int", {val});
                } else if (t == "bool") {
                    s = emitCall("_str_from_bool", {val});
                } else {  // char
                    s = emitCall("_str_from_char", {val});
                }
                partVals.push_back(s);
                partLens.push_back(emitCall("_strlen", {s}));
            }
        }
        // buf = malloc(constTotal + runtime lens)
        IRValue total = IRValue::Constant(constTotal, IRType::I32);
        for (const auto& len : partLens) {
            if (len.kind != IRValue::CONSTANT) total = emitAdd(total, len);
        }
        IRValue buf = emitCall("malloc", {total});
        // Fill: memcpy(buf + off, part, len); off += len.
        IRValue off = IRValue::Constant(0, IRType::I32);
        for (size_t i = 0; i < partVals.size(); ++i) {
            IRValue dst = IRValue::Register(nextTemp(), IRType::PTR);
            currentBlock_->addInstruction({IROp::ADD, dst, {buf, off}});
            currentBlock_->addInstruction({IROp::MEMCPY, std::nullopt, {dst, partVals[i], partLens[i]}});
            off = emitAdd(off, partLens[i]);
        }
        IRValue endp = IRValue::Register(nextTemp(), IRType::PTR);
        currentBlock_->addInstruction({IROp::ADD, endp, {buf, total}});
        currentBlock_->addInstruction({IROp::STORE, std::nullopt, {endp, IRValue::Constant(0, IRType::I32), IRValue::Constant(0, IRType::I32)}});
        lastValue_ = buf;
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
        TypeInfo resType = type;
        resType.pointerLevel++;
        lastExprType_ = resType;
    }
    void visit(DeleteStmt& stmt) override {
        stmt.getTarget()->accept(*this);
        IRValue ptr = lastValue_;
        // Check if the deleted object's type has a destructor
        TypeInfo deletedType = lastExprType_;
        if (deletedType.pointerLevel > 0) {
            TypeInfo baseType = deletedType;
            baseType.pointerLevel--;
            auto sym = symbolTable_.resolve(baseType.name);
            if (sym && sym->kind == SymbolKind::CLASS) {
                auto classSym = std::static_pointer_cast<StructSymbol>(sym);
                if (!classSym->destructorName.empty()) {
                    IRInstruction dtorCall;
                    dtorCall.op = IROp::CALL;
                    dtorCall.dest = std::nullopt;
                    dtorCall.args.push_back(IRValue::Label(classSym->destructorName));
                    dtorCall.args.push_back(ptr);
                    currentBlock_->addInstruction(dtorCall);
                }
            }
        }
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
        if (elemType.isArray) {
            elemType.isArray = false;
            elemType.arraySize = 0;
        } else if (elemType.pointerLevel > 0) {
            elemType.pointerLevel--;
        } else if (elemType.name == "string") {
            elemType.name = "char";
        }

        int elemSize = getTypeSize(elemType);
        IRValue scaledIdx = IRValue::Register(nextTemp(), IRType::I32);
        currentBlock_->addInstruction({IROp::MUL, scaledIdx, {idx, IRValue::Constant(elemSize, IRType::I32)}});
        IRValue addr = IRValue::Register(nextTemp(), IRType::PTR);
        currentBlock_->addInstruction({IROp::ADD, addr, {obj, scaledIdx}});

        lastExprType_ = elemType;
        bool elemIsStruct = (elemType.pointerLevel == 0);
        if (elemIsStruct) {
            auto elemTypeSym = symbolTable_.resolve(elemType.name);
            if (!elemTypeSym || (elemTypeSym->kind != SymbolKind::STRUCT && elemTypeSym->kind != SymbolKind::CLASS)) {
                elemIsStruct = false;
            }
        }
        if (isAddress_ || elemIsStruct) {
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

        IRType srcType = lastValue_.type;
        // Detect int<->float conversions
        if ((srcType == IRType::F32 || srcType == IRType::F64) &&
            (irType == IRType::I32 || irType == IRType::I64)) {
            currentBlock_->addInstruction({IROp::CVT_FTOI, dest, {lastValue_}});
        } else if ((srcType == IRType::I32 || srcType == IRType::I64) &&
                   (irType == IRType::F32 || irType == IRType::F64)) {
            currentBlock_->addInstruction({IROp::CVT_ITOF, dest, {lastValue_}});
        } else if (irType == IRType::I8 || irType == IRType::U8) {
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
            bool isDefault = false;
            if (auto pid = dynamic_cast<IdentifierExpr*>(arm.pattern.get())) {
                isDefault = (pid->getName() == "default");
            }
            if (isDefault) {
                currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(armName)}});
            } else {
                arm.pattern->accept(*this);
                IRValue patternVal = lastValue_;
                IRValue cond = IRValue::Register(nextTemp(), IRType::BOOL);
                currentBlock_->addInstruction({IROp::CMP_EQ, cond, {matchVal, patternVal}});
                currentBlock_->addInstruction({IROp::COND_BR, std::nullopt, {cond, IRValue::Label(armName), IRValue::Label(nextCheckName)}});
            }
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
        destructorStack_.push_back({});
        for (auto& s : stmt.getStatements()) s->accept(*this);
        for (auto it = deferStack_.back().rbegin(); it != deferStack_.back().rend(); ++it) (*it)->accept(*this);
        for (auto it = destructorStack_.back().rbegin(); it != destructorStack_.back().rend(); ++it) {
            IRInstruction call;
            call.op = IROp::CALL;
            call.dest = std::nullopt;
            call.args.push_back(IRValue::Label(it->mangledName));
            call.args.push_back(it->varAddr);
            currentBlock_->addInstruction(call);
        }
        destructorStack_.pop_back();
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
    void visit(ThisExpr& /*expr*/) override {
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
    void visit(ListExpr& expr) override {
        if (!currentFunction_) return;

        auto& elements = expr.getElements();
        if (elements.empty()) {
            lastValue_ = IRValue::Constant(0, IRType::PTR);
            lastExprType_ = {"void", 1};
            return;
        }

        // Determine element type from first element
        elements[0]->accept(*this);
        IRValue firstVal = lastValue_;
        TypeInfo elemType = lastExprType_;
        int elemSize = getTypeSize(elemType);

        // Allocate stack space for all elements
        int totalSize = elemSize * (int)elements.size();
        IRValue addr = IRValue::Register(nextTemp(), IRType::PTR);
        currentBlock_->addInstruction({IROp::ALLOCA, addr, {IRValue::Constant(totalSize, IRType::I32)}});

        // Store first element
        currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, firstVal, IRValue::Constant(0, IRType::I32)}});

        // Store remaining elements
        for (size_t i = 1; i < elements.size(); i++) {
            elements[i]->accept(*this);
            if (lastExprType_.name != elemType.name ||
                lastExprType_.pointerLevel != elemType.pointerLevel) {
                lastValue_ = IRValue::Constant(0, IRType::PTR);
                lastExprType_ = {"error", 0};
                return;
            }
            IRValue offset = IRValue::Constant(i * elemSize, IRType::I32);
            currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, lastValue_, offset}});
        }

        lastValue_ = addr;
        lastExprType_ = elemType;
        lastExprType_.pointerLevel++;
    }

    void visit(TableExpr& expr) override {
        if (!currentFunction_) return;

        auto& entries = expr.getEntries();
        if (entries.empty()) {
            lastValue_ = IRValue::Constant(0, IRType::PTR);
            lastExprType_ = {"void", 1};
            return;
        }

        // Determine key/value types and sizes from first entry
        entries[0].first->accept(*this);
        IRValue firstKey = lastValue_;
        TypeInfo keyType = lastExprType_;
        int keySize = getTypeSize(keyType);

        entries[0].second->accept(*this);
        IRValue firstVal = lastValue_;
        TypeInfo valType = lastExprType_;
        int valSize = getTypeSize(valType);

        int entrySize = keySize + valSize;
        int totalSize = entrySize * (int)entries.size();
        IRValue addr = IRValue::Register(nextTemp(), IRType::PTR);
        currentBlock_->addInstruction({IROp::ALLOCA, addr, {IRValue::Constant(totalSize, IRType::I32)}});

        // Store first entry
        currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, firstKey, IRValue::Constant(0, IRType::I32)}});
        currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, firstVal, IRValue::Constant(keySize, IRType::I32)}});

        // Store remaining entries
        for (size_t i = 1; i < entries.size(); i++) {
            entries[i].first->accept(*this);
            int kvOffset = i * entrySize;
            currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, lastValue_, IRValue::Constant(kvOffset, IRType::I32)}});
            entries[i].second->accept(*this);
            currentBlock_->addInstruction({IROp::STORE, std::nullopt, {addr, lastValue_, IRValue::Constant(kvOffset + keySize, IRType::I32)}});
        }

        lastValue_ = addr;
        lastExprType_ = {"void", 1};
    }
    void visit(SliceExpr& expr) override {
        if (!currentFunction_) return;

        bool wasAddress = isAddress_;
        isAddress_ = false;
        expr.getObject()->accept(*this);
        IRValue base = lastValue_;
        TypeInfo baseType = lastExprType_;

        IRValue start;
        if (expr.getStart()) {
            expr.getStart()->accept(*this);
            start = lastValue_;
        } else {
            start = IRValue::Constant(0, IRType::I32);
        }

        IRValue end;
        if (expr.getEnd()) {
            expr.getEnd()->accept(*this);
            end = lastValue_;
        } else {
            end = IRValue::Constant(baseType.arraySize, IRType::I32);
        }
        isAddress_ = wasAddress;

        TypeInfo elemType = baseType;
        if (elemType.isArray) {
            elemType.isArray = false;
            elemType.arraySize = 0;
        }
        int elemSize = getTypeSize(elemType);

        IRValue scaledStart = IRValue::Register(nextTemp(), IRType::I32);
        currentBlock_->addInstruction({IROp::MUL, scaledStart, {start, IRValue::Constant(elemSize, IRType::I32)}});
        IRValue ptr = IRValue::Register(nextTemp(), IRType::PTR);
        currentBlock_->addInstruction({IROp::ADD, ptr, {base, scaledStart}});

        IRValue len = IRValue::Register(nextTemp(), IRType::I32);
        currentBlock_->addInstruction({IROp::SUB, len, {end, start}});

        IRValue sliceAddr = IRValue::Register(nextTemp(), IRType::PTR);
        currentBlock_->addInstruction({IROp::ALLOCA, sliceAddr, {IRValue::Constant(16, IRType::I32)}});

        currentBlock_->addInstruction({IROp::STORE, std::nullopt, {sliceAddr, ptr, IRValue::Constant(0, IRType::I32)}});
        currentBlock_->addInstruction({IROp::STORE, std::nullopt, {sliceAddr, len, IRValue::Constant(8, IRType::I32)}});

        lastValue_ = sliceAddr;
        lastExprType_ = elemType;
        lastExprType_.isSlice = true;
    }
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
        std::string thenEndName = currentBlock_->getName();

        startBlock(elseName);
        expr.getElseExpr()->accept(*this);
        IRValue elseVal = lastValue_;
        TypeInfo elseType = lastExprType_;
        currentBlock_->addInstruction({IROp::BR, std::nullopt, {IRValue::Label(mergeName)}});
        std::string elseEndName = currentBlock_->getName();

        startBlock(mergeName);
        IRType phiType = thenType.pointerLevel > 0 ? IRType::PTR : mapType(thenType.name);
        IRValue phiDest = IRValue::Register(nextTemp(), phiType);
        IRInstruction phiInstr;
        phiInstr.op = IROp::PHI;
        phiInstr.dest = phiDest;
        phiInstr.phiArgs[thenEndName] = thenVal;
        phiInstr.phiArgs[elseEndName] = elseVal;
        currentBlock_->addInstruction(phiInstr);
        lastValue_ = phiDest;
        lastExprType_ = thenType;
    }

    void visit(SuperExpr& expr) override {
        if (currentClassName_.empty() || !currentFunction_) {
            lastValue_ = IRValue::Constant(0, IRType::PTR);
            lastExprType_ = {"error", 0};
            return;
        }
        auto classSym = symbolTable_.resolve(currentClassName_);
        if (!classSym || classSym->kind != SymbolKind::CLASS) {
            lastValue_ = IRValue::Constant(0, IRType::PTR);
            lastExprType_ = {"error", 0};
            return;
        }
        auto cs = std::static_pointer_cast<StructSymbol>(classSym);
        if (cs->parentName.empty()) {
            lastValue_ = IRValue::Constant(0, IRType::PTR);
            lastExprType_ = {"error", 0};
            return;
        }
        auto parentSym = symbolTable_.resolve(cs->parentName);
        if (!parentSym || (parentSym->kind != SymbolKind::CLASS && parentSym->kind != SymbolKind::STRUCT)) {
            lastValue_ = IRValue::Constant(0, IRType::PTR);
            lastExprType_ = {"error", 0};
            return;
        }
        auto ps = std::static_pointer_cast<StructSymbol>(parentSym);
        for (auto& m : ps->methods) {
            if (m.name == expr.getMember()) {
                lastValue_ = IRValue::Label(m.mangledName);
                lastExprType_ = m.returnType;
                return;
            }
        }
        lastValue_ = IRValue::Constant(0, IRType::PTR);
        lastExprType_ = {"error", 0};
    }
    void visit(AwaitExpr& expr) override {
        // v1 async semantics: the awaited async function runs to completion
        // (cooperative, synchronous). The await lowers to the plain call;
        // the value is the callee's result. The suspension machinery (state
        // machines + scheduler) lands with the stdlib I/O layer.
        if (!currentFunction_) {
            lastValue_ = IRValue::Constant(0, IRType::I32);
            lastExprType_ = {"int", 0};
            return;
        }
        expr.getValue()->accept(*this);
        // The accept emitted the call; its result register is the awaited
        // value.
    }
    void visit(ArrayTypeAST&) override {}
    void visit(TypeAST&) override {}
    void visit(StructDeclStmt& stmt) override {
        structNames_.push_back(stmt.getName());
        lastExprType_ = {stmt.getName(), 0};
    }
    void visit(EnumDeclStmt&) override {}
    void visit(TypeAliasStmt&) override {
        // Type aliases are resolved at the type level; no runtime code generated
    }
    void visit(ImportStmt&) override {}
    void visit(FromImportStmt&) override {}
    void visit(ExportExpressionStmt&) override {}
    void visit(InterfaceDeclStmt& stmt) override { interfaceNames_.push_back(stmt.getName()); }

    void ensureExceptionGlobals() {
        if (!exceptionGlobalsEnsured_) {
            exceptionGlobalsEnsured_ = true;
            module_.addGlobalAsm("global _exception_jmp_buf");
            module_.addGlobalAsm("_exception_jmp_buf: resq 25");
            module_.addGlobalAsm("global _exception_value");
            module_.addGlobalAsm("_exception_value: resq 1");
        }
    }

private:
    ModuleIR module_;
    SymbolTable& symbolTable_;
    std::unique_ptr<FunctionIR> currentFunction_;
    std::unique_ptr<BasicBlock> currentBlock_;
    std::string currentClassName_;
    std::string currentSourcePath_;
    IRValue lastValue_ = IRValue::Constant(0, IRType::I32);
    TypeInfo lastExprType_;
    bool isAddress_ = false;
    std::map<std::string, IRValue> variableValues_;
    std::set<std::string> paramNames_;
    std::set<std::string> assignedParams_;
    std::map<std::string, IRValue> globalValues_;
    std::vector<std::string> classNames_;
    std::vector<std::string> interfaceNames_;
    std::vector<std::string> structNames_;
    std::set<AstProgram*> visitedPrograms_;
    std::vector<std::string> breakLabels_;
    std::vector<std::string> continueLabels_;
    std::vector<std::vector<Statement*>> deferStack_;
    std::vector<std::vector<DestructorInfo>> destructorStack_;
    bool exceptionGlobalsEnsured_ = false;
    bool interpHelpersEnsured_ = false;
    bool currentIsSret_ = false;
    TypeInfo currentReturnType_ = {"void", 0};
    int tempCount_ = 0;
    int globalStrCount_ = 0;
    int labelCount_ = 0;
    int anonFuncCount_ = 0;
    int closureCount_ = 0;
    std::string nextTemp() { return "v" + std::to_string(tempCount_++); }
    std::string nextLabel(const std::string& prefix) {
        return (currentFunction_ ? currentFunction_->getName() + "_" : "") + prefix + std::to_string(labelCount_++);
    }
};

}  // namespace cider
