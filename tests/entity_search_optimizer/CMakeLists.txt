cmake_minimum_required(VERSION 3.20)
project(entity_search_optimizer_test LANGUAGES CXX)

add_executable(entity_search_optimizer_test test.cpp)
target_include_directories(entity_search_optimizer_test PRIVATE ../../src)
target_compile_features(entity_search_optimizer_test PRIVATE cxx_std_17)

enable_testing()
add_test(NAME entity_search_optimizer COMMAND entity_search_optimizer_test)
