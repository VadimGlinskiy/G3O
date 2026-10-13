#include "savegame_guard.h"
#include <minhook.h>
#include <mimalloc.h>
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <string>

// ---------------------------------------------------------------------------
// SaveGame Guard: Pre-save Memory Recovery & Post-save Integrity Validation
// Intercepts SaveGameWorld directly without invasive kernel32 hooks
// ---------------------------------------------------------------------------

typedef bool (__thiscall* SaveGameWorld_t)(void* pThis, void* pSlotName, void* pThumb, void* pExtra, unsigned long extraSize);
static SaveGameWorld_t Real_SaveGameWorld = NULL;

static std::wstring GetGothic3SaveDirectory() {
    wchar_t docPath[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_MYDOCUMENTS, NULL, 0, docPath))) {
        std::wstring saveDir = docPath;
        saveDir += L"\\gothic3";
        return saveDir;
    }
    return L"";
}

static void BackupExistingSaves(const std::wstring& saveDir) {
    if (saveDir.empty()) return;
    std::wstring searchPattern = saveDir + L"\\*.g3savdat";
    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileW(searchPattern.c_str(), &findData);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                std::wstring src = saveDir + L"\\" + findData.cFileName;
                std::wstring dst = src + L".bak";
                CopyFileW(src.c_str(), dst.c_str(), FALSE);
            }
        } while (FindNextFileW(hFind, &findData));
        FindClose(hFind);
    }
}

static void ValidateAndRecoverSaves(const std::wstring& saveDir) {
    if (saveDir.empty()) return;
    std::wstring searchPattern = saveDir + L"\\*.g3savdat";
    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileW(searchPattern.c_str(), &findData);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                uint64_t fileSize = ((uint64_t)findData.nFileSizeHigh << 32) | findData.nFileSizeLow;
                std::wstring filePath = saveDir + L"\\" + findData.cFileName;
                std::wstring bakPath = filePath + L".bak";

                if (fileSize < 256) {
                    // Corrupted 0-byte or truncated save detected! Restore from .bak
                    if (GetFileAttributesW(bakPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                        CopyFileW(bakPath.c_str(), filePath.c_str(), FALSE);
                        OutputDebugStringA("[Gothic3Optima] SaveGame Guard: 0-byte save detected and automatically restored from .bak!\n");
                    }
                }
            }
        } while (FindNextFileW(hFind, &findData));
        FindClose(hFind);
    }
}

static bool __fastcall Hooked_SaveGameWorld(void* pThis, void* /*edx*/, void* pSlotName, void* pThumb, void* pExtra, unsigned long extraSize) {
    // 1. Memory recovery before savegame serialization
    MEMORYSTATUSEX memStatus;
    memStatus.dwLength = sizeof(memStatus);
    GlobalMemoryStatusEx(&memStatus);

    if (memStatus.ullAvailVirtual < 500ULL * 1024ULL * 1024ULL) {
        char buf[256];
        sprintf_s(buf, "[Gothic3Optima] SaveGame Guard: Low virtual memory before save (%.1f MB). Flushing heap caches!\n",
            (double)memStatus.ullAvailVirtual / (1024.0 * 1024.0));
        OutputDebugStringA(buf);

        mi_collect(true);
    }

    // 2. Pre-save safety backup of existing savegames
    std::wstring saveDir = GetGothic3SaveDirectory();
    BackupExistingSaves(saveDir);

    // 3. Execute original savegame routine
    bool result = Real_SaveGameWorld(pThis, pSlotName, pThumb, pExtra, extraSize);

    // 4. Post-save integrity validation
    ValidateAndRecoverSaves(saveDir);

    return result;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void InitializeSaveGameGuard() {
    HMODULE hGame = GetModuleHandleA("Game.dll");
    if (hGame) {
        void* pSaveGameWorld = (void*)GetProcAddress(hGame, "?SaveGameWorld@gCWorld@@QAE_NABVbCString@@PAVeCGfxImageData@@PAXK@Z");
        if (pSaveGameWorld) {
            if (MH_CreateHook(pSaveGameWorld, (LPVOID)Hooked_SaveGameWorld, (LPVOID*)&Real_SaveGameWorld) == MH_OK) {
                MH_EnableHook(pSaveGameWorld);
                OutputDebugStringA("[Gothic3Optima] SaveGame Guard: Safe SaveGameWorld hook active in Game.dll.\n");
            }
        }
    }
}

void ShutdownSaveGameGuard() {
    // Handled by MinHook
}
