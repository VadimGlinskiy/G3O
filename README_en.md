# Gothic 3 Optima

[Русский](README.md)

G3O 1.6.0 is a 32-bit `dinput8.dll` proxy for Gothic 3. It includes borderless-window support, FOV controls, diagnostics, and optional optimization switches. It reads `G3O.ini` beside `Gothic3.exe`. In-game compatibility with PUP has not been tested.

## Diagnostic files

- Crash Investigator catches some fatal engine errors and unhandled exceptions. Reports include module and call-stack details; these can help investigate a crash but do not prove its root cause.
- `ShowCrashDialog` and `WriteCrashDump` apply to unhandled Win32 exceptions. For the engine's `FatalError` path, G3O writes a text report but does not show this dialog or create a minidump.
- With `EnableCrashInvestigator=true` and `WriteCrashLog=true`, G3O creates `G3O_CrashReport_*.log`; with `WriteCrashDump=true`, it creates `G3O_Crash_*.dmp`. Names include the date, time, process ID, and thread ID. The report records the dump path and the result of basic dump checks.
- `EnableProfiler=true` creates `G3O_profile_*.log`; `EnableSavegameAudit=true` enables detailed measurements in `G3O_LoadProfiler_*.log`. The PAK trace cache for a save is named `G3O_PakTrace_<save-name>.csv`; G3O rewrites it during loading when no valid trace exists and matching data is found.
- `EnableDiagnosticLog=true` enables `G3O_diagnostic_*.log`. `DiagOutput` and `OutputDebugStringA` messages are included when interception is enabled.

G3O diagnostic files are stored directly in `<game folder>\G3O\`, with no nested directories. `G3O.ini` remains beside `Gothic3.exe`. The game folder must be writable; G3O does not fall back to Documents if writing fails.

## Install

- Extract `dinput8.dll` from `Gothic3Optima-v1.6.0-Core.zip` next to `Gothic3.exe`. Back up any existing `dinput8.dll` first; another mod may own it.
- DXVK is separate. Pick **one** DXVK archive and extract it to the same folder. Back up existing `d3d9.dll`, `dxgi.dll`, and `dxvk.conf` before replacing them.

## DXVK

DXVK runs the game's Direct3D 9 graphics through Vulkan; it does not add textures or effects. Choose a version your driver supports, regardless of whether your GPU is AMD, NVIDIA, or Intel:

- `1.10.1-async` and `1.10.3-async` — Vulkan 1.1; experimental async shader compilation. Version 1.10.3 adds a cube-map seam option.
- `2.4.1` — Vulkan 1.3; texture-sharpness settings, but no async from the older fork.
- `3.0.1` — Vulkan 1.4 plus required driver features. Some older settings have been removed.
- `3.0.1-hdr` — the same DLLs as `3.0.1`. The archive kept its old name; it does not enable HDR in Gothic 3.

## DXVK settings

Put `dxvk.conf` next to `Gothic3.exe`. All of our packages enable safe window and overlay initialization via `d3d9.deferSurfaceCreation = True` by default; the `-async` versions also enable background shader compilation with `dxvk.enableAsync = true`.

Forcing `d3d9.samplerAnisotropy = 16` caused red artifacts on an RX 570. Changing it to `-1` stopped them. Do not enable `d3d9.supportCubeDepthFormats` in `3.0.1`: DXVK warns about problems with Gothic 3.

Other settings are in the [version-by-version reference](docs/DXVK_OPTIONS.md) (Russian). Back up your working `dxvk.conf` first.

## Build

```cmd
git submodule update --init third_party/minhook
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
```

DLL: `build/Release/dinput8.dll`. Packaging: `scripts/package_releases.ps1`. License: [MIT](LICENSE).
