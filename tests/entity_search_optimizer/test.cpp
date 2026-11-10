#include "entity_search_policy.h"
#include <cstdio>
#include <cstdint>
#include <vector>

static bool Expect(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

static uint32_t s_virtualCallCount = 0;

#if defined(_MSC_VER) && defined(_M_IX86)
static uint32_t __fastcall MockGetType(void* pThis, void* /*edx*/) {
    ++s_virtualCallCount;
    return *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(pThis) + sizeof(void*));
}
#else
static uint32_t MockGetType(void* pThis) {
    ++s_virtualCallCount;
    return *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(pThis) + sizeof(void*));
}
#endif

struct MockElement {
    uintptr_t* pVTable;
    uint32_t   type;
};

int main() {
    bool ok = true;

    // Build vtables for a few distinct types
    // Note: each component type in Genome has its own static vtable
    static uintptr_t vtableA[32] = {};
    static uintptr_t vtableB[32] = {};
    static uintptr_t vtableC[32] = {};
    static uintptr_t vtableD[32] = {};

    vtableA[24] = reinterpret_cast<uintptr_t>(&MockGetType);
    vtableB[24] = reinterpret_cast<uintptr_t>(&MockGetType);
    vtableC[24] = reinterpret_cast<uintptr_t>(&MockGetType);
    vtableD[24] = reinterpret_cast<uintptr_t>(&MockGetType);

    MockElement elemA = { vtableA, 10 };
    MockElement elemB = { vtableB, 20 };
    MockElement elemC = { vtableC, 30 };
    MockElement elemD = { vtableD, 40 };

    void* smallArray[4] = { &elemA, &elemB, &elemC, &elemD };

    G3O::ResetVTableTypeCache();
    s_virtualCallCount = 0;

    // Test 1: Linear scan with Early Exit (count <= 8)
    ok &= Expect(G3O::FastSearchPropertySet(smallArray, 4, 10) == 0, "Find first element (10) at index 0");
    ok &= Expect(G3O::FastSearchPropertySet(smallArray, 4, 20) == 1, "Find second element (20) at index 1");
    ok &= Expect(G3O::FastSearchPropertySet(smallArray, 4, 30) == 2, "Find third element (30) at index 2");
    ok &= Expect(G3O::FastSearchPropertySet(smallArray, 4, 40) == 3, "Find fourth element (40) at index 3");
    ok &= Expect(G3O::FastSearchPropertySet(smallArray, 4, 99) == 0xFFFF, "Missing element > max returns 0xFFFF");
    ok &= Expect(G3O::FastSearchPropertySet(smallArray, 4, 5) == 0xFFFF, "Missing element < min returns 0xFFFF");
    ok &= Expect(G3O::FastSearchPropertySet(smallArray, 4, 25) == 0xFFFF, "Missing element between elements returns 0xFFFF");

    // Test 2: Verify L1 vptr-cache hit rate
    // With 4 distinct vtables, exactly 4 virtual calls should have occurred even after 7 searches
    ok &= Expect(s_virtualCallCount == 4, "L1 cache should serve repeated vptr lookups without virtual calls");

    // Repeat lookups 100 times - call count must remain exactly 4
    for (int i = 0; i < 100; ++i) {
        G3O::FastSearchPropertySet(smallArray, 4, 20);
    }
    ok &= Expect(s_virtualCallCount == 4, "Repeated queries must generate 0 additional virtual calls");

    // Test 3: Large array binary search (count > 8)
    static uintptr_t vtablesLarge[16][32] = {};
    std::vector<MockElement> largeElements;
    std::vector<void*> largeArray;
    for (uint32_t i = 0; i < 16; ++i) {
        vtablesLarge[i][24] = reinterpret_cast<uintptr_t>(&MockGetType);
        largeElements.push_back({ vtablesLarge[i], (i + 1) * 10 }); // 10, 20, 30, ... 160
    }
    for (size_t i = 0; i < largeElements.size(); ++i) {
        largeArray.push_back(&largeElements[i]);
    }

    for (uint16_t i = 0; i < 16; ++i) {
        uint16_t found = G3O::FastSearchPropertySet(largeArray.data(), 16, (i + 1) * 10);
        ok &= Expect(found == i, "Binary search find element in large array");
    }
    ok &= Expect(G3O::FastSearchPropertySet(largeArray.data(), 16, 5) == 0xFFFF, "Binary search missing < min");
    ok &= Expect(G3O::FastSearchPropertySet(largeArray.data(), 16, 165) == 0xFFFF, "Binary search missing > max");
    ok &= Expect(G3O::FastSearchPropertySet(largeArray.data(), 16, 25) == 0xFFFF, "Binary search missing in-between");

    // Test 4: Edge cases
    ok &= Expect(G3O::FastSearchPropertySet(nullptr, 0, 10) == 0xFFFF, "Null array returns 0xFFFF");
    ok &= Expect(G3O::FastSearchPropertySet(smallArray, 0, 10) == 0xFFFF, "Zero count returns 0xFFFF");

    if (!ok) return 1;
    std::puts("PASS: Entity search policy fast linear scan, L1 vptr cache, and binary search verified.");
    return 0;
}
