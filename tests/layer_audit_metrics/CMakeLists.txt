cmake_minimum_required(VERSION 3.20)
project(layer_audit_metrics_test LANGUAGES CXX)

add_executable(layer_audit_metrics_test test.cpp)
target_include_directories(layer_audit_metrics_test PRIVATE ../../src)
target_compile_features(layer_audit_metrics_test PRIVATE cxx_std_17)

enable_testing()
add_test(NAME layer_audit_metrics COMMAND layer_audit_metrics_test)
