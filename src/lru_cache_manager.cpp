#include "lru_cache_manager.h"
#include "d3d9_hook.h"
#include <minhook.h>
#include <windows.h>
#include <unordered_map>
#include <stdio.h>
#include <math.h>

namespace LRUCacheManager {

    // ---------------------------------------------------------------------------
    // Function Pointers & Trampolines for eCMainCache
    // ---------------------------------------------------------------------------
    typedef bool (__thiscall* CacheInResource_t)(void* pThis, void* pResourceEntity);
    typedef bool (__thiscall* CacheOutResource_t)(void* pThis, void* pResourceEntity, int behavior);

    static CacheInResource_t  Real_CacheInResource = nullptr;
    static CacheOutResource_t Real_CacheOutResource = nullptr;

    // ---------------------------------------------------------------------------
    // Resource Tracking State
    // ---------------------------------------------------------------------------
    static CRITICAL_SECTION s_lruLock;
    static std::unordered_map<void*, ULONGLONG> s_resourceLastSeen;

    static HANDLE s_hWatchdogThread = NULL;
    static volatile bool s_bRunning = false;

    // ---------------------------------------------------------------------------
    // Biome Definitions (Gothic 3 Coordinates)
    // ---------------------------------------------------------------------------
    enum BiomeType {
        BIOME_UNKNOWN = 0,
        BIOME_MYRTANA = 1,
        BIOME_VARANT = 2,
        BIOME_NORDMAR = 3
    };

    static BiomeType GetBiomeFromPosition(float x, float y, float z) {
        if (z > 35000.0f || (z > 15000.0f && y > 12000.0f)) {
            return BIOME_NORDMAR;
        } else if (z < -25000.0f) {
            return BIOME_VARANT;
        } else {
            return BIOME_MYRTANA;
        }
    }

    static const char* GetBiomeName(BiomeType b) {
        switch (b) {
            case BIOME_MYRTANA: return "Myrtana";
            case BIOME_VARANT: return "Varant";
            case BIOME_NORDMAR: return "Nordmar";
            default: return "Unknown";
        }
    }

    static BiomeType s_currentBiome = BIOME_UNKNOWN;
    static float s_lastHeroX = 0.0f;
    static float s_lastHeroZ = 0.0f;
    static bool s_hasHeroPos = false;

    // ---------------------------------------------------------------------------
    // Hooks
    // ---------------------------------------------------------------------------
    static bool __fastcall Hooked_CacheInResource(void* pThis, void* /*edx*/, void* pResourceEntity) {
        if (!pThis || !pResourceEntity || !Real_CacheInResource) {
            return Real_CacheInResource ? Real_CacheInResource(pThis, pResourceEntity) : false;
        }

        ULONGLONG now = GetTickCount64();
        EnterCriticalSection(&s_lruLock);
        s_resourceLastSeen[pResourceEntity] = now;
        LeaveCriticalSection(&s_lruLock);

        return Real_CacheInResource(pThis, pResourceEntity);
    }

    static bool __fastcall Hooked_CacheOutResource(void* pThis, void* /*edx*/, void* pResourceEntity, int behavior) {
        if (!pThis || !pResourceEntity || !Real_CacheOutResource) {
            return Real_CacheOutResource ? Real_CacheOutResource(pThis, pResourceEntity, behavior) : false;
        }

        // If forced destroy/unload (behavior == 1), always allow eviction
        if (behavior == 1) {
            EnterCriticalSection(&s_lruLock);
            s_resourceLastSeen.erase(pResourceEntity);
            LeaveCriticalSection(&s_lruLock);
            return Real_CacheOutResource(pThis, pResourceEntity, behavior);
        }

        // 1. Emergency OOM Guard: if free virtual memory < 350 MB, evict immediately
        MEMORYSTATUSEX memStatus = { sizeof(MEMORYSTATUSEX) };
        GlobalMemoryStatusEx(&memStatus);
        if (memStatus.ullAvailVirtual < 350ULL * 1024ULL * 1024ULL) {
            EnterCriticalSection(&s_lruLock);
            s_resourceLastSeen.erase(pResourceEntity);
            LeaveCriticalSection(&s_lruLock);
            return Real_CacheOutResource(pThis, pResourceEntity, behavior);
        }

        // 2. Check age of resource in cache
        ULONGLONG now = GetTickCount64();
        ULONGLONG ageMs = 0;
        bool hasRecord = false;
        EnterCriticalSection(&s_lruLock);
        auto it = s_resourceLastSeen.find(pResourceEntity);
        if (it != s_resourceLastSeen.end()) {
            ageMs = now - it->second;
            hasRecord = true;
        }
        LeaveCriticalSection(&s_lruLock);

        // 3. Stale asset eviction (> 300 seconds without access): allow eviction
        if (hasRecord && ageMs >= 300000ULL) {
            EnterCriticalSection(&s_lruLock);
            s_resourceLastSeen.erase(pResourceEntity);
            LeaveCriticalSection(&s_lruLock);
            return Real_CacheOutResource(pThis, pResourceEntity, behavior);
        }

        EnterCriticalSection(&s_lruLock);
        s_resourceLastSeen.erase(pResourceEntity);
        LeaveCriticalSection(&s_lruLock);

        // Always delegate to real engine eviction to keep cache reference counts and state synchronized
        return Real_CacheOutResource(pThis, pResourceEntity, behavior);
    }

    // ---------------------------------------------------------------------------
    // Adaptive Eviction Implementation
    // ---------------------------------------------------------------------------
    void PerformAdaptiveEviction(bool forceAllStale) {
        ULONGLONG now = GetTickCount64();
        ULONGLONG maxAgeMs = forceAllStale ? 30000ULL : 180000ULL; // 30s on biome change, 180s on low memory

        EnterCriticalSection(&s_lruLock);
        for (auto it = s_resourceLastSeen.begin(); it != s_resourceLastSeen.end(); ) {
            if (now - it->second > maxAgeMs) {
                it = s_resourceLastSeen.erase(it);
            } else {
                ++it;
            }
        }
        LeaveCriticalSection(&s_lruLock);

        // Evict managed video memory textures
        EvictD3D9Resources();

        char buf[256];
        MEMORYSTATUSEX memStatus = { sizeof(MEMORYSTATUSEX) };
        GlobalMemoryStatusEx(&memStatus);
        sprintf_s(buf, "[Gothic3Optima] Phase 13.2: Adaptive cache eviction completed. Free virtual memory: %.2f MB\n",
            (double)memStatus.ullAvailVirtual / (1024.0 * 1024.0));
        OutputDebugStringA(buf);
    }

    void NotifyPlayerPosition(float x, float y, float z) {
        BiomeType newBiome = GetBiomeFromPosition(x, y, z);
        bool isTeleport = false;

        if (s_hasHeroPos) {
            float dx = x - s_lastHeroX;
            float dz = z - s_lastHeroZ;
            float distSq = dx * dx + dz * dz;
            if (distSq > 400000000.0f) { // > 200m jump
                isTeleport = true;
            }
        }

        bool biomeChanged = (s_currentBiome != BIOME_UNKNOWN && s_currentBiome != newBiome);

        if (biomeChanged || isTeleport) {
            char buf[256];
            sprintf_s(buf, "[Gothic3Optima] Phase 13.2: Regional transition detected (%s -> %s, teleport=%d). Triggering adaptive LRU flush.\n",
                GetBiomeName(s_currentBiome), GetBiomeName(newBiome), isTeleport ? 1 : 0);
            OutputDebugStringA(buf);

            PerformAdaptiveEviction(true);
        }

        s_currentBiome = newBiome;
        s_lastHeroX = x;
        s_lastHeroZ = z;
        s_hasHeroPos = true;
    }

    // ---------------------------------------------------------------------------
    // Background Memory Watchdog Thread
    // ---------------------------------------------------------------------------
    static DWORD WINAPI WatchdogThreadProc(LPVOID) {
        while (s_bRunning) {
            Sleep(15000); // 15-second check interval
            if (!s_bRunning) break;

            MEMORYSTATUSEX memStatus = { sizeof(MEMORYSTATUSEX) };
            GlobalMemoryStatusEx(&memStatus);

            // If free virtual memory dips below 500 MB, execute proactive sweep
            if (memStatus.ullAvailVirtual < 500ULL * 1024ULL * 1024ULL) {
                PerformAdaptiveEviction(false);
            }
        }
        return 0;
    }

    // ---------------------------------------------------------------------------
    // Lifecycle
    // ---------------------------------------------------------------------------
    bool Initialize() {
        InitializeCriticalSection(&s_lruLock);

        HMODULE hEngine = GetModuleHandleA("Engine.dll");
        if (!hEngine) {
            OutputDebugStringA("[Gothic3Optima] Engine.dll not loaded for LRU cache hooks!\n");
            return false;
        }

        FARPROC pCacheIn = GetProcAddress(hEngine, "?CacheInResource@eCMainCache@@QAE_NAAVeCResourceDataEntity@@@Z");
        FARPROC pCacheOut = GetProcAddress(hEngine, "?CacheOutResource@eCMainCache@@QAE_NAAVeCResourceDataEntity@@W4eEResourceCacheBehavior@@@Z");

        if (pCacheIn) {
            MH_CreateHook(pCacheIn, reinterpret_cast<LPVOID>(&Hooked_CacheInResource), reinterpret_cast<LPVOID*>(&Real_CacheInResource));
            MH_EnableHook(pCacheIn);
        }
        if (pCacheOut) {
            MH_CreateHook(pCacheOut, reinterpret_cast<LPVOID>(&Hooked_CacheOutResource), reinterpret_cast<LPVOID*>(&Real_CacheOutResource));
            MH_EnableHook(pCacheOut);
        }

        // Start background memory watchdog
        s_bRunning = true;
        s_hWatchdogThread = CreateThread(nullptr, 0, WatchdogThreadProc, nullptr, 0, nullptr);

        OutputDebugStringA("[Gothic3Optima] Phase 13.2: Adaptive LRU Cache Eviction (eCMainCache) initialized successfully.\n");
        return true;
    }

    void Shutdown() {
        s_bRunning = false;
        if (s_hWatchdogThread) {
            WaitForSingleObject(s_hWatchdogThread, 2000);
            CloseHandle(s_hWatchdogThread);
            s_hWatchdogThread = NULL;
        }

        DeleteCriticalSection(&s_lruLock);
    }

} // namespace LRUCacheManager
