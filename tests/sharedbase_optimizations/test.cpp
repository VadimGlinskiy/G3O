#include "sharedbase_string_compare.h"
#include <cstdio>

static bool Expect(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

int main() {
    bool ok = true;
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
    if (!ok) return 1;
    std::puts("PASS: SharedBase string comparisons check complete strings");
    return 0;
}
