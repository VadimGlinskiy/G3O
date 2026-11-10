#include <windows.h>
#include <iostream>
#include <cstdint>
#include <vector>
#include <atomic>

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

static void* MockRealloc(void* /*pAdmin*/, void* pOld, uint32_t newSize) {
    s_mockReallocCount.fetch_add(1, std::memory_order_relaxed);
    return std::realloc(pOld, newSize);
}

static void SimulatePreallocation(MockNode* pParent, MockNode* pChild) {
    if (!pParent || !pChild) return;
    if (pChild->m_pParent != pParent) {
        if (pParent->m_u16Count >= 64 && pParent->m_u16Count == pParent->m_u16Capacity && pParent->m_u16Capacity < 32768) {
            uint16_t newCap = 1024;
            if (pParent->m_u16Capacity >= 8192) {
                newCap = 32768;
            } else if (pParent->m_u16Capacity >= 1024) {
                newCap = 8192;
            }

            void* pNewBuf = MockRealloc(nullptr, pParent->m_ppChildren, newCap * sizeof(void*));
            if (pNewBuf) {
                pParent->m_ppChildren = reinterpret_cast<void**>(pNewBuf);
                pParent->m_u16Capacity = newCap;
            }
        }
    }
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
    parent.m_ppChildren = reinterpret_cast<void**>(std::malloc(parent.m_u16Capacity * sizeof(void*)));

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
