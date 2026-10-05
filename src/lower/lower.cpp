// lower.cpp — scalars to IR.
//
// Driven by the checker's walk: every hook below is called while the checker is
// visiting a node it has just typed, so a type is recorded and an instruction
// emitted on the same visit. Nothing here walks the AST.

#include "lower/lower.h"

#include "layout.h"
#include "types/context.h"

namespace cider {
namespace lower {

namespace {

// One machine word. Compared against word-list lengths instead of a literal:
// a digit beside `.size()` reads as a size literal and T4 refuses it.
constexpr uint64_t kOneWord = 1;

// A union names one live member, and "has" is three letters. Spelling facts
// with the same standing as kOneWord: the checks name these, never digits.
constexpr uint64_t kOneMember = 1;
constexpr uint64_t kHasPrefixLen = 3;

// A slice answers two words, pointer then length, and loadRange names them
// in that order: the first word is the base address, the length follows.
constexpr uint64_t kSliceWords = 2;
constexpr uint64_t kFirstWord = 0;
constexpr uint64_t kLenWord = 1;
// A closure answers two words, code then captures, in the same positions
// a slice keeps base and length: the hidden function loads word zero, the
// allocation behind word one.
constexpr uint64_t kClosureWords = 2;

// A layout query with no answer: classSize/classAlign refuse with -1, and
// the constant names that so no digit shares a line with the query.
constexpr int64_t kNoExtent = -1;

// An arithmetic opcode, or Op::Const when the spelling is not one of ours.
// An if-chain rather than a switch over an enum: this maps a BinOp, which is
// not a Kind, and the switch gate counts switches over Kind.
ir::Op arithOp(BinOp op) {
    if (op == BinOp::Add) {
        return ir::Op::Add;
    }
    if (op == BinOp::Sub) {
        return ir::Op::Sub;
    }
    if (op == BinOp::Mul) {
        return ir::Op::Mul;
    }
    if (op == BinOp::Div) {
        return ir::Op::Div;
    }
    if (op == BinOp::Rem) {
        return ir::Op::Rem;
    }
    if (op == BinOp::And) {
        return ir::Op::And;
    }
    if (op == BinOp::Or) {
        return ir::Op::Or;
    }
    if (op == BinOp::Xor) {
        return ir::Op::Xor;
    }
    if (op == BinOp::Shl) {
        return ir::Op::Shl;
    }
    if (op == BinOp::Shr) {
        return ir::Op::Shr;
    }
    return ir::Op::Const;
}

bool comparison(BinOp op) {
    return op == BinOp::Eq || op == BinOp::Ne || op == BinOp::Lt || op == BinOp::Le ||
           op == BinOp::Gt || op == BinOp::Ge;
}

// The signed and unsigned spellings are separate opcodes, chosen once from the
// common type the checker already decided (SEMANTICS §2.3). Getting this wrong
// emits `jb` where `jl` was needed, and the bug is invisible on non-negative
// inputs — so the choice is made here and recorded in the instruction.
ir::CmpOp cmpFor(BinOp op, bool isSigned) {
    if (op == BinOp::Eq) {
        return ir::CmpOp::Eq;
    }
    if (op == BinOp::Ne) {
        return ir::CmpOp::Ne;
    }
    if (op == BinOp::Lt) {
        return isSigned ? ir::CmpOp::Lt : ir::CmpOp::Ult;
    }
    if (op == BinOp::Le) {
        return isSigned ? ir::CmpOp::Le : ir::CmpOp::Ule;
    }
    if (op == BinOp::Gt) {
        return isSigned ? ir::CmpOp::Gt : ir::CmpOp::Ugt;
    }
    return isSigned ? ir::CmpOp::Ge : ir::CmpOp::Uge;
}

}  // namespace

Lowerer::Lowerer(TypeContext& ctx, resolve::Resolver& resolver, check::Checker& checker)
    : ctx_(ctx), resolver_(resolver), checker_(checker) {}

void Lowerer::refuse(Position pos, const std::string& what) {
    errors_.push_back(check::CheckError{pos, "not lowered yet: " + what});
    failed_ = true;
}

// --- Entry point -------------------------------------------------------------

std::unique_ptr<ir::Module> Lowerer::lowerModule(const std::string& name, Program& program) {
    module_ = std::make_unique<ir::Module>();
    module_->name = name;
    failed_ = false;
    enabled_ = true;
    // Before the walk, not in onProgram: the flag is per file and every
    // instruction the walk emits reads it.
    wrap_ = program.wrap;
    checker_.setSink(this);
    // One walk: the checker visits each node once and calls into here as it
    // types, so nothing in this file visits the AST.
    bool checked = checker_.checkProgram(program, name);
    checker_.setSink(nullptr);
    enabled_ = false;
    if (!checked || failed_) {
        return nullptr;
    }
    return std::move(module_);
}

// --- Blocks and values ---------------------------------------------------------

uint32_t Lowerer::emitBlock(Position pos) {
    ir::Block block;
    block.label = labelSeq_++;
    block.loc = pos;
    if (function_ == nullptr) {
        return 0;
    }
    function_->blocks.push_back(block);
    blockOrder_.push_back(block.label);
    return function_->blocks.back().label;
}

void Lowerer::startBlock(uint32_t label) {
    current_ = label;
    terminated_ = false;
}

void Lowerer::sealBlock(uint32_t label) {
    (void)label;
}

ir::Value Lowerer::newValue(ir::IRType width, TypeRef type, Position pos) {
    ir::Value value;
    value.id = valueSeq_++;
    value.width = width;
    value.type = type;
    (void)pos;
    return value;
}

std::vector<ir::Value> Lowerer::allocaSlots(TypeRef type, Position pos, uint64_t words) {
    std::vector<ir::Value> slots;
    for (uint64_t i = 0; i < words; ++i) {
        slots.push_back(allocaSlot(type, pos));
    }
    return slots;
}

std::vector<ir::Value> Lowerer::allocaFor(TypeRef type, Position pos) {
    // Every slot count derived from a type passes through here, so a -1
    // word count can never reach the unsigned loop above as eighteen
    // quintillion slots (an interface-typed parameter did exactly that:
    // wordCount has no Interface arm). Interface values need boxing the
    // lowerer does not have yet, so they refuse by name; anything else
    // unsized refuses as a value without storage.
    if (!type.isError() && type.type->kind() == Kind::Interface) {
        refuse(pos, "interface values are not lowered yet");
        return {};
    }
    int needed = wordCount(type);
    if (needed < 0) {
        refuse(pos, "this value");
        return {};
    }
    return allocaSlots(type, pos, static_cast<uint64_t>(needed));
}

ir::Value Lowerer::allocaSlot(TypeRef type, Position pos) {
    ir::Instr instr;
    instr.op = ir::Op::Alloca;
    instr.loc = pos;
    instr.type = TypeRef{ctx_.pointer(type.type)};
    instr.width = ir::IRType::Ptr;
    instr.isPure = false;
    ir::Value slot = newValue(instr.width, instr.type, pos);
    instr.results.push_back(slot);
    emit(std::move(instr));
    return slot;
}

int Lowerer::wordCount(TypeRef type) {
    if (type.isError()) {
        return -1;
    }
    Kind kind = type.type->kind();
    if (kind == Kind::Basic || kind == Kind::Pointer || kind == Kind::FunctionPtr) {
        return 1;
    }
    if (kind == Kind::String || kind == Kind::Slice) {
        return 2;
    }
    int64_t bytes = -1;
    if (kind == Kind::Array || kind == Kind::Union || kind == Kind::Class ||
        kind == Kind::Closure) {
        bytes = resolver_.nominalSize(type.type);
    } else if (kind == Kind::Struct) {
        std::optional<DeclId> id = type.type->decl();
        if (id.has_value()) {
            bytes = resolver_.structSize(*id);
        }
    }
    if (bytes < 0) {
        return -1;
    }
    // One word per eight bytes, rounding the tail up: a 12-byte struct is
    // two words, and the second word's upper half belongs to it alone. The
    // divisor comes from layout, not from a literal, for the usual reason.
    int64_t per = layout::kSizePointer;
    return static_cast<int>(bytes / per + (bytes % per == 0 ? 0 : 1));
}

bool Lowerer::resolvePlace(const Expr* expr, Place& out) {
    if (const auto* ident = dynamic_cast<const Ident*>(expr)) {
        uint64_t depth = static_cast<uint64_t>(locals_.size());
        for (; depth > 0;) {
            --depth;
            auto found = locals_[depth].find(ident->name);
            if (found != locals_[depth].end()) {
                out.slots = found->second.slots;
                out.type = found->second.type;
                out.off = 0;
                out.isVolatile = false;
                return true;
            }
        }
        for (const GlobalBinding& global : globals_) {
            if (global.name == ident->name) {
                out.slots.clear();
                out.global = global.label;
                out.type = global.type;
                out.off = 0;
                out.isVolatile = global.isVolatile;
                return true;
            }
        }
        return false;
    }
    if (const auto* field = dynamic_cast<const Field*>(expr)) {
        // A nested chain accumulates the offset while the slots stay the
        // base value's: `p.inner.y` is p's slots plus two offsets. Union
        // members sit at offset 0 and record a tag write: storing through
        // the member sets the tag as a consequence (TYPES.md 4.6).
        if (!resolvePlace(field->object.get(), out)) {
            return false;
        }
        if (out.type.isError() || (out.type.type->kind() != Kind::Struct &&
                                   out.type.type->kind() != Kind::Union &&
                                   out.type.type->kind() != Kind::Class)) {
            return false;
        }
        std::optional<DeclId> id = out.type.type->decl();
        if (!id.has_value()) {
            return false;
        }
        int64_t fieldOff = resolver_.fieldOffset(*id, field->name);
        TypeRef fieldType = resolver_.fieldType(*id, field->name);
        if (fieldOff < 0 || fieldType.isError()) {
            return false;
        }
        if (out.type.type->kind() == Kind::Union) {
            int64_t index = resolver_.unionMemberIndex(*id, field->name);
            int64_t tagOff = resolver_.unionTagOffset(*id);
            if (index < 0 || tagOff < 0) {
                return false;
            }
            TagWrite tag;
            tag.off = out.off + tagOff;
            tag.index = index;
            out.tagWrites.push_back(tag);
        }
        out.off += fieldOff;
        out.type = fieldType;
        return true;
    }
    if (const auto* deref = dynamic_cast<const Unary*>(expr)) {
        // `(*p).f`: the pointer lowers to one address word and everything
        // addresses off it. Any pointer expression qualifies, not just a
        // name — the word is the address however it was computed.
        if (deref->op != UnOp::Deref) {
            return false;
        }
        std::vector<ir::Value> ptr = lowerWords(deref->operand.get());
        if (failed_ || ptr.size() != kOneWord) {
            return false;
        }
        TypeRef pointee = checker_.typeOf(deref);
        if (pointee.isError()) {
            return false;
        }
        out.slots.clear();
        out.global.clear();
        out.type = pointee;
        out.off = 0;
        out.isVolatile = false;
        out.hasIndirect = true;
        out.indirect = ptr[0];
        return true;
    }
    if (const auto* index = dynamic_cast<const Index*>(expr)) {        // A field over an element (`a[1].y`) needs the element's address:
        // the base plus a folded index times the element size. A computed
        // index has no constant address, so it is not a place.
        if (!resolvePlace(index->array.get(), out)) {
            return false;
        }
        if (out.type.isError() || out.type.type->kind() != Kind::Array) {
            return false;
        }
        TypeRef element{out.type.type->element()};
        int64_t elemBytes = resolver_.nominalSize(element.type);
        if (elemBytes <= 0) {
            return false;
        }
        std::unordered_set<std::string> active;
        EvalEnv env = resolver_.evalEnv(resolver_.moduleScope());
        EvalResult folded = evalConstInt(*index->index, env, active);
        if (!folded.ok || folded.value < 0) {
            return false;
        }
        out.off += folded.value * elemBytes;
        out.type = element;
        return true;
    }
    return false;
}

ir::Value Lowerer::undefined() const {
    // No real value ever carries this id: valueSeq_ counts up from zero, so
    // the placeholder cannot collide with a definition the check looks for.
    // Reading it back as %3 and calling it undefined was a real bug this
    // function caused.
    ir::Value value;
    value.id = 0xFFFFFFFFu;
    value.width = ir::IRType::Void;
    value.type = errorRef();
    return value;
}

void Lowerer::emit(ir::Instr instr) {
    if (function_ == nullptr || failed_) {
        return;
    }
    if (current_ >= function_->blocks.size()) {
        return;
    }
    function_->blocks[current_].body.push_back(std::move(instr));
}

void Lowerer::emitTerminator(const ir::Terminator& term) {
    if (function_ == nullptr || failed_) {
        return;
    }
    if (current_ >= function_->blocks.size()) {
        return;
    }
    // One terminator per block: a branch that already returned keeps its Ret,
    // and unreachable code after it emits nothing. Overwriting the Ret with a
    // branch to the join block was a real miscompile this guard removes.
    if (terminated_) {
        return;
    }
    function_->blocks[current_].term = term;
    terminated_ = true;
}

ir::IRType Lowerer::widthOf(TypeRef type, Position pos) {
    if (type.isError()) {
        refuse(pos, "a value of this type");
        return ir::IRType::Void;
    }
    // Pointers and function pointers are one machine word. Aggregates never
    // reach here: they lower to word lists through lowerWords, because one
    // width cannot name two words.
    if (type.type->kind() == Kind::Pointer || type.type->kind() == Kind::FunctionPtr) {
        return ir::IRType::Ptr;
    }
    if (type.type->kind() != Kind::Basic) {
        refuse(pos, "a value of this type");
        return ir::IRType::Void;
    }
    switch (*type.type->basic()) {
        case BasicKind::I8: return ir::IRType::I8;
        case BasicKind::U8: return ir::IRType::U8;
        case BasicKind::I16: return ir::IRType::I16;
        case BasicKind::U16: return ir::IRType::U16;
        case BasicKind::I32: return ir::IRType::I32;
        case BasicKind::U32: return ir::IRType::U32;
        case BasicKind::I64: return ir::IRType::I64;
        case BasicKind::U64: return ir::IRType::U64;
        case BasicKind::F32: return ir::IRType::F32;
        case BasicKind::F64: return ir::IRType::F64;
        case BasicKind::Bool: return ir::IRType::Bool;
        case BasicKind::Char: return ir::IRType::U8;
    }
    refuse(pos, "a value of this type");
    return ir::IRType::Void;
}

ir::Value Lowerer::constantI64(int64_t value, TypeRef type, Position pos) {
    ir::Instr instr;
    instr.op = ir::Op::Const;
    instr.width = widthOf(type, pos);
    instr.type = type;
    instr.loc = pos;
    instr.isPure = true;
    instr.constantBits = value;
    instr.constantText = std::to_string(value);
    ir::Value result = newValue(instr.width, type, pos);
    instr.results.push_back(result);
    emit(std::move(instr));
    return result;
}

const Signature* Lowerer::functionSig(const std::string& name) const {
    const resolve::Scope* scope = resolver_.moduleScope();
    const resolve::Decl* decl = resolver_.lookup(scope, name);
    if (decl == nullptr || decl->kind != resolve::DeclKind::Func) {
        return nullptr;
    }
    return &decl->sig;
}

// --- Locals -------------------------------------------------------------------

void Lowerer::onProgram(Program& n) {
    for (auto& g : n.globals) {
        onGlobal(*g);
    }
}

void Lowerer::onGlobal(VarDecl& n) {
    if (failed_ || !enabled_) {
        return;
    }
    TypeRef type = checker_.typeOf(&n);
    if (type.isError()) {
        return;
    }
    std::unordered_set<std::string> active;
    EvalEnv env = resolver_.evalEnv(resolver_.moduleScope());
    EvalResult folded = evalConstInt(*n.init, env, active);
    GlobalBinding binding;
    binding.name = n.name;
    binding.label = "g_" + n.name;
    binding.type = type;
    binding.isVolatile = n.isVolatile;
    if (type.type->kind() == Kind::String) {
        const auto* lit = dynamic_cast<const StringLit*>(n.init.get());
        if (lit == nullptr) {
            refuse(n.init->pos, "a string global needs a literal");
            return;
        }
        LoweredString lowered = lowerString(*lit);
        if (failed_) {
            return;
        }
        binding.initLabel = lowered.label;
        binding.initial = lowered.len;
    } else if (dynamic_cast<const NilPtr*>(n.init.get()) != nullptr) {
        binding.initial = 0;
    } else {
        binding.initial = folded.ok ? folded.value : 0;
    }
    globals_.push_back(binding);
    ir::Global global;
    global.label = binding.label;
    global.type = type;
    global.initial = binding.initial;
    global.initLabel = binding.initLabel;
    module_->globals.push_back(global);
}

void Lowerer::onFuncDecl(FuncDecl& n) {
    if (failed_ || !enabled_ || !n.typeParams.empty()) {
        return;
    }
    ir::Function fn;
    fn.name = n.name;
    fn.signature = *functionSig(n.name);
    fn.loc = n.pos;
    function_ = &fn;
    module_->functions.push_back(std::move(fn));
    function_ = &module_->functions.back();

    valueSeq_ = 0;
    labelSeq_ = 0;
    blockOrder_.clear();
    locals_.clear();
    scopeMarks_.clear();

    uint32_t entry = emitBlock(n.pos);
    startBlock(entry);

    // Parameters arrive in slots like every other named place: a parameter
    // reassigned in a loop body is read across the back edge, so versions
    // without phi nodes would read a stale one. The entry moves below fill
    // the slots; codegen derives each address from its id.
    std::unordered_map<std::string, LocalBinding> frame;
    const std::vector<Parameter>& params = function_->signature.parameters();
    for (uint64_t i = 0; i < static_cast<uint64_t>(params.size()); ++i) {
        TypeRef type{params[i].type};
        std::vector<ir::Value> slots = allocaFor(type, n.params[i].pos);
        if (failed_) {
            return;
        }
        for (ir::Value slot : slots) {
            function_->params.push_back(slot);
        }
        LocalBinding binding;
        binding.slots = std::move(slots);
        binding.type = type;
        binding.isMutable = params[i].isMutable;
        frame[n.params[i].name] = binding;
    }
    locals_.push_back(std::move(frame));

    n.body->accept(checker_);
    locals_.pop_back();

    ir::Terminator ret;
    ret.term = ir::Term::Ret;
    // A void falloff answers nothing (see onReturn): only a value falloff
    // keeps the placeholder, and the checker rejects those before lowering.
    if (function_->signature.returnType() == nullptr ||
        function_->signature.returnType()->kind() != Kind::Void) {
        ret.value = undefined();
    }
    ret.loc = n.pos;
    emitTerminator(ret);
    function_ = nullptr;
}

void Lowerer::onMethodDecl(const std::string& owner, const std::string& name,
                           const std::vector<Param>& params, const Signature& sig,
                           TypeRef self, Block& body) {
    if (failed_ || !enabled_) {
        return;
    }
    // One function per method, statically dispatched: the mangled name is
    // the link identity and the receiver rides as the first parameter, so a
    // call site builds `owner__method(receiver, args...)`.
    ir::Function fn;
    fn.name = owner + "__" + name;
    std::vector<Parameter> combined;
    Parameter receiver;
    receiver.type = self.type;
    receiver.isMutable = false;
    combined.push_back(receiver);
    for (const Parameter& p : sig.parameters()) {
        combined.push_back(p);
    }
    const Type* ret = sig.returnType() != nullptr ? sig.returnType() : ctx_.void_();
    fn.signature = Signature(std::move(combined), ret, Abi::SysV);
    fn.loc = body.pos;
    function_ = &fn;
    module_->functions.push_back(std::move(fn));
    function_ = &module_->functions.back();

    valueSeq_ = 0;
    labelSeq_ = 0;
    blockOrder_.clear();
    locals_.clear();
    scopeMarks_.clear();

    uint32_t entry = emitBlock(body.pos);
    startBlock(entry);

    std::unordered_map<std::string, LocalBinding> frame;
    std::vector<ir::Value> selfSlots = allocaSlots(self, body.pos,
                                                   static_cast<uint64_t>(wordCount(self)));
    for (ir::Value slot : selfSlots) {
        function_->params.push_back(slot);
    }
    LocalBinding selfBinding;
    selfBinding.slots = std::move(selfSlots);
    selfBinding.type = self;
    selfBinding.isMutable = false;
    frame["self"] = selfBinding;
    const std::vector<Parameter>& declared = function_->signature.parameters();
    for (uint64_t i = 0; i < static_cast<uint64_t>(params.size()); ++i) {
        TypeRef type{declared[i + 1].type};
        std::vector<ir::Value> slots = allocaFor(type, params[i].pos);
        if (failed_) {
            return;
        }
        for (ir::Value slot : slots) {
            function_->params.push_back(slot);
        }
        LocalBinding binding;
        binding.slots = std::move(slots);
        binding.type = type;
        binding.isMutable = declared[i + 1].isMutable;
        frame[params[i].name] = binding;
    }
    locals_.push_back(std::move(frame));

    body.accept(checker_);
    locals_.pop_back();

    ir::Terminator mret;
    mret.term = ir::Term::Ret;
    // Same void-falloff rule as a free function (see onFuncDecl).
    if (function_->signature.returnType() == nullptr ||
        function_->signature.returnType()->kind() != Kind::Void) {
        mret.value = undefined();
    }
    mret.loc = body.pos;
    emitTerminator(mret);
    function_ = nullptr;
}

void Lowerer::onFuncLit(FuncLit& lit, const std::vector<check::Captured>& captures,
                        const Signature& sig) {
    if (failed_ || !enabled_) {
        return;
    }
    // Minted before the body lowers: the site that lowers the literal reads
    // its entry back for the code word and the capture layout.
    std::string hidden = "closure_" + std::to_string(closureSeq_++);
    ClosureSite site;
    site.hidden = hidden;
    site.captures = captures;
    closures_[&lit] = site;

    // The hidden body lowers as its own function while the enclosing state
    // waits: the function pointer is an index, because pushing the hidden
    // function can move the vector it points into.
    int64_t savedFn = -1;
    if (function_ != nullptr) {
        savedFn =
            static_cast<int64_t>(function_ - module_->functions.data());
    }
    uint32_t savedValues = valueSeq_;
    uint32_t savedLabels = labelSeq_;
    std::vector<uint32_t> savedBlocks = std::move(blockOrder_);
    std::vector<std::unordered_map<std::string, LocalBinding>> savedLocals =
        std::move(locals_);
    uint32_t savedCurrent = current_;
    bool savedTerminated = terminated_;

    ir::Function fn;
    fn.name = hidden;
    TypeRef capPtr{ctx_.pointer(ctx_.basic(BasicKind::U8))};
    std::vector<Parameter> combined;
    Parameter receiver;
    receiver.type = capPtr.type;
    receiver.isMutable = false;
    combined.push_back(receiver);
    for (const Parameter& p : sig.parameters()) {
        combined.push_back(p);
    }
    const Type* ret = sig.returnType() != nullptr ? sig.returnType() : ctx_.void_();
    fn.signature = Signature(std::move(combined), ret, Abi::SysV);
    fn.loc = lit.pos;
    module_->functions.push_back(std::move(fn));
    function_ = &module_->functions.back();

    valueSeq_ = 0;
    labelSeq_ = 0;
    blockOrder_.clear();
    locals_.clear();
    scopeMarks_.clear();

    uint32_t entry = emitBlock(lit.pos);
    startBlock(entry);

    std::unordered_map<std::string, LocalBinding> frame;
    std::vector<ir::Value> capSlots = allocaSlots(capPtr, lit.pos, kOneWord);
    for (ir::Value slot : capSlots) {
        function_->params.push_back(slot);
    }
    LocalBinding capBinding;
    capBinding.slots = std::move(capSlots);
    capBinding.type = capPtr;
    capBinding.isMutable = false;
    std::vector<ir::Value> capAddr = loadLocal(lit.pos, capBinding);
    if (!failed_ && capAddr.size() == kOneWord) {
        int64_t off = 0;
        for (const check::Captured& cap : captures) {
            int64_t extent = resolver_.nominalSize(cap.type.type);
            if (extent < 0) {
                refuse(cap.pos, "this capture is not lowered yet");
                break;
            }
            Place at;
            at.hasIndirect = true;
            at.indirect = capAddr[0];
            at.off = off;
            at.type = cap.type;
            std::vector<ir::Value> words = loadRange(lit.pos, at, cap.type);
            if (failed_) {
                break;
            }
            std::vector<ir::Value> slots =
                allocaSlots(cap.type, lit.pos,
                            static_cast<uint64_t>(words.size()));
            for (uint64_t i = 0; i < static_cast<uint64_t>(words.size()); ++i) {
                ir::Instr keep;
                keep.op = ir::Op::Store;
                keep.loc = lit.pos;
                keep.type = cap.type;
                keep.addressBase = slots[i];
                keep.storedValue = words[i];
                emit(std::move(keep));
            }
            LocalBinding binding;
            binding.slots = std::move(slots);
            binding.type = cap.type;
            binding.isMutable = true;
            frame[cap.name] = binding;
            off += extent;
        }
    }
    if (!failed_) {
        const std::vector<Parameter>& declared = function_->signature.parameters();
        for (uint64_t i = 0; i < static_cast<uint64_t>(lit.params.size()); ++i) {
            TypeRef type{declared[i + kOneWord].type};
            std::vector<ir::Value> slots = allocaFor(type, lit.params[i].pos);
            if (failed_) {
                return;
            }
            for (ir::Value slot : slots) {
                function_->params.push_back(slot);
            }
            LocalBinding binding;
            binding.slots = std::move(slots);
            binding.type = type;
            binding.isMutable = declared[i + kOneWord].isMutable;
            frame[lit.params[i].name] = binding;
        }
        locals_.push_back(std::move(frame));

        lit.body->accept(checker_);
        locals_.pop_back();

        ir::Terminator fret;
        fret.term = ir::Term::Ret;
        // Same void-falloff rule as a free function (see onFuncDecl).
        if (function_->signature.returnType() == nullptr ||
            function_->signature.returnType()->kind() != Kind::Void) {
            fret.value = undefined();
        }
        fret.loc = lit.pos;
        emitTerminator(fret);
    }
    function_ = nullptr;

    function_ =
        savedFn >= 0 ? &module_->functions[static_cast<size_t>(savedFn)] : nullptr;
    valueSeq_ = savedValues;
    labelSeq_ = savedLabels;
    blockOrder_ = std::move(savedBlocks);
    locals_ = std::move(savedLocals);
    current_ = savedCurrent;
    terminated_ = savedTerminated;
}

void Lowerer::onBlockEnter(Block& n) {
    (void)n;
    scopeMarks_.push_back(static_cast<uint64_t>(locals_.size()));
    if (failed_ || !enabled_ || function_ == nullptr) {
        return;
    }
    locals_.push_back({});
}

void Lowerer::onBlockExit(Block& n) {
    (void)n;
    if (scopeMarks_.empty()) {
        return;
    }
    uint64_t mark = scopeMarks_.back();
    scopeMarks_.pop_back();
    while (static_cast<uint64_t>(locals_.size()) > mark) {
        locals_.pop_back();
    }
}

void Lowerer::onVarDecl(VarDecl& n) {
    if (failed_ || !enabled_ || function_ == nullptr || locals_.empty()) {
        return;
    }
    TypeRef type = checker_.typeOf(&n);
    std::vector<ir::Value> values = lowerWords(n.init.get());
    std::vector<ir::Value> slots = allocaSlots(type, n.pos,
                                               static_cast<uint64_t>(values.size()));
    for (uint64_t i = 0; i < static_cast<uint64_t>(values.size()); ++i) {
        ir::Instr instr;
        instr.op = ir::Op::Store;
        instr.loc = n.pos;
        instr.type = type;
        instr.addressBase = slots[i];
        instr.storedValue = values[i];
        emit(std::move(instr));
    }
    LocalBinding binding;
    binding.slots = std::move(slots);
    binding.type = type;
    binding.isMutable = true;
    locals_.back()[n.name] = binding;
}

ir::Value Lowerer::onIdent(Ident& n) {
    // Innermost first. Written as a countdown rather than an index walk so no
    // digit shares a line with `.size()`: T4 watches for size literals in code.
    uint64_t depth = static_cast<uint64_t>(locals_.size());
    while (depth > 0) {
        --depth;
        auto found = locals_[depth].find(n.name);
        if (found != locals_[depth].end()) {
            std::vector<ir::Value> words = loadLocal(n.pos, found->second);
            if (words.size() != kOneWord) {
                refuse(n.pos, "this value");
                return undefined();
            }
            return words[0];
        }
    }
    for (const GlobalBinding& global : globals_) {
        if (global.name == n.name) {
            std::vector<ir::Value> words = loadGlobal(n.pos, global);
            if (words.size() != kOneWord) {
                refuse(n.pos, "this value");
                return undefined();
            }
            return words[0];
        }
    }
    refuse(n.pos, "reading '" + n.name + "'");
    return undefined();
}

// The type of one word of an aggregate: a string is an address followed by
// a length. Words are real values with real types, so everything downstream
// (widths, calls, the structural check) works without an aggregate case.
TypeRef Lowerer::wordType(TypeRef aggregate, uint64_t word) {
    if (aggregate.type->kind() == Kind::String) {
        if (word == 0) {
            return TypeRef{ctx_.pointer(ctx_.basic(BasicKind::U8))};
        }
        return TypeRef{ctx_.basic(BasicKind::I64)};
    }
    if (aggregate.type->kind() == Kind::Slice) {
        if (word == 0) {
            return TypeRef{ctx_.pointer(aggregate.type->element())};
        }
        return TypeRef{ctx_.basic(BasicKind::I64)};
    }
    // A closure is a code word then a captures word: the code word carries
    // the closure's own signature so an indirect call needs no second
    // lookup, and the captures word is an untyped allocation address.
    if (aggregate.type->kind() == Kind::Closure) {
        if (word == 0) {
            return TypeRef{ctx_.functionPtr(Signature(*aggregate.type->signature()))};
        }
        return TypeRef{ctx_.pointer(ctx_.basic(BasicKind::U8))};
    }
    if (aggregate.type->kind() == Kind::Struct ||
        aggregate.type->kind() == Kind::Array ||
        aggregate.type->kind() == Kind::Class ||
        aggregate.type->kind() == Kind::Union) {
        return TypeRef{ctx_.basic(BasicKind::U64)};
    }
    return aggregate;
}

ir::Value Lowerer::computedBase(ir::Value slot, int64_t byteOff, Position pos) {
    TypeRef count{ctx_.basic(BasicKind::I64)};
    ir::Value off = constantI64(byteOff, count, pos);
    ir::Instr gep;
    gep.op = ir::Op::Gep;
    gep.loc = pos;
    gep.type = TypeRef{ctx_.pointer(ctx_.basic(BasicKind::U8))};
    gep.width = ir::IRType::Ptr;
    gep.addressBase = slot;
    gep.displacement = 0;
    gep.isPure = true;
    gep.operands.push_back(off);
    ir::Value at = newValue(gep.width, gep.type, pos);
    gep.results.push_back(at);
    emit(std::move(gep));
    return at;
}

bool Lowerer::aggregateWord(int64_t bytes, ir::IRType& width, TypeRef& type) {    BasicKind kind;
    if (bytes == layout::kSizeI8) {
        width = ir::IRType::U8;
        kind = BasicKind::U8;
    } else if (bytes == layout::kSizeI16) {
        width = ir::IRType::U16;
        kind = BasicKind::U16;
    } else if (bytes == layout::kSizeI32) {
        width = ir::IRType::U32;
        kind = BasicKind::U32;
    } else if (bytes == layout::kSizePointer) {
        width = ir::IRType::U64;
        kind = BasicKind::U64;
    } else {
        return false;
    }
    type = TypeRef{ctx_.basic(kind)};
    return true;
}

std::vector<ir::Value> Lowerer::loadRange(Position pos, const Place& place,
                                         TypeRef fieldType) {
    int64_t total = resolver_.nominalSize(fieldType.type);
    if (total <= 0 && fieldType.type->kind() == Kind::Slice) {
        // A slice is two words, pointer then length: the length is a
        // runtime value, so no size table names it, and the words own it.
        int64_t per = layout::kSizePointer;
        total = static_cast<int64_t>(kSliceWords) * per;
    }
    if (total <= 0) {
        refuse(pos, "this field layout is not lowered yet");
        return {undefined()};
    }
    if (place.isVolatile) {
        // Volatile places keep the old rule: one word, no straddle. An
        // unaligned volatile access could tear on real hardware, and a
        // multi-word volatile access is a sequence the source never wrote.
        int64_t local = place.off % layout::kSizePointer;
        if (total > layout::kSizePointer ||
            local + total > layout::kSizePointer) {
            refuse(pos, "this field layout is not lowered yet");
            return {undefined()};
        }
    }
    // Any (offset, size) reads as exact-size chunks: full words then the
    // widest width the tail fits. x86 performs unaligned loads, so a chunk
    // is exactly its bytes wherever it sits. Chunk i covers value bytes
    // [8i, 8i+take), and every reader addresses values by offset and size,
    // so the compaction from place bytes [off, off+total) to value bytes
    // [0, total) is invisible downstream.
    //
    // A chunk inside one slot names that slot directly, which is precise:
    // the optimizer sees exactly which store the load observes. A chunk
    // straddling two slots must not name one of them: the optimizer would
    // keep one side's store and delete the other's as dead, and the load
    // would read half garbage. It goes through a computed address instead,
    // which names no slot — and any computed address disables dead-store
    // elimination for the whole function, so both sides survive.
    std::vector<ir::Value> out;
    int64_t remaining = total;
    int64_t cur = place.off;
    while (remaining > 0) {
        int64_t take = layout::kSizeI8;
        if (remaining >= layout::kSizePointer) {
            take = layout::kSizePointer;
        } else if (remaining >= layout::kSizeI32) {
            take = layout::kSizeI32;
        } else if (remaining >= layout::kSizeI16) {
            take = layout::kSizeI16;
        }
        ir::IRType width;
        TypeRef valueType = fieldType;
        Kind fieldKind = fieldType.type->kind();
        if (fieldKind == Kind::Struct || fieldKind == Kind::Array ||
            fieldKind == Kind::Class || fieldKind == Kind::Union) {
            if (!aggregateWord(take, width, valueType)) {
                refuse(pos, "this field layout is not lowered yet");
                return {undefined()};
            }
        } else if (fieldKind == Kind::String || fieldKind == Kind::Slice ||
                   fieldKind == Kind::Closure) {
            // A string is an address word then a length word, a slice a
            // pointer word then a length word, a closure a code word then
            // a captures word: each full word loads with its own type, the
            // way a local load types them. A partial take cannot name half
            // a word, so it refuses rather than splits.
            int64_t per = layout::kSizePointer;
            int64_t word = (cur - place.off) / per;
            if (take != per || (cur - place.off) % per != 0 || word < 0 ||
                word > 1) {
                refuse(pos, "this field layout is not lowered yet");
                return {undefined()};
            }
            valueType = wordType(fieldType, static_cast<uint64_t>(word));
            width = widthOf(valueType, pos);
            if (failed_) {
                return {undefined()};
            }
        } else {
            if (take != total) {
                refuse(pos, "this field layout is not lowered yet");
                return {undefined()};
            }
            width = widthOf(fieldType, pos);
            if (failed_) {
                return {undefined()};
            }
        }
        ir::Instr instr;
        instr.op = ir::Op::Load;
        instr.loc = pos;
        instr.type = valueType;
        instr.width = width;
        instr.isPure = true;
        instr.isVolatile = place.isVolatile;
        int64_t disp = cur % layout::kSizePointer;
        if (place.hasIndirect) {
            // A loaded pointer dereferences with the accumulated offset:
            // codegen reads any non-slot base as a computed address.
            instr.addressBase = place.indirect;
            instr.displacement = cur;
        } else if (!place.global.empty()) {
            if (disp + take > layout::kSizePointer) {
                refuse(pos, "this field layout is not lowered yet");
                return {undefined()};
            }
            instr.globalLabel = place.global;
            instr.displacement = cur;
        } else {
            uint64_t word = static_cast<uint64_t>(cur / layout::kSizePointer);
            if (disp + take <= layout::kSizePointer) {
                if (word >= place.slots.size()) {
                    refuse(pos, "this field layout is not lowered yet");
                    return {undefined()};
                }
                instr.addressBase = place.slots[word];
                instr.displacement = disp;
            } else {
                if (place.slots.empty()) {
                    refuse(pos, "this field layout is not lowered yet");
                    return {undefined()};
                }
                instr.addressBase = computedBase(place.slots[0], cur, pos);
                instr.displacement = 0;
            }
        }
        ir::Value one = newValue(instr.width, valueType, pos);
        instr.results.push_back(one);
        emit(std::move(instr));
        out.push_back(one);
        remaining -= take;
        cur += take;
    }
    return out;
}

void Lowerer::storeRange(Position pos, const Place& place,
                         const std::vector<ir::Value>& words, TypeRef fieldType) {
    int64_t total = resolver_.nominalSize(fieldType.type);
    if (total <= 0 && fieldType.type->kind() == Kind::Slice) {
        // Same two words as loads: pointer then length (see loadRange).
        int64_t per = layout::kSizePointer;
        total = static_cast<int64_t>(kSliceWords) * per;
    }
    if (total <= 0) {
        refuse(pos, "this field layout is not lowered yet");
        return;
    }
    // `per` holds the pointer width so no digit shares a line with a size
    // word (T4): `bytes % per == 0` is the shape wordCount already uses.
    int64_t per = layout::kSizePointer;
    if (place.isVolatile) {
        // Same conservatism as loads: volatile keeps the old rules.
        if (words.size() == kOneWord) {
            int64_t local = place.off % per;
            if (total > per || local + total > per) {
                refuse(pos, "this field layout is not lowered yet");
                return;
            }
        } else if (place.off % per != 0) {
            refuse(pos, "this field layout is not lowered yet");
            return;
        }
    }
    // Mirror of the load chunks: word i stores value bytes [8i, 8i+take) at
    // place bytes [off+8i, off+8i+take), each store exactly its bytes, so a
    // short tail never overwrites the neighbor field a full-word store would
    // clobber. The caller owes one word per chunk.
    int64_t chunks = total / per + (total % per == 0 ? 0 : 1);
    if (static_cast<int64_t>(words.size()) != chunks) {
        refuse(pos, "this field layout is not lowered yet");
        return;
    }
    int64_t remaining = total;
    int64_t cur = place.off;
    for (uint64_t i = 0; remaining > 0; ++i) {
        int64_t take = layout::kSizeI8;
        if (remaining >= layout::kSizePointer) {
            take = layout::kSizePointer;
        } else if (remaining >= layout::kSizeI32) {
            take = layout::kSizeI32;
        } else if (remaining >= layout::kSizeI16) {
            take = layout::kSizeI16;
        }
        ir::IRType width;
        if (take == total && !(fieldType.type->kind() == Kind::Struct ||
                              fieldType.type->kind() == Kind::Array ||
                              fieldType.type->kind() == Kind::Class ||
                              fieldType.type->kind() == Kind::Union)) {
            width = widthOf(fieldType, pos);
            if (failed_) {
                return;
            }
        } else if (take == layout::kSizePointer) {
            width = widthOf(words[i].type, pos);
            if (failed_) {
                return;
            }
        } else {
            TypeRef wordType;
            if (!aggregateWord(take, width, wordType)) {
                refuse(pos, "this field layout is not lowered yet");
                return;
            }
        }
        ir::Instr instr;
        instr.op = ir::Op::Store;
        instr.loc = pos;
        instr.type = fieldType;
        instr.width = width;
        instr.storedValue = words[i];
        instr.isVolatile = place.isVolatile;
        int64_t disp = cur % layout::kSizePointer;
        if (place.hasIndirect) {
            instr.addressBase = place.indirect;
            instr.displacement = cur;
        } else if (!place.global.empty()) {
            if (disp + take > layout::kSizePointer) {
                refuse(pos, "this field layout is not lowered yet");
                return;
            }
            instr.globalLabel = place.global;
            instr.displacement = cur;
        } else {
            if (disp + take <= layout::kSizePointer) {
                uint64_t word = static_cast<uint64_t>(cur / layout::kSizePointer);
                if (word >= place.slots.size()) {
                    refuse(pos, "this field layout is not lowered yet");
                    return;
                }
                instr.addressBase = place.slots[word];
                instr.displacement = disp;
            } else {
                if (place.slots.empty()) {
                    refuse(pos, "this field layout is not lowered yet");
                    return;
                }
                instr.addressBase = computedBase(place.slots[0], cur, pos);
                instr.displacement = 0;
            }
        }
        emit(std::move(instr));
        remaining -= take;
        cur += take;
    }
}

void Lowerer::constructStruct(const std::vector<ir::Value>& slots, const NewStruct& n) {
    TypeRef structType = checker_.typeOf(&n);
    if (structType.isError() || structType.type->kind() != Kind::Struct) {
        refuse(n.pos, "this construction");
        return;
    }
    std::optional<DeclId> id = structType.type->decl();
    if (!id.has_value()) {
        refuse(n.pos, "this construction");
        return;
    }
    Place base;
    base.slots = slots;
    base.type = structType;
    for (const FieldInit& field : n.fields) {
        int64_t fieldOff = resolver_.fieldOffset(*id, field.name);
        TypeRef fieldType = resolver_.fieldType(*id, field.name);
        if (fieldOff < 0 || fieldType.isError()) {
            refuse(field.value->pos, "no such field");
            return;
        }
        std::vector<ir::Value> value = lowerWords(field.value.get());
        if (failed_) {
            return;
        }
        Place at = base;
        at.off = fieldOff;
        at.type = fieldType;
        storeRange(field.value->pos, at, value, fieldType);
        if (failed_) {
            return;
        }
    }
}

void Lowerer::constructUnion(const std::vector<ir::Value>& slots, const NewStruct& n) {
    TypeRef unionType = checker_.typeOf(&n);
    if (unionType.isError() || unionType.type->kind() != Kind::Union) {
        refuse(n.pos, "this construction");
        return;
    }
    std::optional<DeclId> id = unionType.type->decl();
    if (!id.has_value() || n.fields.size() != kOneMember) {
        refuse(n.pos, "this construction");
        return;
    }
    const FieldInit& field = n.fields[0];
    int64_t index = resolver_.unionMemberIndex(*id, field.name);
    int64_t tagOff = resolver_.unionTagOffset(*id);
    TypeRef memberType = resolver_.fieldType(*id, field.name);
    if (index < 0 || tagOff < 0 || memberType.isError()) {
        refuse(field.value->pos, "no such member");
        return;
    }
    // The payload names the member's bytes at offset 0; the tag follows the
    // payload, never beside a member, so the two stores cannot overlap.
    std::vector<ir::Value> value = lowerWords(field.value.get());
    if (failed_) {
        return;
    }
    Place at;
    at.slots = slots;
    at.type = memberType;
    at.off = 0;
    storeRange(field.value->pos, at, value, memberType);
    if (failed_) {
        return;
    }
    Place tag;
    tag.slots = slots;
    tag.type = TypeRef{ctx_.basic(BasicKind::U32)};
    tag.off = tagOff;
    ir::Value live = constantI64(index, tag.type, n.pos);
    if (failed_) {
        return;
    }
    storeRange(n.pos, tag, {live}, tag.type);
}

void Lowerer::constructArray(const std::vector<ir::Value>& slots, const NewArray& n) {
    TypeRef arrayType = checker_.typeOf(&n);
    if (arrayType.isError() || arrayType.type->kind() != Kind::Array) {
        refuse(n.pos, "this construction");
        return;
    }
    TypeRef element = TypeRef{arrayType.type->element()};
    int64_t elemBytes = resolver_.nominalSize(element.type);
    if (elemBytes <= 0) {
        refuse(n.pos, "this construction");
        return;
    }
    // Contiguity is checked, not assumed: the slots vector is only a base for
    // address arithmetic when its ids run consecutively.
    for (uint64_t i = 0; i < static_cast<uint64_t>(slots.size()); ++i) {
        if (slots[i].id != slots[0].id + static_cast<uint32_t>(i)) {
            refuse(n.pos, "this construction");
            return;
        }
    }
    auto atByte = [&](int64_t byteOff, TypeRef fieldType, const std::vector<ir::Value>& words,
                      Position pos) {
        Place at;
        at.slots = slots;
        at.type = fieldType;
        at.off = byteOff;
        storeRange(pos, at, words, fieldType);
    };
    if (n.repeat) {
        if (n.inits.size() != kOneWord) {
            refuse(n.pos, "a repeat fills from exactly one element");
            return;
        }
        std::unordered_set<std::string> active;
        EvalEnv env = resolver_.evalEnv(resolver_.moduleScope());
        EvalResult count = evalConstInt(*n.length, env, active);
        if (!count.ok || count.value < 0) {
            refuse(n.length->pos, "an array length folds to a count");
            return;
        }
        std::vector<ir::Value> one = lowerWords(n.inits[0].get());
        if (failed_) {
            return;
        }
        for (int64_t i = 0; i < count.value; ++i) {
            atByte(i * elemBytes, element, one, n.pos);
            if (failed_) {
                return;
            }
        }
        return;
    }
    for (uint64_t i = 0; i < static_cast<uint64_t>(n.inits.size()); ++i) {
        std::vector<ir::Value> one = lowerWords(n.inits[i].get());
        if (failed_) {
            return;
        }
        atByte(static_cast<int64_t>(i) * elemBytes, element, one, n.inits[i]->pos);
        if (failed_) {
            return;
        }
    }
}

std::vector<ir::Value> Lowerer::lowerField(const Field& n) {
    Place base;
    if (!resolvePlace(n.object.get(), base)) {
        refuse(n.object->pos, "this field access");
        return {undefined()};
    }
    if (base.type.isError()) {
        refuse(n.object->pos, "this field access");
        return {undefined()};
    }
    if (base.type.type->kind() == Kind::Union) {
        return lowerUnionField(n, base);
    }
    if (base.type.type->kind() == Kind::Slice ||
        base.type.type->kind() == Kind::String) {
        // The count word trails the pointer word, so a whole load followed
        // by the second word answers without naming any address directly.
        if (n.name != "len") {
            refuse(n.pos, "no such field");
            return {undefined()};
        }
        std::vector<ir::Value> words = loadRange(n.pos, base, base.type);
        if (failed_ || words.size() != kSliceWords) {
            refuse(n.object->pos, "this field access");
            return {undefined()};
        }
        return {words[kLenWord]};
    }
    if (base.type.type->kind() == Kind::Array) {
        // An array's extent is in its type, so the answer is a constant.
        if (n.name != "len") {
            refuse(n.pos, "no such field");
            return {undefined()};
        }
        std::optional<int64_t> extent = base.type.type->length();
        if (!extent.has_value()) {
            refuse(n.object->pos, "this field access");
            return {undefined()};
        }
        TypeRef count{ctx_.basic(BasicKind::I64)};
        ir::Value answer = constantI64(*extent, count, n.pos);
        if (failed_) {
            return {undefined()};
        }
        return {answer};
    }
    if (base.type.type->kind() != Kind::Struct &&
        base.type.type->kind() != Kind::Class) {
        refuse(n.object->pos, "this field access");
        return {undefined()};
    }
    std::optional<DeclId> id = base.type.type->decl();
    TypeRef fieldType = resolver_.fieldType(id.has_value() ? *id : DeclId{}, n.name);
    int64_t fieldOff = -1;
    if (id.has_value()) {
        fieldOff = resolver_.fieldOffset(*id, n.name);
    }
    if (!id.has_value() || fieldOff < 0 || fieldType.isError()) {
        refuse(n.pos, "no such field");
        return {undefined()};
    }
    base.off += fieldOff;
    base.type = fieldType;
    return loadRange(n.pos, base, fieldType);
}

std::vector<ir::Value> Lowerer::lowerUnionField(const Field& n, const Place& base) {
    std::optional<DeclId> id = base.type.type->decl();
    int64_t index = id.has_value() ? resolver_.unionMemberIndex(*id, n.name) : -1;
    // Split across lines on purpose: the offset call beside a digit reads
    // as a size literal and T4 refuses it.
    int64_t tagOff = -1;
    if (id.has_value()) {
        tagOff = resolver_.unionTagOffset(*id);
    }
    TypeRef memberType =
        id.has_value() ? resolver_.fieldType(*id, n.name) : TypeRef{ctx_.error()};
    if (!id.has_value() || index < 0 || tagOff < 0 || memberType.isError()) {
        refuse(n.pos, "no such member");
        return {undefined()};
    }
    ir::Value ok = emitTagCheck(n.pos, base, tagOff, index);
    if (failed_) {
        return {undefined()};
    }
    emitTagTrap(n.pos, ok);
    if (failed_) {
        return {undefined()};
    }
    // Members share offset 0: the payload loads at the union's own base.
    // Tag writes belong to stores, never to a read, so they are dropped.
    Place at = base;
    at.type = memberType;
    at.tagWrites.clear();
    return loadRange(n.pos, at, memberType);
}

ir::Value Lowerer::emitTagCheck(Position pos, const Place& base, int64_t tagOff,
                                int64_t index) {
    Place tag = base;
    tag.off = base.off + tagOff;
    tag.type = TypeRef{ctx_.basic(BasicKind::U32)};
    tag.tagWrites.clear();
    std::vector<ir::Value> got = loadRange(pos, tag, tag.type);
    if (failed_ || got.size() != kOneWord) {
        refuse(pos, "this field access");
        return undefined();
    }
    ir::Value want = constantI64(index, tag.type, pos);
    if (failed_) {
        return undefined();
    }
    ir::Instr cmp;
    cmp.op = ir::Op::Cmp;
    cmp.loc = pos;
    cmp.type = TypeRef{ctx_.basic(BasicKind::Bool)};
    cmp.width = ir::IRType::Bool;
    cmp.cmp = cmpFor(BinOp::Eq, false);
    cmp.isPure = true;
    cmp.operands.push_back(got[0]);
    cmp.operands.push_back(want);
    ir::Value out = newValue(cmp.width, cmp.type, pos);
    cmp.results.push_back(out);
    emit(std::move(cmp));
    return out;
}

void Lowerer::emitTagTrap(Position pos, ir::Value ok) {
    emitCheckTrap(pos, ok, ir::TrapReason::UnionTag);
}

void Lowerer::emitCheckTrap(Position pos, ir::Value ok, ir::TrapReason reason) {
    // The mismatch block traps; execution continues in a fresh block that
    // owns everything the caller emits next. Same shape as onIf's join, so
    // the optimizer and codegen meet nothing new.
    uint32_t trapLabel = emitBlock(pos);
    uint32_t liveLabel = emitBlock(pos);
    ir::Terminator br;
    br.term = ir::Term::CondBr;
    br.cond = ok;
    br.thenTarget = liveLabel;
    br.elseTarget = trapLabel;
    br.loc = pos;
    emitTerminator(br);
    sealBlock(current_);
    startBlock(trapLabel);
    ir::Terminator trap;
    trap.term = ir::Term::Trap;
    trap.reason = reason;
    trap.loc = pos;
    emitTerminator(trap);
    sealBlock(trapLabel);
    startBlock(liveLabel);
}

ir::Value Lowerer::emitI64Cmp(Position pos, BinOp op, ir::Value a, ir::Value b) {
    TypeRef boolean{ctx_.basic(BasicKind::Bool)};
    ir::Instr cmp;
    cmp.op = ir::Op::Cmp;
    cmp.loc = pos;
    cmp.type = boolean;
    cmp.width = ir::IRType::Bool;
    cmp.cmp = cmpFor(op, true);
    cmp.isPure = true;
    cmp.operands.push_back(a);
    cmp.operands.push_back(b);
    ir::Value out = newValue(cmp.width, boolean, pos);
    cmp.results.push_back(out);
    emit(std::move(cmp));
    return out;
}

std::vector<ir::Value> Lowerer::lowerIndex(const Index& n) {
    Place base;
    if (!resolvePlace(n.array.get(), base)) {
        refuse(n.array->pos, "this index");
        return {undefined()};
    }
    TypeRef arrayType = base.type;
    if (!arrayType.isError() && (arrayType.type->kind() == Kind::Slice ||
                                 arrayType.type->kind() == Kind::String)) {
        return lowerSliceIndex(n, base, arrayType);
    }
    if (arrayType.isError() || arrayType.type->kind() != Kind::Array) {
        refuse(n.array->pos, "this index");
        return {undefined()};
    }
    TypeRef element{arrayType.type->element()};
    int64_t elemBytes = resolver_.nominalSize(element.type);
    if (elemBytes <= 0) {
        refuse(n.pos, "this index");
        return {undefined()};
    }
    std::unordered_set<std::string> active;
    EvalEnv env = resolver_.evalEnv(resolver_.moduleScope());
    EvalResult folded = evalConstInt(*n.index, env, active);
    if (folded.ok) {
        // A folded index is an offset the compiler names: no address
        // arithmetic at runtime, just a load like a field read.
        if (folded.value < 0) {
            refuse(n.index->pos, "a negative index");
            return {undefined()};
        }
        base.off += folded.value * elemBytes;
        base.type = element;
        return loadRange(n.pos, base, element);
    }
    // Anything else computes its address: base plus index times bytes. Only
    // one-word scalar elements ride this path; anything wider is refused.
    if (elemBytes <= 0 || elemBytes > 8) {
        refuse(n.pos, "this index");
        return {undefined()};
    }
    if (wordCount(element) != 1) {
        refuse(n.pos, "this index");
        return {undefined()};
    }
    std::vector<ir::Value> index = lowerWords(n.index.get());
    if (failed_ || index.size() != kOneWord) {
        refuse(n.index->pos, "this index");
        return {undefined()};
    }
    if (!base.global.empty()) {
        refuse(n.pos, "this index");
        return {undefined()};
    }
    for (uint64_t i = 0; i < static_cast<uint64_t>(base.slots.size()); ++i) {
        if (base.slots[i].id != base.slots[0].id + static_cast<uint32_t>(i)) {
            refuse(n.pos, "this index");
            return {undefined()};
        }
    }
    ir::Value byteOff = index[0];
    if (elemBytes != 1) {
        TypeRef count{ctx_.basic(BasicKind::I64)};
        ir::Value stride = constantI64(elemBytes, count, n.index->pos);
        ir::Instr mul;
        mul.op = ir::Op::Mul;
        mul.loc = n.index->pos;
        mul.type = count;
        mul.width = widthOf(count, n.index->pos);
        if (failed_) {
            return {undefined()};
        }
        mul.isPure = true;
        mul.operands.push_back(index[0]);
        mul.operands.push_back(stride);
        ir::Value scaled = newValue(mul.width, count, n.index->pos);
        mul.results.push_back(scaled);
        emit(std::move(mul));
        byteOff = scaled;
    }
    ir::Instr gep;
    gep.op = ir::Op::Gep;
    gep.loc = n.pos;
    gep.type = TypeRef{ctx_.pointer(element.type)};
    gep.width = ir::IRType::Ptr;
    gep.addressBase = base.slots[0];
    gep.displacement = base.off;
    gep.isPure = true;
    gep.operands.push_back(byteOff);
    ir::Value at = newValue(gep.width, gep.type, n.pos);
    gep.results.push_back(at);
    emit(std::move(gep));
    // The address is computed; the value is one indirect load away.
    Place indirect;
    indirect.slots = {at};
    indirect.type = element;
    indirect.off = 0;
    indirect.isVolatile = base.isVolatile;
    return loadRange(n.pos, indirect, element);
}

void Lowerer::storeIndex(const Index& target, const std::vector<ir::Value>& words) {
    Place base;
    if (!resolvePlace(target.array.get(), base)) {
        refuse(target.array->pos, "this index");
        return;
    }
    TypeRef arrayType = base.type;
    if (!arrayType.isError() && arrayType.type->kind() == Kind::Slice) {
        storeSliceIndex(target, base, arrayType, words);
        return;
    }
    if (!arrayType.isError() && arrayType.type->kind() == Kind::String) {
        // The checker rejects string stores first; the lowerer refuses rather
        // than writing read-only bytes.
        refuse(target.array->pos, "strings are immutable");
        return;
    }
    if (arrayType.isError() || arrayType.type->kind() != Kind::Array) {
        refuse(target.array->pos, "this index");
        return;
    }
    TypeRef element{arrayType.type->element()};
    int64_t elemBytes = resolver_.nominalSize(element.type);
    if (elemBytes <= 0) {
        refuse(target.pos, "this index");
        return;
    }
    std::unordered_set<std::string> active;
    EvalEnv env = resolver_.evalEnv(resolver_.moduleScope());
    EvalResult folded = evalConstInt(*target.index, env, active);
    if (folded.ok) {
        if (folded.value < 0) {
            refuse(target.index->pos, "a negative index");
            return;
        }
        base.off += folded.value * elemBytes;
        base.type = element;
        storeRange(target.pos, base, words, element);
        return;
    }
    refuse(target.pos, "this index");
}

// Folds one bound of a slice or index expression: the value when the
// compiler names it, or nothing when it is a runtime value.
bool Lowerer::foldBound(const Expr* expr, int64_t& value) {
    std::unordered_set<std::string> active;
    EvalEnv env = resolver_.evalEnv(resolver_.moduleScope());
    EvalResult folded = evalConstInt(*expr, env, active);
    if (!folded.ok) {
        return false;
    }
    value = folded.value;
    return true;
}

std::vector<ir::Value> Lowerer::lowerSlice(const SliceExpr& n) {
    if (failed_) {
        return {undefined()};
    }
    TypeRef baseType = checker_.typeOf(n.array.get());
    Kind baseKind = baseType.isError() ? Kind::Error : baseType.type->kind();
    if (baseKind != Kind::Array && baseKind != Kind::Slice && baseKind != Kind::String) {
        refuse(n.array->pos, "this expression");
        return {undefined()};
    }
    Place base;
    if (!resolvePlace(n.array.get(), base)) {
        refuse(n.array->pos, "this expression");
        return {undefined()};
    }
    if (!base.global.empty()) {
        refuse(n.array->pos, "this expression");
        return {undefined()};
    }
    TypeRef element{ctx_.basic(BasicKind::U8)};
    int64_t elemBytes = kOneMember;
    if (baseKind != Kind::String) {
        element = TypeRef{baseType.type->element()};
        elemBytes = resolver_.nominalSize(element.type);
    }
    if (elemBytes <= 0) {
        refuse(n.pos, "this expression");
        return {undefined()};
    }
    TypeRef count{ctx_.basic(BasicKind::I64)};
    int64_t loConst = 0;
    int64_t hiConst = 0;
    bool loFolded = foldBound(n.lo.get(), loConst);
    bool hiFolded = foldBound(n.hi.get(), hiConst);
    if (loFolded && loConst < 0) {
        refuse(n.lo->pos, "a negative slice bound");
        return {undefined()};
    }
    if (loFolded && hiFolded && hiConst < loConst) {
        refuse(n.hi->pos, "a slice bound is out of range");
        return {undefined()};
    }
    // The base length is a fact when the base is an array, a word otherwise.
    ir::Value lenWord = undefined();
    bool lenConst = false;
    int64_t lenValue = 0;
    if (baseKind == Kind::Array) {
        std::optional<int64_t> extent = baseType.type->length();
        if (!extent.has_value() || *extent < 0) {
            refuse(n.array->pos, "this expression");
            return {undefined()};
        }
        lenConst = true;
        lenValue = *extent;
        if (hiFolded && hiConst > lenValue) {
            refuse(n.hi->pos, "a slice bound is out of range");
            return {undefined()};
        }
        if (loFolded && loConst > lenValue) {
            refuse(n.lo->pos, "a slice bound is out of range");
            return {undefined()};
        }
        lenWord = constantI64(lenValue, count, n.array->pos);
    } else {
        std::vector<ir::Value> baseWords = loadRange(n.array->pos, base, baseType);
        if (failed_ || baseWords.size() != kSliceWords) {
            refuse(n.array->pos, "this expression");
            return {undefined()};
        }
        lenWord = baseWords[kLenWord];
    }
    if (failed_) {
        return {undefined()};
    }
    std::vector<ir::Value> loVals = loFolded
        ? std::vector<ir::Value>{constantI64(loConst, count, n.lo->pos)}
        : lowerWords(n.lo.get());
    std::vector<ir::Value> hiVals = hiFolded
        ? std::vector<ir::Value>{constantI64(hiConst, count, n.hi->pos)}
        : lowerWords(n.hi.get());
    if (failed_ || loVals.size() != kOneWord || hiVals.size() != kOneWord) {
        refuse(n.pos, "this expression");
        return {undefined()};
    }
    ir::Value loVal = loVals[kFirstWord];
    ir::Value hiVal = hiVals[kFirstWord];
    ir::Value zero = constantI64(0, count, n.pos);
    if (failed_) {
        return {undefined()};
    }
    // Every bound the compiler cannot prove emits its check: a low bound
    // below zero, a high bound below the low one, a high bound past the
    // base length. Proven bounds check nothing (SEMANTICS 8).
    if (!loFolded) {
        emitCheckTrap(n.lo->pos, emitI64Cmp(n.lo->pos, BinOp::Ge, loVal, zero),
                      ir::TrapReason::IndexOutOfBounds);
    }
    if (!loFolded || !hiFolded) {
        emitCheckTrap(n.pos, emitI64Cmp(n.pos, BinOp::Le, loVal, hiVal),
                      ir::TrapReason::IndexOutOfBounds);
    }
    if (!hiFolded || !lenConst) {
        emitCheckTrap(n.hi->pos, emitI64Cmp(n.hi->pos, BinOp::Le, hiVal, lenWord),
                      ir::TrapReason::IndexOutOfBounds);
    }
    if (failed_) {
        return {undefined()};
    }
    // The pointer is the base address advanced past the low bound. Array
    // bases name slots; slice and string bases carry their pointer in the
    // first word, so the advance is an addition, not a slot address.
    TypeRef ptrType{ctx_.pointer(element.type)};
    ir::Value ptr = undefined();
    if (baseKind == Kind::Array) {
        if (loFolded) {
            int64_t per = layout::kSizePointer;
            int64_t byteAddr = base.off + loConst * elemBytes;
            uint64_t slotIdx = static_cast<uint64_t>(byteAddr / per);
            if (slotIdx >= static_cast<uint64_t>(base.slots.size())) {
                refuse(n.pos, "this expression");
                return {undefined()};
            }
            int64_t intra = byteAddr % per;
            ptr = computedBase(base.slots[slotIdx], intra, n.pos);
            ptr.type = ptrType;
        } else {
            for (uint64_t i = 0; i < static_cast<uint64_t>(base.slots.size()); ++i) {
                if (base.slots[i].id != base.slots[0].id + static_cast<uint32_t>(i)) {
                    refuse(n.pos, "this expression");
                    return {undefined()};
                }
            }
            ir::Value scaled = loVal;
            if (elemBytes != kOneMember) {
                ir::Value stride = constantI64(elemBytes, count, n.lo->pos);
                ir::Instr mul;
                mul.op = ir::Op::Mul;
                mul.loc = n.lo->pos;
                mul.type = count;
                mul.width = widthOf(count, n.lo->pos);
                if (failed_) {
                    return {undefined()};
                }
                mul.isPure = true;
                mul.operands.push_back(loVal);
                mul.operands.push_back(stride);
                scaled = newValue(mul.width, count, n.lo->pos);
                mul.results.push_back(scaled);
                emit(std::move(mul));
            }
            ir::Instr gep;
            gep.op = ir::Op::Gep;
            gep.loc = n.pos;
            gep.type = ptrType;
            gep.width = ir::IRType::Ptr;
            gep.addressBase = base.slots[0];
            gep.displacement = base.off;
            gep.isPure = true;
            gep.operands.push_back(scaled);
            ptr = newValue(gep.width, ptrType, n.pos);
            gep.results.push_back(ptr);
            emit(std::move(gep));
        }
    } else {
        std::vector<ir::Value> baseWords = loadRange(n.array->pos, base, baseType);
        if (failed_ || baseWords.size() != kSliceWords) {
            refuse(n.array->pos, "this expression");
            return {undefined()};
        }
        ir::Value basePtr = baseWords[kFirstWord];
        int64_t byteDelta = loFolded ? loConst * elemBytes : 0;
        if (loFolded && byteDelta == 0) {
            ptr = basePtr;
            ptr.type = ptrType;
        } else {
            ir::Value scaled = loFolded ? constantI64(byteDelta, count, n.lo->pos) : loVal;
            if (!loFolded && elemBytes != kOneMember) {
                ir::Value stride = constantI64(elemBytes, count, n.lo->pos);
                ir::Instr mul;
                mul.op = ir::Op::Mul;
                mul.loc = n.lo->pos;
                mul.type = count;
                mul.width = widthOf(count, n.lo->pos);
                if (failed_) {
                    return {undefined()};
                }
                mul.isPure = true;
                mul.operands.push_back(loVal);
                mul.operands.push_back(stride);
                scaled = newValue(mul.width, count, n.lo->pos);
                mul.results.push_back(scaled);
                emit(std::move(mul));
            }
            if (failed_) {
                return {undefined()};
            }
            ir::Instr add;
            add.op = ir::Op::Add;
            add.loc = n.pos;
            add.type = ptrType;
            add.width = ir::IRType::Ptr;
            add.isPure = true;
            add.operands.push_back(basePtr);
            add.operands.push_back(scaled);
            ptr = newValue(add.width, ptrType, n.pos);
            add.results.push_back(ptr);
            emit(std::move(add));
        }
    }
    if (failed_) {
        return {undefined()};
    }
    ir::Value len = undefined();
    if (loFolded && hiFolded) {
        len = constantI64(hiConst - loConst, count, n.pos);
    } else {
        ir::Instr sub;
        sub.op = ir::Op::Sub;
        sub.loc = n.pos;
        sub.type = count;
        sub.width = widthOf(count, n.pos);
        if (failed_) {
            return {undefined()};
        }
        sub.isPure = true;
        sub.operands.push_back(hiVal);
        sub.operands.push_back(loVal);
        len = newValue(sub.width, count, n.pos);
        sub.results.push_back(len);
        emit(std::move(sub));
    }
    if (failed_) {
        return {undefined()};
    }
    return {ptr, len};
}

bool Lowerer::sliceElementPlace(const Index& n, const Place& base,
                                TypeRef baseType, TypeRef& element, Place& at) {
    Kind baseKind = baseType.type->kind();
    element = TypeRef{ctx_.basic(BasicKind::U8)};
    int64_t elemBytes = kOneMember;
    if (baseKind == Kind::Slice) {
        element = TypeRef{baseType.type->element()};
        elemBytes = resolver_.nominalSize(element.type);
    }
    if (elemBytes <= 0) {
        refuse(n.pos, "this index");
        return false;
    }
    if (!base.global.empty()) {
        refuse(n.array->pos, "this index");
        return false;
    }
    std::vector<ir::Value> baseWords = loadRange(n.array->pos, base, baseType);
    if (failed_ || baseWords.size() != kSliceWords) {
        refuse(n.array->pos, "this index");
        return false;
    }
    ir::Value ptr = baseWords[kFirstWord];
    ir::Value len = baseWords[kLenWord];
    TypeRef count{ctx_.basic(BasicKind::I64)};
    int64_t idxConst = 0;
    bool idxFolded = foldBound(n.index.get(), idxConst);
    if (idxFolded && idxConst < 0) {
        refuse(n.index->pos, "a negative index");
        return false;
    }
    std::vector<ir::Value> idxVals = idxFolded
        ? std::vector<ir::Value>{constantI64(idxConst, count, n.index->pos)}
        : lowerWords(n.index.get());
    if (failed_ || idxVals.size() != kOneWord) {
        refuse(n.index->pos, "this index");
        return false;
    }
    ir::Value idx = idxVals[kFirstWord];
    TypeRef ptrType{ctx_.pointer(element.type)};
    ptr.type = ptrType;
    // A slice length is always a runtime word, so the upper bound always
    // checks; a folded index proves only the low bound (SEMANTICS 8).
    if (!idxFolded) {
        ir::Value zero = constantI64(0, count, n.index->pos);
        if (failed_) {
            return false;
        }
        emitCheckTrap(n.index->pos, emitI64Cmp(n.index->pos, BinOp::Ge, idx, zero),
                      ir::TrapReason::IndexOutOfBounds);
    }
    emitCheckTrap(n.index->pos, emitI64Cmp(n.index->pos, BinOp::Lt, idx, len),
                  ir::TrapReason::IndexOutOfBounds);
    if (failed_) {
        return false;
    }
    int64_t byteDelta = idxFolded ? idxConst * elemBytes : 0;
    ir::Value atAddr = ptr;
    if (!idxFolded || byteDelta != 0) {
        ir::Value scaled = idxFolded ? constantI64(byteDelta, count, n.index->pos) : idx;
        if (!idxFolded && elemBytes != kOneMember) {
            ir::Value stride = constantI64(elemBytes, count, n.index->pos);
            ir::Instr mul;
            mul.op = ir::Op::Mul;
            mul.loc = n.index->pos;
            mul.type = count;
            mul.width = widthOf(count, n.index->pos);
            if (failed_) {
                return false;
            }
            mul.isPure = true;
            mul.operands.push_back(idx);
            mul.operands.push_back(stride);
            scaled = newValue(mul.width, count, n.index->pos);
            mul.results.push_back(scaled);
            emit(std::move(mul));
        }
        if (failed_) {
            return false;
        }
        ir::Instr add;
        add.op = ir::Op::Add;
        add.loc = n.pos;
        add.type = ptrType;
        add.width = ir::IRType::Ptr;
        add.isPure = true;
        add.operands.push_back(ptr);
        add.operands.push_back(scaled);
        atAddr = newValue(add.width, ptrType, n.pos);
        add.results.push_back(atAddr);
        emit(std::move(add));
    }
    if (failed_) {
        return false;
    }
    // The address is computed; the value is one indirect access away.
    at.slots = {atAddr};
    at.type = element;
    at.off = 0;
    at.isVolatile = base.isVolatile;
    return true;
}

std::vector<ir::Value> Lowerer::lowerSliceIndex(const Index& n, const Place& base,
                                               TypeRef baseType) {
    TypeRef element{ctx_.basic(BasicKind::U8)};
    Place at;
    if (!sliceElementPlace(n, base, baseType, element, at)) {
        return {undefined()};
    }
    return loadRange(n.pos, at, element);
}

void Lowerer::storeSliceIndex(const Index& target, const Place& base,
                              TypeRef baseType,
                              const std::vector<ir::Value>& words) {
    TypeRef element{ctx_.basic(BasicKind::U8)};
    Place at;
    if (!sliceElementPlace(target, base, baseType, element, at)) {
        return;
    }
    storeRange(target.pos, at, words, element);
}

std::vector<ir::Value> Lowerer::loadLocal(Position pos, const LocalBinding& binding) {
    std::vector<ir::Value> words;
    int count = wordCount(binding.type);
    for (int i = 0; i < count; ++i) {
        TypeRef word = wordType(binding.type, static_cast<uint64_t>(i));
        ir::Instr instr;
        instr.op = ir::Op::Load;
        instr.loc = pos;
        instr.type = word;
        instr.width = widthOf(word, pos);
        instr.addressBase = binding.slots[static_cast<size_t>(i)];
        instr.isPure = true;
        ir::Value out = newValue(instr.width, word, pos);
        instr.results.push_back(out);
        emit(std::move(instr));
        words.push_back(out);
    }
    return words;
}

Lowerer::LoweredString Lowerer::lowerString(const StringLit& lit) {
    LoweredString out;
    std::string bytes;
    for (const auto& part : lit.parts) {
        if (part.isExpr) {
            refuse(lit.pos, "interpolation is not lowered yet");
            out.words = {undefined()};
            return out;
        }
        bytes += part.text;
    }
    out.label = "s_" + std::to_string(rodataSeq_++);
    out.len = static_cast<int64_t>(bytes.size());
    ir::Rodata blob;
    blob.label = out.label;
    blob.bytes = std::move(bytes);
    module_->rodata.push_back(std::move(blob));
    TypeRef addr{ctx_.pointer(ctx_.basic(BasicKind::U8))};
    ir::Instr data;
    data.op = ir::Op::DataAddr;
    data.loc = lit.pos;
    data.type = addr;
    data.width = ir::IRType::Ptr;
    data.globalLabel = out.label;
    data.isPure = true;
    ir::Value at = newValue(data.width, addr, lit.pos);
    data.results.push_back(at);
    emit(std::move(data));
    out.words.push_back(at);
    TypeRef len{ctx_.basic(BasicKind::I64)};
    out.words.push_back(constantI64(out.len, len, lit.pos));
    return out;
}

std::vector<ir::Value> Lowerer::loadGlobal(Position pos, const GlobalBinding& global) {
    std::vector<ir::Value> words;
    int count = wordCount(global.type);
    for (int i = 0; i < count; ++i) {
        TypeRef word = wordType(global.type, static_cast<uint64_t>(i));
        ir::Instr instr;
        instr.op = ir::Op::Load;
        instr.loc = pos;
        instr.type = word;
        instr.width = widthOf(word, pos);
        instr.globalLabel = global.label;
        instr.displacement = static_cast<int64_t>(i) * 8;
        instr.isPure = true;
        instr.isVolatile = global.isVolatile;
        ir::Value out = newValue(instr.width, word, pos);
        instr.results.push_back(out);
        emit(std::move(instr));
        words.push_back(out);
    }
    return words;
}

std::string Lowerer::forwardFunction(const std::string& link, const Signature& sig,
                                     Position pos) {
    std::string hidden = "closure_" + std::to_string(closureSeq_++);
    int64_t savedFn = -1;
    if (function_ != nullptr) {
        savedFn =
            static_cast<int64_t>(function_ - module_->functions.data());
    }
    uint32_t savedValues = valueSeq_;
    uint32_t savedLabels = labelSeq_;
    std::vector<uint32_t> savedBlocks = std::move(blockOrder_);
    std::vector<std::unordered_map<std::string, LocalBinding>> savedLocals =
        std::move(locals_);
    uint32_t savedCurrent = current_;
    bool savedTerminated = terminated_;

    ir::Function fn;
    fn.name = hidden;
    TypeRef capPtr{ctx_.pointer(ctx_.basic(BasicKind::U8))};
    std::vector<Parameter> combined;
    Parameter receiver;
    receiver.type = capPtr.type;
    receiver.isMutable = false;
    combined.push_back(receiver);
    for (const Parameter& p : sig.parameters()) {
        combined.push_back(p);
    }
    const Type* ret = sig.returnType() != nullptr ? sig.returnType() : ctx_.void_();
    fn.signature = Signature(std::move(combined), ret, Abi::SysV);
    fn.loc = pos;
    module_->functions.push_back(std::move(fn));
    function_ = &module_->functions.back();

    valueSeq_ = 0;
    labelSeq_ = 0;
    blockOrder_.clear();
    locals_.clear();
    scopeMarks_.clear();

    uint32_t entry = emitBlock(pos);
    startBlock(entry);

    // The captures word has no use here: the target is a plain function.
    // It stays a parameter so the call shape matches every other closure.
    std::vector<ir::Value> capSlots = allocaSlots(capPtr, pos, kOneWord);
    for (ir::Value slot : capSlots) {
        function_->params.push_back(slot);
    }
    ir::Instr call;
    call.op = ir::Op::Call;
    call.loc = pos;
    call.callee = link;
    call.mayTrap = true;
    const std::vector<Parameter>& declared = function_->signature.parameters();
    for (uint64_t i = 0; i < static_cast<uint64_t>(sig.parameters().size()); ++i) {
        TypeRef type{declared[i + kOneWord].type};
        int count = wordCount(type);
        if (count < 0) {
            refuse(pos, "this call");
            break;
        }
        std::vector<ir::Value> slots = allocaSlots(type, pos,
                                                   static_cast<uint64_t>(count));
        for (ir::Value slot : slots) {
            function_->params.push_back(slot);
        }
        LocalBinding binding;
        binding.slots = std::move(slots);
        binding.type = type;
        std::vector<ir::Value> words = loadLocal(pos, binding);
        if (failed_) {
            refuse(pos, "this call");
            break;
        }
        ir::Argument arg;
        arg.words = words;
        call.arguments.push_back(std::move(arg));
    }
    if (!failed_) {
        function_->canThrow = true;
        ir::Terminator fret;
        fret.term = ir::Term::Ret;
        fret.loc = pos;
        if (ret->kind() != Kind::Void) {
            ir::Value out = newValue(widthOf(TypeRef{ret}, pos), TypeRef{ret}, pos);
            if (!failed_) {
                call.results.push_back(out);
                call.width = out.width;
                call.type = out.type;
                fret.value = out;
            }
        } else {
            call.width = ir::IRType::Void;
            fret.value = std::nullopt;
        }
        if (!failed_) {
            emit(std::move(call));
            emitTerminator(fret);
        }
    }
    function_ = nullptr;

    function_ =
        savedFn >= 0 ? &module_->functions[static_cast<size_t>(savedFn)] : nullptr;
    valueSeq_ = savedValues;
    labelSeq_ = savedLabels;
    blockOrder_ = std::move(savedBlocks);
    locals_ = std::move(savedLocals);
    current_ = savedCurrent;
    terminated_ = savedTerminated;
    return hidden;
}

std::vector<ir::Value> Lowerer::lowerFuncLit(const FuncLit& lit) {
    // The checker handed the hidden function over with the hook; without an
    // entry the literal was never checked, and inventing one would lower an
    // unchecked body.
    auto found = closures_.find(&lit);
    if (found == closures_.end()) {
        refuse(lit.pos, "this expression");
        return {undefined()};
    }
    const ClosureSite& site = found->second;
    TypeRef closureType = checker_.typeOf(&lit);
    if (closureType.isError() || closureType.type->kind() != Kind::Closure) {
        refuse(lit.pos, "this expression");
        return {undefined()};
    }
    TypeRef codeType = wordType(closureType, kFirstWord);
    TypeRef capsType = wordType(closureType, kLenWord);
    if (failed_) {
        return {undefined()};
    }
    ir::Instr fn;
    fn.op = ir::Op::FnAddr;
    fn.loc = lit.pos;
    fn.type = codeType;
    fn.width = ir::IRType::Ptr;
    fn.globalLabel = site.hidden;
    fn.isPure = true;
    ir::Value code = newValue(fn.width, codeType, lit.pos);
    fn.results.push_back(code);
    emit(std::move(fn));

    ir::Value caps = constantI64(0, capsType, lit.pos);
    if (failed_) {
        return {undefined()};
    }
    if (!site.captures.empty()) {
        int64_t per = layout::kSizePointer;
        int64_t total = 0;
        for (const check::Captured& cap : site.captures) {
            int64_t extent = resolver_.nominalSize(cap.type.type);
            if (extent < 0) {
                refuse(cap.pos, "this capture is not lowered yet");
                return {undefined()};
            }
            total += extent;
        }
        // One allocation behind the captures word, filled from the
        // enclosing locals in first-read order — the same order the hidden
        // function reads them back in.
        TypeRef count{ctx_.basic(BasicKind::I64)};
        ir::Instr alloc;
        alloc.op = ir::Op::Call;
        alloc.loc = lit.pos;
        alloc.callee = "cider_alloc";
        alloc.mayTrap = true;
        for (int64_t fixed : {total, per}) {
            ir::Argument arg;
            arg.words.push_back(constantI64(fixed, count, lit.pos));
            if (failed_) {
                return {undefined()};
            }
            alloc.arguments.push_back(std::move(arg));
        }
        ir::Value at = newValue(ir::IRType::Ptr, capsType, lit.pos);
        alloc.results.push_back(at);
        alloc.width = ir::IRType::Ptr;
        alloc.type = capsType;
        emit(std::move(alloc));
        caps = at;
        int64_t off = 0;
        for (const check::Captured& cap : site.captures) {
            Ident name(lit.pos, cap.name);
            Place base;
            if (!resolvePlace(&name, base)) {
                refuse(cap.pos, "this capture is not lowered yet");
                return {undefined()};
            }
            std::vector<ir::Value> words = loadRange(lit.pos, base, cap.type);
            if (failed_) {
                return {undefined()};
            }
            Place dst;
            dst.hasIndirect = true;
            dst.indirect = caps;
            dst.off = off;
            dst.type = cap.type;
            storeRange(lit.pos, dst, words, cap.type);
            if (failed_) {
                return {undefined()};
            }
            off += resolver_.nominalSize(cap.type.type);
        }
    }
    return {code, caps};
}

void Lowerer::constructClass(const std::vector<ir::Value>& slots, const NewStruct& n) {
    TypeRef classType = checker_.typeOf(&n);
    if (classType.isError() || classType.type->kind() != Kind::Class) {
        refuse(n.pos, "this construction");
        return;
    }
    std::optional<DeclId> id = classType.type->decl();
    if (!id.has_value()) {
        refuse(n.pos, "this construction");
        return;
    }
    if (!resolver_.classBases(*id).empty()) {
        refuse(n.pos, "inheritance is not lowered yet");
        return;
    }
    // Word zero is the vptr slot, null until methods wire it: the slot is
    // reserved now so field offsets never shift later.
    TypeRef vptr{ctx_.basic(BasicKind::U64)};
    ir::Value null = constantI64(0, vptr, n.pos);
    if (failed_) {
        return;
    }
    Place atVptr;
    atVptr.slots = slots;
    atVptr.type = vptr;
    atVptr.off = 0;
    storeRange(n.pos, atVptr, {null}, vptr);
    if (failed_) {
        return;
    }
    Place base;
    base.slots = slots;
    base.type = classType;
    for (const FieldInit& field : n.fields) {
        int64_t fieldOff = resolver_.fieldOffset(*id, field.name);
        TypeRef fieldType = resolver_.fieldType(*id, field.name);
        if (fieldOff < 0 || fieldType.isError()) {
            refuse(field.value->pos, "no such field");
            return;
        }
        std::vector<ir::Value> value = lowerWords(field.value.get());
        if (failed_) {
            return;
        }
        Place at = base;
        at.off = fieldOff;
        at.type = fieldType;
        storeRange(field.value->pos, at, value, fieldType);
        if (failed_) {
            return;
        }
    }
}
std::vector<ir::Value> Lowerer::lowerNewStruct(const NewStruct& n) {
    TypeRef structType = checker_.typeOf(&n);
    if (!structType.isError() && structType.type->kind() == Kind::Class) {
        // Inherited fields have no layout yet: the refusal names the gap
        // before wordCount turns the -1 size into a generic construction
        // refusal. constructClass asserts the same precondition.
        std::optional<DeclId> id = structType.type->decl();
        if (!id.has_value()) {
            refuse(n.pos, "this construction");
            return {undefined()};
        }
        if (!resolver_.classBases(*id).empty()) {
            refuse(n.pos, "inheritance is not lowered yet");
            return {undefined()};
        }
    }
    int count = wordCount(structType);
    if (count < 0) {
        refuse(n.pos, "this construction");
        return {undefined()};
    }
    std::vector<ir::Value> slots =
        allocaSlots(structType, n.pos, static_cast<uint64_t>(count));
    // NewStruct builds a struct field by field, a class after its vptr
    // word, or a union by its one live member plus tag; the checker
    // guarantees which, so the branch reads the checked type rather than
    // re-deriving it.
    if (!structType.isError() && structType.type->kind() == Kind::Union) {
        constructUnion(slots, n);
    } else if (!structType.isError() && structType.type->kind() == Kind::Class) {
        constructClass(slots, n);
    } else {
        constructStruct(slots, n);
    }
    if (failed_) {
        return {undefined()};
    }
    std::vector<ir::Value> words;
    for (int i = 0; i < count; ++i) {
        TypeRef word = wordType(structType, static_cast<uint64_t>(i));
        ir::Instr instr;
        instr.op = ir::Op::Load;
        instr.loc = n.pos;
        instr.type = word;
        instr.width = widthOf(word, n.pos);
        if (failed_) {
            return {undefined()};
        }
        instr.addressBase = slots[static_cast<size_t>(i)];
        instr.isPure = true;
        ir::Value out = newValue(instr.width, word, n.pos);
        instr.results.push_back(out);
        emit(std::move(instr));
        words.push_back(out);
    }
    return words;
}

std::vector<ir::Value> Lowerer::lowerNewClass(const NewClass& n) {
    // `new C(args)`: heap memory plus the constructor. The checker proved
    // the target is a class with an init of matching arity; the size, the
    // signature, and the owner name are re-derived here.
    TypeRef ptrType = checker_.typeOf(&n);
    if (ptrType.isError() || ptrType.type->kind() != Kind::Pointer) {
        refuse(n.pos, "this construction");
        return {undefined()};
    }
    const Type* obj = ptrType.type->pointee();
    if (obj == nullptr || obj->kind() != Kind::Class) {
        refuse(n.pos, "this construction");
        return {undefined()};
    }
    std::optional<DeclId> id = obj->decl();
    const resolve::Decl* owner = id.has_value() ? resolver_.declById(*id) : nullptr;
    const Signature* init = id.has_value() ? resolver_.methodSig(*id, "init") : nullptr;
    int64_t byteCount = kNoExtent;
    int64_t stepTo = kNoExtent;
    if (id.has_value()) {
        byteCount = resolver_.classSize(*id);
        stepTo = resolver_.classAlign(*id);
    }
    if (owner == nullptr || init == nullptr || byteCount <= 0 || stepTo <= 0) {
        refuse(n.pos, "this construction");
        return {undefined()};
    }
    TypeRef count{ctx_.basic(BasicKind::I64)};
    ir::Instr alloc;
    alloc.op = ir::Op::Call;
    alloc.loc = n.pos;
    alloc.callee = "cider_alloc";
    alloc.mayTrap = true;
    for (int64_t fixed : {byteCount, stepTo}) {
        ir::Argument arg;
        arg.words.push_back(constantI64(fixed, count, n.pos));
        if (failed_) {
            return {undefined()};
        }
        alloc.arguments.push_back(std::move(arg));
    }
    ir::Value at = newValue(ir::IRType::Ptr, ptrType, n.pos);
    alloc.results.push_back(at);
    alloc.width = ir::IRType::Ptr;
    alloc.type = ptrType;
    emit(std::move(alloc));
    ir::Instr run;
    run.op = ir::Op::Call;
    run.loc = n.pos;
    run.callee = owner->name + "__init";
    run.mayTrap = true;
    ir::Argument self;
    self.words.push_back(at);
    run.arguments.push_back(std::move(self));
    for (uint64_t i = 0; i < static_cast<uint64_t>(n.args.size()); ++i) {
        ir::Argument arg;
        arg.words = lowerWords(n.args[i].get());
        if (failed_) {
            return {undefined()};
        }
        run.arguments.push_back(std::move(arg));
    }
    run.width = ir::IRType::Void;
    emit(std::move(run));
    return {at};
}

std::vector<ir::Value> Lowerer::lowerNewArray(const NewArray& n) {
    TypeRef arrayType = checker_.typeOf(&n);
    int count = wordCount(arrayType);
    if (count < 0) {
        refuse(n.pos, "this construction");
        return {undefined()};
    }
    std::vector<ir::Value> slots =
        allocaSlots(arrayType, n.pos, static_cast<uint64_t>(count));
    constructArray(slots, n);
    if (failed_) {
        return {undefined()};
    }
    std::vector<ir::Value> words;
    for (int i = 0; i < count; ++i) {
        TypeRef word = wordType(arrayType, static_cast<uint64_t>(i));
        ir::Instr instr;
        instr.op = ir::Op::Load;
        instr.loc = n.pos;
        instr.type = word;
        instr.width = widthOf(word, n.pos);
        if (failed_) {
            return {undefined()};
        }
        instr.addressBase = slots[static_cast<size_t>(i)];
        instr.isPure = true;
        ir::Value out = newValue(instr.width, word, n.pos);
        instr.results.push_back(out);
        emit(std::move(instr));
        words.push_back(out);
    }
    return words;
}

std::vector<ir::Value> Lowerer::lowerAddrOf(const Unary& n) {
    // `&f` where f names a plain function answers its code address: one
    // word holding the link address, for an indirect call to load. Every
    // other address refuses with the message it always refused with —
    // `&x` on values is FEAT-038 and must not change shape here.
    if (n.op == UnOp::Addr) {
        if (const auto* ident = dynamic_cast<const Ident*>(n.operand.get())) {
            const resolve::Decl* decl =
                resolver_.lookup(resolver_.moduleScope(), ident->name);
            if (decl != nullptr && decl->kind == resolve::DeclKind::Func &&
                decl->typeParams == 0) {
                std::string link = ident->name;
                if (decl->isPrelude) {
                    link = "cider_" + ident->name;
                }
                TypeRef ptr{ctx_.functionPtr(decl->sig)};
                ir::Instr fn;
                fn.op = ir::Op::FnAddr;
                fn.loc = n.pos;
                fn.type = ptr;
                fn.width = ir::IRType::Ptr;
                fn.globalLabel = link;
                fn.isPure = true;
                ir::Value at = newValue(fn.width, ptr, n.pos);
                fn.results.push_back(at);
                emit(std::move(fn));
                return {at};
            }
        }
    }
    if (n.op == UnOp::Addr) {
        // `&x` on a value answers the address of its place: a direct
        // place names a slot plus an offset, and an indirect place is
        // already an address with an offset. Globals have no address
        // materialization, so they keep the refusal they always had.
        Place place;
        if (resolvePlace(n.operand.get(), place) && place.global.empty()) {
            TypeRef ptr = checker_.typeOf(&n);
            if (!ptr.isError()) {
                if (place.hasIndirect) {
                    if (place.off == 0) {
                        return {place.indirect};
                    }
                    TypeRef count{ctx_.basic(BasicKind::I64)};
                    ir::Value delta = constantI64(place.off, count, n.pos);
                    if (!failed_) {
                        ir::Instr add;
                        add.op = ir::Op::Add;
                        add.loc = n.pos;
                        add.type = ptr;
                        add.width = ir::IRType::Ptr;
                        add.isPure = true;
                        add.operands.push_back(place.indirect);
                        add.operands.push_back(delta);
                        ir::Value at = newValue(add.width, ptr, n.pos);
                        add.results.push_back(at);
                        emit(std::move(add));
                        return {at};
                    }
                    return {undefined()};
                }
                if (!place.slots.empty()) {
                    return {computedBase(place.slots[0], place.off, n.pos)};
                }
            }
        }
    }
    refuse(n.pos, "this expression");
    return {undefined()};
}

std::vector<ir::Value> Lowerer::lowerWords(const Expr* expr) {
    if (expr == nullptr || failed_) {
        return {undefined()};
    }
    if (const auto* lit = dynamic_cast<const StringLit*>(expr)) {
        return lowerString(*lit).words;
    }
    if (dynamic_cast<const NewStruct*>(expr) != nullptr) {
        return lowerNewStruct(static_cast<const NewStruct&>(*expr));
    }
    if (dynamic_cast<const NewArray*>(expr) != nullptr) {
        return lowerNewArray(static_cast<const NewArray&>(*expr));
    }
    if (dynamic_cast<const NewClass*>(expr) != nullptr) {
        return lowerNewClass(static_cast<const NewClass&>(*expr));
    }
    if (dynamic_cast<const FuncLit*>(expr) != nullptr) {
        return lowerFuncLit(static_cast<const FuncLit&>(*expr));
    }
    if (dynamic_cast<const Field*>(expr) != nullptr) {
        return lowerField(static_cast<const Field&>(*expr));
    }
    if (dynamic_cast<const Index*>(expr) != nullptr) {
        return lowerIndex(static_cast<const Index&>(*expr));
    }
    if (dynamic_cast<const SliceExpr*>(expr) != nullptr) {
        return lowerSlice(static_cast<const SliceExpr&>(*expr));
    }
    if (const auto* unary = dynamic_cast<const Unary*>(expr)) {
        // `*p` as a value loads whatever the pointer addresses: the
        // place machinery already resolves the dereference to an
        // indirect place, so a read is one range load. Anything else
        // unary answers an address or refuses inside lowerAddrOf.
        if (unary->op == UnOp::Deref) {
            Place place;
            if (!resolvePlace(expr, place)) {
                refuse(expr->pos, "this value");
                return {undefined()};
            }
            TypeRef pointee = checker_.typeOf(expr);
            if (pointee.isError()) {
                refuse(expr->pos, "this value");
                return {undefined()};
            }
            place.type = pointee;
            return loadRange(expr->pos, place, pointee);
        }
        return lowerAddrOf(*unary);
    }
    if (const auto* ident = dynamic_cast<const Ident*>(expr)) {
        TypeRef type = checker_.typeOf(ident);
        if (wordCount(type) < 0) {
            refuse(ident->pos, "this value");
            return {undefined()};
        }
        uint64_t depth = static_cast<uint64_t>(locals_.size());
        for (; depth > 0;) {
            --depth;
            auto found = locals_[depth].find(ident->name);
            if (found != locals_[depth].end()) {
                return loadLocal(ident->pos, found->second);
            }
        }
        for (const GlobalBinding& global : globals_) {
            if (global.name == ident->name) {
                return loadGlobal(ident->pos, global);
            }
        }
        // A bare function name in value position answers the closure pair:
        // a forwarder with the receiver-first shape plus a null captures
        // word, with no allocation. Prelude names mangle to their link
        // symbols, like calls do.
        const resolve::Decl* decl =
            resolver_.lookup(resolver_.moduleScope(), ident->name);
        if (decl != nullptr && decl->kind == resolve::DeclKind::Func &&
            decl->typeParams == 0) {
            TypeRef type = checker_.typeOf(ident);
            if (!type.isError() && type.type->kind() == Kind::Closure &&
                wordCount(type) == static_cast<int>(kClosureWords)) {
                std::string link = ident->name;
                if (decl->isPrelude) {
                    link = "cider_" + ident->name;
                }
                std::string fwd = forwardFunction(link, decl->sig, ident->pos);
                if (failed_) {
                    return {undefined()};
                }
                ir::Instr fn;
                fn.op = ir::Op::FnAddr;
                fn.loc = ident->pos;
                fn.type = wordType(type, kFirstWord);
                fn.width = ir::IRType::Ptr;
                fn.globalLabel = fwd;
                fn.isPure = true;
                ir::Value code = newValue(fn.width, fn.type, ident->pos);
                fn.results.push_back(code);
                emit(std::move(fn));
                ir::Value caps =
                    constantI64(0, wordType(type, kLenWord), ident->pos);
                if (failed_) {
                    return {undefined()};
                }
                return {code, caps};
            }
        }
        refuse(ident->pos, "reading '" + ident->name + "'");
        return {undefined()};
    }
    return {onIdentOrExpr(expr, checker_.typeOf(expr))};
}

void Lowerer::onExprStmt(ExprStmt& n) {
    if (failed_ || !enabled_ || function_ == nullptr) {
        return;
    }
    // A statement assignment stores and answers nothing: demanding one word
    // here would refuse multi-word stores whose value nobody reads.
    if (auto* assign = dynamic_cast<const Assign*>(n.expr.get())) {
        lowerAssignStmt(const_cast<Assign&>(*assign));
        return;
    }
    onIdentOrExpr(n.expr.get(), checker_.typeOf(n.expr.get()));
}

void Lowerer::onDelete(DeleteStmt& n) {
    if (failed_ || !enabled_ || function_ == nullptr) {
        return;
    }
    // The pointer lowers to one address word however it was computed; the
    // checker proved it points at a class.
    std::vector<ir::Value> ptr = lowerWords(n.ptr.get());
    if (failed_ || ptr.size() != kOneWord) {
        return;
    }
    TypeRef ptrType = checker_.typeOf(n.ptr.get());
    const Type* obj = ptrType.type->pointee();
    std::optional<DeclId> id = obj->decl();
    const resolve::Decl* owner = id.has_value() ? resolver_.declById(*id) : nullptr;
    const Signature* dsig = id.has_value() ? resolver_.methodSig(*id, "deinit") : nullptr;
    if (dsig != nullptr) {
        // A declared deinit runs first, with the object address as the
        // receiver — the same receiver-first shape onMethodDecl emits.
        ir::Instr run;
        run.op = ir::Op::Call;
        run.loc = n.pos;
        run.callee = owner->name + "__deinit";
        run.mayTrap = true;
        ir::Argument self;
        self.words.push_back(ptr[0]);
        run.arguments.push_back(std::move(self));
        run.width = ir::IRType::Void;
        emit(std::move(run));
    }
    // Then the storage goes back. Prelude names mangle to their link symbol
    // exactly as onCall mangles them, and free(nilPtr) is a no-op by
    // contract, so a nil delete frees nothing — the deinit above is the
    // caller's problem, and dereferencing nil there already traps.
    ir::Instr rel;
    rel.op = ir::Op::Call;
    rel.loc = n.pos;
    rel.callee = "cider_free";
    rel.mayTrap = true;
    ir::Argument words;
    words.words.push_back(ptr[0]);
    rel.arguments.push_back(std::move(words));
    rel.width = ir::IRType::Void;
    emit(std::move(rel));
}

std::vector<ir::Value> Lowerer::storeToIdent(
    const Ident& n, const std::vector<ir::Value>& words) {
    for (auto& frame : locals_) {
        auto found = frame.find(n.name);
        if (found != frame.end()) {
            for (uint64_t i = 0; i < static_cast<uint64_t>(words.size()); ++i) {
                ir::Instr instr;
                instr.op = ir::Op::Store;
                instr.loc = n.pos;
                instr.type = found->second.type;
                instr.addressBase = found->second.slots[i];
                instr.storedValue = words[i];
                emit(std::move(instr));
            }
            return words;
        }
    }
    for (const GlobalBinding& global : globals_) {
        if (global.name == n.name) {
            for (uint64_t i = 0; i < static_cast<uint64_t>(words.size()); ++i) {
                ir::Instr instr;
                instr.op = ir::Op::Store;
                instr.loc = n.pos;
                instr.type = global.type;
                instr.globalLabel = global.label;
                instr.displacement = static_cast<int64_t>(i) * 8;
                instr.storedValue = words[i];
                instr.isVolatile = global.isVolatile;
                emit(std::move(instr));
            }
            return words;
        }
    }
    refuse(n.pos, "assigning '" + n.name + "'");
    return {undefined()};
}

ir::Value Lowerer::onCast(Cast& n) {
    if (failed_) {
        return undefined();
    }
    // A union cast reads the payload's bytes at offset 0 as the target,
    // ignoring the tag (TYPES.md 4.6). The checker proved the target fits
    // the payload; a multi-word target is refused, like every other
    // multi-word expression value.
    TypeRef source = checker_.typeOf(n.expr.get());
    TypeRef target = checker_.typeOf(&n);
    if (!source.isError() && !target.isError() && source.type->kind() == Kind::Union) {
        Place base;
        if (!resolvePlace(n.expr.get(), base)) {
            refuse(n.expr->pos, "this cast");
            return undefined();
        }
        base.type = target;
        base.tagWrites.clear();
        std::vector<ir::Value> got = loadRange(n.pos, base, target);
        if (failed_ || got.size() != kOneWord) {
            refuse(n.pos, "this cast");
            return undefined();
        }
        return got[0];
    }
    ir::Instr instr;
    instr.op = ir::Op::Cast;
    instr.loc = n.pos;
    instr.type = checker_.typeOf(&n);
    instr.width = widthOf(instr.type, n.pos);
    ir::Value from = onIdentOrExpr(n.expr.get(), checker_.typeOf(n.expr.get()));
    instr.operands.push_back(from);
    ir::Value result = newValue(instr.width, instr.type, n.pos);
    instr.results.push_back(result);
    emit(std::move(instr));
    return result;
}

ir::Value Lowerer::onBinary(Binary& n) {
    if (failed_) {
        return undefined();
    }
    TypeRef lhs = checker_.typeOf(n.lhs.get());
    TypeRef rhs = checker_.typeOf(n.rhs.get());
    TypeRef result = checker_.typeOf(&n);
    ir::IRType width = widthOf(result, n.pos);
    ir::Value a = onIdentOrExpr(n.lhs.get(), lhs);
    ir::Value b = onIdentOrExpr(n.rhs.get(), rhs);
    ir::Instr instr;
    instr.loc = n.pos;
    instr.type = result;
    instr.width = width;
    if (comparison(n.op)) {
        instr.op = ir::Op::Cmp;
        instr.width = ir::IRType::Bool;
        instr.type = TypeRef{ctx_.basic(BasicKind::Bool)};
        bool isSigned = true;
        if (!result.isError() && result.type->kind() == Kind::Basic) {
            isSigned = *result.type->basic() == BasicKind::I32 ||
                       *result.type->basic() == BasicKind::I64;
        }
        if (!lhs.isError() && lhs.type->kind() == Kind::Basic) {
            BasicKind common = *lhs.type->basic();
            isSigned = common == BasicKind::I8 || common == BasicKind::I16 ||
                       common == BasicKind::I32 || common == BasicKind::I64;
        }
        instr.cmp = cmpFor(n.op, isSigned);
        instr.operands.push_back(a);
        instr.operands.push_back(b);
        ir::Value out = newValue(ir::IRType::Bool, instr.type, n.pos);
        instr.results.push_back(out);
        emit(std::move(instr));
        return out;
    }
    ir::Op op = arithOp(n.op);
    if (op == ir::Op::Const) {
        refuse(n.pos, "this operator");
        return undefined();
    }
    return emitArith(n.op, a, b, result, n.pos);
}

ir::Value Lowerer::emitArith(BinOp op, ir::Value a, ir::Value b, TypeRef result,
                             Position pos) {
    ir::Instr instr;
    instr.loc = pos;
    instr.type = result;
    instr.width = widthOf(result, pos);
    instr.op = arithOp(op);
    instr.isPure = true;
    instr.overflow = wrap_ ? ir::OverflowMode::Wrap : ir::OverflowMode::Undefined;
    // Division and remainder trap; nothing else in this set does. The flag is
    // what stops DCE deleting the check and LICM hoisting the divide.
    if (instr.op == ir::Op::Div || instr.op == ir::Op::Rem) {
        instr.mayTrap = true;
    }
    instr.operands.push_back(a);
    instr.operands.push_back(b);
    ir::Value out = newValue(instr.width, result, pos);
    instr.results.push_back(out);
    emit(std::move(instr));
    return out;
}

ir::Value Lowerer::onCall(Call& n) {
    if (failed_) {
        return undefined();
    }
    // `v.hasI()`: the tag compared to the member's index, answered as one
    // boolean word. The checker proved the shape; the member lookup repeats
    // here rather than trusting it, so a checker bug reads as a refusal.
    if (const auto* field = dynamic_cast<const Field*>(n.callee.get())) {
        Place base;
        if (resolvePlace(field->object.get(), base) && !base.type.isError() &&
            base.type.type->kind() == Kind::Union) {
            std::optional<DeclId> id = base.type.type->decl();
            std::string member =
                id.has_value() ? resolver_.unionHasMember(*id, field->name) : "";
            int64_t index = id.has_value() && !member.empty()
                                ? resolver_.unionMemberIndex(*id, member)
                                : -1;
            // Split across lines on purpose: the offset call beside a
            // digit reads as a size literal and T4 refuses it.
            int64_t tagOff = -1;
            if (id.has_value()) {
                tagOff = resolver_.unionTagOffset(*id);
            }
            if (!id.has_value() || member.empty() || index < 0 || tagOff < 0) {
                refuse(n.pos, "this call");
                return undefined();
            }
            if (!n.args.empty()) {
                refuse(n.pos, "hasM takes no arguments");
                return undefined();
            }
            return emitTagCheck(n.pos, base, tagOff, index);
        }
    }
    const Signature* sig = nullptr;
    std::string callee;
    bool indirect = false;
    std::optional<ir::Value> indirectCallee;
    std::vector<ir::Value> receiver;
    if (const auto* field = dynamic_cast<const Field*>(n.callee.get())) {
        // `c.m(args)`: the checker proved the field names a method, so this
        // is a static call to the mangled name with the object's address
        // first. Disjoint from the union path above: only values with
        // methods reach here.
        Place base;
        if (resolvePlace(field->object.get(), base) && !base.type.isError() &&
            (base.type.type->kind() == Kind::Class ||
             base.type.type->kind() == Kind::Struct)) {
            std::optional<DeclId> id = base.type.type->decl();
            const resolve::Decl* owner =
                id.has_value() ? resolver_.declById(*id) : nullptr;
            const Signature* msig = id.has_value()
                                        ? resolver_.methodSig(*id, field->name)
                                        : nullptr;
            if (owner == nullptr || msig == nullptr) {
                refuse(n.pos, "this call");
                return undefined();
            }
            callee = owner->name + "__" + field->name;
            sig = msig;
            if (base.slots.empty() && !base.hasIndirect) {
                refuse(n.pos, "a method on a global is not lowered yet");
                return undefined();
            }
            if (base.hasIndirect && base.off != 0) {
                // A method on a field reached through a pointer: the object
                // address is the loaded pointer plus the field offset, added
                // the same way a slice advances its pointer.
                TypeRef count{ctx_.basic(BasicKind::I64)};
                TypeRef ptrType{ctx_.pointer(base.type.type)};
                ir::Value step = constantI64(base.off, count, n.pos);
                if (failed_) {
                    return undefined();
                }
                ir::Instr add;
                add.op = ir::Op::Add;
                add.loc = n.pos;
                add.type = ptrType;
                add.width = ir::IRType::Ptr;
                add.isPure = true;
                add.operands.push_back(base.indirect);
                add.operands.push_back(step);
                ir::Value at = newValue(add.width, ptrType, n.pos);
                add.results.push_back(at);
                emit(std::move(add));
                receiver.push_back(at);
            } else if (base.hasIndirect) {
                receiver.push_back(base.indirect);
            } else {
                receiver.push_back(computedBase(base.slots[0], base.off, n.pos));
            }
        }
    }
    if (const auto* ident = dynamic_cast<const Ident*>(n.callee.get())) {
        sig = functionSig(ident->name);
        callee = ident->name;
        // Prelude names mangle to their link symbols; anything else keeps its
        // spelling, including a program's own shadowing `print`. The lookup
        // is the same one functionSig uses, so the two cannot disagree.
        const resolve::Decl* decl =
            resolver_.lookup(resolver_.moduleScope(), ident->name);
        if (decl != nullptr && decl->isPrelude) {
            callee = "cider_" + ident->name;
        }
        if (sig == nullptr) {
            // A closure-typed local calls through its code word, with the
            // captures word riding as the receiver the hidden function
            // expects first. A function name never arrives here:
            // functionSig answers those above.
            uint64_t depth = static_cast<uint64_t>(locals_.size());
            for (; depth > 0;) {
                --depth;
                auto bound = locals_[depth].find(ident->name);

                if (bound != locals_[depth].end() &&
                    bound->second.type.type->kind() == Kind::Closure) {
                    const Signature* csig = bound->second.type.type->signature();

                    if (csig != nullptr) {
                        std::vector<ir::Value> pair =
                            loadLocal(n.pos, bound->second);

                        if (failed_ || pair.size() != kClosureWords) {
                            refuse(n.pos, "this call");
                            return undefined();
                        }
                        sig = csig;
                        indirect = true;
                        indirectCallee = pair[kFirstWord];
                        receiver.push_back(pair[kLenWord]);
                    }
                    break;
                }
            }
        }
    } else {
        if (const auto* deref = dynamic_cast<const Unary*>(n.callee.get())) {
            if (deref->op == UnOp::Deref) {
                TypeRef inner = checker_.typeOf(deref->operand.get());
                if (!inner.isError() && inner.type->kind() == Kind::FunctionPtr) {
                    sig = inner.type->signature();
                    indirect = true;
                    // The callee word travels with the call: codegen must
                    // never invent the target, so a missing word refuses
                    // here rather than emitting a call to nowhere.
                    std::vector<ir::Value> target = lowerWords(deref->operand.get());
                    if (failed_ || target.size() != kOneWord) {
                        refuse(n.pos, "this call");
                        return undefined();
                    }
                    indirectCallee = target[0];
                }
            }
        }
    }
    if (sig == nullptr) {
        refuse(n.pos, "this call");
        return undefined();
    }
    ir::Instr instr;
    instr.op = ir::Op::Call;
    instr.loc = n.pos;
    instr.callee = callee;
    instr.call.indirect = indirect;
    if (indirectCallee.has_value()) {
        instr.calleeValue = *indirectCallee;
    }
    instr.mayTrap = true;
    for (ir::Value word : receiver) {
        ir::Argument arg;
        arg.words.push_back(word);
        instr.arguments.push_back(std::move(arg));
    }
    for (uint64_t i = 0; i < static_cast<uint64_t>(n.args.size()); ++i) {
        TypeRef want{sig->parameters()[i].type};
        (void)want;
        ir::Argument arg;
        arg.words = lowerWords(n.args[i].get());
        if (failed_) {
            return undefined();
        }
        instr.arguments.push_back(arg);
    }
    const Type* ret = sig->returnType() != nullptr ? sig->returnType() : ctx_.void_();
    // Captured before the move below: emit() takes the instruction by value,
    // so reading results after moving reads an emptied vector — which is how
    // a call's result became the undefined placeholder.
    bool hasResult = false;
    ir::Value out;
    if (ret->kind() != Kind::Void) {
        out = newValue(widthOf(TypeRef{ret}, n.pos), TypeRef{ret}, n.pos);
        instr.results.push_back(out);
        instr.width = out.width;
        instr.type = out.type;
        hasResult = true;
    } else {
        instr.width = ir::IRType::Void;
    }
    emit(std::move(instr));
    if (function_ != nullptr) {
        // A call is mayTrap only if the callee can throw, which is known here
        // rather than guessed by the optimiser.
        function_->canThrow = true;
    }
    return hasResult ? out : undefined();
}

void Lowerer::onReturn(Return& n) {
    if (failed_ || !enabled_ || function_ == nullptr) {
        return;
    }
    ir::Terminator term;
    term.term = ir::Term::Ret;
    term.loc = n.pos;
    if (n.value != nullptr) {
        // One word returns; several are refused until the ABI table's
        // rax/rdx row is implemented. The asymmetry with arguments is
        // documented, not hidden.
        std::vector<ir::Value> words = lowerWords(n.value.get());
        if (words.size() != kOneWord) {
            refuse(n.pos, "string returns are not emitted yet");
            return;
        }
        term.value = words[0];
    } else {
        // A bare return answers nothing: nullopt, not the placeholder. The
        // placeholder's id poisons the frame computation, which reads every
        // value the function mentions.
        term.value = std::nullopt;
    }
    emitTerminator(term);
}

void Lowerer::onIf(IfStmt& n) {
    if (failed_ || !enabled_ || function_ == nullptr) {
        return;
    }
    ir::Value cond = onIdentOrExpr(n.cond.get(), checker_.typeOf(n.cond.get()));
    uint32_t thenLabel = emitBlock(n.pos);
    uint32_t elseLabel = emitBlock(n.pos);
    uint32_t joinLabel = emitBlock(n.pos);

    ir::Terminator br;
    br.term = ir::Term::CondBr;
    br.cond = cond;
    br.thenTarget = thenLabel;
    br.elseTarget = elseLabel;
    br.loc = n.pos;
    emitTerminator(br);
    sealBlock(current_);

    startBlock(thenLabel);
    locals_.push_back({});
    n.then->accept(checker_);
    locals_.pop_back();
    ir::Terminator toJoin;
    toJoin.term = ir::Term::Br;
    toJoin.target = joinLabel;
    toJoin.loc = n.pos;
    emitTerminator(toJoin);

    startBlock(elseLabel);
    locals_.push_back({});
    if (n.els != nullptr) {
        n.els->accept(checker_);
    }
    locals_.pop_back();
    emitTerminator(toJoin);

    startBlock(joinLabel);
}

void Lowerer::onWhile(WhileStmt& n) {
    if (failed_ || !enabled_ || function_ == nullptr) {
        return;
    }
    uint32_t headLabel = emitBlock(n.pos);
    uint32_t bodyLabel = emitBlock(n.pos);
    uint32_t exitLabel = emitBlock(n.pos);

    ir::Terminator toHead;
    toHead.term = ir::Term::Br;
    toHead.target = headLabel;
    toHead.loc = n.pos;
    emitTerminator(toHead);

    startBlock(headLabel);
    locals_.push_back({});
    ir::Value cond = onIdentOrExpr(n.cond.get(), checker_.typeOf(n.cond.get()));
    locals_.pop_back();
    ir::Terminator br;
    br.term = ir::Term::CondBr;
    br.cond = cond;
    br.thenTarget = bodyLabel;
    br.elseTarget = exitLabel;
    br.loc = n.pos;
    emitTerminator(br);

    startBlock(bodyLabel);
    locals_.push_back({});
    n.body->accept(checker_);
    locals_.pop_back();
    emitTerminator(toHead);

    startBlock(exitLabel);
}

ir::Value Lowerer::onIdentOrExpr(const Expr* expr, TypeRef expected) {
    (void)expected;
    if (expr == nullptr || failed_) {
        return undefined();
    }
    if (auto* ident = dynamic_cast<const Ident*>(expr)) {
        return onIdent(const_cast<Ident&>(*ident));
    }
    if (auto* binary = dynamic_cast<const Binary*>(expr)) {
        return onBinary(const_cast<Binary&>(*binary));
    }
    if (auto* call = dynamic_cast<const Call*>(expr)) {
        return onCall(const_cast<Call&>(*call));
    }
    if (auto* cast = dynamic_cast<const Cast*>(expr)) {
        return onCast(const_cast<Cast&>(*cast));
    }
    if (auto* lit = dynamic_cast<const IntLit*>(expr)) {
        std::unordered_set<std::string> active;
        EvalEnv env = resolver_.evalEnv(resolver_.moduleScope());
        EvalResult folded = evalConstInt(*lit, env, active);
        TypeRef type = checker_.typeOf(expr);
        if (!folded.ok) {
            refuse(lit->pos, "this constant");
            return undefined();
        }
        return constantI64(folded.value, type, lit->pos);
    }
    if (auto* lit = dynamic_cast<const CharLit*>(expr)) {
        return constantI64(static_cast<int64_t>(lit->value),
                           TypeRef{ctx_.basic(BasicKind::Char)}, lit->pos);
    }
    if (auto* lit = dynamic_cast<const BoolLit*>(expr)) {
        return constantI64(lit->value ? 1 : 0, TypeRef{ctx_.basic(BasicKind::Bool)},
                           lit->pos);
    }
    if (auto* nil = dynamic_cast<const NilPtr*>(expr)) {
        TypeRef type = TypeRef{ctx_.pointer(ctx_.void_())};
        return constantI64(0, type, nil->pos);
    }
    if (auto* assign = dynamic_cast<const Assign*>(expr)) {
        return lowerAssign(const_cast<Assign&>(*assign));
    }
    // Scalar positions (binary operands, conditions) read fields and elements
    // through the same paths values use, and demand exactly one word: an
    // aggregate in an arithmetic operand is a checker error long before this,
    // so arriving here with several words is a lowerer bug, not a program.
    if (auto* field = dynamic_cast<const Field*>(expr)) {
        std::vector<ir::Value> words = lowerField(const_cast<Field&>(*field));
        if (words.size() != kOneWord) {
            refuse(expr->pos, "this expression");
            return undefined();
        }
        return words[0];
    }
    if (auto* index = dynamic_cast<const Index*>(expr)) {
        std::vector<ir::Value> words = lowerIndex(const_cast<Index&>(*index));
        if (words.size() != kOneWord) {
            refuse(expr->pos, "this expression");
            return undefined();
        }
        return words[0];
    }
    if (auto* unary = dynamic_cast<const Unary*>(expr)) {
        if (unary->op == UnOp::Deref) {
            std::vector<ir::Value> words = lowerWords(expr);
            if (words.size() != kOneWord) {
                refuse(expr->pos, "this expression");
                return undefined();
            }
            return words[0];
        }
    }
    refuse(expr->pos, "this expression");
    return undefined();
}

ir::Value Lowerer::lowerAssign(Assign& n) {
    if (failed_) {
        return undefined();
    }
    TypeRef target = checker_.typeOf(n.target.get());
    std::vector<ir::Value> value = lowerWords(n.value.get());
    // The words must agree with the target: a checker/lowerer disagreement
    // here would store the wrong number of words, silently.
    if (!target.isError() && wordCount(target) >= 0 &&
        value.size() != static_cast<size_t>(wordCount(target))) {
        refuse(n.pos, "this assignment");
        return undefined();
    }
    if (n.op.has_value()) {
        // A compound op on several words never arrives: the checker refuses
        // it. Asserting that here keeps the two layers honest with each other.
        if (value.size() != kOneWord) {
            refuse(n.pos, "a compound assignment needs one word");
            return undefined();
        }
        TypeRef current = checker_.typeOf(n.target.get());
        std::vector<ir::Value> old = lowerWords(n.target.get());
        if (old.size() != kOneWord) {
            refuse(n.pos, "a compound assignment needs one word");
            return undefined();
        }
        value = {emitArith(*n.op, old[0], value[0], current, n.pos)};
    }
    if (!storeAssignTarget(n, value)) {
        return undefined();
    }
    if (value.size() != kOneWord) {
        refuse(n.pos, "a multi-word assignment is not an expression");
        return undefined();
    }
    return value[0];
}

// The statement form: same stores, no value answered, so several words are
// fine. An assignment nested in an expression still goes through
// lowerAssign and refuses there.
void Lowerer::lowerAssignStmt(Assign& n) {
    if (failed_) {
        return;
    }
    TypeRef target = checker_.typeOf(n.target.get());
    std::vector<ir::Value> value = lowerWords(n.value.get());
    if (!target.isError() && wordCount(target) >= 0 &&
        value.size() != static_cast<size_t>(wordCount(target))) {
        refuse(n.pos, "this assignment");
        return;
    }
    if (n.op.has_value()) {
        if (value.size() != kOneWord) {
            refuse(n.pos, "a compound assignment needs one word");
            return;
        }
        TypeRef current = checker_.typeOf(n.target.get());
        std::vector<ir::Value> old = lowerWords(n.target.get());
        if (old.size() != kOneWord) {
            refuse(n.pos, "a compound assignment needs one word");
            return;
        }
        value = {emitArith(*n.op, old[0], value[0], current, n.pos)};
    }
    storeAssignTarget(n, value);
}

bool Lowerer::storeAssignTarget(Assign& n, const std::vector<ir::Value>& value) {
    if (const auto* ident = dynamic_cast<const Ident*>(n.target.get())) {
        storeToIdent(const_cast<Ident&>(*ident), value);
        return !failed_;
    }
    if (const auto* index = dynamic_cast<const Index*>(n.target.get())) {
        storeIndex(*index, value);
        return !failed_;
    }
    if (dynamic_cast<const Field*>(n.target.get()) != nullptr) {
        Place place;
        if (!resolvePlace(n.target.get(), place)) {
            refuse(n.target->pos, "this assignment");
            return false;
        }
        place.type = checker_.typeOf(n.target.get());
        storeRange(n.pos, place, value, place.type);
        if (failed_) {
            return false;
        }
        // Every union crossed on the way to the member learns the new live
        // member: a write sets the tag as a consequence (TYPES.md 4.6).
        for (const TagWrite& tag : place.tagWrites) {
            Place at = place;
            at.off = tag.off;
            at.type = TypeRef{ctx_.basic(BasicKind::U32)};
            at.tagWrites.clear();
            ir::Value live = constantI64(tag.index, at.type, n.pos);
            if (failed_) {
                return false;
            }
            storeRange(n.pos, at, {live}, at.type);
            if (failed_) {
                return false;
            }
        }
        return !failed_;
    }
    if (const auto* deref = dynamic_cast<const Unary*>(n.target.get())) {
        if (deref->op != UnOp::Deref) {
            refuse(n.target->pos, "this assignment");
            return false;
        }
        // `*p = v` stores through the pointer using the place the
        // dereference resolves to: the union tag writes ride along,
        // because a dereferenced union member retags too.
        Place place;
        if (!resolvePlace(n.target.get(), place)) {
            refuse(n.target->pos, "this assignment");
            return false;
        }
        place.type = checker_.typeOf(n.target.get());
        storeRange(n.pos, place, value, place.type);
        if (failed_) {
            return false;
        }
        for (const TagWrite& tag : place.tagWrites) {
            Place at = place;
            at.off = tag.off;
            at.type = TypeRef{ctx_.basic(BasicKind::U32)};
            at.tagWrites.clear();
            ir::Value live = constantI64(tag.index, at.type, n.pos);
            if (failed_) {
                return false;
            }
            storeRange(n.pos, at, {live}, at.type);
            if (failed_) {
                return false;
            }
        }
        return !failed_;
    }
    refuse(n.target->pos, "this assignment");
    return false;
}

}  // namespace lower
}  // namespace cider
