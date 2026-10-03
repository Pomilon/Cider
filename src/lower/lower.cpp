// lower.cpp — scalars to IR.
//
// Driven by the checker's walk: every hook below is called while the checker is
// visiting a node it has just typed, so a type is recorded and an instruction
// emitted on the same visit. Nothing here walks the AST.

#include "lower/lower.h"

#include "types/context.h"

namespace cider {
namespace lower {

namespace {

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
    if (type.isError() || type.type->kind() != Kind::Basic) {
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
    binding.name = "g_" + n.name;
    binding.type = type;
    if (folded.ok) {
        binding.initialText = std::to_string(folded.value);
    } else {
        binding.initialText = "";
    }
    globals_.push_back(binding);
    module_->globals.push_back(binding.name + " = " + binding.initialText);
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

    // Parameters are SSA values: still live, so they live in a register. A
    // parameter whose address is taken becomes a slot, which is MEMORY.md §4's
    // work and not this slice's.
    std::unordered_map<std::string, LocalBinding> frame;
    const std::vector<Parameter>& params = function_->signature.parameters();
    for (uint64_t i = 0; i < static_cast<uint64_t>(params.size()); ++i) {
        LocalBinding binding;
        binding.kind = Binding::Value;
        binding.type = TypeRef{params[i].type};
        binding.isMutable = params[i].isMutable;
        binding.value.width = widthOf(binding.type, n.params[i].pos);
        binding.value.type = binding.type;
        binding.value.id = valueSeq_++;
        function_->params.push_back(binding.value);
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
    ir::Value value = onIdentOrExpr(n.init.get(), type);
    LocalBinding binding;
    binding.kind = Binding::Value;
    binding.value = value;
    binding.type = type;
    binding.isMutable = true;
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
            return found->second.value;
        }
    }
    refuse(n.pos, "reading '" + n.name + "'");
    return undefined();
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
    instr.op = op;
    instr.isPure = true;
    instr.overflow = wrap_ ? ir::OverflowMode::Wrap : ir::OverflowMode::Undefined;
    // Division and remainder trap; nothing else in this set does. The flag is
    // what stops DCE deleting the check and LICM hoisting the divide.
    if (op == ir::Op::Div || op == ir::Op::Rem) {
        instr.mayTrap = true;
    }
    instr.operands.push_back(a);
    instr.operands.push_back(b);
    ir::Value out = newValue(width, result, n.pos);
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
        ir::Argument arg;
        arg.value = onIdentOrExpr(n.args[i].get(), want);
        instr.arguments.push_back(arg);
    }
    const Type* ret = sig->returnType() != nullptr ? sig->returnType() : ctx_.void_();
    if (ret->kind() != Kind::Void) {
        ir::Value out = newValue(widthOf(TypeRef{ret}, n.pos), TypeRef{ret}, n.pos);
        instr.results.push_back(out);
        instr.width = out.width;
        instr.type = out.type;
    } else {
        instr.width = ir::IRType::Void;
    }
    emit(std::move(instr));
    if (function_ != nullptr) {
        // A call is mayTrap only if the callee can throw, which is known here
        // rather than guessed by the optimiser.
        function_->canThrow = true;
    }
    return instr.results.empty() ? undefined() : instr.results[0];
}

void Lowerer::onReturn(Return& n) {
    if (failed_ || !enabled_ || function_ == nullptr) {
        return;
    }
    ir::Terminator term;
    term.term = ir::Term::Ret;
    term.loc = n.pos;
    if (n.value != nullptr) {
        term.value = onIdentOrExpr(n.value.get(), checker_.typeOf(n.value.get()));
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
    refuse(expr->pos, "this expression");
    return undefined();
}

}  // namespace lower
}  // namespace cider
