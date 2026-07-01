#include "cider_codegen.h"
#include "cider_reg_alloc.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <map>
#include <set>

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

            std::vector<std::string> argRegs = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};

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
                
                int spillBase = currentAllocaOffset;
                int totalStack = spillBase + regAllocStackSize;
                
                if (totalStack % 16 != 0) {
                    totalStack += 16 - (totalStack % 16);
                }

                int extraStack = totalStack - savedRegsSize;
                if (extraStack > 0) {
                    addText("    sub rsp, " + std::to_string(extraStack));
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
                                int offset = spillBase + (-a.stackOffset);
                                return "[rbp - " + std::to_string(offset) + "]";
                            }
                            return a.reg;
                        }
                        return val.name;
                    }
                };

                auto getSizedReg = [](const std::string& reg, IRType t) -> std::string {
                    if (reg == "none") return reg;
                    if (reg.find("[") != std::string::npos) {
                        std::string prefix = "qword ";
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
                    else if (t == IRType::I64 || t == IRType::U64 || t == IRType::PTR) idx = 3;
                    return regMap[reg][idx];
                };

                auto genEpilogue = [&]() {
                    if (calleeSaved.size() > 0) {
                        addText("    lea rsp, [rbp - " + std::to_string(calleeSaved.size() * 8) + "]");
                        for (auto it = calleeSaved.rbegin(); it != calleeSaved.rend(); ++it) {
                            addText("    pop " + *it);
                        }
                    } else {
                        addText("    mov rsp, rbp");
                    }
                    addText("    pop rbp");
                    addText("    ret");
                };

                bool firstBlock = true;
                for (auto& block : func->getBlocks()) {
                    addText("." + block->getName() + ":");
                    if (firstBlock) {
                        firstBlock = false;
                        const auto& params = func->getParams();
                        int numParams = std::min((int)params.size(), 6);
                        
                        if (fName == "_start") {
                            // _start is called by the kernel, not by a call instruction.
                            // The kernel passes argc on the stack at [rsp] and the argv array
                            // starts at [rsp+8]. After our prologue, these are at [rbp+8] and
                            // [rbp+16] respectively.
                            for (int i = 0; i < numParams; ++i) {
                                if (i == 0) {
                                    addText("    mov r11, [rbp + 8]");
                                } else if (i == 1) {
                                    addText("    lea r11, [rbp + 16]");
                                } else {
                                    addText("    xor r11, r11");
                                }
                                if (alloc.count(params[i])) {
                                    auto& a = alloc.at(params[i]);
                                    if (a.isSpilled) {
                                        int offset = spillBase + (-a.stackOffset);
                                        addText("    mov qword [rbp - " + std::to_string(offset) + "], r11");
                                    } else {
                                        addText("    mov " + a.reg + ", r11");
                                    }
                                }
                            }
                        } else {
                            // Push parameters to stack to avoid clobbering each other
                            for (int i = 0; i < numParams; ++i) {
                                addText("    push " + argRegs[i]);
                            }
                            
                            // Pop into their allocated locations in reverse order
                            for (int i = numParams - 1; i >= 0; --i) {
                                addText("    pop r11");
                                if (alloc.count(params[i])) {
                                    auto& a = alloc.at(params[i]);
                                    if (a.isSpilled) {
                                        int offset = spillBase + (-a.stackOffset);
                                        addText("    mov qword [rbp - " + std::to_string(offset) + "], r11");
                                    } else {
                                        addText("    mov " + a.reg + ", r11");
                                    }
                                }
                            }
                        }
                    }

                    for (auto& instr : block->getInstructions()) {
                        std::string d = (instr.dest ? getOperand(*instr.dest) : "");
                        std::string a1 = (instr.args.size() > 0 ? getOperand(instr.args[0]) : "");
                        std::string a2 = (instr.args.size() > 1 ? getOperand(instr.args[1]) : "");

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
                                            // 32-to-64 bit sign extension if needed
                                            if (t == IRType::I64 || t == IRType::U64 || t == IRType::PTR) {
                                                if (instr.args[0].kind != IRValue::CONSTANT && !isMem(a1) && (instr.args[0].type == IRType::I32 || instr.args[0].type == IRType::U32)) {
                                                    addText("    movsxd " + getSizedReg(d, t) + ", " + getSizedReg(a1, IRType::I32));
                                                } else {
                                                    addText("    mov " + getSizedReg(d, t) + ", " + getSizedReg(a1, t));
                                                }
                                            } else {
                                                addText("    mov " + getSizedReg(d, t) + ", " + getSizedReg(a1, t));
                                            }
                                        }
                                    }
                                }
                                break;
                            }
                            case IROp::ADD:
                            case IROp::SUB: {
                                std::string opName = (instr.op == IROp::ADD ? "add" : "sub");
                                IRType t = (instr.dest ? instr.dest->type : IRType::I32);
                                bool is64 = (t == IRType::I64 || t == IRType::U64 || t == IRType::PTR);

                                // Load LHS into rax with sign-extension if needed
                                IRType t1 = (instr.args.size() > 0 ? instr.args[0].type : IRType::I32);
                                if (is64 && (t1 == IRType::I32 || t1 == IRType::U32)) {
                                    if (isMem(a1)) addText("    movsxd rax, dword " + a1);
                                    else addText("    movsxd rax, " + getSizedReg(a1, IRType::I32));
                                } else {
                                    addText("    mov " + getSizedReg("rax", t) + ", " + getSizedReg(a1, t));
                                }

                                // Load RHS into r11 with sign-extension if needed
                                IRType t2 = (instr.args.size() > 1 ? instr.args[1].type : IRType::I32);
                                if (is64 && (t2 == IRType::I32 || t2 == IRType::U32)) {
                                    if (isMem(a2)) addText("    movsxd r11, dword " + a2);
                                    else if (instr.args[1].kind == IRValue::CONSTANT) addText("    mov r11, " + a2);
                                    else addText("    movsxd r11, " + getSizedReg(a2, IRType::I32));
                                } else {
                                    addText("    mov " + getSizedReg("r11", t) + ", " + getSizedReg(a2, t));
                                }

                                addText("    " + opName + " " + getSizedReg("rax", t) + ", " + getSizedReg("r11", t));
                                addText("    mov " + getSizedReg(d, t) + ", " + getSizedReg("rax", t));
                                break;
                            }
                            case IROp::MUL: {
                                IRType t = (instr.dest ? instr.dest->type : IRType::I32);
                                bool is64 = (t == IRType::I64 || t == IRType::U64 || t == IRType::PTR);
                                std::string destReg = is64 ? "rax" : "eax";
                                std::string tempReg = is64 ? "r11" : "r11d";
                                if (instr.args[1].kind == IRValue::CONSTANT) {
                                    addText("    mov " + tempReg + ", " + a2);
                                    addText("    mov " + destReg + ", " + getSizedReg(a1, t));
                                    addText("    imul " + destReg + ", " + tempReg);
                                } else {
                                    addText("    mov " + destReg + ", " + getSizedReg(a1, t));
                                    addText("    imul " + destReg + ", " + getSizedReg(a2, t));
                                }
                                addText("    mov " + getSizedReg(d, t) + ", " + destReg);
                                break;
                            }
                            case IROp::DIV:
                            case IROp::MOD: {
                                std::vector<std::string> clobbered = {"rax", "rdx"};
                                std::vector<std::string> toSave;
                                for (const auto& reg : clobbered) {
                                    if (reg == d) continue;
                                    for (const auto& [var, res] : alloc) {
                                        if (!res.isSpilled && res.reg == reg) {
                                            toSave.push_back(reg); break;
                                        }
                                    }
                                }
                                for (const auto& reg : toSave) addText("    push " + reg);
                                IRType t = (instr.dest ? instr.dest->type : IRType::I32);
                                bool is64 = (t == IRType::I64 || t == IRType::U64 || t == IRType::PTR);
                                if (is64) {
                                    addText("    mov r11, " + getSizedReg(a2, t));
                                    addText("    mov rax, " + getSizedReg(a1, t));
                                    addText("    cqo");
                                    addText("    idiv r11");
                                    addText("    mov " + getSizedReg(d, t) + ", " + (instr.op == IROp::DIV ? "rax" : "rdx"));
                                } else {
                                    addText("    mov r11d, " + getSizedReg(a2, t));
                                    addText("    mov eax, " + getSizedReg(a1, t));
                                    addText("    cdq");
                                    addText("    idiv r11d");
                                    addText("    mov " + getSizedReg(d, t) + ", " + (instr.op == IROp::DIV ? "eax" : "edx"));
                                }
                                for (auto it = toSave.rbegin(); it != toSave.rend(); ++it) addText("    pop " + *it);
                                break;
                            }
                            case IROp::BIT_AND:
                            case IROp::BIT_OR:
                            case IROp::BIT_XOR:
                            case IROp::LSHIFT:
                            case IROp::RSHIFT: {
                                std::string opName = (instr.op == IROp::BIT_AND ? "and" : (instr.op == IROp::BIT_OR ? "or" : (instr.op == IROp::BIT_XOR ? "xor" : (instr.op == IROp::LSHIFT ? "shl" : "shr"))));
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
                                    if (instr.args[1].kind == IRValue::CONSTANT) addText("    " + opName + " " + getSizedReg(d, t) + ", " + a2);
                                    else { addText("    mov rcx, " + a2); addText("    " + opName + " " + getSizedReg(d, t) + ", cl"); }
                                } else {
                                    if (isMem(d) && isMem(a2)) {
                                        addText("    mov " + getSizedReg("rax", t) + ", " + getSizedReg(a2, t));
                                        addText("    " + opName + " " + getSizedReg(d, t) + ", " + getSizedReg("rax", t));
                                    } else addText("    " + opName + " " + getSizedReg(d, t) + ", " + getSizedReg(a2, t));
                                }
                                break;
                            }
                            case IROp::BIT_NOT: {
                                IRType t = (instr.dest ? instr.dest->type : IRType::I32);
                                if (d != a1) {
                                    if (isMem(d) && isMem(a1)) {
                                        addText("    mov " + getSizedReg("rax", t) + ", " + getSizedReg(a1, t));
                                        addText("    mov " + getSizedReg(d, t) + ", " + getSizedReg("rax", t));
                                    } else addText("    mov " + getSizedReg(d, t) + ", " + getSizedReg(a1, t));
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
                                IRType t2 = instr.args[1].type;
                                bool is64 = (t == IRType::I64 || t == IRType::U64 || t == IRType::PTR || t2 == IRType::I64 || t2 == IRType::U64 || t2 == IRType::PTR);

                                if (is64 && (t == IRType::I32 || t == IRType::U32)) {
                                    if (isMem(a1)) addText("    movsxd rax, dword " + a1);
                                    else addText("    movsxd rax, " + getSizedReg(a1, IRType::I32));
                                } else {
                                    addText("    mov " + getSizedReg("rax", is64 ? IRType::I64 : t) + ", " + getSizedReg(a1, t));
                                }

                                if (is64 && (t2 == IRType::I32 || t2 == IRType::U32)) {
                                    if (isMem(a2)) addText("    movsxd r11, dword " + a2);
                                    else if (instr.args[1].kind == IRValue::CONSTANT) addText("    mov r11, " + a2);
                                    else addText("    movsxd r11, " + getSizedReg(a2, IRType::I32));
                                } else {
                                    addText("    mov " + getSizedReg("r11", is64 ? IRType::I64 : t2) + ", " + getSizedReg(a2, t2));
                                }

                                addText("    cmp " + getSizedReg("rax", is64 ? IRType::I64 : t) + ", " + getSizedReg("r11", is64 ? IRType::I64 : t));
                                bool isUnsigned = (t == IRType::U8 || t == IRType::U16 || t == IRType::U32 || t == IRType::U64 || t == IRType::PTR);
                                std::string setop = (instr.op == IROp::CMP_EQ ? "sete" : (instr.op == IROp::CMP_NE ? "setne" : (instr.op == IROp::CMP_LT ? (isUnsigned ? "setb" : "setl") : (instr.op == IROp::CMP_GT ? (isUnsigned ? "seta" : "setg") : (instr.op == IROp::CMP_LE ? (isUnsigned ? "setbe" : "setle") : (isUnsigned ? "setae" : "setge"))))));
                                addText("    " + setop + " al");
                                addText("    movzx rax, al");
                                addText("    mov " + d + ", rax");
                                break;
                            }
                            case IROp::BR: addText("    jmp ." + a1); break;
                            case IROp::COND_BR: {
                                if (instr.args[0].kind == IRValue::CONSTANT) {
                                    addText("    mov rax, " + a1);
                                    addText("    cmp rax, 0");
                                } else if (isMem(a1)) {
                                    addText("    mov rax, " + a1);
                                    addText("    cmp rax, 0");
                                } else {
                                    addText("    cmp " + a1 + ", 0");
                                }
                                addText("    jne ." + a2);
                                if (instr.args.size() > 2) addText("    jmp ." + getOperand(instr.args[2]));
                                break;
                            }
                            case IROp::ASM: {
                                std::string code = a1;
                                if (code.size() >= 2 && code.front() == '"' && code.back() == '"') code = code.substr(1, code.size() - 2);
                                
                                std::vector<std::string> callerSavedAsm = {"rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11"};
                                std::vector<std::string> toSaveAsm;
                                for (const auto& reg : callerSavedAsm) {
                                    for (const auto& [var, res] : alloc) { if (!res.isSpilled && res.reg == reg) { toSaveAsm.push_back(reg); break; } }
                                }
                                
                                if (toSaveAsm.size() % 2 != 0) addText("    sub rsp, 8");
                                for (const auto& reg : toSaveAsm) addText("    push " + reg);
                                
                                // REPLACE placeholders like {s}, {len}, {code} literally
                                for (auto& pair : instr.asmMap) {
                                    std::string replacement = getOperand(pair.second);
                                    size_t pos = 0;
                                    std::string search = "{" + pair.first + "}";
                                    while ((pos = code.find(search, pos)) != std::string::npos) {
                                        code.replace(pos, search.length(), replacement);
                                        pos += replacement.length();
                                    }
                                }
                                
                                // Split multi-line ASM and indent each line
                                std::stringstream ss(code);
                                std::string line;
                                while (std::getline(ss, line)) {
                                    if (!line.empty()) addText("    " + line);
                                }
                                
                                for (auto it = toSaveAsm.rbegin(); it != toSaveAsm.rend(); ++it) addText("    pop " + *it);
                                if (toSaveAsm.size() % 2 != 0) addText("    add rsp, 8");
                                break;
                            }
                            case IROp::ALLOCA: {
                                std::string varName = instr.dest->name;
                                addText("    lea r10, [rbp - " + std::to_string(localOffsets[varName]) + "]");
                                addText("    mov " + getSizedReg(d, IRType::PTR) + ", r10");
                                break;
                            }
                            case IROp::LOAD: {
                                std::string ptr = a1;
                                IRType t = (instr.dest ? instr.dest->type : IRType::I32);
                                std::string sizePrefix = "qword ";
                                if (t == IRType::I8 || t == IRType::U8) sizePrefix = "byte ";
                                else if (t == IRType::I16 || t == IRType::U16) sizePrefix = "word ";
                                else if (t == IRType::I32 || t == IRType::U32) sizePrefix = "dword ";
                                else if (t == IRType::I64 || t == IRType::U64 || t == IRType::PTR) sizePrefix = "qword ";
                                std::string srcReg = getSizedReg("rax", t);
                                std::string addrReg = "r10";
                                if (isMem(ptr)) addText("    mov r10, " + ptr); else addrReg = ptr;
                                if (instr.args.size() > 1 && instr.args[1].kind == IRValue::CONSTANT) addText("    mov " + srcReg + ", " + sizePrefix + "[" + addrReg + " + " + std::to_string((int)instr.args[1].constValue) + "]");
                                else if (instr.args[0].kind == IRValue::LABEL) addText("    mov " + srcReg + ", " + sizePrefix + "[rel " + instr.args[0].name + "]");
                                else addText("    mov " + srcReg + ", " + sizePrefix + "[" + addrReg + "]");
                                addText("    mov " + getSizedReg(d, t) + ", " + srcReg);
                                break;
                            }
                            case IROp::STORE: {
                                std::string ptr = a1; std::string val = a2;
                                IRType t = (instr.args.size() > 1 ? instr.args[1].type : IRType::I32);
                                if (instr.args[1].kind == IRValue::LABEL) t = IRType::PTR;
                                std::string sizePrefix = "qword ";
                                if (t == IRType::I8 || t == IRType::U8) sizePrefix = "byte ";
                                else if (t == IRType::I16 || t == IRType::U16) sizePrefix = "word ";
                                else if (t == IRType::I32 || t == IRType::U32) sizePrefix = "dword ";
                                else if (t == IRType::I64 || t == IRType::U64 || t == IRType::PTR) sizePrefix = "qword ";
                                std::string srcReg = getSizedReg("rax", t);
                                if (instr.args[0].kind == IRValue::LABEL) {
                                    if (instr.args[1].kind == IRValue::LABEL) { addText("    lea rax, [rel " + val + "]"); addText("    mov qword [rel " + instr.args[0].name + "], rax"); }
                                    else { addText("    mov rax, " + val); addText("    mov qword [rel " + instr.args[0].name + "], rax"); }
                                } else {
                                    std::string addrReg = "r11";
                                    if (isMem(ptr)) {
                                        addText("    mov r11, " + ptr);
                                    } else {
                                        addrReg = ptr;
                                    }
                                    if (instr.args.size() > 2) addText("    add " + addrReg + ", " + getOperand(instr.args[2]));
                                    
                                    if (instr.args[1].kind == IRValue::LABEL) {
                                        addText("    lea rax, [rel " + val + "]");
                                    } else {
                                        if (isMem(getSizedReg(val, t))) {
                                            addText("    mov " + getSizedReg("rax", t) + ", " + getSizedReg(val, t));
                                        } else {
                                            addText("    mov " + getSizedReg("rax", t) + ", " + getSizedReg(val, t));
                                        }
                                    }
                                    addText("    mov " + sizePrefix + "[" + addrReg + "], " + getSizedReg("rax", t));
                                }
                                break;
                            }
                            case IROp::CALL:
                            case IROp::VIRTUAL_CALL: {
                                std::vector<std::string> callerSaved = {"rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11"};
                                std::vector<std::string> toSave;
                                for (const auto& reg : callerSaved) {
                                    for (const auto& [var, res] : alloc) { if (!res.isSpilled && res.reg == reg) { if (instr.dest && instr.dest->name == var && reg == "rax") continue; toSave.push_back(reg); break; } }
                                }
                                
                                // displacement = 8(rbp) + 40(callee) + extraStack + toSave*8
                                int displacement = savedRegsSize + extraStack + (toSave.size() * 8);
                                int padding = (displacement % 16 != 0) ? 8 : 0;

                                if (padding > 0) addText("    sub rsp, " + std::to_string(padding));
                                for (const auto& reg : toSave) addText("    push " + reg);
                                
                                if (instr.op == IROp::CALL) {
                                    if (instr.args[0].kind == IRValue::LABEL) {
                                        // Direct call: call label
                                        for (size_t i = 1; i < instr.args.size() && (i - 1) < argRegs.size(); ++i) {
                                            addText("    push " + getOperand(instr.args[i]));
                                        }
                                        for (int i = std::min((int)instr.args.size() - 1, 6) - 1; i >= 0; --i) {
                                            addText("    pop " + argRegs[i]);
                                        }
                                        addText("    call " + a1);
                                    } else {
                                        // Indirect call: save target in r11 (not an arg reg), set up args, call r11
                                        std::string target = getOperand(instr.args[0]);
                                        addText("    mov r11, " + target);
                                        for (size_t i = 1; i < instr.args.size() && (i - 1) < argRegs.size(); ++i) {
                                            addText("    push " + getOperand(instr.args[i]));
                                        }
                                        for (int i = std::min((int)instr.args.size() - 1, 6) - 1; i >= 0; --i) {
                                            addText("    pop " + argRegs[i]);
                                        }
                                        addText("    call r11");
                                    }
                                } else {
                                    // VIRTUAL_CALL: args[0] = objPtr, args[1] = vtableIndex, args[2...] = params
                                    std::string objPtr = getOperand(instr.args[0]);
                                    int vtableIndex = (int)instr.args[1].constValue;
                                    
                                    // Load vtable and method FIRST
                                    if (isMem(objPtr)) {
                                        addText("    mov r10, " + objPtr);
                                        addText("    mov rax, [r10]");
                                    } else {
                                        addText("    mov rax, [" + objPtr + "]");
                                    }
                                    addText("    mov r11, [rax + " + std::to_string(vtableIndex * 8) + "]");
                                    
                                    // Push all params including 'this'
                                    addText("    push " + objPtr);
                                    for (size_t i = 2; i < instr.args.size() && (i - 1) < argRegs.size(); ++i) {
                                        addText("    push " + getOperand(instr.args[i]));
                                    }
                                    
                                    // Pop into ABI regs (rdi, rsi, rdx...)
                                    int totalVCArgs = std::min((int)instr.args.size() - 1, 6);
                                    for (int i = totalVCArgs - 1; i >= 0; --i) {
                                        addText("    pop " + argRegs[i]);
                                    }
                                    
                                    addText("    call r11");
                                }
                                for (auto it = toSave.rbegin(); it != toSave.rend(); ++it) addText("    pop " + *it);
                                if (padding > 0) addText("    add rsp, " + std::to_string(padding));
                                if (instr.dest && !d.empty() && d != "none") addText("    mov " + getSizedReg(d, instr.dest->type) + ", " + getSizedReg("rax", instr.dest->type));
                                break;
                            }
                            case IROp::PUSH: {
                                if (instr.args[0].kind == IRValue::CONSTANT) {
                                    addText("    push " + std::to_string((long long)instr.args[0].constValue));
                                } else {
                                    addText("    push " + a1);
                                }
                                break;
                            }
                            case IROp::POP: {
                                addText("    pop " + d);
                                break;
                            }
                            case IROp::RET: {
                                if (instr.args.size() > 0) {
                                    IRType t = instr.args[0].type;
                                    addText("    mov " + getSizedReg("rax", t) + ", " + getSizedReg(a1, t));
                                }
                                genEpilogue();
                                break;
                            }
                            default: break;
                        }
                    }
                }
                genEpilogue();
            }

            std::ofstream out(outputPath, std::ios::trunc);
            if (!out) return false;
            out << "section .bss\n";
            for (const auto& line : bssSection) out << "    " << line << "\n";
            for (const auto& line : globalAsmSection) out << "    " << line << "\n";
            out << "\nsection .data\n";
            for (const auto& line : dataSection) out << "    " << line << "\n";
            out << "\nsection .text\n    extern malloc\n    extern _setjmp\n    extern longjmp\n";
            for (const auto& extFunc : module.getExternFunctions()) {
                out << "    extern " << extFunc << "\n";
            }
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
