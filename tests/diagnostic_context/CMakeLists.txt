cmake_minimum_required(VERSION 3.20)
project(diagnostic_context_test LANGUAGES CXX)

add_executable(diagnostic_context_test test.cpp)
target_include_directories(diagnostic_context_test PRIVATE
    ../../src
    ../../third_party/minhook/include)
target_compile_features(diagnostic_context_test PRIVATE cxx_std_17)
target_link_libraries(diagnostic_context_test PRIVATE shell32)
if(MSVC)
    target_compile_options(diagnostic_context_test PRIVATE /utf-8)
endif()

enable_testing()
add_test(NAME diagnostic_context COMMAND diagnostic_context_test)
