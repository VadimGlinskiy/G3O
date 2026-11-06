cmake_minimum_required(VERSION 3.20)
project(vfs_cache_test LANGUAGES CXX)

add_executable(vfs_cache_test test.cpp)
target_include_directories(vfs_cache_test PRIVATE ../../src)
target_compile_features(vfs_cache_test PRIVATE cxx_std_17)

enable_testing()
add_test(NAME vfs_cache COMMAND vfs_cache_test)
