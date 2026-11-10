#include "layer_ring_arena.h"
#include "config_manager.h"
#include "diagnostic_logger.h"
#include "decompressor_avx2.h"
#include <libdeflate.h>
#include <vector>
#include <string>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdio>

namespace G3O {

struct LayerNodeEntry {
    uint64_t fileOffset = 0;
    uint32_t compSize = 0;
    uint32_t uncompSize = 0;
    uint32_t first4 = 0;
    char name[64] = {0};
};

struct RingSlot {
    std::atomic<bool> isReady{false};
    uint64_t fileOffset = 0;
    uint32_t compSize = 0;
    uint32_t uncompSize = 0;
    uint32_t first4 = 0;
    uint8_t* pData = nullptr;
};

static constexpr size_t kRingSlots = 4;
static constexpr size_t kSlotSize = 1024 * 1024; // 1 MB per slot (max uncompressed node in RB_Mod is 877 KB)

static std::vector<LayerNodeEntry> s_entries;
static std::string s_archivePath;

static RingSlot s_slots[kRingSlots];
static uint8_t* s_ringMemory = nullptr;

static HANDLE s_workerThread = NULL;
static HANDLE s_hStartEvent = NULL;
static HANDLE s_hSlotConsumedEvent = NULL;
static HANDLE s_hStopEvent = NULL;

static std::atomic<bool> s_isSectorActive{false};
static std::atomic<bool> s_shouldExit{false};
static size_t s_consumerIndex = 0;
static size_t s_nextWorkerIndex = 0;

static LayerRingArenaStats s_stats;

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

    uint32_t fat_off = *reinterpret_cast<const uint32_t*>(hdr + 24);
    LARGE_INTEGER fsize = {};
    if (!GetFileSizeEx(hFile, &fsize) || fat_off >= static_cast<uint64_t>(fsize.QuadPart)) {
        CloseHandle(hFile);
        return false;
    }

    uint64_t fat_size = fsize.QuadPart - fat_off;
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
        !ReadFile(hFile, pFat, static_cast<DWORD>(fat_size), &bytesRead, NULL) || bytesRead == 0) {
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
        size_t secdatOffset = static_cast<size_t>(-1);
    };
    std::vector<RawEntry> rawList;

    uint64_t secdatOff = 0;
    uint32_t secdatCompSize = 0;
    uint32_t secdatUncompSize = 0;

    size_t pos = 0;
    while (pos + 40 <= totalBytes) {
        if (pos + targetSubstrLen <= totalBytes && memcmp(pFat + pos, targetSubstr, targetSubstrLen) == 0) {
            size_t nameEnd = pos;
            while (nameEnd < totalBytes && pFat[nameEnd] != '\0') {
                nameEnd++;
            }
            if (nameEnd < totalBytes && nameEnd - pos >= 5 && pos >= 40) {
                const BYTE* pHdr = pFat + pos - 40;
                uint32_t flags = *reinterpret_cast<const uint32_t*>(pHdr);
                uint64_t off   = *reinterpret_cast<const uint64_t*>(pHdr + 4);
                uint64_t c_sz  = *reinterpret_cast<const uint64_t*>(pHdr + 12);
                uint64_t u_sz  = *reinterpret_cast<const uint64_t*>(pHdr + 20);

                const char* ext = reinterpret_cast<const char*>(pFat + nameEnd - 5);
                if (_stricmp(ext, ".node") == 0) {
                    if ((flags & 0x00020020) == 0x00020020 && c_sz > 0 && c_sz < kSlotSize && u_sz > 0 && u_sz < kSlotSize) {
                        RawEntry re;
                        re.fileOffset = off;
                        re.compSize = static_cast<uint32_t>(c_sz);
                        re.uncompSize = static_cast<uint32_t>(u_sz);
                        const char* slash = strrchr(reinterpret_cast<const char*>(pFat + pos), '/');
                        const char* baseName = slash ? (slash + 1) : reinterpret_cast<const char*>(pFat + pos);
                        snprintf(re.name, sizeof(re.name), "%s", baseName);
                        rawList.push_back(re);
                    }
                } else if (_stricmp(reinterpret_cast<const char*>(pFat + pos), "G3_World_01/RB_Mod/RB_Mod.secdat") == 0) {
                    secdatOff = off;
                    secdatCompSize = static_cast<uint32_t>(c_sz);
                    secdatUncompSize = static_cast<uint32_t>(u_sz);
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
    if (secdatOff > 0 && secdatCompSize > 0 && secdatUncompSize > 0 && secdatUncompSize < 64 * 1024) {
        std::vector<BYTE> compSecdat(secdatCompSize);
        std::vector<BYTE> decompSecdat(secdatUncompSize);
        LARGE_INTEGER liSec;
        liSec.QuadPart = secdatOff;
        DWORD r = 0;
        if (SetFilePointerEx(hFile, liSec, NULL, FILE_BEGIN) &&
            ReadFile(hFile, compSecdat.data(), secdatCompSize, &r, NULL) && r == secdatCompSize) {
            libdeflate_decompressor* dec = libdeflate_alloc_decompressor();
            if (dec) {
                size_t actualSec = 0;
                if (libdeflate_zlib_decompress(dec, compSecdat.data(), secdatCompSize, decompSecdat.data(), secdatUncompSize, &actualSec) == 0) {
                    const std::string secdatStr(reinterpret_cast<const char*>(decompSecdat.data()), actualSec);
                    for (auto& re : rawList) {
                        char stem[64] = {0};
                        snprintf(stem, sizeof(stem), "%s", re.name);
                        char* dot = strrchr(stem, '.');
                        if (dot) *dot = '\0';
                        size_t found = secdatStr.find(stem);
                        if (found != std::string::npos) {
                            re.secdatOffset = found;
                        }
                    }
                    std::sort(rawList.begin(), rawList.end(), [](const RawEntry& a, const RawEntry& b) {
                        return a.secdatOffset < b.secdatOffset;
                    });
                }
                libdeflate_free_decompressor(dec);
            }
        }
    }

    // Now populate s_entries with signatures
    for (const auto& re : rawList) {
        LARGE_INTEGER liBlock;
        liBlock.QuadPart = re.fileOffset;
        if (SetFilePointerEx(hFile, liBlock, NULL, FILE_BEGIN)) {
            DWORD read4 = 0;
            uint32_t first4 = 0;
            if (ReadFile(hFile, &first4, 4, &read4, NULL) && read4 == 4) {
                LayerNodeEntry entry;
                entry.fileOffset = re.fileOffset;
                entry.compSize = re.compSize;
                entry.uncompSize = re.uncompSize;
                entry.first4 = first4;
                snprintf(entry.name, sizeof(entry.name), "%s", re.name);
                s_entries.push_back(entry);
            }
        }
    }

    CloseHandle(hFile);
    return !s_entries.empty();
}

static DWORD WINAPI LayerRingArena_WorkerThread(LPVOID /*lpParam*/) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    libdeflate_decompressor* dec = nullptr;
    if (G3O_AVX2_IsSupported()) {
        dec = G3O_AVX2_AllocDecompressor();
    } else {
        dec = libdeflate_alloc_decompressor();
    }
    if (!dec) return 1;

    std::vector<BYTE> compBuf(512 * 1024);
    HANDLE waitHandles[2] = { s_hStopEvent, s_hStartEvent };

    while (!s_shouldExit.load(std::memory_order_relaxed)) {
        DWORD waitRes = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
        if (waitRes == WAIT_OBJECT_0 || s_shouldExit.load(std::memory_order_relaxed)) {
            break; // Stop signaled
        }

        if (!s_isSectorActive.load(std::memory_order_acquire)) {
            continue;
        }

        HANDLE hArchive = CreateFileA(s_archivePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (hArchive == INVALID_HANDLE_VALUE) {
            continue;
        }

        while (s_isSectorActive.load(std::memory_order_relaxed) && !s_shouldExit.load(std::memory_order_relaxed)) {
            if (s_nextWorkerIndex >= s_entries.size()) {
                // All entries decompressed for this sector; wait for sector completion
                HANDLE consumerWait[2] = { s_hStopEvent, s_hSlotConsumedEvent };
                WaitForMultipleObjects(2, consumerWait, FALSE, 25);
                if (!s_isSectorActive.load(std::memory_order_relaxed)) {
                    break;
                }
                continue;
            }

            const size_t targetSlot = s_nextWorkerIndex % kRingSlots;

            // Check if slot is ready to receive
            if (s_slots[targetSlot].isReady.load(std::memory_order_relaxed)) {
                // Ring buffer is full; wait for consumer to free a slot
                HANDLE consumerWait[2] = { s_hStopEvent, s_hSlotConsumedEvent };
                DWORD cRes = WaitForMultipleObjects(2, consumerWait, FALSE, 25);
                if (cRes == WAIT_OBJECT_0 || !s_isSectorActive.load(std::memory_order_relaxed)) {
                    break;
                }
                continue;
            }

            const auto& entry = s_entries[s_nextWorkerIndex];
            if (entry.compSize > compBuf.size()) {
                compBuf.resize(entry.compSize);
            }

            LARGE_INTEGER li;
            li.QuadPart = entry.fileOffset;
            DWORD bytesRead = 0;
            if (SetFilePointerEx(hArchive, li, NULL, FILE_BEGIN) &&
                ReadFile(hArchive, compBuf.data(), entry.compSize, &bytesRead, NULL) &&
                bytesRead == entry.compSize) {

                size_t actualOut = 0;
                int res = -1;
                if (G3O_AVX2_IsSupported()) {
                    res = G3O_AVX2_DecompressZlib(dec, compBuf.data(), entry.compSize, s_slots[targetSlot].pData, kSlotSize, &actualOut);
                } else {
                    res = libdeflate_zlib_decompress(dec, compBuf.data(), entry.compSize, s_slots[targetSlot].pData, kSlotSize, &actualOut);
                }

                if (res == 0 && actualOut > 0) {
                    s_slots[targetSlot].fileOffset = entry.fileOffset;
                    s_slots[targetSlot].compSize = entry.compSize;
                    s_slots[targetSlot].uncompSize = static_cast<uint32_t>(actualOut);
                    s_slots[targetSlot].first4 = entry.first4;
                    s_slots[targetSlot].isReady.store(true, std::memory_order_release);

                    s_nextWorkerIndex++;
                } else {
                    s_nextWorkerIndex++;
                }
            } else {
                s_nextWorkerIndex++;
            }
        }

        CloseHandle(hArchive);
    }

    if (G3O_AVX2_IsSupported()) {
        G3O_AVX2_FreeDecompressor(dec);
    } else {
        libdeflate_free_decompressor(dec);
    }
    return 0;
}

void InitializeLayerRingArena() {
    if (!GetConfig().enableLayerRingArena) {
        return;
    }

    s_archivePath = FindArchivePath("Data\\Projects_compiled.m17");
    if (s_archivePath.empty()) {
        Log("G3O", "[LayerRingArena] Notice: Data\\Projects_compiled.m17 not found (RB_Mod not active). Ring Arena idle.");
        return;
    }

    if (!IndexArchiveLayers(s_archivePath)) {
        Log("G3O", "[LayerRingArena] Notice: No .node layers found in %s.", s_archivePath.c_str());
        return;
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
        return;
    }

    for (size_t i = 0; i < kRingSlots; ++i) {
        s_slots[i].isReady.store(false, std::memory_order_relaxed);
        s_slots[i].compSize = 0;
        s_slots[i].first4 = 0;
        s_slots[i].pData = s_ringMemory + (i * kSlotSize);
    }

    s_hStartEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
    s_hSlotConsumedEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
    s_hStopEvent = CreateEventA(NULL, FALSE, FALSE, NULL);

    s_shouldExit.store(false, std::memory_order_relaxed);
    s_isSectorActive.store(false, std::memory_order_relaxed);

    s_workerThread = CreateThread(NULL, 0, LayerRingArena_WorkerThread, NULL, 0, NULL);
    if (s_workerThread) {
        Log("G3O", "[LayerRingArena] Sliding-Window Ring Arena active: 4 slots x 1 MB (4 MB VAS budget), AVX2 background decompressor ready.");
    }
}

void ShutdownLayerRingArena() {
    s_shouldExit.store(true, std::memory_order_release);
    s_isSectorActive.store(false, std::memory_order_release);

    if (s_hStopEvent) SetEvent(s_hStopEvent);

    if (s_workerThread) {
        WaitForSingleObject(s_workerThread, 1000);
        CloseHandle(s_workerThread);
        s_workerThread = NULL;
    }

    if (s_hStartEvent) { CloseHandle(s_hStartEvent); s_hStartEvent = NULL; }
    if (s_hSlotConsumedEvent) { CloseHandle(s_hSlotConsumedEvent); s_hSlotConsumedEvent = NULL; }
    if (s_hStopEvent) { CloseHandle(s_hStopEvent); s_hStopEvent = NULL; }

    if (s_ringMemory) {
        VirtualFree(s_ringMemory, 0, MEM_RELEASE);
        s_ringMemory = nullptr;
    }

    s_entries.clear();
}

void LayerRingArena_OnSectorStart(const char* sectorName) {
    if (!GetConfig().enableLayerRingArena || s_entries.empty() || !s_workerThread) {
        return;
    }

    if (sectorName && (strcmp(sectorName, "RB_Mod") == 0 || strstr(sectorName, "RB_Mod") != nullptr)) {
        for (size_t i = 0; i < kRingSlots; ++i) {
            s_slots[i].isReady.store(false, std::memory_order_relaxed);
            s_slots[i].compSize = 0;
            s_slots[i].first4 = 0;
        }
        s_consumerIndex = 0;
        s_nextWorkerIndex = 0;
        s_stats = {};

        s_isSectorActive.store(true, std::memory_order_release);
        SetEvent(s_hStartEvent);

        Log("G3O", "[LayerRingArena] Started pre-decompression ring pipeline for sector '%s' (%zu layers).", sectorName, s_entries.size());
    }
}

void LayerRingArena_OnSectorEnd() {
    if (!s_isSectorActive.load(std::memory_order_relaxed)) {
        return;
    }

    s_isSectorActive.store(false, std::memory_order_release);
    if (s_hSlotConsumedEvent) SetEvent(s_hSlotConsumedEvent);

    Log("G3O", "[LayerRingArena] Finished sector: %u / %zu layers served from Ring Arena (%.2f MB zero-overhead decomp, %u misses).",
        s_stats.hits, s_entries.size(),
        static_cast<double>(s_stats.bytesServed) / (1024.0 * 1024.0),
        s_stats.misses);
}

bool LayerRingArena_TryGet(
    const void* inBuffer,
    DWORD inSize,
    void* outBuffer,
    DWORD outCapacity,
    DWORD* pOutWritten
) {
    if (!s_isSectorActive.load(std::memory_order_relaxed) || !inBuffer || inSize < 1000 || !outBuffer || outCapacity == 0) {
        return false;
    }

    const uint32_t inFirst4 = *reinterpret_cast<const uint32_t*>(inBuffer);

    // Identify entry index in s_entries
    size_t matchedIdx = static_cast<size_t>(-1);
    if (s_consumerIndex < s_entries.size()) {
        if (s_entries[s_consumerIndex].compSize == inSize && s_entries[s_consumerIndex].first4 == inFirst4) {
            matchedIdx = s_consumerIndex;
        } else {
            // Check ahead in sliding window (up to kRingSlots)
            const size_t maxAhead = (s_consumerIndex + kRingSlots < s_entries.size()) ? (s_consumerIndex + kRingSlots) : s_entries.size();
            for (size_t a = s_consumerIndex + 1; a < maxAhead; ++a) {
                if (s_entries[a].compSize == inSize && s_entries[a].first4 == inFirst4) {
                    matchedIdx = a;
                    break;
                }
            }
        }
    }

    if (matchedIdx == static_cast<size_t>(-1)) {
        // Not a monitored layer block in the active window
        return false;
    }

    // If layers were skipped, free their ring slots so worker does not stall
    if (matchedIdx > s_consumerIndex) {
        for (size_t skip = s_consumerIndex; skip < matchedIdx; ++skip) {
            s_slots[skip % kRingSlots].isReady.store(false, std::memory_order_release);
        }
        if (s_hSlotConsumedEvent) SetEvent(s_hSlotConsumedEvent);
    }

    const size_t targetSlot = matchedIdx % kRingSlots;
    RingSlot& slot = s_slots[targetSlot];

    // Spin briefly if worker is just finishing writing
    int spin = 0;
    while (!slot.isReady.load(std::memory_order_acquire) && spin < 2000) {
        YieldProcessor();
        spin++;
    }
    for (int w = 0; w < 10 && !slot.isReady.load(std::memory_order_acquire); ++w) {
        Sleep(1);
    }

    if (slot.isReady.load(std::memory_order_acquire)) {
        if (outCapacity >= slot.uncompSize) {
            memcpy(outBuffer, slot.pData, slot.uncompSize);
            if (pOutWritten) *pOutWritten = slot.uncompSize;

            slot.isReady.store(false, std::memory_order_release);
            if (s_hSlotConsumedEvent) SetEvent(s_hSlotConsumedEvent);

            s_consumerIndex = matchedIdx + 1;
            s_stats.hits++;
            s_stats.bytesServed += slot.uncompSize;
            return true;
        }
    }

    // If slot was not ready in time, fall back safely
    s_stats.misses++;
    return false;
}

bool LayerRingArena_IsActive() {
    return s_isSectorActive.load(std::memory_order_relaxed);
}

LayerRingArenaStats LayerRingArena_GetStats() {
    return s_stats;
}

} // namespace G3O
