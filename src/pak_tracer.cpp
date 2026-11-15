#include "pak_tracer.h"
#include "pak_content_key.h"
#include "blackbox_recorder.h"
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
#include <synchapi.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <algorithm>
#include <atomic>
#include <utility>
#include <stdio.h>

namespace G3O {

// -----------------------------------------------------------------------------
// In-Flight Read Tracking (Expanded to 256 entries with Range Containment)
// -----------------------------------------------------------------------------
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

static constexpr size_t IN_FLIGHT_READ_CAPACITY = 256;
using ThreadReadHistory = RecentReadHistory<InFlightRead, IN_FLIGHT_READ_CAPACITY>;
static thread_local ThreadReadHistory tl_history;
static std::atomic<uint64_t> s_loadGeneration{ 0 };

static bool FindMatchingRead(
    const void* inBuffer,
    DWORD inSize,
    InFlightRead& outMatch,
    uint64_t* outCalculatedOffset = nullptr) {
    uint64_t offsetInArchive = 0;
    InFlightRead* slot = tl_history.FindMostRecentContaining(
        inBuffer, inSize, s_loadGeneration.load(std::memory_order_relaxed), &offsetInArchive);
    if (!slot) return false;
    outMatch = *slot;
    if (outCalculatedOffset) {
        *outCalculatedOffset = offsetInArchive;
    }
    return true;
}

// -----------------------------------------------------------------------------
// In-Memory Session Trace (Zero On-Disk Files)
// -----------------------------------------------------------------------------
struct SessionTraceBlock {
    uint64_t contentKey = 0;
    char pakName[32] = {};
    uint64_t offset = 0;
    uint32_t inBytes = 0;
    uint32_t outBytes = 0;
    uint64_t decompUs = 0;
    uint64_t loadTimestampUs = 0;
};

static SRWLOCK s_sessionTraceLock = SRWLOCK_INIT;
static std::vector<SessionTraceBlock> s_lastCompletedTrace;
static std::unordered_map<std::string, std::vector<SessionTraceBlock>> s_sessionTracesBySave;
static std::vector<SessionTraceBlock> s_activeRecordingTrace;

// Telemetry trace records (in RAM for end-of-load diagnostic summary)
struct TraceRecord {
    uint32_t seq = 0;
    char pakName[32] = {};
    uint64_t offset = 0;
    uint32_t inBytes = 0;
    uint32_t outBytes = 0;
    uint64_t decompTimeUs = 0;
    uint64_t loadTimestampUs = 0;
    uint64_t contentKey = 0;
};

static SRWLOCK s_traceLock = SRWLOCK_INIT;
static std::vector<TraceRecord> s_traceRecords;
static std::atomic<uint32_t> s_seqCounter{ 0 };
static std::atomic<uint32_t> s_matchedCount{ 0 };
static std::atomic<uint32_t> s_unmatchedCount{ 0 };
static std::atomic<uint64_t> s_tracerOverheadUs{ 0 };

static SRWLOCK s_handleLock = SRWLOCK_INIT;
static std::unordered_map<HANDLE, std::string> s_handlePathCache;

// -----------------------------------------------------------------------------
// Pre-Decompression Slot State Machine & Dual-Key Indexing
// -----------------------------------------------------------------------------
enum SlotState : uint32_t {
    SLOT_UNCLAIMED = 0,
    SLOT_IN_FLIGHT  = 1,
    SLOT_READY      = 2,
    SLOT_CONSUMED   = 3,
    SLOT_FAILED     = 4,
    SLOT_SKIPPED    = 5
};

struct PredecompSlot {
    uint32_t seq = 0;
    char pakName[32] = {};
    uint64_t offset = 0;
    uint32_t inBytes = 0;
    uint32_t outBytes = 0;
    uint32_t actualOut = 0;
    uint64_t contentKey = 0;
    uint64_t decompUs = 0;
    bool isPriority = false;

    // Futex synchronization state for WaitOnAddress / WakeByAddressAll
    alignas(4) volatile uint32_t state{ SLOT_UNCLAIMED };
    size_t reservedBytes = 0;
    std::vector<BYTE> data;
};

using SlotPtr = std::unique_ptr<PredecompSlot>;
static std::vector<SlotPtr> s_candidates;

// Dual-Key Indexing: O(1) contentKey + archiveOffset lookups without locks
static std::unordered_map<uint64_t, std::vector<PredecompSlot*>> s_slotsByContentKey;
static std::unordered_map<uint64_t, std::vector<PredecompSlot*>> s_slotsByArchiveOffset;

// Worker pool configuration
static const size_t MAX_WORKER_THREADS = 8;
static HANDLE s_workerThreads[MAX_WORKER_THREADS] = {};
static size_t s_activeWorkerCount = 0;
static size_t s_fastWorkerCount = 0;
static size_t s_heavyWorkerCount = 0;

static std::atomic<size_t> s_fastScanIndex{ 0 };
static std::atomic<size_t> s_heavyScanIndex{ 0 };

// Budget and lifecycle flags
static size_t s_maxRollingMemoryBytes = 384 * 1024 * 1024;
static PakCacheBudget s_cacheBudget;
static std::atomic<size_t> s_rollingMemoryBytes{ 0 };
static std::atomic<size_t> s_priorityMemoryBytes{ 0 };
static std::atomic<size_t> s_peakMemoryBytes{ 0 };

static std::atomic<bool> s_isActive{ false };
static std::atomic<bool> s_prefetchLookupEnabled{ false };
static std::string s_currentSaveName = "Unknown";
static HANDLE s_memAvailableEvent = nullptr;

static std::atomic<uint32_t> s_currentMainSeq{ 0 };
static std::atomic<uint32_t> s_preGameWorldMaxSeq{ 0 };
static std::atomic<uint32_t> s_gotoLoadedPosMinSeq{ 0 };
static std::atomic<bool> s_inGotoLoadedPos{ false };
static std::atomic<bool> s_allowGotoPosPrefetch{ false };
static std::atomic<bool> s_stopAtPreGameWorld{ false };

// Hit / Miss & Futex Telemetry
static std::atomic<uint32_t> s_cacheHits{ 0 };
static std::atomic<uint32_t> s_priorityHits{ 0 };
static std::atomic<uint32_t> s_cacheMisses{ 0 };
static std::atomic<uint32_t> s_inFlightWaits{ 0 };
static std::atomic<uint64_t> s_cacheHitBytes{ 0 };
static std::atomic<uint64_t> s_cacheSavedTimeUs{ 0 };

static LARGE_INTEGER s_loadStartTime = {};
static LARGE_INTEGER s_freq = []() {
    LARGE_INTEGER li;
    QueryPerformanceFrequency(&li);
    if (li.QuadPart <= 0) li.QuadPart = 10000000LL;
    return li;
}();

// -----------------------------------------------------------------------------
// Memory Budget Helpers (Smooth Flow with Evict-on-Consume)
// -----------------------------------------------------------------------------
static void ReleaseCacheMemory(size_t bytes, bool priority) {
    if (bytes == 0) return;
    if (priority) {
        s_priorityMemoryBytes.fetch_sub(bytes, std::memory_order_relaxed);
    } else {
        s_rollingMemoryBytes.fetch_sub(bytes, std::memory_order_relaxed);
    }
    s_cacheBudget.Release(bytes);
    if (s_memAvailableEvent) {
        SetEvent(s_memAvailableEvent);
    }
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

static inline uint64_t MakeCacheKey(const std::string& pakName, uint64_t offset) {
    uint64_t hash = 14695981039346656037ULL;
    for (char c : pakName) {
        hash ^= (uint8_t)tolower((unsigned char)c);
        hash *= 1099511628211ULL;
    }
    hash ^= (offset * 0x9E3779B97F4A7C15ULL);
    return hash;
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

// -----------------------------------------------------------------------------
// Two-Lane Background Worker Pipeline
// -----------------------------------------------------------------------------
static HANDLE GetArchiveHandle(const std::string& pakName, std::unordered_map<std::string, HANDLE>& pakHandles) {
    auto hit = pakHandles.find(pakName);
    if (hit != pakHandles.end()) return hit->second;
    std::string fullPath = "Data\\" + pakName;
    HANDLE h = CreateFileA(fullPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        pakHandles[pakName] = h;
    }
    return h;
}

static void ProcessCandidate(
    PredecompSlot* slot,
    libdeflate_decompressor* dec,
    std::vector<BYTE>& compBuf,
    size_t /*workerId*/,
    std::unordered_map<std::string, HANDLE>& pakHandles
) {
    const bool isVip = slot->isPriority;

    // 1. Skip if main thread has already passed this candidate
    if (slot->seq <= s_currentMainSeq.load(std::memory_order_relaxed)) {
        slot->state = SLOT_SKIPPED;
        WakeByAddressAll((PVOID)&slot->state);
        return;
    }

    // 2. Horizon gate for pre-game world
    if (s_preGameWorldMaxSeq.load(std::memory_order_relaxed) > 0 &&
        slot->seq > s_preGameWorldMaxSeq.load(std::memory_order_relaxed)) {
        while (!s_allowGotoPosPrefetch.load(std::memory_order_acquire) &&
               !s_stopAtPreGameWorld.load(std::memory_order_acquire) &&
               !s_inGotoLoadedPos.load(std::memory_order_acquire) &&
               s_isActive.load(std::memory_order_relaxed)) {
            WaitForSingleObject(s_memAvailableEvent, 5);
        }
        if (!s_isActive.load(std::memory_order_relaxed) ||
            s_stopAtPreGameWorld.load(std::memory_order_acquire)) {
            slot->state = SLOT_SKIPPED;
            WakeByAddressAll((PVOID)&slot->state);
            return;
        }
    }

    if (slot->outBytes > s_cacheBudget.Limit()) {
        slot->state = SLOT_SKIPPED;
        WakeByAddressAll((PVOID)&slot->state);
        return;
    }

    // 3. Smooth budget reservation
    bool reserved = ReserveCacheMemory(slot->outBytes, isVip);
    while (!reserved && s_isActive.load(std::memory_order_relaxed)) {
        if (slot->seq <= s_currentMainSeq.load(std::memory_order_relaxed)) {
            slot->state = SLOT_SKIPPED;
            WakeByAddressAll((PVOID)&slot->state);
            return;
        }
        WaitForSingleObject(s_memAvailableEvent, 5);
        reserved = ReserveCacheMemory(slot->outBytes, isVip);
    }

    if (!s_isActive.load(std::memory_order_relaxed) || !reserved) {
        if (reserved) ReleaseCacheMemory(slot->outBytes, isVip);
        slot->state = SLOT_SKIPPED;
        WakeByAddressAll((PVOID)&slot->state);
        return;
    }

    if (slot->seq <= s_currentMainSeq.load(std::memory_order_relaxed)) {
        ReleaseCacheMemory(slot->outBytes, isVip);
        slot->state = SLOT_SKIPPED;
        WakeByAddressAll((PVOID)&slot->state);
        return;
    }

    // 4. Read compressed data from PAK
    HANDLE hFile = GetArchiveHandle(slot->pakName, pakHandles);
    if (hFile == INVALID_HANDLE_VALUE) {
        ReleaseCacheMemory(slot->outBytes, isVip);
        slot->state = SLOT_FAILED;
        WakeByAddressAll((PVOID)&slot->state);
        return;
    }

    if (compBuf.size() < slot->inBytes) {
        compBuf.resize(slot->inBytes);
    }

    bool readOk = false;
    {
        ScopedInternalPakRead internalRead;
        readOk = ReadExactAt(hFile, slot->offset, compBuf.data(), slot->inBytes);
    }
    if (!readOk) {
        ReleaseCacheMemory(slot->outBytes, isVip);
        slot->state = SLOT_FAILED;
        WakeByAddressAll((PVOID)&slot->state);
        return;
    }

    // 5. Calculate contentKey if missing
    if (slot->contentKey == 0) {
        slot->contentKey = ComputeBlockContentKey(compBuf.data(), slot->inBytes, slot->outBytes);
    }

    // 6. Decompress into slot buffer
    slot->data.resize(slot->outBytes);
    slot->reservedBytes = slot->outBytes;

    size_t actualOut = 0;
    int res = -1;
    if (G3O_AVX2_IsSupported()) {
        res = G3O_AVX2_DecompressZlib(dec, compBuf.data(), slot->inBytes, slot->data.data(), slot->outBytes, &actualOut);
    } else {
        res = libdeflate_zlib_decompress(dec, compBuf.data(), slot->inBytes, slot->data.data(), slot->outBytes, &actualOut);
    }

    if (res == 0 && actualOut > 0) {
        slot->actualOut = static_cast<uint32_t>(actualOut);
        slot->state = SLOT_READY;
        WakeByAddressAll((PVOID)&slot->state); // Signal waiting main thread!
    } else {
        ReleaseCacheMemory(slot->reservedBytes, isVip);
        slot->reservedBytes = 0;
        std::vector<BYTE>().swap(slot->data);
        slot->state = SLOT_FAILED;
        WakeByAddressAll((PVOID)&slot->state);
    }
}

static DWORD WINAPI BgPredecompWorker(LPVOID lpParam) {
    const size_t workerId = reinterpret_cast<size_t>(lpParam);
    const bool isFastLane = (workerId < s_fastWorkerCount);

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    libdeflate_decompressor* dec = nullptr;
    if (G3O_AVX2_IsSupported()) {
        dec = G3O_AVX2_AllocDecompressor();
    } else {
        dec = libdeflate_alloc_decompressor();
    }
    if (!dec) return 1;

    std::unordered_map<std::string, HANDLE> pakHandles;
    std::vector<BYTE> compBuf;
    size_t processedCount = 0;

    while (s_isActive.load(std::memory_order_relaxed)) {
        size_t candIdx = 0;
        bool found = false;

        if (isFastLane) {
            // Fast lane: Scans for small/medium blocks (<= 256 KB)
            while (s_isActive.load(std::memory_order_relaxed)) {
                size_t i = s_fastScanIndex.fetch_add(1, std::memory_order_relaxed);
                if (i >= s_candidates.size()) {
                    // Fast queue exhausted; steal from heavy queue
                    i = s_heavyScanIndex.fetch_add(1, std::memory_order_relaxed);
                    if (i >= s_candidates.size()) break;
                }
                PredecompSlot* slot = s_candidates[i].get();
                if (!slot) continue;

                // If block is large and heavy workers exist, leave it to heavy lane
                if (slot->outBytes > 256 * 1024 && s_heavyWorkerCount > 0 && i < s_candidates.size()) {
                    continue;
                }

                uint32_t expected = SLOT_UNCLAIMED;
                if (InterlockedCompareExchange((volatile LONG*)&slot->state, SLOT_IN_FLIGHT, expected) == (LONG)expected) {
                    candIdx = i;
                    found = true;
                    break;
                }
            }
        } else {
            // Heavy lane: Scans for large texture/mesh blocks (> 256 KB)
            while (s_isActive.load(std::memory_order_relaxed)) {
                size_t i = s_heavyScanIndex.fetch_add(1, std::memory_order_relaxed);
                if (i >= s_candidates.size()) {
                    // Heavy queue exhausted; steal from fast queue
                    i = s_fastScanIndex.fetch_add(1, std::memory_order_relaxed);
                    if (i >= s_candidates.size()) break;
                }
                PredecompSlot* slot = s_candidates[i].get();
                if (!slot) continue;

                // If block is small and fast workers exist, leave it to fast lane
                if (slot->outBytes <= 256 * 1024 && s_fastWorkerCount > 0 && i < s_candidates.size()) {
                    continue;
                }

                uint32_t expected = SLOT_UNCLAIMED;
                if (InterlockedCompareExchange((volatile LONG*)&slot->state, SLOT_IN_FLIGHT, expected) == (LONG)expected) {
                    candIdx = i;
                    found = true;
                    break;
                }
            }
        }

        if (!found) break;

        PredecompSlot* slot = s_candidates[candIdx].get();
        ProcessCandidate(slot, dec, compBuf, workerId, pakHandles);
        processedCount++;
    }

    // Clean up open file handles
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

    return 0;
}

// -----------------------------------------------------------------------------
// Telemetry and Trace Recording in RAM
// -----------------------------------------------------------------------------
static void RecordTraceEntry(
    const char* pakName,
    uint64_t offset,
    uint32_t inBytes,
    uint32_t outBytes,
    uint64_t decompUs,
    uint64_t loadTimestampUs,
    uint64_t contentKey
) {
    TraceRecord rec;
    rec.seq = s_seqCounter.fetch_add(1, std::memory_order_relaxed) + 1;
    strncpy_s(rec.pakName, sizeof(rec.pakName), pakName ? pakName : "", _TRUNCATE);
    rec.offset = offset;
    rec.inBytes = inBytes;
    rec.outBytes = outBytes;
    rec.decompTimeUs = decompUs;
    rec.loadTimestampUs = loadTimestampUs;
    rec.contentKey = contentKey;

    AcquireSRWLockExclusive(&s_traceLock);
    s_traceRecords.push_back(rec);
    ReleaseSRWLockExclusive(&s_traceLock);

    // Save candidate-sized blocks for in-memory session trace replay
    if (outBytes >= 2048 && outBytes <= 128 * 1024 * 1024 && pakName && pakName[0]) {
        SessionTraceBlock st;
        st.contentKey = contentKey;
        strncpy_s(st.pakName, sizeof(st.pakName), pakName, _TRUNCATE);
        st.offset = offset;
        st.inBytes = inBytes;
        st.outBytes = outBytes;
        st.decompUs = decompUs;
        st.loadTimestampUs = loadTimestampUs;

        AcquireSRWLockExclusive(&s_sessionTraceLock);
        s_activeRecordingTrace.push_back(st);
        ReleaseSRWLockExclusive(&s_sessionTraceLock);
    }
}

// -----------------------------------------------------------------------------
// O(1) Candidate Lookup
// -----------------------------------------------------------------------------
static PredecompSlot* FindCandidate(uint64_t contentKey, uint64_t archiveOffsetKey) {
    if (s_candidates.empty()) return nullptr;

    // 1. Primary lookup: SSE4.2 CRC32C content key
    if (contentKey != 0) {
        auto it = s_slotsByContentKey.find(contentKey);
        if (it != s_slotsByContentKey.end()) {
            for (PredecompSlot* slot : it->second) {
                const uint32_t st = slot->state;
                if (st != SLOT_CONSUMED && st != SLOT_FAILED && st != SLOT_SKIPPED) {
                    return slot;
                }
            }
        }
    }

    // 2. Secondary lookup: Archive + calculated file offset
    if (archiveOffsetKey != 0) {
        auto it = s_slotsByArchiveOffset.find(archiveOffsetKey);
        if (it != s_slotsByArchiveOffset.end()) {
            for (PredecompSlot* slot : it->second) {
                const uint32_t st = slot->state;
                if (st != SLOT_CONSUMED && st != SLOT_FAILED && st != SLOT_SKIPPED) {
                    return slot;
                }
            }
        }
    }

    return nullptr;
}

// -----------------------------------------------------------------------------
// Lifecycle API Implementation
// -----------------------------------------------------------------------------
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
        Log("G3O", "[PakCache] Previous worker shutdown incomplete; skipping prefetch.");
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

    AcquireSRWLockExclusive(&s_sessionTraceLock);
    s_activeRecordingTrace.clear();
    s_activeRecordingTrace.reserve(8192);
    ReleaseSRWLockExclusive(&s_sessionTraceLock);

    s_seqCounter.store(0, std::memory_order_relaxed);
    s_matchedCount.store(0, std::memory_order_relaxed);
    s_unmatchedCount.store(0, std::memory_order_relaxed);
    s_tracerOverheadUs.store(0, std::memory_order_relaxed);

    s_cacheHits.store(0, std::memory_order_relaxed);
    s_priorityHits.store(0, std::memory_order_relaxed);
    s_cacheMisses.store(0, std::memory_order_relaxed);
    s_inFlightWaits.store(0, std::memory_order_relaxed);
    s_cacheHitBytes.store(0, std::memory_order_relaxed);
    s_cacheSavedTimeUs.store(0, std::memory_order_relaxed);

    s_rollingMemoryBytes.store(0, std::memory_order_relaxed);
    s_priorityMemoryBytes.store(0, std::memory_order_relaxed);
    s_peakMemoryBytes.store(0, std::memory_order_relaxed);

    s_currentMainSeq.store(0, std::memory_order_relaxed);
    s_preGameWorldMaxSeq.store(0, std::memory_order_relaxed);
    s_gotoLoadedPosMinSeq.store(0, std::memory_order_relaxed);
    s_inGotoLoadedPos.store(false, std::memory_order_relaxed);
    s_allowGotoPosPrefetch.store(false, std::memory_order_relaxed);
    s_stopAtPreGameWorld.store(false, std::memory_order_relaxed);

    s_candidates.clear();
    s_slotsByContentKey.clear();
    s_slotsByArchiveOffset.clear();
    s_fastScanIndex.store(0, std::memory_order_relaxed);
    s_heavyScanIndex.store(0, std::memory_order_relaxed);

    tl_history.Clear();

    if (!s_memAvailableEvent) {
        s_memAvailableEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
    }

    // Configure cache budget
    const Config& cfg = GetConfig();
    s_maxRollingMemoryBytes = static_cast<size_t>(cfg.pakCacheBudgetMB) * 1024 * 1024;
    s_cacheBudget.Configure(s_maxRollingMemoryBytes);

    // Check for In-Memory Session Trace (Load #2+ / Replay)
    std::vector<SessionTraceBlock> replayTrace;
    {
        AcquireSRWLockShared(&s_sessionTraceLock);
        auto it = s_sessionTracesBySave.find(s_currentSaveName);
        if (it != s_sessionTracesBySave.end() && !it->second.empty()) {
            replayTrace = it->second;
        } else if (!s_lastCompletedTrace.empty()) {
            // Replay from last completed session trace (shared world assets)
            replayTrace = s_lastCompletedTrace;
        }
        ReleaseSRWLockShared(&s_sessionTraceLock);
    }

    size_t requestedWorkerCount = 0;
    if (!replayTrace.empty() && s_maxRollingMemoryBytes > 0 && s_memAvailableEvent) {
        s_candidates.reserve(replayTrace.size());

        double prevLoadMs = 0.0;
        uint32_t prevSeq = 0;
        uint32_t seqCounter = 1;
        size_t vipCount = 0;

        for (const auto& blk : replayTrace) {
            auto slot = std::make_unique<PredecompSlot>();
            slot->seq = seqCounter++;
            strncpy_s(slot->pakName, sizeof(slot->pakName), blk.pakName, _TRUNCATE);
            slot->offset = blk.offset;
            slot->inBytes = blk.inBytes;
            slot->outBytes = blk.outBytes;
            slot->contentKey = blk.contentKey;
            slot->decompUs = blk.decompUs;
            slot->isPriority = (blk.outBytes >= 50 * 1024 * 1024);
            slot->state = SLOT_UNCLAIMED;

            if (slot->isPriority) vipCount++;

            double loadMs = (s_freq.QuadPart > 0) ? (static_cast<double>(blk.loadTimestampUs) / 1000.0) : 0.0;
            if (prevSeq > 0 && s_preGameWorldMaxSeq.load(std::memory_order_relaxed) == 0 &&
                (loadMs - prevLoadMs) >= 1500.0 && prevLoadMs >= 5000.0) {
                s_preGameWorldMaxSeq.store(prevSeq, std::memory_order_relaxed);
                s_gotoLoadedPosMinSeq.store(slot->seq, std::memory_order_relaxed);
            }
            prevLoadMs = loadMs;
            prevSeq = slot->seq;

            PredecompSlot* rawPtr = slot.get();
            if (slot->contentKey != 0) {
                s_slotsByContentKey[slot->contentKey].push_back(rawPtr);
            }
            const uint64_t archKey = MakeCacheKey(slot->pakName, slot->offset);
            s_slotsByArchiveOffset[archKey].push_back(rawPtr);

            s_candidates.push_back(std::move(slot));
        }

        // Determine worker allocation (Two-Lane pool: 4 Fast / 2 Heavy or proportional)
        int effectiveCores = cfg.cpuCores;
        if (effectiveCores <= 0) {
            SYSTEM_INFO si = {};
            GetSystemInfo(&si);
            effectiveCores = (si.dwNumberOfProcessors >= 2) ? static_cast<int>(si.dwNumberOfProcessors / 2) : 1;
        }

        size_t numWorkers = 4;
        if (cfg.pakPrefetchWorkers > 0) {
            numWorkers = static_cast<size_t>(cfg.pakPrefetchWorkers);
        } else if (effectiveCores >= 8) {
            numWorkers = 6;
        } else if (effectiveCores >= 6) {
            numWorkers = 4;
        } else if (effectiveCores >= 4) {
            numWorkers = 3;
        } else {
            numWorkers = 2;
        }
        if (numWorkers > MAX_WORKER_THREADS) numWorkers = MAX_WORKER_THREADS;

        if (numWorkers >= 4) {
            s_fastWorkerCount = numWorkers - 2;
            s_heavyWorkerCount = 2;
        } else if (numWorkers == 3) {
            s_fastWorkerCount = 2;
            s_heavyWorkerCount = 1;
        } else {
            s_fastWorkerCount = 1;
            s_heavyWorkerCount = (numWorkers > 1) ? 1 : 0;
        }

        requestedWorkerCount = numWorkers;
        Log("G3O", "[PakCache] In-Memory Trace active for '%s': %zu candidates queued (%zu VIP). Starting Two-Lane pool (%zu Fast + %zu Heavy workers), budget %zu MB.",
            s_currentSaveName.c_str(), s_candidates.size(), vipCount, s_fastWorkerCount, s_heavyWorkerCount, s_maxRollingMemoryBytes / (1024 * 1024));
    } else {
        Log("G3O", "[PakCache] First run or cold start for '%s': recording fresh session trace in RAM.", s_currentSaveName.c_str());
    }

    s_isActive.store(true, std::memory_order_release);
    for (size_t w = 0; w < requestedWorkerCount; ++w) {
        HANDLE thread = CreateThread(nullptr, 0, BgPredecompWorker, reinterpret_cast<LPVOID>(w), 0, nullptr);
        if (!thread) {
            Log("G3O", "[PakCache] Worker %zu failed to create (error %lu).", w, GetLastError());
            continue;
        }
        s_workerThreads[s_activeWorkerCount++] = thread;
    }

    if (s_activeWorkerCount > 0) {
        s_prefetchLookupEnabled.store(true, std::memory_order_release);
    }
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

    const uint64_t fileOffset = ((uint64_t)ov->OffsetHigh << 32) | (uint64_t)ov->Offset;
    Blackbox_BeginOp(BB_OP_READING_PAK, archive.c_str(), fileOffset, bytesRead);

    InFlightRead read;
    read.hFile = hFile;
    read.offset = fileOffset;
    read.buffer = buffer;
    read.bytesRead = bytesRead;
    read.loadGeneration = s_loadGeneration.load(std::memory_order_relaxed);
    read.timestampUs = (s_freq.QuadPart > 0) ? ((t0.QuadPart - s_loadStartTime.QuadPart) * 1000000LL / s_freq.QuadPart) : 0;
    read.archiveName = std::move(archive);
    read.valid = true;

    tl_history.Push(std::move(read));
    Blackbox_EndOp();

    QueryPerformanceCounter(&t1);
    uint64_t oh = (s_freq.QuadPart > 0) ? ((t1.QuadPart - t0.QuadPart) * 1000000LL / s_freq.QuadPart) : 0;
    s_tracerOverheadUs.fetch_add(oh, std::memory_order_relaxed);
}

void PakTracer_OnWorldActivateStart() {
    if (!s_isActive.load(std::memory_order_relaxed)) return;
    s_allowGotoPosPrefetch.store(true, std::memory_order_release);
    if (s_memAvailableEvent) SetEvent(s_memAvailableEvent);
}

void PakTracer_OnLoadGameWorldStart(bool worldAlreadyActive) {
    if (!s_isActive.load(std::memory_order_relaxed)) return;
    if (worldAlreadyActive && !s_allowGotoPosPrefetch.load(std::memory_order_acquire)) {
        s_stopAtPreGameWorld.store(true, std::memory_order_release);
    } else {
        s_inGotoLoadedPos.store(true, std::memory_order_release);
    }
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
    if (!inBuffer || inSize < 6 || !outBuffer || outCapacity == 0) return false;

    // 1. Hardware SSE4.2 content key
    const uint64_t contentKey = ComputeBlockContentKey(inBuffer, inSize, outCapacity);

    // 2. Correlate with recent ReadFile to get archive and calculated file offset
    InFlightRead read;
    uint64_t calculatedOffset = 0;
    uint64_t archiveOffsetKey = 0;
    const bool hasReadMatch = FindMatchingRead(inBuffer, inSize, read, &calculatedOffset);
    if (hasReadMatch && !read.archiveName.empty()) {
        archiveOffsetKey = MakeCacheKey(read.archiveName, calculatedOffset);
    }

    // 3. Fast lookup
    PredecompSlot* slot = FindCandidate(contentKey, archiveOffsetKey);
    if (!slot) {
        s_cacheMisses.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // 4. Resolve IN_FLIGHT state with WaitOnAddress (Zero-Stall Futex)
    if (slot->state == SLOT_IN_FLIGHT) {
        uint32_t expected = SLOT_IN_FLIGHT;
        for (int retry = 0; retry < 5 && slot->state == SLOT_IN_FLIGHT; ++retry) {
            WaitOnAddress((PVOID)&slot->state, &expected, sizeof(uint32_t), 4);
        }
        s_inFlightWaits.fetch_add(1, std::memory_order_relaxed);
    }

    // 5. Check if slot is ready
    if (slot->state == SLOT_READY) {
        if (outCapacity < slot->actualOut) {
            s_cacheMisses.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        // Fast lock-free memcpy into engine buffer
        std::memcpy(outBuffer, slot->data.data(), slot->actualOut);
        if (pActualOut) {
            *pActualOut = slot->actualOut;
        }

        // EVICT-ON-CONSUME: Free memory and budget IMMEDIATELY
        slot->state = SLOT_CONSUMED;
        const size_t freedBytes = slot->reservedBytes;
        slot->reservedBytes = 0;
        std::vector<BYTE>().swap(slot->data);
        ReleaseCacheMemory(freedBytes, slot->isPriority);

        // Telemetry
        s_cacheHits.fetch_add(1, std::memory_order_relaxed);
        if (slot->isPriority) s_priorityHits.fetch_add(1, std::memory_order_relaxed);
        s_cacheHitBytes.fetch_add(slot->actualOut, std::memory_order_relaxed);
        s_cacheSavedTimeUs.fetch_add(slot->decompUs, std::memory_order_relaxed);

        // Advance sequence
        uint32_t targetSeq = slot->seq;
        uint32_t prevSeq = s_currentMainSeq.load(std::memory_order_relaxed);
        while (targetSeq > prevSeq && !s_currentMainSeq.compare_exchange_weak(
                   prevSeq, targetSeq, std::memory_order_relaxed)) {}

        // Record trace record in RAM
        RecordTraceEntry(slot->pakName, slot->offset, inSize, slot->actualOut, 50, read.timestampUs, contentKey);

        return true;
    }

    if (slot->state == SLOT_UNCLAIMED) {
        // Main thread beat the workers to this candidate.
        // Mark as consumed so workers skip it and don't do redundant work.
        uint32_t exp = SLOT_UNCLAIMED;
        InterlockedCompareExchange((volatile LONG*)&slot->state, SLOT_CONSUMED, exp);
    }

    s_cacheMisses.fetch_add(1, std::memory_order_relaxed);
    return false;
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

    const uint64_t contentKey = ComputeBlockContentKey(inBuffer, inSize, outCapacity);

    InFlightRead read;
    uint64_t calculatedOffset = 0;
    if (FindMatchingRead(inBuffer, inSize, read, &calculatedOffset)) {
        Blackbox_BeginOp(BB_OP_DECOMPRESSING_PAK, read.archiveName.c_str(), calculatedOffset, inSize);

        RecordTraceEntry(
            read.archiveName.c_str(),
            calculatedOffset,
            inSize,
            actualOut ? actualOut : outCapacity,
            decompTimeUs,
            read.timestampUs,
            contentKey
        );

        s_matchedCount.fetch_add(1, std::memory_order_relaxed);
    } else {
        RecordTraceEntry(
            "",
            0,
            inSize,
            actualOut ? actualOut : outCapacity,
            decompTimeUs,
            0,
            contentKey
        );
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

    // Wake any workers waiting on slot states
    for (const auto& slot : s_candidates) {
        if (slot) {
            WakeByAddressAll((PVOID)&slot->state);
        }
    }

    // Join all background worker threads
    bool allWorkersStopped = true;
    for (size_t w = 0; w < s_activeWorkerCount; ++w) {
        if (!s_workerThreads[w]) continue;
        const DWORD wait = WaitForSingleObject(s_workerThreads[w], 2000);
        if (wait != WAIT_OBJECT_0) {
            Log("G3O", "[PakCache] Worker %zu did not join cleanly (wait=%lu, error=%lu).",
                w, wait, GetLastError());
            allWorkersStopped = false;
        }
        CloseHandle(s_workerThreads[w]);
        s_workerThreads[w] = nullptr;
    }
    s_activeWorkerCount = 0;

    if (s_memAvailableEvent) {
        CloseHandle(s_memAvailableEvent);
        s_memAvailableEvent = nullptr;
    }

    // Save completed in-memory trace for instant replay on subsequent loads
    AcquireSRWLockExclusive(&s_sessionTraceLock);
    if (!s_activeRecordingTrace.empty()) {
        s_lastCompletedTrace = s_activeRecordingTrace;
        if (!s_currentSaveName.empty()) {
            s_sessionTracesBySave[s_currentSaveName] = s_activeRecordingTrace;
        }
        Log("G3O", "[PakCache] In-Memory Session Trace stored: %zu blocks ready for instant replay.",
            s_activeRecordingTrace.size());
    }
    ReleaseSRWLockExclusive(&s_sessionTraceLock);

    // Free candidate slots
    s_candidates.clear();
    s_slotsByContentKey.clear();
    s_slotsByArchiveOffset.clear();

    s_cacheBudget.Configure(0);
    s_rollingMemoryBytes.store(0, std::memory_order_relaxed);
    s_priorityMemoryBytes.store(0, std::memory_order_relaxed);

    // Telemetry Report
    AcquireSRWLockShared(&s_traceLock);
    uint32_t matched = s_matchedCount.load(std::memory_order_relaxed);
    uint32_t unmatched = s_unmatchedCount.load(std::memory_order_relaxed);
    uint32_t totalDecomps = matched + unmatched;
    double matchRate = (totalDecomps > 0) ? (100.0 * (double)matched / (double)totalDecomps) : 0.0;
    double overheadMs = (double)s_tracerOverheadUs.load(std::memory_order_relaxed) / 1000.0;

    uint32_t hits = s_cacheHits.load(std::memory_order_relaxed);
    uint32_t prioHits = s_priorityHits.load(std::memory_order_relaxed);
    uint32_t misses = s_cacheMisses.load(std::memory_order_relaxed);
    uint32_t inFlightWaits = s_inFlightWaits.load(std::memory_order_relaxed);
    double hitMb = (double)s_cacheHitBytes.load(std::memory_order_relaxed) / (1024.0 * 1024.0);
    double savedSec = (double)s_cacheSavedTimeUs.load(std::memory_order_relaxed) / 1000000.0;
    double peakMb = (double)s_peakMemoryBytes.load(std::memory_order_relaxed) / (1024.0 * 1024.0);

    Log("G3O", "================================================================================");
    Log("G3O", "[PakTracer] === IN-MEMORY LOAD TRACE REPORT: '%s' ===", s_currentSaveName.c_str());
    Log("G3O", "  Total decompressions: %u", totalDecomps);
    Log("G3O", "  Matched to PAK reads: %u (%.1f%%)", matched, matchRate);
    Log("G3O", "  Unmatched / Other:    %u (%.1f%%)", unmatched, 100.0 - matchRate);
    Log("G3O", "  Tracer overhead:      %.2f ms", overheadMs);

    if (hits + misses > 0) {
        double hitPct = 100.0 * (double)hits / (double)(hits + misses);
        Log("G3O", "[PakCache] === TWO-LANE PRE-DECOMPRESSION CACHE PERFORMANCE REPORT ===");
        Log("G3O", "  Cache HITS:               %u (%.1f%%)", hits, hitPct);
        Log("G3O", "  VIP Priority HITS:        %u", prioHits);
        Log("G3O", "  In-Flight Futex Waits:    %u (resolved in background without stall)", inFlightWaits);
        Log("G3O", "  Cache MISSES:             %u (%.1f%%)", misses, 100.0 - hitPct);
        Log("G3O", "  Data Served from Cache:   %.2f MB", hitMb);
        Log("G3O", "  Estimated CPU Time Saved: %.2f s", savedSec);
        Log("G3O", "  Peak Cache Memory Used:   %.2f MB (Budget limit: %zu MB)",
            peakMb, s_maxRollingMemoryBytes / (1024 * 1024));
    }

    std::vector<TraceRecord> sorted = s_traceRecords;
    ReleaseSRWLockShared(&s_traceLock);

    std::sort(sorted.begin(), sorted.end(), [](const TraceRecord& a, const TraceRecord& b) {
        return a.decompTimeUs > b.decompTimeUs;
    });

    Log("G3O", "[PakTracer] Top 5 Most Expensive Decompressions:");
    for (size_t i = 0; i < sorted.size() && i < 5; ++i) {
        Log("G3O", "   #%2zu: %-24s offset=0x%08llX | %-6u -> %-7u | %.2f ms",
            i + 1, sorted[i].pakName, sorted[i].offset,
            sorted[i].inBytes, sorted[i].outBytes,
            (double)sorted[i].decompTimeUs / 1000.0);
    }
    Log("G3O", "================================================================================");
}

} // namespace G3O
