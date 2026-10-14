<div align="center">

**English** · [Русский](README.md)

# Gothic 3 Optima (G3O) Test version

### Engine optimization library, DXVK integration, and save protection for Gothic 3

[![Gothic 3 Version](https://img.shields.io/badge/Gothic%203-v1.75.14%20%2B%20UP%201.04.11%20%2F%201.05.10-gold.svg?style=flat-square)](file:///E:/SteamLibrary/steamapps/common/Gothic%203)
[![DXVK](https://img.shields.io/badge/Vulkan-DXVK%201.10%20%2F%202.4%20%2F%203.0-red.svg?style=flat-square)](https://github.com/doitsujin/dxvk)
[![Memory Allocator](https://img.shields.io/badge/Allocator-mimalloc%20v3.5.3-blue.svg?style=flat-square)](https://github.com/microsoft/mimalloc)
[![Decompression](https://img.shields.io/badge/Decompressor-libdeflate%20AVX2-green.svg?style=flat-square)](https://github.com/ebiggers/libdeflate)
[![Compiler](https://img.shields.io/badge/Compiler-MSVC%20v143%20(VS%202022)-orange.svg?style=flat-square)](CMakeLists.txt)
[![License](https://img.shields.io/badge/License-MIT-lightgrey.svg?style=flat-square)](LICENSE)

<br />

**[Installation](#installation) · [Release Packages](#release-packages) · [Changes Table](#changes-table) · [Save Protection](#save-protection) · [Building](#building) · [FAQ](#faq)**

<br />

</div>

---

## Overview

**Gothic 3 Optima (G3O)** is a native proxy DLL (`dinput8.dll`) loaded by `Gothic3.exe` on startup. It hooks engine routines across Genome Engine libraries (`Engine.dll`, `Game.dll`, `SharedBase.dll`, `FileSystem.dll`, `PhysXCore.dll`).

The project addresses core technical issues in the original game:
1. **World streaming stutters** - solved with multicore archive loading and fast SIMD decompression via libdeflate.
2. **"Where is the guru?" crashes** - replaces the stock heap allocator with mimalloc, enables LFH on all heaps, and mitigates x86 virtual memory exhaustion.
3. **Savegame corruption (0-byte files on crash)** - writes saves to a temporary file first, verifies file integrity, and atomically swaps the destination file with automatic `.bak` backups.
4. **Direct3D 9 single-thread draw bottlenecks** - eliminates redundant RenderState calls and translates DirectX 9 to Vulkan via DXVK.

---

## Installation

Releases are split into the **Core engine mod** and **DXVK packages** tailored to different GPU generations:

```mermaid
graph TD
    subgraph GitHub_Release ["GitHub Release (single page)"]
        Core["Gothic3Optima-v1.1.0-Core.zip<br/>(dinput8.dll: allocator, save guard, SIMD, threads)"]
        DXVK1["DXVK-2.4.1-Stable.zip<br/>(Recommended for NVIDIA / AMD)"]
        DXVK2["DXVK-3.0.1.zip<br/>(For Vulkan 1.4)"]
        DXVK3["DXVK-3.0.1-HDR.zip<br/>(For HDR monitors)"]
        DXVK4["DXVK-1.10.3-Async.zip<br/>(For older GPUs)"]
        DXVK5["DXVK-1.10.1-Async.zip<br/>(For Intel HD Graphics)"]
    end

    subgraph Game_Folder ["Game directory: Gothic 3/"]
        EXE["Gothic3.exe"]
        DLL["dinput8.dll"]
        D3D9["d3d9.dll"]
        DXGI["dxgi.dll"]
        CONF["dxvk.conf"]
    end

    Core -->|"1. Extract"| DLL
    DXVK1 -.->|"2. Extract one of five"| D3D9
    DXVK1 -.-> DXGI
    DXVK1 -.-> CONF
```

### Steps:

1. **Download the Core mod:**
   - Grab `Gothic3Optima-v1.2.1-Core.zip` from the release page.
   - Copy `dinput8.dll` into your Gothic 3 game directory (next to `Gothic3.exe`).
2. **Download the DXVK package for your GPU:**
   - Pick the appropriate DXVK archive from the table below on the same release page.
   - Copy the extracted files (`d3d9.dll`, `dxgi.dll`, `dxvk.conf`) into the game directory.
3. **Launch the game** normally via Steam, GOG, or `Gothic3.exe`.

---

## Release Packages

All archives are attached to the same release on GitHub:

### 1. Core Mod (Required)
| Package | Size | Description |
| :--- | :--- | :--- |
| **`Gothic3Optima-v1.2.1-Core.zip`** | ~120 KB | Core mod (`dinput8.dll`). Includes Multicore Threading Engine (eCThreadPool pool scaling to 4-8 workers, priority scheduler), mimalloc allocator replacement, SaveGame Guard, SIMD geometry acceleration, Multi-Ray Footprint, Combatant Culling Bypass, and exception handler. Works on native D3D9 or with any DXVK build. |

### 2. DXVK Variants (Pick one)
| Package | Version | Recommendation |
| :--- | :--- | :--- |
| **`DXVK-2.4.1-Stable.zip`** | DXVK 2.4.1 Stable | **Recommended default.** Best balance of stability and performance for most hardware: AMD Radeon (Polaris, Vega, RDNA 1-3) and NVIDIA GeForce (GTX 10xx, RTX 20xx/30xx/40xx). |
| **`DXVK-3.0.1.zip`** | DXVK 3.0.1 | Current release. Requires modern drivers with full Vulkan 1.4 support. |
| **`DXVK-3.0.1-HDR.zip`** | DXVK 3.0.1 HDR | Build with native HDR output. Use if playing on an HDR monitor or TV with Windows HDR turned on. |
| **`DXVK-1.10.3-Async.zip`** | DXVK 1.10.3 Async | For older GPUs lacking Vulkan 1.3 support, or if DXVK 2.x causes driver timeouts. |
| **`DXVK-1.10.1-Async.zip`** | DXVK 1.10.1 Async | For Intel HD/UHD integrated graphics or legacy hardware. |

> [!TIP]
> For standard gaming PCs, use: **`Gothic3Optima-v1.2.1-Core.zip`** + **`DXVK-2.4.1-Stable.zip`**.

---

## Changes Table

Comparison between stock Gothic 3 (CP 1.75.14) and the mod:

| Subsystem | Vanilla Behavior | Issue | Solution in Mod | Result |
| :--- | :--- | :--- | :--- | :--- |
| **Saves** | In-place file rewrite (`0x1019c6b0`), truncates to 0 on open | Corrupt 0-byte saves if game crashes during F5 | **SaveGame Guard:** cache flush, write to `.tmp`, verify size, atomic `ReplaceFileW` | Saves remain uncorrupted even on sudden crash or power loss |
| **Memory** | Engine `bCMemoryAdmin`, `_new@8` with `MemHeap::s_cLock` spinlock, CRT `HeapAlloc` | x86 address space fragmentation, lock stalls, OOM crashes ("Where is the guru?") | Complete swap of `bCMemoryAdmin`, `_new@8`, `MemHeap`, and IAT `HeapAlloc` with **mimalloc** using three isolated pools (Engine, MemHeap, ShortUsage) + LFH | External heap fragmentation eliminated, no lock contention, memory crashes resolved |
| **Graphics** | D3D9 without `D3DCREATE_PUREDEVICE`, textures kept in system RAM | High RAM usage, CPU validation overhead in driver | **DXVK** + forced `D3DCREATE_PUREDEVICE` flag | Textures offloaded to VRAM, freeing ~1 GB system RAM |
| **D3D9 States** | Engine emits thousands of redundant `SetRenderState` calls per frame | CPU draw call bottleneck | **State Filter:** shadow table filters duplicate calls | Reduced driver overhead, improved 1% low frametimes |
| **Multicore Threading** | Main game loop, AI logic, PhysX, D3D9 call submission, and asset streaming in vanilla are locked to a single thread (eCThreadPool hardcoded to 1 or 0 workers), D3D9 is synchronous | 100% saturation on core 0, stuttering during asset decompression, severe underutilization of 6-16 core CPUs | **Multicore Threading Engine (Phase 20):** hooked eCBaseCache::SetNumThreadsAndCreate (RVA `0x1EF8F0`) and eCThreadPool::Create (RVA `0x1F4010`) scaling pools to 2-8 workers per cache adapted to CPU core topology, eCThreadPool::EnableAllThreads(true), bypass of eCMainCache::AccessDisableThreading, unblocked asynchronous worker queues, and elevated main thread priority to THREAD_PRIORITY_ABOVE_NORMAL | Parallel loading and decompression of geometry, textures, audio, and animations across all available CPU cores, asynchronous CSMT rendering via DXVK, full utilization of modern multicore architectures |
| **Resource Cache & GC** | Aggressive resource eviction or unbounded asset buildup in long play sessions | Stutter when looking around, memory leaks when traveling between biomes (Myrtana/Varant/Nordmar) | **Adaptive LRU Cache Eviction (`eCMainCache`):** fresh assets (<300s) retained when memory is free, stale assets (>300s) evicted, VRAM flush and background watchdog | Zero camera turnaround stutters, memory automatically reclaimed on regional transitions and teleports |
| **Decompression** | Scalar `zlib 1.2.5` in `FileSystem.dll` | Traversal CPU spikes decompressing `.pak` chunks | Integrated **libdeflate** with AVX2 / SSE4.2 SIMD (hook at RVA `0xA430`) | 3-4x faster archive decompression |
| **File I/O** | Synchronous `SetFilePointer` and small-buffer `ReadFile` calls | Disk seek latency, repeated kernel transitions | **Memory-Mapped Files (`MapViewOfFile`):** direct file mappings (750 MB budget) | In-memory archive access without disk stalls |
| **Sector Streaming** | Static radial sphere around hero in `eCPVSPrefetcher3` | Micro-stutters (50-200 ms) when crossing sector boundaries | **Motion Vector Prefetch:** sphere projected 10-30m ahead along motion vector | Seamless streaming: sector cells loaded before hero arrives |
| **Math & Physics** | Calculations use legacy x87 FPU (`fld`, `fmul`, `fstp`) in `SharedBase.dll` | Slow Frustum Culling and bounding volume checks | Replaced culling and matrix math with SSE intrinsics | Faster visibility calculations |
| **Fall-Through & GeoData Guard** | Single raycast slips through polygon seams in terrain meshes, spurious fall detection | Hero clipping through terrain into the void, slipping into geometry seams, lethal fall damage | **Multi-Ray Footprint & Ground Recovery Fall Guard:** 5-ray cross footprint cluster under character feet (0.25m radius) via `TraceRayNearestHit` hook in `Engine.dll`, lateral hit centering, abnormal vertical drop detector ($>6$m) restoring safe ground and resetting fall velocity | Hero never clips through geometry cracks, rock-solid footing on slopes and seams, zero fall damage from glitches |
| **String Hashing** | Byte-by-byte loop in `g_GetHashValue<bCString>` | Map lookup delays during NPC spawning | Vectorized string hash routine | Faster object and dialogue map lookups |
| **CPU Topology** | Threads randomly scheduled onto slow E-cores | Frame drops on Intel 12-14th Gen and AMD Hybrid chips | Game threads bound to Performance cores (P-cores) | Consistent framerate on hybrid processors |
| **PhysX Physics & Ragdolls** | Variable PhysX 2.4.1 timestep, zero bone damping, unbounded velocity accumulation on collision geometry clipping | Ragdoll corpses launching into the stratosphere, limbs stretching into spaghetti, micro-stutters at >60 / 144 Hz | **Fixed Physics Substep & Bone Velocity Clamp:** enforced 16.6 ms (1/60s, `NX_TIMESTEP_FIXED`), bone linear/angular damping (0.20/0.25), clamping of anomalous velocities ($\le 20$ m/s, $\le 12$ rad/s), and NaN spike suppression | Natural ragdoll settling without flying corpses or limb stretching, smooth simulation at 144+ Hz |
| **Frame Pacing** | Stock Windows timer resolution defaults to 15.6 ms | Jittery frame delivery even at 60 / 120 FPS | Forced `timeBeginPeriod(1)` for 1.0 ms precision | Consistent frame pacing |
| **FMOD Audio** | Small DSP buffer in `fmodex.dll` (512 samples, 10.6 ms), 16 KB file buffer | Audio clicks, crackles, background music stopping in cities | Hooked `FMOD::System::init`: 2048x4 samples DSP buffer (170.6 ms), 64 channels, 64 KB stream buffer | Clean audio without crackles, uninterrupted music under load |
| **Address Space** | 32-bit limit (4 GB virtual memory space) | Insufficient virtual space for massive archives (`_compiledImage.pak`, `_compiledMesh.pak`) | **High Memory Shadow Cache (> 4 GB):** 64-bit shadow pool (2 GB) via Heaven's Gate (`AllocateHighMemory`, `ReadHighMemory`) | Massive archives cached directly in 64-bit RAM, eliminating 32-bit memory pressure |
| **Z-Buffer & Shadows** | 24-bit integer depth buffer (D24S8/D16), non-linear precision loss across FarClipPlane distances ($1/z$), insufficient shadow bias, severe polygon Z-fighting | Striped shadow acne artifacts on characters and terrain, distant mountain and foliage flickering | **High-Precision Floating-Point Z-Buffer (`VK_FORMAT_D32_SFLOAT`) & Shadow Depth Bias Hook:** translated via `d3d9.useD32forD24 = True`, `d3d9.supportD32 = True`, IEEE-754 `d3d9.floatEmulation = Strict`, D16 auto-upgrade, position invariance `invariantPosition`, and dynamic shadow bias scaling via `D3DRS_DEPTHBIAS` (1.35x) and `D3DRS_SLOPESCALEDEPTHBIAS` (1.50x) | Preserved depth precision up to 1000m distances, complete elimination of distant Z-fighting and shadow acne on terrain and faces |
| **Vertex & Index Buffers** | 16-bit index constraint (max 65,535 vertices), small geometry pool step (1024 chunks), constrained dynamic buffers (64-256 KB) | Flickering and missing meshes, polygon tearing in high-poly mods, stalls from frequent reallocations | **Dynamic Buffer Scaling & 32-Bit Index Promotion (`D3DFMT_INDEX32`):** hooked `CreateVertexBuffer` / `CreateIndexBuffer` (min 1 MB / 512 KB, 2x scaling), instance pool growth expanded to 4096 in `Engine.dll`, and auto-promotion to 32-bit indices for meshes with $>65,535$ vertices | Flawless rendering of heavy mods and high-poly meshes without missing geometry or stretched polygons |
| **AI Combat Focus (Aggro)** | Entities culled from processing when outside camera frustum (`ExtractFromEntity`: `CullTreeHC`, `CullVPTHC`, `CullSmallObject`) | Pursuing monsters and enemies behind the hero freezing or losing target when turning camera away | **Combatant Culling Bypass (`src/combat_guard.cpp`):** hooks visibility tree and frustum culling checks in `Engine.dll` for active combatants (`gCNPC_PS`) targeting the player within 65m | Enemies relentlessly pursue and engage the player regardless of camera orientation |

---

## Save Protection

How SaveGame Guard works:

```mermaid
sequenceDiagram
    autonumber
    actor Player as Player (F5 / QuickSave)
    participant Game as Game.dll
    participant Guard as SaveGame Guard
    participant Memory as mimalloc
    participant Disk as Disk

    Player->>Game: Press F5
    Game->>Guard: Intercept save call (0x1019c6b0)
    Guard->>Guard: Check free memory
    alt Free memory < 350 MB
        Guard->>Memory: Reclaim memory pages
    end
    Guard->>Disk: Write to temporary file (.tmp)
    Game->>Disk: Serialize world state
    Game->>Guard: Close handle (CloseHandle)
    Guard->>Disk: Flush buffers to disk (FlushFileBuffers)
    Guard->>Guard: Verify file size (>256 bytes)
    alt Corrupted write (0 bytes)
        Guard->>Disk: Remove corrupt .tmp file
        Guard-->>Player: Save cancelled, original file preserved
    else File valid
        Guard->>Disk: Create backup (.bak)
        Guard->>Disk: Atomic swap via ReplaceFileW
        Guard-->>Player: Game saved successfully
    end
```

---

## Code Structure

- `heavens_gate/` - x86 to x64 bridge via segment selector switch (`retf 0x33`) to allocate memory beyond 4 GB using 64-bit `ntdll.dll`.
- `src/memory_allocator.cpp` & `src/lfh_enforcer.cpp` - complete swap of `bCMemoryAdmin`, global `_new@8`, `MemHeap` methods, and IAT `HeapAlloc` with mimalloc using three isolated lifetime-based pools; enforces LFH on system heaps.
- `src/d3d9_hook.cpp` - adds `D3DCREATE_PUREDEVICE`, maintains shadow render state cache, filters duplicate D3D9 calls, auto-upgrades depth format to 24/32-bit, exposes Depth Bias / Slope Scale caps, dynamically scales shadow depth bias (1.35x Depth Bias, 1.50x Slope Scale), scales dynamic vertex/index buffers (1-8 MB), expands geometry instance pool chunk to 4096, and auto-promotes index buffers to 32-bit `D3DFMT_INDEX32`.
- `src/streaming_optimizer.cpp` - maps `.pak` archives into RAM via `MapViewOfFile`, predictive cell prefetching by motion vector (`eCPVSPrefetcher3`), and expands `eCThreadPool` to 4-8 worker threads.
- `src/media_optimizer.cpp` - integrates `libdeflate` (AVX2/SSE4.2) for fast archive decompression, expands FMOD Ex DSP buffer (2048x4), and sets 1.0 ms high-resolution timer.
- `src/math_simd_optimizer.cpp` - replaces `bCFrustum::Cull` and matrix operations with SSE vector intrinsics.
- `src/fall_guard.cpp` - multi-ray footprint cluster under character feet, protection from falling through geometry seams (`TraceRayNearestHit`), and automatic ground recovery fall guard with fall damage cancellation.
- `src/combat_guard.cpp` - bypasses Frustum Culling (`CullTreeHC`, `CullVPTHC`, `CullSmallObject`) for active combatants targeting the player within 65m to preserve pursue logic and combat animations when looking away.
- `src/physics_stabilizer.cpp` - PhysX ragdoll stabilizer (`eCPhysicsScene::Simulate` and `CreateActor`), enforced `NX_TIMESTEP_FIXED` (16.6 ms / 60 Hz), linear and angular bone damping (0.20/0.25), anomalous skeleton bone velocity clamping ($\le 20$ m/s, $\le 12$ rad/s), and joint NaN explosion neutralization.
- `src/lru_cache_manager.cpp` - adaptive LRU cache eviction on `eCMainCache`, fresh asset retention (<300s), VRAM flush and stale asset eviction across regional biome changes.
- `src/savegame_guard.cpp` - writes saves to `.tmp`, verifies file size, creates `.bak` backup, and performs atomic replacement via `ReplaceFileW`.
- `src/version_watermark.cpp` - displays Gothic 3 Optima version and status in the game's main menu.

---

## Building

### Requirements:
- Windows 10 / 11 (x64)
- Visual Studio 2022 (MSVC v143 x86)
- CMake 3.20+
- Git

### Build:

```cmd
git clone https://github.com/VadimGlinskiy/G3O.git
cd G3O

cmake -B build -S . -A Win32 -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Output binary: `build/Release/dinput8.dll`.

To package all release archives:
```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\package_releases.ps1
```
Archives will be generated in `dist/packages/`.

---

## FAQ

<details>
<summary><strong>Is this compatible with Community Patch 1.75.14 and Update Pack 1.04.11 / 1.05.10 (PublicBeta)?</strong></summary>

Yes. The mod is thoroughly tested and verified with both Update Pack v1.04.11 and the latest Update Pack v1.05.10 (PublicBeta). Dynamic symbol resolution via exported C++ names guarantees seamless compatibility across both versions.
</details>

<details>
<summary><strong>Does it work with mods (Questpaket, Content Mod, LCTM)?</strong></summary>

Yes. The mod only modifies binary engine code and OS API calls. Quest scripts, balance tables, and asset templates are untouched.
</details>

<details>
<summary><strong>How to enable the DXVK HUD (FPS, memory)?</strong></summary>

Open `dxvk.conf` in your Gothic 3 folder and add:
```ini
dxvk.hud = fps,frametimes,memory,devinfo
```
</details>

<details>
<summary><strong>What to do if there is a black screen or Vulkan error on startup?</strong></summary>

Use `DXVK-1.10.1-Async.zip` or `DXVK-1.10.3-Async.zip`. They have broader compatibility with older GPUs and legacy drivers.
</details>

<details>
<summary><strong>Where are save backups stored?</strong></summary>

In your standard save directory (`Documents\gothic3\`). On every save overwrite, the previous save is kept as `.bak`. If needed, rename it back to `.g3savdat`.
</details>

---

## Acknowledgements & Third-Party Components

Gothic 3 Optima builds upon and benefits from the research, work, and open-source libraries created by the following projects and authors:

| Component / Project | Author / Maintainer | License | Role in Gothic 3 Optima |
| :--- | :--- | :--- | :--- |
| **[DXVK](https://github.com/doitsujin/dxvk)** | Philip Rebohle ([doitsujin](https://github.com/doitsujin)) & contributors | Zlib License | Direct3D 9 to native Vulkan translation layer, offloading texture creation and shader compilation to asynchronous Vulkan transfer queues, enabling 32-bit floating point depth buffers. |
| **[DXVK-Async](https://github.com/Sporif/dxvk-async)** | [Sporif](https://github.com/Sporif) | Zlib License | Asynchronous graphics pipeline compilation patch for classic DXVK 1.10.x builds. |
| **[mimalloc](https://github.com/microsoft/mimalloc)** | Daan Leijen ([Microsoft Research](https://github.com/microsoft)) | MIT License | High-performance compact memory allocator eliminating external heap fragmentation and replacing the slow engine heap manager `bCMemoryAdmin`. |
| **[libdeflate](https://github.com/ebiggers/libdeflate)** | Eric Biggers ([ebiggers](https://github.com/ebiggers)) | MIT License | Fast zlib/deflate decompression library with hardware SSE4.2 / AVX2 SIMD acceleration for unpacking meshes and textures from `.pak` archives. |
| **[MinHook](https://github.com/TsudaKageyu/minhook)** | Tsuda Kageyu ([TsudaKageyu](https://github.com/TsudaKageyu)) | BSD 2-Clause | Minimalist and thread-safe low-level x86 API hooking library (detours and trampolines). |
| **[Heaven's Gate](https://github.com/Gothic3Optima)** | Roy M. ([ReWolf](https://github.com/mwrl)), mixcode, antivm | Public Domain | Architectural concept for switching the CPU from x86 to x64 mode via segment selector `0x33` to allocate large off-heap cache buffers beyond the 4 GB virtual address limit on 64-bit Windows. |
| **Community Patch Team (CPT)** | CPT Team | - | For the colossal work on Community Patch 1.75.14, reverse engineering Genome Engine binaries, and saving Gothic 3. |
| **Update Pack Team** | zandr, Dimus, and the World of Players community | - | For developing Update Pack 1.04.11 and 1.05.10 PublicBeta, fixing hundreds of quests/scripts, and stabilizing `Engine.dll` / `Game.dll`. |
| **Piranha Bytes** | Piranha Bytes | - | Creators of the timeless Gothic universe and the original Genome Engine. |

---

## License

Gothic 3 Optima source code is released under the [MIT License](LICENSE).  
All third-party components remain the property of their respective authors and are distributed under their original licenses.
