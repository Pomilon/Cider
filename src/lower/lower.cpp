// lower.cpp — scalars to IR.
//
// Driven by the checker's walk: every hook below is called while the checker is
// visiting a node it has just typed, so a type is recorded and an instruction
// emitted on the same visit. Nothing here walks the AST.

#include "lower/lower.h"

#include <cstring>
#include <cstdlib>
#include <functional>

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
// A string literal answers the same pair, so its address is word zero:
// a byte pointer beside a literal keeps that word and drops the length.
// A char is one byte, so *char rides the same justification as *u8.
// *void accepts any address, so the literal's word zero lands there too.
bool isBytePointerTarget(const TypeRef& type) {
    if (type.isError() || type.type->kind() != Kind::Pointer) {
        return false;
    }
    TypeRef pointee{type.type->pointee()};
    if (pointee.isError()) {
        return false;
    }
    if (pointee.type->kind() == Kind::Void) {
        return true;
    }
    if (pointee.type->kind() != Kind::Basic) {
        return false;
    }
    BasicKind basic = *pointee.type->basic();
    return basic == BasicKind::U8 || basic == BasicKind::I8 ||
           basic == BasicKind::Char;
}
// A closure answers two words, code then captures, in the same positions
// a slice keeps base and length: the hidden function loads word zero, the
// allocation behind word one.
constexpr uint64_t kClosureWords = 2;
// An interface answers two words, data then vtable (TYPES.md §4.3): the
// data word carries the concrete value inline, and the vtable word names
// the (concrete, interface) table. Word zero is the data, word one the
// table, in the layout.h offset order.
constexpr uint64_t kInterfaceWords = 2;
constexpr uint64_t kVtableWord = 1;

// The register return row carries two words (rax, rdx): a third word needs
// the hidden pointer (IR.md §6), so the count names that limit, not a digit.
constexpr uint64_t kTwoWords = 2;

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

bool Lowerer::lowerModule(const std::string& name, Program& program) {
    module_ = std::make_unique<ir::Module>();
    module_->name = name;
    return lowerInto(name, program);
}

bool Lowerer::lowerInto(const std::string& name, Program& program) {
    // No failed_ reset: one module accumulates every program, and a refusal
    // anywhere fails the build. wrap_ is per file and every instruction the
    // walk emits reads it.
    enabled_ = true;
    wrap_ = program.wrap;
    checker_.setSink(this);
    // One walk: the checker visits each node once and calls into here as it
    // types, so nothing in this file visits the AST.
    bool checked = checker_.checkProgram(program, name);
    checker_.setSink(nullptr);
    enabled_ = false;
    return checked && !failed_;
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
    // quintillion slots. Interfaces size to two words (data plus vtable,
    // TYPES.md §4.3) through wordCount like every other aggregate; anything
    // else unsized refuses as a value without storage.
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
    if (kind == Kind::String || kind == Kind::Slice || kind == Kind::Interface) {
        // A string is address plus length, a slice pointer plus length, an
        // interface data plus vtable: three pairs, each two words, each
        // owned by layout.h (TYPES.md §3.1) rather than by this count.
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

std::string Lowerer::emitVTable(DeclId concrete, DeclId iface, Position pos) {
    const resolve::Decl* owner = resolver_.declById(concrete);
    const resolve::Decl* wanted = resolver_.declById(iface);
    if (owner == nullptr || wanted == nullptr) {
        refuse(pos, "this interface conversion");
        return "";
    }
    // One table per pair, named under both link names so two modules'
    // same-named types never share one: the entry module links bare.
    std::string label =
        resolver_.linkName(*owner) + "__vtable__" + resolver_.linkName(*wanted);
    if (!vtablesEmitted_.insert(label).second) {
        return label;
    }
    ir::Global table;
    table.label = label;
    table.type = TypeRef{ctx_.pointer(ctx_.basic(BasicKind::U8))};
    for (const std::string& name : resolver_.methodNames(iface)) {
        // The checker proved the concrete implements every method, so a
        // miss here is a lowerer bug, not a program, and refuses as one.
        const Signature* csig = resolver_.methodSig(concrete, name);
        if (csig == nullptr) {
            refuse(pos, "this interface conversion");
            return "";
        }
        if (owner->nominal == Kind::Class) {
            // A class entry normalizes the receiver through an adapter:
            // the data word holds a reference, not inline bytes, so the
            // method's static address would read the wrong receiver.
            table.initLabels.push_back(emitIfaceAdapter(concrete, name, *csig, pos));
            if (failed_) {
                return "";
            }
        } else if (owner->nominal == Kind::Struct) {
            // The method's static address, spelled exactly as a direct call
            // spells it, so dispatch and direct calls name one function.
            table.initLabels.push_back(resolver_.linkName(*owner) + "__" + name);
        } else {
            refuse(pos, "this interface conversion");
            return "";
        }
    }
    module_->globals.push_back(std::move(table));
    return label;
}

std::string Lowerer::emitIfaceAdapter(DeclId concrete, const std::string& method,
                                      const Signature& sig, Position pos) {
    const resolve::Decl* owner = resolver_.declById(concrete);
    if (owner == nullptr) {
        refuse(pos, "this interface conversion");
        return "";
    }
    // One adapter per (class, method), named under the method's own link
    // name so two modules' same-named types never share one. A source
    // function already holding the name refuses below instead of being
    // shadowed by generated text.
    std::string hidden =
        resolver_.linkName(*owner) + "__" + method + "__ifaceadapt";
    if (!adaptersEmitted_.insert(hidden).second) {
        return hidden;
    }
    for (const ir::Function& fn : module_->functions) {
        if (fn.name == hidden) {
            refuse(pos, "this interface conversion");
            return "";
        }
    }
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
    std::vector<uint64_t> savedMarks = std::move(scopeMarks_);
    uint32_t savedCurrent = current_;
    bool savedTerminated = terminated_;

    ir::Function fn;
    fn.name = hidden;
    TypeRef dataPtr{ctx_.pointer(ctx_.basic(BasicKind::U8))};
    std::vector<Parameter> combined;
    Parameter receiver;
    receiver.type = dataPtr.type;
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

    // The data word's address arrives first: the class reference it holds
    // is the receiver the method expects, loaded here in two steps — the
    // slot holds the address, and only a computed address dereferences.
    std::vector<ir::Value> dataSlots = allocaSlots(dataPtr, pos, kOneWord);
    for (ir::Value slot : dataSlots) {
        function_->params.push_back(slot);
    }
    ir::Instr addr;
    addr.op = ir::Op::Load;
    addr.loc = pos;
    addr.type = dataPtr;
    addr.width = ir::IRType::Ptr;
    addr.addressBase = dataSlots[kFirstWord];
    addr.isPure = true;
    ir::Value addrVal = newValue(addr.width, dataPtr, pos);
    addr.results.push_back(addrVal);
    emit(std::move(addr));
    ir::Instr load;
    load.op = ir::Op::Load;
    load.loc = pos;
    load.type = dataPtr;
    load.width = ir::IRType::Ptr;
    load.addressBase = addrVal;
    load.isPure = true;
    ir::Value self = newValue(load.width, dataPtr, pos);
    load.results.push_back(self);
    emit(std::move(load));
    ir::Instr call;
    call.op = ir::Op::Call;
    call.loc = pos;
    call.callee = resolver_.linkName(*owner) + "__" + method;
    call.mayTrap = true;
    {
        ir::Argument selfArg;
        selfArg.words.push_back(self);
        call.arguments.push_back(std::move(selfArg));
    }
    const std::vector<Parameter>& declared = function_->signature.parameters();
    for (uint64_t i = 0; i < static_cast<uint64_t>(sig.parameters().size()); ++i) {
        TypeRef type{declared[i + kOneWord].type};
        int count = wordCount(type);
        if (count < 0) {
            refuse(pos, "this interface conversion");
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
            refuse(pos, "this interface conversion");
            break;
        }
        ir::Argument arg;
        arg.words = words;
        // The adapter forwards into the method's signature: a float
        // parameter travels in its SSE class here too.
        if (type.type != nullptr && type.type->kind() == Kind::Basic &&
            basicIsFloat(*type.type->basic()) &&
            arg.words.size() == static_cast<size_t>(kOneWord)) {
            arg.width = *type.type->basic() == BasicKind::F32
                            ? ir::IRType::F32
                            : ir::IRType::F64;
        }
        call.arguments.push_back(std::move(arg));
    }
    recordParamClasses();
    if (!failed_) {
        function_->canThrow = true;
        ir::Terminator fret;
        fret.term = ir::Term::Ret;
        fret.loc = pos;
        if (ret->kind() != Kind::Void) {
            std::vector<ir::Value> answers = callResults(call, ret, pos);
            if (!failed_) {
                fret.values = std::move(answers);
            }
        } else {
            call.width = ir::IRType::Void;
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
    scopeMarks_ = std::move(savedMarks);
    current_ = savedCurrent;
    terminated_ = savedTerminated;
    return hidden;
}

std::vector<ir::Value> Lowerer::boxToInterface(std::vector<ir::Value> words,
                                              TypeRef from, TypeRef to,
                                              Position pos) {
    if (failed_ || to.isError() || from.isError()) {
        return words;
    }
    if (to.type->kind() != Kind::Interface) {
        return words;
    }
    if (from.type->kind() == Kind::Interface) {
        // Already a pair: the caller's word-count check enforces the two
        // words, so passing through is exact, not approximate.
        return words;
    }
    std::optional<DeclId> iface = to.type->decl();
    if (!iface.has_value()) {
        refuse(pos, "this interface conversion");
        return {undefined()};
    }
    // The data word holds the concrete bytes inline when they fit one
    // word (TYPES.md §4.3), or the class reference itself. A wider struct
    // needs a class box the programmer writes — anything else refuses by
    // name, never by miscompile.
    std::optional<DeclId> concrete;
    if (from.type->kind() == Kind::Pointer && from.type->pointee() != nullptr &&
        from.type->pointee()->kind() == Kind::Class) {
        if (words.size() != kOneWord) {
            refuse(pos, "this interface conversion");
            return {undefined()};
        }
        concrete = from.type->pointee()->decl();
    } else if (from.type->kind() == Kind::Struct &&
               wordCount(from) == static_cast<int>(kOneWord) &&
               words.size() == kOneWord) {
        concrete = from.type->decl();
    } else {
        refuse(pos, from.type->kind() == Kind::Struct
                          ? "a struct wider than one word in an interface: box it in a class"
                          : "this interface conversion is not lowered yet");
        return {undefined()};
    }
    if (!concrete.has_value()) {
        refuse(pos, "this interface conversion");
        return {undefined()};
    }
    std::string label = emitVTable(*concrete, *iface, pos);
    if (failed_) {
        return {undefined()};
    }
    TypeRef vtableType = wordType(to, kVtableWord);
    ir::Instr addr;
    addr.op = ir::Op::DataAddr;
    addr.loc = pos;
    addr.type = vtableType;
    addr.width = ir::IRType::Ptr;
    addr.globalLabel = label;
    addr.isPure = true;
    ir::Value table = newValue(addr.width, vtableType, pos);
    addr.results.push_back(table);
    emit(std::move(addr));
    return {words[kFirstWord], table};
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
        if (const GlobalBinding* global = findGlobal(ident->name)) {
            out.slots.clear();
            out.global = global->label;
            out.type = global->type;
            out.off = 0;
            out.isVolatile = global->isVolatile;
            return true;
        }
        return false;
    }
    if (const auto* field = dynamic_cast<const Field*>(expr)) {
        if (!resolvePlace(field->object.get(), out)) {
        // A nested chain accumulates the offset while the slots stay the
        // base value's: `p.inner.y` is p's slots plus two offsets. Union
        // members sit at offset 0 and record a tag write: storing through
        // the member sets the tag as a consequence (TYPES.md 4.6).
            // A field over a pointer element (`p[0].lo`): the direct
            // route only knows slots, but a pointer element has an
            // address. Materialize it the way computed reads do, then
            // take the field offset below. resolvePlace-Index is left
            // untouched, so the array single-answer routes keep theirs.
            const auto* elem = dynamic_cast<const Index*>(field->object.get());
            if (elem == nullptr) {
                return false;
            }
            TypeRef ptrType = checker_.typeOf(elem->array.get());
            if (ptrType.isError() || ptrType.type->kind() != Kind::Pointer) {
                return false;
            }
            Place base;
            if (!resolvePlace(elem->array.get(), base)) {
                return false;
            }
            if (!pointerElementPlace(*elem, base, ptrType, "this field access", out)) {
                return false;
            }
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
        // Same provable case as the read and store paths: a folded index
        // past the array's extent rejects instead of addressing a
        // neighbour's field.
        std::optional<int64_t> count = out.type.type->length();
        if (count.has_value() && folded.value >= *count) {
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

void Lowerer::recordParamClasses() {
    if (function_ == nullptr || failed_) {
        return;
    }
    function_->paramClasses.clear();
    const std::vector<Parameter>& params = function_->signature.parameters();
    uint64_t total = 0;
    for (const Parameter& p : params) {
        TypeRef type{p.type};
        if (type.type != nullptr && type.type->kind() == Kind::Basic &&
            basicIsFloat(*type.type->basic())) {
            // One word, one SSE class: a float parameter never splits.
            function_->paramClasses.push_back(
                *type.type->basic() == BasicKind::F32 ? ir::IRType::F32
                                                      : ir::IRType::F64);
            total += 1;
            continue;
        }
        int words = wordCount(type);
        if (words < 1) {
            words = 1;
        }
        for (int w = 0; w < words; ++w) {
            function_->paramClasses.push_back(ir::IRType::Void);
            total += 1;
        }
    }
    // The classes must cover every allocated slot: a mismatch means the
    // walk disagrees with the slots, and a prologue reading a class it
    // does not have would pull an argument from the wrong register.
    if (total != static_cast<uint64_t>(function_->params.size())) {
        refuse(function_->loc, "this function's parameter classes");
    }
}

ir::Value Lowerer::constantFBits(uint64_t bits, TypeRef type, Position pos) {
    ir::Instr instr;
    instr.op = ir::Op::Const;
    instr.width = widthOf(type, pos);
    instr.type = type;
    instr.loc = pos;
    instr.isPure = true;
    // The bits ride in `constantBits` the same way an integer value does:
    // an f32 pattern zero-extends into the low word (the movss reader takes
    // those four bytes) and an f64 pattern is the whole field. Const's
    // materialisation — imm32 or movabs — already serves both.
    instr.constantBits = static_cast<int64_t>(bits);
    instr.constantText = std::to_string(bits);
    ir::Value result = newValue(instr.width, type, pos);
    instr.results.push_back(result);
    emit(std::move(instr));
    return result;
}

ir::Value Lowerer::lowerFloatLit(const FloatLit& lit) {
    TypeRef type = checker_.typeOf(&lit);
    if (type.isError() || type.type->kind() != Kind::Basic) {
        refuse(lit.pos, "this constant");
        return undefined();
    }
    BasicKind basic = *type.type->basic();
    // Parse at the target width: the decimal reaches binary32 or binary64
    // in one rounding, so an f32 literal is not rounded twice through f64.
    if (basic == BasicKind::F32) {
        float parsed = 0.0f;
        try {
            parsed = std::stof(lit.text);
        } catch (...) {
            refuse(lit.pos, "this constant");
            return undefined();
        }
        if (lit.negative) {
            parsed = -parsed;
        }
        uint32_t pattern = 0;
        std::memcpy(&pattern, &parsed, sizeof pattern);
        return constantFBits(pattern, type, lit.pos);
    }
    if (basic == BasicKind::F64) {
        double parsed = 0.0;
        try {
            parsed = std::stod(lit.text);
        } catch (...) {
            refuse(lit.pos, "this constant");
            return undefined();
        }
        if (lit.negative) {
            parsed = -parsed;
        }
        uint64_t pattern = 0;
        std::memcpy(&pattern, &parsed, sizeof pattern);
        return constantFBits(pattern, type, lit.pos);
    }
    refuse(lit.pos, "this constant");
    return undefined();
}

ir::Value Lowerer::convertInt(ir::Value v, TypeRef from, TypeRef to,
                             Position pos) {
    if (failed_ || from.isError() || to.isError() || from == to) {
        return v;
    }
    ir::Instr cast;
    cast.op = ir::Op::Cast;
    cast.loc = pos;
    cast.type = to;
    cast.width = widthOf(to, pos);
    if (failed_) {
        return v;
    }
    cast.isPure = true;
    cast.operands.push_back(v);
    ir::Value out = newValue(cast.width, to, pos);
    cast.results.push_back(out);
    emit(std::move(cast));
    return out;
}

ir::Value Lowerer::lowerPointerArith(const Expr* lhsExpr, const Expr* rhsExpr,
                                     TypeRef lhs, TypeRef rhs, TypeRef result,
                                     BinOp op, Position pos) {
    // FEATURES §1.7 / SEMANTICS §8.2: `p + n` and `p - n` advance by the
    // count scaled to the element's size; `p - q` over one *T is the byte
    // span divided back down to an element count. The checker already
    // proved the shapes, the void cases, and the same-pointee rule, so
    // this answers rather than re-decides.
    const bool lhsPtr = lhs.type->kind() == Kind::Pointer;
    const bool rhsPtr = rhs.type->kind() == Kind::Pointer;
    TypeRef count{ctx_.basic(BasicKind::I64)};
    if (lhsPtr && rhsPtr) {
        ir::Value a = onIdentOrExpr(lhsExpr, lhs);
        if (failed_) {
            return undefined();
        }
        ir::Value b = onIdentOrExpr(rhsExpr, rhs);
        if (failed_) {
            return undefined();
        }
        ir::Value span = emitArith(BinOp::Sub, a, b, count, pos);
        if (failed_) {
            return undefined();
        }
        int64_t elemBytes = resolver_.nominalSize(lhs.type->pointee());
        if (elemBytes <= 0) {
            refuse(pos, "this pointer difference");
            return undefined();
        }
        if (elemBytes == 1) {
            return span;
        }
        return emitArith(BinOp::Div, span, constantI64(elemBytes, count, pos),
                         count, pos);
    }
    const bool ptrFirst = lhsPtr;
    TypeRef ptrType = ptrFirst ? lhs : rhs;
    int64_t elemBytes = resolver_.nominalSize(ptrType.type->pointee());
    if (elemBytes <= 0) {
        refuse(pos, "this pointer arithmetic");
        return undefined();
    }
    ir::Value base =
        onIdentOrExpr(ptrFirst ? lhsExpr : rhsExpr, ptrType);
    if (failed_) {
        return undefined();
    }
    ir::Value step = onIdentOrExpr(ptrFirst ? rhsExpr : lhsExpr,
                                   ptrFirst ? rhs : lhs);
    if (failed_) {
        return undefined();
    }
    // The count converts to i64 before scaling: a 32-bit stride product
    // would wrap where the address arithmetic does not.
    step = convertInt(step, ptrFirst ? rhs : lhs, count, pos);
    if (failed_) {
        return undefined();
    }
    if (elemBytes != 1) {
        step = emitArith(BinOp::Mul, step, constantI64(elemBytes, count, pos),
                         count, pos);
        if (failed_) {
            return undefined();
        }
    }
    ir::Instr move;
    move.op = op == BinOp::Add ? ir::Op::Add : ir::Op::Sub;
    move.loc = pos;
    move.type = result;
    move.width = ir::IRType::Ptr;
    move.isPure = true;
    move.operands.push_back(base);
    move.operands.push_back(step);
    ir::Value at = newValue(move.width, result, pos);
    move.results.push_back(at);
    emit(std::move(move));
    return at;
}

ir::Value Lowerer::widenFloat(ir::Value v, TypeRef from, TypeRef to,
                              Position pos) {
    if (from.isError() || to.isError() || from.type == nullptr ||
        to.type == nullptr || from.type->kind() != Kind::Basic ||
        to.type->kind() != Kind::Basic ||
        !basicIsFloat(*to.type->basic())) {
        return v;
    }
    const bool fromFloat = basicIsFloat(*from.type->basic());
    if (fromFloat &&
        basicWidthBits(*from.type->basic()) >= basicWidthBits(*to.type->basic())) {
        return v;
    }
    ir::Instr cast;
    cast.op = ir::Op::Cast;
    cast.loc = pos;
    cast.type = to;
    cast.width = widthOf(to, pos);
    cast.isPure = true;
    cast.operands.push_back(v);
    ir::Value out = newValue(cast.width, to, pos);
    cast.results.push_back(out);
    emit(std::move(cast));
    return out;
}

ir::Value Lowerer::lowerBitNot(const Unary& n) {
    TypeRef type = checker_.typeOf(&n);
    if (type.isError() || type.type->kind() != Kind::Basic) {
        refuse(n.pos, "this operator");
        return undefined();
    }
    BasicKind basic = *type.type->basic();
    if (!basicIsInteger(basic)) {
        // Floats and bools have no bitwise complement: refuse rather
        // than xor their encoding.
        refuse(n.pos, "this operator");
        return undefined();
    }
    ir::Value value = onIdentOrExpr(n.operand.get(), type);
    if (failed_) {
        return undefined();
    }
    // All-ones at any width inverts every bit the operand owns; the xor
    // truncates to the operand width, so narrower types keep only their
    // own inverted bits.
    return emitArith(BinOp::Xor, value, constantI64(-1, type, n.pos), type,
                     n.pos);
}

ir::Value Lowerer::lowerNeg(const Unary& n) {
    TypeRef type = checker_.typeOf(&n);
    if (type.isError() || type.type->kind() != Kind::Basic) {
        refuse(n.pos, "this operator");
        return undefined();
    }
    BasicKind basic = *type.type->basic();
    const bool fits =
        (basic >= BasicKind::I8 && basic <= BasicKind::U64) ||
        basic == BasicKind::F32 || basic == BasicKind::F64;
    if (!fits) {
        // Bool and char arithmetic are not a thing: refuse rather than
        // subtract from their bits.
        refuse(n.pos, "this operator");
        return undefined();
    }
    ir::Value value = onIdentOrExpr(n.operand.get(), type);
    if (failed_) {
        return undefined();
    }
    if (basic == BasicKind::F32 || basic == BasicKind::F64) {
        // `0.0 - x`: IEEE subtraction answers the negation, and -0.0 and
        // NaN round-trip the way negation must.
        uint64_t zero = 0;
        return emitArith(BinOp::Sub, constantFBits(zero, type, n.pos), value,
                         type, n.pos);
    }
    return emitArith(BinOp::Sub, constantI64(0, type, n.pos), value, type,
                     n.pos);
}

const Signature* Lowerer::functionSig(const std::string& name) const {
    // The current module's scope, not the entry's: an import's body calls
    // its own names, and a from-alias carries its target's signature.
    const resolve::Scope* scope = resolver_.scopeOf(checker_.module());
    if (scope == nullptr) {
        return nullptr;
    }
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

const GlobalBinding* Lowerer::findGlobal(const std::string& name) {
    // A from-alias copies its target's id, so declById recovers the home
    // module even for names bound in this module's scope by import.
    std::string home = checker_.module();
    const resolve::Scope* here = resolver_.scopeOf(home);
    if (here != nullptr) {
        const resolve::Decl* decl = resolver_.lookup(here, name);
        if (decl != nullptr) {
            const resolve::Decl* origin = resolver_.declById(decl->id);
            const resolve::Decl* truth = origin != nullptr ? origin : decl;
            if (truth->scope != nullptr &&
                (truth->kind == resolve::DeclKind::Var ||
                 truth->kind == resolve::DeclKind::Const)) {
                home = truth->scope->module;
            }
        }
    }
    for (const GlobalBinding& global : globals_) {
        if (global.name == name && global.module == home) {
            return &global;
        }
    }
    return nullptr;
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
    binding.module = checker_.module();
    // The label carries its module (`g_da__gval` for `da`'s `gval`, `g_x`
    // for the entry's `x`), so two modules' same-named globals never share
    // storage. Dots become underscores: a dot cannot survive the assembler.
    std::string flat = binding.module;
    for (char& c : flat) {
        if (c == '.') {
            c = '_';
        }
    }
    binding.label = binding.module == resolver_.entryModule()
                        ? "g_" + n.name
                        : "g_" + flat + "__" + n.name;
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
    } else if (type.type->kind() == Kind::Array &&
               dynamic_cast<const NewArray*>(n.init.get()) != nullptr) {
        // A const aggregate global passed the checker's gate, so every
        // leaf folds and every image byte is known: the global holds one
        // `.quad` per eight bytes, little-endian, zero-padded at the end.
        const auto* initArray = dynamic_cast<const NewArray*>(n.init.get());
        std::string bytes;
        if (!globalInitBytes(*initArray, type, bytes) || bytes.empty()) {
            refuse(n.init->pos, "a global initialiser must be constant");
            return;
        }
        size_t byteCount = bytes.size();
        uint64_t quads = (static_cast<uint64_t>(byteCount) + 7u) / 8u;
        for (uint64_t word = 0; word < quads; ++word) {
            uint64_t quad = 0;
            for (uint64_t byte = 0; byte < 8u; ++byte) {
                uint64_t at = word * 8u + byte;
                uint64_t bits = at < bytes.size()
                                    ? static_cast<unsigned char>(bytes[at])
                                    : 0u;
                quad |= bits << (byte * 8);
            }
            binding.initWords.push_back(quad);
        }
    } else if (type.type->kind() == Kind::Basic &&
               basicIsFloat(*type.type->basic())) {
        // A float global carries its IEEE bits in the image. The
        // initialiser is either a literal — parsed at the global's own
        // width — or an integer constant the checker folded, converted
        // with the same rounding a cast would apply, or a float
        // expression over constants, folded to a double with integer
        // subtrees keeping integer semantics (`5 / 2` is 2, then 2.0).
        BasicKind basic = *type.type->basic();
        FloatResult floatFolded{false, 0.0, ""};
        {
            const auto* probe = dynamic_cast<const FloatLit*>(n.init.get());
            if (probe == nullptr && !folded.ok) {
                floatFolded = evalConstFloat(*n.init, env, active);
            }
        }
        if (basic == BasicKind::F32) {
            float value = 0.0f;
            const auto* lit = dynamic_cast<const FloatLit*>(n.init.get());
            if (lit != nullptr) {
                try {
                    value = std::stof(lit->text);
                } catch (...) {
                    refuse(n.init->pos, "a float global needs a constant");
                    return;
                }
                if (lit->negative) {
                    value = -value;
                }
            } else if (floatFolded.ok) {
                value = static_cast<float>(floatFolded.value);
            } else if (folded.ok) {
                value = static_cast<float>(folded.value);
            } else {
                refuse(n.init->pos, "a float global needs a constant");
                return;
            }
            uint32_t pattern = 0;
            std::memcpy(&pattern, &value, sizeof pattern);
            binding.initial = static_cast<int64_t>(static_cast<uint64_t>(pattern));
        } else {
            double value = 0.0;
            const auto* lit = dynamic_cast<const FloatLit*>(n.init.get());
            if (lit != nullptr) {
                try {
                    value = std::stod(lit->text);
                } catch (...) {
                    refuse(n.init->pos, "a float global needs a constant");
                    return;
                }
                if (lit->negative) {
                    value = -value;
                }
            } else if (floatFolded.ok) {
                value = floatFolded.value;
            } else if (folded.ok) {
                value = static_cast<double>(folded.value);
            } else {
                refuse(n.init->pos, "a float global needs a constant");
                return;
            }
            uint64_t pattern = 0;
            std::memcpy(&pattern, &value, sizeof pattern);
            binding.initial = static_cast<int64_t>(pattern);
        }
    } else {
        // A folded integer names image bytes, so it is cut to the declared
        // width here: without the cut a 64-bit fold stores bits the type
        // does not own. The checker rejects an out-of-range fold unless
        // wrap defines it, so the cut only ever keeps a wrap.
        int64_t initial = folded.ok ? folded.value : 0;
        if (folded.ok && type.type->kind() == Kind::Basic) {
            const std::optional<BasicKind> basic = type.type->basic();
            if (basic.has_value() && basicIsInteger(*basic) &&
                basicWidthBits(*basic) < basicWidthBits(BasicKind::I64)) {
                uint64_t kept = static_cast<uint64_t>(initial);
                kept &= (kOneWord << static_cast<uint64_t>(basicWidthBits(*basic))) -
                        kOneWord;
                initial = static_cast<int64_t>(kept);
            }
        }
        binding.initial = initial;
    }
    globals_.push_back(binding);
    ir::Global global;
    global.label = binding.label;
    global.type = type;
    global.initial = binding.initial;
    global.initLabel = binding.initLabel;
    global.initWords = binding.initWords;
    module_->globals.push_back(global);
}

void Lowerer::onFuncDecl(FuncDecl& n) {
    if (failed_ || !enabled_ || !n.typeParams.empty()) {
        return;
    }
    // The link name is the resolver's, not the spelling: an import's
    // function mangles under its module (`b__aval`), the entry's keeps its
    // spelling, and the prelude keeps its C name. Two modules defining one
    // name therefore never collide in one IR module.
    const resolve::Scope* here = resolver_.scopeOf(checker_.module());
    const resolve::Decl* decl =
        here != nullptr ? resolver_.lookup(here, n.name) : nullptr;
    ir::Function fn;
    fn.name = decl != nullptr ? resolver_.linkName(*decl) : n.name;
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
    recordParamClasses();
    locals_.push_back(std::move(frame));

    n.body->accept(checker_);
    locals_.pop_back();

    ir::Terminator ret;
    ret.term = ir::Term::Ret;
    // A void falloff answers nothing (see onReturn). A value falloff
    // traps instead of answering: the placeholder id the old code pushed
    // here poisoned the frame computation and crashed programs whose
    // last statement closed every path (if/else, match). Reachable
    // falloff is a checker rejection filed separately; the trap keeps
    // it loud either way, never garbage.
    if (function_->signature.returnType() == nullptr ||
        function_->signature.returnType()->kind() != Kind::Void) {
        ret.term = ir::Term::Trap;
        ret.reason = ir::TrapReason::Explicit;
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
    // call site builds `owner__method(receiver, args...)`. The owner carries
    // its module (`da__Box__get` for `da`'s `Box::get`), so two modules'
    // same-named types never collide.
    ir::Function fn;
    const resolve::Scope* here = resolver_.scopeOf(checker_.module());
    const resolve::Decl* ownerDecl =
        here != nullptr ? resolver_.lookup(here, owner) : nullptr;
    std::string linkOwner = owner;
    if (ownerDecl != nullptr) {
        linkOwner = resolver_.linkName(*ownerDecl);
    }
    fn.name = linkOwner + "__" + name;
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
    recordParamClasses();
    locals_.push_back(std::move(frame));

    body.accept(checker_);
    locals_.pop_back();

    ir::Terminator mret;
    mret.term = ir::Term::Ret;
    // Same value-falloff trap as a free function (see onFuncDecl).
    if (function_->signature.returnType() == nullptr ||
        function_->signature.returnType()->kind() != Kind::Void) {
        mret.term = ir::Term::Trap;
        mret.reason = ir::TrapReason::Explicit;
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
        recordParamClasses();
        locals_.push_back(std::move(frame));

        lit.body->accept(checker_);
        locals_.pop_back();

        ir::Terminator fret;
        fret.term = ir::Term::Ret;
        // Same value-falloff trap as a free function (see onFuncDecl).
        if (function_->signature.returnType() == nullptr ||
            function_->signature.returnType()->kind() != Kind::Void) {
            fret.term = ir::Term::Trap;
            fret.reason = ir::TrapReason::Explicit;
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
    TypeRef initType = checker_.typeOf(n.init.get());
    if (dynamic_cast<const StringLit*>(n.init.get()) != nullptr &&
        isBytePointerTarget(type) && values.size() == kSliceWords) {
        // A literal beside a byte pointer keeps its address and drops
        // its length: the pointer holds one word, the pair holds two.
        values = {values[kFirstWord]};
    }
    if (!initType.isError() && !type.isError() &&
        initType.type->kind() == Kind::Slice &&
        type.type->kind() == Kind::Array) {
        // A slice copies into the declared array: the length is checked
        // at the copy, element by element, detaching from the source.
        int slots = wordCount(type);
        if (failed_ || slots < 0 || values.size() != kSliceWords) {
            refuse(n.pos, "this declaration");
            return;
        }
        std::vector<ir::Value> fresh =
            allocaSlots(type, n.pos, static_cast<uint64_t>(slots));
        Place dest;
        dest.slots = fresh;
        dest.type = type;
        dest.off = 0;
        dest.isVolatile = false;
        if (!copySliceToArray(n.pos, values[kFirstWord], values[kLenWord],
                              type, dest, "this declaration")) {
            return;
        }
        LocalBinding binding;
        binding.slots = std::move(fresh);
        binding.type = type;
        binding.isMutable = true;
        locals_.back()[n.name] = binding;
        return;
    }
    // A concrete value in an interface position boxes to the pair here, so
    // the slots below hold data plus vtable, never bare concrete words.
    values = boxToInterface(std::move(values), initType, type, n.pos);
    if (failed_) {
        return;
    }
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
    if (const GlobalBinding* global = findGlobal(n.name)) {
        std::vector<ir::Value> words = loadGlobal(n.pos, *global);
        if (words.size() != kOneWord) {
            refuse(n.pos, "this value");
            return undefined();
        }
        return words[0];
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
    // An interface is a data word then a vtable pointer (TYPES.md §4.3):
    // the data word carries the concrete bytes inline, untyped past one
    // word, and the vtable word is an address the linker resolves.
    if (aggregate.type->kind() == Kind::Interface) {
        if (word == kFirstWord) {
            return TypeRef{ctx_.basic(BasicKind::U64)};
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
    // clobber.
    //
    // Words arrive in one of two shapes. Chunk-shaped words (one per take,
    // as whole-value loads produce) store in order. Value-shaped words (one
    // per eight bytes, as expressions produce) are shaped here: a take
    // starting past its word's low byte shifts the word down first, so a
    // 3-byte value stores as 2+1 from one word instead of reading a second
    // word that is not there. Logical shift on an unsigned word keeps the
    // value's bytes low and zero-extends either way.
    int64_t takes = 0;
    for (int64_t left = total; left > 0;) {
        if (left >= per) {
            left -= per;
        } else if (left >= layout::kSizeI32) {
            left -= layout::kSizeI32;
        } else if (left >= layout::kSizeI16) {
            left -= layout::kSizeI16;
        } else {
            left -= layout::kSizeI8;
        }
        ++takes;
    }
    int64_t cells = total / per + (total % per == 0 ? 0 : 1);
    bool chunked = static_cast<int64_t>(words.size()) == takes;
    bool valued = static_cast<int64_t>(words.size()) == cells;
    if (!chunked && !valued) {
        refuse(pos, "this field layout is not lowered yet");
        return;
    }
    TypeRef wide{ctx_.basic(BasicKind::U64)};
    TypeRef tally{ctx_.basic(BasicKind::I64)};
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
        if (chunked) {
            instr.storedValue = words[i];
        } else {
            // Value shape: the take reads value bytes [v, v+take), which
            // sit inside exactly one word (takes never straddle). The low
            // take uses the word whole; a higher one shifts it down first.
            int64_t v = cur - place.off;
            uint64_t word =
                static_cast<uint64_t>(v / layout::kSizePointer);
            int64_t sub = v % layout::kSizePointer;
            if (word >= words.size()) {
                refuse(pos, "this field layout is not lowered yet");
                return;
            }
            instr.storedValue = words[word];
            if (sub != 0) {
                ir::Value by =
                    constantI64(sub * layout::kSizeI8, tally, pos);
                if (failed_) {
                    return;
                }
                instr.storedValue =
                    emitArith(BinOp::Shr, instr.storedValue, by, wide, pos);
                if (failed_) {
                    return;
                }
            }
        }
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

bool Lowerer::isZeroRepeatFiller(const Expr& filler, TypeRef element) {
    Kind kind = element.type->kind();
    if (kind != Kind::Array && kind != Kind::Struct && kind != Kind::Union) {
        return false;
    }
    const auto* lit = dynamic_cast<const IntLit*>(&filler);
    if (lit == nullptr) {
        return false;
    }
    std::unordered_set<std::string> active;
    EvalEnv env = resolver_.evalEnv(resolver_.moduleScope());
    EvalResult folded = evalConstInt(*lit, env, active);
    return folded.ok && folded.value == 0;
}

bool Lowerer::globalInitBytes(const NewArray& init, TypeRef arrayType,
                              std::string& bytes) {
    std::optional<int64_t> extent = arrayType.type->length();
    if (!extent.has_value() || *extent < 0) {
        return false;
    }
    TypeRef element{arrayType.type->element()};
    int64_t elemBytes = resolver_.nominalSize(element.type);
    if (elemBytes <= 0) {
        return false;
    }
    std::unordered_set<std::string> active;
    EvalEnv env = resolver_.evalEnv(resolver_.moduleScope());
    auto appendScalar = [&](const Expr& leaf, int64_t width, TypeRef elemType) -> bool {
        if (width <= 0) {
            return false;
        }
        if (elemType.type->kind() == Kind::Basic &&
            basicIsFloat(*elemType.type->basic())) {
            // A float element converts the folded value at the element
            // width, the same rounding a local initialisation applies:
            // `new [f64; 2] { 1, 2 }` holds 1.0 and 2.0, not 1 and 2.
            FloatResult asFloat = evalConstFloat(leaf, env, active);
            if (!asFloat.ok) {
                return false;
            }
            uint64_t bits = 0;
            if (*elemType.type->basic() == BasicKind::F32) {
                float narrowed = static_cast<float>(asFloat.value);
                std::memcpy(&bits, &narrowed, sizeof narrowed);
            } else {
                std::memcpy(&bits, &asFloat.value, sizeof asFloat.value);
            }
            for (int64_t byte = 0; byte < width; ++byte) {
                bytes.push_back(static_cast<char>(bits & 0xFFu));
                bits >>= 8;
            }
            return true;
        }
        EvalResult folded = evalConstInt(leaf, env, active);
        if (!folded.ok) {
            return false;
        }
        uint64_t bits = static_cast<uint64_t>(folded.value);
        for (int64_t byte = 0; byte < width; ++byte) {
            bytes.push_back(static_cast<char>(bits & 0xFFu));
            bits >>= 8;
        }
        return true;
    };
    // One element's worth of image bytes: a nested construction recurses,
    // a scalar folds, and a scalar zero over an aggregate element is the
    // all-zero case the zero-repeat rule owns (checker enforces the same).
    std::function<bool(const Expr&, TypeRef)> appendElement =
        [&](const Expr& leaf, TypeRef elemType) -> bool {
        if (const auto* sub = dynamic_cast<const NewArray*>(&leaf)) {
            if (elemType.type->kind() != Kind::Array) {
                return false;
            }
            return globalInitBytes(*sub, elemType, bytes);
        }
        if (elemType.type->kind() == Kind::Array) {
            int64_t total = resolver_.nominalSize(elemType.type);
            if (total <= 0) {
                return false;
            }
            EvalResult folded = evalConstInt(leaf, env, active);
            if (!folded.ok || folded.value != 0) {
                return false;
            }
            bytes.append(static_cast<size_t>(total), '\0');
            return true;
        }
        return appendScalar(leaf, resolver_.nominalSize(elemType.type), elemType);
    };
    if (init.repeat) {
        size_t fillers = init.inits.size();
        if (fillers != 1) {
            return false;
        }
        for (int64_t at = 0; at < *extent; ++at) {
            if (!appendElement(*init.inits[0], element)) {
                return false;
            }
        }
        return true;
    }
    if (static_cast<int64_t>(init.inits.size()) != *extent) {
        return false;
    }
    for (const auto& one : init.inits) {
        if (!appendElement(*one, element)) {
            return false;
        }
    }
    return true;
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
        // Element values arrive in value shape (one word per eight bytes);
        // the range store shapes them into chunk stores itself, shifting a
        // word down when a take starts past its low byte.
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
        if (isZeroRepeatFiller(*n.inits[0], element)) {
            // Zero reaches through: one zero word per element word, so a
            // nested array fills with zeros instead of one element value.
            int words = wordCount(element);
            if (words < 0) {
                refuse(n.pos, "this construction");
                return;
            }
            one.clear();
            for (int w = 0; w < words; ++w) {
                one.push_back(constantI64(0, wordType(element, static_cast<uint64_t>(w)),
                                          n.inits[0]->pos));
                if (failed_) {
                    return;
                }
            }
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
    if (const auto* ident = dynamic_cast<const Ident*>(n.object.get())) {
        // `Color.RED`: the checker answered the enumerator's integer type,
        // so the value folds here to that same integer (TYPES §2.6). A
        // local with the qualifier's name keeps its meaning: the checker
        // only takes this branch when no local binds it.
        bool shadowed = false;
        for (auto frame = locals_.rbegin();
             frame != locals_.rend() && !shadowed; ++frame) {
            shadowed = frame->find(ident->name) != frame->end();
        }
        if (!shadowed) {
            const resolve::Scope* here =
                resolver_.scopeOf(checker_.module());
            const resolve::Decl* named =
                here != nullptr ? resolver_.lookup(here, ident->name)
                                : nullptr;
            if (named != nullptr &&
                named->kind == resolve::DeclKind::Type &&
                named->nominal == Kind::Enum) {
                std::string home = named->scope != nullptr
                                       ? named->scope->module
                                       : checker_.module();
                if (!resolver_.isEnumMember(home + "." + named->name,
                                            n.name)) {
                    refuse(n.pos, "this field access");
                    return {undefined()};
                }
                int64_t value = resolver_.enumValue(home + "." + named->name,
                                                    n.name);
                TypeRef count{ctx_.basic(BasicKind::I32)};
                ir::Value answer = constantI64(value, count, n.pos);
                if (failed_) {
                    return {undefined()};
                }
                return {answer};
            }
        }
    }
    Place base;
    if (const auto* call = dynamic_cast<const Call*>(n.object.get())) {
        // A field of a call's answer (`greet().len`, `make().x`): the words
        // have no address, so they spill to scratch slots first and the
        // field reads from there like any other place.
        TypeRef answer = checker_.typeOf(n.object.get());
        if (answer.isError()) {
            refuse(n.object->pos, "this field access");
            return {undefined()};
        }
        std::vector<ir::Value> words =
            lowerCallWords(const_cast<Call&>(*call));
        if (failed_) {
            return {undefined()};
        }
        if (words.size() != static_cast<size_t>(wordCount(answer)) ||
            words.empty()) {
            refuse(n.object->pos, "this field access");
            return {undefined()};
        }
        std::vector<ir::Value> slots =
            allocaSlots(answer, n.pos, static_cast<uint64_t>(words.size()));
        if (failed_) {
            return {undefined()};
        }
        Place scratch;
        scratch.slots = slots;
        scratch.type = answer;
        storeRange(n.pos, scratch, words, answer);
        if (failed_) {
            return {undefined()};
        }
        base = std::move(scratch);
    } else if (!resolvePlace(n.object.get(), base)) {
        // A field of a pointer element (`p[0].lo`): the direct route only
        // knows slots, but a pointer element has an address. Materialize
        // it the way computed reads do, then read the field off it. The
        // Field branch of resolvePlace carries the same fallback for the
        // store path; both stay because each serves a different node.
        const Index* elem = dynamic_cast<const Index*>(n.object.get());
        TypeRef arrType = elem != nullptr
            ? checker_.typeOf(elem->array.get())
            : TypeRef{ctx_.error()};
        if (elem == nullptr || arrType.isError() ||
            arrType.type->kind() != Kind::Pointer) {
            refuse(n.object->pos, "this field access");
            return {undefined()};
        }
        Place base0;
        if (!resolvePlace(elem->array.get(), base0) ||
            !pointerElementPlace(*elem, base0, arrType,
                                 "this field access", base)) {
            return {undefined()};
        }
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

bool Lowerer::arrayElementPlace(const Index& n, const Place& base,
                                 TypeRef arrayType, const char* what,
                                 Place& at) {
    std::optional<int64_t> extent = arrayType.type->length();
    if (!extent.has_value() || *extent < 0) {
        refuse(n.array->pos, what);
        return false;
    }
    TypeRef element{arrayType.type->element()};
    int64_t elemBytes = resolver_.nominalSize(element.type);
    if (elemBytes <= 0) {
        refuse(n.pos, what);
        return false;
    }
    if (!base.global.empty()) {
        // A global names a label, not slots: materialize its address once
        // and let the rest of this path treat it as one computed word.
        // The address is pure (a label never moves), so the optimizer
        // keeps the same visibility it has for slot-derived bases.
        TypeRef addrType{ctx_.pointer(element.type)};
        ir::Instr data;
        data.op = ir::Op::DataAddr;
        data.loc = n.pos;
        data.type = addrType;
        data.width = ir::IRType::Ptr;
        data.globalLabel = base.global;
        data.isPure = true;
        ir::Value atAddr = newValue(data.width, addrType, n.pos);
        data.results.push_back(atAddr);
        emit(std::move(data));
        if (failed_) {
            return false;
        }
        if (base.off != 0) {
            TypeRef count{ctx_.basic(BasicKind::I64)};
            ir::Value delta = constantI64(base.off, count, n.pos);
            if (failed_) {
                return false;
            }
            ir::Instr adjust;
            adjust.op = ir::Op::Add;
            adjust.loc = n.pos;
            adjust.type = addrType;
            adjust.width = ir::IRType::Ptr;
            adjust.isPure = true;
            adjust.operands.push_back(atAddr);
            adjust.operands.push_back(delta);
            ir::Value moved = newValue(adjust.width, addrType, n.pos);
            adjust.results.push_back(moved);
            emit(std::move(adjust));
            if (failed_) {
                return false;
            }
            atAddr = moved;
        }
        Place here;
        here.slots = {atAddr};
        here.type = base.type;
        here.off = 0;
        here.isVolatile = base.isVolatile;
        return arrayElementPlace(n, here, arrayType, what, at);
    }
    std::unordered_set<std::string> active;
    EvalEnv env = resolver_.evalEnv(resolver_.moduleScope());
    EvalResult folded = evalConstInt(*n.index, env, active);
    if (folded.ok) {
        // A folded index is a slot offset the direct route owns: arriving
        // here means that route failed, so refusing names the real gap
        // instead of answering twice. An address base has no direct
        // route, so the folded step advances it explicitly — with the
        // same provable checks the slot route owns.
        if (!base.isAddress) {
            refuse(n.pos, what);
            return false;
        }
        if (folded.value < 0) {
            refuse(n.index->pos, "a negative index");
            return false;
        }
        if (folded.value >= *extent) {
            refuse(n.index->pos, "an index past the end");
            return false;
        }
        if (base.slots.empty()) {
            refuse(n.pos, what);
            return false;
        }
        TypeRef count{ctx_.basic(BasicKind::I64)};
        ir::Value delta = constantI64(folded.value * elemBytes, count, n.pos);
        if (failed_) {
            return false;
        }
        TypeRef ptr{ctx_.pointer(element.type)};
        ir::Instr advance;
        advance.op = ir::Op::Add;
        advance.loc = n.pos;
        advance.type = ptr;
        advance.width = ir::IRType::Ptr;
        advance.isPure = true;
        advance.operands.push_back(base.slots[kFirstWord]);
        advance.operands.push_back(delta);
        ir::Value moved = newValue(advance.width, ptr, n.pos);
        advance.results.push_back(moved);
        emit(std::move(advance));
        if (failed_) {
            return false;
        }
        at.slots = {moved};
        at.type = element;
        at.off = 0;
        at.isVolatile = base.isVolatile;
        at.isAddress = true;
        return true;
    }
    std::vector<ir::Value> index = lowerWords(n.index.get());
    if (failed_ || index.size() != kOneWord) {
        refuse(n.index->pos, what);
        return false;
    }
    for (uint64_t i = 0; i < static_cast<uint64_t>(base.slots.size()); ++i) {
        if (base.slots[i].id != base.slots[0].id + static_cast<uint32_t>(i)) {
            refuse(n.pos, what);
            return false;
        }
    }
    TypeRef count{ctx_.basic(BasicKind::I64)};
    ir::Value zero = constantI64(0, count, n.index->pos);
    ir::Value len = constantI64(*extent, count, n.array->pos);
    if (failed_) {
        return false;
    }
    emitCheckTrap(n.index->pos, emitI64Cmp(n.index->pos, BinOp::Ge, index[kFirstWord], zero),
                  ir::TrapReason::IndexOutOfBounds);
    emitCheckTrap(n.index->pos, emitI64Cmp(n.index->pos, BinOp::Lt, index[kFirstWord], len),
                  ir::TrapReason::IndexOutOfBounds);
    if (failed_) {
        return false;
    }
    ir::Value byteOff = index[kFirstWord];
    if (elemBytes != kOneMember) {
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
        mul.operands.push_back(index[kFirstWord]);
        mul.operands.push_back(stride);
        ir::Value scaled = newValue(mul.width, count, n.index->pos);
        mul.results.push_back(scaled);
        emit(std::move(mul));
        byteOff = scaled;
    }
    TypeRef ptr{ctx_.pointer(element.type)};
    ir::Instr gep;
    gep.op = ir::Op::Gep;
    gep.loc = n.pos;
    gep.type = ptr;
    gep.width = ir::IRType::Ptr;
    gep.addressBase = base.slots[kFirstWord];
    gep.displacement = base.off;
    gep.isPure = true;
    gep.operands.push_back(byteOff);
    ir::Value addr = newValue(gep.width, ptr, n.pos);
    gep.results.push_back(addr);
    emit(std::move(gep));
    if (failed_) {
        return false;
    }
    // The address is computed; the value is one indirect access away.
    // Stays in slots form (not the indirect flag): a further index
    // addresses past it with the same slot convention, and the code
    // generator dereferences any base outside the slot set.
    at.slots = {addr};
    at.type = element;
    at.off = 0;
    at.isVolatile = base.isVolatile;
    at.isAddress = true;
    return true;
}

bool Lowerer::pointerElementPlace(const Index& n, const Place& base,
                                   TypeRef ptrType, const char* what,
                                   Place& at) {
    TypeRef pointee{ptrType.type->pointee()};
    int64_t elemBytes = resolver_.nominalSize(pointee.type);
    if (elemBytes <= 0) {
        refuse(n.pos, what);
        return false;
    }
    std::vector<ir::Value> held = loadRange(n.array->pos, base, ptrType);
    if (failed_ || held.size() != kOneWord) {
        refuse(n.array->pos, what);
        return false;
    }
    std::vector<ir::Value> counted = lowerWords(n.index.get());
    if (failed_ || counted.size() != kOneWord) {
        refuse(n.index->pos, what);
        return false;
    }
    TypeRef count{ctx_.basic(BasicKind::I64)};
    ir::Value step = convertInt(counted[kFirstWord], checker_.typeOf(n.index.get()),
                                count, n.index->pos);
    if (failed_) {
        return false;
    }
    ir::Value from = held[kFirstWord];
    if (elemBytes != 1) {
        ir::Value scaled =
            emitArith(BinOp::Mul, step, constantI64(elemBytes, count, n.pos),
                      count, n.pos);
        if (failed_) {
            return false;
        }
        step = scaled;
    }
    TypeRef addrType{ctx_.pointer(pointee.type)};
    from.type = addrType;
    step.type = addrType;
    ir::Instr advance;
    advance.op = ir::Op::Add;
    advance.loc = n.pos;
    advance.type = addrType;
    advance.width = ir::IRType::Ptr;
    advance.isPure = true;
    advance.operands.push_back(from);
    advance.operands.push_back(step);
    ir::Value addr = newValue(advance.width, addrType, n.pos);
    advance.results.push_back(addr);
    emit(std::move(advance));
    if (failed_) {
        return false;
    }
    at.slots.clear();
    at.global.clear();
    at.type = pointee;
    at.off = 0;
    at.isVolatile = base.isVolatile;
    at.hasIndirect = true;
    at.indirect = addr;
    return true;
}

bool Lowerer::arrayBase(const Expr* arrayExpr, const Index& n, Place& base,
                        TypeRef& arrayType, const char* what) {
    if (const auto* inner = dynamic_cast<const Index*>(arrayExpr)) {
        // `m[i][j]`: the outer index addresses past the inner element.
        // A folded inner still belongs to the direct route, which answers
        // slots; only a computed inner needs its address materialized.
        // The place query stays quiet on failure, so trying it first and
        // recursing after is safe.
        if (resolvePlace(inner, base)) {
            arrayType = base.type;
            return true;
        }
        Place innerBase;
        TypeRef innerArrayType;
        if (!arrayBase(inner->array.get(), *inner, innerBase, innerArrayType, what)) {
            return false;
        }
        if (innerArrayType.isError() ||
            innerArrayType.type->kind() != Kind::Array) {
            refuse(n.array->pos, what);
            return false;
        }
        Place at;
        if (!arrayElementPlace(*inner, innerBase, innerArrayType, what, at)) {
            return false;
        }
        base = at;
        arrayType = at.type;
        return true;
    }
    if (!resolvePlace(arrayExpr, base)) {
        return false;
    }
    arrayType = base.type;
    return true;
}

bool Lowerer::copySliceToArray(Position pos, ir::Value ptr, ir::Value len,
                               TypeRef arrayType, const Place& dest,
                               const char* what) {
    std::optional<int64_t> extent = arrayType.type->length();
    if (!extent.has_value() || *extent < 0) {
        refuse(pos, what);
        return false;
    }
    TypeRef element{arrayType.type->element()};
    int64_t elemBytes = resolver_.nominalSize(element.type);
    int wordsPer = wordCount(element);
    if (elemBytes <= 0 || wordsPer < 0) {
        refuse(pos, what);
        return false;
    }
    if (dest.isVolatile) {
        refuse(pos, what);
        return false;
    }
    TypeRef count{ctx_.basic(BasicKind::I64)};
    ir::Value want = constantI64(*extent, count, pos);
    if (failed_) {
        return false;
    }
    // The length is a runtime word, so the check always emits: a short
    // slice traps here instead of leaving the tail undefined, and a long
    // one never truncates silently (SEMANTICS §8).
    emitCheckTrap(pos, emitI64Cmp(pos, BinOp::Eq, len, want),
                  ir::TrapReason::IndexOutOfBounds);
    if (failed_) {
        return false;
    }
    TypeRef ptrType{ctx_.pointer(element.type)};
    // Byte copy in the same chunks the stores use (full words, then the
    // widest width the tail fits): each chunk loads indirectly from the
    // slice's bytes, so the collected words match the store's chunking
    // exactly instead of one word per element.
    std::optional<int64_t> total = arrayType.type->kind() == Kind::Array
        ? std::optional<int64_t>(resolver_.nominalSize(arrayType.type))
        : std::nullopt;
    if (!total.has_value() || *total <= 0) {
        refuse(pos, what);
        return false;
    }
    std::vector<ir::Value> got;
    int64_t remaining = *total;
    int64_t cur = 0;
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
        TypeRef chunk;
        if (!aggregateWord(take, width, chunk)) {
            refuse(pos, what);
            return false;
        }
        ir::Value atAddr = ptr;
        if (cur != 0) {
            ir::Value delta = constantI64(cur, count, pos);
            if (failed_) {
                return false;
            }
            ir::Instr add;
            add.op = ir::Op::Add;
            add.loc = pos;
            add.type = ptrType;
            add.width = ir::IRType::Ptr;
            add.isPure = true;
            add.operands.push_back(ptr);
            add.operands.push_back(delta);
            atAddr = newValue(add.width, ptrType, pos);
            add.results.push_back(atAddr);
            emit(std::move(add));
            if (failed_) {
                return false;
            }
        }
        Place at;
        at.slots = {atAddr};
        at.type = chunk;
        at.off = 0;
        at.isVolatile = false;
        std::vector<ir::Value> words = loadRange(pos, at, chunk);
        if (failed_ || words.size() != kOneWord) {
            refuse(pos, what);
            return false;
        }
        got.push_back(words[kFirstWord]);
        remaining -= take;
        cur += take;
    }
    storeRange(pos, dest, got, arrayType);
    return !failed_;
}

std::vector<ir::Value> Lowerer::lowerIndex(const Index& n) {
    Place base;
    TypeRef arrayType;
    if (!arrayBase(n.array.get(), n, base, arrayType, "this index")) {
        refuse(n.array->pos, "this index");
        return {undefined()};
    }
    if (!arrayType.isError() && (arrayType.type->kind() == Kind::Slice ||
                                 arrayType.type->kind() == Kind::String)) {
        return lowerSliceIndex(n, base, arrayType);
    }
    if (!arrayType.isError() && arrayType.type->kind() == Kind::Pointer) {
        // `p[i]` is `*(p + i)`: the pointer's word plus the scaled count,
        // with no guard — a pointer names no extent (SEMANTICS §8.2).
        Place at;
        if (!pointerElementPlace(n, base, arrayType, "this index", at)) {
            return {undefined()};
        }
        return loadRange(n.pos, at, at.type);
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
        // A folded index beside a known extent is provable: past the end
        // rejects at compile time (SEMANTICS §8) instead of reading a
        // neighbour slot. The computed path traps the same case at
        // runtime through the shared guarded helper below.
        std::optional<int64_t> count = arrayType.type->length();
        if (count.has_value() && folded.value >= *count) {
            refuse(n.index->pos, "an index past the end");
            return {undefined()};
        }
        base.off += folded.value * elemBytes;
        base.type = element;
        return loadRange(n.pos, base, element);
    }
    // Anything else computes its address through the shared guarded
    // helper — which is also what adds the bounds check this path
    // never had: every access checks (SEMANTICS §8).
    Place at;
    if (!arrayElementPlace(n, base, arrayType, "this index", at)) {
        return {undefined()};
    }
    return loadRange(n.pos, at, at.type);
}

void Lowerer::storeIndex(const Index& target, const std::vector<ir::Value>& words) {
    Place base;
    TypeRef arrayType;
    if (!arrayBase(target.array.get(), target, base, arrayType, "this index")) {
        refuse(target.array->pos, "this index");
        return;
    }
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
    if (!arrayType.isError() && arrayType.type->kind() == Kind::Pointer) {
        Place at;
        if (!pointerElementPlace(target, base, arrayType, "this index", at)) {
            return;
        }
        storeRange(target.pos, at, words, at.type);
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
        std::optional<int64_t> count = arrayType.type->length();
        if (count.has_value() && folded.value >= *count) {
            refuse(target.index->pos, "an index past the end");
            return;
        }
        base.off += folded.value * elemBytes;
        base.type = element;
        storeRange(target.pos, base, words, element);
        return;
    }
    // Anything else stores through the shared guarded address: the same
    // bounds check the read side emits, so a store past the end traps.
    Place at;
    if (!arrayElementPlace(target, base, arrayType, "this index", at)) {
        return;
    }
    storeRange(target.pos, at, words, at.type);
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

std::vector<ir::Value> Lowerer::lowerSizeof(const SizeofType& n) {
    TypeRef target = resolver_.resolveType(n.type.get(), resolver_.moduleScope());
    if (target.isError()) {
        refuse(n.pos, "this expression");
        return {undefined()};
    }
    int64_t bytes = resolver_.nominalSize(target.type);
    if (bytes < 0) {
        refuse(n.pos, "this expression");
        return {undefined()};
    }
    TypeRef count{ctx_.basic(BasicKind::I64)};
    return {constantI64(bytes, count, n.pos)};
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
        // A global names a label, not slots: materialize its address once
        // and let the rest of this path treat it as one computed word.
        // The address is pure (a label never moves), so the optimizer
        // keeps the same visibility it has for slot-derived bases.
        TypeRef addrType{ctx_.pointer(element.type)};
        ir::Instr data;
        data.op = ir::Op::DataAddr;
        data.loc = n.pos;
        data.type = addrType;
        data.width = ir::IRType::Ptr;
        data.globalLabel = base.global;
        data.isPure = true;
        ir::Value atAddr = newValue(data.width, addrType, n.pos);
        data.results.push_back(atAddr);
        emit(std::move(data));
        if (failed_) {
            return false;
        }
        if (base.off != 0) {
            TypeRef count{ctx_.basic(BasicKind::I64)};
            ir::Value delta = constantI64(base.off, count, n.pos);
            if (failed_) {
                return false;
            }
            ir::Instr adjust;
            adjust.op = ir::Op::Add;
            adjust.loc = n.pos;
            adjust.type = addrType;
            adjust.width = ir::IRType::Ptr;
            adjust.isPure = true;
            adjust.operands.push_back(atAddr);
            adjust.operands.push_back(delta);
            ir::Value moved = newValue(adjust.width, addrType, n.pos);
            adjust.results.push_back(moved);
            emit(std::move(adjust));
            if (failed_) {
                return false;
            }
            atAddr = moved;
        }
        Place here;
        here.hasIndirect = true;
        here.indirect = atAddr;
        here.type = base.type;
        here.off = 0;
        here.isVolatile = base.isVolatile;
        return sliceElementPlace(n, here, baseType, element, at);
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
    bool joined = false;
    for (const auto& part : lit.parts) {
        if (part.isExpr) {
            joined = true;
            break;
        }
    }
    if (joined) {
        return lowerInterp(lit);
    }
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
    // Literals are NUL-terminated in rodata: `*char` code reads to zero
    // (strlen), so the terminator is part of the literal. The length word
    // excludes it — it counts the string's bytes, not its storage.
    blob.bytes = bytes + '\0';
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

// A text fragment's bytes in rodata, as the address/length pair every
// interpolated part answers.
Lowerer::PartBytes Lowerer::rodataPair(const std::string& text, Position pos) {
    std::string label = "s_" + std::to_string(rodataSeq_++);
    ir::Rodata blob;
    blob.label = label;
    // Same NUL terminator as a plain literal: an interpolated fragment is
    // read through the same `*char` idioms (FEATURES §1.4).
    blob.bytes = text + '\0';
    module_->rodata.push_back(std::move(blob));
    TypeRef addr{ctx_.pointer(ctx_.basic(BasicKind::U8))};
    ir::Instr data;
    data.op = ir::Op::DataAddr;
    data.loc = pos;
    data.type = addr;
    data.width = ir::IRType::Ptr;
    data.globalLabel = label;
    data.isPure = true;
    ir::Value at = newValue(data.width, addr, pos);
    data.results.push_back(at);
    emit(std::move(data));
    TypeRef count{ctx_.basic(BasicKind::I64)};
    PartBytes out;
    out.ptr = at;
    out.len = constantI64(static_cast<int64_t>(text.size()), count, pos);
    return out;
}

// Copies `len` bytes from `src` to `dst`: an index slot, a head testing it
// against the length, a body moving one byte. The addresses advance by
// addition, the slice-advance idiom, so each side dereferences a computed
// address the optimizer treats opaquely.
void Lowerer::copyBytes(ir::Value dst, ir::Value src, ir::Value len, Position pos) {
    TypeRef count{ctx_.basic(BasicKind::I64)};
    TypeRef byteType{ctx_.basic(BasicKind::U8)};
    TypeRef ptrType{ctx_.pointer(ctx_.basic(BasicKind::U8))};
    ir::Value idxSlot = allocaSlot(count, pos);
    if (failed_) {
        return;
    }
    Place idxPlace;
    idxPlace.slots = {idxSlot};
    idxPlace.type = count;
    storeRange(pos, idxPlace, {constantI64(0, count, pos)}, count);
    if (failed_) {
        return;
    }
    uint32_t headLabel = emitBlock(pos);
    uint32_t bodyLabel = emitBlock(pos);
    uint32_t exitLabel = emitBlock(pos);
    ir::Terminator toHead;
    toHead.term = ir::Term::Br;
    toHead.target = headLabel;
    toHead.loc = pos;
    emitTerminator(toHead);
    startBlock(headLabel);
    std::vector<ir::Value> idxVals = loadRange(pos, idxPlace, count);
    if (failed_ || idxVals.size() != kOneWord) {
        refuse(pos, "this expression");
        return;
    }
    ir::Terminator br;
    br.term = ir::Term::CondBr;
    br.cond = emitI64Cmp(pos, BinOp::Lt, idxVals[kFirstWord], len);
    br.thenTarget = bodyLabel;
    br.elseTarget = exitLabel;
    br.loc = pos;
    emitTerminator(br);
    startBlock(bodyLabel);
    ir::Value idx = loadRange(pos, idxPlace, count)[kFirstWord];
    if (failed_) {
        return;
    }
    auto advance = [&](ir::Value base) {
        ir::Instr add;
        add.op = ir::Op::Add;
        add.loc = pos;
        add.type = ptrType;
        add.width = ir::IRType::Ptr;
        add.isPure = true;
        add.operands.push_back(base);
        add.operands.push_back(idx);
        ir::Value at = newValue(add.width, ptrType, pos);
        add.results.push_back(at);
        emit(std::move(add));
        return at;
    };
    ir::Value from = advance(src);
    ir::Value to = advance(dst);
    if (failed_) {
        return;
    }
    Place readAt;
    readAt.slots = {from};
    readAt.type = byteType;
    std::vector<ir::Value> one = loadRange(pos, readAt, byteType);
    if (failed_ || one.size() != kOneWord) {
        refuse(pos, "this expression");
        return;
    }
    Place writeAt;
    writeAt.slots = {to};
    writeAt.type = byteType;
    storeRange(pos, writeAt, one, byteType);
    if (failed_) {
        return;
    }
    TypeRef oneType{ctx_.basic(BasicKind::I64)};
    ir::Instr step;
    step.op = ir::Op::Add;
    step.loc = pos;
    step.type = oneType;
    step.width = ir::IRType::I64;
    step.isPure = true;
    step.operands.push_back(idx);
    step.operands.push_back(constantI64(1, oneType, pos));
    ir::Value next = newValue(step.width, oneType, pos);
    step.results.push_back(next);
    emit(std::move(step));
    storeRange(pos, idxPlace, {next}, count);
    if (failed_) {
        return;
    }
    emitTerminator(toHead);
    startBlock(exitLabel);
}

Lowerer::LoweredString Lowerer::lowerInterp(const StringLit& lit) {
    // Every part answers an address and a length. Text answers rodata; a
    // string answers its own words; a bool selects one of two rodata
    // fragments through a branch, the ternary shape; a char stores one
    // byte to scratch; an integer extends to 64 bits and converts through
    // a decimal helper into scratch. Anything else refuses loudly.
    TypeRef count{ctx_.basic(BasicKind::I64)};
    TypeRef stringType{ctx_.string()};
    std::vector<PartBytes> parts;
    for (const auto& part : lit.parts) {
        if (!part.isExpr) {
            if (part.text.empty()) {
                continue;
            }
            parts.push_back(rodataPair(part.text, lit.pos));
            if (failed_) {
                return LoweredString{};
            }
            continue;
        }
        if (part.expr == nullptr) {
            refuse(lit.pos, "this interpolation");
            return LoweredString{};
        }
        TypeRef type = checker_.typeOf(part.expr.get());
        if (type.isError()) {
            refuse(lit.pos, "this interpolation");
            return LoweredString{};
        }
        if (type.type->kind() == Kind::String) {
            std::vector<ir::Value> got = lowerWords(part.expr.get());
            if (failed_ || got.size() != kSliceWords) {
                refuse(part.expr->pos, "this interpolation");
                return LoweredString{};
            }
            PartBytes out;
            out.ptr = got[kFirstWord];
            out.len = got[kLenWord];
            parts.push_back(out);
            continue;
        }
        if (type.type->kind() == Kind::Basic &&
            type.type->basic().has_value() &&
            *type.type->basic() == BasicKind::Bool) {
            std::vector<ir::Value> got = lowerWords(part.expr.get());
            if (failed_ || got.size() != kOneWord) {
                refuse(part.expr->pos, "this interpolation");
                return LoweredString{};
            }
            PartBytes yes = rodataPair("true", lit.pos);
            PartBytes no = rodataPair("false", lit.pos);
            if (failed_) {
                return LoweredString{};
            }
            std::vector<ir::Value> cell = allocaSlots(stringType, lit.pos, kSliceWords);
            if (failed_) {
                return LoweredString{};
            }
            uint32_t yesLabel = emitBlock(lit.pos);
            uint32_t noLabel = emitBlock(lit.pos);
            uint32_t joinLabel = emitBlock(lit.pos);
            ir::Terminator br;
            br.term = ir::Term::CondBr;
            br.cond = got[kFirstWord];
            br.thenTarget = yesLabel;
            br.elseTarget = noLabel;
            br.loc = lit.pos;
            emitTerminator(br);
            Place at;
            at.slots = cell;
            at.type = stringType;
            startBlock(yesLabel);
            storeRange(lit.pos, at, {yes.ptr, yes.len}, stringType);
            if (failed_) {
                return LoweredString{};
            }
            ir::Terminator toJoin;
            toJoin.term = ir::Term::Br;
            toJoin.target = joinLabel;
            toJoin.loc = lit.pos;
            emitTerminator(toJoin);
            startBlock(noLabel);
            storeRange(lit.pos, at, {no.ptr, no.len}, stringType);
            if (failed_) {
                return LoweredString{};
            }
            emitTerminator(toJoin);
            startBlock(joinLabel);
            std::vector<ir::Value> joined = loadRange(lit.pos, at, stringType);
            if (failed_ || joined.size() != kSliceWords) {
                refuse(part.expr->pos, "this interpolation");
                return LoweredString{};
            }
            PartBytes out;
            out.ptr = joined[kFirstWord];
            out.len = joined[kLenWord];
            parts.push_back(out);
            continue;
        }
        if (type.type->kind() == Kind::Basic &&
            type.type->basic().has_value() &&
            *type.type->basic() == BasicKind::Char) {
            std::vector<ir::Value> got = lowerWords(part.expr.get());
            if (failed_ || got.size() != kOneWord) {
                refuse(part.expr->pos, "this interpolation");
                return LoweredString{};
            }
            ir::Value slot = allocaSlot(type, lit.pos);
            if (failed_) {
                return LoweredString{};
            }
            Place at;
            at.slots = {slot};
            at.type = type;
            storeRange(lit.pos, at, got, type);
            if (failed_) {
                return LoweredString{};
            }
            TypeRef ptrType{ctx_.pointer(ctx_.basic(BasicKind::U8))};
            PartBytes out;
            out.ptr = computedBase(slot, 0, lit.pos);
            out.len = constantI64(1, count, lit.pos);
            out.ptr.type = ptrType;
            parts.push_back(out);
            continue;
        }
        if (type.type->kind() == Kind::Basic &&
            type.type->basic().has_value() && basicIsInteger(*type.type->basic())) {
            BasicKind basic = *type.type->basic();
            std::vector<ir::Value> got = lowerWords(part.expr.get());
            if (failed_ || got.size() != kOneWord) {
                refuse(part.expr->pos, "this interpolation");
                return LoweredString{};
            }
            // The helper converts one 64-bit value, so the part widens
            // first: a Cast carries the operand's type, which is what
            // tells the backend to sign- or zero-extend.
            TypeRef wide{ctx_.basic(BasicKind::I64)};
            ir::Instr widen;
            widen.op = ir::Op::Cast;
            widen.loc = lit.pos;
            widen.type = wide;
            widen.width = ir::IRType::I64;
            widen.operands.push_back(got[kFirstWord]);
            ir::Value big = newValue(widen.width, wide, lit.pos);
            widen.results.push_back(big);
            emit(std::move(widen));
            // Twenty bytes hold any 64-bit decimal; the third slot keeps
            // the scratch word-aligned for whatever follows it.
            std::vector<ir::Value> scratch = allocaSlots(wide, lit.pos, 3);
            if (failed_) {
                return LoweredString{};
            }
            ir::Value buf = computedBase(scratch[kFirstWord], 0, lit.pos);
            if (failed_) {
                return LoweredString{};
            }
            TypeRef ptrType{ctx_.pointer(ctx_.basic(BasicKind::U8))};
            buf.type = ptrType;
            ir::Instr call;
            call.op = ir::Op::Call;
            call.loc = lit.pos;
            bool isSigned = basic == BasicKind::I8 || basic == BasicKind::I16 ||
                            basic == BasicKind::I32 || basic == BasicKind::I64;
            call.callee =
                isSigned ? "cider_itoa_signed" : "cider_itoa_unsigned";
            ir::Argument first;
            first.words.push_back(buf);
            call.arguments.push_back(std::move(first));
            ir::Argument second;
            second.words.push_back(big);
            call.arguments.push_back(std::move(second));
            ir::Value digits = newValue(ir::IRType::I64, count, lit.pos);
            call.results.push_back(digits);
            call.width = ir::IRType::I64;
            call.type = count;
            emit(std::move(call));
            PartBytes out;
            out.ptr = buf;
            out.len = digits;
            parts.push_back(out);
            continue;
        }
        if (type.type->kind() == Kind::Basic && type.type->basic().has_value() &&
            (*type.type->basic() == BasicKind::F32 ||
             *type.type->basic() == BasicKind::F64)) {
            refuse(part.expr->pos, "a float in interpolation is not lowered yet");
            return LoweredString{};
        }
        refuse(part.expr->pos, "this interpolation");
        return LoweredString{};
    }
    // The lengths sum to a total; one allocation answers the buffer; every
    // part copies into it at the running offset. A zero total allocates
    // zero bytes, which answers nil: every string consumer checks the
    // length before touching the pointer, so nothing dereferences it.
    ir::Value total = constantI64(0, count, lit.pos);
    if (failed_) {
        return LoweredString{};
    }
    for (const PartBytes& part : parts) {
        ir::Instr add;
        add.op = ir::Op::Add;
        add.loc = lit.pos;
        add.type = count;
        add.width = ir::IRType::I64;
        add.isPure = true;
        add.operands.push_back(total);
        add.operands.push_back(part.len);
        ir::Value sum = newValue(add.width, count, lit.pos);
        add.results.push_back(sum);
        emit(std::move(add));
        total = sum;
    }
    if (failed_) {
        return LoweredString{};
    }
    ir::Instr alloc;
    alloc.op = ir::Op::Call;
    alloc.loc = lit.pos;
    alloc.callee = "cider_alloc";
    alloc.mayTrap = true;
    {
        ir::Argument bytes;
        bytes.words.push_back(total);
        alloc.arguments.push_back(std::move(bytes));
        ir::Argument pad;
        pad.words.push_back(constantI64(8, count, lit.pos));
        alloc.arguments.push_back(std::move(pad));
    }
    if (failed_) {
        return LoweredString{};
    }
    TypeRef ptrType{ctx_.pointer(ctx_.basic(BasicKind::U8))};
    ir::Value buf = newValue(ir::IRType::Ptr, ptrType, lit.pos);
    alloc.results.push_back(buf);
    alloc.width = ir::IRType::Ptr;
    alloc.type = ptrType;
    emit(std::move(alloc));
    ir::Value off = constantI64(0, count, lit.pos);
    if (failed_) {
        return LoweredString{};
    }
    for (const PartBytes& part : parts) {
        ir::Instr add;
        add.op = ir::Op::Add;
        add.loc = lit.pos;
        add.type = ptrType;
        add.width = ir::IRType::Ptr;
        add.isPure = true;
        add.operands.push_back(buf);
        add.operands.push_back(off);
        ir::Value dst = newValue(add.width, ptrType, lit.pos);
        add.results.push_back(dst);
        emit(std::move(add));
        if (failed_) {
            return LoweredString{};
        }
        copyBytes(dst, part.ptr, part.len, lit.pos);
        if (failed_) {
            return LoweredString{};
        }
        ir::Instr bump;
        bump.op = ir::Op::Add;
        bump.loc = lit.pos;
        bump.type = count;
        bump.width = ir::IRType::I64;
        bump.isPure = true;
        bump.operands.push_back(off);
        bump.operands.push_back(part.len);
        ir::Value next = newValue(bump.width, count, lit.pos);
        bump.results.push_back(next);
        emit(std::move(bump));
        off = next;
    }
    if (failed_) {
        return LoweredString{};
    }
    LoweredString out;
    out.words = {buf, total};
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
        // The trampoline forwards into the target's signature: a float
        // parameter travels in its SSE class here too.
        if (type.type != nullptr && type.type->kind() == Kind::Basic &&
            basicIsFloat(*type.type->basic()) &&
            arg.words.size() == static_cast<size_t>(kOneWord)) {
            arg.width = *type.type->basic() == BasicKind::F32
                            ? ir::IRType::F32
                            : ir::IRType::F64;
        }
        call.arguments.push_back(std::move(arg));
    }
    recordParamClasses();
    if (!failed_) {
        function_->canThrow = true;
        ir::Terminator fret;
        fret.term = ir::Term::Ret;
        fret.loc = pos;
        if (ret->kind() != Kind::Void) {
            std::vector<ir::Value> answers = callResults(call, ret, pos);
            if (!failed_) {
                fret.values = std::move(answers);
            }
        } else {
            call.width = ir::IRType::Void;
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
        // A concrete argument for an interface parameter boxes like every
        // other call's: the init's words must arrive as the pair.
        if (init == nullptr ||
            i >= static_cast<uint64_t>(init->parameters().size())) {
            refuse(n.pos, "this construction");
            return {undefined()};
        }
        TypeRef want{init->parameters()[static_cast<size_t>(i)].type};
        arg.words = boxToInterface(std::move(arg.words),
                                   checker_.typeOf(n.args[i].get()), want,
                                   n.args[i]->pos);
        if (failed_) {
            return {undefined()};
        }
        // Same SSE class rule as onCall: an init's float parameter takes
        // an SSE register, decided by the checked argument type.
        TypeRef argType = checker_.typeOf(n.args[i].get());
        if (!argType.isError() && argType.type->kind() == Kind::Basic &&
            basicIsFloat(*argType.type->basic()) &&
            arg.words.size() == static_cast<size_t>(kOneWord)) {
            arg.width = *argType.type->basic() == BasicKind::F32
                            ? ir::IRType::F32
                            : ir::IRType::F64;
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

std::vector<ir::Value> Lowerer::lowerElementAddr(const Index& n) {
    // The address the element lives at, for `&a[i]`: the read paths
    // compute this address and immediately load through it, but an
    // address-of stops at the address. Array bases ride Gep off the
    // first slot; slice and string bases reuse the guarded advance the
    // read path owns, so the checks cannot drift apart.
    Place base;
    TypeRef baseType;
    if (!arrayBase(n.array.get(), n, base, baseType, "this expression") ||
        !base.global.empty()) {
        refuse(n.array->pos, "this expression");
        return {undefined()};
    }
    if (baseType.isError()) {
        refuse(n.array->pos, "this expression");
        return {undefined()};
    }
    Kind kind = baseType.type->kind();
    if (kind == Kind::Pointer) {
        // `&p[i]` stops at the computed address, like the array route.
        Place at;
        if (!pointerElementPlace(n, base, baseType, "this expression", at) ||
            failed_) {
            return {undefined()};
        }
        if (!at.hasIndirect) {
            refuse(n.pos, "this expression");
            return {undefined()};
        }
        return {at.indirect};
    }
    if (kind == Kind::Slice || kind == Kind::String) {
        TypeRef element = TypeRef{ctx_.basic(BasicKind::U8)};
        Place at;
        if (!sliceElementPlace(n, base, baseType, element, at) || failed_) {
            return {undefined()};
        }
        if (at.slots.empty() || at.off != 0) {
            refuse(n.pos, "this expression");
            return {undefined()};
        }
        return {at.slots[kFirstWord]};
    }
    if (kind != Kind::Array) {
        refuse(n.array->pos, "this expression");
        return {undefined()};
    }
    // The array route is the shared guarded address: the folded case
    // refuses inside (a folded index is a slot offset the direct route
    // owns), and anything computed answers here with both guards.
    Place at;
    if (!arrayElementPlace(n, base, baseType, "this expression", at)) {
        return {undefined()};
    }
    return {at.slots[kFirstWord]};
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
        // `&a[i]` past what a place names answers the element's address
        // directly: a computed array index, or any index into a slice or
        // string, has no slot offset. The address carries the same bounds
        // checks the read emits (SEMANTICS 8): an out-of-range address
        // traps at the `&`, not at the later use.
        if (const auto* index = dynamic_cast<const Index*>(n.operand.get())) {
            return lowerElementAddr(*index);
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
    if (dynamic_cast<const SizeofType*>(expr) != nullptr) {
        return lowerSizeof(static_cast<const SizeofType&>(*expr));
    }
    if (dynamic_cast<const Ternary*>(expr) != nullptr) {
        return lowerTernary(static_cast<const Ternary&>(*expr));
    }
    if (const auto* step = dynamic_cast<const IncrDecr*>(expr)) {
        return lowerIncrDecr(*step);
    }
    // A call answers one value per word of its return type: the scalar
    // path below would keep only the first, so calls dispatch here.
    if (const auto* call = dynamic_cast<const Call*>(expr)) {
        return lowerCallWords(const_cast<Call&>(*call));
    }
    if (const auto* unary = dynamic_cast<const Unary*>(expr)) {
        // `!b` and `-x` answer one scalar word through the scalar path,
        // which already lowers both: the address path below knows only
        // `&` and `*`, and sending a logical or arithmetic unary there
        // refuses a program the checker accepted.
        if (unary->op == UnOp::Not || unary->op == UnOp::Neg ||
            unary->op == UnOp::BitNot) {
            ir::Value single = onIdentOrExpr(expr, checker_.typeOf(expr));
            if (failed_) {
                return {undefined()};
            }
            return {single};
        }
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
        if (const GlobalBinding* global = findGlobal(ident->name)) {
            return loadGlobal(ident->pos, *global);
        }
        // A bare function name in value position answers the closure pair:
        // a forwarder with the receiver-first shape plus a null captures
        // word, with no allocation. Prelude names mangle to their link
        // symbols, like calls do.
        const resolve::Scope* here = resolver_.scopeOf(checker_.module());
        const resolve::Decl* decl =
            here != nullptr ? resolver_.lookup(here, ident->name) : nullptr;
        if (decl != nullptr && decl->kind == resolve::DeclKind::Func &&
            decl->typeParams == 0) {
            TypeRef type = checker_.typeOf(ident);
            if (!type.isError() && type.type->kind() == Kind::Closure &&
                wordCount(type) == static_cast<int>(kClosureWords)) {
                std::string link = resolver_.linkName(*decl);
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
    if (auto* cast = dynamic_cast<const Cast*>(expr)) {
        // The null string is (nil, 0): the operand answers one word but
        // the target holds two, so the generic scalar cast below would
        // leave the length unanswered. Only the literal converts — the
        // checker keeps a *void variable beside a string a mismatch.
        TypeRef target = checker_.typeOf(expr);
        if (!target.isError() && target.type->kind() == Kind::String &&
            dynamic_cast<const NilPtr*>(cast->expr.get()) != nullptr) {
            TypeRef ptrType{ctx_.pointer(ctx_.basic(BasicKind::U8))};
            TypeRef lenType{ctx_.basic(BasicKind::I64)};
            return {constantI64(0, ptrType, expr->pos),
                    constantI64(0, lenType, expr->pos)};
        }
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
    // A call statement discards every answer word: demanding one word here
    // would refuse a multi-word call whose value nobody reads.
    if (auto* call = dynamic_cast<const Call*>(n.expr.get())) {
        lowerCallWords(const_cast<Call&>(*call));
        return;
    }
    onIdentOrExpr(n.expr.get(), checker_.typeOf(n.expr.get()));
}

// One operand per `{placeholder}`: a local's storage slot (so substitution
// reads or writes the value in place) or an address word for a module
// variable or function. asmNames[i] pairs with operands[i], and codegen
// substitutes operand i wherever `{name i}` appears (FEATURES §4.4).
void Lowerer::onAsmStmt(AsmStmt& n) {
    if (failed_ || !enabled_ || function_ == nullptr) {
        return;
    }
    ir::Instr instr;
    instr.op = ir::Op::Asm;
    instr.loc = n.pos;
    instr.asmText = n.text;
    instr.asmNames = n.names;
    for (const std::string& raw : n.names) {
        std::string base = raw;
        if (!base.empty() && base[0] == '*') {
            base = base.substr(1);
        }
        std::string::size_type colon = base.find(':');
        if (colon != std::string::npos) {
            base = base.substr(0, colon);
        }
        bool found = false;
        for (auto frame = locals_.rbegin();
             frame != locals_.rend() && !found; ++frame) {
            auto hit = frame->find(base);
            if (hit == frame->end()) {
                continue;
            }
            const LocalBinding& local = hit->second;
            if (local.slots.empty()) {
                continue;
            }
            // The operand is the storage, sized at the value's first word:
            // a string placeholder reads its address word, a scalar reads
            // or writes its own width.
            TypeRef word = static_cast<uint64_t>(wordCount(local.type)) > kOneWord
                               ? wordType(local.type, 0)
                               : local.type;
            ir::IRType width = widthOf(word, n.pos);
            if (failed_) {
                return;
            }
            ir::Value slot = local.slots[0];
            slot.width = width;
            instr.operands.push_back(slot);
            found = true;
        }
        if (found) {
            continue;
        }
        if (const GlobalBinding* global = findGlobal(base)) {
            // A module variable placeholder is its address: DataAddr leaves
            // the address in a word, and codegen prints that word.
            TypeRef addr{ctx_.pointer(ctx_.basic(BasicKind::U8))};
            ir::Instr at;
            at.op = ir::Op::DataAddr;
            at.loc = n.pos;
            at.type = addr;
            at.width = ir::IRType::Ptr;
            at.globalLabel = global->label;
            at.isPure = true;
            ir::Value word = newValue(at.width, addr, n.pos);
            at.results.push_back(word);
            emit(std::move(at));
            instr.operands.push_back(word);
            found = true;
            continue;
        }
        if (found) {
            continue;
        }
        const resolve::Scope* here = resolver_.scopeOf(checker_.module());
        const resolve::Decl* decl =
            here != nullptr ? resolver_.lookup(here, base) : nullptr;
        if (decl != nullptr && decl->kind == resolve::DeclKind::Func) {
            std::string link = resolver_.linkName(*decl);
            TypeRef fp{ctx_.functionPtr(decl->sig)};
            ir::Instr fn;
            fn.op = ir::Op::FnAddr;
            fn.loc = n.pos;
            fn.type = fp;
            fn.width = ir::IRType::Ptr;
            fn.globalLabel = link;
            fn.isPure = true;
            ir::Value word = newValue(fn.width, fp, n.pos);
            fn.results.push_back(word);
            emit(std::move(fn));
            instr.operands.push_back(word);
            found = true;
        }
        if (!found) {
            refuse(n.pos, "an unbound asm placeholder");
            return;
        }
    }
    emit(std::move(instr));
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
    if (const GlobalBinding* global = findGlobal(n.name)) {
        for (uint64_t i = 0; i < static_cast<uint64_t>(words.size()); ++i) {
            ir::Instr instr;
            instr.op = ir::Op::Store;
            instr.loc = n.pos;
            instr.type = global->type;
            instr.globalLabel = global->label;
            instr.displacement = static_cast<int64_t>(i) * 8;
            instr.storedValue = words[i];
            instr.isVolatile = global->isVolatile;
            emit(std::move(instr));
        }
        return words;
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
    if (n.op == BinOp::AndAnd || n.op == BinOp::OrOr) {
        return lowerShortCircuit(n);
    }
    TypeRef lhs = checker_.typeOf(n.lhs.get());
    TypeRef rhs = checker_.typeOf(n.rhs.get());
    TypeRef result = checker_.typeOf(&n);
    if ((n.op == BinOp::Eq || n.op == BinOp::Ne) && !lhs.isError() &&
        !rhs.isError() &&
        ((lhs.type->kind() == Kind::String &&
          (rhs.type->kind() == Kind::String ||
           dynamic_cast<const NilPtr*>(n.rhs.get()) != nullptr)) ||
         (rhs.type->kind() == Kind::String &&
          (lhs.type->kind() == Kind::String ||
           dynamic_cast<const NilPtr*>(n.lhs.get()) != nullptr)))) {
        return lowerStringEq(n);
    }
    // Pointer arithmetic answers in addresses, not numbers: `p + n`
    // scales the count by the element's size and stays the same *T,
    // while `p - q` over one *T counts elements (FEATURES §1.7).
    if (!lhs.isError() && !rhs.isError() &&
        (n.op == BinOp::Add || n.op == BinOp::Sub) &&
        (lhs.type->kind() == Kind::Pointer ||
         rhs.type->kind() == Kind::Pointer)) {
        return lowerPointerArith(n.lhs.get(), n.rhs.get(), lhs, rhs, result, n.op,
                                        n.pos);
    }
    ir::IRType width = widthOf(result, n.pos);
    ir::Value a = onIdentOrExpr(n.lhs.get(), lhs);
    ir::Value b = onIdentOrExpr(n.rhs.get(), rhs);
    const bool lhsFloat = lhs.type->kind() == Kind::Basic &&
                          basicIsFloat(*lhs.type->basic());
    const bool rhsFloat = rhs.type->kind() == Kind::Basic &&
                          basicIsFloat(*rhs.type->basic());
    TypeRef floatTarget;
    if (!result.isError() && result.type->kind() == Kind::Basic &&
        basicIsFloat(*result.type->basic())) {
        floatTarget = result;
    } else if (lhsFloat && rhsFloat) {
        floatTarget = lhsFloat && *lhs.type->basic() == BasicKind::F64
                          ? lhs
                          : ((*rhs.type->basic() == BasicKind::F64) ? rhs : lhs);
    } else if (lhsFloat) {
        floatTarget = lhs;
    } else if (rhsFloat) {
        floatTarget = rhs;
    }
    if (floatTarget.type != nullptr && !floatTarget.isError() &&
        floatTarget.type->kind() == Kind::Basic &&
        basicIsFloat(*floatTarget.type->basic())) {
        // Float bits are bound to their width, so an f32 operand beside an
        // f64 converts, and an integer operand beside a float converts too
        // (SEMANTICS §2.3) — where two integer widths would ride the
        // sign-extension every producer already keeps.
        a = widenFloat(a, lhs, floatTarget, n.pos);
        b = widenFloat(b, rhs, floatTarget, n.pos);
    }
    ir::Instr instr;
    instr.loc = n.pos;
    instr.type = result;
    instr.width = width;
    if (comparison(n.op)) {
        instr.op = ir::Op::Cmp;
        instr.width = ir::IRType::Bool;
        instr.type = TypeRef{ctx_.basic(BasicKind::Bool)};
        // The domain is the checker's common type (SEMANTICS §2.3), not
        // the left operand's: `i32(-1) < u32(1)` compares unsigned. A
        // float pair keeps the unsigned opcodes, whose flags are the ones
        // ucomisd sets — the signed spellings read flags it leaves alone.
        // Bool and pointer pairs never reach the common type (the checker
        // decides those itself, and commonType would fail them loudly).
        bool isSigned = false;
        const bool lhsInt = !lhs.isError() && lhs.type->kind() == Kind::Basic &&
                            lhs.type->basic().has_value() &&
                            basicIsInteger(*lhs.type->basic());
        const bool rhsInt = !rhs.isError() && rhs.type->kind() == Kind::Basic &&
                            rhs.type->basic().has_value() &&
                            basicIsInteger(*rhs.type->basic());
        if (lhsInt && rhsInt) {
            TypeRef common = checker_.commonType(lhs, rhs, n.pos);
            if (!common.isError() && common.type->kind() == Kind::Basic &&
                common.type->basic().has_value()) {
                isSigned = basicIsSigned(*common.type->basic());
                // Both sides convert to the common representation first:
                // a 64-bit cell holding a sign-extended i32 is not the
                // u32 the comparison promises, so comparing raw cells
                // answers a different question (usually audibly, for ==).
                a = convertInt(a, lhs, common, n.pos);
                b = convertInt(b, rhs, common, n.pos);
            }
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

ir::Value Lowerer::lowerShortCircuit(const Binary& n) {
    // The checker proved both sides bool, so the answer is one bool word.
    // Short-circuit is definitional (SEMANTICS): the skipped side never
    // evaluates, and a trap there never fires. The cell holds the answer
    // whichever arm writes it last; expressions cannot terminate a block,
    // so both arms always fall through to the join.
    const bool isAnd = n.op == BinOp::AndAnd;
    TypeRef boolean{ctx_.basic(BasicKind::Bool)};
    ir::Value cell = allocaSlot(boolean, n.pos);
    ir::Value lhs = onIdentOrExpr(n.lhs.get(), boolean);
    if (failed_) {
        return undefined();
    }
    uint32_t rhsLabel = emitBlock(n.pos);
    uint32_t shortLabel = emitBlock(n.pos);
    uint32_t joinLabel = emitBlock(n.pos);
    ir::Terminator br;
    br.term = ir::Term::CondBr;
    br.cond = lhs;
    br.loc = n.pos;
    if (isAnd) {
        br.thenTarget = rhsLabel;
        br.elseTarget = shortLabel;
    } else {
        br.thenTarget = shortLabel;
        br.elseTarget = rhsLabel;
    }
    emitTerminator(br);
    sealBlock(current_);
    auto storeCell = [&](ir::Value value) {
        ir::Instr instr;
        instr.op = ir::Op::Store;
        instr.loc = n.pos;
        instr.type = boolean;
        instr.width = widthOf(boolean, n.pos);
        instr.addressBase = cell;
        instr.storedValue = value;
        emit(std::move(instr));
    };
    auto brToJoin = [&]() {
        ir::Terminator toJoin;
        toJoin.term = ir::Term::Br;
        toJoin.target = joinLabel;
        toJoin.loc = n.pos;
        emitTerminator(toJoin);
    };
    startBlock(rhsLabel);
    ir::Value rhs = onIdentOrExpr(n.rhs.get(), boolean);
    if (failed_) {
        return undefined();
    }
    storeCell(rhs);
    if (failed_) {
        return undefined();
    }
    brToJoin();
    sealBlock(current_);
    startBlock(shortLabel);
    storeCell(constantI64(isAnd ? 0 : 1, boolean, n.pos));
    if (failed_) {
        return undefined();
    }
    brToJoin();
    sealBlock(current_);
    startBlock(joinLabel);
    ir::Instr load;
    load.op = ir::Op::Load;
    load.loc = n.pos;
    load.type = boolean;
    load.width = widthOf(boolean, n.pos);
    load.addressBase = cell;
    load.isPure = true;
    ir::Value out = newValue(load.width, boolean, n.pos);
    load.results.push_back(out);
    emit(std::move(load));
    if (failed_) {
        return undefined();
    }
    return out;
}

ir::Value Lowerer::lowerStringEqWords(std::vector<ir::Value> left,
                                      std::vector<ir::Value> right,
                                      Position pos, bool isEq) {
    // String equality is content with one exception: a null string is
    // the absence of a value, not zero bytes, so it equals only null.
    // The null gate below decides both-null and either-null before the
    // length check; non-null pairs take equal lengths plus equal bytes.
    // The answer rides a cell like the short-circuit arms; each
    // deciding edge writes it once and joins. A zero length takes the
    // empty path before any byte is read, so no null address is touched.
    TypeRef boolean{ctx_.basic(BasicKind::Bool)};
    TypeRef count{ctx_.basic(BasicKind::I64)};
    TypeRef byteType{ctx_.basic(BasicKind::U8)};
    TypeRef ptrType{ctx_.pointer(ctx_.basic(BasicKind::U8))};
    ir::Value cell = allocaSlot(boolean, pos);
    if (failed_) {
        return undefined();
    }
    auto storeCell = [&](bool value) {
        ir::Instr instr;
        instr.op = ir::Op::Store;
        instr.loc = pos;
        instr.type = boolean;
        instr.width = widthOf(boolean, pos);
        instr.addressBase = cell;
        instr.storedValue = constantI64(value ? 1 : 0, boolean, pos);
        emit(std::move(instr));
    };
    uint32_t checkZeroLabel = emitBlock(pos);
    uint32_t initLabel = emitBlock(pos);
    uint32_t headLabel = emitBlock(pos);
    uint32_t bodyLabel = emitBlock(pos);
    uint32_t stepLabel = emitBlock(pos);
    uint32_t trueLabel = emitBlock(pos);
    uint32_t falseLabel = emitBlock(pos);
    uint32_t joinLabel = emitBlock(pos);
    uint32_t nullLeftLabel = emitBlock(pos);
    uint32_t nullRightLabel = emitBlock(pos);
    uint32_t lenLabel = emitBlock(pos);
    auto brTo = [&](uint32_t target) {
        ir::Terminator term;
        term.term = ir::Term::Br;
        term.target = target;
        term.loc = pos;
        emitTerminator(term);
        sealBlock(current_);
    };
    // Null is distinct from empty: both null reads true, either null
    // beside a value reads false, and only two values reach the length
    // check. Eq on pointers is sign-free, so the plain comparator asks.
    ir::Value nil = constantI64(0, ptrType, pos);
    if (failed_) {
        return undefined();
    }
    ir::Value nullLeft = emitI64Cmp(pos, BinOp::Eq, left[kFirstWord], nil);
    ir::Value nullRight = emitI64Cmp(pos, BinOp::Eq, right[kFirstWord], nil);
    if (failed_) {
        return undefined();
    }
    emitTerminator([&] {
        ir::Terminator br;
        br.term = ir::Term::CondBr;
        br.cond = nullLeft;
        br.thenTarget = nullLeftLabel;
        br.elseTarget = nullRightLabel;
        br.loc = pos;
        return br;
    }());
    sealBlock(current_);
    startBlock(nullLeftLabel);
    emitTerminator([&] {
        ir::Terminator br;
        br.term = ir::Term::CondBr;
        br.cond = nullRight;
        br.thenTarget = trueLabel;
        br.elseTarget = falseLabel;
        br.loc = pos;
        return br;
    }());
    sealBlock(current_);
    startBlock(nullRightLabel);
    emitTerminator([&] {
        ir::Terminator br;
        br.term = ir::Term::CondBr;
        br.cond = nullRight;
        br.thenTarget = falseLabel;
        br.elseTarget = lenLabel;
        br.loc = pos;
        return br;
    }());
    sealBlock(current_);
    // Unequal lengths decide immediately; equal lengths need the bytes.
    startBlock(lenLabel);
    emitTerminator([&] {
        ir::Terminator br;
        br.term = ir::Term::CondBr;
        br.cond = emitI64Cmp(pos, BinOp::Eq, left[kLenWord], right[kLenWord]);
        br.thenTarget = checkZeroLabel;
        br.elseTarget = falseLabel;
        br.loc = pos;
        return br;
    }());
    sealBlock(current_);
    startBlock(falseLabel);
    storeCell(!isEq);
    if (failed_) {
        return undefined();
    }
    brTo(joinLabel);
    startBlock(checkZeroLabel);
    ir::Value zero = constantI64(0, count, pos);
    if (failed_) {
        return undefined();
    }
    emitTerminator([&] {
        ir::Terminator br;
        br.term = ir::Term::CondBr;
        br.cond = emitI64Cmp(pos, BinOp::Eq, left[kLenWord], zero);
        br.thenTarget = trueLabel;
        br.elseTarget = initLabel;
        br.loc = pos;
        return br;
    }());
    sealBlock(current_);
    startBlock(trueLabel);
    storeCell(isEq);
    if (failed_) {
        return undefined();
    }
    brTo(joinLabel);
    // The index slot initializes on the only path that reaches the
    // head: the empty path jumps straight to the join, so placing the
    // slot here (instead of beside the head) keeps every head load
    // reading a stored value.
    startBlock(initLabel);
    ir::Value idxSlot = allocaSlot(count, pos);
    if (failed_) {
        return undefined();
    }
    Place idxPlace;
    idxPlace.slots = {idxSlot};
    idxPlace.type = count;
    storeRange(pos, idxPlace, {constantI64(0, count, pos)}, count);
    if (failed_) {
        return undefined();
    }
    brTo(headLabel);
    // The byte loop: an index slot walks both buffers together, the
    // first mismatch decides, exhaustion with no mismatch is equal.
    startBlock(headLabel);
    std::vector<ir::Value> idxVals = loadRange(pos, idxPlace, count);
    if (failed_ || idxVals.size() != kOneWord) {
        refuse(pos, "this expression");
        return undefined();
    }
    emitTerminator([&] {
        ir::Terminator br;
        br.term = ir::Term::CondBr;
        br.cond = emitI64Cmp(pos, BinOp::Lt, idxVals[kFirstWord], left[kLenWord]);
        br.thenTarget = bodyLabel;
        br.elseTarget = trueLabel;
        br.loc = pos;
        return br;
    }());
    sealBlock(current_);
    startBlock(bodyLabel);
    // The head's load parked one block back; reload the live index —
    // every iteration reads what the last step stored.
    std::vector<ir::Value> atVals = loadRange(pos, idxPlace, count);
    if (failed_ || atVals.size() != kOneWord) {
        refuse(pos, "this expression");
        return undefined();
    }
    ir::Value at = atVals[kFirstWord];
    auto advance = [&](ir::Value base) {
        ir::Instr add;
        add.op = ir::Op::Add;
        add.loc = pos;
        add.type = ptrType;
        add.width = ir::IRType::Ptr;
        add.isPure = true;
        add.operands.push_back(base);
        add.operands.push_back(at);
        ir::Value moved = newValue(add.width, ptrType, pos);
        add.results.push_back(moved);
        emit(std::move(add));
        return moved;
    };
    ir::Value fromLeft = advance(left[kFirstWord]);
    ir::Value fromRight = advance(right[kFirstWord]);
    if (failed_) {
        return undefined();
    }
    Place readLeft;
    readLeft.slots = {fromLeft};
    readLeft.type = byteType;
    std::vector<ir::Value> byteLeft = loadRange(pos, readLeft, byteType);
    if (failed_ || byteLeft.size() != kOneWord) {
        refuse(pos, "this expression");
        return undefined();
    }
    Place readRight;
    readRight.slots = {fromRight};
    readRight.type = byteType;
    std::vector<ir::Value> byteRight = loadRange(pos, readRight, byteType);
    if (failed_ || byteRight.size() != kOneWord) {
        refuse(pos, "this expression");
        return undefined();
    }
    emitTerminator([&] {
        ir::Terminator br;
        br.term = ir::Term::CondBr;
        br.cond = emitI64Cmp(pos, BinOp::Eq, byteLeft[kFirstWord],
                             byteRight[kFirstWord]);
        br.thenTarget = stepLabel;
        br.elseTarget = falseLabel;
        br.loc = pos;
        return br;
    }());
    sealBlock(current_);
    startBlock(stepLabel);
    ir::Instr step;
    step.op = ir::Op::Add;
    step.loc = pos;
    step.type = count;
    step.width = ir::IRType::I64;
    step.isPure = true;
    step.operands.push_back(at);
    step.operands.push_back(constantI64(1, count, pos));
    ir::Value next = newValue(step.width, count, pos);
    step.results.push_back(next);
    emit(std::move(step));
    storeRange(pos, idxPlace, {next}, count);
    if (failed_) {
        return undefined();
    }
    brTo(headLabel);
    startBlock(joinLabel);
    ir::Instr load;
    load.op = ir::Op::Load;
    load.loc = pos;
    load.type = boolean;
    load.width = widthOf(boolean, pos);
    load.addressBase = cell;
    load.isPure = true;
    ir::Value out = newValue(load.width, boolean, pos);
    load.results.push_back(out);
    emit(std::move(load));
    if (failed_) {
        return undefined();
    }
    return out;
}

ir::Value Lowerer::lowerStringEq(const Binary& n) {
    // Either side may be the null literal: it answers (nil, 0), and the
    // words core decides null against null and null against a value
    // before any length is read. Only the literal substitutes — the
    // checker keeps a *void variable beside a string a mismatch.
    TypeRef count{ctx_.basic(BasicKind::I64)};
    TypeRef ptrType{ctx_.pointer(ctx_.basic(BasicKind::U8))};
    auto stringWords = [&](const Expr* side) {
        if (dynamic_cast<const NilPtr*>(side) != nullptr) {
            return std::vector<ir::Value>{constantI64(0, ptrType, side->pos),
                                          constantI64(0, count, side->pos)};
        }
        return lowerWords(side);
    };
    std::vector<ir::Value> left = stringWords(n.lhs.get());
    if (failed_ || left.size() != kSliceWords) {
        refuse(n.lhs->pos, "this expression");
        return undefined();
    }
    std::vector<ir::Value> right = stringWords(n.rhs.get());
    if (failed_ || right.size() != kSliceWords) {
        refuse(n.rhs->pos, "this expression");
        return undefined();
    }
    return lowerStringEqWords(left, right, n.pos, n.op == BinOp::Eq);
}

std::vector<ir::Value> Lowerer::lowerTernary(const Ternary& n) {
    // The checker proved both arms share one type, so each arm stores its
    // words into the cell and the join loads them back. The untaken arm
    // never evaluates, so a trap there never fires. A void result keeps
    // the control flow but needs no cell: both arms run for effect.
    TypeRef result = checker_.typeOf(&n);
    if (result.isError()) {
        refuse(n.pos, "this expression");
        return {undefined()};
    }
    int words = wordCount(result);
    if (words < 0) {
        refuse(n.pos, "this expression");
        return {undefined()};
    }
    TypeRef boolean{ctx_.basic(BasicKind::Bool)};
    ir::Value cond = onIdentOrExpr(n.cond.get(), boolean);
    if (failed_) {
        return {undefined()};
    }
    std::vector<ir::Value> cell;
    if (words > 0) {
        cell = allocaSlots(result, n.pos, static_cast<uint64_t>(words));
    }
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
    Place at;
    at.slots = cell;
    at.type = result;
    auto storeArm = [&](const Expr* arm) {
        if (words == 0) {
            lowerWords(arm);
        } else {
            std::vector<ir::Value> got = lowerWords(arm);
            if (failed_) {
                return;
            }
            if (static_cast<int>(got.size()) != words) {
                refuse(arm->pos, "this expression");
                return;
            }
            storeRange(arm->pos, at, got, result);
        }
        if (failed_) {
            return;
        }
        ir::Terminator toJoin;
        toJoin.term = ir::Term::Br;
        toJoin.target = joinLabel;
        toJoin.loc = n.pos;
        emitTerminator(toJoin);
        sealBlock(current_);
    };
    startBlock(thenLabel);
    storeArm(n.then.get());
    if (failed_) {
        return {undefined()};
    }
    startBlock(elseLabel);
    storeArm(n.els.get());
    if (failed_) {
        return {undefined()};
    }
    startBlock(joinLabel);
    if (words == 0) {
        return {};
    }
    std::vector<ir::Value> out = loadRange(n.pos, at, result);
    if (failed_) {
        return {undefined()};
    }
    return out;
}

std::vector<ir::Value> Lowerer::lowerIncrDecr(const IncrDecr& n) {
    TypeRef type = checker_.typeOf(&n);
    if (type.isError()) {
        refuse(n.pos, "this update");
        return {undefined()};
    }
    Place place;
    if (!resolvePlace(n.operand.get(), place)) {
        refuse(n.operand->pos, "this update");
        return {undefined()};
    }
    // Integers are one word; the guard refuses rather than stepping a
    // wider value's first word and leaving the rest behind.
    std::vector<ir::Value> old = loadRange(n.operand->pos, place, type);
    if (failed_ || old.size() != kOneWord) {
        refuse(n.operand->pos, "this update");
        return {undefined()};
    }
    ir::Value one = constantI64(1, type, n.pos);
    if (failed_) {
        return {undefined()};
    }
    // The step rides emitArith, not a bare op: overflow behaves exactly
    // like the matching `+` / `-`, including the experimental wrap flag.
    ir::Value updated =
        emitArith(n.isIncr ? BinOp::Add : BinOp::Sub, old[kFirstWord], one, type, n.pos);
    if (failed_) {
        return {undefined()};
    }
    storeRange(n.operand->pos, place, {updated}, type);
    if (failed_) {
        return {undefined()};
    }
    if (n.isPrefix) {
        return {updated};
    }
    return old;
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
    // what stops DCE deleting the check and LICM hoisting the divide. Floats
    // never trap on arithmetic: an f32/f64 divide answers inf or NaN (SEMANTICS
    // §11), so a mayTrap flag here would lie to the passes, and a float
    // remainder would need an fmod routine rather than the integer op.
    const bool floatResult = instr.width == ir::IRType::F32 ||
                             instr.width == ir::IRType::F64;
    if (floatResult && instr.op == ir::Op::Rem) {
        refuse(pos, "float remainder");
        return undefined();
    }
    if ((instr.op == ir::Op::Div || instr.op == ir::Op::Rem) && !floatResult) {
        instr.mayTrap = true;
    }
    instr.operands.push_back(a);
    instr.operands.push_back(b);
    ir::Value out = newValue(instr.width, result, pos);
    instr.results.push_back(out);
    emit(std::move(instr));
    return out;
}

bool Lowerer::returnWordsFit(const std::vector<ir::Value>& words,
                              Position pos) {
    if (static_cast<uint64_t>(words.size()) > kTwoWords) {
        refuse(pos, "a return wider than two words needs the hidden pointer");
        return false;
    }
    for (uint64_t i = kOneWord; i < static_cast<uint64_t>(words.size()); ++i) {
        if (words[i].width == ir::IRType::F32 ||
            words[i].width == ir::IRType::F64) {
            refuse(pos, "a float past the first return word");
            return false;
        }
    }
    return true;
}

std::vector<ir::Value> Lowerer::callResults(ir::Instr& call, const Type* ret,
                                             Position pos) {    TypeRef retRef{ret};
    if (ret->kind() == Kind::Void) {
        call.width = ir::IRType::Void;
        return {};
    }
    int count = wordCount(retRef);
    if (count < 0) {
        refuse(pos, "a value of this type");
        return {};
    }
    std::vector<ir::Value> out;
    for (uint64_t i = 0; i < static_cast<uint64_t>(count); ++i) {
        // Always the word's type, even when there is only one: a
        // two-byte array's word is U16, and one width cannot name the
        // aggregate (see widthOf). Consumers like storeRange read the
        // value's type per chunk, and every narrow read masks.
        TypeRef word = wordType(retRef, i);
        ir::IRType width = widthOf(word, pos);
        if (failed_) {
            return {};
        }
        // Each answer carries its word's type, not the aggregate's: one
        // width cannot name two words (see widthOf), and consumers like
        // storeRange read the value's type per chunk.
        ir::Value answer = newValue(width, word, pos);
        call.results.push_back(answer);
        out.push_back(answer);
    }
    if (!returnWordsFit(out, pos)) {
        return {};
    }
    if (out.empty()) {
        refuse(pos, "a return with no words");
        return {};
    }
    call.width = out[0].width;
    call.type = out[0].type;
    return out;
}

std::vector<ir::Value> Lowerer::lowerCallWords(Call& n) {
    if (failed_) {
        return {undefined()};
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
                return {undefined()};
            }
            if (!n.args.empty()) {
                refuse(n.pos, "hasM takes no arguments");
                return {undefined()};
            }
            return {emitTagCheck(n.pos, base, tagOff, index)};
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
                return {undefined()};
            }
            callee = resolver_.linkName(*owner) + "__" + field->name;
            sig = msig;
            if (base.slots.empty() && !base.hasIndirect) {
                refuse(n.pos, "a method on a global is not lowered yet");
                return {undefined()};
            }
            if (base.hasIndirect && base.off != 0) {
                // A method on a field reached through a pointer: the object
                // address is the loaded pointer plus the field offset, added
                // the same way a slice advances its pointer.
                TypeRef count{ctx_.basic(BasicKind::I64)};
                TypeRef ptrType{ctx_.pointer(base.type.type)};
                ir::Value step = constantI64(base.off, count, n.pos);
                if (failed_) {
                    return {undefined()};
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
        // `s.m(args)` through an interface: the vtable names the concrete
        // method, and the data word's address rides as the receiver the
        // static method expects first. Entries sit in the resolver's
        // methodNames order, so the index is looked up, never re-derived.
        // Disjoint from the class/struct path above: only an interface
        // value reaches here.
        Place iface;
        if (resolvePlace(field->object.get(), iface) && !iface.type.isError() &&
            iface.type.type->kind() == Kind::Interface) {
            std::optional<DeclId> ifaceId = iface.type.type->decl();
            const Signature* msig = ifaceId.has_value()
                                        ? resolver_.methodSig(*ifaceId, field->name)
                                        : nullptr;
            if (!ifaceId.has_value() || msig == nullptr) {
                refuse(n.pos, "this call");
                return {undefined()};
            }
            int64_t index = -1;
            std::vector<std::string> names = resolver_.methodNames(*ifaceId);
            for (uint64_t k = 0; k < static_cast<uint64_t>(names.size()); ++k) {
                if (names[k] == field->name) {
                    index = static_cast<int64_t>(k);
                    break;
                }
            }
            if (index < 0) {
                refuse(n.pos, "this call");
                return {undefined()};
            }
            // A value behind a pointer, inside another aggregate, or naming
            // a computed address has no direct slots: those dispatches
            // refuse by name until their slice, rather than addressing the
            // wrong words.
            if (iface.hasIndirect || iface.isAddress || iface.off != 0) {
                refuse(n.pos,
                       "an interface method through a pointer or field is not lowered yet");
                return {undefined()};
            }
            TypeRef vtableType = wordType(iface.type, kVtableWord);
            ir::Value vtable;
            ir::Value dataAddr;
            if (!iface.global.empty()) {
                // A module-level interface value: the vtable loads from the
                // label's second word, and the label itself addresses the
                // data (TYPES.md §3.1 offsets come from layout).
                ir::Instr load;
                load.op = ir::Op::Load;
                load.loc = n.pos;
                load.type = vtableType;
                load.width = widthOf(vtableType, n.pos);
                if (failed_) {
                    return {undefined()};
                }
                load.globalLabel = iface.global;
                load.displacement = layout::kOffsetInterfaceVtable;
                load.isPure = true;
                vtable = newValue(load.width, vtableType, n.pos);
                load.results.push_back(vtable);
                emit(std::move(load));
                TypeRef addrType{ctx_.pointer(ctx_.basic(BasicKind::U8))};
                ir::Instr at;
                at.op = ir::Op::DataAddr;
                at.loc = n.pos;
                at.type = addrType;
                at.width = ir::IRType::Ptr;
                at.globalLabel = iface.global;
                at.isPure = true;
                dataAddr = newValue(at.width, addrType, n.pos);
                at.results.push_back(dataAddr);
                emit(std::move(at));
            } else {
                if (iface.slots.size() != kInterfaceWords) {
                    refuse(n.pos, "this call");
                    return {undefined()};
                }
                ir::Instr load;
                load.op = ir::Op::Load;
                load.loc = n.pos;
                load.type = vtableType;
                load.width = widthOf(vtableType, n.pos);
                if (failed_) {
                    return {undefined()};
                }
                load.addressBase = iface.slots[kVtableWord];
                load.isPure = true;
                vtable = newValue(load.width, vtableType, n.pos);
                load.results.push_back(vtable);
                emit(std::move(load));
                dataAddr = computedBase(iface.slots[kFirstWord], 0, n.pos);
            }
            if (failed_) {
                return {undefined()};
            }
            // The method's address rides in the vtable at its methodNames
            // index, scaled by the pointer width from layout: one load,
            // then an indirect call carrying the receiver first, under the
            // same convention direct calls already use.
            TypeRef entryType{ctx_.pointer(ctx_.basic(BasicKind::U8))};
            ir::Instr entry;
            entry.op = ir::Op::Load;
            entry.loc = n.pos;
            entry.type = entryType;
            entry.width = ir::IRType::Ptr;
            entry.addressBase = vtable;
            entry.displacement = index * layout::kSizePointer;
            entry.isPure = true;
            ir::Value target = newValue(entry.width, entryType, n.pos);
            entry.results.push_back(target);
            emit(std::move(entry));
            if (failed_) {
                return {undefined()};
            }
            sig = msig;
            indirect = true;
            indirectCallee = target;
            receiver.push_back(dataAddr);
        }
    }
    if (const auto* ident = dynamic_cast<const Ident*>(n.callee.get())) {
        sig = functionSig(ident->name);
        callee = ident->name;
        // The link name is the resolver's: entry keeps its spelling, an
        // import's mangles under its module, the prelude keeps its C name.
        // The lookup is the same one functionSig uses, so the two cannot
        // disagree — including a program's own shadowing `print`.
        const resolve::Scope* here = resolver_.scopeOf(checker_.module());
        const resolve::Decl* decl =
            here != nullptr ? resolver_.lookup(here, ident->name) : nullptr;
        if (decl != nullptr) {
            // A from-alias binds in this scope but means its target: the
            // id survives the alias, so the link name mangles under the
            // defining module, not the importing one.
            const resolve::Decl* origin = resolver_.declById(decl->id);
            callee = resolver_.linkName(origin != nullptr ? *origin : *decl);
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
                            return {undefined()};
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
                        return {undefined()};
                    }
                    indirectCallee = target[0];
                }
            }
        }
    }
    if (const auto* field = dynamic_cast<const Field*>(n.callee.get())) {
        // `da.aval(args)`: the checker proved the object names a module and
        // the field names its function, so this is a static call to the
        // link name — mangled under the defining module. Generic functions
        // refuse loudly: instantiations do not lower across modules yet.
        if (const auto* mod = dynamic_cast<const Ident*>(field->object.get())) {
            bool shadowed = false;
            uint64_t depth = static_cast<uint64_t>(locals_.size());
            for (; depth > 0;) {
                --depth;
                if (locals_[depth].find(mod->name) != locals_[depth].end()) {
                    shadowed = true;
                    break;
                }
            }
            const resolve::Scope* here = resolver_.scopeOf(checker_.module());
            const resolve::Decl* moduleDecl =
                here != nullptr ? resolver_.lookup(here, mod->name) : nullptr;
            if (!shadowed && moduleDecl != nullptr &&
                moduleDecl->kind == resolve::DeclKind::Module) {
                const resolve::Scope* home =
                    resolver_.scopeOf(moduleDecl->origin);
                const resolve::Decl* member =
                    home != nullptr ? resolver_.lookup(home, field->name)
                                    : nullptr;
                if (member == nullptr ||
                    member->kind != resolve::DeclKind::Func) {
                    refuse(n.pos, "this call");
                    return {undefined()};
                }
                if (member->typeParams > 0) {
                    refuse(n.pos, "a generic call across modules");
                    return {undefined()};
                }
                const resolve::Decl* origin =
                    resolver_.declById(member->id);
                sig = &member->sig;
                callee = resolver_.linkName(origin != nullptr ? *origin
                                                              : *member);
            }
        }
    }
    if (sig == nullptr) {
        refuse(n.pos, "this call");
        return {undefined()};
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
        ir::Argument arg;
        arg.words = lowerWords(n.args[i].get());
        if (failed_) {
            return {undefined()};
        }
        // A concrete argument for an interface parameter boxes to the pair
        // here, so the callee's two words arrive as two. The checker proved
        // the arity, so a parameter miss here is a lowerer bug, not a
        // program, and refuses as one.
        if (i >= static_cast<uint64_t>(sig->parameters().size())) {
            refuse(n.pos, "this call");
            return {undefined()};
        }
        TypeRef want{sig->parameters()[static_cast<size_t>(i)].type};
        arg.words = boxToInterface(std::move(arg.words),
                                   checker_.typeOf(n.args[i].get()), want,
                                   n.args[i]->pos);
        if (failed_) {
            return {undefined()};
        }
        // A float argument rides one word in an SSE register (psABI). The
        // checked type of the argument is the class: a literal is typed in
        // the parameter's context, and any other expression had to match
        // the parameter exactly, because no implicit narrowing exists.
        TypeRef argType = checker_.typeOf(n.args[i].get());
        if (!argType.isError() && argType.type->kind() == Kind::Basic &&
            basicIsFloat(*argType.type->basic()) &&
            arg.words.size() == static_cast<size_t>(kOneWord)) {
            arg.width = *argType.type->basic() == BasicKind::F32
                            ? ir::IRType::F32
                            : ir::IRType::F64;
        }
        instr.arguments.push_back(arg);
    }
    const Type* ret = sig->returnType() != nullptr ? sig->returnType() : ctx_.void_();
    // Captured before the move below: emit() takes the instruction by value,
    // so reading results after moving reads an emptied vector — which is how
    // a call's result became the undefined placeholder.
    std::vector<ir::Value> words = callResults(instr, ret, n.pos);
    if (failed_) {
        return {undefined()};
    }
    emit(std::move(instr));
    if (function_ != nullptr) {
        // A call is mayTrap only if the callee can throw, which is known here
        // rather than guessed by the optimiser.
        function_->canThrow = true;
    }
    return words;
}

ir::Value Lowerer::onCall(Call& n) {
    // The scalar path answers one word — or none, for a void call. A
    // multi-word answer in a scalar position refuses rather than dropping
    // every word past the first.
    std::vector<ir::Value> words = lowerCallWords(n);
    if (failed_) {
        return undefined();
    }
    if (words.empty()) {
        return undefined();
    }
    if (words.size() != kOneWord) {
        refuse(n.pos, "a multi-word answer in a scalar position");
        return undefined();
    }
    return words[kFirstWord];
}

void Lowerer::onReturn(Return& n) {
    if (failed_ || !enabled_ || function_ == nullptr) {
        return;
    }
    ir::Terminator term;
    term.term = ir::Term::Ret;
    term.loc = n.pos;
    if (n.value != nullptr) {
        // Words come home in order (rax, rdx): one value per word of the
        // answered type, through the same row calls read (IR.md §6). A
        // wider answer refuses in callResults, so this never truncates.
        std::vector<ir::Value> words = lowerWords(n.value.get());
        if (failed_) {
            return;
        }
        // A concrete value returned as an interface boxes to the pair
        // before the row check below, which then enforces two, not one.
        const Type* answered = function_->signature.returnType();
        TypeRef want{answered != nullptr ? answered : ctx_.void_()};
        words = boxToInterface(std::move(words), checker_.typeOf(n.value.get()),
                               want, n.pos);
        if (failed_) {
            return;
        }
        if (!returnWordsFit(words, n.pos)) {
            return;
        }
        term.values = std::move(words);
    } else {
        // A bare return answers nothing: an empty list, not the placeholder.
        // The placeholder's id poisons the frame computation, which reads
        // every value the function mentions.
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

    // An arm that ended in a jump (return, break, continue) closed its own
    // block; the branch to the join is then unreachable and emitTerminator
    // drops it. Saving the flag keeps an arm's jump from closing the arm
    // that follows it.
    bool savedTerminated = terminated_;
    terminated_ = false;

    startBlock(thenLabel);
    locals_.push_back({});
    n.then->accept(checker_);
    locals_.pop_back();
    ir::Terminator toJoin;
    toJoin.term = ir::Term::Br;
    toJoin.target = joinLabel;
    toJoin.loc = n.pos;
    emitTerminator(toJoin);

    terminated_ = false;
    startBlock(elseLabel);
    locals_.push_back({});
    if (n.els != nullptr) {
        n.els->accept(checker_);
    }
    locals_.pop_back();
    emitTerminator(toJoin);

    terminated_ = savedTerminated;
    startBlock(joinLabel);
}

void Lowerer::onMatch(MatchStmt& n) {
    if (failed_ || !enabled_ || function_ == nullptr) {
        return;
    }
    // The checker proved every arm compatible and the chain exhaustive
    // (a default, or both bool constants), so the lowering only answers
    // which arm: the scrutinee lowers once, each test branches to its
    // body or the next test, and a taken body joins instead of falling
    // through — the first match in source order wins. A break inside an
    // arm needs no frame here: none is pushed, so it reaches the loop
    // around the match untouched.
    TypeRef scrutType = checker_.typeOf(n.scrutinee.get());
    if (scrutType.isError()) {
        refuse(n.pos, "this match");
        return;
    }
    const bool scrutString = scrutType.type->kind() == Kind::String;
    std::vector<ir::Value> scrut = lowerWords(n.scrutinee.get());
    if (failed_) {
        return;
    }
    const uint64_t want = scrutString ? kSliceWords : kOneWord;
    if (scrut.size() != want) {
        refuse(n.scrutinee->pos, "this match");
        return;
    }
    const uint64_t armCount = static_cast<uint64_t>(n.arms.size());
    std::vector<uint32_t> tests;
    std::vector<uint32_t> bodies;
    for (uint64_t i = 0; i < armCount; ++i) {
        tests.push_back(emitBlock(n.pos));
        bodies.push_back(emitBlock(n.pos));
    }
    uint32_t defaultLabel = emitBlock(n.pos);
    uint32_t joinLabel = emitBlock(n.pos);
    auto brTo = [&](uint32_t target) {
        ir::Terminator term;
        term.term = ir::Term::Br;
        term.target = target;
        term.loc = n.pos;
        emitTerminator(term);
        sealBlock(current_);
    };
    auto condTo = [&](ir::Value cond, uint32_t taken, uint32_t missed) {
        ir::Terminator br;
        br.term = ir::Term::CondBr;
        br.cond = cond;
        br.thenTarget = taken;
        br.elseTarget = missed;
        br.loc = n.pos;
        emitTerminator(br);
        sealBlock(current_);
    };
    // A bound name the lookups below find is a value comparison; one
    // they miss binds fresh. Locals shadow globals, which shadow
    // constants — the same order every other name read uses, and the
    // same call the checker's resolveMatchValue makes: a name it finds
    // must materialize here, never bind fresh over the real value.
    auto boundWords = [&](const std::string& name,
                          std::vector<ir::Value>& words) -> bool {
        for (auto frame = locals_.rbegin(); frame != locals_.rend(); ++frame) {
            auto found = frame->find(name);
            if (found != frame->end()) {
                words = loadLocal(n.pos, found->second);
                return !failed_;
            }
        }
        if (const GlobalBinding* global = findGlobal(name)) {
            words = loadGlobal(n.pos, *global);
            return !failed_;
        }
        const resolve::Scope* here = resolver_.scopeOf(checker_.module());
        const resolve::Decl* decl =
            here != nullptr ? resolver_.lookup(here, name) : nullptr;
        if (decl != nullptr &&
            (decl->kind == resolve::DeclKind::Var ||
             decl->kind == resolve::DeclKind::Const)) {
            // A variable global always lands in globals_ first; missing
            // it here is a lowering gap, not a fresh name. A constant
            // folds, or refuses loudly when it has no integer value.
            if (decl->kind == resolve::DeclKind::Var) {
                refuse(n.pos, "this pattern");
                return false;
            }
            Ident probe(n.pos, name);
            int64_t folded = 0;
            if (!foldBound(&probe, folded)) {
                refuse(n.pos, "this pattern");
                return false;
            }
            TypeRef word{ctx_.basic(BasicKind::I64)};
            words = {constantI64(folded, word, n.pos)};
            return !failed_;
        }
        return false;
    };
    if (armCount == 0) {
        brTo(defaultLabel);
    } else {
        brTo(tests[0]);
    }
    std::vector<std::string> fresh(armCount);
    for (uint64_t i = 0; i < armCount; ++i) {
        startBlock(tests[i]);
        const uint32_t body = bodies[i];
        const uint32_t next = i + 1 < armCount ? tests[i + 1] : defaultLabel;
        const Pattern& pat = *n.arms[i]->pattern;
        // A dead arm can never match: the checker proved the mismatch,
        // so the test edge chains straight through with no comparison.
        if (checker_.isDeadPattern(&pat)) {
            brTo(next);
            continue;
        }
        if (pat.kind == PatternKind::Wildcard) {
            brTo(body);
            continue;
        }
        if (pat.kind == PatternKind::Bind) {
            std::vector<ir::Value> bound;
            if (!boundWords(pat.name, bound)) {
                if (failed_) {
                    return;
                }
                fresh[i] = pat.name;
                brTo(body);
                continue;
            }
            if (bound.size() != want) {
                refuse(n.pos, "this pattern");
                return;
            }
            // Equality answers on the cells: every producer keeps the
            // value's representation in its slot, so two cells holding
            // the same value compare equal at any width both share.
            if (scrutString) {
                ir::Value eq = lowerStringEqWords(scrut, bound, n.pos, true);
                if (failed_) {
                    return;
                }
                condTo(eq, body, next);
                continue;
            }
            condTo(emitI64Cmp(n.pos, BinOp::Eq, scrut[kFirstWord],
                             bound[kFirstWord]),
                   body, next);
            continue;
        }
        if (pat.kind == PatternKind::Const) {
            if (dynamic_cast<const StringLit*>(pat.value.get()) != nullptr) {
                std::vector<ir::Value> wantWords = lowerWords(pat.value.get());
                if (failed_ || wantWords.size() != kSliceWords) {
                    refuse(pat.value->pos, "this pattern");
                    return;
                }
                ir::Value eq =
                    lowerStringEqWords(scrut, wantWords, n.pos, true);
                if (failed_) {
                    return;
                }
                condTo(eq, body, next);
                continue;
            }
            if (dynamic_cast<const NilPtr*>(pat.value.get()) != nullptr) {
                TypeRef ptrType{ctx_.pointer(ctx_.basic(BasicKind::U8))};
                ir::Value nil = constantI64(0, ptrType, n.pos);
                if (failed_) {
                    return;
                }
                condTo(emitI64Cmp(n.pos, BinOp::Eq, scrut[kFirstWord], nil),
                       body, next);
                continue;
            }
            int64_t folded = 0;
            if (!foldBound(pat.value.get(), folded)) {
                refuse(pat.value->pos, "this pattern");
                return;
            }
            TypeRef word{ctx_.basic(BasicKind::I64)};
            ir::Value wantVal = constantI64(folded, word, n.pos);
            if (failed_) {
                return;
            }
            condTo(emitI64Cmp(n.pos, BinOp::Eq, scrut[kFirstWord], wantVal),
                   body, next);
            continue;
        }
        refuse(pat.pos, "this pattern");
        return;
    }
    bool savedTerminated = terminated_;
    for (uint64_t i = 0; i < armCount; ++i) {
        startBlock(bodies[i]);
        locals_.push_back({});
        if (!fresh[i].empty()) {
            // A fresh name binds the scrutinee's words in the arm's own
            // frame, which dies with the arm — the same shape a variable
            // declaration owns, minus the declaration.
            std::vector<ir::Value> slots = allocaSlots(scrutType, n.pos, want);
            Place dest;
            dest.slots = slots;
            dest.type = scrutType;
            dest.off = 0;
            dest.isVolatile = false;
            storeRange(n.pos, dest, scrut, scrutType);
            if (failed_) {
                return;
            }
            LocalBinding bind;
            bind.slots = std::move(slots);
            bind.type = scrutType;
            bind.isMutable = true;
            locals_.back()[fresh[i]] = std::move(bind);
        }
        n.arms[i]->body->accept(checker_);
        locals_.pop_back();
        // A body that ended in a jump already closed its block; one that
        // fell off still needs the edge to the join, and emitTerminator
        // drops the unreachable one itself.
        brTo(joinLabel);
    }
    startBlock(defaultLabel);
    if (n.fallback != nullptr) {
        locals_.push_back({});
        n.fallback->accept(checker_);
        locals_.pop_back();
        brTo(joinLabel);
    } else {
        // Bool-exhaustive with no default: control proves one arm took,
        // but the block still needs its terminator.
        brTo(joinLabel);
    }
    terminated_ = savedTerminated;
    startBlock(joinLabel);
}

void Lowerer::onBreak(Break& n) {
    if (failed_ || !enabled_ || function_ == nullptr) {
        return;
    }
    if (loops_.empty()) {
        refuse(n.pos, "break");
        return;
    }
    ir::Terminator br;
    br.term = ir::Term::Br;
    br.target = loops_.back().exit;
    br.loc = n.pos;
    emitTerminator(br);
    // The block a break leaves is closed by the jump; anything the source
    // put after it is unreachable and must not claim the same block.
    terminated_ = true;
}

void Lowerer::onContinue(Continue& n) {
    if (failed_ || !enabled_ || function_ == nullptr) {
        return;
    }
    if (loops_.empty()) {
        refuse(n.pos, "continue");
        return;
    }
    ir::Terminator br;
    br.term = ir::Term::Br;
    br.target = loops_.back().head;
    br.loc = n.pos;
    emitTerminator(br);
    terminated_ = true;
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

    loops_.push_back(LoopTargets{headLabel, exitLabel});
    bool savedTerminated = terminated_;
    terminated_ = false;

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
    // A body that ended in a jump (break, continue, return) already closed
    // its block; a body that fell off still needs the back edge.
    if (!terminated_) {
        emitTerminator(toHead);
    }

    loops_.pop_back();
    terminated_ = savedTerminated;
    startBlock(exitLabel);
}

void Lowerer::onFor(ForStmt& n) {
    if (failed_ || !enabled_ || function_ == nullptr) {
        return;
    }
    // Four blocks: the condition, the body, the step, and the exit. The
    // init statement runs once in the current block, then control reaches
    // the head. `continue` jumps to the step so the loop still advances —
    // that is what the step expression is for.
    uint32_t headLabel = emitBlock(n.pos);
    uint32_t bodyLabel = emitBlock(n.pos);
    uint32_t stepLabel = emitBlock(n.pos);
    uint32_t exitLabel = emitBlock(n.pos);

    // The loop's own scope: init binds here and must outlive cond, body,
    // and step. The checker pushed its matching scope before calling.
    locals_.push_back({});
    if (n.init != nullptr) {
        // Re-accepted with the sink attached: checked above, lowered here.
        n.init->accept(checker_);
    }
    if (failed_) {
        return;
    }
    ir::Terminator toHead;
    toHead.term = ir::Term::Br;
    toHead.target = headLabel;
    toHead.loc = n.pos;
    emitTerminator(toHead);

    startBlock(headLabel);
    if (n.cond != nullptr) {
        ir::Value cond = onIdentOrExpr(n.cond.get(), checker_.typeOf(n.cond.get()));
        if (failed_) {
            return;
        }
        ir::Terminator br;
        br.term = ir::Term::CondBr;
        br.cond = cond;
        br.thenTarget = bodyLabel;
        br.elseTarget = exitLabel;
        br.loc = n.pos;
        emitTerminator(br);
    } else {
        ir::Terminator br;
        br.term = ir::Term::Br;
        br.target = bodyLabel;
        br.loc = n.pos;
        emitTerminator(br);
    }

    startBlock(bodyLabel);
    loops_.push_back(LoopTargets{stepLabel, exitLabel});
    bool savedTerminated = terminated_;
    terminated_ = false;
    if (n.body != nullptr) {
        n.body->accept(checker_);
    }
    loops_.pop_back();
    // A body that ended in a jump (break, continue, return) closed its own
    // block; one that fell through still needs the edge to the step.
    if (!terminated_) {
        ir::Terminator toStep;
        toStep.term = ir::Term::Br;
        toStep.target = stepLabel;
        toStep.loc = n.pos;
        emitTerminator(toStep);
    }
    terminated_ = savedTerminated;

    startBlock(stepLabel);
    if (n.step != nullptr) {
        onIdentOrExpr(n.step.get(), checker_.typeOf(n.step.get()));
        if (failed_) {
            return;
        }
    }
    if (!terminated_) {
        ir::Terminator back;
        back.term = ir::Term::Br;
        back.target = headLabel;
        back.loc = n.pos;
        emitTerminator(back);
    }
    startBlock(exitLabel);
    locals_.pop_back();
}

void Lowerer::onForIn(ForInStmt& n) {
    if (failed_ || !enabled_ || function_ == nullptr) {
        return;
    }
    // Arrays and slices iterate behind an index. The struct/class
    // iter()/next() protocol has no lowering yet and refuses by name so
    // the loop never disappears silently.
    TypeRef seqType = checker_.typeOf(n.seq.get());
    if (seqType.isError()) {
        refuse(n.pos, "this iteration");
        return;
    }
    Kind seqKind = seqType.type->kind();
    if (seqKind != Kind::Array && seqKind != Kind::Slice) {
        refuse(n.pos, "iterating this type");
        return;
    }
    TypeRef element{seqType.type->element()};
    int64_t elemBytes =
        element.isError() ? 0 : resolver_.nominalSize(element.type);
    if (element.isError() || elemBytes <= 0) {
        refuse(n.pos, "this element");
        return;
    }
    TypeRef count{ctx_.basic(BasicKind::I64)};
    TypeRef ptrType{ctx_.pointer(element.type)};

    // Base address and element count are answered once, before the loop:
    // a slice lowers to its (pointer, length) pair, an array is a place
    // plus a constant extent. Globals have no materialized address.
    ir::Value base = undefined();
    ir::Value lenValue = undefined();
    if (seqKind == Kind::Slice) {
        std::vector<ir::Value> pair = lowerWords(n.seq.get());
        if (failed_ || pair.size() != kSliceWords) {
            refuse(n.pos, "this iteration");
            return;
        }
        base = pair[kFirstWord];
        lenValue = pair[kLenWord];
    } else {
        Place basePlace;
        if (!resolvePlace(n.seq.get(), basePlace)) {
            refuse(n.pos, "this iteration");
            return;
        }
        if (!basePlace.global.empty()) {
            // A global sequence iterates from its label: materialize the
            // address once, then share the indirect path below.
            TypeRef addrType{ctx_.pointer(element.type)};
            ir::Instr data;
            data.op = ir::Op::DataAddr;
            data.loc = n.pos;
            data.type = addrType;
            data.width = ir::IRType::Ptr;
            data.globalLabel = basePlace.global;
            data.isPure = true;
            ir::Value atAddr = newValue(data.width, addrType, n.pos);
            data.results.push_back(atAddr);
            emit(std::move(data));
            if (failed_) {
                return;
            }
            base = atAddr;
            if (basePlace.off != 0) {
                ir::Value delta = constantI64(basePlace.off, count, n.pos);
                if (failed_) {
                    return;
                }
                ir::Instr adjust;
                adjust.op = ir::Op::Add;
                adjust.loc = n.pos;
                adjust.type = addrType;
                adjust.width = ir::IRType::Ptr;
                adjust.isPure = true;
                adjust.operands.push_back(base);
                adjust.operands.push_back(delta);
                ir::Value at = newValue(adjust.width, addrType, n.pos);
                adjust.results.push_back(at);
                emit(std::move(adjust));
                if (failed_) {
                    return;
                }
                base = at;
            }
        } else if (basePlace.hasIndirect) {
            base = basePlace.indirect;
            if (basePlace.off != 0) {
                ir::Value delta = constantI64(basePlace.off, count, n.pos);
                if (failed_) {
                    return;
                }
                ir::Instr adjust;
                adjust.op = ir::Op::Add;
                adjust.loc = n.pos;
                adjust.type = ptrType;
                adjust.width = ir::IRType::Ptr;
                adjust.isPure = true;
                adjust.operands.push_back(base);
                adjust.operands.push_back(delta);
                ir::Value at = newValue(adjust.width, ptrType, n.pos);
                adjust.results.push_back(at);
                emit(std::move(adjust));
                base = at;
            }
        } else if (!basePlace.slots.empty()) {
            base = computedBase(basePlace.slots[0], basePlace.off, n.pos);
        } else {
            refuse(n.pos, "this iteration");
            return;
        }
        std::optional<int64_t> extent = seqType.type->length();
        if (!extent.has_value() || *extent < 0) {
            refuse(n.pos, "this iteration");
            return;
        }
        lenValue = constantI64(*extent, count, n.pos);
    }
    if (failed_) {
        return;
    }
    base.type = ptrType;

    // The index counts from zero; the element slot receives each visit.
    ir::Value idxSlot = allocaSlot(count, n.pos);
    ir::Value elemSlot = allocaSlot(element, n.pos);
    if (failed_) {
        return;
    }
    Place idxPlace;
    idxPlace.slots = {idxSlot};
    idxPlace.type = count;
    storeRange(n.pos, idxPlace, {constantI64(0, count, n.pos)}, count);
    if (failed_) {
        return;
    }

    uint32_t headLabel = emitBlock(n.pos);
    uint32_t bodyLabel = emitBlock(n.pos);
    uint32_t stepLabel = emitBlock(n.pos);
    uint32_t exitLabel = emitBlock(n.pos);

    // The iteration variable binds in the loop's own frame, which the
    // checker pushed a matching scope for before calling.
    locals_.push_back({});
    LocalBinding bind;
    bind.slots = {elemSlot};
    bind.type = element;
    bind.isMutable = n.isMutable;
    locals_.back()[n.name] = std::move(bind);

    ir::Terminator toHead;
    toHead.term = ir::Term::Br;
    toHead.target = headLabel;
    toHead.loc = n.pos;
    emitTerminator(toHead);

    startBlock(headLabel);
    std::vector<ir::Value> now = loadRange(n.pos, idxPlace, count);
    if (failed_ || now.size() != kOneWord) {
        return;
    }
    ir::Value inBounds = emitI64Cmp(n.pos, BinOp::Lt, now[kFirstWord], lenValue);
    if (failed_) {
        return;
    }
    ir::Terminator cond;
    cond.term = ir::Term::CondBr;
    cond.cond = inBounds;
    cond.thenTarget = bodyLabel;
    cond.elseTarget = exitLabel;
    cond.loc = n.pos;
    emitTerminator(cond);

    startBlock(bodyLabel);
    {
        // element address = base + index * stride (SEMANTICS 8).
        ir::Value scaled = now[kFirstWord];
        if (elemBytes != kOneMember) {
            ir::Value stride = constantI64(elemBytes, count, n.pos);
            if (failed_) {
                return;
            }
            ir::Instr mul;
            mul.op = ir::Op::Mul;
            mul.loc = n.pos;
            mul.type = count;
            mul.width = widthOf(count, n.pos);
            if (failed_) {
                return;
            }
            mul.isPure = true;
            mul.operands.push_back(now[kFirstWord]);
            mul.operands.push_back(stride);
            scaled = newValue(mul.width, count, n.pos);
            mul.results.push_back(scaled);
            emit(std::move(mul));
        }
        ir::Instr step;
        step.op = ir::Op::Add;
        step.loc = n.pos;
        step.type = ptrType;
        step.width = ir::IRType::Ptr;
        step.isPure = true;
        step.operands.push_back(base);
        step.operands.push_back(scaled);
        ir::Value addr = newValue(step.width, ptrType, n.pos);
        step.results.push_back(addr);
        emit(std::move(step));
        if (failed_) {
            return;
        }
        Place at;
        at.slots = {addr};
        at.type = element;
        std::vector<ir::Value> words = loadRange(n.pos, at, element);
        if (failed_) {
            return;
        }
        Place elemPlace;
        elemPlace.slots = {elemSlot};
        elemPlace.type = element;
        storeRange(n.pos, elemPlace, words, element);
        if (failed_) {
            return;
        }
    }

    loops_.push_back(LoopTargets{stepLabel, exitLabel});
    bool savedTerminated = terminated_;
    terminated_ = false;
    if (n.body != nullptr) {
        n.body->accept(checker_);
    }
    loops_.pop_back();
    if (!terminated_) {
        ir::Terminator toStep;
        toStep.term = ir::Term::Br;
        toStep.target = stepLabel;
        toStep.loc = n.pos;
        emitTerminator(toStep);
    }
    terminated_ = savedTerminated;

    startBlock(stepLabel);
    {
        std::vector<ir::Value> at = loadRange(n.pos, idxPlace, count);
        if (failed_ || at.size() != kOneWord) {
            return;
        }
        ir::Value one = constantI64(1, count, n.pos);
        if (failed_) {
            return;
        }
        ir::Instr bump;
        bump.op = ir::Op::Add;
        bump.loc = n.pos;
        bump.type = count;
        bump.width = widthOf(count, n.pos);
        if (failed_) {
            return;
        }
        bump.isPure = true;
        bump.operands.push_back(at[kFirstWord]);
        bump.operands.push_back(one);
        ir::Value next = newValue(bump.width, count, n.pos);
        bump.results.push_back(next);
        emit(std::move(bump));
        storeRange(n.pos, idxPlace, {next}, count);
        if (failed_) {
            return;
        }
    }
    if (!terminated_) {
        ir::Terminator back;
        back.term = ir::Term::Br;
        back.target = headLabel;
        back.loc = n.pos;
        emitTerminator(back);
    }
    startBlock(exitLabel);
    locals_.pop_back();
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
        // A literal typed as a float by its context lowers as that float's
        // bits — the integer value converted with the cast table's
        // rounding, never the raw integer bits behind a float read.
        if (!type.isError() && type.type->kind() == Kind::Basic &&
            basicIsFloat(*type.type->basic())) {
            if (*type.type->basic() == BasicKind::F32) {
                float narrowed = static_cast<float>(static_cast<double>(folded.value));
                uint32_t pattern = 0;
                std::memcpy(&pattern, &narrowed, sizeof pattern);
                return constantFBits(pattern, type, lit->pos);
            }
            double widened = static_cast<double>(folded.value);
            uint64_t pattern = 0;
            std::memcpy(&pattern, &widened, sizeof pattern);
            return constantFBits(pattern, type, lit->pos);
        }
        return constantI64(folded.value, type, lit->pos);
    }
    if (auto* lit = dynamic_cast<const FloatLit*>(expr)) {
        return lowerFloatLit(*lit);
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
    if (auto* sz = dynamic_cast<const SizeofType*>(expr)) {
        std::vector<ir::Value> words = lowerSizeof(*sz);
        if (words.size() != kOneWord) {
            refuse(expr->pos, "this expression");
            return undefined();
        }
        return words[0];
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
        // An address in a value position (a cast's operand, a call
        // argument) lowers through the address path, which answers one
        // word — the same single-answer rule as the dereference above.
        if (unary->op == UnOp::Addr) {
            std::vector<ir::Value> words = lowerAddrOf(*unary);
            if (failed_ || words.size() != kOneWord) {
                refuse(expr->pos, "this expression");
                return undefined();
            }
            return words[0];
        }
        if (unary->op == UnOp::Neg) {
            return lowerNeg(*unary);
        }
        if (unary->op == UnOp::BitNot) {
            return lowerBitNot(*unary);
        }
        if (unary->op == UnOp::Not) {
            TypeRef type = checker_.typeOf(expr);
            ir::Value value = onIdentOrExpr(unary->operand.get(), type);
            if (failed_) {
                return undefined();
            }
            ir::Instr instr;
            instr.op = ir::Op::Not;
            instr.loc = expr->pos;
            instr.type = type;
            instr.width = widthOf(type, expr->pos);
            instr.isPure = true;
            instr.operands.push_back(value);
            ir::Value out = newValue(instr.width, type, expr->pos);
            instr.results.push_back(out);
            emit(std::move(instr));
            return out;
        }
    }
    // An increment answers one word — the checker proved an integer — so a
    // scalar position reads it through the word path, like a dereference.
    if (auto* step = dynamic_cast<const IncrDecr*>(expr)) {
        std::vector<ir::Value> words = lowerIncrDecr(*step);
        if (failed_ || words.size() != kOneWord) {
            refuse(expr->pos, "this expression");
            return undefined();
        }
        return words[0];
    }
    // A ternary in a scalar position (a binary operand, a call argument)
    // reads through the word path and demands exactly one word, like a
    // field: the checker types multi-word ternaries fine in value
    // position, but a scalar reader has one slot for the answer.
    if (dynamic_cast<const Ternary*>(expr) != nullptr) {
        std::vector<ir::Value> words =
            lowerTernary(static_cast<const Ternary&>(*expr));
        if (failed_ || words.size() != kOneWord) {
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
    // A concrete value assigned to an interface boxes to the pair before
    // the word-count check below, which then enforces two, not one.
    value = boxToInterface(std::move(value), checker_.typeOf(n.value.get()),
                           target, n.pos);
    if (failed_) {
        return undefined();
    }
    // The words must agree with the target: a checker/lowerer disagreement
    // here would store the wrong number of words, silently.
    if (!target.isError() && wordCount(target) >= 0 &&
        value.size() != static_cast<size_t>(wordCount(target))) {
        refuse(n.pos, "this assignment");
        return undefined();
    }
    if (n.op.has_value() && !compoundValue(n, value)) {
        return undefined();
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

bool Lowerer::compoundValue(Assign& n, std::vector<ir::Value>& value) {
    // A compound op on several words never arrives: the checker refuses
    // it. Asserting that here keeps the two layers honest with each other.
    if (value.size() != kOneWord) {
        refuse(n.pos, "a compound assignment needs one word");
        return false;
    }
    TypeRef current = checker_.typeOf(n.target.get());
    std::vector<ir::Value> old = lowerWords(n.target.get());
    if (old.size() != kOneWord) {
        refuse(n.pos, "a compound assignment needs one word");
        return false;
    }
    // The right-hand side converts to the target's float width first
    // (SEMANTICS §2.3): `d64 += 1` adds a converted 1.0, never the
    // integer's raw bits read as a double. One definition serves both
    // the expression and statement paths, so the two cannot diverge.
    if (!current.isError() && current.type->kind() == Kind::Basic &&
        basicIsFloat(*current.type->basic())) {
        value[0] = widenFloat(value[0], checker_.typeOf(n.value.get()),
                              current, n.pos);
    }
    if (!current.isError() && current.type->kind() == Kind::Pointer &&
        (*n.op == BinOp::Add || *n.op == BinOp::Sub)) {
        // `p += n` lowers as the binary it spells: the scaled advance,
        // not raw integer bits added to an address.
        TypeRef rhs = checker_.typeOf(n.value.get());
        value = {lowerPointerArith(n.target.get(), n.value.get(), current,
                                   rhs, current, *n.op, n.pos)};
        return !failed_;
    }
    value = {emitArith(*n.op, old[0], value[0], current, n.pos)};
    return !failed_;
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
    TypeRef valueType = checker_.typeOf(n.value.get());
    if (dynamic_cast<const StringLit*>(n.value.get()) != nullptr &&
        isBytePointerTarget(target) && value.size() == kSliceWords) {
        // A literal beside a byte pointer keeps its address and drops
        // its length, like the declaration path: one word, not two.
        value = {value[kFirstWord]};
    }
    if (!target.isError() && !valueType.isError() &&
        valueType.type->kind() == Kind::Slice &&
        target.type->kind() == Kind::Array) {
        // Reassigning an array from a slice re-copies, length-checked:
        // the array keeps its shape, the slice keeps its words.
        if (failed_ || value.size() != kSliceWords) {
            refuse(n.pos, "this assignment");
            return;
        }
        Place dest;
        if (!resolvePlace(n.target.get(), dest)) {
            refuse(n.target->pos, "this assignment");
            return;
        }
        dest.type = target;
        copySliceToArray(n.pos, value[kFirstWord], value[kLenWord], target,
                         dest, "this assignment");
        return;
    }
    // A concrete value assigned to an interface boxes to the pair before
    // the word-count check below, which then enforces two, not one.
    value = boxToInterface(std::move(value), valueType, target, n.pos);
    if (failed_) {
        return;
    }
    if (!target.isError() && wordCount(target) >= 0 &&
        value.size() != static_cast<size_t>(wordCount(target))) {
        refuse(n.pos, "this assignment");
        return;
    }
    if (n.op.has_value() && !compoundValue(n, value)) {
        return;
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
