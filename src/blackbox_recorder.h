#pragma once
#include <cstdint>
#include <atomic>
#include <cstddef>

namespace G3O {

enum BlackboxOpType : uint32_t {
    BB_OP_IDLE = 0,
    BB_OP_READING_PAK = 1,
    BB_OP_DECOMPRESSING_PAK = 2,
    BB_OP_DECOMPRESSING_SAVE = 3,
    BB_OP_CREATING_TEXTURE = 4,
    BB_OP_PARSING_ENTITY = 5,
    BB_OP_VFS_LOOKUP = 6
};

const char* Blackbox_GetOpName(uint32_t opType);

struct ThreadBlackboxSlot {
    std::atomic<uint32_t> tid{0};
    std::atomic<uint32_t> seq_num{0}; // odd = in-flight, even = idle / completed
    char archive_name[64] = {0};
    uint64_t offset{0};
    uint32_t size{0};
    char resource_name[128] = {0};
    uint32_t op_type{BB_OP_IDLE};
    uint64_t timestamp_us{0};
};

static constexpr size_t BLACKBOX_MAX_SLOTS = 32;

// Runtime lock-free operations for active threads
void Blackbox_BeginOp(uint32_t opType, const char* archive, uint64_t offset, uint32_t size, const char* resourceName = nullptr);
void Blackbox_EndOp();

// Post-mortem inspection operations (callable in crash handler without locks or allocations)
const ThreadBlackboxSlot* Blackbox_GetSlotForThread(uint32_t tid);
size_t Blackbox_SnapshotAll(ThreadBlackboxSlot* outSlots, size_t maxSlots);

// RAII scope helper for lightweight execution tracking
struct BlackboxScope {
    BlackboxScope(uint32_t opType, const char* archive, uint64_t offset, uint32_t size, const char* resourceName = nullptr) {
        Blackbox_BeginOp(opType, archive, offset, size, resourceName);
    }
    ~BlackboxScope() {
        Blackbox_EndOp();
    }
};

} // namespace G3O
