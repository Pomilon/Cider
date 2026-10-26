// vm.cpp — the v3 interpreter. See vm.h for the contract.

#include "vm/vm.h"

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <new>
#include <time.h>
#include <unistd.h>
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
constexpr uint64_t kIntIndefinite = 0x8000000000000000ULL;

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

// The x86-64 the asm blocks a Cider program writes may use: the sixteen
// general registers under their own names, the widths a narrow spelling
// writes, and the errno values the syscalls answer.
constexpr uint64_t kRegisterCount = 16;
constexpr uint64_t kOneByte = 1;
constexpr uint64_t kFourBytes = 4;
constexpr uint64_t kExitRegister = 60;
constexpr int64_t kErrFault = -14;
constexpr int64_t kErrInval = -22;
constexpr int64_t kErrNoMem = -12;
constexpr int64_t kErrBadFd = -9;
constexpr int64_t kErrNoSys = -38;
constexpr uint64_t kSysRead = 0;
constexpr uint64_t kSysWrite = 1;
constexpr uint64_t kSysOpen = 2;
constexpr uint64_t kSysClose = 3;
constexpr uint64_t kSysLseek = 8;
constexpr uint64_t kSysMmap = 9;
constexpr uint64_t kSysNanosleep = 35;
constexpr uint64_t kSysUnlink = 87;
constexpr uint64_t kSysClockGettime = 228;
constexpr uint64_t kExitGroup = 231;
// How many placeholders a block may name, which is the count the emitter
// stages into registers: a sixth has nowhere to go.
constexpr uint64_t kStagedLimit = 8;

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

// Host helpers have no IR body, so no function tag: an address of one
// answers a synthetic negative tag instead. Positives are index-plus-one
// into the module's functions, zero is no-such-function, so negatives
// cannot collide with either. The table is the hostCall list below.
int64_t hostTagFor(const std::string& name) {
    static const char* const hosts[] = {
        "cider_print",         // -1
        "putchar",             // -2
        "cider_panic",         // -3
    };
    static const int64_t count = 3;
    for (int64_t i = 0; i < count; ++i) {
        if (name == hosts[i]) {
            return -i - 1;
        }
    }
    return 0;
}

std::string hostNameForTag(int64_t tag) {
    static const char* const hosts[] = {
        "cider_print", "putchar", "cider_panic",
    };
    static const int64_t count = 3;
    if (tag >= 0) {
        return "";
    }
    int64_t at = -tag - 1;
    if (at >= count) {
        return "";
    }
    return hosts[at];
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
    // The shift back has to be arithmetic: shifting the unsigned word would
    // leave the pattern's own width (a -1 in four bytes answers 4294967295).
    return static_cast<int64_t>(static_cast<int64_t>(bits << shift) >> shift);
}

// One activation: a value-id map, the base of the region its allocas own,
// and the ids those allocas name. Ids are per-function, so none of it can be
// shared with a caller.
struct Frame {
    std::unordered_map<uint32_t, int64_t> slots;
    std::unordered_set<uint32_t> allocaIds;
    // The general registers, which outlive one asm block and die with the
    // function: a program writes a syscall as several blocks, and the
    // model has to let them see each other's writes.
    int64_t regs[kRegisterCount] = {};
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
    std::unordered_map<uint32_t, std::string> addrWords;
    std::unordered_map<std::string, int64_t> functionTags;
    std::unordered_map<std::string, const Function*> functionsByName;
    std::unordered_map<int64_t, const Function*> functionsByTag;
    // The bytes the pending input holds, and how far the program has read
    // into them: a read syscall consumes from here and stops at the end.
    std::string stdIn_;
    uint64_t stdInAt_ = 0;
    // Host files this run opened, by the descriptor the program sees:
    // zero through two stay the standard stream numbers, so real files
    // start at three, and closing drops the row.
    std::unordered_map<int64_t, int> openFiles_;
    int64_t nextFileFd_ = 3;
    // A host hand exited the program: no further instruction may step, and
    // the answer is already in `out`.
    bool programDone = false;
    // Set by an instruction that can only answer through a fault, so the
    // run can end in a signal instead of a refusal.
    int faulted = 0;
    uint64_t steps = 0;
    bool refuse(Position pos, const std::string& message) {
        out.ok = false;
        out.error = message;
        out.pos = pos;
        // A refusal ends the run where it stands: without the flag the
        // block loop walks on past the instruction that produced it, and
        // the next terminator reads the half-built frame.
        programDone = true;
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

    // A diagnosed trap is the language's own result: one exact stderr line
    // and exit 2, matching the compiled program. Silent reasons still die by
    // the signal their compiled form would take.
    bool trap(ir::TrapReason reason) {
        if (const char* message = ir::trapMessage(reason)) {
            out.ok = true;
            out.exitCode = 2;
            out.stderrText += message;
            programDone = true;
            return false;
        }
        return die(kSigIll);
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

    // A value that names slot storage writes its own cell, because that is
    // what the emitter's copy-back does; any other value is an ordinary
    // register-style write. An asm block's placeholder is the same kind of
    // name either way, so the split is the only question to ask.
    bool writeBack(const Value& value, int64_t bits, Position pos) {
        if (frames.back().allocaIds.find(value.id) !=
            frames.back().allocaIds.end()) {
            return storeBytes(cellAddress(value.id), layout::kSizePointer,
                              static_cast<uint64_t>(bits), pos);
        }
        write(value, bits);
        return true;
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
        // Below the guard page nothing is mapped, and hardware faults there;
        // so does the interpreter, which reports the fault by the signal it
        // records.
        if (address < static_cast<int64_t>(kNullGuard) || count > layout::kSizePointer) {
            ok = false;
            faulted = kSigSegv;
            return 0;
        }
        uint64_t end = static_cast<uint64_t>(address) + count;
        // Every legitimate address is laid down before use: frames size
        // all cells up front, mmap grows with zero-fill at hand-out,
        // globals are laid at load. Past the last laid-down byte there is
        // no mapping, so the access is out of bounds and faults the way
        // the machine would on an unmapped page.
        if (end > kMemoryCap || end > memory.size()) {
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
        if (address < static_cast<int64_t>(kNullGuard) ||
            count > layout::kSizePointer) {
            return die(kSigSegv);
        }
        uint64_t end = static_cast<uint64_t>(address) + count;
        // The same laid-down rule as a load: a store past the last
        // laid-down byte has no mapping and faults instead of growing the
        // address space from inside the program.
        if (end > kMemoryCap || end > memory.size()) {
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

    // The address a following append will land at: the byte offset the
    // padding step rounds up to. A label recorded this way names the first
    // byte of the next blob, which is what a load through the label reads.
    int64_t nextAppendAt() const {
        uint64_t remainder = memory.size() % layout::kSizePointer;
        if (remainder == kZeroCell) {
            return static_cast<int64_t>(memory.size());
        }
        return static_cast<int64_t>(memory.size() + layout::kSizePointer -
                                    remainder);
    }

    // True when the module holds a label's bytes: a rodata blob answers its
    // address, a global its own, and a vtable the tags of the functions its
    // entries name. Anything else names a label nothing laid down, which is
    // a lowering bug rather than a value.
    // --- The asm blocks -----------------------------------------------------

    // The sixteen general registers, named as the blocks spell them. A 32-
    // or 8-bit spelling writes its own width and zeroes the rest of the
    // register, which is what the hardware does, so a narrow move never
    // leaves a stale upper half behind.
    struct RegName {
        const char* full;
        const char* half;
        const char* byte;
    };
    static constexpr RegName kRegs[kRegisterCount] = {
        {"rax", "eax", "al"},   {"rcx", "ecx", "cl"},
        {"rdx", "edx", "dl"},   {"rbx", "ebx", "bl"},
        {"rsp", "esp", "spl"},  {"rbp", "ebp", "bpl"},
        {"rsi", "esi", "sil"},  {"rdi", "edi", "dil"},
        {"r8", "r8d", "r8b"},   {"r9", "r9d", "r9b"},
        {"r10", "r10d", "r10b"}, {"r11", "r11d", "r11b"},
        {"r12", "r12d", "r12b"}, {"r13", "r13d", "r13b"},
        {"r14", "r14d", "r14b"}, {"r15", "r15d", "r15b"},
    };

    // The register a spelling names, and the width it writes: zero at a
    // full register, four at the 32-bit spelling, one at the 8-bit one.
    int regIndex(const std::string& name, uint64_t& width) const {
        for (uint64_t i = 0; i < kRegisterCount; ++i) {
            if (name == kRegs[i].full) {
                width = kZeroCell;
                return static_cast<int>(i);
            }
            if (name == kRegs[i].half) {
                width = kFourBytes;
                return static_cast<int>(i);
            }
            if (name == kRegs[i].byte) {
                width = kOneByte;
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    // A placeholder's value as the emitter stages it: the slot's own cell
    // when the name is storage, the register-style word otherwise. A
    // parameter is bound into its cell, so reading the cell is the only way
    // to see the argument that arrived.
    int64_t stageValue(const Value& operand, Position pos) {
        if (frames.back().allocaIds.find(operand.id) !=
            frames.back().allocaIds.end()) {
            bool ok = false;
            uint64_t bits = loadBytes(cellAddress(operand.id),
                                      layout::kSizePointer, pos, ok);
            if (!ok) {
                return 0;
            }
            return static_cast<int64_t>(bits);
        }
        auto known = addrWords.find(operand.id);
        if (known != addrWords.end()) {
            auto at = labels.find(known->second);
            if (at == labels.end()) {
                return 0;
            }
            bool ok = false;
            uint64_t bits = loadBytes(at->second, layout::kSizePointer,
                                      pos, ok);
            if (!ok) {
                return 0;
            }
            return static_cast<int64_t>(bits);
        }
        return read(operand);
    }

    void regWrite(const std::string& name, int64_t* regs, int64_t bits) const {        uint64_t width = 0;
        int index = regIndex(name, width);
        if (index < 0) {
            return;
        }
        uint64_t masked = static_cast<uint64_t>(bits);
        if (width == kFourBytes) {
            masked &= kLowFourBytes;
        } else if (width == kOneByte) {
            masked &= kByteMask;
        }
        regs[index] = static_cast<int64_t>(masked);
    }

    // The mapping the kernel answers: a write to one of the two standard
    // streams lands in the run's own output, a buffer outside the program
    // memory is a fault, anything else is the errno the call would return.
    // A path in program memory runs to its NUL within a bounded scan: a
    // missing terminator is a fault, never a host read of runaway bytes.
    // The paths the tests use are short; the bound only stops the scan.
    static constexpr uint64_t kPathScanLimit = 4096;
    bool readPath(int64_t address, std::string& path) {
        path.clear();
        if (address < static_cast<int64_t>(kNullGuard)) {
            return false;
        }
        uint64_t at = static_cast<uint64_t>(address);
        for (uint64_t i = 0; i < kPathScanLimit; ++i) {
            if (at >= memory.size()) {
                return false;
            }
            char byte = static_cast<char>(memory[at]);
            if (byte == '\0') {
                return true;
            }
            path.push_back(byte);
            ++at;
        }
        return false;
    }

    int64_t syscallOpen(int64_t pathAddr, int64_t flags, int64_t mode) {
        std::string path;
        if (!readPath(pathAddr, path)) {
            return kErrFault;
        }
        int host = ::open(path.c_str(),
                           static_cast<int>(flags),
                           static_cast<unsigned int>(mode));
        if (host < 0) {
            return -static_cast<int64_t>(errno);
        }
        int64_t fd = nextFileFd_;
        ++nextFileFd_;
        openFiles_[fd] = host;
        return fd;
    }

    int64_t syscallClose(int64_t fd) {
        auto found = openFiles_.find(fd);
        if (found == openFiles_.end()) {
            return 0;
        }
        int host = found->second;
        openFiles_.erase(found);
        if (::close(host) != 0) {
            return -static_cast<int64_t>(errno);
        }
        return 0;
    }

    int64_t syscallLseek(int64_t fd, int64_t offset, int64_t whence) {
        auto found = openFiles_.find(fd);
        if (found == openFiles_.end()) {
            return kErrBadFd;
        }
        off_t at = ::lseek(found->second, static_cast<off_t>(offset),
                           static_cast<int>(whence));
        if (at == static_cast<off_t>(-1)) {
            return -static_cast<int64_t>(errno);
        }
        return static_cast<int64_t>(at);
    }

    int64_t syscallUnlink(int64_t pathAddr) {
        std::string path;
        if (!readPath(pathAddr, path)) {
            return kErrFault;
        }
        if (::unlink(path.c_str()) != 0) {
            return -static_cast<int64_t>(errno);
        }
        return 0;
    }

    int64_t syscallWrite(int64_t fd, int64_t buffer, int64_t count) {
        if (count == kZeroCell) {
            return 0;
        }
        if (buffer < static_cast<int64_t>(kNullGuard) || count < 0 ||
            static_cast<uint64_t>(buffer) + static_cast<uint64_t>(count) >
                memory.size()) {
            return kErrFault;
        }
        uint64_t begin = static_cast<uint64_t>(buffer);
        uint64_t span = static_cast<uint64_t>(count);
        if (fd == 1) {
            out.stdoutText.append(
                reinterpret_cast<const char*>(memory.data()) +
                    static_cast<long>(begin),
                span);
            return count;
        }
        if (fd == 2) {
            out.stderrText.append(
                reinterpret_cast<const char*>(memory.data()) +
                    static_cast<long>(begin),
                span);
            return count;
        }
        auto found = openFiles_.find(fd);
        if (found == openFiles_.end()) {
            return kErrBadFd;
        }
        ssize_t wrote = ::write(found->second,
                                memory.data() +
                                    static_cast<size_t>(begin),
                                span);
        if (wrote < 0) {
            return -static_cast<int64_t>(errno);
        }
        return static_cast<int64_t>(wrote);
    }

    // A read answers the bytes the pending input holds and moves the
    // cursor: a zero count reads nothing, a bad buffer faults, and the
    // stream ends by answering less than was asked.
    int64_t syscallRead(int64_t fd, int64_t buffer, int64_t count) {
        if (count == kZeroCell) {
            return 0;
        }
        if (fd != 0) {
            auto found = openFiles_.find(fd);
            if (found == openFiles_.end()) {
                return kErrBadFd;
            }
            if (buffer < static_cast<int64_t>(kNullGuard) || count < 0 ||
                static_cast<uint64_t>(buffer) +
                        static_cast<uint64_t>(count) >
                    memory.size()) {
                return kErrFault;
            }
            ssize_t got = ::read(found->second,
                                 memory.data() +
                                     static_cast<size_t>(buffer),
                                 static_cast<size_t>(count));
            if (got < 0) {
                return -static_cast<int64_t>(errno);
            }
            return static_cast<int64_t>(got);
        }
        uint64_t left = stdIn_.size() - stdInAt_;
        if (left == kZeroPointer) {
            // End of input answers zero, but only after the same buffer
            // validation the read below would run: the machine faults a
            // bad buffer before it ever reaches end-of-input.
            if (buffer < static_cast<int64_t>(kNullGuard) || count < 0 ||
                static_cast<uint64_t>(buffer) +
                        static_cast<uint64_t>(count) >
                    memory.size()) {
                return kErrFault;
            }
            return 0;
        }
        if (buffer < static_cast<int64_t>(kNullGuard) || count < 0 ||
            static_cast<uint64_t>(buffer) + static_cast<uint64_t>(count) >
                memory.size()) {
            return kErrFault;
        }
        uint64_t want = static_cast<uint64_t>(count);
        uint64_t take = want < left ? want : left;
        for (uint64_t i = 0; i < take; ++i) {
            memory[static_cast<size_t>(buffer) + i] =
                static_cast<uint8_t>(stdIn_[stdInAt_ + i]);
        }
        stdInAt_ += take;
        return static_cast<int64_t>(take);
    }

    // mmap answers a fresh region or the errno the kernel would: a zero
    // length is invalid, a length no address space would hold is out of
    // memory, and a real one is rounded to a word so an aligned reader
    // never straddles the region's end.
    int64_t syscallMmap(int64_t length) {
        if (length == kZeroCell) {
            return kErrInval;
        }
        if (length < 0 || static_cast<uint64_t>(length) > kMemoryCap ||
            memory.size() + static_cast<uint64_t>(length) > kMemoryCap) {
            return kErrNoMem;
        }
        uint64_t span = static_cast<uint64_t>(length);
        uint64_t slide = memory.size();
        if (slide % layout::kSizePointer != kZeroCell) {
            memory.insert(memory.end(),
                          layout::kSizePointer -
                              slide % layout::kSizePointer,
                          kZeroCell);
            slide = memory.size();
        }
        int64_t at = static_cast<int64_t>(slide);
        try {
            memory.insert(memory.end(), span, kZeroCell);
        } catch (const std::bad_alloc&) {
            // A mapping the host cannot back is the kernel's ENOMEM: the
            // guest's allocation operator sees a failed mmap and answers
            // nil, exactly as it would on a smaller address space.
            return kErrNoMem;
        }
        return at;
    }

    int64_t syscallClockGettime(int64_t clockId, int64_t addr, Position pos) {
        if (clockId != 1) {
            return kErrInval;
        }
        struct timespec ts;
        if (::clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
            return -errno;
        }
        if (!storeBytes(addr, layout::kSizePointer,
                        static_cast<uint64_t>(ts.tv_sec), pos) ||
            !storeBytes(addr + static_cast<int64_t>(layout::kSizePointer),
                        layout::kSizePointer,
                        static_cast<uint64_t>(ts.tv_nsec), pos)) {
            return kErrFault;
        }
        return 0;
    }

    int64_t syscallNanosleep(int64_t reqAddr, int64_t remAddr, Position pos) {
        bool ok = true;
        int64_t sec = static_cast<int64_t>(
            loadBytes(reqAddr, layout::kSizePointer, pos, ok));
        if (!ok) {
            return kErrFault;
        }
        int64_t nsec = static_cast<int64_t>(
            loadBytes(reqAddr + static_cast<int64_t>(layout::kSizePointer),
                      layout::kSizePointer, pos, ok));
        if (!ok) {
            return kErrFault;
        }
        if (sec < 0 || nsec < 0 || nsec >= 1000000000) {
            return kErrInval;
        }
        struct timespec req;
        req.tv_sec = static_cast<time_t>(sec);
        req.tv_nsec = static_cast<long>(nsec);
        struct timespec rem;
        if (::nanosleep(&req, &rem) != 0) {
            int err = errno;
            if (err == EINTR && remAddr >= static_cast<int64_t>(kNullGuard)) {
                storeBytes(remAddr, layout::kSizePointer,
                           static_cast<uint64_t>(rem.tv_sec), pos);
                storeBytes(remAddr + static_cast<int64_t>(layout::kSizePointer),
                           layout::kSizePointer,
                           static_cast<uint64_t>(rem.tv_nsec), pos);
            }
            return -err;
        }
        return 0;
    }

    void hostExit(int64_t code, const std::string& text) {
        out.ok = true;
        out.exitCode = static_cast<int>(code & kByteMask);
        out.stderrText += text;
        programDone = true;
    }

    static std::string trimSpaces(const std::string& line) {
        uint64_t begin = 0;
        uint64_t end = line.size();
        while (begin < end && (line[begin] == ' ' || line[begin] == '\t')) {
            ++begin;
        }
        while (end > begin && (line[end - 1] == ' ' || line[end - 1] == '\t')) {
            --end;
        }
        return line.substr(static_cast<std::string::size_type>(begin),
                           static_cast<std::string::size_type>(end - begin));
    }

    static bool parseSigned(const std::string& text, int64_t& out) {
        if (text.empty()) {
            return false;
        }
        bool negative = false;
        uint64_t at = 0;
        if (text[0] == '-') {
            negative = true;
            at = 1;
        }
        if (at >= text.size()) {
            return false;
        }
        uint64_t value = 0;
        for (; at < text.size(); ++at) {
            if (text[at] < '0' || text[at] > '9') {
                return false;
            }
            value = value * 10 + static_cast<uint64_t>(text[at] - '0');
        }
        uint64_t bits = negative ? (0 - value) : value;
        out = static_cast<int64_t>(bits);
        return true;
    }

    // A block is a closed set of moves and one syscall dispatcher over the
    // interpreter's own registers and memory. Anything outside it refuses
    // naming the line, because a block the interpreter half-ran would
    // answer differently from the compiled one.
    bool asmBlock(const Instr& instr) {
        if (instr.asmNames.size() > kStagedLimit) {
            return refuse(instr.loc,
                          "more placeholders than this block can stage");
        }
        uint64_t named = static_cast<uint64_t>(instr.asmNames.size());
        if (named > instr.operands.size()) {
            named = static_cast<uint64_t>(instr.operands.size());
        }
        // The register file belongs to the running function, not to one
        // block: a `mov rax, 60` in one statement and a `syscall` in the
        // next must see each other's writes, which is why a program writes
        // a syscall as several blocks in the first place.
        int64_t* regs = frames.back().regs;
        std::unordered_map<std::string, int64_t> held;
        std::string text = instr.asmText;
        for (uint64_t i = 0; i < named; ++i) {
            const std::string key = "{" + instr.asmNames[i] + "}";
            const std::string pseudo = "@" + std::to_string(i);
            // A function placeholder has an address word but no laid-down
            // label: the interpreter has no code address to hand out, so
            // staging it would answer silent zero. Data addresses always
            // hit: rodata and globals are all laid down, and DataAddr
            // refuses first — so a miss here names a function value.
            auto known = addrWords.find(instr.operands[i].id);
            if (known != addrWords.end() &&
                labels.find(known->second) == labels.end()) {
                return refuse(instr.loc,
                              "vm cannot stage a function value");
            }
            held[pseudo] = stageValue(instr.operands[i], instr.loc);
            auto at = std::string::size_type{};
            while ((at = text.find(key, at)) != std::string::npos) {
                text.replace(at, key.size(), pseudo);
                at += pseudo.size();
            }
        }
        // A staged operand address can fault (past everything laid down):
        // die on it the way loads do, instead of running the block on a
        // staged zero.
        if (faulted != 0) {
            return die(faulted);
        }
        auto lookup = [&](const std::string& name, int64_t& bits) {
            if (!name.empty() && name[0] == '@') {
                auto found = held.find(name);
                if (found == held.end()) {
                    return false;
                }
                bits = found->second;
                return true;
            }
            uint64_t width = 0;
            int index = regIndex(name, width);
            if (index < 0) {
                return false;
            }
            bits = regs[index];
            return true;
        };
        auto store = [&](const std::string& name, int64_t bits) {
            if (!name.empty() && name[0] == '@') {
                held[name] = bits;
                return;
            }
            regWrite(name, regs, bits);
        };
        uint64_t at = 0;
        while (at < text.size()) {
            size_t eol = text.find('\n', static_cast<size_t>(at));
            if (eol == std::string::npos) {
                eol = text.size();
            }
            std::string line = trimSpaces(text.substr(
                static_cast<size_t>(at), eol - static_cast<size_t>(at)));
            at = eol + 1;
            if (line.empty()) {
                continue;
            }
            if (line == "syscall") {
                int64_t result = 0;
                bool handled = true;
                switch (static_cast<uint64_t>(regs[0])) {
                    case kSysRead:
                        result = syscallRead(regs[7], regs[6], regs[2]);
                        break;
                    case kSysWrite:
                        result = syscallWrite(regs[7], regs[6], regs[2]);
                        break;
                    case kSysOpen:
                        result = syscallOpen(regs[7], regs[6], regs[2]);
                        break;
                    case kSysClose:
                        result = syscallClose(regs[7]);
                        break;
                    case kSysLseek:
                        result = syscallLseek(regs[7], regs[6], regs[2]);
                        break;
                    case kSysUnlink:
                        result = syscallUnlink(regs[7]);
                        break;
                    case kSysMmap:
                        result = syscallMmap(regs[6]);
                        break;
                    case kSysNanosleep:
                        result = syscallNanosleep(regs[7], regs[6], instr.loc);
                        break;
                    case kSysClockGettime:
                        result = syscallClockGettime(regs[7], regs[6], instr.loc);
                        break;
                    case kExitGroup:
                    case kExitRegister:
                        hostExit(regs[7], "");
                        return true;
                    default:
                        handled = false;
                        break;
                }
                if (!handled) {
                    return refuse(instr.loc, "vm cannot run syscall " +
                                                  std::to_string(regs[0]));
                }
                regs[0] = result;
                continue;
            }
            std::string::size_type comma = line.find(',');
            if (line.rfind("mov ", 0) == 0 && comma != std::string::npos) {
                std::string target = trimSpaces(line.substr(4, comma - 4));
                std::string source = trimSpaces(line.substr(comma + 1));
                int64_t bits = 0;
                if (lookup(source, bits) || parseSigned(source, bits)) {
                    store(target, bits);
                    continue;
                }
                return refuse(instr.loc, "vm cannot run this asm block: " + line);
            }
            if (line.rfind("xor ", 0) == 0 && comma != std::string::npos) {
                std::string target = trimSpaces(line.substr(4, comma - 4));
                std::string source = trimSpaces(line.substr(comma + 1));
                int64_t left = 0;
                int64_t right = 0;
                if (!lookup(target, left) || !lookup(source, right)) {
                    return refuse(instr.loc,
                                  "vm cannot run this asm block: " + line);
                }
                store(target, left ^ right);
                continue;
            }
            return refuse(instr.loc, "vm cannot run this asm block: " + line);
        }
        for (uint64_t i = 0; i < named; ++i) {
            const std::string pseudo = "@" + std::to_string(i);
            auto found = held.find(pseudo);
            if (found == held.end()) {
                continue;
            }
            if (!writeBack(instr.operands[i], found->second, instr.loc)) {
                return false;
            }
        }
        return true;
    }

    bool addressOf(const Instr& instr) {
        auto found = labels.find(instr.globalLabel);
        if (found == labels.end()) {
            return refuse(instr.loc, "vm cannot address '" + instr.globalLabel + "'");
        }
        write(instr.results[0], found->second);
        // A word the emitter could print as a memory reference: recorded so
        // an asm placeholder staged from it reads the label's image, the way
        // the compiled leg's memory operand does. Ids restart per function,
        // so only the newest write for an id counts.
        addrWords[instr.results[0].id] = instr.globalLabel;
        return true;
    }

    bool tagOf(const Instr& instr) {
        auto found = functionTags.find(instr.globalLabel);
        if (found != functionTags.end()) {
            write(instr.results[0], found->second);
        } else {
            // A host helper has no IR body and owns no function tag: its
            // address is the synthetic negative tag the indirect call
            // hands back to hostCall. Anything else still refuses loudly.
            int64_t host = hostTagFor(instr.globalLabel);
            if (host == 0) {
                return refuse(instr.loc, "vm cannot address '" + instr.globalLabel + "'");
            }
            write(instr.results[0], host);
        }
        // A function word stages like a data word downstream: record it so
        // an asm placeholder naming a function refuses in the staging loop
        // instead of reading the tag as an address. Function names never
        // land in labels, so the staging check tells the two apart.
        addrWords[instr.results[0].id] = instr.globalLabel;
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
            // A store that names no width writes the whole word: the
            // emitter's flat move puts all eight bytes at the address, and
            // the bytes beyond the aggregate's own size are the register's
            // upper half, matching what the compiled leg stores.
            count = layout::kSizePointer;
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
        uint64_t count = static_cast<uint64_t>(read(instr.operands[1])) &
                         ((uint64_t{1} << shiftMaskBits(instr.width)) - 1);
        uint64_t value = trimmedToWidth(read(instr.operands[0]), instr.width);
        uint64_t answer = 0;
        if (instr.op == Op::Shl) {
            // The mask has to come from trimmedToWidth: shifting a one by the
            // width's own bit count is undefined, and answers a zero mask on
            // every shape that asks for the whole word.
            uint64_t shifted = value << count;
            answer = trimmedToWidth(static_cast<int64_t>(shifted), instr.width);
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

    // A truncating double-to-i64 convert: an out-of-range value answers the
    // "integer indefinite" form instead of wrapping, which is what the
    // emitter's convert answers.
    int64_t truncToI64(double value) {
        if (!(value >= -9223372036854775808.0 && value < 9223372036854775808.0)) {
            return static_cast<int64_t>(kIntIndefinite);
        }
        return static_cast<int64_t>(value);
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
            int64_t written = args[0] & kByteMask;
            out.stdoutText.push_back(static_cast<char>(written));
            // The byte written is the answer, the way the C putchar
            // contract's own return names the character echoed.
            if (!results.empty()) {
                write(results[0], written);
            }
            return true;
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
            int64_t tag = read(instr.calleeValue);
            if (tag == kZeroPointer) {
                // A nil callee is the null function pointer: the machine
                // would enter address zero and fault, so the interpreter
                // dies by the same signal rather than refusing loudly.
                return die(kSigSegv);
            }
            auto found = functionsByTag.find(tag);
            if (found == functionsByTag.end()) {
                // A negative tag names a host helper, which has no IR
                // body: hand its words to hostCall the way a direct call
                // to the same name does.
                std::string host = hostNameForTag(tag);
                if (host.empty()) {
                    return refuse(instr.loc, "vm cannot call this value");
                }
                return hostCall(host, args, instr.loc, results);
            }
            target = found->second;
        } else {
            auto found = functionsByName.find(instr.callee);
            if (found != functionsByName.end()) {
                target = found->second;
            }
        }
        if (target != nullptr && !target->blocks.empty()) {
            // The callee binds one word per parameter. A caller may stage
            // MORE words than the callee names: a string literal arriving
            // where a pointer is declared carries its length alongside the
            // bytes, and the emitter leaves the extra word in a register
            // nobody reads. A shortfall is the mismatch, never a surplus.
            if (args.size() < target->params.size()) {
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
        addrWords.clear();
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
                if (programDone) {
                    // A host call inside the block ended the program: the
                    // remaining instructions of this block never run, and
                    // the run answers the exit the host named.
                    return false;
                }
                if (!step(instr)) {
                    return false;
                }
            }
            if (programDone) {
                // The last instruction of the block ended the program: its
                // terminator is nothing the program ever reaches.
                return false;
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
                    // A checked IR trap: diagnosed reasons report the
                    // language line and exit 2; silent ones keep the signal.
                    return trap(term.reason);
                case Term::Unreachable:
                    // Provably unreached: both legs die, but there is no
                    // source-level diagnostic to name.
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
        // The width's own register form: a signed width answers signed, so
        // the width itself carries the signedness and the extension follows
        // it. Deriving the width from the bit count would throw that away —
        // eight bits answer U8, whose extension answers unsigned — so the
        // signed and unsigned forms are listed by name instead.
        auto widthFor = [](BasicKind kind) {
            int bits = basicWidthBits(kind);
            if (!basicIsSigned(kind)) {
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
            }
            if (bits <= 8) {
                return ir::IRType::I8;
            }
            if (bits <= 16) {
                return ir::IRType::I16;
            }
            if (bits <= 32) {
                return ir::IRType::I32;
            }
            return ir::IRType::I64;
        };
        auto extendFor = [&](BasicKind kind, int64_t bits) {
            TypeRef named{ctx.basic(kind)};
            if (basicIsSigned(kind)) {
                return extendToWidth(bits, widthFor(kind), named);
            }
            return static_cast<int64_t>(trimmedToWidth(bits, widthFor(kind)));
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
                uint64_t raw = static_cast<uint64_t>(word);
                if (kind == BasicKind::U64 &&
                    static_cast<int64_t>(raw) < kZeroPointer) {
                    // Only a value whose high bit is set takes the halving
                    // sequence: the convert instruction reads the bit as a
                    // sign below, so the sequence halves the magnitude first
                    // and doubles the answer back.
                    uint64_t halved = (raw >> 1) | (raw & 1);
                    value = static_cast<double>(halved) * 2.0;
                } else if (basicIsSigned(kind)) {
                    value = static_cast<double>(word);
                } else {
                    value = static_cast<double>(raw);
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
                // The store names the width its own bytes occupy, and a
                // chunk's width is its own even when the value it carries is
                // wider: the value was read whole from the source's word, so
                // reading ITS width would write every byte of it.
                IRType width = instr.width;
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
                if (!call(instr, mutableResults)) {
                    return false;
                }
                return true;
            }
            case Op::FAdd:
            case Op::FSub:
            case Op::FMul:
            case Op::FDiv:
            case Op::FRem:
            case Op::Extract:
            case Op::Insert:
                return refuse(instr.loc,
                              std::string("vm cannot run ") + opName(instr.op) + " yet");
            case Op::Asm:
                // The interpreter runs the block itself, over its own
                // registers and memory, so a program that writes at the
                // machine needs no toolchain on either leg.
                return asmBlock(instr);
            case Op::Trap:
                // The one instruction whose only meaning is a death: a
                // diagnosed reason reports and exits 2, matching the
                // emitted write/exit sequence; the rest die by SIGILL.
                return trap(instr.reason);
        }
        return refuse(instr.loc, "vm reached an unknown instruction");
    }
};

}  // namespace

// The interpreter never lets an exception out. A growth reservation that
// the host cannot back is a loud refusal naming the out-of-memory the
// program would have met, so the harness sees a diagnostic and an exit
// status rather than a C++ throw escaping the IR loop.
static VmResult runProgram(const ir::Module& module,
                           const std::string& pendingInput) {
    Runner run;
    run.stdIn_ = pendingInput;
    // Lock in address space up front so later growth never relocates: a
    // growth that must relocate while the old span is still alive needs
    // twice the high-water mark transiently, which a capped host may not
    // have contiguously. Capacity is unobservable, and a reservation
    // commits nothing until touched, so no program-visible behavior
    // changes. Halve down while the host refuses; when nothing fits,
    // growth proceeds exactly as before.
    for (int down = 0; down <= 5; ++down) {
        try {
            run.memory.reserve(kMemoryCap >> down);
            break;
        } catch (const std::bad_alloc&) {
        }
    }
    // v4's entry is source-defined. `main` is an ordinary function; the
    // interpreter starts at `_start` exactly as the native link does.
    const Function* fn = nullptr;
    for (const Function& any : module.functions) {
        if (any.name == "_start") {
            fn = &any;
            break;
        }
    }
    if (fn == nullptr) {
        run.out.error = "no entry function: define func _start() -> void";
        return run.out;
    }
    if (!fn->signature.parameters().empty()) {
        run.refuse(fn->loc, "_start takes no parameters");
        return run.out;
    }
    const Type* ret = fn->signature.returnType();
    if (ret == nullptr || ret->kind() != Kind::Void) {
        run.refuse(fn->loc, "_start must return void");
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
        int64_t at = run.nextAppendAt();
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
            // A class value's vptr word names a vtable global's address,
            // not a number; resolve each relocation where the word sits.
            std::uint64_t reloc = 0;
            for (std::uint64_t index = 0;
                 index < global.initWords.size(); ++index) {
                if (reloc < global.relocWords.size() &&
                    global.relocWords[reloc].first == index) {
                    auto found =
                        run.labels.find(global.relocWords[reloc].second);
                    if (found == run.labels.end()) {
                        run.refuse(fn->loc, "vm cannot address '" +
                                                global.relocWords[reloc].second +
                                                "'");
                        return run.out;
                    }
                    run.appendWord(static_cast<uint64_t>(found->second));
                    ++reloc;
                } else {
                    run.appendWord(global.initWords[index]);
                }
            }
        } else if (!global.initLabel.empty()) {
            // A string global names its bytes: the address of them, then the
            // length, is the pair a string value reads. A closure global
            // names its code instead: the forwarder's tag beside empty
            // captures reads as the (code, caps) pair everywhere else.
            auto found = run.labels.find(global.initLabel);
            if (found != run.labels.end()) {
                run.appendWord(static_cast<uint64_t>(found->second));
            } else {
                auto code = run.functionTags.find(global.initLabel);
                if (code != run.functionTags.end()) {
                    run.appendWord(static_cast<uint64_t>(code->second));
                } else {
                    int64_t host = hostTagFor(global.initLabel);
                    if (host == 0) {
                        run.refuse(fn->loc, "vm cannot address '" + global.initLabel + "'");
                        return run.out;
                    }
                    run.appendWord(static_cast<uint64_t>(host));
                }
            }
            run.appendWord(static_cast<uint64_t>(global.initial));
        } else {
            run.appendWord(static_cast<uint64_t>(global.initial));
        }
        run.labels[global.label] = at;
    }
    // v4 has no automatic argument binding: `_start` takes no parameters.
    // A program that wants arguments reads them itself (initial stack, a
    // firmware table, or an explicit library).
    std::vector<int64_t> entryArgs;
    std::vector<int64_t> answered;
    if (!run.runFunction(*fn, entryArgs, answered)) {
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

VmResult run(const ir::Module& module, const std::string& pendingInput) {
    try {
        if (std::getenv("CIDER_FAULT_VM_ALLOC") != nullptr) {
            throw std::bad_alloc();
        }
        return runProgram(module, pendingInput);
    } catch (const std::bad_alloc&) {
        // A reservation the host cannot back is the program's out-of-memory
        // result: one exact stderr line and exit 2, the same shape the
        // compiler's OOM takes. The interpreter never lets the C++ throw
        // escape its IR loop.
        VmResult refused;
        refused.ok = true;
        refused.exitCode = 2;
        refused.stderrText = "vm error: out of memory\n";
        refused.pos = Position{"vm", kOneCell, kOneCell};
        return refused;
    } catch (const std::exception& e) {
        VmResult refused;
        refused.ok = false;
        refused.error = std::string("vm internal error: ") + e.what();
        refused.pos = Position{"vm", kOneCell, kOneCell};
        return refused;
    }
}

}  // namespace vm
}  // namespace cider
