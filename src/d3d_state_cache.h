#pragma once

#include <windows.h>
#include <d3d9.h>

namespace G3O {

inline bool CanUseSingleThreadedD3D9StateCache(HRESULT creationParametersResult,
                                               DWORD behaviorFlags) noexcept {
    return SUCCEEDED(creationParametersResult) &&
           (behaviorFlags & D3DCREATE_MULTITHREADED) == 0;
}

template <typename Device, typename Invalidate>
void PrepareD3DStateCacheForDevice(Device device, Device& cachedDevice,
                                   Invalidate&& invalidate) noexcept(noexcept(invalidate())) {
    if (cachedDevice == device) return;
    invalidate();
    cachedDevice = device;
}

template <typename Device, typename Invalidate>
void ResetD3DStateCacheForNewDevice(Device device, Device& cachedDevice,
                                    Invalidate&& invalidate) noexcept(noexcept(invalidate())) {
    invalidate();
    cachedDevice = device;
}

template <typename Invoke>
HRESULT ApplyCachedD3DState(DWORD value, DWORD& cachedValue, BYTE& valid,
                            Invoke&& invoke, bool bypassCache = false) noexcept(noexcept(invoke())) {
    if (bypassCache) {
        valid = 0;
        return invoke();
    }
    if (valid && cachedValue == value) return D3D_OK;

    const HRESULT result = invoke();
    if (SUCCEEDED(result)) {
        cachedValue = value;
        valid = 1;
    }
    return result;
}

template <typename Invoke, typename Invalidate>
HRESULT ApplyD3DStateBlockAndInvalidateCache(Invoke&& invoke, Invalidate&& invalidate,
                                              bool& stateBlockApplyInProgress)
    noexcept(noexcept(invoke()) && noexcept(invalidate())) {
    const bool wasApplying = stateBlockApplyInProgress;
    stateBlockApplyInProgress = true;
    invalidate();
    const HRESULT result = invoke();
    stateBlockApplyInProgress = wasApplying;
    invalidate();
    return result;
}

} // namespace G3O
