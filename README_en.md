# Gothic 3 Optima (G3O)

[Русский](README.md)

G3O currently builds a DirectInput `dinput8.dll` proxy. Only `src/dinput8_proxy.cpp`, `src/main.cpp`, and `src/version_watermark.cpp` are included: they attempt to show a menu version label and load G3SE plugins if `G3SDK.dll`/`G3SE.dll` is available. Other `src/*.cpp` files are excluded; `gothic3sdk` is not linked. G3O does not duplicate Parallel Universe Patch (PUP) engine tweaks. This G3O build has not been tested in-game with PUP.

## DXVK and GPUs

DXVK is a separate Direct3D 9-to-Vulkan layer. It does not add textures, shadows, or HDR; set textures, shadows, draw distance and anti-aliasing in the game. Every DXVK archive contains `d3d9.dll`, `dxgi.dll`, and `dxvk.conf`.

| G3O package | Difference |
| --- | --- |
| `1.10.1-async` | Older version for older drivers; async is experimental. |
| `1.10.3-async` | Later 1.x version; the user reports it works with `samplerAnisotropy = -1`. |
| `2.4.1` | Requires Vulkan 1.3; adds compilation and texture-sharpness options. |
| `3.0.1` | Requires Vulkan 1.4 and the necessary driver features; some old options are gone. |
| `3.0.1-hdr` | Legacy archive name: its DLLs match `3.0.1`; Gothic 3 does not gain HDR. |

| GPU | Choice |
| --- | --- |
| AMD RX 570 8 GB on this PC | The log reports Vulkan 1.3.260: keep the working `1.10.3-async` or compare it separately with `2.4.1`. This driver does not meet `3.0.1`'s Vulkan requirement. |
| NVIDIA | Choose by the DXVK log's Vulkan version: older drivers — 1.x, Vulkan 1.3 — 2.4.1, Vulkan 1.4 — 3.0.1. |
| Intel | The same Vulkan rule applies; the vendor name alone does not call for a special config. |

On the RX 570, forcing `d3d9.samplerAnisotropy = 16` caused red noise. `-1` or omitting the line leaves filtering to the game and cleared the noise (user-reported). Back up a working config and change one option at a time.

**Do not install the old `dist/packages/DXVK-1.10.3-Async.zip`:** another process has locked it, and it still contains `samplerAnisotropy = 16`. The verified archive with the safe config is `DXVK-1.10.3-Async-safe.zip`.

For the highest image quality, start with maximum in-game settings and compare `2.4.1` with the current version. On `2.4.1`, try `d3d9.samplerLodBias = -0.25` separately for sharper textures; remove it if it shimmers. Forced filtering, anti-aliasing, and HDR are not part of the safe preset.

## `dxvk.conf` options

Place the file next to `Gothic3.exe`. G3O packages use DXVK defaults; only `-async` variants contain `dxvk.enableAsync = true`. This table covers options previously used in G3O and options that can change Gothic 3's image. **1** = 1.10.1/1.10.3, **2** = 2.4.1, **3** = 3.0.1. Omit a key to use the DXVK default.

| Key | Versions | Meaning |
| --- | --- | --- |
| `d3d9.samplerAnisotropy` | 1/2/3 | Forces filtering on all textures; `-1` lets the game choose, `16` broke the image here. |
| `d3d9.seamlessCubes` | 1.10.3/2/3 | Cube-map seams; change only if seams are visible. |
| `d3d9.samplerLodBias` | 2/3 | Negative values sharpen textures but add shimmer. |
| `d3d9.clampNegativeLodBias` | 2/3 | Prevents excessive texture sharpening; may reduce shimmer. |
| `d3d9.forceSwapchainMSAA` | 1/2 | Forced anti-aliasing; may break post-processing; removed in 3. |
| `d3d9.forceSampleRateShading` | 2/3 | Per-sample MSAA shading; expensive and may produce artifacts. |
| `dxvk.enableAsync` | async 1.x forks only | Non-blocking compilation; no image-quality gain. |
| `dxvk.numCompilerThreads` | 1/2/3 | Compiler threads; `0` selects automatically. |
| `d3d9.maxFrameLatency` | 1/2/3 | Frame latency, not image quality. |
| `d3d9.deferSurfaceCreation` | 1/2/3 | Workaround for a black window in some games. |
| `d3d9.memoryTrackTest` | 1/2/3 | Memory-tracking test; does not add VRAM. |
| `d3d9.tearFree` / `dxvk.tearFree` | 1 / 2,3 | Presentation: tearing and smoothness, not detail. |
| `d3d9.supportD32` | 1 | Enables the D32 depth format; usually already enabled. |
| `d3d9.useD32forD24` | 2/3 | Forces a depth-format substitution; not in 1.x, usually unnecessary on AMD. |
| `d3d9.invariantPosition` | 1/2 | Reduces flicker between overlapping surfaces; removed in 3. |
| `d3d9.floatEmulation` | 1/2/3 | Arithmetic accuracy; `Strict` may be slower. |
| `dxvk.enableGraphicsPipelineLibrary` | 2/3 | Pipeline compilation; leave at the default `Auto`. |
| `dxvk.trackPipelineLifetime` | 2/3 | Pipeline cleanup in a 32-bit game; no image-quality gain. |
| `dxgi.enableHDR` | 2/3 | HDR for DXGI, not for D3D9-based Gothic 3. |
| `dxvk.logLevel` | none | Invalid old key; use the `DXVK_LOG_LEVEL` environment variable. |
| `dxvk.trackPipelines` | none | Invalid old G3O key; not `trackPipelineLifetime`. |
| `dxvk.enableHDR` | none | Invalid old G3O key; does not enable HDR. |
| `d3d9.enableHDR` | none | Invalid old G3O key; does not enable HDR. |

Complete option lists: [1.10.1](https://github.com/doitsujin/dxvk/blob/v1.10.1/dxvk.conf), [1.10.3](https://github.com/doitsujin/dxvk/blob/v1.10.3/dxvk.conf), [2.4.1](https://github.com/doitsujin/dxvk/blob/v2.4.1/dxvk.conf), [3.0.1](https://github.com/doitsujin/dxvk/blob/v3.0.1/dxvk.conf). [Driver requirements](https://github.com/doitsujin/dxvk/wiki/Driver-support); [async fork](https://github.com/Sporif/dxvk-async).

## Build

```cmd
git submodule update --init third_party/minhook
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
```

DLL: `build/Release/dinput8.dll`; archives: `scripts/package_releases.ps1`. Identify and back up any existing game-folder `dinput8.dll` before replacing it: another mod may own it. Editing this repository does not change the installed game. Source license: [MIT](LICENSE).
