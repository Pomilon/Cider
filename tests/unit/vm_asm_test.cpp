#include <cstdio>
#include <memory>
#include <string>
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
    // Hello via the write syscall whitelist.
    auto fn = std::make_unique<cider::FunctionIR>("main", cider::IRType::VOID);
    auto block = std::make_unique<cider::BasicBlock>("entry");
    block->addInstruction(asmInstr("mov rax, 1"));
    block->addInstruction(asmInstr("mov rdi, 1"));
    auto w = asmInstr("mov rsi, {msg}");
    w.asmMap["msg"] = stringConst("Hello, world!\n");
    block->addInstruction(w);
    block->addInstruction(asmInstr("mov rdx, 14"));
    block->addInstruction(asmInstr("syscall"));
    block->addInstruction(asmInstr("mov rax, 60"));
    block->addInstruction(asmInstr("mov rdi, 0"));
    block->addInstruction(asmInstr("syscall"));
    fn->addBlock(std::move(block));
    cider::ModuleIR mod;
    mod.addFunction(std::move(fn));
    cider::VM vm;
    auto r = vm.run(mod);
    CHECK(r.stdoutText == "Hello, world!\n");
    CHECK(r.exitCode == 0);
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

    // stderr write.
    auto fn3 = std::make_unique<cider::FunctionIR>("main", cider::IRType::VOID);
    auto b3 = std::make_unique<cider::BasicBlock>("entry");
    b3->addInstruction(asmInstr("mov rax, 1"));
    b3->addInstruction(asmInstr("mov rdi, 2"));
    auto w3 = asmInstr("mov rsi, {msg}");
    w3.asmMap["msg"] = stringConst("oops\n");
    b3->addInstruction(w3);
    b3->addInstruction(asmInstr("mov rdx, 5"));
    b3->addInstruction(asmInstr("syscall"));
    b3->addInstruction(asmInstr("mov rax, 60"));
    b3->addInstruction(asmInstr("mov rdi, 3"));
    b3->addInstruction(asmInstr("syscall"));
    fn3->addBlock(std::move(b3));
    cider::ModuleIR mod3;
    mod3.addFunction(std::move(fn3));
    cider::VM vm3;
    auto r3 = vm3.run(mod3);
    CHECK(r3.stderrText == "oops\n");
    CHECK(r3.exitCode == 3);

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
    return 0;
}

int main() { return run(); }
