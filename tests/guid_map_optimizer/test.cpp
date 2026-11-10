#include "../../src/guid_map_policy.h"
#include <iostream>
#include <cassert>
#include <vector>

using namespace G3O;

static bool Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        return false;
    }
    return true;
}

int main() {
    bool ok = true;
    std::cout << "[Test] Universal GUID Map Optimizer Policy Suite starting...\n";

    // 1. Structural layout assertions
    ok &= Expect(sizeof(bCPropertyID) == 20, "bCPropertyID must be exactly 20 bytes");
    ok &= Expect(sizeof(SPropertyIDMapNode) == 28, "SPropertyIDMapNode must be exactly 28 bytes (0x1C)");

    // 2. Hash calculation parity test
    bCPropertyID id1 = { 0x12345678, 0x11111111, 0x22222222, 0x33333333, 0x00000050 };
    uint32_t expectedHash = (0x12345678u >> 3) + 0x00000050u;
    uint32_t calculatedHash = ComputeNativePropertyIDHash(id1);
    ok &= Expect(calculatedHash == expectedHash, "Native hash calculation must match (d[0] >> 3) + d[4]");

    // 3. Equality testing: checks 16-byte UUID core (d[0]..d[3])
    bCPropertyID id2 = id1;
    ok &= Expect(id1.Equals(id2), "Identical bCPropertyID must be equal");
    ok &= Expect(id1 == id2, "operator== must return true for identical IDs");

    // Any difference in d[0]..d[3] must make them not equal
    for (int i = 0; i < 4; ++i) {
        bCPropertyID idDiff = id1;
        idDiff.d[i] ^= 0x01;
        ok &= Expect(!id1.Equals(idDiff), "Differing GUID core must NOT be equal");
        ok &= Expect(id1 != idDiff, "operator!= must return true for differing GUID core");
    }

    // Difference in d[4] (suffix/count) should be ignored by operator== (matching SharedBase.dll)
    bCPropertyID idSuffixDiff = id1;
    idSuffixDiff.d[4] ^= 0x55;
    ok &= Expect(id1.Equals(idSuffixDiff), "Difference in suffix d[4] must not break GUID equality");

    // 4. Mock Map operations
    constexpr uint32_t kBucketCount = 16;
    SPropertyIDMapNode* buckets[kBucketCount] = {};

    bTValMap_PropertyID mockMap{};
    mockMap.m_ppBuckets = buckets;
    mockMap.m_u32BucketCount = kBucketCount;
    mockMap.m_u32Count = 0;

    // Create 3 mock nodes
    bCPropertyID keyA = { 0x1000, 1, 2, 3, 0x10 };
    bCPropertyID keyB = { 0x2000, 1, 2, 3, 0x20 };
    bCPropertyID keyC = { 0x3000, 1, 2, 3, 0x30 };

    int dummyEntityA = 101;
    int dummyEntityB = 202;
    int dummyEntityC = 303;

    SPropertyIDMapNode nodeA = { keyA, &dummyEntityA, nullptr };
    SPropertyIDMapNode nodeB = { keyB, &dummyEntityB, nullptr };
    SPropertyIDMapNode nodeC = { keyC, &dummyEntityC, nullptr };

    // Insert nodeA and nodeB into mock map buckets
    uint32_t bucketA = ComputeNativePropertyIDHash(keyA) % kBucketCount;
    nodeA.m_pNext = buckets[bucketA];
    buckets[bucketA] = &nodeA;
    mockMap.m_u32Count++;

    uint32_t bucketB = ComputeNativePropertyIDHash(keyB) % kBucketCount;
    nodeB.m_pNext = buckets[bucketB];
    buckets[bucketB] = &nodeB;
    mockMap.m_u32Count++;

    SGuidMapTelemetry telemetry{};

    // First lookup of keyA: should be found in bucketA
    uint32_t outBucket = 0xFFFFFFFF;
    SPropertyIDMapNode* pFound = LookupPropertyIDNode(&mockMap, &keyA, &outBucket, &telemetry);
    ok &= Expect(pFound == &nodeA, "KeyA must be found");
    ok &= Expect(pFound->m_pValue == &dummyEntityA, "KeyA payload must match dummyEntityA");
    ok &= Expect(outBucket == bucketA, "OutBucket must match native bucket index");
    ok &= Expect(telemetry.u32FastWalkHits == 1, "First lookup must be fast walk hit");

    // Second lookup of keyA: directly from live map
    outBucket = 0xFFFFFFFF;
    pFound = LookupPropertyIDNode(&mockMap, &keyA, &outBucket, &telemetry);
    ok &= Expect(pFound == &nodeA, "KeyA second lookup must return nodeA");
    ok &= Expect(outBucket == bucketA, "KeyA second lookup outBucket must match");
    ok &= Expect(telemetry.u32FastWalkHits == 2, "Second lookup must increment fast walks");

    // Lookup of keyB
    pFound = LookupPropertyIDNode(&mockMap, &keyB, &outBucket, &telemetry);
    ok &= Expect(pFound == &nodeB, "KeyB must be found");
    ok &= Expect(telemetry.u32FastWalkHits == 3, "KeyB lookup incremented fast walks");

    // Lookup of keyC (non-existent in map): must return nullptr, increment misses, set outBucket correctly
    uint32_t expectedBucketC = ComputeNativePropertyIDHash(keyC) % kBucketCount;
    outBucket = 0xFFFFFFFF;
    pFound = LookupPropertyIDNode(&mockMap, &keyC, &outBucket, &telemetry);
    ok &= Expect(pFound == nullptr, "Non-existent KeyC must return nullptr");
    ok &= Expect(outBucket == expectedBucketC, "Non-existent KeyC must still set native outBucket for Insert");
    ok &= Expect(telemetry.u32Misses == 1, "Non-existent KeyC must increment misses");

    // 5. Test safety: removal of node from bucket (must return nullptr immediately, no stale caching)
    buckets[bucketA] = nodeA.m_pNext; // remove nodeA
    mockMap.m_u32Count--;
    pFound = LookupPropertyIDNode(&mockMap, &keyA, &outBucket, &telemetry);
    ok &= Expect(pFound == nullptr, "Deleted KeyA must immediately return nullptr without dangling pointer");
    ok &= Expect(telemetry.u32Misses == 2, "Deleted KeyA must increment misses");

    std::cout << "[Test] GuidMapOptimizer Policy Suite finished. Status: " << (ok ? "PASSED" : "FAILED") << "\n";
    return ok ? 0 : 1;
}
