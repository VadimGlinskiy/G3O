#include "sharedbase_string_compare.h"
#include <cstdio>
#include <cstring>
#include <cstdint>

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

    if (!ok) return 1;
    std::puts("PASS: SharedBase string comparisons check complete strings and fast bCString header logic");
    return 0;
}
