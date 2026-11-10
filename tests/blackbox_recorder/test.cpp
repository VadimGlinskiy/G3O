#include "blackbox_recorder.h"
#include <windows.h>
#include <iostream>
#include <thread>
#include <vector>
#include <cassert>
#include <cstring>

static bool Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        return false;
    }
    return true;
}

int main() {
    bool ok = true;
    std::cout << "[Test] Blackbox Recorder Test Suite starting...\n";

    // 1. Initial State
    uint32_t myTid = GetCurrentThreadId();
    const G3O::ThreadBlackboxSlot* initialSlot = G3O::Blackbox_GetSlotForThread(myTid);
    // At start, either no slot or idle
    if (initialSlot) {
        ok &= Expect((initialSlot->seq_num.load() & 1) == 0, "Initial slot sequence number should be even (idle)");
    }

    // 2. Scope Tracking (In-Flight)
    {
        G3O::BlackboxScope scope(G3O::BB_OP_READING_PAK, "Data/Templates.pak", 0x12345000ULL, 65536, "NPC_Hero.tple");

        const G3O::ThreadBlackboxSlot* activeSlot = G3O::Blackbox_GetSlotForThread(myTid);
        ok &= Expect(activeSlot != nullptr, "Active slot must be found for current thread");
        if (activeSlot) {
            ok &= Expect((activeSlot->seq_num.load() & 1) == 1, "Active slot sequence number must be odd (in-flight)");
            ok &= Expect(activeSlot->op_type == G3O::BB_OP_READING_PAK, "Op type should be BB_OP_READING_PAK");
            ok &= Expect(std::strcmp(activeSlot->archive_name, "Data/Templates.pak") == 0, "Archive name mismatch");
            ok &= Expect(activeSlot->offset == 0x12345000ULL, "Offset mismatch");
            ok &= Expect(activeSlot->size == 65536, "Size mismatch");
            ok &= Expect(std::strcmp(activeSlot->resource_name, "NPC_Hero.tple") == 0, "Resource name mismatch");
        }
    }

    // 3. Post-Scope (Completed / Idle)
    const G3O::ThreadBlackboxSlot* postSlot = G3O::Blackbox_GetSlotForThread(myTid);
    ok &= Expect(postSlot != nullptr, "Post slot should still exist");
    if (postSlot) {
        ok &= Expect((postSlot->seq_num.load() & 1) == 0, "Post slot sequence number must be even (idle)");
        // Archive and offset are retained as correlation evidence
        ok &= Expect(std::strcmp(postSlot->archive_name, "Data/Templates.pak") == 0, "Archive retained for post-mortem correlation");
    }

    // 4. Multi-threaded Concurrency & Snapshotting
    std::vector<std::thread> workers;
    constexpr int NUM_WORKERS = 4;
    std::atomic<bool> startFlag{false};
    std::atomic<bool> finishFlag{false};

    for (int i = 0; i < NUM_WORKERS; ++i) {
        workers.emplace_back([i, &startFlag, &finishFlag]() {
            while (!startFlag.load()) { std::this_thread::yield(); }
            char name[32];
            std::snprintf(name, sizeof(name), "ThreadWorker_%d.dat", i);
            G3O::BlackboxScope workerScope(G3O::BB_OP_DECOMPRESSING_PAK, "Data/Worlds.pak", 1000 * (i + 1), 4096, name);
            while (!finishFlag.load()) { std::this_thread::yield(); }
        });
    }

    startFlag.store(true);
    // Give threads time to enter scope
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    G3O::ThreadBlackboxSlot snapshot[G3O::BLACKBOX_MAX_SLOTS];
    size_t count = G3O::Blackbox_SnapshotAll(snapshot, G3O::BLACKBOX_MAX_SLOTS);
    ok &= Expect(count >= NUM_WORKERS + 1, "Snapshot should capture worker threads + main thread");

    finishFlag.store(true);
    for (auto& w : workers) {
        w.join();
    }

    if (ok) {
        std::cout << "[Test] Blackbox Recorder Test Suite PASSED!\n";
        return 0;
    } else {
        std::cerr << "[Test] Blackbox Recorder Test Suite FAILED!\n";
        return 1;
    }
}
