cmake_minimum_required(VERSION 3.20)
project(minidump_worker_test VERSION 1.7.0 LANGUAGES CXX)

configure_file(../../src/g3o_build_info.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/g3o_build_info.h @ONLY)

add_executable(minidump_worker_test
    test.cpp
    ../../src/blackbox_recorder.cpp
    ../../src/pak_resolver.cpp)
target_include_directories(minidump_worker_test PRIVATE
    ../../src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    ../../third_party/minhook/include)
target_compile_features(minidump_worker_test PRIVATE cxx_std_17)
target_link_libraries(minidump_worker_test PRIVATE dbghelp psapi shell32 user32 version)

enable_testing()
add_test(NAME minidump_worker COMMAND minidump_worker_test)
