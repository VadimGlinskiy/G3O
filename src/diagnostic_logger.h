#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace G3O {

void InitializeDiagnosticLogger();
void ShutdownDiagnosticLogger();
void FlushDiagnosticLog();

// Thread-safe logging to file and circular memory buffer
void Log(const char* tag, const char* fmt, ...);
void LogRaw(const char* message);

// Retrieve snapshot of recent in-memory log entries (for crash report)
void GetRecentLogs(std::vector<std::string>& outLogs);

// Install hooks for engine DiagOutput and OutputDebugStringA
void HookLoggingFunctions();

// Path to runtime diagnostic log file
const char* GetDiagnosticLogPath();

} // namespace G3O
