#include <harken/Harken.h>

#include "Common/TestUtil.h"
#include "harken/core/LogRing.h"
#include "harken/render/BuiltinEffects.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

static_assert(sizeof(VnEffectCapability) == 32 &&
                  offsetof(VnEffectCapability, effectType) == 4 &&
                  offsetof(VnEffectCapability, descriptorVintage) == 12 &&
                  offsetof(VnEffectCapability, hostScopeMask) == 20,
              "VnEffectCapability row shape is frozen");
static_assert(sizeof(VnEffectParameterCapability) == 40 &&
                  offsetof(VnEffectParameterCapability, parameterIndex) == 8 &&
                  offsetof(VnEffectParameterCapability, flags) == 20 &&
                  offsetof(VnEffectParameterCapability, clampMax) == 32,
              "VnEffectParameterCapability row shape is frozen");
static_assert(sizeof(VnEffectDynamicCapability) == 88 &&
                  alignof(VnEffectDynamicCapability) == 4 &&
                  offsetof(VnEffectDynamicCapability, effectType) == 8 &&
                  offsetof(VnEffectDynamicCapability, flags) == 56 &&
                  offsetof(VnEffectDynamicCapability, reserved1) == 84,
              "VnEffectDynamicCapability row shape is frozen");
static_assert(sizeof(VnEffectDynamicParameterCapability) == 40 &&
                  offsetof(VnEffectDynamicParameterCapability,
                           parameterIndex) == 12 &&
                  offsetof(VnEffectDynamicParameterCapability,
                           smoothingFrames) == 32,
              "VnEffectDynamicParameterCapability row shape is frozen");

namespace {

TEST(CapabilityDiscovery, RequiresAnInitializedEngine) {
    std::uint32_t required = 0;
    EXPECT_EQ(VN_NOT_INITIALIZED,
              Vn_CaptureEffectCapabilities(nullptr, 0, &required));
    EXPECT_EQ(VN_NOT_INITIALIZED,
              Vn_CaptureEffectParameterCapabilities(nullptr, 0, &required));
}

TEST(CapabilityDiscovery, EffectRowsComeFromTheGeneratedRegistry) {
    harkentest::EngineSession session(
        harkentest::MakeNullConfig(4, 256, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());

    std::uint32_t required = 0;
    ASSERT_EQ(VN_OK,
              Vn_CaptureEffectCapabilities(nullptr, 0, &required));
    ASSERT_EQ(5u, required);
    std::vector<VnEffectCapability> rows(required);
    rows[0].structSize = sizeof(VnEffectCapability);
    ASSERT_EQ(VN_OK, Vn_CaptureEffectCapabilities(
                         rows.data(), static_cast<std::uint32_t>(rows.size()),
                         &required));

    ASSERT_EQ(5u, rows.size());
    EXPECT_EQ(VN_EFFECT_FEEDBACK_DELAY, rows[0].effectType);
    EXPECT_EQ(sizeof(VnFeedbackDelayDesc), rows[0].descriptorSize);
    EXPECT_EQ(VN_EFFECT_THREE_BAND_TONE, rows[1].effectType);
    EXPECT_EQ(sizeof(VnThreeBandToneDesc), rows[1].descriptorSize);
    EXPECT_EQ(VN_EFFECT_GAIN, rows[2].effectType);
    EXPECT_EQ(sizeof(VnGainDesc), rows[2].descriptorSize);
    EXPECT_EQ(VN_EFFECT_LIMITER, rows[3].effectType);
    EXPECT_EQ(sizeof(VnLimiterDesc), rows[3].descriptorSize);
    EXPECT_EQ(VN_EFFECT_COMPRESSOR, rows[4].effectType);
    EXPECT_EQ(sizeof(VnCompressorDesc), rows[4].descriptorSize);
    for (const VnEffectCapability& row : rows) {
        EXPECT_EQ(sizeof(VnEffectCapability), row.structSize);
        EXPECT_EQ(2u, row.descriptorVintage);
        EXPECT_EQ(VN_EFFECT_HOST_SCOPE_VOICE | VN_EFFECT_HOST_SCOPE_BUS,
                  row.hostScopeMask);
        EXPECT_EQ(0u, row.reserved0);
        EXPECT_EQ(0u, row.reserved1);
    }
    EXPECT_EQ(4u, rows[0].parameterCount);
    EXPECT_EQ(7u, rows[1].parameterCount);
    EXPECT_EQ(1u, rows[2].parameterCount);
    EXPECT_EQ(2u, rows[3].parameterCount);
    EXPECT_EQ(6u, rows[4].parameterCount);
}

TEST(CapabilityDiscovery, ParameterRowsResolveEngineDependentBounds) {
    harkentest::EngineSession session(
        harkentest::MakeNullConfig(4, 256, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());

    std::uint32_t required = 0;
    ASSERT_EQ(VN_OK, Vn_CaptureEffectParameterCapabilities(
                         nullptr, 0, &required));
    ASSERT_EQ(20u, required);
    std::vector<VnEffectParameterCapability> rows(required);
    rows[0].structSize = sizeof(VnEffectParameterCapability);
    ASSERT_EQ(VN_OK, Vn_CaptureEffectParameterCapabilities(
                         rows.data(), static_cast<std::uint32_t>(rows.size()),
                         &required));

    const VnEffectParameterCapability& delayTime = rows[0];
    EXPECT_EQ(VN_EFFECT_FEEDBACK_DELAY, delayTime.effectType);
    EXPECT_EQ(harken::kDelayParamTimeMs, delayTime.parameterIndex);
    EXPECT_EQ(std::numeric_limits<float>::denorm_min(), delayTime.clampMin);
    EXPECT_FLOAT_EQ(1000.0f, delayTime.clampMax);

    const VnEffectParameterCapability& delayFeedback = rows[1];
    EXPECT_EQ(VN_EFFECT_FEEDBACK_DELAY, delayFeedback.effectType);
    EXPECT_EQ(harken::kDelayParamFeedback, delayFeedback.parameterIndex);
    EXPECT_FLOAT_EQ(0.0f, delayFeedback.clampMin);
    EXPECT_EQ(std::nextafter(1.0f, 0.0f), delayFeedback.clampMax);

    const VnEffectParameterCapability& toneFrequency = rows[5];
    EXPECT_EQ(VN_EFFECT_THREE_BAND_TONE, toneFrequency.effectType);
    EXPECT_EQ(1u, toneFrequency.parameterIndex);
    EXPECT_EQ(VN_EFFECT_PARAMETER_UNIT_HZ, toneFrequency.unit);
    EXPECT_EQ(VN_EFFECT_PARAMETER_COMBINE_ADD_LOG_HZ,
              toneFrequency.combine);
    EXPECT_EQ(VN_EFFECT_PARAMETER_BINDABLE |
                  VN_EFFECT_PARAMETER_SUPPORTS_TRANSITION,
              toneFrequency.flags);
    EXPECT_EQ(std::numeric_limits<float>::denorm_min(),
              toneFrequency.clampMin);
    EXPECT_EQ(std::nextafter(24000.0f, 0.0f),
              toneFrequency.clampMax);

    const VnEffectParameterCapability& gain = rows[11];
    EXPECT_EQ(VN_EFFECT_GAIN, gain.effectType);
    EXPECT_EQ(0u, gain.parameterIndex);
    EXPECT_EQ(VN_EFFECT_PARAMETER_UNIT_DB, gain.unit);
    EXPECT_EQ(VN_EFFECT_PARAMETER_COMBINE_ADD_DB, gain.combine);
    EXPECT_EQ(VN_EFFECT_PARAMETER_BINDABLE |
                  VN_EFFECT_PARAMETER_SUPPORTS_TRANSITION,
              gain.flags);
    EXPECT_FLOAT_EQ(-96.0f, gain.clampMin);
    EXPECT_FLOAT_EQ(24.0f, gain.clampMax);

    const VnEffectParameterCapability& release = rows[13];
    EXPECT_EQ(VN_EFFECT_LIMITER, release.effectType);
    EXPECT_EQ(1u, release.parameterIndex);
    EXPECT_EQ(VN_EFFECT_PARAMETER_UNIT_MS, release.unit);
    EXPECT_EQ(VN_EFFECT_PARAMETER_COMBINE_ADD_LOG_TIME, release.combine);
    EXPECT_EQ(VN_EFFECT_PARAMETER_BINDABLE |
                  VN_EFFECT_PARAMETER_SUPPORTS_TRANSITION,
              release.flags);
    EXPECT_FLOAT_EQ(1.0f, release.clampMin);
    EXPECT_FLOAT_EQ(1000.0f, release.clampMax);

    const VnEffectParameterCapability& ratio = rows[15];
    EXPECT_EQ(VN_EFFECT_COMPRESSOR, ratio.effectType);
    EXPECT_EQ(harken::kCompressorParamRatio, ratio.parameterIndex);
    EXPECT_EQ(VN_EFFECT_PARAMETER_UNIT_RATIO, ratio.unit);
    EXPECT_EQ(VN_EFFECT_PARAMETER_COMBINE_MULTIPLY, ratio.combine);
    EXPECT_EQ(VN_EFFECT_PARAMETER_BINDABLE |
                  VN_EFFECT_PARAMETER_SUPPORTS_TRANSITION,
              ratio.flags);
    EXPECT_FLOAT_EQ(1.0f, ratio.neutral);
    EXPECT_FLOAT_EQ(1.0f, ratio.clampMin);
    EXPECT_FLOAT_EQ(20.0f, ratio.clampMax);
}

TEST(CapabilityDiscovery, CallerBufferShapeIsFailClosedAndTruncatesHonestly) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    std::uint32_t required = 0;
    VnEffectCapability row{};
    row.structSize = sizeof(VnEffectCapability);
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_CaptureEffectCapabilities(&row, 1, nullptr));
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_CaptureEffectCapabilities(nullptr, 1, &required));
    row.structSize -= 4;
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_CaptureEffectCapabilities(&row, 1, &required));
    row.structSize = sizeof(VnEffectCapability);
    EXPECT_EQ(VN_OK,
              Vn_CaptureEffectCapabilities(&row, 1, &required));
    EXPECT_EQ(5u, required);
    EXPECT_EQ(VN_EFFECT_FEEDBACK_DELAY, row.effectType);
}

TEST(CapabilityDiscovery,
     EffectCapabilityTruncationCountsAndNamesOnlyRealShortBuffers) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    std::uint32_t required = 0;
    VnEffectCapability row{};
    row.structSize = sizeof(VnEffectCapability) - 4;
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_CaptureEffectCapabilities(&row, 1, &required));
    EXPECT_EQ(0u, engine->captureTruncationCount);

    ASSERT_EQ(VN_OK,
              Vn_CaptureEffectCapabilities(nullptr, 0, &required));
    ASSERT_EQ(5u, required);
    EXPECT_EQ(0u, engine->captureTruncationCount);

    std::vector<VnEffectCapability> exact(required);
    exact[0].structSize = sizeof(VnEffectCapability);
    ASSERT_EQ(VN_OK, Vn_CaptureEffectCapabilities(
                         exact.data(),
                         static_cast<std::uint32_t>(exact.size()), &required));
    EXPECT_EQ(5u, required);
    EXPECT_EQ(0u, engine->captureTruncationCount);
    harken::LogRecord record{};
    EXPECT_FALSE(harkentest::FindLogicLog(
        engine, harken::kLogCodeCaptureTruncated, &record));

    row = {};
    row.structSize = sizeof(VnEffectCapability);
    ASSERT_EQ(VN_OK,
              Vn_CaptureEffectCapabilities(&row, 1, &required));
    EXPECT_EQ(5u, required);
    EXPECT_EQ(1u, engine->captureTruncationCount);
    ASSERT_TRUE(harkentest::FindLogicLog(
        engine, harken::kLogCodeCaptureTruncated, &record));
    EXPECT_EQ(harken::kCaptureSurfaceEffectCapabilities, record.arg0);
    EXPECT_EQ(5u, record.arg1);
    EXPECT_FALSE(harkentest::FindLogicLog(
        engine, harken::kLogCodeCaptureTruncated, &record));
}

TEST(CapabilityDiscovery,
     EffectParameterCapabilityTruncationCountsAndNamesOnlyRealShortBuffers) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    std::uint32_t required = 0;
    VnEffectParameterCapability row{};
    row.structSize = sizeof(VnEffectParameterCapability) - 4;
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_CaptureEffectParameterCapabilities(&row, 1, &required));
    EXPECT_EQ(0u, engine->captureTruncationCount);

    ASSERT_EQ(VN_OK, Vn_CaptureEffectParameterCapabilities(
                         nullptr, 0, &required));
    ASSERT_EQ(20u, required);
    EXPECT_EQ(0u, engine->captureTruncationCount);

    std::vector<VnEffectParameterCapability> exact(required);
    exact[0].structSize = sizeof(VnEffectParameterCapability);
    ASSERT_EQ(VN_OK, Vn_CaptureEffectParameterCapabilities(
                         exact.data(),
                         static_cast<std::uint32_t>(exact.size()), &required));
    EXPECT_EQ(20u, required);
    EXPECT_EQ(0u, engine->captureTruncationCount);
    harken::LogRecord record{};
    EXPECT_FALSE(harkentest::FindLogicLog(
        engine, harken::kLogCodeCaptureTruncated, &record));

    row = {};
    row.structSize = sizeof(VnEffectParameterCapability);
    ASSERT_EQ(VN_OK, Vn_CaptureEffectParameterCapabilities(
                         &row, 1, &required));
    EXPECT_EQ(20u, required);
    EXPECT_EQ(1u, engine->captureTruncationCount);
    ASSERT_TRUE(harkentest::FindLogicLog(
        engine, harken::kLogCodeCaptureTruncated, &record));
    EXPECT_EQ(harken::kCaptureSurfaceEffectParameterCapabilities, record.arg0);
    EXPECT_EQ(20u, record.arg1);
    EXPECT_FALSE(harkentest::FindLogicLog(
        engine, harken::kLogCodeCaptureTruncated, &record));
}

TEST(CapabilityDiscovery,
     DynamicRowsExposeSharedControlAndCompressorRawAudioContracts) {
    VnConfig config = harkentest::MakeNullConfig(1, 64, 48000);
    config.dynamicRouteCapacity = 7;
    config.dynamicBusTapCapacity = 3;
    config.dynamicVoiceTapCapacity = 2;
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    std::uint32_t required = 0;
    ASSERT_EQ(VN_OK,
              Vn_CaptureEffectDynamicCapabilities(nullptr, 0, &required));
    ASSERT_EQ(5u, required);
    std::vector<VnEffectDynamicCapability> rows(required);
    rows[0].structSize = sizeof rows[0];
    ASSERT_EQ(VN_OK, Vn_CaptureEffectDynamicCapabilities(
                         rows.data(), static_cast<std::uint32_t>(rows.size()),
                         &required));
    for (std::uint32_t i = 0; i < rows.size(); ++i) {
        EXPECT_EQ(i, rows[i].effectType);
        EXPECT_NE(0u, rows[i].acceptedSignalFormsMask &
                          VN_DYNAMIC_SIGNAL_CONTROL);
        EXPECT_NE(0u, rows[i].allowedSourceKindsMask &
                          VN_DYNAMIC_SOURCE_VARIABLE);
    }
    const VnEffectDynamicCapability& compressor =
        rows[VN_EFFECT_COMPRESSOR];
    EXPECT_EQ(VN_DYNAMIC_SIGNAL_CONTROL | VN_DYNAMIC_SIGNAL_RAW_AUDIO,
              compressor.acceptedSignalFormsMask);
    EXPECT_EQ(4u, compressor.maxRawRoutesPerInstance);
    EXPECT_EQ(7u, compressor.configuredGlobalRouteCapacity);
    EXPECT_EQ(3u, compressor.configuredBusTapCapacity);
    EXPECT_EQ(2u, compressor.configuredVoiceTapCapacity);
    EXPECT_EQ(0u, compressor.rawBusLatencyBlocks);
    EXPECT_EQ(1u, compressor.rawVoiceLatencyBlocks);
    EXPECT_FLOAT_EQ(-96.0f, compressor.rawRouteLevelMinDb);
    EXPECT_FLOAT_EQ(24.0f, compressor.rawRouteLevelMaxDb);
    EXPECT_EQ(VN_DYNAMIC_RAW_COMBINE_MAX_MAGNITUDE,
              compressor.rawRouteCombine);
    EXPECT_EQ(VN_DYNAMIC_CHANNEL_REDUCTION_MAX_ABSOLUTE,
              compressor.rawChannelReduction);

    required = 0;
    ASSERT_EQ(VN_OK,
              Vn_CaptureEffectDynamicParameterCapabilities(
                  nullptr, 0, &required));
    ASSERT_EQ(18u, required);
    std::vector<VnEffectDynamicParameterCapability> params(required);
    params[0].structSize = sizeof params[0];
    ASSERT_EQ(VN_OK,
              Vn_CaptureEffectDynamicParameterCapabilities(
                  params.data(),
                  static_cast<std::uint32_t>(params.size()), &required));
    for (const auto& row : params) {
        EXPECT_EQ(VN_DYNAMIC_SIGNAL_CONTROL,
                  row.acceptedSignalFormsMask);
        EXPECT_EQ(VN_DYNAMIC_SOURCE_VARIABLE, row.sourceKindsMask);
        EXPECT_EQ(VN_DYNAMIC_CADENCE_UPDATE, row.updateCadence);
        EXPECT_EQ(VN_DYNAMIC_SMOOTHING_EFFECT_INTERNAL,
                  row.smoothingOwner);
        EXPECT_EQ(480u, row.smoothingFrames);
        EXPECT_EQ(VN_DYNAMIC_PARAM_CONTROL_SAFE, row.flags);
    }
}

TEST(CapabilityDiscovery,
     DynamicParameterSmoothingMatchesTheRuntimeTransitionRounding) {
    constexpr std::uint32_t kSampleRate = 44101;
    harkentest::EngineSession session(
        harkentest::MakeNullConfig(1, 64, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());

    std::uint32_t required = 0;
    ASSERT_EQ(VN_OK,
              Vn_CaptureEffectDynamicParameterCapabilities(
                  nullptr, 0, &required));
    std::vector<VnEffectDynamicParameterCapability> rows(required);
    rows[0].structSize = sizeof rows[0];
    ASSERT_EQ(VN_OK,
              Vn_CaptureEffectDynamicParameterCapabilities(
                  rows.data(),
                  static_cast<std::uint32_t>(rows.size()), &required));

    const std::uint32_t runtimeFrames =
        harken::CompressorTransitionFrames(kSampleRate);
    ASSERT_EQ(441u, runtimeFrames);
    for (const auto& row : rows) {
        EXPECT_EQ(runtimeFrames, row.smoothingFrames);
    }
}

} // namespace
