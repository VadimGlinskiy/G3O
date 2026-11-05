#pragma once
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool G3O_AVX2_IsSupported(void);

struct libdeflate_decompressor;

struct libdeflate_decompressor* G3O_AVX2_AllocDecompressor(void);
void G3O_AVX2_FreeDecompressor(struct libdeflate_decompressor* d);

int G3O_AVX2_DecompressZlib(
    struct libdeflate_decompressor* d,
    const void* in,
    size_t in_bytes,
    void* out,
    size_t out_capacity,
    size_t* actual_out_bytes
);

int G3O_AVX2_DecompressDeflate(
    struct libdeflate_decompressor* d,
    const void* in,
    size_t in_bytes,
    void* out,
    size_t out_capacity,
    size_t* actual_out_bytes
);

#ifdef __cplusplus
}
#endif
