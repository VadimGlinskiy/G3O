#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <iostream>

#pragma pack(push, 1)

struct bCString {
    const char* m_pcText;
};

struct gCInfo_PS {
    int dummy;
};
struct gCInfoManager_PS {
    char pad[0x4C];
    // bTInfoValMap at offset 0x4C
};

struct SInfoMapNode {
    bCString       m_key;     // +0x00
    gCInfo_PS*     m_pValue;  // +0x04
    SInfoMapNode*  m_pNext;   // +0x08
};

struct bTInfoValMap {
    SInfoMapNode** m_ppBuckets;      // +0x00
    uint32_t       m_u32BucketCount; // +0x04
    uint32_t       m_u32Capacity;    // +0x08
    uint32_t       m_u32NodeCount;   // +0x0C
};

#pragma pack(pop)

static inline std::string_view ToStringView(const bCString& str) noexcept {
    if (!str.m_pcText || !str.m_pcText[0]) {
        return {};
    }
    return std::string_view(str.m_pcText);
}

struct FastStringHash {
    using is_transparent = void;

    [[nodiscard]] size_t operator()(std::string_view sv) const noexcept {
        uint32_t hash = 2166136261u;
        for (unsigned char c : sv) {
            hash ^= c;
            hash *= 16777619u;
        }
        return hash;
    }
};

static bool Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        return false;
    }
    return true;
}

int main() {
    bool ok = true;
    std::cout << "[Test] Info Manager Optimizer Suite starting...\n";

    // 1. Layout assertions
    ok &= Expect(sizeof(bCString) == 4, "sizeof(bCString) must be 4");
    ok &= Expect(sizeof(SInfoMapNode) == 12, "sizeof(SInfoMapNode) must be 12");
    ok &= Expect(sizeof(bTInfoValMap) == 16, "sizeof(bTInfoValMap) must be 16");

    ok &= Expect(offsetof(SInfoMapNode, m_key) == 0, "SInfoMapNode.m_key must be at offset 0");
    ok &= Expect(offsetof(SInfoMapNode, m_pValue) == 4, "SInfoMapNode.m_pValue must be at offset 4");
    ok &= Expect(offsetof(SInfoMapNode, m_pNext) == 8, "SInfoMapNode.m_pNext must be at offset 8");

    ok &= Expect(offsetof(bTInfoValMap, m_ppBuckets) == 0, "bTInfoValMap.m_ppBuckets must be at offset 0");
    ok &= Expect(offsetof(bTInfoValMap, m_u32BucketCount) == 4, "bTInfoValMap.m_u32BucketCount must be at offset 4");
    ok &= Expect(offsetof(bTInfoValMap, m_u32Capacity) == 8, "bTInfoValMap.m_u32Capacity must be at offset 8");
    ok &= Expect(offsetof(bTInfoValMap, m_u32NodeCount) == 12, "bTInfoValMap.m_u32NodeCount must be at offset 12");

    // 2. Mock 43-bucket simulation
    constexpr uint32_t kBucketCount = 43;
    std::vector<SInfoMapNode*> buckets(kBucketCount, nullptr);
    std::vector<std::string> keyStorage;
    std::vector<gCInfo_PS> valStorage(500);
    std::vector<SInfoMapNode> nodeStorage(500);

    for (int i = 0; i < 500; ++i) {
        keyStorage.push_back("INFO_DIALOG_QUEST_" + std::to_string(i));
        valStorage[i].dummy = 1000 + i;

        nodeStorage[i].m_key.m_pcText = keyStorage[i].c_str();
        nodeStorage[i].m_pValue = &valStorage[i];

        // Distribute into 43 buckets
        uint32_t b = static_cast<uint32_t>(i % kBucketCount);
        nodeStorage[i].m_pNext = buckets[b];
        buckets[b] = &nodeStorage[i];
    }

    bTInfoValMap mockMap{};
    mockMap.m_ppBuckets = buckets.data();
    mockMap.m_u32BucketCount = kBucketCount;
    mockMap.m_u32Capacity = kBucketCount;
    mockMap.m_u32NodeCount = 500;

    // 3. Build Shadow Index
    std::unordered_map<std::string_view, gCInfo_PS*, FastStringHash, std::equal_to<>> shadowMap;
    shadowMap.reserve(1024);

    for (uint32_t i = 0; i < mockMap.m_u32BucketCount; ++i) {
        for (SInfoMapNode* pNode = mockMap.m_ppBuckets[i]; pNode != nullptr; pNode = pNode->m_pNext) {
            std::string_view sv = ToStringView(pNode->m_key);
            if (!sv.empty() && pNode->m_pValue != nullptr) {
                shadowMap.emplace(sv, pNode->m_pValue);
            }
        }
    }

    ok &= Expect(shadowMap.size() == 500, "Shadow map must contain exactly 500 entries");

    // 4. Verify all 500 lookups
    for (int i = 0; i < 500; ++i) {
        bCString searchKey{ keyStorage[i].c_str() };
        std::string_view sv = ToStringView(searchKey);
        auto it = shadowMap.find(sv);
        ok &= Expect(it != shadowMap.end(), "Entry must be found in shadow map");
        if (it != shadowMap.end()) {
            ok &= Expect(it->second->dummy == 1000 + i, "Value must match expected dummy");
        }
    }

    // 5. Verify negative lookups
    {
        bCString badKey{ "NON_EXISTING_DIALOG_9999" };
        auto it = shadowMap.find(ToStringView(badKey));
        ok &= Expect(it == shadowMap.end(), "Non-existing key must not be found");

        bCString emptyKey{ "" };
        auto itEmpty = shadowMap.find(ToStringView(emptyKey));
        ok &= Expect(itEmpty == shadowMap.end(), "Empty key must not be found");

        bCString nullKey{ nullptr };
        auto itNull = shadowMap.find(ToStringView(nullKey));
        ok &= Expect(itNull == shadowMap.end(), "Null key must not be found");
    }

    // 6. Test 32,768-Bucket Expansion Simulation (24,555 items)
    {
        std::cout << "[Test] Simulating 32,768 bucket expansion with 24,555 dialogs...\n";
        constexpr uint32_t kExpandedBuckets = 32768;
        constexpr uint32_t kDialogCount = 24555;

        std::vector<SInfoMapNode*> expandedBuckets(kExpandedBuckets, nullptr);
        std::vector<std::string> dialogKeys(kDialogCount);
        std::vector<gCInfo_PS> dialogVals(kDialogCount);
        std::vector<SInfoMapNode> dialogNodes(kDialogCount);

        FastStringHash hasher;
        for (uint32_t i = 0; i < kDialogCount; ++i) {
            dialogKeys[i] = "INFO_G3_DIALOG_ENTRY_" + std::to_string(i);
            dialogVals[i].dummy = static_cast<int>(20000 + i);

            dialogNodes[i].m_key.m_pcText = dialogKeys[i].c_str();
            dialogNodes[i].m_pValue = &dialogVals[i];

            uint32_t hashVal = static_cast<uint32_t>(hasher(dialogKeys[i]));
            uint32_t bucketIdx = hashVal % kExpandedBuckets;

            // Insert into bucket head
            dialogNodes[i].m_pNext = expandedBuckets[bucketIdx];
            expandedBuckets[bucketIdx] = &dialogNodes[i];
        }

        bTInfoValMap expandedMap{};
        expandedMap.m_ppBuckets = expandedBuckets.data();
        expandedMap.m_u32BucketCount = kExpandedBuckets;
        expandedMap.m_u32Capacity = kExpandedBuckets;
        expandedMap.m_u32NodeCount = kDialogCount;

        // Check bucket chain lengths
        uint32_t maxChain = 0;
        uint32_t occupiedBuckets = 0;
        for (uint32_t b = 0; b < kExpandedBuckets; ++b) {
            uint32_t chainLen = 0;
            for (SInfoMapNode* n = expandedBuckets[b]; n != nullptr; n = n->m_pNext) {
                chainLen++;
            }
            if (chainLen > 0) occupiedBuckets++;
            if (chainLen > maxChain) maxChain = chainLen;
        }

        std::cout << "  - Occupied buckets: " << occupiedBuckets << " / " << kExpandedBuckets
                  << " (" << (occupiedBuckets * 100.0 / kExpandedBuckets) << "%)\n";
        std::cout << "  - Max bucket chain length: " << maxChain << " (vs ~570 with 43 buckets)\n";

        ok &= Expect(maxChain <= 6, "Max chain in 32,768 buckets should be <= 6 (O(1) access)");
        ok &= Expect(occupiedBuckets > 15000, "At least 15,000 buckets should be utilized");

        // Verify all 24,555 lookups in expanded map
        uint32_t foundCount = 0;
        for (uint32_t i = 0; i < kDialogCount; ++i) {
            uint32_t hashVal = static_cast<uint32_t>(hasher(dialogKeys[i]));
            uint32_t bucketIdx = hashVal % kExpandedBuckets;

            bool found = false;
            for (SInfoMapNode* n = expandedBuckets[bucketIdx]; n != nullptr; n = n->m_pNext) {
                if (std::string_view(n->m_key.m_pcText) == dialogKeys[i]) {
                    if (n->m_pValue->dummy == static_cast<int>(20000 + i)) {
                        found = true;
                        break;
                    }
                }
            }
            if (found) foundCount++;
        }
        ok &= Expect(foundCount == kDialogCount, "All 24,555 dialogs must be found in expanded map");
    }

    // 7. Test In-Place Rehash with Prime 32,771 Buckets (Genome djb2 Hash & QuickLoad Safety)
    {
        std::cout << "[Test] Simulating in-place Rehash from 43 to 32,771 buckets (QuickLoad preservation)...\n";
        constexpr uint32_t kInitialBuckets = 43;
        constexpr uint32_t kPrimeBuckets = 32771;
        constexpr uint32_t kExistingNodes = 500;

        auto genomeHash = [](const bCString& str) -> uint32_t {
            const char* s = str.m_pcText;
            if (!s) return 0;
            uint32_t hash = 0;
            while (*s) {
                hash = (hash * 33) + static_cast<int32_t>(static_cast<signed char>(*s));
                ++s;
            }
            return hash;
        };

        // Allocate initial 43-bucket map
        auto** pOldBuckets = new SInfoMapNode*[kInitialBuckets]();
        std::vector<std::string> testKeys(kExistingNodes);
        std::vector<gCInfo_PS> testVals(kExistingNodes);
        std::vector<SInfoMapNode> testNodes(kExistingNodes);

        for (uint32_t i = 0; i < kExistingNodes; ++i) {
            testKeys[i] = "G3_INFO_REHASH_ENTRY_" + std::to_string(i);
            testVals[i].dummy = 30000 + i;
            testNodes[i].m_key.m_pcText = testKeys[i].c_str();
            testNodes[i].m_pValue = &testVals[i];

            uint32_t h = genomeHash(testNodes[i].m_key);
            uint32_t idx = h % kInitialBuckets;
            testNodes[i].m_pNext = pOldBuckets[idx];
            pOldBuckets[idx] = &testNodes[i];
        }

        bTInfoValMap rehashMap{};
        rehashMap.m_ppBuckets = pOldBuckets;
        rehashMap.m_u32BucketCount = kInitialBuckets;
        rehashMap.m_u32Capacity = kInitialBuckets;
        rehashMap.m_u32NodeCount = kExistingNodes;

        // Perform simulated ExpandAndRehashInfoMap
        auto** pNewBuckets = new SInfoMapNode*[kPrimeBuckets]();
        if (rehashMap.m_ppBuckets && rehashMap.m_u32NodeCount > 0) {
            for (uint32_t i = 0; i < rehashMap.m_u32BucketCount; ++i) {
                SInfoMapNode* pNode = rehashMap.m_ppBuckets[i];
                while (pNode) {
                    SInfoMapNode* pNext = pNode->m_pNext;
                    uint32_t h = genomeHash(pNode->m_key);
                    uint32_t newIdx = h % kPrimeBuckets;
                    pNode->m_pNext = pNewBuckets[newIdx];
                    pNewBuckets[newIdx] = pNode;
                    pNode = pNext;
                }
            }
        }
        delete[] rehashMap.m_ppBuckets;
        rehashMap.m_ppBuckets = pNewBuckets;
        rehashMap.m_u32BucketCount = kPrimeBuckets;
        rehashMap.m_u32Capacity = kPrimeBuckets;

        ok &= Expect(rehashMap.m_u32BucketCount == kPrimeBuckets, "Bucket count must be 32,771");
        ok &= Expect(rehashMap.m_u32NodeCount == kExistingNodes, "Node count must remain 500");

        // Verify every single node is intact and accessible
        uint32_t verifiedCount = 0;
        uint32_t rehashMaxChain = 0;
        for (uint32_t i = 0; i < kExistingNodes; ++i) {
            bCString searchKey{ testKeys[i].c_str() };
            uint32_t h = genomeHash(searchKey);
            uint32_t idx = h % kPrimeBuckets;

            bool found = false;
            uint32_t chain = 0;
            for (SInfoMapNode* n = rehashMap.m_ppBuckets[idx]; n != nullptr; n = n->m_pNext) {
                chain++;
                if (std::string_view(n->m_key.m_pcText) == testKeys[i]) {
                    if (n->m_pValue->dummy == static_cast<int>(30000 + i)) {
                        found = true;
                    }
                }
            }
            if (chain > rehashMaxChain) rehashMaxChain = chain;
            if (found) verifiedCount++;
        }

        std::cout << "  - Verified preserved nodes: " << verifiedCount << " / " << kExistingNodes << "\n";
        std::cout << "  - Max chain length in 32,771 buckets: " << rehashMaxChain << "\n";
        ok &= Expect(verifiedCount == kExistingNodes, "All 500 nodes must be preserved across rehash");
        ok &= Expect(rehashMaxChain <= 2, "Max chain in 32,771 buckets for 500 items must be <= 2");

        delete[] rehashMap.m_ppBuckets;
    }

    std::cout << "[Test] Info Manager Optimizer Suite: " << (ok ? "PASSED" : "FAILED") << "\n";
    return ok ? 0 : 1;
}
