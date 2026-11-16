cmake_minimum_required(VERSION 3.20)
project(dynamic_layer_optimizer_test LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(dynamic_layer_optimizer_test
    test.cpp
)
target_include_directories(dynamic_layer_optimizer_test PRIVATE
    ../../src
)

enable_testing()
add_test(NAME dynamic_layer_optimizer COMMAND dynamic_layer_optimizer_test)
