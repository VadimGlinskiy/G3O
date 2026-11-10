cmake_minimum_required(VERSION 3.20)
project(quest_load_optimizer_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(quest_load_optimizer_test test.cpp)

target_include_directories(quest_load_optimizer_test PRIVATE
    ../../src
)

enable_testing()
add_test(NAME quest_load_optimizer_test COMMAND quest_load_optimizer_test)
