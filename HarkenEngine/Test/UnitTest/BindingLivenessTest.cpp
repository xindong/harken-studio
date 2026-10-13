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
#include "harken/param/ParamTargets.h"

#include <cmath>
#include <cstdint>
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

} // namespace
