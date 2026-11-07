#include "pak_tracer.h"
#include "diagnostic_logger.h"
#include "config_manager.h"
#include "g3o_output_path.h"
#include "pak_cache_budget.h"
#include "pak_prefetch_policy.h"
#include "pak_read_at.h"
#include "pak_read_history.h"
#include "decompressor_avx2.h"
#include <libdeflate.h>
#include <windows.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <utility>
#include <stdio.h>

namespace G3O {

struct TraceRecord {
    uint32_t seq;
    char pakName[32];
    uint64_t offset;
    uint32_t inBytes;
    uint32_t outBytes;
    uint64_t decompTimeUs;
    uint64_t loadTimestampUs;
};

struct InFlightRead {
    HANDLE hFile = nullptr;
    uint64_t offset = 0;
    const void* buffer = nullptr;
    DWORD bytesRead = 0;
    uint64_t timestampUs = 0;
    std::string archiveName;
    bool valid = false;
    uint64_t loadGeneration = 0;
};

static constexpr size_t IN_FLIGHT_READ_CAPACITY = 32;
using ThreadReadHistory = RecentReadHistory<InFlightRead, IN_FLIGHT_READ_CAPACITY>;
static thread_local ThreadReadHistory tl_history;
static std::atomic<uint64_t> s_loadGeneration{ 0 };

static bool FindMatchingRead(
    const void* inBuffer,
    DWORD inSize,
    InFlightRead& outMatch,
    InFlightRead** matchedSlot = nullptr) {
    InFlightRead* slot = tl_history.FindMostRecentExact(
        inBuffer, inSize, s_loadGeneration.load(std::memory_order_relaxed));
    if (!slot) return false;
    outMatch = *slot;
    if (matchedSlot) *matchedSlot = slot;
    return true;
}

static std::atomic<bool> s_isActive{ false };
static std::atomic<bool> s_prefetchLookupEnabled{ false };
static std::string s_currentSaveName = "Unknown";
static std::string BuildPakTraceFileName() {
    std::string fileName = "G3O_PakTrace_";
    const char pathSeparator = static_cast<char>(92);
    for (char value : s_currentSaveName) {
        const unsigned char byte = static_cast<unsigned char>(value);
        if (value == pathSeparator || value == '/' || value == ':' || value == '*' ||
            value == '?' || value == '"' || value == '<' || value == '>' || value == '|' || byte < 32) {
            value = '_';
        }
        fileName.push_back(value);
    }
    fileName += ".csv";
    return fileName;
}

static char s_tracePath[MAX_PATH] = {};
static LARGE_INTEGER s_loadStartTime = {};
static LARGE_INTEGER s_freq = []() {
    LARGE_INTEGER li;
    QueryPerformanceFrequency(&li);
    if (li.QuadPart <= 0) li.QuadPart = 10000000LL;
    return li;
}();

static SRWLOCK s_traceLock = SRWLOCK_INIT;
static std::vector<TraceRecord> s_traceRecords;
static std::atomic<uint32_t> s_seqCounter{ 0 };
static std::atomic<uint32_t> s_matchedCount{ 0 };
static std::atomic<uint32_t> s_unmatchedCount{ 0 };
static std::atomic<uint64_t> s_tracerOverheadUs{ 0 };

static SRWLOCK s_handleLock = SRWLOCK_INIT;
static std::unordered_map<HANDLE, std::string> s_handlePathCache;

// -----------------------------------------------------------------------------
// Step 2.4: Priority VIP Texture Pre-Decompression + Bounded Sliding Window
// -----------------------------------------------------------------------------
struct PrefetchCandidate {
    uint32_t seq = 0;
    std::string pakName;
    uint64_t offset = 0;
    uint32_t inBytes = 0;
    uint32_t outBytes = 0;
    uint64_t decompUs = 0;
};

struct CachedBlock {
    uint32_t seq = 0;
    std::string pakName;
    uint64_t offset = 0;
    uint32_t inBytes = 0;
    uint32_t outBytes = 0;
    size_t reservedBytes = 0;
    uint64_t savedUs = 0;
    bool isPriority = false;
    std::vector<BYTE> data;
};

static size_t s_maxRollingMemoryBytes = 192 * 1024 * 1024; // Default 192 MB rolling budget
static PakCacheBudget s_cacheBudget;
static std::atomic<size_t> s_rollingMemoryBytes{ 0 };
static std::atomic<size_t> s_priorityMemoryBytes{ 0 };
static std::atomic<size_t> s_peakMemoryBytes{ 0 };
static std::atomic<uint32_t> s_currentMainSeq{ 0 };
static std::atomic<uint32_t> s_preGameWorldMaxSeq{ 0 };
static std::atomic<uint32_t> s_gotoLoadedPosMinSeq{ 0 };
static std::atomic<bool> s_inGotoLoadedPos{ false };

static SRWLOCK s_cacheLock = SRWLOCK_INIT;
static std::unordered_map<uint64_t, std::shared_ptr<CachedBlock>> s_predecompCache;
static std::unordered_map<uint64_t, std::vector<uint32_t>> s_candidateSeqsByKey;

static size_t s_vipCandidateCount = 0;
static std::vector<PrefetchCandidate> s_candidates;
static const size_t MAX_WORKER_THREADS = 8;
static HANDLE s_workerThreads[MAX_WORKER_THREADS] = {};
static size_t s_activeWorkerCount = 0;
static std::atomic<size_t> s_candidateIndex{ 0 };

static HANDLE s_memAvailableEvent = nullptr;
static bool s_hasValidTrace = false;

static std::atomic<uint32_t> s_cacheHits{ 0 };
static std::atomic<uint32_t> s_priorityHits{ 0 };
static std::atomic<uint32_t> s_cacheMisses{ 0 };
static std::atomic<uint64_t> s_cacheHitBytes{ 0 };
static std::atomic<uint64_t> s_cacheSavedTimeUs{ 0 };


static void ReleaseCacheMemory(size_t bytes, bool priority) {
    if (priority) {
        s_priorityMemoryBytes.fetch_sub(bytes, std::memory_order_relaxed);
    } else {
        s_rollingMemoryBytes.fetch_sub(bytes, std::memory_order_relaxed);
    }
    s_cacheBudget.Release(bytes);
    if (s_memAvailableEvent) SetEvent(s_memAvailableEvent);
}

static bool ReserveCacheMemory(size_t bytes, bool priority) {
    if (!s_cacheBudget.TryReserve(bytes)) return false;
    if (priority) {
        s_priorityMemoryBytes.fetch_add(bytes, std::memory_order_relaxed);
    } else {
        s_rollingMemoryBytes.fetch_add(bytes, std::memory_order_relaxed);
    }
    const size_t total = s_cacheBudget.Used();
    size_t peak = s_peakMemoryBytes.load(std::memory_order_relaxed);
    while (total > peak && !s_peakMemoryBytes.compare_exchange_weak(
               peak, total, std::memory_order_relaxed)) {}
    return true;
}

static bool StorePrefetchedBlock(uint64_t key, const std::shared_ptr<CachedBlock>& block) {
    AcquireSRWLockExclusive(&s_cacheLock);
    const bool inserted = s_predecompCache.emplace(key, block).second;
    ReleaseSRWLockExclusive(&s_cacheLock);
    if (!inserted) ReleaseCacheMemory(block->reservedBytes, block->isPriority);
    return inserted;
}

static inline uint64_t MakeCacheKey(const std::string& pakName, uint64_t offset) {
    uint64_t hash = 14695981039346656037ULL;
    for (char c : pakName) {
        hash ^= (uint8_t)tolower((unsigned char)c);
        hash *= 1099511628211ULL;
    }
    hash ^= (offset * 0x9E3779B97F4A7C15ULL);
    return hash;
}

static void PurgeStaleBlocksUnderLock(uint32_t minSeq) {
    for (auto it = s_predecompCache.begin(); it != s_predecompCache.end(); ) {
        if (it->second->seq < minSeq) {
            ReleaseCacheMemory(it->second->reservedBytes, it->second->isPriority);
            it = s_predecompCache.erase(it);
        } else {
            ++it;
        }
    }
}

static std::string ResolveArchiveName(HANDLE h) {
    if (!h || h == INVALID_HANDLE_VALUE) return "";

    AcquireSRWLockShared(&s_handleLock);
    auto it = s_handlePathCache.find(h);
    if (it != s_handlePathCache.end()) {
        std::string name = it->second;
        ReleaseSRWLockShared(&s_handleLock);
        return name;
    }
    ReleaseSRWLockShared(&s_handleLock);

    char path[MAX_PATH] = { 0 };
    DWORD len = GetFinalPathNameByHandleA(h, path, MAX_PATH, FILE_NAME_NORMALIZED);
    std::string archiveName = "";
    if (len > 0) {
        std::string s(path);
        size_t pos = s.find_last_of("\\/");
        if (pos != std::string::npos) s = s.substr(pos + 1);

        size_t dot = s.find_last_of('.');
        if (dot != std::string::npos) {
            std::string ext = s.substr(dot);
            for (char& c : ext) c = (char)tolower((unsigned char)c);
            if (ext == ".pak" || ext == ".mod" || ext == ".cpt" ||
                (ext.size() == 4 && (ext[1] == 'm' || ext[1] == 'p' || ext[1] == 'c') && isdigit((unsigned char)ext[2]))) {
                archiveName = s;
            }
        }
    }

    AcquireSRWLockExclusive(&s_handleLock);
    s_handlePathCache[h] = archiveName;
    ReleaseSRWLockExclusive(&s_handleLock);
    return archiveName;
}

static std::string GetSavegamePath(const std::string& saveName) {
    char myDocs[MAX_PATH] = { 0 };
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, myDocs))) {
        std::string path = std::string(myDocs) + "\\Gothic3\\" + saveName + ".g3savcpxdat";
        DWORD attr = GetFileAttributesA(path.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            return path;
        }
    }
    return "";
}

static bool IsTraceValid(const std::string& csvPath, const std::string& saveName) {
    HANDLE hCsv = CreateFileA(csvPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hCsv == INVALID_HANDLE_VALUE) return false;
    FILETIME ftCsvWrite = { 0 };
    GetFileTime(hCsv, NULL, NULL, &ftCsvWrite);
    CloseHandle(hCsv);

    std::string savePath = GetSavegamePath(saveName);
    if (!savePath.empty()) {
        HANDLE hSave = CreateFileA(savePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (hSave != INVALID_HANDLE_VALUE) {
            FILETIME ftSaveWrite = { 0 };
            GetFileTime(hSave, NULL, NULL, &ftSaveWrite);
            CloseHandle(hSave);

            ULARGE_INTEGER uCsv, uSave;
            uCsv.LowPart = ftCsvWrite.dwLowDateTime; uCsv.HighPart = ftCsvWrite.dwHighDateTime;
            uSave.LowPart = ftSaveWrite.dwLowDateTime; uSave.HighPart = ftSaveWrite.dwHighDateTime;

            if (uSave.QuadPart > uCsv.QuadPart) {
                Log("G3O", "[PakTracer] Trace invalidated: savegame '%s' was modified after trace was recorded.", saveName.c_str());
                return false;
            }
        }
    }
    return true;
}

// Background pre-decompression thread worker
static DWORD WINAPI BgPredecompWorker(LPVOID lpParam) {
    size_t workerId = (size_t)lpParam;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    libdeflate_decompressor* dec = nullptr;
    if (G3O_AVX2_IsSupported()) {
        dec = G3O_AVX2_AllocDecompressor();
    } else {
        dec = libdeflate_alloc_decompressor();
    }
    if (!dec) return 1;

    // Cache open PAK handles per thread to eliminate repeated CreateFile/CloseHandle syscalls
    std::unordered_map<std::string, HANDLE> pakHandles;

    auto GetArchiveHandle = [&](const std::string& pakName) -> HANDLE {
        auto hit = pakHandles.find(pakName);
        if (hit != pakHandles.end()) return hit->second;
        std::string fullPath = "Data\\" + pakName;
        HANDLE h = CreateFileA(fullPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            pakHandles[pakName] = h;
        }
        return h;
    };

    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    size_t prioCount = 0;
    size_t normCount = 0;
    std::vector<BYTE> compBuf;

    // =========================================================================
    // Unified Chronological Rolling Window (Regular + VIP blocks in load order)
    // All workers cooperate in parallel via atomic s_candidateIndex
    // =========================================================================
    while (s_isActive.load(std::memory_order_relaxed)) {
        size_t i = s_candidateIndex.fetch_add(1, std::memory_order_relaxed);
        if (i >= s_candidates.size()) break;

        const auto& cand = s_candidates[i];
        const bool isVip = (cand.outBytes >= 50 * 1024 * 1024);

        // Skip immediately if main thread has already passed this candidate
        if (cand.seq <= s_currentMainSeq.load(std::memory_order_relaxed)) {
            continue;
        }
        // Do not prefetch GotoLoadedPosition blocks until Pre-Load sector activation finishes
        if (!s_inGotoLoadedPos.load(std::memory_order_acquire) &&
            s_preGameWorldMaxSeq.load(std::memory_order_relaxed) > 0 &&
            cand.seq > s_preGameWorldMaxSeq.load(std::memory_order_relaxed)) {
            while (!s_inGotoLoadedPos.load(std::memory_order_acquire) &&
                   s_isActive.load(std::memory_order_relaxed)) {
                WaitForSingleObject(s_memAvailableEvent, 5);
            }
            if (!s_isActive.load(std::memory_order_relaxed)) break;
            if (cand.seq <= s_currentMainSeq.load(std::memory_order_relaxed)) continue;
        }

        if (cand.outBytes > s_cacheBudget.Limit()) continue;
        bool reserved = ReserveCacheMemory(cand.outBytes, isVip);
        while (!reserved && s_isActive.load(std::memory_order_relaxed)) {
            const uint32_t curSeq = s_currentMainSeq.load(std::memory_order_relaxed);
            if (cand.seq <= curSeq) break;

            AcquireSRWLockExclusive(&s_cacheLock);
            PurgeStaleBlocksUnderLock(curSeq);
            ReleaseSRWLockExclusive(&s_cacheLock);

            reserved = ReserveCacheMemory(cand.outBytes, isVip);
            if (!reserved) WaitForSingleObject(s_memAvailableEvent, 5);
        }
        if (!s_isActive.load(std::memory_order_relaxed)) break;
        if (!reserved) continue;
        if (cand.seq <= s_currentMainSeq.load(std::memory_order_relaxed)) {
            ReleaseCacheMemory(cand.outBytes, isVip);
            continue;
        }

        HANDLE hFile = GetArchiveHandle(cand.pakName);
        if (hFile == INVALID_HANDLE_VALUE) {
            ReleaseCacheMemory(cand.outBytes, isVip);
            continue;
        }

        if (compBuf.size() < cand.inBytes) {
            compBuf.resize(cand.inBytes);
        }
        bool readOk = false;
        {
            ScopedInternalPakRead internalRead;
            readOk = ReadExactAt(hFile, cand.offset, compBuf.data(), cand.inBytes);
        }
        if (!readOk) {
            ReleaseCacheMemory(cand.outBytes, isVip);
            continue;
        }

        auto block = std::make_shared<CachedBlock>();
        block->seq = cand.seq;
        block->pakName = cand.pakName;
        block->offset = cand.offset;
        block->inBytes = cand.inBytes;
        block->outBytes = cand.outBytes;
        block->reservedBytes = cand.outBytes;
        block->savedUs = cand.decompUs;
        block->isPriority = isVip;
        block->data.resize(cand.outBytes);

        size_t actualOut = 0;
        int res = -1;
        if (G3O_AVX2_IsSupported()) {
            res = G3O_AVX2_DecompressZlib(dec, compBuf.data(), cand.inBytes, block->data.data(), cand.outBytes, &actualOut);
        } else {
            res = libdeflate_zlib_decompress(dec, compBuf.data(), cand.inBytes, block->data.data(), cand.outBytes, &actualOut);
        }

        if (res == 0 && actualOut > 0) {
            block->outBytes = static_cast<uint32_t>(actualOut);
            const uint64_t key = MakeCacheKey(block->pakName, block->offset);
            if (StorePrefetchedBlock(key, block)) {
                if (isVip) prioCount++;
                else normCount++;
            }
        } else {
            ReleaseCacheMemory(block->reservedBytes, isVip);
        }
    }

    // Clean up cached handles
    for (auto& pair : pakHandles) {
        if (pair.second != INVALID_HANDLE_VALUE) {
            CloseHandle(pair.second);
        }
    }
    pakHandles.clear();

    if (G3O_AVX2_IsSupported()) {
        G3O_AVX2_FreeDecompressor(dec);
    } else {
        libdeflate_free_decompressor(dec);
    }

    QueryPerformanceCounter(&t1);
    double elapsedMs = (s_freq.QuadPart > 0) ? ((double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)s_freq.QuadPart) : 0.0;
    if (prioCount + normCount > 0) {
        Log("G3O", "[PakCache] Worker #%zu completed %zu pre-decompressions (%zu VIP + %zu regular) in %.2f ms",
            workerId + 1, prioCount + normCount, prioCount, normCount, elapsedMs);
    }
    return 0;
}

bool PakTracer_IsActive() {
    return s_isActive.load(std::memory_order_relaxed);
}

bool PakTracer_CanTryGetPredecompressed() {
    return ShouldTryPredecompressedLookup(
        s_isActive.load(std::memory_order_acquire),
        s_prefetchLookupEnabled.load(std::memory_order_acquire));
}

void PakTracer_OnLoadStart(const char* saveName) {
    s_prefetchLookupEnabled.store(false, std::memory_order_release);
    if (s_activeWorkerCount != 0) {
        Log("G3O", "[PakCache] Previous worker shutdown is incomplete; skipping prefetch for this load.");
        return;
    }
    s_isActive.store(false, std::memory_order_release);
    s_currentSaveName = (saveName && *saveName) ? saveName : "Savegame";
    s_loadGeneration.fetch_add(1, std::memory_order_relaxed);
    QueryPerformanceCounter(&s_loadStartTime);

    AcquireSRWLockExclusive(&s_traceLock);
    s_traceRecords.clear();
    s_traceRecords.reserve(8192);
    ReleaseSRWLockExclusive(&s_traceLock);

    s_seqCounter.store(0, std::memory_order_relaxed);
    s_matchedCount.store(0, std::memory_order_relaxed);
    s_unmatchedCount.store(0, std::memory_order_relaxed);
    s_tracerOverheadUs.store(0, std::memory_order_relaxed);

    s_cacheHits.store(0, std::memory_order_relaxed);
    s_priorityHits.store(0, std::memory_order_relaxed);
    s_cacheMisses.store(0, std::memory_order_relaxed);
    s_cacheHitBytes.store(0, std::memory_order_relaxed);
    s_cacheSavedTimeUs.store(0, std::memory_order_relaxed);
    s_rollingMemoryBytes.store(0, std::memory_order_relaxed);
    s_priorityMemoryBytes.store(0, std::memory_order_relaxed);
    s_peakMemoryBytes.store(0, std::memory_order_relaxed);
    s_currentMainSeq.store(0, std::memory_order_relaxed);
    s_preGameWorldMaxSeq.store(0, std::memory_order_relaxed);
    s_gotoLoadedPosMinSeq.store(0, std::memory_order_relaxed);
    s_inGotoLoadedPos.store(false, std::memory_order_relaxed);


    s_vipCandidateCount = 0;
    s_candidates.clear();
    s_candidateSeqsByKey.clear();
    tl_history.Clear();

    if (!s_memAvailableEvent) {
        s_memAvailableEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
    }
    if (!s_memAvailableEvent) {
        Log("G3O", "[PakCache] Could not create the cache-budget event; background pre-decompression will be skipped.");
    }

    char outputDirectory[MAX_PATH] = {};
    const std::string traceFileName = BuildPakTraceFileName();
    s_tracePath[0] = '\0';
    if (EnsureG3OOutputDirectory(outputDirectory, sizeof(outputDirectory))) {
        BuildG3OOutputFilePath(s_tracePath, sizeof(s_tracePath), outputDirectory, traceFileName.c_str());
    }
    s_hasValidTrace = s_tracePath[0] && IsTraceValid(s_tracePath, s_currentSaveName);
    if (!s_tracePath[0]) {
        Log("G3O", "[PakTracer] Trace path could not be created under G3O.");
    }

    size_t requestedWorkerCount = 0;
    if (s_hasValidTrace) {
        std::ifstream ifs(s_tracePath);
        if (ifs.is_open()) {
            std::string line;
            std::getline(ifs, line); // header
            double prevLoadMs = 0.0;
            uint32_t prevRowSeq = 0;
            while (std::getline(ifs, line)) {
                std::stringstream ss(line);
                std::string item;
                std::vector<std::string> cols;
                while (std::getline(ss, item, ',')) cols.push_back(item);
                if (cols.size() >= 7) {
                    uint64_t decomp_us = std::stoull(cols[5]);
                    uint32_t outSz = std::stoul(cols[4]);
                    double load_ms = std::stod(cols[6]);

                    PrefetchCandidate cand;
                    cand.seq = std::stoul(cols[0]);
                    cand.pakName = cols[1];
                    cand.offset = std::stoull(cols[2], nullptr, 16);
                    cand.inBytes = std::stoul(cols[3]);
                    cand.outBytes = outSz;
                    cand.decompUs = decomp_us;

                    if (prevRowSeq > 0 && s_preGameWorldMaxSeq.load(std::memory_order_relaxed) == 0 &&
                        (load_ms - prevLoadMs) >= 1500.0 && prevLoadMs >= 5000.0) {
                        s_preGameWorldMaxSeq.store(prevRowSeq, std::memory_order_relaxed);
                        s_gotoLoadedPosMinSeq.store(cand.seq, std::memory_order_relaxed);
                    }
                    prevLoadMs = load_ms;
                    prevRowSeq = cand.seq;

                    const uint64_t key = MakeCacheKey(cand.pakName, cand.offset);
                    s_candidateSeqsByKey[key].push_back(cand.seq);

                    // Universal chronological filter: keep VIP textures in sequence alongside regular blocks
                    if (outSz >= 50 * 1024 * 1024 && outSz <= 128 * 1024 * 1024) {
                        s_vipCandidateCount++;
                        s_candidates.push_back(cand);
                    } else if (decomp_us >= 150 && outSz >= 2048 && outSz < 50 * 1024 * 1024) {
                        s_candidates.push_back(cand);
                    }
                }
            }
            ifs.close();
        }

        const Config& cfg = GetConfig();
        s_maxRollingMemoryBytes = static_cast<size_t>(cfg.pakCacheBudgetMB) * 1024 * 1024;
        s_cacheBudget.Configure(s_maxRollingMemoryBytes);
        if (s_maxRollingMemoryBytes == 0) {
            Log("G3O", "[PakCache] Trace pre-decompression disabled by PakCacheBudgetMB=0.");
        } else if (!s_candidates.empty() && s_memAvailableEvent) {

            size_t numWorkers = 4;
            int effectiveCores = cfg.cpuCores;
            if (effectiveCores <= 0) {
                SYSTEM_INFO si = {};
                GetSystemInfo(&si);
                effectiveCores = (si.dwNumberOfProcessors >= 2) ? static_cast<int>(si.dwNumberOfProcessors / 2) : 1;
            }
            if (cfg.pakPrefetchWorkers > 0) {
                numWorkers = (size_t)cfg.pakPrefetchWorkers;
            } else if (effectiveCores >= 8) {
                numWorkers = 6;
            } else if (effectiveCores >= 6) {
                numWorkers = 4;
            } else if (effectiveCores >= 4) {
                numWorkers = 3;
            } else {
                numWorkers = 1;
            }
            if (numWorkers > MAX_WORKER_THREADS) numWorkers = MAX_WORKER_THREADS;

            s_candidateIndex.store(0, std::memory_order_relaxed);
            const size_t regularCount = s_candidates.size() - s_vipCandidateCount;

            Log("G3O", "[PakCache] Starting pre-decompression pool: %zu worker threads, budget: %zu MB (%zu VIP + %zu regular candidates) for '%s'",
                numWorkers, s_maxRollingMemoryBytes / (1024 * 1024), s_vipCandidateCount, regularCount, s_currentSaveName.c_str());

            requestedWorkerCount = numWorkers;
        }
    } else {
        Log("G3O", "[PakTracer] No valid trace for '%s' (savegame newer or first run) - recording fresh trace",
            s_currentSaveName.c_str());
    }

    s_isActive.store(true, std::memory_order_release);
    for (size_t w = 0; w < requestedWorkerCount; ++w) {
        HANDLE thread = CreateThread(nullptr, 0, BgPredecompWorker, reinterpret_cast<LPVOID>(w), 0, nullptr);
        if (!thread) {
            Log("G3O", "[PakCache] Worker %zu could not be created (error %lu).", w, GetLastError());
            continue;
        }
        s_workerThreads[s_activeWorkerCount++] = thread;
    }
    if (requestedWorkerCount > 0 && s_activeWorkerCount == 0) {
        Log("G3O", "[PakCache] No background workers started; the load will use the normal PAK read path.");
    }
    if (s_activeWorkerCount > 0) {
        s_prefetchLookupEnabled.store(true, std::memory_order_release);
    }
    Log("G3O", "[PakTracer] Tracing started for savegame '%s'", s_currentSaveName.c_str());
}

void PakTracer_RecordRead(
    HANDLE hFile,
    const void* buffer,
    DWORD bytesRequested,
    DWORD bytesRead,
    LPOVERLAPPED ov
) {
    if (!s_isActive.load(std::memory_order_relaxed)) return;
    if (!ov || !buffer || bytesRead == 0) return;

    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);

    std::string archive = ResolveArchiveName(hFile);
    if (archive.empty()) return;

    InFlightRead read;
    read.hFile = hFile;
    read.offset = ((uint64_t)ov->OffsetHigh << 32) | (uint64_t)ov->Offset;
    read.buffer = buffer;
    read.bytesRead = bytesRead;
    read.loadGeneration = s_loadGeneration.load(std::memory_order_relaxed);
    read.timestampUs = (s_freq.QuadPart > 0) ? ((t0.QuadPart - s_loadStartTime.QuadPart) * 1000000LL / s_freq.QuadPart) : 0;
    read.archiveName = std::move(archive);
    read.valid = true;

    tl_history.Push(std::move(read));

    QueryPerformanceCounter(&t1);
    uint64_t oh = (s_freq.QuadPart > 0) ? ((t1.QuadPart - t0.QuadPart) * 1000000LL / s_freq.QuadPart) : 0;
    s_tracerOverheadUs.fetch_add(oh, std::memory_order_relaxed);
}

void PakTracer_OnLoadGameWorldStart() {
    if (!s_isActive.load(std::memory_order_relaxed)) return;
    // Allow background workers to start prefetching GotoLoadedPosition blocks during LoadGameWorld's 2.3s CPU phase
    s_inGotoLoadedPos.store(true, std::memory_order_release);
    if (s_memAvailableEvent) SetEvent(s_memAvailableEvent);
}

void PakTracer_OnGotoLoadedPositionStart() {
    if (!s_isActive.load(std::memory_order_relaxed)) return;
    s_inGotoLoadedPos.store(true, std::memory_order_release);
    const uint32_t minSeq = s_gotoLoadedPosMinSeq.load(std::memory_order_relaxed);
    if (minSeq > 0) {
        uint32_t prevSeq = s_currentMainSeq.load(std::memory_order_relaxed);
        while (minSeq - 1 > prevSeq && !s_currentMainSeq.compare_exchange_weak(
                   prevSeq, minSeq - 1, std::memory_order_relaxed)) {}
        AcquireSRWLockExclusive(&s_cacheLock);
        PurgeStaleBlocksUnderLock(minSeq);
        ReleaseSRWLockExclusive(&s_cacheLock);
    }
    if (s_memAvailableEvent) SetEvent(s_memAvailableEvent);
}

bool PakTracer_TryGetPredecompressed(
    const void* inBuffer,
    DWORD inSize,
    void* outBuffer,
    DWORD outCapacity,
    DWORD* pActualOut
) {
    if (!PakTracer_CanTryGetPredecompressed()) return false;
    InFlightRead read;
    InFlightRead* matchedSlot = nullptr;
    if (!FindMatchingRead(inBuffer, inSize, read, &matchedSlot)) {
        return false;
    }

    const std::string& archive = read.archiveName;
    if (archive.empty()) return false;

    uint64_t key = MakeCacheKey(archive, read.offset);

    std::shared_ptr<CachedBlock> block;
    AcquireSRWLockExclusive(&s_cacheLock);
    auto it = s_predecompCache.find(key);
    if (it == s_predecompCache.end() || !it->second) {
        ReleaseSRWLockExclusive(&s_cacheLock);
        s_cacheMisses.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    block = it->second;
    if (!PakReadIdentityMatches(
            block->pakName.c_str(), block->offset, block->inBytes,
            archive.c_str(), read.offset, inSize)) {
        ReleaseSRWLockExclusive(&s_cacheLock);
        s_cacheMisses.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    s_predecompCache.erase(it);

    if (outCapacity < block->outBytes) {
        s_cacheMisses.fetch_add(1, std::memory_order_relaxed);
        std::vector<BYTE>().swap(block->data);
        ReleaseCacheMemory(block->reservedBytes, block->isPriority);
        ReleaseSRWLockExclusive(&s_cacheLock);
        return false;
    }

    // Fast memcpy directly into engine buffer!
    memcpy(outBuffer, block->data.data(), block->outBytes);
    if (pActualOut) {
        *pActualOut = block->outBytes;
    }

    // Telemetry & budget
    s_cacheHits.fetch_add(1, std::memory_order_relaxed);
    if (block->isPriority) {
        s_priorityHits.fetch_add(1, std::memory_order_relaxed);
    }
    s_cacheHitBytes.fetch_add(block->outBytes, std::memory_order_relaxed);
    s_cacheSavedTimeUs.fetch_add(block->savedUs, std::memory_order_relaxed);

    // Free block memory immediately
    std::vector<BYTE>().swap(block->data);
    ReleaseCacheMemory(block->reservedBytes, block->isPriority);
    ReleaseSRWLockExclusive(&s_cacheLock);

    // Record trace record for profiler reporting
    TraceRecord rec;
    rec.seq = s_seqCounter.fetch_add(1, std::memory_order_relaxed) + 1;
    strncpy_s(rec.pakName, sizeof(rec.pakName), archive.c_str(), _TRUNCATE);
    rec.offset = read.offset;
    rec.inBytes = inSize;
    rec.outBytes = block->outBytes;
    rec.decompTimeUs = 50; // served from cache
    rec.loadTimestampUs = read.timestampUs;

    uint32_t targetSeq = (block->seq > rec.seq) ? block->seq : rec.seq;
    uint32_t prevSeq = s_currentMainSeq.load(std::memory_order_relaxed);
    while (targetSeq > prevSeq && !s_currentMainSeq.compare_exchange_weak(
               prevSeq, targetSeq, std::memory_order_relaxed)) {}

    AcquireSRWLockExclusive(&s_traceLock);
    s_traceRecords.push_back(rec);
    ReleaseSRWLockExclusive(&s_traceLock);

    s_matchedCount.fetch_add(1, std::memory_order_relaxed);
    tl_history.Consume(matchedSlot);
    return true;
}

void PakTracer_RecordDecompress(
    const void* inBuffer,
    DWORD inSize,
    DWORD outCapacity,
    DWORD actualOut,
    uint64_t decompTimeUs,
    const void* /*outBuffer*/
) {
    if (!s_isActive.load(std::memory_order_relaxed)) return;

    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);

    InFlightRead read;
    InFlightRead* matchedSlot = nullptr;
    if (FindMatchingRead(inBuffer, inSize, read, &matchedSlot)) {
        tl_history.Consume(matchedSlot);
        TraceRecord rec;
        rec.seq = s_seqCounter.fetch_add(1, std::memory_order_relaxed) + 1;
        const std::string& archive = read.archiveName;
        strncpy_s(rec.pakName, sizeof(rec.pakName), archive.c_str(), _TRUNCATE);
        rec.offset = read.offset;
        rec.inBytes = inSize;
        rec.outBytes = actualOut ? actualOut : outCapacity;
        rec.decompTimeUs = decompTimeUs;
        rec.loadTimestampUs = read.timestampUs;

        const uint32_t curMainSeq = s_currentMainSeq.load(std::memory_order_relaxed);
        uint32_t targetSeq = rec.seq;
        const uint64_t key = MakeCacheKey(archive, rec.offset);
        auto seqIt = s_candidateSeqsByKey.find(key);
        if (seqIt != s_candidateSeqsByKey.end()) {
            const auto& seqVec = seqIt->second;
            auto itNext = std::lower_bound(seqVec.begin(), seqVec.end(), curMainSeq + 1);
            if (itNext != seqVec.end()) {
                // Only advance if within a reasonable chronological window (avoid jumping 4000 seq ahead on repeated blocks)
                if (*itNext <= curMainSeq + 256 || curMainSeq == 0) {
                    targetSeq = *itNext;
                }
            }
        }
        uint32_t prevSeq = curMainSeq;
        while (targetSeq > prevSeq && !s_currentMainSeq.compare_exchange_weak(
                   prevSeq, targetSeq, std::memory_order_relaxed)) {}
        const uint32_t purgeSeq = s_currentMainSeq.load(std::memory_order_relaxed);

        // Purge any stale blocks older than purgeSeq
        AcquireSRWLockExclusive(&s_cacheLock);
        PurgeStaleBlocksUnderLock(purgeSeq);
        ReleaseSRWLockExclusive(&s_cacheLock);
        if (s_memAvailableEvent) SetEvent(s_memAvailableEvent);

        AcquireSRWLockExclusive(&s_traceLock);
        s_traceRecords.push_back(rec);
        ReleaseSRWLockExclusive(&s_traceLock);

        s_matchedCount.fetch_add(1, std::memory_order_relaxed);
    } else {
        s_unmatchedCount.fetch_add(1, std::memory_order_relaxed);
    }

    QueryPerformanceCounter(&t1);
    uint64_t oh = (s_freq.QuadPart > 0) ? ((t1.QuadPart - t0.QuadPart) * 1000000LL / s_freq.QuadPart) : 0;
    s_tracerOverheadUs.fetch_add(oh, std::memory_order_relaxed);
}


void PakTracer_OnLoadEnd() {
    s_prefetchLookupEnabled.store(false, std::memory_order_release);
    s_isActive.store(false, std::memory_order_release);
    tl_history.Clear();

    if (s_memAvailableEvent) {
        SetEvent(s_memAvailableEvent);
    }

    bool allWorkersStopped = true;
    for (size_t w = 0; w < s_activeWorkerCount; ++w) {
        if (!s_workerThreads[w]) continue;
        const DWORD wait = WaitForSingleObject(s_workerThreads[w], INFINITE);
        if (wait != WAIT_OBJECT_0) {
            Log("G3O", "[PakCache] Worker %zu did not join (wait=%lu, error=%lu); cache state retained.",
                w, wait, GetLastError());
            allWorkersStopped = false;
            continue;
        }
        CloseHandle(s_workerThreads[w]);
        s_workerThreads[w] = nullptr;
    }
    if (!allWorkersStopped) {
        return;
    }
    s_activeWorkerCount = 0;

    if (s_memAvailableEvent) {
        CloseHandle(s_memAvailableEvent);
        s_memAvailableEvent = nullptr;
    }

    // Free all remaining cached blocks
    AcquireSRWLockExclusive(&s_cacheLock);
    s_predecompCache.clear();
    ReleaseSRWLockExclusive(&s_cacheLock);
    s_cacheBudget.Configure(0);
    s_rollingMemoryBytes.store(0, std::memory_order_relaxed);
    s_priorityMemoryBytes.store(0, std::memory_order_relaxed);

    AcquireSRWLockShared(&s_traceLock);
    uint32_t totalMatched = (uint32_t)s_traceRecords.size();
    uint32_t matched = s_matchedCount.load(std::memory_order_relaxed);
    uint32_t unmatched = s_unmatchedCount.load(std::memory_order_relaxed);
    uint32_t totalDecomps = matched + unmatched;
    double matchRate = (totalDecomps > 0) ? (100.0 * (double)matched / (double)totalDecomps) : 0.0;
    double overheadMs = (double)s_tracerOverheadUs.load(std::memory_order_relaxed) / 1000.0;

    uint32_t hits = s_cacheHits.load(std::memory_order_relaxed);
    uint32_t prioHits = s_priorityHits.load(std::memory_order_relaxed);
    uint32_t misses = s_cacheMisses.load(std::memory_order_relaxed);
    double hitMb = (double)s_cacheHitBytes.load(std::memory_order_relaxed) / (1024.0 * 1024.0);
    double savedSec = (double)s_cacheSavedTimeUs.load(std::memory_order_relaxed) / 1000000.0;
    double peakMb = (double)s_peakMemoryBytes.load(std::memory_order_relaxed) / (1024.0 * 1024.0);

    Log("G3O", "================================================================================");
    Log("G3O", "[PakTracer] === SAVEGAME LOAD TRACE REPORT: '%s' ===", s_currentSaveName.c_str());
    Log("G3O", "  Total decompressions: %u", totalDecomps);
    Log("G3O", "  Matched to PAK reads: %u (%.1f%%)", matched, matchRate);
    Log("G3O", "  Unmatched / Other:    %u (%.1f%%)", unmatched, 100.0 - matchRate);
    Log("G3O", "  Tracer overhead:      %.2f ms", overheadMs);

    size_t totalCandidates = s_candidates.size();
    if (totalCandidates > 0) {
        const size_t regularCount = totalCandidates - s_vipCandidateCount;
        double hitPct = (hits + misses > 0) ? (100.0 * (double)hits / (double)(hits + misses)) : 0.0;
        Log("G3O", "[PakCache] === PRE-DECOMPRESSION CACHE PERFORMANCE REPORT ===");
        Log("G3O", "  Candidates Queued:        %zu (%zu VIP Priority + %zu Regular)",
            totalCandidates, s_vipCandidateCount, regularCount);
        Log("G3O", "  Cache HITS:               %u (%.1f%%)", hits, hitPct);
        Log("G3O", "  VIP Priority HITS:        %u / %zu", prioHits, s_vipCandidateCount);
        Log("G3O", "  Cache MISSES:             %u (%.1f%%)", misses, 100.0 - hitPct);
        Log("G3O", "  Data Served from Cache:   %.2f MB", hitMb);
        Log("G3O", "  Estimated CPU Time Saved: %.2f s", savedSec);

        Log("G3O", "  Peak Cache Memory Used:   %.2f MB (Total budget: %zu MB)",
            peakMb, s_maxRollingMemoryBytes / (1024 * 1024));
    }

    // Write trace to CSV if there was no valid trace, or if we matched significantly more records than the old baseline
    if (totalMatched > 0 && s_tracePath[0] && (!s_hasValidTrace || totalMatched > s_candidates.size() + 100)) {
        std::ofstream ofs(s_tracePath);
        if (ofs.is_open()) {
            ofs << "seq,pak,offset,in_bytes,out_bytes,decomp_us,load_time_ms\n";
            for (const auto& r : s_traceRecords) {
                double loadTimeMs = (s_freq.QuadPart > 0) ? ((double)r.loadTimestampUs / 1000.0) : 0.0;
                ofs << r.seq << "," << r.pakName << ",0x" << std::hex << std::uppercase << r.offset << std::dec
                    << "," << r.inBytes << "," << r.outBytes << "," << r.decompTimeUs
                    << "," << std::fixed << std::setprecision(2) << loadTimeMs << "\n";
            }
            ofs.close();
            Log("G3O", "[PakTracer] Fresh baseline trace written to '%s' (%u records)", s_tracePath, totalMatched);
        }
    }
    ReleaseSRWLockShared(&s_traceLock);

    std::vector<TraceRecord> sorted = s_traceRecords;

    std::sort(sorted.begin(), sorted.end(), [](const TraceRecord& a, const TraceRecord& b) {
        return a.decompTimeUs > b.decompTimeUs;
    });

    Log("G3O", "[PakTracer] Top 5 Most Expensive Decompressions:");
    for (size_t i = 0; i < sorted.size() && i < 5; ++i) {
        Log("G3O", "   #%2zu: %-24s offset=0x%08llX | %-6u -> %-7u | %.2f ms (at t=%.2f s)",
            i + 1, sorted[i].pakName, sorted[i].offset,
            sorted[i].inBytes, sorted[i].outBytes,
            (double)sorted[i].decompTimeUs / 1000.0,
            (double)sorted[i].loadTimestampUs / 1000000.0);
    }
    Log("G3O", "================================================================================");
}

} // namespace G3O
