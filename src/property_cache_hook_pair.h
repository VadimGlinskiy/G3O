#pragma once

namespace G3O {

template <typename Create, typename Enable, typename Disable, typename Remove>
bool InstallHookPairTransactional(
    void* first,
    void* second,
    Create&& create,
    Enable&& enable,
    Disable&& disable,
    Remove&& remove) {
    if (!first || !second || first == second) {
        return false;
    }

    if (!create(first)) {
        return false;
    }
    if (!create(second)) {
        remove(first);
        return false;
    }
    if (!enable(first)) {
        disable(first);
        remove(second);
        remove(first);
        return false;
    }
    if (!enable(second)) {
        disable(first);
        disable(second);
        remove(second);
        remove(first);
        return false;
    }

    return true;
}

} // namespace G3O
