#pragma once
#include <cstdint>

namespace G3O {

// Initializes hook on bCPropertyObjectSingleton::ReadObject in SharedBase.dll
bool InitializeClassFactoryOptimizer();

// Shuts down hook and cleans up resources
void ShutdownClassFactoryOptimizer();

// Resets per-session telemetry counters
void ResetClassFactoryOptimizerSession();

// Telemetry getters
uint32_t ClassFactory_GetHits();
uint32_t ClassFactory_GetMisses();
uint32_t ClassFactory_GetCachedClassesCount();

} // namespace G3O
