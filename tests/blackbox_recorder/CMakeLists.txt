cmake_minimum_required(VERSION 3.20)
project(blackbox_recorder_test LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(blackbox_recorder_test
    test.cpp
    ../../src/blackbox_recorder.cpp
)
target_include_directories(blackbox_recorder_test PRIVATE
    ../../src
)

enable_testing()
add_test(NAME blackbox_recorder COMMAND blackbox_recorder_test)
