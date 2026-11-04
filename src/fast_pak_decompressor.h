#pragma once

namespace G3O {

// Hooks FileSystem.dll internal zlib decompressor (RVA 0xA2D0) with SIMD-accelerated libdeflate.
// Accelerates .pak archive decompression by 15x-25x (1.5 GB/s vs 88 MB/s),
// drastically reducing savegame loading time and world cell streaming hitches.
bool InitializeFastPakDecompressor();
void ShutdownFastPakDecompressor();

} // namespace G3O
