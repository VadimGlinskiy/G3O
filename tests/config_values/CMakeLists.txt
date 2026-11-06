cmake_minimum_required(VERSION 3.20)
project(config_values_test LANGUAGES CXX)

add_executable(config_values_test test.cpp)
target_include_directories(config_values_test PRIVATE ../../src)
target_compile_features(config_values_test PRIVATE cxx_std_17)

enable_testing()
add_test(NAME config_values COMMAND config_values_test)
