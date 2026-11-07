#include "pak_read_history.h"
#include <cstdint>
#include <cstdio>

struct TestRead {
    const void* buffer = nullptr;
    std::uint32_t bytesRead = 0;
    std::uint64_t offset = 0;
    bool valid = false;
    std::uint64_t loadGeneration = 1;
};

static bool Expect(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

int main() {
    using History = G3O::RecentReadHistory<TestRead, 3>;
    auto address = [](std::uintptr_t value) {
        return reinterpret_cast<const void*>(value);
    };

    bool ok = true;
    History history;
    const void* firstBuffer = address(0x1000);
    const void* secondBuffer = address(0x2000);
    history.Push(TestRead{firstBuffer, 8, 0x100, true});
    history.Push(TestRead{secondBuffer, 4, 0x200, true});

    TestRead* match = history.FindMostRecentExact(firstBuffer, 8, 1);
    ok &= Expect(match && match->offset == 0x100,
                 "an intervening header read must not hide the earlier block read");
    ok &= Expect(history.Consume(match), "a matched read should be consumable");
    ok &= Expect(history.FindMostRecentExact(firstBuffer, 8, 1) == nullptr,
                 "a consumed read must not match twice");
    ok &= Expect(history.FindMostRecentExact(secondBuffer, 4, 1) != nullptr,
                 "consuming one read must preserve unrelated recent reads");

    history.Clear();
    history.Push(TestRead{firstBuffer, 8, 0x300, true});
    history.Push(TestRead{secondBuffer, 4, 0x400, true});
    history.Push(TestRead{firstBuffer, 8, 0x500, true});
    match = history.FindMostRecentExact(firstBuffer, 8, 1);
    ok &= Expect(match && match->offset == 0x500,
                 "the newest read must win when a buffer address is reused");
    history.Consume(match);
    ok &= Expect(history.FindMostRecentExact(firstBuffer, 8, 1) == nullptr,
                 "reused buffer addresses must not fall back to stale history");

    history.Clear();
    history.Push(TestRead{address(0x3000), 1, 1, true});
    history.Push(TestRead{address(0x4000), 2, 2, true});
    history.Push(TestRead{address(0x5000), 3, 3, true});
    history.Push(TestRead{address(0x6000), 4, 4, true});
    ok &= Expect(history.FindMostRecentExact(address(0x3000), 1, 1) == nullptr,
                 "a full ring must evict its oldest read");
    ok &= Expect(history.FindMostRecentExact(address(0x6000), 4, 1) != nullptr,
                 "a wrapped ring must retain its newest read");
    ok &= Expect(history.FindMostRecentExact(nullptr, 4, 1) == nullptr
                 && history.FindMostRecentExact(address(0x6000), 0, 1) == nullptr,
                 "null buffers and empty reads must never match");

    history.Clear();
    history.Push(TestRead{firstBuffer, 8, 0x700, true, 1});
    ok &= Expect(history.FindMostRecentExact(firstBuffer, 8, 2) == nullptr,
                 "a read from a previous load generation must never match");

    ok &= Expect(G3O::PakReadIdentityMatches("Data.pak", 0x800, 256,
                                               "data.PAK", 0x800, 256),
                 "matching archive, offset, and compressed size should be accepted");
    ok &= Expect(!G3O::PakReadIdentityMatches("Data.pak", 0x800, 256,
                                                "Other.pak", 0x800, 256)
                 && !G3O::PakReadIdentityMatches("Data.pak", 0x800, 256,
                                                  "Data.pak", 0x801, 256)
                 && !G3O::PakReadIdentityMatches("Data.pak", 0x800, 256,
                                                  "Data.pak", 0x800, 255),
                 "archive, offset, and compressed-size mismatches must be rejected");

    return ok ? 0 : 1;
}
