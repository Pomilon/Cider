#include "codegen.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>

#include "reg_alloc.h"

namespace cider {

Abi targetAbiFor(const std::string& targetStr) {
    return targetStr == "uefi" ? Abi::Ms : Abi::SysV;
}

CodeGenerator::CodeGenerator(const std::string& targetStr) : target_(targetStr) {}

bool CodeGenerator::generate(ModuleIR& module, const std::string& outputPath,
                             const std::string& sourceFile) {
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
            else if (c == '\b') ss << "`, 8, `";
            else if (c == '\0') ss << "`, 0, `";
            // NASM processes C-style escapes inside backquotes (CID-023):
            // a runtime backslash must be doubled, and a backquote cannot
            // appear at all, so it becomes numeric 96. (A runtime `"` is
            // safe inside backquotes.)
            else if (c == '\\') ss << "\\\\";
            else if (c == '`') ss << "`, 96, `";
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

    // CID-003 follow-up (M6): key the float-constant pool by exact IEEE-754
    // bits, not by std::pair<double, IRType>. A double key compares with
    // operator<, which treats -0.0 and +0.0 as equivalent in both
    // directions, so a module holding both reused one label (and one sign).
    // F32 keys use the (float) bit pattern zero-extended, keeping the
    // fc32_N/fc64_N label scheme and hex payloads.
    std::map<std::pair<uint64_t, IRType>, std::string> floatConsts;
    int floatConstCount = 0;

    auto getFloatConst = [&](double val, IRType t) -> std::string {
        // CID-003: keyed by width as well as value. F32 and F64 constants
        // share a module and may hold the same numeric value (e.g. a
        // folded `2.5 as f64` next to the F32 literal 2.5); a value-only
        // key would reuse the first width's label and the other use would
        // read the wrong number of bytes.
        uint32_t f32Bits = 0;
        uint64_t bits = 0;
        if (t == IRType::F32) {
            float f = (float)val;
            std::memcpy(&f32Bits, &f, sizeof(f32Bits));
            bits = (uint64_t)f32Bits;
        } else {
            std::memcpy(&bits, &val, sizeof(bits));
        }
        auto key = std::make_pair(bits, t);
        auto it = floatConsts.find(key);
        if (it != floatConsts.end()) return it->second;
        std::string label = (t == IRType::F32 ? "fc32_" : "fc64_") +
                            std::to_string(floatConstCount++);
        floatConsts[key] = label;
        // CID-003: emit the exact IEEE-754 bit pattern as hex (uppercase,
        // like the float-global initializer path). std::to_string(double)
        // loses digits for doubles — it could never round-trip an f64
        // literal — and fixed-precision decimal text is not exact for
        // every value.
        std::ostringstream hs;
        hs << "0x" << std::hex << std::uppercase;
        if (t == IRType::F32) {
            hs << f32Bits;
            addData(label + ": dd " + hs.str());
        } else {
            hs << bits;
            addData(label + ": dq " + hs.str());
        }
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

    // Format an integer immediate for NASM (Issue #18): 32-bit values with
    // bit 31 set (e.g. 2147483648) warn as decimal — "signed dword exceeds
    // bounds" — but assemble cleanly as hex. Narrow/64-bit values are
    // unaffected (small decimals stay decimal).
    auto fmtImm = [](int64_t v, IRType t) -> std::string {
        if (t == IRType::I32 || t == IRType::U32 || t == IRType::BOOL) {
            uint64_t u = (uint64_t)v & 0xFFFFFFFFULL;
            if (u > (uint64_t)INT32_MAX) {
                std::ostringstream hs;
                hs << "0x" << std::hex << std::uppercase << u;
                return hs.str();
            }
        }
        return std::to_string(v);
    };

    for (const auto& var : module.getGlobalVars()) {
        if (var.initialIsFloat) {
            // Float initializer (CID-029): emit the exact IEEE-754 bit
            // pattern as hex. A decimal `dq 10` is an integer 10, not
            // the float 10.0.
            std::ostringstream hs;
            hs << "0x" << std::hex << std::uppercase;
            if (var.type == IRType::F32) {
                hs << (uint32_t)(var.initialValue & 0xFFFFFFFFLL);
                addData(var.name + ": dd " + hs.str());
            } else {
                hs << (uint64_t)var.initialValue;
                addData(var.name + ": dq " + hs.str());
            }
        } else if (var.initialValue != 0) {
            std::string sizeDir = "dq";
            if (var.type == IRType::I8 || var.type == IRType::U8) sizeDir = "db";
            else if (var.type == IRType::I16 || var.type == IRType::U16) sizeDir = "dw";
            else if (var.type == IRType::I32 || var.type == IRType::U32) sizeDir = "dd";
            addData(var.name + ": " + sizeDir + " " + fmtImm(var.initialValue, var.type));
        } else {
            if (var.totalBytes > 0) {
                addBss(var.name + ": resb " + std::to_string(var.totalBytes));
            } else {
                std::string resDir = "resq";
                if (var.type == IRType::I8 || var.type == IRType::U8) resDir = "resb";
                else if (var.type == IRType::I16 || var.type == IRType::U16) resDir = "resw";
                else if (var.type == IRType::I32 || var.type == IRType::U32) resDir = "resd";
                int count = var.arraySize > 0 ? var.arraySize : 1;
                addBss(var.name + ": " + resDir + " " + std::to_string(count));
            }
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

    // Declared width of every global (Issue #20): BSS reserves the exact
    // byte size (resb N), so a store must use the global's own width. The
    // STORE instruction's value type can be wider (e.g. an int value stored
    // to a bool global), and sizing by the value emits `mov qword [rel g]`
    // which zeroes the neighbor's bytes.
    std::map<std::string, IRType> globalTypes;
    for (const auto& var : module.getGlobalVars()) {
        globalTypes[var.name] = var.type;
    }

    std::vector<std::string> argRegs = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};

    // CID-005: ABI descriptors. `desc` is the target's default convention
    // (`native`/`pomeos` -> SysV, `uefi` -> MS); `msDesc` is the MS
    // convention for explicit `msabi` definitions under any target.
    struct AbiDesc {
        bool ms = false;
        std::vector<std::string> intArgRegs = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
        std::vector<std::string> floatArgRegs;   // MS: xmm0-3; SysV path unused
        int shadowSpace = 0;                     // MS: 32
        int stackArgBase = 16;                   // callee [rbp + stackArgBase + 8k]
        bool saveXmm = false;                    // MS: preserve xmm8-15
    };
    auto abiDescFor = [](const std::string& t) -> AbiDesc {
        AbiDesc d;
        if (targetAbiFor(t) == Abi::Ms) {
            d.ms = true;
            d.intArgRegs = {"rcx", "rdx", "r8", "r9"};
            d.floatArgRegs = {"xmm0", "xmm1", "xmm2", "xmm3"};
            d.shadowSpace = 32;
            d.stackArgBase = 48;
            d.saveXmm = true;
        }
        return d;
    };
    const AbiDesc desc = abiDescFor(target_);

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

        if (std::getenv("CIDER_DEBUG_ALLOC")) {
            std::fprintf(stderr, "=== allocation for %s (stackSize=%d) ===\n",
                         func->getName().c_str(), regAllocStackSize);
            for (auto& [v, res] : alloc) {
                if (res.isSpilled)
                    std::fprintf(stderr, "  %s -> spill -%d\n", v.c_str(), -res.stackOffset);
                else
                    std::fprintf(stderr, "  %s -> %s\n", v.c_str(), res.reg.c_str());
            }
        }

        std::string fName = func->getName();
        if (fName == "_start" || fName == "main") {
            addText("global " + fName);
        }
        if (!sourceFile.empty()) {
            addText("global " + fName);
        }

        // CID-005: resolve the definition's ABI from the FunctionIR.
        // `Default` follows the target descriptor (uefi -> MS); explicit
        // `msabi` is MS on any target. Pinned-SysV definitions
        // (virtual/override/interface/closure, see ir_gen) arrive as
        // Abi::SysV and stay SysV under `--target uefi`.
        const AbiDesc msDesc = abiDescFor("uefi");  // explicit msabi works on any target
        bool defMs = (func->getAbi() == Abi::Default && desc.ms) ||
                     (func->getAbi() == Abi::Ms);
        std::vector<std::string> calleeSaved = {"rbx", "r12", "r13", "r14", "r15"};
        std::vector<std::string> xmmSaved;
        if (defMs) {
            // Win64 makes RDI/RSI callee-saved (under SysV they are the
            // first argument registers). IROp::MEMCPY writes them for
            // `rep movsb`, so every MS definition preserves them — the
            // save set is ABI-driven rather than MEMCPY-driven, which
            // also covers any future body use. (Fix round 2: a struct
            // copy previously leaked the caller's RDI/RSI.)
            calleeSaved.push_back("rdi");
            calleeSaved.push_back("rsi");
            // MS-ABI XMM8-15 are callee-saved. The allocator hands the float
            // pool out top-down (xmm15 first), so renumber the used part to
            // a dense xmm8.. run: the epilogue then saves exactly those used
            // (bijective rename, so disjoint intervals stay disjoint).
            std::vector<std::string> xmmPool;
            for (int i = 8; i <= 15; ++i) xmmPool.push_back("xmm" + std::to_string(i));
            std::map<std::string, std::string> xmmRenumber;
            size_t nextXmm = 8;
            for (const auto& xr : xmmPool) {
                for (const auto& [var, res] : alloc) {
                    (void)var;
                    if (!res.isSpilled && res.reg == xr) {
                        xmmRenumber[xr] = "xmm" + std::to_string(nextXmm++);
                        break;
                    }
                }
            }
            for (auto& [var, res] : alloc) {
                (void)var;
                if (res.isSpilled) continue;
                auto it = xmmRenumber.find(res.reg);
                if (it != xmmRenumber.end()) res.reg = it->second;
            }
            for (const auto& xr : xmmPool) {
                for (const auto& [var, res] : alloc) {
                    (void)var;
                    if (!res.isSpilled && res.reg == xr) { xmmSaved.push_back(xr); break; }
                }
            }
        }

        addText(fName + ":");
        addText("    push rbp");
        addText("    mov rbp, rsp");
        for (const auto& reg : calleeSaved) addText("    push " + reg);
        // CID-005 fix round 3: MS XMM6-15 are callee-saved and 128 bits
        // wide, so each save gets its own 16-byte slot (`movdqu`) — the
        // earlier 8-byte `movq` slot dropped the upper half. The GP
        // pushes above are 8 bytes each.
        for (const auto& reg : xmmSaved) {
            addText("    sub rsp, 16");
            addText("    movdqu [rsp], " + reg);
        }
        int savedRegsSize = 8 + (int)calleeSaved.size() * 8 +
                            (int)xmmSaved.size() * 16;

        std::map<std::string, int> localOffsets;
        int currentAllocaOffset = savedRegsSize;
        for (auto& block : func->getBlocks()) {
            for (auto& instr : block->getInstructions()) {
                if (instr.op == IROp::ALLOCA) {
                    std::string varName = instr.dest->name;
                    if (localOffsets.find(varName) == localOffsets.end()) {
                        int size = (int)instr.args[0].intValue;
                        if (size < 8) size = 8;
                        if (size % 8 != 0) size += 8 - (size % 8);
                        localOffsets[varName] = currentAllocaOffset + size;
                        currentAllocaOffset += size;
                    }
                }
            }
        }

        int spillBase = currentAllocaOffset;
        int totalStack = spillBase + regAllocStackSize + 8;  // +8 to protect locals from call pushes

        if (totalStack % 16 != 0) {
            totalStack += 16 - (totalStack % 16);
        }

        int extraStack = totalStack - savedRegsSize;
        int prologueFixupIndex = -1;
        int maxSlotOffset = 0;
        int cvtLabelCount = 0;  // NASM-local labels for u64 -> float (I2)
        if (extraStack > 0) {
            prologueFixupIndex = (int)textSection.size();
            addText("    sub rsp, " + std::to_string(extraStack));
        }

        auto getOperand = [&](const IRValue& val) -> std::string {
            if (val.kind == IRValue::CONSTANT) {
                if (val.type == IRType::F32 || val.type == IRType::F64) return "[rel " + getFloatConst(val.constValue, val.type) + "]";
                return fmtImm(val.intValue, val.type);
            } else if (val.kind == IRValue::LABEL) {
                return val.name;
            } else {
                if (alloc.count(val.name)) {
                    const auto& a = alloc.at(val.name);
                    if (a.isSpilled) {
                        int offset = spillBase + (-a.stackOffset);
                        if (offset > maxSlotOffset) maxSlotOffset = offset;
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
            } else if (getOperand(arg).rfind("xmm", 0) == 0) {
                // Float args live in XMM regs (CID-004): `push xmm` is
                // invalid, so spill 8 bytes manually. The full 64 bits
                // round-trip; float ops only read the low 32.
                addText("    sub rsp, 8");
                addText("    movq [rsp], " + getOperand(arg));
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
            } else if (arg.kind == IRValue::CONSTANT &&
                       arg.type != IRType::F32 && arg.type != IRType::F64 &&
                       ((int64_t)arg.intValue > (int64_t)INT32_MAX ||
                        (int64_t)arg.intValue < (int64_t)INT32_MIN)) {
                // x86-64 push takes imm32 only (CID-018): `push 0xFFAAAAAA`
                // warns and sign-extends. Materialize out-of-range
                // immediates first: 32-bit values via eax (zero-extends),
                // wider ones via movabs.
                if ((uint64_t)arg.intValue <= 0xFFFFFFFFULL) {
                    addText("    mov eax, " + getOperand(arg));
                } else {
                    addText("    mov rax, " + getOperand(arg));
                }
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
                else if (t == IRType::I32 || t == IRType::U32 || t == IRType::BOOL) prefix = "dword ";
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
                {"r8", {"r8b", "r8w", "r8d", "r8"}},
                {"r9", {"r9b", "r9w", "r9d", "r9"}},
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
            else if (t == IRType::I32 || t == IRType::U32 || t == IRType::BOOL) idx = 2;
            else if (t == IRType::I64 || t == IRType::U64 || t == IRType::PTR) idx = 3;
            return regMap[reg][idx];
        };

        // CID-005: lower an MS-ABI (Win64) call. Argument slots are
        // positional: argument N (0-based, N < 4) uses the Nth slot of its
        // class (int/pointer -> rcx/rdx/r8/r9, float/f64 -> xmm0-3);
        // arguments 4+ go on the stack in source order at [rsp + 32 + 8k]
        // above the 32-byte shadow space. `pad` keeps RSP 16-byte aligned at
        // the call (design §2). The caller drops the caller-saved XMM spills
        // for MS callees: Win64 makes XMM6-15 callee-saved and the T2
        // prologue saves exactly the ones a definition uses.
        auto emitMsCall = [&](const IRInstruction& instr, const IRValue& target,
                              bool indirect, int pad) {
            size_t numArgs = instr.args.size() - 1;
            int stackArgs = std::max(0, (int)numArgs - 4);
            int reserve = msDesc.shadowSpace + stackArgs * 8 + pad;
            addText("    sub rsp, " + std::to_string(reserve));
            // Indirect target first: a spilled slot must be read before the
            // arg-register moves (which never touch r11).
            if (indirect) addText("    mov r11, " + getOperand(target));
            auto isFloatArg = [](const IRValue& v) {
                return v.type == IRType::F32 || v.type == IRType::F64;
            };
            auto loadIntArg = [&](const IRValue& arg, const std::string& dst) {
                std::string op = getOperand(arg);
                std::string d32 = getSizedReg(dst, IRType::I32);
                if (arg.kind == IRValue::LABEL) {
                    addText("    lea " + dst + ", [rel " + arg.name + "]");
                } else if (arg.kind == IRValue::CONSTANT) {
                    uint64_t c = (uint64_t)arg.intValue;
                    if ((int64_t)c >= (int64_t)INT32_MIN && (int64_t)c <= (int64_t)INT32_MAX)
                        addText("    mov " + dst + ", " + op);
                    else if (c <= 0xFFFFFFFFULL)
                        addText("    mov " + d32 + ", " + op);
                    else
                        addText("    mov " + dst + ", " + op);
                } else if (isMem(op)) {
                    if (arg.type == IRType::I32) addText("    movsxd " + dst + ", dword " + op);
                    else if (arg.type == IRType::U32 || arg.type == IRType::BOOL) addText("    mov " + d32 + ", dword " + op);
                    else if (arg.type == IRType::I8) addText("    movsx " + dst + ", byte " + op);
                    else if (arg.type == IRType::U8) addText("    movzx " + dst + ", byte " + op);
                    else if (arg.type == IRType::I16) addText("    movsx " + dst + ", word " + op);
                    else if (arg.type == IRType::U16) addText("    movzx " + dst + ", word " + op);
                    else addText("    mov " + dst + ", " + op);
                } else if (arg.type == IRType::I32) {
                    addText("    movsxd " + dst + ", " + getSizedReg(op, IRType::I32));
                } else if (arg.type == IRType::U32 || arg.type == IRType::BOOL) {
                    addText("    mov " + d32 + ", " + getSizedReg(op, IRType::U32));
                } else if (arg.type == IRType::I8) {
                    addText("    movsx " + dst + ", " + getSizedReg(op, IRType::I8));
                } else if (arg.type == IRType::U8) {
                    addText("    movzx " + dst + ", " + getSizedReg(op, IRType::U8));
                } else if (arg.type == IRType::I16) {
                    addText("    movsx " + dst + ", " + getSizedReg(op, IRType::I16));
                } else if (arg.type == IRType::U16) {
                    addText("    movzx " + dst + ", " + getSizedReg(op, IRType::U16));
                } else {
                    addText("    mov " + dst + ", " + getSizedReg(op, arg.type));
                }
            };
            auto loadFloatArg = [&](const IRValue& arg, const std::string& xmm) {
                std::string op = getOperand(arg);
                bool f32 = (arg.type == IRType::F32);
                std::string mv = f32 ? "movss" : "movsd";
                std::string sz = f32 ? "dword " : "qword ";
                if (arg.kind == IRValue::CONSTANT || isMem(op))
                    addText("    " + mv + " " + xmm + ", " + sz + op);
                else
                    addText("    " + mv + " " + xmm + ", " + op);
            };
            auto storeStackArg = [&](const IRValue& arg, int slot) {
                std::string dst = "[rsp + " + std::to_string(msDesc.shadowSpace + slot * 8) + "]";
                if (isFloatArg(arg)) {
                    // Move the bits through rax, never through xmm0-3: those
                    // may already hold earlier float arguments (the stack
                    // args are processed last), and an xmm0 scratch would
                    // overwrite them.
                    std::string op = getOperand(arg);
                    bool f32 = (arg.type == IRType::F32);
                    if (arg.kind == IRValue::CONSTANT || isMem(op)) {
                        if (f32) addText("    mov eax, dword " + op);
                        else addText("    mov rax, qword " + op);
                    } else {
                        addText("    movq rax, " + op);
                    }
                } else {
                    loadIntArg(arg, "rax");
                }
                addText("    mov " + dst + ", rax");
            };
            int slotCursor = 0, stackCursor = 0;
            for (size_t i = 1; i <= numArgs; ++i) {
                const IRValue& arg = instr.args[i];
                // Win64 slots are positional: argument (i-1) uses the
                // (i-1)-th slot of its class; args 4+ stack.
                if (slotCursor < 4) {
                    if (isFloatArg(arg)) loadFloatArg(arg, msDesc.floatArgRegs[slotCursor]);
                    else loadIntArg(arg, msDesc.intArgRegs[slotCursor]);
                    slotCursor++;
                } else {
                    storeStackArg(arg, stackCursor++);
                }
            }
            if (indirect) addText("    call r11");
            else addText("    call " + getOperand(target));
            addText("    add rsp, " + std::to_string(reserve));
            if (instr.dest && !getOperand(*instr.dest).empty() &&
                getOperand(*instr.dest) != "none") {
                std::string d = getOperand(*instr.dest);
                IRType retType = instr.dest->type;
                if (retType == IRType::F32)
                    addText("    movss " + (isMem(d) ? "dword " + d : d) + ", xmm0");
                else if (retType == IRType::F64)
                    addText("    movsd " + (isMem(d) ? "qword " + d : d) + ", xmm0");
                else
                    addText("    mov " + getSizedReg(d, retType) + ", " + getSizedReg("rax", retType));
            }
        };

        auto genEpilogue = [&](bool zeroVoid = false) {
            if (zeroVoid) addText("    xor eax, eax");
            if (!calleeSaved.empty() || !xmmSaved.empty()) {
                // Rewind over the frame sub to the saved-register area. Any
                // MS-ABI XMM saves sit just below the callee-saved GP
                // pushes (16 bytes each), so they must be restored (in
                // reverse) before the pops; without the rewind [rsp] would
                // point into the frame.
                addText("    lea rsp, [rbp - " +
                        std::to_string(calleeSaved.size() * 8 +
                                       xmmSaved.size() * 16) + "]");
                for (auto it = xmmSaved.rbegin(); it != xmmSaved.rend(); ++it) {
                    addText("    movdqu " + *it + ", [rsp]");
                    addText("    add rsp, 16");
                }
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
                    double allocSize = def->args.empty() ? 0.0 : (double)def->args[0].intValue;
                    return totalOffset == 0 && allocSize == 8.0;
                }
                if (def->op == IROp::ADD) {
                    if (!def->args.empty() && def->args[0].kind == IRValue::REGISTER) {
                        if (def->args.size() > 1 && def->args[1].kind == IRValue::CONSTANT) {
                            totalOffset += (long long)def->args[1].intValue;
                        } else {
                            // A variable index (scaled array access): the
                            // address is an array element, not the slot
                            // itself — a qword narrow store would clobber
                            // the neighbor.
                            return false;
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

                if (fName == "_start" && !defMs) {
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
                            } else if (a.reg.rfind("xmm", 0) == 0) {
                                // Float param in an XMM reg (CID-004): movq
                                // moves the bits; float ops read low-32.
                                addText("    movq " + a.reg + ", r11");
                            } else {
                                addText("    mov " + a.reg + ", r11");
                            }
                        }
                    }
                } else if (defMs) {
                    // CID-005: MS-ABI parameter marshaling. Win64 assigns
                    // argument slots positionally: argument N (0-based, N < 4)
                    // uses the Nth slot of its class (int/pointer ->
                    // rcx/rdx/r8/r9, float/f64 -> xmm0-3); arguments 4+ arrive
                    // on the stack in source order at [rbp + 48 + 8k]
                    // (32-byte shadow space above them).
                    int stackSlot = 0;
                    const auto& ptypes = func->getParamTypes();
                    for (size_t pi = 0; pi < params.size(); ++pi) {
                        IRType pt = (pi < ptypes.size()) ? ptypes[pi] : IRType::I32;
                        bool isFloat = (pt == IRType::F32 || pt == IRType::F64);
                        std::string src;
                        bool onStack = false;
                        if (pi < msDesc.intArgRegs.size()) {
                            src = isFloat ? msDesc.floatArgRegs[pi]
                                          : msDesc.intArgRegs[pi];
                        } else {
                            onStack = true;
                        }
                        if (onStack) {
                            src = "[rbp + " + std::to_string(msDesc.stackArgBase + 8 * stackSlot++) + "]";
                        }
                        if (!alloc.count(params[pi])) continue;
                        const auto& a = alloc.at(params[pi]);
                        if (a.isSpilled) {
                            int offset = spillBase + (-a.stackOffset);
                            if (onStack) {
                                // A spilled destination needs a register
                                // hop; `mov mem, mem` does not exist. r11
                                // is not an MS argument register, so it is
                                // safe scratch before the body.
                                addText("    mov r11, " + src);
                                src = "r11";
                            }
                            if (src.rfind("xmm", 0) == 0)
                                addText("    movq [rbp - " + std::to_string(offset) + "], " + src);
                            else
                                addText("    mov qword [rbp - " + std::to_string(offset) + "], " + src);
                        } else if (a.reg.rfind("xmm", 0) == 0) {
                            addText("    movq " + a.reg + ", " + src);
                        } else {
                            addText("    mov " + a.reg + ", " + src);
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
                            } else if (a.reg.rfind("xmm", 0) == 0) {
                                addText("    movq " + a.reg + ", r11");
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
                            } else if (a.reg.rfind("xmm", 0) == 0) {
                                addText("    movq " + a.reg + ", r11");
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
                    IRType srcT = (instr.args.size() > 0 ? instr.args[0].type
                                                         : IRType::I32);
                    std::string cvt = (t == IRType::F32) ? "cvtsi2ss" : "cvtsi2sd";
                    // CID-003 follow-up (I2): load the source into rax with
                    // its declared width and signedness. The old path read
                    // eax only: i64 sources truncated to their low half and
                    // u32/u64 sources converted as if signed.
                    if (instr.args[0].kind == IRValue::CONSTANT) {
                        uint64_t bits = (uint64_t)instr.args[0].intValue;
                        if (srcT == IRType::U8 || srcT == IRType::U16 ||
                            srcT == IRType::U32 || srcT == IRType::BOOL) {
                            // 32-bit mov zero-extends into rax.
                            addText("    mov eax, " + fmtImm((int64_t)bits, srcT));
                        } else if ((int64_t)bits >= (int64_t)INT32_MIN &&
                                   (int64_t)bits <= (int64_t)INT32_MAX) {
                            // mov r64, imm32 sign-extends.
                            addText("    mov rax, " + std::to_string((int64_t)bits));
                        } else {
                            // Full 64-bit immediate; hex keeps the unsigned
                            // bit pattern clear of NASM's signed-dword
                            // bounds warning.
                            std::ostringstream hs;
                            hs << "0x" << std::hex << std::uppercase << bits;
                            addText("    mov rax, " + hs.str());
                        }
                    } else {
                        auto srcOp = [&](const char* size) -> std::string {
                            return isMem(a1) ? std::string(size) + a1
                                             : getSizedReg(a1, srcT);
                        };
                        if (srcT == IRType::I8) {
                            addText("    movsx rax, " + srcOp("byte "));
                        } else if (srcT == IRType::U8 || srcT == IRType::BOOL) {
                            addText("    movzx rax, " + srcOp("byte "));
                        } else if (srcT == IRType::I16) {
                            addText("    movsx rax, " + srcOp("word "));
                        } else if (srcT == IRType::U16) {
                            addText("    movzx rax, " + srcOp("word "));
                        } else if (srcT == IRType::I32) {
                            addText("    movsxd rax, " + srcOp("dword "));
                        } else if (srcT == IRType::U32) {
                            addText("    mov eax, " + srcOp("dword "));
                        } else {
                            addText("    mov rax, " + srcOp("qword "));
                        }
                    }
                    if (srcT == IRType::U64) {
                        // Unsigned 64-bit conversion (CID-003 follow-up I2):
                        // cvtsi2sd would read the high bit as a sign. For
                        // values >= 2^63 convert (x >> 1) | (x & 1) and
                        // double — correctly rounded, like the VM's
                        // (double)(uint64_t) / (float)(uint64_t).
                        std::string lbl = ".u64cvt" + std::to_string(cvtLabelCount++);
                        addText("    test rax, rax");
                        addText("    js " + lbl);
                        addText("    " + cvt + " xmm0, rax");
                        addText("    jmp " + lbl + "_done");
                        addText(lbl + ":");
                        addText("    mov r11, rax");
                        addText("    shr r11, 1");
                        addText("    and eax, 1");
                        addText("    or r11, rax");
                        addText("    " + cvt + " xmm0, r11");
                        addText(t == IRType::F32 ? "    addss xmm0, xmm0"
                                                 : "    addsd xmm0, xmm0");
                        addText(lbl + "_done:");
                    } else {
                        addText("    " + cvt + " xmm0, rax");
                    }
                    addText(t == IRType::F32 ? "    movss " + d + ", xmm0"
                                             : "    movsd " + d + ", xmm0");
                    break;
                }
                case IROp::CVT_F32F64: {
                    addText("    cvtss2sd xmm0, " + a1);
                    addText("    movsd " + d + ", xmm0");
                    break;
                }
                case IROp::CVT_F64F32: {
                    addText("    cvtsd2ss xmm0, " + a1);
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
                        } else {
                            addText("    movss xmm0, " + a1);
                        }
                        // Convert at the destination width (CID-025): a
                        // 32-bit convert cannot feed a 64-bit slot, and a
                        // bare `mov <narrow-mem>, eax` would not assemble.
                        if (t == IRType::I64 || t == IRType::U64 || t == IRType::PTR) {
                            addText(srcType == IRType::F64 ? "    cvttsd2si rax, xmm0" : "    cvttss2si rax, xmm0");
                            addText("    mov " + dstReg + ", rax");
                        } else {
                            addText(srcType == IRType::F64 ? "    cvttsd2si eax, xmm0" : "    cvttss2si eax, xmm0");
                            addText("    mov " + dstReg + ", " + getSizedReg("rax", t));
                        }
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
                            // A spilled narrow integer's slot only has
                            // `src`-sized valid bytes; a raw copy sized by the
                            // destination would drag stale stack garbage into
                            // the upper bits. Sign/zero-extend through
                            // rax/eax instead.
                            IRType srcT = (instr.args.size() > 0 ? instr.args[0].type : t);
                            auto bitWidth = [](IRType x) -> int {
                                if (x == IRType::I8 || x == IRType::U8) return 8;
                                if (x == IRType::I16 || x == IRType::U16) return 16;
                                if (x == IRType::I32 || x == IRType::U32 || x == IRType::BOOL) return 32;
                                return 64;
                            };
                            bool widening = (instr.args[0].kind != IRValue::CONSTANT &&
                                             srcT != t && bitWidth(t) > bitWidth(srcT) &&
                                             (srcT == IRType::I8 || srcT == IRType::U8 ||
                                              srcT == IRType::I16 || srcT == IRType::U16 ||
                                              srcT == IRType::I32 || srcT == IRType::U32 ||
                                              srcT == IRType::BOOL));
                            if (widening) {
                                bool dest64 = (t == IRType::I64 || t == IRType::U64 || t == IRType::PTR);
                                bool dest32 = (t == IRType::I32 || t == IRType::U32 || t == IRType::BOOL);
                                if (dest64) {
                                    if (srcT == IRType::I8) addText("    movsx rax, byte " + a1);
                                    else if (srcT == IRType::U8 || srcT == IRType::BOOL) addText("    movzx rax, byte " + a1);
                                    else if (srcT == IRType::I16) addText("    movsx rax, word " + a1);
                                    else if (srcT == IRType::U16) addText("    movzx rax, word " + a1);
                                    else if (srcT == IRType::I32) addText("    movsxd rax, dword " + a1);
                                    else addText("    mov eax, dword " + a1);
                                    addText("    mov " + getSizedReg(d, t) + ", rax");
                                } else if (dest32) {
                                    if (srcT == IRType::I8) addText("    movsx eax, byte " + a1);
                                    else if (srcT == IRType::U8 || srcT == IRType::BOOL) addText("    movzx eax, byte " + a1);
                                    else if (srcT == IRType::I16) addText("    movsx eax, word " + a1);
                                    else if (srcT == IRType::U16) addText("    movzx eax, word " + a1);
                                    else addText("    mov eax, dword " + a1);
                                    addText("    mov " + getSizedReg(d, t) + ", eax");
                                } else {
                                    addText("    mov " + getSizedReg("rax", t) + ", " + getSizedReg(a1, t));
                                    addText("    mov " + getSizedReg(d, t) + ", " + getSizedReg("rax", t));
                                }
                            } else {
                                addText("    mov " + getSizedReg("rax", t) + ", " + getSizedReg(a1, t));
                                addText("    mov " + getSizedReg(d, t) + ", " + getSizedReg("rax", t));
                            }
                        } else {
                            uint64_t cval = (uint64_t)instr.args[0].intValue;
                            bool largeConst = (instr.args[0].kind == IRValue::CONSTANT &&
                                               (t == IRType::I64 || t == IRType::U64 || t == IRType::PTR) &&
                                               cval > (uint64_t)0xFFFFFFFFULL);
                            if (largeConst) {
                                // Full 64-bit immediate: movabs via rax.
                                addText("    mov rax, " + a1);
                                addText("    mov " + getSizedReg(d, t) + ", " + getSizedReg("rax", t));
                            } else {
                                // 32-to-64 bit extension: signed (I32) sign-extends
                                // via movsxd; unsigned (U32) zero-extends via
                                // a 32-bit mov (writing eax clears upper rax).
                                // Narrow (8/16-bit) sources zero/sign-extend
                                // via movzx/movsx (a raw 64-bit mov would copy
                                // stale upper register/stack bytes).
                                if (t == IRType::I64 || t == IRType::U64 || t == IRType::PTR) {
                                    IRType srcT = (instr.args.size() > 0 ? instr.args[0].type : t);
                                    bool srcIsConst = (instr.args.size() > 0 && instr.args[0].kind == IRValue::CONSTANT);
                                    // NB: BOOL lives in 32-bit regs (xor r14d, 1
                                    // zero-extends), so it widens like U32 via a
                                    // plain 32-bit mov — never movzx (Issue #23:
                                    // `movzx r32, r32` is invalid; movzx needs
                                    // a byte/word source).
                                    if (!srcIsConst &&
                                        (srcT == IRType::I8 || srcT == IRType::U8 ||
                                         srcT == IRType::I16 || srcT == IRType::U16)) {
                                        std::string ext = (srcT == IRType::I8 || srcT == IRType::I16) ? "movsx" : "movzx";
                                        std::string srcOp = isMem(a1)
                                            ? ((srcT == IRType::I8 || srcT == IRType::U8)
                                               ? ("byte " + a1) : ("word " + a1))
                                            : getSizedReg(a1, srcT);
                                        if (isMem(d)) {
                                            addText("    " + ext + " rax, " + srcOp);
                                            addText("    mov " + getSizedReg(d, t) + ", rax");
                                        } else {
                                            addText("    " + ext + " " + getSizedReg(d, t) + ", " + srcOp);
                                        }
                                    } else if (!srcIsConst && srcT == IRType::I32) {
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
                                    } else if (srcT == IRType::U32 || srcT == IRType::BOOL) {
                                        // Zero-extension: 32-bit mov clears upper bits.
                                        if (isMem(d) && isMem(a1)) {
                                            addText("    mov eax, dword " + a1);
                                            addText("    mov " + getSizedReg(d, t) + ", rax");
                                        } else if (isMem(d)) {
                                            if (srcIsConst) {
                                                addText("    mov eax, " + a1);
                                            } else if (isMem(a1)) {
                                                addText("    mov eax, dword " + a1);
                                            } else {
                                                addText("    mov eax, " + getSizedReg(a1, IRType::U32));
                                            }
                                            addText("    mov " + getSizedReg(d, t) + ", rax");
                                        } else if (isMem(a1) && !srcIsConst) {
                                            addText("    mov " + getSizedReg(d, IRType::U32) + ", dword " + a1);
                                        } else if (srcIsConst) {
                                            // mov <dest32>, imm zero-extends the full reg.
                                            addText("    mov " + getSizedReg(d, IRType::U32) + ", " + a1);
                                        } else {
                                            addText("    mov " + getSizedReg(d, IRType::U32) + ", " + getSizedReg(a1, IRType::U32));
                                        }
                                    } else {
                                        addText("    mov " + getSizedReg(d, t) + ", " + getSizedReg(a1, t));
                                    }
                                } else if (t == IRType::I32 || t == IRType::U32 || t == IRType::BOOL) {
                                    IRType srcT2 = (instr.args.size() > 0 ? instr.args[0].type : t);
                                    bool srcConst2 = (instr.args.size() > 0 && instr.args[0].kind == IRValue::CONSTANT);
                                    // BOOL sources are already full 32-bit regs:
                                    // a plain mov suffices (Issue #23).
                                    if (!srcConst2 &&
                                        (srcT2 == IRType::I8 || srcT2 == IRType::U8 ||
                                         srcT2 == IRType::I16 || srcT2 == IRType::U16)) {
                                        std::string ext = (srcT2 == IRType::I8 || srcT2 == IRType::I16) ? "movsx" : "movzx";
                                        std::string srcOp = isMem(a1)
                                            ? ((srcT2 == IRType::I8 || srcT2 == IRType::U8)
                                               ? ("byte " + a1) : ("word " + a1))
                                            : getSizedReg(a1, srcT2);
                                        if (isMem(d)) {
                                            addText("    " + ext + " eax, " + srcOp);
                                            addText("    mov " + getSizedReg(d, t) + ", eax");
                                        } else {
                                            addText("    " + ext + " " + getSizedReg(d, t) + ", " + srcOp);
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
                        bool isNarrow = (t == IRType::I8 || t == IRType::U8 ||
                                         t == IRType::I16 || t == IRType::U16 ||
                                         t == IRType::BOOL);
                        // Narrow integers compute in 32 bits, then truncate.
                        IRType compT = isNarrow ? IRType::I32 : t;
                        std::string destReg = is64 ? "rax" : "eax";
                        std::string tempReg = is64 ? "r11" : "r11d";
                        auto loadOperand = [&](const std::string& opStr, const IRValue& v,
                                               const std::string& targetReg) {
                            IRType vt = v.type;
                            bool mem = isMem(opStr);
                            if (vt == IRType::I8 && (mem || !mem)) {
                                // movsx works for both mem byte and reg byte (bl).
                                std::string src = mem ? ("byte " + opStr) : getSizedReg(opStr, vt);
                                addText("    movsx " + targetReg + ", " + src);
                            } else if ((vt == IRType::U8 || vt == IRType::BOOL)) {
                                std::string src = mem ? ("byte " + opStr) : getSizedReg(opStr, vt);
                                addText("    movzx " + targetReg + ", " + src);
                            } else if (vt == IRType::I16) {
                                std::string src = mem ? ("word " + opStr) : getSizedReg(opStr, vt);
                                addText("    movsx " + targetReg + ", " + src);
                            } else if (vt == IRType::U16) {
                                std::string src = mem ? ("word " + opStr) : getSizedReg(opStr, vt);
                                addText("    movzx " + targetReg + ", " + src);
                            } else {
                                addText("    mov " + targetReg + ", " + getSizedReg(opStr, compT));
                            }
                        };
                        if (instr.args[1].kind == IRValue::CONSTANT) {
                            addText("    mov " + tempReg + ", " + a2);
                            loadOperand(a1, instr.args[0], destReg);
                            addText("    imul " + destReg + ", " + tempReg);
                        } else {
                            loadOperand(a1, instr.args[0], destReg);
                            // Second operand: narrow (reg or mem) needs zx/sx.
                            IRType vt2 = instr.args[1].type;
                            if (vt2 == IRType::I8 || vt2 == IRType::U8 || vt2 == IRType::BOOL ||
                                vt2 == IRType::I16 || vt2 == IRType::U16) {
                                loadOperand(a2, instr.args[1], tempReg);
                                addText("    imul " + destReg + ", " + tempReg);
                            } else {
                                addText("    mov " + tempReg + ", " + getSizedReg(a2, compT));
                                addText("    imul " + destReg + ", " + tempReg);
                            }
                        }
                        if (isNarrow) {
                            if (t == IRType::I8 || t == IRType::U8 || t == IRType::BOOL) {
                                if (isMem(d)) addText("    mov byte " + d + ", al");
                                else addText("    mov " + getSizedReg(d, t) + ", al");
                            } else {
                                if (isMem(d)) addText("    mov word " + d + ", ax");
                                else addText("    mov " + getSizedReg(d, t) + ", ax");
                            }
                        } else {
                            addText("    mov " + getSizedReg(d, t) + ", " + destReg);
                        }
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
                                toSave.push_back(reg);
                                break;
                            }
                        }
                    }
                    for (const auto& reg : toSave) addText("    push " + reg);
                    t = (instr.dest ? instr.dest->type : IRType::I32);
                    bool is64 = (t == IRType::I64 || t == IRType::U64 || t == IRType::PTR);
                    bool isNarrowDiv = (t == IRType::I8 || t == IRType::U8 ||
                                        t == IRType::I16 || t == IRType::U16 ||
                                        t == IRType::BOOL);
                    auto loadDivOp = [&](const std::string& opStr, const IRValue& v,
                                         const std::string& target32) {
                        IRType vt = v.type;
                        bool mem = isMem(opStr);
                        if (vt == IRType::I8) {
                            addText("    movsx " + target32 + ", " + (mem ? ("byte " + opStr) : getSizedReg(opStr, vt)));
                        } else if (vt == IRType::U8 || vt == IRType::BOOL) {
                            addText("    movzx " + target32 + ", " + (mem ? ("byte " + opStr) : getSizedReg(opStr, vt)));
                        } else if (vt == IRType::I16) {
                            addText("    movsx " + target32 + ", " + (mem ? ("word " + opStr) : getSizedReg(opStr, vt)));
                        } else if (vt == IRType::U16) {
                            addText("    movzx " + target32 + ", " + (mem ? ("word " + opStr) : getSizedReg(opStr, vt)));
                        } else {
                            addText("    mov " + target32 + ", " + getSizedReg(opStr, IRType::I32));
                        }
                    };
                    if (is64) {
                        // Widen each operand to 64 bits with the correct
                        // extension (CID-028): a bare `mov rax, <r32/u16>`
                        // is invalid or reads garbage upper bits.
                        auto widenDiv64 = [&](const std::string& dst, const IRValue& arg, const std::string& a) {
                            IRType srcT = arg.type;
                            if (arg.kind == IRValue::CONSTANT) {
                                addText("    mov " + dst + ", " + a);
                                return;
                            }
                            std::string srcOp;
                            if (isMem(a)) {
                                if (srcT == IRType::I8) srcOp = "byte " + a;
                                else if (srcT == IRType::U8 || srcT == IRType::BOOL) srcOp = "byte " + a;
                                else if (srcT == IRType::I16) srcOp = "word " + a;
                                else if (srcT == IRType::U16) srcOp = "word " + a;
                                else if (srcT == IRType::I32 || srcT == IRType::U32) srcOp = "dword " + a;
                                else srcOp = "qword " + a;
                            } else {
                                srcOp = getSizedReg(a, srcT);
                            }
                            if (srcT == IRType::I8) addText("    movsx " + dst + ", " + srcOp);
                            else if (srcT == IRType::U8 || srcT == IRType::BOOL) addText("    movzx " + dst + ", " + srcOp);
                            else if (srcT == IRType::I16) addText("    movsx " + dst + ", " + srcOp);
                            else if (srcT == IRType::U16) addText("    movzx " + dst + ", " + srcOp);
                            else if (srcT == IRType::I32) addText("    movsxd " + dst + ", " + srcOp);
                            else if (srcT == IRType::U32) addText("    mov " + getSizedReg(dst, IRType::I32) + ", " + srcOp);
                            else addText("    mov " + dst + ", " + srcOp);
                        };
                        widenDiv64("r11", instr.args[1], a2);
                        widenDiv64("rax", instr.args[0], a1);
                        // Unsigned divides zero-extend the dividend
                        // (CID-028): cqo/idiv sign-fills and miscomputes
                        // u64 values >= 2^63.
                        if (t == IRType::U64 || t == IRType::PTR) {
                            addText("    xor edx, edx");
                            addText("    div r11");
                        } else {
                            addText("    cqo");
                            addText("    idiv r11");
                        }
                        addText("    mov " + getSizedReg(d, t) + ", " + (instr.op == IROp::DIV ? "rax" : "rdx"));
                    } else if (isNarrowDiv) {
                        // Widen to 32 bits, divide, then truncate to narrow.
                        loadDivOp(a2, instr.args[1], "r11d");
                        loadDivOp(a1, instr.args[0], "eax");
                        if (t == IRType::U8 || t == IRType::U16 || t == IRType::BOOL) {
                            addText("    xor edx, edx");
                            addText("    div r11d");
                        } else {
                            addText("    cdq");
                            addText("    idiv r11d");
                        }
                        std::string res32 = (instr.op == IROp::DIV ? "eax" : "edx");
                        if (t == IRType::I8 || t == IRType::U8 || t == IRType::BOOL) {
                            if (isMem(d)) {
                                addText("    mov byte " + d + ", " + std::string(instr.op == IROp::DIV ? "al" : "dl"));
                            } else {
                                addText("    mov " + getSizedReg(d, t) + ", " + std::string(instr.op == IROp::DIV ? "al" : "dl"));
                            }
                        } else {
                            if (isMem(d)) {
                                addText("    mov word " + d + ", " + std::string(instr.op == IROp::DIV ? "ax" : "dx"));
                            } else {
                                addText("    mov " + getSizedReg(d, t) + ", " + std::string(instr.op == IROp::DIV ? "ax" : "dx"));
                            }
                        }
                        (void)res32;
                    } else {
                        // Widen narrow operands by their own type (a bare
                        // 32-bit mov would read garbage upper bits), then
                        // divide with the destination's signedness.
                        auto widenDiv32 = [&](const std::string& dst32, const IRValue& arg, const std::string& a) {
                            IRType srcT = arg.type;
                            if (arg.kind == IRValue::CONSTANT) {
                                addText("    mov " + dst32 + ", " + a);
                                return;
                            }
                            std::string srcOp;
                            if (isMem(a)) {
                                if (srcT == IRType::I8) srcOp = "byte " + a;
                                else if (srcT == IRType::U8 || srcT == IRType::BOOL) srcOp = "byte " + a;
                                else if (srcT == IRType::I16) srcOp = "word " + a;
                                else if (srcT == IRType::U16) srcOp = "word " + a;
                                else srcOp = "dword " + a;
                            } else {
                                srcOp = getSizedReg(a, srcT);
                            }
                            if (srcT == IRType::I8) addText("    movsx " + dst32 + ", " + srcOp);
                            else if (srcT == IRType::U8 || srcT == IRType::BOOL) addText("    movzx " + dst32 + ", " + srcOp);
                            else if (srcT == IRType::I16) addText("    movsx " + dst32 + ", " + srcOp);
                            else if (srcT == IRType::U16) addText("    movzx " + dst32 + ", " + srcOp);
                            else addText("    mov " + dst32 + ", " + srcOp);
                        };
                        widenDiv32("r11d", instr.args[1], a2);
                        widenDiv32("eax", instr.args[0], a1);
                        if (t == IRType::U32 || t == IRType::BOOL) {
                            addText("    xor edx, edx");
                            addText("    div r11d");
                        } else {
                            addText("    cdq");
                            addText("    idiv r11d");
                        }
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
                    // Signed right shift is arithmetic (sar), unsigned is
                    // logical (shr): `u64 >> 32` must zero-fill, `i64 >> 2`
                    // must sign-fill. The value's own type decides.
                    std::string opName = (instr.op == IROp::BIT_AND ? "and" : (instr.op == IROp::BIT_OR ? "or" : (instr.op == IROp::BIT_XOR ? "xor" : (instr.op == IROp::LSHIFT ? "shl" : "shr"))));
                    if (instr.op == IROp::RSHIFT && instr.args.size() > 0) {
                        IRType srcT = instr.args[0].type;
                        if (srcT == IRType::I8 || srcT == IRType::I16 || srcT == IRType::I32 || srcT == IRType::I64) opName = "sar";
                    }
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
                        // A 64-bit logic op with a constant that does not fit
                        // a sign-extended imm32 (CID-008): `and r64, mask`
                        // warns and TRUNCATES (imm32 sign-extended), so move
                        // the full mask with movabs first. r10/r11 are
                        // scratch (never allocator-held).
                        bool wideMask = false;
                        if ((t == IRType::I64 || t == IRType::U64 || t == IRType::PTR) &&
                            instr.args[1].kind == IRValue::CONSTANT) {
                            int64_t mv = (int64_t)instr.args[1].intValue;
                            wideMask = (mv > (int64_t)INT32_MAX || mv < (int64_t)INT32_MIN);
                        }
                        if (wideMask) {
                            // a2 is a wide immediate (never memory), so a
                            // single op against the scratch register works
                            // for both register and memory destinations.
                            std::string dstOp = getSizedReg(d, t);
                            std::string scratch = (dstOp == "r11") ? "r10" : "r11";
                            addText("    movabs " + scratch + ", " + a2);
                            addText("    " + opName + " " + dstOp + ", " + scratch);
                        } else if (isMem(d) && isMem(a2)) {
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
                    std::string setcc;
                    switch (instr.op) {
                    case IROp::CMP_EQ: setcc = "sete"; break;
                    case IROp::CMP_NE: setcc = "setne"; break;
                    case IROp::CMP_LT: setcc = "setb"; break;
                    case IROp::CMP_LE: setcc = "setbe"; break;
                    case IROp::CMP_GT: setcc = "seta"; break;
                    case IROp::CMP_GE: setcc = "setae"; break;
                    default: setcc = "sete";
                    }
                    // CID-003 follow-up (I5): ucomiss/ucomisd set PF for
                    // unordered operands (NaN) together with ZF and CF, so a
                    // bare setcc is IEEE-wrong for NaN: setne is false for
                    // n != n, sete is true for n == n, and CF=1 makes
                    // setb/setbe true for every ordered compare. Emit the
                    // VM's (native C++) semantics instead: != is
                    // (setne || setp), == is (sete && !setp), and <,<=,>,>=
                    // are (setcc && !setp). r11 is codegen scratch.
                    auto emitSetcc = [&](const std::string& op) {
                        if (instr.op == IROp::CMP_NE) {
                            addText("    setne al");
                            addText("    setp r11b");
                            addText("    or al, r11b");
                        } else if (instr.op == IROp::CMP_EQ) {
                            addText("    sete al");
                            addText("    setnp r11b");
                            addText("    and al, r11b");
                        } else {
                            addText("    " + op + " al");
                            addText("    setnp r11b");
                            addText("    and al, r11b");
                        }
                    };
                    if (t == IRType::F32) {
                        if (isMem(a1)) {
                            addText("    movss xmm0, " + a1);
                            addText("    ucomiss xmm0, " + a2);
                        } else if (isMem(a2)) {
                            addText("    movss xmm0, " + a2);
                            addText("    ucomiss " + a1 + ", xmm0");
                        } else {
                            addText("    ucomiss " + a1 + ", " + a2);
                        }
                        emitSetcc(setcc);
                        addText("    movzx eax, al");
                        addText("    mov " + d + ", rax");
                        break;
                    } else if (t == IRType::F64) {
                        if (isMem(a1)) {
                            addText("    movsd xmm0, " + a1);
                            addText("    ucomisd xmm0, " + a2);
                        } else if (isMem(a2)) {
                            addText("    movsd xmm0, " + a2);
                            addText("    ucomisd " + a1 + ", xmm0");
                        } else {
                            addText("    ucomisd " + a1 + ", " + a2);
                        }
                        emitSetcc(setcc);
                        addText("    movzx eax, al");
                        addText("    mov " + d + ", rax");
                        break;
                    }
                    IRType t2 = instr.args[1].type;
                    bool is64 = (t == IRType::I64 || t == IRType::U64 || t == IRType::PTR || t2 == IRType::I64 || t2 == IRType::U64 || t2 == IRType::PTR);

                    // Widen one comparison operand into rax/r11 (Issue #21):
                    // x86 has no `mov rax, r16`, so a 16/8-bit source must
                    // be extended with movzx (unsigned) / movsx (signed).
                    // Both sides are widened to the same width (64-bit when
                    // either side is 64-bit, else 32-bit) so `cmp` always
                    // sees matching operands. U32 uses a 32-bit mov (which
                    // zero-extends); only I32 sign-extends via movsxd.
                    auto widenCmpOp = [&](const std::string& dst, const IRValue& arg, const std::string& a, bool to64) {
                        IRType srcT = arg.type;
                        if (arg.kind == IRValue::LABEL) {
                            addText("    lea " + dst + ", [rel " + arg.name + "]");
                            return;
                        }
                        if (arg.kind == IRValue::CONSTANT) {
                            if (to64) addText("    mov " + dst + ", " + a);
                            else addText("    mov " + getSizedReg(dst, IRType::I32) + ", " + a);
                            return;
                        }
                        std::string srcOp;
                        if (isMem(a)) {
                            if (srcT == IRType::I8 || srcT == IRType::U8 || srcT == IRType::BOOL) srcOp = "byte " + a;
                            else if (srcT == IRType::I16 || srcT == IRType::U16) srcOp = "word " + a;
                            else if (srcT == IRType::I32 || srcT == IRType::U32) srcOp = "dword " + a;
                            else srcOp = (to64 ? "qword " + a : "dword " + a);
                        } else {
                            srcOp = getSizedReg(a, srcT);
                        }
                        if (to64) {
                            if (srcT == IRType::I8) addText("    movsx " + dst + ", " + srcOp);
                            else if (srcT == IRType::U8 || srcT == IRType::BOOL) addText("    movzx " + dst + ", " + srcOp);
                            else if (srcT == IRType::I16) addText("    movsx " + dst + ", " + srcOp);
                            else if (srcT == IRType::U16) addText("    movzx " + dst + ", " + srcOp);
                            else if (srcT == IRType::I32) addText("    movsxd " + dst + ", " + srcOp);
                            else if (srcT == IRType::U32) addText("    mov " + getSizedReg(dst, IRType::I32) + ", " + srcOp);
                            else addText("    mov " + dst + ", " + srcOp);
                        } else {
                            std::string dst32 = getSizedReg(dst, IRType::I32);
                            if (srcT == IRType::I8) addText("    movsx " + dst32 + ", " + srcOp);
                            else if (srcT == IRType::U8 || srcT == IRType::BOOL) addText("    movzx " + dst32 + ", " + srcOp);
                            else if (srcT == IRType::I16) addText("    movsx " + dst32 + ", " + srcOp);
                            else if (srcT == IRType::U16) addText("    movzx " + dst32 + ", " + srcOp);
                            else addText("    mov " + dst32 + ", " + srcOp);
                        }
                    };
                    widenCmpOp("rax", instr.args[0], a1, is64);
                    widenCmpOp("r11", instr.args[1], a2, is64);

                    addText("    cmp " + getSizedReg("rax", is64 ? IRType::I64 : IRType::I32) + ", " + getSizedReg("r11", is64 ? IRType::I64 : IRType::I32));
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

                    // REPLACE placeholders like {s}, {len}, {code} literally.
                    // A width suffix re-sizes a REGISTER operand to the width
                    // the template instruction needs (Issue #24): {fd:q} is
                    // the 64-bit name (r15), {fd:d} 32-bit (r15d), {fd:w}
                    // 16-bit, {fd:b} 8-bit. Plain {fd} keeps the declared
                    // width. Suffixed forms are replaced first: {fd} is a
                    // prefix of {fd:q} and would otherwise match inside it.
                    for (auto& pair : instr.asmMap) {
                        std::string raw = getOperand(pair.second);
                        auto sizedAs = [&](char spec) -> std::string {
                            IRType t = pair.second.type;
                            if (spec == 'q' || spec == 'Q') t = IRType::U64;
                            else if (spec == 'd' || spec == 'D') t = IRType::U32;
                            else if (spec == 'w' || spec == 'W') t = IRType::U16;
                            else if (spec == 'b' || spec == 'B') t = IRType::U8;
                            return getSizedReg(raw, t);
                        };
                        std::string key = "{" + pair.first + ":";
                        size_t pos = 0;
                        while ((pos = code.find(key, pos)) != std::string::npos) {
                            size_t end = code.find('}', pos);
                            if (end == std::string::npos) break;
                            std::string spec = code.substr(pos + key.size(), end - (pos + key.size()));
                            char c = spec.empty() ? '\0' : spec[0];
                            std::string rep = (c == 'q' || c == 'Q' || c == 'd' || c == 'D' ||
                                               c == 'w' || c == 'W' || c == 'b' || c == 'B')
                                              ? sizedAs(c) : getSizedReg(raw, pair.second.type);
                            code.replace(pos, end - pos + 1, rep);
                            pos += rep.length();
                        }
                        std::string replacement = getSizedReg(raw, pair.second.type);
                        pos = 0;
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
                            addText("    movss xmm0, dword [" + addrReg + " + " + std::to_string((int)instr.args[1].intValue) + "]");
                        else if (instr.args[0].kind == IRValue::LABEL)
                            addText("    movss xmm0, dword [rel " + instr.args[0].name + "]");
                        else
                            addText("    movss xmm0, dword [" + addrReg + "]");
                        addText("    movss " + d + ", xmm0");
                    } else if (t == IRType::F64) {
                        if (instr.args.size() > 1 && instr.args[1].kind == IRValue::CONSTANT)
                            addText("    movsd xmm0, qword [" + addrReg + " + " + std::to_string((int)instr.args[1].intValue) + "]");
                        else if (instr.args[0].kind == IRValue::LABEL)
                            addText("    movsd xmm0, qword [rel " + instr.args[0].name + "]");
                        else
                            addText("    movsd xmm0, qword [" + addrReg + "]");
                        addText("    movsd " + d + ", xmm0");
                    } else if (t == IRType::I8 || t == IRType::U8) {
                        std::string op = (t == IRType::I8) ? "movsx" : "movzx";
                        if (instr.args.size() > 1 && instr.args[1].kind == IRValue::CONSTANT)
                            addText("    " + op + " eax, byte [" + addrReg + " + " + std::to_string((int)instr.args[1].intValue) + "]");
                        else if (instr.args[0].kind == IRValue::LABEL)
                            addText("    " + op + " eax, byte [rel " + instr.args[0].name + "]");
                        else
                            addText("    " + op + " eax, byte [" + addrReg + "]");
                        // Move the full zero/sign-extended 32-bit value: a
                        // `mov bl, al` would leave the destination register's
                        // upper 56 bits stale, corrupting every later
                        // comparison of this value (e.g. strcmp).
                        addText("    mov " + getSizedReg(d, IRType::I32) + ", eax");
                    } else if (t == IRType::I16 || t == IRType::U16) {
                        std::string op = (t == IRType::I16) ? "movsx" : "movzx";
                        if (instr.args.size() > 1 && instr.args[1].kind == IRValue::CONSTANT)
                            addText("    " + op + " eax, word [" + addrReg + " + " + std::to_string((int)instr.args[1].intValue) + "]");
                        else if (instr.args[0].kind == IRValue::LABEL)
                            addText("    " + op + " eax, word [rel " + instr.args[0].name + "]");
                        else
                            addText("    " + op + " eax, word [" + addrReg + "]");
                        // Full 32-bit move: zeroes the register's upper half.
                        addText("    mov " + getSizedReg(d, IRType::I32) + ", eax");
                    } else {
                        std::string sizePrefix = "qword ";
                        if (t == IRType::I32 || t == IRType::U32 || t == IRType::BOOL) sizePrefix = "dword ";
                        else if (t == IRType::I64 || t == IRType::U64 || t == IRType::PTR) sizePrefix = "qword ";
                        std::string srcReg = getSizedReg("rax", t);
                        if (instr.args.size() > 1 && instr.args[1].kind == IRValue::CONSTANT)
                            addText("    mov " + srcReg + ", " + sizePrefix + "[" + addrReg + " + " + std::to_string((int)instr.args[1].intValue) + "]");
                        else if (instr.args[0].kind == IRValue::LABEL)
                            addText("    mov " + srcReg + ", " + sizePrefix + "[rel " + instr.args[0].name + "]");
                        else
                            addText("    mov " + srcReg + ", " + sizePrefix + "[" + addrReg + "]");
                        addText("    mov " + getSizedReg(d, t) + ", " + srcReg);
                    }
                    break;
                }
                case IROp::STORE: {
                    std::string ptr = a1;
                    std::string val = a2;
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
                        else if (t == IRType::I32 || t == IRType::U32 || t == IRType::BOOL) sizePrefix = "dword ";
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
                            // Issue #20: globals occupy exactly their declared
                            // bytes in .bss/.data (resb N), unlike 8-byte
                            // local slots. Size the store by the global's
                            // declared type: a qword store to a u8/u16/u32
                            // global overwrites the following symbol.
                            // LOAD already reads globals with the matching
                            // narrow width (movzx/mov dword), so no
                            // extend-to-qword is needed here.
                            IRType st = t;
                            auto git = globalTypes.find(instr.args[0].name);
                            if (git != globalTypes.end()) st = git->second;
                            std::string stPrefix = "qword ";
                            if (st == IRType::I8 || st == IRType::U8) stPrefix = "byte ";
                            else if (st == IRType::I16 || st == IRType::U16) stPrefix = "word ";
                            else if (st == IRType::I32 || st == IRType::U32 || st == IRType::BOOL) stPrefix = "dword ";
                            else if (st == IRType::I64 || st == IRType::U64 || st == IRType::PTR) stPrefix = "qword ";
                            if (instr.args[1].kind == IRValue::LABEL) {
                                addText("    lea rax, [rel " + val + "]");
                                addText("    mov " + stPrefix + "[rel " + instr.args[0].name + "], " + getSizedReg("rax", st));
                            } else {
                                addText("    mov " + getSizedReg("rax", st) + ", " + getSizedReg(val, st));
                                addText("    mov " + stPrefix + "[rel " + instr.args[0].name + "], " + getSizedReg("rax", st));
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
                    // CID-005: resolve the calling convention. Explicit
                    // Ms/SysV wins; Default follows the target descriptor,
                    // except that a direct call to a definition pinned after
                    // accept (interface implementations, T1 fix round 1)
                    // resolves from the callee FunctionIR so the call matches
                    // the emitted definition.
                    Abi resolvedCallAbi = instr.callAbi;
                    if (resolvedCallAbi == Abi::Default && !instr.args.empty() &&
                        instr.args[0].kind == IRValue::LABEL) {
                        for (const auto& calleeFn : module.getFunctions()) {
                            if (calleeFn->getName() == instr.args[0].name &&
                                calleeFn->getAbi() != Abi::Default) {
                                resolvedCallAbi = calleeFn->getAbi();
                                break;
                            }
                        }
                    }
                    if (resolvedCallAbi == Abi::Default) {
                        resolvedCallAbi = targetAbiFor(target_);
                    }
                    const bool msCall = (resolvedCallAbi == Abi::Ms &&
                                         instr.op == IROp::CALL &&
                                         !instr.args.empty());

                    // RSP must be 16-byte aligned at every call (design §2).
                    // The body's rsp is `rbp - totalStack + 8`; rbp is
                    // 16-aligned for a function entered by `call` (entry rsp
                    // ≡ 8) and 8 mod 16 for the SysV `_start` kernel entry
                    // (no return address). Under uefi `_start` is a normal
                    // function called by `call` (design §1), so only the
                    // native/pomeos kernel entry takes the bias. `callPad`
                    // is the 0/8 adjustment that closes the gap for the
                    // words still on the stack at the call.
                    const bool kernelEntry =
                        (fName == "_start" && !defMs && target_ != "uefi");
                    const int frameBias = kernelEntry ? 8 : 0;
                    auto callPad = [&](int stackWords) {
                        return (frameBias + 8 - (stackWords % 2) * 8) % 16;
                    };

                    std::vector<std::string> callerSaved = {"rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11"};
                    std::vector<std::string> toSave;
                    for (const auto& reg : callerSaved) {
                        for (const auto& [var, res] : alloc) { if (!res.isSpilled && res.reg == reg) { if (instr.dest && instr.dest->name == var && reg == "rax") continue; toSave.push_back(reg); break; } }
                    }
                    std::string destReg;
                    if (instr.dest && alloc.count(instr.dest->name) && !alloc.at(instr.dest->name).isSpilled) {
                        destReg = alloc.at(instr.dest->name).reg;
                    }
                    std::vector<std::string> toSaveXmm;
                    if (!msCall) {
                        // XMM registers are caller-saved for SysV callees
                        // (CID-027): a callee freely uses xmm8-xmm15 as
                        // scratch, so any float value held in an XMM reg
                        // across the call must be spilled here. MS callees
                        // preserve XMM6-15 (T2 saves exactly the ones a
                        // definition uses, full width), so an MS call needs
                        // no caller-side spill. The dest's own reg is skipped
                        // (the return value arrives in it).
                        for (const auto& [var, res] : alloc) {
                            if (!res.isSpilled && res.reg.rfind("xmm", 0) == 0 && res.reg != destReg) {
                                if (std::find(toSaveXmm.begin(), toSaveXmm.end(), res.reg) == toSaveXmm.end()) {
                                    toSaveXmm.push_back(res.reg);
                                }
                            }
                        }
                    }

                    if (msCall) {
                        int msStackArgs = 0;
                        if (!instr.args.empty()) {
                            size_t numArgs = instr.args.size() - 1;
                            msStackArgs = std::max(0, (int)numArgs - 4);
                        }
                        emitMsCall(instr, instr.args[0],
                                   instr.args[0].kind != IRValue::LABEL,
                                   callPad(msStackArgs));
                        break;
                    }

                    // SysV path: native/pomeos, and SysV-pinned callees under
                    // uefi. The pad accounts for the words that stay on the
                    // stack (callee-saved spills + SysV stack args) so rsp is
                    // 16-byte aligned at the call.
                    int stackArgs = 0;
                    if (instr.op == IROp::CALL && !instr.args.empty()) {
                        size_t numArgs = instr.args.size() - 1;
                        if (numArgs > 6) stackArgs = (int)numArgs - 6;
                    }
                    int padding = callPad((int)toSave.size() +
                                          (int)toSaveXmm.size() + stackArgs);

                    if (padding > 0) addText("    sub rsp, " + std::to_string(padding));
                    for (const auto& reg : toSave) addText("    push " + reg);
                    for (const auto& reg : toSaveXmm) {
                        addText("    sub rsp, 8");
                        addText("    movq [rsp], " + reg);
                    }

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
                        int vtableIndex = (int)instr.args[1].intValue;

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
                    for (auto it = toSaveXmm.rbegin(); it != toSaveXmm.rend(); ++it) {
                        addText("    movq " + *it + ", [rsp]");
                        addText("    add rsp, 8");
                    }
                    for (auto it = toSave.rbegin(); it != toSave.rend(); ++it) addText("    pop " + *it);
                    if (padding > 0) addText("    add rsp, " + std::to_string(padding));
                    if (instr.dest && !d.empty() && d != "none") {
                        IRType retType = instr.dest->type;
                        if (retType == IRType::F32)
                            addText("    movss " + (isMem(d) ? "dword " + d : d) + ", xmm0");
                        else if (retType == IRType::F64)
                            addText("    movsd " + (isMem(d) ? "qword " + d : d) + ", xmm0");
                        else
                            addText("    mov " + getSizedReg(d, retType) + ", " + getSizedReg("rax", retType));
                    }
                    break;
                }
                case IROp::PUSH: {
                    IRType pt = instr.args[0].type;
                    if (pt == IRType::F32 || pt == IRType::F64) {
                        // CID-003 follow-up (I4): `push xmm` is not an
                        // instruction. Route the float bits through rax;
                        // F32 pool entries/memory slots hold only 4 bytes,
                        // so load exactly that width.
                        std::string mv = (pt == IRType::F32) ? "movss" : "movsd";
                        std::string sz = (pt == IRType::F32) ? "dword " : "qword ";
                        if (isMem(a1)) {
                            addText("    " + mv + " xmm0, " + sz + a1);
                            addText("    movq rax, xmm0");
                        } else {
                            addText("    movq rax, " + a1);
                        }
                        addText("    push rax");
                        break;
                    }
                    if (instr.args[0].kind == IRValue::CONSTANT &&
                        instr.args[0].type != IRType::F32 && instr.args[0].type != IRType::F64 &&
                        ((int64_t)instr.args[0].intValue > (int64_t)INT32_MAX ||
                         (int64_t)instr.args[0].intValue < (int64_t)INT32_MIN)) {
                        // Same imm32-only push rule as call args (CID-018).
                        if ((uint64_t)instr.args[0].intValue <= 0xFFFFFFFFULL) {
                            addText("    mov eax, " + fmtImm(instr.args[0].intValue, instr.args[0].type));
                        } else {
                            addText("    mov rax, " + fmtImm(instr.args[0].intValue, instr.args[0].type));
                        }
                        addText("    push rax");
                    } else if (instr.args[0].kind == IRValue::CONSTANT) {
                        addText("    push " + fmtImm(instr.args[0].intValue, instr.args[0].type));
                    } else if (isMem(a1)) {
                        IRType t = instr.args[0].type;
                        if (t == IRType::I32 || t == IRType::U32 || t == IRType::BOOL) {
                            addText("    movsxd rax, " + getSizedReg(a1, IRType::I32));
                            addText("    push rax");
                        } else {
                            addText("    mov " + getSizedReg("rax", t) + ", " + getSizedReg(a1, t));
                            addText("    push rax");
                        }
                    } else {
                        addText("    push " + a1);
                    }
                    break;
                }
                case IROp::POP: {
                    IRType pt = (instr.dest ? instr.dest->type : IRType::I32);
                    if (pt == IRType::F32 || pt == IRType::F64) {
                        // CID-003 follow-up (I4): `pop xmm` is not an
                        // instruction; land the bits in rax and move them.
                        std::string mv = (pt == IRType::F32) ? "movss" : "movsd";
                        std::string sz = (pt == IRType::F32) ? "dword " : "qword ";
                        addText("    pop rax");
                        if (isMem(d)) {
                            addText("    movq xmm0, rax");
                            addText("    " + mv + " " + sz + d + ", xmm0");
                        } else {
                            addText("    movq " + d + ", rax");
                        }
                        break;
                    }
                    if (isMem(d)) {
                        addText("    pop rax");
                        addText("    mov qword " + d + ", rax");
                    } else {
                        addText("    pop " + d);
                    }
                    break;
                }
                case IROp::MEMCPY: {
                    std::string a3 = (instr.args.size() > 2 ? getOperand(instr.args[2]) : "0");
                    addText("    mov rdi, " + a1);
                    addText("    mov rsi, " + a2);
                    addText("    mov rcx, " + a3);
                    addText("    cld");
                    addText("    rep movsb");
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
                        genEpilogue();
                    } else {
                        // Void return: leave a well-defined 0 in eax so the
                        // CRT uses it as the exit status of a void main.
                        genEpilogue(true);
                    }
                    break;
                }
                default: break;
                }
            }
        }
        genEpilogue(func->getReturnType() == IRType::VOID);

        // Fix up the prologue frame size now that the whole body is emitted.
        // Frame-overrun fix (Cider port CID-009): a spilled value can be
        // addressed at an offset deeper than spillBase + regAllocStackSize
        // (IR temporaries with no allocator entry), so size the frame by the
        // deepest slot actually referenced. Compared against the FULL frame
        // (extraStack + pushed regs): comparing against extraStack alone
        // over-grows every spilling function by up to 48 bytes.
        if (prologueFixupIndex >= 0) {
            int frameSize = extraStack + savedRegsSize;
            int needed = maxSlotOffset + 8;
            if (needed > frameSize) {
                // The fixed-up rsp is `rbp - paddedTotal + 8` (the +8
                // is totalStack's call-push guard, which is never
                // subtracted), so a 16-aligned paddedTotal keeps the
                // same 16-byte phase as the original `sub rsp,
                // extraStack` — independent of savedRegsSize.
                int paddedTotal = ((needed + 15) / 16) * 16;
                textSection[(size_t)prologueFixupIndex] =
                    "    sub rsp, " + std::to_string(paddedTotal - savedRegsSize);
            }
        }
    }

    std::ofstream out(outputPath, std::ios::trunc);
    if (!out) return false;
    // 64-bit relative addressing is the standard for ELF output (Issue #18):
    // without this NASM warns that implicit absolute is deprecated.
    out << "default rel\n";
    out << "section .bss\n";
    for (const auto& line : bssSection) out << "    " << line << "\n";
    for (const auto& line : globalAsmSection) out << "    " << line << "\n";
    out << "\nsection .data\n";
    for (const auto& line : dataSection) out << "    " << line << "\n";
    for (const auto& line : module.getStringData()) out << "    " << line << "\n";
    out << "\nsection .text\n    extern malloc\n    extern free\n    extern _setjmp\n    extern longjmp\n";
    for (const auto& extFunc : module.getExternFunctions()) {
        out << "    extern " << extFunc << "\n";
    }
    // CID-006 F1: extern variables are data symbols; they need the same
    // `extern` directive as extern functions.
    for (const auto& extVar : module.getExternVariables()) {
        out << "    extern " << extVar << "\n";
    }
    for (const auto& line : textSection) out << line << "\n";

    if (target_ == "pomeos") {
        // Flush and close the initial emission first: its destructor would
        // otherwise re-flush the raw text over the processed result below.
        out.flush();
        out.close();
        // The PomeOS backend:
        // 1. Swap the Linux syscall numbers for PomeOS's (the asm blocks
        //    emit `mov rax, <linux>` + `syscall`; only the mov immediately
        //    preceding a syscall is rewritten).
        // 2. Replace the _setjmp/_longjmp externs with self-contained
        //    definitions so the output assembles standalone with pasm.
        std::string final;
        std::ifstream in(outputPath);
        final.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        in.close();

        static const std::map<int, int> kSyscallSwap = {
            {0, 12}, {1, 13}, {2, 10}, {3, 11}, {8, 34},  // read/write/open/close/lseek
            {9, 48}, {12, 9}, {39, 36}, {57, 33}, {60, 2}, {61, 14},  // mmap/sbrk/getpid/fork/exit/waitpid
        };
        std::istringstream ss(final);
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(ss, line)) lines.push_back(line);
        for (size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].find("syscall") == std::string::npos) continue;
            // nearest preceding `mov rax, N` / `mov eax, N`
            for (size_t j = i; j > 0 && i - j < 8; --j) {
                std::string t = lines[j - 1];
                size_t pos = t.find("mov ");
                if (pos == std::string::npos) continue;
                std::string rest = t.substr(pos + 4);
                if (rest.rfind("rax,", 0) != 0 && rest.rfind("eax,", 0) != 0) continue;
                size_t numPos = rest.find_first_of("0123456789");
                if (numPos == std::string::npos) continue;
                size_t end = numPos;
                while (end < rest.size() && isdigit((unsigned char)rest[end])) end++;
                int n = std::atoi(rest.substr(numPos, end - numPos).c_str());
                auto it = kSyscallSwap.find(n);
                if (it != kSyscallSwap.end()) {
                    lines[j - 1] = t.substr(0, pos + 4 + (rest[4] == 'a' ? 4 : 4)) + " " +
                                   std::to_string(it->second);
                }
                break;
            }
        }
        // Replace the externs with the runtime definitions.
        std::string result;
        for (const auto& l : lines) {
            if (l.find("extern _setjmp") != std::string::npos ||
                l.find("extern longjmp") != std::string::npos) {
                continue;
            }
            result += l + "\n";
        }
        result += R"(
; PomeOS runtime: _setjmp/longjmp (jmp_buf = 8 qwords)
global _setjmp
global longjmp
_setjmp:
    mov [rdi + 0], rbx
    mov [rdi + 8], rbp
    mov [rdi + 16], r12
    mov [rdi + 24], r13
    mov [rdi + 32], r14
    mov [rdi + 40], r15
    mov [rdi + 48], rsp
    mov rax, [rsp]
    mov [rdi + 56], rax
    xor eax, eax
    ret
longjmp:
    mov rbx, [rdi + 0]
    mov rbp, [rdi + 8]
    mov r12, [rdi + 16]
    mov r13, [rdi + 24]
    mov r14, [rdi + 32]
    mov r15, [rdi + 40]
    mov rsp, [rdi + 48]
    mov rax, rsi
    test eax, eax
    jnz .done
    mov eax, 1
.done:
    jmp qword [rdi + 56]
)";
        std::ofstream out2(outputPath, std::ios::trunc);
        out2 << result;
    }
    return true;
}

}  // namespace cider
