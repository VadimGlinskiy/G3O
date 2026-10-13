#include "streaming_optimizer.h"
#include <minhook.h>
#include <stdio.h>
#include <unordered_map>

// ---------------------------------------------------------------------------
// 1. In-Memory Archive Streaming (Memory-Mapped Files for .pak archives)
// ---------------------------------------------------------------------------

struct MappedArchive {
    void*    pView;
    HANDLE   hMapping;
    uint64_t fileSize;
    uint64_t currentPos;
};

static CRITICAL_SECTION g_archiveLock;
static std::unordered_map<HANDLE, MappedArchive> g_mappedArchives;
static volatile DWORD g_mmapReadOperations = 0;
static volatile uint64_t g_mmapBytesRead = 0;

// Maximum size of a single archive to keep entirely in virtual memory (350 MB)
// Ensures we don't exhaust the 32-bit address space while accelerating 90%+ of archives.
static const uint64_t MAX_MMAP_ARCHIVE_SIZE = 350ULL * 1024ULL * 1024ULL;

typedef HANDLE (WINAPI* CreateFileA_t)(
    LPCSTR lpFileName,
    DWORD dwDesiredAccess,
    DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes,
    HANDLE hTemplateFile
);
static CreateFileA_t Real_CreateFileA = NULL;

typedef BOOL (WINAPI* ReadFile_t)(
    HANDLE hFile,
    LPVOID lpBuffer,
    DWORD nNumberOfBytesToRead,
    LPDWORD lpNumberOfBytesRead,
    LPOVERLAPPED lpOverlapped
);
static ReadFile_t Real_ReadFile = NULL;

typedef DWORD (WINAPI* SetFilePointer_t)(
    HANDLE hFile,
    LONG lDistanceToMove,
    PLONG lpDistanceToMoveHigh,
    DWORD dwMoveMethod
);
static SetFilePointer_t Real_SetFilePointer = NULL;

typedef BOOL (WINAPI* CloseHandle_t)(HANDLE hObject);
static CloseHandle_t Real_CloseHandle = NULL;

static bool HasArchiveExtension(const char* path) {
    if (!path) return false;
    const char* ext = strrchr(path, '.');
    if (!ext) return false;
    return (_stricmp(ext, ".pak") == 0 ||
            _stricmp(ext, ".nod") == 0 ||
            _stricmp(ext, ".cpt") == 0 ||
            _stricmp(ext, ".hdr") == 0);
}

static HANDLE WINAPI Hooked_CreateFileA(
    LPCSTR lpFileName,
    DWORD dwDesiredAccess,
    DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes,
    HANDLE hTemplateFile
) {
    bool isArchive = HasArchiveExtension(lpFileName);
    if (isArchive) {
        // Optimize OS caching flags for archive streaming
        dwFlagsAndAttributes |= FILE_FLAG_SEQUENTIAL_SCAN;
    }

    HANDLE hFile = Real_CreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
                                   dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);

    if (hFile != INVALID_HANDLE_VALUE && isArchive && (dwDesiredAccess & GENERIC_READ)) {
        LARGE_INTEGER size;
        if (GetFileSizeEx(hFile, &size) && (uint64_t)size.QuadPart > 0 && (uint64_t)size.QuadPart <= MAX_MMAP_ARCHIVE_SIZE) {
            HANDLE hMapping = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
            if (hMapping) {
                void* pView = MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
                if (pView) {
                    MappedArchive ma;
                    ma.pView = pView;
                    ma.hMapping = hMapping;
                    ma.fileSize = (uint64_t)size.QuadPart;
                    ma.currentPos = 0;

                    EnterCriticalSection(&g_archiveLock);
                    g_mappedArchives[hFile] = ma;
                    LeaveCriticalSection(&g_archiveLock);

                    char buf[256];
                    sprintf_s(buf, "[Gothic3Optima] Memory-mapped archive: %s (%.2f MB)\n",
                        lpFileName, (double)size.QuadPart / (1024.0 * 1024.0));
                    OutputDebugStringA(buf);
                } else {
                    CloseHandle(hMapping);
                }
            }
        }
    }
    return hFile;
}

static DWORD WINAPI Hooked_SetFilePointer(
    HANDLE hFile,
    LONG lDistanceToMove,
    PLONG lpDistanceToMoveHigh,
    DWORD dwMoveMethod
) {
    EnterCriticalSection(&g_archiveLock);
    auto it = g_mappedArchives.find(hFile);
    if (it != g_mappedArchives.end()) {
        MappedArchive& ma = it->second;
        int64_t moveDist = (int64_t)lDistanceToMove;
        if (lpDistanceToMoveHigh && *lpDistanceToMoveHigh) {
            moveDist |= ((int64_t)(*lpDistanceToMoveHigh) << 32);
        }

        uint64_t newPos = ma.currentPos;
        switch (dwMoveMethod) {
        case FILE_BEGIN:
            newPos = (moveDist >= 0) ? (uint64_t)moveDist : 0;
            break;
        case FILE_CURRENT:
            newPos = (int64_t)ma.currentPos + moveDist >= 0 ? (uint64_t)((int64_t)ma.currentPos + moveDist) : 0;
            break;
        case FILE_END:
            newPos = (int64_t)ma.fileSize + moveDist >= 0 ? (uint64_t)((int64_t)ma.fileSize + moveDist) : 0;
            break;
        }

        if (newPos > ma.fileSize) newPos = ma.fileSize;
        ma.currentPos = newPos;

        if (lpDistanceToMoveHigh) {
            *lpDistanceToMoveHigh = (LONG)(newPos >> 32);
        }
        LeaveCriticalSection(&g_archiveLock);
        return (DWORD)(newPos & 0xFFFFFFFF);
    }
    LeaveCriticalSection(&g_archiveLock);

    return Real_SetFilePointer(hFile, lDistanceToMove, lpDistanceToMoveHigh, dwMoveMethod);
}

static BOOL WINAPI Hooked_ReadFile(
    HANDLE hFile,
    LPVOID lpBuffer,
    DWORD nNumberOfBytesToRead,
    LPDWORD lpNumberOfBytesRead,
    LPOVERLAPPED lpOverlapped
) {
    // Only intercept synchronous non-overlapped reads for mapped archives
    if (!lpOverlapped) {
        EnterCriticalSection(&g_archiveLock);
        auto it = g_mappedArchives.find(hFile);
        if (it != g_mappedArchives.end()) {
            MappedArchive& ma = it->second;
            uint64_t remain = (ma.currentPos < ma.fileSize) ? (ma.fileSize - ma.currentPos) : 0;
            DWORD toCopy = (DWORD)((remain < (uint64_t)nNumberOfBytesToRead) ? remain : (uint64_t)nNumberOfBytesToRead);

            if (toCopy > 0) {
                memcpy(lpBuffer, (const char*)ma.pView + ma.currentPos, toCopy);
                ma.currentPos += toCopy;
            }

            if (lpNumberOfBytesRead) {
                *lpNumberOfBytesRead = toCopy;
            }

            g_mmapReadOperations++;
            g_mmapBytesRead += toCopy;
            LeaveCriticalSection(&g_archiveLock);
            return TRUE;
        }
        LeaveCriticalSection(&g_archiveLock);
    }

    return Real_ReadFile(hFile, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead, lpOverlapped);
}

static BOOL WINAPI Hooked_CloseHandle(HANDLE hObject) {
    EnterCriticalSection(&g_archiveLock);
    auto it = g_mappedArchives.find(hObject);
    if (it != g_mappedArchives.end()) {
        UnmapViewOfFile(it->second.pView);
        CloseHandle(it->second.hMapping);
        g_mappedArchives.erase(it);
    }
    LeaveCriticalSection(&g_archiveLock);

    return Real_CloseHandle(hObject);
}

// ---------------------------------------------------------------------------
// 2. Thread Pool Expansion (eCThreadPool::Create in Engine.dll)
// ---------------------------------------------------------------------------

typedef bool (__thiscall* eCThreadPool_Create_t)(void* pThis, unsigned long numThreads);
static eCThreadPool_Create_t Real_eCThreadPool_Create = NULL;

static bool __fastcall Hooked_eCThreadPool_Create(void* pThis, void* /*edx*/, unsigned long numThreads) {
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    unsigned long optimalThreads = sysInfo.dwNumberOfProcessors;
    if (optimalThreads < 4) optimalThreads = 4;
    if (optimalThreads > 8) optimalThreads = 8; // Avoid thread contention

    char buf[128];
    sprintf_s(buf, "[Gothic3Optima] eCThreadPool::Create: Scaled threads from %lu to %lu\n", numThreads, optimalThreads);
    OutputDebugStringA(buf);

    return Real_eCThreadPool_Create(pThis, optimalThreads);
}

// ---------------------------------------------------------------------------
// 3. Cache Anti-Thrashing & OOM Guard (eCMainCache::CacheOutResource)
// ---------------------------------------------------------------------------

typedef bool (__thiscall* CacheOutResource_t)(void* pThis, void* pEntity, int behavior);
static CacheOutResource_t Real_CacheOutResource = NULL;
static volatile DWORD g_evictionsThrottled = 0;

static bool __fastcall Hooked_CacheOutResource(void* pThis, void* /*edx*/, void* pEntity, int behavior) {
    MEMORYSTATUSEX memStatus;
    memStatus.dwLength = sizeof(memStatus);
    GlobalMemoryStatusEx(&memStatus);

    // OOM Guard: Virtual memory critically low (< 200 MB free)
    if (memStatus.ullAvailVirtual < 200ULL * 1024ULL * 1024ULL) {
        static DWORD s_lastOOMAlert = 0;
        DWORD now = GetTickCount();
        if (now - s_lastOOMAlert > 5000) {
            char buf[128];
            sprintf_s(buf, "[Gothic3Optima] OOM Guard: Low virtual memory (%.1f MB free)! Forcing cache eviction.\n",
                (double)memStatus.ullAvailVirtual / (1024.0 * 1024.0));
            OutputDebugStringA(buf);
            s_lastOOMAlert = now;
        }
        return Real_CacheOutResource(pThis, pEntity, behavior);
    }

    // Cache Anti-Thrashing: Ample memory available (> 600 MB free virtual memory)
    // Behavior == 0 represents passive/speculative cache eviction during camera rotations
    if (memStatus.ullAvailVirtual >= 600ULL * 1024ULL * 1024ULL && behavior == 0) {
        InterlockedIncrement((volatile LONG*)&g_evictionsThrottled);
        return true; // Keep resource cached in memory
    }

    return Real_CacheOutResource(pThis, pEntity, behavior);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void InitializeStreamingOptimizer() {
    InitializeCriticalSection(&g_archiveLock);

    // 1. File IO hooks disabled - Gothic 3 uses multi-threaded concurrent reads per handle,
    // which caused file pointer race conditions and corrupted string reads.
    /*
    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    if (hKernel32) {
        void* pCreateFileA = (void*)GetProcAddress(hKernel32, "CreateFileA");
        void* pReadFile = (void*)GetProcAddress(hKernel32, "ReadFile");
        void* pSetFilePointer = (void*)GetProcAddress(hKernel32, "SetFilePointer");
        void* pCloseHandle = (void*)GetProcAddress(hKernel32, "CloseHandle");

        if (pCreateFileA) {
            MH_CreateHook(pCreateFileA, (LPVOID)Hooked_CreateFileA, (LPVOID*)&Real_CreateFileA);
            MH_EnableHook(pCreateFileA);
        }
        if (pReadFile) {
            MH_CreateHook(pReadFile, (LPVOID)Hooked_ReadFile, (LPVOID*)&Real_ReadFile);
            MH_EnableHook(pReadFile);
        }
        if (pSetFilePointer) {
            MH_CreateHook(pSetFilePointer, (LPVOID)Hooked_SetFilePointer, (LPVOID*)&Real_SetFilePointer);
            MH_EnableHook(pSetFilePointer);
        }
        if (pCloseHandle) {
            MH_CreateHook(pCloseHandle, (LPVOID)Hooked_CloseHandle, (LPVOID*)&Real_CloseHandle);
            MH_EnableHook(pCloseHandle);
        }
    }
    */

    // 2. Thread pool & CacheOut hooks disabled to preserve engine thread safety during startup
    /*
    HMODULE hEngine = GetModuleHandleA("Engine.dll");
    if (hEngine) {
        void* pThreadPoolCreate = (void*)GetProcAddress(hEngine, "?Create@eCThreadPool@@QAE_NK@Z");
        if (pThreadPoolCreate) {
            if (MH_CreateHook(pThreadPoolCreate, (LPVOID)Hooked_eCThreadPool_Create, (LPVOID*)&Real_eCThreadPool_Create) == MH_OK) {
                MH_EnableHook(pThreadPoolCreate);
                OutputDebugStringA("[Gothic3Optima] Hooked eCThreadPool::Create (4-8 worker threads).\n");
            }
        }

        // 3. Hook eCMainCache::CacheOutResource in Engine.dll
        void* pCacheOut = (void*)GetProcAddress(hEngine, "?CacheOutResource@eCMainCache@@QAE_NAAVeCResourceDataEntity@@W4eEResourceCacheBehavior@@@Z");
        if (pCacheOut) {
            if (MH_CreateHook(pCacheOut, (LPVOID)Hooked_CacheOutResource, (LPVOID*)&Real_CacheOutResource) == MH_OK) {
                MH_EnableHook(pCacheOut);
                OutputDebugStringA("[Gothic3Optima] Hooked eCMainCache::CacheOutResource (Anti-thrashing & OOM Guard).\n");
            }
        }
    }
    */
}

void ShutdownStreamingOptimizer() {
    EnterCriticalSection(&g_archiveLock);
    for (auto& pair : g_mappedArchives) {
        UnmapViewOfFile(pair.second.pView);
        CloseHandle(pair.second.hMapping);
    }
    g_mappedArchives.clear();
    LeaveCriticalSection(&g_archiveLock);
    DeleteCriticalSection(&g_archiveLock);
}
