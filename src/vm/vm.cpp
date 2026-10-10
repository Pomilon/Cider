// vm.cpp — the v3 interpreter. See vm.h for the contract.

#include "vm/vm.h"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "layout.h"
#include "types/context.h"

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

// The byte count a width's own register form owns, so a narrow operation
// narrows its answer to exactly that.
uint64_t narrowBits(IRType width) {
    if (width == IRType::I8 || width == IRType::U8 || width == IRType::Bool) {
        return 8;
    }
    if (width == IRType::I16 || width == IRType::U16) {
        return 16;
    }
    if (width == IRType::I32 || width == IRType::U32) {
        return 32;
    }
    return 64;
}

// The shift count the hardware keeps at each operand size (SEMANTICS §3):
// five bits at 32 bits and below, six at 64.
uint64_t shiftMaskBits(IRType width) {
    return narrowBits(width) == 64 ? 6 : 5;
}

// Truncates a computed word to the width and re-extends it, which is what
// the emitter's post-operation extension does. It also answers a width's
// own value: a division form reads exactly the width's bits, so the same
// read produces its operand.
int64_t extendToWidth(int64_t bits, IRType width, TypeRef type) {
    uint64_t count = narrowBits(width);
    uint64_t mask = count >= 64 ? ~uint64_t{0} : (uint64_t{1} << count) - 1;
    uint64_t trimmed = static_cast<uint64_t>(bits) & mask;
    if (!ir::signedWidth(width, type) || count >= 64) {
        return static_cast<int64_t>(trimmed);
    }
    uint64_t sign = uint64_t{1} << (count - 1);
    if ((trimmed & sign) == 0) {
        return static_cast<int64_t>(trimmed);
    }
    return static_cast<int64_t>(trimmed | ~mask);
}

// The low bits of a word at a width, with no extension: the operand an
// unsigned form divides by, and the value a shift moves.
uint64_t trimmedToWidth(int64_t bits, IRType width) {
    uint64_t count = narrowBits(width);
    uint64_t mask = count >= 64 ? ~uint64_t{0} : (uint64_t{1} << count) - 1;
    return static_cast<uint64_t>(bits) & mask;
}

int64_t signExtend(uint64_t bits, uint64_t bytes) {
    uint64_t shift = kByteBits * (sizeof(uint64_t) - bytes);
    return static_cast<int64_t>((bits << shift) >> shift);
}

struct Runner {
    VmResult out;
    // The interpreter's own context, for the basic type a cast needs when it
    // answers a signedness question. It holds no program types.
    TypeContext ctx;
    std::unordered_map<uint32_t, int64_t> slots;
    std::vector<uint8_t> memory;
    std::unordered_set<uint32_t> slotIds;
    // Link label to the address its bytes were laid down at, and link name
    // to the tag an indirect call resolves by.
    std::unordered_map<std::string, int64_t> labels;
    std::unordered_map<std::string, int64_t> functionTags;
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

    // A byte address in the interpreter's flat memory. Negative means no
    // address, which only an instruction can ask for by naming a label the
    // module never laid down.
    int64_t appendBytes(const uint8_t* data, uint64_t count) {
        // Pad to a word boundary so an aligned reader does not straddle two
        // blobs: the frame is a whole number of words, and the globals that
        // follow it must start at one too.
        uint64_t slide = memory.size();
        uint64_t remainder = slide % layout::kSizePointer;
        if (remainder != kZeroCell) {
            memory.insert(memory.end(), layout::kSizePointer - remainder,
                          kZeroCell);
        }
        int64_t at = static_cast<int64_t>(memory.size());
        memory.insert(memory.end(), data, data + count);
        return at;
    }

    // Word-sized little-endian bytes for one value, which is what a scalar
    // or aggregate global's image holds.
    void appendWord(uint64_t bits) {
        uint8_t bytes[layout::kSizePointer];
        for (uint64_t i = 0; i < layout::kSizePointer; ++i) {
            bytes[i] = static_cast<uint8_t>((bits >> (i * kByteBits)) & kByteMask);
        }
        appendBytes(bytes, layout::kSizePointer);
    }

    // True when the module holds a label's bytes: a rodata blob answers its
    // address, a global its own, and a vtable the tags of the functions its
    // entries name. Anything else names a label nothing laid down, which is
    // a lowering bug rather than a value.
    bool addressOf(const Instr& instr) {
        auto found = labels.find(instr.globalLabel);
        if (found == labels.end()) {
            return refuse(instr.loc, "vm cannot address '" + instr.globalLabel + "'");
        }
        write(instr.results[0], found->second);
        return true;
    }

    bool tagOf(const Instr& instr) {
        auto found = functionTags.find(instr.globalLabel);
        if (found == functionTags.end()) {
            return refuse(instr.loc, "vm cannot address '" + instr.globalLabel + "'");
        }
        write(instr.results[0], found->second);
        return true;
    }

    // A memory instruction names its place two ways: a link label, and an
    // address value. The label wins when it is set, which is what makes a
    // global read the same instruction shape a slot read is.
    int64_t labelAddress(const Instr& instr, const std::string& label) {
        auto found = labels.find(label);
        if (found == labels.end()) {
            refuse(instr.loc, "vm cannot address '" + label + "'");
            return 0;
        }
        return found->second + instr.displacement;
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

    // Every integer form computes at 64 bits and then re-extends, which is
    // what the emitter's post-operation extension does: a narrow sum wraps
    // because the width's own answer is what a later load reads.
    bool logic(const Instr& instr) {
        if (instr.width == ir::IRType::F32 || instr.width == ir::IRType::F64) {
            return refuse(instr.loc, std::string("vm cannot run ") +
                                         opName(instr.op) + " on floats yet");
        }
        int64_t a = read(instr.operands[0]);
        int64_t b = read(instr.operands[1]);
        int64_t answer = 0;
        switch (instr.op) {
            case Op::Add:
                answer = a + b;
                break;
            case Op::Sub:
                answer = a - b;
                break;
            case Op::Mul:
                if (instr.width == ir::IRType::I8 ||
                    instr.width == ir::IRType::U8) {
                    // The 8-bit multiply has no two-operand form, so the
                    // product lands in a 16-bit register and the extension
                    // reads that: the answer is the product at sixteeen
                    // bits, not the product at eight.
                    uint16_t product = static_cast<uint16_t>(
                        static_cast<uint16_t>(a) * static_cast<uint16_t>(b));
                    bool narrowSigned = ir::signedWidth(instr.width, instr.type);
                    answer = narrowSigned
                                 ? static_cast<int64_t>(static_cast<int16_t>(product))
                                 : static_cast<int64_t>(product);
                    write(instr.results[0], answer);
                    return true;
                }
                answer = a * b;
                break;
            case Op::And:
                answer = a & b;
                break;
            case Op::Or:
                answer = a | b;
                break;
            case Op::Xor:
                answer = a ^ b;
                break;
            default:
                return refuse(instr.loc, "vm met an unknown logic instruction");
        }
        write(instr.results[0], extendToWidth(answer, instr.width, instr.type));
        return true;
    }

    // A division runs at the operand's own width, faults on a zero divisor
    // and refuses the MIN/-1 quotient that does not fit — the two things the
    // emit forms trap on. A signed divide truncates toward zero, and a
    // remainder follows the dividend's sign, both by the hardware's
    // definition rather than a rule of the interpreter's own.
    bool divide(const Instr& instr) {
        if (instr.width == ir::IRType::F32 || instr.width == ir::IRType::F64) {
            return refuse(instr.loc, "vm cannot run float division yet");
        }
        int64_t a = extendToWidth(read(instr.operands[0]), instr.width, instr.type);
        int64_t b = extendToWidth(read(instr.operands[1]), instr.width, instr.type);
        if (b == 0) {
            return refuse(instr.loc, "vm cannot divide by zero");
        }
        bool wantQuotient = instr.op == Op::Div;
        int64_t answer = 0;
        if (ir::signedWidth(instr.width, instr.type)) {
            // The signed form's widest quotient is the width's most
            // negative value; dividing it by minus one leaves the field.
            int64_t least = extendToWidth(~uint64_t{0} << (narrowBits(instr.width) - 1),
                                          instr.width, instr.type);
            if (a == least && b == -1) {
                return refuse(instr.loc, "this division overflows");
            }
            int64_t quotient = a / b;
            int64_t remainder = a % b;
            answer = wantQuotient ? quotient : remainder;
        } else {
            uint64_t ua = static_cast<uint64_t>(a);
            uint64_t ub = static_cast<uint64_t>(b);
            answer = static_cast<int64_t>(wantQuotient ? ua / ub : ua % ub);
        }
        write(instr.results[0], extendToWidth(answer, instr.width, instr.type));
        return true;
    }

    // A shift moves the value at its own width by a count the hardware
    // keeps (SEMANTICS §3), then re-extends: an arithmetic right shift
    // fills with the sign bit, a logical one with zeroes.
    bool shift(const Instr& instr) {
        if (instr.width == ir::IRType::F32 || instr.width == ir::IRType::F64) {
            return refuse(instr.loc, "vm cannot run float shifts yet");
        }
        uint64_t bits = narrowBits(instr.width);
        uint64_t count = static_cast<uint64_t>(read(instr.operands[1])) &
                         ((uint64_t{1} << shiftMaskBits(instr.width)) - 1);
        uint64_t value = trimmedToWidth(read(instr.operands[0]), instr.width);
        uint64_t answer = 0;
        if (instr.op == Op::Shl) {
            answer = (value << count) & ((uint64_t{1} << bits) - 1);
        } else if (ir::signedWidth(instr.width, instr.type)) {
            int64_t extended = extendToWidth(read(instr.operands[0]), instr.width,
                                             instr.type);
            answer = trimmedToWidth(extended >> count, instr.width);
        } else {
            answer = value >> count;
        }
        write(instr.results[0], extendToWidth(static_cast<int64_t>(answer),
                                              instr.width, instr.type));
        return true;
    }

    // An integer or address cast truncates, re-extends or widens, exactly as
    // the emitter's integer path does: a same-width cross-signedness cast
    // re-extends through the target, a narrow one truncates through the
    // source first, and a pointer rides the 64-bit move. Floats, which need
    // the SSE and x87 sequences, are not here yet and say so.
    bool cast(const Instr& instr) {
        if (instr.operands.size() != kOneCell || instr.results.empty()) {
            return refuse(instr.loc, "a bad cast");
        }
        TypeRef from = instr.operands[0].type;
        TypeRef to = instr.type;
        if (from.isError() || to.isError()) {
            return refuse(instr.loc, "this cast is not emitted yet");
        }
        auto isFloatType = [](TypeRef t) {
            return !t.isError() && t.type->kind() == Kind::Basic &&
                   basicIsFloat(*t.type->basic());
        };
        if (isFloatType(from) || isFloatType(to)) {
            return refuse(instr.loc, "vm cannot run float casts yet");
        }
        bool fromPtr = from.type->kind() == Kind::Pointer ||
                       from.type->kind() == Kind::FunctionPtr;
        bool toPtr = to.type->kind() == Kind::Pointer ||
                     to.type->kind() == Kind::FunctionPtr;
        auto scalarKind = [](TypeRef t) {
            Kind kind = t.type->kind();
            return kind == Kind::Basic || kind == Kind::Enum;
        };
        if (!fromPtr && !toPtr && (!scalarKind(from) || !scalarKind(to))) {
            return refuse(instr.loc, "this cast is not emitted yet");
        }
        // A pointer participates as its machine width; an enum rides its
        // underlying width (FEATURES §2).
        auto bitsOf = [](TypeRef t, bool ptr) {
            if (ptr) {
                return 64;
            }
            if (t.type->kind() == Kind::Enum) {
                return basicWidthBits(BasicKind::I32);
            }
            return basicWidthBits(*t.type->basic());
        };
        int fromBits = bitsOf(from, fromPtr);
        int toBits = bitsOf(to, toPtr);
        auto asKind = [](TypeRef t, bool ptr) -> BasicKind {
            if (ptr) {
                return BasicKind::U64;
            }
            if (t.type->kind() == Kind::Enum) {
                return BasicKind::I32;
            }
            return *t.type->basic();
        };
        // The width's own register form, and the answer extended through
        // the target's signedness — one place, so a cast and an arithmetic
        // operation narrow the same way.
        auto widthFor = [](int bits) {
            if (bits <= 8) {
                return ir::IRType::U8;
            }
            if (bits <= 16) {
                return ir::IRType::U16;
            }
            if (bits <= 32) {
                return ir::IRType::U32;
            }
            return ir::IRType::U64;
        };
        auto extendFor = [&](BasicKind kind, int64_t bits) {
            TypeRef named{ctx.basic(kind)};
            if (basicIsSigned(kind)) {
                return extendToWidth(bits, widthFor(basicWidthBits(kind)), named);
            }
            return static_cast<int64_t>(trimmedToWidth(
                bits, widthFor(basicWidthBits(kind))));
        };
        int64_t bits = read(instr.operands[0]);
        if (fromBits == toBits) {
            // Same width, different signedness: the source's extension is
            // already in the word, so a cross-signed cast re-extends through
            // the target and every other pair keeps the word.
            bool fromInt = !fromPtr && scalarKind(from);
            bool toInt = !toPtr && scalarKind(to);
            BasicKind fromKind = asKind(from, fromPtr);
            BasicKind toKind = asKind(to, toPtr);
            bool crossSign =
                fromInt && toInt && basicIsSigned(fromKind) != basicIsSigned(toKind);
            if (!crossSign) {
                write(instr.results[0], bits);
                return true;
            }
            write(instr.results[0], extendFor(toKind, bits));
            return true;
        }
        // A narrow target truncates through the source width first, so a
        // wide word keeps the low bits whatever the high ones held.
        if (fromBits > toBits) {
            bits = extendFor(asKind(from, fromPtr), bits);
        }
        write(instr.results[0], extendFor(asKind(to, toPtr), bits));
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
            case Op::Sub:
            case Op::Mul:
            case Op::And:
            case Op::Or:
            case Op::Xor:
                return logic(instr);
            case Op::Div:
            case Op::Rem:
                return divide(instr);
            case Op::Shl:
            case Op::Shr:
                return shift(instr);
            case Op::Neg:
                // No extension: the emitter negates the whole word, and a
                // later load narrows to the width's own bytes.
                write(instr.results[0], -read(instr.operands[0]));
                return true;
            case Op::BitNot:
                write(instr.results[0], ~read(instr.operands[0]));
                return true;
            case Op::Not:
                write(instr.results[0], read(instr.operands[0]) == 0 ? 1 : 0);
                return true;
            case Op::Alloca:
                write(instr.results[0], cellAddress(instr.results[0].id));
                return true;
            case Op::Gep: {
                // Base plus byte offset, plus a runtime index: a base that
                // is a slot holds its own cell address, and a base that is
                // a computed address holds that address instead, so both
                // arrive as a value and add the same way.
                int64_t base = read(instr.addressBase) + instr.displacement;
                if (!instr.operands.empty()) {
                    base += read(instr.operands[0]);
                }
                write(instr.results[0], base);
                return true;
            }
            case Op::Load: {
                int64_t base = instr.globalLabel.empty()
                                   ? read(instr.addressBase) + instr.displacement
                                   : labelAddress(instr, instr.globalLabel);
                return load(instr.results[0], instr.type, instr.width, base,
                            instr.loc);
            }
            case Op::Store: {
                int64_t base = instr.globalLabel.empty()
                                   ? read(instr.addressBase) + instr.displacement
                                   : labelAddress(instr, instr.globalLabel);
                IRType width = instr.storedValue.width;
                return store(instr.type, width, base,
                             read(instr.storedValue), instr.loc);
            }
            case Op::Cmp:
                return compare(instr);
            case Op::Cast:
                return cast(instr);
            case Op::DataAddr:
                return addressOf(instr);
            case Op::FnAddr:
                return tagOf(instr);
            case Op::FAdd:
            case Op::FSub:
            case Op::FMul:
            case Op::FDiv:
            case Op::FRem:
            case Op::Extract:
            case Op::Insert:
            case Op::Call:
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
    // A function link name names one address by tag: id zero is never a
    // function, so a tag of zero is the "no such function" answer a caller
    // cannot confuse with a real target.
    for (const Function& any : module.functions) {
        run.functionTags[any.name] = static_cast<int64_t>(&any - module.functions.data()) + 1;
    }
    // The frame owns the low addresses, so the program's constants and
    // globals lay down after it and nothing the entry frame stores can
    // collide with an address a load reads.
    for (const Rodata& blob : module.rodata) {
        run.labels[blob.label] = run.appendBytes(
            reinterpret_cast<const uint8_t*>(blob.bytes.data()),
            static_cast<uint64_t>(blob.bytes.size()));
    }
    for (const Global& global : module.globals) {
        size_t at = run.memory.size();
        if (!global.initLabels.empty()) {
            // A vtable global is one word per entry, each the tag of the
            // entry's function.
            for (const std::string& entry : global.initLabels) {
                auto found = run.functionTags.find(entry);
                if (found == run.functionTags.end()) {
                    run.refuse(fn->loc, "vm cannot address '" + entry + "'");
                    return run.out;
                }
                run.appendWord(static_cast<uint64_t>(found->second));
            }
        } else if (!global.initWords.empty()) {
            for (uint64_t word : global.initWords) {
                run.appendWord(word);
            }
        } else if (!global.initLabel.empty()) {
            // A string global names its bytes: the address of them, then the
            // length, is the pair a string value reads.
            auto found = run.labels.find(global.initLabel);
            if (found == run.labels.end()) {
                run.refuse(fn->loc, "vm cannot address '" + global.initLabel + "'");
                return run.out;
            }
            run.appendWord(static_cast<uint64_t>(found->second));
            run.appendWord(static_cast<uint64_t>(global.initial));
        } else {
            run.appendWord(static_cast<uint64_t>(global.initial));
        }
        run.labels[global.label] = static_cast<int64_t>(at);
    }
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
