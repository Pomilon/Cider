// vm.cpp — the v3 interpreter. See vm.h for the contract.

#include "vm/vm.h"

#include <cmath>
#include <cstdint>
#include <cstring>
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
constexpr uint64_t kTwoWords = 2;
constexpr uint64_t kThreeWords = 3;

// Digits, bytes and scales the host hand-outs replay: twenty hold any
// 64-bit decimal, six fraction digits follow the point.
constexpr uint64_t kMaxDigits = 20;
constexpr uint64_t kFracCount = 6;
constexpr uint64_t kZeroDigit = '0';
constexpr uint64_t kMinusByte = '-';
constexpr uint64_t kPointByte = '.';
constexpr uint64_t kIntIndefinite = 0x8000000000000000ULL;
constexpr int64_t kFloatScale = 1000000;

// The allocator's own numbers: the smallest effective alignment, the cap a
// refusal answers nil at, and the ceiling every byte of the run stays under.
constexpr int64_t kLeastBound = 16;
constexpr uint64_t kHeapCap = 1073741824ULL;
constexpr uint64_t kMemoryCap = 4294967296ULL;
// The low half of a word, which is what an f32 keeps in a register: the
// wider form's upper bytes are the ones the narrower width discards.
constexpr uint64_t kLowFourBytes = 0xFFFFFFFFULL;
// How many bytes an f32 keeps, and how many a wider form does.
constexpr uint64_t kFloat32Bytes = 4;
constexpr int64_t kZeroPointer = 0;
constexpr uint64_t kFrameLimit = 100000;

// The three ways a program dies on hardware, by the number the signal
// carries: an illegal divide or an emitted trap, a divide by zero or a
// quotient that does not fit, and a read or write outside its own memory.
// The interpreter plays the same deaths out so a harness leg sees the same
// signal the compiled program would die by.
constexpr int kSigFault = 8;
constexpr int kSigSegv = 11;
constexpr int kSigIll = 4;

// The first page is never mapped: a null pointer names address zero, and
// reading there must be the fault hardware would raise rather than a silent
// answer of whatever byte the frame happens to leave there.
constexpr uint64_t kNullGuard = 4096;

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

// One activation: a value-id map, the base of the region its allocas own,
// and the ids those allocas name. Ids are per-function, so none of it can be
// shared with a caller.
struct Frame {
    std::unordered_map<uint32_t, int64_t> slots;
    std::unordered_set<uint32_t> allocaIds;
    int64_t base = 0;
};

struct Runner {
    VmResult out;
    // The interpreter's own context, for the basic type a cast needs when it
    // answers a signedness question. It holds no program types.
    TypeContext ctx;
    std::vector<Frame> frames;
    std::vector<uint8_t> memory;
    // Link label to the address its bytes were laid down at, and link name
    // to the tag an indirect call resolves by.
    std::unordered_map<std::string, int64_t> labels;
    std::unordered_map<std::string, int64_t> functionTags;
    std::unordered_map<std::string, const Function*> functionsByName;
    std::unordered_map<int64_t, const Function*> functionsByTag;
    // Every host hand-out lives from here to the end of memory, and remembers
    // its size so realloc copies and free validates.
    int64_t heapAt = 0;
    std::unordered_map<int64_t, int64_t> heapBlocks;
    // A host hand exited the program: no further instruction may step, and
    // the answer is already in `out`.
    bool programDone = false;
    // Set by an instruction that can only answer through a fault, so the
    // run can end in a signal instead of a refusal.
    int faulted = 0;
    uint64_t steps = 0;

    bool refuse(Position pos, const std::string& message) {
        out.error = message;
        out.pos = pos;
        return false;
    }

    // The program died, the way the compiled one would: no diagnostic, a
    // signal number, and the run over. Only an instruction the interpreter
    // cannot execute refuses; a program it can run always ends in an exit or
    // a death.
    bool die(int signal) {
        out.ok = true;
        out.signal = signal;
        programDone = true;
        return false;
    }

    int64_t read(const Value& value) {
        auto found = frames.back().slots.find(value.id);
        if (found == frames.back().slots.end()) {
            return 0;
        }
        return found->second;
    }

    void write(const Value& result, int64_t bits) {
        frames.back().slots[result.id] = bits;
    }

    // The native frame covers every value id at rsp+id*8, and an alloca's
    // address names its own cell: the cell doubles as the storage. The
    // interpreter keeps the same cells in the frame's own region, so an
    // address is that region's base plus an id times eight.
    int64_t cellAddress(uint32_t id) {
        return frames.back().base +
               static_cast<int64_t>(id) * layout::kSizePointer;
    }

    // The address an instruction's base names: an alloca id is its own slot
    // storage, and any other id holds a computed address as a value. The
    // two never share an id, because ids restart per function and only an
    // alloca contributes a slot.
    int64_t baseAddress(const Value& base) {
        if (frames.back().allocaIds.find(base.id) !=
            frames.back().allocaIds.end()) {
            return cellAddress(base.id);
        }
        return read(base);
    }

    uint64_t loadBytes(int64_t address, uint64_t count, Position pos, bool& ok) {
        // Below the guard page, or past every byte laid down, is outside the
        // program's memory: hardware faults there, and so does the
        // interpreter, which reports the fault by the signal it records.
        if (address < static_cast<int64_t>(kNullGuard) || count > 8 ||
            static_cast<uint64_t>(address) + count > memory.size()) {
            ok = false;
            faulted = kSigSegv;
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
        if (address < static_cast<int64_t>(kNullGuard) || count > 8 ||
            static_cast<uint64_t>(address) + count > memory.size()) {
            return die(kSigSegv);
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

    // The double whose bits the word holds: a float value is its own bit
    // pattern everywhere a slot keeps it, the way a register would.
    double bitsToDouble(int64_t bits) {
        uint64_t raw = static_cast<uint64_t>(bits);
        double out = 0.0;
        std::memcpy(&out, &raw, sizeof(out));
        return out;
    }

    // The single precision the narrower width reads back, with the high
    // bits zeroed as a register's upper half would be.
    int64_t doubleToFloatBits(double value) {
        float narrowed = static_cast<float>(value);
        uint32_t raw = 0;
        std::memcpy(&raw, &narrowed, sizeof(raw));
        return static_cast<int64_t>(static_cast<uint64_t>(raw));
    }

    int64_t floatBitsToWord(IRType width, double value) {
        if (width == IRType::F32) {
            return doubleToFloatBits(value);
        }
        uint64_t raw = 0;
        std::memcpy(&raw, &value, sizeof(raw));
        return static_cast<int64_t>(raw);
    }

    // The value a float word holds at the width the instruction names.
    double wordToFloat(IRType width, int64_t bits) {
        if (width == IRType::F32) {
            uint32_t raw = static_cast<uint32_t>(static_cast<uint64_t>(bits) &
                                                kLowFourBytes);
            float out = 0.0F;
            std::memcpy(&out, &raw, sizeof(out));
            return static_cast<double>(out);
        }
        return bitsToDouble(bits);
    }

    // Mirrors codegen's emitLoadMem: exactly the width's bytes, extended to
    // 64 bits the same way (signed types sign-extend, the rest zero-extend;
    // I32 sign-extends through eax like movsxd, U32 zeroes like a dword mov).
    bool load(Value result, TypeRef type, IRType width, int64_t address,
              Position pos) {
        if (widthIsFloat(width)) {
            // The SSE loads move a float's own bytes into a register, so the
            // slot holds the bit pattern: an f32 keeps four bytes and an f64
            // eight, and a load reads exactly those back as the value.
            uint64_t count = width == IRType::F32 ? kFloat32Bytes
                                                  : layout::kSizePointer;
            bool ok = false;
            uint64_t bits = loadBytes(address, count, pos, ok);
            if (!ok) {
                return faulted != 0 ? die(faulted)
                                    : refuse(pos, "vm address out of range");
            }
            write(result, static_cast<int64_t>(bits));
            return true;
        }
        uint64_t count = widthBytes(width);
        if (count == 0) {
            return refuse(pos, "vm cannot load a void");
        }
        bool ok = false;
        uint64_t bits = loadBytes(address, count, pos, ok);
        if (!ok) {
            return faulted != 0 ? die(faulted)
                                : refuse(pos, "vm address out of range");
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
            // A store of a float writes the value's own bit pattern, at the
            // width the instruction names: the register's whole upper half
            // is what a later load reads back.
            uint64_t count = width == IRType::F32 ? kFloat32Bytes
                                                  : layout::kSizePointer;
            return storeBytes(address, count, static_cast<uint64_t>(bits), pos);
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
        bool floatPair = lhs.type.type != nullptr &&
                        lhs.type.type->kind() == Kind::Basic &&
                        basicIsFloat(*lhs.type.type->basic());
        if (floatPair) {
            // The compare spells the unsigned forms because ucomis's flags
            // are the ones they read, so a NaN fails every operator but
            // not-equals — which is what the C++ operators answer. The
            // pair's own width decides how the words are read: it is the
            // operand's, not the instruction's, which holds the answer.
            ir::IRType pairWidth =
                *lhs.type.type->basic() == BasicKind::F32 ? ir::IRType::F32
                                                         : ir::IRType::F64;
            double a = wordToFloat(pairWidth, read(lhs));
            double b = wordToFloat(pairWidth, read(rhs));
            bool answer = false;
            switch (instr.cmp) {
                case CmpOp::Eq:
                    answer = a == b;
                    break;
                case CmpOp::Ne:
                    answer = a != b;
                    break;
                // A float pair lowers to the unsigned spellings, because the
                // flags ucomis leaves are the ones they read: the ordering
                // is still IEEE ordered, and the parity guard makes a NaN
                // answer false everywhere but not-equals.
                case CmpOp::Lt:
                case CmpOp::Ult:
                    answer = a < b;
                    break;
                case CmpOp::Le:
                case CmpOp::Ule:
                    answer = a <= b;
                    break;
                case CmpOp::Gt:
                case CmpOp::Ugt:
                    answer = a > b;
                    break;
                case CmpOp::Ge:
                case CmpOp::Uge:
                    answer = a >= b;
                    break;
            }
            write(instr.results[0], answer ? 1 : 0);
            return true;
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
        if (widthIsFloat(instr.width)) {
            // A float rides the same opcodes as the integers, with its own
            // SSE form: the arithmetic answers at the instruction's width
            // and the answer is its own bit pattern.
            double a = wordToFloat(instr.width, read(instr.operands[0]));
            double b = wordToFloat(instr.width, read(instr.operands[1]));
            double answer = 0.0;
            switch (instr.op) {
                case Op::Add:
                    answer = a + b;
                    break;
                case Op::Sub:
                    answer = a - b;
                    break;
                case Op::Mul:
                    answer = a * b;
                    break;
                default:
                    return refuse(instr.loc, "vm met an unknown logic instruction");
            }
            write(instr.results[0], floatBitsToWord(instr.width, answer));
            return true;
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
        if (widthIsFloat(instr.width)) {
            // A float division never traps: a zero divisor answers an
            // infinity, and the remainder answers a NaN — the SSE forms do
            // not fault, so neither does the interpreter.
            double a = wordToFloat(instr.width, read(instr.operands[0]));
            double b = wordToFloat(instr.width, read(instr.operands[1]));
            double answer = instr.op == Op::Div ? a / b : std::fmod(a, b);
            write(instr.results[0], floatBitsToWord(instr.width, answer));
            return true;
        }
        int64_t a = extendToWidth(read(instr.operands[0]), instr.width, instr.type);
        int64_t b = extendToWidth(read(instr.operands[1]), instr.width, instr.type);
        // The integer divide faults: a zero divisor, and a signed widest
        // quotient divided by minus one, both take the hardware's divide
        // error, and the interpreter dies the same death rather than
        // answering a plausible value.
        if (b == 0) {
            return die(kSigFault);
        }
        bool wantQuotient = instr.op == Op::Div;
        int64_t answer = 0;
        if (ir::signedWidth(instr.width, instr.type)) {
            // The signed form's widest quotient is the width's most
            // negative value; dividing it by minus one leaves the field.
            int64_t least = extendToWidth(~uint64_t{0} << (narrowBits(instr.width) - 1),
                                          instr.width, instr.type);
            if (a == least && b == -1) {
                return die(kSigFault);
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

    // A hand exits the program from inside any call: the flag aborts every
    // step and the run answers what the helper answered.
    bool hostExit(int exitCode, const std::string& text) {
        out.ok = true;
        out.exitCode = exitCode;
        out.stderrText += text;
        programDone = true;
        return true;
    }

    bool readBytes(int64_t address, uint64_t count, std::vector<uint8_t>& into,
                   Position pos) {
        if (address < 0 || count > memory.size() ||
            static_cast<uint64_t>(address) > memory.size() - count) {
            refuse(pos, "vm address out of range");
            return false;
        }
        into.assign(memory.begin() + static_cast<size_t>(address),
                    memory.begin() + static_cast<size_t>(address + count));
        return true;
    }

    bool writeBytes(int64_t address, const std::vector<uint8_t>& bytes,
                    Position pos) {
        uint64_t captured = bytes.size();
        if (address < 0 || captured > memory.size() ||
            static_cast<uint64_t>(address) > memory.size() - captured) {
            return refuse(pos, "vm address out of range");
        }
        for (uint64_t i = kZeroCell; i < captured; ++i) {
            memory[static_cast<size_t>(address) + i] = bytes[i];
        }
        return true;
    }

    // Decimal into a caller's buffer, answering the byte count. The signed
    // form prints a leading minus and both work the magnitude as unsigned,
    // so the most negative value converts instead of wrapping.
    int64_t formatDigits(int64_t buffer, int64_t value, bool isSigned,
                         Position pos, bool& ok) {
        bool negative = isSigned && value < 0;
        uint64_t magnitude =
            negative ? ~static_cast<uint64_t>(value) + 1
                     : static_cast<uint64_t>(value);
        uint8_t reversed[kMaxDigits];
        uint64_t count = kZeroCell;
        do {
            reversed[count++] = static_cast<uint8_t>(kZeroDigit + magnitude % 10);
            magnitude /= 10;
        } while (magnitude != 0 && count < kMaxDigits);
        std::vector<uint8_t> digits;
        if (negative) {
            digits.push_back(static_cast<uint8_t>(kMinusByte));
        }
        while (count > kZeroCell) {
            digits.push_back(reversed[--count]);
        }
        ok = writeBytes(buffer, digits, pos);
        return static_cast<int64_t>(digits.size());
    }

    // A truncating double-to-i64 convert: an out-of-range value answers the
    // "integer indefinite" form instead of wrapping, which is what the
    // emitter's convert answers and what the digit sequence replays.
    int64_t truncToI64(double value) {
        if (!(value >= -9223372036854775808.0 && value < 9223372036854775808.0)) {
            return static_cast<int64_t>(kIntIndefinite);
        }
        return static_cast<int64_t>(value);
    }

    // printF64's shape rebuilt on the interpreter's own numbers: a sign when
    // the value is below zero and not NaN, the whole digits, a point, six
    // truncated fraction digits zero-padded on the left. Every step follows
    // the emitter's sequence — the parity-guarded compare, the truncating
    // converts, the six idiv digits — so edge values print what the helper
    // prints. Thirty-two bytes hold the longest shape.
    int64_t formatFloat(int64_t buffer, uint64_t bits, Position pos, bool& ok) {
        double value = 0;
        std::memcpy(&value, &bits, sizeof(double));
        bool isNan = std::isnan(value);
        bool negative = value < 0.0 && !isNan;
        double magnitude = negative ? 0.0 - value : value;
        std::vector<uint8_t> text;
        if (negative) {
            text.push_back(static_cast<uint8_t>(kMinusByte));
        }
        int64_t whole = truncToI64(magnitude);
        int64_t ahead = 0;
        bool wrote = false;
        int64_t count = formatDigits(buffer, whole, true, pos, wrote);
        if (!wrote) {
            ok = false;
            return 0;
        }
        ahead += count;
        text.push_back(static_cast<uint8_t>(kPointByte));
        int64_t scaled = truncToI64(
            (magnitude - static_cast<double>(whole)) * kFloatScale);
        uint8_t digits[kFracCount];
        for (int64_t i = kZeroCell; i < static_cast<int64_t>(kFracCount); ++i) {
            int64_t digit = scaled % 10;
            scaled /= 10;
            digits[kFracCount - 1 - static_cast<size_t>(i)] =
                static_cast<uint8_t>(kZeroDigit + (digit < 0 ? -digit : digit));
        }
        // The helper writes the six fraction digits in two moves; the bytes
        // that land in the buffer are the same six.
        for (uint64_t i = kZeroCell; i < kFracCount; ++i) {
            text.push_back(digits[i]);
        }
        if (!writeBytes(buffer + ahead, text, pos)) {
            ok = false;
            return 0;
        }
        ok = true;
        return ahead + static_cast<int64_t>(text.size());
    }

    // The heap answers the allocator's refusals as nil, its corruption as
    // exit 134, and every other block as zeroed bytes at an aligned address.
    int64_t hostAlloc(int64_t bytes, int64_t align, Position pos) {
        if (bytes < 0) {
            return 0;
        }
        if (align <= kZeroPointer) {
            return 0;
        }
        int64_t effective = align < kLeastBound ? kLeastBound : align;
        if (effective > kLeastBound && (effective & (effective - 1)) != 0) {
            return 0;
        }
        // Round the cursor up to the alignment, then answer a block that
        // starts there: the bump cursor never rewinds, so a hand-out stays
        // live for the rest of the run.
        int64_t slide = heapAt + effective - 1;
        if (slide < heapAt) {
            return 0;
        }
        slide &= ~(effective - 1);
        int64_t end = slide + bytes;
        if (end < slide || static_cast<uint64_t>(end) > kHeapCap) {
            return 0;
        }
        if (static_cast<uint64_t>(end) > memory.size()) {
            if (static_cast<uint64_t>(end) > kMemoryCap) {
                return 0;
            }
            memory.resize(static_cast<size_t>(end), kZeroCell);
        }
        heapBlocks[slide] = bytes;
        if (end > heapAt) {
            heapAt = end;
        }
        return slide;
    }

    bool hostFree(int64_t address, Position pos) {
        if (address == kZeroPointer) {
            return true;
        }
        auto found = heapBlocks.find(address);
        if (found == heapBlocks.end()) {
            return hostExit(134, "cider heap: corrupt block header\n");
        }
        heapBlocks.erase(found);
        return true;
    }

    bool hostRealloc(int64_t address, int64_t bytes, int64_t align, Position pos,
                     std::vector<Value>& results) {
        if (bytes < 0) {
            return true;
        }
        int64_t sized = hostAlloc(bytes, align, pos);
        if (address == kZeroPointer) {
            if (!results.empty()) {
                write(results[0], sized);
            }
            return true;
        }
        auto found = heapBlocks.find(address);
        if (found == heapBlocks.end()) {
            return hostExit(134, "cider heap: corrupt block header\n");
        }
        int64_t oldBytes = found->second;
        if (sized != kZeroPointer && oldBytes > kZeroPointer) {
            int64_t wanted = oldBytes < bytes ? oldBytes : bytes;
            std::vector<uint8_t> carried;
            if (readBytes(address, static_cast<uint64_t>(oldBytes), carried, pos)) {
                std::vector<uint8_t> keep(
                    carried.begin(),
                    carried.begin() + static_cast<size_t>(wanted));
                if (!writeBytes(sized, keep, pos)) {
                    return false;
                }
            }
        }
        if (!hostFree(address, pos)) {
            return false;
        }
        if (!results.empty()) {
            write(results[0], sized);
        }
        return true;
    }

    // A call to a body the module never laid down: the freestanding helpers
    // the program names but does not define. Each one answers what its
    // emitter does — bytes on the right stream, a count, a pointer, or the
    // exit code a panic dies with.
    bool hostCall(const std::string& name, const std::vector<int64_t>& args,
                  Position pos, std::vector<Value>& results) {
        if (name == "cider_print") {
            if (args.size() < kTwoWords) {
                return refuse(pos, "vm call passes too few words to print");
            }
            if (args[1] > 0) {
                std::vector<uint8_t> carried;
                if (!readBytes(args[0], static_cast<uint64_t>(args[1]), carried,
                               pos)) {
                    return false;
                }
                out.stdoutText.append(reinterpret_cast<const char*>(carried.data()),
                                      carried.size());
            }
            return true;
        }
        if (name == "putchar") {
            if (args.empty()) {
                return refuse(pos, "vm call passes too few words to putchar");
            }
            out.stdoutText.push_back(static_cast<char>(args[0] & kByteMask));
            return true;
        }
        if (name == "cider_itoa_signed" || name == "cider_itoa_unsigned") {
            if (args.size() < kTwoWords) {
                return refuse(pos, "vm call passes too few words to a converter");
            }
            bool wrote = false;
            int64_t count = formatDigits(args[0], args[1],
                                         name == "cider_itoa_signed", pos, wrote);
            if (!wrote) {
                return false;
            }
            if (!results.empty()) {
                write(results[0], count);
            }
            return true;
        }
        if (name == "cider_ftoa") {
            if (args.size() < kTwoWords) {
                return refuse(pos, "vm call passes too few words to a converter");
            }
            bool wrote = false;
            int64_t count = formatFloat(args[0], static_cast<uint64_t>(args[1]),
                                        pos, wrote);
            if (!wrote) {
                return false;
            }
            if (!results.empty()) {
                write(results[0], count);
            }
            return true;
        }
        if (name == "cider_panic") {
            if (args.size() < kTwoWords) {
                return refuse(pos, "vm call passes too few words to panic");
            }
            std::string text;
            if (args[1] > 0) {
                std::vector<uint8_t> carried;
                if (!readBytes(args[0], static_cast<uint64_t>(args[1]), carried,
                               pos)) {
                    return false;
                }
                text.append(reinterpret_cast<const char*>(carried.data()),
                            carried.size());
            }
            // A newline always follows, an empty message included.
            return hostExit(134, text + "\n");
        }
        if (name == "cider_heap_fail") {
            return hostExit(134, "cider heap: corrupt block header\n");
        }
        if (name == "cider_alloc") {
            if (args.size() < kTwoWords) {
                return refuse(pos, "vm call passes too few words to alloc");
            }
            int64_t answer = hostAlloc(args[0], args[1], pos);
            if (!results.empty()) {
                write(results[0], answer);
            }
            return true;
        }
        if (name == "cider_free") {
            if (args.empty()) {
                return refuse(pos, "vm call passes too few words to free");
            }
            return hostFree(args[0], pos);
        }
        if (name == "cider_realloc") {
            if (args.size() < kThreeWords) {
                return refuse(pos, "vm call passes too few words to realloc");
            }
            return hostRealloc(args[0], args[1], args[2], pos, results);
        }
        return refuse(pos, "vm cannot call '" + name + "' yet");
    }

    // A call runs a callee's body in a region of its own — ids restart at
    // zero per function — or hands off to a helper, whichever the name
    // answers. Arguments arrive in register order, results go back in it.
    bool call(const Instr& instr, std::vector<Value>& results) {
        std::vector<int64_t> args;
        for (const Argument& arg : instr.arguments) {
            for (const Value& word : arg.words) {
                args.push_back(read(word));
            }
        }
        const Function* target = nullptr;
        if (instr.call.indirect) {
            auto found = functionsByTag.find(read(instr.calleeValue));
            if (found == functionsByTag.end()) {
                return refuse(instr.loc, "vm cannot call this value");
            }
            target = found->second;
        } else {
            auto found = functionsByName.find(instr.callee);
            if (found != functionsByName.end()) {
                target = found->second;
            }
        }
        if (target != nullptr && !target->blocks.empty()) {
            if (args.size() != target->params.size()) {
                return refuse(instr.loc, "vm call arguments do not match the callee");
            }
            std::vector<int64_t> answer;
            if (!runFunction(*target, args, answer)) {
                return false;
            }
            if (programDone) {
                return false;
            }
            if (answer.size() != instr.results.size()) {
                return refuse(instr.loc, "vm call answers with the wrong width");
            }
            uint64_t answered = static_cast<uint64_t>(answer.size());
            for (uint64_t i = kZeroCell; i < answered; ++i) {
                write(instr.results[i], answer[i]);
            }
            return true;
        }
        std::string name = target != nullptr ? target->name : instr.callee;
        return hostCall(name, args, instr.loc, results);
    }

    // One activation of a function: a fresh region of cells zeroed, the
    // parameters bound in order, then the blocks walked from the entry.
    bool runFunction(const Function& fn, const std::vector<int64_t>& args,
                     std::vector<int64_t>& values) {
        if (frames.size() >= kFrameLimit) {
            return refuse(fn.loc, "vm call depth exhausted");
        }
        uint32_t top = topId(fn);
        uint64_t bytes = frameWords(top) * layout::kSizePointer;
        if (bytes > kMemoryCap || memory.size() + bytes > kMemoryCap) {
            return refuse(fn.loc, "vm memory exhausted");
        }
        Frame frame;
        frame.base = static_cast<int64_t>(memory.size());
        memory.resize(memory.size() + static_cast<size_t>(bytes), kZeroCell);
        // The emitter's prologue moves each argument word into its own slot,
        // where a load through the slot's id reads it back: the parameter's
        // value lives in the cell, not in a value register.
        uint64_t supplied = static_cast<uint64_t>(fn.params.size());
        uint64_t arrived = static_cast<uint64_t>(args.size());
        uint64_t bound = supplied < arrived ? supplied : arrived;
        for (uint64_t i = kZeroCell; i < bound; ++i) {
            if (!storeBytes(frame.base + static_cast<int64_t>(fn.params[i].id) *
                                            layout::kSizePointer,
                            layout::kSizePointer, static_cast<uint64_t>(args[i]),
                            fn.loc)) {
                return false;
            }
        }
        // Only an alloca names slot storage; every other id that reaches an
        // addressBase is a computed address, so the set decides the branch.
        for (const Block& block : fn.blocks) {
            for (const Instr& instr : block.body) {
                if (instr.op == Op::Alloca && !instr.results.empty()) {
                    frame.allocaIds.insert(instr.results[0].id);
                }
            }
        }
        frames.push_back(std::move(frame));
        // The frame leaves with this call however it leaves: a return, a
        // refusal, or a host exit all unwind the region.
        struct Pop {
            Runner& owner;
            ~Pop() { owner.frames.pop_back(); }
        } pop{*this};
        std::unordered_map<uint32_t, size_t> blockAt;
        for (const Block& block : fn.blocks) {
            blockAt[block.label] = blockAt.size();
        }
        // The lowerer emits the entry block first, so the front of the list
        // is the entry — the same assumption the emitter's _start
        // trampoline makes when it falls into the function's first block.
        size_t current = kZeroCell;
        for (;;) {
            if (++steps > kStepLimit) {
                refuse(fn.loc, "vm step limit exhausted");
                return false;
            }
            const Block& block = fn.blocks[current];
            for (const Instr& instr : block.body) {
                if (!step(instr)) {
                    return false;
                }
            }
            const Terminator& term = block.term;
            switch (term.term) {
                case Term::Ret: {
                    for (const Value& value : term.values) {
                        values.push_back(read(value));
                    }
                    return true;
                }
                case Term::Br: {
                    auto found = blockAt.find(term.target);
                    if (found == blockAt.end()) {
                        refuse(term.loc, "vm branch names no block");
                        return false;
                    }
                    current = found->second;
                    break;
                }
                case Term::CondBr: {
                    if (!term.cond.has_value()) {
                        refuse(term.loc, "vm branch tests nothing");
                        return false;
                    }
                    auto found = frames.back().slots.find(term.cond->id);
                    int64_t tested =
                        found == frames.back().slots.end() ? 0 : found->second;
                    uint32_t label = tested != 0 ? term.thenTarget : term.elseTarget;
                    auto at = blockAt.find(label);
                    if (at == blockAt.end()) {
                        refuse(term.loc, "vm branch names no block");
                        return false;
                    }
                    current = at->second;
                    break;
                }
                case Term::Switch:
                    // Nothing lowers a switch in this slice: the lowerer
                    // spells every branch as an if chain, so the form is a
                    // promise about IR the producer never emits. Refusing
                    // names it rather than pretending an empty table runs.
                    refuse(term.loc, "vm cannot run this terminator yet");
                    return false;
                case Term::Trap:
                case Term::Unreachable:
                    // A trap block, and a block provably unreached, both emit
                    // the interpreter's own death: the compiled program takes
                    // an illegal instruction, and so does this leg.
                    return die(kSigIll);
            }
        }
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
        // A float on either side rides the SSE conversions: a float to an
        // integer truncates toward zero, and out of range answers the
        // processor's indefinite value; an integer to a float converts the
        // sign-extended word, except a u64 whose magnitude the conversion
        // cannot hold, which halves first and doubles the answer; and a
        // float to a float widens or narrows by the target's own width.
        if (isFloatType(from) || isFloatType(to)) {
            auto floatWidthOf = [](TypeRef t) -> ir::IRType {
                return *t.type->basic() == BasicKind::F32 ? ir::IRType::F32
                                                         : ir::IRType::F64;
            };
            if (isFloatType(from) && isFloatType(to)) {
                double value = wordToFloat(floatWidthOf(from),
                                           read(instr.operands[0]));
                write(instr.results[0],
                      floatBitsToWord(floatWidthOf(to), value));
                return true;
            }
            if (isFloatType(from)) {
                double value = wordToFloat(floatWidthOf(from),
                                          read(instr.operands[0]));
                int64_t truncated = truncToI64(value);
                if (toPtr) {
                    write(instr.results[0], truncated);
                    return true;
                }
                BasicKind kind = to.type->kind() == Kind::Enum
                                    ? BasicKind::I32
                                    : *to.type->basic();
                write(instr.results[0], extendFor(kind, truncated));
                return true;
            }
            double value = 0.0;
            if (fromPtr) {
                value = static_cast<double>(
                    static_cast<uint64_t>(read(instr.operands[0])));
            } else {
                BasicKind kind = from.type->kind() == Kind::Enum
                                    ? BasicKind::I32
                                    : *from.type->basic();
                int64_t word = extendFor(kind, read(instr.operands[0]));
                if (kind == BasicKind::U64) {
                    uint64_t raw = static_cast<uint64_t>(word);
                    uint64_t halved = (raw >> 1) | (raw & 1);
                    value = static_cast<double>(halved) * 2.0;
                } else if (basicIsSigned(kind)) {
                    value = static_cast<double>(word);
                } else {
                    value = static_cast<double>(static_cast<uint64_t>(word));
                }
            }
            ir::IRType toWidth = isFloatType(to) ? floatWidthOf(to)
                                                 : ir::IRType::F64;
            write(instr.results[0], floatBitsToWord(toWidth, value));
            return true;
        }
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
                // The slot's address is its id times eight by construction,
                // and the emitter's prologue binds parameter ids before any
                // block runs: neither writes a value here. Only the frame
                // address matters, and a load or store through the slot
                // finds it because the frame sized a cell for the id.
                frames.back().allocaIds.insert(instr.results[0].id);
                return true;
            case Op::Gep: {
                // Base plus byte offset, plus a runtime index: a base that
                // is a slot holds its own cell address, and a base that is
                // a computed address holds that address instead, so both
                // arrive as a value and add the same way.
                int64_t base = baseAddress(instr.addressBase) + instr.displacement;
                if (!instr.operands.empty()) {
                    base += read(instr.operands[0]);
                }
                write(instr.results[0], base);
                return true;
            }
            case Op::Load: {
                int64_t base = instr.globalLabel.empty()
                                   ? baseAddress(instr.addressBase) + instr.displacement
                                   : labelAddress(instr, instr.globalLabel);
                return load(instr.results[0], instr.type, instr.width, base,
                            instr.loc);
            }
            case Op::Store: {
                IRType width = instr.storedValue.width;
                int64_t base = kZeroPointer;
                if (!instr.globalLabel.empty()) {
                    base = labelAddress(instr, instr.globalLabel);
                } else {
                    // The store's addressBase is the slot's own id when the
                    // target is storage, and a computed address when it is
                    // not: the same split the load makes.
                    base = baseAddress(instr.addressBase) + instr.displacement;
                }
                return store(instr.type, width, base, read(instr.storedValue),
                             instr.loc);
            }
            case Op::Cmp:
                return compare(instr);
            case Op::Cast:
                return cast(instr);
            case Op::DataAddr:
                return addressOf(instr);
            case Op::FnAddr:
                return tagOf(instr);
            case Op::Call: {
                std::vector<Value> mutableResults = instr.results;
                return call(instr, mutableResults);
            }
            case Op::FAdd:
            case Op::FSub:
            case Op::FMul:
            case Op::FDiv:
            case Op::FRem:
            case Op::Extract:
            case Op::Insert:
            case Op::Asm:
                return refuse(instr.loc,
                              std::string("vm cannot run ") + opName(instr.op) + " yet");
            case Op::Trap:
                // The one instruction whose only meaning is a death: the
                // emitter writes the same illegal instruction, so the
                // interpreter dies rather than answering a value.
                return die(kSigIll);
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
    // they are defined. The first page stays unmapped beneath it, so a null
    // pointer names nothing.
    uint32_t top = topId(*fn);
    uint64_t words = frameWords(top);
    uint64_t bytes = words * layout::kSizePointer;
    run.memory.assign(kNullGuard + bytes, kZeroCell);
    // A function link name names one address by tag: id zero is never a
    // function, so a tag of zero is the "no such function" answer a caller
    // cannot confuse with a real target.
    for (const Function& any : module.functions) {
        int64_t tag = static_cast<int64_t>(&any - module.functions.data()) + 1;
        run.functionTags[any.name] = tag;
        run.functionsByTag[tag] = &any;
        run.functionsByName[any.name] = &any;
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
    // Everything the helpers hand out starts past the last global, and the
    // frames a call pushes grow from there: neither region rewinds, so an
    // address stays live for the rest of the run.
    run.heapAt = static_cast<int64_t>(run.memory.size());
    std::vector<int64_t> noArgs;
    std::vector<int64_t> answered;
    if (!run.runFunction(*fn, noArgs, answered)) {
        return run.out;
    }
    if (run.programDone) {
        return run.out;
    }
    int64_t bits = answered.empty() ? 0 : answered[0];
    run.out.ok = true;
    run.out.exitCode = static_cast<int>(bits & kByteMask);
    return run.out;
}

}  // namespace vm
}  // namespace cider
