#pragma once
#include <windows.h>

namespace G3O {

// Initialize top-level crash interception and exception filter hooks
void InitializeCrashHandler();

// Shutdown crash handler
void ShutdownCrashHandler();

// Hook engine-level fatal errors in SharedBase.dll (bCErrorAdmin)
void HookEngineFatalErrors();

// Trigger a manual crash report (e.g. on fatal assertion)
void ReportManualCrash(const char* title, const char* message, const char* file, int line);

// Path to the crash report log
const char* GetCrashReportPath();

} // namespace G3O
