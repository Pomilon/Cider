#pragma once
#include <string>
#include <vector>

namespace cider {

struct Diagnostic {
    // CIDER-FEAT-005: diagnostics carry a severity so the CLI can report
    // warnings without failing the build. Errors keep their historical
    // behaviour (stage failure, exit 1); warnings are opt-in (--warn) and
    // never affect hasErrors()/exit codes. Kept as the LAST member so
    // existing aggregate initialisations default to Error.
    enum class Severity { Error, Warning };
    int line = 0;
    int col = 0;
    std::string message;
    std::string filename;
    Severity severity = Severity::Error;
};

class ErrorReporter {
public:
    void error(const std::string& message, int line, int col) {
        diags_.push_back({line, col, message, currentFile_});
    }
    void error(const std::string& message, int line) {
        diags_.push_back({line, 0, message, currentFile_});
    }
    // CIDER-FEAT-005: warning collection. Warnings live in their own list so
    // hasErrors()/errors() are byte-for-byte unaffected by --warn; callers
    // print them via warnings() only when the flag is on.
    void warning(const std::string& message, int line, int col) {
        warnings_.push_back({line, col, message, currentFile_, Diagnostic::Severity::Warning});
    }
    void warning(const std::string& message, int line) {
        warnings_.push_back({line, 0, message, currentFile_, Diagnostic::Severity::Warning});
    }
    // Source attribution for multi-file builds (Issue #16): the driver sets
    // the main input file, and the importer sets each imported module's path
    // while it is parsed/collected, so every diagnostic knows its own file.
    void setFile(const std::string& path) { currentFile_ = path; }
    const std::string& getFile() const { return currentFile_; }
    bool hasErrors() const { return !diags_.empty(); }
    bool hasWarnings() const { return !warnings_.empty(); }
    const std::vector<Diagnostic>& errors() const { return diags_; }
    const std::vector<Diagnostic>& warnings() const { return warnings_; }
    // All diagnostics in collection order (errors first, then warnings).
    std::vector<Diagnostic> diagnostics() const {
        std::vector<Diagnostic> all = diags_;
        all.insert(all.end(), warnings_.begin(), warnings_.end());
        return all;
    }
    void clear() {
        diags_.clear();
        warnings_.clear();
    }

private:
    std::vector<Diagnostic> diags_;
    std::vector<Diagnostic> warnings_;
    std::string currentFile_;
};

}  // namespace cider
