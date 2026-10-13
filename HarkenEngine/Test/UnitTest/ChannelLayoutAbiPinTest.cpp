// ABI pin for the multichannel-output amendment (spec M5 §3 machine proof).
// The amendment is append-only: the VnChannelLayout enum and VN_UNSUPPORTED_
// LAYOUT take fixed values, the new VnConfig / VnSnapshot fields are appended
// strictly after the M4-close tail (never reordered or inserted mid-struct),
// and VnDownmixMatrixDesc has the pinned packing. Everything here is a
// compile-time assertion; the runtime test exists so the pin participates in
// discovery and reporting. A renumber, a mid-struct insertion, or a packing
// change fails compilation.
#include <harken/Harken.h>

#include "harken/core/ChannelLayout.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

// Channel-layout enum values are fixed (Appendix A) and never renumbered.
static_assert(VN_CHANNEL_LAYOUT_DEFAULT == 0, "layout value must not renumber");
static_assert(VN_CHANNEL_LAYOUT_MONO == 1, "layout value must not renumber");
static_assert(VN_CHANNEL_LAYOUT_STEREO == 2, "layout value must not renumber");
static_assert(VN_CHANNEL_LAYOUT_QUAD == 3, "layout value must not renumber");
static_assert(VN_CHANNEL_LAYOUT_5_1 == 4, "layout value must not renumber");
static_assert(VN_CHANNEL_LAYOUT_7_1 == 5, "layout value must not renumber");

// The new error code keeps its value in the -3xx state band.
static_assert(VN_UNSUPPORTED_LAYOUT == -319,
              "new error code must keep its value");

// VnConfig: the multichannel fields are appended immediately after the M4-close
// tail (reserved5), contiguous and in order, with no unexpected tail padding —
// a mid-struct insertion or a reorder moves these offsets and fails here.
static_assert(offsetof(VnConfig, structSize) == 0,
              "structSize must remain the first field");
static_assert(offsetof(VnConfig, mixChannelLayout) ==
                  offsetof(VnConfig, reserved5) + sizeof(std::uint32_t),
              "the multichannel fields must append right after reserved5");
static_assert(offsetof(VnConfig, mixChannelLayout) ==
                  offsetof(VnConfig, maxTriggerDepth) +
                      2 * sizeof(std::uint32_t),
              "maxTriggerDepth, then reserved5, then the multichannel fields");
static_assert(offsetof(VnConfig, deviceChannelLayout) ==
                  offsetof(VnConfig, mixChannelLayout) + sizeof(std::uint32_t),
              "deviceChannelLayout follows mixChannelLayout");
static_assert(offsetof(VnConfig, renderWorkerCount) ==
                  offsetof(VnConfig, mixChannelLayout) +
                      2 * sizeof(std::uint32_t),
              "renderWorkerCount follows deviceChannelLayout");
static_assert(offsetof(VnConfig, reserved6) ==
                  offsetof(VnConfig, mixChannelLayout) +
                      3 * sizeof(std::uint32_t),
              "reserved6 is the last multichannel field");
static_assert(offsetof(VnConfig, maxBindings) ==
                  offsetof(VnConfig, reserved6) + sizeof(std::uint32_t),
              "later vintages append strictly after reserved6, so the "
              "multichannel fields keep their exact offsets");

// VnSnapshot: same discipline — the multichannel/job-parallel fields append
// right after the M4-close tail (reserved2), contiguous, no extra tail padding.
static_assert(offsetof(VnSnapshot, structSize) == 0,
              "structSize must remain the first field");
static_assert(offsetof(VnSnapshot, mixChannelCount) ==
                  offsetof(VnSnapshot, reserved2) + sizeof(std::uint32_t),
              "the multichannel fields must append right after reserved2");
static_assert(offsetof(VnSnapshot, deviceChannelCount) ==
                  offsetof(VnSnapshot, mixChannelCount) + sizeof(std::uint32_t),
              "deviceChannelCount follows mixChannelCount");
static_assert(offsetof(VnSnapshot, outputDownmixActive) ==
                  offsetof(VnSnapshot, mixChannelCount) +
                      2 * sizeof(std::uint32_t),
              "outputDownmixActive follows deviceChannelCount");
static_assert(offsetof(VnSnapshot, renderWorkerCount) ==
                  offsetof(VnSnapshot, mixChannelCount) +
                      3 * sizeof(std::uint32_t),
              "renderWorkerCount follows outputDownmixActive");
static_assert(offsetof(VnSnapshot, renderWorkerBusyPeakUs) ==
                  offsetof(VnSnapshot, mixChannelCount) +
                      4 * sizeof(std::uint32_t),
              "renderWorkerBusyPeakUs follows renderWorkerCount");
static_assert(offsetof(VnSnapshot, reserved3) ==
                  offsetof(VnSnapshot, mixChannelCount) +
                      5 * sizeof(std::uint32_t),
              "reserved3 is the last multichannel field");
// The property/binding observability tail appends right after reserved3
// (a later vintage; the multichannel fields keep their exact offsets).
static_assert(offsetof(VnSnapshot, bindingDefinitionHighWater) ==
                  offsetof(VnSnapshot, reserved3) + sizeof(std::uint32_t),
              "the binding observability tail appends after reserved3");
static_assert(offsetof(VnSnapshot, bindingDefinitionHighWater) == 2680,
              "the pre-binding-observability vintage boundary is pinned");
// The bus audibility masks append right after the effect-stats table (a
// later vintage; every field above keeps its exact offset).
static_assert(offsetof(VnSnapshot, busMuteMask) ==
                  offsetof(VnSnapshot, effectTypeStats) +
                      sizeof(VnEffectTypeStats) * VN_MAX_EFFECT_TYPE_STATS,
              "the bus audibility masks append after the effect-stats table");
static_assert(offsetof(VnSnapshot, busSoloEffectiveMask) ==
                  offsetof(VnSnapshot, busMuteMask) +
                      sizeof(std::uint32_t),
              "busSoloEffectiveMask follows busMuteMask");
static_assert(offsetof(VnSnapshot, actionEmitterMultichannelFallbackCount) ==
                  offsetof(VnSnapshot, busSoloEffectiveMask) +
                      sizeof(std::uint32_t),
              "the Action demotion counter appends after bus audibility masks");
static_assert(offsetof(VnSnapshot, busPeaksPreFader) == 3160,
              "the Pre Fader peaks append after the Action demotion counter");
static_assert(offsetof(VnSnapshot, busPeaksPostFader) == 3288,
              "the Post Fader peaks append after the Pre Fader peaks");
static_assert(offsetof(VnSnapshot, dynamicRouteCurrent) == 3416,
              "the Dynamic Route aggregate appends after Bus meter peaks");
static_assert(offsetof(VnSnapshot, dynamicRoutePlanCurrent) == 3428);
static_assert(offsetof(VnSnapshot, dynamicBusTapCurrent) == 3440);
static_assert(offsetof(VnSnapshot, dynamicVoiceTapCurrent) == 3452);
static_assert(offsetof(VnSnapshot, dynamicRouteRefusalCount) == 3464);
static_assert(offsetof(VnSnapshot, dynamicRouteCycleCount) == 3472);
static_assert(offsetof(VnSnapshot, dynamicRouteOrderRefusalCount) == 3480);
static_assert(offsetof(VnSnapshot, dynamicRouteStaleReadCount) == 3488);
static_assert(offsetof(VnSnapshot, dynamicTapGenerationMismatchCount) == 3496);
static_assert(offsetof(VnSnapshot, dynamicPlanPublicationCount) == 3504);
static_assert(offsetof(VnSnapshot, dynamicPlanRetireOverflowCount) == 3512);
static_assert(offsetof(VnSnapshot, compressorDetectorNonFiniteCount) == 3520);
static_assert(offsetof(VnSnapshot, compressorDetectorSaturationCount) == 3528);
static_assert(offsetof(VnSnapshot, activeBusCount) == 3536);

// VnDownmixMatrixDesc: authoritative packing — a u32 header then the fixed 8x8
// coefficient block, coeffs[r*8 + c] the gain from src c into dst r.
static_assert(offsetof(VnDownmixMatrixDesc, structSize) == 0,
              "structSize first");
static_assert(offsetof(VnDownmixMatrixDesc, srcLayout) == 4, "srcLayout at 4");
static_assert(offsetof(VnDownmixMatrixDesc, dstLayout) == 8, "dstLayout at 8");
static_assert(offsetof(VnDownmixMatrixDesc, reserved0) == 12,
              "reserved0 at 12");
static_assert(offsetof(VnDownmixMatrixDesc, coeffs) == 16,
              "coeffs at 16 (after the 16-byte header)");
static_assert(sizeof(VnDownmixMatrixDesc) == 16 + 64 * sizeof(float),
              "desc is a 16-byte header plus the fixed 8x8 float table");

TEST(ChannelLayoutAbiPin, ResolvedPanMatchesFormerStereoFormulaBitExactly) {
    const float pans[] = {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f};
    for (float pan : pans) {
        const float theta =
            (pan + 1.0f) * 0.25f * 3.14159265358979323846f;
        const harken::ResolvedPanGains resolved =
            harken::ResolvePanGains(VN_CHANNEL_LAYOUT_STEREO, pan);
        EXPECT_FLOAT_EQ(pan, resolved.pan);
        EXPECT_FLOAT_EQ(std::cos(theta), resolved.gain0);
        EXPECT_FLOAT_EQ(std::sin(theta), resolved.gain1);
        EXPECT_EQ(0u, resolved.channel0);
        EXPECT_EQ(1u, resolved.channel1);
    }
}

TEST(ChannelLayoutAbiPin, ResolvedPanMatchesFormerWiderFormulaBitExactly) {
    const float pans[] = {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f};
    for (float pan : pans) {
        const float scaled = (pan + 1.0f);
        std::uint32_t segment = static_cast<std::uint32_t>(scaled);
        if (segment > 1u) {
            segment = 1u;
        }
        const float q = scaled - static_cast<float>(segment);
        const float theta = q * 0.5f * 3.14159265358979323846f;
        const std::uint32_t front[] = {0u, 2u, 1u};
        const harken::ResolvedPanGains resolved =
            harken::ResolvePanGains(VN_CHANNEL_LAYOUT_5_1, pan);
        EXPECT_FLOAT_EQ(pan, resolved.pan);
        EXPECT_FLOAT_EQ(std::cos(theta), resolved.gain0);
        EXPECT_FLOAT_EQ(std::sin(theta), resolved.gain1);
        EXPECT_EQ(front[segment], resolved.channel0);
        EXPECT_EQ(front[segment + 1], resolved.channel1);
    }
}

TEST(ChannelLayoutAbiPin, ResolvedPanSanitizesNonFiniteAndOutOfRangeInputs) {
    struct Case {
        float input;
        float expectedPan;
    };
    const Case cases[] = {
        {std::numeric_limits<float>::quiet_NaN(), 0.0f},
        {std::numeric_limits<float>::infinity(), 1.0f},
        {-std::numeric_limits<float>::infinity(), -1.0f},
        {5.1f, 1.0f},
        {-5.1f, -1.0f},
    };

    for (const Case& test : cases) {
        const float theta =
            (test.expectedPan + 1.0f) * 0.25f * 3.14159265358979323846f;
        const harken::ResolvedPanGains resolved =
            harken::ResolvePanGains(VN_CHANNEL_LAYOUT_STEREO, test.input);
        EXPECT_FLOAT_EQ(test.expectedPan, resolved.pan);
        EXPECT_FLOAT_EQ(std::cos(theta), resolved.gain0);
        EXPECT_FLOAT_EQ(std::sin(theta), resolved.gain1);
        EXPECT_EQ(0u, resolved.channel0);
        EXPECT_EQ(1u, resolved.channel1);
    }
}
