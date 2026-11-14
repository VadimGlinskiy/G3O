# Gothic 3 Optima (G3O)

[Русский](README.md)

G3O is a high-performance 32-bit `dinput8.dll` proxy library for **Gothic 3** (v1.75.14). It features engine-level optimizations for the Genome Engine (decompression, scene graph, frustum culling database, property and template caching), borderless windowed mode, crash investigation tools (Crash Investigator), and load-time profiling.

Fully compatible with Community Patch 1.75.14, Update Pack, Parallel Universe Patch (PUP v1.1.1), and large mod builds (Union, Questpaket, Content Mod, LTM, Conquest, and setups with 300+ mods).

---

## Features and Architecture

### Performance and Load-Time Optimizations
- **Hardware-Accelerated PAK and Savegame Decompression (libdeflate SIMD):** Replaces scalar zlib in `FileSystem.dll` and `Game.dll` with an optimized libdeflate decompressor supporting AVX2 and BMI2. Accelerates mod archive and savegame unpacking by over 4x (from 9.2 s down to 2.2 s).
- **Speculative PAK Prefetcher (PakPrefetch):** Background worker thread pool that decompresses `.pak` data blocks ahead of engine requests based on traces from previous loads, eliminating 4–8 seconds of disk I/O stall.
- **O(1) Entity Property Cache:** Flat hash cache for `bCAccessorProperty::ResolveProperty` with zero dynamic allocations. Delivers 100% hit rate across 1,000,000+ lookups per load, eliminating redundant traversal of engine property tables.
- **Register-Optimized String Hashing (Fast DJB2) & Template Cache:** Register-based DJB2 string hashing and `eCTemplateEntityProxy` caching, cutting millions of CRT hash calls and accelerating template resolution in mod layers.
- **Batched Frustum Visibility Database (Frustum Database Optimizer):** Defers and deduplicates `eCFrustumDatabase::ExtractFromEntity` calls during entity spawning. Reduces intermediate visibility database rebuilds from 130,000+ down to ~9,000 per load.
- **Stepped Scene Node Pre-allocation (Scene Node Optimizer):** Replaces the engine's 25% incremental growth of `eCNode::m_ppChildren` with power-of-two capacity pre-allocations (1024 -> 8192 -> 32768) via native `bCMemoryAdmin::Realloc`. Eliminates 25–30 cascading reallocations per major world sector (`RB_Mod`, `SysDyn`).
- **Directional Shadow Caster Optimizer:** Replaces quadratic backwards linear scans and `memmove` shifts in `eCShadowMapDirectionalLightBase` with an O(1) hash index and Swap-and-Pop. Eliminates up to ~8 seconds of stall during camera teleportation (`GotoLoadedPosition`) and streaming stutters during active gameplay.
- **SharedBase Class Factory Optimizer:** Fast L1 cache for engine reflection type descriptors and Zero-Alloc `bCString` buffer reuse for `bCPropertyObjectSingleton::ReadObject`. Eliminates linear linked-list traversal across 43 hash buckets and prevents over 60,000 engine memory allocations during dynamic entity deserialization (`SysDyn`).
- **Info Manager and Navigation Optimizers:** These are separate hooks. Their exact actions and activation conditions are documented next to the corresponding settings in `G3O.ini`.
- **Component Search Optimizer (Entity Search Optimizer):** Caches vptr table entries in `eCEntity::SearchForEntity` with an early-exit path.
- **System Timer and Thread Scheduling:** Enables 1 ms OS timer resolution (`timeBeginPeriod(1)`), assigns high process priority, and automatically pins game threads to performance cores (P-cores) while avoiding efficiency cores (E-cores).

### Display
- **Borderless Windowed Mode:** Eliminates crashes, freezing, and resolution resets during Alt-Tab and window switching. Includes protection against accidental minimization when clicking secondary monitors (`PreventMinimizeOnFocusLoss`).
- **D3D9 State Filtering:** Deduplicates redundant `SetRenderState` and `SetSamplerState` calls when running on native Direct3D 9.

### Diagnostics and Crash Investigation
- **Crash Investigator:** Intercepts unhandled Win32 exceptions (SEH), engine fatal errors (`FatalError`), and "Where is the guru?" crashes. Generates `G3O_CrashReport_*.log` containing CPU registers, module lists, and call stacks, alongside minidumps (`G3O_Crash_*.dmp`) for debugging in Visual Studio / WinDbg.
- **Load Profiler (`EnableSavegameAudit`):** Detailed `G3O_LoadProfiler_*.log` providing millisecond-accurate breakdowns of disk reads, decompression, sector activations, and world initialization.
- **Performance Profiler (`EnableProfiler`):** Tracks FPS, 1% Low, p99 frame times, and CPU load in `G3O_profile_*.log`.

---

## Directory Structure and Configuration

All diagnostic and runtime cache files are kept cleanly inside the `G3O\` directory in the game folder:
```text
Gothic 3\
├── Gothic3.exe
├── dinput8.dll          (G3O proxy DLL)
├── G3O.ini              (main configuration file)
└── G3O\
    ├── G3O_CrashReport_*.log   (crash reports)
    ├── G3O_Crash_*.dmp         (memory minidumps)
    ├── G3O_LoadProfiler_*.log  (savegame load audit logs)
    ├── G3O_profile_*.log       (performance & frame time logs)
    └── G3O_PakTrace_*.csv      (PAK trace cache for prefetcher)
```

The configuration file `G3O.ini` resides next to `Gothic3.exe`. Every configuration key is documented in-place with its purpose, benefit, and valid options.

---

## Installation

1. Copy `dinput8.dll` and `G3O.ini` into your Gothic 3 game directory (beside `Gothic3.exe`).
   > Back up any existing `dinput8.dll` first (it might belong to another mod).
   > When updating G3O, back up your current `G3O.ini` before replacing it with the new default profile.
2. Adjust `G3O.ini` if desired. The shipped profile matches the project's default `G3O.ini`; diagnostic logs and profilers are off by default and can be enabled individually.
3. Optional: Install DXVK for Vulkan-based rendering.

---

## DXVK

DXVK translates Direct3D 9 calls to Vulkan, reducing CPU driver overhead. Select the version supported by your GPU driver:

- **1.10.1-async / 1.10.3-async** — Vulkan 1.1; asynchronous background shader compilation without stutter.
- **2.4.1** — Vulkan 1.3; stable upstream release with texture sharpness controls.
- **3.0.1** — Vulkan 1.4; requires modern GPU drivers.

Place `dxvk.conf` next to `Gothic3.exe`. Full configuration options are detailed in the [DXVK Options Guide](docs/DXVK_OPTIONS.md) (Russian).

---

## Building from Source

Requirements:
- Visual Studio 2022 (MSVC v143 x86 / 32-bit compiler)
- CMake 3.20+
- Git

Build steps:
```cmd
git submodule update --init --recursive
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
```

The output DLL is generated at `build/Release/dinput8.dll`.

Run test suite:
```cmd
python -c "import subprocess, glob; [subprocess.run([f], check=True) for f in glob.glob(r'build/tests/**/*_test.exe', recursive=True)]"
```

---

## License

This project is licensed under the [MIT License](LICENSE).
