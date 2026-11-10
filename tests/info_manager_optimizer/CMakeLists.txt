cmake_minimum_required(VERSION 3.20)
project(info_manager_optimizer_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(info_manager_optimizer_test test.cpp)

target_include_directories(info_manager_optimizer_test PRIVATE
    ../../src
)

enable_testing()
add_test(NAME info_manager_optimizer_test COMMAND info_manager_optimizer_test)
