cmake_minimum_required(VERSION 3.20)
project(cpu_affinity_policy_test LANGUAGES CXX)

add_executable(cpu_affinity_policy_test test.cpp)
target_include_directories(cpu_affinity_policy_test PRIVATE ../../src)
target_compile_features(cpu_affinity_policy_test PRIVATE cxx_std_17)

enable_testing()
add_test(NAME cpu_affinity_policy COMMAND cpu_affinity_policy_test)
