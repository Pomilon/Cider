#include "codegen.h"

#include <cstdlib>
#include <sstream>

namespace cider {

namespace {

int regIndex(const std::string& reg) {
    if (reg.empty() || reg[0] != '%') return -1;
    char* end = nullptr;
    long v = std::strtol(reg.c_str() + 1, &end, 10);
    if (end == reg.c_str() + 1 || v < 0) return -1;
    return static_cast<int>(v);
}

std::string slotFor(const std::string& reg) {
    return "[rbp - " + std::to_string(8 * (regIndex(reg) + 1)) + "]";
}

std::string loadValue(const Operand& o, const char* dst) {
    if (o.isReg) return "    mov " + std::string(dst) + ", " + slotFor(o.reg) + "\n";
    return std::string("    mov ") + dst + ", " + std::to_string(o.imm) + "\n";
}

std::string escapeData(const std::string& s) {
    std::ostringstream os;
    for (unsigned char c : s) {
        if (c == '\n') os << "10, ";
        else if (c == '\t') os << "9, ";
        else if (c == '\\') os << "'\\\\', ";
        else if (c == '"') os << "'\\\"', ";
        else if (c >= 32 && c < 127) os << "'" << static_cast<char>(c) << "', ";
        else os << static_cast<int>(c) << ", ";
    }
    os << "0";
    return os.str();
}

}  // namespace

std::string generateAsm(const Program& prog) {
    const Function* main = nullptr;
    for (const auto& fn : prog.functions) {
        if (fn.name == "main") { main = &fn; break; }
    }
    if (!main) throw CodegenError{"no main function"};

    int maxReg = -1;
    auto scanReg = [&](const std::string& r) {
        int idx = regIndex(r);
        if (idx > maxReg) maxReg = idx;
    };
    for (const auto& in : main->instrs) {
        if (!in.dest.empty()) scanReg(in.dest);
        for (const auto& a : in.args) if (a.isReg) scanReg(a.reg);
    }

    int frame = (maxReg + 1) * 8 + 8;
    frame = (frame + 15) & ~15;

    std::ostringstream os;
    os << "section .data\n";
    for (size_t i = 0; i < prog.strings.size(); ++i) {
        os << "str" << i << ": db " << escapeData(prog.strings[i]) << "\n";
    }
    os << "section .text\n"
       << "global _start\n"
       << "_start:\n"
       << "    push rbp\n"
       << "    mov rbp, rsp\n"
       << "    sub rsp, " << frame << "\n";

    for (const auto& in : main->instrs) {
        switch (in.op) {
            case Op::CONST:
                os << loadValue(in.args[0], "rax");
                os << "    mov " << slotFor(in.dest) << ", rax\n";
                break;
            case Op::STR: {
                int64_t idx = in.args[0].imm;
                os << "    lea rax, [rel str" << idx << "]\n";
                os << "    mov " << slotFor(in.dest) << ", rax\n";
                break;
            }
            case Op::WRITE:
                os << loadValue(in.args[0], "rdi");
                os << loadValue(in.args[1], "rsi");
                os << loadValue(in.args[2], "rdx");
                os << "    mov rax, 1\n"
                   << "    syscall\n";
                break;
            case Op::EXIT:
                os << loadValue(in.args[0], "rdi");
                os << "    mov rax, 60\n"
                   << "    syscall\n";
                break;
        }
    }
    os << "    mov rax, 60\n"
       << "    xor edi, edi\n"
       << "    syscall\n";
    return os.str();
}

}  // namespace cider
