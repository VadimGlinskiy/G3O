#include "session_start_optimizer.h"
#include "session_start_policy.h"
#include "config_manager.h"
#include "diagnostic_logger.h"
#include <windows.h>
#include <atomic>

namespace G3O {

namespace {

static uint8_t*  s_pPatchSite = nullptr;
static uint8_t   s_origBytes[6] = {};
static bool      s_bHooked = false;

static std::atomic<uint32_t> s_u32TotalSleepCalls{0};
static std::atomic<uint32_t> s_u32TotalSavedMs{0};

// WINAPI gateway matching kernel32!Sleep signature (__stdcall, 1 DWORD param, ret 4, callee-saved registers)
void WINAPI G3O_SessionStartSleepGateway(DWORD dwMilliseconds) {
    const auto& cfg = GetConfig();

    SessionStartConfig policyCfg;
    policyCfg.enableSessionStartOptimizer = cfg.enableSessionStartOptimizer;
    policyCfg.sessionStartSleepMs = cfg.sessionStartSleepMs;

    SessionStartTelemetry telemetry;
    uint32_t executedMs = EvaluateSessionStartSleep(
        policyCfg,
        dwMilliseconds,
        telemetry,
        &::Sleep,
        &::SwitchToThread
    );

    uint32_t count = s_u32TotalSleepCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (dwMilliseconds > executedMs) {
        s_u32TotalSavedMs.fetch_add(dwMilliseconds - executedMs, std::memory_order_relaxed);
    }

    if (count % 12 == 0) {
        uint32_t saved = 12 * (dwMilliseconds > executedMs ? (dwMilliseconds - executedMs) : 0);
        Log("G3O", "[SessionStartOptimizer] Completed 12 warm-up iterations: throttled 12x Sleep(%u) to Sleep(%u) (saved ~%u ms)",
            dwMilliseconds, executedMs, saved);
    }
}

} // namespace

bool InitializeSessionStartOptimizer() {
    if (s_bHooked) {
        return true;
    }

    const auto& cfg = GetConfig();
    if (!cfg.enableSessionStartOptimizer) {
        Log("G3O", "Session Start Optimizer: Disabled in configuration.");
        return false;
    }

    HMODULE hGame = GetModuleHandleA("Game.dll");
    if (!hGame) {
        Log("G3O", "Session Start Optimizer: Game.dll is not loaded.");
        return false;
    }

    uintptr_t uGame = reinterpret_cast<uintptr_t>(hGame);
    uint8_t* pPatch = reinterpret_cast<uint8_t*>(uGame + 0x173FCB);

    if (!VerifySessionStartPatchSite(pPatch, 32)) {
        Log("G3O", "Session Start Optimizer: Signature verification failed at Game.dll+0x173FCB.");
        return false;
    }

    // Verify that the dereferenced pointer is indeed kernel32!Sleep
    void** ppSleep = *reinterpret_cast<void***>(pPatch + 2);
    if (!ppSleep || *ppSleep != reinterpret_cast<void*>(&::Sleep)) {
        Log("G3O", "Session Start Optimizer: IAT entry at 0x%p does not point to kernel32!Sleep (got 0x%p).",
            ppSleep, ppSleep ? *ppSleep : nullptr);
        return false;
    }

    s_pPatchSite = pPatch;
    memcpy(s_origBytes, pPatch, 6);

    uint8_t patch[6];
    patch[0] = 0xBF; // mov edi, imm32
    *reinterpret_cast<uint32_t*>(&patch[1]) = reinterpret_cast<uint32_t>(&G3O_SessionStartSleepGateway);
    patch[5] = 0x90; // nop

    DWORD oldProtect = 0;
    if (!VirtualProtect(pPatch, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log("G3O", "Session Start Optimizer: Failed to unprotect memory at Game.dll+0x173FCB.");
        return false;
    }

    memcpy(pPatch, patch, 6);
    VirtualProtect(pPatch, 6, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), pPatch, 6);

    s_bHooked = true;
    Log("G3O", "Session Start Optimizer: Successfully installed gateway at Game.dll+0x173FCB (SleepMs=%u)",
        cfg.sessionStartSleepMs);
    return true;
}

void ShutdownSessionStartOptimizer() {
    if (!s_bHooked || !s_pPatchSite) {
        return;
    }

    DWORD oldProtect = 0;
    if (VirtualProtect(s_pPatchSite, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        memcpy(s_pPatchSite, s_origBytes, 6);
        VirtualProtect(s_pPatchSite, 6, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), s_pPatchSite, 6);
        Log("G3O", "Session Start Optimizer: Restored original code at Game.dll+0x173FCB.");
    }

    s_bHooked = false;
    s_pPatchSite = nullptr;
}

uint32_t GetSessionStartSleepHookCount() noexcept {
    return s_u32TotalSleepCalls.load(std::memory_order_relaxed);
}

} // namespace G3O
