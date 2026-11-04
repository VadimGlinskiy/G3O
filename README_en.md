# Gothic 3 Optima (G3O)

[Русский](README.md)

G3O currently builds a `dinput8.dll` proxy. Only `src/dinput8_proxy.cpp`, `src/main.cpp`, and `src/version_watermark.cpp` are included: a version label and an attempt to load G3SE plugins. The remaining optimization code is not built, and `gothic3sdk` is not linked. This build has not been tested in-game with Parallel Universe Patch (PUP).

## Choose a DXVK version

DXVK translates Gothic 3's Direct3D 9 calls to Vulkan. It does not add textures or effects. DXVK packages are separate from `dinput8.dll`; each includes `d3d9.dll`, `dxgi.dll`, and `dxvk.conf`.

| Package | Driver | Difference |
| --- | --- | --- |
| `1.10.1-async` | Vulkan 1.1 | Older version; experimental async. |
| `1.10.3-async` | Vulkan 1.1 | Adds cube-map seam control. The user reports it works with anisotropy `-1`. |
| `2.4.1` | Vulkan 1.3 | Texture-sharpness and compilation options; no async. |
| `3.0.1` | Vulkan 1.4 plus required features | New GPU/memory options; some older keys are gone. |
| `3.0.1-hdr` | Same as `3.0.1` | Identical DLLs. The name does not mean Gothic 3 gains HDR. |

The same rule applies to AMD, NVIDIA, and Intel: choose by Vulkan support, not GPU brand. The RX 570 8 GB on this PC reports Vulkan 1.3.260: keep the working `1.10.3-async` or compare it with `2.4.1`; this driver does not meet `3.0.1`'s requirement. The Vulkan version alone does not guarantee every required driver feature.

## What to change

Put `dxvk.conf` next to `Gothic3.exe`. G3O's base configs are almost empty; only the `-async` packages set `dxvk.enableAsync = true`. Back up the working file and change one line at a time.

### Image quality

- Set textures, shadows, draw distance, and anti-aliasing in the game. DXVK does not add these details.
- All versions: keep `d3d9.samplerAnisotropy = -1` or omit the line. Forcing `16` caused red noise on the RX 570; `-1` fixed it for the user.
- `2.4.1`/`3.0.1`: `d3d9.samplerLodBias = -0.25` may sharpen textures but cause shimmer. Test it separately; it is not in the base config.
- `1.10.1`/`1.10.3`/`2.4.1`: `d3d9.forceSwapchainMSAA` forces anti-aliasing but may break post-processing. Removed in `3.0.1`.

### FPS and latency

- `1.10.x-async`: `dxvk.enableAsync = true` reduces compilation stalls, not image quality.
- `2.4.1`/`3.0.1`: `dxvk.enableGraphicsPipelineLibrary` controls pipeline compilation; leave it at the default `Auto`.
- All versions: `d3d9.maxFrameRate` caps FPS; `d3d9.maxFrameLatency` changes the frame queue. Neither increases detail.

### Compatibility

- Do not enable `d3d9.supportCubeDepthFormats`: the official DXVK config warns of issues specifically in Gothic 3 (available in `3.0.1`).
- `dxgi.enableHDR` (`2.4.1`/`3.0.1`) does not enable HDR in a D3D9 game. Spoofing GPU vendors is a game-specific workaround, not an AMD/NVIDIA/Intel speed boost.

**Complete reference:** [all 127 published DXVK keys across the four versions, grouped by purpose and availability](docs/DXVK_OPTIONS.md) (Russian; links to the official English configs included). Environment variables, internal keys, and game profiles are not included in the 127.

## Build G3O

```cmd
git submodule update --init third_party/minhook
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
```

DLL: `build/Release/dinput8.dll`; packaging: `scripts/package_releases.ps1`. Back up an existing game-folder `dinput8.dll` before replacing it: another mod may own it. Editing this repository does not change the installed game. License: [MIT](LICENSE).
