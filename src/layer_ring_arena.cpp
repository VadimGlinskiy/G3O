#include "layer_ring_arena.h"
#include "config_manager.h"
#include "diagnostic_logger.h"
#include "decompressor_avx2.h"
#include "pak_read_at.h"
#include <libdeflate.h>
#include <vector>
#include <string>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace G3O {

static constexpr size_t kInvalidIndex = static_cast<size_t>(-1);
static constexpr size_t kRingSlots = 4;
static constexpr size_t kSlotSize = 1024 * 1024; // 1 MB per slot (max uncompressed node in RB_Mod is 877 KB)
static constexpr uint64_t kMaxTotalCompressedBytes = 32ULL * 1024ULL * 1024ULL;

struct LayerNodeEntry {
    uint64_t fileOffset = 0;
    uint32_t compSize = 0;
    uint32_t uncompSize = 0;
    uint32_t first4 = 0;
    char name[64] = {0};
    std::vector<uint8_t> compData;
};

struct RingSlot {
    std::atomic<size_t> readyIndex{kInvalidIndex};
    std::atomic<size_t> failedIndex{kInvalidIndex};
    uint64_t fileOffset = 0;
    uint32_t compSize = 0;
    uint32_t uncompSize = 0;
    uint32_t first4 = 0;
    uint8_t* pData = nullptr;
};

static std::vector<LayerNodeEntry> s_entries;
static std::string s_archivePath;

static RingSlot s_slots[kRingSlots];
static uint8_t* s_ringMemory = nullptr;

static HANDLE s_workerThread = NULL;
static HANDLE s_hStartEvent = NULL;
static HANDLE s_hSlotConsumedEvent = NULL;
static HANDLE s_hStopEvent = NULL;
static HANDLE s_hWorkerIdleEvent = NULL;

static std::atomic<bool> s_isSectorActive{false};
static std::atomic<bool> s_shouldExit{false};
static std::atomic<bool> s_workerPausedForTest{false};
static std::atomic<size_t> s_consumerIndex{0};
static std::atomic<size_t> s_nextWorkerIndex{0};
static uint32_t s_waitTimeoutMs = 10;
static std::atomic<bool> s_explicitArchiveInit{false};

static LayerRingArenaStats s_stats;
static SRWLOCK s_stateLock = SRWLOCK_INIT;

class ScopedArenaLock {
public:
    ScopedArenaLock() { AcquireSRWLockExclusive(&s_stateLock); }
    ~ScopedArenaLock() { ReleaseSRWLockExclusive(&s_stateLock); }
    ScopedArenaLock(const ScopedArenaLock&) = delete;
    ScopedArenaLock& operator=(const ScopedArenaLock&) = delete;
};

class ScopedArenaTryLock {
public:
    ScopedArenaTryLock() : m_acquired(TryAcquireSRWLockExclusive(&s_stateLock) != FALSE) {}
    ~ScopedArenaTryLock() {
        if (m_acquired) ReleaseSRWLockExclusive(&s_stateLock);
    }
    ScopedArenaTryLock(const ScopedArenaTryLock&) = delete;
    ScopedArenaTryLock& operator=(const ScopedArenaTryLock&) = delete;
    bool owns_lock() const { return m_acquired; }

private:
    bool m_acquired;
};

static void ShutdownLayerRingArenaLocked();

static bool IsLayerRingArenaEnabled() {
    return s_explicitArchiveInit.load(std::memory_order_acquire) || GetConfig().enableLayerRingArena;
}

static uint32_t ReadLE32(const void* p) {
    uint32_t val = 0;
    std::memcpy(&val, p, sizeof(val));
    return val;
}

static uint64_t ReadLE64(const void* p) {
    uint64_t val = 0;
    std::memcpy(&val, p, sizeof(val));
    return val;
}

static bool IsIdentifierChar(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static size_t FindLayerStemInSecdat(const std::string& secdatStr, const char* stem) {
    if (!stem || !*stem) return kInvalidIndex;
    const size_t stemLen = std::strlen(stem);
    if (secdatStr.size() < stemLen) return kInvalidIndex;

    size_t searchPos = 0;
    while ((searchPos = secdatStr.find(stem, searchPos)) != std::string::npos) {
        const bool leftOk = (searchPos == 0) ||
                            !IsIdentifierChar(static_cast<unsigned char>(secdatStr[searchPos - 1]));
        const size_t endPos = searchPos + stemLen;
        const bool rightOk = (endPos >= secdatStr.size()) ||
                             !IsIdentifierChar(static_cast<unsigned char>(secdatStr[endPos]));
        if (leftOk && rightOk) {
            return searchPos;
        }
        ++searchPos;
    }
    return kInvalidIndex;
}

static std::string FindArchivePath(const char* relName) {
    if (!relName || !*relName) return {};

    HANDLE h = CreateFileA(relName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        return relName;
    }

    char exePath[MAX_PATH] = {0};
    if (GetModuleFileNameA(NULL, exePath, MAX_PATH) > 0) {
        char* slash = strrchr(exePath, '\\');
        if (!slash) slash = strrchr(exePath, '/');
        if (slash) {
            *(slash + 1) = '\0';
            char candidate[MAX_PATH];
            snprintf(candidate, sizeof(candidate), "%s%s", exePath, relName);
            h = CreateFileA(candidate, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                CloseHandle(h);
                return candidate;
            }
        }
    }
    return {};
}

static bool IndexArchiveLayers(const std::string& archivePath) {
    ScopedInternalPakRead internalRead;
    s_entries.clear();
    HANDLE hFile = CreateFileA(archivePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    BYTE hdr[32];
    DWORD bytesRead = 0;
    if (!ReadFile(hFile, hdr, 32, &bytesRead, NULL) || bytesRead != 32) {
        CloseHandle(hFile);
        return false;
    }

    if (memcmp(hdr + 4, "G3V0", 4) != 0) {
        CloseHandle(hFile);
        return false;
    }

    const uint32_t fat_off = ReadLE32(hdr + 24);
    LARGE_INTEGER fsize = {};
    if (!GetFileSizeEx(hFile, &fsize) || fsize.QuadPart <= 32 ||
        fat_off < 32 || static_cast<uint64_t>(fat_off) >= static_cast<uint64_t>(fsize.QuadPart)) {
        CloseHandle(hFile);
        return false;
    }

    const uint64_t fileSize = static_cast<uint64_t>(fsize.QuadPart);
    const uint64_t fat_size = fileSize - fat_off;
    if (fat_size == 0 || fat_size > 16 * 1024 * 1024) {
        CloseHandle(hFile);
        return false;
    }

    BYTE* pFat = reinterpret_cast<BYTE*>(VirtualAlloc(NULL, static_cast<SIZE_T>(fat_size), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!pFat) {
        CloseHandle(hFile);
        return false;
    }

    LARGE_INTEGER liFat;
    liFat.QuadPart = fat_off;
    if (!SetFilePointerEx(hFile, liFat, NULL, FILE_BEGIN) ||
        !ReadFile(hFile, pFat, static_cast<DWORD>(fat_size), &bytesRead, NULL) ||
        bytesRead != fat_size) {
        VirtualFree(pFat, 0, MEM_RELEASE);
        CloseHandle(hFile);
        return false;
    }

    const size_t totalBytes = static_cast<size_t>(bytesRead);
    const char targetSubstr[] = "G3_World_01/RB_Mod/";
    const size_t targetSubstrLen = sizeof(targetSubstr) - 1;

    struct RawEntry {
        uint64_t fileOffset = 0;
        uint32_t compSize = 0;
        uint32_t uncompSize = 0;
        char name[64] = {0};
        size_t secdatOffset = kInvalidIndex;
    };
    std::vector<RawEntry> rawList;

    uint64_t secdatOff = 0;
    uint32_t secdatCompSize = 0;
    uint32_t secdatUncompSize = 0;
    uint32_t secdatFlags = 0;

    size_t pos = 40;
    while (pos < totalBytes) {
        if (pos + targetSubstrLen <= totalBytes && memcmp(pFat + pos, targetSubstr, targetSubstrLen) == 0) {
            size_t nameEnd = pos;
            while (nameEnd < totalBytes && pFat[nameEnd] != '\0') {
                nameEnd++;
            }
            if (nameEnd < totalBytes && nameEnd - pos >= 5 && pos >= 40) {
                const BYTE* pHdr = pFat + pos - 40;
                const uint32_t flags = ReadLE32(pHdr);
                const uint64_t off   = ReadLE64(pHdr + 4);
                const uint64_t c_sz  = ReadLE64(pHdr + 12);
                const uint64_t u_sz  = ReadLE64(pHdr + 20);
                const bool validRange = (off >= 32 && off <= fileSize && c_sz <= fileSize - off);

                const char* ext = reinterpret_cast<const char*>(pFat + nameEnd - 5);
                if (_stricmp(ext, ".node") == 0) {
                    if (validRange && (flags & 0x00020020) == 0x00020020 &&
                        c_sz >= sizeof(uint32_t) && c_sz <= kSlotSize &&
                        u_sz > 0 && u_sz <= kSlotSize) {
                        RawEntry re;
                        re.fileOffset = off;
                        re.compSize = static_cast<uint32_t>(c_sz);
                        re.uncompSize = static_cast<uint32_t>(u_sz);
                        const char* slash = strrchr(reinterpret_cast<const char*>(pFat + pos), '/');
                        const char* baseName = slash ? (slash + 1) : reinterpret_cast<const char*>(pFat + pos);
                        snprintf(re.name, sizeof(re.name), "%s", baseName);

                        auto dupIt = std::find_if(rawList.begin(), rawList.end(), [&](const RawEntry& existing) {
                            return _stricmp(existing.name, re.name) == 0;
                        });
                        if (dupIt != rawList.end()) {
                            *dupIt = re;
                        } else {
                            rawList.push_back(re);
                        }
                    }
                } else if (_stricmp(reinterpret_cast<const char*>(pFat + pos), "G3_World_01/RB_Mod/RB_Mod.secdat") == 0) {
                    if (validRange && c_sz > 0 && c_sz <= 64 * 1024 && u_sz > 0 && u_sz <= 64 * 1024) {
                        secdatOff = off;
                        secdatCompSize = static_cast<uint32_t>(c_sz);
                        secdatUncompSize = static_cast<uint32_t>(u_sz);
                        secdatFlags = flags;
                    }
                }
            }
            pos = nameEnd + 1;
            continue;
        }
        pos++;
    }

    VirtualFree(pFat, 0, MEM_RELEASE);

    if (rawList.empty()) {
        CloseHandle(hFile);
        return false;
    }

    // If RB_Mod.secdat exists, decompress it to get the exact layer activation order
    if (secdatOff > 0 && secdatCompSize > 0 && secdatUncompSize > 0 && secdatUncompSize <= 64 * 1024) {
        std::vector<BYTE> compSecdat(secdatCompSize);
        std::vector<BYTE> decompSecdat(secdatUncompSize);
        LARGE_INTEGER liSec;
        liSec.QuadPart = secdatOff;
        DWORD r = 0;
        if (SetFilePointerEx(hFile, liSec, NULL, FILE_BEGIN) &&
            ReadFile(hFile, compSecdat.data(), secdatCompSize, &r, NULL) && r == secdatCompSize) {
            bool secdatReady = false;
            size_t actualSec = 0;
            if ((secdatFlags & 0x00000020) == 0 && secdatCompSize == secdatUncompSize) {
                std::memcpy(decompSecdat.data(), compSecdat.data(), secdatUncompSize);
                actualSec = secdatUncompSize;
                secdatReady = true;
            } else {
                libdeflate_decompressor* dec = libdeflate_alloc_decompressor();
                if (dec) {
                    if (libdeflate_zlib_decompress(dec, compSecdat.data(), secdatCompSize,
                                                   decompSecdat.data(), secdatUncompSize, &actualSec) == 0 &&
                        actualSec == secdatUncompSize) {
                        secdatReady = true;
                    }
                    libdeflate_free_decompressor(dec);
                }
            }

            if (secdatReady && actualSec > 0) {
                const std::string secdatStr(reinterpret_cast<const char*>(decompSecdat.data()), actualSec);
                for (auto& re : rawList) {
                    char stem[64] = {0};
                    snprintf(stem, sizeof(stem), "%s", re.name);
                    char* dot = strrchr(stem, '.');
                    if (dot) *dot = '\0';
                    re.secdatOffset = FindLayerStemInSecdat(secdatStr, stem);
                }
                std::stable_sort(rawList.begin(), rawList.end(), [](const RawEntry& a, const RawEntry& b) {
                    if (a.secdatOffset != b.secdatOffset) {
                        return a.secdatOffset < b.secdatOffset;
                    }
                    return a.fileOffset < b.fileOffset;
                });
            }
        }
    }

    // Populate s_entries with full compressed payload and 4-byte prefix
    uint64_t accumulatedCompBytes = 0;
    for (const auto& re : rawList) {
        if (accumulatedCompBytes + re.compSize > kMaxTotalCompressedBytes) {
            s_entries.clear();
            CloseHandle(hFile);
            return false;
        }
        LARGE_INTEGER liBlock;
        liBlock.QuadPart = re.fileOffset;
        if (SetFilePointerEx(hFile, liBlock, NULL, FILE_BEGIN)) {
            std::vector<uint8_t> compData(re.compSize);
            DWORD bytesReadBlock = 0;
            if (ReadFile(hFile, compData.data(), re.compSize, &bytesReadBlock, NULL) &&
                bytesReadBlock == re.compSize) {
                LayerNodeEntry entry;
                entry.fileOffset = re.fileOffset;
                entry.compSize = re.compSize;
                entry.uncompSize = re.uncompSize;
                entry.first4 = ReadLE32(compData.data());
                snprintf(entry.name, sizeof(entry.name), "%s", re.name);
                entry.compData = std::move(compData);
                s_entries.push_back(std::move(entry));
                accumulatedCompBytes += re.compSize;
            }
        }
    }

    CloseHandle(hFile);
    return !s_entries.empty();
}

static DWORD WINAPI LayerRingArena_WorkerThread(LPVOID /*lpParam*/) {
    ScopedInternalPakRead internalRead;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    libdeflate_decompressor* dec = nullptr;
    if (G3O_AVX2_IsSupported()) {
        dec = G3O_AVX2_AllocDecompressor();
    } else {
        dec = libdeflate_alloc_decompressor();
    }
    if (!dec) {
        s_isSectorActive.store(false, std::memory_order_release);
        if (s_hWorkerIdleEvent) SetEvent(s_hWorkerIdleEvent);
        return 1;
    }

    std::vector<BYTE> compBuf(512 * 1024);
    HANDLE waitHandles[2] = { s_hStopEvent, s_hStartEvent };

    while (!s_shouldExit.load(std::memory_order_acquire)) {
        DWORD waitRes = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
        if (waitRes == WAIT_OBJECT_0 || s_shouldExit.load(std::memory_order_acquire)) {
            break; // Stop signaled
        }

        if (!s_isSectorActive.load(std::memory_order_acquire)) {
            if (s_hWorkerIdleEvent) SetEvent(s_hWorkerIdleEvent);
            continue;
        }

        HANDLE hArchive = CreateFileA(s_archivePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (hArchive == INVALID_HANDLE_VALUE) {
            s_isSectorActive.store(false, std::memory_order_release);
            if (s_hWorkerIdleEvent) SetEvent(s_hWorkerIdleEvent);
            continue;
        }

        while (s_isSectorActive.load(std::memory_order_acquire) && !s_shouldExit.load(std::memory_order_acquire)) {
            if (s_workerPausedForTest.load(std::memory_order_acquire)) {
                HANDLE consumerWait[2] = { s_hStopEvent, s_hSlotConsumedEvent };
                DWORD pauseRes = WaitForMultipleObjects(2, consumerWait, FALSE, 5);
                if (pauseRes == WAIT_OBJECT_0 || !s_isSectorActive.load(std::memory_order_acquire)) {
                    break;
                }
                continue;
            }

            const size_t consumerIdx = s_consumerIndex.load(std::memory_order_acquire);
            size_t workerIdx = s_nextWorkerIndex.load(std::memory_order_relaxed);
            if (workerIdx < consumerIdx) {
                workerIdx = consumerIdx;
                s_nextWorkerIndex.store(workerIdx, std::memory_order_relaxed);
            }

            if (workerIdx >= s_entries.size()) {
                // All entries decompressed for this sector; wait for sector completion
                HANDLE consumerWait[2] = { s_hStopEvent, s_hSlotConsumedEvent };
                DWORD doneRes = WaitForMultipleObjects(2, consumerWait, FALSE, 25);
                if (doneRes == WAIT_OBJECT_0 || !s_isSectorActive.load(std::memory_order_acquire)) {
                    break;
                }
                continue;
            }

            // Enforce strict ring window bound: worker may not advance past consumerIdx + kRingSlots
            if (workerIdx - consumerIdx >= kRingSlots) {
                // Ring buffer is full; wait for consumer to free a slot
                HANDLE consumerWait[2] = { s_hStopEvent, s_hSlotConsumedEvent };
                DWORD cRes = WaitForMultipleObjects(2, consumerWait, FALSE, 25);
                if (cRes == WAIT_OBJECT_0 || !s_isSectorActive.load(std::memory_order_acquire)) {
                    break;
                }
                continue;
            }

            const size_t targetSlot = workerIdx % kRingSlots;
            RingSlot& slot = s_slots[targetSlot];
            slot.readyIndex.store(kInvalidIndex, std::memory_order_release);
            slot.failedIndex.store(kInvalidIndex, std::memory_order_release);

            const auto& entry = s_entries[workerIdx];
            if (entry.compSize > compBuf.size()) {
                compBuf.resize(entry.compSize);
            }

            LARGE_INTEGER li;
            li.QuadPart = entry.fileOffset;
            DWORD bytesRead = 0;
            if (SetFilePointerEx(hArchive, li, NULL, FILE_BEGIN) &&
                ReadFile(hArchive, compBuf.data(), entry.compSize, &bytesRead, NULL) &&
                bytesRead == entry.compSize &&
                entry.compData.size() == entry.compSize &&
                std::memcmp(compBuf.data(), entry.compData.data(), entry.compSize) == 0) {

                size_t actualOut = 0;
                int res = -1;
                if (G3O_AVX2_IsSupported()) {
                    res = G3O_AVX2_DecompressZlib(dec, compBuf.data(), entry.compSize, slot.pData, kSlotSize, &actualOut);
                } else {
                    res = libdeflate_zlib_decompress(dec, compBuf.data(), entry.compSize, slot.pData, kSlotSize, &actualOut);
                }

                if (res == 0 && actualOut == entry.uncompSize) {
                    slot.fileOffset = entry.fileOffset;
                    slot.compSize = entry.compSize;
                    slot.uncompSize = static_cast<uint32_t>(actualOut);
                    slot.first4 = entry.first4;
                    slot.readyIndex.store(workerIdx, std::memory_order_release);
                } else {
                    slot.failedIndex.store(workerIdx, std::memory_order_release);
                }
            } else {
                slot.failedIndex.store(workerIdx, std::memory_order_release);
            }

            s_nextWorkerIndex.store(workerIdx + 1, std::memory_order_release);
        }

        CloseHandle(hArchive);
        if (s_hWorkerIdleEvent) SetEvent(s_hWorkerIdleEvent);
    }

    if (G3O_AVX2_IsSupported()) {
        G3O_AVX2_FreeDecompressor(dec);
    } else {
        libdeflate_free_decompressor(dec);
    }
    return 0;
}

bool InitializeLayerRingArenaFromArchive(const char* archivePath, uint32_t waitTimeoutMs) {
    ShutdownLayerRingArena();
    ScopedArenaLock lock;

    if (!archivePath || !*archivePath) {
        return false;
    }

    s_archivePath = FindArchivePath(archivePath);
    if (s_archivePath.empty()) {
        Log("G3O", "[LayerRingArena] Notice: %s not found (RB_Mod not active). Ring Arena idle.", archivePath);
        return false;
    }

    if (!IndexArchiveLayers(s_archivePath)) {
        Log("G3O", "[LayerRingArena] Notice: No .node layers found in %s.", s_archivePath.c_str());
        return false;
    }

    uint64_t totalComp = 0;
    uint64_t totalUncomp = 0;
    for (const auto& e : s_entries) {
        totalComp += e.compSize;
        totalUncomp += e.uncompSize;
    }

    Log("G3O", "[LayerRingArena] Indexed %zu .node layers for RB_Mod in %s (%.2f MB compressed -> %.2f MB uncompressed, secdat-ordered).",
        s_entries.size(), s_archivePath.c_str(),
        static_cast<double>(totalComp) / (1024.0 * 1024.0),
        static_cast<double>(totalUncomp) / (1024.0 * 1024.0));

    // Allocate 4 MB ring memory (4 slots * 1 MB)
    s_ringMemory = reinterpret_cast<uint8_t*>(VirtualAlloc(NULL, kRingSlots * kSlotSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!s_ringMemory) {
        Log("G3O", "[LayerRingArena] Error: Failed to allocate %zu MB ring arena memory.", (kRingSlots * kSlotSize) / (1024 * 1024));
        s_entries.clear();
        return false;
    }

    for (size_t i = 0; i < kRingSlots; ++i) {
        s_slots[i].readyIndex.store(kInvalidIndex, std::memory_order_relaxed);
        s_slots[i].failedIndex.store(kInvalidIndex, std::memory_order_relaxed);
        s_slots[i].compSize = 0;
        s_slots[i].uncompSize = 0;
        s_slots[i].first4 = 0;
        s_slots[i].pData = s_ringMemory + (i * kSlotSize);
    }

    s_hStartEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
    s_hSlotConsumedEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
    s_hStopEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    s_hWorkerIdleEvent = CreateEventA(NULL, TRUE, TRUE, NULL);

    if (!s_hStartEvent || !s_hSlotConsumedEvent || !s_hStopEvent || !s_hWorkerIdleEvent) {
        ShutdownLayerRingArenaLocked();
        return false;
    }

    s_shouldExit.store(false, std::memory_order_relaxed);
    s_isSectorActive.store(false, std::memory_order_relaxed);
    s_workerPausedForTest.store(false, std::memory_order_relaxed);
    s_consumerIndex.store(0, std::memory_order_relaxed);
    s_nextWorkerIndex.store(0, std::memory_order_relaxed);
    s_waitTimeoutMs = waitTimeoutMs;

    s_workerThread = CreateThread(NULL, 0, LayerRingArena_WorkerThread, NULL, 0, NULL);
    if (s_workerThread) {
        s_explicitArchiveInit.store(true, std::memory_order_release);
        Log("G3O", "[LayerRingArena] Sliding-Window Ring Arena active: 4 slots x 1 MB (4 MB VAS budget), AVX2 background decompressor ready.");
        return true;
    }

    ShutdownLayerRingArenaLocked();
    return false;
}

void InitializeLayerRingArena() {
    if (!IsLayerRingArenaEnabled()) {
        return;
    }
    InitializeLayerRingArenaFromArchive("Data\\Projects_compiled.m17", 10);
}

static void ShutdownLayerRingArenaLocked() {
    s_shouldExit.store(true, std::memory_order_release);
    s_isSectorActive.store(false, std::memory_order_release);
    s_workerPausedForTest.store(false, std::memory_order_release);

    if (s_hStopEvent) SetEvent(s_hStopEvent);
    if (s_hSlotConsumedEvent) SetEvent(s_hSlotConsumedEvent);

    if (s_workerThread) {
        CancelSynchronousIo(s_workerThread);
        WaitForSingleObject(s_workerThread, INFINITE);
        CloseHandle(s_workerThread);
        s_workerThread = NULL;
    }

    if (s_hStartEvent) { CloseHandle(s_hStartEvent); s_hStartEvent = NULL; }
    if (s_hSlotConsumedEvent) { CloseHandle(s_hSlotConsumedEvent); s_hSlotConsumedEvent = NULL; }
    if (s_hStopEvent) { CloseHandle(s_hStopEvent); s_hStopEvent = NULL; }
    if (s_hWorkerIdleEvent) { CloseHandle(s_hWorkerIdleEvent); s_hWorkerIdleEvent = NULL; }

    if (s_ringMemory) {
        VirtualFree(s_ringMemory, 0, MEM_RELEASE);
        s_ringMemory = nullptr;
    }

    for (size_t i = 0; i < kRingSlots; ++i) {
        s_slots[i].readyIndex.store(kInvalidIndex, std::memory_order_relaxed);
        s_slots[i].failedIndex.store(kInvalidIndex, std::memory_order_relaxed);
        s_slots[i].pData = nullptr;
    }

    s_entries.clear();
    s_explicitArchiveInit.store(false, std::memory_order_release);
}

void ShutdownLayerRingArena() {
    ScopedArenaLock lock;
    ShutdownLayerRingArenaLocked();
}

void LayerRingArena_SetWorkerPausedForTest(bool paused) {
    ScopedArenaLock lock;
    s_workerPausedForTest.store(paused, std::memory_order_release);
    if (s_hSlotConsumedEvent) {
        SetEvent(s_hSlotConsumedEvent);
    }
}

static bool WaitForWorkerIdle() {
    if (!s_hWorkerIdleEvent || !s_workerThread) {
        return false;
    }
    HANDLE waitHandles[2] = { s_hWorkerIdleEvent, s_workerThread };
    return WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE) == WAIT_OBJECT_0;
}

static void EndSectorLocked() {
    if (!s_isSectorActive.load(std::memory_order_acquire)) {
        return;
    }

    s_isSectorActive.store(false, std::memory_order_release);
    if (s_hSlotConsumedEvent) SetEvent(s_hSlotConsumedEvent);
    if (s_workerThread) CancelSynchronousIo(s_workerThread);
    WaitForWorkerIdle();

    Log("G3O", "[LayerRingArena] Finished sector: %u / %zu layers served from Ring Arena (%.2f MB zero-overhead decomp, %u misses).",
        s_stats.hits, s_entries.size(),
        static_cast<double>(s_stats.bytesServed) / (1024.0 * 1024.0),
        s_stats.misses);
}

void LayerRingArena_OnSectorStart(const char* sectorName) {
    ScopedArenaLock lock;
    if (!IsLayerRingArenaEnabled() || s_entries.empty() || !s_workerThread ||
        WaitForSingleObject(s_workerThread, 0) != WAIT_TIMEOUT) {
        return;
    }

    if (sectorName && (strcmp(sectorName, "RB_Mod") == 0 || strstr(sectorName, "RB_Mod") != nullptr)) {
        EndSectorLocked();
        if (!WaitForWorkerIdle()) {
            return;
        }

        for (size_t i = 0; i < kRingSlots; ++i) {
            s_slots[i].readyIndex.store(kInvalidIndex, std::memory_order_relaxed);
            s_slots[i].failedIndex.store(kInvalidIndex, std::memory_order_relaxed);
            s_slots[i].compSize = 0;
            s_slots[i].uncompSize = 0;
            s_slots[i].first4 = 0;
        }
        s_consumerIndex.store(0, std::memory_order_relaxed);
        s_nextWorkerIndex.store(0, std::memory_order_relaxed);
        s_stats = {};

        if (s_hWorkerIdleEvent) ResetEvent(s_hWorkerIdleEvent);
        s_isSectorActive.store(true, std::memory_order_release);
        SetEvent(s_hStartEvent);

        Log("G3O", "[LayerRingArena] Started pre-decompression ring pipeline for sector '%s' (%zu layers).", sectorName, s_entries.size());
    }
}

void LayerRingArena_OnSectorEnd() {
    ScopedArenaLock lock;
    EndSectorLocked();
}

bool LayerRingArena_TryGet(
    const void* inBuffer,
    DWORD inSize,
    void* outBuffer,
    DWORD outCapacity,
    DWORD* pOutWritten
) {
    // Let contending load threads fall back rather than block behind this lookup.
    ScopedArenaTryLock lock;
    if (!lock.owns_lock() || !s_isSectorActive.load(std::memory_order_acquire) ||
        !inBuffer || inSize < sizeof(uint32_t) || !outBuffer || outCapacity == 0) {
        return false;
    }

    const uint32_t inFirst4 = ReadLE32(inBuffer);
    const size_t currentConsumer = s_consumerIndex.load(std::memory_order_acquire);

    // Identify entry index in s_entries using exact full compressed block comparison
    size_t matchedIdx = kInvalidIndex;
    if (currentConsumer < s_entries.size()) {
        const size_t maxAhead = (currentConsumer + kRingSlots < s_entries.size())
                                    ? (currentConsumer + kRingSlots)
                                    : s_entries.size();
        for (size_t a = currentConsumer; a < maxAhead; ++a) {
            const auto& candidate = s_entries[a];
            if (candidate.compSize == inSize &&
                candidate.first4 == inFirst4 &&
                candidate.compData.size() == inSize &&
                std::memcmp(inBuffer, candidate.compData.data(), inSize) == 0) {
                matchedIdx = a;
                break;
            }
        }
    }

    if (matchedIdx == kInvalidIndex) {
        // Not a monitored RB_Mod layer block in the active window
        return false;
    }

    // Validate caller output buffer capacity before mutating ring state
    if (outCapacity < s_entries[matchedIdx].uncompSize) {
        s_stats.misses++;
        return false;
    }

    // If prior layers were skipped, advance consumer window to matchedIdx so worker can progress
    if (matchedIdx > currentConsumer) {
        for (size_t skip = currentConsumer; skip < matchedIdx; ++skip) {
            s_slots[skip % kRingSlots].readyIndex.store(kInvalidIndex, std::memory_order_release);
            s_slots[skip % kRingSlots].failedIndex.store(kInvalidIndex, std::memory_order_release);
        }
        s_consumerIndex.store(matchedIdx, std::memory_order_release);
        if (s_hSlotConsumedEvent) SetEvent(s_hSlotConsumedEvent);
    }

    const size_t targetSlot = matchedIdx % kRingSlots;
    RingSlot& slot = s_slots[targetSlot];

    // Spin briefly if worker is just finishing writing
    int spin = 0;
    while (slot.readyIndex.load(std::memory_order_acquire) != matchedIdx &&
           slot.failedIndex.load(std::memory_order_acquire) != matchedIdx &&
           s_isSectorActive.load(std::memory_order_acquire) &&
           spin < 2000) {
        YieldProcessor();
        spin++;
    }
    for (uint32_t w = 0;
         w < s_waitTimeoutMs &&
         slot.readyIndex.load(std::memory_order_acquire) != matchedIdx &&
         slot.failedIndex.load(std::memory_order_acquire) != matchedIdx &&
         s_isSectorActive.load(std::memory_order_acquire);
         ++w) {
        Sleep(1);
    }

    if (slot.readyIndex.load(std::memory_order_acquire) == matchedIdx) {
        if (outCapacity >= slot.uncompSize && slot.uncompSize == s_entries[matchedIdx].uncompSize) {
            memcpy(outBuffer, slot.pData, slot.uncompSize);
            if (pOutWritten) *pOutWritten = slot.uncompSize;

            slot.readyIndex.store(kInvalidIndex, std::memory_order_release);
            s_consumerIndex.store(matchedIdx + 1, std::memory_order_release);
            if (s_hSlotConsumedEvent) SetEvent(s_hSlotConsumedEvent);

            s_stats.hits++;
            s_stats.bytesServed += slot.uncompSize;
            return true;
        }
    }

    // Slot failed or timed out for this confirmed layer; advance consumer index past it and fall back safely
    slot.readyIndex.store(kInvalidIndex, std::memory_order_release);
    slot.failedIndex.store(kInvalidIndex, std::memory_order_release);
    s_consumerIndex.store(matchedIdx + 1, std::memory_order_release);
    if (s_hSlotConsumedEvent) SetEvent(s_hSlotConsumedEvent);

    s_stats.misses++;
    return false;
}

bool LayerRingArena_IsActive() {
    return s_isSectorActive.load(std::memory_order_relaxed);
}

LayerRingArenaStats LayerRingArena_GetStats() {
    ScopedArenaLock lock;
    return s_stats;
}

} // namespace G3O
