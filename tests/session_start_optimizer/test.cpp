#include "session_start_policy.h"
#include <iostream>
#include <vector>
#include <cstdint>
#include <cstring>

static uint32_t s_mockSleepCalls = 0;
static DWORD    s_lastSleepArg = 0;
static uint32_t s_mockYieldCalls = 0;

static void WINAPI MockSleep(DWORD ms) {
    s_mockSleepCalls++;
    s_lastSleepArg = ms;
}

static BOOL WINAPI MockYield() {
    s_mockYieldCalls++;
    return TRUE;
}

static bool Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        return false;
    }
    return true;
}

int main() {
    bool ok = true;
    std::cout << "[Test] Session Start Optimizer Policy & Signature Suite starting...\n";

    // 1. Test signature verification
    {
        uint8_t validStream[32] = {
            0x8B, 0x3D, 0x68, 0xD7, 0x24, 0x10,             // mov edi, dword ptr [1024D768h]
            0xC6, 0x80, 0x21, 0x11, 0x00, 0x00, 0x01,       // mov byte ptr [eax + 1121h], 1
            0xBE, 0x0C, 0x00, 0x00, 0x00,                   // mov esi, 0Ch
            0x8D, 0x49, 0x00,                               // lea ecx, [ecx]
            0x83, 0xEE, 0x01,                               // sub esi, 1
            0xFF, 0x15, 0x64, 0xD0, 0x24, 0x10              // call dword ptr [1024D064h]
        };

        ok &= Expect(G3O::VerifySessionStartPatchSite(validStream, sizeof(validStream)),
                     "valid Game.dll 1.75.14 stream should verify successfully");

        // Corrupt mov edi opcode
        validStream[0] = 0x90;
        ok &= Expect(!G3O::VerifySessionStartPatchSite(validStream, sizeof(validStream)),
                     "corrupted mov edi opcode should fail verification");
        validStream[0] = 0x8B;

        // Corrupt mov esi count
        validStream[14] = 0x0A;
        ok &= Expect(!G3O::VerifySessionStartPatchSite(validStream, sizeof(validStream)),
                     "corrupted loop count (not 12) should fail verification");
        validStream[14] = 0x0C;

        // Null pointer and short buffers
        ok &= Expect(!G3O::VerifySessionStartPatchSite(nullptr, 32),
                     "null pointer should fail safely");
        ok &= Expect(!G3O::VerifySessionStartPatchSite(validStream, 10),
                     "truncated buffer should fail safely");
    }

    // 2. Test EvaluateSessionStartSleep (Fast Path)
    {
        s_mockSleepCalls = 0;
        s_lastSleepArg = 0;
        s_mockYieldCalls = 0;

        G3O::SessionStartConfig cfg;
        cfg.enableSessionStartOptimizer = true;
        cfg.sessionStartSleepMs = 1;

        G3O::SessionStartTelemetry telemetry{};

        for (int i = 0; i < 12; i++) {
            uint32_t slept = G3O::EvaluateSessionStartSleep(cfg, 100, telemetry, &MockSleep, &MockYield);
            ok &= Expect(slept == 1, "accelerated sleep should return configured sleepMs (1)");
            ok &= Expect(s_lastSleepArg == 1, "MockSleep should be called with 1 ms");
        }

        ok &= Expect(s_mockSleepCalls == 12, "exactly 12 sleep calls expected");
        ok &= Expect(s_mockYieldCalls == 12, "exactly 12 yield calls expected");
        ok &= Expect(telemetry.totalCalls == 12, "telemetry total calls should be 12");
        ok &= Expect(telemetry.acceleratedCalls == 12, "telemetry accelerated calls should be 12");
        ok &= Expect(telemetry.totalSavedMs == (12 * 99), "saved ms should be 12 * 99 = 1188 ms");
    }

    // 3. Test EvaluateSessionStartSleep (Disabled / Fallback Path)
    {
        s_mockSleepCalls = 0;
        s_lastSleepArg = 0;
        s_mockYieldCalls = 0;

        G3O::SessionStartConfig cfg;
        cfg.enableSessionStartOptimizer = false;
        cfg.sessionStartSleepMs = 1;

        G3O::SessionStartTelemetry telemetry{};

        uint32_t slept = G3O::EvaluateSessionStartSleep(cfg, 100, telemetry, &MockSleep, &MockYield);
        ok &= Expect(slept == 100, "disabled optimizer should return requested 100 ms");
        ok &= Expect(s_lastSleepArg == 100, "MockSleep should receive original 100 ms");
        ok &= Expect(s_mockYieldCalls == 0, "yield should not be called when disabled");
        ok &= Expect(telemetry.acceleratedCalls == 0, "telemetry accelerated calls should be 0");
        ok &= Expect(telemetry.totalSavedMs == 0, "telemetry total saved ms should be 0");
    }

    // 4. Test Simulated Patch & Restoration
    {
        uint8_t buffer[6] = { 0x8B, 0x3D, 0x68, 0xD7, 0x24, 0x10 };
        uint8_t origBackup[6];
        std::memcpy(origBackup, buffer, 6);

        uintptr_t dummyGateway = 0x12345678;
        uint8_t patch[6];
        patch[0] = 0xBF; // mov edi, imm32
        *reinterpret_cast<uint32_t*>(&patch[1]) = static_cast<uint32_t>(dummyGateway);
        patch[5] = 0x90; // nop

        std::memcpy(buffer, patch, 6);
        ok &= Expect(buffer[0] == 0xBF, "patch opcode must be 0xBF (mov edi)");
        ok &= Expect(*reinterpret_cast<uint32_t*>(&buffer[1]) == 0x12345678, "patch address must match gateway");
        ok &= Expect(buffer[5] == 0x90, "patch byte 5 must be NOP");

        // Restore
        std::memcpy(buffer, origBackup, 6);
        ok &= Expect(std::memcmp(buffer, origBackup, 6) == 0, "restored buffer must match backup byte-for-byte");
    }

    if (!ok) {
        std::cerr << "FAIL: session_start_optimizer tests failed!\n";
        return 1;
    }

    std::cout << "PASS: session_start_optimizer policy, signature verification, and patch simulation passed.\n";
    return 0;
}
