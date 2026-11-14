cmake_minimum_required(VERSION 3.20)
project(crash_guards_policy_test LANGUAGES CXX)

add_executable(crash_guards_policy_test test.cpp)
target_include_directories(crash_guards_policy_test PRIVATE ../../src)
target_compile_features(crash_guards_policy_test PRIVATE cxx_std_17)

enable_testing()
add_test(NAME crash_guards_policy COMMAND crash_guards_policy_test)
