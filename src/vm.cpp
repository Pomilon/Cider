#include "vm.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <new>
#include <sstream>
#include <stdexcept>
#include <fcntl.h>
#include <unistd.h>

namespace cider {

namespace {

double regToFloat(const VM::RegVal& v) { return v.isFloat ? v.f : (double)v.i; }

// CID-003 fix round 2: extend an integer value to 64 bits by its IR type,
// exactly as native widens narrow operands (movsx for I8/I16/I32, movzx for
// U8/U16/U32). Narrow registers may hold their raw low bits (e.g. an i8 AND
// leaves 0xFD); every widening use must normalize so the VM agrees with
// codegen's movsx/movzx.
int64_t extendIntByType(int64_t v, IRType t) {
    switch (t) {
    case IRType::I8: return (int64_t)(int8_t)v;
    case IRType::U8: return (int64_t)(uint8_t)v;
    case IRType::I16: return (int64_t)(int16_t)v;
    case IRType::U16: return (int64_t)(uint16_t)v;
    case IRType::I32: return (int64_t)(int32_t)v;
    case IRType::U32: return (int64_t)(uint32_t)v;
    default: return v;  // 64-bit types, PTR, BOOL unaffected
    }
}

// The stdlib's asm blocks emit raw Linux syscalls, whose failures are
// negative errno values; the libc wrappers the VM uses instead return -1
// and set errno. Translate so VM callers observe the same values native
// does (e.g. -EBADF rather than -1 for a write to a closed fd).
int64_t syscallResult(int64_t r) { return r < 0 ? -(int64_t)errno : r; }

// CID-003 follow-up (M5/W5): common comparison domain for mixed
// signed/unsigned integer operands, mirroring IRGenerator::mixedCmpCommon.
// Equal widths (or a wider unsigned operand) compare in the unsigned
// domain; a wider signed operand keeps the compare signed. Same-signedness
// pairs return VOID — the per-operand widening below is enough.
IRType mixedCmpCommon(IRType a, IRType b) {
    auto isUnsigned = [](IRType t) {
        return t == IRType::U8 || t == IRType::U16 || t == IRType::U32 ||
               t == IRType::U64;
    };
    auto widthOf = [](IRType t) -> int {
        if (t == IRType::I8 || t == IRType::U8) return 8;
        if (t == IRType::I16 || t == IRType::U16) return 16;
        if (t == IRType::I32 || t == IRType::U32 || t == IRType::BOOL) return 32;
        return 64;
    };
    if (a == b) return IRType::VOID;
    bool au = isUnsigned(a), bu = isUnsigned(b);
    if (au == bu) return IRType::VOID;
    IRType u = au ? a : b;
    IRType s = au ? b : a;
    return (widthOf(u) >= widthOf(s)) ? u : s;
}

// Unsigned for comparison purposes: codegen selects setcc (setb/seta/...)
// from the left operand's type, with PTR and BOOL counting as unsigned.
bool isUnsignedCmpType(IRType t) {
    return t == IRType::U8 || t == IRType::U16 || t == IRType::U32 ||
           t == IRType::U64 || t == IRType::PTR || t == IRType::BOOL;
}

}  // namespace

// CIDER-BUG-121: the interpreter performs raw host allocations everywhere
// (heap_ vector growth for frame-local chunks / oversized locals /
// materialized data, frames_ and register maps). Under host memory pressure
// those throw std::bad_alloc or std::length_error. The mmap syscall handler
// catches them locally to report -ENOMEM like native, but anywhere else they
// used to escape VM::run and the CLI's generic std::exception handler
// reported them as a compiler failure (exit 1). Translate every escaping
// allocation failure into the VM's single error convention so `run` reports
// `vm error: <message>` with the VM's exit code.
VMResult VM::run(const ModuleIR& module, const std::string& runFunction) {
    try {
        return runBody(module, runFunction);
    } catch (const std::bad_alloc&) {
        throw VMError{"out of memory"};
    } catch (const std::length_error&) {
        throw VMError{"out of memory"};
    }
}

VMResult VM::runBody(const ModuleIR& module, const std::string& runFunction) {
    module_ = &module;
    runFunction_ = runFunction;
    result_ = VMResult{};
    // CIDER-BUG-171: putchar's buffer is per-run state. Clear it with the
    // rest so a reused VM cannot re-emit a previous run's bytes — including
    // after an exit that never flushed (fall-off) or a thrown VMError.
    putcharBuf_.clear();
    regs_.clear();
    frames_.clear();
    jumpStates_.clear();
    freeList_.clear();
    stringOffsets_.clear();
    vtableOffsets_.clear();
    retReg_ = RegVal{};
    steps_ = 0;
    heap_.clear();
    heap_.reserve(1 << 20);

    materializeLabels();

    asmRax_ = 0;
    asmRdi_ = 0;
    asmRsi_ = 0;
    asmRdx_ = 0;
    asmR8_ = 0;
    asmR9_ = 0;
    asmR10_ = 0;
    asmR11_ = 0;

    const FunctionIR* entry = nullptr;
    for (const auto& fn : module_->getFunctions()) {
        if (fn->getName() == runFunction_) {
            entry = fn.get();
            break;
        }
    }
    if (!entry && runFunction_ == "main") {
        // Programs written for the bare-metal path use _start.
        for (const auto& fn : module_->getFunctions()) {
            if (fn->getName() == "_start") {
                entry = fn.get();
                break;
            }
        }
    }
    if (!entry) throw VMError{"no function named '" + runFunction + "'"};

    frames_.push_back(Frame{entry, 0, 0, "", "", {}});

    // The kernel-style entry (argc, argv) gets minimal values: the program
    // name counts, so argc is 1 (matching the native binary).
    if (entry->getName() == "_start") {
        const auto& params = entry->getParams();
        if (!params.empty() && params[0] == "arg_argc") {
            RegVal v;
            v.i = 1;
            regs_["arg_argc"] = v;
        }
        if (params.size() > 1 && params[1] == "arg_argv") {
            // CIDER-BUG-059: a process never starts with a nil argv. The old
            // zero placeholder was masked by LOAD(0) returning the first heap
            // block's header (garbage); with the nil guard it would trap, but
            // native argv is a real vector. Materialize a minimal one
            // (program name + NULL terminator) so argv[0] is a valid pointer
            // while a genuine nil deref still fails loudly.
            int64_t nameOff = materializeString("\"cider\"");
            int64_t argvOff = malloc(16);
            std::memcpy(heap_.data() + argvOff, &nameOff, 8);
            int64_t nullArg = 0;
            std::memcpy(heap_.data() + argvOff + 8, &nullArg, 8);
            RegVal v;
            v.i = argvOff;
            regs_["arg_argv"] = v;
        }
    }

    // Parameters arrive in arg_* registers (the ir-gen names them).
    while (!frames_.empty()) {
        Frame& frame = frames_.back();
        const auto& blocks = frame.fn->getBlocks();
        if (frame.blockIdx >= blocks.size()) {
            // Fell off the end: implicit void return.
            retireFrame(frame);
            frames_.pop_back();
            continue;
        }
        const auto& instrs = blocks[frame.blockIdx]->getInstructions();
        if (frame.instrIdx >= instrs.size()) {
            retireFrame(frame);
            frames_.pop_back();
            continue;
        }
        const IRInstruction& instr = instrs[frame.instrIdx];
        frame.instrIdx++;

        if (++steps_ > stepLimit_) throw VMError{"vm: step limit exceeded"};

        switch (instr.op) {
        case IROp::PHI: {
            RegVal v = RegVal{};
            auto it = instr.phiArgs.find(frame.prevBlock);
            if (it != instr.phiArgs.end()) {
                v = resolveValue(it->second);
            }
            if (instr.dest) regs_[instr.dest->name] = v;
            break;
        }
        case IROp::ADD:
        case IROp::SUB:
        case IROp::MUL:
        case IROp::DIV:
        case IROp::MOD:
        case IROp::BIT_AND:
        case IROp::BIT_OR:
        case IROp::BIT_XOR:
        case IROp::LSHIFT:
        case IROp::RSHIFT: {
            RegVal a = resolveValue(instr.args[0]);
            RegVal b = resolveValue(instr.args[1]);
            RegVal d;
            if (a.isFloat || b.isFloat) {
                double x = regToFloat(a), y = regToFloat(b);
                d.isFloat = true;
                // CID-003 follow-up (M1): an F32 operation rounds its result
                // through float, exactly like addss/subss/mulss/divss. The
                // plain double computation kept the extra precision:
                // `0.3f + 0.1f` stayed 0.30000001341104508 instead of
                // rounding to 0.4f, so the VM disagreed with native. The
                // result width is the destination's type (codegen selects
                // its instruction the same way). Double rounding through
                // binary64 is exact for binary32 +,-,*,/ (53 >= 2*24+2).
                IRType dt = instr.dest ? instr.dest->type
                                       : (instr.args.size() > 0 ? instr.args[0].type
                                                                : IRType::F64);
                auto roundResult = [&](double v) -> double {
                    return (dt == IRType::F32) ? (double)(float)v : v;
                };
                switch (instr.op) {
                case IROp::ADD: d.f = roundResult(x + y); break;
                case IROp::SUB: d.f = roundResult(x - y); break;
                case IROp::MUL: d.f = roundResult(x * y); break;
                case IROp::DIV: d.f = roundResult(x / y); break;
                default: throw VMError{"float op not supported"};
                }
            } else {
                int64_t x = a.i, y = b.i;
                // Shift counts mask to 6 bits (x86 semantics; also avoids
                // C++ UB for counts >= 64). Right shift is arithmetic for
                // signed values, logical for unsigned (the value's type).
                // Division follows C usual conversions: unsigned if the
                // destination or either operand is unsigned (CID-028).
                uint64_t ux = (uint64_t)x;
                uint64_t uy = (uint64_t)y;
                unsigned n = (unsigned)(y & 63);
                auto isUnsignedInt = [](IRType t) {
                    return t == IRType::U8 || t == IRType::U16 || t == IRType::U32 ||
                           t == IRType::U64 || t == IRType::PTR || t == IRType::BOOL;
                };
                bool srcUnsigned = false;
                if (instr.op == IROp::RSHIFT && instr.args.size() > 0) {
                    srcUnsigned = isUnsignedInt(instr.args[0].type);
                }
                bool divUnsigned = false;
                if (instr.op == IROp::DIV || instr.op == IROp::MOD) {
                    divUnsigned = (instr.dest && isUnsignedInt(instr.dest->type)) ||
                                  (instr.args.size() > 0 && isUnsignedInt(instr.args[0].type)) ||
                                  (instr.args.size() > 1 && isUnsignedInt(instr.args[1].type));
                }
                // CID-016: integer ALU results wrap at the destination width
                // (native uses 32-bit registers for int). Without narrowing
                // the VM accumulates full 64-bit sums, so an overflowing i32
                // add chain followed by % disagrees with native idiv.
                auto narrowInt = [](int64_t v, IRType t) -> int64_t {
                    switch (t) {
                    case IRType::I8: return (int64_t)(int8_t)v;
                    case IRType::U8: return (int64_t)(uint8_t)v;
                    case IRType::I16: return (int64_t)(int16_t)v;
                    case IRType::U16: return (int64_t)(uint16_t)v;
                    case IRType::I32: return (int64_t)(int32_t)v;
                    case IRType::U32: return (int64_t)(uint32_t)v;
                    default: return v;  // 64-bit types, PTR, BOOL unaffected
                    }
                };
                IRType dt = instr.dest ? instr.dest->type : IRType::I64;
                // CIDER-ISS-013: signed division overflow (INT_MIN / -1)
                // is defined as 0, extending the CIDER-BUG-058
                // division-by-zero policy (faulting integer division
                // yields 0; the const evaluator already folds these
                // literal cases to 0 with unsigned semantics). Raw C++
                // / and % on INT_MIN / -1 are undefined behavior - in
                // practice the 64-bit case traps like x86 idiv while a
                // narrower quotient silently exceeds the destination
                // width - and native idiv raises #DE, so the overflow
                // needs an explicit guard on both sides. Unsigned
                // division cannot overflow. The width follows the
                // destination type, matching codegen's path selection
                // (64-bit, 32-bit, or widened-narrow, whose range
                // cannot reach INT32_MIN).
                IRType divWidth = instr.dest ? instr.dest->type : IRType::I32;
                bool divOverflow = false;
                if (!divUnsigned && y != 0) {
                    if (divWidth == IRType::I64)
                        divOverflow = (x == INT64_MIN && y == -1);
                    else if (divWidth == IRType::I32)
                        divOverflow = ((int32_t)x == INT32_MIN && (int32_t)y == -1);
                }
                switch (instr.op) {
                case IROp::ADD: d.i = narrowInt((int64_t)(ux + uy), dt); break;
                case IROp::SUB: d.i = narrowInt((int64_t)(ux - uy), dt); break;
                case IROp::MUL: d.i = narrowInt((int64_t)(ux * uy), dt); break;
                case IROp::DIV: d.i = (y != 0 && !divOverflow) ? (divUnsigned ? (int64_t)(ux / uy) : x / y) : 0; break;
                case IROp::MOD: d.i = (y != 0 && !divOverflow) ? (divUnsigned ? (int64_t)(ux % uy) : x % y) : 0; break;
                case IROp::BIT_AND: d.i = x & y; break;
                case IROp::BIT_OR: d.i = x | y; break;
                case IROp::BIT_XOR: d.i = x ^ y; break;
                case IROp::LSHIFT: d.i = narrowInt((int64_t)(ux << n), dt); break;
                case IROp::RSHIFT: d.i = srcUnsigned ? (int64_t)(ux >> n) : (x >> n); break;
                default: break;
                }
            }
            if (instr.dest) regs_[instr.dest->name] = d;
            break;
        }
        case IROp::BIT_NOT: {
            RegVal a = resolveValue(instr.args[0]);
            RegVal d;
            d.i = ~a.i;
            if (instr.dest) regs_[instr.dest->name] = d;
            break;
        }
        case IROp::CMP_EQ:
        case IROp::CMP_NE:
        case IROp::CMP_LT:
        case IROp::CMP_GT:
        case IROp::CMP_LE:
        case IROp::CMP_GE: {
            RegVal a = resolveValue(instr.args[0]);
            RegVal b = resolveValue(instr.args[1]);
            bool r = false;
            if (a.isFloat || b.isFloat) {
                double x = regToFloat(a), y = regToFloat(b);
                switch (instr.op) {
                case IROp::CMP_EQ: r = x == y; break;
                case IROp::CMP_NE: r = x != y; break;
                case IROp::CMP_LT: r = x < y; break;
                case IROp::CMP_GT: r = x > y; break;
                case IROp::CMP_LE: r = x <= y; break;
                case IROp::CMP_GE: r = x >= y; break;
                default: break;
                }
            } else {
                // CID-003 fix round 2: widen each operand by its own IR type
                // first, exactly like codegen's widenCmpOp (movsx/movzx), so
                // an unnormalized narrow register compares by value.
                IRType ca = instr.args[0].type;
                IRType cb = instr.args[1].type;
                int64_t x = extendIntByType(a.i, ca);
                int64_t y = extendIntByType(b.i, cb);
                // CID-003 follow-up (W5): mixed signed/unsigned pairs and
                // unsigned pairs compare in the unsigned domain, mirroring
                // ir_gen's mixedCmpCommon (which converts both operands) and
                // codegen's setcc selection. Comparing i64(-1) < u64(1)
                // signed made the VM disagree with native.
                IRType common = mixedCmpCommon(ca, cb);
                bool uns;
                if (common != IRType::VOID) {
                    uns = isUnsignedCmpType(common) && common != IRType::PTR &&
                          common != IRType::BOOL;
                    if (uns) {
                        // C cast semantics: wrap the extended value to the
                        // common unsigned width before comparing.
                        uint64_t mask = (common == IRType::U8) ? 0xFFULL
                                      : (common == IRType::U16) ? 0xFFFFULL
                                      : (common == IRType::U32) ? 0xFFFFFFFFULL
                                                                : ~0ULL;
                        x = (int64_t)((uint64_t)x & mask);
                        y = (int64_t)((uint64_t)y & mask);
                    }
                } else {
                    uns = isUnsignedCmpType(ca);
                }
                if (uns) {
                    uint64_t ux = (uint64_t)x, uy = (uint64_t)y;
                    switch (instr.op) {
                    case IROp::CMP_EQ: r = ux == uy; break;
                    case IROp::CMP_NE: r = ux != uy; break;
                    case IROp::CMP_LT: r = ux < uy; break;
                    case IROp::CMP_GT: r = ux > uy; break;
                    case IROp::CMP_LE: r = ux <= uy; break;
                    case IROp::CMP_GE: r = ux >= uy; break;
                    default: break;
                    }
                } else {
                    switch (instr.op) {
                    case IROp::CMP_EQ: r = x == y; break;
                    case IROp::CMP_NE: r = x != y; break;
                    case IROp::CMP_LT: r = x < y; break;
                    case IROp::CMP_GT: r = x > y; break;
                    case IROp::CMP_LE: r = x <= y; break;
                    case IROp::CMP_GE: r = x >= y; break;
                    default: break;
                    }
                }
            }
            RegVal d;
            d.i = r ? 1 : 0;
            if (instr.dest) regs_[instr.dest->name] = d;
            break;
        }
        case IROp::LOAD: {
            RegVal ptr = resolveValue(instr.args[0]);
            if (debug_) fprintf(stderr, "VM load [%lld]\n", (long long)ptr.i);
            int64_t offset = 0;


            if (instr.args.size() > 1 && instr.args[1].kind != IRValue::LABEL) {
                offset = resolveValue(instr.args[1]).i;
            }
            IRType type = instr.dest ? instr.dest->type : IRType::I64;
            if (instr.args[0].kind == IRValue::LABEL) type = IRType::I64;  // global cells are 8 bytes
            if (instr.args[0].kind == IRValue::LABEL && offset == 0) {
                // Float global (CID-029): the cell holds IEEE-754 bits,
                // not a numeric integer. Reinterpret via the declared
                // type; other types keep the legacy I64 cell read.
                auto git = globalVarTypes_.find(instr.args[0].name);
                if (git != globalVarTypes_.end() &&
                    (git->second == IRType::F32 || git->second == IRType::F64)) {
                    type = git->second;
                }
            }
            RegVal d = load(ptr.i + offset, type);
            if (instr.dest) regs_[instr.dest->name] = d;
            break;
        }
        case IROp::STORE: {
            RegVal addr = resolveValue(instr.args[0]);
            RegVal val = resolveValue(instr.args[1]);
            if (debug_) fprintf(stderr, "VM store [%lld] = %lld\n", (long long)addr.i, (long long)val.i);

            int64_t offset = 0;
            if (instr.args.size() > 2 && instr.args[2].kind != IRValue::LABEL) {
                offset = resolveValue(instr.args[2]).i;
            }
            // The store width comes from the VALUE's IR type: bools and
            // 32-bit ints are 4 bytes — an 8-byte store would clobber the
            // neighbor in a packed array. (Global cells are 8 bytes.)
            IRType type = instr.args[1].type;
            if (type == IRType::BOOL) type = IRType::I32;
            if (instr.args[0].kind == IRValue::LABEL) {
                type = IRType::I64;  // global cells are 8 bytes
                if (offset == 0) {
                    // Float global (CID-029): the cell holds IEEE-754 bits
                    // of the declared width, so an F64 value must be stored
                    // as its 8-byte pattern (and an F32 value as 4 bytes).
                    // Forcing I64 wrote val.i, which is 0 for a float
                    // constant (it carries only .f). Mirrors the LOAD
                    // global-type lookup above.
                    auto git = globalVarTypes_.find(instr.args[0].name);
                    if (git != globalVarTypes_.end() &&
                        (git->second == IRType::F32 || git->second == IRType::F64)) {
                        type = git->second;
                    }
                }
            }
            store(addr.i + offset, val, type);
            break;
        }
        case IROp::ALLOCA: {
            RegVal size = resolveValue(instr.args[0]);
            RegVal d;
            // Frame-local bump allocation: a frame's ALLOCAs come from its
            // own region, released when the frame returns, so stack slots
            // never collide with another frame's (or reuse freed heap
            // blocks). Oversized locals fall back to the heap.
            Frame& af = frames_.back();
            constexpr size_t kLocalChunk = 4096;
            if (af.localBase == 0) {
                af.localBase = malloc(kLocalChunk);
                af.localSize = kLocalChunk;
                af.localUsed = 0;
                // CIDER-BUG-115: zero a freshly claimed frame region so a
                // chunk reused from a retired frame cannot serve its old
                // contents to a dangling data pointer. Mirrors native, where
                // the next call's frame overwrites the dead stack slots;
                // reading before any reuse still sees the old bytes, exactly
                // like native.
                if (af.localBase > 0 &&
                    af.localBase + (int64_t)kLocalChunk <= (int64_t)heap_.size()) {
                    std::memset(heap_.data() + af.localBase, 0, kLocalChunk);
                }
            }
            // CIDER-BUG-110: a negative size must not enter the bump path.
            // Casting it to size_t makes sz huge, and localUsed + sz can
            // wrap back below localSize, handing out a stale slot. Route it
            // to malloc, whose negative-size rule fails the allocation.
            size_t sz = (size_t)size.i;
            sz = (sz + 7) & ~(size_t)7;
            if (size.i >= 0 && af.localUsed + sz <= af.localSize) {
                d.i = af.localBase + (int64_t)af.localUsed;
                af.localUsed += sz;
            } else {
                // Oversized local: heap fallback. CIDER-BUG-149: the block
                // is frame storage like the bump chunk — retireFrame frees
                // it when the frame dies — and is zeroed on claim like a
                // claimed chunk (CIDER-BUG-115): a block recycled from a
                // dead frame must not serve the old object back to a
                // dangling data pointer, while a read before any reuse
                // still sees the old bytes exactly as for a dead native
                // stack slot.
                d.i = malloc(size.i);
                if (size.i > 0) std::memset(heap_.data() + d.i, 0, (size_t)size.i);
                af.localHeapAllocs.push_back({d.i, size.i > 0 ? size.i : 0});
            }
            if (instr.dest) regs_[instr.dest->name] = d;
            break;
        }
        case IROp::MOV: {
            RegVal d = resolveValue(instr.args[0]);
            // CID-003 fix round 2: mirror codegen's MOV contract — extend by
            // the SOURCE type (movsx for I8/I16/I32, movzx for U8/U16/U32)
            // so a narrow signed value reads back negative, not as its raw
            // unsigned bit pattern.
            if (!d.isFloat && instr.args.size() > 0) {
                d.i = extendIntByType(d.i, instr.args[0].type);
            }
            if (instr.dest) regs_[instr.dest->name] = d;
            break;
        }
        case IROp::MEMCPY: {
            RegVal dst = resolveValue(instr.args[0]);
            RegVal src = resolveValue(instr.args[1]);
            RegVal len = resolveValue(instr.args[2]);
            int64_t d = dst.i, s = src.i, n = len.i;
            // CIDER-BUG-059: copying through a nil (zero) pointer is a null
            // dereference; offset 0 is the first heap block's header, so this
            // used to silently read/write it. A zero-length copy stays a
            // no-op (memcpy(NULL, NULL, 0) is the conventional shape).
            if (n > 0 && d == 0) throw VMError{"memcpy: null pointer dereference (destination)"};
            if (n > 0 && s == 0) throw VMError{"memcpy: null pointer dereference (source)"};
            if (d + n > (int64_t)heap_.size()) heap_.resize(d + n);
            for (int64_t k = 0; k < n; ++k) {
                heap_[(size_t)(d + k)] = s + k < (int64_t)heap_.size() ? heap_[(size_t)(s + k)] : 0;
            }
            break;
        }
        case IROp::PUSH:
        case IROp::POP:
            break;  // handled by the SSA deconstructor's block-edge moves; no-op here
        case IROp::BR: {
            const std::string& target = instr.args[0].name;
            frame.prevBlock = blocks[frame.blockIdx]->getName();
            size_t next = 0;
            for (size_t bi = 0; bi < blocks.size(); ++bi) {
                if (blocks[bi]->getName() == target) {
                    next = bi;
                    break;
                }
            }
            frame.blockIdx = next;
            frame.instrIdx = 0;
            break;
        }
        case IROp::COND_BR: {
            RegVal cond = resolveValue(instr.args[0]);
            const std::string& target = cond.i != 0 ? instr.args[1].name : instr.args[2].name;
            frame.prevBlock = blocks[frame.blockIdx]->getName();
            size_t next = 0;
            for (size_t bi = 0; bi < blocks.size(); ++bi) {
                if (blocks[bi]->getName() == target) {
                    next = bi;
                    break;
                }
            }
            frame.blockIdx = next;
            frame.instrIdx = 0;
            break;
        }
        case IROp::CALL: {
            size_t before = frames_.size();
            doCall(instr.args[0], std::vector<IRValue>(instr.args.begin() + 1, instr.args.end()),
                   instr.dest ? instr.dest->name : "");
            if (instr.dest) {
                if (frames_.size() == before) {
                    // Builtin: the callee returned without pushing a frame.
                    regs_[instr.dest->name] = retReg_;
                } else if (frames_.size() >= 2) {
                    frames_[frames_.size() - 2].pendingDest = instr.dest->name;
                }
            }
            break;
        }
        case IROp::VIRTUAL_CALL: {
            doVirtualCall(instr.args[0], resolveValue(instr.args[1]).i,
                          std::vector<IRValue>(instr.args.begin() + 2, instr.args.end()));
            if (instr.dest && frames_.size() >= 2) {
                frames_[frames_.size() - 2].pendingDest = instr.dest->name;
            }
            break;
        }
        case IROp::RET: {
            retReg_ = instr.args.empty() ? RegVal{} : resolveValue(instr.args[0]);
            Frame callee = std::move(frames_.back());
            // CIDER-BUG-115: the callee's frame-local storage dies here.
            retireFrame(callee);
            frames_.pop_back();
            if (frames_.empty()) {
                // The entry function returned: its value is the exit code.
                result_.exitCode = (int)retReg_.i;
                result_.stdoutText += putcharBuf_;
                putcharBuf_.clear();  // CIDER-BUG-171: bytes are delivered
            } else {
                // Restore the caller's registers from the callee's snapshot
                // (the callee frame holds the caller's registers), then
                // deliver the return value to the caller's pending dest.
                regs_ = std::move(callee.savedRegs);
                if (!frames_.back().pendingDest.empty()) {
                    regs_[frames_.back().pendingDest] = retReg_;
                    frames_.back().pendingDest.clear();
                }
            }
            break;
        }
        case IROp::CVT_ITOF: {
            // CID-003 follow-up (I2): convert by the source's declared
            // width/sign. RegVal.i is int64_t even for U64 (raw bits), so
            // u64 must convert unsigned; F32 destinations round once, like
            // cvtsi2ss. CID-003 follow-up (W5): a narrow source register
            // may still hold raw low bits (an i8 cast lowers to AND, which
            // does not sign-normalize), so extend by the source type first
            // — native reads it with movsx/movzx.
            IRType srcT = (instr.args.size() > 0 ? instr.args[0].type : IRType::I32);
            IRType dstT = (instr.dest ? instr.dest->type : IRType::F32);
            RegVal src = resolveValue(instr.args[0]);
            int64_t sv = extendIntByType(src.i, srcT);
            RegVal d;
            d.isFloat = true;
            if (dstT == IRType::F32) {
                float f = (srcT == IRType::U64) ? (float)(uint64_t)sv
                                                : (float)sv;
                d.f = (double)f;
            } else {
                d.f = (srcT == IRType::U64) ? (double)(uint64_t)sv
                                            : (double)sv;
            }
            if (instr.dest) regs_[instr.dest->name] = d;
            break;
        }
        case IROp::CVT_FTOI: {
            RegVal d;
            d.i = (int64_t)regToFloat(resolveValue(instr.args[0]));
            if (instr.dest) regs_[instr.dest->name] = d;
            break;
        }
        case IROp::CVT_F32F64: {
            RegVal d;
            d.isFloat = true;
            d.f = regToFloat(resolveValue(instr.args[0]));
            if (instr.dest) regs_[instr.dest->name] = d;
            break;
        }
        case IROp::CVT_F64F32: {
            RegVal d;
            d.isFloat = true;
            d.f = (double)(float)regToFloat(resolveValue(instr.args[0]));
            if (instr.dest) regs_[instr.dest->name] = d;
            break;
        }
        case IROp::ASM: {
            execAsm(instr);
            break;
        }
        }
    }
    return result_;
}

VM::RegVal VM::resolveValue(const IRValue& v) {
    switch (v.kind) {
    case IRValue::CONSTANT: {
        if (v.type == IRType::PTR || (!v.name.empty() && v.name[0] == '"')) {
            // CID-005: `L"..."` wide string constant.
            if (v.name.size() >= 2 && v.name[0] == 'L' && v.name[1] == '"') {
                RegVal d;
                d.i = materializeWideString(v.name);
                return d;
            }
            // String constant: materialize into the heap.
            if (!v.name.empty() && v.name[0] == '"') {
                RegVal d;
                d.i = materializeString(v.name);
                return d;
            }
            RegVal d;
            d.i = v.intValue;
            return d;
        }
        if (v.type == IRType::F32 || v.type == IRType::F64) {
            RegVal d;
            d.isFloat = true;
            // CID-003: an F32 constant is single-precision. Native emission
            // stores the rounded float bits, so round here too — otherwise
            // `var x: float = 0.3; if (x != 0.3) return 1;` reads the
            // rounded memory value against the unrounded literal and the VM
            // disagrees with native.
            d.f = (v.type == IRType::F32) ? (double)(float)v.constValue : v.constValue;
            return d;
        }
        RegVal d;
        d.i = v.intValue;
        return d;
    }
    case IRValue::REGISTER: {
        auto it = regs_.find(v.name);
        if (it == regs_.end()) {
            std::string loc = frames_.empty() ? std::string("?") : frames_.back().fn->getName();
            const auto& fb = frames_.back();
            if (fb.blockIdx < fb.fn->getBlocks().size() && fb.instrIdx > 0) {
                loc += " @" + fb.fn->getBlocks()[fb.blockIdx]->getName() + ":" + std::to_string(fb.instrIdx - 1);
            }
            throw VMError{"read of unset register %" + v.name + " in " + loc};
        }
        return it->second;
    }
    case IRValue::LABEL: {
        auto fit = functionIds_.find(v.name);
        if (fit != functionIds_.end()) {
            RegVal d;
            d.i = fit->second;
            return d;
        }
        auto sit = stringOffsets_.find(v.name);
        if (sit != stringOffsets_.end()) {
            RegVal d;
            d.i = sit->second;
            return d;
        }
        auto git = globalVarOffsets_.find(v.name);
        if (git != globalVarOffsets_.end()) {
            RegVal d;
            d.i = git->second;
            return d;
        }
        auto vit = vtableOffsets_.find(v.name);
        if (vit != vtableOffsets_.end()) {
            RegVal d;
            d.i = vit->second;
            return d;
        }
        // CID-006: taking the value of a linker-provided extern (a
        // callback such as putchar). The synthetic id round-trips through
        // indirect calls; it is never a module function index.
        if (isExternFunctionName(v.name)) {
            RegVal d;
            d.i = externValueId(v.name);
            return d;
        }
        throw VMError{"unresolved label " + v.name};
    }
    }
    throw VMError{"bad IRValue"};
}

void VM::materializeLabels() {
    functionIds_.clear();
    externValueIds_.clear();
    externValueNames_.clear();
    const auto& funcs = module_->getFunctions();
    for (size_t i = 0; i < funcs.size(); ++i) {
        // CID-006: function VALUES are 1-based so 0 can mean nilPtr; the
        // dispatch sites convert back with (id - 1).
        functionIds_[funcs[i]->getName()] = (int64_t)i + 1;
    }
    for (const auto& [label, methods] : module_->getVTables()) {
        materializeVTable(label);
    }
    // Exception globals from globalAsm ("label: resq N" / "resd N"): an
    // 8-byte cell per label so setjmp/longjmp code can address them.
    // "label: db ..." lines (raw data blocks) are materialized like string
    // data, with the label mapping to the bytes themselves.
    // CIDER-BUG-142: file-scope user asm blocks live in their own list
    // (codegen emits them in .text); their data labels must resolve here
    // exactly as before, so scan both lists.
    std::vector<std::string> allGlobalAsm = module_->getGlobalAsm();
    for (const auto& code : module_->getUserAsm()) allGlobalAsm.push_back(code);
    for (const auto& code : allGlobalAsm) {
        std::istringstream gass(code);
        std::string line;
        while (std::getline(gass, line)) {
            size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string label = line.substr(0, colon);
            std::string body = line.substr(colon + 1);
            if (body.find("resq") != std::string::npos || body.find("resd") != std::string::npos) {
                int64_t cell = malloc(8);
                if (cell + 8 > (int64_t)heap_.size()) heap_.resize(cell + 8);
                globalVarOffsets_[label] = cell;
            } else if (body.find("db") != std::string::npos && stringOffsets_.count(label) == 0) {
                // Decode the db byte list ('H', 'e', ... or 10, 0, ...).
                std::vector<uint8_t> bytes;
                size_t pos = body.find("db") + 2;
                while (pos < body.size()) {
                    while (pos < body.size() && (body[pos] == ' ' || body[pos] == ',')) pos++;
                    if (pos >= body.size()) break;
                    if (body[pos] == '\'') {
                        // Multi-char literal: 'Hello, world!' -> one byte per char.
                        pos++;
                        while (pos < body.size() && body[pos] != '\'') {
                            char c = body[pos];
                            if (c == '\\' && pos + 1 < body.size()) {
                                char e = body[pos + 1];
                                if (e == 'n') c = '\n';
                                else if (e == 't') c = '\t';
                                else if (e == '\\') c = '\\';
                                else if (e == '"') c = '"';
                                pos++;
                            }
                            bytes.push_back((uint8_t)c);
                            pos++;
                        }
                        if (pos < body.size()) pos++;  // closing quote
                    } else {
                        char* end = nullptr;
                        bytes.push_back((uint8_t)std::strtoll(body.c_str() + pos, &end, 10));
                        if (end == body.c_str() + pos) break;  // no digits: stop
                        pos = (size_t)(end - body.c_str());
                    }
                }
                int64_t off = malloc((int64_t)bytes.size());
                if (off + (int64_t)bytes.size() > (int64_t)heap_.size()) heap_.resize(off + (int64_t)bytes.size());
                std::memcpy(heap_.data() + off, bytes.data(), bytes.size());
                stringOffsets_[label] = off;
            }
        }
    }

    // Global variables: allocate the full object size (Issue #11). A global
    // `[IDTEntry; 256]` needs 4096 bytes, a `TSSEntry` 104 bytes — not a
    // single 8-byte cell. The native backend emits `resb <totalBytes>`; the
    // VM must mirror that so indexed stores stay inside the object.
    for (const auto& var : module_->getGlobalVars()) {
        int64_t size = var.totalBytes > 0 ? (int64_t)var.totalBytes : 8;
        if (size < 8) size = 8;
        int64_t cell = malloc(size);
        if (cell + size > (int64_t)heap_.size()) heap_.resize(cell + size);
        std::memset(heap_.data() + cell, 0, (size_t)size);
        int64_t v = (int64_t)var.initialValue;
        std::memcpy(heap_.data() + cell, &v, 8);
        globalVarOffsets_[var.name] = cell;
        globalVarTypes_[var.name] = var.type;
    }

    // Pre-materialize every string in stringData (the ir-gen emits global
    // string labels there). Two passes: db lines first, then dq aliases.
    for (const auto& line : module_->getStringData()) {
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string label = line.substr(0, colon);
        std::string body = line.substr(colon + 1);
        // CID-005 fix round 1 (Q2): the width keyword is the line's FIRST
        // token. Searching the whole body matched "dw"/"db" inside a `dq`
        // target name (`g: dq dworker`), claiming the alias cell as (empty)
        // wide data and skipping the dq pass — the indirect call then saw a
        // nil/garbage value, and a `db`-named target spun forever in the
        // byte decoder below (no digits consumed, no position advance).
        size_t bodyStart = body.find_first_not_of(" \t");
        if (bodyStart == std::string::npos) continue;
        bool isWide = body.compare(bodyStart, 2, "dw") == 0;
        bool isNarrow = body.compare(bodyStart, 2, "db") == 0;
        if (!isWide && !isNarrow) continue;
        if (stringOffsets_.count(label)) continue;
        if (isWide) {
            std::vector<uint8_t> bytes;
            size_t pos = bodyStart + 2;
            while (pos < body.size()) {
                while (pos < body.size() && (body[pos] == ' ' || body[pos] == ',')) pos++;
                if (pos >= body.size()) break;
                char* end = nullptr;
                long v = std::strtol(body.c_str() + pos, &end, 0);
                if (end == body.c_str() + pos) break;
                bytes.push_back((uint8_t)(v & 0xFF));
                bytes.push_back((uint8_t)((v >> 8) & 0xFF));
                pos = (size_t)(end - body.c_str());
            }
            int64_t off = malloc((int64_t)bytes.size());
            if (off + (int64_t)bytes.size() > (int64_t)heap_.size()) heap_.resize(off + (int64_t)bytes.size());
            std::memcpy(heap_.data() + off, bytes.data(), bytes.size());
            stringOffsets_[label] = off;
            continue;
        }
        // Decode the db byte list.
        std::vector<uint8_t> bytes;
        size_t pos = bodyStart + 2;
        while (pos < body.size()) {
            // skip separators
            while (pos < body.size() && (body[pos] == ' ' || body[pos] == ',')) pos++;
            if (pos >= body.size()) break;
            int64_t val = 0;
            if (body[pos] == '\'') {
                // char literal: [quote][char][quote]
                char c = body[pos + 1];
                if (c == '\\' && pos + 2 < body.size()) {
                    char e = body[pos + 2];
                    if (e == 'n') c = '\n';
                    else if (e == 't') c = '\t';
                    else if (e == '\\') c = '\\';
                    else if (e == '"') c = '"';
                    pos += 4;  // quote + backslash + char + quote
                } else {
                    pos += 3;  // quote + char + quote
                }
                val = (unsigned char)c;
            } else {
                char* end = nullptr;
                val = std::strtoll(body.c_str() + pos, &end, 10);
                pos = (size_t)(end - body.c_str());
            }
            bytes.push_back((uint8_t)val);
        }
        int64_t off = malloc((int64_t)bytes.size());
        if (off + (int64_t)bytes.size() > (int64_t)heap_.size()) heap_.resize(off + (int64_t)bytes.size());
        std::memcpy(heap_.data() + off, bytes.data(), bytes.size());
        stringOffsets_[label] = off;
    }
    for (const auto& line : module_->getStringData()) {
        // "<label>: dq <otherlabel>" — a global variable cell holding a
        // pointer to the materialized string.
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string label = line.substr(0, colon);
        if (stringOffsets_.count(label)) continue;
        std::string body = line.substr(colon + 1);
        // CID-005 fix round 1 (Q2): same first-token anchoring as the db/dw
        // pass, so only real `dq` alias lines reach the target extraction.
        size_t dq = body.find_first_not_of(" \t");
        if (dq == std::string::npos || body.compare(dq, 2, "dq") != 0) continue;
        size_t b = body.find_first_not_of(" \t", dq + 2);
        size_t e = body.find_last_not_of(" \t");
        if (b == std::string::npos || e == std::string::npos) continue;
        std::string target = body.substr(b, e - b + 1);
        auto it = stringOffsets_.find(target);
        auto fit = functionIds_.find(target);
        if (it != stringOffsets_.end()) {
            // Allocate an 8-byte cell holding the target's offset.
            int64_t cell = malloc(8);
            if (cell + 8 > (int64_t)heap_.size()) heap_.resize(cell + 8);
            int64_t ptrVal = it->second;
            std::memcpy(heap_.data() + cell, &ptrVal, 8);
            stringOffsets_[label] = cell;
        } else if (fit != functionIds_.end() || isExternFunctionName(target)) {
            // CID-006: a global function-pointer cell holds the function's
            // id (module function) or the synthetic extern id.
            int64_t cell = malloc(8);
            if (cell + 8 > (int64_t)heap_.size()) heap_.resize(cell + 8);
            int64_t fid = (fit != functionIds_.end()) ? fit->second : externValueId(target);
            std::memcpy(heap_.data() + cell, &fid, 8);
            stringOffsets_[label] = cell;
        }
    }
}

int64_t VM::materializeString(const std::string& name) {
    // name = "\"content\"" — decode escapes.
    std::string content;
    for (size_t i = 1; i + 1 < name.size(); ++i) {
        char c = name[i];
        if (c == '\\' && i + 1 < name.size() - 1) {
            char n = name[i + 1];
            if (n == 'n') { content += '\n'; ++i; continue; }
            if (n == 't') { content += '\t'; ++i; continue; }
            if (n == '\\') { content += '\\'; ++i; continue; }
            if (n == '"') { content += '"'; ++i; continue; }
        }
        content += c;
    }
    // Materialize the string bytes plus a NUL terminator (strlen etc. rely
    // on it; the codegen's db lines also end with 0).
    int64_t off = malloc((int64_t)content.size() + 1);
    if (off + (int64_t)content.size() + 1 > (int64_t)heap_.size()) heap_.resize(off + (int64_t)content.size() + 1);
    std::memcpy(heap_.data() + off, content.data(), content.size());
    heap_[(size_t)(off + content.size())] = 0;
    stringOffsets_[name] = off;
    return off;
}

// CID-005: materialize `L"<UTF-16LE bytes incl. NUL unit>"` into the heap.
// The payload already carries the wide NUL; two extra zero bytes keep the
// allocation word-clean (mirrors the codegen's dw block plus alignment).
int64_t VM::materializeWideString(const std::string& name) {
    // name = L"<UTF-16LE bytes incl. NUL unit>"
    std::string bytes = name.substr(2, name.size() - 3);
    int64_t off = malloc((int64_t)bytes.size() + 2);
    if (off + (int64_t)bytes.size() + 2 > (int64_t)heap_.size())
        heap_.resize(off + (int64_t)bytes.size() + 2);
    std::memcpy(heap_.data() + off, bytes.data(), bytes.size());
    heap_[(size_t)(off + bytes.size())] = 0;
    heap_[(size_t)(off + bytes.size() + 1)] = 0;
    stringOffsets_[name] = off;
    return off;
}

int64_t VM::materializeVTable(const std::string& label) {
    auto it = module_->getVTables().find(label);
    if (it == module_->getVTables().end()) return 0;
    int64_t n = (int64_t)it->second.size();
    int64_t off = malloc(n * 8);
    if (off + n * 8 > (int64_t)heap_.size()) heap_.resize(off + n * 8);
    for (int64_t k = 0; k < n; ++k) {
        auto fit = functionIds_.find(it->second[(size_t)k]);
        // CID-006: vtable slots hold function VALUE ids (1-based, 0 = nil);
        // doVirtualCall converts back to an index.
        int64_t fid = fit != functionIds_.end() ? fit->second : -1;
        std::memcpy(heap_.data() + off + k * 8, &fid, 8);
    }
    // The ir-gen references the vtable as "vtable_<name>" (NewExpr stores
    // `vtable_` + the class name); the module's vtable map is keyed by the
    // bare name.
    vtableOffsets_["vtable_" + label] = off;
    return off;
}

int64_t VM::malloc(int64_t size) {
    // CIDER-BUG-110: a negative size is not an allocatable request. Native
    // malloc takes a size_t, so the negative value is a huge request that
    // returns NULL; the VM has no NULL-return path and its allocation
    // failures already surface as an allocation exception (the mmap caller
    // maps std::bad_alloc to -ENOMEM; run() translates any that escape to
    // VMError), so throw that instead of silently handing out a clamped
    // 1-byte block. size == 0 keeps the 1-byte block: native malloc(0)
    // returns a unique, freeable pointer.
    if (size < 0) throw std::bad_alloc();
    if (size == 0) size = 1;
    int64_t total = size + kHeaderSize;
    // First fit.
    for (size_t fi = 0; fi < freeList_.size(); ++fi) {
        if (freeList_[fi].size >= total) {
            int64_t off = freeList_[fi].offset;
            int64_t remaining = freeList_[fi].size - total;
            if (remaining > kHeaderSize) {
                freeList_[fi].offset += total;
                freeList_[fi].size = remaining;
            } else {
                freeList_.erase(freeList_.begin() + (long)fi);
            }
            if (off + total > (int64_t)heap_.size()) heap_.resize(off + total);
            std::memcpy(heap_.data() + off, &total, 8);  // size header
            return off + kHeaderSize;
        }
    }
    int64_t off = (int64_t)heap_.size();
    heap_.resize(off + total);
    std::memcpy(heap_.data() + off, &total, 8);
    return off + kHeaderSize;
}

void VM::free(int64_t offset) {
    if (offset == 0) return;
    int64_t block = offset - kHeaderSize;
    if (block < 0 || block + kHeaderSize > (int64_t)heap_.size()) throw VMError{"free: bad pointer"};
    int64_t total;
    std::memcpy(&total, heap_.data() + block, 8);
    if (total <= 0 || block + total > (int64_t)heap_.size()) throw VMError{"free: corrupt block"};
    // CIDER-BUG-159: double-free guard. Freeing the same block twice is UB
    // (native's allocator aborts); the VM instead keeps its free list
    // consistent by ignoring a block that overlaps one already on the list.
    // This is reachable from otherwise clean IR: a program frees an ALLOCA
    // pointer explicitly (UB) and retireFrame then releases the same block
    // when the frame dies — the duplicate entry made malloc hand the same
    // bytes out twice while both entries remained. Live blocks never
    // overlap, so no valid free is affected.
    for (const FreeBlock& b : freeList_) {
        if (block < b.offset + b.size && b.offset < block + total) return;
    }
    freeList_.push_back({block, total});
}

// CIDER-BUG-115: a frame's ALLOCA region dies with the frame. Native stack
// storage stays physically readable until a later call overwrites it, so
// programs that consume a returned frame pointer immediately (std.io's
// read_all/read_line buffers, a template's `return [x, x, x]`) still work —
// but once another frame reuses the region, the old object is gone. The VM
// used to recycle the chunk without touching its bytes, so a dangling fat
// pointer (an interface built from a callee-local by-value object) could
// read the old, plausible object back even after the region was reused,
// masking the hazard native exposes as garbage. Releasing stays as before;
// the ALLOCA path zeroes a freshly claimed chunk so frame REUSE is
// observable, exactly where native's next call overwrites the slots.
// CIDER-BUG-149: a frame's oversized-local heap fallbacks (allocated in the
// ALLOCA path when the chunk cannot hold the local) are frame storage too.
// They used to be leaked: the block stayed live and readable across returns
// and every call allotted a fresh one. Release each here, alongside the
// chunk; like the chunk they are zeroed when claimed again, not when freed,
// so an immediate post-return read still sees the dead frame's bytes.
void VM::retireFrame(const Frame& frame) {
    if (frame.localBase != 0) free(frame.localBase);
    for (const Frame::HeapAlloc& a : frame.localHeapAllocs) free(a.offset);
    // CIDER-ISS-016: the frame's jump states die with it. The handler chain
    // is a global, so a pointer to one of these buffers can outlive the
    // frame (a try that is still installed when its function returns). The
    // entry must not survive to be resumed after the frame is gone — and
    // its address can be recycled by the next frame, which would make the
    // surviving entry resume an unrelated frame. See resolveJumpState.
    for (int64_t addr : frame.jumpStateAddrs) jumpStates_.erase(addr);
}

// CIDER-BUG-159: the exit paths (the exit builtin, the asm exit syscall)
// abandon every live frame instead of unwinding it. Retire them first so
// their chunks and oversized fallbacks return to the free list, exactly as
// RET, longjmp and fall-off do. The heap dies with the VM either way, but
// the free list stays consistent for the accounting accessors and for an
// embedder that keeps using the VM instance.
void VM::retireAllFrames() {
    for (size_t i = frames_.size(); i > 0; --i) retireFrame(frames_[i - 1]);
    frames_.clear();
}

VM::RegVal VM::load(int64_t addr, IRType type) {
    // CID-006 F1: a negative address must fail cleanly. resolveValue can
    // hand out negative ids (extern value handles); memcpy-ing from
    // heap_.data() + addr is host OOB. Mirrors store's guard.
    if (addr < 0) throw VMError{"load: negative address"};
    // CIDER-BUG-059: nilPtr is the zero address. Offset 0 is the first heap
    // block's size header, so this used to return silent garbage where
    // native traps with SIGSEGV; fail loudly instead.
    if (addr == 0) throw VMError{"load: null pointer dereference"};
    RegVal d;
    switch (type) {
    case IRType::I8: {
        int8_t v = 0;
        if (addr + 1 <= (int64_t)heap_.size()) std::memcpy(&v, heap_.data() + addr, 1);
        d.i = v;
        break;
    }
    case IRType::U8: {
        uint8_t v = 0;
        if (addr + 1 <= (int64_t)heap_.size()) std::memcpy(&v, heap_.data() + addr, 1);
        d.i = v;
        break;
    }
    case IRType::I16: {
        int16_t v = 0;
        if (addr + 2 <= (int64_t)heap_.size()) std::memcpy(&v, heap_.data() + addr, 2);
        d.i = v;
        break;
    }
    case IRType::U16: {
        uint16_t v = 0;
        if (addr + 2 <= (int64_t)heap_.size()) std::memcpy(&v, heap_.data() + addr, 2);
        d.i = v;
        break;
    }
    case IRType::F32: {
        float v = 0;
        if (addr + 4 <= (int64_t)heap_.size()) std::memcpy(&v, heap_.data() + addr, 4);
        d.isFloat = true;
        d.f = v;
        break;
    }
    case IRType::I32:
    case IRType::BOOL: {
        int32_t v = 0;
        if (addr + 4 <= (int64_t)heap_.size()) std::memcpy(&v, heap_.data() + addr, 4);
        d.i = v;
        break;
    }
    case IRType::U32: {
        // Unsigned 32-bit loads zero-extend (native `mov eax, dword`),
        // so u32 values above INT32_MAX compare equal to the same u64.
        uint32_t v = 0;
        if (addr + 4 <= (int64_t)heap_.size()) std::memcpy(&v, heap_.data() + addr, 4);
        d.i = v;
        break;
    }
    case IRType::F64: {
        double v = 0;
        if (addr + 8 <= (int64_t)heap_.size()) std::memcpy(&v, heap_.data() + addr, 8);
        d.isFloat = true;
        d.f = v;
        break;
    }
    default: {
        int64_t v = 0;
        if (addr + 8 <= (int64_t)heap_.size()) std::memcpy(&v, heap_.data() + addr, 8);
        d.i = v;
        break;
    }
    }
    return d;
}

void VM::store(int64_t addr, const RegVal& val, IRType type, int64_t offset) {
    addr += offset;
    if (addr < 0) throw VMError{"store: negative address"};
    // CIDER-BUG-059: nilPtr is the zero address (see load).
    if (addr == 0) throw VMError{"store: null pointer dereference"};
    if (addr + 8 > (int64_t)heap_.size()) heap_.resize(addr + 8);
    switch (type) {
    case IRType::I8: {
        int8_t v = (int8_t)val.i;
        std::memcpy(heap_.data() + addr, &v, 1);
        break;
    }
    case IRType::U8: {
        uint8_t v = (uint8_t)val.i;
        std::memcpy(heap_.data() + addr, &v, 1);
        break;
    }
    case IRType::I16:
    case IRType::U16: {
        int16_t v = (int16_t)val.i;
        std::memcpy(heap_.data() + addr, &v, 2);
        break;
    }
    case IRType::F32: {
        float v = (float)regToFloat(val);
        std::memcpy(heap_.data() + addr, &v, 4);
        break;
    }
    case IRType::I32:
    case IRType::U32:
    case IRType::BOOL: {
        int32_t v = (int32_t)val.i;
        std::memcpy(heap_.data() + addr, &v, 4);
        break;
    }
    case IRType::F64: {
        double v = regToFloat(val);
        std::memcpy(heap_.data() + addr, &v, 8);
        break;
    }
    default: {
        int64_t v = val.i;
        std::memcpy(heap_.data() + addr, &v, 8);
        break;
    }
    }
}

namespace {
const char* const kVMBuiltins[] = {"malloc", "free", "exit", "putchar", "_setjmp", "longjmp",
                                   "_uncaught_exception", "_bounds_fail"};
}  // namespace

bool VM::isExternFunctionName(const std::string& name) const {
    for (const char* b : kVMBuiltins) {
        if (name == b) return true;
    }
    const auto& ext = module_->getExternFunctions();
    return std::find(ext.begin(), ext.end(), name) != ext.end();
}

int64_t VM::externValueId(const std::string& name) {
    auto it = externValueIds_.find(name);
    if (it != externValueIds_.end()) return it->second;
    int64_t id = -(int64_t)externValueIds_.size() - 1;
    externValueIds_[name] = id;
    externValueNames_[id] = name;
    return id;
}

// Builtins the VM provides directly (no module body). Shared by direct
// calls to unresolved labels and indirect calls carrying an extern id.
// destName carries the CALL's destination register for _setjmp
// (CIDER-BUG-053); the other builtins deliver through retReg_.
bool VM::tryBuiltinCall(const std::string& name, const std::vector<IRValue>& args,
                        const std::string& destName) {
    if (name == "malloc") {
        RegVal v = args.empty() ? RegVal{} : resolveValue(args[0]);
        RegVal d;
        d.i = malloc(v.i);
        retReg_ = d;
        return true;
    }
    if (name == "free") {
        RegVal v = args.empty() ? RegVal{} : resolveValue(args[0]);
        free(v.i);
        return true;
    }
    if (name == "exit") {
        RegVal v = args.empty() ? RegVal{} : resolveValue(args[0]);
        result_.exitCode = (int)v.i;
        result_.stdoutText += putcharBuf_;
        putcharBuf_.clear();  // CIDER-BUG-171: bytes are delivered
        // CIDER-BUG-159: abandoning the stack still retires the live frames,
        // like every other frame-death site (RET/longjmp/fall-off).
        retireAllFrames();
        return true;
    }
    if (name == "putchar") {
        RegVal v = args.empty() ? RegVal{} : resolveValue(args[0]);
        putcharBuf_ += (char)(unsigned char)v.i;
        retReg_ = v;
        return true;
    }
    if (name == "_uncaught_exception") {
        // CIDER-ISS-010: ir-gen routes a throw whose active-handler
        // pointer is null here (no enclosing try, or a rethrow from the
        // outermost catch). VMError makes main print
        // `vm error: uncaught exception` and exit 2, matching the native
        // and pomeos stub byte for byte.
        // CIDER-BUG-154: the throw site already ran this frame's scope
        // exits (destructors print via raw syscalls on native), so flush
        // what they buffered and carry it on the error: main prints it
        // to stdout before the stderr line, exactly like native.
        result_.stdoutText += putcharBuf_;
        putcharBuf_.clear();
        VMError err{"uncaught exception"};
        err.stdoutText = result_.stdoutText;
        throw err;
    }
    if (name == "_bounds_fail") {
        // CIDER-ISS-001: --checks routed a fixed-size array index outside
        // [0, N) here. VMError makes main print
        // `vm error: index out of bounds` and exit 2, matching the native
        // and pomeos stub byte for byte.
        throw VMError{"index out of bounds"};
    }
    if (name == "_setjmp") {
        RegVal buf = args.empty() ? RegVal{} : resolveValue(args[0]);
        JumpState st;
        st.frameIndex = (int)frames_.size() - 1;
        const Frame& f = frames_.back();
        st.blockIdx = (int)f.blockIdx;
        st.instrIdx = (int)f.instrIdx;
        st.dest = destName;
        st.regs = regs_;
        // CIDER-ISS-016: remember the address for this frame so retireFrame
        // can drop the entry when the frame dies. A re-try in a loop
        // re-registers the same address, so avoid a duplicate.
        Frame& hf = frames_.back();
        if (std::find(hf.jumpStateAddrs.begin(), hf.jumpStateAddrs.end(), buf.i) ==
            hf.jumpStateAddrs.end())
            hf.jumpStateAddrs.push_back(buf.i);
        jumpStates_[buf.i] = std::move(st);
        retReg_ = RegVal{};
        return true;
    }
    if (name == "longjmp") {
        RegVal buf = args.empty() ? RegVal{} : resolveValue(args[0]);
        RegVal val = args.size() > 1 ? resolveValue(args[1]) : RegVal{};
        // CIDER-ISS-010: a null handler pointer is the uncaught case.
        // ir-gen branches around longjmp for it; keep the VM defined if a
        // null buffer ever reaches this builtin anyway.
        //
        // CIDER-ISS-016: a non-null pointer is no guarantee of a live
        // handler. The chain is one global, so a frame that exits with a try
        // still installed leaves a pointer into its dead jmp_buf behind, and
        // the next throw jumps through it: the frame is gone, and its
        // ALLOCA region has already been recycled (CIDER-BUG-115), so the
        // jump lands in whatever now occupies those bytes. resolveJumpState
        // accepts a buffer only when it belongs to a frame that is still on
        // the stack, so a stale or recycled pointer takes the same defined
        // uncaught path as a null one — byte-identical stderr and exit code
        // to the null case, which is also what native's
        // _uncaught_exception stub reports.
        const JumpState* st = resolveJumpState(buf.i);
        if (!st) throw VMError{"uncaught exception"};
        // CIDER-BUG-053: discard the unwound callee frames and release
        // their frame-local regions. The interrupted call's pending result
        // is voided — the jump leaves that call site for good.
        // CIDER-BUG-115: retirement releases each region; reuse zeroes it.
        for (size_t i = frames_.size(); i > (size_t)st->frameIndex + 1; --i) {
            retireFrame(frames_[i - 1]);
        }
        frames_.resize((size_t)st->frameIndex + 1);
        Frame& f = frames_[(size_t)st->frameIndex];
        f.blockIdx = (size_t)st->blockIdx;
        f.instrIdx = (size_t)st->instrIdx;
        regs_ = st->regs;
        f.pendingDest.clear();
        // Redeliver the setjmp result to its own destination register: the
        // snapshot predates the CALL handler's write, and a callee CALL may
        // have overwritten pendingDest in the meantime.
        if (!st->dest.empty()) regs_[st->dest] = val;
        return true;
    }
    return false;
}

// CIDER-ISS-016: resolve a handler buffer to the jump state it stands for,
// or nullptr when there is no live handler at that address.
//
// A non-null buffer is not enough. The chain is one global, so a frame that
// returned with a try still installed leaves a pointer into its dead jmp_buf
// behind (retireFrame now drops those entries, so this is the belt to the
// ir-gen braces that restore the chain on frame exit), and even a live
// entry's address can have been recycled: the map is keyed by address, and
// CIDER-BUG-115 hands a retired frame's region straight back to the next
// frame. Accepting either would resume a foreign frame at a foreign
// instruction, so both must read as "no handler here" — the same defined
// uncaught path a null pointer takes.
const VM::JumpState* VM::resolveJumpState(int64_t buf) {
    if (buf == 0) return nullptr;
    auto it = jumpStates_.find(buf);
    if (it == jumpStates_.end()) return nullptr;
    const JumpState& st = it->second;
    if (st.frameIndex < 0 || st.frameIndex >= (int)frames_.size()) return nullptr;
    // The buffer must be storage the target frame still owns: either its
    // ALLOCA bump region or one of its oversized-local fallbacks. This is
    // what makes an address-recycled buffer fail even though some other
    // live frame's jump state happens to sit under the same key.
    const Frame& tf = frames_[(size_t)st.frameIndex];
    bool inRegion = tf.localSize > 0 && buf >= tf.localBase &&
                    buf + 8 <= tf.localBase + (int64_t)tf.localSize;
    for (const Frame::HeapAlloc& a : tf.localHeapAllocs)
        if (a.size > 0 && buf >= a.offset && buf + 8 <= a.offset + a.size)
            inRegion = true;
    if (!inRegion) return nullptr;
    return &st;
}

void VM::doCall(const IRValue& target, const std::vector<IRValue>& args,
                const std::string& destName) {
    int64_t fid;
    if (target.kind == IRValue::LABEL) {
        // A module-defined function wins over the builtins (e.g. the
        // imported std.alloc malloc); builtins only serve the externs.
        auto defined = functionIds_.find(target.name);
        if (defined != functionIds_.end()) {
            fid = defined->second;
        } else {
            if (tryBuiltinCall(target.name, args, destName)) return;
            throw VMError{"call to unknown function " + target.name};
        }
    } else {
        fid = resolveValue(target).i;
        if (fid < 0) {
            // CID-006: a value reference to an extern function carries a
            // synthetic negative id; dispatch builtins by name. A valid
            // non-builtin extern has no VM body — name it instead of
            // reporting a bare bad id.
            auto nit = externValueNames_.find(fid);
            if (nit != externValueNames_.end()) {
                if (tryBuiltinCall(nit->second, args, destName)) return;
                throw VMError{"call: unresolved extern function " + nit->second};
            }
            throw VMError{"call: bad function id"};
        }
    }
    // CID-006: module function values are 1-based so that 0 is nilPtr;
    // convert back to the 0-based function index for dispatch.
    if (fid == 0) throw VMError{"call: nil function pointer"};
    if (fid < 0 || fid > (int64_t)module_->getFunctions().size()) throw VMError{"call: bad function id"};
    const FunctionIR* fn = module_->getFunctions()[(size_t)(fid - 1)].get();

    // Bind arguments to the callee's parameter registers. The ir-gen names
    // params "arg_<name>" and the callee body reads those exact registers,
    // so the param name IS the register name. The caller's registers are
    // snapshotted on the callee's frame and restored when it returns.
    // Note: resolve the arguments BEFORE moving regs_ (the move empties it).
    std::vector<RegVal> resolvedArgs;
    resolvedArgs.reserve(args.size());
    for (const auto& a : args) resolvedArgs.push_back(resolveValue(a));
    // The snapshot must stay a pure copy of the caller's registers; the
    // argument bindings go into the ACTIVE register file so the callee's
    // arg_* registers never pollute the caller's snapshot.
    Frame calleeFrame{fn, 0, 0, "", "", std::move(regs_)};
    regs_ = calleeFrame.savedRegs;
    for (size_t i = 0; i < fn->getParams().size() && i < resolvedArgs.size(); ++i) {
        regs_[fn->getParams()[i]] = resolvedArgs[i];
    }
    frames_.push_back(std::move(calleeFrame));
}

void VM::doVirtualCall(const IRValue& obj, int64_t vtableIndex, const std::vector<IRValue>& args) {
    RegVal objPtr = resolveValue(obj);
    int64_t vptr = load(objPtr.i, IRType::I64).i;
    int64_t fid = load(vptr + vtableIndex * 8, IRType::I64).i;
    // CID-006: vtable slots hold 1-based function value ids (0 = nil).
    if (fid < 1 || fid > (int64_t)module_->getFunctions().size()) throw VMError{"vcall: bad function id"};
    const FunctionIR* fn = module_->getFunctions()[(size_t)(fid - 1)].get();

    std::vector<RegVal> resolvedArgs;
    resolvedArgs.reserve(args.size());
    for (const auto& a : args) resolvedArgs.push_back(resolveValue(a));
    Frame calleeFrame{fn, 0, 0, "", "", std::move(regs_)};
    regs_ = calleeFrame.savedRegs;
    size_t pi = 0;
    if (!fn->getParams().empty() && fn->getParams()[0] == "this") {
        regs_["this"] = objPtr;
        pi = 1;
    }
    for (size_t i = 0; i + pi < fn->getParams().size() && i < resolvedArgs.size(); ++i) {
        regs_[fn->getParams()[i + pi]] = resolvedArgs[i];
    }
    frames_.push_back(std::move(calleeFrame));
}

void VM::execAsm(const IRInstruction& instr) {
    if (instr.args.empty() || instr.args[0].kind != IRValue::LABEL) {
        throw VMError{"asm: missing code string"};
    }
    std::string code = instr.args[0].name;

    int64_t& rax = asmRax_;
    int64_t& rdi = asmRdi_;
    int64_t& rsi = asmRsi_;
    int64_t& rdx = asmRdx_;
    int64_t& r8 = asmR8_;
    int64_t& r9 = asmR9_;
    int64_t& r10 = asmR10_;
    int64_t& r11 = asmR11_;

    auto regVal = [&](const std::string& name) -> RegVal {
        if (name == "rax" || name == "eax") { RegVal d; d.i = (name == "eax") ? (int32_t)rax : rax; return d; }
        if (name == "rdi" || name == "edi") { RegVal d; d.i = (name == "edi") ? (int32_t)rdi : rdi; return d; }
        if (name == "rsi" || name == "esi") { RegVal d; d.i = (name == "esi") ? (int32_t)rsi : rsi; return d; }
        if (name == "rdx" || name == "edx") { RegVal d; d.i = (name == "edx") ? (int32_t)rdx : rdx; return d; }
        if (name == "r8") { RegVal d; d.i = r8; return d; }
        if (name == "r9") { RegVal d; d.i = r9; return d; }
        if (name == "r10") { RegVal d; d.i = r10; return d; }
        if (name == "r11") { RegVal d; d.i = r11; return d; }
        if (!name.empty() && name[0] == '{') {
            std::string var = name.substr(1, name.size() - 2);
            // Width suffix (Issue #24) is a codegen-only concern: the VM
            // holds full int64 values, so {fd:q} resolves like {fd}.
            size_t colon = var.find(':');
            if (colon != std::string::npos) var = var.substr(0, colon);
            auto it = instr.asmMap.find(var);
            if (it == instr.asmMap.end()) throw VMError{"asm: unknown placeholder {" + var + "}"};
            return resolveValue(it->second);
        }
        if (!name.empty() && name[0] == '%') {
            auto it = regs_.find(name.substr(1));
            if (it == regs_.end()) throw VMError{"asm: unset register " + name};
            return it->second;
        }
        RegVal d;
        d.i = std::strtoll(name.c_str(), nullptr, 0);
        return d;
    };

    bool wrote = false;
    bool exited = false;

    std::istringstream ss(code);
    std::string line;
    while (std::getline(ss, line)) {
        // strip whitespace
        size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        line = line.substr(b);
        size_t e = line.find_last_not_of(" \t");
        line = line.substr(0, e + 1);
        if (line.empty()) continue;

        std::istringstream ls(line);
        std::string mnem, a, b2;
        ls >> mnem >> a >> b2;
        if (mnem == "mov") {
            if (b2.empty()) throw VMError{"unsupported asm: " + line};
            std::string dst = a;
            if (dst.size() > 1 && dst.back() == ',') dst.pop_back();
            RegVal v = regVal(b2);
            int64_t val = v.i;
            if (dst == "rax") rax = val;
            else if (dst == "rdi") rdi = val;
            else if (dst == "rsi") rsi = val;
            else if (dst == "rdx") rdx = val;
            else if (dst == "r10") r10 = val;
            else if (dst == "r8") r8 = val;
            else if (dst == "r9") r9 = val;
            else if (dst == "edi") rdi = (int32_t)val;
            else if (dst == "esi") rsi = (int32_t)val;
            else if (dst == "edx") rdx = (int32_t)val;
            else if (dst == "eax") rax = (int32_t)val;
            else if (dst == "r8d") r8 = (int32_t)val;
            else if (dst == "r9d") r9 = (int32_t)val;
            else if (dst == "r10d") r10 = (int32_t)val;
            else if (dst == "r11d") r11 = (int32_t)val;
            else if (!dst.empty() && dst[0] == '{') {
                // Placeholder destination: write the value into the var's register.
                std::string var = dst.substr(1, dst.size() - 2);
                size_t colon = var.find(':');
                if (colon != std::string::npos) var = var.substr(0, colon);
                auto it = instr.asmMap.find(var);
                if (it == instr.asmMap.end() || it->second.kind != IRValue::REGISTER)
                    throw VMError{"unsupported asm: " + line};
                RegVal d;
                d.i = val;
                regs_[it->second.name] = d;
            } else throw VMError{"unsupported asm: " + line};
        } else if (mnem == "lea") {
            // lea <reg>, [rel <label>] — the codegen's label placeholder
            // form. The operand contains a space, so reparse the raw line.
            size_t ob = line.find('[');
            size_t oe = line.find(']', ob);
            if (ob == std::string::npos || oe == std::string::npos ||
                line.substr(ob, 5) != "[rel ")
                throw VMError{"unsupported asm: " + line};
            std::string label = line.substr(ob + 5, oe - ob - 5);
            std::string dst = a;
            if (dst.size() > 1 && dst.back() == ',') dst.pop_back();
            int64_t val = resolveValue(IRValue::Label(label)).i;
            if (dst == "rsi") rsi = val;
            else if (dst == "rdi") rdi = val;
            else if (dst == "rdx") rdx = val;
            else if (dst == "rax") rax = val;
            else throw VMError{"unsupported asm: " + line};
        } else if (mnem == "movsxd") {
            if (b2.empty()) throw VMError{"unsupported asm: " + line};
            std::string dst = a;
            if (dst.size() > 1 && dst.back() == ',') dst.pop_back();
            RegVal v = regVal(b2);
            if (dst == "rdx") rdx = (int32_t)v.i;
            else if (dst == "rax") rax = (int32_t)v.i;
            else if (dst == "rdi") rdi = (int32_t)v.i;
            else if (dst == "rsi") rsi = (int32_t)v.i;
            else throw VMError{"unsupported asm: " + line};
        } else if (mnem == "xor") {
            if (b2.empty()) throw VMError{"unsupported asm: " + line};
            std::string dst = a;
            if (dst.size() > 1 && dst.back() == ',') dst.pop_back();
            RegVal v = regVal(b2);
            int64_t x = 0;
            if (dst == "rdi") x = rdi;
            else if (dst == "rax") x = rax;
            else if (dst == "rsi") x = rsi;
            else if (dst == "rdx") x = rdx;
            else if (dst == "r8") x = r8;
            else if (dst == "r9") x = r9;
            else if (dst == "r10") x = r10;
            else throw VMError{"unsupported asm: " + line};
            int64_t y = x ^ v.i;
            if (dst == "rdi") rdi = y;
            else if (dst == "rax") rax = y;
            else if (dst == "rsi") rsi = y;
            else if (dst == "rdx") rdx = y;
            else if (dst == "r8") r8 = y;
            else if (dst == "r9") r9 = y;
            else if (dst == "r10") r10 = y;
        } else if (mnem == "syscall") {
            // CIDER-BUG-099: validate a heap-relative buffer as a signed
            // range the way native validates a user pointer. The old
            // unsigned-style `off + len <= heap_.size()` let a negative
            // offset through and indexed the host heap out of bounds;
            // subtracting off instead of adding len also keeps the check
            // overflow-safe for bogus huge offsets.
            // CIDER-BUG-103: offset 0 is the nil sentinel, not a heap
            // address. VM::malloc hands out offsets from kHeaderSize on
            // (offset 0 is the first block's size header) and load/store/
            // memcpy already treat it as null, so a non-empty buffer there
            // is a null-pointer access: EFAULT like native; count-0 calls
            // never reach this check (CIDER-BUG-110).
            auto inHeap = [&](int64_t off, int64_t len) {
                if (off == 0 && len > 0) return false;
                return off >= 0 && len >= 0 && off <= (int64_t)heap_.size() &&
                       len <= (int64_t)heap_.size() - off;
            };
            if (rax == 1) {
                // write(fd=rdi, buf=rsi, len=rdx)
                int64_t fd = rdi;
                int64_t len = rdx;
                // CIDER-BUG-110: count 0 transfers no bytes and faults on
                // nothing, so the buffer needs no validation at all — any
                // pointer is accepted and the call reports 0 bytes written.
                // Non-zero lengths keep the CIDER-BUG-099/103 checks.
                bool valid = len == 0 || inHeap(rsi, len);
                std::string bytes;
                if (valid && len > 0) {
                    bytes.assign((const char*)heap_.data() + rsi, (size_t)len);
                }
                if (fd == 1 || fd == 2) {
                    // Buffered std streams: report the number of bytes
                    // accepted, like a kernel write on a healthy fd. A
                    // negative offset or count is a bad user pointer in
                    // native; it cannot be emulated, so fail with EFAULT.
                    if (valid) {
                        if (fd == 1) result_.stdoutText += bytes;
                        else result_.stderrText += bytes;
                        rax = len;
                    } else {
                        rax = -EFAULT;
                    }
                } else {
                    // Real file descriptor (opened via the open syscall).
                    if (valid) {
                        // Count 0 must not form a host pointer from a bogus
                        // offset: that is out-of-range pointer arithmetic
                        // even though the kernel would not dereference it.
                        uint8_t* wbuf = len > 0 ? heap_.data() + rsi : nullptr;
                        rax = syscallResult((int64_t)::write((int)fd, wbuf, (size_t)len));
                    } else {
                        rax = -EFAULT;
                    }
                }
                wrote = true;
            } else if (rax == 60 || rax == 231) {
                // CIDER-BUG-153: exit_group(2) (231) is exit(2) (60) for
                // this single-threaded VM, and the CIDER-BUG-153 entry-exit
                // recognizer accepts both. The status rides rdi either way.
                result_.exitCode = (int)rdi;
                exited = true;
            } else if (rax == 9) {
                // mmap(len=rsi) -> fresh region, returned in rax
                // CIDER-BUG-103: validate the length before calling malloc.
                // VM::malloc used to clamp any size < 1 to one byte (so a
                // negative request used to "succeed" with a 1-byte block) and
                // computes
                // size + kHeaderSize on top of the current heap size
                // (near-INT64_MAX requests overflowed that arithmetic).
                // CIDER-BUG-114: native mmap takes the length as size_t, so
                // only length 0 is EINVAL; a negative i64 is a huge size_t
                // and, like a length that overflows the allocator's signed
                // arithmetic or simply cannot be satisfied, reports ENOMEM.
                // Valid requests still return the same offsets the allocator
                // always handed out.
                if (rsi == 0) {
                    rax = -EINVAL;
                } else if (rsi < 0 ||
                           rsi > INT64_MAX - kHeaderSize - (int64_t)heap_.size()) {
                    rax = -ENOMEM;
                } else {
                    try {
                        rdi = malloc(rsi);
                        rax = rdi;
                    } catch (const std::bad_alloc&) {
                        rax = -ENOMEM;
                    } catch (const std::length_error&) {
                        rax = -ENOMEM;
                    }
                }
            } else if (rax == 0) {
                // read(fd=rdi, buf=rsi, len=rdx) — stdin (0) reads the real
                // stdin into the VM heap; any other fd is a real file.
                // CIDER-BUG-099: a negative offset/length or a range past
                // the heap is EFAULT like native. The old `room` clamp ran
                // a negative offset through as a positive room and turned a
                // fully out-of-heap buffer into an EOF (0).
                // CIDER-BUG-110: count 0 performs no access and faults on
                // nothing, so skip the heap check like write above.
                if (rdx != 0 && !inHeap(rsi, rdx)) {
                    rax = -EFAULT;
                } else {
                    uint8_t* rbuf = rdx > 0 ? heap_.data() + rsi : nullptr;
                    rax = syscallResult((int64_t)::read((int)rdi, rbuf, (size_t)rdx));
                }
            } else {
                // General passthrough for the remaining syscalls (open,
                // close, unlink, lseek, ...): run the real syscall with the
                // heap-relative pointers mapped to real memory.
                auto ptrOf = [&](int64_t v) -> void* {
                    // CIDER-BUG-099: the same signed-range validation; the
                    // old `v + 8 <= size` form overflowed near INT64_MAX.
                    if (v > 0 && inHeap(v, 8)) return heap_.data() + v;
                    return nullptr;
                };
                int64_t result = 0;
                switch (rax) {
                case 2:  // open(path, flags, mode)
                    result = syscallResult((int64_t)::open((const char*)ptrOf(rdi), (int)rsi, (int)rdx));
                    break;
                case 3:  // close(fd)
                    result = syscallResult((int64_t)::close((int)rdi));
                    break;
                case 8:  // lseek(fd, offset, whence)
                    result = syscallResult((int64_t)::lseek((int)rdi, rsi, (int)rdx));
                    break;
                case 87:  // unlink(path)
                    result = syscallResult((int64_t)::unlink((const char*)ptrOf(rdi)));
                    break;
                default:
                    throw VMError{"unsupported asm syscall: rax=" + std::to_string(rax)};
                }
                rax = result;
            }
        } else {
            throw VMError{"unsupported asm: " + line};
        }
    }
    (void)wrote;
    if (exited) {
        // CIDER-BUG-169: the asm exit syscall (what std.io.exit lowers to)
        // must flush putchar's buffer exactly like the exit builtin, or a
        // program mixing extern putchar with it loses everything still
        // buffered. Flush first, then drop the stack, matching the
        // builtin's order.
        result_.stdoutText += putcharBuf_;
        putcharBuf_.clear();  // CIDER-BUG-171: bytes are delivered
        // CIDER-BUG-159: the asm exit syscall drops the stack too; retire
        // the live frames, same as the builtin exit.
        retireAllFrames();
    }
}

}  // namespace cider
