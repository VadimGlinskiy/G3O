#pragma once
#include <cstdint>

namespace MemoryOptimizer {
    // Initialize isolated mimalloc replacement for bCMemoryAdmin and MemHeap in SharedBase.dll
    bool Initialize();
    void Shutdown();
    void Collect();

    uint64_t GetMiAllocCount();
    uint64_t GetMiFreeCount();
    uint64_t GetLegacyFreeCount();
    uint64_t GetMiReallocCount();
    uint64_t GetLegacyReallocCount();
}
