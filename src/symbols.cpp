#include "symbols.h"

namespace cider {

// CIDER-BUG-145: `volatile [T; N]` qualifies the array object, so every
// element — through any number of nested array dimensions — is an
// observable access, exactly like `[volatile T; N]` marks its element.
// The TypeInfo descriptor is flat at the top (a single `isVolatile` flag
// serves the array and its element), so mark the descriptor and every
// nested `arrayElement` it carries.
static void markVolatileDeep(TypeInfo& t) {
    t.isVolatile = true;
    if (t.arrayElement) markVolatileDeep(*t.arrayElement);
}

TypeInfo TypeInfo::fromAST(TypeAST* ast) {
    if (!ast) return {"void", 0};

    // Use dynamic_cast to check for ArrayTypeAST
    if (auto arrayAst = dynamic_cast<ArrayTypeAST*>(ast)) {
        TypeInfo info = fromAST(arrayAst->getBaseType());
        // CIDER-BUG-145: the wrapper qualifier (`volatile [T; N]`) reaches
        // the descriptor here; without this fold parseType's consumed
        // `volatile` was dropped and element accesses stayed plain. Marking
        // before the arrayElement copy below also covers the inner shape.
        if (arrayAst->isVolatile()) markVolatileDeep(info);
        // CIDER-BUG-052: preserve the full inner shape. The flat
        // isArray/arraySize pair only carries the outermost dimension, so a
        // nested base's shape used to be overwritten and lost.
        if (info.isArray) info.arrayElement = std::make_shared<TypeInfo>(info);
        info.isArray = true;
        info.arraySize = arrayAst->getSize();
        return info;
    }

    TypeInfo info;
    info.name = ast->getName();
    // CID-003: `f32` is an alias spelling of `float`; normalize it so every
    // downstream comparison and diagnostic uses the canonical name.
    if (info.name == "f32") info.name = "float";

    // CIDER-BUG-063: the legacy `func`/`function` spelling is the closure
    // type. Mark the runtime representation so a plain `fn` value with the
    // same signature is not silently interchangeable at a call site (the
    // closure path passes the object as a hidden first argument).
    // CIDER-BUG-068: the checker can also infer the closure representation
    // for an `fn(...)` parameter that only ever receives closure values;
    // its TypeAST carries that decision (see TypeAST::setClosureRepr).
    if (info.name == "func" || info.name == "function" || ast->hasClosureRepr()) {
        info.isClosure = true;
    }

    // CID-006: `fn(...)` carries ordered parameter types and a return
    // type. The array branch above copies these fields onto the array
    // wrapper (isArray + signature), so `[fn(...); N]` indexing recovers
    // the element signature.
    if (ast->isFunctionType()) {
        info.isFunctionPointer = true;
        info.isMsAbi = (ast->getAbi() == Abi::Ms);
        info.fnParamTypes = std::make_shared<std::vector<TypeInfo>>();
        for (const auto& p : ast->getFnParamTypes()) {
            info.fnParamTypes->push_back(fromAST(p.get()));
        }
        info.fnReturnType = std::make_shared<TypeInfo>(fromAST(ast->getFnReturnType()));
    }

    info.pointerLevel = ast->getPointerLevel();
    info.isVolatile = ast->isVolatile();
    return info;
}

SymbolTable::SymbolTable() {
    // Initialize with Global Scope
    auto globalScope = std::make_unique<Scope>(nullptr);
    currentScope_ = globalScope.get();
    scopes_.push_back(std::move(globalScope));
}

void SymbolTable::enterScope() {
    auto newScope = std::make_unique<Scope>(currentScope_);
    currentScope_ = newScope.get();
    scopes_.push_back(std::move(newScope));
}

void SymbolTable::exitScope() {
    if (currentScope_->getParent()) {
        currentScope_ = currentScope_->getParent();
    }
}

void SymbolTable::define(std::shared_ptr<Symbol> symbol) {
    currentScope_->define(symbol);
}

std::shared_ptr<Symbol> SymbolTable::resolve(const std::string& name) {
    return currentScope_->resolve(name);
}

}  // namespace cider
