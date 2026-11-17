cmake_minimum_required(VERSION 3.20)
project(static_slab_arena_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(static_slab_arena_test
    test.cpp
    ../../src/static_slab_arena.cpp
)
target_include_directories(static_slab_arena_test PRIVATE
    ../../src
)
target_compile_definitions(static_slab_arena_test PRIVATE _CRT_SECURE_NO_WARNINGS)

enable_testing()
add_test(NAME static_slab_arena_test COMMAND static_slab_arena_test)
