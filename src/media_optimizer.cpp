#include "media_optimizer.h"
#include <minhook.h>
#include <libdeflate.h>
#include <mmsystem.h>
#include <stdio.h>

#pragma comment(lib, "winmm.lib")

// ---------------------------------------------------------------------------
// 1. Hardware-Accelerated Decompression (libdeflate for FileSystem.dll)
// ---------------------------------------------------------------------------

struct ZlibStreamProxy {
    const unsigned char* next_in;
    unsigned int        avail_in;
    unsigned long       total_in;
    unsigned char*      next_out;
    unsigned int        avail_out;
    unsigned long       total_out;
    char*               msg;
    void*               state;
    void*               zalloc;
    void*               zfree;
    void*               opaque;
    int                 data_type;
    unsigned long       adler;
    unsigned long       reserved;
};

#define Z_OK         0
#define Z_STREAM_END 1
#define Z_FINISH     4

typedef int (__cdecl* Inflate_t)(ZlibStreamProxy* strm, int flush);
static Inflate_t Real_Inflate = NULL;

static thread_local struct libdeflate_decompressor* t_decompressor = nullptr;
static volatile DWORD g_libdeflateOperations = 0;
static volatile DWORD g_zlibFallbackOperations = 0;

static int __cdecl Hooked_Inflate(ZlibStreamProxy* strm, int flush) {
    if (strm && strm->next_in && strm->next_out && strm->avail_in > 0 && strm->avail_out > 0) {
        if (!t_decompressor) {
            t_decompressor = libdeflate_alloc_decompressor();
        }

        if (t_decompressor) {
            size_t actual_out_nbytes = 0;
            // Attempt fast SIMD zlib-stream decompression
            enum libdeflate_result res = libdeflate_zlib_decompress(
                t_decompressor,
                strm->next_in, strm->avail_in,
                strm->next_out, strm->avail_out,
                &actual_out_nbytes
            );

            if (res == LIBDEFLATE_SUCCESS) {
                strm->next_in += strm->avail_in;
                strm->avail_in = 0;
                strm->next_out += actual_out_nbytes;
                strm->avail_out -= (unsigned int)actual_out_nbytes;
                strm->total_out += (unsigned long)actual_out_nbytes;
                InterlockedIncrement((volatile LONG*)&g_libdeflateOperations);
                return Z_STREAM_END;
            }
        }
    }

    InterlockedIncrement((volatile LONG*)&g_zlibFallbackOperations);
    return Real_Inflate(strm, flush);
}

// ---------------------------------------------------------------------------
// 2. High-Performance String Hashing (SharedBase.dll)
// ---------------------------------------------------------------------------

static inline unsigned long FastDJB2(const char* str) {
    if (!str) return 0;
    unsigned long hash = 0;
    while (*str) {
        hash = ((hash << 5) + hash) + (unsigned char)(*str++);
    }
    return hash;
}

typedef unsigned long (__stdcall* GetHashValue_bCString_t)(const void* pStr);
static GetHashValue_bCString_t Real_GetHashValue_bCString = NULL;

typedef unsigned long (__stdcall* GetHashValue_PAD_t)(const char* const* ppStr);
static GetHashValue_PAD_t Real_GetHashValue_PAD = NULL;

static unsigned long __stdcall Hooked_GetHashValue_bCString(const void* pStr) {
    if (!pStr) return 0;
    const char* str = *(const char* const*)pStr;
    return FastDJB2(str);
}

static unsigned long __stdcall Hooked_GetHashValue_PAD(const char* const* ppStr) {
    if (!ppStr) return 0;
    return FastDJB2(*ppStr);
}

// ---------------------------------------------------------------------------
// 3. FMOD Ex DSP Buffer Tuning (fmodex.dll)
// ---------------------------------------------------------------------------

typedef int (__stdcall* FMOD_System_Init_t)(void* pSystem, int maxchannels, unsigned int flags, void* extradriverdata);
static FMOD_System_Init_t Real_FMOD_System_Init = NULL;

typedef int (__stdcall* FMOD_System_SetDSPBufferSize_t)(void* pSystem, unsigned int bufferlength, int numbuffers);
static FMOD_System_SetDSPBufferSize_t Real_FMOD_System_SetDSPBufferSize = NULL;

static int __stdcall Hooked_FMOD_System_Init(void* pSystem, int maxchannels, unsigned int flags, void* extradriverdata) {
    if (Real_FMOD_System_SetDSPBufferSize && pSystem) {
        // Increase DSP buffer from 512 to 1024 samples (4 buffers) to eliminate clicks and pops
        Real_FMOD_System_SetDSPBufferSize(pSystem, 1024, 4);
        OutputDebugStringA("[Gothic3Optima] FMOD DSP Buffer tuned to 1024 samples x 4 buffers.\n");
    }
    return Real_FMOD_System_Init(pSystem, maxchannels, flags, extradriverdata);
}

// ---------------------------------------------------------------------------
// 4. Bink Video Stabilization (binkw32.dll)
// ---------------------------------------------------------------------------

typedef int (__stdcall* BinkWait_t)(void* bink);
static BinkWait_t Real_BinkWait = NULL;

static int __stdcall Hooked_BinkWait(void* bink) {
    int wait = Real_BinkWait(bink);
    if (wait) {
        Sleep(1); // Maintain smooth 30/60 fps cadence on high-refresh monitors
    }
    return wait;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void InitializeMediaOptimizer() {
    // 1. Enable 1ms OS timer resolution for smooth frame pacing
    timeBeginPeriod(1);
    OutputDebugStringA("[Gothic3Optima] 1ms High-Resolution Multimedia Timer activated.\n");

    // 2. FileSystem.dll internal zlib inflate hook disabled (unexported static function)
    /*
    HMODULE hFileSystem = GetModuleHandleA("FileSystem.dll");
    if (hFileSystem) {
        void* pInflate = (void*)((uintptr_t)hFileSystem + 0x153dc);
        if (MH_CreateHook(pInflate, (LPVOID)Hooked_Inflate, (LPVOID*)&Real_Inflate) == MH_OK) {
            MH_EnableHook(pInflate);
        }
    }
    */

    // 3. Hook g_GetHashValue in SharedBase.dll
    HMODULE hSharedBase = GetModuleHandleA("SharedBase.dll");
    if (hSharedBase) {
        void* pHashBCString = (void*)GetProcAddress(hSharedBase, "??$g_GetHashValue@VbCString@@@@YGKABVbCString@@@Z");
        if (pHashBCString) {
            if (MH_CreateHook(pHashBCString, (LPVOID)Hooked_GetHashValue_bCString, (LPVOID*)&Real_GetHashValue_bCString) == MH_OK) {
                MH_EnableHook(pHashBCString);
                OutputDebugStringA("[Gothic3Optima] Hooked g_GetHashValue<bCString>.\n");
            }
        }

        void* pHashPAD = (void*)GetProcAddress(hSharedBase, "??$g_GetHashValue@PAD@@YGKABQAD@Z");
        if (pHashPAD) {
            if (MH_CreateHook(pHashPAD, (LPVOID)Hooked_GetHashValue_PAD, (LPVOID*)&Real_GetHashValue_PAD) == MH_OK) {
                MH_EnableHook(pHashPAD);
                OutputDebugStringA("[Gothic3Optima] Hooked g_GetHashValue<PAD>.\n");
            }
        }
    }

    // 4. Hook FMOD Ex
    HMODULE hFmod = GetModuleHandleA("fmodex.dll");
    if (hFmod) {
        Real_FMOD_System_SetDSPBufferSize = (FMOD_System_SetDSPBufferSize_t)GetProcAddress(hFmod, "FMOD_System_SetDSPBufferSize");
        void* pInit = (void*)GetProcAddress(hFmod, "FMOD_System_Init");
        if (pInit) {
            if (MH_CreateHook(pInit, (LPVOID)Hooked_FMOD_System_Init, (LPVOID*)&Real_FMOD_System_Init) == MH_OK) {
                MH_EnableHook(pInit);
                OutputDebugStringA("[Gothic3Optima] Hooked FMOD_System_Init for audio stability.\n");
            }
        }
    }

    // 5. Hook Bink Video
    HMODULE hBink = GetModuleHandleA("binkw32.dll");
    if (hBink) {
        void* pBinkWait = (void*)GetProcAddress(hBink, "_BinkWait@4");
        if (pBinkWait) {
            if (MH_CreateHook(pBinkWait, (LPVOID)Hooked_BinkWait, (LPVOID*)&Real_BinkWait) == MH_OK) {
                MH_EnableHook(pBinkWait);
                OutputDebugStringA("[Gothic3Optima] Hooked _BinkWait@4 for high-refresh video stability.\n");
            }
        }
    }
}

void ShutdownMediaOptimizer() {
    timeEndPeriod(1);
}
