#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "ir.h"

namespace cider {

struct VMError {
    std::string message;
};

struct VMResult {
    int exitCode = 0;
    std::string stdoutText;
    std::string stderrText;
};

class VM {
public:
    VMResult run(const ModuleIR& module, const std::string& runFunction = "main");

    struct RegVal {
        bool isFloat = false;
        int64_t i = 0;
        double f = 0;
    };

private:
    struct Frame {
        const FunctionIR* fn = nullptr;
        size_t blockIdx = 0;
        size_t instrIdx = 0;
        std::string prevBlock;
        std::string pendingDest;  // register to receive the return value
        std::map<std::string, RegVal> savedRegs;  // caller's registers (restored on RET)
    };

    // Heap: byte vector; pointers are offsets.
    std::vector<uint8_t> heap_;

    // Free list of blocks: {offset, size} in heap coordinates.
    struct FreeBlock {
        int64_t offset;
        int64_t size;
    };
    std::vector<FreeBlock> freeList_;

    std::map<std::string, RegVal> regs_;
    std::vector<Frame> frames_;
    RegVal retReg_;
    const ModuleIR* module_ = nullptr;
    std::map<std::string, int64_t> functionIds_;   // label -> id (id = index into functions)
    std::map<std::string, int64_t> stringOffsets_; // materialized string constant -> heap offset
    std::map<std::string, int64_t> globalVarOffsets_; // global variable -> heap cell offset
    std::map<std::string, int64_t> vtableOffsets_; // vtable label -> heap offset
    std::string runFunction_;
    VMResult result_;
    int64_t steps_ = 0;

    static constexpr int64_t kStepLimit = 1000000000;  // fail-fast guard, not a work cap
    static constexpr int64_t kHeaderSize = 16;

    RegVal resolveValue(const IRValue& v);
    void materializeLabels();
    int64_t materializeString(const std::string& name);
    int64_t materializeVTable(const std::string& label);

    int64_t malloc(int64_t size);
    void free(int64_t offset);

    // Asm register file: persists across ASM instructions (the ir-gen
    // splits one asm block into several instructions).
    int64_t asmRax_ = 0, asmRdi_ = 0, asmRsi_ = 0, asmRdx_ = 0;
    int64_t asmR8_ = 0, asmR9_ = 0, asmR10_ = 0;

    RegVal load(int64_t addr, IRType type);
    void store(int64_t addr, const RegVal& val, IRType type, int64_t offset = 0);

    void execAsm(const IRInstruction& instr);
    void doCall(const IRValue& target, const std::vector<IRValue>& args);
    void doVirtualCall(const IRValue& obj, int64_t vtableIndex, const std::vector<IRValue>& args);
};

}  // namespace cider
