# Gothic 3 Optima (G3O) — 1.3.1, PUP-compatible development build

[Русский](README.md)

G3O is an experimental `dinput8.dll` proxy for Gothic 3. This branch is designed to coexist with the installed **Parallel Universe Patch 1.1.1**. The boundary was checked against the G3O sources and the local `Parallel Universe Patch/Changelog_en.txt`; **in-game compatibility has not been tested yet**.

## What the current binary does

- Forwards `DirectInput8Create` to the system `dinput8.dll`.

- Adds `G3O v1.3.1` to the main-menu version label (`src/version_watermark.cpp`).
- Optionally invokes `LoadPlugins` for `Data\\G3SE\\Plugins` if a third-party `G3SDK.dll` or `G3SE.dll` exists. **This legacy G3SE loader is not gothic3sdk integration**; neither DLL exists in the supplied game installation.

## Boundary with Parallel Universe Patch

PUP 1.1.1 already handles background resource work, caches, prefetching, file system/decompression, FMOD audio, GPU-resident textures, and asynchronous save writes. G3O therefore **does not build or run** its `multicore_manager`, `streaming_optimizer`, `media_optimizer`, `lru_cache_manager`, `d3d9_hook`, or `savegame_guard`. The last module scanned all saves and checked file size immediately after `SaveGameWorld`, which is not a valid post-write check with PUP's asynchronous saves. Other experimental source files remain in the repository but are excluded from the binary.

`E:\Projects\gothic3sdk` is a reference for future **distinct** features; the SDK is not linked in this build. DXVK packages are optional separate graphics components, not G3O functionality or replacements for PUP.

## Build and test installation

```cmd
 git submodule update --init third_party/minhook
 cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
 cmake --build build --config Release
```

Output: `build/Release/dinput8.dll`. **Do not overwrite** the existing `dinput8.dll` in the Gothic 3 directory without backing it up and identifying its owner. This change does not modify the game installation or claim a successful in-game run. To test, place the built DLL next to `Gothic3.exe` after preserving the previous DLL.

Older 1.3.0 releases and feature descriptions do not describe this branch. `scripts/package_releases.ps1` is for packaging **after** the game build has been validated; DXVK packages are independent of the core.

Source license: [MIT](LICENSE). PUP and gothic3sdk are separate projects.
