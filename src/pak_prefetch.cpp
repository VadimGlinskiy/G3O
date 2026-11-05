#include "pak_prefetch.h"
#include "config_manager.h"
#include "diagnostic_logger.h"
#include <windows.h>
#include <string>
#include <vector>

namespace G3O {

static DWORD WINAPI PrefetchThread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    char exe[MAX_PATH] = { 0 };
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir(exe);
    size_t slash = dir.find_last_of("\\/");
    if (slash != std::string::npos) dir.resize(slash + 1);
    std::string pattern = dir + "Data\\*.pak";

    std::vector<std::string> files;
    WIN32_FIND_DATAA fd;
    HANDLE hf = FindFirstFileA(pattern.c_str(), &fd);
    if (hf != INVALID_HANDLE_VALUE) {
        do { files.push_back(dir + "Data\\" + fd.cFileName); } while (FindNextFileA(hf, &fd));
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
        while (ReadFile(h, buf, kBuf, &got, nullptr) && got > 0) {
            total += got;
            Sleep(0); // yield to the game's own loading
        }
        CloseHandle(h);
    }
    VirtualFree(buf, 0, MEM_RELEASE);
    QueryPerformanceCounter(&t1);
    Log("G3O", "[PakPrefetch] Warmed %u pak files, %.2f GB in %.2f s",
        (unsigned)files.size(), total / 1073741824.0, (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart);
    return 0;
}

void InitializePakPrefetch() {
    if (!GetConfig().enablePakPrefetch) return;
    HANDLE h = CreateThread(nullptr, 0, PrefetchThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
}

} // namespace G3O
