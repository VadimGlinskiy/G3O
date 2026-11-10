cmake_minimum_required(VERSION 3.20)
project(class_factory_optimizer_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(class_factory_optimizer_test test.cpp)

target_include_directories(class_factory_optimizer_test PRIVATE
    ../../src
)

enable_testing()
add_test(NAME class_factory_optimizer_test COMMAND class_factory_optimizer_test)
