#include "pak_streaming_ring.h"
#include "pak_content_key.h"
#include "diagnostic_logger.h"
#include <libdeflate.h>
#include <atomic>
#include <vector>
#include <string>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <memory>
#include <immintrin.h>

namespace G3O {

namespace {

constexpr uint32_t kSlotCount = 32;
constexpr uint32_t kSlotCapacity = 2 * 1024 * 1024; // 2 MB per slot (covers >99.5% of blocks)
constexpr uint32_t kArenaTotalBytes = kSlotCount * kSlotCapacity; // Strictly 64 MB (<= 88 MB safe VAS)
constexpr uint32_t kDefaultWorkerCount = 4;
constexpr uint32_t kMaxQueueTasks = 48;
constexpr size_t   kLookaheadSteps = 12;

enum SlotState : uint32_t {
    SLOT_EMPTY = 0,
    SLOT_CLAIMED = 1,
    SLOT_READY = 2,
    SLOT_CONSUMING = 3
};

struct PakRingSlot {
    std::atomic<uint32_t> state{SLOT_EMPTY};
    std::atomic<uint64_t> contentKey{0};
    uint64_t fileOffset{0};
    uint32_t archiveHash{0};
    uint32_t outCapacity{0};
    uint32_t actualOut{0};
    uint8_t* buffer{nullptr};
};

struct PakBlockMeta {
    uint64_t fileOffset;
    uint32_t compressedSize;
    uint32_t uncompressedSize;
};

struct ArchiveRecord {
    std::string path;
    uint32_t archiveHash{0};
    std::shared_ptr<const std::vector<PakBlockMeta>> blocks; // Immutable sorted blocks
};

struct LookaheadTask {
    std::string archivePath;
    uint32_t archiveHash{0};
    uint64_t fileOffset{0};
    uint32_t compressedSize{0};
    uint32_t uncompressedSize{0};
};

// Global arena & slot state
static uint8_t* s_arenaBase = nullptr;
static PakRingSlot s_slots[kSlotCount];
static std::atomic<uint64_t> s_lastReadOffset{0};
static std::atomic<uint32_t> s_ringWriteHead{0};

// Performance counter frequency
static LARGE_INTEGER s_timerFreq = []() {
    LARGE_INTEGER li;
    QueryPerformanceFrequency(&li);
    if (li.QuadPart <= 0) li.QuadPart = 10000000LL;
    return li;
}();

// Archive metadata cache
static std::mutex s_archiveMutex;
static std::unordered_map<HANDLE, ArchiveRecord> s_archiveHandles;
static std::unordered_map<std::string, std::shared_ptr<const std::vector<PakBlockMeta>>> s_archiveBlocksByPath;

// Task queue & worker threads
static std::mutex s_queueMutex;
static std::condition_variable s_queueCv;
static std::condition_variable s_slotCv;
static std::vector<LookaheadTask> s_taskQueue;
static std::vector<std::thread> s_workers;
static std::atomic<bool> s_workersRunning{false};
static std::atomic<bool> s_initialized{false};

// Telemetry counters
static volatile LONG64 s_hits = 0;
static volatile LONG64 s_misses = 0;
static volatile LONG64 s_producedBlocks = 0;
static volatile LONG64 s_consumedBytes = 0;

static uint32_t HashPath(const std::string& str) noexcept {
    uint32_t h = 2166136261u;
    for (char c : str) {
        h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
    }
    return h;
}

// Claim a slot: prefer empty slots, then stale slots behind read cursor, then rotating head eviction
static uint32_t ClaimSlotForWorker(uint32_t archiveHash, uint64_t targetOffset) {
    (void)archiveHash;
    (void)targetOffset;
    // 1. Try to find a truly EMPTY slot first
    for (uint32_t i = 0; i < kSlotCount; ++i) {
        uint32_t expected = SLOT_EMPTY;
        if (s_slots[i].state.compare_exchange_strong(expected, SLOT_CLAIMED,
                                                    std::memory_order_acq_rel)) {
            return i;
        }
    }

    // 2. Try to find an unconsumed slot that is BEHIND the read cursor (already passed by game)
    const uint64_t lastRead = s_lastReadOffset.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < kSlotCount; ++i) {
        if (s_slots[i].state.load(std::memory_order_relaxed) == SLOT_READY) {
            if (s_slots[i].fileOffset < lastRead) {
                uint32_t expected = SLOT_READY;
                if (s_slots[i].state.compare_exchange_strong(expected, SLOT_CLAIMED,
                                                            std::memory_order_acq_rel)) {
                    s_slots[i].contentKey.store(0, std::memory_order_relaxed);
                    return i;
                }
            }
        }
    }

    // 3. Fallback: Rotating head eviction of oldest SLOT_READY (ensures ring NEVER starves or deadlocks)
    for (uint32_t attempt = 0; attempt < kSlotCount; ++attempt) {
        uint32_t slot = s_ringWriteHead.fetch_add(1, std::memory_order_relaxed) % kSlotCount;
        uint32_t expected = SLOT_READY;
        if (s_slots[slot].state.compare_exchange_strong(expected, SLOT_CLAIMED,
                                                        std::memory_order_acq_rel)) {
            s_slots[slot].contentKey.store(0, std::memory_order_relaxed);
            return slot;
        }
    }

    return kSlotCount;
}

// Parse the archive FAT table to extract all compressed block metadata
bool ParseArchiveFat(const char* path, std::vector<PakBlockMeta>& outBlocks) {
    if (!path || !path[0]) return false;

    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    BYTE hdr[32];
    DWORD readBytes = 0;
    if (!ReadFile(hFile, hdr, 32, &readBytes, NULL) || readBytes != 32) {
        CloseHandle(hFile);
        return false;
    }

    if (std::memcmp(hdr + 4, "G3V0", 4) != 0) {
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
    if (fat_size == 0 || fat_size > 64 * 1024 * 1024) {
        CloseHandle(hFile);
        return false;
    }

    BYTE* pFat = reinterpret_cast<BYTE*>(VirtualAlloc(NULL, static_cast<SIZE_T>(fat_size),
                                                      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!pFat) {
        CloseHandle(hFile);
        return false;
    }

    LARGE_INTEGER liFat;
    liFat.QuadPart = fat_off;
    if (!SetFilePointerEx(hFile, liFat, NULL, FILE_BEGIN) ||
        !ReadFile(hFile, pFat, static_cast<DWORD>(fat_size), &readBytes, NULL) ||
        readBytes == 0) {
        VirtualFree(pFat, 0, MEM_RELEASE);
        CloseHandle(hFile);
        return false;
    }
    CloseHandle(hFile);

    // Scan for compressed block entries using the 8-byte flags pattern
    const size_t totalBytes = static_cast<size_t>(readBytes);
    const BYTE pattern[] = { 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00 };
    size_t pos = 0;

    outBlocks.clear();
    outBlocks.reserve(4096);

    while (pos + 32 <= totalBytes) {
        const void* found = std::memchr(pFat + pos, 0x00, totalBytes - pos);
        if (!found) break;
        size_t idx = static_cast<const BYTE*>(found) - pFat;
        if (idx + 8 <= totalBytes && std::memcmp(pFat + idx, pattern, 8) == 0) {
            if (idx >= 24) {
                uint64_t off = *reinterpret_cast<const uint64_t*>(pFat + idx - 24);
                uint64_t c_sz = *reinterpret_cast<const uint64_t*>(pFat + idx - 16);
                uint64_t u_sz = *reinterpret_cast<const uint64_t*>(pFat + idx - 8);

                if (off > 0 && off < fat_off && c_sz > 0 && c_sz < 0x20000000 &&
                    u_sz > 0 && u_sz <= kSlotCapacity) {
                    outBlocks.push_back({ off, static_cast<uint32_t>(c_sz), static_cast<uint32_t>(u_sz) });
                }
            }
            pos = idx + 8;
        } else {
            pos = idx + 1;
        }
    }

    VirtualFree(pFat, 0, MEM_RELEASE);

    std::sort(outBlocks.begin(), outBlocks.end(), [](const PakBlockMeta& a, const PakBlockMeta& b) {
        return a.fileOffset < b.fileOffset;
    });

    return !outBlocks.empty();
}

// Background worker thread execution loop
void WorkerThreadFunc() {
    struct libdeflate_decompressor* dec = libdeflate_alloc_decompressor();
    if (!dec) return;

    std::unordered_map<std::string, HANDLE> localHandles;
    uint8_t* readBuffer = reinterpret_cast<uint8_t*>(VirtualAlloc(NULL, kSlotCapacity,
                                                                 MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!readBuffer) {
        libdeflate_free_decompressor(dec);
        return;
    }

    while (s_workersRunning.load(std::memory_order_relaxed)) {
        LookaheadTask task;
        {
            std::unique_lock<std::mutex> lock(s_queueMutex);
            s_queueCv.wait(lock, [] {
                return !s_workersRunning.load(std::memory_order_relaxed) || !s_taskQueue.empty();
            });
            if (!s_workersRunning.load(std::memory_order_relaxed)) break;
            if (s_taskQueue.empty()) continue;
            task = std::move(s_taskQueue.front());
            s_taskQueue.erase(s_taskQueue.begin());
        }

        // 2. Claim a slot using rotating head and stale eviction
        uint32_t targetSlot = ClaimSlotForWorker(task.archiveHash, task.fileOffset);
        if (targetSlot == kSlotCount) {
            continue;
        }

        // 3. Open or get local handle for this archive
        HANDLE hArchive = INVALID_HANDLE_VALUE;
        auto itHandle = localHandles.find(task.archivePath);
        if (itHandle != localHandles.end()) {
            hArchive = itHandle->second;
        } else {
            hArchive = CreateFileA(task.archivePath.c_str(), GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hArchive != INVALID_HANDLE_VALUE) {
                localHandles[task.archivePath] = hArchive;
            }
        }

        if (hArchive == INVALID_HANDLE_VALUE) {
            s_slots[targetSlot].state.store(SLOT_EMPTY, std::memory_order_release);
            s_slotCv.notify_one();
            continue;
        }

        // 4. Seek to fileOffset and read compressed block
        LARGE_INTEGER liOffset;
        liOffset.QuadPart = task.fileOffset;
        if (!SetFilePointerEx(hArchive, liOffset, NULL, FILE_BEGIN)) {
            s_slots[targetSlot].state.store(SLOT_EMPTY, std::memory_order_release);
            s_slotCv.notify_one();
            continue;
        }

        DWORD bytesRead = 0;
        if (!ReadFile(hArchive, readBuffer, task.compressedSize, &bytesRead, NULL) ||
            bytesRead != task.compressedSize) {
            s_slots[targetSlot].state.store(SLOT_EMPTY, std::memory_order_release);
            s_slotCv.notify_one();
            continue;
        }

        // 5. Compute hardware SSE4.2 CRC32C content key
        uint64_t key = ComputeBlockContentKey(readBuffer, bytesRead, task.uncompressedSize);
        if (key == 0) {
            s_slots[targetSlot].state.store(SLOT_EMPTY, std::memory_order_release);
            s_slotCv.notify_one();
            continue;
        }

        // 6. Early publication of key & metadata while in SLOT_CLAIMED state
        s_slots[targetSlot].fileOffset = task.fileOffset;
        s_slots[targetSlot].archiveHash = task.archiveHash;
        s_slots[targetSlot].outCapacity = task.uncompressedSize;
        s_slots[targetSlot].actualOut = 0;
        s_slots[targetSlot].contentKey.store(key, std::memory_order_release);

        // 7. Decompress directly into slot buffer
        size_t actualOut = 0;
        int res = libdeflate_zlib_decompress(dec, readBuffer, bytesRead,
                                            s_slots[targetSlot].buffer, task.uncompressedSize, &actualOut);
        if (res != 0 && bytesRead >= 6) {
            res = libdeflate_deflate_decompress(dec, readBuffer + 2, bytesRead - 6,
                                                s_slots[targetSlot].buffer, task.uncompressedSize, &actualOut);
        }

        if (res == 0 && actualOut > 0) {
            s_slots[targetSlot].actualOut = static_cast<uint32_t>(actualOut);
            // Atomically publish as SLOT_READY only after full decompression is complete
            s_slots[targetSlot].state.store(SLOT_READY, std::memory_order_release);
            InterlockedIncrement64(&s_producedBlocks);
        } else {
            s_slots[targetSlot].contentKey.store(0, std::memory_order_relaxed);
            s_slots[targetSlot].fileOffset = 0;
            s_slots[targetSlot].archiveHash = 0;
            s_slots[targetSlot].state.store(SLOT_EMPTY, std::memory_order_release);
            s_slotCv.notify_one();
        }
    }

    // Cleanup worker resources
    for (auto& pair : localHandles) {
        if (pair.second != INVALID_HANDLE_VALUE) {
            CloseHandle(pair.second);
        }
    }
    localHandles.clear();

    if (readBuffer) {
        VirtualFree(readBuffer, 0, MEM_RELEASE);
    }
    libdeflate_free_decompressor(dec);
}

} // namespace

bool InitializePakStreamingRing(uint32_t workerCount) {
    if (s_initialized.load(std::memory_order_relaxed)) return true;

    if (workerCount == 0) {
        unsigned int hw = std::thread::hardware_concurrency();
        if (hw <= 2) {
            workerCount = 1;
        } else if (hw <= 4) {
            workerCount = 2;
        } else {
            workerCount = kDefaultWorkerCount;
        }
    } else {
        workerCount = std::clamp(workerCount, 1u, 16u);
    }

    // Allocate bounded 64 MB arena
    s_arenaBase = reinterpret_cast<uint8_t*>(VirtualAlloc(NULL, kArenaTotalBytes,
                                                          MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!s_arenaBase) {
        Log("G3O", "[PakStreamingRing] ERROR: Failed to allocate 64 MB virtual arena!");
        return false;
    }

    for (uint32_t i = 0; i < kSlotCount; ++i) {
        s_slots[i].state.store(SLOT_EMPTY, std::memory_order_relaxed);
        s_slots[i].contentKey.store(0, std::memory_order_relaxed);
        s_slots[i].fileOffset = 0;
        s_slots[i].archiveHash = 0;
        s_slots[i].outCapacity = 0;
        s_slots[i].actualOut = 0;
        s_slots[i].buffer = s_arenaBase + (i * kSlotCapacity);
    }

    s_lastReadOffset.store(0, std::memory_order_relaxed);
    s_ringWriteHead.store(0, std::memory_order_relaxed);

    // Start worker threads
    s_workersRunning.store(true, std::memory_order_relaxed);
    s_workers.reserve(workerCount);
    for (uint32_t w = 0; w < workerCount; ++w) {
        s_workers.emplace_back(WorkerThreadFunc);
    }

    s_initialized.store(true, std::memory_order_release);
    Log("G3O", "[PakStreamingRing] Initialized: 64 MB bounded arena (32 slots x 2 MB), %u background workers active (lookahead=%zu).",
        workerCount, kLookaheadSteps);
    return true;
}

void ShutdownPakStreamingRing() {
    if (!s_initialized.load(std::memory_order_relaxed)) return;

    s_workersRunning.store(false, std::memory_order_release);
    s_queueCv.notify_all();
    s_slotCv.notify_all();

    for (auto& t : s_workers) {
        if (t.joinable()) {
            t.join();
        }
    }
    s_workers.clear();

    {
        std::lock_guard<std::mutex> lock(s_queueMutex);
        s_taskQueue.clear();
    }

    {
        std::lock_guard<std::mutex> lock(s_archiveMutex);
        s_archiveHandles.clear();
        s_archiveBlocksByPath.clear();
    }

    if (s_arenaBase) {
        VirtualFree(s_arenaBase, 0, MEM_RELEASE);
        s_arenaBase = nullptr;
    }

    s_initialized.store(false, std::memory_order_release);
    Log("G3O", "[PakStreamingRing] Shutdown: Hits=%lld, Misses=%lld, Produced=%lld, Consumed=%.2f MB",
        s_hits, s_misses, s_producedBlocks, (double)s_consumedBytes / (1024.0 * 1024.0));
}

void PakStreamingRing_OnBlockRead(HANDLE hFile, uint64_t fileOffset, uint32_t compressedBytes) {
    if (!s_initialized.load(std::memory_order_relaxed) || hFile == INVALID_HANDLE_VALUE || compressedBytes == 0) {
        return;
    }

    s_lastReadOffset.store(fileOffset, std::memory_order_relaxed);

    std::shared_ptr<const std::vector<PakBlockMeta>> pBlocks;
    std::string archivePath;
    uint32_t archiveHash = 0;
    {
        std::lock_guard<std::mutex> lock(s_archiveMutex);
        auto it = s_archiveHandles.find(hFile);
        if (it != s_archiveHandles.end()) {
            pBlocks = it->second.blocks;
            archivePath = it->second.path;
            archiveHash = it->second.archiveHash;
        } else {
            char rawPath[MAX_PATH] = { 0 };
            DWORD len = GetFinalPathNameByHandleA(hFile, rawPath, sizeof(rawPath), FILE_NAME_NORMALIZED);
            if (len > 0 && len < sizeof(rawPath)) {
                const char* actualPath = rawPath;
                if (std::strncmp(actualPath, "\\\\?\\", 4) == 0) {
                    actualPath += 4;
                }

                const char* dot = std::strrchr(actualPath, '.');
                if (dot) {
                    uint32_t hash = HashPath(actualPath);
                    auto pathHit = s_archiveBlocksByPath.find(actualPath);
                    if (pathHit != s_archiveBlocksByPath.end()) {
                        ArchiveRecord rec;
                        rec.path = actualPath;
                        rec.archiveHash = hash;
                        rec.blocks = pathHit->second;
                        pBlocks = rec.blocks;
                        archivePath = rec.path;
                        archiveHash = hash;
                        s_archiveHandles.emplace(hFile, std::move(rec));
                    } else {
                        auto newBlocks = std::make_shared<std::vector<PakBlockMeta>>();
                        if (ParseArchiveFat(actualPath, *newBlocks)) {
                            s_archiveBlocksByPath[actualPath] = newBlocks;
                            ArchiveRecord rec;
                            rec.path = actualPath;
                            rec.archiveHash = hash;
                            rec.blocks = newBlocks;
                            pBlocks = rec.blocks;
                            archivePath = rec.path;
                            archiveHash = hash;
                            s_archiveHandles.emplace(hFile, std::move(rec));
                        } else {
                            // Non-archive file: cache null blocks to skip future GetFinalPathNameByHandleA calls
                            ArchiveRecord rec;
                            rec.path = actualPath;
                            rec.archiveHash = hash;
                            rec.blocks = nullptr;
                            s_archiveHandles.emplace(hFile, std::move(rec));
                        }
                    }
                }
            }
        }
    }

    if (!pBlocks || pBlocks->empty()) return;

    // Locate current block in sorted metadata
    auto it = std::lower_bound(pBlocks->begin(), pBlocks->end(), fileOffset,
        [](const PakBlockMeta& m, uint64_t off) { return m.fileOffset < off; });

    if (it != pBlocks->end()) {
        if (it->fileOffset != fileOffset && it != pBlocks->begin()) {
            auto prev = it - 1;
            if (fileOffset >= prev->fileOffset && fileOffset < prev->fileOffset + prev->compressedSize) {
                it = prev;
            }
        }

        // Enqueue lookahead for next kLookaheadSteps contiguous blocks
        std::lock_guard<std::mutex> lock(s_queueMutex);
        bool addedAny = false;

        for (size_t step = 1; step <= kLookaheadSteps && (it + step) != pBlocks->end(); ++step) {
            if (s_taskQueue.size() >= kMaxQueueTasks) break;

            const auto& nextBlock = *(it + step);
            if (nextBlock.uncompressedSize <= kSlotCapacity && nextBlock.compressedSize > 0) {
                // Check if already in queue
                bool alreadyQueued = false;
                for (const auto& q : s_taskQueue) {
                    if (q.fileOffset == nextBlock.fileOffset && q.archiveHash == archiveHash) {
                        alreadyQueued = true;
                        break;
                    }
                }
                if (alreadyQueued) continue;

                // Check if already in ring buffer
                bool alreadyInRing = false;
                for (uint32_t i = 0; i < kSlotCount; ++i) {
                    if (s_slots[i].fileOffset == nextBlock.fileOffset &&
                        s_slots[i].archiveHash == archiveHash &&
                        s_slots[i].state.load(std::memory_order_relaxed) != SLOT_EMPTY) {
                        alreadyInRing = true;
                        break;
                    }
                }
                if (!alreadyInRing) {
                    s_taskQueue.push_back({ archivePath, archiveHash, nextBlock.fileOffset,
                                           nextBlock.compressedSize, nextBlock.uncompressedSize });
                    addedAny = true;
                }
            }
        }
        if (addedAny) {
            s_queueCv.notify_all();
        }
    }
}

bool PakStreamingRing_TryConsume(uint64_t contentKey, void* outBuffer, uint32_t outCapacity, DWORD* pOutWritten) {
    if (contentKey == 0 || !outBuffer || outCapacity == 0) return false;

    for (uint32_t i = 0; i < kSlotCount; ++i) {
        if (s_slots[i].contentKey.load(std::memory_order_acquire) == contentKey &&
            s_slots[i].outCapacity == outCapacity) {

            uint32_t state = s_slots[i].state.load(std::memory_order_acquire);

            // In-flight decompression by worker: wait briefly (up to 500 us) for worker to complete
            if (state == SLOT_CLAIMED) {
                LARGE_INTEGER tStart, tNow;
                QueryPerformanceCounter(&tStart);
                while (state == SLOT_CLAIMED) {
                    _mm_pause();
                    state = s_slots[i].state.load(std::memory_order_acquire);
                    if (state != SLOT_CLAIMED) break;
                    QueryPerformanceCounter(&tNow);
                    if (s_timerFreq.QuadPart > 0 &&
                        ((tNow.QuadPart - tStart.QuadPart) * 1000000LL / s_timerFreq.QuadPart) >= 500) {
                        break;
                    }
                }
            }

            if (state == SLOT_READY) {
                // Atomically claim slot for consumption so NO worker can evict or overwrite it during memcpy!
                uint32_t expected = SLOT_READY;
                if (s_slots[i].state.compare_exchange_strong(expected, SLOT_CONSUMING,
                                                            std::memory_order_acq_rel)) {
                    // Exact hit: instant memcpy protected by SLOT_CONSUMING state
                    uint32_t toCopy = s_slots[i].actualOut;
                    if (toCopy > outCapacity) toCopy = outCapacity;
                    std::memcpy(outBuffer, s_slots[i].buffer, toCopy);
                    if (pOutWritten) *pOutWritten = toCopy;

                    // Release slot immediately for the next request. Zero inter-request cache!
                    s_slots[i].contentKey.store(0, std::memory_order_relaxed);
                    s_slots[i].fileOffset = 0;
                    s_slots[i].archiveHash = 0;
                    s_slots[i].outCapacity = 0;
                    s_slots[i].actualOut = 0;
                    s_slots[i].state.store(SLOT_EMPTY, std::memory_order_release);

                    InterlockedIncrement64(&s_hits);
                    InterlockedExchangeAdd64(&s_consumedBytes, toCopy);
                    s_slotCv.notify_one();
                    return true;
                }
            }
        }
    }

    InterlockedIncrement64(&s_misses);
    return false;
}

void PakStreamingRing_ResetLoadState() {
    {
        std::lock_guard<std::mutex> lock(s_queueMutex);
        s_taskQueue.clear();
    }
    for (uint32_t i = 0; i < kSlotCount; ++i) {
        s_slots[i].contentKey.store(0, std::memory_order_relaxed);
        s_slots[i].fileOffset = 0;
        s_slots[i].archiveHash = 0;
        s_slots[i].outCapacity = 0;
        s_slots[i].actualOut = 0;
        s_slots[i].state.store(SLOT_EMPTY, std::memory_order_release);
    }
    s_lastReadOffset.store(0, std::memory_order_relaxed);
    s_ringWriteHead.store(0, std::memory_order_relaxed);
    s_slotCv.notify_all();
}

LONG64 PakStreamingRing_GetHits() { return s_hits; }
LONG64 PakStreamingRing_GetMisses() { return s_misses; }
LONG64 PakStreamingRing_GetProducedBlocks() { return s_producedBlocks; }
LONG64 PakStreamingRing_GetConsumedBytes() { return s_consumedBytes; }

} // namespace G3O
