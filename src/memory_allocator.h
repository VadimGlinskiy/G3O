#pragma once

namespace MemoryOptimizer {
    // Initialize complete mimalloc replacement for bCMemoryAdmin, MemHeap, operator new and game modules
    bool Initialize();
    void Shutdown();
    void Collect();
}
