#pragma once
#include <windows.h>

namespace G3O {

bool InitializeFastPakDecompressor();
void ShutdownFastPakDecompressor();

LONG GetPakDecompCount();
LONG64 GetPakDecompInBytes();
LONG64 GetPakDecompOutBytes();
double GetPakDecompTimeMs();

LONG GetSavegameDecompCount();
LONG64 GetSavegameDecompInBytes();
LONG64 GetSavegameDecompOutBytes();
double GetSavegameDecompTimeMs();

void ResetDecompTelemetry();

LONG GetDecompCacheHits();
LONG64 GetDecompCacheBytesSaved();
void ClearDecompCache();

} // namespace G3O
