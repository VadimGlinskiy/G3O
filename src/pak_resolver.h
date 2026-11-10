#pragma once

#include <cstdint>
#include <cstddef>

namespace G3O {

struct PakResolvedResource {
    char virtualPath[260] = {0};
    char authorPath[260] = {0};
    uint64_t fileOffset = 0;
    uint64_t compressedSize = 0;
    uint64_t uncompressedSize = 0;
    bool found = false;
};

// Post-mortem resolver: searches for the virtual file matching the given physical archive offset.
// Designed specifically for crash reporting (zero runtime cost, runs only upon crash).
// Heap-independent: uses VirtualAlloc to guarantee execution even during heap corruption.
bool ResolvePakVirtualResource(const char* archiveName, uint64_t targetOffset, PakResolvedResource& outResource);

} // namespace G3O
