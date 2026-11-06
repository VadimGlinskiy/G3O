cmake_minimum_required(VERSION 3.20)
project(d3d_state_cache_test LANGUAGES CXX)

add_executable(d3d_state_cache_test test.cpp)
target_include_directories(d3d_state_cache_test PRIVATE ../../src)
target_compile_features(d3d_state_cache_test PRIVATE cxx_std_17)

enable_testing()
add_test(NAME d3d_state_cache COMMAND d3d_state_cache_test)
