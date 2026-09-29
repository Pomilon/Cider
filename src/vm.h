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
    // CIDER-CHORE-002: the step cap is a fail-fast guard, not a work cap.
    // Tests that pin the exhaustion path can lower it instead of burning
    // the default 1e9 steps (~170-240 s).
    void setStepLimit(int64_t limit) { stepLimit_ = limit; }
    bool debug_ = false;

    // CIDER-BUG-149: read-only allocation accounting for unit tests. A dead
    // frame's oversized-local fallbacks must return to the free list, so
    // repeated calls do not grow heapBytes(). After a completed run in which
    // the program frees everything it allocated, every heap byte sits on the
    // free list, i.e. freeListBytes() == heapBytes().
    size_t heapBytes() const { return heap_.size(); }
    size_t freeListBytes() const {
        size_t total = 0;
        for (const FreeBlock& b : freeList_) total += (size_t)b.size;
        return total;
    }

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
        int64_t localBase = 0;    // frame-local region for ALLOCAs (bump)
        size_t localSize = 0;
        size_t localUsed = 0;
        // CIDER-BUG-149: oversized ALLOCAs that fell back to VM::malloc
        // (the 4096-byte bump region could not hold them). Frame storage
        // like localBase: retireFrame releases each block on frame death.
        std::vector<int64_t> localHeapAllocs{};
    };

    // State captured by _setjmp for longjmp to restore.
    struct JumpState {
        int frameIndex = 0;
        int blockIdx = 0;
        int instrIdx = 0;
        // CIDER-BUG-053: register that receives the _setjmp result. The
        // snapshot below is taken before the CALL handler writes it, so
        // longjmp must redeliver the jump value there itself.
        std::string dest;
        std::map<std::string, RegVal> regs;
    };

    // Heap: byte vector; pointers are offsets.
    std::vector<uint8_t> heap_;

    // Free list of blocks: {offset, size} in heap coordinates.
    struct FreeBlock {
        int64_t offset;
        int64_t size;
    };
    std::vector<FreeBlock> freeList_;

    std::string putcharBuf_;  // buffered putchar output (flushed at exit)
    std::map<std::string, RegVal> regs_;
    std::vector<Frame> frames_;
    std::map<int64_t, JumpState> jumpStates_;  // jmp_buf address -> saved state
    RegVal retReg_;
    const ModuleIR* module_ = nullptr;
    // label -> function VALUE id. CID-006: value ids are 1-based (module
    // index + 1) so that id 0 is reserved for nilPtr — otherwise the
    // module's first function would compare equal to nilPtr.
    std::map<std::string, int64_t> functionIds_;
    // CID-006: linker-provided extern functions have no IR body. A value
    // reference to one gets a stable negative id; indirect calls route
    // back to the builtin table by name. Never a module function index.
    std::map<std::string, int64_t> externValueIds_;   // name -> negative id
    std::map<int64_t, std::string> externValueNames_; // negative id -> name
    std::map<std::string, int64_t> stringOffsets_; // materialized string constant -> heap offset
    std::map<std::string, int64_t> globalVarOffsets_; // global variable -> heap cell offset
    // Declared type per global (CID-029): float globals hold IEEE-754
    // bits in their cell, so LOADs must reinterpret (F32/F64), not
    // read I64 and convert numerically.
    std::map<std::string, IRType> globalVarTypes_; // global variable -> declared type
    std::map<std::string, int64_t> vtableOffsets_; // vtable label -> heap offset
    std::string runFunction_;
    VMResult result_;
    int64_t steps_ = 0;

    static constexpr int64_t kStepLimit = 1000000000;  // fail-fast guard, not a work cap
    int64_t stepLimit_ = kStepLimit;
    static constexpr int64_t kHeaderSize = 16;

    // CIDER-BUG-121: the interpreter body. run() wraps it so that allocation
    // failures (std::bad_alloc / std::length_error from heap_/frame growth)
    // surface through the VMError channel instead of escaping as a host
    // exception that the CLI reports as a compiler failure.
    VMResult runBody(const ModuleIR& module, const std::string& runFunction);

    RegVal resolveValue(const IRValue& v);
    void materializeLabels();
    int64_t materializeString(const std::string& name);
    int64_t materializeWideString(const std::string& name);
    int64_t materializeVTable(const std::string& label);

    bool isExternFunctionName(const std::string& name) const;
    int64_t externValueId(const std::string& name);
    // CIDER-BUG-053: destName is the CALL destination register; _setjmp
    // records it so longjmp can redeliver the jump value after a callee
    // frame unwinds.
    bool tryBuiltinCall(const std::string& name, const std::vector<IRValue>& args,
                        const std::string& destName = "");

    int64_t malloc(int64_t size);
    void free(int64_t offset);

    // Asm register file: persists across ASM instructions (the ir-gen
    // splits one asm block into several instructions).
    int64_t asmRax_ = 0, asmRdi_ = 0, asmRsi_ = 0, asmRdx_ = 0;
    int64_t asmR8_ = 0, asmR9_ = 0, asmR10_ = 0, asmR11_ = 0;

    RegVal load(int64_t addr, IRType type);
    void store(int64_t addr, const RegVal& val, IRType type, int64_t offset = 0);

    void execAsm(const IRInstruction& instr);
    void doCall(const IRValue& target, const std::vector<IRValue>& args,
                const std::string& destName = "");
    void doVirtualCall(const IRValue& obj, int64_t vtableIndex, const std::vector<IRValue>& args);
    // CIDER-BUG-115: release a dead frame's ALLOCA region. The region's
    // bytes are zeroed when a later frame claims (and reuses) the chunk,
    // mirroring native's next call overwriting the dead stack slots.
    // CIDER-BUG-149: oversized-local heap fallbacks are frame storage too;
    // they are released here and zeroed on claim, like a claimed chunk.
    void retireFrame(const Frame& frame);
    // CIDER-BUG-159: the exit paths (exit builtin, asm exit syscall)
    // abandon every live frame at once; retire the lot through the same
    // release as RET/longjmp/fall-off before dropping the stack.
    void retireAllFrames();
};

}  // namespace cider
