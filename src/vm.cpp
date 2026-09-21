#include "vm.h"

#include <cstring>
#include <sstream>
#include <fcntl.h>
#include <unistd.h>

namespace cider {

namespace {

double regToFloat(const VM::RegVal& v) { return v.isFloat ? v.f : (double)v.i; }

}  // namespace

VMResult VM::run(const ModuleIR& module, const std::string& runFunction) {
    module_ = &module;
    runFunction_ = runFunction;
    result_ = VMResult{};
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
            RegVal v;
            v.i = 0;
            regs_["arg_argv"] = v;
        }
    }

    // Parameters arrive in arg_* registers (the ir-gen names them).
    while (!frames_.empty()) {
        Frame& frame = frames_.back();
        const auto& blocks = frame.fn->getBlocks();
        if (frame.blockIdx >= blocks.size()) {
            // Fell off the end: implicit void return.
            frames_.pop_back();
            continue;
        }
        const auto& instrs = blocks[frame.blockIdx]->getInstructions();
        if (frame.instrIdx >= instrs.size()) {
            frames_.pop_back();
            continue;
        }
        const IRInstruction& instr = instrs[frame.instrIdx];
        frame.instrIdx++;

        if (++steps_ > kStepLimit) throw VMError{"vm: step limit exceeded"};

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
                switch (instr.op) {
                case IROp::ADD: d.f = x + y; break;
                case IROp::SUB: d.f = x - y; break;
                case IROp::MUL: d.f = x * y; break;
                case IROp::DIV: d.f = x / y; break;
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
                switch (instr.op) {
                case IROp::ADD: d.i = x + y; break;
                case IROp::SUB: d.i = x - y; break;
                case IROp::MUL: d.i = x * y; break;
                case IROp::DIV: d.i = (y != 0) ? (divUnsigned ? (int64_t)(ux / uy) : x / y) : 0; break;
                case IROp::MOD: d.i = (y != 0) ? (divUnsigned ? (int64_t)(ux % uy) : x % y) : 0; break;
                case IROp::BIT_AND: d.i = x & y; break;
                case IROp::BIT_OR: d.i = x | y; break;
                case IROp::BIT_XOR: d.i = x ^ y; break;
                case IROp::LSHIFT: d.i = (int64_t)(ux << n); break;
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
                int64_t x = a.i, y = b.i;
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
            if (instr.args[0].kind == IRValue::LABEL) type = IRType::I64;  // global cells are 8 bytes
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
            }
            size_t sz = (size_t)size.i;
            sz = (sz + 7) & ~(size_t)7;
            if (af.localUsed + sz <= af.localSize) {
                d.i = af.localBase + (int64_t)af.localUsed;
                af.localUsed += sz;
            } else {
                d.i = malloc(size.i);  // oversized local: heap fallback
            }
            if (instr.dest) regs_[instr.dest->name] = d;
            break;
        }
        case IROp::MOV: {
            RegVal d = resolveValue(instr.args[0]);
            if (instr.dest) regs_[instr.dest->name] = d;
            break;
        }
        case IROp::MEMCPY: {
            RegVal dst = resolveValue(instr.args[0]);
            RegVal src = resolveValue(instr.args[1]);
            RegVal len = resolveValue(instr.args[2]);
            int64_t d = dst.i, s = src.i, n = len.i;
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
            doCall(instr.args[0], std::vector<IRValue>(instr.args.begin() + 1, instr.args.end()));
            if (instr.dest) {
                if (frames_.size() == before) {
                    // Builtin: the callee returned without pushing a frame.
                    regs_[instr.dest->name] = retReg_;
                    // Also record the pending dest so a longjmp can
                    // re-deliver the setjmp's result.
                    frames_.back().pendingDest = instr.dest->name;
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
            if (callee.localBase != 0) free(callee.localBase);
            frames_.pop_back();
            if (frames_.empty()) {
                // The entry function returned: its value is the exit code.
                result_.exitCode = (int)retReg_.i;
                result_.stdoutText += putcharBuf_;
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
            RegVal d;
            d.isFloat = true;
            d.f = (double)resolveValue(instr.args[0]).i;
            if (instr.dest) regs_[instr.dest->name] = d;
            break;
        }
        case IROp::CVT_FTOI: {
            RegVal d;
            d.i = (int64_t)regToFloat(resolveValue(instr.args[0]));
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
            d.f = v.constValue;
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
        throw VMError{"unresolved label " + v.name};
    }
    }
    throw VMError{"bad IRValue"};
}

void VM::materializeLabels() {
    functionIds_.clear();
    const auto& funcs = module_->getFunctions();
    for (size_t i = 0; i < funcs.size(); ++i) {
        functionIds_[funcs[i]->getName()] = (int64_t)i;
    }
    for (const auto& [label, methods] : module_->getVTables()) {
        materializeVTable(label);
    }
    // Exception globals from globalAsm ("label: resq N" / "resd N"): an
    // 8-byte cell per label so setjmp/longjmp code can address them.
    // "label: db ..." lines (raw data blocks) are materialized like string
    // data, with the label mapping to the bytes themselves.
    for (const auto& code : module_->getGlobalAsm()) {
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
        if (body.find("db") == std::string::npos) continue;
        if (stringOffsets_.count(label)) continue;
        // Decode the db byte list.
        std::vector<uint8_t> bytes;
        size_t pos = body.find("db") + 2;
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
        size_t dq = body.find("dq");
        if (dq == std::string::npos) continue;
        size_t b = body.find_first_not_of(" \t", dq + 2);
        size_t e = body.find_last_not_of(" \t");
        if (b == std::string::npos || e == std::string::npos) continue;
        std::string target = body.substr(b, e - b + 1);
        auto it = stringOffsets_.find(target);
        if (it != stringOffsets_.end()) {
            // Allocate an 8-byte cell holding the target's offset.
            int64_t cell = malloc(8);
            if (cell + 8 > (int64_t)heap_.size()) heap_.resize(cell + 8);
            int64_t ptrVal = it->second;
            std::memcpy(heap_.data() + cell, &ptrVal, 8);
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

int64_t VM::materializeVTable(const std::string& label) {
    auto it = module_->getVTables().find(label);
    if (it == module_->getVTables().end()) return 0;
    int64_t n = (int64_t)it->second.size();
    int64_t off = malloc(n * 8);
    if (off + n * 8 > (int64_t)heap_.size()) heap_.resize(off + n * 8);
    for (int64_t k = 0; k < n; ++k) {
        auto fit = functionIds_.find(it->second[(size_t)k]);
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
    if (size < 1) size = 1;
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
    freeList_.push_back({block, total});
}

VM::RegVal VM::load(int64_t addr, IRType type) {
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

void VM::doCall(const IRValue& target, const std::vector<IRValue>& args) {
    int64_t fid;
    if (target.kind == IRValue::LABEL) {
        // A module-defined function wins over the builtins (e.g. the
        // imported std.alloc malloc); builtins only serve the externs.
        auto defined = functionIds_.find(target.name);
        if (defined != functionIds_.end()) {
            fid = defined->second;
        } else {
        // VM builtins: extern runtime functions the VM provides directly.
        if (target.name == "malloc") {
            RegVal v = args.empty() ? RegVal{} : resolveValue(args[0]);
            RegVal d;
            d.i = malloc(v.i);
            retReg_ = d;
            return;
        }
        if (target.name == "free") {
            RegVal v = args.empty() ? RegVal{} : resolveValue(args[0]);
            free(v.i);
            return;
        }
        if (target.name == "exit") {
            RegVal v = args.empty() ? RegVal{} : resolveValue(args[0]);
            result_.exitCode = (int)v.i;
            result_.stdoutText += putcharBuf_;
            frames_.clear();
            return;
        }
        if (target.name == "putchar") {
            // The libc putchar buffers stdout; the flush happens at exit,
            // so putchar output lands after all direct write syscalls.
            RegVal v = args.empty() ? RegVal{} : resolveValue(args[0]);
            putcharBuf_ += (char)(unsigned char)v.i;
            retReg_ = v;
            return;
        }
        if (target.name == "_setjmp") {
            // Save the current execution state keyed by the jmp_buf address;
            // longjmp restores it. Returns 0 the first time.
            RegVal buf = args.empty() ? RegVal{} : resolveValue(args[0]);
            JumpState st;
            st.frameIndex = (int)frames_.size() - 1;
            const Frame& f = frames_.back();
            st.blockIdx = (int)f.blockIdx;
            st.instrIdx = (int)f.instrIdx;
            st.regs = regs_;
            jumpStates_[buf.i] = std::move(st);
            retReg_ = RegVal{};
            return;
        }
        if (target.name == "longjmp") {
            RegVal buf = args.empty() ? RegVal{} : resolveValue(args[0]);
            RegVal val = args.size() > 1 ? resolveValue(args[1]) : RegVal{};
            auto it = jumpStates_.find(buf.i);
            if (it == jumpStates_.end()) throw VMError{"longjmp: no matching setjmp"};
            const JumpState& st = it->second;
            if (st.frameIndex < 0 || st.frameIndex >= (int)frames_.size())
                throw VMError{"longjmp: frame gone"};
            frames_.resize((size_t)st.frameIndex + 1);
            Frame& f = frames_[(size_t)st.frameIndex];
            f.blockIdx = (size_t)st.blockIdx;
            f.instrIdx = (size_t)st.instrIdx;
            regs_ = st.regs;
            // Deliver the longjmp value as the setjmp call's result.
            if (!f.pendingDest.empty()) {
                regs_[f.pendingDest] = val;
                f.pendingDest.clear();
            }
            return;
        }
        auto it = functionIds_.find(target.name);
        if (it == functionIds_.end()) throw VMError{"call to unknown function " + target.name};
        fid = it->second;
        }
    } else {
        fid = resolveValue(target).i;
    }
    if (fid < 0 || fid >= (int64_t)module_->getFunctions().size()) throw VMError{"call: bad function id"};
    const FunctionIR* fn = module_->getFunctions()[(size_t)fid].get();

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
    if (fid < 0 || fid >= (int64_t)module_->getFunctions().size()) throw VMError{"vcall: bad function id"};
    const FunctionIR* fn = module_->getFunctions()[(size_t)fid].get();

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
            if (rax == 1) {
                // write(fd=rdi, buf=rsi, len=rdx)
                int64_t fd = rdi;
                int64_t len = rdx;
                std::string bytes;
                for (int64_t k = 0; k < len; ++k) {
                    bytes += (char)(rsi + k < (int64_t)heap_.size() ? heap_[(size_t)(rsi + k)] : 0);
                }
                if (fd == 1) result_.stdoutText += bytes;
                else if (fd == 2) result_.stderrText += bytes;
                else {
                    // Real file descriptor (opened via the open syscall).
                    if (rsi + len <= (int64_t)heap_.size()) {
                        rax = (int64_t)::write((int)fd, heap_.data() + rsi, (size_t)len);
                    } else {
                        rax = -1;
                    }
                }
                wrote = true;
            } else if (rax == 60) {
                result_.exitCode = (int)rdi;
                exited = true;
            } else if (rax == 9) {
                // mmap(len=rsi) -> fresh region, returned in rax
                rdi = malloc(rsi);
                rax = rdi;
            } else if (rax == 0) {
                // read(fd=rdi, buf=rsi, len=rdx) — stdin (0) reads the real
                // stdin into the VM heap; any other fd is a real file.
                int64_t len = rdx;
                int64_t got = 0;
                int64_t room = (rsi + len <= (int64_t)heap_.size()) ? len : (int64_t)heap_.size() - rsi;
                if (room > 0) {
                    got = (int64_t)::read((int)rdi, heap_.data() + rsi, (size_t)room);
                }
                rax = got;
            } else {
                // General passthrough for the remaining syscalls (open,
                // close, unlink, lseek, ...): run the real syscall with the
                // heap-relative pointers mapped to real memory.
                auto ptrOf = [&](int64_t v) -> void* {
                    if (v > 0 && v + 8 <= (int64_t)heap_.size()) return heap_.data() + v;
                    return nullptr;
                };
                int64_t result = 0;
                switch (rax) {
                case 2:  // open(path, flags, mode)
                    result = (int64_t)::open((const char*)ptrOf(rdi), (int)rsi, (int)rdx);
                    break;
                case 3:  // close(fd)
                    result = (int64_t)::close((int)rdi);
                    break;
                case 8:  // lseek(fd, offset, whence)
                    result = (int64_t)::lseek((int)rdi, rsi, (int)rdx);
                    break;
                case 87:  // unlink(path)
                    result = (int64_t)::unlink((const char*)ptrOf(rdi));
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
        frames_.clear();
    }
}

}  // namespace cider
