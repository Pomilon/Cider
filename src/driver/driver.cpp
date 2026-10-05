// driver.cpp — the `cider` command behind a testable function.
//
// check: lex, parse, resolve and check; diagnostics name file, line and
//   column, and success is quiet.
// compile: check, then emit assembly to a file or standard output.
// ir: check, then dump the module for people to read.
// run: compile to a temporary file, assemble and link it, execute it, and
//   answer its exit code.
// link, bundle: refused by name. A pipeline stage that does not exist must say
//   so rather than look like it worked.

#include "driver/driver.h"

#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <fstream>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

#include "check/checker.h"
#include "codegen/codegen.h"
#include "lexer/lexer.h"
#include "lower/lower.h"
#include "opt/opt.h"
#include "parse/parser.h"
#include "types/context.h"

namespace cider {
namespace driver {

namespace {

struct Pipeline {
    bool ok = false;
    std::string file;
    std::string diagnostic;
    std::unique_ptr<TypeContext> types;
    std::unique_ptr<Program> program;
    std::unique_ptr<ir::Module> module;
};

std::string at(const Position& pos) {
    return pos.file + ":" + std::to_string(pos.line) + ":" + std::to_string(pos.column);
}

std::string readFile(const std::string& path, bool& found);

bool runFront(const std::string& file, const std::string& text, Pipeline& pipe) {
    pipe.file = file;
    pipe.types = std::make_unique<TypeContext>();
    // Imports load beside the entry file: `import da;` reads `<dir>/da.cid`
    // and parses it with the same front end, so a dependency's lex and
    // parse diagnostics name its own file. A dotted name (`std.io`) has no
    // library search behind it yet and fails loudly at resolve time. The
    // program is returned even when broken: the parser recovers, downstream
    // phases report only true cascades, and the recorded diagnostics fail
    // the build before lowering runs.
    std::string entryDir;
    std::string::size_type slash = file.rfind('/');
    if (slash != std::string::npos) {
        entryDir = file.substr(0, slash);
    }
    std::string loadDiag;
    // Dotted names (`std.io`) resolve through the library, not the entry
    // directory: CIDER_LIB names the tree root and dots become separators,
    // so `std.io` reads `$CIDER_LIB/std/io.cid`. Without the variable a
    // dotted import still fails loudly at resolve time. Plain names stay
    // entry-relative (FEAT-047).
    const char* libRoot = std::getenv("CIDER_LIB");
    std::string lib = libRoot != nullptr ? libRoot : "";
    auto loadFile = [&loadDiag](const std::string& path) -> std::unique_ptr<Program> {
        bool found = false;
        std::string depText = readFile(path, found);
        if (!found) {
            return nullptr;
        }
        Lexer lexer(path, depText);
        std::vector<Token> tokens = lexer.lex();
        for (const LexError& error : lexer.errors()) {
            loadDiag += at(error.pos) + ": " + error.message + "\n";
        }
        Parser parser(path, std::move(tokens));
        std::unique_ptr<Program> program = parser.parse();
        for (const ParseError& error : parser.errors()) {
            loadDiag += at(error.pos) + ": " + error.message + "\n";
        }
        return program;
    };
    resolve::Resolver::ModuleLoader load =
        [&entryDir, &lib, &loadFile](const std::string& name) -> std::unique_ptr<Program> {
        if (name.find('.') != std::string::npos) {
            if (lib.empty()) {
                return nullptr;
            }
            std::string rel = name;
            auto dot = std::string::npos;
            while ((dot = rel.find('.', dot == std::string::npos ? 0 : dot)) !=
                   std::string::npos) {
                rel[dot] = '/';
            }
            return loadFile(lib + "/" + rel + ".cid");
        }
        std::string path = entryDir.empty() ? name + ".cid" : entryDir + "/" + name + ".cid";
        return loadFile(path);
    };
    Lexer lexer(file, text);
    std::vector<Token> tokens = lexer.lex();
    for (const LexError& error : lexer.errors()) {
        pipe.diagnostic += at(error.pos) + ": " + error.message + "\n";
    }
    Parser parser(file, std::move(tokens));
    pipe.program = parser.parse();
    for (const ParseError& error : parser.errors()) {
        pipe.diagnostic += at(error.pos) + ": " + error.message + "\n";
    }
    if (!pipe.diagnostic.empty()) {
        return false;
    }
    resolve::Resolver resolver(*pipe.types, std::move(load));
    if (!resolver.resolveProgram(*pipe.program, "main")) {
        for (const resolve::ResolveError& error : resolver.errors()) {
            pipe.diagnostic += at(error.pos) + ": " + error.message + "\n";
        }
        pipe.diagnostic += loadDiag;
        return false;
    }
    if (!loadDiag.empty()) {
        pipe.diagnostic += loadDiag;
        for (const resolve::ResolveError& error : resolver.errors()) {
            pipe.diagnostic += at(error.pos) + ": " + error.message + "\n";
        }
        return false;
    }
    check::Checker checker(*pipe.types, resolver);
    lower::Lowerer lowerer(*pipe.types, resolver, checker);
    // Every resolved module lowers into one IR module in resolve order, so
    // link names mangle under their own module and rodata labels never
    // collide. Emission order carries no semantics: calls resolve by link
    // name, not by position.
    bool lowered = false;
    for (const std::string& module : resolver.moduleOrder()) {
        Program* program =
            module == "main" ? pipe.program.get() : resolver.programOf(module);
        if (program == nullptr) {
            pipe.diagnostic += "cannot lower missing module '" + module + "'\n";
            return false;
        }
        if (module == "main") {
            lowered = lowerer.lowerModule(module, *program);
        } else {
            lowered = lowerer.lowerInto(module, *program);
        }
        if (!lowered) {
            break;
        }
    }
    if (lowered) {
        pipe.module = lowerer.takeModule();
    }
    // The checker calls back into the resolver (local spellings,
    // instantiations), so resolve diagnostics can appear after the header
    // pass succeeded. A successful resolve records none, so anything here
    // is new and must fail the build rather than vanish.
    bool resolveFailed = !resolver.errors().empty();
    if (pipe.module == nullptr || resolveFailed) {
        for (const resolve::ResolveError& error : resolver.errors()) {
            pipe.diagnostic += at(error.pos) + ": " + error.message + "\n";
        }
        for (const check::CheckError& error : checker.errors()) {
            pipe.diagnostic += at(error.pos) + ": " + error.message + "\n";
        }
        for (const check::CheckError& error : lowerer.errors()) {
            pipe.diagnostic += at(error.pos) + ": " + error.message + "\n";
        }
        return false;
    }
    opt::OptResult result = opt::run(*pipe.module);
    if (!result.ok) {
        pipe.diagnostic += file + ": " + result.error + "\n";
        return false;
    }
    pipe.ok = true;
    return true;
}

std::string readFile(const std::string& path, bool& found) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        found = false;
        return "";
    }
    found = true;
    std::ostringstream text;
    text << stream.rdbuf();
    return text.str();
}

bool writeFile(const std::string& path, const std::string& text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return false;
    }
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(stream);
}

int runBinary(const std::string& path) {
    pid_t child = fork();
    if (child == 0) {
        execl(path.c_str(), path.c_str(), nullptr);
        _exit(127);
    }
    int status = 0;
    waitpid(child, &status, 0);
    if (WIFSIGNALED(status)) {
        // A trapped program dies by the same signal, the way a shell
        // re-raises: the death stays a death instead of becoming an exit
        // code. Collapsing it to 128+N would steal every legitimate exit
        // status in that band (a program may return 150), and the harness
        // tells the two apart only when the signal survives (OUTPUT.md).
        signal(WTERMSIG(status), SIG_DFL);
        raise(WTERMSIG(status));
        _exit(127);
    }
    if (!WIFEXITED(status)) {
        return 127;
    }
    return WEXITSTATUS(status);
}

}  // namespace

int runDriver(const std::vector<std::string>& args, std::string& out, std::string& err) {
    if (args.empty()) {
        err = "usage: cider <check|compile|ir|run|link|bundle> [options] <file>\n";
        return 2;
    }
    std::string command = args[0];
    if (command != "check" && command != "compile" && command != "ir" &&
        command != "run" && command != "link" && command != "bundle") {
        err = "unknown subcommand '" + command + "'\n";
        return 2;
    }
    if (command == "link" || command == "bundle") {
        err = command + " is not implemented yet\n";
        return 2;
    }
    std::string target = "native";
    std::string output;
    std::string input;
    for (uint64_t i = 1; i < static_cast<uint64_t>(args.size()); ++i) {
        if (args[i] == "--target" && i + 1 < static_cast<uint64_t>(args.size())) {
            target = args[++i];
        } else if (args[i] == "-o" && i + 1 < static_cast<uint64_t>(args.size())) {
            output = args[++i];
        } else if (!args[i].empty() && args[i][0] == '-') {
            err = "unknown flag '" + args[i] + "'\n";
            return 2;
        } else if (input.empty()) {
            input = args[i];
        } else {
            err = "too many files\n";
            return 2;
        }
    }
    if (input.empty()) {
        err = "no input file\n";
        return 2;
    }
    if (target != "native") {
        err = "target " + target + " is not emitted yet: native only in this slice\n";
        return 2;
    }
    bool found = false;
    std::string text = readFile(input, found);
    if (!found) {
        err = "cannot open '" + input + "'\n";
        return 1;
    }
    Pipeline pipe;
    if (!runFront(input, text, pipe)) {
        err = pipe.diagnostic;
        return 1;
    }
    if (command == "check") {
        return 0;
    }
    if (command == "ir") {
        out = ir::dumpModule(*pipe.module);
        return 0;
    }
    codegen::EmitResult emitted = codegen::emitNative(*pipe.module);
    if (!emitted.ok) {
        err = at(emitted.pos) + ": " + emitted.error + "\n";
        return 1;
    }
    if (command == "compile") {
        if (output.empty()) {
            out = emitted.text;
            return 0;
        }
        if (!writeFile(output, emitted.text)) {
            err = "cannot write '" + output + "'\n";
            return 1;
        }
        return 0;
    }
    // run: assemble and link to temporaries, execute, answer the exit code.
    // Sovereign link: nasm plus ld with no libc and no runtime archive —
    // every symbol the text references, the text itself defines.
    char asmPath[] = "/tmp/cider_run_XXXXXX.s";
    int asmFd = mkstemps(asmPath, 2);
    if (asmFd < 0) {
        err = "cannot create a temporary file\n";
        return 1;
    }
    FILE* asmFile = fdopen(asmFd, "w");
    fwrite(emitted.text.data(), 1, emitted.text.size(), asmFile);
    fclose(asmFile);
    std::string objPath = std::string(asmPath) + ".o";
    std::string binPath = std::string(asmPath) + ".bin";
    std::string assembled =
        "as -o " + objPath + " " + std::string(asmPath) + " 2>/dev/null";
    std::string linked =
        "ld -nostdlib -e _start -o " + binPath + " " + objPath + " 2>/dev/null";
    bool built = system(assembled.c_str()) == 0 && system(linked.c_str()) == 0;
    unlink(asmPath);
    unlink(objPath.c_str());
    if (!built) {
        err = "assembling failed\n";
        return 1;
    }
    int code = runBinary(binPath);
    unlink(binPath.c_str());
    return code;
}

}  // namespace driver
}  // namespace cider
