// Effect-parameter binding at the BindingTable + EffectParamTargetStore seam,
// without an engine: registration validates the packed locator against the
// metadata authority (None-domain refused at register), a voice apply instance
// and a bus attachment each fold their Variable-through-curve contribution into
// the store, the interpose query reports a bound parameter, and BOTH removal
// paths (explicit clear and voice-end) return the resolved value to its base
// (the detach edge). Pure logic-thread state; no audio, no commands.
#include "Common/ObjectIdTestSupport.h"
#include <harken/Harken.h>

#include "harken/core/EngineConfig.h"
#include "harken/core/LogRing.h"
#include "harken/param/BindingTable.h"
#include "harken/param/EffectParamLocator.h"
#include "harken/param/EffectParamTargetStore.h"
#include "harken/param/ParamSystem.h"
#include "harken/render/BuiltinEffects.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using Scope = harken::EffectParamTargetStore::Scope;

constexpr std::uint32_t kRate = 48000;
constexpr VnVariableId kVar = harkentest::ObjectId(7);
constexpr harken::BindingId kVoiceBinding = harkentest::ObjectId(100);
constexpr harken::BindingId kBusBinding = harkentest::ObjectId(200);
constexpr VnVoiceHandle kRender = 1;
constexpr std::uint32_t kBusIndex = 0;
constexpr std::uint32_t kSlot = 0;

// A two-point linear curve mapping variable [0,1] -> contribution [y0,y1].
VnResult RegisterLinear(harken::BindingTable& bt, harken::LogRing& log,
                        harken::BindingId id, std::uint32_t scopeKind,
                        std::uint32_t targetId, float y0, float y1) {
    const harken::CurvePoint pts[] = {{0.0f, y0}, {1.0f, y1}};
    const harken::CurveHandlePair handles[] = {{pts[0].x, pts[0].y, pts[1].x, pts[1].y}};
    return bt.RegisterBinding(id, kVar, scopeKind, targetId, pts, handles, 2,
                              /*transitionMs=*/0, &log);
}

std::uint32_t GainLocator() {
    return harken::PackEffectParamLocator(VN_EFFECT_GAIN, kSlot,
                                         harken::kGainParamGainDb);
}

struct Fixture {
    harken::EngineConfig config;
    harken::ParamSystem params;
    harken::BindingTable bindings;
    harken::EffectParamTargetStore store;
    harken::LogRing log;
    Fixture()
        : params(config),
          bindings(config.maxBindings, config.maxBindingAttachments,
                   config.maxBuses),
          store(kRate, /*capacity=*/64) {}
};

// --- registration validation via the metadata authority ---

TEST(EffectParamBindingTable, RegisterAcceptsBindableParamRefusesNoneDomain) {
    Fixture f;
    // gainDb is an AddDb parameter: registration accepts it.
    EXPECT_EQ(VN_OK, RegisterLinear(f.bindings, f.log, kVoiceBinding,
                                    VN_TARGET_SCOPE_VOICE_EFFECT_PARAM,
                                    GainLocator(), 0.0f, 6.0f));
    // delay feedback is a None-domain parameter: registration refuses it
    // (fail-early, before any attach), zero side effects.
    const std::uint32_t noneLoc = harken::PackEffectParamLocator(
        VN_EFFECT_FEEDBACK_DELAY, kSlot, harken::kDelayParamFeedback);
    EXPECT_EQ(VN_INVALID_ARG,
              RegisterLinear(f.bindings, f.log, harkentest::ObjectId(101),
                             VN_TARGET_SCOPE_VOICE_EFFECT_PARAM, noneLoc, 0.0f,
                             0.5f));
    // An out-of-range parameter index for the type is refused too.
    const std::uint32_t badIdx = harken::PackEffectParamLocator(
        VN_EFFECT_GAIN, kSlot, harken::kGainParamCount);
    EXPECT_EQ(VN_INVALID_ARG,
              RegisterLinear(f.bindings, f.log, harkentest::ObjectId(102),
                             VN_TARGET_SCOPE_VOICE_EFFECT_PARAM, badIdx, 0.0f,
                             1.0f));
}

// --- voice apply: fold onto the seeded base ---

TEST(EffectParamBindingTable, VoiceApplyFoldsContributionOntoBase) {
    Fixture f;
    ASSERT_EQ(VN_OK, RegisterLinear(f.bindings, f.log, kVoiceBinding,
                                    VN_TARGET_SCOPE_VOICE_EFFECT_PARAM,
                                    GainLocator(), 0.0f, 6.0f));
    const std::vector<float> bases = {-6.0f};
    ASSERT_TRUE(f.store.SeedInstalledEffect(Scope::Voice, kRender, kSlot,
                                            VN_EFFECT_GAIN, bases.data(),
                                            harken::kGainParamCount));
    ASSERT_EQ(VN_OK, f.bindings.ApplyVoiceEffectInstance(kVoiceBinding, kRender,
                                                         &f.log));
    EXPECT_TRUE(f.bindings.HasEffectParamBinding(Scope::Voice, kRender, kSlot,
                                                 harken::kGainParamGainDb,
                                                 VN_EFFECT_GAIN));
    // A mismatched installed type does not read as bound (interpose guard).
    EXPECT_FALSE(f.bindings.HasEffectParamBinding(Scope::Voice, kRender, kSlot,
                                                  harken::kGainParamGainDb,
                                                  VN_EFFECT_FEEDBACK_DELAY));

    ASSERT_EQ(VN_OK, f.params.SetValue(kVar, 1.0f)); // curve -> +6 dB
    f.bindings.EvaluateEffectParamBindings(f.params, f.store, nullptr);
    // base -6 dB + contribution +6 dB = 0 dB.
    EXPECT_FLOAT_EQ(0.0f, f.store.Reduce(Scope::Voice, kRender, kSlot,
                                         harken::kGainParamGainDb));
}

// --- detach edge: BOTH removal paths return resolved to base ---

TEST(EffectParamBindingTable, ExplicitClearReturnsResolvedToBase) {
    Fixture f;
    ASSERT_EQ(VN_OK, RegisterLinear(f.bindings, f.log, kVoiceBinding,
                                    VN_TARGET_SCOPE_VOICE_EFFECT_PARAM,
                                    GainLocator(), 0.0f, 6.0f));
    const std::vector<float> bases = {-6.0f};
    ASSERT_TRUE(f.store.SeedInstalledEffect(Scope::Voice, kRender, kSlot,
                                            VN_EFFECT_GAIN, bases.data(),
                                            harken::kGainParamCount));
    ASSERT_EQ(VN_OK, f.bindings.ApplyVoiceEffectInstance(kVoiceBinding, kRender,
                                                         &f.log));
    ASSERT_EQ(VN_OK, f.params.SetValue(kVar, 1.0f));
    f.bindings.EvaluateEffectParamBindings(f.params, f.store, nullptr);
    ASSERT_FLOAT_EQ(0.0f, f.store.Reduce(Scope::Voice, kRender, kSlot,
                                         harken::kGainParamGainDb));

    // Explicit detach: the next sweep folds nothing, so the resolved value
    // returns to the base.
    EXPECT_TRUE(f.bindings.ClearVoiceEffectInstance(kRender, kVoiceBinding));
    EXPECT_FALSE(f.bindings.HasEffectParamBinding(Scope::Voice, kRender, kSlot,
                                                  harken::kGainParamGainDb,
                                                  VN_EFFECT_GAIN));
    f.bindings.EvaluateEffectParamBindings(f.params, f.store, nullptr);
    EXPECT_FLOAT_EQ(-6.0f, f.store.Reduce(Scope::Voice, kRender, kSlot,
                                          harken::kGainParamGainDb));
}

TEST(EffectParamBindingTable, VoiceEndRemovalReturnsResolvedToBase) {
    Fixture f;
    ASSERT_EQ(VN_OK, RegisterLinear(f.bindings, f.log, kVoiceBinding,
                                    VN_TARGET_SCOPE_VOICE_EFFECT_PARAM,
                                    GainLocator(), 0.0f, 6.0f));
    const std::vector<float> bases = {-6.0f};
    ASSERT_TRUE(f.store.SeedInstalledEffect(Scope::Voice, kRender, kSlot,
                                            VN_EFFECT_GAIN, bases.data(),
                                            harken::kGainParamCount));
    ASSERT_EQ(VN_OK, f.bindings.ApplyVoiceEffectInstance(kVoiceBinding, kRender,
                                                         &f.log));
    ASSERT_EQ(VN_OK, f.params.SetValue(kVar, 1.0f));
    f.bindings.EvaluateEffectParamBindings(f.params, f.store, nullptr);
    ASSERT_FLOAT_EQ(0.0f, f.store.Reduce(Scope::Voice, kRender, kSlot,
                                         harken::kGainParamGainDb));

    // Voice-end reclamation removes all of the voice's instances; the store's
    // records are cleared beside this in the engine, but the sweep alone must
    // already return the (still-installed) record to its base.
    f.bindings.RemoveVoiceEffectInstances(kRender);
    f.bindings.EvaluateEffectParamBindings(f.params, f.store, nullptr);
    EXPECT_FLOAT_EQ(-6.0f, f.store.Reduce(Scope::Voice, kRender, kSlot,
                                          harken::kGainParamGainDb));
}

// --- duplicate apply ---

TEST(EffectParamBindingTable, DuplicateVoiceApplyRefuses) {
    Fixture f;
    ASSERT_EQ(VN_OK, RegisterLinear(f.bindings, f.log, kVoiceBinding,
                                    VN_TARGET_SCOPE_VOICE_EFFECT_PARAM,
                                    GainLocator(), 0.0f, 6.0f));
    ASSERT_EQ(VN_OK, f.bindings.ApplyVoiceEffectInstance(kVoiceBinding, kRender,
                                                         &f.log));
    EXPECT_EQ(VN_IN_USE, f.bindings.ApplyVoiceEffectInstance(kVoiceBinding,
                                                             kRender, &f.log));
}

TEST(EffectParamBindingTable, PendingReservationActivatesAtomically) {
    Fixture f;
    ASSERT_EQ(VN_OK, RegisterLinear(f.bindings, f.log, kVoiceBinding,
                                    VN_TARGET_SCOPE_VOICE_EFFECT_PARAM,
                                    GainLocator(), 0.0f, 6.0f));
    const harken::BindingId ids[] = {kVoiceBinding};
    constexpr VnVoiceHandle kPublic = 77;
    ASSERT_EQ(VN_OK,
              f.bindings.CanReserveVoiceEffectInstances(ids, 1, &f.log));
    ASSERT_EQ(VN_OK, f.bindings.ReserveVoiceEffectInstances(
                         kPublic, ids, 1, &f.log));
    EXPECT_FALSE(f.bindings.HasEffectParamBinding(
        Scope::Voice, kRender, kSlot, harken::kGainParamGainDb,
        VN_EFFECT_GAIN));
    ASSERT_TRUE(f.bindings.ActivateReservedVoiceEffectInstances(
        kPublic, kRender));
    EXPECT_TRUE(f.bindings.HasEffectParamBinding(
        Scope::Voice, kRender, kSlot, harken::kGainParamGainDb,
        VN_EFFECT_GAIN));
    constexpr VnVoiceHandle kReturnedRender = 89;
    f.bindings.ParkVoiceEffectInstances(kRender);
    f.bindings.ReturnVoiceEffectInstances(kRender, kReturnedRender);
    EXPECT_TRUE(f.bindings.ActivateReservedVoiceEffectInstances(
        kPublic, kReturnedRender));
    f.bindings.RemoveVoiceEffectInstances(kReturnedRender);
    EXPECT_EQ(VN_OK, f.bindings.ReserveVoiceEffectInstances(
                         kPublic + 1, ids, 1, &f.log));
    f.bindings.RemoveVoiceEffectInstances(0);
    EXPECT_TRUE(f.bindings.ActivateReservedVoiceEffectInstances(
        kPublic + 1, kRender));
    f.bindings.RemoveVoiceEffectInstances(kRender);
    EXPECT_EQ(VN_OK, f.bindings.ReserveVoiceEffectInstances(
                         kPublic + 2, ids, 1, &f.log));
    f.bindings.CancelReservedVoiceEffectInstances(kPublic + 2);
}

// --- bus attachment path ---

TEST(EffectParamBindingTable, BusAttachmentFoldsContributionOntoBase) {
    Fixture f;
    ASSERT_EQ(VN_OK, RegisterLinear(f.bindings, f.log, kBusBinding,
                                    VN_TARGET_SCOPE_BUS_EFFECT_PARAM,
                                    GainLocator(), 0.0f, 6.0f));
    const std::vector<float> bases = {-6.0f};
    ASSERT_TRUE(f.store.SeedInstalledEffect(Scope::Bus, kBusIndex, kSlot,
                                            VN_EFFECT_GAIN, bases.data(),
                                            harken::kGainParamCount));
    // Bus attach carries the object (a bus id) and its resolved dense index.
    ASSERT_EQ(VN_OK, f.bindings.Attach(kBusBinding,
                                       VN_TARGET_SCOPE_BUS_EFFECT_PARAM,
                                       /*objectId=*/harkentest::ObjectId(42), kBusIndex, &f.log));
    EXPECT_TRUE(f.bindings.HasEffectParamBinding(Scope::Bus, kBusIndex, kSlot,
                                                 harken::kGainParamGainDb,
                                                 VN_EFFECT_GAIN));

    ASSERT_EQ(VN_OK, f.params.SetValue(kVar, 0.5f)); // curve -> +3 dB
    f.bindings.EvaluateEffectParamBindings(f.params, f.store, nullptr);
    EXPECT_FLOAT_EQ(-3.0f, f.store.Reduce(Scope::Bus, kBusIndex, kSlot,
                                          harken::kGainParamGainDb));
}

} // namespace
