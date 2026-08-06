cmake_minimum_required(VERSION 3.16)
project(cider VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
    add_compile_options(-Wall -Wextra -Werror)
endif()

enable_testing()

add_executable(cider src/main.cpp src/ir_text.cpp src/vm.cpp src/codegen.cpp src/lexer.cpp src/parser.cpp)
target_include_directories(cider PRIVATE src)

add_executable(ir_test tests/unit/ir_test.cpp)
target_include_directories(ir_test PRIVATE src)
add_test(NAME ir_test COMMAND ir_test)

add_executable(ast_test tests/unit/ast_test.cpp)
target_include_directories(ast_test PRIVATE src)
add_test(NAME ast_test COMMAND ast_test)

add_executable(lexer_test tests/unit/lexer_test.cpp src/lexer.cpp)
target_include_directories(lexer_test PRIVATE src)
add_test(NAME lexer_test COMMAND lexer_test)

add_executable(parser_expr_test tests/unit/parser_expr_test.cpp src/parser.cpp src/lexer.cpp)
target_include_directories(parser_expr_test PRIVATE src)
add_test(NAME parser_expr_test COMMAND parser_expr_test)

add_executable(parser_stmt_test tests/unit/parser_stmt_test.cpp src/parser.cpp src/lexer.cpp)
target_include_directories(parser_stmt_test PRIVATE src)
add_test(NAME parser_stmt_test COMMAND parser_stmt_test)

add_executable(ir_text_test tests/unit/ir_text_test.cpp src/ir_text.cpp)
target_include_directories(ir_text_test PRIVATE src)
add_test(NAME ir_text_test COMMAND ir_text_test)

add_executable(vm_test tests/unit/vm_test.cpp src/vm.cpp src/ir_text.cpp)
target_include_directories(vm_test PRIVATE src)
add_test(NAME vm_test COMMAND vm_test)

add_executable(codegen_test tests/unit/codegen_test.cpp src/codegen.cpp src/ir_text.cpp)
target_include_directories(codegen_test PRIVATE src)
add_test(NAME codegen_test COMMAND codegen_test)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME harness COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_tests.py)
endif()

add_executable(parser_recovery_test tests/unit/parser_recovery_test.cpp src/parser.cpp src/lexer.cpp)
target_include_directories(parser_recovery_test PRIVATE src)
add_test(NAME parser_recovery_test COMMAND parser_recovery_test)
