cmake_minimum_required(VERSION 3.20)
project(config_ini_test LANGUAGES CXX)

add_executable(config_ini_test test.cpp ../../src/config_manager.cpp)
target_include_directories(config_ini_test PRIVATE ../../src)
target_compile_features(config_ini_test PRIVATE cxx_std_17)
target_link_libraries(config_ini_test PRIVATE user32)

enable_testing()
add_test(NAME config_ini COMMAND config_ini_test)
