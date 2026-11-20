cmake_minimum_required(VERSION 3.20)
project(pak_streaming_ring_test LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(../../third_party/minhook minhook_build)

set(LIBDEFLATE_BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_GZIP OFF CACHE BOOL "" FORCE)
add_subdirectory(../../third_party/libdeflate libdeflate_build)

add_executable(pak_streaming_ring_test
    test.cpp
    ../../src/pak_streaming_ring.cpp
    ../../src/diagnostic_logger.cpp
)

target_include_directories(pak_streaming_ring_test PRIVATE
    ../../src
    ../../third_party/libdeflate
    ../../third_party/minhook/include
)

target_link_libraries(pak_streaming_ring_test PRIVATE
    minhook
    libdeflate_static
    dbghelp
)

enable_testing()
add_test(NAME pak_streaming_ring COMMAND pak_streaming_ring_test)
