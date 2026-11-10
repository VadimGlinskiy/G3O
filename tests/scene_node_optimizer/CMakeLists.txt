cmake_minimum_required(VERSION 3.20)
project(scene_node_optimizer_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(scene_node_optimizer_test test.cpp)

target_include_directories(scene_node_optimizer_test PRIVATE
    ../../src
)

enable_testing()
add_test(NAME scene_node_optimizer_test COMMAND scene_node_optimizer_test)
