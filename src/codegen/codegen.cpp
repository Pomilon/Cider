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

#include <unordered_map>
#include <unordered_set>
#include <vector>

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

bool calleeArg(std::string&, std::string&, const ir::Instr&) = delete;  // placeholder

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
                        for (uint64_t word : global.initWords) {
                            out_ += "  .quad " + std::to_string(word) + "\n";
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
            "cider_realloc", "putchar", "cider_itoa_signed",
            "cider_itoa_unsigned", "cider_ftoa",
        };
        std::vector<std::string> out;
        for (const ir::Function& fn : module_.functions) {
            for (const ir::Block& block : fn.blocks) {
                for (const ir::Instr& instr : block.body) {
                    // A helper is needed when a call names it or a function
                    // address takes it: `&print` must emit `cider_print`
                    // exactly as a call would.
                    std::string wanted;
                    if (instr.op == ir::Op::Call) {
                        wanted = instr.callee;
                    } else if (instr.op == ir::Op::FnAddr) {
                        wanted = instr.globalLabel;
                    } else {
                        continue;
                    }
                    if (defined.count(wanted) != 0) {
                        continue;
                    }
                    if (seen.insert(wanted).second) {
                        for (const std::string& candidate : order) {
                            if (candidate == wanted) {
                                out.push_back(candidate);
                            }
                        }
                        // The float helper prints its whole part through the
                        // signed helper: an IR scan never sees that inner
                        // call, so the dependency rides explicitly here.
                        if (wanted == "cider_ftoa" &&
                            seen.insert("cider_itoa_signed").second) {
                            out.push_back("cider_itoa_signed");
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
        if (name == "putchar") {
            // The one blessed extern: one `write` syscall of the low byte,
            // answering the character (FEATURES §4.4 names syscalls through
            // assembly; the freestanding link has no libc to provide this).
            // A program defining its own `putchar` never reaches here, and
            // any other undefined extern still fails the link loudly.
            out_ += "putchar:\n";
            out_ += "  push rdi\n";
            out_ += "  mov eax, 1\n";
            out_ += "  mov edi, 1\n";
            out_ += "  mov rsi, rsp\n";
            out_ += "  mov edx, 1\n";
            out_ += "  syscall\n";
            out_ += "  pop rax\n";
            out_ += "  ret\n";
            return true;
        }
        if (name == "cider_itoa_signed" || name == "cider_itoa_unsigned") {
            // Decimal into the caller's buffer: rdi answers the bytes, rsi
            // the value, rax the count. The signed form prints a leading
            // minus; both work the digits from the magnitude as an unsigned
            // quantity, so INT64_MIN prints instead of trapping — the same
            // rule printInt follows, because the language refuses to hide
            // precision behind one character. Twenty bytes hold any 64-bit
            // decimal; the caller owns the buffer.
            bool isSigned = name == "cider_itoa_signed";
            const std::string tag = isSigned ? "s" : "u";
            out_ += name + ":\n";
            out_ += "  push rbx\n";
            out_ += "  sub rsp, 24\n";
            out_ += "  mov rax, rsi\n";
            out_ += "  xor ecx, ecx\n";
            if (isSigned) {
                out_ += "  test rax, rax\n";
                out_ += "  jns .itoa_" + tag + "_digits\n";
                out_ += "  mov byte ptr [rdi], 45\n";
                out_ += "  inc rdi\n";
                out_ += "  inc rcx\n";
                out_ += "  neg rax\n";
            }
            out_ += ".itoa_" + tag + "_digits:\n";
            out_ += "  xor edx, edx\n";
            out_ += "  mov rbx, 10\n";
            out_ += "  div rbx\n";
            out_ += "  add dl, 48\n";
            out_ += "  mov [rsp + rcx], dl\n";
            out_ += "  inc rcx\n";
            out_ += "  test rax, rax\n";
            out_ += "  jnz .itoa_" + tag + "_digits\n";
            out_ += "  mov rax, rcx\n";
            out_ += ".itoa_" + tag + "_copy:\n";
            out_ += "  dec rcx\n";
            out_ += "  mov bl, [rsp + rcx]\n";
            out_ += "  mov [rdi], bl\n";
            out_ += "  inc rdi\n";
            out_ += "  test rcx, rcx\n";
            out_ += "  jnz .itoa_" + tag + "_copy\n";
            out_ += "  add rsp, 24\n";
            out_ += "  pop rbx\n";
            out_ += "  ret\n";
            return true;
        }
        if (name == "cider_ftoa") {
            // A double into the caller's buffer: rdi answers the bytes,
            // xmm0 the value, rax the count. The shape is printF64's
            // (std.io), rebuilt in assembly so interpolation needs no
            // module linked: a sign, the whole digits through
            // cider_itoa_signed, a point, then six fraction digits,
            // truncated never rounded, zero-padded on the left. Every
            // operation matches the std.io sequence — the same compare
            // with its parity guard, the same truncating converts — so
            // edge values (including NaN and infinities) print exactly
            // what printF64 prints. Thirty-two bytes hold the longest
            // shape: a sign, nineteen whole digits, the point, six
            // fraction digits; the caller owns the buffer.
            out_ += "cider_ftoa:\n";
            out_ += "  push rbx\n";
            out_ += "  sub rsp, 48\n";
            out_ += "  movsd [rsp + 16], xmm0\n";
            out_ += "  mov qword ptr [rsp + 24], 0\n";
            out_ += "  xorpd xmm1, xmm1\n";
            out_ += "  ucomisd xmm0, xmm1\n";
            out_ += "  setb al\n";
            out_ += "  setnp cl\n";
            out_ += "  and al, cl\n";
            out_ += "  test al, al\n";
            out_ += "  jz .ftoa_whole\n";
            out_ += "  mov byte ptr [rdi], 45\n";
            out_ += "  inc rdi\n";
            out_ += "  mov qword ptr [rsp + 24], 1\n";
            out_ += "  movsd xmm0, [rsp + 16]\n";
            out_ += "  subsd xmm1, xmm0\n";
            out_ += "  movsd xmm0, xmm1\n";
            out_ += "  movsd [rsp + 16], xmm0\n";
            out_ += ".ftoa_whole:\n";
            out_ += "  cvttsd2si rax, xmm0\n";
            out_ += "  mov [rsp], rax\n";
            out_ += "  mov rsi, rax\n";
            out_ += "  call cider_itoa_signed\n";
            // The helper leaves rdi past the digits it wrote, so only
            // the count still needs adding to the running total.
            out_ += "  add [rsp + 24], rax\n";
            out_ += "  mov byte ptr [rdi], 46\n";
            out_ += "  inc rdi\n";
            out_ += "  inc qword ptr [rsp + 24]\n";
            out_ += "  cvtsi2sd xmm1, qword ptr [rsp]\n";
            out_ += "  movsd xmm0, [rsp + 16]\n";
            out_ += "  subsd xmm0, xmm1\n";
            out_ += "  mov rax, 0x412E848000000000\n";
            out_ += "  mov [rsp + 40], rax\n";
            out_ += "  mulsd xmm0, [rsp + 40]\n";
            out_ += "  cvttsd2si rax, xmm0\n";
            out_ += "  mov [rsp + 8], rax\n";
            out_ += "  mov rax, [rsp + 8]\n";
            out_ += "  mov rbx, 10\n";
            out_ += "  mov rcx, 6\n";
            out_ += ".ftoa_frac:\n";
            out_ += "  cqo\n";
            out_ += "  idiv rbx\n";
            out_ += "  add dl, 48\n";
            out_ += "  mov [rsp + rcx + 31], dl\n";
            out_ += "  dec rcx\n";
            out_ += "  jnz .ftoa_frac\n";
            out_ += "  mov eax, [rsp + 32]\n";
            out_ += "  mov [rdi], eax\n";
            out_ += "  mov ax, [rsp + 36]\n";
            out_ += "  mov [rdi + 4], ax\n";
            out_ += "  add rdi, 6\n";
            out_ += "  add qword ptr [rsp + 24], 6\n";
            out_ += "  mov rax, [rsp + 24]\n";
            out_ += "  add rsp, 48\n";
            out_ += "  pop rbx\n";
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
        out_ += "  add qword ptr [rip + cider_heap_total], rsi\n";
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
        uint32_t span = (cells * 8 + 15) & ~15u;

        out_ += ".globl " + fn.name + "\n";
        out_ += fn.name + ":\n";
        out_ += "  push rbp\n";
        out_ += "  mov rbp, rsp\n";
        out_ += "  sub rsp, " + std::to_string(span) + "\n";

        // Parameters arrive in registers (then stack); they live in slots
        // from here on, like every other value. A float parameter comes in
        // on its SSE register — the class lowering recorded per word — so
        // the two counters mirror the caller's split exactly; an integer
        // word past the sixth would arrive on the stack, which the caller
        // refuses to send, so that branch stays a defensive reading.
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
            if (gpr < 6) {
                out_ += "  mov " + slot(fn.params[w].id) + ", " + argReg(gpr) + "\n";
            } else {
                uint64_t stackAt = 16 + (gpr - 6) * 8;
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
                    out_ += "  fnstsw ax\n";
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
            const uint64_t kIntRegs = 6;
            const uint64_t kSseRegs = 8;
            if (gpr > kIntRegs) {
                refuse(instr.loc, "more than six registers of arguments");
                return false;
            }
            if (sse > kSseRegs) {
                refuse(instr.loc, "more than eight float arguments");
                return false;
            }
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
            out_ += "  ud2\n";
            return true;
        }
        if (instr.op == ir::Op::DataAddr) {
            out_ += "  lea rax, [rip + " + instr.globalLabel + "]\n";
            out_ += "  mov " + slot(instr.results[0].id) + ", rax\n";
            addrWords_[instr.results[0].id] = instr.globalLabel;
            return true;
        }
        if (instr.op == ir::Op::FnAddr) {
            out_ += "  lea rax, [rip + " + instr.globalLabel + "]\n";
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
    // Result id of a DataAddr word -> its label, so an asm placeholder
    // paired with that word prints `[rip + label]` (the global's value)
    // instead of the temporary holding the address.
    std::unordered_map<uint32_t, std::string> addrWords_;
    std::unordered_set<std::string> emittedHelpers_;
    std::string out_;
    // Unique suffix for u64-to-float conversion labels (int→float casts
    // need a two-path sequence; see emitCast).
    uint64_t cvtLabelSeq_ = 0;
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
