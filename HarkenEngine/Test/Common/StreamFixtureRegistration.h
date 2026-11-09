#pragma once

#include "Engine.h"
#include "HarkenApiInternal.h"

namespace harkentest {

// Legacy file fixtures exercise the byte supply and renderer independently of
// Pack authoring. This test seam is not a host registration surface.
inline VnResult RegisterFixtureStream(VnSoundId id, const char* path) {
    auto* engine = harken::detail::ActiveEngine();
    if (!engine || engine->callbackFatalSticky) return VN_NOT_INITIALIZED;
    if (harken::detail::CanonicalSetMutationBlocked()) return VN_IN_USE;
    if (!path) return VN_INVALID_ARG;
    if (engine->registry.CookieOf(id)) return VN_IN_USE;
    return engine->streams.RegisterFile(id, path);
}
inline VnResult ReplaceFixtureStream(VnSoundId id, const char* path) {
    auto* engine = harken::detail::ActiveEngine();
    if (!engine || engine->callbackFatalSticky) return VN_NOT_INITIALIZED;
    if (harken::detail::CanonicalSetMutationBlocked()) return VN_IN_USE;
    if (!path) return VN_INVALID_ARG;
    return engine->streams.ReplaceFile(id, path);
}
inline VnResult UnregisterFixtureStream(VnSoundId id) {
    auto* engine = harken::detail::ActiveEngine();
    if (!engine || engine->callbackFatalSticky) return VN_NOT_INITIALIZED;
    if (harken::detail::CanonicalSetMutationBlocked()) return VN_IN_USE;
    return engine->streams.UnregisterFile(id);
}

} // namespace harkentest
