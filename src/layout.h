// layout.h — the only place in the compiler where a size or an alignment is
// written down. TYPES.md §3.1: "layout.h owns every size and alignment in the
// language. Nothing else computes one."
//
// Gate T4 enforces that. It is a grep for size-looking literals outside this
// file, plus tests/unit/layout_test.cpp, which asserts that every sizeOf agrees
// with the C++ counterpart where one exists. Both halves are needed: the grep
// cannot see a computed size like `2 * alignOf(T) + 8`, and the test cannot see
// a constant nobody ever asks about.
//
// Everything here is derived from TYPES.md §3.3 and §3.4a. Nothing here is a
// design decision made in this file; where a number looks arbitrary, the
// document says why.

#ifndef CIDER_LAYOUT_H
#define CIDER_LAYOUT_H

#include <cstddef>
#include <cstdint>

namespace cider {
namespace layout {

// --- The scalar widths -------------------------------------------------------
//
// FEATURES.md §1.1 fixes eight integer widths and four float widths. `int`,
// `uint`, `float` and `byte` are aliases, not distinct widths, so they get no
// constants of their own: naming SIZE_INT would create a second thing that has
// to agree with SIZE_I32, which is the copied-rule defect T6 exists to catch.

constexpr int64_t kSizeI8 = 1;
constexpr int64_t kSizeI16 = 2;
constexpr int64_t kSizeI32 = 4;
constexpr int64_t kSizeI64 = 8;
constexpr int64_t kSizeF32 = 4;
constexpr int64_t kSizeF64 = 8;

// A `bool` is one byte and holds 0 or 1. One byte rather than a bit: a bitfield
// is the construct for packing, and a one-byte bool keeps every load and store
// of it the same width as every other scalar.
constexpr int64_t kSizeBool = 1;

// `char` is a distinct 1-byte type, not an alias of u8: it carries an
// interpretation (printing it prints a character). Same width, different type —
// TYPES.md §10.2.
constexpr int64_t kSizeChar = 1;

// A pointer is 8 bytes at every depth, and so is a function pointer. One
// constant covers both because they are the same width and nothing in the
// language may depend on the difference; the distinction is in the type, not
// the size.
constexpr int64_t kSizePointer = 8;

// --- The aggregate forms -----------------------------------------------------

// An interface value is a data word and a vtable pointer. TYPES.md §4.3.
constexpr int64_t kOffsetInterfaceData = 0;
constexpr int64_t kOffsetInterfaceVtable = 8;
constexpr int64_t kSizeInterface = 16;

// A slice is a pointer and a length. TYPES.md §10.4.
constexpr int64_t kSizeSlice = 16;

// A closure value is a 16-byte header — a code pointer and a captures pointer.
// The captured values live in one allocation behind the second word, so the
// size of a closure *type* is 16 whatever it captures. TYPES.md §4.5, and the
// ruling in §10 row 9 that `sizeof` of a closure type is therefore 16.
constexpr int64_t kSizeClosure = 16;

// A string is a pointer and a length, immutable. TYPES.md §3.3.
constexpr int64_t kSizeString = 16;

// An enum is its underlying integer. TYPES.md §3.3.
constexpr int64_t kSizeEnum = 4;

// The union tag is a u32 index into the member list, and it is not a member.
constexpr int64_t kSizeUnionTag = 4;

// --- Alignment ---------------------------------------------------------------
//
// Every 8-byte form aligns to 8, every 4-byte form to 4. There is no 16-byte
// alignment in the language: no scalar is wider than 8, and an aggregate's
// alignment is the maximum of its members', so nothing can ask for more.
constexpr int64_t kAlignPointer = 8;
constexpr int64_t kAlignI64 = 8;
constexpr int64_t kAlignI32 = 4;
constexpr int64_t kAlignI16 = 2;
constexpr int64_t kAlignByte = 1;

// --- Derived quantities ------------------------------------------------------

// Round `value` up to a multiple of `alignment`. The one place padding is
// computed, because both struct trailing padding and the union tag offset need
// it and the two must not be able to disagree.
constexpr int64_t roundUp(int64_t value, int64_t alignment) {
    if (alignment <= 0) {
        return value;
    }
    const int64_t remainder = value % alignment;
    return remainder == 0 ? value : value + (alignment - remainder);
}

// A struct's size is the sum of its members rounded up to its own alignment,
// and no padding beyond that: TYPES.md §3.3 and the ruling in §10 row 6.
constexpr int64_t structSize(int64_t membersSize, int64_t maxMemberAlign) {
    return roundUp(membersSize, maxMemberAlign);
}

// A union's payload is as wide as its widest member, every member at offset 0,
// and the tag sits immediately after the payload padded to the payload's
// alignment. TYPES.md §3.4a. Worked through for `union Value { int; float;
// string }`: payload 16, tag offset roundUp(16, 8) = 16, total 20, rounded to
// the 8-byte alignment of `string` = 24.
constexpr int64_t unionTagOffset(int64_t maxMemberSize, int64_t maxMemberAlign) {
    return roundUp(maxMemberSize, maxMemberAlign);
}

constexpr int64_t unionSize(int64_t maxMemberSize, int64_t maxMemberAlign) {
    return roundUp(unionTagOffset(maxMemberSize, maxMemberAlign) + kSizeUnionTag, maxMemberAlign);
}

// A fixed array is N elements, each aligned to the element's alignment.
constexpr int64_t arraySize(int64_t elementSize, int64_t count) {
    return elementSize * count;
}

}  // namespace layout
}  // namespace cider

#endif  // CIDER_LAYOUT_H
