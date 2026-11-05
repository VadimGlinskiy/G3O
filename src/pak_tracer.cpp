#include "pak_tracer.h"
#include "diagnostic_logger.h"
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
    bool valid = false;
};

static thread_local InFlightRead tl_lastRead;

static std::atomic<bool> s_isActive{ false };
static std::string s_currentSaveName = "Unknown";
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
// Step 2.3: Bounded Speculative Pre-Decompression Cache (Sliding Window)
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
    uint32_t outBytes = 0;
    uint64_t savedUs = 0;
    std::vector<BYTE> data;
};

static const size_t MAX_CACHE_MEMORY_BYTES = 128 * 1024 * 1024; // 128 MB strict budget
static std::atomic<size_t> s_currentMemoryBytes{ 0 };
static std::atomic<size_t> s_peakMemoryBytes{ 0 };
static std::atomic<uint32_t> s_currentMainSeq{ 0 };

static SRWLOCK s_cacheLock = SRWLOCK_INIT;
static std::unordered_map<uint64_t, std::shared_ptr<CachedBlock>> s_predecompCache;

static std::vector<PrefetchCandidate> s_candidates;
static HANDLE s_bgWorkerThread = nullptr;
static HANDLE s_memAvailableEvent = nullptr;
static bool s_hasValidTrace = false;

static std::atomic<uint32_t> s_cacheHits{ 0 };
static std::atomic<uint32_t> s_cacheMisses{ 0 };
static std::atomic<uint64_t> s_cacheHitBytes{ 0 };
static std::atomic<uint64_t> s_cacheSavedTimeUs{ 0 };

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
            s_currentMemoryBytes.fetch_sub(it->second->outBytes, std::memory_order_relaxed);
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
static DWORD WINAPI BgPredecompWorker(LPVOID /*lpParam*/) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    libdeflate_decompressor* dec = nullptr;
    if (G3O_AVX2_IsSupported()) {
        dec = G3O_AVX2_AllocDecompressor();
    } else {
        dec = libdeflate_alloc_decompressor();
    }
    if (!dec) return 1;

    // Cache open PAK handles to eliminate repeated CreateFile/CloseHandle syscalls
    std::unordered_map<std::string, HANDLE> pakHandles;

    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    size_t decompressedCount = 0;

    for (size_t i = 0; i < s_candidates.size(); ++i) {
        if (!s_isActive.load(std::memory_order_relaxed)) break;

        const auto& cand = s_candidates[i];

        // Skip immediately if main thread has already passed this candidate
        if (cand.seq <= s_currentMainSeq.load(std::memory_order_relaxed)) {
            continue;
        }

        // Memory budget check: wait if adding this block exceeds budget
        while (s_currentMemoryBytes.load(std::memory_order_relaxed) + cand.outBytes > MAX_CACHE_MEMORY_BYTES &&
               s_currentMemoryBytes.load(std::memory_order_relaxed) > 0) {
            if (!s_isActive.load(std::memory_order_relaxed)) break;

            uint32_t curSeq = s_currentMainSeq.load(std::memory_order_relaxed);
            if (cand.seq <= curSeq) {
                break; // Stale candidate, skip
            }

            AcquireSRWLockExclusive(&s_cacheLock);
            PurgeStaleBlocksUnderLock(curSeq);
            ReleaseSRWLockExclusive(&s_cacheLock);

            WaitForSingleObject(s_memAvailableEvent, 5);
        }
        if (!s_isActive.load(std::memory_order_relaxed)) break;
        if (cand.seq <= s_currentMainSeq.load(std::memory_order_relaxed)) {
            continue;
        }

        // Get or open archive handle
        HANDLE hFile = INVALID_HANDLE_VALUE;
        auto hit = pakHandles.find(cand.pakName);
        if (hit != pakHandles.end()) {
            hFile = hit->second;
        } else {
            std::string fullPath = "Data\\" + cand.pakName;
            hFile = CreateFileA(fullPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hFile != INVALID_HANDLE_VALUE) {
                pakHandles[cand.pakName] = hFile;
            }
        }
        if (hFile == INVALID_HANDLE_VALUE) continue;

        // Read compressed chunk
        std::vector<BYTE> compBuf(cand.inBytes);
        OVERLAPPED ov = { 0 };
        ov.Offset = (DWORD)cand.offset;
        ov.OffsetHigh = (DWORD)(cand.offset >> 32);

        DWORD bytesRead = 0;
        if (!ReadFile(hFile, compBuf.data(), cand.inBytes, &bytesRead, &ov) || bytesRead != cand.inBytes) {
            continue;
        }

        // Allocate block buffer
        auto block = std::make_shared<CachedBlock>();
        block->seq = cand.seq;
        block->pakName = cand.pakName;
        block->offset = cand.offset;
        block->outBytes = cand.outBytes;
        block->savedUs = cand.decompUs;
        block->data.resize(cand.outBytes);

        size_t actualOut = 0;
        int res = -1;
        if (G3O_AVX2_IsSupported()) {
            res = G3O_AVX2_DecompressZlib(dec, compBuf.data(), cand.inBytes, block->data.data(), cand.outBytes, &actualOut);
        } else {
            res = libdeflate_zlib_decompress(dec, compBuf.data(), cand.inBytes, block->data.data(), cand.outBytes, &actualOut);
        }

        if (res == 0 && actualOut > 0) {
            if (actualOut != cand.outBytes) {
                block->outBytes = (uint32_t)actualOut;
                block->data.resize(actualOut);
            }

            uint64_t key = MakeCacheKey(block->pakName, block->offset);

            // Track memory usage
            size_t curMem = s_currentMemoryBytes.fetch_add(block->outBytes, std::memory_order_relaxed) + block->outBytes;
            size_t peak = s_peakMemoryBytes.load(std::memory_order_relaxed);
            while (curMem > peak && !s_peakMemoryBytes.compare_exchange_weak(peak, curMem, std::memory_order_relaxed)) {}

            // Insert into lookup map
            AcquireSRWLockExclusive(&s_cacheLock);
            s_predecompCache[key] = block;
            ReleaseSRWLockExclusive(&s_cacheLock);

            decompressedCount++;
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
    Log("G3O", "[PakCache] Background worker completed %zu pre-decompressions in %.2f ms", decompressedCount, elapsedMs);
    return 0;
}

bool PakTracer_IsActive() {
    return s_isActive.load(std::memory_order_relaxed);
}

void PakTracer_OnLoadStart(const char* saveName) {
    s_currentSaveName = (saveName && *saveName) ? saveName : "Savegame";
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
    s_cacheMisses.store(0, std::memory_order_relaxed);
    s_cacheHitBytes.store(0, std::memory_order_relaxed);
    s_cacheSavedTimeUs.store(0, std::memory_order_relaxed);
    s_currentMemoryBytes.store(0, std::memory_order_relaxed);
    s_peakMemoryBytes.store(0, std::memory_order_relaxed);
    s_currentMainSeq.store(0, std::memory_order_relaxed);

    s_candidates.clear();
    tl_lastRead.valid = false;

    if (!s_memAvailableEvent) {
        s_memAvailableEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
    }

    std::string csvPath = "G3O_PakTrace_" + s_currentSaveName + ".csv";
    s_hasValidTrace = IsTraceValid(csvPath, s_currentSaveName);

    if (s_hasValidTrace) {
        std::ifstream ifs(csvPath);
        if (ifs.is_open()) {
            std::string line;
            std::getline(ifs, line); // header
            while (std::getline(ifs, line)) {
                std::stringstream ss(line);
                std::string item;
                std::vector<std::string> cols;
                while (std::getline(ss, item, ',')) cols.push_back(item);
                if (cols.size() >= 7) {
                    double t_ms = std::stod(cols[6]);
                    uint64_t decomp_us = std::stoull(cols[5]);
                    uint32_t outSz = std::stoul(cols[4]);
                    // Prefetch candidates: GotoLoadedPosition & Start blocks taking >= 500 us
                    if (t_ms >= 10000.0 && decomp_us >= 500 && outSz <= 96 * 1024 * 1024 && outSz > 1024) {
                        PrefetchCandidate cand;
                        cand.seq = std::stoul(cols[0]);
                        cand.pakName = cols[1];
                        cand.offset = std::stoull(cols[2], nullptr, 16);
                        cand.inBytes = std::stoul(cols[3]);
                        cand.outBytes = outSz;
                        cand.decompUs = decomp_us;
                        s_candidates.push_back(cand);
                    }
                }
            }
            ifs.close();
        }

        if (!s_candidates.empty()) {
            Log("G3O", "[PakCache] Loaded %zu prefetch candidates (budget: 128 MB) for savegame '%s'",
                s_candidates.size(), s_currentSaveName.c_str());
            s_bgWorkerThread = CreateThread(nullptr, 0, BgPredecompWorker, nullptr, 0, nullptr);
        }
    } else {
        Log("G3O", "[PakTracer] No valid trace for '%s' (savegame newer or first run) - recording fresh trace",
            s_currentSaveName.c_str());
    }

    s_isActive.store(true, std::memory_order_release);
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
    if (!ov || !buffer || bytesRead == 0 || bytesRead != bytesRequested) {
        tl_lastRead.valid = false;
        return;
    }

    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);

    std::string archive = ResolveArchiveName(hFile);
    if (archive.empty()) {
        tl_lastRead.valid = false;
        return;
    }

    tl_lastRead.hFile = hFile;
    tl_lastRead.offset = ((uint64_t)ov->OffsetHigh << 32) | (uint64_t)ov->Offset;
    tl_lastRead.buffer = buffer;
    tl_lastRead.bytesRead = bytesRead;
    tl_lastRead.timestampUs = (s_freq.QuadPart > 0) ? ((t0.QuadPart - s_loadStartTime.QuadPart) * 1000000LL / s_freq.QuadPart) : 0;
    tl_lastRead.valid = true;

    QueryPerformanceCounter(&t1);
    uint64_t oh = (s_freq.QuadPart > 0) ? ((t1.QuadPart - t0.QuadPart) * 1000000LL / s_freq.QuadPart) : 0;
    s_tracerOverheadUs.fetch_add(oh, std::memory_order_relaxed);
}

bool PakTracer_TryGetPredecompressed(
    const void* inBuffer,
    DWORD inSize,
    void* outBuffer,
    DWORD outCapacity,
    DWORD* pActualOut
) {
    if (!s_isActive.load(std::memory_order_relaxed)) return false;
    if (!tl_lastRead.valid || tl_lastRead.buffer != inBuffer || tl_lastRead.bytesRead != inSize) {
        return false;
    }

    std::string archive = ResolveArchiveName(tl_lastRead.hFile);
    if (archive.empty()) return false;

    uint64_t key = MakeCacheKey(archive, tl_lastRead.offset);

    std::shared_ptr<CachedBlock> block;
    AcquireSRWLockExclusive(&s_cacheLock);
    auto it = s_predecompCache.find(key);
    if (it != s_predecompCache.end()) {
        block = it->second;
        s_predecompCache.erase(it);
    }
    ReleaseSRWLockExclusive(&s_cacheLock);

    if (!block) {
        s_cacheMisses.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    if (outCapacity < block->outBytes) {
        s_cacheMisses.fetch_add(1, std::memory_order_relaxed);
        s_currentMemoryBytes.fetch_sub(block->outBytes, std::memory_order_relaxed);
        if (s_memAvailableEvent) SetEvent(s_memAvailableEvent);
        return false;
    }

    // Fast memcpy directly into engine buffer!
    memcpy(outBuffer, block->data.data(), block->outBytes);
    if (pActualOut) {
        *pActualOut = block->outBytes;
    }

    // Telemetry & budget
    s_cacheHits.fetch_add(1, std::memory_order_relaxed);
    s_cacheHitBytes.fetch_add(block->outBytes, std::memory_order_relaxed);
    s_cacheSavedTimeUs.fetch_add(block->savedUs, std::memory_order_relaxed);

    s_currentMemoryBytes.fetch_sub(block->outBytes, std::memory_order_relaxed);
    if (s_memAvailableEvent) SetEvent(s_memAvailableEvent);

    // Free block memory immediately
    block->data.clear();
    block->data.shrink_to_fit();

    // Record trace record for profiler reporting
    TraceRecord rec;
    rec.seq = s_seqCounter.fetch_add(1, std::memory_order_relaxed) + 1;
    strncpy_s(rec.pakName, sizeof(rec.pakName), archive.c_str(), _TRUNCATE);
    rec.offset = tl_lastRead.offset;
    rec.inBytes = inSize;
    rec.outBytes = block->outBytes;
    rec.decompTimeUs = 50; // served from cache
    rec.loadTimestampUs = tl_lastRead.timestampUs;

    s_currentMainSeq.store(rec.seq, std::memory_order_relaxed);

    AcquireSRWLockExclusive(&s_traceLock);
    s_traceRecords.push_back(rec);
    ReleaseSRWLockExclusive(&s_traceLock);

    s_matchedCount.fetch_add(1, std::memory_order_relaxed);

    // Invalidate read
    tl_lastRead.valid = false;
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

    if (tl_lastRead.valid && tl_lastRead.buffer == inBuffer && tl_lastRead.bytesRead == inSize) {
        TraceRecord rec;
        rec.seq = s_seqCounter.fetch_add(1, std::memory_order_relaxed) + 1;
        std::string archive = ResolveArchiveName(tl_lastRead.hFile);
        strncpy_s(rec.pakName, sizeof(rec.pakName), archive.c_str(), _TRUNCATE);
        rec.offset = tl_lastRead.offset;
        rec.inBytes = inSize;
        rec.outBytes = actualOut ? actualOut : outCapacity;
        rec.decompTimeUs = decompTimeUs;
        rec.loadTimestampUs = tl_lastRead.timestampUs;

        // Update main thread sequence and purge any stale blocks older than rec.seq
        s_currentMainSeq.store(rec.seq, std::memory_order_relaxed);
        AcquireSRWLockExclusive(&s_cacheLock);
        PurgeStaleBlocksUnderLock(rec.seq);
        ReleaseSRWLockExclusive(&s_cacheLock);
        if (s_memAvailableEvent) SetEvent(s_memAvailableEvent);

        tl_lastRead.valid = false;

        AcquireSRWLockExclusive(&s_traceLock);
        s_traceRecords.push_back(rec);
        ReleaseSRWLockExclusive(&s_traceLock);

        s_matchedCount.fetch_add(1, std::memory_order_relaxed);
    } else {
        tl_lastRead.valid = false;
        s_unmatchedCount.fetch_add(1, std::memory_order_relaxed);
    }

    QueryPerformanceCounter(&t1);
    uint64_t oh = (s_freq.QuadPart > 0) ? ((t1.QuadPart - t0.QuadPart) * 1000000LL / s_freq.QuadPart) : 0;
    s_tracerOverheadUs.fetch_add(oh, std::memory_order_relaxed);
}

void PakTracer_OnLoadEnd() {
    s_isActive.store(false, std::memory_order_release);
    tl_lastRead.valid = false;

    if (s_memAvailableEvent) {
        SetEvent(s_memAvailableEvent);
    }

    if (s_bgWorkerThread) {
        WaitForSingleObject(s_bgWorkerThread, 1000);
        CloseHandle(s_bgWorkerThread);
        s_bgWorkerThread = nullptr;
    }

    if (s_memAvailableEvent) {
        CloseHandle(s_memAvailableEvent);
        s_memAvailableEvent = nullptr;
    }

    // Free all remaining cached blocks
    AcquireSRWLockExclusive(&s_cacheLock);
    s_predecompCache.clear();
    ReleaseSRWLockExclusive(&s_cacheLock);
    s_currentMemoryBytes.store(0, std::memory_order_relaxed);

    AcquireSRWLockShared(&s_traceLock);
    uint32_t totalMatched = (uint32_t)s_traceRecords.size();
    uint32_t matched = s_matchedCount.load(std::memory_order_relaxed);
    uint32_t unmatched = s_unmatchedCount.load(std::memory_order_relaxed);
    uint32_t totalDecomps = matched + unmatched;
    double matchRate = (totalDecomps > 0) ? (100.0 * (double)matched / (double)totalDecomps) : 0.0;
    double overheadMs = (double)s_tracerOverheadUs.load(std::memory_order_relaxed) / 1000.0;

    uint32_t hits = s_cacheHits.load(std::memory_order_relaxed);
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

    if (!s_candidates.empty()) {
        double hitPct = (hits + misses > 0) ? (100.0 * (double)hits / (double)(hits + misses)) : 0.0;
        Log("G3O", "[PakCache] === PRE-DECOMPRESSION CACHE PERFORMANCE REPORT ===");
        Log("G3O", "  Candidates Queued:        %zu", s_candidates.size());
        Log("G3O", "  Cache HITS:               %u (%.1f%%)", hits, hitPct);
        Log("G3O", "  Cache MISSES:             %u (%.1f%%)", misses, 100.0 - hitPct);
        Log("G3O", "  Data Served from Cache:   %.2f MB", hitMb);
        Log("G3O", "  Estimated CPU Time Saved: %.2f s", savedSec);
        Log("G3O", "  Peak Cache Memory Used:   %.2f MB (Budget: 128.00 MB)", peakMb);
    }

    // Write trace to CSV only if there was no valid trace (so we preserve golden baseline trace)
    if (!s_hasValidTrace && totalMatched > 0) {
        char csvName[MAX_PATH] = { 0 };
        snprintf(csvName, sizeof(csvName), "G3O_PakTrace_%s.csv", s_currentSaveName.c_str());
        std::ofstream ofs(csvName);
        if (ofs.is_open()) {
            ofs << "seq,pak,offset,in_bytes,out_bytes,decomp_us,load_time_ms\n";
            for (const auto& r : s_traceRecords) {
                double loadTimeMs = (s_freq.QuadPart > 0) ? ((double)r.loadTimestampUs / 1000.0) : 0.0;
                ofs << r.seq << "," << r.pakName << ",0x" << std::hex << std::uppercase << r.offset << std::dec
                    << "," << r.inBytes << "," << r.outBytes << "," << r.decompTimeUs
                    << "," << std::fixed << std::setprecision(2) << loadTimeMs << "\n";
            }
            ofs.close();
            Log("G3O", "[PakTracer] Fresh baseline trace written to '%s' (%u records)", csvName, totalMatched);
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
