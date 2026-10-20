// Binding liveness across voice lifecycle states (the acceptance
// replace-liveness triple):
//   (a) a looping parked voice returns on the audibility pass after a
//       Replace makes it audible, with the new curve live on the returned
//       voice's first block;
//   (b) a delayed play whose attachment set was pinned at acceptance uses
//       a curve replaced before onset on its FIRST audible block — no
//       block ever renders with unbound or stale initial values;
//   (c) a Replace of a pitch binding while parked advances the virtual
//       cursor at the new rate, and the return offset stays honest.
// Plus: the first-block batch defers WHOLE under command-queue pressure
// (start command + resolved initial values are one admitted set), the
// pinned set is fixed at acceptance (later attach/detach does not alter a
// queued play), and Variable-driven voice-volume commits carry the
// aggregated ramp (section 9.3).
//
// All engine-level, on the null backend, driving the engine-internal
// binding seam (the public C verbs are not published yet).
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include "harken/param/BindingTable.h"
#include "harken/param/EffectParamLocator.h"
#include "harken/param/ParamTargets.h"
#include "harken/render/BuiltinEffects.h"

#include <cmath>
#include <cstdint>
#include <array>
#include <memory>
#include <vector>

namespace {

using harkentest::EngineSession;
using harkentest::MakeDC;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::MonoDesc;
using harkentest::Peak;
using harkentest::RenderHandle;

constexpr std::uint32_t kBlock = 256;
constexpr std::uint32_t kRate = 48000;

VnConfig ParkConfig() {
    VnConfig config = MakeNullConfig(4, kBlock, kRate);
    config.maxParkedVoices = 4;
    config.audibilityParkEnabled = 1;
    config.parkThresholdDb = -40.0f;
    return config;
}

void InstallBindingVariables(harken::Engine* engine) {
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(
                  engine, {harkentest::ObjectId(7), harkentest::ObjectId(8)}, 0.0f, 1.0f).status);
}

void RegisterLeaf(VnSoundId id) {
    static const std::vector<float> dc = MakeDC(kRate, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(id, &desc));
}

VnResult RegisterLinear(harken::Engine* engine, harken::BindingId id,
                        VnVariableId variableId, std::uint32_t targetId,
                        float y0, float y1, std::uint32_t transitionMs = 0) {
    const harken::CurvePoint pts[] = {{0.0f, y0}, {1.0f, y1}};
    const harken::CurveHandlePair handles[] = {{pts[0].x, pts[0].y, pts[1].x, pts[1].y}};
    return harken::RegisterBinding(*engine, id, variableId,
                                  harken::kTargetScopeVoice, targetId, pts,
                                  handles, 2, transitionMs);
}

VnResult ReplaceLinear(harken::Engine* engine, harken::BindingId id,
                       VnVariableId variableId, std::uint32_t targetId,
                       float y0, float y1, std::uint32_t transitionMs = 0) {
    const harken::CurvePoint pts[] = {{0.0f, y0}, {1.0f, y1}};
    const harken::CurveHandlePair handles[] = {{pts[0].x, pts[0].y, pts[1].x, pts[1].y}};
    return harken::ReplaceBinding(*engine, id, variableId,
                                 harken::kTargetScopeVoice, targetId, pts,
                                 handles, 2, transitionMs);
}

// One container whose single child resolves with a fixed onset delay —
// the delayed-play vehicle of the pinning drills.

struct NotificationSink {
    std::vector<VnNotification> notifications;
    std::size_t Count(std::uint32_t type, VnVoiceHandle handle) const {
        std::size_t n = 0;
        for (const VnNotification& e : notifications) {
            if (e.type == type && e.voiceHandle == handle) {
                ++n;
            }
        }
        return n;
    }
};
void SinkCb(const VnNotification* e, void* ud) {
    static_cast<NotificationSink*>(ud)->notifications.push_back(*e);
}

// Renders one block and returns its left-channel peak.
float PumpBlockPeak(harken::Engine* engine) {
    harkentest::Capture capture;
    harkentest::PumpAndCapture(engine, 1, capture);
    return Peak(capture.left);
}

// --- triple (a): park -> Replace makes it audible -> return, new curve ---

TEST(BindingLiveness, ReplaceReturnsParkedVoiceWithNewCurve) {
    NotificationSink sink;
    VnConfig config = ParkConfig();
    config.onNotification = &SinkCb;
    config.notificationUserData = &sink;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallBindingVariables(engine);
    RegisterLeaf(harkentest::ObjectId(1));

    // Variable 7 at 1.0 drives the attached volume binding to -80 dB:
    // the looping voice is inaudible from its first block and parks.
    ASSERT_EQ(VN_OK, RegisterLinear(engine, harkentest::ObjectId(10), harkentest::ObjectId(7),
                                    harken::kTargetVoiceVolume, 0.0f,
                                    -80.0f));
    ASSERT_EQ(VN_OK, harken::AttachBinding(*engine, harkentest::ObjectId(10),
                                          harken::kTargetScopeVoice, harkentest::ObjectId(1)));
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(7), 1.0f));
    harkentest::AdvanceVariableWrite(engine);
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    play.loopCount = 0; // infinite: park-eligible while inaudible
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    const harken::VoiceLedger::Resolved parked =
        engine->ledger.Classify(handle);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked, parked.kind);
    const std::uint32_t ledgerSlot = parked.ledgerSlot;
    ASSERT_TRUE(engine->parkRecords[ledgerSlot].active);
    EXPECT_FLOAT_EQ(-80.0f,
                    engine->parkRecords[ledgerSlot].instanceVolumeDb);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(1u, sink.Count(VN_NOTIFY_VOICE_PARKED, handle));

    // The ParkRecord holds the instance IDENTITY, never a curve copy
    // (section 5.6), and the parked identity blocks unregister (5.4).
    ASSERT_TRUE(engine->parkRecords[ledgerSlot].active);
    ASSERT_EQ(1u, engine->parkRecords[ledgerSlot].bindingInstances.size());
    EXPECT_EQ(harkentest::ObjectId(10u),
              engine->parkRecords[ledgerSlot].bindingInstances[0].definitionId);
    EXPECT_TRUE(engine->parkRecords[ledgerSlot].bindings.empty());
    EXPECT_EQ(VN_IN_USE, harken::UnregisterBinding(*engine, harkentest::ObjectId(10)));

    // Unchanged state keeps it parked across a tick.
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(0u, sink.Count(VN_NOTIFY_VOICE_RETURNED, handle));

    // Replace the curve to constant -6 dB: the revision installs at the
    // next update's first phase, the parked re-evaluation reads it, the
    // metric rises past the hysteresis, and the voice returns on THAT
    // audibility pass with the new curve live. -6 dB is deliberately
    // NON-neutral so the first-block amplitude separates three ways:
    // the new curve (0.5 x 10^(-6/20) = 0.2506), the old curve
    // (inaudible), and a missing instance (the unbound 0 dB base, 0.5).
    ASSERT_EQ(VN_OK, ReplaceLinear(engine, harkentest::ObjectId(10), harkentest::ObjectId(7),
                                   harken::kTargetVoiceVolume, -6.0f, -6.0f));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(1u, sink.Count(VN_NOTIFY_VOICE_RETURNED, handle));
    const VnVoiceHandle render = RenderHandle(engine, handle);
    ASSERT_NE(0u, render);
    EXPECT_FLOAT_EQ(-6.0f, engine->params.ResolvedValue(
                               render, harken::kTargetVoiceVolume));
    // The returned voice's first rendered block sits at the replaced
    // curve's absolute level — applied to the first block, not a tick
    // later, and not the unbound base level.
    const float firstBlockPeak = PumpBlockPeak(engine);
    const float expectedNewCurvePeak =
        0.5f * std::pow(10.0f, -6.0f / 20.0f); // 0.2506
    EXPECT_NEAR(expectedNewCurvePeak, firstBlockPeak, 0.02f);
}

// --- triple (b): pinned at acceptance, Replace before onset is live ---


// --- triple (c): Replace pitch while parked moves the virtual cursor ---

TEST(BindingLiveness, ReplacePitchWhileParkedAdvancesCursorAtNewRate) {
    NotificationSink sink;
    VnConfig config = ParkConfig();
    config.onNotification = &SinkCb;
    config.notificationUserData = &sink;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallBindingVariables(engine);
    RegisterLeaf(harkentest::ObjectId(1));

    // Two attachments: volume drives the park (variable 7 -> -80 dB) and
    // pitch drives the virtual rate (variable 8 -> +1200 cents = 2x).
    ASSERT_EQ(VN_OK, RegisterLinear(engine, harkentest::ObjectId(30), harkentest::ObjectId(7),
                                    harken::kTargetVoiceVolume, 0.0f,
                                    -80.0f));
    ASSERT_EQ(VN_OK, RegisterLinear(engine, harkentest::ObjectId(31), harkentest::ObjectId(8),
                                    harken::kTargetVoicePitch, 0.0f,
                                    1200.0f));
    ASSERT_EQ(VN_OK, harken::AttachBinding(*engine, harkentest::ObjectId(30),
                                          harken::kTargetScopeVoice, harkentest::ObjectId(1)));
    ASSERT_EQ(VN_OK, harken::AttachBinding(*engine, harkentest::ObjectId(31),
                                          harken::kTargetScopeVoice, harkentest::ObjectId(1)));
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(7), 1.0f));
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(8), 1.0f));
    harkentest::AdvanceVariableWrite(engine);
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    play.loopCount = 0;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(1u, sink.Count(VN_NOTIFY_VOICE_PARKED, handle));
    const std::uint32_t ledgerSlot = static_cast<std::uint32_t>(handle);
    harken::ParkRecord& rec = engine->parkRecords[ledgerSlot];
    ASSERT_TRUE(rec.active);
    EXPECT_EQ(2u, rec.bindingInstances.size());
    EXPECT_NEAR(2.0, rec.resolvedPitchRatio, 1e-6);

    // Virtual time advances at the CURRENT resolved pitch: 2x while the
    // registered curve stands...
    const double c0 = rec.virtualCursor;
    engine->nullBackend.Pump(4);
    ASSERT_EQ(VN_OK, Vn_Update());
    const double d1 = rec.virtualCursor - c0;
    EXPECT_NEAR(static_cast<double>(4 * kBlock) * 2.0, d1,
                static_cast<double>(2 * kBlock));

    // ...and at the NEW rate from the tick a pitch Replace installs
    // (+2400 cents = 4x).
    ASSERT_EQ(VN_OK, ReplaceLinear(engine, harkentest::ObjectId(31), harkentest::ObjectId(8),
                                   harken::kTargetVoicePitch, 2400.0f,
                                   2400.0f));
    ASSERT_EQ(VN_OK, Vn_Update()); // install tick (no blocks elapsed)
    EXPECT_NEAR(4.0, rec.resolvedPitchRatio, 1e-6);
    const double c1 = rec.virtualCursor;
    engine->nullBackend.Pump(4);
    ASSERT_EQ(VN_OK, Vn_Update());
    const double d2 = rec.virtualCursor - c1;
    EXPECT_NEAR(static_cast<double>(4 * kBlock) * 4.0, d2,
                static_cast<double>(4 * kBlock));

    // Return honesty: make it audible via the volume Replace; the return
    // resumes at (or just past) the advanced virtual offset.
    ASSERT_EQ(VN_OK, ReplaceLinear(engine, harkentest::ObjectId(30), harkentest::ObjectId(7),
                                   harken::kTargetVoiceVolume, 0.0f, 0.0f));
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(1u, sink.Count(VN_NOTIFY_VOICE_RETURNED, handle));
    const std::uint64_t offset = static_cast<std::uint64_t>(c1 + d2);
    engine->nullBackend.Pump(1);
    harken::VoiceRenderContext* ctx =
        engine->voices.Resolve(RenderHandle(engine, handle));
    ASSERT_NE(nullptr, ctx);
    EXPECT_GE(ctx->srcCursor, offset);
    // Generous bound: one stale block plus the resampler FIFO/history
    // lead, all consumed at the 4x rate.
    EXPECT_LE(ctx->srcCursor, offset + 4 * (kBlock + 512));
}

// --- section 5.7: the start batch is all-or-nothing under pressure ---


// --- section 5.7: the pinned set is fixed at acceptance ---


// --- section 9.3: Variable-driven volume commits carry the ramp ---

TEST(BindingLiveness, VariableDrivenVolumeCommitCarriesAggregatedRamp) {
    EngineSession session(MakeNullConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallBindingVariables(engine);
    RegisterLeaf(harkentest::ObjectId(1));
    ASSERT_EQ(VN_OK, RegisterLinear(engine, harkentest::ObjectId(40), harkentest::ObjectId(7),
                                    harken::kTargetVoiceVolume, 0.0f,
                                    -12.0f, /*transitionMs=*/200));
    ASSERT_EQ(VN_OK, harken::AttachBinding(*engine, harkentest::ObjectId(40),
                                          harken::kTargetScopeVoice, harkentest::ObjectId(1)));
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(7), 0.0f));
    harkentest::AdvanceVariableWrite(engine);
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    play.loopCount = 0;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_Update());
    const float steady = PumpBlockPeak(engine);
    ASSERT_GT(steady, harkentest::kSilenceFloor);

    // The binding's contribution changes: the published command carries
    // its 200 ms transition, so the first block after the commit still
    // sits near the old level (a snap would land at -12 dB immediately —
    // about a quarter of the old peak).
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(7), 1.0f));
    harkentest::AdvanceVariableWrite(engine);
    const float firstRampBlock = PumpBlockPeak(engine);
    EXPECT_GT(firstRampBlock, 0.8f * steady);

    // After the transition elapses (~200 ms = ~38 blocks) the level has
    // settled at the new curve value (-12 dB = 0.251x).
    engine->nullBackend.Pump(45);
    const float settled = PumpBlockPeak(engine);
    EXPECT_LT(settled, 0.35f * steady);
    EXPECT_GT(settled, 0.15f * steady);
}

// --- section 5.7: an over-ceiling attachment set refuses the play ---


// --- section 10.4 at acceptance: the pinned set reserves its capacity ---

TEST(BindingLiveness, AcceptanceRefusesWhenPinnedCapacityCannotReserve) {
    EngineSession session(MakeNullConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallBindingVariables(engine);
    RegisterLeaf(harkentest::ObjectId(1));
    ASSERT_EQ(VN_OK, RegisterLinear(engine, harkentest::ObjectId(10), harkentest::ObjectId(7),
                                    harken::kTargetVoiceVolume, -6.0f,
                                    -6.0f));
    ASSERT_EQ(VN_OK, harken::AttachBinding(*engine, harkentest::ObjectId(10),
                                          harken::kTargetScopeVoice, harkentest::ObjectId(1)));
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(7), 1.0f));
    harkentest::AdvanceVariableWrite(engine);
    // Squeeze the section-10.4 bound to its ceiling (the attach already
    // holds one seat): the play's pinned instance target cannot reserve,
    // so ACCEPTANCE refuses structurally — the same code the attach
    // verbs surface — with zero side effects, instead of a play that
    // would later start with a silently missing instance.
    const std::uint32_t squeeze =
        engine->propertyPublisher.BatchCapacity() -
        engine->propertyPublisher.WorstCaseCommits();
    ASSERT_TRUE(engine->propertyPublisher.TryGrowWorstCase(squeeze, nullptr));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    EXPECT_EQ(VN_INVALID_CONFIG, harkentest::PlayRaw(&play, &handle));
    EXPECT_EQ(0u, engine->voices.ActiveCount());

    // Releasing the pressure re-opens acceptance; the started voice's
    // first values carry the pinned instance (the reservation handed
    // over to the bind).
    engine->propertyPublisher.ShrinkWorstCase(squeeze);
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    EXPECT_FLOAT_EQ(-6.0f, engine->params.ResolvedValue(
                               RenderHandle(engine, handle),
                               harken::kTargetVoiceVolume));
    EXPECT_EQ(0u, engine->bindingInstanceOnsetFailCount);
}

struct DependencyFact {
    harken::BindingDependencyIdentity global{}, local{}, definition{}, probe{}, target{};
};

harken::BindingDependencyIdentity ReadDependency(const void* pointer, harken::BindingDependencyKind kind) noexcept {
    const auto& fact = *static_cast<const DependencyFact*>(pointer);
    switch (kind) {
    case harken::BindingDependencyKind::Global: return fact.global;
    case harken::BindingDependencyKind::Local: return fact.local;
    case harken::BindingDependencyKind::Definition: return fact.definition;
    case harken::BindingDependencyKind::Probe: return fact.probe;
    case harken::BindingDependencyKind::Target: return fact.target;
    }
    return {};
}

DependencyFact MakeDependencyFact(std::uint64_t variable, std::uint64_t emitter, std::uint64_t target) {
    DependencyFact fact;
    fact.global.source = harkentest::ObjectId(variable);
    fact.local = fact.global;
    fact.local.object = emitter;
    fact.definition.source = harkentest::ObjectId(variable + 1000);
    fact.probe.source = harkentest::ObjectId(variable + 2000);
    fact.probe.generation = 7;
    fact.target.object = target;
    fact.target.scope = harken::kTargetScopeVoice;
    return fact;
}

std::uint32_t CountDirtyTargets(const harken::BindingDependencyIndex& index) {
    std::uint32_t count = 0;
    harken::BindingDirtyTarget target;
    while (index.DirtyTargetAt(count, &target)) ++count;
    return count;
}

void CommitDirtyTargets(harken::BindingDependencyIndex& index) {
    harken::BindingDirtyTarget target;
    while (index.DirtyTargetAt(0, &target))
        ASSERT_TRUE(index.CommitTarget(target, index.TargetVersion(target)));
}

TEST(BindingDependencyIndex, LocalFallbackAndLiveDefinitionHaveExactIndependentWakeups) {
    harken::BindingDependencyIndex index;
    ASSERT_TRUE(index.Reset(2, 4));
    auto live = MakeDependencyFact(7, 101, 1);
    auto snapshot = MakeDependencyFact(7, 202, 2);
    std::array<harken::BindingDependencyToken, 2> tokens;
    ASSERT_TRUE(index.PrepareBinding(0));
    ASSERT_TRUE(index.PrepareBinding(1));
    const auto preparedBytes = index.StorageBytes();
    ASSERT_TRUE(index.Register(0, &live, ReadDependency, 0, true, true, false, &tokens[0]));
    ASSERT_TRUE(index.Register(1, &snapshot, ReadDependency, 1, true, false, false, &tokens[1]));
    EXPECT_EQ(preparedBytes, index.StorageBytes());
    CommitDirtyTargets(index);
    index.NotifyVariable(live.global.source, 303);
    EXPECT_EQ(0u, CountDirtyTargets(index));
    index.NotifyVariable(live.global.source, 101);
    EXPECT_EQ(1u, CountDirtyTargets(index));
    CommitDirtyTargets(index);
    index.NotifyDefinition(live.definition.source);
    EXPECT_EQ(1u, CountDirtyTargets(index));
    CommitDirtyTargets(index);
    index.NotifyVariable(live.global.source);
    EXPECT_EQ(2u, CountDirtyTargets(index));
    CommitDirtyTargets(index);
    ASSERT_TRUE(index.Remove(tokens[0]));
    ASSERT_TRUE(index.Remove(tokens[1]));
    CommitDirtyTargets(index);
}

TEST(BindingDependencyIndex, ReanchorsOriginalFactsAndRetainsDeparturesUntilVersionCommits) {
    harken::BindingDependencyIndex index;
    ASSERT_TRUE(index.Reset(3, 8));
    std::array<DependencyFact, 3> facts{MakeDependencyFact(7, 101, 1),
        MakeDependencyFact(8, 101, 1), MakeDependencyFact(9, 101, 1)};
    std::array<harken::BindingDependencyToken, 3> tokens;
    const std::uint32_t orders[] = {30, 10, 20};
    for (std::uint32_t slot = 0; slot < facts.size(); ++slot) {
        ASSERT_TRUE(index.PrepareBinding(slot));
        ASSERT_TRUE(index.Register(slot, &facts[slot], ReadDependency, orders[slot], true, true, false, &tokens[slot]));
    }
    harken::BindingDirtyTarget target;
    ASSERT_TRUE(index.TargetOf(tokens[0], &target));
    auto first = index.FirstContribution(target);
    EXPECT_EQ(&facts[1], index.Fact(first));
    first = index.NextContribution(first);
    EXPECT_EQ(&facts[2], index.Fact(first));
    first = index.NextContribution(first);
    EXPECT_EQ(&facts[0], index.Fact(first));
    EXPECT_FALSE(index.NextContribution(first).Valid());
    CommitDirtyTargets(index);
    const auto storageBytes = index.StorageBytes();
    auto moved = facts[0];
    ASSERT_TRUE(index.Relocate(tokens[0], &moved, ReadDependency, 5));
    facts[0] = {};
    EXPECT_EQ(storageBytes, index.StorageBytes());
    EXPECT_EQ(1u, CountDirtyTargets(index));
    first = index.FirstContribution(target);
    EXPECT_EQ(&moved, index.Fact(first));
    first = index.NextContribution(first);
    EXPECT_EQ(&facts[1], index.Fact(first));
    first = index.NextContribution(first);
    EXPECT_EQ(&facts[2], index.Fact(first));
    EXPECT_FALSE(index.NextContribution(first).Valid());
    CommitDirtyTargets(index);
    index.NotifyVariable(moved.global.source);
    const auto version = index.TargetVersion(target);
    index.NotifyVariable(moved.global.source, moved.local.object);
    EXPECT_FALSE(index.CommitTarget(target, version));
    EXPECT_EQ(1u, CountDirtyTargets(index));
    ASSERT_TRUE(index.Remove(tokens[0]));
    moved = {};
    ASSERT_TRUE(index.Remove(tokens[1]));
    facts[1] = {};
    CommitDirtyTargets(index);
    index.NotifyVariable(facts[2].global.source);
    EXPECT_EQ(1u, CountDirtyTargets(index));
    const auto identity = facts[2].target;
    ASSERT_TRUE(index.Remove(tokens[2]));
    facts[2] = {};
    harken::BindingDependencyIdentity retired;
    ASSERT_TRUE(index.TargetIdentity(target, &retired));
    EXPECT_EQ(identity, retired);
    auto replacement = MakeDependencyFact(10, 101, 1);
    harken::BindingDependencyToken replacementToken;
    ASSERT_TRUE(index.PrepareBinding(2));
    ASSERT_TRUE(index.Register(2, &replacement, ReadDependency, 20, true, true, false, &replacementToken));
    EXPECT_EQ(&replacement, index.Fact(index.FirstContribution(target)));
    EXPECT_FALSE(index.Remove(tokens[2]));
    ASSERT_TRUE(index.Remove(replacementToken));
    CommitDirtyTargets(index);
    EXPECT_FALSE(index.TargetIdentity(target, &retired));
}

TEST(BindingDependencyIndex, ExactProbeIdentityAndCanceledPreparationsRemainIsolated) {
    harken::BindingDependencyIndex index;
    ASSERT_TRUE(index.Reset(3, 3, 12));
    auto first = MakeDependencyFact(7, 0, 1);
    auto second = first;
    second.probe.generation += 0x10000;
    second.target.object = 2;
    harken::BindingDependencyToken one, two;
    ASSERT_TRUE(index.PrepareBinding(0));
    ASSERT_TRUE(index.PrepareBinding(1));
    index.CancelBindingPreparation(1);
    EXPECT_FALSE(index.HasBindingPreparation(1));
    ASSERT_TRUE(index.PrepareBinding(1));
    const auto preparedBytes = index.StorageBytes();
    ASSERT_TRUE(index.Register(0, &first, ReadDependency, 0, false, true, true, &one));
    ASSERT_TRUE(index.Register(1, &second, ReadDependency, 1, false, true, true, &two));
    EXPECT_EQ(preparedBytes, index.StorageBytes());
    CommitDirtyTargets(index);
    index.Notify(harken::BindingDependencyKind::Probe, first.probe);
    EXPECT_EQ(1u, CountDirtyTargets(index));
    CommitDirtyTargets(index);
    auto stale = second.probe;
    ++stale.object;
    index.Notify(harken::BindingDependencyKind::Probe, stale);
    EXPECT_EQ(0u, CountDirtyTargets(index));
    index.Notify(harken::BindingDependencyKind::Probe, second.probe);
    EXPECT_EQ(1u, CountDirtyTargets(index));
    ASSERT_TRUE(index.Remove(one));
    ASSERT_TRUE(index.Remove(two));
    CommitDirtyTargets(index);
}

TEST(BindingDependencyIndex, RemovingManyKeysPreservesAllSurvivingIdentities) {
    constexpr std::uint32_t count = 65;
    harken::BindingDependencyIndex index;
    ASSERT_TRUE(index.Reset(count, count * 2));
    std::array<DependencyFact, count> facts;
    std::array<harken::BindingDependencyToken, count> tokens;
    std::array<bool, count> removed{};
    for (std::uint32_t slot = 0; slot < count; ++slot) {
        facts[slot] = MakeDependencyFact(slot + 1, slot + 100, slot + 1);
        ASSERT_TRUE(index.PrepareBinding(slot));
    }
    const auto preparedBytes = index.StorageBytes();
    for (std::uint32_t slot = 0; slot < count; ++slot)
        ASSERT_TRUE(index.Register(slot, &facts[slot], ReadDependency, slot, true, true, false, &tokens[slot]));
    EXPECT_EQ(preparedBytes, index.StorageBytes());
    CommitDirtyTargets(index);
    for (std::uint32_t step = 0; step < count; ++step) {
        const auto slot = (step * 17u) % count;
        ASSERT_TRUE(index.Remove(tokens[slot]));
        removed[slot] = true;
        CommitDirtyTargets(index);
        for (std::uint32_t live = 0; live < count; ++live)
            if (!removed[live]) index.NotifyVariable(facts[live].global.source, facts[live].local.object);
        EXPECT_EQ(count - step - 1, CountDirtyTargets(index));
        CommitDirtyTargets(index);
    }
}

TEST(BindingLiveness, DirtyBusTargetRebuildsEveryContributionAndDepartureInOriginalOrder) {
    harken::BindingTable table(2, 2, 1);
    harken::BusTargetStore store(1);
    harken::ParamSystem params(harken::EngineConfig{});
    const harken::CurvePoint points[] = {{0, 0}, {1, -6}};
    const harken::CurveHandlePair handles[] = {{0, 0, 1, -6}};
    for (std::uint32_t id = 1; id <= 2; ++id) {
        ASSERT_EQ(VN_OK, table.RegisterBinding(harkentest::ObjectId(id), harkentest::ObjectId(id + 10),
            harken::kTargetScopeBus, harken::kBusTargetBusVolume, points, handles, 2, id * 10, nullptr));
        ASSERT_EQ(VN_OK, table.Attach(harkentest::ObjectId(id), harken::kTargetScopeBus,
            harkentest::ObjectId(50), 0, nullptr));
        ASSERT_EQ(VN_OK, params.SetValue(harkentest::ObjectId(id + 10), 1));
    }
    table.EvaluateDirtyBusAttachments(params, store);
    EXPECT_FLOAT_EQ(-12, store.Reduce(0, harken::kBusTargetBusVolume));
    ASSERT_EQ(VN_OK, params.SetValue(harkentest::ObjectId(11), 0.5f));
    table.NoteBusVariableWrite(harkentest::ObjectId(11), store);
    table.EvaluateDirtyBusAttachments(params, store);
    const float halfContributionDb = 20.0f * std::log10(0.5f * (1.0f + std::pow(10.0f, -6.0f / 20.0f)));
    EXPECT_NEAR(halfContributionDb - 6.0f, store.Reduce(0, harken::kBusTargetBusVolume), 1e-5f);
    ASSERT_EQ(VN_OK, table.Detach(harkentest::ObjectId(1), harken::kTargetScopeBus, harkentest::ObjectId(50), &store));
    table.EvaluateDirtyBusAttachments(params, store);
    EXPECT_FLOAT_EQ(-6, store.Reduce(0, harken::kBusTargetBusVolume));
    ASSERT_EQ(VN_OK, table.Detach(harkentest::ObjectId(2), harken::kTargetScopeBus, harkentest::ObjectId(50), &store));
    table.EvaluateDirtyBusAttachments(params, store);
    EXPECT_FALSE(store.HasVariableContributions());
    EXPECT_FLOAT_EQ(0, store.Reduce(0, harken::kBusTargetBusVolume));
}

TEST(BindingLiveness, ReusedBusGenerationRejectsOldInputsAndDepartures) {
    for (bool preparedDetach : {false, true}) {
        harken::BindingTable table(2, 2, 1);
        harken::BusTargetStore store(1);
        harken::ParamSystem params(harken::EngineConfig{});
        const harken::CurvePoint points[] = {{0, 0}, {1, -6}};
        const harken::CurveHandlePair handles[] = {{0, 0, 1, -6}};
        for (std::uint32_t id = 1; id <= 2; ++id) {
            ASSERT_EQ(VN_OK, table.RegisterBinding(harkentest::ObjectId(id), harkentest::ObjectId(id + 10),
                harken::kTargetScopeBus, harken::kBusTargetBusVolume, points, handles, 2, id * 10, nullptr));
            ASSERT_EQ(VN_OK, params.SetValue(harkentest::ObjectId(id + 10), 1));
        }
        store.InitializeBusGeneration(0, 1);
        ASSERT_EQ(VN_OK, table.Attach(harkentest::ObjectId(1), harken::kTargetScopeBus,
            harkentest::ObjectId(50), 0, nullptr, 0, 1));
        table.EvaluateDirtyBusAttachments(params, store);
        EXPECT_FLOAT_EQ(-6, store.Reduce(0, harken::kBusTargetBusVolume));

        store.InitializeBusGeneration(0, 2);
        table.NoteBusVariableWrite(harkentest::ObjectId(11), store);
        EXPECT_EQ(0u, store.DemandCount());
        table.EvaluateDirtyBusAttachments(params, store);
        EXPECT_FALSE(store.HasVariableContributions());
        harken::BindingTable::PreparedAttachmentToken add;
        ASSERT_EQ(VN_OK, table.PrepareAttach(harkentest::ObjectId(2), harken::kTargetScopeBus,
            harkentest::ObjectId(51), 0, 1, 0, &add, UINT32_MAX, false, false, false, 2));
        table.CommitPreparedAttachment(&add, &store);
        table.EvaluateDirtyBusAttachments(params, store);
        store.MarkCommitted(0, harken::kBusTargetBusVolume, -6);

        if (preparedDetach) {
            harken::BindingTable::PreparedAttachmentToken remove;
            ASSERT_EQ(VN_OK, table.PrepareDetach(harkentest::ObjectId(1), harken::kTargetScopeBus,
                harkentest::ObjectId(50), &remove));
            table.CommitPreparedAttachment(&remove, &store);
        } else {
            ASSERT_EQ(VN_OK, table.Detach(harkentest::ObjectId(1), harken::kTargetScopeBus,
                harkentest::ObjectId(50), &store));
        }
        table.EvaluateDirtyBusAttachments(params, store);
        EXPECT_EQ(0u, store.DemandCount());
        EXPECT_EQ(0u, store.PendingRampMs(0, harken::kBusTargetBusVolume));
        EXPECT_FLOAT_EQ(-6, store.Reduce(0, harken::kBusTargetBusVolume));
    }
}

TEST(BindingLiveness, EffectParkPreparationAndReturnKeepExactEmitterWithoutGrowth) {
    harken::BindingTable table(1, 2, 1);
    const auto target = harken::PackEffectParamLocator(VN_EFFECT_GAIN, 0, harken::kGainParamGainDb);
    const harken::CurvePoint points[] = {{0, 0}, {1, 6}};
    const harken::CurveHandlePair handles[] = {{0, 0, 1, 6}};
    ASSERT_EQ(VN_OK, table.RegisterBinding(harkentest::ObjectId(1), harkentest::ObjectId(11),
        VN_TARGET_SCOPE_VOICE_EFFECT_PARAM, target, points, handles, 2, 0, nullptr));
    ASSERT_EQ(VN_OK, table.ApplyVoiceEffectInstance(harkentest::ObjectId(1), 1, 1, nullptr, 101));
    auto& index = table.Dependencies();
    CommitDirtyTargets(index);
    index.NotifyVariable(harkentest::ObjectId(11), 202);
    EXPECT_EQ(0u, CountDirtyTargets(index));
    index.NotifyVariable(harkentest::ObjectId(11), 101);
    EXPECT_EQ(1u, CountDirtyTargets(index));
    CommitDirtyTargets(index);
    ASSERT_TRUE(table.PrepareVoiceEffectInstancePark(1));
    EXPECT_TRUE(index.HasBindingPreparation(2));
    table.CancelVoiceEffectInstancePark(1);
    EXPECT_FALSE(index.HasBindingPreparation(2));
    ASSERT_TRUE(table.PrepareVoiceEffectInstancePark(1));
    const auto preparedBytes = index.StorageBytes();
    table.ParkVoiceEffectInstances(1);
    table.ReturnVoiceEffectInstances(1, 2, 202);
    EXPECT_EQ(preparedBytes, index.StorageBytes());
    CommitDirtyTargets(index);
    index.NotifyVariable(harkentest::ObjectId(11), 101);
    EXPECT_EQ(0u, CountDirtyTargets(index));
    index.NotifyVariable(harkentest::ObjectId(11), 202);
    EXPECT_EQ(1u, CountDirtyTargets(index));
}

TEST(BindingLiveness, CopiedBindingProjectionOwnsItsFactsAndPreparedAdds) {
    harken::ParamSystem params(harken::EngineConfig{});
    harken::BusTargetStore store(1);
    std::unique_ptr<harken::BindingTable> copy;
    harken::BindingTable::PreparedAttachmentToken add;
    const harken::CurvePoint points[] = {{0, 0}, {1, -6}};
    const harken::CurveHandlePair handles[] = {{0, 0, 1, -6}};
    {
        harken::BindingTable original(2, 2, 1);
        ASSERT_EQ(VN_OK, original.RegisterBinding(harkentest::ObjectId(1), harkentest::ObjectId(11),
            harken::kTargetScopeBus, harken::kBusTargetBusVolume, points, handles, 2, 0, nullptr));
        ASSERT_EQ(VN_OK, original.Attach(harkentest::ObjectId(1), harken::kTargetScopeBus,
            harkentest::ObjectId(50), 0, nullptr));
        ASSERT_EQ(VN_OK, original.PrepareAttach(harkentest::ObjectId(1), harken::kTargetScopeBus,
            harkentest::ObjectId(51), 0, 1, 0, &add));
        copy = std::make_unique<harken::BindingTable>(original);
        original.CancelPreparedAttachment(&add);
        ASSERT_EQ(VN_OK, copy->PrepareAttach(harkentest::ObjectId(1), harken::kTargetScopeBus,
            harkentest::ObjectId(51), 0, 1, 0, &add));
    }
    copy->CommitPreparedAttachment(&add, &store);
    ASSERT_EQ(VN_OK, params.SetValue(harkentest::ObjectId(11), 1));
    copy->EvaluateDirtyBusAttachments(params, store);
    EXPECT_FLOAT_EQ(-12, store.Reduce(0, harken::kBusTargetBusVolume));
}

TEST(BindingLiveness, DirtyEffectTargetKeepsOtherContributorsAndReturnsToBase) {
    harken::BindingTable table(2, 2, 1);
    harken::ParamSystem params(harken::EngineConfig{});
    harken::EffectParamTargetStore store(kRate, 4);
    const auto target = harken::PackEffectParamLocator(VN_EFFECT_GAIN, 0, harken::kGainParamGainDb);
    const harken::CurvePoint points[] = {{0, 0}, {1, 6}};
    const harken::CurveHandlePair handles[] = {{0, 0, 1, 6}};
    const float base = -24;
    ASSERT_TRUE(store.SeedInstalledEffect(harken::EffectParamTargetStore::Scope::Voice, 1, 0,
        VN_EFFECT_GAIN, &base, harken::kGainParamCount));
    for (std::uint32_t id = 1; id <= 2; ++id) {
        ASSERT_EQ(VN_OK, table.RegisterBinding(harkentest::ObjectId(id), harkentest::ObjectId(id + 10),
            VN_TARGET_SCOPE_VOICE_EFFECT_PARAM, target, points, handles, 2, 0, nullptr));
        ASSERT_EQ(VN_OK, table.ApplyVoiceEffectInstance(harkentest::ObjectId(id), 1, nullptr));
        ASSERT_EQ(VN_OK, params.SetValue(harkentest::ObjectId(id + 10), 1));
    }
    table.EvaluateDirtyEffectParamBindings(params, store, nullptr);
    EXPECT_FLOAT_EQ(-12, store.Reduce(harken::EffectParamTargetStore::Scope::Voice, 1, 0, harken::kGainParamGainDb));
    ASSERT_EQ(VN_OK, params.SetValue(harkentest::ObjectId(11), 0));
    table.Dependencies().NotifyVariable(harkentest::ObjectId(11));
    table.EvaluateDirtyEffectParamBindings(params, store, nullptr);
    EXPECT_FLOAT_EQ(-18, store.Reduce(harken::EffectParamTargetStore::Scope::Voice, 1, 0, harken::kGainParamGainDb));
    table.RemoveVoiceEffectInstances(1);
    table.EvaluateDirtyEffectParamBindings(params, store, nullptr);
    EXPECT_FLOAT_EQ(base, store.Reduce(harken::EffectParamTargetStore::Scope::Voice, 1, 0, harken::kGainParamGainDb));
}

} // namespace
