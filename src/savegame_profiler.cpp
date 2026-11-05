#include "savegame_profiler.h"
#include "diagnostic_logger.h"
#include "fast_pak_decompressor.h"
#include "parallel_pak_decompressor.h"
#include "world_deactivate_optimizer.h"
#include "property_cache_optimizer.h"
#include "vfs_cache_optimizer.h"
#include <minhook.h>
#include <windows.h>
#include <stdio.h>
#include <string>

namespace G3O {

static std::string s_lastSaveName = "QuickSave";

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
// Sub-Phase Measurements
// -----------------------------------------------------------------------------
struct PhaseStats {
    double durationSec = 0.0;
    LONG   pakBlocks = 0;
    double pakOutMb = 0.0;
    LONG   readCalls = 0;
    double readMb = 0.0;
};

static LARGE_INTEGER s_tHUDStart = {};
static LARGE_INTEGER s_tWorldStart = {};

static PhaseStats s_statHeader = {};
static PhaseStats s_statSessionStop = {};
static PhaseStats s_statClearEntities = {};
static PhaseStats s_statUnloadCaches = {};
static PhaseStats s_statLoadGameWorld = {};
static PhaseStats s_statGotoPos = {};
static PhaseStats s_statSessionStart = {};
static PhaseStats s_statWorldActivate = {};

static double s_preWorldTotalSec = 0.0;
static volatile LONG   s_cellsProcessed = 0;
static volatile LONG64 s_hierarchyTimeUs = 0;

static inline void SnapshotPhase(PhaseStats& st, LARGE_INTEGER t0, LARGE_INTEGER t1,
                                 LONG pakCount0, LONG64 pakOut0, LONG read0, LONG64 readBytes0) {
    st.durationSec = (double)(t1.QuadPart - t0.QuadPart) / (double)s_freq.QuadPart;
    st.pakBlocks = GetPakDecompCount() - pakCount0;
    st.pakOutMb = (double)(GetPakDecompOutBytes() - pakOut0) / (1024.0 * 1024.0);
    st.readCalls = s_readCalls - read0;
    st.readMb = (double)(s_readBytes - readBytes0) / (1024.0 * 1024.0);
}

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

    double preKnown = s_statHeader.durationSec + s_statSessionStop.durationSec + s_statClearEntities.durationSec + s_statUnloadCaches.durationSec;
    double preOther = s_preWorldTotalSec - preKnown;
    if (preOther < 0.0) preOther = 0.0;

    double postKnown = s_statGotoPos.durationSec + s_statSessionStart.durationSec;
    double postTotal = totalSec - (s_preWorldTotalSec + s_statLoadGameWorld.durationSec);
    if (postTotal < 0.0) postTotal = 0.0;
    double postOther = postTotal - postKnown;
    if (postOther < 0.0) postOther = 0.0;

    Log("G3O", "================================================================================");
    Log("G3O", "[LoadProfile] === DETAILED SAVEGAME LOAD BENCHMARK (Total: %.3f s) ===", totalSec);
    Log("G3O", "--------------------------------------------------------------------------------");
    Log("G3O", "Detailed Phase Breakdown:");
    Log("G3O", "  1. Pre-Load Preparation:          %6.3f s (%5.1f%%)",
        s_preWorldTotalSec, totalSec > 0 ? (s_preWorldTotalSec * 100.0 / totalSec) : 0.0);
    Log("G3O", "       * GetSaveGameHeader:         %6.3f s [%ld pak blocks, %.2f MB read]",
        s_statHeader.durationSec, s_statHeader.pakBlocks, s_statHeader.readMb);
    Log("G3O", "       * gCSession::Stop:           %6.3f s", s_statSessionStop.durationSec);
    Log("G3O", "       * gCGUIManager::ClearEnt:    %6.3f s", s_statClearEntities.durationSec);
    Log("G3O", "       * gCWorld::UnloadCaches:     %6.3f s [%ld pak blocks, %.2f MB read]",
        s_statUnloadCaches.durationSec, s_statUnloadCaches.pakBlocks, s_statUnloadCaches.readMb);
    Log("G3O", "       * Other Pre-Load Overhead:   %6.3f s", preOther);
    Log("G3O", "  2. gCWorld::LoadGameWorld:        %6.3f s (%5.1f%%) [%ld pak blocks, %.2f MB decomp, %.2f MB read]",
        s_statLoadGameWorld.durationSec, totalSec > 0 ? (s_statLoadGameWorld.durationSec * 100.0 / totalSec) : 0.0,
        s_statLoadGameWorld.pakBlocks, s_statLoadGameWorld.pakOutMb, s_statLoadGameWorld.readMb);
    Log("G3O", "  3. Post-Load Processing:          %6.3f s (%5.1f%%)",
        postTotal, totalSec > 0 ? (postTotal * 100.0 / totalSec) : 0.0);
    Log("G3O", "       * gCSession::GotoLoadedPos:  %6.3f s [%ld pak blocks, %.2f MB decomp, %.2f MB read]",
        s_statGotoPos.durationSec, s_statGotoPos.pakBlocks, s_statGotoPos.pakOutMb, s_statGotoPos.readMb);
    Log("G3O", "       * gCSession::Start (World):  %6.3f s [%ld pak blocks, %.2f MB read]",
        s_statSessionStart.durationSec, s_statSessionStart.pakBlocks, s_statSessionStart.readMb);
    Log("G3O", "       * Other Post Overhead:       %6.3f s", postOther);
    Log("G3O", "--------------------------------------------------------------------------------");
    Log("G3O", "Subsystem Activity:");
    Log("G3O", "  - Disk ReadFile I/O:              %6.3f s (%5.1f%%) [%ld calls, %.2f MB read, avg speed %.1f MB/s]",
        rSec, totalSec > 0 ? (rSec * 100.0 / totalSec) : 0.0, rc, rbMb, rSec > 0 ? (rbMb / rSec) : 0.0);
    Log("G3O", "  - Pak Decompress (libdeflate):    %6.3f s (%5.1f%%) [%ld blocks, %.2f MB -> %.2f MB]",
        pakSec, totalSec > 0 ? (pakSec * 100.0 / totalSec) : 0.0, pakCount, pakInMb, pakOutMb);
    Log("G3O", "  - Save Decompress (libdeflate):   %6.3f s (%5.1f%%) [%ld chunks, %.2f MB -> %.2f MB]",
        saveSec, totalSec > 0 ? (saveSec * 100.0 / totalSec) : 0.0, saveCount, saveInMb, saveOutMb);
    Log("G3O", "  - Property Resolution Cache:      %ld resolves (%ld hits, %ld misses, %.1f%% hit rate)",
        totalProps, strHits + idHits, strMiss + idMiss, propHitRate);
    Log("G3O", "  - World Deactivate Optimizer:     Last duration: %.3f s", GetLastWorldDeactivateDurationSec());
    Log("G3O", "  - Parallel Pak Prefetcher:        %ld hits, %ld misses (est. %.2f s saved)",
        GetPakPrefetchHits(), GetPakPrefetchMisses(), GetPakPrefetchTimeSavedSec());
    Log("G3O", "================================================================================");

    FILE* fp = fopen("G3O_LoadProfiler.log", "w");
    if (fp) {
        fprintf(fp, "================================================================================\n");
        fprintf(fp, "[LoadProfile] === DETAILED SAVEGAME LOAD BENCHMARK (Total: %.3f s) ===\n", totalSec);
        fprintf(fp, "--------------------------------------------------------------------------------\n");
        fprintf(fp, "Detailed Phase Breakdown:\n");
        fprintf(fp, "  1. Pre-Load Preparation:          %6.3f s (%5.1f%%)\n",
            s_preWorldTotalSec, totalSec > 0 ? (s_preWorldTotalSec * 100.0 / totalSec) : 0.0);
        fprintf(fp, "       * GetSaveGameHeader:         %6.3f s [%ld pak blocks, %.2f MB read]\n",
            s_statHeader.durationSec, s_statHeader.pakBlocks, s_statHeader.readMb);
        fprintf(fp, "       * gCSession::Stop:           %6.3f s\n", s_statSessionStop.durationSec);
        fprintf(fp, "       * gCGUIManager::ClearEnt:    %6.3f s\n", s_statClearEntities.durationSec);
        fprintf(fp, "       * gCWorld::UnloadCaches:     %6.3f s [%ld pak blocks, %.2f MB read]\n",
            s_statUnloadCaches.durationSec, s_statUnloadCaches.pakBlocks, s_statUnloadCaches.readMb);
        fprintf(fp, "       * Other Pre-Load Overhead:   %6.3f s\n", preOther);
        fprintf(fp, "  2. gCWorld::LoadGameWorld:        %6.3f s (%5.1f%%) [%ld pak blocks, %.2f MB decomp, %.2f MB read]\n",
            s_statLoadGameWorld.durationSec, totalSec > 0 ? (s_statLoadGameWorld.durationSec * 100.0 / totalSec) : 0.0,
            s_statLoadGameWorld.pakBlocks, s_statLoadGameWorld.pakOutMb, s_statLoadGameWorld.readMb);
        fprintf(fp, "  3. Post-Load Processing:          %6.3f s (%5.1f%%)\n",
            postTotal, totalSec > 0 ? (postTotal * 100.0 / totalSec) : 0.0);
        fprintf(fp, "       * gCSession::GotoLoadedPos:  %6.3f s [%ld pak blocks, %.2f MB decomp, %.2f MB read]\n",
            s_statGotoPos.durationSec, s_statGotoPos.pakBlocks, s_statGotoPos.pakOutMb, s_statGotoPos.readMb);
        fprintf(fp, "       * gCSession::Start (World):  %6.3f s [%ld pak blocks, %.2f MB read]\n",
            s_statSessionStart.durationSec, s_statSessionStart.pakBlocks, s_statSessionStart.readMb);
        fprintf(fp, "       * Other Post Overhead:       %6.3f s\n", postOther);
        fprintf(fp, "--------------------------------------------------------------------------------\n");
        fprintf(fp, "Subsystem Activity:\n");
        fprintf(fp, "  - Disk ReadFile I/O:              %6.3f s (%5.1f%%) [%ld calls, %.2f MB read, speed %.1f MB/s]\n",
            rSec, totalSec > 0 ? (rSec * 100.0 / totalSec) : 0.0, rc, rbMb, rSec > 0 ? (rbMb / rSec) : 0.0);
        fprintf(fp, "  - Pak Decompress (libdeflate):    %6.3f s (%5.1f%%) [%ld blocks, %.2f MB -> %.2f MB]\n",
            pakSec, totalSec > 0 ? (pakSec * 100.0 / totalSec) : 0.0, pakCount, pakInMb, pakOutMb);
        fprintf(fp, "  - Save Decompress (libdeflate):   %6.3f s (%5.1f%%) [%ld chunks, %.2f MB -> %.2f MB]\n",
            saveSec, totalSec > 0 ? (saveSec * 100.0 / totalSec) : 0.0, saveCount, saveInMb, saveOutMb);
        fprintf(fp, "  - Property Resolution Cache:      %ld resolves (%ld hits, %ld misses, %.1f%% hit rate)\n",
            totalProps, strHits + idHits, strMiss + idMiss, propHitRate);
        fprintf(fp, "  - World Deactivate Optimizer:     Last duration: %.3f s\n", GetLastWorldDeactivateDurationSec());
        fprintf(fp, "  - Parallel Pak Prefetcher:        %ld hits, %ld misses (est. %.2f s saved)\n",
            GetPakPrefetchHits(), GetPakPrefetchMisses(), GetPakPrefetchTimeSavedSec());
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

    s_statHeader = {};
    s_statSessionStop = {};
    s_statClearEntities = {};
    s_statUnloadCaches = {};
    s_statLoadGameWorld = {};
    s_statGotoPos = {};
    s_statSessionStart = {};
    s_statWorldActivate = {};
    s_preWorldTotalSec = 0.0;

    bool r = Real_HUDLoadGame(pThis, a, b);

    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    s_isLoadActive = false;
    double totalSec = (double)(t1.QuadPart - s_tHUDStart.QuadPart) / (double)s_freq.QuadPart;

    LogLoadReport(totalSec);
    return r;
}

// 1. GetSaveGameHeader hook
typedef int (__thiscall* GetSaveGameHeader_t)(void* pThis, const void* pStr, void* pHeader);
static GetSaveGameHeader_t Real_GetSaveGameHeader = nullptr;

static int __fastcall Hooked_GetSaveGameHeader(void* pThis, void* /*edx*/, const void* pStr, void* pHeader) {
    if (pStr) {
        const char* s = *(const char**)pStr;
        if (s && *s) s_lastSaveName = s;
    }
    if (!s_isLoadActive) return Real_GetSaveGameHeader(pThis, pStr, pHeader);
    LARGE_INTEGER t0, t1;
    LONG pak0 = GetPakDecompCount(); LONG64 pakOut0 = GetPakDecompOutBytes();
    LONG read0 = s_readCalls; LONG64 readBytes0 = s_readBytes;
    QueryPerformanceCounter(&t0);

    int r = Real_GetSaveGameHeader(pThis, pStr, pHeader);

    QueryPerformanceCounter(&t1);
    SnapshotPhase(s_statHeader, t0, t1, pak0, pakOut0, read0, readBytes0);
    Log("G3O", "[LoadProfile] gCWorld::GetSaveGameHeader took %.3f s", s_statHeader.durationSec);
    return r;
}

// 2. SessionStop hook
typedef void (__thiscall* SessionStop_t)(void* pThis);
static SessionStop_t Real_SessionStop = nullptr;

static void __fastcall Hooked_SessionStop(void* pThis, void* /*edx*/) {
    if (!s_isLoadActive) { Real_SessionStop(pThis); return; }
    LARGE_INTEGER t0, t1;
    LONG pak0 = GetPakDecompCount(); LONG64 pakOut0 = GetPakDecompOutBytes();
    LONG read0 = s_readCalls; LONG64 readBytes0 = s_readBytes;
    QueryPerformanceCounter(&t0);

    Real_SessionStop(pThis);

    QueryPerformanceCounter(&t1);
    SnapshotPhase(s_statSessionStop, t0, t1, pak0, pakOut0, read0, readBytes0);
    Log("G3O", "[LoadProfile] gCSession::Stop took %.3f s", s_statSessionStop.durationSec);
}

// 3. ClearAllEntities hook
typedef bool (__thiscall* ClearAllEntities_t)(void* pThis);
static ClearAllEntities_t Real_ClearAllEntities = nullptr;

static bool __fastcall Hooked_ClearAllEntities(void* pThis, void* /*edx*/) {
    if (!s_isLoadActive) return Real_ClearAllEntities(pThis);
    LARGE_INTEGER t0, t1;
    LONG pak0 = GetPakDecompCount(); LONG64 pakOut0 = GetPakDecompOutBytes();
    LONG read0 = s_readCalls; LONG64 readBytes0 = s_readBytes;
    QueryPerformanceCounter(&t0);

    bool r = Real_ClearAllEntities(pThis);

    QueryPerformanceCounter(&t1);
    SnapshotPhase(s_statClearEntities, t0, t1, pak0, pakOut0, read0, readBytes0);
    Log("G3O", "[LoadProfile] gCGUIManager::ClearAllEntities took %.3f s", s_statClearEntities.durationSec);
    return r;
}

// 4. UnloadCaches hook
typedef void (__thiscall* UnloadCaches_t)(void* pThis);
static UnloadCaches_t Real_UnloadCaches = nullptr;

static void __fastcall Hooked_UnloadCaches(void* pThis, void* /*edx*/) {
    if (!s_isLoadActive) { Real_UnloadCaches(pThis); return; }
    LARGE_INTEGER t0, t1;
    LONG pak0 = GetPakDecompCount(); LONG64 pakOut0 = GetPakDecompOutBytes();
    LONG read0 = s_readCalls; LONG64 readBytes0 = s_readBytes;
    QueryPerformanceCounter(&t0);

    Real_UnloadCaches(pThis);

    QueryPerformanceCounter(&t1);
    SnapshotPhase(s_statUnloadCaches, t0, t1, pak0, pakOut0, read0, readBytes0);
    Log("G3O", "[LoadProfile] gCWorld::UnloadCaches took %.3f s", s_statUnloadCaches.durationSec);
}

// 5. LoadGameWorld hook
typedef bool (__thiscall* LoadGameWorld_t)(void* pThis, const void* pSaveName, bool isNewGame);
static LoadGameWorld_t Real_LoadGameWorld = nullptr;

static bool __fastcall Hooked_LoadGameWorld(void* pThis, void* /*edx*/, const void* pSaveName, bool isNewGame) {
    QueryPerformanceCounter(&s_tWorldStart);
    if (s_tHUDStart.QuadPart > 0) {
        s_preWorldTotalSec = (double)(s_tWorldStart.QuadPart - s_tHUDStart.QuadPart) / (double)s_freq.QuadPart;
    }
    Log("G3O", "[LoadProfile] -> gCWorld::LoadGameWorld START (pre-load total: %.3f s)", s_preWorldTotalSec);

    LARGE_INTEGER t0, t1;
    LONG pak0 = GetPakDecompCount(); LONG64 pakOut0 = GetPakDecompOutBytes();
    LONG read0 = s_readCalls; LONG64 readBytes0 = s_readBytes;
    QueryPerformanceCounter(&t0);

    bool r = Real_LoadGameWorld(pThis, pSaveName, isNewGame);

    QueryPerformanceCounter(&t1);
    SnapshotPhase(s_statLoadGameWorld, t0, t1, pak0, pakOut0, read0, readBytes0);
    Log("G3O", "[LoadProfile] <- gCWorld::LoadGameWorld FINISHED in %.3f s (result=%d, pak blocks=%ld, read=%.2f MB)",
        s_statLoadGameWorld.durationSec, (int)r, s_statLoadGameWorld.pakBlocks, s_statLoadGameWorld.readMb);
    return r;
}

// 6. GotoLoadedPosition hook
typedef void (__thiscall* GotoLoadedPosition_t)(void* pThis, void* pEntity, bool a);
static GotoLoadedPosition_t Real_GotoLoadedPosition = nullptr;

static void __fastcall Hooked_GotoLoadedPosition(void* pThis, void* /*edx*/, void* pEntity, bool a) {
    if (!s_isLoadActive) { Real_GotoLoadedPosition(pThis, pEntity, a); return; }
    LARGE_INTEGER t0, t1;
    LONG pak0 = GetPakDecompCount(); LONG64 pakOut0 = GetPakDecompOutBytes();
    LONG read0 = s_readCalls; LONG64 readBytes0 = s_readBytes;
    QueryPerformanceCounter(&t0);
    Log("G3O", "[LoadProfile] -> gCSession::GotoLoadedPosition START (Resource Streaming / Scene Init)");

    NotifyGotoLoadedPositionStart(s_lastSaveName.c_str());
    Real_GotoLoadedPosition(pThis, pEntity, a);
    NotifyGotoLoadedPositionEnd();

    QueryPerformanceCounter(&t1);
    SnapshotPhase(s_statGotoPos, t0, t1, pak0, pakOut0, read0, readBytes0);
    Log("G3O", "[LoadProfile] <- gCSession::GotoLoadedPosition FINISHED in %.3f s (pak blocks=%ld, decomp=%.2f MB, read=%.2f MB)",
        s_statGotoPos.durationSec, s_statGotoPos.pakBlocks, s_statGotoPos.pakOutMb, s_statGotoPos.readMb);
}

// 7. SessionStart hook
typedef void (__thiscall* SessionStart_t)(void* pThis, int mode);
static SessionStart_t Real_SessionStart = nullptr;

static void __fastcall Hooked_SessionStart(void* pThis, void* /*edx*/, int mode) {
    if (!s_isLoadActive) { Real_SessionStart(pThis, mode); return; }
    LARGE_INTEGER t0, t1;
    LONG pak0 = GetPakDecompCount(); LONG64 pakOut0 = GetPakDecompOutBytes();
    LONG read0 = s_readCalls; LONG64 readBytes0 = s_readBytes;
    QueryPerformanceCounter(&t0);
    Log("G3O", "[LoadProfile] -> gCSession::Start (mode=%d) START", mode);

    Real_SessionStart(pThis, mode);

    QueryPerformanceCounter(&t1);
    SnapshotPhase(s_statSessionStart, t0, t1, pak0, pakOut0, read0, readBytes0);
    Log("G3O", "[LoadProfile] <- gCSession::Start FINISHED in %.3f s (pak blocks=%ld, read=%.2f MB)",
        s_statSessionStart.durationSec, s_statSessionStart.pakBlocks, s_statSessionStart.readMb);
}

// 8. WorldActivate hook
typedef int (__thiscall* WorldActivate_t)(void* pThis);
static WorldActivate_t Real_WorldActivate = nullptr;

static int __fastcall Hooked_WorldActivate(void* pThis, void* /*edx*/) {
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);

    int r = Real_WorldActivate(pThis);

    QueryPerformanceCounter(&t1);
    s_statWorldActivate.durationSec = (double)(t1.QuadPart - t0.QuadPart) / (double)s_freq.QuadPart;
    return r;
}

// 9. BuildStaticHierachy hook
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
static void* s_targetHeader = nullptr;
static void* s_targetSessionStop = nullptr;
static void* s_targetClearEntities = nullptr;
static void* s_targetUnloadCaches = nullptr;
static void* s_targetLoadGameWorld = nullptr;
static void* s_targetGotoPos = nullptr;
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

        void* pHeader = (void*)GetProcAddress(hGame, "?GetSaveGameHeader@gCWorld@@QAE?AW4bEResult@@ABVbCString@@AAVgCSaveGameHeader@1@@Z");
        if (pHeader) {
            s_targetHeader = pHeader;
            MH_CreateHook(pHeader, (LPVOID)Hooked_GetSaveGameHeader, (LPVOID*)&Real_GetSaveGameHeader);
            MH_EnableHook(pHeader);
        }

        void* pStop = (void*)GetProcAddress(hGame, "?Stop@gCSession@@QAEXXZ");
        if (pStop) {
            s_targetSessionStop = pStop;
            MH_CreateHook(pStop, (LPVOID)Hooked_SessionStop, (LPVOID*)&Real_SessionStop);
            MH_EnableHook(pStop);
        }

        void* pClear = (void*)GetProcAddress(hGame, "?ClearAllEntities@gCGUIManager@@QAE_NXZ");
        if (pClear) {
            s_targetClearEntities = pClear;
            MH_CreateHook(pClear, (LPVOID)Hooked_ClearAllEntities, (LPVOID*)&Real_ClearAllEntities);
            MH_EnableHook(pClear);
        }

        void* pUnload = (void*)GetProcAddress(hGame, "?UnloadCaches@gCWorld@@QAEXXZ");
        if (pUnload) {
            s_targetUnloadCaches = pUnload;
            MH_CreateHook(pUnload, (LPVOID)Hooked_UnloadCaches, (LPVOID*)&Real_UnloadCaches);
            MH_EnableHook(pUnload);
        }

        void* pWorld = (void*)GetProcAddress(hGame, "?LoadGameWorld@gCWorld@@QAE_NABVbCString@@_N@Z");
        if (pWorld) {
            s_targetLoadGameWorld = pWorld;
            MH_CreateHook(pWorld, (LPVOID)Hooked_LoadGameWorld, (LPVOID*)&Real_LoadGameWorld);
            MH_EnableHook(pWorld);
            Log("G3O", "Savegame Profiler: Hooked gCWorld::LoadGameWorld (0x%p)", pWorld);
        }

        void* pGoto = (void*)GetProcAddress(hGame, "?GotoLoadedPosition@gCSession@@QAEXPAVeCEntity@@_N@Z");
        if (pGoto) {
            s_targetGotoPos = pGoto;
            MH_CreateHook(pGoto, (LPVOID)Hooked_GotoLoadedPosition, (LPVOID*)&Real_GotoLoadedPosition);
            MH_EnableHook(pGoto);
            Log("G3O", "Savegame Profiler: Hooked gCSession::GotoLoadedPosition (0x%p)", pGoto);
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
        }
    }

    HMODULE hEngine = GetModuleHandleA("Engine.dll");
    if (hEngine) {
        void* pBuild = (void*)GetProcAddress(hEngine, "?BuildStaticHierachy@eCPVSCellItem@@IAEXXZ");
        if (pBuild) {
            s_targetBuildStatic = pBuild;
            MH_CreateHook(pBuild, (LPVOID)Hooked_BuildStaticHierachy, (LPVOID*)&Real_BuildStaticHierachy);
            MH_EnableHook(pBuild);
        }
    }

    Log("G3O", "Savegame Profiler initialized with 10 deep phase inspection hooks.");
    return true;
}

void ShutdownSavegameProfiler() {
    if (s_targetReadFile) MH_DisableHook(s_targetReadFile);
    if (s_targetHUDLoadGame) MH_DisableHook(s_targetHUDLoadGame);
    if (s_targetHeader) MH_DisableHook(s_targetHeader);
    if (s_targetSessionStop) MH_DisableHook(s_targetSessionStop);
    if (s_targetClearEntities) MH_DisableHook(s_targetClearEntities);
    if (s_targetUnloadCaches) MH_DisableHook(s_targetUnloadCaches);
    if (s_targetLoadGameWorld) MH_DisableHook(s_targetLoadGameWorld);
    if (s_targetGotoPos) MH_DisableHook(s_targetGotoPos);
    if (s_targetSessionStart) MH_DisableHook(s_targetSessionStart);
    if (s_targetWorldActivate) MH_DisableHook(s_targetWorldActivate);
    if (s_targetBuildStatic) MH_DisableHook(s_targetBuildStatic);
}

} // namespace G3O
