#include "pak_prefetch.h"
#include "config_manager.h"
#include "diagnostic_logger.h"
#include "pak_read_at.h"
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace G3O {

static bool IsGothic3ArchiveExtension(const std::string& fileName) {
    size_t dot = fileName.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = fileName.substr(dot);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".pak" || ext == ".mod" || ext == ".cpt") return true;
    if (ext.size() == 4 && (ext[1] == 'm' || ext[1] == 'p' || ext[1] == 'c') &&
        std::isdigit(static_cast<unsigned char>(ext[2])) &&
        std::isdigit(static_cast<unsigned char>(ext[3]))) {
        return true;
    }
    return false;
}

static DWORD WINAPI PrefetchThread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    char exe[MAX_PATH] = { 0 };
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir(exe);
    size_t slash = dir.find_last_of("\\/");
    if (slash != std::string::npos) dir.resize(slash + 1);
    std::string pattern = dir + "Data\\*.*";

    std::vector<std::string> files;
    WIN32_FIND_DATAA fd;
    HANDLE hf = FindFirstFileA(pattern.c_str(), &fd);
    if (hf != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                std::string name(fd.cFileName);
                if (IsGothic3ArchiveExtension(name)) {
                    files.push_back(dir + "Data\\" + name);
                }
            }
        } while (FindNextFileA(hf, &fd));
        FindClose(hf);
    }

    const DWORD kBuf = 4 * 1024 * 1024;
    BYTE* buf = (BYTE*)VirtualAlloc(nullptr, kBuf, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) return 0;

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    unsigned long long total = 0;
    for (const std::string& f : files) {
        HANDLE h = CreateFileA(f.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        DWORD got = 0;
        for (;;) {
            BOOL readOk = FALSE;
            {
                ScopedInternalPakRead internalRead;
                readOk = ReadFile(h, buf, kBuf, &got, nullptr);
            }
            if (!readOk || got == 0) break;
            total += got;
            Sleep(0); // yield to the game's own loading
        }
        CloseHandle(h);
    }
    VirtualFree(buf, 0, MEM_RELEASE);
    QueryPerformanceCounter(&t1);
    Log("G3O", "[PakPrefetch] Warmed %u archive files (.pak/.mod/.mXX), %.2f GB in %.2f s",
        (unsigned)files.size(), total / 1073741824.0, (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart);
    return 0;
}

void InitializePakPrefetch() {
    if (!GetConfig().enablePakPrefetch) return;
    HANDLE h = CreateThread(nullptr, 0, PrefetchThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
}

} // namespace G3O
