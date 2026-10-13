#pragma once
#include <windows.h>

namespace LRUCacheManager {
    // Initialize adaptive LRU cache hooks on eCMainCache and background memory watchdog
    bool Initialize();
    void Shutdown();

    // Track hero position to trigger smart cache evictions on regional biome changes (Myrtana / Varant / Nordmar)
    void NotifyPlayerPosition(float x, float y, float z);

    // Explicitly perform memory cleanup and purge stale cache entries
    void PerformAdaptiveEviction(bool forceAllStale);
}
