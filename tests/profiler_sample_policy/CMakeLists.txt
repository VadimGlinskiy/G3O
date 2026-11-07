cmake_minimum_required(VERSION 3.20)
project(profiler_sample_policy_test LANGUAGES CXX)

add_executable(profiler_sample_policy_test test.cpp)
target_include_directories(profiler_sample_policy_test PRIVATE ../../src)
target_compile_features(profiler_sample_policy_test PRIVATE cxx_std_17)

enable_testing()
add_test(NAME profiler_sample_policy COMMAND profiler_sample_policy_test)
