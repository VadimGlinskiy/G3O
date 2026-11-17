# Gothic 3 Optima (G3O) — Savegame Load Optimization Dossier
> **Target Audience:** AI Engineering Models (Claude 3.5 Sonnet, GPT-4o, o1, etc.) and Systems Reverse Engineers.  
> **Purpose:** Comprehensive technical context, architecture breakdown, memory limits, disassembly, exported symbols, telemetry benchmarks, and constraints to design high-performance C++20 hooks for Gothic 3.

---

## 1. Executive Summary & Problem Statement

**Gothic 3** is a 2006 open-world action RPG running on the proprietary **Genome Engine** (developed by Piranha Bytes). The game is a 32-bit x86 Windows PE executable (`Gothic3.exe`), patched with the official **Community Patch 1.75.14** and running heavy mod packs (Update Pack 1.05, QuestPaket, Content Mod, Return of the General Lee / RB_Mod, Lively Town Mod / LTM, Conquest / CNQ).

**The Hooking Subsystem:**  
**Gothic 3 Optima (G3O)** is an open-source performance and stability modification written in modern C++20. It compiles as `dinput8.dll` (a direct DirectInput8 proxy hook loaded automatically by the game), utilizing **MinHook 1.3.3**, **mimalloc 3.5.3**, and **libdeflate** (with runtime AVX2/BMI2 dynamic CPU dispatch).

### The Core Problem:
On modern high-end hardware (e.g., AMD Ryzen 9 / Intel Core i7/i9 with 16–24 hardware threads, 32 GB RAM, and high-speed NVMe SSDs achieving >1,000 MB/s), **loading a saved game takes ~31.0 seconds**.

### Where Do the Seconds Go? (Real Telemetry Profiler Breakdown)
A full-precision hardware timer (QPC) telemetry log (`G3O_LoadProfiler_*.log`) reveals the following distribution:

| Subsystem / Phase | Duration | % Total | Underlying Root Cause |
| :--- | :---: | :---: | :--- |
| **1. PAK Decompression inside `GotoLoadedPos`** | **9.672 s** | **31.2%** | **Single-threaded** decompression of 3,373 blocks (~1,734 MB compressed $\to$ 2,930 MB uncompressed) on the main thread via `libdeflate`. 19+ CPU cores idle. |
| **2. Dynamic Layer Parsing (`SysDyn`)** | **6.014 s** | **19.4%** | **Single-threaded** virtual stream deserialization of hundreds of thousands of property objects in the global dynamic state layer (`SysDyn`). |
| **3. Scene & Entity Setup (`GotoLoadedPos`)** | **6.207 s** | **20.0%** | Main-thread creation of Direct3D 9 Vertex/Index Buffers, textures, PhysX 2.x scene actors, and collision meshes around the player. |
| **4. Other Dynamic Layers (344 layers)** | **2.602 s** | **8.4%** | Deserialization of mod dynamic layers (`RB_Mod`, `CNQ`, `LTM`, `AD_WorldContent`, etc.). |
| **5. Savegame File Deserialization (`LoadGameWorld`)** | **2.081 s** | **6.7%** | Savegame chunk decompression (61.9 MB uncompressed) and state mapping. |
| **6. Disk I/O (`ReadFile`)** | **1.787 s** | **5.8%** | 45,216 calls reading 1,931.85 MB at **1,081.1 MB/s**. **The NVMe SSD is NOT the bottleneck!** |
| **7. World Activation Overhead & Misc** | **1.511 s** | **4.9%** | GUI entity clearing, sector root node expansions, PVS cell processing. |
| **TOTAL LOAD TIME** | **30.974 s** | **100%** | **~80% of total loading time is spent on 3 strictly single-threaded CPU bottlenecks!** |

---

## 2. Hard Constraints & Safety Invariants (The "Graveyard of Disasters")

Any proposed solution **MUST** strictly respect these rules. Violating any of them breaks the game:

1. **The 32-Bit Virtual Address Space (VAS) Limit (CRITICAL):**
   - Gothic 3 is a 32-bit application compiled with `/LARGEADDRESSAWARE`. It has at most **3.5 GB of usable Virtual Address Space** before `VirtualAlloc` / `malloc` returns `nullptr` and the engine triggers an unrecoverable `Out of Memory` Guru crash.
   - The uncompressed size of the 3,373 PAK blocks in `GotoLoadedPosition` is **2,929.99 MB (~2.93 GB)**!
   - **RULE:** You **CANNOT** decompress all blocks into RAM simultaneously. Any prefetching or caching scheme **MUST** use a bounded sliding window / ring buffer capped at **$\le$ 96–128 MB RAM**.
2. **Ground Clamping & `gCNavigationAdmin::Process()`:**
   - In Gothic 3, `gCNavigationAdmin::Process()` is directly responsible for raycasting entities to the terrain and snapping them to the NavMesh (`NavMesh clamping`).
   - In a previous build, skipping navigation ticks during save loading (`if (IsSavegameLoadActive()) return;`) or rate-limiting them caused NPCs and monsters to spawn under the terrain and instantly snap upwards ("creature vertical teleportation").
   - **RULE:** **DO NOT** hook, drop, or throttle `gCNavigationAdmin::Process()`.
3. **Stream Position & Deserialization Integrity:**
   - In an experimental build, attempting to pre-read 5 bytes of header (ClassID, Flags, Version) in `FastPropertyFactoryReadNewObject` caused:
     `Module: bTPropertyObject<CLASS, BASECLASS>::ReadData - illegal propertyobject version detected. Can't read version ! Filebounds exceeded ?`
   - The engine's reflection reader expects the stream cursor (`bCIStream`) to remain untouched.
   - **RULE:** **DO NOT** advance, alter, or pre-read bytes from `bCIStream` during property object deserialization.
4. **No Negative Caching Across Sectors:**
   - Gothic 3 loads entities across sector boundaries lazily. Storing `nullptr` (negative cache) in entity template maps causes unresolved template pointers, resulting in disappearing quest NPCs and broken quest logic.
   - **RULE:** Only cache positive lookups or use strict epoch invalidation.
5. **No Dialogue State Short-Circuiting (`gCInfoManager`):**
   - Bypassing or caching dialogue conditions breaks NPC dialogues and removes merchant trading lines ("Show me your wares").
   - **RULE:** `gCInfoManager` dialogue evaluation must run via vanilla execution.

---

## 3. Deep Dive — Bottleneck 1: Single-Threaded PAK Decompression (~10 s)

### 3.1 Mechanics & Architecture
All game assets (meshes, textures, animations, physics, materials, compiled scripts) are packaged inside PAK archives in the `Data/` directory:
- Files: `_compiledImage.pak`, `_compiledImage.m00`..`.m41` (~6 GB textures)
- `_compiledMesh.pak`, `_compiledMesh.mod`, `_compiledMesh.m01`..`.m20` (~700 MB meshes)
- `_compiledAnimation.pak`, `_compiledAnimation.mod` (~300 MB skeletal animations)
- `_compiledPhysic.pak`, `_compiledPhysic.mod` (~150 MB collision geometry)

**Archive Format (`G3V0`):**
- 32-byte header: `[4 bytes signature: G3V0] [offsets to FAT table]`.
- FAT table is located at the end of the archive, mapping virtual paths (e.g., `_compiledImage/G3_World_01.ximg`) to `(fileOffset, compressedSize, uncompressedSize, flags)`.
- Flags `0x00020020` indicates zlib/deflate compression.

**The Engine Decompressor Hook:**
In `FileSystem.dll`, the virtual file system uses an exported function with signature:
```cpp
// FileSystem.dll target RVA 0xA2D0:
// 55 8B EC 8B 45 10 8B 55 0C 56 8D 34 02 83 F8 06
typedef int (__stdcall* ZlibDecompress_t)(
    void* context,
    const void* inBuffer,
    DWORD inSize,
    void* outBuffer,
    DWORD outCapacity,
    DWORD* pOutWritten
);
```

### 3.2 Why it takes 10 seconds:
During `gCSession::GotoLoadedPosition(void* pEntity, bool)`:
1. The engine calculates the player's coordinate in the world and gathers the Potentially Visible Set (PVS) / Radius of Interest (ROI).
2. For every entity in the loaded sectors, Genome opens its `.xshm` (mesh), `.ximg` (texture), `.xact` (animation), and `.xcom` (collision) virtual files.
3. Every virtual file read synchronously invokes `ZlibDecompress` on the **MAIN THREAD**.
4. 3,373 blocks are decompressed sequentially:
   - Compressed bytes read: **1,734.35 MB**
   - Uncompressed bytes generated: **2,929.99 MB**
   - Even with `libdeflate` AVX2 running at ~300 MB/s per core, $2,930 \text{ MB} / 300 \text{ MB/s} \approx 9.7 \text{ seconds}$.
   - Meanwhile, 19 other CPU cores sit at 0% utilization.

### 3.3 Proposed Architectural Direction for AI:
- **Bounded Sliding-Window Pre-Decompression Pool:**
  Since holding 2.93 GB in RAM is impossible in 32-bit VAS, design a producer-consumer ring buffer:
  - Memory budget: Fixed at **64 MB to 96 MB** (e.g., 512 slots of up to 2 MB or chunked arena).
  - How to know what to decompress ahead of time?
    - *Option A (Trace-Assisted Sliding Window):* Record the block offsets requested during load in memory. On subsequent loads or quickloads of that session, worker threads look ahead 30–50 blocks in the sequence and decompress into the ring buffer.
    - *Option B (FAT Sector Pre-Scanning):* When `GotoLoadedPosition` starts, read the sector's `.node` or `.lrentdat` dependencies and queue their assets into a thread pool (`std::jthread` or Win32 ThreadPool).
  - Main thread hook: In `Hooked_ZlibDecompress`, check if `(inBuffer, inSize)` or `(archive, offset)` is already decompressed in the ring buffer. If ready $\to$ `memcpy` into `outBuffer` in 0.05 ms and free the slot!

---

## 4. Deep Dive — Bottleneck 2: `SysDyn` Dynamic Layer Property Parsing (~6 s)

### 4.1 Mechanics & Architecture
World activation happens in `gCWorld::Activate` (10.08 s total).
The engine deserializes two types of sector layers:
1. Static Geometry layers (`.nod`, `.node`)
2. Dynamic layers (`.lrentdat`) via `gCDynamicLayer::DoLoadData(bCIStream&)`:
   - Exported in `Game.dll`: `?DoLoadData@gCDynamicLayer@@MAE?AW4eEResult@eCProcessibleElement@@AAVbCIStream@@@Z` (RVA `0xC5A50`).
   - 345 dynamic layers are loaded.
   - **344 mod/world dynamic layers take 2.602 s total.**
   - **`SysDyn_{9A103CC2-4190-4DB3-9618-0419E5445AAD}` takes 6,014.22 ms alone (70% of dynamic layer activation)!**

### 4.2 What is `SysDyn`?
`SysDyn` is the global system dynamic layer. It stores the modified runtime state of every quest, dialog, NPC attribute, inventory change, and script variable in the entire game world. In heavily modded setups, `SysDyn` contains over 100,000 serialized property objects.

### 4.3 Why is it so slow?
In `SharedBase.dll`, property deserialization is handled by `bCPropertyObjectFactory` and `bTPropertyObject`:
- Address `0x10225f40` (and `0x1020DD50`):
```asm
0x10225f62: mov ecx, edi            ; edi = bCIStream*
0x10225f64: call ebx                ; call bCIStream::Read(4 bytes)
0x10225f6b: mov ecx, esi            ; esi = property object
0x10225f6d: call 0x10224f50         ; set property value
0x10225f77: mov ecx, edi
0x10225f79: call ebx                ; call bCIStream::Read(4 bytes)
...
```
- Every property of every object is read through individual virtual function calls on `bCIStream` / `bCMemoryStream`.
- Hundreds of thousands of allocations occur through Genome's custom memory administrator:
  `SharedBase.dll!?GetInstance@bCMemoryAdmin@@SGAAV1@XZ`
  `SharedBase.dll!?Realloc@bCMemoryAdmin@@QAEPAXPAXK@Z`
- The entire process is strictly single-threaded on the main thread.

### 4.4 Proposed Architectural Direction for AI:
- **Parallel Dynamic Layer Loading:**
  Can the other 344 dynamic layers be parsed in parallel while the main thread processes `SysDyn`?
  - Dynamic layers are attached to individual `gCSector` objects:
    `Game.dll!?AccessDynamicLayer@gCSector@@QAEPAVgCDynamicLayer@@ABVbCString@@@Z` (0x192ff0).
  - Are dynamic layers independent data structures until they are linked into the scene graph?
- **Stream In-Memory Buffer Pre-Allocation:**
  During `DoLoadData`, `bCMemoryStream` reallocates dynamically. Pre-allocating capacity eliminates thousands of internal `bCMemoryAdmin::Realloc` calls.
- **Bypassing `bCMemoryAdmin` Internal Heap Lock:**
  Redirecting property object allocations directly to `mimalloc` thread-local heaps.

---

## 5. Deep Dive — Bottleneck 3: D3D9 Buffers & PhysX Physics Setup (~6 s)

### 5.1 Mechanics & Architecture
Inside `gCSession::GotoLoadedPosition`, after blocks are decompressed, **6.207 seconds** are spent in Scene & Entity Setup:
1. **Direct3D 9 Allocations (`Engine.dll!eCDX9Device`):**
   - `?CreateTexture@eCDX9Device@@UAE?AW4bEResult@@ABUeSGfxLayersDesc@eCGfxShared@@PAPAVeCAPITexture@@@Z` (0x9B810)
   - `?CreateTextureFromMemory@eCDX9Device@@UAE?AW4bEResult@@AAUeSGfxLayersDesc@eCGfxShared@@PAXIPAPAVeCAPITexture@@@Z` (0x9B930)
   - `CreateVertexBuffer` and `CreateIndexBuffer` for thousands of static and dynamic meshes.
   - When running on DXVK, these calls translate to Vulkan staging buffers and memory allocations.
2. **PhysX 2.x Scene & Collision Registration (`Engine.dll`):**
   - `?CreatePhysicObject@eCEntity@@QAEXXZ` (0x225750)
   - `?AddPhysicObject@eCPhysicsScene@@IAEXPAVeCPhysicObject@@@Z` (0x171400)
   - Genome registers static collision hulls (`.xshm`) and actor collision shapes with `NxScene` for every entity within the load radius.
   - In PhysX 2.x, mesh cooking (`NxCookingInterface`) and inserting thousands of collision shapes into the spatial hash table is CPU-intensive and executes entirely on the main thread.
3. **Scene Node Attachment:**
   - Entities are added to the scene hierarchy via `Engine.dll!?AttachChild@eCNode@@UAEHPAV1@@Z` (0x20DDF0).
   - *(Note: G3O already optimizes `eCNode::AttachChild` by exponentially preallocating node capacity from 64 to 1024 $\to$ 8192 $\to$ 32768, saving ~26 reallocations per sector).*

### 5.2 Proposed Architectural Direction for AI:
- **PhysX Mesh Cooking Cache / Parallelization:**
  Can collision meshes be cooked asynchronously on worker threads before `GotoLoadedPosition` reaches them, or can cooked collision data be cached in memory?
- **D3D9 Vertex/Index Buffer Staging:**
  Can vertex/index raw data parsing be separated from GPU buffer creation, preparing the raw memory arrays in parallel so the main thread only does fast single-batch GPU creation?

---

## 6. Key Module Exports & Memory Map

### 6.1 `Game.dll`
- `0x173ab0`: `?GotoLoadedPosition@gCSession@@QAEXPAVeCEntity@@_N@Z`
- `0x0c5a50`: `?DoLoadData@gCDynamicLayer@@MAE?AW4eEResult@eCProcessibleElement@@AAVbCIStream@@@Z`
- `0x192ff0`: `?AccessDynamicLayer@gCSector@@QAEPAVgCDynamicLayer@@ABVbCString@@@Z`
- `0x0c54f0`: `??0gCDynamicLayer@@QAE@XZ`
- `0x172e90`: `??0gCSession@@QAE@XZ`
- `0x193c40`: `?DoLoadData@gCSector@@MAE?AW4eEResult@eCProcessibleElement@@AAVbCIStream@@@Z`
- `0x19e8b0`: `?DoLoadData@gCWorld@@MAE?AW4eEResult@eCProcessibleElement@@AAVbCIStream@@@Z`

### 6.2 `Engine.dll`
- `0x20ddf0`: `?AttachChild@eCNode@@UAEHPAV1@@Z`
- `0x225750`: `?CreatePhysicObject@eCEntity@@QAEXXZ`
- `0x171400`: `?AddPhysicObject@eCPhysicsScene@@IAEXPAVeCPhysicObject@@@Z`
- `0x170e70`: `??0eCPhysicsScene@@QAE@XZ`
- `0x2320c0`: `??0eCSceneAdmin@@QAE@XZ`
- `0x09b810`: `?CreateTexture@eCDX9Device@@UAE?AW4bEResult@@ABUeSGfxLayersDesc@eCGfxShared@@PAPAVeCAPITexture@@@Z`
- `0x09b930`: `?CreateTextureFromMemory@eCDX9Device@@UAE?AW4bEResult@@AAUeSGfxLayersDesc@eCGfxShared@@PAXIPAPAVeCAPITexture@@@Z`

### 6.3 `SharedBase.dll`
- `0x00c8e0`: `?GetInstance@bCMemoryAdmin@@SGAAV1@XZ`
- `0x00c9e0`: `?Realloc@bCMemoryAdmin@@QAEPAXPAXK@Z`
- `0x00bee0`: `?AccessBuffer@bCMemoryStream@@QAEPAEK@Z`
- `0x059330`: `?ReadNewObject@bCPropertyObjectFactory@@IAEPAVbCPropertyObjectBase@@AAVbCIStream@@@Z`
- `0x05be50`: `?ReadNewObject@bCPropertyObjectSingleton@@QAEPAVbCPropertyObjectBase@@AAVbCIStream@@@Z`

### 6.4 `FileSystem.dll`
- RVA `0x00A2D0`: `ZlibDecompress`
  - Signature: `55 8B EC 8B 45 10 8B 55 0C 56 8D 34 02 83 F8 06`

---

## 7. Challenge for AI Models

Given the exact architecture, memory constraints, and signatures above, provide:
1. **Concrete Architecture & Design:** How to safely eliminate the 9.67 s single-threaded PAK decompression bottleneck without exceeding the 96 MB RAM budget in 32-bit x86?
2. **Concrete C++20 / MinHook Implementation:** Exact hook functions, thread synchronization (SRWLOCK / std::atomic / Condition Variables), memory arenas, and fallback mechanisms.
3. **Safety Analysis:** Proof that the design will not cause Guru crashes, corrupt property stream cursors, break merchant dialogues, or cause vertical creature popping.
