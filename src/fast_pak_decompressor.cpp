#include "fast_pak_decompressor.h"
#include "layer_ring_arena.h"
#include "decompressor_avx2.h"
#include "config_manager.h"
#include "diagnostic_logger.h"
#include "pak_tracer.h"
#include "blackbox_recorder.h"
#include <minhook.h>
#include <windows.h>
#include <libdeflate.h>
#include <stdio.h>
#include <vector>

namespace G3O {

// -----------------------------------------------------------------------------
// CPU Dispatch Architecture (SSE2 baseline + AVX2/BMI2 hardware acceleration)
// -----------------------------------------------------------------------------
typedef void* (*AllocDecompFn)();
typedef void  (*FreeDecompFn)(void*);
typedef int   (*DecompZlibFn)(void*, const void*, size_t, void*, size_t, size_t*);
typedef int   (*DecompDeflateFn)(void*, const void*, size_t, void*, size_t, size_t*);

static AllocDecompFn   s_fnAlloc = nullptr;
static FreeDecompFn    s_fnFree = nullptr;
static DecompZlibFn    s_fnZlib = nullptr;
static DecompDeflateFn s_fnDeflate = nullptr;
static const char*     s_backendName = "Uninitialized";
static bool            s_isAvx2Active = false;

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

static thread_local void* s_tlDecompressor = nullptr;

static void* GetThreadDecompressor() {
    if (!s_tlDecompressor && s_fnAlloc) {
        s_tlDecompressor = s_fnAlloc();
    }
    return s_tlDecompressor;
}

static volatile LONG s_consecutiveHits = 0;
static volatile LONG64 s_consecutiveBytesSaved = 0;

// -----------------------------------------------------------------------------
// Decompression Cache Telemetry
// -----------------------------------------------------------------------------
LONG GetDecompCacheHits() { return s_consecutiveHits; }
LONG64 GetDecompCacheBytesSaved() { return s_consecutiveBytesSaved; }
void ClearDecompCache() {
    InterlockedExchange(&s_consecutiveHits, 0);
    InterlockedExchange64(&s_consecutiveBytesSaved, 0);
}

static int __stdcall Hooked_ZlibDecompress(
    void* context,
    const void* inBuffer,
    DWORD inSize,
    void* outBuffer,
    DWORD outCapacity,
    DWORD* pOutWritten
) {
    BlackboxScope bbScope(BB_OP_DECOMPRESSING_PAK, "PAK_Stream", (uint64_t)(uintptr_t)inBuffer, inSize);
    if (inBuffer && inSize >= 6 && outBuffer && outCapacity > 0) {
        // Fast path: Sliding-Window Ring Arena for heavy static layers (RB_Mod)
        if (LayerRingArena_IsActive()) {
            DWORD arenaOut = 0;
            if (LayerRingArena_TryGet(inBuffer, inSize, outBuffer, outCapacity, &arenaOut)) {
                if (pOutWritten) *pOutWritten = arenaOut;
                InterlockedIncrement(&s_decompCount);
                InterlockedExchangeAdd64(&s_decompInBytes, (LONG64)inSize);
                InterlockedExchangeAdd64(&s_decompOutBytes, (LONG64)arenaOut);
                return 0; // Served with zero decompression overhead from Ring Arena
            }
        }

        // Step 2.3: Fast speculative pre-decompression cache lookup
        if (PakTracer_CanTryGetPredecompressed()) {
            DWORD cachedOut = 0;
            LARGE_INTEGER t0_cache, t1_cache;
            QueryPerformanceCounter(&t0_cache);
            if (PakTracer_TryGetPredecompressed(inBuffer, inSize, outBuffer, outCapacity, &cachedOut)) {
                QueryPerformanceCounter(&t1_cache);
                LONG64 us = (s_timerFreq.QuadPart > 0) ? ((t1_cache.QuadPart - t0_cache.QuadPart) * 1000000LL / s_timerFreq.QuadPart) : 0;
                InterlockedExchangeAdd64(&s_decompTimeUs, us);
                if (pOutWritten) {
                    *pOutWritten = cachedOut;
                }
                InterlockedIncrement(&s_decompCount);
                InterlockedExchangeAdd64(&s_decompInBytes, (LONG64)inSize);
                InterlockedExchangeAdd64(&s_decompOutBytes, (LONG64)cachedOut);
                return 0; // Served from the pre-decompressed cache
            }
        }

        if (s_fnZlib) {
            void* dec = GetThreadDecompressor();
            if (dec) {
            LARGE_INTEGER t0, t1;
            QueryPerformanceCounter(&t0);

            size_t actual_out = 0;
            // Primary path: Standard zlib-wrapped stream
            int res = s_fnZlib(
                dec,
                inBuffer,
                (size_t)inSize,
                outBuffer,
                (size_t)outCapacity,
                &actual_out
            );

            // Secondary path: Raw deflate with header/trailer stripped
            if (res != 0 && inSize >= 6 && s_fnDeflate) {
                res = s_fnDeflate(
                    dec,
                    (const BYTE*)inBuffer + 2,
                    (size_t)(inSize - 6),
                    outBuffer,
                    (size_t)outCapacity,
                    &actual_out
                );
            }

            if (res == 0) { // 0 == LIBDEFLATE_SUCCESS
                QueryPerformanceCounter(&t1);
                LONG64 us = (s_timerFreq.QuadPart > 0) ? ((t1.QuadPart - t0.QuadPart) * 1000000LL / s_timerFreq.QuadPart) : 0;
                InterlockedExchangeAdd64(&s_decompTimeUs, us);

                if (pOutWritten) {
                    *pOutWritten = (DWORD)actual_out;
                }
                InterlockedIncrement(&s_decompCount);
                InterlockedExchangeAdd64(&s_decompInBytes, (LONG64)inSize);
                InterlockedExchangeAdd64(&s_decompOutBytes, (LONG64)actual_out);

                PakTracer_RecordDecompress(inBuffer, inSize, outCapacity, (DWORD)actual_out, (uint64_t)us, outBuffer);

                return 0; // Success
            }
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
    BlackboxScope bbScope(BB_OP_DECOMPRESSING_SAVE, "SavegameCPX", (uint64_t)(uintptr_t)inBuffer, inSize);
    if (inBuffer && inSize >= 6 && outBuffer && pDestLen && *pDestLen > 0 && s_fnZlib) {
        void* dec = GetThreadDecompressor();
        if (dec) {
            LARGE_INTEGER t0, t1;
            QueryPerformanceCounter(&t0);

            size_t actual_out = 0;
            // Primary path: Standard zlib decompress
            int res = s_fnZlib(
                dec,
                inBuffer,
                (size_t)inSize,
                outBuffer,
                (size_t)*pDestLen,
                &actual_out
            );
            if (res == 0) {
                QueryPerformanceCounter(&t1);
                LONG64 us = (s_timerFreq.QuadPart > 0) ? ((t1.QuadPart - t0.QuadPart) * 1000000LL / s_timerFreq.QuadPart) : 0;
                InterlockedExchangeAdd64(&s_savegameDecompTimeUs, us);

                *pDestLen = (DWORD)actual_out;
                LONG count = InterlockedIncrement(&s_savegameDecompCount);
                InterlockedExchangeAdd64(&s_savegameDecompInBytes, (LONG64)inSize);
                InterlockedExchangeAdd64(&s_savegameDecompOutBytes, (LONG64)actual_out);

                Log("G3O", "[SavegameDecomp] Chunk #%ld: %lu bytes -> %lu bytes (%.2f ms, %s)",
                    count, inSize, (DWORD)actual_out, (double)us / 1000.0, s_backendName);
                return 0; // Z_OK
            }

            // Secondary path: Raw deflate
            if (inSize >= 6 && s_fnDeflate) {
                res = s_fnDeflate(
                    dec,
                    (const BYTE*)inBuffer + 2,
                    (size_t)(inSize - 6),
                    outBuffer,
                    (size_t)*pDestLen,
                    &actual_out
                );
                if (res == 0) {
                    QueryPerformanceCounter(&t1);
                    LONG64 us = (s_timerFreq.QuadPart > 0) ? ((t1.QuadPart - t0.QuadPart) * 1000000LL / s_timerFreq.QuadPart) : 0;
                    InterlockedExchangeAdd64(&s_savegameDecompTimeUs, us);

                    *pDestLen = (DWORD)actual_out;
                    LONG count = InterlockedIncrement(&s_savegameDecompCount);
                    InterlockedExchangeAdd64(&s_savegameDecompInBytes, (LONG64)inSize);
                    InterlockedExchangeAdd64(&s_savegameDecompOutBytes, (LONG64)actual_out);

                    Log("G3O", "[SavegameDecomp] Chunk #%ld: %lu bytes -> %lu bytes (%.2f ms, %s raw)",
                        count, inSize, (DWORD)actual_out, (double)us / 1000.0, s_backendName);
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

    // 0. Dynamic CPU Dispatch (Select AVX2+BMI2 if CPU supports it, else universal SSE2 baseline)
    if (G3O_AVX2_IsSupported()) {
        s_fnAlloc = (AllocDecompFn)G3O_AVX2_AllocDecompressor;
        s_fnFree = (FreeDecompFn)G3O_AVX2_FreeDecompressor;
        s_fnZlib = (DecompZlibFn)G3O_AVX2_DecompressZlib;
        s_fnDeflate = (DecompDeflateFn)G3O_AVX2_DecompressDeflate;
        s_backendName = "libdeflate [AVX2 + BMI2 Hardware Accelerated]";
        s_isAvx2Active = true;
    } else {
        s_fnAlloc = (AllocDecompFn)libdeflate_alloc_decompressor;
        s_fnFree = (FreeDecompFn)libdeflate_free_decompressor;
        s_fnZlib = (DecompZlibFn)libdeflate_zlib_decompress;
        s_fnDeflate = (DecompDeflateFn)libdeflate_deflate_decompress;
        s_backendName = "libdeflate [Universal SSE2 Baseline]";
        s_isAvx2Active = false;
    }
    Log("G3O", "[CPU Dispatch] Decompressor backend: %s", s_backendName);

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
    if (s_tlDecompressor && s_fnFree) {
        s_fnFree(s_tlDecompressor);
        s_tlDecompressor = nullptr;
    }
    ClearDecompCache();
    Log("G3O", "Decompressor stats: Pak files: %ld blocks (in: %.2f MB, out: %.2f MB, time: %.2f ms, cache hits: %ld, saved: %.2f MB) | Savegame: %ld chunks (in: %.2f MB, out: %.2f MB, time: %.2f ms)",
        s_decompCount,
        (double)s_decompInBytes / (1024.0 * 1024.0),
        (double)s_decompOutBytes / (1024.0 * 1024.0),
        (double)s_decompTimeUs / 1000.0,
        s_consecutiveHits,
        (double)s_consecutiveBytesSaved / (1024.0 * 1024.0),
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
}

} // namespace G3O
