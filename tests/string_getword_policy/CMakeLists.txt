cmake_minimum_required(VERSION 3.20)
project(string_getword_policy_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(string_getword_policy_test test.cpp)
target_include_directories(string_getword_policy_test PRIVATE ../../src)

enable_testing()
add_test(NAME string_getword_policy COMMAND string_getword_policy_test)
