// driver.cpp — the `cider` command behind a testable function.
//
// check: lex, parse, resolve and check; diagnostics name file, line and
//   column, and success is quiet.
// compile: check, then emit assembly to a file or standard output. With
//   `-c`, assemble that text into an ELF64 relocatable object; split-module
//   modes emit one module's definitions and leave imports external.
// ir: check, then dump the module for people to read.
// run: compile to a temporary file, assemble and link it, execute it, and
//   answer its exit code.
// link: link ELF64 objects with GNU ld, surfacing every tool diagnostic.
// bundle: refused by name. A pipeline stage that does not exist must say so
//   rather than look like it worked.

#include "driver/driver.h"

#include "target/target.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits.h>
#include <map>
#include <new>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "check/checker.h"
#include "codegen/codegen.h"
#include "interface/interface.h"
#include "lexer/lexer.h"
#include "lower/lower.h"
#include "opt/opt.h"
#include "parse/parser.h"
#include "types/context.h"
#include "vm/vm.h"

namespace cider {
namespace driver {

namespace {

// How much pending input one read pulls: a chunk, not a byte at a time.
constexpr uint64_t kPendingChunk = 4096;
constexpr uint64_t kOneByte = 1;

struct Pipeline {
    bool ok = false;
    std::string file;
    std::string diagnostic;
    std::unique_ptr<TypeContext> types;
    std::unique_ptr<Program> program;
    std::unique_ptr<ir::Module> module;
    // SHA-256 of every interface this front end loaded, by module. An
    // emitted interface records these as dependency hashes and the build
    // cache keys on them.
    std::map<std::string, std::string> interfaceHashes;
};

std::string at(const Position& pos) {
    return pos.file + ":" + std::to_string(pos.line) + ":" + std::to_string(pos.column);
}

std::string readFile(const std::string& path, bool& found);
bool writeFile(const std::string& path, const std::string& text);
bool endsWith(const std::string& text, const std::string& suffix);
// The bytes a read from standard input answers on the interpreter leg.
// The compiled leg's child inherits the same stream, so both see the same
// bytes; a terminal has nothing pending to give, and reading it would wait
// forever.
std::string readPendingInput() {
    if (isatty(0)) {
        return "";
    }
    std::string text;
    char chunk[kPendingChunk];
    uint64_t taken = std::fread(chunk, kOneByte, sizeof(chunk), stdin);
    while (taken > 0) {
        text.append(chunk, static_cast<size_t>(taken));
        taken = std::fread(chunk, kOneByte, sizeof(chunk), stdin);
    }
    return text;
}

// Lexical normalization only: no filesystem walk, no symlink resolution.
// The same spelling must resolve to the same candidate on every machine.
std::string normalizePath(const std::string& path) {
    const bool absolute = !path.empty() && path[0] == '/';
    std::vector<std::string> parts;
    std::string segment;
    auto flush = [&]() {
        if (segment.empty() || segment == ".") {
            segment.clear();
            return;
        }
        if (segment == "..") {
            if (!parts.empty() && parts.back() != "..") {
                parts.pop_back();
            } else if (!absolute) {
                parts.push_back("..");
            }
        } else {
            parts.push_back(segment);
        }
        segment.clear();
    };
    for (char c : path) {
        if (c == '/') {
            flush();
        } else {
            segment += c;
        }
    }
    flush();
    std::string out;
    uint64_t index = 0;
    for (const std::string& part : parts) {
        if (absolute || index > 0) {
            out += '/';
        }
        out += part;
        ++index;
    }
    if (out.empty()) {
        return absolute ? "/" : ".";
    }
    return out;
}

std::string dirName(const std::string& path) {
    std::string::size_type slash = path.rfind('/');
    if (slash == std::string::npos) {
        return ".";
    }
    if (slash == 0) {
        return "/";
    }
    return path.substr(0, slash);
}

std::string joinPath(const std::string& base, const std::string& rel) {
    if (base.empty()) {
        return rel;
    }
    return base.back() == '/' ? base + rel : base + "/" + rel;
}

std::vector<std::string> dirsAbove(const std::string& start) {
    std::vector<std::string> dirs;
    std::string current = normalizePath(start);
    for (int depth = 0; depth < 64; ++depth) {
        dirs.push_back(current);
        std::string parent = dirName(current);
        if (parent == current) {
            break;
        }
        current = parent;
    }
    return dirs;
}

std::string currentDir() {
    char buf[PATH_MAX];
    if (getcwd(buf, PATH_MAX) == nullptr) {
        return ".";
    }
    return normalizePath(buf);
}

std::string binaryDir() {
    char buf[PATH_MAX];
    long n = readlink("/proc/self/exe", buf, PATH_MAX - 1);
    if (n <= 0) {
        return "";
    }
    buf[n] = '\0';
    return dirName(normalizePath(buf));
}

std::vector<std::string> moduleRoots(const std::string& entryDir) {
    std::vector<std::string> roots;
    auto add = [&](const std::string& path) {
        std::string clean = normalizePath(path);
        for (const std::string& have : roots) {
            if (have == clean) {
                return;
            }
        }
        roots.push_back(std::move(clean));
    };
    const std::string origin = entryDir.empty() ? "." : entryDir;
    add(origin);
    add(joinPath(origin, "modules"));
    const std::string here = currentDir();
    add(here);
    add(joinPath(here, "modules"));
    add(joinPath(here, "Cider/modules"));
    for (const std::string& dir : dirsAbove(dirName(here))) {
        add(joinPath(dir, "modules"));
    }
    const std::string bin = binaryDir();
    if (!bin.empty()) {
        add(bin);
        add(joinPath(bin, "modules"));
        for (const std::string& dir : dirsAbove(dirName(bin))) {
            add(joinPath(dir, "modules"));
        }
    }
    return roots;
}

// Where `import name;` finds its source. A dotted name prefers the library
// root (CIDER_LIB) and then the v2 search roots; a plain name stays
// entry-relative. An empty answer means no source exists, not that the module
// is unknown: an interface may still describe it.
std::string findModuleFile(const std::string& name, const std::string& lib,
                           const std::vector<std::string>& roots) {
    std::string rel = name;
    for (char& c : rel) {
        if (c == '.') {
            c = '/';
        }
    }
    if (name.find('.') != std::string::npos) {
        if (!lib.empty()) {
            std::string path =
                normalizePath(joinPath(lib, rel + ".cid"));
            if (access(path.c_str(), R_OK) == 0) {
                return path;
            }
        }
        for (const std::string& root : roots) {
            std::string path =
                normalizePath(joinPath(root, rel + ".cid"));
            if (access(path.c_str(), R_OK) == 0) {
                return path;
            }
        }
        return "";
    }
    for (const std::string& root : roots) {
        std::string path = normalizePath(joinPath(root, name + ".cid"));
        if (access(path.c_str(), R_OK) == 0) {
            return path;
        }
    }
    return "";
}

std::string interfaceArtifactPath(const std::string& objectPath) {
    constexpr std::string::difference_type kObjectSuffix = 2;
    if (endsWith(objectPath, ".o")) {
        return objectPath.substr(
                   0, objectPath.size() -
                          static_cast<std::string::size_type>(kObjectSuffix)) +
               ".cidm";
    }
    return objectPath + ".cidm";
}

std::string interfaceSearchPath(const std::string& dir,
                                const std::string& module) {
    if (dir.empty()) {
        return "";
    }
    return normalizePath(
        joinPath(dir, interfacefmt::modulePath(module) + ".cidm"));
}

void collectImportNames(const Program& program,
                        std::vector<std::string>& names) {
    for (const std::unique_ptr<Decl>& decl : program.decls) {
        std::string name;
        if (const auto* import = dynamic_cast<const ImportDecl*>(decl.get())) {
            name = import->module;
        } else if (const auto* from =
                       dynamic_cast<const FromImport*>(decl.get())) {
            name = from->module;
        }
        if (name.empty()) {
            continue;
        }
        bool seen = false;
        for (const std::string& have : names) {
            if (have == name) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            names.push_back(std::move(name));
        }
    }
}

std::vector<interfacefmt::Dependency> interfaceDependencies(
    const Program& program, const Pipeline& pipe) {
    std::vector<std::string> names;
    collectImportNames(program, names);
    std::vector<interfacefmt::Dependency> deps;
    for (const std::string& name : names) {
        interfacefmt::Dependency dep;
        dep.module = name;
        auto found = pipe.interfaceHashes.find(name);
        if (found != pipe.interfaceHashes.end()) {
            dep.hash = found->second;
        }
        deps.push_back(std::move(dep));
    }
    return deps;
}

// The interface beside an object is a sidecar, not a new required input. A
// failed write removes the object so a half-finished compile leaves neither
// artifact claiming to be current.
bool writeModuleInterface(const Pipeline& pipe, const std::string& module,
                          const std::string& sourceText,
                          const std::string& objectPath, std::string& error) {
    if (pipe.program == nullptr) {
        error = "cannot emit an interface for a missing module\n";
        return false;
    }
    interfacefmt::RenderResult rendered = interfacefmt::render(
        module, *pipe.program, interfacefmt::sha256(sourceText),
        interfaceDependencies(*pipe.program, pipe));
    const std::string path = interfaceArtifactPath(objectPath);
    if (!writeFile(path, rendered.text)) {
        error = "cannot write interface '" + path + "'\n";
        return false;
    }
    return true;
}

void faultAlloc(const char* stage);

struct FrontOptions {
    target::Kind targetKind = target::Kind::Native;
    bool noOpt = false;
    bool checks = false;
    bool requireEntry = true;
    // Whole-program is the existing compile/run path: every resolved module
    // lowers into one IR module. A split-module object lowers only `module`.
    bool wholeProgram = true;
    // A non-entry module object mangles every definition, including this
    // file's, so its names match the importing entry object's references.
    bool mangleAll = false;
    std::string module = "main";
    // Interface artifacts replace imported source bodies. Off by default,
    // so every existing path keeps resolving source exactly as before.
    bool useInterfaces = false;
    bool requireInterfaces = false;
    bool allowStaleInterfaces = false;
    std::string interfaceDir;
};

bool runFront(const std::string& file, const std::string& text, Pipeline& pipe,
              const FrontOptions& opts) {
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
    const std::vector<std::string> roots = moduleRoots(entryDir);
    auto loadSource = [&lib, &loadFile,
                       &roots](const std::string& name) -> std::unique_ptr<Program> {
        if (name.find('.') != std::string::npos) {
            std::string rel = name;
            for (char& c : rel) {
                if (c == '.') {
                    c = '/';
                }
            }
            // Library names keep their priority, so `std.io` still
            // answers from the library. A miss falls back through the
            // v2 search roots, where `import a.util` can answer from the
            // entry directory, a modules/ tree, or an ancestor.
            if (!lib.empty()) {
                std::unique_ptr<Program> found =
                    loadFile(normalizePath(joinPath(lib, rel + ".cid")));
                if (found != nullptr) {
                    return found;
                }
            }
            for (const std::string& root : roots) {
                std::unique_ptr<Program> found =
                    loadFile(normalizePath(joinPath(root, rel + ".cid")));
                if (found != nullptr) {
                    return found;
                }
            }
            return nullptr;
        }
        for (const std::string& root : roots) {
            std::unique_ptr<Program> found =
                loadFile(normalizePath(joinPath(root, name + ".cid")));
            if (found != nullptr) {
                return found;
            }
        }
        return nullptr;
    };
    resolve::Resolver::ModuleLoader load =
        [&lib, &roots, &loadSource, &pipe, &loadDiag,
         &opts](const std::string& name) -> std::unique_ptr<Program> {
        if (opts.useInterfaces) {
            const std::string path = interfaceSearchPath(opts.interfaceDir, name);
            bool found = false;
            const std::string text = readFile(path, found);
            if (found) {
                interfacefmt::Header header;
                std::string headerError;
                if (!interfacefmt::parseHeader(text, header, headerError)) {
                    if (opts.allowStaleInterfaces) {
                        return loadSource(name);
                    }
                    loadDiag += path + ": invalid module interface: " +
                                headerError + "\n";
                    return nullptr;
                }
                if (header.format != interfacefmt::kFormatVersion) {
                    if (opts.allowStaleInterfaces) {
                        return loadSource(name);
                    }
                    loadDiag += path + ": interface format " +
                                std::to_string(header.format) +
                                " is not supported (this compiler reads " +
                                std::to_string(interfacefmt::kFormatVersion) +
                                "); rebuild the interface\n";
                    return nullptr;
                }
                if (header.compiler != interfacefmt::kCompilerTag) {
                    if (opts.allowStaleInterfaces) {
                        return loadSource(name);
                    }
                    loadDiag += path + ": interface was built by '" +
                                header.compiler + "', this compiler is '" +
                                interfacefmt::kCompilerTag +
                                "'; rebuild the interface\n";
                    return nullptr;
                }
                if (header.module != name) {
                    if (opts.allowStaleInterfaces) {
                        return loadSource(name);
                    }
                    loadDiag += path + ": interface declares module '" +
                                header.module + "', not '" + name + "'\n";
                    return nullptr;
                }
                // Record the hash even when the body forces a source
                // fallback: it is still this dependency's interface
                // identity for the caller's cache key.
                pipe.interfaceHashes[name] = interfacefmt::sha256(text);
                if (!header.requiresSource.empty()) {
                    std::unique_ptr<Program> source = loadSource(name);
                    if (source == nullptr) {
                        loadDiag += path + ": interface requires source (" +
                                    header.requiresSource +
                                    ") but module '" + name +
                                    "' has no source\n";
                    }
                    return source;
                }
                // A present source must be the one this interface was
                // rendered from. A missing source is the separate-compile
                // case and is trusted.
                const std::string sourcePath =
                    findModuleFile(name, lib, roots);
                if (!sourcePath.empty()) {
                    bool sourceFound = false;
                    const std::string sourceText =
                        readFile(sourcePath, sourceFound);
                    if (sourceFound &&
                        interfacefmt::sha256(sourceText) != header.sourceHash) {
                        if (opts.allowStaleInterfaces) {
                            return loadSource(name);
                        }
                        loadDiag += sourcePath +
                                    ": module interface '" + path +
                                    "' is stale (source changed); rebuild "
                                    "the interface\n";
                        return nullptr;
                    }
                }
                Lexer interfaceLexer(path, text);
                std::vector<Token> interfaceTokens = interfaceLexer.lex();
                for (const LexError& error : interfaceLexer.errors()) {
                    loadDiag += at(error.pos) + ": " + error.message + "\n";
                }
                Parser interfaceParser(path, std::move(interfaceTokens), true);
                std::unique_ptr<Program> program = interfaceParser.parse();
                for (const ParseError& error : interfaceParser.errors()) {
                    loadDiag += at(error.pos) + ": " + error.message + "\n";
                }
                program->isInterface = true;
                return program;
            }
            if (opts.requireInterfaces) {
                loadDiag += "missing interface for module '" + name +
                            "' at '" + path +
                            "' (required by --require-interface)\n";
                return nullptr;
            }
        }
        return loadSource(name);
    };
    Lexer lexer(file, text);
    std::vector<Token> tokens = lexer.lex();
    faultAlloc("lex");
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
    faultAlloc("parse");
    resolve::Resolver resolver(*pipe.types, std::move(load),
                               opts.targetKind, opts.mangleAll);
    if (!resolver.resolveProgram(*pipe.program, opts.module)) {
        // A loader problem (missing/stale/incompatible interface) is the
        // cause; the resolver's "cannot find module" is the symptom. Lead
        // with the cause.
        pipe.diagnostic += loadDiag;
        for (const resolve::ResolveError& error : resolver.errors()) {
            pipe.diagnostic += at(error.pos) + ": " + error.message + "\n";
        }
        return false;
    }
    if (!loadDiag.empty()) {
        pipe.diagnostic += loadDiag;
        for (const resolve::ResolveError& error : resolver.errors()) {
            pipe.diagnostic += at(error.pos) + ": " + error.message + "\n";
        }
        return false;
    }
    faultAlloc("resolve");
    check::Checker checker(*pipe.types, resolver, opts.targetKind);
    lower::Lowerer lowerer(*pipe.types, resolver, checker, opts.targetKind,
                           opts.checks);
    // Closure provenance runs before any body checks: callee bodies see
    // caller proofs regardless of module order. Pure analysis — no
    // diagnostics, no recorded types.
    checker.prescanProvenance(resolver.moduleOrder());
    // A split-module object still needs the imported modules' bodies in the
    // checker's tables: a generic specialization is walked from its template
    // body, and that body belongs to the module that declared it. Check the
    // imports with the lowering sink detached, then lower the requested
    // module alone. An interface module carries no bodies: its declaration
    // types came from the header pass, so there is nothing here to check.
    if (!opts.wholeProgram) {
        checker.setSink(nullptr);
        for (const std::string& module : resolver.moduleOrder()) {
            if (module == opts.module) {
                continue;
            }
            Program* imported = resolver.programOf(module);
            if (imported != nullptr && imported->isInterface) {
                continue;
            }
            if (imported == nullptr ||
                !checker.checkProgram(*imported, module)) {
                for (const check::CheckError& error : checker.errors()) {
                    pipe.diagnostic +=
                        at(error.pos) + ": " + error.message + "\n";
                }
                for (const resolve::ResolveError& error : resolver.errors()) {
                    pipe.diagnostic +=
                        at(error.pos) + ": " + error.message + "\n";
                }
                return false;
            }
        }
    }
    faultAlloc("check");
    // `check`, `compile`, and `run` promise a program, so each names a
    // missing or wrong source-defined entry. `ir` may dump a partial module
    // and stays lenient.
    if (opts.requireEntry && !checker.checkEntry(*pipe.program)) {
        for (const check::CheckError& error : checker.errors()) {
            pipe.diagnostic += at(error.pos) + ": " + error.message + "\n";
        }
        return false;
    }
    // Every resolved module lowers into one IR module in resolve order, so
    // link names mangle under their own module and rodata labels never
    // collide. Emission order carries no semantics: calls resolve by link
    // name, not by position. A split-module object lowers only its own
    // definitions and binds every other module's globals as unresolved
    // references, which the linker satisfies from the other objects.
    bool lowered = false;
    if (opts.wholeProgram) {
        // The first module in order starts the IR module; the rest
        // accumulate into it. Order is dependencies-first, so the first
        // module is a leaf, not necessarily the entry.
        bool first = true;
        for (const std::string& module : resolver.moduleOrder()) {
            Program* program = module == opts.module
                                   ? pipe.program.get()
                                   : resolver.programOf(module);
            if (program == nullptr) {
                pipe.diagnostic +=
                    "cannot lower missing module '" + module + "'\n";
                return false;
            }
            if (first) {
                lowered = lowerer.lowerModule(module, *program);
                first = false;
            } else {
                lowered = lowerer.lowerInto(module, *program);
            }
            if (!lowered) {
                break;
            }
        }
    } else {
        std::vector<std::string> external;
        for (const std::string& module : resolver.moduleOrder()) {
            if (module != opts.module) {
                external.push_back(module);
            }
        }
        lowerer.declareExternalGlobals(external);
        lowered = lowerer.lowerModule(opts.module, *pipe.program);
    }
    // Deferred monomorphization: cross-module specs are unknown while
    // their home module lowers, so every used specialization emits here,
    // once the resolver cache is complete.
    if (lowered) {
        lowered = lowerer.emitSpecBodies();
    }
    if (lowered) {
        pipe.module = lowerer.takeModule();
    }
    faultAlloc("lower");
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
    opt::OptResult result;
    result.ok = true;
    if (!opts.noOpt) {
        result = opt::run(*pipe.module);
    }
    if (!result.ok) {
        pipe.diagnostic += file + ": " + result.error + "\n";
        return false;
    }
    faultAlloc("opt");
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

// Deterministic compiler-OOM injection for the v2 smoke contract. The
// environment names a stage; `<stage>:len` throws the length_error shape.
// The v2 stage spellings stay valid aliases for the v3 boundaries.
void faultAlloc(const char* stage) {
    const char* spec = std::getenv("CIDER_FAULT_ALLOC");
    if (spec == nullptr) {
        return;
    }
    std::string requested(spec);
    std::string wanted(stage);
    const std::string lengthSuffix = ":len";
    const bool wantsLength =
        requested.size() >= lengthSuffix.size() &&
        requested.compare(requested.size() - lengthSuffix.size(),
                          lengthSuffix.size(), lengthSuffix) ==
            0;
    if (wantsLength) {
        requested.resize(requested.size() - lengthSuffix.size());
    }
    if (requested == "symbol") {
        requested = "resolve";
    } else if (requested == "typecheck") {
        requested = "check";
    } else if (requested == "ir_gen" || requested == "deconstruct") {
        requested = "lower";
    } else if (requested == "optimize") {
        requested = "opt";
    }
    if (requested != wanted) {
        return;
    }
    if (wantsLength) {
        throw std::length_error("fault allocation");
    }
    throw std::bad_alloc();
}

// Write through a sibling temporary and rename only after every byte landed.
// A failed compile therefore never truncates the destination or leaves a
// half-written object behind; the rename is the one visible step.
bool writeFile(const std::string& path, const std::string& text) {
    std::string temp = path + ".tmpXXXXXX";
    std::vector<char> name(temp.begin(), temp.end());
    name.push_back('\0');
    int fd = mkstemp(name.data());
    if (fd < 0) {
        return false;
    }
    uint64_t written = 0;
    while (written < static_cast<uint64_t>(text.size())) {
        ssize_t got = ::write(fd, text.data() + written,
                              text.size() - written);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            ::close(fd);
            ::unlink(name.data());
            return false;
        }
        written += static_cast<uint64_t>(got);
    }
    if (::close(fd) != 0) {
        ::unlink(name.data());
        return false;
    }
    if (::rename(name.data(), path.c_str()) != 0) {
        ::unlink(name.data());
        return false;
    }
    return true;
}

// Run a tool without a shell, capturing both streams. `ld` and `as` put
// their diagnostics on stderr; a failed link must show them verbatim rather
// than pretend the stage was unavailable or fall back to another path.
bool runTool(const std::vector<std::string>& args, std::string& output) {
    constexpr long kChunk = 4096;
    int fds[2];
    if (pipe(fds) != 0) {
        output = "cannot create a tool-output pipe\n";
        return false;
    }
    pid_t child = fork();
    if (child < 0) {
        close(fds[0]);
        close(fds[1]);
        output = "cannot start '" + args[0] + "'\n";
        return false;
    }
    if (child == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        std::vector<char*> argv;
        argv.reserve(args.size());
        for (const std::string& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        std::fprintf(stderr, "cider: cannot execute '%s': %s\n", argv[0],
                     std::strerror(errno));
        _exit(127);
    }
    close(fds[1]);
    char buffer[kChunk];
    for (;;) {
        long got = read(fds[0], buffer, kChunk);
        if (got > 0) {
            output.append(buffer, static_cast<size_t>(got));
            continue;
        }
        if (got < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    close(fds[0]);
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    if (WIFSIGNALED(status)) {
        output += "'" + args[0] + "' terminated by signal " +
                  std::to_string(WTERMSIG(status)) + "\n";
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (!output.empty() && output.back() != '\n') {
            output += '\n';
        }
        output += "'" + args[0] + "' exited with status " +
                  std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : 127) +
                  "\n";
        return false;
    }
    return true;
}

std::string tool(const char* env, const char* fallback) {
    const char* configured = std::getenv(env);
    return configured != nullptr && *configured != '\0' ? configured : fallback;
}

bool makeTempPath(const std::string& prefix, std::string& path) {
    std::string pattern = prefix + "XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    int fd = mkstemp(name.data());
    if (fd < 0) {
        return false;
    }
    close(fd);
    path = name.data();
    return true;
}

constexpr unsigned long kCidExtension = 4;

bool endsWith(const std::string& text, const std::string& suffix) {
    if (text.size() < suffix.size()) {
        return false;
    }
    return !text.compare(text.size() - suffix.size(), suffix.size(), suffix);
}

std::string baseName(const std::string& path) {
    std::string::size_type slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string moduleNameFromPath(const std::string& path) {
    std::string name = baseName(path);
    if (endsWith(name, ".cid")) {
        name.resize(name.size() - kCidExtension);
    }
    return name;
}

bool validModuleName(const std::string& name) {
    if (name.empty() || name == "." || name == "..") {
        return false;
    }
    for (char c : name) {
        const bool allowed =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '.';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

std::string defaultObjectPath(const std::string& input) {
    std::string path = input;
    if (endsWith(path, ".cid")) {
        path.resize(path.size() - kCidExtension);
    }
    return path + ".o";
}

std::string hexAddress(uint64_t value) {
    char text[32];
    std::snprintf(text, sizeof text, "0x%llx",
                  static_cast<unsigned long long>(value));
    return text;
}

bool parseAddress(const std::string& text, uint64_t& out) {
    if (text.empty()) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    unsigned long long value = std::strtoull(text.c_str(), &end, 0);
    if (errno != 0 || end == text.c_str() || *end != '\0') {
        return false;
    }
    out = static_cast<uint64_t>(value);
    return true;
}

struct ElfAudit {
    struct Symbol {
        std::string name;
        bool weak = false;
    };
    std::vector<Symbol> defined;
    std::vector<std::string> undefined;
    bool hasEntry = false;
};

constexpr unsigned long kElfHeaderBytes = 64;
constexpr unsigned long kElfMagicBytes = 4;
constexpr unsigned long kElfSectionHeaderBytes = 64;
constexpr unsigned long kElfSymbolBytes = 24;
constexpr unsigned char kElfMagic0 = 0x7f;
constexpr unsigned char kElfMagic1 = 0x45;
constexpr unsigned char kElfMagic2 = 0x4c;
constexpr unsigned char kElfMagic3 = 0x46;

bool hasElfMagic(const std::string& data) {
    return data.size() >= kElfMagicBytes &&
           static_cast<unsigned char>(data[0]) == kElfMagic0 &&
           static_cast<unsigned char>(data[1]) == kElfMagic1 &&
           static_cast<unsigned char>(data[2]) == kElfMagic2 &&
           static_cast<unsigned char>(data[3]) == kElfMagic3;
}

// Minimal ELF64 little-endian reader for the link inputs. It is not a
// replacement for `ld`: it only answers the two questions Cider must own
// before invoking it — is every input an x86-64 ET_REL object, and does the
// link set define the target entry exactly once.
bool auditObject(const std::string& path, const std::string& entry,
                 ElfAudit& audit, std::string& error) {
    bool found = false;
    std::string data = readFile(path, found);
    if (!found) {
        error = "cannot open '" + path + "'";
        return false;
    }
    auto u16 = [&](size_t at) -> uint16_t {
        return static_cast<uint16_t>(static_cast<unsigned char>(data[at])) |
               static_cast<uint16_t>(
                   static_cast<unsigned char>(data[at + 1])) << 8;
    };
    auto u32 = [&](size_t at) -> uint32_t {
        uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i) {
            value |= static_cast<uint32_t>(
                         static_cast<unsigned char>(data[at + i]))
                     << (8 * i);
        }
        return value;
    };
    auto u64 = [&](size_t at) -> uint64_t {
        uint64_t value = 0;
        for (unsigned i = 0; i < 8; ++i) {
            value |= static_cast<uint64_t>(
                         static_cast<unsigned char>(data[at + i]))
                     << (8 * i);
        }
        return value;
    };
    if (data.size() < kElfHeaderBytes || !hasElfMagic(data)) {
        error = "not an ELF file";
        return false;
    }
    if (static_cast<unsigned char>(data[4]) != 2 ||
        static_cast<unsigned char>(data[5]) != 1) {
        error = "not a little-endian ELF64 object";
        return false;
    }
    if (u16(16) != 1) {
        error = "not a relocatable object (ELF type " +
                std::to_string(u16(16)) + ")";
        return false;
    }
    if (u16(18) != 62) {
        error = "not an x86-64 object (machine " +
                std::to_string(u16(18)) + ")";
        return false;
    }
    uint64_t sectionOff = u64(40);
    uint16_t sectionEnt = u16(58);
    uint16_t sectionCount = u16(60);
    if (sectionOff == 0 || sectionCount == 0 ||
        sectionEnt < kElfSectionHeaderBytes) {
        error = "has no section table";
        return false;
    }
    if (sectionOff + static_cast<uint64_t>(sectionCount) * sectionEnt >
        data.size()) {
        error = "has a truncated section table";
        return false;
    }
    bool haveSymtab = false;
    uint64_t symOff = 0;
    uint64_t symBytes = 0;
    uint64_t symEnt = 0;
    uint32_t symLink = 0;
    for (uint16_t i = 0; i < sectionCount; ++i) {
        uint64_t base =
            sectionOff + static_cast<uint64_t>(i) * sectionEnt;
        if (u32(base + 4) != 2) {
            continue;
        }
        haveSymtab = true;
        symOff = u64(base + 24);
        symBytes = u64(base + 32);
        symLink = u32(base + 40);
        symEnt = u64(base + 56);
        break;
    }
    if (!haveSymtab) {
        error = "has no symbol table";
        return false;
    }
    if (symLink >= sectionCount) {
        error = "has a malformed symbol string table";
        return false;
    }
    uint64_t strBase =
        sectionOff + static_cast<uint64_t>(symLink) * sectionEnt;
    uint64_t strOff = u64(strBase + 24);
    uint64_t strBytes = u64(strBase + 32);
    if (strOff + strBytes > data.size() || symOff + symBytes > data.size()) {
        error = "has a truncated symbol table";
        return false;
    }
    uint64_t entryEnt = symEnt == 0 ? kElfSymbolBytes : symEnt;
    if (entryEnt < kElfSymbolBytes) {
        error = "has a malformed symbol table";
        return false;
    }
    for (uint64_t at = symOff; at + kElfSymbolBytes <= symOff + symBytes;
         at += entryEnt) {
        uint32_t nameOff = u32(at);
        unsigned char info = static_cast<unsigned char>(data[at + 4]);
        uint16_t section = u16(at + 6);
        unsigned binding = info >> 4;
        if (binding != 1 && binding != 2) {
            continue;
        }
        if (nameOff >= strBytes) {
            continue;
        }
        std::string name;
        for (uint64_t i = strOff + nameOff; i < strOff + strBytes; ++i) {
            if (data[i] == '\0') {
                break;
            }
            name += data[i];
        }
        if (name.empty()) {
            continue;
        }
        if (section == 0) {
            audit.undefined.push_back(name);
            continue;
        }
        audit.defined.push_back(ElfAudit::Symbol{name, binding == 2});
        if (name == entry) {
            audit.hasEntry = true;
        }
    }
    return true;
}

bool auditLinkInputs(const std::vector<std::string>& inputs,
                     const std::string& entry, std::string& error) {
    std::map<std::string, std::pair<std::string, bool>> definitions;
    bool entryFound = false;
    for (const std::string& input : inputs) {
        ElfAudit audit;
        std::string detail;
        if (!auditObject(input, entry, audit, detail)) {
            error = input + ": " + detail + "\n";
            return false;
        }
        entryFound = entryFound || audit.hasEntry;
        for (const ElfAudit::Symbol& symbol : audit.defined) {
            auto found = definitions.find(symbol.name);
            if (found != definitions.end() && !found->second.second &&
                !symbol.weak) {
                error = "duplicate symbol '" + symbol.name +
                        "' defined by '" + found->second.first + "' and '" +
                        input + "'\n";
                return false;
            }
            if (found == definitions.end() || (!symbol.weak &&
                                               found->second.second)) {
                definitions[symbol.name] = {input, symbol.weak};
            }
        }
    }
    if (!entryFound) {
        error = "no defined entry symbol '" + entry + "' in the link inputs\n";
        return false;
    }
    return true;
}

std::string pomeosLinkScript(uint64_t base) {
    std::ostringstream text;
    text << "OUTPUT_FORMAT(elf64-x86-64)\n"
         << "ENTRY(_start)\n\n"
         << "SECTIONS\n"
         << "{\n"
         << "    . = " << hexAddress(base) << ";\n"
         << "    .text : { *(.text) *(.text.*) }\n"
         << "    .rodata : { *(.rodata) *(.rodata.*) }\n"
         << "    .data : { *(.data) *(.data.*) }\n"
         << "    .bss : { *(.bss) *(.bss.*) *(COMMON) }\n"
         << "}\n";
    return text.str();
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

int linkInputs(target::Kind targetKind, const std::vector<std::string>& inputs,
               const std::string& output, const std::string& script,
               bool hasBase, uint64_t base, std::string& err) {
    const target::Info& info = target::info(targetKind);
    if (targetKind == target::Kind::Vm) {
        err = "target vm is interpreted: it has no object link step\n";
        return 2;
    }
    if (info.object != target::Object::Elf64) {
        err = "target " + std::string(info.name) +
              " links PE32+ images, not ELF64 objects: use its external "
              "link path\n";
        return 2;
    }
    if (!hasBase) {
        base = 0x2000000;
    }
    if (!auditLinkInputs(inputs, info.entry, err)) {
        return 1;
    }
    std::string temporaryScript;
    std::string scriptPath = script;
    if (targetKind == target::Kind::Pomeos && scriptPath.empty()) {
        if (!makeTempPath("/tmp/cider_link_", temporaryScript)) {
            err = "cannot create a temporary linker script\n";
            return 1;
        }
        if (!writeFile(temporaryScript, pomeosLinkScript(base))) {
            unlink(temporaryScript.c_str());
            err = "cannot write a temporary linker script\n";
            return 1;
        }
        scriptPath = temporaryScript;
    } else if (!scriptPath.empty() && access(scriptPath.c_str(), R_OK) != 0) {
        err = "cannot read linker script '" + scriptPath + "'\n";
        return 1;
    }

    std::vector<std::string> command{tool("LD", "ld"), "-nostdlib",
                                     "-e", info.entry};
    if (targetKind == target::Kind::Pomeos) {
        command.push_back("-static");
        command.push_back("--defsym=BASE_ADDR=" + hexAddress(base));
    }
    if (!scriptPath.empty()) {
        command.push_back("-T");
        command.push_back(scriptPath);
    }
    command.push_back("-o");
    command.push_back(output);
    for (const std::string& input : inputs) {
        command.push_back(input);
    }
    std::string toolOutput;
    bool ok = runTool(command, toolOutput);
    if (!temporaryScript.empty()) {
        unlink(temporaryScript.c_str());
    }
    if (!ok) {
        err = "linking '" + output + "' failed:\n" + toolOutput;
        return 1;
    }
    return 0;
}

}  // namespace

// --- Module build ------------------------------------------------------------
//
// `build` is the orchestration layer around the existing object emitter and
// linker. It discovers the import graph from source, compiles each module in
// dependency order against the interfaces of the modules before it, and keys
// every object and interface by the module's source hash, its dependency keys,
// the target and the two interface version numbers. An unchanged module is
// never re-parsed or re-lowered: its cached object and interface are copied
// forward.
namespace {

struct BuildModule {
    std::string name;
    std::string path;
    std::string text;
    std::string sourceHash;
    std::string cacheKey;
    std::string interfaceText;
    std::string interfaceHash;
    std::string objectPath;
    std::vector<std::string> deps;
    bool isEntry = false;
};

bool ensureDirectory(const std::string& path, std::string& error) {
    std::error_code failure;
    std::filesystem::create_directories(path, failure);
    if (failure) {
        error = "cannot create directory '" + path + "': " +
                failure.message() + "\n";
        return false;
    }
    return true;
}

bool ensureParentDirectory(const std::string& path, std::string& error) {
    std::string::size_type slash = path.rfind('/');
    if (slash == std::string::npos || slash == 0) {
        return true;
    }
    return ensureDirectory(path.substr(0, slash), error);
}

bool copyArtifact(const std::string& from, const std::string& to,
                  std::string& error) {
    if (from == to) {
        return true;
    }
    if (!ensureParentDirectory(to, error)) {
        return false;
    }
    std::error_code failure;
    std::filesystem::copy_file(from, to,
                               std::filesystem::copy_options::overwrite_existing,
                               failure);
    if (failure) {
        error = "cannot copy '" + from + "' to '" + to + "': " +
                failure.message() + "\n";
        return false;
    }
    return true;
}

bool assembleObject(const std::string& assemblyText,
                    const std::string& objectPath, std::string& error) {
    std::string assemblyPath;
    if (!makeTempPath("/tmp/cider_object_", assemblyPath)) {
        error = "cannot create a temporary assembly file\n";
        return false;
    }
    if (!writeFile(assemblyPath, assemblyText)) {
        unlink(assemblyPath.c_str());
        error = "cannot write a temporary assembly file\n";
        return false;
    }
    std::string toolOutput;
    const bool assembled = runTool(
        {tool("AS", "as"), "-o", objectPath, assemblyPath}, toolOutput);
    unlink(assemblyPath.c_str());
    if (!assembled) {
        error = "assembling '" + objectPath + "' failed:\n" + toolOutput;
        return false;
    }
    return true;
}

std::string codegenDiagnostic(const codegen::EmitResult& emitted) {
    if (emitted.pos.file.empty()) {
        return emitted.error + "\n";
    }
    return at(emitted.pos) + ": " + emitted.error + "\n";
}

bool scanSource(const std::string& path, const std::string& text,
                std::vector<std::string>& deps, std::string& error) {
    Lexer lexer(path, text);
    std::vector<Token> tokens = lexer.lex();
    std::string diagnostic;
    for (const LexError& lexError : lexer.errors()) {
        diagnostic += at(lexError.pos) + ": " + lexError.message + "\n";
    }
    Parser parser(path, std::move(tokens));
    std::unique_ptr<Program> program = parser.parse();
    for (const ParseError& parseError : parser.errors()) {
        diagnostic += at(parseError.pos) + ": " + parseError.message + "\n";
    }
    if (!diagnostic.empty() || program == nullptr) {
        error = diagnostic.empty() ? "cannot parse '" + path + "'\n"
                                   : diagnostic;
        return false;
    }
    collectImportNames(*program, deps);
    return true;
}

bool discoverBuildGraph(const std::string& entryPath,
                        const std::string& entryDir,
                        const std::string& lib,
                        const std::vector<std::string>& roots,
                        std::map<std::string, BuildModule>& modules,
                        std::vector<std::string>& order, std::string& error) {
    std::set<std::string> visiting;
    std::function<bool(const std::string&, const std::string&, bool)> visit =
        [&](const std::string& name, const std::string& path,
            bool isEntry) -> bool {
        if (visiting.count(name) != 0) {
            error = "import cycle through module '" + name + "'\n";
            return false;
        }
        auto known = modules.find(name);
        if (known != modules.end()) {
            return true;
        }
        visiting.insert(name);
        bool found = false;
        std::string text = readFile(path, found);
        if (!found) {
            error = "cannot open module '" + name + "' at '" + path + "'\n";
            return false;
        }
        BuildModule module;
        module.name = name;
        module.path = path;
        module.text = text;
        module.isEntry = isEntry;
        module.sourceHash = interfacefmt::sha256(text);
        if (!scanSource(path, text, module.deps, error)) {
            return false;
        }
        for (const std::string& dep : module.deps) {
            std::string depPath = findModuleFile(dep, lib, roots);
            if (depPath.empty()) {
                error = "cannot find module '" + dep + "' imported by '" +
                        name + "'\n";
                return false;
            }
            if (!visit(dep, depPath, false)) {
                return false;
            }
        }
        visiting.erase(name);
        modules.emplace(name, std::move(module));
        order.push_back(name);
        return true;
    };
    if (!visit("main", entryPath, true)) {
        return false;
    }
    return true;
}

std::string cacheKeyFor(const BuildModule& module, target::Kind targetKind,
                        bool noOpt, bool checks,
                        const std::map<std::string, BuildModule>& modules) {
    std::ostringstream manifest;
    manifest << "cider-build-cache: 1\n";
    manifest << "interface-format: " << interfacefmt::kFormatVersion << "\n";
    manifest << "compiler: " << interfacefmt::kCompilerTag << "\n";
    manifest << "target: " << target::info(targetKind).name << "\n";
    manifest << "module: " << module.name << "\n";
    manifest << "entry: " << (module.isEntry ? 1 : 0) << "\n";
    manifest << "source: " << module.sourceHash << "\n";
    manifest << "no-opt: " << (noOpt ? 1 : 0) << "\n";
    manifest << "checks: " << (checks ? 1 : 0) << "\n";
    for (const std::string& dep : module.deps) {
        manifest << "dep: " << dep;
        auto found = modules.find(dep);
        if (found != modules.end()) {
            manifest << " " << found->second.cacheKey << " "
                     << found->second.interfaceHash;
        }
        manifest << "\n";
    }
    return interfacefmt::sha256(manifest.str());
}

bool cacheHit(const std::string& entry, const BuildModule& module,
              std::string& error) {
    std::string metaPath = entry + "/meta";
    std::string objectPath = entry + "/module.o";
    std::string interfacePath = entry + "/module.cidm";
    bool found = false;
    std::string meta = readFile(metaPath, found);
    if (!found || !std::filesystem::exists(objectPath) ||
        !std::filesystem::exists(interfacePath)) {
        return false;
    }
    std::string wantModule = "module: " + module.name + "\n";
    std::string wantSource = "source: " + module.sourceHash + "\n";
    if (meta.find(wantModule) == std::string::npos ||
        meta.find(wantSource) == std::string::npos) {
        return false;
    }
    (void)error;
    return true;
}

bool buildOneModule(BuildModule& module,
                    const std::map<std::string, BuildModule>& modules,
                    const std::string& cacheRoot,
                    const std::string& interfaceDir, target::Kind targetKind,
                    bool noOpt, bool checks, bool verbose, std::string& out,
                    std::string& error) {
    module.cacheKey = cacheKeyFor(module, targetKind, noOpt, checks, modules);
    const std::string entries = joinPath(cacheRoot, "v1");
    const std::string entry = joinPath(entries, module.cacheKey);
    if (!ensureDirectory(entries, error) ||
        !ensureDirectory(interfaceDir, error)) {
        return false;
    }
    bool hit = cacheHit(entry, module, error);
    if (hit) {
        bool found = false;
        module.interfaceText = readFile(entry + "/module.cidm", found);
        if (!found) {
            error = "cache entry '" + entry + "' lost its interface\n";
            return false;
        }
        module.interfaceHash = interfacefmt::sha256(module.interfaceText);
        module.objectPath = entry + "/module.o";
    } else {
        if (!ensureDirectory(entry, error)) {
            return false;
        }
        FrontOptions opts;
        opts.targetKind = targetKind;
        opts.noOpt = noOpt;
        opts.checks = checks;
        opts.requireEntry = module.isEntry;
        opts.wholeProgram = false;
        opts.mangleAll = !module.isEntry;
        opts.module = module.name;
        opts.useInterfaces = true;
        opts.interfaceDir = interfaceDir;
        Pipeline pipe;
        if (!runFront(module.path, module.text, pipe, opts)) {
            error = pipe.diagnostic;
            return false;
        }
        opt::PhiLowerResult lowered = opt::lowerPhis(*pipe.module);
        if (!lowered.ok) {
            error = lowered.error;
            return false;
        }
        codegen::EmitResult emitted =
            codegen::emit(*pipe.module, targetKind, module.isEntry);
        if (!emitted.ok) {
            error = codegenDiagnostic(emitted);
            return false;
        }
        const std::string objectPath = entry + "/module.o";
        if (!assembleObject(emitted.text, objectPath, error)) {
            return false;
        }
        std::vector<interfacefmt::Dependency> deps;
        for (const std::string& dep : module.deps) {
            interfacefmt::Dependency dependency;
            dependency.module = dep;
            auto found = modules.find(dep);
            if (found != modules.end()) {
                dependency.hash = found->second.interfaceHash;
            }
            deps.push_back(std::move(dependency));
        }
        interfacefmt::RenderResult rendered = interfacefmt::render(
            module.name, *pipe.program, module.sourceHash, deps);
        module.interfaceText = rendered.text;
        module.interfaceHash = interfacefmt::sha256(module.interfaceText);
        if (!writeFile(entry + "/module.cidm", module.interfaceText) ||
            !writeFile(entry + "/meta",
                       "module: " + module.name + "\n" +
                           "source: " + module.sourceHash + "\n" +
                           "key: " + module.cacheKey + "\n")) {
            error = "cannot write cache entry '" + entry + "'\n";
            return false;
        }
        module.objectPath = objectPath;
    }
    const std::string activePath =
        joinPath(interfaceDir, interfacefmt::modulePath(module.name) + ".cidm");
    if (!ensureParentDirectory(activePath, error) ||
        !writeFile(activePath, module.interfaceText)) {
        error = "cannot write interface '" + activePath + "'\n";
        return false;
    }
    if (verbose) {
        out += "cider: build: module " + module.name + ": " +
               (hit ? "cached " : "compiled ") + module.cacheKey + "\n";
    }
    return true;
}

int buildProgram(const std::string& input, const std::string& output,
                 target::Kind targetKind, const std::string& cacheDir,
                 const std::string& interfaceDirFlag,
                 const std::string& objectsDir, bool noLink, bool verbose,
                 bool noOpt, bool checks, std::string& out,
                 std::string& err) {
    if (targetKind == target::Kind::Vm) {
        err = "target vm is not built: an interpreter, not an object emitter\n";
        return 2;
    }
    if (target::info(targetKind).object != target::Object::Elf64) {
        err = "target " + std::string(target::info(targetKind).name) +
              " does not build ELF64 objects: use its external link path\n";
        return 2;
    }
    bool found = false;
    readFile(input, found);
    if (!found) {
        err = "cannot open '" + input + "'\n";
        return 1;
    }
    std::string entryDir;
    std::string::size_type slash = input.rfind('/');
    if (slash != std::string::npos) {
        entryDir = input.substr(0, slash);
    }
    const char* libRoot = std::getenv("CIDER_LIB");
    const std::string lib = libRoot != nullptr ? libRoot : "";
    const std::vector<std::string> roots = moduleRoots(entryDir);
    const std::string cacheRoot =
        cacheDir.empty() ? ".cider-cache" : cacheDir;
    const std::string activeDir = interfaceDirFlag.empty()
                                      ? joinPath(cacheRoot, "interfaces")
                                      : interfaceDirFlag;
    std::map<std::string, BuildModule> modules;
    std::vector<std::string> order;
    if (!discoverBuildGraph(input, entryDir, lib, roots, modules, order, err)) {
        return 1;
    }
    for (const std::string& name : order) {
        if (!buildOneModule(modules[name], modules, cacheRoot, activeDir,
                            targetKind, noOpt, checks, verbose, out, err)) {
            return 1;
        }
        BuildModule& module = modules[name];
        if (!objectsDir.empty()) {
            const std::string destination =
                joinPath(objectsDir, interfacefmt::modulePath(name) + ".o");
            if (!copyArtifact(module.objectPath, destination, err)) {
                return 1;
            }
            module.objectPath = destination;
        }
    }
    if (noLink) {
        return 0;
    }
    std::vector<std::string> objects;
    for (const std::string& name : order) {
        objects.push_back(modules[name].objectPath);
    }
    return linkInputs(targetKind, objects, output, "", false, 0, err);
}

}  // namespace

int runDriver(const std::vector<std::string>& args, std::string& out, std::string& err) {
    if (args.empty()) {
        err = "usage: cider <check|compile|ir|run|link|build|bundle> [options] <file>\n"
              "  compile -c <file> -o <file.o>  emit an ELF64 relocatable object\n"
              "  compile -c --no-entry <file>  emit a non-entry module object\n"
              "  compile -c --interface-dir <dir>  resolve imports from .cidm\n"
              "  build [--cache-dir <dir>] [-o out] <entry.cid>\n"
              "  link [--target native|pomeos] [-o out] "
              "[--script <ld script>] [--base-address <addr>] <objects...>\n";
        return 2;
    }
    std::string command = args[0];
    if (command != "check" && command != "compile" && command != "ir" &&
        command != "run" && command != "link" && command != "build" &&
        command != "bundle") {
        err = "unknown subcommand '" + command + "'\n";
        return 2;
    }
    if (command == "bundle") {
        err = "bundle is not implemented yet\n";
        return 2;
    }

    target::Kind targetKind = target::Kind::Native;
    std::string output;
    std::string input;
    std::string script;
    std::string moduleFlag;
    std::vector<std::string> linkObjects;
    uint64_t baseAddress = 0;
    bool noOpt = false;
    bool checks = false;
    bool objectMode = false;
    bool noEntry = false;
    bool entryFlag = false;
    bool hasBase = false;
    bool noInterface = false;
    bool requireInterfaces = false;
    bool allowStaleInterfaces = false;
    bool noLink = false;
    bool verbose = false;
    std::string interfaceDir;
    std::string cacheDir;
    std::string objectsDir;
    for (uint64_t i = 1; i < static_cast<uint64_t>(args.size()); ++i) {
        const std::string& arg = args[i];
        if (arg == "--target" && i + 1 < static_cast<uint64_t>(args.size())) {
            const std::string name = args[++i];
            if (!target::parse(name, targetKind)) {
                err = "unknown target '" + name + "'\n";
                return 2;
            }
        } else if (arg == "--no-opt" && command != "link") {
            noOpt = true;
        } else if (arg == "--checks" && command != "link") {
            checks = true;
        } else if (arg == "-o" && i + 1 < static_cast<uint64_t>(args.size())) {
            output = args[++i];
        } else if (arg == "-c") {
            if (command != "compile") {
                err = "flag '-c' is only valid for compile\n";
                return 2;
            }
            objectMode = true;
        } else if (arg == "--module" &&
                   i + 1 < static_cast<uint64_t>(args.size())) {
            if (command != "compile") {
                err = "flag '--module' is only valid for compile\n";
                return 2;
            }
            moduleFlag = args[++i];
        } else if (arg == "--no-entry") {
            if (command != "compile") {
                err = "flag '--no-entry' is only valid for compile\n";
                return 2;
            }
            noEntry = true;
        } else if (arg == "--entry") {
            if (command != "compile") {
                err = "flag '--entry' is only valid for compile\n";
                return 2;
            }
            entryFlag = true;
        } else if (arg == "--interface-dir" &&
                   i + 1 < static_cast<uint64_t>(args.size())) {
            if (command != "compile" && command != "build") {
                err = "flag '--interface-dir' is only valid for compile or build\n";
                return 2;
            }
            interfaceDir = args[++i];
        } else if (arg == "--require-interface") {
            if (command != "compile") {
                err = "flag '--require-interface' is only valid for compile\n";
                return 2;
            }
            requireInterfaces = true;
        } else if (arg == "--allow-stale-interface") {
            if (command != "compile") {
                err = "flag '--allow-stale-interface' is only valid for compile\n";
                return 2;
            }
            allowStaleInterfaces = true;
        } else if (arg == "--no-interface") {
            if (command != "compile") {
                err = "flag '--no-interface' is only valid for compile\n";
                return 2;
            }
            noInterface = true;
        } else if (arg == "--cache-dir" &&
                   i + 1 < static_cast<uint64_t>(args.size())) {
            if (command != "build") {
                err = "flag '--cache-dir' is only valid for build\n";
                return 2;
            }
            cacheDir = args[++i];
        } else if (arg == "--objects-dir" &&
                   i + 1 < static_cast<uint64_t>(args.size())) {
            if (command != "build") {
                err = "flag '--objects-dir' is only valid for build\n";
                return 2;
            }
            objectsDir = args[++i];
        } else if (arg == "--no-link") {
            if (command != "build") {
                err = "flag '--no-link' is only valid for build\n";
                return 2;
            }
            noLink = true;
        } else if (arg == "--verbose" || arg == "-v") {
            if (command != "build") {
                err = "flag '--verbose' is only valid for build\n";
                return 2;
            }
            verbose = true;
        } else if ((arg == "--script" || arg == "-T") &&
                   command == "link" &&
                   i + 1 < static_cast<uint64_t>(args.size())) {
            script = args[++i];
        } else if (arg == "--base-address" && command == "link" &&
                   i + 1 < static_cast<uint64_t>(args.size())) {
            if (!parseAddress(args[++i], baseAddress)) {
                err = "invalid base address '" + args[i] + "'\n";
                return 2;
            }
            hasBase = true;
        } else if (!arg.empty() && arg[0] == '-') {
            err = "unknown flag '" + arg + "'\n";
            return 2;
        } else if (command == "link") {
            linkObjects.push_back(arg);
        } else if (input.empty()) {
            input = arg;
        } else {
            err = "too many files\n";
            return 2;
        }
    }

    if (command == "link") {
        if (linkObjects.empty()) {
            err = "no object files\n";
            return 2;
        }
        if (output.empty()) {
            output = "a.out";
        }
        if (targetKind != target::Kind::Pomeos && hasBase) {
            err = "flag '--base-address' is only valid for target pomeos\n";
            return 2;
        }
        return linkInputs(targetKind, linkObjects, output, script, hasBase,
                          baseAddress, err);
    }

    if (command == "build") {
        if (input.empty()) {
            err = "no input file\n";
            return 2;
        }
        if (output.empty()) {
            output = "a.out";
        }
        return buildProgram(input, output, targetKind, cacheDir, interfaceDir,
                            objectsDir, noLink, verbose, noOpt, checks, out,
                            err);
    }

    if (noEntry && entryFlag) {
        err = "flags '--no-entry' and '--entry' are mutually exclusive\n";
        return 2;
    }
    if (requireInterfaces && interfaceDir.empty()) {
        err = "flag '--require-interface' needs '--interface-dir'\n";
        return 2;
    }
    if (allowStaleInterfaces && interfaceDir.empty()) {
        err = "flag '--allow-stale-interface' needs '--interface-dir'\n";
        return 2;
    }
    if (objectMode && targetKind == target::Kind::Vm) {
        err = "target vm is not emitted: an interpreter, not an emitter\n";
        return 2;
    }
    if (objectMode &&
        target::info(targetKind).object != target::Object::Elf64) {
        err = "target " + std::string(target::info(targetKind).name) +
              " does not emit an ELF64 relocatable object: use its assembly "
              "and external link path\n";
        return 2;
    }
    if (input.empty()) {
        err = "no input file\n";
        return 2;
    }

    // A split-module compile names exactly one module. `--module main` is the
    // conventional entry object; other module names mangle every definition
    // and may omit the process entry. `--no-entry` derives the module name
    // from the file and always emits a library fragment.
    const bool split = !moduleFlag.empty() || noEntry || entryFlag;
    bool entryMode = command != "ir";
    std::string module = "main";
    bool mangleAll = false;
    if (split) {
        entryMode = entryFlag || (!noEntry && moduleFlag == "main");
        if (noEntry) {
            entryMode = false;
        }
        module = moduleFlag;
        if (module.empty()) {
            module = entryMode ? "main" : moduleNameFromPath(input);
        }
        if (!validModuleName(module)) {
            err = "invalid module name '" + module + "'\n";
            return 2;
        }
        mangleAll = !entryMode;
        entryMode = entryMode && command != "ir";
    }
    const bool requireEntry = command == "ir" ? false : entryMode;

    bool found = false;
    std::string text = readFile(input, found);
    if (!found) {
        err = "cannot open '" + input + "'\n";
        return 1;
    }
    FrontOptions opts;
    opts.targetKind = targetKind;
    opts.noOpt = noOpt;
    opts.checks = checks;
    opts.requireEntry = requireEntry;
    opts.wholeProgram = !split;
    opts.mangleAll = mangleAll;
    opts.module = module;
    opts.useInterfaces = !interfaceDir.empty();
    opts.requireInterfaces = requireInterfaces;
    opts.allowStaleInterfaces = allowStaleInterfaces;
    opts.interfaceDir = interfaceDir;
    Pipeline pipe;
    if (!runFront(input, text, pipe, opts)) {
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
    if (targetKind == target::Kind::Vm) {
        // An interpreter is not an emitter: there is no assembly to write,
        // so compile names the target and refuses. Run interprets instead.
        if (command == "compile") {
            err = "target vm is not emitted: an interpreter, not an emitter\n";
            return 2;
        }
        vm::VmResult interpreted = vm::run(*pipe.module, readPendingInput());
        if (!interpreted.ok) {
            err = at(interpreted.pos) + ": " + interpreted.error + "\n";
            return 1;
        }
        out = interpreted.stdoutText;
        err = interpreted.stderrText;
        if (interpreted.signal != 0) {
            // The program died, the same death the compiled leg takes, and
            // the harness reads a death by the signal's number: the bytes
            // already written go out before the signal ends the process.
            std::fflush(stdout);
            std::fflush(stderr);
            raise(interpreted.signal);
        }
        return interpreted.exitCode;
    }
    opt::PhiLowerResult lowered = opt::lowerPhis(*pipe.module);
    if (!lowered.ok) {
        err = lowered.error + "\n";
        return 1;
    }
    codegen::EmitResult emitted =
        codegen::emit(*pipe.module, targetKind, requireEntry);
    if (!emitted.ok) {
        if (emitted.pos.file.empty()) {
            err = emitted.error + "\n";
        } else {
            err = at(emitted.pos) + ": " + emitted.error + "\n";
        }
        return 1;
    }
    faultAlloc("codegen");
    if (targetKind == target::Kind::Uefi && command == "run") {
        err = "target uefi produces a PE32+ image: compile with -o <file.asm> "
              "and link it with scripts/link_uefi.sh\n";
        return 2;
    }
    if (targetKind == target::Kind::Pomeos && command == "run") {
        err = "target pomeos produces a PomeOS image, not a host executable: "
              "compile with -o <file.asm>, assemble it, link it with the "
              "PomeOS programs/link.ld script, and pack the ELF with "
              "pxe_packer.py\n";
        return 2;
    }
    if (command == "compile") {
        if (!objectMode) {
            if (output.empty()) {
                out = emitted.text;
                return 0;
            }
            if (!writeFile(output, emitted.text)) {
                err = "compiler: cannot write '" + output + "'\n";
                return 2;
            }
            return 0;
        }
        // Object output: the emitter still writes GNU as text, and the
        // assembler turns it into the target's ELF64 ET_REL object. No custom
        // object format and no hidden linker input.
        const std::string objectPath =
            output.empty() ? defaultObjectPath(input) : output;
        std::string assemblyPath;
        if (!makeTempPath("/tmp/cider_object_", assemblyPath)) {
            err = "cannot create a temporary assembly file\n";
            return 1;
        }
        if (!writeFile(assemblyPath, emitted.text)) {
            unlink(assemblyPath.c_str());
            err = "cannot write a temporary assembly file\n";
            return 1;
        }
        std::string toolOutput;
        const bool assembled = runTool(
            {tool("AS", "as"), "-o", objectPath, assemblyPath}, toolOutput);
        unlink(assemblyPath.c_str());
        if (!assembled) {
            err = "assembling '" + objectPath + "' failed:\n" + toolOutput;
            return 1;
        }
        if (!noInterface) {
            std::string interfaceError;
            if (!writeModuleInterface(pipe, module, text, objectPath,
                                      interfaceError)) {
                unlink(objectPath.c_str());
                err = interfaceError;
                return 1;
            }
        }
        return 0;
    }
    // run: assemble and link to temporaries, execute, answer the exit code.
    // Sovereign link: as plus ld with no libc and no runtime archive —
    // every symbol the text references, the text itself defines.
    std::string assemblyPath;
    std::string objectPath;
    std::string binaryPath;
    if (!makeTempPath("/tmp/cider_run_", assemblyPath) ||
        !makeTempPath("/tmp/cider_run_obj_", objectPath) ||
        !makeTempPath("/tmp/cider_run_bin_", binaryPath)) {
        unlink(assemblyPath.c_str());
        unlink(objectPath.c_str());
        unlink(binaryPath.c_str());
        err = "cannot create a temporary file\n";
        return 1;
    }
    if (!writeFile(assemblyPath, emitted.text)) {
        unlink(assemblyPath.c_str());
        unlink(objectPath.c_str());
        unlink(binaryPath.c_str());
        err = "cannot write a temporary assembly file\n";
        return 1;
    }
    std::string toolOutput;
    bool built = runTool({tool("AS", "as"), "-o", objectPath, assemblyPath},
                         toolOutput);
    if (built) {
        built = runTool({tool("LD", "ld"), "-nostdlib", "-e",
                         target::info(targetKind).entry, "-o", binaryPath,
                         objectPath},
                        toolOutput);
    }
    unlink(assemblyPath.c_str());
    unlink(objectPath.c_str());
    if (!built) {
        unlink(binaryPath.c_str());
        err = "assembling/linking failed:\n" + toolOutput;
        return 1;
    }
    int code = runBinary(binaryPath);
    unlink(binaryPath.c_str());
    return code;
}

}  // namespace driver
}  // namespace cider
