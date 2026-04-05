#include "cider_module_resolver.h"
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

namespace Cider {

    static bool fileExists(const std::string& name) {
        struct stat buffer;   
        return (stat(name.c_str(), &buffer) == 0); 
    }

    ModuleResolver::ModuleResolver() {
        // Default search paths
        addSearchPath(".");
        addSearchPath("./Cider");
        addSearchPath("./modules");
        addSearchPath("./Cider/modules");
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

        for (const auto& basePath : searchPaths_) {
            // Try .cdr file
            std::string fileCandidate = basePath + pathSegment + ".cdr";
            if (fileExists(fileCandidate)) {
                return {ModuleType::CIDER_SOURCE_FILE, fileCandidate, moduleName};
            }

            // Try package directory
            std::string initCandidate = basePath + pathSegment + "/__init__.cdr";
            if (fileExists(initCandidate)) {
                return {ModuleType::CIDER_PACKAGE_DIR, basePath + pathSegment, moduleName};
            }
        }

        return {ModuleType::NOT_FOUND, "", ""};
    }

} // namespace Cider
