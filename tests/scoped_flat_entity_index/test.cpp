#include <windows.h>
#include <cstdint>
#include <vector>
#include <iostream>
#include <cassert>
#include <cstring>
#include <random>

enum SlotState : uint8_t {
    State_Empty = 0,
    State_Occupied = 1,
    State_Tombstone = 2
};

struct FlatSlot {
    uint32_t key[5];
    void* pNode;
    void* pMap;
    uint8_t state;
    uint8_t pad[3];
};

static constexpr size_t kTableSize = 131072; // 1 << 17
static constexpr size_t kTableMask = kTableSize - 1;

static inline uint32_t HashPropertyID(const void* pMap, const void* pKey) {
    const uint32_t* k = reinterpret_cast<const uint32_t*>(pKey);
    uint32_t h = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pMap)) ^ 0x9e3779b9;
    h = (h ^ k[0]) * 0x01000193;
    h = (h ^ k[1]) * 0x01000193;
    h = (h ^ k[2]) * 0x01000193;
    h = (h ^ k[3]) * 0x01000193;
    h = (h ^ k[4]) * 0x01000193;
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    return h;
}

class TestFlatIndex {
public:
    FlatSlot* m_slots = nullptr;
    size_t m_count = 0;

    TestFlatIndex() {
        m_slots = reinterpret_cast<FlatSlot*>(VirtualAlloc(NULL, kTableSize * sizeof(FlatSlot), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    }
    ~TestFlatIndex() {
        if (m_slots) VirtualFree(m_slots, 0, MEM_RELEASE);
    }

    void Upsert(void* pMap, const void* pKey, void* pNode) {
        const uint32_t* k = reinterpret_cast<const uint32_t*>(pKey);
        uint32_t idx = HashPropertyID(pMap, pKey) & kTableMask;
        uint32_t firstTombstone = static_cast<uint32_t>(-1);

        while (true) {
            auto& slot = m_slots[idx];
            if (slot.state == State_Empty) {
                uint32_t targetIdx = (firstTombstone != static_cast<uint32_t>(-1)) ? firstTombstone : idx;
                auto& targetSlot = m_slots[targetIdx];
                targetSlot.key[0] = k[0];
                targetSlot.key[1] = k[1];
                targetSlot.key[2] = k[2];
                targetSlot.key[3] = k[3];
                targetSlot.key[4] = k[4];
                targetSlot.pNode = pNode;
                targetSlot.pMap = pMap;
                targetSlot.state = State_Occupied;
                m_count++;
                return;
            }
            if (slot.state == State_Tombstone && firstTombstone == static_cast<uint32_t>(-1)) {
                firstTombstone = idx;
            } else if (slot.state == State_Occupied && slot.pMap == pMap) {
                if (slot.key[0] == k[0] && slot.key[1] == k[1] && slot.key[2] == k[2] &&
                    slot.key[3] == k[3] && slot.key[4] == k[4]) {
                    slot.pNode = pNode;
                    return;
                }
            }
            idx = (idx + 1) & kTableMask;
        }
    }

    void* Find(void* pMap, const void* pKey) {
        const uint32_t* k = reinterpret_cast<const uint32_t*>(pKey);
        uint32_t idx = HashPropertyID(pMap, pKey) & kTableMask;

        while (true) {
            const auto& slot = m_slots[idx];
            if (slot.state == State_Empty) {
                return nullptr;
            }
            if (slot.state == State_Occupied && slot.pMap == pMap) {
                if (slot.key[0] == k[0] && slot.key[1] == k[1] && slot.key[2] == k[2] &&
                    slot.key[3] == k[3] && slot.key[4] == k[4]) {
                    return slot.pNode;
                }
            }
            idx = (idx + 1) & kTableMask;
        }
    }

    bool Remove(void* pMap, const void* pKey) {
        const uint32_t* k = reinterpret_cast<const uint32_t*>(pKey);
        uint32_t idx = HashPropertyID(pMap, pKey) & kTableMask;

        while (true) {
            auto& slot = m_slots[idx];
            if (slot.state == State_Empty) {
                return false;
            }
            if (slot.state == State_Occupied && slot.pMap == pMap) {
                if (slot.key[0] == k[0] && slot.key[1] == k[1] && slot.key[2] == k[2] &&
                    slot.key[3] == k[3] && slot.key[4] == k[4]) {
                    slot.state = State_Tombstone;
                    if (m_count > 0) m_count--;
                    return true;
                }
            }
            idx = (idx + 1) & kTableMask;
        }
    }
};

struct Key20 {
    uint32_t d[5];
};

static bool Expect(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "FAIL: " << msg << "\n";
        return false;
    }
    return true;
}

int main() {
    bool ok = true;
    std::cout << "[Test] ScopedFlatEntityIndex benchmark & validation starting...\n";

    TestFlatIndex idx;
    ok &= Expect(idx.m_slots != nullptr, "VirtualAlloc 4 MB buffer");

    constexpr size_t kItems = 35000;
    std::vector<Key20> keys(kItems);
    std::mt19937 rng(1337);

    void* pMap1 = reinterpret_cast<void*>(0x12340000);
    void* pMap2 = reinterpret_cast<void*>(0x56780000);

    for (size_t i = 0; i < kItems; ++i) {
        for (int j = 0; j < 5; ++j) keys[i].d[j] = rng();
    }

    // 1. Insert 35,000 items
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    for (size_t i = 0; i < kItems; ++i) {
        void* pMap = (i % 5 == 0) ? pMap2 : pMap1;
        void* pNode = reinterpret_cast<void*>(0x10000000 + i * 32);
        idx.Upsert(pMap, &keys[i], pNode);
    }

    QueryPerformanceCounter(&t1);
    double insertMs = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
    std::cout << "[Test] Inserted " << kItems << " items in " << insertMs << " ms ("
              << (insertMs * 1000.0 / kItems) << " us/item)\n";
    ok &= Expect(insertMs < 50.0, "Insert 35k items should take < 50 ms");

    // 2. Lookup all 35,000 items
    QueryPerformanceCounter(&t0);
    for (size_t i = 0; i < kItems; ++i) {
        void* pMap = (i % 5 == 0) ? pMap2 : pMap1;
        void* expected = reinterpret_cast<void*>(0x10000000 + i * 32);
        void* actual = idx.Find(pMap, &keys[i]);
        if (actual != expected) {
            ok &= Expect(false, "Found node must match expected pointer");
            break;
        }
    }
    QueryPerformanceCounter(&t1);
    double lookupMs = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
    std::cout << "[Test] Looked up " << kItems << " items in " << lookupMs << " ms (100% hits, "
              << (lookupMs * 1000.0 / kItems) << " ns/lookup)\n";
    ok &= Expect(lookupMs < 20.0, "35k lookups should take < 20 ms");

    // 3. Test non-existent keys (must return nullptr)
    for (size_t i = 0; i < 5000; ++i) {
        Key20 fakeKey;
        for (int j = 0; j < 5; ++j) fakeKey.d[j] = rng();
        void* found = idx.Find(pMap1, &fakeKey);
        ok &= Expect(found == nullptr, "Non-existent key must return nullptr");
    }

    // 4. Remove 2,000 items (tombstones)
    for (size_t i = 0; i < 2000; ++i) {
        void* pMap = (i % 5 == 0) ? pMap2 : pMap1;
        bool removed = idx.Remove(pMap, &keys[i]);
        ok &= Expect(removed, "Remove existing key should return true");
        void* found = idx.Find(pMap, &keys[i]);
        ok &= Expect(found == nullptr, "Removed key must return nullptr");
    }

    // 5. Verify remaining items still hit
    for (size_t i = 2000; i < 5000; ++i) {
        void* pMap = (i % 5 == 0) ? pMap2 : pMap1;
        void* expected = reinterpret_cast<void*>(0x10000000 + i * 32);
        void* actual = idx.Find(pMap, &keys[i]);
        ok &= Expect(actual == expected, "Remaining items must still hit after removals");
    }

    // 6. Upsert into tombstone slots
    for (size_t i = 0; i < 1000; ++i) {
        void* pMap = (i % 5 == 0) ? pMap2 : pMap1;
        void* pNewNode = reinterpret_cast<void*>(0x20000000 + i * 32);
        idx.Upsert(pMap, &keys[i], pNewNode);
        void* actual = idx.Find(pMap, &keys[i]);
        ok &= Expect(actual == pNewNode, "Upsert over tombstone must find new node");
    }

    if (ok) {
        std::cout << "[Test] ScopedFlatEntityIndex validation: PASS!\n";
        return 0;
    }
    return 1;
}
