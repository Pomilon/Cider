#include "cider_codegen.h"
#include "cider_reg_alloc.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace Cider {

    class X86_64_Linux_Backend : public TargetBackend {
    public:
        bool generate(ModuleIR& module, const std::string& outputPath) override {
            std::vector<std::string> bssSection;
            std::vector<std::string> dataSection;
            std::vector<std::string> globalAsmSection;
            std::vector<std::string> textSection;
            int strCount = 0;

            auto addBss = [&](const std::string& line) { bssSection.push_back(line); };
            auto addData = [&](const std::string& line) { dataSection.push_back(line); };
            auto addGlobalAsm = [&](const std::string& line) { globalAsmSection.push_back(line); };
            auto addText = [&](const std::string& line) { textSection.push_back(line); };

            auto sanitizeString = [](const std::string& s) {
                std::string res = s;
                if (res.size() >= 2 && res.front() == '"' && res.back() == '"') {
                    res = res.substr(1, res.size() - 2);
                }
                
                std::stringstream ss;
                ss << "`";
                for (char c : res) {
                    if (c == '\n') ss << "`, 10, `";
                    else if (c == '\r') ss << "`, 13, `";
                    else if (c == '\t') ss << "`, 9, `";
                    else ss << c;
                }
                ss << "`";
                std::string sres = ss.str();
                size_t pos;
                while ((pos = sres.find(", ``")) != std::string::npos) sres.erase(pos, 4);
                while ((pos = sres.find("``, ")) != std::string::npos) sres.erase(pos, 4);
                if (sres == "``") return std::string("");
                return sres;
            };

            auto isMem = [](const std::string& op) {
                return op.find("[") != std::string::npos;
            };

            RegisterAllocator regAlloc;
            
            for (auto& func : module.getFunctions()) {
                for (auto& block : func->getBlocks()) {
                    for (auto& instr : block->getInstructions()) {
                        for (auto& arg : instr.args) {
                            if (arg.kind == IRValue::CONSTANT && arg.type == IRType::PTR && arg.name.front() == '"') {
                                std::string label = "str" + std::to_string(strCount++);
                                addData(label + ": db " + sanitizeString(arg.name) + ", 0");
                                arg.name = label;
                                arg.kind = IRValue::LABEL;
                            }
                        }
                    }
                }
            }

            for (const auto& c : module.getGlobalAsm()) {
                addGlobalAsm(c);
            }

            for (const auto& var : module.getGlobalVars()) {
                std::string sizeDir = "dq";
                std::string resDir = "resq";
                if (var.type == IRType::I8 || var.type == IRType::U8) { sizeDir = "db"; resDir = "resb"; }
                else if (var.type == IRType::I16 || var.type == IRType::U16) { sizeDir = "dw"; resDir = "resw"; }
                else if (var.type == IRType::I32 || var.type == IRType::U32) { sizeDir = "dd"; resDir = "resd"; }

                if (var.initialValue != 0) {
                    addData(var.name + ": " + sizeDir + " " + std::to_string((long long)var.initialValue));
                } else {
                    addBss(var.name + ": " + resDir + " 1");
                }
            }

            for (const auto& [className, methods] : module.getVTables()) {
                std::string line = "vtable_" + className + ": dq ";
                for (size_t i = 0; i < methods.size(); ++i) {
                    line += methods[i];
                    if (i < methods.size() - 1) line += ", ";
                }
                addData(line);
            }

            for (auto& func : module.getFunctions()) {
                int regAllocStackSize = 0;
                auto alloc = regAlloc.allocate(*func, regAllocStackSize);
                
                std::string fName = func->getName();
                if (fName == "_start" || fName == "main") {
                    addText("global " + fName);
                }
                
                addText(fName + ":");
                addText("    push rbp");
                addText("    mov rbp, rsp");
                
                std::vector<std::string> calleeSaved = {"rbx", "r12", "r13", "r14", "r15"};
                for (const auto& reg : calleeSaved) {
                    addText("    push " + reg);
                }

                int savedRegsSize = 8 + (calleeSaved.size() * 8); // rbp + 5 regs = 48 bytes
                
                std::map<std::string, int> localOffsets;
                int currentAllocaOffset = savedRegsSize;
                for (auto& block : func->getBlocks()) {
                    for (auto& instr : block->getInstructions()) {
                        if (instr.op == IROp::ALLOCA) {
                            std::string varName = instr.dest->name;
                            if (localOffsets.find(varName) == localOffsets.end()) {
                                int size = (int)instr.args[0].constValue;
                                if (size < 8) size = 8;
                                if (size % 8 != 0) size += 8 - (size % 8);
                                localOffsets[varName] = currentAllocaOffset + size;
                                currentAllocaOffset += size;
                            }
                        }
                    }
                }
                
                // Spills start after allocas
                int spillBase = currentAllocaOffset;
                int totalStack = spillBase + regAllocStackSize;
                
                // Align total stack to 16 bytes for ABI compliance
                if (totalStack % 16 != 0) {
                    totalStack += 16 - (totalStack % 16);
                }

                if (totalStack > 0) {
                    addText("    sub rsp, " + std::to_string(totalStack));
                }

                auto getOperand = [&](const IRValue& val) -> std::string {
                    if (val.kind == IRValue::CONSTANT) {
                        if (val.type == IRType::F32 || val.type == IRType::F64) return std::to_string(val.constValue);
                        return std::to_string((long long)val.constValue);
                    } else if (val.kind == IRValue::LABEL) {
                        return val.name;
                    } else {
                        if (alloc.count(val.name)) {
                            const auto& a = alloc.at(val.name);
                            if (a.isSpilled) {
                                // stackOffset is negative from regAlloc.h (-8, -16, etc)
                                int offset = spillBase + (-a.stackOffset);
                                return "[rbp - " + std::to_string(offset) + "]";
                            }
                            return a.reg;
                        }
                        return val.name;
                    }
                };
                
                const char* argRegs[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};

                auto getSizedReg = [](const std::string& reg, IRType t) -> std::string {
                    if (reg == "none") return reg;
                    if (reg.find("[") != std::string::npos) {
                        std::string prefix = "dword ";
                        if (t == IRType::I8 || t == IRType::U8) prefix = "byte ";
                        else if (t == IRType::I16 || t == IRType::U16) prefix = "word ";
                        else if (t == IRType::I32 || t == IRType::U32) prefix = "dword ";
                        else if (t == IRType::I64 || t == IRType::U64 || t == IRType::PTR) prefix = "qword ";
                        return prefix + reg;
                    }
                    static std::map<std::string, std::vector<std::string>> regMap = {
                        {"rax", {"al", "ax", "eax", "rax"}},
                        {"rbx", {"bl", "bx", "ebx", "rbx"}},
                        {"rcx", {"cl", "cx", "ecx", "rcx"}},
                        {"rdx", {"dl", "dx", "edx", "rdx"}},
                        {"rsi", {"sil", "si", "esi", "rsi"}},
                        {"rdi", {"dil", "di", "edi", "rdi"}},
                        {"rbp", {"bpl", "bp", "ebp", "rbp"}},
                        {"rsp", {"spl", "sp", "esp", "rsp"}},
                        {"r8",  {"r8b", "r8w", "r8d", "r8"}},
                        {"r9",  {"r9b", "r9w", "r9d", "r9"}},
                        {"r10", {"r10b", "r10w", "r10d", "r10"}},
                        {"r11", {"r11b", "r11w", "r11d", "r11"}},
                        {"r12", {"r12b", "r12w", "r12d", "r12"}},
                        {"r13", {"r13b", "r13w", "r13d", "r13"}},
                        {"r14", {"r14b", "r14w", "r14d", "r14"}},
                        {"r15", {"r15b", "r15w", "r15d", "r15"}}
                    };
                    if (regMap.find(reg) == regMap.end()) return reg;
                    int idx = 3;
                    if (t == IRType::I8 || t == IRType::U8) idx = 0;
                    else if (t == IRType::I16 || t == IRType::U16) idx = 1;
                    else if (t == IRType::I32 || t == IRType::U32) idx = 2;
                    return regMap[reg][idx];
                };

                bool firstBlock = true;
                for (auto& block : func->getBlocks()) {
                    addText("." + block->getName() + ":");
                    if (firstBlock) {
                        firstBlock = false;
                        const auto& params = func->getParams();
                        for (size_t i = 0; i < params.size() && i < 6; ++i) {
                            IRValue pReg;
                            pReg.kind = IRValue::REGISTER;
                            pReg.name = params[i];
                            std::string dest = getOperand(pReg);
                            if (isMem(dest)) {
                                addText("    mov rax, " + std::string(argRegs[i]));
                                addText("    mov " + dest + ", rax");
                            } else {
                                addText("    mov " + dest + ", " + std::string(argRegs[i]));
                            }
                        }
                    }
                    for (auto& instr : block->getInstructions()) {
                        std::string d = instr.dest ? getOperand(*instr.dest) : "";
                        std::string a1 = instr.args.size() > 0 ? getOperand(instr.args[0]) : "";
                        std::string a2 = instr.args.size() > 1 ? getOperand(instr.args[1]) : "";

                        bool isB = (instr.dest && (instr.dest->type == IRType::I8 || instr.dest->type == IRType::U8));
                        if (instr.dest && instr.dest->type == IRType::PTR) isB = false;

                        switch (instr.op) {
                            case IROp::MOV: {
                                if (d != a1) {
                                    IRType t = (instr.dest ? instr.dest->type : IRType::I32);
                                    if (instr.args[0].kind == IRValue::LABEL) {
                                        addText("    lea rax, [rel " + a1 + "]");
                                        addText("    mov " + getSizedReg(d, t) + ", " + getSizedReg("rax", t));
                                    } else if (isMem(d) && isMem(a1)) {
                                        addText("    mov " + getSizedReg("rax", t) + ", " + getSizedReg(a1, t));
                                        addText("    mov " + getSizedReg(d, t) + ", " + getSizedReg("rax", t));
                                    } else {
                                        if (instr.args[0].kind == IRValue::CONSTANT && instr.args[0].constValue > 0xFFFFFFFF) {
                                            addText("    mov rax, " + a1);
                                            addText("    mov " + getSizedReg(d, t) + ", " + getSizedReg("rax", t));
                                        } else {
                                            addText("    mov " + getSizedReg(d, t) + ", " + getSizedReg(a1, t));
                                        }
                                    }
                                }
                                break;
                            }
                            case IROp::MEMCPY: {
                                std::string dest = a1;
                                std::string src = a2;
                                std::string size = getOperand(instr.args[2]);

                                addText("    push rsi");
                                addText("    push rdi");
                                addText("    push rcx");

                                if (isMem(dest)) {
                                    addText("    mov rdi, " + dest);
                                } else {
                                    addText("    mov rdi, " + dest);
                                }

                                if (isMem(src)) {
                                    addText("    mov rsi, " + src);
                                } else {
                                    addText("    mov rsi, " + src);
                                }

                                if (instr.args[2].kind == IRValue::CONSTANT) {
                                    addText("    mov rcx, " + size);
                                } else {
                                    addText("    mov rcx, " + size);
                                }
                                addText("    rep movsb");

                                addText("    pop rcx");
                                addText("    pop rdi");
                                addText("    pop rsi");
                                break;
                            }
                            case IROp::PUSH:
                                addText("    push " + a1);
                                break;
                            case IROp::POP:
                                if (isMem(d)) {
                                    addText("    pop rax");
                                    addText("    mov " + getSizedReg(d, IRType::I64) + ", rax");
                                } else {
                                    addText("    pop " + d);
                                }
                                break;
                            case IROp::ADD:
                            case IROp::SUB: {
                                std::string opName = (instr.op == IROp::ADD ? "add" : "sub");
                                IRType t = (instr.dest ? instr.dest->type : IRType::I32);

                                if (d != a1) {
                                    if (isMem(d) && isMem(a1)) {
                                        addText("    mov " + getSizedReg("rax", t) + ", " + getSizedReg(a1, t));
                                        addText("    mov " + getSizedReg(d, t) + ", " + getSizedReg("rax", t));
                                    } else {
                                        addText("    mov " + getSizedReg(d, t) + ", " + getSizedReg(a1, t));
                                    }
                                }
                                
                                std::string rhs = a2;
                                if (instr.args[1].kind == IRValue::CONSTANT) {
                                    addText("    mov " + getSizedReg("rax", t) + ", " + a2);
                                    rhs = "rax";
                                }

                                if (isMem(d)) {
                                    addText("    mov " + getSizedReg("rax", t) + ", " + getSizedReg(rhs, t));
                                    addText("    " + opName + " " + getSizedReg(d, t) + ", " + getSizedReg("rax", t));
                                } else {
                                    addText("    " + opName + " " + getSizedReg(d, t) + ", " + getSizedReg(rhs, t));
                                }
                                break;
                            }
                            case IROp::MUL: {
                                IRType t = (instr.dest ? instr.dest->type : IRType::I32);
                                bool is64 = (t == IRType::I64 || t == IRType::U64 || t == IRType::PTR);

                                if (is64) {
                                    addText("    mov rax, " + a1);
                                    if (instr.args[1].kind == IRValue::CONSTANT) {
                                        addText("    mov r11, " + a2);
                                        addText("    imul r11");
                                    } else {
                                        addText("    imul " + getSizedReg(a2, t));
                                    }
                                    addText("    mov " + getSizedReg(d, t) + ", rax");
                                } else {
                                    addText("    mov eax, " + getSizedReg(a1, t));
                                    if (instr.args[1].kind == IRValue::CONSTANT) {
                                        addText("    mov ecx, " + a2);
                                        addText("    imul ecx");
                                    } else {
                                        addText("    imul " + getSizedReg(a2, t));
                                    }
                                    addText("    mov " + getSizedReg(d, t) + ", eax");
                                }
                                break;
                            }
                            case IROp::DIV:
                            case IROp::MOD: {
                                std::vector<std::string> clobbered = {"rax", "rdx", "rcx"};
                                std::vector<std::string> toSave;
                                for (const auto& reg : clobbered) {
                                    if (reg == d) continue;
                                    for (const auto& [var, res] : alloc) {
                                        if (!res.isSpilled && res.reg == reg) {
                                            toSave.push_back(reg);
                                            break;
                                        }
                                    }
                                }

                                for (const auto& reg : toSave) addText("    push " + reg);

                                IRType t = (instr.dest ? instr.dest->type : IRType::I32);
                                bool is64 = (t == IRType::I64 || t == IRType::U64 || t == IRType::PTR);

                                if (is64) {
                                    addText("    mov rax, " + getSizedReg(a1, t));
                                    addText("    cqo");
                                    if (instr.args[1].kind == IRValue::CONSTANT) {
                                        addText("    mov rcx, " + a2);
                                        addText("    idiv rcx");
                                    } else {
                                        addText("    idiv " + getSizedReg(a2, t));
                                    }
                                    addText("    mov " + getSizedReg(d, t) + ", " + (instr.op == IROp::DIV ? "rax" : "rdx"));
                                } else {
                                    addText("    mov eax, " + getSizedReg(a1, t));
                                    addText("    cdq");
                                    if (instr.args[1].kind == IRValue::CONSTANT) {
                                        addText("    mov ecx, " + a2);
                                        addText("    idiv ecx");
                                    } else {
                                        addText("    idiv " + getSizedReg(a2, t));
                                    }
                                    addText("    mov " + getSizedReg(d, t) + ", " + (instr.op == IROp::DIV ? "eax" : "edx"));
                                }

                                for (auto it = toSave.rbegin(); it != toSave.rend(); ++it) {
                                    addText("    pop " + *it);
                                }
                                break;
                            }
                            case IROp::BIT_AND:
                            case IROp::BIT_OR:
                            case IROp::BIT_XOR:
                            case IROp::LSHIFT:
                            case IROp::RSHIFT: {
                                std::string opName = "";
                                if (instr.op == IROp::BIT_AND) opName = "and";
                                else if (instr.op == IROp::BIT_OR) opName = "or";
                                else if (instr.op == IROp::BIT_XOR) opName = "xor";
                                else if (instr.op == IROp::LSHIFT) opName = "shl";
                                else if (instr.op == IROp::RSHIFT) opName = "shr";

                                IRType t = (instr.dest ? instr.dest->type : IRType::I32);

                                if (d != a1) {
                                    if (isMem(d) && isMem(a1)) {
                                        addText("    mov " + getSizedReg("rax", t) + ", " + getSizedReg(a1, t));
                                        addText("    mov " + getSizedReg(d, t) + ", " + getSizedReg("rax", t));
                                    } else {
                                        addText("    mov " + getSizedReg(d, t) + ", " + getSizedReg(a1, t));
                                    }
                                }

                                if (instr.op == IROp::LSHIFT || instr.op == IROp::RSHIFT) {
                                    if (instr.args[1].kind == IRValue::CONSTANT) {
                                        addText("    " + opName + " " + getSizedReg(d, t) + ", " + a2);
                                    } else {
                                        addText("    mov rcx, " + a2);
                                        addText("    " + opName + " " + getSizedReg(d, t) + ", cl");
                                    }
                                } else {
                                    if (isMem(d) && isMem(a2)) {
                                        addText("    mov " + getSizedReg("rax", t) + ", " + getSizedReg(a2, t));
                                        addText("    " + opName + " " + getSizedReg(d, t) + ", " + getSizedReg("rax", t));
                                    } else {
                                        addText("    " + opName + " " + getSizedReg(d, t) + ", " + getSizedReg(a2, t));
                                    }
                                }
                                break;
                            }
                            case IROp::BIT_NOT: {
                                IRType t = (instr.dest ? instr.dest->type : IRType::I32);
                                if (d != a1) {
                                    if (isMem(d) && isMem(a1)) {
                                        addText("    mov " + getSizedReg("rax", t) + ", " + getSizedReg(a1, t));
                                        addText("    mov " + getSizedReg(d, t) + ", " + getSizedReg("rax", t));
                                    } else {
                                        addText("    mov " + getSizedReg(d, t) + ", " + getSizedReg(a1, t));
                                    }
                                }
                                addText("    not " + getSizedReg(d, t));
                                break;
                            }
                            case IROp::CMP_EQ:
                            case IROp::CMP_NE:
                            case IROp::CMP_LT:
                            case IROp::CMP_GT:
                            case IROp::CMP_LE:
                            case IROp::CMP_GE: {
                                IRType t = instr.args[0].type;
                                std::string lhs = a1;
                                if (instr.args[0].kind == IRValue::CONSTANT || isMem(lhs)) {
                                    addText("    mov " + getSizedReg("rax", t) + ", " + getSizedReg(lhs, t));
                                    lhs = "rax";
                                }
                                addText("    cmp " + getSizedReg(lhs, t) + ", " + getSizedReg(a2, t));
                                
                                bool isUnsigned = (t == IRType::U8 || t == IRType::U16 || t == IRType::U32 || t == IRType::U64 || t == IRType::PTR);

                                std::string setop;
                                if (instr.op == IROp::CMP_EQ) setop = "sete";
                                else if (instr.op == IROp::CMP_NE) setop = "setne";
                                else if (instr.op == IROp::CMP_LT) setop = isUnsigned ? "setb" : "setl";
                                else if (instr.op == IROp::CMP_GT) setop = isUnsigned ? "seta" : "setg";
                                else if (instr.op == IROp::CMP_LE) setop = isUnsigned ? "setbe" : "setle";
                                else if (instr.op == IROp::CMP_GE) setop = isUnsigned ? "setae" : "setge";
                                
                                addText("    " + setop + " al");
                                addText("    movzx rax, al");
                                addText("    mov " + d + ", rax");
                                break;
                            }
                            case IROp::BR:
                                addText("    jmp ." + a1);
                                break;
                            case IROp::COND_BR:
                                if (isMem(a1)) {
                                    addText("    mov rax, " + a1);
                                    addText("    cmp rax, 0");
                                } else {
                                    addText("    cmp " + a1 + ", 0");
                                }
                                addText("    jne ." + a2);
                                if (instr.args.size() > 2) {
                                    addText("    jmp ." + getOperand(instr.args[2]));
                                }
                                break;
                            case IROp::ASM: {
                                std::string code = a1;
                                if (code.size() >= 2 && code.front() == '"' && code.back() == '"') {
                                    code = code.substr(1, code.size() - 2);
                                }

                                std::vector<std::string> callerSaved = {"rax", "r10", "r11", "rdi", "rsi", "rdx", "rcx", "r8", "r9"};
                                std::vector<std::string> toSave;

                                std::set<std::string> outputRegs;
                                for (auto& pair : instr.asmMap) {
                                    std::string placeholder = "{" + pair.first + "}";
                                    if (code.find("mov " + placeholder + ",") != std::string::npos ||
                                        code.find("pop " + placeholder) != std::string::npos ||
                                        code.find("lea " + placeholder + ",") != std::string::npos ||
                                        code.find("movzx " + placeholder + ",") != std::string::npos) {
                                        std::string op = getOperand(pair.second);
                                        if (!isMem(op)) outputRegs.insert(op);
                                    }
                                }

                                for (const auto& reg : callerSaved) {
                                    if (outputRegs.count(reg)) continue;
                                    for (const auto& [var, res] : alloc) {
                                        if (!res.isSpilled && res.reg == reg) {
                                            toSave.push_back(reg);
                                            break;
                                        }
                                    }
                                }
                                if (toSave.size() % 2 != 0) {
                                    addText("    sub rsp, 8");
                                }
                                for (const auto& reg : toSave) {
                                    addText("    push " + reg);
                                }

                                const char* tempRegs[] = {"r10", "r11", "r12", "r13", "r14", "r15"};
                                int tempIdx = 0;
                                std::string prefixCode = "";

                                for (auto& pair : instr.asmMap) {                                    const std::string& placeholder = pair.first;
                                    const IRValue& val = pair.second;

                                    std::string derefSearch = "{*" + placeholder + "}";
                                    size_t derefPos = 0;
                                    bool foundDeref = false;
                                    std::string usedTemp = "rax";

                                    while ((derefPos = code.find(derefSearch, derefPos)) != std::string::npos) {
                                        if (!foundDeref) {
                                            usedTemp = tempRegs[tempIdx++];
                                            std::string addrReg = getOperand(val);
                                            if (isMem(addrReg)) {
                                                prefixCode += "    mov " + usedTemp + ", " + addrReg + "\n";
                                                prefixCode += "    mov " + usedTemp + ", [" + usedTemp + "]\n";
                                            } else {
                                                prefixCode += "    mov " + usedTemp + ", [" + addrReg + "]\n";
                                            }
                                            foundDeref = true;
                                        }
                                        code.replace(derefPos, derefSearch.length(), usedTemp);
                                        derefPos += usedTemp.length();
                                    }

                                    std::string replacement = getOperand(val);
                                    size_t pos = 0;
                                    std::string search = "{" + placeholder + "}";
                                    while ((pos = code.find(search, pos)) != std::string::npos) {
                                        code.replace(pos, search.length(), replacement);
                                        pos += replacement.length();
                                    }
                                }
                                if (!prefixCode.empty()) {
                                    std::string line;
                                    std::stringstream ss(prefixCode);
                                    while (std::getline(ss, line)) if (!line.empty()) addText(line);
                                }
                                addText("    " + code);

                                for (auto it = toSave.rbegin(); it != toSave.rend(); ++it) {
                                    addText("    pop " + *it);
                                }
                                if (toSave.size() % 2 != 0) {
                                    addText("    add rsp, 8");
                                }
                                break;
                            }
                            case IROp::ALLOCA: {
                                std::string varName = instr.dest->name;
                                addText("    lea rax, [rbp - " + std::to_string(localOffsets[varName]) + "]");
                                addText("    mov " + getSizedReg(d, IRType::PTR) + ", rax");
                                break;
                            }
                            case IROp::LOAD: {
                                std::string ptr = a1;
                                IRType t = (instr.dest ? instr.dest->type : IRType::I32);
                                std::string movOp = "mov rax, ";
                                if (t == IRType::I8 || t == IRType::U8) movOp = "movzx rax, byte ";
                                else if (t == IRType::I16 || t == IRType::U16) movOp = "movzx rax, word ";
                                else if (t == IRType::I32 || t == IRType::U32) movOp = "mov eax, ";
                                else if (t == IRType::I64 || t == IRType::U64 || t == IRType::PTR) movOp = "mov rax, qword ";

                                if (instr.args[0].kind == IRValue::LABEL) {
                                    addText("    " + movOp + "[rel " + instr.args[0].name + "]");
                                } else {
                                    std::vector<std::string> candidates = {"r11", "r10", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "rax"};
                                    std::string addrReg = "";
                                    for (const auto& c : candidates) {
                                        bool isBusy = false;
                                        if (c == ptr || (instr.args.size() > 2 && getOperand(instr.args[2]) == c)) isBusy = true;
                                        if (!isBusy) {
                                            for (const auto& [var, res] : alloc) {
                                                if (!res.isSpilled && res.reg == c) {
                                                    isBusy = true;
                                                    break;
                                                }
                                            }
                                        }
                                        if (!isBusy) {
                                            addrReg = c;
                                            break;
                                        }
                                    }

                                    bool spilled = false;
                                    if (addrReg.empty()) {
                                        addrReg = "r11";
                                        addText("    push r11");
                                        spilled = true;
                                    }

                                    addText("    mov " + addrReg + ", " + ptr);
                                    if (instr.args.size() > 2) {
                                        addText("    add " + addrReg + ", " + getOperand(instr.args[2]));
                                    }
                                    addText("    " + movOp + "[" + addrReg + "]");
                                    if (spilled) {
                                        addText("    pop r11");
                                    }
                                }
                                addText("    mov " + d + ", rax");
                                break;
                            }
                            case IROp::STORE: {
                                std::string ptr = a1;
                                std::string val = a2;
                                IRType t = (instr.args.size() > 1 ? instr.args[1].type : IRType::I32);
                                std::string sizePrefix = "dword ";
                                if (t == IRType::I8 || t == IRType::U8) sizePrefix = "byte ";
                                else if (t == IRType::I16 || t == IRType::U16) sizePrefix = "word ";
                                else if (t == IRType::I32 || t == IRType::U32) sizePrefix = "dword ";
                                else if (t == IRType::I64 || t == IRType::U64 || t == IRType::PTR) sizePrefix = "qword ";

                                std::string srcReg = "rax";
                                if (t == IRType::I8 || t == IRType::U8) srcReg = "al";
                                else if (t == IRType::I16 || t == IRType::U16) srcReg = "ax";
                                else if (t == IRType::I32 || t == IRType::U32) srcReg = "eax";

                                if (instr.args[0].kind == IRValue::LABEL) {
                                    if (instr.args[1].kind == IRValue::LABEL) {
                                        addText("    lea rax, [rel " + val + "]");
                                    } else {
                                        addText("    mov rax, " + val);
                                    }
                                    addText("    mov " + sizePrefix + "[rel " + instr.args[0].name + "], " + srcReg);
                                } else {
                                    std::vector<std::string> candidates = {"r11", "r10", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "rbx"};
                                    std::string addrReg = "";
                                    for (const auto& c : candidates) {
                                        bool isBusy = false;
                                        if (c == ptr || c == val || (instr.args.size() > 2 && getOperand(instr.args[2]) == c)) isBusy = true;
                                        if (!isBusy) {
                                            for (const auto& [var, res] : alloc) {
                                                if (!res.isSpilled && res.reg == c) {
                                                    isBusy = true;
                                                    break;
                                                }
                                            }
                                        }
                                        if (!isBusy) {
                                            addrReg = c;
                                            break;
                                        }
                                    }

                                    bool spilled = false;
                                    if (addrReg.empty()) {
                                        addrReg = "r11";
                                        addText("    push r11");
                                        spilled = true;
                                    }

                                    addText("    mov " + addrReg + ", " + ptr);
                                    if (instr.args.size() > 2) {
                                        addText("    add " + addrReg + ", " + getOperand(instr.args[2]));
                                    }

                                    if (instr.args[1].kind == IRValue::LABEL) {
                                        addText("    lea rax, [rel " + val + "]");
                                    } else {
                                        addText("    mov rax, " + val);
                                    }

                                    addText("    mov " + sizePrefix + "[" + addrReg + "], " + srcReg);

                                    if (spilled) {
                                        addText("    pop r11");
                                    }
                                }
                                break;
                            }
                            case IROp::CALL:
                            case IROp::VIRTUAL_CALL: {
                                std::vector<std::string> callerSaved = {"r10", "r11", "rdi", "rsi", "rdx", "rcx", "r8", "r9"};
                                std::vector<std::string> toSave;
                                for (const auto& reg : callerSaved) {
                                    for (const auto& [var, res] : alloc) {
                                        if (!res.isSpilled && res.reg == reg) {
                                            toSave.push_back(reg);
                                            break;
                                        }
                                    }
                                }

                                if (toSave.size() % 2 != 0) {
                                    addText("    sub rsp, 8");
                                }
                                for (const auto& reg : toSave) {
                                    addText("    push " + reg);
                                }

                                if (instr.op == IROp::VIRTUAL_CALL) {
                                    // VIRTUAL_CALL: args[0] = objPtr, args[1] = vtableIndex
                                    std::string objPtr = getOperand(instr.args[0]);
                                    int vtableIndex = (int)instr.args[1].constValue;

                                    if (isMem(objPtr)) {
                                        addText("    mov rax, " + objPtr);
                                        addText("    mov rax, [rax]"); // Load vptr (vtable address)
                                    } else {
                                        addText("    mov rax, [" + objPtr + "]"); // Load vptr (vtable address)
                                    }
                                    addText("    mov r11, [rax + " + std::to_string(vtableIndex * 8) + "]"); // Load function address into r11
                                }

                                size_t startArg = (instr.op == IROp::VIRTUAL_CALL ? 2 : 1);
                                size_t argCount = instr.args.size() - startArg;
                                if (instr.op == IROp::VIRTUAL_CALL) argCount++; // Include 'this'

                                for (size_t i = 0; i < (instr.op == IROp::VIRTUAL_CALL ? argCount : instr.args.size() - 1); ++i) {
                                    if (i < 6) {
                                        IRValue argVal;
                                        if (instr.op == IROp::VIRTUAL_CALL) {
                                            if (i == 0) argVal = instr.args[0]; // 'this'
                                            else argVal = instr.args[i + 1];
                                        } else {
                                            argVal = instr.args[i + 1];
                                        }

                                        std::string argStr = getOperand(argVal);
                                        if (argVal.kind == IRValue::LABEL) {
                                            addText("    lea rax, [" + argStr + "]");
                                            addText("    mov " + std::string(argRegs[i]) + ", rax");
                                        } else if (isMem(argStr)) {
                                            addText("    mov rax, " + argStr);
                                            addText("    mov " + std::string(argRegs[i]) + ", rax");
                                        } else {
                                            addText("    mov " + std::string(argRegs[i]) + ", " + argStr);
                                        }
                                    }
                                }

                                if (instr.op == IROp::VIRTUAL_CALL) {
                                    addText("    call r11");
                                } else {
                                    addText("    call " + a1);
                                }

                                for (auto it = toSave.rbegin(); it != toSave.rend(); ++it) {
                                    addText("    pop " + *it);
                                }
                                if (toSave.size() % 2 != 0) {
                                    addText("    add rsp, 8");
                                }

                                if (instr.dest && !d.empty() && d != "none") {
                                    addText("    mov " + d + ", rax");
                                }
                                break;
                            }
                            case IROp::RET: {
                                if (instr.args.size() > 0) addText("    mov rax, " + a1);
                                if (calleeSaved.size() > 0) {
                                    addText("    lea rsp, [rbp - " + std::to_string(calleeSaved.size() * 8) + "]");
                                    for (auto it = calleeSaved.rbegin(); it != calleeSaved.rend(); ++it) addText("    pop " + *it);
                                } else {
                                    addText("    mov rsp, rbp");
                                }
                                addText("    pop rbp");
                                addText("    ret");
                                break;
                            }
                            default: break;
                        }
                    }
                }
            }

            std::ofstream out(outputPath, std::ios::trunc);
            if (!out) return false;
            out << "section .bss\n";
            for (const auto& line : bssSection) out << "    " << line << "\n";
            for (const auto& line : globalAsmSection) out << "    " << line << "\n";
            out << "\nsection .data\n";
            for (const auto& line : dataSection) out << "    " << line << "\n";
            out << "\nsection .text\n";
            out << "    extern malloc\n";
            out << "    extern _setjmp\n";
            out << "    extern longjmp\n";
            for (const auto& line : textSection) out << line << "\n";
            return true;
        }
    };

    CodeGenerator::CodeGenerator(const std::string& targetStr) {
        backend = std::make_unique<X86_64_Linux_Backend>();
    }

    bool CodeGenerator::generate(ModuleIR& module, const std::string& outputPath) {
        if (!backend) return false;
        return backend->generate(module, outputPath);
    }

} // namespace Cider
