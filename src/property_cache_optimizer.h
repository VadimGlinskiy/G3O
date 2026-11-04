#pragma once
#include <windows.h>

namespace G3O {

bool InitializePropertyCacheOptimizer();
void ShutdownPropertyCacheOptimizer();

LONG GetPropertyCacheStringHits();
LONG GetPropertyCacheStringMisses();
LONG GetPropertyCacheIdHits();
LONG GetPropertyCacheIdMisses();
void ResetPropertyCacheTelemetry();

} // namespace G3O
