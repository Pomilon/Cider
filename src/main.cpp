#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <vector>

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
#include "module_meta.h"
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
    bool warn = false;        // CIDER-FEAT-005: opt-in diagnostics
    bool checks = false;      // CIDER-ISS-001: opt-in runtime bounds checks
    bool debug = false;       // CIDER-FEAT-001: DWARF in compile output
    bool emitMeta = false;    // CIDER-FEAT-006: module mode, write .cider-meta
    std::string cacheDir;     // CIDER-FEAT-006: .cider-meta cache directory
    std::string inputFile;
    std::string outputFile;
    std::string target = "native";  // canonical: "native" | "pomeos" | "uefi"
};

// CIDER-ISS-006: the CodeGenerator names the native ELF/x86-64 output
// `x86_64-linux` (its constructor default); the CLI has always called it
// `native`. Accept both backend spellings plus the short `linux` and
// canonicalize them to `native`, so every downstream target check (uefi ABI
// typing, codegen) sees one value. Returns "" for an unknown target.
std::string canonicalTarget(const std::string& target) {
    if (target == "native" || target == "linux" || target == "x86_64-linux") return "native";
    if (target == "pomeos" || target == "uefi") return target;
    return "";
}

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
        } else if (a == "--warn") {
            // CIDER-FEAT-005: accepted by every command; only the checking
            // commands have an analysis pass to drive.
            out.warn = true;
        } else if (a == "--checks") {
            // CIDER-ISS-001: accepted by every command; only `run` and
            // `compile` have an IR-gen path where bounds checks can be
            // emitted, so elsewhere the flag is a no-op.
            out.checks = true;
        } else if (a == "-g" || a == "--debug") {
            // CIDER-FEAT-001: DWARF debug info. Accepted by every command;
            // only `compile` emits anything (the others have no output).
            out.debug = true;
        } else if (a == "--cache") {
            // CIDER-FEAT-006: `.cider-meta` cache directory.
            if (i + 1 >= argc) return false;
            out.cacheDir = argv[++i];
        } else if (a == "--emit-meta") {
            // CIDER-FEAT-006: compile this file as a module and write its
            // `.cider-meta` snapshot into the cache directory.
            out.emitMeta = true;
        } else if (a == "--target") {
            if (i + 1 >= argc) return false;
            // CIDER-ISS-006: validate + canonicalize here; the accepted set
            // lives in canonicalTarget() above.
            out.target = canonicalTarget(argv[++i]);
            if (out.target.empty()) return false;
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
    // CIDER-FEAT-006: --emit-meta writes `<cache-dir>/<module>.cider-meta`;
    // without a cache directory there is nowhere to write, and `parse` /
    // `bundle` never build a symbol table to snapshot.
    if (out.emitMeta) {
        if (out.cacheDir.empty()) return false;
        if (out.command == "parse" || out.command == "bundle") return false;
    }
    // CIDER-FEAT-006: `run` executes the whole program on the VM, which
    // needs every function body; a meta import is an extern declaration
    // with no body, so the cache flags do not apply to it.
    if (out.command == "run" && (out.emitMeta || !out.cacheDir.empty())) return false;
    return true;
}

// CIDER-BUG-258: the one wording for a failed output write, shared by every
// path that produces output — the `-o` artifacts, the `.cider-meta` cache and
// the stdout/stderr dumps — so a failed write is greppable the same way
// everywhere. `target` is the path, or `<stdout>` / `<stderr>` for a stream.
// See printHelp's exit-code table: this is an exit-1 failure.
std::string cannotWrite(const std::string& target) {
    return "cannot write " + target;
}

// CIDER-BUG-258: write `content` to `path`.
//
// The open was always checked; the WRITE was not. A std::ofstream reports a
// failed write on its own state (failbit/badbit) and never throws, so a full
// disk, a full device or any other ENOSPC/EIO/EFBIG reached the destructor
// and was discarded: `compile -o` and `bundle -o` exited 0 over an empty or
// truncated file, and the cache snapshot said "module built" over garbage.
//
// close() is what flushes the last buffer, so it is also where a small
// output's write failure surfaces (a few KB never fills the buffer) — the
// check has to come after it, and it then covers a mid-stream failure too.
// The wording stays the driver's existing fixed one, so the generic handler
// reports `<input>: cannot write <target>` with exit 1, as it always has for
// the open failure here.
void writeFileOrThrow(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error(cannotWrite(path));
    out << content;
    out.close();
    if (!out) throw std::runtime_error(cannotWrite(path));
}

// CIDER-BUG-258: write `bytes` to a C stream and report a failed write as
// `cannot write <name>`, returning false so the caller can `return 1`.
//
// Both halves matter. stderr is unbuffered, so a rejected write shows up at
// once as a short fwrite and a later flush has nothing left to say about it;
// stdout is buffered, so the bytes are accepted into the buffer and only the
// flush finds out that the kernel refused them. A stream with nothing to write
// reports nothing: no bytes were lost.
//
// `name` is the target the message names: `<stdout>` or `<stderr>`.
bool writeStreamOrReport(std::FILE* stream, const char* name, const std::string& bytes) {
    if (!bytes.empty() && std::fwrite(bytes.data(), 1, bytes.size(), stream) != bytes.size()) {
        std::fprintf(stderr, "%s\n", cannotWrite(name).c_str());
        return false;
    }
    if (std::fflush(stream) == 0) return true;
    std::fprintf(stderr, "%s\n", cannotWrite(name).c_str());
    return false;
}

// CIDER-BUG-258: the same check for the one stdout write the driver does not
// make with fwrite (`parse`'s printf). stdout is never unbuffered, so printf
// succeeds into the buffer and the flush is where a failed write appears.
bool flushStreamOrReport(std::FILE* stream, const char* name) {
    if (std::fflush(stream) == 0) return true;
    std::fprintf(stderr, "%s\n", cannotWrite(name).c_str());
    return false;
}

// CIDER-BUG-258: the same check for the IR dump (`build`, and `compile` under
// CIDER_DEBUG_IR), which src/ir.cpp prints through std::cout rather than
// stdio. Without the explicit flush the error would only show up in the
// static destructor at exit, i.e. never.
bool flushCoutOrReport() {
    std::cout.flush();
    if (static_cast<bool>(std::cout)) return true;
    std::fprintf(stderr, "%s\n", cannotWrite("<stdout>").c_str());
    return false;
}

// `mkdir -p` for the --cache directory.
void makeDirs(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur += path[i];
        if (path[i] == '/' && cur.size() > 1) {
            mkdir(cur.c_str(), 0755);
        }
    }
    if (!path.empty()) mkdir(path.c_str(), 0755);
}

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open file: " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// CIDER-BUG-129: test-only allocation-failure injection, so the CLI's OOM
// path can be exercised deterministically and per stage instead of by
// exhausting host memory (see tests/unit/compiler_oom_smoke.sh).
//
//   CIDER_FAULT_ALLOC=<stage>       a std::bad_alloc at that stage
//   CIDER_FAULT_ALLOC=<stage>:len   a std::length_error at that stage
//
// Unset in every normal run: one getenv() the first time any stage asks, then
// a strncmp per stage and nothing else.
//
// The injected failures are REAL allocation failures rather than a bare
// `throw`, so the diagnostic path under test is the one memory pressure
// actually takes — and neither touches a page, so the test never needs (or
// spends) real memory.
static void faultInjectAlloc(const char* stage) {
    static const char* kWanted = std::getenv("CIDER_FAULT_ALLOC");
    if (kWanted == nullptr || *kWanted == '\0') return;
    const size_t n = std::strlen(stage);
    if (std::strncmp(kWanted, stage, n) != 0) return;
    if (std::strcmp(kWanted + n, ":len") == 0) {
        std::vector<char> v;
        v.resize(static_cast<size_t>(-1));
        return;   // unreachable: the resize above always throws
    }
    // A request larger than any address space: the allocator fails without
    // touching a page, so the throw is the only observable effect.
    void* p = ::operator new(static_cast<size_t>(-1));   // std::bad_alloc
    ::operator delete(p);                                // unreachable
}

// The optimization pipeline (main's fixpoint):
//   SSA LICM -> GVN (pre-decon), then per round: DCE, inline, post-LICM (<=10).
void optimizeModule(cider::ModuleIR& module) {
    // CIDER-BUG-129: fault-injection marker for the optimizer stage (the
    // pass fixpoint below is the stage's whole body). No-op unless
    // CIDER_FAULT_ALLOC names it.
    faultInjectAlloc("optimize");
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
// Prefixes distinguish the compiler stage (the error-code scheme). Warnings
// (CIDER-FEAT-005) use the severity-neutral `warning:` prefix.
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
        } catch (const std::runtime_error&) {
            // CIDER-BUG-129: readFile's only non-allocation failure is the
            // "cannot open file" throw, so the diagnostic falls back to the
            // main text. An allocation failure is deliberately NOT caught
            // here: this used to be `catch (...)`, which swallowed a bad_alloc
            // into a bogus source line and hid the real problem. Letting it
            // reach the driver's boundary reports `compiler: out of memory`.
            fileTexts[file] = text;
        }
        return fileTexts[file];
    };
    for (const auto& d : diags) {
        const std::string& file = d.filename.empty() ? path : d.filename;
        if (d.severity == cider::Diagnostic::Severity::Warning) {
            std::fprintf(stderr, "%s:%d:%d: warning: %s\n", file.c_str(),
                         d.line, d.col, d.message.c_str());
        } else {
            std::fprintf(stderr, "%s:%d:%d: %s error: %s\n", file.c_str(),
                         d.line, d.col, stage, d.message.c_str());
        }
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

// CIDER-FEAT-013: the long help. Every line is derived from the dispatch in
// parseCli() above and the branches in main() below, not from intent:
//   - the command list mirrors the accepted set in parseCli;
//   - each option's "(...)" note names the commands that actually consume it
//     (the flags are parsed for every command; the ones in a note are the
//     only ones that read it);
//   - the output-file rules mirror the per-command branches.
void printHelp() {
    std::printf(
        "usage: cider <command> [options] <file.cid>\n"
        "\n"
        "commands:\n"
        "  parse <file.cid>                 parse only, print the AST dump\n"
        "  check <file.cid>                 collect symbols and type check\n"
        "  build <file.cid>                 generate IR, print the module dump\n"
        "  run <file.cid>                   execute the program in the VM\n"
        "  compile <file.cid> -o out.asm    emit NASM assembly for the target\n"
        "  bundle <file.cid> [-o bundle.cid]  flatten a program into one file\n"
        "\n"
        "options:\n"
        "  --target <target>    native|linux|x86_64-linux|pomeos|uefi (default: native)\n"
        "  --no-opt             skip the optimizer (compile, run)\n"
        "  --warn               opt-in warnings on stderr; never changes the exit\n"
        "                       code (check, build, run, compile)\n"
        "  --checks             runtime bounds checks on fixed-size array indexing\n"
        "                       (compile, run)\n"
        "  -g, --debug          emit DWARF debug info into the output (compile;\n"
        "                       native target only)\n"
        "  -o <file>            output file: required by compile, optional for bundle,\n"
        "                       ignored by parse, check, build and run\n"
        "  --cache <dir>        separate compilation: read <dir>/<module>.cider-meta\n"
        "                       snapshots instead of the imported source\n"
        "  --emit-meta          compile this file as a module and write\n"
        "                       <dir>/<module>.cider-meta and <dir>/<module>.hash\n"
        "                       (requires --cache; rejected by parse, bundle, run)\n"
        "  -h, --help           print this help and exit 0\n"
        "  --version            print the version and exit 0\n"
        "\n"
        "separate compilation (--cache, --emit-meta) is also accepted by check and\n"
        "build. A bad command line prints the one-line usage on stderr and exits 2.\n"
        "\n"
        "exit codes:\n"
        "  0  success\n"
        "  1  parse, symbol, type or IR error; unreadable input; write failure\n"
        "  2  usage error (unknown command or flag, missing operand), a VM\n"
        "     runtime error under `run`, or the compiler or the VM running out\n"
        "     of memory (`compiler: out of memory` / `vm error: out of\n"
        "     memory`); `run` otherwise forwards the program's own exit code\n"
        "\n"
        "every failed write — an output file that cannot be opened or written,\n"
        "a `.cider-meta` snapshot, or stdout/stderr under `parse`, `build`,\n"
        "`bundle` and `run` — is reported as `cannot write <target>` and exits\n"
        "1. A partially written output is left in place, not removed: `-o` may\n"
        "name a device or a FIFO, so the driver never unlinks the target; check\n"
        "a stale file before reusing it.\n");
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::printf("cider 0.2.0\n");
        return 0;
    }
    // CIDER-FEAT-013: `--help` / `-h` / `help` print the long help and exit
    // 0. Only the first argument is inspected, so `cider <cmd> --help` keeps
    // its meaning: an unknown flag for that command, i.e. a usage error.
    if (argc >= 2) {
        const std::string first = argv[1];
        if (first == "--help" || first == "-h" || first == "help") {
            printHelp();
            return 0;
        }
    }
    CliOptions opts;
    if (!parseCli(argc, argv, opts)) {
        std::fprintf(stderr, "usage: cider <parse|check|build|run|compile|bundle> [--no-opt] [--warn] [--checks] [-g|--debug] [--cache <dir>] [--emit-meta] [--target native|linux|x86_64-linux|pomeos|uefi] [-o out.asm] <file>\n");
        return 2;
    }
    try {
        // CIDER-BUG-129: bundle is self-contained (see bundler.h) — it runs
        // before, and instead of, the normal parse/collect/check pipeline —
        // but it is still a compiler stage, so it lives inside the same
        // boundary as the rest. Outside it, an allocation failure escaped
        // main and std::terminate printed the raw C++ type name and aborted.
        if (opts.command == "bundle") {
            faultInjectAlloc("bundle");
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
                // CIDER-BUG-258: the open was checked but the write was not,
                // so `bundle -o /dev/full` exited 0. Same close-then-check as
                // writeFileOrThrow above (which the cache path uses), and the
                // same wording this path already used for a failed open.
                out.close();
                if (!out) {
                    std::fprintf(stderr, "cannot write %s\n", opts.outputFile.c_str());
                    return 1;
                }
            } else {
                // CIDER-BUG-258: `bundle f.cid > /dev/full` lost the bundle and
                // still exited 0.
                if (!writeStreamOrReport(stdout, "<stdout>", r.output)) return 1;
            }
            return 0;
        }
        faultInjectAlloc("read");
        std::string text = readFile(opts.inputFile);
        cider::ErrorReporter rep;
        rep.setFile(opts.inputFile);
        faultInjectAlloc("lex");
        cider::Lexer lex(text, rep);
        cider::Parser parser(lex, rep);
        faultInjectAlloc("parse");
        auto prog = parser.parseProgram();
        if (rep.hasErrors()) {
            printDiagnostics(text, opts.inputFile, rep.errors(), "parse");
            return 1;
        }
        if (opts.command == "parse") {
            std::printf("%s\n", cider::dump(*prog).c_str());
            // CIDER-BUG-258: the AST dump is an output too — a full stdout used
            // to swallow the failure and exit 0.
            if (!flushStreamOrReport(stdout, "<stdout>")) return 1;
            return 0;
        }
        faultInjectAlloc("symbol");
        cider::SymbolTable table;
        cider::ModuleResolver resolver;
        cider::SymbolCollector collector(table, rep, resolver, opts.inputFile);
        // CIDER-FEAT-006: separate-compilation mode. `--cache` makes imports
        // read `<dir>/<module>.cider-meta` snapshots (falling back to
        // source); `--emit-meta` compiles this file as a named module and
        // snapshots its declarations after checking.
        const bool moduleMode = opts.emitMeta;
        const bool separateCompilation = !opts.cacheDir.empty() || opts.emitMeta;
        std::string rootModuleName;
        if (moduleMode) rootModuleName = cider::deriveModuleName(opts.inputFile);
        collector.setCacheDir(opts.cacheDir);
        collector.setSeparateCompilation(separateCompilation);
        if (moduleMode) {
            collector.collectModule(*prog, rootModuleName);
        } else {
            collector.collect(*prog);
        }
        if (rep.hasErrors()) {
            printDiagnostics(text, opts.inputFile, rep.errors(), "symbol");
            return 1;
        }
        rep.clear();
        cider::TypeChecker checker(table, rep, moduleMode);
        checker.setModuleScopes(collector.getModuleScopes());
        checker.setProgramModules(collector.getProgramModules());
        checker.setAmbiguousModules(collector.getAmbiguousModules());
        checker.setSymbolModules(collector.getSymbolModules());
        checker.setQualifiedTemplateInstantiations(collector.getQualifiedTemplateInstantiations());
        checker.setTemplateInstanceIdentities(collector.getTemplateInstanceIdentities());
        checker.setLocalAggregates(collector.getLocalAggregates());
        checker.setModuleTemplateNames(collector.getModuleTemplateNames());
        // CID-005: the uefi target types unmarked functions MS-ABI and
        // allows `efi_main` as the entry point (check and compile alike).
        const bool defaultAbiMs = (opts.target == "uefi");
        checker.setDefaultAbiMs(defaultAbiMs);
        checker.setUefiEntryAllowed(defaultAbiMs);
        // CIDER-FEAT-005: warnings are opt-in. They are collected and
        // printed only with --warn and never change the exit code.
        checker.setWarningsEnabled(opts.warn);
        faultInjectAlloc("typecheck");
        checker.check(*prog);
        if (rep.hasErrors()) {
            printDiagnostics(text, opts.inputFile, rep.errors(), "type");
            return 1;
        }
        if (opts.warn && rep.hasWarnings()) {
            printDiagnostics(text, opts.inputFile, rep.warnings(), "type");
        }
        // CIDER-FEAT-006: module mode writes the declaration snapshot and the
        // source hash into the cache directory, after checking so a failed
        // module never lands in the cache.
        if (opts.emitMeta) {
            faultInjectAlloc("meta");
            cider::Scope* moduleScope = table.getRootScope();
            auto scopeIt = collector.getModuleScopes().find(opts.inputFile);
            if (scopeIt != collector.getModuleScopes().end()) moduleScope = scopeIt->second;
            cider::MetaEmitResult meta = cider::MetaEmitter::emit(
                *prog, text, rootModuleName, moduleScope, collector.getTemplateTable());
            makeDirs(opts.cacheDir);
            writeFileOrThrow(
                opts.cacheDir + "/" + cider::metaFileName(rootModuleName, ".cider-meta"),
                meta.text);
            writeFileOrThrow(
                opts.cacheDir + "/" + cider::metaFileName(rootModuleName, ".hash"),
                meta.hash);
        }
        if (opts.command == "check") {
            return 0;
        }
        if (opts.command == "compile") {
            cider::IRGenerator gen(table, opts.inputFile);
            gen.setModuleScopes(collector.getModuleScopes());
            gen.setProgramModules(collector.getProgramModules());
            gen.setSymbolModules(collector.getSymbolModules());
            gen.setQualifiedTemplateInstantiations(collector.getQualifiedTemplateInstantiations());
            gen.setTemplateInstanceIdentities(collector.getTemplateInstanceIdentities());
            gen.setLocalAggregates(collector.getLocalAggregates());
            gen.setDefaultAbiMs(defaultAbiMs);
            gen.setReporter(&rep);
            // CIDER-BUG-184: interface returns that box a concrete value
            // get one program-wide box representation (tagged boxes with a
            // caller-side release); programs that never box stay
            // byte-identical.
            gen.setInterfaceBoxing(checker.hasInterfaceBoxing());
            // CIDER-FEAT-007: `sizeof e` is lowered from the operand TYPE the
            // checker recorded; the operand itself is never generated.
            gen.setSizeofOperandTypes(checker.getSizeofOperandTypes());
            gen.setChecks(opts.checks);  // CIDER-ISS-001
            // CIDER-FEAT-006: per-object data labels when this is one object
            // of a separate-compilation build.
            gen.setSeparateCompilation(separateCompilation);
            faultInjectAlloc("ir_gen");
            auto module = gen.generate(*prog);
            if (rep.hasErrors()) {
                printDiagnostics(text, opts.inputFile, rep.errors(), "ir");
                return 1;
            }
            if (!opts.noOpt) {
                optimizeModule(module);
            }
            cider::SSADeconstructor decon;
            faultInjectAlloc("deconstruct");
            decon.process(module);
            if (std::getenv("CIDER_DEBUG_IR")) {
                module.print();
                // CIDER-BUG-258: the IR dump is an output artifact like any
                // other (this is the `build` dump on the debug path).
                if (!flushCoutOrReport()) return 1;
            }
            cider::CodeGenerator cg(opts.target);
            // CIDER-FEAT-001: -g emits DWARF line tables + function DIEs.
            cg.setDebugInfo(opts.debug);
            cg.setSourcePath(opts.inputFile);
            // CIDER-FEAT-006: in separate-compilation mode (`--cache` /
            // `--emit-meta`) codegen emits only this file's functions;
            // imported definitions become extern references resolved at link
            // time. Without the flags the whole-program output is unchanged.
            const std::string codegenSource =
                separateCompilation ? opts.inputFile : std::string();
            faultInjectAlloc("codegen");
            if (!cg.generate(module, opts.outputFile, codegenSource)) {
                // CIDER-BUG-258: generate() now also returns false when a
                // WRITE failed (it checked only the open before), so this one
                // message covers both and a full disk no longer exits 0.
                std::fprintf(stderr, "cannot write %s\n", opts.outputFile.c_str());
                return 1;
            }
            return 0;
        }
        if (opts.command == "run") {
            cider::IRGenerator gen(table, opts.inputFile);
            gen.setModuleScopes(collector.getModuleScopes());
            gen.setProgramModules(collector.getProgramModules());
            gen.setSymbolModules(collector.getSymbolModules());
            gen.setQualifiedTemplateInstantiations(collector.getQualifiedTemplateInstantiations());
            gen.setTemplateInstanceIdentities(collector.getTemplateInstanceIdentities());
            gen.setLocalAggregates(collector.getLocalAggregates());
            gen.setDefaultAbiMs(defaultAbiMs);
            gen.setReporter(&rep);
            // CIDER-BUG-184: interface returns that box a concrete value
            // get one program-wide box representation (tagged boxes with a
            // caller-side release); programs that never box stay
            // byte-identical.
            gen.setInterfaceBoxing(checker.hasInterfaceBoxing());
            // CIDER-FEAT-007: `sizeof e` is lowered from the operand TYPE the
            // checker recorded; the operand itself is never generated.
            gen.setSizeofOperandTypes(checker.getSizeofOperandTypes());
            gen.setChecks(opts.checks);  // CIDER-ISS-001
            faultInjectAlloc("ir_gen");
            auto module = gen.generate(*prog);
            if (rep.hasErrors()) {
                printDiagnostics(text, opts.inputFile, rep.errors(), "ir");
                return 1;
            }
            if (!opts.noOpt) {
                optimizeModule(module);
            }
            // CIDER-BUG-204: deliberately NO SSADeconstructor here, unlike
            // the compile path above. The VM executes the PHI form of the IR
            // (its own interpreter resolves phi edges and rejects the
            // deconstructor's PUSH/POP); the deconstructor is a native-only
            // physical-register lowering. See src/ssa_deconstructor.h.
            cider::VM vm;
            try {
                cider::VMResult r = vm.run(module);
                // CIDER-BUG-258: the driver's copy of the program's output is
                // an output write too — `run > /dev/full` used to lose it
                // silently and still forward the program's exit code. A
                // program that printed nothing writes no bytes and correctly
                // reports nothing; one whose bytes were rejected gets this
                // diagnostic and exit 1 (the documented write failure) in
                // place of the program's own exit code.
                if (!writeStreamOrReport(stdout, "<stdout>", r.stdoutText)) return 1;
                if (!writeStreamOrReport(stderr, "<stderr>", r.stderrText)) return 1;
                return r.exitCode;
            } catch (const cider::VMError& e) {
                // CIDER-BUG-154: an error may carry bytes the program
                // printed before failing (the uncaught-exception path
                // flushes destructor output); emit them first so VM stdout
                // matches native's raw syscalls byte for byte.
                std::fprintf(stderr, "vm error: %s\n", e.message.c_str());
                // CIDER-BUG-258: same check on the path above.
                if (!writeStreamOrReport(stdout, "<stdout>", e.stdoutText)) return 1;
                return 2;
            }
        }
        // build: generate IR and print the module dump.
        cider::IRGenerator gen(table, opts.inputFile);
        gen.setModuleScopes(collector.getModuleScopes());
        gen.setProgramModules(collector.getProgramModules());
        gen.setSymbolModules(collector.getSymbolModules());
        gen.setQualifiedTemplateInstantiations(collector.getQualifiedTemplateInstantiations());
        gen.setTemplateInstanceIdentities(collector.getTemplateInstanceIdentities());
        gen.setLocalAggregates(collector.getLocalAggregates());
        gen.setDefaultAbiMs(defaultAbiMs);
        gen.setReporter(&rep);
        // CIDER-BUG-184: boxed interface returns use the program-wide tagged
        // representation when the checker found one (see setInterfaceBoxing).
        gen.setInterfaceBoxing(checker.hasInterfaceBoxing());
        // CIDER-FEAT-007: `sizeof e` is lowered from the operand TYPE the
        // checker recorded; the operand itself is never generated.
        gen.setSizeofOperandTypes(checker.getSizeofOperandTypes());
        faultInjectAlloc("ir_gen");
        auto module = gen.generate(*prog);
        if (rep.hasErrors()) {
            printDiagnostics(text, opts.inputFile, rep.errors(), "ir");
            return 1;
        }
        module.print();
        // CIDER-BUG-258: the IR dump is this command's whole output.
        if (!flushCoutOrReport()) return 1;
        return 0;
    } catch (const std::bad_alloc&) {
        // CIDER-BUG-129: THE allocation-failure boundary for every compiler
        // stage (bundle, read, lex, parse, symbol, typecheck, meta, ir_gen,
        // optimize, deconstruct, codegen) plus the VM run. Before this the
        // generic `catch (std::exception&)` below reported an allocation
        // failure as `<file>: std::bad_alloc` with exit 1 — a C++ type name
        // where a diagnostic belongs, and exit 1, which means "the program is
        // wrong" (a parse/type/IR error). The VM already had its own
        // convention (CIDER-BUG-121): `vm error: out of memory` with exit 2.
        // OOM now uses that same exit code 2 everywhere, with the stage-free
        // `compiler: out of memory` message the VM's counterpart is modelled
        // on. std::bad_array_new_length derives from std::bad_alloc, so a
        // `new T[n]` with a bogus count is covered by this clause too.
        std::fprintf(stderr, "compiler: out of memory\n");
        return 2;
    } catch (const std::length_error&) {
        // CIDER-BUG-129: std::string / std::vector growth past max_size()
        // throws length_error (a std::logic_error, NOT a std::bad_alloc), so
        // the clause above alone would let it reach the generic handler as
        // the bare string "basic_string::_M_create". Same convention.
        std::fprintf(stderr, "compiler: out of memory\n");
        return 2;
    } catch (const std::exception& e) {
        // CIDER-BUG-129: still no `std::` name reaches the user — the
        // driver-level failures that are not allocation failures all have a
        // fixed wording already (see readFile / writeFileOrThrow).
        std::fprintf(stderr, "%s: %s\n", opts.inputFile.c_str(), e.what());
        return 1;
    }
}
