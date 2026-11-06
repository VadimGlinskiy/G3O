#include "pak_cache_budget.h"
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

static bool Expect(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

int main() {
    G3O::PakCacheBudget budget;
    budget.Configure(64);
    bool ok = true;

    ok &= Expect(budget.TryReserve(40), "reserve within budget");
    ok &= Expect(!budget.TryReserve(25), "reject reservation that would exceed budget");
    ok &= Expect(!budget.TryReserve(65), "reject single reservation larger than budget");
    ok &= Expect(budget.Used() == 40, "failed reservations must not change usage");
    budget.Release(16);
    ok &= Expect(budget.Used() == 24 && budget.TryReserve(40),
                 "released capacity should be reusable without exceeding the limit");
    ok &= Expect(budget.Used() == 64, "concurrent reservation state should remain within the limit");
    budget.Release(64);
    ok &= Expect(budget.Used() == 0, "balanced releases should return usage to zero");

    budget.Configure(64);
    constexpr unsigned workerCount = 16;
    std::atomic<unsigned> ready{ 0 };
    std::atomic<unsigned> attempted{ 0 };
    std::atomic<unsigned> reserved{ 0 };
    std::atomic<bool> start{ false };
    std::atomic<bool> release{ false };
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < workerCount; ++i) {
        workers.emplace_back([&] {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            const bool acquired = budget.TryReserve(8);
            if (acquired) reserved.fetch_add(1, std::memory_order_relaxed);
            attempted.fetch_add(1, std::memory_order_release);
            while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
            if (acquired) budget.Release(8);
        });
    }
    while (ready.load(std::memory_order_acquire) != workerCount) std::this_thread::yield();
    start.store(true, std::memory_order_release);
    while (attempted.load(std::memory_order_acquire) != workerCount) std::this_thread::yield();
    ok &= Expect(reserved.load(std::memory_order_relaxed) == 8 && budget.Used() == 64,
                 "parallel reservations should fill but never exceed the shared budget");
    release.store(true, std::memory_order_release);
    for (std::thread& worker : workers) worker.join();
    ok &= Expect(budget.Used() == 0, "parallel releases should restore the shared budget");

    if (!ok) return 1;
    std::puts("PASS: PAK cache reservations never exceed the configured budget");
    return 0;
}
