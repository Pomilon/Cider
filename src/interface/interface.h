// interface.h — the `.cidm` module interface artifact.
//
// An interface is a Cider-source-shaped declaration list with every body
// omitted: functions end at `;`, globals carry their type but no initialiser,
// and aggregate declarations keep their fields and method signatures. The
// lexer skips the leading `// cider-interface:` metadata comments, so the
// existing header pass parses the body; the driver only has to skip body
// checking for a module that came from one.
//
// The format is deliberately small. It is not a second IR: it is the source
// spelling of the module's declaration surface plus a content hash. A module
// whose surface this slice cannot describe (classes, interfaces, unions,
// generics) is marked `requires-source` and falls back to its source, loudly
// in the documentation and silently in the loader.

#ifndef CIDER_INTERFACE_INTERFACE_H
#define CIDER_INTERFACE_INTERFACE_H

#include <string>
#include <vector>

#include "ast/ast.h"

namespace cider {
namespace interfacefmt {

// Bump when the header meaning or declaration grammar changes. The cache key
// includes both numbers, so an old artifact is never mistaken for a current
// one.
constexpr int kFormatVersion = 1;
constexpr const char* kCompilerTag = "cider-v4-interface-1";

struct Dependency {
    std::string module;
    // SHA-256 of the dependency's interface text when it was built. Empty
    // when only the module name is known (a whole-program object's sidecar).
    std::string hash;
};

struct Header {
    int format = 0;
    std::string compiler;
    std::string module;
    std::string sourceHash;
    std::vector<Dependency> deps;
    // Empty when every declaration was rendered. Otherwise a short reason
    // (`classes`, `generics`, ...): an importer must use the source.
    std::string requiresSource;
};

struct RenderResult {
    std::string text;
    // Set when at least one declaration was skipped. The text still parses;
    // it is just not a complete surface.
    std::string requiresSource;
};

// The complete interface artifact for one module. `sourceHash` is the hash of
// the module's source bytes, and `deps` are its direct imports.
RenderResult render(const std::string& module, const Program& program,
                    const std::string& sourceHash,
                    const std::vector<Dependency>& deps);

// Reads the metadata comments. The body is not inspected.
bool parseHeader(const std::string& text, Header& header, std::string& error);

// SHA-256 in lowercase hex. Used for source hashes, interface hashes and
// content-addressed cache keys; no external tool is required.
std::string sha256(const std::string& text);

// `std.io` -> `std/io`; the path fragment an interface directory uses.
std::string modulePath(const std::string& module);

}  // namespace interfacefmt
}  // namespace cider

#endif  // CIDER_INTERFACE_INTERFACE_H
