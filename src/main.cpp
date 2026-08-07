#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "ast.h"
#include "codegen.h"
#include "constant_folder.h"
#include "errors.h"
#include "gvn.h"
#include "inliner.h"
#include "ir_gen.h"
#include "lexer.h"
#include "licm.h"
#include "module_resolver.h"
#include "optimizer.h"
#include "parser.h"
#include "ssa_deconstructor.h"
#include "symbol_collector.h"
#include "symbols.h"
#include "type_checker.h"
#include "vm.h"

struct CliOptions {
    std::string command;      // "parse" | "check" | "build" | "run" | "compile"
    bool noOpt = false;
    std::string inputFile;
    std::string outputFile;
};

// Returns false + prints usage to stderr on bad arguments.
bool parseCli(int argc, char** argv, CliOptions& out) {
    if (argc < 2) return false;
    std::string cmd = argv[1];
    if (cmd != "parse" && cmd != "check" && cmd != "build" && cmd != "run" && cmd != "compile") return false;
    out.command = cmd;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--no-opt") {
            out.noOpt = true;
        } else if (a == "-o") {
            if (i + 1 >= argc) return false;
            out.outputFile = argv[++i];
        } else if (!a.empty() && a[0] == '-') {
            return false;   // unknown flag
        } else if (out.inputFile.empty()) {
            out.inputFile = a;
        } else {
            return false;   // extra positional
        }
    }
    if (out.inputFile.empty()) return false;
    if (out.command == "compile" && out.outputFile.empty()) return false;
    return true;
}

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open file: " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The optimization pipeline (main's fixpoint):
//   SSA LICM -> GVN (pre-decon), then per round: DCE, inline, post-LICM (<=10).
void optimizeModule(cider::ModuleIR& module) {
    cider::SSALicmPass ssaLicm;
    ssaLicm.run(module);
    cider::GvnPass gvn;
    gvn.run(module);

    cider::Inliner<> inliner;
    cider::PostLicmPass postLicm;
    constexpr int kMaxOptPasses = 10;
    for (int pass = 0; pass < kMaxOptPasses; ++pass) {
        for (auto& func : module.getFunctions()) {
            cider::Optimizer::dce(*func);
        }
        bool inlined = inliner.run(module);
        bool licmed = postLicm.run(module);
        if (!inlined && !licmed) break;
    }
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::printf("cider 0.2.0\n");
        return 0;
    }
    CliOptions opts;
    if (!parseCli(argc, argv, opts)) {
        std::fprintf(stderr, "usage: cider <parse|check|build|run|compile> [--no-opt] [-o out.asm] <file>\n");
        return 2;
    }
    try {
        std::string text = readFile(opts.inputFile);
        cider::ErrorReporter rep;
        cider::Lexer lex(text, rep);
        cider::Parser parser(lex, rep);
        auto prog = parser.parseProgram();
        if (rep.hasErrors()) {
            for (const auto& d : rep.errors())
                std::fprintf(stderr, "%s:%d:%d: %s\n", opts.inputFile.c_str(),
                             d.line, d.col, d.message.c_str());
            return 1;
        }
        if (opts.command == "parse") {
            std::printf("%s\n", cider::dump(*prog).c_str());
            return 0;
        }
        cider::SymbolTable table;
        cider::ModuleResolver resolver;
        cider::SymbolCollector collector(table, rep, resolver, opts.inputFile);
        collector.collect(*prog);
        rep.clear();
        cider::TypeChecker checker(table, rep);
        checker.check(*prog);
        if (rep.hasErrors()) {
            for (const auto& d : rep.errors())
                std::fprintf(stderr, "%s:%d:%d: %s\n", opts.inputFile.c_str(),
                             d.line, d.col, d.message.c_str());
            return 1;
        }
        if (opts.command == "check") {
            return 0;
        }
        if (opts.command == "compile") {
            cider::IRGenerator gen(table, opts.inputFile);
            auto module = gen.generate(*prog);
            if (!opts.noOpt) {
                optimizeModule(module);
            }
            cider::SSADeconstructor decon;
            decon.process(module);
            cider::CodeGenerator cg;
            if (!cg.generate(module, opts.outputFile, opts.inputFile)) {
                std::fprintf(stderr, "cannot write %s\n", opts.outputFile.c_str());
                return 1;
            }
            return 0;
        }
        if (opts.command == "run") {
            cider::IRGenerator gen(table, opts.inputFile);
            auto module = gen.generate(*prog);
            if (!opts.noOpt) {
                optimizeModule(module);
            }
            cider::VM vm;
            cider::VMResult r = vm.run(module);
            std::fwrite(r.stdoutText.data(), 1, r.stdoutText.size(), stdout);
            std::fwrite(r.stderrText.data(), 1, r.stderrText.size(), stderr);
            return r.exitCode;
        }
        // build: generate IR and print the module dump.
        cider::IRGenerator gen(table, opts.inputFile);
        auto module = gen.generate(*prog);
        module.print();
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s: %s\n", opts.inputFile.c_str(), e.what());
        return 1;
    }
}
