#include "layer_ring_arena.h"
#include "config_manager.h"
#include "diagnostic_logger.h"
#include "decompressor_avx2.h"
#include <libdeflate.h>
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

// Minimal stubs for standalone test compilation without game runtime dependencies
namespace G3O {
const Config& GetConfig() {
    static Config s_cfg{};
    return s_cfg;
}
void Log(const char* /*category*/, const char* /*fmt*/, ...) {}
} // namespace G3O

extern "C" {
bool G3O_AVX2_IsSupported() { return false; }
libdeflate_decompressor* G3O_AVX2_AllocDecompressor() { return libdeflate_alloc_decompressor(); }
int G3O_AVX2_DecompressZlib(libdeflate_decompressor* d, const void* in, size_t inBytes, void* out, size_t outCap, size_t* actualOut) {
    return libdeflate_zlib_decompress(d, in, inBytes, out, outCap, actualOut);
}
void G3O_AVX2_FreeDecompressor(libdeflate_decompressor* d) { libdeflate_free_decompressor(d); }
}

namespace {

struct SyntheticNode {
    std::string name;
    std::vector<uint8_t> rawData;
    std::vector<uint8_t> compData;
    bool corruptCompressed = false;
};

static bool Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        return false;
    }
    return true;
}

static void WriteLE32(std::vector<uint8_t>& buf, size_t offset, uint32_t val) {
    std::memcpy(buf.data() + offset, &val, sizeof(val));
}

static void AppendLE32(std::vector<uint8_t>& buf, uint32_t val) {
    const size_t off = buf.size();
    buf.resize(off + sizeof(val));
    std::memcpy(buf.data() + off, &val, sizeof(val));
}

static void AppendLE64(std::vector<uint8_t>& buf, uint64_t val) {
    const size_t off = buf.size();
    buf.resize(off + sizeof(val));
    std::memcpy(buf.data() + off, &val, sizeof(val));
}

static std::vector<uint8_t> CompressZlib(const std::vector<uint8_t>& raw) {
    libdeflate_compressor* comp = libdeflate_alloc_compressor(6);
    if (!comp) return {};
    std::vector<uint8_t> out(libdeflate_zlib_compress_bound(comp, raw.size()));
    const size_t written = libdeflate_zlib_compress(comp, raw.data(), raw.size(), out.data(), out.size());
    libdeflate_free_compressor(comp);
    out.resize(written);
    return out;
}

static bool ReferenceDecompressZlib(const void* inBuf, size_t inSize, std::vector<uint8_t>& outBuf, size_t expectedUncompSize) {
    libdeflate_decompressor* dec = libdeflate_alloc_decompressor();
    if (!dec) return false;
    outBuf.assign(expectedUncompSize, 0);
    size_t actualOut = 0;
    const int res = libdeflate_zlib_decompress(dec, inBuf, inSize, outBuf.data(), outBuf.size(), &actualOut);
    libdeflate_free_decompressor(dec);
    return res == 0 && actualOut == expectedUncompSize;
}

static void AppendFatEntry(
    std::vector<uint8_t>& fat,
    uint32_t flags,
    uint64_t fileOffset,
    uint64_t compSize,
    uint64_t uncompSize,
    const std::string& fullPath
) {
    AppendLE32(fat, flags);
    AppendLE64(fat, fileOffset);
    AppendLE64(fat, compSize);
    AppendLE64(fat, uncompSize);
    for (int i = 0; i < 12; ++i) {
        fat.push_back(0);
    }
    fat.insert(fat.end(), fullPath.begin(), fullPath.end());
    fat.push_back('\0');
}

static bool BuildSyntheticArchive(
    const char* archivePath,
    std::vector<SyntheticNode>& nodesInArchiveOrder,
    const std::vector<std::string>& secdatOrderStems
) {
    std::vector<uint8_t> fileBytes(32, 0);
    std::memcpy(fileBytes.data() + 4, "G3V0", 4);

    std::vector<uint8_t> fatBytes;

    for (auto& node : nodesInArchiveOrder) {
        node.compData = CompressZlib(node.rawData);
        if (node.compData.size() < 16) return false;
        if (node.corruptCompressed) {
            // Preserve first 4 bytes, corrupt interior deflate payload
            for (size_t k = 4; k + 4 < node.compData.size(); ++k) {
                node.compData[k] ^= 0xFF;
            }
        }
        const uint64_t offset = fileBytes.size();
        fileBytes.insert(fileBytes.end(), node.compData.begin(), node.compData.end());
        AppendFatEntry(
            fatBytes,
            0x00020020,
            offset,
            node.compData.size(),
            node.rawData.size(),
            "G3_World_01/RB_Mod/" + node.name
        );
    }

    if (!secdatOrderStems.empty()) {
        std::string secdatRaw;
        for (const auto& stem : secdatOrderStems) {
            secdatRaw += stem;
            secdatRaw.push_back('\0');
        }
        std::vector<uint8_t> secdatVec(secdatRaw.begin(), secdatRaw.end());
        std::vector<uint8_t> secdatComp = CompressZlib(secdatVec);
        const uint64_t secdatOff = fileBytes.size();
        fileBytes.insert(fileBytes.end(), secdatComp.begin(), secdatComp.end());
        AppendFatEntry(
            fatBytes,
            0x00020020,
            secdatOff,
            secdatComp.size(),
            secdatVec.size(),
            "G3_World_01/RB_Mod/RB_Mod.secdat"
        );
    }

    const uint32_t fatOffset = static_cast<uint32_t>(fileBytes.size());
    WriteLE32(fileBytes, 24, fatOffset);
    fileBytes.insert(fileBytes.end(), fatBytes.begin(), fatBytes.end());

    HANDLE hFile = CreateFileA(archivePath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(hFile, fileBytes.data(), static_cast<DWORD>(fileBytes.size()), &written, NULL);
    CloseHandle(hFile);
    return ok && written == fileBytes.size();
}

static SyntheticNode MakeNode(const std::string& name, size_t rawSize, uint8_t seed, bool corrupt = false) {
    SyntheticNode n;
    n.name = name;
    n.rawData.resize(rawSize);
    uint32_t state = 0x12345678u ^ (static_cast<uint32_t>(seed) * 0x9E3779B9u);
    for (size_t i = 0; i < rawSize; ++i) {
        state = state * 1664525u + 1013904223u;
        n.rawData[i] = static_cast<uint8_t>((state >> 16) ^ seed ^ (i & 0xFF));
    }
    n.corruptCompressed = corrupt;
    return n;
}

} // namespace

int main() {
    bool ok = true;
    std::cout << "[Test] LayerRingArena standalone verification starting...\n";

    char tempDir[MAX_PATH] = {0};
    char tempFile[MAX_PATH] = {0};
    if (GetTempPathA(MAX_PATH, tempDir) == 0 || !GetTempFileNameA(tempDir, "g3o", 0, tempFile)) {
        std::cerr << "FAIL: could not create unique temporary archive path\n";
        return 1;
    }
    std::string archivePath = tempFile;

    // Archive order intentionally differs from .secdat order, and tests prefix stem "RB_Mod_Layer_1" vs "RB_Mod_Layer_10"
    std::vector<SyntheticNode> archiveNodes = {
        MakeNode("RB_Mod_Layer_1.node",  4096, 10),
        MakeNode("RB_Mod_Layer_10.node", 4200, 20),
        MakeNode("RB_Mod_Layer_2.node",  4400, 30),
        MakeNode("RB_Mod_Layer_3.node",  4600, 40),
        MakeNode("RB_Mod_Layer_4.node",  4800, 50, true), // Corrupted deflate payload to test worker failure
        MakeNode("RB_Mod_Layer_5.node",  5000, 60),
        MakeNode("RB_Mod_Layer_6.node",  5200, 70),
        MakeNode("RB_Mod_Layer_7.node",  5400, 80),
        MakeNode("RB_Mod_Layer_8.node",  5600, 90),
        MakeNode("RB_Mod_Layer_9.node",  5800, 100)
    };

    // In .secdat, RB_Mod_Layer_10 appears BEFORE RB_Mod_Layer_1 to verify token-boundary stem matching
    std::vector<std::string> secdatOrder = {
        "RB_Mod_Layer_10",
        "RB_Mod_Layer_1",
        "RB_Mod_Layer_2",
        "RB_Mod_Layer_3",
        "RB_Mod_Layer_4",
        "RB_Mod_Layer_5",
        "RB_Mod_Layer_6",
        "RB_Mod_Layer_7",
        "RB_Mod_Layer_8",
        "RB_Mod_Layer_9"
    };

    ok &= Expect(BuildSyntheticArchive(archivePath.c_str(), archiveNodes, secdatOrder), "Build synthetic G3V0 archive");

    // Expected active sequence after .secdat sorting:
    // [0]=Layer_10, [1]=Layer_1, [2]=Layer_2, [3]=Layer_3, [4]=Layer_4(corrupt),
    // [5]=Layer_5,  [6]=Layer_6, [7]=Layer_7, [8]=Layer_8, [9]=Layer_9
    std::vector<SyntheticNode> ordered = {
        archiveNodes[1], archiveNodes[0], archiveNodes[2], archiveNodes[3], archiveNodes[4],
        archiveNodes[5], archiveNodes[6], archiveNodes[7], archiveNodes[8], archiveNodes[9]
    };

    ok &= Expect(G3O::InitializeLayerRingArenaFromArchive(archivePath.c_str(), 2), "InitializeLayerRingArenaFromArchive");
    ok &= Expect(!G3O::LayerRingArena_IsActive(), "Arena inactive before OnSectorStart");

    // -------------------------------------------------------------------------
    // Phase 1: Unready slot fallback (worker paused), then unpause and verify
    // -------------------------------------------------------------------------
    G3O::LayerRingArena_SetWorkerPausedForTest(true);
    G3O::LayerRingArena_OnSectorStart("RB_Mod");
    ok &= Expect(G3O::LayerRingArena_IsActive(), "Arena active after OnSectorStart(RB_Mod)");

    {
        std::vector<uint8_t> outBuf(ordered[0].rawData.size(), 0xAA);
        DWORD written = 0xDEADBEEFu;
        const bool hitUnready = G3O::LayerRingArena_TryGet(
            ordered[0].compData.data(),
            static_cast<DWORD>(ordered[0].compData.size()),
            outBuf.data(),
            static_cast<DWORD>(outBuf.size()),
            &written
        );
        ok &= Expect(!hitUnready, "Unready slot must safely fall back (return false)");
        ok &= Expect(written == 0xDEADBEEFu, "pOutWritten untouched on unready slot fallback");
        ok &= Expect(G3O::LayerRingArena_GetStats().misses == 1, "Unready slot increments misses");
    }

    // Unpause worker and restart sector cleanly for full matrix testing
    G3O::LayerRingArena_SetWorkerPausedForTest(false);
    G3O::LayerRingArena_OnSectorEnd();
    ok &= Expect(!G3O::LayerRingArena_IsActive(), "Arena inactive after OnSectorEnd");

    // -------------------------------------------------------------------------
    // Phase 2: Collision rejection, exact match (unaligned), capacity check,
    //          skips, worker failure, and ring wrap-around
    // -------------------------------------------------------------------------
    G3O::LayerRingArena_OnSectorStart("RB_Mod");
    ok &= Expect(G3O::LayerRingArena_GetStats().hits == 0 && G3O::LayerRingArena_GetStats().misses == 0,
                 "Stats reset on sector restart");

    // 2.1 Same compSize and first4 as ordered[0] and ordered[1], but modified tail byte -> must reject!
    for (size_t idx : {size_t(0), size_t(1)}) {
        std::vector<uint8_t> colliding = ordered[idx].compData;
        colliding.back() ^= 0x5Au;
        std::vector<uint8_t> outBuf(ordered[idx].rawData.size(), 0xCC);
        DWORD written = 0;
        const bool falseHit = G3O::LayerRingArena_TryGet(
            colliding.data(),
            static_cast<DWORD>(colliding.size()),
            outBuf.data(),
            static_cast<DWORD>(outBuf.size()),
            &written
        );
        ok &= Expect(!falseHit, "Same compSize + first4 with different body must NOT match");
        ok &= Expect(outBuf[0] == 0xCC && written == 0, "Output untouched on rejected collision");
    }

    // 2.2 Completely unrelated compressed block -> safe fallback
    {
        SyntheticNode unrelated = MakeNode("OtherSector.node", 3500, 0xFE);
        unrelated.compData = CompressZlib(unrelated.rawData);
        std::vector<uint8_t> outBuf(unrelated.rawData.size(), 0xDD);
        DWORD written = 0;
        const bool unrelatedHit = G3O::LayerRingArena_TryGet(
            unrelated.compData.data(),
            static_cast<DWORD>(unrelated.compData.size()),
            outBuf.data(),
            static_cast<DWORD>(outBuf.size()),
            &written
        );
        ok &= Expect(!unrelatedHit, "Unrelated compressed block must safely fall back");
    }

    // 2.3 Insufficient outCapacity on ordered[0] -> must fail without consuming slot
    {
        std::vector<uint8_t> smallOut(ordered[0].rawData.size() - 1, 0xEE);
        DWORD written = 0x11223344u;
        const bool smallHit = G3O::LayerRingArena_TryGet(
            ordered[0].compData.data(),
            static_cast<DWORD>(ordered[0].compData.size()),
            smallOut.data(),
            static_cast<DWORD>(smallOut.size()),
            &written
        );
        ok &= Expect(!smallHit, "Insufficient outCapacity must return false");
        ok &= Expect(written == 0x11223344u, "pOutWritten unchanged when outCapacity is too small");
    }

    // 2.4 Exact match on ordered[0] using an intentionally unaligned input buffer (+1 byte offset)
    {
        std::vector<uint8_t> unalignedStorage(ordered[0].compData.size() + 8, 0);
        uint8_t* unalignedPtr = unalignedStorage.data() + 1;
        std::memcpy(unalignedPtr, ordered[0].compData.data(), ordered[0].compData.size());

        std::vector<uint8_t> outBuf(ordered[0].rawData.size(), 0);
        DWORD written = 0;
        const bool hit0 = G3O::LayerRingArena_TryGet(
            unalignedPtr,
            static_cast<DWORD>(ordered[0].compData.size()),
            outBuf.data(),
            static_cast<DWORD>(outBuf.size()),
            &written
        );
        std::vector<uint8_t> refOut;
        ok &= Expect(ReferenceDecompressZlib(unalignedPtr, ordered[0].compData.size(), refOut, ordered[0].rawData.size()),
                     "Reference zlib decompression of ordered[0]");
        ok &= Expect(hit0, "Exact match on ordered[0] (RB_Mod_Layer_10) must succeed");
        ok &= Expect(written == ordered[0].rawData.size(), "pOutWritten equals exact uncompressed size");
        ok &= Expect(std::memcmp(outBuf.data(), refOut.data(), refOut.size()) == 0,
                     "Byte-for-byte equality with reference decompression for ordered[0]");
    }

    // 2.5 Skip ordered[1] (Layer_1), request ordered[2] (Layer_2) and ordered[3] (Layer_3)
    for (size_t idx : {size_t(2), size_t(3)}) {
        std::vector<uint8_t> outBuf(ordered[idx].rawData.size(), 0);
        DWORD written = 0;
        const bool hit = G3O::LayerRingArena_TryGet(
            ordered[idx].compData.data(),
            static_cast<DWORD>(ordered[idx].compData.size()),
            outBuf.data(),
            static_cast<DWORD>(outBuf.size()),
            &written
        );
        std::vector<uint8_t> refOut;
        ok &= Expect(ReferenceDecompressZlib(ordered[idx].compData.data(), ordered[idx].compData.size(), refOut, ordered[idx].rawData.size()),
                     "Reference decompression for skipped/window layer");
        ok &= Expect(hit, "Layer after skip must succeed from ring slot");
        ok &= Expect(written == refOut.size() && std::memcmp(outBuf.data(), refOut.data(), refOut.size()) == 0,
                     "Byte-for-byte equality with reference decompression after skip");
    }

    // 2.6 Request ordered[4] (corrupted deflate stream in archive -> worker decompression failure)
    // Slot 4 % 4 == 0. Worker must mark failure and must NOT alias with slot 0 from ordered[0] or ordered[8]!
    {
        std::vector<uint8_t> outBuf(ordered[4].rawData.size(), 0x77);
        DWORD written = 0x55667788u;
        const bool corruptHit = G3O::LayerRingArena_TryGet(
            ordered[4].compData.data(),
            static_cast<DWORD>(ordered[4].compData.size()),
            outBuf.data(),
            static_cast<DWORD>(outBuf.size()),
            &written
        );
        ok &= Expect(!corruptHit, "Corrupted entry where worker decompression failed must return false");
        ok &= Expect(outBuf[0] == 0x77 && written == 0x55667788u, "Failed decompression must not write stale slot data");
    }

    // 2.7 Continue across ring boundary: ordered[5], skip ordered[6], request ordered[7], ordered[8] (reuses slot 0!), ordered[9]
    for (size_t idx : {size_t(5), size_t(7), size_t(8), size_t(9)}) {
        std::vector<uint8_t> outBuf(ordered[idx].rawData.size(), 0);
        DWORD written = 0;
        const bool hit = G3O::LayerRingArena_TryGet(
            ordered[idx].compData.data(),
            static_cast<DWORD>(ordered[idx].compData.size()),
            outBuf.data(),
            static_cast<DWORD>(outBuf.size()),
            &written
        );
        std::vector<uint8_t> refOut;
        ok &= Expect(ReferenceDecompressZlib(ordered[idx].compData.data(), ordered[idx].compData.size(), refOut, ordered[idx].rawData.size()),
                     "Reference decompression for wrap-around layer");
        ok &= Expect(hit, "Wrap-around layer (including slot 4%4 == 8%4 reuse after worker failure) must succeed");
        ok &= Expect(written == refOut.size() && std::memcmp(outBuf.data(), refOut.data(), refOut.size()) == 0,
                     "Byte-for-byte equality with reference decompression on wrap-around");
    }

    // -------------------------------------------------------------------------
    // Phase 3: Sector restart and Shutdown lifecycle verification
    // -------------------------------------------------------------------------
    G3O::LayerRingArena_OnSectorStart("RB_Mod");
    {
        std::vector<uint8_t> outBuf(ordered[0].rawData.size(), 0);
        DWORD written = 0;
        const bool hitRestart = G3O::LayerRingArena_TryGet(
            ordered[0].compData.data(),
            static_cast<DWORD>(ordered[0].compData.size()),
            outBuf.data(),
            static_cast<DWORD>(outBuf.size()),
            &written
        );
        ok &= Expect(hitRestart && written == ordered[0].rawData.size() &&
                     std::memcmp(outBuf.data(), ordered[0].rawData.data(), ordered[0].rawData.size()) == 0,
                     "Immediate sector restart serves ordered[0] accurately");
    }

    G3O::ShutdownLayerRingArena();
    ok &= Expect(!G3O::LayerRingArena_IsActive(), "Arena inactive after ShutdownLayerRingArena");
    DeleteFileA(archivePath.c_str());

    if (ok) {
        std::cout << "[Test] LayerRingArena standalone verification: PASS!\n";
        return 0;
    }
    return 1;
}
