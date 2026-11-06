cmake_minimum_required(VERSION 3.20)
project(pak_prefetch_policy_test LANGUAGES CXX)

add_executable(pak_prefetch_policy_test test.cpp)
target_include_directories(pak_prefetch_policy_test PRIVATE ../../src)
target_compile_features(pak_prefetch_policy_test PRIVATE cxx_std_17)

enable_testing()
add_test(NAME pak_prefetch_policy COMMAND pak_prefetch_policy_test)
