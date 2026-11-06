#pragma once
#include <windows.h>
#include <cstddef>

namespace G3O {

void InitializeDiagnosticLogger();
void ShutdownDiagnosticLogger();
void FlushDiagnosticLog();

// Thread-safe logging to file and circular memory buffer
void Log(const char* tag, const char* fmt, ...);
void LogRaw(const char* message);

// Recent-event storage is fixed-size; the crash-path snapshot does not allocate or lock.
constexpr std::size_t kRecentLogEventCount = 256;
constexpr std::size_t kRecentLogEventBytes = 512;
constexpr std::size_t kRecentLogSnapshotCapacity =
    kRecentLogEventCount * kRecentLogEventBytes + 1;
std::size_t CopyRecentLogs(char* outBuffer, std::size_t capacity);
void OutputCrashReportToDebugger(const char* report);

// Install hooks separately so engine diagnostics can be captured during startup.
void HookOutputDebugStringA();
void HookEngineDiagOutput();

// Path to runtime diagnostic log file
const char* GetDiagnosticLogPath();

} // namespace G3O
