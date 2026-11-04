cmake_minimum_required(VERSION 3.20)
project(Gothic3Optima VERSION 1.3.5 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/O2 /Oi /Oy /W3 /EHsc /Zi /utf-8)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /MAP:dinput8.map /DEBUG")
endif()

# The PUP-compatible core does not replace the engine's caches, threading, audio,
# file system, save writer or GPU capability checks.
add_subdirectory(third_party/minhook)

# Main dinput8.dll Proxy library
add_library(dinput8 SHARED
    src/main.cpp
    src/dinput8_proxy.cpp
    src/dinput8_proxy.h
    src/dinput8.def
    src/version_watermark.cpp
    src/version_watermark.h
    src/diagnostic_logger.cpp
    src/diagnostic_logger.h
    src/crash_handler.cpp
    src/crash_handler.h
    src/borderless_window.cpp
    src/borderless_window.h
    src/camera_controller.cpp
    src/camera_controller.h
    src/config_manager.cpp
    src/config_manager.h
    src/math_simd_optimizer.cpp
    src/math_simd_optimizer.h
    src/d3d9_hook.cpp
    src/d3d9_hook.h
    src/cpu_topology.cpp
    src/cpu_topology.h
)

target_include_directories(dinput8 PRIVATE
    src
    third_party/minhook/include
)

target_link_libraries(dinput8 PRIVATE
    minhook
    dbghelp
    psapi
    winmm
)

# Output name must be dinput8.dll
set_target_properties(dinput8 PROPERTIES
    OUTPUT_NAME "dinput8"
    PREFIX ""
)
