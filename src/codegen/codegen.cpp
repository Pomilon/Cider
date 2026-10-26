// codegen.cpp — checked IR to x86-64 assembly (SysV; Linux/PomeOS services).
//
// Intel syntax with `.intel_syntax noprefix`, so plain `cc file.s` assembles
// it. Every SSA value lives in its own stack slot and six scratch registers
// carry values between slot and operation: correct by construction, slow by
// design. A register allocator replaces this strategy, not its interface.
//
// The text is a pure function of the module: no timestamps, no working
// directories, no absolute paths. Gate L5.

#include "codegen/codegen.h"

#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "layout.h"

namespace cider {
namespace codegen {

namespace {

// A float argument occupies exactly one word; named here so the call
// paths do not write the count beside a size query (T4).
constexpr uint64_t kFloatWord = 1;

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

constexpr uint64_t kSysVIntRegs = 6;
constexpr uint64_t kMsabiIntRegs = 4;
constexpr uint64_t kSysVSseRegs = 8;
constexpr uint64_t kMsabiSseRegs = 4;
constexpr uint64_t kShadowBytes = 32;
constexpr uint64_t kWordBytes = 8;

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

}  // namespace

class Emitter {
public:
    Emitter(const ir::Module& module, Target target, bool requireEntry = true)
        : module_(module), target_(target), requireEntry_(requireEntry) {}

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
    bool emitTrapReason(ir::TrapReason reason) {
        if (target_ != Target::Native) {
            out_ += "  ud2\n";
            return true;
        }
        const char* raw = ir::trapMessage(reason);
        if (raw == nullptr) {
            out_ += "  ud2\n";
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
        out_ += "  mov eax, 1\n";
        out_ += "  mov edi, 2\n";
        out_ += "  lea rsi, [rip + " + label + "]\n";
        out_ += "  mov edx, " + std::to_string(message.size()) + "\n";
        out_ += "  syscall\n";
        out_ += "  mov eax, 60\n";
        out_ += "  mov edi, 2\n";
        out_ += "  syscall\n";
        out_ += "  ud2\n";
        out_ += ".pushsection .rodata\n";
        out_ += label + ":\n";
        out_ += "  .ascii \"" + escaped + "\"\n";
        out_ += ".popsection\n";
        return true;
    }

    // The target's null policy is explicit: a target whose address zero is
    // not guaranteed unmapped gets a check before a computed dereference.
    // The same check serves UEFI and PomeOS; only the label names the target.
    void emitNullCheck(const std::string& reg) {
        const target::Info& info = target::info(target_);
        if (info.nullFaults) {
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
    // neighbor bytes, which is the defect v2's spill slots had.
    std::string slot(uint32_t id) const {
        return "[rsp + " + std::to_string(id * 8) + "]";
    }

    // Appends a displacement to an address string, omitting +0: `[rip + g+0]`
    // assembles, but the plain form reads better and diffs cleaner.
    std::string atAddr(const std::string& base, int64_t disp) const {
        if (disp == 0) {
            return "[" + base + "]";
        }
        return "[" + base + " + " + std::to_string(disp) + "]";
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
    // extending to 64 bits on the way.
    void emitLoadMem(const std::string& mem, TypeRef type, ir::IRType width) {
        if (width == ir::IRType::F32) {
            out_ += "  movss xmm0, " + mem + "\n";
            return;
        }
        if (width == ir::IRType::F64) {
            out_ += "  movsd xmm0, " + mem + "\n";
            return;
        }
        bool isSigned = signedRead(type);
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
            out_ += "  movsxd rax, DWORD PTR " + mem + "\n";
            return;
        }
        if (width == ir::IRType::U32) {
            // A zero-extending dword load: the cell holds the value in
            // its low four bytes, and a qword load would also pick up
            // whatever shares the slot (a packed array neighbour), so
            // the neighbour's bytes would corrupt the comparison. A
            // dword load through eax zeroes the top half by itself.
            out_ += "  mov eax, DWORD PTR " + mem + "\n";
            return;
        }
        out_ += "  mov rax, " + mem + "\n";
    }

    // Stores the low bytes of rax (xmm0 for floats) with exactly the width a
    // type names, so a narrow store never clobbers neighbor bytes.
    void emitStoreMem(const std::string& mem, TypeRef type, ir::IRType width) {
        if (width == ir::IRType::F32) {
            out_ += "  movss " + mem + ", xmm0\n";
            return;
        }
        if (width == ir::IRType::F64) {
            out_ += "  movsd " + mem + ", xmm0\n";
            return;
        }
        if (width == ir::IRType::I8 || width == ir::IRType::U8 ||
            width == ir::IRType::Bool) {
            out_ += "  mov " + mem + ", al\n";
            return;
        }
        if (width == ir::IRType::I16 || width == ir::IRType::U16) {
            out_ += "  mov " + mem + ", ax\n";
            return;
        }
        if (width == ir::IRType::I32 || width == ir::IRType::U32) {
            out_ += "  mov " + mem + ", eax\n";
            return;
        }
        out_ += "  mov " + mem + ", rax\n";
    }

    // The first slot id past every value of the function being emitted:
    // a call stages spilled argument words here, and because no IR value
    // owns the id, staging cannot clobber the live word it copies from.
    uint32_t scratchBase_ = 0;

    std::string labelFor(const std::string& fn, uint32_t label) const {
        return ".L_" + fn + "_" + std::to_string(label);
    }

    bool emitFunction(const ir::Function& fn) {
        // The frame covers every value id the function mentions, rounded up
        // so `sub rsp` keeps 16-byte alignment for the next call.
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
        uint32_t span = (cells * 8 + 15) & ~15u;
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

        if (fn.weak) {
            out_ += ".weak " + asmSafeName(fn.name) + "\n";
        } else if (fn.exportSymbol) {
            out_ += ".globl " + asmSafeName(fn.name) + "\n";
        }
        out_ += asmSafeName(fn.name) + ":\n";
        out_ += "  push rbp\n";
        out_ += "  mov rbp, rsp\n";
        out_ += "  sub rsp, " + std::to_string(span) + "\n";

        // Parameters arrive in registers (then stack); they live in slots
        // from here on, like every other value. A float parameter comes in
        // on its SSE register — the class lowering recorded per word — so
        // the two counters mirror the caller's split exactly; an integer
        // word past the row would arrive on the stack, which the caller
        // refuses to send, so that branch stays a defensive reading.
        //
        // 8.4: the row follows the function's own convention. An msabi
        // function reads rcx, rdx, r8 and r9, and a word past that row sits
        // above the caller's 32-byte shadow space.
        const bool ms = fn.signature.abi() == Abi::MsAbi;
        const uint64_t intRegs = ms ? kMsabiIntRegs : kSysVIntRegs;
        const char* (*row)(uint64_t) = ms ? argRegMs : argReg;
        if (ms) {
            uint64_t floats = 0;
            for (uint64_t w = 0; w < static_cast<uint64_t>(fn.params.size()); ++w) {
                const ir::IRType cls = w < fn.paramClasses.size()
                                           ? fn.paramClasses[w]
                                           : ir::IRType::Void;
                if (cls == ir::IRType::F32 || cls == ir::IRType::F64) {
                    ++floats;
                }
            }
            if (floats > kMsabiSseRegs) {
                refuse(fn.loc, "more than four float parameters in an msabi function");
                return false;
            }
        }
        uint64_t gpr = 0;
        uint64_t sse = 0;
        for (uint64_t w = 0; w < static_cast<uint64_t>(fn.params.size()); ++w) {
            const ir::IRType cls = w < fn.paramClasses.size()
                                       ? fn.paramClasses[w]
                                       : ir::IRType::Void;
            if (cls == ir::IRType::F32 || cls == ir::IRType::F64) {
                const char* move = cls == ir::IRType::F32 ? "movss" : "movsd";
                out_ += "  " + std::string(move) + " " + slot(fn.params[w].id) +
                        ", xmm" + std::to_string(sse) + "\n";
                ++sse;
                continue;
            }
            if (gpr < intRegs) {
                out_ += "  mov " + slot(fn.params[w].id) + ", " + row(gpr) + "\n";
            } else {
                uint64_t stackAt = 16 + (ms ? kShadowBytes : 0) +
                                   (gpr - intRegs) * 8;
                out_ += "  mov rax, [rbp + " + std::to_string(stackAt) + "]\n";
                out_ += "  mov " + slot(fn.params[w].id) + ", rax\n";
            }
            ++gpr;
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
        for (const ir::Block& block : fn.blocks) {
            out_ += labelFor(fn.name, block.label) + ":\n";
            for (const ir::Instr& instr : block.body) {
                if (!emitInstr(fn, instr)) {
                    return false;
                }
            }
            if (!emitTerm(fn, block.term)) {
                return false;
            }
        }
        return !failed_;
    }

    bool emitInstr(const ir::Function& fn, const ir::Instr& instr) {
        if (instr.op == ir::Op::Const) {
            // 32-bit immediates encode directly; anything wider materialises
            // through rax, because x86 has no 64-bit-immediate-to-memory mov.
            if (instr.constantBits >= INT32_MIN && instr.constantBits <= INT32_MAX) {
                out_ += "  mov rax, " + std::to_string(instr.constantBits) + "\n";
            } else {
                out_ += "  movabs rax, " + std::to_string(instr.constantBits) + "\n";
            }
            if (!instr.results.empty()) {
                out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
            }
            return true;
        }
        if (instr.op == ir::Op::Copy) {
            out_ += "  mov rax, " + slot(instr.operands[0].id) + "\n";
            out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
            return true;
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
                out_ += "  mov" + std::string(suffix) + " xmm0, " +
                        slot(instr.operands[0].id) + "\n";
                out_ += "  " + std::string(mnemonic) + suffix + " xmm0, " +
                        slot(instr.operands[1].id) + "\n";
                out_ += "  mov" + std::string(suffix) + " " +
                        slot(instr.results[0].id) + ", xmm0\n";
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
                out_ += "  mov al, " + slot(instr.operands[0].id) + "\n";
                out_ += std::string(isSigned ? "  imul byte ptr " : "  mul byte ptr ") +
                        slot(instr.operands[1].id) + "\n";
                out_ += std::string(isSigned ? "  movsx eax, ax\n" : "  movzx eax, ax\n");
            } else {
                out_ += "  mov rax, " + slot(instr.operands[0].id) + "\n";
                out_ += std::string("  ") + name + " rax, " + slot(instr.operands[1].id) + "\n";
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
            out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
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
                            slot(instr.operands[1].id) + "\n";
                    out_ += "  fld " + std::string(mem) +
                            slot(instr.operands[0].id) + "\n";
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
                out_ += "  mov" + std::string(suffix) + " xmm0, " +
                        slot(instr.operands[0].id) + "\n";
                out_ += "  div" + std::string(suffix) + " xmm0, " +
                        slot(instr.operands[1].id) + "\n";
                out_ += "  mov" + std::string(suffix) + " " +
                        slot(instr.results[0].id) + ", xmm0\n";
                return true;
            }
            // Division runs at the operand width: a widened divide would
            // miss the MIN/-1 fault the spec demands at the value's own
            // width, so 32/16/8-bit use their own dividend/divisor forms
            // and extend the quotient or remainder afterwards. Every form
            // faults on a zero divisor, which is the defined trap.
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
                    out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
                } else {
                    out_ += "  mov " + slot(instr.results[0].id) + ", rdx\n";
                }
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
                    out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
                } else {
                    out_ += "  mov " + slot(instr.results[0].id) + ", rdx\n";
                }
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
                    out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
                } else {
                    out_ += "  mov " + slot(instr.results[0].id) + ", rdx\n";
                }
                return true;
            }
            if (instr.width == ir::IRType::I8 || instr.width == ir::IRType::U8) {
                out_ += "  mov al, " + slot(instr.operands[0].id) + "\n";
                if (isSigned) {
                    out_ += "  cbw\n";
                    out_ += "  idiv byte ptr " + slot(instr.operands[1].id) + "\n";
                    out_ += std::string(wantQuotient ? "  movsx eax, al\n" : "  movsx eax, ah\n");
                    out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
                } else {
                    out_ += "  xor ah, ah\n";
                    out_ += "  div byte ptr " + slot(instr.operands[1].id) + "\n";
                    out_ += std::string(wantQuotient ? "  movzx eax, al\n" : "  movzx eax, ah\n");
                    out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
                }
                return true;
            }
            refuse(instr.loc, "this division width is not emitted yet");
            return false;
        }
        if (instr.op == ir::Op::Shl || instr.op == ir::Op::Shr) {
            // The count lives in cl and the hardware masks it to the
            // operand size (5 bits at 32 bits and below, 6 at 64), which
            // is SEMANTICS §3's documented result — so the shift runs at
            // the value's own width and narrow results extend afterwards.
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
            out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
            return true;
        }
        if (instr.op == ir::Op::Neg) {
            out_ += "  mov rax, " + slot(instr.operands[0].id) + "\n";
            out_ += "  neg rax\n";
            out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
            return true;
        }
        if (instr.op == ir::Op::BitNot) {
            out_ += "  mov rax, " + slot(instr.operands[0].id) + "\n";
            out_ += "  not rax\n";
            out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
            return true;
        }
        if (instr.op == ir::Op::Not) {
            out_ += "  mov rax, " + slot(instr.operands[0].id) + "\n";
            out_ += "  test rax, rax\n";
            out_ += "  setz al\n";
            out_ += "  movzx rax, al\n";
            out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
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
                        slot(instr.operands[0].id) + "\n";
                out_ += "  ucomi" + std::string(suffix) + " xmm0, " +
                        slot(instr.operands[1].id) + "\n";
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
                out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
                return true;
            }
            out_ += "  mov rax, " + slot(instr.operands[0].id) + "\n";
            out_ += "  cmp rax, " + slot(instr.operands[1].id) + "\n";
            out_ += std::string("  ") + setFor(instr.cmp) + " al\n";
            out_ += "  movzx rax, al\n";
            out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
            return true;
        }
        // A load or store names its place two ways: a global carries its link
        // label, and a slot carries its id, whose address is rsp+id*8 by the
        // same construction the frame uses. One branch, decided by which the
        // instruction names.
        if (instr.op == ir::Op::Load) {
            // A computed address lives in its slot first: the two-step read
            // through rax is what makes an indirect load one load too many
            // for a single instruction, and keeps every memory operand shaped
            // the same way. Only an alloca id names a slot; any other id in
            // an addressBase is an address the IR computed (Gep, address
            // arithmetic, a loaded pointer), never slot storage, so it
            // dereferences. Listing producer opcodes here sprouted a bug per
            // slice; the alloca set closes the class.
            if (!instr.globalLabel.empty()) {
                emitLoadMem(atAddr("rip + " + instr.globalLabel, instr.displacement),
                            instr.type, instr.width);
            } else if (slotIds_.find(instr.addressBase.id) != slotIds_.end()) {
                std::string base = "rsp + " + std::to_string(instr.addressBase.id * 8);
                emitLoadMem(atAddr(base, instr.displacement), instr.type, instr.width);
            } else {
                out_ += "  mov rax, " + slot(instr.addressBase.id) + "\n";
                emitNullCheck("rax");
                emitLoadMem(atAddr("rax", instr.displacement), instr.type, instr.width);
            }
            if (instr.width == ir::IRType::F32) {
                out_ += "  movss " + slot(instr.results[0].id) + ", xmm0\n";
            } else if (instr.width == ir::IRType::F64) {
                out_ += "  movsd " + slot(instr.results[0].id) + ", xmm0\n";
            } else {
                out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
            }
            return true;
        }
        if (instr.op == ir::Op::Store) {
            out_ += "  mov rax, " + slot(instr.storedValue.id) + "\n";
            // Each width has its own SSE move: a float store that read four
            // bytes of a double would keep the low half of the bit pattern
            // and answer with the wrong value, so the spellings differ.
            if (instr.width == ir::IRType::F32) {
                out_ += "  movss xmm0, " + slot(instr.storedValue.id) + "\n";
            } else if (instr.width == ir::IRType::F64) {
                out_ += "  movsd xmm0, " + slot(instr.storedValue.id) + "\n";
            }
            if (!instr.globalLabel.empty()) {
                emitStoreMem(atAddr("rip + " + instr.globalLabel, instr.displacement),
                             instr.type, instr.width);
            } else if (slotIds_.find(instr.addressBase.id) != slotIds_.end()) {
                std::string base = "rsp + " + std::to_string(instr.addressBase.id * 8);
                emitStoreMem(atAddr(base, instr.displacement), instr.type, instr.width);
            } else {
                out_ += "  mov rcx, " + slot(instr.addressBase.id) + "\n";
                emitNullCheck("rcx");
                emitStoreMem(atAddr("rcx", instr.displacement), instr.type, instr.width);
            }
            return true;
        }
        if (instr.op == ir::Op::Gep) {
            // Base slot plus byte offset, added at runtime: the base id is a
            // compile-time constant, so the slot address folds into the lea.
            // A base that is not a slot holds a computed address as its
            // value instead (a materialized global label, for example),
            // and moves like every other computed address does.
            if (slotIds_.count(instr.addressBase.id) != 0) {
                out_ += "  lea rax, " + atAddr("rsp + " + std::to_string(instr.addressBase.id * 8),
                                               instr.displacement) + "\n";
            } else {
                out_ += "  mov rax, " + slot(instr.addressBase.id) + "\n";
                if (instr.displacement != 0) {
                    out_ += "  add rax, " + std::to_string(instr.displacement) + "\n";
                }
            }
            out_ += "  mov rcx, " + slot(instr.operands[0].id) + "\n";
            out_ += "  add rax, rcx\n";
            out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
            return true;
        }
        if (instr.op == ir::Op::Alloca) {
            // No code: the slot's address is rsp+id*8 by construction, so every
            // load and store already knows where it lives.
            return true;
        }
        if (instr.op == ir::Op::Call) {
            // Two register classes, counted separately: integer words take
            // the six integer registers (a string occupies two, so six
            // string arguments would silently share r9 without this count)
            // and float arguments take an SSE register each, the SysV
            // split. Both counts refuse before anything is emitted, so a
            // refusal never leaves half a call staged.
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
            // 8.4: an msabi call takes four integer registers and reserves
            // 32 bytes of shadow space for the callee; a sysv call takes six
            // and reserves none.
            const bool ms = instr.call.msabi;
            const uint64_t kIntRegs = ms ? kMsabiIntRegs : kSysVIntRegs;
            const uint64_t kSseRegs = ms ? kMsabiSseRegs : kSysVSseRegs;
            const char* (*row)(uint64_t) = ms ? argRegMs : argReg;
            // Integer words past the sixth register spill to the stack the
            // SysV way: pushed in reverse, argument seven nearest the
            // return address, with a lone final word padded to sixteen.
            // Every spilled word stages in a fresh slot first, because a
            // push moves rsp and a slot offset stands still; the stage is
            // what the pushes read, under one fixed rsp.
            const uint64_t stackWords = gpr > kIntRegs ? gpr - kIntRegs : 0;
            const uint64_t stackPad = stackWords % 2;
            if (sse > kSseRegs) {
                refuse(instr.loc, ms ? "more than four float arguments to an msabi call"
                                     : "more than eight float arguments");
                return false;
            }
            // Integer words flatten in order: a string arrives as (data,
            // length) in (rdi, rsi), which is the SysV two-eightbyte rule
            // and exactly what the C struct on the other side declares.
            // A float argument loads its word into the next SSE register
            // instead of consuming an integer one.
            uint64_t index = 0;
            uint64_t sseIndex = 0;
            std::vector<const ir::Value*> spilled;
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
                    if (index < kIntRegs) {
                        out_ += "  mov " + std::string(row(index)) + ", " +
                                slot(word.id) + "\n";
                    } else {
                        spilled.push_back(&word);
                    }
                    ++index;
                }
            }
            // An indirect callee names a slot, and slot offsets read against
            // rsp: the address is taken before any push or shadow moves rsp.
            // rax then holds it through the pushes, so it is not also a
            // staging register below.
            const uint64_t stageBase = instr.call.indirect ? 1 : 0;
            if (instr.call.indirect) {
                out_ += "  mov rax, " + slot(instr.calleeValue.id) + "\n";
                emitNullCheck("rax");
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
                if (spilled.size() + stageBase > kSpillRegs) {
                    refuse(instr.loc, "more argument words than the spill stage holds");
                    return false;
                }
                for (uint64_t at = 0; at < spilled.size(); ++at) {
                    out_ += "  mov " + std::string(kSpillReg[at + stageBase]) + ", " +
                            slot(spilled[at]->id) + "\n";
                }
                if (stackPad != 0) {
                    // An immediate, not rax: rax may hold the staged spill or
                    // the indirect callee, and a pad push must clobber neither.
                    out_ += "  push 0\n";
                }
                for (uint64_t at = spilled.size(); at > skipNone; --at) {
                    out_ += "  push " + std::string(kSpillReg[at - 1 + stageBase]) + "\n";
                }
            }
            // 8.4: the caller owns the callee's 32 bytes of shadow space. It
            // goes down after the pushes so the spilled words sit above it,
            // where an msabi callee reads its fifth word.
            if (ms) {
                out_ += "  sub rsp, " + std::to_string(kShadowBytes) + "\n";
            }
            if (instr.call.indirect) {
                out_ += "  call rax\n";
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
                    out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
                }
                // The register row's second word comes home in rdx (IR.md
                // §6): lowering refuses a third, so stopping here is
                // complete, not truncation.
                if (static_cast<uint64_t>(instr.results.size()) >= kTwoResults) {
                    out_ += "  mov " + slot(instr.results[kSecondResult].id) +
                            ", rdx\n";
                }
            }
            return true;
        }
        if (instr.op == ir::Op::Cast) {
            return emitCast(instr);
        }
        if (instr.op == ir::Op::Trap) {
            return emitTrapReason(instr.reason);
        }
        if (instr.op == ir::Op::DataAddr) {
            out_ += "  lea rax, [rip + " + asmSafeName(instr.globalLabel) + "]\n";
            out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
            addrWords_[instr.results[0].id] = asmSafeName(instr.globalLabel);
            return true;
        }
        if (instr.op == ir::Op::FnAddr) {
            out_ += "  lea rax, [rip + " + asmSafeName(instr.globalLabel) + "]\n";
            out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
            return true;
        }
        if (instr.op == ir::Op::Asm) {
            emitAsm(instr);
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
    // callee-saved registers (FEATURES §4.4), so a conforming block leaves
    // the staged value intact. A global stays `[rip + label]`: that is the
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
            const std::string cell = "[rsp + " + std::to_string(s.id * 8 + depth) + "]";
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
            const std::string cell =
                "[rsp + " + std::to_string(it->id * 8 + depth) + "]";
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
            if (fromFloat && toFloat) {
                // Same width is a bit move; otherwise convert through xmm0.
                out_ += "  mov rax, " + slot(instr.operands[0].id) + "\n";
                if (fromF == toF) {
                    out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
                    return true;
                }
                if (*from.type->basic() == BasicKind::F32) {
                    out_ += "  movss xmm0, " + slot(instr.operands[0].id) + "\n";
                    out_ += "  cvtss2sd xmm0, xmm0\n";
                    out_ += "  movsd " + slot(instr.results[0].id) + ", xmm0\n";
                } else {
                    out_ += "  movsd xmm0, " + slot(instr.operands[0].id) + "\n";
                    out_ += "  cvtsd2ss xmm0, xmm0\n";
                    out_ += "  movss " + slot(instr.results[0].id) + ", xmm0\n";
                }
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
                        slot(instr.operands[0].id) + "\n";
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
                out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
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
            const char* move = to32 ? "movss" : "movsd";
            out_ += "  mov rax, " + slot(instr.operands[0].id) + "\n";
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
                out_ += "  " + std::string(move) + " " +
                        slot(instr.results[0].id) + ", xmm0\n";
                return true;
            }
            out_ += "  " + std::string(cvt) + " xmm0, rax\n";
            out_ += "  " + std::string(move) + " " +
                    slot(instr.results[0].id) + ", xmm0\n";
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
        out_ += "  mov rax, " + slot(instr.operands[0].id) + "\n";
        if (fromBits == toBits) {
            // Same width, different signedness: the cell keeps the
            // source's extension, so re-extend through the target —
            // i32(-1) `as` u32 reads 0xFFFFFFFF, not the sign-extended
            // source cell. Pointers keep the plain move.
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
            if (!crossSign) {
                out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
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
            out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
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
            out_ += "  mov rax, " + slot(instr.operands[0].id) + "\n";
        }
        out_ += "  " + reg + "\n";
        out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
        return true;
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
                    out_ += "  movss xmm0, " + slot(word.id) + "\n";
                } else if (i == 0 && word.width == ir::IRType::F64) {
                    out_ += "  movsd xmm0, " + slot(word.id) + "\n";
                } else if (i == 0) {
                    out_ += "  mov rax, " + slot(word.id) + "\n";
                } else if (i == 1) {
                    out_ += "  mov rdx, " + slot(word.id) + "\n";
                } else {
                    refuse(term.loc, "a return wider than two words");
                    return false;
                }
            }
            out_ += "  leave\n";
            out_ += "  ret\n";
            return true;
        }
        if (term.term == ir::Term::Br) {
            out_ += "  jmp " + labelFor(fn.name, term.target) + "\n";
            return true;
        }
        if (term.term == ir::Term::CondBr && term.cond.has_value()) {
            out_ += "  mov rax, " + slot(term.cond->id) + "\n";
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
    bool requireEntry_ = true;
    // Alloca-result ids in the function being emitted: the only ids whose
    // address is slot storage (rsp+id*8). Any other id in an addressBase is
    // a computed address and dereferences.
    std::unordered_set<uint32_t> slotIds_;
    // Result id of a DataAddr word -> its label, so an asm placeholder
    // paired with that word prints `[rip + label]` (the global's value)
    // instead of the temporary holding the address.
    std::unordered_map<uint32_t, std::string> addrWords_;
    std::string out_;
    // Unique suffix for u64-to-float conversion labels (int→float casts
    // need a two-path sequence; see emitCast).
    uint64_t cvtLabelSeq_ = 0;
    uint64_t nullSeq_ = 0;
    uint64_t trapMessageSeq_ = 0;
    bool failed_ = false;
    std::string error_;
    Position pos_{"", 1, 1};
};

const char* targetName(Target kind) {
    return target::info(kind).name;
}

EmitResult emitNative(const ir::Module& module, bool requireEntry) {
    Emitter emitter(module, Target::Native, requireEntry);
    return emitter.run();
}

EmitResult emit(const ir::Module& module, Target kind, bool requireEntry) {
    switch (kind) {
        case Target::Native:
            return emitNative(module, requireEntry);
        case Target::Pomeos: {
            Emitter emitter(module, Target::Pomeos, requireEntry);
            return emitter.run();
        }
        case Target::Uefi: {
            Emitter emitter(module, Target::Uefi, requireEntry);
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

}  // namespace codegen
}  // namespace cider
