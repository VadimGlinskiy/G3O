cmake_minimum_required(VERSION 3.20)
project(pak_cache_budget_test LANGUAGES CXX)

add_executable(pak_cache_budget_test test.cpp)
target_include_directories(pak_cache_budget_test PRIVATE ../../src)
target_compile_features(pak_cache_budget_test PRIVATE cxx_std_17)

enable_testing()
add_test(NAME pak_cache_budget COMMAND pak_cache_budget_test)
