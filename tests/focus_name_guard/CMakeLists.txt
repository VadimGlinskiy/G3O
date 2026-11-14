cmake_minimum_required(VERSION 3.20)
project(focus_name_guard_policy_test LANGUAGES CXX)

add_executable(focus_name_guard_policy_test test.cpp)
target_include_directories(focus_name_guard_policy_test PRIVATE ../../src)
target_compile_features(focus_name_guard_policy_test PRIVATE cxx_std_17)

enable_testing()
add_test(NAME focus_name_guard_policy COMMAND focus_name_guard_policy_test)
