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

#include "PackcManifest.h"
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
using harkenstudiojson::packc::RecipeParamCount;
using harkenstudiojson::packc::RecipeParamInAuthoredRange;

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

// The packc authoring-range predicate is the creation-time gate for a recipe's
// effect parameters; the runtime validator is the install-time gate. Their
// accept domains must coincide, otherwise a pack authored against one window
// can be built yet refused at load (or vice versa). This pins that equality at
// every parameter's boundary against the runtime validator as the truth anchor,
// so widening or narrowing either side of a packc range by one representable
// step is caught. The registry static_asserts already freeze param counts; the
// coverage case below keeps this bound table exhaustive against the registry.
constexpr std::uint32_t kParitySampleRate = 48000;

struct AuthoredRangeBound {
    std::uint32_t type;
    std::uint32_t index;
    float lo;
    bool loInclusive;
    float hi;
    bool hiInclusive;
    // Frequency parameters carry no upper bound in the packc authoring predicate
    // because packc has no engine sample rate at authoring time; the Nyquist
    // ceiling is only reachable in the runtime validator. For those parameters
    // the upper-edge probes assert the runtime verdict alone, since packc
    // structurally accepts arbitrarily high frequencies until install.
    bool packcHasUpperBound;
};

constexpr AuthoredRangeBound kBounds[] = {
    {VN_EFFECT_FEEDBACK_DELAY, harken::kDelayParamTimeMs, 0.0f, false, 1000.0f,
     true, true},
    {VN_EFFECT_FEEDBACK_DELAY, harken::kDelayParamFeedback, 0.0f, true, 1.0f,
     false, true},
    {VN_EFFECT_FEEDBACK_DELAY, harken::kDelayParamWet, 0.0f, true, 16.0f, true,
     true},
    {VN_EFFECT_FEEDBACK_DELAY, harken::kDelayParamDry, 0.0f, true, 16.0f, true,
     true},
    {VN_EFFECT_THREE_BAND_TONE, harken::kToneParamLowShelfGainDb, -48.0f, true,
     48.0f, true, true},
    {VN_EFFECT_THREE_BAND_TONE, harken::kToneParamLowShelfFreqHz, 0.0f, false,
     24000.0f, false, false},
    {VN_EFFECT_THREE_BAND_TONE, harken::kToneParamPeakGainDb, -48.0f, true, 48.0f,
     true, true},
    {VN_EFFECT_THREE_BAND_TONE, harken::kToneParamPeakFreqHz, 0.0f, false,
     24000.0f, false, false},
    {VN_EFFECT_THREE_BAND_TONE, harken::kToneParamPeakQ, 0.0f, false, 100.0f,
     true, true},
    {VN_EFFECT_THREE_BAND_TONE, harken::kToneParamHighShelfGainDb, -48.0f, true,
     48.0f, true, true},
    {VN_EFFECT_THREE_BAND_TONE, harken::kToneParamHighShelfFreqHz, 0.0f, false,
     24000.0f, false, false},
    {VN_EFFECT_GAIN, harken::kGainParamGainDb, -96.0f, true, 24.0f, true, true},
    {VN_EFFECT_LIMITER, harken::kLimiterParamCeilingDb, -24.0f, true, 0.0f, true,
     true},
    {VN_EFFECT_LIMITER, harken::kLimiterParamReleaseMs, 1.0f, true, 1000.0f, true,
     true},
    {VN_EFFECT_COMPRESSOR, harken::kCompressorParamThresholdDb, -96.0f, true,
     0.0f, true, true},
    {VN_EFFECT_COMPRESSOR, harken::kCompressorParamRatio, 1.0f, true, 20.0f,
     true, true},
    {VN_EFFECT_COMPRESSOR, harken::kCompressorParamAttackMs, 0.1f, true, 200.0f,
     true, true},
    {VN_EFFECT_COMPRESSOR, harken::kCompressorParamReleaseMs, 5.0f, true,
     3000.0f, true, true},
    {VN_EFFECT_COMPRESSOR, harken::kCompressorParamKneeDb, 0.0f, true, 24.0f,
     true, true},
    {VN_EFFECT_COMPRESSOR, harken::kCompressorParamMakeupDb, -24.0f, true,
     24.0f, true, true},
};
constexpr std::size_t kBoundCount = sizeof(kBounds) / sizeof(kBounds[0]);

void Probe(const AuthoredRangeBound& b, float value, bool expectedAccept,
           bool comparePackc) {
    const bool runtime =
        ValidateEffectParam(b.type, b.index, value, kParitySampleRate) == VN_OK;
    EXPECT_EQ(expectedAccept, runtime)
        << "runtime validator, type " << b.type << " index " << b.index
        << " value " << value;
    if (comparePackc) {
        const bool authored =
            RecipeParamInAuthoredRange(b.type, b.index, value);
        EXPECT_EQ(runtime, authored)
            << "packc authoring vs runtime, type " << b.type << " index "
            << b.index << " value " << value;
    }
}

TEST(PackcAuthoredRangeParity, BoundTableCoversTheWholeEffectRegistry) {
    std::uint32_t registryParamTotal = 0;
    for (std::uint32_t r = 0; r < harken::kEffectRegistryRowCount; ++r) {
        const harken::EffectRegistryRow& row = harken::kEffectRegistry[r];
        registryParamTotal += row.paramCount;
        EXPECT_EQ(row.paramCount, RecipeParamCount(row.effectType))
            << "packc parameter-count drift for effect type " << row.effectType;
        for (std::uint32_t i = 0; i < row.paramCount; ++i) {
            bool found = false;
            for (const AuthoredRangeBound& b : kBounds) {
                if (b.type == row.effectType && b.index == i) {
                    found = true;
                    break;
                }
            }
            EXPECT_TRUE(found) << "no bound descriptor for effect type "
                               << row.effectType << " parameter " << i;
        }
    }
    EXPECT_EQ(registryParamTotal, static_cast<std::uint32_t>(kBoundCount));
}

TEST(PackcAuthoredRangeParity, AuthoringWindowMatchesRuntimeValidatorAtBounds) {
    constexpr float kInf = std::numeric_limits<float>::infinity();
    for (const AuthoredRangeBound& b : kBounds) {
        const float belowLo = std::nextafter(b.lo, -kInf);
        const float insideLo = std::nextafter(b.lo, kInf);
        const float insideHi = std::nextafter(b.hi, -kInf);
        const float aboveHi = std::nextafter(b.hi, kInf);

        Probe(b, belowLo, false, true);
        Probe(b, b.lo, b.loInclusive, true);
        Probe(b, insideLo, true, true);
        Probe(b, insideHi, true, true);
        Probe(b, b.hi, b.hiInclusive, b.packcHasUpperBound);
        Probe(b, aboveHi, false, b.packcHasUpperBound);
    }
}

} // namespace
