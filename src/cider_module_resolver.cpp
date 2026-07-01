    #include "cider_module_resolver.h"
    #include <fstream>
    #include <iostream>
    #include <sys/stat.h>
    #include <unistd.h>
    #include <libgen.h>

    namespace Cider {

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

} // namespace Cider
