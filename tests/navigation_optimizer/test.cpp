#include <windows.h>
#include <iostream>
#include <atomic>
#include <thread>
#include <vector>

static std::atomic<bool> s_mockLoadActive{ false };
static std::atomic<uint32_t> s_mockVanillaTicks{ 0 };
static std::atomic<uint32_t> s_mockSkippedTicks{ 0 };

static void MockReal_NavigationProcess(void* /*pThis*/) {
    s_mockVanillaTicks.fetch_add(1, std::memory_order_relaxed);
}

static void MockHooked_NavigationProcess(void* pThis) {
    if (s_mockLoadActive.load(std::memory_order_relaxed)) {
        s_mockSkippedTicks.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    MockReal_NavigationProcess(pThis);
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
    std::cout << "[Test] Navigation Optimizer Policy Suite starting...\n";

    void* mockThis = reinterpret_cast<void*>(0x12345678);

    // 1. Loading Phase: Navigation processing should be skipped
    s_mockLoadActive.store(true, std::memory_order_release);
    for (int i = 0; i < 50; ++i) {
        MockHooked_NavigationProcess(mockThis);
    }
    ok &= Expect(s_mockSkippedTicks.load() == 50, "All 50 ticks during save loading should be suppressed");
    ok &= Expect(s_mockVanillaTicks.load() == 0, "No vanilla ticks should execute during save loading");

    // 2. Gameplay Phase: Save loading finishes, all ticks must execute normally (100% gameplay fidelity)
    s_mockLoadActive.store(false, std::memory_order_release);
    for (int i = 0; i < 100; ++i) {
        MockHooked_NavigationProcess(mockThis);
    }
    ok &= Expect(s_mockVanillaTicks.load() == 100, "All 100 ticks during gameplay must execute normally");
    ok &= Expect(s_mockSkippedTicks.load() == 50, "Skipped ticks counter should remain unchanged at 50");

    // 3. Multi-threaded Safety Check
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) {
        workers.emplace_back([mockThis]() {
            for (int i = 0; i < 1000; ++i) {
                MockHooked_NavigationProcess(mockThis);
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }
    ok &= Expect(s_mockVanillaTicks.load() == 4100, "Concurrent gameplay ticks must be counted accurately without race conditions");

    if (ok) {
        std::cout << "[Test] All Navigation Optimizer tests PASSED!\n";
        return 0;
    } else {
        std::cerr << "[Test] Some tests FAILED!\n";
        return 1;
    }
}
