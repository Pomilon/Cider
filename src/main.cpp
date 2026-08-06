#include <cstdio>
#include <string>

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
        std::fprintf(stderr, "run: not implemented yet\n");
        return 1;
    }
    std::fprintf(stderr, "compile: not implemented yet\n");
    return 1;
}
