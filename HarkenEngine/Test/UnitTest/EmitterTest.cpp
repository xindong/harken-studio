// Emitters and per-emitter Variable scoping (spec §4, acceptance §9.4): the
// isolation goldens (two emitters, same sound, different local Variable values
// resolve to different bound-target results), fallback to global when an
// override is cleared, the global reach of Vn_SetValue to unscoped voices,
// emitter-scoped precision of dirty tracking, emitter-driven distance and the
// move-once-update-all fan-out, and the contract gates: VN_EMITTER_ATTACHED on
// attached per-voice spatial writes, destroy-detaches-with-frozen-state, stale
// emitter ops, Vn_StopEmitter, capacity exhaustion VN_NO_EMITTER, and a play
// naming a dead emitter.
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include "harken/param/Curve.h"
#include "harken/param/ParamTargets.h"
#include "MonitorEmitterRegistry.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

TEST(MonitorEmitterIdentity, CaptureOffPreservesOriginAcrossDestroyAndSlotReuse) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    auto& registry = engine.monitorEmitters;
    EXPECT_EQ(engine.config.maxEmitters, registry.Capacity());
    const auto sdk = registry.BeginSession();
    ASSERT_NE(0u, sdk);
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    ASSERT_TRUE(registry.Register(registry.EngineInstance(), sdk, emitter, 1,
        harken::MonitorHostOwnership::UnityEmitter, 1));
    const auto origin = registry.CaptureOrigin(emitter);
    ASSERT_TRUE(origin.host.Valid());
    EXPECT_EQ(engine.monitorRequests.EngineInstance(), origin.host.engineInstance);
    EXPECT_EQ(harken::MonitorEmitterKind::ExplicitSpatial, origin.kind);
    EXPECT_EQ(harken::MonitorBindingState::Bound,
        registry.CaptureBinding(origin, emitter, emitter).spatial);

    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(emitter));
    // Even a spatial mirror which has not seen Update is no longer bound.
    const auto detached = registry.CaptureBinding(origin, 0, emitter);
    EXPECT_EQ(harken::MonitorBindingState::Detached, detached.state);
    EXPECT_EQ(harken::MonitorBindingState::Detached, detached.spatial);
    EXPECT_EQ(0u, detached.spatialEmitter);
    EXPECT_EQ(1u, origin.host.localSequence);
    EXPECT_FALSE(registry.CaptureOrigin(emitter).host.Valid());

    VnEmitterHandle reused = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&reused));
    ASSERT_EQ(static_cast<std::uint32_t>(emitter), static_cast<std::uint32_t>(reused));
    ASSERT_NE(emitter, reused);
    EXPECT_FALSE(registry.CaptureOrigin(reused).host.Valid());
    EXPECT_FALSE(registry.Register(registry.EngineInstance(), sdk, reused, 1,
        harken::MonitorHostOwnership::UnityEmitter, 1));
    ASSERT_TRUE(registry.Register(registry.EngineInstance(), sdk, reused, 2,
        harken::MonitorHostOwnership::UnityEmitter, 1));
    registry.NoteDestroyed(emitter);
    EXPECT_FALSE(registry.Unregister(registry.EngineInstance(), sdk, emitter, 1));
    EXPECT_EQ(2u, registry.CaptureOrigin(reused).host.localSequence);
    EXPECT_EQ(1u, origin.host.localSequence);
}

TEST(MonitorEmitterIdentity, NoneDefaultStateOnlyAndUnknownHostRemainDistinct) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    auto& registry = engine.monitorEmitters;
    const auto none = registry.CaptureOrigin(0);
    const auto fallback = registry.CaptureOrigin(engine.emitters.DefaultState());
    EXPECT_EQ(harken::MonitorEmitterKind::None, none.kind);
    EXPECT_EQ(harken::MonitorEmitterKind::DefaultState, fallback.kind);
    EXPECT_EQ(harken::MonitorBindingState::None, registry.CaptureBinding(none, 0, 0).state);
    EXPECT_EQ(harken::MonitorBindingState::Bound,
        registry.CaptureBinding(fallback, fallback.emitter, 0).state);
    EXPECT_EQ(harken::MonitorBindingState::None,
        registry.CaptureBinding(fallback, fallback.emitter, 0).spatial);
    VnEmitterCreateDescV1 desc{sizeof(VnEmitterCreateDescV1), VN_EMITTER_CREATE_STATE_ONLY, 0};
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitterEx(&desc, &emitter));
    const auto stateOnly = registry.CaptureOrigin(emitter);
    EXPECT_EQ(harken::MonitorEmitterKind::ExplicitStateOnly, stateOnly.kind);
    EXPECT_FALSE(stateOnly.host.Valid());
    const auto bound = registry.CaptureBinding(stateOnly, emitter, 0);
    EXPECT_EQ(harken::MonitorBindingState::Bound, bound.state);
    EXPECT_EQ(harken::MonitorBindingState::None, bound.spatial);
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(emitter));
    const auto detached = registry.CaptureBinding(stateOnly, emitter, 0);
    EXPECT_EQ(harken::MonitorBindingState::Detached, detached.state);
    EXPECT_EQ(harken::MonitorBindingState::None, detached.spatial);
}

TEST(MonitorEmitterIdentity, ReconnectRejectsOldSessionWithoutChangingEmitterLifetime) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    auto& registry = engine.monitorEmitters;
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    const auto first = registry.BeginSession();
    ASSERT_TRUE(registry.Register(registry.EngineInstance(), first, emitter, 7,
        harken::MonitorHostOwnership::UnitySpatialSource, 3));
    const auto original = registry.CaptureOrigin(emitter);
    ASSERT_TRUE(registry.Register(registry.EngineInstance(), first, emitter, 7,
        harken::MonitorHostOwnership::UnitySpatialSource, 3));
    EXPECT_FALSE(registry.Register(registry.EngineInstance(), first, emitter, 7,
        harken::MonitorHostOwnership::UnitySpatialSource, 2));
    const auto second = registry.BeginSession();
    ASSERT_GT(second, first);
    EXPECT_FALSE(registry.CaptureOrigin(emitter).host.Valid());
    ASSERT_TRUE(registry.Register(registry.EngineInstance(), second, emitter, 1,
        harken::MonitorHostOwnership::UnitySpatialSource, 1));
    EXPECT_FALSE(registry.EndSession(registry.EngineInstance(), first));
    EXPECT_FALSE(registry.Unregister(registry.EngineInstance(), first, emitter, 7));
    EXPECT_FALSE(registry.Register(registry.EngineInstance(), first, emitter, 9,
        harken::MonitorHostOwnership::UnityEmitter, 4));
    EXPECT_EQ(second, registry.CaptureOrigin(emitter).host.sdkSession);
    EXPECT_EQ(first, original.host.sdkSession);
    EXPECT_EQ(3u, original.registrationRevision);
    EXPECT_TRUE(registry.EndSession(registry.EngineInstance(), second));
    EXPECT_TRUE(engine.emitters.IsPublicLive(emitter));
    EXPECT_FALSE(registry.CaptureOrigin(emitter).host.Valid());
}

TEST(MonitorEmitterIdentity, ExhaustedDiagnosticSessionDoesNotStopNativeEmitterCreation) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    harken::MonitorEmitterRegistry exhausted(engine.emitters, engine.config.maxEmitters,
        engine.monitorRequests.EngineInstance(), UINT64_MAX);
    EXPECT_EQ(0u, exhausted.BeginSession());
    EXPECT_EQ(0u, exhausted.BeginSession());
    EXPECT_EQ(2u, exhausted.UnavailableCount());
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    exhausted.NoteCreated(emitter);
    EXPECT_EQ(harken::MonitorEmitterKind::ExplicitSpatial, exhausted.CaptureOrigin(emitter).kind);
    EXPECT_FALSE(exhausted.Register(exhausted.EngineInstance(), UINT64_MAX, emitter, 1,
        harken::MonitorHostOwnership::UnityEmitter, 1));
    EXPECT_EQ(VN_OK, Vn_DestroyEmitter(emitter));
}

TEST(MonitorEmitterIdentity, OldEngineCallbacksCannotChangeSameNumericSessionAndEmitter) {
    harken::EngineConfig config{};
    config.maxEmitters = 1;
    harken::EmitterTable firstTable(config), secondTable(config);
    harken::MonitorEmitterRegistry first(firstTable, 1, 71), second(secondTable, 1, 72);
    const auto firstSession = first.BeginSession();
    const auto secondSession = second.BeginSession();
    ASSERT_EQ(firstSession, secondSession);
    VnEmitterHandle a = 0, b = 0;
    ASSERT_TRUE(firstTable.Create(&a));
    ASSERT_TRUE(secondTable.Create(&b));
    ASSERT_EQ(a, b);
    first.NoteCreated(a);
    second.NoteCreated(b);
    ASSERT_TRUE(first.Register(71, firstSession, a, 1, harken::MonitorHostOwnership::UnityEmitter, 1));
    ASSERT_TRUE(second.Register(72, secondSession, b, 1, harken::MonitorHostOwnership::UnityEmitter, 1));
    EXPECT_FALSE(second.Register(71, firstSession, a, 1, harken::MonitorHostOwnership::UnityEmitter, 2));
    EXPECT_FALSE(second.Unregister(71, firstSession, a, 1));
    EXPECT_FALSE(second.EndSession(71, firstSession));
    const auto current = second.CaptureOrigin(b);
    EXPECT_EQ(72u, current.host.engineInstance);
    EXPECT_EQ(1u, current.registrationRevision);
    EXPECT_TRUE(current.host.Valid());
}

TEST(MonitorEmitterIdentity, OptionalAbiRejectsMalformedAndStaleRegistrationWithoutMutation) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    VnMonitorHostSessionV1 info{};
    EXPECT_EQ(VN_INVALID_ARG, Vn_MonitorBeginHostSession(&info));
    info.structSize = sizeof(info);
    ASSERT_EQ(VN_OK, Vn_MonitorBeginHostSession(&info));
    EXPECT_EQ(engine.config.maxEmitters, info.emitterCapacity);
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    VnMonitorHostRegistrationV1 desc{sizeof(desc), VN_MONITOR_HOST_UNITY_EMITTER,
        info.engineInstance, info.sdkSession, emitter, 1, 1, 0};
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHost(&desc));
    desc.reserved0 = 1;
    EXPECT_EQ(VN_INVALID_ARG, Vn_MonitorRegisterHost(&desc));
    desc.reserved0 = 0;
    ++desc.engineInstance;
    EXPECT_EQ(VN_MONITOR_STALE_SESSION, Vn_MonitorRegisterHost(&desc));
    EXPECT_EQ(VN_MONITOR_STALE_SESSION, Vn_MonitorEndHostSession(desc.engineInstance, desc.sdkSession));
    EXPECT_TRUE(engine.monitorEmitters.CaptureOrigin(emitter).host.Valid());
    EXPECT_EQ(VN_OK, Vn_MonitorEndHostSession(info.engineInstance, info.sdkSession));
    EXPECT_TRUE(engine.emitters.IsPublicLive(emitter));
    EXPECT_FALSE(engine.monitorEmitters.CaptureOrigin(emitter).host.Valid());
}

using harkentest::EngineSession;
using harkentest::InterleavedDesc;
using harkentest::MakeAttenuationDesc;
using harkentest::MakeDC;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::MonoDesc;
using harkentest::RenderHandle;
using harkentest::Vec3;

constexpr std::uint32_t kBlock = 256;
constexpr std::uint32_t kRate = 48000;
constexpr float kNan = std::numeric_limits<float>::quiet_NaN();

harken::Curve VolumeCurve() {
    harken::Curve curve;
    const harken::CurvePoint pts[] = {{0.0f, 0.0f}, {1.0f, -24.0f}};
    const harken::CurveHandlePair handles[] = {{pts[0].x, pts[0].y, pts[1].x, pts[1].y}};
    EXPECT_TRUE(curve.SetPoints(pts, handles, 2));
    return curve;
}

float VolumeDbAt(float input) {
    return 20.0f * std::log10(1.0f - input + input * std::pow(10.0f, -24.0f / 20.0f));
}

void InstallEmitterVariables(harken::Engine* engine) {
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(
                  engine, {harkentest::ObjectId(5), harkentest::ObjectId(77)}, 0.0f, 1.0f, 0.0f).status);
}

VnConfig EmitterConfig(std::uint32_t maxVoices = 8,
                       std::uint32_t maxEmitters = 8) {
    VnConfig config = MakeNullConfig(maxVoices, kBlock, kRate);
    config.maxEmitters = maxEmitters;
    return config;
}

VnPlayDesc AttachedPlay(VnSoundId sound, VnEmitterHandle emitter,
                        float volumeDb = 0.0f) {
    VnPlayDesc play = MakePlayDesc(sound, volumeDb);
    play.emitterHandle = emitter;
    return play;
}

VnSnapshot Snap() {
    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    EXPECT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
    return snapshot;
}

// --- Variable isolation goldens ---

TEST(Emitter, VariableProjectionRestoresPresenceAndReleasesSpeculativeLocalSlot) {
    harken::EngineConfig config{};
    config.maxEmitters = 1;
    config.emitterVariableSlots = 1;
    harken::EmitterTable emitters(config);
    VnEmitterHandle emitter = 0;
    ASSERT_TRUE(emitters.Create(&emitter));
    const auto original = harkentest::ObjectId(5);
    const auto candidate = harkentest::ObjectId(77);
    ASSERT_EQ(VN_OK, emitters.SetLocalValue(emitter, original, 0.25f));
    ASSERT_TRUE(emitters.BeginVariableProjection());
    EXPECT_FALSE(emitters.BeginVariableProjection());
    ASSERT_EQ(VN_OK, emitters.ClearLocalValue(emitter, original));
    ASSERT_EQ(VN_OK, emitters.SetLocalValue(emitter, candidate, 0.75f));
    emitters.RestoreVariableProjection();
    float value = -1.0f;
    ASSERT_TRUE(emitters.LocalValue(emitter, original, &value));
    EXPECT_FLOAT_EQ(0.25f, value);
    EXPECT_FALSE(emitters.LocalValue(emitter, candidate, &value));
    ASSERT_EQ(VN_OK, emitters.ClearLocalValue(emitter, original));
    EXPECT_EQ(VN_OK, emitters.SetLocalValue(emitter, candidate, 1.0f));
    EXPECT_GT(emitters.VariableProjectionStorageBytes(), 0u);
}

TEST(Emitter, LocalVariableValuesIsolatePerEmitter) {
    EngineSession session(EmitterConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallEmitterVariables(engine);
    const std::vector<float> dc = MakeDC(kRate, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnEmitterHandle emitterA = 0;
    VnEmitterHandle emitterB = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitterA));
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitterB));
    EXPECT_NE(emitterA, emitterB);

    VnPlayDesc playA = AttachedPlay(harkentest::ObjectId(1), emitterA);
    VnPlayDesc playB = AttachedPlay(harkentest::ObjectId(1), emitterB);
    VnVoiceHandle voiceA = 0;
    VnVoiceHandle voiceB = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&playA, &voiceA));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&playB, &voiceB));

    // One Variable id bound to volume on both voices; the emitters give it
    // different local values, so the same binding resolves differently.
    const harken::Curve curve = VolumeCurve();
    ASSERT_TRUE(engine->params.BindVoiceTarget(
        harkentest::ObjectId(77), RenderHandle(engine, voiceA), harken::kTargetVoiceVolume, curve));
    ASSERT_TRUE(engine->params.BindVoiceTarget(
        harkentest::ObjectId(77), RenderHandle(engine, voiceB), harken::kTargetVoiceVolume, curve));
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitterA, harkentest::ObjectId(77), 0.25f));
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitterB, harkentest::ObjectId(77), 0.75f));
    harkentest::AdvanceVariableWrite(engine);

    EXPECT_NEAR(VolumeDbAt(0.25f), engine->params.ResolvedValue(
                               RenderHandle(engine, voiceA),
                               harken::kTargetVoiceVolume), 1.0e-5f);
    EXPECT_NEAR(VolumeDbAt(0.75f), engine->params.ResolvedValue(
                                RenderHandle(engine, voiceB),
                                harken::kTargetVoiceVolume), 1.0e-5f);
}

TEST(Emitter, GlobalWriteReplacesLocalAndReachesUnscopedVoices) {
    EngineSession session(EmitterConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallEmitterVariables(engine);
    const std::vector<float> dc = MakeDC(kRate, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));

    // One voice attached to the emitter, one with no emitter (global scope).
    VnPlayDesc attachedPlay = AttachedPlay(harkentest::ObjectId(1), emitter);
    VnPlayDesc globalPlay = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle attached = 0;
    VnVoiceHandle unscoped = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&attachedPlay, &attached));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&globalPlay, &unscoped));

    const harken::Curve curve = VolumeCurve();
    ASSERT_TRUE(engine->params.BindVoiceTarget(
        harkentest::ObjectId(77), RenderHandle(engine, attached), harken::kTargetVoiceVolume, curve));
    ASSERT_TRUE(engine->params.BindVoiceTarget(
        harkentest::ObjectId(77), RenderHandle(engine, unscoped), harken::kTargetVoiceVolume, curve));

    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(77), 0.5f));
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, harkentest::ObjectId(77), 0.25f));
    harkentest::AdvanceVariableWrite(engine);
    EXPECT_NEAR(VolumeDbAt(0.25f), engine->params.ResolvedValue(
                               RenderHandle(engine, attached),
                               harken::kTargetVoiceVolume), 1.0e-5f);
    EXPECT_NEAR(VolumeDbAt(0.5f), engine->params.ResolvedValue(
                                RenderHandle(engine, unscoped),
                                harken::kTargetVoiceVolume), 1.0e-5f);

    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(77), 0.5f));
    harkentest::AdvanceVariableWrite(engine);
    EXPECT_NEAR(VolumeDbAt(0.5f), engine->params.ResolvedValue(
                                RenderHandle(engine, attached),
                                harken::kTargetVoiceVolume), 1.0e-5f);

    // A later global change still reaches the unscoped voice.
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(77), 1.0f)); // -24 dB
    harkentest::AdvanceVariableWrite(engine);
    EXPECT_FLOAT_EQ(-24.0f, engine->params.ResolvedValue(
                                RenderHandle(engine, unscoped),
                                harken::kTargetVoiceVolume));
}

TEST(Emitter, LocalWriteDirtiesOnlyThatEmittersVoices) {
    // Dirty tracking is scoped: an emitter-local write recommits only that
    // emitter's voices, never the other emitter's (whose resolved value did
    // not move). Proven through the commit counter.
    EngineSession session(EmitterConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallEmitterVariables(engine);
    const std::vector<float> dc = MakeDC(kRate, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnEmitterHandle emitterA = 0;
    VnEmitterHandle emitterB = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitterA));
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitterB));
    VnPlayDesc playA = AttachedPlay(harkentest::ObjectId(1), emitterA);
    VnPlayDesc playB = AttachedPlay(harkentest::ObjectId(1), emitterB);
    VnVoiceHandle voiceA = 0;
    VnVoiceHandle voiceB = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&playA, &voiceA));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&playB, &voiceB));

    const harken::Curve curve = VolumeCurve();
    ASSERT_TRUE(engine->params.BindVoiceTarget(
        harkentest::ObjectId(77), RenderHandle(engine, voiceA), harken::kTargetVoiceVolume, curve));
    ASSERT_TRUE(engine->params.BindVoiceTarget(
        harkentest::ObjectId(77), RenderHandle(engine, voiceB), harken::kTargetVoiceVolume, curve));
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitterA, harkentest::ObjectId(77), 0.25f));
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitterB, harkentest::ObjectId(77), 0.75f));
    harkentest::AdvanceVariableWrite(engine); // both commit their initial resolved values

    const std::uint64_t before = engine->params.CommitCount();
    // Move only emitter A's local value: only voice A's resolved volume moves.
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitterA, harkentest::ObjectId(77), 0.5f));
    harkentest::AdvanceVariableWrite(engine);
    EXPECT_EQ(before + 1u, engine->params.CommitCount());
    EXPECT_NEAR(VolumeDbAt(0.5f), engine->params.ResolvedValue(
                                RenderHandle(engine, voiceA),
                                harken::kTargetVoiceVolume), 1.0e-5f);
    EXPECT_NEAR(VolumeDbAt(0.75f), engine->params.ResolvedValue(
                                RenderHandle(engine, voiceB),
                                harken::kTargetVoiceVolume), 1.0e-5f);
}

// --- emitter-driven spatial ---

TEST(Emitter, DrivesPerVoiceDistanceAndFansOutAMove) {
    EngineSession session(EmitterConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallEmitterVariables(engine);
    const std::vector<float> dc = MakeDC(kRate, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    ASSERT_EQ(VN_OK, Vn_SetListener(Vec3(0.0f, 0.0f, 0.0f),
                                    Vec3(0.0f, 0.0f, -1.0f),
                                    Vec3(0.0f, 1.0f, 0.0f)));
    const VnAttenuationDesc atten = MakeAttenuationDesc(1.0f, 100.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(9), &atten));

    VnEmitterHandle nearEmitter = 0;
    VnEmitterHandle farEmitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&nearEmitter));
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&farEmitter));
    ASSERT_EQ(VN_OK, Vn_SetEmitterPosition(nearEmitter, Vec3(10.0f, 0.0f, 0.0f)));
    ASSERT_EQ(VN_OK, Vn_SetEmitterPosition(farEmitter, Vec3(50.0f, 0.0f, 0.0f)));

    // Spatialized voices attached to the two emitters; the emitter pose
    // overrides the (0,0,0) play position on the first evaluate.
    VnPlayDesc nearPlay = AttachedPlay(harkentest::ObjectId(1), nearEmitter);
    nearPlay.spatialized = 1;
    VnPlayDesc farPlay = AttachedPlay(harkentest::ObjectId(1), farEmitter);
    farPlay.spatialized = 1;
    VnVoiceHandle nearVoice = 0;
    VnVoiceHandle farVoice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&nearPlay, &nearVoice));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&farPlay, &farVoice));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(nearVoice, harkentest::ObjectId(9)));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(farVoice, harkentest::ObjectId(9)));
    ASSERT_EQ(VN_OK, Vn_Update());

    const float nearDb = engine->params.ResolvedValue(
        RenderHandle(engine, nearVoice), harken::kTargetVoiceVolume);
    const float farDb = engine->params.ResolvedValue(
        RenderHandle(engine, farVoice), harken::kTargetVoiceVolume);
    EXPECT_GT(nearDb, farDb); // closer emitter is louder
    EXPECT_LT(farDb, 0.0f);   // distance attenuates

    // Move the near emitter out to the far distance: its voice now matches the
    // far voice's attenuation (one emitter move updates its attached voice).
    ASSERT_EQ(VN_OK, Vn_SetEmitterPosition(nearEmitter, Vec3(50.0f, 0.0f, 0.0f)));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_FLOAT_EQ(farDb, engine->params.ResolvedValue(
                               RenderHandle(engine, nearVoice),
                               harken::kTargetVoiceVolume));
}

// --- contract gates ---

TEST(Emitter, AttachedVoiceRefusesPerVoiceSpatialWrites) {
    EngineSession session(EmitterConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::vector<float> dc = MakeDC(kRate, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));

    VnPlayDesc attachedPlay = AttachedPlay(harkentest::ObjectId(1), emitter);
    attachedPlay.spatialized = 1;
    VnPlayDesc freePlay = MakePlayDesc(harkentest::ObjectId(1));
    freePlay.spatialized = 1;
    VnVoiceHandle attached = 0;
    VnVoiceHandle freeVoice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&attachedPlay, &attached));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&freePlay, &freeVoice));

    // The attached voice's geometry belongs to the emitter: per-voice writes
    // are refused.
    EXPECT_EQ(VN_EMITTER_ATTACHED,
              Vn_SetVoicePosition(attached, Vec3(1.0f, 2.0f, 3.0f)));
    EXPECT_EQ(VN_EMITTER_ATTACHED,
              Vn_SetVoiceOrientation(attached, Vec3(1.0f, 0.0f, 0.0f)));
    // Refusal precedence: the attachment gate outranks argument validation, so
    // even a value that would otherwise be VN_INVALID_ARG (a non-finite
    // position, a zero-length orientation) reports the attachment state — the
    // caller learns the voice is emitter-owned regardless of what it passed.
    EXPECT_EQ(VN_EMITTER_ATTACHED,
              Vn_SetVoicePosition(attached, Vec3(kNan, 0.0f, 0.0f)));
    EXPECT_EQ(VN_EMITTER_ATTACHED,
              Vn_SetVoiceOrientation(attached, Vec3(0.0f, 0.0f, 0.0f)));
    // An emitterless voice keeps the per-voice path.
    EXPECT_EQ(VN_OK, Vn_SetVoicePosition(freeVoice, Vec3(1.0f, 2.0f, 3.0f)));
    EXPECT_EQ(VN_OK, Vn_SetVoiceOrientation(freeVoice, Vec3(1.0f, 0.0f, 0.0f)));
}

TEST(Emitter, DestroyDetachesVoiceWithFrozenStateAndReenablesPerVoice) {
    EngineSession session(EmitterConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallEmitterVariables(engine);
    const std::vector<float> dc = MakeDC(kRate, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    ASSERT_EQ(VN_OK, Vn_SetListener(Vec3(0.0f, 0.0f, 0.0f),
                                    Vec3(0.0f, 0.0f, -1.0f),
                                    Vec3(0.0f, 1.0f, 0.0f)));
    const VnAttenuationDesc atten = MakeAttenuationDesc(1.0f, 100.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(9), &atten));

    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    ASSERT_EQ(VN_OK, Vn_SetEmitterPosition(emitter, Vec3(10.0f, 0.0f, 0.0f)));

    VnPlayDesc play = AttachedPlay(harkentest::ObjectId(1), emitter);
    play.spatialized = 1;
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(voice, harkentest::ObjectId(9)));
    ASSERT_EQ(VN_OK, Vn_Update());
    const float attachedDb = engine->params.ResolvedValue(
        RenderHandle(engine, voice), harken::kTargetVoiceVolume);
    EXPECT_LT(attachedDb, 0.0f); // driven by the emitter at distance 10

    // Destroy the emitter: the voice detaches, keeps playing, and freezes at
    // its last emitter-derived state.
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(emitter));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_NE(nullptr,
              engine->voices.Resolve(RenderHandle(engine, voice))); // still live
    EXPECT_FLOAT_EQ(attachedDb, engine->params.ResolvedValue(
                                    RenderHandle(engine, voice),
                                    harken::kTargetVoiceVolume)); // frozen

    // The voice is individually addressable again: a per-voice position set is
    // now accepted and moves it, proving detachment.
    ASSERT_EQ(VN_OK, Vn_SetVoicePosition(voice, Vec3(2.0f, 0.0f, 0.0f)));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_GT(engine->params.ResolvedValue(RenderHandle(engine, voice),
                                           harken::kTargetVoiceVolume),
              attachedDb); // closer than distance 10 -> louder
}

TEST(Emitter, StaleEmitterOpsAreBenign) {
    EngineSession session(EmitterConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(emitter));

    // Every op on the destroyed handle is a benign VN_STALE_HANDLE no-op.
    EXPECT_EQ(VN_STALE_HANDLE, Vn_DestroyEmitter(emitter));
    EXPECT_EQ(VN_STALE_HANDLE,
              Vn_SetEmitterPosition(emitter, Vec3(1.0f, 0.0f, 0.0f)));
    EXPECT_EQ(VN_STALE_HANDLE,
              Vn_SetEmitterOrientation(emitter, Vec3(1.0f, 0.0f, 0.0f)));
    EXPECT_EQ(VN_STALE_HANDLE, Vn_StopEmitter(emitter, 0));
    EXPECT_EQ(VN_STALE_HANDLE, Vn_SetEmitterValue(emitter, harkentest::ObjectId(5), 1.0f));
}

TEST(Emitter, PlayNamingADeadEmitterIsRefused) {
    EngineSession session(EmitterConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::vector<float> dc = MakeDC(kRate, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(emitter));

    VnPlayDesc play = AttachedPlay(harkentest::ObjectId(1), emitter);
    VnVoiceHandle voice = 0;
    EXPECT_EQ(VN_STALE_HANDLE, harkentest::PlayRaw(&play, &voice));
    // Side-effect-free: a fresh unattached play still succeeds.
    VnPlayDesc plain = MakePlayDesc(harkentest::ObjectId(1));
    EXPECT_EQ(VN_OK, harkentest::PlayRaw(&plain, &voice));
}

TEST(Emitter, StopEmitterBatchWaitsRatherThanApplyingAPrefix) {
    EngineSession session(EmitterConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallEmitterVariables(engine);
    const std::vector<float> dc = MakeDC(kRate, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    VnPlayDesc attachedPlay = AttachedPlay(harkentest::ObjectId(1), emitter);
    VnVoiceHandle a = 0;
    VnVoiceHandle b = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&attachedPlay, &a));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&attachedPlay, &b));
    engine->nullBackend.Pump(1);

    ASSERT_EQ(VN_OK, Vn_SetBusVolume(VN_BUS_MASTER, -1.0f, 0));
    ASSERT_EQ(VN_OK, Vn_StopEmitter(emitter, 0));
    engine->mixer.SetMaxCommandsPerQuantumForTest(2);
    engine->nullBackend.Pump(1);
    EXPECT_NE(nullptr, engine->voices.Resolve(RenderHandle(engine, a)));
    EXPECT_NE(nullptr, engine->voices.Resolve(RenderHandle(engine, b)));
    EXPECT_EQ(2u, engine->commands.PublishedHead() -
                      engine->commands.ConsumedCount());

    engine->mixer.SetMaxCommandsPerQuantumForTest(2);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(nullptr, engine->voices.Resolve(RenderHandle(engine, a)));
    EXPECT_EQ(nullptr, engine->voices.Resolve(RenderHandle(engine, b)));
}

TEST(Emitter, StopEmitterStopsOnlyItsVoices) {
    EngineSession session(EmitterConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallEmitterVariables(engine);
    const std::vector<float> dc = MakeDC(kRate, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    VnPlayDesc attachedPlay = AttachedPlay(harkentest::ObjectId(1), emitter);
    VnPlayDesc freePlay = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle a = 0;
    VnVoiceHandle b = 0;
    VnVoiceHandle keep = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&attachedPlay, &a));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&attachedPlay, &b));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&freePlay, &keep));

    // Immediate stop of the emitter's voices; the unattached voice is left.
    ASSERT_EQ(VN_OK, Vn_StopEmitter(emitter, 0));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());

    EXPECT_EQ(nullptr, engine->voices.Resolve(RenderHandle(engine, a)));
    EXPECT_EQ(nullptr, engine->voices.Resolve(RenderHandle(engine, b)));
    EXPECT_NE(nullptr, engine->voices.Resolve(RenderHandle(engine, keep)));
    // The emitter itself survives StopEmitter and remains usable.
    EXPECT_EQ(VN_OK, Vn_SetEmitterPosition(emitter, Vec3(1.0f, 0.0f, 0.0f)));
}

TEST(Emitter, CapacityExhaustionRefusesWithNoEmitterAndCounts) {
    EngineSession session(EmitterConfig(/*maxVoices=*/8, /*maxEmitters=*/2));
    ASSERT_EQ(VN_OK, session.InitResult());

    VnEmitterHandle a = 0;
    VnEmitterHandle b = 0;
    VnEmitterHandle overflow = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&a));
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&b));
    EXPECT_EQ(VN_NO_EMITTER, Vn_CreateEmitter(&overflow));
    EXPECT_EQ(1u, Snap().noEmitterCount);
    EXPECT_EQ(2u, Snap().emitterHighWater);

    // Destroying one frees a slot; the next create reuses it with a distinct
    // (generationally advanced) handle.
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(a));
    VnEmitterHandle reused = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&reused));
    EXPECT_NE(reused, a);
}

// --- dirty-tracking edge cases ---

TEST(Emitter, BindAfterGlobalSetEvaluatesWithExistingValue) {
    // A binding created after its Variable already holds a value must evaluate
    // against that pre-existing value on its first pass (the never-evaluated
    // path), not wait for a later Vn_SetValue to move it.
    EngineSession session(EmitterConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallEmitterVariables(engine);
    const std::vector<float> dc = MakeDC(kRate, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));

    // Value set BEFORE the binding exists.
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(77), 0.5f));
    harkentest::AdvanceVariableWrite(engine);
    float current = 0.0f;
    ASSERT_TRUE(engine->variables.CurrentValue(harkentest::ObjectId(77), &current));
    ASSERT_FLOAT_EQ(0.5f, current);
    const harken::Curve curve = VolumeCurve();
    ASSERT_TRUE(engine->params.BindVoiceTarget(
        harkentest::ObjectId(77), RenderHandle(engine, voice), harken::kTargetVoiceVolume, curve));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_NEAR(VolumeDbAt(0.5f), engine->params.ResolvedValue(
                                RenderHandle(engine, voice),
                                harken::kTargetVoiceVolume), 1.0e-5f);
}

TEST(Emitter, SetSameValueProducesNoCommitAtEitherScope) {
    // Setting a Variable to the value it already holds must not recommit its
    // bound targets — the per-binding change detector sees no input change.
    // Checked at global scope (Vn_SetValue) and emitter scope
    // (Vn_SetEmitterValue).
    EngineSession session(EmitterConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallEmitterVariables(engine);
    const std::vector<float> dc = MakeDC(kRate, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    VnPlayDesc globalPlay = MakePlayDesc(harkentest::ObjectId(1));
    VnPlayDesc attachedPlay = AttachedPlay(harkentest::ObjectId(1), emitter);
    VnVoiceHandle unscoped = 0;
    VnVoiceHandle attached = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&globalPlay, &unscoped));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&attachedPlay, &attached));

    const harken::Curve curve = VolumeCurve();
    ASSERT_TRUE(engine->params.BindVoiceTarget(
        harkentest::ObjectId(77), RenderHandle(engine, unscoped), harken::kTargetVoiceVolume, curve));
    ASSERT_TRUE(engine->params.BindVoiceTarget(
        harkentest::ObjectId(77), RenderHandle(engine, attached), harken::kTargetVoiceVolume, curve));
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(77), 0.5f));
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, harkentest::ObjectId(77), 0.25f));
    harkentest::AdvanceVariableWrite(engine); // both commit their initial resolved values

    // Global scope: re-set the same value; no new commit.
    std::uint64_t before = engine->params.CommitCount();
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(77), 0.5f));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(before, engine->params.CommitCount());

    // Emitter scope: re-set the same local value; no new commit.
    before = engine->params.CommitCount();
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, harkentest::ObjectId(77), 0.25f));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(before, engine->params.CommitCount());
}

TEST(Emitter, LocalGlobalLocalWritesEachDetectedOnce) {
    EngineSession session(EmitterConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallEmitterVariables(engine);
    const std::vector<float> dc = MakeDC(kRate, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    VnPlayDesc play = AttachedPlay(harkentest::ObjectId(1), emitter);
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    const harken::Curve curve = VolumeCurve();
    ASSERT_TRUE(engine->params.BindVoiceTarget(
        harkentest::ObjectId(77), RenderHandle(engine, voice), harken::kTargetVoiceVolume, curve));

    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(77), 0.5f));
    harkentest::AdvanceVariableWrite(engine);
    EXPECT_NEAR(VolumeDbAt(0.5f), engine->params.ResolvedValue(
                                RenderHandle(engine, voice),
                                harken::kTargetVoiceVolume), 1.0e-5f);

    std::uint64_t before = engine->params.CommitCount();
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, harkentest::ObjectId(77), 0.25f));
    harkentest::AdvanceVariableWrite(engine);
    EXPECT_EQ(before + 1u, engine->params.CommitCount());
    EXPECT_NEAR(VolumeDbAt(0.25f), engine->params.ResolvedValue(
                               RenderHandle(engine, voice),
                               harken::kTargetVoiceVolume), 1.0e-5f);

    before = engine->params.CommitCount();
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(77), 0.5f));
    harkentest::AdvanceVariableWrite(engine);
    EXPECT_EQ(before + 1u, engine->params.CommitCount());
    EXPECT_NEAR(VolumeDbAt(0.5f), engine->params.ResolvedValue(
                                RenderHandle(engine, voice),
                                harken::kTargetVoiceVolume), 1.0e-5f);

    before = engine->params.CommitCount();
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, harkentest::ObjectId(77), 0.75f));
    harkentest::AdvanceVariableWrite(engine);
    EXPECT_EQ(before + 1u, engine->params.CommitCount());
    EXPECT_NEAR(VolumeDbAt(0.75f), engine->params.ResolvedValue(
                                RenderHandle(engine, voice),
                                harken::kTargetVoiceVolume), 1.0e-5f);
}

TEST(Emitter, DestroyWithLocalOverrideReresolvesToGlobalOnce) {
    // An emitter destroyed while its local override differs from the global
    // value: the now-detached voice's binding re-resolves to the global value
    // exactly once, then holds without further commits.
    EngineSession session(EmitterConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallEmitterVariables(engine);
    const std::vector<float> dc = MakeDC(kRate, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    VnPlayDesc play = AttachedPlay(harkentest::ObjectId(1), emitter);
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    const harken::Curve curve = VolumeCurve();
    ASSERT_TRUE(engine->params.BindVoiceTarget(
        harkentest::ObjectId(77), RenderHandle(engine, voice), harken::kTargetVoiceVolume, curve));

    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(77), 0.5f));
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, harkentest::ObjectId(77), 0.25f));
    harkentest::AdvanceVariableWrite(engine);
    EXPECT_NEAR(VolumeDbAt(0.25f), engine->params.ResolvedValue(
                               RenderHandle(engine, voice),
                               harken::kTargetVoiceVolume), 1.0e-5f);

    // Destroy: the override vanishes with the emitter, so the binding falls
    // through to the global value on the next update — one commit.
    const std::uint64_t before = engine->params.CommitCount();
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(emitter));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(before + 1u, engine->params.CommitCount());
    EXPECT_NEAR(VolumeDbAt(0.5f), engine->params.ResolvedValue(
                                RenderHandle(engine, voice),
                                harken::kTargetVoiceVolume), 1.0e-5f);

    // No further movement without a new input change.
    const std::uint64_t after = engine->params.CommitCount();
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(after, engine->params.CommitCount());
}

// --- Vn_StopEmitter with pending voices and backpressure atomicity ---

TEST(Emitter, StopEmitterCancelsPendingAttachedAndStopsLive) {
    // Mixed case: an emitter with one live attached voice and one steal-deferred
    // (pending) attached voice. Vn_StopEmitter cancels the pending start within
    // the call — no queued command — and stops the live one.
    VnConfig config = EmitterConfig(/*maxVoices=*/1);
    config.voiceStealPolicy = 2; // steal-oldest: the second play defers
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallEmitterVariables(engine);
    const std::vector<float> dc = MakeDC(kRate, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));

    VnPlayDesc play = AttachedPlay(harkentest::ObjectId(1), emitter);
    VnVoiceHandle live = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &live));
    engine->nullBackend.Pump(1); // the victim renders before it is stolen

    VnVoiceHandle pending = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &pending)); // pool full -> steal-deferred
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Pending,
              engine->ledger.Classify(pending).kind);
    ASSERT_EQ(1u, engine->ledger.PendingStartCount());

    // The pending start is cancelled synchronously (the count drops with no
    // pump), and the live voice is queued to stop.
    ASSERT_EQ(VN_OK, Vn_StopEmitter(emitter, 0));
    EXPECT_EQ(0u, engine->ledger.PendingStartCount());

    engine->nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(nullptr, engine->voices.Resolve(RenderHandle(engine, live)));
    // The cancelled pending voice never started.
    EXPECT_LT(harkentest::VoiceRowForHandle(Snap(), pending), 0);
    // The emitter itself survives StopEmitter.
    EXPECT_EQ(VN_OK, Vn_SetEmitterPosition(emitter, Vec3(1.0f, 0.0f, 0.0f)));
}

TEST(Emitter, StopEmitterBackpressureIsAtomicThenSucceedsAfterPump) {
    // When the command queue cannot hold a stop for every attached voice,
    // Vn_StopEmitter refuses atomically: VN_BACKPRESSURE, zero stops issued,
    // every voice still live, emitter unchanged. After a pump drains the queue,
    // the retry admits all the stops.
    VnConfig config = EmitterConfig(/*maxVoices=*/4);
    config.commandQueueDepth = 4; // capacity 4 after power-of-two rounding
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallEmitterVariables(engine);
    const std::vector<float> dc = MakeDC(kRate, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));

    VnPlayDesc attachedPlay = AttachedPlay(harkentest::ObjectId(1), emitter);
    VnPlayDesc freePlay = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle a = 0;
    VnVoiceHandle b = 0;
    VnVoiceHandle c = 0;
    VnVoiceHandle filler = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&attachedPlay, &a));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&attachedPlay, &b));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&attachedPlay, &c));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&freePlay, &filler));
    engine->nullBackend.Pump(1); // drain the four play commands
    ASSERT_EQ(VN_OK, Vn_Update());

    // Occupy two of the four queue slots with commands on the unattached voice,
    // leaving room for only two stops — one short of the three attached voices.
    ASSERT_EQ(VN_OK, Vn_SetVoicePosition(filler, Vec3(1.0f, 0.0f, 0.0f)));
    ASSERT_EQ(VN_OK, Vn_SetVoicePosition(filler, Vec3(2.0f, 0.0f, 0.0f)));

    EXPECT_EQ(VN_BACKPRESSURE, Vn_StopEmitter(emitter, 0));
    // Zero stops issued: draining and updating leaves every attached voice (and
    // the filler) live.
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_NE(nullptr, engine->voices.Resolve(RenderHandle(engine, a)));
    EXPECT_NE(nullptr, engine->voices.Resolve(RenderHandle(engine, b)));
    EXPECT_NE(nullptr, engine->voices.Resolve(RenderHandle(engine, c)));
    EXPECT_NE(nullptr, engine->voices.Resolve(RenderHandle(engine, filler)));

    // Queue drained: the retry now admits all three stops.
    ASSERT_EQ(VN_OK, Vn_StopEmitter(emitter, 0));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(nullptr, engine->voices.Resolve(RenderHandle(engine, a)));
    EXPECT_EQ(nullptr, engine->voices.Resolve(RenderHandle(engine, b)));
    EXPECT_EQ(nullptr, engine->voices.Resolve(RenderHandle(engine, c)));
    EXPECT_NE(nullptr, engine->voices.Resolve(RenderHandle(engine, filler)));
}

// --- captured-output E2E coverage ---

float CapturedSample(const harkentest::Capture& capture, bool right) {
    const std::vector<float>& channel = right ? capture.right : capture.left;
    return channel.empty() ? 0.0f : std::fabs(channel.back());
}

std::vector<float> MakeStereoDC(std::size_t frames, float left, float right) {
    std::vector<float> samples(frames * 2u);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        samples[frame * 2u] = left;
        samples[frame * 2u + 1u] = right;
    }
    return samples;
}

TEST(Emitter, RenderOutputPanAttenuationAndListenerPose) {
    EngineSession session(EmitterConfig(/*maxVoices=*/4));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallEmitterVariables(engine);

    constexpr float kAmplitude = 0.2f;
    const std::vector<float> source = MakeDC(kBlock * 4u, kAmplitude);
    const VnPCMDesc sourceDesc = MonoDesc(source, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &sourceDesc));
    const VnAttenuationDesc attenuation = MakeAttenuationDesc(1.0f, 100.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(9), &attenuation));
    ASSERT_EQ(VN_OK,
              Vn_SetListener(Vec3(0.0f, 0.0f, 0.0f),
                             Vec3(0.0f, 0.0f, -1.0f),
                             Vec3(0.0f, 1.0f, 0.0f)));

    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    ASSERT_EQ(VN_OK, Vn_SetEmitterPosition(emitter, Vec3(-2.0f, 0.0f, 0.0f)));

    VnPlayDesc play = AttachedPlay(harkentest::ObjectId(1), emitter);
    play.spatialized = 1;
    play.loopCount = 0;
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    ASSERT_EQ(VN_OK, Vn_Update());

    // The first evaluation uses a zero-length transition: installation snaps
    // to the left pose before the first captured frame.
    harkentest::Capture left;
    harkentest::PumpAndCapture(engine, 1, left);
    ASSERT_EQ(engine->nullBackend.BlockFrames(), left.left.size());
    HARKENTEST_ASSERT_NON_SILENT(left.left);
    EXPECT_NEAR(left.left.front(), left.left.back(), 1e-6f);
    EXPECT_NEAR(left.right.front(), left.right.back(), 1e-6f);
    EXPECT_GT(CapturedSample(left, false), CapturedSample(left, true) * 100.0f);

    const auto moveEmitterAndCapture = [&](VnVec3 position) {
        EXPECT_EQ(VN_OK, Vn_SetEmitterPosition(emitter, position));
        EXPECT_EQ(VN_OK, Vn_Update());
        harkentest::Capture capture;
        harkentest::PumpAndCapture(engine, 3, capture);
        return capture;
    };

    // Equal-distance left, center, and right poses exercise the point-source
    // pan through the public emitter attachment.
    const harkentest::Capture center =
        moveEmitterAndCapture(Vec3(0.0f, 0.0f, -2.0f));
    EXPECT_NEAR(CapturedSample(center, false), CapturedSample(center, true),
                1e-5f);
    const harkentest::Capture right =
        moveEmitterAndCapture(Vec3(2.0f, 0.0f, 0.0f));
    EXPECT_GT(CapturedSample(right, true),
              CapturedSample(right, false) * 100.0f);

    // Moving the listener changes the relative azimuth without touching the
    // emitter. Rotating it changes the right axis and therefore the pan.
    moveEmitterAndCapture(Vec3(0.0f, 0.0f, -2.0f));

    ASSERT_EQ(VN_OK,
              Vn_SetListener(Vec3(2.0f, 0.0f, 0.0f),
                             Vec3(0.0f, 0.0f, -1.0f),
                             Vec3(0.0f, 1.0f, 0.0f)));
    ASSERT_EQ(VN_OK, Vn_Update());
    harkentest::Capture listenerMoved;
    harkentest::PumpAndCapture(engine, 3, listenerMoved);
    EXPECT_GT(CapturedSample(listenerMoved, false),
              CapturedSample(listenerMoved, true) * 2.0f);

    ASSERT_EQ(VN_OK,
              Vn_SetListener(Vec3(0.0f, 0.0f, 0.0f),
                             Vec3(1.0f, 0.0f, 0.0f),
                             Vec3(0.0f, 1.0f, 0.0f)));
    ASSERT_EQ(VN_OK, Vn_Update());
    harkentest::Capture listenerRotated;
    harkentest::PumpAndCapture(engine, 3, listenerRotated);
    EXPECT_GT(CapturedSample(listenerRotated, false),
              CapturedSample(listenerRotated, true) * 100.0f);

    ASSERT_EQ(VN_OK,
              Vn_SetListener(Vec3(0.0f, 0.0f, 0.0f),
                             Vec3(0.0f, 0.0f, -1.0f),
                             Vec3(0.0f, 1.0f, 0.0f)));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(voice, harkentest::ObjectId(9)));
    ASSERT_EQ(VN_OK, Vn_Update());

    // The same source at near, mid, and far distances must monotonically
    // attenuate. The total stereo power avoids making pan part of this check.
    const auto stereoPower = [](const harkentest::Capture& capture) {
        const float leftSample = CapturedSample(capture, false);
        const float rightSample = CapturedSample(capture, true);
        return std::sqrt(leftSample * leftSample + rightSample * rightSample);
    };
    const harkentest::Capture nearCapture =
        moveEmitterAndCapture(Vec3(0.0f, 0.0f, -1.0f));
    const harkentest::Capture midCapture =
        moveEmitterAndCapture(Vec3(0.0f, 0.0f, -10.0f));
    const harkentest::Capture farCapture =
        moveEmitterAndCapture(Vec3(0.0f, 0.0f, -90.0f));
    EXPECT_GT(stereoPower(nearCapture), stereoPower(midCapture));
    EXPECT_GT(stereoPower(midCapture), stereoPower(farCapture));
    EXPECT_GT(stereoPower(farCapture), harkentest::kSilenceFloor);
}

TEST(Emitter, RenderOutputLiveTransitionAndFanOut) {
    EngineSession session(EmitterConfig(/*maxVoices=*/4));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallEmitterVariables(engine);

    constexpr float kAmplitude = 0.1f;
    const std::vector<float> source = MakeDC(kBlock * 4u, kAmplitude);
    const VnPCMDesc sourceDesc = MonoDesc(source, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &sourceDesc));
    ASSERT_EQ(VN_OK,
              Vn_SetListener(Vec3(0.0f, 0.0f, 0.0f),
                             Vec3(0.0f, 0.0f, -1.0f),
                             Vec3(0.0f, 1.0f, 0.0f)));

    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    ASSERT_EQ(VN_OK, Vn_SetEmitterPosition(emitter, Vec3(-2.0f, 0.0f, 0.0f)));

    VnPlayDesc play = AttachedPlay(harkentest::ObjectId(1), emitter);
    play.spatialized = 1;
    play.loopCount = 0;
    VnVoiceHandle firstVoice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &firstVoice));
    ASSERT_EQ(VN_OK, Vn_Update());
    harkentest::Capture oneVoice;
    harkentest::PumpAndCapture(engine, 2, oneVoice);
    const float oneVoiceLeft = CapturedSample(oneVoice, false);
    HARKENTEST_ASSERT_NON_SILENT(oneVoice.left);

    VnVoiceHandle secondVoice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &secondVoice));
    ASSERT_EQ(VN_OK, Vn_Update());
    harkentest::Capture twoVoices;
    harkentest::PumpAndCapture(engine, 2, twoVoices);
    const float twoVoiceLeft = CapturedSample(twoVoices, false);
    // Two attached voices render through the same emitter pose and therefore
    // fan out as two copies of the source rather than one shared slot.
    EXPECT_GT(twoVoiceLeft, oneVoiceLeft * 1.8f);

    ASSERT_EQ(VN_OK, Vn_SetEmitterPosition(emitter, Vec3(2.0f, 0.0f, 0.0f)));
    ASSERT_EQ(VN_OK, Vn_Update());
    harkentest::Capture transition;
    harkentest::PumpAndCapture(engine, 1, transition);
    HARKENTEST_ASSERT_NON_SILENT(transition.left);
    HARKENTEST_ASSERT_NON_SILENT(transition.right);
    // The live retarget is non-snap: the first block starts on the old left
    // image, contains audible right output, and settles within four blocks.
    EXPECT_GT(std::fabs(transition.left.front()),
              std::fabs(transition.right.front()));
    EXPECT_GT(std::fabs(transition.left.back()), harkentest::kSilenceFloor);
    EXPECT_GT(std::fabs(transition.right.back()), harkentest::kSilenceFloor);

    harkentest::Capture settled;
    harkentest::PumpAndCapture(engine, 3, settled);
    EXPECT_GT(CapturedSample(settled, true),
              CapturedSample(settled, false) * 10.0f);
}

TEST(Emitter, RenderOutputParkReturn) {
    VnConfig config = EmitterConfig(/*maxVoices=*/4);
    config.maxParkedVoices = 2;
    config.audibilityParkEnabled = 1;
    config.parkThresholdDb = -40.0f;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallEmitterVariables(engine);

    const std::vector<float> mono = MakeDC(kBlock * 4u, 0.25f);
    const VnPCMDesc monoDesc = MonoDesc(mono, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &monoDesc));
    const VnAttenuationDesc attenuation = MakeAttenuationDesc(1.0f, 100.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(9), &attenuation));
    ASSERT_EQ(VN_OK,
              Vn_SetListener(Vec3(0.0f, 0.0f, 0.0f),
                             Vec3(0.0f, 0.0f, -1.0f),
                             Vec3(0.0f, 1.0f, 0.0f)));

    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    ASSERT_EQ(VN_OK, Vn_SetEmitterPosition(emitter, Vec3(100.0f, 0.0f, 0.0f)));
    VnPlayDesc play = AttachedPlay(harkentest::ObjectId(1), emitter);
    play.spatialized = 1;
    play.loopCount = 0;
    VnVoiceHandle parkedVoice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &parkedVoice));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(parkedVoice, harkentest::ObjectId(9)));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    ASSERT_EQ(1u, Snap().parkedVoiceCount);

    ASSERT_EQ(VN_OK, Vn_SetEmitterPosition(emitter, Vec3(1.0f, 0.0f, 0.0f)));
    ASSERT_EQ(VN_OK, Vn_Update());
    harkentest::Capture returned;
    harkentest::PumpAndCapture(engine, 2, returned);
    HARKENTEST_ASSERT_NON_SILENT(returned.left);
    HARKENTEST_ASSERT_NON_SILENT(returned.right);
    EXPECT_EQ(0u, Snap().parkedVoiceCount);

    ASSERT_EQ(VN_OK, Vn_Stop(parkedVoice, 0));
    engine->nullBackend.Pump(1);
}


VnEmitterHandle CreateStateOnly() {
    VnEmitterCreateDescV1 desc{};
    desc.structSize = sizeof(desc);
    desc.flags = VN_EMITTER_CREATE_STATE_ONLY;
    VnEmitterHandle handle = 0;
    EXPECT_EQ(VN_OK, Vn_CreateEmitterEx(&desc, &handle));
    return handle;
}

TEST(Emitter, StateOnlyPreservesVoicePoseAndOwnsStops) {
    EngineSession session(EmitterConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    const auto samples = MakeDC(kRate, 0.25f);
    const auto pcm = MonoDesc(samples, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    const auto state = CreateStateOnly();
    ASSERT_NE(0u, state);
    EXPECT_EQ(VN_CAPABILITY_UNAVAILABLE, Vn_SetEmitterPosition(state, Vec3(3, 0, 0)));
    EXPECT_EQ(VN_CAPABILITY_UNAVAILABLE, Vn_SetEmitterOrientation(state, Vec3(0, 0, 1)));
    auto play = AttachedPlay(harkentest::ObjectId(1), state);
    play.spatialized = 1;
    play.positionX = 3.0f;
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    const auto render = RenderHandle(engine, voice);
    EXPECT_EQ(state, engine->params.EmitterHandleOf(render));
    EXPECT_EQ(0u, engine->spatial.AttachedEmitter(render));
    EXPECT_EQ(VN_OK, Vn_SetVoicePosition(voice, Vec3(5, 0, -2)));
    EXPECT_EQ(VN_OK, Vn_SetVoiceOrientation(voice, Vec3(0, 0, 1)));
    ASSERT_EQ(VN_OK, Vn_Update());
    harken::Spatializer::SpatialVoiceState spatial{};
    ASSERT_TRUE(engine->spatial.CaptureVoice(render, &spatial));
    EXPECT_FLOAT_EQ(5.0f, spatial.posX);
    EXPECT_FLOAT_EQ(-2.0f, spatial.posZ);
    harkentest::Capture capture;
    harkentest::PumpAndCapture(engine, 2, capture);
    HARKENTEST_ASSERT_NON_SILENT(capture.left);
    ASSERT_EQ(VN_OK, Vn_StopEmitter(state, 0));
    engine->nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(0u, engine->voices.ActiveCount());
    EXPECT_EQ(VN_OK, Vn_DestroyEmitter(state));
}

TEST(Emitter, StateOnlyPendingVoiceAcceptsPoseAndStopsWithItsOwner) {
    auto config = EmitterConfig(1);
    config.voiceStealPolicy = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    const auto samples = MakeDC(kRate, 0.25f);
    const auto pcm = MonoDesc(samples, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    const auto state = CreateStateOnly();
    auto play = AttachedPlay(harkentest::ObjectId(1), state);
    play.spatialized = 1;
    VnVoiceHandle live = 0, pending = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &live));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &pending));
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Pending, engine->ledger.Classify(pending).kind);
    EXPECT_EQ(VN_OK, Vn_SetVoicePosition(pending, Vec3(5, 0, 0)));
    EXPECT_EQ(VN_OK, Vn_SetVoiceOrientation(pending, Vec3(0, 0, 1)));
    ASSERT_EQ(VN_OK, Vn_StopEmitter(state, 0));
    EXPECT_EQ(0u, engine->ledger.PendingStartCount());
    engine->nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(0u, engine->voices.ActiveCount());
}

TEST(Emitter, StateOnlyPendingPromotionPreservesPoseAndVariableContext) {
    auto config = EmitterConfig(1); config.voiceStealPolicy = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    InstallEmitterVariables(engine);
    const auto samples = MakeDC(kRate, 0.25f);
    const auto pcm = MonoDesc(samples, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    const auto state = CreateStateOnly();
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(state, harkentest::ObjectId(77), 0.75f));
    harkentest::AdvanceVariableWrite(engine);
    auto play = AttachedPlay(harkentest::ObjectId(1), state); play.spatialized = 1;
    VnVoiceHandle first = 0, pending = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &first));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &pending));
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Pending, engine->ledger.Classify(pending).kind);
    ASSERT_EQ(VN_OK, Vn_SetVoicePosition(pending, Vec3(5, 0, -2)));
    for (unsigned i = 0; i < 30 && engine->ledger.Classify(pending).kind == harken::VoiceLedger::HandleKind::Pending; ++i) {
        engine->nullBackend.Pump(1); ASSERT_EQ(VN_OK, Vn_Update());
    }
    const auto render = RenderHandle(engine, pending);
    ASSERT_NE(0u, render);
    EXPECT_EQ(state, engine->params.EmitterHandleOf(render));
    EXPECT_EQ(0u, engine->spatial.AttachedEmitter(render));
    harken::Spatializer::SpatialVoiceState pose{};
    ASSERT_TRUE(engine->spatial.CaptureVoice(render, &pose));
    EXPECT_FLOAT_EQ(5.0f, pose.posX); EXPECT_FLOAT_EQ(-2.0f, pose.posZ);
    ASSERT_TRUE(engine->params.BindVoiceTarget(harkentest::ObjectId(77), render, harken::kTargetVoiceVolume, VolumeCurve()));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_NEAR(VolumeDbAt(0.75f), engine->params.ResolvedValue(render, harken::kTargetVoiceVolume), 1e-5f);
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(state));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_NEAR(VolumeDbAt(0), engine->params.ResolvedValue(render, harken::kTargetVoiceVolume), 1e-5f);
}

TEST(Emitter, StateOnlyParkReturnPreservesOwnerAndPose) {
    auto config = EmitterConfig(4);
    config.maxParkedVoices = 2;
    config.audibilityParkEnabled = 1;
    config.parkThresholdDb = -40.0f;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    InstallEmitterVariables(engine);
    const auto samples = MakeDC(kBlock * 4u, 0.25f);
    const auto pcm = MonoDesc(samples, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    const auto attenuation = MakeAttenuationDesc(1.0f, 100.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(9), &attenuation));
    const auto state = CreateStateOnly();
    auto play = AttachedPlay(harkentest::ObjectId(1), state);
    play.spatialized = 1;
    play.positionX = 100.0f;
    play.loopCount = 0;
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(voice, harkentest::ObjectId(9)));
    ASSERT_TRUE(engine->params.BindVoiceTarget(harkentest::ObjectId(77), RenderHandle(engine, voice), harken::kTargetVoiceVolume, VolumeCurve()));
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(state, harkentest::ObjectId(77), 0.25f));
    harkentest::AdvanceVariableWrite(engine);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    const auto parked = engine->ledger.Classify(voice);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked, parked.kind);
    EXPECT_EQ(state, engine->parkRecords[parked.ledgerSlot].stateEmitter);
    EXPECT_EQ(0u, engine->parkRecords[parked.ledgerSlot].spatial.emitterHandle);
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(state, harkentest::ObjectId(77), 0.75f));
    harkentest::AdvanceVariableWrite(engine);
    ASSERT_EQ(VN_OK, Vn_SetVoicePosition(voice, Vec3(1, 0, 0)));
    ASSERT_EQ(VN_OK, Vn_Update());
    harkentest::Capture capture;
    harkentest::PumpAndCapture(engine, 2, capture);
    HARKENTEST_ASSERT_NON_SILENT(capture.left);
    EXPECT_NEAR(VolumeDbAt(0.75f), engine->params.ResolvedValue(RenderHandle(engine, voice), harken::kTargetVoiceVolume), 1.0e-5f);
    EXPECT_EQ(state, engine->params.EmitterHandleOf(RenderHandle(engine, voice)));
    EXPECT_EQ(0u, engine->spatial.AttachedEmitter(RenderHandle(engine, voice)));
    ASSERT_EQ(VN_OK, Vn_SetVoicePosition(voice, Vec3(100, 0, 0)));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked, engine->ledger.Classify(voice).kind);
    ASSERT_EQ(VN_OK, Vn_StopEmitter(state, 0));
    EXPECT_EQ(0u, engine->ledger.ParkedCount());
}

TEST(Emitter, DefaultStateIsPrivateAndDoesNotConsumePublicCapacity) {
    EngineSession session(EmitterConfig(4, 2));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    const auto privateState = engine->emitters.DefaultState();
    EXPECT_EQ(2u, static_cast<std::uint32_t>(privateState));
    EXPECT_EQ(0u, engine->emitters.ActiveCount());
    EXPECT_EQ(VN_STALE_HANDLE, Vn_DestroyEmitter(privateState));
    EXPECT_EQ(VN_STALE_HANDLE, Vn_StopEmitter(privateState, 0));
    EXPECT_EQ(VN_STALE_HANDLE, Vn_SetEmitterPosition(privateState, Vec3(1, 0, 0)));
    EXPECT_EQ(VN_STALE_HANDLE, Vn_SetEmitterValue(privateState, harkentest::ObjectId(7), 0.5f));
    VnEmitterHandle ordinary = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&ordinary));
    EXPECT_EQ(0u, static_cast<std::uint32_t>(ordinary));
    const auto state = CreateStateOnly();
    EXPECT_EQ(1u, static_cast<std::uint32_t>(state));
    VnEmitterHandle refused = 99;
    EXPECT_EQ(VN_NO_EMITTER, Vn_CreateEmitter(&refused));
    EXPECT_EQ(0u, refused);
    EXPECT_EQ(2u, engine->emitters.ActiveCount());
    EXPECT_TRUE(engine->emitters.IsLive(privateState));
    const auto samples = MakeDC(kBlock, 0.25f);
    const auto pcm = MonoDesc(samples, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    auto raw = AttachedPlay(harkentest::ObjectId(1), privateState);
    VnVoiceHandle voice = 0;
    EXPECT_EQ(VN_STALE_HANDLE, harkentest::PlayRaw(&raw, &voice));
    raw.emitterHandle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&raw, &voice));
    EXPECT_EQ(0u, engine->params.EmitterHandleOf(RenderHandle(engine, voice)));
}

TEST(Emitter, CreateExRejectsInvalidDescriptorsWithoutAllocating) {
    EngineSession session(EmitterConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    VnEmitterCreateDescV1 desc{};
    VnEmitterHandle handle = 99;
    EXPECT_EQ(VN_INVALID_ARG, Vn_CreateEmitterEx(&desc, &handle));
    EXPECT_EQ(0u, handle);
    desc.structSize = sizeof(desc);
    desc.flags = 2;
    EXPECT_EQ(VN_INVALID_ARG, Vn_CreateEmitterEx(&desc, &handle));
    desc.flags = 0;
    desc.reserved0 = 1;
    EXPECT_EQ(VN_INVALID_ARG, Vn_CreateEmitterEx(&desc, &handle));
    EXPECT_EQ(0u, session.Engine()->emitters.ActiveCount());
    desc.reserved0 = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitterEx(&desc, &handle));
    EXPECT_EQ(VN_OK, Vn_SetEmitterPosition(handle, Vec3(2, 0, 0)));
}

TEST(MonitorEmitterIdentity, RawVoiceRetainsAcceptedOriginAcrossEmitterReuse) {
    EngineSession session(EmitterConfig(4, 1));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    auto& hosts = engine.monitorEmitters;
    const auto sdk = hosts.BeginSession();
    VnEmitterHandle first = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&first));
    ASSERT_TRUE(hosts.Register(hosts.EngineInstance(), sdk, first, 1,
        harken::MonitorHostOwnership::UnityEmitter, 2));
    const auto samples = MakeDC(kRate, 0.25f);
    const auto pcm = MonoDesc(samples, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    auto play = AttachedPlay(harkentest::ObjectId(1), first);
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    const auto owner = engine.ledger.Classify(voice);
    harken::VoiceLedger::MonitorIdentity accepted{};
    ASSERT_TRUE(engine.ledger.CaptureMonitorIdentity(owner.ledgerSlot, &accepted));
    ASSERT_EQ(first, accepted.emitterOrigin.emitter);
    ASSERT_EQ(1u, accepted.emitterOrigin.host.localSequence);
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(first));
    VnEmitterHandle next = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&next));
    ASSERT_NE(first, next);
    ASSERT_EQ(static_cast<std::uint32_t>(first), static_cast<std::uint32_t>(next));
    ASSERT_TRUE(hosts.Register(hosts.EngineInstance(), sdk, next, 2,
        harken::MonitorHostOwnership::UnitySpatialSource, 1));
    engine.nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    harken::VoiceLedger::MonitorIdentity retained{};
    ASSERT_TRUE(engine.ledger.CaptureMonitorIdentity(owner.ledgerSlot, &retained));
    EXPECT_EQ(accepted.logicalHandle, retained.logicalHandle);
    EXPECT_EQ(accepted.emitterOrigin.host, retained.emitterOrigin.host);
    EXPECT_EQ(first, retained.emitterOrigin.emitter);
    EXPECT_EQ(2u, retained.emitterOrigin.registrationRevision);
    play.emitterHandle = 0;
    VnVoiceHandle unbound = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &unbound));
    const auto unboundOwner = engine.ledger.Classify(unbound);
    harken::VoiceLedger::MonitorIdentity unboundIdentity{};
    ASSERT_TRUE(engine.ledger.CaptureMonitorIdentity(unboundOwner.ledgerSlot, &unboundIdentity));
    EXPECT_EQ(harken::MonitorEmitterKind::None, unboundIdentity.emitterOrigin.kind);
    EXPECT_EQ(0u, unboundIdentity.emitterOrigin.emitter);
    EXPECT_FALSE(unboundIdentity.emitterOrigin.host.Valid());
}

} // namespace
