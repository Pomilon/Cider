// linear_scan.cpp — whole-function linear-scan register allocation.
//
// The input is the lowered, non-phi IR codegen receives. Liveness supplies the
// CFG facts: live-in/live-out sets per block, one RPO position per definition
// and use, a use count, and whether a call/asm/trap/volatile barrier sits
// between a definition and a later use.
//
// Slots stay the oracle. A value the scan homes is written to a register by
// the emitter and read from it; every value it refuses keeps the frame slot
// the old allocator gave it. That is what lets this land while the emitter is
// still stack-slot first: no new fallback path is invented, and a wrong home
// can always be turned back into a slot.
//
// Register classes:
//   - caller: rax, rcx, rdx, rsi, rdi, r8, r9, r10, r11
//   - callee: rbx, r12, r13, r14, r15
//   - rsp and rbp are reserved.
// A value that crosses a barrier is restricted to the callee row. Callee homes
// may cross calls; the emitter saves and restores every one it uses.
//
// The current emitter still uses fixed scratch registers inside individual
// instructions. Those are real clobbers even though they are not IR
// definitions, so the scan records a clobber mask per instruction and refuses
// a home whose live range would cross a clobber of that register. The masks
// mirror codegen.cpp; when a later Stage 1 slice stops clobbering a register,
// its mask entry shrinks in the same change. Call argument staging is the one
// case the brief singles out: it borrows r12-r15 for stack words without
// restoring them, so a value crossing such a call cannot use the staged
// registers.
//
// Spill cost is `uses * (loop depth + 1)`. If no register is free, the scan
// evicts the lowest-cost active interval that does not itself cross a barrier;
// otherwise the current interval keeps its slot. A Const is rematerialisable,
// so it carries cost zero and is the first thing evicted. A Copy destination
// inherits its source's register when the ranges do not interfere.
//
// Allocation is deterministic: candidates are ordered by position, then end,
// then value id; loops and register rows are scanned in a fixed order.

#include "codegen/linear_scan.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "codegen/liveness.h"

namespace cider {
namespace codegen {

namespace {

constexpr uint32_t kInvalidValue = 0xFFFFFFFFu;
constexpr uint8_t kNoRegister = static_cast<uint8_t>(Register::None);
constexpr uint32_t kRegisterCount =
    static_cast<uint32_t>(Register::R15) + 1;
constexpr uint64_t kFloatWords = 1;
constexpr uint32_t kSingleBlock = 1;

constexpr Register kSysVCallerPool[] = {
    Register::Rax, Register::Rcx, Register::Rdx, Register::Rsi,
    Register::Rdi, Register::R8,  Register::R9,  Register::R10,
    Register::R11,
};

// Microsoft x64 makes rsi/rdi nonvolatile. Saving and restoring an arbitrary
// callee-saved GPR is Task 13's ABI slice, so Task 9 keeps them out of the
// pool rather than clobbering the caller's values. The old r8/r9/r10 lead is
// retained so the existing UEFI contract test still sees a caller-saved home.
constexpr Register kMsAbiCallerPool[] = {
    Register::R8, Register::R9,  Register::R10,
    Register::Rax, Register::Rcx, Register::Rdx, Register::R11,
};

constexpr Register kCalleePool[] = {
    Register::Rbx, Register::R12, Register::R13, Register::R14,
    Register::R15,
};

// The incoming GPR argument rows, mirrored from codegen.cpp's argRegister /
// argRegisterMs (both private to the emitter). A SysV float word travels in
// XMM and does not consume a GPR index; on MS every non-float positional slot
// consumes one. These rows drive the fast-parameter collision mask below.
constexpr Register kIncomingSysVGpr[] = {
    Register::Rdi, Register::Rsi, Register::Rdx,
    Register::Rcx, Register::R8,  Register::R9,
};
constexpr Register kIncomingMsGpr[] = {
    Register::Rcx, Register::Rdx, Register::R8, Register::R9,
};

uint16_t bitOf(Register reg) {
    return static_cast<uint16_t>(1u << static_cast<uint8_t>(reg));
}

uint16_t maskOf(const Register* begin, const Register* end) {
    uint16_t mask = 0;
    for (const Register* at = begin; at != end; ++at) {
        mask |= bitOf(*at);
    }
    return mask;
}

uint16_t callerMask() {
    return maskOf(std::begin(kSysVCallerPool), std::end(kSysVCallerPool));
}

uint16_t callerPoolMask(bool ms) {
    return ms ? maskOf(std::begin(kMsAbiCallerPool),
                       std::end(kMsAbiCallerPool))
              : maskOf(std::begin(kSysVCallerPool),
                       std::end(kSysVCallerPool));
}

uint16_t calleeMask() {
    return maskOf(std::begin(kCalleePool), std::end(kCalleePool));
}

uint16_t allMaskFor(bool ms) {
    return static_cast<uint16_t>(callerPoolMask(ms) | calleeMask());
}

bool isByteWidth(ir::IRType width) {
    return width == ir::IRType::I8 || width == ir::IRType::U8;
}

// The emitter's fixed-scratch rule: these instructions keep their operands and
// results in memory until a later slice rewrites the sequence.
bool isFixedScratchInstruction(const ir::Instr& instr) {
    if (instr.op == ir::Op::Div || instr.op == ir::Op::Rem ||
        instr.op == ir::Op::Shl || instr.op == ir::Op::Shr ||
        instr.op == ir::Op::VaStart || instr.op == ir::Op::VaArg) {
        return true;
    }
    return instr.op == ir::Op::Mul && isByteWidth(instr.width);
}

bool isBarrier(const ir::Instr& instr) {
    if (instr.op == ir::Op::Call || instr.op == ir::Op::Asm ||
        instr.op == ir::Op::Trap || instr.op == ir::Op::VaStart ||
        instr.op == ir::Op::VaArg) {
        return true;
    }
    return (instr.op == ir::Op::Load || instr.op == ir::Op::Store) &&
           instr.isVolatile;
}

bool isCalleeSavedRegister(Register reg) {
    switch (reg) {
        case Register::Rbx:
        case Register::R12:
        case Register::R13:
        case Register::R14:
        case Register::R15:
            return true;
        case Register::Rsi:
        case Register::Rdi:
            // Microsoft x64 keeps rsi/rdi across calls, but Task 9 keeps them
            // out of the pool; Task 13 owns saving arbitrary callee-saved
            // GPRs for the fast backend.
        case Register::None:
        case Register::Rax:
        case Register::Rcx:
        case Register::Rdx:
        case Register::R8:
        case Register::R9:
        case Register::R10:
        case Register::R11:
            return false;
    }
    return false;
}

void exclude(std::unordered_set<uint32_t>& out, const ir::Value& value) {
    if (value.id != kInvalidValue) {
        out.insert(value.id);
    }
}

// Values the current emitter cannot safely read through a home. Exclusion is
// function-wide, so an id is never half-promoted in one block and read from a
// slot in another. The legacy emitter's prologue writes parameter slots and no
// code writes a home, so parameters join the set. The fast emitter writes a
// parameter's home in the prologue and reads call argument words through the
// staging list, so it keeps both out of the exclusion set.
void collectExclusions(const ir::Function& fn,
                       const std::unordered_set<uint32_t>& paramIds,
                       std::unordered_set<uint32_t>& excluded,
                       bool fast) {
    if (!fast) {
        for (const ir::Value& param : fn.params) {
            exclude(excluded, param);
        }
    }
    for (const ir::Block& block : fn.blocks) {
        for (const ir::Instr& instr : block.body) {
            if (isFixedScratchInstruction(instr)) {
                for (const ir::Value& operand : instr.operands) {
                    exclude(excluded, operand);
                }
                for (const ir::Value& result : instr.results) {
                    exclude(excluded, result);
                }
            }
            if (instr.op == ir::Op::Phi) {
                for (const ir::Value& result : instr.results) {
                    exclude(excluded, result);
                }
                for (const ir::Value& operand : instr.operands) {
                    exclude(excluded, operand);
                }
                for (const ir::PhiIncoming& incoming : instr.phiIncoming) {
                    exclude(excluded, incoming.value);
                }
            }
            if (instr.op == ir::Op::Alloca) {
                for (const ir::Value& result : instr.results) {
                    // A parameter is lowered to an alloca cell. In fast mode
                    // the emitter may promote such a cell to a register home
                    // and rewrite its direct loads and stores; any other alloca
                    // result keeps the exclusion.
                    if (fast && paramIds.find(result.id) != paramIds.end()) {
                        continue;
                    }
                    exclude(excluded, result);
                }
            }
            if (instr.op == ir::Op::Extract || instr.op == ir::Op::Insert) {
                for (const ir::Value& result : instr.results) {
                    exclude(excluded, result);
                }
                for (const ir::Value& operand : instr.operands) {
                    exclude(excluded, operand);
                }
                exclude(excluded, instr.addressBase);
                exclude(excluded, instr.storedValue);
            }
            if (instr.op == ir::Op::Call) {
                // The fast staging list reads a word from its home when it has
                // one, so a call word may keep its register. The indirect
                // callee is always read from its slot, so it stays excluded on
                // both paths.
                if (!fast) {
                    for (const ir::Argument& arg : instr.arguments) {
                        for (const ir::Value& word : arg.words) {
                            exclude(excluded, word);
                        }
                    }
                }
                if (instr.call.indirect) {
                    exclude(excluded, instr.calleeValue);
                }
            }
            if (instr.op == ir::Op::Asm) {
                for (const ir::Value& operand : instr.operands) {
                    exclude(excluded, operand);
                }
                for (const ir::Value& result : instr.results) {
                    exclude(excluded, result);
                }
            }
            if ((instr.op == ir::Op::Load || instr.op == ir::Op::Store) &&
                instr.isVolatile) {
                if (instr.op == ir::Op::Load) {
                    for (const ir::Value& result : instr.results) {
                        exclude(excluded, result);
                    }
                }
                if (instr.op == ir::Op::Store) {
                    exclude(excluded, instr.storedValue);
                }
                exclude(excluded, instr.addressBase);
            }
        }
    }
}

// The registers a call's argument staging writes. This mirrors
// Emitter::emitInstr's legacy call path exactly: the words past the argument
// row are loaded into rax/r10/r11 and then r12-r15, pushed, and not restored
// after the call. A caller-saved register is already barred from crossing a
// call; the point here is to bar the staged callee-saved registers too.
//
// The fast emitter stages stack words through rax and the frame's outgoing
// area and leaves every callee-saved register alone, so the fast path has no
// staged-register clobber to model.
uint16_t callStagingMask(const ir::Instr& instr, bool fast) {
    if (fast) {
        return 0;
    }
    const bool ms = instr.call.msabi;
    const bool setAl = instr.call.variadic && !ms;
    const bool stageInR11 = instr.call.indirect && setAl;
    const uint64_t stageBase =
        instr.call.indirect && !stageInR11 ? 1u : 0u;

    uint64_t stackWords = 0;
    if (ms) {
        uint64_t slots = 0;
        for (const ir::Argument& arg : instr.arguments) {
            const bool floatArg =
                arg.width == ir::IRType::F32 || arg.width == ir::IRType::F64;
            slots += floatArg && arg.words.size() == kFloatWords
                         ? 1
                         : static_cast<uint64_t>(arg.words.size());
        }
        constexpr uint64_t kMsArgumentRegs = 4;
        stackWords = slots > kMsArgumentRegs ? slots - kMsArgumentRegs : 0;
    } else {
        uint64_t gpr = 0;
        for (const ir::Argument& arg : instr.arguments) {
            const bool floatArg =
                arg.width == ir::IRType::F32 || arg.width == ir::IRType::F64;
            if (!floatArg) {
                gpr += static_cast<uint64_t>(arg.words.size());
            }
        }
        constexpr uint64_t kSysVArgumentRegs = 6;
        stackWords =
            gpr > kSysVArgumentRegs ? gpr - kSysVArgumentRegs : 0;
    }

    static constexpr Register kSpillRow[] = {
        Register::Rax, Register::R10, Register::R11, Register::R12,
        Register::R13, Register::R14, Register::R15,
    };
    static constexpr Register kSpillRowNoR11[] = {
        Register::Rax, Register::R10, Register::R12,
        Register::R13, Register::R14, Register::R15,
    };
    const Register* row = stageInR11 ? kSpillRowNoR11 : kSpillRow;
    const uint64_t cap = stageInR11
                             ? static_cast<uint64_t>(std::size(kSpillRowNoR11))
                             : static_cast<uint64_t>(std::size(kSpillRow));

    uint16_t mask = 0;
    if (stackWords + stageBase > cap) {
        // The emitter refuses this call before writing anything, so no home
        // can be proved safe. Baring the whole staged tail is conservative
        // and harmless: the function will not produce code.
        for (uint64_t at = stageBase; at < cap; ++at) {
            mask |= bitOf(row[at]);
        }
        return mask;
    }
    for (uint64_t at = 0; at < stackWords; ++at) {
        mask |= bitOf(row[at + stageBase]);
    }
    return mask;
}

// The registers the emitter may overwrite while executing one instruction,
// outside its own result home. These are not IR definitions but they are real
// clobbers; a home live across one of them would be silently destroyed.
uint16_t instructionClobbers(const ir::Instr& instr, bool fast) {
    const uint16_t caller = callerMask();
    switch (instr.op) {
        case ir::Op::Alloca:
        case ir::Op::Phi:
        case ir::Op::Trap:
            return 0;
        case ir::Op::Call:
            return static_cast<uint16_t>(caller |
                                         callStagingMask(instr, fast));
        case ir::Op::Asm:
            // The block's contract preserves callee-saved registers; every
            // caller-saved register is fair game. Staging pushes and pops the
            // callee register it borrows, so those survive.
            return caller;
        case ir::Op::VaStart:
        case ir::Op::VaArg:
            return static_cast<uint16_t>(caller | bitOf(Register::R11) |
                                         bitOf(Register::Rdx));
        case ir::Op::Div:
        case ir::Op::Rem:
            return static_cast<uint16_t>(bitOf(Register::Rax) |
                                         bitOf(Register::Rdx));
        case ir::Op::Shl:
        case ir::Op::Shr:
            return static_cast<uint16_t>(bitOf(Register::Rax) |
                                         bitOf(Register::Rcx));
        case ir::Op::Store:
        case ir::Op::Gep:
            return static_cast<uint16_t>(bitOf(Register::Rax) |
                                         bitOf(Register::Rcx));
        case ir::Op::Cmp:
            // The float compare path also answers its parity guard in cl.
            return static_cast<uint16_t>(bitOf(Register::Rax) |
                                         bitOf(Register::Rcx));
        case ir::Op::Cast:
            // The u64-to-float sequence answers via r11.
            return static_cast<uint16_t>(bitOf(Register::Rax) |
                                         bitOf(Register::R11));
        case ir::Op::Const:
        case ir::Op::Copy:
        case ir::Op::Add:
        case ir::Op::Sub:
        case ir::Op::Mul:
        case ir::Op::And:
        case ir::Op::Or:
        case ir::Op::Xor:
        case ir::Op::Neg:
        case ir::Op::Not:
        case ir::Op::BitNot:
        case ir::Op::FAdd:
        case ir::Op::FSub:
        case ir::Op::FMul:
        case ir::Op::FDiv:
        case ir::Op::FRem:
        case ir::Op::Load:
        case ir::Op::DataAddr:
        case ir::Op::FnAddr:
        case ir::Op::Extract:
        case ir::Op::Insert:
            return bitOf(Register::Rax);
    }
    return bitOf(Register::Rax);
}

// The set of registers argument staging can WRITE before a call argument's
// value is read. The value is safe in a caller-saved home `reg` only when
// `reg` is NOT in this mask.
//
// This mirrors emitFastCall / stageGprArguments in codegen.cpp:
//   - stack words are staged first and may write rax;
//   - a GPR argument move writes its ABI destination (rdi/rsi/rdx/rcx/r8/r9
//     on SysV) and reads its source from the word's home;
//   - stageGprArguments writes a destination only after every remaining move
//     that still reads it has executed, and skips a self-copy;
//   - a cycle scratch is drawn only from registers that are neither a
//     destination nor a sourceHome of any move, so the candidate's own home
//     (a sourceHome) can never be chosen as scratch.
// So the only writes that can precede the value's read are rax and the other
// GPR argument destinations; the value's own destination is a self-copy.
// Variadic SysV calls write AL/r11 and indirect calls reload the callee into
// rax/r11, and the Microsoft row is not modelled here, so those keep the
// conservative full-caller mask (no relaxation).
uint16_t argumentStagingEarlyWriteMask(const ir::Instr& instr,
                                       uint32_t valueId, bool ms,
                                       bool indirect) {
    const uint16_t allCaller = callerPoolMask(ms);
    if (ms || instr.call.variadic || indirect) {
        return allCaller;
    }

    // A local copy of codegen.cpp's SysV integer argument row; that helper is
    // private to the emitter, so duplicate the ordering instead of reaching
    // across. A float argument's single word travels in XMM and does not take
    // a GPR index, matching the emitter's positional walk.
    static constexpr Register kSysVArgRow[] = {
        Register::Rdi, Register::Rsi, Register::Rdx,
        Register::Rcx, Register::R8,  Register::R9,
    };
    constexpr uint32_t kGprArgCount =
        static_cast<uint32_t>(std::size(kSysVArgRow));

    // The value must occur exactly once as a GPR argument word. A repeated
    // occurrence (or none) cannot be proved safe and keeps the full mask.
    uint32_t occurrences = 0;
    uint32_t gpr = 0;
    for (const ir::Argument& arg : instr.arguments) {
        const bool floatArg = arg.width == ir::IRType::F32 ||
                              arg.width == ir::IRType::F64;
        if (floatArg && arg.words.size() == kFloatWords) {
            continue;
        }
        for (const ir::Value& word : arg.words) {
            if (gpr < kGprArgCount && word.id == valueId) {
                ++occurrences;
            }
            ++gpr;
        }
    }
    if (occurrences != 1) {
        return allCaller;
    }

    // rax is written by stack-word staging before any GPR move. Every other
    // early write is a GPR argument destination; the word carrying the value
    // is a self-copy and is excluded. Scratch cannot be the candidate's home
    // (it is that move's sourceHome), so no other register can be written.
    uint16_t early = bitOf(Register::Rax);
    gpr = 0;
    for (const ir::Argument& arg : instr.arguments) {
        const bool floatArg = arg.width == ir::IRType::F32 ||
                              arg.width == ir::IRType::F64;
        if (floatArg && arg.words.size() == kFloatWords) {
            continue;
        }
        for (const ir::Value& word : arg.words) {
            if (gpr < kGprArgCount && word.id != valueId) {
                early |= bitOf(kSysVArgRow[gpr]);
            }
            ++gpr;
        }
    }
    return early;
}

// Mirrors settleFastParameters in codegen.cpp: the fast prologue writes a
// parameter's home from its incoming ABI register, so a home that equals the
// incoming register of any LATER parameter would clobber that later word. Such
// a home is dropped there and the value falls back to its frame slot on every
// use. Forbid those registers here so the scan never proposes a home the
// prologue will discard. A parameter's own incoming register stays allowed:
// writing its home from that register is a self-copy the emitter skips.
//
// The result is indexed by parameter word (fn.params position), matching the
// positional walk in settleFastParameters. A later word staged from the stack
// writes rax before the GPR moves, so rax is forbidden too in that case.
std::vector<uint16_t> laterParameterForbiddenMasks(const ir::Function& fn,
                                                   bool ms) {
    const uint64_t count = fn.params.size();
    std::vector<uint8_t> source(count, 0);
    std::vector<uint8_t> usesRax(count, 0);
    uint64_t gpr = 0;
    for (uint64_t w = 0; w < count; ++w) {
        const ir::IRType cls = w < fn.paramClasses.size()
                                   ? fn.paramClasses[w]
                                   : ir::IRType::Void;
        const bool isFloat = cls == ir::IRType::F32 || cls == ir::IRType::F64;
        if (ms) {
            if (isFloat) {
                continue;
            }
            if (w < std::size(kIncomingMsGpr)) {
                source[w] = static_cast<uint8_t>(kIncomingMsGpr[w]);
            } else {
                usesRax[w] = 1;
            }
            continue;
        }
        if (isFloat) {
            continue;
        }
        if (gpr < std::size(kIncomingSysVGpr)) {
            source[w] = static_cast<uint8_t>(kIncomingSysVGpr[gpr]);
        } else {
            usesRax[w] = 1;
        }
        ++gpr;
    }

    std::vector<uint16_t> forbidden(count, 0);
    for (uint64_t w = 0; w < count; ++w) {
        uint16_t mask = 0;
        for (uint64_t later = w + 1; later < count; ++later) {
            if (source[later] != 0) {
                mask |= bitOf(static_cast<Register>(source[later]));
            }
            if (usesRax[later] != 0) {
                mask |= bitOf(Register::Rax);
            }
        }
        forbidden[w] = mask;
    }
    return forbidden;
}

// True when `range`'s only use is a final Store of `id` into a named slot,
// and `id` has a single definition that precedes that Store. Such a value can
// be reloaded from its slot after the barrier, so it need not occupy a
// callee-saved register. Anything else keeps today's rule.
//
// Note (verified against codegen.cpp): 0xFFFFFFFF is kInvalidValue ("no
// value"), so `addressBase.id != 0xFFFFFFFF` only proves the Store has a base,
// not that the base is a frame/alloca address. A faithful frame-address check
// would need the emitter's alloca-id set; this helper stays deliberately
// conservative on definitions/uses and relies on the checksum gate for the
// base encoding.
bool onlyPostBarrierUseIsFinalStore(const ir::Function& fn,
                                    const LiveRange& range,
                                    uint32_t id) {
    if (range.uses == 0 || !range.crossesCall) {
        return false;
    }
    uint32_t definitions = 0;
    uint32_t postBarrierUses = 0;
    bool sawFinalStore = false;
    for (const ir::Block& block : fn.blocks) {
        for (const ir::Instr& instr : block.body) {
            if (instr.op == ir::Op::Store &&
                instr.storedValue.id == id &&
                instr.addressBase.id != kInvalidValue &&
                instr.addressBase.id != id) {
                ++postBarrierUses;
                sawFinalStore = true;
                continue;
            }
            for (const ir::Value& v : instr.operands) {
                if (v.id == id) {
                    ++postBarrierUses;
                }
            }
            for (const ir::Value& v : instr.results) {
                if (v.id == id) {
                    // A definition after the final store invalidates the
                    // slot-reload assumption. The brief's verbatim version
                    // returned false on every definition, including the one
                    // before the barrier, which made its own test unreachable.
                    if (sawFinalStore) {
                        return false;
                    }
                    ++definitions;
                }
            }
        }
    }
    return definitions == 1 && postBarrierUses == 1;
}

bool terminatorClobbers(const ir::Terminator& term) {
    switch (term.term) {
        case ir::Term::Ret:
            return true;  // rax/rdx receive the return words
        case ir::Term::CondBr:
            return true;  // rax receives the condition
        case ir::Term::Switch:
            return true;
        case ir::Term::Br:
            return false;
        case ir::Term::Unreachable:
        case ir::Term::Trap:
            return false;
    }
    return false;
}

struct Candidate {
    uint32_t id = 0;
    uint32_t start = 0;
    uint32_t end = 0;
    uint32_t uses = 0;
    uint32_t depth = 0;
    uint64_t cost = 0;
    uint16_t forbidden = 0;
    bool crosses = false;
    bool isConst = false;
    bool global = false;
    // True when `start` is the definition position, so a clobber at `start`
    // is the defining instruction's own scratch, not a live-across clobber.
    // A value live into a loop header starts at that block's first position
    // instead — the first instruction really can destroy it before the def.
    // A parameter is written by the prologue, before position zero, so its
    // start is never a definition even though the live range starts there.
    bool startsAtDefinition = true;
};

struct Active {
    uint32_t id = 0;
    uint32_t end = 0;
    uint64_t cost = 0;
    uint8_t reg = kNoRegister;
    bool crosses = false;
};

bool candidateLess(const Candidate& lhs, const Candidate& rhs) {
    if (lhs.start != rhs.start) {
        return lhs.start < rhs.start;
    }
    if (lhs.end != rhs.end) {
        return lhs.end > rhs.end;
    }
    return lhs.id < rhs.id;
}

}  // namespace

FunctionAllocation allocateLinearScan(const ir::Function& fn,
                                      const Liveness& live, bool fast) {
    FunctionAllocation result;
    const size_t blockCount = fn.blocks.size();
    result.blocks.resize(blockCount);
    result.live = live;
    if (blockCount == 0) {
        return result;
    }

    const bool ms = fn.signature.abi() == Abi::MsAbi;

    uint32_t top = 0;
    for (const LiveRange& range : live.ranges) {
        if (range.value > top) {
            top = range.value;
        }
    }
    for (const ir::Value& param : fn.params) {
        if (param.id != kInvalidValue && param.id > top) {
            top = param.id;
        }
    }
    for (const ir::Block& block : fn.blocks) {
        for (const ir::Instr& instr : block.body) {
            for (const ir::Value& value : instr.results) {
                if (value.id != kInvalidValue && value.id > top) {
                    top = value.id;
                }
            }
        }
    }

    const uint32_t valueCount = top + 1;

    for (BlockAllocation& block : result.blocks) {
        block.home.assign(valueCount, kNoRegister);
    }
    result.spillSlot.resize(valueCount);
    for (uint32_t id = 0; id <= top; ++id) {
        // The current frame gives every value id its own eight-byte cell.
        result.spillSlot[id] = id;
    }

    std::vector<const LiveRange*> rangeAt(valueCount,
                                          nullptr);
    for (const LiveRange& range : live.ranges) {
        rangeAt[range.value] = &range;
    }

    std::vector<uint32_t> defCount(valueCount, 0);
    // Every block that defines the value, not just the last definition. After
    // lowerPhis a loop value has one Copy on each incoming edge; all of them
    // write the same home and the range already covers first def -> last use.
    // A single home is valid because every definition writes it and the range
    // is the union of all definition-to-use ranges.
    std::vector<std::vector<uint32_t>> defBlocks(valueCount);
    std::vector<ir::IRType> widthAt(valueCount,
                                   ir::IRType::Void);
    std::vector<uint8_t> isConstValue(valueCount, 0);

    // A parameter is lowered to an Alloca, so the unoptimised IR mentions its
    // id twice: once in `fn.params` and once as the Alloca's result. The cell
    // is one definition, not two; counting both would keep every such
    // parameter out of the scan.
    std::unordered_set<uint32_t> allocaIds;
    for (const ir::Block& block : fn.blocks) {
        for (const ir::Instr& instr : block.body) {
            if (instr.op != ir::Op::Alloca) {
                continue;
            }
            for (const ir::Value& result : instr.results) {
                if (result.id != kInvalidValue) {
                    allocaIds.insert(result.id);
                }
            }
        }
    }
    for (const ir::Value& param : fn.params) {
        if (param.id == kInvalidValue || allocaIds.find(param.id) !=
                                             allocaIds.end()) {
            continue;
        }
        ++defCount[param.id];
        defBlocks[param.id].push_back(0);
        widthAt[param.id] = param.width;
    }
    for (uint32_t b = 0; b < blockCount; ++b) {
        for (const ir::Instr& instr : fn.blocks[b].body) {
            for (const ir::Value& value : instr.results) {
                if (value.id == kInvalidValue) {
                    continue;
                }
                ++defCount[value.id];
                defBlocks[value.id].push_back(b);
                widthAt[value.id] = value.width;
                isConstValue[value.id] = instr.op == ir::Op::Const ? 1 : 0;
            }
        }
    }

    std::unordered_set<uint32_t> excluded;
    std::unordered_set<uint32_t> paramIds;
    for (const ir::Value& param : fn.params) {
        if (param.id != kInvalidValue) {
            paramIds.insert(param.id);
        }
    }
    collectExclusions(fn, paramIds, excluded, fast);

    // Fast-mode parameter homes are dropped by settleFastParameters when they
    // collide with a later parameter's incoming register. Mirror that walk so
    // the scan chooses a home the prologue will actually keep.
    std::unordered_map<uint32_t, uint16_t> paramForbiddenById;
    if (fast) {
        const std::vector<uint16_t> masks =
            laterParameterForbiddenMasks(fn, ms);
        for (uint64_t w = 0; w < fn.params.size() && w < masks.size(); ++w) {
            const uint32_t id = fn.params[w].id;
            if (id != kInvalidValue) {
                paramForbiddenById[id] = masks[w];
            }
        }
    }

    // Every block where a value is defined, used, or live through. Liveness
    // already covers a use in a block whose definition sits earlier in the
    // same block: the defining block is added explicitly below.
    std::vector<std::vector<uint32_t>> presence(blockCount);
    std::vector<std::vector<uint32_t>> valueBlocks(
        valueCount);
    for (uint32_t b = 0; b < blockCount; ++b) {
        auto addIds = [&](const std::vector<uint32_t>& ids) {
            for (uint32_t id : ids) {
                if (id <= top) {
                    presence[b].push_back(id);
                }
            }
        };
        addIds(live.liveIn[b]);
        addIds(live.liveOut[b]);
    }
    for (uint32_t id = 0; id <= top; ++id) {
        for (uint32_t b : defBlocks[id]) {
            presence[b].push_back(id);
        }
    }
    for (uint32_t b = 0; b < blockCount; ++b) {
        std::sort(presence[b].begin(), presence[b].end());
        presence[b].erase(std::unique(presence[b].begin(), presence[b].end()),
                          presence[b].end());
        for (uint32_t id : presence[b]) {
            valueBlocks[id].push_back(b);
        }
    }

    std::vector<uint32_t> blockPosition(blockCount, 0);
    for (uint32_t at = 0; at < live.blockOrder.size(); ++at) {
        blockPosition[live.blockOrder[at]] = at;
    }

    std::vector<std::vector<uint32_t>> preds(blockCount);
    std::vector<std::vector<uint32_t>> succ(blockCount);
    std::unordered_map<uint32_t, uint32_t> blockAt;
    for (uint32_t b = 0; b < blockCount; ++b) {
        blockAt[fn.blocks[b].label] = b;
    }
    auto addTarget = [&](std::vector<uint32_t>& out, uint32_t label) {
        auto found = blockAt.find(label);
        if (found == blockAt.end()) {
            return;
        }
        if (std::find(out.begin(), out.end(), found->second) == out.end()) {
            out.push_back(found->second);
        }
    };
    for (uint32_t b = 0; b < blockCount; ++b) {
        const ir::Terminator& term = fn.blocks[b].term;
        switch (term.term) {
            case ir::Term::Br:
                addTarget(succ[b], term.target);
                break;
            case ir::Term::CondBr:
                addTarget(succ[b], term.thenTarget);
                addTarget(succ[b], term.elseTarget);
                break;
            case ir::Term::Switch:
                for (const ir::Case& arm : term.cases) {
                    addTarget(succ[b], arm.target);
                }
                addTarget(succ[b], term.defaultTarget);
                break;
            case ir::Term::Ret:
            case ir::Term::Unreachable:
            case ir::Term::Trap:
                break;
        }
    }
    for (uint32_t b = 0; b < blockCount; ++b) {
        for (uint32_t target : succ[b]) {
            preds[target].push_back(b);
        }
    }

    // Natural-loop depth. An edge to a block at or before the source's RPO
    // position is a back edge in the reducible CFG lowering produces. Walking
    // predecessors back to the header gives the loop body; nested loops add
    // their depth. This feeds only the spill cost, so an inexact answer on an
    // irreducible graph changes which value spills, never whether a home is
    // safe.
    std::vector<uint32_t> loopDepth(blockCount, 0);
    for (uint32_t tail = 0; tail < blockCount; ++tail) {
        for (uint32_t head : succ[tail]) {
            if (blockPosition[head] > blockPosition[tail]) {
                continue;
            }
            std::vector<uint8_t> inLoop(blockCount, 0);
            std::vector<uint32_t> stack;
            stack.push_back(tail);
            inLoop[head] = 1;
            while (!stack.empty()) {
                const uint32_t node = stack.back();
                stack.pop_back();
                if (inLoop[node] != 0) {
                    continue;
                }
                inLoop[node] = 1;
                if (node == head) {
                    continue;
                }
                for (uint32_t pred : preds[node]) {
                    if (inLoop[pred] == 0) {
                        stack.push_back(pred);
                    }
                }
            }
            for (uint32_t b = 0; b < blockCount; ++b) {
                if (inLoop[b] != 0) {
                    ++loopDepth[b];
                }
            }
        }
    }

    const bool positionSafe = blockCount <= kPositionedBlockLimit;
    std::array<std::vector<uint32_t>, kRegisterCount> clobberPositions;
    std::vector<uint32_t> hardStops;
    std::unordered_map<uint32_t, const ir::Instr*> callAtPosition;
    if (positionSafe) {
        for (uint32_t b = 0; b < blockCount; ++b) {
            const uint32_t base = blockPosition[b] * kBlockPositionScale;
            const ir::Block& block = fn.blocks[b];
            for (uint32_t i = 0; i < block.body.size(); ++i) {
                uint16_t clobbers = instructionClobbers(block.body[i], fast);
                for (uint8_t reg = 1; reg < kRegisterCount; ++reg) {
                    if ((clobbers & bitOf(static_cast<Register>(reg))) != 0) {
                        clobberPositions[reg].push_back(base + i);
                    }
                }
                if (fast && block.body[i].op == ir::Op::Call) {
                    callAtPosition[base + i] = &block.body[i];
                }
                if (block.body[i].op == ir::Op::Trap) {
                    // A trap is terminal: no path reaches a later use, so a
                    // value whose linear range spans it keeps its slot rather
                    // than claiming a register for unreachable code.
                    hardStops.push_back(base + i);
                }
            }
            if (terminatorClobbers(block.term)) {
                const uint32_t at = base + static_cast<uint32_t>(block.body.size());
                const uint16_t clobbers = static_cast<uint16_t>(
                    bitOf(Register::Rax) | bitOf(Register::Rdx));
                for (uint8_t reg = 1; reg < kRegisterCount; ++reg) {
                    if ((clobbers & bitOf(static_cast<Register>(reg))) != 0) {
                        clobberPositions[reg].push_back(at);
                    }
                }
            }
        }
        for (auto& positions : clobberPositions) {
            std::sort(positions.begin(), positions.end());
            positions.erase(std::unique(positions.begin(), positions.end()),
                            positions.end());
        }
        std::sort(hardStops.begin(), hardStops.end());
    }

    std::vector<Candidate> candidates;
    candidates.reserve(top + 1);
    for (uint32_t id = 0; id <= top; ++id) {
        const LiveRange* range = rangeAt[id];
        // A multi-definition value is a loop-carried phi lowered to one Copy
        // per incoming edge. One home is safe because every definition writes
        // it and the range spans first definition to last use. The legacy
        // emitter keeps its old slot text, so only the fast scan takes these.
        const bool multiDefinition = defCount[id] > 1;
        if (range == nullptr || defCount[id] == 0 ||
            (multiDefinition && !fast) ||
            excluded.find(id) != excluded.end() ||
            widthAt[id] == ir::IRType::Void || range->isFloat ||
            range->uses == 0) {
            continue;
        }

        Candidate candidate;
        candidate.id = id;
        candidate.start = range->start;
        candidate.end = range->end;
        candidate.uses = range->uses;
        candidate.crosses = range->crossesCall;
        candidate.isConst = isConstValue[id] != 0;
        candidate.global = valueBlocks[id].size() > kSingleBlock;

        for (uint32_t b : valueBlocks[id]) {
            const uint32_t base = blockPosition[b] * kBlockPositionScale;
            const uint32_t blockEnd =
                base + static_cast<uint32_t>(fn.blocks[b].body.size());
            const bool liveIn = std::binary_search(live.liveIn[b].begin(),
                                                   live.liveIn[b].end(), id);
            const bool liveOut = std::binary_search(live.liveOut[b].begin(),
                                                    live.liveOut[b].end(), id);
            // A definition block starts the range at the first definition,
            // which is already range->start, unless the value is live into the
            // block. A block the value merely passes through widens the range
            // to the whole block. With several definitions the first one sets
            // the start; later definitions sit inside the same range.
            const bool definedHere = std::binary_search(
                defBlocks[id].begin(), defBlocks[id].end(), b);
            if (!definedHere || liveIn) {
                candidate.start = std::min(candidate.start, base);
            }
            if (!definedHere || liveOut) {
                candidate.end = std::max(candidate.end, blockEnd);
            }
            candidate.depth = std::max(candidate.depth, loopDepth[b]);
        }
        candidate.cost = candidate.isConst
                             ? 0
                             : static_cast<uint64_t>(candidate.uses) *
                                   (static_cast<uint64_t>(candidate.depth) + 1);
        candidate.startsAtDefinition = candidate.start == range->start &&
                                       paramIds.find(id) == paramIds.end();

        if (!positionSafe) {
            // Wrapped positions cannot order intervals safely; keep every
            // candidate in its slot instead of guessing.
            candidate.forbidden = allMaskFor(ms);
        } else {
            for (uint8_t reg = 1; reg < kRegisterCount; ++reg) {
                const auto& positions = clobberPositions[reg];
                // At the definition, the instruction's own scratch is written
                // after the result is computed, so it is not a clobber the
                // value must survive. At any other start (a loop live-in), it
                // is: the value must already be in the register there.
                auto at = candidate.startsAtDefinition
                              ? std::upper_bound(positions.begin(),
                                                 positions.end(),
                                                 candidate.start)
                              : std::lower_bound(positions.begin(),
                                                 positions.end(),
                                                 candidate.start);
                if (at == positions.end() || *at > candidate.end) {
                    continue;
                }
                bool relaxed = false;
                // A call clobber at the value's last-use position is allowed
                // only when staging cannot write the candidate's home before
                // the value is read as that call's argument. No other
                // position, and no other instruction, may relax the rule.
                if (fast && *at == candidate.end) {
                    auto callAt = callAtPosition.find(*at);
                    if (callAt != callAtPosition.end()) {
                        const ir::Instr& call = *callAt->second;
                        const uint16_t early =
                            argumentStagingEarlyWriteMask(
                                call, candidate.id, call.call.msabi,
                                call.call.indirect);
                        const Register regEnum = static_cast<Register>(reg);
                        relaxed = (early & bitOf(regEnum)) == 0;
                    }
                }
                if (!relaxed) {
                    candidate.forbidden |= bitOf(static_cast<Register>(reg));
                }
            }
            auto stop = std::upper_bound(hardStops.begin(), hardStops.end(),
                                         candidate.start);
            if (stop != hardStops.end() && *stop <= candidate.end) {
                candidate.forbidden = allMaskFor(ms);
            }
        }
        if (fast) {
            const auto it = paramForbiddenById.find(id);
            if (it != paramForbiddenById.end()) {
                candidate.forbidden |= it->second;
            }
        }
        candidates.push_back(candidate);
    }

    // A Copy destination that does not interfere with its source can keep the
    // same register; the emitter then emits no instruction for the copy.
    std::vector<uint32_t> copySource(valueCount,
                                     kInvalidValue);
    for (const ir::Block& block : fn.blocks) {
        for (const ir::Instr& instr : block.body) {
            if (instr.op != ir::Op::Copy || instr.results.empty() ||
                instr.operands.empty()) {
                continue;
            }
            const uint32_t dest = instr.results[0].id;
            const uint32_t source = instr.operands[0].id;
            if (dest <= top && source <= top) {
                copySource[dest] = source;
            }
        }
    }

    std::sort(candidates.begin(), candidates.end(), candidateLess);

    std::vector<uint8_t> regOf(valueCount, kNoRegister);
    std::vector<Active> active;
    const uint16_t all = allMaskFor(ms);
    const uint16_t callee = calleeMask();
    const Register* callerBegin = ms ? std::begin(kMsAbiCallerPool)
                                     : std::begin(kSysVCallerPool);
    const Register* callerEnd = ms ? std::end(kMsAbiCallerPool)
                                   : std::end(kSysVCallerPool);

    for (const Candidate& candidate : candidates) {
        // An interval ending exactly where this one begins is dead after the
        // boundary instruction's reads, so its register is free. That reuse
        // is only safe when this value is the boundary instruction's own
        // result: a value live into the position (a parameter written by the
        // prologue, or a loop live-in) must already occupy its register when
        // the instruction reads it, so sharing it with an operand read at the
        // same position loses one of the two values. The clobber mask above
        // is what keeps scratch-register reuse safe.
        active.erase(std::remove_if(active.begin(), active.end(),
                                    [&](const Active& held) {
                                        return held.end < candidate.start ||
                                               (held.end == candidate.start &&
                                                candidate.startsAtDefinition);
                                    }),
                     active.end());

        uint16_t taken = 0;
        for (const Active& held : active) {
            taken |= bitOf(static_cast<Register>(held.reg));
        }
        // Rank 2: a crosser whose only post-barrier use is a final Store keeps
        // its slot: the value is reloaded from the frame after the barrier
        // instead of spending a scarce callee home. Forcing the slot (rather
        // than giving it a caller home that a call could clobber) keeps this
        // sound for every value shape, not just rematerialisable constants.
        const bool slotReloadSafe = fast && candidate.crosses &&
            onlyPostBarrierUseIsFinalStore(fn, *rangeAt[candidate.id],
                                           candidate.id);
        const uint16_t effectiveForbidden =
            slotReloadSafe ? all : candidate.forbidden;
        const uint16_t classMask = candidate.crosses ? callee : all;
        uint16_t allowed =
            static_cast<uint16_t>(classMask & ~effectiveForbidden &
                                  ~taken);

        uint8_t chosen = kNoRegister;
        const uint32_t source = copySource[candidate.id];
        if (source != kInvalidValue && source <= top) {
            const uint8_t sourceReg = regOf[source];
            const LiveRange* sourceRange = rangeAt[source];
            if (sourceReg != kNoRegister && sourceRange != nullptr &&
                sourceRange->end <= candidate.start &&
                (allowed & bitOf(static_cast<Register>(sourceReg))) != 0) {
                chosen = sourceReg;
            }
        }
        if (chosen == kNoRegister) {
            const Register* begin = candidate.crosses ? std::begin(kCalleePool)
                                                      : callerBegin;
            const Register* end = candidate.crosses ? std::end(kCalleePool)
                                                    : callerEnd;
            for (const Register* at = begin; at != end; ++at) {
                if ((allowed & bitOf(*at)) != 0) {
                    chosen = static_cast<uint8_t>(*at);
                    break;
                }
            }
            if (chosen == kNoRegister && !candidate.crosses) {
                for (const Register* at = std::begin(kCalleePool);
                     at != std::end(kCalleePool); ++at) {
                    if ((allowed & bitOf(*at)) != 0) {
                        chosen = static_cast<uint8_t>(*at);
                        break;
                    }
                }
            }
        }

        if (chosen == kNoRegister) {
            // No free register in the class. Evict the lowest-cost active
            // interval whose register this interval can still take. An
            // interval that crosses a barrier is never evicted: it is the
            // callee-saved home whose value a call depends on.
            const uint16_t usable =
                static_cast<uint16_t>(classMask & ~effectiveForbidden);
            Active* victim = nullptr;
            for (Active& held : active) {
                if (held.crosses) {
                    continue;
                }
                if ((usable & bitOf(static_cast<Register>(held.reg))) == 0) {
                    continue;
                }
                if (victim == nullptr || held.cost < victim->cost ||
                    (held.cost == victim->cost && held.end > victim->end) ||
                    (held.cost == victim->cost && held.end == victim->end &&
                     held.id < victim->id)) {
                    victim = &held;
                }
            }
            if (victim != nullptr && victim->cost < candidate.cost) {
                const uint8_t freed = victim->reg;
                regOf[victim->id] = kNoRegister;
                const uint32_t victimId = victim->id;
                active.erase(std::remove_if(active.begin(), active.end(),
                                            [&](const Active& held) {
                                                return held.id == victimId;
                                            }),
                             active.end());
                chosen = freed;
            }
        }

        if (chosen == kNoRegister) {
            continue;
        }
        regOf[candidate.id] = chosen;
        active.push_back(Active{candidate.id, candidate.end, candidate.cost,
                                chosen, candidate.crosses});
    }

    uint16_t used = 0;
    for (uint32_t id = 0; id <= top; ++id) {
        if (regOf[id] != kNoRegister) {
            used |= bitOf(static_cast<Register>(regOf[id]));
        }
    }
    for (uint8_t reg = 1; reg < kRegisterCount; ++reg) {
        const Register candidate = static_cast<Register>(reg);
        if ((used & bitOf(candidate)) != 0 &&
            isCalleeSavedRegister(candidate)) {
            result.calleeSaved.push_back(candidate);
        }
    }

    for (uint32_t b = 0; b < blockCount; ++b) {
        for (uint32_t id : presence[b]) {
            if (regOf[id] != kNoRegister) {
                result.blocks[b].home[id] = regOf[id];
            }
        }
    }

    for (const Candidate& candidate : candidates) {
        if (regOf[candidate.id] != kNoRegister) {
            ++result.valuesAllocated;
            if (candidate.global) {
                ++result.globalValues;
            }
        } else {
            ++result.valuesSpilled;
            if (candidate.global) {
                ++result.globalSpilled;
            }
        }
    }

    for (const ir::Block& block : fn.blocks) {
        for (const ir::Instr& instr : block.body) {
            if (isBarrier(instr)) {
                ++result.barrierCount;
            }
        }
        if (block.term.term == ir::Term::Trap) {
            ++result.barrierCount;
        }
    }
    return result;
}

FunctionAllocation allocateLinearScan(const ir::Function& fn) {
    return allocateLinearScan(fn, analyzeLiveness(fn));
}

}  // namespace codegen
}  // namespace cider
