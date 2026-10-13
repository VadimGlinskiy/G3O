cmake_minimum_required(VERSION 3.20)
project(Gothic3Optima LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/O2 /Oi /Oy /W3 /EHsc /Zi)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /MAP:dinput8.map /DEBUG")
endif()

# Mimalloc configuration (static library, security overhead disabled for raw speed)
set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(MI_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MI_SECURE OFF CACHE BOOL "" FORCE)

# Libdeflate configuration
set(LIBDEFLATE_BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_GZIP OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_TESTS OFF CACHE BOOL "" FORCE)

# Subprojects
add_subdirectory(third_party/minhook)
add_subdirectory(third_party/mimalloc)
add_subdirectory(third_party/libdeflate)
add_subdirectory(heavens_gate)

# Main dinput8.dll Proxy library
add_library(dinput8 SHARED
    src/main.cpp
    src/dinput8_proxy.cpp
    src/dinput8_proxy.h
    src/dinput8.def
    src/memory_allocator.cpp
    src/memory_allocator.h
    src/lfh_enforcer.cpp
    src/lfh_enforcer.h
    src/d3d9_hook.cpp
    src/d3d9_hook.h
    src/cpu_topology.cpp
    src/cpu_topology.h
    src/streaming_optimizer.cpp
    src/streaming_optimizer.h
    src/media_optimizer.cpp
    src/media_optimizer.h
    src/math_simd_optimizer.cpp
    src/math_simd_optimizer.h
    src/savegame_guard.cpp
    src/savegame_guard.h
    src/version_watermark.cpp
    src/version_watermark.h
    src/fall_guard.cpp
    src/fall_guard.h
    src/lru_cache_manager.cpp
    src/lru_cache_manager.h
)

target_include_directories(dinput8 PRIVATE
    src
    third_party/minhook/include
    third_party/mimalloc/include
    third_party/libdeflate
    heavens_gate
)

target_link_libraries(dinput8 PRIVATE
    minhook
    mimalloc-static
    libdeflate_static
    heavens_gate
    d3d9
    winmm
)

# Output name must be dinput8.dll
set_target_properties(dinput8 PROPERTIES
    OUTPUT_NAME "dinput8"
    PREFIX ""
)
