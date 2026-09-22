#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>

#include "ast.h"
#include "bundler.h"
#include "codegen.h"
#include "constant_folder.h"
#include "errors.h"
#include "escape.h"
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
    std::string command;      // "parse" | "check" | "build" | "run" | "compile" | "bundle"
    bool noOpt = false;
    std::string inputFile;
    std::string outputFile;
    std::string target = "native";  // "native" | "pomeos"
};

// Returns false + prints usage to stderr on bad arguments.
bool parseCli(int argc, char** argv, CliOptions& out) {
    if (argc < 2) return false;
    std::string cmd = argv[1];
    if (cmd != "parse" && cmd != "check" && cmd != "build" && cmd != "run" && cmd != "compile" && cmd != "bundle") return false;
    out.command = cmd;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--no-opt") {
            out.noOpt = true;
        } else if (a == "--target") {
            if (i + 1 >= argc) return false;
            out.target = argv[++i];
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

    // Escape analysis: non-escaping mallocs become stack slots and their
    // frees are removed. Runs after the fixpoint, before the deconstruction.
    cider::EscapeAnalysis escape;
    escape.run(module);
}

// Print a diagnostic with the source line and a caret under the column.
// Prefixes distinguish the compiler stage (the error-code scheme).
// Each diagnostic carries its own filename (Issue #16): errors in imported
// modules show that module's path and source line, not the main file's.
void printDiagnostics(const std::string& text, const std::string& path,
                      const std::vector<cider::Diagnostic>& diags, const char* stage) {
    std::map<std::string, std::string> fileTexts;
    auto textFor = [&](const std::string& file) -> const std::string& {
        if (file.empty() || file == path) return text;
        auto it = fileTexts.find(file);
        if (it != fileTexts.end()) return it->second;
        try {
            fileTexts[file] = readFile(file);
        } catch (...) {
            fileTexts[file] = text;
        }
        return fileTexts[file];
    };
    for (const auto& d : diags) {
        const std::string& file = d.filename.empty() ? path : d.filename;
        std::fprintf(stderr, "%s:%d:%d: %s error: %s\n", file.c_str(),
                     d.line, d.col, stage, d.message.c_str());
        // Extract the offending source line and draw the caret.
        const std::string& src = textFor(file);
        size_t start = 0;
        int line = 1;
        while (line < d.line) {
            start = src.find('\n', start);
            if (start == std::string::npos) break;
            start++;
            line++;
        }
        size_t end = src.find('\n', start);
        std::string srcLine = src.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!srcLine.empty()) {
            std::fprintf(stderr, "  %s\n", srcLine.c_str());
            std::string caret(static_cast<size_t>(d.col > 0 ? d.col - 1 : 0), ' ');
            std::fprintf(stderr, "  %s^\n", caret.c_str());
        }
    }
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::printf("cider 0.2.0\n");
        return 0;
    }
    CliOptions opts;
    if (!parseCli(argc, argv, opts)) {
        std::fprintf(stderr, "usage: cider <parse|check|build|run|compile|bundle> [--no-opt] [-o out.asm] <file>\n");
        return 2;
    }
    // bundle is self-contained (see bundler.h): it runs before — and
    // outside — the normal parse/collect/check pipeline.
    if (opts.command == "bundle") {
        cider::Bundler bundler;
        cider::BundleResult r = bundler.bundle(opts.inputFile);
        if (!r.ok) {
            std::fprintf(stderr, "bundle error: %s\n", r.error.c_str());
            return 1;
        }
        if (!opts.outputFile.empty()) {
            std::ofstream out(opts.outputFile, std::ios::binary | std::ios::trunc);
            if (!out) {
                std::fprintf(stderr, "cannot write %s\n", opts.outputFile.c_str());
                return 1;
            }
            out << r.output;
        } else {
            std::fwrite(r.output.data(), 1, r.output.size(), stdout);
        }
        return 0;
    }
    try {
        std::string text = readFile(opts.inputFile);
        cider::ErrorReporter rep;
        rep.setFile(opts.inputFile);
        cider::Lexer lex(text, rep);
        cider::Parser parser(lex, rep);
        auto prog = parser.parseProgram();
        if (rep.hasErrors()) {
            printDiagnostics(text, opts.inputFile, rep.errors(), "parse");
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
        if (rep.hasErrors()) {
            printDiagnostics(text, opts.inputFile, rep.errors(), "symbol");
            return 1;
        }
        rep.clear();
        cider::TypeChecker checker(table, rep);
        checker.setModuleScopes(collector.getModuleScopes());
        checker.setProgramModules(collector.getProgramModules());
        checker.setAmbiguousModules(collector.getAmbiguousModules());
        checker.check(*prog);
        if (rep.hasErrors()) {
            printDiagnostics(text, opts.inputFile, rep.errors(), "type");
            return 1;
        }
        if (opts.command == "check") {
            return 0;
        }
        if (opts.command == "compile") {
            cider::IRGenerator gen(table, opts.inputFile);
            gen.setModuleScopes(collector.getModuleScopes());
            gen.setProgramModules(collector.getProgramModules());
            auto module = gen.generate(*prog);
            if (!opts.noOpt) {
                optimizeModule(module);
            }
            cider::SSADeconstructor decon;
            decon.process(module);
            if (std::getenv("CIDER_DEBUG_IR")) module.print();
            cider::CodeGenerator cg(opts.target);
            // Whole-program compilation: every function (including module
            // functions) is emitted into the binary.
            if (!cg.generate(module, opts.outputFile)) {
                std::fprintf(stderr, "cannot write %s\n", opts.outputFile.c_str());
                return 1;
            }
            return 0;
        }
        if (opts.command == "run") {
            cider::IRGenerator gen(table, opts.inputFile);
            gen.setModuleScopes(collector.getModuleScopes());
            gen.setProgramModules(collector.getProgramModules());
            auto module = gen.generate(*prog);
            if (!opts.noOpt) {
                optimizeModule(module);
            }
            cider::VM vm;
            try {
                cider::VMResult r = vm.run(module);
                std::fwrite(r.stdoutText.data(), 1, r.stdoutText.size(), stdout);
                std::fwrite(r.stderrText.data(), 1, r.stderrText.size(), stderr);
                return r.exitCode;
            } catch (const cider::VMError& e) {
                std::fprintf(stderr, "vm error: %s\n", e.message.c_str());
                return 2;
            }
        }
        // build: generate IR and print the module dump.
        cider::IRGenerator gen(table, opts.inputFile);
        gen.setModuleScopes(collector.getModuleScopes());
        gen.setProgramModules(collector.getProgramModules());
        auto module = gen.generate(*prog);
        module.print();
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s: %s\n", opts.inputFile.c_str(), e.what());
        return 1;
    }
}
