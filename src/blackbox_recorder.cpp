#include "blackbox_recorder.h"
#include <windows.h>
#include <cstring>
#include <algorithm>

namespace G3O {

static ThreadBlackboxSlot s_slots[BLACKBOX_MAX_SLOTS];
static thread_local ThreadBlackboxSlot* tl_mySlot = nullptr;

const char* Blackbox_GetOpName(uint32_t opType) {
    switch (opType) {
        case BB_OP_READING_PAK: return "Чтение архива (.pak/.mod)";
        case BB_OP_DECOMPRESSING_PAK: return "Распаковка блока (.pak/.mod)";
        case BB_OP_DECOMPRESSING_SAVE: return "Распаковка сейва (.g3savcpxdat)";
        case BB_OP_CREATING_TEXTURE: return "Создание текстуры Direct3D 9";
        case BB_OP_PARSING_ENTITY: return "Десериализация сущности/свойства";
        case BB_OP_VFS_LOOKUP: return "Поиск файла в VFS";
        default: return "Ожидание / Фоновая работа";
    }
}

static ThreadBlackboxSlot* AcquireSlot() {
    if (tl_mySlot) return tl_mySlot;

    const uint32_t currentTid = GetCurrentThreadId();

    // 1. Try to find an existing slot already assigned to this TID
    for (size_t i = 0; i < BLACKBOX_MAX_SLOTS; ++i) {
        if (s_slots[i].tid.load(std::memory_order_relaxed) == currentTid) {
            tl_mySlot = &s_slots[i];
            return tl_mySlot;
        }
    }

    // 2. Claim an empty slot (tid == 0)
    for (size_t i = 0; i < BLACKBOX_MAX_SLOTS; ++i) {
        uint32_t expected = 0;
        if (s_slots[i].tid.compare_exchange_strong(expected, currentTid, std::memory_order_acq_rel)) {
            tl_mySlot = &s_slots[i];
            return tl_mySlot;
        }
    }

    // 3. Fallback: direct modulo slot if all 32 are claimed
    tl_mySlot = &s_slots[currentTid % BLACKBOX_MAX_SLOTS];
    tl_mySlot->tid.store(currentTid, std::memory_order_release);
    return tl_mySlot;
}

void Blackbox_BeginOp(uint32_t opType, const char* archive, uint64_t offset, uint32_t size, const char* resourceName) {
    ThreadBlackboxSlot* slot = AcquireSlot();
    if (!slot) return;

    // Advance sequence number to odd (indicating operation is active / in-flight)
    const uint32_t s = slot->seq_num.load(std::memory_order_relaxed);
    slot->seq_num.store((s + 1) | 1, std::memory_order_release);

    slot->op_type = opType;

    // For texture operations created from memory, preserve the preceding PAK archive and offset
    // so that errors correlate directly to the mod archive and block that supplied the texture bytes.
    if (opType == BB_OP_CREATING_TEXTURE) {
        if (resourceName && resourceName[0]) {
            size_t len = 0;
            while (resourceName[len] && len < sizeof(slot->resource_name) - 1) {
                slot->resource_name[len] = resourceName[len];
                len++;
            }
            slot->resource_name[len] = '\0';
        } else if (archive && (strcmp(archive, "TextureInMemory") == 0 || strcmp(archive, "TextureFile") == 0)) {
            strncpy_s(slot->resource_name, sizeof(slot->resource_name), archive, _TRUNCATE);
        }

        // Only override archive_name if an explicit, non-generic archive name was supplied
        if (archive && archive[0] && strcmp(archive, "TextureInMemory") != 0 && strcmp(archive, "TextureFile") != 0) {
            size_t len = 0;
            while (archive[len] && len < sizeof(slot->archive_name) - 1) {
                slot->archive_name[len] = archive[len];
                len++;
            }
            slot->archive_name[len] = '\0';
            slot->offset = offset;
            slot->size = size;
        }
    } else {
        slot->offset = offset;
        slot->size = size;
        if (archive && archive[0]) {
            size_t len = 0;
            while (archive[len] && len < sizeof(slot->archive_name) - 1) {
                slot->archive_name[len] = archive[len];
                len++;
            }
            slot->archive_name[len] = '\0';
        } else {
            slot->archive_name[0] = '\0';
        }

        if (resourceName && resourceName[0]) {
            size_t len = 0;
            while (resourceName[len] && len < sizeof(slot->resource_name) - 1) {
                slot->resource_name[len] = resourceName[len];
                len++;
            }
            slot->resource_name[len] = '\0';
        } else {
            slot->resource_name[0] = '\0';
        }
    }

    // Lightweight timestamp
    slot->timestamp_us = GetTickCount();
}

void Blackbox_EndOp() {
    ThreadBlackboxSlot* slot = tl_mySlot;
    if (!slot) return;

    // Advance sequence number to next even (indicating operation completed cleanly)
    const uint32_t s = slot->seq_num.load(std::memory_order_relaxed);
    slot->seq_num.store((s + 1) & ~1u, std::memory_order_release);
}

const ThreadBlackboxSlot* Blackbox_GetSlotForThread(uint32_t tid) {
    for (size_t i = 0; i < BLACKBOX_MAX_SLOTS; ++i) {
        if (s_slots[i].tid.load(std::memory_order_relaxed) == tid) {
            return &s_slots[i];
        }
    }
    return nullptr;
}

size_t Blackbox_SnapshotAll(ThreadBlackboxSlot* outSlots, size_t maxSlots) {
    if (!outSlots || maxSlots == 0) return 0;
    size_t count = 0;
    for (size_t i = 0; i < BLACKBOX_MAX_SLOTS && count < maxSlots; ++i) {
        const uint32_t tid = s_slots[i].tid.load(std::memory_order_relaxed);
        if (tid != 0) {
            outSlots[count].tid.store(tid, std::memory_order_relaxed);
            outSlots[count].seq_num.store(s_slots[i].seq_num.load(std::memory_order_relaxed), std::memory_order_relaxed);
            std::memcpy(outSlots[count].archive_name, s_slots[i].archive_name, sizeof(outSlots[count].archive_name));
            outSlots[count].offset = s_slots[i].offset;
            outSlots[count].size = s_slots[i].size;
            std::memcpy(outSlots[count].resource_name, s_slots[i].resource_name, sizeof(outSlots[count].resource_name));
            outSlots[count].op_type = s_slots[i].op_type;
            outSlots[count].timestamp_us = s_slots[i].timestamp_us;
            count++;
        }
    }
    return count;
}

} // namespace G3O
