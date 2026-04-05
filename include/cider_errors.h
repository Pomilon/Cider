#ifndef CIDER_ERRORS_H
#define CIDER_ERRORS_H

#include <string>
#include <iostream>
#include <vector>

namespace Cider {

    enum class ErrorLevel {
        WARNING,
        ERROR,
        FATAL
    };

    struct Diagnostic {
        ErrorLevel level;
        std::string message;
        int line;
        int col;
        std::string fileName;
    };

    class ErrorReporter {
    public:
        ErrorReporter() : errorCount_(0) {}

        void report(ErrorLevel level, const std::string& msg, int line = -1, int col = -1, const std::string& file = "") {
            Diagnostic diag = {level, msg, line, col, file};
            diagnostics_.push_back(diag);

            if (level == ErrorLevel::ERROR || level == ErrorLevel::FATAL) {
                errorCount_++;
            }

            std::string levelStr = (level == ErrorLevel::WARNING) ? "\033[1;33mwarning\033[0m" : "\033[1;31merror\033[0m";
            
            std::cerr << levelStr;
            if (!file.empty()) std::cerr << " in " << file;
            if (line != -1) {
                std::cerr << " at line " << line;
                if (col != -1) std::cerr << ":" << col;
            }
            std::cerr << ": " << msg << "\n";

            if (level == ErrorLevel::FATAL) {
                exit(1);
            }
        }

        void error(const std::string& msg, int line = -1, int col = -1, const std::string& file = "") {
            report(ErrorLevel::ERROR, msg, line, col, file);
        }

        void warning(const std::string& msg, int line = -1, int col = -1, const std::string& file = "") {
            report(ErrorLevel::WARNING, msg, line, col, file);
        }

        bool hasErrors() const { return errorCount_ > 0; }
        int getErrorCount() const { return errorCount_; }

    private:
        std::vector<Diagnostic> diagnostics_;
        int errorCount_;
    };

} // namespace Cider

#endif
