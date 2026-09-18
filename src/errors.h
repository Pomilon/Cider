#pragma once
#include <string>
#include <vector>

namespace cider {

struct Diagnostic {
    int line = 0;
    int col = 0;
    std::string message;
    std::string filename;
};

class ErrorReporter {
public:
    void error(const std::string& message, int line, int col) {
        diags_.push_back({line, col, message, currentFile_});
    }
    void error(const std::string& message, int line) {
        diags_.push_back({line, 0, message, currentFile_});
    }
    // Source attribution for multi-file builds (Issue #16): the driver sets
    // the main input file, and the importer sets each imported module's path
    // while it is parsed/collected, so every diagnostic knows its own file.
    void setFile(const std::string& path) { currentFile_ = path; }
    const std::string& getFile() const { return currentFile_; }
    bool hasErrors() const { return !diags_.empty(); }
    const std::vector<Diagnostic>& errors() const { return diags_; }
    void clear() { diags_.clear(); }

private:
    std::vector<Diagnostic> diags_;
    std::string currentFile_;
};

}  // namespace cider
