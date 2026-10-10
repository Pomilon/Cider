// vm.cpp — the v3 interpreter. See vm.h for the contract.

#include "vm/vm.h"

#include <cstdint>
#include <unordered_map>

namespace cider {
namespace vm {

using namespace ir;

namespace {

struct Runner {
    VmResult out;
    std::unordered_map<uint32_t, int64_t> slots;

    bool refuse(Position pos, const std::string& message) {
        out.error = message;
        out.pos = pos;
        return false;
    }

    int64_t read(const Value& value) {
        auto found = slots.find(value.id);
        if (found == slots.end()) {
            return 0;
        }
        return found->second;
    }

    bool step(const Instr& instr) {
        auto take = [&](const Value& result, int64_t bits) {
            slots[result.id] = bits;
            return true;
        };
        switch (instr.op) {
            case Op::Const:
                return take(instr.results[0], instr.constantBits);
            case Op::Copy:
                return take(instr.results[0], read(instr.operands[0]));
            case Op::Add:
                return take(instr.results[0],
                            read(instr.operands[0]) + read(instr.operands[1]));
            case Op::Sub:
                return take(instr.results[0],
                            read(instr.operands[0]) - read(instr.operands[1]));
            case Op::Mul:
                return take(instr.results[0],
                            read(instr.operands[0]) * read(instr.operands[1]));
            case Op::Div:
            case Op::Rem:
            case Op::And:
            case Op::Or:
            case Op::Xor:
            case Op::Shl:
            case Op::Shr:
            case Op::Neg:
            case Op::Not:
            case Op::BitNot:
            case Op::FAdd:
            case Op::FSub:
            case Op::FMul:
            case Op::FDiv:
            case Op::FRem:
            case Op::Cmp:
            case Op::Load:
            case Op::Store:
            case Op::Alloca:
            case Op::DataAddr:
            case Op::FnAddr:
            case Op::Extract:
            case Op::Insert:
            case Op::Gep:
            case Op::Call:
            case Op::Cast:
            case Op::Trap:
            case Op::Asm:
                return refuse(instr.loc,
                              std::string("vm cannot run ") + opName(instr.op) + " yet");
        }
        return refuse(instr.loc, "vm reached an unknown instruction");
    }
};

}  // namespace

VmResult run(const ir::Module& module) {
    Runner run;
    const Function* entry = nullptr;
    const Function* start = nullptr;
    for (const Function& fn : module.functions) {
        if (fn.name == "main") {
            entry = &fn;
        }
        if (fn.name == "_start") {
            start = &fn;
        }
    }
    if (entry != nullptr && start != nullptr) {
        return run.refuse(entry->loc, "the program defines both main and _start"),
               run.out;
    }
    const Function* fn = entry != nullptr ? entry : start;
    if (fn == nullptr) {
        run.out.error = "no entry function: define main or _start";
        return run.out;
    }
    if (fn->signature.arity() != 0) {
        return run.refuse(fn->loc, "vm cannot pass arguments yet"), run.out;
    }
    if (fn->blocks.empty()) {
        return run.refuse(fn->loc, "vm cannot run an empty function"), run.out;
    }
    // The lowerer emits the entry block first, so blocks[0] is the entry —
    // the same assumption the native emitter's _start trampoline makes when
    // it falls into the function's first block.
    const Block& block = fn->blocks[0];
    for (const Instr& instr : block.body) {
        if (!run.step(instr)) {
            return run.out;
        }
    }
    if (block.term.term != Term::Ret) {
        return run.refuse(block.term.loc, "vm cannot run a program that falls off"),
               run.out;
    }
    if (block.term.values.empty()) {
        run.out.ok = true;
        run.out.exitCode = 0;
        return run.out;
    }
    // The native leg answers the low eight bits through the process exit
    // status; the interpreter answers the same bits so the legs compare.
    run.out.ok = true;
    run.out.exitCode = static_cast<int>(run.read(block.term.values[0]) & 0xFF);
    return run.out;
}

}  // namespace vm
}  // namespace cider
