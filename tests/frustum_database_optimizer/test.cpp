#include "frustum_database_policy.h"
#include <cstdio>
#include <vector>
#include <tuple>

static bool Expect(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

int main() {
    bool ok = true;
    G3O::FrustumBatcher batcher;

    void* mockDB1 = reinterpret_cast<void*>(0x1000);
    void* mockDB2 = reinterpret_cast<void*>(0x2000);
    void* entityA = reinterpret_cast<void*>(0xA000);
    void* entityB = reinterpret_cast<void*>(0xB000);
    void* entityC = reinterpret_cast<void*>(0xC000);

    // 1. Basic insertion & deduplication
    ok &= Expect(batcher.AddOrUpdate(mockDB1, entityA, 1), "Add entityA to DB1");
    ok &= Expect(batcher.AddOrUpdate(mockDB1, entityA, 2), "Update entityA in DB1 (cull=2)");
    ok &= Expect(batcher.AddOrUpdate(mockDB1, entityA, 3), "Update entityA in DB1 (cull=3)");
    ok &= Expect(batcher.TotalDeferredCalls() == 3, "Total deferred calls should be 3");
    ok &= Expect(batcher.UniqueEntityCount() == 1, "Unique entity count should be 1");

    // 2. Multi-database support for same entity
    ok &= Expect(batcher.AddOrUpdate(mockDB2, entityA, 1), "Add entityA to DB2");
    ok &= Expect(batcher.UniqueEntityCount() == 1, "Unique entity count remains 1");

    // 3. Add more entities
    ok &= Expect(batcher.AddOrUpdate(mockDB1, entityB, 0), "Add entityB to DB1");
    ok &= Expect(batcher.AddOrUpdate(mockDB1, entityC, 0), "Add entityC to DB1");
    ok &= Expect(batcher.UniqueEntityCount() == 3, "Unique entity count should be 3");

    // 4. Entity deletion guard (simulating entity destruction during GotoLoadedPosition)
    ok &= Expect(batcher.RemoveEntity(entityB), "Remove entityB upon destruction");
    ok &= Expect(!batcher.RemoveEntity(entityB), "Removing already removed entityB returns false");
    ok &= Expect(batcher.DeletionsHandled() == 1, "Deletions handled should be 1");
    ok &= Expect(batcher.UniqueEntityCount() == 2, "Unique entities after deletion should be 2");

    // 5. Flush and verify surviving calls
    std::vector<std::tuple<void*, void*, int>> flushed;
    batcher.Flush([&](void* db, void* ent, int status) {
        flushed.emplace_back(db, ent, status);
    });

    ok &= Expect(flushed.size() == 3, "Flushed updates should be 3 (entityA in DB1, entityA in DB2, entityC in DB1)");
    ok &= Expect(batcher.UniqueEntityCount() == 0, "Batcher map should be empty after flush");
    ok &= Expect(batcher.TotalFlushedUpdates() == 3, "TotalFlushedUpdates should be 3");

    // Verify entityB was never flushed
    for (const auto& item : flushed) {
        ok &= Expect(std::get<1>(item) != entityB, "Deleted entityB must never be flushed");
    }

    // Verify entityA in DB1 has final cullStatus == 3
    bool foundA_DB1 = false;
    for (const auto& item : flushed) {
        if (std::get<0>(item) == mockDB1 && std::get<1>(item) == entityA) {
            ok &= Expect(std::get<2>(item) == 3, "entityA in DB1 should have latest status 3");
            foundA_DB1 = true;
        }
    }
    ok &= Expect(foundA_DB1, "entityA in DB1 must be found in flushed list");

    // 6. Test SnapshotAndClear and RecordFlushedCount
    batcher.AddOrUpdate(mockDB1, entityA, 5);
    batcher.AddOrUpdate(mockDB2, entityC, 10);
    ok &= Expect(batcher.UniqueEntityCount() == 2, "Batcher has 2 entities before snapshot");
    std::unordered_map<void*, std::vector<G3O::FrustumBatcher::DatabaseEntry>> snap;
    batcher.SnapshotAndClear(snap);
    ok &= Expect(batcher.UniqueEntityCount() == 0, "Batcher map is empty after SnapshotAndClear");
    ok &= Expect(snap.size() == 2, "Snapshot has 2 entities");
    batcher.RecordFlushedCount(2);
    ok &= Expect(batcher.TotalFlushedUpdates() == 5, "TotalFlushedUpdates should be 3 + 2 = 5");

    if (!ok) return 1;
    std::puts("PASS: FrustumBatcher deduplication, multi-DB tracking, and deletion safety verified.");
    return 0;
}
