#pragma once

#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cider {

enum class IRType {
    I8, U8, I16, U16, I32, U32, I64, U64, F32, F64, BOOL, PTR, VOID
};

struct IRValue {
    enum Kind { CONSTANT, REGISTER, LABEL } kind;
    std::string name;
    IRType type;
    double constValue = 0;
    int64_t intValue = 0;

    static IRValue Register(const std::string& n, IRType t) {
        IRValue v;
        v.kind = REGISTER;
        v.name = n;
        v.type = t;
        return v;
    }
    static IRValue Constant(double val, IRType t) {
        IRValue v;
        v.kind = CONSTANT;
        v.name = std::to_string(val);
        v.type = t;
        v.constValue = val;
        v.intValue = (int64_t)val;
        return v;
    }
    static IRValue Constant(int64_t val, IRType t) {
        IRValue v;
        v.kind = CONSTANT;
        v.name = std::to_string(val);
        v.type = t;
        v.constValue = (double)val;
        v.intValue = val;
        return v;
    }
    static IRValue Constant(uint64_t val, IRType t) {
        IRValue v;
        v.kind = CONSTANT;
        v.name = std::to_string(val);
        v.type = t;
        v.constValue = (double)val;
        v.intValue = (int64_t)val;
        return v;
    }
    static IRValue Constant(int val, IRType t) {
        return Constant((int64_t)val, t);
    }
    static IRValue Label(const std::string& n) {
        IRValue v;
        v.kind = LABEL;
        v.name = n;
        v.type = IRType::VOID;
        return v;
    }

    int64_t intConst() const { return intValue; }
    uint64_t uintConst() const { return (uint64_t)intValue; }

    std::string toString() const {
        if (kind == CONSTANT) {
            if (!name.empty() && type == IRType::PTR) return name;
            if (type == IRType::F32 || type == IRType::F64) return std::to_string(constValue);
            return std::to_string(intValue);
        }
        if (kind == LABEL) return name;
        return "%" + name;
    }
};

enum class IROp {
    ADD, SUB, MUL, DIV, MOD,
    BIT_AND, BIT_OR, BIT_XOR, BIT_NOT,
    LSHIFT, RSHIFT,
    CMP_EQ, CMP_NE, CMP_LT, CMP_GT, CMP_LE, CMP_GE,
    LOAD, STORE, ALLOCA, MOV,
    MEMCPY,
    PUSH, POP,
    BR, COND_BR,
    CALL, VIRTUAL_CALL, RET,
    CVT_ITOF, CVT_FTOI, CVT_F32F64, CVT_F64F32,
    PHI, ASM
};

struct IRInstruction {
    IROp op;
    std::optional<IRValue> dest;
    std::vector<IRValue> args;

    // For PHI nodes: maps basic block name to value
    std::map<std::string, IRValue> phiArgs;

    // For ASM substitution: maps placeholder name to IRValue
    std::map<std::string, IRValue> asmMap;

    IRInstruction() = default;
    IRInstruction(IROp o, std::optional<IRValue> d, std::vector<IRValue> a)
        : op(o), dest(std::move(d)), args(std::move(a)) {}

    std::string toString() const;
};

class BasicBlock {
public:
    BasicBlock(const std::string& name) : name_(name) {}
    void addInstruction(IRInstruction instr) { instructions_.push_back(std::move(instr)); }
    void prependInstruction(IRInstruction instr) { instructions_.insert(instructions_.begin(), std::move(instr)); }

    const std::string& getName() const { return name_; }
    void setName(const std::string& n) { name_ = n; }
    std::vector<IRInstruction>& getInstructions() { return instructions_; }
    const std::vector<IRInstruction>& getInstructions() const { return instructions_; }

private:
    std::string name_;
    std::vector<IRInstruction> instructions_;
};

class FunctionIR {
public:
    FunctionIR(const std::string& name, IRType returnType)
        : name_(name), returnType_(returnType) {}

    void addBlock(std::unique_ptr<BasicBlock> block) { blocks_.push_back(std::move(block)); }
    void addParam(const std::string& paramName) { params_.push_back(paramName); }
    const std::string& getName() const { return name_; }
    void setSourcePath(const std::string& p) { sourcePath_ = p; }
    const std::string& getSourcePath() const { return sourcePath_; }
    std::vector<std::unique_ptr<BasicBlock>>& getBlocks() { return blocks_; }
    const std::vector<std::unique_ptr<BasicBlock>>& getBlocks() const { return blocks_; }
    const std::vector<std::string>& getParams() const { return params_; }
    IRType getReturnType() const { return returnType_; }

private:
    std::string name_;
    IRType returnType_;
    std::vector<std::unique_ptr<BasicBlock>> blocks_;
    std::vector<std::string> params_;
    std::string sourcePath_;
};

class ModuleIR {
public:
    void addFunction(std::unique_ptr<FunctionIR> func) { functions_.push_back(std::move(func)); }
    std::vector<std::unique_ptr<FunctionIR>>& getFunctions() { return functions_; }
    const std::vector<std::unique_ptr<FunctionIR>>& getFunctions() const { return functions_; }

    void addGlobalAsm(const std::string& code) { globalAsm_.push_back(code); }
    const std::vector<std::string>& getGlobalAsm() const { return globalAsm_; }

    void addStringData(const std::string& line) { stringData_.push_back(line); }
    const std::vector<std::string>& getStringData() const { return stringData_; }

    struct GlobalVar {
        std::string name;
        IRType type;
        int64_t initialValue;
        int arraySize = 0;
        int totalBytes = 0;
        // Float initializer (CID-029): initialValue holds the IEEE-754
        // bit pattern (u32 bits for F32, u64 bits for F64), not a
        // truncated integer. A plain `10.0` used to land as `dq 10`.
        bool initialIsFloat = false;
    };
    void addGlobalVar(const std::string& name, IRType type, int64_t init = 0, int arraySize = 0, int totalBytes = 0, bool isFloat = false) {
        globalVars_.push_back({name, type, init, arraySize, totalBytes, isFloat});
    }
    const std::vector<GlobalVar>& getGlobalVars() const { return globalVars_; }

    void addVTable(const std::string& className, std::vector<std::string> methods) {
        vtables_[className] = std::move(methods);
    }
    const std::map<std::string, std::vector<std::string>>& getVTables() const {
        return vtables_;
    }

    void addExternFunction(const std::string& name) {
        externFunctions_.push_back(name);
    }
    const std::vector<std::string>& getExternFunctions() const {
        return externFunctions_;
    }

    // CID-006 F1: `extern var` symbols are linker-provided DATA, not
    // functions. Keeping them out of externFunctions_ stops the VM from
    // fabricating a function VALUE for a variable read.
    void addExternVariable(const std::string& name) {
        externVariables_.push_back(name);
    }
    const std::vector<std::string>& getExternVariables() const {
        return externVariables_;
    }

    void print() const;

private:
    std::vector<std::unique_ptr<FunctionIR>> functions_;
    std::vector<std::string> globalAsm_;
    std::vector<std::string> stringData_;
    std::vector<GlobalVar> globalVars_;
    std::map<std::string, std::vector<std::string>> vtables_;
    std::vector<std::string> externFunctions_;
    std::vector<std::string> externVariables_;
};

}  // namespace cider
