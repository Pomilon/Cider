#include "module_resolver.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <fstream>
#include <iostream>
#include <libgen.h>

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

// Directory of the running cider binary (via /proc/self/exe), so the
// stdlib can be found even when the working directory is not the project
// root (e.g. ctest running from build/).
static std::string binaryDir() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return "";
    buf[n] = '\0';
    std::string exe(buf);
    size_t slash = exe.find_last_of('/');
    return (slash == std::string::npos) ? "" : exe.substr(0, slash + 1);
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

    // Build search paths: origin directory first, then defaults
    std::vector<std::string> paths;
    if (!originPath.empty()) {
        std::string originDir = dirName(originPath);
        paths.push_back(originDir + "/");
        paths.push_back(originDir + "/modules/");
    }
    for (const auto& p : searchPaths_) {
        paths.push_back(p);
    }
    paths.emplace_back("./modules/");
    paths.emplace_back("./Cider/modules/");
    // Binary-relative fallback: build/cider -> <repo>/modules. Kept after
    // the CWD-relative paths so existing invocations behave identically.
    static const std::string binDir = binaryDir();
    if (!binDir.empty()) {
        paths.push_back(binDir + "../modules/");
        paths.push_back(binDir + "../../modules/");
    }

    for (const auto& basePath : paths) {
        std::string fileCandidate = basePath + pathSegment + ".cdr";
        if (fileExists(fileCandidate)) {
            return {ModuleType::CIDER_SOURCE_FILE, fileCandidate, moduleName};
        }

        std::string initCandidate = basePath + pathSegment + "/__init__.cdr";
        if (fileExists(initCandidate)) {
            return {ModuleType::CIDER_PACKAGE_DIR, basePath + pathSegment, moduleName};
        }
    }

    return {ModuleType::NOT_FOUND, "", ""};
}

}  // namespace cider
