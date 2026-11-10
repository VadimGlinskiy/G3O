#include <mimalloc.h>
#include <cstdio>

int main() {
    mi_option_set(mi_option_purge_delay, -1);
    mi_option_set(mi_option_purge_decommits, 0);

    size_t sizes[] = { 16, 64, 256, 1024, 4096, 16384, 65536, 128*1024, 256*1024, 512*1024, 1024*1024, 2*1024*1024, 4*1024*1024, 8*1024*1024, 16*1024*1024, 32*1024*1024, 64*1024*1024 };
    for (size_t s : sizes) {
        void* p = mi_malloc(s);
        bool in_heap = mi_is_in_heap_region(p);
        std::printf("size %8u: p = %p, mi_is_in_heap_region = %d\n", (unsigned)s, p, in_heap ? 1 : 0);
        mi_free(p);
    }
    return 0;
}
