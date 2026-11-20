#include "sharedbase_string_compare.h"
#include "math_simd_optimizer.h"
#include <cstdio>
#include <cstring>
#include <cstdint>

#ifdef CompareString
#undef CompareString
#endif

static bool Expect(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

#pragma pack(push, 1)
struct MockBCStringData {
    int32_t length;
    int16_t refCount;
    int16_t pad;
    char chars[64];
};
#pragma pack(pop)

static const char* MakeMockBuffer(MockBCStringData& storage, const char* str) {
    if (!str) return nullptr;
    int32_t len = static_cast<int32_t>(std::strlen(str));
    storage.length = len;
    storage.refCount = 1;
    storage.pad = 0;
    std::memcpy(storage.chars, str, static_cast<size_t>(len + 1));
    return storage.chars;
}

int main() {
    bool ok = true;

    // Legacy raw-pointer comparisons
    ok &= Expect(G3O::SharedBase::CompareString("same", "same"),
                 "equal strings should compare equal");
    ok &= Expect(!G3O::SharedBase::CompareString("prefix", "pre"),
                 "different lengths with the same prefix should not compare equal");
    ok &= Expect(!G3O::SharedBase::CompareString("left", "lest"),
                 "different same-length strings should not compare equal");
    ok &= Expect(G3O::SharedBase::CompareStringNoCase("Gothic", "gOTHIC"),
                 "case-insensitive comparison should ignore letter case");
    ok &= Expect(!G3O::SharedBase::CompareStringNoCase("Gothic", "Goth"),
                 "case-insensitive comparison should still compare the full string");
    ok &= Expect(G3O::SharedBase::CompareString(nullptr, nullptr),
                 "identical null inputs should compare equal");
    ok &= Expect(!G3O::SharedBase::CompareString(nullptr, ""),
                 "one null input should not compare equal to an empty string");

    // Fast bCString structure comparisons (with [str-8] length header)
    MockBCStringData buf1{}, buf2{}, buf3{}, buf4{}, bufEmpty{};
    const char* pGothic1 = MakeMockBuffer(buf1, "Gothic");
    const char* pGothic2 = MakeMockBuffer(buf2, "Gothic");
    const char* pgOTHIC  = MakeMockBuffer(buf3, "gOTHIC");
    const char* pGoth    = MakeMockBuffer(buf4, "Goth");
    const char* pEmpty   = MakeMockBuffer(bufEmpty, "");
    const char* pNull    = nullptr;

    // Pointer equality / COW shared buffer
    ok &= Expect(G3O::SharedBase::FastStringCompare(&pGothic1, &pGothic1),
                 "FastStringCompare: identical pointer should compare equal");
    ok &= Expect(G3O::SharedBase::FastStringCompare(&pGothic1, &pGothic2),
                 "FastStringCompare: identical content in different buffers should compare equal");
    ok &= Expect(!G3O::SharedBase::FastStringCompare(&pGothic1, &pGoth),
                 "FastStringCompare: different length strings should immediately return false");
    ok &= Expect(!G3O::SharedBase::FastStringCompare(&pGothic1, &pgOTHIC),
                 "FastStringCompare: different case should not compare equal in case-sensitive mode");
    ok &= Expect(G3O::SharedBase::FastStringCompare(&pNull, &pNull),
                 "FastStringCompare: two null bCString instances should compare equal");
    ok &= Expect(!G3O::SharedBase::FastStringCompare(&pNull, &pGothic1),
                 "FastStringCompare: null vs non-null should return false");
    ok &= Expect(G3O::SharedBase::FastStringCompare(&pEmpty, &pEmpty),
                 "FastStringCompare: empty strings should compare equal");

    // Case-insensitive fast comparison
    ok &= Expect(G3O::SharedBase::FastStringCompareNoCase(&pGothic1, &pgOTHIC),
                 "FastStringCompareNoCase: different case should compare equal");
    ok &= Expect(!G3O::SharedBase::FastStringCompareNoCase(&pGothic1, &pGoth),
                 "FastStringCompareNoCase: different length strings should immediately return false");
    ok &= Expect(G3O::SharedBase::FastStringCompareNoCase(&pNull, &pNull),
                 "FastStringCompareNoCase: two null bCString instances should compare equal");

    // Fast sphere-sphere intersection test (bCSphere::Intersects replacement)
    G3O::Sphere3 s1{ 10.0f, 0.0f, 0.0f, 0.0f };
    G3O::Sphere3 sOverlapping{ 5.0f, 12.0f, 0.0f, 0.0f }; // dist = 12, rSum = 15 -> dist < rSum -> intersects
    G3O::Sphere3 sSeparated{ 5.0f, 20.0f, 0.0f, 0.0f };   // dist = 20, rSum = 15 -> dist > rSum -> no intersection
    G3O::Sphere3 sTouching{ 5.0f, 15.0f, 0.0f, 0.0f };    // dist = 15, rSum = 15 -> dist == rSum -> no intersection (rSum^2 > dist^2 is false)
    G3O::Sphere3 sConcentric{ 2.0f, 0.0f, 0.0f, 0.0f };   // dist = 0, rSum = 12 -> intersects

    ok &= Expect(G3O::FastSphereIntersects(&s1, &sOverlapping),
                 "FastSphereIntersects: overlapping spheres should intersect");
    ok &= Expect(!G3O::FastSphereIntersects(&s1, &sSeparated),
                 "FastSphereIntersects: separated spheres should not intersect");
    ok &= Expect(!G3O::FastSphereIntersects(&s1, &sTouching),
                 "FastSphereIntersects: tangent/touching boundary should not intersect (strict >)");
    ok &= Expect(G3O::FastSphereIntersects(&s1, &sConcentric),
                 "FastSphereIntersects: concentric spheres should intersect");
    ok &= Expect(!G3O::FastSphereIntersects(nullptr, &s1),
                 "FastSphereIntersects: null pointer should return false");

    // Fast box-box intersection test (bCBox::Intersects)
    G3O::Box3 b1{ { 0.0f, 0.0f, 0.0f }, { 10.0f, 10.0f, 10.0f } };
    G3O::Box3 bOverlapping{ { 5.0f, 5.0f, 5.0f }, { 15.0f, 15.0f, 15.0f } };
    G3O::Box3 bSeparated{ { 20.0f, 0.0f, 0.0f }, { 30.0f, 10.0f, 10.0f } };

    ok &= Expect(G3O::FastBoxIntersects(&b1, &bOverlapping),
                 "FastBoxIntersects: overlapping boxes should intersect");
    ok &= Expect(!G3O::FastBoxIntersects(&b1, &bSeparated),
                 "FastBoxIntersects: separated boxes should not intersect");
    ok &= Expect(!G3O::FastBoxIntersects(nullptr, &b1),
                 "FastBoxIntersects: null pointer should return false");

    // Fast DJB2 hash verification against native SharedBase.dll!0x55F40 model
    auto ReferenceDjb2 = [](const char* s) -> uint32_t {
        if (!s) return 0;
        uint32_t hash = 0;
        while (*s) {
            signed char c = static_cast<signed char>(*s++);
            hash = hash * 33u + static_cast<uint32_t>(static_cast<int32_t>(c));
        }
        return hash;
    };

    const char* testStrings[] = {
        "",
        "a",
        "ab",
        "abc",
        "abcd",
        "abcde",
        "12345678",
        "Gothic3_Hero_Player_Entity",
        "bTPropertyContainer<gCInfo_PS>",
        "Герой_Миртана_Монтера_123", // CP1251 Cyrillic with negative signed char
        "Umlauts_äöüß_Test_Length_Longer_Than_32_Bytes_To_Check_Loop"
    };

    for (const char* str : testStrings) {
        MockBCStringData mock{};
        const char* pBuf = MakeMockBuffer(mock, str);
        uint32_t expected = ReferenceDjb2(str);
        uint32_t actual = G3O::SharedBase::FastDjb2Hash(&pBuf);
        if (actual != expected) {
            std::fprintf(stderr, "FAIL: FastDjb2Hash mismatch for '%s': expected 0x%08X, got 0x%08X\n",
                         str, expected, actual);
            ok = false;
        }
    }

    // Null and empty bCString tests
    ok &= Expect(G3O::SharedBase::FastDjb2Hash(&pNull) == 0, "FastDjb2Hash(null) should return 0");
    ok &= Expect(G3O::SharedBase::FastDjb2Hash(nullptr) == 0, "FastDjb2Hash(nullptr) should return 0");
    ok &= Expect(G3O::SharedBase::FastDjb2Hash(&pEmpty) == 0, "FastDjb2Hash(empty) should return 0");

    // Vectorized in-place CompareFast replacement test in live executable memory
    static const uint8_t s_compareFastReplacement[75] = {
        0x8B, 0x01,             // mov eax, [ecx]
        0x8B, 0x4C, 0x24, 0x04, // mov ecx, [esp+4]
        0x8B, 0x11,             // mov edx, [ecx]
        0x3B, 0xC2,             // cmp eax, edx
        0x74, 0x24,             // je equal (10001b10)
        0x85, 0xC0,             // test eax, eax
        0x74, 0x25,             // jz not_equal (10001b15)
        0x85, 0xD2,             // test edx, edx
        0x74, 0x21,             // jz not_equal (10001b15)
        0x8B, 0x48, 0xF8,       // mov ecx, [eax-8]
        0x3B, 0x4A, 0xF8,       // cmp ecx, [edx-8]
        0x75, 0x19,             // jne not_equal (10001b15)
        0x85, 0xC9,             // test ecx, ecx
        0x74, 0x10,             // jz equal (10001b10)
        0x56,                   // push esi
        0x57,                   // push edi
        0x8B, 0xF0,             // mov esi, eax
        0x8B, 0xFA,             // mov edi, edx
        0xF3, 0xA6,             // repe cmpsb
        0x0F, 0x94, 0xC0,       // sete al
        0x5F,                   // pop edi
        0x5E,                   // pop esi
        0xC2, 0x04, 0x00,       // ret 4
        // equal:
        0xB0, 0x01,             // mov al, 1
        0xC2, 0x04, 0x00,       // ret 4
        // not_equal:
        0x32, 0xC0,             // xor al, al
        0xC2, 0x04, 0x00,       // ret 4
        // padding:
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
        0x90
    };

    void* execMem = VirtualAlloc(NULL, sizeof(s_compareFastReplacement),
                                 MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (execMem) {
        std::memcpy(execMem, s_compareFastReplacement, sizeof(s_compareFastReplacement));
        FlushInstructionCache(GetCurrentProcess(), execMem, sizeof(s_compareFastReplacement));
        using CompareFastFn = bool (__thiscall*)(const void* pThis, const void* pOther);
        auto fnCompare = reinterpret_cast<CompareFastFn>(execMem);

        ok &= Expect(fnCompare(&pGothic1, &pGothic1) == true, "Vectorized CompareFast: identical pointer returns true");
        ok &= Expect(fnCompare(&pGothic1, &pGothic2) == true, "Vectorized CompareFast: identical content returns true");
        ok &= Expect(fnCompare(&pGothic1, &pGoth) == false, "Vectorized CompareFast: prefix/different length returns false");
        ok &= Expect(fnCompare(&pGothic1, &pgOTHIC) == false, "Vectorized CompareFast: different case returns false");
        ok &= Expect(fnCompare(&pNull, &pNull) == true, "Vectorized CompareFast: two null bCString instances return true");
        ok &= Expect(fnCompare(&pNull, &pGothic1) == false, "Vectorized CompareFast: null vs non-null returns false");
        ok &= Expect(fnCompare(&pGothic1, &pNull) == false, "Vectorized CompareFast: non-null vs null returns false");
        ok &= Expect(fnCompare(&pEmpty, &pEmpty) == true, "Vectorized CompareFast: empty strings return true");

        VirtualFree(execMem, 0, MEM_RELEASE);
    } else {
        std::puts("FAIL: VirtualAlloc for CompareFast test buffer failed");
        ok = false;
    }

    if (!ok) return 1;
    std::puts("PASS: SharedBase string and geometry optimizations verified");
    return 0;
}
