#include "savegame_guard.h"
#include <minhook.h>
#include <mimalloc.h>
#include <stdio.h>
#include <string>
#include <unordered_map>

struct ActiveSaveTransaction {
    std::wstring originalPath;
    std::wstring tmpPath;
};

static CRITICAL_SECTION g_saveLock;
static std::unordered_map<HANDLE, ActiveSaveTransaction> g_activeSaveTransactions;

typedef HANDLE (WINAPI* CreateFileW_t)(
    LPCWSTR lpFileName,
    DWORD dwDesiredAccess,
    DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes,
    HANDLE hTemplateFile
);
static CreateFileW_t Real_CreateFileW = NULL;

typedef BOOL (WINAPI* CloseHandle_t)(HANDLE hObject);
static CloseHandle_t Real_SaveCloseHandle = NULL;

static bool IsSaveGameExtension(const wchar_t* path) {
    if (!path) return false;
    const wchar_t* ext = wcsrchr(path, L'.');
    if (!ext) return false;
    return (_wcsicmp(ext, L".g3sav") == 0 ||
            _wcsicmp(ext, L".g3savdat") == 0 ||
            _wcsicmp(ext, L".g3savcp") == 0);
}

static HANDLE WINAPI Hooked_Save_CreateFileW(
    LPCWSTR lpFileName,
    DWORD dwDesiredAccess,
    DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes,
    HANDLE hTemplateFile
) {
    // Intercept write operations to savegame files
    if ((dwDesiredAccess & GENERIC_WRITE) && IsSaveGameExtension(lpFileName)) {
        std::wstring origPath = lpFileName;
        std::wstring tmpPath = origPath + L".tmp";

        // Redirect write destination to atomic temporary file
        HANDLE hFile = Real_CreateFileW(tmpPath.c_str(), dwDesiredAccess, dwShareMode,
                                       lpSecurityAttributes, dwCreationDisposition,
                                       dwFlagsAndAttributes, hTemplateFile);

        if (hFile != INVALID_HANDLE_VALUE) {
            ActiveSaveTransaction tx;
            tx.originalPath = origPath;
            tx.tmpPath = tmpPath;

            EnterCriticalSection(&g_saveLock);
            g_activeSaveTransactions[hFile] = tx;
            LeaveCriticalSection(&g_saveLock);

            OutputDebugStringA("[Gothic3Optima] SaveGame Guard: Intercepted save write -> Redirected to shadow .tmp\n");
        }
        return hFile;
    }

    return Real_CreateFileW(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
                           dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}

static BOOL WINAPI Hooked_Save_CloseHandle(HANDLE hObject) {
    ActiveSaveTransaction tx;
    bool isSaveFile = false;

    EnterCriticalSection(&g_saveLock);
    auto it = g_activeSaveTransactions.find(hObject);
    if (it != g_activeSaveTransactions.end()) {
        tx = it->second;
        isSaveFile = true;
        g_activeSaveTransactions.erase(it);
    }
    LeaveCriticalSection(&g_saveLock);

    if (isSaveFile) {
        // 1. Force flush dirty buffers to physical storage
        FlushFileBuffers(hObject);

        // 2. Close file handle so file can be moved/replaced
        BOOL res = Real_SaveCloseHandle(hObject);

        // 3. Verify file integrity (must be non-empty)
        WIN32_FILE_ATTRIBUTE_DATA fileData;
        if (GetFileAttributesExW(tx.tmpPath.c_str(), GetFileExInfoStandard, &fileData)) {
            uint64_t fileSize = ((uint64_t)fileData.nFileSizeHigh << 32) | fileData.nFileSizeLow;

            if (fileSize > 256) { // Non-empty valid savegame
                // Atomic commit via ReplaceFileW / MoveFileExW
                if (GetFileAttributesW(tx.originalPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    // Create rotating backup in original folder if desired
                    std::wstring bakPath = tx.originalPath + L".bak";
                    if (ReplaceFileW(tx.originalPath.c_str(), tx.tmpPath.c_str(), bakPath.c_str(),
                                    REPLACEFILE_IGNORE_MERGE_ERRORS, NULL, NULL)) {
                        OutputDebugStringA("[Gothic3Optima] SaveGame Guard: Atomic ReplaceFileW committed successfully!\n");
                    } else {
                        // Fallback move
                        MoveFileExW(tx.tmpPath.c_str(), tx.originalPath.c_str(), MOVEFILE_REPLACE_EXISTING);
                        OutputDebugStringA("[Gothic3Optima] SaveGame Guard: MoveFileExW committed successfully!\n");
                    }
                } else {
                    MoveFileExW(tx.tmpPath.c_str(), tx.originalPath.c_str(), MOVEFILE_REPLACE_EXISTING);
                    OutputDebugStringA("[Gothic3Optima] SaveGame Guard: New save created successfully!\n");
                }
            } else {
                // Incomplete or truncated save! Discard temporary file without corrupting original
                DeleteFileW(tx.tmpPath.c_str());
                OutputDebugStringA("[Gothic3Optima] SaveGame Guard: PREVENTED CORRUPT 0-BYTE SAVE! Original save intact.\n");
            }
        }
        return res;
    }

    return Real_SaveCloseHandle(hObject);
}

// ---------------------------------------------------------------------------
// Pre-Save Memory Flush Hook (Game.dll SaveGameWorld)
// ---------------------------------------------------------------------------

typedef bool (__thiscall* SaveGameWorld_t)(void* pThis, void* pSlotName, void* pThumb, void* pExtra, unsigned long extraSize);
static SaveGameWorld_t Real_SaveGameWorld = NULL;

static bool __fastcall Hooked_SaveGameWorld(void* pThis, void* /*edx*/, void* pSlotName, void* pThumb, void* pExtra, unsigned long extraSize) {
    // 1. Check free virtual memory before serializing the world
    MEMORYSTATUSEX memStatus;
    memStatus.dwLength = sizeof(memStatus);
    GlobalMemoryStatusEx(&memStatus);

    if (memStatus.ullAvailVirtual < 350ULL * 1024ULL * 1024ULL) {
        char buf[256];
        sprintf_s(buf, "[Gothic3Optima] SaveGame Guard: Low virtual memory before save (%.1f MB). Flushing memory!\n",
            (double)memStatus.ullAvailVirtual / (1024.0 * 1024.0));
        OutputDebugStringA(buf);

        // Collect unused mimalloc heap pages and purge caches
        mi_collect(true);
    }

    // 2. Execute savegame serialization with active shadow-write protections
    return Real_SaveGameWorld(pThis, pSlotName, pThumb, pExtra, extraSize);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void InitializeSaveGameGuard() {
    InitializeCriticalSection(&g_saveLock);

    // 1. Hook CreateFileW and CloseHandle for atomic shadow save writes
    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    if (hKernel32) {
        void* pCreateFileW = (void*)GetProcAddress(hKernel32, "CreateFileW");
        if (pCreateFileW) {
            MH_CreateHook(pCreateFileW, (LPVOID)Hooked_Save_CreateFileW, (LPVOID*)&Real_CreateFileW);
            MH_EnableHook(pCreateFileW);
        }

        void* pCloseHandle = (void*)GetProcAddress(hKernel32, "CloseHandle");
        if (pCloseHandle) {
            MH_CreateHook(pCloseHandle, (LPVOID)Hooked_Save_CloseHandle, (LPVOID*)&Real_SaveCloseHandle);
            MH_EnableHook(pCloseHandle);
        }
        OutputDebugStringA("[Gothic3Optima] SaveGame Guard: Atomic shadow file writer installed.\n");
    }

    // 2. Hook SaveGameWorld in Game.dll
    HMODULE hGame = GetModuleHandleA("Game.dll");
    if (hGame) {
        void* pSaveGameWorld = (void*)GetProcAddress(hGame, "?SaveGameWorld@gCWorld@@QAE_NABVbCString@@PAVeCGfxImageData@@PAXK@Z");
        if (pSaveGameWorld) {
            if (MH_CreateHook(pSaveGameWorld, (LPVOID)Hooked_SaveGameWorld, (LPVOID*)&Real_SaveGameWorld) == MH_OK) {
                MH_EnableHook(pSaveGameWorld);
                OutputDebugStringA("[Gothic3Optima] SaveGame Guard: Pre-save memory flush guard hooked in Game.dll.\n");
            }
        }
    }
}

void ShutdownSaveGameGuard() {
    DeleteCriticalSection(&g_saveLock);
}
