// Effect-parameter binding resolution store, in isolation: the combine math per
// declared domain (AddDb / Multiply / AddLog with Nyquist clamp / None), base
// seeding from the install-applied values, the direct-value retarget, the
// attach eligibility gate (empty slot / type mismatch / None domain), the
// detach edge (contribution returns to neutral -> resolved returns to base),
// and the dirty / last-committed publication bookkeeping. No engine or audio
// thread is involved; this pins the metadata authority's semantics as realized
// by the store.
#include "Common/ObjectIdTestSupport.h"
#include <harken/Harken.h>

#include "harken/param/EffectParamLocator.h"
#include "harken/param/EffectParamTargetStore.h"
#include "harken/render/BuiltinEffects.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

using harken::EffectParamLocator;
using harken::EffectParamTargetStore;
using Scope = harken::EffectParamTargetStore::Scope;

constexpr std::uint32_t kRate = 48000;
constexpr std::uint64_t kVoice = 1;
constexpr std::uint32_t kSlot = 0;

// Descriptor-applied bases in parameter-index order.
std::vector<float> DelayBases(float delayMs, float feedback, float wet,
                              float dry) {
    return {delayMs, feedback, wet, dry};
}
std::vector<float> ToneBases() {
    return {0.0f, 200.0f, 0.0f, 1000.0f, 1.5f, 0.0f, 6000.0f};
}
std::vector<float> GainBases(float gainDb) { return {gainDb}; }
std::vector<float> LimiterBases(float ceilingDb, float releaseMs) {
    return {ceilingDb, releaseMs};
}

EffectParamTargetStore MakeStore() {
    return EffectParamTargetStore(kRate, /*capacity=*/64);
}

// Runs one contribution sweep folding the given contributions onto (obj, slot,
// index) for effect type `type`. An empty list simulates the last binding
// detaching (no accumulate, so the record returns to its domain neutral in
// EndContributionSweep).
void SweepAs(EffectParamTargetStore& s, std::uint64_t obj, std::uint32_t slot,
             std::uint32_t index, std::uint32_t type,
             std::initializer_list<float> contributions) {
    s.BeginContributionSweep();
    for (float c : contributions) {
        s.AccumulateBinding(Scope::Voice, obj, slot, index, type, c);
    }
    s.EndContributionSweep();
}

// Sweep for the type that was seeded in each test's common case.
void Sweep(EffectParamTargetStore& s, std::uint64_t obj, std::uint32_t slot,
           std::uint32_t index, std::initializer_list<float> contributions) {
    SweepAs(s, obj, slot, index, VN_EFFECT_FEEDBACK_DELAY, contributions);
}

// --- base seeding ---

TEST(EffectParamTargetStore, SeedComposesBindingOnAuthoredBaseNotNeutral) {
    // The mandated case: an effect installed by descriptor only (wet=1.0, never
    // directly set), a Multiply 0.5 binding contribution -> resolved 0.5.
    EffectParamTargetStore s = MakeStore();
    const std::vector<float> bases = DelayBases(10.0f, 0.0f, 1.0f, 1.0f);
    ASSERT_TRUE(s.SeedInstalledEffect(Scope::Voice, kVoice, kSlot,
                                      VN_EFFECT_FEEDBACK_DELAY, bases.data(),
                                      harken::kDelayParamCount));
    // No binding yet: reduces to the authored base, nothing to publish.
    EXPECT_FLOAT_EQ(1.0f, s.Reduce(Scope::Voice, kVoice, kSlot,
                                   harken::kDelayParamWet));
    EXPECT_EQ(0u, s.DemandCount());

    Sweep(s, kVoice, kSlot, harken::kDelayParamWet, {0.5f});
    EXPECT_FLOAT_EQ(0.5f, s.Reduce(Scope::Voice, kVoice, kSlot,
                                   harken::kDelayParamWet));
    EXPECT_EQ(1u, s.DemandCount());
}

TEST(EffectParamTargetStore, SharedPublicationKeepsLaterSnapshotAndNewContribution) {
    auto store = MakeStore();
    const auto bases = DelayBases(10.0f, 0.0f, 1.0f, 1.0f);
    ASSERT_TRUE(store.SeedInstalledEffect(Scope::Voice, kVoice, kSlot,
        VN_EFFECT_FEEDBACK_DELAY, bases.data(), harken::kDelayParamCount));
    constexpr auto param = harken::kDelayParamWet;
    const auto revision = store.TargetRevision(Scope::Voice, kVoice, kSlot, param);
    const harken::TargetClaimOwner owner{0, 1, harken::TargetClaimKind::VariablePublication};
    const harken::VariableSliceRef probe{0, 1};
    ASSERT_TRUE(store.TryClaim(Scope::Voice, kVoice, kSlot, param, owner, revision));
    ASSERT_TRUE(store.TryClaim(Scope::Voice, kVoice, kSlot, param, owner, revision));
    EXPECT_FALSE(store.TryClaim(Scope::Voice, kVoice, kSlot, param, probe, revision));
    Sweep(store, kVoice, kSlot, param, {0.5f});
    store.MarkSliceCommitted(Scope::Voice, kVoice, kSlot, param, owner, 1.0f);
    EXPECT_TRUE(store.ForeignClaim(Scope::Voice, kVoice, kSlot, param));
    EXPECT_EQ(1u, store.DemandCount());
    store.ReleaseClaim(Scope::Voice, kVoice, kSlot, param, probe);
    EXPECT_TRUE(store.ForeignClaim(Scope::Voice, kVoice, kSlot, param));
    store.ReleaseClaim(Scope::Voice, kVoice, kSlot, param, owner);
    EXPECT_FALSE(store.ForeignClaim(Scope::Voice, kVoice, kSlot, param));
    EXPECT_EQ(1u, store.DemandCount());
    ASSERT_TRUE(store.TryClaim(Scope::Voice, kVoice, kSlot, param, probe, revision));
    store.ReleaseClaim(Scope::Voice, kVoice, kSlot, param, probe);
}

TEST(EffectParamTargetStore, VariableProjectionRestoresFoldAndPublicationDemand) {
    auto store = MakeStore();
    const auto bases = DelayBases(10.0f, 0.0f, 1.0f, 1.0f);
    ASSERT_TRUE(store.SeedInstalledEffect(Scope::Voice, kVoice, kSlot,
        VN_EFFECT_FEEDBACK_DELAY, bases.data(), harken::kDelayParamCount));
    constexpr auto param = harken::kDelayParamWet;
    Sweep(store, kVoice, kSlot, param, {0.5f});
    const auto demand = store.DemandCount();
    ASSERT_TRUE(store.BeginVariableProjection());
    EXPECT_FALSE(store.BeginVariableProjection());
    Sweep(store, kVoice, kSlot, param, {0.25f});
    EXPECT_FLOAT_EQ(0.25f, store.Reduce(Scope::Voice, kVoice, kSlot, param));
    store.RestoreVariableProjection();
    EXPECT_FLOAT_EQ(0.5f, store.Reduce(Scope::Voice, kVoice, kSlot, param));
    EXPECT_EQ(demand, store.DemandCount());
    Sweep(store, kVoice, kSlot, param, {0.75f});
    EXPECT_FLOAT_EQ(0.75f, store.Reduce(Scope::Voice, kVoice, kSlot, param));
    EXPECT_GT(store.VariableProjectionStorageBytes(), 0u);
}

TEST(EffectParamTargetStore, DelayOpenBoundsResolveToConcreteFloats) {
    EffectParamTargetStore s = MakeStore();
    const std::vector<float> bases = DelayBases(0.0f, 1.0f, 1.0f, 1.0f);
    ASSERT_TRUE(s.SeedInstalledEffect(Scope::Voice, kVoice, kSlot,
                                      VN_EFFECT_FEEDBACK_DELAY, bases.data(),
                                      harken::kDelayParamCount));
    EXPECT_EQ(std::numeric_limits<float>::denorm_min(),
              s.Reduce(Scope::Voice, kVoice, kSlot,
                       harken::kDelayParamTimeMs));
    EXPECT_EQ(std::nextafter(1.0f, 0.0f),
              s.Reduce(Scope::Voice, kVoice, kSlot,
                       harken::kDelayParamFeedback));
}

// --- combine domains ---

TEST(EffectParamTargetStore, MultiplyFoldsCommutativeProduct) {
    EffectParamTargetStore s = MakeStore();
    const std::vector<float> bases = DelayBases(10.0f, 0.0f, 1.0f, 1.0f);
    ASSERT_TRUE(s.SeedInstalledEffect(Scope::Voice, kVoice, kSlot,
                                      VN_EFFECT_FEEDBACK_DELAY, bases.data(),
                                      harken::kDelayParamCount));
    Sweep(s, kVoice, kSlot, harken::kDelayParamWet, {0.5f, 0.5f});
    EXPECT_FLOAT_EQ(0.25f, s.Reduce(Scope::Voice, kVoice, kSlot,
                                    harken::kDelayParamWet));
}

TEST(EffectParamTargetStore,
     MeterProbeContributionUsesAnIndependentNeutralSlot) {
    EffectParamTargetStore s = MakeStore();
    const std::vector<float> bases =
        DelayBases(10.0f, 0.0f, 0.8f, 1.0f);
    ASSERT_TRUE(s.SeedInstalledEffect(
        Scope::Voice, kVoice, kSlot, VN_EFFECT_FEEDBACK_DELAY,
        bases.data(), harken::kDelayParamCount));

    Sweep(s, kVoice, kSlot, harken::kDelayParamWet, {0.5f});
    EXPECT_FLOAT_EQ(0.4f, s.Reduce(
                              Scope::Voice, kVoice, kSlot,
                              harken::kDelayParamWet));

    s.BeginMeterProbeSweep();
    s.AccumulateMeterProbe(
        Scope::Voice, kVoice, kSlot, harken::kDelayParamWet,
        VN_EFFECT_FEEDBACK_DELAY, /*bindingId=*/harkentest::ObjectId(1), 0.25f);
    s.EndMeterProbeSweep();
    EXPECT_FLOAT_EQ(0.1f, s.Reduce(
                              Scope::Voice, kVoice, kSlot,
                              harken::kDelayParamWet));

    s.ClearMeterProbeContribution(
        Scope::Voice, kVoice, kSlot, harken::kDelayParamWet,
        VN_EFFECT_FEEDBACK_DELAY, /*bindingId=*/harkentest::ObjectId(1));
    EXPECT_FLOAT_EQ(0.4f, s.Reduce(
                              Scope::Voice, kVoice, kSlot,
                              harken::kDelayParamWet));
}

TEST(EffectParamTargetStore, AddDbSumsAndClamps) {
    EffectParamTargetStore s = MakeStore();
    std::vector<float> bases = GainBases(-6.0f);
    ASSERT_TRUE(s.SeedInstalledEffect(Scope::Voice, kVoice, kSlot,
                                      VN_EFFECT_GAIN, bases.data(),
                                      harken::kGainParamCount));
    SweepAs(s, kVoice, kSlot, harken::kGainParamGainDb, VN_EFFECT_GAIN, {2.0f});
    EXPECT_FLOAT_EQ(-4.0f, s.Reduce(Scope::Voice, kVoice, kSlot,
                                    harken::kGainParamGainDb));

    // Clamp to the gain range [-96, 24]: base 20 + 10 = 30 -> 24.
    bases = GainBases(20.0f);
    ASSERT_TRUE(s.SeedInstalledEffect(Scope::Voice, kVoice, kSlot,
                                      VN_EFFECT_GAIN, bases.data(),
                                      harken::kGainParamCount));
    SweepAs(s, kVoice, kSlot, harken::kGainParamGainDb, VN_EFFECT_GAIN, {10.0f});
    EXPECT_FLOAT_EQ(24.0f, s.Reduce(Scope::Voice, kVoice, kSlot,
                                    harken::kGainParamGainDb));
}

TEST(EffectParamTargetStore, AddLogHzScalesByOctavesAndClampsToNyquist) {
    EffectParamTargetStore s = MakeStore();
    const std::vector<float> bases = ToneBases(); // peakFreqHz base 1000
    ASSERT_TRUE(s.SeedInstalledEffect(Scope::Voice, kVoice, kSlot,
                                      VN_EFFECT_THREE_BAND_TONE, bases.data(),
                                      harken::kToneParamCount));
    // +1 octave: 1000 -> 2000.
    SweepAs(s, kVoice, kSlot, harken::kToneParamPeakFreqHz, VN_EFFECT_THREE_BAND_TONE, {1.0f});
    EXPECT_FLOAT_EQ(2000.0f, s.Reduce(Scope::Voice, kVoice, kSlot,
                                      harken::kToneParamPeakFreqHz));
    // +5 octaves: 1000 * 32 = 32000 -> clamped STRICTLY BELOW Nyquist (the
    // released validator requires freq < nyquist), so the resolved value is
    // just under 0.5 * kRate, never equal to it.
    SweepAs(s, kVoice, kSlot, harken::kToneParamPeakFreqHz, VN_EFFECT_THREE_BAND_TONE, {5.0f});
    const float nyquist = 0.5f * static_cast<float>(kRate);
    const float resolvedHz =
        s.Reduce(Scope::Voice, kVoice, kSlot, harken::kToneParamPeakFreqHz);
    EXPECT_LT(resolvedHz, nyquist);
    EXPECT_NEAR(nyquist, resolvedHz, 1.0f);
}

TEST(EffectParamTargetStore, AddLogQScalesByOctaves) {
    EffectParamTargetStore s = MakeStore();
    const std::vector<float> bases = ToneBases(); // peakQ base 1.5
    ASSERT_TRUE(s.SeedInstalledEffect(Scope::Voice, kVoice, kSlot,
                                      VN_EFFECT_THREE_BAND_TONE, bases.data(),
                                      harken::kToneParamCount));
    SweepAs(s, kVoice, kSlot, harken::kToneParamPeakQ, VN_EFFECT_THREE_BAND_TONE, {1.0f}); // 1.5 * 2 = 3.0
    EXPECT_FLOAT_EQ(3.0f, s.Reduce(Scope::Voice, kVoice, kSlot,
                                   harken::kToneParamPeakQ));
}

TEST(EffectParamTargetStore, AddLogTimeScalesByOctavesAndClamps) {
    EffectParamTargetStore s = MakeStore();
    std::vector<float> bases = LimiterBases(-1.0f, 100.0f);
    ASSERT_TRUE(s.SeedInstalledEffect(Scope::Voice, kVoice, kSlot,
                                      VN_EFFECT_LIMITER, bases.data(),
                                      harken::kLimiterParamCount));
    SweepAs(s, kVoice, kSlot, harken::kLimiterParamReleaseMs,
            VN_EFFECT_LIMITER, {1.0f});
    EXPECT_FLOAT_EQ(200.0f, s.Reduce(Scope::Voice, kVoice, kSlot,
                                     harken::kLimiterParamReleaseMs));

    SweepAs(s, kVoice, kSlot, harken::kLimiterParamReleaseMs,
            VN_EFFECT_LIMITER, {4.0f});
    EXPECT_FLOAT_EQ(1000.0f, s.Reduce(Scope::Voice, kVoice, kSlot,
                                      harken::kLimiterParamReleaseMs));
}

TEST(EffectParamTargetStore,
     SubnormalFreqSeedsExactlyAndNormalFoldIsNotSuppressed) {
    // The released validator accepts every finite frequency greater than zero,
    // including subnormals. Preserve that exact base as the committed reference
    // so a later fold to the smallest normal float is observed as a change.
    EffectParamTargetStore s = MakeStore();
    const float base = std::numeric_limits<float>::denorm_min();
    const float folded = std::numeric_limits<float>::min();
    const std::vector<float> bases = {0.0f, base, 0.0f, 1000.0f,
                                      1.5f, 0.0f, 6000.0f};
    ASSERT_TRUE(s.SeedInstalledEffect(Scope::Voice, kVoice, kSlot,
                                      VN_EFFECT_THREE_BAND_TONE, bases.data(),
                                      harken::kToneParamCount));
    EXPECT_FLOAT_EQ(base, s.LastCommitted(Scope::Voice, kVoice, kSlot,
                                          harken::kToneParamLowShelfFreqHz));
    EXPECT_FLOAT_EQ(base, s.Reduce(Scope::Voice, kVoice, kSlot,
                                   harken::kToneParamLowShelfFreqHz));

    // denorm_min * 2^23 is the smallest normal float. A clamp-up during seeding
    // would make this equal the committed reference and suppress the emission.
    SweepAs(s, kVoice, kSlot, harken::kToneParamLowShelfFreqHz,
            VN_EFFECT_THREE_BAND_TONE, {23.0f});
    EXPECT_FLOAT_EQ(folded, s.Reduce(Scope::Voice, kVoice, kSlot,
                                     harken::kToneParamLowShelfFreqHz));
    int emitted = 0;
    float got = 0.0f;
    s.FlushDirty([&](Scope, std::uint64_t, std::uint32_t, std::uint32_t,
                     float resolved) {
        ++emitted;
        got = resolved;
        return true;
    });
    EXPECT_EQ(1, emitted);
    EXPECT_FLOAT_EQ(folded, got);

    // Detach returns to the exact subnormal base and emits that change too.
    SweepAs(s, kVoice, kSlot, harken::kToneParamLowShelfFreqHz,
            VN_EFFECT_THREE_BAND_TONE, {});
    EXPECT_FLOAT_EQ(base, s.Reduce(Scope::Voice, kVoice, kSlot,
                                   harken::kToneParamLowShelfFreqHz));
    int emitted2 = 0;
    float got2 = 0.0f;
    s.FlushDirty([&](Scope, std::uint64_t, std::uint32_t, std::uint32_t,
                     float resolved) {
        ++emitted2;
        got2 = resolved;
        return true;
    });
    EXPECT_EQ(1, emitted2);
    EXPECT_FLOAT_EQ(base, got2);
}

TEST(EffectParamTargetStore, ExtremeFoldsStayStrictlyPositiveAndWithinCeilings) {
    // The strictly-positive floor keeps an extreme downward fold above zero
    // (never exactly 0, which would fail the released freq > 0 / Q > 0 rule), and
    // the ceilings keep an extreme upward fold below Nyquist / at Q 100.
    EffectParamTargetStore s = MakeStore();
    const std::vector<float> bases = ToneBases();
    ASSERT_TRUE(s.SeedInstalledEffect(Scope::Voice, kVoice, kSlot,
                                      VN_EFFECT_THREE_BAND_TONE, bases.data(),
                                      harken::kToneParamCount));
    const float nyquist = 0.5f * static_cast<float>(kRate);

    SweepAs(s, kVoice, kSlot, harken::kToneParamPeakFreqHz,
            VN_EFFECT_THREE_BAND_TONE, {-200.0f}); // -> underflow toward 0
    float f = s.Reduce(Scope::Voice, kVoice, kSlot, harken::kToneParamPeakFreqHz);
    EXPECT_GT(f, 0.0f);
    EXPECT_LT(f, nyquist);

    SweepAs(s, kVoice, kSlot, harken::kToneParamPeakFreqHz,
            VN_EFFECT_THREE_BAND_TONE, {200.0f}); // -> overflow past Nyquist
    f = s.Reduce(Scope::Voice, kVoice, kSlot, harken::kToneParamPeakFreqHz);
    EXPECT_GT(f, 0.0f);
    EXPECT_LT(f, nyquist);

    SweepAs(s, kVoice, kSlot, harken::kToneParamPeakQ,
            VN_EFFECT_THREE_BAND_TONE, {-200.0f}); // Q toward 0
    float q = s.Reduce(Scope::Voice, kVoice, kSlot, harken::kToneParamPeakQ);
    EXPECT_GT(q, 0.0f);
    EXPECT_LE(q, 100.0f);

    SweepAs(s, kVoice, kSlot, harken::kToneParamPeakQ,
            VN_EFFECT_THREE_BAND_TONE, {200.0f}); // Q past the 100 ceiling
    q = s.Reduce(Scope::Voice, kVoice, kSlot, harken::kToneParamPeakQ);
    EXPECT_GT(q, 0.0f);
    EXPECT_FLOAT_EQ(100.0f, q);
}

TEST(EffectParamTargetStore, ExtremeFreqAndQFoldsStayInValidatorRange) {
    EffectParamTargetStore s = MakeStore();
    const std::vector<float> bases = ToneBases();
    ASSERT_TRUE(s.SeedInstalledEffect(Scope::Voice, kVoice, kSlot,
                                      VN_EFFECT_THREE_BAND_TONE, bases.data(),
                                      harken::kToneParamCount));
    // Every composed value the flush would emit must pass the released
    // validator (freq > 0 && < nyquist; Q > 0), even for extreme folds — the
    // audio thread applies opcode 14/22 without re-validating.
    SweepAs(s, kVoice, kSlot, harken::kToneParamPeakFreqHz,
            VN_EFFECT_THREE_BAND_TONE, {-40.0f}); // -> ~0 Hz, clamped up
    EXPECT_EQ(VN_OK, harken::ValidateEffectParam(
                         VN_EFFECT_THREE_BAND_TONE, harken::kToneParamPeakFreqHz,
                         s.Reduce(Scope::Voice, kVoice, kSlot,
                                  harken::kToneParamPeakFreqHz),
                         kRate));
    SweepAs(s, kVoice, kSlot, harken::kToneParamPeakFreqHz,
            VN_EFFECT_THREE_BAND_TONE, {40.0f}); // -> >> nyquist, clamped down
    EXPECT_EQ(VN_OK, harken::ValidateEffectParam(
                         VN_EFFECT_THREE_BAND_TONE, harken::kToneParamPeakFreqHz,
                         s.Reduce(Scope::Voice, kVoice, kSlot,
                                  harken::kToneParamPeakFreqHz),
                         kRate));
    SweepAs(s, kVoice, kSlot, harken::kToneParamPeakQ,
            VN_EFFECT_THREE_BAND_TONE, {-40.0f}); // -> ~0 Q, clamped up
    EXPECT_EQ(VN_OK, harken::ValidateEffectParam(
                         VN_EFFECT_THREE_BAND_TONE, harken::kToneParamPeakQ,
                         s.Reduce(Scope::Voice, kVoice, kSlot,
                                  harken::kToneParamPeakQ),
                         kRate));
}

// --- attach eligibility (CanBind) ---

TEST(EffectParamTargetStore, CanBindGatesSlotTypeAndDomain) {
    EffectParamTargetStore s = MakeStore();
    const std::vector<float> bases = DelayBases(10.0f, 0.5f, 1.0f, 1.0f);
    ASSERT_TRUE(s.SeedInstalledEffect(Scope::Voice, kVoice, kSlot,
                                      VN_EFFECT_FEEDBACK_DELAY, bases.data(),
                                      harken::kDelayParamCount));

    auto loc = [](std::uint32_t type, std::uint32_t slot, std::uint32_t idx) {
        EffectParamLocator l;
        l.effectType = type;
        l.slot = slot;
        l.paramIndex = idx;
        return l;
    };

    // Bindable: matching type, existing slot, Multiply-domain wet.
    EXPECT_TRUE(s.CanBind(Scope::Voice, kVoice,
                          loc(VN_EFFECT_FEEDBACK_DELAY, kSlot,
                              harken::kDelayParamWet)));
    // None-domain parameter (feedback): refused.
    EXPECT_FALSE(s.CanBind(Scope::Voice, kVoice,
                           loc(VN_EFFECT_FEEDBACK_DELAY, kSlot,
                               harken::kDelayParamFeedback)));
    // Expected type mismatch: refused.
    EXPECT_FALSE(s.CanBind(Scope::Voice, kVoice,
                           loc(VN_EFFECT_THREE_BAND_TONE, kSlot,
                               harken::kDelayParamWet)));
    // Out-of-range parameter index (no record): refused.
    EXPECT_FALSE(s.CanBind(Scope::Voice, kVoice,
                           loc(VN_EFFECT_FEEDBACK_DELAY, kSlot,
                               harken::kDelayParamCount)));
    // Empty slot / unknown object: refused.
    EXPECT_FALSE(s.CanBind(Scope::Voice, 999,
                           loc(VN_EFFECT_FEEDBACK_DELAY, kSlot,
                               harken::kDelayParamWet)));
}

TEST(EffectParamTargetStore, TypeMismatchFoldsNothing) {
    EffectParamTargetStore s = MakeStore();
    const std::vector<float> bases = DelayBases(10.0f, 0.0f, 1.0f, 1.0f);
    ASSERT_TRUE(s.SeedInstalledEffect(Scope::Voice, kVoice, kSlot,
                                      VN_EFFECT_FEEDBACK_DELAY, bases.data(),
                                      harken::kDelayParamCount));
    // A binding whose locator expects a different effect type than the one
    // installed at the slot folds nothing: the parameter stays at its base.
    SweepAs(s, kVoice, kSlot, harken::kDelayParamWet, VN_EFFECT_GAIN, {0.5f});
    EXPECT_FLOAT_EQ(1.0f, s.Reduce(Scope::Voice, kVoice, kSlot,
                                   harken::kDelayParamWet));
    EXPECT_EQ(0u, s.DemandCount());
}

// --- detach edge: contribution returns to neutral -> resolved returns to base ---

TEST(EffectParamTargetStore, DetachReturnsResolvedToBase) {
    EffectParamTargetStore s = MakeStore();
    const std::vector<float> bases = DelayBases(10.0f, 0.0f, 1.0f, 1.0f);
    ASSERT_TRUE(s.SeedInstalledEffect(Scope::Voice, kVoice, kSlot,
                                      VN_EFFECT_FEEDBACK_DELAY, bases.data(),
                                      harken::kDelayParamCount));
    Sweep(s, kVoice, kSlot, harken::kDelayParamWet, {0.5f});
    EXPECT_FLOAT_EQ(0.5f, s.Reduce(Scope::Voice, kVoice, kSlot,
                                   harken::kDelayParamWet));

    // The last binding detaches: the next rebuild folds nothing, so the
    // contribution returns to the Multiply neutral and the resolved value
    // returns to the base alone.
    Sweep(s, kVoice, kSlot, harken::kDelayParamWet, {});
    EXPECT_FLOAT_EQ(1.0f, s.Reduce(Scope::Voice, kVoice, kSlot,
                                   harken::kDelayParamWet));
}

// --- publication bookkeeping ---

TEST(EffectParamTargetStore, FlushRetriesOnPushFailureThenCleansOnCommit) {
    EffectParamTargetStore s = MakeStore();
    const std::vector<float> bases = GainBases(-6.0f);
    ASSERT_TRUE(s.SeedInstalledEffect(Scope::Voice, kVoice, kSlot,
                                      VN_EFFECT_GAIN, bases.data(),
                                      harken::kGainParamCount));
    SweepAs(s, kVoice, kSlot, harken::kGainParamGainDb, VN_EFFECT_GAIN, {3.0f}); // -6 + 3 = -3

    // Push refused (queue full): the record stays dirty and retries — no loss.
    int emitted = 0;
    float got = 0.0f;
    s.FlushDirty([&](Scope, std::uint64_t, std::uint32_t, std::uint32_t,
                     float resolved) {
        ++emitted;
        got = resolved;
        return false; // queue full
    });
    EXPECT_EQ(1, emitted);
    EXPECT_FLOAT_EQ(-3.0f, got);
    EXPECT_EQ(1u, s.DemandCount()); // still dirty, retries next update

    // Push succeeds: the record adopts the value and cleans.
    int emitted2 = 0;
    s.FlushDirty([&](Scope, std::uint64_t, std::uint32_t, std::uint32_t, float) {
        ++emitted2;
        return true;
    });
    EXPECT_EQ(1, emitted2);
    EXPECT_EQ(0u, s.DemandCount());

    // A third flush with nothing changed emits nothing.
    int again = 0;
    s.FlushDirty([&](Scope, std::uint64_t, std::uint32_t, std::uint32_t, float) {
        ++again;
        return true;
    });
    EXPECT_EQ(0, again);
}

TEST(EffectParamTargetStore,
     ClaimedEqualReductionStaysDirtyUntilTheSliceOwnerCommits) {
    EffectParamTargetStore store = MakeStore();
    const std::vector<float> gain = GainBases(0.0f);
    ASSERT_TRUE(store.SeedInstalledEffect(
        Scope::Voice, kVoice, kSlot, VN_EFFECT_GAIN, gain.data(),
        harken::kGainParamCount));
    SweepAs(store, kVoice, kSlot, harken::kGainParamGainDb, VN_EFFECT_GAIN,
            {-3.0f});
    const harken::VariableSliceRef owner{0, 1};
    const std::uint32_t revision = store.TargetRevision(
        Scope::Voice, kVoice, kSlot, harken::kGainParamGainDb);
    ASSERT_TRUE(store.TryClaim(Scope::Voice, kVoice, kSlot,
                               harken::kGainParamGainDb, owner, revision));
    float frozenResolved = 0.0f;
    std::uint32_t frozenRevision = 0;
    std::uint32_t effectType = 0;
    ASSERT_TRUE(store.SnapshotForClaim(
        Scope::Voice, kVoice, kSlot, harken::kGainParamGainDb, owner,
        &frozenResolved, &frozenRevision, &effectType));
    EXPECT_FLOAT_EQ(-3.0f, frozenResolved);
    EXPECT_EQ(VN_EFFECT_GAIN, effectType);

    SweepAs(store, kVoice, kSlot, harken::kGainParamGainDb, VN_EFFECT_GAIN,
            {0.0f});
    std::uint32_t ordinaryFlushCalls = 0;
    store.FlushDirty([&](Scope, std::uint64_t, std::uint32_t,
                         std::uint32_t, float) {
        ++ordinaryFlushCalls;
        return true;
    });
    EXPECT_EQ(0u, ordinaryFlushCalls);
    EXPECT_TRUE(store.PendingPublication(
        Scope::Voice, kVoice, kSlot, harken::kGainParamGainDb));
    EXPECT_TRUE(store.ForeignClaim(
        Scope::Voice, kVoice, kSlot, harken::kGainParamGainDb));

    store.MarkSliceCommitted(Scope::Voice, kVoice, kSlot,
                             harken::kGainParamGainDb, owner,
                             frozenResolved);
    EXPECT_FALSE(store.ForeignClaim(
        Scope::Voice, kVoice, kSlot, harken::kGainParamGainDb));
    EXPECT_TRUE(store.PendingPublication(
        Scope::Voice, kVoice, kSlot, harken::kGainParamGainDb));
    std::uint32_t staged = 0;
    store.FlushDirty([&](Scope, std::uint64_t, std::uint32_t,
                         std::uint32_t, float resolved) {
        ++staged;
        EXPECT_FLOAT_EQ(0.0f, resolved);
        return true;
    });
    EXPECT_EQ(1u, staged);
}

TEST(EffectParamTargetStore, DroppedValuePreservesLastCommittedReference) {
    EffectParamTargetStore s = MakeStore();
    const std::vector<float> bases = DelayBases(10.0f, 0.0f, 1.0f, 1.0f);
    ASSERT_TRUE(s.SeedInstalledEffect(Scope::Voice, kVoice, kSlot,
                                      VN_EFFECT_FEEDBACK_DELAY, bases.data(),
                                      harken::kDelayParamCount));
    Sweep(s, kVoice, kSlot, harken::kDelayParamWet, {0.5f});
    EXPECT_EQ(1u, s.DemandCount());
    s.MarkDropped(Scope::Voice, kVoice, kSlot, harken::kDelayParamWet);
    EXPECT_EQ(0u, s.DemandCount());
    EXPECT_FLOAT_EQ(1.0f, s.LastCommitted(Scope::Voice, kVoice, kSlot,
                                           harken::kDelayParamWet));
}

TEST(EffectParamTargetStore, NoteDirectRetargetsBase) {
    EffectParamTargetStore s = MakeStore();
    const std::vector<float> bases = DelayBases(10.0f, 0.0f, 1.0f, 1.0f);
    ASSERT_TRUE(s.SeedInstalledEffect(Scope::Voice, kVoice, kSlot,
                                      VN_EFFECT_FEEDBACK_DELAY, bases.data(),
                                      harken::kDelayParamCount));
    // A direct set moves the base to 0.5 (published as 0.5 with neutral fold).
    s.NoteDirect(Scope::Voice, kVoice, kSlot, harken::kDelayParamWet, 0.5f, 0.5f);
    EXPECT_FLOAT_EQ(0.5f, s.Reduce(Scope::Voice, kVoice, kSlot,
                                   harken::kDelayParamWet));
    // A Multiply 0.5 binding now composes on the new base: 0.5 * 0.5 = 0.25.
    Sweep(s, kVoice, kSlot, harken::kDelayParamWet, {0.5f});
    EXPECT_FLOAT_EQ(0.25f, s.Reduce(Scope::Voice, kVoice, kSlot,
                                    harken::kDelayParamWet));
}

// --- lifecycle: replace, clear, object-gone ---

TEST(EffectParamTargetStore, InPlaceReplaceReseedsBase) {
    EffectParamTargetStore s = MakeStore();
    std::vector<float> bases = DelayBases(10.0f, 0.0f, 1.0f, 1.0f);
    ASSERT_TRUE(s.SeedInstalledEffect(Scope::Voice, kVoice, kSlot,
                                      VN_EFFECT_FEEDBACK_DELAY, bases.data(),
                                      harken::kDelayParamCount));
    bases = DelayBases(10.0f, 0.0f, 0.8f, 1.0f);
    ASSERT_TRUE(s.SeedInstalledEffect(Scope::Voice, kVoice, kSlot,
                                      VN_EFFECT_FEEDBACK_DELAY, bases.data(),
                                      harken::kDelayParamCount));
    EXPECT_FLOAT_EQ(0.8f, s.Reduce(Scope::Voice, kVoice, kSlot,
                                   harken::kDelayParamWet));
}

TEST(EffectParamTargetStore, ClearSlotAndObjectRemoveRecords) {
    EffectParamTargetStore s = MakeStore();
    const std::vector<float> bases = DelayBases(10.0f, 0.0f, 1.0f, 1.0f);
    ASSERT_TRUE(s.SeedInstalledEffect(Scope::Voice, kVoice, kSlot,
                                      VN_EFFECT_FEEDBACK_DELAY, bases.data(),
                                      harken::kDelayParamCount));
    Sweep(s, kVoice, kSlot, harken::kDelayParamWet, {0.5f});
    EXPECT_EQ(1u, s.DemandCount());

    s.ClearSlot(Scope::Voice, kVoice, kSlot);
    EXPECT_EQ(0.0f, s.Reduce(Scope::Voice, kVoice, kSlot,
                             harken::kDelayParamWet));
    EXPECT_EQ(0u, s.DemandCount()); // a dirty record's demand is released

    // Re-seed on another object, then clear the whole object.
    ASSERT_TRUE(s.SeedInstalledEffect(Scope::Voice, 2, kSlot,
                                      VN_EFFECT_FEEDBACK_DELAY, bases.data(),
                                      harken::kDelayParamCount));
    EXPECT_FLOAT_EQ(1.0f, s.Reduce(Scope::Voice, 2, kSlot,
                                   harken::kDelayParamWet));
    s.ClearObject(Scope::Voice, 2);
    EXPECT_EQ(0.0f, s.Reduce(Scope::Voice, 2, kSlot, harken::kDelayParamWet));
}

TEST(EffectParamTargetStore, RefusedReplacePreservesIncumbent) {
    // Capacity exactly holds one delay (4 params). A replace that cannot fit
    // (paramCount > free + incumbent) must refuse BEFORE clearing the incumbent,
    // leaving the existing records fully intact (all-or-nothing on replace).
    EffectParamTargetStore small(kRate, /*capacity=*/4);
    const std::vector<float> delay = DelayBases(10.0f, 0.0f, 1.0f, 1.0f);
    ASSERT_TRUE(small.SeedInstalledEffect(Scope::Voice, kVoice, kSlot,
                                          VN_EFFECT_FEEDBACK_DELAY, delay.data(),
                                          harken::kDelayParamCount));
    // A tone (7 params) into the SAME slot: 7 > (free 0 + incumbent 4) = 4 ->
    // refused. The delay records must survive untouched.
    const std::vector<float> tone = ToneBases();
    EXPECT_FALSE(small.SeedInstalledEffect(Scope::Voice, kVoice, kSlot,
                                           VN_EFFECT_THREE_BAND_TONE,
                                           tone.data(),
                                           harken::kToneParamCount));
    EXPECT_FLOAT_EQ(1.0f, small.Reduce(Scope::Voice, kVoice, kSlot,
                                       harken::kDelayParamWet));
    // The incumbent is still bindable as its original type.
    EffectParamLocator loc;
    loc.effectType = VN_EFFECT_FEEDBACK_DELAY;
    loc.slot = kSlot;
    loc.paramIndex = harken::kDelayParamWet;
    EXPECT_TRUE(small.CanBind(Scope::Voice, kVoice, loc));
}

TEST(EffectParamTargetStore, SeedRefusesWhenCapacityExhausted) {
    // Capacity 2 holds one gain (1 param) plus room for one more; a delay (4
    // params) cannot fit and is refused all-or-nothing.
    EffectParamTargetStore small(kRate, /*capacity=*/2);
    const std::vector<float> gain = GainBases(0.0f);
    ASSERT_TRUE(small.SeedInstalledEffect(Scope::Voice, kVoice, kSlot,
                                          VN_EFFECT_GAIN, gain.data(),
                                          harken::kGainParamCount));
    const std::vector<float> delay = DelayBases(10.0f, 0.0f, 1.0f, 1.0f);
    EXPECT_FALSE(small.SeedInstalledEffect(Scope::Voice, 2, kSlot,
                                           VN_EFFECT_FEEDBACK_DELAY,
                                           delay.data(),
                                           harken::kDelayParamCount));
    // The gain record is untouched by the refused seed.
    EXPECT_FLOAT_EQ(0.0f, small.Reduce(Scope::Voice, kVoice, kSlot,
                                       harken::kGainParamGainDb));
}

TEST(EffectParamTargetStore, SeedPreflightAndInjectedFailureArePure) {
    EffectParamTargetStore store(kRate, /*capacity=*/2);
    EXPECT_TRUE(store.CanSeedFreshEffects(2));
    store.SetSeedFailureForTests(true);
    EXPECT_FALSE(store.CanSeedFreshEffects(1));
    const std::vector<float> gain = GainBases(-6.0f);
    EXPECT_FALSE(store.SeedInstalledEffect(
        Scope::Voice, kVoice, kSlot, VN_EFFECT_GAIN, gain.data(),
        harken::kGainParamCount));
    EXPECT_EQ(0u, store.LiveRecordCount());
    store.SetSeedFailureForTests(false);
    EXPECT_TRUE(store.SeedInstalledEffect(
        Scope::Voice, kVoice, kSlot, VN_EFFECT_GAIN, gain.data(),
        harken::kGainParamCount));
}

TEST(EffectParamTargetStore,
     PreparedSeedCommitIsNoexceptAndIgnoresLaterFailureInjection) {
    EffectParamTargetStore store(kRate, /*capacity=*/2);
    const std::vector<float> oldGain = GainBases(-3.0f);
    ASSERT_TRUE(store.SeedInstalledEffect(
        Scope::Bus, 7, kSlot, VN_EFFECT_GAIN, oldGain.data(),
        harken::kGainParamCount));
    const std::vector<float> newGain = GainBases(-9.0f);
    EffectParamTargetStore::PreparedSeedToken token;
    ASSERT_TRUE(store.PrepareInstalledEffectSeed(
        Scope::Bus, 7, kSlot, VN_EFFECT_GAIN, newGain.data(),
        harken::kGainParamCount, &token));
    ASSERT_TRUE(token.IsPrepared());
    static_assert(noexcept(
        store.CommitPreparedInstalledEffectSeed(&token)));

    store.SetSeedFailureForTests(true);
    store.CommitPreparedInstalledEffectSeed(&token);

    EXPECT_FALSE(token.IsPrepared());
    EXPECT_EQ(1u, store.LiveRecordCount());
    EXPECT_FLOAT_EQ(
        -9.0f,
        store.Reduce(Scope::Bus, 7, kSlot,
                     harken::kGainParamGainDb));
}

TEST(EffectParamTargetStore,
     PreparedToneSeedCanShrinkToGainWithoutReservationLeak) {
    EffectParamTargetStore store(
        kRate, /*capacity=*/harken::kToneParamCount);
    const std::vector<float> tone = ToneBases();
    EffectParamTargetStore::PreparedSeedToken token;
    ASSERT_TRUE(store.PrepareInstalledEffectSeed(
        Scope::Voice, kVoice, kSlot, VN_EFFECT_THREE_BAND_TONE,
        tone.data(), harken::kToneParamCount, &token));
    ASSERT_TRUE(token.IsPrepared());
    ASSERT_EQ(harken::kToneParamCount,
              store.ReservedRecordCountForTests());

    const std::vector<float> gain = GainBases(-12.0f);
    ASSERT_TRUE(store.ReplacePreparedInstalledEffectSeed(
        &token, VN_EFFECT_GAIN, gain.data(), harken::kGainParamCount));
    EXPECT_TRUE(token.IsPrepared());
    EXPECT_EQ(harken::kGainParamCount,
              store.ReservedRecordCountForTests());
    EXPECT_EQ(0u, store.LiveRecordCount());

    store.CommitPreparedInstalledEffectSeed(&token);

    EXPECT_FALSE(token.IsPrepared());
    EXPECT_EQ(0u, store.ReservedRecordCountForTests());
    EXPECT_EQ(harken::kGainParamCount, store.LiveRecordCount());
    EXPECT_FLOAT_EQ(
        -12.0f,
        store.Reduce(Scope::Voice, kVoice, kSlot,
                     harken::kGainParamGainDb));
    EffectParamLocator gainLocator;
    gainLocator.effectType = VN_EFFECT_GAIN;
    gainLocator.slot = kSlot;
    gainLocator.paramIndex = harken::kGainParamGainDb;
    EXPECT_TRUE(store.CanBind(
        Scope::Voice, kVoice, gainLocator));
    EffectParamLocator toneLocator;
    toneLocator.effectType = VN_EFFECT_THREE_BAND_TONE;
    toneLocator.slot = kSlot;
    toneLocator.paramIndex = harken::kToneParamPeakGainDb;
    EXPECT_FALSE(store.CanBind(
        Scope::Voice, kVoice, toneLocator));
}

TEST(EffectParamTargetStore, PrimeContributionAdoptsFirstBlockCommit) {
    EffectParamTargetStore store = MakeStore();
    const std::vector<float> gain = GainBases(-6.0f);
    ASSERT_TRUE(store.SeedInstalledEffect(
        Scope::Voice, kVoice, kSlot, VN_EFFECT_GAIN, gain.data(),
        harken::kGainParamCount));
    float resolved = 0.0f;
    ASSERT_TRUE(store.PrimeContribution(
        Scope::Voice, kVoice, kSlot, harken::kGainParamGainDb,
        VN_EFFECT_GAIN, 3.0f, &resolved));
    EXPECT_FLOAT_EQ(-3.0f, resolved);
    EXPECT_FLOAT_EQ(-3.0f,
                    store.LastCommitted(Scope::Voice, kVoice, kSlot,
                                        harken::kGainParamGainDb));
    EXPECT_EQ(0u, store.DemandCount());
}

TEST(EffectParamTargetStore,
     StructuralPrepareAndVariableClaimGateEachOtherAcrossTheWholeSlot) {
    EffectParamTargetStore store(kRate, /*capacity=*/4,
                                 /*retiringCapacity=*/4);
    constexpr std::uint64_t kBus = 0;
    constexpr std::uint32_t kGeneration = 5;
    const std::vector<float> delay =
        DelayBases(10.0f, 0.25f, 0.8f, 1.0f);
    ASSERT_TRUE(store.SeedInstalledEffect(
        Scope::Bus, kBus, kGeneration, kSlot, VN_EFFECT_FEEDBACK_DELAY,
        delay.data(), harken::kDelayParamCount));
    std::uint32_t dryRevision = 0;
    std::uint32_t wetRevision = 0;
    ASSERT_TRUE(store.ExactTargetRevision(
        Scope::Bus, kBus, kGeneration, kSlot, harken::kDelayParamDry,
        VN_EFFECT_FEEDBACK_DELAY, &dryRevision));
    ASSERT_TRUE(store.ExactTargetRevision(
        Scope::Bus, kBus, kGeneration, kSlot, harken::kDelayParamWet,
        VN_EFFECT_FEEDBACK_DELAY, &wetRevision));
    const float dryCommitted = store.LastCommitted(
        Scope::Bus, kBus, kSlot, harken::kDelayParamDry);
    const float wetCommitted = store.LastCommitted(
        Scope::Bus, kBus, kSlot, harken::kDelayParamWet);

    EffectParamTargetStore::PreparedSeedToken prepared;
    const std::vector<float> replacement =
        DelayBases(10.0f, 0.25f, 0.5f, 1.0f);
    ASSERT_TRUE(store.PrepareInstalledEffectSeed(
        Scope::Bus, kBus, kGeneration, kSlot, VN_EFFECT_FEEDBACK_DELAY,
        replacement.data(), harken::kDelayParamCount, &prepared));
    EXPECT_TRUE(store.ForeignClaim(Scope::Bus, kBus, kGeneration, kSlot,
                                   harken::kDelayParamDry));
    EXPECT_FALSE(store.TryClaim(
        Scope::Bus, kBus, kGeneration, kSlot, harken::kDelayParamDry,
        harken::VariableSliceRef{0, 1}, dryRevision));
    store.CancelPreparedInstalledEffectSeed(&prepared);
    EXPECT_EQ(0u, store.ReservedRecordCountForTests());

    const harken::VariableSliceRef owner{1, 1};
    ASSERT_TRUE(store.TryClaim(Scope::Bus, kBus, kGeneration, kSlot,
                               harken::kDelayParamDry, owner, dryRevision));
    EXPECT_FALSE(store.PrepareInstalledEffectSeed(
        Scope::Bus, kBus, kGeneration, kSlot, VN_EFFECT_FEEDBACK_DELAY,
        replacement.data(), harken::kDelayParamCount, &prepared));
    EXPECT_EQ(0u, store.ReservedRecordCountForTests());
    store.ReleaseClaim(Scope::Bus, kBus, kGeneration, kSlot,
                       harken::kDelayParamDry, owner);
    std::uint32_t observedRevision = 0;
    ASSERT_TRUE(store.ExactTargetRevision(
        Scope::Bus, kBus, kGeneration, kSlot, harken::kDelayParamDry,
        VN_EFFECT_FEEDBACK_DELAY, &observedRevision));
    EXPECT_EQ(dryRevision, observedRevision);
    ASSERT_TRUE(store.ExactTargetRevision(
        Scope::Bus, kBus, kGeneration, kSlot, harken::kDelayParamWet,
        VN_EFFECT_FEEDBACK_DELAY, &observedRevision));
    EXPECT_EQ(wetRevision, observedRevision);
    EXPECT_FLOAT_EQ(dryCommitted,
                    store.LastCommitted(Scope::Bus, kBus, kSlot,
                                        harken::kDelayParamDry));
    EXPECT_FLOAT_EQ(wetCommitted,
                    store.LastCommitted(Scope::Bus, kBus, kSlot,
                                        harken::kDelayParamWet));
    ASSERT_TRUE(store.PrepareInstalledEffectSeed(
        Scope::Bus, kBus, kGeneration, kSlot, VN_EFFECT_FEEDBACK_DELAY,
        replacement.data(), harken::kDelayParamCount, &prepared));
    store.CancelPreparedInstalledEffectSeed(&prepared);
}

TEST(EffectParamTargetStore,
     ShrinkingStructuralPrepareAlsoGatesDroppedSiblingParameters) {
    EffectParamTargetStore store(kRate, /*capacity=*/4,
                                 /*retiringCapacity=*/4);
    constexpr std::uint64_t kBus = 0;
    constexpr std::uint32_t kGeneration = 6;
    const std::vector<float> delay =
        DelayBases(10.0f, 0.25f, 0.8f, 1.0f);
    ASSERT_TRUE(store.SeedInstalledEffect(
        Scope::Bus, kBus, kGeneration, kSlot, VN_EFFECT_FEEDBACK_DELAY,
        delay.data(), harken::kDelayParamCount));
    std::uint32_t dryRevision = 0;
    ASSERT_TRUE(store.ExactTargetRevision(
        Scope::Bus, kBus, kGeneration, kSlot, harken::kDelayParamDry,
        VN_EFFECT_FEEDBACK_DELAY, &dryRevision));
    const float dryCommitted = store.LastCommitted(
        Scope::Bus, kBus, kSlot, harken::kDelayParamDry);
    const harken::VariableSliceRef owner{2, 1};
    ASSERT_TRUE(store.TryClaim(Scope::Bus, kBus, kGeneration, kSlot,
                               harken::kDelayParamDry, owner, dryRevision));

    const std::vector<float> gain = GainBases(-9.0f);
    EffectParamTargetStore::PreparedSeedToken prepared;
    EXPECT_FALSE(store.PrepareInstalledEffectSeed(
        Scope::Bus, kBus, kGeneration, kSlot, VN_EFFECT_GAIN, gain.data(),
        harken::kGainParamCount, &prepared));
    EXPECT_FALSE(prepared.IsPrepared());
    EXPECT_EQ(0u, store.ReservedRecordCountForTests());
    EXPECT_EQ(harken::kDelayParamCount, store.LiveRecordCount());
    EXPECT_TRUE(store.ForeignClaim(Scope::Bus, kBus, kGeneration, kSlot,
                                   harken::kDelayParamDry));
    float claimedResolved = 0.0f;
    std::uint32_t claimedRevision = 0;
    std::uint32_t claimedEffectType = 0;
    ASSERT_TRUE(store.SnapshotForClaim(
        Scope::Bus, kBus, kGeneration, kSlot, harken::kDelayParamDry, owner,
        &claimedResolved, &claimedRevision, &claimedEffectType));
    EXPECT_FLOAT_EQ(delay[harken::kDelayParamDry], claimedResolved);
    EXPECT_EQ(dryRevision, claimedRevision);
    EXPECT_EQ(VN_EFFECT_FEEDBACK_DELAY, claimedEffectType);
    store.ReleaseClaim(Scope::Bus, kBus, kGeneration, kSlot,
                       harken::kDelayParamDry, owner);

    ASSERT_TRUE(store.PrepareInstalledEffectSeed(
        Scope::Bus, kBus, kGeneration, kSlot, VN_EFFECT_GAIN, gain.data(),
        harken::kGainParamCount, &prepared));
    EXPECT_TRUE(prepared.IsPrepared());
    EXPECT_EQ(harken::kDelayParamCount,
              store.ReservedRecordCountForTests());
    EXPECT_TRUE(store.ForeignClaim(Scope::Bus, kBus, kGeneration, kSlot,
                                   harken::kDelayParamDry));
    EXPECT_FALSE(store.TryClaim(
        Scope::Bus, kBus, kGeneration, kSlot, harken::kDelayParamDry,
        harken::VariableSliceRef{3, 1}, dryRevision));
    store.CancelPreparedInstalledEffectSeed(&prepared);
    EXPECT_EQ(0u, store.ReservedRecordCountForTests());
    EXPECT_EQ(harken::kDelayParamCount, store.LiveRecordCount());
    std::uint32_t observedRevision = 0;
    ASSERT_TRUE(store.ExactTargetRevision(
        Scope::Bus, kBus, kGeneration, kSlot, harken::kDelayParamDry,
        VN_EFFECT_FEEDBACK_DELAY, &observedRevision));
    EXPECT_EQ(dryRevision, observedRevision);
    EXPECT_FLOAT_EQ(dryCommitted,
                    store.LastCommitted(Scope::Bus, kBus, kSlot,
                                        harken::kDelayParamDry));
}

TEST(EffectParamTargetStore,
     ExactCapacityRetirementAndSameGenerationReseedNeverAlias) {
    EffectParamTargetStore store(kRate, /*capacity=*/harken::kToneParamCount,
                                 /*retiringCapacity=*/harken::kToneParamCount);
    constexpr std::uint64_t kBus = 0;
    constexpr std::uint32_t kGeneration = 9;
    const std::vector<float> first = ToneBases();
    ASSERT_TRUE(store.SeedInstalledEffect(
        Scope::Bus, kBus, kGeneration, kSlot, VN_EFFECT_THREE_BAND_TONE,
        first.data(), harken::kToneParamCount));
    const harken::VariableSliceRef owner{7, 3};
    std::uint32_t oldRevisions[harken::kToneParamCount]{};
    for (std::uint32_t param = 0; param < harken::kToneParamCount; ++param) {
        ASSERT_TRUE(store.ExactTargetRevision(
            Scope::Bus, kBus, kGeneration, kSlot, param,
            VN_EFFECT_THREE_BAND_TONE, &oldRevisions[param]));
        ASSERT_TRUE(store.TryClaim(Scope::Bus, kBus, kGeneration, kSlot,
                                   param, owner, oldRevisions[param]));
    }

    store.ClearSlot(Scope::Bus, kBus, kGeneration, kSlot);
    EXPECT_EQ(0u, store.LiveRecordCount());
    ASSERT_EQ(harken::kToneParamCount,
              store.RetiringRecordCountForTests());
    for (std::uint32_t param = 0; param < harken::kToneParamCount; ++param) {
        float resolved = 0.0f;
        std::uint32_t revision = 0;
        std::uint32_t effectType = 0;
        ASSERT_TRUE(store.SnapshotForClaim(
            Scope::Bus, kBus, kGeneration, kSlot, param, owner, &resolved,
            &revision, &effectType));
        EXPECT_EQ(oldRevisions[param], revision);
        EXPECT_EQ(VN_EFFECT_THREE_BAND_TONE, effectType);
        store.ReleaseClaim(Scope::Bus, kBus, kGeneration, kSlot, param,
                           owner);
        EXPECT_EQ(harken::kToneParamCount - param - 1u,
                  store.RetiringRecordCountForTests());
    }

    std::vector<float> second = ToneBases();
    second[harken::kToneParamPeakGainDb] = 3.0f;
    ASSERT_TRUE(store.SeedInstalledEffect(
        Scope::Bus, kBus, kGeneration, kSlot, VN_EFFECT_THREE_BAND_TONE,
        second.data(), harken::kToneParamCount));
    for (std::uint32_t param = 0; param < harken::kToneParamCount; ++param) {
        float resolved = 0.0f;
        std::uint32_t snapshotRevision = 0;
        std::uint32_t effectType = 0;
        EXPECT_FALSE(store.SnapshotForClaim(
            Scope::Bus, kBus, kGeneration, kSlot, param, owner, &resolved,
            &snapshotRevision, &effectType));
        std::uint32_t newRevision = 0;
        ASSERT_TRUE(store.ExactTargetRevision(
            Scope::Bus, kBus, kGeneration, kSlot, param,
            VN_EFFECT_THREE_BAND_TONE, &newRevision));
        EXPECT_GT(newRevision, oldRevisions[param]);
        EXPECT_FALSE(store.TryClaim(Scope::Bus, kBus, kGeneration, kSlot,
                                    param, owner, oldRevisions[param]));
    }
}

TEST(EffectParamTargetStore,
     RetiringOwnerPinsOnlyItsExactGenerationAndRevisionsNeverAlias) {
    EffectParamTargetStore store(kRate, /*capacity=*/1,
                                 /*retiringCapacity=*/1);
    constexpr std::uint64_t kBus = 0;
    constexpr std::uint32_t kOldGeneration = 10;
    constexpr std::uint32_t kNewGeneration = 11;
    const std::vector<float> oldGain = GainBases(-6.0f);
    ASSERT_TRUE(store.SeedInstalledEffect(
        Scope::Bus, kBus, kOldGeneration, kSlot, VN_EFFECT_GAIN,
        oldGain.data(), harken::kGainParamCount));
    std::uint32_t oldRevision = 0;
    ASSERT_TRUE(store.ExactTargetRevision(
        Scope::Bus, kBus, kOldGeneration, kSlot,
        harken::kGainParamGainDb, VN_EFFECT_GAIN, &oldRevision));
    const harken::VariableSliceRef oldOwner{0, 1};
    ASSERT_TRUE(store.TryClaim(
        Scope::Bus, kBus, kOldGeneration, kSlot,
        harken::kGainParamGainDb, oldOwner, oldRevision));

    store.ClearSlot(Scope::Bus, kBus, kOldGeneration, kSlot);
    EXPECT_EQ(0u, store.LiveRecordCount());
    ASSERT_EQ(1u, store.RetiringRecordCountForTests());
    EXPECT_FALSE(store.CanSeedInstalledEffect(
        Scope::Bus, kBus, kOldGeneration, kSlot, VN_EFFECT_GAIN,
        harken::kGainParamCount));

    const std::vector<float> newGain = GainBases(-3.0f);
    ASSERT_TRUE(store.SeedInstalledEffect(
        Scope::Bus, kBus, kNewGeneration, kSlot, VN_EFFECT_GAIN,
        newGain.data(), harken::kGainParamCount));
    std::uint32_t newRevision = 0;
    ASSERT_TRUE(store.ExactTargetRevision(
        Scope::Bus, kBus, kNewGeneration, kSlot,
        harken::kGainParamGainDb, VN_EFFECT_GAIN, &newRevision));
    EXPECT_GT(newRevision, oldRevision);
    const harken::VariableSliceRef newOwner{1, 1};
    ASSERT_TRUE(store.TryClaim(
        Scope::Bus, kBus, kNewGeneration, kSlot,
        harken::kGainParamGainDb, newOwner, newRevision));

    float resolved = 0.0f;
    std::uint32_t snapshotRevision = 0;
    std::uint32_t effectType = 0;
    ASSERT_TRUE(store.SnapshotForClaim(
        Scope::Bus, kBus, kOldGeneration, kSlot,
        harken::kGainParamGainDb, oldOwner, &resolved, &snapshotRevision,
        &effectType));
    EXPECT_FLOAT_EQ(-6.0f, resolved);
    EXPECT_EQ(oldRevision, snapshotRevision);
    EXPECT_EQ(VN_EFFECT_GAIN, effectType);
    store.ReleaseClaim(Scope::Bus, kBus, kOldGeneration, kSlot,
                       harken::kGainParamGainDb, oldOwner);
    EXPECT_EQ(0u, store.RetiringRecordCountForTests());
    EXPECT_TRUE(store.ForeignClaim(Scope::Bus, kBus, kNewGeneration, kSlot,
                                   harken::kGainParamGainDb));

    store.ClearSlot(Scope::Bus, kBus, kNewGeneration, kSlot);
    ASSERT_EQ(1u, store.RetiringRecordCountForTests());
    constexpr std::uint32_t kNewestGeneration = 12;
    const std::vector<float> newestGain = GainBases(0.0f);
    ASSERT_TRUE(store.SeedInstalledEffect(
        Scope::Bus, kBus, kNewestGeneration, kSlot, VN_EFFECT_GAIN,
        newestGain.data(), harken::kGainParamCount));
    std::uint32_t newestRevision = 0;
    ASSERT_TRUE(store.ExactTargetRevision(
        Scope::Bus, kBus, kNewestGeneration, kSlot,
        harken::kGainParamGainDb, VN_EFFECT_GAIN, &newestRevision));
    EXPECT_GT(newestRevision, newRevision);
    const harken::VariableSliceRef newestOwner{2, 1};
    ASSERT_TRUE(store.TryClaim(
        Scope::Bus, kBus, kNewestGeneration, kSlot,
        harken::kGainParamGainDb, newestOwner, newestRevision));
    store.MarkSliceCommitted(Scope::Bus, kBus, kNewGeneration, kSlot,
                             harken::kGainParamGainDb, newOwner, -3.0f);
    EXPECT_EQ(0u, store.RetiringRecordCountForTests());
    EXPECT_TRUE(store.ForeignClaim(Scope::Bus, kBus, kNewestGeneration, kSlot,
                                   harken::kGainParamGainDb));
    store.ReleaseClaim(Scope::Bus, kBus, kNewestGeneration, kSlot,
                       harken::kGainParamGainDb, newestOwner);
}

TEST(EffectParamTargetStore, MaximumRevisionIsValidOnceThenExhaustsCleanly) {
    EffectParamTargetStore store(kRate, /*capacity=*/2);
    store.SetNextRevisionForTests(
        (std::numeric_limits<std::uint32_t>::max)());
    const std::vector<float> gain = GainBases(-6.0f);
    ASSERT_TRUE(store.SeedInstalledEffect(
        Scope::Bus, 0, 1, 0, VN_EFFECT_GAIN, gain.data(),
        harken::kGainParamCount));
    std::uint32_t revision = 0;
    ASSERT_TRUE(store.ExactTargetRevision(
        Scope::Bus, 0, 1, 0, harken::kGainParamGainDb, VN_EFFECT_GAIN,
        &revision));
    EXPECT_EQ((std::numeric_limits<std::uint32_t>::max)(), revision);
    EXPECT_FALSE(store.CanSeedFreshEffects(1));
    EXPECT_FALSE(store.SeedInstalledEffect(
        Scope::Bus, 1, 1, 0, VN_EFFECT_GAIN, gain.data(),
        harken::kGainParamCount));
    EXPECT_EQ(1u, store.LiveRecordCount());
}

} // namespace
