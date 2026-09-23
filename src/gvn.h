#pragma once

#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "dominators.h"
#include "ir.h"

namespace cider {

class GvnPass {
public:
    bool run(ModuleIR& module) {
        bool changed = false;
        for (auto& func : module.getFunctions()) {
            if (runOnFunction(*func)) changed = true;
        }
        return changed;
    }

private:
    DominatorTree dt_;
    std::map<std::string, std::string> valueNumbers_;  // reg name → value number string
    std::map<std::string, std::string> vnToReg_;       // value number string → canonical reg
    std::map<std::string, std::string> defBlock_;      // reg name → defining block name
    std::map<std::string, IRType> regTypes_;           // reg name → IR type
    int uniqueCounter_ = 0;

    static std::string opToString(IROp op) {
        switch (op) {
        case IROp::ADD: return "ADD";
        case IROp::SUB: return "SUB";
        case IROp::MUL: return "MUL";
        case IROp::DIV: return "DIV";
        case IROp::MOD: return "MOD";
        case IROp::BIT_AND: return "BIT_AND";
        case IROp::BIT_OR: return "BIT_OR";
        case IROp::BIT_XOR: return "BIT_XOR";
        case IROp::BIT_NOT: return "BIT_NOT";
        case IROp::LSHIFT: return "LSHIFT";
        case IROp::RSHIFT: return "RSHIFT";
        case IROp::CMP_EQ: return "CMP_EQ";
        case IROp::CMP_NE: return "CMP_NE";
        case IROp::CMP_LT: return "CMP_LT";
        case IROp::CMP_GT: return "CMP_GT";
        case IROp::CMP_LE: return "CMP_LE";
        case IROp::CMP_GE: return "CMP_GE";
        case IROp::MOV: return "MOV";
        case IROp::CVT_ITOF: return "CVT_ITOF";
        case IROp::CVT_FTOI: return "CVT_FTOI";
        case IROp::PHI: return "PHI";
        default: return "OP";
        }
    }

    // Ops safe to deduplicate (pure, no side effects, no unique identity).
    static bool isDedupable(IROp op) {
        switch (op) {
        case IROp::ADD: case IROp::SUB: case IROp::MUL: case IROp::DIV: case IROp::MOD:
        case IROp::BIT_AND: case IROp::BIT_OR: case IROp::BIT_XOR: case IROp::BIT_NOT:
        case IROp::LSHIFT: case IROp::RSHIFT:
        case IROp::CMP_EQ: case IROp::CMP_NE: case IROp::CMP_LT: case IROp::CMP_GT:
        case IROp::CMP_LE: case IROp::CMP_GE:
        case IROp::CVT_ITOF: case IROp::CVT_FTOI:
        case IROp::PHI: case IROp::MOV:
            return true;
        default:
            return false;  // STORE, CALL, VIRTUAL_CALL, RET, BR, COND_BR, ASM, PUSH, POP, LOAD, ALLOCA, MEMCPY
        }
    }

    static std::string typeToString(IRType t) {
        switch (t) {
        case IRType::I8: return "I8";
        case IRType::U8: return "U8";
        case IRType::I16: return "I16";
        case IRType::U16: return "U16";
        case IRType::I32: return "I32";
        case IRType::U32: return "U32";
        case IRType::I64: return "I64";
        case IRType::U64: return "U64";
        case IRType::F32: return "F32";
        case IRType::F64: return "F64";
        case IRType::BOOL: return "BOOL";
        case IRType::PTR: return "PTR";
        case IRType::VOID: return "VOID";
        }
        return "UNKNOWN";
    }

    std::string vnOfOperand(const IRValue& v) {
        if (v.kind == IRValue::CONSTANT) {
            // String literals are PTR constants with intValue=0 and their
            // content in name (e.g. "\"Hello\""). They must be numbered by
            // content — two different literals are different values, and
            // sharing a VN would let GVN replace one literal with a MOV of
            // the other.
            if (v.type == IRType::PTR && v.name.size() >= 2 && v.name.front() == '"') {
                return "const_str_" + v.name;
            }
            // Float constants are keyed by their exact IEEE-754 bits:
            // std::to_string(double) renders fixed 6-decimal text, so tiny
            // values (1e-7, 2e-7, 0.0) all collapsed to one value number and
            // GVN deduplicated distinct constants. Bit keys also keep +0.0
            // and -0.0 apart.
            if (v.type == IRType::F32) {
                float f = (float)v.constValue;
                uint32_t bits = 0;
                std::memcpy(&bits, &f, sizeof(bits));
                return "const_F32_" + std::to_string(bits);
            }
            if (v.type == IRType::F64) {
                uint64_t bits = 0;
                std::memcpy(&bits, &v.constValue, sizeof(bits));
                return "const_F64_" + std::to_string(bits);
            }
            return "const_" + typeToString(v.type) + "_" + std::to_string(v.intValue);
        }
        if (v.kind == IRValue::LABEL) {
            return "label_" + v.name;
        }
        auto it = valueNumbers_.find(v.name);
        if (it != valueNumbers_.end()) return it->second;
        return v.name;
    }

    std::string computeVN(const IRInstruction& instr) {
        if (!isDedupable(instr.op)) {
            return opToString(instr.op) + "_" + std::to_string(uniqueCounter_++);
        }
        if (instr.op == IROp::PHI) {
            std::string vn = "PHI(";
            bool first = true;
            for (const auto& [pred, val] : instr.phiArgs) {
                if (!first) vn += ", ";
                first = false;
                vn += pred + ":" + vnOfOperand(val);
            }
            vn += ")";
            return vn;
        }
        std::string vn = opToString(instr.op) + "(";
        for (size_t i = 0; i < instr.args.size(); ++i) {
            if (i > 0) vn += ", ";
            vn += vnOfOperand(instr.args[i]);
        }
        vn += ")";
        return vn;
    }

    bool runOnFunction(FunctionIR& func) {
        dt_.build(func);
        valueNumbers_.clear();
        vnToReg_.clear();
        defBlock_.clear();
        uniqueCounter_ = 0;

        if (func.getBlocks().empty()) return false;
        bool changed = false;
        std::set<std::string> visited;

        std::function<void(const std::string&)> visit = [&](const std::string& blockName) {
            if (!visited.insert(blockName).second) return;

            BasicBlock* block = nullptr;
            for (auto& b : func.getBlocks()) {
                if (b->getName() == blockName) {
                    block = b.get();
                    break;
                }
            }
            if (!block) return;

            auto& instrs = block->getInstructions();
            for (auto& instr : instrs) {
                if (!instr.dest || instr.dest->kind != IRValue::REGISTER) continue;
                std::string vn = computeVN(instr);
                auto it = vnToReg_.find(vn);
                if (it != vnToReg_.end() && it->second != instr.dest->name) {
                    std::string canonical = it->second;
                    // PHI dedup uses the same dominance rule: identical phiArgs (keyed by
                    // predecessor name) yield equal values on every edge, and the dominance
                    // check guarantees the canonical definition is available here.
                    auto db = defBlock_.find(canonical);
                    bool dominant = (db != defBlock_.end()) && dt_.dominates(db->second, blockName);
                    if (dominant && regTypes_.count(canonical) && regTypes_[canonical] == instr.dest->type) {
                        IRValue canonicalVal = IRValue::Register(canonical, instr.dest->type);
                        instr.op = IROp::MOV;
                        instr.args.clear();
                        instr.args.push_back(canonicalVal);
                        instr.phiArgs.clear();
                        instr.asmMap.clear();
                        valueNumbers_[instr.dest->name] = vn;
                        changed = true;
                        continue;
                    }
                }
                vnToReg_[vn] = instr.dest->name;
                valueNumbers_[instr.dest->name] = vn;
                defBlock_[instr.dest->name] = blockName;
                regTypes_[instr.dest->name] = instr.dest->type;
            }

            for (auto& b : func.getBlocks()) {
                if (dt_.dominates(blockName, b->getName()) && dt_.idom(b->getName()) == blockName) {
                    visit(b->getName());
                }
            }
        };

        visit(func.getBlocks().front()->getName());
        return changed;
    }
};

}  // namespace cider
