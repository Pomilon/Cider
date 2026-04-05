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
            std::vector<std::string> textSection;
            int strCount = 0;

            auto addBss = [&](const std::string& line) { bssSection.push_back(line); };
            auto addData = [&](const std::string& line) { dataSection.push_back(line); };
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
                addText(c);
            }

            for (const auto& var : module.getGlobalVars()) {
                if (var.initialValue != 0) {
                    addData(var.name + ": dq " + std::to_string((long long)var.initialValue));
                } else {
                    addBss(var.name + ": resq 1");
                }
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
                
                std::vector<std::string> calleeSaved = {"rbx", "r10", "r11", "r12", "r13", "r14", "r15"};
                for (const auto& reg : calleeSaved) {
                    addText("    push " + reg);
                }

                int savedRegsSize = 8 + (calleeSaved.size() * 8); 
                
                int allocaReserved = 0;
                std::map<std::string, int> localOffsets;
                int currentAllocaOffset = 0;
                for (auto& block : func->getBlocks()) {
                    for (auto& instr : block->getInstructions()) {
                        if (instr.op == IROp::ALLOCA) {
                            std::string varName = instr.dest->name;
                            if (localOffsets.find(varName) == localOffsets.end()) {
                                currentAllocaOffset += 256; 
                                localOffsets[varName] = currentAllocaOffset;
                            }
                        }
                    }
                }
                allocaReserved = currentAllocaOffset;
                
                int totalStack = regAllocStackSize + allocaReserved;
                if ((savedRegsSize + totalStack) % 16 != 0) {
                    totalStack += 16 - ((savedRegsSize + totalStack) % 16);
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
                                int offset = a.stackOffset - savedRegsSize - allocaReserved;
                                return "qword [rbp " + std::to_string(offset) + "]";
                            }
                            return a.reg;
                        }
                        return val.name;
                    }
                };
                
                const char* argRegs[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};

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
                            case IROp::MOV:
                                if (d != a1) {
                                    if (isMem(d) && isMem(a1)) {
                                        addText("    mov rax, " + a1);
                                        addText("    mov " + d + ", rax");
                                    } else {
                                        addText("    mov " + d + ", " + a1);
                                    }
                                }
                                break;
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
                                    addText("    mov " + d + ", rax");
                                } else {
                                    addText("    pop " + d);
                                }
                                break;
                            case IROp::ADD:
                                if (d != a1) {
                                    if (isMem(d) && isMem(a1)) {
                                        addText("    mov rax, " + a1);
                                        addText("    mov " + d + ", rax");
                                    } else {
                                        addText("    mov " + d + ", " + a1);
                                    }
                                }
                                if (isMem(d) && isMem(a2)) {
                                    addText("    mov r11, " + a2);
                                    addText("    add " + d + ", r11");
                                } else {
                                    addText("    add " + d + ", " + a2);
                                }
                                break;
                            case IROp::SUB:
                                if (d != a1) {
                                    if (isMem(d) && isMem(a1)) {
                                        addText("    mov rax, " + a1);
                                        addText("    mov " + d + ", rax");
                                    } else {
                                        addText("    mov " + d + ", " + a1);
                                    }
                                }
                                if (isMem(d) && isMem(a2)) {
                                    addText("    mov rcx, " + a2);
                                    addText("    sub " + d + ", rcx");
                                } else {
                                    addText("    sub " + d + ", " + a2);
                                }
                                break;
                            case IROp::MUL:
                                if (d != a1) {
                                    if (isMem(d) && isMem(a1)) {
                                        addText("    mov rax, " + a1);
                                        addText("    mov " + d + ", rax");
                                    } else {
                                        addText("    mov " + d + ", " + a1);
                                    }
                                }
                                if (isMem(d)) {
                                    addText("    mov rax, " + d);
                                    if (isMem(a2)) {
                                        addText("    mov r11, " + a2);
                                        addText("    imul rax, r11");
                                    } else {
                                        addText("    imul rax, " + a2);
                                    }
                                    addText("    mov " + d + ", rax");
                                } else {
                                    if (isMem(a2)) {
                                        addText("    mov r11, " + a2);
                                        addText("    imul " + d + ", r11");
                                    } else {
                                        addText("    imul " + d + ", " + a2);
                                    }
                                }
                                break;
                            case IROp::DIV:
                            case IROp::MOD: {
                                addText("    mov rax, " + a1);
                                addText("    cqo");
                                if (instr.args[1].kind == IRValue::CONSTANT) {
                                    addText("    mov rcx, " + a2);
                                    addText("    idiv rcx");
                                } else {
                                    addText("    idiv " + a2);
                                }
                                addText("    mov " + d + ", " + (instr.op == IROp::DIV ? "rax" : "rdx"));
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

                                if (d != a1) {
                                    if (isMem(d) && isMem(a1)) {
                                        addText("    mov rax, " + a1);
                                        addText("    mov " + d + ", rax");
                                    } else {
                                        addText("    mov " + d + ", " + a1);
                                    }
                                }

                                if (instr.op == IROp::LSHIFT || instr.op == IROp::RSHIFT) {
                                    if (instr.args[1].kind == IRValue::CONSTANT) {
                                        addText("    " + opName + " " + d + ", " + a2);
                                    } else {
                                        addText("    mov rcx, " + a2);
                                        addText("    " + opName + " " + d + ", cl");
                                    }
                                } else {
                                    if (isMem(d) && isMem(a2)) {
                                        addText("    mov r11, " + a2);
                                        addText("    " + opName + " " + d + ", r11");
                                    } else {
                                        addText("    " + opName + " " + d + ", " + a2);
                                    }
                                }
                                break;
                            }
                            case IROp::BIT_NOT: {
                                if (d != a1) {
                                    if (isMem(d) && isMem(a1)) {
                                        addText("    mov rax, " + a1);
                                        addText("    mov " + d + ", rax");
                                    } else {
                                        addText("    mov " + d + ", " + a1);
                                    }
                                }
                                addText("    not " + d);
                                break;
                            }
                            case IROp::CMP_EQ:
                            case IROp::CMP_NE:
                            case IROp::CMP_LT:
                            case IROp::CMP_GT:
                            case IROp::CMP_LE:
                            case IROp::CMP_GE: {
                                std::string lhs = a1;
                                if (instr.args[0].kind == IRValue::CONSTANT || isMem(lhs)) {
                                    addText("    mov rax, " + lhs);
                                    lhs = "rax";
                                }
                                addText("    cmp " + lhs + ", " + a2);
                                std::string setop;
                                if (instr.op == IROp::CMP_EQ) setop = "sete";
                                else if (instr.op == IROp::CMP_NE) setop = "setne";
                                else if (instr.op == IROp::CMP_LT) setop = "setl";
                                else if (instr.op == IROp::CMP_GT) setop = "setg";
                                else if (instr.op == IROp::CMP_LE) setop = "setle";
                                else if (instr.op == IROp::CMP_GE) setop = "setge";
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
                                
                                const char* tempRegs[] = {"r10", "r11", "r12", "r13", "r14", "r15"};
                                int tempIdx = 0;
                                std::string prefixCode = "";

                                for (auto& pair : instr.asmMap) {
                                    const std::string& placeholder = pair.first;
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
                                break;
                            }
                            case IROp::ALLOCA: {
                                std::string varName = instr.dest->name;
                                addText("    lea rax, [rbp - " + std::to_string(localOffsets[varName] + savedRegsSize) + "]");
                                addText("    mov " + d + ", rax");
                                break;
                            }
                            case IROp::LOAD: {
                                std::string ptr = a1;
                                bool isB = (instr.dest && (instr.dest->type == IRType::I8 || instr.dest->type == IRType::U8));
                                if (instr.dest && instr.dest->type == IRType::PTR) isB = false;
                                
                                std::string movOp = isB ? "movzx rax, byte " : "mov rax, ";

                                if (instr.args[0].kind == IRValue::LABEL) {
                                    addText("    " + movOp + "[rel " + instr.args[0].name + "]");
                                } else if (instr.args.size() > 2) {
                                    std::string off = getOperand(instr.args[2]);
                                    addText("    mov rdx, " + ptr);
                                    addText("    add rdx, " + off);
                                    addText("    " + movOp + "[rdx]");
                                } else {
                                    if (isMem(ptr)) {
                                        addText("    mov rcx, " + ptr);
                                        ptr = "rcx";
                                    }
                                    addText("    " + movOp + "[" + ptr + "]");
                                }
                                addText("    mov " + d + ", rax");
                                break;
                            }
                            case IROp::STORE: {
                                std::string ptr = a1;
                                std::string val = a2;
                                bool isB = (instr.args.size() > 1 && (instr.args[1].type == IRType::I8 || instr.args[1].type == IRType::U8));
                                if (instr.args.size() > 1 && instr.args[1].type == IRType::PTR) isB = false;

                                if (instr.args[0].kind == IRValue::LABEL) {
                                    addText("    mov rax, " + val);
                                    if (isB) addText("    mov byte [rel " + instr.args[0].name + "], al");
                                    else addText("    mov [rel " + instr.args[0].name + "], rax");
                                } else {
                                    if (isMem(ptr)) {
                                        addText("    mov rcx, " + ptr);
                                        ptr = "rcx";
                                    }
                                    addText("    mov rax, " + val);
                                    std::string addrStr;
                                    if (instr.args.size() > 2) {
                                        std::string off = getOperand(instr.args[2]);
                                        addText("    mov rdx, " + ptr);
                                        addText("    add rdx, " + off);
                                        addrStr = "[rdx]";
                                    } else {
                                        addrStr = "[" + ptr + "]";
                                    }
                                    if (isB) addText("    mov byte " + addrStr + ", al");
                                    else addText("    mov " + addrStr + ", rax");
                                }
                                break;
                            }
                            case IROp::CALL: {
                                for (size_t i = 1; i < instr.args.size(); ++i) {
                                    if (i - 1 < 6) {
                                        std::string argStr = getOperand(instr.args[i]);
                                        if (isMem(argStr)) {
                                            addText("    mov rax, " + argStr);
                                            addText("    mov " + std::string(argRegs[i - 1]) + ", rax");
                                        } else {
                                            addText("    mov " + std::string(argRegs[i - 1]) + ", " + argStr);
                                        }
                                    }
                                }
                                addText("    call " + a1);
                                if (instr.dest && !d.empty() && d != "none") addText("    mov " + d + ", rax");
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
