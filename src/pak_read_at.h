#pragma once

#include <windows.h>
#include <cstdint>
#include <limits>

namespace G3O {

inline bool& InternalPakReadFlag() noexcept {
    static thread_local bool active = false;
    return active;
}

inline bool IsInternalPakRead() noexcept {
    return InternalPakReadFlag();
}

class ScopedInternalPakRead {
public:
    ScopedInternalPakRead() noexcept : previous_(InternalPakReadFlag()) {
        InternalPakReadFlag() = true;
    }
    ~ScopedInternalPakRead() { InternalPakReadFlag() = previous_; }

    ScopedInternalPakRead(const ScopedInternalPakRead&) = delete;
    ScopedInternalPakRead& operator=(const ScopedInternalPakRead&) = delete;

private:
    bool previous_;
};

inline bool ReadExactAt(HANDLE file, uint64_t offset, void* buffer, DWORD bytes) noexcept {
    if (!file || file == INVALID_HANDLE_VALUE || (!buffer && bytes != 0) ||
        offset > static_cast<uint64_t>((std::numeric_limits<LONGLONG>::max)())) {
        return false;
    }

    LARGE_INTEGER position = {};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) return false;

    DWORD bytesRead = 0;
    return ReadFile(file, buffer, bytes, &bytesRead, nullptr) && bytesRead == bytes;
}

} // namespace G3O
