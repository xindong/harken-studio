// Platform backend selection gate. The real-device backend list is chosen at
// compile time per platform (spec §7.1); CI runs on the null backend and never
// opens a device, so this proves the selection itself rather than any hardware:
// the Windows build still selects WASAPI (and no platform-branch backend is
// miscompiled into it), and no shipping platform silently falls back to the
// null backend. These runtime checks mirror the compile-time static_asserts in
// PlatformBackend.h so a broken selection is visible as a red test as well as
// a failed build.
#include "PlatformBackend.h"

#include <gtest/gtest.h>

namespace {

using harken::kPlatformBackendCount;
using harken::kPlatformBackends;
using harken::PlatformAudioBackend;

TEST(PlatformBackend, SelectsThePlatformDefaultBackend) {
#if defined(_WIN32)
    ASSERT_EQ(1u, kPlatformBackendCount);
    EXPECT_EQ(PlatformAudioBackend::Wasapi, kPlatformBackends[0]);
#elif defined(__APPLE__)
    ASSERT_EQ(1u, kPlatformBackendCount);
    EXPECT_EQ(PlatformAudioBackend::CoreAudio, kPlatformBackends[0]);
#elif defined(__ANDROID__)
    ASSERT_EQ(2u, kPlatformBackendCount);
    EXPECT_EQ(PlatformAudioBackend::AAudio, kPlatformBackends[0]);
    EXPECT_EQ(PlatformAudioBackend::OpenSL, kPlatformBackends[1]);
#else
    // Linux and other desktops are OUT of M5: no real backend is enabled, so a
    // real-device request fails at Vn_Init rather than opening miniaudio's null.
    EXPECT_EQ(0u, kPlatformBackendCount);
#endif
}

TEST(PlatformBackend, ShippingPlatformNamesConcreteBackendOutPlatformNamesNone) {
#if defined(_WIN32) || defined(__APPLE__) || defined(__ANDROID__)
    // A target platform always names a concrete device backend first; the null
    // backend is never a member of the list — it is a separate device mode, not
    // a silent real-device fallback.
    ASSERT_GT(kPlatformBackendCount, 0u);
    for (std::size_t i = 0; i < kPlatformBackendCount; ++i) {
        EXPECT_NE(PlatformAudioBackend::Null, kPlatformBackends[i]);
    }
#else
    // An OUT platform lists no backend at all.
    EXPECT_EQ(0u, kPlatformBackendCount);
#endif
}

} // namespace
