#include <mimalloc.h>
#include <cstdio>
#include <windows.h>

int main() {
    mi_option_set(mi_option_arena_reserve, 2 * 1024);
    mi_option_set(mi_option_arena_max_object_size, 64);
    mi_option_set(mi_option_arena_eager_commit, 0);
    mi_option_set(mi_option_purge_decommits, 1);
    mi_option_set(mi_option_purge_delay, 100);
    mi_option_set(mi_option_retry_on_oom, 0);

    mi_arena_id_t arena_id = 0;
    int res = mi_reserve_os_memory_ex(16 * 1024 * 1024, false, false, false, &arena_id);
    
    // Allocate something around 16KB-64KB
    void* p = mi_malloc(16384);
    std::printf("p = %p, in_region = %d\n", p, mi_is_in_heap_region(p));

    // Allocate 100 different sizes
    for (int i = 1; i <= 20; i++) {
        void* ptr = mi_malloc(i * 4096);
        if (!mi_is_in_heap_region(ptr)) {
            std::printf("FAILED for size %d: ptr = %p, in_region = 0!\n", i * 4096, ptr);
        }
        mi_free(ptr);
    }
    std::puts("Done testing allocations.");
    return 0;
}
