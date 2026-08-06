#include "ir.h"

#include <sstream>

namespace cider {

std::string IRInstruction::toString() const {
    std::stringstream ss;
    if (dest) ss << dest->toString() << " = ";

    switch (op) {
    case IROp::ADD: ss << "add "; break;
    case IROp::SUB: ss << "sub "; break;
    case IROp::MUL: ss << "mul "; break;
    case IROp::DIV: ss << "div "; break;
    case IROp::MOD: ss << "mod "; break;
    case IROp::BIT_AND: ss << "and "; break;
    case IROp::BIT_OR: ss << "or "; break;
    case IROp::BIT_XOR: ss << "xor "; break;
    case IROp::BIT_NOT: ss << "not "; break;
    case IROp::LSHIFT: ss << "shl "; break;
    case IROp::RSHIFT: ss << "shr "; break;
    case IROp::CMP_EQ: ss << "icmp eq "; break;
    case IROp::CMP_NE: ss << "icmp ne "; break;
    case IROp::CMP_LT: ss << "icmp lt "; break;
    case IROp::CMP_GT: ss << "icmp gt "; break;
    case IROp::CMP_LE: ss << "icmp le "; break;
    case IROp::CMP_GE: ss << "icmp ge "; break;
    case IROp::LOAD: ss << "load "; break;
    case IROp::STORE: ss << "store "; break;
    case IROp::ALLOCA: ss << "alloca "; break;
    case IROp::MOV: ss << "mov "; break;
    case IROp::MEMCPY: ss << "memcpy "; break;
    case IROp::PUSH: ss << "push "; break;
    case IROp::POP: ss << "pop "; break;
    case IROp::BR: ss << "br "; break;
    case IROp::COND_BR: ss << "br "; break;
    case IROp::CALL: ss << "call "; break;
    case IROp::VIRTUAL_CALL: ss << "vcall "; break;
    case IROp::RET: ss << "ret "; break;
    case IROp::PHI: ss << "phi "; break;
    case IROp::CVT_ITOF: ss << "cvt_itof "; break;
    case IROp::CVT_FTOI: ss << "cvt_ftoi "; break;
    case IROp::ASM: ss << "asm "; break;
    }

    if (op == IROp::PHI) {
        bool first = true;
        for (const auto& [block, val] : phiArgs) {
            if (!first) ss << ", ";
            ss << "[" << val.toString() << ", " << block << "]";
            first = false;
        }
    } else {
        for (size_t i = 0; i < args.size(); ++i) {
            if (i > 0) ss << ", ";
            ss << args[i].toString();
        }
    }

    return ss.str();
}

void ModuleIR::print() const {
    for (const auto& func : functions_) {
        std::cout << "define " << func->getName() << "() {\n";
        for (const auto& block : func->getBlocks()) {
            std::cout << block->getName() << ":\n";
            for (const auto& instr : block->getInstructions()) {
                std::cout << "  " << instr.toString() << "\n";
            }
        }
        std::cout << "}\n\n";
    }
}

}  // namespace cider
