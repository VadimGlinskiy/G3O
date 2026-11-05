#include "pak_tracer.h"
#include "diagnostic_logger.h"
#include <windows.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <atomic>
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

    // Query file name from OS
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
            // .pak, .mod, .cpt, or .m00 - .m99, .p00 - .p99, .c00 - .c99
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

    tl_lastRead.valid = false;
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

void PakTracer_RecordDecompress(
    const void* inBuffer,
    DWORD inSize,
    DWORD outCapacity,
    DWORD actualOut,
    uint64_t decompTimeUs
) {
    if (!s_isActive.load(std::memory_order_relaxed)) return;

    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);

    // Strict association: buffer pointer, size, and single-shot valid flag
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

        // Invalidate immediately so this read can never be matched again
        tl_lastRead.valid = false;

        AcquireSRWLockExclusive(&s_traceLock);
        s_traceRecords.push_back(rec);
        ReleaseSRWLockExclusive(&s_traceLock);

        s_matchedCount.fetch_add(1, std::memory_order_relaxed);
    } else {
        // Unmatched / non-pak decompression
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

    AcquireSRWLockShared(&s_traceLock);
    uint32_t totalMatched = (uint32_t)s_traceRecords.size();
    uint32_t matched = s_matchedCount.load(std::memory_order_relaxed);
    uint32_t unmatched = s_unmatchedCount.load(std::memory_order_relaxed);
    uint32_t totalDecomps = matched + unmatched;
    double matchRate = (totalDecomps > 0) ? (100.0 * (double)matched / (double)totalDecomps) : 0.0;
    double overheadMs = (double)s_tracerOverheadUs.load(std::memory_order_relaxed) / 1000.0;

    Log("G3O", "================================================================================");
    Log("G3O", "[PakTracer] === SAVEGAME LOAD TRACE REPORT: '%s' ===", s_currentSaveName.c_str());
    Log("G3O", "  Total decompressions: %u", totalDecomps);
    Log("G3O", "  Matched to PAK reads: %u (%.1f%%)", matched, matchRate);
    Log("G3O", "  Unmatched / Other:    %u (%.1f%%)", unmatched, 100.0 - matchRate);
    Log("G3O", "  Tracer overhead:      %.2f ms", overheadMs);

    // Write trace to CSV file: Gothic 3/G3O_PakTrace_<SaveName>.csv
    char csvName[MAX_PATH] = { 0 };
    snprintf(csvName, sizeof(csvName), "G3O_PakTrace_%s.csv", s_currentSaveName.c_str());
    FILE* fp = fopen(csvName, "w");
    if (fp) {
        fprintf(fp, "seq,pak,offset,in_bytes,out_bytes,decomp_us,load_time_ms\n");
        for (const auto& r : s_traceRecords) {
            fprintf(fp, "%u,%s,0x%llX,%u,%u,%llu,%.2f\n",
                r.seq, r.pakName, r.offset, r.inBytes, r.outBytes,
                r.decompTimeUs, (double)r.loadTimestampUs / 1000.0);
        }
        fclose(fp);
        Log("G3O", "[PakTracer] Full trace written to '%s' (%u records)", csvName, totalMatched);
    }

    // Identify top 10 most expensive blocks
    std::vector<TraceRecord> sorted = s_traceRecords;
    ReleaseSRWLockShared(&s_traceLock);

    std::sort(sorted.begin(), sorted.end(), [](const TraceRecord& a, const TraceRecord& b) {
        return a.decompTimeUs > b.decompTimeUs;
    });

    Log("G3O", "[PakTracer] Top 10 Most Expensive Decompressions:");
    for (size_t i = 0; i < sorted.size() && i < 10; ++i) {
        Log("G3O", "   #%2zu: %-24s offset=0x%08llX | %-6u -> %-7u | %.2f ms (at t=%.2f s)",
            i + 1, sorted[i].pakName, sorted[i].offset,
            sorted[i].inBytes, sorted[i].outBytes,
            (double)sorted[i].decompTimeUs / 1000.0,
            (double)sorted[i].loadTimestampUs / 1000000.0);
    }
    Log("G3O", "================================================================================");
}

} // namespace G3O
