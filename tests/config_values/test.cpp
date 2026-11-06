#include "config_value_parser.h"
#include <cstdio>
#include <cstdint>

static bool Expect(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

int main() {
    bool ok = true;
    float parsedFloat = -1.0f;
    ok &= Expect(G3O::IniValue::ParseFloat(" 85.5 ", 40.0f, 120.0f, parsedFloat) && parsedFloat == 85.5f,
                 "finite float with surrounding whitespace should parse");
    ok &= Expect(!G3O::IniValue::ParseFloat("85.5junk", 40.0f, 120.0f, parsedFloat),
                 "float prefixes with trailing text should be rejected");
    ok &= Expect(!G3O::IniValue::ParseFloat("nan", 40.0f, 120.0f, parsedFloat),
                 "NaN should be rejected");
    ok &= Expect(!G3O::IniValue::ParseFloat("inf", 0.0f, 20000.0f, parsedFloat),
                 "infinity should be rejected");
    ok &= Expect(!G3O::IniValue::ParseFloat("-1", 0.0f, 20000.0f, parsedFloat),
                 "out-of-range float should be rejected");

    int parsedInt = -1;
    ok &= Expect(G3O::IniValue::ParseInt(" 20 ", 0, 32, parsedInt) && parsedInt == 20,
                 "bounded integer should parse");
    ok &= Expect(!G3O::IniValue::ParseInt("20workers", 0, 32, parsedInt),
                 "integer prefixes with trailing text should be rejected");
    ok &= Expect(!G3O::IniValue::ParseInt("-1", 0, 32, parsedInt),
                 "out-of-range integer should be rejected");

    std::uintptr_t parsedMask = 0;
    ok &= Expect(G3O::IniValue::ParseUnsigned("0xFFFF", 0xFFFFFFFFu, parsedMask) && parsedMask == 0xFFFFu,
                 "hex affinity mask should parse");
    ok &= Expect(!G3O::IniValue::ParseUnsigned("0x100000000", 0xFFFFFFFFu, parsedMask),
                 "mask wider than x86 pointer should be rejected");
    ok &= Expect(!G3O::IniValue::ParseUnsigned("-1", 0xFFFFFFFFu, parsedMask),
                 "negative mask should be rejected");

    if (!ok) return 1;
    std::puts("PASS: strict numeric parsing rejects partial, non-finite, and out-of-range values");
    return 0;
}
