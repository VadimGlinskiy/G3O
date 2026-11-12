cmake_minimum_required(VERSION 3.20)
project(selective_culling_optimizer_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(selective_culling_optimizer_test test.cpp)

target_include_directories(selective_culling_optimizer_test PRIVATE
    ../../src
)

enable_testing()
add_test(NAME selective_culling_optimizer_test COMMAND selective_culling_optimizer_test)
