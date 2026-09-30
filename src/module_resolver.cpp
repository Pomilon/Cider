#include "module_resolver.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <fstream>
#include <iostream>
#include <libgen.h>
#include <vector>

namespace cider {

static bool fileExists(const std::string& name) {
    struct stat buffer;
    return (stat(name.c_str(), &buffer) == 0);
}

static std::string dirName(const std::string& path) {
    if (path.empty()) return ".";
    std::string copy = path;
    char* result = dirname(&copy[0]);
    return result ? std::string(result) : ".";
}

// CIDER-BUG-222: join a base directory and a relative path with exactly one
// separator, so a base spelled with or without a trailing slash names the
// same file.
static std::string joinPath(const std::string& base, const std::string& rel) {
    if (base.empty()) return rel;
    if (base.back() == '/') return base + rel;
    return base + "/" + rel;
}

// CIDER-BUG-222: LEXICAL path normalization. Collapses repeated separators,
// "." segments and "<segment>/.." pairs. Purely textual: it never looks at
// the filesystem (no realpath, no symlink resolution), so one tree always
// normalizes to one string.
//
// This is what makes resolution reproducible. The resolved path is handed
// back to the collector and from there reaches compiler OUTPUT -- the
// `cider bundle` per-module banner and the `-g` DWARF file table -- so a
// candidate spelled `build/../modules/std/io.cid` and one spelled
// `modules/std/io.cid` would otherwise print differently depending purely
// on WHICH search root found them. A path that cannot be lexically collapsed
// (`a/b/../c` where `b` is a symlink) keeps its meaning: the kernel still
// resolves the `..` through the link.
static std::string normalizePath(const std::string& path) {
    const bool absolute = !path.empty() && path[0] == '/';
    std::vector<std::string> parts;
    std::string segment;
    auto flush = [&]() {
        if (segment.empty()) return;
        if (segment == ".") {
            segment.clear();
            return;
        }
        if (segment == "..") {
            if (!parts.empty() && parts.back() != "..") {
                parts.pop_back();
            } else if (!absolute) {
                parts.push_back("..");  // a relative path may start above "."
            }
            // An absolute path cannot rise above "/", so drop leading "..".
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

    std::string result;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (absolute || i > 0) result += '/';
        result += parts[i];
    }
    if (result.empty()) return absolute ? "/" : ".";
    return result;
}

// Directory of the running cider binary (via /proc/self/exe), or "" when it
// cannot be determined. Only the LAST-RESORT search root uses it -- see
// searchRoots() and the resolution rule documented in README.md
// ("Modules"): the stdlib must not be found BECAUSE of where the binary
// lives, only as a fallback for an installed/bundled layout.
static std::string binaryDir() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return "";
    buf[n] = '\0';
    std::string exe(buf);
    size_t slash = exe.find_last_of('/');
    return (slash == std::string::npos) ? "" : exe.substr(0, slash + 1);
}

// CIDER-BUG-222: the STRICT ancestors of `start`, nearest first -- "..",
// "../..", ... for a RELATIVE start, "/usr/local", "/usr", "/" for an
// absolute one. Relative starts yield relative results, so a walk anchored at
// the working directory stays spelled relative to the working directory (and
// an absolute one, anchored at the binary, stays absolute).
// `dirname` is useless for the relative case -- dirname("..") is ".." -- so
// the prefixes are built directly. The depth cap is far above any real
// tree and only bounds a pathological loop.
static const int kMaxWalkDepth = 32;

static std::vector<std::string> dirsAbove(const std::string& start) {
    std::vector<std::string> dirs;
    if (start.empty() || start[0] != '/') {
        std::string prefix = "..";
        for (int depth = 0; depth < kMaxWalkDepth; ++depth) {
            dirs.push_back(prefix);
            prefix += "/..";
        }
        return dirs;
    }
    std::string current = normalizePath(start);
    for (int depth = 0; depth < kMaxWalkDepth; ++depth) {
        dirs.push_back(current);
        std::string parent = dirName(current);
        if (parent == current) break;  // reached "/": dirname("/") == "/"
        current = parent;
    }
    return dirs;
}

ModuleResolver::ModuleResolver() {
    addSearchPath(".");
}

void ModuleResolver::addSearchPath(const std::string& path) {
    std::string cleanPath = path;
    if (!cleanPath.empty() && cleanPath.back() != '/') {
        cleanPath += "/";
    }
    searchPaths_.push_back(cleanPath);
}

// CIDER-BUG-222: the resolution rule, in order. The first hit wins.
//
//   1. the importing file's own directory, then its `modules/` subdirectory
//      (design §5, file-relative imports);
//   2. every explicitly added search path -- the CLI adds none, and the "."
//      the constructor adds comes first, so a relative override still wins;
//   3. the working directory's `modules/`, then `Cider/modules/`;
//   4. `<ancestor>/modules/` for every ancestor of the WORKING DIRECTORY.
//      This repo-relative rule is what makes the stdlib reachable from a
//      build directory, from any source subdirectory, and from a checkout
//      whose directory name and depth are arbitrary;
//   5. LAST RESORT ONLY: the binary's own directory and the same ancestor
//      walk from there. This is the installed / bundled layout, where the
//      stdlib ships next to `cider`; the previous two-level `../` and
//      `../../` probe was CIDER-BUG-222, because a build directory's depth
//      is a property of the BUILD, not of the source: the same program
//      resolved the stdlib to a different path -- or to nothing at all --
//      depending on where the binary happened to sit.
//
// Every root is normalized lexically before it is stat'ed and before a hit
// is returned, so the path a module resolves to does not carry the search
// root that found it. Same source + same working directory => same resolved
// string => same bytes, for every binary wherever it was built.
static std::vector<std::string> searchRoots(const std::string& originPath,
                                            const std::vector<std::string>& searchPaths) {
    std::vector<std::string> roots;
    auto addModulesUnder = [&roots](const std::string& dir) {
        roots.push_back(joinPath(dir, "modules"));
    };

    if (!originPath.empty()) {
        const std::string originDir = dirName(originPath);
        roots.push_back(originDir);
        addModulesUnder(originDir);
    }
    for (const auto& p : searchPaths) {
        roots.push_back(p);
    }
    addModulesUnder(".");
    roots.push_back("./Cider/modules");

    for (const auto& dir : dirsAbove(".")) {
        addModulesUnder(dir);
    }

    static const std::string binDir = binaryDir();
    if (!binDir.empty()) {
        addModulesUnder(binDir);
        for (const auto& dir : dirsAbove(binDir)) {
            addModulesUnder(dir);
        }
    }
    return roots;
}

ResolutionResult ModuleResolver::resolve(const std::string& logicalPath, const std::string& originPath) {
    std::string pathSegment = logicalPath;
    size_t pos = 0;
    while ((pos = pathSegment.find('.', pos)) != std::string::npos) {
        pathSegment.replace(pos, 1, "/");
        pos++;
    }

    std::string moduleName = logicalPath;
    size_t lastDot = logicalPath.find_last_of('.');
    if (lastDot != std::string::npos) {
        moduleName = logicalPath.substr(lastDot + 1);
    }

    for (const auto& basePath : searchRoots(originPath, searchPaths_)) {
        const std::string root = normalizePath(basePath);
        const std::string fileCandidate = joinPath(root, pathSegment + ".cid");
        if (fileExists(fileCandidate)) {
            return {ModuleType::CIDER_SOURCE_FILE, fileCandidate, moduleName};
        }

        const std::string initCandidate = joinPath(root, pathSegment + "/__init__.cid");
        if (fileExists(initCandidate)) {
            return {ModuleType::CIDER_PACKAGE_DIR, joinPath(root, pathSegment), moduleName};
        }
    }

    return {ModuleType::NOT_FOUND, "", ""};
}

}  // namespace cider
