cmake_minimum_required(VERSION 3.16)
project(sharedbase_optimizations_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_testing()

add_executable(sharedbase_optimizations_test test.cpp)
target_include_directories(sharedbase_optimizations_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src")
add_test(NAME sharedbase_optimizations COMMAND sharedbase_optimizations_test)
