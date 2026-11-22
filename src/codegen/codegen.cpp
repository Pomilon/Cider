// codegen.cpp — checked IR to x86-64 assembly (SysV; Linux/PomeOS services).
//
// Intel syntax with `.intel_syntax noprefix`, so plain `cc file.s` assembles
// it. The legacy emitter gives every value its own stack slot and fixed
// scratch registers carry values between slot and operation; the fast emitter
// keeps the same register homes but gives memory only to a value with no home,
// using the compact frame in frame.h. A global live-range scan (regalloc.cpp)
// homes eligible values across blocks, the original block-local scan homes
// values wholly inside one block, and everything else uses the slot path
// unchanged. rbx is the one callee-saved home; the prologue saves it in a
// frame cell and every return restores it.
//
// The text is a pure function of the module: no timestamps, no working
// directories, no absolute paths. Gate L5.

#include "codegen/codegen.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "codegen/frame.h"
#include "codegen/liveness.h"
#include "codegen/regalloc.h"
#include "layout.h"

namespace cider {
namespace codegen {

namespace {

// A float argument occupies exactly one word; named here so the call
// paths do not write the count beside a size query (T4).
constexpr uint64_t kFloatWord = kSingleFloatWord;
constexpr uint64_t kOneResult = 1;
// A binary instruction has exactly two value operands, and a value consumed
// once has exactly one use. Named so the fast selector does not write a digit
// beside a size query (T4).
constexpr uint64_t kBinaryOperands = 2;
constexpr uint32_t kSingleUse = 1;
// A count that has reached zero. Named so gate T4 does not read a digit beside
// a `.size()` query as a second size authority.
constexpr uint32_t kNoItems = 0;
// An IR field that names no value. The allocator and the liveness analysis
// use the same sentinel; codegen needs it to skip an empty parameter word.
constexpr uint32_t kInvalidValue = 0xFFFFFFFFu;

// The register row answers two words (rax, rdx): the count and the second
// position, named so neither line carries a digit (T4).
constexpr uint64_t kTwoResults = 2;
constexpr uint64_t kSecondResult = 1;

const char* argReg(uint64_t i) {
    if (i == 0) {
        return "rdi";
    }
    if (i == 1) {
        return "rsi";
    }
    if (i == 2) {
        return "rdx";
    }
    if (i == 3) {
        return "rcx";
    }
    if (i == 4) {
        return "r8";
    }
    return "r9";
}

// 8.4: the Microsoft x64 row. Integer words answer rcx, rdx, r8 and r9, and
// the caller reserves 32 bytes of shadow space above the return address, so
// the first word past the row sits at [rbp + 16 + 32 + (word - 4) * 8] in a
// callee that has pushed rbp.
const char* argRegMs(uint64_t i) {
    if (i == 0) {
        return "rcx";
    }
    if (i == 1) {
        return "rdx";
    }
    if (i == 2) {
        return "r8";
    }
    return "r9";
}

Register argRegister(uint64_t i) {
    switch (i) {
        case 0:
            return Register::Rdi;
        case 1:
            return Register::Rsi;
        case 2:
            return Register::Rdx;
        case 3:
            return Register::Rcx;
        case 4:
            return Register::R8;
        default:
            return Register::R9;
    }
}

Register argRegisterMs(uint64_t i) {
    switch (i) {
        case 0:
            return Register::Rcx;
        case 1:
            return Register::Rdx;
        case 2:
            return Register::R8;
        default:
            return Register::R9;
    }
}

constexpr uint64_t kSysVIntRegs = kSysVIntegerArgumentRegs;
constexpr uint64_t kMsabiIntRegs = kMsPositionalArgumentHomes;
constexpr uint64_t kSysVSseRegs = kSysVSseArgumentRegs;
constexpr uint64_t kMsabiSseRegs = kMsPositionalArgumentHomes;
constexpr uint64_t kShadowBytes = kMsShadowSpaceBytes;
constexpr uint64_t kWordBytes = kFrameWordBytes;
// A Microsoft x64 xmm6-xmm15 save owns all 128 bits: the scalar homes only
// read the low half, but the ABI promises the whole register to the caller.
constexpr uint32_t kXmmSaveBytes = 16;

// VARIADICS.md §7: an msabi callee's four integer homes sit in the caller's
// 32-byte shadow space at [rbp + 16 .. rbp + 40], and the first stack word is
// just above it at [rbp + 48]. Both the variadic prologue and the va_arg
// cursor read these offsets, so they are named once.
constexpr uint64_t kMsHomeBase = 2 * kWordBytes;
constexpr uint64_t kMsStackBase = kMsHomeBase + kShadowBytes;

// VARIADICS.md §6: a SysV variadic callee keeps the incoming argument
// registers in a 176-byte area. Six GPR slots sit at offsets 0..40, then two
// padding words, and eight 16-byte XMM slots at offsets 48..160. The slot
// counts and the XMM slot extent are named, so the save layout has one writer
// and `va_start`/`va_arg` read the same numbers the prologue wrote.
constexpr uint64_t kSysVVarargsGpBase = 0;
constexpr uint64_t kSysVVarargsFpBase = kSysVIntRegs * kWordBytes;
constexpr uint64_t kSysVVarargsGpLimit =
    kSysVVarargsGpBase + kSysVIntRegs * kWordBytes;
constexpr uint64_t kSysVVarargsXmmSlot = kVarargsXmmSlotBytes;
constexpr uint64_t kSysVVarargsFpLimit =
    kSysVVarargsFpBase + kSysVSseRegs * kSysVVarargsXmmSlot;
constexpr uint64_t kSysVVarargsSaveBytes = kVarargsSaveBytes;
constexpr uint64_t kSysVVarargsSaveCells =
    kSysVVarargsSaveBytes / kWordBytes;

bool calleeArg(std::string&, std::string&, const ir::Instr&) = delete;  // placeholder

// True when a global's image is entirely zero bytes and its exact extent is
// known. Such a global is storage, not data: the emitter can reserve it in
// `.bss` with one `.zero` instead of one `.quad 0` per eight bytes. Relocated
// words, vtables, strings and non-zero images all keep the `.data` path.
bool allZeroAggregate(const ir::Global& global) {
    if (global.initWords.empty() || !global.initLabel.empty() ||
        !global.initLabels.empty() || !global.relocWords.empty()) {
        return false;
    }
    if (global.sizeBytes == layout::kSizeUnknown ||
        global.alignment == layout::kAlignUnknown) {
        return false;
    }
    for (uint64_t word : global.initWords) {
        if (word != 0) {
            return false;
        }
    }
    return true;
}

const char* setFor(ir::CmpOp cmp) {
    if (cmp == ir::CmpOp::Eq) {
        return "sete";
    }
    if (cmp == ir::CmpOp::Ne) {
        return "setne";
    }
    if (cmp == ir::CmpOp::Lt) {
        return "setl";
    }
    if (cmp == ir::CmpOp::Ult) {
        return "setb";
    }
    if (cmp == ir::CmpOp::Le) {
        return "setle";
    }
    if (cmp == ir::CmpOp::Ule) {
        return "setbe";
    }
    if (cmp == ir::CmpOp::Gt) {
        return "setg";
    }
    if (cmp == ir::CmpOp::Ugt) {
        return "seta";
    }
    if (cmp == ir::CmpOp::Ge) {
        return "setge";
    }
    return "setae";
}

// The branch that a fused integer compare takes when the comparison is true.
// It is the jump twin of setFor: the same flag test, without materialising a
// boolean.
const char* jumpFor(ir::CmpOp cmp) {
    if (cmp == ir::CmpOp::Eq) {
        return "je";
    }
    if (cmp == ir::CmpOp::Ne) {
        return "jne";
    }
    if (cmp == ir::CmpOp::Lt) {
        return "jl";
    }
    if (cmp == ir::CmpOp::Ult) {
        return "jb";
    }
    if (cmp == ir::CmpOp::Le) {
        return "jle";
    }
    if (cmp == ir::CmpOp::Ule) {
        return "jbe";
    }
    if (cmp == ir::CmpOp::Gt) {
        return "jg";
    }
    if (cmp == ir::CmpOp::Ugt) {
        return "ja";
    }
    if (cmp == ir::CmpOp::Ge) {
        return "jge";
    }
    return "jae";
}

// The XMM spelling of an allocated float home. The table is enum-ordered so a
// home byte can be named without a second mapping.
const char* xmmName(uint8_t reg) {
    static const char* const kNames[] = {
        "none", "xmm0", "xmm1",  "xmm2",  "xmm3",  "xmm4",  "xmm5",
        "xmm6", "xmm7", "xmm8",  "xmm9",  "xmm10", "xmm11", "xmm12",
        "xmm13", "xmm14", "xmm15"};
    if (reg > static_cast<uint8_t>(XmmRegister::Xmm15)) {
        return "none";
    }
    return kNames[reg];
}

// The register name at a machine width. The fast binary path writes a value's
// allocated home directly, so it needs the home's 32/16/8-bit spelling for the
// narrowing step; the legacy emitter always spelled the intermediate as rax.
const char* regWidth(uint8_t reg, char size) {
    static const char* const kQ[] = {
        "none", "rax", "rcx", "rdx", "rbx", "rsi", "rdi", "r8",
        "r9",   "r10", "r11", "r12", "r13", "r14", "r15"};
    static const char* const kD[] = {
        "none", "eax", "ecx", "edx", "ebx", "esi", "edi", "r8d",
        "r9d",  "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"};
    static const char* const kW[] = {
        "none", "ax", "cx", "dx", "bx", "si", "di", "r8w",
        "r9w",  "r10w", "r11w", "r12w", "r13w", "r14w", "r15w"};
    static const char* const kB[] = {
        "none", "al", "cl", "dl", "bl", "sil", "dil", "r8b",
        "r9b",  "r10b", "r11b", "r12b", "r13b", "r14b", "r15b"};
    if (reg > static_cast<uint8_t>(Register::R15)) {
        return "none";
    }
    if (size == 'q') {
        return kQ[reg];
    }
    if (size == 'd') {
        return kD[reg];
    }
    if (size == 'w') {
        return kW[reg];
    }
    return kB[reg];
}

// ---------------------------------------------------------------------------
// Fast-path peepholes
//
// The fast emitter is native-only and writes the same instruction stream the
// legacy emitter does for a value without a home. Several requested peepholes
// are local and textual: they act on the generated block, never on the IR, so
// the legacy emitter cannot be affected by them (peepholeFastText is called
// only when fast_ is set). The passes are deliberately conservative:
//
//   * copy propagation and dead-move removal only rewrite 64-bit general
//     register moves, and a move is deleted only when its destination is
//     overwritten before the end of the local segment;
//   * memory forwarding tracks one exact 64-bit store address at a time and
//     drops the state on any other memory write, unknown instruction, barrier,
//     or write to the held register;
//   * a load folds into the next instruction only when that instruction can
//     encode a memory source and the load's destination is overwritten later
//     in the segment;
//   * a branch is inverted only for the canonical `jcc / jmp / target:` shape,
//     and an unconditional jump is removed only when its target is the next
//     instruction.
//
// Calls, inline asm, traps and volatile accesses are surrounded by barrier
// markers in the block loop below, so no pass crosses one. Every marker and
// synthetic boundary is removed from the final assembly.

const char* const kPeepholeBarrier = "# __cider_peephole_barrier__";
const char* const kPeepholeAsmBegin = "# __cider_peephole_asm_begin__";
const char* const kPeepholeAsmEnd = "# __cider_peephole_asm_end__";
// Fused compare branches are pinned so the fast peephole does not undo the
// fallthrough inversion; the direction may invert when the taken edge is the
// next block.
const char* const kPeepholePinBegin = "# __cider_peephole_pin_begin__";
const char* const kPeepholePinEnd = "# __cider_peephole_pin_end__";

constexpr uint64_t kPairOperands = 2;
constexpr uint64_t kTripleOperands = 3;
constexpr uint64_t kSingleOperand = 1;

enum class PLineKind { Code, Label, Directive, Protected, Boundary };

struct PLine {
    std::string text;
    PLineKind kind = PLineKind::Code;
    // A fused-compare branch: its direction is preserved, though a jump whose
    // target is the very next block may still fall through.
    bool pinned = false;
};

struct PInstr {
    bool valid = false;
    std::string mnemonic;
    std::vector<std::string> ops;
};

struct PEffects {
    bool known = false;
    bool writesMemory = false;
    std::unordered_set<std::string> reads;
    std::unordered_set<std::string> writes;
};

std::string pTrim(const std::string& value) {
    uint64_t begin = 0;
    uint64_t end = value.size();
    while (begin < end &&
           (value[begin] == ' ' || value[begin] == '\t' ||
            value[begin] == '\r')) {
        ++begin;
    }
    while (end > begin &&
           (value[end - 1] == ' ' || value[end - 1] == '\t' ||
            value[end - 1] == '\r')) {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::string pLower(const std::string& value) {
    std::string out = value;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

bool pStartsWith(const std::string& value, const char* prefix) {
    const uint64_t length = std::string(prefix).size();
    return value.size() >= length &&
           pLower(value.substr(0, length)) == pLower(prefix);
}

std::string pGpParent(const std::string& raw) {
    static const struct {
        const char* name;
        const char* parent;
    } kTable[] = {
        {"rax", "rax"},   {"eax", "rax"},   {"ax", "rax"},
        {"al", "rax"},    {"rcx", "rcx"},   {"ecx", "rcx"},
        {"cx", "rcx"},    {"cl", "rcx"},    {"rdx", "rdx"},
        {"edx", "rdx"},   {"dx", "rdx"},    {"dl", "rdx"},
        {"rbx", "rbx"},   {"ebx", "rbx"},   {"bx", "rbx"},
        {"bl", "rbx"},    {"rsi", "rsi"},   {"esi", "rsi"},
        {"si", "rsi"},    {"sil", "rsi"},   {"rdi", "rdi"},
        {"edi", "rdi"},   {"di", "rdi"},    {"dil", "rdi"},
        {"rbp", "rbp"},   {"ebp", "rbp"},   {"bp", "rbp"},
        {"bpl", "rbp"},   {"rsp", "rsp"},   {"esp", "rsp"},
        {"sp", "rsp"},    {"spl", "rsp"},   {"r8", "r8"},
        {"r8d", "r8"},    {"r8w", "r8"},    {"r8b", "r8"},
        {"r9", "r9"},     {"r9d", "r9"},    {"r9w", "r9"},
        {"r9b", "r9"},    {"r10", "r10"},   {"r10d", "r10"},
        {"r10w", "r10"},  {"r10b", "r10"},  {"r11", "r11"},
        {"r11d", "r11"},  {"r11w", "r11"},  {"r11b", "r11"},
        {"r12", "r12"},   {"r12d", "r12"},  {"r12w", "r12"},
        {"r12b", "r12"},  {"r13", "r13"},   {"r13d", "r13"},
        {"r13w", "r13"},  {"r13b", "r13"},  {"r14", "r14"},
        {"r14d", "r14"},  {"r14w", "r14"},  {"r14b", "r14"},
        {"r15", "r15"},   {"r15d", "r15"},  {"r15w", "r15"},
        {"r15b", "r15"},
    };
    const std::string name = pLower(raw);
    for (const auto& entry : kTable) {
        if (name == entry.name) {
            return entry.parent;
        }
    }
    return "";
}

bool pIsReg64(const std::string& raw) {
    static const char* const kNames[] = {
        "rax", "rcx", "rdx", "rbx", "rsi", "rdi", "rbp", "rsp",
        "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
    const std::string name = pLower(raw);
    for (const char* candidate : kNames) {
        if (name == candidate) {
            return true;
        }
    }
    return false;
}

bool pIsCopyableReg(const std::string& raw) {
    if (!pIsReg64(raw)) {
        return false;
    }
    const std::string name = pLower(raw);
    return name != "rsp" && name != "rbp";
}

void pAddTextRegs(const std::string& text,
                  std::unordered_set<std::string>& out) {
    std::string token;
    auto flush = [&]() {
        if (!token.empty()) {
            const std::string parent = pGpParent(token);
            if (!parent.empty()) {
                out.insert(parent);
            }
        }
        token.clear();
    };
    for (char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            token.push_back(c);
        } else {
            flush();
        }
    }
    flush();
}

std::vector<std::string> pSplitOperands(const std::string& text) {
    std::vector<std::string> ops;
    std::string current;
    uint32_t depth = 0;
    for (char c : text) {
        if (c == '[') {
            ++depth;
        } else if (c == ']' && depth > 0) {
            --depth;
        }
        if (c == ',' && depth == 0) {
            const std::string op = pTrim(current);
            if (!op.empty()) {
                ops.push_back(op);
            }
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    const std::string op = pTrim(current);
    if (!op.empty()) {
        ops.push_back(op);
    }
    return ops;
}

PInstr pParseInstr(const std::string& line) {
    PInstr instr;
    const std::string text = pTrim(line);
    if (text.empty() || text[0] == '.' || text[0] == '#') {
        return instr;
    }
    if (text.back() == ':') {
        return instr;
    }
    uint64_t split = 0;
    while (split < text.size() && text[split] != ' ' &&
           text[split] != '\t') {
        ++split;
    }
    instr.mnemonic = pLower(text.substr(0, split));
    if (instr.mnemonic.empty()) {
        return instr;
    }
    const std::string rest = pTrim(text.substr(split));
    instr.ops = pSplitOperands(rest);
    instr.valid = true;
    return instr;
}

bool pIsMemory(const std::string& op) {
    return op.find('[') != std::string::npos;
}

// A decimal or hexadecimal literal. The dead-home fold leaves only immediates
// in the shapes this pass removes; a symbol or register source is never one.
bool pIsImmediate(const std::string& op) {
    uint64_t at = 0;
    if (at < op.size() && (op[at] == '-' || op[at] == '+')) {
        ++at;
    }
    if (at >= op.size()) {
        return false;
    }
    if (op[at] == '0' && at + 1 < op.size() &&
        (op[at + 1] == 'x' || op[at + 1] == 'X')) {
        at += 2;
        if (at >= op.size()) {
            return false;
        }
        for (; at < op.size(); ++at) {
            if (!std::isxdigit(static_cast<unsigned char>(op[at]))) {
                return false;
            }
        }
        return true;
    }
    for (; at < op.size(); ++at) {
        if (!std::isdigit(static_cast<unsigned char>(op[at]))) {
            return false;
        }
    }
    return true;
}

bool pIsQwordMemory(const std::string& op) {
    if (pStartsWith(op, "qword ptr")) {
        return true;
    }
    return !op.empty() && op[0] == '[';
}

std::string pMemoryKey(const std::string& op) {
    std::string key = pTrim(op);
    if (pStartsWith(key, "qword ptr")) {
        key = pTrim(key.substr(std::string("qword ptr").size()));
    }
    return key;
}

// The flag the conditional branch must test after a fallthrough inversion.
const char* pInverseJump(const std::string& mnemonic) {
    static const struct {
        const char* from;
        const char* to;
    } kTable[] = {
        {"je", "jne"},   {"jne", "je"},   {"jz", "jnz"},
        {"jnz", "jz"},   {"jl", "jge"},   {"jge", "jl"},
        {"jle", "jg"},   {"jg", "jle"},   {"jb", "jae"},
        {"jae", "jb"},   {"jbe", "ja"},   {"ja", "jbe"},
        {"js", "jns"},   {"jns", "js"},   {"jo", "jno"},
        {"jno", "jo"},   {"jp", "jnp"},   {"jnp", "jp"},
    };
    for (const auto& entry : kTable) {
        if (mnemonic == entry.from) {
            return entry.to;
        }
    }
    return nullptr;
}

bool pIsJump(const std::string& mnemonic) {
    return mnemonic == "jmp" || pInverseJump(mnemonic) != nullptr;
}

// SSE instructions that touch only XMM registers (and, for a memory operand,
// the address registers). Conversions between GP and SSE registers are not
// here: `cvtsi2ss`/`cvtsi2sd` read a GP register and `cvttss2si`/`cvttsd2si`
// write one. An instruction not named here or by an explicit handler below is
// unknown, so every analysis resets instead of assuming no effect.
bool pIsPureSse(const std::string& mnemonic) {
    static const char* const kPureSse[] = {
        "addss",   "addsd",    "subss",    "subsd",    "mulss",
        "mulsd",   "divss",    "divsd",    "minss",    "minsd",
        "maxss",   "maxsd",    "sqrtss",   "sqrtsd",   "ucomiss",
        "ucomisd", "comiss",   "comisd",   "movss",    "movsd",
        "movaps",  "movapd",   "movups",   "movupd",   "xorps",
        "xorpd",   "andps",    "andpd",    "orps",     "orpd",
        "pxor",    "cvtss2sd", "cvtsd2ss", "cvtdq2ps", "cvtps2dq",
        "cvttps2dq", "cvtpd2ps", "cvtps2pd", "cvtdq2pd", "cvtpd2dq",
        "cvttpd2dq", "shufps", "shufpd",   "unpcklps", "unpcklpd",
        "unpckhps", "unpckhpd", "movlhps", "movhlps",  "roundss",
        "roundsd",
    };
    for (const char* candidate : kPureSse) {
        if (mnemonic == candidate) {
            return true;
        }
    }
    return false;
}

PEffects pEffects(const PInstr& instr) {
    PEffects effects;
    if (!instr.valid) {
        return effects;
    }
    const std::string& m = instr.mnemonic;
    const auto& ops = instr.ops;
    auto addReads = [&](uint64_t index) {
        if (index < ops.size()) {
            if (pGpParent(ops[index]).empty() || pIsMemory(ops[index])) {
                pAddTextRegs(ops[index], effects.reads);
            } else {
                effects.reads.insert(pGpParent(ops[index]));
            }
        }
    };
    auto addWrite = [&](uint64_t index) {
        if (index >= ops.size()) {
            return;
        }
        const std::string parent = pGpParent(ops[index]);
        if (!parent.empty() && !pIsMemory(ops[index])) {
            effects.writes.insert(parent);
        }
    };
    auto addMemoryAddressReads = [&](uint64_t index) {
        if (index < ops.size() && pIsMemory(ops[index])) {
            pAddTextRegs(ops[index], effects.reads);
        }
    };

    if (m == "mov" || m == "movabs") {
        if (ops.size() != kPairOperands) {
            return effects;
        }
        if (pIsMemory(ops[0])) {
            effects.writesMemory = true;
            addMemoryAddressReads(0);
        } else {
            addWrite(0);
        }
        addReads(1);
        effects.known = true;
        return effects;
    }
    if (m == "movsx" || m == "movzx" || m == "movsxd") {
        if (ops.size() != kPairOperands) {
            return effects;
        }
        addWrite(0);
        addReads(1);
        effects.known = true;
        return effects;
    }
    // movd/movq move between a general register and an XMM register. They are
    // not SSE-only: `movd eax, xmm1` defines rax and `movq xmm1, rax` reads
    // it, so both directions must join the GP liveness model.
    if (m == "movd" || m == "movq") {
        if (ops.size() != kPairOperands) {
            return effects;
        }
        if (pIsMemory(ops[0])) {
            effects.writesMemory = true;
            addMemoryAddressReads(0);
        } else {
            addWrite(0);
        }
        addReads(1);
        effects.known = true;
        return effects;
    }
    if (m == "lea") {
        if (ops.size() != kPairOperands) {
            return effects;
        }
        addWrite(0);
        addMemoryAddressReads(1);
        effects.known = true;
        return effects;
    }
    if (m == "add" || m == "sub" || m == "and" || m == "or" ||
        m == "xor") {
        if (ops.size() != kPairOperands) {
            return effects;
        }
        const bool sameRegister =
            !ops[0].empty() && ops[0] == ops[1] && pIsReg64(ops[0]);
        if (pIsMemory(ops[0])) {
            effects.writesMemory = true;
            addMemoryAddressReads(0);
        } else {
            addWrite(0);
            if (!sameRegister) {
                addReads(0);
            }
        }
        addReads(1);
        effects.known = true;
        return effects;
    }
    if (m == "imul" || m == "mul") {
        // The one-operand form is the implicit rax/rdx product (and `mul` has
        // no other form). The two- and three-operand `imul` forms name their
        // destination explicitly.
        if (ops.size() == kSingleOperand) {
            effects.writes.insert("rax");
            effects.writes.insert("rdx");
            effects.reads.insert("rax");
            addReads(0);
            effects.known = true;
            return effects;
        }
        if (m == "imul" && ops.size() == kPairOperands) {
            addWrite(0);
            addReads(0);
            addReads(1);
            effects.known = true;
            return effects;
        }
        if (m == "imul" && ops.size() == kTripleOperands) {
            addWrite(0);
            addReads(1);
            addReads(2);
            effects.known = true;
            return effects;
        }
        return effects;
    }
    if (m == "neg" || m == "not" || m == "inc" || m == "dec") {
        if (ops.size() != kSingleOperand) {
            return effects;
        }
        addWrite(0);
        addReads(0);
        effects.known = true;
        return effects;
    }
    if (m == "shl" || m == "shr" || m == "sar") {
        if (ops.size() != kPairOperands) {
            return effects;
        }
        if (pIsMemory(ops[0])) {
            // A memory destination is a read-modify-write: it changes memory
            // and reads the address registers, so forwarding must reset.
            effects.writesMemory = true;
            addMemoryAddressReads(0);
        } else {
            addWrite(0);
            addReads(0);
        }
        addReads(1);
        effects.known = true;
        return effects;
    }
    if (m == "cmp" || m == "test") {
        for (uint64_t i = 0; i < ops.size(); ++i) {
            addReads(i);
        }
        effects.known = true;
        return effects;
    }
    if (pStartsWith(m, "set")) {
        if (ops.size() != kSingleOperand) {
            return effects;
        }
        addWrite(0);
        effects.known = true;
        return effects;
    }
    if (m == "push") {
        if (ops.size() != kSingleOperand) {
            return effects;
        }
        addReads(0);
        effects.writes.insert("rsp");
        effects.writesMemory = true;
        effects.known = true;
        return effects;
    }
    if (m == "pop") {
        if (ops.size() != kSingleOperand) {
            return effects;
        }
        addWrite(0);
        effects.writes.insert("rsp");
        effects.reads.insert("rsp");
        effects.known = true;
        return effects;
    }
    if (m == "cqo" || m == "cdq") {
        effects.writes.insert("rdx");
        effects.reads.insert("rax");
        effects.known = true;
        return effects;
    }
    if (m == "idiv" || m == "div") {
        if (ops.size() != kSingleOperand) {
            return effects;
        }
        effects.writes.insert("rax");
        effects.writes.insert("rdx");
        effects.reads.insert("rax");
        effects.reads.insert("rdx");
        addReads(0);
        effects.known = true;
        return effects;
    }
    if (m == "leave") {
        effects.writes.insert("rsp");
        effects.writes.insert("rbp");
        effects.reads.insert("rbp");
        effects.known = true;
        return effects;
    }
    // GP <-> SSE conversions. The integer source/destination joins the GP
    // liveness model: omitting these reads/writes let copy propagation keep a
    // stale alias and dead-move removal delete the feeding move.
    if (m == "cvtsi2ss" || m == "cvtsi2sd") {
        if (ops.size() != kPairOperands) {
            return effects;
        }
        if (pIsMemory(ops[0])) {
            effects.writesMemory = true;
            addMemoryAddressReads(0);
        }
        addReads(1);  // GP source, or the address of a memory source.
        effects.known = true;
        return effects;
    }
    if (m == "cvttss2si" || m == "cvttsd2si") {
        if (ops.size() != kPairOperands) {
            return effects;
        }
        if (pIsMemory(ops[0])) {
            effects.writesMemory = true;
            addMemoryAddressReads(0);
        } else {
            addWrite(0);  // GP destination.
        }
        addReads(1);  // XMM source, or the address of a memory source.
        effects.known = true;
        return effects;
    }
    // Control flow and trap instructions. They are local-segment boundaries in
    // practice; modelling them as no register effect keeps the backward scan
    // from discarding a value the successor may read (the backward live set
    // starts with every register live).
    if (m == "ret" || m == "nop" || m == "ud2" || m == "int3" ||
        pIsJump(m)) {
        effects.known = true;
        return effects;
    }
    if (m == "syscall") {
        // A Linux syscall has fixed register arguments and clobbers (rax,
        // rcx, r11 and the argument registers). The trap emission and inline
        // asm are barrier-isolated, but an unrecognised effect model must
        // never make the analyses assume the instruction is free.
        return effects;  // known = false.
    }
    // Pure SSE instructions touch no general register. Their memory operand
    // still names address registers the backward scan must keep live.
    if (pIsPureSse(m)) {
        for (uint64_t i = 0; i < ops.size(); ++i) {
            if (pIsMemory(ops[i])) {
                if (i == 0) {
                    effects.writesMemory = true;
                }
                pAddTextRegs(ops[i], effects.reads);
            }
        }
        effects.known = true;
        return effects;
    }
    // Anything not explicitly modelled is unknown, so copy propagation,
    // memory forwarding and dead-move removal reset instead of guessing.
    return effects;
}

void pCopyPropagate(std::vector<PLine>& lines) {
    std::unordered_map<std::string, std::string> alias;
    auto resolve = [&](const std::string& reg) {
        std::string current = reg;
        for (uint32_t step = 0; step < kTripleOperands; ++step) {
            auto found = alias.find(current);
            if (found == alias.end()) {
                break;
            }
            current = found->second;
        }
        return current;
    };
    for (PLine& line : lines) {
        if (line.kind == PLineKind::Boundary ||
            line.kind == PLineKind::Label ||
            line.kind == PLineKind::Directive ||
            line.kind == PLineKind::Protected) {
            alias.clear();
            continue;
        }
        PInstr instr = pParseInstr(line.text);
        if (!instr.valid) {
            alias.clear();
            continue;
        }
        // Only exact general-register source operands are substituted. A
        // register inside an address expression is left alone: rewriting
        // `[rax]` to `[rcx]` is valid only while the copy is live, and the
        // conservative exact-register form is the one this pass proves.
        if (instr.mnemonic == "cmp" || instr.mnemonic == "test") {
            for (std::string& op : instr.ops) {
                if (pIsReg64(op)) {
                    op = resolve(pLower(op));
                }
            }
        } else {
            const uint64_t first = instr.ops.empty() ? 0 : 1;
            for (uint64_t i = first; i < instr.ops.size(); ++i) {
                if (pIsReg64(instr.ops[i])) {
                    instr.ops[i] = resolve(pLower(instr.ops[i]));
                }
            }
        }
        // Rebuild the line from the (possibly substituted) operands.
        std::string text = "  " + instr.mnemonic;
        for (uint64_t i = 0; i < instr.ops.size(); ++i) {
            text += i == 0 ? " " : ", ";
            text += instr.ops[i];
        }
        line.text = text;

        PEffects effects = pEffects(instr);
        if (!effects.known) {
            alias.clear();
            continue;
        }
        for (auto it = alias.begin(); it != alias.end();) {
            if (effects.writes.find(it->first) != effects.writes.end() ||
                effects.writes.find(it->second) != effects.writes.end()) {
                it = alias.erase(it);
            } else {
                ++it;
            }
        }
        if (instr.mnemonic == "mov" &&
            instr.ops.size() == kPairOperands &&
            pIsCopyableReg(instr.ops[0]) &&
            pIsCopyableReg(instr.ops[1]) &&
            pLower(instr.ops[0]) != pLower(instr.ops[1])) {
            alias[pLower(instr.ops[0])] = pLower(instr.ops[1]);
        }
    }
}

void pMemoryForward(std::vector<PLine>& lines) {
    bool valid = false;
    std::string address;
    std::string value;
    for (PLine& line : lines) {
        if (line.kind != PLineKind::Code) {
            valid = false;
            continue;
        }
        PInstr instr = pParseInstr(line.text);
        if (!instr.valid) {
            valid = false;
            continue;
        }
        const bool rightShape = instr.ops.size() == kPairOperands;
        const bool store =
            instr.mnemonic == "mov" && rightShape &&
            pIsMemory(instr.ops[0]);
        if (store) {
            if (pIsQwordMemory(instr.ops[0]) && pIsReg64(instr.ops[1])) {
                const std::string key = pMemoryKey(instr.ops[0]);
                if (valid && key == address && pLower(instr.ops[1]) == value) {
                    line.text.clear();
                } else {
                    valid = true;
                    address = key;
                    value = pLower(instr.ops[1]);
                }
            } else {
                valid = false;
            }
            continue;
        }
        const bool load =
            instr.mnemonic == "mov" && rightShape &&
            pIsReg64(instr.ops[0]) && pIsQwordMemory(instr.ops[1]);
        if (load) {
            const std::string key = pMemoryKey(instr.ops[1]);
            if (valid && key == address) {
                if (pLower(instr.ops[0]) == value) {
                    line.text.clear();
                } else {
                    line.text = "  mov " + instr.ops[0] + ", " + value;
                }
                continue;
            }
            valid = false;
            continue;
        }
        PEffects effects = pEffects(instr);
        if (!effects.known || effects.writesMemory) {
            valid = false;
            continue;
        }
        if (valid) {
            std::unordered_set<std::string> addressRegs;
            pAddTextRegs(address, addressRegs);
            bool touched = false;
            for (const std::string& written : effects.writes) {
                if (written == value ||
                    addressRegs.find(written) != addressRegs.end()) {
                    touched = true;
                }
            }
            if (touched) {
                valid = false;
            }
        }
    }
}

// True when `reg` is provably overwritten after `from` before any later read,
// within the same local segment. Reaching the segment end without an overwrite
// returns false: the value may be live out of the block.
bool pDeadAfter(const std::vector<PLine>& lines, uint64_t from, uint64_t end,
                const std::string& reg) {
    for (uint64_t i = from; i < end; ++i) {
        if (lines[i].kind != PLineKind::Code) {
            return false;
        }
        PInstr instr = pParseInstr(lines[i].text);
        if (!instr.valid) {
            return false;
        }
        PEffects effects = pEffects(instr);
        if (!effects.known) {
            return false;
        }
        if (effects.reads.find(reg) != effects.reads.end()) {
            return false;
        }
        if (effects.writes.find(reg) != effects.writes.end()) {
            return true;
        }
    }
    return false;
}

bool pFoldsIntoMemorySource(const std::string& mnemonic) {
    return mnemonic == "mov" || mnemonic == "add" || mnemonic == "sub" ||
           mnemonic == "and" || mnemonic == "or" || mnemonic == "xor" ||
           mnemonic == "cmp" || mnemonic == "imul";
}

void pMemoryFold(std::vector<PLine>& lines) {
    uint64_t segmentStart = 0;
    while (segmentStart < lines.size()) {
        uint64_t segmentEnd = segmentStart;
        while (segmentEnd < lines.size() &&
               lines[segmentEnd].kind == PLineKind::Code) {
            ++segmentEnd;
        }
        for (uint64_t i = segmentStart; i + 1 < segmentEnd; ++i) {
            PInstr load = pParseInstr(lines[i].text);
            if (!load.valid) {
                continue;
            }
            if (load.mnemonic != "mov" ||
                load.ops.size() != kPairOperands ||
                !pIsReg64(load.ops[0]) ||
                !pIsQwordMemory(load.ops[1])) {
                continue;
            }
            const std::string reg = pLower(load.ops[0]);
            PInstr next = pParseInstr(lines[i + 1].text);
            if (!next.valid || next.ops.size() != kPairOperands ||
                !pFoldsIntoMemorySource(next.mnemonic)) {
                continue;
            }
            if (pLower(next.ops[1]) != reg || pIsMemory(next.ops[0]) ||
                pLower(next.ops[0]) == reg) {
                continue;
            }
            // The load result must be dead after its single consumer.
            if (!pDeadAfter(lines, i + 2, segmentEnd, reg)) {
                continue;
            }
            next.ops[1] = load.ops[1];
            std::string text = "  " + next.mnemonic;
            for (uint64_t op = 0; op < next.ops.size(); ++op) {
                text += op == 0 ? " " : ", ";
                text += next.ops[op];
            }
            lines[i + 1].text = text;
            lines[i].text.clear();
        }
        segmentStart = segmentEnd + 1;
    }
}

void pDuplicateMoves(std::vector<PLine>& lines) {
    std::string previous;
    for (PLine& line : lines) {
        if (line.kind != PLineKind::Code) {
            previous.clear();
            continue;
        }
        PInstr instr = pParseInstr(line.text);
        if (!instr.valid) {
            previous.clear();
            continue;
        }
        if (instr.mnemonic != "mov" || instr.ops.size() != kPairOperands) {
            previous = line.text;
            continue;
        }
        if (pIsReg64(instr.ops[0]) &&
            pLower(instr.ops[0]) == pLower(instr.ops[1])) {
            line.text.clear();
            previous.clear();
            continue;
        }
        if (!previous.empty() && previous == line.text) {
            line.text.clear();
            continue;
        }
        // `mov A, B; mov B, A` is the second move writing B with B's own old
        // value, which the first move just copied into A.
        PInstr prior = pParseInstr(previous);
        if (prior.valid && prior.mnemonic == "mov" &&
            prior.ops.size() == kPairOperands &&
            pIsCopyableReg(instr.ops[0]) &&
            pIsCopyableReg(instr.ops[1]) &&
            pLower(prior.ops[0]) == pLower(instr.ops[1]) &&
            pLower(prior.ops[1]) == pLower(instr.ops[0])) {
            line.text.clear();
            continue;
        }
        previous = line.text;
    }
}

void pDeadMoves(std::vector<PLine>& lines) {
    static const char* const kEveryReg[] = {
        "rax", "rcx", "rdx", "rbx", "rsi", "rdi", "rbp", "rsp",
        "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
    uint64_t segmentStart = 0;
    while (segmentStart < lines.size()) {
        uint64_t segmentEnd = segmentStart;
        while (segmentEnd < lines.size() &&
               lines[segmentEnd].kind == PLineKind::Code) {
            ++segmentEnd;
        }
        std::unordered_set<std::string> live;
        for (const char* reg : kEveryReg) {
            live.insert(reg);
        }
        for (uint64_t at = segmentEnd; at > segmentStart; --at) {
            PLine& line = lines[at - 1];
            PInstr instr = pParseInstr(line.text);
            if (!instr.valid) {
                for (const char* reg : kEveryReg) {
                    live.insert(reg);
                }
                continue;
            }
            PEffects effects = pEffects(instr);
            if (!effects.known) {
                for (const char* reg : kEveryReg) {
                    live.insert(reg);
                }
                continue;
            }
            const bool deadCopy =
                instr.mnemonic == "mov" &&
                instr.ops.size() == kPairOperands &&
                pIsCopyableReg(instr.ops[0]) &&
                pIsCopyableReg(instr.ops[1]) &&
                pLower(instr.ops[0]) != pLower(instr.ops[1]) &&
                live.find(pLower(instr.ops[0])) == live.end();
            if (deadCopy) {
                line.text.clear();
                continue;
            }
            for (const std::string& written : effects.writes) {
                live.erase(written);
            }
            for (const std::string& read : effects.reads) {
                live.insert(read);
            }
        }
        segmentStart = segmentEnd + 1;
    }
}

void pCompact(std::vector<PLine>& lines) {
    std::vector<PLine> kept;
    kept.reserve(lines.size());
    for (PLine& line : lines) {
        if (line.kind == PLineKind::Boundary || line.text.empty()) {
            continue;
        }
        kept.push_back(std::move(line));
    }
    lines.swap(kept);
}

void pBranchCleanup(std::vector<PLine>& lines) {
    // A jump to the immediately following instruction is a fallthrough.
    for (uint64_t i = 0; i + 1 < lines.size();) {
        if (lines[i].kind != PLineKind::Code) {
            ++i;
            continue;
        }
        PInstr instr = pParseInstr(lines[i].text);
        if (instr.valid && instr.mnemonic == "jmp" &&
            instr.ops.size() == kSingleOperand &&
            lines[i + 1].kind == PLineKind::Label &&
            pTrim(lines[i + 1].text) == pTrim(instr.ops[0]) + ":") {
            lines.erase(lines.begin() + static_cast<long>(i));
            continue;
        }
        ++i;
    }
    // `jcc L1 / jmp L2 / L1:` -> `inverse-jcc L2`; the inverted branch's
    // fallthrough is the original L1 path.
    for (uint64_t i = 0; i + 2 < lines.size(); ++i) {
        if (lines[i].kind != PLineKind::Code ||
            lines[i + 1].kind != PLineKind::Code ||
            lines[i].pinned || lines[i + 1].pinned) {
            continue;
        }
        PInstr cond = pParseInstr(lines[i].text);
        if (!cond.valid || cond.ops.size() != kSingleOperand) {
            continue;
        }
        const char* inverse = pInverseJump(cond.mnemonic);
        if (inverse == nullptr) {
            continue;
        }
        PInstr skip = pParseInstr(lines[i + 1].text);
        if (!skip.valid || skip.mnemonic != "jmp" ||
            skip.ops.size() != kSingleOperand) {
            continue;
        }
        if (lines[i + 2].kind != PLineKind::Label ||
            pTrim(lines[i + 2].text) != pTrim(cond.ops[0]) + ":") {
            continue;
        }
        lines[i].text = "  " + std::string(inverse) + " " + skip.ops[0];
        lines.erase(lines.begin() + static_cast<long>(i + 1));
    }
}

void peepholeFastText(std::string& text) {
    std::vector<PLine> lines;
    bool inAsm = false;
    bool inPinned = false;
    bool boundary = false;
    std::string current;
    for (uint64_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            std::string line = current;
            current.clear();
            const std::string trimmed = pTrim(line);
            if (trimmed == kPeepholeAsmBegin) {
                inAsm = true;
                boundary = true;
                continue;
            }
            if (trimmed == kPeepholeAsmEnd) {
                inAsm = false;
                boundary = true;
                continue;
            }
            if (trimmed == kPeepholePinBegin) {
                inPinned = true;
                boundary = true;
                continue;
            }
            if (trimmed == kPeepholePinEnd) {
                inPinned = false;
                boundary = true;
                continue;
            }
            if (trimmed == kPeepholeBarrier) {
                boundary = true;
                continue;
            }
            if (inAsm) {
                lines.push_back(PLine{line, PLineKind::Protected});
                continue;
            }
            if (boundary) {
                lines.push_back(PLine{"", PLineKind::Boundary});
                boundary = false;
            }
            if (trimmed.empty()) {
                lines.push_back(PLine{"", PLineKind::Boundary});
                continue;
            }
            PLineKind kind = PLineKind::Code;
            if (trimmed.back() == ':') {
                kind = PLineKind::Label;
            } else if (trimmed[0] == '.') {
                kind = PLineKind::Directive;
            } else if (trimmed[0] == '#') {
                kind = PLineKind::Boundary;
            }
            PLine record{line, kind};
            record.pinned = inPinned;
            lines.push_back(record);
        } else {
            current.push_back(text[i]);
        }
    }
    if (boundary) {
        lines.push_back(PLine{"", PLineKind::Boundary});
    }

    pCopyPropagate(lines);
    pMemoryForward(lines);
    pCompact(lines);
    pMemoryFold(lines);
    pDuplicateMoves(lines);
    pCompact(lines);
    pDeadMoves(lines);
    pCompact(lines);
    pBranchCleanup(lines);

    std::string result;
    for (const PLine& line : lines) {
        if (line.kind == PLineKind::Boundary || line.text.empty()) {
            continue;
        }
        result += line.text;
        result += "\n";
    }
    text = std::move(result);
}

}  // namespace

class Emitter {
public:
    Emitter(const ir::Module& module, Target target, CodegenKind kind,
            bool requireEntry = true)
        : module_(module),
          target_(target),
          fast_(kind == CodegenKind::Fast),
          requireEntry_(requireEntry) {}

    EmitResult run() {
        out_ += ".intel_syntax noprefix\n.text\n";
        out_ += module_.moduleAsm;
        for (const ir::Function& fn : module_.functions) {
            if (!emitFunction(fn)) {
                return EmitResult{false, "", error_, pos_};
            }
        }
        // The validated entry point is the only process-level symbol the
        // emitter contributes. There is no helper archive and no hidden
        // runtime: every other symbol comes from the program or an explicit
        // library the source imported. A `--no-entry` module object is a
        // library fragment: it defines its own symbols and remains linkable,
        // but it is not asked to provide the process entry.
        if (requireEntry_ && !emitEntry()) {
            return EmitResult{false, "", error_, pos_};
        }
        if (!module_.globals.empty()) {
            const bool elfObject =
                target::info(target_).object == target::Object::Elf64;
            bool inData = false;
            bool inBss = false;
            for (const ir::Global& global : module_.globals) {
                if (allZeroAggregate(global)) {
                    // Zero storage belongs in `.bss`: one directive reserves
                    // the object's exact extent, so a large zero aggregate
                    // costs its size and not one `.quad 0` per eight bytes.
                    // Alignment is the type's natural alignment, placed
                    // before the label so the symbol itself is aligned.
                    if (!inBss) {
                        out_ += ".section .bss\n";
                        inBss = true;
                        inData = false;
                    }
                    if (global.weak) {
                        out_ += "  .weak " + global.label + "\n";
                    } else if (global.exportSymbol) {
                        out_ += "  .globl " + global.label + "\n";
                    }
                    out_ += "  .balign " +
                            std::to_string(global.alignment) + "\n";
                    if (elfObject) {
                        // ELF symbol metadata carries the extent the image
                        // path leaves implicit; COFF has no `.size`.
                        out_ += "  .type " + global.label + ", @object\n";
                    }
                    out_ += global.label + ":\n";
                    out_ += "  .zero " +
                            std::to_string(global.sizeBytes) + "\n";
                    if (elfObject) {
                        out_ += "  .size " + global.label + ", " +
                                std::to_string(global.sizeBytes) + "\n";
                    }
                    continue;
                }
                if (!inData) {
                    out_ += ".data\n";
                    inData = true;
                    inBss = false;
                }
                if (global.weak) {
                    out_ += "  .weak " + global.label + "\n";
                } else if (global.exportSymbol) {
                    out_ += "  .globl " + global.label + "\n";
                }
                out_ += global.label + ":\n";
                if (!global.initLabels.empty()) {
                    // A vtable: one link-time address per method, in
                    // interface method order, resolved by the linker.
                    for (const std::string& entry : global.initLabels) {
                        out_ += "  .quad " + entry + "\n";
                    }
                } else if (global.initLabel.empty()) {
                    if (global.initWords.empty()) {
                        out_ += "  .quad " + std::to_string(global.initial) + "\n";
                    } else {
                        // An aggregate global: one word per eight image
                        // bytes, so element stores land on image bytes.
                        // A class value's vptr word is a link-time label,
                        // so it substitutes the numeric word in place.
                        std::uint64_t reloc = 0;
                        for (std::uint64_t index = 0;
                             index < global.initWords.size(); ++index) {
                            if (reloc < global.relocWords.size() &&
                                global.relocWords[reloc].first == index) {
                                out_ += "  .quad " +
                                        global.relocWords[reloc].second + "\n";
                                ++reloc;
                            } else {
                                out_ += "  .quad " +
                                        std::to_string(
                                            global.initWords[index]) +
                                        "\n";
                            }
                        }
                    }
                } else {
                    // A string global is its bytes' address followed by its
                    // length — the same two words a literal lowers to.
                    out_ += "  .quad " + global.initLabel + "\n";
                    out_ += "  .quad " + std::to_string(global.initial) + "\n";
                }
            }
        }
        if (!module_.rodata.empty()) {
            // Bytes in first-seen order: the order is the lowering order,
            // which is deterministic in the source, so the bytes are too.
            out_ += ".section .rodata\n";
            for (const ir::Rodata& blob : module_.rodata) {
                out_ += blob.label + ":\n";
                if (!blob.bytes.empty()) {
                    std::string line = "  .byte ";
                    for (unsigned char byte : blob.bytes) {
                        line += std::to_string(byte) + ",";
                    }
                    line.back() = '\n';
                    out_ += line;
                }
            }
        }
        return EmitResult{true, out_, "", Position{"", 1, 1}};
    }

private:
    // A diagnosed trap is a language result, not a machine accident. Native
    // has no libc, so the line goes out with the Linux write syscall and the
    // exit status is 2. UEFI and PomeOS have no compiler-owned target service
    // for a diagnostic, and borrowing Linux's numbers would be a lie about
    // the ABI, so they trap silently. Undiagnosed reasons keep the same ud2.
    //
    // The sequence is written to `sink`: the legacy emitter passes `out_` and
    // keeps the body inline, while the fast emitter appends it to the cold
    // trap text after the function's blocks. One helper keeps the syscall,
    // message and .rodata bytes byte-identical on both paths.
    bool emitTrapBody(ir::TrapReason reason, std::string& sink) {
        if (target_ != Target::Native) {
            sink += "  ud2\n";
            return true;
        }
        const char* raw = ir::trapMessage(reason);
        if (raw == nullptr) {
            sink += "  ud2\n";
            return true;
        }
        std::string message(raw);
        std::string escaped;
        for (char c : message) {
            if (c == '\n') {
                escaped += "\\n";
            } else if (c == '"' || c == '\\') {
                escaped += '\\';
                escaped += c;
            } else {
                escaped += c;
            }
        }
        const std::string label =
            ".L_cider_trap_msg_" + std::to_string(trapMessageSeq_++);
        sink += "  mov eax, 1\n";
        sink += "  mov edi, 2\n";
        sink += "  lea rsi, [rip + " + label + "]\n";
        sink += "  mov edx, " + std::to_string(message.size()) + "\n";
        sink += "  syscall\n";
        sink += "  mov eax, 60\n";
        sink += "  mov edi, 2\n";
        sink += "  syscall\n";
        sink += "  ud2\n";
        sink += ".pushsection .rodata\n";
        sink += label + ":\n";
        sink += "  .ascii \"" + escaped + "\"\n";
        sink += ".popsection\n";
        return true;
    }

    // A trap reason is an unconditional transfer. The fast backend leaves only
    // the jump at the site and accumulates the body for the cold stubs after
    // every block; the legacy backend keeps the old inline sequence exactly.
    bool emitTrapReason(ir::TrapReason reason) {
        if (!fast_) {
            return emitTrapBody(reason, out_);
        }
        const std::string label = trapStubLabel();
        out_ += "  jmp " + label + "\n";
        coldTrapText_ += label + ":\n";
        return emitTrapBody(reason, coldTrapText_);
    }

    // The target's null policy is explicit: a target whose address zero is
    // not guaranteed unmapped gets a check before a computed dereference.
    // The same check serves UEFI and PomeOS; only the label names the target.
    void emitNullCheck(const std::string& reg) {
        const target::Info& info = target::info(target_);
        if (info.nullFaults) {
            return;
        }
        if (fast_) {
            // Trap on zero and fall through on non-null. The cold stub sits
            // after every block, so the hot path is only the test and branch.
            const std::string label = trapStubLabel();
            out_ += "  test " + reg + ", " + reg + "\n";
            out_ += "  jz " + label + "\n";
            coldTrapText_ += label + ":\n  ud2\n";
            return;
        }
        const std::string ok = ".L_" + std::string(info.name) + "_null_" +
                               std::to_string(nullSeq_++);
        out_ += "  test " + reg + ", " + reg + "\n";
        out_ += "  jnz " + ok + "\n";
        out_ += "  ud2\n";
        out_ += ok + ":\n";
    }

    // The process entry. v4 has no startup shim: the program defines
    // `func _start() -> void` itself and the kernel enters there. PomeOS also
    // enters with argc in rdi and argv in rsi, so it may declare the reference
    // signature; both forms are ordinary SysV parameters. The compiler only
    // validates the source signature; it emits no trampoline, no automatic
    // main call, and no automatic exit.
    bool emitEntry() {
        if (target_ == Target::Uefi) {
            return emitUefiEntry();
        }
        const bool pomeos = target_ == Target::Pomeos;
        const ir::Function* startFn = nullptr;
        for (const ir::Function& fn : module_.functions) {
            if (fn.name == "_start") {
                startFn = &fn;
                break;
            }
        }
        if (startFn == nullptr) {
            refuse(Position{"", 1, 1},
                   "no entry function: define func _start() -> void");
            return false;
        }
        const Signature& sig = startFn->signature;
        if (!sig.parameters().empty()) {
            if (!pomeos) {
                refuse(startFn->loc, "_start takes no parameters");
                return false;
            }
            if (sig.arity() != 2) {
                refuse(startFn->loc,
                       "_start takes no parameters or (argc: int, argv: **char)");
                return false;
            }
            const Type* argcType = sig.parameters()[0].type;
            if (argcType == nullptr || argcType->kind() != Kind::Basic ||
                *argcType->basic() != BasicKind::I32) {
                refuse(startFn->loc, "_start argc parameter must be int");
                return false;
            }
            const Type* argvType = sig.parameters()[1].type;
            const Type* pointer = argvType != nullptr &&
                                          argvType->kind() == Kind::Pointer
                                      ? argvType->pointee()
                                      : nullptr;
            const Type* character = pointer != nullptr &&
                                            pointer->kind() == Kind::Pointer
                                        ? pointer->pointee()
                                        : nullptr;
            if (character == nullptr || character->kind() != Kind::Basic ||
                *character->basic() != BasicKind::Char) {
                refuse(startFn->loc, "_start argv parameter must be **char");
                return false;
            }
        }
        const Type* ret = sig.returnType();
        if (ret == nullptr || ret->kind() != Kind::Void) {
            refuse(startFn->loc, "_start must return void");
            return false;
        }
        if (sig.abi() != Abi::SysV) {
            refuse(startFn->loc, "_start must use the sysv calling convention");
            return false;
        }
        return true;
    }

    // The firmware entry. v4 has no shim: the program defines the real
    // Microsoft x64 entry itself and the firmware calls it directly. The
    // compiler validates the shape and emits no symbol of its own, stores
    // no handles, and never renames the source function.
    bool emitUefiEntry() {
        const ir::Function* user = nullptr;
        for (const ir::Function& fn : module_.functions) {
            if (fn.name == "efi_main") {
                user = &fn;
                break;
            }
        }
        if (user == nullptr) {
            refuse(Position{"", 1, 1},
                   "no entry function for uefi: define msabi func "
                   "efi_main(image_handle: *void, system_table: *void) -> int");
            return false;
        }
        const Signature& sig = user->signature;
        if (sig.abi() != Abi::MsAbi) {
            refuse(user->loc,
                   "efi_main must use the msabi calling convention");
            return false;
        }
        if (sig.arity() != 2) {
            refuse(user->loc,
                   "efi_main takes exactly two parameters: "
                   "image_handle and system_table");
            return false;
        }
        for (uint64_t i = 0; i < sig.arity(); ++i) {
            const Type* type = sig.parameters()[i].type;
            if (type == nullptr || type->kind() != Kind::Pointer ||
                type->pointee()->kind() != Kind::Void) {
                refuse(user->loc,
                       "efi_main parameter " + std::to_string(i + 1) +
                           " must be *void");
                return false;
            }
        }
        const Type* ret = sig.returnType();
        if (ret == nullptr || ret->kind() != Kind::Basic ||
            *ret->basic() != BasicKind::I32) {
            refuse(user->loc, "efi_main must return int");
            return false;
        }
        return true;
    }

    void refuse(Position pos, const std::string& what) {
        if (failed_) {
            return;
        }
        failed_ = true;
        error_ = what;
        pos_ = pos;
    }

    // One slot per value id, eight bytes each. A slot holds a value extended
    // to 64 bits on store (sign or zero per the type), and every load reads
    // exactly the bytes its type names — so a narrow field never reads
    // neighbor bytes, which is the defect v2's spill slots had. The legacy
    // backend keeps the id*8 formula; the fast backend uses the compact
    // frame's per-id spill offset.
    std::string slot(uint32_t id) {
        return "[rsp + " + std::to_string(slotOffset(id)) + "]";
    }

    // The byte offset of a value's cell. A fast frame answers the compact
    // spill offset; an alloca id falls back to its cell so a stray value read
    // still names valid storage. A missing entry marks the emission failed
    // rather than silently naming cell zero.
    uint32_t slotOffset(uint32_t id) {
        if (!fast_) {
            return id * static_cast<uint32_t>(layout::kSizePointer);
        }
        if (id < frame_.spillOffset.size() &&
            frame_.spillOffset[id] != kNoFrameSlot) {
            return frame_.spillOffset[id];
        }
        if (id < frame_.allocaOffset.size() &&
            frame_.allocaOffset[id] != kNoFrameSlot) {
            return frame_.allocaOffset[id];
        }
        slotMissing_ = true;
        return 0;
    }

    // A slot plus a temporary depth (inline asm pushes move rsp). The legacy
    // spelling is exactly id*8 + depth; the fast spelling is compact.
    std::string slotPlus(uint32_t id, uint32_t extra) {
        return "[rsp + " + std::to_string(slotOffset(id) + extra) + "]";
    }

    // The base of an alloca cell, without brackets. The legacy frame gives
    // every id a cell; the fast frame names the separate alloca region.
    std::string allocaBase(uint32_t id) {
        if (!fast_) {
            return "rsp + " +
                   std::to_string(id *
                                  static_cast<uint32_t>(layout::kSizePointer));
        }
        if (id < frame_.allocaOffset.size() &&
            frame_.allocaOffset[id] != kNoFrameSlot) {
            return "rsp + " + std::to_string(frame_.allocaOffset[id]);
        }
        slotMissing_ = true;
        return "rsp + 0";
    }

    // An XMM callee-save cell in the fast frame. These cells sit above the
    // frame layout and exist only on Microsoft x64 when a home uses one. A
    // nonvolatile XMM register is 16 bytes wide, so each cell owns a full
    // XMM word and the prologue/epilogue move it with `movdqu`.
    std::string xmmSaveSlotRef(uint32_t index) const {
        return "[rsp + " + std::to_string(xmmSaveBase_ +
                                          index * kXmmSaveBytes) +
               "]";
    }

    // A callee-save cell. The legacy frame numbers cells; the fast frame uses
    // byte offsets from rsp.
    std::string saveSlotRef(uint32_t index) {
        if (fast_) {
            return "[rsp + " +
                   std::to_string(calleeSaveBase_ +
                                  index * static_cast<uint32_t>(
                                              layout::kSizePointer)) +
                   "]";
        }
        return "[rsp + " +
               std::to_string((calleeSaveBase_ + index) *
                              static_cast<uint32_t>(layout::kSizePointer)) +
               "]";
    }

    // The register currently carrying a value in this block, or none. The
    // allocator's map is per block; outside the block loop (the prologue,
    // which writes parameter slots) it is null and every value is in memory.
    uint8_t homeOf(uint32_t id) const {
        if (currentHome_ == nullptr || id >= currentHome_->home.size()) {
            return static_cast<uint8_t>(Register::None);
        }
        return currentHome_->home[id];
    }

    // True when `id` is a constant whose home write the current consumer
    // folds into an immediate operand. The fold is recorded by the Const/Cast
    // lookahead under the consumer's position, so a constant that is still
    // live at an earlier instruction keeps its home until that consumer.
    bool foldedAtCurrent(uint32_t id) const {
        auto found = foldedAtPos_.find(currentPos_);
        return found != foldedAtPos_.end() &&
               found->second.find(id) != found->second.end();
    }

    // The home a value reads as at the current instruction. A constant folded
    // into this consumer has no register: every selector sees it through the
    // rematerialised immediate instead of a move the emitter elided.
    uint8_t effectiveHome(uint32_t id) const {
        if (foldedAtCurrent(id)) {
            return static_cast<uint8_t>(Register::None);
        }
        return homeOf(id);
    }

    std::string regRef(uint8_t reg) const {
        return registerName(static_cast<Register>(reg));
    }

    // The XMM home of a float value in the current block, or None. Kept apart
    // from the GPR table: a float never receives a GPR home, and a GPR value
    // never appears here.
    uint8_t floatHomeOf(uint32_t id) const {
        if (currentFloatHome_ == nullptr ||
            id >= currentFloatHome_->size()) {
            return static_cast<uint8_t>(XmmRegister::None);
        }
        return (*currentFloatHome_)[id];
    }

    std::string xmmRef(uint8_t reg) const {
        return xmmName(reg);
    }

    // A float source: its XMM home when it has one, otherwise its frame slot.
    // The fast float selectors all consume this helper, so a spilled float and
    // a homed float are interchangeable operands.
    std::string floatValueRef(uint32_t id) {
        const uint8_t reg = floatHomeOf(id);
        if (reg != static_cast<uint8_t>(XmmRegister::None)) {
            return xmmRef(reg);
        }
        return slot(id);
    }

    // Write an already-computed float in xmm0 (or a home) to its destination.
    // The width names the SSE move; a spilled result keeps the legacy slot
    // spelling.
    void storeFloatResult(uint32_t id, const std::string& source,
                          ir::IRType width) {
        const char* move = width == ir::IRType::F32 ? "movss" : "movsd";
        const uint8_t reg = floatHomeOf(id);
        std::string dest = reg == static_cast<uint8_t>(XmmRegister::None)
                               ? slot(id)
                               : xmmRef(reg);
        out_ += "  " + std::string(move) + " " + dest + ", " + source + "\n";
    }

    // A source operand: its home register when it has one, otherwise a
    // rematerialised immediate for a uniquely-defined integer constant, and
    // finally its frame slot. Every consumer of this helper expects a 64-bit
    // value; the immediate is restricted to the signed 32-bit range so every
    // such consumer has an encoding.
    std::string valueRef(uint32_t id) {
        const uint8_t reg = effectiveHome(id);
        if (reg != static_cast<uint8_t>(Register::None)) {
            return regRef(reg);
        }
        auto constant = rematConstants_.find(id);
        if (constant != rematConstants_.end()) {
            return std::to_string(constant->second);
        }
        if (foldedAtCurrent(id)) {
            // A folded Cast names the immediate its source carried. The
            // ordinary no-home path would read the frame cell the elided cast
            // never wrote.
            int64_t value = 0;
            if (safeConstantValue(id, &value)) {
                return std::to_string(value);
            }
        }
        return slot(id);
    }

    // A destination: write the already-computed 64-bit word to its home, or
    // to the fallback slot.
    void storeResult(uint32_t id, const std::string& source) {
        const uint8_t reg = homeOf(id);
        if (reg == static_cast<uint8_t>(Register::None)) {
            out_ += "  mov " + slot(id) + ", " + source + "\n";
        } else {
            out_ += "  mov " + regRef(reg) + ", " + source + "\n";
        }
    }

    // Write an incoming parameter to the storage the allocator chose. The fast
    // allocator may home a parameter, and the prologue is the only writer of
    // that home, so the write must land there instead of in the frame slot
    // (which the compact frame does not even reserve for a homed value). A
    // parameter the allocator refused keeps the legacy slot write.
    void storeParam(uint32_t id, const std::string& source, bool isFloat,
                    ir::IRType width) {
        if (fast_ && !allocation_.blocks.empty()) {
            if (isFloat && !allocation_.floats.home.empty() &&
                id < allocation_.floats.home[0].size()) {
                const uint8_t x = allocation_.floats.home[0][id];
                if (x != static_cast<uint8_t>(XmmRegister::None)) {
                    const char* move =
                        width == ir::IRType::F32 ? "movss" : "movsd";
                    out_ += "  " + std::string(move) + " " + xmmRef(x) +
                            ", " + source + "\n";
                    return;
                }
            } else if (!isFloat && paramHomeIds_.find(id) !=
                                       paramHomeIds_.end() &&
                       id < allocation_.blocks[0].home.size()) {
                const uint8_t h = allocation_.blocks[0].home[id];
                if (h != static_cast<uint8_t>(Register::None)) {
                    out_ += "  mov " + regRef(h) + ", " + source + "\n";
                    return;
                }
            }
        }
        if (isFloat) {
            const char* move = width == ir::IRType::F32 ? "movss" : "movsd";
            out_ +=
                "  " + std::string(move) + " " + slot(id) + ", " + source +
                "\n";
        } else {
            out_ += "  mov " + slot(id) + ", " + source + "\n";
        }
    }

    // True when the function defines this id with an Alloca. Before mem2reg a
    // parameter's `fn.params` id names such a cell; after mem2reg the same id
    // names an SSA value and no Alloca defines it.
    bool definesAlloca(const ir::Function& fn, uint32_t id) const {
        for (const ir::Block& block : fn.blocks) {
            for (const ir::Instr& instr : block.body) {
                if (instr.op != ir::Op::Alloca) {
                    continue;
                }
                for (const ir::Value& result : instr.results) {
                    if (result.id == id) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    // A parameter cell that no instruction uses except a direct, non-volatile
    // load or store at displacement zero can live in the allocator's register
    // home: the prologue writes that home and the accesses read and write it.
    // An address-taken cell, a Gep base, a volatile access or a displaced
    // access keeps the memory cell, exactly as the legacy path does.
    bool parameterCellPromotable(const ir::Function& fn, uint32_t id) const {
        auto named = [&](const ir::Value& value) { return value.id == id; };
        for (const ir::Block& block : fn.blocks) {
            for (const ir::Instr& instr : block.body) {
                const bool access =
                    (instr.op == ir::Op::Load || instr.op == ir::Op::Store) &&
                    instr.addressBase.id == id;
                if (access &&
                    (instr.isVolatile || instr.displacement != 0 ||
                     !instr.globalLabel.empty())) {
                    return false;
                }
                for (const ir::Value& result : instr.results) {
                    if (!named(result)) {
                        continue;
                    }
                    // The Alloca that defines the cell is the only definition.
                    if (instr.op != ir::Op::Alloca) {
                        return false;
                    }
                }
                for (const ir::Value& operand : instr.operands) {
                    if (named(operand)) {
                        return false;
                    }
                }
                for (const ir::PhiIncoming& incoming : instr.phiIncoming) {
                    if (named(incoming.value)) {
                        return false;
                    }
                }
                if ((instr.op == ir::Op::Load || instr.op == ir::Op::Store ||
                     instr.op == ir::Op::Extract ||
                     instr.op == ir::Op::Insert || instr.op == ir::Op::Gep) &&
                    named(instr.addressBase) && !access) {
                    return false;
                }
                if ((instr.op == ir::Op::Store ||
                     instr.op == ir::Op::Insert) &&
                    named(instr.storedValue)) {
                    return false;
                }
                if (instr.op == ir::Op::Call) {
                    for (const ir::Argument& arg : instr.arguments) {
                        for (const ir::Value& word : arg.words) {
                            if (named(word)) {
                                return false;
                            }
                        }
                    }
                    if (instr.call.indirect && named(instr.calleeValue)) {
                        return false;
                    }
                }
            }
            const ir::Terminator& term = block.term;
            for (const ir::Value& value : term.values) {
                if (named(value)) {
                    return false;
                }
            }
            if (term.cond.has_value() && named(*term.cond)) {
                return false;
            }
            if (term.term == ir::Term::Switch) {
                if (named(term.switchValue)) {
                    return false;
                }
                for (const ir::Case& arm : term.cases) {
                    if (named(arm.value)) {
                        return false;
                    }
                }
            }
        }
        return true;
    }

    // Remove an unwritten or unsafe parameter home from every block. The
    // parameter then keeps its alloca cell, which is the frame storage the
    // legacy path used, so no access can read a register that nothing wrote.
    void dropParamHome(uint32_t id) {
        for (BlockAllocation& block : allocation_.blocks) {
            if (id < block.home.size()) {
                block.home[id] = static_cast<uint8_t>(Register::None);
            }
        }
        for (auto& home : allocation_.floats.home) {
            if (id < home.size()) {
                home[id] = static_cast<uint8_t>(XmmRegister::None);
            }
        }
    }

    // Decide which parameter homes the fast prologue writes, and make the
    // writes safe as a set. The ABI hands parameters to the function in
    // argument registers, so one parameter's home write can destroy the
    // incoming word of a parameter written later (or a later stack word's rax
    // scratch can destroy an earlier home). Such a home is dropped and the
    // parameter keeps its frame slot, as the legacy path does. Every
    // alloca-backed integer cell that survives joins `residentParams_`, whose
    // loads and stores the emitter reads and writes through the home.
    void settleFastParameters(const ir::Function& fn) {
        residentParams_.clear();
        paramHomeIds_.clear();
        if (!fast_ || allocation_.blocks.empty()) {
            return;
        }
        const bool ms = fn.signature.abi() == Abi::MsAbi;
        const uint64_t count = fn.params.size();
        std::vector<uint8_t> source(count, 0);
        std::vector<uint8_t> usesRax(count, 0);
        std::vector<uint8_t> isFloat(count, 0);
        uint64_t gpr = 0;
        for (uint64_t w = 0; w < count; ++w) {
            const ir::IRType cls = w < fn.paramClasses.size()
                                       ? fn.paramClasses[w]
                                       : ir::IRType::Void;
            isFloat[w] = cls == ir::IRType::F32 || cls == ir::IRType::F64
                             ? 1
                             : 0;
            if (ms) {
                // Microsoft x64 gives every argument word one positional
                // slot; a float in slot w travels in xmm{w}.
                if (isFloat[w] != 0) {
                    continue;
                }
                if (w < kMsabiIntRegs) {
                    source[w] = static_cast<uint8_t>(argRegisterMs(w));
                } else {
                    usesRax[w] = 1;
                }
                continue;
            }
            if (isFloat[w] != 0) {
                // SysV floats take their own SSE counter; a GPR home can never
                // collide with an XMM argument register.
                continue;
            }
            if (gpr < kSysVIntRegs) {
                source[w] = static_cast<uint8_t>(argRegister(gpr));
            } else {
                usesRax[w] = 1;
            }
            ++gpr;
        }

        for (uint64_t w = 0; w < count; ++w) {
            const uint32_t id = fn.params[w].id;
            if (id == kInvalidValue) {
                continue;
            }
            if (isFloat[w] != 0) {
                // Float parameters stay excluded from the float allocator and
                // keep their cells; the float branch of `storeParam` decides.
                // A lowered float cell can still receive a GPR home from the
                // integer scan (its type is a pointer); no path may read that
                // unwritten home, so it goes.
                if (definesAlloca(fn, id)) {
                    dropParamHome(id);
                }
                continue;
            }
            const bool allocaBacked = definesAlloca(fn, id);
            if (!allocaBacked) {
                // An SSA parameter (mem2reg ran): its home is the value home
                // every other instruction already reads and writes.
                if (id < allocation_.blocks[0].home.size() &&
                    allocation_.blocks[0].home[id] !=
                        static_cast<uint8_t>(Register::None)) {
                    paramHomeIds_.insert(id);
                }
                continue;
            }
            if (!parameterCellPromotable(fn, id) ||
                id >= allocation_.blocks[0].home.size() ||
                allocation_.blocks[0].home[id] ==
                    static_cast<uint8_t>(Register::None)) {
                // The cell stays in memory: the prologue writes its slot and
                // every access names it. An unwritten home would still answer
                // `homeOf` in paths such as call staging, so drop it.
                dropParamHome(id);
                continue;
            }
            residentParams_.insert(id);
            paramHomeIds_.insert(id);
        }

        for (uint64_t i = 0; i < count; ++i) {
            const uint32_t id = fn.params[i].id;
            if (id == kInvalidValue ||
                paramHomeIds_.find(id) == paramHomeIds_.end() ||
                id >= allocation_.blocks[0].home.size()) {
                continue;
            }
            const uint8_t home = allocation_.blocks[0].home[id];
            if (home == static_cast<uint8_t>(Register::None)) {
                continue;
            }
            bool clobbered = false;
            for (uint64_t j = i + 1; j < count && !clobbered; ++j) {
                if (source[j] != 0 && source[j] == home) {
                    clobbered = true;
                }
                if (home == static_cast<uint8_t>(Register::Rax) &&
                    usesRax[j] != 0) {
                    clobbered = true;
                }
            }
            if (!clobbered) {
                continue;
            }
            paramHomeIds_.erase(id);
            residentParams_.erase(id);
            dropParamHome(id);
        }
    }

    // The register carrying a promoted parameter cell in this block. The
    // allocator homes it in every block that reads or writes the cell, so a
    // missing home is a compiler invariant failure, not a value without
    // storage.
    uint8_t residentParamReg(uint32_t id) {
        const uint8_t reg = homeOf(id);
        if (reg == static_cast<uint8_t>(Register::None)) {
            refuse(Position{"", 1, 1},
                   "parameter home is missing in a block that accesses it");
        }
        return reg;
    }

    // A load from a register-resident parameter cell. The width rules are
    // `emitLoadMem`'s, applied to the home register instead of memory so the
    // loaded word is canonicalized exactly as the slot path canonicalized it.
    // A homed result is written directly, like the memory path: x86 reads the
    // source before writing the destination, so the result home may alias the
    // parameter home.
    void emitResidentParamLoad(const ir::Instr& instr) {
        const uint8_t reg = residentParamReg(instr.addressBase.id);
        const uint8_t home = homeOf(instr.results[0].id);
        if (instr.width == ir::IRType::F32 ||
            instr.width == ir::IRType::F64) {
            if (instr.width == ir::IRType::F32) {
                out_ += "  movd xmm0, " +
                        std::string(regWidth(reg, 'd')) + "\n";
            } else {
                out_ += "  movq xmm0, " +
                        std::string(regWidth(reg, 'q')) + "\n";
            }
            storeFloatResult(instr.results[0].id, "xmm0", instr.width);
            return;
        }
        if (home != static_cast<uint8_t>(Register::None)) {
            const std::string q = regWidth(home, 'q');
            const std::string d = regWidth(home, 'd');
            switch (instr.width) {
                case ir::IRType::I8:
                case ir::IRType::U8:
                case ir::IRType::Bool:
                    out_ += signedRead(instr.type) ? "  movsx " + q + ", "
                                                   : "  movzx " + d + ", ";
                    out_ += std::string(regWidth(reg, 'b')) + "\n";
                    return;
                case ir::IRType::I16:
                case ir::IRType::U16:
                    out_ += signedRead(instr.type) ? "  movsx " + q + ", "
                                                   : "  movzx " + d + ", ";
                    out_ += std::string(regWidth(reg, 'w')) + "\n";
                    return;
                case ir::IRType::I32:
                    out_ += "  movsxd " + q + ", " +
                            std::string(regWidth(reg, 'd')) + "\n";
                    return;
                case ir::IRType::U32:
                    out_ += "  mov " + d + ", " +
                            std::string(regWidth(reg, 'd')) + "\n";
                    return;
                default:
                    out_ += "  mov " + q + ", " +
                            std::string(regWidth(reg, 'q')) + "\n";
                    return;
            }
        }
        switch (instr.width) {
            case ir::IRType::I8:
            case ir::IRType::U8:
            case ir::IRType::Bool:
                out_ += signedRead(instr.type) ? "  movsx eax, "
                                               : "  movzx eax, ";
                out_ += std::string(regWidth(reg, 'b')) + "\n";
                out_ += "  movsxd rax, eax\n";
                break;
            case ir::IRType::I16:
            case ir::IRType::U16:
                out_ += signedRead(instr.type) ? "  movsx eax, "
                                               : "  movzx eax, ";
                out_ += std::string(regWidth(reg, 'w')) + "\n";
                out_ += "  movsxd rax, eax\n";
                break;
            case ir::IRType::I32:
                out_ += "  movsxd rax, " +
                        std::string(regWidth(reg, 'd')) + "\n";
                break;
            case ir::IRType::U32:
                out_ += "  mov eax, " +
                        std::string(regWidth(reg, 'd')) + "\n";
                break;
            default:
                out_ += "  mov rax, " +
                        std::string(regWidth(reg, 'q')) + "\n";
                break;
        }
        storeResult(instr.results[0].id, "rax");
    }

    // A store to a register-resident parameter cell. The value is already in
    // rax or xmm0 exactly as the memory path left it; the width names the
    // bytes that move into the home, mirroring `emitStoreMem`'s width rules.
    void emitResidentParamStore(const ir::Instr& instr) {
        const uint8_t reg = residentParamReg(instr.addressBase.id);
        switch (instr.width) {
            case ir::IRType::F32:
                out_ += "  movd " + std::string(regWidth(reg, 'd')) +
                        ", xmm0\n";
                break;
            case ir::IRType::F64:
                out_ += "  movq " + std::string(regWidth(reg, 'q')) +
                        ", xmm0\n";
                break;
            case ir::IRType::I8:
            case ir::IRType::U8:
            case ir::IRType::Bool:
                out_ += "  mov " + std::string(regWidth(reg, 'b')) + ", al\n";
                break;
            case ir::IRType::I16:
            case ir::IRType::U16:
                out_ += "  mov " + std::string(regWidth(reg, 'w')) + ", ax\n";
                break;
            case ir::IRType::I32:
            case ir::IRType::U32:
                out_ += "  mov " + std::string(regWidth(reg, 'd')) +
                        ", eax\n";
                break;
            default:
                out_ += "  mov " + std::string(regWidth(reg, 'q')) + ", rax\n";
                break;
        }
    }

    // True when a value is a rematerialisable immediate and has no register
    // home, in which case `valueRef` returns the number itself. The fast
    // selectors use this to pick an immediate form instead of loading a
    // register first.
    bool immediateRef(uint32_t id, int64_t* value) const {
        if (effectiveHome(id) != static_cast<uint8_t>(Register::None)) {
            return false;
        }
        auto found = rematConstants_.find(id);
        if (found != rematConstants_.end()) {
            if (value != nullptr) {
                *value = found->second;
            }
            return true;
        }
        if (foldedAtCurrent(id)) {
            int64_t constant = 0;
            if (safeConstantValue(id, &constant)) {
                if (value != nullptr) {
                    *value = constant;
                }
                return true;
            }
        }
        return false;
    }

    // True when `instr` reads the value `id` through any value-bearing field.
    // The constant-fold lookahead uses this to find a Const's first reader: a
    // `Value` that the opcode does not use still holds id 0, so the
    // opcode-specific fields are guarded by op.
    static bool instructionReads(const ir::Instr& instr, uint32_t id) {
        for (const ir::Value& operand : instr.operands) {
            if (operand.id == id) {
                return true;
            }
        }
        switch (instr.op) {
            case ir::Op::Load:
            case ir::Op::Store:
            case ir::Op::Extract:
            case ir::Op::Insert:
            case ir::Op::Gep:
                if (instr.addressBase.id == id) {
                    return true;
                }
                break;
            default:
                break;
        }
        if ((instr.op == ir::Op::Store || instr.op == ir::Op::Insert) &&
            instr.storedValue.id == id) {
            return true;
        }
        if (instr.op == ir::Op::Call && instr.calleeValue.id == id) {
            return true;
        }
        for (const ir::Argument& arg : instr.arguments) {
            for (const ir::Value& word : arg.words) {
                if (word.id == id) {
                    return true;
                }
            }
        }
        return false;
    }

    // The immediate a value materialises, whether it is a Const or any Cast
    // with a recorded source. This is only safe for the conservative overlap
    // guard below: a cast that changes its source value is still a cast, and
    // the folds must use safeConstantValue() instead.
    bool constantValue(uint32_t id, int64_t* value) const {
        auto constant = rematConstants_.find(id);
        if (constant != rematConstants_.end()) {
            if (value != nullptr) {
                *value = constant->second;
            }
            return true;
        }
        auto cast = castConstants_.find(id);
        if (cast != castConstants_.end()) {
            if (value != nullptr) {
                *value = cast->second;
            }
            return true;
        }
        return false;
    }

    // The storage width of an IR word, in bits. Bool rides its byte cell.
    static int irWidthBits(ir::IRType width) {
        switch (width) {
            case ir::IRType::I8:
            case ir::IRType::U8:
            case ir::IRType::Bool:
                return 8;
            case ir::IRType::I16:
            case ir::IRType::U16:
                return 16;
            case ir::IRType::I32:
            case ir::IRType::U32:
            case ir::IRType::F32:
                return 32;
            case ir::IRType::I64:
            case ir::IRType::U64:
            case ir::IRType::F64:
            case ir::IRType::Ptr:
                return 64;
            case ir::IRType::Void:
                return 0;
        }
        return 0;
    }

    // True when a cast of the constant `value` leaves that value unchanged, so
    // the source immediate can stand in for the cast result. A narrowing cast
    // or a same-width signedness change does not: the emitter would truncate
    // or re-extend the word, and the source number would be wrong.
    static bool castPreservesConstant(const ir::Instr& cast, int64_t value) {
        if (cast.operands.size() != kOneResult) {
            return false;
        }
        const int fromBits = irWidthBits(cast.operands[0].width);
        const int toBits = irWidthBits(cast.width);
        if (toBits < fromBits) {
            return false;
        }
        const bool fromSigned = ir::signedWidth(cast.operands[0].width,
                                                cast.operands[0].type);
        const bool toSigned = ir::signedWidth(cast.width, cast.type);
        // Widening a negative signed value to an unsigned type is not value
        // preserving: the cast zero-extends, while the source immediate is
        // negative and would be sign-extended by a consumer.
        if (fromSigned && !toSigned && value < 0) {
            return false;
        }
        if (toBits == fromBits && fromSigned != toSigned && value != 0) {
            return false;
        }
        return true;
    }

    // A constant an immediate fold may use: a Const, or a Cast whose value is
    // exactly its source immediate. `constantValue` remains for the
    // conservative overlap guard, which only needs to know that a home holds
    // some constant.
    bool safeConstantValue(uint32_t id, int64_t* value) const {
        auto constant = rematConstants_.find(id);
        if (constant != rematConstants_.end()) {
            if (value != nullptr) {
                *value = constant->second;
            }
            return true;
        }
        if (preservingCastIds_.find(id) == preservingCastIds_.end()) {
            return false;
        }
        auto cast = castConstants_.find(id);
        if (cast == castConstants_.end()) {
            return false;
        }
        if (value != nullptr) {
            *value = cast->second;
        }
        return true;
    }

    // True when the instruction reads the register `home` through any value
    // it names. Used only by the dead-home fold, which must leave a home
    // alone if the block still reads it before writing it.
    bool instructionReadsHome(const ir::Instr& instr, uint8_t home) const {
        auto reads = [&](const ir::Value& value) {
            return value.id != kInvalidValue && homeOf(value.id) == home;
        };
        for (const ir::Value& operand : instr.operands) {
            if (reads(operand)) {
                return true;
            }
        }
        switch (instr.op) {
            case ir::Op::Load:
            case ir::Op::Store:
            case ir::Op::Extract:
            case ir::Op::Insert:
            case ir::Op::Gep:
                if (reads(instr.addressBase)) {
                    return true;
                }
                break;
            default:
                break;
        }
        if ((instr.op == ir::Op::Store || instr.op == ir::Op::Insert) &&
            reads(instr.storedValue)) {
            return true;
        }
        if (instr.op == ir::Op::Call) {
            if (instr.call.indirect && reads(instr.calleeValue)) {
                return true;
            }
            for (const ir::Argument& arg : instr.arguments) {
                for (const ir::Value& word : arg.words) {
                    if (reads(word)) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    // The terminator's value reads, for the same home-safety check.
    bool terminatorReadsHome(const ir::Terminator& term, uint8_t home) const {
        auto reads = [&](const ir::Value& value) {
            return value.id != kInvalidValue && homeOf(value.id) == home;
        };
        for (const ir::Value& word : term.values) {
            if (reads(word)) {
                return true;
            }
        }
        if (term.cond.has_value() && reads(*term.cond)) {
            return true;
        }
        for (const ir::Case& arm : term.cases) {
            if (reads(arm.value)) {
                return true;
            }
        }
        if (term.term == ir::Term::Switch && reads(term.switchValue)) {
            return true;
        }
        return false;
    }

    // True when the instruction writes `home`, either as an IR result or as
    // one of the fixed registers div/rem and variable shifts own.
    bool instructionWritesHome(const ir::Instr& instr, uint8_t home) const {
        for (const ir::Value& result : instr.results) {
            if (homeOf(result.id) == home) {
                return true;
            }
        }
        const uint16_t fixed = fixedRegisterWrites(instr);
        return home < 16 && (fixed & (1u << home)) != 0;
    }

    // A call preserves the callee-saved homes (rbx, r12-r15) and clobbers the
    // caller-saved ones; the home-safety scan may stop at a clobber.
    bool callerSavedHome(uint8_t home) const {
        const uint8_t rax = static_cast<uint8_t>(Register::Rax);
        const uint8_t rcx = static_cast<uint8_t>(Register::Rcx);
        const uint8_t rdx = static_cast<uint8_t>(Register::Rdx);
        const uint8_t rsi = static_cast<uint8_t>(Register::Rsi);
        const uint8_t rdi = static_cast<uint8_t>(Register::Rdi);
        const uint8_t r8 = static_cast<uint8_t>(Register::R8);
        const uint8_t r11 = static_cast<uint8_t>(Register::R11);
        return home == rax || home == rcx || home == rdx || home == rsi ||
               home == rdi || (home >= r8 && home <= r11);
    }

    // True when no later instruction in the block reads `home` before a write
    // to it. The constant's own value is single-use, but the allocator may
    // hand the same register to another value in the block; suppressing the
    // constant move must not leave that value reading a stale word.
    bool homeSafeAfter(const ir::Block& block, uint32_t index,
                       uint8_t home) const {
        for (uint32_t k = index + 1; k < block.body.size(); ++k) {
            const ir::Instr& later = block.body[k];
            if (instructionReadsHome(later, home)) {
                return false;
            }
            if (instructionWritesHome(later, home)) {
                return true;
            }
            if (later.op == ir::Op::Asm) {
                // Raw asm can name a register without an IR operand.
                return false;
            }
            if (later.op == ir::Op::Trap) {
                return true;
            }
            if (later.op == ir::Op::Call && callerSavedHome(home)) {
                return true;
            }
        }
        return !terminatorReadsHome(block.term, home);
    }

    // True when the call stages `id` as a GPR argument word in one of its
    // argument registers. A word past the register row travels through the
    // outgoing area, where the register-move staging this fold feeds does not
    // apply; a float word is staged through its XMM home, not the GPR row.
    bool callRegisterWord(const ir::Instr& call, uint32_t id) const {
        if (call.op != ir::Op::Call) {
            return false;
        }
        uint64_t gpr = 0;
        uint64_t position = 0;
        if (call.call.msabi) {
            for (const ir::Argument& arg : call.arguments) {
                if (isSingleFloatWordArgument(arg)) {
                    ++position;
                    continue;
                }
                for (const ir::Value& word : arg.words) {
                    if (word.id == id) {
                        return position < kMsabiIntRegs;
                    }
                    ++position;
                }
            }
            return false;
        }
        for (const ir::Argument& arg : call.arguments) {
            if (isSingleFloatWordArgument(arg)) {
                continue;
            }
            for (const ir::Value& word : arg.words) {
                if (word.id == id) {
                    return gpr < kSysVIntRegs;
                }
                ++gpr;
            }
        }
        return false;
    }

    // True when a fast `mov D, imm` can be elided and its immediate used by
    // `consumer` directly. Unlike `fastImmHomeFold`, the result home need not
    // be D: the allocator did not reuse D, and the constant is simply dead
    // after this one reader. Stores, address uses, compares with the constant
    // on the left, and a multiply whose other operand is already an immediate
    // stay on their existing paths. A call folds only a GPR argument word,
    // and only when no asm or volatile access sits between the constant and
    // the call.
    bool deadImmConsumer(const ir::Block& block, uint32_t index,
                         const ir::Instr& consumer, uint32_t constantId,
                         int64_t value, uint8_t home,
                         bool crossedBarrier) const {
        if (value < INT32_MIN || value > INT32_MAX) {
            return false;
        }
        const uint8_t resultHome =
            consumer.results.empty()
                ? static_cast<uint8_t>(Register::None)
                : homeOf(consumer.results[0].id);
        // The existing 18.2 fold owns a consumer whose constant home is the
        // result home. Leave it, and any second constant feeding that same
        // consumer, on the register path so the two folds cannot overlap.
        if (resultHome == home || fastImmHomeFold(consumer, nullptr, nullptr)) {
            return false;
        }
        for (const ir::Value& operand : consumer.operands) {
            if (operand.id != constantId && constantValue(operand.id, nullptr) &&
                homeOf(operand.id) == resultHome) {
                return false;
            }
        }
        switch (consumer.op) {
            case ir::Op::Add:
            case ir::Op::Sub:
            case ir::Op::And:
            case ir::Op::Or:
            case ir::Op::Xor:
            case ir::Op::Mul: {
                if (consumer.operands.size() != kBinaryOperands) {
                    return false;
                }
                uint32_t other = kInvalidValue;
                if (consumer.operands[0].id == constantId) {
                    other = consumer.operands[1].id;
                } else if (consumer.operands[1].id == constantId) {
                    other = consumer.operands[0].id;
                }
                if (other == kInvalidValue) {
                    return false;
                }
                if (consumer.op == ir::Op::Mul) {
                    // Match fastImmHomeFold's width rule, and keep the
                    // three-operand imul shape whose other operand is already
                    // an immediate: x86 cannot encode two immediates, so the
                    // emitter would have to materialise one again.
                    switch (consumer.width) {
                        case ir::IRType::I32:
                        case ir::IRType::U32:
                        case ir::IRType::I64:
                        case ir::IRType::U64:
                        case ir::IRType::Ptr:
                            break;
                        default:
                            return false;
                    }
                    if (constantValue(other, nullptr)) {
                        return false;
                    }
                }
                break;
            }
            case ir::Op::Cmp:
                // Only the right operand: folding a left immediate would
                // still need a register for `cmp reg, imm`, so it saves
                // nothing and would need the excluded branch inversion.
                if (consumer.operands.size() < kBinaryOperands ||
                    consumer.operands[1].id != constantId) {
                    return false;
                }
                break;
            case ir::Op::Copy:
                if (consumer.operands.size() != kOneResult ||
                    consumer.operands[0].id != constantId ||
                    consumer.results.empty() ||
                    homeOf(consumer.results[0].id) ==
                        static_cast<uint8_t>(Register::None)) {
                    return false;
                }
                break;
            case ir::Op::Cast:
                if (consumer.operands.size() != kOneResult ||
                    consumer.operands[0].id != constantId ||
                    consumer.results.empty()) {
                    return false;
                }
                break;
            case ir::Op::Call:
                // The constant must be a register argument word, not the
                // callee or a word past the register row. The lookahead has
                // already ruled out an earlier reader and a home overwrite.
                if (crossedBarrier ||
                    !callRegisterWord(consumer, constantId)) {
                    return false;
                }
                break;
            default:
                return false;
        }
        if (!fast_ || constantId >= useCount_.size() ||
            useCount_[constantId] != kSingleUse || currentLiveOut_ == nullptr ||
            std::binary_search(currentLiveOut_->begin(),
                               currentLiveOut_->end(), constantId)) {
            return false;
        }
        return homeSafeAfter(block, index, home);
    }

    // True when a fast `mov D, imm` feeding the next binary can be elided:
    // one operand is a single-definition integer constant, the allocator gave
    // its home to the result (so the constant's live interval ends here), and
    // the operation has an immediate form that reads the other operand's home
    // directly. `add` becomes `lea D, [R + imm]`; `imul` becomes the
    // three-operand form. Either operand may be the constant: the allocator is
    // free to place a commutative operation's result in the constant's home.
    // 8/16-bit multiplies stay on the compute-and-narrow path because they
    // have no such form in the emitter's 64-bit model.
    bool fastImmHomeFold(const ir::Instr& instr, int64_t* value,
                         uint8_t* otherHome) const {
        if (!fast_ || instr.results.empty() ||
            instr.operands.size() != kBinaryOperands) {
            return false;
        }
        if (instr.op != ir::Op::Add && instr.op != ir::Op::Mul) {
            return false;
        }
        switch (instr.width) {
            case ir::IRType::I8:
            case ir::IRType::U8:
            case ir::IRType::I16:
            case ir::IRType::U16:
            case ir::IRType::I32:
            case ir::IRType::U32:
            case ir::IRType::I64:
            case ir::IRType::U64:
            case ir::IRType::Ptr:
                break;
            default:
                return false;
        }
        if (instr.op == ir::Op::Mul && instr.width != ir::IRType::I32 &&
            instr.width != ir::IRType::U32 &&
            instr.width != ir::IRType::I64 &&
            instr.width != ir::IRType::U64 &&
            instr.width != ir::IRType::Ptr) {
            return false;
        }
        const uint8_t dest = homeOf(instr.results[0].id);
        if (dest == static_cast<uint8_t>(Register::None)) {
            return false;
        }
        const uint32_t leftId = instr.operands[0].id;
        const uint32_t rightId = instr.operands[1].id;
        int64_t leftValue = 0;
        int64_t rightValue = 0;
        const bool leftConstant = safeConstantValue(leftId, &leftValue);
        const bool rightConstant = safeConstantValue(rightId, &rightValue);
        const bool leftFold =
            homeOf(leftId) == dest && leftConstant &&
            leftValue >= INT32_MIN && leftValue <= INT32_MAX;
        const bool rightFold =
            homeOf(rightId) == dest && rightConstant &&
            rightValue >= INT32_MIN && rightValue <= INT32_MAX;
        uint8_t other = static_cast<uint8_t>(Register::None);
        int64_t constant = 0;
        if (leftFold) {
            other = homeOf(rightId);
            constant = leftValue;
        } else if (rightFold) {
            other = homeOf(leftId);
            constant = rightValue;
        } else {
            return false;
        }
        if (other == static_cast<uint8_t>(Register::None)) {
            return false;
        }
        if (value != nullptr) {
            *value = constant;
        }
        if (otherHome != nullptr) {
            *otherHome = other;
        }
        return true;
    }

    // Canonicalize the fast arithmetic result in its home exactly as the
    // legacy path canonicalized rax: sub-64-bit arithmetic computes at 64 bits,
    // then the low bits are sign- or zero-extended per the declared width. The
    // narrow extension writings keep the home's high bits equal to what the
    // legacy `storeResult` copied, so a later 64-bit consumer reads the same
    // word either way.
    void emitFastNarrow(uint8_t home, const ir::Instr& instr) {
        const bool isSigned = ir::signedWidth(instr.width, instr.type);
        switch (instr.width) {
            case ir::IRType::I32:
                out_ += std::string("  movsxd ") + regWidth(home, 'q') + ", " +
                        regWidth(home, 'd') + "\n";
                return;
            case ir::IRType::U32:
                out_ += std::string("  mov ") + regWidth(home, 'd') + ", " +
                        regWidth(home, 'd') + "\n";
                return;
            case ir::IRType::I16:
            case ir::IRType::U16:
                out_ += std::string(isSigned ? "  movsx " : "  movzx ") +
                        regWidth(home, 'q') + ", " + regWidth(home, 'w') + "\n";
                return;
            case ir::IRType::I8:
            case ir::IRType::U8:
                out_ += std::string(isSigned ? "  movsx " : "  movzx ") +
                        (isSigned ? regWidth(home, 'q') : regWidth(home, 'd')) +
                        ", " + regWidth(home, 'b') + "\n";
                return;
            default:
                return;
        }
    }

    // Direct destination-aware integer arithmetic. The result's home receives
    // the left operand (unless it already lives there), the right operand is
    // applied in place, and only the width canonicalization follows. A spilled
    // right operand becomes a memory operand, an add of a rematerialised
    // displacement becomes `lea`, and a multiply whose result differs from the
    // left uses the three-operand immediate form. The unconditional rax round
    // trip belongs to the legacy path only.
    bool emitFastBinary(const ir::Instr& instr) {
        if (!fast_ || instr.results.empty() ||
            instr.operands.size() != kBinaryOperands) {
            return false;
        }
        switch (instr.op) {
            case ir::Op::Add:
            case ir::Op::Sub:
            case ir::Op::And:
            case ir::Op::Or:
            case ir::Op::Xor:
            case ir::Op::Mul:
            case ir::Op::Shl:
            case ir::Op::Shr:
                break;
            default:
                // The selector below derives its mnemonic from the opcode;
                // an opcode outside this allowlist must never fall through to
                // the default `add` spelling.
                return false;
        }
        switch (instr.width) {
            case ir::IRType::I8:
            case ir::IRType::U8:
            case ir::IRType::I16:
            case ir::IRType::U16:
            case ir::IRType::I32:
            case ir::IRType::U32:
            case ir::IRType::I64:
            case ir::IRType::U64:
            case ir::IRType::Ptr:
                break;
            default:
                return false;
        }
        const uint32_t resultId = instr.results[0].id;
        const uint8_t dest = homeOf(resultId);
        const uint32_t leftId = instr.operands[0].id;
        const uint32_t rightId = instr.operands[1].id;
        const uint8_t leftHome = effectiveHome(leftId);
        const uint8_t rightHome = effectiveHome(rightId);
        int64_t rightConstant = 0;
        const bool rightImm = immediateRef(rightId, &rightConstant);
        int64_t leftConstant = 0;
        const bool leftImm = immediateRef(leftId, &leftConstant);
        const char* name = "add";
        if (instr.op == ir::Op::Sub) {
            name = "sub";
        } else if (instr.op == ir::Op::And) {
            name = "and";
        } else if (instr.op == ir::Op::Or) {
            name = "or";
        } else if (instr.op == ir::Op::Xor) {
            name = "xor";
        } else if (instr.op == ir::Op::Mul) {
            name = "imul";
        }

        // An immediate shift needs no cl staging. The count is an 8-bit
        // immediate whose low bits the hardware masks at the operand width,
        // so the existing cl sequence and this form agree for 0..63. A
        // non-rematerialised count or an out-of-range one falls back to the
        // cl sequence below.
        if (instr.op == ir::Op::Shl || instr.op == ir::Op::Shr) {
            if (!rightImm || rightConstant < 0 || rightConstant > 63) {
                return false;
            }
            const bool isSigned = ir::signedWidth(instr.width, instr.type);
            const char* shiftName =
                instr.op == ir::Op::Shl ? "shl" : (isSigned ? "sar" : "shr");
            if (dest == static_cast<uint8_t>(Register::None)) {
                if (instr.width != ir::IRType::I64 &&
                    instr.width != ir::IRType::U64 &&
                    instr.width != ir::IRType::Ptr) {
                    return false;
                }
                const std::string dst = "qword ptr " + slot(resultId);
                const bool leftMemory =
                    leftHome == static_cast<uint8_t>(Register::None) &&
                    !leftImm;
                if (leftMemory) {
                    // No memory-to-memory move exists. rax is this path's
                    // fixed scratch, so the guard preserves any live home it
                    // would otherwise destroy.
                    beginFixedGuard(instr);
                    out_ += "  mov rax, " + valueRef(leftId) + "\n";
                    out_ += "  mov " + dst + ", rax\n";
                } else {
                    out_ += "  mov " + dst + ", " + valueRef(leftId) + "\n";
                }
                out_ += "  " + std::string(shiftName) + " " + dst + ", " +
                        std::to_string(rightConstant) + "\n";
                if (leftMemory) {
                    endFixedGuard(instr);
                }
                return true;
            }
            char width = 'q';
            if (instr.width == ir::IRType::I32 ||
                instr.width == ir::IRType::U32) {
                width = 'd';
            } else if (instr.width == ir::IRType::I16 ||
                       instr.width == ir::IRType::U16) {
                width = 'w';
            } else if (instr.width == ir::IRType::I8 ||
                       instr.width == ir::IRType::U8) {
                width = 'b';
            }
            const std::string destReg = regWidth(dest, 'q');
            if (leftHome != dest) {
                out_ += "  mov " + destReg + ", " + valueRef(leftId) + "\n";
            }
            out_ += "  " + std::string(shiftName) + " " +
                    regWidth(dest, width) + ", " +
                    std::to_string(rightConstant) + "\n";
            emitFastNarrow(dest, instr);
            return true;
        }

        if (dest == static_cast<uint8_t>(Register::None)) {
            // A 64-bit result with no home updates its frame cell in place.
            // Multiplication has no memory-destination form, and a narrow
            // result still needs the generic width canonicalization, so both
            // stay on the rax path.
            if (instr.op == ir::Op::Mul ||
                (instr.width != ir::IRType::I64 &&
                 instr.width != ir::IRType::U64 &&
                 instr.width != ir::IRType::Ptr)) {
                return false;
            }
            const bool leftMemory =
                leftHome == static_cast<uint8_t>(Register::None) && !leftImm;
            const bool rightMemory =
                rightHome == static_cast<uint8_t>(Register::None) && !rightImm;
            // x86 has no mem,mem arithmetic. The sequence below moves the left
            // operand into the destination cell, so both sources must already
            // be register or immediate values; anything else stays on the
            // generic rax path.
            if (leftMemory || rightMemory) {
                return false;
            }
            const std::string dst = "qword ptr " + slot(resultId);
            out_ += "  mov " + dst + ", " + valueRef(leftId) + "\n";
            out_ += "  " + std::string(name) + " " + dst + ", " +
                    (rightImm ? std::to_string(rightConstant)
                              : valueRef(rightId)) +
                    "\n";
            return true;
        }
        const std::string destReg = regWidth(dest, 'q');

        // A constant whose home is the result home is dead after this
        // operation: the allocator reused its register only because its live
        // interval ended here. Replace the separate constant move and the
        // two-operand op with the immediate form, which reads the other
        // operand's home directly.
        int64_t foldedConstant = 0;
        uint8_t foldedOther = static_cast<uint8_t>(Register::None);
        if (fastImmHomeFold(instr, &foldedConstant, &foldedOther)) {
            if (instr.op == ir::Op::Add) {
                out_ += "  lea " + destReg + ", " +
                        atAddr(regRef(foldedOther), foldedConstant) + "\n";
            } else {
                const bool narrow = instr.width == ir::IRType::I32 ||
                                    instr.width == ir::IRType::U32;
                const char width = narrow ? 'd' : 'q';
                out_ += std::string("  imul ") + regWidth(dest, width) + ", " +
                        regWidth(foldedOther, width) + ", " +
                        std::to_string(foldedConstant) + "\n";
            }
            emitFastNarrow(dest, instr);
            return true;
        }

        // The allocator lets a result reuse a source register whose interval
        // ends at this instruction. If that source is the right operand, a
        // "move left into dest" would destroy it first. Commutative opcodes
        // apply the left operand instead; subtraction negates the held right
        // and adds the left, which is the same low-width result.
        if (rightHome == dest && leftHome != dest) {
            if (instr.op == ir::Op::Sub) {
                out_ += "  neg " + destReg + "\n";
                out_ += "  add " + destReg + ", " + valueRef(leftId) + "\n";
            } else if (instr.op == ir::Op::Mul && leftImm) {
                out_ += "  imul " + destReg + ", " + destReg + ", " +
                        std::to_string(leftConstant) + "\n";
            } else {
                out_ += std::string("  ") + name + " " + destReg + ", " +
                        valueRef(leftId) + "\n";
            }
            emitFastNarrow(dest, instr);
            return true;
        }

        if (leftHome != dest) {
            if (instr.op == ir::Op::Mul && rightImm) {
                if (leftImm) {
                    out_ += "  mov " + destReg + ", " + valueRef(leftId) + "\n";
                    out_ += "  imul " + destReg + ", " + destReg + ", " +
                            std::to_string(rightConstant) + "\n";
                } else {
                    out_ += "  imul " + destReg + ", " + valueRef(leftId) +
                            ", " + std::to_string(rightConstant) + "\n";
                }
            } else if (instr.op == ir::Op::Add && rightImm && !leftImm &&
                       leftHome != static_cast<uint8_t>(Register::None)) {
                // A displacement add with a homed left is one lea: no
                // intermediate register and no flag dependency.
                out_ += "  lea " + destReg + ", " +
                        atAddr(valueRef(leftId), rightConstant) + "\n";
            } else {
                out_ += "  mov " + destReg + ", " + valueRef(leftId) + "\n";
                out_ += std::string("  ") + name + " " + destReg + ", " +
                        valueRef(rightId) + "\n";
            }
        } else if (instr.op == ir::Op::Mul && rightImm) {
            // Result and left share the home: the three-operand form still
            // names it explicitly, so the immediate has an unambiguous width.
            out_ += "  imul " + destReg + ", " + destReg + ", " +
                    std::to_string(rightConstant) + "\n";
        } else {
            out_ += std::string("  ") + name + " " + destReg + ", " +
                    valueRef(rightId) + "\n";
        }
        emitFastNarrow(dest, instr);
        return true;
    }

    // Integer comparison flags for a fast Cmp, without materialising a
    // boolean. `test x, x` is the zero-compare form; otherwise one operand may
    // stay in memory. rax is fixed scratch when both operands are memory or the
    // left is an immediate, which the allocator already accounts for.
    void emitFastCompareFlags(const ir::Instr& instr) {
        const uint32_t leftId = instr.operands[0].id;
        const uint32_t rightId = instr.operands[1].id;
        auto constant = constValues_.find(rightId);
        if (constant != constValues_.end() && constant->second == 0) {
            const uint8_t leftHome = effectiveHome(leftId);
            if (leftHome != static_cast<uint8_t>(Register::None)) {
                const std::string reg = regRef(leftHome);
                out_ += "  test " + reg + ", " + reg + "\n";
            } else {
                out_ += "  mov rax, " + valueRef(leftId) + "\n";
                out_ += "  test rax, rax\n";
            }
            return;
        }
        const uint8_t leftHome = effectiveHome(leftId);
        const uint8_t rightHome = effectiveHome(rightId);
        const bool leftImm = immediateRef(leftId, nullptr);
        const bool rightImm = immediateRef(rightId, nullptr);
        std::string left = valueRef(leftId);
        bool leftMemory = leftHome == static_cast<uint8_t>(Register::None) &&
                          !leftImm;
        if (leftImm) {
            out_ += "  mov rax, " + left + "\n";
            left = "rax";
        }
        if (!rightImm && leftMemory && !leftImm &&
            rightHome == static_cast<uint8_t>(Register::None)) {
            // Both operands are in frame cells; x86 has no mem,mem compare.
            out_ += "  mov rax, " + left + "\n";
            left = "rax";
            leftMemory = false;
        }
        if (leftMemory) {
            // A memory left operand has no register from which to infer the
            // operand width; name the canonical 64-bit cell. A memory/memory
            // pair was already moved to rax above.
            left = "qword ptr " + left;
        }
        out_ += "  cmp " + left + ", " + valueRef(rightId) + "\n";
    }

    bool emitFastCompare(const ir::Instr& instr) {
        if (!fast_ || instr.results.empty() ||
            instr.operands.size() < kBinaryOperands) {
            return false;
        }
        const uint32_t result = instr.results[0].id;
        // Liveness is the conservative half of the fusion decision: a boolean
        // that is live after this block has a consumer the branch cannot
        // replace, so flags alone would not define its value. The use tally
        // below is the narrower single-use/terminator check; this guard does
        // not rely on that enumeration being complete.
        bool liveOut = currentLiveOut_ == nullptr;
        if (currentLiveOut_ != nullptr) {
            liveOut = std::binary_search(currentLiveOut_->begin(),
                                         currentLiveOut_->end(), result);
        }
        const bool last = currentBlock_ != nullptr &&
                          !currentBlock_->body.empty() &&
                          &currentBlock_->body.back() == &instr;
        const bool consumedByBranch =
            !liveOut && currentBlock_ != nullptr &&
            currentBlock_->term.term == ir::Term::CondBr &&
            currentBlock_->term.cond.has_value() &&
            currentBlock_->term.cond->id == result &&
            result < useCount_.size() && useCount_[result] == kSingleUse;
        if (last && consumedByBranch) {
            // The boolean has exactly one consumer, the branch that follows
            // this instruction, so flags are enough.
            emitFastCompareFlags(instr);
            fusedCmp_ = instr.cmp;
            fusedCompareValid_ = true;
            return true;
        }
        const uint8_t home = homeOf(result);
        if (home == static_cast<uint8_t>(Register::None)) {
            return false;
        }
        emitFastCompareFlags(instr);
        out_ += std::string("  ") + setFor(instr.cmp) + " " +
                regWidth(home, 'b') + "\n";
        out_ += std::string("  movzx ") + regWidth(home, 'd') + ", " +
                regWidth(home, 'b') + "\n";
        return true;
    }

    // The fixed registers an instruction writes as part of its machine
    // sequence rather than as an IR definition. Div/rem own rax (and rdx for
    // the high half), variable shifts own rax and cl. A value homed in one of
    // these registers must be copied out before the instruction and restored
    // afterwards; the allocator currently forbids such homes, but the emitter
    // must stay correct if a later allocator stops avoiding them.
    uint16_t fixedRegisterWrites(const ir::Instr& instr) const {
        if (!fast_) {
            return 0;
        }
        const uint16_t rax = static_cast<uint16_t>(
            1u << static_cast<uint8_t>(Register::Rax));
        const uint16_t rdx = static_cast<uint16_t>(
            1u << static_cast<uint8_t>(Register::Rdx));
        const uint16_t rcx = static_cast<uint16_t>(
            1u << static_cast<uint8_t>(Register::Rcx));
        if (instr.op == ir::Op::Div || instr.op == ir::Op::Rem) {
            if (instr.width == ir::IRType::F32 ||
                instr.width == ir::IRType::F64) {
                return 0;
            }
            return static_cast<uint16_t>(rax | rdx);
        }
        if (instr.op == ir::Op::Shl || instr.op == ir::Op::Shr) {
            return static_cast<uint16_t>(rax | rcx);
        }
        return 0;
    }

    // The live values whose home is one of the instruction's fixed registers.
    // `currentPos_` is the instruction's RPO position, so a value is live
    // across it exactly when its range starts earlier and ends later. The
    // instruction's own operands end at the position and its result starts
    // there, so neither is saved.
    std::vector<std::pair<uint32_t, uint8_t>> liveFixedHomes(
        uint16_t mask) const {
        std::vector<std::pair<uint32_t, uint8_t>> out;
        if (mask == 0 || currentHome_ == nullptr) {
            return out;
        }
        for (const LiveRange& range : allocation_.live.ranges) {
            if (range.start >= currentPos_ || range.end <= currentPos_) {
                continue;
            }
            if (range.value >= currentHome_->home.size()) {
                continue;
            }
            const uint8_t home = currentHome_->home[range.value];
            if (home == static_cast<uint8_t>(Register::None)) {
                continue;
            }
            if ((mask & (1u << home)) != 0) {
                out.emplace_back(range.value, home);
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    // Count the largest set of fixed-register homes any one division or shift
    // must carry across itself. The frame reserves exactly one scratch word
    // per live value; a result that collides with a guarded home is an
    // allocator inconsistency and is refused rather than silently mis-stored.
    uint32_t fixedScratchNeed(const ir::Function& fn) const {
        if (!fast_) {
            return 0;
        }
        // Most functions contain no division or variable shift at all; skip
        // the range scan entirely rather than walking every live range once
        // per block.
        bool anyGuard = false;
        for (const ir::Block& block : fn.blocks) {
            for (const ir::Instr& instr : block.body) {
                if (fixedRegisterWrites(instr) != 0) {
                    anyGuard = true;
                    break;
                }
            }
            if (anyGuard) {
                break;
            }
        }
        if (!anyGuard) {
            return 0;
        }
        std::vector<uint32_t> bases(fn.blocks.size());
        for (uint32_t at = 0; at < allocation_.live.blockOrder.size(); ++at) {
            const uint32_t block = allocation_.live.blockOrder[at];
            if (block < bases.size()) {
                bases[block] = at * kBlockPositionScale;
            }
        }
        uint32_t need = 0;
        for (uint32_t b = 0; b < fn.blocks.size(); ++b) {
            const BlockAllocation* home =
                b < allocation_.blocks.size() ? &allocation_.blocks[b]
                                              : nullptr;
            if (home == nullptr) {
                continue;
            }
            const ir::Block& block = fn.blocks[b];
            for (uint32_t i = 0; i < block.body.size(); ++i) {
                const uint16_t mask = fixedRegisterWrites(block.body[i]);
                if (mask == 0) {
                    continue;
                }
                const uint32_t pos = bases[b] + i;
                uint32_t live = 0;
                for (const LiveRange& range : allocation_.live.ranges) {
                    if (range.start >= pos || range.end <= pos ||
                        range.value >= home->home.size()) {
                        continue;
                    }
                    const uint8_t reg = home->home[range.value];
                    if (reg != static_cast<uint8_t>(Register::None) &&
                        (mask & (1u << reg)) != 0) {
                        ++live;
                    }
                }
                need = std::max(need, live);
            }
        }
        return need;
    }

    void beginFixedGuard(const ir::Instr& instr) {
        fixedGuardCount_ = 0;
        const uint16_t mask = fixedRegisterWrites(instr);
        if (mask == 0 || fixedScratchCells_ == 0) {
            return;
        }
        const auto live = liveFixedHomes(mask);
        for (const auto& entry : live) {
            if (fixedGuardCount_ >= fixedScratchCells_) {
                break;
            }
            out_ += "  mov qword ptr [rsp + " +
                    std::to_string(fixedScratchBase_ +
                                   fixedGuardCount_ * kWordBytes) +
                    "], " + regRef(entry.second) + "\n";
            fixedGuardRegs_[fixedGuardCount_] = entry.second;
            ++fixedGuardCount_;
        }
    }

    // Restore the saved homes after the instruction's result has been stored.
    // A result home equal to a guarded register would require the register to
    // hold both the caller's value and the result at the same time; the
    // allocator's interference rule forbids that, so encountering it means
    // the home tables disagree. Refuse rather than silently lose a value.
    void endFixedGuard(const ir::Instr& instr) {
        if (fixedGuardCount_ == 0) {
            return;
        }
        if (!instr.results.empty()) {
            const uint8_t resultHome = homeOf(instr.results[0].id);
            for (uint32_t i = 0; i < fixedGuardCount_; ++i) {
                if (fixedGuardRegs_[i] == resultHome) {
                    fixedGuardCount_ = 0;
                    refuse(instr.loc,
                           "fixed-register guard result collides with a live "
                           "home");
                    return;
                }
            }
        }
        for (uint32_t i = 0; i < fixedGuardCount_; ++i) {
            out_ += "  mov " + regRef(fixedGuardRegs_[i]) +
                    ", qword ptr [rsp + " +
                    std::to_string(fixedScratchBase_ + i * kWordBytes) +
                    "]\n";
        }
        fixedGuardCount_ = 0;
    }

    // Appends a displacement to an address string, omitting +0: `[rip + g+0]`
    // assembles, but the plain form reads better and diffs cleaner.
    std::string atAddr(const std::string& base, int64_t disp) const {
        if (disp == 0) {
            return "[" + base + "]";
        }
        return "[" + base + " + " + std::to_string(disp) + "]";
    }

    // A GEP whose address is consumed once, by the very next Load or Store in
    // the same block, does not need its own result cell. Its alloca base,
    // index and byte displacement fold straight into the memory operand:
    // `[rsp + frame + index + displacement]`. The index keeps its home because
    // the skipped GEP was the only instruction between it and the access, so
    // no register can be reused in between. `scratch` is a register the
    // consuming instruction already clobbers (rax for Load, rcx for Store); it
    // is used only when the index itself has no register home.
    bool foldedMemoryOperand(const ir::Instr& instr, const char* scratch,
                             std::string& mem) {
        if (!fast_) {
            return false;
        }
        auto found = foldedGep_.find(instr.addressBase.id);
        if (found == foldedGep_.end()) {
            return false;
        }
        const ir::Instr& gep = *found->second;
        std::string base = allocaBase(gep.addressBase.id);
        int64_t disp = gep.displacement + instr.displacement;
        std::string index;
        if (!gep.operands.empty()) {
            const uint32_t indexId = gep.operands[0].id;
            int64_t immediate = 0;
            if (immediateRef(indexId, &immediate)) {
                disp += immediate;
            } else {
                const uint8_t home = homeOf(indexId);
                if (home != static_cast<uint8_t>(Register::None)) {
                    index = regRef(home);
                } else {
                    out_ += "  mov " + std::string(scratch) + ", " +
                            slot(indexId) + "\n";
                    index = scratch;
                }
            }
        }
        if (index.empty()) {
            mem = atAddr(base, disp);
            return true;
        }
        std::string operand = "[" + base + " + " + index;
        if (disp != 0) {
            operand += " + " + std::to_string(disp);
        }
        operand += "]";
        mem = operand;
        return true;
    }

    // True for basic kinds that read back sign-extended. Pointers read as
    // addresses: 8 bytes, no extension question.
    bool signedRead(TypeRef type) const {
        if (type.isError() || type.type->kind() != Kind::Basic) {
            return false;
        }
        return basicIsSigned(*type.type->basic());
    }

    // Loads exactly the bytes a type names into rax (xmm0 for floats),
    // extending to 64 bits on the way. The fast emitter may name the
    // allocator's home instead, so a homed result needs no following copy;
    // x86 reads the effective address before writing the destination, so the
    // home may alias the address or a folded index register.
    void emitLoadMem(const std::string& mem, TypeRef type, ir::IRType width,
                     uint8_t home = static_cast<uint8_t>(Register::None)) {
        if (width == ir::IRType::F32) {
            out_ += "  movss xmm0, " + mem + "\n";
            return;
        }
        if (width == ir::IRType::F64) {
            out_ += "  movsd xmm0, " + mem + "\n";
            return;
        }
        bool isSigned = signedRead(type);
        // `emitLoadMem` is shared with legacy; change only the fast path.
        const bool direct = fast_ && home != static_cast<uint8_t>(Register::None);
        const std::string q = direct ? regWidth(home, 'q') : "rax";
        const std::string d = direct ? regWidth(home, 'd') : "eax";
        if (fast_ && (width == ir::IRType::I8 || width == ir::IRType::U8 ||
                      width == ir::IRType::Bool)) {
            out_ += isSigned ? "  movsx " + q + ", BYTE PTR "
                             : "  movzx " + d + ", BYTE PTR ";
            out_ += mem + "\n";
            return;
        }
        if (fast_ && (width == ir::IRType::I16 || width == ir::IRType::U16)) {
            out_ += isSigned ? "  movsx " + q + ", WORD PTR "
                             : "  movzx " + d + ", WORD PTR ";
            out_ += mem + "\n";
            return;
        }
        // Legacy keeps its existing two-step extension below.
        if (width == ir::IRType::I8 || width == ir::IRType::U8 ||
            width == ir::IRType::Bool) {
            out_ += isSigned ? "  movsx eax, BYTE PTR " : "  movzx eax, BYTE PTR ";
            out_ += mem + "\n";
            out_ += "  movsxd rax, eax\n";
            return;
        }
        if (width == ir::IRType::I16 || width == ir::IRType::U16) {
            out_ += isSigned ? "  movsx eax, WORD PTR " : "  movzx eax, WORD PTR ";
            out_ += mem + "\n";
            out_ += "  movsxd rax, eax\n";
            return;
        }
        if (width == ir::IRType::I32) {
            out_ += "  movsxd " + q + ", DWORD PTR " + mem + "\n";
            return;
        }
        if (width == ir::IRType::U32) {
            // A zero-extending dword load: the cell holds the value in
            // its low four bytes, and a qword load would also pick up
            // whatever shares the slot (a packed array neighbour), so
            // the neighbour's bytes would corrupt the comparison. A
            // dword load through a 32-bit destination zeroes the top
            // half by itself.
            out_ += "  mov " + d + ", DWORD PTR " + mem + "\n";
            return;
        }
        out_ += "  mov " + q + ", " + mem + "\n";
    }

    // Stores the low bytes of a source register (xmm0 for floats) with exactly
    // the width a type names, so a narrow store never clobbers neighbor bytes.
    // The fast emitter may name the value's home instead of copying it to rax
    // first; the address scratch is rcx, so a caller never passes rcx.
    void emitStoreMem(const std::string& mem, TypeRef type, ir::IRType width,
                      uint8_t source =
                          static_cast<uint8_t>(Register::None)) {
        if (width == ir::IRType::F32) {
            out_ += "  movss " + mem + ", xmm0\n";
            return;
        }
        if (width == ir::IRType::F64) {
            out_ += "  movsd " + mem + ", xmm0\n";
            return;
        }
        const bool direct = source != static_cast<uint8_t>(Register::None);
        const std::string q = direct ? regWidth(source, 'q') : "rax";
        const std::string d = direct ? regWidth(source, 'd') : "eax";
        const std::string w = direct ? regWidth(source, 'w') : "ax";
        const std::string b = direct ? regWidth(source, 'b') : "al";
        if (width == ir::IRType::I8 || width == ir::IRType::U8 ||
            width == ir::IRType::Bool) {
            out_ += "  mov " + mem + ", " + b + "\n";
            return;
        }
        if (width == ir::IRType::I16 || width == ir::IRType::U16) {
            out_ += "  mov " + mem + ", " + w + "\n";
            return;
        }
        if (width == ir::IRType::I32 || width == ir::IRType::U32) {
            out_ += "  mov " + mem + ", " + d + "\n";
            return;
        }
        out_ += "  mov " + mem + ", " + q + "\n";
    }

    // The width spelling for an immediate-to-memory store. A float or unknown
    // width has no integer immediate form and keeps the register path.
    const char* immediateStoreWidth(ir::IRType width) const {
        switch (width) {
            case ir::IRType::I8:
            case ir::IRType::U8:
            case ir::IRType::Bool:
                return "byte ptr ";
            case ir::IRType::I16:
            case ir::IRType::U16:
                return "word ptr ";
            case ir::IRType::I32:
            case ir::IRType::U32:
                return "dword ptr ";
            case ir::IRType::I64:
            case ir::IRType::U64:
            case ir::IRType::Ptr:
                return "qword ptr ";
            default:
                return nullptr;
        }
    }

    // The legacy frame's first slot id past every value of the function being
    // emitted: its call path stages spilled argument words there, and because
    // no IR value owns the id, staging cannot clobber the live word it copies
    // from. The fast frame uses its outgoing area instead.
    uint32_t scratchBase_ = 0;

    std::string labelFor(const std::string& fn, uint32_t label) const {
        return ".L_" + fn + "_" + std::to_string(label);
    }

    // Deterministic label for an out-of-line trap stub. The sequence is
    // module-wide because the assembler rejects a repeated `.L_cider_trap_N`
    // anywhere in one file, even though `.L_` labels are local symbols.
    std::string trapStubLabel() {
        return ".L_cider_trap_" + std::to_string(trapStubSeq_++);
    }

    bool emitFunction(const ir::Function& fn) {
        // Fast traps are accumulated here and appended after the last block.
        // Clear the text at entry so no function can inherit another's stubs;
        // the label sequence stays module-wide so labels never repeat.
        coldTrapText_.clear();
        sawAsm_ = false;
        // A fast function with at least four returns emits its callee-save
        // restores, frame adjustment, `leave`, and `ret` once. Each `Ret`
        // stages its own return words and jumps here; the last emitted block
        // falls through. Functions with one to three returns keep the local
        // per-return epilogue: deduping a short body costs more in layout than
        // the restores it removes. Legacy always keeps its per-return path.
        uint32_t retCount = 0;
        for (const ir::Block& block : fn.blocks) {
            if (block.term.term == ir::Term::Ret) {
                ++retCount;
            }
        }
        sharedEpilogue_ = fast_ && retCount >= 4;
        sharedEpilogueLabel_ =
            sharedEpilogue_
                ? ".L_" + asmSafeName(fn.name) + "_epilogue"
                : std::string();
        // The fast peepholes rewrite only this function's generated text. The
        // legacy emitter never reaches peepholeFastText, so its output stays
        // byte-identical.
        const uint64_t peepholeStart = out_.size();
        // The legacy frame covers every value id the function mentions,
        // rounded up so `sub rsp` keeps 16-byte alignment for the next call.
        // The fast frame uses the compact layout instead.
        uint32_t top = 0;
        auto note = [&](uint32_t id) {
            top = id > top ? id : top;
        };
        for (const ir::Value& param : fn.params) {
            note(param.id);
        }
        for (const ir::Block& block : fn.blocks) {
            for (const ir::Instr& instr : block.body) {
                for (const ir::Value& result : instr.results) {
                    note(result.id);
                }
                for (const ir::Value& operand : instr.operands) {
                    note(operand.id);
                }
                for (const ir::Argument& arg : instr.arguments) {
                    for (const ir::Value& word : arg.words) {
                        note(word.id);
                    }
                }
            }
            const ir::Terminator& term = block.term;
            for (const ir::Value& word : term.values) {
                note(word.id);
            }
            if (term.cond.has_value()) {
                note(term.cond->id);
            }
        }
        uint32_t cells = top + 1;
        scratchBase_ = cells;

        // A constant with one definition and a 32-bit immediate is available
        // to every use no matter which value was evicted to make pressure fit.
        // Cache it once per function; a homed constant still reads its
        // register, so this path only fills the no-home case.
        rematConstants_.clear();
        foldedAtPos_.clear();
        foldedDefinitions_.clear();
        std::unordered_map<uint32_t, uint32_t> definitions;
        {
            for (const ir::Value& param : fn.params) {
                ++definitions[param.id];
            }
            for (const ir::Block& block : fn.blocks) {
                for (const ir::Instr& instr : block.body) {
                    for (const ir::Value& result : instr.results) {
                        ++definitions[result.id];
                    }
                }
            }
            for (const ir::Block& block : fn.blocks) {
                for (const ir::Instr& instr : block.body) {
                    if (instr.op != ir::Op::Const ||
                        instr.results.size() != kOneResult) {
                        continue;
                    }
                    const ir::Value& result = instr.results[0];
                    const ir::IRType width = result.width;
                    if (width == ir::IRType::Void ||
                        width == ir::IRType::F32 ||
                        width == ir::IRType::F64) {
                        continue;
                    }
                    if (definitions[result.id] != 1) {
                        continue;
                    }
                    if (instr.constantBits < INT32_MIN ||
                        instr.constantBits > INT32_MAX) {
                        continue;
                    }
                    rematConstants_[result.id] = instr.constantBits;
                }
            }
        }

        // A cast that only renames an immediate (an integer widening of a
        // remat constant, say) is itself a constant for the fold: the emitter
        // writes the immediate into the result home, so a consumer can use it
        // directly. Definitions dominate uses, and blocks are walked in
        // emission order, so a cast of a cast resolves in this single pass.
        castConstants_.clear();
        preservingCastIds_.clear();
        for (const ir::Block& block : fn.blocks) {
            for (const ir::Instr& instr : block.body) {
                if (instr.op != ir::Op::Cast ||
                    instr.results.size() != kOneResult ||
                    instr.operands.size() != kOneResult) {
                    continue;
                }
                const uint32_t sourceId = instr.operands[0].id;
                const uint32_t resultId = instr.results[0].id;
                if (definitions[resultId] != 1) {
                    continue;
                }
                int64_t sourceValue = 0;
                bool sourceSafe = false;
                auto source = rematConstants_.find(sourceId);
                if (source != rematConstants_.end()) {
                    sourceValue = source->second;
                    sourceSafe = true;
                } else {
                    auto cast = castConstants_.find(sourceId);
                    if (cast == castConstants_.end()) {
                        continue;
                    }
                    sourceValue = cast->second;
                    sourceSafe =
                        preservingCastIds_.find(sourceId) !=
                        preservingCastIds_.end();
                }
                castConstants_[resultId] = sourceValue;
                if (sourceSafe && castPreservesConstant(instr, sourceValue)) {
                    preservingCastIds_.insert(resultId);
                }
            }
        }

        // The fast compare selector needs two facts the allocator does not
        // publish: how many times each value is used (a boolean with one use
        // that is the following branch can stay in flags) and the constants
        // (so a compare against zero becomes `test` even when the zero was
        // homed in a register). Both are indexed by value id and rebuilt for
        // every fast function; the legacy emitter never consults them.
        if (fast_) {
            useCount_.assign(cells, 0);
            constValues_.clear();
            auto countUse = [&](const ir::Value& value) {
                if (value.id < useCount_.size()) {
                    ++useCount_[value.id];
                }
            };
            for (const ir::Block& block : fn.blocks) {
                for (const ir::Instr& instr : block.body) {
                    for (const ir::Value& operand : instr.operands) {
                        countUse(operand);
                    }
                    if (instr.op == ir::Op::Phi) {
                        for (const ir::PhiIncoming& incoming :
                             instr.phiIncoming) {
                            countUse(incoming.value);
                        }
                    }
                    if (instr.op == ir::Op::Store ||
                        instr.op == ir::Op::Insert) {
                        countUse(instr.storedValue);
                    }
                    if (instr.op == ir::Op::Load ||
                        instr.op == ir::Op::Store ||
                        instr.op == ir::Op::Extract ||
                        instr.op == ir::Op::Insert ||
                        instr.op == ir::Op::Gep) {
                        countUse(instr.addressBase);
                    }
                    if (instr.op == ir::Op::Call) {
                        for (const ir::Argument& argument : instr.arguments) {
                            for (const ir::Value& word : argument.words) {
                                countUse(word);
                            }
                        }
                        if (instr.call.indirect) {
                            countUse(instr.calleeValue);
                        }
                    }
                    if (instr.op == ir::Op::Const &&
                        instr.results.size() == kOneResult) {
                        constValues_[instr.results[0].id] =
                            instr.constantBits;
                    }
                }
                const ir::Terminator& term = block.term;
                for (const ir::Value& word : term.values) {
                    countUse(word);
                }
                if (term.cond.has_value()) {
                    countUse(*term.cond);
                }
                for (const ir::Case& arm : term.cases) {
                    countUse(arm.value);
                }
                if (term.term == ir::Term::Switch) {
                    countUse(term.switchValue);
                }
            }
        }

        // Reserve the spill stage for the widest call: past every live id,
        // padded even, so `sub rsp` below covers the slots already.
        uint64_t widest = 0;
        for (const ir::Block& block : fn.blocks) {
            for (const ir::Instr& instr : block.body) {
                if (instr.op != ir::Op::Call) {
                    continue;
                }
                uint64_t words = 0;
                for (const ir::Argument& arg : instr.arguments) {
                    words += static_cast<uint64_t>(arg.words.size());
                }
                if (words > widest) {
                    widest = words;
                }
            }
        }
        const uint64_t kRegisterRow = 6;
        if (widest > kRegisterRow) {
            uint64_t stages = widest - kRegisterRow;
            stages = (stages + 1) & ~static_cast<uint64_t>(1);
            cells += static_cast<uint32_t>(stages);
        }
        // The allocation decides whether a callee-saved register is used, so
        // it runs before the frame size is fixed. Its save cells sit past the
        // value slots and call stage, and the frame rounds over them: every
        // entry-alignment rule below only sees a span that is still a
        // 16-multiple (plus the native _start word).
        allocation_ = allocateFunction(fn, fast_, fast_);
        settleFastParameters(fn);
        const bool ms = fn.signature.abi() == Abi::MsAbi;
        currentMsAbi_ = ms;
        varargsSave_ = !ms && fn.signature.variadic();
        msVarargs_ = ms && fn.signature.variadic();
        uint32_t span = 0;
        if (fast_) {
            // The compact frame owns the outgoing area, one cell per unhomed
            // value, alloca cells, callee-save cells and the SysV variadic
            // area. The layout and the emitter ask frame.h for the same region
            // bases, so neither can accumulate the span differently.
            frame_ = layoutFrame(fn, allocation_);
            calleeSaveBase_ = frameSaveBase(frame_);
            varargsBase_ = 0;
            if (varargsSave_) {
                varargsBase_ = frameVarargsBase(
                    frame_,
                    static_cast<uint32_t>(allocation_.calleeSaved.size()));
            }
            // Nonvolatile XMM homes get their own cells after the compact
            // frame. SysV has none, so this changes nothing on the native
            // backend; Microsoft x64 rounds the extra bytes back to 16.
            xmmSaveBase_ = frame_.frameBytes;
            const uint32_t xmmSaves =
                static_cast<uint32_t>(allocation_.floats.calleeSaved.size());
            // The fixed-register guard needs cells only when a live value is
            // actually homed in rax/rdx/rcx across a division or shift. The
            // present allocator refuses those homes, so this normally costs
            // no frame bytes at all.
            const uint32_t fixedLive = fixedScratchNeed(fn);
            fixedScratchCells_ = fixedLive;
            fixedScratchBase_ =
                frame_.frameBytes + xmmSaves * kXmmSaveBytes;
            span = (fixedScratchBase_ +
                    fixedScratchCells_ * static_cast<uint32_t>(kWordBytes) +
                    15u) &
                   ~15u;
        } else {
            fixedScratchBase_ = 0;
            fixedScratchCells_ = 0;
            calleeSaveBase_ = cells;
            cells += static_cast<uint32_t>(allocation_.calleeSaved.size());
            // 8.4/VARIADICS.md §6: only a SysV variadic function owns a
            // register-save area. It sits past the value slots, call stage and
            // callee-save cells, inside the same rounded span, so the frame's
            // alignment rule does not change.
            varargsBase_ = 0;
            if (varargsSave_) {
                varargsBase_ = cells * static_cast<uint32_t>(kWordBytes);
                cells += static_cast<uint32_t>(kSysVVarargsSaveCells);
            }
            span = (cells * 8 + 15) & ~15u;
        }
        // A normal SysV callee enters with rsp 8 mod 16 (the caller aligned
        // rsp before its call), so push rbp makes rbp 16-aligned and a
        // 16-multiple span keeps the frame call-aligned. Linux instead enters
        // a native process with rsp 16-aligned, and `_start` has no caller to
        // create that call frame. Reserve one extra word so `_start`'s own
        // calls hand their callees the alignment every normal function
        // expects. PomeOS documents call-like entry alignment and UEFI calls
        // efi_main, so neither gets this adjustment.
        if (target_ == Target::Native &&
            fn.name == target::info(target_).entry) {
            span += layout::kSizePointer;
        }
        fastSpan_ = span;

        if (fn.weak) {
            out_ += ".weak " + asmSafeName(fn.name) + "\n";
        } else if (fn.exportSymbol) {
            out_ += ".globl " + asmSafeName(fn.name) + "\n";
        }
        out_ += asmSafeName(fn.name) + ":\n";
        out_ += "  push rbp\n";
        out_ += "  mov rbp, rsp\n";
        out_ += "  sub rsp, " + std::to_string(span) + "\n";
        // VARIADICS.md §6: the callee preserves the incoming argument state
        // before any user code can clobber it. GPR words go down first, then
        // the eight XMM registers; movdqu keeps the save correct even when the
        // frame puts the 16-byte slots at an 8-byte offset.
        if (varargsSave_) {
            static const char* const kVarargsGpRegs[kSysVIntRegs] = {
                "rdi", "rsi", "rdx", "rcx", "r8", "r9"};
            for (uint64_t i = 0; i < kSysVIntRegs; ++i) {
                out_ += "  mov qword ptr [rsp + " +
                        std::to_string(varargsBase_ +
                                       kSysVVarargsGpBase + i * kWordBytes) +
                        "], " + kVarargsGpRegs[i] + "\n";
            }
            for (uint64_t i = 0; i < kSysVSseRegs; ++i) {
                out_ += "  movdqu xmmword ptr [rsp + " +
                        std::to_string(varargsBase_ + kSysVVarargsFpBase +
                                       i * kSysVVarargsXmmSlot) +
                        "], xmm" + std::to_string(i) + "\n";
            }
        }
        // VARIADICS.md §7: the Microsoft x64 callee homes the four integer
        // argument registers in the caller's shadow space before user code
        // can clobber them. A variadic float's bits are already in the GPR
        // duplicate at the same positional slot, so no XMM home is needed.
        if (msVarargs_) {
            static const char* const kMsHomeRegs[kMsabiIntRegs] = {
                "rcx", "rdx", "r8", "r9"};
            for (uint64_t i = 0; i < kMsabiIntRegs; ++i) {
                out_ += "  mov qword ptr [rbp + " +
                        std::to_string(kMsHomeBase + i * kWordBytes) + "], " +
                        kMsHomeRegs[i] + "\n";
            }
        }
        // Callee-saved homes keep their caller's values in frame cells, so the
        // prologue/epilogue pair never moves rsp and the span above already
        // carries their bytes. rbx is the only pool member today.
        uint32_t saveIndex = 0;
        for (Register reg : allocation_.calleeSaved) {
            out_ += "  mov qword ptr " + saveSlotRef(saveIndex) + ", " +
                    registerName(reg) + "\n";
            ++saveIndex;
        }
        if (fast_) {
            uint32_t xmmSaveIndex = 0;
            for (XmmRegister reg : allocation_.floats.calleeSaved) {
                out_ += "  movdqu xmmword ptr " +
                        xmmSaveSlotRef(xmmSaveIndex) + ", " +
                        xmmRegisterName(reg) + "\n";
                ++xmmSaveIndex;
            }
        }

        // Parameters arrive in registers (then stack); they live in slots
        // from here on, like every other value.
        //
        // 8.4: an msabi function uses one positional slot per argument word.
        // Slot i below four reads rcx/rdx/r8/r9 for an integer word and
        // xmm0..xmm3 for a float word; a later slot reads the caller's stack
        // at [rbp + 48 + (slot - 4) * 8], above the 32-byte shadow space.
        // VARIADICS.md §6: SysV keeps its separate integer and SSE counters.
        if (ms) {
            for (uint64_t w = 0; w < static_cast<uint64_t>(fn.params.size());
                 ++w) {
                const ir::IRType cls = w < fn.paramClasses.size()
                                           ? fn.paramClasses[w]
                                           : ir::IRType::Void;
                if (cls == ir::IRType::F32 || cls == ir::IRType::F64) {
                    const char* move =
                        cls == ir::IRType::F32 ? "movss" : "movsd";
                    if (w < kMsabiSseRegs) {
                        storeParam(fn.params[w].id,
                                   "xmm" + std::to_string(w), true, cls);
                    } else {
                        // A stack home is one eight-byte word holding the
                        // float's bits; read the scalar width into xmm0 first
                        // so neither operand is memory-to-memory.
                        uint64_t stackAt =
                            kMsStackBase + (w - kMsabiIntRegs) * kWordBytes;
                        out_ += "  " + std::string(move) + " xmm0, [rbp + " +
                                std::to_string(stackAt) + "]\n";
                        storeParam(fn.params[w].id, "xmm0", true, cls);
                    }
                    continue;
                }
                if (w < kMsabiIntRegs) {
                    storeParam(fn.params[w].id, argRegMs(w), false,
                               fn.params[w].width);
                } else {
                    uint64_t stackAt =
                        kMsStackBase + (w - kMsabiIntRegs) * kWordBytes;
                    out_ += "  mov rax, [rbp + " + std::to_string(stackAt) +
                            "]\n";
                    storeParam(fn.params[w].id, "rax", false,
                               fn.params[w].width);
                }
            }
        } else {
            uint64_t gpr = 0;
            uint64_t sse = 0;
            for (uint64_t w = 0;
                 w < static_cast<uint64_t>(fn.params.size()); ++w) {
                const ir::IRType cls = w < fn.paramClasses.size()
                                           ? fn.paramClasses[w]
                                           : ir::IRType::Void;
                if (cls == ir::IRType::F32 || cls == ir::IRType::F64) {
                    storeParam(fn.params[w].id,
                               "xmm" + std::to_string(sse), true, cls);
                    ++sse;
                    continue;
                }
                if (gpr < kSysVIntRegs) {
                    storeParam(fn.params[w].id, argReg(gpr), false,
                               fn.params[w].width);
                } else {
                    uint64_t stackAt =
                        2 * kWordBytes + (gpr - kSysVIntRegs) * kWordBytes;
                    out_ += "  mov rax, [rbp + " + std::to_string(stackAt) +
                            "]\n";
                    storeParam(fn.params[w].id, "rax", false,
                               fn.params[w].width);
                }
                ++gpr;
            }
        }

        slotIds_.clear();
        // Value ids restart in every function, so a DataAddr label recorded
        // for one function must never answer for another: main's string
        // address once posed as writeAll's count slot, and the write failed
        // silently with the syscall error ignored.
        addrWords_.clear();
        for (const ir::Block& block : fn.blocks) {
            for (const ir::Instr& instr : block.body) {
                // Only an alloca names slot storage: every other id that
                // reaches an addressBase is a computed address (see the load
                // and store paths), so the set collected here decides the
                // branch, not a list of producer opcodes.
                if (instr.op == ir::Op::Alloca && !instr.results.empty()) {
                    slotIds_.insert(instr.results[0].id);
                }
            }
        }
        // GEP addresses that a single following access consumes fold into the
        // access's memory operand. The producer must be the instruction
        // immediately before the Load/Store in the same block: moving the
        // index's use from the GEP to the access then crosses no instruction,
        // so the allocator cannot have reused the index's home in between.
        // Only an Alloca base is folded; a computed base still needs the
        // null-pointer check the access emits today.
        foldedGep_.clear();
        if (fast_) {
            std::unordered_map<uint32_t, const ir::Instr*> gepAt;
            std::unordered_set<uint32_t> repeated;
            for (const ir::Block& block : fn.blocks) {
                for (const ir::Instr& instr : block.body) {
                    if (instr.op != ir::Op::Gep || instr.results.empty()) {
                        continue;
                    }
                    const uint32_t id = instr.results[0].id;
                    if (!gepAt.emplace(id, &instr).second) {
                        repeated.insert(id);
                    }
                }
            }
            for (const ir::Block& block : fn.blocks) {
                for (uint32_t i = 1; i < block.body.size(); ++i) {
                    const ir::Instr& instr = block.body[i];
                    if (instr.op != ir::Op::Load &&
                        instr.op != ir::Op::Store) {
                        continue;
                    }
                    auto found = gepAt.find(instr.addressBase.id);
                    if (found == gepAt.end() ||
                        repeated.find(instr.addressBase.id) !=
                            repeated.end()) {
                        continue;
                    }
                    const ir::Instr& gep = *found->second;
                    if (&block.body[i - 1] != &gep) {
                        continue;
                    }
                    if (instr.addressBase.id >= useCount_.size() ||
                        useCount_[instr.addressBase.id] != kSingleUse) {
                        continue;
                    }
                    if (slotIds_.find(gep.addressBase.id) ==
                        slotIds_.end()) {
                        continue;
                    }
                    foldedGep_[instr.addressBase.id] = &gep;
                }
            }
        }

        // The allocation already ran: it decided the frame's callee-save
        // cells above. It is an analysis only: the frame and every slot are
        // still present, and a value without a home uses exactly the old
        // stack path.
        std::vector<uint32_t> blockBase(fn.blocks.size());
        for (uint32_t at = 0; at < allocation_.live.blockOrder.size(); ++at) {
            const uint32_t block = allocation_.live.blockOrder[at];
            if (block < blockBase.size()) {
                blockBase[block] = at * kBlockPositionScale;
            }
        }
        for (uint32_t b = 0; b < fn.blocks.size(); ++b) {
            const ir::Block& block = fn.blocks[b];
            // The label emitted immediately after this block's terminator. A
            // conditional branch whose successor is this label falls through
            // instead of jumping, so the terminator needs one branch, not two.
            // The last block has no successor in emission order: its labels are
            // all earlier, and the cold trap stubs follow it.
            nextBlockLabel_ =
                b + 1 < fn.blocks.size()
                    ? labelFor(fn.name, fn.blocks[b + 1].label)
                    : std::string();
            currentHome_ = b < allocation_.blocks.size()
                               ? &allocation_.blocks[b]
                               : nullptr;
            currentFloatHome_ =
                fast_ && b < allocation_.floats.home.size()
                    ? &allocation_.floats.home[b]
                    : nullptr;
            currentBlock_ = &block;
            currentLiveOut_ =
                fast_ && b < allocation_.live.liveOut.size()
                    ? &allocation_.live.liveOut[b]
                    : nullptr;
            fusedCompareValid_ = false;
            out_ += labelFor(fn.name, block.label) + ":\n";
            for (uint32_t i = 0; i < block.body.size(); ++i) {
                const ir::Instr& instr = block.body[i];
                const bool barrier =
                    fast_ &&
                    (instr.op == ir::Op::Call ||
                     instr.op == ir::Op::Asm ||
                     instr.op == ir::Op::Trap ||
                     ((instr.op == ir::Op::Load ||
                       instr.op == ir::Op::Store) &&
                      instr.isVolatile));
                if (barrier) {
                    out_ += std::string(kPeepholeBarrier) + "\n";
                }
                currentPos_ = blockBase[b] + i;
                foldedValueId_ = kInvalidValue;
                if (fast_ && !instr.results.empty() &&
                    (instr.op == ir::Op::Const ||
                     instr.op == ir::Op::Cast)) {
                    const uint32_t id = instr.results[0].id;
                    const uint8_t home = homeOf(id);
                    if (home != static_cast<uint8_t>(Register::None) &&
                        safeConstantValue(id, nullptr)) {
                        // Walk to the constant's first reader. Lowering can
                        // put transparent instructions (a forwarded Load)
                        // between the Const and the operation, so adjacency
                        // in the body is not enough. The allocator reuses the
                        // constant's home for the result only when its live
                        // interval ends at the operation; an intervening
                        // reader or a write to that home rules the fold out.
                        bool crossedBarrier = false;
                        for (uint32_t j = i + 1; j < block.body.size(); ++j) {
                            const ir::Instr& next = block.body[j];
                            if (next.op == ir::Op::Asm ||
                                ((next.op == ir::Op::Load ||
                                  next.op == ir::Op::Store) &&
                                 next.isVolatile)) {
                                // Raw asm and volatile accesses may name a
                                // home without an IR operand. A call fold
                                // must not trust a home read after them.
                                crossedBarrier = true;
                            }
                            if (instructionReads(next, id)) {
                                const bool operand =
                                    next.operands.size() == kBinaryOperands &&
                                    (next.operands[0].id == id ||
                                     next.operands[1].id == id);
                                if (operand && !next.results.empty() &&
                                    home == homeOf(next.results[0].id) &&
                                    fastImmHomeFold(next, nullptr, nullptr)) {
                                    foldedValueId_ = id;
                                } else {
                                    int64_t constant = 0;
                                    if (safeConstantValue(id, &constant) &&
                                        deadImmConsumer(block, j, next, id,
                                                        constant, home,
                                                        crossedBarrier)) {
                                        foldedAtPos_[blockBase[b] + j].insert(
                                            id);
                                        foldedDefinitions_.insert(id);
                                    }
                                }
                                break;
                            }
                            bool overwrites = false;
                            for (const ir::Value& result : next.results) {
                                if (homeOf(result.id) == home) {
                                    overwrites = true;
                                }
                            }
                            if (overwrites) {
                                break;
                            }
                        }
                    }
                }
                if (!emitInstr(fn, instr)) {
                    return false;
                }
                if (barrier) {
                    out_ += std::string(kPeepholeBarrier) + "\n";
                }
            }
            // A terminator trap is the same barrier as a body trap: the trap
            // emission writes fixed registers and issues syscalls, so no
            // peephole may reason across it.
            const bool termBarrier =
                fast_ && block.term.term == ir::Term::Trap;
            if (termBarrier) {
                out_ += std::string(kPeepholeBarrier) + "\n";
            }
            if (!emitTerm(fn, block.term)) {
                return false;
            }
            if (termBarrier) {
                out_ += std::string(kPeepholeBarrier) + "\n";
            }
        }
        currentHome_ = nullptr;
        currentFloatHome_ = nullptr;
        currentBlock_ = nullptr;
        currentLiveOut_ = nullptr;
        nextBlockLabel_.clear();
        fusedCompareValid_ = false;
        // The shared fast epilogue follows the last block. It is emitted
        // before the cold trap stubs: the last block either falls through
        // into it or jumps to it, and the epilogue's `ret` keeps the stubs
        // unreachable.
        if (sharedEpilogue_) {
            out_ += sharedEpilogueLabel_ + ":\n";
            emitReturnEpilogue();
        }
        // Every normal block ends in a terminator, so the accumulated trap
        // stubs are unreachable. Append them after the last block and before
        // the peephole, never inside a block.
        if (fast_ && !coldTrapText_.empty()) {
            out_ += coldTrapText_;
            coldTrapText_.clear();
        }
        if (fast_ && slotMissing_) {
            refuse(Position{"", 1, 1},
                   "the compact frame has no slot for a referenced value");
            return false;
        }
        if (fast_ && !failed_ && out_.size() > peepholeStart) {
            std::string functionText = out_.substr(peepholeStart);
            peepholeFastText(functionText);
            dropDeadCalleeSaves(functionText);
            out_.resize(peepholeStart);
            out_ += functionText;
        }
        sharedEpilogue_ = false;
        sharedEpilogueLabel_.clear();
        return !failed_;
    }

    // VARIADICS.md §6: `va_start` writes the four SysV cursor fields through
    // the operand's pointer. The named counts ride on the instruction, so the
    // target never re-walks the signature: gp_offset steps over every named
    // integer word (including a hidden sret) and fp_offset over every named
    // SSE register. The overflow cursor starts past all named stack words —
    // integer words past the register row first, then any named float that
    // also had to spill — which is why it is not `max(0, namedGp - 6)` alone.
    //
    // VARIADICS.md §7: the Microsoft list uses only the first of the type's
    // 24 bytes. That word is a stack cursor at the first variadic home: inside
    // the four shadow-space slots while a home exists, then the first stack
    // word above them. The rest of the type is left untouched, so a 24-byte
    // VaList keeps one target-independent extent.
    bool emitVaStart(const ir::Instr& instr) {
        if (!varargsSave_ && !msVarargs_) {
            refuse(instr.loc, "va_start needs a variadic function");
            return false;
        }
        if (msVarargs_) {
            const uint64_t named = static_cast<uint64_t>(instr.vaNamedGp) +
                                   static_cast<uint64_t>(instr.vaNamedSse);
            const uint64_t first =
                named < kMsabiIntRegs
                    ? kMsHomeBase + named * kWordBytes
                    : kMsStackBase + (named - kMsabiIntRegs) * kWordBytes;
            out_ += "  mov rax, " + valueRef(instr.operands[0].id) + "\n";
            emitNullCheck("rax");
            out_ += "  lea rcx, [rbp + " + std::to_string(first) + "]\n";
            out_ += "  mov qword ptr [rax], rcx\n";
            return true;
        }
        const uint64_t gpOffset = instr.vaNamedGp * kWordBytes;
        const uint64_t fpOffset =
            kSysVVarargsFpBase + instr.vaNamedSse * kSysVVarargsXmmSlot;
        const uint64_t gpStack =
            instr.vaNamedGp > kSysVIntRegs ? instr.vaNamedGp - kSysVIntRegs
                                           : 0;
        const uint64_t sseStack =
            instr.vaNamedSse > kSysVSseRegs ? instr.vaNamedSse - kSysVSseRegs
                                            : 0;
        const uint64_t overflow =
            2 * kWordBytes + (gpStack + sseStack) * kWordBytes;
        out_ += "  mov rax, " + valueRef(instr.operands[0].id) + "\n";
        emitNullCheck("rax");
        out_ += "  mov dword ptr [rax], " + std::to_string(gpOffset) + "\n";
        out_ += "  mov dword ptr [rax + 4], " + std::to_string(fpOffset) + "\n";
        out_ += "  lea rcx, [rbp + " + std::to_string(overflow) + "]\n";
        out_ += "  mov qword ptr [rax + 8], rcx\n";
        out_ += "  lea rcx, [rsp + " + std::to_string(varargsBase_) + "]\n";
        out_ += "  mov qword ptr [rax + 16], rcx\n";
        return true;
    }

    // A save slot is eight bytes, but the IR value is typed. A C caller
    // writes a negative i32 as `mov esi, 0xffffffff`, leaving the upper half
    // zero, so reading the whole word would turn -1 into 0x00000000ffffffff
    // and answer every signed comparison wrong. Canonicalize the low `width`
    // bytes of rdx the way emitLoadMem extends a memory read. Widths wider
    // than the write (I64/U64/Ptr) and floats keep their word unchanged.
    void canonicalizeVaArg(const ir::Instr& instr) {
        const bool isSigned = ir::signedWidth(instr.width, instr.type);
        switch (instr.width) {
            case ir::IRType::I8:
            case ir::IRType::U8:
            case ir::IRType::Bool:
                out_ += isSigned ? "  movsx rdx, dl\n" : "  movzx edx, dl\n";
                return;
            case ir::IRType::I16:
            case ir::IRType::U16:
                out_ += isSigned ? "  movsx rdx, dx\n" : "  movzx edx, dx\n";
                return;
            case ir::IRType::I32:
                out_ += "  movsxd rdx, edx\n";
                return;
            case ir::IRType::U32:
                out_ += "  mov edx, edx\n";
                return;
            default:
                return;
        }
    }

    // VARIADICS.md §6: one value out of the cursor, then advance it. Integer
    // and pointer arguments consume the next 8-byte GPR save slot while
    // gp_offset < 48; an f64 consumes the next 16-byte XMM slot while
    // fp_offset < 176. Past either row the value comes from the overflow
    // area and the pointer advances by one word. r11 is fixed scratch here
    // (VaArg is a barrier), so the read and the advance cannot collide.
    bool emitVaArg(const ir::Instr& instr) {
        // VARIADICS.md §7: the Microsoft cursor is one stack word. Every
        // argument home is eight bytes, so an f64 reads the same word an
        // integer does and reinterprets its bits; the cursor then advances by
        // one word. r11 is fixed scratch (VaArg is a barrier), and rax holds
        // the list pointer across the read.
        if (msVarargs_) {
            out_ += "  mov rax, " + valueRef(instr.operands[0].id) + "\n";
            emitNullCheck("rax");
            out_ += "  mov rdx, qword ptr [rax]\n";
            out_ += "  mov r11, qword ptr [rdx]\n";
            out_ += "  add rdx, " + std::to_string(kWordBytes) + "\n";
            out_ += "  mov qword ptr [rax], rdx\n";
            out_ += "  mov rdx, r11\n";
            canonicalizeVaArg(instr);
            storeVaArgResult(instr);
            return true;
        }
        const bool floating = instr.width == ir::IRType::F64 ||
                              instr.width == ir::IRType::F32;
        const std::string base =
            ".L_cider_va_arg_" + std::to_string(vaArgSeq_++);
        const std::string done = base + "_done";
        const std::string stack = base + "_stack";
        out_ += "  mov rax, " + valueRef(instr.operands[0].id) + "\n";
        emitNullCheck("rax");
        if (floating) {
            out_ += "  mov ecx, dword ptr [rax + 4]\n";
            out_ += "  cmp ecx, " +
                    std::to_string(kSysVVarargsFpLimit) + "\n";
        } else {
            out_ += "  mov ecx, dword ptr [rax]\n";
            out_ += "  cmp ecx, " +
                    std::to_string(kSysVVarargsGpLimit) + "\n";
        }
        out_ += "  jae " + stack + "\n";
        out_ += "  mov rdx, qword ptr [rax + 16]\n";
        out_ += "  add rdx, rcx\n";
        out_ += "  mov rdx, qword ptr [rdx]\n";
        if (floating) {
            out_ += "  add ecx, " +
                    std::to_string(kSysVVarargsXmmSlot) + "\n";
            out_ += "  mov dword ptr [rax + 4], ecx\n";
        } else {
            out_ += "  add ecx, " + std::to_string(kWordBytes) + "\n";
            out_ += "  mov dword ptr [rax], ecx\n";
        }
        out_ += "  jmp " + done + "\n";
        out_ += stack + ":\n";
        out_ += "  mov rdx, qword ptr [rax + 8]\n";
        out_ += "  mov r11, qword ptr [rdx]\n";
        out_ += "  add rdx, " + std::to_string(kWordBytes) + "\n";
        out_ += "  mov qword ptr [rax + 8], rdx\n";
        out_ += "  mov rdx, r11\n";
        out_ += done + ":\n";
        canonicalizeVaArg(instr);
        storeVaArgResult(instr);
        return true;
    }

    // A va_arg word arrives in rdx as bits. A float result homed in XMM moves
    // there directly; a spilled one keeps the qword cell write the legacy
    // emitter used (an f32's upper half is never read by a typed load).
    void storeVaArgResult(const ir::Instr& instr) {
        if (instr.results.empty()) {
            return;
        }
        const uint32_t id = instr.results[0].id;
        if (instr.width == ir::IRType::F32 ||
            instr.width == ir::IRType::F64) {
            const uint8_t home = floatHomeOf(id);
            if (home != static_cast<uint8_t>(XmmRegister::None)) {
                out_ += instr.width == ir::IRType::F32
                            ? "  movd " + xmmRef(home) + ", edx\n"
                            : "  movq " + xmmRef(home) + ", rdx\n";
                return;
            }
        }
        storeResult(id, "rdx");
    }

    // One argument-register move. A source with a register home names that
    // home so the parallel scheduler can see a destination it must not
    // overwrite; a source without one spells either its frame cell or, for a
    // folded constant, the immediate itself.
    struct ArgMove {
        uint8_t dest = 0;
        uint8_t sourceHome = 0;
        std::string source;
        ir::IRType width = ir::IRType::Void;
    };

    // A parallel register move into the SysV/Microsoft integer argument row.
    // Sources that do not overlap destinations take the direct one-move path;
    // an overlap is ordered so a destination is written only after every
    // remaining move that still needs its old value has read it. A cycle
    // breaks through a caller-saved scratch that no argument reads.
    bool stageGprArguments(std::vector<ArgMove>& moves) {
        std::vector<uint8_t> done(moves.size());
        auto remainingSource = [&](uint8_t reg) {
            for (uint32_t i = 0; i < moves.size(); ++i) {
                if (done[i] == 0 && moves[i].sourceHome == reg) {
                    return true;
                }
            }
            return false;
        };
        auto emitMove = [&](const ArgMove& move) {
            if (move.sourceHome != 0 &&
                move.sourceHome == move.dest) {
                return;
            }
            out_ += "  mov " + regRef(move.dest) + ", " + move.source + "\n";
        };
        for (uint32_t left = static_cast<uint32_t>(moves.size());
             left > kNoItems;) {
            bool progressed = false;
            for (uint32_t i = 0; i < moves.size(); ++i) {
                if (done[i] != 0) {
                    continue;
                }
                // A destination that a remaining move still reads must not
                // be written, even when this move's own source is a frame
                // cell. Only a self-copy is safe with no ordering.
                if (moves[i].sourceHome != moves[i].dest &&
                    remainingSource(moves[i].dest)) {
                    continue;
                }
                emitMove(moves[i]);
                done[i] = 1;
                --left;
                progressed = true;
            }
            if (progressed) {
                continue;
            }
            // Every remaining destination is still a source: break one cycle
            // through a scratch register nobody reads or writes.
            uint32_t cycle = moves.size();
            for (uint32_t i = 0; i < moves.size(); ++i) {
                if (done[i] == 0 && moves[i].sourceHome != 0) {
                    cycle = i;
                    break;
                }
            }
            if (cycle == moves.size()) {
                refuse(Position{"", 1, 1},
                       "argument staging cannot order its register moves");
                return false;
            }
            static const Register kScratch[] = {
                Register::Rax, Register::R10, Register::R11,
                Register::Rdi, Register::Rsi, Register::R8,
                Register::R9, Register::Rcx, Register::Rdx,
            };
            uint8_t scratch = 0;
            for (Register candidate : kScratch) {
                const uint8_t reg = static_cast<uint8_t>(candidate);
                bool used = false;
                // A nonvolatile home may carry a value that crosses this call
                // without being one of its arguments; it must never serve as
                // the cycle scratch.
                for (Register saved : allocation_.calleeSaved) {
                    if (static_cast<uint8_t>(saved) == reg) {
                        used = true;
                        break;
                    }
                }
                for (const ArgMove& move : moves) {
                    if (move.dest == reg || move.sourceHome == reg) {
                        used = true;
                        break;
                    }
                }
                if (!used) {
                    scratch = reg;
                    break;
                }
            }
            if (scratch == 0) {
                refuse(Position{"", 1, 1},
                       "argument staging has no scratch register");
                return false;
            }
            const uint8_t saved = moves[cycle].sourceHome;
            out_ += "  mov " + regRef(scratch) + ", " + regRef(saved) +
                    "\n";
            for (ArgMove& move : moves) {
                if (move.sourceHome == saved) {
                    move.sourceHome = scratch;
                    move.source = regRef(scratch);
                }
            }
        }
        return true;
    }

    // The SSE twin of stageGprArguments. `dest` is the positional XMM index
    // (zero-based), and the scratch always comes from a register that neither
    // an argument reads nor writes.
    bool stageXmmArguments(std::vector<ArgMove>& moves) {
        std::vector<uint8_t> done(moves.size());
        auto xmmEnum = [](uint8_t index) {
            return static_cast<uint8_t>(static_cast<uint8_t>(
                       XmmRegister::Xmm0) + index);
        };
        auto remainingSource = [&](uint8_t reg) {
            for (uint32_t i = 0; i < moves.size(); ++i) {
                if (done[i] == 0 && moves[i].sourceHome == reg) {
                    return true;
                }
            }
            return false;
        };
        auto emitMove = [&](const ArgMove& move) {
            const uint8_t dest = xmmEnum(move.dest);
            if (move.sourceHome == dest) {
                return;
            }
            const char* moveName =
                move.width == ir::IRType::F32 ? "movss" : "movsd";
            out_ += std::string("  ") + moveName + " " + xmmRef(dest) + ", " +
                    move.source + "\n";
        };
        for (uint32_t left = static_cast<uint32_t>(moves.size());
             left > kNoItems;) {
            bool progressed = false;
            for (uint32_t i = 0; i < moves.size(); ++i) {
                if (done[i] != 0) {
                    continue;
                }
                // As in the GPR path, a frame-sourced move must also wait
                // until its destination is no longer a pending source.
                if (moves[i].sourceHome != xmmEnum(moves[i].dest) &&
                    remainingSource(xmmEnum(moves[i].dest))) {
                    continue;
                }
                emitMove(moves[i]);
                done[i] = 1;
                --left;
                progressed = true;
            }
            if (progressed) {
                continue;
            }
            uint32_t cycle = moves.size();
            for (uint32_t i = 0; i < moves.size(); ++i) {
                if (done[i] == 0 && moves[i].sourceHome != 0) {
                    cycle = i;
                    break;
                }
            }
            if (cycle == moves.size()) {
                refuse(Position{"", 1, 1},
                       "float argument staging cannot order its moves");
                return false;
            }
            uint8_t scratch = 0;
            for (int32_t index = 15; index >= 0; --index) {
                const uint8_t candidate =
                    xmmEnum(static_cast<uint8_t>(index));
                bool used = false;
                // Microsoft x64 keeps the whole xmm6-xmm15 row nonvolatile.
                // An msabi function must leave every one of them untouched
                // even when the allocator does not use it as a home, so none
                // may serve as cycle scratch.
                if (currentMsAbi_ &&
                    candidate >= static_cast<uint8_t>(XmmRegister::Xmm6)) {
                    used = true;
                }
                for (XmmRegister saved : allocation_.floats.calleeSaved) {
                    if (static_cast<uint8_t>(saved) == candidate) {
                        used = true;
                        break;
                    }
                }
                for (const ArgMove& move : moves) {
                    if (xmmEnum(move.dest) == candidate ||
                        move.sourceHome == candidate) {
                        used = true;
                        break;
                    }
                }
                if (!used) {
                    scratch = candidate;
                    break;
                }
            }
            if (scratch == 0) {
                refuse(Position{"", 1, 1},
                       "float argument staging has no scratch register");
                return false;
            }
            const uint8_t saved = moves[cycle].sourceHome;
            const char* moveName =
                moves[cycle].width == ir::IRType::F32 ? "movss" : "movsd";
            out_ += std::string("  ") + moveName + " " + xmmRef(scratch) +
                    ", " + xmmRef(saved) + "\n";
            for (ArgMove& move : moves) {
                if (move.sourceHome == saved) {
                    move.sourceHome = scratch;
                    move.source = xmmRef(scratch);
                }
            }
        }
        return true;
    }

    // Write a call's result words home. Two words arrive in rax and rdx, so a
    // home that names the other return register must be written before the
    // register it overwrites is read. The allocator currently gives the first
    // word rax, but the ordering must not depend on that.
    void storeCallResults(const ir::Instr& instr) {
        if (instr.results.empty()) {
            return;
        }
        const uint32_t firstId = instr.results[0].id;
        if (instr.width == ir::IRType::F32 ||
            instr.width == ir::IRType::F64) {
            storeFloatResult(firstId, "xmm0", instr.width);
            if (static_cast<uint64_t>(instr.results.size()) >= kTwoResults) {
                storeResult(instr.results[kSecondResult].id, "rdx");
            }
            return;
        }
        if (static_cast<uint64_t>(instr.results.size()) < kTwoResults) {
            storeResult(firstId, "rax");
            return;
        }
        const uint32_t secondId = instr.results[kSecondResult].id;
        const uint8_t firstHome = homeOf(firstId);
        const uint8_t secondHome = homeOf(secondId);
        const uint8_t rax = static_cast<uint8_t>(Register::Rax);
        const uint8_t rdx = static_cast<uint8_t>(Register::Rdx);
        if (firstHome == rdx && secondHome == rax) {
            out_ += "  xchg rax, rdx\n";
            return;
        }
        if (firstHome == rdx) {
            storeResult(secondId, "rdx");
            storeResult(firstId, "rax");
            return;
        }
        if (secondHome == rax) {
            storeResult(firstId, "rax");
            storeResult(secondId, "rdx");
            return;
        }
        storeResult(firstId, "rax");
        storeResult(secondId, "rdx");
    }

    // The fast call path. Argument register words read their compact spill
    // cells; words past the register row go to the outgoing area at the
    // bottom of the frame instead of being pushed, so rsp never moves and the
    // frame's alignment is the call's alignment. Microsoft x64 uses the same
    // area: 32 bytes of shadow space at its bottom, then the stack words.
    bool emitFastCall(const ir::Instr& instr) {
        const bool ms = instr.call.msabi;
        const bool setAl = instr.call.variadic && !ms;
        const char* calleeStage = setAl ? "r11" : "rax";
        const CallFrameShape shape = callFrameShape(instr);

        struct StagedWord {
            const ir::Value* value;
            ir::IRType width;
        };
        std::vector<StagedWord> stackWords;
        uint64_t gpr = 0;
        uint64_t sse = 0;
        uint64_t position = 0;
        uint64_t vectorRegs = 0;
        if (ms) {
            for (const ir::Argument& arg : instr.arguments) {
                if (isSingleFloatWordArgument(arg)) {
                    if (position >= kMsabiIntRegs) {
                        stackWords.push_back(StagedWord{&arg.words[0],
                                                        arg.width});
                    }
                    ++position;
                    continue;
                }
                for (const ir::Value& word : arg.words) {
                    if (position >= kMsabiIntRegs) {
                        stackWords.push_back(StagedWord{&word,
                                                        ir::IRType::Void});
                    }
                    ++position;
                }
            }
        } else {
            for (const ir::Argument& arg : instr.arguments) {
                if (isSingleFloatWordArgument(arg)) {
                    ++sse;
                    continue;
                }
                for (const ir::Value& word : arg.words) {
                    if (gpr >= kSysVIntRegs) {
                        stackWords.push_back(StagedWord{&word,
                                                        ir::IRType::Void});
                    }
                    ++gpr;
                }
            }
            if (sse > kSysVSseRegs) {
                refuse(instr.loc, "more than eight float arguments");
                return false;
            }
            vectorRegs = sse;
        }

        // The layout and the emitter must agree on the call shape. Refuse
        // before writing anything if the outgoing area cannot hold this
        // call's stack words; a silent drift would overwrite spill or alloca
        // cells above the outgoing area.
        if (static_cast<uint32_t>(stackWords.size()) != shape.stackWords) {
            refuse(instr.loc,
                   "compact frame call shape disagrees with the argument row");
            return false;
        }
        const uint64_t outgoingNeed =
            static_cast<uint64_t>(shape.outgoingBase) +
            static_cast<uint64_t>(stackWords.size()) * kWordBytes;
        if (outgoingNeed > frame_.outgoingBytes) {
            refuse(instr.loc,
                   "compact frame outgoing area is too small for this call");
            return false;
        }

        // Stage the stack words before loading any argument register. rax is
        // free here: the indirect callee target is loaded afterwards, and no
        // argument register owns a source cell.
        const uint32_t stackBase = shape.outgoingBase;
        for (uint32_t at = 0; at < stackWords.size(); ++at) {
            const ir::Value& word = *stackWords[at].value;
            const std::string dest =
                "[rsp + " +
                std::to_string(stackBase +
                               at * static_cast<uint32_t>(kWordBytes)) +
                "]";
            if (stackWords[at].width == ir::IRType::F32 ||
                stackWords[at].width == ir::IRType::F64) {
                const char* move =
                    stackWords[at].width == ir::IRType::F32 ? "movss"
                                                           : "movsd";
                const uint8_t home = floatHomeOf(word.id);
                if (home != static_cast<uint8_t>(XmmRegister::None)) {
                    out_ += "  " + std::string(move) + " " + dest + ", " +
                            xmmRef(home) + "\n";
                } else {
                    out_ += "  mov rax, " + slot(word.id) + "\n";
                    out_ += "  mov qword ptr " + dest + ", rax\n";
                }
                continue;
            }
            const uint8_t home = homeOf(word.id);
            if (home != static_cast<uint8_t>(Register::None)) {
                out_ += "  mov qword ptr " + dest + ", " + regRef(home) +
                        "\n";
            } else {
                out_ += "  mov rax, " + slot(word.id) + "\n";
                out_ += "  mov qword ptr " + dest + ", rax\n";
            }
        }

        // Collect the register arguments as moves first. Lowering has already
        // excluded call words from GPR homes, and a SysV float argument ends
        // at the call barrier so it normally stays in its cell too; staging
        // through this list is what keeps a future allocator honest when a
        // source does occupy another argument's destination register.
        //
        // A constant the dead-home fold elided has no home and no written
        // cell, so its argument move names the immediate instead. The fold
        // admits only register words whose signed 32-bit immediate the
        // `mov reg, imm` staging can encode.
        auto stageSource = [&](ArgMove& move, const ir::Value& word) {
            if (foldedAtCurrent(word.id)) {
                int64_t immediate = 0;
                if (safeConstantValue(word.id, &immediate)) {
                    move.sourceHome = 0;
                    move.source = std::to_string(immediate);
                    return;
                }
            }
            const uint8_t home = homeOf(word.id);
            if (home != static_cast<uint8_t>(Register::None)) {
                move.sourceHome = home;
                move.source = regRef(home);
            } else {
                move.source = slot(word.id);
            }
        };
        std::vector<ArgMove> gprMoves;
        std::vector<ArgMove> xmmMoves;
        std::vector<uint64_t> msFloatPositions;
        if (ms) {
            position = 0;
            for (const ir::Argument& arg : instr.arguments) {
                if (isSingleFloatWordArgument(arg)) {
                    if (position < kMsabiIntRegs) {
                        ArgMove move;
                        move.dest = static_cast<uint8_t>(position);
                        move.width = arg.width;
                        const uint8_t home =
                            floatHomeOf(arg.words[0].id);
                        if (home !=
                            static_cast<uint8_t>(XmmRegister::None)) {
                            move.sourceHome = home;
                            move.source = xmmRef(home);
                        } else {
                            move.source = slot(arg.words[0].id);
                        }
                        xmmMoves.push_back(move);
                        if (instr.call.variadic) {
                            msFloatPositions.push_back(position);
                        }
                    }
                    ++position;
                    continue;
                }
                for (const ir::Value& word : arg.words) {
                    if (position < kMsabiIntRegs) {
                        ArgMove move;
                        move.dest = static_cast<uint8_t>(
                            argRegisterMs(position));
                        stageSource(move, word);
                        gprMoves.push_back(move);
                    }
                    ++position;
                }
            }
        } else {
            gpr = 0;
            uint64_t sseIndex = 0;
            for (const ir::Argument& arg : instr.arguments) {
                if (isSingleFloatWordArgument(arg)) {
                    ArgMove move;
                    move.dest = static_cast<uint8_t>(sseIndex);
                    move.width = arg.width;
                    const uint8_t home = floatHomeOf(arg.words[0].id);
                    if (home != static_cast<uint8_t>(XmmRegister::None)) {
                        move.sourceHome = home;
                        move.source = xmmRef(home);
                    } else {
                        move.source = slot(arg.words[0].id);
                    }
                    xmmMoves.push_back(move);
                    ++sseIndex;
                    continue;
                }
                for (const ir::Value& word : arg.words) {
                    if (gpr < kSysVIntRegs) {
                        ArgMove move;
                        move.dest =
                            static_cast<uint8_t>(argRegister(gpr));
                        stageSource(move, word);
                        gprMoves.push_back(move);
                    }
                    ++gpr;
                }
            }
        }

        if (!stageGprArguments(gprMoves)) {
            return false;
        }
        if (!stageXmmArguments(xmmMoves)) {
            return false;
        }
        if (ms && instr.call.variadic) {
            // Microsoft x64 variadic floats travel in both their positional
            // XMM register and the matching integer home; duplicate only
            // after every XMM destination has been staged.
            for (uint64_t at : msFloatPositions) {
                out_ += "  movq " + std::string(argRegMs(at)) + ", xmm" +
                        std::to_string(at) + "\n";
            }
        }

        if (instr.call.indirect) {
            out_ += "  mov " + std::string(calleeStage) + ", " +
                    slot(instr.calleeValue.id) + "\n";
            emitNullCheck(calleeStage);
        }
        if (setAl) {
            out_ += "  mov al, " + std::to_string(vectorRegs) + "\n";
        }
        if (instr.call.indirect) {
            out_ += "  call " + std::string(calleeStage) + "\n";
        } else {
            out_ += "  call " + asmSafeName(instr.callee) + "\n";
        }
        storeCallResults(instr);
        return true;
    }

    bool emitInstr(const ir::Function& fn, const ir::Instr& instr) {
        if (instr.op == ir::Op::Phi) {
            // The driver runs opt::lowerPhis before generation; reaching one
            // here means a consumer skipped that step rather than that the
            // emitter should silently pick an incoming edge.
            refuse(instr.loc, "phi must be lowered before codegen");
            return false;
        }
        if (instr.op == ir::Op::Const) {
            if (!instr.results.empty()) {
                const uint32_t resultId = instr.results[0].id;
                if (fast_ && (resultId == foldedValueId_ ||
                              foldedDefinitions_.find(resultId) !=
                                  foldedDefinitions_.end())) {
                    // The next instruction folds this immediate into its own
                    // lea / three-operand imul; the constant never needs a
                    // home write.
                    return true;
                }
                const uint8_t floatHome = floatHomeOf(resultId);
                if (floatHome != static_cast<uint8_t>(XmmRegister::None)) {
                    // A float constant is a bit pattern. Materialise the word
                    // through rax, then move it into the XMM home; movd takes
                    // the low 32 bits of an f32 pattern and movq all 64.
                    const bool fits32 =
                        instr.constantBits >= INT32_MIN &&
                        instr.constantBits <= INT32_MAX;
                    out_ += std::string(fits32 ? "  mov " : "  movabs ") +
                            "rax, " + std::to_string(instr.constantBits) +
                            "\n";
                    out_ += instr.width == ir::IRType::F32
                                ? "  movd " + xmmRef(floatHome) + ", eax\n"
                                : "  movq " + xmmRef(floatHome) + ", rax\n";
                    return true;
                }
                const uint8_t home = homeOf(resultId);
                if (home != static_cast<uint8_t>(Register::None)) {
                    // A homed constant needs no rax round trip. The 32-bit
                    // form sign-extends exactly as the rax spelling did.
                    const bool fits32 =
                        instr.constantBits >= INT32_MIN &&
                        instr.constantBits <= INT32_MAX;
                    out_ += std::string(fits32 ? "  mov " : "  movabs ") +
                            regRef(home) + ", " +
                            std::to_string(instr.constantBits) + "\n";
                    return true;
                }
            }
            // 32-bit immediates encode directly; anything wider materialises
            // through rax, because x86 has no 64-bit-immediate-to-memory mov.
            if (instr.constantBits >= INT32_MIN && instr.constantBits <= INT32_MAX) {
                out_ += "  mov rax, " + std::to_string(instr.constantBits) + "\n";
            } else {
                out_ += "  movabs rax, " + std::to_string(instr.constantBits) + "\n";
            }
            if (!instr.results.empty()) {
                storeResult(instr.results[0].id, "rax");
            }
            return true;
        }
        if (instr.op == ir::Op::Copy) {
            if (fast_ && (instr.width == ir::IRType::F32 ||
                          instr.width == ir::IRType::F64)) {
                const uint32_t destId = instr.results[0].id;
                const uint32_t sourceId = instr.operands[0].id;
                if (destId == sourceId) {
                    return true;
                }
                const uint8_t dest = floatHomeOf(destId);
                const uint8_t source = floatHomeOf(sourceId);
                const char* move =
                    instr.width == ir::IRType::F32 ? "movss" : "movsd";
                if (dest != static_cast<uint8_t>(XmmRegister::None)) {
                    if (dest == source) {
                        return true;
                    }
                    out_ += "  " + std::string(move) + " " + xmmRef(dest) +
                            ", " + floatValueRef(sourceId) + "\n";
                } else if (source !=
                           static_cast<uint8_t>(XmmRegister::None)) {
                    out_ += "  " + std::string(move) + " " + slot(destId) +
                            ", " + xmmRef(source) + "\n";
                } else {
                    // SSE has no memory-to-memory form: use the reserved
                    // Xmm0 scratch for a slot-to-slot bit copy.
                    out_ += "  " + std::string(move) + " xmm0, " +
                            slot(sourceId) + "\n";
                    out_ += "  " + std::string(move) + " " + slot(destId) +
                            ", xmm0\n";
                }
                return true;
            }
            const uint8_t dest = effectiveHome(instr.results[0].id);
            const uint8_t source = effectiveHome(instr.operands[0].id);
            if (dest != static_cast<uint8_t>(Register::None)) {
                // A register destination can copy directly from a register
                // or a slot; this is the one-instruction copy path.
                if (dest != source) {
                    out_ += "  mov " + regRef(dest) + ", " +
                            valueRef(instr.operands[0].id) + "\n";
                }
                return true;
            }
            if (source != static_cast<uint8_t>(Register::None)) {
                // A slot destination can take a register source directly.
                out_ += "  mov " + slot(instr.results[0].id) + ", " +
                        regRef(source) + "\n";
                return true;
            }
            out_ += "  mov rax, " + valueRef(instr.operands[0].id) + "\n";
            storeResult(instr.results[0].id, "rax");
            return true;
        }
        if (instr.op == ir::Op::Shl || instr.op == ir::Op::Shr) {
            // The immediate form avoids the cl staging. A count that is not a
            // rematerialised 0..63 immediate falls through to the existing
            // variable-count sequence below.
            if (emitFastBinary(instr)) {
                return true;
            }
        }
        if (instr.op == ir::Op::Add || instr.op == ir::Op::Sub ||
            instr.op == ir::Op::And || instr.op == ir::Op::Or ||
            instr.op == ir::Op::Xor || instr.op == ir::Op::Mul) {
            // Floats compute in xmm0 with the ss/sd opcode their width
            // names; the integer shapes below would add bit patterns.
            if (instr.width == ir::IRType::F32 || instr.width == ir::IRType::F64) {
                if (instr.op == ir::Op::And || instr.op == ir::Op::Or ||
                    instr.op == ir::Op::Xor) {
                    refuse(instr.loc, "this float instruction is not emitted yet");
                    return false;
                }
                const char* suffix =
                    instr.width == ir::IRType::F32 ? "ss" : "sd";
                const char* mnemonic = "add";
                if (instr.op == ir::Op::Sub) {
                    mnemonic = "sub";
                } else if (instr.op == ir::Op::Mul) {
                    mnemonic = "mul";
                }
                const uint32_t leftId = instr.operands[0].id;
                const uint32_t rightId = instr.operands[1].id;
                const uint32_t resultId = instr.results[0].id;
                const uint8_t dest = floatHomeOf(resultId);
                if (dest != static_cast<uint8_t>(XmmRegister::None) &&
                    dest == floatHomeOf(leftId) &&
                    dest != floatHomeOf(rightId)) {
                    out_ += "  " + std::string(mnemonic) + suffix + " " +
                            xmmRef(dest) + ", " + floatValueRef(rightId) +
                            "\n";
                    return true;
                }
                if (dest != static_cast<uint8_t>(XmmRegister::None) &&
                    (instr.op == ir::Op::Add || instr.op == ir::Op::Mul) &&
                    dest == floatHomeOf(rightId) &&
                    dest != floatHomeOf(leftId)) {
                    out_ += "  " + std::string(mnemonic) + suffix + " " +
                            xmmRef(dest) + ", " + floatValueRef(leftId) +
                            "\n";
                    return true;
                }
                if (dest != static_cast<uint8_t>(XmmRegister::None) &&
                    dest != floatHomeOf(rightId)) {
                    out_ += "  mov" + std::string(suffix) + " " +
                            xmmRef(dest) + ", " + floatValueRef(leftId) +
                            "\n";
                    out_ += "  " + std::string(mnemonic) + suffix + " " +
                            xmmRef(dest) + ", " + floatValueRef(rightId) +
                            "\n";
                    return true;
                }
                out_ += "  mov" + std::string(suffix) + " xmm0, " +
                        floatValueRef(leftId) + "\n";
                out_ += "  " + std::string(mnemonic) + suffix + " xmm0, " +
                        floatValueRef(rightId) + "\n";
                storeFloatResult(resultId, "xmm0", instr.width);
                return true;
            }
            if (emitFastBinary(instr)) {
                return true;
            }
            const char* name = "add";
            if (instr.op == ir::Op::Sub) {
                name = "sub";
            } else if (instr.op == ir::Op::And) {
                name = "and";
            } else if (instr.op == ir::Op::Or) {
                name = "or";
            } else if (instr.op == ir::Op::Xor) {
                name = "xor";
            } else if (instr.op == ir::Op::Mul) {
                name = "imul";
            }
            // 64-bit wraps naturally, so Wrap and Undefined emit the same
            // instruction: the mode travelled on the IR for the optimiser,
            // not for a second opcode here.
            // Sub-64-bit arithmetic computes at 64 bits and truncates:
            // a 64-bit sum carries into bits the width does not own, so the
            // result is narrowed with a sign-correct extension. Unsigned
            // 32-bit needs no instruction (eax zeroes the top); signed and
            // narrower widths extend explicitly. 8-bit multiply has no
            // two-operand form, so it uses the implicit al/ax product.
            bool narrow = instr.width == ir::IRType::I32 || instr.width == ir::IRType::U32 ||
                          instr.width == ir::IRType::I16 || instr.width == ir::IRType::U16 ||
                          instr.width == ir::IRType::I8 || instr.width == ir::IRType::U8;
            bool isSigned = ir::signedWidth(instr.width, instr.type);
            bool isMul = instr.op == ir::Op::Mul;
            bool byteMul = isMul && (instr.width == ir::IRType::I8 || instr.width == ir::IRType::U8);
            if (byteMul) {
                // Fixed scratch: the allocator excludes both operands, so
                // their values are always in their frame cells. Reading the
                // slot also keeps a rematerialised constant out of the
                // byte-sized AL destination.
                out_ += "  mov al, " + slot(instr.operands[0].id) + "\n";
                out_ += std::string(isSigned ? "  imul byte ptr " : "  mul byte ptr ") +
                        slot(instr.operands[1].id) + "\n";
                out_ += std::string(isSigned ? "  movsx eax, ax\n" : "  movzx eax, ax\n");
            } else {
                out_ += "  mov rax, " + valueRef(instr.operands[0].id) + "\n";
                out_ += std::string("  ") + name + " rax, " +
                        valueRef(instr.operands[1].id) + "\n";
                if (narrow) {
                    if (instr.width == ir::IRType::I32 && isSigned) {
                        out_ += "  movsxd rax, eax\n";
                    } else if (instr.width == ir::IRType::U32) {
                        // The op above ran 64 bits wide, so the low 32 must
                        // be taken explicitly: writing eax zeroes the top,
                        // which is the wrap the type promises.
                        out_ += "  mov eax, eax\n";
                    } else if (instr.width == ir::IRType::I16) {
                        out_ += std::string(isSigned ? "  movsx eax, ax\n" : "  movzx eax, ax\n");
                    } else if (instr.width == ir::IRType::I8 || instr.width == ir::IRType::U8) {
                        out_ += std::string(isSigned ? "  movsx eax, al\n" : "  movzx eax, al\n");
                    }
                }
            }
            storeResult(instr.results[0].id, "rax");
            return true;
        }
        if (instr.op == ir::Op::Div || instr.op == ir::Op::Rem) {
            // Float division answers inf or NaN and never faults, so it
            // emits the plain SSE divide with no zero check. SSE has no
            // remainder, and the project links no libc to call fmod from,
            // so a float remainder emits the x87 partial remainder: fprem
            // truncates the quotient (C fmod semantics, not the IEEE
            // round-to-nearest remainder) and C2 reports an incomplete
            // reduction, so the loop repeats until exact. A zero divisor
            // raises the masked invalid operation and answers NaN,
            // matching fmod; nothing faults. The divisor loads first so
            // it sits under the dividend, as fprem requires.
            if (instr.width == ir::IRType::F32 || instr.width == ir::IRType::F64) {
                if (instr.op == ir::Op::Rem) {
                    const char* mem =
                        instr.width == ir::IRType::F32 ? "dword ptr " : "qword ptr ";
                    std::string again =
                        ".L_" + fn.name + "_fprem_" +
                        std::to_string(instr.results[0].id);
                    out_ += "  fld " + std::string(mem) +
                            valueRef(instr.operands[1].id) + "\n";
                    out_ += "  fld " + std::string(mem) +
                            valueRef(instr.operands[0].id) + "\n";
                    out_ += again + ":\n";
                    out_ += "  fprem\n";
                    // fstsw, not fnstsw: the store must wait for fprem to
                    // finish, or the C2 read below can be stale and the
                    // loop exits early with a partial remainder.
                    out_ += "  fstsw ax\n";
                    out_ += "  test ah, 4\n";
                    out_ += "  jnz " + again + "\n";
                    out_ += "  fstp " + std::string(mem) +
                            slot(instr.results[0].id) + "\n";
                    out_ += "  fstp st(0)\n";
                    return true;
                }
                const char* suffix =
                    instr.width == ir::IRType::F32 ? "ss" : "sd";
                const uint32_t leftId = instr.operands[0].id;
                const uint32_t rightId = instr.operands[1].id;
                const uint32_t resultId = instr.results[0].id;
                const uint8_t dest = floatHomeOf(resultId);
                if (dest != static_cast<uint8_t>(XmmRegister::None) &&
                    dest == floatHomeOf(leftId) &&
                    dest != floatHomeOf(rightId)) {
                    out_ += "  div" + std::string(suffix) + " " +
                            xmmRef(dest) + ", " + floatValueRef(rightId) +
                            "\n";
                    return true;
                }
                if (dest != static_cast<uint8_t>(XmmRegister::None) &&
                    dest != floatHomeOf(rightId)) {
                    out_ += "  mov" + std::string(suffix) + " " +
                            xmmRef(dest) + ", " + floatValueRef(leftId) +
                            "\n";
                    out_ += "  div" + std::string(suffix) + " " +
                            xmmRef(dest) + ", " + floatValueRef(rightId) +
                            "\n";
                    return true;
                }
                out_ += "  mov" + std::string(suffix) + " xmm0, " +
                        floatValueRef(leftId) + "\n";
                out_ += "  div" + std::string(suffix) + " xmm0, " +
                        floatValueRef(rightId) + "\n";
                storeFloatResult(resultId, "xmm0", instr.width);
                return true;
            }
            // Division runs at the operand width: a widened divide would
            // miss the MIN/-1 fault the spec demands at the value's own
            // width, so 32/16/8-bit use their own dividend/divisor forms
            // and extend the quotient or remainder afterwards. Every form
            // faults on a zero divisor, which is the defined trap.
            beginFixedGuard(instr);
            bool isSigned = ir::signedWidth(instr.width, instr.type);
            bool wantQuotient = instr.op == ir::Op::Div;
            if (instr.width == ir::IRType::I64 || instr.width == ir::IRType::U64) {
                out_ += "  mov rax, " + slot(instr.operands[0].id) + "\n";
                if (isSigned) {
                    out_ += "  cqo\n";
                    out_ += "  idiv qword ptr " + slot(instr.operands[1].id) + "\n";
                } else {
                    out_ += "  xor edx, edx\n";
                    out_ += "  div qword ptr " + slot(instr.operands[1].id) + "\n";
                }
                if (wantQuotient) {
                    storeResult(instr.results[0].id, "rax");
                } else {
                    out_ += "  mov " + slot(instr.results[0].id) + ", rdx\n";
                }
                endFixedGuard(instr);
                return true;
            }
            if (instr.width == ir::IRType::I32 || instr.width == ir::IRType::U32) {
                out_ += "  mov eax, " + slot(instr.operands[0].id) + "\n";
                if (isSigned) {
                    out_ += "  cdq\n";
                    out_ += "  idiv dword ptr " + slot(instr.operands[1].id) + "\n";
                    out_ += wantQuotient ? "  movsxd rax, eax\n" : "  movsxd rdx, edx\n";
                } else {
                    out_ += "  xor edx, edx\n";
                    out_ += "  div dword ptr " + slot(instr.operands[1].id) + "\n";
                }
                if (wantQuotient) {
                    storeResult(instr.results[0].id, "rax");
                } else {
                    out_ += "  mov " + slot(instr.results[0].id) + ", rdx\n";
                }
                endFixedGuard(instr);
                return true;
            }
            if (instr.width == ir::IRType::I16 || instr.width == ir::IRType::U16) {
                out_ += "  mov ax, " + slot(instr.operands[0].id) + "\n";
                if (isSigned) {
                    out_ += "  cwd\n";
                    out_ += "  idiv word ptr " + slot(instr.operands[1].id) + "\n";
                    out_ += std::string(wantQuotient ? "  movsx eax, ax\n" : "  movsx edx, dx\n");
                } else {
                    out_ += "  xor dx, dx\n";
                    out_ += "  div word ptr " + slot(instr.operands[1].id) + "\n";
                    out_ += std::string(wantQuotient ? "  movzx eax, ax\n" : "  movzx edx, dx\n");
                }
                if (wantQuotient) {
                    storeResult(instr.results[0].id, "rax");
                } else {
                    out_ += "  mov " + slot(instr.results[0].id) + ", rdx\n";
                }
                endFixedGuard(instr);
                return true;
            }
            if (instr.width == ir::IRType::I8 || instr.width == ir::IRType::U8) {
                out_ += "  mov al, " + slot(instr.operands[0].id) + "\n";
                if (isSigned) {
                    out_ += "  cbw\n";
                    out_ += "  idiv byte ptr " + slot(instr.operands[1].id) + "\n";
                    out_ += std::string(wantQuotient ? "  movsx eax, al\n" : "  movsx eax, ah\n");
                    storeResult(instr.results[0].id, "rax");
                } else {
                    out_ += "  xor ah, ah\n";
                    out_ += "  div byte ptr " + slot(instr.operands[1].id) + "\n";
                    out_ += std::string(wantQuotient ? "  movzx eax, al\n" : "  movzx eax, ah\n");
                    storeResult(instr.results[0].id, "rax");
                }
                endFixedGuard(instr);
                return true;
            }
            endFixedGuard(instr);
            refuse(instr.loc, "this division width is not emitted yet");
            return false;
        }
        if (instr.op == ir::Op::Shl || instr.op == ir::Op::Shr) {
            // The count lives in cl and the hardware masks it to the
            // operand size (5 bits at 32 bits and below, 6 at 64), which
            // is SEMANTICS §3's documented result — so the shift runs at
            // the value's own width and narrow results extend afterwards.
            beginFixedGuard(instr);
            bool isSigned = ir::signedWidth(instr.width, instr.type);
            const char* acc = "rax";
            const char* ext = nullptr;
            if (instr.width == ir::IRType::I32 || instr.width == ir::IRType::U32) {
                acc = "eax";
                ext = (instr.width == ir::IRType::I32) ? "movsxd rax, eax" : nullptr;
            } else if (instr.width == ir::IRType::I16 || instr.width == ir::IRType::U16) {
                acc = "ax";
                ext = isSigned ? "movsx eax, ax" : "movzx eax, ax";
            } else if (instr.width == ir::IRType::I8 || instr.width == ir::IRType::U8) {
                acc = "al";
                ext = isSigned ? "movsx eax, al" : "movzx eax, al";
            }
            out_ += std::string("  mov ") + acc + ", " + slot(instr.operands[0].id) + "\n";
            out_ += "  mov rcx, " + slot(instr.operands[1].id) + "\n";
            std::string op = "shl ";
            if (instr.op == ir::Op::Shr) {
                op = isSigned ? "sar " : "shr ";
            }
            out_ += "  " + op + acc + ", cl\n";
            if (ext != nullptr) {
                out_ += std::string("  ") + ext + "\n";
            }
            storeResult(instr.results[0].id, "rax");
            endFixedGuard(instr);
            return true;
        }
        if (instr.op == ir::Op::Neg) {
            out_ += "  mov rax, " + valueRef(instr.operands[0].id) + "\n";
            out_ += "  neg rax\n";
            storeResult(instr.results[0].id, "rax");
            return true;
        }
        if (instr.op == ir::Op::BitNot) {
            out_ += "  mov rax, " + valueRef(instr.operands[0].id) + "\n";
            out_ += "  not rax\n";
            storeResult(instr.results[0].id, "rax");
            return true;
        }
        if (instr.op == ir::Op::Not) {
            // `!x` is a boolean, so a fast result with a GPR home can take
            // the flag byte and its zero extension in that same home. The
            // source is tested where it already lives, which also removes
            // the rax staging when the source is homed. A missing result
            // home keeps the rax result fallback, and a missing source home
            // keeps the rax source staging.
            if (fast_) {
                const uint32_t source = instr.operands[0].id;
                const uint8_t sourceHome = homeOf(source);
                if (sourceHome != static_cast<uint8_t>(Register::None)) {
                    const std::string reg = regRef(sourceHome);
                    out_ += "  test " + reg + ", " + reg + "\n";
                } else {
                    out_ += "  mov rax, " + valueRef(source) + "\n";
                    out_ += "  test rax, rax\n";
                }
                const uint8_t home =
                    instr.results.empty()
                        ? static_cast<uint8_t>(Register::None)
                        : homeOf(instr.results[0].id);
                if (home != static_cast<uint8_t>(Register::None)) {
                    out_ += std::string("  setz ") + regWidth(home, 'b') + "\n";
                    out_ += std::string("  movzx ") + regWidth(home, 'd') +
                            ", " + regWidth(home, 'b') + "\n";
                } else {
                    out_ += "  setz al\n";
                    out_ += "  movzx rax, al\n";
                    storeResult(instr.results[0].id, "rax");
                }
                return true;
            }
            out_ += "  mov rax, " + valueRef(instr.operands[0].id) + "\n";
            out_ += "  test rax, rax\n";
            out_ += "  setz al\n";
            out_ += "  movzx rax, al\n";
            storeResult(instr.results[0].id, "rax");
            return true;
        }
        if (instr.op == ir::Op::Cmp) {
            // A float compare runs on xmm with ucomis, whose flags answer
            // IEEE: unordered (NaN) must read false for < <= == >= > and
            // true for !=, so the four flags need their parity guard —
            // `sete` alone would call NaN equal, `setne` alone would call
            // it different... and it is neither: == is false, != is true.
            // The guard turns the parity bit into the missing half.
            TypeRef left = instr.operands[0].type;
            const bool floatCmp =
                !left.isError() && left.type->kind() == Kind::Basic &&
                basicIsFloat(*left.type->basic());
            if (floatCmp) {
                const bool single = *left.type->basic() == BasicKind::F32;
                const char* suffix = single ? "ss" : "sd";
                const char* setcc = setFor(instr.cmp);
                out_ += "  mov" + std::string(suffix) + " xmm0, " +
                        floatValueRef(instr.operands[0].id) + "\n";
                out_ += "  ucomi" + std::string(suffix) + " xmm0, " +
                        floatValueRef(instr.operands[1].id) + "\n";
                out_ += std::string("  ") + setcc + " al\n";
                const std::string name = setcc;
                if (name == "setne") {
                    // Unordered sets ZF, so setne reads false while !=
                    // must read true; setp (parity set) ORs that case in.
                    out_ += "  setp cl\n";
                    out_ += "  or al, cl\n";
                } else if (name == "sete" || name == "setl" ||
                           name == "setle" || name == "setge" ||
                           name == "setb" || name == "setbe") {
                    // Unordered sets ZF (and CF), which would read these
                    // true while every float relation with NaN is false;
                    // setnp masks the unordered case out.
                    out_ += "  setnp cl\n";
                    out_ += "  and al, cl\n";
                }
                out_ += "  movzx rax, al\n";
                storeResult(instr.results[0].id, "rax");
                return true;
            }
            if (emitFastCompare(instr)) {
                return true;
            }
            out_ += "  mov rax, " + valueRef(instr.operands[0].id) + "\n";
            out_ += "  cmp rax, " + valueRef(instr.operands[1].id) + "\n";
            out_ += std::string("  ") + setFor(instr.cmp) + " al\n";
            out_ += "  movzx rax, al\n";
            storeResult(instr.results[0].id, "rax");
            return true;
        }
        // A load or store names its place two ways: a global carries its link
        // label, and an alloca id names a cell whose address comes from the
        // frame's alloca region. One branch, decided by which the instruction
        // names.
        if (instr.op == ir::Op::Load) {
            if (residentParams_.find(instr.addressBase.id) !=
                residentParams_.end()) {
                emitResidentParamLoad(instr);
                return true;
            }
            // A computed address lives in its slot first: the two-step read
            // through rax is what makes an indirect load one load too many
            // for a single instruction, and keeps every memory operand shaped
            // the same way. Only an alloca id names a slot; any other id in
            // an addressBase is an address the IR computed (Gep, address
            // arithmetic, a loaded pointer), never slot storage, so it
            // dereferences. Listing producer opcodes here sprouted a bug per
            // slice; the alloca set closes the class.
            std::string mem;
            if (!instr.globalLabel.empty()) {
                mem = atAddr("rip + " + instr.globalLabel,
                             instr.displacement);
            } else if (foldedMemoryOperand(instr, "rax", mem)) {
                // The alloca base, GEP index and byte displacement folded
                // into one memory operand; no separate address cell is read.
            } else if (slotIds_.find(instr.addressBase.id) != slotIds_.end()) {
                std::string base = allocaBase(instr.addressBase.id);
                mem = atAddr(base, instr.displacement);
            } else {
                out_ += "  mov rax, " + valueRef(instr.addressBase.id) + "\n";
                emitNullCheck("rax");
                mem = atAddr("rax", instr.displacement);
            }
            // The fast emitter writes the allocator's home directly; a load
            // that already wrote the home needs no following copy. Legacy
            // keeps its two-step rax sequence.
            const uint8_t dest =
                fast_ ? homeOf(instr.results[0].id)
                      : static_cast<uint8_t>(Register::None);
            emitLoadMem(mem, instr.type, instr.width, dest);
            if (instr.width == ir::IRType::F32 ||
                instr.width == ir::IRType::F64) {
                storeFloatResult(instr.results[0].id, "xmm0", instr.width);
            } else if (dest == static_cast<uint8_t>(Register::None)) {
                storeResult(instr.results[0].id, "rax");
            }
            return true;
        }
        if (instr.op == ir::Op::Store) {
            // The fast path may store a homed integer value straight from its
            // home; every other path still materialises it in rax.
            uint8_t storeHome = static_cast<uint8_t>(Register::None);
            // A rematerialisable integer constant can be written straight to
            // memory, so the destination operand is computed before any source
            // register is loaded. Floats, resident parameters, volatile stores
            // and non-constant values with homes keep the register path.
            bool directImmediate = false;
            int64_t immediate = 0;
            const char* immediateWidth = nullptr;
            // Each width has its own SSE move: a float store that read four
            // bytes of a double would keep the low half of the bit pattern
            // and answer with the wrong value, so the spellings differ.
            if (!fast_) {
                // The legacy emitter keeps its exact old sequence, including
                // the rax preload that only its slot path needed.
                out_ += "  mov rax, " +
                        valueRef(instr.storedValue.id) + "\n";
                if (instr.width == ir::IRType::F32) {
                    out_ += "  movss xmm0, " +
                            slot(instr.storedValue.id) + "\n";
                } else if (instr.width == ir::IRType::F64) {
                    out_ += "  movsd xmm0, " +
                            slot(instr.storedValue.id) + "\n";
                }
            } else {
                const bool typedFloat =
                    instr.width == ir::IRType::F32 ||
                    instr.width == ir::IRType::F64;
                const bool wordFloat =
                    instr.storedValue.width == ir::IRType::F32 ||
                    instr.storedValue.width == ir::IRType::F64;
                // A rematerialisable integer constant can be written straight
                // to memory even when it also has a register home: the
                // immediate is the same value and the home stays intact for
                // any later use. Non-constant values with homes keep the
                // register path.
                auto remat = rematConstants_.find(instr.storedValue.id);
                if (!typedFloat && !wordFloat && !instr.isVolatile &&
                    residentParams_.find(instr.addressBase.id) ==
                        residentParams_.end() &&
                    remat != rematConstants_.end() &&
                    remat->second >= INT32_MIN &&
                    remat->second <= INT32_MAX) {
                    immediate = remat->second;
                    immediateWidth = immediateStoreWidth(instr.width);
                    directImmediate = immediateWidth != nullptr;
                }
                if (!directImmediate) {
                    if (typedFloat) {
                        const char* move =
                            instr.width == ir::IRType::F32 ? "movss" : "movsd";
                        out_ += "  " + std::string(move) + " xmm0, " +
                                floatValueRef(instr.storedValue.id) + "\n";
                    } else if (wordFloat) {
                        // A `store void` writes the variable's full word. A
                        // homed float must therefore cross from XMM to rax as
                        // bits; the low four bytes keep an f32 pattern and the
                        // upper half is irrelevant to the typed load that
                        // follows.
                        const uint8_t home = floatHomeOf(instr.storedValue.id);
                        if (home != static_cast<uint8_t>(XmmRegister::None)) {
                            out_ += instr.storedValue.width == ir::IRType::F32
                                        ? "  movd eax, " + xmmRef(home) + "\n"
                                        : "  movq rax, " + xmmRef(home) + "\n";
                        } else {
                            out_ += "  mov rax, " +
                                    slot(instr.storedValue.id) + "\n";
                        }
                    } else {
                        const bool residentAddress =
                            residentParams_.find(instr.addressBase.id) !=
                            residentParams_.end();
                        const uint8_t home =
                            homeOf(instr.storedValue.id);
                        if (!instr.isVolatile && !residentAddress &&
                            home != static_cast<uint8_t>(Register::None) &&
                            home != static_cast<uint8_t>(Register::Rcx)) {
                            // The store names the home directly: no rax copy.
                            storeHome = home;
                        } else {
                            out_ += "  mov rax, " +
                                    valueRef(instr.storedValue.id) + "\n";
                        }
                    }
                }
            }
            if (residentParams_.find(instr.addressBase.id) !=
                residentParams_.end()) {
                emitResidentParamStore(instr);
                return true;
            }
            std::string mem;
            if (!instr.globalLabel.empty()) {
                mem = atAddr("rip + " + instr.globalLabel,
                             instr.displacement);
            } else if (foldedMemoryOperand(instr, "rcx", mem)) {
                // Folded as in the load path; rcx is already Store scratch.
            } else if (slotIds_.find(instr.addressBase.id) != slotIds_.end()) {
                std::string base = allocaBase(instr.addressBase.id);
                mem = atAddr(base, instr.displacement);
            } else {
                out_ += "  mov rcx, " + valueRef(instr.addressBase.id) + "\n";
                emitNullCheck("rcx");
                mem = atAddr("rcx", instr.displacement);
            }
            if (directImmediate) {
                out_ += "  mov " + std::string(immediateWidth) + mem + ", " +
                        std::to_string(immediate) + "\n";
                return true;
            }
            emitStoreMem(mem, instr.type, instr.width, storeHome);
            return true;
        }
        if (instr.op == ir::Op::Gep) {
            if (fast_ && !instr.results.empty() &&
                foldedGep_.find(instr.results[0].id) !=
                    foldedGep_.end()) {
                // The consuming load/store names the folded memory operand
                // directly; emitting this address would only waste a cell.
                return true;
            }
            // Base slot plus byte offset, added at runtime: the base id is a
            // compile-time constant, so the slot address folds into the lea.
            // A base that is not a slot holds a computed address as its
            // value instead (a materialized global label, for example),
            // and moves like every other computed address does.
            if (slotIds_.count(instr.addressBase.id) != 0) {
                out_ += "  lea rax, " + atAddr(allocaBase(instr.addressBase.id),
                                               instr.displacement) + "\n";
            } else {
                out_ += "  mov rax, " + valueRef(instr.addressBase.id) + "\n";
                if (instr.displacement != 0) {
                    out_ += "  add rax, " + std::to_string(instr.displacement) + "\n";
                }
            }
            out_ += "  mov rcx, " + valueRef(instr.operands[0].id) + "\n";
            out_ += "  add rax, rcx\n";
            storeResult(instr.results[0].id, "rax");
            return true;
        }
        if (instr.op == ir::Op::Alloca) {
            // No code: the alloca cell's address is fixed by the frame, so
            // every load and store already knows where it lives.
            return true;
        }
        if (instr.op == ir::Op::VaStart) {
            return emitVaStart(instr);
        }
        if (instr.op == ir::Op::VaArg) {
            return emitVaArg(instr);
        }
        if (instr.op == ir::Op::Call) {
            if (fast_) {
                return emitFastCall(instr);
            }
            // SysV counts its two register classes separately: integer words
            // take the six integer registers (a string occupies two, so six
            // string arguments would silently share r9 without this count)
            // and float arguments take an SSE register each. Microsoft x64
            // gives every argument word one positional slot instead: a float
            // in slot i uses xmm{i} and, for a variadic call, also duplicates
            // its bits into that slot's GPR home (VARIADICS.md §7). SysV's
            // counts refuse before anything is emitted, so a refusal never
            // leaves half a call staged.
            const bool ms = instr.call.msabi;
            // Every slot past the fourth goes on the stack. The count is
            // known before staging so the spill pad is fixed.
            uint64_t stackWords = 0;
            uint64_t stackPad = 0;
            // SysV's `al` count: the number of vector registers the call
            // used. Only the SysV branch fills it.
            uint64_t vectorRegs = 0;
            std::vector<const ir::Value*> spilled;
            if (ms) {
                uint64_t slots = 0;
                for (const ir::Argument& arg : instr.arguments) {
                    const bool floatArg = arg.width == ir::IRType::F32 ||
                                          arg.width == ir::IRType::F64;
                    slots += floatArg && arg.words.size() == kFloatWord
                                 ? kFloatWord
                                 : static_cast<uint64_t>(arg.words.size());
                }
                stackWords =
                    slots > kMsabiIntRegs ? slots - kMsabiIntRegs : 0;
                stackPad = stackWords % 2;
                uint64_t position = 0;
                for (const ir::Argument& arg : instr.arguments) {
                    const bool floatArg = arg.width == ir::IRType::F32 ||
                                          arg.width == ir::IRType::F64;
                    if (floatArg && arg.words.size() == kFloatWord) {
                        const char* move =
                            arg.width == ir::IRType::F32 ? "movss" : "movsd";
                        if (position < kMsabiSseRegs) {
                            out_ += "  " + std::string(move) + " xmm" +
                                    std::to_string(position) + ", " +
                                    slot(arg.words[0].id) + "\n";
                            if (instr.call.variadic) {
                                // The callee's va_arg reads the eight-byte
                                // home slot, not the XMM register, so the bit
                                // pattern must also land in the slot's GPR.
                                out_ += "  movq " + std::string(argRegMs(position)) +
                                        ", xmm" + std::to_string(position) + "\n";
                            }
                        } else {
                            spilled.push_back(&arg.words[0]);
                        }
                        ++position;
                        continue;
                    }
                    for (const ir::Value& word : arg.words) {
                        if (position < kMsabiIntRegs) {
                            out_ += "  mov " + std::string(argRegMs(position)) +
                                    ", " + slot(word.id) + "\n";
                        } else {
                            spilled.push_back(&word);
                        }
                        ++position;
                    }
                }
            } else {
                uint64_t gpr = 0;
                uint64_t sse = 0;
                for (const ir::Argument& arg : instr.arguments) {
                    const bool floatArg = arg.width == ir::IRType::F32 ||
                                          arg.width == ir::IRType::F64;
                    if (floatArg) {
                        sse += 1;
                    } else {
                        gpr += static_cast<uint64_t>(arg.words.size());
                    }
                }
                // Integer words past the sixth register spill to the stack
                // the SysV way: pushed in reverse, argument seven nearest
                // the return address, with a lone final word padded to
                // sixteen.
                stackWords =
                    gpr > kSysVIntRegs ? gpr - kSysVIntRegs : 0;
                stackPad = stackWords % 2;
                if (sse > kSysVSseRegs) {
                    refuse(instr.loc, "more than eight float arguments");
                    return false;
                }
                vectorRegs = sse;
                // Integer words flatten in order: a string arrives as (data,
                // length) in (rdi, rsi), which is the SysV two-eightbyte rule
                // and exactly what the C struct on the other side declares.
                // A float argument loads its word into the next SSE register
                // instead of consuming an integer one.
                uint64_t index = 0;
                uint64_t sseIndex = 0;
                for (const ir::Argument& arg : instr.arguments) {
                    const bool floatArg = arg.width == ir::IRType::F32 ||
                                          arg.width == ir::IRType::F64;
                    if (floatArg && arg.words.size() == kFloatWord) {
                        const char* move =
                            arg.width == ir::IRType::F32 ? "movss" : "movsd";
                        out_ += "  " + std::string(move) + " xmm" +
                                std::to_string(sseIndex) + ", " +
                                slot(arg.words[0].id) + "\n";
                        ++sseIndex;
                        continue;
                    }
                    for (const ir::Value& word : arg.words) {
                        if (index < kSysVIntRegs) {
                            out_ += "  mov " + std::string(argReg(index)) +
                                    ", " + slot(word.id) + "\n";
                        } else {
                            spilled.push_back(&word);
                        }
                        ++index;
                    }
                }
            }
            // An indirect callee names a slot, and slot offsets read against
            // rsp: the address is taken before any push or shadow moves rsp.
            // A non-variadic call keeps it in rax through the pushes. A SysV
            // variadic call must set `al` just before the call, so its target
            // is staged in r11 instead and rax stays free for the count.
            const bool setAl = instr.call.variadic && !ms;
            const bool stageInR11 = instr.call.indirect && setAl;
            const char* calleeStage = stageInR11 ? "r11" : "rax";
            const uint64_t stageBase =
                instr.call.indirect && !stageInR11 ? 1 : 0;
            if (instr.call.indirect) {
                out_ += "  mov " + std::string(calleeStage) + ", " +
                        slot(instr.calleeValue.id) + "\n";
                emitNullCheck(calleeStage);
            }
            if (stackWords != 0 || stackPad != 0) {
                // The spill goes down as pushes and nothing else: the
                // frame's slot offsets are static, so every read below
                // sees rsp where the frame left it, and the only movement
                // is the pushes themselves, which no slot read crosses.
                // A lone final word pads the stack back to sixteen so the
                // callee enters aligned.
                // Every word is read into its register BEFORE the pushes
                // start: `mov rax, [rsp + N]` recomputes its operand against
                // the rsp the moment it runs, so a read that follows a push
                // would take a word the push moved. A register per spilled
                // word then pushes in reverse, argument seven landing
                // nearest the return address.
                // T4: the count a loop walks from is written as a name, not
                // a literal beside a size word.
                static constexpr uint64_t skipNone = 0;
                // The row's extent is written once and names the array.
                static constexpr uint64_t kSpillRegs = 7;
                static const char* const kSpillReg[kSpillRegs] = {
                    "rax", "r10", "r11", "r12", "r13", "r14", "r15"};
                // A variadic indirect call reserves r11 for the target, so
                // its spill row skips that slot and rax takes the first word.
                static constexpr uint64_t kSpillRegsNoR11 = kSpillRegs - 1;
                static const char* const kSpillRegNoR11[kSpillRegsNoR11] = {
                    "rax", "r10", "r12", "r13", "r14", "r15"};
                const char* const* spillRow =
                    stageInR11 ? kSpillRegNoR11 : kSpillReg;
                const uint64_t spillCap =
                    stageInR11 ? kSpillRegsNoR11 : kSpillRegs;
                if (spilled.size() + stageBase > spillCap) {
                    refuse(instr.loc, "more argument words than the spill stage holds");
                    return false;
                }
                for (uint64_t at = 0; at < spilled.size(); ++at) {
                    out_ += "  mov " + std::string(spillRow[at + stageBase]) + ", " +
                            slot(spilled[at]->id) + "\n";
                }
                if (stackPad != 0) {
                    // An immediate, not rax: rax may hold the staged spill or
                    // the indirect callee, and a pad push must clobber neither.
                    out_ += "  push 0\n";
                }
                for (uint64_t at = spilled.size(); at > skipNone; --at) {
                    out_ += "  push " + std::string(spillRow[at - 1 + stageBase]) + "\n";
                }
            }
            // 8.4: the caller owns the callee's 32 bytes of shadow space. It
            // goes down after the pushes so the spilled words sit above it,
            // where an msabi callee reads its fifth word.
            if (ms) {
                out_ += "  sub rsp, " + std::to_string(kShadowBytes) + "\n";
            }
            // VARIADICS.md §6: a SysV variadic caller states how many vector
            // registers the call used. Nothing staged for the call depends on
            // rax any more: a direct call needs no target register, and an
            // indirect one holds its target in r11.
            if (setAl) {
                out_ += "  mov al, " + std::to_string(vectorRegs) + "\n";
            }
            if (instr.call.indirect) {
                out_ += "  call " + std::string(calleeStage) + "\n";
            } else {
                out_ += "  call " + asmSafeName(instr.callee) + "\n";
            }
            // The spill comes off the stack BEFORE the answer is stored:
            // a slot offset stands still, and while the pushed words are
            // still down there rsp names a different base for them.
            if (stackWords != 0 || stackPad != 0 || ms) {
                out_ += "  add rsp, " +
                        std::to_string(stackWords * 8 + stackPad * 8 +
                                       (ms ? kShadowBytes : 0)) + "\n";
            }
            if (!instr.results.empty()) {
                if (instr.width == ir::IRType::F32) {
                    out_ += "  movss " + slot(instr.results[0].id) + ", xmm0\n";
                } else if (instr.width == ir::IRType::F64) {
                    out_ += "  movsd " + slot(instr.results[0].id) + ", xmm0\n";
                } else {
                    storeResult(instr.results[0].id, "rax");
                }
                // The register row's second word comes home in rdx (IR.md
                // §6): lowering refuses a third, so stopping here is
                // complete, not truncation.
                if (static_cast<uint64_t>(instr.results.size()) >= kTwoResults) {
                    storeResult(instr.results[kSecondResult].id, "rdx");
                }
            }
            return true;
        }
        if (instr.op == ir::Op::Cast) {
            if (fast_ && !instr.results.empty() &&
                (instr.results[0].id == foldedValueId_ ||
                 foldedDefinitions_.find(instr.results[0].id) !=
                     foldedDefinitions_.end())) {
                // The next instruction folds this immediate into its own lea
                // / three-operand imul; the cast never needs a home write.
                return true;
            }
            return emitCast(instr);
        }
        if (instr.op == ir::Op::Trap) {
            return emitTrapReason(instr.reason);
        }
        if (instr.op == ir::Op::DataAddr) {
            out_ += "  lea rax, [rip + " + asmSafeName(instr.globalLabel) + "]\n";
            storeResult(instr.results[0].id, "rax");
            addrWords_[instr.results[0].id] = asmSafeName(instr.globalLabel);
            return true;
        }
        if (instr.op == ir::Op::FnAddr) {
            out_ += "  lea rax, [rip + " + asmSafeName(instr.globalLabel) + "]\n";
            storeResult(instr.results[0].id, "rax");
            return true;
        }
        if (instr.op == ir::Op::Asm) {
            sawAsm_ = true;
            if (fast_) {
                out_ += std::string(kPeepholeAsmBegin) + "\n";
            }
            emitAsm(instr);
            if (fast_) {
                out_ += std::string(kPeepholeAsmEnd) + "\n";
            }
            return true;
        }
        refuse(instr.loc, "this instruction is not emitted yet");
        return false;
    }

    // The size an asm placeholder asks for: a `{name:q}`-style suffix
    // overrides, and plain `{name}` is the value's own width, always with
    // an explicit PTR so both operands of the template stay unambiguous.
    // GAS parses certain spellings as registers in operand position
    // (`call cs` reads the segment register, never the symbol): a
    // function carrying one of those spellings is renamed at every site
    // that prints it. All other spellings — instruction mnemonics like
    // `add` included — print unchanged.
    bool isMachineReg(const std::string& spelling) {
        std::string low;
        for (char c : spelling) {
            low.push_back(c >= 'A' && c <= 'Z'
                              ? static_cast<char>(c + ('a' - 'A'))
                              : c);
        }
        static const char* const regs[] = {
            "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
            "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
            "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp",
            "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d",
            "ax", "bx", "cx", "dx", "si", "di", "bp", "sp",
            "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w",
            "al", "bl", "cl", "dl", "sil", "dil", "bpl", "spl",
            "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b",
            "ah", "bh", "ch", "dh",
            "rip", "eip",
            "cs", "ds", "es", "ss", "fs", "gs",
            "cr0", "cr1", "cr2", "cr3", "cr4", "cr5", "cr6", "cr7",
            "cr8", "cr9", "cr10", "cr11", "cr12", "cr13", "cr14", "cr15",
            "dr0", "dr1", "dr2", "dr3", "dr4", "dr5", "dr6", "dr7",
            "dr8", "dr9", "dr10", "dr11", "dr12", "dr13", "dr14", "dr15",
            "mm0", "mm1", "mm2", "mm3", "mm4", "mm5", "mm6", "mm7",
            "st", "st0", "st1", "st2", "st3", "st4", "st5", "st6", "st7",
            "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
            "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15",
            "xmm16", "xmm17", "xmm18", "xmm19", "xmm20", "xmm21", "xmm22", "xmm23",
            "xmm24", "xmm25", "xmm26", "xmm27", "xmm28", "xmm29", "xmm30", "xmm31",
            "ymm0", "ymm1", "ymm2", "ymm3", "ymm4", "ymm5", "ymm6", "ymm7",
            "ymm8", "ymm9", "ymm10", "ymm11", "ymm12", "ymm13", "ymm14", "ymm15",
            "ymm16", "ymm17", "ymm18", "ymm19", "ymm20", "ymm21", "ymm22", "ymm23",
            "ymm24", "ymm25", "ymm26", "ymm27", "ymm28", "ymm29", "ymm30", "ymm31",
            "zmm0", "zmm1", "zmm2", "zmm3", "zmm4", "zmm5", "zmm6", "zmm7",
            "zmm8", "zmm9", "zmm10", "zmm11", "zmm12", "zmm13", "zmm14", "zmm15",
            "zmm16", "zmm17", "zmm18", "zmm19", "zmm20", "zmm21", "zmm22", "zmm23",
            "zmm24", "zmm25", "zmm26", "zmm27", "zmm28", "zmm29", "zmm30", "zmm31",
            "k0", "k1", "k2", "k3", "k4", "k5", "k6", "k7",
            "bnd0", "bnd1", "bnd2", "bnd3",
        };
        for (const char* reg : regs) {
            if (low == reg) {
                return true;
            }
        }
        return false;
    }
    std::string asmSafeName(const std::string& spelling) {
        return isMachineReg(spelling) ? spelling + "_" : spelling;
    }
    std::string asmSizeName(ir::IRType width, char forced) {
        if (forced == 'b') {
            return "BYTE PTR";
        }
        if (forced == 'w') {
            return "WORD PTR";
        }
        if (forced == 'd') {
            return "DWORD PTR";
        }
        if (forced == 'q') {
            return "QWORD PTR";
        }
        if (width == ir::IRType::I8 || width == ir::IRType::U8 ||
            width == ir::IRType::Bool) {
            return "BYTE PTR";
        }
        if (width == ir::IRType::I16 || width == ir::IRType::U16) {
            return "WORD PTR";
        }
        if (width == ir::IRType::I32 || width == ir::IRType::U32 ||
            width == ir::IRType::F32) {
            return "DWORD PTR";
        }
        return "QWORD PTR";
    }

    // The register-name form for a staged placeholder: the same width
    // rules as asmSizeName, but the register's own spelling — `r12b` has
    // no `PTR`, and `rbx` names `ebx` at 32 bits. A `{name:q}` suffix
    // overrides; plain `{name}` is the value's own width (FEATURES §4.4).
    std::string asmRegName(const std::string& stem, ir::IRType width, char forced) {
        if (forced == 'q') {
            return stem;
        }
        if (forced == 'd') {
            return stem == "rbx" ? "ebx" : stem + "d";
        }
        if (forced == 'w') {
            return stem == "rbx" ? "bx" : stem + "w";
        }
        if (forced == 'b') {
            return stem == "rbx" ? "bl" : stem + "b";
        }
        if (width == ir::IRType::I8 || width == ir::IRType::U8 ||
            width == ir::IRType::Bool) {
            return stem == "rbx" ? "bl" : stem + "b";
        }
        if (width == ir::IRType::I16 || width == ir::IRType::U16) {
            return stem == "rbx" ? "bx" : stem + "w";
        }
        if (width == ir::IRType::I32 || width == ir::IRType::U32) {
            return stem == "rbx" ? "ebx" : stem + "d";
        }
        return stem;
    }

    // Substitutes every `{name}` in the block's text with the operand the
    // lowerer paired with that name. A local, a parameter, or a function is
    // STAGED in a callee-saved register: loaded at the value's own width
    // before the block, named as a register inside it, and copied back to
    // its storage afterwards, so a write through the placeholder lands
    // where the rest of the function reads. Staging is what keeps a
    // template with two local operands (`mov {a}, {b:q}`) assembling — v2
    // staged each into a memory slot instead, and a mem-to-mem `mov` is
    // rejected by this assembler. The block's own contract preserves
    // callee-saved registers (OPTIMIZER_DESIGN §"Inline asm contract"), so a
    // conforming block leaves the staged value intact. A block that names a
    // callee-saved register outside `{...}`, or that uses an instruction with
    // an implicit callee-saved clobber such as `cpuid`, is non-conforming and
    // is not repaired here. A global stays `[rip + label]`: that is the
    // global's value in its own storage, writable in place, with nothing
    // held across the block.
    void emitAsm(const ir::Instr& instr) {
        // Callee-saved only: rbp is the frame pointer, and a caller-saved
        // register is exactly what the block is promised to clobber.
        static const char* const kPool[] = {"r12", "r13", "r14", "r15", "rbx"};
        static constexpr uint32_t kStagedMax = 5;
        static constexpr uint32_t kPair = 2;
        struct Staged {
            uint32_t id;
            uint32_t regIndex;
            ir::IRType width;
        };
        std::vector<Staged> staged;
        std::unordered_map<uint32_t, uint32_t> stageOfId;
        std::string text = instr.asmText;
        // Classify and substitute first, refusing before any output so a
        // refused block emits nothing half-formed.
        std::vector<std::string> replacements(instr.asmNames.size());
        for (uint32_t i = 0; i < instr.asmNames.size(); ++i) {
            if (i >= instr.operands.size()) {
                break;
            }
            const std::string& raw = instr.asmNames[i];
            std::string probe = raw;
            if (!probe.empty() && probe[0] == '*') {
                probe = probe.substr(1);
            }
            char forced = 0;
            std::string::size_type colon = probe.find(':');
            if (colon != std::string::npos && colon + 1 < probe.size()) {
                forced = probe[colon + 1];
            }
            const ir::IRType width = instr.operands[i].width;
            if (width == ir::IRType::F32 || width == ir::IRType::F64) {
                refuse(instr.loc, "a float asm placeholder");
                return;
            }
            auto known = addrWords_.find(instr.operands[i].id);
            if (known != addrWords_.end()) {
                replacements[i] = asmSizeName(width, forced) +
                                  " [rip + " + known->second + "]";
                continue;
            }
            auto found = stageOfId.find(instr.operands[i].id);
            uint32_t regIndex = 0;
            if (found != stageOfId.end()) {
                regIndex = found->second;
            } else {
                if (staged.size() >= kStagedMax) {
                    refuse(instr.loc, "more placeholders than this block can stage");
                    return;
                }
                regIndex = static_cast<uint32_t>(staged.size());
                stageOfId[instr.operands[i].id] = regIndex;
                staged.push_back(Staged{instr.operands[i].id, regIndex, width});
            }
            replacements[i] = asmRegName(kPool[regIndex], width, forced);
        }
        // Stage loads and copy-backs address the frame through rsp, and the
        // pushes below move rsp, so every one of those addresses carries
        // the same depth. An odd number of pushes would leave a call the
        // block makes 8 bytes out of alignment, so rax pads the count —
        // nothing is live in registers at a block boundary.
        const uint32_t stagedCount = static_cast<uint32_t>(staged.size());
        const bool padRax = stagedCount % kPair != 0;
        const uint32_t pushes = stagedCount + (padRax ? 1u : 0u);
        const uint32_t depth = pushes * 8u;
        for (const Staged& s : staged) {
            out_ += "  push " + std::string(kPool[s.regIndex]) + "\n";
        }
        if (padRax) {
            out_ += "  push rax\n";
        }
        for (const Staged& s : staged) {
            const std::string cell = slotPlus(s.id, depth);
            const std::string reg64 = kPool[s.regIndex];
            // The load widens per the declared type, so the register holds
            // the value: a `u8` 7 reads as 7 at whatever width the block
            // asks for, and an `i32` -5 reads as -5 (FEATURES §4.4
            // substitutes local *values*, not raw cells).
            switch (s.width) {
                case ir::IRType::I8:
                    out_ += "  movsx " + reg64 + ", byte ptr " + cell + "\n";
                    break;
                case ir::IRType::U8:
                case ir::IRType::Bool:
                    out_ += "  movzx " + reg64 + ", byte ptr " + cell + "\n";
                    break;
                case ir::IRType::I16:
                    out_ += "  movsx " + reg64 + ", word ptr " + cell + "\n";
                    break;
                case ir::IRType::U16:
                    out_ += "  movzx " + reg64 + ", word ptr " + cell + "\n";
                    break;
                case ir::IRType::I32:
                    out_ += "  movsxd " + reg64 + ", dword ptr " + cell + "\n";
                    break;
                default:
                    out_ += "  mov " + reg64 + ", qword ptr " + cell + "\n";
                    break;
            }
        }
        for (uint32_t i = 0; i < instr.asmNames.size(); ++i) {
            if (i >= replacements.size() || i >= instr.operands.size()) {
                break;
            }
            const std::string key = "{" + instr.asmNames[i] + "}";
            auto at = std::string::size_type{};
            while ((at = text.find(key, at)) != std::string::npos) {
                text.replace(at, key.size(), replacements[i]);
                at += replacements[i].size();
            }
        }
        out_ += text;
        out_ += "\n";
        // Copy each staged register back at full width: a narrow store
        // would leave the cell's other bytes undefined for any 64-bit
        // reader, and the staged register already holds the widened value.
        for (auto it = staged.rbegin(); it != staged.rend(); ++it) {
            const std::string cell = slotPlus(it->id, depth);
            out_ += "  mov qword ptr " + cell + ", " +
                    std::string(kPool[it->regIndex]) + "\n";
        }
        if (padRax) {
            out_ += "  pop rax\n";
        }
        for (auto it = staged.rbegin(); it != staged.rend(); ++it) {
            out_ += "  pop " + std::string(kPool[it->regIndex]) + "\n";
        }
    }

    // Integer and address casts: same size is a move, narrowing truncates
    // and re-extends, widening extends. Pointers and function pointers are
    // one 64-bit word, so ptr<->ptr is that move (SEMANTICS: address-
    // preserving) and int<->ptr rides the same width logic. Floats refuse
    // with a diagnostic naming the gap rather than a wrong move.
    bool emitCast(const ir::Instr& instr) {
        uint64_t sources = 0;
        for (const ir::Value& operand : instr.operands) {
            (void)operand;
            ++sources;
        }
        const uint64_t want = 1;
        if (sources != want || instr.results.empty()) {
            refuse(instr.loc, "a bad cast");
            return false;
        }
        TypeRef from = instr.operands[0].type;
        TypeRef to = instr.type;
        if (from.isError() || to.isError()) {
            refuse(instr.loc, "this cast is not emitted yet");
            return false;
        }
        bool fromPtr = from.type->kind() == Kind::Pointer ||
                       from.type->kind() == Kind::FunctionPtr;
        bool toPtr = to.type->kind() == Kind::Pointer ||
                     to.type->kind() == Kind::FunctionPtr;
        // basicOf answers the enum side as its underlying signed i32
        // (FEATURES §2, enum ↔ int row). It is called only where the call
        // sites already proved non-pointer scalar sides, so a pointer
        // never meets the dereference.
        auto basicOf = [](TypeRef t) -> BasicKind {
            if (t.type->kind() == Kind::Enum) {
                return BasicKind::I32;
            }
            return *t.type->basic();
        };
        bool fromFloat = from.type->kind() == Kind::Basic &&
                         basicIsFloat(*from.type->basic());
        bool toFloat = to.type->kind() == Kind::Basic &&
                       basicIsFloat(*to.type->basic());
        if (fromFloat || toFloat) {
            if (fromPtr || toPtr) {
                refuse(instr.loc, "this cast is not emitted yet");
                return false;
            }
            // Past the pointer refuse both sides are scalar basics or an
            // enum riding its underlying i32 (enum-to-float reaches here;
            // float-to-enum never does — lowering diverts every enum
            // target through the membership trap first).
            const int fromF = basicWidthBits(basicOf(from));
            const int toF = basicWidthBits(basicOf(to));
            if (fromFloat && toFloat && !fast_) {
                // The legacy emitter keeps its exact old sequence.
                out_ += "  mov rax, " +
                        valueRef(instr.operands[0].id) + "\n";
                if (fromF == toF) {
                    storeResult(instr.results[0].id, "rax");
                    return true;
                }
                if (*from.type->basic() == BasicKind::F32) {
                    out_ += "  movss xmm0, " +
                            valueRef(instr.operands[0].id) + "\n";
                    out_ += "  cvtss2sd xmm0, xmm0\n";
                    out_ += "  movsd " + slot(instr.results[0].id) +
                            ", xmm0\n";
                } else {
                    out_ += "  movsd xmm0, " +
                            valueRef(instr.operands[0].id) + "\n";
                    out_ += "  cvtsd2ss xmm0, xmm0\n";
                    out_ += "  movss " + slot(instr.results[0].id) +
                            ", xmm0\n";
                }
                return true;
            }
            if (fromFloat && toFloat) {
                // Same width is a bit move; otherwise convert through the
                // destination home when it has one. Xmm0 remains the scratch
                // register for a spilled destination.
                const uint32_t resultId = instr.results[0].id;
                const uint32_t sourceId = instr.operands[0].id;
                const uint8_t dest = floatHomeOf(resultId);
                if (fromF == toF) {
                    const char* move =
                        fromF == 32 ? "movss" : "movsd";
                    const uint8_t source = floatHomeOf(sourceId);
                    if (dest != static_cast<uint8_t>(XmmRegister::None)) {
                        if (dest != source) {
                            out_ += "  " + std::string(move) + " " +
                                    xmmRef(dest) + ", " +
                                    floatValueRef(sourceId) + "\n";
                        }
                    } else if (source !=
                               static_cast<uint8_t>(XmmRegister::None)) {
                        out_ += "  " + std::string(move) + " " +
                                slot(resultId) + ", " + xmmRef(source) +
                                "\n";
                    } else {
                        out_ += "  " + std::string(move) +
                                " xmm0, " + slot(sourceId) + "\n";
                        out_ += "  " + std::string(move) + " " +
                                slot(resultId) + ", xmm0\n";
                    }
                    return true;
                }
                const bool from32 = *from.type->basic() == BasicKind::F32;
                const char* sourceMove = from32 ? "movss" : "movsd";
                const char* convert = from32 ? "cvtss2sd" : "cvtsd2ss";
                const char* resultMove = from32 ? "movsd" : "movss";
                if (dest != static_cast<uint8_t>(XmmRegister::None) &&
                    dest != floatHomeOf(sourceId)) {
                    out_ += "  " + std::string(sourceMove) + " " +
                            xmmRef(dest) + ", " + floatValueRef(sourceId) +
                            "\n";
                    out_ += "  " + std::string(convert) + " " +
                            xmmRef(dest) + ", " + xmmRef(dest) + "\n";
                    return true;
                }
                out_ += "  " + std::string(sourceMove) + " xmm0, " +
                        floatValueRef(sourceId) + "\n";
                out_ += "  " + std::string(convert) + " xmm0, xmm0\n";
                out_ += "  " + std::string(resultMove) + " " +
                        (dest == static_cast<uint8_t>(XmmRegister::None)
                             ? slot(resultId)
                             : xmmRef(dest)) +
                        ", xmm0\n";
                return true;
            }
            if (fromFloat) {
                // float -> int truncates. The convert runs at the target
                // width when it has one (cvtt has no 8/16-bit form, so
                // those go through r32 and narrow like an integer cast);
                // an out-of-range value reads the processor's indefinite
                // answer, which is the spec's undefined result, never a
                // check. A u64 target rides the signed convert.
                const bool from32 = *from.type->basic() == BasicKind::F32;
                const char* cvt = from32 ? "cvttss2si" : "cvttsd2si";
                const char* move = from32 ? "movss" : "movsd";
                out_ += "  " + std::string(move) + " xmm0, " +
                        floatValueRef(instr.operands[0].id) + "\n";
                const bool signedTarget = basicIsSigned(*to.type->basic());
                if (toF == 64) {
                    out_ += "  " + std::string(cvt) + " rax, xmm0\n";
                } else {
                    out_ += "  " + std::string(cvt) + " eax, xmm0\n";
                    if (toF == 32) {
                        if (signedTarget) {
                            out_ += "  movsxd rax, eax\n";
                        }
                    } else {
                        const char* ext = signedTarget ? "movsx" : "movzx";
                        if (toF == 8) {
                            out_ += std::string("  ") + ext + " rax, al\n";
                        } else {
                            out_ += std::string("  ") + ext + " rax, ax\n";
                        }
                    }
                }
                storeResult(instr.results[0].id, "rax");
                return true;
            }
            // int -> float: every integer producer keeps its value extended
            // in rax, so the 64-bit convert answers in one instruction —
            // except u64, where cvtsi2sd would read the high bit as a sign.
            // Values at or above 2^63 convert (x >> 1) | (x & 1) and
            // double, which rounds correctly (the sequence v2 verified on
            // the same corpus).
            const bool to32 = *to.type->basic() == BasicKind::F32;
            const char* cvt = to32 ? "cvtsi2ss" : "cvtsi2sd";
            out_ += "  mov rax, " + valueRef(instr.operands[0].id) + "\n";
            if (from.type->kind() == Kind::Basic &&
                *from.type->basic() == BasicKind::U64) {
                std::string lbl =
                    ".Lu64cvt" + std::to_string(cvtLabelSeq_++);
                out_ += "  test rax, rax\n";
                out_ += "  js " + lbl + "\n";
                out_ += "  " + std::string(cvt) + " xmm0, rax\n";
                out_ += "  jmp " + lbl + "_done\n";
                out_ += lbl + ":\n";
                out_ += "  mov r11, rax\n";
                out_ += "  shr r11, 1\n";
                out_ += "  and eax, 1\n";
                out_ += "  or r11, rax\n";
                out_ += "  " + std::string(cvt) + " xmm0, r11\n";
                out_ += std::string("  ") + (to32 ? "addss" : "addsd") +
                        " xmm0, xmm0\n";
                out_ += lbl + "_done:\n";
                storeFloatResult(instr.results[0].id, "xmm0",
                                 to32 ? ir::IRType::F32
                                      : ir::IRType::F64);
                return true;
            }
            out_ += "  " + std::string(cvt) + " xmm0, rax\n";
            storeFloatResult(instr.results[0].id, "xmm0",
                             to32 ? ir::IRType::F32 : ir::IRType::F64);
            return true;
        }
        // Scalar casts run on basics; an enum side rides its underlying
        // i32 (int-to-enum already passed its membership trap at
        // lowering). Anything else refuses.
        auto scalarKind = [](Kind kind) {
            return kind == Kind::Basic || kind == Kind::Enum;
        };
        if (!fromPtr && !toPtr &&
            (!scalarKind(from.type->kind()) || !scalarKind(to.type->kind()))) {
            refuse(instr.loc, "this cast is not emitted yet");
            return false;
        }
        // A pointer participates as its machine width; everything else
        // answers through the basic table.
        int fromBits = fromPtr ? 64 : basicWidthBits(basicOf(from));
        int toBits = toPtr ? 64 : basicWidthBits(basicOf(to));
        bool signedTarget =
            !toPtr &&
            (to.type->kind() == Kind::Basic || to.type->kind() == Kind::Enum) &&
            basicIsSigned(basicOf(to));
        const bool fromInt =
            !fromPtr &&
            (from.type->kind() == Kind::Basic || from.type->kind() == Kind::Enum) &&
            basicIsInteger(basicOf(from));
        const bool toInt =
            !toPtr && (to.type->kind() == Kind::Basic || to.type->kind() == Kind::Enum) &&
            basicIsInteger(basicOf(to));
        const bool crossSign = fromInt && toInt &&
                               (basicIsSigned(basicOf(from)) !=
                                basicIsSigned(basicOf(to)));
        const uint32_t sourceId = instr.operands[0].id;
        const uint8_t dest =
            fast_ ? effectiveHome(instr.results[0].id)
                  : static_cast<uint8_t>(Register::None);
        // A fast homed result receives the cast directly: the source word
        // moves into the home (unless it already lives there), then the
        // home's low bits are extended exactly as the legacy path extended
        // rax. A spilled result and the legacy emitter keep the rax path.
        if (dest != static_cast<uint8_t>(Register::None)) {
            if (effectiveHome(sourceId) != dest) {
                out_ += std::string("  mov ") + regWidth(dest, 'q') + ", " +
                        valueRef(sourceId) + "\n";
            }
            if ((fromBits != toBits || crossSign) && toBits != 64) {
                if (toBits == 8) {
                    out_ += std::string(signedTarget ? "  movsx " : "  movzx ") +
                            (signedTarget ? regWidth(dest, 'q')
                                          : regWidth(dest, 'd')) +
                            ", " + regWidth(dest, 'b') + "\n";
                } else if (toBits == 16) {
                    out_ += std::string(signedTarget ? "  movsx " : "  movzx ") +
                            regWidth(dest, 'q') + ", " +
                            regWidth(dest, 'w') + "\n";
                } else if (toBits == 32) {
                    out_ += signedTarget
                                ? std::string("  movsxd ") +
                                      regWidth(dest, 'q') + ", " +
                                      regWidth(dest, 'd') + "\n"
                                : std::string("  mov ") + regWidth(dest, 'd') +
                                      ", " + regWidth(dest, 'd') + "\n";
                }
            }
            return true;
        }
        out_ += "  mov rax, " + valueRef(sourceId) + "\n";
        if (fromBits == toBits) {
            // Same width, different signedness: the cell keeps the
            // source's extension, so re-extend through the target —
            // i32(-1) `as` u32 reads 0xFFFFFFFF, not the sign-extended
            // source cell. Pointers keep the plain move.
            if (!crossSign) {
                storeResult(instr.results[0].id, "rax");
                return true;
            }
            if (toBits == 8) {
                out_ += std::string(signedTarget ? "  movsx rax, al\n"
                                                 : "  movzx rax, al\n");
            } else if (toBits == 16) {
                out_ += std::string(signedTarget ? "  movsx rax, ax\n"
                                                 : "  movzx rax, ax\n");
            } else if (toBits == 32) {
                out_ += std::string(signedTarget ? "  movsxd rax, eax\n"
                                                 : "  mov eax, eax\n");
            }
            storeResult(instr.results[0].id, "rax");
            return true;
        }
        const char* move = signedTarget ? "movsx" : "movzx";
        std::string reg = "rax";
        if (toBits == 8) {
            reg = std::string(move) + " rax, al";
        } else if (toBits == 16) {
            reg = std::string(move) + " rax, ax";
        } else if (toBits == 32) {
            reg = signedTarget ? "movsxd rax, eax" : "mov eax, eax";
        } else {
            reg = "mov rax, rax";
        }
        // Narrowing first truncates through the source width: a 64-bit value
        // narrowed to u8 keeps its low byte, whatever the high bits held.
        if (fromBits > toBits) {
            out_ += "  mov rax, " + valueRef(instr.operands[0].id) + "\n";
        }
        out_ += "  " + reg + "\n";
        storeResult(instr.results[0].id, "rax");
        return true;
    }

    // Remove a callee-save prologue/epilogue pair whose register is never
    // read in the body. After the fast immediate folds, a dead constant home
    // can leave `mov reg, imm` as the register's only mention; the caller then
    // cannot observe the saved value, so the save cell write and every restore
    // are dead too. The allocation's callee-saved list and the frame layout stay
    // untouched: stageGprArguments uses the list to exclude scratch registers
    // even when no home survives, and every offset must remain what the rest of
    // the emitter computed.
    void dropDeadCalleeSaves(std::string& text) {
        if (sawAsm_ || allocation_.calleeSaved.empty()) {
            return;
        }
        std::vector<std::string> lines;
        uint64_t start = 0;
        while (start < text.size()) {
            const uint64_t end = text.find('\n', start);
            if (end == std::string::npos) {
                lines.push_back(text.substr(start));
                start = text.size();
            } else {
                lines.push_back(text.substr(start, end - start));
                start = end + 1;
            }
        }
        const bool trailingNewline = !text.empty() && text.back() == '\n';
        std::vector<bool> remove(lines.size(), false);
        bool changed = false;
        uint32_t index = 0;
        for (Register reg : allocation_.calleeSaved) {
            const std::string name = registerName(reg);
            const std::string save = "  mov qword ptr " + saveSlotRef(index) +
                                     ", " + name;
            const std::string restore = "  mov " + name + ", qword ptr " +
                                        saveSlotRef(index);
            uint64_t saveAt = lines.size();
            std::vector<uint64_t> restores;
            for (uint64_t i = 0; i < lines.size(); ++i) {
                if (lines[i] == save) {
                    saveAt = i;
                } else if (lines[i] == restore) {
                    restores.push_back(i);
                }
            }
            ++index;
            // A pair is removable only when both halves are present in the
            // shapes the emitter wrote. Anything else is left alone.
            if (saveAt == lines.size() || restores.empty()) {
                continue;
            }
            std::vector<uint64_t> mentions;
            for (uint64_t i = 0; i < lines.size(); ++i) {
                if (i == saveAt) {
                    continue;
                }
                bool isRestore = false;
                for (uint64_t at : restores) {
                    if (at == i) {
                        isRestore = true;
                        break;
                    }
                }
                if (isRestore) {
                    continue;
                }
                if (!pParseInstr(lines[i]).valid) {
                    continue;
                }
                std::unordered_set<std::string> named;
                pAddTextRegs(lines[i], named);
                if (named.find(pLower(name)) != named.end()) {
                    mentions.push_back(i);
                }
            }
            bool dead = mentions.empty();
            const uint64_t mentionCount = mentions.size();
            if (mentionCount == 1) {
                // The only body mention must be a write of an immediate. It
                // reads no width of the register, and no other line names one.
                const PInstr instr = pParseInstr(lines[mentions[0]]);
                dead = instr.valid && instr.mnemonic == "mov" &&
                       instr.ops.size() == kPairOperands &&
                       pGpParent(instr.ops[0]) == pLower(name) &&
                       pIsImmediate(instr.ops[1]);
            }
            if (!dead) {
                continue;
            }
            remove[saveAt] = true;
            for (uint64_t at : restores) {
                remove[at] = true;
            }
            for (uint64_t at : mentions) {
                remove[at] = true;
            }
            changed = true;
        }
        if (!changed) {
            return;
        }
        std::string result;
        for (uint64_t i = 0; i < lines.size(); ++i) {
            if (remove[i]) {
                continue;
            }
            result += lines[i];
            result += "\n";
        }
        if (!trailingNewline && !result.empty() && result.back() == '\n') {
            result.pop_back();
        }
        text = std::move(result);
    }

    // The tail every return shares: restore the callee-saved integer and
    // float homes, then leave. The return words are already in rax/rdx/xmm0,
    // and the frame cells use rsp, so they are read before `leave` moves it.
    // `leave` restores rsp from rbp, so no frame adjustment is needed first.
    void emitReturnEpilogue() {
        uint32_t saveIndex = 0;
        for (Register reg : allocation_.calleeSaved) {
            out_ += "  mov " + regRef(static_cast<uint8_t>(reg)) +
                    ", qword ptr " + saveSlotRef(saveIndex) + "\n";
            ++saveIndex;
        }
        if (fast_) {
            uint32_t xmmSaveIndex = 0;
            for (XmmRegister reg : allocation_.floats.calleeSaved) {
                out_ += std::string("  movdqu ") + xmmRegisterName(reg) +
                        ", xmmword ptr " +
                        xmmSaveSlotRef(xmmSaveIndex) + "\n";
                ++xmmSaveIndex;
            }
        }
        out_ += "  leave\n";
        out_ += "  ret\n";
    }

    bool emitTerm(const ir::Function& fn, const ir::Terminator& term) {
        if (term.term == ir::Term::Ret) {
            // Words come home in order: the first in rax (or xmm0 for a
            // float), the second in rdx. A third word never arrives here:
            // lowering refuses it, so emitting only two is complete, not
            // truncation. The poison id (a value falloff the checker
            // rejected) answers nothing, as before.
            for (uint32_t i = 0; i < term.values.size(); ++i) {
                const ir::Value& word = term.values[i];
                if (word.id == 0xFFFFFFFFu) {
                    continue;
                }
                if (i == 0 && word.width == ir::IRType::F32) {
                    out_ += "  movss xmm0, " + floatValueRef(word.id) + "\n";
                } else if (i == 0 && word.width == ir::IRType::F64) {
                    out_ += "  movsd xmm0, " + floatValueRef(word.id) + "\n";
                } else if (i == 0) {
                    out_ += "  mov rax, " + valueRef(word.id) + "\n";
                } else if (i == 1) {
                    out_ += "  mov rdx, " + valueRef(word.id) + "\n";
                } else {
                    refuse(term.loc, "a return wider than two words");
                    return false;
                }
            }
            // The return words are in rax/rdx/xmm0. A shared fast epilogue
            // owns the restores; this return only stages its values and
            // jumps. The last emitted block has no next label, so it falls
            // through into the epilogue instead of jumping to the next
            // instruction.
            if (sharedEpilogue_) {
                if (!nextBlockLabel_.empty()) {
                    out_ += "  jmp " + sharedEpilogueLabel_ + "\n";
                }
                return true;
            }
            emitReturnEpilogue();
            return true;
        }
        if (term.term == ir::Term::Br) {
            out_ += "  jmp " + labelFor(fn.name, term.target) + "\n";
            return true;
        }
        if (term.term == ir::Term::CondBr && term.cond.has_value()) {
            if (fast_) {
                // A successor that is the next block in emission order needs
                // no jump: invert the condition and fall through into it. Both
                // successors still reach exactly the same code; only the branch
                // layout changes, and nothing moves across an asm, volatile or
                // trap barrier (the branch is already the block's last
                // instruction).
                const std::string thenLabel =
                    labelFor(fn.name, term.thenTarget);
                const std::string elseLabel =
                    labelFor(fn.name, term.elseTarget);
                const bool elseNext =
                    !nextBlockLabel_.empty() && elseLabel == nextBlockLabel_;
                const bool thenNext =
                    !nextBlockLabel_.empty() && thenLabel == nextBlockLabel_;
                if (elseNext && thenNext) {
                    // Both edges land on the next block, so the branch is
                    // dead weight and the condition's flags are unused.
                    fusedCompareValid_ = false;
                    return true;
                }
                // The then edge falls through, so the branch must test the
                // inverse condition and name the else target.
                bool invert = thenNext;
                if (fusedCompareValid_) {
                    // The flags are the compare's; the pinned pair keeps the
                    // fast peephole from re-deriving a layout that would undo
                    // the inversion or move the branch off the flags.
                    const char* branch = jumpFor(fusedCmp_);
                    if (invert) {
                        const char* inverse = pInverseJump(branch);
                        if (inverse != nullptr) {
                            branch = inverse;
                        } else {
                            // No inverse mnemonic: keep the canonical
                            // two-branch form instead of emitting a null.
                            invert = false;
                        }
                    }
                    out_ += std::string(kPeepholePinBegin) + "\n";
                    out_ += std::string("  ") + branch + " " +
                            (invert ? elseLabel : thenLabel) + "\n";
                    if (!invert && !elseNext) {
                        out_ += "  jmp " + elseLabel + "\n";
                    }
                    out_ += std::string(kPeepholePinEnd) + "\n";
                    fusedCompareValid_ = false;
                    return true;
                }
                // The condition is a boolean word in its home or slot. A
                // register tests itself; a slot compares against zero in
                // place, so the branch needs no rax round trip. A
                // rematerialised constant has no memory operand, so it takes
                // the one-register form.
                const uint32_t id = term.cond->id;
                const uint8_t home = homeOf(id);
                if (home != static_cast<uint8_t>(Register::None)) {
                    const std::string reg = regRef(home);
                    out_ += "  test " + reg + ", " + reg + "\n";
                } else if (immediateRef(id, nullptr)) {
                    out_ += "  mov rax, " + valueRef(id) + "\n";
                    out_ += "  test rax, rax\n";
                } else {
                    out_ += "  cmp qword ptr " + slot(id) + ", 0\n";
                }
                if (invert) {
                    out_ += "  jz " + elseLabel + "\n";
                } else {
                    out_ += "  jnz " + thenLabel + "\n";
                    if (!elseNext) {
                        out_ += "  jmp " + elseLabel + "\n";
                    }
                }
                return true;
            }
            out_ += "  mov rax, " + valueRef(term.cond->id) + "\n";
            out_ += "  test rax, rax\n";
            out_ += "  jnz " + labelFor(fn.name, term.thenTarget) + "\n";
            out_ += "  jmp " + labelFor(fn.name, term.elseTarget) + "\n";
            return true;
        }
        if (term.term == ir::Term::Unreachable) {
            // Provably unreached, so a trap is the honest emission: silence
            // would fall through into whatever block follows.
            out_ += "  ud2\n";
            return true;
        }
        if (term.term == ir::Term::Trap) {
            // A lowering check failed at runtime (a union tag mismatch, a
            // bounds check, a nil guard): stop rather than continue with a
            // known-wrong value. Diagnosed reasons write the language line;
            // the rest keep the silent ud2.
            return emitTrapReason(term.reason);
        }
        refuse(term.loc, "this terminator is not emitted yet");
        return false;
    }





    const ir::Module& module_;
    Target target_;
    bool fast_ = false;
    bool requireEntry_ = true;
    // Whether the function being emitted contains inline asm. Its implicit
    // clobbers (cpuid's rbx, xlat's rbx base) are invisible to the emitted
    // text, so the dead callee-save pass must leave such a function alone.
    // This is conservative safety, not validation: a block with an implicit
    // callee-saved clobber is outside the contract and unsupported.
    bool sawAsm_ = false;
    // The compact frame and its span for the function being emitted. The
    // span carries the native `_start` entry adjustment; frameBytes itself
    // stays a 16-byte multiple.
    FrameLayout frame_;
    uint32_t fastSpan_ = 0;
    bool slotMissing_ = false;
    // Alloca-result ids in the function being emitted: the only ids whose
    // address is frame storage. Any other id in an addressBase is a computed
    // address and dereferences.
    std::unordered_set<uint32_t> slotIds_;
    // Result id of a DataAddr word -> its label, so an asm placeholder
    // paired with that word prints `[rip + label]` (the global's value)
    // instead of the temporary holding the address.
    std::unordered_map<uint32_t, std::string> addrWords_;
    // Uniquely-defined integer Const ids -> their immediate. A value with no
    // register home is rematerialised from this map instead of being reloaded
    // from its frame slot. Only constants that fit a signed 32-bit immediate
    // enter the map, so every context that consumes a value reference can
    // encode the result.
    std::unordered_map<uint32_t, int64_t> rematConstants_;
    // Cast results whose source is a remat constant, so an integer cast that
    // only renames an immediate is itself foldable into a consumer. The map
    // is filled in block order, so a cast of a cast resolves in one pass.
    std::unordered_map<uint32_t, int64_t> castConstants_;
    // The subset of castConstants_ whose cast leaves the source number
    // unchanged. The dead-home fold may use only these; a narrowing or
    // cross-signed cast still needs its own instruction.
    std::unordered_set<uint32_t> preservingCastIds_;
    // One function-wide allocation per function, and the block whose homes
    // the current instruction is emitted under. Callee-saved homes are stored
    // in frame cells starting at calleeSaveBase_.
    FunctionAllocation allocation_;
    // Parameter ids whose home the fast prologue writes. It excludes an
    // alloca-backed parameter cell that cannot be register-resident (its
    // address escapes, or the ABI's incoming registers make the write order
    // unsafe); such a parameter keeps its cell and the slot write.
    std::unordered_set<uint32_t> paramHomeIds_;
    // Alloca-backed parameter cells that live in their home instead of the
    // frame cell. Loads and stores to these ids read and write the home.
    std::unordered_set<uint32_t> residentParams_;
    const BlockAllocation* currentHome_ = nullptr;
    // The current block's float homes, or null outside the block loop and on
    // the legacy backend. Xmm0 is never a home; it is the float scratch.
    const std::vector<uint8_t>* currentFloatHome_ = nullptr;
    // GEP result id -> producer, for addresses folded into the next access.
    std::unordered_map<uint32_t, const ir::Instr*> foldedGep_;
    // Byte offset of the first nonvolatile XMM save cell in the fast frame.
    uint32_t xmmSaveBase_ = 0;
    // Fixed-register guard cells and the values currently saved there. The
    // guard exists only when a live home would otherwise be clobbered by
    // div/rem (rax/rdx) or a variable shift (rax/rcx).
    uint32_t fixedScratchBase_ = 0;
    uint32_t fixedScratchCells_ = 0;
    uint8_t fixedGuardRegs_[2] = {0, 0};
    uint32_t fixedGuardCount_ = 0;
    // The current instruction's RPO position, used by the fixed-register
    // guard to ask the liveness ranges which homes are live across it.
    uint32_t currentPos_ = 0;
    // True when the function being emitted uses the Microsoft x64 convention;
    // its xmm6-xmm15 row is nonvolatile even for registers it does not home.
    bool currentMsAbi_ = false;
    // The fast compare fusion state: the block being emitted (for deciding
    // whether a Cmp is the last instruction before its CondBr), how many times
    // each value is used, and the Cmp whose flags the next terminator reads.
    const ir::Block* currentBlock_ = nullptr;
    // The label emitted immediately after the block being emitted, or empty on
    // the last block. The fast conditional terminator compares against it to
    // fall through into a successor instead of jumping to it.
    std::string nextBlockLabel_;
    // The shared fast epilogue for a many-return function: whether it is in
    // use, and the label every non-last return jumps to.
    bool sharedEpilogue_ = false;
    std::string sharedEpilogueLabel_;
    std::vector<uint32_t> useCount_;
    // The fast fusion safety net: live-out value ids per block, borrowed from
    // the allocation's single whole-function liveness analysis. A compare
    // result in this set has a consumer after the branch. Null for legacy.
    const std::vector<uint32_t>* currentLiveOut_ = nullptr;
    std::unordered_map<uint32_t, int64_t> constValues_;
    ir::CmpOp fusedCmp_ = ir::CmpOp::Eq;
    bool fusedCompareValid_ = false;
    // The constant (a Const or a cast of one) whose home write the next
    // instruction will fold into a lea or three-operand imul. Value id 0 is
    // valid, so "none" is a separate sentinel.
    uint32_t foldedValueId_ = kInvalidValue;
    // Constants folded into a consumer whose result home is not the
    // constant's: consumer position -> the constant ids it reads as
    // immediates. A map because two constants can feed one consumer, and
    // because a cast of a constant can itself be folded into a later one.
    std::unordered_map<uint32_t, std::unordered_set<uint32_t>> foldedAtPos_;
    // Const/Cast result ids whose home write the fold elided. Unlike
    // foldedValueId_, this survives until the consumer, which may be several
    // transparent instructions later.
    std::unordered_set<uint32_t> foldedDefinitions_;
    uint32_t calleeSaveBase_ = 0;
    // The SysV variadic register-save area for the function being emitted:
    // whether this frame has one, and its byte offset from rsp. The area and
    // the `va_start` cursor both read these, so they cannot disagree.
    bool varargsSave_ = false;
    uint32_t varargsBase_ = 0;
    // VARIADICS.md §7: whether the current function is an msabi variadic
    // callee. Its cursor lives in the first eight bytes of the 24-byte
    // VaList, and its incoming integer registers are homed in shadow space.
    bool msVarargs_ = false;
    // Unique suffix for `va_arg`'s register/overflow paths across the module.
    uint64_t vaArgSeq_ = 0;
    std::string out_;
    // Unique suffix for u64-to-float conversion labels (int→float casts
    // need a two-path sequence; see emitCast).
    uint64_t cvtLabelSeq_ = 0;
    uint64_t nullSeq_ = 0;
    uint64_t trapMessageSeq_ = 0;
    // Out-of-line fast trap stubs for the function being emitted. The site
    // holds only a jump or a null-check branch; the bodies wait here until
    // every block has been emitted. The text is cleared at function entry; the
    // label sequence is module-wide so the assembler sees each label once.
    std::string coldTrapText_;
    uint64_t trapStubSeq_ = 0;
    bool failed_ = false;
    std::string error_;
    Position pos_{"", 1, 1};
};

const char* targetName(Target kind) {
    return target::info(kind).name;
}

EmitResult emitNative(const ir::Module& module, CodegenKind kind,
                      bool requireEntry) {
    // The fast backend is native-only during bring-up. Non-native targets
    // route through the legacy emitter even when `--codegen=fast` is named:
    // they have not been executed on hardware, so silently selecting an
    // untested frame would be a regression, not an optimization.
    switch (kind) {
        case CodegenKind::Legacy:
        case CodegenKind::Fast:
            break;
    }
    Emitter emitter(module, Target::Native, kind, requireEntry);
    return emitter.run();
}

EmitResult emitNative(const ir::Module& module, bool requireEntry) {
    return emitNative(module, CodegenKind::Legacy, requireEntry);
}

EmitResult emit(const ir::Module& module, Target kind, CodegenKind codegen,
                bool requireEntry) {
    switch (kind) {
        case Target::Native:
            return emitNative(module, codegen, requireEntry);
        case Target::Pomeos: {
            // `fast` is native-only during bring-up; Pomeos and UEFI keep
            // the legacy emitter so an untested target cannot regress.
            Emitter emitter(module, Target::Pomeos, CodegenKind::Legacy,
                            requireEntry);
            return emitter.run();
        }
        case Target::Uefi: {
            Emitter emitter(module, Target::Uefi, CodegenKind::Legacy,
                            requireEntry);
            return emitter.run();
        }
        case Target::Vm:
            break;
    }
    EmitResult denied;
    denied.ok = false;
    denied.error = std::string("the ") + targetName(kind) +
                   " emitter is not implemented yet";
    return denied;
}

EmitResult emit(const ir::Module& module, Target kind, bool requireEntry) {
    return emit(module, kind, CodegenKind::Legacy, requireEntry);
}

}  // namespace codegen
}  // namespace cider
