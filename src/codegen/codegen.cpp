// codegen.cpp — checked IR to x86-64 assembly (SysV, Linux).
//
// Intel syntax with `.intel_syntax noprefix`, so plain `cc file.s` assembles
// it. Every SSA value lives in its own stack slot and six scratch registers
// carry values between slot and operation: correct by construction, slow by
// design. A register allocator replaces this strategy, not its interface.
//
// The text is a pure function of the module: no timestamps, no working
// directories, no absolute paths. Gate L5.

#include "codegen/codegen.h"

#include <unordered_set>
#include <vector>

namespace cider {
namespace codegen {

namespace {

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

bool signedWidth(ir::IRType width, TypeRef type) {
    if (width == ir::IRType::I8 || width == ir::IRType::I16 || width == ir::IRType::I32 ||
        width == ir::IRType::I64) {
        return true;
    }
    if (width == ir::IRType::U8 || width == ir::IRType::U16 || width == ir::IRType::U32 ||
        width == ir::IRType::U64) {
        return false;
    }
    if (type.isError() || type.type->kind() != Kind::Basic) {
        return true;
    }
    return basicIsSigned(*type.type->basic());
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
    explicit Emitter(const ir::Module& module) : module_(module) {}

    EmitResult run() {
        out_ += ".intel_syntax noprefix\n.text\n";
        for (const ir::Function& fn : module_.functions) {
            if (!emitFunction(fn)) {
                return EmitResult{false, "", error_, pos_};
            }
        }
        // The entry point and the prelude helpers are emitted here, in this
        // one place, because sovereignty is a property of the whole text: a
        // program links with `ld` alone, no libc, no crt0, no archive.
        std::vector<std::string> helpers = neededHelpers();
        if (!emitEntry()) {
            return EmitResult{false, "", error_, pos_};
        }
        for (const std::string& helper : helpers) {
            if (!emitHelper(helper)) {
                return EmitResult{false, "", error_, pos_};
            }
        }
        bool heap = false;
        bool panic = false;
        for (const std::string& helper : helpers) {
            if (helper == "cider_alloc" || helper == "cider_realloc" ||
                helper == "cider_free") {
                heap = true;
            }
            if (helper == "cider_panic") {
                panic = true;
            }
        }
        if (!module_.globals.empty()) {
            out_ += ".data\n";
            for (const ir::Global& global : module_.globals) {
                out_ += global.label + ":\n";
                if (global.initLabel.empty()) {
                    out_ += "  .quad " + std::to_string(global.initial) + "\n";
                } else {
                    // A string global is its bytes' address followed by its
                    // length — the same two words a literal lowers to.
                    out_ += "  .quad " + global.initLabel + "\n";
                    out_ += "  .quad " + std::to_string(global.initial) + "\n";
                }
            }
        }
        if (heap) {
            if (module_.globals.empty()) {
                out_ += ".data\n";
            }
            // The allocator's two words of state: the free-list head and the
            // bytes mapped so far. Zero-initialized images carry them.
            out_ += "cider_fl_head:\n  .quad 0\n";
            out_ += "cider_heap_total:\n  .quad 0\n";
        }
        if (!module_.rodata.empty() || panic || heap) {
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
            if (panic) {
                out_ += "cider_panic_nl:\n  .byte 10\n";
            }
            if (heap) {
                out_ += "cider_heap_msg:\n";
                out_ += "  .byte 99,105,100,101,114,32,104,101,97,112,58,32,99,111,114,114,117,112,116,32,98,108,111,99,107,32,104,101,97,100,101,114,10\n";
                out_ += "cider_heap_msg_end:\n";
            }
        }
        return EmitResult{true, out_, "", Position{"", 1, 1}};
    }

private:
    // Prelude helpers the text actually references, in a fixed emission
    // order so the bytes stay deterministic. A helper the module itself
    // defines (the name is taken) is skipped: the program's own spelling
    // wins, as FEATURES.md §7 promises for shadowing.
    std::vector<std::string> neededHelpers() {
        std::unordered_set<std::string> defined;
        for (const ir::Function& fn : module_.functions) {
            defined.insert(fn.name);
        }
        std::unordered_set<std::string> seen;
        std::vector<std::string> order = {
            "cider_print", "cider_panic", "cider_alloc", "cider_free",
            "cider_realloc",
        };
        std::vector<std::string> out;
        for (const ir::Function& fn : module_.functions) {
            for (const ir::Block& block : fn.blocks) {
                for (const ir::Instr& instr : block.body) {
                    if (instr.op != ir::Op::Call) {
                        continue;
                    }
                    if (defined.count(instr.callee) != 0) {
                        continue;
                    }
                    if (seen.insert(instr.callee).second) {
                        for (const std::string& candidate : order) {
                            if (candidate == instr.callee) {
                                out.push_back(candidate);
                            }
                        }
                    }
                }
            }
        }
        return out;
    }

    // The process entry. A program names its entry by defining `main` or
    // `_start` (FEATURES.md §10); the kernel enters at `_start` either way.
    // When only `main` exists this trampoline bridges the two worlds: it
    // reads argc/argv where the kernel left them, calls main at the ABI's
    // register positions, and exits with main's answer. No crt0 exists in
    // the link, so this text is the only thing between the kernel and the
    // program.
    bool emitEntry() {
        const ir::Function* mainFn = nullptr;
        const ir::Function* startFn = nullptr;
        for (const ir::Function& fn : module_.functions) {
            if (fn.name == "main") {
                mainFn = &fn;
            }
            if (fn.name == "_start") {
                startFn = &fn;
            }
        }
        if (mainFn != nullptr && startFn != nullptr) {
            refuse(mainFn->loc, "the program defines both main and _start");
            return false;
        }
        if (mainFn == nullptr && startFn == nullptr) {
            refuse(Position{"", 1, 1}, "no entry function: define main or _start");
            return false;
        }
        if (startFn != nullptr) {
            if (!startFn->signature.parameters().empty()) {
                refuse(startFn->loc, "_start takes no parameters");
                return false;
            }
            return true;
        }
        uint64_t arity = mainFn->signature.arity();
        if (arity > 2) {
            refuse(mainFn->loc, "main takes at most two parameters");
            return false;
        }
        const Type* ret = mainFn->signature.returnType();
        if (ret == nullptr || ret->kind() != Kind::Basic ||
            (*ret->basic() != BasicKind::I32 && *ret->basic() != BasicKind::I64)) {
            refuse(mainFn->loc, "main must return int");
            return false;
        }
        out_ += ".globl _start\n";
        out_ += "_start:\n";
        out_ += "  xor ebp, ebp\n";
        if (arity >= 1) {
            out_ += "  mov rdi, [rsp]\n";        // argc, where the kernel left it
        }
        if (arity >= 2) {
            out_ += "  lea rsi, [rsp + 8]\n";    // argv, the array after argc
        }
        out_ += "  call main\n";
        out_ += "  mov rdi, rax\n";
        out_ += "  mov eax, 60\n";               // exit(main's answer)
        out_ += "  syscall\n";
        return true;
    }

    // One prelude helper, emitted as text only when referenced. Each one is
    // raw syscalls: there is no libc behind this compiler, and no archive
    // behind these symbols.
    bool emitHelper(const std::string& name) {
        if (emittedHelpers_.insert(name).second == false) {
            return true;                         // already in the text once
        }
        if (name == "cider_print") {
            // (data, length) in (rdi, rsi) — the two words a string lowers
            // to. A non-positive length writes nothing.
            out_ += "cider_print:\n";
            out_ += "  test rsi, rsi\n";
            out_ += "  jle .done\n";
            out_ += "  mov rdx, rsi\n";
            out_ += "  mov rsi, rdi\n";
            out_ += "  mov eax, 1\n";
            out_ += "  mov edi, 1\n";
            out_ += "  syscall\n";
            out_ += ".done:\n";
            out_ += "  ret\n";
            return true;
        }
        if (name == "cider_panic") {
            // The message on stderr, a newline, then exit 134 — the code the
            // VM answers for panic, so the two agree without a flag.
            out_ += "cider_panic:\n";
            out_ += "  test rsi, rsi\n";
            out_ += "  jle .newline\n";
            out_ += "  mov rdx, rsi\n";
            out_ += "  mov rsi, rdi\n";
            out_ += "  mov eax, 1\n";
            out_ += "  mov edi, 2\n";
            out_ += "  syscall\n";
            out_ += ".newline:\n";
            out_ += "  mov eax, 1\n";
            out_ += "  mov edi, 2\n";
            out_ += "  lea rsi, [rip + cider_panic_nl]\n";
            out_ += "  mov edx, 1\n";
            out_ += "  syscall\n";
            out_ += "  mov eax, 60\n";
            out_ += "  mov edi, 134\n";
            out_ += "  syscall\n";
            return true;
        }
        if (name == "cider_heap_fail") {
            out_ += "cider_heap_fail:\n";
            out_ += "  mov eax, 1\n";
            out_ += "  mov edi, 2\n";
            out_ += "  lea rsi, [rip + cider_heap_msg]\n";
            // GAS intel syntax reads a bare symbol as a memory operand, so
            // the length rides as the literal it is (the message above).
            out_ += "  mov edx, 33\n";
            out_ += "  syscall\n";
            out_ += "  mov eax, 60\n";
            out_ += "  mov edi, 134\n";
            out_ += "  syscall\n";
            return true;
        }
        if (name == "cider_alloc") {
            return emitAlloc();
        }
        if (name == "cider_free") {
            return emitFree();
        }
        if (name == "cider_realloc") {
            return emitRealloc();
        }
        refuse(Position{"", 1, 1}, "unknown prelude helper '" + name + "'");
        return false;
    }

    // The heap. A first-fit free list over anonymous mmap arenas; no libc.
    //
    // Block layout, offsets from the user pointer:
    //   [-32] user bytes   [-24] block base   [-16] block bytes
    //   [ -8] front canary [0..bytes) user    [bytes] rear canary
    // A free block is its own list node: [0] block bytes, [8] next node.
    //
    // Refusals answer 0 (nilPtr): negative bytes, non-positive alignment, a
    // non-power-of-two alignment above 16, or the heap cap. free(nilPtr) is
    // a defined no-op. A broken canary or a cyclic free list dies loudly on
    // stderr with exit 134 rather than continuing on corrupt memory.
    bool emitAlloc() {
        emitHelper("cider_heap_fail");
        out_ += "cider_alloc:\n";
        out_ += "  cmp rdi, 0\n";
        out_ += "  jl .refuse\n";
        out_ += "  cmp rsi, 0\n";
        out_ += "  jle .refuse\n";
        out_ += "  push rbx\n";
        out_ += "  push rbp\n";
        out_ += "  push r12\n";
        out_ += "  push r13\n";
        out_ += "  mov rbx, rdi\n";              // the user's byte count
        out_ += "  cmp rsi, 16\n";
        out_ += "  jge .maybePow\n";
        out_ += "  mov rsi, 16\n";
        out_ += ".maybePow:\n";
        out_ += "  je .alignDone\n";
        out_ += "  lea rdx, [rsi - 1]\n";
        out_ += "  test rsi, rdx\n";
        out_ += "  jnz .refusePop\n";            // above 16 must be a power of two
        out_ += ".alignDone:\n";
        out_ += "  mov r12, rsi\n";              // effective alignment
        out_ += "  mov rbp, rbx\n";
        out_ += "  add rbp, 40\n";               // header + rear canary + rounding headroom
        out_ += "  jo .refusePop\n";
        out_ += "  add rbp, 15\n";
        out_ += "  and rbp, -16\n";
        out_ += "  cmp r12, 16\n";
        out_ += "  jle .needDone\n";
        out_ += "  add rbp, r12\n";              // room to align the user pointer
        out_ += "  jo .refusePop\n";
        out_ += ".needDone:\n";
        out_ += "  xor r13, r13\n";              // the node before the cursor
        out_ += ".restart:\n";
        out_ += "  mov r11, [rip + cider_fl_head]\n";
        out_ += "  xor r8, r8\n";                // steps walked
        out_ += ".search:\n";
        out_ += "  test r11, r11\n";
        out_ += "  jz .grow\n";
        out_ += "  add r8, 1\n";
        out_ += "  cmp r8, 65536\n";
        out_ += "  ja cider_heap_fail\n";        // a cycle is corruption
        out_ += "  mov rdx, [r11]\n";            // this block's byte count
        out_ += "  cmp rdx, rbp\n";
        out_ += "  jb .advance\n";
        out_ += "  mov rax, rdx\n";
        out_ += "  sub rax, rbp\n";
        out_ += "  cmp rax, 48\n";
        out_ += "  jb .unlink\n";                // tail too small to stand alone
        out_ += "  lea r9, [r11 + rbp]\n";       // the tail node
        out_ += "  mov [r9], rax\n";
        out_ += "  mov rdx, [r11 + 8]\n";
        out_ += "  mov [r9 + 8], rdx\n";
        out_ += "  mov rcx, rbp\n";              // the front keeps `need` bytes
        out_ += "  test r13, r13\n";
        out_ += "  jz .splitHead\n";
        out_ += "  mov [r13 + 8], r9\n";
        out_ += "  jmp .carve\n";
        out_ += ".splitHead:\n";
        out_ += "  mov [rip + cider_fl_head], r9\n";
        out_ += "  jmp .carve\n";
        out_ += ".unlink:\n";
        out_ += "  mov rcx, [r11]\n";            // the whole block goes out
        out_ += "  mov rdx, [r11 + 8]\n";
        out_ += "  test r13, r13\n";
        out_ += "  jz .unlinkHead\n";
        out_ += "  mov [r13 + 8], rdx\n";
        out_ += "  jmp .carve\n";
        out_ += ".unlinkHead:\n";
        out_ += "  mov [rip + cider_fl_head], rdx\n";
        out_ += "  jmp .carve\n";
        out_ += ".advance:\n";
        out_ += "  mov r13, r11\n";
        out_ += "  mov r11, [r11 + 8]\n";
        out_ += "  jmp .search\n";
        out_ += ".grow:\n";
        out_ += "  mov rdx, 1048576\n";          // one mebibyte at a time
        out_ += "  lea rax, [rbp + 16]\n";
        out_ += "  cmp rax, rdx\n";
        out_ += "  jbe .haveLen\n";
        out_ += "  mov rdx, rax\n";              // a request larger than one arena
        out_ += ".haveLen:\n";
        out_ += "  mov rax, [rip + cider_heap_total]\n";
        out_ += "  add rax, rdx\n";
        out_ += "  cmp rax, 1073741824\n";       // the whole heap stays under 1 GiB
        out_ += "  ja .refusePop\n";
        out_ += "  mov rsi, rdx\n";
        out_ += "  mov rax, 9\n";                // mmap
        out_ += "  xor rdi, rdi\n";
        out_ += "  mov rdx, 3\n";                // PROT_READ | PROT_WRITE
        out_ += "  mov r10, 34\n";               // MAP_PRIVATE | MAP_ANONYMOUS
        out_ += "  mov r8, -1\n";
        out_ += "  xor r9, r9\n";
        out_ += "  syscall\n";
        out_ += "  test rax, rax\n";
        out_ += "  js .refusePop\n";
        out_ += "  mov [rax], rsi\n";            // the arena starts as one node
        out_ += "  mov rdx, [rip + cider_fl_head]\n";
        out_ += "  mov [rax + 8], rdx\n";
        out_ += "  mov [rip + cider_fl_head], rax\n";
        out_ += "  add qword [rip + cider_heap_total], rsi\n";
        out_ += "  jmp .restart\n";
        out_ += ".carve:\n";
        out_ += "  cmp r12, 16\n";
        out_ += "  jg .alignedUser\n";
        out_ += "  lea rax, [r11 + 32]\n";       // 32 keeps the user 16-aligned
        out_ += "  jmp .haveUser\n";
        out_ += ".alignedUser:\n";
        out_ += "  lea rax, [r11 + r12 + 31]\n";
        out_ += "  mov rdx, r12\n";
        out_ += "  neg rdx\n";
        out_ += "  and rax, rdx\n";
        out_ += ".haveUser:\n";
        out_ += "  mov [rax - 32], rbx\n";       // user bytes
        out_ += "  mov [rax - 24], r11\n";       // block base
        out_ += "  mov [rax - 16], rcx\n";       // block bytes
        out_ += "  mov rdx, 0x9E3779B97F4A7C15\n";
        out_ += "  mov [rax - 8], rdx\n";        // front canary
        out_ += "  mov [rax + rbx], rdx\n";      // rear canary
        out_ += "  pop r13\n";
        out_ += "  pop r12\n";
        out_ += "  pop rbp\n";
        out_ += "  pop rbx\n";
        out_ += "  ret\n";
        out_ += ".refusePop:\n";
        out_ += "  xor eax, eax\n";
        out_ += "  pop r13\n";
        out_ += "  pop r12\n";
        out_ += "  pop rbp\n";
        out_ += "  pop rbx\n";
        out_ += ".refuse:\n";
        out_ += "  xor eax, eax\n";
        out_ += "  ret\n";
        return true;
    }

    bool emitFree() {
        emitHelper("cider_heap_fail");
        out_ += "cider_free:\n";
        out_ += "  test rdi, rdi\n";
        out_ += "  jz .nothing\n";               // free(nilPtr) does nothing
        out_ += "  push rbx\n";
        out_ += "  mov rbx, rdi\n";
        out_ += "  mov rax, 0x9E3779B97F4A7C15\n";
        out_ += "  cmp [rbx - 8], rax\n";
        out_ += "  jne cider_heap_fail\n";
        out_ += "  mov rcx, [rbx - 32]\n";       // user bytes (header)
        out_ += "  cmp [rbx + rcx], rax\n";
        out_ += "  jne cider_heap_fail\n";
        out_ += "  mov rdx, 0xDEADDEADDEADDEAD\n";
        out_ += "  mov [rbx - 8], rdx\n";        // scrub: use-after-free reads dead
        out_ += "  mov [rbx + rcx], rdx\n";
        out_ += "  mov rax, [rbx - 24]\n";       // block base
        out_ += "  mov rdx, [rbx - 16]\n";       // block bytes
        out_ += "  mov [rax], rdx\n";
        out_ += "  mov rcx, [rip + cider_fl_head]\n";
        out_ += "  mov [rax + 8], rcx\n";
        out_ += "  mov [rip + cider_fl_head], rax\n";
        out_ += "  pop rbx\n";
        out_ += ".nothing:\n";
        out_ += "  ret\n";
        return true;
    }

    bool emitRealloc() {
        emitHelper("cider_heap_fail");
        out_ += "cider_realloc:\n";
        out_ += "  test rdi, rdi\n";
        out_ += "  jnz .haveOld\n";
        out_ += "  mov rdi, rsi\n";              // realloc(nilPtr, n) allocates
        out_ += "  mov rsi, rdx\n";
        out_ += "  jmp cider_alloc\n";
        out_ += ".haveOld:\n";
        out_ += "  cmp rsi, 0\n";
        out_ += "  jge .sizeOk\n";
        out_ += "  xor eax, eax\n";
        out_ += "  ret\n";
        out_ += ".sizeOk:\n";
        out_ += "  push rbx\n";
        out_ += "  push r12\n";
        out_ += "  mov rbx, rdi\n";
        out_ += "  mov r12, rsi\n";              // the new byte count
        out_ += "  mov rax, 0x9E3779B97F4A7C15\n";
        out_ += "  cmp [rbx - 8], rax\n";
        out_ += "  jne cider_heap_fail\n";
        out_ += "  mov rcx, [rbx - 32]\n";       // user bytes (header)
        out_ += "  cmp [rbx + rcx], rax\n";
        out_ += "  jne cider_heap_fail\n";
        out_ += "  mov rdi, r12\n";
        out_ += "  mov rsi, rdx\n";              // the alignment rides in rdx
        out_ += "  call cider_alloc\n";
        out_ += "  test rax, rax\n";
        out_ += "  jz .keepOld\n";               // refusal leaves the old block owned
        out_ += "  mov r9, rax\n";
        out_ += "  mov rdx, [rbx]\n";            // old bytes
        out_ += "  cmp rdx, r12\n";
        out_ += "  jbe .haveCount\n";
        out_ += "  mov rdx, r12\n";              // copy the shorter span
        out_ += ".haveCount:\n";
        out_ += "  test rdx, rdx\n";
        out_ += "  jz .dropOld\n";
        out_ += "  mov rsi, rbx\n";
        out_ += "  mov rdi, r9\n";
        out_ += "  mov rcx, rdx\n";
        out_ += "  cld\n";
        out_ += "  rep movsb\n";
        out_ += ".dropOld:\n";
        out_ += "  mov rdi, rbx\n";
        out_ += "  call cider_free\n";
        out_ += "  mov rax, r9\n";
        out_ += ".keepOld:\n";
        out_ += "  pop r12\n";
        out_ += "  pop rbx\n";
        out_ += "  ret\n";
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
            if (term.value.has_value()) {
                note(term.value->id);
            }
            if (term.cond.has_value()) {
                note(term.cond->id);
            }
        }
        uint32_t cells = top + 1;
        uint32_t span = (cells * 8 + 15) & ~15u;

        out_ += ".globl " + fn.name + "\n";
        out_ += fn.name + ":\n";
        out_ += "  push rbp\n";
        out_ += "  mov rbp, rsp\n";
        out_ += "  sub rsp, " + std::to_string(span) + "\n";

        // Parameters arrive in registers (then stack); they live in slots
        // from here on, like every other value.
        uint64_t index = 0;
        for (const ir::Value& param : fn.params) {
            if (index < 6) {
                out_ += "  mov " + slot(param.id) + ", " + argReg(index) + "\n";
            } else {
                uint64_t stackAt = 16 + (index - 6) * 8;
                out_ += "  mov rax, [rbp + " + std::to_string(stackAt) + "]\n";
                out_ += "  mov " + slot(param.id) + ", rax\n";
            }
            ++index;
        }

        slotIds_.clear();
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
            out_ += "  mov rax, " + slot(instr.operands[0].id) + "\n";
            out_ += std::string("  ") + name + " rax, " + slot(instr.operands[1].id) + "\n";
            out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
            return true;
        }
        if (instr.op == ir::Op::Div || instr.op == ir::Op::Rem) {
            // Signed divides sign-extend; unsigned ones zero the top instead.
            // Either faults on a zero divisor, which is the defined trap.
            out_ += "  mov rax, " + slot(instr.operands[0].id) + "\n";
            if (signedWidth(instr.width, instr.type)) {
                out_ += "  cqo\n";
                out_ += "  idiv qword ptr " + slot(instr.operands[1].id) + "\n";
            } else {
                out_ += "  xor edx, edx\n";
                out_ += "  div qword ptr " + slot(instr.operands[1].id) + "\n";
            }
            if (instr.op == ir::Op::Div) {
                out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
            } else {
                out_ += "  mov " + slot(instr.results[0].id) + ", rdx\n";
            }
            return true;
        }
        if (instr.op == ir::Op::Shl || instr.op == ir::Op::Shr) {
            // The count lives in cl: x86 masks it to 5 or 6 bits itself,
            // which is SEMANTICS §3's documented result, not an accident.
            out_ += "  mov rax, " + slot(instr.operands[0].id) + "\n";
            out_ += "  mov rcx, " + slot(instr.operands[1].id) + "\n";
            if (instr.op == ir::Op::Shl) {
                out_ += "  shl rax, cl\n";
            } else if (signedWidth(instr.width, instr.type)) {
                out_ += "  sar rax, cl\n";
            } else {
                out_ += "  shr rax, cl\n";
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
            if (instr.width == ir::IRType::F32 || instr.width == ir::IRType::F64) {
                out_ += "  movss xmm0, " + slot(instr.storedValue.id) + "\n";
            }
            if (!instr.globalLabel.empty()) {
                emitStoreMem(atAddr("rip + " + instr.globalLabel, instr.displacement),
                             instr.type, instr.width);
            } else if (slotIds_.find(instr.addressBase.id) != slotIds_.end()) {
                std::string base = "rsp + " + std::to_string(instr.addressBase.id * 8);
                emitStoreMem(atAddr(base, instr.displacement), instr.type, instr.width);
            } else {
                out_ += "  mov rcx, " + slot(instr.addressBase.id) + "\n";
                emitStoreMem(atAddr("rcx", instr.displacement), instr.type, instr.width);
            }
            return true;
        }
        if (instr.op == ir::Op::Gep) {
            // Base slot plus byte offset, added at runtime: the base id is a
            // compile-time constant, so the slot address folds into the lea.
            out_ += "  lea rax, " + atAddr("rsp + " + std::to_string(instr.addressBase.id * 8),
                                           instr.displacement) + "\n";
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
            // Words, not arguments: a string occupies two registers, so six
            // string arguments would silently share r9 without this count.
            uint64_t arity = 0;
            for (const ir::Argument& arg : instr.arguments) {
                for (const ir::Value& word : arg.words) {
                    (void)word;
                    ++arity;
                }
            }
            const uint64_t regArgs = 6;
            if (arity > regArgs) {
                refuse(instr.loc, "more than six registers of arguments");
                return false;
            }
            // Words flatten in order: a string arrives as (data, length) in
            // (rdi, rsi), which is the SysV two-eightbyte rule and exactly
            // what the C struct on the other side declares.
            uint64_t index = 0;
            for (const ir::Argument& arg : instr.arguments) {
                for (const ir::Value& word : arg.words) {
                    out_ += "  mov " + std::string(argReg(index)) + ", " +
                            slot(word.id) + "\n";
                    ++index;
                }
            }
            if (instr.call.indirect) {
                out_ += "  mov rax, " + slot(instr.calleeValue.id) + "\n";
                out_ += "  call rax\n";
            } else {
                out_ += "  call " + instr.callee + "\n";
            }
            if (!instr.results.empty()) {
                out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
            }
            return true;
        }
        if (instr.op == ir::Op::Cast) {
            return emitCast(instr);
        }
        if (instr.op == ir::Op::Trap) {
            out_ += "  ud2\n";
            return true;
        }
        if (instr.op == ir::Op::DataAddr) {
            out_ += "  lea rax, [rip + " + instr.globalLabel + "]\n";
            out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
            return true;
        }
        refuse(instr.loc, "this instruction is not emitted yet");
        return false;
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
        if (!fromPtr && !toPtr) {
            if (from.type->kind() != Kind::Basic || to.type->kind() != Kind::Basic) {
                refuse(instr.loc, "this cast is not emitted yet");
                return false;
            }
            if (basicIsFloat(*from.type->basic()) || basicIsFloat(*to.type->basic())) {
                refuse(instr.loc, "float casts are not emitted yet");
                return false;
            }
        }
        // A pointer participates as its machine width; everything else
        // answers through the basic table.
        int fromBits = fromPtr ? 64 : basicWidthBits(*from.type->basic());
        int toBits = toPtr ? 64 : basicWidthBits(*to.type->basic());
        bool signedTarget = !toPtr && to.type->kind() == Kind::Basic &&
                            basicIsSigned(*to.type->basic());
        out_ += "  mov rax, " + slot(instr.operands[0].id) + "\n";
        if (fromBits == toBits) {
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
            if (term.value.has_value() && term.value->id != 0xFFFFFFFFu) {
                out_ += "  mov rax, " + slot(term.value->id) + "\n";
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
            // bounds check): halt rather than continue with a known-wrong
            // value. The reason travels in IR for diagnostics; native code
            // has no channel for it, so every reason halts the same way.
            out_ += "  ud2\n";
            return true;
        }
        refuse(term.loc, "this terminator is not emitted yet");
        return false;
    }





    const ir::Module& module_;
    // Alloca-result ids in the function being emitted: the only ids whose
    // address is slot storage (rsp+id*8). Any other id in an addressBase is
    // a computed address and dereferences.
    std::unordered_set<uint32_t> slotIds_;
    std::unordered_set<std::string> emittedHelpers_;
    std::string out_;
    bool failed_ = false;
    std::string error_;
    Position pos_{"", 1, 1};
};

const char* targetName(Target target) {
    if (target == Target::Native) {
        return "native";
    }
    if (target == Target::Pomeos) {
        return "pomeos";
    }
    if (target == Target::Uefi) {
        return "uefi";
    }
    return "vm";
}

EmitResult emitNative(const ir::Module& module) {
    Emitter emitter(module);
    return emitter.run();
}

EmitResult emit(const ir::Module& module, Target target) {
    if (target != Target::Native) {
        EmitResult denied;
        denied.ok = false;
        denied.error = std::string("target ") + targetName(target) +
                       " is not emitted yet: native only in this slice";
        return denied;
    }
    return emitNative(module);
}

}  // namespace codegen
}  // namespace cider
