cmake_minimum_required(VERSION 3.16)
project(cider VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
    add_compile_options(-Wall -Wextra -Werror)
endif()

add_executable(cider src/main.cpp)
