#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "codegen.h"
#include "ir_text.h"
#include "vm.h"

struct CliOptions {
    std::string command;      // "run" | "compile"
    bool noOpt = false;
    std::string inputFile;
    std::string outputFile;   // compile only
};

// Returns false + prints usage to stderr on bad arguments.
bool parseCli(int argc, char** argv, CliOptions& out) {
    if (argc < 2) return false;
    std::string cmd = argv[1];
    if (cmd != "run" && cmd != "compile") return false;
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

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::printf("cider 0.2.0\n");
        return 0;
    }
    CliOptions opts;
    if (!parseCli(argc, argv, opts)) {
        std::fprintf(stderr, "usage: cider <run|compile> [--no-opt] [-o out.asm] <file.ir>\n");
        return 2;
    }
    if (opts.command == "run") {
        try {
            std::string text = readFile(opts.inputFile);
            cider::Program prog = cider::parseIR(text);
            cider::VMResult r = cider::runVM(prog);
            std::fwrite(r.stdoutText.data(), 1, r.stdoutText.size(), stdout);
            std::fwrite(r.stderrText.data(), 1, r.stderrText.size(), stderr);
            return r.exitCode;
        } catch (const cider::ParseError& e) {
            std::fprintf(stderr, "%s:%d: %s\n", opts.inputFile.c_str(), e.line, e.message.c_str());
            return 1;
        } catch (const cider::VMError& e) {
            std::fprintf(stderr, "%s: %s\n", opts.inputFile.c_str(), e.message.c_str());
            return 1;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "%s: %s\n", opts.inputFile.c_str(), e.what());
            return 1;
        }
    }
    try {
        std::string text = readFile(opts.inputFile);
        cider::Program prog = cider::parseIR(text);
        std::string asmText = cider::generateAsm(prog);
        std::ofstream out(opts.outputFile);
        if (!out) {
            std::fprintf(stderr, "cannot write %s\n", opts.outputFile.c_str());
            return 1;
        }
        out << asmText;
        return 0;
    } catch (const cider::ParseError& e) {
        std::fprintf(stderr, "%s:%d: %s\n", opts.inputFile.c_str(), e.line, e.message.c_str());
        return 1;
    } catch (const cider::CodegenError& e) {
        std::fprintf(stderr, "%s: %s\n", opts.inputFile.c_str(), e.message.c_str());
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s: %s\n", opts.inputFile.c_str(), e.what());
        return 1;
    }
}
