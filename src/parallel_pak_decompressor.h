#pragma once
#include <windows.h>
#include <cstdint>

namespace G3O {

bool InitializeParallelPakDecompressor();
void ShutdownParallelPakDecompressor();

void NotifyGotoLoadedPositionStart(const char* saveName);
void NotifyGotoLoadedPositionEnd();

LONG GetPakPrefetchHits();
LONG GetPakPrefetchMisses();
double GetPakPrefetchTimeSavedSec();

} // namespace G3O
