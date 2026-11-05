#include "parallel_pak_decompressor.h"
#include "config_manager.h"
#include "diagnostic_logger.h"
#include <minhook.h>
#include <windows.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <fstream>
#include <stdio.h>

namespace G3O {

static volatile LONG s_prefetchHits = 0;
static volatile LONG s_prefetchMisses = 0;
static volatile LONG64 s_prefetchTimeSavedUs = 0;

LONG GetPakPrefetchHits() { return s_prefetchHits; }
LONG GetPakPrefetchMisses() { return s_prefetchMisses; }
double GetPakPrefetchTimeSavedSec() { return (double)s_prefetchTimeSavedUs / 1000000.0; }

// -----------------------------------------------------------------------------
// bCString Helper in SharedBase.dll
// -----------------------------------------------------------------------------
typedef void (__thiscall* bCString_ctor_t)(void* pThis, const char* str);
typedef void (__thiscall* bCString_dtor_t)(void* pThis);
static bCString_ctor_t Real_bCString_ctor = nullptr;
static bCString_dtor_t Real_bCString_dtor = nullptr;

struct Fake_bCString {
    char data[16];
    Fake_bCString(const char* s) {
        if (Real_bCString_ctor) Real_bCString_ctor(data, s);
        else memset(data, 0, sizeof(data));
    }
    ~Fake_bCString() {
        if (Real_bCString_dtor) Real_bCString_dtor(data);
    }
    void* ptr() { return data; }
};

static const char* GetCStringText(const void* pStr) {
    if (!pStr) return "";
    const char* s = *(const char**)pStr;
    return s ? s : "";
}

// -----------------------------------------------------------------------------
// OpenFileView Hook in FileSystem.dll
// -----------------------------------------------------------------------------
typedef void* (__thiscall* OpenFileView_t)(
    void* pFileSystem,
    const void* pFileNameStr,
    DWORD accessMode,
    DWORD shareMode,
    DWORD creationDisposition
);
static OpenFileView_t Real_OpenFileView = nullptr;
static void* s_targetOpenFileView = nullptr;
static void* s_pFileSystemInstance = nullptr;

// -----------------------------------------------------------------------------
// Prefetch Engine State
// -----------------------------------------------------------------------------
static std::mutex s_prefetchMutex;
static std::condition_variable s_cvQueueNotFull;
static std::unordered_map<std::string, void*> s_prefetchMap;
static std::vector<std::string> s_currentSequence;
static std::vector<std::string> s_recordedSequence;

static std::atomic<bool> s_isPrefetchActive(false);
static std::atomic<bool> s_isRecording(false);
static std::atomic<bool> s_stopWorkers(false);
static std::atomic<size_t> s_workerIndex(0);

static std::vector<std::thread> s_workerThreads;
static std::string s_activeSaveName = "";
static const size_t kMaxPrefetchWindow = 64; // Max prefetched views (~50 MB RAM)

static int GetOptimalWorkerCount() {
    const Config& cfg = GetConfig();
    int count = cfg.pakPrefetchThreads > 0 ? cfg.pakPrefetchThreads :
               (cfg.cpuThreads > 0 ? cfg.cpuThreads : (cfg.cpuCores > 0 ? cfg.cpuCores : 0));
    if (count <= 0) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        count = (int)si.dwNumberOfProcessors;
    }
    // Reserve 1-2 cores for OS/renderer, clamp to reasonable bounds
    if (count > 4) count -= 2;
    if (count < 2) count = 2;
    if (count > 20) count = 20;
    return count;
}

static std::string CleanSaveName(const std::string& name) {
    std::string clean;
    for (char c : name) {
        if (isalnum((unsigned char)c) || c == '_' || c == '-') clean += c;
    }
    return clean.empty() ? "Save" : clean;
}

static std::string GetSeqFilePath(const std::string& saveName) {
    return "G3O_Prefetch_" + CleanSaveName(saveName) + ".seq";
}

static bool LoadSequenceFromFile(const std::string& path, std::vector<std::string>& outSeq) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return false;

    char header[4] = { 0 };
    ifs.read(header, 4);
    if (memcmp(header, "G3OP", 4) != 0) return false;

    uint32_t count = 0;
    ifs.read(reinterpret_cast<char*>(&count), sizeof(count));
    if (count == 0 || count > 50000) return false;

    outSeq.clear();
    outSeq.reserve(count);

    for (uint32_t i = 0; i < count; ++i) {
        uint16_t len = 0;
        ifs.read(reinterpret_cast<char*>(&len), sizeof(len));
        if (len == 0 || len > 1024) break;

        std::string s(len, '\0');
        ifs.read(&s[0], len);
        outSeq.push_back(std::move(s));
    }

    return !outSeq.empty();
}

static void SaveSequenceToFile(const std::string& path, const std::vector<std::string>& seq) {
    if (seq.empty()) return;
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) return;

    ofs.write("G3OP", 4);
    uint32_t count = (uint32_t)seq.size();
    ofs.write(reinterpret_cast<const char*>(&count), sizeof(count));

    for (const auto& s : seq) {
        uint16_t len = (uint16_t)s.size();
        ofs.write(reinterpret_cast<const char*>(&len), sizeof(len));
        ofs.write(s.data(), len);
    }
}

static void ReleaseView(void* pView) {
    if (!pView) return;
    typedef void (__thiscall* Dtor_t)(void* pThis, int flags);
    Dtor_t* pVtable = *(Dtor_t**)pView;
    if (pVtable && pVtable[0]) {
        pVtable[0](pView, 1); // delete pView
    }
}

static void WorkerThreadProc() {
    while (!s_stopWorkers.load(std::memory_order_relaxed)) {
        size_t idx = s_workerIndex.fetch_add(1, std::memory_order_relaxed);
        if (idx >= s_currentSequence.size()) {
            break;
        }

        const std::string& path = s_currentSequence[idx];

        // Wait if prefetch window is full
        {
            std::unique_lock<std::mutex> lock(s_prefetchMutex);
            s_cvQueueNotFull.wait(lock, [&]() {
                return s_stopWorkers.load(std::memory_order_relaxed) ||
                       s_prefetchMap.size() < kMaxPrefetchWindow;
            });
            if (s_stopWorkers.load(std::memory_order_relaxed)) break;
        }

        // Decompress asset via real OpenFileView
        if (s_pFileSystemInstance && Real_OpenFileView) {
            Fake_bCString fakeStr(path.c_str());
            void* pView = Real_OpenFileView(s_pFileSystemInstance, fakeStr.ptr(), 1, 1, 3);
            if (pView) {
                std::lock_guard<std::mutex> lock(s_prefetchMutex);
                s_prefetchMap[path] = pView;
            }
        }
    }
}

static void* __fastcall Hooked_OpenFileView(
    void* pFileSystem,
    void* /*edx*/,
    const void* pFileNameStr,
    DWORD accessMode,
    DWORD shareMode,
    DWORD creationDisposition
) {
    if (!s_pFileSystemInstance) {
        s_pFileSystemInstance = pFileSystem;
    }

    const char* rawPath = GetCStringText(pFileNameStr);
    std::string path = rawPath ? rawPath : "";

    if (s_isPrefetchActive.load(std::memory_order_relaxed) && !path.empty()) {
        void* prefetchedView = nullptr;
        {
            std::lock_guard<std::mutex> lock(s_prefetchMutex);
            auto it = s_prefetchMap.find(path);
            if (it != s_prefetchMap.end()) {
                prefetchedView = it->second;
                s_prefetchMap.erase(it);
                s_cvQueueNotFull.notify_one();
            }
        }

        if (prefetchedView) {
            InterlockedIncrement(&s_prefetchHits);
            InterlockedExchangeAdd64(&s_prefetchTimeSavedUs, 3900); // ~3.9 ms average saved per hit
            return prefetchedView;
        }

        InterlockedIncrement(&s_prefetchMisses);
    }

    if (s_isRecording.load(std::memory_order_relaxed) && !path.empty()) {
        s_recordedSequence.push_back(path);
    }

    return Real_OpenFileView(pFileSystem, pFileNameStr, accessMode, shareMode, creationDisposition);
}

void NotifyGotoLoadedPositionStart(const char* saveName) {
    s_activeSaveName = saveName ? saveName : "QuickSave";
    s_prefetchHits = 0;
    s_prefetchMisses = 0;
    s_prefetchTimeSavedUs = 0;

    std::string seqFile = GetSeqFilePath(s_activeSaveName);
    std::vector<std::string> loadedSeq;

    if (LoadSequenceFromFile(seqFile, loadedSeq)) {
        s_currentSequence = std::move(loadedSeq);
        s_isPrefetchActive.store(true, std::memory_order_relaxed);
        s_isRecording.store(false, std::memory_order_relaxed);
        s_stopWorkers.store(false, std::memory_order_relaxed);
        s_workerIndex.store(0, std::memory_order_relaxed);

        int workers = GetOptimalWorkerCount();
        s_workerThreads.clear();
        s_workerThreads.reserve(workers);

        for (int i = 0; i < workers; ++i) {
            s_workerThreads.emplace_back(WorkerThreadProc);
        }

        Log("G3O", "[ParallelPak] Prefetch pipeline launched: %zu files queued across %d worker threads (Save: %s)",
            s_currentSequence.size(), workers, s_activeSaveName.c_str());
    } else {
        // First run: record sequence for subsequent instant loads
        s_isPrefetchActive.store(false, std::memory_order_relaxed);
        s_isRecording.store(true, std::memory_order_relaxed);
        s_recordedSequence.clear();
        s_recordedSequence.reserve(4096);

        Log("G3O", "[ParallelPak] Recording load sequence for '%s' (will prefetch on next load)", s_activeSaveName.c_str());
    }
}

void NotifyGotoLoadedPositionEnd() {
    // 1. Stop workers
    s_stopWorkers.store(true, std::memory_order_relaxed);
    s_cvQueueNotFull.notify_all();

    for (auto& w : s_workerThreads) {
        if (w.joinable()) w.join();
    }
    s_workerThreads.clear();

    s_isPrefetchActive.store(false, std::memory_order_relaxed);

    // 2. Clean up any prefetched views that weren't consumed
    {
        std::lock_guard<std::mutex> lock(s_prefetchMutex);
        for (auto& pair : s_prefetchMap) {
            ReleaseView(pair.second);
        }
        s_prefetchMap.clear();
    }

    // 3. Save recorded sequence if this was a learning pass
    if (s_isRecording.load(std::memory_order_relaxed) && !s_recordedSequence.empty()) {
        std::string seqFile = GetSeqFilePath(s_activeSaveName);
        SaveSequenceToFile(seqFile, s_recordedSequence);
        Log("G3O", "[ParallelPak] Saved sequence of %zu files to '%s'", s_recordedSequence.size(), seqFile.c_str());
        s_isRecording.store(false, std::memory_order_relaxed);
    } else if (s_prefetchHits > 0 || s_prefetchMisses > 0) {
        Log("G3O", "[ParallelPak] GotoLoadedPosition summary: %ld hits, %ld misses (%.1f%% hit rate), estimated time saved: %.2f s",
            s_prefetchHits, s_prefetchMisses,
            (s_prefetchHits + s_prefetchMisses > 0) ? (100.0 * s_prefetchHits / (s_prefetchHits + s_prefetchMisses)) : 0.0,
            GetPakPrefetchTimeSavedSec());
    }
}

bool InitializeParallelPakDecompressor() {
    HMODULE hShared = GetModuleHandleA("SharedBase.dll");
    if (hShared) {
        Real_bCString_ctor = (bCString_ctor_t)GetProcAddress(hShared, "??0bCString@@QAE@PBD@Z");
        Real_bCString_dtor = (bCString_dtor_t)GetProcAddress(hShared, "??1bCString@@QAE@XZ");
    }

    HMODULE hFs = GetModuleHandleA("FileSystem.dll");
    if (!hFs) return false;

    BYTE* pTarget = (BYTE*)hFs + 0x24E0;
    const BYTE expectedPrologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x51, 0x8B, 0x45, 0x10, 0x56 };
    if (memcmp(pTarget, expectedPrologue, sizeof(expectedPrologue)) == 0) {
        s_targetOpenFileView = pTarget;
        MH_STATUS st = MH_CreateHook(pTarget, (LPVOID)Hooked_OpenFileView, (LPVOID*)&Real_OpenFileView);
        if (st == MH_OK) {
            MH_EnableHook(pTarget);
            Log("G3O", "Parallel Pak Decompressor: Hooked OpenFileView (0x%p)", pTarget);
            return true;
        }
    }

    Log("G3O", "Parallel Pak Decompressor: Could not hook OpenFileView (signature mismatch).");
    return false;
}

void ShutdownParallelPakDecompressor() {
    NotifyGotoLoadedPositionEnd();
    if (s_targetOpenFileView) {
        MH_DisableHook(s_targetOpenFileView);
        s_targetOpenFileView = nullptr;
    }
}

} // namespace G3O
