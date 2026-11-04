#include "fast_pak_decompressor.h"
#include "config_manager.h"
#include "diagnostic_logger.h"
#include <minhook.h>
#include <windows.h>
#include <libdeflate.h>
#include <stdio.h>

namespace G3O {

typedef int (__stdcall* ZlibDecompress_t)(
    void* context,
    const void* inBuffer,
    DWORD inSize,
    void* outBuffer,
    DWORD outCapacity,
    DWORD* pOutWritten
);

static ZlibDecompress_t Real_ZlibDecompress = nullptr;
static void* s_targetDecompressor = nullptr;

static volatile LONG   s_decompCount = 0;
static volatile LONG64 s_decompInBytes = 0;
static volatile LONG64 s_decompOutBytes = 0;

static thread_local struct libdeflate_decompressor* s_tlDecompressor = nullptr;

static struct libdeflate_decompressor* GetThreadDecompressor() {
    if (!s_tlDecompressor) {
        s_tlDecompressor = libdeflate_alloc_decompressor();
    }
    return s_tlDecompressor;
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
        struct libdeflate_decompressor* dec = GetThreadDecompressor();
        if (dec) {
            size_t actual_out = 0;
            enum libdeflate_result res = libdeflate_zlib_decompress(
                dec,
                inBuffer,
                (size_t)inSize,
                outBuffer,
                (size_t)outCapacity,
                &actual_out
            );

            if (res == LIBDEFLATE_SUCCESS) {
                if (pOutWritten) {
                    *pOutWritten = (DWORD)actual_out;
                }
                InterlockedIncrement(&s_decompCount);
                InterlockedExchangeAdd64(&s_decompInBytes, (LONG64)inSize);
                InterlockedExchangeAdd64(&s_decompOutBytes, (LONG64)actual_out);
                return 0; // Success
            }
        }
    }

    // Fallback to original engine decompressor
    return Real_ZlibDecompress(context, inBuffer, inSize, outBuffer, outCapacity, pOutWritten);
}

static void* FindDecompressorTarget(HMODULE hFs) {
    if (!hFs) return nullptr;

    const BYTE sig[] = {
        0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x10, 0x8B, 0x55,
        0x0C, 0x56, 0x8D, 0x34, 0x02, 0x83, 0xF8, 0x06
    };
    const size_t sigLen = sizeof(sig);

    // Primary check: expected RVA 0xA2D0
    BYTE* targetRva = (BYTE*)hFs + 0xA2D0;
    if (memcmp(targetRva, sig, sigLen) == 0) {
        return targetRva;
    }

    // Secondary scan across .text section of FileSystem.dll
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

bool InitializeFastPakDecompressor() {
    if (!GetConfig().enableFastPakDecompressor) {
        return false;
    }

    HMODULE hFs = GetModuleHandleA("FileSystem.dll");
    if (!hFs) {
        Log("G3O", "Fast Pak Decompressor: FileSystem.dll not loaded.");
        return false;
    }

    void* target = FindDecompressorTarget(hFs);
    if (!target) {
        Log("G3O", "Fast Pak Decompressor: Signature not found in FileSystem.dll.");
        return false;
    }

    s_targetDecompressor = target;
    MH_STATUS status = MH_CreateHook(target, (LPVOID)Hooked_ZlibDecompress, (LPVOID*)&Real_ZlibDecompress);
    if (status != MH_OK) {
        Log("G3O", "Fast Pak Decompressor: Failed to create hook (code %d).", (int)status);
        return false;
    }

    status = MH_EnableHook(target);
    if (status != MH_OK) {
        Log("G3O", "Fast Pak Decompressor: Failed to enable hook (code %d).", (int)status);
        return false;
    }

    Log("G3O", "Fast Pak Decompressor hooked successfully (FileSystem.dll!0x%p). SIMD zlib decompression active.", target);
    return true;
}

void ShutdownFastPakDecompressor() {
    if (s_targetDecompressor) {
        MH_DisableHook(s_targetDecompressor);
        s_targetDecompressor = nullptr;
    }
}

} // namespace G3O
