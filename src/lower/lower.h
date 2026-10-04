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
// A local is a stack slot, never an SSA value: a variable assigned in a
// loop and read across the back edge cannot be one version per store without
// phi nodes, so every named place is memory and only temporaries are SSA.
// This is MEMORY.md section 4, and it is what makes `while` correct.
struct LocalBinding {
    // One slot id per word, in order. Scalars hold one; a string holds two.
    // Separate ids rather than base-plus-index: nothing here may assume two
    // allocations land consecutively.
    std::vector<ir::Value> slots;
    TypeRef type;
    bool isMutable = false;
};

struct GlobalBinding {
    std::string name;
    std::string label;
    TypeRef type;
    int64_t initial = 0;
    // A read-only bytes object this global points at, empty when none. Set
    // for string globals, whose 16 bytes are (rodata address, length).
    std::string initLabel;
    bool isVolatile = false;
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
    void onExprStmt(ExprStmt& n) override;
    void onReturn(Return& n) override;
    void onIf(IfStmt& n) override;
    void onWhile(WhileStmt& n) override;
    void onBlockEnter(Block& n) override;
    void onBlockExit(Block& n) override;
    ir::Value onBinary(Binary& n);
    ir::Value onCall(Call& n);
    ir::Value onIdent(Ident& n);
    ir::Value onCast(Cast& n);
    // The slice's expression dispatcher. Returns undefined() and records a
    // diagnostic for a construct this slice does not lower, so nothing is ever
    // approximated.
    ir::Value onIdentOrExpr(const Expr* expr, TypeRef expected);
    // One pinned stack slot for a named place. The result id names the slot;
    // rsp+id*8 is its address by construction, so loads and stores name it
    // through addressBase and the slot never needs materialising.
    ir::Value allocaSlot(TypeRef type, Position pos);
    std::vector<ir::Value> allocaSlots(TypeRef type, Position pos, uint64_t words);
    TypeRef wordType(TypeRef aggregate, uint64_t word);
    std::vector<ir::Value> loadLocal(Position pos, const LocalBinding& binding);
    std::vector<ir::Value> loadGlobal(Position pos, const GlobalBinding& global);
    // A string literal answers its address, its length, and where its bytes
    // live. Expression positions use the words; globals use the label and
    // length for their image. One function so the two cannot disagree.
    struct LoweredString {
        std::vector<ir::Value> words;
        std::string label;
        int64_t len = 0;
    };
    LoweredString lowerString(const StringLit& lit);
    // How many machine words a value occupies: one for scalars and pointers,
    // two for the 16-byte aggregates (strings today). -1 for anything larger,
    // which this slice does not lower.
    int wordCount(TypeRef type);
    // Lowers any expression to its words. Scalars answer one value; a string
    // literal answers its address and length. Anything larger is refused.
    std::vector<ir::Value> lowerWords(const Expr* expr);
    // Stores every word into the place an identifier names: consecutive
    // slots for a local, the label plus displacement for a global. Answers
    // the stored words, so an assignment is still an expression. Anything
    // else is refused, because fields and elements need aggregates and this
    // slice has none.
    std::vector<ir::Value> storeToIdent(const Ident& target,
                                        const std::vector<ir::Value>& words);
    // A place something lives: local slots or one global label, plus a byte
    // offset into it for fields and elements. Nested field chains accumulate
    // the offset while the slots stay the base value's.
    struct Place {
        std::vector<ir::Value> slots;
        std::string global;
        TypeRef type;
        int64_t off = 0;
        bool isVolatile = false;
    };
    // Names a place: identifiers, and field chains over them. Anything else
    // (a call result, arithmetic) has no address and is refused.
    bool resolvePlace(const Expr* expr, Place& out);
    // One scalar load/store at a place plus offset. The bytes must sit inside
    // a single word; anything straddling words is refused, not split, because
    // a partial-word write that guessed its neighbors would corrupt them.
    // A struct/array field that fits one word is addressed by offset and
    // size, never by scalar type: the byte count names an unsigned width and
    // the loaded value takes that width's type, so width and type never
    // disagree downstream. Only whole widths exist; any other byte count
    // refuses rather than splits.
    bool aggregateWord(int64_t bytes, ir::IRType& width, TypeRef& type);
    // A runtime address for slot bytes the slot grid cannot name: the slot
    // base plus a constant byte offset, added at runtime. The result names
    // no slot, so slot-precise optimizations treat it conservatively.
    ir::Value computedBase(ir::Value slot, int64_t byteOff, Position pos);
    std::vector<ir::Value> loadRange(Position pos, const Place& place, TypeRef fieldType);
    void storeRange(Position pos, const Place& place,
                    const std::vector<ir::Value>& words, TypeRef fieldType);
    // Construction into given slots, field by field (element by element). A
    // nested value lands word-aligned or is refused: unaligned multi-word
    // copies guess neighbor bytes.
    void constructStruct(const std::vector<ir::Value>& slots, const NewStruct& n);
    void constructArray(const std::vector<ir::Value>& slots, const NewArray& n);
    std::vector<ir::Value> lowerNewStruct(const NewStruct& n);
    std::vector<ir::Value> lowerNewArray(const NewArray& n);
    std::vector<ir::Value> lowerField(const Field& n);
    std::vector<ir::Value> lowerIndex(const Index& n);
    void storeIndex(const Index& target, const std::vector<ir::Value>& words);
    // One arithmetic operation, shared by Binary and compound assignment so
    // the two cannot disagree about what `+` emits.
    ir::Value emitArith(BinOp op, ir::Value a, ir::Value b, TypeRef result,
                        Position pos);
    // Assignments evaluate the value (computing first for a compound form)
    // and store it into the place the target names.
    ir::Value lowerAssign(Assign& n);
    // The statement form stores without answering a value, so several words
    // are fine where the expression form must refuse.
    void lowerAssignStmt(Assign& n);
    // The target stores shared by both forms. True when the store emitted.
    bool storeAssignTarget(Assign& n, const std::vector<ir::Value>& value);

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
    uint32_t rodataSeq_ = 0;
    uint32_t current_ = 0;
    bool terminated_ = false;
    std::vector<uint32_t> blockOrder_;
    std::vector<std::unordered_map<std::string, LocalBinding>> locals_;
    std::vector<GlobalBinding> globals_;
    std::unordered_map<std::string, std::string> globalNames_;
    std::vector<check::CheckError> errors_;
};

}  // namespace lower
}  // namespace cider

#endif  // CIDER_LOWER_LOWER_H
