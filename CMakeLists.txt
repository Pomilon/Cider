# CMakeLists.txt — Cider v3.
#
# OUTPUT.md §3 requires byte-reproducible output, and gate L5 checks it by
# compiling the same IR twice from two different build directories and comparing
# bytes. So there is no absolute path in this file, no timestamp, and no
# generator-dependent ordering: nothing here may write a path into an artifact.
#
# There is one compiler target (`cider`) and one test target (`layout_test`).
# The test target exists because gate T4's second half is a test, not a grep —
# see tests/unit/layout_test.cpp for why both halves are needed.

cmake_minimum_required(VERSION 3.20)
project(cider LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Gate T1's first half is -Werror=switch, which is what makes a missing
# `enum class Kind` case a build failure instead of a silent fallthrough. It is
# here rather than in the test target so that every future target inherits it.
add_compile_options(-Wall -Wextra -Werror -Werror=switch -Wno-unused-parameter)

# --- The type system ----------------------------------------------------------
#
# TYPES.md §2.1: a type is a tagged union, and the tag is the one list that
# every switch in the compiler must agree with. Gate T1 polices that agreement,
# which is why these switches have no `default:` -- see kind.cpp.

add_library(cider_types STATIC src/types/kind.cpp src/types/type.cpp
                               src/types/signature.cpp src/types/context.cpp)
target_include_directories(cider_types PUBLIC src)

# --- The lexer ---------------------------------------------------------------
#
# LAYERS.md §3.1: one character in, one token out, with a position. May read
# the source text and nothing else.

add_library(cider_lexer STATIC src/lexer/token.cpp src/lexer/lexer.cpp)
target_include_directories(cider_lexer PUBLIC src)

# --- Tests --------------------------------------------------------------------

enable_testing()

add_executable(layout_test tests/unit/layout_test.cpp)
target_include_directories(layout_test PRIVATE src)

add_executable(type_test tests/unit/type_test.cpp)
target_link_libraries(type_test PRIVATE cider_types)
target_include_directories(type_test PRIVATE src)

add_executable(lexer_test tests/unit/lexer_test.cpp)
target_link_libraries(lexer_test PRIVATE cider_lexer)
target_include_directories(lexer_test PRIVATE src)

# The gates are a test like any other, so `ctest` alone is the whole gate. The
# script reports three states (PASS/FAIL/PENDING) and exits non-zero only on
# FAIL; a pending gate is unexamined, not satisfied, and the script says which
# file will make it checkable.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME gates COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gates.py)
add_test(NAME layout COMMAND layout_test)
add_test(NAME types COMMAND type_test)
add_test(NAME lexer COMMAND lexer_test)

# A gate that fails must fail the build, so nothing here is allowed to be
# "informational".
set_tests_properties(gates layout types lexer PROPERTIES LABELS "gate")
