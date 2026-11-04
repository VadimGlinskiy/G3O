# Gothic 3 Optima (G3O) — actual source state 1.3.1

[Русский](README.md)

**The current DLL is not an active engine-optimization suite.** It is an experimental `dinput8.dll` proxy; modules overlapping with Parallel Universe Patch (PUP) are excluded from the build. Building and packaging were checked; runtime compatibility with PUP **was not tested**.

## What is actually built into the DLL

| Source | Actual code behavior | Verification |
| --- | --- | --- |
| `src/dinput8_proxy.cpp` | Exports `DirectInput8Create` and forwards it to the system `dinput8.dll`; returns `E_FAIL` if loading fails. | Export verified; not tested in game. |
| `src/main.cpp` | Starts an initialization thread, initializes MinHook and attempts the optional actions below. | Builds; not tested in game. |
| `src/version_watermark.cpp` | Attempts to hook `CFFGFCWnd::SetWindowTextA` and append `G3O v1.3.1` to a matching version string. No matching export or string means no label. | Builds; label not verified in game. |

`src/main.cpp` also tries to load a third-party `G3SDK.dll` or `G3SE.dll` and call its `LoadPlugins` export for `Data\\G3SE\\Plugins`. Neither DLL exists in the supplied game installation: plugin loading has not been demonstrated there. This is **not** integration with `E:\Projects\gothic3sdk`: its headers and libraries are not linked. MinHook is the only third-party library included in the build.

## Present in the repository, but not active in the current DLL

None of these files is in the `dinput8` target in `CMakeLists.txt` or called from the current `src/main.cpp`. Source files remaining in the tree do **not** make these features available:

| Source | Subject of remaining code |
| --- | --- |
| `src/combat_guard.cpp` | Character culling. |
| `src/cpu_topology.cpp` | Process-wide performance-core affinity. |
| `src/d3d9_hook.cpp` | GPU capability checks. |
| `src/fall_guard.cpp` | Fall recovery. |
| `src/high_memory_cache.cpp` | Cache beyond the x86 address space. |
| `src/lfh_enforcer.cpp` | LFH heap settings. |
| `src/lru_cache_manager.cpp` | Resource-cache tracking and cleanup. |
| `src/math_simd_optimizer.cpp` | Math/physics hooks. |
| `src/media_optimizer.cpp` | Decompression, FMOD, timer, and video code. |
| `src/memory_allocator.cpp` | Memory-allocator replacement. |
| `src/multicore_manager.cpp` | Engine thread-pool changes. |
| `src/physics_stabilizer.cpp` | PhysX changes. |
| `src/savegame_guard.cpp` | Save backup and validation. |
| `src/streaming_optimizer.cpp` | Sector prefetching. |

`heavens_gate/` and its test source are also excluded from the build. `mimalloc` and `libdeflate` remain in `.gitmodules` but are not linked to the DLL. G3O **does not accelerate** world loading, threading, decompression, rendering, or physics; it **does not protect saves** or move textures to VRAM.

## PUP and separate DXVK packages

The local v1.1.1 `Parallel Universe Patch/Changelog_en.txt` describes PUP's own changes to resource threads, caches, prefetching, file system, FMOD, textures, and save writes. G3O therefore excludes the corresponding modules. This **does not prove** runtime compatibility.

`resources/dxvk/` contains third-party DXVK builds. `scripts/package_releases.ps1` creates one core archive (`dinput8.dll`, `README_INSTALL.txt`) and five separate DXVK archives (`d3d9.dll`, `dxgi.dll`, `dxvk.conf`, `README_DXVK.txt`). DXVK is not embedded in the core or implemented by G3O code. Packaging does not run or test the game.

## Build and test installation

```cmd
git submodule update --init third_party/minhook
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
```

Output: `build/Release/dinput8.dll`. Packaging: `powershell -ExecutionPolicy Bypass -File .\scripts\package_releases.ps1`, output in `dist/packages/`. The 1.3.1 core archive was checked for contents and for a DLL matching the build; this is **not** an in-game test.

Before installing, identify and back up any `dinput8.dll` already present next to `Gothic3.exe`: replacing it may disable another mod. Game files were not changed here. Features described for older 1.3.0 releases do not apply to this build.

Source license: [MIT](LICENSE). PUP, DXVK, and gothic3sdk are separate projects.
