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

    // Step cap: an infinite loop fails fast instead of hanging.
    auto fn6 = makeFn("main", cider::IRType::VOID);
    auto b6 = std::make_unique<cider::BasicBlock>("entry");
    cider::IRInstruction loop;
    loop.op = cider::IROp::BR;
    loop.args = {cider::IRValue::Label("entry")};
    b6->addInstruction(loop);
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

    // CID-003 follow-up (W5): CVT_ITOF extends a narrow source by its
    // declared type before converting. An i8 AND leaves 156 (0x9C) in the
    // register; converting the raw bits unsigned gave 156.0 instead of
    // -100.0 (native movsx reads -100).
    {
        auto fn7 = makeFn("main7", cider::IRType::I32);
        auto b7 = std::make_unique<cider::BasicBlock>("entry");
        cider::IRInstruction raw;
        raw.op = cider::IROp::BIT_AND;
        raw.dest = cider::IRValue::Register("raw8", cider::IRType::I8);
        raw.args = {cider::IRValue::Constant(-100, cider::IRType::I32),
                    cider::IRValue::Constant(255, cider::IRType::I8)};
        b7->addInstruction(raw);
        cider::IRInstruction cvt;
        cvt.op = cider::IROp::CVT_ITOF;
        cvt.dest = cider::IRValue::Register("d", cider::IRType::F64);
        cvt.args = {cider::IRValue::Register("raw8", cider::IRType::I8)};
        b7->addInstruction(cvt);
        cider::IRInstruction cmp;
        cmp.op = cider::IROp::CMP_EQ;
        cmp.dest = cider::IRValue::Register("eq", cider::IRType::I32);
        cmp.args = {cider::IRValue::Register("d", cider::IRType::F64),
                    cider::IRValue::Constant(-100.0, cider::IRType::F64)};
        b7->addInstruction(cmp);
        cider::IRInstruction r7;
        r7.op = cider::IROp::RET;
        r7.args = {cider::IRValue::Register("eq", cider::IRType::I32)};
        b7->addInstruction(r7);
        fn7->addBlock(std::move(b7));
        cider::ModuleIR mod7;
        mod7.addFunction(std::move(fn7));
        cider::VM vm7;
        CHECK(vm7.run(mod7, "main7").exitCode == 1);
    }

    // CID-003 follow-up (W5): a U64 comparison is unsigned. ir_gen converts
    // i64/u64 pairs to U64 with a MOV before the CMP; the VM compared the
    // extended values signed, so i64(-1) < u64(1) was true.
    {
        auto fn8 = makeFn("main8", cider::IRType::I32);
        auto b8 = std::make_unique<cider::BasicBlock>("entry");
        cider::IRInstruction movNeg;
        movNeg.op = cider::IROp::MOV;
        movNeg.dest = cider::IRValue::Register("a", cider::IRType::U64);
        movNeg.args = {cider::IRValue::Constant(-1, cider::IRType::I64)};
        b8->addInstruction(movNeg);
        cider::IRInstruction movOne;
        movOne.op = cider::IROp::MOV;
        movOne.dest = cider::IRValue::Register("b", cider::IRType::U64);
        movOne.args = {cider::IRValue::Constant(1, cider::IRType::U64)};
        b8->addInstruction(movOne);
        cider::IRInstruction lt;
        lt.op = cider::IROp::CMP_LT;
        lt.dest = cider::IRValue::Register("lt", cider::IRType::I32);
        lt.args = {cider::IRValue::Register("a", cider::IRType::U64),
                   cider::IRValue::Register("b", cider::IRType::U64)};
        b8->addInstruction(lt);
        cider::IRInstruction r8;
        r8.op = cider::IROp::RET;
        r8.args = {cider::IRValue::Register("lt", cider::IRType::I32)};
        b8->addInstruction(r8);
        fn8->addBlock(std::move(b8));
        cider::ModuleIR mod8;
        mod8.addFunction(std::move(fn8));
        cider::VM vm8;
        CHECK(vm8.run(mod8, "main8").exitCode == 0);
    }

    // CID-006: store a module function label in a local and call through
    // the loaded value.
    {
        auto worker = makeFn("worker", cider::IRType::I32);
        auto we = std::make_unique<cider::BasicBlock>("entry");
        cider::IRInstruction wret;
        wret.op = cider::IROp::RET;
        wret.args = {cider::IRValue::Constant(7, cider::IRType::I32)};
        we->addInstruction(wret);
        worker->addBlock(std::move(we));

        auto fn = makeFn("main", cider::IRType::I32);
        auto e = std::make_unique<cider::BasicBlock>("entry");
        e->addInstruction(opInstr(cider::IROp::ALLOCA, "slot",
                                  {cider::IRValue::Constant(8, cider::IRType::I32)}));
        cider::IRInstruction st;
        st.op = cider::IROp::STORE;
        st.args = {cider::IRValue::Register("slot", cider::IRType::PTR),
                   cider::IRValue::Label("worker"),
                   cider::IRValue::Constant(0, cider::IRType::I32)};
        e->addInstruction(st);
        cider::IRInstruction ld;
        ld.op = cider::IROp::LOAD;
        ld.dest = cider::IRValue::Register("fp", cider::IRType::PTR);
        ld.args = {cider::IRValue::Register("slot", cider::IRType::PTR),
                   cider::IRValue::Constant(0, cider::IRType::I32)};
        e->addInstruction(ld);
        cider::IRInstruction call;
        call.op = cider::IROp::CALL;
        call.dest = cider::IRValue::Register("r", cider::IRType::I32);
        call.args = {cider::IRValue::Register("fp", cider::IRType::PTR)};
        e->addInstruction(call);
        cider::IRInstruction ret;
        ret.op = cider::IROp::RET;
        ret.args = {cider::IRValue::Register("r", cider::IRType::I32)};
        e->addInstruction(ret);
        fn->addBlock(std::move(e));

        cider::ModuleIR mod;
        mod.addFunction(std::move(worker));
        mod.addFunction(std::move(fn));
        cider::VM vm;
        CHECK(vm.run(mod).exitCode == 7);
    }

    // CID-006: extern function value (putchar) called indirectly.
    {
        auto fn = makeFn("main", cider::IRType::I32);
        auto e = std::make_unique<cider::BasicBlock>("entry");
        e->addInstruction(opInstr(cider::IROp::ALLOCA, "eslot",
                                  {cider::IRValue::Constant(8, cider::IRType::I32)}));
        cider::IRInstruction st;
        st.op = cider::IROp::STORE;
        st.args = {cider::IRValue::Register("eslot", cider::IRType::PTR),
                   cider::IRValue::Label("putchar"),
                   cider::IRValue::Constant(0, cider::IRType::I32)};
        e->addInstruction(st);
        cider::IRInstruction ld;
        ld.op = cider::IROp::LOAD;
        ld.dest = cider::IRValue::Register("efp", cider::IRType::PTR);
        ld.args = {cider::IRValue::Register("eslot", cider::IRType::PTR),
                   cider::IRValue::Constant(0, cider::IRType::I32)};
        e->addInstruction(ld);
        cider::IRInstruction call;
        call.op = cider::IROp::CALL;
        call.dest = cider::IRValue::Register("er", cider::IRType::I32);
        call.args = {cider::IRValue::Register("efp", cider::IRType::PTR),
                     cider::IRValue::Constant(65, cider::IRType::I32)};
        e->addInstruction(call);
        cider::IRInstruction ret;
        ret.op = cider::IROp::RET;
        ret.args = {cider::IRValue::Constant(0, cider::IRType::I32)};
        e->addInstruction(ret);
        fn->addBlock(std::move(e));

        cider::ModuleIR mod;
        mod.addExternFunction("putchar");
        mod.addFunction(std::move(fn));
        cider::VM vm;
        auto r = vm.run(mod);
        CHECK(r.exitCode == 0);
        CHECK(r.stdoutText == "A");
    }

    // CID-006: a global cell `<g>: dq <function>` materializes the id.
    {
        auto target = makeFn("worker2", cider::IRType::I32);
        auto we = std::make_unique<cider::BasicBlock>("entry");
        cider::IRInstruction wret;
        wret.op = cider::IROp::RET;
        wret.args = {cider::IRValue::Constant(9, cider::IRType::I32)};
        we->addInstruction(wret);
        target->addBlock(std::move(we));

        auto fn = makeFn("main", cider::IRType::I32);
        auto e = std::make_unique<cider::BasicBlock>("entry");
        cider::IRInstruction ld;
        ld.op = cider::IROp::LOAD;
        ld.dest = cider::IRValue::Register("gfp", cider::IRType::PTR);
        ld.args = {cider::IRValue::Label("g"), cider::IRValue::Constant(0, cider::IRType::I32)};
        e->addInstruction(ld);
        cider::IRInstruction call;
        call.op = cider::IROp::CALL;
        call.dest = cider::IRValue::Register("gr", cider::IRType::I32);
        call.args = {cider::IRValue::Register("gfp", cider::IRType::PTR)};
        e->addInstruction(call);
        cider::IRInstruction ret;
        ret.op = cider::IROp::RET;
        ret.args = {cider::IRValue::Register("gr", cider::IRType::I32)};
        e->addInstruction(ret);
        fn->addBlock(std::move(e));

        cider::ModuleIR mod;
        mod.addStringData("g: dq worker2");
        mod.addFunction(std::move(target));
        mod.addFunction(std::move(fn));
        cider::VM vm;
        CHECK(vm.run(mod).exitCode == 9);
    }

    // CID-006 carry-over: module function ids are offset so id 0 is nilPtr.
    // The module's FIRST function must not compare equal to nilPtr (the
    // `if (cb != nilPtr)` guard idiom), and calling an actual nil value must
    // not dispatch to functions[0].
    {
        auto first = makeFn("first", cider::IRType::I32);
        auto fe = std::make_unique<cider::BasicBlock>("entry");
        cider::IRInstruction fret;
        fret.op = cider::IROp::RET;
        fret.args = {cider::IRValue::Constant(11, cider::IRType::I32)};
        fe->addInstruction(fret);
        first->addBlock(std::move(fe));

        auto fn = makeFn("main", cider::IRType::I32);
        auto e = std::make_unique<cider::BasicBlock>("entry");
        e->addInstruction(opInstr(cider::IROp::ALLOCA, "nslot",
                                  {cider::IRValue::Constant(8, cider::IRType::I32)}));
        cider::IRInstruction st;
        st.op = cider::IROp::STORE;
        st.args = {cider::IRValue::Register("nslot", cider::IRType::PTR),
                   cider::IRValue::Label("first"),
                   cider::IRValue::Constant(0, cider::IRType::I32)};
        e->addInstruction(st);
        cider::IRInstruction ld;
        ld.op = cider::IROp::LOAD;
        ld.dest = cider::IRValue::Register("nfp", cider::IRType::PTR);
        ld.args = {cider::IRValue::Register("nslot", cider::IRType::PTR),
                   cider::IRValue::Constant(0, cider::IRType::I32)};
        e->addInstruction(ld);
        cider::IRInstruction cmp;
        cmp.op = cider::IROp::CMP_NE;
        cmp.dest = cider::IRValue::Register("ne", cider::IRType::I32);
        cmp.args = {cider::IRValue::Register("nfp", cider::IRType::PTR),
                    cider::IRValue::Constant(0, cider::IRType::PTR)};
        e->addInstruction(cmp);
        cider::IRInstruction ret;
        ret.op = cider::IROp::RET;
        ret.args = {cider::IRValue::Register("ne", cider::IRType::I32)};
        e->addInstruction(ret);
        fn->addBlock(std::move(e));

        cider::ModuleIR mod;
        mod.addFunction(std::move(first));
        mod.addFunction(std::move(fn));
        cider::VM vm;
        CHECK(vm.run(mod).exitCode == 1);
    }
    {
        auto first = makeFn("first", cider::IRType::I32);
        auto fe = std::make_unique<cider::BasicBlock>("entry");
        cider::IRInstruction fret;
        fret.op = cider::IROp::RET;
        fret.args = {cider::IRValue::Constant(11, cider::IRType::I32)};
        fe->addInstruction(fret);
        first->addBlock(std::move(fe));

        auto fn = makeFn("main", cider::IRType::I32);
        auto e = std::make_unique<cider::BasicBlock>("entry");
        cider::IRInstruction call;
        call.op = cider::IROp::CALL;
        call.dest = cider::IRValue::Register("x", cider::IRType::I32);
        call.args = {cider::IRValue::Constant(0, cider::IRType::PTR)};
        e->addInstruction(call);
        cider::IRInstruction ret;
        ret.op = cider::IROp::RET;
        ret.args = {cider::IRValue::Constant(11, cider::IRType::I32)};
        e->addInstruction(ret);
        fn->addBlock(std::move(e));

        cider::ModuleIR mod;
        mod.addFunction(std::move(first));
        mod.addFunction(std::move(fn));
        cider::VM vm;
        bool threw = false;
        try {
            vm.run(mod);
        } catch (const cider::VMError& e) {
            threw = true;
            CHECK(std::string(e.message).find("nil") != std::string::npos);
        }
        CHECK(threw);
    }

    // CID-006 F1: a linker-provided DATA symbol registered as an extern
    // function (the shape ir_gen emitted for `extern var errno`) must never
    // be turned into a function VALUE. Reading its value used to fabricate
    // a negative "function id" and LOAD it with no negative-address guard,
    // memcpy-ing from heap_.data()-1 (host UB, silent garbage exit code).
    {
        auto fn = makeFn("main", cider::IRType::I32);
        auto e = std::make_unique<cider::BasicBlock>("entry");
        e->addInstruction(opInstr(cider::IROp::ALLOCA, "v0",
                                  {cider::IRValue::Constant(4, cider::IRType::I32)}));
        cider::IRInstruction ld;
        ld.op = cider::IROp::LOAD;
        ld.dest = cider::IRValue::Register("v", cider::IRType::I32);
        ld.args = {cider::IRValue::Label("errno")};
        e->addInstruction(ld);
        cider::IRInstruction ret;
        ret.op = cider::IROp::RET;
        ret.args = {cider::IRValue::Constant(0, cider::IRType::I32)};
        e->addInstruction(ret);
        fn->addBlock(std::move(e));

        cider::ModuleIR mod;
        mod.addExternFunction("errno");  // extern data symbol
        mod.addFunction(std::move(fn));
        cider::VM vm;
        bool threw = false;
        try {
            vm.run(mod);
        } catch (const cider::VMError& e) {
            threw = true;
            CHECK(e.message.find("negative address") != std::string::npos);
        }
        CHECK(threw);  // before the fix: no throw, silent OOB read
    }

    // CID-006 F1 (front-end shape): ir_gen now registers `extern var` via
    // addExternVariable, so a value read cannot resolve to any function id
    // and fails with the pre-CID-006 clean unresolved-label error.
    {
        auto fn = makeFn("main", cider::IRType::I32);
        auto e = std::make_unique<cider::BasicBlock>("entry");
        e->addInstruction(opInstr(cider::IROp::ALLOCA, "v0",
                                  {cider::IRValue::Constant(4, cider::IRType::I32)}));
        cider::IRInstruction ld;
        ld.op = cider::IROp::LOAD;
        ld.dest = cider::IRValue::Register("v", cider::IRType::I32);
        ld.args = {cider::IRValue::Label("errno")};
        e->addInstruction(ld);
        cider::IRInstruction ret;
        ret.op = cider::IROp::RET;
        ret.args = {cider::IRValue::Constant(0, cider::IRType::I32)};
        e->addInstruction(ret);
        fn->addBlock(std::move(e));

        cider::ModuleIR mod;
        mod.addExternVariable("errno");
        mod.addFunction(std::move(fn));
        cider::VM vm;
        bool threw = false;
        try {
            vm.run(mod);
        } catch (const cider::VMError& e) {
            threw = true;
            CHECK(e.message.find("unresolved label errno") != std::string::npos);
        }
        CHECK(threw);
        // The split must not leak the variable into the function list.
        CHECK(mod.getExternFunctions().empty());
    }

    // CID-006 F3: the `<g>: dq <extern>` global-cell branch materializes the
    // synthetic extern id (negative), which is non-nil and dispatchable.
    {
        auto fn = makeFn("main", cider::IRType::I32);
        auto e = std::make_unique<cider::BasicBlock>("entry");
        cider::IRInstruction ld;
        ld.op = cider::IROp::LOAD;
        ld.dest = cider::IRValue::Register("gfp", cider::IRType::PTR);
        ld.args = {cider::IRValue::Label("gext"), cider::IRValue::Constant(0, cider::IRType::I32)};
        e->addInstruction(ld);
        cider::IRInstruction cmp;
        cmp.op = cider::IROp::CMP_NE;
        cmp.dest = cider::IRValue::Register("gne", cider::IRType::I32);
        cmp.args = {cider::IRValue::Register("gfp", cider::IRType::PTR),
                    cider::IRValue::Constant(0, cider::IRType::PTR)};
        e->addInstruction(cmp);
        cider::IRInstruction call;
        call.op = cider::IROp::CALL;
        call.dest = cider::IRValue::Register("gr", cider::IRType::I32);
        call.args = {cider::IRValue::Register("gfp", cider::IRType::PTR),
                     cider::IRValue::Constant(66, cider::IRType::I32)};
        e->addInstruction(call);
        cider::IRInstruction ret;
        ret.op = cider::IROp::RET;
        ret.args = {cider::IRValue::Register("gne", cider::IRType::I32)};
        e->addInstruction(ret);
        fn->addBlock(std::move(e));

        cider::ModuleIR mod;
        mod.addStringData("gext: dq putchar");
        mod.addExternFunction("putchar");
        mod.addFunction(std::move(fn));
        cider::VM vm;
        auto r = vm.run(mod);
        CHECK(r.exitCode == 1);  // extern id != nilPtr
        CHECK(r.stdoutText == "B");
    }

    // CID-006 F3: extern builtin values dispatch through indirect calls,
    // including malloc/free (not just the already-covered putchar).
    {
        auto fn = makeFn("main", cider::IRType::I32);
        auto e = std::make_unique<cider::BasicBlock>("entry");
        // slot = malloc; call it with 16 -> ptr
        e->addInstruction(opInstr(cider::IROp::ALLOCA, "m",
                                  {cider::IRValue::Constant(8, cider::IRType::I32)}));
        cider::IRInstruction stm;
        stm.op = cider::IROp::STORE;
        stm.args = {cider::IRValue::Register("m", cider::IRType::PTR),
                    cider::IRValue::Label("malloc"),
                    cider::IRValue::Constant(0, cider::IRType::I32)};
        e->addInstruction(stm);
        cider::IRInstruction ldm;
        ldm.op = cider::IROp::LOAD;
        ldm.dest = cider::IRValue::Register("mfp", cider::IRType::PTR);
        ldm.args = {cider::IRValue::Register("m", cider::IRType::PTR),
                    cider::IRValue::Constant(0, cider::IRType::I32)};
        e->addInstruction(ldm);
        cider::IRInstruction callm;
        callm.op = cider::IROp::CALL;
        callm.dest = cider::IRValue::Register("ptr", cider::IRType::PTR);
        callm.args = {cider::IRValue::Register("mfp", cider::IRType::PTR),
                      cider::IRValue::Constant(16, cider::IRType::I32)};
        e->addInstruction(callm);
        // call free value on that pointer
        e->addInstruction(opInstr(cider::IROp::ALLOCA, "f",
                                  {cider::IRValue::Constant(8, cider::IRType::I32)}));
        cider::IRInstruction stf;
        stf.op = cider::IROp::STORE;
        stf.args = {cider::IRValue::Register("f", cider::IRType::PTR),
                    cider::IRValue::Label("free"),
                    cider::IRValue::Constant(0, cider::IRType::I32)};
        e->addInstruction(stf);
        cider::IRInstruction ldf;
        ldf.op = cider::IROp::LOAD;
        ldf.dest = cider::IRValue::Register("ffp", cider::IRType::PTR);
        ldf.args = {cider::IRValue::Register("f", cider::IRType::PTR),
                    cider::IRValue::Constant(0, cider::IRType::I32)};
        e->addInstruction(ldf);
        cider::IRInstruction callf;
        callf.op = cider::IROp::CALL;
        callf.args = {cider::IRValue::Register("ffp", cider::IRType::PTR),
                      cider::IRValue::Register("ptr", cider::IRType::PTR)};
        e->addInstruction(callf);
        // return (ptr != 0)
        cider::IRInstruction cmp;
        cmp.op = cider::IROp::CMP_NE;
        cmp.dest = cider::IRValue::Register("ne", cider::IRType::I32);
        cmp.args = {cider::IRValue::Register("ptr", cider::IRType::PTR),
                    cider::IRValue::Constant(0, cider::IRType::PTR)};
        e->addInstruction(cmp);
        cider::IRInstruction ret;
        ret.op = cider::IROp::RET;
        ret.args = {cider::IRValue::Register("ne", cider::IRType::I32)};
        e->addInstruction(ret);
        fn->addBlock(std::move(e));

        cider::ModuleIR mod;
        mod.addFunction(std::move(fn));
        cider::VM vm;
        CHECK(vm.run(mod).exitCode == 1);
    }

    // CID-006 F3: an unknown negative id is rejected as a bad function id
    // (it must not be misread as an extern id).
    {
        auto fn = makeFn("main", cider::IRType::I32);
        auto e = std::make_unique<cider::BasicBlock>("entry");
        cider::IRInstruction call;
        call.op = cider::IROp::CALL;
        call.dest = cider::IRValue::Register("x", cider::IRType::I32);
        call.args = {cider::IRValue::Constant(-42, cider::IRType::PTR)};
        e->addInstruction(call);
        cider::IRInstruction ret;
        ret.op = cider::IROp::RET;
        ret.args = {cider::IRValue::Constant(0, cider::IRType::I32)};
        e->addInstruction(ret);
        fn->addBlock(std::move(e));

        cider::ModuleIR mod;
        mod.addFunction(std::move(fn));
        cider::VM vm;
        bool threw = false;
        try {
            vm.run(mod);
        } catch (const cider::VMError& err) {
            threw = true;
            CHECK(err.message.find("bad function id") != std::string::npos);
        }
        CHECK(threw);
    }

    // CID-006 F2: a valid non-builtin extern called through its value gets
    // a name-bearing error instead of a bare `call: bad function id`.
    {
        auto fn = makeFn("main", cider::IRType::I32);
        auto e = std::make_unique<cider::BasicBlock>("entry");
        e->addInstruction(opInstr(cider::IROp::ALLOCA, "eseam",
                                  {cider::IRValue::Constant(8, cider::IRType::I32)}));
        cider::IRInstruction st;
        st.op = cider::IROp::STORE;
        st.args = {cider::IRValue::Register("eseam", cider::IRType::PTR),
                   cider::IRValue::Label("ext_fn"),
                   cider::IRValue::Constant(0, cider::IRType::I32)};
        e->addInstruction(st);
        cider::IRInstruction ld;
        ld.op = cider::IROp::LOAD;
        ld.dest = cider::IRValue::Register("efp2", cider::IRType::PTR);
        ld.args = {cider::IRValue::Register("eseam", cider::IRType::PTR),
                   cider::IRValue::Constant(0, cider::IRType::I32)};
        e->addInstruction(ld);
        cider::IRInstruction call;
        call.op = cider::IROp::CALL;
        call.args = {cider::IRValue::Register("efp2", cider::IRType::PTR)};
        e->addInstruction(call);
        cider::IRInstruction ret;
        ret.op = cider::IROp::RET;
        ret.args = {cider::IRValue::Constant(0, cider::IRType::I32)};
        e->addInstruction(ret);
        fn->addBlock(std::move(e));

        cider::ModuleIR mod;
        mod.addExternFunction("ext_fn");
        mod.addFunction(std::move(fn));
        cider::VM vm;
        bool threw = false;
        try {
            vm.run(mod);
        } catch (const cider::VMError& err) {
            threw = true;
            CHECK(err.message.find("ext_fn") != std::string::npos);
        }
        CHECK(threw);
    }
    // CID-005 fix round 1: a `dq` global cell whose target name contains
    // "dw" must still resolve through the dq alias pass. The width scan used
    // to match the keyword anywhere in the line, claim the cell as (empty)
    // wide data and skip the alias pass, so the indirect call saw a bogus
    // value ("call: nil function pointer").
    {
        auto target = makeFn("dworker", cider::IRType::I32);
        auto we = std::make_unique<cider::BasicBlock>("entry");
        cider::IRInstruction wret;
        wret.op = cider::IROp::RET;
        wret.args = {cider::IRValue::Constant(21, cider::IRType::I32)};
        we->addInstruction(wret);
        target->addBlock(std::move(we));

        auto fn = makeFn("main", cider::IRType::I32);
        auto e = std::make_unique<cider::BasicBlock>("entry");
        cider::IRInstruction ld;
        ld.op = cider::IROp::LOAD;
        ld.dest = cider::IRValue::Register("gfp", cider::IRType::PTR);
        ld.args = {cider::IRValue::Label("gdw"), cider::IRValue::Constant(0, cider::IRType::I32)};
        e->addInstruction(ld);
        cider::IRInstruction call;
        call.op = cider::IROp::CALL;
        call.dest = cider::IRValue::Register("gr", cider::IRType::I32);
        call.args = {cider::IRValue::Register("gfp", cider::IRType::PTR)};
        e->addInstruction(call);
        cider::IRInstruction ret;
        ret.op = cider::IROp::RET;
        ret.args = {cider::IRValue::Register("gr", cider::IRType::I32)};
        e->addInstruction(ret);
        fn->addBlock(std::move(e));

        cider::ModuleIR mod;
        mod.addStringData("gdw: dq dworker");
        mod.addFunction(std::move(target));
        mod.addFunction(std::move(fn));
        cider::VM vm;
        CHECK(vm.run(mod).exitCode == 21);
    }

    // Same shape with a "db"-named target: this one used to loop forever in
    // the db decoder (the scan matched "db" inside the target name and
    // pushed zero bytes without advancing).
    {
        auto target = makeFn("dbworker", cider::IRType::I32);
        auto we = std::make_unique<cider::BasicBlock>("entry");
        cider::IRInstruction wret;
        wret.op = cider::IROp::RET;
        wret.args = {cider::IRValue::Constant(22, cider::IRType::I32)};
        we->addInstruction(wret);
        target->addBlock(std::move(we));

        auto fn = makeFn("main", cider::IRType::I32);
        auto e = std::make_unique<cider::BasicBlock>("entry");
        cider::IRInstruction ld;
        ld.op = cider::IROp::LOAD;
        ld.dest = cider::IRValue::Register("gfp", cider::IRType::PTR);
        ld.args = {cider::IRValue::Label("gdb"), cider::IRValue::Constant(0, cider::IRType::I32)};
        e->addInstruction(ld);
        cider::IRInstruction call;
        call.op = cider::IROp::CALL;
        call.dest = cider::IRValue::Register("gr", cider::IRType::I32);
        call.args = {cider::IRValue::Register("gfp", cider::IRType::PTR)};
        e->addInstruction(call);
        cider::IRInstruction ret;
        ret.op = cider::IROp::RET;
        ret.args = {cider::IRValue::Register("gr", cider::IRType::I32)};
        e->addInstruction(ret);
        fn->addBlock(std::move(e));

        cider::ModuleIR mod;
        mod.addStringData("gdb: dq dbworker");
        mod.addFunction(std::move(target));
        mod.addFunction(std::move(fn));
        cider::VM vm;
        CHECK(vm.run(mod).exitCode == 22);
    }
    return 0;
}

int main() { return run(); }
