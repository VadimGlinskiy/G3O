cmake_minimum_required(VERSION 3.20)
project(navigation_optimizer_test LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(navigation_optimizer_test
    test.cpp
)
target_include_directories(navigation_optimizer_test PRIVATE
    ../../src
)

enable_testing()
add_test(NAME navigation_optimizer COMMAND navigation_optimizer_test)
