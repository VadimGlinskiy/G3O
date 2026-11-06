cmake_minimum_required(VERSION 3.20)
project(pak_read_at_test LANGUAGES CXX)

add_executable(pak_read_at_test test.cpp)
target_include_directories(pak_read_at_test PRIVATE ../../src)
target_compile_features(pak_read_at_test PRIVATE cxx_std_17)

enable_testing()
add_test(NAME pak_read_at COMMAND pak_read_at_test)
