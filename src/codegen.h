#pragma once

#include <memory>
#include <string>

#include "abi.h"
#include "ir.h"

namespace cider {

class CodeGenerator {
public:
    explicit CodeGenerator(const std::string& targetStr = "x86_64-linux");
    // CIDER-FEAT-001: `compile -g` emits DWARF (.debug_line line tables and
    // minimal .debug_info function DIEs) into the NASM source. Off by
    // default; the default output stays byte-identical.
    void setDebugInfo(bool on) { debugInfo_ = on; }
    // CU source file recorded in .debug_info; falls back to the first
    // function's own source path when empty.
    void setSourcePath(const std::string& p) { sourcePath_ = p; }
    bool generate(ModuleIR& module, const std::string& outputPath,
                  const std::string& sourceFile = "");

private:
    std::string target_;
    bool debugInfo_ = false;
    std::string sourcePath_;
};

// CID-005: the ABI an unmarked function/call uses for a target.
Abi targetAbiFor(const std::string& targetStr);

}  // namespace cider
