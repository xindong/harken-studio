// Walks every row of the scoped target metadata table: the table is the
// single point of
// truth for unit, combine domain, neutral value, legal range, transition
// support, and wire opcode per (scope, targetId). Completeness against the
// id constants is a compile-time assert in the header; these tests pin the
// documented row VALUES, prove the live voice-scope helpers
// (DomainOfTarget, ClampResolvedTarget) RETURN the table rows — the table
// is the consumed authority, not a parallel record — and pin the
// transition declaration.
#include "harken/core/CommandQueue.h"
#include "harken/param/ParamTargetTable.h"

#include <gtest/gtest.h>

#include <cstring>
#include <limits>

namespace {

using harken::CombineDomain;
using harken::FindTargetMeta;
using harken::TargetMeta;

static_assert(VN_TARGET_VOICE_VOLUME == 0 &&
                  VN_TARGET_VOICE_PITCH == 1 &&
                  VN_TARGET_VOICE_LOWPASS_CUTOFF == 2 &&
                  VN_TARGET_VOICE_HIGHPASS_CUTOFF == 3 &&
                  VN_TARGET_VOICE_PAN == 4 &&
                  VN_TARGET_VOICE_WIDTH == 5 &&
                  VN_TARGET_VOICE_LOW_SHELF_GAIN_DB == 6 &&
                  VN_TARGET_VOICE_LOWPASS_Q == 7 &&
                  VN_TARGET_VOICE_HIGHPASS_Q == 8 &&
                  VN_TARGET_VOICE_LOW_SHELF_FREQUENCY_HZ == 9 &&
                  VN_TARGET_VOICE_LOW_SHELF_Q == 10 &&
                  VN_TARGET_VOICE_HIGH_SHELF_GAIN_DB == 11 &&
                  VN_TARGET_VOICE_HIGH_SHELF_FREQUENCY_HZ == 12 &&
                  VN_TARGET_VOICE_HIGH_SHELF_Q == 13,
              "voice target ids are append-only");

TEST(TargetMetadata, TableCoversEveryTargetExactlyOnceInOrder) {
    ASSERT_EQ(harken::kBuiltinTargetCount + harken::kBusTargetCount,
              harken::kTargetMetaCount);
    for (std::uint32_t i = 0; i < harken::kBuiltinTargetCount; ++i) {
        EXPECT_EQ(harken::kTargetScopeVoice,
                  harken::kTargetMetaTable[i].scopeKind);
        EXPECT_EQ(i, harken::kTargetMetaTable[i].targetId);
    }
    for (std::uint32_t i = 0; i < harken::kBusTargetCount; ++i) {
        const TargetMeta& bus =
            harken::kTargetMetaTable[harken::kBuiltinTargetCount + i];
        EXPECT_EQ(harken::kTargetScopeBus, bus.scopeKind);
        EXPECT_EQ(i, bus.targetId);
    }
    EXPECT_EQ(2u, harken::kBusTargetCount);
    EXPECT_EQ(1u, harken::kBusTargetBusOutputVolume);
}

TEST(TargetMetadata, RowsMatchDocumentedUnitsDomainsAndNeutrals) {
    struct ExpectedRow {
        std::uint32_t scopeKind;
        std::uint32_t targetId;
        const char* unit;
        CombineDomain domain;
        float neutral;
    };
    const ExpectedRow expected[] = {
        {harken::kTargetScopeVoice, harken::kTargetVoiceVolume, "dB",
         CombineDomain::AddDb, 0.0f},
        {harken::kTargetScopeVoice, harken::kTargetVoicePitch, "cents",
         CombineDomain::AddCents, 0.0f},
        {harken::kTargetScopeVoice, harken::kTargetVoiceLowpassCutoff, "Hz",
         CombineDomain::MinHz, harken::kLowpassOpenHz},
        {harken::kTargetScopeVoice, harken::kTargetVoiceHighpassCutoff, "Hz",
         CombineDomain::MaxHz, harken::kHighpassClosedHz},
        {harken::kTargetScopeVoice, harken::kTargetVoicePan, "position",
         CombineDomain::Add, 0.0f},
        {harken::kTargetScopeVoice, harken::kTargetVoiceWidth, "width",
         CombineDomain::Max, 0.0f},
        {harken::kTargetScopeVoice, harken::kTargetVoiceLowShelfGainDb, "dB",
         CombineDomain::AddDb, 0.0f},
        {harken::kTargetScopeVoice, harken::kTargetVoiceLowpassQ, "Q",
         CombineDomain::AddLogQ, harken::kToneDefaultQ},
        {harken::kTargetScopeVoice, harken::kTargetVoiceHighpassQ, "Q",
         CombineDomain::AddLogQ, harken::kToneDefaultQ},
        {harken::kTargetScopeVoice, harken::kTargetVoiceLowShelfFreqHz, "Hz",
         CombineDomain::AddLogHz, harken::kToneLowShelfDefaultHz},
        {harken::kTargetScopeVoice, harken::kTargetVoiceLowShelfQ, "Q",
         CombineDomain::AddLogQ, harken::kToneDefaultQ},
        {harken::kTargetScopeVoice, harken::kTargetVoiceHighShelfGainDb, "dB",
         CombineDomain::AddDb, 0.0f},
        {harken::kTargetScopeVoice, harken::kTargetVoiceHighShelfFreqHz, "Hz",
         CombineDomain::AddLogHz, harken::kToneHighShelfDefaultHz},
        {harken::kTargetScopeVoice, harken::kTargetVoiceHighShelfQ, "Q",
         CombineDomain::AddLogQ, harken::kToneDefaultQ},
        {harken::kTargetScopeBus, harken::kBusTargetBusVolume, "dB",
         CombineDomain::AddDb, 0.0f},
        {harken::kTargetScopeBus, harken::kBusTargetBusOutputVolume, "dB",
         CombineDomain::AddDb, 0.0f},
    };
    ASSERT_EQ(harken::kTargetMetaCount,
              sizeof(expected) / sizeof(expected[0]));
    for (std::uint32_t i = 0; i < harken::kTargetMetaCount; ++i) {
        const TargetMeta& row = harken::kTargetMetaTable[i];
        EXPECT_EQ(expected[i].scopeKind, row.scopeKind) << "row " << i;
        EXPECT_EQ(expected[i].targetId, row.targetId) << "row " << i;
        EXPECT_STREQ(expected[i].unit, row.unit) << "row " << i;
        EXPECT_EQ(expected[i].domain, row.domain) << "row " << i;
        EXPECT_EQ(expected[i].neutral, row.neutral) << "row " << i;
    }
}

TEST(TargetMetadata, DomainOfTargetReturnsTheTableRowDomain) {
    // The live reduction path's domain helper reads the table rows: every
    // voice row's domain comes back verbatim, and unknown ids keep the
    // historical AddDb fallback.
    for (std::uint32_t i = 0; i < harken::kBuiltinTargetCount; ++i) {
        EXPECT_EQ(harken::kTargetMetaTable[i].domain,
                  harken::DomainOfTarget(harken::kTargetMetaTable[i].targetId))
            << "row " << i;
    }
    EXPECT_EQ(CombineDomain::AddDb,
              harken::DomainOfTarget(harken::kBuiltinTargetCount));
    EXPECT_EQ(CombineDomain::AddDb, harken::DomainOfTarget(1024));
}

TEST(TargetMetadata, ClampResolvedTargetEnforcesTheTableClampColumns) {
    // The live clamp helper enforces the table's clamp columns: clamped
    // rows pin extremes to the row span and pass interior values through;
    // unclamped rows pass extremes through untouched.
    for (std::uint32_t i = 0; i < harken::kBuiltinTargetCount; ++i) {
        const TargetMeta& row = harken::kTargetMetaTable[i];
        if (row.clamped) {
            EXPECT_EQ(row.clampMin,
                      harken::ClampResolvedTarget(row.targetId, -1.0e9f))
                << "row " << i;
            EXPECT_EQ(row.clampMax,
                      harken::ClampResolvedTarget(
                          row.targetId,
                          std::numeric_limits<float>::infinity()))
                << "row " << i;
            const float interior =
                row.clampMin * 0.5f + row.clampMax * 0.5f;
            EXPECT_EQ(interior,
                      harken::ClampResolvedTarget(row.targetId, interior))
                << "row " << i;
        } else {
            EXPECT_EQ(-1.0e9f, harken::ClampResolvedTarget(row.targetId, -1.0e9f))
                << "row " << i;
            EXPECT_EQ(1.0e9f, harken::ClampResolvedTarget(row.targetId, 1.0e9f))
                << "row " << i;
        }
    }
    // Pan, width, and every newly authored Tone parameter carry clamps.
    EXPECT_TRUE(harken::kTargetMetaTable[harken::kTargetVoicePan].clamped);
    EXPECT_TRUE(harken::kTargetMetaTable[harken::kTargetVoiceWidth].clamped);
    std::uint32_t clampedRows = 0;
    for (std::uint32_t i = 0; i < harken::kTargetMetaCount; ++i) {
        clampedRows += harken::kTargetMetaTable[i].clamped ? 1u : 0u;
    }
    EXPECT_EQ(10u, clampedRows);
}

TEST(TargetMetadata, PitchContributionSupportsTransition) {
    std::uint32_t transitions = 0;
    for (std::uint32_t i = 0; i < harken::kTargetMetaCount; ++i) {
        transitions += harken::kTargetMetaTable[i].supportsTransition ? 1u : 0u;
    }
    EXPECT_EQ(14u, transitions);
    EXPECT_TRUE(
        harken::kTargetMetaTable[harken::kTargetVoicePitch].supportsTransition);
}

TEST(TargetMetadata, ToneGlideTargetsSupportTransition) {
    std::uint32_t transitions = 0;
    for (std::uint32_t i = 0; i < harken::kTargetMetaCount; ++i) {
        transitions += harken::kTargetMetaTable[i].supportsTransition ? 1u : 0u;
    }
    EXPECT_EQ(14u, transitions);
    EXPECT_TRUE(
        harken::kTargetMetaTable[harken::kTargetVoiceVolume].supportsTransition);
    EXPECT_TRUE(harken::kTargetMetaTable[harken::kBuiltinTargetCount +
                                        harken::kBusTargetBusVolume]
                    .supportsTransition);
    EXPECT_TRUE(harken::kTargetMetaTable[harken::kBuiltinTargetCount +
                                        harken::kBusTargetBusOutputVolume]
                    .supportsTransition);
    EXPECT_TRUE(
        harken::kTargetMetaTable[harken::kTargetVoicePitch].supportsTransition);
    EXPECT_TRUE(harken::kTargetMetaTable[harken::kTargetVoiceLowpassCutoff]
                    .supportsTransition);
    EXPECT_TRUE(harken::kTargetMetaTable[harken::kTargetVoiceHighShelfQ]
                    .supportsTransition);
}

TEST(TargetMetadata, WireOpcodesMatchTheCommandVocabulary) {
    EXPECT_EQ(harken::kOpcodeSetVoiceVolume,
              harken::kTargetMetaTable[harken::kTargetVoiceVolume].wireOpcode);
    EXPECT_EQ(harken::kOpcodeSetVoicePitch,
              harken::kTargetMetaTable[harken::kTargetVoicePitch].wireOpcode);
    // Both filter cutoffs commit through the one filter command.
    EXPECT_EQ(
        harken::kOpcodeSetVoiceFilter,
        harken::kTargetMetaTable[harken::kTargetVoiceLowpassCutoff].wireOpcode);
    EXPECT_EQ(
        harken::kOpcodeSetVoiceFilter,
        harken::kTargetMetaTable[harken::kTargetVoiceHighpassCutoff].wireOpcode);
    EXPECT_EQ(harken::kOpcodeSetVoicePan,
              harken::kTargetMetaTable[harken::kTargetVoicePan].wireOpcode);
    EXPECT_EQ(harken::kOpcodeSetVoiceWidth,
              harken::kTargetMetaTable[harken::kTargetVoiceWidth].wireOpcode);
    EXPECT_EQ(
        harken::kOpcodeSetVoiceLowShelf,
        harken::kTargetMetaTable[harken::kTargetVoiceLowShelfGainDb].wireOpcode);
    EXPECT_EQ(harken::kOpcodeSetBusVolume,
              harken::kTargetMetaTable[harken::kBuiltinTargetCount +
                                      harken::kBusTargetBusVolume]
                  .wireOpcode);
    EXPECT_EQ(harken::kOpcodeSetBusVolume,
              harken::kTargetMetaTable[harken::kBuiltinTargetCount +
                                      harken::kBusTargetBusOutputVolume]
                  .wireOpcode);
}

TEST(TargetMetadata, FindTargetMetaResolvesKnownAndRefusesUnknown) {
    const TargetMeta* highpass =
        FindTargetMeta(harken::kTargetScopeVoice,
                       harken::kTargetVoiceHighpassCutoff);
    ASSERT_NE(nullptr, highpass);
    EXPECT_EQ(harken::kTargetVoiceHighpassCutoff, highpass->targetId);

    const TargetMeta* busVolume =
        FindTargetMeta(harken::kTargetScopeBus, harken::kBusTargetBusVolume);
    ASSERT_NE(nullptr, busVolume);
    EXPECT_EQ(CombineDomain::AddDb, busVolume->domain);
    const TargetMeta* busOutput =
        FindTargetMeta(harken::kTargetScopeBus,
                       harken::kBusTargetBusOutputVolume);
    ASSERT_NE(nullptr, busOutput);
    EXPECT_EQ(CombineDomain::AddDb, busOutput->domain);

    // Unknown target id within a known scope, and a scope kind that does
    // not exist yet, both resolve to no row (refused, never guessed).
    EXPECT_EQ(nullptr, FindTargetMeta(harken::kTargetScopeBus,
                                      harken::kBusTargetCount));
    EXPECT_EQ(nullptr,
              FindTargetMeta(harken::kTargetScopeVoice,
                             harken::kBuiltinTargetCount));
    EXPECT_EQ(nullptr, FindTargetMeta(2, 0));
}

} // namespace
