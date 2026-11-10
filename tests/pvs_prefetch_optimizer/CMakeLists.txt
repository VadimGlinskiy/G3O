cmake_minimum_required(VERSION 3.20)
project(pvs_prefetch_optimizer_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(pvs_prefetch_optimizer_test test.cpp)

target_include_directories(pvs_prefetch_optimizer_test PRIVATE
    ../../src
)

enable_testing()
add_test(NAME pvs_prefetch_optimizer_test COMMAND pvs_prefetch_optimizer_test)
