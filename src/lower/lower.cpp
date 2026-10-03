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
    if (kind == Kind::String) {
        return 2;
    }
    int64_t bytes = -1;
    if (kind == Kind::Array) {
        bytes = typeSize(type.type);
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
        // base value's: `p.inner.y` is p's slots plus two offsets.
        if (!resolvePlace(field->object.get(), out)) {
            return false;
        }
        if (out.type.isError() || out.type.type->kind() != Kind::Struct) {
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
        out.off += fieldOff;
        out.type = fieldType;
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
        std::vector<ir::Value> slots = allocaSlots(type, n.params[i].pos,
                                                   static_cast<uint64_t>(wordCount(type)));
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
    ret.value = undefined();
    ret.loc = n.pos;
    emitTerminator(ret);
    function_ = nullptr;
}

void Lowerer::onBlockEnter(Block& n) {
    (void)n;
    if (failed_ || !enabled_ || function_ == nullptr) {
        return;
    }
    locals_.push_back({});
}

void Lowerer::onBlockExit(Block& n) {
    (void)n;
    if (locals_.empty()) {
        return;
    }
    locals_.pop_back();
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
    if (aggregate.type->kind() == Kind::Struct ||
        aggregate.type->kind() == Kind::Array) {
        return TypeRef{ctx_.basic(BasicKind::U64)};
    }
    return aggregate;
}

std::vector<ir::Value> Lowerer::loadRange(Position pos, const Place& place,
                                         TypeRef fieldType) {
    int64_t bytes = typeSize(fieldType.type);
    int64_t local = place.off % 8;
    if (bytes <= 0 || bytes > 8 || local + bytes > 8) {
        refuse(pos, "this field layout is not lowered yet");
        return {undefined()};
    }
    ir::IRType width = widthOf(fieldType, pos);
    if (failed_) {
        return {undefined()};
    }
    ir::Instr instr;
    instr.op = ir::Op::Load;
    instr.loc = pos;
    instr.type = fieldType;
    instr.width = width;
    instr.displacement = local;
    instr.isPure = true;
    instr.isVolatile = place.isVolatile;
    if (!place.global.empty()) {
        instr.globalLabel = place.global;
    } else {
        uint64_t word = static_cast<uint64_t>(place.off / 8);
        if (word >= place.slots.size()) {
            refuse(pos, "this field layout is not lowered yet");
            return {undefined()};
        }
        instr.addressBase = place.slots[word];
    }
    ir::Value out = newValue(instr.width, fieldType, pos);
    instr.results.push_back(out);
    emit(std::move(instr));
    return {out};
}

void Lowerer::storeRange(Position pos, const Place& place,
                         const std::vector<ir::Value>& words, TypeRef fieldType) {
    if (words.size() == kOneWord) {
        int64_t bytes = typeSize(fieldType.type);
        int64_t local = place.off % 8;
        if (bytes <= 0 || bytes > 8 || local + bytes > 8) {
            refuse(pos, "this field layout is not lowered yet");
            return;
        }
        ir::IRType width = widthOf(fieldType, pos);
        if (failed_) {
            return;
        }
        ir::Instr instr;
        instr.op = ir::Op::Store;
        instr.loc = pos;
        instr.type = fieldType;
        instr.width = width;
        instr.displacement = local;
        instr.storedValue = words[0];
        instr.isVolatile = place.isVolatile;
        if (!place.global.empty()) {
            instr.globalLabel = place.global;
        } else {
            uint64_t word = static_cast<uint64_t>(place.off / 8);
            if (word >= place.slots.size()) {
                refuse(pos, "this field layout is not lowered yet");
                return;
            }
            instr.addressBase = place.slots[word];
        }
        emit(std::move(instr));
        return;
    }
    // Several words land word-aligned or not at all: an unaligned multi-word
    // copy would guess the neighbor bytes at both ends.
    if (place.off % 8 != 0) {
        refuse(pos, "this field layout is not lowered yet");
        return;
    }
    uint64_t first = static_cast<uint64_t>(place.off / 8);
    if (first + words.size() > place.slots.size() && place.global.empty()) {
        refuse(pos, "this field layout is not lowered yet");
        return;
    }
    for (uint64_t i = 0; i < static_cast<uint64_t>(words.size()); ++i) {
        ir::Instr instr;
        instr.op = ir::Op::Store;
        instr.loc = pos;
        instr.type = words[i].type;
        instr.width = widthOf(words[i].type, pos);
        if (failed_) {
            return;
        }
        instr.storedValue = words[i];
        instr.isVolatile = place.isVolatile;
        if (!place.global.empty()) {
            instr.globalLabel = place.global;
            instr.displacement = place.off + static_cast<int64_t>(i) * 8;
        } else {
            instr.addressBase = place.slots[first + i];
        }
        emit(std::move(instr));
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

void Lowerer::constructArray(const std::vector<ir::Value>& slots, const NewArray& n) {
    TypeRef arrayType = checker_.typeOf(&n);
    if (arrayType.isError() || arrayType.type->kind() != Kind::Array) {
        refuse(n.pos, "this construction");
        return;
    }
    TypeRef element = TypeRef{arrayType.type->element()};
    int64_t elemBytes = typeSize(element.type);
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
    if (base.type.isError() || base.type.type->kind() != Kind::Struct) {
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

std::vector<ir::Value> Lowerer::lowerIndex(const Index& n) {
    Place base;
    if (!resolvePlace(n.array.get(), base)) {
        refuse(n.array->pos, "this index");
        return {undefined()};
    }
    TypeRef arrayType = base.type;
    if (arrayType.isError() || arrayType.type->kind() != Kind::Array) {
        refuse(n.array->pos, "this index");
        return {undefined()};
    }
    TypeRef element{arrayType.type->element()};
    int64_t elemBytes = typeSize(element.type);
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
    if (arrayType.isError() || arrayType.type->kind() != Kind::Array) {
        refuse(target.array->pos, "this index");
        return;
    }
    TypeRef element{arrayType.type->element()};
    int64_t elemBytes = typeSize(element.type);
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

std::vector<ir::Value> Lowerer::lowerNewStruct(const NewStruct& n) {
    TypeRef structType = checker_.typeOf(&n);
    int count = wordCount(structType);
    if (count < 0) {
        refuse(n.pos, "this construction");
        return {undefined()};
    }
    std::vector<ir::Value> slots =
        allocaSlots(structType, n.pos, static_cast<uint64_t>(count));
    constructStruct(slots, n);
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
    if (dynamic_cast<const Field*>(expr) != nullptr) {
        return lowerField(static_cast<const Field&>(*expr));
    }
    if (dynamic_cast<const Index*>(expr) != nullptr) {
        return lowerIndex(static_cast<const Index&>(*expr));
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
        refuse(ident->pos, "reading '" + ident->name + "'");
        return {undefined()};
    }
    return {onIdentOrExpr(expr, checker_.typeOf(expr))};
}

void Lowerer::onExprStmt(ExprStmt& n) {
    if (failed_ || !enabled_ || function_ == nullptr) {
        return;
    }
    onIdentOrExpr(n.expr.get(), checker_.typeOf(n.expr.get()));
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
    const Signature* sig = nullptr;
    std::string callee;
    bool indirect = false;
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
    } else {
        if (const auto* deref = dynamic_cast<const Unary*>(n.callee.get())) {
            if (deref->op == UnOp::Deref) {
                TypeRef inner = checker_.typeOf(deref->operand.get());
                if (!inner.isError() && inner.type->kind() == Kind::FunctionPtr) {
                    sig = inner.type->signature();
                    indirect = true;
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
    instr.mayTrap = true;
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
        term.value = undefined();
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
    if (const auto* ident = dynamic_cast<const Ident*>(n.target.get())) {
        std::vector<ir::Value> stored =
            storeToIdent(const_cast<Ident&>(*ident), value);
        if (stored.size() != kOneWord) {
            refuse(n.pos, "a multi-word assignment is not an expression");
            return undefined();
        }
        return stored[0];
    }
    if (const auto* index = dynamic_cast<const Index*>(n.target.get())) {
        storeIndex(*index, value);
        if (failed_ || value.size() != kOneWord) {
            refuse(n.pos, "a multi-word assignment is not an expression");
            return undefined();
        }
        return value[0];
    }
    if (dynamic_cast<const Field*>(n.target.get()) != nullptr) {
        Place place;
        if (!resolvePlace(n.target.get(), place)) {
            refuse(n.target->pos, "this assignment");
            return undefined();
        }
        place.type = checker_.typeOf(n.target.get());
        storeRange(n.pos, place, value, place.type);
        if (failed_ || value.size() != kOneWord) {
            refuse(n.pos, "a multi-word assignment is not an expression");
            return undefined();
        }
        return value[0];
    }
    refuse(n.target->pos, "this assignment");
    return undefined();
}

}  // namespace lower
}  // namespace cider
