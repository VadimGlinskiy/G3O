cmake_minimum_required(VERSION 3.20)
project(recent_event_buffer_test LANGUAGES CXX)

add_executable(recent_event_buffer_test test.cpp)
target_include_directories(recent_event_buffer_test PRIVATE ../../src)
target_compile_features(recent_event_buffer_test PRIVATE cxx_std_17)
enable_testing()
add_test(NAME recent_event_buffer COMMAND recent_event_buffer_test)
