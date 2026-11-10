cmake_minimum_required(VERSION 3.20)
project(Gothic3Optima VERSION 1.6.1 LANGUAGES C CXX)

configure_file(src/g3o_build_info.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/g3o_build_info.h @ONLY)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(G3O_ENABLE_TELEMETRY "Enable detailed load profiling and diagnostic audit hooks" OFF)
if(G3O_ENABLE_TELEMETRY)
    add_compile_definitions(G3O_ENABLE_TELEMETRY=1)
endif()

if(MSVC)
    add_compile_options(/O2 /Oi /Oy /W3 /EHsc /Zi /utf-8 /arch:SSE2)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /MAP:dinput8.map /DEBUG")
endif()

# G3O is a dinput8 proxy; optional hooks and optimizations are controlled by G3O.ini.
add_subdirectory(third_party/minhook)

set(LIBDEFLATE_BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_GZIP OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/libdeflate)

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
    src/recent_event_buffer.h
    src/fixed_text_buffer.h
    src/blackbox_recorder.cpp
    src/blackbox_recorder.h
    src/pak_resolver.cpp
    src/pak_resolver.h
    src/crash_handler.cpp
    src/crash_handler.h
    src/engine_fatal_report.h
    src/applied_config_snapshot.h
    src/timestamped_path.h
    src/g3o_output_path.h
    src/diag_output_bridge.h
    src/borderless_window.cpp
    src/borderless_window.h
    src/camera_controller.cpp
    src/camera_controller.h
    src/config_manager.cpp
    src/config_manager.h
    src/math_simd_optimizer.cpp
    src/math_simd_optimizer.h
    src/sharedbase_string_compare.h
    src/window_input_policy.h
    src/d3d_state_cache.h
    src/pak_cache_budget.h
    src/pak_read_at.h
    src/savegame_profiler_policy.h
    src/layer_audit_metrics.h
    src/pak_prefetch_policy.h
    src/d3d9_hook.cpp
    src/d3d9_hook.h
    src/profiler.cpp
    src/profiler.h
    src/cpu_topology.cpp
    src/cpu_topology.h
    src/cpu_affinity_policy.h
    src/vegetation_optimizer.cpp
    src/vegetation_optimizer.h
    src/decompressor_avx2.c
    src/decompressor_avx2.h
    src/fast_pak_decompressor.cpp
    src/fast_pak_decompressor.h
    src/frustum_database_policy.h
    src/frustum_database_optimizer.cpp
    src/frustum_database_optimizer.h
    src/fast_djb2.h
    src/template_cache_policy.h
    src/hash_proxy_optimizer.cpp
    src/hash_proxy_optimizer.h
    src/navigation_optimizer.cpp
    src/navigation_optimizer.h
    src/scene_node_optimizer.cpp
    src/scene_node_optimizer.h
    src/shadow_caster_policy.h
    src/shadow_caster_optimizer.cpp
    src/shadow_caster_optimizer.h
    src/class_factory_policy.h
    src/class_factory_optimizer.cpp
    src/class_factory_optimizer.h
    src/pvs_prefetch_policy.h
    src/pvs_prefetch_optimizer.cpp
    src/pvs_prefetch_optimizer.h
    src/quest_load_optimizer.cpp
    src/quest_load_optimizer.h
    src/info_manager_optimizer.cpp
    src/info_manager_optimizer.h
    src/layer_ring_arena.cpp
    src/layer_ring_arena.h
    src/property_cache_optimizer.cpp
    src/property_cache_optimizer.h
    src/entity_search_optimizer.cpp
    src/entity_search_optimizer.h
    src/entity_search_policy.h
    src/savegame_profiler.cpp
    src/savegame_profiler.h
    src/pak_prefetch.cpp
    src/pak_prefetch.h
    src/load_sampler.cpp
    src/load_sampler.h
    src/vfs_cache_optimizer.cpp
    src/vfs_cache_optimizer.h
    src/vfs_cache_core.h
    src/config_value_parser.h
    src/pak_tracer.cpp
    src/pak_tracer.h
)

if(MSVC)
    # Enable AVX2 and BMI2 only for the dedicated hardware-accelerated translation unit (C11 mode)
    set_source_files_properties(src/decompressor_avx2.c PROPERTIES COMPILE_FLAGS "/TC /arch:AVX2 /Oi /O2")
endif()

target_include_directories(dinput8 PRIVATE
    src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    third_party/minhook/include
    third_party/libdeflate
    third_party/libdeflate/lib
)

target_compile_definitions(dinput8 PRIVATE G3O_BUILD_CONFIGURATION=\"$<CONFIG>\")

target_link_libraries(dinput8 PRIVATE
    minhook
    libdeflate_static
    dbghelp
    psapi
    version
    winmm
)

# Output name must be dinput8.dll
set_target_properties(dinput8 PROPERTIES
    OUTPUT_NAME "dinput8"
    PREFIX ""
)
