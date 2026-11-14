cmake_minimum_required(VERSION 3.20)
project(memory_allocator_test LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(MI_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MI_OVERRIDE OFF CACHE BOOL "" FORCE)
add_subdirectory(../../third_party/mimalloc mimalloc_sub)

add_executable(memory_allocator_test test.cpp)
target_include_directories(memory_allocator_test PRIVATE
    ../../src
    ../../third_party/mimalloc/include
)
target_link_libraries(memory_allocator_test PRIVATE
    mimalloc-static
)

add_executable(test_arena test_arena.cpp)
target_include_directories(test_arena PRIVATE
    ../../src
    ../../third_party/mimalloc/include
)
target_link_libraries(test_arena PRIVATE
    mimalloc-static
)

enable_testing()
add_test(NAME memory_allocator COMMAND memory_allocator_test)
