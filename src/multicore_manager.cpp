#include "multicore_manager.h"
#include "cpu_topology.h"
#include <minhook.h>
#include <tlhelp32.h>
#include <cstdio>

// ---------------------------------------------------------------------------
// Phase 20: Multicore Threading Engine for Genome Engine
// ---------------------------------------------------------------------------
// 1. Scales eCThreadPool background worker threads from 1 to 4-8 across physical cores.
// 2. Unlocks asynchronous streaming queues via eCThreadPool::EnableAllThreads(true).
// 3. Eliminates synchronous cache locks via eCMainCache::AccessDisableThreading hook.
// 4. Boosts main game loop thread priority and balances worker priorities.
// 5. Configures PhysX 2.4.1 scene multithreading (4 worker threads).
// ---------------------------------------------------------------------------

namespace {

// RVA offsets in Engine.dll
constexpr uintptr_t RVA_BASECACHE_SETNUMTHREADS             = 0x001EF8F0;
constexpr uintptr_t RVA_THREADPOOL_CREATE                   = 0x001F4010;
constexpr uintptr_t RVA_MAINCACHE_ACCESS_DISABLE_THREADING  = 0x000065B0;
constexpr uintptr_t RVA_THREADPOOL_ENABLE_ALL_THREADS       = 0x001F3C60;
constexpr uintptr_t RVA_THREADPOOL_GET_GLOBAL_JOBS          = 0x001F4260;
constexpr uintptr_t RVA_THREADPOOL_GET_GLOBAL_WORKING       = 0x001F4270;
constexpr uintptr_t RVA_ENGINE_CLOCK_PTR                    = 0x00602A88;

// Function pointer types
typedef void (__thiscall* BaseCache_SetNumThreads_t)(void* pThis, uint32_t uNumThreads);
static BaseCache_SetNumThreads_t Real_BaseCache_SetNumThreads = nullptr;

typedef bool (__thiscall* ThreadPool_Create_t)(void* pThis, uint32_t uNumThreads);
static ThreadPool_Create_t Real_ThreadPool_Create = nullptr;

typedef bool& (__thiscall* AccessDisableThreading_t)(void* pThis);
static AccessDisableThreading_t Real_AccessDisableThreading = nullptr;

typedef void (__stdcall* EnableAllThreads_t)(bool bEnable);
typedef unsigned long (__stdcall* GetGlobalThreadStats_t)();

static EnableAllThreads_t g_pfnEnableAllThreads = nullptr;
static GetGlobalThreadStats_t g_pfnGetGlobalJobsInQueue = nullptr;
static GetGlobalThreadStats_t g_pfnGetGlobalThreadsWorking = nullptr;
static void** g_ppEngineClock = nullptr;

// State tracking
static uint32_t g_targetWorkerThreads = 4;
static volatile LONG g_scaledCachesCount = 0;
static volatile LONG g_scaledPoolsCount = 0;
static bool s_alwaysFalse = false;
static bool s_multicoreInitialized = false;

// ---------------------------------------------------------------------------
// 1. Hook for eCBaseCache::SetNumThreadsAndCreate (0x101ef8f0)
// Intercepts cache thread configuration for Mesh, Image, Animation, Collision, etc.
// ---------------------------------------------------------------------------
static void __fastcall Hooked_BaseCache_SetNumThreads(void* pThis, void* /*edx*/, uint32_t uNumThreads) {
    uint32_t configuredThreads = uNumThreads;
    if (uNumThreads < g_targetWorkerThreads) {
        uNumThreads = g_targetWorkerThreads;
    }

    InterlockedIncrement(&g_scaledCachesCount);

    char buf[256];
    sprintf_s(buf, "[Gothic3Optima] Multicore: eCBaseCache thread pool scaled from %u to %u workers (cache: %p)\n",
        configuredThreads, uNumThreads, pThis);
    OutputDebugStringA(buf);

    Real_BaseCache_SetNumThreads(pThis, uNumThreads);
}

// ---------------------------------------------------------------------------
// 2. Hook for eCThreadPool::Create (0x101f4010)
// Overrides thread count for any thread pool created dynamically by the engine.
// ---------------------------------------------------------------------------
static bool __fastcall Hooked_ThreadPool_Create(void* pThis, void* /*edx*/, uint32_t uNumThreads) {
    uint32_t originalThreads = uNumThreads;
    if (uNumThreads < g_targetWorkerThreads) {
        uNumThreads = g_targetWorkerThreads;
    }

    bool result = Real_ThreadPool_Create(pThis, uNumThreads);
    if (result) {
        InterlockedIncrement(&g_scaledPoolsCount);
        char buf[256];
        sprintf_s(buf, "[Gothic3Optima] Multicore: eCThreadPool::Create expanded pool to %u threads (orig: %u, pool: %p)\n",
            uNumThreads, originalThreads, pThis);
        OutputDebugStringA(buf);
    }
    return result;
}

// ---------------------------------------------------------------------------
// 3. Hook for eCMainCache::AccessDisableThreading (0x100065b0)
// Always returns reference to false, preventing engine from disabling threading.
// ---------------------------------------------------------------------------
static bool& __fastcall Hooked_AccessDisableThreading(void* /*pThis*/, void* /*edx*/) {
    return s_alwaysFalse;
}

// ---------------------------------------------------------------------------
// 4. Set Main Game Thread to Above Normal Priority
// ---------------------------------------------------------------------------
static void BoostMainThreadPriority() {
    DWORD currentPid = GetCurrentProcessId();
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return;

    THREADENTRY32 te;
    te.dwSize = sizeof(THREADENTRY32);

    DWORD primaryThreadId = 0;
    ULONGLONG minCreationTime = MAXULONGLONG;

    if (Thread32First(hSnapshot, &te)) {
        do {
            if (te.th32OwnerProcessID == currentPid) {
                HANDLE hThread = OpenThread(THREAD_QUERY_INFORMATION | THREAD_SET_INFORMATION, FALSE, te.th32ThreadID);
                if (hThread) {
                    FILETIME ftCreate, ftExit, ftKernel, ftUser;
                    if (GetThreadTimes(hThread, &ftCreate, &ftExit, &ftKernel, &ftUser)) {
                        ULARGE_INTEGER uli;
                        uli.LowPart = ftCreate.dwLowDateTime;
                        uli.HighPart = ftCreate.dwHighDateTime;
                        if (uli.QuadPart < minCreationTime) {
                            minCreationTime = uli.QuadPart;
                            primaryThreadId = te.th32ThreadID;
                        }
                    }
                    CloseHandle(hThread);
                }
            }
        } while (Thread32Next(hSnapshot, &te));
    }
    CloseHandle(hSnapshot);

    if (primaryThreadId != 0) {
        HANDLE hMain = OpenThread(THREAD_SET_INFORMATION, FALSE, primaryThreadId);
        if (hMain) {
            SetThreadPriority(hMain, THREAD_PRIORITY_ABOVE_NORMAL);
            CloseHandle(hMain);
            char buf[128];
            sprintf_s(buf, "[Gothic3Optima] Multicore: Boosted main game thread (TID %u) to THREAD_PRIORITY_ABOVE_NORMAL\n", primaryThreadId);
            OutputDebugStringA(buf);
        }
    }
}

// ---------------------------------------------------------------------------
// 5. Configure PhysX 2.4.1 Scene Multithreading
// ---------------------------------------------------------------------------
static void ConfigurePhysXMultithreading(uintptr_t engineBase) {
    g_ppEngineClock = reinterpret_cast<void**>(engineBase + RVA_ENGINE_CLOCK_PTR);
    if (g_ppEngineClock && *g_ppEngineClock) {
        // Offset 0x148 in clock / engine object controls PhysX scene worker threads
        uint32_t* pPhysXThreads = reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(*g_ppEngineClock) + 0x148);
        if (*pPhysXThreads == 0) {
            *pPhysXThreads = (g_targetWorkerThreads >= 4) ? 4 : g_targetWorkerThreads;
            char buf[128];
            sprintf_s(buf, "[Gothic3Optima] Multicore: Enabled PhysX 2.4.1 scene multithreading with %u threads\n", *pPhysXThreads);
            OutputDebugStringA(buf);
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Public Interface
// ---------------------------------------------------------------------------

void InitializeMulticoreManager() {
    if (s_multicoreInitialized) return;

    HMODULE hEngine = GetModuleHandleA("Engine.dll");
    if (!hEngine) {
        OutputDebugStringA("[Gothic3Optima] Multicore Manager: Engine.dll not loaded!\n");
        return;
    }
    uintptr_t engineBase = reinterpret_cast<uintptr_t>(hEngine);

    // 1. Determine optimal worker thread count based on active CPU cores
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    DWORD numCores = si.dwNumberOfProcessors;

    // Scale dynamically: 4 cores -> 4 workers; 6 cores -> 5 workers; 8+ cores -> 6-8 workers
    // Max capped at 8 to remain safely below Genome Engine assertion limit (10).
    if (numCores <= 4) {
        g_targetWorkerThreads = 4;
    } else if (numCores <= 6) {
        g_targetWorkerThreads = 5;
    } else if (numCores <= 8) {
        g_targetWorkerThreads = 6;
    } else {
        g_targetWorkerThreads = 8;
    }

    char buf[256];
    sprintf_s(buf, "[Gothic3Optima] Initializing Multicore Threading Engine: Detected %u logical cores, configured %u streaming worker threads per pool.\n",
        numCores, g_targetWorkerThreads);
    OutputDebugStringA(buf);

    // 2. Hook eCBaseCache::SetNumThreadsAndCreate (RVA 0x1EF8F0)
    void* pSetNumThreads = reinterpret_cast<void*>(engineBase + RVA_BASECACHE_SETNUMTHREADS);
    if (pSetNumThreads) {
        const BYTE expectedBytes[] = { 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x0C };
        if (memcmp(pSetNumThreads, expectedBytes, sizeof(expectedBytes)) == 0) {
            if (MH_CreateHook(pSetNumThreads, (LPVOID)Hooked_BaseCache_SetNumThreads, (LPVOID*)&Real_BaseCache_SetNumThreads) == MH_OK) {
                MH_EnableHook(pSetNumThreads);
                OutputDebugStringA("[Gothic3Optima] Multicore: Hooked eCBaseCache::SetNumThreadsAndCreate.\n");
            }
        }
    }

    // 3. Hook eCThreadPool::Create (RVA 0x1F4010)
    void* pThreadPoolCreate = (void*)GetProcAddress(hEngine, "?Create@eCThreadPool@@QAE_NK@Z");
    if (!pThreadPoolCreate) {
        pThreadPoolCreate = reinterpret_cast<void*>(engineBase + RVA_THREADPOOL_CREATE);
    }
    if (pThreadPoolCreate) {
        if (MH_CreateHook(pThreadPoolCreate, (LPVOID)Hooked_ThreadPool_Create, (LPVOID*)&Real_ThreadPool_Create) == MH_OK) {
            MH_EnableHook(pThreadPoolCreate);
            OutputDebugStringA("[Gothic3Optima] Multicore: Hooked eCThreadPool::Create.\n");
        }
    }

    // 4. Hook eCMainCache::AccessDisableThreading (RVA 0x65B0)
    void* pAccessDisableThreading = (void*)GetProcAddress(hEngine, "?AccessDisableThreading@eCMainCache@@QAEAA_NXZ");
    if (!pAccessDisableThreading) {
        pAccessDisableThreading = reinterpret_cast<void*>(engineBase + RVA_MAINCACHE_ACCESS_DISABLE_THREADING);
    }
    if (pAccessDisableThreading) {
        if (MH_CreateHook(pAccessDisableThreading, (LPVOID)Hooked_AccessDisableThreading, (LPVOID*)&Real_AccessDisableThreading) == MH_OK) {
            MH_EnableHook(pAccessDisableThreading);
            OutputDebugStringA("[Gothic3Optima] Multicore: Hooked eCMainCache::AccessDisableThreading (enforcing active threading).\n");
        }
    }

    // 5. Force unlock all threading queues via eCThreadPool::EnableAllThreads(true)
    g_pfnEnableAllThreads = (EnableAllThreads_t)GetProcAddress(hEngine, "?EnableAllThreads@eCThreadPool@@SGX_N@Z");
    if (!g_pfnEnableAllThreads) {
        g_pfnEnableAllThreads = reinterpret_cast<EnableAllThreads_t>(engineBase + RVA_THREADPOOL_ENABLE_ALL_THREADS);
    }
    if (g_pfnEnableAllThreads) {
        g_pfnEnableAllThreads(true);
        OutputDebugStringA("[Gothic3Optima] Multicore: Called eCThreadPool::EnableAllThreads(true).\n");
    }

    // 6. Resolve global thread pool metrics
    g_pfnGetGlobalJobsInQueue = (GetGlobalThreadStats_t)GetProcAddress(hEngine, "?GetGlobalNumberOfJobsInQueue@eCThreadPool@@SGKXZ");
    if (!g_pfnGetGlobalJobsInQueue) {
        g_pfnGetGlobalJobsInQueue = reinterpret_cast<GetGlobalThreadStats_t>(engineBase + RVA_THREADPOOL_GET_GLOBAL_JOBS);
    }

    g_pfnGetGlobalThreadsWorking = (GetGlobalThreadStats_t)GetProcAddress(hEngine, "?GetGlobalNumberOfThreadsWorking@eCThreadPool@@SGKXZ");
    if (!g_pfnGetGlobalThreadsWorking) {
        g_pfnGetGlobalThreadsWorking = reinterpret_cast<GetGlobalThreadStats_t>(engineBase + RVA_THREADPOOL_GET_GLOBAL_WORKING);
    }

    // 7. Boost main thread priority
    BoostMainThreadPriority();

    // 8. Configure PhysX multithreading
    ConfigurePhysXMultithreading(engineBase);

    s_multicoreInitialized = true;
    OutputDebugStringA("[Gothic3Optima] Multicore Threading Engine initialized successfully.\n");
}

void ShutdownMulticoreManager() {
    if (!s_multicoreInitialized) return;

    HMODULE hEngine = GetModuleHandleA("Engine.dll");
    if (hEngine) {
        uintptr_t engineBase = reinterpret_cast<uintptr_t>(hEngine);
        void* pSetNumThreads = reinterpret_cast<void*>(engineBase + RVA_BASECACHE_SETNUMTHREADS);
        if (pSetNumThreads) MH_DisableHook(pSetNumThreads);

        void* pThreadPoolCreate = (void*)GetProcAddress(hEngine, "?Create@eCThreadPool@@QAE_NK@Z");
        if (pThreadPoolCreate) MH_DisableHook(pThreadPoolCreate);

        void* pAccessDisableThreading = (void*)GetProcAddress(hEngine, "?AccessDisableThreading@eCMainCache@@QAEAA_NXZ");
        if (pAccessDisableThreading) MH_DisableHook(pAccessDisableThreading);
    }

    s_multicoreInitialized = false;
    OutputDebugStringA("[Gothic3Optima] Multicore Manager shut down.\n");
}

uint32_t GetMulticoreWorkerCount() {
    return g_targetWorkerThreads;
}

uint32_t GetMulticoreActiveJobs() {
    if (g_pfnGetGlobalJobsInQueue) {
        return static_cast<uint32_t>(g_pfnGetGlobalJobsInQueue());
    }
    return 0;
}

uint32_t GetMulticoreWorkingThreads() {
    if (g_pfnGetGlobalThreadsWorking) {
        return static_cast<uint32_t>(g_pfnGetGlobalThreadsWorking());
    }
    return 0;
}
