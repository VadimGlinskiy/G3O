#pragma once
#include <windows.h>

namespace G3O {

// Initialize top-level crash interception and exception filter hooks
void InitializeCrashHandler();

// Shutdown crash handler
void ShutdownCrashHandler();

// Hook engine-level fatal errors in SharedBase.dll (bCErrorAdmin)
void HookEngineFatalErrors();

// Write an engine Fatal Error report without synthesizing an exception.
void ReportEngineFatalError(const char* module, const char* message, const char* file, int line);

// Path to the crash report log
const char* GetCrashReportPath();

} // namespace G3O
