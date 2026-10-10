// vm.cpp — the v3 interpreter. See vm.h for the contract.

#include "vm/vm.h"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "layout.h"

namespace cider {
namespace vm {

using namespace ir;

namespace {

// A fail-fast guard, not a work cap: a program that runs this many
// instructions without returning is looping forever, and the harness would
// otherwise wait out its whole timeout before saying so.
constexpr uint64_t kStepLimit = 100000000;

constexpr uint64_t kByteBits = 8;
constexpr uint64_t kByteMask = 0xFF;
constexpr uint64_t kZeroCell = 0;
constexpr uint64_t kOneCell = 1;

// The largest value id the function defines, params included: ids are dense
// from zero, so the id width is a word count.
uint32_t topId(const Function& fn) {
    uint32_t top = kZeroCell;
    for (const Value& param : fn.params) {
        top = param.id > top ? param.id : top;
    }
    for (const Block& block : fn.blocks) {
        for (const Instr& instr : block.body) {
            for (const Value& result : instr.results) {
                top = result.id > top ? result.id : top;
            }
            for (const Value& operand : instr.operands) {
                top = operand.id > top ? operand.id : top;
            }
        }
    }
    return top;
}

// Id zero is never defined, so a frame is one extra cell beyond the ids.
uint64_t frameWords(uint32_t top) {
    return static_cast<uint64_t>(top) + kOneCell;
}

uint64_t widthBytes(IRType width) {
    switch (width) {
        case IRType::I8:
        case IRType::U8:
        case IRType::Bool:
            return 1;
        case IRType::I16:
        case IRType::U16:
            return 2;
        case IRType::I32:
        case IRType::U32:
        case IRType::F32:
            return 4;
        case IRType::I64:
        case IRType::U64:
        case IRType::F64:
        case IRType::Ptr:
            return 8;
        case IRType::Void:
            return 0;
    }
    return 0;
}

bool widthIsFloat(IRType width) {
    return width == IRType::F32 || width == IRType::F64;
}

int64_t signExtend(uint64_t bits, uint64_t bytes) {
    uint64_t shift = kByteBits * (sizeof(uint64_t) - bytes);
    return static_cast<int64_t>((bits << shift) >> shift);
}

struct Runner {
    VmResult out;
    std::unordered_map<uint32_t, int64_t> slots;
    std::vector<uint8_t> memory;
    std::unordered_set<uint32_t> slotIds;
    uint64_t steps = 0;

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

    void write(const Value& result, int64_t bits) {
        slots[result.id] = bits;
    }

    // The native frame covers every value id at rsp+id*8, and an alloca's
    // address names its own cell: the cell doubles as the storage. The
    // interpreter keeps the same cells, so an address is an id times eight.
    int64_t cellAddress(uint32_t id) {
        return static_cast<int64_t>(id) * layout::kSizePointer;
    }

    uint64_t loadBytes(int64_t address, uint64_t count, Position pos, bool& ok) {
        if (address < 0 || count > 8 ||
            static_cast<uint64_t>(address) + count > memory.size()) {
            ok = false;
            return 0;
        }
        uint64_t bits = 0;
        for (uint64_t i = 0; i < count; ++i) {
            bits |= static_cast<uint64_t>(memory[static_cast<size_t>(address) + i])
                    << (i * kByteBits);
        }
        (void)pos;
        ok = true;
        return bits;
    }

    bool storeBytes(int64_t address, uint64_t count, uint64_t bits, Position pos) {
        if (address < 0 || count > 8 ||
            static_cast<uint64_t>(address) + count > memory.size()) {
            return refuse(pos, "vm address out of range");
        }
        for (uint64_t i = 0; i < count; ++i) {
            memory[static_cast<size_t>(address) + i] =
                static_cast<uint8_t>((bits >> (i * kByteBits)) & kByteMask);
        }
        return true;
    }

    // Mirrors codegen's emitLoadMem: exactly the width's bytes, extended to
    // 64 bits the same way (signed types sign-extend, the rest zero-extend;
    // I32 sign-extends through eax like movsxd, U32 zeroes like a dword mov).
    bool load(Value result, TypeRef type, IRType width, int64_t address,
              Position pos) {
        if (widthIsFloat(width)) {
            return refuse(pos, "vm cannot load floats yet");
        }
        uint64_t count = widthBytes(width);
        if (count == 0) {
            return refuse(pos, "vm cannot load a void");
        }
        bool ok = false;
        uint64_t bits = loadBytes(address, count, pos, ok);
        if (!ok) {
            return refuse(pos, "vm address out of range");
        }
        bool isSigned = false;
        if (!type.isError() && type.type->kind() == Kind::Basic &&
            type.type->basic().has_value()) {
            isSigned = basicIsSigned(*type.type->basic());
        }
        if (width == IRType::I32 || isSigned) {
            write(result, signExtend(bits, count));
        } else {
            write(result, static_cast<int64_t>(bits));
        }
        return true;
    }

    bool store(TypeRef type, IRType width, int64_t address, int64_t bits,
               Position pos) {
        if (widthIsFloat(width)) {
            return refuse(pos, "vm cannot store floats yet");
        }
        uint64_t count = widthBytes(width);
        if (count == 0) {
            return refuse(pos, "vm cannot store a void");
        }
        (void)type;
        return storeBytes(address, count, static_cast<uint64_t>(bits), pos);
    }

    bool compare(const Instr& instr) {
        const Value& lhs = instr.operands[0];
        const Value& rhs = instr.operands[1];
        if (widthIsFloat(lhs.width) || widthIsFloat(rhs.width)) {
            return refuse(instr.loc, "vm cannot compare floats yet");
        }
        uint64_t count = widthBytes(lhs.width);
        if (count == 0 || count > 8) {
            return refuse(instr.loc, "vm cannot compare voids");
        }
        bool ok = false;
        (void)ok;
        int64_t a = read(lhs);
        int64_t b = read(rhs);
        // Narrow slots hold extended values (see load), so a full-width
        // comparison decides what the instruction says it decides.
        bool answer = false;
        switch (instr.cmp) {
            case CmpOp::Eq:
                answer = a == b;
                break;
            case CmpOp::Ne:
                answer = a != b;
                break;
            case CmpOp::Lt:
                answer = a < b;
                break;
            case CmpOp::Le:
                answer = a <= b;
                break;
            case CmpOp::Gt:
                answer = a > b;
                break;
            case CmpOp::Ge:
                answer = a >= b;
                break;
            case CmpOp::Ult:
                answer = static_cast<uint64_t>(a) < static_cast<uint64_t>(b);
                break;
            case CmpOp::Ule:
                answer = static_cast<uint64_t>(a) <= static_cast<uint64_t>(b);
                break;
            case CmpOp::Ugt:
                answer = static_cast<uint64_t>(a) > static_cast<uint64_t>(b);
                break;
            case CmpOp::Uge:
                answer = static_cast<uint64_t>(a) >= static_cast<uint64_t>(b);
                break;
        }
        write(instr.results[0], answer ? 1 : 0);
        return true;
    }

    bool step(const Instr& instr) {
        switch (instr.op) {
            case Op::Const:
                write(instr.results[0], instr.constantBits);
                return true;
            case Op::Copy:
                write(instr.results[0], read(instr.operands[0]));
                return true;
            case Op::Add:
                write(instr.results[0], read(instr.operands[0]) + read(instr.operands[1]));
                return true;
            case Op::Sub:
                write(instr.results[0], read(instr.operands[0]) - read(instr.operands[1]));
                return true;
            case Op::Mul:
                write(instr.results[0], read(instr.operands[0]) * read(instr.operands[1]));
                return true;
            case Op::Alloca:
                write(instr.results[0], cellAddress(instr.results[0].id));
                return true;
            case Op::Load: {
                int64_t base = read(instr.addressBase) + instr.displacement;
                return load(instr.results[0], instr.type, instr.width, base,
                            instr.loc);
            }
            case Op::Store: {
                int64_t base = read(instr.addressBase) + instr.displacement;
                IRType width = instr.storedValue.width;
                return store(instr.type, width, base,
                             read(instr.storedValue), instr.loc);
            }
            case Op::Cmp:
                return compare(instr);
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
        run.refuse(entry->loc, "the program defines both main and _start");
        return run.out;
    }
    const Function* fn = entry != nullptr ? entry : start;
    if (fn == nullptr) {
        run.out.error = "no entry function: define main or _start";
        return run.out;
    }
    if (fn->signature.arity() != 0) {
        run.refuse(fn->loc, "vm cannot pass arguments yet");
        return run.out;
    }
    if (fn->blocks.empty()) {
        run.refuse(fn->loc, "vm cannot run an empty function");
        return run.out;
    }
    // The frame covers every value id: each id owns one storage word, so
    // both an alloca cell and a scratch slot are addressable the moment
    // they are defined.
    uint32_t top = topId(*fn);
    uint64_t words = frameWords(top);
    uint64_t bytes = words * layout::kSizePointer;
    run.memory.assign(bytes, kZeroCell);
    std::unordered_map<uint32_t, size_t> blockAt;
    for (const Block& block : fn->blocks) {
        blockAt[block.label] = blockAt.size();
    }
    // The lowerer emits the entry block first, so the front of the list is
    // the entry — the same assumption the emitter's _start trampoline makes
    // when it falls into the function's first block.
    size_t current = kZeroCell;
    for (;;) {
        if (++run.steps > kStepLimit) {
            run.refuse(fn->loc, "vm step limit exhausted");
            return run.out;
        }
        const Block& block = fn->blocks[current];
        for (const Instr& instr : block.body) {
            if (!run.step(instr)) {
                return run.out;
            }
        }
        const Terminator& term = block.term;
        switch (term.term) {
            case Term::Ret: {
                if (term.values.empty()) {
                    run.out.ok = true;
                    run.out.exitCode = 0;
                    return run.out;
                }
                // The exit status answers the low bits of the program's
                // value; the interpreter answers the same bits so the two
                // legs compare without a normalising rule.
                auto found = run.slots.find(term.values[0].id);
                int64_t bits = found == run.slots.end() ? 0 : found->second;
                run.out.ok = true;
                run.out.exitCode = static_cast<int>(bits & kByteMask);
                return run.out;
            }
            case Term::Br: {
                auto found = blockAt.find(term.target);
                if (found == blockAt.end()) {
                    run.refuse(term.loc, "vm branch names no block");
                    return run.out;
                }
                current = found->second;
                break;
            }
            case Term::CondBr: {
                if (!term.cond.has_value()) {
                    run.refuse(term.loc, "vm branch tests nothing");
                    return run.out;
                }
                auto found = run.slots.find(term.cond->id);
                int64_t tested = found == run.slots.end() ? 0 : found->second;
                uint32_t label = tested != 0 ? term.thenTarget : term.elseTarget;
                auto at = blockAt.find(label);
                if (at == blockAt.end()) {
                    run.refuse(term.loc, "vm branch names no block");
                    return run.out;
                }
                current = at->second;
                break;
            }
            case Term::Switch:
            case Term::Trap:
            case Term::Unreachable:
                run.refuse(term.loc, "vm cannot run this terminator yet");
                return run.out;
        }
    }
}

}  // namespace vm
}  // namespace cider
