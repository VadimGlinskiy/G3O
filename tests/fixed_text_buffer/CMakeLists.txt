cmake_minimum_required(VERSION 3.20)
project(fixed_text_buffer_test LANGUAGES CXX)

add_executable(fixed_text_buffer_test test.cpp)
target_include_directories(fixed_text_buffer_test PRIVATE ../../src)
target_compile_features(fixed_text_buffer_test PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(fixed_text_buffer_test PRIVATE /utf-8)
endif()

enable_testing()
add_test(NAME fixed_text_buffer COMMAND fixed_text_buffer_test)
