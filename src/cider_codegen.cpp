#include "cider_codegen.h"
#include "cider_reg_alloc.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <functional>
#include <map>
#include <set>

namespace Cider {

    class X86_64_Linux_Backend : public TargetBackend {
    public:
        bool generate(ModuleIR& module, const std::string& outputPath,
                      const std::string& sourceFile = "") override {
            std::vector<std::string> bssSection;
            std::vector<std::string> dataSection;
            std::vector<std::string> globalAsmSection;
            std::vector<std::string> textSection;
            int strCount = 0;
            std::set<std::string> generatedFunctions_;

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

            std::map<double, std::string> floatConsts;
            int floatConstCount = 0;

            auto getFloatConst = [&](double val, IRType t) -> std::string {
                auto it = floatConsts.find(val);
                if (it != floatConsts.end()) return it->second;
                std::string label = "fc" + std::to_string(floatConstCount++);
                floatConsts[val] = label;
                if (t == IRType::F32)
                    addData(label + ": dd " + std::to_string(val));
                else
                    addData(label + ": dq " + std::to_string(val));
                return label;
            };

            RegisterAllocator regAlloc;
            
            for (auto& func : module.getFunctions()) {
                for (auto& block : func->getBlocks()) {
                    for (auto& instr : block->getInstructions()) {
                        for (auto& arg : instr.args) {
                            if (arg.kind == IRValue::CONSTANT && arg.type == IRType::PTR && arg.name.front() == '"') {
                                std::string label = "str" + std::to_string(strCount++);
                                std::string sanitized = sanitizeString(arg.name);
                                if (sanitized.empty())
                                    addData(label + ": db 0");
                                else
                                    addData(label + ": db " + sanitized + ", 0");
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

                int count = var.arraySize > 0 ? var.arraySize : 1;
                if (var.initialValue != 0) {
                    addData(var.name + ": " + sizeDir + " " + std::to_string((long long)var.initialValue));
                } else {
                    addBss(var.name + ": " + resDir + " " + std::to_string(count));
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
                if (generatedFunctions_.count(func->getName())) continue;
                generatedFunctions_.insert(func->getName());

                // Skip functions not belonging to the target source file (separate compilation mode)
                if (!sourceFile.empty() && func->getSourcePath() != sourceFile &&
                    func->getSourcePath() != "") {
                    addGlobalAsm("extern " + func->getName());
                    continue;
                }

                int regAllocStackSize = 0;
                auto alloc = regAlloc.allocate(*func, regAllocStackSize);
                
                std::string fName = func->getName();
                if (fName == "_start" || fName == "main") {
                    addText("global " + fName);
                }
                if (!sourceFile.empty()) {
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
                int totalStack = spillBase + regAllocStackSize + 8; // +8 to protect locals from call pushes
                
                if (totalStack % 16 != 0) {
                    totalStack += 16 - (totalStack % 16);
                }

                int extraStack = totalStack - savedRegsSize;
                if (extraStack > 0) {
                    addText("    sub rsp, " + std::to_string(extraStack));
                }

                auto getOperand = [&](const IRValue& val) -> std::string {
                    if (val.kind == IRValue::CONSTANT) {
                        if (val.type == IRType::F32 || val.type == IRType::F64) return "[rel " + getFloatConst(val.constValue, val.type) + "]";
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

                auto pushArg = [&](const IRValue& arg) {
                    if (arg.kind == IRValue::LABEL) {
                        addText("    mov rax, " + arg.name);
                        addText("    push rax");
                    } else if (arg.kind == IRValue::REGISTER && arg.type == IRType::I32 &&
                               isMem(getOperand(arg))) {
                        // A spilled I32 arg's slot only has 4 valid bytes; the
                        // upper 4 hold stale stack data. Pushing the raw slot
                        // would pass a garbage 64-bit value (e.g. 0x7fff00000005).
                        addText("    movsxd rax, dword " + getOperand(arg));
                        addText("    push rax");
                    } else if (arg.kind == IRValue::REGISTER && arg.type == IRType::U32 &&
                               isMem(getOperand(arg))) {
                        addText("    mov eax, dword " + getOperand(arg));
                        addText("    push rax");
                    } else {
                        addText("    push " + getOperand(arg));
                    }
                };

                auto getSizedReg = [](const std::string& reg, IRType t) -> std::string {
                    if (reg == "none") return reg;
                    if (reg.size() >= 3 && reg[0] == 'x' && reg[1] == 'm' && reg[2] == 'm') return reg;
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

                // Map every register to its defining instruction so we can tell
                // whether a STORE's address is an 8-byte local slot (alloca-based)
                // or a native-width heap/array element.
                std::map<std::string, const IRInstruction*> regDefs;
                for (auto& db : func->getBlocks()) {
                    for (auto& di : db->getInstructions()) {
                        if (di.dest && di.dest->kind == IRValue::REGISTER) {
                            regDefs[di.dest->name] = &di;
                        }
                    }
                }
                std::function<bool(const std::string&)> isAllocaBased =
                    [&](const std::string& reg) -> bool {
                    std::set<std::string> seen;
                    std::string cur = reg;
                    long long totalOffset = 0;
                    while (!cur.empty() && !seen.count(cur)) {
                        seen.insert(cur);
                        auto it = regDefs.find(cur);
                        if (it == regDefs.end()) return false;
                        const IRInstruction* def = it->second;
                        if (def->op == IROp::ALLOCA) {
                            // The qword+extend narrow store is only safe for a
                            // plain local variable's own 8-byte slot: offset 0
                            // AND alloca size 8. Struct fields / array elements
                            // (offset > 0, or any store into a larger alloca)
                            // are 4-byte packed — a qword store there would
                            // clobber the neighbor.
                            double allocSize = def->args.empty() ? 0.0 : def->args[0].constValue;
                            return totalOffset == 0 && allocSize == 8.0;
                        }
                        if (def->op == IROp::ADD) {
                            if (!def->args.empty() && def->args[0].kind == IRValue::REGISTER) {
                                if (def->args.size() > 1 && def->args[1].kind == IRValue::CONSTANT) {
                                    totalOffset += (long long)def->args[1].constValue;
                                }
                                cur = def->args[0].name;
                                continue;
                            }
                            return false;
                        }
                        if (def->op == IROp::MOV) {
                            if (!def->args.empty() && def->args[0].kind == IRValue::REGISTER) {
                                cur = def->args[0].name;
                                continue;
                            }
                        }
                        return false;
                    }
                    return false;
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
                            // Push register parameters to stack to avoid clobbering each other
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
                            // Load stack parameters (args 7+) from [rbp+16], [rbp+24], ...
                            for (int i = 6; i < (int)params.size(); ++i) {
                                addText("    mov r11, [rbp + " + std::to_string(8 + (i - 6 + 1) * 8) + "]");
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
                            case IROp::CVT_ITOF: {
                                IRType t = (instr.dest ? instr.dest->type : IRType::F32);
                                addText("    mov eax, " + getSizedReg(a1, IRType::I32));
                                if (t == IRType::F32)
                                    addText("    cvtsi2ss xmm0, eax");
                                else
                                    addText("    cvtsi2sd xmm0, eax");
                                addText("    movss " + d + ", xmm0");
                                break;
                            }
                            case IROp::CVT_FTOI: {
                                IRType t = (instr.dest ? instr.dest->type : IRType::I32);
                                IRType srcType = (instr.args.size() > 0 ? instr.args[0].type : IRType::F32);
                                std::string dstReg = getSizedReg(d, t);
                                if (isMem(dstReg)) {
                                    if (srcType == IRType::F64) {
                                        addText("    movsd xmm0, " + a1);
                                        addText("    cvttsd2si eax, xmm0");
                                    } else {
                                        addText("    movss xmm0, " + a1);
                                        addText("    cvttss2si eax, xmm0");
                                    }
                                    addText("    mov " + dstReg + ", eax");
                                } else {
                                    if (srcType == IRType::F64) {
                                        addText("    movsd xmm0, " + a1);
                                        addText("    cvttsd2si " + dstReg + ", xmm0");
                                    } else {
                                        addText("    movss xmm0, " + a1);
                                        addText("    cvttss2si " + dstReg + ", xmm0");
                                    }
                                }
                                break;
                            }
                            case IROp::MOV: {
                                if (d != a1) {
                                    IRType t = (instr.dest ? instr.dest->type : IRType::I32);
                                    if (t == IRType::F32 || t == IRType::F64) {
                                        std::string mv = (t == IRType::F32) ? "movss" : "movsd";
                                        std::string sz = (t == IRType::F32) ? "dword " : "qword ";
                                        if (isMem(d)) {
                                            if (isMem(a1)) {
                                                addText("    " + mv + " xmm0, " + sz + a1);
                                                addText("    " + mv + " " + sz + d + ", xmm0");
                                            } else {
                                                addText("    " + mv + " " + sz + d + ", " + a1);
                                            }
                                        } else {
                                            addText("    " + mv + " " + d + ", " + (isMem(a1) ? sz + a1 : a1));
                                        }
                                    } else if (instr.args[0].kind == IRValue::LABEL) {
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
                                                if (instr.args[0].kind != IRValue::CONSTANT && (instr.args[0].type == IRType::I32 || instr.args[0].type == IRType::U32)) {
                                                    if (isMem(d)) {
                                                        // movsxd cannot write to memory — route through rax
                                                        // whenever the destination is spilled.
                                                        if (isMem(a1)) {
                                                            addText("    movsxd rax, dword " + a1);
                                                        } else {
                                                            addText("    movsxd rax, " + getSizedReg(a1, IRType::I32));
                                                        }
                                                        addText("    mov " + getSizedReg(d, t) + ", rax");
                                                    } else if (isMem(a1)) {
                                                        // Spilled I32 source: read dword and sign-extend so the
                                                        // upper 32 bits of the qword load don't carry stack garbage.
                                                        addText("    movsxd " + getSizedReg(d, t) + ", dword " + a1);
                                                    } else {
                                                        addText("    movsxd " + getSizedReg(d, t) + ", " + getSizedReg(a1, IRType::I32));
                                                    }
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
                                IRType t = (instr.dest ? instr.dest->type : IRType::I32);
                                if (t == IRType::F32) {
                                    std::string op = (instr.op == IROp::ADD ? "addss" : "subss");
                                    addText("    movss xmm0, " + a1);
                                    addText("    " + op + " xmm0, " + a2);
                                    addText("    movss " + d + ", xmm0");
                                } else if (t == IRType::F64) {
                                    std::string op = (instr.op == IROp::ADD ? "addsd" : "subsd");
                                    addText("    movsd xmm0, " + a1);
                                    addText("    " + op + " xmm0, " + a2);
                                    addText("    movsd " + d + ", xmm0");
                                } else {
                                std::string opName = (instr.op == IROp::ADD ? "add" : "sub");
                                bool is64 = (t == IRType::I64 || t == IRType::U64 || t == IRType::PTR);
                                IRType t1 = (instr.args.size() > 0 ? instr.args[0].type : IRType::I32);
                                if (is64 && (t1 == IRType::I32 || t1 == IRType::U32)) {
                                    if (isMem(a1)) addText("    movsxd rax, dword " + a1);
                                    else if (instr.args[0].kind == IRValue::CONSTANT) addText("    mov rax, " + a1);
                                    else addText("    movsxd rax, " + getSizedReg(a1, IRType::I32));
                                } else {
                                    addText("    mov " + getSizedReg("rax", t) + ", " + getSizedReg(a1, t));
                                }
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
                                }
                                break;
                            }
                            case IROp::MUL: {
                                IRType t = (instr.dest ? instr.dest->type : IRType::I32);
                                if (t == IRType::F32) {
                                    addText("    movss xmm0, " + a1);
                                    addText("    mulss xmm0, " + a2);
                                    addText("    movss " + d + ", xmm0");
                                } else if (t == IRType::F64) {
                                    addText("    movsd xmm0, " + a1);
                                    addText("    mulsd xmm0, " + a2);
                                    addText("    movsd " + d + ", xmm0");
                                } else {
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
                                }
                                break;
                            }
                            case IROp::DIV:
                            case IROp::MOD: {
                                IRType t = (instr.dest ? instr.dest->type : IRType::I32);
                                if (t == IRType::F32) {
                                    addText("    movss xmm0, " + a1);
                                    addText("    divss xmm0, " + a2);
                                    addText("    movss " + d + ", xmm0");
                                    break;
                                } else if (t == IRType::F64) {
                                    addText("    movsd xmm0, " + a1);
                                    addText("    divsd xmm0, " + a2);
                                    addText("    movsd " + d + ", xmm0");
                                    break;
                                }
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
                                t = (instr.dest ? instr.dest->type : IRType::I32);
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
                                if (t == IRType::F32) {
                                    std::string setcc;
                                    switch (instr.op) {
                                        case IROp::CMP_EQ: setcc = "sete";  break;
                                        case IROp::CMP_NE: setcc = "setne"; break;
                                        case IROp::CMP_LT: setcc = "setb";  break;
                                        case IROp::CMP_LE: setcc = "setbe"; break;
                                        case IROp::CMP_GT: setcc = "seta";  break;
                                        case IROp::CMP_GE: setcc = "setae"; break;
                                        default: setcc = "sete";
                                    }
                                    if (isMem(a1)) {
                                        addText("    movss xmm0, " + a1);
                                        addText("    ucomiss xmm0, " + a2);
                                    } else if (isMem(a2)) {
                                        addText("    movss xmm0, " + a2);
                                        addText("    ucomiss " + a1 + ", xmm0");
                                    } else {
                                        addText("    ucomiss " + a1 + ", " + a2);
                                    }
                                    addText("    " + setcc + " al");
                                    addText("    movzx eax, al");
                                    addText("    mov " + d + ", rax");
                                    break;
                                } else if (t == IRType::F64) {
                                    std::string setcc;
                                    switch (instr.op) {
                                        case IROp::CMP_EQ: setcc = "sete";  break;
                                        case IROp::CMP_NE: setcc = "setne"; break;
                                        case IROp::CMP_LT: setcc = "setb";  break;
                                        case IROp::CMP_LE: setcc = "setbe"; break;
                                        case IROp::CMP_GT: setcc = "seta";  break;
                                        case IROp::CMP_GE: setcc = "setae"; break;
                                        default: setcc = "sete";
                                    }
                                    if (isMem(a1)) {
                                        addText("    movsd xmm0, " + a1);
                                        addText("    ucomisd xmm0, " + a2);
                                    } else if (isMem(a2)) {
                                        addText("    movsd xmm0, " + a2);
                                        addText("    ucomisd " + a1 + ", xmm0");
                                    } else {
                                        addText("    ucomisd " + a1 + ", " + a2);
                                    }
                                    addText("    " + setcc + " al");
                                    addText("    movzx eax, al");
                                    addText("    mov " + d + ", rax");
                                    break;
                                }
                                IRType t2 = instr.args[1].type;
                                bool is64 = (t == IRType::I64 || t == IRType::U64 || t == IRType::PTR || t2 == IRType::I64 || t2 == IRType::U64 || t2 == IRType::PTR);

                                if (is64 && (t == IRType::I32 || t == IRType::U32)) {
                                    if (isMem(a1)) addText("    movsxd rax, dword " + a1);
                                    else if (instr.args[0].kind == IRValue::CONSTANT) addText("    mov rax, " + a1);
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
                                    IRType ct = instr.args[0].type;
                                    addText("    mov " + getSizedReg("rax", ct) + ", " + getSizedReg(a1, ct));
                                    addText("    cmp " + getSizedReg("rax", ct) + ", 0");
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
                                    std::string raw = getOperand(pair.second);
                                    std::string replacement = getSizedReg(raw, pair.second.type);
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
                                std::string addrReg = "r10";
                                if (isMem(ptr)) addText("    mov r10, " + ptr); else addrReg = ptr;
                                if (t == IRType::F32) {
                                    if (instr.args.size() > 1 && instr.args[1].kind == IRValue::CONSTANT)
                                        addText("    movss xmm0, dword [" + addrReg + " + " + std::to_string((int)instr.args[1].constValue) + "]");
                                    else if (instr.args[0].kind == IRValue::LABEL)
                                        addText("    movss xmm0, dword [rel " + instr.args[0].name + "]");
                                    else
                                        addText("    movss xmm0, dword [" + addrReg + "]");
                                    addText("    movss " + d + ", xmm0");
                                } else if (t == IRType::F64) {
                                    if (instr.args.size() > 1 && instr.args[1].kind == IRValue::CONSTANT)
                                        addText("    movsd xmm0, qword [" + addrReg + " + " + std::to_string((int)instr.args[1].constValue) + "]");
                                    else if (instr.args[0].kind == IRValue::LABEL)
                                        addText("    movsd xmm0, qword [rel " + instr.args[0].name + "]");
                                    else
                                        addText("    movsd xmm0, qword [" + addrReg + "]");
                                    addText("    movsd " + d + ", xmm0");
                                } else if (t == IRType::I8 || t == IRType::U8) {
                                    std::string op = (t == IRType::I8) ? "movsx" : "movzx";
                                    if (instr.args.size() > 1 && instr.args[1].kind == IRValue::CONSTANT)
                                        addText("    " + op + " eax, byte [" + addrReg + " + " + std::to_string((int)instr.args[1].constValue) + "]");
                                    else if (instr.args[0].kind == IRValue::LABEL)
                                        addText("    " + op + " eax, byte [rel " + instr.args[0].name + "]");
                                    else
                                        addText("    " + op + " eax, byte [" + addrReg + "]");
                                    addText("    mov " + getSizedReg(d, t) + ", al");
                                } else if (t == IRType::I16 || t == IRType::U16) {
                                    std::string op = (t == IRType::I16) ? "movsx" : "movzx";
                                    if (instr.args.size() > 1 && instr.args[1].kind == IRValue::CONSTANT)
                                        addText("    " + op + " eax, word [" + addrReg + " + " + std::to_string((int)instr.args[1].constValue) + "]");
                                    else if (instr.args[0].kind == IRValue::LABEL)
                                        addText("    " + op + " eax, word [rel " + instr.args[0].name + "]");
                                    else
                                        addText("    " + op + " eax, word [" + addrReg + "]");
                                    addText("    mov " + getSizedReg(d, t) + ", ax");
                                } else {
                                    std::string sizePrefix = "qword ";
                                if (t == IRType::I32 || t == IRType::U32 || t == IRType::BOOL) sizePrefix = "dword ";
                                else if (t == IRType::I64 || t == IRType::U64 || t == IRType::PTR) sizePrefix = "qword ";
                                std::string srcReg = getSizedReg("rax", t);
                                    if (instr.args.size() > 1 && instr.args[1].kind == IRValue::CONSTANT)
                                        addText("    mov " + srcReg + ", " + sizePrefix + "[" + addrReg + " + " + std::to_string((int)instr.args[1].constValue) + "]");
                                    else if (instr.args[0].kind == IRValue::LABEL)
                                        addText("    mov " + srcReg + ", " + sizePrefix + "[rel " + instr.args[0].name + "]");
                                    else
                                        addText("    mov " + srcReg + ", " + sizePrefix + "[" + addrReg + "]");
                                    addText("    mov " + getSizedReg(d, t) + ", " + srcReg);
                                }
                                break;
                            }
                            case IROp::STORE: {
                                std::string ptr = a1; std::string val = a2;
                                IRType t = (instr.args.size() > 1 ? instr.args[1].type : IRType::I32);
                                if (instr.args[1].kind == IRValue::LABEL) t = IRType::PTR;
                                if (t == IRType::F32) {
                                    std::string addrReg = "r11";
                                    addText("    mov r11, " + ptr);
                                    if (instr.args.size() > 2) addText("    add r11, " + getOperand(instr.args[2]));
                                    if (isMem(val)) addText("    movss xmm0, " + val);
                                    addText("    movss dword [r11], " + (isMem(val) ? "xmm0" : val));
                                    break;
                                } else if (t == IRType::F64) {
                                    std::string addrReg = "r11";
                                    addText("    mov r11, " + ptr);
                                    if (instr.args.size() > 2) addText("    add r11, " + getOperand(instr.args[2]));
                                    if (isMem(val)) addText("    movsd xmm0, " + val);
                                    addText("    movsd qword [r11], " + (isMem(val) ? "xmm0" : val));
                                    break;
                                } else {
                                std::string sizePrefix = "qword ";
                                if (t == IRType::I8 || t == IRType::U8) sizePrefix = "byte ";
                                else if (t == IRType::I16 || t == IRType::U16) sizePrefix = "word ";
                                else if (t == IRType::I32 || t == IRType::U32) sizePrefix = "dword ";
                                else if (t == IRType::I64 || t == IRType::U64 || t == IRType::PTR) sizePrefix = "qword ";
                                std::string srcReg = getSizedReg("rax", t);
                                // Locals are allocated as 8-byte slots and always loaded
                                // with 64-bit loads. If we store a narrow integer with a
                                // sized store (e.g. mov dword), the upper bytes keep stale
                                // stack garbage and the later 64-bit load reads garbage.
                                // So: extend narrow integer values to 64 bits, then store qword.
                                bool isNarrowInt = (t == IRType::I8 || t == IRType::U8 ||
                                                    t == IRType::I16 || t == IRType::U16 ||
                                                    t == IRType::I32 || t == IRType::U32 ||
                                                    t == IRType::BOOL);
                                if (instr.args[0].kind == IRValue::LABEL) {
                                    if (instr.args[1].kind == IRValue::LABEL) { addText("    lea rax, [rel " + val + "]"); addText("    mov qword [rel " + instr.args[0].name + "], rax"); }
                                    else {
                                        addText("    mov " + getSizedReg("rax", t) + ", " + getSizedReg(val, t));
                                        if (isNarrowInt) {
                                            if (t == IRType::I8) addText("    movsx rax, al");
                                            else if (t == IRType::U8 || t == IRType::BOOL) addText("    movzx rax, al");
                                            else if (t == IRType::I16) addText("    movsx rax, ax");
                                            else if (t == IRType::U16) addText("    movzx rax, ax");
                                            else if (t == IRType::I32) addText("    movsxd rax, eax");
                                            else addText("    mov rax, rax"); // U32 already zero-extended by mov eax
                                            addText("    mov qword [rel " + instr.args[0].name + "], rax");
                                        } else {
                                            addText("    mov " + sizePrefix + "[rel " + instr.args[0].name + "], " + getSizedReg("rax", t));
                                        }
                                    }
                                } else {
                                    std::string addrReg = "r11";
                                    addText("    mov r11, " + ptr);
                                    if (instr.args.size() > 2) addText("    add r11, " + getOperand(instr.args[2]));
                                    
                                    if (instr.args[1].kind == IRValue::LABEL) {
                                        addText("    lea rax, [rel " + val + "]");
                                        addText("    mov qword [" + addrReg + "], rax");
                                    } else {
                                        if (isMem(getSizedReg(val, t))) {
                                            addText("    mov " + getSizedReg("rax", t) + ", " + getSizedReg(val, t));
                                        } else {
                                            addText("    mov " + getSizedReg("rax", t) + ", " + getSizedReg(val, t));
                                        }
                                        if (isNarrowInt) {
                                            // 8-byte local slots (alloca-based) must be stored
                                            // full-width so later 64-bit reads don't pick up
                                            // stack garbage in the upper bytes. Heap/array
                                            // elements keep their native width — a qword store
                                            // into a 4-byte-element array clobbers the neighbor.
                                            bool slotDest = (instr.args[0].kind == IRValue::REGISTER) &&
                                                            isAllocaBased(instr.args[0].name);
                                            if (slotDest) {
                                                if (t == IRType::I8) addText("    movsx rax, al");
                                                else if (t == IRType::U8 || t == IRType::BOOL) addText("    movzx rax, al");
                                                else if (t == IRType::I16) addText("    movsx rax, ax");
                                                else if (t == IRType::U16) addText("    movzx rax, ax");
                                                else if (t == IRType::I32) addText("    movsxd rax, eax");
                                                // U32: mov eax already zero-extends rax
                                                addText("    mov qword [" + addrReg + "], rax");
                                            } else {
                                                addText("    mov " + sizePrefix + "[" + addrReg + "], " + getSizedReg("rax", t));
                                            }
                                        } else {
                                            addText("    mov " + sizePrefix + "[" + addrReg + "], " + getSizedReg("rax", t));
                                        }
                                    }
                                }
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
                                        // Push extra stack args (args 7+) in reverse order
                                        size_t numArgs = instr.args.size() - 1;
                                        for (size_t i = numArgs; i > 6; --i) {
                                            pushArg(instr.args[i]);
                                        }
                                        // Push first 6 args, pop into registers
                                        for (size_t i = 1; i <= numArgs && i <= 6; ++i) {
                                            pushArg(instr.args[i]);
                                        }
                                        for (int i = std::min((int)numArgs, 6) - 1; i >= 0; --i) {
                                            addText("    pop " + argRegs[i]);
                                        }
                                        addText("    call " + a1);
                                        // Clean up stack args
                                        if (numArgs > 6) {
                                            addText("    add rsp, " + std::to_string((numArgs - 6) * 8));
                                        }
                                    } else {
                                        // Indirect call: save target in r11 (not an arg reg), set up args, call r11
                                        std::string target = getOperand(instr.args[0]);
                                        addText("    mov r11, " + target);
                                        size_t numArgs = instr.args.size() - 1;
                                        for (size_t i = numArgs; i > 6; --i) {
                                            pushArg(instr.args[i]);
                                        }
                                        for (size_t i = 1; i <= numArgs && i <= 6; ++i) {
                                            pushArg(instr.args[i]);
                                        }
                                        for (int i = std::min((int)numArgs, 6) - 1; i >= 0; --i) {
                                            addText("    pop " + argRegs[i]);
                                        }
                                        addText("    call r11");
                                        if (numArgs > 6) {
                                            addText("    add rsp, " + std::to_string((numArgs - 6) * 8));
                                        }
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
                                if (instr.dest && !d.empty() && d != "none") {
                                    IRType retType = instr.dest->type;
                                    if (retType == IRType::F32)
                                        addText("    movss " + d + ", xmm0");
                                    else if (retType == IRType::F64)
                                        addText("    movsd " + d + ", xmm0");
                                    else
                                        addText("    mov " + getSizedReg(d, retType) + ", " + getSizedReg("rax", retType));
                                }
                                break;
                            }
                            case IROp::PUSH: {
                                if (instr.args[0].kind == IRValue::CONSTANT) {
                                    addText("    push " + std::to_string((long long)instr.args[0].constValue));
                                } else if (isMem(a1)) {
                                    IRType t = instr.args[0].type;
                                    if (t == IRType::I32 || t == IRType::U32) {
                                        addText("    movsxd rax, " + getSizedReg(a1, IRType::I32));
                                        addText("    push rax");
                                    } else {
                                        addText("    mov rax, " + getSizedReg(a1, t));
                                        addText("    push rax");
                                    }
                                } else {
                                    addText("    push " + a1);
                                }
                                break;
                            }
                            case IROp::POP: {
                                if (isMem(d)) {
                                    addText("    pop rax");
                                    addText("    mov qword " + d + ", rax");
                                } else {
                                    addText("    pop " + d);
                                }
                                break;
                            }
                            case IROp::MEMCPY: {
                                {
                                    std::string a3 = (instr.args.size() > 2 ? getOperand(instr.args[2]) : "0");
                                    addText("    mov rdi, " + a1);
                                    addText("    mov rsi, " + a2);
                                    addText("    mov rcx, " + a3);
                                    addText("    cld");
                                    addText("    rep movsb");
                                }
                                break;
                            }
                            case IROp::RET: {
                                if (instr.args.size() > 0) {
                                    IRType t = instr.args[0].type;
                                    if (t == IRType::F32)
                                        addText("    movss xmm0, " + a1);
                                    else if (t == IRType::F64)
                                        addText("    movsd xmm0, " + a1);
                                    else
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
            for (const auto& line : module.getStringData()) out << "    " << line << "\n";
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

    bool CodeGenerator::generate(ModuleIR& module, const std::string& outputPath,
                                  const std::string& sourceFile) {
        if (!backend) return false;
        return backend->generate(module, outputPath, sourceFile);
    }

} // namespace Cider
