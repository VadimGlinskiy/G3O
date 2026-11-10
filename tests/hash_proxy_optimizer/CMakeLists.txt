cmake_minimum_required(VERSION 3.20)
project(hash_proxy_optimizer_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(hash_proxy_optimizer_test test.cpp)

target_include_directories(hash_proxy_optimizer_test PRIVATE
    ../../src
)

enable_testing()
add_test(NAME hash_proxy_optimizer_test COMMAND hash_proxy_optimizer_test)
