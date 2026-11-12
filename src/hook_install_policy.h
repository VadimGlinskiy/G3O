#pragma once

namespace G3O {

enum class HookInstallResult {
    CreateFailed,
    EnableFailed,
    Installed
};

template <typename Create, typename Enable, typename Remove, typename ClearOriginal>
HookInstallResult InstallHookSafely(
    Create create, Enable enable, Remove remove, ClearOriginal clearOriginal) {
    if (!create()) {
        clearOriginal();
        return HookInstallResult::CreateFailed;
    }
    if (!enable()) {
        remove();
        clearOriginal();
        return HookInstallResult::EnableFailed;
    }
    return HookInstallResult::Installed;
}

} // namespace G3O
