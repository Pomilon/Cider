#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "reg_alloc.h"

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

cider::IRInstruction makeUse(const std::string& reg, const std::string& other) {
    cider::IRInstruction in;
    in.op = cider::IROp::ADD;
    in.dest = cider::IRValue::Register(other, cider::IRType::I32);
    in.args = {cider::IRValue::Register(reg, cider::IRType::I32),
               cider::IRValue::Constant(1, cider::IRType::I32)};
    return in;
}

int run() {
    // Two values in disjoint blocks: both fit in registers, no spills.
    auto fn = std::make_unique<cider::FunctionIR>("f", cider::IRType::VOID);
    auto b0 = std::make_unique<cider::BasicBlock>("b0");
    b0->addInstruction(makeUse("a", "t0"));
    cider::IRInstruction br;
    br.op = cider::IROp::BR;
    br.args = {cider::IRValue::Label("b3")};
    b0->addInstruction(br);
    auto b1 = std::make_unique<cider::BasicBlock>("b1");
    cider::IRInstruction br2;
    br2.op = cider::IROp::BR;
    br2.args = {cider::IRValue::Label("b3")};
    b1->addInstruction(br2);
    auto b2 = std::make_unique<cider::BasicBlock>("b2");
    b2->addInstruction(br2);
    auto b3 = std::make_unique<cider::BasicBlock>("b3");
    b3->addInstruction(makeUse("b", "t1"));
    fn->addBlock(std::move(b0));
    fn->addBlock(std::move(b1));
    fn->addBlock(std::move(b2));
    fn->addBlock(std::move(b3));

    cider::RegisterAllocator alloc;
    int stackSize = 0;
    auto result = alloc.allocate(*fn, stackSize);
    CHECK(stackSize == 0);  // a and b both in registers
    CHECK(!result.at("a").isSpilled);
    CHECK(!result.at("b").isSpilled);

    // Two waves of 14 simultaneously-live values each (each makeUse adds a
    // source AND a dest): wave 1 spills 9, wave 2 reuses the 9 freed slots
    // -> the stack stays at 72 instead of growing to 144.
    auto fn2 = std::make_unique<cider::FunctionIR>("g", cider::IRType::VOID);
    auto e2 = std::make_unique<cider::BasicBlock>("entry");
    // wave 1: v0..v6 + w0..w6 all live together in entry
    for (int i = 0; i < 7; ++i) {
        e2->addInstruction(makeUse("v" + std::to_string(i), "w" + std::to_string(i)));
    }
    cider::IRInstruction brA;
    brA.op = cider::IROp::BR;
    brA.args = {cider::IRValue::Label("mid")};
    e2->addInstruction(brA);
    auto mid = std::make_unique<cider::BasicBlock>("mid");
    // wave 2: u0..u6 + x0..x6 live together in a later block (v/w dead here)
    for (int i = 0; i < 7; ++i) {
        mid->addInstruction(makeUse("u" + std::to_string(i), "x" + std::to_string(i)));
    }
    fn2->addBlock(std::move(e2));
    fn2->addBlock(std::move(mid));

    cider::RegisterAllocator alloc2;
    int stackSize2 = 0;
    auto result2 = alloc2.allocate(*fn2, stackSize2);
    CHECK(stackSize2 == 72);  // 9 spill slots, reused across the waves
    int spills = 0;
    for (const auto& [name, res] : result2) {
        if (res.isSpilled) spills++;
    }
    CHECK(spills == 18);  // 9 per wave, sharing the same 9 slots
    return 0;
}

int main() { return run(); }
