#pragma once

namespace G3O {

// Hooks FileSystem.dll internal zlib decompressor (RVA 0xA2D0) and
// Game.dll savegame decompressor (0x101D4900) with SIMD-accelerated libdeflate.
// Accelerates .pak virtual archives and .g3savcpxdat savegame decompression by 15x-25x,
// drastically reducing savegame loading time and world streaming hitches.
bool InitializeFastPakDecompressor();
void ShutdownFastPakDecompressor();

} // namespace G3O
