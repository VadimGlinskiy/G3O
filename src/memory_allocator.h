#pragma once

namespace MemoryOptimizer {
    // Initialize mimalloc hooks on bCMemoryAdmin, MemHeap and CRT
    bool Initialize();
    void Shutdown();
}
