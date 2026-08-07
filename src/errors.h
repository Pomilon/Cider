#pragma once
#include <string>
#include <vector>

namespace cider {

struct Diagnostic {
    int line = 0;
    int col = 0;
    std::string message;
};

class ErrorReporter {
public:
    void error(const std::string& message, int line, int col) {
        diags_.push_back({line, col, message});
    }
    void error(const std::string& message, int line) {
        diags_.push_back({line, 0, message});
    }
    bool hasErrors() const { return !diags_.empty(); }
    const std::vector<Diagnostic>& errors() const { return diags_; }
    void clear() { diags_.clear(); }

private:
    std::vector<Diagnostic> diags_;
};

}  // namespace cider
