#include "diagnostic_logger.h"
#include <minhook.h>
#include <cstdio>
#include <cstdarg>
#include <deque>
#include <mutex>
#include <shlobj.h>

namespace G3O {

static CRITICAL_SECTION s_logCs;
static bool s_csInitialized = false;
static FILE* s_logFile = nullptr;
static char s_logPath[MAX_PATH] = { 0 };
static std::deque<std::string> s_recentLogs;
static const size_t MAX_RECENT_LOGS = 256;
static thread_local bool s_inLogger = false;

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

static void DetermineLogPath() {
    // Try game folder first
    char exePath[MAX_PATH] = { 0 };
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    char* lastSlash = strrchr(exePath, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';

    snprintf(s_logPath, sizeof(s_logPath), "%sG3O_diagnostic.log", exePath);
    FILE* testF = fopen(s_logPath, "a");
    if (testF) {
        fclose(testF);
        return;
    }

    // Fallback to Documents\Gothic3
    char docPath[MAX_PATH] = { 0 };
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, SHGFP_TYPE_CURRENT, docPath))) {
        snprintf(s_logPath, sizeof(s_logPath), "%s\\Gothic3\\G3O_diagnostic.log", docPath);
    }
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
    s_logFile = fopen(s_logPath, "w");
    if (s_logFile) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(s_logFile, "==================================================================\n");
        fprintf(s_logFile, " Gothic 3 Optima (G3O) Diagnostic Log\n");
        fprintf(s_logFile, " Started: %04d-%02d-%02d %02d:%02d:%02d.%03d\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
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

    if (s_recentLogs.size() >= MAX_RECENT_LOGS) {
        s_recentLogs.pop_front();
    }
    s_recentLogs.push_back(fullLine);

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

void GetRecentLogs(std::vector<std::string>& outLogs) {
    EnsureCS();
    EnterCriticalSection(&s_logCs);
    outLogs.assign(s_recentLogs.begin(), s_recentLogs.end());
    LeaveCriticalSection(&s_logCs);
}

static void WINAPI HookedOutputDebugStringA(LPCSTR lpOutputString) {
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
    if (fmt) {
        char buffer[2048];
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
        Log("EngineDiag", "%s", buffer);
    }
}

void HookLoggingFunctions() {
    // 1. Hook OutputDebugStringA (captures DXVK, scripts, Windows debug)
    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    if (hKernel32) {
        void* pODS = (void*)GetProcAddress(hKernel32, "OutputDebugStringA");
        if (pODS) {
            MH_CreateHook(pODS, (void*)&HookedOutputDebugStringA, (void**)&s_origOutputDebugStringA);
            MH_EnableHook(pODS);
        }
    }

    // 2. Hook SharedBase.dll DiagOutput if available
    HMODULE hSharedBase = GetModuleHandleA("SharedBase.dll");
    if (hSharedBase) {
        void* pDiag = (void*)GetProcAddress(hSharedBase, "?DiagOutput@@YAXPBDZZ");
        if (pDiag) {
            MH_CreateHook(pDiag, (void*)&HookedDiagOutput, (void**)&s_origDiagOutput);
            MH_EnableHook(pDiag);
            Log("G3O", "Hooked SharedBase.dll!DiagOutput successfully.");
        }
    }
}

} // namespace G3O
