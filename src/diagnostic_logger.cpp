#include "diagnostic_logger.h"
#include "recent_event_buffer.h"
#include "diag_output_bridge.h"
#include "timestamped_path.h"
#include "g3o_output_path.h"
#include <minhook.h>
#include <cstdio>
#include <cstdarg>
#include <string>

namespace G3O {

static CRITICAL_SECTION s_logCs;
static bool s_csInitialized = false;
static FILE* s_logFile = nullptr;
static char s_logPath[MAX_PATH] = { 0 };
static RecentEventBuffer<kRecentLogEventCount, kRecentLogEventBytes> s_recentEvents;
static thread_local bool s_inLogger = false;
static thread_local bool s_forwardingOriginalDiagOutput = false;
static volatile LONG s_logPathSequence = 0;
static bool s_outputDebugStringHookInstalled = false;
static bool s_diagOutputHookInstalled = false;

typedef void (WINAPI* OutputDebugStringA_t)(LPCSTR);
static OutputDebugStringA_t s_origOutputDebugStringA = nullptr;

typedef void (__cdecl* DiagOutput_t)(const char*, ...);
static DiagOutput_t s_origDiagOutput = nullptr;

static void EnsureCS() {
    if (!s_csInitialized) {
        InitializeCriticalSection(&s_logCs);
        s_csInitialized = true;
    }
}

static bool TryLogDirectory(const char* directory) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    for (int attempt = 0; attempt < 8; ++attempt) {
        const unsigned long sequence = static_cast<unsigned long>(InterlockedIncrement(&s_logPathSequence));
        if (!BuildTimestampedFilePath(s_logPath, sizeof(s_logPath), directory,
                                      "G3O_diagnostic", "log", st,
                                      GetCurrentProcessId(), GetCurrentThreadId(), sequence)) {
            return false;
        }
        FILE* probe = fopen(s_logPath, "a");
        if (probe) {
            fclose(probe);
            return true;
        }
    }
    s_logPath[0] = '\0';
    return false;
}

static void DetermineLogPath() {
    if (s_logPath[0] != '\0') return;
    char directory[MAX_PATH] = {};
    if (!EnsureG3OOutputDirectory(directory, sizeof(directory))) return;
    TryLogDirectory(directory);
}

const char* GetDiagnosticLogPath() {
    if (s_logPath[0] == '\0') {
        DetermineLogPath();
    }
    return s_logPath;
}

void InitializeDiagnosticLogger() {
    EnsureCS();
    DetermineLogPath();

    EnterCriticalSection(&s_logCs);
    s_logFile = fopen(s_logPath, "a");
    if (s_logFile) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(s_logFile, "==================================================================\n");
        fprintf(s_logFile, " Gothic 3 Optima (G3O) Diagnostic Log\n");
        fprintf(s_logFile, " Started: %04d-%02d-%02d %02d:%02d:%02d.%03d\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        fprintf(s_logFile, " PID: %lu, TID: %lu\n", GetCurrentProcessId(), GetCurrentThreadId());
        fprintf(s_logFile, "==================================================================\n\n");
        fflush(s_logFile);
    }
    LeaveCriticalSection(&s_logCs);
}

void FlushDiagnosticLog() {
    if (!s_csInitialized) return;
    EnterCriticalSection(&s_logCs);
    if (s_logFile) {
        fflush(s_logFile);
    }
    LeaveCriticalSection(&s_logCs);
}

void ShutdownDiagnosticLogger() {
    if (!s_csInitialized) return;

    EnterCriticalSection(&s_logCs);
    if (s_logFile) {
        fprintf(s_logFile, "\n[G3O] Diagnostic Logger terminated cleanly.\n");
        fflush(s_logFile);
        fclose(s_logFile);
        s_logFile = nullptr;
    }
    LeaveCriticalSection(&s_logCs);
}

void LogRaw(const char* message) {
    if (!message || s_inLogger) return;
    s_inLogger = true;

    EnsureCS();
    EnterCriticalSection(&s_logCs);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char timeBuf[32];
    snprintf(timeBuf, sizeof(timeBuf), "[%02d:%02d:%02d.%03d] ",
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    std::string fullLine = std::string(timeBuf) + message;
    if (fullLine.back() != '\n') {
        fullLine += '\n';
    }

    s_recentEvents.Append(fullLine.c_str(), fullLine.size());

    static int s_flushCounter = 0;
    if (s_logFile) {
        fputs(fullLine.c_str(), s_logFile);
        if (++s_flushCounter >= 128) {
            fflush(s_logFile);
            s_flushCounter = 0;
        }
    }

    LeaveCriticalSection(&s_logCs);
    s_inLogger = false;
}

void Log(const char* tag, const char* fmt, ...) {
    if (!fmt) return;

    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    char fullMsg[2200];
    if (tag && tag[0] != '\0') {
        snprintf(fullMsg, sizeof(fullMsg), "[%s] %s", tag, buffer);
    } else {
        snprintf(fullMsg, sizeof(fullMsg), "%s", buffer);
    }

    LogRaw(fullMsg);
}

std::size_t CopyRecentLogs(char* outBuffer, std::size_t capacity) {
    return s_recentEvents.CopyTo(outBuffer, capacity);
}

void OutputCrashReportToDebugger(const char* report) {
    if (report && s_origOutputDebugStringA && IsDebuggerPresent()) {
        s_origOutputDebugStringA(report);
    }
}

static void WINAPI HookedOutputDebugStringA(LPCSTR lpOutputString) {
    if (ForwardNestedDiagOutputDebugString(s_forwardingOriginalDiagOutput,
                                           s_origOutputDebugStringA, lpOutputString,
                                           IsDebuggerPresent() != FALSE)) {
        return;
    }
    if (lpOutputString && lpOutputString[0] != '\0') {
        // Strip trailing newline for clean formatting
        char clean[1024];
        strncpy(clean, lpOutputString, sizeof(clean) - 1);
        clean[sizeof(clean) - 1] = '\0';
        size_t len = strlen(clean);
        while (len > 0 && (clean[len - 1] == '\r' || clean[len - 1] == '\n')) {
            clean[len - 1] = '\0';
            len--;
        }
        if (clean[0] != '\0') {
            Log("DebugOut", "%s", clean);
        }
    }
    // Only forward to Windows kernel if a real debugger is attached.
    // In normal execution, calling OutputDebugStringA forces Windows kernel
    // exception dispatch (DBG_PRINTEXCEPTION_C) and LPC messaging, causing severe disk/thread stalls.
    if (s_origOutputDebugStringA && IsDebuggerPresent()) {
        s_origOutputDebugStringA(lpOutputString);
    }
}

static void __cdecl HookedDiagOutput(const char* fmt, ...) {
    const DWORD entryLastError = GetLastError();
    char buffer[2048] = {};
    if (fmt) {
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);

        // Strip trailing newline
        size_t len = strlen(buffer);
        while (len > 0 && (buffer[len - 1] == '\r' || buffer[len - 1] == '\n')) {
            buffer[len - 1] = '\0';
            len--;
        }
        ForwardDiagOutput(s_origDiagOutput, buffer, s_forwardingOriginalDiagOutput);
        Log("EngineDiag", "%s", buffer);
    }
    SetLastError(entryLastError);
}

void HookOutputDebugStringA() {
    if (s_outputDebugStringHookInstalled) return;
    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    if (hKernel32) {
        void* pODS = (void*)GetProcAddress(hKernel32, "OutputDebugStringA");
        if (pODS) {
            const MH_STATUS createStatus = MH_CreateHook(
                pODS, (void*)&HookedOutputDebugStringA, (void**)&s_origOutputDebugStringA);
            if (createStatus == MH_OK || createStatus == MH_ERROR_ALREADY_CREATED) {
                const MH_STATUS enableStatus = MH_EnableHook(pODS);
                s_outputDebugStringHookInstalled = enableStatus == MH_OK || enableStatus == MH_ERROR_ENABLED;
            }
        }
    }
}

void HookEngineDiagOutput() {
    if (s_diagOutputHookInstalled) return;
    HMODULE hSharedBase = GetModuleHandleA("SharedBase.dll");
    if (hSharedBase) {
        void* pDiag = (void*)GetProcAddress(hSharedBase, "?DiagOutput@@YAXPBDZZ");
        if (pDiag) {
            const MH_STATUS createStatus = MH_CreateHook(
                pDiag, (void*)&HookedDiagOutput, (void**)&s_origDiagOutput);
            if (createStatus == MH_OK || createStatus == MH_ERROR_ALREADY_CREATED) {
                const MH_STATUS enableStatus = MH_EnableHook(pDiag);
                s_diagOutputHookInstalled = enableStatus == MH_OK || enableStatus == MH_ERROR_ENABLED;
                if (s_diagOutputHookInstalled) {
                    Log("G3O", "Hooked SharedBase.dll!DiagOutput successfully.");
                }
            }
        }
    }
}

} // namespace G3O
