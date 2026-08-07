#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "vm.h"

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

std::unique_ptr<cider::FunctionIR> makeFn(const std::string& name, cider::IRType retType) {
    return std::make_unique<cider::FunctionIR>(name, retType);
}

cider::IRInstruction opInstr(cider::IROp op, const std::string& dest,
                             std::vector<cider::IRValue> args) {
    cider::IRInstruction in;
    in.op = op;
    if (!dest.empty()) in.dest = cider::IRValue::Register(dest, cider::IRType::I32);
    in.args = std::move(args);
    return in;
}

int run() {
    // func main() -> int { return 6 * 7; }
    auto fn = makeFn("main", cider::IRType::I32);
    auto block = std::make_unique<cider::BasicBlock>("entry");
    block->addInstruction(opInstr(cider::IROp::MUL, "0",
        {cider::IRValue::Constant(6, cider::IRType::I32), cider::IRValue::Constant(7, cider::IRType::I32)}));
    cider::IRInstruction ret;
    ret.op = cider::IROp::RET;
    ret.args = {cider::IRValue::Register("0", cider::IRType::I32)};
    block->addInstruction(ret);
    fn->addBlock(std::move(block));
    cider::ModuleIR mod;
    mod.addFunction(std::move(fn));
    cider::VM vm;
    auto r = vm.run(mod);
    CHECK(r.exitCode == 42);

    // Branches: if (1) go A else go B; A returns 3, B returns 4.
    auto fn2 = makeFn("main2", cider::IRType::I32);
    auto e = std::make_unique<cider::BasicBlock>("entry");
    e->addInstruction(opInstr(cider::IROp::COND_BR, "",
        {cider::IRValue::Constant(1, cider::IRType::I32),
         cider::IRValue::Label("A"), cider::IRValue::Label("B")}));
    auto a = std::make_unique<cider::BasicBlock>("A");
    cider::IRInstruction retA;
    retA.op = cider::IROp::RET;
    retA.args = {cider::IRValue::Constant(3, cider::IRType::I32)};
    a->addInstruction(retA);
    auto b = std::make_unique<cider::BasicBlock>("B");
    cider::IRInstruction retB;
    retB.op = cider::IROp::RET;
    retB.args = {cider::IRValue::Constant(4, cider::IRType::I32)};
    b->addInstruction(retB);
    fn2->addBlock(std::move(e));
    fn2->addBlock(std::move(a));
    fn2->addBlock(std::move(b));
    cider::ModuleIR mod2;
    mod2.addFunction(std::move(fn2));
    cider::VM vm2;
    CHECK(vm2.run(mod2, "main2").exitCode == 3);

    // PHI: diamond selecting 1 from then-block, 2 from else-block.
    auto fn3 = makeFn("main3", cider::IRType::I32);
    auto e3 = std::make_unique<cider::BasicBlock>("entry");
    e3->addInstruction(opInstr(cider::IROp::COND_BR, "",
        {cider::IRValue::Constant(0, cider::IRType::I32),
         cider::IRValue::Label("T"), cider::IRValue::Label("F")}));
    auto t3 = std::make_unique<cider::BasicBlock>("T");
    t3->addInstruction(opInstr(cider::IROp::BR, "",
        {cider::IRValue::Label("M")}));
    auto f3 = std::make_unique<cider::BasicBlock>("F");
    f3->addInstruction(opInstr(cider::IROp::BR, "",
        {cider::IRValue::Label("M")}));
    auto m3 = std::make_unique<cider::BasicBlock>("M");
    cider::IRInstruction phi;
    phi.op = cider::IROp::PHI;
    phi.dest = cider::IRValue::Register("p", cider::IRType::I32);
    phi.phiArgs["T"] = cider::IRValue::Constant(1, cider::IRType::I32);
    phi.phiArgs["F"] = cider::IRValue::Constant(2, cider::IRType::I32);
    m3->addInstruction(phi);
    cider::IRInstruction retP;
    retP.op = cider::IROp::RET;
    retP.args = {cider::IRValue::Register("p", cider::IRType::I32)};
    m3->addInstruction(retP);
    fn3->addBlock(std::move(e3));
    fn3->addBlock(std::move(t3));
    fn3->addBlock(std::move(f3));
    fn3->addBlock(std::move(m3));
    cider::ModuleIR mod3;
    mod3.addFunction(std::move(fn3));
    cider::VM vm3;
    CHECK(vm3.run(mod3, "main3").exitCode == 2);

    // Calls: main calls helper(21) which returns x*2.
    auto helper = makeFn("helper", cider::IRType::I32);
    helper->addParam("arg_x");
    auto hb = std::make_unique<cider::BasicBlock>("entry");
    cider::IRInstruction mul;
    mul.op = cider::IROp::MUL;
    mul.dest = cider::IRValue::Register("r", cider::IRType::I32);
    mul.args = {cider::IRValue::Register("arg_x", cider::IRType::I32),
                cider::IRValue::Constant(2, cider::IRType::I32)};
    hb->addInstruction(mul);
    cider::IRInstruction hret;
    hret.op = cider::IROp::RET;
    hret.args = {cider::IRValue::Register("r", cider::IRType::I32)};
    hb->addInstruction(hret);
    helper->addBlock(std::move(hb));

    auto caller = makeFn("main", cider::IRType::I32);
    auto cb = std::make_unique<cider::BasicBlock>("entry");
    cider::IRInstruction call;
    call.op = cider::IROp::CALL;
    call.dest = cider::IRValue::Register("res", cider::IRType::I32);
    call.args = {cider::IRValue::Label("helper"), cider::IRValue::Constant(21, cider::IRType::I32)};
    cb->addInstruction(call);
    cider::IRInstruction cret;
    cret.op = cider::IROp::RET;
    cret.args = {cider::IRValue::Register("res", cider::IRType::I32)};
    cb->addInstruction(cret);
    caller->addBlock(std::move(cb));

    cider::ModuleIR mod4;
    mod4.addFunction(std::move(helper));
    mod4.addFunction(std::move(caller));
    cider::VM vm4;
    CHECK(vm4.run(mod4).exitCode == 42);

    // Memory: alloca + store + load round trip.
    auto fn5 = makeFn("main", cider::IRType::I32);
    auto b5 = std::make_unique<cider::BasicBlock>("entry");
    b5->addInstruction(opInstr(cider::IROp::ALLOCA, "p", {cider::IRValue::Constant(8, cider::IRType::I32)}));
    cider::IRInstruction st;
    st.op = cider::IROp::STORE;
    st.args = {cider::IRValue::Register("p", cider::IRType::PTR),
               cider::IRValue::Constant(99, cider::IRType::I32),
               cider::IRValue::Constant(0, cider::IRType::I32)};
    b5->addInstruction(st);
    cider::IRInstruction ld;
    ld.op = cider::IROp::LOAD;
    ld.dest = cider::IRValue::Register("v", cider::IRType::I32);
    ld.args = {cider::IRValue::Register("p", cider::IRType::PTR),
               cider::IRValue::Constant(0, cider::IRType::I32)};
    b5->addInstruction(ld);
    cider::IRInstruction r5;
    r5.op = cider::IROp::RET;
    r5.args = {cider::IRValue::Register("v", cider::IRType::I32)};
    b5->addInstruction(r5);
    fn5->addBlock(std::move(b5));
    cider::ModuleIR mod5;
    mod5.addFunction(std::move(fn5));
    cider::VM vm5;
    CHECK(vm5.run(mod5).exitCode == 99);

    // Step cap: a 10M+ instruction straight-line program fails fast.
    auto fn6 = makeFn("main", cider::IRType::VOID);
    auto b6 = std::make_unique<cider::BasicBlock>("entry");
    for (int i = 0; i < 10000001; ++i) {
        b6->addInstruction(opInstr(cider::IROp::MOV, "t", {cider::IRValue::Constant(0, cider::IRType::I32)}));
    }
    fn6->addBlock(std::move(b6));
    cider::ModuleIR mod6;
    mod6.addFunction(std::move(fn6));
    cider::VM vm6;
    bool threw = false;
    try {
        vm6.run(mod6);
    } catch (const cider::VMError& e) {
        threw = true;
        CHECK(std::string(e.message).find("step limit") != std::string::npos);
    }
    CHECK(threw);
    return 0;
}

int main() { return run(); }
