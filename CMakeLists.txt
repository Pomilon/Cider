cmake_minimum_required(VERSION 3.16)
project(cider VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
    add_compile_options(-Wall -Wextra -Werror)
endif()

add_executable(cider src/main.cpp)

add_executable(ir_test tests/unit/ir_test.cpp)
target_include_directories(ir_test PRIVATE src)
add_test(NAME ir_test COMMAND ir_test)

add_executable(ir_text_test tests/unit/ir_text_test.cpp src/ir_text.cpp)
target_include_directories(ir_text_test PRIVATE src)
add_test(NAME ir_text_test COMMAND ir_text_test)

add_executable(vm_test tests/unit/vm_test.cpp src/vm.cpp src/ir_text.cpp)
target_include_directories(vm_test PRIVATE src)
add_test(NAME vm_test COMMAND vm_test)

add_executable(codegen_test tests/unit/codegen_test.cpp src/codegen.cpp src/ir_text.cpp)
target_include_directories(codegen_test PRIVATE src)
add_test(NAME codegen_test COMMAND codegen_test)
