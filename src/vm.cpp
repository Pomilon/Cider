#include "vm.h"

#include <cstring>
#include <sstream>

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
    freeList_.clear();
    stringOffsets_.clear();
    vtableOffsets_.clear();
    retReg_ = RegVal{};
    steps_ = 0;
    heap_.clear();
    heap_.reserve(1 << 20);

    materializeLabels();

    const FunctionIR* entry = nullptr;
    for (const auto& fn : module_->getFunctions()) {
        if (fn->getName() == runFunction_) {
            entry = fn.get();
            break;
        }
    }
    if (!entry) throw VMError{"no function named '" + runFunction + "'"};

    frames_.push_back(Frame{entry, 0, 0, "", ""});

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
                switch (instr.op) {
                case IROp::ADD: d.i = x + y; break;
                case IROp::SUB: d.i = x - y; break;
                case IROp::MUL: d.i = x * y; break;
                case IROp::DIV: d.i = y != 0 ? x / y : 0; break;
                case IROp::MOD: d.i = y != 0 ? x % y : 0; break;
                case IROp::BIT_AND: d.i = x & y; break;
                case IROp::BIT_OR: d.i = x | y; break;
                case IROp::BIT_XOR: d.i = x ^ y; break;
                case IROp::LSHIFT: d.i = x << y; break;
                case IROp::RSHIFT: d.i = x >> y; break;
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
            int64_t offset = 0;
            if (instr.args.size() > 1 && instr.args[1].kind != IRValue::LABEL) {
                offset = resolveValue(instr.args[1]).i;
            }
            IRType type = instr.dest ? instr.dest->type : IRType::I64;
            RegVal d = load(ptr.i + offset, type);
            if (instr.dest) regs_[instr.dest->name] = d;
            break;
        }
        case IROp::STORE: {
            RegVal addr = resolveValue(instr.args[0]);
            RegVal val = resolveValue(instr.args[1]);
            int64_t offset = 0;
            if (instr.args.size() > 2 && instr.args[2].kind != IRValue::LABEL) {
                offset = resolveValue(instr.args[2]).i;
            }
            IRType type = val.isFloat ? IRType::F64 : IRType::I64;
            store(addr.i + offset, val, type);
            break;
        }
        case IROp::ALLOCA: {
            RegVal size = resolveValue(instr.args[0]);
            RegVal d;
            d.i = malloc(size.i);
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
            doCall(instr.args[0], std::vector<IRValue>(instr.args.begin() + 1, instr.args.end()));
            if (instr.dest && frames_.size() >= 2) {
                frames_[frames_.size() - 2].pendingDest = instr.dest->name;
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
            frames_.pop_back();
            if (frames_.empty()) {
                // The entry function returned: its value is the exit code.
                result_.exitCode = (int)retReg_.i;
            } else if (!frames_.back().pendingDest.empty()) {
                regs_[frames_.back().pendingDest] = retReg_;
                frames_.back().pendingDest.clear();
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
            d.i = (int64_t)v.constValue;
            return d;
        }
        if (v.type == IRType::F32 || v.type == IRType::F64) {
            RegVal d;
            d.isFloat = true;
            d.f = v.constValue;
            return d;
        }
        RegVal d;
        d.i = (int64_t)v.constValue;
        return d;
    }
    case IRValue::REGISTER: {
        auto it = regs_.find(v.name);
        if (it == regs_.end()) throw VMError{"read of unset register %" + v.name};
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
    // Pre-materialize every string in stringData (the ir-gen emits global
    // string labels there).
    for (const auto& line : module_->getStringData()) {
        // format: "<label>: db <bytes>, 0"  or "<label>: dq <otherlabel>"
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string label = line.substr(0, colon);
        std::string body = line.substr(colon + 1);
        size_t db = body.find("db");
        if (db == std::string::npos) continue;
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
                // char literal
                char c = body[pos + 1];
                if (c == '\\' && pos + 2 < body.size()) {
                    char e = body[pos + 2];
                    if (e == 'n') c = '\n';
                    else if (e == 't') c = '\t';
                    else if (e == '\\') c = '\\';
                    else if (e == '"') c = '"';
                    pos += 3;
                } else {
                    pos += 2;
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
    int64_t off = malloc((int64_t)content.size());
    if (off + (int64_t)content.size() > (int64_t)heap_.size()) heap_.resize(off + (int64_t)content.size());
    std::memcpy(heap_.data() + off, content.data(), content.size());
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
    vtableOffsets_[label] = off;
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
    case IRType::U32: {
        int32_t v = 0;
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
    case IRType::U32: {
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
        auto it = functionIds_.find(target.name);
        if (it == functionIds_.end()) throw VMError{"call to unknown function " + target.name};
        fid = it->second;
    } else {
        fid = resolveValue(target).i;
    }
    if (fid < 0 || fid >= (int64_t)module_->getFunctions().size()) throw VMError{"call: bad function id"};
    const FunctionIR* fn = module_->getFunctions()[(size_t)fid].get();

    // Bind arguments to the callee's arg_* registers.
    std::map<std::string, RegVal> saved = regs_;
    for (size_t i = 0; i < fn->getParams().size() && i < args.size(); ++i) {
        RegVal v = resolveValue(args[i]);
        if (v.isFloat) {
            saved["arg_" + fn->getParams()[i]] = v;
        } else {
            saved["arg_" + fn->getParams()[i]] = v;
        }
    }
    regs_ = std::move(saved);
    frames_.push_back(Frame{fn, 0, 0, "", ""});
}

void VM::doVirtualCall(const IRValue& obj, int64_t vtableIndex, const std::vector<IRValue>& args) {
    RegVal objPtr = resolveValue(obj);
    int64_t vptr = load(objPtr.i, IRType::I64).i;
    int64_t fid = load(vptr + vtableIndex * 8, IRType::I64).i;
    if (fid < 0 || fid >= (int64_t)module_->getFunctions().size()) throw VMError{"vcall: bad function id"};
    const FunctionIR* fn = module_->getFunctions()[(size_t)fid].get();

    std::map<std::string, RegVal> saved = regs_;
    size_t pi = 0;
    if (!fn->getParams().empty() && fn->getParams()[0] == "this") {
        saved["arg_this"] = objPtr;
        pi = 1;
    }
    for (size_t i = 0; i + pi < fn->getParams().size() && i < args.size(); ++i) {
        saved["arg_" + fn->getParams()[i + pi]] = resolveValue(args[i]);
    }
    regs_ = std::move(saved);
    frames_.push_back(Frame{fn, 0, 0, "", ""});
}

void VM::execAsm(const IRInstruction& instr) {
    if (instr.args.empty() || instr.args[0].kind != IRValue::LABEL) {
        throw VMError{"asm: missing code string"};
    }
    std::string code = instr.args[0].name;

    auto regVal = [&](const std::string& name) -> RegVal {
        if (!name.empty() && name[0] == '{') {
            std::string var = name.substr(1, name.size() - 2);
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

    int64_t rax = 0, rdi = 0, rsi = 0, rdx = 0;
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
            if (dst == "rax") rax = v.i;
            else if (dst == "rdi") rdi = v.i;
            else if (dst == "rsi") rsi = v.i;
            else if (dst == "rdx") rdx = v.i;
            else if (dst == "edi") rdi = (int32_t)v.i;
            else if (dst == "eax") rax = (int32_t)v.i;
            else throw VMError{"unsupported asm: " + line};
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
                else throw VMError{"asm: unsupported fd " + std::to_string(fd)};
                wrote = true;
            } else if (rax == 60) {
                result_.exitCode = (int)rdi;
                exited = true;
            } else if (rax == 9) {
                // mmap(len=rsi) -> fresh region
                rdi = malloc(rsi);
            } else {
                throw VMError{"unsupported asm syscall: rax=" + std::to_string(rax)};
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
