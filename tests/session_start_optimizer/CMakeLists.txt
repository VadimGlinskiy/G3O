cmake_minimum_required(VERSION 3.20)
project(session_start_optimizer_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(session_start_optimizer_test test.cpp)

target_include_directories(session_start_optimizer_test PRIVATE
    ../../src
)

enable_testing()
add_test(NAME session_start_optimizer_test COMMAND session_start_optimizer_test)
