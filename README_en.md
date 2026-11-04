# Gothic 3 Optima

[Русский](README.md)

G3O v1.3.1 is a `dinput8.dll` proxy with a version label in the menu, built-in **Crash Investigator**, and **Diagnostic Logger**. If G3SE is installed, it attempts to load its plugins. The v1.3.0 engine optimizations are absent to avoid overlap with Parallel Universe Patch (PUP). In-game compatibility with PUP has not been tested.

## Diagnostic Features
- **Crash Investigator:** Catches fatal crashes (Access Violations, Stack Overflows, etc.) before the legacy "Where is the guru?" prompt and accurately blames the faulting module:
  - Script mods (`scripts\Script_*.dll`);
  - Third-party overlays (Bandicam `bdcamvk32.dll`, RTSS `RTSSHooks.dll`, Discord, Steam);
  - Graphics drivers and translation layers (`amdvlk32.dll`, `nvoglv32.dll`, DXVK `d3d9.dll`);
  - Core engine faults and 32-bit virtual address space exhaustion (>3.6 GB).
- **Stack Walking & Root-Cause Analysis:** Identifies when a script mod provoked an engine crash higher in the call stack.
- **Detailed Reports & MiniDumps:** Generates `G3O_CrashReport.log`, a full `.dmp` minidump, and a clear error dialog with actionable recommendations.
- **Diagnostic Logger:** Records `G3O_diagnostic.log`, capturing engine messages (`DiagOutput`) and system debug output (`OutputDebugStringA`).

## Install

- Extract `dinput8.dll` from `Gothic3Optima-v1.3.1-Core.zip` next to `Gothic3.exe`. Back up any existing `dinput8.dll` first; another mod may own it.
- DXVK is separate. Pick **one** DXVK archive and extract it to the same folder. Back up existing `d3d9.dll`, `dxgi.dll`, and `dxvk.conf` before replacing them.

## DXVK

DXVK runs the game's Direct3D 9 graphics through Vulkan; it does not add textures or effects. Choose a version your driver supports, regardless of whether your GPU is AMD, NVIDIA, or Intel:

- `1.10.1-async` and `1.10.3-async` — Vulkan 1.1; experimental async shader compilation. Version 1.10.3 adds a cube-map seam option.
- `2.4.1` — Vulkan 1.3; texture-sharpness settings, but no async from the older fork.
- `3.0.1` — Vulkan 1.4 plus required driver features. Some older settings have been removed.
- `3.0.1-hdr` — the same DLLs as `3.0.1`. The archive kept its old name; it does not enable HDR in Gothic 3.

## DXVK settings

Put `dxvk.conf` next to `Gothic3.exe`. Our packages have just one active override: `dxvk.enableAsync = true` in the two `-async` versions.

Forcing `d3d9.samplerAnisotropy = 16` caused red artifacts on an RX 570. Changing it to `-1` stopped them. Do not enable `d3d9.supportCubeDepthFormats` in `3.0.1`: DXVK warns about problems with Gothic 3.

Other settings are in the [version-by-version reference](docs/DXVK_OPTIONS.md) (Russian). Back up your working `dxvk.conf` first.

## Build

```cmd
git submodule update --init third_party/minhook
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
```

DLL: `build/Release/dinput8.dll`. Packaging: `scripts/package_releases.ps1`. License: [MIT](LICENSE).
