#pragma once

#include <string>
#include "ir.h"

namespace cider {

struct CodegenError {
    std::string message;
};

std::string generateAsm(const Program& prog);

}  // namespace cider
