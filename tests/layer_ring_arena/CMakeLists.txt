cmake_minimum_required(VERSION 3.20)
project(layer_ring_arena_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(layer_ring_arena_test test.cpp)

target_include_directories(layer_ring_arena_test PRIVATE
    ../../src
)

enable_testing()
add_test(NAME layer_ring_arena_test COMMAND layer_ring_arena_test)
