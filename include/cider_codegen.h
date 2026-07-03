#ifndef CIDER_CODEGEN_H
#define CIDER_CODEGEN_H

#include "cider_ir.h"
#include <string>
#include <vector>
#include <memory>

namespace Cider {

    class TargetBackend {
    public:
        virtual ~TargetBackend() = default;
        
        virtual bool generate(ModuleIR& module, const std::string& outputPath,
                              const std::string& sourceFile = "") = 0;

    protected:
        void addText(const std::string& line) { textSection.push_back(line); }
        void addData(const std::string& line) { dataSection.push_back(line); }
        void addBss(const std::string& line) { bssSection.push_back(line); }

        std::vector<std::string> textSection;
        std::vector<std::string> dataSection;
        std::vector<std::string> bssSection;
    };

    class CodeGenerator {
    public:
        CodeGenerator(const std::string& targetStr);
        bool generate(ModuleIR& module, const std::string& outputPath,
                      const std::string& sourceFile = "");

    private:
        std::unique_ptr<TargetBackend> backend;
    };

} // namespace Cider

#endif // CIDER_CODEGEN_H