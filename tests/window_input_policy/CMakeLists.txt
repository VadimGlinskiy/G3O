cmake_minimum_required(VERSION 3.20)
project(window_input_policy_test LANGUAGES CXX)

add_executable(window_input_policy_test test.cpp)
target_include_directories(window_input_policy_test PRIVATE ../../src)
target_compile_features(window_input_policy_test PRIVATE cxx_std_17)

enable_testing()
add_test(NAME window_input_policy COMMAND window_input_policy_test)
