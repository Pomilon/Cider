#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cider {

enum class Op : uint8_t {
    CONST,  // %dest = <imm>            (i64)
    STR,    // %dest = "<literal>"      (ptr)
    WRITE,  // write <fd>, <ptr>, <len> (fd/len may be immediates)
    EXIT,   // exit <code>              (code may be immediate)
};

inline const char* opName(Op op) {
    switch (op) {
        case Op::CONST: return "const";
        case Op::STR:   return "str";
        case Op::WRITE: return "write";
        case Op::EXIT:  return "exit";
    }
    return "?";
}

// A register operand is `reg` (e.g. "%0"); an immediate is `imm`.
struct Operand {
    bool isReg = false;
    std::string reg;
    int64_t imm = 0;
};

struct Instruction {
    Op op;
    std::string dest;                 // "" for ops without a dest (write/exit)
    std::vector<Operand> args;
    int line = 0;                     // source line for diagnostics
};

struct Function {
    std::string name;
    std::vector<Instruction> instrs;
};

struct Program {
    std::vector<Function> functions;
    std::vector<std::string> strings; // STR literal pool
};

}  // namespace cider
