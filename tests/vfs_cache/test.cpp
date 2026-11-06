#include "vfs_cache_core.h"
#include <cstdio>
#include <cstring>
#include <windows.h>

struct StressContext {
    G3O::VfsLookupCache<4>* cache;
    const int* first;
    const int* second;
    volatile LONG failed;
};

static DWORD WINAPI Writer(LPVOID parameter) {
    auto& context = *static_cast<StressContext*>(parameter);
    for (int i = 0; i < 20000; ++i) {
        context.cache->Store(1, "stress-path", 11, (i & 1) ? context.first : context.second);
        if ((i & 7) == 0) context.cache->Clear();
    }
    return 0;
}

static DWORD WINAPI Reader(LPVOID parameter) {
    auto& context = *static_cast<StressContext*>(parameter);
    for (int i = 0; i < 20000; ++i) {
        const void* value = context.cache->Find(1, "stress-path", 11);
        if (value && value != context.first && value != context.second) {
            InterlockedExchange(&context.failed, 1);
        }
    }
    return 0;
}

static bool Expect(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

int main() {
    G3O::VfsLookupCache<4> cache;
    const int first = 1;
    const int second = 2;
    bool ok = true;

    cache.Store(7, "Game\\Path.pak", 13, &first);
    ok &= Expect(cache.Find(7, "game\\path.PAK", 13) == &first,
                 "case-insensitive path should return its cached result");
    ok &= Expect(cache.Find(7, "other\\path.pak", 14) == nullptr,
                 "hash collisions must verify the full path");
    cache.Clear();
    ok &= Expect(cache.Find(7, "Game\\Path.pak", 13) == nullptr,
                 "clearing the cache should invalidate entries");

    StressContext context{ &cache, &first, &second, 0 };
    HANDLE writer = CreateThread(nullptr, 0, Writer, &context, 0, nullptr);
    HANDLE reader = CreateThread(nullptr, 0, Reader, &context, 0, nullptr);
    if (!writer || !reader) {
        if (writer) { WaitForSingleObject(writer, INFINITE); CloseHandle(writer); }
        if (reader) { WaitForSingleObject(reader, INFINITE); CloseHandle(reader); }
        return 2;
    }
    WaitForSingleObject(writer, INFINITE);
    WaitForSingleObject(reader, INFINITE);
    CloseHandle(writer);
    CloseHandle(reader);
    ok &= Expect(context.failed == 0, "concurrent cache reads and writes should remain valid");

    if (!ok) return 1;
    std::puts("PASS: VFS cache lookup, collision check, invalidation, and concurrent access");
    return 0;
}
