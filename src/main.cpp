// Cider/src/main.cpp
#include <iostream>
#include <fstream>
#include <sstream>
#include "cider_lexer.h"
#include "cider_parser.h"
#include "cider_ast.h"
#include "cider_codegen.h"
#include "cider_symbols.h"
#include "cider_symbol_collector.h"
#include "cider_type_checker.h"
#include "cider_constant_folder.h"
#include "cider_module_resolver.h"
#include "cider_ir_gen.h"
#include "cider_errors.h"
#include "cider_ssa_deconstructor.h"
#include "cider_reg_alloc.h"
#include "cider_optimizer.h"
#include "cider_meta.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: ciderc <source_file.cdr> [-o output.asm] [-v]\n";
        return 1;
    }

    std::string sourceFile = argv[1];
    std::string outputFile = "out.asm";
    bool verbose = false;
    std::string cacheDir = "";
    bool emitMeta = false;
    bool moduleMode = false;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-o" && i + 1 < argc) {
            outputFile = argv[++i];
        } else if (arg == "-v" || arg == "--verbose") {
            verbose = true;
        } else if (arg == "--cache" && i + 1 < argc) {
            cacheDir = argv[++i];
        } else if (arg == "--emit-meta") {
            emitMeta = true;
        } else if (arg == "--module") {
            moduleMode = true;
        }
    }

    std::ifstream file(sourceFile);
    if (!file) {
        std::cerr << "Error: Cannot open file " << sourceFile << "\n";
        return 1;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string source = buffer.str();

    Cider::ErrorReporter reporter;

    // 1. Lexical Analysis
    Cider::Lexer lexer(source, reporter);
    
    // 2. Parsing
    Cider::Parser parser(lexer, reporter);
    auto ast = parser.parseProgram();

    if (!ast || reporter.hasErrors()) {
        std::cerr << "Compilation failed during parsing.\n";
        return 1;
    }

    std::cout << "Successfully parsed " << sourceFile << " into Cider AST.\n";
    
    // 3. Semantic Analysis (Phase 1: Symbol Collection)
    Cider::ModuleResolver resolver;
    Cider::SymbolTable symbolTable;
    Cider::SymbolCollector collector(symbolTable, resolver, reporter, sourceFile, cacheDir);
    collector.collect(*ast);
    
    if (reporter.hasErrors()) {
        std::cerr << "Compilation failed during symbol collection.\n";
        return 1;
    }
    std::cout << "Successfully collected symbols.\n";
    
    // 3.5 Semantic Analysis (Phase 2: Type Checking)

    {
        Cider::TypeChecker typeChecker(symbolTable, reporter, moduleMode);
        typeChecker.check(*ast);
    }
    
    if (reporter.hasErrors()) {
        std::cerr << "Compilation failed during type checking.\n";
        return 1;
    }
    
    if (reporter.hasErrors()) {
        std::cerr << "Compilation failed during type checking.\n";
        return 1;
    }
    std::cout << "Successfully type checked.\n";
    
    // 3.7 Semantic Analysis (Phase 4: Constant Folding)
    Cider::ConstantFolder folder;
    folder.fold(*ast);
    std::cout << "Successfully folded constants.\n";
    
    // 3.9 Semantic Analysis (Phase 6: SSA IR Generation)
    Cider::IRGenerator irGen(symbolTable, sourceFile);
    Cider::ModuleIR irModule = irGen.generate(*ast);
    std::cout << "Successfully generated SSA IR.\n";
    
    // 3.95 Semantic Analysis (Phase 9: SSA Deconstruction)
    Cider::SSADeconstructor deconstructor;
    deconstructor.process(irModule);
    std::cout << "Successfully deconstructed SSA IR.\n";
    if (verbose) irModule.print();
    
    // Optimization passes
    for (auto& func : irModule.getFunctions()) {
        Cider::Optimizer::dce(*func);
    }
    Cider::Optimizer::inlineFunctions(irModule);
    for (auto& func : irModule.getFunctions()) {
        Cider::Optimizer::dce(*func);
    }
    std::cout << "Optimization passes complete.\n";
    
    // Phase 10: Register Allocation
    Cider::RegisterAllocator regAlloc;
    for (auto& func : irModule.getFunctions()) {
        int stackSize = 0;
        auto alloc = regAlloc.allocate(*func, stackSize);
        std::cout << "Registers for function " << func->getName() << " (stack size: " << stackSize << "):\n";
        for (const auto& [var, res] : alloc) {
            std::cout << "  " << var << " -> " << (res.isSpilled ? "[rbp" + std::to_string(res.stackOffset) + "]" : res.reg) << "\n";
        }
    }
    
    // 4. Code Generation (Direct to Native Assembly)
    Cider::CodeGenerator codegen("x86_64-linux");
    if (!codegen.generate(irModule, outputFile, sourceFile)) {
        std::cerr << "Code generation failed.\n";
        return 1;
    }

    // 4.5 Emit meta file if requested
    if (emitMeta && !cacheDir.empty()) {
        std::string metaName = sourceFile;
        for (auto& c : metaName) if (c == '/') c = '.';
        if (metaName.size() > 4 && metaName.substr(metaName.size()-4) == ".cdr")
            metaName = metaName.substr(0, metaName.size()-4);
        std::string metaPath = cacheDir + "/" + metaName + ".cider-meta";
        std::ofstream metaOut(metaPath);
        if (metaOut) {
            metaOut << "module " << metaName << "\n";
            metaOut.close();
        }
    }

    std::cout << "Compilation complete. Assembly written to " << outputFile << "\n";

    // Phase 13: Pipeline Automation
    // Assuming output is out.asm, let's assemble and link it if the user didn't just ask for assembly
    std::string objFile = outputFile.substr(0, outputFile.find_last_of('.')) + ".o";
    std::string binFile = outputFile.substr(0, outputFile.find_last_of('.'));
    if (binFile == outputFile) binFile = "a.out"; // Default if no extension
    
    std::string nasmCmd = "nasm -O0 -f elf64 " + outputFile + " -o " + objFile;
    
    bool hasStart = false;
    for (const auto& func : irModule.getFunctions()) {
        if (func->getName() == "_start") {
            hasStart = true;
            break;
        }
    }

    std::string linkerCmd;
    if (hasStart) {
        linkerCmd = "ld " + objFile + " -o " + binFile;
    } else {
        linkerCmd = "gcc " + objFile + " -o " + binFile + " -no-pie";
    }
    
    std::cout << "Running assembler: " << nasmCmd << "\n";
    if (system(nasmCmd.c_str()) != 0) {
        std::cerr << "Assembler failed. Make sure nasm is installed.\n";
        return 1;
    }
    
    std::cout << "Running linker: " << linkerCmd << "\n";
    if (system(linkerCmd.c_str()) != 0) {
        std::cerr << "Linker failed.\n";
        return 1;
    }
    
    std::cout << "Successfully built executable: " << binFile << "\n";

    return 0;
}
