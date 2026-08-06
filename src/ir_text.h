#pragma once

#include <string>
#include "ir.h"

namespace cider {

struct ParseError {
    std::string message;
    int line = 0;
};

Program parseIR(const std::string& text);
std::string printIR(const Program& prog);

}  // namespace cider
