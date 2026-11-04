#include "profiler.h"
#include "diagnostic_logger.h"
#include "d3d9_hook.h"
#include "config_manager.h"
#include <minhook.h>
#include <d3d9.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <algorithm>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include <stdio.h>

namespace G3O {

volatile LONG g_cullCalls = 0;

static volatile LONG   s_frames = 0;
static volatile LONG   s_slowFrames = 0;
static volatile LONG   s_maxFrameUs = 0;
static volatile LONG   s_readCalls = 0;
static volatile LONG64 s_readBytes = 0;
static volatile LONG64 s_readUs = 0;
static volatile LONG   s_busyTid = 0;

static LARGE_INTEGER s_freq = []() {
    LARGE_INTEGER li;
    QueryPerformanceFrequency(&li);
    if (li.QuadPart <= 0) li.QuadPart = 10000000LL;
    return li;
}();
static LARGE_INTEGER s_lastFrame = {};
static volatile bool s_stop = false;
static FILE* s_file = NULL;

// Sample ring buffer, preallocated: the sampler must never allocate while the
// target thread is suspended.
static const LONG SAMPLE_CAP = 1 << 16;
static DWORD s_samples[SAMPLE_CAP];
static volatile LONG s_sampleCount = 0;

// ---- ReadFile timing --------------------------------------------------------
typedef BOOL (WINAPI* ReadFile_t)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
static ReadFile_t Real_ReadFile = NULL;

static BOOL WINAPI Hooked_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD pRead, LPOVERLAPPED ov) {
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    BOOL r = Real_ReadFile(h, buf, n, pRead, ov);
    QueryPerformanceCounter(&t1);
    InterlockedIncrement(&s_readCalls);
    InterlockedExchangeAdd64(&s_readBytes, (LONG64)((pRead && r) ? *pRead : 0));
    if (s_freq.QuadPart > 0) {
        InterlockedExchangeAdd64(&s_readUs, (t1.QuadPart - t0.QuadPart) * 1000000LL / s_freq.QuadPart);
    }
    return r;
}

// ---- FileSystem.dll!RemoveFromDirCache (suspected load-time hotspot) --------
typedef void (__thiscall* RemoveFromDirCache_t)(void* pThis, const void* name, bool a, bool b);
static RemoveFromDirCache_t Real_RemoveFromDirCache = NULL;
static volatile LONG   s_rfdCalls = 0;
static volatile LONG64 s_rfdUs = 0;
static volatile LONG   s_rfdEntries = 0;
static const int RFD_SAMPLES = 40;
static char s_rfdText[RFD_SAMPLES][112];
static volatile LONG s_rfdTextCount = 0;
static LONG s_rfdTextPrinted = 0;

static void __fastcall Hooked_RemoveFromDirCache(void* pThis, void*, const void* name, bool a, bool b) {
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    Real_RemoveFromDirCache(pThis, name, a, b);
    QueryPerformanceCounter(&t1);
    LONG64 us = (s_freq.QuadPart > 0) ? ((t1.QuadPart - t0.QuadPart) * 1000000LL / s_freq.QuadPart) : 0;
    InterlockedIncrement(&s_rfdCalls);
    InterlockedExchangeAdd64(&s_rfdUs, us);
    LONG entries = *(LONG*)((BYTE*)pThis + 0x38); // element count of the dir cache map
    s_rfdEntries = entries;
    LONG idx = s_rfdTextCount;
    if (idx < RFD_SAMPLES && name) {
        // eastl::basic_string<char> with small-string optimisation
        const BYTE* s = (const BYTE*)name;
        const char* ptr; DWORD len;
        if (s[0x0B] & 0x80) { ptr = *(const char**)s; len = *(const DWORD*)(s + 4); }
        else { ptr = (const char*)s; len = 0x0B - (DWORD)(signed char)s[0x0B]; }
        if (len > 90) len = 90;
        char tmp[100] = {};
        if (ptr && !IsBadReadPtr(ptr, len)) memcpy(tmp, ptr, len);
        snprintf(s_rfdText[idx], sizeof(s_rfdText[idx]), "args(%d,%d) entries=%ld took=%lldus name=%s", (int)a, (int)b, entries, us, tmp);
        s_rfdTextCount = idx + 1;
    }
}
// ---- Frame timing (IDirect3DDevice9::Present) -------------------------------
typedef HRESULT (STDMETHODCALLTYPE* Present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
static Present_t Real_Present = NULL;

static HRESULT STDMETHODCALLTYPE Hooked_Present(IDirect3DDevice9* dev, const RECT* a, const RECT* b, HWND w, const RGNDATA* d) {
    InvalidateD3D9StateCaches();
    if (GetConfig().enableProfiler && s_freq.QuadPart > 0) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (s_lastFrame.QuadPart) {
            LONG us = (LONG)((now.QuadPart - s_lastFrame.QuadPart) * 1000000LL / s_freq.QuadPart);
            if (us > s_maxFrameUs) s_maxFrameUs = us;
            if (us > 33333) InterlockedIncrement(&s_slowFrames);
        }
        s_lastFrame = now;
        InterlockedIncrement(&s_frames);
    }
    return Real_Present(dev, a, b, w, d);
}

void ProfilerHookPresent(void** vtbl) {
    if (!vtbl) return;
    if (MH_CreateHook(vtbl[17], (LPVOID)Hooked_Present, (LPVOID*)&Real_Present) == MH_OK) {
        MH_EnableHook(vtbl[17]);
        Log("G3O", "Profiler: Present hooked for frame timing.");
    }
}

// ---- Sampling profiler of the busiest thread --------------------------------
static DWORD WINAPI SamplerThread(LPVOID) {
    HANDLE hThread = NULL;
    DWORD hTid = 0;
    while (!s_stop) {
        Sleep(1);
        DWORD tid = (DWORD)s_busyTid;
        if (!tid) continue;
        if (tid != hTid) {
            if (hThread) CloseHandle(hThread);
            hThread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
            hTid = tid;
        }
        if (!hThread) continue;
        if (SuspendThread(hThread) == (DWORD)-1) continue;
        CONTEXT ctx = {};
        ctx.ContextFlags = CONTEXT_CONTROL;
        DWORD eip = GetThreadContext(hThread, &ctx) ? ctx.Eip : 0;
        ResumeThread(hThread);
        if (eip) {
            LONG i = s_sampleCount;
            if (i < SAMPLE_CAP) { s_samples[i] = eip; s_sampleCount = i + 1; }
        }
    }
    if (hThread) CloseHandle(hThread);
    return 0;
}

// Export table lookup: nearest named export at or below the RVA.
struct ExportEntry { DWORD rva; std::string name; };
static std::map<HMODULE, std::vector<ExportEntry>> s_exports;

static std::vector<ExportEntry>& GetExports(HMODULE mod) {
    auto it = s_exports.find(mod);
    if (it != s_exports.end()) return it->second;
    std::vector<ExportEntry>& v = s_exports[mod];
    BYTE* base = (BYTE*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dd.VirtualAddress) return v;
    IMAGE_EXPORT_DIRECTORY* ex = (IMAGE_EXPORT_DIRECTORY*)(base + dd.VirtualAddress);
    DWORD* funcs = (DWORD*)(base + ex->AddressOfFunctions);
    DWORD* names = (DWORD*)(base + ex->AddressOfNames);
    WORD* ords = (WORD*)(base + ex->AddressOfNameOrdinals);
    for (DWORD i = 0; i < ex->NumberOfNames; ++i) {
        DWORD rva = funcs[ords[i]];
        if (rva >= dd.VirtualAddress && rva < dd.VirtualAddress + dd.Size) continue; // forwarder
        v.push_back({ rva, (const char*)(base + names[i]) });
    }
    std::sort(v.begin(), v.end(), [](const ExportEntry& a, const ExportEntry& b) { return a.rva < b.rva; });
    return v;
}

static std::string Resolve(DWORD eip, std::string& moduleOut) {
    HMODULE mod = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(ULONG_PTR)eip, &mod) || !mod) {
        moduleOut = "?";
        return "unknown";
    }
    char path[MAX_PATH] = {};
    GetModuleFileNameA(mod, path, MAX_PATH);
    const char* slash = strrchr(path, '\\');
    moduleOut = slash ? slash + 1 : path;
    DWORD rva = eip - (DWORD)(ULONG_PTR)mod;
    std::vector<ExportEntry>& ex = GetExports(mod);
    char buf[512];
    if (ex.empty()) {
        snprintf(buf, sizeof(buf), "%s+0x%X", moduleOut.c_str(), rva);
        return buf;
    }
    auto it = std::upper_bound(ex.begin(), ex.end(), rva, [](DWORD r, const ExportEntry& e) { return r < e.rva; });
    if (it == ex.begin()) {
        snprintf(buf, sizeof(buf), "%s+0x%X", moduleOut.c_str(), rva);
        return buf;
    }
    --it;
    snprintf(buf, sizeof(buf), "%s!%s+0x%X", moduleOut.c_str(), it->name.c_str(), rva - it->rva);
    return buf;
}

static void ReportHotspots() {
    LONG n = s_sampleCount;
    if (n < 50 || !s_file) { s_sampleCount = 0; return; }
    std::unordered_map<std::string, int> byFunc, byModule;
    std::unordered_map<DWORD, std::string> cache;
    for (LONG i = 0; i < n; ++i) {
        DWORD e = s_samples[i];
        std::string mod, fn;
        auto c = cache.find(e >> 4);
        if (c != cache.end()) {
            fn = c->second;
        } else {
            fn = Resolve(e, mod);
            cache[e >> 4] = fn;
        }
        // Strip +offset so samples of the same function aggregate.
        std::string key = fn;
        size_t plus = key.rfind("+0x");
        if (plus != std::string::npos) key.resize(plus);
        byFunc[key]++;
        size_t bang = key.find('!');
        byModule[bang != std::string::npos ? key.substr(0, bang) : key]++;
    }
    s_sampleCount = 0;

    auto dump = [&](const char* title, std::unordered_map<std::string, int>& m, size_t top) {
        std::vector<std::pair<std::string, int>> v(m.begin(), m.end());
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        fprintf(s_file, "HOT %s (%ld samples):\n", title, n);
        for (size_t i = 0; i < v.size() && i < top; ++i)
            fprintf(s_file, "HOT   %5.1f%%  %s\n", 100.0 * v[i].second / n, v[i].first.c_str());
    };
    dump("modules", byModule, 6);
    dump("functions", byFunc, 15);
    fflush(s_file);
}

// ---- Reporter thread --------------------------------------------------------
static ULONGLONG FtToU64(const FILETIME& f) {
    return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime;
}

static DWORD WINAPI ReporterThread(LPVOID) {
    std::unordered_map<DWORD, ULONGLONG> prevThread;
    ULONGLONG prevProc = 0;
    DWORD start = GetTickCount();
    DWORD pid = GetCurrentProcessId();
    SYSTEM_INFO si; GetSystemInfo(&si);
    int tick = 0;

    while (!s_stop) {
        Sleep(1000);

        LONG frames = InterlockedExchange(&s_frames, 0);
        LONG slow = InterlockedExchange(&s_slowFrames, 0);
        LONG maxUs = InterlockedExchange(&s_maxFrameUs, 0);
        LONG rc = InterlockedExchange(&s_readCalls, 0);
        LONG64 rb = InterlockedExchange64(&s_readBytes, 0);
        LONG64 ru = InterlockedExchange64(&s_readUs, 0);
        LONG cull = InterlockedExchange(&g_cullCalls, 0);
        LONG rfdCalls = InterlockedExchange(&s_rfdCalls, 0);
        LONG64 rfdUs = InterlockedExchange64(&s_rfdUs, 0);

        FILETIME c, e, k, u;
        GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
        ULONGLONG proc = FtToU64(k) + FtToU64(u);
        double procPct = prevProc ? (double)(proc - prevProc) / 10000000.0 * 100.0 : 0.0;
        prevProc = proc;

        DWORD busyTid = 0; double busyPct = 0.0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            THREADENTRY32 te; te.dwSize = sizeof(te);
            if (Thread32First(snap, &te)) {
                do {
                    if (te.th32OwnerProcessID != pid) continue;
                    HANDLE th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
                    if (!th) continue;
                    FILETIME tc, te2, tk, tu;
                    if (GetThreadTimes(th, &tc, &te2, &tk, &tu)) {
                        ULONGLONG t = FtToU64(tk) + FtToU64(tu);
                        auto it = prevThread.find(te.th32ThreadID);
                        if (it != prevThread.end()) {
                            double pct = (double)(t - it->second) / 10000000.0 * 100.0;
                            if (pct > busyPct) { busyPct = pct; busyTid = te.th32ThreadID; }
                        }
                        prevThread[te.th32ThreadID] = t;
                    }
                    CloseHandle(th);
                } while (Thread32Next(snap, &te));
            }
            CloseHandle(snap);
        }
        // Sample the busiest thread only when it is clearly the bottleneck.
        s_busyTid = (busyPct > 60.0) ? (LONG)busyTid : 0;

        if (s_file) {
            fprintf(s_file,
                "t=%3lus fps=%3d maxFrame=%6.1fms slow(>33ms)=%2d | read: %5d calls %7.1f MB %6.0f ms | cull=%7d | rfd=%ld calls %.0f ms (N=%ld) | cpu=%4.0f%% (cores=%lu) busiestThread=%u %3.0f%%\n",
                (DWORD)((GetTickCount() - start) / 1000), (int)frames, maxUs / 1000.0, (int)slow,
                (int)rc, rb / 1048576.0, ru / 1000.0, (int)cull, rfdCalls, rfdUs / 1000.0, (long)s_rfdEntries,
                procPct, si.dwNumberOfProcessors, busyTid, busyPct);
            fflush(s_file);
        }
        while (s_rfdTextPrinted < s_rfdTextCount && s_file) { fprintf(s_file, "RFD %s\n", s_rfdText[s_rfdTextPrinted]); s_rfdTextPrinted++; }
        if (++tick % 15 == 0) ReportHotspots();
    }
    return 0;
}

void InitializeProfiler() {
    QueryPerformanceFrequency(&s_freq);

    char path[MAX_PATH] = {};
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) *(slash + 1) = '\0';
    strcat_s(path, "G3O_profile.log");
    s_file = fopen(path, "w");
    if (s_file) {
        fprintf(s_file, "# G3O profiler. fps=0 means no frames presented (loading screen / menu transition).\n"
                        "# HOT lines: sampling of the busiest thread every 15 s (share of CPU time per function).\n");
        fflush(s_file);
    }

    HMODULE hK32 = GetModuleHandleA("kernel32.dll");
    void* pRead = hK32 ? (void*)GetProcAddress(hK32, "ReadFile") : NULL;
    if (pRead && MH_CreateHook(pRead, (LPVOID)Hooked_ReadFile, (LPVOID*)&Real_ReadFile) == MH_OK) {
        MH_EnableHook(pRead);
    }

    HMODULE hFs = GetModuleHandleA("FileSystem.dll");
    void* pRfd = hFs ? (void*)GetProcAddress(hFs, "?RemoveFromDirCache@CFFFileSystemModule@@AAEXABV?$basic_string@DVallocator@eastl@@@eastl@@_N1@Z") : NULL;
    if (pRfd && MH_CreateHook(pRfd, (LPVOID)Hooked_RemoveFromDirCache, (LPVOID*)&Real_RemoveFromDirCache) == MH_OK) {
        MH_EnableHook(pRfd);
        Log("G3O", "Profiler: RemoveFromDirCache instrumented.");
    }

    CreateThread(NULL, 0, ReporterThread, NULL, 0, NULL);
    // SamplerThread disabled for zero overhead
    Log("G3O", "Profiler enabled, writing G3O_profile.log");
}

void ShutdownProfiler() {
    s_stop = true;
    if (s_file) { fflush(s_file); }
}

} // namespace G3O
