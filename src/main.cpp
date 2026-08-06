#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "ast.h"
#include "errors.h"
#include "lexer.h"
#include "parser.h"
#include "symbol_collector.h"
#include "symbols.h"
#include "type_checker.h"

struct CliOptions {
    std::string command;      // "parse" | "check" | "build"
    bool noOpt = false;
    std::string inputFile;
    std::string outputFile;
};

// Returns false + prints usage to stderr on bad arguments.
bool parseCli(int argc, char** argv, CliOptions& out) {
    if (argc < 2) return false;
    std::string cmd = argv[1];
    if (cmd != "parse" && cmd != "check" && cmd != "build") return false;
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
    return true;
}

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open file: " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::printf("cider 0.2.0\n");
        return 0;
    }
    CliOptions opts;
    if (!parseCli(argc, argv, opts)) {
        std::fprintf(stderr, "usage: cider <parse|check|build> [--no-opt] [-o out.asm] <file>\n");
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
        cider::SymbolCollector collector(table, rep);
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
        std::fprintf(stderr, "build: IR generation arrives in Phase 4 task 2\n");
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s: %s\n", opts.inputFile.c_str(), e.what());
        return 1;
    }
}
