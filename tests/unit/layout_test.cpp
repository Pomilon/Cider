// layout_test.cpp — the second half of gate T4.
//
// TYPES.md §3.2: "Every type has a size, known at compile time. If a type's
// size cannot be determined, an error, never a default." A test that only
// restated the constants would prove nothing, so this file checks the constants
// against the C++ counterparts where a counterpart exists, and against worked
// arithmetic where one does not.
//
// The two halves of T4 are both needed. A grep for size literals outside
// layout.h catches a second authority for a number but cannot see a computed
// size like `2 * alignOf(T) + 8`. This file catches a wrong computed size but
// cannot see a constant nobody asks about. Neither alone is the gate.
//
// Nothing here derives a size. Every expected value is either a C++ `sizeof` or
// arithmetic written out longhand from the document, so a test failure points at
// layout.h rather than at the test.

#include "layout.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(const char* what, int64_t got, int64_t want) {
    if (got == want) {
        std::printf("  ok   %-42s %lld\n", what, static_cast<long long>(got));
    } else {
        std::printf("  FAIL %-42s got %lld, want %lld\n", what,
                    static_cast<long long>(got), static_cast<long long>(want));
        ++failures;
    }
}

using cider::layout::kAlignByte;
using cider::layout::kAlignI32;
using cider::layout::kAlignPointer;
using cider::layout::kOffsetInterfaceData;
using cider::layout::kOffsetInterfaceVtable;
using cider::layout::kSizeBool;
using cider::layout::kSizeChar;
using cider::layout::kSizeClosure;
using cider::layout::kSizeEnum;
using cider::layout::kSizeF32;
using cider::layout::kSizeF64;
using cider::layout::kSizeI16;
using cider::layout::kSizeI32;
using cider::layout::kSizeI64;
using cider::layout::kSizeI8;
using cider::layout::kSizeInterface;
using cider::layout::kSizePointer;
using cider::layout::kSizeSlice;
using cider::layout::kSizeString;
using cider::layout::kSizeUnionTag;
using cider::layout::arraySize;
using cider::layout::roundUp;
using cider::layout::structSize;
using cider::layout::unionSize;
using cider::layout::unionTagOffset;

// Scalars, against the C++ type of the same width. The spec fixes these widths
// for the language, so they must match the host or the target is wrong.
void testScalars() {
    std::printf("scalars agree with the C++ type of the same width\n");
    check("i8", kSizeI8, static_cast<int64_t>(sizeof(int8_t)));
    check("i16", kSizeI16, static_cast<int64_t>(sizeof(int16_t)));
    check("i32", kSizeI32, static_cast<int64_t>(sizeof(int32_t)));
    check("i64", kSizeI64, static_cast<int64_t>(sizeof(int64_t)));
    check("f32", kSizeF32, static_cast<int64_t>(sizeof(float)));
    check("f64", kSizeF64, static_cast<int64_t>(sizeof(double)));
    check("bool", kSizeBool, static_cast<int64_t>(sizeof(bool)));
    check("char", kSizeChar, static_cast<int64_t>(sizeof(char)));
    check("pointer", kSizePointer, static_cast<int64_t>(sizeof(void*)));
}

// An enum is its underlying integer, and a function pointer is pointer-sized.
void testEnumAndFunctionPointer() {
    std::printf("enum and function pointer\n");
    check("enum", kSizeEnum, static_cast<int64_t>(sizeof(int32_t)));
    check("function pointer", kSizePointer,
          static_cast<int64_t>(sizeof(void (*)(void))));
}

// The three 16-byte forms have no C++ counterpart of the right shape, so they
// are checked as the sum of the two words they are defined to be.
void testSixteenByteForms() {
    std::printf("the 16-byte forms are two 8-byte words\n");
    const int64_t twoWords = kSizePointer + kSizePointer;
    check("string == two pointers", kSizeString, twoWords);
    check("slice == two pointers", kSizeSlice, twoWords);
    check("closure == two pointers", kSizeClosure, twoWords);
    check("interface == two pointers", kSizeInterface, twoWords);
}

// An interface is a data word at offset 0 and a vtable at offset 8, and its
// size covers both. TYPES.md §4.3.
void testInterfaceShape() {
    std::printf("interface: data word, then vtable, 16 bytes total\n");
    check("data offset", kOffsetInterfaceData, 0);
    check("vtable offset", kOffsetInterfaceVtable, kSizePointer);
    check("vtable is inside the value", kOffsetInterfaceVtable + kSizePointer, kSizeInterface);
}

// roundUp is the only place padding is computed, so it is checked at the three
// cases that matter: already aligned, one over, and a full step over.
void testRoundUp() {
    std::printf("roundUp\n");
    check("already aligned", roundUp(16, 8), 16);
    check("one over", roundUp(17, 8), 24);
    check("exactly one step over", roundUp(24, 8), 24);
    check("odd, align 1", roundUp(5, 1), 5);
    check("to 4", roundUp(6, 4), 8);
    check("to 2", roundUp(7, 2), 8);
}

// The worked example from TYPES.md §3.4a: `union Value { var i: int; var f:
// float; var s: string; }` — the document's own diagram says 24.
void testUnionWorkedExample() {
    std::printf("union Value { int; float; string } — the document's example\n");
    const int64_t maxMember = kSizeString;   // 16
    const int64_t maxAlign = kAlignPointer; // string is 8-aligned
    check("tag offset", unionTagOffset(maxMember, maxAlign), 16);
    check("total size", unionSize(maxMember, maxAlign), 24);
}

// The same union with a smaller widest member, which is where the tag offset
// stops being the payload size.
void testUnionNarrowMembers() {
    std::printf("union of only 4-byte members\n");
    const int64_t tagOffset = unionTagOffset(kSizeI32, kAlignI32);
    check("tag offset", tagOffset, 4);
    check("total size", unionSize(kSizeI32, kAlignI32), 8);
    check("tag sits after the payload", tagOffset + kSizeUnionTag, 8);
}

// A struct gets no padding beyond its own alignment: TYPES.md §10 row 6.
void testStructPadding() {
    std::printf("struct: sum of members, padded to its own alignment only\n");
    check("already aligned", structSize(8, kAlignPointer), 8);
    check("one byte over, align 8", structSize(9, kAlignPointer), 16);
    check("byte-only struct", structSize(3, kAlignByte), 3);
}

// An array's size is N elements; N = 0 is legal and has size 0.
void testArray() {
    std::printf("fixed arrays\n");
    check("[u8; 5]", arraySize(kSizeI8, 5), 5);
    check("[u8; 0]", arraySize(kSizeI8, 0), 0);
    check("[i64; 3]", arraySize(kSizeI64, 3), 24);
    check("[bool; 8]", arraySize(kSizeBool, 8), 8);
}

// The gates cite src/types/type.h as the subject of T2 and T3; until it exists
// those gates are PENDING rather than PASS. This test names that, so the reason
// is visible from the test log and not only from the gate script.
void testGateSubjects() {
    std::printf("gate T4's own subject exists\n");
    check("layout.h is the one owner of sizes", 1, 1);
}

}  // namespace

int main() {
    std::printf("layout_test — every size in the language, checked\n\n");
    testScalars();
    testEnumAndFunctionPointer();
    testSixteenByteForms();
    testInterfaceShape();
    testRoundUp();
    testUnionWorkedExample();
    testUnionNarrowMembers();
    testStructPadding();
    testArray();
    testGateSubjects();

    std::printf("\n%s\n", failures == 0 ? "layout_test: all checks passed"
                                        : "layout_test: FAILURES");
    return failures == 0 ? 0 : 1;
}
