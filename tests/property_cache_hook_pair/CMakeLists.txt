cmake_minimum_required(VERSION 3.20)
project(property_cache_hook_pair_test LANGUAGES CXX)

add_executable(property_cache_hook_pair_test test.cpp)
target_include_directories(property_cache_hook_pair_test PRIVATE ../../src)
target_compile_features(property_cache_hook_pair_test PRIVATE cxx_std_17)

enable_testing()
add_test(NAME property_cache_hook_pair COMMAND property_cache_hook_pair_test)
