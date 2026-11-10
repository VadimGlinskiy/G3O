cmake_minimum_required(VERSION 3.20)
project(guid_map_optimizer_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(guid_map_optimizer_test test.cpp)

target_include_directories(guid_map_optimizer_test PRIVATE
    ../../src
)

enable_testing()
add_test(NAME guid_map_optimizer_test COMMAND guid_map_optimizer_test)
