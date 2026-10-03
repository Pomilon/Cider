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

# --- The AST ------------------------------------------------------------------
#
# Header-only: every node and the one visitor live in src/ast/ast.h, so there
# is nothing to compile. The INTERFACE target exists so tests link against
# something named, and so a future ast.cpp has a home.

add_library(cider_ast INTERFACE)
target_include_directories(cider_ast INTERFACE src)

# --- Constant evaluation -------------------------------------------------------
#
# LAYERS.md §3.4: one folder for source expressions, shared by the resolver,
# the checker and the optimiser.

add_library(cider_eval STATIC src/eval/eval.cpp)
target_link_libraries(cider_eval PUBLIC cider_types cider_ast)
target_include_directories(cider_eval PUBLIC src)

# --- The resolver ---------------------------------------------------------------
#
# LAYERS.md §3.3: headers of every module first, so module scope is
# order-independent and imports resolve recursively.

add_library(cider_resolve STATIC src/resolve/resolver.cpp)
target_link_libraries(cider_resolve PUBLIC cider_eval cider_ast cider_types)
target_include_directories(cider_resolve PUBLIC src)

# --- The checker -----------------------------------------------------------------
#
# LAYERS.md §3.4: the body walk. Writes each expression's Type and checks it
# in place. This is the one `public ASTVisitor` in src/ (gates T5/L1).

add_library(cider_check STATIC src/check/checker.cpp)
target_link_libraries(cider_check PUBLIC cider_resolve cider_eval cider_types)
target_include_directories(cider_check PUBLIC src)

# --- The IR --------------------------------------------------------------------

# IR.md: the instruction set, the aggregate representation and the ABI. Three
# per-instruction flags exist because dead-code elimination, LICM and inlining
# all depend on them.

add_library(cider_ir STATIC src/ir/ir.cpp)
target_link_libraries(cider_ir PUBLIC cider_types)
target_include_directories(cider_ir PUBLIC src)

# --- Lowering ------------------------------------------------------------------

# Driven by the checker's walk: LAYERS.md 3.5.1 says lowering queries the walk's
# result and does not re-walk the AST.

add_library(cider_lower STATIC src/lower/lower.cpp)
target_link_libraries(cider_lower PUBLIC cider_check cider_ir)
target_include_directories(cider_lower PUBLIC src)

# --- The parser ---------------------------------------------------------------
#
# LAYERS.md §3.2: accepts or rejects on syntax alone. Never resolves a name,
# never infers a type, never consults the resolver.

add_library(cider_parse STATIC src/parse/parser.cpp)
target_link_libraries(cider_parse PUBLIC cider_ast cider_lexer)
target_include_directories(cider_parse PUBLIC src)

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

add_executable(ast_test tests/unit/ast_test.cpp)
target_link_libraries(ast_test PRIVATE cider_ast cider_lexer)
target_include_directories(ast_test PRIVATE src)
add_test(NAME ast COMMAND ast_test)

add_executable(parser_test tests/unit/parser_test.cpp)
target_link_libraries(parser_test PRIVATE cider_parse cider_lexer)
target_include_directories(parser_test PRIVATE src)
add_test(NAME parser COMMAND parser_test)

add_executable(resolve_test tests/unit/resolve_test.cpp)
target_link_libraries(resolve_test PRIVATE cider_resolve cider_parse cider_lexer)
target_include_directories(resolve_test PRIVATE src)
add_test(NAME resolver COMMAND resolve_test)

add_executable(check_test tests/unit/check_test.cpp)
target_link_libraries(check_test PRIVATE cider_check cider_resolve cider_parse cider_lexer)
target_include_directories(check_test PRIVATE src)
add_test(NAME checker COMMAND check_test)

add_executable(lower_test tests/unit/lower_test.cpp)
target_link_libraries(lower_test PRIVATE cider_lower cider_check cider_parse cider_lexer)
target_include_directories(lower_test PRIVATE src)
add_test(NAME lowering COMMAND lower_test)

# --- The optimiser ---------------------------------------------------------------
#
# IR.md section 7: a pass may change the shape and not the meaning. The
# structural check runs at the entry, before any pass (gate L4).

add_library(cider_opt STATIC src/opt/structural.cpp src/opt/opt.cpp)
target_link_libraries(cider_opt PUBLIC cider_ir cider_types)
target_include_directories(cider_opt PUBLIC src)

add_executable(opt_test tests/unit/opt_test.cpp)
target_link_libraries(opt_test PRIVATE cider_opt cider_lower cider_check cider_parse cider_lexer)
target_include_directories(opt_test PRIVATE src)
add_test(NAME optimise COMMAND opt_test)

# A gate that fails must fail the build, so nothing here is allowed to be
# "informational".
set_tests_properties(gates layout types lexer ast parser resolver checker lowering optimise
             PROPERTIES LABELS "gate")
