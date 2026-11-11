// The generated effect-parameter metadata authority: every implemented effect
// parameter (delay 4 + tone 7 + gain 1 + limiter 2 + compressor 6 = 20) has
// exactly one
// metadata row in private-index order, and each row's declared unit,
// combination domain, neutral, clamp, and transition support match the frozen
// contract. The
// generated header already static_asserts each effect's metadata-row count
// against its parameter-count constant; this pins the actual field values so a
// silent domain/clamp change is caught, and proves the runtime accessor
// resolves the same rows.
#include <harken/Harken.h>

#include "harken/media/generated/harken_pack_effect_schema.g.h"
#include "harken/render/BuiltinEffects.h"
#include "harken/render/generated/harken_effect_registry.g.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace {

using harken::EffectParamCombine;
using harken::EffectParamMeta;
using harken::EffectParamUnit;
using harken::FindEffectParamMeta;
using harken::FindPackEffectSchemaByManifestName;
using harken::FindPackEffectSchemaByType;
using harken::PackEffectSchemaRow;
using harken::ValidateEffectParam;

const EffectParamMeta& Meta(std::uint32_t type, std::uint32_t index) {
    const EffectParamMeta* m = FindEffectParamMeta(type, index);
    EXPECT_NE(nullptr, m) << "type " << type << " index " << index;
    return *m;
}

void ExpectMeta(std::uint32_t type, std::uint32_t index,
                EffectParamUnit unit, EffectParamCombine combine, float neutral,
                float clampMin, float clampMax,
                bool supportsTransition = false) {
    const EffectParamMeta& m = Meta(type, index);
    EXPECT_EQ(static_cast<std::uint32_t>(unit),
              static_cast<std::uint32_t>(m.unit))
        << "type " << type << " index " << index << " unit";
    EXPECT_EQ(static_cast<std::uint32_t>(combine),
              static_cast<std::uint32_t>(m.combine))
        << "type " << type << " index " << index << " combine";
    EXPECT_EQ(neutral, m.neutral);
    EXPECT_EQ(clampMin, m.clampMin);
    EXPECT_EQ(clampMax, m.clampMax);
    EXPECT_EQ(supportsTransition, m.supportsTransition);
}

TEST(EffectParamMetadata, DelayRowsMatchContract) {
    ExpectMeta(VN_EFFECT_FEEDBACK_DELAY, harken::kDelayParamTimeMs,
               EffectParamUnit::Ms, EffectParamCombine::None, 0.0f,
               harken::kEffectParamClampStrictlyPositive, 1000.0f);
    ExpectMeta(VN_EFFECT_FEEDBACK_DELAY, harken::kDelayParamFeedback,
               EffectParamUnit::Linear, EffectParamCombine::None, 0.0f, 0.0f,
               harken::kEffectParamClampStrictlyBelowOne);
    ExpectMeta(VN_EFFECT_FEEDBACK_DELAY, harken::kDelayParamWet,
               EffectParamUnit::Linear, EffectParamCombine::Multiply, 1.0f, 0.0f,
               16.0f, true);
    ExpectMeta(VN_EFFECT_FEEDBACK_DELAY, harken::kDelayParamDry,
               EffectParamUnit::Linear, EffectParamCombine::Multiply, 1.0f, 0.0f,
               16.0f, true);
}

TEST(EffectParamMetadata, GeneratedOpenBoundSentinelsAreUnique) {
    EXPECT_NE(harken::kEffectParamClampStrictlyPositive,
              harken::kEffectParamClampNyquist);
    EXPECT_NE(harken::kEffectParamClampStrictlyPositive,
              harken::kEffectParamClampStrictlyBelowOne);
    EXPECT_NE(harken::kEffectParamClampNyquist,
              harken::kEffectParamClampStrictlyBelowOne);
}

TEST(EffectParamMetadata, ToneRowsMatchContract) {
    ExpectMeta(VN_EFFECT_THREE_BAND_TONE, harken::kToneParamLowShelfGainDb,
               EffectParamUnit::Db, EffectParamCombine::AddDb, 0.0f, -48.0f,
               48.0f, true);
    // Strictly-positive clampMin is resolved to the smallest positive float,
    // so composed frequency and Q values satisfy the released open validator
    // bounds without imposing a tighter numeric floor.
    ExpectMeta(VN_EFFECT_THREE_BAND_TONE, harken::kToneParamLowShelfFreqHz,
               EffectParamUnit::Hz, EffectParamCombine::AddLogHz, 0.0f,
               harken::kEffectParamClampStrictlyPositive,
               harken::kEffectParamClampNyquist, true);
    ExpectMeta(VN_EFFECT_THREE_BAND_TONE, harken::kToneParamPeakGainDb,
               EffectParamUnit::Db, EffectParamCombine::AddDb, 0.0f, -48.0f,
               48.0f, true);
    ExpectMeta(VN_EFFECT_THREE_BAND_TONE, harken::kToneParamPeakFreqHz,
               EffectParamUnit::Hz, EffectParamCombine::AddLogHz, 0.0f,
               harken::kEffectParamClampStrictlyPositive,
               harken::kEffectParamClampNyquist, true);
    ExpectMeta(VN_EFFECT_THREE_BAND_TONE, harken::kToneParamPeakQ,
               EffectParamUnit::Q, EffectParamCombine::AddLogQ, 0.0f,
               harken::kEffectParamClampStrictlyPositive,
               100.0f, true);
    ExpectMeta(VN_EFFECT_THREE_BAND_TONE, harken::kToneParamHighShelfGainDb,
               EffectParamUnit::Db, EffectParamCombine::AddDb, 0.0f, -48.0f,
               48.0f, true);
    ExpectMeta(VN_EFFECT_THREE_BAND_TONE, harken::kToneParamHighShelfFreqHz,
               EffectParamUnit::Hz, EffectParamCombine::AddLogHz, 0.0f,
               harken::kEffectParamClampStrictlyPositive,
               harken::kEffectParamClampNyquist, true);
}

TEST(EffectParamMetadata, GainRowMatchesContract) {
    ExpectMeta(VN_EFFECT_GAIN, harken::kGainParamGainDb, EffectParamUnit::Db,
               EffectParamCombine::AddDb, 0.0f, -96.0f, 24.0f, true);
}

TEST(EffectParamMetadata, LimiterRowsMatchContract) {
    ExpectMeta(VN_EFFECT_LIMITER, harken::kLimiterParamCeilingDb,
               EffectParamUnit::Db, EffectParamCombine::AddDb, 0.0f, -24.0f,
               0.0f, true);
    ExpectMeta(VN_EFFECT_LIMITER, harken::kLimiterParamReleaseMs,
               EffectParamUnit::Ms, EffectParamCombine::AddLogTime, 0.0f, 1.0f,
               1000.0f, true);
}

TEST(EffectParamMetadata, CompressorRowsMatchContract) {
    ExpectMeta(VN_EFFECT_COMPRESSOR, harken::kCompressorParamThresholdDb,
               EffectParamUnit::Db, EffectParamCombine::AddDb, 0.0f, -96.0f,
               0.0f, true);
    ExpectMeta(VN_EFFECT_COMPRESSOR, harken::kCompressorParamRatio,
               EffectParamUnit::Ratio, EffectParamCombine::Multiply, 1.0f,
               1.0f, 20.0f, true);
    ExpectMeta(VN_EFFECT_COMPRESSOR, harken::kCompressorParamAttackMs,
               EffectParamUnit::Ms, EffectParamCombine::AddLogTime, 0.0f,
               0.1f, 200.0f, true);
    ExpectMeta(VN_EFFECT_COMPRESSOR, harken::kCompressorParamReleaseMs,
               EffectParamUnit::Ms, EffectParamCombine::AddLogTime, 0.0f,
               5.0f, 3000.0f, true);
    ExpectMeta(VN_EFFECT_COMPRESSOR, harken::kCompressorParamKneeDb,
               EffectParamUnit::Db, EffectParamCombine::AddDb, 0.0f, 0.0f,
               24.0f, true);
    ExpectMeta(VN_EFFECT_COMPRESSOR, harken::kCompressorParamMakeupDb,
               EffectParamUnit::Db, EffectParamCombine::AddDb, 0.0f, -24.0f,
               24.0f, true);
}

TEST(EffectParamMetadata, AccessorRefusesUnknownTypeAndOutOfRangeIndex) {
    // Unknown effect type: no metadata.
    EXPECT_EQ(nullptr, FindEffectParamMeta(0xFFFFFFFFu, 0));
    // Index at/after the type's parameter count: no metadata (delay has 4).
    EXPECT_EQ(nullptr, FindEffectParamMeta(VN_EFFECT_FEEDBACK_DELAY,
                                           harken::kDelayParamCount));
    EXPECT_NE(nullptr, FindEffectParamMeta(VN_EFFECT_FEEDBACK_DELAY,
                                           harken::kDelayParamCount - 1));
    // Gain has exactly one parameter.
    EXPECT_EQ(nullptr, FindEffectParamMeta(VN_EFFECT_GAIN,
                                           harken::kGainParamCount));
    // Limiter has exactly two parameters.
    EXPECT_EQ(nullptr, FindEffectParamMeta(VN_EFFECT_LIMITER,
                                           harken::kLimiterParamCount));
    EXPECT_EQ(nullptr, FindEffectParamMeta(VN_EFFECT_COMPRESSOR,
                                           harken::kCompressorParamCount));
}

TEST(PackEffectSchema, GeneratedRowsCarryManifestIdentityAndShape) {
    struct Expected {
        const char* name;
        std::uint32_t type;
        std::uint32_t paramCount;
    };
    constexpr Expected kExpected[] = {
        {"feedbackDelay", VN_EFFECT_FEEDBACK_DELAY, 4},
        {"threeBandTone", VN_EFFECT_THREE_BAND_TONE, 7},
        {"gain", VN_EFFECT_GAIN, 1},
        {"limiter", VN_EFFECT_LIMITER, 2},
        {"compressor", VN_EFFECT_COMPRESSOR, 6},
    };

    ASSERT_EQ(sizeof(kExpected) / sizeof(kExpected[0]),
              harken::kPackEffectSchemaRowCount);
    for (const Expected& expected : kExpected) {
        const PackEffectSchemaRow* byType =
            FindPackEffectSchemaByType(expected.type);
        const PackEffectSchemaRow* byName =
            FindPackEffectSchemaByManifestName(expected.name);
        ASSERT_NE(nullptr, byType);
        EXPECT_EQ(byType, byName);
        EXPECT_STREQ(expected.name, byType->manifestName);
        EXPECT_EQ(expected.type, byType->effectType);
        EXPECT_EQ(2u, byType->descriptorVintage);
        EXPECT_EQ(expected.paramCount, byType->paramCount);
    }
    EXPECT_EQ(nullptr, FindPackEffectSchemaByType(0xFFFFFFFFu));
    EXPECT_EQ(nullptr, FindPackEffectSchemaByManifestName("unknownEffect"));
}

TEST(PackEffectSchema, NyquistUpperBoundRemainsRuntimeDependent) {
    const PackEffectSchemaRow* tone =
        FindPackEffectSchemaByType(VN_EFFECT_THREE_BAND_TONE);
    ASSERT_NE(nullptr, tone);
    ASSERT_LT(harken::kToneParamLowShelfFreqHz, tone->paramCount);
    const harken::PackEffectAuthoredRange& frequency =
        tone->authoredRanges[harken::kToneParamLowShelfFreqHz];
    EXPECT_TRUE(frequency.hasMinimum);
    EXPECT_FALSE(frequency.minimumInclusive);
    EXPECT_FLOAT_EQ(0.0f, frequency.minimum);
    EXPECT_FALSE(frequency.hasMaximum);
    EXPECT_TRUE(harken::PackEffectParamInAuthoredRange(
        VN_EFFECT_THREE_BAND_TONE,
        harken::kToneParamLowShelfFreqHz, 1000000.0f));
}

} // namespace
