// structural.h — the IR entry validator. Gate L4.
//
// "Every block terminated, every target valid" is a property of the IR, not of
// the source text, so this gate is the checker itself rather than a grep: it
// runs at the optimiser's entry, before any pass, and no pass runs on a module
// that fails it. A block without a terminator falls off into whatever follows
// it; a dangling target reads as a label defined later, or never.

#ifndef CIDER_OPT_STRUCTURAL_H
#define CIDER_OPT_STRUCTURAL_H

#include <cstdint>
#include <string>
#include <vector>

#include "ir/ir.h"

namespace cider {
namespace opt {

struct StructuralError {
    std::string function;
    uint32_t block = 0;
    std::string message;
};

// Every block terminated, every branch target valid, every operand defined
// before use (or a function parameter), every value defined exactly once.
// Empty means the module is well-formed.
std::vector<StructuralError> checkModule(const ir::Module& module);

}  // namespace opt
}  // namespace cider

#endif  // CIDER_OPT_STRUCTURAL_H
