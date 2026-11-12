#pragma once
#include <cstdint>
#include <windows.h>

namespace G3O {

struct SessionStartConfig {
    bool enableSessionStartOptimizer = true;
    uint32_t sessionStartSleepMs = 1;
};

struct SessionStartTelemetry {
    uint32_t totalCalls = 0;
    uint32_t acceleratedCalls = 0;
    uint32_t totalSavedMs = 0;
};

using SleepFn = void (WINAPI*)(DWORD dwMilliseconds);
using YieldFn = BOOL (WINAPI*)();

inline uint32_t EvaluateSessionStartSleep(
    const SessionStartConfig& cfg,
    DWORD requestedMs,
    SessionStartTelemetry& telemetry,
    SleepFn pfnSleep,
    YieldFn pfnYield)
{
    telemetry.totalCalls++;
    if (!cfg.enableSessionStartOptimizer) {
        if (pfnSleep) {
            pfnSleep(requestedMs);
        }
        return requestedMs;
    }

    telemetry.acceleratedCalls++;
    if (pfnYield) {
        pfnYield();
    }

    uint32_t sleepMs = cfg.sessionStartSleepMs;
    if (sleepMs > 0 && pfnSleep) {
        pfnSleep(sleepMs);
    }

    if (requestedMs > sleepMs) {
        telemetry.totalSavedMs += (requestedMs - sleepMs);
    }
    return sleepMs;
}

// Verifies the exact instruction sequence inside Game.dll gCSession::Start warm-up loop:
// Offset 0:  8B 3D xx xx xx xx  (mov edi, dword ptr [IAT:kernel32!Sleep]) - 6 bytes
// Offset 6:  C6 80 21 11 00 00 01 (mov byte ptr [eax + 1121h], 1)        - 7 bytes
// Offset 13: BE 0C 00 00 00     (mov esi, 0Ch)                            - 5 bytes
inline bool VerifySessionStartPatchSite(const uint8_t* pCode, size_t size) {
    if (!pCode || size < 18) return false;

    // Check mov edi, [IAT]
    if (pCode[0] != 0x8B || pCode[1] != 0x3D) return false;

    // Check mov byte ptr [eax + 1121h], 1
    if (pCode[6]  != 0xC6 || pCode[7]  != 0x80 || pCode[8]  != 0x21 || pCode[9] != 0x11 ||
        pCode[10] != 0x00 || pCode[11] != 0x00 || pCode[12] != 0x01) {
        return false;
    }

    // Check mov esi, 0x0C
    if (pCode[13] != 0xBE || pCode[14] != 0x0C || pCode[15] != 0x00 || pCode[16] != 0x00 || pCode[17] != 0x00) {
        return false;
    }

    return true;
}

} // namespace G3O
