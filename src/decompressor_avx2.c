#include "decompressor_avx2.h"
#include <windows.h>
#include <intrin.h>
#include <immintrin.h>

// Prefix all libdeflate C functions to avoid symbol collisions with libdeflate_static
#define libdeflate_alloc_decompressor          libdeflate_alloc_decompressor_avx2
#define libdeflate_alloc_decompressor_ex       libdeflate_alloc_decompressor_ex_avx2
#define libdeflate_free_decompressor           libdeflate_free_decompressor_avx2
#define libdeflate_deflate_decompress          libdeflate_deflate_decompress_avx2
#define libdeflate_deflate_decompress_ex       libdeflate_deflate_decompress_ex_avx2
#define libdeflate_zlib_decompress             libdeflate_zlib_decompress_avx2
#define libdeflate_zlib_decompress_ex          libdeflate_zlib_decompress_ex_avx2
#define libdeflate_adler32                     libdeflate_adler32_avx2
#define libdeflate_init_x86_cpu_features       libdeflate_init_x86_cpu_features_avx2
#define libdeflate_x86_cpu_features            libdeflate_x86_cpu_features_avx2
#define libdeflate_aligned_malloc              libdeflate_aligned_malloc_avx2
#define libdeflate_aligned_free                libdeflate_aligned_free_avx2
#define libdeflate_default_malloc_func         libdeflate_default_malloc_func_avx2
#define libdeflate_default_free_func           libdeflate_default_free_func_avx2
#define libdeflate_set_memory_allocator        libdeflate_set_memory_allocator_avx2

// Include libdeflate C implementation directly (compiled with /arch:AVX2 /Oi /O2)
#include "../third_party/libdeflate/lib/utils.c"
#include "../third_party/libdeflate/lib/x86/cpu_features.c"
#include "../third_party/libdeflate/lib/adler32.c"
#include "../third_party/libdeflate/lib/deflate_decompress.c"
#include "../third_party/libdeflate/lib/zlib_decompress.c"

bool G3O_AVX2_IsSupported(void) {
    int cpuInfo[4] = {0};
    __cpuid(cpuInfo, 0);
    int nIds = cpuInfo[0];
    if (nIds < 7) return false;

    // Check OSXSAVE support (XMM/YMM state enabled by OS)
    __cpuid(cpuInfo, 1);
    bool osxsave = (cpuInfo[2] & (1 << 27)) != 0;
    if (!osxsave) return false;

    // Check XGETBV for XMM and YMM enabled by OS
    unsigned __int64 xcr0 = _xgetbv(0);
    if ((xcr0 & 0x6) != 0x6) return false;

    // Check AVX2 and BMI2 in feature leaf 7
    __cpuidex(cpuInfo, 7, 0);
    bool avx2 = (cpuInfo[1] & (1 << 5)) != 0;
    bool bmi2 = (cpuInfo[1] & (1 << 8)) != 0;

    return avx2 && bmi2;
}

struct libdeflate_decompressor* G3O_AVX2_AllocDecompressor(void) {
    return libdeflate_alloc_decompressor_avx2();
}

void G3O_AVX2_FreeDecompressor(struct libdeflate_decompressor* d) {
    if (d) {
        libdeflate_free_decompressor_avx2(d);
    }
}

int G3O_AVX2_DecompressZlib(
    struct libdeflate_decompressor* d,
    const void* in,
    size_t in_bytes,
    void* out,
    size_t out_capacity,
    size_t* actual_out_bytes
) {
    return (int)libdeflate_zlib_decompress_avx2(
        d,
        in,
        in_bytes,
        out,
        out_capacity,
        actual_out_bytes
    );
}

int G3O_AVX2_DecompressDeflate(
    struct libdeflate_decompressor* d,
    const void* in,
    size_t in_bytes,
    void* out,
    size_t out_capacity,
    size_t* actual_out_bytes
) {
    return (int)libdeflate_deflate_decompress_avx2(
        d,
        in,
        in_bytes,
        out,
        out_capacity,
        actual_out_bytes
    );
}
