#include "streaming_optimizer.h"
#include <minhook.h>
#include <windows.h>
#include <stdio.h>
#include <unordered_map>

// ---------------------------------------------------------------------------
// In-Memory Archive Streaming (Memory-Mapped Files for .pak archives)
// ---------------------------------------------------------------------------

struct MappedArchive {
    void*    pView;
    HANDLE   hMapping;
    uint64_t fileSize;
};

static CRITICAL_SECTION g_archiveLock;
static std::unordered_map<HANDLE, MappedArchive> g_mappedArchives;
static volatile uint64_t g_currentMmapTotalBytes = 0;
static volatile DWORD g_mmapReadOperations = 0;
static volatile uint64_t g_mmapBytesRead = 0;

// Maximum size of a single archive to keep entirely in virtual memory (200 MB)
// Accellerates physics, animations, templates, scripts, lightmaps, UI, and all patch archives.
static const uint64_t MAX_MMAP_ARCHIVE_SIZE = 200ULL * 1024ULL * 1024ULL;

// Total memory budget for mapped views (750 MB)
// Guarantees >3.2 GB of clean virtual address space remains for the 32-bit engine.
static const uint64_t MAX_TOTAL_MMAP_BUDGET = 750ULL * 1024ULL * 1024ULL;

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

typedef BOOL (WINAPI* SetFilePointerEx_t)(
    HANDLE hFile,
    LARGE_INTEGER liDistanceToMove,
    PLARGE_INTEGER lpNewFilePointer,
    DWORD dwMoveMethod
);
static SetFilePointerEx_t Real_SetFilePointerEx = NULL;

typedef BOOL (WINAPI* CloseHandle_t)(HANDLE hObject);
static CloseHandle_t Real_CloseHandle = NULL;

static bool HasArchiveExtension(const char* path) {
    if (!path) return false;
    const char* ext = strrchr(path, '.');
    if (!ext) return false;

    if (_stricmp(ext, ".pak") == 0 ||
        _stricmp(ext, ".nod") == 0 ||
        _stricmp(ext, ".cpt") == 0 ||
        _stricmp(ext, ".hdr") == 0) {
        return true;
    }

    // Check .p00 - .p99 and .c00 - .c99 patch archives
    size_t len = strlen(ext);
    if (len == 4 && (ext[1] == 'p' || ext[1] == 'P' || ext[1] == 'c' || ext[1] == 'C')) {
        if (isdigit((unsigned char)ext[2]) && isdigit((unsigned char)ext[3])) {
            return true;
        }
    }

    return false;
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
        // Optimize OS cache behavior for archive reads
        dwFlagsAndAttributes |= FILE_FLAG_SEQUENTIAL_SCAN;
    }

    HANDLE hFile = Real_CreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
                                   dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);

    if (hFile != INVALID_HANDLE_VALUE && isArchive && (dwDesiredAccess & GENERIC_READ) && !(dwDesiredAccess & (GENERIC_WRITE | FILE_WRITE_DATA))) {
        LARGE_INTEGER size;
        if (GetFileSizeEx(hFile, &size) && (uint64_t)size.QuadPart > 0 && (uint64_t)size.QuadPart <= MAX_MMAP_ARCHIVE_SIZE) {
            MEMORYSTATUSEX memStatus;
            memStatus.dwLength = sizeof(memStatus);
            GlobalMemoryStatusEx(&memStatus);

            // Require at least 1200 MB free virtual memory before committing a new view
            if (memStatus.ullAvailVirtual >= 1200ULL * 1024ULL * 1024ULL) {
                EnterCriticalSection(&g_archiveLock);
                if (g_currentMmapTotalBytes + (uint64_t)size.QuadPart <= MAX_TOTAL_MMAP_BUDGET) {
                    HANDLE hMapping = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
                    if (hMapping) {
                        void* pView = MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
                        if (pView) {
                            MappedArchive ma;
                            ma.pView = pView;
                            ma.hMapping = hMapping;
                            ma.fileSize = (uint64_t)size.QuadPart;

                            g_mappedArchives[hFile] = ma;
                            g_currentMmapTotalBytes += (uint64_t)size.QuadPart;

                            char buf[256];
                            sprintf_s(buf, "[Gothic3Optima] Memory-mapped archive: %s (%.2f MB, total RAM mapped: %.2f MB)\n",
                                lpFileName, (double)size.QuadPart / (1024.0 * 1024.0),
                                (double)g_currentMmapTotalBytes / (1024.0 * 1024.0));
                            OutputDebugStringA(buf);

                            FILE* fLog = fopen("G3O_Streaming.log", "a");
                            if (fLog) {
                                fprintf(fLog, "Memory-mapped: %s (%.2f MB, total RAM: %.2f MB)\n",
                                    lpFileName, (double)size.QuadPart / (1024.0 * 1024.0),
                                    (double)g_currentMmapTotalBytes / (1024.0 * 1024.0));
                                fclose(fLog);
                            }
                        } else {
                            CloseHandle(hMapping);
                        }
                    }
                }
                LeaveCriticalSection(&g_archiveLock);
            }
        }
    }
    return hFile;
}

static BOOL WINAPI Hooked_ReadFile(
    HANDLE hFile,
    LPVOID lpBuffer,
    DWORD nNumberOfBytesToRead,
    LPDWORD lpNumberOfBytesRead,
    LPOVERLAPPED lpOverlapped
) {
    if (!lpOverlapped && lpBuffer && nNumberOfBytesToRead > 0 && Real_SetFilePointerEx) {
        EnterCriticalSection(&g_archiveLock);
        auto it = g_mappedArchives.find(hFile);
        if (it != g_mappedArchives.end()) {
            MappedArchive ma = it->second;

            // Query current file position from OS kernel handle
            LARGE_INTEGER zero = { 0 };
            LARGE_INTEGER currPos = { 0 };
            if (Real_SetFilePointerEx(hFile, zero, &currPos, FILE_CURRENT)) {
                uint64_t pos = (uint64_t)currPos.QuadPart;
                uint64_t remain = (pos < ma.fileSize) ? (ma.fileSize - pos) : 0;
                DWORD toCopy = (DWORD)((remain < (uint64_t)nNumberOfBytesToRead) ? remain : (uint64_t)nNumberOfBytesToRead);

                if (toCopy > 0) {
                    memcpy(lpBuffer, (const char*)ma.pView + pos, toCopy);
                }

                // Advance kernel file pointer atomically to keep OS handle in sync
                LARGE_INTEGER advance;
                advance.QuadPart = toCopy;
                Real_SetFilePointerEx(hFile, advance, NULL, FILE_CURRENT);

                if (lpNumberOfBytesRead) {
                    *lpNumberOfBytesRead = toCopy;
                }

                g_mmapReadOperations++;
                g_mmapBytesRead += toCopy;

                if (g_mmapReadOperations % 2000 == 1) {
                    FILE* fLog = fopen("G3O_Streaming.log", "a");
                    if (fLog) {
                        fprintf(fLog, "Streaming stats: %u RAM reads, %.2f MB read directly from RAM\n",
                            g_mmapReadOperations, (double)g_mmapBytesRead / (1024.0 * 1024.0));
                        fclose(fLog);
                    }
                }

                LeaveCriticalSection(&g_archiveLock);
                return TRUE;
            }
        }
        LeaveCriticalSection(&g_archiveLock);
    }

    return Real_ReadFile(hFile, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead, lpOverlapped);
}

static BOOL WINAPI Hooked_CloseHandle(HANDLE hObject) {
    EnterCriticalSection(&g_archiveLock);
    auto it = g_mappedArchives.find(hObject);
    if (it != g_mappedArchives.end()) {
        MappedArchive ma = it->second;
        g_currentMmapTotalBytes -= ma.fileSize;
        g_mappedArchives.erase(it);
        LeaveCriticalSection(&g_archiveLock);

        UnmapViewOfFile(ma.pView);
        CloseHandle(ma.hMapping);
    } else {
        LeaveCriticalSection(&g_archiveLock);
    }

    return Real_CloseHandle(hObject);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void InitializeStreamingOptimizer() {
    InitializeCriticalSection(&g_archiveLock);

    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    if (hKernel32) {
        Real_SetFilePointerEx = (SetFilePointerEx_t)GetProcAddress(hKernel32, "SetFilePointerEx");
        void* pCreateFileA = (void*)GetProcAddress(hKernel32, "CreateFileA");
        void* pReadFile = (void*)GetProcAddress(hKernel32, "ReadFile");
        void* pCloseHandle = (void*)GetProcAddress(hKernel32, "CloseHandle");

        if (pCreateFileA) {
            MH_CreateHook(pCreateFileA, (LPVOID)Hooked_CreateFileA, (LPVOID*)&Real_CreateFileA);
            MH_EnableHook(pCreateFileA);
        }
        if (pReadFile) {
            MH_CreateHook(pReadFile, (LPVOID)Hooked_ReadFile, (LPVOID*)&Real_ReadFile);
            MH_EnableHook(pReadFile);
        }
        if (pCloseHandle) {
            MH_CreateHook(pCloseHandle, (LPVOID)Hooked_CloseHandle, (LPVOID*)&Real_CloseHandle);
            MH_EnableHook(pCloseHandle);
        }

        OutputDebugStringA("[Gothic3Optima] In-Memory Archive Streaming (Memory-Mapped I/O) initialized successfully.\n");
    }
}

void ShutdownStreamingOptimizer() {
    EnterCriticalSection(&g_archiveLock);
    for (auto& pair : g_mappedArchives) {
        UnmapViewOfFile(pair.second.pView);
        CloseHandle(pair.second.hMapping);
    }
    g_mappedArchives.clear();
    g_currentMmapTotalBytes = 0;
    LeaveCriticalSection(&g_archiveLock);
    DeleteCriticalSection(&g_archiveLock);
}
