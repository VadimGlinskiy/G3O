#pragma once

#include <windows.h>
#include <d3d9.h>

namespace G3O {

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
                            Invoke&& invoke) noexcept(noexcept(invoke())) {
    if (valid && cachedValue == value) return D3D_OK;

    const HRESULT result = invoke();
    if (SUCCEEDED(result)) {
        cachedValue = value;
        valid = 1;
    }
    return result;
}

} // namespace G3O
