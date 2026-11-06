cmake_minimum_required(VERSION 3.20)
project(engine_fatal_report_test LANGUAGES CXX)

add_executable(engine_fatal_report_test test.cpp)
target_include_directories(engine_fatal_report_test PRIVATE ../../src)
target_compile_features(engine_fatal_report_test PRIVATE cxx_std_17)

enable_testing()
add_test(NAME engine_fatal_report COMMAND engine_fatal_report_test)
