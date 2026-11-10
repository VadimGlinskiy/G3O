#include <windows.h>
#include <cstdint>
#include <vector>
#include <atomic>
#include <iostream>
#include <cstring>
#include <algorithm>

struct MockRingSlot {
    std::atomic<bool> isReady{false};
    uint64_t fileOffset = 0;
    uint32_t compSize = 0;
    uint32_t uncompSize = 0;
    uint32_t first4 = 0;
    uint8_t buffer[64] = {0};
};

static constexpr size_t kTestSlots = 4;
static MockRingSlot s_testSlots[kTestSlots];

static HANDLE s_hTestSlotConsumed = NULL;
static HANDLE s_hTestStop = NULL;
static std::atomic<bool> s_testActive{true};

struct TestItem {
    uint32_t compSize;
    uint32_t uncompSize;
    uint32_t first4;
};

static std::vector<TestItem> s_testEntries;

static DWORD WINAPI MockProducerThread(LPVOID lpParam) {
    const size_t totalItems = reinterpret_cast<size_t>(lpParam);

    for (size_t i = 0; i < totalItems; ++i) {
        const size_t targetSlot = i % kTestSlots;

        // Wait if slot is still ready (full)
        while (s_testSlots[targetSlot].isReady.load(std::memory_order_relaxed)) {
            HANDLE waits[2] = { s_hTestStop, s_hTestSlotConsumed };
            DWORD res = WaitForMultipleObjects(2, waits, FALSE, 20);
            if (res == WAIT_OBJECT_0 || !s_testActive.load(std::memory_order_relaxed)) return 0;
        }

        s_testSlots[targetSlot].compSize = s_testEntries[i].compSize;
        s_testSlots[targetSlot].uncompSize = s_testEntries[i].uncompSize;
        s_testSlots[targetSlot].first4 = s_testEntries[i].first4;
        std::memset(s_testSlots[targetSlot].buffer, static_cast<int>(i & 0xFF), sizeof(s_testSlots[targetSlot].buffer));

        s_testSlots[targetSlot].isReady.store(true, std::memory_order_release);
    }
    return 0;
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
    std::cout << "[Test] Layer Ring Arena Simulation starting...\n";

    constexpr size_t kItems = 106; // Exact number of RB_Mod layers
    s_testEntries.resize(kItems);
    for (size_t i = 0; i < kItems; ++i) {
        s_testEntries[i].compSize = static_cast<uint32_t>(1000 + i);
        s_testEntries[i].uncompSize = static_cast<uint32_t>(3000 + i);
        s_testEntries[i].first4 = static_cast<uint32_t>(0xAABB0000 | i);
    }

    s_hTestSlotConsumed = CreateEventA(NULL, FALSE, FALSE, NULL);
    s_hTestStop = CreateEventA(NULL, FALSE, FALSE, NULL);

    HANDLE hProducer = CreateThread(NULL, 0, MockProducerThread, reinterpret_cast<LPVOID>(kItems), 0, NULL);
    ok &= Expect(hProducer != NULL, "Producer thread creation");

    size_t consumerIdx = 0;
    size_t consumed = 0;

    while (consumerIdx < kItems) {
        // Simulate a skip at index 20 (layer 20 skipped, requesting 21 directly)
        size_t requestedItem = consumerIdx;
        if (consumerIdx == 20) {
            requestedItem = 21; // Skip 20
        }

        uint32_t inCompSize = s_testEntries[requestedItem].compSize;
        uint32_t inFirst4 = s_testEntries[requestedItem].first4;

        // Match logic from LayerRingArena_TryGet
        size_t matchedIdx = static_cast<size_t>(-1);
        if (consumerIdx < s_testEntries.size()) {
            if (s_testEntries[consumerIdx].compSize == inCompSize && s_testEntries[consumerIdx].first4 == inFirst4) {
                matchedIdx = consumerIdx;
            } else {
                const size_t maxAhead = (consumerIdx + kTestSlots < s_testEntries.size()) ? (consumerIdx + kTestSlots) : s_testEntries.size();
                for (size_t a = consumerIdx + 1; a < maxAhead; ++a) {
                    if (s_testEntries[a].compSize == inCompSize && s_testEntries[a].first4 == inFirst4) {
                        matchedIdx = a;
                        break;
                    }
                }
            }
        }

        ok &= Expect(matchedIdx == requestedItem, "matchedIdx should identify requested item");

        // Free skipped slots
        if (matchedIdx > consumerIdx) {
            for (size_t skip = consumerIdx; skip < matchedIdx; ++skip) {
                s_testSlots[skip % kTestSlots].isReady.store(false, std::memory_order_release);
            }
            SetEvent(s_hTestSlotConsumed);
        }

        const size_t targetSlot = matchedIdx % kTestSlots;
        MockRingSlot& slot = s_testSlots[targetSlot];

        int spin = 0;
        while (!slot.isReady.load(std::memory_order_acquire) && spin < 2000) {
            YieldProcessor();
            spin++;
        }
        for (int w = 0; w < 50 && !slot.isReady.load(std::memory_order_acquire); ++w) {
            Sleep(1);
        }

        ok &= Expect(slot.isReady.load(std::memory_order_acquire), "Slot must be ready for consumer");
        ok &= Expect(slot.compSize == inCompSize, "compSize match");
        ok &= Expect(slot.first4 == inFirst4, "first4 signature match");
        ok &= Expect(slot.buffer[0] == static_cast<uint8_t>(requestedItem & 0xFF), "payload match");

        slot.isReady.store(false, std::memory_order_release);
        SetEvent(s_hTestSlotConsumed);

        consumerIdx = matchedIdx + 1;
        consumed++;
    }

    WaitForSingleObject(hProducer, 2000);
    CloseHandle(hProducer);
    CloseHandle(s_hTestSlotConsumed);
    CloseHandle(s_hTestStop);

    ok &= Expect(consumed == 105, "105 items consumed (1 intentionally skipped) without stall");

    if (ok) {
        std::cout << "[Test] Layer Ring Arena Simulation: PASS (Sequential + Skip resilience verified)!\n";
        return 0;
    }
    return 1;
}
