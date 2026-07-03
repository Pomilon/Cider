#ifndef CIDER_SYMBOLS_H
#define CIDER_SYMBOLS_H

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <optional>

namespace Cider {

    class TypeAST;
    class Statement;

    /**
     * Internal representation of a Cider type.
     */
    struct TypeInfo {
        std::string name;
        int pointerLevel = 0;
        bool isUserDefined = false; // Struct or Enum
        bool isArray = false;
        int arraySize = 0;
        bool isVolatile = false;
        bool isInterface = false;
        bool isSlice = false;
        
        static TypeInfo fromAST(TypeAST* ast);
        
        bool operator==(const TypeInfo& other) const {
            return name == other.name && pointerLevel == other.pointerLevel && 
                   isArray == other.isArray && arraySize == other.arraySize &&
                   isVolatile == other.isVolatile && isInterface == other.isInterface &&
                   isSlice == other.isSlice;
        }
        
        bool operator!=(const TypeInfo& other) const {
            return !(*this == other);
        }

        std::string toString() const {
            std::string res = "";
            if (isVolatile) res += "volatile ";
            for (int i = 0; i < pointerLevel; ++i) res += "*";
            if (isArray) {
                res += "[" + name + "; " + std::to_string(arraySize) + "]";
            } else {
                res += name;
            }
            return res;
        }
    };

    /**
     * Base class for all symbols (Variables, Functions, Types).
     */
    enum class SymbolKind { VARIABLE, FUNCTION, STRUCT, ENUM, CLASS, INTERFACE, TEMPLATE };

    struct Symbol {
        SymbolKind kind;
        std::string name;
        TypeInfo type;
        int line;
        int col;
        bool isConst = false;
        bool isExported = false;
        double constValue = 0; // For const variables and enum variant values
        
        // For variables: stack offset or global address (set during codegen)
        int memoryOffset = 0;
    };

    struct FunctionSymbol : public Symbol {
        std::vector<TypeInfo> parameterTypes;
        TypeInfo returnType;
        bool isExtern = false;
    };

    struct StructSymbol : public Symbol {
        struct Field {
            std::string name;
            TypeInfo type;
            int offset;
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
        std::vector<std::string> vtable; // List of mangled function names
        std::string parentName; // For inheritance
        std::string destructorName; // Mangled name of destructor, empty if none
        int totalSize = 0;
    };

    struct InterfaceSymbol : public Symbol {
        std::vector<StructSymbol::Method> methods;
    };

    struct TemplateSymbol : public Symbol {
        std::unique_ptr<Statement> ast;  // FunctionDeclStmt or StructDeclStmt
        std::vector<std::string> typeParams;
    };

    /**
     * A single scope (Block, Function, or Global).
     */
    class Scope {
    public:
        Scope(Scope* parent = nullptr) : parent_(parent) {}
        
        void define(std::shared_ptr<Symbol> symbol) {
            symbols_[symbol->name] = symbol;
        }

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
                auto parent_syms = parent_->getAllSymbols();
                result.insert(result.end(), parent_syms.begin(), parent_syms.end());
            }
            return result;
        }

        Scope* getParent() const { return parent_; }

    private:
        Scope* parent_;
        std::map<std::string, std::shared_ptr<Symbol>> symbols_;
    };

    /**
     * The Symbol Table manager.
     */
    class SymbolTable {
    public:
        SymbolTable();
        
        void enterScope();
        void exitScope();
        
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

    private:
        std::vector<std::unique_ptr<Scope>> scopes_;
        Scope* currentScope_;
    };

} // namespace Cider

#endif
