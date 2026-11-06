cmake_minimum_required(VERSION 3.20)
project(savegame_profiler_policy_test LANGUAGES CXX)

add_executable(savegame_profiler_policy_test test.cpp)
target_include_directories(savegame_profiler_policy_test PRIVATE ../../src)
target_compile_features(savegame_profiler_policy_test PRIVATE cxx_std_17)

enable_testing()
add_test(NAME savegame_profiler_policy COMMAND savegame_profiler_policy_test)
