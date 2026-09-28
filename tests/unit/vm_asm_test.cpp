#include <cstdio>
#include <memory>
#include <new>
#include <string>
#include <unistd.h>
#include <vector>

#include "vm.h"

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

cider::IRInstruction asmInstr(const std::string& code) {
    cider::IRInstruction in;
    in.op = cider::IROp::ASM;
    in.args = {cider::IRValue::Label(code)};
    return in;
}

cider::IRValue stringConst(const std::string& s) {
    cider::IRValue v;
    v.kind = cider::IRValue::CONSTANT;
    v.name = "\"" + s + "\"";
    v.type = cider::IRType::PTR;
    return v;
}

int run() {
    // Hello via the write syscall whitelist. The write returns the byte
    // count in rax (CIDER-BUG-076); move it to rdi so the exit code proves
    // it is 14, not the syscall number or the fd.
    auto fn = std::make_unique<cider::FunctionIR>("main", cider::IRType::VOID);
    auto block = std::make_unique<cider::BasicBlock>("entry");
    block->addInstruction(asmInstr("mov rax, 1"));
    block->addInstruction(asmInstr("mov rdi, 1"));
    auto w = asmInstr("mov rsi, {msg}");
    w.asmMap["msg"] = stringConst("Hello, world!\n");
    block->addInstruction(w);
    block->addInstruction(asmInstr("mov rdx, 14"));
    block->addInstruction(asmInstr("syscall"));
    block->addInstruction(asmInstr("mov rdi, rax"));
    block->addInstruction(asmInstr("mov rax, 60"));
    block->addInstruction(asmInstr("syscall"));
    fn->addBlock(std::move(block));
    cider::ModuleIR mod;
    mod.addFunction(std::move(fn));
    cider::VM vm;
    auto r = vm.run(mod);
    CHECK(r.stdoutText == "Hello, world!\n");
    CHECK(r.exitCode == 14);
    CHECK(r.stderrText.empty());

    // Exit code variant.
    auto fn2 = std::make_unique<cider::FunctionIR>("main", cider::IRType::VOID);
    auto b2 = std::make_unique<cider::BasicBlock>("entry");
    b2->addInstruction(asmInstr("mov rax, 60"));
    b2->addInstruction(asmInstr("mov rdi, 7"));
    b2->addInstruction(asmInstr("syscall"));
    fn2->addBlock(std::move(b2));
    cider::ModuleIR mod2;
    mod2.addFunction(std::move(fn2));
    cider::VM vm2;
    CHECK(vm2.run(mod2).exitCode == 7);

    // stderr write: its byte count comes back in rax too.
    auto fn3 = std::make_unique<cider::FunctionIR>("main", cider::IRType::VOID);
    auto b3 = std::make_unique<cider::BasicBlock>("entry");
    b3->addInstruction(asmInstr("mov rax, 1"));
    b3->addInstruction(asmInstr("mov rdi, 2"));
    auto w3 = asmInstr("mov rsi, {msg}");
    w3.asmMap["msg"] = stringConst("oops\n");
    b3->addInstruction(w3);
    b3->addInstruction(asmInstr("mov rdx, 5"));
    b3->addInstruction(asmInstr("syscall"));
    b3->addInstruction(asmInstr("mov rdi, rax"));
    b3->addInstruction(asmInstr("mov rax, 60"));
    b3->addInstruction(asmInstr("syscall"));
    fn3->addBlock(std::move(b3));
    cider::ModuleIR mod3;
    mod3.addFunction(std::move(fn3));
    cider::VM vm3;
    auto r3 = vm3.run(mod3);
    CHECK(r3.stderrText == "oops\n");
    CHECK(r3.exitCode == 5);

    // Unsupported asm -> VMError.
    auto fn4 = std::make_unique<cider::FunctionIR>("main", cider::IRType::VOID);
    auto b4 = std::make_unique<cider::BasicBlock>("entry");
    b4->addInstruction(asmInstr("mov rax, 999"));
    b4->addInstruction(asmInstr("syscall"));
    fn4->addBlock(std::move(b4));
    cider::ModuleIR mod4;
    mod4.addFunction(std::move(fn4));
    cider::VM vm4;
    bool threw = false;
    try {
        vm4.run(mod4);
    } catch (const cider::VMError& e) {
        threw = true;
        CHECK(std::string(e.message).find("unsupported asm") != std::string::npos);
    }
    CHECK(threw);

    // CIDER-BUG-099: write/read must validate the buffer as a signed heap
    // range. A negative offset used to index the host heap out of bounds
    // and an out-of-heap read used to report EOF (0) instead of the
    // -EFAULT native returns for a bad user pointer. Each check exits with
    // the syscall result so a wrong rax shows up as the exit code.
    auto runAsm = [](const std::string& code) {
        auto fn = std::make_unique<cider::FunctionIR>("main", cider::IRType::VOID);
        auto block = std::make_unique<cider::BasicBlock>("entry");
        block->addInstruction(asmInstr(code));
        fn->addBlock(std::move(block));
        cider::ModuleIR mod;
        mod.addFunction(std::move(fn));
        cider::VM vm;
        return vm.run(mod);
    };
    // A live file fd makes the read/write paths take their real-fd branch,
    // where the pre-fix check called ::write/::read with an out-of-heap
    // host pointer (the buffered fd 1/2 path read host memory instead).
    std::FILE* tf = std::tmpfile();
    CHECK(tf != nullptr);
    CHECK(std::fwrite("ABCD", 1, 4, tf) == 4);
    std::fflush(tf);
    int tfd = fileno(tf);
    // Raw fd positioning only: the VM reads the fd directly, and stdio's
    // lazy seek tracking would otherwise disagree with the real offset.
    CHECK(::lseek(tfd, 0, SEEK_SET) == 0);
    const std::string fdArg = std::to_string(tfd);

    // write(1, (void*)-16, 4) -> -EFAULT, nothing buffered.
    {
        auto r = runAsm("mov rax, 1\nmov rdi, 1\nmov rsi, -16\nmov rdx, 4\n"
                        "syscall\nmov rdi, rax\nmov rax, 60\nsyscall\n");
        CHECK(r.stdoutText.empty());
        CHECK(r.stderrText.empty());
        CHECK(r.exitCode == -14);
    }

    // write(<file>, (void*)-16, 4) -> -EFAULT, no bytes reach the file.
    {
        auto r = runAsm("mov rax, 1\nmov rdi, " + fdArg + "\nmov rsi, -16\nmov rdx, 4\n"
                        "syscall\nmov rdi, rax\nmov rax, 60\nsyscall\n");
        CHECK(r.exitCode == -14);
        CHECK(::lseek(tfd, 0, SEEK_END) == 4);  // the bad write left 4 bytes
        CHECK(::lseek(tfd, 0, SEEK_SET) == 0);
    }

    // read(<file>, (void*)-16, 4) -> -EFAULT (negative offset).
    {
        auto r = runAsm("mov rax, 0\nmov rdi, " + fdArg + "\nmov rsi, -16\nmov rdx, 4\n"
                        "syscall\nmov rdi, rax\nmov rax, 60\nsyscall\n");
        CHECK(r.exitCode == -14);
    }

    // read(<file>, heap+2^40, 4) -> -EFAULT, not the old EOF (0).
    {
        auto r = runAsm("mov rax, 0\nmov rdi, " + fdArg + "\nmov rsi, 1099511627776\nmov rdx, 4\n"
                        "syscall\nmov rdi, rax\nmov rax, 60\nsyscall\n");
        CHECK(r.exitCode == -14);
    }

    // read(<file>, buf, -4) -> -EFAULT (negative length).
    {
        auto r = runAsm("mov rax, 0\nmov rdi, " + fdArg + "\nmov rsi, 0\nmov rdx, -4\n"
                        "syscall\nmov rdi, rax\nmov rax, 60\nsyscall\n");
        CHECK(r.exitCode == -14);
    }

    // A valid in-heap buffer extended past the heap -> -EFAULT; exact
    // in-heap ranges stay usable (the good path is unchanged).
    {
        auto fn = std::make_unique<cider::FunctionIR>("main", cider::IRType::VOID);
        auto block = std::make_unique<cider::BasicBlock>("entry");
        auto readIn = asmInstr("mov rax, 0\nmov rdi, " + fdArg + "\nmov rsi, {buf}\nmov rdx, 4\n"
                               "syscall\nmov rdi, rax\nmov rax, 60\nsyscall\n");
        readIn.asmMap["buf"] = stringConst("efault-buffer");
        block->addInstruction(readIn);
        fn->addBlock(std::move(block));
        cider::ModuleIR mod;
        mod.addFunction(std::move(fn));
        cider::VM vm;
        CHECK(vm.run(mod).exitCode == 4);  // file still at offset 0
    }
    {
        auto fn = std::make_unique<cider::FunctionIR>("main", cider::IRType::VOID);
        auto block = std::make_unique<cider::BasicBlock>("entry");
        auto overrun = asmInstr("mov rax, 0\nmov rdi, " + fdArg + "\nmov rsi, {buf}\n"
                                "mov rdx, 1099511627776\n"
                                "syscall\nmov rdi, rax\nmov rax, 60\nsyscall\n");
        overrun.asmMap["buf"] = stringConst("efault-buffer");
        block->addInstruction(overrun);
        fn->addBlock(std::move(block));
        cider::ModuleIR mod;
        mod.addFunction(std::move(fn));
        cider::VM vm;
        CHECK(vm.run(mod).exitCode == -14);
    }

    // CIDER-BUG-103: mmap(len=rsi) must validate the length. VM::malloc
    // used to clamp any size < 1 to one byte, so a negative request used to
    // "succeed" as a tiny block, and a near-INT64_MAX request overflowed
    // its size + kHeaderSize arithmetic; a merely huge one threw
    // std::bad_alloc out of the VM instead of reporting failure. Native
    // mmap takes the length as size_t, so only 0 is EINVAL: a negative i64
    // is a huge size_t that reports ENOMEM (CIDER-BUG-114), as does any
    // mapping that cannot be established; the syscall result rides rax into
    // the exit code.
    {
        // (size_t)-1: native fails with ENOMEM, not EINVAL (CIDER-BUG-114).
        auto r = runAsm("mov rax, 9\nmov rsi, -1\n"
                        "syscall\nmov rdi, rax\nmov rax, 60\nsyscall\n");
        CHECK(r.exitCode == -12);  // -ENOMEM
    }
    {
        // INT64_MIN is the other end of the negative range; still ENOMEM.
        auto r = runAsm("mov rax, 9\nmov rsi, -9223372036854775808\n"
                        "syscall\nmov rdi, rax\nmov rax, 60\nsyscall\n");
        CHECK(r.exitCode == -12);  // -ENOMEM
    }
    {
        // Native mmap(NULL, 0, ...) is EINVAL (verified against Linux).
        auto r = runAsm("mov rax, 9\nmov rsi, 0\n"
                        "syscall\nmov rdi, rax\nmov rax, 60\nsyscall\n");
        CHECK(r.exitCode == -22);  // -EINVAL
    }
    {
        // 2^62 exceeds every Linux user address space -> -ENOMEM.
        auto r = runAsm("mov rax, 9\nmov rsi, 4611686018427387904\n"
                        "syscall\nmov rdi, rax\nmov rax, 60\nsyscall\n");
        CHECK(r.exitCode == -12);  // -ENOMEM
    }
    {
        // INT64_MAX + kHeaderSize must not wrap: -ENOMEM, not a tiny block.
        auto r = runAsm("mov rax, 9\nmov rsi, 9223372036854775807\n"
                        "syscall\nmov rdi, rax\nmov rax, 60\nsyscall\n");
        CHECK(r.exitCode == -12);  // -ENOMEM
    }

    // CIDER-BUG-103: offset 0 is the nil sentinel. VM::malloc returns
    // offsets from kHeaderSize on, and offset 0 is the first block's size
    // header (a read there used to corrupt it; the later frame cleanup
    // reported "free: corrupt block"). A non-empty buffer at 0 must EFAULT
    // like native; NULL with count 0 stays the accepted no-op.
    {
        auto r = runAsm("mov rax, 1\nmov rdi, 1\nmov rsi, 0\nmov rdx, 4\n"
                        "syscall\nmov rdi, rax\nmov rax, 60\nsyscall\n");
        CHECK(r.stdoutText.empty());
        CHECK(r.exitCode == -14);  // -EFAULT
    }
    CHECK(::lseek(tfd, 0, SEEK_SET) == 0);
    {
        auto r = runAsm("mov rax, 0\nmov rdi, " + fdArg + "\nmov rsi, 0\nmov rdx, 4\n"
                        "syscall\nmov rdi, rax\nmov rax, 60\nsyscall\n");
        CHECK(r.exitCode == -14);               // -EFAULT
        CHECK(::lseek(tfd, 0, SEEK_CUR) == 0);  // nothing consumed
    }
    {
        auto r = runAsm("mov rax, 1\nmov rdi, 1\nmov rsi, 0\nmov rdx, 0\n"
                        "syscall\nmov rdi, rax\nmov rax, 60\nsyscall\n");
        CHECK(r.stdoutText.empty());
        CHECK(r.exitCode == 0);  // native accepts NULL with count 0
    }
    {
        auto r = runAsm("mov rax, 0\nmov rdi, " + fdArg + "\nmov rsi, 0\nmov rdx, 0\n"
                        "syscall\nmov rdi, rax\nmov rax, 60\nsyscall\n");
        CHECK(r.exitCode == 0);
        CHECK(::lseek(tfd, 0, SEEK_CUR) == 0);
    }

    // CIDER-BUG-110: count 0 performs no access and faults on nothing, so
    // write/read accept ANY buffer pointer and report 0 bytes (the pre-fix
    // inHeap check rejected a bogus pointer with -EFAULT even though no
    // byte is transferred). Non-zero lengths keep the CIDER-BUG-099/103
    // validation exercised above.
    {
        // write(1, (void*)-16, 0) -> 0, nothing buffered.
        auto r = runAsm("mov rax, 1\nmov rdi, 1\nmov rsi, -16\nmov rdx, 0\n"
                        "syscall\nmov rdi, rax\nmov rax, 60\nsyscall\n");
        CHECK(r.stdoutText.empty());
        CHECK(r.stderrText.empty());
        CHECK(r.exitCode == 0);
    }
    {
        // write(2, (void*)-16, 0) -> 0, nothing buffered.
        auto r = runAsm("mov rax, 1\nmov rdi, 2\nmov rsi, -16\nmov rdx, 0\n"
                        "syscall\nmov rdi, rax\nmov rax, 60\nsyscall\n");
        CHECK(r.stderrText.empty());
        CHECK(r.exitCode == 0);
    }
    {
        // write(<file>, (void*)-16, 0) -> 0 bytes written, file unchanged.
        auto r = runAsm("mov rax, 1\nmov rdi, " + fdArg + "\nmov rsi, -16\nmov rdx, 0\n"
                        "syscall\nmov rdi, rax\nmov rax, 60\nsyscall\n");
        CHECK(r.exitCode == 0);
        CHECK(::lseek(tfd, 0, SEEK_END) == 4);
        CHECK(::lseek(tfd, 0, SEEK_SET) == 0);
    }
    {
        // read(<file>, (void*)-16, 0) -> 0, nothing consumed.
        auto r = runAsm("mov rax, 0\nmov rdi, " + fdArg + "\nmov rsi, -16\nmov rdx, 0\n"
                        "syscall\nmov rdi, rax\nmov rax, 60\nsyscall\n");
        CHECK(r.exitCode == 0);
        CHECK(::lseek(tfd, 0, SEEK_CUR) == 0);
    }
    {
        // A huge out-of-heap offset with count 0 is accepted too.
        auto r = runAsm("mov rax, 0\nmov rdi, " + fdArg + "\nmov rsi, 1099511627776\nmov rdx, 0\n"
                        "syscall\nmov rdi, rax\nmov rax, 60\nsyscall\n");
        CHECK(r.exitCode == 0);
        CHECK(::lseek(tfd, 0, SEEK_CUR) == 0);
    }

    // CIDER-BUG-110: a NEGATIVE allocation size must not silently allocate.
    // ALLOCA's unsigned bump arithmetic used to wrap a negative size into a
    // bogus slot and VM::malloc clamped it to one byte; the allocator now
    // fails the request like any other impossible allocation (the same
    // std::bad_alloc the mmap path maps to -ENOMEM). A zero-size ALLOCA
    // keeps working, mirroring a native malloc(0) block.
    auto runAlloca = [](int64_t size) {
        auto fn = std::make_unique<cider::FunctionIR>("main", cider::IRType::VOID);
        auto block = std::make_unique<cider::BasicBlock>("entry");
        cider::IRInstruction al;
        al.op = cider::IROp::ALLOCA;
        al.dest = cider::IRValue::Register("p", cider::IRType::PTR);
        al.args = {cider::IRValue::Constant(size, cider::IRType::I64)};
        block->addInstruction(al);
        fn->addBlock(std::move(block));
        cider::ModuleIR mod;
        mod.addFunction(std::move(fn));
        cider::VM vm;
        return vm.run(mod);
    };
    {
        bool zeroOk = true;
        try {
            (void)runAlloca(0);
        } catch (...) {
            zeroOk = false;
        }
        CHECK(zeroOk);
    }
    {
        bool threw = false;
        try {
            (void)runAlloca(-8);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        CHECK(threw);
    }
    {
        // The builtin allocator rejects a negative size the same way.
        auto fn = std::make_unique<cider::FunctionIR>("main", cider::IRType::PTR);
        auto block = std::make_unique<cider::BasicBlock>("entry");
        cider::IRInstruction call;
        call.op = cider::IROp::CALL;
        call.dest = cider::IRValue::Register("p", cider::IRType::PTR);
        call.args = {cider::IRValue::Label("malloc"),
                     cider::IRValue::Constant(-1, cider::IRType::I64)};
        block->addInstruction(call);
        fn->addBlock(std::move(block));
        cider::ModuleIR mod;
        mod.addFunction(std::move(fn));
        cider::VM vm;
        bool threw = false;
        try {
            (void)vm.run(mod);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        CHECK(threw);
    }

    // A valid mmap keeps working: allocate 64 bytes, read the file into the
    // fresh region, and report the byte count. The non-zero exit path above
    // shows the validation did not reject usable lengths.
    {
        auto r = runAsm("mov rax, 9\nmov rsi, 64\nsyscall\n"
                        "mov r8, rax\n"  // keep the buffer offset
                        "mov rax, 0\n"   // read(<file>, buf, 4)
                        "mov rdi, " + fdArg + "\n"
                        "mov rsi, r8\n"
                        "mov rdx, 4\n"
                        "syscall\n"
                        "mov rdi, rax\n"
                        "mov rax, 60\n"
                        "syscall\n");
        CHECK(r.exitCode == 4);
    }
    std::fclose(tf);
    return 0;
}

int main() { return run(); }
