#pragma once
#include <windows.h>

namespace G3O {

bool InitializeFastPakDecompressor();
void ShutdownFastPakDecompressor();

LONG GetPakDecompCount();
LONG64 GetPakDecompInBytes();
LONG64 GetPakDecompOutBytes();
double GetPakDecompTimeMs();

void SetDecompLoadThreadId(DWORD tid);
LONG GetPakDecompMainThreadCount();
double GetPakDecompMainThreadTimeMs();
LONG GetPakDecompWorkerCount();
double GetPakDecompWorkerTimeMs();

LONG GetSavegameDecompCount();
LONG64 GetSavegameDecompInBytes();
LONG64 GetSavegameDecompOutBytes();
double GetSavegameDecompTimeMs();

void ResetDecompTelemetry();

} // namespace G3O
