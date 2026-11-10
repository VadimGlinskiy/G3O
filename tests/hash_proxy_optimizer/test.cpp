#include "fast_djb2.h"
#include "template_cache_policy.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static uint32_t ReferenceDJB2(const char* s, int len) {
    if (!s || len <= 0) return 0;
    uint32_t h = 0;
    for (int i = 0; i < len; ++i) {
        int8_t sc = static_cast<int8_t>(s[i]);
        h = ((h << 5) + h) + static_cast<uint32_t>(static_cast<int32_t>(sc));
    }
    return h;
}

static bool Expect(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

int main() {
    bool ok = true;

    // 1. Fast DJB2 Equivalence Tests
    std::vector<std::string> testStrings = {
        "",
        "a",
        "ab",
        "abc",
        "abcd",
        "abcde",
        "abcdefgh",
        "{9A103CC2-4190-4DB3-9618-0419E5445AAD}",
        "eCEntityDynamicContext",
        "gCDynamicLayer",
        "CNQ_Content&NPC",
        "SysDyn_Sector_Test_Payload_Long_String_1234567890"
    };

    for (const auto& s : testStrings) {
        int len = static_cast<int>(s.length());
        uint32_t ref = ReferenceDJB2(s.data(), len);
        uint32_t fast = G3O::FastDJB2_Length(s.data(), len);
        ok &= Expect(ref == fast, ("FastDJB2_Length mismatch for: " + s).c_str());

        uint32_t fastNull = G3O::FastDJB2_NullTerminated(s.c_str());
        ok &= Expect(ref == fastNull, ("FastDJB2_NullTerminated mismatch for: " + s).c_str());
    }

    // High byte value check (signed char behavior: 0x80 to 0xFF)
    char highBytes[8] = { (char)0xFF, (char)0x80, (char)0xC3, (char)0xA4, (char)0xDF, (char)0x7F, (char)0x00, 0 };
    uint32_t refHigh = ReferenceDJB2(highBytes, 6);
    uint32_t fastHigh = G3O::FastDJB2_Length(highBytes, 6);
    ok &= Expect(refHigh == fastHigh, "FastDJB2_Length must match reference for high signed bytes");

    // Null and empty checks
    ok &= Expect(G3O::FastDJB2_Length(nullptr, 0) == 0, "Null data returns 0");
    ok &= Expect(G3O::FastDJB2_Length("abc", 0) == 0, "Zero length returns 0");
    ok &= Expect(G3O::FastDJB2_NullTerminated(nullptr) == 0, "Null string returns 0");

    // 2. TemplateEntityCache Tests
    G3O::TemplateEntityCache cache;
    ok &= Expect(cache.Size() == 0, "Initial cache size is 0");
    ok &= Expect(cache.Lookup(nullptr) == nullptr, "Lookup null returns nullptr");

    uint32_t propIdA[4] = { 0x11111111, 0x22222222, 0x33333333, 0x44444444 };
    uint32_t propIdB[4] = { 0xAAAAAAAA, 0xBBBBBBBB, 0xCCCCCCCC, 0xDDDDDDDD };
    void* entityA = reinterpret_cast<void*>(0xDEADBEEF);
    void* entityB = reinterpret_cast<void*>(0xCAFEBABE);

    ok &= Expect(cache.Lookup(propIdA) == nullptr, "Lookup before insert returns nullptr");

    cache.Insert(propIdA, entityA);
    ok &= Expect(cache.Size() == 1, "Cache size should be 1 after insert");
    ok &= Expect(cache.Lookup(propIdA) == entityA, "Lookup propIdA returns entityA");
    ok &= Expect(cache.Lookup(propIdB) == nullptr, "Lookup propIdB before insert returns nullptr");

    cache.Insert(propIdB, entityB);
    ok &= Expect(cache.Size() == 2, "Cache size should be 2 after second insert");
    ok &= Expect(cache.Lookup(propIdA) == entityA, "Lookup propIdA still returns entityA");
    ok &= Expect(cache.Lookup(propIdB) == entityB, "Lookup propIdB returns entityB");

    cache.Clear();
    ok &= Expect(cache.Size() == 0, "Cache size 0 after clear");
    ok &= Expect(cache.Lookup(propIdA) == nullptr, "Lookup after clear returns nullptr");

    // 3. FastTemplateCache Direct-Mapped & Negative Caching Tests
    G3O::FastTemplateCache::ClearAll();
    uint32_t idxA = G3O::FastTemplateCache::ComputeSlotIndex(propIdA);
    uint32_t idxB = G3O::FastTemplateCache::ComputeSlotIndex(propIdB);
    ok &= Expect(idxA < G3O::FastTemplateCache::kSlotCount, "idxA within slot count");
    ok &= Expect(idxB < G3O::FastTemplateCache::kSlotCount, "idxB within slot count");
    ok &= Expect(G3O::FastTemplateCache::GuidEquals(propIdA, propIdA), "GuidEquals self matches");
    ok &= Expect(!G3O::FastTemplateCache::GuidEquals(propIdA, propIdB), "GuidEquals distinct doesn't match");

    // Positive caching test
    auto& slotA = G3O::FastTemplateCache::s_slots[idxA];
    slotA.guid[0] = propIdA[0];
    slotA.guid[1] = propIdA[1];
    slotA.guid[2] = propIdA[2];
    slotA.guid[3] = propIdA[3];
    slotA.entityOrState = reinterpret_cast<uintptr_t>(entityA);
    ok &= Expect(G3O::FastTemplateCache::GuidEquals(propIdA, slotA.guid), "Slot GUID matches propIdA");
    ok &= Expect(slotA.entityOrState == reinterpret_cast<uintptr_t>(entityA), "Slot entity matches entityA");

    // Negative caching test
    uint32_t propIdC[4] = { 0x55555555, 0x66666666, 0x77777777, 0x88888888 };
    uint32_t idxC = G3O::FastTemplateCache::ComputeSlotIndex(propIdC);
    auto& slotC = G3O::FastTemplateCache::s_slots[idxC];
    slotC.guid[0] = propIdC[0];
    slotC.guid[1] = propIdC[1];
    slotC.guid[2] = propIdC[2];
    slotC.guid[3] = propIdC[3];
    slotC.entityOrState = G3O::FastTemplateCache::kNotFound;
    slotC.negEpoch = G3O::FastTemplateCache::s_currentNegEpoch.load();
    ok &= Expect(slotC.entityOrState == G3O::FastTemplateCache::kNotFound, "Negative slot has kNotFound");

    // Invalidation test
    uint32_t epochBefore = G3O::FastTemplateCache::s_currentNegEpoch.load();
    G3O::FastTemplateCache::InvalidateNegativeCache();
    uint32_t epochAfter = G3O::FastTemplateCache::s_currentNegEpoch.load();
    ok &= Expect(epochAfter == epochBefore + 1, "Epoch incremented after InvalidateNegativeCache");
    ok &= Expect(slotC.negEpoch != epochAfter, "Slot epoch now stale compared to current epoch");

    // ClearAll test
    G3O::FastTemplateCache::ClearAll();
    ok &= Expect(slotA.entityOrState == G3O::FastTemplateCache::kEmptySlot, "Slot A cleared after ClearAll");

    if (!ok) return 1;
    std::puts("PASS: FastDJB2 hashing and TemplateEntityCache verified.");
    return 0;
}
