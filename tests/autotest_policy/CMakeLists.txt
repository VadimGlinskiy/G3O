cmake_minimum_required(VERSION 3.20)
project(autotest_policy_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(autotest_policy_test test.cpp)
target_include_directories(autotest_policy_test PRIVATE ../../src)

enable_testing()
add_test(NAME autotest_policy COMMAND autotest_policy_test)
