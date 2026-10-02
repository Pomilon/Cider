// lower.h — checked expressions to IR. LAYERS.md §3.5, IR.md.
//
// ## Why this has no visitor of its own
//
// LAYERS.md §3.5.1: "The AST is walked once, in the resolver / checker.
// Lowering **queries** that result; it does not re-walk." So this is not an
// ASTVisitor — that is the checker's, and gate T5 counts exactly one. The
// Checker calls into this while it walks, which is what "one traversal" means
// when a phase has to emit as it goes: the type is recorded and the IR is
// emitted on the same visit.
//
// The slice is scalars. Integer and float arithmetic, comparisons, casts,
// calls, returns, if/else, while, locals, parameters and global initialisers.
// Anything else is **refused with a diagnostic** rather than approximated,
// because a phase that receives an Error stops (TYPES.md §7) and emitting a
// plausible instruction for a construct nobody designed is how a wrong answer
// becomes a shipped one.

#ifndef CIDER_LOWER_LOWER_H
#define CIDER_LOWER_LOWER_H

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "check/checker.h"
#include "ir/ir.h"

namespace cider {
namespace lower {

// Where a value lives while a function is being built. A local or parameter
// that is still live is an SSA value; anything needing an address becomes a
// stack slot, which is MEMORY.md §4's territory and not this slice's.
enum class Binding {
    Value,   // an SSA value
    Slot,    // a stack slot, address taken
    Global,  // a module global
};

struct LocalBinding {
    Binding kind = Binding::Value;
    ir::Value value;
    TypeRef type;
    bool isMutable = false;
};

struct GlobalBinding {
    std::string name;
    TypeRef type;
    std::string initialText;
};

class Lowerer : public check::LowerSink {
public:
    Lowerer(TypeContext& ctx, resolve::Resolver& resolver, check::Checker& checker);

    // Runs the checker's walk with this lowerer attached, so types are
    // recorded and IR is emitted on the same visit. Returns the module, or null
    // when the program failed any layer or hit a construct not yet lowered.
    std::unique_ptr<ir::Module> lowerModule(const std::string& name, Program& program);

    const std::vector<check::CheckError>& errors() const { return errors_; }

    // --- called by the Checker during its walk, through check::LowerSink ---
    void onProgram(Program& n) override;
    void onFuncDecl(FuncDecl& n) override;
    void onGlobal(VarDecl& n);
    void onVarDecl(VarDecl& n) override;
    void onReturn(Return& n) override;
    void onIf(IfStmt& n) override;
    void onWhile(WhileStmt& n) override;
    void onBlock(Block& n);
    ir::Value onBinary(Binary& n);
    ir::Value onCall(Call& n);
    ir::Value onIdent(Ident& n);
    ir::Value onCast(Cast& n);
    // The slice's expression dispatcher. Returns undefined() and records a
    // diagnostic for a construct this slice does not lower, so nothing is ever
    // approximated.
    ir::Value onIdentOrExpr(const Expr* expr, TypeRef expected);

    // Emitted by the expression hooks above, in visit order. The Checker calls
    // one of these for each expression it types.
    void emit(ir::Instr instr);
    ir::Value newValue(ir::IRType width, TypeRef type, Position pos);

    // The `wrap` flag is per file and inlining crosses files, so it travels on
    // each instruction (IR.md §3.1).
    bool wrapEnabled() const { return wrap_; }
    void setWrap(bool on) { wrap_ = on; }

    // The signature of a function by name, for direct calls.
    const Signature* functionSig(const std::string& name) const;

private:
    void refuse(Position pos, const std::string& what);
    void emitTerminator(const ir::Terminator& term);
    ir::Value constantI64(int64_t value, TypeRef type, Position pos);
    ir::IRType widthOf(TypeRef type, Position pos);

    uint32_t emitBlock(Position pos);
    void startBlock(uint32_t label);
    void sealBlock(uint32_t label);
    uint32_t current() const { return current_; }
    ir::Value undefined() const;

    TypeContext& ctx_;
    resolve::Resolver& resolver_;
    check::Checker& checker_;
    std::unique_ptr<ir::Module> module_;
    ir::Function* function_ = nullptr;
    bool enabled_ = false;
    bool wrap_ = false;
    bool failed_ = false;
    uint32_t valueSeq_ = 0;
    uint32_t labelSeq_ = 0;
    uint32_t current_ = 0;
    std::vector<uint32_t> blockOrder_;
    std::vector<std::unordered_map<std::string, LocalBinding>> locals_;
    std::vector<GlobalBinding> globals_;
    std::unordered_map<std::string, std::string> globalNames_;
    std::vector<check::CheckError> errors_;
};

}  // namespace lower
}  // namespace cider

#endif  // CIDER_LOWER_LOWER_H
