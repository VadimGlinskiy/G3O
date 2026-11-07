#include "property_cache_hook_pair.h"
#include <cstdint>
#include <cstdio>
#include <vector>

static bool Expect(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

struct FakeHooks {
    int failCreate = 0;
    int failEnable = 0;
    std::vector<int> events;

    bool Create(void* target) {
        const int id = static_cast<int>(reinterpret_cast<std::uintptr_t>(target));
        events.push_back(10 + id);
        return id != failCreate;
    }
    bool Enable(void* target) {
        const int id = static_cast<int>(reinterpret_cast<std::uintptr_t>(target));
        events.push_back(20 + id);
        return id != failEnable;
    }
    void Disable(void* target) {
        events.push_back(30 + static_cast<int>(reinterpret_cast<std::uintptr_t>(target)));
    }
    void Remove(void* target) {
        events.push_back(40 + static_cast<int>(reinterpret_cast<std::uintptr_t>(target)));
    }
};

int main() {
    bool ok = true;
    void* first = reinterpret_cast<void*>(1);
    void* second = reinterpret_cast<void*>(2);

    FakeHooks hooks;
    ok &= Expect(G3O::InstallHookPairTransactional(first, second,
                  [&](void* p) { return hooks.Create(p); },
                  [&](void* p) { return hooks.Enable(p); },
                  [&](void* p) { hooks.Disable(p); },
                  [&](void* p) { hooks.Remove(p); }),
                 "both hooks should install and enable together");
    ok &= Expect(hooks.events == std::vector<int>({11, 12, 21, 22}),
                 "successful setup should create both before enabling either");

    hooks = FakeHooks{};
    hooks.failCreate = 2;
    ok &= Expect(!G3O::InstallHookPairTransactional(first, second,
                  [&](void* p) { return hooks.Create(p); },
                  [&](void* p) { return hooks.Enable(p); },
                  [&](void* p) { hooks.Disable(p); },
                  [&](void* p) { hooks.Remove(p); }),
                 "a second-hook creation failure must fail the pair");
    ok &= Expect(hooks.events == std::vector<int>({11, 12, 41}),
                 "a failed second create must remove only the first hook created here");

    hooks = FakeHooks{};
    hooks.failEnable = 2;
    ok &= Expect(!G3O::InstallHookPairTransactional(first, second,
                  [&](void* p) { return hooks.Create(p); },
                  [&](void* p) { return hooks.Enable(p); },
                  [&](void* p) { hooks.Disable(p); },
                  [&](void* p) { hooks.Remove(p); }),
                 "a second-hook enable failure must fail the pair");
    ok &= Expect(hooks.events == std::vector<int>({11, 12, 21, 22, 31, 32, 42, 41}),
                 "an enable failure must disable and remove both created hooks");

    hooks = FakeHooks{};
    hooks.failCreate = 1;
    ok &= Expect(!G3O::InstallHookPairTransactional(first, second,
                  [&](void* p) { return hooks.Create(p); },
                  [&](void* p) { return hooks.Enable(p); },
                  [&](void* p) { hooks.Disable(p); },
                  [&](void* p) { hooks.Remove(p); }),
                 "a first-hook creation failure must reject the pair");
    ok &= Expect(hooks.events == std::vector<int>({11}),
                 "a failed create must not remove a hook not owned by this attempt");
    return ok ? 0 : 1;
}
