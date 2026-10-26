// alloc_fixture.h — the test harness's allocation operators.
//
// Unit tests that exercise `new`/`delete` need a library operator, exactly
// as a real program does. This fixture is prepended only when the source
// uses an allocation expression and does not declare its own operator. It
// is test scaffolding, not a compiler prelude.

#ifndef CIDER_TESTS_ALLOC_FIXTURE_H
#define CIDER_TESTS_ALLOC_FIXTURE_H

#include <cctype>
#include <string>

namespace cider_test {

// A `new`/`delete` spelling with identifier boundaries. `try_new` does not
// match `new` because the preceding character is an identifier character.
inline bool hasAllocationWord(const std::string& text, const char* word) {
    std::string needle(word);
    std::string::size_type at = 0;
    while ((at = text.find(needle, at)) != std::string::npos) {
        bool left = at == 0 || !(std::isalnum(static_cast<unsigned char>(
                                     text[at - 1])) ||
                                 text[at - 1] == '_');
        std::string::size_type after = at + needle.size();
        bool right = after >= text.size() ||
                     !(std::isalnum(static_cast<unsigned char>(text[after])) ||
                       text[after] == '_');
        if (left && right) {
            return true;
        }
        at = after;
    }
    return false;
}

inline bool needsAllocationFixture(const std::string& text) {
    if (text.find("operator new") != std::string::npos ||
        text.find("operator delete") != std::string::npos) {
        return false;
    }
    return hasAllocationWord(text, "new") || hasAllocationWord(text, "delete");
}

inline const char* allocationFixture() {
    return R"CIDER(
func operator new(size: i64, align: i64) -> *void {
    var step: i64 = align;
    if (step < 1) { step = 1; }
    var len: i64 = size + step + 8;
    var at: i64 = 0;
    asm {
        "mov rax, 9"
        "xor rdi, rdi"
        "mov rsi, {len:q}"
        "mov rdx, 3"
        "mov r10, 34"
        "mov r8, -1"
        "xor r9, r9"
        "syscall"
        "mov {at:q}, rax"
    }
    if (at <= 0) { return nilPtr; }
    return ((at + step - 1) / step * step) as *void;
}
func operator delete(ptr: *void) -> void { }
)CIDER";
}

inline std::string withAllocationFixture(const std::string& text) {
    if (!needsAllocationFixture(text)) {
        return text;
    }
    return std::string(allocationFixture()) + text;
}

}  // namespace cider_test

#endif  // CIDER_TESTS_ALLOC_FIXTURE_H
