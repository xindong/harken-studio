// Cascade bus destroy: rendered-output proof that destroying a mid-tree bus
// tombstones its whole subtree in one swap, migrates routed voices to the
// surviving parent, disconnects sends into the cascade, retires installed
// effect instances, retires meter probes, frees the ids for fresh
// creates, keeps snapshot busPeaks dense-indexed with dead slots reading 0,
// and recomputes solo audibility when a soloed member dies — plus the
// refusal matrix and backpressure atomicity.
//
// Tolerances follow BusMixGoldenTest: DC through <= 3 gain stages stays
// within 1e-6 of the closed form; 1e-5 leaves headroom while failing on any
// wrong audibility or migration (each shifts the sum by a whole
// contribution).
#include "Common/ObjectIdTestSupport.h"
#include "Engine.h"
#include "Common/TestUtil.h"
#include "harken/param/EffectParamLocator.h"
#include "harken/render/BuiltinEffects.h"

#include <cmath>
#include <cstddef>
#include <new>

#include "UnitTest/AllocationProbe.h"

namespace {

using harkentest::Capture;
using harkentest::EngineSession;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::MonoDesc;
using harkentest::PumpAndCapture;

constexpr std::uint32_t kBlockFrames = 256;
constexpr VnBusId kBusA = harkentest::ObjectId(10);
constexpr VnBusId kBusB = harkentest::ObjectId(11);
constexpr VnBusId kBusC = harkentest::ObjectId(12);
constexpr VnVariableId kEffectVariable = harkentest::ObjectId(700);
constexpr VnBindingId kEffectBinding = harkentest::ObjectId(701);

void InstallEffectVariable(harken::Engine* engine) {
    const harken::VariableDefinitionV1 definition =
        harkentest::MakeVariableDefinition(kEffectVariable,
                                         harken::VariableKindV1::Smooth,
                                         0.0f, 1.0f, 0.0f);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallDefinitionsWorld(
                  engine, &definition, 1).status);
}

// 20*log10(0.5): requests exactly half amplitude.
const float kHalfDb = 20.0f * std::log10(0.5f);

Capture PumpBlock(harken::Engine* engine) {
    Capture out;
    PumpAndCapture(engine, 1, out);
    return out;
}

void RegisterDcVoiceOnBus(VnSoundId soundId, float amplitude, VnBusId busId,
                          std::vector<float>& storage) {
    storage = harkentest::MakeDC(kBlockFrames, amplitude);
    const VnPCMDesc desc = MonoDesc(storage, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(soundId, &desc));
    VnPlayDesc play = MakePlayDesc(soundId);
    play.busId = busId;
    play.loopCount = 0; // loop so every captured block carries the DC
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
}

VnSnapshot Snap() {
    VnSnapshot s{};
    s.structSize = sizeof(VnSnapshot);
    EXPECT_EQ(VN_OK, Vn_CaptureSnapshot(&s));
    return s;
}

VnGainDesc GainDesc(float gainDb) {
    VnGainDesc desc{};
    desc.structSize = sizeof(VnGainDesc);
    desc.gainDb = gainDb;
    return desc;
}

VnBindingDesc BusGainBindingDesc() {
    VnBindingDesc desc{};
    desc.structSize = sizeof(VnBindingDesc);
    desc.variableId = kEffectVariable;
    desc.targetScopeKind = VN_TARGET_SCOPE_BUS_EFFECT_PARAM;
    desc.targetId = harken::PackEffectParamLocator(
        VN_EFFECT_GAIN, 0, harken::kGainParamGainDb);
    desc.curve.structSize = sizeof(VnCurveDesc);
    desc.curve.pointCount = 2;
    desc.curve.points[0] = {0.0f, 0.0f};
    desc.curve.points[1] = {1.0f, 6.0f};
    desc.curve.handles[0] = harkentest::EndpointHandles(
        desc.curve.points[0], desc.curve.points[1]);
    return desc;
}

VnBindingAttachDesc BusEffectAttachment(VnBusId busId) {
    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(VnBindingAttachDesc);
    attach.scopeKind = VN_TARGET_SCOPE_BUS_EFFECT_PARAM;
    attach.objectId = busId;
    return attach;
}

TEST(BusDestroy, CascadeTombstonesSubtreeAndMigratesRoutedVoices) {
    EngineSession session(MakeNullConfig(4, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    // master <- A <- B, C a sibling of A; gains halve at A and B.
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, kBusA));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusC, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(kBusA, kHalfDb, 0));
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(kBusB, kHalfDb, 0));

    std::vector<float> dcB;
    std::vector<float> dcC;
    RegisterDcVoiceOnBus(harkentest::ObjectId(1), 0.6f, kBusB, dcB);
    RegisterDcVoiceOnBus(harkentest::ObjectId(2), 0.25f, kBusC, dcC);
    Capture out = PumpBlock(session.Engine());
    // 0.6 * 0.5 (B) * 0.5 (A) + 0.25 (C at unity).
    ASSERT_NEAR(0.4f, out.left[kBlockFrames - 1], 1e-5f);

    // Destroying A cascades to B; the voice on B migrates its dry output to
    // A's surviving parent (the master) in the same swap, so it keeps
    // sounding — now without the destroyed chain's gains.
    ASSERT_EQ(VN_OK, Vn_DestroyBus(kBusA));
    out = PumpBlock(session.Engine());
    ASSERT_NEAR(0.85f, out.left[kBlockFrames - 1], 1e-5f);

    // Both ids resolve nowhere from the moment the destroy returned.
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_SetBusVolume(kBusA, 0.0f, 0));
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_SetBusVolume(kBusB, 0.0f, 0));
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_DestroyBus(kBusA));
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_DestroyBus(kBusB));
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_ReparentBus(kBusB, VN_BUS_MASTER));

    // Snapshot: busCount keeps counting slots; the dead slots' peaks read 0
    // while the survivors keep reporting.
    const VnSnapshot s = Snap();
    EXPECT_EQ(4u, s.busCount); // master + A + B + C (A and B tombstoned)
    EXPECT_EQ(0.0f, s.busPeaks[1]); // A
    EXPECT_EQ(0.0f, s.busPeaks[2]); // B
    EXPECT_GT(s.busPeaks[3], 0.2f); // C still mixing
    for (std::uint32_t channel = 0; channel < VN_MAX_CHANNELS; ++channel) {
        EXPECT_FLOAT_EQ(s.busChannelPeaks[VN_MAX_CHANNELS + channel], 0.0f);
        EXPECT_FLOAT_EQ(s.busChannelPeaks[2 * VN_MAX_CHANNELS + channel], 0.0f);
    }
    EXPECT_GT(s.busChannelPeaks[3 * VN_MAX_CHANNELS], 0.2f);
}

TEST(BusDestroy, DestroyedIdRecreatesAsAFreshBus) {
    EngineSession session(MakeNullConfig(2, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    harken::Engine* engine = session.Engine();
    const std::uint32_t oldIndex = engine->buses.FindIndex(kBusA);
    const std::uint32_t oldGeneration =
        engine->buses.GenerationAt(oldIndex);
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(kBusA, kHalfDb, 0));
    const harken::VariableSliceRef oldOwner{0, 1};
    ASSERT_TRUE(engine->busTargets.TryClaim(
        oldIndex, oldGeneration, harken::kBusTargetBusVolume, oldOwner,
        engine->busTargets.TargetRevision(
            oldIndex, oldGeneration, harken::kBusTargetBusVolume)));
    ASSERT_EQ(VN_OK, Vn_DestroyBus(kBusA));
    ASSERT_EQ(1u, engine->busTargets.RetiringTargetCountForTests());
    Capture warm = PumpBlock(session.Engine());
    ASSERT_EQ(VN_OK, Vn_Update());

    // The id is free again; the recreate uses a fresh generation with default
    // state (unity gain), not the destroyed generation's half gain.
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    const std::uint32_t newIndex = engine->buses.FindIndex(kBusA);
    const std::uint32_t newGeneration =
        engine->buses.GenerationAt(newIndex);
    ASSERT_EQ(oldIndex, newIndex);
    ASSERT_NE(oldGeneration, newGeneration);
    const harken::VariableSliceRef newOwner{1, 1};
    ASSERT_TRUE(engine->busTargets.TryClaim(
        newIndex, newGeneration, harken::kBusTargetBusVolume, newOwner,
        engine->busTargets.TargetRevision(
            newIndex, newGeneration, harken::kBusTargetBusVolume)));
    float oldResolved = 0.0f;
    std::uint32_t oldRampMs = 0;
    std::uint32_t oldRevision = 0;
    ASSERT_TRUE(engine->busTargets.SnapshotForClaim(
        oldIndex, oldGeneration, harken::kBusTargetBusVolume, oldOwner,
        &oldResolved, &oldRampMs, &oldRevision));
    EXPECT_FLOAT_EQ(kHalfDb, oldResolved);
    engine->busTargets.ReleaseClaim(
        oldIndex, oldGeneration, harken::kBusTargetBusVolume, oldOwner);
    EXPECT_EQ(0u, engine->busTargets.RetiringTargetCountForTests());
    EXPECT_TRUE(engine->busTargets.ForeignClaim(
        newIndex, newGeneration, harken::kBusTargetBusVolume));
    engine->busTargets.ReleaseClaim(
        newIndex, newGeneration, harken::kBusTargetBusVolume, newOwner);
    std::vector<float> dc;
    RegisterDcVoiceOnBus(harkentest::ObjectId(1), 0.6f, kBusA, dc);
    Capture out = PumpBlock(session.Engine());
    ASSERT_NEAR(0.6f, out.left[kBlockFrames - 1], 1e-5f);

    // Slot accounting: the freed dense slot is reused.
    EXPECT_EQ(2u, Snap().busCount);
}

TEST(BusDestroy, SendsIntoTheCascadeDisconnect) {
    EngineSession session(MakeNullConfig(2, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_ConnectBusSend(kBusA, kBusB, 0.0f));

    std::vector<float> dc;
    RegisterDcVoiceOnBus(harkentest::ObjectId(1), 0.3f, kBusA, dc);
    Capture out = PumpBlock(session.Engine());
    // Dry through A plus the unity send copy through B: double.
    ASSERT_NEAR(0.6f, out.left[kBlockFrames - 1], 1e-5f);

    // Destroying the send target drops the edge with the cascade; the dry
    // path is untouched.
    ASSERT_EQ(VN_OK, Vn_DestroyBus(kBusB));
    out = PumpBlock(session.Engine());
    ASSERT_NEAR(0.3f, out.left[kBlockFrames - 1], 1e-5f);
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_SetBusSendLevel(kBusA, kBusB, -6.0f, 0));
}

TEST(BusDestroy, VoiceSendsTargetingTheCascadeDeactivate) {
    EngineSession session(MakeNullConfig(2, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, VN_BUS_MASTER));

    std::vector<float> dc = harkentest::MakeDC(kBlockFrames, 0.3f);
    const VnPCMDesc desc = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    play.loopCount = 0;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_SetVoiceSend(handle, 0, kBusB, 0.0f));
    ASSERT_EQ(1u, session.Engine()->voiceSendActive);
    Capture out = PumpBlock(session.Engine());
    // Dry on the master plus the unity send copy through B: double.
    ASSERT_NEAR(0.6f, out.left[kBlockFrames - 1], 1e-5f);

    ASSERT_EQ(VN_OK, Vn_DestroyBus(kBusB));
    EXPECT_EQ(0u, session.Engine()->voiceSendActive);
    out = PumpBlock(session.Engine());
    ASSERT_NEAR(0.3f, out.left[kBlockFrames - 1], 1e-5f);
    // The staged mirror cleared with the swap: the slot holds no send.
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceSendLevel(handle, 0, -6.0f, 0));
}

TEST(BusDestroy, PendingAndParkedSendMirrorsDropCascadeTargets) {
    EngineSession session(MakeNullConfig(2, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, kBusA));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusC, VN_BUS_MASTER));

    harken::Engine* engine = session.Engine();
    ASSERT_TRUE(engine->pendingStarts.Emplace(0));
    ASSERT_TRUE(engine->parkRecords.Emplace(0));

    harken::PendingStartRecord& pending = engine->pendingStarts[0];
    pending.active = true;
    pending.busIndex = 2;
    pending.voiceSends[0] = {true, 2, -3.0f};
    pending.voiceSends[1] = {true, 3, -6.0f};

    harken::ParkRecord& parked = engine->parkRecords[0];
    parked.active = true;
    parked.facts.busIndex = 2;
    parked.voiceSends[0] = {true, 2, -9.0f};
    parked.voiceSends[1] = {true, 3, -12.0f};

    ASSERT_EQ(VN_OK, Vn_DestroyBus(kBusA));

    EXPECT_EQ(0u, pending.busIndex);
    EXPECT_FALSE(pending.voiceSends[0].active);
    EXPECT_EQ(0u, pending.voiceSends[0].targetBusIndex);
    EXPECT_FLOAT_EQ(0.0f, pending.voiceSends[0].levelDb);
    EXPECT_TRUE(pending.voiceSends[1].active);
    EXPECT_EQ(3u, pending.voiceSends[1].targetBusIndex);
    EXPECT_FLOAT_EQ(-6.0f, pending.voiceSends[1].levelDb);

    EXPECT_EQ(0u, parked.facts.busIndex);
    EXPECT_FALSE(parked.voiceSends[0].active);
    EXPECT_EQ(0u, parked.voiceSends[0].targetBusIndex);
    EXPECT_FLOAT_EQ(0.0f, parked.voiceSends[0].levelDb);
    EXPECT_TRUE(parked.voiceSends[1].active);
    EXPECT_EQ(3u, parked.voiceSends[1].targetBusIndex);
    EXPECT_FLOAT_EQ(-12.0f, parked.voiceSends[1].levelDb);

    pending.active = false;
    parked.active = false;
}

TEST(BusDestroy, PublishedTimelineIdentityMigratesWithFactsAndDoesNotHoldAfterAudioAck) {
    auto config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 2;
    config.topologyPlanPoolCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    std::vector<float> samples = harkentest::MakeDC(kBlockFrames, 0.25f);
    const VnPCMDesc desc = MonoDesc(samples, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(902), &desc));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(902));
    play.busId = kBusA;
    play.loopCount = 0;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    (void)PumpBlock(engine);
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto doomed = engine->buses.FindRef(kBusA);
    ASSERT_TRUE(doomed.Valid());
    const auto slot = static_cast<std::uint32_t>(handle);
    ASSERT_LT(slot, engine->timelineVoiceInstallIdentities.size());
    auto& identity = engine->timelineVoiceInstallIdentities[slot];
    identity.renderHandle = handle;
    identity.busIndex = doomed.index;
    identity.busGeneration = doomed.generation;
    identity.voiceSends[0] = {true, doomed.index, -3.0f, doomed.generation};
    ASSERT_NE(nullptr, engine->voices.Resolve(identity.renderHandle));
    ASSERT_EQ(VN_OK, Vn_DestroyBus(kBusA));
    EXPECT_EQ(0u, identity.busIndex);
    EXPECT_EQ(engine->buses.GenerationAt(0), identity.busGeneration);
    EXPECT_FALSE(identity.voiceSends[0].active);
    EXPECT_NE(0u, engine->buses.RetirementRevisionAt(doomed.index));
    EXPECT_EQ(0u, harken::DrainTopologyRetirementsV1(*engine));
    (void)PumpBlock(engine);
    EXPECT_EQ(1u, harken::DrainTopologyRetirementsV1(*engine));
    EXPECT_EQ(0u, engine->buses.RetirementRevisionAt(doomed.index));
    identity = {};
}

TEST(BusDestroy, StalePublishedIdentityPinsHoldUntilMigratedOffDeadSlot) {
    auto config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 2;
    config.topologyPlanPoolCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    std::vector<float> samples = harkentest::MakeDC(kBlockFrames, 0.25f);
    const VnPCMDesc desc = MonoDesc(samples, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(903), &desc));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(903));
    play.busId = kBusA;
    play.loopCount = 0;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    (void)PumpBlock(engine);
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto doomed = engine->buses.FindRef(kBusA);
    ASSERT_TRUE(doomed.Valid());
    ASSERT_EQ(VN_OK, Vn_DestroyBus(kBusA));
    (void)PumpBlock(engine);
    const auto slot = static_cast<std::uint32_t>(handle);
    ASSERT_LT(slot, engine->timelineVoiceInstallIdentities.size());
    auto& identity = engine->timelineVoiceInstallIdentities[slot];
    identity.renderHandle = handle;
    identity.busIndex = doomed.index;
    identity.busGeneration = doomed.generation;
    ASSERT_NE(nullptr, engine->voices.Resolve(identity.renderHandle));
    EXPECT_EQ(0u, harken::DrainTopologyRetirementsV1(*engine));
    EXPECT_NE(0u, engine->buses.RetirementRevisionAt(doomed.index));
    identity.busIndex = 0;
    identity.busGeneration = engine->buses.GenerationAt(0);
    EXPECT_EQ(1u, harken::DrainTopologyRetirementsV1(*engine));
    EXPECT_EQ(0u, engine->buses.RetirementRevisionAt(doomed.index));
    identity = {};
}

TEST(BusDestroy, InstalledEffectInstancesRetireToTheirPools) {
    EngineSession session(MakeNullConfig(2, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));

    VnGainDesc gain{};
    gain.structSize = sizeof(VnGainDesc);
    gain.gainDb = -6.0f;
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(kBusA, 0, VN_EFFECT_GAIN,
                                     reinterpret_cast<const VnEffectDesc*>(
                                         &gain)));
    Capture warm = PumpBlock(session.Engine());
    ASSERT_EQ(VN_OK, Vn_Update());

    const auto gainRowActive = [](const VnSnapshot& s) -> std::uint32_t {
        for (std::uint32_t i = 0; i < s.effectTypeStatCount; ++i) {
            if (s.effectTypeStats[i].effectType == VN_EFFECT_GAIN) {
                return s.effectTypeStats[i].activeCount;
            }
        }
        return 0xFFFFFFFFu;
    };
    EXPECT_EQ(1u, gainRowActive(Snap()));

    // The destroy swap retires the installed instance; the next Vn_Update
    // drains the retire queue back into the pool.
    ASSERT_EQ(VN_OK, Vn_DestroyBus(kBusA));
    warm = PumpBlock(session.Engine());
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(0u, gainRowActive(Snap()));
}

TEST(BusDestroy, ReclaimsBusEffectBindingAndParameterStoreState) {
    VnConfig config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 4;
    config.fxSlotsPerBus = 1;
    config.gainEffectPoolCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    InstallEffectVariable(session.Engine());
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, VN_BUS_MASTER));

    const VnGainDesc gain = GainDesc(-6.0f);
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(
                         kBusA, 0, VN_EFFECT_GAIN,
                         reinterpret_cast<const VnEffectDesc*>(&gain)));
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(
                         kBusB, 0, VN_EFFECT_GAIN,
                         reinterpret_cast<const VnEffectDesc*>(&gain)));
    Capture warm = PumpBlock(session.Engine());
    EXPECT_EQ(2u, session.Engine()->effectParamTargets.LiveRecordCount());

    const VnBindingDesc binding = BusGainBindingDesc();
    const VnBindingAttachDesc attachA = BusEffectAttachment(kBusA);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(kEffectBinding, &binding));
    ASSERT_EQ(VN_OK, Vn_AttachBinding(kEffectBinding, &attachA));
    ASSERT_EQ(VN_OK, Vn_SetValue(kEffectVariable, 1.0f));
    harkentest::AdvanceVariableWrite(session.Engine());

    // A bound direct set only retargets the base. The composed opcode-22
    // command is emitted by the next fold, never by the setter itself.
    harken::CommandRecord command{};
    EXPECT_FALSE(session.Engine()->commands.TryPop(command));
    ASSERT_EQ(VN_OK, Vn_SetBusEffectParam(
                         kBusA, 0, harken::kGainParamGainDb, -3.0f));
    EXPECT_FALSE(session.Engine()->commands.TryPop(command));
    ASSERT_EQ(VN_OK, Vn_Update());
    warm = PumpBlock(session.Engine());

    ASSERT_EQ(VN_OK, Vn_DestroyBus(kBusA));
    warm = PumpBlock(session.Engine());

    // The scope-3 attachment and every parameter row keyed by the dead dense
    // index disappear in the same commit.
    EXPECT_FALSE(session.Engine()->bindings.HasAttachment(
        kEffectBinding, VN_TARGET_SCOPE_BUS_EFFECT_PARAM, kBusA));
    EXPECT_EQ(1u, session.Engine()->effectParamTargets.LiveRecordCount());
    EXPECT_FLOAT_EQ(
        0.0f,
        session.Engine()->effectParamTargets.LastCommitted(
            harken::EffectParamTargetStore::Scope::Bus, 1, 0,
            harken::kGainParamGainDb));

    // A later update cannot emit an effect-parameter command to the dead
    // index, even when the former binding input changes.
    ASSERT_EQ(VN_OK, Vn_SetValue(kEffectVariable, 0.0f));
    harkentest::AdvanceVariableWrite(session.Engine());
    EXPECT_FALSE(session.Engine()->commands.TryPop(command));
    EXPECT_EQ(VN_OK, Vn_UnregisterBinding(kEffectBinding));

    // The sibling bus remains unbound: its direct setter still emits exactly
    // one version-2 bus-effect command without waiting for Vn_Update.
    ASSERT_EQ(VN_OK, Vn_SetBusEffectParam(
                         kBusB, 0, harken::kGainParamGainDb, 0.0f));
    ASSERT_TRUE(session.Engine()->commands.TryPop(command));
    EXPECT_EQ(harken::kOpcodeSetBusEffectParam, command.opcode);
    EXPECT_EQ(harken::kCommandVersion, command.version);
    EXPECT_EQ(2u, command.payload.setBusEffectParam.busIndex);
    EXPECT_FALSE(session.Engine()->commands.TryPop(command));

    // Recreating the public id appends a fresh dense index. The reclaimed
    // store capacity seeds its effect, and the same binding triple can attach
    // again after re-registration.
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    warm = PumpBlock(session.Engine());
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(
                         kBusA, 0, VN_EFFECT_GAIN,
                         reinterpret_cast<const VnEffectDesc*>(&gain)));
    warm = PumpBlock(session.Engine());
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(kEffectBinding, &binding));
    ASSERT_EQ(VN_OK, Vn_AttachBinding(kEffectBinding, &attachA));
    EXPECT_EQ(2u, session.Engine()->effectParamTargets.LiveRecordCount());
}

TEST(BusDestroy, CascadeReclaimsChildBusEffectBindingAndParameterStore) {
    VnConfig config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 4;
    config.fxSlotsPerBus = 1;
    config.gainEffectPoolCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    InstallEffectVariable(session.Engine());
    // master <- A (parent) <- B (child). Every scope-3 artifact lives on the
    // CHILD, which is never the direct destroy target: the cascade must reclaim
    // it purely by virtue of the parent's death tombstoning the whole subtree.
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, kBusA));

    const VnGainDesc gain = GainDesc(-6.0f);
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(
                         kBusB, 0, VN_EFFECT_GAIN,
                         reinterpret_cast<const VnEffectDesc*>(&gain)));
    Capture warm = PumpBlock(session.Engine());
    // The child's installed effect owns the single live store row.
    EXPECT_EQ(1u, session.Engine()->effectParamTargets.LiveRecordCount());

    const VnBindingDesc binding = BusGainBindingDesc();
    const VnBindingAttachDesc attachB = BusEffectAttachment(kBusB);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(kEffectBinding, &binding));
    ASSERT_EQ(VN_OK, Vn_AttachBinding(kEffectBinding, &attachB));
    ASSERT_EQ(VN_OK, Vn_SetValue(kEffectVariable, 1.0f));
    // Fold once so the child's dense index (2) carries a committed composed row.
    ASSERT_EQ(VN_OK, Vn_Update());
    warm = PumpBlock(session.Engine());

    // Destroying the PARENT cascades to the child in the same swap.
    ASSERT_EQ(VN_OK, Vn_DestroyBus(kBusA));
    warm = PumpBlock(session.Engine());

    // The child's scope-3 attachment and its parameter store row disappear in
    // the cascade commit, even though the destroy call named only the parent.
    EXPECT_FALSE(session.Engine()->bindings.HasAttachment(
        kEffectBinding, VN_TARGET_SCOPE_BUS_EFFECT_PARAM, kBusB));
    EXPECT_EQ(0u, session.Engine()->effectParamTargets.LiveRecordCount());
    EXPECT_FLOAT_EQ(
        0.0f,
        session.Engine()->effectParamTargets.LastCommitted(
            harken::EffectParamTargetStore::Scope::Bus, 2, 0,
            harken::kGainParamGainDb));

    // The child's dense index is now a tombstone: driving the former binding
    // input and folding again must emit no command at all — in particular no
    // effect-parameter opcode aimed at the dead index.
    harken::CommandRecord command{};
    EXPECT_FALSE(session.Engine()->commands.TryPop(command));
    ASSERT_EQ(VN_OK, Vn_SetValue(kEffectVariable, 0.0f));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_FALSE(session.Engine()->commands.TryPop(command));
    EXPECT_EQ(VN_OK, Vn_UnregisterBinding(kEffectBinding));
}

TEST(BusDestroy, MeterProbeRetiresWithItsSourceBus) {
    EngineSession session(MakeNullConfig(2, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, VN_BUS_MASTER));

    harken::Engine* engine = session.Engine();
    constexpr VnObjectId kProbe = harkentest::ObjectId(500);
    std::uint32_t generationOnA = 0;
    ASSERT_EQ(VN_OK, harken::InstallMeterProbe(*engine, kProbe, kBusA, 0, 0, 0,
                                              &generationOnA));
    EXPECT_TRUE(engine->meterDetectors.IsProbe(kProbe));
    EXPECT_EQ(1u, engine->meterDetectors.ActiveCount());

    // Destroying the source bus starts withdrawal in the same commit. The
    // identity cannot be reused until publication, command settlement, and
    // its audio fence have all passed.
    ASSERT_EQ(VN_OK, Vn_DestroyBus(kBusA));
    EXPECT_TRUE(engine->meterDetectors.IsProbe(kProbe));
    EXPECT_EQ(0u, engine->meterDetectors.ActiveCount());
    EXPECT_EQ(VN_IN_USE,
              harken::InstallMeterProbe(*engine, kProbe, kBusB, 0, 0, 0));
    for (int i = 0; i < 3; ++i) {
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    EXPECT_FALSE(engine->meterDetectors.IsProbe(kProbe));

    std::uint32_t generationOnB = 0;
    EXPECT_EQ(VN_OK, harken::InstallMeterProbe(*engine, kProbe, kBusB, 0, 0, 0,
                                              &generationOnB));
    EXPECT_TRUE(engine->meterDetectors.IsProbe(kProbe));
    EXPECT_NE(generationOnA, generationOnB);
    EXPECT_EQ(VN_STALE_HANDLE,
              harken::UninstallMeterProbe(*engine, kProbe, generationOnA));
    EXPECT_EQ(VN_OK,
              harken::UninstallMeterProbe(*engine, kProbe, generationOnB));
}

TEST(BusDestroy, CascadeRetirementWaitsForAcknowledgementAndMeterWithdrawal) {
    auto config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 3;
    config.topologyPlanPoolCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, kBusA));
    std::vector<float> samples;
    RegisterDcVoiceOnBus(harkentest::ObjectId(901), 0.25f, kBusB, samples);
    (void)PumpBlock(engine);
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto parent = engine->buses.FindRef(kBusA);
    const auto child = engine->buses.FindRef(kBusB);
    ASSERT_TRUE(parent.Valid());
    ASSERT_TRUE(child.Valid());
    ASSERT_EQ(VN_OK, harken::InstallMeterProbe(*engine, harkentest::ObjectId(8101), kBusB, VN_BUS_METER_TAP_POST_EFFECTS, 0, 0));
    auto* probe = engine->meterDetectors.Find(harkentest::ObjectId(8101));
    ASSERT_NE(nullptr, probe);
    struct CommitBoundary { harken::Engine* engine; std::uint32_t slot; } boundary{engine, parent.index};
    allocation_probe::count = 0;
    allocation_probe::countContext = &boundary;
    allocation_probe::countIf = [](void* context) noexcept {
        const auto& state = *static_cast<CommitBoundary*>(context);
        return !state.engine->buses.AliveAt(state.slot);
    };
    allocation_probe::enabled = true;
    void* prepareProof = ::operator new(16);
    ::operator delete(prepareProof);
    const auto beforeCommit = allocation_probe::count;
    const auto result = Vn_DestroyBus(kBusA);
    const auto commitAllocations = allocation_probe::count;
    void* commitProof = ::operator new(16);
    ::operator delete(commitProof);
    allocation_probe::enabled = false;
    allocation_probe::countIf = nullptr;
    allocation_probe::countContext = nullptr;
    EXPECT_EQ(0u, beforeCommit);
    EXPECT_EQ(0u, commitAllocations);
    EXPECT_EQ(1u, allocation_probe::count);
    ASSERT_EQ(VN_OK, result);
    EXPECT_NE(0u, engine->buses.RetirementRevisionAt(parent.index));
    EXPECT_NE(0u, engine->buses.RetirementRevisionAt(child.index));
    EXPECT_EQ(harken::MeterSystem::State::Withdrawing, probe->state);
    EXPECT_EQ(VN_NO_BUS, Vn_CreateBus(kBusC, VN_BUS_MASTER));
    EXPECT_EQ(0u, harken::DrainTopologyRetirementsV1(*engine));
    const auto audio = PumpBlock(engine);
    EXPECT_NEAR(0.25f, audio.left.back(), 1e-5f);
    EXPECT_EQ(1u, harken::DrainTopologyRetirementsV1(*engine));
    EXPECT_EQ(0u, engine->buses.RetirementRevisionAt(parent.index));
    EXPECT_NE(0u, engine->buses.RetirementRevisionAt(child.index));
    for (std::uint32_t i = 0; i < 4; ++i) {
        ASSERT_EQ(VN_OK, Vn_Update());
        (void)PumpBlock(engine);
    }
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(harken::MeterSystem::State::Retired, probe->state);
    EXPECT_EQ(0u, engine->buses.RetirementRevisionAt(child.index));
    EXPECT_EQ(VN_OK, Vn_CreateBus(kBusC, VN_BUS_MASTER));
}

TEST(BusDestroy, ExhaustedRevisionsOrMeterSequencesPublishNothing) {
    for (const bool meterExhausted : {false, true}) {
        SCOPED_TRACE(meterExhausted);
        EngineSession session(MakeNullConfig(2, kBlockFrames));
        ASSERT_EQ(VN_OK, session.InitResult());
        auto* engine = session.Engine();
        ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
        (void)PumpBlock(engine);
        ASSERT_EQ(VN_OK, Vn_Update());
        ASSERT_EQ(VN_OK, harken::InstallMeterProbe(*engine, harkentest::ObjectId(8101), kBusA, VN_BUS_METER_TAP_POST_EFFECTS, 0, 0));
        ASSERT_EQ(VN_OK, harken::InstallMeterProbe(*engine, harkentest::ObjectId(8102), kBusA, VN_BUS_METER_TAP_POST_EFFECTS, 0, 0));
        const auto ref = engine->buses.FindRef(kBusA);
        if (meterExhausted) engine->logicCommits.SetCurrentForTests(UINT64_MAX - 1);
        else engine->acceptedBusTopologyRevision = UINT64_MAX;
        const auto revision = engine->acceptedBusTopologyRevision;
        const auto head = engine->commands.PublishedHead();
        const auto plans = engine->topologyPlans.InUse();
        EXPECT_EQ(meterExhausted ? VN_BACKPRESSURE : VN_DEVICE_ERROR, Vn_DestroyBus(kBusA));
        EXPECT_TRUE(engine->buses.LogicRefMatches(ref));
        EXPECT_EQ(0u, engine->buses.RetirementRevisionAt(ref.index));
        EXPECT_EQ(revision, engine->acceptedBusTopologyRevision);
        EXPECT_EQ(head, engine->commands.PublishedHead());
        EXPECT_EQ(plans, engine->topologyPlans.InUse());
        for (const auto id : {harkentest::ObjectId(8101u), harkentest::ObjectId(8102u)}) {
            ASSERT_NE(nullptr, engine->meterDetectors.Find(id));
            EXPECT_EQ(harken::MeterSystem::State::Installed, engine->meterDetectors.Find(id)->state);
            EXPECT_EQ(0u, engine->meterDetectors.Find(id)->withdrawalLogicSequence);
        }
    }
}

TEST(BusDestroy, SoloMemberDestroyRestoresTheAudibleSet) {
    EngineSession session(MakeNullConfig(4, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusC, VN_BUS_MASTER));

    std::vector<float> dc;
    RegisterDcVoiceOnBus(harkentest::ObjectId(1), 0.25f, kBusC, dc);

    // Solo A: the off-path C silences.
    ASSERT_EQ(VN_OK, Vn_SetBusSolo(kBusA, 1, 0));
    Capture out = PumpBlock(session.Engine());
    ASSERT_EQ(0.0f, out.left[kBlockFrames - 1]);

    harken::Engine* engine = session.Engine();
    const std::uint32_t busC = engine->buses.FindIndex(kBusC);
    ASSERT_NE(harken::kInvalidBusIndex, busC);
    const harken::VariableSliceRef owner{31u, 9u};
    ASSERT_TRUE(engine->busTargets.TryClaim(
        busC, harken::kBusTargetBusVolume, owner,
        engine->busTargets.TargetRevision(
            busC, harken::kBusTargetBusVolume)));
    const std::uint64_t commandHead = engine->commands.PublishedHead();
    EXPECT_EQ(VN_BACKPRESSURE, Vn_DestroyBus(kBusA));
    EXPECT_NE(harken::kInvalidBusIndex,
              engine->buses.FindIndex(kBusA));
    EXPECT_EQ(1u, engine->busSoloActiveCount);
    EXPECT_EQ(commandHead, engine->commands.PublishedHead());
    EXPECT_TRUE(engine->busTargets.ForeignClaim(
        busC, harken::kBusTargetBusVolume));
    engine->busTargets.ReleaseClaim(
        busC, harken::kBusTargetBusVolume, owner);

    // Destroying the soloed A clears the last solo: C returns audible in
    // the same call, and the snapshot masks agree.
    const auto beforeDestroyHead = engine->commands.PublishedHead();
    ASSERT_EQ(VN_OK, Vn_DestroyBus(kBusA));
    EXPECT_EQ(beforeDestroyHead + 2, engine->commands.PublishedHead());
    out = PumpBlock(session.Engine());
    ASSERT_NEAR(0.25f, out.left[kBlockFrames - 1], 1e-5f);
    const VnSnapshot s = Snap();
    // Dense indices: master 0, A 1 (dead), C 2. Audible = master + C.
    EXPECT_EQ(0x5u, s.busSoloEffectiveMask);
}

TEST(BusDestroy, DirectDestroyClearsCallerBusVolumeBinding) {
    EngineSession session(MakeNullConfig(2, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    constexpr VnVariableId kVolumeVariable = harkentest::ObjectId(720);
    constexpr VnBindingId kVolumeBinding = harkentest::ObjectId(721);
    const harken::VariableDefinitionV1 definition =
        harkentest::MakeVariableDefinition(kVolumeVariable,
                                         harken::VariableKindV1::Smooth,
                                         0.0f, 1.0f, 0.0f);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallDefinitionsWorld(
                  session.Engine(), &definition, 1).status);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    VnBindingDesc binding{};
    binding.structSize = sizeof(VnBindingDesc);
    binding.variableId = kVolumeVariable;
    binding.targetScopeKind = VN_TARGET_SCOPE_BUS;
    binding.targetId = harken::kBusTargetBusVolume;
    binding.curve.structSize = sizeof(VnCurveDesc);
    binding.curve.pointCount = 2;
    binding.curve.points[0] = {0.0f, 0.0f};
    binding.curve.points[1] = {1.0f, -6.0f};
    binding.curve.handles[0] = harkentest::EndpointHandles(
        binding.curve.points[0], binding.curve.points[1]);
    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(VnBindingAttachDesc);
    attach.scopeKind = VN_TARGET_SCOPE_BUS;
    attach.objectId = kBusA;
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(kVolumeBinding, &binding));
    ASSERT_EQ(VN_OK, Vn_AttachBinding(kVolumeBinding, &attach));
    ASSERT_TRUE(session.Engine()->bindings.HasAttachment(
        kVolumeBinding, VN_TARGET_SCOPE_BUS, kBusA));
    ASSERT_EQ(VN_OK, Vn_SetValue(kVolumeVariable, 1.0f));
    harkentest::AdvanceVariableWrite(session.Engine());
    Capture warm = PumpBlock(session.Engine());
    ASSERT_EQ(VN_OK, Vn_DestroyBus(kBusA));
    EXPECT_FALSE(session.Engine()->bindings.HasAttachment(
        kVolumeBinding, VN_TARGET_SCOPE_BUS, kBusA));
    EXPECT_EQ(VN_OK, Vn_UnregisterBinding(kVolumeBinding));
    (void)warm;
}

TEST(BusDestroy, RefusalMatrixAndBackpressureAtomicity) {
    VnConfig config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 4;
    config.fxSlotsPerBus = 1;
    config.gainEffectPoolCapacity = 2;
    config.commandQueueDepth = 2; // rounds to 2; easy to fill
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    InstallEffectVariable(session.Engine());
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    const VnGainDesc gain = GainDesc(-6.0f);
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(
                         kBusA, 0, VN_EFFECT_GAIN,
                         reinterpret_cast<const VnEffectDesc*>(&gain)));
    const VnBindingDesc binding = BusGainBindingDesc();
    const VnBindingAttachDesc attachA = BusEffectAttachment(kBusA);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(kEffectBinding, &binding));
    ASSERT_EQ(VN_OK, Vn_AttachBinding(kEffectBinding, &attachA));
    Capture warm;
    PumpAndCapture(session.Engine(), 1, warm);
    Vn_Update();

    EXPECT_EQ(VN_INVALID_ARG, Vn_DestroyBus(VN_BUS_MASTER));
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_DestroyBus(harkentest::ObjectId(999)));

    // Fill the command queue (no pump between): the destroy refuses whole
    // and the bus stays fully alive.
    VnResult r = VN_OK;
    int pushed = 0;
    while ((r = Vn_SetBusVolume(kBusA, -1.0f, 0)) == VN_OK && pushed < 64) {
        ++pushed;
    }
    ASSERT_EQ(VN_BACKPRESSURE, r);
    EXPECT_EQ(VN_BACKPRESSURE, Vn_DestroyBus(kBusA));
    EXPECT_TRUE(session.Engine()->bindings.HasAttachment(
        kEffectBinding, VN_TARGET_SCOPE_BUS_EFFECT_PARAM, kBusA));
    EXPECT_EQ(1u, session.Engine()->effectParamTargets.LiveRecordCount());
    PumpAndCapture(session.Engine(), 1, warm);
    Vn_Update();
    EXPECT_EQ(VN_OK, Vn_SetBusVolume(kBusA, 0.0f, 0)); // still alive
    EXPECT_EQ(VN_OK, Vn_DestroyBus(kBusA)); // and still destroyable
}

} // namespace
