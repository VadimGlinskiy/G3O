#include "crash_handler.h"
#include "diagnostic_logger.h"
#include "config_manager.h"
#include <minhook.h>
#include <dbghelp.h>
#include <psapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")

namespace G3O {

static char s_crashLogPath[MAX_PATH] = { 0 };
static char s_docCrashLogPath[MAX_PATH] = { 0 };
static char s_dumpPath[MAX_PATH] = { 0 };
static volatile LONG s_crashHandlingActive = 0;

typedef LPTOP_LEVEL_EXCEPTION_FILTER (WINAPI *SetUnhandledExceptionFilter_t)(LPTOP_LEVEL_EXCEPTION_FILTER);
static SetUnhandledExceptionFilter_t s_origSetUnhandledExceptionFilter = nullptr;
static LPTOP_LEVEL_EXCEPTION_FILTER s_previousFilter = nullptr;
static PVOID s_vehHandle = nullptr;

struct BlameResult {
    std::string moduleName;
    std::string modulePath;
    uintptr_t baseAddress = 0;
    uintptr_t rva = 0;
    std::string category;
    std::string description;
    std::string recommendation;
    bool isKnownCulprit = false;
};

static void DetermineCrashPaths() {
    char exePath[MAX_PATH] = { 0 };
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    char* lastSlash = strrchr(exePath, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';

    snprintf(s_crashLogPath, sizeof(s_crashLogPath), "%sG3O_CrashReport.log", exePath);

    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(s_dumpPath, sizeof(s_dumpPath), "%sG3O_Crash_%04d%02d%02d_%02d%02d%02d.dmp",
             exePath, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    char docPath[MAX_PATH] = { 0 };
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, SHGFP_TYPE_CURRENT, docPath))) {
        snprintf(s_docCrashLogPath, sizeof(s_docCrashLogPath), "%s\\Gothic3\\G3O_CrashReport.log", docPath);
    }
}

const char* GetCrashReportPath() {
    if (s_crashLogPath[0] == '\0') DetermineCrashPaths();
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

static std::string ToLower(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) { return (char)::tolower(c); });
    return str;
}

static BlameResult AnalyzeAddress(uintptr_t address) {
    BlameResult result;
    result.baseAddress = 0;
    result.rva = 0;

    MEMORY_BASIC_INFORMATION mbi = { 0 };
    if (VirtualQuery((LPCVOID)address, &mbi, sizeof(mbi)) && mbi.AllocationBase) {
        HMODULE hMod = (HMODULE)mbi.AllocationBase;
        char modPath[MAX_PATH] = { 0 };
        if (GetModuleFileNameA(hMod, modPath, MAX_PATH)) {
            result.modulePath = modPath;
            result.baseAddress = (uintptr_t)hMod;
            result.rva = address - result.baseAddress;

            const char* slash = strrchr(modPath, '\\');
            result.moduleName = slash ? (slash + 1) : modPath;
        }
    }

    if (result.moduleName.empty()) {
        result.moduleName = "<UNKNOWN MEMORY>";
        result.category = "Неизвестная область памяти / Сбой указателя";
        result.description = "Адрес сбоя не принадлежит ни одной загруженной библиотеке DLL (возможно, вызов по нулевому указателю или повреждённой таблице vtable).";
        result.recommendation = "Проверьте целостность файлов игры и порядок загрузки модов.";
        return result;
    }

    std::string lowerName = ToLower(result.moduleName);
    std::string lowerPath = ToLower(result.modulePath);

    // 1. External Screen Capture & Overlays
    if (lowerName.find("bdcam") != std::string::npos) {
        result.category = "Внешний оверлей (Bandicam Video Capture)";
        result.description = "Хук оверлея Bandicam (bdcamvk32.dll / bdcam32.dll) конфликтует с трансляцией Vulkan/DXVK.";
        result.recommendation = "Отключите функцию оверлея/захвата Vulkan в настройках Bandicam или отключите Bandicam перед запуском игры.";
        result.isKnownCulprit = true;
    } else if (lowerName.find("rtss") != std::string::npos) {
        result.category = "Внешний оверлей (RivaTuner Statistics Server / MSI Afterburner)";
        result.description = "Оверлей RTSS конфликтует с графическим пайплайном игры.";
        result.recommendation = "Добавьте Gothic3.exe в список RTSS и установите Application Detection Level в 'None' или 'Low'.";
        result.isKnownCulprit = true;
    } else if (lowerName.find("discord") != std::string::npos) {
        result.category = "Внешний оверлей (Discord In-Game Overlay)";
        result.description = "Хук оверлея Discord вызвал сбой в графическом контексте.";
        result.recommendation = "Отключите внутриигровой оверлей в настройках Discord.";
        result.isKnownCulprit = true;
    } else if (lowerName.find("gameoverlayrenderer") != std::string::npos) {
        result.category = "Внешний оверлей (Steam Community Overlay)";
        result.description = "Сбой в модуле оверлея Steam.";
        result.recommendation = "Отключите оверлей Steam в свойствах Gothic 3 в библиотеке Steam.";
        result.isKnownCulprit = true;
    } else if (lowerName.find("action_x86") != std::string::npos) {
        result.category = "Внешний оверлей (Mirillis Action)";
        result.description = "Оверлей захвата экрана Mirillis Action вызвал сбой.";
        result.recommendation = "Отключите оверлей Action для Gothic3.exe.";
        result.isKnownCulprit = true;
    }
    // 2. GPU Drivers
    else if (lowerName.find("amdvlk") != std::string::npos || lowerName.find("atiogl") != std::string::npos || lowerName.find("amdxc") != std::string::npos) {
        result.category = "Видеодрайвер (AMD Radeon Vulkan/OpenGL)";
        result.description = "Сбой внутри драйвера AMD Radeon при обработке команд Vulkan.";
        result.recommendation = "Обновите драйвер видеокарты AMD до актуального или выберите другую версию DXVK в папке игры.";
        result.isKnownCulprit = true;
    } else if (lowerName.find("nvoglv") != std::string::npos || lowerName.find("nvwgf2um") != std::string::npos || lowerName.find("nvd3dum") != std::string::npos) {
        result.category = "Видеодрайвер (NVIDIA GeForce)";
        result.description = "Сбой внутри драйвера NVIDIA.";
        result.recommendation = "Обновите видеодрайвер NVIDIA или настройте параметры DXVK в dxvk.conf.";
        result.isKnownCulprit = true;
    } else if (lowerName.find("igvk") != std::string::npos) {
        result.category = "Видеодрайвер (Intel Graphics Vulkan)";
        result.description = "Сбой внутри драйвера Intel Graphics.";
        result.recommendation = "Обновите драйвер встроенной графики Intel.";
        result.isKnownCulprit = true;
    }
    // 3. DXVK Graphics Wrapper
    else if (lowerName == "d3d9.dll" || lowerName == "dxgi.dll") {
        result.category = "Графический транслятор (DXVK Direct3D 9 -> Vulkan)";
        result.description = "Сбой в трансляторе DXVK при обработке Direct3D 9 вызовов.";
        result.recommendation = "Проверьте параметры в dxvk.conf (например dxvk.enableAsync) или протестируйте другую версию DXVK (1.10.3 / 2.4.1 / 3.0.1).";
        result.isKnownCulprit = true;
    }
    // 4. Gothic 3 Script Mods (scripts\Script_*.dll)
    else if (lowerName.rfind("script_", 0) == 0 || lowerPath.find("\\scripts\\") != std::string::npos) {
        result.category = "Скриптовый мод Gothic 3 (" + result.moduleName + ")";
        result.description = "Сбой произошёл непосредственно внутри кода скриптового мода '" + result.moduleName + "'.";
        result.recommendation = "Временно переместите файл 'scripts\\" + result.moduleName + "' в другую папку для проверки, либо обновите этот мод до последней совместимой версии.";
        result.isKnownCulprit = true;
    }
    // 5. Parallel Universe Patch (PUP)
    else if (lowerName.find("paru_") != std::string::npos) {
        result.category = "Parallel Universe Patch (PUP)";
        result.description = "Сбой в модуле многопоточных оптимизаций PUP (" + result.moduleName + ").";
        result.recommendation = "Проверьте параметры paru.ini или убедитесь в совместимости с другими установленными модами.";
        result.isKnownCulprit = true;
    }
    // 6. Base Gothic 3 Engine
    else if (lowerName == "engine.dll" || lowerName == "game.dll" || lowerName == "sharedbase.dll" ||
             lowerName == "gfc.dll" || lowerName == "gui.dll" || lowerName == "script.dll" ||
             lowerName == "filesystem.dll" || lowerName == "dialogs.dll" || lowerName == "importer.dll") {
        result.category = "Базовый движок Gothic 3 (" + result.moduleName + ")";
        result.description = "Сбой в основном коде движка Genome Engine.";
        result.recommendation = "Смотрите стек вызовов ниже — часто движок падает из-за некорректных параметров, переданных из скриптовых модов или при исчерпании 32-битной памяти.";
    }
    // 7. G3O Core
    else if (lowerName == "dinput8.dll") {
        result.category = "Gothic 3 Optima (G3O)";
        result.description = "Сбой внутри прокси-модуля G3O.";
        result.recommendation = "Сообщите разработчику G3O с приложением данного отчёта.";
    }
    else {
        result.category = "Системная или сторонняя библиотека (" + result.moduleName + ")";
        result.description = "Сбой произошёл в библиотеке: " + result.modulePath;
        result.recommendation = "Проверьте зависимости и совместимость установленного ПО.";
    }

    return result;
}

struct StackFrameInfo {
    uintptr_t address = 0;
    std::string moduleName;
    uintptr_t rva = 0;
    std::string symbolName;
    std::string sourceFile;
    int lineNumber = 0;
    bool isScriptMod = false;
};

static std::vector<StackFrameInfo> WalkCallStack(CONTEXT* pContext) {
    std::vector<StackFrameInfo> frames;
    if (!pContext) return frames;

    HANDLE hProcess = GetCurrentProcess();
    HANDLE hThread = GetCurrentThread();

    SymInitialize(hProcess, NULL, TRUE);

    CONTEXT ctx = *pContext;
    STACKFRAME64 frame = { 0 };
    frame.AddrPC.Offset = ctx.Eip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Ebp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Esp;
    frame.AddrStack.Mode = AddrModeFlat;

    for (int i = 0; i < 32; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_I386, hProcess, hThread, &frame, &ctx,
                         NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL)) {
            break;
        }

        if (frame.AddrPC.Offset == 0) break;

        StackFrameInfo sfi;
        sfi.address = (uintptr_t)frame.AddrPC.Offset;

        BlameResult modInfo = AnalyzeAddress(sfi.address);
        sfi.moduleName = modInfo.moduleName;
        sfi.rva = modInfo.rva;

        std::string lowerMod = ToLower(sfi.moduleName);
        if (lowerMod.rfind("script_", 0) == 0) {
            sfi.isScriptMod = true;
        }

        // Symbol resolution
        char symbolBuffer[sizeof(SYMBOL_INFO) + 256] = { 0 };
        PSYMBOL_INFO pSymbol = (PSYMBOL_INFO)symbolBuffer;
        pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        pSymbol->MaxNameLen = 255;
        DWORD64 displacement = 0;

        if (SymFromAddr(hProcess, frame.AddrPC.Offset, &displacement, pSymbol)) {
            sfi.symbolName = pSymbol->Name;
            if (displacement > 0) {
                char dispBuf[32];
                snprintf(dispBuf, sizeof(dispBuf), "+0x%llX", displacement);
                sfi.symbolName += dispBuf;
            }
        }

        IMAGEHLP_LINE64 line = { 0 };
        line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
        DWORD lineDisplacement = 0;
        if (SymGetLineFromAddr64(hProcess, frame.AddrPC.Offset, &lineDisplacement, &line)) {
            sfi.sourceFile = line.FileName;
            sfi.lineNumber = (int)line.LineNumber;
        }

        frames.push_back(sfi);
    }

    SymCleanup(hProcess);
    return frames;
}

static void WriteMiniDump(EXCEPTION_POINTERS* pExceptionInfo) {
    if (!s_dumpPath[0]) DetermineCrashPaths();

    HANDLE hDumpFile = CreateFileA(s_dumpPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hDumpFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = pExceptionInfo;
        mei.ClientPointers = FALSE;

        MiniDumpWriteDump(
            GetCurrentProcess(),
            GetCurrentProcessId(),
            hDumpFile,
            (MINIDUMP_TYPE)(MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithUnloadedModules | MiniDumpWithProcessThreadData),
            &mei,
            NULL,
            NULL
        );
        CloseHandle(hDumpFile);
    }
}

static void BuildReportText(std::string& outReport, EXCEPTION_POINTERS* pExceptionInfo,
                            const BlameResult& blame, const std::vector<StackFrameInfo>& stackFrames) {
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

    // 1. Diagnosis Summary
    outReport += "--------------------------------------------------------------------------------\n";
    outReport += "[ДИАГНОЗ И ВИНОВНИК СБОЯ]\n";
    outReport += "--------------------------------------------------------------------------------\n";
    snprintf(buf, sizeof(buf), "Категория:      %s\n", blame.category.c_str()); outReport += buf;
    snprintf(buf, sizeof(buf), "Модуль:         %s\n", blame.moduleName.c_str()); outReport += buf;
    snprintf(buf, sizeof(buf), "Путь к файлу:   %s\n", blame.modulePath.c_str()); outReport += buf;
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

    snprintf(buf, sizeof(buf), "\nОписание проблемы:\n  %s\n", blame.description.c_str()); outReport += buf;
    snprintf(buf, sizeof(buf), "\nРЕКОМЕНДАЦИЯ ПО ИСПРАВЛЕНИЮ:\n  %s\n\n", blame.recommendation.c_str()); outReport += buf;

    // Check if an indirect initiator was found in stack frames
    std::string initiatorMod;
    for (size_t i = 1; i < stackFrames.size(); ++i) {
        if (stackFrames[i].isScriptMod) {
            initiatorMod = stackFrames[i].moduleName;
            break;
        }
    }
    if (!initiatorMod.empty() && blame.moduleName != initiatorMod) {
        snprintf(buf, sizeof(buf),
                 "(!) ОБРАТИТЕ ВНИМАНИЕ:\n"
                 "    Хотя сбой произошёл в '%s', вызов в стек пришёл из скриптового мода '%s'!\n"
                 "    Проверьте именно мод '%s'.\n\n",
                 blame.moduleName.c_str(), initiatorMod.c_str(), initiatorMod.c_str());
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
    if (stackFrames.empty()) {
        outReport += "Стек вызовов недоступен.\n";
    } else {
        for (size_t i = 0; i < stackFrames.size(); ++i) {
            const auto& frame = stackFrames[i];
            snprintf(buf, sizeof(buf), "#%02d 0x%p [%s+0x%X]",
                     (int)i, (void*)frame.address, frame.moduleName.c_str(), (unsigned int)frame.rva);
            outReport += buf;
            if (!frame.symbolName.empty()) {
                snprintf(buf, sizeof(buf), " %s", frame.symbolName.c_str());
                outReport += buf;
            }
            if (!frame.sourceFile.empty()) {
                snprintf(buf, sizeof(buf), " (%s:%d)", frame.sourceFile.c_str(), frame.lineNumber);
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
    std::vector<std::string> recentLogs;
    GetRecentLogs(recentLogs);
    outReport += "--------------------------------------------------------------------------------\n";
    outReport += "[ПОСЛЕДНИЕ СОБЫТИЯ ПЕРЕД СБОЕМ (РЕГИСТРАТОР G3O)]\n";
    outReport += "--------------------------------------------------------------------------------\n";
    if (recentLogs.empty()) {
        outReport += "Записей в логе нет.\n";
    } else {
        for (const auto& line : recentLogs) {
            outReport += line;
        }
    }
    outReport += "\n";

    // 6. Loaded Modules Table
    outReport += "--------------------------------------------------------------------------------\n";
    outReport += "[ЗАГРУЖЕННЫЕ МОДУЛИ]\n";
    outReport += "--------------------------------------------------------------------------------\n";
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (hSnap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32 me = { sizeof(MODULEENTRY32) };
        if (Module32First(hSnap, &me)) {
            do {
                snprintf(buf, sizeof(buf), "0x%p - 0x%p  %-25s  %s\n",
                         me.modBaseAddr, me.modBaseAddr + me.modBaseSize, me.szModule, me.szExePath);
                outReport += buf;
            } while (Module32Next(hSnap, &me));
        }
        CloseHandle(hSnap);
    }
    outReport += "\n================================================================================\n";
}

static void SaveReportToFile(const std::string& report) {
    if (!s_crashLogPath[0]) DetermineCrashPaths();

    // 1. Save to game folder
    FILE* f = fopen(s_crashLogPath, "w");
    if (f) {
        fputs(report.c_str(), f);
        fclose(f);
    }

    // 2. Save to Documents\Gothic3
    if (s_docCrashLogPath[0] != '\0') {
        FILE* fDoc = fopen(s_docCrashLogPath, "w");
        if (fDoc) {
            fputs(report.c_str(), fDoc);
            fclose(fDoc);
        }
    }
}

static LONG HandleCrash(EXCEPTION_POINTERS* pExceptionInfo) {
    if (InterlockedCompareExchange(&s_crashHandlingActive, 1, 0) != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    DetermineCrashPaths();

    uintptr_t faultAddr = (uintptr_t)pExceptionInfo->ExceptionRecord->ExceptionAddress;
    BlameResult blame = AnalyzeAddress(faultAddr);
    std::vector<StackFrameInfo> stackFrames = WalkCallStack(pExceptionInfo->ContextRecord);

    if (GetConfig().writeCrashDump) {
        WriteMiniDump(pExceptionInfo);
    }

    std::string report;
    BuildReportText(report, pExceptionInfo, blame, stackFrames);
    if (GetConfig().writeCrashLog) {
        SaveReportToFile(report);
    }

    if (GetConfig().showCrashDialog) {
        // Prepare human-friendly message for popup dialog
        char msg[2048];
        snprintf(msg, sizeof(msg),
                 "Gothic 3 Optima — Обнаружен критический сбой!\n\n"
                 "Виновник сбоя: %s\n"
                 "Категория:     %s\n"
                 "Адрес сбоя:    0x%p (смещение +0x%X)\n"
                 "Код ошибки:    0x%08X\n\n"
                 "Описание:\n%s\n\n"
                 "Рекомендация:\n%s\n\n"
                 "Полный отчёт сохранён в:\n%s",
                 blame.moduleName.c_str(),
                 blame.category.c_str(),
                 (void*)faultAddr, (unsigned int)blame.rva,
                 pExceptionInfo->ExceptionRecord->ExceptionCode,
                 blame.description.c_str(),
                 blame.recommendation.c_str(),
                 s_crashLogPath);

        int wideLen = MultiByteToWideChar(CP_UTF8, 0, msg, -1, nullptr, 0);
        if (wideLen > 0) {
            std::vector<wchar_t> wideMsg(wideLen);
            MultiByteToWideChar(CP_UTF8, 0, msg, -1, wideMsg.data(), wideLen);
            MessageBoxW(NULL, wideMsg.data(), L"Gothic 3 Optima — Crash Investigator", MB_OK | MB_ICONERROR | MB_SYSTEMMODAL);
        } else {
            MessageBoxA(NULL, msg, "Gothic 3 Optima — Crash Investigator", MB_OK | MB_ICONERROR | MB_SYSTEMMODAL);
        }
    }

    // Log to debugger before leaving
    OutputDebugStringA(report.c_str());

    return EXCEPTION_EXECUTE_HANDLER;
}

static LONG WINAPI G3OUnhandledCrashFilter(EXCEPTION_POINTERS* pExceptionInfo) {
    HandleCrash(pExceptionInfo);
    if (s_previousFilter) {
        return s_previousFilter(pExceptionInfo);
    }
    return EXCEPTION_EXECUTE_HANDLER;
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

    Log("G3O", "Crash Investigator initialized. Crash reports will be saved to '%s'", s_crashLogPath);
}

void ShutdownCrashHandler() {
    if (s_vehHandle) {
        RemoveVectoredExceptionHandler(s_vehHandle);
        s_vehHandle = nullptr;
    }
    if (s_origSetUnhandledExceptionFilter) {
        MH_DisableHook((void*)SetUnhandledExceptionFilter);
    }
}

// -----------------------------------------------------------------------------
// Engine-level Fatal Error hooks (SharedBase.dll bCErrorAdmin)
// -----------------------------------------------------------------------------
typedef void (__thiscall *CallFatalError1_t)(void* pThis, const char* msg, const char* file, int line);
static CallFatalError1_t s_origCallFatalError1 = nullptr;

typedef void (__thiscall *CallFatalError2_t)(void* pThis, const char* msg, const char* file, const char* func, int line);
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

static void __fastcall HookedCallFatalError1(void* pThis, void* /*edx*/, const char* msg, const char* file, int line) {
    char fullMsg[1024];
    snprintf(fullMsg, sizeof(fullMsg), "Движок Gothic 3 вызвал Fatal Error:\nСообщение: %s\nФайл: %s (строка %d)",
             msg ? msg : "<null>", file ? file : "<unknown>", line);
    Log("EngineFatal", "%s", fullMsg);

    ReportManualCrash("Gothic 3 Engine Fatal Error", msg ? msg : "Engine Fatal Error", file, line);

    if (s_origCallFatalError1) {
        s_origCallFatalError1(pThis, msg, file, line);
    }
}

static void __fastcall HookedCallFatalError2(void* pThis, void* /*edx*/, const char* msg, const char* file, const char* func, int line) {
    char fullMsg[1024];
    snprintf(fullMsg, sizeof(fullMsg), "Движок Gothic 3 вызвал Fatal Error:\nСообщение: %s\nФункция: %s\nФайл: %s (строка %d)",
             msg ? msg : "<null>", func ? func : "<unknown>", file ? file : "<unknown>", line);
    Log("EngineFatal", "%s", fullMsg);

    ReportManualCrash("Gothic 3 Engine Fatal Error", msg ? msg : "Engine Fatal Error", file, line);

    if (s_origCallFatalError2) {
        s_origCallFatalError2(pThis, msg, file, func, line);
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

void ReportManualCrash(const char* title, const char* message, const char* file, int line) {
    __try {
        RaiseException(0xE0000001, EXCEPTION_NONCONTINUABLE, 0, NULL);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Exception reported
    }
}

} // namespace G3O
