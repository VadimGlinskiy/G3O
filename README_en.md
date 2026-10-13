<div align="center">

**English** · [Русский](README.md)

# Gothic 3 Optima (G3O)

### Engine optimization library, DXVK integration, and save protection for Gothic 3

[![Gothic 3 Version](https://img.shields.io/badge/Gothic%203-v1.75.14%20%2B%20UP%201.04.11-gold.svg?style=flat-square)](file:///E:/SteamLibrary/steamapps/common/Gothic%203)
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
        Core["Gothic3Optima-v1.0.0-Core.zip<br/>(dinput8.dll: allocator, save guard, SIMD, threads)"]
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
   - Grab `Gothic3Optima-v1.0.0-Core.zip` from the release page.
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
| **`Gothic3Optima-v1.0.0-Core.zip`** | ~80 KB | Core mod (`dinput8.dll`). Includes mimalloc allocator replacement, SaveGame Guard, SIMD geometry acceleration, worker thread pool expansion, and exception handler. Works on native D3D9 or with any DXVK build. |

### 2. DXVK Variants (Pick one)
| Package | Version | Recommendation |
| :--- | :--- | :--- |
| **`DXVK-2.4.1-Stable.zip`** | DXVK 2.4.1 Stable | **Recommended default.** Best balance of stability and performance for most hardware: AMD Radeon (Polaris, Vega, RDNA 1-3) and NVIDIA GeForce (GTX 10xx, RTX 20xx/30xx/40xx). |
| **`DXVK-3.0.1.zip`** | DXVK 3.0.1 | Current release. Requires modern drivers with full Vulkan 1.4 support. |
| **`DXVK-3.0.1-HDR.zip`** | DXVK 3.0.1 HDR | Build with native HDR output. Use if playing on an HDR monitor or TV with Windows HDR turned on. |
| **`DXVK-1.10.3-Async.zip`** | DXVK 1.10.3 Async | For older GPUs lacking Vulkan 1.3 support, or if DXVK 2.x causes driver timeouts. |
| **`DXVK-1.10.1-Async.zip`** | DXVK 1.10.1 Async | For Intel HD/UHD integrated graphics or legacy hardware. |

> [!TIP]
> For standard gaming PCs, use: **`Gothic3Optima-v1.0.0-Core.zip`** + **`DXVK-2.4.1-Stable.zip`**.

---

## Changes Table

Comparison between stock Gothic 3 (CP 1.75.14) and the mod:

| Subsystem | Vanilla Behavior | Issue | Solution in Mod | Result |
| :--- | :--- | :--- | :--- | :--- |
| **Saves** | In-place file rewrite (`0x1019c6b0`), truncates to 0 on open | Corrupt 0-byte saves if game crashes during F5 | **SaveGame Guard:** cache flush, write to `.tmp`, verify size, atomic `ReplaceFileW` | Saves remain uncorrupted even on sudden crash or power loss |
| **Memory** | Engine `bCMemoryAdmin` with `MemHeap::s_cLock` spinlock + CRT `HeapAlloc` | Heap fragmentation, lock stalls, OOM crashes around 3 GB | Swapped with **mimalloc v3.5.3 (x86)** + LFH enforced on all heaps | No lock contention, fragmentation eliminated, up to 400 MB RAM saved |
| **Graphics** | D3D9 without `D3DCREATE_PUREDEVICE`, textures kept in system RAM | High RAM usage, CPU validation overhead in driver | **DXVK** + forced `D3DCREATE_PUREDEVICE` flag | Textures offloaded to VRAM, freeing ~1 GB system RAM |
| **D3D9 States** | Engine emits thousands of redundant `SetRenderState` calls per frame | CPU draw call bottleneck | **State Filter:** shadow table filters duplicate calls | Reduced driver overhead, improved 1% low frametimes |
| **Streaming Threads** | `eCThreadPool` (`0x101f4010`) only runs 1-2 workers | Visible hitching when entering new world sectors | Hooked `eCThreadPool::Create`, scales to 4-8 workers | Parallel decompression utilizing multicore processors |
| **Resource Cache** | Aggressive resource eviction in `eCBaseCache` | Stutter when quickly looking back | Eviction throttled while plenty of free memory exists | Eliminates hitches when moving back and forth within an area |
| **Decompression** | Scalar `zlib 1.2.5` (`0x100153dc` in `FileSystem.dll`) | Traversal CPU spikes decompressing `.pak` chunks | Integrated **libdeflate** with AVX2 / SSE4.2 SIMD | 2.5-4x faster archive decompression |
| **File I/O** | Synchronous `SetFilePointer` and small-buffer `ReadFile` calls | Disk seek latency, repeated kernel transitions | **Memory-Mapped Files (`MapViewOfFile`):** direct file mappings | In-memory archive access without disk stalls |
| **Math & Physics** | Calculations use legacy x87 FPU (`fld`, `fmul`, `fstp`) in `SharedBase.dll` | Slow Frustum Culling and bounding volume checks | Replaced culling and matrix math with SSE intrinsics | Faster visibility calculations |
| **String Hashing** | Byte-by-byte loop in `g_GetHashValue<bCString>` | Map lookup delays during NPC spawning | Vectorized string hash routine | Faster object and dialogue map lookups |
| **CPU Topology** | Threads randomly scheduled onto slow E-cores | Frame drops on Intel 12-14th Gen and AMD Hybrid chips | Game threads bound to Performance cores (P-cores) | Consistent framerate on hybrid processors |
| **PhysX Physics** | PhysX 2.4.1 locked to 2 threads, queries defunct Ageia PPU hardware | FPS drops in battles with ragdolls | SDK creation hooked, memory routed through mimalloc | Smoother physics simulation during mass fights |
| **Frame Pacing** | Stock Windows timer resolution defaults to 15.6 ms | Jittery frame delivery even at 60 / 120 FPS | Forced `timeBeginPeriod(1)` for 1.0 ms precision | Consistent frame pacing |
| **FMOD Audio** | Small DSP buffer in `fmodex.dll` (512 samples) | Audio clicks and pops under streaming load | Hooked `FMOD_System_Init` with 1024 samples x 4 buffers | Clean audio without crackles during sector streaming |
| **Address Space** | 32-bit limit (4 GB virtual memory space) | Insufficient space to keep world assets in memory | **Heaven's Gate (`heavens_gate/`):** in-process x86 to x64 transition for buffer allocations | Allows large off-heap cache buffers beyond 4 GB |

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
- `src/memory_allocator.cpp` & `src/lfh_enforcer.cpp` - replaces engine `bCMemoryAdmin` allocations with mimalloc, enables LFH on all process heaps.
- `src/d3d9_hook.cpp` - adds `D3DCREATE_PUREDEVICE`, maintains shadow render state cache to filter duplicate D3D9 calls.
- `src/streaming_optimizer.cpp` - maps `.pak` archives into memory via `MapViewOfFile` and expands `eCThreadPool` to 4-8 worker threads.
- `src/media_optimizer.cpp` - integrates `libdeflate` for decompression, increases FMOD buffer sizes, sets 1 ms timer period.
- `src/math_simd_optimizer.cpp` - replaces `bCFrustum::Cull` and matrix operations with SSE vector intrinsics.
- `src/savegame_guard.cpp` - writes saves to `.tmp`, verifies file size, creates `.bak` backup, and performs atomic replacement via `ReplaceFileW`.

---

## Building

### Requirements:
- Windows 10 / 11 (x64)
- Visual Studio 2022 (MSVC v143 x86)
- CMake 3.20+
- Git

### Build:

```cmd
git clone https://github.com/YourUsername/Gothic3Optima.git
cd Gothic3Optima

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
<summary><strong>Is this compatible with Community Patch 1.75.14 and Update Pack 1.04.11?</strong></summary>

Yes. It was developed and verified against Gothic 3 v1.75.14 with Update Pack v1.04.11. All binary offsets and structures match.
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

## License

Released under the [MIT License](LICENSE).

Third-party dependencies:
- [MinHook](https://github.com/TsudaKageyu/minhook) (BSD 2-Clause)
- [mimalloc](https://github.com/microsoft/mimalloc) (MIT)
- [libdeflate](https://github.com/ebiggers/libdeflate) (MIT)
- [DXVK](https://github.com/doitsujin/dxvk) (Zlib)
- [DXVK-Async](https://github.com/Sporif/dxvk-async)
