#pragma once

#include <cstdint>

namespace G3O {

enum class AllocatorMode : int {
    Disabled = 0,     // Vanilla engine allocators
    ShortUsage = 1,   // ShortUsage / ScrapHeap mimalloc replacement
    FullMemHeap = 2   // Full MemHeap / bCMemoryAdmin replacement with safe dispatch free
};

bool InitializeMemoryAllocator(int mode);
void ShutdownMemoryAllocator();

std::uint64_t GetMiAllocCount();
std::uint64_t GetMiFreeCount();
std::uint64_t GetLegacyFreeCount();
std::uint64_t GetMiReallocCount();
std::uint64_t GetLegacyReallocCount();

} // namespace G3O
