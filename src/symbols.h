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
    // CIDER-BUG-052: full element type of a nested fixed array — the inner
    // dimension, itself possibly an array. Null for single-dimension arrays
    // (their element is this TypeInfo with the wrapper stripped), so every
    // pre-existing flat-array path stays byte-identical. name/pointerLevel
    // still mirror the innermost element (the legacy flat spelling).
    std::shared_ptr<TypeInfo> arrayElement = nullptr;
    bool isVolatile = false;
    bool isInterface = false;
    bool isSlice = false;

    // CID-006: `fn(...) -> R`. When set, fnParamTypes/fnReturnType hold the
    // signature and name is "fn". Arrays of function pointers keep
    // isFunctionPointer on the array wrapper (fromAST copies the element
    // fields) so indexing recovers the element signature.
    bool isFunctionPointer = false;
    // CIDER-BUG-063: closure-object representation. The legacy
    // `func(...)`/`function(...)` spelling denotes a closure object
    // {fnptr, captures...} whose call passes the object as a hidden first
    // argument; `fn(...)` values are raw code pointers. The two share the
    // signature but not the runtime shape, so identity tracks this marker
    // (the name is kept too, for the legacy spelling and diagnostics).
    bool isClosure = false;
    // CID-005: an `msabi fn(...)` type. `fn` stays SysV; arrays copy the
    // element's ABI through TypeInfo::fromAST.
    bool isMsAbi = false;
    std::shared_ptr<std::vector<TypeInfo>> fnParamTypes = nullptr;
    std::shared_ptr<TypeInfo> fnReturnType = nullptr;

    static TypeInfo fromAST(TypeAST* ast);

    // CIDER-BUG-052: drop exactly one array dimension. For a nested fixed
    // array this returns the inner dimension (descriptor); for a
    // single-dimension array it returns the flat element, exactly as the
    // former manual `isArray = false; arraySize = 0;` strip did.
    TypeInfo elementType() const {
        if (isArray && arrayElement) return *arrayElement;
        TypeInfo elem = *this;
        elem.isArray = false;
        elem.arraySize = 0;
        elem.arrayElement = nullptr;
        return elem;
    }

    // CIDER-BUG-052: exact inner-shape comparison (both absent = flat array).
    bool sameArrayElement(const TypeInfo& other) const {
        if (static_cast<bool>(arrayElement) != static_cast<bool>(other.arrayElement)) return false;
        if (!arrayElement) return true;
        return *arrayElement == *other.arrayElement;
    }

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
                   isClosure == other.isClosure &&
                   isArray == other.isArray && arraySize == other.arraySize &&
                   sameArrayElement(other) &&
                   isMsAbi == other.isMsAbi &&
                   signatureEquals(other);
        }
        return name == other.name && pointerLevel == other.pointerLevel &&
               isArray == other.isArray && arraySize == other.arraySize &&
               sameArrayElement(other) &&
               isVolatile == other.isVolatile && isInterface == other.isInterface &&
               isSlice == other.isSlice;
    }

    bool operator!=(const TypeInfo& other) const { return !(*this == other); }

    // Canonical diagnostic spelling: `fn(int, int) -> int`; the legacy
    // closure spelling keeps its `func(...)` name (CIDER-BUG-063), so a
    // closure/plain mismatch is readable.
    std::string functionPointerToString() const {
        std::string res = isMsAbi ? "msabi " : "";
        res += isClosure ? "func(" : "fn(";
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
        // CIDER-BUG-052: a nested fixed array renders its full inner shape.
        // The descriptor carries the volatile/pointer/function-pointer
        // spelling copied from the base, so the outer wrapper contributes
        // only its own dimension.
        if (isArray && arrayElement) {
            return "[" + arrayElement->toString() + "; " + std::to_string(arraySize) + "]";
        }
        std::string res = "";
        if (isVolatile) res += "volatile ";
        // CIDER-BUG-063: a non-array function type renders its canonical
        // signature; the legacy `func(...)` spelling's +1 level is the
        // closure object pointer, not part of the printed identity.
        if (!(isFunctionPointer && !isArray)) {
            for (int i = 0; i < pointerLevel; ++i) res += "*";
        }
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
    // CIDER-BUG-060: module visibility. Only exported symbols are importable
    // from another module (`from m import x`, `m.x`, `m.f()`, `m.T`).
    // Intra-module use is unrestricted, and `isExtern` symbols are exempt:
    // they name linker-provided storage/functions, not module-private state,
    // so they stay accessible without an `export` (see the collector's
    // bindFromAliases and the checker's isVisibleMember).
    bool isExported = false;
    bool isExtern = false;  // linker-provided (extern var / extern func)
    bool isTypeAlias = false;  // `type X = T` provider (STRUCT-kind, not StructSymbol)
    double constValue = 0;  // For const variables and enum variant values

    // CIDER-FEAT-002: a bare variant constant of an enum declaration
    // (`RED` in `enum Color { RED }`). The variant has no storage; its
    // value is constValue and its type is the owning enum's canonical
    // name. Qualified access (`Color.RED`) resolves through EnumSymbol.
    bool isEnumVariant = false;

    // CIDER-BUG-048: initializer AST of a `const` declaration. Global
    // scalar initializers fold references to const symbols by recursing
    // into this expression, so the value stays exact (no double round
    // trip) and forward references resolve order-independently. Null for
    // enum variants and for symbols that are not const declarations.
    Expression* constInit = nullptr;

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

// CIDER-FEAT-002: a real enum type. The runtime representation is the
// underlying integer (int-sized, IRType::I32); the distinct symbol carries
// the type's nominal identity plus the variant list, which powers
// qualified access (`Color.RED`, `lib.Color.RED`) and the opt-in match
// exhaustiveness warning. Variants also stay bound as module-scope const
// symbols, so the legacy bare spelling (`var c: int = RED;`) keeps
// working.
struct EnumSymbol : public Symbol {
    struct Variant {
        std::string name;
        int64_t value = 0;
    };
    std::vector<Variant> variants;

    // Look one variant up by name. Returns false when not a variant.
    bool findVariant(const std::string& variantName, int64_t& out) const {
        for (const auto& v : variants) {
            if (v.name == variantName) {
                out = v.value;
                return true;
            }
        }
        return false;
    }
};

struct StructSymbol : public Symbol {
    struct Field {
        std::string name;
        TypeInfo type;
        int offset = 0;
        int bits = 0;      // declared bitfield width (0 = regular field)
        int bitOffset = 0; // bit position within the containing word
        int bitWidth = 0;  // effective width in the layout
        // CIDER-BUG-049: a class field declaration's initializer (null when
        // the field has none). Non-owning: the declaration AST outlives
        // collection and IR generation. Struct fields and struct-literal
        // fields never carry one (the parser rejects initializers there).
        Expression* initializer = nullptr;
        // Module the initializer's identifiers bind in ("" == main); an
        // inherited field keeps its declaring class's module. Used when
        // folding const references at the object creation site.
        std::string initializerModule;
        // CIDER-BUG-090: module the field's TYPE SPELLING binds in ("" ==
        // main). An inherited field keeps its declaring class's module,
        // which may differ from the module of the class being accessed or
        // laid out, so consumers must resolve `type` there rather than in
        // the derived class's scope (same defining-module rule as
        // initializerModule).
        std::string typeModule;
    };
    struct Method {
        std::string name;
        std::string mangledName;
        std::vector<TypeInfo> parameterTypes;
        TypeInfo returnType;
        int vtableIndex = -1;
        bool isVirtual = false;
        bool isOverride = false;
        // CIDER-BUG-126: declaration site, so a late diagnostic about an
        // override with no virtual base slot can point at the declaration
        // (the collector reports after the whole program is collected).
        int line = 0;
        int col = 0;
        // CIDER-BUG-090: module this method's signature spellings bind in
        // ("" == main). An inherited method keeps its declaring class's
        // module, which may differ from the derived class's and may be the
        // only module where a parameter/return type is visible.
        std::string declaringModule;
    };
    std::vector<Field> fields;
    std::vector<Method> methods;
    std::vector<std::string> vtable;  // List of mangled function names
    std::string parentName;           // For inheritance
    std::string destructorName;       // Mangled name of destructor, empty if none
    // CIDER-FEAT-003: the class's constructor, declared as `func init(...)`
    // in the class body. `constructorName` is the mangled name ("C_init"),
    // empty when the class declares none. Own declarations only: an
    // inherited `init` method is an ordinary method, never the derived
    // class's constructor (base constructors are not inherited).
    std::string constructorName;
    // Constructor parameter types WITHOUT the hidden `this`, recorded at
    // collection time so the checker can validate `new C(args)` even when
    // an inherited `init` shadows the own one in `methods`.
    std::vector<TypeInfo> constructorParamTypes;
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
