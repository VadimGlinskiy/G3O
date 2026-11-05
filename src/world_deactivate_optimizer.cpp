#include "world_deactivate_optimizer.h"
#include "config_manager.h"
#include "diagnostic_logger.h"
#include <minhook.h>
#include <windows.h>
#include <thread>
#include <vector>
#include <atomic>
#include <stdio.h>

namespace G3O {

static volatile bool s_suppressLogging = false;
static double s_lastDeactivateSec = 0.0;

static LARGE_INTEGER s_timerFreq = []() {
    LARGE_INTEGER li;
    QueryPerformanceFrequency(&li);
    if (li.QuadPart <= 0) li.QuadPart = 10000000LL;
    return li;
}();

void SetWorldDeactivateLoggingSuppressed(bool suppress) {
    s_suppressLogging = suppress;
}

bool IsWorldDeactivateLoggingSuppressed() {
    return s_suppressLogging;
}

double GetLastWorldDeactivateDurationSec() {
    return s_lastDeactivateSec;
}

// -----------------------------------------------------------------------------
// 1. GE_MESSAGEF_INFO Hook (SharedBase.dll)
// Silences verbose debug logging during world release to prevent critical section locks
// -----------------------------------------------------------------------------
typedef void (__cdecl* GE_MESSAGEF_INFO_t)(int level, const char* fmt, ...);
static GE_MESSAGEF_INFO_t Real_GE_MESSAGEF_INFO = nullptr;
static void* s_targetMessagefInfo = nullptr;

static void __cdecl Hooked_GE_MESSAGEF_INFO(int level, const char* fmt, ...) {
    if (s_suppressLogging) {
        // Drop informational logs during world deactivation / scene loading
        return;
    }

    va_list args;
    va_start(args, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    Real_GE_MESSAGEF_INFO(level, "%s", buf);
}

// -----------------------------------------------------------------------------
// 2. gCWorld::ReleaseData Hook (Game.dll)
// Parallelizes sector deactivation and layer teardown across CPU cores
// -----------------------------------------------------------------------------
typedef int (__thiscall* WorldReleaseData_t)(void* pWorld);
static WorldReleaseData_t Real_WorldReleaseData = nullptr;
static void* s_targetWorldReleaseData = nullptr;

typedef int (__thiscall* Deactivate_t)(void* pElem);
static Deactivate_t Real_Deactivate = nullptr;

typedef bool (__thiscall* RemoveAll_t)(void* pSector);
static RemoveAll_t Real_RemoveAll = nullptr;

typedef void* (__thiscall* GetElem_t)(void* pWrapper);

static int GetOptimalWorkerThreads() {
    const Config& cfg = GetConfig();
    int count = cfg.cpuThreads > 0 ? cfg.cpuThreads : (cfg.cpuCores > 0 ? cfg.cpuCores : 0);
    if (count <= 0) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        count = (int)si.dwNumberOfProcessors;
    }
    if (count < 2) count = 2;
    if (count > 32) count = 32;
    return count;
}

static int __fastcall Hooked_WorldReleaseData(void* pWorld, void* /*edx*/) {
    if (!pWorld) return 0;

    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);

    bool prevSuppress = s_suppressLogging;
    s_suppressLogging = true;

    int numSectors = *(int*)((char*)pWorld + 0x4C);
    void** sectorArray = *(void***)((char*)pWorld + 0x48);

    int workerCount = GetOptimalWorkerThreads();

    if (numSectors > 4 && sectorArray && Real_Deactivate && Real_RemoveAll) {
        // Parallel sector teardown: distribute sectors across worker threads
        std::vector<std::thread> workers;
        workers.reserve(workerCount);

        std::atomic<int> nextIndex(0);

        for (int t = 0; t < workerCount; ++t) {
            workers.emplace_back([&]() {
                while (true) {
                    int i = nextIndex.fetch_add(1);
                    if (i >= numSectors) break;

                    void* pWrapper = sectorArray[i];
                    if (pWrapper) {
                        GetElem_t* pVtable = *(GetElem_t**)pWrapper;
                        if (pVtable && pVtable[0]) {
                            void* pElem = pVtable[0](pWrapper);
                            if (pElem) {
                                // Step 1: Deactivate the sector
                                Real_Deactivate(pElem);
                                // Step 2: Clean up dynamic and geometry layers in parallel
                                Real_RemoveAll(pElem);
                            }
                        }
                    }
                }
            });
        }

        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }
    }

    // Call the original ReleaseData to cleanly handle world array deallocation,
    // HUD clearance, and spatial/physics reset
    int result = Real_WorldReleaseData(pWorld);

    s_suppressLogging = prevSuppress;

    QueryPerformanceCounter(&t1);
    s_lastDeactivateSec = (double)(t1.QuadPart - t0.QuadPart) / (double)s_timerFreq.QuadPart;

    Log("G3O", "[WorldDeactivate] gCWorld::ReleaseData completed in %.3f s (accelerated across %d threads for %d sectors)",
        s_lastDeactivateSec, workerCount, numSectors);

    return result;
}

bool InitializeWorldDeactivateOptimizer() {
    HMODULE hEngine = GetModuleHandleA("Engine.dll");
    if (hEngine) {
        Real_Deactivate = (Deactivate_t)GetProcAddress(hEngine, "?Deactivate@eCProcessibleElement@@QAE?AW4eEResult@1@XZ");
    }

    HMODULE hGame = GetModuleHandleA("Game.dll");
    if (hGame) {
        Real_RemoveAll = (RemoveAll_t)GetProcAddress(hGame, "?RemoveAll@gCSector@@QAE_NXZ");

        void* pReleaseData = (void*)GetProcAddress(hGame, "?ReleaseData@gCWorld@@MAE?AW4eEResult@eCProcessibleElement@@XZ");
        if (pReleaseData) {
            s_targetWorldReleaseData = pReleaseData;
            MH_STATUS st = MH_CreateHook(pReleaseData, (LPVOID)Hooked_WorldReleaseData, (LPVOID*)&Real_WorldReleaseData);
            if (st == MH_OK) {
                MH_EnableHook(pReleaseData);
                Log("G3O", "World Deactivate Optimizer: Hooked gCWorld::ReleaseData (0x%p)", pReleaseData);
            }
        }
    }

    HMODULE hShared = GetModuleHandleA("SharedBase.dll");
    if (hShared) {
        void* pMsg = (void*)GetProcAddress(hShared, "GE_MESSAGEF_INFO");
        if (pMsg) {
            s_targetMessagefInfo = pMsg;
            MH_STATUS st = MH_CreateHook(pMsg, (LPVOID)Hooked_GE_MESSAGEF_INFO, (LPVOID*)&Real_GE_MESSAGEF_INFO);
            if (st == MH_OK) {
                MH_EnableHook(pMsg);
                Log("G3O", "World Deactivate Optimizer: Hooked GE_MESSAGEF_INFO (0x%p)", pMsg);
            }
        }
    }

    Log("G3O", "World Deactivate Optimizer initialized successfully.");
    return true;
}

void ShutdownWorldDeactivateOptimizer() {
    if (s_targetWorldReleaseData) {
        MH_DisableHook(s_targetWorldReleaseData);
        s_targetWorldReleaseData = nullptr;
    }
    if (s_targetMessagefInfo) {
        MH_DisableHook(s_targetMessagefInfo);
        s_targetMessagefInfo = nullptr;
    }
}

} // namespace G3O
