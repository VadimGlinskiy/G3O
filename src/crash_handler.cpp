#include "crash_handler.h"
#include "diagnostic_logger.h"
#include "config_manager.h"
#include "applied_config_snapshot.h"
#include "timestamped_path.h"
#include "g3o_output_path.h"
#include "engine_fatal_report.h"
#include "g3o_build_info.h"
#include <minhook.h>
#include <dbghelp.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <winver.h>
#include <cstdio>
#include <cstring>
#include <cctype>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")

namespace G3O {

static char s_crashLogPath[MAX_PATH] = { 0 };
static char s_crashLogDirectory[MAX_PATH] = { 0 };
static char s_dumpPath[MAX_PATH] = { 0 };
static char s_crashEventSnapshot[kRecentLogSnapshotCapacity] = { 0 };
static char s_engineFatalEventSnapshot[kRecentLogSnapshotCapacity] = { 0 };
static char s_runtimeContextStorage[128 * 1024] = { 0 };
static BYTE s_fileVersionStorage[64 * 1024] = { 0 };
static volatile LONG s_reportPipelineActive = 0;
static volatile LONG s_reportSequence = 0;

struct ReportFileTargets {
    HANDLE reportFile = INVALID_HANDLE_VALUE;
    char reportPath[MAX_PATH] = {};
};

struct MiniDumpResult {
    bool attempted = false;
    bool written = false;
    bool headerVerified = false;
    unsigned long long sizeBytes = 0;
    DWORD error = ERROR_SUCCESS;
};

static HANDLE s_dumpRequestEvent = nullptr;
static HANDLE s_dumpCompleteEvent = nullptr;
static HANDLE s_dumpWorkerThread = nullptr;
static DWORD s_dumpWorkerThreadId = 0;
static EXCEPTION_RECORD s_dumpExceptionRecord = {};
static CONTEXT s_dumpExceptionContext = {};
static EXCEPTION_POINTERS s_dumpExceptionPointers = {};
static DWORD s_dumpExceptionThreadId = 0;
static MiniDumpResult s_dumpWorkerResult;
static constexpr DWORD kDumpWorkerTimeoutMs = 30000;

typedef LPTOP_LEVEL_EXCEPTION_FILTER (WINAPI *SetUnhandledExceptionFilter_t)(LPTOP_LEVEL_EXCEPTION_FILTER);
static SetUnhandledExceptionFilter_t s_origSetUnhandledExceptionFilter = nullptr;
static LPTOP_LEVEL_EXCEPTION_FILTER s_previousFilter = nullptr;
static PVOID s_vehHandle = nullptr;
typedef BOOL (WINAPI *SymRefreshModuleList_t)(HANDLE hProcess);
static SymRefreshModuleList_t s_symRefreshModuleList = nullptr;

static volatile LONG s_dbgHelpActive = 0;
static HANDLE s_symbolProcess = nullptr;
static volatile LONG s_symbolHandlerReady = 0;

class DbgHelpLockGuard {
public:
    DbgHelpLockGuard() : m_acquired(InterlockedCompareExchange(&s_dbgHelpActive, 1, 0) == 0) {}
    ~DbgHelpLockGuard() {
        if (m_acquired) InterlockedExchange(&s_dbgHelpActive, 0);
    }
    bool acquired() const { return m_acquired; }

private:
    bool m_acquired;
};

struct BlameResult {
    char moduleName[MAX_PATH] = {};
    char modulePath[MAX_PATH] = {};
    uintptr_t baseAddress = 0;
    uintptr_t rva = 0;
    char category[256] = {};
    char description[768] = {};
    char recommendation[768] = {};
    bool isKnownCulprit = false;
};

static void DetermineCrashPaths() {
    EnsureG3OOutputDirectory(s_crashLogDirectory, sizeof(s_crashLogDirectory));
}

static HANDLE ReserveUniqueReportFile(const char* directory, char* outputPath,
                                     size_t outputCapacity, const SYSTEMTIME& time,
                                     DWORD processId, DWORD threadId) {
    if (!directory || !directory[0] || !outputPath || outputCapacity == 0) return INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 32; ++attempt) {
        const unsigned long sequence = static_cast<unsigned long>(InterlockedIncrement(&s_reportSequence));
        if (!BuildTimestampedFilePath(outputPath, outputCapacity, directory,
                                      "G3O_CrashReport", "log", time,
                                      processId, threadId, sequence)) {
            return INVALID_HANDLE_VALUE;
        }
        HANDLE file = CreateFileA(outputPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) return file;
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) break;
    }
    outputPath[0] = '\0';
    return INVALID_HANDLE_VALUE;
}

static ReportFileTargets ReserveReportFiles(bool enabled) {
    ReportFileTargets files;
    s_crashLogPath[0] = '\0';
    if (!enabled) return files;
    DetermineCrashPaths();
    if (!s_crashLogDirectory[0]) return files;

    SYSTEMTIME time;
    GetLocalTime(&time);
    files.reportFile = ReserveUniqueReportFile(s_crashLogDirectory, files.reportPath,
                                               sizeof(files.reportPath), time,
                                               GetCurrentProcessId(), GetCurrentThreadId());
    if (files.reportFile != INVALID_HANDLE_VALUE) {
        snprintf(s_crashLogPath, sizeof(s_crashLogPath), "%s", files.reportPath);
    }
    return files;
}

static bool WriteReportToHandle(HANDLE file, const char* report) {
    if (file == INVALID_HANDLE_VALUE || !report) return false;
    const size_t length = strlen(report);
    if (length > 0xFFFFFFFFu) return false;
    const DWORD bytesToWrite = static_cast<DWORD>(length);
    DWORD bytesWritten = 0;
    return WriteFile(file, report, bytesToWrite, &bytesWritten, nullptr) &&
           bytesWritten == bytesToWrite && FlushFileBuffers(file);
}

static void CloseReportFiles(ReportFileTargets& files) {
    if (files.reportFile != INVALID_HANDLE_VALUE) {
        CloseHandle(files.reportFile);
        files.reportFile = INVALID_HANDLE_VALUE;
    }
}

const char* GetCrashReportPath() {
    if (!s_crashLogDirectory[0]) DetermineCrashPaths();
    return s_crashLogPath;
}

static const char* GetExceptionCodeString(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:         return "EXCEPTION_ACCESS_VIOLATION (Access Violation)";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED (Array Bounds Exceeded)";
    case EXCEPTION_BREAKPOINT:               return "EXCEPTION_BREAKPOINT (Breakpoint Hit)";
    case EXCEPTION_DATATYPE_MISALIGNMENT:    return "EXCEPTION_DATATYPE_MISALIGNMENT (Datatype Misalignment)";
    case EXCEPTION_FLT_DENORMAL_OPERAND:     return "EXCEPTION_FLT_DENORMAL_OPERAND (Float Denormal)";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "EXCEPTION_FLT_DIVIDE_BY_ZERO (Float Divide By Zero)";
    case EXCEPTION_FLT_INEXACT_RESULT:       return "EXCEPTION_FLT_INEXACT_RESULT (Float Inexact Result)";
    case EXCEPTION_FLT_INVALID_OPERATION:    return "EXCEPTION_FLT_INVALID_OPERATION (Float Invalid Operation)";
    case EXCEPTION_FLT_OVERFLOW:             return "EXCEPTION_FLT_OVERFLOW (Float Overflow)";
    case EXCEPTION_FLT_STACK_CHECK:          return "EXCEPTION_FLT_STACK_CHECK (Float Stack Check)";
    case EXCEPTION_FLT_UNDERFLOW:            return "EXCEPTION_FLT_UNDERFLOW (Float Underflow)";
    case EXCEPTION_ILLEGAL_INSTRUCTION:      return "EXCEPTION_ILLEGAL_INSTRUCTION (Illegal Instruction)";
    case EXCEPTION_IN_PAGE_ERROR:            return "EXCEPTION_IN_PAGE_ERROR (In-Page Error)";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "EXCEPTION_INT_DIVIDE_BY_ZERO (Integer Divide By Zero)";
    case EXCEPTION_INT_OVERFLOW:             return "EXCEPTION_INT_OVERFLOW (Integer Overflow)";
    case EXCEPTION_INVALID_DISPOSITION:      return "EXCEPTION_INVALID_DISPOSITION (Invalid Disposition)";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "EXCEPTION_NONCONTINUABLE_EXCEPTION (Non-Continuable Exception)";
    case EXCEPTION_PRIV_INSTRUCTION:         return "EXCEPTION_PRIV_INSTRUCTION (Privileged Instruction)";
    case EXCEPTION_SINGLE_STEP:              return "EXCEPTION_SINGLE_STEP (Single Step)";
    case EXCEPTION_STACK_OVERFLOW:           return "EXCEPTION_STACK_OVERFLOW (Stack Overflow)";
    case 0xE06D7363:                         return "MSVC_CPP_EXCEPTION (Uncaught C++ Exception)";
    default:                                 return "UNKNOWN_EXCEPTION";
    }
}

static void CopyText(char* destination, size_t capacity, const char* source) {
    if (!destination || capacity == 0) return;
    if (!source) source = "";
    size_t length = strlen(source);
    if (length >= capacity) length = capacity - 1;
    if (length) memcpy(destination, source, length);
    destination[length] = 0;
}

static void CopyLowercase(char* destination, size_t capacity, const char* source) {
    CopyText(destination, capacity, source);
    for (size_t i = 0; destination && i + 1 < capacity && destination[i]; ++i) {
        destination[i] = static_cast<char>(tolower(static_cast<unsigned char>(destination[i])));
    }
}

static bool IsScriptsPath(const char* path) {
    if (!path) return false;
    const char separator = static_cast<char>(92);
    for (const char* current = path; *current; ++current) {
        const bool atSegmentStart = current == path || current[-1] == '/' || current[-1] == separator;
        if (atSegmentStart && strncmp(current, "scripts", 7) == 0 &&
            (current[7] == '/' || current[7] == separator)) return true;
    }
    return false;
}

static void AnalyzeAddress(uintptr_t address, BlameResult& result) {
    memset(&result, 0, sizeof(result));
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery((LPCVOID)address, &mbi, sizeof(mbi)) && mbi.AllocationBase) {
        HMODULE hMod = (HMODULE)mbi.AllocationBase;
        char modulePath[MAX_PATH] = {};
        if (GetModuleFileNameA(hMod, modulePath, MAX_PATH)) {
            CopyText(result.modulePath, sizeof(result.modulePath), modulePath);
            result.baseAddress = (uintptr_t)hMod;
            result.rva = address - result.baseAddress;
            const char* slash = strrchr(modulePath, static_cast<char>(92));
            const char* forwardSlash = strrchr(modulePath, '/');
            if (!slash || (forwardSlash && forwardSlash > slash)) slash = forwardSlash;
            CopyText(result.moduleName, sizeof(result.moduleName), slash ? slash + 1 : modulePath);
        }
    }

    if (result.moduleName[0] == 0) {
        CopyText(result.moduleName, sizeof(result.moduleName), "<UNKNOWN MEMORY>");
        CopyText(result.category, sizeof(result.category), "Неизвестная область памяти / Сбой указателя");
        CopyText(result.description, sizeof(result.description), "Адрес сбоя не принадлежит ни одной загруженной библиотеке DLL (возможно, вызов по нулевому указателю или повреждённой таблице vtable).");
        CopyText(result.recommendation, sizeof(result.recommendation), "Проверьте целостность файлов игры и порядок загрузки модов.");
        return;
    }

    char lowerName[MAX_PATH] = {};
    char lowerPath[MAX_PATH] = {};
    CopyLowercase(lowerName, sizeof(lowerName), result.moduleName);
    CopyLowercase(lowerPath, sizeof(lowerPath), result.modulePath);

    if (strstr(lowerName, "bdcam")) {
        CopyText(result.category, sizeof(result.category), "Внешний оверлей (Bandicam Video Capture)");
        CopyText(result.description, sizeof(result.description), "Хук оверлея Bandicam (bdcamvk32.dll / bdcam32.dll) конфликтует с трансляцией Vulkan/DXVK.");
        CopyText(result.recommendation, sizeof(result.recommendation), "Отключите функцию оверлея/захвата Vulkan в настройках Bandicam или отключите Bandicam перед запуском игры.");
        result.isKnownCulprit = true;
    } else if (strstr(lowerName, "rtss")) {
        CopyText(result.category, sizeof(result.category), "Внешний оверлей (RivaTuner Statistics Server / MSI Afterburner)");
        CopyText(result.description, sizeof(result.description), "Оверлей RTSS конфликтует с графическим пайплайном игры.");
        CopyText(result.recommendation, sizeof(result.recommendation), "Добавьте Gothic3.exe в список RTSS и установите Application Detection Level в 'None' или 'Low'.");
        result.isKnownCulprit = true;
    } else if (strstr(lowerName, "discord")) {
        CopyText(result.category, sizeof(result.category), "Внешний оверлей (Discord In-Game Overlay)");
        CopyText(result.description, sizeof(result.description), "Хук оверлея Discord вызвал сбой в графическом контексте.");
        CopyText(result.recommendation, sizeof(result.recommendation), "Отключите внутриигровой оверлей в настройках Discord.");
        result.isKnownCulprit = true;
    } else if (strstr(lowerName, "gameoverlayrenderer")) {
        CopyText(result.category, sizeof(result.category), "Внешний оверлей (Steam Community Overlay)");
        CopyText(result.description, sizeof(result.description), "Сбой в модуле оверлея Steam.");
        CopyText(result.recommendation, sizeof(result.recommendation), "Отключите оверлей Steam в свойствах Gothic 3 в библиотеке Steam.");
        result.isKnownCulprit = true;
    } else if (strstr(lowerName, "action_x86")) {
        CopyText(result.category, sizeof(result.category), "Внешний оверлей (Mirillis Action)");
        CopyText(result.description, sizeof(result.description), "Оверлей захвата экрана Mirillis Action вызвал сбой.");
        CopyText(result.recommendation, sizeof(result.recommendation), "Отключите оверлей Action для Gothic3.exe.");
        result.isKnownCulprit = true;
    } else if (strstr(lowerName, "amdvlk") || strstr(lowerName, "atiogl") || strstr(lowerName, "amdxc")) {
        CopyText(result.category, sizeof(result.category), "Видеодрайвер (AMD Radeon Vulkan/OpenGL)");
        CopyText(result.description, sizeof(result.description), "Сбой внутри драйвера AMD Radeon при обработке команд Vulkan.");
        CopyText(result.recommendation, sizeof(result.recommendation), "Обновите драйвер видеокарты AMD до актуального или выберите другую версию DXVK в папке игры.");
        result.isKnownCulprit = true;
    } else if (strstr(lowerName, "nvoglv") || strstr(lowerName, "nvwgf2um") || strstr(lowerName, "nvd3dum")) {
        CopyText(result.category, sizeof(result.category), "Видеодрайвер (NVIDIA GeForce)");
        CopyText(result.description, sizeof(result.description), "Сбой внутри драйвера NVIDIA.");
        CopyText(result.recommendation, sizeof(result.recommendation), "Обновите видеодрайвер NVIDIA или настройте параметры DXVK в dxvk.conf.");
        result.isKnownCulprit = true;
    } else if (strstr(lowerName, "igvk")) {
        CopyText(result.category, sizeof(result.category), "Видеодрайвер (Intel Graphics Vulkan)");
        CopyText(result.description, sizeof(result.description), "Сбой внутри драйвера Intel Graphics.");
        CopyText(result.recommendation, sizeof(result.recommendation), "Обновите драйвер встроенной графики Intel.");
        result.isKnownCulprit = true;
    } else if (strcmp(lowerName, "d3d9.dll") == 0 || strcmp(lowerName, "dxgi.dll") == 0) {
        CopyText(result.category, sizeof(result.category), "Графический транслятор (DXVK Direct3D 9 -> Vulkan)");
        CopyText(result.description, sizeof(result.description), "Сбой в трансляторе DXVK при обработке Direct3D 9 вызовов.");
        CopyText(result.recommendation, sizeof(result.recommendation), "Проверьте параметры в dxvk.conf (например dxvk.enableAsync) или протестируйте другую версию DXVK (1.10.3 / 2.4.1 / 3.0.1).");
        result.isKnownCulprit = true;
    } else if (strncmp(lowerName, "script_", 7) == 0 || IsScriptsPath(lowerPath)) {
        snprintf(result.category, sizeof(result.category), "Скриптовый мод Gothic 3 (%s)", result.moduleName);
        snprintf(result.description, sizeof(result.description), "Сбой произошёл непосредственно внутри кода скриптового мода '%s'.", result.moduleName);
        snprintf(result.recommendation, sizeof(result.recommendation), "Временно переместите файл 'scripts/%s' в другую папку для проверки, либо обновите этот мод до последней совместимой версии.", result.moduleName);
        result.isKnownCulprit = true;
    } else if (strstr(lowerName, "paru_")) {
        CopyText(result.category, sizeof(result.category), "Parallel Universe Patch (PUP)");
        snprintf(result.description, sizeof(result.description), "Сбой в модуле многопоточных оптимизаций PUP (%s).", result.moduleName);
        CopyText(result.recommendation, sizeof(result.recommendation), "Проверьте параметры paru.ini или убедитесь в совместимости с другими установленными модами.");
        result.isKnownCulprit = true;
    } else if (strcmp(lowerName, "engine.dll") == 0 || strcmp(lowerName, "game.dll") == 0 || strcmp(lowerName, "sharedbase.dll") == 0 ||
               strcmp(lowerName, "gfc.dll") == 0 || strcmp(lowerName, "gui.dll") == 0 || strcmp(lowerName, "script.dll") == 0 ||
               strcmp(lowerName, "filesystem.dll") == 0 || strcmp(lowerName, "dialogs.dll") == 0 || strcmp(lowerName, "importer.dll") == 0) {
        snprintf(result.category, sizeof(result.category), "Базовый движок Gothic 3 (%s)", result.moduleName);
        CopyText(result.description, sizeof(result.description), "Сбой в основном коде движка Genome Engine.");
        CopyText(result.recommendation, sizeof(result.recommendation), "Смотрите стек вызовов ниже — часто движок падает из-за некорректных параметров, переданных из скриптовых модов или при исчерпании 32-битной памяти.");
    } else if (strcmp(lowerName, "dinput8.dll") == 0) {
        CopyText(result.category, sizeof(result.category), "Gothic 3 Optima (G3O)");
        CopyText(result.description, sizeof(result.description), "Сбой внутри прокси-модуля G3O.");
        CopyText(result.recommendation, sizeof(result.recommendation), "Сообщите разработчику G3O с приложением данного отчёта.");
    } else {
        snprintf(result.category, sizeof(result.category), "Системная или сторонняя библиотека (%s)", result.moduleName);
        snprintf(result.description, sizeof(result.description), "Сбой произошёл в библиотеке: %s", result.modulePath);
        CopyText(result.recommendation, sizeof(result.recommendation), "Проверьте зависимости и совместимость установленного ПО.");
    }
}

struct StackFrameInfo {
    uintptr_t address = 0;
    char moduleName[MAX_PATH] = {};
    uintptr_t rva = 0;
    char symbolName[512] = {};
    char sourceFile[MAX_PATH] = {};
    int lineNumber = 0;
    bool isScriptMod = false;
};

static constexpr size_t kMaxStackFrames = 32;
static constexpr size_t kCrashReportCapacity = 384 * 1024;
static constexpr size_t kEngineFatalStackCapacity = kMaxStackFrames * (MAX_PATH + 560) + 1;
static StackFrameInfo s_crashStackFrames[kMaxStackFrames] = {};
static StackFrameInfo s_engineFatalStackFrames[kMaxStackFrames] = {};
static BlameResult s_crashBlame = {};
static char s_crashReportStorage[kCrashReportCapacity] = {};
static char s_engineFatalReportStorage[kCrashReportCapacity] = {};
static char s_engineFatalStackStorage[kEngineFatalStackCapacity] = {};
static char s_engineFatalCallerStorage[512] = {};
static char s_crashDialogMessage[2048] = {};
static wchar_t s_crashDialogWideMessage[2048] = {};

static size_t WalkCallStack(CONTEXT* pContext, StackFrameInfo* frames, size_t capacity) {
    if (!frames || capacity == 0) return 0;
    memset(frames, 0, capacity * sizeof(*frames));
    if (!pContext) return 0;

    DbgHelpLockGuard dbgHelpLock;
    if (!dbgHelpLock.acquired() ||
        InterlockedCompareExchange(&s_symbolHandlerReady, 0, 0) == 0 || !s_symbolProcess) {
        return 0;
    }
    HANDLE hProcess = s_symbolProcess;
    HANDLE hThread = GetCurrentThread();

    const BOOL modulesRefreshed = s_symRefreshModuleList && s_symRefreshModuleList(hProcess);
    if (!modulesRefreshed) {
        SymCleanup(hProcess);
        if (!SymInitialize(hProcess, nullptr, TRUE)) {
            InterlockedExchange(&s_symbolHandlerReady, 0);
            return 0;
        }
    }

    CONTEXT ctx = *pContext;
    STACKFRAME64 frame = {};
    frame.AddrPC.Offset = ctx.Eip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Ebp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Esp;
    frame.AddrStack.Mode = AddrModeFlat;

    size_t frameCount = 0;
    while (frameCount < capacity) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_I386, hProcess, hThread, &frame, &ctx,
                         NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL)) {
            break;
        }
        if (frame.AddrPC.Offset == 0) break;

        StackFrameInfo& sfi = frames[frameCount];
        sfi.address = (uintptr_t)frame.AddrPC.Offset;
        BlameResult modInfo = {};
        AnalyzeAddress(sfi.address, modInfo);
        CopyText(sfi.moduleName, sizeof(sfi.moduleName), modInfo.moduleName);
        sfi.rva = modInfo.rva;
        sfi.isScriptMod = strncmp(sfi.moduleName, "script_", 7) == 0;

        char symbolBuffer[sizeof(SYMBOL_INFO) + 256] = {};
        PSYMBOL_INFO pSymbol = (PSYMBOL_INFO)symbolBuffer;
        pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        pSymbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        if (SymFromAddr(hProcess, frame.AddrPC.Offset, &displacement, pSymbol)) {
            CopyText(sfi.symbolName, sizeof(sfi.symbolName), pSymbol->Name);
            if (displacement > 0) {
                const size_t symbolLength = strlen(sfi.symbolName);
                if (symbolLength < sizeof(sfi.symbolName)) {
                    snprintf(sfi.symbolName + symbolLength, sizeof(sfi.symbolName) - symbolLength,
                             "+0x%llX", displacement);
                }
            }
        }

        IMAGEHLP_LINE64 line = {};
        line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
        DWORD lineDisplacement = 0;
        if (SymGetLineFromAddr64(hProcess, frame.AddrPC.Offset, &lineDisplacement, &line)) {
            CopyText(sfi.sourceFile, sizeof(sfi.sourceFile), line.FileName);
            sfi.lineNumber = (int)line.LineNumber;
        }
        ++frameCount;
    }
    return frameCount;
}

static bool RefreshCrashDumpPath() {
    DetermineCrashPaths();
    if (!s_crashLogDirectory[0]) return false;
    SYSTEMTIME time;
    GetLocalTime(&time);
    const unsigned long sequence = static_cast<unsigned long>(InterlockedIncrement(&s_reportSequence));
    return BuildTimestampedFilePath(s_dumpPath, sizeof(s_dumpPath), s_crashLogDirectory,
                                   "G3O_Crash", "dmp", time,
                                   GetCurrentProcessId(), GetCurrentThreadId(), sequence);
}

static BOOL CALLBACK ExcludeDumpWorkerThread(
    PVOID callbackParam,
    const PMINIDUMP_CALLBACK_INPUT callbackInput,
    PMINIDUMP_CALLBACK_OUTPUT callbackOutput) {
    if (!callbackInput || !callbackOutput) return TRUE;

    const DWORD workerThreadId = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(callbackParam));
    if (callbackInput->CallbackType == ThreadCallback &&
        callbackInput->Thread.ThreadId == workerThreadId) {
        callbackOutput->ThreadWriteFlags = 0;
    } else if (callbackInput->CallbackType == ThreadExCallback &&
               callbackInput->ThreadEx.ThreadId == workerThreadId) {
        callbackOutput->ThreadWriteFlags = 0;
    }
    return TRUE;
}

// shortcut: this stays in-process; a helper process is needed to eliminate loader-lock deadlocks.
static MiniDumpResult WriteMiniDump(EXCEPTION_POINTERS* pExceptionInfo,
                                    DWORD exceptionThreadId, DWORD workerThreadId) {
    MiniDumpResult result;
    result.attempted = true;

    if (!pExceptionInfo || !pExceptionInfo->ExceptionRecord) {
        result.error = ERROR_INVALID_PARAMETER;
        return result;
    }
    if (!s_dumpPath[0]) {
        result.error = ERROR_PATH_NOT_FOUND;
        return result;
    }

    DbgHelpLockGuard dbgHelpLock;
    if (!dbgHelpLock.acquired()) {
        result.error = ERROR_BUSY;
        return result;
    }

    HANDLE hDumpFile = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 8; ++attempt) {
        hDumpFile = CreateFileA(s_dumpPath, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hDumpFile != INVALID_HANDLE_VALUE) break;
        const DWORD openError = GetLastError();
        if (openError != ERROR_FILE_EXISTS && openError != ERROR_ALREADY_EXISTS) {
            result.error = openError;
            return result;
        }
        if (!RefreshCrashDumpPath()) {
            result.error = ERROR_PATH_NOT_FOUND;
            return result;
        }
    }
    if (hDumpFile == INVALID_HANDLE_VALUE) {
        result.error = ERROR_FILE_EXISTS;
        return result;
    }

    MINIDUMP_EXCEPTION_INFORMATION mei = {};
    mei.ThreadId = exceptionThreadId;
    mei.ExceptionPointers = pExceptionInfo;
    mei.ClientPointers = FALSE;

    MINIDUMP_CALLBACK_INFORMATION callbackInfo = {};
    callbackInfo.CallbackRoutine = ExcludeDumpWorkerThread;
    callbackInfo.CallbackParam = reinterpret_cast<PVOID>(static_cast<ULONG_PTR>(workerThreadId));

    const BOOL dumpWritten = MiniDumpWriteDump(
        GetCurrentProcess(),
        GetCurrentProcessId(),
        hDumpFile,
        (MINIDUMP_TYPE)(MiniDumpWithDataSegs | MiniDumpWithHandleData |
                        MiniDumpWithUnloadedModules | MiniDumpWithProcessThreadData),
        &mei,
        NULL,
        &callbackInfo);
    const DWORD dumpError = dumpWritten ? ERROR_SUCCESS : GetLastError();

    const BOOL flushed = FlushFileBuffers(hDumpFile);
    const DWORD flushError = flushed ? ERROR_SUCCESS : GetLastError();

    LARGE_INTEGER fileSize = {};
    const BOOL sizeVerified = GetFileSizeEx(hDumpFile, &fileSize);
    const DWORD sizeError = sizeVerified ? ERROR_SUCCESS : GetLastError();
    if (sizeVerified && fileSize.QuadPart > 0) {
        result.sizeBytes = static_cast<unsigned long long>(fileSize.QuadPart);
    }

    MINIDUMP_HEADER dumpHeader = {};
    DWORD headerBytesRead = 0;
    DWORD headerError = ERROR_BAD_FORMAT;
    BOOL headerRead = FALSE;
    if (sizeVerified && fileSize.QuadPart >= static_cast<LONGLONG>(sizeof(dumpHeader))) {
        LARGE_INTEGER start = {};
        if (SetFilePointerEx(hDumpFile, start, NULL, FILE_BEGIN)) {
            headerRead = ReadFile(hDumpFile, &dumpHeader, sizeof(dumpHeader), &headerBytesRead, NULL);
            headerError = headerRead ? ERROR_SUCCESS : GetLastError();
        } else {
            headerError = GetLastError();
        }
    }
    result.headerVerified = headerRead &&
                            headerBytesRead == sizeof(dumpHeader) &&
                            dumpHeader.Signature == MINIDUMP_SIGNATURE;
    if (headerRead && !result.headerVerified) {
        headerError = ERROR_BAD_FORMAT;
    } else if (headerRead && headerBytesRead != sizeof(dumpHeader)) {
        headerError = ERROR_BAD_FORMAT;
    }

    const BOOL closed = CloseHandle(hDumpFile);
    const DWORD closeError = closed ? ERROR_SUCCESS : GetLastError();

    if (!dumpWritten) {
        result.error = dumpError;
    } else if (!flushed) {
        result.error = flushError;
    } else if (!sizeVerified) {
        result.error = sizeError;
    } else if (fileSize.QuadPart <= 0) {
        result.error = ERROR_FILE_INVALID;
    } else if (!result.headerVerified) {
        result.error = headerError;
    } else if (!closed) {
        result.error = closeError;
    } else {
        result.written = true;
        result.error = ERROR_SUCCESS;
    }
    return result;
}

static DWORD WINAPI MiniDumpWorkerThreadProc(LPVOID) {
    for (;;) {
        if (WaitForSingleObject(s_dumpRequestEvent, INFINITE) != WAIT_OBJECT_0) {
            return 0;
        }
        s_dumpWorkerResult = WriteMiniDump(
            &s_dumpExceptionPointers, s_dumpExceptionThreadId, s_dumpWorkerThreadId);
        MemoryBarrier();
        if (!SetEvent(s_dumpCompleteEvent)) {
            return 0;
        }
    }
}

static DWORD InitializeMiniDumpWorker() {
    if (s_dumpWorkerThread && s_dumpRequestEvent && s_dumpCompleteEvent) {
        return ERROR_SUCCESS;
    }

    s_dumpRequestEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!s_dumpRequestEvent) {
        return GetLastError();
    }

    s_dumpCompleteEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!s_dumpCompleteEvent) {
        const DWORD error = GetLastError();
        CloseHandle(s_dumpRequestEvent);
        s_dumpRequestEvent = nullptr;
        return error;
    }

    s_dumpWorkerThread = CreateThread(
        nullptr, 0, MiniDumpWorkerThreadProc, nullptr, 0, &s_dumpWorkerThreadId);
    if (!s_dumpWorkerThread) {
        const DWORD error = GetLastError();
        CloseHandle(s_dumpCompleteEvent);
        CloseHandle(s_dumpRequestEvent);
        s_dumpCompleteEvent = nullptr;
        s_dumpRequestEvent = nullptr;
        s_dumpWorkerThreadId = 0;
        return error;
    }
    return ERROR_SUCCESS;
}

static MiniDumpResult WriteMiniDumpOnWorker(EXCEPTION_POINTERS* pExceptionInfo) {
    MiniDumpResult result;
    result.attempted = true;

    if (!pExceptionInfo || !pExceptionInfo->ExceptionRecord) {
        result.error = ERROR_INVALID_PARAMETER;
        return result;
    }
    if (!RefreshCrashDumpPath()) {
        result.error = ERROR_PATH_NOT_FOUND;
        return result;
    }
    if (!s_dumpWorkerThread || !s_dumpRequestEvent || !s_dumpCompleteEvent) {
        result.error = ERROR_NOT_READY;
        return result;
    }

    s_dumpExceptionRecord = *pExceptionInfo->ExceptionRecord;
    s_dumpExceptionRecord.ExceptionRecord = nullptr;
    s_dumpExceptionPointers.ExceptionRecord = &s_dumpExceptionRecord;
    if (pExceptionInfo->ContextRecord) {
        s_dumpExceptionContext = *pExceptionInfo->ContextRecord;
        s_dumpExceptionPointers.ContextRecord = &s_dumpExceptionContext;
    } else {
        s_dumpExceptionPointers.ContextRecord = nullptr;
    }
    s_dumpExceptionThreadId = GetCurrentThreadId();

    if (!ResetEvent(s_dumpCompleteEvent) || !SetEvent(s_dumpRequestEvent)) {
        result.error = GetLastError();
        return result;
    }

    HANDLE waitHandles[] = { s_dumpCompleteEvent, s_dumpWorkerThread };
    const DWORD waitResult = WaitForMultipleObjects(
        2, waitHandles, FALSE, kDumpWorkerTimeoutMs);
    if (waitResult == WAIT_OBJECT_0) {
        MemoryBarrier();
        return s_dumpWorkerResult;
    }
    if (waitResult == WAIT_OBJECT_0 + 1) {
        result.error = ERROR_PROCESS_ABORTED;
    } else if (waitResult == WAIT_TIMEOUT) {
        result.error = ERROR_TIMEOUT;
    } else {
        result.error = GetLastError();
    }
    return result;
}

static const char* GetFileVersionText(const char* path, char* output, size_t capacity) {
    if (!output || capacity == 0) return "<нет данных>";
    output[0] = '\0';
    if (!path || !path[0]) {
        snprintf(output, capacity, "<путь недоступен>");
        return output;
    }

    DWORD ignored = 0;
    const DWORD versionSize = GetFileVersionInfoSizeA(path, &ignored);
    if (versionSize == 0) {
        snprintf(output, capacity, "<нет версии>");
        return output;
    }
    if (versionSize > sizeof(s_fileVersionStorage)) {
        snprintf(output, capacity, "<ресурс версии слишком велик>");
        return output;
    }
    if (!GetFileVersionInfoA(path, 0, versionSize, s_fileVersionStorage)) {
        snprintf(output, capacity, "<ошибка чтения: %lu>", GetLastError());
        return output;
    }

    VS_FIXEDFILEINFO* fixedInfo = nullptr;
    UINT fixedInfoSize = 0;
    if (!VerQueryValueA(s_fileVersionStorage, "\\",
                        reinterpret_cast<LPVOID*>(&fixedInfo), &fixedInfoSize) ||
        !fixedInfo || fixedInfoSize < sizeof(VS_FIXEDFILEINFO) ||
        fixedInfo->dwSignature != 0xFEEF04BD) {
        snprintf(output, capacity, "<версия не определена>");
        return output;
    }

    snprintf(output, capacity, "%u.%u.%u.%u",
             HIWORD(fixedInfo->dwFileVersionMS), LOWORD(fixedInfo->dwFileVersionMS),
             HIWORD(fixedInfo->dwFileVersionLS), LOWORD(fixedInfo->dwFileVersionLS));
    return output;
}

static void AppendLoadedModuleVersions(FixedTextBuffer& output) {
    output.Append("[Версии загруженных модулей]\n");
    HANDLE snapshot = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 8; ++attempt) {
        snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
        if (snapshot != INVALID_HANDLE_VALUE || GetLastError() != ERROR_BAD_LENGTH) break;
    }
    if (snapshot == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        output.AppendFormat("Список модулей недоступен (ошибка %lu).\n", error);
        return;
    }

    MODULEENTRY32 module = {};
    module.dwSize = sizeof(module);
    if (Module32First(snapshot, &module)) {
        do {
            char version[64] = {};
            GetFileVersionText(module.szExePath, version, sizeof(version));
            output.AppendFormat("%s | %s | %s\n", module.szModule, version, module.szExePath);
        } while (Module32Next(snapshot, &module));
    } else {
        output.AppendFormat("Список модулей пуст или недоступен (ошибка %lu).\n", GetLastError());
    }
    CloseHandle(snapshot);
}

static void BuildRuntimeContext(FixedTextBuffer& output, const ReportFileTargets& reportFiles,
                                bool reportEnabled, const char* dumpStatus,
                                const char* dumpPath, DWORD processId, DWORD threadId) {
    output.Reset();
    output.AppendFormat("[Версия G3O] %s\n[Сборка G3O] %s; %s\nPID: %lu\nTID: %lu\n",
                        G3O_VERSION_STRING, G3O_BUILD_CONFIGURATION, G3O_BUILD_TIMESTAMP,
                        processId, threadId);
    output.Append("[Путь отчёта] ");
    if (!reportEnabled) {
        output.Append("файловая запись отключена (WriteCrashLog=false)\n");
    } else {
        output.Append(reportFiles.reportPath[0] ? reportFiles.reportPath : "не удалось создать файл в G3O");
        output.Append(reportFiles.reportFile != INVALID_HANDLE_VALUE ? " (файл создан)\n" : "\n");
    }
    output.AppendFormat("[Статус дампа] %s\n[Путь дампа] %s\n",
                        dumpStatus ? dumpStatus : "неизвестно",
                        dumpPath && dumpPath[0] ? dumpPath : "не создавался");

    DWORD_PTR processAffinity = 0;
    DWORD_PTR systemAffinity = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &processAffinity, &systemAffinity)) {
        output.AppendFormat("[Фактическая маска процессора] 0x%llX (доступно системе: 0x%llX)\n",
                            static_cast<unsigned long long>(processAffinity),
                            static_cast<unsigned long long>(systemAffinity));
    } else {
        output.AppendFormat("[Фактическая маска процессора] недоступна (ошибка %lu)\n", GetLastError());
    }

    char configStorage[8192] = {};
    FixedTextBuffer configSnapshot(configStorage, sizeof(configStorage));
    AppendAppliedConfigSnapshot(configSnapshot, GetConfig());
    output.Append(configSnapshot.CStr());
    output.Append("\n");
    AppendLoadedModuleVersions(output);
    output.Finish();
}

static void BuildReportText(FixedTextBuffer& outReport, EXCEPTION_POINTERS* pExceptionInfo,
                            const BlameResult& blame, const StackFrameInfo* stackFrames,
                            size_t stackFrameCount, const char* recentEvents,
                            const char* runtimeContext) {
    char buf[1024];
    outReport.clear();

    SYSTEMTIME st;
    GetLocalTime(&st);

    snprintf(buf, sizeof(buf),
             "================================================================================\n"
             "                 Gothic 3 Optima (G3O) — Отчёт о сбое (Crash Report)             \n"
             "                 Дата и время: %04d-%02d-%02d %02d:%02d:%02d                      \n"
             "================================================================================\n\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    outReport += buf;

    outReport += runtimeContext ? runtimeContext : "[Контекст отчёта недоступен]\n";
    outReport += "\n";

    // 1. Diagnosis Summary
    outReport += "--------------------------------------------------------------------------------\n";
    outReport += "[ДИАГНОЗ И ВИНОВНИК СБОЯ]\n";
    outReport += "--------------------------------------------------------------------------------\n";
    snprintf(buf, sizeof(buf), "Категория:      %s\n", blame.category); outReport += buf;
    snprintf(buf, sizeof(buf), "Модуль:         %s\n", blame.moduleName); outReport += buf;
    snprintf(buf, sizeof(buf), "Путь к файлу:   %s\n", blame.modulePath); outReport += buf;
    snprintf(buf, sizeof(buf), "Адрес ошибки:   0x%p (смещение в модуле: +0x%X)\n",
             (void*)pExceptionInfo->ExceptionRecord->ExceptionAddress, (unsigned int)blame.rva); outReport += buf;
    snprintf(buf, sizeof(buf), "Код сбоя:       0x%08X — %s\n",
             pExceptionInfo->ExceptionRecord->ExceptionCode,
             GetExceptionCodeString(pExceptionInfo->ExceptionRecord->ExceptionCode)); outReport += buf;

    if (pExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        pExceptionInfo->ExceptionRecord->NumberParameters >= 2) {
        ULONG_PTR accessType = pExceptionInfo->ExceptionRecord->ExceptionInformation[0];
        ULONG_PTR targetAddr = pExceptionInfo->ExceptionRecord->ExceptionInformation[1];
        const char* typeStr = (accessType == 0) ? "чтения (Read)" : (accessType == 1) ? "записи (Write)" : "выполнения (Execute)";
        snprintf(buf, sizeof(buf), "Операция:       Попытка %s по недопустимому адресу 0x%p %s\n",
                 typeStr, (void*)targetAddr, (targetAddr < 0x10000) ? "(NULL pointer dereference!)" : "");
        outReport += buf;
    }

    snprintf(buf, sizeof(buf), "\nОписание проблемы:\n  %s\n", blame.description); outReport += buf;
    snprintf(buf, sizeof(buf), "\nРЕКОМЕНДАЦИЯ ПО ИСПРАВЛЕНИЮ:\n  %s\n\n", blame.recommendation); outReport += buf;

    // Check if an indirect initiator was found in stack frames
    char initiatorMod[MAX_PATH] = {};
    for (size_t i = 1; i < stackFrameCount; ++i) {
        if (stackFrames[i].isScriptMod) {
            CopyText(initiatorMod, sizeof(initiatorMod), stackFrames[i].moduleName);
            break;
        }
    }
    if (initiatorMod[0] != 0 && strcmp(blame.moduleName, initiatorMod) != 0) {
        snprintf(buf, sizeof(buf),
                 "(!) ОБРАТИТЕ ВНИМАНИЕ:\n"
                 "    Хотя сбой произошёл в '%s', вызов в стек пришёл из скриптового мода '%s'!\n"
                 "    Проверьте именно мод '%s'.\n\n",
                 blame.moduleName, initiatorMod, initiatorMod);
        outReport += buf;
    }

    // 2. Memory & Virtual Address Space status (Gothic 3 32-bit VAS check!)
    MEMORYSTATUSEX memStatus = { sizeof(MEMORYSTATUSEX) };
    if (GlobalMemoryStatusEx(&memStatus)) {
        PROCESS_MEMORY_COUNTERS_EX pmc = { sizeof(PROCESS_MEMORY_COUNTERS_EX) };
        GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc));

        outReport += "--------------------------------------------------------------------------------\n";
        outReport += "[СОСТОЯНИЕ ПАМЯТИ]\n";
        outReport += "--------------------------------------------------------------------------------\n";
        snprintf(buf, sizeof(buf), "Физическая RAM:  Всего: %llu МБ, Свободно: %llu МБ\n",
                 memStatus.ullTotalPhys / (1024 * 1024), memStatus.ullAvailPhys / (1024 * 1024));
        outReport += buf;
        snprintf(buf, sizeof(buf), "Виртуальная память процесса (VAS): %llu МБ / %llu МБ (Занято: %llu МБ)\n",
                 (memStatus.ullTotalVirtual - memStatus.ullAvailVirtual) / (1024 * 1024),
                 memStatus.ullTotalVirtual / (1024 * 1024),
                 (unsigned long long)(pmc.PrivateUsage / (1024 * 1024)));
        outReport += buf;

        unsigned long long vasUsedMB = (memStatus.ullTotalVirtual - memStatus.ullAvailVirtual) / (1024 * 1024);
        if (vasUsedMB > 3600) {
            outReport += "\n(!!!) ВНИМАНИЕ: Виртуальная память процесса превышает 3.6 ГБ!\n";
            outReport += "      В 32-битных приложениях лимит равен 4 ГБ. Процесс исчерпал адресное пространство.\n";
            outReport += "      Это классический вылет Gothic 3 из-за утечки памяти или экстремальной дальности прорисовки.\n";
        }
        outReport += "\n";
    }

    // 3. CPU Registers
    if (pExceptionInfo->ContextRecord) {
        CONTEXT* c = pExceptionInfo->ContextRecord;
        outReport += "--------------------------------------------------------------------------------\n";
        outReport += "[РЕГИСТРЫ ПРОЦЕССОРА (x86)]\n";
        outReport += "--------------------------------------------------------------------------------\n";
        snprintf(buf, sizeof(buf),
                 "EAX = 0x%08X  EBX = 0x%08X  ECX = 0x%08X  EDX = 0x%08X\n"
                 "ESI = 0x%08X  EDI = 0x%08X  EBP = 0x%08X  ESP = 0x%08X\n"
                 "EIP = 0x%08X  EFLAGS = 0x%08X\n\n",
                 c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp, c->Eip, c->EFlags);
        outReport += buf;
    }

    // 4. Call Stack Trace
    outReport += "--------------------------------------------------------------------------------\n";
    outReport += "[СТЕК ВЫЗОВОВ (CALL STACK)]\n";
    outReport += "--------------------------------------------------------------------------------\n";
    if (stackFrameCount == 0) {
        outReport += "Стек вызовов недоступен.\n";
    } else {
        for (size_t i = 0; i < stackFrameCount; ++i) {
            const auto& frame = stackFrames[i];
            snprintf(buf, sizeof(buf), "#%02d 0x%p [%s+0x%X]",
                     (int)i, (void*)frame.address, frame.moduleName, (unsigned int)frame.rva);
            outReport += buf;
            if (frame.symbolName[0] != 0) {
                snprintf(buf, sizeof(buf), " %s", frame.symbolName);
                outReport += buf;
            }
            if (frame.sourceFile[0] != 0) {
                snprintf(buf, sizeof(buf), " (%s:%d)", frame.sourceFile, frame.lineNumber);
                outReport += buf;
            }
            if (frame.isScriptMod) {
                outReport += "  <-- [Скриптовый мод]";
            }
            outReport += "\n";
        }
    }
    outReport += "\n";

    // 5. Recent Diagnostic Logs
    outReport += "--------------------------------------------------------------------------------\n";
    outReport += "[ПОСЛЕДНИЕ СОБЫТИЯ ПЕРЕД СБОЕМ (ЗАГРУЗКА / GURU / G3O)]\n";
    outReport += "--------------------------------------------------------------------------------\n";
    if (!recentEvents || recentEvents[0] == '\\0') {
        outReport += "Записей в логе нет.\n";
    } else {
        outReport += recentEvents;
    }
    outReport += "\n";

    outReport += "\n================================================================================\n";
    outReport.Finish();
}

static bool SaveReportToFile(const char* report, ReportFileTargets& files) {
    if (!report) {
        CloseReportFiles(files);
        return false;
    }
    const bool saved = WriteReportToHandle(files.reportFile, report);
    CloseReportFiles(files);
    return saved;
}

static LONG HandleCrash(EXCEPTION_POINTERS* pExceptionInfo) {
    if (!pExceptionInfo || !pExceptionInfo->ExceptionRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (InterlockedCompareExchange(&s_reportPipelineActive, 1, 0) != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    struct ReportPipelineGuard {
        ~ReportPipelineGuard() { InterlockedExchange(&s_reportPipelineActive, 0); }
    } reportPipelineGuard;

    const Config& config = GetConfig();
    ReportFileTargets reportFiles;
    s_dumpPath[0] = '\0';

    // Preserve the dump before symbolization, logging, or report formatting can fail.
    MiniDumpResult dumpResult;
    if (config.writeCrashDump) {
        dumpResult = WriteMiniDumpOnWorker(pExceptionInfo);
    }

    char dumpStatus[256] = {};
    if (!config.writeCrashDump) {
        snprintf(dumpStatus, sizeof(dumpStatus), "отключён настройкой WriteCrashDump=false");
    } else if (dumpResult.written) {
        snprintf(dumpStatus, sizeof(dumpStatus),
                 "записан; размер %llu байт; сигнатура MDMP проверена (полная целостность не проверялась)",
                 dumpResult.sizeBytes);
    } else {
        snprintf(dumpStatus, sizeof(dumpStatus),
                 "не создан или не прошёл проверку; ошибка 0x%08lX; размер %llu байт",
                 static_cast<unsigned long>(dumpResult.error), dumpResult.sizeBytes);
    }

    reportFiles = ReserveReportFiles(config.writeCrashLog);
    CopyRecentLogs(s_crashEventSnapshot, sizeof(s_crashEventSnapshot));

    uintptr_t faultAddr = (uintptr_t)pExceptionInfo->ExceptionRecord->ExceptionAddress;
    AnalyzeAddress(faultAddr, s_crashBlame);
    const size_t stackFrameCount = WalkCallStack(
        pExceptionInfo->ContextRecord, s_crashStackFrames, kMaxStackFrames);

    FixedTextBuffer runtimeContext(s_runtimeContextStorage, sizeof(s_runtimeContextStorage));
    BuildRuntimeContext(runtimeContext, reportFiles, config.writeCrashLog, dumpStatus, s_dumpPath,
                        GetCurrentProcessId(), GetCurrentThreadId());
    FixedTextBuffer report(s_crashReportStorage, sizeof(s_crashReportStorage));
    BuildReportText(report, pExceptionInfo, s_crashBlame, s_crashStackFrames,
                    stackFrameCount, s_crashEventSnapshot, runtimeContext.CStr());
    const bool reportSaved = config.writeCrashLog
        ? SaveReportToFile(report.CStr(), reportFiles)
        : false;

    if (config.showCrashDialog) {
        snprintf(s_crashDialogMessage, sizeof(s_crashDialogMessage),
                 "Gothic 3 Optima — Обнаружен критический сбой!\n\n"
                 "Виновник сбоя: %s\n"
                 "Категория:     %s\n"
                 "Адрес сбоя:    0x%p (смещение +0x%X)\n"
                 "Код ошибки:    0x%08X\n\n"
                 "Описание:\n%s\n\n"
                 "Рекомендация:\n%s\n\n"
                 "Отчёт: %s",
                 s_crashBlame.moduleName,
                 s_crashBlame.category,
                 (void*)faultAddr, (unsigned int)s_crashBlame.rva,
                 pExceptionInfo->ExceptionRecord->ExceptionCode,
                 s_crashBlame.description,
                 s_crashBlame.recommendation,
                 reportSaved ? s_crashLogPath :
                     (config.writeCrashLog ? "не удалось записать файл отчёта" : "запись файла отчёта отключена"));

        int wideLen = MultiByteToWideChar(CP_UTF8, 0, s_crashDialogMessage, -1, nullptr, 0);
        if (wideLen > 0 && wideLen <= static_cast<int>(sizeof(s_crashDialogWideMessage) / sizeof(s_crashDialogWideMessage[0]))) {
            const int converted = MultiByteToWideChar(
                CP_UTF8, 0, s_crashDialogMessage, -1, s_crashDialogWideMessage,
                static_cast<int>(sizeof(s_crashDialogWideMessage) / sizeof(s_crashDialogWideMessage[0])));
            if (converted > 0) {
                MessageBoxW(NULL, s_crashDialogWideMessage, L"Gothic 3 Optima — Crash Investigator", MB_OK | MB_ICONERROR | MB_SYSTEMMODAL);
            } else {
                MessageBoxA(NULL, s_crashDialogMessage, "Gothic 3 Optima — Crash Investigator", MB_OK | MB_ICONERROR | MB_SYSTEMMODAL);
            }
        } else {
            MessageBoxA(NULL, s_crashDialogMessage, "Gothic 3 Optima — Crash Investigator", MB_OK | MB_ICONERROR | MB_SYSTEMMODAL);
        }
    }

    // Bypass the logging hook to avoid re-entering the logger during a crash.
    OutputCrashReportToDebugger(report.CStr());

    return EXCEPTION_EXECUTE_HANDLER;
}

static LONG WINAPI G3OUnhandledCrashFilter(EXCEPTION_POINTERS* pExceptionInfo) {
    HandleCrash(pExceptionInfo);
    if (s_previousFilter) {
        return s_previousFilter(pExceptionInfo);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

static void InitializeSymbolHandler() {
    HANDLE currentProcess = GetCurrentProcess();
    HANDLE symbolProcess = nullptr;
    if (!DuplicateHandle(currentProcess, currentProcess, currentProcess, &symbolProcess,
                         0, FALSE, DUPLICATE_SAME_ACCESS)) {
        Log("Crash", "DbgHelp symbol session unavailable: DuplicateHandle failed (%lu).", GetLastError());
        return;
    }

    DbgHelpLockGuard dbgHelpLock;
    if (!dbgHelpLock.acquired()) {
        CloseHandle(symbolProcess);
        Log("Crash", "DbgHelp symbol session unavailable: another DbgHelp call is active.");
        return;
    }
    HMODULE hDbgHelp = GetModuleHandleA("dbghelp.dll");
    s_symRefreshModuleList = hDbgHelp
        ? reinterpret_cast<SymRefreshModuleList_t>(GetProcAddress(hDbgHelp, "SymRefreshModuleList"))
        : nullptr;
    if (!SymInitialize(symbolProcess, nullptr, TRUE)) {
        const DWORD error = GetLastError();
        CloseHandle(symbolProcess);
        Log("Crash", "DbgHelp symbol session unavailable: SymInitialize failed (%lu).", error);
        return;
    }

    s_symbolProcess = symbolProcess;
    InterlockedExchange(&s_symbolHandlerReady, 1);
}

static void ShutdownSymbolHandler() {
    DbgHelpLockGuard dbgHelpLock;
    if (!dbgHelpLock.acquired()) return;

    if (InterlockedExchange(&s_symbolHandlerReady, 0) != 0 && s_symbolProcess) {
        SymCleanup(s_symbolProcess);
    }
    if (s_symbolProcess) {
        CloseHandle(s_symbolProcess);
        s_symbolProcess = nullptr;
    }
    s_symRefreshModuleList = nullptr;
}

static LONG WINAPI G3OVectoredExceptionHandler(PEXCEPTION_POINTERS pExceptionInfo) {
    if (!pExceptionInfo || !pExceptionInfo->ExceptionRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    DWORD code = pExceptionInfo->ExceptionRecord->ExceptionCode;

    // Filter out normal, handled, or debug exceptions
    if (code == 0xE06D7363 || // MSVC C++ Exception
        code == 0x406D1388 || // SetThreadName exception
        code == EXCEPTION_BREAKPOINT ||
        code == EXCEPTION_SINGLE_STEP ||
        code < 0xC0000000) {  // Informational / Warnings
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Only catch catastrophic unhandled hardware exceptions if already in second-chance
    // or if stack overflow / severe access violation
    if (code == EXCEPTION_ACCESS_VIOLATION ||
        code == EXCEPTION_ILLEGAL_INSTRUCTION ||
        code == EXCEPTION_STACK_OVERFLOW ||
        code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
        code == EXCEPTION_IN_PAGE_ERROR) {
        // Let normal filters run first unless this is a recurring exception
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

static LPTOP_LEVEL_EXCEPTION_FILTER WINAPI HookedSetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER lpTopLevelExceptionFilter) {
    if (lpTopLevelExceptionFilter != G3OUnhandledCrashFilter) {
        s_previousFilter = lpTopLevelExceptionFilter;
    }
    return s_previousFilter;
}

void InitializeCrashHandler() {
    DetermineCrashPaths();
    FlushDiagnosticLog();
    InitializeSymbolHandler();

    // Start the dedicated thread before installing the crash filter.
    if (GetConfig().writeCrashDump) {
        const DWORD workerError = InitializeMiniDumpWorker();
        if (workerError != ERROR_SUCCESS) {
            Log("Crash", "Minidump worker unavailable (%lu).", workerError);
        }
    }

    // 1. Install top-level unhandled exception filter
    s_previousFilter = SetUnhandledExceptionFilter(G3OUnhandledCrashFilter);

    // 2. Hook SetUnhandledExceptionFilter so neither Gothic 3 nor other mods can silently overwrite us
    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    if (hKernel32) {
        void* pSUEF = (void*)GetProcAddress(hKernel32, "SetUnhandledExceptionFilter");
        if (pSUEF) {
            MH_CreateHook(pSUEF, (void*)&HookedSetUnhandledExceptionFilter, (void**)&s_origSetUnhandledExceptionFilter);
            MH_EnableHook(pSUEF);
        }
    }

    // 3. Register VEH as secondary watchdog
    s_vehHandle = AddVectoredExceptionHandler(0, G3OVectoredExceptionHandler);

    Log("G3O", "Crash reports and dumps are written under '%s'.", s_crashLogDirectory);
}

void ShutdownCrashHandler() {
    if (s_vehHandle) {
        RemoveVectoredExceptionHandler(s_vehHandle);
        s_vehHandle = nullptr;
    }
    if (s_origSetUnhandledExceptionFilter) {
        MH_DisableHook((void*)SetUnhandledExceptionFilter);
    }
    // The dump worker is process-lifetime; joining it from DllMain can deadlock.
    ShutdownSymbolHandler();
}

// -----------------------------------------------------------------------------
// Engine-level Fatal Error hooks (SharedBase.dll bCErrorAdmin)
// -----------------------------------------------------------------------------
typedef void (__thiscall *CallFatalError1_t)(void* pThis, const char* module, const char* file, int line);
static CallFatalError1_t s_origCallFatalError1 = nullptr;

typedef void (__thiscall *CallFatalError2_t)(void* pThis, const char* module, const char* description, const char* file, int line);
static CallFatalError2_t s_origCallFatalError2 = nullptr;

typedef int (__thiscall *CallExceptionError_t)(void* pThis, void* pExceptionPointers);
static CallExceptionError_t s_origCallExceptionError = nullptr;

static int __fastcall HookedCallExceptionError(void* pThis, void* /*edx*/, void* pExceptionPointers) {
    if (pExceptionPointers) {
        EXCEPTION_POINTERS* pEP = (EXCEPTION_POINTERS*)pExceptionPointers;
        HandleCrash(pEP);
    }
    if (s_origCallExceptionError) {
        return s_origCallExceptionError(pThis, pExceptionPointers);
    }
    return 1;
}

static void __fastcall HookedCallFatalError1(void* pThis, void* /*edx*/, const char* module, const char* file, int line) {
    ReportEngineFatalError(module, nullptr, file, line);

    if (s_origCallFatalError1) {
        s_origCallFatalError1(pThis, module, file, line);
    }
}

static void __fastcall HookedCallFatalError2(
    void* pThis, void* /*edx*/, const char* module, const char* description, const char* file, int line) {
    ReportEngineFatalError(module, description, file, line);

    if (s_origCallFatalError2) {
        s_origCallFatalError2(pThis, module, description, file, line);
    }
}

void HookEngineFatalErrors() {
    HMODULE hSharedBase = GetModuleHandleA("SharedBase.dll");
    if (!hSharedBase) return;

    void* pFatal1 = (void*)GetProcAddress(hSharedBase, "?CallFatalError@bCErrorAdmin@@QAEXPBD0H@Z");
    if (pFatal1) {
        MH_CreateHook(pFatal1, (void*)&HookedCallFatalError1, (void**)&s_origCallFatalError1);
        MH_EnableHook(pFatal1);
    }

    void* pFatal2 = (void*)GetProcAddress(hSharedBase, "?CallFatalError@bCErrorAdmin@@QAEXPBD00H@Z");
    if (pFatal2) {
        MH_CreateHook(pFatal2, (void*)&HookedCallFatalError2, (void**)&s_origCallFatalError2);
        MH_EnableHook(pFatal2);
    }

    void* pExceptionError = (void*)GetProcAddress(hSharedBase, "?CallExceptionError@bCErrorAdmin@@QAEHPAUbSException_Pointers@@@Z");
    if (pExceptionError) {
        MH_CreateHook(pExceptionError, (void*)&HookedCallExceptionError, (void**)&s_origCallExceptionError);
        MH_EnableHook(pExceptionError);
        Log("G3O", "Guru interceptor hook active (?CallExceptionError@bCErrorAdmin).");
    }

    Log("G3O", "Engine Fatal Error and Guru hooks installed in SharedBase.dll");
}

static void CaptureFatalCaller(FixedTextBuffer& callerFunction, FixedTextBuffer& stackTrace) {
    callerFunction.Reset();
    stackTrace.Reset();
    CONTEXT context = {};
    RtlCaptureContext(&context);
    const size_t frameCount = WalkCallStack(&context, s_engineFatalStackFrames, kMaxStackFrames);

    for (size_t i = 0; i < frameCount; ++i) {
        const StackFrameInfo& frame = s_engineFatalStackFrames[i];
        stackTrace.AppendFormat("#%02u 0x%p [%s+0x%X]%s%s\n",
                                static_cast<unsigned>(i), (void*)frame.address, frame.moduleName,
                                static_cast<unsigned>(frame.rva), frame.symbolName[0] == 0 ? "" : " ",
                                frame.symbolName);

        if (callerFunction.Size() == 0 && frame.moduleName[0] != 0 &&
            frame.moduleName[0] != '<' && _stricmp(frame.moduleName, "dinput8.dll") != 0) {
            if (frame.symbolName[0] != 0) {
                callerFunction.AppendFormat("%s!%s", frame.moduleName, frame.symbolName);
            } else {
                callerFunction.AppendFormat("%s+0x%X", frame.moduleName,
                                            static_cast<unsigned>(frame.rva));
            }
        }
    }

    if (frameCount == 0) stackTrace.Append("<стек недоступен>\n");
    if (callerFunction.Size() == 0) callerFunction.Append("<не удалось определить по стеку>");
    callerFunction.Finish();
    stackTrace.Finish();
}

void ReportEngineFatalError(const char* module, const char* message, const char* file, int line) {
    if (InterlockedCompareExchange(&s_reportPipelineActive, 1, 0) != 0) {
        return;
    }
    struct ReportPipelineGuard {
        ~ReportPipelineGuard() { InterlockedExchange(&s_reportPipelineActive, 0); }
    } reportPipelineGuard;

    const Config& config = GetConfig();
    ReportFileTargets reportFiles = ReserveReportFiles(config.writeCrashLog);

    CopyRecentLogs(s_engineFatalEventSnapshot, sizeof(s_engineFatalEventSnapshot));

    SYSTEMTIME st;
    GetLocalTime(&st);
    char timestamp[40];
    snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    FixedTextBuffer callerFunction(s_engineFatalCallerStorage, sizeof(s_engineFatalCallerStorage));
    FixedTextBuffer stackTrace(s_engineFatalStackStorage, sizeof(s_engineFatalStackStorage));
    CaptureFatalCaller(callerFunction, stackTrace);

    FixedTextBuffer runtimeContext(s_runtimeContextStorage, sizeof(s_runtimeContextStorage));
    BuildRuntimeContext(runtimeContext, reportFiles, config.writeCrashLog,
                        "не создавался: движковая FatalError не передаёт EXCEPTION_POINTERS",
                        nullptr, GetCurrentProcessId(), GetCurrentThreadId());
    FixedTextBuffer report(s_engineFatalReportStorage, sizeof(s_engineFatalReportStorage));
    BuildEngineFatalReport(report, timestamp, module, message, callerFunction.CStr(), file, line,
                           stackTrace.CStr(), s_engineFatalEventSnapshot,
                           GetCurrentProcessId(), GetCurrentThreadId(), runtimeContext.CStr());
    if (config.writeCrashLog) {
        SaveReportToFile(report.CStr(), reportFiles);
    } else {
        CloseReportFiles(reportFiles);
    }
}

} // namespace G3O
