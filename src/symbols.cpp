#include "symbols.h"

namespace cider {

TypeInfo TypeInfo::fromAST(TypeAST* ast) {
    if (!ast) return {"void", 0};

    // Use dynamic_cast to check for ArrayTypeAST
    if (auto arrayAst = dynamic_cast<ArrayTypeAST*>(ast)) {
        TypeInfo info = fromAST(arrayAst->getBaseType());
        info.isArray = true;
        info.arraySize = arrayAst->getSize();
        return info;
    }

    TypeInfo info;
    info.name = ast->getName();
    // CID-003: `f32` is an alias spelling of `float`; normalize it so every
    // downstream comparison and diagnostic uses the canonical name.
    if (info.name == "f32") info.name = "float";

    // CID-006: `fn(...)` carries ordered parameter types and a return
    // type. The array branch above copies these fields onto the array
    // wrapper (isArray + signature), so `[fn(...); N]` indexing recovers
    // the element signature.
    if (ast->isFunctionType()) {
        info.isFunctionPointer = true;
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
