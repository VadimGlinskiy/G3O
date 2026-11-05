#include "pak_tracer.h"
#include "diagnostic_logger.h"
#include "decompressor_avx2.h"
#include <libdeflate.h>
#include <windows.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <sstream>
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
// Controlled Pre-Decompression Verification Slots (Stage 2.2)
// -----------------------------------------------------------------------------
struct VerifySlot {
    std::string pakName;
    uint64_t offset = 0;
    uint32_t inBytes = 0;
    uint32_t outBytes = 0;
    std::vector<BYTE> decompressedData;
    uint64_t bgDecompTimeUs = 0;
    volatile LONG ready = 0;
    bool verified = false;
    bool matchSuccess = false;

    VerifySlot() = default;
    VerifySlot(const VerifySlot& o)
        : pakName(o.pakName), offset(o.offset), inBytes(o.inBytes), outBytes(o.outBytes),
          decompressedData(o.decompressedData), bgDecompTimeUs(o.bgDecompTimeUs),
          ready(o.ready), verified(o.verified), matchSuccess(o.matchSuccess) {}
    VerifySlot& operator=(const VerifySlot& o) {
        if (this != &o) {
            pakName = o.pakName; offset = o.offset; inBytes = o.inBytes; outBytes = o.outBytes;
            decompressedData = o.decompressedData; bgDecompTimeUs = o.bgDecompTimeUs;
            ready = o.ready; verified = o.verified; matchSuccess = o.matchSuccess;
        }
        return *this;
    }
};

static std::vector<VerifySlot> s_verifySlots;
static HANDLE s_bgWorkerThread = nullptr;
static std::atomic<uint32_t> s_verifyMatchCount{ 0 };
static std::atomic<uint32_t> s_verifyMismatchCount{ 0 };

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

    size_t totalBytes = 0;
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);

    for (size_t i = 0; i < s_verifySlots.size(); ++i) {
        VerifySlot& slot = s_verifySlots[i];
        std::string fullPath = "Data\\" + slot.pakName;
        HANDLE hFile = CreateFileA(fullPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) continue;

        std::vector<BYTE> compBuf(slot.inBytes);
        OVERLAPPED ov = { 0 };
        ov.Offset = (DWORD)slot.offset;
        ov.OffsetHigh = (DWORD)(slot.offset >> 32);

        DWORD bytesRead = 0;
        if (ReadFile(hFile, compBuf.data(), slot.inBytes, &bytesRead, &ov) && bytesRead == slot.inBytes) {
            slot.decompressedData.resize(slot.outBytes);
            size_t actualOut = 0;
            LARGE_INTEGER dt0, dt1;
            QueryPerformanceCounter(&dt0);

            int res = -1;
            if (G3O_AVX2_IsSupported()) {
                res = G3O_AVX2_DecompressZlib(dec, compBuf.data(), slot.inBytes, slot.decompressedData.data(), slot.outBytes, &actualOut);
            } else {
                res = libdeflate_zlib_decompress(dec, compBuf.data(), slot.inBytes, slot.decompressedData.data(), slot.outBytes, &actualOut);
            }

            QueryPerformanceCounter(&dt1);
            if (res == 0 && actualOut > 0) {
                slot.outBytes = (uint32_t)actualOut;
                slot.decompressedData.resize(actualOut);
                slot.bgDecompTimeUs = (s_freq.QuadPart > 0) ? ((dt1.QuadPart - dt0.QuadPart) * 1000000LL / s_freq.QuadPart) : 0;
                totalBytes += actualOut;
                InterlockedExchange(&slot.ready, 1);
            }
        }
        CloseHandle(hFile);
    }

    if (G3O_AVX2_IsSupported()) {
        G3O_AVX2_FreeDecompressor(dec);
    } else {
        libdeflate_free_decompressor(dec);
    }

    QueryPerformanceCounter(&t1);
    double totalMs = (s_freq.QuadPart > 0) ? ((double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)s_freq.QuadPart) : 0.0;
    Log("G3O", "[PakVerify] Background worker pre-decompressed %zu slots (%.2f MB) in %.2f ms",
        s_verifySlots.size(), (double)totalBytes / (1024.0 * 1024.0), totalMs);
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
    s_verifyMatchCount.store(0, std::memory_order_relaxed);
    s_verifyMismatchCount.store(0, std::memory_order_relaxed);

    s_verifySlots.clear();
    tl_lastRead.valid = false;

    // Check if valid trace exists from previous run
    std::string csvPath = "G3O_PakTrace_" + s_currentSaveName + ".csv";
    if (IsTraceValid(csvPath, s_currentSaveName)) {
        // Load target blocks for controlled Stage 2.2 verification:
        // Pick first 16 blocks from GotoLoadedPosition where out_bytes <= 4 MB
        std::ifstream ifs(csvPath);
        if (ifs.is_open()) {
            std::string line;
            std::getline(ifs, line); // header
            while (std::getline(ifs, line) && s_verifySlots.size() < 16) {
                std::stringstream ss(line);
                std::string item;
                std::vector<std::string> cols;
                while (std::getline(ss, item, ',')) cols.push_back(item);
                if (cols.size() >= 7) {
                    double t_ms = std::stod(cols[6]);
                    uint32_t outSz = std::stoul(cols[4]);
                    // GotoLoadedPosition targets
                    if (t_ms >= 12000.0 && outSz <= 4 * 1024 * 1024 && outSz > 1024) {
                        VerifySlot slot;
                        slot.pakName = cols[1];
                        slot.offset = std::stoull(cols[2], nullptr, 16);
                        slot.inBytes = std::stoul(cols[3]);
                        slot.outBytes = outSz;
                        s_verifySlots.push_back(slot);
                    }
                }
            }
            ifs.close();
        }

        if (!s_verifySlots.empty()) {
            Log("G3O", "[PakVerify] Selected %zu candidate blocks for controlled Stage 2.2 pre-decompression & memcmp verification",
                s_verifySlots.size());
            s_bgWorkerThread = CreateThread(nullptr, 0, BgPredecompWorker, nullptr, 0, nullptr);
        }
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

void PakTracer_RecordDecompress(
    const void* inBuffer,
    DWORD inSize,
    DWORD outCapacity,
    DWORD actualOut,
    uint64_t decompTimeUs,
    const void* outBuffer
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

        // Stage 2.2: Controlled memcmp verification
        if (outBuffer && actualOut > 0) {
            for (auto& slot : s_verifySlots) {
                if (slot.ready != 0 && !slot.verified &&
                    slot.offset == rec.offset && slot.pakName == archive) {
                    
                    slot.verified = true;
                    if (actualOut == slot.decompressedData.size()) {
                        int diff = memcmp(outBuffer, slot.decompressedData.data(), actualOut);
                        if (diff == 0) {
                            slot.matchSuccess = true;
                            s_verifyMatchCount.fetch_add(1, std::memory_order_relaxed);
                            Log("G3O", "[PakVerify] Block #%u [%-24s off=0x%08llX size=%-7u]: 100%% BYTE-FOR-BYTE MATCH! (BgTime=%.2f ms, MainTime=%.2f ms)",
                                rec.seq, slot.pakName.c_str(), slot.offset, actualOut,
                                (double)slot.bgDecompTimeUs / 1000.0, (double)decompTimeUs / 1000.0);
                        } else {
                            s_verifyMismatchCount.fetch_add(1, std::memory_order_relaxed);
                            Log("G3O", "[PakVerify] Block #%u [%-24s off=0x%08llX size=%-7u]: MISMATCH (memcmp diff=%d)!",
                                rec.seq, slot.pakName.c_str(), slot.offset, actualOut, diff);
                        }
                    } else {
                        s_verifyMismatchCount.fetch_add(1, std::memory_order_relaxed);
                        Log("G3O", "[PakVerify] Block #%u [%-24s off=0x%08llX]: SIZE MISMATCH (engine=%u, bg=%zu)!",
                            rec.seq, slot.pakName.c_str(), slot.offset, actualOut, slot.decompressedData.size());
                    }
                    break;
                }
            }
        }

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

    if (s_bgWorkerThread) {
        WaitForSingleObject(s_bgWorkerThread, 1000);
        CloseHandle(s_bgWorkerThread);
        s_bgWorkerThread = nullptr;
    }

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

    uint32_t vMatch = s_verifyMatchCount.load(std::memory_order_relaxed);
    uint32_t vMismatch = s_verifyMismatchCount.load(std::memory_order_relaxed);
    if (!s_verifySlots.empty()) {
        Log("G3O", "[PakVerify] Stage 2.2 Controlled Experiment Results:");
        Log("G3O", "  Candidate slots tested: %zu", s_verifySlots.size());
        Log("G3O", "  100%% Byte-for-Byte Matches: %u", vMatch);
        Log("G3O", "  Mismatches:                 %u", vMismatch);
    }

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

    std::vector<TraceRecord> sorted = s_traceRecords;
    ReleaseSRWLockShared(&s_traceLock);

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
