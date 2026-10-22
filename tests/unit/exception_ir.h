// exception_ir.h — small hand-built modules for the exception IR tests.
//
// The lowerer does not emit landing pads yet, so these tests build the two
// shapes directly: a landing that reads the payload, and a call whose landing
// pad is entered only after the callee raises the flag. Keeping the builders
// in one header makes the codegen and VM tests assert the same module.

#ifndef CIDER_TEST_EXCEPTION_IR_H
#define CIDER_TEST_EXCEPTION_IR_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ir/ir.h"
#include "types/context.h"
#include "types/signature.h"

namespace testir {

struct Built {
    std::unique_ptr<cider::TypeContext> types;
    cider::ir::Module module;
};

inline cider::ir::Value word(uint32_t id, cider::TypeRef type,
                             cider::ir::IRType width = cider::ir::IRType::I64) {
    cider::ir::Value value;
    value.id = id;
    value.width = width;
    value.type = type;
    return value;
}

inline void addExceptionGlobals(cider::ir::Module& module,
                                cider::TypeRef type) {
    auto add = [&](const std::string& label, int64_t initial) {
        cider::ir::Global global;
        global.label = label;
        global.type = type;
        global.initial = initial;
        module.globals.push_back(global);
    };
    add("cider_exc_flag", 0);
    add("cider_exc_type", 7);
    add("cider_exc_value", 42);
    module.usesExceptions = true;
    module.exceptionFlag = "cider_exc_flag";
    module.exceptionType = "cider_exc_type";
    module.exceptionValue = "cider_exc_value";
}

inline cider::ir::Function function(const std::string& name,
                                    const cider::Type* returnType,
                                    std::vector<cider::ir::Block> blocks) {
    cider::ir::Function fn;
    fn.name = name;
    fn.signature = cider::Signature({}, returnType, cider::Abi::SysV);
    fn.blocks = std::move(blocks);
    return fn;
}

// A module whose main either leaves an exception flagged (uncaught) or reads
// the payload through one `landing` and answers it.
inline Built landing(bool uncaught) {
    Built out;
    out.types = std::make_unique<cider::TypeContext>();
    const cider::Type* i64 = out.types->basic(cider::BasicKind::I64);
    const cider::TypeRef type{i64};
    addExceptionGlobals(out.module, type);
    out.module.name = "landing";

    cider::ir::Block body;
    body.label = 0;
    if (uncaught) {
        cider::ir::Instr set;
        set.op = cider::ir::Op::Const;
        set.width = cider::ir::IRType::I64;
        set.type = type;
        set.constantBits = 1;
        set.isPure = true;
        set.results.push_back(word(0, type));
        body.body.push_back(set);

        cider::ir::Instr store;
        store.op = cider::ir::Op::Store;
        store.width = cider::ir::IRType::I64;
        store.type = type;
        store.globalLabel = "cider_exc_flag";
        store.storedValue = word(0, type);
        body.body.push_back(store);

        cider::ir::Terminator ret;
        ret.term = cider::ir::Term::Ret;
        ret.values.push_back(word(0, type));
        body.term = ret;
    } else {
        cider::ir::Instr landingInstr;
        landingInstr.op = cider::ir::Op::Landing;
        landingInstr.width = cider::ir::IRType::I64;
        landingInstr.type = type;
        landingInstr.typeId = 7;
        landingInstr.results.push_back(word(0, type));
        body.body.push_back(landingInstr);

        cider::ir::Terminator ret;
        ret.term = cider::ir::Term::Ret;
        ret.values.push_back(word(0, type));
        body.term = ret;
    }

    out.module.functions.push_back(function("main", i64, {body}));
    return out;
}

// A module where raise() sets the flag and returns, and main's call carries
// landingPad 1. The landing reads the payload, clears the flag, and answers.
inline Built callLanding() {
    Built out;
    out.types = std::make_unique<cider::TypeContext>();
    const cider::Type* i64 = out.types->basic(cider::BasicKind::I64);
    const cider::TypeRef type{i64};
    addExceptionGlobals(out.module, type);
    out.module.name = "call-landing";

    cider::ir::Block callBlock;
    callBlock.label = 0;
    cider::ir::Instr call;
    call.op = cider::ir::Op::Call;
    call.width = cider::ir::IRType::I64;
    call.type = type;
    call.callee = "raise";
    call.mayTrap = true;
    call.call.landingPad = 1;
    call.results.push_back(word(0, type));
    callBlock.body.push_back(call);
    cider::ir::Terminator callRet;
    callRet.term = cider::ir::Term::Ret;
    callRet.values.push_back(word(0, type));
    callBlock.term = callRet;

    cider::ir::Block landingBlock;
    landingBlock.label = 1;
    cider::ir::Instr landingInstr;
    landingInstr.op = cider::ir::Op::Landing;
    landingInstr.width = cider::ir::IRType::I64;
    landingInstr.type = type;
    landingInstr.typeId = 7;
    landingInstr.results.push_back(word(1, type));
    landingBlock.body.push_back(landingInstr);

    cider::ir::Instr clear;
    clear.op = cider::ir::Op::Const;
    clear.width = cider::ir::IRType::I64;
    clear.type = type;
    clear.constantBits = 0;
    clear.isPure = true;
    clear.results.push_back(word(2, type));
    landingBlock.body.push_back(clear);

    cider::ir::Instr store;
    store.op = cider::ir::Op::Store;
    store.width = cider::ir::IRType::I64;
    store.type = type;
    store.globalLabel = "cider_exc_flag";
    store.storedValue = word(2, type);
    landingBlock.body.push_back(store);

    cider::ir::Terminator landingRet;
    landingRet.term = cider::ir::Term::Ret;
    landingRet.values.push_back(word(1, type));
    landingBlock.term = landingRet;

    cider::ir::Block raiseBlock;
    raiseBlock.label = 0;
    cider::ir::Instr set;
    set.op = cider::ir::Op::Const;
    set.width = cider::ir::IRType::I64;
    set.type = type;
    set.constantBits = 1;
    set.isPure = true;
    set.results.push_back(word(0, type));
    raiseBlock.body.push_back(set);

    cider::ir::Instr raise;
    raise.op = cider::ir::Op::Store;
    raise.width = cider::ir::IRType::I64;
    raise.type = type;
    raise.globalLabel = "cider_exc_flag";
    raise.storedValue = word(0, type);
    raiseBlock.body.push_back(raise);

    cider::ir::Instr zero;
    zero.op = cider::ir::Op::Const;
    zero.width = cider::ir::IRType::I64;
    zero.type = type;
    zero.constantBits = 0;
    zero.isPure = true;
    zero.results.push_back(word(1, type));
    raiseBlock.body.push_back(zero);

    cider::ir::Terminator raiseRet;
    raiseRet.term = cider::ir::Term::Ret;
    raiseRet.values.push_back(word(1, type));
    raiseBlock.term = raiseRet;

    out.module.functions.push_back(function("main", i64, {callBlock, landingBlock}));
    out.module.functions.push_back(function("raise", i64, {raiseBlock}));
    return out;
}

}  // namespace testir

#endif  // CIDER_TEST_EXCEPTION_IR_H
