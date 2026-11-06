#include "pak_prefetch_policy.h"
#include <cstdio>

static bool Expect(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

int main() {
    bool ok = true;
    ok &= Expect(!G3O::ShouldTryPredecompressedLookup(false, false),
                 "inactive tracer must not attempt cache lookup");
    ok &= Expect(!G3O::ShouldTryPredecompressedLookup(false, true),
                 "inactive tracer must not attempt cache lookup even if workers were started");
    ok &= Expect(!G3O::ShouldTryPredecompressedLookup(true, false),
                 "trace-only loads without workers must skip speculative cache lookup");
    ok &= Expect(G3O::ShouldTryPredecompressedLookup(true, true),
                 "active loads with prefetch workers must retain cache lookup");

    if (!ok) return 1;
    std::puts("PASS: speculative cache lookup is gated on active prefetch workers");
    return 0;
}
