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

typedef int (__stdcall* FileSystem_Inflate_t)(ZlibStreamProxy* strm);
static FileSystem_Inflate_t Real_FileSystem_Inflate = NULL;

static thread_local struct libdeflate_decompressor* t_decompressor = nullptr;
static volatile DWORD g_libdeflateOperations = 0;
static volatile DWORD g_zlibFallbackOperations = 0;

static int __stdcall Hooked_FileSystem_Inflate(ZlibStreamProxy* strm) {
    if (strm && strm->next_in && strm->next_out && strm->avail_in > 0 && strm->avail_out > 0) {
        if (!t_decompressor) {
            t_decompressor = libdeflate_alloc_decompressor();
        }

        if (t_decompressor) {
            size_t actual_out_nbytes = 0;
            // 1. Attempt fast SIMD zlib-stream decompression (standard RFC 1950)
            enum libdeflate_result res = libdeflate_zlib_decompress(
                t_decompressor,
                strm->next_in, strm->avail_in,
                strm->next_out, strm->avail_out,
                &actual_out_nbytes
            );

            // 2. Attempt raw deflate stream decompression if zlib header was not present
            if (res != LIBDEFLATE_SUCCESS) {
                res = libdeflate_deflate_decompress(
                    t_decompressor,
                    strm->next_in, strm->avail_in,
                    strm->next_out, strm->avail_out,
                    &actual_out_nbytes
                );
            }

            if (res == LIBDEFLATE_SUCCESS) {
                strm->next_in += strm->avail_in;
                strm->avail_in = 0;
                strm->next_out += actual_out_nbytes;
                strm->avail_out -= (unsigned int)actual_out_nbytes;
                strm->total_out += (unsigned long)actual_out_nbytes;
                InterlockedIncrement((volatile LONG*)&g_libdeflateOperations);
                return Z_STREAM_END; // 1
            }
        }
    }

    // Graceful fallback to original scalar zlib
    InterlockedIncrement((volatile LONG*)&g_zlibFallbackOperations);
    return Real_FileSystem_Inflate(strm);
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
// 3. FMOD Ex DSP Buffer & Stream Tuning (fmodex.dll) - Phase 16
// ---------------------------------------------------------------------------

typedef int (__stdcall* FMOD_System_Init_t)(void* pSystem, int maxchannels, unsigned int flags, void* extradriverdata);
static FMOD_System_Init_t Real_FMOD_System_Init = NULL;
static FMOD_System_Init_t Real_FMOD_System_CppInit = NULL;

typedef int (__stdcall* FMOD_System_SetDSPBufferSize_t)(void* pSystem, unsigned int bufferlength, int numbuffers);
static FMOD_System_SetDSPBufferSize_t Real_FMOD_System_SetDSPBufferSize = NULL;
static FMOD_System_SetDSPBufferSize_t Real_FMOD_System_CppSetDSPBufferSize = NULL;

typedef int (__stdcall* FMOD_System_SetSoftwareChannels_t)(void* pSystem, int numsoftwarechannels);
static FMOD_System_SetSoftwareChannels_t Real_FMOD_System_SetSoftwareChannels = NULL;
static FMOD_System_SetSoftwareChannels_t Real_FMOD_System_CppSetSoftwareChannels = NULL;

typedef int (__stdcall* FMOD_System_SetStreamBufferSize_t)(void* pSystem, unsigned int filebuffersize, unsigned int filebuffersizetype);
static FMOD_System_SetStreamBufferSize_t Real_FMOD_System_SetStreamBufferSize = NULL;
static FMOD_System_SetStreamBufferSize_t Real_FMOD_System_CppSetStreamBufferSize = NULL;

static void ApplyFMODAudioTuning(void* pSystem) {
    if (!pSystem) return;

    // 1. DSP Buffer: expand from 512 (2 slices) to 2048 samples (4 slices)
    // 2048 samples @ 48kHz = 42.6 ms * 4 = 170.6 ms jitter buffer (prevents audio underruns during sector loading)
    if (Real_FMOD_System_SetDSPBufferSize) {
        Real_FMOD_System_SetDSPBufferSize(pSystem, 2048, 4);
    } else if (Real_FMOD_System_CppSetDSPBufferSize) {
        Real_FMOD_System_CppSetDSPBufferSize(pSystem, 2048, 4);
    }

    // 2. Software Channels: expand mixer pool to 64 software voices
    if (Real_FMOD_System_SetSoftwareChannels) {
        Real_FMOD_System_SetSoftwareChannels(pSystem, 64);
    } else if (Real_FMOD_System_CppSetSoftwareChannels) {
        Real_FMOD_System_CppSetSoftwareChannels(pSystem, 64);
    }

    // 3. Stream Buffer: expand to 64 KB (0x10 = FMOD_TIMEUNIT_RAWBYTES) to eliminate music cutoffs
    if (Real_FMOD_System_SetStreamBufferSize) {
        Real_FMOD_System_SetStreamBufferSize(pSystem, 64 * 1024, 0x10);
    } else if (Real_FMOD_System_CppSetStreamBufferSize) {
        Real_FMOD_System_CppSetStreamBufferSize(pSystem, 64 * 1024, 0x10);
    }

    OutputDebugStringA("[Gothic3Optima] FMOD audio tuning applied: DSP 2048x4 (170ms), 64 SW channels, 64KB stream buffer.\n");
}

static int __stdcall Hooked_FMOD_System_Init(void* pSystem, int maxchannels, unsigned int flags, void* extradriverdata) {
    ApplyFMODAudioTuning(pSystem);
    if (maxchannels < 64) maxchannels = 64;
    return Real_FMOD_System_Init(pSystem, maxchannels, flags, extradriverdata);
}

static int __stdcall Hooked_FMOD_System_CppInit(void* pSystem, int maxchannels, unsigned int flags, void* extradriverdata) {
    ApplyFMODAudioTuning(pSystem);
    if (maxchannels < 64) maxchannels = 64;
    return Real_FMOD_System_CppInit(pSystem, maxchannels, flags, extradriverdata);
}

static int __stdcall Hooked_FMOD_System_SetDSPBufferSize(void* pSystem, unsigned int bufferlength, int numbuffers) {
    // Preserve our expanded 2048x4 buffer if engine attempts to downsize
    if (bufferlength < 1024) bufferlength = 2048;
    if (numbuffers < 4) numbuffers = 4;
    return Real_FMOD_System_SetDSPBufferSize(pSystem, bufferlength, numbuffers);
}

static int __stdcall Hooked_FMOD_System_CppSetDSPBufferSize(void* pSystem, unsigned int bufferlength, int numbuffers) {
    if (bufferlength < 1024) bufferlength = 2048;
    if (numbuffers < 4) numbuffers = 4;
    return Real_FMOD_System_CppSetDSPBufferSize(pSystem, bufferlength, numbuffers);
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

    // 2. FileSystem.dll internal zlib inflate hook disabled to ensure 100% stable archive decompression
    /*
    HMODULE hFileSystem = GetModuleHandleA("FileSystem.dll");
    if (hFileSystem) {
        void* pInflate = (void*)((uintptr_t)hFileSystem + 0xA430);
        if (MH_CreateHook(pInflate, (LPVOID)Hooked_FileSystem_Inflate, (LPVOID*)&Real_FileSystem_Inflate) == MH_OK) {
            MH_EnableHook(pInflate);
            OutputDebugStringA("[Gothic3Optima] Phase 11.2: libdeflate AVX2/SSE4.2 hardware decompression active (+0xA430)!\n");
        }
    }
    */

    // 3. String hashing hooks disabled to prevent hash table desynchronization
    /*
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
    */

    // 4. Hook FMOD Ex (Phase 16)
    HMODULE hFmod = GetModuleHandleA("fmodex.dll");
    if (hFmod) {
        Real_FMOD_System_SetDSPBufferSize = (FMOD_System_SetDSPBufferSize_t)GetProcAddress(hFmod, "FMOD_System_SetDSPBufferSize");
        Real_FMOD_System_CppSetDSPBufferSize = (FMOD_System_SetDSPBufferSize_t)GetProcAddress(hFmod, "?setDSPBufferSize@System@FMOD@@QAG?AW4FMOD_RESULT@@IH@Z");
        Real_FMOD_System_SetSoftwareChannels = (FMOD_System_SetSoftwareChannels_t)GetProcAddress(hFmod, "FMOD_System_SetSoftwareChannels");
        Real_FMOD_System_CppSetSoftwareChannels = (FMOD_System_SetSoftwareChannels_t)GetProcAddress(hFmod, "?setSoftwareChannels@System@FMOD@@QAG?AW4FMOD_RESULT@@H@Z");
        Real_FMOD_System_SetStreamBufferSize = (FMOD_System_SetStreamBufferSize_t)GetProcAddress(hFmod, "FMOD_System_SetStreamBufferSize");
        Real_FMOD_System_CppSetStreamBufferSize = (FMOD_System_SetStreamBufferSize_t)GetProcAddress(hFmod, "?setStreamBufferSize@System@FMOD@@QAG?AW4FMOD_RESULT@@II@Z");

        // Hook C API FMOD_System_Init
        void* pInit = (void*)GetProcAddress(hFmod, "FMOD_System_Init");
        if (pInit) {
            if (MH_CreateHook(pInit, (LPVOID)Hooked_FMOD_System_Init, (LPVOID*)&Real_FMOD_System_Init) == MH_OK) {
                MH_EnableHook(pInit);
                OutputDebugStringA("[Gothic3Optima] Hooked FMOD_System_Init.\n");
            }
        }

        // Hook C++ member API FMOD::System::init (?init@System@FMOD@@QAG?AW4FMOD_RESULT@@HIPAX@Z)
        void* pCppInit = (void*)GetProcAddress(hFmod, "?init@System@FMOD@@QAG?AW4FMOD_RESULT@@HIPAX@Z");
        if (pCppInit) {
            if (MH_CreateHook(pCppInit, (LPVOID)Hooked_FMOD_System_CppInit, (LPVOID*)&Real_FMOD_System_CppInit) == MH_OK) {
                MH_EnableHook(pCppInit);
                OutputDebugStringA("[Gothic3Optima] Hooked FMOD::System::init (C++).\n");
            }
        }

        // Hook buffer size setters to prevent engine from downsizing below 2048x4
        if (Real_FMOD_System_SetDSPBufferSize) {
            void* pSetDSP = (void*)Real_FMOD_System_SetDSPBufferSize;
            MH_CreateHook(pSetDSP, (LPVOID)Hooked_FMOD_System_SetDSPBufferSize, (LPVOID*)&Real_FMOD_System_SetDSPBufferSize);
            MH_EnableHook(pSetDSP);
        }
        if (Real_FMOD_System_CppSetDSPBufferSize) {
            void* pCppSetDSP = (void*)Real_FMOD_System_CppSetDSPBufferSize;
            MH_CreateHook(pCppSetDSP, (LPVOID)Hooked_FMOD_System_CppSetDSPBufferSize, (LPVOID*)&Real_FMOD_System_CppSetDSPBufferSize);
            MH_EnableHook(pCppSetDSP);
        }

        OutputDebugStringA("[Gothic3Optima] Phase 16: FMOD Ex audio stability hooks active.\n");
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
