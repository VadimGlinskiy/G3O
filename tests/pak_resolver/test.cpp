#include "pak_resolver.h"
#include <windows.h>
#include <iostream>
#include <vector>
#include <cstring>

static bool Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        return false;
    }
    return true;
}

int main() {
    bool ok = true;
    std::cout << "[Test] Pak Resolver Test Suite starting...\n";

    // 1. Non-existent file test
    {
        G3O::PakResolvedResource res;
        bool found = G3O::ResolvePakVirtualResource("non_existent_archive_12345.pak", 100, res);
        ok &= Expect(!found, "ResolvePakVirtualResource should fail for non-existent file");
    }

    // 2. Synthetic G3V0 file test
    char tempPath[MAX_PATH] = {0};
    char tempDir[MAX_PATH] = {0};
    GetTempPathA(MAX_PATH, tempDir);
    GetTempFileNameA(tempDir, "g3v", 0, tempPath);

    HANDLE hFile = CreateFileA(tempPath, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        std::cerr << "FAIL: Could not create temp file " << tempPath << "\n";
        return 1;
    }

    // Build synthetic G3V0 archive
    // Header (32 bytes):
    // 0..3: version/dummy (0)
    // 4..7: 'G', '3', 'V', '0'
    // 8..23: dummy metadata
    // 24..27: fat_offset uint32
    // 28..31: dummy
    std::vector<uint8_t> fileData;
    fileData.resize(32, 0);
    fileData[4] = 'G';
    fileData[5] = '3';
    fileData[6] = 'V';
    fileData[7] = '0';

    // File 1 data: at offset 32, length 100 bytes
    const uint64_t f1_offset = 32;
    const uint64_t f1_csize = 100;
    const uint64_t f1_usize = 250;
    std::vector<uint8_t> dummyContent(f1_csize, 0xAA);
    fileData.insert(fileData.end(), dummyContent.begin(), dummyContent.end());

    // File 2 data: at offset 132, length 50 bytes
    const uint64_t f2_offset = 132;
    const uint64_t f2_csize = 50;
    const uint64_t f2_usize = 80;
    std::vector<uint8_t> dummyContent2(f2_csize, 0xBB);
    fileData.insert(fileData.end(), dummyContent2.begin(), dummyContent2.end());

    // FAT begins at fileData.size() = 182
    const uint32_t fat_offset = static_cast<uint32_t>(fileData.size());
    *reinterpret_cast<uint32_t*>(&fileData[24]) = fat_offset;

    // FAT Entry 1:
    // uint64_t off (8), uint64_t csize (8), uint64_t usize (8), uint64_t flags (8) -> 32 bytes
    // uint32_t l1 (4), string l1 (including null)
    // uint32_t l2 (4), string l2 (including null)
    const char* vpath1 = "_compiledImage/G3_World_Texture.dds";
    const uint32_t l1 = static_cast<uint32_t>(strlen(vpath1) + 1);
    const char* apath1 = "RawTextures/World/Texture.tga";
    const uint32_t l2 = static_cast<uint32_t>(strlen(apath1) + 1);

    auto append64 = [&](uint64_t val) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&val);
        fileData.insert(fileData.end(), p, p + 8);
    };
    auto append32 = [&](uint32_t val) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&val);
        fileData.insert(fileData.end(), p, p + 4);
    };

    append64(f1_offset);
    append64(f1_csize);
    append64(f1_usize);
    append64(0); // flags/time
    append32(l1);
    fileData.insert(fileData.end(), reinterpret_cast<const uint8_t*>(vpath1), reinterpret_cast<const uint8_t*>(vpath1) + l1);
    append32(l2);
    fileData.insert(fileData.end(), reinterpret_cast<const uint8_t*>(apath1), reinterpret_cast<const uint8_t*>(apath1) + l2);

    // FAT Entry 2:
    const char* vpath2 = "Templates/NPC_Orc_Warrior.tple";
    const uint32_t l1_2 = static_cast<uint32_t>(strlen(vpath2) + 1);
    const char* apath2 = "Author/Orc.tple";
    const uint32_t l2_2 = static_cast<uint32_t>(strlen(apath2) + 1);

    append64(f2_offset);
    append64(f2_csize);
    append64(f2_usize);
    append64(0);
    append32(l1_2);
    fileData.insert(fileData.end(), reinterpret_cast<const uint8_t*>(vpath2), reinterpret_cast<const uint8_t*>(vpath2) + l1_2);
    append32(l2_2);
    fileData.insert(fileData.end(), reinterpret_cast<const uint8_t*>(apath2), reinterpret_cast<const uint8_t*>(apath2) + l2_2);

    DWORD written = 0;
    WriteFile(hFile, fileData.data(), static_cast<DWORD>(fileData.size()), &written, NULL);
    CloseHandle(hFile);

    // Test resolving offset inside Entry 1 (e.g. f1_offset + 25 = 57)
    {
        G3O::PakResolvedResource res;
        bool found = G3O::ResolvePakVirtualResource(tempPath, 57, res);
        ok &= Expect(found, "Entry 1 should be found for offset 57");
        ok &= Expect(res.found, "res.found must be true");
        ok &= Expect(std::strcmp(res.virtualPath, vpath1) == 0, "Virtual path mismatch for Entry 1");
        ok &= Expect(std::strcmp(res.authorPath, apath1) == 0, "Author path mismatch for Entry 1");
        ok &= Expect(res.fileOffset == f1_offset, "File offset mismatch for Entry 1");
        ok &= Expect(res.compressedSize == f1_csize, "Compressed size mismatch for Entry 1");
        ok &= Expect(res.uncompressedSize == f1_usize, "Uncompressed size mismatch for Entry 1");
    }

    // Test resolving offset inside Entry 2 (e.g. f2_offset = 132)
    {
        G3O::PakResolvedResource res;
        bool found = G3O::ResolvePakVirtualResource(tempPath, 132, res);
        ok &= Expect(found, "Entry 2 should be found for offset 132");
        ok &= Expect(std::strcmp(res.virtualPath, vpath2) == 0, "Virtual path mismatch for Entry 2");
        ok &= Expect(std::strcmp(res.authorPath, apath2) == 0, "Author path mismatch for Entry 2");
        ok &= Expect(res.fileOffset == f2_offset, "File offset mismatch for Entry 2");
    }

    // Test resolving offset beyond files (e.g. 500)
    {
        G3O::PakResolvedResource res;
        bool found = G3O::ResolvePakVirtualResource(tempPath, 500, res);
        ok &= Expect(!found, "Offset 500 should not match any file");
    }

    // Clean up
    DeleteFileA(tempPath);

    if (ok) {
        std::cout << "[Test] All Pak Resolver tests PASSED!\n";
        return 0;
    } else {
        std::cerr << "[Test] Some tests FAILED!\n";
        return 1;
    }
}
