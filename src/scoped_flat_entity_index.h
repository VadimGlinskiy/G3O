#pragma once
#include <windows.h>
#include <cstdint>

namespace G3O {

struct FlatEntityIndexStats {
    uint64_t totalLookups = 0;
    uint64_t fastHits = 0;
    uint64_t fallbacks = 0;
    uint64_t mismatches = 0;
    uint64_t upserts = 0;
    uint64_t removes = 0;
    uint32_t itemCount = 0;
    uint32_t managedMapCount = 0;
};

bool InitializeScopedFlatEntityIndex();
void ShutdownScopedFlatEntityIndex();

void ScopedFlatEntityIndex_OnLoadStart(void* pWorld);
void ScopedFlatEntityIndex_OnLoadEnd();

bool ScopedFlatEntityIndex_IsActive();
FlatEntityIndexStats ScopedFlatEntityIndex_GetStats();

} // namespace G3O
