cmake_minimum_required(VERSION 3.16)
project(cider VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
    add_compile_options(-Wall -Wextra -Werror)
endif()

enable_testing()

add_executable(cider src/main.cpp src/lexer.cpp src/parser.cpp src/symbols.cpp src/module_resolver.cpp src/ir.cpp src/vm.cpp)
target_include_directories(cider PRIVATE src)

add_executable(ir_test tests/unit/ir_test.cpp src/ir.cpp)
target_include_directories(ir_test PRIVATE src)
add_test(NAME ir_test COMMAND ir_test)

add_executable(ir_gen_test tests/unit/ir_gen_test.cpp src/ir.cpp src/lexer.cpp src/parser.cpp src/symbols.cpp src/module_resolver.cpp)
target_include_directories(ir_gen_test PRIVATE src)
add_test(NAME ir_gen_test COMMAND ir_gen_test)

add_executable(vm_test tests/unit/vm_test.cpp src/vm.cpp)
target_include_directories(vm_test PRIVATE src)
add_test(NAME vm_test COMMAND vm_test)

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




find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME harness COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_tests.py)
endif()

add_executable(parser_recovery_test tests/unit/parser_recovery_test.cpp src/parser.cpp src/lexer.cpp)
target_include_directories(parser_recovery_test PRIVATE src)
add_test(NAME parser_recovery_test COMMAND parser_recovery_test)

add_executable(ast_visitor_test tests/unit/ast_visitor_test.cpp)
target_include_directories(ast_visitor_test PRIVATE src)
add_test(NAME ast_visitor_test COMMAND ast_visitor_test)

add_executable(symbols_test tests/unit/symbols_test.cpp src/symbols.cpp)
target_include_directories(symbols_test PRIVATE src)
add_test(NAME symbols_test COMMAND symbols_test)

add_executable(symbol_collector_test tests/unit/symbol_collector_test.cpp src/symbols.cpp src/parser.cpp src/lexer.cpp src/module_resolver.cpp)
target_include_directories(symbol_collector_test PRIVATE src)
add_test(NAME symbol_collector_test COMMAND symbol_collector_test)

add_executable(type_checker_test tests/unit/type_checker_test.cpp src/symbols.cpp src/parser.cpp src/lexer.cpp src/module_resolver.cpp)
target_include_directories(type_checker_test PRIVATE src)
add_test(NAME type_checker_test COMMAND type_checker_test)

add_executable(module_resolver_test tests/unit/module_resolver_test.cpp src/module_resolver.cpp src/symbols.cpp src/parser.cpp src/lexer.cpp)
target_include_directories(module_resolver_test PRIVATE src)
add_test(NAME module_resolver_test COMMAND module_resolver_test)

add_executable(vm_asm_test tests/unit/vm_asm_test.cpp src/vm.cpp)
target_include_directories(vm_asm_test PRIVATE src)
add_test(NAME vm_asm_test COMMAND vm_asm_test)
