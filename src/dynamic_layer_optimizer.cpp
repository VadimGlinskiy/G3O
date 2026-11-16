#include "dynamic_layer_optimizer.h"
#include "config_manager.h"
#include "diagnostic_logger.h"
#include "pak_content_key.h"
#include "decompressor_avx2.h"
#include <libdeflate.h>
#include <windows.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <cstdio>

namespace G3O {

struct LayerTask {
    std::string archivePath;
    uint64_t fileOffset = 0;
    uint32_t compSize = 0;
    uint32_t uncompSize = 0;
    std::string name;
};

struct PredecompEntry {
    uint32_t compSize = 0;
    uint32_t uncompSize = 0;
    std::vector<uint8_t> data;
};

static constexpr size_t kNumBuckets = 16;
struct ShardedBucket {
    SRWLOCK lock = SRWLOCK_INIT;
    std::unordered_map<uint64_t, PredecompEntry> map;
};

static ShardedBucket s_buckets[kNumBuckets];
static std::vector<LayerTask> s_tasks;
static std::vector<std::thread> s_workers;
static std::atomic<bool> s_isActive{false};
static std::atomic<size_t> s_nextTaskIndex{0};
static std::atomic<uint32_t> s_statHits{0};
static std::atomic<uint32_t> s_statMisses{0};
static std::atomic<uint64_t> s_statBytesServed{0};
static bool s_initialized = false;
static SRWLOCK s_initLock = SRWLOCK_INIT;

static std::pair<int, int> GetArchivePriority(const std::string& path) {
    const char* ext = strrchr(path.c_str(), '.');
    if (!ext) return {0, 0};
    if (_strnicmp(ext, ".m", 2) == 0 && isdigit(ext[2])) {
        return {4, atoi(ext + 2)};
    }
    if (_stricmp(ext, ".mod") == 0) {
        return {3, 0};
    }
    if (_strnicmp(ext, ".p", 2) == 0 && isdigit(ext[2])) {
        return {2, atoi(ext + 2)};
    }
    if (_stricmp(ext, ".pak") == 0) {
        return {1, 0};
    }
    return {0, 0};
}

static std::vector<std::string> DiscoverProjectsCompiledArchives() {
    std::vector<std::string> archives;
    char exePath[MAX_PATH] = {0};
    std::string baseDir = "Data\\";
    if (GetModuleFileNameA(NULL, exePath, MAX_PATH) > 0) {
        char* slash = strrchr(exePath, '\\');
        if (!slash) slash = strrchr(exePath, '/');
        if (slash) {
            *(slash + 1) = '\0';
            baseDir = std::string(exePath) + "Data\\";
        }
    }

    std::string searchPattern = baseDir + "Projects_compiled.*";
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(searchPattern.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                archives.push_back(baseDir + fd.cFileName);
            }
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }

    std::sort(archives.begin(), archives.end(), [](const std::string& a, const std::string& b) {
        return GetArchivePriority(a) > GetArchivePriority(b);
    });

    return archives;
}

static uint32_t ReadLE32(const void* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

static uint64_t ReadLE64(const void* p) {
    uint64_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

static void IndexArchives() {
    s_tasks.clear();
    std::unordered_set<std::string> seenPaths;
    auto archives = DiscoverProjectsCompiledArchives();
    if (archives.empty()) {
        Log("G3O", "[DynamicLayerOptimizer] No Projects_compiled archives found.");
        return;
    }

    uint64_t totalComp = 0;
    uint64_t totalUncomp = 0;

    for (const auto& archPath : archives) {
        if (GetArchivePriority(archPath).first == 0) continue;

        HANDLE hFile = CreateFileA(archPath.c_str(), GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) continue;

        LARGE_INTEGER fsize = {};
        if (!GetFileSizeEx(hFile, &fsize) || fsize.QuadPart <= 32) {
            CloseHandle(hFile);
            continue;
        }

        BYTE hdr[32];
        DWORD bytesRead = 0;
        if (!ReadFile(hFile, hdr, 32, &bytesRead, NULL) || bytesRead != 32 ||
            memcmp(hdr + 4, "G3V0", 4) != 0) {
            CloseHandle(hFile);
            continue;
        }

        const uint32_t fat_off = ReadLE32(hdr + 24);
        if (fat_off < 32 || static_cast<uint64_t>(fat_off) >= static_cast<uint64_t>(fsize.QuadPart)) {
            CloseHandle(hFile);
            continue;
        }

        const uint64_t fileSize = static_cast<uint64_t>(fsize.QuadPart);
        const uint64_t fat_size = fileSize - fat_off;
        if (fat_size == 0 || fat_size > 64 * 1024 * 1024) {
            CloseHandle(hFile);
            continue;
        }

        BYTE* pFat = reinterpret_cast<BYTE*>(VirtualAlloc(NULL, static_cast<SIZE_T>(fat_size),
                                                          MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!pFat) {
            CloseHandle(hFile);
            continue;
        }

        LARGE_INTEGER liFat;
        liFat.QuadPart = fat_off;
        if (SetFilePointerEx(hFile, liFat, NULL, FILE_BEGIN) &&
            ReadFile(hFile, pFat, static_cast<DWORD>(fat_size), &bytesRead, NULL) &&
            bytesRead == fat_size) {

            const size_t totalBytes = static_cast<size_t>(bytesRead);
            size_t pos = 40;
            while (pos < totalBytes) {
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

                    if ((flags & 0x00020020) == 0x00020020 &&
                        off >= 32 && off <= fileSize && c_sz <= fileSize - off &&
                        u_sz > 0 && u_sz <= 32 * 1024 * 1024 && c_sz >= 6) {

                        const char* nameStr = reinterpret_cast<const char*>(pFat + pos);
                        if (strchr(nameStr, '/')) { // Virtual path only
                            bool isTarget = false;
                            const char* dot = strrchr(nameStr, '.');
                            if (dot) {
                                if (_stricmp(dot, ".lrentdat") == 0) {
                                    isTarget = true;
                                } else if (_stricmp(dot, ".node") == 0) {
                                    if (strstr(nameStr, "CNQ") || strstr(nameStr, "LTM") ||
                                        strstr(nameStr, "AD_World") || strstr(nameStr, "RB_Mod") ||
                                        strstr(nameStr, "Alternativ") || strstr(nameStr, "Immersive") ||
                                        strstr(nameStr, "Mod_") || strstr(nameStr, "NPC")) {
                                        isTarget = true;
                                    }
                                }
                            }

                            if (isTarget) {
                                std::string vpath(nameStr);
                                if (seenPaths.find(vpath) == seenPaths.end()) {
                                    seenPaths.insert(vpath);
                                    LayerTask task;
                                    task.archivePath = archPath;
                                    task.fileOffset = off;
                                    task.compSize = static_cast<uint32_t>(c_sz);
                                    task.uncompSize = static_cast<uint32_t>(u_sz);
                                    task.name = vpath;
                                    s_tasks.push_back(std::move(task));
                                    totalComp += c_sz;
                                    totalUncomp += u_sz;
                                }
                            }
                        }
                    }
                }
                pos = nameEnd + 1;
            }
        }

        VirtualFree(pFat, 0, MEM_RELEASE);
        CloseHandle(hFile);
    }

    Log("G3O", "[DynamicLayerOptimizer] Indexed %zu unique target dynamic layers (%.2f MB compressed -> %.2f MB uncompressed).",
        s_tasks.size(),
        static_cast<double>(totalComp) / (1024.0 * 1024.0),
        static_cast<double>(totalUncomp) / (1024.0 * 1024.0));
}

void InitializeDynamicLayerOptimizer() {
    AcquireSRWLockExclusive(&s_initLock);
    if (!s_initialized) {
        IndexArchives();
        s_initialized = true;
    }
    ReleaseSRWLockExclusive(&s_initLock);
}

void ShutdownDynamicLayerOptimizer() {
    DynamicLayerOptimizer_OnLoadEnd();
    AcquireSRWLockExclusive(&s_initLock);
    s_tasks.clear();
    s_initialized = false;
    ReleaseSRWLockExclusive(&s_initLock);
}

void DynamicLayerOptimizer_OnLoadStart() {
    const Config& cfg = GetConfig();
    if (!cfg.enableParallelDynamicLayers) return;

    AcquireSRWLockExclusive(&s_initLock);
    if (!s_initialized) {
        IndexArchives();
        s_initialized = true;
    }
    ReleaseSRWLockExclusive(&s_initLock);

    if (s_tasks.empty()) return;
    if (s_isActive.exchange(true)) {
        // Already active
        return;
    }

    for (size_t b = 0; b < kNumBuckets; ++b) {
        AcquireSRWLockExclusive(&s_buckets[b].lock);
        s_buckets[b].map.clear();
        ReleaseSRWLockExclusive(&s_buckets[b].lock);
    }

    s_statHits.store(0, std::memory_order_relaxed);
    s_statMisses.store(0, std::memory_order_relaxed);
    s_statBytesServed.store(0, std::memory_order_relaxed);
    s_nextTaskIndex.store(0, std::memory_order_relaxed);

    unsigned int numThreads = std::thread::hardware_concurrency();
    if (numThreads < 2) numThreads = 2;
    if (numThreads > 6) numThreads = 6;

    s_workers.clear();
    s_workers.reserve(numThreads);

    for (unsigned int w = 0; w < numThreads; ++w) {
        s_workers.emplace_back([]() {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            libdeflate_decompressor* dec = nullptr;
            if (G3O_AVX2_IsSupported()) {
                dec = G3O_AVX2_AllocDecompressor();
            } else {
                dec = libdeflate_alloc_decompressor();
            }

            std::unordered_map<std::string, HANDLE> localHandles;
            std::vector<uint8_t> compBuf;
            std::vector<uint8_t> decompBuf;

            while (s_isActive.load(std::memory_order_relaxed)) {
                size_t taskIdx = s_nextTaskIndex.fetch_add(1, std::memory_order_relaxed);
                if (taskIdx >= s_tasks.size()) break;

                const auto& task = s_tasks[taskIdx];

                HANDLE hFile = INVALID_HANDLE_VALUE;
                auto hIt = localHandles.find(task.archivePath);
                if (hIt != localHandles.end()) {
                    hFile = hIt->second;
                } else {
                    hFile = CreateFileA(task.archivePath.c_str(), GENERIC_READ,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                    localHandles[task.archivePath] = hFile;
                }
                if (hFile == INVALID_HANDLE_VALUE) continue;

                if (compBuf.size() < task.compSize) compBuf.resize(task.compSize);
                LARGE_INTEGER li;
                li.QuadPart = task.fileOffset;
                DWORD bytesRead = 0;
                if (!SetFilePointerEx(hFile, li, NULL, FILE_BEGIN) ||
                    !ReadFile(hFile, compBuf.data(), task.compSize, &bytesRead, NULL) ||
                    bytesRead != task.compSize) {
                    continue;
                }

                uint64_t key = ComputeCompressedBufferKey(compBuf.data(), task.compSize);
                if (key == 0) continue;

                if (decompBuf.size() < task.uncompSize) decompBuf.resize(task.uncompSize);
                size_t actualOut = 0;
                int res = -1;
                if (G3O_AVX2_IsSupported()) {
                    res = G3O_AVX2_DecompressZlib(dec, compBuf.data(), task.compSize,
                                                  decompBuf.data(), task.uncompSize, &actualOut);
                } else if (dec) {
                    res = libdeflate_zlib_decompress(dec,
                                                     compBuf.data(), task.compSize,
                                                     decompBuf.data(), task.uncompSize, &actualOut);
                }

                if (res == 0 && actualOut == task.uncompSize) {
                    PredecompEntry entry;
                    entry.compSize = task.compSize;
                    entry.uncompSize = task.uncompSize;
                    entry.data.assign(decompBuf.data(), decompBuf.data() + actualOut);

                    const size_t b = (key ^ (key >> 32)) & (kNumBuckets - 1);
                    AcquireSRWLockExclusive(&s_buckets[b].lock);
                    s_buckets[b].map[key] = std::move(entry);
                    ReleaseSRWLockExclusive(&s_buckets[b].lock);
                }
            }

            for (auto& pair : localHandles) {
                if (pair.second != INVALID_HANDLE_VALUE) {
                    CloseHandle(pair.second);
                }
            }
            if (dec) {
                if (G3O_AVX2_IsSupported()) {
                    G3O_AVX2_FreeDecompressor(dec);
                } else {
                    libdeflate_free_decompressor(dec);
                }
            }
        });
    }

    Log("G3O", "[DynamicLayerOptimizer] Background pre-decompression pool launched with %u workers for %zu tasks.",
        numThreads, s_tasks.size());
}

void DynamicLayerOptimizer_OnLoadEnd() {
    if (!s_isActive.exchange(false)) return;

    for (auto& w : s_workers) {
        if (w.joinable()) {
            w.join();
        }
    }
    s_workers.clear();

    for (size_t b = 0; b < kNumBuckets; ++b) {
        AcquireSRWLockExclusive(&s_buckets[b].lock);
        s_buckets[b].map.clear();
        ReleaseSRWLockExclusive(&s_buckets[b].lock);
    }

    Log("G3O", "[DynamicLayerOptimizer] Load session finished: %u hits, %u misses, %.2f MB served from RAM cache.",
        s_statHits.load(), s_statMisses.load(),
        static_cast<double>(s_statBytesServed.load()) / (1024.0 * 1024.0));
}

bool DynamicLayerOptimizer_TryGet(
    const void* inBuffer,
    DWORD inSize,
    void* outBuffer,
    DWORD outCapacity,
    DWORD* pOutWritten
) {
    if (!s_isActive.load(std::memory_order_relaxed)) return false;
    if (!inBuffer || inSize < 6 || !outBuffer || outCapacity == 0) return false;

    const uint64_t key = ComputeCompressedBufferKey(inBuffer, inSize);
    if (key == 0) return false;

    const size_t b = (key ^ (key >> 32)) & (kNumBuckets - 1);
    AcquireSRWLockShared(&s_buckets[b].lock);
    auto it = s_buckets[b].map.find(key);
    if (it != s_buckets[b].map.end()) {
        if (outCapacity >= it->second.uncompSize) {
            const uint32_t outSize = it->second.uncompSize;
            std::memcpy(outBuffer, it->second.data.data(), outSize);
            if (pOutWritten) *pOutWritten = outSize;
            s_statHits.fetch_add(1, std::memory_order_relaxed);
            s_statBytesServed.fetch_add(outSize, std::memory_order_relaxed);
            ReleaseSRWLockShared(&s_buckets[b].lock);

            // Evict larger layers immediately after consumption to minimize VAS footprint
            if (outSize > 4096) {
                AcquireSRWLockExclusive(&s_buckets[b].lock);
                s_buckets[b].map.erase(key);
                ReleaseSRWLockExclusive(&s_buckets[b].lock);
            }
            return true;
        }
        ReleaseSRWLockShared(&s_buckets[b].lock);
        s_statMisses.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    ReleaseSRWLockShared(&s_buckets[b].lock);
    return false;
}

void DynamicLayerOptimizer_ProcessWorldActivate(void* pWorld, WorldOnActivate_t originalWorldOnActivate) {
    if (!originalWorldOnActivate || !pWorld) return;

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    // Main thread executes all sector activations sequentially and safely!
    originalWorldOnActivate(pWorld);

    QueryPerformanceCounter(&t1);
    double elapsedMs = (freq.QuadPart > 0)
        ? ((t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart))
        : 0.0;

    Log("G3O", "[DynamicLayerOptimizer] WorldOnActivate finished safely on main thread in %.2f ms (Served %.2f MB across %u hits, misses=%u).",
        elapsedMs, static_cast<double>(s_statBytesServed.load()) / (1024.0 * 1024.0),
        s_statHits.load(), s_statMisses.load());
}

DynamicLayerStats DynamicLayerOptimizer_GetStats() {
    DynamicLayerStats st;
    st.hits = s_statHits.load();
    st.misses = s_statMisses.load();
    st.bytesServed = s_statBytesServed.load();
    return st;
}

} // namespace G3O
