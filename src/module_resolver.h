#pragma once

#include <set>
#include <string>
#include <vector>

namespace cider {

enum class ModuleType {
    NOT_FOUND,
    CIDER_SOURCE_FILE,   // .cdr
    CIDER_PACKAGE_DIR,   // directory with __init__.cdr
};

struct ResolutionResult {
    ModuleType type;
    std::string path;       // Absolute or relative path to file/dir
    std::string moduleName; // Resolved module name
};

class ModuleResolver {
public:
    ModuleResolver();

    void addSearchPath(const std::string& path);
    ResolutionResult resolve(const std::string& logicalPath, const std::string& originPath = "");

    const std::vector<std::string>& getSearchPaths() const { return searchPaths_; }

private:
    std::vector<std::string> searchPaths_;
};

}  // namespace cider
