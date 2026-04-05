#ifndef CIDER_SYMBOLS_H
#define CIDER_SYMBOLS_H

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <optional>

namespace Cider {

    class TypeAST;

    /**
     * Internal representation of a Cider type.
     */
    struct TypeInfo {
        std::string name;
        int pointerLevel = 0;
        bool isUserDefined = false; // Struct or Enum
        bool isArray = false;
        int arraySize = 0;
        
        static TypeInfo fromAST(TypeAST* ast);
        
        bool operator==(const TypeInfo& other) const {
            return name == other.name && pointerLevel == other.pointerLevel && 
                   isArray == other.isArray && arraySize == other.arraySize;
        }
        
        bool operator!=(const TypeInfo& other) const {
            return !(*this == other);
        }

        std::string toString() const {
            std::string res = "";
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
    enum class SymbolKind { VARIABLE, FUNCTION, STRUCT, ENUM, CLASS };

    struct Symbol {
        SymbolKind kind;
        std::string name;
        TypeInfo type;
        int line;
        int col;
        bool isConst = false;
        
        // For variables: stack offset or global address (set during codegen)
        int memoryOffset = 0;
    };

    struct FunctionSymbol : public Symbol {
        std::vector<TypeInfo> parameterTypes;
        TypeInfo returnType;
    };

    struct StructSymbol : public Symbol {
        struct Field {
            std::string name;
            TypeInfo type;
            int offset;
        };
        struct Method {
            std::string name;
            std::vector<TypeInfo> parameterTypes;
            TypeInfo returnType;
            int vtableIndex;
        };
        std::vector<Field> fields;
        std::vector<Method> methods;
        std::string parentName; // For inheritance
        int totalSize = 0;
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
        
        Scope* getCurrentScope() const { return currentScope_; }

    private:
        std::vector<std::unique_ptr<Scope>> scopes_;
        Scope* currentScope_;
    };

} // namespace Cider

#endif
