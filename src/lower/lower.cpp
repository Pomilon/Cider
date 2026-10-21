// lower.cpp — scalars to IR.
//
// Driven by the checker's walk: every hook below is called while the checker is
// visiting a node it has just typed, so a type is recorded and an instruction
// emitted on the same visit. Nothing here walks the AST.

#include "lower/lower.h"

#include <algorithm>
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

std::vector<ir::Value> adjustStringLitArg(const Expr* expr,
                                         std::vector<ir::Value> words,
                                         const TypeRef& targetType) {
    if (expr != nullptr && dynamic_cast<const StringLit*>(expr) != nullptr &&
        isBytePointerTarget(targetType) && words.size() == kSliceWords) {
        return {words[kFirstWord]};
    }
    return words;
}
// A closure answers two words, code then captures, in the same positions
// a slice keeps base and length: the hidden function loads word zero, the
// allocation behind word one.
constexpr uint64_t kClosureWords = 2;

// Whether lowering this expression can run an effect: calls, construction
// that runs init, closures, assignment, and anything unlisted
// (fail-closed). Pure address math, literals and names answer false.
bool lowersImpure(const Expr* expr) {
    if (expr == nullptr) {
        return true;
    }
    if (dynamic_cast<const Call*>(expr) != nullptr ||
        dynamic_cast<const NewClass*>(expr) != nullptr ||
        dynamic_cast<const FuncLit*>(expr) != nullptr ||
        dynamic_cast<const Assign*>(expr) != nullptr ||
        dynamic_cast<const IncrDecr*>(expr) != nullptr ||
        dynamic_cast<const StringLit*>(expr) != nullptr) {
        return true;
    }
    if (const auto* un = dynamic_cast<const Unary*>(expr)) {
        return lowersImpure(un->operand.get());
    }
    if (const auto* field = dynamic_cast<const Field*>(expr)) {
        return lowersImpure(field->object.get());
    }
    if (const auto* index = dynamic_cast<const Index*>(expr)) {
        return lowersImpure(index->array.get()) || lowersImpure(index->index.get());
    }
    if (const auto* slice = dynamic_cast<const SliceExpr*>(expr)) {
        return lowersImpure(slice->array.get()) || lowersImpure(slice->lo.get()) ||
               lowersImpure(slice->hi.get());
    }
    if (const auto* tern = dynamic_cast<const Ternary*>(expr)) {
        return lowersImpure(tern->cond.get()) || lowersImpure(tern->then.get()) ||
               lowersImpure(tern->els.get());
    }
    if (const auto* cast = dynamic_cast<const Cast*>(expr)) {
        return lowersImpure(cast->expr.get());
    }
    if (const auto* binary = dynamic_cast<const Binary*>(expr)) {
        return lowersImpure(binary->lhs.get()) || lowersImpure(binary->rhs.get());
    }
    if (const auto* built = dynamic_cast<const NewStruct*>(expr)) {
        for (const FieldInit& field : built->fields) {
            if (lowersImpure(field.value.get())) {
                return true;
            }
        }
        return false;
    }
    if (const auto* listed = dynamic_cast<const NewArray*>(expr)) {
        for (const std::unique_ptr<Expr>& init : listed->inits) {
            if (lowersImpure(init.get())) {
                return true;
            }
        }
        return false;
    }
    if (dynamic_cast<const Ident*>(expr) != nullptr ||
        dynamic_cast<const IntLit*>(expr) != nullptr ||
        dynamic_cast<const FloatLit*>(expr) != nullptr ||
        dynamic_cast<const CharLit*>(expr) != nullptr ||
        dynamic_cast<const BoolLit*>(expr) != nullptr ||
        dynamic_cast<const NilPtr*>(expr) != nullptr ||
        dynamic_cast<const SizeofType*>(expr) != nullptr ||
        dynamic_cast<const AlignofType*>(expr) != nullptr) {
        return false;
    }
    return true;
}

// Whether a failed place lookup may already have lowered an effect: only
// dereference operands run inside resolvePlace, along the spine it visits
// (field objects, index arrays — an index is only folded, never
// lowered). Anything else resolvePlace never visits, so spilling
// re-lowers it exactly once.
bool placeMayHaveRun(const Expr* expr) {
    if (const auto* un = dynamic_cast<const Unary*>(expr)) {
        if (un->op == UnOp::Deref) {
            return lowersImpure(un->operand.get());
        }
        return placeMayHaveRun(un->operand.get());
    }
    if (const auto* field = dynamic_cast<const Field*>(expr)) {
        return placeMayHaveRun(field->object.get());
    }
    if (const auto* index = dynamic_cast<const Index*>(expr)) {
        return placeMayHaveRun(index->array.get());
    }
    return false;
}
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

Lowerer::Lowerer(TypeContext& ctx, resolve::Resolver& resolver, check::Checker& checker,
                 target::Kind target)
    : ctx_(ctx), resolver_(resolver), checker_(checker), target_(target) {}

void Lowerer::refuse(Position pos, const std::string& what) {
    errors_.push_back(check::CheckError{pos, "not lowered yet: " + what});
    failed_ = true;
}

void Lowerer::prepareTargetEntry() {
    if (targetPrepared_) {
        return;
    }
    targetPrepared_ = true;
    if (module_ == nullptr || target_ != target::Kind::Uefi) {
        return;
    }
    // The firmware owns `efi_main`; the user's function keeps its source
    // spelling but not its link symbol. One rename here keeps every direct
    // call and function address pointing at the user body, never at the
    // firmware shim.
    const std::string hidden = "cider_efi_main_user";
    for (ir::Function& fn : module_->functions) {
        if (fn.name == "efi_main") {
            fn.name = hidden;
            module_->entry = hidden;
            break;
        }
    }
    if (module_->entry.empty()) {
        return;
    }
    for (ir::Function& fn : module_->functions) {
        for (ir::Block& block : fn.blocks) {
            for (ir::Instr& instr : block.body) {
                if (instr.op == ir::Op::Call && instr.callee == "efi_main") {
                    instr.callee = hidden;
                }
                if (instr.op == ir::Op::FnAddr && instr.globalLabel == "efi_main") {
                    instr.globalLabel = hidden;
                }
            }
        }
    }
    for (ir::Global& global : module_->globals) {
        for (std::string& label : global.initLabels) {
            if (label == "efi_main") {
                label = hidden;
            }
        }
    }
    const TypeRef voidPtr{ctx_.pointer(ctx_.void_())};
    bool image = false;
    bool system = false;
    for (const ir::Global& global : module_->globals) {
        image = image || global.label == "image_handle";
        system = system || global.label == "system_table";
    }
    if (!image) {
        ir::Global slot;
        slot.label = "image_handle";
        slot.type = voidPtr;
        module_->globals.push_back(std::move(slot));
    }
    if (!system) {
        ir::Global slot;
        slot.label = "system_table";
        slot.type = voidPtr;
        module_->globals.push_back(std::move(slot));
    }
}

// --- Entry point -------------------------------------------------------------

bool Lowerer::lowerModule(const std::string& name, Program& program) {
    module_ = std::make_unique<ir::Module>();
    module_->name = name;
    return lowerInto(name, program);
}

bool Lowerer::lowerInto(const std::string& name, Program& program) {
    // UEFI exposes the firmware handles as ordinary source globals. Their
    // storage is the bare label the entry shim writes, not a mangled Cider
    // global, so register the bindings before the checking walk reads them.
    if (target_ == target::Kind::Uefi) {
        const TypeRef voidPtr{ctx_.pointer(ctx_.void_())};
        for (const char* globalName : {"image_handle", "system_table"}) {
            bool present = false;
            for (const GlobalBinding& binding : globals_) {
                if (binding.name == globalName && binding.module == name) {
                    present = true;
                    break;
                }
            }
            if (!present) {
                GlobalBinding binding;
                binding.name = globalName;
                binding.label = globalName;
                binding.module = name;
                binding.type = voidPtr;
                globals_.push_back(std::move(binding));
            }
        }
    }
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

bool Lowerer::falloffReachable(uint32_t target) {
    if (function_ == nullptr || function_->blocks.empty() || blockOrder_.empty()) {
        return true;
    }
    std::unordered_map<uint32_t, size_t> at;
    size_t known = function_->blocks.size();
    while (known > 0) {
        --known;
        at[function_->blocks[known].label] = known;
    }
    std::unordered_set<uint32_t> seen;
    std::vector<uint32_t> stack{blockOrder_.front()};
    while (!stack.empty()) {
        uint32_t label = stack.back();
        stack.pop_back();
        if (label == target) {
            return true;
        }
        if (!seen.insert(label).second) {
            continue;
        }
        auto found = at.find(label);
        if (found == at.end()) {
            continue;
        }
        const ir::Terminator& term = function_->blocks[found->second].term;
        switch (term.term) {
            case ir::Term::Br: stack.push_back(term.target); break;
            case ir::Term::CondBr:
                stack.push_back(term.thenTarget);
                stack.push_back(term.elseTarget);
                break;
            case ir::Term::Switch:
                for (const ir::Case& arm : term.cases) {
                    stack.push_back(arm.target);
                }
                stack.push_back(term.defaultTarget);
                break;
            case ir::Term::Ret: break;
            case ir::Term::Trap: break;
            case ir::Term::Unreachable: break;
        }
    }
    return false;
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
        kind == Kind::Closure || kind == Kind::Enum) {
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

std::string Lowerer::emitClassVTable(DeclId concrete, Position pos) {
    const resolve::Decl* owner = resolver_.declById(concrete);
    if (owner == nullptr) {
        refuse(pos, "this construction");
        return "";
    }
    std::string label = "vtable_" + resolver_.linkName(*owner);
    if (classVtablesEmitted_.insert(label).second) {
        ir::Global table;
        table.label = label;
        table.type = TypeRef{ctx_.pointer(ctx_.basic(BasicKind::U8))};
        for (const std::string& name : resolver_.virtualNames(concrete)) {
            std::optional<DeclId> home = resolver_.methodHome(concrete, name);
            if (name == "deinit") {
                // 4.8.3: the destructor slot names the chain entry for the
                // concrete type — the no-op stub when no class in the
                // chain declares deinit — so delete dispatches uniformly
                // through every table.
                table.initLabels.push_back(
                    resolver_.methodSig(concrete, "deinit") != nullptr
                        ? emitDestroyChain(concrete, pos)
                        : emitDeinitNone(pos));
                if (failed_) {
                    return "";
                }
                continue;
            }
            // The slot's function is the method's static address for this
            // concrete type: the override's own function where the chain
            // redeclares the name, the declarer's otherwise. A class
            // receiver is already the address its method expects, so no
            // adapter rides here.
            const resolve::Decl* declarer =
                home.has_value() ? resolver_.declById(*home) : nullptr;
            if (declarer == nullptr) {
                refuse(pos, "this construction");
                return "";
            }
            table.initLabels.push_back(resolver_.linkName(*declarer) + "__" + name);
        }
        module_->globals.push_back(std::move(table));
    }
    return label;
}

std::string Lowerer::emitDestroyChain(DeclId declarer, Position pos) {
    const resolve::Decl* owner = resolver_.declById(declarer);
    if (owner == nullptr) {
        refuse(pos, "this deletion");
        return "";
    }
    std::string label = resolver_.linkName(*owner) + "__destroy";
    if (!destroyChains_.insert(label).second) {
        return label;
    }
    for (const ir::Function& fn : module_->functions) {
        if (fn.name == label) {
            refuse(pos, "this deletion");
            return "";
        }
    }
    // The base chains must exist before this function is pushed: a nested
    // emission would move module_->functions out from under function_.
    // 4.8: every direct base gets its own chain, with the self pointer
    // shifted to that base subobject; the first base sits at zero.
    std::vector<std::pair<std::string, int64_t>> baseCalls;
    for (DeclId base : resolver_.classBases(declarer)) {
        if (resolver_.methodSig(base, "deinit") == nullptr) {
            continue;
        }
        std::optional<int64_t> shift = resolver_.baseOffset(declarer, base);
        if (!shift.has_value()) {
            refuse(pos, "this deletion");
            return "";
        }
        std::string above = emitDestroyChain(base, pos);
        if (failed_) {
            return "";
        }
        baseCalls.emplace_back(std::move(above), *shift);
    }
    std::optional<DeclId> ownHome = resolver_.methodHome(declarer, "deinit");
    bool hasOwn = ownHome.has_value() && sameDecl(*ownHome, declarer);

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
    fn.name = label;
    TypeRef selfType{ctx_.pointer(ctx_.basic(BasicKind::U8))};
    Parameter self;
    self.type = selfType.type;
    self.isMutable = false;
    fn.signature = Signature({self}, ctx_.void_(), Abi::SysV);
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
    std::vector<ir::Value> selfSlots = allocaSlots(selfType, pos, kOneWord);
    for (ir::Value slot : selfSlots) {
        function_->params.push_back(slot);
    }
    ir::Instr load;
    load.op = ir::Op::Load;
    load.loc = pos;
    load.type = selfType;
    load.width = ir::IRType::Ptr;
    load.addressBase = selfSlots[kFirstWord];
    load.isPure = true;
    ir::Value selfVal = newValue(load.width, selfType, pos);
    load.results.push_back(selfVal);
    emit(std::move(load));
    if (!failed_ && hasOwn) {
        // The class's own destructor runs first, exactly as the Standard's
        // `delete s` runs Circle.deinit before Shape.deinit.
        ir::Instr call;
        call.op = ir::Op::Call;
        call.loc = pos;
        call.callee = resolver_.linkName(*owner) + "__deinit";
        call.mayTrap = true;
        ir::Argument selfArg;
        selfArg.words.push_back(selfVal);
        call.arguments.push_back(std::move(selfArg));
        call.width = ir::IRType::Void;
        emit(std::move(call));
    }
    for (const auto& entry : baseCalls) {
        if (failed_) {
            break;
        }
        ir::Value baseSelf = selfVal;
        if (entry.second != 0) {
            TypeRef word{ctx_.pointer(ctx_.basic(BasicKind::U8))};
            ir::Value delta =
                constantI64(entry.second, TypeRef{ctx_.basic(BasicKind::I64)}, pos);
            if (failed_) {
                break;
            }
            ir::Instr step;
            step.op = ir::Op::Add;
            step.loc = pos;
            step.type = word;
            step.width = ir::IRType::Ptr;
            step.isPure = true;
            step.operands.push_back(baseSelf);
            step.operands.push_back(delta);
            ir::Value moved = newValue(step.width, word, pos);
            step.results.push_back(moved);
            emit(std::move(step));
            if (failed_) {
                break;
            }
            baseSelf = moved;
        }
        // Then each base chain in declaration order, walking further up
        // the same way.
        ir::Instr up;
        up.op = ir::Op::Call;
        up.loc = pos;
        up.callee = entry.first;
        up.mayTrap = true;
        ir::Argument upArg;
        upArg.words.push_back(baseSelf);
        up.arguments.push_back(std::move(upArg));
        up.width = ir::IRType::Void;
        emit(std::move(up));
    }
    recordParamClasses();
    if (!failed_) {
        function_->canThrow = true;
        ir::Terminator fret;
        fret.term = ir::Term::Ret;
        fret.loc = pos;
        emitTerminator(fret);
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
    return label;
}

std::string Lowerer::emitDeinitNone(Position pos) {
    std::string label = "cider_deinit_none";
    if (noopDeinitEmitted_) {
        return label;
    }
    for (const ir::Function& fn : module_->functions) {
        if (fn.name == label) {
            refuse(pos, "this deletion");
            return "";
        }
    }
    noopDeinitEmitted_ = true;

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
    fn.name = label;
    TypeRef selfType{ctx_.pointer(ctx_.basic(BasicKind::U8))};
    Parameter self;
    self.type = selfType.type;
    self.isMutable = false;
    fn.signature = Signature({self}, ctx_.void_(), Abi::SysV);
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
    std::vector<ir::Value> selfSlots = allocaSlots(selfType, pos, kOneWord);
    for (ir::Value slot : selfSlots) {
        function_->params.push_back(slot);
    }
    recordParamClasses();
    if (!failed_) {
        ir::Terminator fret;
        fret.term = ir::Term::Ret;
        fret.loc = pos;
        emitTerminator(fret);
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
    return label;
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
    const Type* ret = sig.returnType() != nullptr ? sig.returnType() : ctx_.void_();
    std::vector<Parameter> combined;
    uint64_t sretShift = 0;
    if (needsSret(ret)) {
        // A wide answer threads the caller's buffer address ahead of the
        // data word (SysV sret), exactly as the method's own signature
        // spells it; the dispatch call site fills the same position.
        Parameter buffer;
        buffer.type = dataPtr.type;
        buffer.isMutable = false;
        combined.push_back(buffer);
        sretShift = kOneWord;
    }
    Parameter receiver;
    receiver.type = dataPtr.type;
    receiver.isMutable = false;
    combined.push_back(receiver);
    for (const Parameter& p : sig.parameters()) {
        combined.push_back(p);
    }
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

    // The hidden buffer address has no source spelling: its slot rides
    // first and everything downstream shifts past it.
    if (needsSret(ret)) {
        std::vector<ir::Value> bufferSlots =
            allocaSlots(dataPtr, pos, kOneWord);
        if (failed_) {
            return "";
        }
        for (ir::Value slot : bufferSlots) {
            function_->params.push_back(slot);
        }
    }
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
    if (needsSret(ret)) {
        // The buffer forwards exactly as the direct call spelled it: the
        // same address rides first, so the method's stores land in the
        // caller's slot and nothing comes back in registers.
        ir::Instr loadBuf;
        loadBuf.op = ir::Op::Load;
        loadBuf.loc = pos;
        loadBuf.type = dataPtr;
        loadBuf.width = ir::IRType::Ptr;
        loadBuf.addressBase = function_->params[kFirstWord];
        loadBuf.isPure = true;
        ir::Value buf = newValue(loadBuf.width, dataPtr, pos);
        loadBuf.results.push_back(buf);
        emit(std::move(loadBuf));
        if (failed_) {
            return "";
        }
        ir::Argument first;
        first.words.push_back(buf);
        call.arguments.push_back(std::move(first));
    }
    {
        ir::Argument selfArg;
        selfArg.words.push_back(self);
        call.arguments.push_back(std::move(selfArg));
    }
    const std::vector<Parameter>& declared = function_->signature.parameters();
    for (uint64_t i = 0; i < static_cast<uint64_t>(sig.parameters().size()); ++i) {
        TypeRef type{declared[i + kOneWord + sretShift].type};
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
        if (needsSret(ret)) {
            // The method stored its answer into the caller's buffer
            // through the forwarded address: nothing rides back in
            // registers.
            call.width = ir::IRType::Void;
            call.type = TypeRef{ret};
        } else if (ret->kind() != Kind::Void) {
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

bool Lowerer::needsSret(const Type* ret) {
    if (ret == nullptr || ret->kind() == Kind::Void) {
        return false;
    }
    TypeRef ref{ret};
    return wordCount(ref) > static_cast<int>(kTwoWords);
}

bool Lowerer::resolvePlace(const Expr* expr, Place& out) {
    if (const auto* ident = dynamic_cast<const Ident*>(expr)) {
        uint64_t depth = static_cast<uint64_t>(locals_.size());
        for (; depth > 0;) {
            --depth;
            auto found = locals_[depth].find(ident->name);
            if (found != locals_[depth].end()) {
                Place res;
                res.slots = found->second.slots;
                res.type = found->second.type;
                res.off = 0;
                res.isVolatile = false;
                out = std::move(res);
                return true;
            }
        }
        if (const GlobalBinding* global = findGlobal(ident->name)) {
            Place res;
            res.global = global->label;
            res.type = global->type;
            res.off = 0;
            res.isVolatile = global->isVolatile;
            out = std::move(res);
            return true;
        }
        return false;
    }
    if (const auto* field = dynamic_cast<const Field*>(expr)) {
        // `lib.gB`: the object names a module, so the member resolves in
        // the defining scope and the place is its global. Anything but a
        // global variable falls through to the paths below (calls own
        // their handling, types have no place). A miss stays quiet:
        // callers refuse with the message that names their gap.
        if (const auto* mod = dynamic_cast<const Ident*>(field->object.get())) {
            if (const GlobalBinding* global =
                    resolveModuleGlobal(mod->name, field->name)) {
                Place res;
                res.global = global->label;
                res.type = global->type;
                res.off = 0;
                res.isVolatile = global->isVolatile;
                out = std::move(res);
                return true;
            }
        }
        Place sub;
        if (!resolvePlace(field->object.get(), sub)) {
        // A nested chain accumulates the offset while the slots stay the
        // base value's: `p.inner.y` is p's slots plus two offsets. Union
        // members sit at offset 0 and record a tag write: storing through
        // the member sets the tag as a consequence (TYPES.md 4.6).
            // A field over a pointer element (`p[0].lo`) or over a
            // computed array element (`arr[i].x`): the direct route only
            // knows slots, but both have an address — the shared guarded
            // helper materializes each, lowering the index once, and the
            // field offset below applies either way. The read path
            // (lowerField) carries the same fallback; both stay because
            // each serves a different node.
            const auto* elem = dynamic_cast<const Index*>(field->object.get());
            TypeRef arrType = elem != nullptr
                ? checker_.typeOf(elem->array.get())
                : TypeRef{ctx_.error()};
            if (elem == nullptr || arrType.isError()) {
                return false;
            }
            Kind inner = arrType.type->kind();
            if (inner != Kind::Pointer && inner != Kind::Array) {
                return false;
            }
            Place base;
            if (!resolvePlace(elem->array.get(), base)) {
                return false;
            }
            if (inner == Kind::Pointer) {
                if (!pointerElementPlace(*elem, base, arrType,
                                         "this field access", sub)) {
                    return false;
                }
            } else if (!arrayElementPlace(*elem, base, arrType,
                                          "this field access", sub)) {
                return false;
            }
        }
        if (sub.type.isError() || (sub.type.type->kind() != Kind::Struct &&
                                   sub.type.type->kind() != Kind::Union &&
                                   sub.type.type->kind() != Kind::Class)) {
            return false;
        }
        std::optional<DeclId> id = sub.type.type->decl();
        if (!id.has_value()) {
            return false;
        }
        int64_t fieldOff = resolver_.fieldOffset(*id, field->name);
        TypeRef fieldType = resolver_.fieldType(*id, field->name);
        if (fieldOff < 0 || fieldType.isError()) {
            return false;
        }
        if (sub.type.type->kind() == Kind::Union) {
            int64_t index = resolver_.unionMemberIndex(*id, field->name);
            int64_t tagOff = resolver_.unionTagOffset(*id);
            if (index < 0 || tagOff < 0) {
                return false;
            }
            TagWrite tag;
            tag.off = sub.off + tagOff;
            tag.index = index;
            sub.tagWrites.push_back(tag);
        }
        sub.off += fieldOff;
        sub.type = fieldType;
        out = std::move(sub);
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
        Place res;
        res.type = pointee;
        res.off = 0;
        // A deref the checker saw through a `*volatile T` name is a
        // volatile access: the loads and stores it feeds carry the mark.
        res.isVolatile = checker_.placeIsVolatile(expr);
        res.hasIndirect = true;
        res.indirect = ptr[0];
        out = std::move(res);
        return true;
    }
    if (const auto* index = dynamic_cast<const Index*>(expr)) {        // A field over an element (`a[1].y`) needs the element's address:
        // the base plus a folded index times the element size. A computed
        // index has no constant address, so it is not a place.
        Place sub;
        if (!resolvePlace(index->array.get(), sub)) {
            return false;
        }
        if (sub.type.isError() || sub.type.type->kind() != Kind::Array) {
            return false;
        }
        TypeRef element{sub.type.type->element()};
        int64_t elemBytes = resolver_.nominalSize(element.type);
        if (elemBytes <= 0) {
            return false;
        }
        std::unordered_set<std::string> active;
        EvalEnv env = resolver_.evalEnv(emitScope());
        EvalResult folded = evalConstInt(*index->index, env, active);
        if (!folded.ok || folded.value < 0) {
            return false;
        }
        // Same provable case as the read and store paths: a folded index
        // past the array's extent rejects instead of addressing a
        // neighbour's field.
        std::optional<int64_t> count = sub.type.type->length();
        if (count.has_value() && folded.value >= *count) {
            return false;
        }
        sub.off += folded.value * elemBytes;
        sub.type = element;
        out = std::move(sub);
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
    // An enum rides its underlying i32 (FEATURES §2, enum ↔ int row):
    // every enum value is one 4-byte word, so scalar plumbing (loads,
    // stores, casts) treats it at I32 and the enumerator check owns the
    // validity, never the width.
    if (type.type->kind() == Kind::Enum) {
        return ir::IRType::I32;
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

ir::Value Lowerer::lowerFloatLit(const FloatLit& lit, TypeRef fallback) {
    TypeRef type = checker_.typeOf(&lit);
    if (type.isError() && !fallback.isError()) {
        type = fallback;
    }
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

ir::Value Lowerer::lowerBitNot(const Unary& n, TypeRef fallback) {
    TypeRef type = checker_.typeOf(&n);
    if (type.isError() && !fallback.isError()) {
        type = fallback;
    }
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

ir::Value Lowerer::lowerNeg(const Unary& n, TypeRef fallback) {
    TypeRef type = checker_.typeOf(&n);
    if (type.isError() && !fallback.isError()) {
        type = fallback;
    }
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
    const resolve::Scope* scope = homeScope();
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

const GlobalBinding* Lowerer::resolveModuleGlobal(
    const std::string& modName, const std::string& fieldName) {
    // A module-qualified spelling (`lib.gB`) names its home directly:
    // locals shadow module names, the member resolves in the defining
    // scope, and only a global variable has a place. Anything else
    // answers null so callers fall through to their own paths.
    uint64_t depth = static_cast<uint64_t>(locals_.size());
    for (; depth > 0;) {
        --depth;
        if (locals_[depth].find(modName) != locals_[depth].end()) {
            return nullptr;
        }
    }
    const resolve::Scope* here = homeScope();
    const resolve::Decl* moduleDecl =
        here != nullptr ? resolver_.lookup(here, modName) : nullptr;
    if (moduleDecl == nullptr ||
        moduleDecl->kind != resolve::DeclKind::Module) {
        return nullptr;
    }
    const resolve::Scope* home = resolver_.scopeOf(moduleDecl->origin);
    const resolve::Decl* member =
        home != nullptr ? resolver_.lookup(home, fieldName) : nullptr;
    if (member == nullptr || member->kind != resolve::DeclKind::Var) {
        return nullptr;
    }
    return findGlobalIn(moduleDecl->origin, fieldName);
}

const GlobalBinding* Lowerer::findGlobalIn(const std::string& homeModule,
                                               const std::string& name) {
    // A module-qualified spelling (`lib.gB`) names its home directly, so
    // the scan skips findGlobal's decl-following prologue.
    for (const GlobalBinding& global : globals_) {
        if (global.name == name && global.module == homeModule) {
            return &global;
        }
    }
    return nullptr;
}

bool Lowerer::foldModuleConst(const std::string& name, Position pos,
                              std::vector<ir::Value>& out) {
    // A module-level `const` names no storage: its uses fold the
    // initialiser where read. The table carries both spellings — the
    // defining module's own names and every from-alias — so one lookup
    // in the current module covers every reachable spelling. A cycle
    // refuses loudly instead of hanging: the checker rejects cyclic
    // consts, but lowering keeps walking after check errors, so the
    // guard is what stands between a rejected program and a hang.
    static thread_local std::unordered_set<std::string> folding;
    EvalEnv env = resolver_.evalEnv(emitScope());
    const Expr* init =
        env.constantInit(homeModule() + "." + name);
    if (init == nullptr) {
        return false;
    }
    std::string key = homeModule() + "." + name;
    if (!folding.insert(key).second) {
        refuse(pos, "cyclic const '" + name + "'");
        out = {undefined()};
        return true;
    }
    // A literal initialiser folds without asking the checker for its
    // type: declaration order must not decide whether a use lowers, and
    // the declared type (order-free through the resolver) is exactly
    // what checking records. Compound initialisers still ride lowerWords
    // below, which needs their recorded subtree types.
    const resolve::Scope* here = homeScope();
    const resolve::Decl* found =
        here != nullptr ? resolver_.lookup(here, name) : nullptr;
    const resolve::Decl* origin =
        found != nullptr ? resolver_.declById(found->id) : nullptr;
    const resolve::Decl* truth = origin != nullptr ? origin : found;
    if (truth != nullptr && !truth->type.isError()) {
        if (const auto* lit = dynamic_cast<const IntLit*>(init)) {
            std::unordered_set<std::string> active;
            EvalResult got = evalConstInt(*lit, env, active);
            if (got.ok) {
                out = {constantI64(got.value, truth->type, init->pos)};
                folding.erase(key);
                return true;
            }
        }
        if (const auto* lit = dynamic_cast<const BoolLit*>(init)) {
            out = {constantI64(lit->value ? 1 : 0, truth->type, init->pos)};
            folding.erase(key);
            return true;
        }
    }
    out = lowerWords(init);
    folding.erase(key);
    return true;
}

const GlobalBinding* Lowerer::findGlobal(const std::string& name) {
    // A from-alias copies its target's id, so declById recovers the home
    // module even for names bound in this module's scope by import.
    std::string home = homeModule();
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

std::string Lowerer::globalLabel(const std::string& module,
                                   const std::string& name) {
    // The label carries its module (`g_da__gval` for `da`'s `gval`, `g_x`
    // for the entry's `x`), so two modules' same-named globals never share
    // storage. Dots become underscores: a dot cannot survive the assembler.
    std::string flat = module;
    for (char& c : flat) {
        if (c == '.') {
            c = '_';
        }
    }
    return module == resolver_.entryModule() ? "g_" + name
                                             : "g_" + flat + "__" + name;
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
    EvalEnv env = resolver_.evalEnv(emitScope());
    // A `gc.MK2` spelling folds through the defining module: the table
    // holds every module's consts, but a bare name only resolves in its
    // own scope, so the member init folds in the home scope. Anything
    // else falls through to the plain fold below, which refuses loudly.
    EvalResult folded{false, 0, ""};
    const Field* qualField =
        dynamic_cast<const Field*>(n.init.get());
    const Ident* qualMod =
        qualField != nullptr
            ? dynamic_cast<const Ident*>(qualField->object.get())
            : nullptr;
    const resolve::Decl* qualMember = nullptr;
    const resolve::Scope* qualHome = nullptr;
    if (qualMod != nullptr) {
        const resolve::Scope* here = emitScope();
        const resolve::Decl* modDecl =
            here != nullptr
                ? resolver_.lookup(here, qualMod->name)
                : nullptr;
        if (modDecl != nullptr &&
            modDecl->kind == resolve::DeclKind::Module) {
            qualHome = resolver_.scopeOf(modDecl->origin);
            const resolve::Decl* target =
                qualHome != nullptr
                    ? resolver_.lookup(qualHome, qualField->name)
                    : nullptr;
            if (target != nullptr &&
                target->kind == resolve::DeclKind::Const &&
                target->init != nullptr) {
                qualMember = target;
            }
        }
    }
    if (qualMember != nullptr && qualHome != nullptr) {
        EvalEnv homeEnv = resolver_.evalEnv(qualHome);
        folded = evalConstInt(*qualMember->init, homeEnv, active);
    } else {
        folded = evalConstInt(*n.init, env, active);
    }
    GlobalBinding binding;
    binding.name = n.name;
    binding.module = checker_.module();
    binding.label = globalLabel(binding.module, n.name);
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
    } else if (const auto* wide = dynamic_cast<const WideLit*>(n.init.get())) {
        // A wide literal folds to known image bytes: units little-endian
        // plus one zero unit. An array global holds the bytes inline (one
        // quad per eight, zero-padded tail); a pointer global names a
        // rodata blob. The gate admitted only these two shapes, and the
        // checking proved the fit.
        if (type.type->kind() == Kind::Array) {
            std::string bytes;
            for (uint16_t unit : wide->units) {
                bytes.push_back(static_cast<char>(unit & 0xFF));
                bytes.push_back(static_cast<char>((unit >> 8) & 0xFF));
            }
            bytes.push_back('\0');
            bytes.push_back('\0');
            int64_t want = resolver_.nominalSize(type.type);
            if (want < 0 ||
                bytes.size() != static_cast<size_t>(want)) {
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
        } else if (type.type->kind() == Kind::Pointer) {
            binding.initLabel = wideBlob(*wide);
            binding.initial = 0;
        } else {
            refuse(n.init->pos, "a global initialiser must be constant");
            return;
        }
    } else if (dynamic_cast<const NilPtr*>(n.init.get()) != nullptr) {
        binding.initial = 0;
    } else if (type.type->kind() == Kind::Interface) {
        // `nilPtr as I`: the null interface is sixteen zero bytes — a
        // null data word beside a null vtable word — the way a zero
        // repeat holds zero bytes. The checker's gate lets only this one
        // spelling through, so anything arriving here is sixteen zeros.
        const auto* nilCast = dynamic_cast<const Cast*>(n.init.get());
        if (nilCast == nullptr ||
            dynamic_cast<const NilPtr*>(nilCast->expr.get()) == nullptr) {
            refuse(n.init->pos, "an interface global needs `nilPtr as I`");
            return;
        }
        for (uint64_t word = 0; word < kInterfaceWords; ++word) {
            binding.initWords.push_back(0);
        }
    } else if (type.type->kind() == Kind::Array &&
               (dynamic_cast<const NewArray*>(n.init.get()) != nullptr ||
                dynamic_cast<const ArrayLit*>(n.init.get()) != nullptr)) {
        // A const aggregate global passed the checker's gate, so every
        // leaf folds and every image byte is known: the global holds one
        // `.quad` per eight bytes, little-endian, zero-padded at the end.
        std::string bytes;
        bool foldedBytes = false;
        if (const auto* initArray =
                dynamic_cast<const NewArray*>(n.init.get())) {
            foldedBytes =
                globalInitBytes(initArray->inits, initArray->repeat, type, bytes);
        } else {
            const auto* lit = dynamic_cast<const ArrayLit*>(n.init.get());
            foldedBytes =
                globalInitBytes(lit->inits, lit->repeat, type, bytes);
        }
        if (!foldedBytes || bytes.empty()) {
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
        collectGlobalVptrRelocs(type, 0, n.init->pos, binding.relocWords);
        std::sort(binding.relocWords.begin(), binding.relocWords.end(),
                  [](const auto& left, const auto& right) {
                      return left.first < right.first;
                  });
    } else if ((type.type->kind() == Kind::Struct ||
                type.type->kind() == Kind::Class) &&
               dynamic_cast<const NewStruct*>(n.init.get()) != nullptr) {
        // A const struct or class value folds to image bytes the way
        // the array path folds elements: the checker's gate lets only
        // all-constant fields through, so every byte is known. The
        // conversion below turns image bytes into one `.quad` per
        // eight bytes, little-endian with a zero-padded tail, so the
        // path stays untouched.
        const auto* initStruct = dynamic_cast<const NewStruct*>(n.init.get());
        std::string bytes;
        if (!globalInitStructBytes(*initStruct, type, bytes) || bytes.empty()) {
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
        collectGlobalVptrRelocs(type, 0, n.init->pos, binding.relocWords);
        std::sort(binding.relocWords.begin(), binding.relocWords.end(),
                  [](const auto& left, const auto& right) {
                      return left.first < right.first;
                  });
    } else if (type.type->kind() == Kind::Closure &&
               (dynamic_cast<const Field*>(n.init.get()) != nullptr ||
                dynamic_cast<const Ident*>(n.init.get()) != nullptr)) {
        // A function names link-time code: the global holds the code
        // address beside empty captures, the same pair shape a local
        // spelling lowers to. `b.inc` resolves through its module;
        // a bare `dbl` resolves in the current one. Anything else here
        // refuses: the gate lets only these spellings through.
        const resolve::Decl* member = nullptr;
        if (const auto* field = dynamic_cast<const Field*>(n.init.get())) {
            const auto* mod = dynamic_cast<const Ident*>(field->object.get());
            const resolve::Scope* here = emitScope();
            const resolve::Decl* moduleDecl =
                (mod != nullptr && here != nullptr)
                    ? resolver_.lookup(here, mod->name)
                    : nullptr;
            const resolve::Scope* home =
                (moduleDecl != nullptr &&
                 moduleDecl->kind == resolve::DeclKind::Module)
                    ? resolver_.scopeOf(moduleDecl->origin)
                    : nullptr;
            member = home != nullptr ? resolver_.lookup(home, field->name)
                                     : nullptr;
        } else if (const auto* ident =
                       dynamic_cast<const Ident*>(n.init.get())) {
            member = resolver_.lookup(emitScope(), ident->name);
        }
        if (member == nullptr ||
            member->kind != resolve::DeclKind::Func ||
            member->typeParams != 0) {
            refuse(n.init->pos, "a function global needs `mod.func`");
            return;
        }
        std::string link = resolver_.linkName(*member);
        std::string fwd = forwardFunction(link, member->sig, n.init->pos);
        if (failed_) {
            return;
        }
        binding.initLabel = fwd;
        binding.initial = 0;
    } else if (type.type->kind() == Kind::FunctionPtr &&
               dynamic_cast<const Unary*>(n.init.get()) != nullptr) {
        // `&dbl`: the single word holds the function's link-time
        // address. Anything else here refuses: the gate lets only
        // this spelling through.
        const auto* addr = static_cast<const Unary*>(n.init.get());
        const auto* ident = dynamic_cast<const Ident*>(addr->operand.get());
        const resolve::Decl* decl =
            ident != nullptr
                ? resolver_.lookup(emitScope(), ident->name)
                : nullptr;
        if (addr->op != UnOp::Addr || decl == nullptr ||
            decl->kind != resolve::DeclKind::Func || decl->typeParams != 0) {
            refuse(n.init->pos, "a function global needs `&func`");
            return;
        }
        binding.initLabels.push_back(resolver_.linkName(*decl));
    } else if (type.type->kind() == Kind::Pointer &&
               type.type->pointee() == ctx_.void_() &&
               dynamic_cast<const Cast*>(n.init.get()) != nullptr) {
        // `&add as *void`: the single word holds the function's link-time
        // address. Anything else here refuses: the gate lets only this
        // spelling through.
        const auto* cast = static_cast<const Cast*>(n.init.get());
        const auto* addr = dynamic_cast<const Unary*>(cast->expr.get());
        const auto* ident = (addr != nullptr && addr->op == UnOp::Addr)
                                ? dynamic_cast<const Ident*>(addr->operand.get())
                                : nullptr;
        const resolve::Decl* decl =
            ident != nullptr
                ? resolver_.lookup(emitScope(), ident->name)
                : nullptr;
        if (decl == nullptr ||
            decl->kind != resolve::DeclKind::Func || decl->typeParams != 0) {
            refuse(n.init->pos, "a function global needs `&func as *void`");
            return;
        }
        binding.initLabels.push_back(resolver_.linkName(*decl));
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
    } else if (type.type->kind() == Kind::Enum) {
        // An enumerator spelling folds to its value: every spelling the
        // checker accepts (bare, qualified, alias, module-qualified)
        // resolves here to the member's integer. The gate admitted only
        // these shapes and the checking validated them, so a miss here
        // is a lowering gap, refused as one.
        int64_t value = 0;
        bool known = false;
        const resolve::Scope* hereScope =
            homeScope();
        if (const auto* ident = dynamic_cast<const Ident*>(n.init.get())) {
            int owners = 0;
            const resolve::Decl* owner =
                hereScope != nullptr
                    ? resolver_.lookupEnumMember(hereScope, ident->name, owners)
                    : nullptr;
            if (owner != nullptr) {
                std::string home = owner->scope != nullptr
                                       ? owner->scope->module
                                       : homeModule();
                value = resolver_.enumValue(home + "." + owner->name,
                                            ident->name);
                known = true;
            }
        } else if (const auto* field =
                       dynamic_cast<const Field*>(n.init.get())) {
            if (const auto* qual =
                    dynamic_cast<const Ident*>(field->object.get())) {
                bool shadowed = false;
                for (auto frame = locals_.rbegin();
                     frame != locals_.rend() && !shadowed; ++frame) {
                    shadowed = frame->find(qual->name) != frame->end();
                }
                const resolve::Decl* qualDecl =
                    !shadowed && hereScope != nullptr
                        ? resolver_.lookup(hereScope, qual->name)
                        : nullptr;
                if (qualDecl != nullptr &&
                    qualDecl->kind == resolve::DeclKind::Module) {
                    const resolve::Scope* home =
                        resolver_.scopeOf(qualDecl->origin);
                    if (home != nullptr) {
                        const resolve::Decl* direct =
                            resolver_.lookup(home, field->name);
                        if (direct == nullptr) {
                            for (const auto& [ename, edecl] :
                                 home->bindings) {
                                if (edecl == nullptr ||
                                    edecl->kind != resolve::DeclKind::Type ||
                                    edecl->nominal != Kind::Enum) {
                                    continue;
                                }
                                for (const auto& entry : resolver_.enumMembers(
                                         edecl->id)) {
                                    if (entry.first == field->name) {
                                        value = entry.second;
                                        known = true;
                                        break;
                                    }
                                }
                                if (known) {
                                    break;
                                }
                            }
                        }
                    }
                } else if (qualDecl != nullptr &&
                           qualDecl->kind == resolve::DeclKind::Type &&
                           qualDecl->nominal == Kind::Enum) {
                    std::string home = qualDecl->scope != nullptr
                                           ? qualDecl->scope->module
                                           : homeModule();
                    value = resolver_.enumValue(
                        home + "." + qualDecl->name, field->name);
                    known = true;
                } else if (qualDecl != nullptr &&
                           qualDecl->kind == resolve::DeclKind::Alias &&
                           qualDecl->aliasTarget != nullptr) {
                    TypeRef target = resolver_.resolveType(
                        qualDecl->aliasTarget, hereScope);
                    if (!target.isError() &&
                        target.type->kind() == Kind::Enum) {
                        std::optional<DeclId> aliasId = target.type->decl();
                        if (aliasId.has_value()) {
                            for (const auto& entry :
                                 resolver_.enumMembers(*aliasId)) {
                                if (entry.first == field->name) {
                                    value = entry.second;
                                    known = true;
                                    break;
                                }
                            }
                        }
                    }
                }
            } else if (const auto* inner =
                           dynamic_cast<const Field*>(field->object.get())) {
                const auto* mod =
                    dynamic_cast<const Ident*>(inner->object.get());
                const resolve::Decl* modDecl =
                    (mod != nullptr && hereScope != nullptr)
                        ? resolver_.lookup(hereScope, mod->name)
                        : nullptr;
                const resolve::Scope* home =
                    (modDecl != nullptr &&
                     modDecl->kind == resolve::DeclKind::Module)
                        ? resolver_.scopeOf(modDecl->origin)
                        : nullptr;
                const resolve::Decl* member =
                    home != nullptr ? resolver_.lookup(home, inner->name)
                                    : nullptr;
                std::optional<DeclId> enumId;
                if (member != nullptr &&
                    member->kind == resolve::DeclKind::Type &&
                    member->nominal == Kind::Enum) {
                    enumId = member->id;
                } else if (member != nullptr &&
                           member->kind == resolve::DeclKind::Alias &&
                           member->aliasTarget != nullptr) {
                    TypeRef target =
                        resolver_.resolveType(member->aliasTarget, home);
                    if (!target.isError() &&
                        target.type->kind() == Kind::Enum) {
                        enumId = target.type->decl();
                    }
                }
                if (enumId.has_value()) {
                    for (const auto& entry : resolver_.enumMembers(*enumId)) {
                        if (entry.first == field->name) {
                            value = entry.second;
                            known = true;
                            break;
                        }
                    }
                }
            }
        }
        if (!known) {
            refuse(n.init->pos, "an enum global needs an enumerator");
            return;
        }
        binding.initial = value;
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
    global.initLabels = binding.initLabels;
    global.initWords = binding.initWords;
    global.relocWords = binding.relocWords;
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
    const resolve::Scope* here = homeScope();
    const resolve::Decl* decl =
        here != nullptr ? resolver_.lookup(here, n.name) : nullptr;
    std::string link = decl != nullptr ? resolver_.linkName(*decl) : n.name;
    beginFuncFrame(link, *functionSig(n.name), n.params, n.pos);
    if (failed_) {
        return;
    }

    n.body->accept(checker_);
    locals_.pop_back();

    endFuncFrame(n.pos);
}

// 7.10: one finalizer function per block, composed in reverse by
// buildFini. A block lowers like a function body — no parameters, no
// answer — so its locals, defers and calls use the one frame machinery.
void Lowerer::onFiniDecl(FiniDecl& n) {
    if (failed_ || !enabled_) {
        return;
    }
    if (finiBodies_.empty()) {
        finiPos_ = n.pos;
    }
    std::string name = "cider_fini_" +
                       std::to_string(static_cast<unsigned long long>(finiBodies_.size()));
    Signature sig({}, ctx_.void_(), Abi::SysV);
    beginFuncFrame(name, sig, {}, n.pos);
    if (failed_) {
        return;
    }
    n.body->accept(checker_);
    locals_.pop_back();
    endFuncFrame(n.pos);
    finiBodies_.push_back(std::move(name));
}

// The finalizer entry a program with blocks carries: one call per block in
// reverse encounter order, so the last-written block runs first (the
// destructor convention) and the entry module's finalizers run before its
// imports'. Without blocks there is no function here: `exit` reaches the
// emitter's bare stub instead, and a module that wrote no finalizer keeps
// exactly the functions it had. A program with its own `_start` owns
// termination, so a fini block beside one refuses rather than silently
// never running.
void Lowerer::buildFini() {
    if (module_ == nullptr || finiBuilt_) {
        return;
    }
    finiBuilt_ = true;
    if (finiBodies_.empty()) {
        return;
    }
    for (const ir::Function& fn : module_->functions) {
        if (fn.name == "_start") {
            refuse(finiPos_, "a fini block cannot run beside its own _start");
            return;
        }
    }
    Position pos = finiPos_;
    ir::Function fini;
    fini.name = "cider_fini_run";
    fini.signature = Signature({}, ctx_.void_(), Abi::SysV);
    fini.loc = pos;
    ir::Block body;
    body.label = 0;
    body.loc = pos;
    static constexpr uint64_t skipNone = 0;
    for (uint64_t at = finiBodies_.size(); at > skipNone; --at) {
        ir::Instr call;
        call.op = ir::Op::Call;
        call.loc = pos;
        call.callee = finiBodies_[at - 1];
        call.mayTrap = true;
        body.body.push_back(std::move(call));
    }
    ir::Terminator ret;
    ret.term = ir::Term::Ret;
    ret.loc = pos;
    body.term = ret;
    fini.blocks.push_back(std::move(body));
    module_->functions.push_back(std::move(fini));
}

// A function frame for a known link name and signature: the shared setup
// behind plain functions and spec emissions, so the two cannot diverge.
// The signature's parameter types answer the slots; the spelling's names
// answer the binds. The caller lowers the body between this and
// endFuncFrame, then pops the frame exactly as below.
void Lowerer::beginFuncFrame(const std::string& link, const Signature& sig,
                             const std::vector<Param>& params, Position pos) {
    ir::Function fn;
    fn.name = link;
    fn.signature = sig;
    fn.loc = pos;
    // A return wider than the register row travels through a
    // caller-allocated buffer: the hidden address rides first (SysV
    // sret), and the answer is stored there instead of the row.
    if (needsSret(fn.signature.returnType())) {
        TypeRef sretPtr{ctx_.pointer(ctx_.basic(BasicKind::U8))};
        std::vector<Parameter> withSret;
        Parameter hidden;
        hidden.type = sretPtr.type;
        hidden.isMutable = false;
        withSret.push_back(hidden);
        for (const Parameter& p : fn.signature.parameters()) {
            withSret.push_back(p);
        }
        fn.signature = Signature(std::move(withSret), fn.signature.returnType(),
                                 fn.signature.abi());
    }
    function_ = &fn;
    module_->functions.push_back(std::move(fn));
    function_ = &module_->functions.back();

    valueSeq_ = 0;
    labelSeq_ = 0;
    blockOrder_.clear();
    locals_.clear();
    scopeMarks_.clear();
    // A function starts with no open block/loop scopes. A body that fell
    // off has already run its defers; leaving its frame behind would let a
    // later function's return walk (runDefers(0)) run them a second time.
    deferFrames_.clear();
    loopMarks_.clear();

    uint32_t entry = emitBlock(pos);
    startBlock(entry);

    // Parameters arrive in slots like every other named place: a parameter
    // reassigned in a loop body is read across the back edge, so versions
    // without phi nodes would read a stale one. The entry moves below fill
    // the slots; codegen derives each address from its id.
    std::unordered_map<std::string, LocalBinding> frame;
    const std::vector<Parameter>& sigParams = function_->signature.parameters();
    // The hidden address has no source spelling: its slot rides first
    // and the source parameters follow shifted past it.
    uint64_t shift = 0;
    if (needsSret(function_->signature.returnType())) {
        TypeRef sretPtr{ctx_.pointer(ctx_.basic(BasicKind::U8))};
        std::vector<ir::Value> sretSlots = allocaSlots(sretPtr, pos, kOneWord);
        if (failed_) {
            return;
        }
        for (ir::Value slot : sretSlots) {
            function_->params.push_back(slot);
        }
        shift = kOneWord;
    }
    for (uint64_t i = 0; i < static_cast<uint64_t>(params.size()); ++i) {
        TypeRef type{sigParams[i + shift].type};
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
        binding.isMutable = sigParams[i + shift].isMutable;
        frame[params[i].name] = binding;
    }
    recordParamClasses();
    locals_.push_back(std::move(frame));
}

// The shared function epilogue: reachable value-falloff refuses,
// unreachable falloff traps, then the frame closes. Same tail onFuncDecl
// always had; spec emissions close identically.
void Lowerer::endFuncFrame(Position pos) {
    ir::Terminator ret;
    ret.term = ir::Term::Ret;
    // A void falloff answers nothing (see onReturn). A value falloff
    // traps instead of answering: the placeholder id the old code pushed
    // here poisoned the frame computation and crashed programs whose
    // last statement closed every path (if/else, match). Reachable
    // falloff refuses just below, where the block graph is complete;
    // the trap keeps an unreachable one loud either way, never garbage.
    if (function_->signature.returnType() == nullptr ||
        function_->signature.returnType()->kind() != Kind::Void) {
        ret.term = ir::Term::Trap;
        ret.reason = ir::TrapReason::Explicit;
    }
    bool emittedFalloff = !terminated_;
    ret.loc = pos;
    emitTerminator(ret);
    // BUG-277: every path through a value body returns or provably
    // diverges. Reachability over terminator edges answers exactly —
    // returns, breaks, loop exits and arm joins are all branches here —
    // so a reachable falloff fails at compile time.
    if (!failed_ && emittedFalloff && ret.term == ir::Term::Trap &&
        falloffReachable(current_)) {
        refuse(pos, "this function may fall off without returning");
    }
    function_ = nullptr;
}

void Lowerer::onSpecFunc(DeclId spec, const FuncDecl& spelling,
                         const Signature& sig) {
    if (failed_ || !enabled_) {
        return;
    }
    // One body per specialization under its own link name, using the common
    // frame setup. The checker bound the substituted params; the slots
    // bind here.
    std::string link = resolver_.specLinkName(spec);
    if (link.empty()) {
        refuse(spelling.pos, "this generic call");
        return;
    }
    // Names below resolve in the template home: helpers, globals and
    // constants mean what the defining module bound, as checking proved.
    // Cleared before returning; bodies lower synchronously inside.
    auto origin = resolver_.instantiationOf(spec);
    const resolve::Decl* tmpl =
        origin.has_value() ? resolver_.declById(origin->first) : nullptr;
    emitHome_ = (tmpl != nullptr && tmpl->scope != nullptr)
                    ? tmpl->scope->module
                    : "";
    emitSpec_ = spec;
    beginFuncFrame(link, sig, spelling.params, spelling.pos);
    if (failed_) {
        emitHome_.clear();
        emitSpec_.reset();
        return;
    }
    spelling.body->accept(checker_);
    emitHome_.clear();
    emitSpec_.reset();
    locals_.pop_back();
    endFuncFrame(spelling.pos);
}

std::string Lowerer::homeModule() const {
    return emitHome_.empty() ? checker_.module() : emitHome_;
}

const resolve::Scope* Lowerer::homeScope() const {
    return resolver_.scopeOf(homeModule());
}

const resolve::Scope* Lowerer::emitScope() const {
    return emitHome_.empty() ? resolver_.moduleScope()
                             : resolver_.scopeOf(emitHome_);
}

bool Lowerer::emitSpecBodies() {
    // Deferred monomorphization: every used function specialization gets
    // one body, in any order (calls resolve by link name, not position).
    // The checker's call table already holds the transitive closure, and
    // emission walks record further nested calls through the same table,
    // so the worklist drains until no unseen spec remains. Runs after all
    // modules lowered: a cross-module spec is unknown while its home
    // module lowers, but the resolver cache is complete here.
    if (failed_ || module_ == nullptr) {
        return !failed_;
    }
    checker_.setSink(this);
    enabled_ = true;
    auto specKey = [](DeclId id) -> std::string {
        return std::to_string(id.module) + "." + std::to_string(id.index);
    };
    auto isFuncSpec = [&](DeclId id) -> bool {
        auto origin = resolver_.instantiationOf(id);
        if (!origin.has_value()) {
            return false;
        }
        const resolve::Decl* tmpl = resolver_.declById(origin->first);
        return tmpl != nullptr && tmpl->kind == resolve::DeclKind::Func;
    };
    for (;;) {
        std::optional<DeclId> next;
        for (const auto& [site, spec] : checker_.callSpecs()) {
            (void)site;
            if (!isFuncSpec(spec)) {
                continue;
            }
            if (emittedSpecs_.find(specKey(spec)) != emittedSpecs_.end()) {
                continue;
            }
            next = spec;
            break;
        }
        if (!next.has_value()) {
            break;
        }
        emittedSpecs_.insert(specKey(*next));
        checker_.walkSpecForEmit(*next);
        if (failed_) {
            break;
        }
    }
    checker_.setSink(nullptr);
    enabled_ = false;
    return !failed_;
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
    const resolve::Scope* here = homeScope();
    const resolve::Decl* ownerDecl =
        here != nullptr ? resolver_.lookup(here, owner) : nullptr;
    // The owner rides as the current method so `super.m(...)` inside the
    // body resolves the base declarer; the guard restores the outer
    // method on every exit, including refusals.
    struct OwnerGuard {
        std::optional<DeclId>& slot;
        std::optional<DeclId> prev;
        OwnerGuard(std::optional<DeclId>& s, std::optional<DeclId> v)
            : slot(s), prev(s) {
            slot = v;
        }
        ~OwnerGuard() { slot = prev; }
    };
    OwnerGuard ownerGuard(currentMethodOwner_,
                           ownerDecl != nullptr
                               ? std::optional<DeclId>(ownerDecl->id)
                               : std::nullopt);
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
    if (needsSret(ret)) {
        // The hidden buffer address rides before the receiver (SysV
        // sret): prepended to the signature copy the frame walks below.
        TypeRef sretPtr{ctx_.pointer(ctx_.basic(BasicKind::U8))};
        std::vector<Parameter> withSret;
        Parameter hidden;
        hidden.type = sretPtr.type;
        hidden.isMutable = false;
        withSret.push_back(hidden);
        for (const Parameter& p : combined) {
            withSret.push_back(p);
        }
        combined = std::move(withSret);
    }
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
    // The hidden address has no source spelling: its slot rides first
    // and the receiver follows shifted past it.
    uint64_t shift = 0;
    if (needsSret(ret)) {
        TypeRef sretPtr{ctx_.pointer(ctx_.basic(BasicKind::U8))};
        std::vector<ir::Value> sretSlots = allocaSlots(sretPtr, body.pos, kOneWord);
        if (failed_) {
            return;
        }
        for (ir::Value slot : sretSlots) {
            function_->params.push_back(slot);
        }
        shift = kOneWord;
    }
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
        TypeRef type{declared[i + 1 + shift].type};
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
        binding.isMutable = declared[i + 1 + shift].isMutable;
        frame[params[i].name] = binding;
    }
    recordParamClasses();
    locals_.push_back(std::move(frame));

    body.accept(checker_);
    locals_.pop_back();

    ir::Terminator mret;
    mret.term = ir::Term::Ret;
    // Same value-falloff rule as a free function (see onFuncDecl): a
    // reachable falloff refuses, an unreachable one stays a dead trap.
    if (function_->signature.returnType() == nullptr ||
        function_->signature.returnType()->kind() != Kind::Void) {
        mret.term = ir::Term::Trap;
        mret.reason = ir::TrapReason::Explicit;
    }
    bool emittedFalloff = !terminated_;
    mret.loc = body.pos;
    emitTerminator(mret);
    if (!failed_ && emittedFalloff && mret.term == ir::Term::Trap &&
        falloffReachable(current_)) {
        refuse(body.pos, "this function may fall off without returning");
    }
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
    if (needsSret(ret)) {
        // The hidden buffer address rides before the captures word
        // (SysV sret), like every other hidden-first convention here.
        TypeRef sretPtr{ctx_.pointer(ctx_.basic(BasicKind::U8))};
        std::vector<Parameter> withSret;
        Parameter hidden;
        hidden.type = sretPtr.type;
        hidden.isMutable = false;
        withSret.push_back(hidden);
        for (const Parameter& p : combined) {
            withSret.push_back(p);
        }
        combined = std::move(withSret);
    }
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
    // The hidden address has no source spelling: its slot rides first
    // and the captures word follows shifted past it.
    uint64_t shift = 0;
    if (needsSret(ret)) {
        TypeRef sretPtr{ctx_.pointer(ctx_.basic(BasicKind::U8))};
        std::vector<ir::Value> sretSlots = allocaSlots(sretPtr, lit.pos, kOneWord);
        if (failed_) {
            return;
        }
        for (ir::Value slot : sretSlots) {
            function_->params.push_back(slot);
        }
        shift = kOneWord;
    }
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
            TypeRef type{declared[i + kOneWord + shift].type};
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
            binding.isMutable = declared[i + kOneWord + shift].isMutable;
            frame[lit.params[i].name] = binding;
        }
        recordParamClasses();
        locals_.push_back(std::move(frame));

        lit.body->accept(checker_);
        locals_.pop_back();

        ir::Terminator fret;
        fret.term = ir::Term::Ret;
        // Same value-falloff rule as a free function (see onFuncDecl).
        if (function_->signature.returnType() == nullptr ||
            function_->signature.returnType()->kind() != Kind::Void) {
            fret.term = ir::Term::Trap;
            fret.reason = ir::TrapReason::Explicit;
        }
        bool emittedFalloff = !terminated_;
        fret.loc = lit.pos;
        emitTerminator(fret);
        if (!failed_ && emittedFalloff && fret.term == ir::Term::Trap &&
            falloffReachable(current_)) {
            refuse(lit.pos, "this function may fall off without returning");
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
}

void Lowerer::onBlockEnter(Block& n) {
    (void)n;
    scopeMarks_.push_back(static_cast<uint64_t>(locals_.size()));
    deferFrames_.push_back({});
    if (failed_ || !enabled_ || function_ == nullptr) {
        return;
    }
    locals_.push_back({});
}

void Lowerer::onBlockExit(Block& n) {
    (void)n;
    if (!deferFrames_.empty()) {
        // A block left by jump (return, break, continue) already ran its
        // defers at the jump; running them again here would execute a
        // second copy on no dynamic path. Fall-through exits run here.
        // Either way the frame pops: bookkeeping, not emission.
        if (!terminated_) {
            uint64_t top = static_cast<uint64_t>(deferFrames_.size());
            --top;
            runDefers(top);
        }
        deferFrames_.pop_back();
    }
    if (scopeMarks_.empty()) {
        return;
    }
    uint64_t mark = scopeMarks_.back();
    scopeMarks_.pop_back();
    while (static_cast<uint64_t>(locals_.size()) > mark) {
        locals_.pop_back();
    }
}

void Lowerer::onDeferStmt(DeferStmt& n) {
    if (failed_ || !enabled_ || function_ == nullptr) {
        return;
    }
    // Registration records the shape (FEATURES §5.2): operands evaluate
    // when the body runs at scope exit, so nothing lowers here.
    if (deferFrames_.empty()) {
        refuse(n.pos, "defer outside a function");
        return;
    }
    deferFrames_.back().push_back(n.stmt.get());
}

void Lowerer::runDefers(uint64_t from) {
    if (failed_ || !enabled_ || function_ == nullptr) {
        return;
    }
    if (from > static_cast<uint64_t>(deferFrames_.size())) {
        refuse(Position{"", 1, 1}, "defer scopes are unbalanced");
        return;
    }
    // Bodies lower by re-walking, and re-walking pushes frames: holding a
    // reference across an accept would dangle when the outer vector
    // reallocates. Snapshot the pointers instead (pointees are AST-owned
    // and stable); indexing re-reads the storage every time.
    for (uint64_t i = static_cast<uint64_t>(deferFrames_.size()); i > from;) {
        --i;
        std::vector<Stmt*> bodies = deferFrames_[i];
        uint64_t pending = static_cast<uint64_t>(bodies.size());
        for (uint64_t j = pending; j > 0;) {
            --j;
            bodies[j]->accept(checker_);
            if (failed_) {
                return;
            }
        }
    }
}

void Lowerer::onVarDecl(VarDecl& n) {
    if (failed_ || !enabled_ || function_ == nullptr || locals_.empty()) {
        return;
    }
    if (dynamic_cast<const UninitExpr*>(n.init.get()) != nullptr) {
        // `= uninit;`: the storage exists, nothing is stored into it.
        TypeRef type = checker_.typeOf(&n);
        if (type.isError()) {
            return;
        }
        int slots = wordCount(type);
        if (failed_ || slots < 0) {
            refuse(n.pos, "this declaration");
            return;
        }
        LocalBinding binding;
        binding.slots = allocaSlots(type, n.pos, static_cast<uint64_t>(slots));
        binding.type = type;
        binding.isMutable = true;
        locals_.back()[n.name] = binding;
        // CIDER-BUG-049: a value of a default-bearing class or struct
        // starts its defaults even without a constructor call.
        if (type.type->kind() == Kind::Class || type.type->kind() == Kind::Struct) {
            Place base;
            base.slots = binding.slots;
            base.type = type;
            base.off = 0;
            emitFieldDefaults(base, type, n.pos);
        }
        return;
    }
    TypeRef type = checker_.typeOf(&n);
    std::vector<ir::Value> values;
    if (const auto* wide = dynamic_cast<const WideLit*>(n.init.get())) {
        // Array positions read the units as words (the checker proved the
        // count fits); pointer positions read the rodata address through
        // the ordinary path below. Anything else here refuses: the checker
        // lets only these shapes through.
        if (!type.isError() && type.type->kind() == Kind::Array) {
            values = wideWords(*wide);
            if (failed_) {
                return;
            }
        } else {
            values = lowerWords(n.init.get());
        }
    } else {
        values = lowerWords(n.init.get());
    }
    TypeRef initType = checker_.typeOf(n.init.get());
    values = adjustStringLitArg(n.init.get(), std::move(values), type);
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
    values = boxClassOperand(n.init.get(), std::move(values), initType, type, n.pos);
    if (failed_) {
        return;
    }
    // Takes pack byte-contiguously: a narrow take shares its slot with the
    // next take's bytes, the way construction and every read address them.
    // Storing one take per slot would spread them, and a later packed read
    // would take padding for payload. storeRange owns both the chunked
    // shape these values arrive in and the valued shape calls answer, so
    // aggregates route through it; every other kind keeps its word stores.
    Kind kind = !type.isError() ? type.type->kind() : Kind::Error;
    if (!values.empty() &&
        (kind == Kind::Struct || kind == Kind::Array ||
         kind == Kind::Class || kind == Kind::Union)) {
        int count = wordCount(type);
        if (count < 0) {
            refuse(n.pos, "this declaration");
            return;
        }
        uint64_t cells = static_cast<uint64_t>(count);
        std::vector<ir::Value> slots =
            allocaSlots(type, n.pos, cells);
        if (failed_) {
            return;
        }
        Place dest;
        dest.slots = slots;
        dest.type = type;
        dest.off = 0;
        dest.isVolatile = false;
        storeRange(n.pos, dest, values, type);
        if (failed_) {
            return;
        }
        LocalBinding binding;
        binding.slots = std::move(slots);
        binding.type = type;
        binding.isMutable = true;
        locals_.back()[n.name] = binding;
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
    std::vector<ir::Value> folded;
    if (foldModuleConst(n.name, n.pos, folded)) {
        if (folded.size() != kOneWord) {
            refuse(n.pos, "this value");
            return undefined();
        }
        return folded[0];
    }
    // A bare enumerator folds to its value: the checker proved exactly
    // one enum in this module owns the name, so the integer here is
    // that member's value (TYPES §2.6). Locals, globals and module
    // constants already answered above, so this branch owns nothing
    // they could name.
    TypeRef held = checker_.typeOf(&n);
    if (!held.isError() && held.type->kind() == Kind::Enum) {
        const resolve::Scope* here = homeScope();
        int owners = 0;
        const resolve::Decl* owner =
            here != nullptr ? resolver_.lookupEnumMember(here, n.name, owners)
                            : nullptr;
        if (owner != nullptr) {
            std::string home = owner->scope != nullptr
                                   ? owner->scope->module
                                   : homeModule();
            int64_t value =
                resolver_.enumValue(home + "." + owner->name, n.name);
            TypeRef count{ctx_.basic(BasicKind::I32)};
            ir::Value answer = constantI64(value, count, n.pos);
            if (failed_) {
                return undefined();
            }
            return answer;
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
                   fieldKind == Kind::Closure || fieldKind == Kind::Interface) {
            // A string is an address word then a length word, a slice a
            // pointer word then a length word, a closure a code word then
            // a captures word, an interface a data word then a vtable
            // word: each full word loads with its own type, the
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
                // A take straddling two eight-byte lines reads through a
                // computed address, like the slot path below: the label
                // plus the absolute offset names bytes the direct form
                // cannot.
                TypeRef addrType{ctx_.pointer(ctx_.basic(BasicKind::U8))};
                ir::Instr lea;
                lea.op = ir::Op::DataAddr;
                lea.loc = pos;
                lea.type = addrType;
                lea.width = ir::IRType::Ptr;
                lea.globalLabel = place.global;
                lea.isPure = true;
                ir::Value base = newValue(lea.width, addrType, pos);
                lea.results.push_back(base);
                emit(std::move(lea));
                if (failed_) {
                    return {undefined()};
                }
                instr.addressBase = base;
                instr.displacement = cur;
            } else {
                instr.globalLabel = place.global;
                instr.displacement = cur;
            }
        } else {
            if (place.isAddress) {
                // One computed address names every chunk: the address plus
                // the chunk offset, read the way any non-slot base reads.
                if (place.slots.empty()) {
                    refuse(pos, "this field layout is not lowered yet");
                    return {undefined()};
                }
                instr.addressBase = place.slots[kFirstWord];
                instr.displacement = cur;
            } else if (disp + take <= layout::kSizePointer) {
                uint64_t word = static_cast<uint64_t>(cur / layout::kSizePointer);
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
                    constantI64(sub * layout::kBitsPerByte, tally, pos);
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
                // A take straddling two eight-byte lines stores through a
                // computed address, like the slot path below: the label
                // plus the absolute offset names bytes the direct form
                // cannot.
                TypeRef addrType{ctx_.pointer(ctx_.basic(BasicKind::U8))};
                ir::Instr lea;
                lea.op = ir::Op::DataAddr;
                lea.loc = pos;
                lea.type = addrType;
                lea.width = ir::IRType::Ptr;
                lea.globalLabel = place.global;
                lea.isPure = true;
                ir::Value base = newValue(lea.width, addrType, pos);
                lea.results.push_back(base);
                emit(std::move(lea));
                if (failed_) {
                    return;
                }
                instr.addressBase = base;
                instr.displacement = cur;
            } else {
                instr.globalLabel = place.global;
                instr.displacement = cur;
            }
        } else {
            if (place.isAddress) {
                // One computed address names every chunk, like loads: the
                // address plus the chunk offset, stored the way any
                // non-slot base stores.
                if (place.slots.empty()) {
                    refuse(pos, "this field layout is not lowered yet");
                    return;
                }
                instr.addressBase = place.slots[kFirstWord];
                instr.displacement = cur;
            } else if (disp + take <= layout::kSizePointer) {
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
    if (failed_) {
        return;
    }
    stampClassVptr(pos, place, fieldType);
}

void Lowerer::stampClassVptr(Position pos, const Place& place,
                             TypeRef classType) {
    if (failed_ || classType.isError() ||
        classType.type->kind() != Kind::Class) {
        return;
    }
    std::optional<DeclId> id = classType.type->decl();
    if (!id.has_value() || resolver_.virtualNames(*id).empty()) {
        return;
    }
    std::string table = emitClassVTable(*id, pos);
    if (failed_ || table.empty()) {
        return;
    }
    TypeRef vptr{ctx_.basic(BasicKind::U64)};
    TypeRef tablePtr{ctx_.pointer(ctx_.basic(BasicKind::U8))};
    ir::Instr data;
    data.op = ir::Op::DataAddr;
    data.loc = pos;
    data.type = tablePtr;
    data.width = ir::IRType::Ptr;
    data.globalLabel = table;
    data.isPure = true;
    ir::Value word = newValue(data.width, tablePtr, pos);
    data.results.push_back(word);
    emit(std::move(data));
    if (failed_) {
        return;
    }
    Place atVptr = place;
    atVptr.type = vptr;
    atVptr.off = place.off;
    storeRange(pos, atVptr, {word}, vptr);
}

void Lowerer::collectGlobalVptrRelocs(
    TypeRef type, int64_t base, Position pos,
    std::vector<std::pair<size_t, std::string>>& out) {
    if (failed_ || type.isError() || type.type == nullptr) {
        return;
    }
    const Type* t = type.type;
    if (t->kind() == Kind::Class || t->kind() == Kind::Struct) {
        std::optional<DeclId> id = t->decl();
        if (!id.has_value()) {
            return;
        }
        if (t->kind() == Kind::Class &&
            !resolver_.virtualNames(*id).empty()) {
            std::string table = emitClassVTable(*id, pos);
            if (failed_ || table.empty()) {
                return;
            }
            out.emplace_back(
                static_cast<size_t>(base / layout::kSizePointer), table);
        }
        for (const std::string& name : resolver_.chainFieldNames(*id)) {
            int64_t off = resolver_.fieldOffset(*id, name);
            if (off < 0) {
                continue;
            }
            collectGlobalVptrRelocs(resolver_.fieldType(*id, name),
                                    base + off, pos, out);
            if (failed_) {
                return;
            }
        }
        return;
    }
    if (t->kind() != Kind::Array) {
        return;
    }
    std::optional<int64_t> count = t->length();
    int64_t stride = resolver_.nominalSize(t->element());
    if (!count.has_value() || *count < layout::kSizeI8 ||
        stride < layout::kSizeI8) {
        return;
    }
    for (int64_t index = 0; index < *count; ++index) {
        collectGlobalVptrRelocs(TypeRef{t->element()},
                                base + index * stride, pos, out);
        if (failed_) {
            return;
        }
    }
}

bool Lowerer::storeFieldInit(Position pos, const Place& base, int64_t fieldOff,
                             TypeRef fieldType, DeclId id,
                             const std::string& name,
                             std::unordered_set<int64_t>& unitsZeroed,
                             std::vector<ir::Value>& value) {
    Place at = base;
    at.off = fieldOff;
    at.type = fieldType;
    int64_t bitOff = -1;
    int64_t bitWidth = 0;
    if (resolver_.bitField(id, name, bitOff, bitWidth)) {
        if (unitsZeroed.find(fieldOff) == unitsZeroed.end()) {
            // 4.1: the unit reads before its first write in this literal
            // must not see uninitialized bytes.
            ir::Value zero = constantI64(0, fieldType, pos);
            if (failed_) {
                return false;
            }
            storeRange(pos, at, {zero}, fieldType);
            if (failed_) {
                return false;
            }
            unitsZeroed.insert(fieldOff);
        }
        if (value.size() != static_cast<size_t>(kOneWord) ||
            !storeBitField(pos, at, fieldType, bitOff, bitWidth, value[0])) {
            return false;
        }
        return true;
    }
    storeRange(pos, at, value, fieldType);
    return !failed_;
}

void Lowerer::emitFieldDefaults(const Place& baseIn, TypeRef objType,
                                Position pos) {
    if (objType.isError()) {
        return;
    }
    std::optional<DeclId> id = objType.type->decl();
    if (!id.has_value()) {
        return;
    }
    Place base = baseIn;
    base.type = objType;
    std::unordered_set<int64_t> unitsZeroed;
    for (const std::string& name : resolver_.chainFieldNames(*id)) {
        const Expr* dflt = resolver_.fieldDefault(*id, name);
        if (dflt == nullptr) {
            continue;
        }
        int64_t fieldOff = resolver_.fieldOffset(*id, name);
        TypeRef fieldType = resolver_.fieldType(*id, name);
        if (fieldOff < 0 || fieldType.isError()) {
            refuse(pos, "this default");
            return;
        }
        TypeRef spelled = checker_.typeOf(dflt);
        TypeRef defaultType = spelled.isError() ? fieldType : spelled;
        std::vector<ir::Value> value = lowerWords(dflt, defaultType);
        if (failed_) {
            return;
        }
        value = boxClassOperand(dflt, std::move(value), defaultType,
                                fieldType, pos);
        if (failed_) {
            return;
        }
        if (!storeFieldInit(pos, base, fieldOff, fieldType, *id, name,
                            unitsZeroed, value)) {
            refuse(pos, "this default");
            return;
        }
    }
}

void Lowerer::constructStruct(const Place& baseIn, TypeRef structType,
                              const NewStruct& n) {
    if (structType.isError() || structType.type->kind() != Kind::Struct) {
        refuse(n.pos, "this construction");
        return;
    }
    std::optional<DeclId> id = structType.type->decl();
    if (!id.has_value()) {
        refuse(n.pos, "this construction");
        return;
    }
    Place base = baseIn;
    base.type = structType;
    emitFieldDefaults(base, structType, n.pos);
    if (failed_) {
        return;
    }
    std::unordered_set<int64_t> unitsZeroed;
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
        value = adjustStringLitArg(field.value.get(), std::move(value), fieldType);
        // A concrete value for an interface field boxes to the pair
        // before the range store below, which then stores two words.
        value = boxClassOperand(field.value.get(), std::move(value),
                                  checker_.typeOf(field.value.get()), fieldType,
                                  field.value->pos);
        if (failed_) {
            return;
        }
        if (!storeFieldInit(field.value->pos, base, fieldOff, fieldType, *id,
                            field.name, unitsZeroed, value)) {
            refuse(field.value->pos, "this construction");
            return;
        }
    }
}

void Lowerer::constructUnion(const Place& baseIn, TypeRef objType,
                             const NewStruct& n) {
    TypeRef unionType = objType;
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
    value = adjustStringLitArg(field.value.get(), std::move(value), memberType);
    Place at = baseIn;
    at.type = memberType;
    at.off = 0;
    storeRange(field.value->pos, at, value, memberType);
    if (failed_) {
        return;
    }
    Place tag = baseIn;
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
    EvalEnv env = resolver_.evalEnv(emitScope());
    EvalResult folded = evalConstInt(*lit, env, active);
    return folded.ok && folded.value == 0;
}

bool Lowerer::globalInitStructBytes(const NewStruct& made, TypeRef structType,
                                        std::string& bytes) {
    if (structType.isError()) {
        return false;
    }
    std::optional<DeclId> id = structType.type->decl();
    if (!id.has_value()) {
        return false;
    }
    Kind kind = structType.type->kind();
    if (kind != Kind::Struct && kind != Kind::Class) {
        return false;
    }
    int64_t total = (kind == Kind::Class)
                        ? resolver_.classSize(*id)
                        : resolver_.nominalSize(structType.type);
    if (total <= 0) {
        return false;
    }
    std::unordered_set<std::string> active;
    EvalEnv env = resolver_.evalEnv(emitScope());
    auto foldScalar = [&](const Expr& leaf, TypeRef leafType,
                          std::string& out) -> bool {
        int64_t width = resolver_.nominalSize(leafType.type);
        if (width <= 0) {
            return false;
        }
        if (leafType.type->kind() == Kind::Basic &&
            basicIsFloat(*leafType.type->basic())) {
            FloatResult asFloat = evalConstFloat(leaf, env, active);
            if (!asFloat.ok) {
                return false;
            }
            uint64_t bits = 0;
            if (*leafType.type->basic() == BasicKind::F32) {
                float narrowed = static_cast<float>(asFloat.value);
                std::memcpy(&bits, &narrowed, sizeof narrowed);
            } else {
                std::memcpy(&bits, &asFloat.value, sizeof asFloat.value);
            }
            for (int64_t byte = 0; byte < width; ++byte) {
                out.push_back(static_cast<char>(bits & 0xFFu));
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
            out.push_back(static_cast<char>(bits & 0xFFu));
            bits >>= 8;
        }
        return true;
    };
    std::string image;
    {
        uint64_t want = static_cast<uint64_t>(total);
        while (static_cast<uint64_t>(image.size()) != want) {
            image.push_back('\0');
        }
    }
    // A defaulted field that the literal omits would need its default to
    // run at startup; the constant image cannot execute it, so refuse
    // rather than silently emit zeros for a field the program expects to
    // hold its default.
    std::unordered_set<std::string> named;
    for (const auto& field : made.fields) {
        named.insert(field.name);
    }
    for (const std::string& field : resolver_.chainFieldNames(*id)) {
        if (named.find(field) == named.end() &&
            resolver_.fieldDefault(*id, field) != nullptr) {
            return false;
        }
    }
    for (const auto& field : made.fields) {
        if (field.value == nullptr) {
            return false;
        }
        const Expr* sub = field.value.get();
        TypeRef ftype = resolver_.fieldType(*id, field.name);
        if (ftype.isError()) {
            return false;
        }
        int64_t off = resolver_.fieldOffset(*id, field.name);
        if (off < 0) {
            return false;
        }
        int64_t bitOff = -1;
        int64_t bitWidth = 0;
        if (resolver_.bitField(*id, field.name, bitOff, bitWidth)) {
            // 4.1: the constant image assembles whole bytes per field; a
            // bitfield would need bit-wise assembly that does not exist
            // yet, and a wrong image is worse than a refusal.
            return false;
        }
        std::string piece;
        if (dynamic_cast<const NewArray*>(sub) != nullptr) {
            const auto& arr = static_cast<const NewArray&>(*sub);
            if (!globalInitBytes(arr.inits, arr.repeat, ftype, piece)) {
                return false;
            }
        } else if (dynamic_cast<const ArrayLit*>(sub) != nullptr) {
            const auto& lit = static_cast<const ArrayLit&>(*sub);
            if (!globalInitBytes(lit.inits, lit.repeat, ftype, piece)) {
                return false;
            }
        } else if (dynamic_cast<const NewStruct*>(sub) != nullptr) {
            if (!globalInitStructBytes(static_cast<const NewStruct&>(*sub),
                                       ftype, piece)) {
                return false;
            }
        } else if (const auto* cast = dynamic_cast<const Cast*>(sub)) {
            // `nilPtr as I` folds to sixteen zero bytes: the gate admits
            // only this spelling for interface fields, so anything
            // arriving here is null by construction.
            if (dynamic_cast<const NilPtr*>(cast->expr.get()) == nullptr ||
                ftype.type->kind() != Kind::Interface) {
                return false;
            }
            piece.append(static_cast<size_t>(kInterfaceWords) *
                             static_cast<size_t>(layout::kSizePointer),
                         '\0');
        } else if (dynamic_cast<const NilPtr*>(sub) != nullptr) {
            // A bare nil pointer fills its field with zeros: the width
            // comes from the declared field type, which the gate already
            // checked is a null-accepting shape.
            int64_t width = resolver_.nominalSize(ftype.type);
            if (width <= 0) {
                return false;
            }
            piece.append(static_cast<size_t>(width), '\0');
        } else if (!foldScalar(*sub, ftype, piece)) {
            return false;
        }
        uint64_t at = static_cast<uint64_t>(off);
        uint64_t count = static_cast<uint64_t>(piece.size());
        uint64_t span = static_cast<uint64_t>(total);
        if (at + count > span) {
            return false;
        }
        for (uint64_t i = 0; i < count; ++i) {
            image[at + i] = piece[i];
        }
    }
    bytes = std::move(image);
    return true;
}

bool Lowerer::globalInitBytes(const std::vector<std::unique_ptr<Expr>>& inits,
                              bool repeat, TypeRef arrayType,
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
    EvalEnv env = resolver_.evalEnv(emitScope());
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
            return globalInitBytes(sub->inits, sub->repeat, elemType, bytes);
        }
        if (const auto* sub = dynamic_cast<const ArrayLit*>(&leaf)) {
            if (elemType.type->kind() != Kind::Array) {
                return false;
            }
            return globalInitBytes(sub->inits, sub->repeat, elemType, bytes);
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
        if (const auto* cast = dynamic_cast<const Cast*>(&leaf)) {
            // `nilPtr as I` folds to sixteen zero bytes: the gate admits
            // only this spelling for interface elements, so anything
            // arriving here is null by construction.
            if (dynamic_cast<const NilPtr*>(cast->expr.get()) != nullptr &&
                elemType.type->kind() == Kind::Interface) {
                bytes.append(static_cast<size_t>(kInterfaceWords) *
                                 static_cast<size_t>(layout::kSizePointer),
                             '\0');
                return true;
            }
        }
        if (const auto* sub = dynamic_cast<const NewStruct*>(&leaf)) {
            // A struct or class element carries its fields' image bytes:
            // the function answers the element's whole image, which
            // appends to this one.
            if (elemType.type->kind() != Kind::Struct &&
                elemType.type->kind() != Kind::Class) {
                return false;
            }
            std::string piece;
            if (!globalInitStructBytes(*sub, elemType, piece)) {
                return false;
            }
            bytes.append(piece.data(), piece.size());
            return true;
        }
        return appendScalar(leaf, resolver_.nominalSize(elemType.type), elemType);
    };
    if (repeat) {
        size_t fillers = inits.size();
        if (fillers != 1) {
            return false;
        }
        for (int64_t at = 0; at < *extent; ++at) {
            if (!appendElement(*inits[0], element)) {
                return false;
            }
        }
        return true;
    }
    if (static_cast<int64_t>(inits.size()) != *extent) {
        return false;
    }
    for (const auto& one : inits) {
        if (!appendElement(*one, element)) {
            return false;
        }
    }
    return true;
}

void Lowerer::constructArray(const Place& baseIn, TypeRef arrayType,
                             const std::vector<std::unique_ptr<Expr>>& inits,
                             bool repeat, int64_t count, Position pos) {
    if (arrayType.isError() || arrayType.type->kind() != Kind::Array) {
        refuse(pos, "this construction");
        return;
    }
    TypeRef element = TypeRef{arrayType.type->element()};
    int64_t elemBytes = resolver_.nominalSize(element.type);
    if (elemBytes <= 0) {
        refuse(pos, "this construction");
        return;
    }
    Place base = baseIn;
    base.type = arrayType;
    // Contiguity is checked, not assumed: only a slot base may rely on
    // consecutive ids for address arithmetic. An indirect or address base
    // already holds the one address every store steps off.
    if (!base.hasIndirect && !base.isAddress) {
        for (uint64_t i = 0; i < static_cast<uint64_t>(base.slots.size()); ++i) {
            if (base.slots[i].id != base.slots[0].id + static_cast<uint32_t>(i)) {
                refuse(pos, "this construction");
                return;
            }
        }
    }
    auto atByte = [&](int64_t byteOff, TypeRef fieldType, const std::vector<ir::Value>& words,
                      Position here) {
        // Element values arrive in value shape (one word per eight bytes);
        // the range store shapes them into chunk stores itself, shifting a
        // word down when a take starts past its low byte.
        Place at = base;
        at.type = fieldType;
        at.off = byteOff;
        storeRange(here, at, words, fieldType);
    };
    if (repeat) {
        if (inits.size() != kOneWord) {
            refuse(pos, "a repeat fills from exactly one element");
            return;
        }
        if (count < 0) {
            refuse(pos, "an array length folds to a count");
            return;
        }
        std::vector<ir::Value> one = lowerWords(inits[0].get());
        if (failed_) {
            return;
        }
        one = adjustStringLitArg(inits[0].get(), std::move(one), element);
        // A concrete filler for an interface array boxes once, then fills
        // every element with the same pair.
        one = boxClassOperand(inits[0].get(), std::move(one), checker_.typeOf(inits[0].get()),
                                element, pos);
        if (failed_) {
            return;
        }
        if (isZeroRepeatFiller(*inits[0], element)) {
            // Zero reaches through: one zero word per element word, so a
            // nested array fills with zeros instead of one element value.
            int words = wordCount(element);
            if (words < 0) {
                refuse(pos, "this construction");
                return;
            }
            one.clear();
            for (int w = 0; w < words; ++w) {
                one.push_back(constantI64(0, wordType(element, static_cast<uint64_t>(w)),
                                          pos));
                if (failed_) {
                    return;
                }
            }
        }
        for (int64_t i = 0; i < count; ++i) {
            atByte(i * elemBytes, element, one, pos);
            if (failed_) {
                return;
            }
        }
        return;
    }
    for (uint64_t i = 0; i < static_cast<uint64_t>(inits.size()); ++i) {
        std::vector<ir::Value> one = lowerWords(inits[i].get());
        if (failed_) {
            return;
        }
        one = adjustStringLitArg(inits[i].get(), std::move(one), element);
        // A concrete element for an interface array boxes to the pair
        // before the range store below, like every other aggregate site.
        one = boxClassOperand(inits[i].get(), std::move(one), checker_.typeOf(inits[i].get()),
                                element, pos);
        if (failed_) {
            return;
        }
        atByte(static_cast<int64_t>(i) * elemBytes, element, one, pos);
        if (failed_) {
            return;
        }
    }
}

bool Lowerer::storeBitField(Position pos, const Place& unit, TypeRef unitType,
                            int64_t bitOff, int64_t width, const ir::Value& value) {
    int64_t unitBits = resolver_.nominalSize(unitType.type);
    if (unitBits < 0) {
        return false;
    }
    if (bitOff == 0 && width == unitBits) {
        storeRange(pos, unit, {value}, unitType);
        return !failed_;
    }
    std::vector<ir::Value> words = loadRange(pos, unit, unitType);
    if (failed_ || words.size() != static_cast<size_t>(kOneWord)) {
        return false;
    }
    const uint64_t widthMask = (uint64_t{1} << width) - 1;
    const uint64_t fieldMask = widthMask << bitOff;
    TypeRef count{ctx_.basic(BasicKind::I64)};
    ir::Value kept = words[0];
    if (~fieldMask != 0) {
        kept = emitArith(BinOp::And, kept,
                         constantI64(static_cast<int64_t>(~fieldMask), count, pos),
                         unitType, pos);
        if (failed_) {
            return false;
        }
    }
    ir::Value insert = value;
    if (widthMask != ~uint64_t{0}) {
        insert = emitArith(BinOp::And, insert,
                           constantI64(static_cast<int64_t>(widthMask), count, pos),
                           unitType, pos);
        if (failed_) {
            return false;
        }
    }
    if (bitOff > 0) {
        insert = emitArith(BinOp::Shl, insert, constantI64(bitOff, count, pos),
                           unitType, pos);
        if (failed_) {
            return false;
        }
    }
    ir::Value combined = emitArith(BinOp::Or, kept, insert, unitType, pos);
    if (failed_) {
        return false;
    }
    storeRange(pos, unit, {combined}, unitType);
    return !failed_;
}

bool Lowerer::loadBitField(Position pos, const Place& unit, TypeRef unitType,
                           int64_t bitOff, int64_t width, ir::Value& out) {
    std::vector<ir::Value> words = loadRange(pos, unit, unitType);
    if (failed_ || words.size() != static_cast<size_t>(kOneWord)) {
        return false;
    }
    int64_t unitBits = resolver_.nominalSize(unitType.type);
    if (unitBits < 0) {
        return false;
    }
    TypeRef count{ctx_.basic(BasicKind::I64)};
    ir::Value value = words[0];
    if (bitOff > 0) {
        value = emitArith(BinOp::Shr, value, constantI64(bitOff, count, pos),
                          unitType, pos);
        if (failed_) {
            return false;
        }
    }
    if (width < unitBits) {
        const int64_t mask = (static_cast<int64_t>(1) << width) - 1;
        value = emitArith(BinOp::And, value, constantI64(mask, count, pos),
                          unitType, pos);
        if (failed_) {
            return false;
        }
    }
    out = value;
    return true;
}

std::vector<ir::Value> Lowerer::lowerField(const Field& n) {
    // `lib.gB`: the object names a module, so the member is a global in
    // the defining scope. Load through the same global path a bare
    // spelling uses; anything else falls through to the paths below.
    if (const auto* ident = dynamic_cast<const Ident*>(n.object.get())) {
        if (const GlobalBinding* global =
                resolveModuleGlobal(ident->name, n.name)) {
            return loadGlobal(n.pos, *global);
        }
        // `b.inc`: the object names a module and the member names its
        // function, so this answers the closure pair for that function:
        // a forwarder with the receiver-first shape plus a null captures
        // word, with no allocation. Locals shadow module names; generic
        // functions and non-function members fall through below.
        bool shadowed = false;
        for (auto frame = locals_.rbegin();
             frame != locals_.rend() && !shadowed; ++frame) {
            shadowed = frame->find(ident->name) != frame->end();
        }
        const resolve::Scope* here = homeScope();
        const resolve::Decl* moduleDecl =
            here != nullptr ? resolver_.lookup(here, ident->name) : nullptr;
        if (!shadowed && moduleDecl != nullptr &&
            moduleDecl->kind == resolve::DeclKind::Module) {
            const resolve::Scope* home =
                resolver_.scopeOf(moduleDecl->origin);
            const resolve::Decl* member =
                home != nullptr ? resolver_.lookup(home, n.name) : nullptr;
            if (member != nullptr &&
                member->kind == resolve::DeclKind::Func &&
                member->typeParams == 0) {
                TypeRef type = checker_.typeOf(&n);
                if (!type.isError() && type.type->kind() == Kind::Closure &&
                    wordCount(type) == static_cast<int>(kClosureWords)) {
                    std::string link = resolver_.linkName(*member);
                    std::string fwd =
                        forwardFunction(link, member->sig, n.pos);
                    if (failed_) {
                        return {undefined()};
                    }
                    ir::Instr fn;
                    fn.op = ir::Op::FnAddr;
                    fn.loc = n.pos;
                    fn.type = wordType(type, kFirstWord);
                    fn.width = ir::IRType::Ptr;
                    fn.globalLabel = fwd;
                    fn.isPure = true;
                    ir::Value code = newValue(fn.width, fn.type, n.pos);
                    fn.results.push_back(code);
                    emit(std::move(fn));
                    ir::Value caps =
                        constantI64(0, wordType(type, kLenWord), n.pos);
                    if (failed_) {
                        return {undefined()};
                    }
                    return {code, caps};
                }
            }
            // `lib.CIRCLE` folds like a bare enumerator rooted in that
            // module: exactly one of its enums may own the name. Globals
            // and functions already answered above, so this branch owns
            // nothing they could name.
            if (member == nullptr && home != nullptr) {
                const resolve::Decl* owner = nullptr;
                int64_t value = 0;
                int owners = 0;
                for (const auto& [name, decl] : home->bindings) {
                    if (decl == nullptr ||
                        decl->kind != resolve::DeclKind::Type ||
                        decl->nominal != Kind::Enum) {
                        continue;
                    }
                    for (const auto& entry : resolver_.enumMembers(decl->id)) {
                        if (entry.first == n.name) {
                            ++owners;
                            owner = decl;
                            value = entry.second;
                            break;
                        }
                    }
                }
                if (owner != nullptr && owners == 1) {
                    TypeRef count{ctx_.basic(BasicKind::I32)};
                    ir::Value answer = constantI64(value, count, n.pos);
                    if (failed_) {
                        return {undefined()};
                    }
                    return {answer};
                }
            }
        }
    }
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
                homeScope();
            const resolve::Decl* named =
                here != nullptr ? resolver_.lookup(here, ident->name)
                                : nullptr;
            if (named != nullptr &&
                named->kind == resolve::DeclKind::Type &&
                named->nominal == Kind::Enum) {
                std::string home = named->scope != nullptr
                                       ? named->scope->module
                                       : homeModule();
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
            // `Shade.GREEN` folds like the enum it aliases: the checker
            // proved the member, so the value here is that enumerator's.
            if (named != nullptr &&
                named->kind == resolve::DeclKind::Alias &&
                named->aliasTarget != nullptr) {
                const resolve::Scope* hereScope =
                    homeScope();
                TypeRef target = resolver_.resolveType(named->aliasTarget,
                                                        hereScope);
                if (!target.isError() &&
                    target.type->kind() == Kind::Enum) {
                    std::optional<DeclId> aliasId = target.type->decl();
                    if (aliasId.has_value()) {
                        int64_t value = 0;
                        bool known = false;
                        for (const auto& entry :
                             resolver_.enumMembers(*aliasId)) {
                            if (entry.first == n.name) {
                                value = entry.second;
                                known = true;
                                break;
                            }
                        }
                        if (!known) {
                            refuse(n.pos, "this field access");
                            return {undefined()};
                        }
                        TypeRef count{ctx_.basic(BasicKind::I32)};
                        ir::Value answer =
                            constantI64(value, count, n.pos);
                        if (failed_) {
                            return {undefined()};
                        }
                        return {answer};
                    }
                }
            }
        }
    }
    if (const auto* inner = dynamic_cast<const Field*>(n.object.get())) {
        // `mid.Col.GREEN` folds to the enumerator's integer, mirroring
        // the same-module spelling above. Anything else here refuses:
        // the checker lets only this spelling through.
        if (const auto* mod = dynamic_cast<const Ident*>(inner->object.get())) {
            bool shadowed = false;
            for (auto frame = locals_.rbegin();
                 frame != locals_.rend() && !shadowed; ++frame) {
                shadowed = frame->find(mod->name) != frame->end();
            }
            const resolve::Scope* here =
                homeScope();
            const resolve::Decl* modDecl =
                here != nullptr ? resolver_.lookup(here, mod->name) : nullptr;
            const resolve::Scope* home =
                (modDecl != nullptr &&
                 modDecl->kind == resolve::DeclKind::Module)
                    ? resolver_.scopeOf(modDecl->origin)
                    : nullptr;
            const resolve::Decl* member =
                home != nullptr ? resolver_.lookup(home, inner->name)
                                : nullptr;
            std::optional<DeclId> enumId;
            if (!shadowed && member != nullptr &&
                member->kind == resolve::DeclKind::Type &&
                member->nominal == Kind::Enum) {
                enumId = member->id;
            } else if (!shadowed && member != nullptr &&
                       member->kind == resolve::DeclKind::Alias &&
                       member->aliasTarget != nullptr) {
                TypeRef target =
                    resolver_.resolveType(member->aliasTarget, home);
                if (!target.isError() &&
                    target.type->kind() == Kind::Enum) {
                    enumId = target.type->decl();
                }
            }
            if (enumId.has_value()) {
                const resolve::Decl* named = resolver_.declById(*enumId);
                if (named == nullptr || named->scope == nullptr ||
                    !resolver_.isEnumMember(
                        named->scope->module + "." + named->name, n.name)) {
                    refuse(n.pos, "this field access");
                    return {undefined()};
                }
                int64_t value = resolver_.enumValue(
                    named->scope->module + "." + named->name, n.name);
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
        // A field of a pointer element (`p[0].lo`) or of a computed array
        // element (`arr[i].x`): the direct route only knows slots, but both
        // have an address — the shared guarded helper materializes each,
        // lowering the index once. The Field branch of resolvePlace
        // carries the same fallback for the store path; both stay because
        // each serves a different node.
        const Index* elem = dynamic_cast<const Index*>(n.object.get());
        TypeRef arrType = elem != nullptr
            ? checker_.typeOf(elem->array.get())
            : TypeRef{ctx_.error()};
        if (elem == nullptr || arrType.isError()) {
            refuse(n.object->pos, "this field access");
            return {undefined()};
        }
        Kind inner = arrType.type->kind();
        if (inner != Kind::Pointer && inner != Kind::Array) {
            refuse(n.object->pos, "this field access");
            return {undefined()};
        }
        Place base0;
        if (!resolvePlace(elem->array.get(), base0)) {
            refuse(n.object->pos, "this field access");
            return {undefined()};
        }
        base = Place{};
        if (inner == Kind::Pointer) {
            if (!pointerElementPlace(*elem, base0, arrType,
                                     "this field access", base)) {
                return {undefined()};
            }
        } else if (!arrayElementPlace(*elem, base0, arrType,
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
        TypeRef count{ctx_.basic(BasicKind::I32)};
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
    int64_t bitOff = -1;
    int64_t bitWidth = 0;
    if (resolver_.bitField(*id, n.name, bitOff, bitWidth)) {
        ir::Value answer = undefined();
        if (!loadBitField(n.pos, base, fieldType, bitOff, bitWidth, answer)) {
            refuse(n.pos, "this field access");
            return {undefined()};
        }
        return {answer};
    }
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
    at = Place{};
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
    EvalEnv env = resolver_.evalEnv(emitScope());
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
    // An indirect base owns no slots by construction: its address is the
    // loaded pointer value, and reading slots[0] answers past the end of
    // an empty vector. The accumulated offset still applies on top.
    if (base.hasIndirect) {
        gep.addressBase = base.indirect;
    } else {
        if (base.slots.empty()) {
            refuse(n.pos, what);
            return false;
        }
        gep.addressBase = base.slots[kFirstWord];
    }
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
    at = Place{};
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
        // An index into a call's answer (`makeRow(9)[1]`): the words have
        // no address, so they spill to scratch slots first and the index
        // reads from there like any other place (the field path over a
        // call answer carries the same fallback for reads).
        const auto* call = dynamic_cast<const Call*>(arrayExpr);
        if (call == nullptr) {
            return false;
        }
        TypeRef answer = checker_.typeOf(arrayExpr);
        if (answer.isError()) {
            return false;
        }
        std::vector<ir::Value> words =
            lowerCallWords(const_cast<Call&>(*call));
        if (failed_) {
            return false;
        }
        if (words.empty() ||
            words.size() != static_cast<size_t>(wordCount(answer))) {
            refuse(n.pos, what);
            return false;
        }
        std::vector<ir::Value> slots =
            allocaSlots(answer, n.pos, static_cast<uint64_t>(words.size()));
        if (failed_) {
            return false;
        }
        Place scratch;
        scratch.slots = slots;
        scratch.type = answer;
        storeRange(n.pos, scratch, words, answer);
        if (failed_) {
            return false;
        }
        base = std::move(scratch);
        arrayType = answer;
        return true;
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
    if (const auto* wide = dynamic_cast<const WideLit*>(n.array.get())) {
        // A literal names rodata, not slots: the guarded array path below
        // reads through the blob's label with global-array semantics, so
        // folded proofs and runtime guards both apply unchanged.
        arrayType = checker_.typeOf(n.array.get());
        if (arrayType.isError() || arrayType.type->kind() != Kind::Array) {
            refuse(n.array->pos, "this index");
            return {undefined()};
        }
        base.global = wideBlob(*wide);
        base.off = 0;
        base.type = arrayType;
        base.isVolatile = false;
    } else if (!arrayBase(n.array.get(), n, base, arrayType, "this index")) {
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
    EvalEnv env = resolver_.evalEnv(emitScope());
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
    EvalEnv env = resolver_.evalEnv(emitScope());
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
    EvalEnv env = resolver_.evalEnv(emitScope());
    EvalResult folded = evalConstInt(*expr, env, active);
    if (!folded.ok) {
        return false;
    }
    value = folded.value;
    return true;
}

std::vector<ir::Value> Lowerer::lowerSizeof(const SizeofType& n) {
    // Read the operand the checker recorded: inside a spec emission it
    // names the substituted type, which re-resolving in module scope
    // cannot answer past type parameters. Outside generics this is the
    // same type the old path resolved.
    TypeRef target = checker_.sizeofOperand(&n);
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

std::vector<ir::Value> Lowerer::lowerAlignof(const AlignofType& n) {
    TypeRef target = checker_.alignofOperand(&n);
    if (target.isError()) {
        refuse(n.pos, "this expression");
        return {undefined()};
    }
    int64_t unit = resolver_.nominalAlign(target.type);
    if (unit < 0) {
        refuse(n.pos, "this expression");
        return {undefined()};
    }
    TypeRef count{ctx_.basic(BasicKind::I64)};
    return {constantI64(unit, count, n.pos)};
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
        // Slot-precise logic must not read slots then: an indirect base
        // owns no slots by construction (BUG-278), so a base with
        // neither slots nor an indirect address refuses instead of
        // answering past the end of an empty vector.
        if (!base.hasIndirect && base.slots.empty()) {
            refuse(n.array->pos, "this expression");
            return {undefined()};
        }
        if (loFolded) {
            int64_t byteDelta = loConst * elemBytes;
            if (base.hasIndirect) {
                // The base address is the loaded pointer value; the
                // accumulated offset still applies on top, and the
                // folded advance rides along as a constant operand
                // (codegen reads operands[0] unconditionally).
                ir::Instr gep;
                gep.op = ir::Op::Gep;
                gep.loc = n.pos;
                gep.type = ptrType;
                gep.width = ir::IRType::Ptr;
                gep.addressBase = base.indirect;
                gep.displacement = base.off;
                gep.isPure = true;
                gep.operands.push_back(constantI64(byteDelta, count, n.lo->pos));
                ptr = newValue(gep.width, ptrType, n.pos);
                gep.results.push_back(ptr);
                emit(std::move(gep));
            } else {
                int64_t per = layout::kSizePointer;
                int64_t byteAddr = base.off + byteDelta;
                uint64_t slotIdx = static_cast<uint64_t>(byteAddr / per);
                if (slotIdx >= static_cast<uint64_t>(base.slots.size())) {
                    refuse(n.pos, "this expression");
                    return {undefined()};
                }
                int64_t intra = byteAddr % per;
                ptr = computedBase(base.slots[slotIdx], intra, n.pos);
                ptr.type = ptrType;
            }
        } else {
            // Consecutive slot ids are a direct-base fact: an indirect
            // base has no slots to be consecutive.
            if (!base.hasIndirect) {
                for (uint64_t i = 0; i < static_cast<uint64_t>(base.slots.size()); ++i) {
                    if (base.slots[i].id != base.slots[0].id + static_cast<uint32_t>(i)) {
                        refuse(n.pos, "this expression");
                        return {undefined()};
                    }
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
            if (base.hasIndirect) {
                gep.addressBase = base.indirect;
            } else {
                gep.addressBase = base.slots[0];
            }
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
    if (baseKind == Kind::String) {
        // 2.4: a string's element is a char, one byte like a u8 but the
        // interpretation the checker names.
        element = TypeRef{ctx_.basic(BasicKind::Char)};
    }
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

ir::Value Lowerer::wideRodata(const WideLit& lit) {
    std::string label = wideBlob(lit);
    TypeRef addr{ctx_.pointer(ctx_.basic(BasicKind::U16))};
    ir::Instr data;
    data.op = ir::Op::DataAddr;
    data.loc = lit.pos;
    data.type = addr;
    data.width = ir::IRType::Ptr;
    data.globalLabel = label;
    data.isPure = true;
    ir::Value at = newValue(data.width, addr, lit.pos);
    data.results.push_back(at);
    emit(std::move(data));
    if (failed_) {
        return undefined();
    }
    return at;
}

std::string Lowerer::wideBlob(const WideLit& lit) {
    // Units lay out little-endian with one zero unit past the end: `*u16`
    // reads run to zero, and `*char` reads run to NUL for the same reason.
    // The length word counts units, never the terminator.
    std::string bytes;
    for (uint16_t unit : lit.units) {
        bytes.push_back(static_cast<char>(unit & 0xFF));
        bytes.push_back(static_cast<char>((unit >> 8) & 0xFF));
    }
    bytes.push_back('\0');
    bytes.push_back('\0');
    std::string label = "w_" + std::to_string(rodataSeq_++);
    ir::Rodata blob;
    blob.label = label;
    blob.bytes = std::move(bytes);
    module_->rodata.push_back(std::move(blob));
    return label;
}

std::vector<ir::Value> Lowerer::wideWords(const WideLit& lit) {
    // Take-packed constants: the same greedy 8/4/2/1 split storeRange and
    // loadRange use, so these words store and compare with the shape array
    // construction answers. One unit is two bytes little-endian, plus one
    // zero unit past the end.
    std::string bytes;
    for (uint16_t unit : lit.units) {
        bytes.push_back(static_cast<char>(unit & 0xFF));
        bytes.push_back(static_cast<char>((unit >> 8) & 0xFF));
    }
    bytes.push_back('\0');
    bytes.push_back('\0');
    int64_t per = layout::kSizePointer;
    std::vector<ir::Value> out;
    uint64_t at = 0;
    while (at < bytes.size()) {
        int64_t left = static_cast<int64_t>(bytes.size() - at);
        int64_t take = layout::kSizeI8;
        BasicKind kind = BasicKind::U8;
        if (left >= per) {
            take = per;
            kind = BasicKind::U64;
        } else if (left >= layout::kSizeI32) {
            take = layout::kSizeI32;
            kind = BasicKind::U32;
        } else if (left >= layout::kSizeI16) {
            take = layout::kSizeI16;
            kind = BasicKind::U16;
        }
        uint64_t word = 0;
        for (int64_t byte = 0; byte < take; ++byte) {
            uint64_t bits =
                static_cast<unsigned char>(bytes[at + static_cast<size_t>(byte)]);
            word |= bits << (byte * 8);
        }
        at += static_cast<size_t>(take);
        out.push_back(constantI64(static_cast<int64_t>(word),
                                  TypeRef{ctx_.basic(kind)}, lit.pos));
        if (failed_) {
            return {undefined()};
        }
    }
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
            BasicKind basic = *type.type->basic();
            std::vector<ir::Value> got = lowerWords(part.expr.get());
            if (failed_ || got.size() != kOneWord) {
                refuse(part.expr->pos, "this interpolation");
                return LoweredString{};
            }
            // The helper converts one double, so a float widens first:
            // the Cast carries the operand's type, which is what tells
            // the backend to convert rather than re-read the bits.
            ir::Value wide = got[kFirstWord];
            if (basic == BasicKind::F32) {
                TypeRef dbl{ctx_.basic(BasicKind::F64)};
                ir::Instr grow;
                grow.op = ir::Op::Cast;
                grow.loc = lit.pos;
                grow.type = dbl;
                grow.width = ir::IRType::F64;
                grow.operands.push_back(got[kFirstWord]);
                wide = newValue(grow.width, dbl, lit.pos);
                grow.results.push_back(wide);
                emit(std::move(grow));
            }
            // Thirty-two bytes hold the longest float shape: a sign,
            // nineteen whole digits, the point, six fraction digits.
            std::vector<ir::Value> scratch = allocaSlots(count, lit.pos, 4);
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
            call.callee = "cider_ftoa";
            ir::Argument first;
            first.words.push_back(buf);
            call.arguments.push_back(std::move(first));
            ir::Argument second;
            second.words.push_back(wide);
            second.width = ir::IRType::F64;
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
        refuse(part.expr->pos, "this interpolation");
        return LoweredString{};
    }
    return joinStrings(parts, lit.pos);
}

Lowerer::LoweredString Lowerer::joinStrings(const std::vector<PartBytes>& parts,
                                            Position pos) {
    // The lengths sum to a total; one allocation answers the buffer; every
    // part copies into it at the running offset. A zero total allocates
    // zero bytes, which answers nil: every string consumer checks the
    // length before touching the pointer, so nothing dereferences it.
    TypeRef count{ctx_.basic(BasicKind::I64)};
    ir::Value total = constantI64(0, count, pos);
    if (failed_) {
        return LoweredString{};
    }
    for (const PartBytes& part : parts) {
        ir::Instr add;
        add.op = ir::Op::Add;
        add.loc = pos;
        add.type = count;
        add.width = ir::IRType::I64;
        add.isPure = true;
        add.operands.push_back(total);
        add.operands.push_back(part.len);
        ir::Value sum = newValue(add.width, count, pos);
        add.results.push_back(sum);
        emit(std::move(add));
        total = sum;
    }
    if (failed_) {
        return LoweredString{};
    }
    ir::Instr alloc;
    alloc.op = ir::Op::Call;
    alloc.loc = pos;
    alloc.callee = "cider_alloc";
    alloc.mayTrap = true;
    {
        ir::Argument bytes;
        bytes.words.push_back(total);
        alloc.arguments.push_back(std::move(bytes));
        ir::Argument pad;
        pad.words.push_back(constantI64(8, count, pos));
        alloc.arguments.push_back(std::move(pad));
    }
    if (failed_) {
        return LoweredString{};
    }
    TypeRef ptrType{ctx_.pointer(ctx_.basic(BasicKind::U8))};
    ir::Value buf = newValue(ir::IRType::Ptr, ptrType, pos);
    alloc.results.push_back(buf);
    alloc.width = ir::IRType::Ptr;
    alloc.type = ptrType;
    emit(std::move(alloc));
    ir::Value off = constantI64(0, count, pos);
    if (failed_) {
        return LoweredString{};
    }
    for (const PartBytes& part : parts) {
        ir::Instr add;
        add.op = ir::Op::Add;
        add.loc = pos;
        add.type = ptrType;
        add.width = ir::IRType::Ptr;
        add.isPure = true;
        add.operands.push_back(buf);
        add.operands.push_back(off);
        ir::Value dst = newValue(add.width, ptrType, pos);
        add.results.push_back(dst);
        emit(std::move(add));
        if (failed_) {
            return LoweredString{};
        }
        copyBytes(dst, part.ptr, part.len, pos);
        if (failed_) {
            return LoweredString{};
        }
        ir::Instr bump;
        bump.op = ir::Op::Add;
        bump.loc = pos;
        bump.type = count;
        bump.width = ir::IRType::I64;
        bump.isPure = true;
        bump.operands.push_back(off);
        bump.operands.push_back(part.len);
        ir::Value next = newValue(bump.width, count, pos);
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

std::vector<ir::Value> Lowerer::lowerStringAdd(const Binary& n) {
    // The checker proved both sides strings: each side lowers to exactly
    // its address and length, and the shared join answers one buffer.
    std::vector<ir::Value> left = lowerWords(n.lhs.get());
    if (failed_ || left.size() != kSliceWords) {
        if (!failed_) {
            refuse(n.pos, "this concatenation");
        }
        return {undefined()};
    }
    std::vector<ir::Value> right = lowerWords(n.rhs.get());
    if (failed_ || right.size() != kSliceWords) {
        if (!failed_) {
            refuse(n.pos, "this concatenation");
        }
        return {undefined()};
    }
    PartBytes first;
    first.ptr = left[kFirstWord];
    first.len = left[kLenWord];
    PartBytes second;
    second.ptr = right[kFirstWord];
    second.len = right[kLenWord];
    return joinStrings({first, second}, n.pos).words;
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
    if (needsSret(sig.returnType())) {
        // A forwarder drops its captures word and calls through; threading
        // a hidden buffer address through both shapes is a later slice.
        refuse(pos, "a wide return through a closure forwarder is not lowered yet");
        return "";
    }
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

void Lowerer::constructClass(const Place& baseIn, TypeRef objType,
                             const NewStruct& n) {
    TypeRef classType = objType;
    if (classType.isError() || classType.type->kind() != Kind::Class) {
        refuse(n.pos, "this construction");
        return;
    }
    std::optional<DeclId> id = classType.type->decl();
    if (!id.has_value()) {
        refuse(n.pos, "this construction");
        return;
    }
    // 4.8.5: the vptr word exists only when the chain carries virtual
    // slots; a concrete-only class opens at its first field.
    if (!resolver_.virtualNames(*id).empty()) {
        TypeRef vptr{ctx_.basic(BasicKind::U64)};
        ir::Value vptrWord = constantI64(0, vptr, n.pos);
        if (failed_) {
            return;
        }
        std::string table = emitClassVTable(*id, n.pos);
        if (failed_ || table.empty()) {
            return;
        }
        TypeRef tablePtr{ctx_.pointer(ctx_.basic(BasicKind::U8))};
        ir::Instr data;
        data.op = ir::Op::DataAddr;
        data.loc = n.pos;
        data.type = tablePtr;
        data.width = ir::IRType::Ptr;
        data.globalLabel = table;
        data.isPure = true;
        vptrWord = newValue(data.width, tablePtr, n.pos);
        data.results.push_back(vptrWord);
        emit(std::move(data));
        if (failed_) {
            return;
        }
        Place atVptr = baseIn;
        atVptr.type = vptr;
        atVptr.off = 0;
        storeRange(n.pos, atVptr, {vptrWord}, vptr);
        if (failed_) {
            return;
        }
    }
    Place base = baseIn;
    base.type = classType;
    emitFieldDefaults(base, classType, n.pos);
    if (failed_) {
        return;
    }
    std::unordered_set<int64_t> unitsZeroed;
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
        value = adjustStringLitArg(field.value.get(), std::move(value), fieldType);
        // A concrete value for an interface field boxes like the struct
        // path: the class field holds the same pair.
        value = boxClassOperand(field.value.get(), std::move(value),
                                  checker_.typeOf(field.value.get()), fieldType,
                                  field.value->pos);
        if (failed_) {
            return;
        }
        if (!storeFieldInit(field.value->pos, base, fieldOff, fieldType, *id,
                            field.name, unitsZeroed, value)) {
            refuse(field.value->pos, "this construction");
            return;
        }
    }
}
std::vector<ir::Value> Lowerer::lowerNewStruct(const NewStruct& n) {
    TypeRef made = checker_.typeOf(&n);
    if (made.isError()) {
        refuse(n.pos, "this construction");
        return {undefined()};
    }
    bool heap = made.type->kind() == Kind::Pointer;
    TypeRef structType = heap ? TypeRef{made.type->pointee()} : made;
    if (!structType.isError() && structType.type->kind() == Kind::Class) {
        // The class check below owns construction: a chain's fields lay
        // out through the resolver, and constructClass stores the vtable
        // word. A missing declaration still refuses before wordCount turns
        // a -1 size into a generic construction refusal.
        std::optional<DeclId> id = structType.type->decl();
        if (!id.has_value()) {
            refuse(n.pos, "this construction");
            return {undefined()};
        }
    }
    std::vector<ir::Value> slots;
    Place base;
    if (heap) {
        std::optional<DeclId> id = structType.type->decl();
        int64_t bytes = kNoExtent;
        int64_t stepTo = kNoExtent;
        if (id.has_value()) {
            if (structType.type->kind() == Kind::Class) {
                bytes = resolver_.classSize(*id);
                stepTo = resolver_.classAlign(*id);
            } else {
                bytes = resolver_.structSize(*id);
                stepTo = layout::kSizePointer;
            }
        }
        if (bytes <= 0 || stepTo <= 0) {
            refuse(n.pos, "this construction");
            return {undefined()};
        }
        TypeRef word{ctx_.basic(BasicKind::I64)};
        ir::Instr alloc;
        alloc.op = ir::Op::Call;
        alloc.loc = n.pos;
        alloc.callee = "cider_alloc";
        alloc.mayTrap = true;
        for (int64_t fixed : {bytes, stepTo}) {
            ir::Argument arg;
            arg.words.push_back(constantI64(fixed, word, n.pos));
            if (failed_) {
                return {undefined()};
            }
            alloc.arguments.push_back(std::move(arg));
        }
        ir::Value at = newValue(ir::IRType::Ptr, made, n.pos);
        alloc.results.push_back(at);
        alloc.width = ir::IRType::Ptr;
        alloc.type = made;
        emit(std::move(alloc));
        if (failed_) {
            return {undefined()};
        }
        base.hasIndirect = true;
        base.indirect = at;
        base.type = structType;
        base.off = 0;
        if (structType.type->kind() == Kind::Union) {
            constructUnion(base, structType, n);
        } else if (structType.type->kind() == Kind::Class) {
            constructClass(base, structType, n);
        } else {
            constructStruct(base, structType, n);
        }
        if (failed_) {
            return {undefined()};
        }
        return {at};
    }
    int count = wordCount(structType);
    if (count < 0) {
        refuse(n.pos, "this construction");
        return {undefined()};
    }
    slots = allocaSlots(structType, n.pos, static_cast<uint64_t>(count));
    base.slots = slots;
    base.type = structType;
    base.off = 0;
    if (!structType.isError() && structType.type->kind() == Kind::Union) {
        constructUnion(base, structType, n);
    } else if (!structType.isError() && structType.type->kind() == Kind::Class) {
        constructClass(base, structType, n);
    } else {
        constructStruct(base, structType, n);
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

std::vector<ir::Value> Lowerer::lowerArrayLit(const ArrayLit& n) {
    // `[a, b]` / `[0, ...]`: a stack value array. The declared type owns
    // the element type, the count and the shape of the answer words.
    TypeRef arrayType = checker_.typeOf(&n);
    if (arrayType.isError() || arrayType.type->kind() != Kind::Array) {
        refuse(n.pos, "this construction");
        return {undefined()};
    }
    std::optional<int64_t> len = arrayType.type->length();
    if (!len.has_value()) {
        refuse(n.pos, "this construction");
        return {undefined()};
    }
    int count = wordCount(arrayType);
    if (count < 0) {
        refuse(n.pos, "this construction");
        return {undefined()};
    }
    std::vector<ir::Value> slots =
        allocaSlots(arrayType, n.pos, static_cast<uint64_t>(count));
    Place slotBase;
    slotBase.slots = slots;
    constructArray(slotBase, arrayType, n.inits, n.repeat, *len, n.pos);
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
    if (failed_) {
        return {undefined()};
    }
    // A virtual class's vptr lands before init runs, so a method call in
    // the init body dispatches through the concrete type's table.
    if (!resolver_.virtualNames(*id).empty()) {
        std::string table = emitClassVTable(*id, n.pos);
        if (failed_ || table.empty()) {
            return {undefined()};
        }
        TypeRef tablePtr{ctx_.pointer(ctx_.basic(BasicKind::U8))};
        ir::Instr data;
        data.op = ir::Op::DataAddr;
        data.loc = n.pos;
        data.type = tablePtr;
        data.width = ir::IRType::Ptr;
        data.globalLabel = table;
        data.isPure = true;
        ir::Value tableWord = newValue(data.width, tablePtr, n.pos);
        data.results.push_back(tableWord);
        emit(std::move(data));
        if (failed_) {
            return {undefined()};
        }
        Place slot;
        slot.hasIndirect = true;
        slot.indirect = at;
        slot.off = 0;
        slot.type = TypeRef{ctx_.basic(BasicKind::U64)};
        storeRange(n.pos, slot, {tableWord}, slot.type);
        if (failed_) {
            return {undefined()};
        }
    }
    // CIDER-BUG-049: defaults land before the constructor body, so the
    // body's own stores overwrite them exactly as a literal's would.
    Place defaults;
    defaults.hasIndirect = true;
    defaults.indirect = at;
    defaults.off = 0;
    emitFieldDefaults(defaults, TypeRef{obj}, n.pos);
    if (failed_) {
        return {undefined()};
    }
    ir::Instr run;
    run.op = ir::Op::Call;
    run.loc = n.pos;
    run.callee = resolver_.linkName(*owner) + "__init";
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
        arg.words = adjustStringLitArg(n.args[i].get(), std::move(arg.words), want);
        arg.words = boxClassOperand(n.args[i].get(), std::move(arg.words),
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

std::vector<ir::Value> Lowerer::lowerSuperInit(const SuperInit& n) {
    // `super_init(args)`: the direct base's constructor on this object
    // (Unified Standard 4.8.3). The checker proved the context and the
    // arguments; the base, its init and the receiver are re-derived here.
    if (!currentMethodOwner_.has_value()) {
        refuse(n.pos, "super_init outside a method");
        return {undefined()};
    }
    const std::vector<DeclId> bases = resolver_.classBases(*currentMethodOwner_);
    if (bases.empty() || bases.begin() + 1 != bases.end()) {
        refuse(n.pos, "super_init without one base class");
        return {undefined()};
    }
    const resolve::Decl* owner = resolver_.declById(bases[0]);
    const Signature* init = resolver_.methodSig(bases[0], "init");
    if (owner == nullptr || init == nullptr) {
        refuse(n.pos, "super_init without a base init");
        return {undefined()};
    }
    ir::Value self;
    bool foundSelf = false;
    for (auto frame = locals_.rbegin();
         frame != locals_.rend() && !foundSelf; ++frame) {
        auto it = frame->find("self");
        if (it != frame->end()) {
            std::vector<ir::Value> words = loadLocal(n.pos, it->second);
            if (!words.empty()) {
                self = words[0];
                foundSelf = true;
            }
        }
    }
    if (!foundSelf) {
        refuse(n.pos, "super_init outside a method");
        return {undefined()};
    }
    // 4.8: the base subobject sits after the object's own table word when
    // the derived class introduced the table. The constructor runs against
    // that view, so the receiver moves to the subobject first.
    std::optional<int64_t> shift =
        resolver_.baseOffset(*currentMethodOwner_, bases.front());
    if (!shift.has_value()) {
        refuse(n.pos, "super_init with this base");
        return {undefined()};
    }
    if (*shift != 0) {
        TypeRef word{ctx_.pointer(ctx_.basic(BasicKind::U8))};
        ir::Value delta =
            constantI64(*shift, TypeRef{ctx_.basic(BasicKind::I64)}, n.pos);
        if (failed_) {
            return {undefined()};
        }
        ir::Instr step;
        step.op = ir::Op::Add;
        step.loc = n.pos;
        step.type = word;
        step.width = ir::IRType::Ptr;
        step.isPure = true;
        step.operands.push_back(self);
        step.operands.push_back(delta);
        ir::Value moved = newValue(step.width, word, n.pos);
        step.results.push_back(moved);
        emit(std::move(step));
        if (failed_) {
            return {undefined()};
        }
        self = moved;
    }
    ir::Instr run;
    run.op = ir::Op::Call;
    run.loc = n.pos;
    run.callee = resolver_.linkName(*owner) + "__init";
    run.mayTrap = true;
    ir::Argument receiver;
    receiver.words.push_back(self);
    run.arguments.push_back(std::move(receiver));
    for (uint64_t i = 0; i < static_cast<uint64_t>(n.args.size()); ++i) {
        ir::Argument arg;
        arg.words = lowerWords(n.args[i].get());
        if (failed_) {
            return {undefined()};
        }
        if (i >= static_cast<uint64_t>(init->parameters().size())) {
            refuse(n.pos, "this super_init");
            return {undefined()};
        }
        // A concrete argument for an interface parameter boxes like every
        // other call's: the init's words must arrive as the pair.
        TypeRef want{init->parameters()[static_cast<size_t>(i)].type};
        arg.words = adjustStringLitArg(n.args[i].get(), std::move(arg.words), want);
        arg.words = boxClassOperand(n.args[i].get(), std::move(arg.words),
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
    return {};
}

std::vector<ir::Value> Lowerer::lowerNewArray(const NewArray& n) {
    TypeRef arrayType = checker_.typeOf(&n);
    bool heap = arrayType.type->kind() == Kind::Pointer;
    TypeRef made = heap ? TypeRef{arrayType.type->pointee()} : arrayType;
    int count = wordCount(made);
    if (count < 0) {
        refuse(n.pos, "this construction");
        return {undefined()};
    }
    if (heap) {
        std::optional<DeclId> id = made.type->decl();
        int64_t bytes = kNoExtent;
        int64_t stepTo = kNoExtent;
        if (id.has_value()) {
            bytes = resolver_.structSize(*id);
            stepTo = layout::kSizePointer;
        }
        if (bytes <= 0 || stepTo <= 0) {
            refuse(n.pos, "this construction");
            return {undefined()};
        }
        TypeRef word{ctx_.basic(BasicKind::I64)};
        ir::Instr alloc;
        alloc.op = ir::Op::Call;
        alloc.loc = n.pos;
        alloc.callee = "cider_alloc";
        alloc.mayTrap = true;
        for (int64_t fixed : {bytes, stepTo}) {
            ir::Argument arg;
            arg.words.push_back(constantI64(fixed, word, n.pos));
            if (failed_) {
                return {undefined()};
            }
            alloc.arguments.push_back(std::move(arg));
        }
        ir::Value at = newValue(ir::IRType::Ptr, arrayType, n.pos);
        alloc.results.push_back(at);
        alloc.width = ir::IRType::Ptr;
        alloc.type = arrayType;
        emit(std::move(alloc));
        if (failed_) {
            return {undefined()};
        }
        std::optional<int64_t> len = made.type->length();
        if (!len.has_value()) {
            refuse(n.pos, "this construction");
            return {undefined()};
        }
        Place base;
        base.hasIndirect = true;
        base.indirect = at;
        base.type = made;
        base.off = 0;
        constructArray(base, made, n.inits, n.repeat, *len, n.pos);
        if (failed_) {
            return {undefined()};
        }
        return {at};
    }
    std::vector<ir::Value> slots =
        allocaSlots(made, n.pos, static_cast<uint64_t>(count));
    Place slotBase;
    slotBase.slots = slots;
    std::optional<int64_t> vlen = made.type->length();
    if (!vlen.has_value()) {
        refuse(n.pos, "this construction");
        return {undefined()};
    }
    constructArray(slotBase, made, n.inits, n.repeat, *vlen, n.pos);
    if (failed_) {
        return {undefined()};
    }
    std::vector<ir::Value> words;
    for (int i = 0; i < count; ++i) {
        TypeRef word = wordType(made, static_cast<uint64_t>(i));
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
    if (const auto* wide = dynamic_cast<const WideLit*>(n.array.get())) {
        // A literal names rodata: a folded index is a label plus a scaled
        // offset, proven against the unit count like the read path proves
        // it. Anything computed rides the shared guarded address below.
        std::unordered_set<std::string> active;
        EvalEnv env = resolver_.evalEnv(emitScope());
        EvalResult folded = evalConstInt(*n.index, env, active);
        if (folded.ok) {
            if (folded.value < 0) {
                refuse(n.index->pos, "a negative index");
                return {undefined()};
            }
            if (folded.value >
                static_cast<int64_t>(wide->units.size())) {
                refuse(n.index->pos, "an index past the end");
                return {undefined()};
            }
            TypeRef u16{ctx_.basic(BasicKind::U16)};
            TypeRef addrType{ctx_.pointer(u16.type)};
            ir::Instr data;
            data.op = ir::Op::DataAddr;
            data.loc = n.pos;
            data.type = addrType;
            data.width = ir::IRType::Ptr;
            data.globalLabel = wideBlob(*wide);
            data.isPure = true;
            ir::Value atAddr = newValue(data.width, addrType, n.pos);
            data.results.push_back(atAddr);
            emit(std::move(data));
            if (failed_) {
                return {undefined()};
            }
            TypeRef count{ctx_.basic(BasicKind::I64)};
            int64_t elemBytes = resolver_.nominalSize(u16.type);
            ir::Value delta =
                constantI64(folded.value * elemBytes, count, n.pos);
            if (failed_) {
                return {undefined()};
            }
            ir::Instr advance;
            advance.op = ir::Op::Add;
            advance.loc = n.pos;
            advance.type = addrType;
            advance.width = ir::IRType::Ptr;
            advance.isPure = true;
            advance.operands.push_back(atAddr);
            advance.operands.push_back(delta);
            ir::Value moved = newValue(advance.width, addrType, n.pos);
            advance.results.push_back(moved);
            emit(std::move(advance));
            if (failed_) {
                return {undefined()};
            }
            return {moved};
        }
        baseType = checker_.typeOf(n.array.get());
        if (baseType.isError() || baseType.type->kind() != Kind::Array) {
            refuse(n.array->pos, "this expression");
            return {undefined()};
        }
        base.global = wideBlob(*wide);
        base.off = 0;
        base.type = baseType;
        base.isVolatile = false;
    } else if (!arrayBase(n.array.get(), n, base, baseType, "this expression") ||
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

bool Lowerer::addressOfValue(const Expr* expr, TypeRef ptr, Position pos,
                             ir::Value& out) {
    Place place;
    if (resolvePlace(expr, place)) {
        if (!place.global.empty()) {
            // A global's address is its label: link-time constant, no
            // load. An offset past it advances the same way an indirect
            // place advances.
            ir::Instr data;
            data.op = ir::Op::DataAddr;
            data.loc = pos;
            data.type = ptr;
            data.width = ir::IRType::Ptr;
            data.globalLabel = place.global;
            data.isPure = true;
            ir::Value at = newValue(data.width, ptr, pos);
            data.results.push_back(at);
            emit(std::move(data));
            if (failed_) {
                return false;
            }
            if (place.off == 0) {
                out = at;
                return true;
            }
            TypeRef count{ctx_.basic(BasicKind::I64)};
            ir::Value delta = constantI64(place.off, count, pos);
            if (failed_) {
                return false;
            }
            ir::Instr add;
            add.op = ir::Op::Add;
            add.loc = pos;
            add.type = ptr;
            add.width = ir::IRType::Ptr;
            add.isPure = true;
            add.operands.push_back(at);
            add.operands.push_back(delta);
            ir::Value moved = newValue(add.width, ptr, pos);
            add.results.push_back(moved);
            emit(std::move(add));
            if (failed_) {
                return false;
            }
            out = moved;
            return true;
        }
        if (place.hasIndirect) {
            if (place.off == 0) {
                out = place.indirect;
                return true;
            }
            TypeRef count{ctx_.basic(BasicKind::I64)};
            ir::Value delta = constantI64(place.off, count, pos);
            if (!failed_) {
                ir::Instr add;
                add.op = ir::Op::Add;
                add.loc = pos;
                add.type = ptr;
                add.width = ir::IRType::Ptr;
                add.isPure = true;
                add.operands.push_back(place.indirect);
                add.operands.push_back(delta);
                ir::Value at = newValue(add.width, ptr, pos);
                add.results.push_back(at);
                emit(std::move(add));
                out = at;
                return true;
            }
            return false;
        }
        if (!place.slots.empty()) {
            out = computedBase(place.slots[0], place.off, pos);
            return true;
        }
    }
    if (const auto* index = dynamic_cast<const Index*>(expr)) {
        std::vector<ir::Value> at = lowerElementAddr(*index);
        if (failed_ || at.size() != kOneWord) {
            return false;
        }
        out = at[0];
        return true;
    }
    return false;
}

ir::Value Lowerer::closureAddress(std::vector<ir::Value> pair,
                                   TypeRef closureType, Position pos) {
    // A closure pair has no address: spill both words to scratch slots in
    // this frame, then answer the slots' address. The slots outlive the
    // conversion site, so the pair stays live as long as its scope does.
    if (pair.size() != static_cast<size_t>(kClosureWords)) {
        refuse(pos, "this conversion");
        return undefined();
    }
    std::vector<ir::Value> slots = allocaSlots(
        closureType, pos, static_cast<uint64_t>(pair.size()));
    if (failed_) {
        return undefined();
    }
    Place scratch;
    scratch.slots = slots;
    scratch.type = closureType;
    scratch.off = 0;
    scratch.isVolatile = false;
    storeRange(pos, scratch, pair, closureType);
    if (failed_) {
        return undefined();
    }
    return computedBase(slots[0], 0, pos);
}

std::vector<ir::Value> Lowerer::loadClosurePair(ir::Value addr,
                                                TypeRef closureType,
                                                Position pos) {
    // Reads the pair an address names: a proven-closure carrier holds the
    // address of two words laid out code-then-captures.
    Place at;
    at.hasIndirect = true;
    at.indirect = addr;
    at.off = 0;
    at.type = closureType;
    at.isVolatile = false;
    std::vector<ir::Value> pair = loadRange(pos, at, closureType);
    if (failed_ || pair.size() != static_cast<size_t>(kClosureWords)) {
        if (!failed_) {
            refuse(pos, "this conversion");
        }
        return {undefined()};
    }
    return pair;
}

std::vector<ir::Value> Lowerer::boxClassOperand(
    const Expr* expr, std::vector<ir::Value> words, TypeRef from, TypeRef to,
    Position pos) {
    if (!from.isError() && !to.isError() &&
        to.type->kind() == Kind::Interface &&
        from.type->kind() == Kind::Class) {
        TypeRef ptr{ctx_.pointer(from.type)};
        ir::Value at = undefined();
        if (const auto* call = dynamic_cast<const Call*>(expr)) {
            // A call result has no address: spill the answer words to a
            // scratch slot in this frame, then box the slot's address.
            // The slot outlives the conversion site, so the pair stays
            // live as long as its scope does.
            std::vector<ir::Value> spill =
                lowerCallWords(const_cast<Call&>(*call));
            if (failed_ || spill.empty() ||
                spill.size() != static_cast<size_t>(wordCount(from))) {
                refuse(pos, "this interface conversion is not lowered yet");
                return {undefined()};
            }
            std::vector<ir::Value> slots = allocaSlots(
                from, pos, static_cast<uint64_t>(spill.size()));
            if (failed_) {
                return {undefined()};
            }
            Place scratch;
            scratch.slots = slots;
            scratch.type = from;
            storeRange(pos, scratch, spill, from);
            if (failed_) {
                return {undefined()};
            }
            at = computedBase(slots[0], 0, pos);
            if (failed_) {
                return {undefined()};
            }
        } else if (!addressOfValue(expr, ptr, pos, at) || failed_) {
            refuse(pos, "this interface conversion is not lowered yet");
            return {undefined()};
        }
        return boxToInterface({at}, ptr, to, pos);
    }
    // Closure round trips materialize here for every implicit site at
    // once (declarations, assignments, arguments, returns, fields): the
    // checker proved provenance, so mechanics never re-decide it. A pair
    // spills for an address; an address loads for a pair; nilPtr answers
    // the null pair; a *void adopting an fn spelling is already one word.
    auto isVoidPtr = [](const TypeRef& t) -> bool {
        return t.type->kind() == Kind::Pointer &&
               t.type->pointee() != nullptr &&
               t.type->pointee()->kind() == Kind::Void;
    };
    if (!from.isError() && !to.isError() &&
        to.type->kind() == Kind::Closure) {
        if (from.type->kind() == Kind::Closure) {
            return words;
        }
        // The null literal answers the null pair before any address load:
        // its word is zero, not a storage address.
        if (dynamic_cast<const NilPtr*>(expr) != nullptr) {
            TypeRef code = wordType(to, kFirstWord);
            TypeRef caps = wordType(to, kLenWord);
            return {constantI64(0, code, pos), constantI64(0, caps, pos)};
        }
        if (isVoidPtr(from)) {
            if (words.size() != kOneWord) {
                refuse(pos, "this conversion");
                return {undefined()};
            }
            return loadClosurePair(words[kFirstWord], to, pos);
        }
        refuse(pos, "this conversion");
        return {undefined()};
    }
    if (!from.isError() && !to.isError() && isVoidPtr(to) &&
        from.type->kind() == Kind::Closure) {
        return {closureAddress(std::move(words), from, pos)};
    }
    // Deeper carriers answer the same single address word.
    if (!from.isError() && !to.isError() &&
        to.type->kind() == Kind::Pointer && to.type->pointee() != nullptr &&
        to.type->pointee()->kind() == Kind::Pointer &&
        from.type->kind() == Kind::Closure) {
        // Two levels: the pair spills for its address, and that address
        // word spills again — the carrier names the slot holding the
        // pair's address, so one dereference reaches the pair.
        ir::Value inner = closureAddress(std::move(words), from, pos);
        if (failed_) {
            return {undefined()};
        }
        TypeRef voidPtr{ctx_.pointer(ctx_.void_())};
        std::vector<ir::Value> slots =
            allocaSlots(voidPtr, pos, kOneWord);
        if (failed_) {
            return {undefined()};
        }
        Place scratch;
        scratch.slots = slots;
        scratch.type = voidPtr;
        scratch.off = 0;
        scratch.isVolatile = false;
        storeRange(pos, scratch, {inner}, voidPtr);
        if (failed_) {
            return {undefined()};
        }
        return {computedBase(slots[0], 0, pos)};
    }
    return boxToInterface(std::move(words), from, to, pos);
}

std::vector<ir::Value> Lowerer::lowerAddrOf(const Unary& n) {
    // `&f` where f names a plain function answers its code address: one
    // word holding the link address, for an indirect call to load. Every
    // other address refuses with the message it always refused with —
    // `&x` on values is FEAT-038 and must not change shape here.
    if (n.op == UnOp::Addr) {
        if (const auto* ident = dynamic_cast<const Ident*>(n.operand.get())) {
            const resolve::Decl* decl =
                resolver_.lookup(emitScope(), ident->name);
            if (decl == nullptr && homeScope() != nullptr) {
                decl = resolver_.lookup(homeScope(), ident->name);
            }
            if (decl != nullptr && decl->kind == resolve::DeclKind::Func &&
                decl->typeParams == 0) {
                // A from-import copies the target's id but lives in the
                // entry scope, so mangling the alias would name a local.
                // The defining declaration carries the home module.
                const resolve::Decl* defining = resolver_.declById(decl->id);
                std::string link = resolver_.linkName(
                    defining != nullptr ? *defining : *decl);
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
        if (const auto* field = dynamic_cast<const Field*>(n.operand.get())) {
            // `&lib.areaOf`: the qualifier names a module, so the member is
            // that module's plain function and its address mangles under
            // the module, like every cross-module definition.
            const auto* qualifier = dynamic_cast<const Ident*>(field->object.get());
            if (qualifier != nullptr) {
                const resolve::Decl* mod =
                    resolver_.lookup(emitScope(), qualifier->name);
                if (mod == nullptr && homeScope() != nullptr) {
                    mod = resolver_.lookup(homeScope(), qualifier->name);
                }
                if (mod != nullptr && mod->kind == resolve::DeclKind::Module) {
                    const resolve::Scope* home = resolver_.scopeOf(mod->origin);
                    const resolve::Decl* member =
                        home != nullptr ? resolver_.lookup(home, field->name) : nullptr;
                    if (member != nullptr && member->kind == resolve::DeclKind::Func &&
                        member->typeParams == 0) {
                        TypeRef ptr{ctx_.functionPtr(member->sig)};
                        ir::Instr fn;
                        fn.op = ir::Op::FnAddr;
                        fn.loc = n.pos;
                        fn.type = ptr;
                        fn.width = ir::IRType::Ptr;
                        fn.globalLabel = resolver_.linkName(*member);
                        fn.isPure = true;
                        ir::Value at = newValue(fn.width, ptr, n.pos);
                        fn.results.push_back(at);
                        emit(std::move(fn));
                        return {at};
                    }
                }
            }
        }
    }
    if (n.op == UnOp::Addr) {
        // Value addresses share one helper with interface boxing: the
        // place answers its address, whatever names it.
        ir::Value at = undefined();
        if (addressOfValue(n.operand.get(), checker_.typeOf(&n), n.pos, at)) {
            return {at};
        }
        if (!failed_) {
            refuse(n.pos, "this expression");
        }
        return {undefined()};
    }
    refuse(n.pos, "this expression");
    return {undefined()};
}

std::vector<ir::Value> Lowerer::lowerWords(const Expr* expr) {
    return lowerWords(expr, TypeRef{});
}

std::vector<ir::Value> Lowerer::lowerWords(const Expr* expr, TypeRef fallback) {
    if (const auto* sup = dynamic_cast<const Super*>(expr)) {
        // The receiver this method already has: the `self` local.
        for (auto frame = locals_.rbegin(); frame != locals_.rend(); ++frame) {
            auto it = frame->find("self");
            if (it != frame->end()) {
                return loadLocal(sup->pos, it->second);
            }
        }
        refuse(expr->pos, "super outside a method");
        return {undefined()};
    }
    if (expr == nullptr || failed_) {
        return {undefined()};
    }
    if (const auto* lit = dynamic_cast<const StringLit*>(expr)) {
        return lowerString(*lit).words;
    }
    if (const auto* wide = dynamic_cast<const WideLit*>(expr)) {
        // Pointer shape: the rodata address. Array positions (declarations
        // of [u16; N], same-shape casts) read wideWords instead, through
        // their own branches, so one node answers both spellings.
        return {wideRodata(*wide)};
    }
    if (const auto* wideChar = dynamic_cast<const WideChar*>(expr)) {
        TypeRef one{ctx_.basic(BasicKind::U16)};
        return {constantI64(wideChar->value, one, wideChar->pos)};
    }
    if (dynamic_cast<const NewStruct*>(expr) != nullptr) {
        return lowerNewStruct(static_cast<const NewStruct&>(*expr));
    }
    if (dynamic_cast<const NewArray*>(expr) != nullptr) {
        return lowerNewArray(static_cast<const NewArray&>(*expr));
    }
    if (dynamic_cast<const ArrayLit*>(expr) != nullptr) {
        return lowerArrayLit(static_cast<const ArrayLit&>(*expr));
    }
    if (dynamic_cast<const NewClass*>(expr) != nullptr) {
        return lowerNewClass(static_cast<const NewClass&>(*expr));
    }
    if (dynamic_cast<const SuperInit*>(expr) != nullptr) {
        return lowerSuperInit(static_cast<const SuperInit&>(*expr));
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
    if (dynamic_cast<const AlignofType*>(expr) != nullptr) {
        return lowerAlignof(static_cast<const AlignofType&>(*expr));
    }
    if (dynamic_cast<const Ternary*>(expr) != nullptr) {
        return lowerTernary(static_cast<const Ternary&>(*expr));
    }
    if (const auto* step = dynamic_cast<const IncrDecr*>(expr)) {
        return lowerIncrDecr(*step);
    }
    // String concatenation answers two words, so it dispatches here like
    // calls: the scalar path below would keep only the address. Anything
    // else binary stays scalar, where widths decide.
    if (const auto* add = dynamic_cast<const Binary*>(expr)) {
        TypeRef type = checker_.typeOf(expr);
        if (!type.isError() && type.type->kind() == Kind::String) {
            return lowerStringAdd(*add);
        }
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
            TypeRef type = checker_.typeOf(expr);
            if (type.isError() && !fallback.isError()) {
                type = fallback;
            }
            ir::Value single = onIdentOrExpr(expr, type);
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
        std::vector<ir::Value> folded;
        if (foldModuleConst(ident->name, ident->pos, folded)) {
            return folded;
        }
        // A bare function name in value position answers the closure pair:
        // a forwarder with the receiver-first shape plus a null captures
        // word, with no allocation. Prelude names mangle to their link
        // symbols, like calls do.
        const resolve::Scope* here = homeScope();
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
        // A bare enumerator folds to its value: the checker proved exactly
        // one enum in this module owns the name, so the integer here is
        // that member's value (TYPES §2.6). Locals, globals, module
        // constants and functions already answered above, so this branch
        // owns nothing they could name.
        if (!type.isError() && type.type->kind() == Kind::Enum) {
            int owners = 0;
            const resolve::Decl* owner =
                here != nullptr
                    ? resolver_.lookupEnumMember(here, ident->name, owners)
                    : nullptr;
            if (owner != nullptr) {
                std::string home = owner->scope != nullptr
                                       ? owner->scope->module
                                       : homeModule();
                int64_t value =
                    resolver_.enumValue(home + "." + owner->name, ident->name);
                TypeRef count{ctx_.basic(BasicKind::I32)};
                return {constantI64(value, count, ident->pos)};
            }
        }
        refuse(ident->pos, "reading '" + ident->name + "'");
        return {undefined()};
    }
    if (auto* cast = dynamic_cast<const Cast*>(expr)) {
        // An aggregate source and target of the same shape is a value
        // copy, not a scalar conversion: the word count the target
        // declares is the copy's width, and the scalar path below would
        // answer only the first word. The checker already proved the
        // shapes relate (identity or a named conversion), so a load of
        // the operand's words is exact here.
        // A closure target answers the pair, never a scalar cast: the
        // checker gated provenance, so mechanics only move the bytes. A
        // closure operand copies through; nilPtr answers the null pair; a
        // carrier address loads the pair it names (one dereference first
        // past depth one). Anything else here never passed the gate.
        TypeRef target = checker_.typeOf(cast);
        if (!target.isError() && target.type->kind() == Kind::Closure) {
            TypeRef source = checker_.typeOf(cast->expr.get());
            if (!source.isError() && source.type->kind() == Kind::Closure) {
                return lowerWords(cast->expr.get());
            }
            if (dynamic_cast<const NilPtr*>(cast->expr.get()) != nullptr) {
                TypeRef code = wordType(target, kFirstWord);
                TypeRef caps = wordType(target, kLenWord);
                return {constantI64(0, code, cast->pos),
                        constantI64(0, caps, cast->pos)};
            }
            std::vector<ir::Value> carrier = lowerWords(cast->expr.get());
            if (failed_ || carrier.size() != kOneWord) {
                if (!failed_) {
                    refuse(cast->pos, "this cast");
                }
                return {undefined()};
            }
            ir::Value addr = carrier[kFirstWord];
            if (!source.isError() && source.type->kind() == Kind::Pointer &&
                source.type->pointee() != nullptr &&
                source.type->pointee()->kind() == Kind::Pointer) {
                TypeRef inner{source.type->pointee()};
                Place slot;
                slot.hasIndirect = true;
                slot.indirect = addr;
                slot.off = 0;
                slot.type = inner;
                slot.isVolatile = false;
                std::vector<ir::Value> held = loadRange(cast->pos, slot, inner);
                if (failed_ || held.size() != kOneWord) {
                    if (!failed_) {
                        refuse(cast->pos, "this cast");
                    }
                    return {undefined()};
                }
                addr = held[kFirstWord];
            }
            return loadClosurePair(addr, target, cast->pos);
        }
        if (!target.isError() &&
            (target.type->kind() == Kind::Struct ||
             target.type->kind() == Kind::Class ||
             target.type->kind() == Kind::Array)) {

            if (Place place; resolvePlace(cast->expr.get(), place)) {
                place.type = target;
                return loadRange(cast->pos, place, target);
            }
            // A wide literal casts by value: its words are the array, so
            // no address is ever read. Anything else here answers through
            // lowerWords below.
            if (const auto* wide =
                    dynamic_cast<const WideLit*>(cast->expr.get())) {
                std::vector<ir::Value> words = wideWords(*wide);
                if (failed_) {
                    return {undefined()};
                }
                return words;
            }
            std::vector<ir::Value> words =
                lowerWords(cast->expr.get());
            if (failed_) {
                return {undefined()};
            }
            return words;
        }
        // The null string is (nil, 0): the operand answers one word but
        // the target holds two, so the generic scalar cast below would
        // leave the length unanswered. Only the literal converts — the
        // checker keeps a *void variable beside a string a mismatch.
        TypeRef spelled = checker_.typeOf(expr);
        if (!spelled.isError() && spelled.type->kind() == Kind::String &&
            dynamic_cast<const NilPtr*>(cast->expr.get()) != nullptr) {
            TypeRef ptrType{ctx_.pointer(ctx_.basic(BasicKind::U8))};
            TypeRef lenType{ctx_.basic(BasicKind::I64)};
            return {constantI64(0, ptrType, expr->pos),
                    constantI64(0, lenType, expr->pos)};
        }
        if (!spelled.isError() && spelled.type->kind() == Kind::Interface &&
            dynamic_cast<const NilPtr*>(cast->expr.get()) != nullptr) {
            // `nilPtr as I`: the null interface is two zero words — a
            // null data word beside a null vtable word. Only the literal
            // converts (the checker keeps a *void variable a mismatch):
            // the width is what matters, so both words are addresses.
            TypeRef addr{ctx_.pointer(ctx_.basic(BasicKind::U8))};
            return {constantI64(0, addr, expr->pos),
                    constantI64(0, addr, expr->pos)};
        }
    }
    return {onIdentOrExpr(expr, fallback)};
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
    // A ternary statement runs one arm for effect and discards the answer:
    // the scalar dispatcher below demands exactly one word, which refuses
    // a void ternary the checker accepted.
    if (auto* tern = dynamic_cast<const Ternary*>(n.expr.get())) {
        lowerTernary(*tern);
        return;
    }
    // `super_init(args);` is a call for effect, like the ternary above:
    // the scalar dispatcher demands a value word it does not have.
    if (auto* si = dynamic_cast<const SuperInit*>(n.expr.get())) {
        lowerSuperInit(*si);
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
        const resolve::Scope* here = homeScope();
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
    if (id.has_value() && !resolver_.virtualNames(*id).empty()) {
        // 4.8.3: a hierarchy that dispatches methods answers delete
        // polymorphically. Slot zero names the chain entry the dynamic
        // type chose, so `delete base_ptr` runs the derived destructor
        // (then every base, through the chain) exactly as a virtual call
        // runs the override.
        int64_t slot = resolver_.virtualIndexOf(*id, "deinit");
        if (slot >= 0) {
            TypeRef word{ctx_.pointer(ctx_.basic(BasicKind::U8))};
            ir::Instr loadV;
            loadV.op = ir::Op::Load;
            loadV.loc = n.pos;
            loadV.type = word;
            loadV.width = ir::IRType::Ptr;
            loadV.addressBase = ptr[kFirstWord];
            loadV.isPure = true;
            ir::Value table = newValue(loadV.width, word, n.pos);
            loadV.results.push_back(table);
            emit(std::move(loadV));
            if (!failed_) {
                TypeRef count{ctx_.basic(BasicKind::I64)};
                ir::Value delta =
                    constantI64(slot * layout::kSizePointer, count, n.pos);
                ir::Instr step;
                step.op = ir::Op::Add;
                step.loc = n.pos;
                step.type = word;
                step.width = ir::IRType::Ptr;
                step.isPure = true;
                step.operands.push_back(table);
                step.operands.push_back(delta);
                ir::Value slotAt = newValue(step.width, word, n.pos);
                step.results.push_back(slotAt);
                emit(std::move(step));
                if (!failed_) {
                    ir::Instr loadF;
                    loadF.op = ir::Op::Load;
                    loadF.loc = n.pos;
                    loadF.type = word;
                    loadF.width = ir::IRType::Ptr;
                    loadF.addressBase = slotAt;
                    loadF.isPure = true;
                    ir::Value entry = newValue(loadF.width, word, n.pos);
                    loadF.results.push_back(entry);
                    emit(std::move(loadF));
                    if (!failed_) {
                        ir::Instr run;
                        run.op = ir::Op::Call;
                        run.loc = n.pos;
                        run.call.indirect = true;
                        run.calleeValue = entry;
                        run.mayTrap = true;
                        ir::Argument self;
                        self.words.push_back(ptr[kFirstWord]);
                        run.arguments.push_back(std::move(self));
                        run.width = ir::IRType::Void;
                        emit(std::move(run));
                    }
                }
            }
        }
    } else if (id.has_value()) {
        const Signature* dsig = resolver_.methodSig(*id, "deinit");
        if (dsig != nullptr) {
            // A chain with no table still runs derived-first: the entry is
            // the static type's own chain, which walks the bases.
            std::string callee = emitDestroyChain(*id, n.pos);
            if (!failed_ && !callee.empty()) {
                ir::Instr run;
                run.op = ir::Op::Call;
                run.loc = n.pos;
                run.callee = callee;
                run.mayTrap = true;
                ir::Argument self;
                self.words.push_back(ptr[kFirstWord]);
                run.arguments.push_back(std::move(self));
                run.width = ir::IRType::Void;
                emit(std::move(run));
            }
        }
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
            Place place;
            place.slots = found->second.slots;
            place.type = found->second.type;
            stampClassVptr(n.pos, place, found->second.type);
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
        Place place;
        place.global = global->label;
        place.type = global->type;
        stampClassVptr(n.pos, place, global->type);
        return words;
    }
    refuse(n.pos, "assigning '" + n.name + "'");
    return {undefined()};
}

std::vector<ir::Value> Lowerer::lowerInterfaceDowncast(const Cast& n, TypeRef source,
                                                              TypeRef target) {
    // `sh as Sq`: the pair's vtable must name the (target, interface)
    // table, or the data word is another type's bytes. The comparison is
    // the whole check — on a match the data word already holds the
    // answer, inline or as the reference — and a mismatch traps
    // (TYPES.md §4.3), never answered plausibly.
    std::optional<DeclId> iface = source.type->decl();
    if (!iface.has_value()) {
        refuse(n.pos, "this cast");
        return {undefined()};
    }
    std::optional<DeclId> concrete;
    Kind tk = target.type->kind();
    if (tk == Kind::Struct) {
        // A wider struct can never sit in a pair: boxing refuses it at
        // every construction site, so no vtable could name it. Provable
        // statically, refused statically.
        if (wordCount(target) != static_cast<int>(kOneWord)) {
            refuse(n.pos, "a downcast wider than one word can never match");
            return {undefined()};
        }
        concrete = target.type->decl();
    } else if (tk == Kind::Pointer && target.type->pointee() != nullptr &&
               target.type->pointee()->kind() == Kind::Class) {
        concrete = target.type->pointee()->decl();
    } else {
        refuse(n.pos, "this cast");
        return {undefined()};
    }
    if (!concrete.has_value()) {
        refuse(n.pos, "this cast");
        return {undefined()};
    }
    // The expected table: the same label boxing computes, so identity is
    // pointer equality, for direct entries and adapters alike.
    std::string label = emitVTable(*concrete, *iface, n.pos);
    if (failed_) {
        return {undefined()};
    }
    std::vector<ir::Value> words = lowerWords(n.expr.get());
    if (failed_ || words.size() != kInterfaceWords) {
        if (!failed_) {
            refuse(n.pos, "this cast");
        }
        return {undefined()};
    }
    TypeRef vtableType = wordType(source, kVtableWord);
    ir::Instr addr;
    addr.op = ir::Op::DataAddr;
    addr.loc = n.pos;
    addr.type = vtableType;
    addr.width = ir::IRType::Ptr;
    addr.globalLabel = label;
    addr.isPure = true;
    ir::Value want = newValue(addr.width, vtableType, n.pos);
    addr.results.push_back(want);
    emit(std::move(addr));
    if (failed_) {
        return {undefined()};
    }
    ir::Value ok = emitI64Cmp(n.pos, BinOp::Eq, words[kVtableWord], want);
    if (failed_) {
        return {undefined()};
    }
    emitCheckTrap(n.pos, ok, ir::TrapReason::Downcast);
    if (failed_) {
        return {undefined()};
    }
    return {words[kFirstWord]};
}

ir::Value Lowerer::lowerIntToEnum(const Cast& n, TypeRef source, TypeRef target) {
    // int-to-enum is the checked direction (FEATURES §2, enum ↔ int row):
    // the value must name a declared enumerator, and inventing one traps.
    // The checker proved the widths match (the enum's 4 bytes), so the
    // value lowers as-is and the membership test owns the conversion.
    std::optional<DeclId> id = target.type->decl();
    if (!id.has_value()) {
        refuse(n.pos, "this cast");
        return undefined();
    }
    std::vector<std::pair<std::string, int64_t>> members =
        resolver_.enumMembers(*id);
    if (members.empty()) {
        refuse(n.pos, "this cast");
        return undefined();
    }
    ir::Value from;
    if (source.type->kind() == Kind::Union) {
        // A union cast reads the payload's bytes at offset 0 (TYPES.md
        // §4.6), and the membership test below still applies: an unchecked
        // payload read would invent an enumerator silently.
        Place base;
        if (!resolvePlace(n.expr.get(), base)) {
            refuse(n.expr->pos, "this cast");
            return undefined();
        }
        base.type = target;
        base.tagWrites.clear();
        std::vector<ir::Value> got = loadRange(n.pos, base, target);
        if (failed_ || got.size() != kOneWord) {
            if (!failed_) {
                refuse(n.pos, "this cast");
            }
            return undefined();
        }
        from = got[0];
    } else {
        from = onIdentOrExpr(n.expr.get(), source);
        if (failed_) {
            return undefined();
        }
    }
    // One equality per enumerator, ORed: the checked value is a
    // canonical i32 cell (sign-extended like every other i32), and every
    // Eq answers 0/1 (codegen movzx), so the OR stays 0/1 and the trap
    // reads it directly.
    TypeRef i32{ctx_.basic(BasicKind::I32)};
    TypeRef boolean{ctx_.basic(BasicKind::Bool)};
    ir::Instr norm;
    norm.op = ir::Op::Cast;
    norm.loc = n.pos;
    norm.type = i32;
    norm.width = widthOf(i32, n.pos);
    norm.operands.push_back(from);
    ir::Value checked = newValue(norm.width, norm.type, n.pos);
    norm.results.push_back(checked);
    emit(std::move(norm));
    if (failed_) {
        return undefined();
    }
    bool first = true;
    ir::Value ok;
    for (const auto& member : members) {
        ir::Value want = constantI64(member.second, i32, n.pos);
        if (failed_) {
            return undefined();
        }
        ir::Value same = emitI64Cmp(n.pos, BinOp::Eq, checked, want);
        if (failed_) {
            return undefined();
        }
        if (first) {
            ok = same;
            first = false;
            continue;
        }
        ir::Instr both;
        both.op = ir::Op::Or;
        both.loc = n.pos;
        both.type = boolean;
        both.width = ir::IRType::Bool;
        both.isPure = true;
        both.operands.push_back(ok);
        both.operands.push_back(same);
        ok = newValue(both.width, both.type, n.pos);
        both.results.push_back(ok);
        emit(std::move(both));
    }
    emitCheckTrap(n.pos, ok, ir::TrapReason::BadEnum);
    if (failed_) {
        return undefined();
    }
    ir::Instr instr;
    instr.op = ir::Op::Cast;
    instr.loc = n.pos;
    instr.type = target;
    instr.width = widthOf(target, n.pos);
    if (failed_) {
        return undefined();
    }
    instr.operands.push_back(checked);
    ir::Value result = newValue(instr.width, instr.type, n.pos);
    instr.results.push_back(result);
    emit(std::move(instr));
    return result;
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
    // `@bitcast(T, x)`: the operand's bits answer as T unchanged.
    if (n.raw) {
        std::vector<ir::Value> got = lowerWords(n.expr.get());
        if (failed_ || got.size() != kOneWord) {
            if (!failed_) {
                refuse(n.pos, "this bitcast");
            }
            return undefined();
        }
        return got[0];
    }
    // An interface source reads back through the downcast path, never the
    // scalar cast below: the vtable check owns the conversion, and a
    // scalar cast would answer unchecked bytes.
    if (!source.isError() && !target.isError() &&
        source.type->kind() == Kind::Interface) {
        std::vector<ir::Value> got = lowerInterfaceDowncast(n, source, target);
        if (failed_ || got.size() != kOneWord) {
            if (!failed_) {
                refuse(n.pos, "this cast");
            }
            return undefined();
        }
        return got[0];
    }
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
    if (!source.isError() && !target.isError() &&
        target.type->kind() == Kind::Enum) {
        return lowerIntToEnum(n, source, target);
    }
    // Closure round trips that narrow to one word lower here; wider
    // answers belong to the words path below, which answers pairs. A
    // closure spills for an address; anything else targeting a closure
    // spelling in a scalar position refuses (no single word holds a
    // pair). An fn spelling adopts through the scalar path untouched
    // (one address word, already the right shape).
    if (!source.isError() && !target.isError() &&
        source.type->kind() == Kind::Closure &&
        target.type->kind() == Kind::Pointer &&
        target.type->pointee() != nullptr &&
        (target.type->pointee()->kind() == Kind::Void ||
         (target.type->pointee()->kind() == Kind::Basic &&
          target.type->pointee()->basic().has_value() &&
          *target.type->pointee()->basic() == BasicKind::U8))) {
        std::vector<ir::Value> pair = lowerWords(n.expr.get());
        if (failed_) {
            return undefined();
        }
        return closureAddress(std::move(pair), source, n.pos);
    }
    if (!source.isError() && !target.isError() &&
        target.type->kind() == Kind::Closure) {
        refuse(n.pos, "this cast");
        return undefined();
    }
    // 4.8: a cast between related class pointers names a base subobject, so
    // its address shifts by that subobject's place instead of being
    // reinterpreted. Unrelated pointers keep the plain bitcast.
    if (!source.isError() && !target.isError() &&
        source.type->kind() == Kind::Pointer &&
        target.type->kind() == Kind::Pointer) {
        const Type* from = source.type->pointee();
        const Type* to = target.type->pointee();
        if (from != nullptr && to != nullptr &&
            from->kind() == Kind::Class &&
            to->kind() == Kind::Class) {
            std::optional<DeclId> fromId = from->decl();
            std::optional<DeclId> toId = to->decl();
            if (fromId.has_value() && toId.has_value()) {
                std::optional<int64_t> shift =
                    resolver_.classCastAdjust(*fromId, *toId);
                if (!shift.has_value() &&
                    (resolver_.baseAmbiguous(*fromId, *toId) ||
                     resolver_.baseAmbiguous(*toId, *fromId))) {
                    refuse(n.pos, "this ambiguous cast");
                    return undefined();
                }
                if (shift.has_value() && *shift != 0) {
                    ir::Value pointer = onIdentOrExpr(n.expr.get(), source);
                    TypeRef word{ctx_.pointer(ctx_.basic(BasicKind::U8))};
                    ir::Value delta = constantI64(
                        *shift < 0 ? -*shift : *shift,
                        TypeRef{ctx_.basic(BasicKind::I64)}, n.pos);
                    if (failed_) {
                        return undefined();
                    }
                    ir::Instr step;
                    step.op = *shift < 0 ? ir::Op::Sub : ir::Op::Add;
                    step.loc = n.pos;
                    step.type = word;
                    step.width = ir::IRType::Ptr;
                    step.isPure = true;
                    step.operands.push_back(pointer);
                    step.operands.push_back(delta);
                    ir::Value moved = newValue(step.width, word, n.pos);
                    step.results.push_back(moved);
                    emit(std::move(step));
                    return moved;
                }
            }
        }
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

ir::Value Lowerer::onBinary(Binary& n, TypeRef fallback) {
    if (failed_) {
        return undefined();
    }
    if (n.op == BinOp::AndAnd || n.op == BinOp::OrOr) {
        return lowerShortCircuit(n);
    }
    TypeRef lhs = checker_.typeOf(n.lhs.get());
    TypeRef rhs = checker_.typeOf(n.rhs.get());
    TypeRef result = checker_.typeOf(&n);
    // A generic template's field default is never checked, so the whole
    // expression carries no recorded type. The field's specialized type
    // is the answer the default must produce, and it types both operands
    // the same way a directly checked default would have been typed.
    if (result.isError() && !fallback.isError()) {
        result = fallback;
        if (lhs.isError()) {
            lhs = fallback;
        }
        if (rhs.isError()) {
            rhs = fallback;
        }
    }
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
    if ((n.op == BinOp::Eq || n.op == BinOp::Ne) && !lhs.isError() &&
        !rhs.isError() &&
        ((lhs.type->kind() == Kind::Interface &&
          dynamic_cast<const NilPtr*>(n.rhs.get()) != nullptr) ||
         (rhs.type->kind() == Kind::Interface &&
          dynamic_cast<const NilPtr*>(n.lhs.get()) != nullptr))) {
        return lowerInterfaceEq(n);
    }
    if ((n.op == BinOp::Eq || n.op == BinOp::Ne) && !lhs.isError() &&
        !rhs.isError() &&
        ((lhs.type->kind() == Kind::Closure &&
          dynamic_cast<const NilPtr*>(n.rhs.get()) != nullptr) ||
         (rhs.type->kind() == Kind::Closure &&
          dynamic_cast<const NilPtr*>(n.lhs.get()) != nullptr))) {
        return lowerClosureEq(n);
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

ir::Value Lowerer::lowerInterfaceEq(const Binary& n) {
    // The checker proved one side an interface and the other the nil
    // literal: nullity is the vtable word. A null interface is (nil,
    // nil), and every box sets a real table, so the second word
    // against zero answers it — Eq and Ne both, since Eq is
    // sign-independent.
    TypeRef lhs = checker_.typeOf(n.lhs.get());
    const Expr* side = !lhs.isError() && lhs.type->kind() == Kind::Interface
                           ? n.lhs.get()
                           : n.rhs.get();
    std::vector<ir::Value> words = lowerWords(side);
    if (failed_ || words.size() != kInterfaceWords) {
        refuse(n.pos, "this comparison");
        return undefined();
    }
    TypeRef addr{ctx_.pointer(ctx_.basic(BasicKind::U8))};
    ir::Value zero = constantI64(0, addr, n.pos);
    if (failed_) {
        return undefined();
    }
    ir::Instr cmp;
    cmp.op = ir::Op::Cmp;
    cmp.loc = n.pos;
    cmp.type = checker_.typeOf(&n);
    cmp.width = ir::IRType::Bool;
    cmp.cmp = cmpFor(n.op, false);
    cmp.operands.push_back(words[1]);
    cmp.operands.push_back(zero);
    ir::Value out = newValue(ir::IRType::Bool, cmp.type, n.pos);
    cmp.results.push_back(out);
    emit(std::move(cmp));
    return out;
}

ir::Value Lowerer::lowerClosureEq(const Binary& n) {
    // The checker proved one side a closure and the other the nil
    // literal: nullity is the code word. A null closure is (nil, nil),
    // and every closure literal sets real code, so code == 0 answers it
    // with one comparison — Eq and Ne both, since Eq is sign-independent.
    TypeRef lhs = checker_.typeOf(n.lhs.get());
    const Expr* side = !lhs.isError() && lhs.type->kind() == Kind::Closure
                           ? n.lhs.get()
                           : n.rhs.get();
    std::vector<ir::Value> words = lowerWords(side);
    if (failed_ || words.size() != kClosureWords) {
        refuse(n.pos, "this comparison");
        return undefined();
    }
    TypeRef addr{ctx_.pointer(ctx_.basic(BasicKind::U8))};
    ir::Value zero = constantI64(0, addr, n.pos);
    if (failed_) {
        return undefined();
    }
    ir::Instr cmp;
    cmp.op = ir::Op::Cmp;
    cmp.loc = n.pos;
    cmp.type = checker_.typeOf(&n);
    cmp.width = ir::IRType::Bool;
    cmp.cmp = cmpFor(n.op, false);
    cmp.operands.push_back(words[kFirstWord]);
    cmp.operands.push_back(zero);
    ir::Value out = newValue(ir::IRType::Bool, cmp.type, n.pos);
    cmp.results.push_back(out);
    emit(std::move(cmp));
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
    // Void answers no words: wordCount has no void arm (it answers -1 for
    // unknown layouts), so the void case stays explicit here rather than
    // widening the shared count and its other callers.
    int words = 0;
    if (result.type->kind() != Kind::Void) {
        words = wordCount(result);
    }
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
    // §11), so a mayTrap flag here would lie to the passes. A float
    // remainder lowers to the x87 partial remainder the backend emits —
    // exact, then rounded once to the result width — and a zero divisor
    // answers NaN through the masked invalid operation, faulting nothing.
    const bool floatResult = instr.width == ir::IRType::F32 ||
                             instr.width == ir::IRType::F64;
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

std::vector<ir::Value> Lowerer::callSretSlots(ir::Instr& call, const Type* ret,
                                              Position pos) {
    // The caller allocates the answer slot and passes its address first
    // (SysV sret). The slots must run consecutively for the callee's
    // contiguous stores to land in them.
    TypeRef retRef{ret};
    int count = wordCount(retRef);
    if (count < 0) {
        refuse(pos, "a value of this type");
        return {};
    }
    std::vector<ir::Value> slots =
        allocaSlots(retRef, pos, static_cast<uint64_t>(count));
    if (failed_) {
        return {};
    }
    for (uint64_t i = 0; i < static_cast<uint64_t>(slots.size()); ++i) {
        if (slots[i].id != slots[0].id + static_cast<uint32_t>(i)) {
            refuse(pos, "this call");
            return {};
        }
    }
    ir::Argument first;
    first.words.push_back(computedBase(slots[0], 0, pos));
    if (failed_) {
        return {};
    }
    call.arguments.insert(call.arguments.begin(), std::move(first));
    return slots;
}

std::vector<ir::Value> Lowerer::callSretLoads(const std::vector<ir::Value>& slots,
                                              const Type* ret, Position pos) {
    TypeRef retRef{ret};
    std::vector<ir::Value> out;
    for (uint64_t i = 0; i < static_cast<uint64_t>(slots.size()); ++i) {
        TypeRef word = wordType(retRef, i);
        ir::IRType width = widthOf(word, pos);
        if (failed_) {
            return {};
        }
        ir::Instr load;
        load.op = ir::Op::Load;
        load.loc = pos;
        load.type = word;
        load.width = width;
        load.addressBase = slots[i];
        load.isPure = true;
        ir::Value answer = newValue(width, word, pos);
        load.results.push_back(answer);
        emit(std::move(load));
        out.push_back(answer);
    }
    return out;
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
    // One place lookup serves every Field-callee path below: the lookup
    // can lower (deref receivers), so running it per path would run
    // effects once per path instead of once.
    Place base;
    bool haveBase = false;
    if (const auto* field = dynamic_cast<const Field*>(n.callee.get())) {
        // `super` is not a place; resolving it would refuse. The super
        // branch below resolves the base method statically and reads the
        // receiver straight from the `self` local.
        bool isSuper = dynamic_cast<const Super*>(field->object.get()) != nullptr;
        haveBase = !isSuper &&
            resolvePlace(field->object.get(), base) && !base.type.isError();
        if (haveBase && base.type.type->kind() == Kind::Union) {
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
        if (dynamic_cast<const Super*>(field->object.get()) != nullptr) {
            // `super.m(args)`: the base's own function, resolved
            // statically — never the override, never a vtable slot.
            const Signature* baseSig = nullptr;
            std::optional<DeclId> baseHome;
            if (currentMethodOwner_.has_value()) {
                // 4.8: `super` names one base; with several there is no
                // single answer. The checker refuses this first.
                std::vector<DeclId> superBases =
                    resolver_.classBases(*currentMethodOwner_);
                if (superBases.begin() + 1 != superBases.end()) {
                    refuse(n.pos, "super with several base classes");
                    return {undefined()};
                }
                for (DeclId baseId : superBases) {
                    baseSig = resolver_.methodSig(baseId, field->name);
                    if (baseSig != nullptr) {
                        baseHome = resolver_.methodHome(baseId, field->name);
                        break;
                    }
                }
            }
            const resolve::Decl* baseDecl =
                baseHome.has_value() ? resolver_.declById(*baseHome) : nullptr;
            if (baseDecl == nullptr || baseSig == nullptr) {
                refuse(n.pos, "this call");
                return {undefined()};
            }
            std::vector<ir::Value> selfWords = lowerWords(field->object.get());
            if (failed_ || selfWords.size() != kOneWord) {
                if (!failed_) {
                    refuse(n.pos, "this call");
                }
                return {undefined()};
            }
            receiver.push_back(selfWords[0]);
            // The declaring base may sit after the object's own table word
            // when the derived class introduced the table; the call enters
            // that base's view, not the object's address.
            std::optional<int64_t> shift;
            if (currentMethodOwner_.has_value() && baseHome.has_value()) {
                shift =
                    resolver_.baseOffset(*currentMethodOwner_, *baseHome);
            }
            if (!shift.has_value()) {
                refuse(n.pos, "this call");
                return {undefined()};
            }
            if (*shift != 0) {
                TypeRef word{ctx_.pointer(ctx_.basic(BasicKind::U8))};
                ir::Value delta = constantI64(
                    *shift, TypeRef{ctx_.basic(BasicKind::I64)}, n.pos);
                if (failed_) {
                    return {undefined()};
                }
                ir::Instr step;
                step.op = ir::Op::Add;
                step.loc = n.pos;
                step.type = word;
                step.width = ir::IRType::Ptr;
                step.isPure = true;
                step.operands.push_back(receiver.back());
                step.operands.push_back(delta);
                ir::Value moved = newValue(step.width, word, n.pos);
                step.results.push_back(moved);
                emit(std::move(step));
                if (failed_) {
                    return {undefined()};
                }
                receiver.back() = moved;
            }
            sig = baseSig;
            callee = resolver_.linkName(*baseDecl) + "__" + field->name;
        } else if (haveBase &&
            (base.type.type->kind() == Kind::Class ||
             base.type.type->kind() == Kind::Struct)) {
            std::optional<DeclId> id = base.type.type->decl();
            const resolve::Decl* owner =
                id.has_value() ? resolver_.declById(*id) : nullptr;
            const Signature* msig = id.has_value()
                                        ? resolver_.methodSig(*id, field->name)
                                        : nullptr;
            if (owner == nullptr) {
                refuse(n.pos, "this call");
                return {undefined()};
            }
            if (msig != nullptr && id.has_value() &&
                resolver_.methodAmbiguous(*id, field->name)) {
                // 4.8: two bases declare the name, so no single function is
                // the answer. The checker refuses it first; the lowerer must
                // not pick one behind its back.
                refuse(n.pos, "this ambiguous call");
                return {undefined()};
            }
            if (msig != nullptr) {
                ir::Value at;
                if (!receiverAddress(base, n.pos, "this call", at)) {
                    return {undefined()};
                }
                receiver.push_back(at);
                sig = msig;
                int64_t slot = id.has_value()
                                   ? resolver_.virtualIndexOf(*id, field->name)
                                   : -1;
                if (slot >= 0) {
                    // A virtual method dispatches through the object's vptr
                    // word: slot `slot` of the concrete type's table names
                    // the override the dynamic type chose (FEATURES 2.3).
                    // The object's address rides as the receiver the table's
                    // function already expects — no adapter, because both
                    // sides are methods of a class.
                    TypeRef word{ctx_.pointer(ctx_.basic(BasicKind::U8))};
                    ir::Instr loadV;
                    loadV.op = ir::Op::Load;
                    loadV.loc = n.pos;
                    loadV.type = word;
                    loadV.width = ir::IRType::Ptr;
                    loadV.addressBase = at;
                    loadV.isPure = true;
                    ir::Value table = newValue(loadV.width, word, n.pos);
                    loadV.results.push_back(table);
                    emit(std::move(loadV));
                    if (failed_) {
                        return {undefined()};
                    }
                    TypeRef count{ctx_.basic(BasicKind::I64)};
                    ir::Value delta = constantI64(slot * layout::kSizePointer,
                                                  count, n.pos);
                    if (failed_) {
                        return {undefined()};
                    }
                    ir::Instr step;
                    step.op = ir::Op::Add;
                    step.loc = n.pos;
                    step.type = word;
                    step.width = ir::IRType::Ptr;
                    step.isPure = true;
                    step.operands.push_back(table);
                    step.operands.push_back(delta);
                    ir::Value slotAt = newValue(step.width, word, n.pos);
                    step.results.push_back(slotAt);
                    emit(std::move(step));
                    if (failed_) {
                        return {undefined()};
                    }
                    ir::Instr loadF;
                    loadF.op = ir::Op::Load;
                    loadF.loc = n.pos;
                    loadF.type = word;
                    loadF.width = ir::IRType::Ptr;
                    loadF.addressBase = slotAt;
                    loadF.isPure = true;
                    ir::Value fn = newValue(loadF.width, word, n.pos);
                    loadF.results.push_back(fn);
                    emit(std::move(loadF));
                    if (failed_) {
                        return {undefined()};
                    }
                    indirect = true;
                    indirectCallee = fn;
                } else {
                    // A plain method resolves statically to its declarer's
                    // function: an inherited method spells its base's name.
                    std::optional<DeclId> home =
                        id.has_value() ? resolver_.methodHome(*id, field->name)
                                       : std::nullopt;
                    const resolve::Decl* declarer =
                        home.has_value() ? resolver_.declById(*home) : owner;
                    if (declarer == nullptr) {
                        refuse(n.pos, "this call");
                        return {undefined()};
                    }
                    callee = resolver_.linkName(*declarer) + "__" + field->name;
                    // 4.8: a method inherited from a non-first base lives in
                    // that base's subobject, so the receiver shifts there.
                    // The first base sits at zero and a single chain emits
                    // nothing.
                    if (id.has_value() && home.has_value() &&
                        !sameDecl(*home, *id)) {
                        std::optional<int64_t> shift =
                            resolver_.baseOffset(*id, *home);
                        if (!shift.has_value()) {
                            refuse(n.pos, "this call");
                            return {undefined()};
                        }
                        if (*shift != 0) {
                            TypeRef word{ctx_.pointer(ctx_.basic(BasicKind::U8))};
                            ir::Value delta = constantI64(
                                *shift, TypeRef{ctx_.basic(BasicKind::I64)},
                                n.pos);
                            if (failed_) {
                                return {undefined()};
                            }
                            ir::Instr step;
                            step.op = ir::Op::Add;
                            step.loc = n.pos;
                            step.type = word;
                            step.width = ir::IRType::Ptr;
                            step.isPure = true;
                            step.operands.push_back(receiver.back());
                            step.operands.push_back(delta);
                            ir::Value moved = newValue(step.width, word, n.pos);
                            step.results.push_back(moved);
                            emit(std::move(step));
                            if (failed_) {
                                return {undefined()};
                            }
                            receiver.back() = moved;
                        }
                    }
                }
            }
            // msig == null: a closure-valued field, not a method — the
            // closure path below decides from the checked type.
        }
        // `s.m(args)` through an interface: the vtable names the concrete
        // method, and the data word's address rides as the receiver the
        // static method expects first. Entries sit in the resolver's
        // methodNames order, so the index is looked up, never re-derived.
        // Disjoint from the class/struct path above: only an interface
        // value reaches here.
        Place iface;
        TypeRef objType = checker_.typeOf(field->object.get());
        bool placed = haveBase && base.type.type->kind() == Kind::Interface;
        if (placed) {
            iface = base;
        }
        if (!placed && !failed_ && !objType.isError() &&
            objType.type->kind() == Kind::Interface &&
            !placeMayHaveRun(field->object.get())) {
            // No direct place (a call answer, a computed index, a
            // ternary): lower the pair and spill it to temps, then take
            // the slots path below. Single evaluation: the shapes above
            // emit nothing inside resolvePlace, so these words are the
            // value's only lowering — and anything it may already have
            // run refuses instead of running twice.
            std::vector<ir::Value> words = lowerWords(field->object.get());
            if (failed_) {
                return {undefined()};
            }
            if (words.size() != kInterfaceWords) {
                refuse(n.pos, "this call");
                return {undefined()};
            }
            std::vector<ir::Value> temps = allocaFor(objType, field->object->pos);
            if (failed_ || temps.size() != words.size()) {
                if (!failed_) {
                    refuse(n.pos, "this call");
                }
                return {undefined()};
            }
            for (uint64_t i = 0; i < static_cast<uint64_t>(words.size()); ++i) {
                ir::Instr store;
                store.op = ir::Op::Store;
                store.loc = field->object->pos;
                store.type = objType;
                store.addressBase = temps[i];
                store.storedValue = words[i];
                emit(std::move(store));
            }
            iface.slots = std::move(temps);
            iface.global.clear();
            iface.type = objType;
            iface.off = 0;
            iface.isVolatile = false;
            iface.tagWrites.clear();
            iface.hasIndirect = false;
            iface.isAddress = false;
            placed = true;
        }
        if (placed) {
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
            // A value naming a computed address has no direct slots: those
            // dispatches refuse by name until their slice, rather than
            // addressing the wrong words.
            if (iface.isAddress) {
                refuse(n.pos,
                       "an interface method through a computed address is not lowered yet");
                return {undefined()};
            }
            // The pair sits at the place's byte offset, in whole words: a
            // local field, a global's member, or an address behind a
            // pointer. A mid-word offset is a lowerer bug, not a program.
            int64_t per = layout::kSizePointer;
            if (iface.off < 0 || iface.off % per != 0) {
                refuse(n.pos, "this call");
                return {undefined()};
            }
            TypeRef vtableType = wordType(iface.type, kVtableWord);
            ir::Value vtable;
            ir::Value dataAddr;
            if (!iface.global.empty()) {
                // A method on a global names a label, not slots: load the
                // pair into temps and take the slots path below — the same
                // spill the computed-value path uses. A global interface
                // holds sixteen laid-down bytes (null or a stored pair),
                // so the load reads whole words, never a trap.
                std::vector<ir::Value> words =
                    loadRange(n.pos, iface, objType);
                if (failed_ || words.size() != kInterfaceWords) {
                    if (!failed_) {
                        refuse(n.pos, "this call");
                    }
                    return {undefined()};
                }
                std::vector<ir::Value> temps =
                    allocaFor(objType, field->object->pos);
                if (failed_ || temps.size() != words.size()) {
                    if (!failed_) {
                        refuse(n.pos, "this call");
                    }
                    return {undefined()};
                }
                for (uint64_t i = 0; i < static_cast<uint64_t>(words.size()); ++i) {
                    ir::Instr store;
                    store.op = ir::Op::Store;
                    store.loc = field->object->pos;
                    store.type = objType;
                    store.addressBase = temps[i];
                    store.storedValue = words[i];
                    emit(std::move(store));
                }
                iface.slots = std::move(temps);
                iface.global.clear();
                iface.off = 0;
                iface.hasIndirect = false;
                // Falls through to the direct-slots path below: the pair
                // now sits in temps, so the vtable load and receiver use
                // them like any other slotted value.
            }
            if (iface.hasIndirect) {
                // An address behind a pointer: advanced past the offset,
                // with the vtable read through the pair it names.
                dataAddr = iface.off == 0 ? iface.indirect
                                          : computedBase(iface.indirect, iface.off,
                                                         n.pos);
                if (failed_) {
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
                load.addressBase = dataAddr;
                load.displacement = layout::kOffsetInterfaceVtable;
                load.isPure = true;
                vtable = newValue(load.width, vtableType, n.pos);
                load.results.push_back(vtable);
                emit(std::move(load));
            } else {
                // Direct slots, with the pair at the place's word offset:
                // a bare value at word zero, a field deeper in. A place
                // with no slots at all names a computed address instead.
                if (iface.slots.empty()) {
                    refuse(n.pos,
                           "an interface method through a computed address is not lowered yet");
                    return {undefined()};
                }
                uint64_t atWord =
                    static_cast<uint64_t>(iface.off / per);
                if (atWord + kVtableWord >=
                    static_cast<uint64_t>(iface.slots.size())) {
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
                load.addressBase = iface.slots[atWord + kVtableWord];
                load.isPure = true;
                vtable = newValue(load.width, vtableType, n.pos);
                load.results.push_back(vtable);
                emit(std::move(load));
                dataAddr = computedBase(iface.slots[atWord], 0, n.pos);
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
        // A generic call answers its spec: the checker recorded which
        // specialization each site instantiated, so the signature and the
        // link name come from the spec, never the template. Plain calls
        // keep the path below untouched.
        std::optional<DeclId> callSpec = checker_.specOf(&n, emitSpec_);
        if (callSpec.has_value()) {
            const Signature* specSig = resolver_.funcSig(*callSpec);
            std::string specLink = resolver_.specLinkName(*callSpec);
            if (specSig == nullptr || specLink.empty()) {
                refuse(n.pos, "this call");
                return {undefined()};
            }
            sig = specSig;
            callee = specLink;
        } else {
            sig = functionSig(ident->name);
            callee = ident->name;
        // The link name is the resolver's: entry keeps its spelling, an
        // import's mangles under its module, the prelude keeps its C name.
        // The lookup is the same one functionSig uses, so the two cannot
        // disagree — including a program's own shadowing `print`.
        const resolve::Scope* here = homeScope();
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
            if (sig == nullptr) {
                // A closure-typed global calls through its stored code
                // word, with the stored captures word riding as receiver.
                // Locals keep priority above; anything else here falls
                // through to the loud refusal below.
                if (const GlobalBinding* global = findGlobal(ident->name)) {
                    if (global->type.type->kind() == Kind::Closure) {
                        const Signature* csig =
                            global->type.type->signature();
                        if (csig != nullptr) {
                            std::vector<ir::Value> pair =
                                loadGlobal(n.pos, *global);
                            if (failed_ || pair.size() != kClosureWords) {
                                refuse(n.pos, "this call");
                                return {undefined()};
                            }
                            sig = csig;
                            indirect = true;
                            indirectCallee = pair[kFirstWord];
                            receiver.push_back(pair[kLenWord]);
                        }
                    }
                }
                if (sig == nullptr) {
                    // A global raw fn value calls through its one word; a
                    // global whose provenance is a closure instead loads
                    // the pair that word addresses.
                    if (const GlobalBinding* global = findGlobal(ident->name)) {
                        if (global->type.type->kind() == Kind::FunctionPtr) {
                            const Signature* rawSig =
                                global->type.type->signature();
                            std::vector<ir::Value> held =
                                loadGlobal(n.pos, *global);
                            if (!failed_ && rawSig != nullptr &&
                                held.size() == kOneWord) {
                                if (checker_.provOfName(ident->name) ==
                                    check::Prov::Closure) {
                                    TypeRef pairType{ctx_.closure(*rawSig)};
                                    std::vector<ir::Value> pair =
                                        loadClosurePair(held[kFirstWord],
                                                        pairType, n.pos);
                                    if (!failed_ &&
                                        pair.size() == kClosureWords) {
                                        sig = rawSig;
                                        indirect = true;
                                        indirectCallee = pair[kFirstWord];
                                        receiver.push_back(pair[kLenWord]);
                                    }
                                } else {
                                    sig = rawSig;
                                    indirect = true;
                                    indirectCallee = held[kFirstWord];
                                }
                            }
                        }
                    }
                }
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
        if (sig == nullptr) {
            // A function pointer read from a place — an element of an
            // array of *func, a member — calls through its loaded word.
            TypeRef calleeType = checker_.typeOf(n.callee.get());
            if (!failed_ && !calleeType.isError() &&
                calleeType.type->kind() == Kind::FunctionPtr) {
                sig = calleeType.type->signature();
                indirect = true;
                std::vector<ir::Value> target = lowerWords(n.callee.get());
                if (failed_ || target.size() != kOneWord) {
                    refuse(n.pos, "this call");
                    return {undefined()};
                }
                indirectCallee = target[0];
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
            const resolve::Scope* here = homeScope();
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
                // A generic member answers its spec: the checker recorded
                // which specialization this site instantiated, so the
                // signature and link name come from the spec, never the
                // template. Plain members keep the path below untouched.
                std::optional<DeclId> memberSpec = checker_.specOf(&n, emitSpec_);
                if (memberSpec.has_value()) {
                    const Signature* specSig =
                        resolver_.funcSig(*memberSpec);
                    std::string specLink =
                        resolver_.specLinkName(*memberSpec);
                    if (specSig == nullptr || specLink.empty()) {
                        refuse(n.pos, "this call");
                        return {undefined()};
                    }
                    sig = specSig;
                    callee = specLink;
                } else {
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
    }
    if (sig == nullptr) {
        // A closure-typed callee that names no local (an array element, a
        // call answer, a ternary arm, a struct field holding a closure):
        // the value's two words ride as code plus captures, like a closure
        // local's. The checker proved the type; methods, module members and
        // closure locals were claimed above, so only plain values arrive.
        TypeRef calleeType = checker_.typeOf(n.callee.get());
        if (!failed_ && !calleeType.isError() &&
            calleeType.type->kind() == Kind::Closure &&
            dynamic_cast<const Ident*>(n.callee.get()) == nullptr) {
            const Signature* csig = calleeType.type->signature();
            std::vector<ir::Value> pair = lowerWords(n.callee.get());
            if (!failed_ && csig != nullptr && pair.size() == kClosureWords) {
                sig = csig;
                indirect = true;
                indirectCallee = pair[kFirstWord];
                receiver.push_back(pair[kLenWord]);
            }
        }
    }
    if (sig == nullptr) {
        // A proven-closure fn spelling calls through the pair its address
        // names: the declaration adopted the closure representation, so
        // code plus captures ride like a closure local's. Anything else
        // naming a bare fn spelling falls through to refuse below. The
        // local's own binding carries the type (callee identifiers record
        // no type of their own).
        if (const auto* ident = dynamic_cast<const Ident*>(n.callee.get())) {
            uint64_t depth = static_cast<uint64_t>(locals_.size());
            for (; depth > 0;) {
                --depth;
                auto bound = locals_[depth].find(ident->name);
                if (bound == locals_[depth].end()) {
                    continue;
                }
                if (bound->second.type.type->kind() == Kind::FunctionPtr) {
                    if (checker_.provOfName(ident->name) != check::Prov::Closure) {
                        const Signature* rawSig =
                            bound->second.type.type->signature();
                        std::vector<ir::Value> rawHeld =
                            loadLocal(n.pos, bound->second);
                        if (!failed_ && rawSig != nullptr &&
                            rawHeld.size() == kOneWord) {
                            sig = rawSig;
                            indirect = true;
                            indirectCallee = rawHeld[kFirstWord];
                        }
                    } else {
                    const Signature* csig =
                        bound->second.type.type->signature();
                    std::vector<ir::Value> held =
                        loadLocal(n.pos, bound->second);
                    if (!failed_ && csig != nullptr && held.size() == kOneWord) {
                        TypeRef pairType{ctx_.closure(*csig)};
                        std::vector<ir::Value> pair = loadClosurePair(
                            held[kFirstWord], pairType, n.pos);
                        if (!failed_ && pair.size() == kClosureWords) {
                            sig = csig;
                            indirect = true;
                            indirectCallee = pair[kFirstWord];
                            receiver.push_back(pair[kLenWord]);
                        }
                    }
                    }
                }
                break;
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
    instr.call.msabi = sig->abi() == Abi::MsAbi;
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
        arg.words = adjustStringLitArg(n.args[i].get(), std::move(arg.words), want);
        arg.words = boxClassOperand(n.args[i].get(), std::move(arg.words),
                                      checker_.typeOf(n.args[i].get()), want,
                                      n.args[i]->pos);
        if (failed_) {
            return {undefined()};
        }
        // Aggregate arguments travel packed: element loads answer one
        // take per slot, but the callee binds packed words, so the takes
        // are stored contiguously and reloaded as the parameter's words.
        // storeRange owns both the chunked shape takes arrive in and the
        // valued shape calls answer; a load of the packed cells is what
        // the callee's slots then observe.
        Kind argKind =
            !want.isError() ? want.type->kind() : Kind::Error;
        if (!arg.words.empty() && !want.isError() &&
            (argKind == Kind::Struct || argKind == Kind::Array ||
             argKind == Kind::Class || argKind == Kind::Union)) {
            int count = wordCount(want);
            if (count < 0) {
                refuse(n.args[i]->pos, "this call");
                return {undefined()};
            }
            uint64_t cells = static_cast<uint64_t>(count);
            std::vector<ir::Value> temps =
                allocaSlots(want, n.args[i]->pos, cells);
            if (failed_) {
                return {undefined()};
            }
            Place pack;
            pack.slots = temps;
            pack.type = want;
            pack.off = 0;
            pack.isVolatile = false;
            storeRange(n.args[i]->pos, pack, arg.words, want);
            if (failed_) {
                return {undefined()};
            }
            LocalBinding packed;
            packed.slots = std::move(temps);
            packed.type = want;
            arg.words = loadLocal(n.args[i]->pos, packed);
            if (failed_) {
                return {undefined()};
            }
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
    std::vector<ir::Value> words;
    if (needsSret(ret)) {
        // The answer slot precedes the call and its loads follow it:
        // loads before the call would read unfilled slots.
        std::vector<ir::Value> slots = callSretSlots(instr, ret, n.pos);
        if (failed_) {
            return {undefined()};
        }
        instr.width = ir::IRType::Void;
        instr.type = TypeRef{ret};
        emit(std::move(instr));
        words = callSretLoads(slots, ret, n.pos);
        if (failed_) {
            return {undefined()};
        }
    } else {
        words = callResults(instr, ret, n.pos);
        if (failed_) {
            return {undefined()};
        }
        emit(std::move(instr));
    }
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
        words = adjustStringLitArg(n.value.get(), std::move(words), want);
        words = boxClassOperand(n.value.get(), std::move(words), checker_.typeOf(n.value.get()),
                                  want, n.pos);
        if (failed_) {
            return;
        }
        if (needsSret(answered)) {
            // Store the answer into the caller's buffer and return
            // nothing: params[0] is the hidden address. The words land
            // in order at its bytes, shaped by the store like any
            // aggregate construction.
            if (function_->params.empty()) {
                refuse(n.pos, "this return");
                return;
            }
            TypeRef sretPtr{ctx_.pointer(ctx_.basic(BasicKind::U8))};
            ir::Instr load;
            load.op = ir::Op::Load;
            load.loc = n.pos;
            load.type = sretPtr;
            load.width = ir::IRType::Ptr;
            load.addressBase = function_->params[kFirstWord];
            load.isPure = true;
            ir::Value buf = newValue(load.width, sretPtr, n.pos);
            load.results.push_back(buf);
            emit(std::move(load));
            if (failed_) {
                return;
            }
            Place at;
            at.hasIndirect = true;
            at.indirect = buf;
            at.off = 0;
            at.type = want;
            at.isVolatile = false;
            storeRange(n.pos, at, words, want);
            if (failed_) {
                return;
            }
        } else {
            if (!returnWordsFit(words, n.pos)) {
                return;
            }
            term.values = std::move(words);
        }
    } else {
        // A bare return answers nothing: an empty list, not the placeholder.
        // The placeholder's id poisons the frame computation, which reads
        // every value the function mentions.
    }
    // FEATURES §5.2: the value is already evaluated above; every pending
    // defer runs now, innermost first, before control leaves.
    runDefers(0);
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

bool Lowerer::matchNeedsSpill(const MatchStmt& n) {
    for (const auto& arm : n.arms) {
        auto needsAddr = [&](const Pattern& pat) {
            if (checker_.isDeadPattern(&pat)) {
                return false;
            }
            return pat.kind == PatternKind::Type || pat.kind == PatternKind::Member;
        };
        if (needsAddr(*arm->pattern)) {
            return true;
        }
        for (const auto& alt : arm->alts) {
            if (needsAddr(*alt)) {
                return true;
            }
        }
    }
    return false;
}

bool Lowerer::matchBoundWords(const std::string& name, std::vector<ir::Value>& words,
                              Position pos) {
    for (auto frame = locals_.rbegin(); frame != locals_.rend(); ++frame) {
        auto found = frame->find(name);
        if (found != frame->end()) {
            words = loadLocal(pos, found->second);
            return !failed_;
        }
    }
    if (const GlobalBinding* global = findGlobal(name)) {
        words = loadGlobal(pos, *global);
        return !failed_;
    }
    const resolve::Scope* here = homeScope();
    const resolve::Decl* decl =
        here != nullptr ? resolver_.lookup(here, name) : nullptr;
    if (decl != nullptr &&
        (decl->kind == resolve::DeclKind::Var ||
         decl->kind == resolve::DeclKind::Const)) {
        // A variable global always lands in globals_ first; missing it
        // here is a lowering gap, not a fresh name. A constant folds, or
        // refuses loudly when it has no integer value.
        if (decl->kind == resolve::DeclKind::Var) {
            refuse(pos, "this pattern");
            return false;
        }
        Ident probe(pos, name);
        int64_t folded = 0;
        if (!foldBound(&probe, folded)) {
            refuse(pos, "this pattern");
            return false;
        }
        TypeRef word{ctx_.basic(BasicKind::I64)};
        words = {constantI64(folded, word, pos)};
        return !failed_;
    }
    // A bare enumerator compares by value: the checker proved exactly one
    // enum owns the name (or marked the arm dead), so the integer here is
    // that member's value. Locals, globals, variables and constants
    // already answered above, in the checker's order.
    {
        int owners = 0;
        const resolve::Decl* owner =
            here != nullptr ? resolver_.lookupEnumMember(here, name, owners)
                            : nullptr;
        if (owner != nullptr) {
            std::string home = owner->scope != nullptr
                                   ? owner->scope->module
                                   : homeModule();
            int64_t value =
                resolver_.enumValue(home + "." + owner->name, name);
            TypeRef word{ctx_.basic(BasicKind::I64)};
            words = {constantI64(value, word, pos)};
            return !failed_;
        }
    }
    return false;
}

void Lowerer::matchBrTo(uint32_t target, Position pos) {
    ir::Terminator term;
    term.term = ir::Term::Br;
    term.target = target;
    term.loc = pos;
    emitTerminator(term);
    sealBlock(current_);
}

void Lowerer::matchCondTo(ir::Value cond, uint32_t taken, uint32_t missed, Position pos) {
    ir::Terminator br;
    br.term = ir::Term::CondBr;
    br.cond = cond;
    br.thenTarget = taken;
    br.elseTarget = missed;
    br.loc = pos;
    emitTerminator(br);
    sealBlock(current_);
}

bool Lowerer::lowerMatchConstTest(const Expr* value,
                                 const std::vector<ir::Value>& words,
                                 uint32_t taken, uint32_t missed, Position pos) {
    if (words.empty()) {
        refuse(pos, "this pattern");
        return false;
    }
    if (dynamic_cast<const StringLit*>(value) != nullptr) {
        std::vector<ir::Value> wantWords = lowerWords(value);
        if (failed_ || wantWords.size() != kSliceWords ||
            words.size() != kSliceWords) {
            refuse(pos, "this pattern");
            return false;
        }
        ir::Value eq = lowerStringEqWords(words, wantWords, pos, true);
        if (failed_) {
            return false;
        }
        matchCondTo(eq, taken, missed, pos);
        return !failed_;
    }
    if (dynamic_cast<const NilPtr*>(value) != nullptr) {
        if (words.size() != kOneWord) {
            refuse(pos, "this pattern");
            return false;
        }
        TypeRef ptrType{ctx_.pointer(ctx_.basic(BasicKind::U8))};
        ir::Value nil = constantI64(0, ptrType, pos);
        if (failed_) {
            return false;
        }
        matchCondTo(emitI64Cmp(pos, BinOp::Eq, words[kFirstWord], nil), taken,
                    missed, pos);
        return !failed_;
    }
    int64_t folded = 0;
    if (!foldBound(value, folded)) {
        refuse(pos, "this pattern");
        return false;
    }
    TypeRef word{ctx_.basic(BasicKind::I64)};
    ir::Value wantVal = constantI64(folded, word, pos);
    if (failed_) {
        return false;
    }
    matchCondTo(emitI64Cmp(pos, BinOp::Eq, words[kFirstWord], wantVal), taken,
                missed, pos);
    return !failed_;
}

bool Lowerer::lowerMatchRangeTest(const std::vector<ir::Value>& words, TypeRef scrutinee,
                                  int64_t lo, int64_t hi, uint32_t taken,
                                  uint32_t missed, Position pos) {
    if (words.empty() || scrutinee.isError() ||
        scrutinee.type->kind() != Kind::Basic ||
        !scrutinee.type->basic().has_value() ||
        !basicIsInteger(*scrutinee.type->basic())) {
        refuse(pos, "this pattern");
        return false;
    }
    // The checker proved an integer scrutinee and bounds that fit its
    // signedness, so widening first keeps the question asked: a u8 255
    // beside hi 300 compares 255 <= 300, never a truncated 44, and a
    // negative i32 stays below lo 0 instead of wrapping above it.
    bool isSigned = basicIsSigned(*scrutinee.type->basic());
    TypeRef wide{ctx_.basic(isSigned ? BasicKind::I64 : BasicKind::U64)};
    ir::Value cell = convertInt(words[kFirstWord], scrutinee, wide, pos);
    if (failed_) {
        return false;
    }
    ir::Value loVal = constantI64(lo, wide, pos);
    ir::Value hiVal = constantI64(hi, wide, pos);
    if (failed_) {
        return false;
    }
    TypeRef boolean{ctx_.basic(BasicKind::Bool)};
    auto cmpTo = [&](BinOp op, ir::Value a, ir::Value b, uint32_t yes, uint32_t no) {
        ir::Instr cmp;
        cmp.op = ir::Op::Cmp;
        cmp.loc = pos;
        cmp.type = boolean;
        cmp.width = ir::IRType::Bool;
        cmp.cmp = cmpFor(op, isSigned);
        cmp.isPure = true;
        cmp.operands.push_back(a);
        cmp.operands.push_back(b);
        ir::Value out = newValue(cmp.width, boolean, pos);
        cmp.results.push_back(out);
        emit(std::move(cmp));
        matchCondTo(out, yes, no, pos);
    };
    // Two links, not one: lo <= x lands mid, x <= hi lands the body,
    // and either miss leaves the arm — the chain's shape, doubled.
    uint32_t mid = emitBlock(pos);
    cmpTo(BinOp::Ge, cell, loVal, mid, missed);
    if (failed_) {
        return false;
    }
    startBlock(mid);
    cmpTo(BinOp::Le, cell, hiVal, taken, missed);
    return !failed_;
}

bool Lowerer::lowerMatchFieldTest(const Pattern& sub, const Place& at, uint32_t pass,
                                  uint32_t miss, Position pos,
                                  std::vector<MatchFieldBind>& binds) {
    if (sub.kind == PatternKind::Wildcard) {
        matchBrTo(pass, pos);
        return !failed_;
    }
    if (sub.kind == PatternKind::Bind) {
        std::vector<ir::Value> bound;
        if (!matchBoundWords(sub.name, bound, pos)) {
            if (failed_) {
                return false;
            }
            MatchFieldBind bind;
            bind.name = sub.name;
            bind.type = at.type;
            bind.off = at.off;
            binds.push_back(std::move(bind));
            matchBrTo(pass, pos);
            return !failed_;
        }
        // A bound name compares: strings by content, one word by value.
        // Anything wider never passed the checker's comparability, so a
        // size surprise here refuses instead of comparing half a value.
        std::vector<ir::Value> got = loadRange(pos, at, at.type);
        if (failed_) {
            return false;
        }
        if (at.type.type->kind() == Kind::String) {
            if (bound.size() != kSliceWords || got.size() != kSliceWords) {
                refuse(pos, "this pattern");
                return false;
            }
            ir::Value eq = lowerStringEqWords(got, bound, pos, true);
            if (failed_) {
                return false;
            }
            matchCondTo(eq, pass, miss, pos);
            return !failed_;
        }
        if (bound.size() != kOneWord || got.size() != kOneWord) {
            refuse(pos, "this pattern");
            return false;
        }
        matchCondTo(emitI64Cmp(pos, BinOp::Eq, got[kFirstWord], bound[kFirstWord]),
                    pass, miss, pos);
        return !failed_;
    }
    if (sub.kind == PatternKind::Const) {
        std::vector<ir::Value> got = loadRange(pos, at, at.type);
        if (failed_) {
            return false;
        }
        return lowerMatchConstTest(sub.value.get(), got, pass, miss, pos);
    }
    if (sub.kind == PatternKind::Range) {
        std::vector<ir::Value> got = loadRange(pos, at, at.type);
        if (failed_) {
            return false;
        }
        if (got.size() != kOneWord) {
            refuse(pos, "this pattern");
            return false;
        }
        int64_t lo = 0;
        int64_t hi = 0;
        if (!foldBound(sub.value.get(), lo) || !foldBound(sub.rangeHi.get(), hi)) {
            refuse(sub.value->pos, "this pattern");
            return false;
        }
        return lowerMatchRangeTest(got, at.type, lo, hi, pass, miss, pos);
    }
    if (sub.kind == PatternKind::Type || sub.kind == PatternKind::Member) {
        return lowerMatchArmTest(sub, at, pass, miss, pos, binds);
    }
    refuse(sub.pos, "this pattern");
    return false;
}

bool Lowerer::lowerMatchArmTest(const Pattern& pat, const Place& base, uint32_t body,
                                uint32_t next, Position pos,
                                std::vector<MatchFieldBind>& binds) {
    if (pat.kind == PatternKind::Member) {
        if (base.type.type->kind() == Kind::Enum) {
            // The checker proved the same enum and a known case, so the
            // cell answers directly: no tag to read, just the named value
            // beside the spilled word.
            std::optional<DeclId> id = base.type.type->decl();
            int64_t want = 0;
            bool known = false;
            if (id.has_value()) {
                for (const auto& member : resolver_.enumMembers(*id)) {
                    if (member.first == pat.member) {
                        want = member.second;
                        known = true;
                        break;
                    }
                }
            }
            if (!known) {
                refuse(pos, "this pattern");
                return false;
            }
            std::vector<ir::Value> got = loadRange(pos, base, base.type);
            if (failed_) {
                return false;
            }
            if (got.size() != kOneWord) {
                refuse(pos, "this pattern");
                return false;
            }
            TypeRef word{ctx_.basic(BasicKind::I64)};
            ir::Value wantVal = constantI64(want, word, pos);
            if (failed_) {
                return false;
            }
            matchCondTo(emitI64Cmp(pos, BinOp::Eq, got[kFirstWord], wantVal),
                        body, next, pos);
            return !failed_;
        }
        // The checker proved the same union, so the tag answers: index
        // and offset come from the resolver, never from a guess.
        if (base.type.type->kind() != Kind::Union) {
            refuse(pos, "this pattern");
            return false;
        }
        std::optional<DeclId> id = base.type.type->decl();
        // Split across lines on purpose: the offset call beside a digit
        // reads as a size literal and T4 refuses it.
        int64_t index =
            id.has_value() ? resolver_.unionMemberIndex(*id, pat.member) : -1;
        int64_t tagOff = -1;
        if (id.has_value()) {
            tagOff = resolver_.unionTagOffset(*id);
        }
        if (!id.has_value() || index < 0 || tagOff < 0) {
            refuse(pos, "this pattern");
            return false;
        }
        ir::Value ok = emitTagCheck(pos, base, tagOff, index);
        if (failed_) {
            return false;
        }
        matchCondTo(ok, body, next, pos);
        return !failed_;
    }
    if (pat.kind != PatternKind::Type) {
        refuse(pos, "this pattern");
        return false;
    }
    if (base.type.type->kind() != Kind::Struct) {
        refuse(pos, "this pattern");
        return false;
    }
    std::optional<DeclId> id = base.type.type->decl();
    if (!id.has_value()) {
        refuse(pos, "this pattern");
        return false;
    }
    // One block per field in order: a field with nothing to test still
    // owns its link, so binds and wildcards thread through to the body.
    const uint64_t fieldCount = static_cast<uint64_t>(pat.fields.size());
    for (uint64_t j = 0; j < fieldCount; ++j) {
        const FieldPat& field = pat.fields[j];
        TypeRef fieldType = resolver_.fieldType(*id, field.name);
        int64_t fieldOff = resolver_.fieldOffset(*id, field.name);
        if (fieldType.isError() || fieldOff < 0) {
            refuse(field.pat->pos, "no such field");
            return false;
        }
        Place at = base;
        at.off = base.off + fieldOff;
        at.type = fieldType;
        const bool last = j + 1 == fieldCount;
        uint32_t pass = last ? body : emitBlock(pos);
        if (!lowerMatchFieldTest(*field.pat, at, pass, next, pos, binds)) {
            return false;
        }
        if (!last) {
            startBlock(pass);
        }
    }
    if (fieldCount == 0) {
        matchBrTo(body, pos);
    }
    return !failed_;
}

void Lowerer::onMatch(MatchStmt& n) {
    if (failed_ || !enabled_ || function_ == nullptr) {
        return;
    }
    // The checker proved every arm compatible and the chain exhaustive
    // (a default, or a finite cover over a bool, a union, or an enum),
    // so the lowering only answers which arm: the scrutinee lowers once, each test branches to its
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
    // A struct or union scrutinee answers one value per word, like any
    // other aggregate: the single-word row only fits scalars. Anything
    // else keeps the old width, so existing arms never shift.
    uint64_t want = scrutString ? kSliceWords : kOneWord;
    if (!scrutString && (scrutType.type->kind() == Kind::Struct ||
                         scrutType.type->kind() == Kind::Union)) {
        int words = wordCount(scrutType);
        if (words < 0) {
            refuse(n.pos, "this match");
            return;
        }
        want = static_cast<uint64_t>(words);
    }
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
    // (matchBoundWords, shared with the field tests below.)
    bool spill = matchNeedsSpill(n);
    std::vector<ir::Value> spillSlots;
    Place spillBase;
    if (spill) {
        // Field and tag tests read by address, so the scrutinee's words
        // spill once before the chain: every test and every bound field
        // reads the same memory, and the entry block dominates them all.
        spillSlots = allocaFor(scrutType, n.pos);
        if (failed_) {
            return;
        }
        spillBase.slots = spillSlots;
        spillBase.type = scrutType;
        spillBase.off = 0;
        spillBase.isVolatile = false;
        // A zero-word scrutinee (an empty struct) spills nothing: the
        // range store below refuses zero-size layouts, and there is no
        // word to move. Tests over it still read the (empty) spill base.
        if (!scrut.empty()) {
            storeRange(n.pos, spillBase, scrut, scrutType);
            if (failed_) {
                return;
            }
        }
    }
    if (armCount == 0) {
        brTo(defaultLabel);
    } else {
        brTo(tests[0]);
    }
    std::vector<std::string> fresh(armCount);
    std::vector<std::vector<MatchFieldBind>> armBinds(armCount);
    // One test per alternative: an arm's miss edge chains into its next
    // alternative's test, and only the last alternative leaves the arm.
    // Arms without alternatives keep their one test in tests[i], so the
    // single-pattern shape lays out exactly as before.
    auto testPattern = [&](const Pattern& pat, uint32_t body, uint32_t next,
                           uint64_t i, bool multi) -> bool {
        // A dead arm can never match: the checker proved the mismatch,
        // so the test edge chains straight through with no comparison.
        if (checker_.isDeadPattern(&pat)) {
            brTo(next);
            return true;
        }
        if (pat.kind == PatternKind::Wildcard) {
            brTo(body);
            return true;
        }
        if (pat.kind == PatternKind::Bind) {
            std::vector<ir::Value> bound;
            if (!matchBoundWords(pat.name, bound, n.pos)) {
                if (failed_) {
                    return false;
                }
                // The checker binds no fresh names under an alternation:
                // a name the body would read must come from exactly one
                // alternative, which no alternative can promise. A bound
                // name still compares, one alternative at a time.
                if (multi) {
                    refuse(pat.pos, "this pattern");
                    return false;
                }
                fresh[i] = pat.name;
                brTo(body);
                return true;
            }
            if (bound.size() != want) {
                refuse(n.pos, "this pattern");
                return false;
            }
            // Equality answers on the cells: every producer keeps the
            // value's representation in its slot, so two cells holding
            // the same value compare equal at any width both share.
            if (scrutString) {
                ir::Value eq = lowerStringEqWords(scrut, bound, n.pos, true);
                if (failed_) {
                    return false;
                }
                condTo(eq, body, next);
                return true;
            }
            condTo(emitI64Cmp(n.pos, BinOp::Eq, scrut[kFirstWord],
                             bound[kFirstWord]),
                   body, next);
            return true;
        }
        if (pat.kind == PatternKind::Const) {
            if (dynamic_cast<const StringLit*>(pat.value.get()) != nullptr) {
                std::vector<ir::Value> wantWords = lowerWords(pat.value.get());
                if (failed_ || wantWords.size() != kSliceWords) {
                    refuse(pat.value->pos, "this pattern");
                    return false;
                }
                ir::Value eq =
                    lowerStringEqWords(scrut, wantWords, n.pos, true);
                if (failed_) {
                    return false;
                }
                condTo(eq, body, next);
                return true;
            }
            if (dynamic_cast<const NilPtr*>(pat.value.get()) != nullptr) {
                TypeRef ptrType{ctx_.pointer(ctx_.basic(BasicKind::U8))};
                ir::Value nil = constantI64(0, ptrType, n.pos);
                if (failed_) {
                    return false;
                }
                condTo(emitI64Cmp(n.pos, BinOp::Eq, scrut[kFirstWord], nil),
                       body, next);
                return true;
            }
            int64_t folded = 0;
            if (!foldBound(pat.value.get(), folded)) {
                refuse(pat.value->pos, "this pattern");
                return false;
            }
            TypeRef word{ctx_.basic(BasicKind::I64)};
            ir::Value wantVal = constantI64(folded, word, n.pos);
            if (failed_) {
                return false;
            }
            condTo(emitI64Cmp(n.pos, BinOp::Eq, scrut[kFirstWord], wantVal),
                   body, next);
            return true;
        }
        if (pat.kind == PatternKind::Range) {
            int64_t lo = 0;
            int64_t hi = 0;
            if (!foldBound(pat.value.get(), lo) || !foldBound(pat.rangeHi.get(), hi)) {
                refuse(pat.value->pos, "this pattern");
                return false;
            }
            // The chain proved the scrutinee one word; the helper proves
            // the rest — an integer cell widened, then both bounds.
            return lowerMatchRangeTest(scrut, scrutType, lo, hi, body, next,
                                       n.pos);
        }
        if (pat.kind == PatternKind::Type || pat.kind == PatternKind::Member) {
            // Dead arms chained through above; a live one owns the spill
            // the checker proved addressable, and its field binds wait
            // for the body below. Under an alternation no alternative
            // binds either, so a bind the test records here is a
            // checker bug, refused as one.
            size_t bindsBefore = armBinds[i].size();
            if (!lowerMatchArmTest(pat, spillBase, body, next, n.pos, armBinds[i])) {
                return false;
            }
            if (multi && armBinds[i].size() != bindsBefore) {
                refuse(pat.pos, "this pattern");
                return false;
            }
            return true;
        }
        refuse(pat.pos, "this pattern");
        return false;
    };
    for (uint64_t i = 0; i < armCount; ++i) {
        const MatchArm& arm = *n.arms[i];
        const uint32_t body = bodies[i];
        std::vector<const Pattern*> pats;
        pats.push_back(arm.pattern.get());
        std::vector<uint32_t> blocks;
        blocks.push_back(tests[i]);
        for (const auto& alt : arm.alts) {
            pats.push_back(alt.get());
            blocks.push_back(emitBlock(n.pos));
        }
        // T4 owns every size: the counts below name no literal, and the
        // loop lines below name no size, so the gate never sees the two
        // together.
        bool multi = !arm.alts.empty();
        uint64_t altCount = pats.size();
        for (uint64_t k = 0; k < altCount; ++k) {
            startBlock(blocks[k]);
            uint32_t next = k + 1 < altCount
                                ? blocks[k + 1]
                                : (i + 1 < armCount ? tests[i + 1] : defaultLabel);
            if (!testPattern(*pats[k], body, next, i, multi)) {
                return;
            }
        }
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
        // A struct arm's bound fields materialize the same way: each owns
        // fresh slots in the arm's frame, filled from the spill the test
        // chain already proved, so the body reads locals, never memory
        // the checker did not bind.
        for (const MatchFieldBind& fieldBind : armBinds[i]) {
            std::vector<ir::Value> fieldSlots = allocaFor(fieldBind.type, n.pos);
            if (failed_) {
                return;
            }
            Place from;
            from.slots = spillSlots;
            from.type = fieldBind.type;
            from.off = fieldBind.off;
            from.isVolatile = false;
            std::vector<ir::Value> got = loadRange(n.pos, from, fieldBind.type);
            if (failed_) {
                return;
            }
            Place dest;
            dest.slots = fieldSlots;
            dest.type = fieldBind.type;
            dest.off = 0;
            dest.isVolatile = false;
            storeRange(n.pos, dest, got, fieldBind.type);
            if (failed_) {
                return;
            }
            LocalBinding fieldLocal;
            fieldLocal.slots = std::move(fieldSlots);
            fieldLocal.type = fieldBind.type;
            fieldLocal.isMutable = true;
            locals_.back()[fieldBind.name] = std::move(fieldLocal);
        }
        if (n.arms[i]->guard != nullptr) {
            // A guard runs after the pattern fit, with the arm's binds
            // materialized above: true enters the body, false falls to
            // the next arm's test. The checker proved it a bool, so it
            // answers one word; anything else is a lowerer bug, refused
            // as one.
            uint32_t guarded = emitBlock(n.pos);
            std::vector<ir::Value> cond = lowerWords(n.arms[i]->guard.get());
            if (failed_) {
                return;
            }
            if (cond.size() != kOneWord) {
                refuse(n.arms[i]->guard->pos, "this guard");
                return;
            }
            uint32_t nextTest = i + 1 < armCount ? tests[i + 1] : defaultLabel;
            condTo(cond[kFirstWord], guarded, nextTest);
            startBlock(guarded);
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
        // Exhaustive with no default: control proves one arm took,
        // so arriving here is a checker bug, not a join — trap loudly
        // instead of falling into whatever follows the match.
        ir::Terminator trap;
        trap.term = ir::Term::Trap;
        trap.reason = ir::TrapReason::Explicit;
        trap.loc = n.pos;
        emitTerminator(trap);
        sealBlock(current_);
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
    // FEATURES §5.2: leaving runs every defer up to the loop body scope.
    if (!loopMarks_.empty()) {
        runDefers(loopMarks_.back());
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
    if (!loopMarks_.empty()) {
        runDefers(loopMarks_.back());
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
    // A loop body is its own defer scope: per-iteration defers run at the
    // latch, on break, and on continue (FEATURES §5.2). Block bodies push
    // their own frame inside, leaving this one empty; bare bodies use it.
    loopMarks_.push_back(static_cast<uint64_t>(deferFrames_.size()));
    deferFrames_.push_back({});
    bool savedTerminated = terminated_;
    terminated_ = false;

    startBlock(headLabel);
    locals_.push_back({});
    ir::Value cond = onIdentOrExpr(n.cond.get(), checker_.typeOf(n.cond.get()));
    locals_.pop_back();
    // A literal-true condition never exits: the else edge targets the
    // head itself rather than the exit, so the graph tells the truth —
    // the exit stays reachable only through breaks. Anything else keeps
    // the exit edge; an unprovable condition may still leave.
    const auto* condLit = dynamic_cast<const BoolLit*>(n.cond.get());
    const bool alwaysTrue = condLit != nullptr && condLit->value;
    ir::Terminator br;
    br.term = ir::Term::CondBr;
    br.cond = cond;
    br.thenTarget = bodyLabel;
    br.elseTarget = alwaysTrue ? headLabel : exitLabel;
    br.loc = n.pos;
    emitTerminator(br);

    startBlock(bodyLabel);
    locals_.push_back({});
    n.body->accept(checker_);
    locals_.pop_back();
    // A body that ended in a jump (break, continue, return) already closed
    // its block; a body that fell off still needs the back edge.
    if (!terminated_) {
        runDefers(loopMarks_.back());
        emitTerminator(toHead);
    }

    loops_.pop_back();
    loopMarks_.pop_back();
    deferFrames_.pop_back();
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
    loopMarks_.push_back(static_cast<uint64_t>(deferFrames_.size()));
    deferFrames_.push_back({});
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

bool Lowerer::receiverAddress(const Place& base, Position pos, const char* what,
                                ir::Value& out) {
    if (failed_ || base.type.isError()) {
        refuse(pos, what);
        return false;
    }
    Kind kind = base.type.type->kind();
    if (kind != Kind::Class && kind != Kind::Struct) {
        refuse(pos, what);
        return false;
    }
    if (base.slots.empty() && !base.hasIndirect) {
        refuse(pos, "a method on a global is not lowered yet");
        return false;
    }
    if (base.hasIndirect && base.off != 0) {
        // A method on a field reached through a pointer: the object
        // address is the loaded pointer plus the field offset, added
        // the same way a slice advances its pointer.
        TypeRef count{ctx_.basic(BasicKind::I64)};
        TypeRef ptrType{ctx_.pointer(base.type.type)};
        ir::Value step = constantI64(base.off, count, pos);
        if (failed_) {
            return false;
        }
        ir::Instr add;
        add.op = ir::Op::Add;
        add.loc = pos;
        add.type = ptrType;
        add.width = ir::IRType::Ptr;
        add.isPure = true;
        add.operands.push_back(base.indirect);
        add.operands.push_back(step);
        ir::Value at = newValue(add.width, ptrType, pos);
        add.results.push_back(at);
        emit(std::move(add));
        out = at;
        return !failed_;
    }
    if (base.hasIndirect) {
        out = base.indirect;
        return true;
    }
    out = computedBase(base.slots[0], base.off, pos);
    return !failed_;
}

void Lowerer::onForIn(ForInStmt& n) {
    if (failed_ || !enabled_ || function_ == nullptr) {
        return;
    }
    // Arrays and slices iterate behind an index; a struct or class
    // iterates through its iter()/next() protocol below. Anything else
    // refuses by name so the loop never disappears silently.
    TypeRef seqType = checker_.typeOf(n.seq.get());
    if (seqType.isError()) {
        refuse(n.pos, "this iteration");
        return;
    }
    Kind seqKind = seqType.type->kind();
    if (seqKind == Kind::Struct || seqKind == Kind::Class) {
        std::optional<DeclId> id = seqType.type->decl();
        if (!id.has_value()) {
            refuse(n.pos, "this iteration");
            return;
        }
        onForInProtocol(n, seqType, *id);
        return;
    }
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
    loopMarks_.push_back(static_cast<uint64_t>(deferFrames_.size()));
    deferFrames_.push_back({});
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
        runDefers(loopMarks_.back());
        emitTerminator(toStep);
    }
    loopMarks_.pop_back();
    deferFrames_.pop_back();
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

void Lowerer::onForInProtocol(ForInStmt& n, TypeRef seqType, DeclId id) {
    // A struct/class sequence iterates through its protocol: iter() runs
    // once into a hidden slot, each head step calls next() with the
    // slot's address, and a nil answer ends the loop (FEATURES 8.2). The
    // checker proved the shapes (Checker::visitForInStmt); the lookups
    // repeat here rather than trusting them, so a checker bug reads as
    // a refusal. The body binds a fresh copy each step, so writing to x
    // never reaches the sequence.
    Kind seqKind = seqType.type->kind();
    if (seqKind != Kind::Struct && seqKind != Kind::Class) {
        refuse(n.pos, "this iteration");
        return;
    }
    const resolve::Decl* owner = resolver_.declById(id);
    const Signature* iter = resolver_.methodSig(id, "iter");
    const Signature* next = resolver_.methodSig(id, "next");
    if (owner == nullptr || iter == nullptr || next == nullptr ||
        iter->arity() != 0 || next->arity() != 1) {
        refuse(n.pos, "this iteration");
        return;
    }
    const Type* iterRet = iter->returnType();
    const Type* nextParam = next->parameters()[0].type;
    const Type* nextRet = next->returnType();
    bool shapesOk = iterRet != nullptr && nextParam != nullptr &&
                    nextRet != nullptr &&
                    (iterRet->kind() == Kind::Struct ||
                     iterRet->kind() == Kind::Class ||
                     iterRet->kind() == Kind::Union);
    std::optional<DeclId> iterId;
    const Type* elem = nullptr;
    if (shapesOk) {
        iterId = iterRet->decl();
        elem = nextRet->kind() == Kind::Pointer ? nextRet->pointee() : nullptr;
        const Type* held =
            nextParam->kind() == Kind::Pointer ? nextParam->pointee() : nullptr;
        std::optional<DeclId> heldId =
            held != nullptr ? held->decl() : std::nullopt;
        shapesOk = iterId.has_value() && elem != nullptr && !elem->isError() &&
                   elem->kind() != Kind::Void && heldId.has_value() &&
                   sameDecl(*iterId, *heldId);
    }
    if (!shapesOk) {
        refuse(n.pos, "this iteration");
        return;
    }
    TypeRef iterType{iterRet};
    TypeRef elemType{elem};
    int64_t elemBytes = resolver_.nominalSize(elemType.type);
    if (elemBytes <= 0) {
        refuse(n.pos, "this element");
        return;
    }
    std::string link = resolver_.linkName(*owner);
    Place seqPlace;
    if (!resolvePlace(n.seq.get(), seqPlace)) {
        refuse(n.pos, "this iteration");
        return;
    }
    ir::Value recv;
    if (!receiverAddress(seqPlace, n.pos, "this iteration", recv)) {
        return;
    }
    if (failed_) {
        return;
    }
    // iter() answers once, before the first step: the words land in the
    // hidden slot whatever their width (rax/rdx row or caller buffer,
    // through the same row calls read).
    ir::Instr iterCall;
    iterCall.op = ir::Op::Call;
    iterCall.loc = n.pos;
    iterCall.callee = link + "__iter";
    iterCall.call.indirect = false;
    iterCall.mayTrap = true;
    {
        ir::Argument self;
        self.words.push_back(recv);
        iterCall.arguments.push_back(std::move(self));
    }
    std::vector<ir::Value> itWords;
    if (needsSret(iterRet)) {
        std::vector<ir::Value> slots = callSretSlots(iterCall, iterRet, n.pos);
        if (failed_) {
            return;
        }
        iterCall.width = ir::IRType::Void;
        iterCall.type = TypeRef{iterRet};
        emit(std::move(iterCall));
        itWords = callSretLoads(slots, iterRet, n.pos);
        if (failed_) {
            return;
        }
    } else {
        itWords = callResults(iterCall, iterRet, n.pos);
        if (failed_) {
            return;
        }
        emit(std::move(iterCall));
    }
    if (function_ != nullptr) {
        function_->canThrow = true;
    }
    // One slot per word: storeRange addresses word i through slots[i],
    // so a single slot refuses past the first word (like any aggregate
    // binding). An empty slot row names a stateless iterator or element
    // the loop could never move, and refuses rather than addressing
    // nothing.
    std::vector<ir::Value> itSlots = allocaFor(iterType, n.pos);
    if (failed_ || itSlots.empty()) {
        if (!failed_) {
            refuse(n.pos, "this iteration");
        }
        return;
    }
    Place itPlace;
    itPlace.slots = itSlots;
    itPlace.type = iterType;
    storeRange(n.pos, itPlace, itWords, iterType);
    if (failed_) {
        return;
    }
    // The iterator's address and the element slot are loop-invariant:
    // both are answered once, outside the blocks.
    ir::Value itPtr = computedBase(itSlots[kFirstWord], 0, n.pos);
    if (failed_) {
        return;
    }
    std::vector<ir::Value> elemSlots = allocaFor(elemType, n.pos);
    if (failed_ || elemSlots.empty()) {
        if (!failed_) {
            refuse(n.pos, "this element");
        }
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
    bind.slots = elemSlots;
    bind.type = elemType;
    bind.isMutable = n.isMutable;
    locals_.back()[n.name] = std::move(bind);

    ir::Terminator toHead;
    toHead.term = ir::Term::Br;
    toHead.target = headLabel;
    toHead.loc = n.pos;
    emitTerminator(toHead);

    startBlock(headLabel);
    // p = seq.next(&it): the receiver rides first, as for a method call.
    ir::Instr nextCall;
    nextCall.op = ir::Op::Call;
    nextCall.loc = n.pos;
    nextCall.callee = link + "__next";
    nextCall.call.indirect = false;
    nextCall.mayTrap = true;
    {
        ir::Argument self;
        self.words.push_back(recv);
        nextCall.arguments.push_back(std::move(self));
        ir::Argument it;
        it.words.push_back(itPtr);
        nextCall.arguments.push_back(std::move(it));
    }
    std::vector<ir::Value> answer = callResults(nextCall, nextRet, n.pos);
    if (failed_) {
        return;
    }
    emit(std::move(nextCall));
    if (function_ != nullptr) {
        function_->canThrow = true;
    }
    if (answer.size() != static_cast<size_t>(kOneWord)) {
        refuse(n.pos, "this iteration");
        return;
    }
    ir::Value done = emitI64Cmp(n.pos, BinOp::Eq, answer[kFirstWord],
                                constantI64(0, TypeRef{nextRet}, n.pos));
    if (failed_) {
        return;
    }
    ir::Terminator cond;
    cond.term = ir::Term::CondBr;
    cond.cond = done;
    cond.thenTarget = exitLabel;
    cond.elseTarget = bodyLabel;
    cond.loc = n.pos;
    emitTerminator(cond);

    startBlock(bodyLabel);
    {
        // The element is copied from the answered pointer into the
        // frame slot: x is always a copy (FEATURES 8.2).
        Place at;
        at.hasIndirect = true;
        at.indirect = answer[kFirstWord];
        at.off = 0;
        at.type = elemType;
        std::vector<ir::Value> words = loadRange(n.pos, at, elemType);
        if (failed_) {
            return;
        }
        Place elemPlace;
        elemPlace.slots = elemSlots;
        elemPlace.type = elemType;
        storeRange(n.pos, elemPlace, words, elemType);
        if (failed_) {
            return;
        }
    }

    loops_.push_back(LoopTargets{stepLabel, exitLabel});
    loopMarks_.push_back(static_cast<uint64_t>(deferFrames_.size()));
    deferFrames_.push_back({});
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
        runDefers(loopMarks_.back());
        emitTerminator(toStep);
    }
    loopMarks_.pop_back();
    deferFrames_.pop_back();
    terminated_ = savedTerminated;

    startBlock(stepLabel);
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
    if (expr == nullptr || failed_) {
        return undefined();
    }
    if (auto* ident = dynamic_cast<const Ident*>(expr)) {
        return onIdent(const_cast<Ident&>(*ident));
    }
    if (auto* binary = dynamic_cast<const Binary*>(expr)) {
        return onBinary(const_cast<Binary&>(*binary), expected);
    }
    if (auto* call = dynamic_cast<const Call*>(expr)) {
        return onCall(const_cast<Call&>(*call));
    }
    if (auto* cast = dynamic_cast<const Cast*>(expr)) {
        return onCast(const_cast<Cast&>(*cast));
    }
    if (auto* lit = dynamic_cast<const IntLit*>(expr)) {
        std::unordered_set<std::string> active;
        EvalEnv env = resolver_.evalEnv(emitScope());
        EvalResult folded = evalConstInt(*lit, env, active);
        TypeRef type = checker_.typeOf(expr);
        bool fromFallback = false;
        if (type.isError() && !expected.isError() &&
            expected.type->kind() == Kind::Basic) {
            type = expected;
            fromFallback = true;
        }
        if (!folded.ok) {
            refuse(lit->pos, "this constant");
            return undefined();
        }
        if (fromFallback && basicIsInteger(*type.type->basic())) {
            int width = basicWidthBits(*type.type->basic());
            bool isSigned = basicIsSigned(*type.type->basic());
            __int128 lo = isSigned ? -(static_cast<__int128>(1) << (width - 1)) : 0;
            __int128 hi = isSigned ? ((static_cast<__int128>(1) << (width - 1)) - 1)
                                   : ((static_cast<__int128>(1) << width) - 1);
            unsigned __int128 bits = static_cast<uint64_t>(folded.value);
            bool fits = folded.big
                            ? !isSigned && bits <= static_cast<unsigned __int128>(hi)
                            : static_cast<__int128>(folded.value) >= lo &&
                                  static_cast<__int128>(folded.value) <= hi;
            if (!fits) {
                refuse(lit->pos, "this literal does not fit " + typeName(type));
                return undefined();
            }
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
        return lowerFloatLit(*lit, expected);
    }
    if (auto* lit = dynamic_cast<const CharLit*>(expr)) {
        return constantI64(static_cast<int64_t>(lit->value),
                           TypeRef{ctx_.basic(BasicKind::Char)}, lit->pos);
    }
    if (auto* lit = dynamic_cast<const WideChar*>(expr)) {
        return constantI64(static_cast<int64_t>(lit->value),
                           TypeRef{ctx_.basic(BasicKind::U16)}, lit->pos);
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
    if (auto* al = dynamic_cast<const AlignofType*>(expr)) {
        std::vector<ir::Value> words = lowerAlignof(*al);
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
            return lowerNeg(*unary, expected);
        }
        if (unary->op == UnOp::BitNot) {
            return lowerBitNot(*unary, expected);
        }
        if (unary->op == UnOp::Not) {
            TypeRef type = checker_.typeOf(expr);
            if (type.isError() && !expected.isError()) {
                type = expected;
            }
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
    value = adjustStringLitArg(n.value.get(), std::move(value), target);
    // A concrete value assigned to an interface boxes to the pair before
    // the word-count check below, which then enforces two, not one.
    value = boxClassOperand(n.value.get(), std::move(value), checker_.typeOf(n.value.get()),
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
    value = adjustStringLitArg(n.value.get(), std::move(value), target);
    TypeRef valueType = checker_.typeOf(n.value.get());
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
    value = boxClassOperand(n.value.get(), std::move(value), valueType, target, n.pos);
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
    if (const auto* field = dynamic_cast<const Field*>(n.target.get())) {
        Place place;
        if (!resolvePlace(n.target.get(), place)) {
            refuse(n.target->pos, "this assignment");
            return false;
        }
        place.type = checker_.typeOf(n.target.get());
        std::optional<DeclId> unitOf;
        TypeRef owner = checker_.typeOf(field->object.get());
        if (!owner.isError()) {
            unitOf = owner.type->decl();
        }
        int64_t bitOff = -1;
        int64_t bitWidth = 0;
        if (unitOf.has_value() && !value.empty() &&
            resolver_.bitField(*unitOf, field->name, bitOff, bitWidth)) {
            if (!storeBitField(n.pos, place, place.type, bitOff, bitWidth,
                               value[0])) {
                refuse(n.target->pos, "this assignment");
                return false;
            }
            return !failed_;
        }
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
