#include "d3d_state_cache.h"
#include <cstdio>

static bool Expect(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

int main() {
    DWORD cached = 0;
    BYTE valid = 0;
    int calls = 0;
    bool ok = true;

    HRESULT result = G3O::ApplyCachedD3DState(7, cached, valid, [&]() {
        ++calls;
        return E_FAIL;
    });
    ok &= Expect(result == E_FAIL && valid == 0 && cached == 0,
                 "failed Direct3D call must not populate the cache");

    result = G3O::ApplyCachedD3DState(7, cached, valid, [&]() {
        ++calls;
        return D3D_OK;
    });
    ok &= Expect(result == D3D_OK && valid == 1 && cached == 7,
                 "successful Direct3D call should populate the cache");

    result = G3O::ApplyCachedD3DState(7, cached, valid, [&]() {
        ++calls;
        return E_FAIL;
    });
    ok &= Expect(result == D3D_OK && calls == 2,
                 "repeating a successfully applied state should skip the Direct3D call");

    result = G3O::ApplyCachedD3DState(8, cached, valid, [&]() {
        ++calls;
        return E_FAIL;
    });
    ok &= Expect(result == E_FAIL && cached == 7,
                 "failed state change must preserve the last successful cache value");
    result = G3O::ApplyCachedD3DState(8, cached, valid, [&]() {
        ++calls;
        return D3D_OK;
    });
    ok &= Expect(result == D3D_OK && cached == 8 && calls == 4,
                 "failed state change should be retried until it succeeds");

    int firstDeviceStorage = 0;
    int secondDeviceStorage = 0;
    void* firstDevice = &firstDeviceStorage;
    void* secondDevice = &secondDeviceStorage;
    void* cacheDevice = nullptr;
    DWORD perDeviceValue = 0;
    BYTE perDeviceValid = 0;
    BYTE otherStateValid = 1;
    int invalidations = 0;
    int deviceCalls = 0;
    auto invalidateAll = [&] {
        perDeviceValid = 0;
        otherStateValid = 0;
        ++invalidations;
    };
    G3O::PrepareD3DStateCacheForDevice(firstDevice, cacheDevice, invalidateAll);
    perDeviceValid = 1;
    otherStateValid = 1;
    G3O::PrepareD3DStateCacheForDevice(firstDevice, cacheDevice, invalidateAll);
    ok &= Expect(invalidations == 1 && otherStateValid == 1,
                 "reusing one device should retain its cached state");
    G3O::PrepareD3DStateCacheForDevice(secondDevice, cacheDevice, invalidateAll);
    ok &= Expect(invalidations == 2 && perDeviceValid == 0 && otherStateValid == 0,
                 "switching devices should invalidate every cached state");
    result = G3O::ApplyCachedD3DState(7, perDeviceValue, perDeviceValid, [&]() {
        ++deviceCalls;
        return D3D_OK;
    });
    ok &= Expect(result == D3D_OK && deviceCalls == 1,
                 "a second device must not reuse the first device's state cache");

    perDeviceValid = 1;
    otherStateValid = 1;
    G3O::ResetD3DStateCacheForNewDevice(secondDevice, cacheDevice, invalidateAll);
    ok &= Expect(invalidations == 3 && perDeviceValid == 0 && otherStateValid == 0,
                 "constructing a new device at a reused address must clear the old cache");

    if (!ok) return 1;
    std::puts("PASS: D3D9 state cache records only successful API calls");
    return 0;
}
