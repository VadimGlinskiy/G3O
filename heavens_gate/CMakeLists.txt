cmake_minimum_required(VERSION 3.20)
project(HeavensGate LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compile options for x86 MSVC
if(MSVC)
    add_compile_options(/O2 /Oi /Oy /W3 /EHsc)
endif()

# Static library for Heaven's Gate
add_library(heavens_gate STATIC
    heavens_gate.cpp
    heavens_gate.h
)

target_include_directories(heavens_gate PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Autonomous test executable
add_executable(test_heavens_gate
    test_heavens_gate.cpp
)

target_link_libraries(test_heavens_gate PRIVATE
    heavens_gate
)
