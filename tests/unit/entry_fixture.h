// entry_fixture.h — the test harness's source-level entry wrapper.
//
// v4 has no compiler startup shim: a native program defines its own
// `_start`. Test fixtures may still express a `main` function because the
// tests are about the language and the runtime, not about spelling the
// process entry in every case. This header appends a real Cider `_start`
// that calls `main` and exits with its value. It is test scaffolding, not a
// compiler feature: the emitted assembly contains both functions exactly as
// written here.

#ifndef CIDER_TESTS_ENTRY_FIXTURE_H
#define CIDER_TESTS_ENTRY_FIXTURE_H

#include <string>

namespace cider_test {

// Whether the source already spells a target entry. A fixture that defines
// its own `_start` or `efi_main` owns its entry and must not be wrapped.
inline bool hasSourceEntry(const std::string& text) {
    return text.find("func _start(") != std::string::npos ||
           text.find("func efi_main(") != std::string::npos;
}

inline bool hasMainFunction(const std::string& text) {
    return text.find("func main(") != std::string::npos;
}

inline const char* mainEntryFixture() {
    return R"CIDER(
func _start() -> void {
    var code: int = main();
    asm {
        "mov rax, 60"
        "mov edi, {code}"
        "syscall"
    }
}
)CIDER";
}

inline std::string withMainEntry(const std::string& text) {
    if (!hasMainFunction(text) || hasSourceEntry(text)) {
        return text;
    }
    return text + mainEntryFixture();
}

}  // namespace cider_test

#endif  // CIDER_TESTS_ENTRY_FIXTURE_H
