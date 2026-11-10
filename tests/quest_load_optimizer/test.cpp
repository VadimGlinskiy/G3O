#include <windows.h>
#include <iostream>
#include <cstdint>
#include <atomic>

static std::atomic<bool> s_mockLoadActive{ false };
static std::atomic<uint32_t> s_mockQuestSuppressed{ 0 };
static std::atomic<uint32_t> s_mockInfoSuppressed{ 0 };
static std::atomic<uint32_t> s_mockQuestVanillaCalls{ 0 };
static std::atomic<uint32_t> s_mockInfoVanillaCalls{ 0 };

static bool MockVanilla_HaveNPCsAllRequiredItems(void* /*pThis*/, const void* /*p1*/, const void* /*p2*/) {
    s_mockQuestVanillaCalls.fetch_add(1, std::memory_order_relaxed);
    return true;
}

static bool MockVanilla_HasImportantInfo(void* /*pThis*/, void* /*pEntity*/) {
    s_mockInfoVanillaCalls.fetch_add(1, std::memory_order_relaxed);
    return true;
}

static bool MockHooked_HaveNPCsAllRequiredItems(void* pThis, const void* p1, const void* p2) {
    if (s_mockLoadActive.load(std::memory_order_relaxed)) {
        s_mockQuestSuppressed.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return MockVanilla_HaveNPCsAllRequiredItems(pThis, p1, p2);
}

static bool MockHooked_HasImportantInfo(void* pThis, void* pEntity) {
    if (s_mockLoadActive.load(std::memory_order_relaxed)) {
        s_mockInfoSuppressed.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return MockVanilla_HasImportantInfo(pThis, pEntity);
}

static bool Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        return false;
    }
    return true;
}

int main() {
    bool ok = true;
    std::cout << "[Test] Quest Load Optimizer Policy Suite starting...\n";

    // 1. Loading Phase: queries should be suppressed, returning false
    s_mockLoadActive.store(true);
    for (int i = 0; i < 50; ++i) {
        bool resQ = MockHooked_HaveNPCsAllRequiredItems(nullptr, nullptr, nullptr);
        bool resI = MockHooked_HasImportantInfo(nullptr, nullptr);
        ok &= Expect(resQ == false, "HaveNPCsAllRequiredItems must return false during load");
        ok &= Expect(resI == false, "HasImportantInfo must return false during load");
    }
    ok &= Expect(s_mockQuestSuppressed.load() == 50, "50 quest checks suppressed");
    ok &= Expect(s_mockInfoSuppressed.load() == 50, "50 info checks suppressed");
    ok &= Expect(s_mockQuestVanillaCalls.load() == 0, "No vanilla quest calls during load");
    ok &= Expect(s_mockInfoVanillaCalls.load() == 0, "No vanilla info calls during load");

    // 2. Gameplay Phase: queries execute 100% vanilla
    s_mockLoadActive.store(false);
    for (int i = 0; i < 25; ++i) {
        bool resQ = MockHooked_HaveNPCsAllRequiredItems(nullptr, nullptr, nullptr);
        bool resI = MockHooked_HasImportantInfo(nullptr, nullptr);
        ok &= Expect(resQ == true, "HaveNPCsAllRequiredItems executes vanilla during gameplay");
        ok &= Expect(resI == true, "HasImportantInfo executes vanilla during gameplay");
    }
    ok &= Expect(s_mockQuestVanillaCalls.load() == 25, "25 vanilla quest calls during gameplay");
    ok &= Expect(s_mockInfoVanillaCalls.load() == 25, "25 vanilla info calls during gameplay");

    if (ok) {
        std::cout << "[Test] Quest Load Optimizer Policy Suite PASSED!\n";
        return 0;
    }
    return 1;
}
