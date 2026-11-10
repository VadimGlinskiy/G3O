cmake_minimum_required(VERSION 3.20)
project(frustum_database_optimizer_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(frustum_database_optimizer_test test.cpp)

target_include_directories(frustum_database_optimizer_test PRIVATE
    ../../src
)

enable_testing()
add_test(NAME frustum_database_optimizer_test COMMAND frustum_database_optimizer_test)
