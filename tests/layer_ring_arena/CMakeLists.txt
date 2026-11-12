cmake_minimum_required(VERSION 3.20)
project(layer_ring_arena_test LANGUAGES C CXX)

# Standalone test target; the root dinput8 target contains the production integration.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

set(LIBDEFLATE_BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_GZIP OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(../../third_party/libdeflate ${CMAKE_BINARY_DIR}/libdeflate)

add_executable(layer_ring_arena_test
    test.cpp
    ../../src/layer_ring_arena.cpp
)
target_include_directories(layer_ring_arena_test PRIVATE
    ../../src
    ../../third_party/libdeflate
    ../../third_party/libdeflate/lib
)
target_link_libraries(layer_ring_arena_test PRIVATE libdeflate_static)
target_compile_definitions(layer_ring_arena_test PRIVATE _CRT_SECURE_NO_WARNINGS)

enable_testing()
add_test(NAME layer_ring_arena_test COMMAND layer_ring_arena_test)
