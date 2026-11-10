#include "../../src/class_factory_policy.h"
#include <iostream>
#include <vector>
#include <string>
#include <cassert>

using namespace G3O;

static bool Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        return false;
    }
    return true;
}

int main() {
    bool ok = true;
    std::cout << "[Test] Class Factory Optimizer Policy Suite starting...\n";

    // 1. Prefix tests
    {
        uint64_t pref1 = ReadPrefix8("eCVisual", 8);
        uint64_t pref2 = ReadPrefix8("eCVisualMesh_PS", 15);
        ok &= Expect(pref1 == pref2, "prefix of 8 and 15 chars starting with eCVisual must match");

        uint64_t prefShort = ReadPrefix8("abc", 3);
        uint64_t prefShort2 = 0;
        std::memcpy(&prefShort2, "abc", 3);
        ok &= Expect(prefShort == prefShort2, "short prefix must match byte copy");
    }

    // 2. Hash computation tests
    {
        uint32_t h1 = ComputeFastClassHash("eCEntity", 8);
        uint32_t h2 = ComputeFastClassHash("eCEntity", 8);
        uint32_t h3 = ComputeFastClassHash("eCVisualMesh_PS", 15);
        ok &= Expect(h1 != 0, "hash must not be zero");
        ok &= Expect(h1 == h2, "hash must be deterministic");
        ok &= Expect(h1 != h3, "different classes should have different hashes");
    }

    // 3. Cache Insertion & Lookup
    static SClassNodeCacheEntry s_table[kClassCacheCapacity] = {};
    static char s_arena[8192] = {};
    uint32_t arenaOffset = 0;

    void* mockMapA = reinterpret_cast<void*>(0x11110000);
    void* mockMapB = reinterpret_cast<void*>(0x22220000);

    const char* kClasses[] = {
        "eCEntity",
        "eCVisualMesh_PS",
        "eCVisualAnimation_PS",
        "eCSpatialEntity",
        "eCCollisionShape_PS",
        "eCRigidBody_PS",
        "eCDynamicLight_PS",
        "bTPropertyContainer<enum eEPropertySetType>",
        "eCParticleFX_PS",
        "eCNavigation_PS"
    };
    constexpr uint32_t kNumClasses = sizeof(kClasses) / sizeof(kClasses[0]);

    // Initial lookup must miss
    for (uint32_t i = 0; i < kNumClasses; ++i) {
        const char* name = kClasses[i];
        uint32_t len = static_cast<uint32_t>(std::strlen(name));
        uint32_t h = ComputeFastClassHash(name, len);
        uint64_t pref = ReadPrefix8(name, len);
        const SClassNodeCacheEntry* e = FindInNodeCache(s_table, mockMapA, name, len, h, pref);
        ok &= Expect(e == nullptr, "initial lookup on empty cache must return nullptr");
    }

    // Insert classes into MapA
    for (uint32_t i = 0; i < kNumClasses; ++i) {
        const char* name = kClasses[i];
        uint32_t len = static_cast<uint32_t>(std::strlen(name));
        uint32_t h = ComputeFastClassHash(name, len);
        uint64_t pref = ReadPrefix8(name, len);
        void* mockNode = reinterpret_cast<void*>(static_cast<uintptr_t>(0x30000000 + i * 0x100));

        const SClassNodeCacheEntry* inserted = InsertIntoNodeCache(
            s_table, s_arena, arenaOffset, sizeof(s_arena),
            mockMapA, name, len, h, pref, mockNode, i % 43);
        ok &= Expect(inserted != nullptr, "insert must succeed");
        ok &= Expect(inserted->u32Hash == h, "inserted hash must match");
        ok &= Expect(inserted->pNode == mockNode, "inserted node must match");
        ok &= Expect(inserted->u32Bucket == (i % 43), "inserted bucket must match");
    }

    // Lookup inserted classes in MapA
    for (uint32_t i = 0; i < kNumClasses; ++i) {
        const char* name = kClasses[i];
        uint32_t len = static_cast<uint32_t>(std::strlen(name));
        uint32_t h = ComputeFastClassHash(name, len);
        uint64_t pref = ReadPrefix8(name, len);
        void* expectedNode = reinterpret_cast<void*>(static_cast<uintptr_t>(0x30000000 + i * 0x100));

        const SClassNodeCacheEntry* e = FindInNodeCache(s_table, mockMapA, name, len, h, pref);
        ok &= Expect(e != nullptr, "lookup must find inserted class");
        if (e) {
            ok &= Expect(e->pNode == expectedNode, "found node must match expected");
            ok &= Expect(std::strcmp(e->pszKey, name) == 0, "found class name must match");
        }

        // Map isolation check: MapB must NOT find MapA's classes
        const SClassNodeCacheEntry* eB = FindInNodeCache(s_table, mockMapB, name, len, h, pref);
        ok &= Expect(eB == nullptr, "lookup with different map must return nullptr");
    }

    // Duplicate insert must not duplicate in arena
    uint32_t savedArenaOffset = arenaOffset;
    {
        const char* name = kClasses[0];
        uint32_t len = static_cast<uint32_t>(std::strlen(name));
        uint32_t h = ComputeFastClassHash(name, len);
        uint64_t pref = ReadPrefix8(name, len);
        void* mockNode = reinterpret_cast<void*>(0x30000000);

        const SClassNodeCacheEntry* reinserted = InsertIntoNodeCache(
            s_table, s_arena, arenaOffset, sizeof(s_arena),
            mockMapA, name, len, h, pref, mockNode, 0);
        ok &= Expect(reinserted != nullptr, "duplicate insert should return valid entry");
        ok &= Expect(arenaOffset == savedArenaOffset, "duplicate insert must not consume arena space");
    }

    // Lookup non-existent class
    {
        const char* missing = "eCUnknownMissingClass";
        uint32_t len = static_cast<uint32_t>(std::strlen(missing));
        uint32_t h = ComputeFastClassHash(missing, len);
        uint64_t pref = ReadPrefix8(missing, len);
        const SClassNodeCacheEntry* e = FindInNodeCache(s_table, mockMapA, missing, len, h, pref);
        ok &= Expect(e == nullptr, "missing class lookup must return nullptr");
    }

    // 4. High-volume lookup stress test (simulating SysDyn 60,000 objects)
    for (int rep = 0; rep < 1000; ++rep) {
        for (uint32_t i = 0; i < kNumClasses; ++i) {
            const char* name = kClasses[i];
            uint32_t len = static_cast<uint32_t>(std::strlen(name));
            uint32_t h = ComputeFastClassHash(name, len);
            uint64_t pref = ReadPrefix8(name, len);
            const SClassNodeCacheEntry* e = FindInNodeCache(s_table, mockMapA, name, len, h, pref);
            assert(e != nullptr);
        }
    }
    std::cout << "[Test] High-volume 10,000 lookups completed successfully.\n";

    if (ok) {
        std::cout << "[Test] Class Factory Optimizer Policy Suite PASSED!\n";
        return 0;
    } else {
        std::cerr << "[Test] Class Factory Optimizer Policy Suite FAILED!\n";
        return 1;
    }
}
