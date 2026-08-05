cmake_minimum_required(VERSION 3.10)
project(Cider CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include)

file(GLOB SOURCES "src/*.cpp")

add_executable(ciderc ${SOURCES})

# Testing
enable_testing()

# GVN pass unit test (synthetic IR — Cider source cannot express
# register-operand redundancy because all variable reads are LOADs)
add_executable(gvn_pass_test test/gvn_pass_test.cpp)
add_test(NAME gvn_pass_test COMMAND gvn_pass_test)

# Discover test .cdr files and add test targets
file(GLOB CDR_TESTS "test/*.cdr")
foreach(CDR_FILE ${CDR_TESTS})
    get_filename_component(TEST_NAME ${CDR_FILE} NAME_WE)
    add_test(
        NAME ${TEST_NAME}
        COMMAND ${CMAKE_SOURCE_DIR}/test/run_test.sh ${CMAKE_BINARY_DIR}/ciderc ${CDR_FILE}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
endforeach()
