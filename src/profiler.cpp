#include "profiler.h"
#include "diagnostic_logger.h"
#include "d3d9_hook.h"
#include "config_manager.h"
#include "savegame_profiler.h"
#include "timestamped_path.h"
#include "g3o_output_path.h"
#include "profiler_sample_policy.h"
#include "hook_install_policy.h"
#include <minhook.h>
#include <d3d9.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <algorithm>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include <atomic>
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
static std::atomic<bool> s_gameplaySessionStarted{ false };

static SRWLOCK s_frameWindowLock = SRWLOCK_INIT;
static std::vector<LONG> s_frameTimesUsWindow;
static std::vector<LONG> s_allGameplayFrameTimesUs;

static LARGE_INTEGER s_freq = []() {
    LARGE_INTEGER li;
    QueryPerformanceFrequency(&li);
    if (li.QuadPart <= 0) li.QuadPart = 10000000LL;
    return li;
}();
static LARGE_INTEGER s_lastFrame = {};
static volatile bool s_stop = false;
static FILE* s_file = NULL;
static volatile LONG s_profilePathSequence = 0;

// Sample ring buffer, preallocated: the sampler must never allocate while the
// target thread is suspended.
static const LONG SAMPLE_CAP = 1 << 16;
static DWORD s_samples[SAMPLE_CAP];
static volatile LONG s_sampleCount = 0;

// ---- Frame timing (IDirect3DDevice9::Present / eCDX9Device::SwapScreen) -----
typedef HRESULT (STDMETHODCALLTYPE* Present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
static Present_t Real_Present = NULL;
static void* s_targetPresent = nullptr;

void ProfilerRecordPresentedFrame() {
    if (GetConfig().enableProfiler && s_freq.QuadPart > 0) {
        if (!IsGameplayCaptureActive(
                s_gameplaySessionStarted.load(std::memory_order_acquire),
                IsSavegameLoadActive())) {
            s_lastFrame = {};
            return;
        }
        InterlockedExchange(&s_busyTid, (LONG)GetCurrentThreadId());
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (s_lastFrame.QuadPart) {
            LONG us = (LONG)((now.QuadPart - s_lastFrame.QuadPart) * 1000000LL / s_freq.QuadPart);
            LONG prevMax = s_maxFrameUs;
            while (us > prevMax && InterlockedCompareExchange(&s_maxFrameUs, us, prevMax) != prevMax) {
                prevMax = s_maxFrameUs;
            }
            if (us > 33333) InterlockedIncrement(&s_slowFrames);
            if (ShouldRecordGameplayFrameDelta(us)) {
                AcquireSRWLockExclusive(&s_frameWindowLock);
                s_frameTimesUsWindow.push_back(us);
                if (s_allGameplayFrameTimesUs.size() < 360000) {
                    s_allGameplayFrameTimesUs.push_back(us);
                }
                ReleaseSRWLockExclusive(&s_frameWindowLock);
            }
        }
        s_lastFrame = now;
        InterlockedIncrement(&s_frames);
    }
}

void ProfilerSetGameplayReady(bool ready) {
    s_gameplaySessionStarted.store(ready, std::memory_order_release);
}

static HRESULT STDMETHODCALLTYPE Hooked_Present(IDirect3DDevice9* dev, const RECT* a, const RECT* b, HWND w, const RGNDATA* d) {
    ProfilerRecordPresentedFrame();
    return Real_Present(dev, a, b, w, d);
}

typedef int (__thiscall* SwapScreen_t)(void* pThis, HWND hWnd, const void* pSrcRect, const void* pDestRect);
static SwapScreen_t Real_SwapScreen = NULL;
static SwapScreen_t Real_MixerSwapScreen = NULL;
static void* s_targetSwapScreen = nullptr;
static void* s_targetMixerSwapScreen = nullptr;

static int __fastcall Hooked_SwapScreen(void* pThis, void* /*edx*/, HWND hWnd, const void* pSrcRect, const void* pDestRect) {
    if (pThis) {
        void* pDevice = *reinterpret_cast<void**>(reinterpret_cast<BYTE*>(pThis) + 0x08);
        if (pDevice) AttachD3D9DeviceIfReady(pDevice);
    }
    if (!Real_Present) {
        ProfilerRecordPresentedFrame();
    }
    return Real_SwapScreen(pThis, hWnd, pSrcRect, pDestRect);
}

static int __fastcall Hooked_MixerSwapScreen(void* pThis, void* /*edx*/, HWND hWnd, const void* pSrcRect, const void* pDestRect) {
    if (pThis) {
        BYTE* pMixer = reinterpret_cast<BYTE*>(pThis);
        void* pAPIDevice = *reinterpret_cast<void**>(pMixer + 0x0C);
        if (pAPIDevice) {
            void* pDevice = *reinterpret_cast<void**>(reinterpret_cast<BYTE*>(pAPIDevice) + 0x08);
            if (pDevice) AttachD3D9DeviceIfReady(pDevice);
        }
        // [pThis + 0x44] is m_iSkipNextSwapScreens; only record when frame is actually swapped
        int skipCount = *reinterpret_cast<int*>(pMixer + 0x44);
        if (skipCount <= 0 && !Real_Present) {
            ProfilerRecordPresentedFrame();
        }
    }
    return Real_MixerSwapScreen(pThis, hWnd, pSrcRect, pDestRect);
}

void ProfilerHookPresent(void** vtbl) {
    if (!vtbl || !vtbl[17] || s_targetPresent) return;
    void* target = vtbl[17];
    MH_STATUS createStatus = MH_OK, enableStatus = MH_OK, removeStatus = MH_OK;
    const HookInstallResult result = InstallHookSafely(
        [&] { createStatus = MH_CreateHook(target, (LPVOID)Hooked_Present, (LPVOID*)&Real_Present); return createStatus == MH_OK; },
        [&] { enableStatus = MH_EnableHook(target); return enableStatus == MH_OK; },
        [&] { removeStatus = MH_RemoveHook(target); },
        [&] { Real_Present = nullptr; });
    if (result == HookInstallResult::Installed) {
        s_targetPresent = target;
        Log("G3O", "Profiler: Present hooked for frame timing.");
    } else {
        Log("G3O", "Profiler: Present hook failed (create=%d, enable=%d, remove=%d).",
            createStatus, enableStatus, removeStatus);
    }
}

// ---- Sampling profiler of the busiest thread --------------------------------
static DWORD WINAPI SamplerThread(LPVOID) {
    HANDLE hThread = NULL;
    DWORD hTid = 0;
    while (!s_stop) {
        Sleep(10);
        if (!IsGameplayCaptureActive(
                s_gameplaySessionStarted.load(std::memory_order_acquire),
                IsSavegameLoadActive())) {
            Sleep(50);
            continue;
        }
        DWORD tid = (DWORD)InterlockedCompareExchange(&s_busyTid, 0, 0);
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
    if (rva - it->rva > 0x800) {
        // If more than 2KB past the export, it is an unexported internal function; keep module+RVA
        snprintf(buf, sizeof(buf), "%s+0x%X", moduleOut.c_str(), rva);
        return buf;
    }
    snprintf(buf, sizeof(buf), "%s!%s+0x%X", moduleOut.c_str(), it->name.c_str(), rva - it->rva);
    return buf;
}

static void ReportHotspots() {
    LONG n = s_sampleCount;
    if (n < 20 || !s_file) { s_sampleCount = 0; return; }
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
        const std::string key = NormalizeHotspotKey(fn);
        byFunc[key]++;
        size_t bang = key.find('!');
        if (bang != std::string::npos) {
            byModule[key.substr(0, bang)]++;
        } else {
            const size_t rva = key.find("+0x");
            byModule[key.substr(0, rva)]++;
        }
    }
    s_sampleCount = 0;

    auto dump = [&](const char* title, std::unordered_map<std::string, int>& m, size_t top) {
        std::vector<std::pair<std::string, int>> v(m.begin(), m.end());
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        if (s_file) fprintf(s_file, "HOT %s (%ld samples):\n", title, n);
        Log("G3O", "[HotspotProfiler] HOT %s (%ld samples):", title, n);
        for (size_t i = 0; i < v.size() && i < top; ++i) {
            if (s_file) fprintf(s_file, "HOT   %5.1f%%  %s\n", 100.0 * v[i].second / n, v[i].first.c_str());
            Log("G3O", "  HOT   %5.1f%%  %s", 100.0 * v[i].second / n, v[i].first.c_str());
        }
    };
    dump("modules", byModule, 6);
    dump("EIP buckets (nearest export where available)", byFunc, 15);
    if (s_file) fflush(s_file);
}

// ---- Reporter thread --------------------------------------------------------
static ULONGLONG FtToU64(const FILETIME& f) {
    return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime;
}

static DWORD WINAPI ReporterThread(LPVOID) {
    ULONGLONG prevProc = 0;
    DWORD start = GetTickCount();
    SYSTEM_INFO si; GetSystemInfo(&si);
    int tick = 0;
    std::vector<LONG> localFrames;

    while (!s_stop) {
        Sleep(1000);

        LONG frames = InterlockedExchange(&s_frames, 0);
        LONG slow = InterlockedExchange(&s_slowFrames, 0);
        LONG maxUs = InterlockedExchange(&s_maxFrameUs, 0);
        LONG rc = InterlockedExchange(&s_readCalls, 0);
        LONG64 rb = InterlockedExchange64(&s_readBytes, 0);
        LONG64 ru = InterlockedExchange64(&s_readUs, 0);
        LONG cull = InterlockedExchange(&g_cullCalls, 0);

        localFrames.clear();
        AcquireSRWLockExclusive(&s_frameWindowLock);
        localFrames.swap(s_frameTimesUsWindow);
        ReleaseSRWLockExclusive(&s_frameWindowLock);

        double p99Ms = 0.0;
        double low1PctFps = 0.0;
        if (!localFrames.empty()) {
            std::sort(localFrames.begin(), localFrames.end());
            size_t p99Idx = (localFrames.size() * 99) / 100;
            if (p99Idx >= localFrames.size()) p99Idx = localFrames.size() - 1;
            LONG p99Us = localFrames[p99Idx];
            p99Ms = p99Us / 1000.0;
            if (p99Us > 0) low1PctFps = 1000000.0 / (double)p99Us;
        }

        FILETIME c, e, k, u;
        GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
        ULONGLONG proc = FtToU64(k) + FtToU64(u);
        double procPct = prevProc ? (double)(proc - prevProc) / 10000000.0 * 100.0 : 0.0;
        prevProc = proc;

        if (s_file) {
            fprintf(s_file,
                "t=%3lus fps=%3d 1%%low=%5.1f p99=%5.1fms maxFrame=%6.1fms slow(>33ms)=%2d | read: %5d calls %7.1f MB %6.0f ms | cull=%7d | cpu=%4.0f%% (cores=%lu)\n",
                (DWORD)((GetTickCount() - start) / 1000), (int)frames, low1PctFps, p99Ms, maxUs / 1000.0, (int)slow,
                (int)rc, rb / 1048576.0, ru / 1000.0, (int)cull,
                procPct, si.dwNumberOfProcessors);
            fflush(s_file);
        }
        if (++tick % 15 == 0) ReportHotspots();
    }
    return 0;
}

void InitializeProfiler() {
    QueryPerformanceFrequency(&s_freq);

    char path[MAX_PATH] = {};
    char directory[MAX_PATH] = {};
    SYSTEMTIME time;
    GetLocalTime(&time);
    const unsigned long sequence = static_cast<unsigned long>(InterlockedIncrement(&s_profilePathSequence));
    if (EnsureG3OOutputDirectory(directory, sizeof(directory)) &&
        BuildTimestampedFilePath(path, sizeof(path), directory, "G3O_profile", "log", time,
                                 GetCurrentProcessId(), GetCurrentThreadId(), sequence)) {
        s_file = fopen(path, "w");
    }
    if (s_file) {
        fprintf(s_file, "# G3O profiler. fps=0 means no frames presented (loading screen / menu transition).\n"
                        "# FPS/frame-time capture starts after a game session starts and pauses during savegame loading.\n"
                        "# EIP sample shares are not call stacks or direct CPU-utilization percentages.\n"
                        "# Columns include real-time FPS, 1%% low FPS, p99 frame time, max frame time, and process CPU usage.\n");
        fflush(s_file);
    }

    HMODULE hEngine = GetModuleHandleA("Engine.dll");
    void* pSwap = hEngine ? (void*)GetProcAddress(hEngine, "?SwapScreen@eCDX9Device@@UAE?AW4bEResult@@PAUHWND__@@PBVbCRect@@1@Z") : NULL;
    if (pSwap) {
        MH_STATUS createStatus = MH_OK, enableStatus = MH_OK, removeStatus = MH_OK;
        const HookInstallResult result = InstallHookSafely(
            [&] { createStatus = MH_CreateHook(pSwap, (LPVOID)Hooked_SwapScreen, (LPVOID*)&Real_SwapScreen); return createStatus == MH_OK; },
            [&] { enableStatus = MH_EnableHook(pSwap); return enableStatus == MH_OK; },
            [&] { removeStatus = MH_RemoveHook(pSwap); },
            [&] { Real_SwapScreen = nullptr; });
        if (result == HookInstallResult::Installed) {
            s_targetSwapScreen = pSwap;
            Log("G3O", "Profiler: Hooked Engine.dll!eCDX9Device::SwapScreen for frame timing.");
        } else {
            Log("G3O", "Profiler: eCDX9Device::SwapScreen hook failed (create=%d, enable=%d, remove=%d).",
                createStatus, enableStatus, removeStatus);
        }
    }
    void* pMixerSwap = hEngine ? (void*)GetProcAddress(hEngine, "?SwapScreen@eCGfxMixerAdmin@@QAE?AW4bEResult@@QAUHWND__@@PBVbCRect@@1@Z") : NULL;
    if (pMixerSwap) {
        MH_STATUS createStatus = MH_OK, enableStatus = MH_OK, removeStatus = MH_OK;
        const HookInstallResult result = InstallHookSafely(
            [&] { createStatus = MH_CreateHook(pMixerSwap, (LPVOID)Hooked_MixerSwapScreen, (LPVOID*)&Real_MixerSwapScreen); return createStatus == MH_OK; },
            [&] { enableStatus = MH_EnableHook(pMixerSwap); return enableStatus == MH_OK; },
            [&] { removeStatus = MH_RemoveHook(pMixerSwap); },
            [&] { Real_MixerSwapScreen = nullptr; });
        if (result == HookInstallResult::Installed) {
            s_targetMixerSwapScreen = pMixerSwap;
            Log("G3O", "Profiler: Hooked Engine.dll!eCGfxMixerAdmin::SwapScreen for frame timing.");
        } else {
            Log("G3O", "Profiler: eCGfxMixerAdmin::SwapScreen hook failed (create=%d, enable=%d, remove=%d).",
                createStatus, enableStatus, removeStatus);
        }
    }

    CreateThread(NULL, 0, ReporterThread, NULL, 0, NULL);
    CreateThread(NULL, 0, SamplerThread, NULL, 0, NULL);
    Log("G3O", "Profiler enabled; output: %s.", s_file ? path : "unavailable");
}

void ShutdownProfiler() {
    if (s_targetPresent) {
        MH_DisableHook(s_targetPresent);
        MH_RemoveHook(s_targetPresent);
        s_targetPresent = nullptr;
        Real_Present = nullptr;
    }
    if (s_targetSwapScreen) {
        MH_DisableHook(s_targetSwapScreen);
        MH_RemoveHook(s_targetSwapScreen);
        s_targetSwapScreen = nullptr;
        Real_SwapScreen = nullptr;
    }
    if (s_targetMixerSwapScreen) {
        MH_DisableHook(s_targetMixerSwapScreen);
        MH_RemoveHook(s_targetMixerSwapScreen);
        s_targetMixerSwapScreen = nullptr;
        Real_MixerSwapScreen = nullptr;
    }
    s_stop = true;
    ReportHotspots();
    std::vector<LONG> allFrames;
    AcquireSRWLockExclusive(&s_frameWindowLock);
    allFrames = s_allGameplayFrameTimesUs;
    ReleaseSRWLockExclusive(&s_frameWindowLock);

    if (!allFrames.empty()) {
        std::sort(allFrames.begin(), allFrames.end());
        double totalUs = 0.0;
        for (LONG v : allFrames) totalUs += (double)v;
        double avgUs = totalUs / (double)allFrames.size();
        double avgFps = (avgUs > 0.0) ? (1000000.0 / avgUs) : 0.0;
        LONG p99Us = allFrames[(allFrames.size() * 99) / 100];
        LONG p999Us = allFrames[(allFrames.size() * 999) / 1000];
        LONG maxUs = allFrames.back();
        double low1Fps = (p99Us > 0) ? (1000000.0 / (double)p99Us) : 0.0;
        double low01Fps = (p999Us > 0) ? (1000000.0 / (double)p999Us) : 0.0;

        Log("G3O", "[FPS Summary] Total frames=%zu | Avg FPS=%.1f (%.2f ms) | 1%% Low=%.1f FPS (p99=%.2f ms) | 0.1%% Low=%.1f FPS (p99.9=%.2f ms) | Max Spike=%.2f ms",
            allFrames.size(), avgFps, avgUs / 1000.0, low1Fps, p99Us / 1000.0, low01Fps, p999Us / 1000.0, maxUs / 1000.0);
        if (s_file) {
            fprintf(s_file, "# [FPS Summary] Total frames=%zu | Avg FPS=%.1f (%.2f ms) | 1%% Low=%.1f FPS (p99=%.2f ms) | 0.1%% Low=%.1f FPS (p99.9=%.2f ms) | Max Spike=%.2f ms\n",
                allFrames.size(), avgFps, avgUs / 1000.0, low1Fps, p99Us / 1000.0, low01Fps, p999Us / 1000.0, maxUs / 1000.0);
        }
    }
    if (s_file) { fflush(s_file); }
}

} // namespace G3O
