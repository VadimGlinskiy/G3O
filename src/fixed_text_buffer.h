#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace G3O {

class FixedTextBuffer {
public:
    FixedTextBuffer(char* storage, std::size_t capacity) noexcept
        : m_storage(storage), m_capacity(capacity), m_length(0), m_truncated(false) {
        if (m_storage && m_capacity > 0) m_storage[0] = '\0';
    }

    void Reset() noexcept {
        m_length = 0;
        m_truncated = false;
        if (m_storage && m_capacity > 0) m_storage[0] = '\0';
    }

    bool Append(const char* text) noexcept {
        if (!text || !m_storage || m_capacity == 0 || m_truncated) return false;
        const std::size_t textLength = std::strlen(text);
        const std::size_t available = m_capacity - m_length - 1;
        if (textLength > available) {
            if (available > 0) std::memcpy(m_storage + m_length, text, available);
            m_length += available;
            m_storage[m_length] = '\0';
            m_truncated = true;
            return false;
        }
        if (textLength > 0) std::memcpy(m_storage + m_length, text, textLength);
        m_length += textLength;
        m_storage[m_length] = '\0';
        return true;
    }

    bool AppendFormat(const char* format, ...) noexcept {
        if (!format || !m_storage || m_capacity == 0 || m_truncated) return false;
        const std::size_t available = m_capacity - m_length;
        va_list args;
        va_start(args, format);
        const int written = std::vsnprintf(m_storage + m_length, available, format, args);
        va_end(args);
        if (written < 0 || static_cast<std::size_t>(written) >= available) {
            m_length = m_capacity - 1;
            m_storage[m_length] = '\0';
            m_truncated = true;
            return false;
        }
        m_length += static_cast<std::size_t>(written);
        return true;
    }

    void Finish() noexcept {
        if (!m_storage || m_capacity == 0) return;
        if (m_truncated) {
            static constexpr char marker[] = "\n[ОТЧЁТ УКОРОЧЕН]\n";
            constexpr std::size_t markerLength = sizeof(marker) - 1;
            const std::size_t copyLength = markerLength < m_capacity - 1
                ? markerLength
                : m_capacity - 1;
            const std::size_t markerOffset = markerLength - copyLength;
            m_length = m_capacity - 1 - copyLength;
            std::memcpy(m_storage + m_length, marker + markerOffset, copyLength);
            m_length += copyLength;
        }
        m_storage[m_length] = '\0';
    }

    void clear() noexcept { Reset(); }
    const char* CStr() const noexcept { return m_storage ? m_storage : ""; }
    const char* c_str() const noexcept { return CStr(); }
    std::size_t Size() const noexcept { return m_length; }
    bool empty() const noexcept { return m_length == 0; }
    bool IsTruncated() const noexcept { return m_truncated; }

    FixedTextBuffer& operator+=(const char* text) noexcept {
        Append(text);
        return *this;
    }

private:
    char* m_storage;
    std::size_t m_capacity;
    std::size_t m_length;
    bool m_truncated;
};

} // namespace G3O
