#include "vm.h"

#include <cstdint>
#include <map>

namespace cider {

namespace {
constexpr int64_t kPoolBase = 0x1000;
constexpr int64_t kStepLimit = 1000000;
}

VMResult runVM(const Program& prog) {
    const Function* main = nullptr;
    for (const auto& fn : prog.functions) {
        if (fn.name == "main") { main = &fn; break; }
    }
    if (!main) throw VMError{"no main function"};

    VMResult result;
    std::map<std::string, int64_t> regs;
    int64_t steps = 0;

    auto valueOf = [&](const Operand& o) -> int64_t {
        if (o.isReg) {
            auto it = regs.find(o.reg);
            if (it == regs.end()) throw VMError{"read of unset register " + o.reg};
            return it->second;
        }
        return o.imm;
    };

    for (const auto& instr : main->instrs) {
        if (++steps > kStepLimit) throw VMError{"vm: step limit exceeded"};
        switch (instr.op) {
            case Op::CONST:
                regs[instr.dest] = valueOf(instr.args[0]);
                break;
            case Op::STR: {
                int64_t idx = instr.args[0].imm;
                if (idx < 0 || static_cast<size_t>(idx) >= prog.strings.size())
                    throw VMError{"str: bad pool index"};
                regs[instr.dest] = kPoolBase + idx;
                break;
            }
            case Op::WRITE: {
                int64_t fd = valueOf(instr.args[0]);
                int64_t ptr = valueOf(instr.args[1]);
                int64_t len = valueOf(instr.args[2]);
                if (ptr < kPoolBase || ptr >= kPoolBase + static_cast<int64_t>(prog.strings.size()))
                    throw VMError{"write: invalid pointer"};
                const std::string& s = prog.strings[static_cast<size_t>(ptr - kPoolBase)];
                if (len < 0 || static_cast<size_t>(len) > s.size())
                    throw VMError{"write: invalid length"};
                std::string bytes = s.substr(0, static_cast<size_t>(len));
                if (fd == 1) result.stdoutText += bytes;
                else if (fd == 2) result.stderrText += bytes;
                else throw VMError{"write: unsupported fd"};
                break;
            }
            case Op::EXIT:
                result.exitCode = static_cast<int>(valueOf(instr.args[0]));
                return result;
        }
    }
    return result;  // fell off the end: exit 0
}

}  // namespace cider
