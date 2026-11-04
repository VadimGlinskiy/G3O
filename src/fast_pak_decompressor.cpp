#include "fast_pak_decompressor.h"
#include "config_manager.h"
#include "diagnostic_logger.h"
#include <minhook.h>
#include <windows.h>
#include <libdeflate.h>
#include <stdio.h>

namespace G3O {

// -----------------------------------------------------------------------------
// FileSystem.dll hook (for virtual files inside .pak archives)
// -----------------------------------------------------------------------------
typedef int (__stdcall* ZlibDecompress_t)(
    void* context,
    const void* inBuffer,
    DWORD inSize,
    void* outBuffer,
    DWORD outCapacity,
    DWORD* pOutWritten
);

static ZlibDecompress_t Real_ZlibDecompress = nullptr;
static void* s_targetPakDecompressor = nullptr;

static volatile LONG   s_decompCount = 0;
static volatile LONG64 s_decompInBytes = 0;
static volatile LONG64 s_decompOutBytes = 0;
static volatile LONG64 s_decompTimeUs = 0;

// -----------------------------------------------------------------------------
// Game.dll hook (for savegame files: *.g3savcpxdat)
// Custom calling convention: EAX=inBuffer, EDX=outBuffer, ECX=inSize, [ESP+4]=pDestLen
// -----------------------------------------------------------------------------
static void* Real_GameUncompressTrampoline = nullptr;
static void* s_targetGameDecompressor = nullptr;

static volatile LONG   s_savegameDecompCount = 0;
static volatile LONG64 s_savegameDecompInBytes = 0;
static volatile LONG64 s_savegameDecompOutBytes = 0;
static volatile LONG64 s_savegameDecompTimeUs = 0;

static LARGE_INTEGER s_timerFreq = []() {
    LARGE_INTEGER li;
    QueryPerformanceFrequency(&li);
    if (li.QuadPart <= 0) li.QuadPart = 10000000LL;
    return li;
}();

static thread_local struct libdeflate_decompressor* s_tlDecompressor = nullptr;

static struct libdeflate_decompressor* GetThreadDecompressor() {
    if (!s_tlDecompressor) {
        s_tlDecompressor = libdeflate_alloc_decompressor();
    }
    return s_tlDecompressor;
}

// -----------------------------------------------------------------------------
// Decompression Cache (LRU in-memory cache for repeated assets)
// -----------------------------------------------------------------------------
static volatile LONG   s_decompCacheHits = 0;
static volatile LONG64 s_decompCacheBytesSaved = 0;

LONG GetDecompCacheHits() { return s_decompCacheHits; }
LONG64 GetDecompCacheBytesSaved() { return s_decompCacheBytesSaved; }

static const DWORD  MAX_CACHE_BLOCK_SIZE = 1024 * 1024;       // 1 MB max per chunk
static const size_t MAX_CACHE_TOTAL_MEM  = 256 * 1024 * 1024; // 256 MB RAM limit

struct DecompCacheNode {
    uint64_t hash;
    DWORD inSize;
    DWORD outSize;
    BYTE* data;
    DecompCacheNode* bucketNext;
    DecompCacheNode* lruPrev;
    DecompCacheNode* lruNext;
};

static const size_t DECOMP_BUCKET_COUNT = 4096;
static DecompCacheNode* s_decompBuckets[DECOMP_BUCKET_COUNT] = {};
static DecompCacheNode* s_lruHead = nullptr; // MRU
static DecompCacheNode* s_lruTail = nullptr; // LRU
static size_t s_currentCacheMem = 0;
static SRWLOCK s_decompCacheLock = SRWLOCK_INIT;

static inline uint64_t HashCompressedChunk(const void* inBuffer, DWORD inSize, DWORD outCap) {
    uint64_t h = 14695981039346656037ULL ^ (uint64_t)inSize ^ ((uint64_t)outCap << 32);
    const BYTE* p = (const BYTE*)inBuffer;
    for (DWORD i = 0; i < inSize; ++i) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static void EvictLRUTail_Locked() {
    while (s_lruTail && s_currentCacheMem > MAX_CACHE_TOTAL_MEM) {
        DecompCacheNode* victim = s_lruTail;
        s_lruTail = victim->lruPrev;
        if (s_lruTail) s_lruTail->lruNext = nullptr;
        else s_lruHead = nullptr;

        size_t bIdx = (size_t)(victim->hash % DECOMP_BUCKET_COUNT);
        DecompCacheNode** pp = &s_decompBuckets[bIdx];
        while (*pp) {
            if (*pp == victim) {
                *pp = victim->bucketNext;
                break;
            }
            pp = &((*pp)->bucketNext);
        }

        s_currentCacheMem -= victim->outSize;
        if (victim->data) free(victim->data);
        delete victim;
    }
}

void ClearDecompCache() {
    AcquireSRWLockExclusive(&s_decompCacheLock);
    DecompCacheNode* curr = s_lruHead;
    while (curr) {
        DecompCacheNode* next = curr->lruNext;
        if (curr->data) free(curr->data);
        delete curr;
        curr = next;
    }
    memset(s_decompBuckets, 0, sizeof(s_decompBuckets));
    s_lruHead = nullptr;
    s_lruTail = nullptr;
    s_currentCacheMem = 0;
    ReleaseSRWLockExclusive(&s_decompCacheLock);
}

static int __stdcall Hooked_ZlibDecompress(
    void* context,
    const void* inBuffer,
    DWORD inSize,
    void* outBuffer,
    DWORD outCapacity,
    DWORD* pOutWritten
) {
    if (inBuffer && inSize >= 6 && outBuffer && outCapacity > 0) {
        uint64_t chunkHash = HashCompressedChunk(inBuffer, inSize, outCapacity);
        size_t bIdx = (size_t)(chunkHash % DECOMP_BUCKET_COUNT);

        // Check if block was already decompressed and cached
        AcquireSRWLockShared(&s_decompCacheLock);
        for (DecompCacheNode* n = s_decompBuckets[bIdx]; n; n = n->bucketNext) {
            if (n->hash == chunkHash && n->inSize == inSize && n->outSize <= outCapacity) {
                memcpy(outBuffer, n->data, n->outSize);
                if (pOutWritten) *pOutWritten = n->outSize;
                InterlockedIncrement(&s_decompCacheHits);
                InterlockedExchangeAdd64(&s_decompCacheBytesSaved, (LONG64)n->outSize);
                ReleaseSRWLockShared(&s_decompCacheLock);
                return 0; // Instant cache hit
            }
        }
        ReleaseSRWLockShared(&s_decompCacheLock);

        struct libdeflate_decompressor* dec = GetThreadDecompressor();
        if (dec) {
            LARGE_INTEGER t0, t1;
            QueryPerformanceCounter(&t0);

            size_t actual_out = 0;
            // Primary path: Standard zlib-wrapped stream
            enum libdeflate_result res = libdeflate_zlib_decompress(
                dec,
                inBuffer,
                (size_t)inSize,
                outBuffer,
                (size_t)outCapacity,
                &actual_out
            );

            // Secondary path: Raw deflate with header/trailer stripped
            if (res != LIBDEFLATE_SUCCESS && inSize >= 6) {
                res = libdeflate_deflate_decompress(
                    dec,
                    (const BYTE*)inBuffer + 2,
                    (size_t)(inSize - 6),
                    outBuffer,
                    (size_t)outCapacity,
                    &actual_out
                );
            }

            if (res == LIBDEFLATE_SUCCESS) {
                QueryPerformanceCounter(&t1);
                LONG64 us = (s_timerFreq.QuadPart > 0) ? ((t1.QuadPart - t0.QuadPart) * 1000000LL / s_timerFreq.QuadPart) : 0;
                InterlockedExchangeAdd64(&s_decompTimeUs, us);

                if (pOutWritten) {
                    *pOutWritten = (DWORD)actual_out;
                }
                InterlockedIncrement(&s_decompCount);
                InterlockedExchangeAdd64(&s_decompInBytes, (LONG64)inSize);
                InterlockedExchangeAdd64(&s_decompOutBytes, (LONG64)actual_out);

                // Insert into Decompression Cache if within cacheable size
                if (actual_out <= MAX_CACHE_BLOCK_SIZE) {
                    BYTE* copy = (BYTE*)malloc(actual_out);
                    if (copy) {
                        memcpy(copy, outBuffer, actual_out);
                        AcquireSRWLockExclusive(&s_decompCacheLock);
                        DecompCacheNode* node = new DecompCacheNode();
                        node->hash = chunkHash;
                        node->inSize = inSize;
                        node->outSize = (DWORD)actual_out;
                        node->data = copy;
                        node->bucketNext = s_decompBuckets[bIdx];
                        s_decompBuckets[bIdx] = node;

                        node->lruPrev = nullptr;
                        node->lruNext = s_lruHead;
                        if (s_lruHead) s_lruHead->lruPrev = node;
                        s_lruHead = node;
                        if (!s_lruTail) s_lruTail = node;

                        s_currentCacheMem += actual_out;
                        EvictLRUTail_Locked();
                        ReleaseSRWLockExclusive(&s_decompCacheLock);
                    }
                }

                return 0; // Success
            }
        }
    }

    // Fallback to original engine decompressor
    return Real_ZlibDecompress(context, inBuffer, inSize, outBuffer, outCapacity, pOutWritten);
}

static int __stdcall SavegameDecompressImpl(
    const void* inBuffer,
    DWORD inSize,
    void* outBuffer,
    DWORD* pDestLen
) {
    if (inBuffer && inSize >= 6 && outBuffer && pDestLen && *pDestLen > 0) {
        struct libdeflate_decompressor* dec = GetThreadDecompressor();
        if (dec) {
            LARGE_INTEGER t0, t1;
            QueryPerformanceCounter(&t0);

            size_t actual_out = 0;
            // Primary path: Standard zlib decompress
            enum libdeflate_result res = libdeflate_zlib_decompress(
                dec,
                inBuffer,
                (size_t)inSize,
                outBuffer,
                (size_t)*pDestLen,
                &actual_out
            );
            if (res == LIBDEFLATE_SUCCESS) {
                QueryPerformanceCounter(&t1);
                LONG64 us = (s_timerFreq.QuadPart > 0) ? ((t1.QuadPart - t0.QuadPart) * 1000000LL / s_timerFreq.QuadPart) : 0;
                InterlockedExchangeAdd64(&s_savegameDecompTimeUs, us);

                *pDestLen = (DWORD)actual_out;
                LONG count = InterlockedIncrement(&s_savegameDecompCount);
                InterlockedExchangeAdd64(&s_savegameDecompInBytes, (LONG64)inSize);
                InterlockedExchangeAdd64(&s_savegameDecompOutBytes, (LONG64)actual_out);

                Log("G3O", "[SavegameDecomp] Chunk #%ld: %lu bytes -> %lu bytes (%.2f ms, libdeflate zlib)",
                    count, inSize, (DWORD)actual_out, (double)us / 1000.0);
                return 0; // Z_OK
            }

            // Secondary path: Raw deflate
            if (inSize >= 6) {
                res = libdeflate_deflate_decompress(
                    dec,
                    (const BYTE*)inBuffer + 2,
                    (size_t)(inSize - 6),
                    outBuffer,
                    (size_t)*pDestLen,
                    &actual_out
                );
                if (res == LIBDEFLATE_SUCCESS) {
                    QueryPerformanceCounter(&t1);
                    LONG64 us = (s_timerFreq.QuadPart > 0) ? ((t1.QuadPart - t0.QuadPart) * 1000000LL / s_timerFreq.QuadPart) : 0;
                    InterlockedExchangeAdd64(&s_savegameDecompTimeUs, us);

                    *pDestLen = (DWORD)actual_out;
                    LONG count = InterlockedIncrement(&s_savegameDecompCount);
                    InterlockedExchangeAdd64(&s_savegameDecompInBytes, (LONG64)inSize);
                    InterlockedExchangeAdd64(&s_savegameDecompOutBytes, (LONG64)actual_out);

                    Log("G3O", "[SavegameDecomp] Chunk #%ld: %lu bytes -> %lu bytes (%.2f ms, libdeflate raw)",
                        count, inSize, (DWORD)actual_out, (double)us / 1000.0);
                    return 0; // Z_OK
                }
            }
        }
    }

    // Fallback: call the original trampoline
    int result = -1;
    void* tramp = Real_GameUncompressTrampoline;
    __asm {
        mov eax, inBuffer
        mov edx, outBuffer
        mov ecx, inSize
        push pDestLen
        call dword ptr [tramp]
        mov result, eax
    }
    Log("G3O", "[SavegameDecomp] Fallback to original engine decompressor: in=%lu, result=%d", inSize, result);
    return result;
}

__declspec(naked) static void Hooked_GameUncompressThunk() {
    __asm {
        push dword ptr [esp + 4]  // pDestLen
        push edx                  // outBuffer
        push ecx                  // inSize
        push eax                  // inBuffer
        call SavegameDecompressImpl
        ret 4
    }
}

static void* FindDecompressorTarget(HMODULE hFs) {
    if (!hFs) return nullptr;

    const BYTE sig[] = {
        0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x10, 0x8B, 0x55,
        0x0C, 0x56, 0x8D, 0x34, 0x02, 0x83, 0xF8, 0x06
    };
    const size_t sigLen = sizeof(sig);

    BYTE* targetRva = (BYTE*)hFs + 0xA2D0;
    if (memcmp(targetRva, sig, sigLen) == 0) {
        return targetRva;
    }

    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)hFs;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((BYTE*)hFs + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    IMAGE_SECTION_HEADER* sect = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (memcmp(sect[i].Name, ".text", 5) == 0) {
            BYTE* start = (BYTE*)hFs + sect[i].VirtualAddress;
            DWORD size = sect[i].Misc.VirtualSize;
            if (size > sigLen) {
                for (DWORD offset = 0; offset <= size - sigLen; ++offset) {
                    if (memcmp(start + offset, sig, sigLen) == 0) {
                        return start + offset;
                    }
                }
            }
        }
    }

    return nullptr;
}

static void* FindSavegameDecompressorTarget(HMODULE hGame) {
    if (!hGame) return nullptr;

    const BYTE sig[] = {
        0x83, 0xEC, 0x38, 0x56, 0x8B, 0x74, 0x24, 0x40,
        0x89, 0x44, 0x24, 0x04, 0x8B, 0x06, 0x89, 0x44, 0x24, 0x14
    };
    const size_t sigLen = sizeof(sig);

    BYTE* targetRva = (BYTE*)hGame + 0x1D4900;
    if (memcmp(targetRva, sig, sigLen) == 0) {
        return targetRva;
    }

    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)hGame;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((BYTE*)hGame + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    IMAGE_SECTION_HEADER* sect = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (memcmp(sect[i].Name, ".text", 5) == 0) {
            BYTE* start = (BYTE*)hGame + sect[i].VirtualAddress;
            DWORD size = sect[i].Misc.VirtualSize;
            if (size > sigLen) {
                for (DWORD offset = 0; offset <= size - sigLen; ++offset) {
                    if (memcmp(start + offset, sig, sigLen) == 0) {
                        return start + offset;
                    }
                }
            }
        }
    }

    return nullptr;
}

bool InitializeFastPakDecompressor() {
    if (!GetConfig().enableFastPakDecompressor) {
        return false;
    }

    // 1. FileSystem.dll hook
    HMODULE hFs = GetModuleHandleA("FileSystem.dll");
    if (hFs) {
        void* targetFs = FindDecompressorTarget(hFs);
        if (targetFs) {
            s_targetPakDecompressor = targetFs;
            MH_STATUS st = MH_CreateHook(targetFs, (LPVOID)Hooked_ZlibDecompress, (LPVOID*)&Real_ZlibDecompress);
            if (st == MH_OK) {
                MH_EnableHook(targetFs);
                Log("G3O", "Fast Pak Decompressor hooked successfully (FileSystem.dll!0x%p). SIMD zlib decompression active.", targetFs);
            } else {
                Log("G3O", "Fast Pak Decompressor: Failed to hook FileSystem.dll (code %d).", (int)st);
            }
        } else {
            Log("G3O", "Fast Pak Decompressor: Signature not found in FileSystem.dll.");
        }
    } else {
        Log("G3O", "Fast Pak Decompressor: FileSystem.dll not loaded.");
    }

    // 2. Game.dll savegame decompressor hook
    HMODULE hGame = GetModuleHandleA("Game.dll");
    if (hGame) {
        void* targetGame = FindSavegameDecompressorTarget(hGame);
        if (targetGame) {
            s_targetGameDecompressor = targetGame;
            MH_STATUS st = MH_CreateHook(targetGame, (LPVOID)Hooked_GameUncompressThunk, (LPVOID*)&Real_GameUncompressTrampoline);
            if (st == MH_OK) {
                MH_EnableHook(targetGame);
                Log("G3O", "Savegame Fast Decompressor hooked successfully (Game.dll!0x%p). SIMD savegame decompression active.", targetGame);
            } else {
                Log("G3O", "Savegame Fast Decompressor: Failed to hook Game.dll (code %d).", (int)st);
            }
        } else {
            Log("G3O", "Savegame Fast Decompressor: Signature not found in Game.dll.");
        }
    } else {
        Log("G3O", "Savegame Fast Decompressor: Game.dll not loaded.");
    }

    return (s_targetPakDecompressor != nullptr || s_targetGameDecompressor != nullptr);
}

void ShutdownFastPakDecompressor() {
    if (s_targetPakDecompressor) {
        MH_DisableHook(s_targetPakDecompressor);
        s_targetPakDecompressor = nullptr;
    }
    if (s_targetGameDecompressor) {
        MH_DisableHook(s_targetGameDecompressor);
        s_targetGameDecompressor = nullptr;
    }
    ClearDecompCache();
    Log("G3O", "Decompressor stats: Pak files: %ld blocks (in: %.2f MB, out: %.2f MB, time: %.2f ms, cache hits: %ld) | Savegame: %ld chunks (in: %.2f MB, out: %.2f MB, time: %.2f ms)",
        s_decompCount,
        (double)s_decompInBytes / (1024.0 * 1024.0),
        (double)s_decompOutBytes / (1024.0 * 1024.0),
        (double)s_decompTimeUs / 1000.0,
        s_decompCacheHits,
        s_savegameDecompCount,
        (double)s_savegameDecompInBytes / (1024.0 * 1024.0),
        (double)s_savegameDecompOutBytes / (1024.0 * 1024.0),
        (double)s_savegameDecompTimeUs / 1000.0);
}

LONG GetPakDecompCount() { return s_decompCount; }
LONG64 GetPakDecompInBytes() { return s_decompInBytes; }
LONG64 GetPakDecompOutBytes() { return s_decompOutBytes; }
double GetPakDecompTimeMs() { return (double)s_decompTimeUs / 1000.0; }

LONG GetSavegameDecompCount() { return s_savegameDecompCount; }
LONG64 GetSavegameDecompInBytes() { return s_savegameDecompInBytes; }
LONG64 GetSavegameDecompOutBytes() { return s_savegameDecompOutBytes; }
double GetSavegameDecompTimeMs() { return (double)s_savegameDecompTimeUs / 1000.0; }

void ResetDecompTelemetry() {
    InterlockedExchange(&s_decompCount, 0);
    InterlockedExchange64(&s_decompInBytes, 0);
    InterlockedExchange64(&s_decompOutBytes, 0);
    InterlockedExchange64(&s_decompTimeUs, 0);

    InterlockedExchange(&s_savegameDecompCount, 0);
    InterlockedExchange64(&s_savegameDecompInBytes, 0);
    InterlockedExchange64(&s_savegameDecompOutBytes, 0);
    InterlockedExchange64(&s_savegameDecompTimeUs, 0);

    InterlockedExchange(&s_decompCacheHits, 0);
    InterlockedExchange64(&s_decompCacheBytesSaved, 0);
}

} // namespace G3O
