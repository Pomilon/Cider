#pragma once

#include <memory>
#include <string>

#include "ir.h"

namespace cider {

class CodeGenerator {
public:
    explicit CodeGenerator(const std::string& targetStr = "x86_64-linux");
    bool generate(ModuleIR& module, const std::string& outputPath,
                  const std::string& sourceFile = "");

private:
    std::string target_;
};

}  // namespace cider
