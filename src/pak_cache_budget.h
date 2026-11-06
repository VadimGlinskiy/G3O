#pragma once

#include <atomic>
#include <cstddef>

namespace G3O {

class PakCacheBudget {
public:
    void Configure(size_t limit) noexcept {
        m_used.store(0, std::memory_order_relaxed);
        m_limit.store(limit, std::memory_order_release);
    }

    bool TryReserve(size_t bytes) noexcept {
        const size_t limit = m_limit.load(std::memory_order_acquire);
        if (bytes > limit) return false;

        size_t used = m_used.load(std::memory_order_relaxed);
        while (used <= limit - bytes) {
            if (m_used.compare_exchange_weak(used, used + bytes,
                                             std::memory_order_acq_rel,
                                             std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }

    void Release(size_t bytes) noexcept {
        m_used.fetch_sub(bytes, std::memory_order_acq_rel);
    }

    size_t Used() const noexcept {
        return m_used.load(std::memory_order_acquire);
    }

    size_t Limit() const noexcept {
        return m_limit.load(std::memory_order_acquire);
    }

private:
    std::atomic<size_t> m_used{0};
    std::atomic<size_t> m_limit{0};
};

} // namespace G3O
