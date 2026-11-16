# Gothic 3 Optima (G3O)

[Русский](README.md)

**Gothic 3 Optima (G3O)** is an optimization mod and proxy library `dinput8.dll` for **Gothic 3** (v1.75.14). It resolves long-standing technical bottlenecks in the Genome Engine: significantly speeds up savegame loading, removes micro-stutters during world exploration, adds a true borderless fullscreen windowed mode, extends visual draw distance without the opaque wall of white fog, and protects the game from random "Where is the guru?" crashes.

### Compatibility
G3O is fully compatible with:
* Community Patch 1.75.14 and Update Pack;
* Parallel Universe Patch (PUP v1.1.1);
* Major mod packages (Union, Questpaket, Content Mod, LTM, Conquest, and large builds with 300+ mods).

---

## Configuration Guide (G3O.ini)

The configuration file `G3O.ini` is located in the game folder next to `Gothic3.exe`. All settings are grouped into logical sections. Below is an explanation of what each option does and why it is useful.

---

### [General] — Core Settings

* **`EnableG3O`** (default `true`)<br>
  Master switch. When set to `false`, the library acts as a transparent passthrough for DirectInput: all hooks, optimizations, and crash guards are disabled, and the game runs in its stock vanilla state.<br>
  *Usage:* Useful for quick troubleshooting if you suspect a conflict with another mod.

* **`LoadG3SEPlugins`** (default `true`)<br>
  Automatically loads Script Extender plugins from the `Data\G3SE\Plugins` folder.<br>
  *Usage:* Allows G3SE/G3SDK mods to run without requiring a third-party DLL injector.

* **`LoadReShade`** (default `true`)<br>
  Automatically loads the ReShade post-processing pipeline (`ReShade32.dll`) when present in the game directory.<br>
  *Usage:* Enables visual enhancement shaders without needing DLL renaming or external injectors.

* **`FixVulkanLayerConflicts`** (default `true`)<br>
  Disables conflicting third-party Vulkan overlay layers (such as Bandicam) that hook into the graphics pipeline and cause game crashes on startup when running with DXVK.

---

### [Display] — Window, Display & Input

* **`EnableBorderless`** (default `true`)<br>
  Borderless Fullscreen windowed mode.<br>
  *Vanilla issue:* In exclusive fullscreen, pressing `Alt + Tab` frequently causes Gothic 3 to freeze permanently, reset desktop resolution, or crash the GPU driver.<br>
  *G3O solution:* The game runs in a borderless window matching your desktop resolution. Switching between the game, browser, or Discord is instant and smooth.

* **`AutoBorderless`** (default `true`)<br>
  Automatically switches the game window into borderless fullscreen on startup if windowed mode is selected in the in-game video settings.

* **`PreventMinimizeOnFocusLoss`** (default `true`)<br>
  Prevents the game from minimizing when clicking outside or onto a secondary monitor.<br>
  *Usage:* Ideal for multi-monitor setups: clicking a browser, interactive map, or chat on your second screen won't minimize Gothic 3 — the game continues rendering and audio keeps playing.

* **`WindowCursorLock`** (default `true`)<br>
  Locks the mouse cursor inside the game window during active gameplay.<br>
  *Usage:* On multi-monitor setups, prevents the mouse pointer from straying onto another monitor during combat. The cursor is immediately and cleanly released upon `Alt + Tab` or switching focus.

* **`InputSystemKeyPassthrough`** (default `true`)<br>
  Passes multimedia and volume keys through to Windows.<br>
  *Vanilla issue:* Gothic 3 captures keyboard input exclusively and blocks keyboard media controls.<br>
  *G3O solution:* Lets you adjust Windows volume (`Volume Up/Down`, `Mute`) and control music players (`Play/Pause`, next/previous track) while playing.

---

### [CrashInvestigator] — Crash Protection & Diagnostics

* **`EnableCrashInvestigator`** (default `true`)<br>
  Global crash handler. Catches unhandled Win32 exceptions (SEH), engine `FatalError` crashes, and "Where is the guru?" dialogs, producing detailed diagnostic reports.

* **`EnableCrashGuards`** (default `true`)<br>
  Comprehensive engine crash guards against null-pointer exceptions and missing objects.<br>
  *Intercepted crash paths:*
  1. `DrawFocusName` (`Game.dll`): skips focus label rendering when targeting newly spawned Human/Orc NPCs without an assembled animation actor.
  2. `GetBoneMatrix` (`Engine.dll`): prevents Access Violation (`0x000001B4`) when calculating bone transforms during ragdoll physics or entity despawns with detached actor/skeleton hierarchies.
  3. `FindStack` / `FindStackIndex` (`Game.dll`): safely intercepts null item template entity lookups, preventing the fatal `CallFatalError` ("Where is the guru? - Template is invalid (null)").

* **`ShowCrashDialog`** (default `true`)<br>
  Shows an informative error dialog on crash displaying the offending module and crash address instead of closing silently.

* **`WriteCrashDump`** (default `true`)<br>
  Generates a process minidump in `G3O\G3O_Crash_*.dmp` for debugging in Visual Studio or WinDbg.

* **`WriteCrashLog`** (default `true`)<br>
  Saves a readable crash report in `G3O\G3O_CrashReport_*.log` containing CPU register states, loaded module addresses, and stack traces.

---

### [Performance] — Performance, Graphics & Loading

#### 1. Savegame Loading and Asset Streaming

* **`EnableFastPakDecompressor`** (default `true`)<br>
  Hardware-accelerated decompression for `.pak` archives and `.g3savcpx` savegames using libdeflate with AVX2 vector instructions. Decompresses data up to 4x faster than vanilla zlib.

* **`EnablePakPrefetch`** (default `true`), **`PakCacheBudgetMB`** (default `768`), **`PakPrefetchWorkers`** (default `0` — auto)<br>
  Background asset prefetching. Preheats the Windows file cache and decompresses frequently needed archive blocks into memory based on previous session traces. Eliminates long load hitches and streaming stutters.<br>
  *Memory limit:* 768 MB is a safe limit for a 32-bit process, preventing out-of-memory crashes. Setting `0` disables the cache.

* **`EnablePropertyCache`** (default `true`)<br>
  High-speed entity property cache. During save loading, the engine resolves properties hundreds of thousands of times. This cache stores resolved properties in a fast hash table, eliminating redundant lookups.

* **`EnableVFSCache`** (default `true`)<br>
  Virtual File System (VFS) path caching. Avoids repeated directory searches through long chains of archive files — especially valuable for large mod packages.

* **`EnableSessionStartOptimizer`** (default `true`), **`SessionStartSleepMs`** (default `1`)<br>
  Removes idle wait delays on world start.<br>
  *Vanilla issue:* The original engine ran twelve consecutive `Sleep(100)` calls during session start, causing 1.2 seconds of dead waiting time on every save load.<br>
  *G3O solution:* Reduces idle sleep calls from 100 ms to 1 ms, shaving over a second off every loading screen.

* **`EnableLayerRingArena`** (default `false`)<br>
  Circular arena buffer for massive world layers (`RB_Mod`). Recommended off (`false`) for standard gameplay.

* **`EnableParallelDynamicLayers`** (default `true`)<br>
  Safe background pre-fetching and decompression of dynamic world layers (`Projects_compiled.*`) into RAM. Background worker threads read and decompress independent mod layers (CNQ, LTM, AD_World, RB_Mod) in parallel via AVX2/BMI2 `libdeflate`, while the engine activates them on the main thread without I/O stalls or crash risks. Reduces world activation time by 5–10 seconds.

#### 2. Engine Memory, Scene Graph & Core Algorithms

* **`EnableSharedBaseOptimizations`** (default `true`)<br>
  Accelerates core math operations and string comparisons in `SharedBase.dll`. Uses fast CPU instructions for bounding box checks and name hashing, reducing CPU overhead in dense scenes.

* **`EnableFrustumDatabaseOptimizer`** (default `true`)<br>
  Optimizes visibility database updates during loading. Batches frequent intermediate frustum queries, significantly reducing save loading times.

* **`EnableSceneNodeOptimizer`** (default `true`)<br>
  Pre-allocates scene node child arrays in stepped blocks (1024 -> 8192 -> 32768) rather than the engine's incremental 25% growth, eliminating cascading reallocations in heavily populated cities.

* **`EnableHashProxyOptimizer`** (default `true`)<br>
  Fast entity template lookup by name. Caches templates in memory to avoid linear list traversals when spawning items, monsters, and NPCs.

* **`EnableClassFactoryOptimizer`** (default `true`)<br>
  Class factory caching. Caches reflection type descriptors and reuses string buffers, eliminating tens of thousands of memory allocations during sector deserialization.

* **`EnableEntitySearchOptimizer`** (default `true`)<br>
  Optimizes entity component lookups with fast-path early exit.

* **`EnableInfoManagerOptimizer`** (default `true`)<br>
  Accelerates dialogue resolution using a hash table, removing micro-stutters when approaching NPCs and opening dialogue screens.

* **`EnableInfoManagerParallelColdLoad`** (default `false`)<br>
  Multi-threaded parser for the initial ("cold") loading of `InfoManager` dialogs across CPU cores. Kept off by default for maximum stability, as G3O's built-in O(1) Shadow Table cache and fast RAM-replay already deliver instantaneous dialog loading (~3 ms) on subsequent loads.

* **`EnableNavigationOptimizer`** (default `true`)<br>
  Optimizes pathfinding lookups, cutting CPU overhead during navigation updates in gameplay and loading.

#### 3. Graphics, Draw Distance & Atmosphere

* **`EnableAtmosphericFogControl`** (default `true`), **`FogDistanceMultiplier`** (default `2.5`), **`FogEndMinDistance`** (default `250000.0`), **`PreserveUnderwaterFog`** (default `true`)<br>
  Direct3D 9 atmospheric fog control.<br>
  *Vanilla issue:* Stock weather triggers fog starting at 200–300 meters, completely burying the horizon in an opaque white wall at 600–800 meters. Even with maximum view distance in `ge3.ini`, distant islands and shorelines remain invisible.<br>
  *G3O solution:* Extends fog range by 2.5x (at least 2.5 km in clear weather). Looking out from coastal cliffs reveals panoramic views of mountains, dunes, and islands, while keeping a gentle, artistic haze at the horizon.<br>
  *Atmosphere preservation:* In heavy rain, sandstorms, caves, or underwater (`PreserveUnderwaterFog=true`), close fog is strictly preserved to maintain the game's intended mood.

* **`EnableSelectiveCulling`** (default `true`), **`ClutterCullThreshold`** (default `0.008`), **`ImportantCullThreshold`** (default `0.002`)<br>
  Selective screen-size culling.<br>
  *Vanilla issue:* The engine culls all objects using one uniform threshold. Increasing view distance causes the GPU to render thousands of tiny plates, spoons, and wood chips inside distant huts, tanking FPS.<br>
  *G3O solution:* Categorizes objects into important items (chests, herbs, enemies, NPCs, doors — rendered at long distances) and clutter (cutlery, debris — culled early). Yields **+15–25% FPS** in towns while keeping quest loot clearly visible.

* **`EnableVegetationOptimizer`** (default `true`)<br>
  Vegetation shadow culling. Skips shadow calculations for distant tree canopies that are not visually perceptible, providing a noticeable FPS boost outdoors.

* **`EnableAdaptiveTreeShadows`** (default `true`), **`TreeShadowDistance`** (default `6500.0`)<br>
  Height-adaptive tree shadows.<br>
  *Vanilla issue:* Tree shadows are abruptly clipped at a fixed 65 meters, causing an annoying sharp line of popping shadows to follow the player.<br>
  *G3O solution:* Dynamically scales shadow distance based on tree height: small bushes lose shadows earlier, while tall oaks and pines cast shadows farther out. The popping line disappears completely.

* **`VegetationGridRangeClamp`** (default `7500.0`)<br>
  Clamps the dense grass grid radius to 75 meters. Prevents grass vertex buffer overflows on ultra settings, avoiding sudden FPS drops in dense meadows.

* **`EnableShadowOptimizer`** (default `true`)<br>
  Directional sunlight shadow caster optimization. Replaces quadratic scans with an O(1) hash index, eliminating multi-second hitches during teleportation and streaming stutters in forests.

* **`EnableFastTextureLoader`** (default `true`)<br>
  Streamlined Direct3D 9 texture loading that avoids redundant intermediate buffer copies.

* **`EnableD3D9StateFilter`** (default `true`)<br>
  DirectX 9 state filtering. Discards duplicate render state calls before sending them to the GPU, saving CPU cycles.

#### 4. CPU Scheduling, Priority & System Timer

* **`EnableHighPrecisionTimer`** (default `true`)<br>
  Requests a 1 ms Windows system timer (`timeBeginPeriod(1)`), smoothing out frame pacing and eliminating camera jitter.

* **`EnableHighProcessPriority`** (default `true`)<br>
  Assigns high priority to `Gothic3.exe`, preventing background Windows applications from stealing CPU time.

* **`EnableCPUTopologyAffinity`** (default `true`), **`CPUCores`** (`0`), **`CPUThreads`** (`0`), **`CPUAffinityMask`** (`0x0`)<br>
  Pins the game to performance cores (P-cores).<br>
  *Vanilla issue:* On hybrid CPUs (Intel 12th gen and newer), Windows often incorrectly schedules the main Gothic 3 thread onto slow efficiency cores (E-cores), cutting performance in half.<br>
  *G3O solution:* Detects CPU topology and ensures threads run exclusively on high-performance cores.

---

### [Memory] — High-Performance Heap Allocator (mimalloc)

* **`AllocatorMode`** (default `0` — Vanilla)<br>
  Integrates Microsoft mimalloc, configured specifically for 32-bit Large Address Aware game execution (based on the proven architecture from Psycho Engine Fixes: 8MB private arenas, demand paging, reset-on-purge without decommit to eliminate virtual address space fragmentation).
  * `0` — Vanilla engine allocators (maximum compatibility).
  * `1` — `ShortUsage` / `ScrapHeap` replacement. Accelerates per-frame scratch strings and temporary buffers with zero risk of cross-module free conflicts.
  * `2` — Full `MemHeap` and `bCMemoryAdmin` replacement with `Safe Dispatch Free` (`mi_is_in_heap_region`). Eliminates memory fragmentation and prevents micro-stutters during marathon play sessions.

---

### [Logging] — Logs & Profiling

*All logging options are disabled by default during normal gameplay to save disk I/O.*

* **`EnableDiagnosticLog`** (default `false`)<br>
  Writes detailed subsystem startup and hook logs to `G3O\G3O_diagnostic_*.log`.

* **`InterceptOutputDebugString`** (default `false`)<br>
  Captures internal debug messages from the engine and mod DLLs into the diagnostic log.

* **`EnableProfiler`** (default `false`)<br>
  Per-second telemetry in `G3O\G3O_profile_*.log`: tracks average FPS, 1% Low, 0.1% Low, frame times, and CPU utilization.

* **`EnableSavegameAudit`** (default `false`)<br>
  Detailed millisecond breakdown of savegame loading times in `G3O\G3O_LoadProfiler_*.log` (disk reads, decompression, entity spawns, world activation).

* **`EnableLayerAudit`** (default `false`)<br>
  Micro-profiling of world layers and property reads for mod authors.

---

## File and Folder Structure

All diagnostic reports, logs, and dumps are cleanly stored in a dedicated `G3O\` directory:

```text
Gothic 3\
├── Gothic3.exe
├── dinput8.dll          (G3O proxy DLL)
├── G3O.ini              (configuration file)
└── G3O\
    ├── G3O_CrashReport_*.log   (crash reports)
    ├── G3O_Crash_*.dmp         (memory minidumps)
    ├── G3O_LoadProfiler_*.log  (savegame load audit logs)
    ├── G3O_profile_*.log       (performance & frame time logs)
    ├── G3O_diagnostic_*.log    (subsystem initialization logs)
    └── G3O_PakTrace_*.csv      (PAK trace cache for prefetcher)
```

---

## Installation & Updating

1. Copy `dinput8.dll` and `G3O.ini` into your Gothic 3 game directory (beside `Gothic3.exe`).
   > Back up any existing `dinput8.dll` first if it was installed by another mod.<br>
   > When updating G3O, transfer your personal settings from the old `G3O.ini` to the new one.
2. Adjust settings in `G3O.ini` if desired. The default shipped configuration provides maximum performance and stability out of the box.
3. Launch the game normally through Steam, GOG, or `Gothic3.exe`.

---

## DXVK Integration

DXVK translates Direct3D 9 calls to Vulkan, reducing GPU driver overhead and eliminating shader compilation stutter.

* **DXVK 1.10.1-async / 1.10.3-async** — Vulkan 1.1; features asynchronous background shader compilation without stutter.
* **DXVK 2.4.1** — Vulkan 1.3; stable upstream release with texture sharpness controls.
* **DXVK 3.0.1** — Vulkan 1.4; requires modern GPU drivers.

Place `dxvk.conf` next to `Gothic3.exe`. A complete breakdown of configuration options is available in the [DXVK Options Guide](docs/DXVK_OPTIONS.md) (Russian).

---

## Optional ReShade Visual Enhancement Pack

The release page includes an optional standalone addon archive **`ReShade-RTGI-Gothic3.zip`** featuring ReShade 6.8 and a curated shader suite (*Marty's Mods / iMMERSE* and *qUINT* by Pascal Gilcher).

### Why It Helps Gothic 3:
Vanilla Gothic 3 suffers from notable distant rendering artifacts, which become particularly visible when extended fog reveals distant landscapes:
* **Anti-Aliasing & Distant Pixel Stabilization (`MartysMods_SMAA`):**<br>
  Vanilla Gothic 3 lacks modern edge anti-aliasing. Tree canopies, fences, and mountain ridges in the distance crawl with sub-pixel noise and flickering during camera movement. SMAA stabilizes high-frequency geometric edges, eliminating shimmering.
* **Ambient Depth for Distant Terrain (`MartysMods_MXAO`):**<br>
  Dynamic shadows in Gothic 3 stop past 65 meters, leaving distant Nordmar peaks and Varant cliffs looking flat. MXAO reads the game's depth buffer (Z-Buffer) and reconstructs soft contact shadows in rocky crevasses, valleys, and under distant tree canopies, restoring depth and scale.
* **Elimination of Sky & Fog Banding (`qUINT_deband`):**<br>
  The vintage D3D9 8-bit color pipeline frequently produces visible color banding and stepped rings in the sky and distant fog. The deband filter smooths gradients into clean color transitions.
* **Soft Atmospheric Light Diffusion (`MartysMods_SOLARIS` / `qUINT_bloom`):**<br>
  Atmospheric diffusion naturally softens low-polygon LOD transitions on the horizon.
* **Real-Time Water Reflections (`qUINT_ssr`):**<br>
  Adds screen-space reflections of shorelines and objects across lakes, rivers, and the sea.
* **Adaptive Texture Sharpening (`MartysMods_SHARPEN`):**<br>
  Enhances surface textures without ringing or edge artifacts.

### Installation:
1. Download **`ReShade-RTGI-Gothic3.zip`** from the GitHub Releases page.
2. Extract all contents (`ReShade32.dll`, `ReShade.ini`, and the `reshade-shaders\` folder) directly into your Gothic 3 game directory (beside `Gothic3.exe`).
3. Launch the game. G3O will automatically detect and load `ReShade32.dll` on startup via `LoadReShade=true`.
4. Press the **`Home`** key on your keyboard to open the ReShade configuration overlay.
   > On compact or laptop keyboards without a dedicated `Home` key, use **`Fn + Home`** or **`Fn + Left Arrow`**. The key binding can also be customized in `ReShade.ini` (`OverlayKey`).

### Renderer Compatibility: Native DirectX 9 vs. DXVK

> [!IMPORTANT]
> **Compute Shaders and Native DirectX 9:**
> * Advanced effects from **Marty's Mods** (RTGI ray-traced global illumination, physical film grain, Solaris) require **Compute Shaders** (`cs_5_0`), which are **not supported by the native Direct3D 9 API**. Attempting to compile them on raw DirectX 9 can cause `D3DCompiler_47.dll` compiler faults.
> * **DXVK is required for full Marty's Mods / RTGI effects** (`DXVK-2.4.1-Stable.zip` or `DXVK-1.10.3-Async.zip`). DXVK translates Direct3D 9 calls to Vulkan with full Compute Shader support.
> * **Running on Native DirectX 9 (Without DXVK):** If you prefer playing without DXVK, use the **qUINT** suite (`qUINT_mxao`, `qUINT_bloom`, `qUINT_ssr`, `qUINT_dof`, `qUINT_lightroom`, `qUINT_sharp`, `qUINT_deband`) along with `MartysMods_SMAA`. They are 100% Direct3D 9 compliant, do not require compute shaders, and compile instantly on any GPU.

> [!NOTE]
> The ReShade graphics package is entirely optional. If you remove `ReShade32.dll` or set `LoadReShade=false`, the game and G3O continue operating normally.

---

## Building from Source

Requirements:
* Visual Studio 2022 (MSVC v143 x86 / 32-bit compiler)
* CMake 3.20+
* Git

Build commands:
```cmd
git submodule update --init --recursive
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
```

The compiled library will be output at: `build/Release/dinput8.dll`.

Running unit tests:
```cmd
python -c "import subprocess, glob; [subprocess.run([f], check=True) for f in glob.glob(r'build/tests/**/*_test.exe', recursive=True)]"
```

---

## License

This project is licensed under the [MIT License](LICENSE).
