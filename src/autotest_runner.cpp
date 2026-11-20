#include "autotest_runner.h"
#include "autotest_policy.h"
#include "config_manager.h"
#include "diagnostic_logger.h"
#include "g3o_build_info.h"
#include "g3o_output_path.h"
#include "hook_install_policy.h"
#include "profiler.h"
#include "savegame_profiler.h"

#include <minhook.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace G3O {

namespace {

struct AutoTestLoadRecord {
    int index = 0;
    bool ok = false;
    double totalSec = 0.0;
    std::string loadProfilerLog;
};

using HUDFileManagerGetInstanceFn = void* (__cdecl*)();
using HUDLoadGameStdcallFn = bool (__stdcall*)(void* pHUDFileManager, const void* pSlotNameBString);
using SessionAccessFn = void* (__stdcall*)();
using SessionIsGameRunningFn = bool (__thiscall*)(const void* pSession);
using SessionGetGUIManagerFn = void* (__thiscall*)(const void* pSession);
using SessionProcessFn = void (__thiscall*)(void* pSession);
using GUIManagerOpenMenuFn = bool (__thiscall*)(void* pGuiManager);
using HUDMenuOpenMenuFn = void (__thiscall*)(void* pHudMenu, int menuId);
using bCStringCtorCStrFn = void* (__thiscall*)(void* pStr, const char* text);
using bCStringDtorFn = void (__thiscall*)(void* pStr);

constexpr int kHUDMenuMain = 0;
constexpr int kHUDMenuIntro = 12;

CRITICAL_SECTION s_atCs{};
bool s_atCsInit = false;
AutoTestStateMachine s_sm;
std::vector<AutoTestLoadRecord> s_loadRecords;
std::atomic<bool> s_active{ false };
std::atomic<bool> s_exiting{ false };
std::atomic<bool> s_menuReadyLogged{ false };
std::atomic<bool> s_loadCallbackSeen{ false };
std::uint64_t s_startTickMs = 0;

HANDLE s_watchdogThread = nullptr;
std::atomic<bool> s_watchdogStop{ false };

HUDFileManagerGetInstanceFn s_getHudFileManager = nullptr;
HUDLoadGameStdcallFn s_callHudLoadGame = nullptr;
SessionAccessFn s_accessSession = nullptr;
SessionIsGameRunningFn s_isGameRunning = nullptr;
SessionGetGUIManagerFn s_getGuiManager = nullptr;
GUIManagerOpenMenuFn s_guiOpenMenu = nullptr;
bCStringCtorCStrFn s_bstrCtor = nullptr;
bCStringDtorFn s_bstrDtor = nullptr;

SessionProcessFn Real_SessionProcess = nullptr;
HUDMenuOpenMenuFn Real_HUDMenuOpenMenu = nullptr;
void* s_targetSessionProcess = nullptr;
void* s_targetHUDMenuOpenMenu = nullptr;

void EnsureAutoTestCS() {
    if (!s_atCsInit) {
        InitializeCriticalSection(&s_atCs);
        s_atCsInit = true;
    }
}

std::string EscapeJson(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (char c : input) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"':  out += "\\\""; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:   out += c;      break;
        }
    }
    return out;
}

void WriteAutoTestSummaryJson(int exitCode, const std::string& status, const std::string& abortReason) {
    char directory[MAX_PATH] = {};
    if (!EnsureG3OOutputDirectory(directory, sizeof(directory))) {
        return;
    }

    const Config& cfg = GetConfig();
    char fileName[128] = {};
    if (!cfg.autoTestRunId.empty()) {
        std::snprintf(fileName, sizeof(fileName), "G3O_AutoTest_%s.json", cfg.autoTestRunId.c_str());
    } else {
        std::snprintf(fileName, sizeof(fileName), "G3O_AutoTest_%lu.json", GetCurrentProcessId());
    }

    char outPath[MAX_PATH] = {};
    if (!BuildG3OOutputFilePath(outPath, sizeof(outPath), directory, fileName)) {
        return;
    }

    FILE* fp = std::fopen(outPath, "wb");
    if (!fp) return;

    const char* diagPath = GetDiagnosticLogPath();
    std::fprintf(fp, "{\n");
    std::fprintf(fp, "  \"runId\": \"%s\",\n", EscapeJson(cfg.autoTestRunId).c_str());
    std::fprintf(fp, "  \"pid\": %lu,\n", GetCurrentProcessId());
    std::fprintf(fp, "  \"buildVersion\": \"%s\",\n", EscapeJson(G3O_VERSION_STRING).c_str());
    std::fprintf(fp, "  \"buildConfig\": \"%s\",\n", EscapeJson(G3O_BUILD_CONFIGURATION).c_str());
    std::fprintf(fp, "  \"buildTimestamp\": \"%s\",\n", EscapeJson(G3O_BUILD_TIMESTAMP).c_str());
    std::fprintf(fp, "  \"status\": \"%s\",\n", EscapeJson(status).c_str());
    std::fprintf(fp, "  \"exitCode\": %d,\n", exitCode);
    std::fprintf(fp, "  \"abortReason\": \"%s\",\n", EscapeJson(abortReason).c_str());
    std::fprintf(fp, "  \"slot\": \"%s\",\n", EscapeJson(cfg.autoTestSlot).c_str());
    std::fprintf(fp, "  \"targetLoads\": %d,\n", cfg.autoTestLoads);
    std::fprintf(fp, "  \"loadsCompleted\": %d,\n", s_sm.LoadsCompleted());
    std::fprintf(fp, "  \"diagnosticLog\": \"%s\",\n", EscapeJson(diagPath ? diagPath : "").c_str());
    std::fprintf(fp, "  \"loads\": [\n");
    for (std::size_t i = 0; i < s_loadRecords.size(); ++i) {
        const auto& rec = s_loadRecords[i];
        std::fprintf(fp, "    {\"index\": %d, \"ok\": %s, \"totalSec\": %.3f, \"loadProfilerLog\": \"%s\"}%s\n",
            rec.index,
            rec.ok ? "true" : "false",
            rec.totalSec,
            EscapeJson(rec.loadProfilerLog).c_str(),
            (i + 1 < s_loadRecords.size()) ? "," : "");
    }
    std::fprintf(fp, "  ]\n");
    std::fprintf(fp, "}\n");
    std::fflush(fp);
    std::fclose(fp);

    Log("AutoTest", "[AutoTest] SUMMARY_JSON path=%s", outPath);
}

[[noreturn]] void FlushAndTerminate(int exitCode, const std::string& abortReason) {
    if (s_exiting.exchange(true, std::memory_order_acq_rel)) {
        Sleep(10000);
        TerminateProcess(GetCurrentProcess(), static_cast<UINT>(exitCode));
    }
    s_watchdogStop.store(true, std::memory_order_release);

    const bool isSuccess = (exitCode == kAutoTestExitSuccess);
    const std::string status = isSuccess ? "DONE" : "ABORT";
    if (isSuccess) {
        Log("AutoTest", "[AutoTest] DONE loadsCompleted=%d/%d exitCode=%d",
            s_sm.LoadsCompleted(), s_sm.TargetLoads(), exitCode);
    } else {
        Log("AutoTest", "[AutoTest] ABORT reason=%s loadsCompleted=%d/%d exitCode=%d",
            abortReason.c_str(), s_sm.LoadsCompleted(), s_sm.TargetLoads(), exitCode);
    }

    WriteAutoTestSummaryJson(exitCode, status, abortReason);

    FlushDiagnosticLog();
    if (GetConfig().enableProfiler) {
        ShutdownProfiler();
    }
    ShutdownDiagnosticLogger();

    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(exitCode));
}

bool IsMainMenuReadySafe() {
    if (!s_getHudFileManager || !s_accessSession || !s_isGameRunning) return false;
    void* pMgr = s_getHudFileManager();
    if (!pMgr) return false;
    void* pMainMenu = *reinterpret_cast<void**>(reinterpret_cast<BYTE*>(pMgr) + 4);
    if (!pMainMenu) return false;
    const int currentMenu = *reinterpret_cast<int*>(reinterpret_cast<BYTE*>(pMainMenu) + 0x48);
    if (currentMenu != kHUDMenuMain) return false;
    void* pSession = s_accessSession();
    if (!pSession) return false;
    return !s_isGameRunning(pSession);
}

bool IsGameplayReadySafe() {
    if (!s_accessSession || !s_isGameRunning) return false;
    void* pSession = s_accessSession();
    return pSession && s_isGameRunning(pSession);
}

void ExecuteLoadOnMainThread(int loadIndex, const std::string& slot) {
    Log("AutoTest", "[AutoTest] LOAD_BEGIN index=%d/%d slot=%s",
        loadIndex, s_sm.TargetLoads(), slot.c_str());
    FlushDiagnosticLog();

    if (s_accessSession && s_isGameRunning && s_getGuiManager && s_guiOpenMenu) {
        void* pSession = s_accessSession();
        if (pSession && s_isGameRunning(pSession)) {
            void* pGui = s_getGuiManager(pSession);
            if (pGui) {
                s_guiOpenMenu(pGui);
            }
        }
    }

    s_loadCallbackSeen.store(false, std::memory_order_release);

    std::uint32_t bstrStorage = 0;
    s_bstrCtor(&bstrStorage, slot.c_str());
    void* pMgr = s_getHudFileManager();
    const bool ok = pMgr ? s_callHudLoadGame(pMgr, &bstrStorage) : false;
    s_bstrDtor(&bstrStorage);

    if (!s_loadCallbackSeen.load(std::memory_order_acquire)) {
        AutoTestNotifyLoadFinished(ok, 0.0, "");
    }
}

void __fastcall Hooked_SessionProcess(void* pSession, void* /*edx*/) {
    Real_SessionProcess(pSession);

    if (!s_active.load(std::memory_order_acquire) || s_exiting.load(std::memory_order_acquire)) {
        return;
    }

    const bool menuReady = IsMainMenuReadySafe();
    if (menuReady && !s_menuReadyLogged.exchange(true, std::memory_order_acq_rel)) {
        const std::uint64_t elapsedMs = GetTickCount64() - s_startTickMs;
        Log("AutoTest", "[AutoTest] MENU_READY elapsedMs=%llu",
            static_cast<unsigned long long>(elapsedMs));
    }
    const bool loadActive = IsSavegameLoadActive();
    const bool gameplayReady = IsGameplayReadySafe();

    AutoTestAction action = AutoTestAction::None;
    int loadIndex = 0;
    std::string slot;
    int exitCode = kAutoTestExitSuccess;
    std::string abortReason;

    EnterCriticalSection(&s_atCs);
    action = s_sm.OnMainLoopTick(menuReady, loadActive, gameplayReady, GetTickCount64());
    if (action == AutoTestAction::TriggerLoad) {
        loadIndex = s_sm.CurrentLoadIndex();
        slot = s_sm.Slot();
    } else if (action == AutoTestAction::FlushAndExit) {
        exitCode = s_sm.ExitCode();
        abortReason = s_sm.AbortReason();
    }
    LeaveCriticalSection(&s_atCs);

    if (action == AutoTestAction::TriggerLoad) {
        ExecuteLoadOnMainThread(loadIndex, slot);
    } else if (action == AutoTestAction::FlushAndExit) {
        FlushAndTerminate(exitCode, abortReason);
    }
}

void __fastcall Hooked_HUDMenuOpenMenu(void* pHudMenu, void* /*edx*/, int menuId) {
    if (s_active.load(std::memory_order_acquire) &&
        GetConfig().autoTestSkipIntro &&
        menuId == kHUDMenuIntro) {
        Log("AutoTest", "[AutoTest] Skipping intro menu (gEHUDMenu_Intro -> gEHUDMenu_Main).");
        menuId = kHUDMenuMain;
    }
    Real_HUDMenuOpenMenu(pHudMenu, menuId);
}

DWORD WINAPI AutoTestWatchdogProc(LPVOID) {
    while (!s_watchdogStop.load(std::memory_order_acquire)) {
        Sleep(500);
        if (s_watchdogStop.load(std::memory_order_acquire) || s_exiting.load(std::memory_order_acquire)) {
            break;
        }
        AutoTestAction action = AutoTestAction::None;
        int exitCode = kAutoTestExitInternalTimeout;
        std::string abortReason;
        EnterCriticalSection(&s_atCs);
        action = s_sm.OnWatchdogTick(GetTickCount64());
        if (action == AutoTestAction::FlushAndExit) {
            exitCode = s_sm.ExitCode();
            abortReason = s_sm.AbortReason();
        }
        LeaveCriticalSection(&s_atCs);

        if (action == AutoTestAction::FlushAndExit) {
            FlushAndTerminate(exitCode, abortReason);
        }
    }
    return 0;
}

void* FindHUDLoadGameAddr(HMODULE hGame) {
    if (!hGame) return nullptr;
    const BYTE sig[] = {
        0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x8B, 0x45, 0x08,
        0x83, 0xEC, 0x5C, 0x83, 0x78, 0x04, 0x00, 0x53, 0x56, 0x57
    };
    BYTE* candidate = reinterpret_cast<BYTE*>(hGame) + 0xA9A10;
    if (std::memcmp(candidate, sig, sizeof(sig)) == 0) {
        return candidate;
    }
    return nullptr;
}

bool InstallAutoTestHook(void* target, LPVOID detour, LPVOID* original, void** slot, const char* name) {
    if (!target || !original || !slot) return false;
    MH_STATUS createStatus = MH_OK;
    MH_STATUS enableStatus = MH_OK;
    const HookInstallResult res = InstallHookSafely(
        [&] { createStatus = MH_CreateHook(target, detour, original); return createStatus == MH_OK; },
        [&] { enableStatus = MH_EnableHook(target); return enableStatus == MH_OK; },
        [&] { MH_RemoveHook(target); },
        [&] { *original = nullptr; });
    if (res != HookInstallResult::Installed) {
        Log("AutoTest", "[AutoTest] Failed to install hook %s (create=%d, enable=%d).",
            name, static_cast<int>(createStatus), static_cast<int>(enableStatus));
        return false;
    }
    *slot = target;
    Log("AutoTest", "[AutoTest] Hooked %s at 0x%p.", name, target);
    return true;
}

} // namespace

bool InitializeAutoTestRunner() {
    const Config& cfg = GetConfig();
    if (!cfg.autoTestEnabled) {
        return true;
    }

    EnsureAutoTestCS();
    s_startTickMs = GetTickCount64();

    AutoTestPolicyConfig policyCfg{};
    policyCfg.enabled = true;
    policyCfg.slot = cfg.autoTestSlot;
    policyCfg.targetLoads = cfg.autoTestLoads;
    policyCfg.settleFrames = cfg.autoTestSettleFrames;
    policyCfg.menuSettleTicks = cfg.autoTestMenuSettleTicks;
    policyCfg.timeoutSec = cfg.autoTestTimeoutSec;

    EnterCriticalSection(&s_atCs);
    s_loadRecords.clear();
    s_sm.Start(policyCfg, s_startTickMs);
    LeaveCriticalSection(&s_atCs);

    Log("AutoTest",
        "[AutoTest] START runId=%s pid=%lu slot=%s loads=%d settleFrames=%d menuSettleTicks=%d timeoutSec=%d skipIntro=%d build=%s",
        cfg.autoTestRunId.empty() ? "<auto>" : cfg.autoTestRunId.c_str(),
        GetCurrentProcessId(),
        cfg.autoTestSlot.c_str(),
        cfg.autoTestLoads,
        cfg.autoTestSettleFrames,
        cfg.autoTestMenuSettleTicks,
        cfg.autoTestTimeoutSec,
        cfg.autoTestSkipIntro ? 1 : 0,
        G3O_VERSION_STRING);

    HMODULE hGame = GetModuleHandleA("Game.dll");
    HMODULE hSharedBase = GetModuleHandleA("SharedBase.dll");
    if (!hGame || !hSharedBase) {
        EnterCriticalSection(&s_atCs);
        s_sm.AbortHookUnavailable("missing_game_or_sharedbase");
        LeaveCriticalSection(&s_atCs);
        FlushAndTerminate(kAutoTestExitHookUnavailable, "missing_game_or_sharedbase");
    }

    s_getHudFileManager = reinterpret_cast<HUDFileManagerGetInstanceFn>(reinterpret_cast<BYTE*>(hGame) + 0xA92D0);
    void* pHudLoadGame = GetHUDLoadGameTarget();
    if (!pHudLoadGame) pHudLoadGame = FindHUDLoadGameAddr(hGame);
    s_callHudLoadGame = reinterpret_cast<HUDLoadGameStdcallFn>(pHudLoadGame);
    s_accessSession = reinterpret_cast<SessionAccessFn>(GetProcAddress(hGame, "?AccessSession@gCSession@@SGAAV1@XZ"));
    s_isGameRunning = reinterpret_cast<SessionIsGameRunningFn>(GetProcAddress(hGame, "?IsGameRunning@gCSession@@QBE?B_NXZ"));
    s_getGuiManager = reinterpret_cast<SessionGetGUIManagerFn>(GetProcAddress(hGame, "?GetGUIManager@gCSession@@QBEPAVgCGUIManager@@XZ"));
    s_guiOpenMenu = reinterpret_cast<GUIManagerOpenMenuFn>(GetProcAddress(hGame, "?OpenMenu@gCGUIManager@@QAE_NXZ"));
    s_bstrCtor = reinterpret_cast<bCStringCtorCStrFn>(GetProcAddress(hSharedBase, "??0bCString@@QAE@PBD@Z"));
    s_bstrDtor = reinterpret_cast<bCStringDtorFn>(GetProcAddress(hSharedBase, "??1bCString@@QAE@XZ"));

    void* pSessionProcess = reinterpret_cast<void*>(GetProcAddress(hGame, "?Process@gCSession@@UAEXXZ"));
    void* pHUDMenuOpenMenu = reinterpret_cast<void*>(GetProcAddress(hGame, "?OpenMenu@gCHUDMenu@@IAEXW4gEHUDMenu@@@Z"));

    if (!s_callHudLoadGame || !s_accessSession || !s_isGameRunning ||
        !s_bstrCtor || !s_bstrDtor || !pSessionProcess) {
        EnterCriticalSection(&s_atCs);
        s_sm.AbortHookUnavailable("unresolved_autotest_symbols");
        LeaveCriticalSection(&s_atCs);
        FlushAndTerminate(kAutoTestExitHookUnavailable, "unresolved_autotest_symbols");
    }

    if (!InstallAutoTestHook(pSessionProcess, reinterpret_cast<LPVOID>(&Hooked_SessionProcess),
                             reinterpret_cast<LPVOID*>(&Real_SessionProcess),
                             &s_targetSessionProcess, "gCSession::Process")) {
        EnterCriticalSection(&s_atCs);
        s_sm.AbortHookUnavailable("session_process_hook_failed");
        LeaveCriticalSection(&s_atCs);
        FlushAndTerminate(kAutoTestExitHookUnavailable, "session_process_hook_failed");
    }

    if (cfg.autoTestSkipIntro && pHUDMenuOpenMenu) {
        InstallAutoTestHook(pHUDMenuOpenMenu, reinterpret_cast<LPVOID>(&Hooked_HUDMenuOpenMenu),
                            reinterpret_cast<LPVOID*>(&Real_HUDMenuOpenMenu),
                            &s_targetHUDMenuOpenMenu, "gCHUDMenu::OpenMenu");
    }

    s_active.store(true, std::memory_order_release);
    s_watchdogStop.store(false, std::memory_order_release);
    s_watchdogThread = CreateThread(nullptr, 0, AutoTestWatchdogProc, nullptr, 0, nullptr);
    return true;
}

void ShutdownAutoTestRunner() {
    s_active.store(false, std::memory_order_release);
    s_watchdogStop.store(true, std::memory_order_release);
    if (s_watchdogThread) {
        WaitForSingleObject(s_watchdogThread, 1500);
        CloseHandle(s_watchdogThread);
        s_watchdogThread = nullptr;
    }
    if (s_targetSessionProcess) {
        MH_DisableHook(s_targetSessionProcess);
        s_targetSessionProcess = nullptr;
    }
    if (s_targetHUDMenuOpenMenu) {
        MH_DisableHook(s_targetHUDMenuOpenMenu);
        s_targetHUDMenuOpenMenu = nullptr;
    }
}

void AutoTestNotifyLoadStarted() {
    if (!s_active.load(std::memory_order_acquire)) return;
    EnterCriticalSection(&s_atCs);
    s_sm.OnLoadStarted(GetTickCount64());
    LeaveCriticalSection(&s_atCs);
}

void AutoTestNotifyLoadFinished(bool succeeded, double totalSec, const char* loadProfilerPath) {
    if (!s_active.load(std::memory_order_acquire)) return;
    s_loadCallbackSeen.store(true, std::memory_order_release);

    AutoTestAction action = AutoTestAction::None;
    int exitCode = kAutoTestExitSuccess;
    std::string abortReason;
    int idx = 0;
    int target = 0;

    EnterCriticalSection(&s_atCs);
    idx = s_sm.CurrentLoadIndex();
    target = s_sm.TargetLoads();
    s_loadRecords.push_back(AutoTestLoadRecord{
        idx,
        succeeded,
        totalSec,
        loadProfilerPath ? loadProfilerPath : ""
    });
    action = s_sm.OnLoadFinished(succeeded, totalSec, GetTickCount64());
    if (action == AutoTestAction::FlushAndExit) {
        exitCode = s_sm.ExitCode();
        abortReason = s_sm.AbortReason();
    }
    LeaveCriticalSection(&s_atCs);

    Log("AutoTest", "[AutoTest] LOAD_END index=%d/%d ok=%d totalSec=%.3f loadProfiler=%s",
        idx, target, succeeded ? 1 : 0, totalSec,
        (loadProfilerPath && loadProfilerPath[0]) ? loadProfilerPath : "<none>");
    FlushDiagnosticLog();

    if (action == AutoTestAction::FlushAndExit) {
        FlushAndTerminate(exitCode, abortReason);
    }
}

} // namespace G3O
