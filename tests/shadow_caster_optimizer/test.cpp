#include "../../src/shadow_caster_policy.h"
#include <iostream>
#include <vector>
#include <cstdlib>
#include <cassert>

using namespace G3O;

static bool Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        return false;
    }
    return true;
}

// Mock layout matching eCShadowMapDirectionalLightBase
struct MockDirectionalLight {
    uint8_t   pad[OFFSET_CASTER_ARRAY_PTR]; // 0x68 bytes padding
    void**    m_pArray;                     // +0x68
    uint32_t  m_u32Count;                   // +0x6C
    uint32_t  m_u32Capacity;                // +0x70
};

int main() {
    bool ok = true;
    std::cout << "[Test] Shadow Caster Optimizer Policy Suite starting...\n";

    // Static table test
    static ShadowIndexMap::Entry s_table[ShadowIndexMap::kCapacity] = {};
    ShadowIndexMap::Clear(s_table);

    void* lightA = reinterpret_cast<void*>(0x10001000);
    void* lightB = reinterpret_cast<void*>(0x20002000);
    void* caster1 = reinterpret_cast<void*>(0x00A00001);
    void* caster2 = reinterpret_cast<void*>(0x00A00002);
    void* caster3 = reinterpret_cast<void*>(0x00A00003);

    // 1. Initial lookup on empty table
    uint32_t idx = 999;
    ok &= Expect(!ShadowIndexMap::FindIndex(s_table, lightA, caster1, idx), "empty table must return false");

    // 2. Insert and find
    ShadowIndexMap::Upsert(s_table, lightA, caster1, 0);
    ok &= Expect(ShadowIndexMap::FindIndex(s_table, lightA, caster1, idx) && idx == 0, "caster1 on lightA should be at index 0");
    ok &= Expect(!ShadowIndexMap::FindIndex(s_table, lightB, caster1, idx), "caster1 on lightB must not match lightA");

    // 3. Insert caster2 and caster3
    ShadowIndexMap::Upsert(s_table, lightA, caster2, 1);
    ShadowIndexMap::Upsert(s_table, lightA, caster3, 2);
    ok &= Expect(ShadowIndexMap::FindIndex(s_table, lightA, caster2, idx) && idx == 1, "caster2 on lightA should be at index 1");
    ok &= Expect(ShadowIndexMap::FindIndex(s_table, lightA, caster3, idx) && idx == 2, "caster3 on lightA should be at index 2");

    // 4. Update index (simulate Swap-and-Pop relocation)
    ShadowIndexMap::Upsert(s_table, lightA, caster3, 0);
    ok &= Expect(ShadowIndexMap::FindIndex(s_table, lightA, caster3, idx) && idx == 0, "caster3 relocated to index 0");

    // 5. Erase
    ShadowIndexMap::Erase(s_table, lightA, caster1);
    ok &= Expect(!ShadowIndexMap::FindIndex(s_table, lightA, caster1, idx), "erased caster1 must not be found");
    // Other items should still be found
    ok &= Expect(ShadowIndexMap::FindIndex(s_table, lightA, caster2, idx) && idx == 1, "caster2 still at index 1");
    ok &= Expect(ShadowIndexMap::FindIndex(s_table, lightA, caster3, idx) && idx == 0, "caster3 still at index 0");

    // 6. Re-insert into tombstone
    ShadowIndexMap::Upsert(s_table, lightA, caster1, 42);
    ok &= Expect(ShadowIndexMap::FindIndex(s_table, lightA, caster1, idx) && idx == 42, "re-inserted caster1 at index 42");

    // 7. High-volume Swap-and-Pop simulation with mock light array
    ShadowIndexMap::Clear(s_table);

    MockDirectionalLight mockLight{};
    mockLight.m_u32Capacity = 2048;
    mockLight.m_u32Count = 0;
    mockLight.m_pArray = reinterpret_cast<void**>(std::malloc(mockLight.m_u32Capacity * sizeof(void*)));

    // Insert 1000 items
    constexpr uint32_t kNumItems = 1000;
    std::vector<void*> testEntities;
    testEntities.reserve(kNumItems);
    for (uint32_t i = 0; i < kNumItems; ++i) {
        void* entity = reinterpret_cast<void*>(static_cast<uintptr_t>(0x30000000 + i * 4));
        testEntities.push_back(entity);

        // Append to mock array
        mockLight.m_pArray[mockLight.m_u32Count] = entity;
        ShadowIndexMap::Upsert(s_table, &mockLight, entity, mockLight.m_u32Count);
        mockLight.m_u32Count++;
    }
    ok &= Expect(mockLight.m_u32Count == kNumItems, "all 1000 items inserted");

    // Check duplicate rejection
    uint32_t dupIdx = 0;
    for (uint32_t i = 0; i < 100; ++i) {
        void* dupEntity = testEntities[i];
        bool found = ShadowIndexMap::FindIndex(s_table, &mockLight, dupEntity, dupIdx);
        ok &= Expect(found && dupIdx < mockLight.m_u32Count && mockLight.m_pArray[dupIdx] == dupEntity,
                     "duplicate entity correctly identified in O(1)");
    }

    // Remove 500 items using Swap-and-Pop
    for (uint32_t i = 0; i < 500; ++i) {
        void* toRemove = testEntities[i * 2]; // remove every second item
        uint32_t targetIdx = 0;
        bool found = ShadowIndexMap::FindIndex(s_table, &mockLight, toRemove, targetIdx);
        ok &= Expect(found, "toRemove must be found in index map");

        if (found && targetIdx < mockLight.m_u32Count && mockLight.m_pArray[targetIdx] == toRemove) {
            const uint32_t lastIdx = mockLight.m_u32Count - 1;
            void* lastEntity = mockLight.m_pArray[lastIdx];

            // Swap-and-pop
            mockLight.m_pArray[targetIdx] = lastEntity;
            mockLight.m_u32Count = lastIdx;

            ShadowIndexMap::Erase(s_table, &mockLight, toRemove);
            if (targetIdx != lastIdx) {
                ShadowIndexMap::Upsert(s_table, &mockLight, lastEntity, targetIdx);
            }
        }
    }
    ok &= Expect(mockLight.m_u32Count == 500, "500 items remaining after removals");

    // Verify all remaining items in mock array match their index in ShadowIndexMap
    for (uint32_t i = 0; i < mockLight.m_u32Count; ++i) {
        void* entity = mockLight.m_pArray[i];
        uint32_t foundIdx = 0;
        bool found = ShadowIndexMap::FindIndex(s_table, &mockLight, entity, foundIdx);
        ok &= Expect(found && foundIdx == i, "remaining item index in array matches ShadowIndexMap");
    }

    std::free(mockLight.m_pArray);

    if (ok) {
        std::cout << "[Test] Shadow Caster Optimizer Policy Suite PASSED!\n";
        return 0;
    } else {
        std::cerr << "[Test] Shadow Caster Optimizer Policy Suite FAILED!\n";
        return 1;
    }
}
