#include "pak_streaming_ring.h"
#include "pak_content_key.h"
#include <libdeflate.h>
#include <windows.h>
#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <chrono>
#include <thread>

static bool Expect(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "FAIL: " << msg << "\n";
        return false;
    }
    return true;
}

int main() {
    bool ok = true;
    std::cout << "[Test] Pak Streaming Ring Test Suite starting...\n";

    // 1. Initialize
    ok &= Expect(G3O::InitializePakStreamingRing(), "InitializePakStreamingRing should succeed");

    // 2. Create temporary G3V0 test archive
    char tempPath[MAX_PATH] = { 0 };
    char tempDir[MAX_PATH] = { 0 };
    GetTempPathA(MAX_PATH, tempDir);
    GetTempFileNameA(tempDir, "pak", 0, tempPath);

    HANDLE hArchive = CreateFileA(tempPath, GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    ok &= Expect(hArchive != INVALID_HANDLE_VALUE, "Create temporary archive file");

    struct TestBlock {
        std::vector<uint8_t> uncompressed;
        std::vector<uint8_t> compressed;
        uint64_t fileOffset;
        uint64_t contentKey;
    };

    struct libdeflate_compressor* comp = libdeflate_alloc_compressor(6);
    ok &= Expect(comp != nullptr, "libdeflate_alloc_compressor should succeed");

    constexpr size_t kBlockCount = 10;
    std::vector<TestBlock> blocks(kBlockCount);

    // Build synthetic compressed blocks
    std::vector<uint8_t> fileBytes;
    fileBytes.resize(32, 0); // 32-byte header
    fileBytes[4] = 'G'; fileBytes[5] = '3'; fileBytes[6] = 'V'; fileBytes[7] = '0';

    for (size_t i = 0; i < kBlockCount; ++i) {
        // Generate pseudo-random uncompressed data
        size_t uSize = 4096 + (i * 2048);
        blocks[i].uncompressed.resize(uSize);
        for (size_t b = 0; b < uSize; ++b) {
            blocks[i].uncompressed[b] = static_cast<uint8_t>((b * 13 + i * 37) & 0xFF);
        }

        blocks[i].compressed.resize(uSize + 256);
        size_t cSize = libdeflate_zlib_compress(comp, blocks[i].uncompressed.data(), uSize,
                                               blocks[i].compressed.data(), blocks[i].compressed.size());
        blocks[i].compressed.resize(cSize);

        blocks[i].fileOffset = fileBytes.size();
        blocks[i].contentKey = G3O::ComputeBlockContentKey(blocks[i].compressed.data(),
                                                          static_cast<DWORD>(cSize),
                                                          static_cast<DWORD>(uSize));

        fileBytes.insert(fileBytes.end(), blocks[i].compressed.begin(), blocks[i].compressed.end());
    }

    libdeflate_free_compressor(comp);

    // Build FAT table
    uint32_t fatOffset = static_cast<uint32_t>(fileBytes.size());
    *reinterpret_cast<uint32_t*>(&fileBytes[24]) = fatOffset;

    for (size_t i = 0; i < kBlockCount; ++i) {
        // Pattern: [off: 8][c_sz: 8][u_sz: 8][flags: 8][l1: 4][vpath: l1][l2: 4][apath: l2]
        uint64_t off = blocks[i].fileOffset;
        uint64_t c_sz = blocks[i].compressed.size();
        uint64_t u_sz = blocks[i].uncompressed.size();
        uint64_t flags = 0x0200000000ULL; // Pattern with 0x02 in high dword

        auto append64 = [&](uint64_t v) {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
            fileBytes.insert(fileBytes.end(), p, p + 8);
        };
        auto append32 = [&](uint32_t v) {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
            fileBytes.insert(fileBytes.end(), p, p + 4);
        };

        append64(off);
        append64(c_sz);
        append64(u_sz);
        append64(flags);

        std::string vpath = "_compiledMesh/Mesh_Object_" + std::to_string(i) + ".xcms";
        uint32_t l1 = static_cast<uint32_t>(vpath.length() + 1);
        append32(l1);
        fileBytes.insert(fileBytes.end(), reinterpret_cast<const uint8_t*>(vpath.c_str()),
                         reinterpret_cast<const uint8_t*>(vpath.c_str()) + l1);

        std::string apath = "Author/Raw_Object_" + std::to_string(i) + ".xcms";
        uint32_t l2 = static_cast<uint32_t>(apath.length() + 1);
        append32(l2);
        fileBytes.insert(fileBytes.end(), reinterpret_cast<const uint8_t*>(apath.c_str()),
                         reinterpret_cast<const uint8_t*>(apath.c_str()) + l2);
    }

    // Write file to disk
    DWORD written = 0;
    WriteFile(hArchive, fileBytes.data(), static_cast<DWORD>(fileBytes.size()), &written, NULL);
    FlushFileBuffers(hArchive);
    CloseHandle(hArchive);

    // Reopen for reading
    hArchive = CreateFileA(tempPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    ok &= Expect(hArchive != INVALID_HANDLE_VALUE, "Reopen archive for shared read");

    // 3. Trigger lookahead on block 0
    G3O::PakStreamingRing_OnBlockRead(hArchive, blocks[0].fileOffset, static_cast<uint32_t>(blocks[0].compressed.size()));

    // Give background workers 100ms to decompress lookahead blocks
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 4. Try consuming lookahead block 1
    std::vector<uint8_t> outBuffer(blocks[1].uncompressed.size());
    DWORD outWritten = 0;
    bool consumed1 = G3O::PakStreamingRing_TryConsume(blocks[1].contentKey, outBuffer.data(),
                                                     static_cast<uint32_t>(outBuffer.size()), &outWritten);
    ok &= Expect(consumed1, "Block 1 should be pre-decompressed and consumed");
    ok &= Expect(outWritten == blocks[1].uncompressed.size(), "Block 1 size should match");
    ok &= Expect(std::memcmp(outBuffer.data(), blocks[1].uncompressed.data(), outWritten) == 0,
                 "Block 1 decompressed content must be bit-for-bit identical");

    // 5. Try consuming block 1 AGAIN (must fail! Zero inter-request caching rule!)
    bool consumed1Again = G3O::PakStreamingRing_TryConsume(blocks[1].contentKey, outBuffer.data(),
                                                          static_cast<uint32_t>(outBuffer.size()), &outWritten);
    ok &= Expect(!consumed1Again, "Block 1 must NOT be consumable twice (zero persistence)");

    // 6. Miss test with non-existent key
    bool consumedMiss = G3O::PakStreamingRing_TryConsume(0xDEADBEEF12345678ULL, outBuffer.data(),
                                                        static_cast<uint32_t>(outBuffer.size()), &outWritten);
    ok &= Expect(!consumedMiss, "Non-existent key should return false (miss)");

    // 7. Telemetry check
    ok &= Expect(G3O::PakStreamingRing_GetHits() >= 1, "Hits counter >= 1");
    ok &= Expect(G3O::PakStreamingRing_GetMisses() >= 2, "Misses counter >= 2");
    ok &= Expect(G3O::PakStreamingRing_GetConsumedBytes() > 0, "Consumed bytes > 0");

    // 8. Cleanup
    CloseHandle(hArchive);
    DeleteFileA(tempPath);
    G3O::ShutdownPakStreamingRing();

    std::cout << "[Test] Pak Streaming Ring Test Suite result: " << (ok ? "ALL PASS" : "FAILED") << "\n";
    return ok ? 0 : 1;
}
