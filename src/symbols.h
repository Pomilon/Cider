#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ast.h"

namespace cider {

class TypeAST;
class Statement;
class Scope;

struct TypeInfo {
    std::string name;
    int pointerLevel = 0;
    bool isUserDefined = false;  // Struct or Enum
    bool isArray = false;
    int arraySize = 0;
    bool isVolatile = false;
    bool isInterface = false;
    bool isSlice = false;

    // CID-006: `fn(...) -> R`. When set, fnParamTypes/fnReturnType hold the
    // signature and name is "fn". Arrays of function pointers keep
    // isFunctionPointer on the array wrapper (fromAST copies the element
    // fields) so indexing recovers the element signature.
    bool isFunctionPointer = false;
    // CID-005: an `msabi fn(...)` type. `fn` stays SysV; arrays copy the
    // element's ABI through TypeInfo::fromAST.
    bool isMsAbi = false;
    std::shared_ptr<std::vector<TypeInfo>> fnParamTypes = nullptr;
    std::shared_ptr<TypeInfo> fnReturnType = nullptr;

    static TypeInfo fromAST(TypeAST* ast);

    // Exact signature equality (spec §2): same parameter count, same
    // parameter types, same return type.
    bool signatureEquals(const TypeInfo& other) const {
        if (!isFunctionPointer || !other.isFunctionPointer) return false;
        if (!fnParamTypes || !other.fnParamTypes || !fnReturnType || !other.fnReturnType) return false;
        if (isMsAbi != other.isMsAbi) return false;
        return *fnParamTypes == *other.fnParamTypes && *fnReturnType == *other.fnReturnType;
    }

    bool operator==(const TypeInfo& other) const {
        if (isFunctionPointer || other.isFunctionPointer) {
            return isFunctionPointer == other.isFunctionPointer &&
                   isArray == other.isArray && arraySize == other.arraySize &&
                   isMsAbi == other.isMsAbi &&
                   signatureEquals(other);
        }
        return name == other.name && pointerLevel == other.pointerLevel &&
               isArray == other.isArray && arraySize == other.arraySize &&
               isVolatile == other.isVolatile && isInterface == other.isInterface &&
               isSlice == other.isSlice;
    }

    bool operator!=(const TypeInfo& other) const { return !(*this == other); }

    // Canonical diagnostic spelling: `fn(int, int) -> int`.
    std::string functionPointerToString() const {
        std::string res = isMsAbi ? "msabi fn(" : "fn(";
        if (fnParamTypes) {
            for (size_t i = 0; i < fnParamTypes->size(); ++i) {
                if (i) res += ", ";
                res += (*fnParamTypes)[i].toString();
            }
        }
        res += ") -> ";
        res += fnReturnType ? fnReturnType->toString() : "void";
        return res;
    }

    std::string toString() const {
        std::string res = "";
        if (isVolatile) res += "volatile ";
        for (int i = 0; i < pointerLevel; ++i) res += "*";
        if (isArray) {
            if (isFunctionPointer) {
                TypeInfo elem = *this;
                elem.isArray = false;
                elem.arraySize = 0;
                res += "[" + elem.toString() + "; " + std::to_string(arraySize) + "]";
            } else {
                res += "[" + name + "; " + std::to_string(arraySize) + "]";
            }
        } else if (isFunctionPointer) {
            res += functionPointerToString();
        } else {
            res += name;
        }
        return res;
    }
};

enum class SymbolKind { VARIABLE, FUNCTION, STRUCT, ENUM, CLASS, INTERFACE, TEMPLATE, MODULE };

struct Symbol {
    SymbolKind kind;
    std::string name;
    TypeInfo type;
    int line;
    int col;
    bool isConst = false;
    bool isExported = false;
    bool isExtern = false;  // linker-provided (extern var / extern func)
    bool isTypeAlias = false;  // `type X = T` provider (STRUCT-kind, not StructSymbol)
    double constValue = 0;  // For const variables and enum variant values

    int memoryOffset = 0;  // stack offset or global address (set during codegen)

    // CIDER-BUG-044: stable identity for the defining-module provenance
    // maps. Keying by the raw pointer is unsafe: freed symbols' addresses
    // are reused, so a fresh symbol can inherit a dead one's entry (an
    // ir_gen-created global inherited a discarded `common.net` local's
    // module, and its reads fell into the unresolved-identifier fallback).
    uint64_t id = nextSymbolId();
    static uint64_t nextSymbolId() {
        static uint64_t next = 1;
        return next++;
    }
};

struct FunctionSymbol : public Symbol {
    std::vector<TypeInfo> parameterTypes;
    TypeInfo returnType;
    bool isAsync = false;
    Abi abi = Abi::Default;
};

struct StructSymbol : public Symbol {
    struct Field {
        std::string name;
        TypeInfo type;
        int offset = 0;
        int bits = 0;      // declared bitfield width (0 = regular field)
        int bitOffset = 0; // bit position within the containing word
        int bitWidth = 0;  // effective width in the layout
    };
    struct Method {
        std::string name;
        std::string mangledName;
        std::vector<TypeInfo> parameterTypes;
        TypeInfo returnType;
        int vtableIndex = -1;
        bool isVirtual = false;
        bool isOverride = false;
    };
    std::vector<Field> fields;
    std::vector<Method> methods;
    std::vector<std::string> vtable;  // List of mangled function names
    std::string parentName;           // For inheritance
    std::string destructorName;       // Mangled name of destructor, empty if none
    int totalSize = 0;
};

struct InterfaceSymbol : public Symbol {
    std::vector<StructSymbol::Method> methods;
};

struct TemplateSymbol : public Symbol {
    std::unique_ptr<Statement> ast;  // FunctionDeclStmt or StructDeclStmt
    std::vector<std::string> typeParams;
};

struct ModuleSymbol : public Symbol {
    std::string logicalName;  // full dotted path as imported, e.g. "a.util"
    Scope* scope = nullptr;   // owned by the SymbolTable scope tree
};

class Scope {
public:
    explicit Scope(Scope* parent = nullptr) : parent_(parent) {}

    void define(std::shared_ptr<Symbol> symbol) { symbols_[symbol->name] = symbol; }

    std::shared_ptr<Symbol> resolve(const std::string& name) {
        if (symbols_.count(name)) return symbols_[name];
        if (parent_) return parent_->resolve(name);
        return nullptr;
    }

    std::shared_ptr<Symbol> resolveLocal(const std::string& name) {
        if (symbols_.count(name)) return symbols_[name];
        return nullptr;
    }

    std::vector<std::shared_ptr<Symbol>> getAllSymbols() const {
        std::vector<std::shared_ptr<Symbol>> result;
        for (auto& [name, sym] : symbols_) result.push_back(sym);
        if (parent_) {
            auto parentSyms = parent_->getAllSymbols();
            result.insert(result.end(), parentSyms.begin(), parentSyms.end());
        }
        return result;
    }

    Scope* getParent() const { return parent_; }

    // CID-020/021 (spec §6): marks a scope that holds one imported module's
    // symbols. Depth queries skip module scopes so "global scope" keeps its
    // meaning for code checked inside a module.
    bool isModuleScope() const { return isModule_; }
    void markModuleScope() { isModule_ = true; }

private:
    Scope* parent_;
    std::map<std::string, std::shared_ptr<Symbol>> symbols_;
    bool isModule_ = false;
};

class SymbolTable {
public:
    SymbolTable();

    void enterScope();
    void exitScope();

    // CID-020/021: enter a scope flagged as a module scope (spec §6).
    void enterModuleScope() {
        enterScope();
        currentScope_->markModuleScope();
    }

    void define(std::shared_ptr<Symbol> symbol);
    std::shared_ptr<Symbol> resolve(const std::string& name);
    std::vector<std::shared_ptr<Symbol>> getAllSymbols() {
        Scope* root = currentScope_;
        while (root && root->getParent()) root = root->getParent();
        return root ? root->getAllSymbols() : std::vector<std::shared_ptr<Symbol>>();
    }

    Scope* getRootScope() {
        Scope* root = currentScope_;
        while (root && root->getParent()) root = root->getParent();
        return root;
    }

    Scope* getCurrentScope() const { return currentScope_; }

    // CID-020: enter an existing table-owned scope (a module scope from the
    // collector registry). Callers must restore the previous scope.
    void setCurrentScope(Scope* scope) {
        if (scope) currentScope_ = scope;
    }

    // Nesting depth of the current scope (root/global scope is 0). Module
    // scopes are skipped (spec §6 scope-kind flag): a module-level
    // declaration still counts as global. Used to tell global variable
    // declarations from function-local ones.
    int scopeDepth() const {
        int depth = 0;
        for (Scope* s = currentScope_; s && s->getParent(); s = s->getParent())
            if (!s->isModuleScope()) ++depth;
        return depth;
    }

private:
    std::vector<std::unique_ptr<Scope>> scopes_;
    Scope* currentScope_;
};

}  // namespace cider
