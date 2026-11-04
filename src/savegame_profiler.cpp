#include "savegame_profiler.h"
#include "diagnostic_logger.h"
#include "fast_pak_decompressor.h"
#include "property_cache_optimizer.h"
#include "vfs_cache_optimizer.h"
#include <minhook.h>
#include <windows.h>
#include <stdio.h>

namespace G3O {

static LARGE_INTEGER s_freq = []() {
    LARGE_INTEGER li;
    QueryPerformanceFrequency(&li);
    if (li.QuadPart <= 0) li.QuadPart = 10000000LL;
    return li;
}();

// -----------------------------------------------------------------------------
// ReadFile I/O Tracker
// -----------------------------------------------------------------------------
typedef BOOL (WINAPI* ReadFile_t)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
static ReadFile_t Real_ReadFile = nullptr;
static volatile LONG   s_readCalls = 0;
static volatile LONG64 s_readBytes = 0;
static volatile LONG64 s_readTimeUs = 0;
static volatile bool   s_isLoadActive = false;

static BOOL WINAPI Hooked_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD pRead, LPOVERLAPPED ov) {
    if (!s_isLoadActive) {
        return Real_ReadFile(h, buf, n, pRead, ov);
    }
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    BOOL r = Real_ReadFile(h, buf, n, pRead, ov);
    QueryPerformanceCounter(&t1);
    if (pRead && r) {
        InterlockedExchangeAdd64(&s_readBytes, (LONG64)*pRead);
    }
    InterlockedIncrement(&s_readCalls);
    LONG64 us = (s_freq.QuadPart > 0) ? ((t1.QuadPart - t0.QuadPart) * 1000000LL / s_freq.QuadPart) : 0;
    InterlockedExchangeAdd64(&s_readTimeUs, us);
    return r;
}

// -----------------------------------------------------------------------------
// Detailed Phase Timers
// -----------------------------------------------------------------------------
static LARGE_INTEGER s_tHUDStart = {};
static LARGE_INTEGER s_tWorldStart = {};
static LARGE_INTEGER s_tWorldEnd = {};

static double s_preWorldSec = 0.0;
static double s_loadGameWorldSec = 0.0;
static double s_sessionStartSec = 0.0;
static double s_sessionStopSec = 0.0;
static double s_worldActivateSec = 0.0;
static volatile LONG   s_cellsProcessed = 0;
static volatile LONG64 s_hierarchyTimeUs = 0;

static void LogLoadReport(double totalSec) {
    LONG rc = s_readCalls;
    double rbMb = (double)s_readBytes / (1024.0 * 1024.0);
    double rSec = (double)s_readTimeUs / 1000000.0;

    LONG pakCount = GetPakDecompCount();
    double pakInMb = (double)GetPakDecompInBytes() / (1024.0 * 1024.0);
    double pakOutMb = (double)GetPakDecompOutBytes() / (1024.0 * 1024.0);
    double pakSec = GetPakDecompTimeMs() / 1000.0;

    LONG saveCount = GetSavegameDecompCount();
    double saveInMb = (double)GetSavegameDecompInBytes() / (1024.0 * 1024.0);
    double saveOutMb = (double)GetSavegameDecompOutBytes() / (1024.0 * 1024.0);
    double saveSec = GetSavegameDecompTimeMs() / 1000.0;

    LONG strHits = GetPropertyCacheStringHits();
    LONG strMiss = GetPropertyCacheStringMisses();
    LONG idHits = GetPropertyCacheIdHits();
    LONG idMiss = GetPropertyCacheIdMisses();
    LONG totalProps = strHits + strMiss + idHits + idMiss;
    double propHitRate = totalProps ? (100.0 * (strHits + idHits) / totalProps) : 0.0;

    LONG cells = s_cellsProcessed;
    double hierSec = (double)s_hierarchyTimeUs / 1000000.0;

    double preSec = s_preWorldSec;
    double worldSec = s_loadGameWorldSec;
    double startSec = s_sessionStartSec;
    double otherPostSec = totalSec - (preSec + worldSec + startSec);
    if (otherPostSec < 0.0) otherPostSec = 0.0;

    Log("G3O", "================================================================================");
    Log("G3O", "[LoadProfile] === SAVEGAME LOAD BENCHMARK (Total: %.3f s) ===", totalSec);
    Log("G3O", "  Major Engine Phases:");
    Log("G3O", "    1. Pre-Load Preparation:        %6.3f s (%5.1f%%) [gCSession::Stop: %.3f s]",
        preSec, totalSec > 0 ? (preSec * 100.0 / totalSec) : 0, s_sessionStopSec);
    Log("G3O", "    2. gCWorld::LoadGameWorld:      %6.3f s (%5.1f%%)",
        worldSec, totalSec > 0 ? (worldSec * 100.0 / totalSec) : 0);
    Log("G3O", "    3. gCSession::Start (World):    %6.3f s (%5.1f%%)",
        startSec, totalSec > 0 ? (startSec * 100.0 / totalSec) : 0);
    Log("G3O", "    4. Other Post-Processing:       %6.3f s (%5.1f%%)",
        otherPostSec, totalSec > 0 ? (otherPostSec * 100.0 / totalSec) : 0);
    Log("G3O", "         * Cell Hierarchy Build:    %6.3f s [%ld cells processed]", hierSec, cells);
    Log("G3O", "  Subsystem Activity:");
    Log("G3O", "    - Disk ReadFile I/O:            %6.3f s (%5.1f%%) [%ld calls, %.2f MB read, avg speed %.1f MB/s]",
        rSec, totalSec > 0 ? (rSec * 100.0 / totalSec) : 0, rc, rbMb, rSec > 0 ? (rbMb / rSec) : 0.0);
    Log("G3O", "    - Pak Decompress (libdeflate):  %6.3f s (%5.1f%%) [%ld blocks, %.2f MB -> %.2f MB]",
        pakSec, totalSec > 0 ? (pakSec * 100.0 / totalSec) : 0, pakCount, pakInMb, pakOutMb);
    Log("G3O", "    - Save Decompress (libdeflate): %6.3f s (%5.1f%%) [%ld chunks, %.2f MB -> %.2f MB]",
        saveSec, totalSec > 0 ? (saveSec * 100.0 / totalSec) : 0, saveCount, saveInMb, saveOutMb);
    Log("G3O", "    - Property Resolution Cache:    %ld resolves (%ld hits, %ld misses, %.1f%% hit rate)",
        totalProps, strHits + idHits, strMiss + idMiss, propHitRate);
    Log("G3O", "================================================================================");

    FILE* fp = fopen("G3O_LoadProfiler.log", "w");
    if (fp) {
        fprintf(fp, "================================================================================\n");
        fprintf(fp, "[LoadProfile] === SAVEGAME LOAD BENCHMARK (Total: %.3f s) ===\n", totalSec);
        fprintf(fp, "--------------------------------------------------------------------------------\n");
        fprintf(fp, "Phase Breakdown:\n");
        fprintf(fp, "  1. Pre-Load Preparation:        %6.3f s (%5.1f%%) [gCSession::Stop: %.3f s]\n",
            preSec, totalSec > 0 ? (preSec * 100.0 / totalSec) : 0, s_sessionStopSec);
        fprintf(fp, "  2. gCWorld::LoadGameWorld:      %6.3f s (%5.1f%%)\n",
            worldSec, totalSec > 0 ? (worldSec * 100.0 / totalSec) : 0);
        fprintf(fp, "  3. gCSession::Start (World):    %6.3f s (%5.1f%%)\n",
            startSec, totalSec > 0 ? (startSec * 100.0 / totalSec) : 0);
        fprintf(fp, "  4. Other Post-Processing:       %6.3f s (%5.1f%%)\n",
            otherPostSec, totalSec > 0 ? (otherPostSec * 100.0 / totalSec) : 0);
        fprintf(fp, "       * Cell Hierarchy Build:    %6.3f s [%ld cells processed]\n", hierSec, cells);
        fprintf(fp, "--------------------------------------------------------------------------------\n");
        fprintf(fp, "Subsystem Activity:\n");
        fprintf(fp, "  - Disk ReadFile I/O:             %6.3f s (%5.1f%%) [%ld calls, %.2f MB read, speed %.1f MB/s]\n",
            rSec, totalSec > 0 ? (rSec * 100.0 / totalSec) : 0, rc, rbMb, rSec > 0 ? (rbMb / rSec) : 0.0);
        fprintf(fp, "  - Pak Decompress (libdeflate):   %6.3f s (%5.1f%%) [%ld blocks, %.2f MB -> %.2f MB]\n",
            pakSec, totalSec > 0 ? (pakSec * 100.0 / totalSec) : 0, pakCount, pakInMb, pakOutMb);
        fprintf(fp, "  - Save Decompress (libdeflate):  %6.3f s (%5.1f%%) [%ld chunks, %.2f MB -> %.2f MB]\n",
            saveSec, totalSec > 0 ? (saveSec * 100.0 / totalSec) : 0, saveCount, saveInMb, saveOutMb);
        fprintf(fp, "  - Property Resolution Cache:     %ld resolves (%ld hits, %ld misses, %.1f%% hit rate)\n",
            totalProps, strHits + idHits, strMiss + idMiss, propHitRate);
        fprintf(fp, "================================================================================\n");
        fclose(fp);
    }
}

// -----------------------------------------------------------------------------
// Hooks
// -----------------------------------------------------------------------------
typedef bool (__thiscall* HUDLoadGame_t)(void* pThis, void* a, void* b);
static HUDLoadGame_t Real_HUDLoadGame = nullptr;

static bool __fastcall Hooked_HUDLoadGame(void* pThis, void* /*edx*/, void* a, void* b) {
    s_isLoadActive = true;
    QueryPerformanceCounter(&s_tHUDStart);
    Log("G3O", "================================================================================");
    Log("G3O", "[LoadProfile] >>> SAVEGAME LOAD INITIATED (gCHUDFileManager::LoadGame) >>>");

    ResetDecompTelemetry();
    ResetPropertyCacheTelemetry();
    ResetVFSCacheTelemetry();
    InterlockedExchange(&s_readCalls, 0);
    InterlockedExchange64(&s_readBytes, 0);
    InterlockedExchange64(&s_readTimeUs, 0);
    InterlockedExchange(&s_cellsProcessed, 0);
    InterlockedExchange64(&s_hierarchyTimeUs, 0);
    s_preWorldSec = 0.0;
    s_loadGameWorldSec = 0.0;
    s_sessionStartSec = 0.0;
    s_sessionStopSec = 0.0;
    s_worldActivateSec = 0.0;

    bool r = Real_HUDLoadGame(pThis, a, b);

    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    s_isLoadActive = false;
    double totalSec = (double)(t1.QuadPart - s_tHUDStart.QuadPart) / (double)s_freq.QuadPart;

    LogLoadReport(totalSec);
    return r;
}

static volatile bool s_standaloneLoadWorld = false;

typedef bool (__thiscall* LoadGameWorld_t)(void* pThis, const void* pSaveName, bool isNewGame);
static LoadGameWorld_t Real_LoadGameWorld = nullptr;

static bool __fastcall Hooked_LoadGameWorld(void* pThis, void* /*edx*/, const void* pSaveName, bool isNewGame) {
    bool wasActive = s_isLoadActive;
    if (!wasActive) {
        s_isLoadActive = true;
        s_standaloneLoadWorld = true;
        ResetDecompTelemetry();
        ResetPropertyCacheTelemetry();
        ResetVFSCacheTelemetry();
        InterlockedExchange(&s_readCalls, 0);
        InterlockedExchange64(&s_readBytes, 0);
        InterlockedExchange64(&s_readTimeUs, 0);
        InterlockedExchange(&s_cellsProcessed, 0);
        InterlockedExchange64(&s_hierarchyTimeUs, 0);
        s_preWorldSec = 0.0;
        s_loadGameWorldSec = 0.0;
        s_sessionStartSec = 0.0;
        s_sessionStopSec = 0.0;
        s_worldActivateSec = 0.0;
        QueryPerformanceCounter(&s_tHUDStart);
        Log("G3O", "================================================================================");
        Log("G3O", "[LoadProfile] >>> SAVEGAME LOAD INITIATED (gCWorld::LoadGameWorld) >>>");
    }

    QueryPerformanceCounter(&s_tWorldStart);
    if (s_tHUDStart.QuadPart > 0) {
        s_preWorldSec = (double)(s_tWorldStart.QuadPart - s_tHUDStart.QuadPart) / (double)s_freq.QuadPart;
    }
    Log("G3O", "[LoadProfile] -> gCWorld::LoadGameWorld START (pre-world: %.3f s)", s_preWorldSec);

    bool r = Real_LoadGameWorld(pThis, pSaveName, isNewGame);

    QueryPerformanceCounter(&s_tWorldEnd);
    s_loadGameWorldSec = (double)(s_tWorldEnd.QuadPart - s_tWorldStart.QuadPart) / (double)s_freq.QuadPart;
    Log("G3O", "[LoadProfile] <- gCWorld::LoadGameWorld FINISHED in %.3f s (result=%d)", s_loadGameWorldSec, (int)r);
    return r;
}

typedef void (__thiscall* SessionStop_t)(void* pThis);
static SessionStop_t Real_SessionStop = nullptr;

static void __fastcall Hooked_SessionStop(void* pThis, void* /*edx*/) {
    if (!s_isLoadActive) {
        Real_SessionStop(pThis);
        return;
    }
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    Real_SessionStop(pThis);
    QueryPerformanceCounter(&t1);
    s_sessionStopSec = (double)(t1.QuadPart - t0.QuadPart) / (double)s_freq.QuadPart;
    Log("G3O", "[LoadProfile] gCSession::Stop took %.3f s", s_sessionStopSec);
}

typedef void (__thiscall* SessionStart_t)(void* pThis, int mode);
static SessionStart_t Real_SessionStart = nullptr;

static void __fastcall Hooked_SessionStart(void* pThis, void* /*edx*/, int mode) {
    if (!s_isLoadActive) {
        Real_SessionStart(pThis, mode);
        return;
    }
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    Log("G3O", "[LoadProfile] -> gCSession::Start (mode=%d) START", mode);

    Real_SessionStart(pThis, mode);

    QueryPerformanceCounter(&t1);
    s_sessionStartSec = (double)(t1.QuadPart - t0.QuadPart) / (double)s_freq.QuadPart;
    Log("G3O", "[LoadProfile] <- gCSession::Start FINISHED in %.3f s", s_sessionStartSec);

    if (s_standaloneLoadWorld) {
        s_standaloneLoadWorld = false;
        s_isLoadActive = false;
        LogLoadReport(s_preWorldSec + s_loadGameWorldSec + s_sessionStartSec);
    }
}

typedef int (__thiscall* WorldActivate_t)(void* pThis);
static WorldActivate_t Real_WorldActivate = nullptr;

static int __fastcall Hooked_WorldActivate(void* pThis, void* /*edx*/) {
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);

    int r = Real_WorldActivate(pThis);

    QueryPerformanceCounter(&t1);
    s_worldActivateSec = (double)(t1.QuadPart - t0.QuadPart) / (double)s_freq.QuadPart;
    return r;
}

typedef void (__thiscall* BuildStaticHierachy_t)(void* pThis);
static BuildStaticHierachy_t Real_BuildStaticHierachy = nullptr;

static void __fastcall Hooked_BuildStaticHierachy(void* pThis, void* /*edx*/) {
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);

    Real_BuildStaticHierachy(pThis);

    QueryPerformanceCounter(&t1);
    InterlockedIncrement(&s_cellsProcessed);
    LONG64 us = (s_freq.QuadPart > 0) ? ((t1.QuadPart - t0.QuadPart) * 1000000LL / s_freq.QuadPart) : 0;
    InterlockedExchangeAdd64(&s_hierarchyTimeUs, us);
}

static void* FindHUDLoadGameTarget(HMODULE hGame) {
    if (!hGame) return nullptr;

    const BYTE sig[] = {
        0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x8B, 0x45, 0x08,
        0x83, 0xEC, 0x5C, 0x83, 0x78, 0x04, 0x00, 0x53, 0x56, 0x57
    };
    const size_t sigLen = sizeof(sig);

    BYTE* targetRva = (BYTE*)hGame + 0xA9A10;
    if (memcmp(targetRva, sig, sigLen) == 0) {
        return targetRva;
    }

    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)hGame;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((BYTE*)hGame + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    IMAGE_SECTION_HEADER* sect = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (memcmp(sect[i].Name, ".text", 5) == 0) {
            BYTE* start = (BYTE*)hGame + sect[i].VirtualAddress;
            DWORD size = sect[i].Misc.VirtualSize;
            if (size > sigLen) {
                for (DWORD offset = 0; offset <= size - sigLen; ++offset) {
                    if (memcmp(start + offset, sig, sigLen) == 0) {
                        return start + offset;
                    }
                }
            }
        }
    }

    return nullptr;
}

static void* s_targetReadFile = nullptr;
static void* s_targetHUDLoadGame = nullptr;
static void* s_targetLoadGameWorld = nullptr;
static void* s_targetSessionStop = nullptr;
static void* s_targetSessionStart = nullptr;
static void* s_targetWorldActivate = nullptr;
static void* s_targetBuildStatic = nullptr;

bool InitializeSavegameProfiler() {
    HMODULE hK32 = GetModuleHandleA("kernel32.dll");
    if (hK32) {
        void* pRead = (void*)GetProcAddress(hK32, "ReadFile");
        if (pRead) {
            s_targetReadFile = pRead;
            MH_CreateHook(pRead, (LPVOID)Hooked_ReadFile, (LPVOID*)&Real_ReadFile);
            MH_EnableHook(pRead);
        }
    }

    HMODULE hGame = GetModuleHandleA("Game.dll");
    if (hGame) {
        void* pHUD = FindHUDLoadGameTarget(hGame);
        if (pHUD) {
            s_targetHUDLoadGame = pHUD;
            MH_CreateHook(pHUD, (LPVOID)Hooked_HUDLoadGame, (LPVOID*)&Real_HUDLoadGame);
            MH_EnableHook(pHUD);
            Log("G3O", "Savegame Profiler: Hooked gCHUDFileManager::LoadGame (0x%p)", pHUD);
        }

        void* pWorld = (void*)GetProcAddress(hGame, "?LoadGameWorld@gCWorld@@QAE_NABVbCString@@_N@Z");
        if (pWorld) {
            s_targetLoadGameWorld = pWorld;
            MH_CreateHook(pWorld, (LPVOID)Hooked_LoadGameWorld, (LPVOID*)&Real_LoadGameWorld);
            MH_EnableHook(pWorld);
            Log("G3O", "Savegame Profiler: Hooked gCWorld::LoadGameWorld (0x%p)", pWorld);
        }

        void* pStop = (void*)GetProcAddress(hGame, "?Stop@gCSession@@QAEXXZ");
        if (pStop) {
            s_targetSessionStop = pStop;
            MH_CreateHook(pStop, (LPVOID)Hooked_SessionStop, (LPVOID*)&Real_SessionStop);
            MH_EnableHook(pStop);
            Log("G3O", "Savegame Profiler: Hooked gCSession::Stop (0x%p)", pStop);
        }

        void* pStart = (void*)GetProcAddress(hGame, "?Start@gCSession@@QAEXW4gESession_StartMode@@@Z");
        if (pStart) {
            s_targetSessionStart = pStart;
            MH_CreateHook(pStart, (LPVOID)Hooked_SessionStart, (LPVOID*)&Real_SessionStart);
            MH_EnableHook(pStart);
            Log("G3O", "Savegame Profiler: Hooked gCSession::Start (0x%p)", pStart);
        }

        void* pAct = (void*)GetProcAddress(hGame, "?WorldActivate@gCSession@@UAE?AW4bEResult@@XZ");
        if (pAct) {
            s_targetWorldActivate = pAct;
            MH_CreateHook(pAct, (LPVOID)Hooked_WorldActivate, (LPVOID*)&Real_WorldActivate);
            MH_EnableHook(pAct);
            Log("G3O", "Savegame Profiler: Hooked gCSession::WorldActivate (0x%p)", pAct);
        }
    }

    HMODULE hEngine = GetModuleHandleA("Engine.dll");
    if (hEngine) {
        void* pBuild = (void*)GetProcAddress(hEngine, "?BuildStaticHierachy@eCPVSCellItem@@IAEXXZ");
        if (pBuild) {
            s_targetBuildStatic = pBuild;
            MH_CreateHook(pBuild, (LPVOID)Hooked_BuildStaticHierachy, (LPVOID*)&Real_BuildStaticHierachy);
            MH_EnableHook(pBuild);
            Log("G3O", "Savegame Profiler: Hooked eCPVSCellItem::BuildStaticHierachy (0x%p)", pBuild);
        }
    }

    Log("G3O", "Savegame Profiler initialized. Full telemetry active for next save load.");
    return true;
}

void ShutdownSavegameProfiler() {
    if (s_targetReadFile) MH_DisableHook(s_targetReadFile);
    if (s_targetHUDLoadGame) MH_DisableHook(s_targetHUDLoadGame);
    if (s_targetLoadGameWorld) MH_DisableHook(s_targetLoadGameWorld);
    if (s_targetSessionStop) MH_DisableHook(s_targetSessionStop);
    if (s_targetSessionStart) MH_DisableHook(s_targetSessionStart);
    if (s_targetWorldActivate) MH_DisableHook(s_targetWorldActivate);
    if (s_targetBuildStatic) MH_DisableHook(s_targetBuildStatic);
}

} // namespace G3O
