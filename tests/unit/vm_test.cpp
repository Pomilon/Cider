#include <cstdio>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "ssa_deconstructor.h"
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

    // CIDER-BUG-204: the VM executes the PHI form of the IR, never the
    // SSADeconstructor's PUSH/POP form (see src/ssa_deconstructor.h). Pin
    // both halves of that contract:
    //   1. deconstructing a phi diamond does remove the phi and emit
    //      PUSH/POP, so the two backends really do see different IR; and
    //   2. feeding that deconstructed IR to the VM is REJECTED with a
    //      diagnostic naming the invariant -- not silently ignored, which
    //      would leave the phi destination unset and fail later with a
    //      confusing "read of unset register" further from the cause.
    auto phiDiamond = []() {
        auto fn = makeFn("maind", cider::IRType::I32);
        auto e = std::make_unique<cider::BasicBlock>("entry");
        e->addInstruction(opInstr(cider::IROp::COND_BR, "",
            {cider::IRValue::Constant(0, cider::IRType::I32),
             cider::IRValue::Label("T"), cider::IRValue::Label("F")}));
        auto t = std::make_unique<cider::BasicBlock>("T");
        t->addInstruction(opInstr(cider::IROp::BR, "", {cider::IRValue::Label("M")}));
        auto f = std::make_unique<cider::BasicBlock>("F");
        f->addInstruction(opInstr(cider::IROp::BR, "", {cider::IRValue::Label("M")}));
        auto m = std::make_unique<cider::BasicBlock>("M");
        cider::IRInstruction p;
        p.op = cider::IROp::PHI;
        p.dest = cider::IRValue::Register("p", cider::IRType::I32);
        p.phiArgs["T"] = cider::IRValue::Constant(1, cider::IRType::I32);
        p.phiArgs["F"] = cider::IRValue::Constant(2, cider::IRType::I32);
        m->addInstruction(p);
        cider::IRInstruction r;
        r.op = cider::IROp::RET;
        r.args = {cider::IRValue::Register("p", cider::IRType::I32)};
        m->addInstruction(r);
        fn->addBlock(std::move(e));
        fn->addBlock(std::move(t));
        fn->addBlock(std::move(f));
        fn->addBlock(std::move(m));
        return fn;
    };

    auto countOp = [](const cider::FunctionIR* f, cider::IROp op) {
        size_t n = 0;
        for (const auto& b : f->getBlocks())
            for (const auto& in : b->getInstructions())
                if (in.op == op) ++n;
        return n;
    };

    auto deconMod = std::make_unique<cider::ModuleIR>();
    deconMod->addFunction(phiDiamond());
    const cider::FunctionIR* deconIn = deconMod->getFunctions().front().get();
    CHECK(countOp(deconIn, cider::IROp::PHI) == 1);
    CHECK(countOp(deconIn, cider::IROp::PUSH) == 0);
    cider::SSADeconstructor decon;
    decon.process(*deconMod);
    const cider::FunctionIR* deconOut = deconMod->getFunctions().front().get();
    CHECK(countOp(deconOut, cider::IROp::PHI) == 0);
    CHECK(countOp(deconOut, cider::IROp::PUSH) == 2);
    CHECK(countOp(deconOut, cider::IROp::POP) == 2);

    // (2) The VM must refuse that deconstructed IR outright.
    cider::VM vmDecon;
    bool deconThrew = false;
    std::string deconMsg;
    try {
        vmDecon.run(*deconMod, "maind");
    } catch (const cider::VMError& err) {
        deconThrew = true;
        deconMsg = err.message;
    }
    CHECK(deconThrew);
    CHECK(deconMsg.find("native-only") != std::string::npos);

    // (3) The phi-form IR still runs: a fresh undeconstructed diamond.
    auto phiMod = std::make_unique<cider::ModuleIR>();
    phiMod->addFunction(phiDiamond());
    cider::VM vmPhi;
    CHECK(vmPhi.run(*phiMod, "maind").exitCode == 2);

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

    // CIDER-BUG-115: frame reuse is observable on the VM. A callee-local
    // by-value object whose address escaped (an interface's data word, a
    // returned `[x, x, x]`, a std.io buffer) stays readable immediately
    // after the return — native's dead stack slots are still there — but
    // once another frame claims the region the bytes are gone, exactly like
    // native's next call overwriting those slots. The VM zeroes a freshly
    // claimed frame chunk so a dangling pointer cannot read the old object
    // back after reuse.
    {
        auto retarget = makeFn("retarget", cider::IRType::VOID);
        retarget->addParam("arg_p", cider::IRType::PTR);
        auto rb = std::make_unique<cider::BasicBlock>("entry");
        rb->addInstruction(opInstr(cider::IROp::ALLOCA, "tmp",
                                   {cider::IRValue::Constant(8, cider::IRType::I32)}));
        cider::IRInstruction seed;
        seed.op = cider::IROp::STORE;
        seed.args = {cider::IRValue::Register("tmp", cider::IRType::PTR),
                     cider::IRValue::Constant(42, cider::IRType::I32),
                     cider::IRValue::Constant(0, cider::IRType::I32)};
        rb->addInstruction(seed);
        cider::IRInstruction leak;
        leak.op = cider::IROp::STORE;
        leak.args = {cider::IRValue::Register("arg_p", cider::IRType::PTR),
                     cider::IRValue::Register("tmp", cider::IRType::PTR),
                     cider::IRValue::Constant(0, cider::IRType::I32)};
        rb->addInstruction(leak);
        cider::IRInstruction rret;
        rret.op = cider::IROp::RET;
        rb->addInstruction(rret);
        retarget->addBlock(std::move(rb));

        // A later call whose first ALLOCA claims a frame region (and writes
        // nothing there).
        auto reuse = makeFn("reuse", cider::IRType::VOID);
        auto ub = std::make_unique<cider::BasicBlock>("entry");
        ub->addInstruction(opInstr(cider::IROp::ALLOCA, "scratch",
                                   {cider::IRValue::Constant(8, cider::IRType::I32)}));
        cider::IRInstruction uret;
        uret.op = cider::IROp::RET;
        ub->addInstruction(uret);
        reuse->addBlock(std::move(ub));

        // entry: call retarget(&slot) [, call reuse], v = *(slot's pointee).
        auto buildEntry = [&](const std::string& name, bool callReuse) {
            auto entry = makeFn(name, cider::IRType::I32);
            auto b = std::make_unique<cider::BasicBlock>("entry");
            b->addInstruction(opInstr(cider::IROp::ALLOCA, "slot",
                                      {cider::IRValue::Constant(8, cider::IRType::I32)}));
            cider::IRInstruction callRetarget;
            callRetarget.op = cider::IROp::CALL;
            callRetarget.args = {cider::IRValue::Label("retarget"),
                                 cider::IRValue::Register("slot", cider::IRType::PTR)};
            b->addInstruction(callRetarget);
            if (callReuse) {
                cider::IRInstruction callReuse;
                callReuse.op = cider::IROp::CALL;
                callReuse.args = {cider::IRValue::Label("reuse")};
                b->addInstruction(callReuse);
            }
            cider::IRInstruction loadP;
            loadP.op = cider::IROp::LOAD;
            loadP.dest = cider::IRValue::Register("p", cider::IRType::PTR);
            loadP.args = {cider::IRValue::Register("slot", cider::IRType::PTR),
                          cider::IRValue::Constant(0, cider::IRType::I32)};
            b->addInstruction(loadP);
            cider::IRInstruction loadV;
            loadV.op = cider::IROp::LOAD;
            loadV.dest = cider::IRValue::Register("v", cider::IRType::I32);
            loadV.args = {cider::IRValue::Register("p", cider::IRType::PTR),
                          cider::IRValue::Constant(0, cider::IRType::I32)};
            b->addInstruction(loadV);
            cider::IRInstruction ret;
            ret.op = cider::IROp::RET;
            ret.args = {cider::IRValue::Register("v", cider::IRType::I32)};
            b->addInstruction(ret);
            entry->addBlock(std::move(b));
            return entry;
        };

        cider::ModuleIR mod;
        mod.addFunction(std::move(retarget));
        mod.addFunction(std::move(reuse));
        mod.addFunction(buildEntry("immediate", /*callReuse=*/false));
        mod.addFunction(buildEntry("reuseFrame", /*callReuse=*/true));

        // Before any reuse the dead bytes are still visible, like native's
        // unconsumed stack slots (the supported read-all/tripled pattern).
        cider::VM vmNow;
        CHECK(vmNow.run(mod, "immediate").exitCode == 42);
        // After a later frame claims (and its prologue zeroes) the region,
        // the dangling pointer no longer reads the old object.
        cider::VM vmReuse;
        CHECK(vmReuse.run(mod, "reuseFrame").exitCode == 0);
    }

    // CIDER-BUG-149: an oversized local (one the 4096-byte frame chunk cannot
    // hold) is a heap fallback in the VM. It is frame storage: freed when the
    // frame dies and zeroed when the block is claimed again, so a dangling
    // pointer cannot keep reading the dead frame's object after the block is
    // recycled, and repeated calls reuse the retired blocks instead of
    // stacking up fresh ones (the old leak).
    {
        // retargetBig(&slot): seed an 8 KiB local, publish its address.
        auto retargetBig = makeFn("retargetBig", cider::IRType::VOID);
        retargetBig->addParam("arg_p", cider::IRType::PTR);
        auto rb = std::make_unique<cider::BasicBlock>("entry");
        rb->addInstruction(opInstr(cider::IROp::ALLOCA, "big",
                                   {cider::IRValue::Constant(8192, cider::IRType::I64)}));
        cider::IRInstruction seed;
        seed.op = cider::IROp::STORE;
        seed.args = {cider::IRValue::Register("big", cider::IRType::PTR),
                     cider::IRValue::Constant(42, cider::IRType::I32),
                     cider::IRValue::Constant(0, cider::IRType::I32)};
        rb->addInstruction(seed);
        cider::IRInstruction leak;
        leak.op = cider::IROp::STORE;
        leak.args = {cider::IRValue::Register("arg_p", cider::IRType::PTR),
                     cider::IRValue::Register("big", cider::IRType::PTR),
                     cider::IRValue::Constant(0, cider::IRType::I32)};
        rb->addInstruction(leak);
        cider::IRInstruction rret;
        rret.op = cider::IROp::RET;
        rb->addInstruction(rret);
        retargetBig->addBlock(std::move(rb));

        // reuseBig(): claims a frame chunk and an 8 KiB fallback, writes
        // nothing there.
        auto reuseBig = makeFn("reuseBig", cider::IRType::VOID);
        auto ub = std::make_unique<cider::BasicBlock>("entry");
        ub->addInstruction(opInstr(cider::IROp::ALLOCA, "scratch",
                                   {cider::IRValue::Constant(8192, cider::IRType::I64)}));
        cider::IRInstruction uret;
        uret.op = cider::IROp::RET;
        ub->addInstruction(uret);
        reuseBig->addBlock(std::move(ub));

        // entry: call retargetBig(&slot) [, call reuseBig], v = *slot's pointee.
        auto buildBigEntry = [&](const std::string& name, bool callReuse) {
            auto entry = makeFn(name, cider::IRType::I32);
            auto b = std::make_unique<cider::BasicBlock>("entry");
            b->addInstruction(opInstr(cider::IROp::ALLOCA, "slot",
                                      {cider::IRValue::Constant(8, cider::IRType::I32)}));
            cider::IRInstruction callRetarget;
            callRetarget.op = cider::IROp::CALL;
            callRetarget.args = {cider::IRValue::Label("retargetBig"),
                                 cider::IRValue::Register("slot", cider::IRType::PTR)};
            b->addInstruction(callRetarget);
            if (callReuse) {
                cider::IRInstruction callReuse;
                callReuse.op = cider::IROp::CALL;
                callReuse.args = {cider::IRValue::Label("reuseBig")};
                b->addInstruction(callReuse);
            }
            cider::IRInstruction loadP;
            loadP.op = cider::IROp::LOAD;
            loadP.dest = cider::IRValue::Register("p", cider::IRType::PTR);
            loadP.args = {cider::IRValue::Register("slot", cider::IRType::PTR),
                          cider::IRValue::Constant(0, cider::IRType::I32)};
            b->addInstruction(loadP);
            cider::IRInstruction loadV;
            loadV.op = cider::IROp::LOAD;
            loadV.dest = cider::IRValue::Register("v", cider::IRType::I32);
            loadV.args = {cider::IRValue::Register("p", cider::IRType::PTR),
                          cider::IRValue::Constant(0, cider::IRType::I32)};
            b->addInstruction(loadV);
            cider::IRInstruction ret;
            ret.op = cider::IROp::RET;
            ret.args = {cider::IRValue::Register("v", cider::IRType::I32)};
            b->addInstruction(ret);
            entry->addBlock(std::move(b));
            return entry;
        };

        cider::ModuleIR mod;
        mod.addFunction(std::move(retargetBig));
        mod.addFunction(std::move(reuseBig));
        mod.addFunction(buildBigEntry("bigImmediate", /*callReuse=*/false));
        mod.addFunction(buildBigEntry("bigReuse", /*callReuse=*/true));

        // Immediate post-return read: the dead bytes are still there, like a
        // dead native stack slot (the supported read-all/tripled pattern).
        cider::VM vmNow;
        CHECK(vmNow.run(mod, "bigImmediate").exitCode == 42);
        // Once the fallback block is claimed again it is zeroed: no old object.
        cider::VM vmReuse;
        CHECK(vmReuse.run(mod, "bigReuse").exitCode == 0);
        // Retirement returned every block to the free list (no leak).
        CHECK(vmReuse.heapBytes() == vmReuse.freeListBytes());
    }

    // CIDER-BUG-149 accounting: repeated calls whose frames carry oversized
    // locals must not grow the VM heap; the retired blocks are recycled.
    {
        auto makeBusy = []() {
            auto fn = makeFn("busy", cider::IRType::VOID);
            auto b = std::make_unique<cider::BasicBlock>("entry");
            b->addInstruction(opInstr(cider::IROp::ALLOCA, "big",
                                      {cider::IRValue::Constant(8192, cider::IRType::I64)}));
            cider::IRInstruction st;
            st.op = cider::IROp::STORE;
            st.args = {cider::IRValue::Register("big", cider::IRType::PTR),
                       cider::IRValue::Constant(7, cider::IRType::I32),
                       cider::IRValue::Constant(0, cider::IRType::I32)};
            b->addInstruction(st);
            cider::IRInstruction ret;
            ret.op = cider::IROp::RET;
            b->addInstruction(ret);
            fn->addBlock(std::move(b));
            return fn;
        };
        auto buildCalls = [&](const std::string& name, int calls) {
            auto fn = makeFn(name, cider::IRType::I32);
            auto b = std::make_unique<cider::BasicBlock>("entry");
            for (int i = 0; i < calls; ++i) {
                cider::IRInstruction call;
                call.op = cider::IROp::CALL;
                call.args = {cider::IRValue::Label("busy")};
                b->addInstruction(call);
            }
            cider::IRInstruction ret;
            ret.op = cider::IROp::RET;
            ret.args = {cider::IRValue::Constant(0, cider::IRType::I32)};
            b->addInstruction(ret);
            fn->addBlock(std::move(b));
            return fn;
        };

        cider::ModuleIR modOne;
        modOne.addFunction(makeBusy());
        modOne.addFunction(buildCalls("one", 1));
        cider::ModuleIR modMany;
        modMany.addFunction(makeBusy());
        modMany.addFunction(buildCalls("many", 8));
        cider::VM vmOne;
        CHECK(vmOne.run(modOne, "one").exitCode == 0);
        cider::VM vmMany;
        CHECK(vmMany.run(modMany, "many").exitCode == 0);
        CHECK(vmOne.heapBytes() == vmMany.heapBytes());
        CHECK(vmMany.heapBytes() == vmMany.freeListBytes());
    }

    // CIDER-BUG-149: the longjmp unwind retires unwound frames through the
    // same retireFrame; an oversized local in an unwound frame is freed too.
    {
        auto deep = makeFn("deep", cider::IRType::VOID);
        deep->addParam("arg_buf", cider::IRType::PTR);
        auto db = std::make_unique<cider::BasicBlock>("entry");
        db->addInstruction(opInstr(cider::IROp::ALLOCA, "big",
                                   {cider::IRValue::Constant(8192, cider::IRType::I64)}));
        cider::IRInstruction jump;
        jump.op = cider::IROp::CALL;
        jump.args = {cider::IRValue::Label("longjmp"),
                     cider::IRValue::Register("arg_buf", cider::IRType::PTR),
                     cider::IRValue::Constant(1, cider::IRType::I32)};
        db->addInstruction(jump);
        cider::IRInstruction dret;
        dret.op = cider::IROp::RET;
        db->addInstruction(dret);
        deep->addBlock(std::move(db));

        auto fn = makeFn("main", cider::IRType::I32);
        auto e = std::make_unique<cider::BasicBlock>("entry");
        e->addInstruction(opInstr(cider::IROp::ALLOCA, "buf",
                                  {cider::IRValue::Constant(64, cider::IRType::I32)}));
        cider::IRInstruction setjmpCall;
        setjmpCall.op = cider::IROp::CALL;
        setjmpCall.dest = cider::IRValue::Register("sj", cider::IRType::I32);
        setjmpCall.args = {cider::IRValue::Label("_setjmp"),
                           cider::IRValue::Register("buf", cider::IRType::PTR)};
        e->addInstruction(setjmpCall);
        cider::IRInstruction br;
        br.op = cider::IROp::COND_BR;
        br.args = {cider::IRValue::Register("sj", cider::IRType::I32),
                   cider::IRValue::Label("after"), cider::IRValue::Label("first")};
        e->addInstruction(br);

        auto first = std::make_unique<cider::BasicBlock>("first");
        cider::IRInstruction callDeep;
        callDeep.op = cider::IROp::CALL;
        callDeep.args = {cider::IRValue::Label("deep"),
                         cider::IRValue::Register("buf", cider::IRType::PTR)};
        first->addInstruction(callDeep);
        cider::IRInstruction brAfter;
        brAfter.op = cider::IROp::BR;
        brAfter.args = {cider::IRValue::Label("after")};
        first->addInstruction(brAfter);

        auto after = std::make_unique<cider::BasicBlock>("after");
        cider::IRInstruction ret;
        ret.op = cider::IROp::RET;
        ret.args = {cider::IRValue::Constant(0, cider::IRType::I32)};
        after->addInstruction(ret);

        fn->addBlock(std::move(e));
        fn->addBlock(std::move(first));
        fn->addBlock(std::move(after));

        cider::ModuleIR mod;
        mod.addFunction(std::move(deep));
        mod.addFunction(std::move(fn));
        cider::VM vm;
        CHECK(vm.run(mod).exitCode == 0);
        // deep's chunk and fallback were unwound; every allocated byte is
        // back on the free list (the old leak left the 8 KiB block live).
        CHECK(vm.heapBytes() == vm.freeListBytes());
    }

    // CIDER-BUG-159: the exit builtin abandons every live frame instead of
    // unwinding it. The frames must still be retired, like RET/longjmp/
    // fall-off, so their chunks and oversized fallbacks return to the free
    // list (the heap dies with the VM either way, but the free list stays
    // consistent for a reused VM and for the accounting).
    {
        // deep(): an oversized local, then exit(7) from inside the call.
        auto deep = makeFn("deep", cider::IRType::VOID);
        auto db = std::make_unique<cider::BasicBlock>("entry");
        db->addInstruction(opInstr(cider::IROp::ALLOCA, "big",
                                   {cider::IRValue::Constant(8192, cider::IRType::I64)}));
        cider::IRInstruction ex;
        ex.op = cider::IROp::CALL;
        ex.args = {cider::IRValue::Label("exit"),
                   cider::IRValue::Constant(7, cider::IRType::I32)};
        db->addInstruction(ex);
        cider::IRInstruction dret;
        dret.op = cider::IROp::RET;
        db->addInstruction(dret);
        deep->addBlock(std::move(db));

        // main(): a frame chunk, then the exiting call.
        auto fn = makeFn("main", cider::IRType::I32);
        auto b = std::make_unique<cider::BasicBlock>("entry");
        b->addInstruction(opInstr(cider::IROp::ALLOCA, "slot",
                                  {cider::IRValue::Constant(8, cider::IRType::I32)}));
        cider::IRInstruction callDeep;
        callDeep.op = cider::IROp::CALL;
        callDeep.args = {cider::IRValue::Label("deep")};
        b->addInstruction(callDeep);
        cider::IRInstruction ret;
        ret.op = cider::IROp::RET;
        ret.args = {cider::IRValue::Constant(0, cider::IRType::I32)};
        b->addInstruction(ret);
        fn->addBlock(std::move(b));

        cider::ModuleIR mod;
        mod.addFunction(std::move(deep));
        mod.addFunction(std::move(fn));
        cider::VM vm;
        CHECK(vm.run(mod).exitCode == 7);
        // main's chunk and deep's chunk + fallback were retired on exit
        // (pre-fix every block stayed live, freeListBytes() == 0).
        CHECK(vm.heapBytes() == vm.freeListBytes());
    }

    // CIDER-BUG-159: freeing an ALLOCA pointer by hand is UB (native
    // crashes); pre-fix the VM pushed the same block onto the free list
    // again when retireFrame released it, so the block was counted twice
    // and a later malloc could hand the same bytes out twice. VM::free now
    // ignores a free that overlaps a block already on the free list.
    {
        // (a) explicit free of a frame chunk, then RET retires it again.
        auto fn = makeFn("main", cider::IRType::I32);
        auto b = std::make_unique<cider::BasicBlock>("entry");
        b->addInstruction(opInstr(cider::IROp::ALLOCA, "p",
                                  {cider::IRValue::Constant(8, cider::IRType::I32)}));
        cider::IRInstruction fr;
        fr.op = cider::IROp::CALL;
        fr.args = {cider::IRValue::Label("free"),
                   cider::IRValue::Register("p", cider::IRType::PTR)};
        b->addInstruction(fr);
        cider::IRInstruction ret;
        ret.op = cider::IROp::RET;
        ret.args = {cider::IRValue::Constant(0, cider::IRType::I32)};
        b->addInstruction(ret);
        fn->addBlock(std::move(b));
        cider::ModuleIR mod;
        mod.addFunction(std::move(fn));
        cider::VM vm;
        CHECK(vm.run(mod).exitCode == 0);
        // One block, one free-list entry (pre-fix: 2x the chunk bytes).
        CHECK(vm.heapBytes() == vm.freeListBytes());
    }
    {
        // (b) the same UB on an oversized fallback: retireFrame's second
        // free must not duplicate the fallback block either.
        auto fn = makeFn("main", cider::IRType::I32);
        auto b = std::make_unique<cider::BasicBlock>("entry");
        b->addInstruction(opInstr(cider::IROp::ALLOCA, "big",
                                  {cider::IRValue::Constant(8192, cider::IRType::I64)}));
        cider::IRInstruction fr;
        fr.op = cider::IROp::CALL;
        fr.args = {cider::IRValue::Label("free"),
                   cider::IRValue::Register("big", cider::IRType::PTR)};
        b->addInstruction(fr);
        cider::IRInstruction ret;
        ret.op = cider::IROp::RET;
        ret.args = {cider::IRValue::Constant(0, cider::IRType::I32)};
        b->addInstruction(ret);
        fn->addBlock(std::move(b));
        cider::ModuleIR mod;
        mod.addFunction(std::move(fn));
        cider::VM vm;
        CHECK(vm.run(mod).exitCode == 0);
        CHECK(vm.heapBytes() == vm.freeListBytes());
    }
    {
        // (c) classic double free of a malloc'd block, with valid frees of
        // two distinct blocks around it: the guard drops only the
        // duplicate, so every distinct block is listed exactly once.
        auto fn = makeFn("main", cider::IRType::I32);
        auto b = std::make_unique<cider::BasicBlock>("entry");
        auto mallocCall = [](const std::string& destName) {
            cider::IRInstruction m;
            m.op = cider::IROp::CALL;
            m.dest = cider::IRValue::Register(destName, cider::IRType::PTR);
            m.args = {cider::IRValue::Label("malloc"),
                      cider::IRValue::Constant(64, cider::IRType::I64)};
            return m;
        };
        auto freeCall = [](const std::string& reg) {
            cider::IRInstruction f;
            f.op = cider::IROp::CALL;
            f.args = {cider::IRValue::Label("free"),
                      cider::IRValue::Register(reg, cider::IRType::PTR)};
            return f;
        };
        b->addInstruction(mallocCall("p"));
        b->addInstruction(mallocCall("q"));
        b->addInstruction(freeCall("p"));
        b->addInstruction(freeCall("q"));
        b->addInstruction(freeCall("q"));  // duplicate: ignored, not re-listed
        cider::IRInstruction ret;
        ret.op = cider::IROp::RET;
        ret.args = {cider::IRValue::Constant(0, cider::IRType::I32)};
        b->addInstruction(ret);
        fn->addBlock(std::move(b));
        cider::ModuleIR mod;
        mod.addFunction(std::move(fn));
        cider::VM vm;
        CHECK(vm.run(mod).exitCode == 0);
        CHECK(vm.heapBytes() == vm.freeListBytes());
    }

    // CIDER-ISS-016: a handler buffer whose frame is gone must read as "no
    // handler", not be resumed. The IR here is hand-built to the shape the
    // bug needs — a callee that registers a handler and returns WITHOUT
    // restoring the chain, then a throw that finds the stale pointer. Before
    // the fix the longjmp found the surviving jumpStates_ entry (its
    // frameIndex still in range) and resumed a dead frame, so the program
    // continued at a retired frame's saved block/instr and printed garbage;
    // now it takes the defined uncaught path.
    {
        // register(): alloca buf; setjmp(buf); ret 0 — the chain keeps buf.
        auto reg = makeFn("register", cider::IRType::I32);
        auto rb = std::make_unique<cider::BasicBlock>("entry");
        rb->addInstruction(opInstr(cider::IROp::ALLOCA, "rbuf",
                                   {cider::IRValue::Constant(200, cider::IRType::I32)}));
        rb->addInstruction(opInstr(cider::IROp::STORE, "",
                                   {cider::IRValue::Label("_exception_buf_ptr"),
                                    cider::IRValue::Register("rbuf", cider::IRType::PTR)}));
        cider::IRInstruction rsj;
        rsj.op = cider::IROp::CALL;
        rsj.dest = cider::IRValue::Register("rsjd", cider::IRType::I32);
        rsj.args = {cider::IRValue::Label("_setjmp"),
                    cider::IRValue::Register("rbuf", cider::IRType::PTR)};
        rb->addInstruction(rsj);
        cider::IRInstruction rret;
        rret.op = cider::IROp::RET;
        rret.args = {cider::IRValue::Constant(0, cider::IRType::I32)};
        rb->addInstruction(rret);
        reg->addBlock(std::move(rb));

        // main(): call register(); then longjmp to whatever the chain holds.
        auto fn = makeFn("main", cider::IRType::I32);
        auto e = std::make_unique<cider::BasicBlock>("entry");
        e->addInstruction(opInstr(cider::IROp::CALL, "",
                                  {cider::IRValue::Label("register")}));
        e->addInstruction(opInstr(cider::IROp::LOAD, "chain",
                                  {cider::IRValue::Label("_exception_buf_ptr")}));
        cider::IRInstruction jump;
        jump.op = cider::IROp::CALL;
        jump.args = {cider::IRValue::Label("longjmp"),
                     cider::IRValue::Register("chain", cider::IRType::PTR),
                     cider::IRValue::Constant(1, cider::IRType::I32)};
        e->addInstruction(jump);
        cider::IRInstruction ret;
        ret.op = cider::IROp::RET;
        ret.args = {cider::IRValue::Constant(0, cider::IRType::I32)};
        e->addInstruction(ret);
        fn->addBlock(std::move(e));

        cider::ModuleIR mod;
        mod.addGlobalAsm("global _exception_buf_ptr");
        mod.addGlobalAsm("_exception_buf_ptr: resq 1");
        mod.addFunction(std::move(reg));
        mod.addFunction(std::move(fn));
        cider::VM vm;
        bool threw = false;
        try {
            vm.run(mod);
        } catch (const cider::VMError& e) {
            threw = true;
            // The stale pointer reports the same message as the null one
            // (CIDER-ISS-010), so both backends print the same stderr.
            CHECK(std::string(e.message) == "uncaught exception");
        }
        CHECK(threw);
    }

    // CIDER-ISS-016: the same stale buffer, but the frame-index guard alone
    // does not catch it. register()'s jumpStates_ entry records frameIndex
    // 2; after it returns, a() calls b(), which becomes frame index 2 again,
    // so a longjmp to the stale buffer finds an in-range index and resumes
    // b at register's saved block with register's register snapshot. Only
    // the ownership check (the buffer must live in the frame that captured
    // it) rejects this. Pre-fix this corrupted state; b is entered as if
    // mid-body, so the program runs off into b's blocks with the wrong
    // registers and the observable result is not the defined error.
    {
        // register(): alloca buf; store chain=buf; setjmp(buf); ret 0.
        auto reg = makeFn("register", cider::IRType::I32);
        auto rb = std::make_unique<cider::BasicBlock>("entry");
        rb->addInstruction(opInstr(cider::IROp::ALLOCA, "rbuf",
                                   {cider::IRValue::Constant(200, cider::IRType::I32)}));
        rb->addInstruction(opInstr(cider::IROp::STORE, "",
                                   {cider::IRValue::Label("_exception_buf_ptr"),
                                    cider::IRValue::Register("rbuf", cider::IRType::PTR)}));
        cider::IRInstruction rsj;
        rsj.op = cider::IROp::CALL;
        rsj.dest = cider::IRValue::Register("rsjd", cider::IRType::I32);
        rsj.args = {cider::IRValue::Label("_setjmp"),
                    cider::IRValue::Register("rbuf", cider::IRType::PTR)};
        rb->addInstruction(rsj);
        cider::IRInstruction rret;
        rret.op = cider::IROp::RET;
        rret.args = {cider::IRValue::Constant(0, cider::IRType::I32)};
        rb->addInstruction(rret);
        reg->addBlock(std::move(rb));

        // a(): register(); then b(); then ret 0.  a is frame index 1, so
        // register runs as index 2 and b later runs as index 2 too.
        auto a = makeFn("a", cider::IRType::I32);
        auto ab = std::make_unique<cider::BasicBlock>("entry");
        ab->addInstruction(opInstr(cider::IROp::CALL, "ra",
                                   {cider::IRValue::Label("register")}));
        ab->addInstruction(opInstr(cider::IROp::CALL, "rb",
                                   {cider::IRValue::Label("b")}));
        cider::IRInstruction aret;
        aret.op = cider::IROp::RET;
        aret.args = {cider::IRValue::Constant(0, cider::IRType::I32)};
        ab->addInstruction(aret);
        a->addBlock(std::move(ab));

        // b(): load the chain and longjmp to whatever it holds. Its own
        // frame is index 2, the index register's dead one recorded.
        auto b = makeFn("b", cider::IRType::I32);
        auto bb = std::make_unique<cider::BasicBlock>("entry");
        bb->addInstruction(opInstr(cider::IROp::LOAD, "chain",
                                   {cider::IRValue::Label("_exception_buf_ptr")}));
        cider::IRInstruction bj;
        bj.op = cider::IROp::CALL;
        bj.args = {cider::IRValue::Label("longjmp"),
                   cider::IRValue::Register("chain", cider::IRType::PTR),
                   cider::IRValue::Constant(1, cider::IRType::I32)};
        bb->addInstruction(bj);
        cider::IRInstruction bret;
        bret.op = cider::IROp::RET;
        bret.args = {cider::IRValue::Constant(0, cider::IRType::I32)};
        bb->addInstruction(bret);
        b->addBlock(std::move(bb));

        auto fn = makeFn("main", cider::IRType::I32);
        auto e = std::make_unique<cider::BasicBlock>("entry");
        e->addInstruction(opInstr(cider::IROp::CALL, "r",
                                  {cider::IRValue::Label("a")}));
        cider::IRInstruction ret;
        ret.op = cider::IROp::RET;
        ret.args = {cider::IRValue::Constant(0, cider::IRType::I32)};
        e->addInstruction(ret);
        fn->addBlock(std::move(e));

        cider::ModuleIR mod;
        mod.addGlobalAsm("global _exception_buf_ptr");
        mod.addGlobalAsm("_exception_buf_ptr: resq 1");
        mod.addFunction(std::move(reg));
        mod.addFunction(std::move(a));
        mod.addFunction(std::move(b));
        mod.addFunction(std::move(fn));
        cider::VM vm;
        bool threw = false;
        try {
            vm.run(mod);
        } catch (const cider::VMError& e) {
            threw = true;
            CHECK(std::string(e.message) == "uncaught exception");
        }
        CHECK(threw);
    }

    // CIDER-ISS-016: a live handler still works. Same shape, but the frame
    // that owns the buffer is still on the stack when the longjmp runs, so
    // the jump must land in its catch and resume normally.
    {
        // owner(): the frame that owns buf installs a handler, calls the
        // thrower (which longjmps) and the jump lands back in catch.
        auto owner = makeFn("owner", cider::IRType::I32);
        auto ob = std::make_unique<cider::BasicBlock>("entry");
        ob->addInstruction(opInstr(cider::IROp::ALLOCA, "obuf",
                                   {cider::IRValue::Constant(200, cider::IRType::I32)}));
        ob->addInstruction(opInstr(cider::IROp::STORE, "",
                                   {cider::IRValue::Label("_exception_buf_ptr"),
                                    cider::IRValue::Register("obuf", cider::IRType::PTR)}));
        cider::IRInstruction osj;
        osj.op = cider::IROp::CALL;
        osj.dest = cider::IRValue::Register("osjd", cider::IRType::I32);
        osj.args = {cider::IRValue::Label("_setjmp"),
                    cider::IRValue::Register("obuf", cider::IRType::PTR)};
        ob->addInstruction(osj);
        cider::IRInstruction obr;
        obr.op = cider::IROp::COND_BR;
        obr.args = {cider::IRValue::Register("osjd", cider::IRType::I32),
                    cider::IRValue::Label("catch"), cider::IRValue::Label("body")};
        ob->addInstruction(obr);
        auto obody = std::make_unique<cider::BasicBlock>("body");
        obody->addInstruction(opInstr(cider::IROp::CALL, "",
                                      {cider::IRValue::Label("thrower"),
                                       cider::IRValue::Register("obuf", cider::IRType::PTR)}));
        cider::IRInstruction ob2;
        ob2.op = cider::IROp::BR;
        ob2.args = {cider::IRValue::Label("catch")};
        obody->addInstruction(ob2);
        auto ocatch = std::make_unique<cider::BasicBlock>("catch");
        cider::IRInstruction oret;
        oret.op = cider::IROp::RET;
        oret.args = {cider::IRValue::Constant(33, cider::IRType::I32)};
        ocatch->addInstruction(oret);
        owner->addBlock(std::move(ob));
        owner->addBlock(std::move(obody));
        owner->addBlock(std::move(ocatch));

        auto thr = makeFn("thrower", cider::IRType::VOID);
        thr->addParam("arg_buf", cider::IRType::PTR);
        auto tb = std::make_unique<cider::BasicBlock>("entry");
        cider::IRInstruction tj;
        tj.op = cider::IROp::CALL;
        tj.args = {cider::IRValue::Label("longjmp"),
                   cider::IRValue::Register("arg_buf", cider::IRType::PTR),
                   cider::IRValue::Constant(1, cider::IRType::I32)};
        tb->addInstruction(tj);
        cider::IRInstruction tret;
        tret.op = cider::IROp::RET;
        tb->addInstruction(tret);
        thr->addBlock(std::move(tb));

        auto fn = makeFn("main", cider::IRType::I32);
        auto e = std::make_unique<cider::BasicBlock>("entry");
        e->addInstruction(opInstr(cider::IROp::CALL, "r",
                                  {cider::IRValue::Label("owner")}));
        cider::IRInstruction ret;
        ret.op = cider::IROp::RET;
        ret.args = {cider::IRValue::Register("r", cider::IRType::I32)};
        e->addInstruction(ret);
        fn->addBlock(std::move(e));

        cider::ModuleIR mod;
        mod.addGlobalAsm("global _exception_buf_ptr");
        mod.addGlobalAsm("_exception_buf_ptr: resq 1");
        mod.addFunction(std::move(owner));
        mod.addFunction(std::move(thr));
        mod.addFunction(std::move(fn));
        cider::VM vm;
        // 33 comes from the catch block, so the jump really landed there.
        CHECK(vm.run(mod).exitCode == 33);
    }

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
    vm6.setStepLimit(10000);  // CIDER-CHORE-002: pin the cap, not 1e9 steps
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

    // CIDER-BUG-059: nilPtr is the zero address. A LOAD/STORE/MEMCPY through
    // it used to read/write heap offset 0 (the first block's size header) and
    // return silent garbage where native traps with SIGSEGV. Each must now
    // throw a clear VMError instead.
    {
        auto fn = makeFn("main", cider::IRType::I32);
        auto e = std::make_unique<cider::BasicBlock>("entry");
        cider::IRInstruction ld;
        ld.op = cider::IROp::LOAD;
        ld.dest = cider::IRValue::Register("v", cider::IRType::I32);
        ld.args = {cider::IRValue::Constant(0, cider::IRType::PTR),
                   cider::IRValue::Constant(0, cider::IRType::I32)};
        e->addInstruction(ld);
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
            CHECK(err.message.find("null pointer dereference") != std::string::npos);
        }
        CHECK(threw);  // before the fix: silent read of heap offset 0
    }
    {
        auto fn = makeFn("main", cider::IRType::I32);
        auto e = std::make_unique<cider::BasicBlock>("entry");
        cider::IRInstruction st;
        st.op = cider::IROp::STORE;
        st.args = {cider::IRValue::Constant(0, cider::IRType::PTR),
                   cider::IRValue::Constant(7, cider::IRType::I32),
                   cider::IRValue::Constant(0, cider::IRType::I32)};
        e->addInstruction(st);
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
            CHECK(err.message.find("null pointer dereference") != std::string::npos);
        }
        CHECK(threw);  // before the fix: silent write to heap offset 0
    }
    {
        // MEMCPY with a nil destination and a real source.
        auto fn = makeFn("main", cider::IRType::I32);
        auto e = std::make_unique<cider::BasicBlock>("entry");
        e->addInstruction(opInstr(cider::IROp::ALLOCA, "buf",
                                  {cider::IRValue::Constant(8, cider::IRType::I32)}));
        cider::IRInstruction mc;
        mc.op = cider::IROp::MEMCPY;
        mc.args = {cider::IRValue::Constant(0, cider::IRType::PTR),
                   cider::IRValue::Register("buf", cider::IRType::PTR),
                   cider::IRValue::Constant(4, cider::IRType::I32)};
        e->addInstruction(mc);
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
            CHECK(err.message.find("null pointer dereference") != std::string::npos);
        }
        CHECK(threw);  // before the fix: silent write to heap offset 0
    }
    {
        // MEMCPY with a real destination and a nil source.
        auto fn = makeFn("main", cider::IRType::I32);
        auto e = std::make_unique<cider::BasicBlock>("entry");
        e->addInstruction(opInstr(cider::IROp::ALLOCA, "buf",
                                  {cider::IRValue::Constant(8, cider::IRType::I32)}));
        cider::IRInstruction mc;
        mc.op = cider::IROp::MEMCPY;
        mc.args = {cider::IRValue::Register("buf", cider::IRType::PTR),
                   cider::IRValue::Constant(0, cider::IRType::PTR),
                   cider::IRValue::Constant(4, cider::IRType::I32)};
        e->addInstruction(mc);
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
            CHECK(err.message.find("null pointer dereference") != std::string::npos);
        }
        CHECK(threw);  // before the fix: silent read from heap offset 0
    }
    {
        // A zero-length MEMCPY through nil stays the conventional no-op.
        auto fn = makeFn("main", cider::IRType::I32);
        auto e = std::make_unique<cider::BasicBlock>("entry");
        cider::IRInstruction mc;
        mc.op = cider::IROp::MEMCPY;
        mc.args = {cider::IRValue::Constant(0, cider::IRType::PTR),
                   cider::IRValue::Constant(0, cider::IRType::PTR),
                   cider::IRValue::Constant(0, cider::IRType::I32)};
        e->addInstruction(mc);
        cider::IRInstruction ret;
        ret.op = cider::IROp::RET;
        ret.args = {cider::IRValue::Constant(0, cider::IRType::I32)};
        e->addInstruction(ret);
        fn->addBlock(std::move(e));

        cider::ModuleIR mod;
        mod.addFunction(std::move(fn));
        cider::VM vm;
        CHECK(vm.run(mod).exitCode == 0);
    }

    // CIDER-BUG-211: ir-gen's interface-dispatch guard calls
    // `_nil_dispatch_fail` when the receiver's vtable word is zero. The VM
    // must answer with the same message the native helper writes, so both
    // backends report `vm error: nil interface dispatch` and exit 2. This
    // used to surface only as load's CIDER-BUG-059 "load: null pointer
    // dereference", naming the wrong fault entirely.
    {
        auto fn = makeFn("main", cider::IRType::I32);
        auto e = std::make_unique<cider::BasicBlock>("entry");
        cider::IRInstruction boom;
        boom.op = cider::IROp::CALL;
        boom.args = {cider::IRValue::Label("_nil_dispatch_fail")};
        e->addInstruction(boom);
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
            // Exact text: run_tests.py pins the same string byte for byte
            // against the native helper's `db "vm error: nil interface
            // dispatch"` payload.
            CHECK(err.message == "nil interface dispatch");
        }
        CHECK(threw);
    }
    {
        // The guard is a builtin, so `cider run` must recognise the label
        // instead of failing with an unresolved-label error. This mirrors
        // the shape of the branch ir-gen emits: a nil comparison whose fail
        // edge calls the builtin and rejoins the live path. The live path
        // must still run, so the guard costs a taken branch and nothing
        // else when the receiver is not nil.
        auto fn = makeFn("main", cider::IRType::I32);
        auto e = std::make_unique<cider::BasicBlock>("entry");
        e->addInstruction(opInstr(cider::IROp::ALLOCA, "vt",
                                  {cider::IRValue::Constant(8, cider::IRType::I32)}));
        cider::IRInstruction st;
        st.op = cider::IROp::STORE;
        st.dest = std::nullopt;
        st.args = {cider::IRValue::Register("vt", cider::IRType::PTR),
                   cider::IRValue::Constant(0x1234, cider::IRType::I64),
                   cider::IRValue::Constant(0, cider::IRType::I32)};
        e->addInstruction(st);
        cider::IRInstruction ld;
        ld.op = cider::IROp::LOAD;
        ld.dest = cider::IRValue::Register("v", cider::IRType::PTR);
        ld.args = {cider::IRValue::Register("vt", cider::IRType::PTR),
                   cider::IRValue::Constant(0, cider::IRType::I32)};
        e->addInstruction(ld);
        cider::IRInstruction cmp;
        cmp.op = cider::IROp::CMP_EQ;
        cmp.dest = cider::IRValue::Register("isnil", cider::IRType::BOOL);
        cmp.args = {cider::IRValue::Register("v", cider::IRType::PTR),
                    cider::IRValue::Constant(0, cider::IRType::I64)};
        e->addInstruction(cmp);
        e->addInstruction({cider::IROp::COND_BR, std::nullopt,
                           {cider::IRValue::Register("isnil", cider::IRType::BOOL),
                            cider::IRValue::Label("fail"),
                            cider::IRValue::Label("ok")}});
        fn->addBlock(std::move(e));
        auto fail = std::make_unique<cider::BasicBlock>("fail");
        cider::IRInstruction boom;
        boom.op = cider::IROp::CALL;
        boom.args = {cider::IRValue::Label("_nil_dispatch_fail")};
        fail->addInstruction(boom);
        fail->addInstruction({cider::IROp::BR, std::nullopt, {cider::IRValue::Label("ok")}});
        fn->addBlock(std::move(fail));
        auto ok = std::make_unique<cider::BasicBlock>("ok");
        cider::IRInstruction ret;
        ret.op = cider::IROp::RET;
        ret.args = {cider::IRValue::Constant(0, cider::IRType::I32)};
        ok->addInstruction(ret);
        fn->addBlock(std::move(ok));
        cider::ModuleIR mod;
        mod.addFunction(std::move(fn));
        cider::VM vm;
        CHECK(vm.run(mod).exitCode == 0);
    }

    // CIDER-BUG-219: ir-gen's nil-pointer guard calls `_nil_deref_fail` when a
    // source-level load, store, copy or method call is about to go through a
    // nil pointer. The VM must answer with the same message the native helper
    // writes, so both backends report `vm error: null pointer dereference` and
    // exit 2. Without the guard this surfaced only through load's
    // CIDER-BUG-059 exact-zero test, as `load:`/`store:`/`memcpy: null pointer
    // dereference` — and not at all for `p[1]` through a nil `p`, whose
    // effective address 8 the exact-zero test cannot see.
    {
        auto fn = makeFn("main", cider::IRType::I32);
        auto e = std::make_unique<cider::BasicBlock>("entry");
        cider::IRInstruction boom;
        boom.op = cider::IROp::CALL;
        boom.args = {cider::IRValue::Label("_nil_deref_fail")};
        e->addInstruction(boom);
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
            // Exact text: run_tests.py pins the same string byte for byte
            // against the native helper's `db "vm error: null pointer
            // dereference"` payload, and the smoke test pins `mov rdx, 35`
            // (its length) alongside it.
            CHECK(err.message == "null pointer dereference");
        }
        CHECK(threw);
    }
    {
        // The guard is a builtin, so `cider run` must recognise the label
        // instead of failing with an unresolved-label error. This mirrors the
        // three-block shape ir-gen emits: a nil comparison on the POINTER
        // (before any address arithmetic) whose fail edge calls the builtin
        // and rejoins the live path, with the dereference itself in that
        // rejoined block. The live path must still run, so the guard costs a
        // not-taken branch and nothing else when the pointer is not nil.
        auto fn = makeFn("main", cider::IRType::I32);
        auto e = std::make_unique<cider::BasicBlock>("entry");
        // `alloca` yields the slot's address, so %slot IS the pointer: a real,
        // non-nil heap address, never the zero address nilPtr is.
        e->addInstruction(opInstr(cider::IROp::ALLOCA, "slot",
                                  {cider::IRValue::Constant(8, cider::IRType::I32)}));
        cider::IRInstruction st;
        st.op = cider::IROp::STORE;
        st.dest = std::nullopt;
        st.args = {cider::IRValue::Register("slot", cider::IRType::PTR),
                   cider::IRValue::Constant(0x1234, cider::IRType::I64),
                   cider::IRValue::Constant(0, cider::IRType::I32)};
        e->addInstruction(st);
        cider::IRInstruction cmp;
        cmp.op = cider::IROp::CMP_EQ;
        cmp.dest = cider::IRValue::Register("isnil", cider::IRType::BOOL);
        cmp.args = {cider::IRValue::Register("slot", cider::IRType::PTR),
                    cider::IRValue::Constant(0, cider::IRType::I64)};
        e->addInstruction(cmp);
        e->addInstruction({cider::IROp::COND_BR, std::nullopt,
                           {cider::IRValue::Register("isnil", cider::IRType::BOOL),
                            cider::IRValue::Label("fail"),
                            cider::IRValue::Label("ok")}});
        fn->addBlock(std::move(e));
        auto fail = std::make_unique<cider::BasicBlock>("fail");
        cider::IRInstruction boom;
        boom.op = cider::IROp::CALL;
        boom.args = {cider::IRValue::Label("_nil_deref_fail")};
        fail->addInstruction(boom);
        fail->addInstruction({cider::IROp::BR, std::nullopt, {cider::IRValue::Label("ok")}});
        fn->addBlock(std::move(fail));
        auto ok = std::make_unique<cider::BasicBlock>("ok");
        // The dereference the guard protects, in the block both edges rejoin.
        cider::IRInstruction ld2;
        ld2.op = cider::IROp::LOAD;
        ld2.dest = cider::IRValue::Register("val", cider::IRType::I64);
        ld2.args = {cider::IRValue::Register("slot", cider::IRType::PTR),
                    cider::IRValue::Constant(0, cider::IRType::I32)};
        ok->addInstruction(ld2);
        cider::IRInstruction ret;
        ret.op = cider::IROp::RET;
        // Returning the guarded load's own result proves both that the live
        // path ran and that the guard did not clobber the pointer.
        ret.args = {cider::IRValue::Register("val", cider::IRType::I64)};
        ok->addInstruction(ret);
        fn->addBlock(std::move(ok));
        cider::ModuleIR mod;
        mod.addFunction(std::move(fn));
        cider::VM vm;
        CHECK(vm.run(mod).exitCode == 0x1234);
    }

    // CIDER-BUG-225: ir-gen's nil function-pointer guard calls
    // `_nil_fnptr_fail` when an indirect call is about to ENTER a nil
    // `fn(...)` value. The VM must answer with the same message the native
    // helper writes, so both backends report `vm error: call: nil function
    // pointer` and exit 2. This is its OWN pseudo-builtin, NOT CIDER-BUG-219's
    // `_nil_deref_fail`: a nil function pointer is entered, not dereferenced,
    // so it keeps the `call:` text doCall already used for the same value.
    {
        auto fn = makeFn("main", cider::IRType::I32);
        auto e = std::make_unique<cider::BasicBlock>("entry");
        cider::IRInstruction boom;
        boom.op = cider::IROp::CALL;
        boom.args = {cider::IRValue::Label("_nil_fnptr_fail")};
        e->addInstruction(boom);
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
            // Exact text: run_tests.py pins the same string byte for byte
            // against the native helper's `db "vm error: call: nil function
            // pointer"` payload, and the smoke test pins `mov rdx, 37` (its
            // length) alongside it.
            CHECK(err.message == "call: nil function pointer");
        }
        CHECK(threw);
    }
    {
        // The guard is a builtin, so `cider run` must recognise the label
        // instead of failing with an unresolved-label error. This mirrors the
        // three-block shape ir-gen emits: a nil comparison on the CALLEE
        // value (the loaded slot, immediately before the CALL) whose fail edge
        // calls the builtin and rejoins the live path, with the indirect call
        // itself in that rejoined block. The live path must still run, so the
        // guard costs a not-taken branch and nothing else when the callee is
        // not nil.
        auto callee = makeFn("callee", cider::IRType::I32);
        auto ce = std::make_unique<cider::BasicBlock>("entry");
        cider::IRInstruction cret;
        cret.op = cider::IROp::RET;
        cret.args = {cider::IRValue::Constant(7, cider::IRType::I32)};
        ce->addInstruction(cret);
        callee->addBlock(std::move(ce));

        auto fn = makeFn("main", cider::IRType::I32);
        auto e = std::make_unique<cider::BasicBlock>("entry");
        e->addInstruction(opInstr(cider::IROp::ALLOCA, "slot",
                                  {cider::IRValue::Constant(8, cider::IRType::I32)}));
        // A non-nil callee: the function-value id of `callee`, which is 1
        // because module function values are 1-based and 0 is nilPtr.
        cider::IRInstruction st;
        st.op = cider::IROp::STORE;
        st.dest = std::nullopt;
        st.args = {cider::IRValue::Register("slot", cider::IRType::PTR),
                   cider::IRValue::Constant(1, cider::IRType::I64),
                   cider::IRValue::Constant(0, cider::IRType::I32)};
        e->addInstruction(st);
        cider::IRInstruction ld;
        ld.op = cider::IROp::LOAD;
        ld.dest = cider::IRValue::Register("fp", cider::IRType::PTR);
        ld.args = {cider::IRValue::Register("slot", cider::IRType::PTR),
                   cider::IRValue::Constant(0, cider::IRType::I32)};
        e->addInstruction(ld);
        cider::IRInstruction cmp;
        cmp.op = cider::IROp::CMP_EQ;
        cmp.dest = cider::IRValue::Register("isnil", cider::IRType::BOOL);
        cmp.args = {cider::IRValue::Register("fp", cider::IRType::PTR),
                    cider::IRValue::Constant(0, cider::IRType::I64)};
        e->addInstruction(cmp);
        e->addInstruction({cider::IROp::COND_BR, std::nullopt,
                           {cider::IRValue::Register("isnil", cider::IRType::BOOL),
                            cider::IRValue::Label("fail"),
                            cider::IRValue::Label("ok")}});
        fn->addBlock(std::move(e));
        auto fail = std::make_unique<cider::BasicBlock>("fail");
        cider::IRInstruction boom;
        boom.op = cider::IROp::CALL;
        boom.args = {cider::IRValue::Label("_nil_fnptr_fail")};
        fail->addInstruction(boom);
        fail->addInstruction({cider::IROp::BR, std::nullopt, {cider::IRValue::Label("ok")}});
        fn->addBlock(std::move(fail));
        auto ok = std::make_unique<cider::BasicBlock>("ok");
        // The indirect call the guard protects, in the block both edges rejoin.
        cider::IRInstruction call;
        call.op = cider::IROp::CALL;
        call.dest = cider::IRValue::Register("r", cider::IRType::I32);
        call.args = {cider::IRValue::Register("fp", cider::IRType::PTR)};
        ok->addInstruction(call);
        cider::IRInstruction ret;
        ret.op = cider::IROp::RET;
        // Returning the callee's own result proves both that the live path ran
        // and that the guard did not clobber the callee.
        ret.args = {cider::IRValue::Register("r", cider::IRType::I32)};
        ok->addInstruction(ret);
        fn->addBlock(std::move(ok));
        cider::ModuleIR mod;
        mod.addFunction(std::move(callee));
        mod.addFunction(std::move(fn));
        cider::VM vm;
        CHECK(vm.run(mod).exitCode == 7);
    }
    {
        // doCall's own `fid == 0` check is the IR-level safety net that stays
        // behind the guard, for a zero callee formed without one. It must keep
        // reporting the same message the builtin does, so the diagnostic does
        // not depend on which of the two caught it.
        auto fn = makeFn("main", cider::IRType::I32);
        auto e = std::make_unique<cider::BasicBlock>("entry");
        e->addInstruction(opInstr(cider::IROp::ALLOCA, "slot",
                                  {cider::IRValue::Constant(8, cider::IRType::I32)}));
        cider::IRInstruction st;
        st.op = cider::IROp::STORE;
        st.dest = std::nullopt;
        st.args = {cider::IRValue::Register("slot", cider::IRType::PTR),
                   cider::IRValue::Constant(0, cider::IRType::I64),
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
        call.args = {cider::IRValue::Register("fp", cider::IRType::PTR)};
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
            CHECK(err.message == "call: nil function pointer");
        }
        CHECK(threw);
    }
    {
        // The three guards stay distinguishable: CIDER-BUG-219's
        // `_nil_deref_fail` must not answer with the `call:` text, and this
        // ticket's `_nil_fnptr_fail` must not answer with the dereference one.
        // If either borrowed the other's message, a program could no longer
        // tell a failed call from a failed load.
        for (const char* name : {"_nil_deref_fail", "_nil_fnptr_fail",
                                 "_nil_dispatch_fail"}) {
            auto fn = makeFn("main", cider::IRType::I32);
            auto e = std::make_unique<cider::BasicBlock>("entry");
            cider::IRInstruction boom;
            boom.op = cider::IROp::CALL;
            boom.args = {cider::IRValue::Label(name)};
            e->addInstruction(boom);
            cider::IRInstruction ret;
            ret.op = cider::IROp::RET;
            ret.args = {cider::IRValue::Constant(0, cider::IRType::I32)};
            e->addInstruction(ret);
            fn->addBlock(std::move(e));
            cider::ModuleIR mod;
            mod.addFunction(std::move(fn));
            cider::VM vm;
            bool threw = false;
            std::string msg;
            try {
                vm.run(mod);
            } catch (const cider::VMError& err) {
                threw = true;
                msg = err.message;
            }
            CHECK(threw);
            const std::string want = std::string(name) == "_nil_deref_fail"
                                         ? "null pointer dereference"
                                     : std::string(name) == "_nil_fnptr_fail"
                                         ? "call: nil function pointer"
                                         : "nil interface dispatch";
            CHECK(msg == want);
        }
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
    // CIDER-BUG-121: an allocation failure inside the interpreter must
    // surface as VMError through run(), never as an escaping std::bad_alloc
    // (which the CLI's generic handler reports as a compiler failure, exit
    // 1). A 2^62-byte ALLOCA cannot be satisfied by the host, so VM::malloc's
    // heap growth throws; run() must translate it.
    {
        auto fn = makeFn("main", cider::IRType::I32);
        auto e = std::make_unique<cider::BasicBlock>("entry");
        e->addInstruction(opInstr(cider::IROp::ALLOCA, "huge",
                                  {cider::IRValue::Constant(INT64_C(4611686018427387904),
                                                            cider::IRType::I64)}));
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
            CHECK(err.message == "out of memory");
        } catch (const std::bad_alloc&) {
            CHECK(false);  // before the fix: escaped run() as bad_alloc
        }
        CHECK(threw);
    }

    // CIDER-BUG-121: VM::malloc's negative-size failure (CIDER-BUG-110) is
    // the allocator's own std::bad_alloc signal; it too must reach callers
    // as a clean VMError once it escapes run().
    {
        auto fn = makeFn("main", cider::IRType::I32);
        auto e = std::make_unique<cider::BasicBlock>("entry");
        e->addInstruction(opInstr(cider::IROp::ALLOCA, "neg",
                                  {cider::IRValue::Constant(-1, cider::IRType::I64)}));
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
            CHECK(err.message == "out of memory");
        } catch (const std::bad_alloc&) {
            CHECK(false);  // before the fix: escaped run() as bad_alloc
        }
        CHECK(threw);
    }

    // CIDER-BUG-171: putchar's buffer is per-run state. A reused VM instance
    // must not re-emit a previous run's bytes, no matter how that run ended:
    // a normal RET, the exit builtin, falling off the entry block (which
    // never reaches a flush and leaves bytes buffered), or a thrown VMError.
    {
        auto putcCall = [](int c) {
            cider::IRInstruction in;
            in.op = cider::IROp::CALL;
            in.args = {cider::IRValue::Label("putchar"),
                       cider::IRValue::Constant(c, cider::IRType::I32)};
            return in;
        };
        // main() emits "A\n"; endKind 0 = RET, 1 = exit builtin, 2 = fall-off.
        auto buildMain = [&](int endKind) {
            auto fn = makeFn("main", cider::IRType::I32);
            auto e = std::make_unique<cider::BasicBlock>("entry");
            e->addInstruction(putcCall('A'));
            e->addInstruction(putcCall('\n'));
            if (endKind == 0) {
                cider::IRInstruction ret;
                ret.op = cider::IROp::RET;
                ret.args = {cider::IRValue::Constant(0, cider::IRType::I32)};
                e->addInstruction(ret);
            } else if (endKind == 1) {
                cider::IRInstruction ex;
                ex.op = cider::IROp::CALL;
                ex.args = {cider::IRValue::Label("exit"),
                           cider::IRValue::Constant(9, cider::IRType::I32)};
                e->addInstruction(ex);
            }
            fn->addBlock(std::move(e));
            cider::ModuleIR mod;
            mod.addFunction(std::move(fn));
            return mod;
        };
        // RET ending: both runs report their own bytes only.
        {
            auto mod = buildMain(0);
            cider::VM vm;
            CHECK(vm.run(mod).stdoutText == "A\n");
            auto r2 = vm.run(mod);
            CHECK(r2.exitCode == 0);
            CHECK(r2.stdoutText == "A\n");
        }
        // exit builtin ending: same independence.
        {
            auto mod = buildMain(1);
            cider::VM vm;
            CHECK(vm.run(mod).exitCode == 9);
            auto r2 = vm.run(mod);
            CHECK(r2.exitCode == 9);
            CHECK(r2.stdoutText == "A\n");
        }
        // Fall-off ending buffers bytes without flushing; the next run must
        // start clean (a RET run must not emit the fall-off run's bytes).
        {
            auto modOff = buildMain(2);
            auto modRet = buildMain(0);
            cider::VM vm;
            CHECK(vm.run(modOff).stdoutText.empty());
            auto r2 = vm.run(modRet);
            CHECK(r2.exitCode == 0);
            CHECK(r2.stdoutText == "A\n");
        }
        // A run that throws before any flush leaves bytes buffered; the next
        // run must still start clean.
        {
            auto fn = makeFn("main", cider::IRType::I32);
            auto e = std::make_unique<cider::BasicBlock>("entry");
            e->addInstruction(putcCall('X'));
            cider::IRInstruction boom;
            boom.op = cider::IROp::CALL;
            boom.args = {cider::IRValue::Label("_uncaught_exception")};
            e->addInstruction(boom);
            fn->addBlock(std::move(e));
            cider::ModuleIR mod;
            mod.addFunction(std::move(fn));
            cider::VM vm;
            bool threw = false;
            try {
                vm.run(mod);
            } catch (const cider::VMError&) {
                threw = true;
            }
            CHECK(threw);
            auto r2 = vm.run(buildMain(0));
            CHECK(r2.exitCode == 0);
            CHECK(r2.stdoutText == "A\n");
        }
    }

    // CIDER-BUG-248: a global (LABEL-base) STORE writes at the width of the
    // STORE instruction, whatever the address spelling. The VM forced I64
    // for every LABEL base ("global cells are 8 bytes"), which is wrong in
    // both directions: a global cell is the declared type's width in
    // native's .bss (`resb totalBytes`, Issue #20/#11) and can be WIDER
    // than 8 bytes (an interface's 16-byte fat pointer, a 2-word struct),
    // so forcing 8 bytes truncated a wide aggregate store to its first
    // word and let a narrow store write past the cell native reserves as
    // `resb 1`. ir_gen casts the right-hand side to the target's declared
    // type before storing, so the instruction width IS the global's
    // declared width — the rule native applies (codegen.cpp STORE,
    // `globalTypes`) and the one the LOAD handler follows (CIDER-BUG-245).
    //
    // Poisoning a cell first is what makes the width observable at all: a
    // narrow store must leave the bytes it does not cover alone. Before the
    // fix every store below wrote 8 bytes, so checks 1, 2 and 3 read back a
    // zero-extended low word with the poisoned high bytes overwritten. The
    // f32/f64 cells guard the CID-029 float reinterpretation, which this
    // path still honors.
    {
        auto fn = makeFn("main", cider::IRType::I32);
        std::vector<std::unique_ptr<cider::BasicBlock>> blocks;
        auto block = [&](const std::string& name) -> cider::BasicBlock* {
            blocks.push_back(std::make_unique<cider::BasicBlock>(name));
            return blocks.back().get();
        };
        cider::BasicBlock* cur = block("entry");
        // STORE (label, value) and STORE (label, value, offset).
        auto storeTo = [&](const std::string& label, cider::IRValue val, int64_t off) {
            cider::IRInstruction st;
            st.op = cider::IROp::STORE;
            st.args = {cider::IRValue::Label(label), val};
            if (off != 0) st.args.push_back(cider::IRValue::Constant(off, cider::IRType::I32));
            cur->addInstruction(st);
        };
        // LOAD (label, offset) at `width`, compared against `expect`. A
        // mismatch returns the check number, so a failure names the rule
        // that moved.
        auto check = [&](int64_t code, const std::string& label, int64_t off,
                         cider::IRType width, cider::IRValue expect) {
            std::string dst = "v" + std::to_string(code);
            std::string cond = "c" + std::to_string(code);
            std::string next = "s" + std::to_string(code + 1);
            cider::IRInstruction ld;
            ld.op = cider::IROp::LOAD;
            ld.dest = cider::IRValue::Register(dst, width);
            ld.args = {cider::IRValue::Label(label)};
            if (off != 0) ld.args.push_back(cider::IRValue::Constant(off, cider::IRType::I32));
            cur->addInstruction(ld);
            cur->addInstruction(opInstr(cider::IROp::CMP_EQ, cond,
                {cider::IRValue::Register(dst, width), expect}));
            cur->addInstruction(opInstr(cider::IROp::COND_BR, "",
                {cider::IRValue::Register(cond, cider::IRType::I32),
                 cider::IRValue::Label(next),
                 cider::IRValue::Label("fail" + std::to_string(code))}));
            cur = block(next);
        };
        auto retCode = [&](const std::string& name, int64_t code) {
            cider::IRInstruction ret;
            ret.op = cider::IROp::RET;
            ret.args = {cider::IRValue::Constant(code, cider::IRType::I32)};
            block(name)->addInstruction(ret);
        };

        // A 1-byte global (`resb 1` in native, an 8-byte VM cell), a word
        // inside a 16-byte global (the interface/struct shape), and the two
        // float cell widths.
        const uint64_t poison = 0x1122334455667788ULL;
        storeTo("gCell", cider::IRValue::Constant(poison, cider::IRType::I64), 0);
        storeTo("gCell", cider::IRValue::Constant(uint64_t(0xAABBCCDD), cider::IRType::I32), 0);
        check(1, "gCell", 0, cider::IRType::I64, cider::IRValue::Constant(uint64_t(0x11223344AABBCCDD), cider::IRType::I64));
        storeTo("gCell", cider::IRValue::Constant(0x99, cider::IRType::U8), 0);
        check(2, "gCell", 0, cider::IRType::I64, cider::IRValue::Constant(uint64_t(0x11223344AABBCC99), cider::IRType::I64));

        storeTo("gWide", cider::IRValue::Constant(poison, cider::IRType::I64), 0);
        storeTo("gWide", cider::IRValue::Constant(uint64_t(0xFFFFFFFFFFFFFFFFULL), cider::IRType::I64), 12);
        storeTo("gWide", cider::IRValue::Constant(0x01020304, cider::IRType::I32), 8);
        // The 4-byte store into the second word must not touch the third.
        check(3, "gWide", 8, cider::IRType::I64, cider::IRValue::Constant(uint64_t(0xFFFFFFFF01020304), cider::IRType::I64));
        check(4, "gWide", 0, cider::IRType::I64, cider::IRValue::Constant(poison, cider::IRType::I64));

        // A float cell keeps its declared width and its IEEE-754 bits
        // (CID-029). The comparison is a float compare, because loading a
        // float global reinterprets the cell through the declared type.
        storeTo("gF64", cider::IRValue::Constant(3.5, cider::IRType::F64), 0);
        check(5, "gF64", 0, cider::IRType::F64, cider::IRValue::Constant(3.5, cider::IRType::F64));
        storeTo("gF32", cider::IRValue::Constant(0.25, cider::IRType::F32), 0);
        check(6, "gF32", 0, cider::IRType::F32, cider::IRValue::Constant(0.25, cider::IRType::F32));

        retCode("s7", 0);
        for (int64_t i = 1; i <= 6; i++) retCode("fail" + std::to_string(i), i);
        for (auto& b : blocks) fn->addBlock(std::move(b));

        cider::ModuleIR mod;
        mod.addGlobalVar("gCell", cider::IRType::U8, 0, 0, 1);
        mod.addGlobalVar("gWide", cider::IRType::PTR, 0, 0, 16);
        mod.addGlobalVar("gF64", cider::IRType::F64, 0, 0, 8);
        mod.addGlobalVar("gF32", cider::IRType::F32, 0, 0, 4);
        mod.addFunction(std::move(fn));
        cider::VM vm;
        CHECK(vm.run(mod).exitCode == 0);
    }

    return 0;
}

int main() { return run(); }
