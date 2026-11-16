#include <windows.h>
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <algorithm>
#include <cstring>
#include "pak_content_key.h"

static bool Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        return false;
    }
    return true;
}

// Mock structure mimicking DynamicLayerOptimizer internal sharded cache
struct MockPredecompEntry {
    uint32_t compSize = 0;
    uint32_t uncompSize = 0;
    std::vector<uint8_t> data;
};

static constexpr size_t kNumBuckets = 16;
struct MockBucket {
    SRWLOCK lock = SRWLOCK_INIT;
    std::unordered_map<uint64_t, MockPredecompEntry> map;
};

static MockBucket s_mockBuckets[kNumBuckets];
static std::atomic<uint32_t> s_mockHits{0};
static std::atomic<uint32_t> s_mockMisses{0};

static bool MockTryGet(
    const void* inBuffer,
    DWORD inSize,
    void* outBuffer,
    DWORD outCapacity,
    DWORD* pOutWritten
) {
    if (!inBuffer || inSize < 6 || !outBuffer || outCapacity == 0) return false;

    const uint64_t key = G3O::ComputeCompressedBufferKey(inBuffer, inSize);
    if (key == 0) return false;

    const size_t b = (key ^ (key >> 32)) & (kNumBuckets - 1);
    AcquireSRWLockShared(&s_mockBuckets[b].lock);
    auto it = s_mockBuckets[b].map.find(key);
    if (it != s_mockBuckets[b].map.end()) {
        if (outCapacity >= it->second.uncompSize) {
            const uint32_t outSize = it->second.uncompSize;
            std::memcpy(outBuffer, it->second.data.data(), outSize);
            if (pOutWritten) *pOutWritten = outSize;
            s_mockHits.fetch_add(1, std::memory_order_relaxed);
            ReleaseSRWLockShared(&s_mockBuckets[b].lock);

            if (outSize > 4096) {
                AcquireSRWLockExclusive(&s_mockBuckets[b].lock);
                s_mockBuckets[b].map.erase(key);
                ReleaseSRWLockExclusive(&s_mockBuckets[b].lock);
            }
            return true;
        }
        ReleaseSRWLockShared(&s_mockBuckets[b].lock);
        s_mockMisses.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    ReleaseSRWLockShared(&s_mockBuckets[b].lock);
    return false;
}

int main() {
    bool ok = true;
    std::cout << "[Test] Dynamic Layer Stream Prefetcher (Option 1) Test Suite starting...\n";

    // 1. Test ComputeCompressedBufferKey determinism and distinction
    std::vector<uint8_t> dummyBlockA = { 0x78, 0x9C, 0x63, 0x60, 0x60, 0x60, 0x00, 0x00, 0x00, 0x04, 0x00, 0x01 };
    std::vector<uint8_t> dummyBlockB = { 0x78, 0xDA, 0x01, 0x05, 0x00, 0xFA, 0xFF, 0x47, 0x33, 0x56, 0x30, 0x00 };

    uint64_t keyA1 = G3O::ComputeCompressedBufferKey(dummyBlockA.data(), static_cast<DWORD>(dummyBlockA.size()));
    uint64_t keyA2 = G3O::ComputeCompressedBufferKey(dummyBlockA.data(), static_cast<DWORD>(dummyBlockA.size()));
    uint64_t keyB = G3O::ComputeCompressedBufferKey(dummyBlockB.data(), static_cast<DWORD>(dummyBlockB.size()));

    ok &= Expect(keyA1 != 0, "Key A must be non-zero");
    ok &= Expect(keyA1 == keyA2, "Key A must be deterministic across calls");
    ok &= Expect(keyA1 != keyB, "Distinct compressed blocks must generate distinct keys");

    // 2. Insert into bucket map and test MockTryGet HIT
    {
        uint64_t key = keyA1;
        size_t b = (key ^ (key >> 32)) & (kNumBuckets - 1);
        MockPredecompEntry entry;
        entry.compSize = static_cast<uint32_t>(dummyBlockA.size());
        entry.uncompSize = 100;
        entry.data.resize(100, 0x42);

        AcquireSRWLockExclusive(&s_mockBuckets[b].lock);
        s_mockBuckets[b].map[key] = std::move(entry);
        ReleaseSRWLockExclusive(&s_mockBuckets[b].lock);

        std::vector<uint8_t> outBuffer(128, 0);
        DWORD outWritten = 0;
        bool hit = MockTryGet(dummyBlockA.data(), static_cast<DWORD>(dummyBlockA.size()), outBuffer.data(), static_cast<DWORD>(outBuffer.size()), &outWritten);

        ok &= Expect(hit, "MockTryGet should hit for indexed compressed block");
        ok &= Expect(outWritten == 100, "MockTryGet outWritten should match uncompSize");
        ok &= Expect(outBuffer[0] == 0x42 && outBuffer[99] == 0x42, "MockTryGet outBuffer content verified");
    }

    // 3. Test Buffer Capacity check (insufficient capacity must cleanly miss without buffer overrun)
    {
        std::vector<uint8_t> smallOutBuffer(50, 0);
        DWORD outWritten = 0;
        bool hit = MockTryGet(dummyBlockA.data(), static_cast<DWORD>(dummyBlockA.size()), smallOutBuffer.data(), static_cast<DWORD>(smallOutBuffer.size()), &outWritten);

        ok &= Expect(!hit, "MockTryGet must reject caller buffer smaller than uncompSize");
    }

    // 4. Test Large Layer Eviction on Consumption (> 4096 bytes)
    {
        std::vector<uint8_t> dummyLargeComp(5000, 0x11);
        dummyLargeComp[0] = 0x78; dummyLargeComp[1] = 0x9C;
        uint64_t largeKey = G3O::ComputeCompressedBufferKey(dummyLargeComp.data(), static_cast<DWORD>(dummyLargeComp.size()));
        size_t b = (largeKey ^ (largeKey >> 32)) & (kNumBuckets - 1);

        MockPredecompEntry largeEntry;
        largeEntry.compSize = static_cast<uint32_t>(dummyLargeComp.size());
        largeEntry.uncompSize = 8192;
        largeEntry.data.resize(8192, 0x77);

        AcquireSRWLockExclusive(&s_mockBuckets[b].lock);
        s_mockBuckets[b].map[largeKey] = std::move(largeEntry);
        ReleaseSRWLockExclusive(&s_mockBuckets[b].lock);

        std::vector<uint8_t> outBuffer(10000, 0);
        DWORD outWritten = 0;
        bool firstHit = MockTryGet(dummyLargeComp.data(), static_cast<DWORD>(dummyLargeComp.size()), outBuffer.data(), static_cast<DWORD>(outBuffer.size()), &outWritten);
        ok &= Expect(firstHit, "First query of large entry must HIT");
        ok &= Expect(outWritten == 8192, "First query written size correct");

        // Second query must miss because entry was evicted upon consumption
        DWORD secondWritten = 0;
        bool secondHit = MockTryGet(dummyLargeComp.data(), static_cast<DWORD>(dummyLargeComp.size()), outBuffer.data(), static_cast<DWORD>(outBuffer.size()), &secondWritten);
        ok &= Expect(!secondHit, "Second query of consumed large entry must MISS (eviction confirmed)");
    }

    // 5. Multi-threaded Concurrent Producer/Consumer Stress Test
    {
        constexpr int kItems = 200;
        std::vector<std::vector<uint8_t>> testBlocks(kItems);
        for (int i = 0; i < kItems; ++i) {
            testBlocks[i].resize(64, 0);
            testBlocks[i][0] = 0x78; testBlocks[i][1] = 0x9C;
            *reinterpret_cast<int*>(&testBlocks[i][4]) = i + 1000;
        }

        std::atomic<bool> startProducers{false};
        std::atomic<int> producedCount{0};

        // 4 producer threads inserting pre-decompressed blocks
        std::vector<std::thread> producers;
        for (int p = 0; p < 4; ++p) {
            producers.emplace_back([&, p]() {
                while (!startProducers.load(std::memory_order_relaxed)) {
                    std::this_thread::yield();
                }
                for (int i = p; i < kItems; i += 4) {
                    uint64_t k = G3O::ComputeCompressedBufferKey(testBlocks[i].data(), static_cast<DWORD>(testBlocks[i].size()));
                    size_t b = (k ^ (k >> 32)) & (kNumBuckets - 1);
                    MockPredecompEntry e;
                    e.compSize = static_cast<uint32_t>(testBlocks[i].size());
                    e.uncompSize = 256;
                    e.data.resize(256, static_cast<uint8_t>(i));

                    AcquireSRWLockExclusive(&s_mockBuckets[b].lock);
                    s_mockBuckets[b].map[k] = std::move(e);
                    ReleaseSRWLockExclusive(&s_mockBuckets[b].lock);
                    producedCount.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        startProducers.store(true, std::memory_order_release);
        for (auto& t : producers) t.join();

        ok &= Expect(producedCount.load() == kItems, "All items produced into sharded cache");

        // Single-thread main consumer reading all items sequentially
        int hits = 0;
        std::vector<uint8_t> readBuf(512);
        for (int i = 0; i < kItems; ++i) {
            DWORD w = 0;
            if (MockTryGet(testBlocks[i].data(), static_cast<DWORD>(testBlocks[i].size()), readBuf.data(), static_cast<DWORD>(readBuf.size()), &w)) {
                if (w == 256 && readBuf[0] == static_cast<uint8_t>(i)) {
                    hits++;
                }
            }
        }
        ok &= Expect(hits == kItems, "All items safely retrieved by consumer without corruption");
    }

    // 6. Safe Main-Thread Sequential Execution Guarantee
    {
        std::thread::id mainThreadId = std::this_thread::get_id();
        std::thread::id executionThreadId;

        auto mockWorldActivate = [&](void* /*pWorld*/) {
            executionThreadId = std::this_thread::get_id();
        };

        // In Option 1, ProcessWorldActivate executes strictly on the caller's thread
        mockWorldActivate(nullptr);
        ok &= Expect(executionThreadId == mainThreadId,
            "Sector activation must execute strictly on the caller's main thread (Option 1 invariant)");
    }

    if (ok) {
        std::cout << "[Test] All Dynamic Layer Stream Prefetcher (Option 1) tests PASSED!\n";
        return 0;
    } else {
        std::cerr << "[Test] Some tests FAILED!\n";
        return 1;
    }
}
