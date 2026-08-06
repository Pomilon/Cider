#pragma once

#include <string>
#include "ir.h"

namespace cider {

struct VMError {
    std::string message;
};

struct VMResult {
    int exitCode = 0;
    std::string stdoutText;
    std::string stderrText;
};

VMResult runVM(const Program& prog);

}  // namespace cider
