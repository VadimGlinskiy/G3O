#pragma once

namespace G3O {

template <typename OriginalDiagOutput>
inline void ForwardDiagOutput(OriginalDiagOutput original, const char* message,
                              bool& forwardingOriginal) {
    if (!original || !message) return;
    const bool wasForwarding = forwardingOriginal;
    forwardingOriginal = true;
    original("%s", message);
    forwardingOriginal = wasForwarding;
}

template <typename OriginalOutputDebugString>
inline bool ForwardNestedDiagOutputDebugString(bool forwardingOriginal,
                                               OriginalOutputDebugString original,
                                               const char* message,
                                               bool debuggerAttached = true) {
    if (!forwardingOriginal) return false;
    if (original && message && debuggerAttached) original(message);
    return true;
}

} // namespace G3O
