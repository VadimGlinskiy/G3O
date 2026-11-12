#include <windows.h>
#include <iostream>
#include <cstdint>
#include <cstring>
#include <vector>
#include <atomic>
#include "../../src/scene_node_policy.h"

#pragma pack(push, 1)
struct MockNode {
    void*     vtable;       // +0x00
    uint32_t  pad1[2];      // +0x04, +0x08
    void*     m_pParent;    // +0x0C
    uint16_t  m_u16Capacity;// +0x10
    uint16_t  m_u16Count;   // +0x12
    void**    m_ppChildren; // +0x14
};
#pragma pack(pop)

static std::atomic<uint32_t> s_mockReallocCount{ 0 };
static uint16_t s_mockOldCapacity = 0;

static void* MockRealloc(void* /*pAdmin*/, void* pOld, uint32_t newSize) {
    s_mockReallocCount.fetch_add(1, std::memory_order_relaxed);
    void* pNew = std::realloc(pOld, newSize);
    if (pNew) {
        const size_t oldBytes = static_cast<size_t>(s_mockOldCapacity) * sizeof(void*);
        if (oldBytes < newSize) {
            std::memset(static_cast<unsigned char*>(pNew) + oldBytes, 0xA5, newSize - oldBytes);
        }
    }
    return pNew;
}

static void SimulatePreallocation(MockNode* pParent, MockNode* pChild) {
    if (!pParent || !pChild || pChild->m_pParent == pParent ||
        !G3O::SceneNodePolicy::ShouldPreallocate(pParent->m_u16Count, pParent->m_u16Capacity)) {
        return;
    }

    const uint16_t newCapacity = G3O::SceneNodePolicy::NextCapacity(pParent->m_u16Capacity);
    s_mockOldCapacity = pParent->m_u16Capacity;
    G3O::SceneNodePolicy::TryPreallocate(
        nullptr, pParent->m_ppChildren, pParent->m_u16Capacity, newCapacity, MockRealloc);
}

static bool Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        return false;
    }
    return true;
}

int main() {
    bool ok = true;
    std::cout << "[Test] Scene Node Optimizer Policy Suite starting...\n";

    MockNode parent{};
    parent.m_u16Capacity = 16;
    parent.m_u16Count = 0;
    parent.m_ppChildren = reinterpret_cast<void**>(std::malloc(32768 * sizeof(void*)));
    std::memset(parent.m_ppChildren, 0xCC, 32768 * sizeof(void*));

    MockNode child{};

    // Small growth under threshold 64: should NOT preallocate to 1024
    parent.m_u16Count = 16;
    SimulatePreallocation(&parent, &child);
    ok &= Expect(s_mockReallocCount.load() == 0, "No preallocation when count < 64");
    ok &= Expect(parent.m_u16Capacity == 16, "Capacity remains 16");

    // Threshold 64 reached: should jump to 1024
    parent.m_u16Capacity = 64;
    parent.m_u16Count = 64;
    SimulatePreallocation(&parent, &child);
    ok &= Expect(s_mockReallocCount.load() == 1, "Preallocated once at threshold 64");
    ok &= Expect(parent.m_u16Capacity == 1024, "Capacity jumped to 1024");
    ok &= Expect(parent.m_ppChildren[64] == nullptr && parent.m_ppChildren[1023] == nullptr,
                 "newly allocated child slots must be zeroed before the engine sees the larger capacity");

    // Inside 65..1023: no reallocations needed
    parent.m_u16Count = 500;
    SimulatePreallocation(&parent, &child);
    ok &= Expect(s_mockReallocCount.load() == 1, "No realloc while within 1024 capacity");

    // Capacity 1024 reached: jumps to 8192
    parent.m_u16Count = 1024;
    SimulatePreallocation(&parent, &child);
    ok &= Expect(s_mockReallocCount.load() == 2, "Preallocated second time at 1024");
    ok &= Expect(parent.m_u16Capacity == 8192, "Capacity jumped to 8192");

    // Capacity 8192 reached: jumps to 32768
    parent.m_u16Count = 8192;
    SimulatePreallocation(&parent, &child);
    ok &= Expect(s_mockReallocCount.load() == 3, "Preallocated third time at 8192");
    ok &= Expect(parent.m_u16Capacity == 32768, "Capacity jumped to 32768");

    // At 32768 (ceiling): no further growth beyond 32768
    parent.m_u16Count = 32768;
    SimulatePreallocation(&parent, &child);
    ok &= Expect(s_mockReallocCount.load() == 3, "No further preallocation beyond 32768 ceiling");
    ok &= Expect(parent.m_u16Capacity == 32768, "Capacity clamped at 32768");

    std::free(parent.m_ppChildren);

    if (ok) {
        std::cout << "[Test] Scene Node Optimizer Policy Suite PASSED!\n";
        return 0;
    }
    return 1;
}
