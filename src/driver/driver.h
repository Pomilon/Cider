// driver.h — the `cider` command. OUTPUT.md section 1.
//
// One function per subcommand, all returning an exit code and writing
// diagnostics to the error string rather than throwing: the only throw in the
// driver is for I/O and argument failures the caller cannot anticipate, and
// the top-level catch in main.cpp turns those into diagnostics (gate L6).
// Nothing here prints; the caller decides where text goes.

#ifndef CIDER_DRIVER_DRIVER_H
#define CIDER_DRIVER_DRIVER_H

#include <string>
#include <vector>

namespace cider {
namespace driver {

// Runs the command named by args (without argv[0]). `out` receives normal
// output, `err` receives diagnostics. Answers a process exit code.
int runDriver(const std::vector<std::string>& args, std::string& out, std::string& err);

}  // namespace driver
}  // namespace cider

#endif  // CIDER_DRIVER_DRIVER_H
