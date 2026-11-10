#pragma once

#include <vector>
#include <unordered_map>
#include <cstdint>
#include <utility>

namespace G3O {

class FrustumBatcher {
public:
    struct DatabaseEntry {
        void* pFrustumDB;
        int   cullStatus;
    };

    bool AddOrUpdate(void* pFrustumDB, void* pEntity, int cullStatus) {
        if (!pFrustumDB || !pEntity) return false;
        ++m_totalDeferredCalls;
        auto& dbs = m_map[pEntity];
        for (auto& entry : dbs) {
            if (entry.pFrustumDB == pFrustumDB) {
                entry.cullStatus = cullStatus;
                return true;
            }
        }
        dbs.push_back({ pFrustumDB, cullStatus });
        return true;
    }

    bool RemoveEntity(void* pEntity) {
        if (!pEntity) return false;
        auto it = m_map.find(pEntity);
        if (it != m_map.end()) {
            m_map.erase(it);
            ++m_deletionsHandled;
            return true;
        }
        return false;
    }

    template <typename FlushCallback>
    void Flush(FlushCallback&& cb) {
        for (const auto& kv : m_map) {
            void* pEntity = kv.first;
            for (const auto& dbEntry : kv.second) {
                cb(dbEntry.pFrustumDB, pEntity, dbEntry.cullStatus);
                ++m_totalFlushedUpdates;
            }
        }
        m_map.clear();
    }

    void SnapshotAndClear(std::unordered_map<void*, std::vector<DatabaseEntry>>& outSnapshot) {
        outSnapshot.clear();
        outSnapshot.swap(m_map);
    }

    void RecordFlushedCount(std::uint32_t count) noexcept {
        m_totalFlushedUpdates += count;
    }

    void Clear() {
        m_map.clear();
    }

    std::size_t UniqueEntityCount() const noexcept { return m_map.size(); }
    std::uint32_t TotalDeferredCalls() const noexcept { return m_totalDeferredCalls; }
    std::uint32_t TotalFlushedUpdates() const noexcept { return m_totalFlushedUpdates; }
    std::uint32_t DeletionsHandled() const noexcept { return m_deletionsHandled; }

    void ResetStats() noexcept {
        m_totalDeferredCalls = 0;
        m_totalFlushedUpdates = 0;
        m_deletionsHandled = 0;
    }

private:
    std::unordered_map<void*, std::vector<DatabaseEntry>> m_map;
    std::uint32_t m_totalDeferredCalls = 0;
    std::uint32_t m_totalFlushedUpdates = 0;
    std::uint32_t m_deletionsHandled = 0;
};

} // namespace G3O
