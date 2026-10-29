#include "Common/ObjectIdTestSupport.h"
#include "Common/RawVoiceTestSupport.h"
#include "Common/TestUtil.h"

#include "Engine.h"
#include "PlayableRuntime.h"
#include "ProjectGenerationTransaction.h"
#include "TraceFormat.h"
#include "TraceRecorder.h"
#include "harken/core/ChannelLayout.h"
#include "harken/core/CommandQueue.h"
#include "harken/core/CurrentWorld.h"
#include "harken/core/LogicAllocCheckpoint.h"
#include "harken/dsp/GainRamp.h"
#include "harken_preview/PreviewAbi.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <utility>
#include <vector>

namespace {

constexpr VnObjectId kSound = harkentest::ObjectId(9101);
constexpr VnObjectId kContainer = harkentest::ObjectId(9102);
constexpr VnObjectId kAction = harkentest::ObjectId(91);

harken::PlayablePropertyPayloadViewV1 CanonicalPayload() {
    harken::PlayablePropertyPayloadViewV1 payload{};
    payload.values[3].bits = 1;
    payload.values[5].bits = std::bit_cast<std::uint32_t>(0.0f);
    payload.values[8].bits = std::bit_cast<std::uint32_t>(0.0f);
    payload.values[9].bits = std::bit_cast<std::uint32_t>(0.0f);
    payload.values[11].bits = std::bit_cast<std::uint32_t>(1.0f);
    payload.values[16].bits = std::bit_cast<std::uint32_t>(1.0f);
    return payload;
}

harken::PlayablePropertyPayloadViewV1 ResolvedPayload() {
    auto payload = CanonicalPayload();
    payload.presentMask = harken::kResolvedSectionGroupPropertyMaskV1;
    payload.values[6].objectId = VN_BUS_MASTER;
    payload.values[7].bits = 0;
    payload.values[10].bits = 0;
    return payload;
}

void SetAbsoluteTime(harken::AuthoredTimeRationalV1* time,
                     std::int64_t nanoseconds = 0) {
    time->domain = harken::AuthoredTimeDomainV1::AbsoluteNanoseconds;
    time->numerator = nanoseconds;
    time->denominator = 1;
}

struct AdmissionWorldImage {
    std::array<harken::PlayablePropertyProgramViewV1, 2> properties{};
    harken::PlayableOccurrencePropertyViewV1 occurrence{};
    harken::ContainerChildProgramViewV2 child{};
    harken::ContainerProgramViewV3 container{};
    harken::ActionStepProgramViewV1 playStep{};
    harken::ActionProgramViewV1 action{};
    harken::ContainerWorldInstallViewV1 world{};

    explicit AdmissionWorldImage(std::uint64_t soundRevision) {
        properties[0].contentPropertyKind =
            harken::playableproperties::ContentPropertyKindV1::Sound;
        properties[0].stableId = harkentest::ObjectId(kSound);
        properties[0].authoredStableOrder = 0;
        properties[0].contentRevision = soundRevision;
        properties[0].routeStableId = VN_BUS_MASTER;
        properties[0].compatibilityFingerprint = 1;
        properties[0].payload = ResolvedPayload();
        properties[0].programSemanticFingerprint =
            harken::ComputePlayablePropertyFingerprintV1(properties[0]);

        properties[1].contentPropertyKind =
            harken::playableproperties::ContentPropertyKindV1::Container;
        properties[1].stableId = harkentest::ObjectId(kContainer);
        properties[1].authoredStableOrder = 1;
        properties[1].contentRevision = 1;
        properties[1].routeStableId = VN_BUS_MASTER;
        properties[1].compatibilityFingerprint = 2;
        properties[1].payload = ResolvedPayload();
        properties[1].programSemanticFingerprint =
            harken::ComputePlayablePropertyFingerprintV1(properties[1]);

        child.stableChildOrdinal = 7;
        child.authoredStableOrder = 0;
        child.playable.kind = harken::PlayableKindV1::Sound;
        child.playable.stableId = harkentest::ObjectId(kSound);
        child.playable.expectedProgramSemanticFingerprint =
            properties[0].programSemanticFingerprint;
        child.playable.contentRevision = soundRevision;
        child.playable.routeStableId = VN_BUS_MASTER;
        child.playable.compatibilityFingerprint = 1;
        child.frequencyWeight = 1;

        occurrence.parentKind =
            harken::playableproperties::ContentPropertyKindV1::Container;
        occurrence.parentStableId = harkentest::ObjectId(kContainer);
        occurrence.parentProgramSemanticFingerprint =
            properties[1].programSemanticFingerprint;
        occurrence.stableOccurrenceOrdinal = child.stableChildOrdinal;
        occurrence.childPlayable = child.playable;
        occurrence.payload = CanonicalPayload();
        occurrence.occurrenceSemanticFingerprint =
            harken::ComputePlayableOccurrenceFingerprintV1(occurrence);

        container.containerStableId = harkentest::ObjectId(kContainer);
        container.programSemanticFingerprint =
            properties[1].programSemanticFingerprint;
        container.mode = harken::ContainerModeV1::Random;
        container.randomDiscipline = harken::RandomDisciplineV1::Draw;
        container.memoryScope = harken::MemoryScopeV1::Global;
        container.childCount = 1;
        container.children = &child;

        playStep.opcode = harken::ActionOpcodeV1::Play;
        playStep.target.kind = harken::ActionTargetKindV1::Sound;
        playStep.target.stableId = properties[0].stableId;
        playStep.target.expectedProgramSemanticFingerprint =
            properties[0].programSemanticFingerprint;
        SetAbsoluteTime(&playStep.delay);
        SetAbsoluteTime(&playStep.fade);
        SetAbsoluteTime(&playStep.cooldown);

        action.actionStableId = harkentest::ObjectId(kAction);
        action.steps = &playStep;
        action.stepCount = 1;
        action.expandedStepOccurrenceCount = 1;
        action.expandedFiringCount = 1;
        action.programSemanticFingerprint =
            harken::ComputeActionProgramFingerprintV1(action);

        world.playableProperties = properties.data();
        world.playablePropertyCount = properties.size();
        world.playableOccurrences = &occurrence;
        world.playableOccurrenceCount = 1;
        world.containers = &container;
        world.containerCount = 1;
        world.actions = &action;
        world.actionCount = 1;
        world.capacity.maxActionDefinitions = 4;
        world.capacity.maxActionStepsPerDefinition = 2;
        world.capacity.maxActionStepsTotal = 4;
        world.identity.capabilityMask =
            harken::kWorldCapabilityPlayableProperties |
            harken::kWorldCapabilityRandomDraw |
            harken::kWorldCapabilityActionDefinitions;
        BindWorld();
        world.identity.runtimeSemanticFingerprint =
            harken::ComputeCurrentWorldFingerprintV5(world);
    }

    AdmissionWorldImage(const AdmissionWorldImage& other) { *this = other; }
    AdmissionWorldImage& operator=(const AdmissionWorldImage& other) {
        if (this == &other) {
            return *this;
        }
        properties = other.properties;
        occurrence = other.occurrence;
        child = other.child;
        container = other.container;
        playStep = other.playStep;
        action = other.action;
        world = other.world;
        BindWorld();
        return *this;
    }

    void BindWorld() {
        child.playable.expectedProgramSemanticFingerprint =
            properties[0].programSemanticFingerprint;
        occurrence.parentProgramSemanticFingerprint =
            properties[1].programSemanticFingerprint;
        occurrence.childPlayable = child.playable;
        container.programSemanticFingerprint =
            properties[1].programSemanticFingerprint;
        container.children = &child;
        playStep.target.expectedProgramSemanticFingerprint =
            properties[0].programSemanticFingerprint;
        action.steps = &playStep;
        world.playableProperties = properties.data();
        world.playableOccurrences = &occurrence;
        world.containers = &container;
        world.actions = &action;
    }

    harken::PlayableRefV1 Root() const {
        harken::PlayableRefV1 root{};
        root.kind = harken::PlayableKindV1::Container;
        root.stableId = harkentest::ObjectId(kContainer);
        root.expectedProgramSemanticFingerprint =
            properties[1].programSemanticFingerprint;
        root.contentRevision = properties[1].contentRevision;
        root.routeStableId = properties[1].routeStableId;
        root.compatibilityFingerprint = properties[1].compatibilityFingerprint;
        return root;
    }
};

harken::BusAdmissionClaimKeyV1 StudioKey() {
    harken::BusAdmissionClaimKeyV1 key{};
    key.tag = harken::kBusAdmissionTagStudioCurrentWorld;
    return key;
}

VnConfig AdmissionConfig(
    std::uint32_t mixLayout = VN_CHANNEL_LAYOUT_DEFAULT,
    std::uint32_t deviceLayout = VN_CHANNEL_LAYOUT_DEFAULT) {
    auto config = harkentest::MakeNullConfigLayout(4, 64, 48000, mixLayout, deviceLayout);
    // Bus admission coverage does not require the default nested Action scratch.
    config.maxTriggerDepth = 2;
    return config;
}

struct AdmissionSession {
    VnConfig config;
    harkentest::EngineSession session;
    std::vector<float> samples = std::vector<float>(4096, 0.25f);
    AdmissionWorldImage image;

    explicit AdmissionSession(
        std::uint32_t mixLayout = VN_CHANNEL_LAYOUT_DEFAULT,
        std::uint32_t deviceLayout = VN_CHANNEL_LAYOUT_DEFAULT)
        : config(AdmissionConfig(mixLayout, deviceLayout)),
          session(config),
          image(0) {
        EXPECT_EQ(session.InitResult(), VN_OK);
        const VnPCMDesc pcm = harkentest::MonoDesc(samples, 48000);
        EXPECT_EQ(Vn_RegisterPCM(kSound, &pcm), VN_OK);
        harken::Engine* engine = session.Engine();
        EXPECT_NE(engine, nullptr);
        if (engine == nullptr) {
            return;
        }
        image = AdmissionWorldImage(engine->registry.Lookup(kSound).contentRevision);
        EXPECT_EQ(harken::InstallContainerWorldV1(*engine, image.world).status,
                  harken::WorldInstallStatus::Ok);
    }
};

harken::ProjectGenerationInstallViewV1 MakeInstallView(
    const AdmissionWorldImage& image) {
    harken::ProjectGenerationInstallViewV1 view{};
    view.token.projectSessionId = 11;
    view.token.authoringRevision = 2;
    view.token.runtimeEpoch = 3;
    view.token.submissionSerial = 4;
    view.ownerId = 77;
    view.ownerRevision = 1;
    view.compiledWorld = image.world;
    return view;
}

TEST(BusAdmissionStart, PreparedPoolRecyclesExactlyOnceAfterRetirement) {
    harken::TopologyPlanPool pool(2, 4, 2);
    static_assert(noexcept(pool.Recycle(0)));
    static_assert(noexcept(std::declval<harken::ParamSystem&>().SetInstalledValue(harkentest::ObjectId(1), 0.0f)));
    static_assert(noexcept(std::declval<harken::ParamSystem&>().NoteVariableWrite(harkentest::ObjectId(1))));
    static_assert(noexcept(std::declval<harken::EmitterTable&>().SetLocalValue(1, harkentest::ObjectId(1), 0.0f)));
    static_assert(noexcept(std::declval<harken::EmitterTable&>().ClearLocalValues(1)));
    const auto first = pool.Acquire();
    const auto second = pool.Acquire();
    ASSERT_NE(first, second);
    pool.Retire(first);
    EXPECT_TRUE(pool.IsAcquired(first));
    EXPECT_EQ(2u, pool.InUse());
    EXPECT_EQ(harken::kInvalidTopologyPlanIndex, pool.Acquire());
    pool.DrainRetire();
    EXPECT_FALSE(pool.IsAcquired(first));
    EXPECT_EQ(1u, pool.InUse());
    pool.Recycle(first);
    pool.Recycle(UINT32_MAX);
    EXPECT_EQ(1u, pool.InUse());
    EXPECT_EQ(first, pool.Acquire());
    EXPECT_EQ(harken::kInvalidTopologyPlanIndex, pool.Acquire());
    pool.Recycle(first);
    pool.Recycle(second);
    pool.Recycle(second);
    EXPECT_EQ(0u, pool.InUse());
}

TEST(BusAdmissionStart, PreparedLifetimeMoveAndDestructorReleaseExactReservations) {
    AdmissionSession fixture;
    auto& engine = *fixture.session.Engine();
    auto firstView = MakeInstallView(fixture.image);
    auto secondView = firstView;
    secondView.ownerId = 78;
    {
        harken::PreparedProjectGenerationV1 first;
        harken::PreparedProjectGenerationV1 second;
        ASSERT_EQ(harken::ProjectGenerationPrepareStatusV1::Ok,
                  harken::PrepareProjectGenerationV1(engine, firstView, first).status);
        ASSERT_EQ(harken::ProjectGenerationPrepareStatusV1::Ok,
                  harken::PrepareProjectGenerationV1(engine, secondView, second).status);
        EXPECT_EQ(2u, engine.preparedTopologyPlans.InUse());
        const auto retainedIndex = second.PreparedPlanIndex();
        first = std::move(second);
        EXPECT_FALSE(second.Valid());
        EXPECT_TRUE(first.Valid());
        EXPECT_EQ(retainedIndex, first.PreparedPlanIndex());
        EXPECT_EQ(1u, engine.preparedTopologyPlans.InUse());
        harken::PreparedProjectGenerationV1 moved(std::move(first));
        EXPECT_FALSE(first.Valid());
        EXPECT_TRUE(moved.Valid());
    }
    EXPECT_EQ(0u, engine.preparedTopologyPlans.InUse());
    for (const auto& claim : engine.preparedOwnerClaims) EXPECT_EQ(0u, claim.inUse);
}

TEST(BusAdmissionStart, WrongEngineCancelLeavesTokenArmedUntilOwnDomainRelease) {
    AdmissionSession fixture;
    auto& engine = *fixture.session.Engine();
    auto other = std::make_unique<harken::Engine>(engine.config);
    harken::PreparedProjectGenerationV1 prepared;
    ASSERT_EQ(harken::ProjectGenerationPrepareStatusV1::Ok,
              harken::PrepareProjectGenerationV1(engine, MakeInstallView(fixture.image), prepared).status);
    harken::CancelPreparedProjectGenerationV1(*other, std::move(prepared));
    EXPECT_TRUE(prepared.Valid());
    EXPECT_EQ(1u, engine.preparedTopologyPlans.InUse());
    EXPECT_EQ(0u, other->preparedTopologyPlans.InUse());
    harken::CancelPreparedProjectGenerationV1(std::move(prepared));
    EXPECT_FALSE(prepared.Valid());
    EXPECT_EQ(0u, engine.preparedTopologyPlans.InUse());
}

TEST(BusAdmissionStart, PreparedTokenOutlivesTermWithoutReleasingNewSession) {
    harken::PreparedProjectGenerationV1 old;
    std::weak_ptr<harken::ProjectGenerationReservationDomainV1> oldDomain;
    {
        AdmissionSession fixture;
        auto& engine = *fixture.session.Engine();
        oldDomain = engine.projectGenerationReservationDomain;
        ASSERT_EQ(harken::ProjectGenerationPrepareStatusV1::Ok,
                  harken::PrepareProjectGenerationV1(engine, MakeInstallView(fixture.image), old).status);
        EXPECT_TRUE(old.Valid());
    }
    EXPECT_TRUE(oldDomain.expired());
    EXPECT_FALSE(old.Valid());
    AdmissionSession next;
    auto& engine = *next.session.Engine();
    harken::PreparedProjectGenerationV1 current;
    ASSERT_EQ(harken::ProjectGenerationPrepareStatusV1::Ok,
              harken::PrepareProjectGenerationV1(engine, MakeInstallView(next.image), current).status);
    harken::CancelPreparedProjectGenerationV1(std::move(old));
    EXPECT_TRUE(current.Valid());
    EXPECT_EQ(1u, engine.preparedTopologyPlans.InUse());
    old = std::move(current);
    EXPECT_FALSE(current.Valid());
    EXPECT_TRUE(old.Valid());
    harken::CancelPreparedProjectGenerationV1(std::move(old));
    EXPECT_EQ(0u, engine.preparedTopologyPlans.InUse());
}

TEST(BusAdmissionStart, CommandWaistStaysFrozen) {
    EXPECT_EQ(sizeof(harken::CommandRecord), 64u);
    EXPECT_LE(sizeof(harken::CommandRecord::Payload::SetBusTopologyPlan), 56u);
    EXPECT_EQ(harken::kOpcodeSetBusTopologyPlan, 15u);
    EXPECT_EQ(harken::kOpcodeSeekVoice, 26u);
    EXPECT_EQ(harken::kMaxKnownOpcode, harken::kOpcodeSeekVoice);
    EXPECT_EQ(sizeof(harken::PreparedOwnerClaimV1), 56u);
    EXPECT_EQ(sizeof(harken::ProjectGenerationInstallAckIdentityV1), 40u);
    EXPECT_EQ(sizeof(harken::ProjectGenerationInstallAckV1), 40u);
}

TEST(BusAdmissionStart, PrepareFillsPreparedPlanAndCommitBackpressuresOnConsumers) {
    AdmissionSession fixture;
    harken::Engine* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    const std::uint32_t liveBefore = engine->topologyPlans.InUse();
    const std::uint32_t commandFreeBefore = engine->commands.FreeSlots();
    const std::uint32_t instanceFreeBefore = engine->playableInstances.FreeCount();
    const harken::ProjectGenerationInstallViewV1 view =
        MakeInstallView(fixture.image);
    harken::PreparedProjectGenerationV1 prepared;
    const harken::ProjectGenerationPrepareResultV1 preparedResult =
        harken::PrepareProjectGenerationV1(*engine, view, prepared);
    ASSERT_EQ(preparedResult.status,
              harken::ProjectGenerationPrepareStatusV1::Ok);
    EXPECT_EQ(engine->topologyPlans.InUse(), liveBefore);
    EXPECT_EQ(engine->commands.FreeSlots(), commandFreeBefore);
    EXPECT_EQ(engine->playableInstances.FreeCount(), instanceFreeBefore);
    EXPECT_TRUE(prepared.Valid());
    EXPECT_EQ(engine->preparedTopologyPlans.InUse(), 1u);
    EXPECT_NE(engine->preparedTopologyPlans.View(prepared.PreparedPlanIndex())
                  .orderCount,
              0u);

    harken::PlayableStartRequest request{};
    request.root = fixture.image.Root();
    EXPECT_EQ(harken::StartPlayable(*engine, request).status,
              harken::PlayableStartStatus::Accepted);
    const std::uint32_t liveAfterStart = engine->topologyPlans.InUse();
    const std::uint32_t commandFreeAfterStart = engine->commands.FreeSlots();
    const std::uint32_t instanceFreeAfterStart = engine->playableInstances.FreeCount();
    EXPECT_EQ(liveAfterStart, liveBefore);
    EXPECT_LT(instanceFreeAfterStart, instanceFreeBefore);

    const harken::ProjectGenerationCommitResultV1 committed =
        harken::CommitPreparedProjectGenerationV1(*engine, prepared);
    EXPECT_EQ(committed.status,
              harken::ProjectGenerationCommitStatusV1::Backpressure);
    EXPECT_TRUE(prepared.Valid());
    EXPECT_EQ(engine->topologyPlans.InUse(), liveAfterStart);
    EXPECT_EQ(engine->commands.FreeSlots(), commandFreeAfterStart);
    EXPECT_EQ(engine->playableInstances.FreeCount(), instanceFreeAfterStart);
    EXPECT_EQ(engine->currentWorldReplacementAdmissionClosed, false);
    EXPECT_EQ(harken::StartPlayable(*engine, request).status,
              harken::PlayableStartStatus::Accepted);

    harken::CancelPreparedProjectGenerationV1(*engine, std::move(prepared));
    EXPECT_EQ(engine->topologyPlans.InUse(), liveBefore);
    EXPECT_EQ(engine->preparedTopologyPlans.InUse(), 0u);
    EXPECT_EQ(harken::StartPlayable(*engine, request).status,
              harken::PlayableStartStatus::Accepted);
}

TEST(BusAdmissionStart, PublishedMasterHolderBlocksPropertyWritesButNotForeignBus) {
    AdmissionSession fixture;
    auto* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    constexpr VnBusId foreign = harkentest::ObjectId(7001);
    ASSERT_EQ(VN_OK, Vn_CreateBus(foreign, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(VN_BUS_MASTER, -3.0f, 0));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    auto view = MakeInstallView(fixture.image);
    harken::PreparedProjectGenerationV1 prepared;
    ASSERT_EQ(harken::ProjectGenerationPrepareStatusV1::Ok,
        harken::PrepareProjectGenerationV1(*engine, view, prepared).status);
    ASSERT_EQ(harken::ProjectGenerationCommitStatusV1::Ok,
        harken::CommitPreparedProjectGenerationV1(*engine, prepared).status);
    ASSERT_TRUE(harken::MasterHasAuthoredClaimV1(*engine));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());

    VnGainDesc gain{};
    gain.structSize = sizeof(gain);
    gain.gainDb = -6.0f;
    VnDownmixMatrixDesc matrix{};
    matrix.structSize = sizeof(matrix);
    matrix.srcLayout = engine->config.mixChannelLayout;
    matrix.dstLayout = engine->busOutputLayout[0];
    harken::DefaultDownmixMatrix(matrix.srcLayout, matrix.dstLayout, matrix.coeffs);
    VnEffectDynamicRouteDesc route{};
    route.structSize = sizeof(route);
    route.sourceKind = VN_DYNAMIC_SOURCE_BUS;
    route.sourceId = foreign;
    route.signalForm = VN_DYNAMIC_SIGNAL_RAW_AUDIO;
    route.tap = VN_DYNAMIC_TAP_BUS_POST_FADER;
    route.expectedEffectType = VN_EFFECT_COMPRESSOR;
    const auto head = engine->commands.PublishedHead();
    const auto revision = engine->acceptedBusTopologyRevision;
    const auto plans = engine->topologyPlanCount;
    EXPECT_EQ(VN_IN_USE, Vn_SetBusEffect(VN_BUS_MASTER, 0, VN_EFFECT_GAIN,
        reinterpret_cast<const VnEffectDesc*>(&gain)));
    EXPECT_EQ(VN_IN_USE, Vn_ClearBusEffect(VN_BUS_MASTER, 0));
    EXPECT_EQ(VN_IN_USE, Vn_SetBusEffectParam(VN_BUS_MASTER, 0, 0, 0.0f));
    EXPECT_EQ(VN_IN_USE, Vn_SetBusEffectBypass(VN_BUS_MASTER, 0, 1));
    EXPECT_EQ(VN_IN_USE, Vn_ResetBusEffect(VN_BUS_MASTER, 0, VN_EFFECT_COMPRESSOR));
    EXPECT_EQ(VN_IN_USE, Vn_SetBusEffectDynamicRoute(VN_BUS_MASTER, 0, 0, &route));
    EXPECT_EQ(VN_IN_USE, Vn_ClearBusEffectDynamicRoute(VN_BUS_MASTER, 0, 0, VN_EFFECT_COMPRESSOR));
    EXPECT_EQ(VN_IN_USE, Vn_SetBusFaderPoint(VN_BUS_MASTER, 0));
    EXPECT_EQ(VN_OK, Vn_SetBusOutputVolume(VN_BUS_MASTER, -6.0f, 0));
    EXPECT_EQ(VN_IN_USE, Vn_SetBusVolume(VN_BUS_MASTER, -6.0f, 0));
    EXPECT_EQ(VN_IN_USE, Vn_SetBusMute(VN_BUS_MASTER, 1, 0));
    EXPECT_EQ(VN_IN_USE, Vn_SetBusSolo(VN_BUS_MASTER, 1, 0));
    EXPECT_EQ(VN_IN_USE, Vn_SetBusOutputLayout(VN_BUS_MASTER, matrix.dstLayout));
    EXPECT_EQ(VN_IN_USE, Vn_SetBusDownmixMatrix(VN_BUS_MASTER, &matrix));
    EXPECT_EQ(VN_IN_USE, Vn_ResetBusDownmixMatrix(VN_BUS_MASTER));
    EXPECT_EQ(head + 1, engine->commands.PublishedHead());
    EXPECT_EQ(revision, engine->acceptedBusTopologyRevision);
    EXPECT_EQ(plans, engine->topologyPlanCount);
    EXPECT_EQ(0u, engine->busMuted[0]);
    EXPECT_EQ(0u, engine->busSoloed[0]);
    EXPECT_EQ(0u, engine->busEffectSlotBypass[0][0]);
    EXPECT_EQ(engine->config.fxSlotsPerBus, engine->busFaderAfterSlots[0]);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    auto* masterBus = engine->buses.BusState(0);
    ASSERT_NE(masterBus, nullptr);
    float expectedOutput = 0.0f;
    ASSERT_TRUE(harken::TryVolumeGainFromDb(-6.0f, &expectedOutput));
    EXPECT_NEAR(masterBus->outputGain.target, expectedOutput, 1e-5f);
    EXPECT_EQ(VN_OK, Vn_SetBusVolume(foreign, -6.0f, 0));
    EXPECT_EQ(VN_OK, Vn_SetBusEffect(foreign, 0, VN_EFFECT_GAIN,
        reinterpret_cast<const VnEffectDesc*>(&gain)));
    EXPECT_EQ(VN_OK, Vn_SetBusEffectBypass(foreign, 0, 1));
}

TEST(BusAdmissionStart, EmptySuccessorDropsOnlyExactHolderAndRetryDoesNotResetMaster) {
    AdmissionSession fixture;
    auto* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    auto view = MakeInstallView(fixture.image);
    view.ownerId = view.token.projectSessionId;
    harken::PreparedProjectGenerationV1 prepared;
    ASSERT_EQ(harken::ProjectGenerationPrepareStatusV1::Ok,
        harken::PrepareProjectGenerationV1(*engine, view, prepared).status);
    ASSERT_EQ(harken::ProjectGenerationCommitStatusV1::Ok,
        harken::CommitPreparedProjectGenerationV1(*engine, prepared).status);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_TRUE(harken::MasterHasAuthoredClaimV1(*engine));

    VnBusTopologyDesc empty{};
    empty.structSize = sizeof(empty);
    empty.busStructSize = sizeof(VnBusTopologyBus);
    empty.sendStructSize = sizeof(VnBusTopologySend);
    empty.ownerId = 77;
    empty.revision = 1;
    ASSERT_NE(empty.ownerId, view.ownerId);
    ASSERT_EQ(VN_OK, Vn_ApplyBusTopology(&empty));
    EXPECT_TRUE(harken::MasterHasAuthoredClaimV1(*engine));
    empty.ownerId = view.token.projectSessionId;
    empty.baseRevision = 0;
    empty.revision = view.ownerRevision + 1;
    const auto beforeRefusal = engine->commands.PublishedHead();
    EXPECT_EQ(VN_REVISION_CONFLICT, Vn_ApplyBusTopology(&empty));
    EXPECT_EQ(beforeRefusal, engine->commands.PublishedHead());
    EXPECT_TRUE(harken::MasterHasAuthoredClaimV1(*engine));
    empty.baseRevision = view.ownerRevision;
    ASSERT_EQ(VN_OK, Vn_ApplyBusTopology(&empty));
    EXPECT_FALSE(harken::MasterHasAuthoredClaimV1(*engine));
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(VN_BUS_MASTER, -9.0f, 0));
    const auto head = engine->commands.PublishedHead();
    const auto revision = engine->acceptedBusTopologyRevision;
    EXPECT_EQ(VN_OK, Vn_ApplyBusTopology(&empty));
    EXPECT_EQ(head, engine->commands.PublishedHead());
    EXPECT_EQ(revision, engine->acceptedBusTopologyRevision);
    EXPECT_FLOAT_EQ(-9.0f, engine->busTargets.Reduce(0, harken::kBusTargetBusVolume));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
}

TEST(BusAdmissionStart, DirectOwnerDoesNotTakeReservedPreviewDestination) {
    AdmissionSession fixture;
    auto* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    const auto view = MakeInstallView(fixture.image);
    harken::PreparedProjectGenerationV1 prepared;
    ASSERT_EQ(harken::ProjectGenerationPrepareStatusV1::Ok,
        harken::PrepareProjectGenerationV1(*engine, view, prepared).status);
    const auto destination = std::find_if(engine->preparedPreviewOwnerSlots.begin(),
        engine->preparedPreviewOwnerSlots.end(), [](auto slot) { return slot != UINT32_MAX; });
    ASSERT_NE(engine->preparedPreviewOwnerSlots.end(), destination);
    const auto reservedSlot = *destination;
    ASSERT_FALSE(engine->busTopologyOwners[reservedSlot].live);
    VnBusTopologyDesc direct{};
    direct.structSize = sizeof(direct);
    direct.busStructSize = sizeof(VnBusTopologyBus);
    direct.sendStructSize = sizeof(VnBusTopologySend);
    direct.ownerId = view.ownerId;
    direct.revision = 1;
    ASSERT_EQ(VN_OK, Vn_ApplyBusTopology(&direct));
    EXPECT_FALSE(engine->busTopologyOwners[reservedSlot].live);
    EXPECT_TRUE(prepared.Valid());
    EXPECT_TRUE(std::any_of(engine->busTopologyOwners.begin(), engine->busTopologyOwners.end(),
        [&](const auto& row) { return row.live && row.ownerId == direct.ownerId; }));
}

TEST(BusAdmissionStart, PrepareThenCommitWithoutConsumersInstallsPendingClaim) {
    AdmissionSession fixture;
    harken::Engine* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    const std::uint32_t liveBefore = engine->topologyPlans.InUse();
    const harken::ProjectGenerationInstallViewV1 view =
        MakeInstallView(fixture.image);
    harken::PreparedProjectGenerationV1 prepared;
    ASSERT_EQ(harken::PrepareProjectGenerationV1(*engine, view, prepared).status,
              harken::ProjectGenerationPrepareStatusV1::Ok);
    EXPECT_EQ(engine->topologyPlans.InUse(), liveBefore);

    const harken::ProjectGenerationCommitResultV1 committed =
        harken::CommitPreparedProjectGenerationV1(*engine, prepared);
    ASSERT_EQ(committed.status, harken::ProjectGenerationCommitStatusV1::Ok);
    EXPECT_FALSE(prepared.Valid());
    EXPECT_EQ(engine->topologyPlans.InUse(), liveBefore + 1u);
    EXPECT_NE(committed.installedWorldGeneration, 0u);
    EXPECT_EQ(engine->currentWorldReplacementAdmissionClosed, false);
    std::uint32_t pairedAckSlots = 0;
    for (const auto& identity : engine->projectGenerationInstallAckSideTable) {
        if (identity.valid == 1u) {
            ++pairedAckSlots;
        }
    }
    EXPECT_EQ(pairedAckSlots, 1u);

    harken::PlayableStartRequest request{};
    request.root = fixture.image.Root();
    EXPECT_EQ(harken::StartPlayable(*engine, request).status,
              harken::PlayableStartStatus::Backpressure);

    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(harken::StartPlayable(*engine, request).status,
              harken::PlayableStartStatus::Accepted);
    EXPECT_TRUE(harken::CurrentWorldBusAdmissionIsOpenV1(
        *engine, committed.installedWorldGeneration));

    VnBusTopologyDesc unpaired{};
    unpaired.structSize = sizeof(unpaired);
    unpaired.ownerId = 0x53545544494F4255ull;
    unpaired.revision = 1;
    unpaired.busStructSize = sizeof(VnBusTopologyBus);
    unpaired.sendStructSize = sizeof(VnBusTopologySend);
    ASSERT_EQ(VN_OK, Vn_ApplyBusTopology(&unpaired));
    bool admissionArmed = false;
    for (const auto& row : engine->currentWorldBusAdmission) {
        if (row.occupied) {
            admissionArmed = true;
            break;
        }
    }
    EXPECT_FALSE(admissionArmed);
    EXPECT_EQ(harken::CheckPlayableWorldBusAdmissionV1(
                  *engine, engine->variables.CurrentPlayableWorld()),
              harken::BusAdmissionVerdict::NotAuthored);
    EXPECT_EQ(harken::StartPlayable(*engine, request).status,
              harken::PlayableStartStatus::Accepted);
}

TEST(BusAdmissionStart, PrepareWithLiveConsumersReturnsBackpressure) {
    AdmissionSession fixture;
    harken::Engine* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    harken::PlayableStartRequest request{};
    request.root = fixture.image.Root();
    ASSERT_EQ(harken::StartPlayable(*engine, request).status,
              harken::PlayableStartStatus::Accepted);
    ASSERT_TRUE(harken::HasCurrentWorldPublicationConsumers(*engine));

    const std::uint32_t liveBefore = engine->topologyPlans.InUse();
    const std::uint32_t preparedBefore = engine->preparedTopologyPlans.InUse();
    const std::uint32_t commandFreeBefore = engine->commands.FreeSlots();
    const harken::ProjectGenerationInstallViewV1 view =
        MakeInstallView(fixture.image);
    harken::PreparedProjectGenerationV1 prepared;
    const harken::ProjectGenerationPrepareResultV1 preparedResult =
        harken::PrepareProjectGenerationV1(*engine, view, prepared);
    EXPECT_EQ(preparedResult.status,
              harken::ProjectGenerationPrepareStatusV1::Backpressure);
    EXPECT_FALSE(prepared.Valid());
    EXPECT_EQ(engine->topologyPlans.InUse(), liveBefore);
    EXPECT_EQ(engine->preparedTopologyPlans.InUse(), preparedBefore);
    EXPECT_EQ(engine->commands.FreeSlots(), commandFreeBefore);
    EXPECT_EQ(engine->currentWorldReplacementAdmissionClosed, false);
}

TEST(BusAdmissionStart, NeutralProjectionMixNeDeviceMatchesHeaderTables) {
    AdmissionSession fixture(VN_CHANNEL_LAYOUT_5_1, VN_CHANNEL_LAYOUT_STEREO);
    harken::Engine* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    ASSERT_EQ(engine->config.mixChannelLayout, VN_CHANNEL_LAYOUT_5_1);
    ASSERT_EQ(engine->config.deviceChannelLayout, VN_CHANNEL_LAYOUT_STEREO);
    ASSERT_EQ(Vn_SetBusOutputLayout(VN_BUS_MASTER, VN_CHANNEL_LAYOUT_MONO),
              VN_OK);

    const harken::ProjectGenerationInstallViewV1 view =
        MakeInstallView(fixture.image);
    harken::PreparedProjectGenerationV1 prepared;
    ASSERT_EQ(harken::PrepareProjectGenerationV1(*engine, view, prepared).status,
              harken::ProjectGenerationPrepareStatusV1::Ok);
    const harken::TopologyPlanView plan =
        engine->preparedTopologyPlans.View(prepared.PreparedPlanIndex());
    ASSERT_TRUE(plan.valid);
    ASSERT_GE(plan.slotCount, 1u);
    ASSERT_NE(plan.outputLayouts, nullptr);
    ASSERT_NE(plan.downmixCoeffs, nullptr);
    ASSERT_NE(plan.busIds, nullptr);
    ASSERT_NE(plan.alive, nullptr);
    ASSERT_NE(plan.busVolumeApply, nullptr);

    float expectedMaster[64];
    float expectedNonMaster[64];
    harken::DefaultDownmixMatrix(VN_CHANNEL_LAYOUT_5_1, VN_CHANNEL_LAYOUT_STEREO,
                                expectedMaster);
    harken::DefaultDownmixMatrix(VN_CHANNEL_LAYOUT_5_1, VN_CHANNEL_LAYOUT_5_1,
                                expectedNonMaster);
    bool sawMaster = false;
    for (std::uint32_t i = 0; i < plan.slotCount; ++i) {
        if (plan.alive[i] == 0) {
            continue;
        }
        EXPECT_EQ(plan.busVolumeApply[i], harken::kApplyBusNeutralProperties |
            harken::kApplyBusVolume | harken::kApplyBusOutputVolume);
        EXPECT_FLOAT_EQ(0.0f, plan.preparedBusVolumeDb[i]);
        EXPECT_FLOAT_EQ(0.0f, plan.preparedBusOutputVolumeDb[i]);
        EXPECT_EQ(0u, plan.preparedBusVolumeRampMs[i]);
        const float* coeffs =
            plan.downmixCoeffs + static_cast<std::size_t>(i) * 64;
        if (plan.busIds[i] == VN_BUS_MASTER) {
            sawMaster = true;
            EXPECT_EQ(plan.outputLayouts[i], VN_CHANNEL_LAYOUT_STEREO);
            EXPECT_EQ(std::memcmp(coeffs, expectedMaster, sizeof(expectedMaster)),
                      0);
            EXPECT_NE(std::memcmp(coeffs, engine->busDownmixCoeffs.data(),
                                  sizeof(expectedMaster)),
                      0);
        } else {
            EXPECT_EQ(plan.outputLayouts[i], VN_CHANNEL_LAYOUT_5_1);
            EXPECT_EQ(
                std::memcmp(coeffs, expectedNonMaster, sizeof(expectedNonMaster)),
                0);
        }
    }
    EXPECT_TRUE(sawMaster);
    harken::CancelPreparedProjectGenerationV1(*engine, std::move(prepared));
}

TEST(BusAdmissionStart, CancelAfterPrepareInvalidatesCommit) {
    AdmissionSession fixture;
    harken::Engine* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    const std::uint32_t liveBefore = engine->topologyPlans.InUse();
    const harken::ProjectGenerationInstallViewV1 view =
        MakeInstallView(fixture.image);
    harken::PreparedProjectGenerationV1 prepared;
    ASSERT_EQ(harken::PrepareProjectGenerationV1(*engine, view, prepared).status,
              harken::ProjectGenerationPrepareStatusV1::Ok);
    EXPECT_EQ(engine->preparedTopologyPlans.InUse(), 1u);
    harken::CancelPreparedProjectGenerationV1(*engine, std::move(prepared));
    EXPECT_FALSE(prepared.Valid());
    EXPECT_EQ(engine->preparedTopologyPlans.InUse(), 0u);
    EXPECT_EQ(engine->topologyPlans.InUse(), liveBefore);
    EXPECT_EQ(harken::CommitPreparedProjectGenerationV1(*engine, prepared).status,
              harken::ProjectGenerationCommitStatusV1::InvalidPrepared);
    EXPECT_EQ(engine->topologyPlans.InUse(), liveBefore);
}

TEST(BusAdmissionStart, IdempotentCommitDoesNotIncrementGeneration) {
    AdmissionSession fixture;
    harken::Engine* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    const harken::WorldGeneration generationBefore =
        engine->variables.CurrentWorldGeneration();
    ASSERT_NE(generationBefore, 0u);
    const harken::ProjectGenerationInstallViewV1 view =
        MakeInstallView(fixture.image);
    harken::PreparedProjectGenerationV1 prepared;
    ASSERT_EQ(harken::PrepareProjectGenerationV1(*engine, view, prepared).status,
              harken::ProjectGenerationPrepareStatusV1::Ok);
    const harken::ProjectGenerationCommitResultV1 committed =
        harken::CommitPreparedProjectGenerationV1(*engine, prepared);
    ASSERT_EQ(committed.status, harken::ProjectGenerationCommitStatusV1::Ok);
    EXPECT_EQ(committed.installedWorldGeneration, generationBefore);
    EXPECT_EQ(engine->variables.CurrentWorldGeneration(), generationBefore);
}

TEST(BusAdmissionStart, PreparedPoolAcquireDoesNotChangeLiveInUse) {
    AdmissionSession fixture;
    harken::Engine* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    const std::uint32_t liveBefore = engine->topologyPlans.InUse();
    const std::uint32_t preparedBefore = engine->preparedTopologyPlans.InUse();
    const std::uint32_t preparedIndex = engine->preparedTopologyPlans.Acquire();
    ASSERT_NE(preparedIndex, harken::kInvalidTopologyPlanIndex);
    EXPECT_EQ(engine->topologyPlans.InUse(), liveBefore);
    EXPECT_EQ(engine->preparedTopologyPlans.InUse(), preparedBefore + 1u);
    engine->preparedTopologyPlans.Recycle(preparedIndex);
    EXPECT_EQ(engine->topologyPlans.InUse(), liveBefore);
    EXPECT_EQ(engine->preparedTopologyPlans.InUse(), preparedBefore);
    EXPECT_EQ(engine->preparedOwnerClaims.size(),
              engine->busTopologyOwners.size());
    EXPECT_EQ(engine->projectGenerationInstallAckSideTable.size(),
              engine->topologyPlans.Capacity());
}

TEST(BusAdmissionStart, UnpairedCurrentWorldStillStarts) {
    AdmissionSession fixture;
    harken::Engine* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    const std::uint32_t livePlans = engine->topologyPlans.InUse();

    harken::PlayableStartRequest request{};
    request.root = fixture.image.Root();
    const harken::PlayableStartResult started =
        harken::StartPlayable(*engine, request);
    ASSERT_EQ(started.status, harken::PlayableStartStatus::Accepted);
    EXPECT_NE(started.instance, 0u);
    EXPECT_EQ(engine->topologyPlans.InUse(), livePlans);

    VnPreviewPlayPlayableDescV2 preview{};
    preview.structSize = sizeof(preview);
    preview.playableKind = VN_PREVIEW_PLAYABLE_KIND_SOUND;
    preview.stableId = harkentest::ObjectId(kSound);
    VnPreviewPlayableHandle handle = 0;
    EXPECT_EQ(VnPreview_PlayPlayable(&preview, &handle), VN_PREVIEW_OK);
    EXPECT_NE(handle, 0u);

    VnPostActionDesc action{};
    action.structSize = sizeof(action);
    action.actionId = harkentest::ObjectId(kAction);
    VnActionFiringHandle firing = 0;
    EXPECT_EQ(Vn_PostAction(&action, &firing), VN_OK);
}

TEST(BusAdmissionStart, ArmedMissingClaimRefusesAllThreeStarts) {
    AdmissionSession fixture;
    harken::Engine* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    const harken::WorldGeneration generation =
        engine->variables.CurrentPlayableWorld().generation;
    ASSERT_NE(generation, 0u);
    harken::ArmCurrentWorldBusAdmissionV1(
        *engine, generation + 1u, StudioKey(),
        harken::BusAdmissionClaimState::Open);
    const std::uint32_t livePlans = engine->topologyPlans.InUse();

    harken::PlayableStartRequest request{};
    request.root = fixture.image.Root();
    const harken::PlayableStartResult started =
        harken::StartPlayable(*engine, request);
    EXPECT_EQ(started.status, harken::PlayableStartStatus::GenerationConflict);
    EXPECT_EQ(started.instance, 0u);
    EXPECT_EQ(engine->topologyPlans.InUse(), livePlans);

    VnPreviewPlayPlayableDescV2 preview{};
    preview.structSize = sizeof(preview);
    preview.playableKind = VN_PREVIEW_PLAYABLE_KIND_CONTAINER;
    preview.stableId = harkentest::ObjectId(kContainer);
    VnPreviewPlayableHandle handle = 99;
    EXPECT_EQ(VnPreview_PlayPlayable(&preview, &handle),
              VN_PREVIEW_REVISION_CONFLICT);
    EXPECT_EQ(handle, 0u);

    VnPostActionDesc action{};
    action.structSize = sizeof(action);
    action.actionId = harkentest::ObjectId(kAction);
    VnActionFiringHandle firing = 99;
    EXPECT_EQ(Vn_PostAction(&action, &firing), VN_REVISION_CONFLICT);
    EXPECT_EQ(firing, 0u);
}

TEST(BusAdmissionStart, OpenMatchingClaimAdmitsAndPendingBackpressures) {
    AdmissionSession fixture;
    harken::Engine* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    const harken::WorldGeneration generation =
        engine->variables.CurrentPlayableWorld().generation;
    ASSERT_NE(generation, 0u);

    harken::ArmCurrentWorldBusAdmissionV1(
        *engine, generation, StudioKey(),
        harken::BusAdmissionClaimState::Pending);
    harken::PlayableStartRequest request{};
    request.root = fixture.image.Root();
    EXPECT_EQ(harken::StartPlayable(*engine, request).status,
              harken::PlayableStartStatus::Backpressure);

    VnPostActionDesc pendingAction{};
    pendingAction.structSize = sizeof(pendingAction);
    pendingAction.actionId = harkentest::ObjectId(kAction);
    VnActionFiringHandle pendingFiring = 99;
    EXPECT_EQ(Vn_PostAction(&pendingAction, &pendingFiring), VN_BACKPRESSURE);
    EXPECT_EQ(pendingFiring, 0u);

    harken::ArmCurrentWorldBusAdmissionV1(
        *engine, generation, StudioKey(),
        harken::BusAdmissionClaimState::Open);
    const harken::PlayableStartResult admitted =
        harken::StartPlayable(*engine, request);
    EXPECT_EQ(admitted.status, harken::PlayableStartStatus::Accepted);
    EXPECT_NE(admitted.instance, 0u);

    harken::ArmCurrentWorldBusAdmissionV1(
        *engine, generation, StudioKey(),
        harken::BusAdmissionClaimState::Closed);
    EXPECT_EQ(harken::StartPlayable(*engine, request).status,
              harken::PlayableStartStatus::GenerationConflict);
}

struct StagedTraceFixture {
    harkentest::EngineSession session{AdmissionConfig()};
    AdmissionWorldImage image{1};
    std::string path = std::string(HARKEN_TEMP_DIR "/") + "u3_9_staged_pair.vtrc";
    std::uint32_t plan = harken::kInvalidTopologyPlanIndex;

    StagedTraceFixture() {
        EXPECT_EQ(VN_OK, session.InitResult());
        std::filesystem::create_directories(HARKEN_TEMP_DIR);
        EXPECT_EQ(VN_OK, Vn_TraceStart(path.c_str()));
        auto& plans = session.Engine()->preparedTopologyPlans;
        plan = plans.Acquire();
        EXPECT_NE(harken::kInvalidTopologyPlanIndex, plan);
        plans.SetSlotCount(plan, 1);
        plans.SetOrderCount(plan, 1);
        plans.OrderData(plan)[0] = 0;
        plans.BusIdsData(plan)[0] = VN_BUS_MASTER;
        plans.GenerationsData(plan)[0] = 1;
        plans.ParentsData(plan)[0] = 0;
    }
    ~StagedTraceFixture() {
        harken::SetTraceRecorderFaultForTests(harken::TraceRecorderFault::None);
        session.Engine()->preparedTopologyPlans.Recycle(plan);
    }
    bool Stage(harken::PreparedProjectGenerationTraceV1* out) {
        const auto install = MakeInstallView(image);
        harken::WorldInstallResult result{};
        result.status = harken::WorldInstallStatus::Ok;
        result.resultGeneration = 1;
        return harken::TraceRecorder::PrepareProjectGenerationTraceV1(
            *session.Engine(), session.Engine()->preparedTopologyPlans.View(plan),
            image.world, result, install.token, 101, 202, 1, out);
    }
    void Fail(bool writeFailure) {
        auto* trace = session.Engine()->trace.get();
        if (writeFailure) {
            trace->InjectWriteFailure();
            const std::vector<std::uint8_t> bytes(300 * 1024, 0);
            harken::TraceCallScribe call(trace, harken::kTraceCallSetValue);
            call.Bytes(bytes.data(), bytes.size());
        } else {
            harken::SetTraceRecorderFaultForTests(harken::TraceRecorderFault::Append);
            { harken::TraceCallScribe call(trace, harken::kTraceCallSetValue); }
            harken::SetTraceRecorderFaultForTests(harken::TraceRecorderFault::None);
        }
    }
};

TEST(BusAdmissionStart, StagedTracePublishesExactlyTwoExistingGrammarRecords) {
    StagedTraceFixture fixture;
    auto& engine = *fixture.session.Engine();
    ASSERT_NE(nullptr, engine.trace);
    const auto ordinal = engine.trace->NextOrdinal();
    const auto size = engine.trace->BufferedBytes();
    harken::PreparedProjectGenerationTraceV1 staged;
    ASSERT_TRUE(fixture.Stage(&staged));
    EXPECT_EQ(ordinal, engine.trace->NextOrdinal());
    EXPECT_EQ(size, engine.trace->BufferedBytes());
    ASSERT_TRUE(harken::TraceRecorder::CanCommitProjectGenerationTraceV1(staged));
    const auto hits = harken::memguard::HitCount();
    {
        harken::memguard::AudioThreadScope scope;
        harken::TraceRecorder::CommitProjectGenerationTraceV1(&staged);
    }
    EXPECT_EQ(hits, harken::memguard::HitCount());
    EXPECT_FALSE(staged.IsPrepared());
    EXPECT_EQ(ordinal + 2, engine.trace->NextOrdinal());
    EXPECT_GT(engine.trace->BufferedBytes(), size);
    ASSERT_EQ(VN_OK, Vn_TraceStop());
    std::ifstream in(fixture.path, std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
    ASSERT_GE(bytes.size(), harken::kTraceFixedHeaderBytes);
    std::uint32_t header = 0;
    std::memcpy(&header, bytes.data() + 8, sizeof(header));
    std::size_t offset = header;
    std::vector<std::uint16_t> types;
    std::vector<std::uint64_t> ordinals;
    while (offset + harken::kTraceRecordHeaderBytes <= bytes.size()) {
        std::uint32_t payload = 0;
        std::uint16_t type = 0;
        std::uint64_t recordOrdinal = 0;
        std::memcpy(&payload, bytes.data() + offset, 4);
        std::memcpy(&type, bytes.data() + offset + 4, 2);
        std::memcpy(&recordOrdinal, bytes.data() + offset + 8, 8);
        offset += harken::kTraceRecordHeaderBytes;
        ASSERT_LE(payload, bytes.size() - offset);
        types.push_back(type);
        ordinals.push_back(recordOrdinal);
        offset += payload;
    }
    EXPECT_EQ(bytes.size(), offset);
    const auto found = std::find(types.begin(), types.end(), harken::kTraceRecordPreparedTopologyPlan);
    ASSERT_NE(types.end(), found);
    const auto index = static_cast<std::size_t>(found - types.begin());
    ASSERT_LT(index + 1, types.size());
    EXPECT_EQ(harken::kTraceRecordCurrentWorldInstall, types[index + 1]);
    EXPECT_EQ(ordinal, ordinals[index]);
    EXPECT_EQ(ordinal + 1, ordinals[index + 1]);
    EXPECT_EQ(1, std::count(types.begin(), types.end(), harken::kTraceRecordPreparedTopologyPlan));
    EXPECT_EQ(1, std::count(types.begin(), types.end(), harken::kTraceRecordCurrentWorldInstall));
}

TEST(BusAdmissionStart, StagedTracePreparationFaultsLeaveRecorderUnchanged) {
    StagedTraceFixture fixture;
    auto& engine = *fixture.session.Engine();
    ASSERT_NE(nullptr, engine.trace);
    const auto ordinal = engine.trace->NextOrdinal();
    const auto size = engine.trace->BufferedBytes();
    for (auto fault : {harken::TraceRecorderFault::ProjectWorldEncode,
                       harken::TraceRecorderFault::ProjectBufferReserve}) {
        harken::PreparedProjectGenerationTraceV1 staged;
        harken::SetTraceRecorderFaultForTests(fault);
        EXPECT_FALSE(fixture.Stage(&staged));
        harken::SetTraceRecorderFaultForTests(harken::TraceRecorderFault::None);
        EXPECT_FALSE(staged.IsPrepared());
        EXPECT_TRUE(engine.trace->Active());
        EXPECT_FALSE(engine.traceFailedSticky);
        EXPECT_EQ(ordinal, engine.trace->NextOrdinal());
        EXPECT_EQ(size, engine.trace->BufferedBytes());
    }
    harken::PreparedProjectGenerationTraceV1 staged;
    ASSERT_TRUE(fixture.Stage(&staged));
    { harken::TraceCallScribe call(engine.trace.get(), harken::kTraceCallSetValue); call.ObjectId(harkentest::ObjectId(1)).F32(0.0f); }
    EXPECT_FALSE(harken::TraceRecorder::CanCommitProjectGenerationTraceV1(staged));
}

TEST(BusAdmissionStart, FailedNonnullTraceAllowsEmptyStageButInvalidatesActiveToken) {
    for (bool writeFailure : {false, true}) {
        StagedTraceFixture fixture;
        auto& engine = *fixture.session.Engine();
        harken::PreparedProjectGenerationTraceV1 active;
        ASSERT_TRUE(fixture.Stage(&active));
        fixture.Fail(writeFailure);
        ASSERT_NE(nullptr, engine.trace);
        ASSERT_FALSE(engine.trace->Active());
        ASSERT_TRUE(engine.traceFailedSticky);
        EXPECT_FALSE(harken::TraceRecorder::CanCommitProjectGenerationTraceV1(active));
        const auto* identity = engine.trace.get();
        const auto ordinal = engine.trace->NextOrdinal();
        const auto size = engine.trace->BufferedBytes();
        EXPECT_EQ(0u, size);
        const auto epoch = engine.traceSessionEpoch;
        harken::PreparedProjectGenerationTraceV1 empty;
        harken::SetTraceRecorderFaultForTests(harken::TraceRecorderFault::ProjectWorldEncode);
        const auto hits = harken::memguard::HitCount();
        bool prepared = false;
        {
            harken::memguard::AudioThreadScope scope;
            prepared = fixture.Stage(&empty);
        }
        harken::SetTraceRecorderFaultForTests(harken::TraceRecorderFault::None);
        ASSERT_TRUE(prepared);
        ASSERT_TRUE(harken::TraceRecorder::CanCommitProjectGenerationTraceV1(empty));
        {
            harken::memguard::AudioThreadScope scope;
            harken::TraceRecorder::CommitProjectGenerationTraceV1(&empty);
        }
        EXPECT_EQ(hits, harken::memguard::HitCount());
        EXPECT_EQ(identity, engine.trace.get());
        EXPECT_EQ(epoch, engine.traceSessionEpoch);
        EXPECT_EQ(ordinal, engine.trace->NextOrdinal());
        EXPECT_EQ(size, engine.trace->BufferedBytes());
        EXPECT_TRUE(engine.traceFailedSticky);
        harken::PreparedProjectGenerationTraceV1 beforeStop;
        ASSERT_TRUE(fixture.Stage(&beforeStop));
        ASSERT_EQ(VN_OK, Vn_TraceStop());
        EXPECT_EQ(epoch + 1, engine.traceSessionEpoch);
        EXPECT_FALSE(harken::TraceRecorder::CanCommitProjectGenerationTraceV1(beforeStop));
    }
}

TEST(BusAdmissionStart, TraceEpochCoversNullStopFailedAttemptRestartAndSaturation) {
    StagedTraceFixture fixture;
    auto& engine = *fixture.session.Engine();
    ASSERT_EQ(VN_OK, Vn_TraceStop());
    const auto stoppedEpoch = engine.traceSessionEpoch;
    harken::PreparedProjectGenerationTraceV1 inactive;
    ASSERT_TRUE(fixture.Stage(&inactive));
    ASSERT_EQ(VN_OK, Vn_TraceStop());
    EXPECT_EQ(stoppedEpoch, engine.traceSessionEpoch);
    EXPECT_TRUE(harken::TraceRecorder::CanCommitProjectGenerationTraceV1(inactive));
    EXPECT_EQ(VN_BAD_TRACE_FILE, Vn_TraceStart(HARKEN_TEMP_DIR));
    EXPECT_EQ(stoppedEpoch + 1, engine.traceSessionEpoch);
    EXPECT_EQ(nullptr, engine.trace);
    EXPECT_FALSE(harken::TraceRecorder::CanCommitProjectGenerationTraceV1(inactive));
    ASSERT_EQ(VN_OK, Vn_TraceStart(fixture.path.c_str()));
    harken::PreparedProjectGenerationTraceV1 active;
    ASSERT_TRUE(fixture.Stage(&active));
    ASSERT_EQ(VN_OK, Vn_TraceStop());
    ASSERT_EQ(VN_OK, Vn_TraceStart(fixture.path.c_str()));
    EXPECT_FALSE(harken::TraceRecorder::CanCommitProjectGenerationTraceV1(active));
    engine.traceSessionEpoch = UINT64_MAX;
    ASSERT_EQ(VN_OK, Vn_TraceStop());
    EXPECT_EQ(UINT64_MAX, engine.traceSessionEpoch);
    EXPECT_EQ(nullptr, engine.trace);
    ASSERT_EQ(VN_OK, Vn_TraceStop());
    EXPECT_EQ(UINT64_MAX, engine.traceSessionEpoch);
    EXPECT_EQ(VN_DEVICE_ERROR, Vn_TraceStart(fixture.path.c_str()));
    EXPECT_EQ(nullptr, engine.trace);
}

TEST(BusAdmissionStart, PairedCommitWritesCurrentWorldInstallWithBoundTail) {
    AdmissionSession fixture;
    harken::Engine* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    std::filesystem::create_directories(HARKEN_TEMP_DIR);
    const std::string path =
        std::string(HARKEN_TEMP_DIR "/") + "plan09_u2_commit_type6.vtrc";
    std::filesystem::remove(path);
    auto recorder = std::make_unique<harken::TraceRecorder>(*engine);
    ASSERT_EQ(VN_OK, recorder->Start(path.c_str()));
    engine->trace = std::move(recorder);

    const harken::ProjectGenerationInstallViewV1 view =
        MakeInstallView(fixture.image);
    harken::PreparedProjectGenerationV1 prepared;
    ASSERT_EQ(harken::PrepareProjectGenerationV1(*engine, view, prepared).status,
              harken::ProjectGenerationPrepareStatusV1::Ok);
    ASSERT_EQ(harken::CommitPreparedProjectGenerationV1(*engine, prepared).status,
              harken::ProjectGenerationCommitStatusV1::Ok);
    engine->trace->Stop();
    engine->trace.reset();

    std::ifstream in(path, std::ios::binary);
    ASSERT_TRUE(in);
    const std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(in)),
        std::istreambuf_iterator<char>());
    ASSERT_GE(bytes.size(), harken::kTraceFixedHeaderBytes);
    std::uint32_t headerBytes = 0;
    std::memcpy(&headerBytes, bytes.data() + 8, sizeof(headerBytes));
    std::size_t offset = headerBytes;
    bool foundPairedWorld = false;
    bool foundPreparedPlan = false;
    while (offset + harken::kTraceRecordHeaderBytes <= bytes.size()) {
        std::uint32_t payloadBytes = 0;
        std::uint16_t type = 0;
        std::memcpy(&payloadBytes, bytes.data() + offset, sizeof(payloadBytes));
        std::memcpy(&type, bytes.data() + offset + 4, sizeof(type));
        offset += harken::kTraceRecordHeaderBytes;
        ASSERT_LE(payloadBytes, bytes.size() - offset);
        if (type == harken::kTraceRecordPreparedTopologyPlan &&
            payloadBytes >= 16u)
            foundPreparedPlan = true;
        if (type == harken::kTraceRecordCurrentWorldInstall &&
            payloadBytes >= 32u + 52u) {
            std::uint64_t sessionId = 0;
            std::uint64_t runtimeEpoch = 0;
            std::memcpy(&sessionId,
                        bytes.data() + offset + payloadBytes - 52u, 8);
            std::memcpy(&runtimeEpoch,
                        bytes.data() + offset + payloadBytes - 44u, 8);
            if (sessionId == 11u && runtimeEpoch == 3u)
                foundPairedWorld = true;
        }
        offset += payloadBytes;
    }
    EXPECT_TRUE(foundPreparedPlan);
    EXPECT_TRUE(foundPairedWorld);
}

std::uint32_t LiveOwnerRevision(const harken::Engine& engine, std::uint64_t ownerId) {
    for (const auto& owner : engine.busTopologyOwners) {
        if (owner.live && owner.ownerId == ownerId) {
            return static_cast<std::uint32_t>(owner.currentRevision);
        }
    }
    return 0;
}

std::uint32_t ValidAckCount(const harken::Engine& engine) {
    std::uint32_t count = 0;
    for (const auto& identity : engine.projectGenerationInstallAckSideTable) {
        if (identity.valid == 1u) {
            ++count;
        }
    }
    return count;
}

TEST(BusAdmissionStart, CommitBackpressuresWhenLivePoolIsExhausted) {
    AdmissionSession fixture;
    harken::Engine* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    const harken::ProjectGenerationInstallViewV1 view = MakeInstallView(fixture.image);
    harken::PreparedProjectGenerationV1 prepared;
    ASSERT_EQ(harken::PrepareProjectGenerationV1(*engine, view, prepared).status,
              harken::ProjectGenerationPrepareStatusV1::Ok);
    EXPECT_TRUE(prepared.Valid());

    std::vector<std::uint32_t> leased;
    while (engine->topologyPlans.InUse() < engine->topologyPlans.Capacity()) {
        const std::uint32_t index = engine->topologyPlans.Acquire();
        ASSERT_NE(index, harken::kInvalidTopologyPlanIndex);
        leased.push_back(index);
    }
    ASSERT_EQ(engine->topologyPlans.InUse(), engine->topologyPlans.Capacity());
    const std::uint32_t liveBefore = engine->topologyPlans.InUse();
    const std::uint32_t preparedBefore = engine->preparedTopologyPlans.InUse();
    const std::uint32_t commandFreeBefore = engine->commands.FreeSlots();
    const std::uint32_t ownerRevisionBefore = LiveOwnerRevision(*engine, 77);
    const std::uint32_t ackBefore = ValidAckCount(*engine);
    const harken::WorldGeneration generationBefore =
        engine->variables.CurrentWorldGeneration();

    const harken::ProjectGenerationCommitResultV1 committed =
        harken::CommitPreparedProjectGenerationV1(*engine, prepared);
    EXPECT_EQ(committed.status, harken::ProjectGenerationCommitStatusV1::Backpressure);
    EXPECT_TRUE(prepared.Valid());
    EXPECT_EQ(engine->topologyPlans.InUse(), liveBefore);
    EXPECT_EQ(engine->preparedTopologyPlans.InUse(), preparedBefore);
    EXPECT_EQ(engine->commands.FreeSlots(), commandFreeBefore);
    EXPECT_EQ(LiveOwnerRevision(*engine, 77), ownerRevisionBefore);
    EXPECT_EQ(ValidAckCount(*engine), ackBefore);
    EXPECT_EQ(engine->variables.CurrentWorldGeneration(), generationBefore);

    for (const std::uint32_t index : leased) {
        engine->topologyPlans.Recycle(index);
    }
    const harken::ProjectGenerationCommitResultV1 retried =
        harken::CommitPreparedProjectGenerationV1(*engine, prepared);
    EXPECT_EQ(retried.status, harken::ProjectGenerationCommitStatusV1::Ok);
    EXPECT_FALSE(prepared.Valid());
}

TEST(BusAdmissionStart, CommitBackpressuresWhenCommandQueueIsFull) {
    AdmissionSession fixture;
    harken::Engine* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    const harken::ProjectGenerationInstallViewV1 view = MakeInstallView(fixture.image);
    harken::PreparedProjectGenerationV1 prepared;
    ASSERT_EQ(harken::PrepareProjectGenerationV1(*engine, view, prepared).status,
              harken::ProjectGenerationPrepareStatusV1::Ok);
    EXPECT_TRUE(prepared.Valid());

    harken::CommandRecord filler{};
    filler.opcode = harken::kOpcodeSetBusVolume;
    while (!engine->commands.Full()) {
        ASSERT_TRUE(engine->commands.TryPush(filler));
    }
    ASSERT_TRUE(engine->commands.Full());
    const std::uint32_t liveBefore = engine->topologyPlans.InUse();
    const std::uint32_t preparedBefore = engine->preparedTopologyPlans.InUse();
    const std::uint32_t commandFreeBefore = engine->commands.FreeSlots();
    const std::uint32_t ownerRevisionBefore = LiveOwnerRevision(*engine, 77);
    const std::uint32_t ackBefore = ValidAckCount(*engine);
    const harken::WorldGeneration generationBefore =
        engine->variables.CurrentWorldGeneration();

    const harken::ProjectGenerationCommitResultV1 committed =
        harken::CommitPreparedProjectGenerationV1(*engine, prepared);
    EXPECT_EQ(committed.status, harken::ProjectGenerationCommitStatusV1::Backpressure);
    EXPECT_TRUE(prepared.Valid());
    EXPECT_EQ(engine->topologyPlans.InUse(), liveBefore);
    EXPECT_EQ(engine->preparedTopologyPlans.InUse(), preparedBefore);
    EXPECT_EQ(engine->commands.FreeSlots(), commandFreeBefore);
    EXPECT_EQ(LiveOwnerRevision(*engine, 77), ownerRevisionBefore);
    EXPECT_EQ(ValidAckCount(*engine), ackBefore);
    EXPECT_EQ(engine->variables.CurrentWorldGeneration(), generationBefore);

    engine->nullBackend.Pump(engine->commands.Capacity());
    const harken::ProjectGenerationCommitResultV1 retried =
        harken::CommitPreparedProjectGenerationV1(*engine, prepared);
    EXPECT_EQ(retried.status, harken::ProjectGenerationCommitStatusV1::Ok);
    EXPECT_FALSE(prepared.Valid());
}

std::uint32_t OccupiedAdmissionCount(const harken::Engine& engine) {
    std::uint32_t count = 0;
    for (const auto& row : engine.currentWorldBusAdmission) {
        if (row.occupied) {
            ++count;
        }
    }
    return count;
}

TEST(BusAdmissionStart, PairedCommitRecyclesStaleCurrentWorldAdmissionRows) {
    VnConfig config = AdmissionConfig();
    config.maxBusTopologyOwners = 2;
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    std::vector<float> samples(4096, 0.25f);
    const VnPCMDesc pcm = harkentest::MonoDesc(samples, 48000);
    ASSERT_EQ(Vn_RegisterPCM(kSound, &pcm), VN_OK);
    harken::Engine* engine = session.Engine();
    ASSERT_NE(engine, nullptr);
    ASSERT_EQ(engine->currentWorldBusAdmission.size(), 2u);
    AdmissionWorldImage image(engine->registry.Lookup(kSound).contentRevision);
    ASSERT_EQ(harken::InstallContainerWorldV1(*engine, image.world).status,
              harken::WorldInstallStatus::Ok);

    harken::WorldGeneration lastInstalled = 0;
    for (std::uint32_t round = 0; round < 5; ++round) {
        image.action.actionStableId = harkentest::OffsetObjectId(kAction, round + 1u);
        image.action.programSemanticFingerprint =
            harken::ComputeActionProgramFingerprintV1(image.action);
        image.BindWorld();
        image.world.identity.runtimeSemanticFingerprint =
            harken::ComputeCurrentWorldFingerprintV5(image.world);
        harken::ProjectGenerationInstallViewV1 view = MakeInstallView(image);
        view.token.submissionSerial = 100u + round;
        view.ownerBaseRevision = LiveOwnerRevision(*engine, view.ownerId);
        view.ownerRevision = view.ownerBaseRevision + 1u;
        harken::PreparedProjectGenerationV1 prepared;
        ASSERT_EQ(harken::PrepareProjectGenerationV1(*engine, view, prepared).status,
                  harken::ProjectGenerationPrepareStatusV1::Ok);
        const harken::ProjectGenerationCommitResultV1 committed =
            harken::CommitPreparedProjectGenerationV1(*engine, prepared);
        ASSERT_EQ(committed.status, harken::ProjectGenerationCommitStatusV1::Ok)
            << "round " << round;
        EXPECT_FALSE(prepared.Valid());
        EXPECT_NE(committed.installedWorldGeneration, lastInstalled);
        lastInstalled = committed.installedWorldGeneration;
        EXPECT_LE(OccupiedAdmissionCount(*engine), 2u);
        ASSERT_EQ(VN_OK, Vn_Update());
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
        EXPECT_TRUE(harken::CurrentWorldBusAdmissionIsOpenV1(
            *engine, committed.installedWorldGeneration))
            << "round " << round;
    }
    EXPECT_EQ(OccupiedAdmissionCount(*engine), 1u);
    harken::PlayableStartRequest last{};
    last.root = image.Root();
    EXPECT_EQ(harken::StartPlayable(*engine, last).status,
              harken::PlayableStartStatus::Accepted);
}

TEST(BusAdmissionStart, WorldEditsReclaimSelectionMemoryForTheSameEmitter) {
    for (const bool paired : {true, false}) {
        SCOPED_TRACE(paired);
        AdmissionSession fixture;
        auto* engine = fixture.session.Engine();
        auto& image = fixture.image;
        image.container.memoryScope = harken::MemoryScopeV1::Emitter;
        harken::VariableDefinitionV1 variable{
            harkentest::ObjectId(41), harken::VariableKindV1::Smooth, 0.0f, 100.0f, 0.0f};
        image.world.variables = &variable;
        image.world.variableCount = 1;
        image.world.identity.capabilityMask |= harken::kWorldCapabilityVariableDefinitions;
        VnEmitterHandle emitter = 0;
        ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
        for (unsigned round = 0; round < 20; ++round) {
            SCOPED_TRACE(round);
            variable.defaultValue = static_cast<float>(round);
            image.world.identity.runtimeSemanticFingerprint = harken::ComputeCurrentWorldFingerprintV5(image.world);
            if (paired) {
                auto view = MakeInstallView(image);
                view.token.submissionSerial = 100u + round;
                view.token.authoringRevision = 100u + round;
                view.ownerBaseRevision = LiveOwnerRevision(*engine, view.ownerId);
                view.ownerRevision = view.ownerBaseRevision + 1;
                harken::PreparedProjectGenerationV1 prepared;
                ASSERT_EQ(harken::ProjectGenerationPrepareStatusV1::Ok,
                    harken::PrepareProjectGenerationV1(*engine, view, prepared).status);
                ASSERT_EQ(harken::ProjectGenerationCommitStatusV1::Ok,
                    harken::CommitPreparedProjectGenerationV1(*engine, prepared).status);
            } else {
                ASSERT_EQ(harken::WorldInstallStatus::Ok,
                    harken::InstallContainerWorldV1(*engine, image.world).status);
            }
            ASSERT_EQ(VN_OK, Vn_Update());
            engine->nullBackend.Pump(1);
            ASSERT_EQ(VN_OK, Vn_Update());
            const auto started = harken::StartCurrentPlayableByIdentity(
                harken::PlayableKindV1::Container, kContainer, emitter);
            ASSERT_EQ(harken::PlayableStartStatus::Accepted, started.status);
            EXPECT_EQ(1u, engine->selectionMemory.RowCount());
            bool heard = false;
            for (unsigned block = 0; block < 96; ++block) {
                ASSERT_EQ(VN_OK, Vn_Update());
                engine->nullBackend.Pump(1);
                heard |= harkentest::Peak(engine->nullBackend.LastBlock(),
                    engine->nullBackend.BlockFrames() * engine->nullBackend.Channels()) > 0.001f;
            }
            EXPECT_TRUE(heard);
            ASSERT_EQ(0u, engine->playableInstances.LiveCount());
            ASSERT_EQ(0u, engine->voices.ActiveCount());
            ASSERT_EQ(0u, engine->selectionMemory.PendingCount());
            ASSERT_FALSE(engine->runtimeRecreateRequired);
        }
    }
}

TEST(BusAdmissionStart, OldSelectionMemoryWaitsForLiveSoundAndPreservesIdempotentOrRejectedInstalls) {
    AdmissionSession fixture;
    auto* engine = fixture.session.Engine();
    auto& image = fixture.image;
    harken::SelectionMemoryKeyV1 key{};
    key.world = engine->variables.CurrentWorldGeneration();
    key.containerStableId = kContainer;
    key.programSemanticFingerprint = image.container.programSemanticFingerprint;
    key.memoryScope = harken::MemoryScopeV1::Global;
    const auto started = harken::StartCurrentPlayableByIdentity(harken::PlayableKindV1::Container, kContainer, 0);
    ASSERT_EQ(harken::PlayableStartStatus::Accepted, started.status);
    ASSERT_TRUE(engine->selectionMemory.Snapshot(key).found);
    const auto ordinal = engine->selectionMemory.Snapshot(key).selectionOrdinal;
    const auto identical = harken::InstallContainerWorldV1(*engine, image.world);
    ASSERT_EQ(harken::WorldInstallStatus::Ok, identical.status);
    ASSERT_TRUE(identical.idempotent);
    EXPECT_EQ(ordinal, engine->selectionMemory.Snapshot(key).selectionOrdinal);
    auto invalid = image.world;
    invalid.identity.runtimeSemanticFingerprint ^= 1;
    EXPECT_NE(harken::WorldInstallStatus::Ok, harken::InstallContainerWorldV1(*engine, invalid).status);
    EXPECT_EQ(ordinal, engine->selectionMemory.Snapshot(key).selectionOrdinal);
    image.action.actionStableId = harkentest::OffsetObjectId(kAction, 1);
    image.action.programSemanticFingerprint = harken::ComputeActionProgramFingerprintV1(image.action);
    image.world.identity.runtimeSemanticFingerprint = harken::ComputeCurrentWorldFingerprintV5(image.world);
    ASSERT_EQ(harken::WorldInstallStatus::Ok, harken::InstallContainerWorldV1(*engine, image.world).status);
    EXPECT_TRUE(engine->selectionMemory.Snapshot(key).found);
    EXPECT_TRUE(engine->playableInstances.Classify(started.instance).live);
    bool heard = false;
    for (unsigned block = 0; block < 96; ++block) {
        ASSERT_EQ(VN_OK, Vn_Update());
        engine->nullBackend.Pump(1);
        heard |= harkentest::Peak(engine->nullBackend.LastBlock(), 128) > 0.001f;
    }
    EXPECT_TRUE(heard);
    EXPECT_EQ(0u, engine->playableInstances.LiveCount());
    EXPECT_FALSE(engine->selectionMemory.Snapshot(key).found);
    EXPECT_EQ(0u, engine->selectionMemory.RowCount());
}

TEST(BusAdmissionStart, CurrentWorldReclamationDoesNotRetirePackAuthoritySelectionMemory) {
    AdmissionSession fixture;
    auto* engine = fixture.session.Engine();
    auto& image = fixture.image;
    ASSERT_EQ(VN_OK, engine->packComposition.EnterPackAuthority());
    harken::SelectionPlayRequestV1 request{};
    request.key.world = engine->variables.CurrentWorldGeneration();
    request.key.containerStableId = kContainer;
    request.key.programSemanticFingerprint = image.container.programSemanticFingerprint;
    request.key.memoryScope = harken::MemoryScopeV1::Global;
    request.program = &image.container;
    request.worldSemanticFingerprint = image.world.identity.runtimeSemanticFingerprint;
    request.rootStableId = kContainer;
    request.acceptedPostOrdinal = request.ticket = 1;
    const auto selection = engine->selectionMemory.AppendPlay(request);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, selection.status);
    ASSERT_EQ(harken::SelectionMemoryStatusV1::Ok, engine->selectionMemory.CommitHead(selection.delta));
    image.action.actionStableId = harkentest::OffsetObjectId(kAction, 1);
    image.action.programSemanticFingerprint = harken::ComputeActionProgramFingerprintV1(image.action);
    image.world.identity.runtimeSemanticFingerprint = harken::ComputeCurrentWorldFingerprintV5(image.world);
    ASSERT_EQ(harken::WorldInstallStatus::Ok, harken::InstallContainerWorldV1(*engine, image.world).status);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_TRUE(engine->selectionMemory.Snapshot(request.key).found);
    EXPECT_EQ(1u, engine->selectionMemory.Snapshot(request.key).selectionOrdinal);
}

TEST(BusAdmissionStart, VolumeV2RendersParentAndChildOnceInFreshFirstBlock) {
    AdmissionSession fixture;
    harken::Engine* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    const VnBusId parentId = harkentest::ObjectId(40);
    const VnBusId childId = harkentest::ObjectId(41);
    const std::array<VnBusId, 3> ids{VN_BUS_MASTER, parentId, childId};
    const std::array<std::uint8_t, 3> alive{1, 1, 1};
    const std::array<std::uint32_t, 3> generations{1, 1, 1};
    const std::array<std::uint32_t, 3> parents{0, 0, 1};
    const auto sendSlots = engine->config.busSendSlotsPerBus;
    std::vector<std::uint32_t> sendTargets(ids.size() * sendSlots,
        harken::kEmptySendTarget);
    std::vector<float> sendLevels(sendTargets.size(), 0.0f);
    std::vector<std::uint8_t> sendLevelApply(sendTargets.size(), 0);
    std::array<std::uint32_t, 3> feeder{}, emittedFeeders{};
    std::array<std::uint8_t, 3> emitted{};
    harken::ProspectiveTopologyView prospective{};
    prospective.busCount = ids.size();
    prospective.busIds = ids.data();
    prospective.generations = generations.data();
    prospective.parents = parents.data();
    prospective.alive = alive.data();
    prospective.sendTargets = sendTargets.data();
    prospective.sendLevels = sendLevels.data();
    prospective.sendLevelApply = sendLevelApply.data();
    prospective.feederCount = feeder.data();
    prospective.emittedFeeders = emittedFeeders.data();
    prospective.emitted = emitted.data();
    const std::array<float, 3> volume{0.0f, -6.0f, -6.0f};
    const std::array<float, 3> neutral{};
    auto view = MakeInstallView(fixture.image);
    view.publicationVintage = 3;
    view.prospective = &prospective;
    view.authoredBusIds = ids.data();
    view.authoredBusVolumeDb = volume.data();
    view.authoredBusMakeUpGainDb = neutral.data();
    view.authoredBusOutputVolumeDb = neutral.data();
    view.authoredBusCount = ids.size();
    harken::PreparedProjectGenerationV1 prepared;
    ASSERT_EQ(harken::ProjectGenerationPrepareStatusV1::Ok,
        harken::PrepareProjectGenerationV1(*engine, view, prepared).status);
    ASSERT_EQ(harken::ProjectGenerationCommitStatusV1::Ok,
        harken::CommitPreparedProjectGenerationV1(*engine, prepared).status);
    const auto beforeInstallBlock = engine->mixer.BlocksRendered();
    engine->nullBackend.Pump(1);
    ASSERT_GT(engine->mixer.BlocksRendered(), beforeInstallBlock);
    ASSERT_EQ(VN_OK, Vn_Update());

    VnPlayDesc play = harkentest::MakePlayDesc(kSound);
    play.busId = childId;
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    const auto beforeSoundBlock = engine->mixer.BlocksRendered();
    engine->nullBackend.Pump(1);
    ASSERT_GT(engine->mixer.BlocksRendered(), beforeSoundBlock);
    const float expected = 0.25f * std::pow(10.0f, -6.0f / 20.0f)
        * std::pow(10.0f, -6.0f / 20.0f);
    const float* output = engine->nullBackend.LastBlock();
    EXPECT_NEAR(expected, output[0], 1e-5f);
    EXPECT_NEAR(expected, output[1], 1e-5f);
}

TEST(BusAdmissionStart, VolumeV2SecondOwnerPreservesForeignMasterAndBusTargets) {
    AdmissionSession fixture;
    auto* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    const VnBusId firstBus = harkentest::ObjectId(40);
    const VnBusId secondBus = harkentest::ObjectId(41);
    std::array<VnBusId, 2> ids{VN_BUS_MASTER, firstBus};
    const std::array<std::uint8_t, 2> alive{1, 1};
    const std::array<std::uint32_t, 2> generations{1, 1};
    const std::array<std::uint32_t, 2> parents{0, 0};
    std::vector<std::uint32_t> sends(ids.size() * engine->config.busSendSlotsPerBus,
        harken::kEmptySendTarget);
    std::vector<float> levels(sends.size(), 0.0f);
    std::vector<std::uint8_t> levelApply(sends.size(), 0);
    std::array<std::uint32_t, 2> feeders{}, emittedFeeders{};
    std::array<std::uint8_t, 2> emitted{};
    harken::ProspectiveTopologyView topology{};
    topology.busCount = ids.size();
    topology.busIds = ids.data();
    topology.generations = generations.data();
    topology.parents = parents.data();
    topology.alive = alive.data();
    topology.sendTargets = sends.data();
    topology.sendLevels = levels.data();
    topology.sendLevelApply = levelApply.data();
    topology.feederCount = feeders.data();
    topology.emittedFeeders = emittedFeeders.data();
    topology.emitted = emitted.data();
    std::array<float, 2> volume{-3.0f, -6.0f};
    const std::array<float, 2> neutral{};
    auto view = MakeInstallView(fixture.image);
    view.publicationVintage = 3;
    view.prospective = &topology;
    view.authoredBusIds = ids.data();
    view.authoredBusVolumeDb = volume.data();
    view.authoredBusMakeUpGainDb = neutral.data();
    view.authoredBusOutputVolumeDb = neutral.data();
    view.authoredBusCount = ids.size();
    harken::PreparedProjectGenerationV1 first;
    ASSERT_EQ(harken::ProjectGenerationPrepareStatusV1::Ok,
        harken::PrepareProjectGenerationV1(*engine, view, first).status);
    ASSERT_EQ(harken::ProjectGenerationCommitStatusV1::Ok,
        harken::CommitPreparedProjectGenerationV1(*engine, first).status);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_FLOAT_EQ(-3.0f, engine->busTargets.SlotValue(0,
        harken::kBusTargetBusVolume, harken::BusTargetStore::kSlotAuthoredIndex));

    ids[1] = secondBus;
    volume = {0.0f, -9.0f};
    view.token.projectSessionId = 12;
    ++view.token.authoringRevision;
    ++view.token.submissionSerial;
    view.ownerId = 78;
    view.ownerRevision = 1;
    harken::PreparedProjectGenerationV1 second;
    ASSERT_EQ(harken::ProjectGenerationPrepareStatusV1::Ok,
        harken::PrepareProjectGenerationV1(*engine, view, second).status);
    ASSERT_EQ(harken::ProjectGenerationCommitStatusV1::Ok,
        harken::CommitPreparedProjectGenerationV1(*engine, second).status);
    const auto before = engine->mixer.BlocksRendered();
    engine->nullBackend.Pump(1);
    ASSERT_GT(engine->mixer.BlocksRendered(), before);
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto firstSlot = engine->buses.FindIndex(firstBus);
    const auto secondSlot = engine->buses.FindIndex(secondBus);
    ASSERT_NE(firstSlot, harken::kInvalidBusIndex);
    ASSERT_NE(secondSlot, harken::kInvalidBusIndex);
    EXPECT_FLOAT_EQ(-3.0f, engine->busTargets.SlotValue(0,
        harken::kBusTargetBusVolume, harken::BusTargetStore::kSlotAuthoredIndex));
    EXPECT_FLOAT_EQ(-6.0f, engine->busTargets.SlotValue(firstSlot,
        harken::kBusTargetBusVolume, harken::BusTargetStore::kSlotAuthoredIndex));
    EXPECT_FLOAT_EQ(-9.0f, engine->busTargets.SlotValue(secondSlot,
        harken::kBusTargetBusVolume, harken::BusTargetStore::kSlotAuthoredIndex));
}

TEST(BusAdmissionStart, VolumeV2KeepsMasterSiblingAndPlayableGainIndependent) {
    AdmissionSession fixture;
    auto* engine = fixture.session.Engine();
    ASSERT_NE(nullptr, engine);
    const VnBusId parentId = harkentest::ObjectId(40);
    const VnBusId childId = harkentest::ObjectId(41);
    const VnBusId siblingId = harkentest::ObjectId(42);
    const std::array<VnBusId, 4> ids{VN_BUS_MASTER, parentId, childId, siblingId};
    const std::array<std::uint8_t, 4> alive{1, 1, 1, 1};
    const std::array<std::uint32_t, 4> generations{1, 1, 1, 1};
    const std::array<std::uint32_t, 4> parents{0, 0, 1, 0};
    const auto sendSlots = engine->config.busSendSlotsPerBus;
    std::vector<std::uint32_t> sendTargets(ids.size() * sendSlots,
        harken::kEmptySendTarget);
    std::vector<float> sendLevels(sendTargets.size(), 0.0f);
    std::vector<std::uint8_t> sendLevelApply(sendTargets.size(), 0);
    std::array<std::uint32_t, 4> feeder{}, emittedFeeders{};
    std::array<std::uint8_t, 4> emitted{};
    harken::ProspectiveTopologyView prospective{};
    prospective.busCount = ids.size();
    prospective.busIds = ids.data();
    prospective.generations = generations.data();
    prospective.parents = parents.data();
    prospective.alive = alive.data();
    prospective.sendTargets = sendTargets.data();
    prospective.sendLevels = sendLevels.data();
    prospective.sendLevelApply = sendLevelApply.data();
    prospective.feederCount = feeder.data();
    prospective.emittedFeeders = emittedFeeders.data();
    prospective.emitted = emitted.data();
    const std::array<float, 4> volume{-3.0f, -6.0f, -6.0f, 0.0f};
    const std::array<float, 4> neutral{};
    auto view = MakeInstallView(fixture.image);
    view.publicationVintage = 3;
    view.prospective = &prospective;
    view.authoredBusIds = ids.data();
    view.authoredBusVolumeDb = volume.data();
    view.authoredBusMakeUpGainDb = neutral.data();
    view.authoredBusOutputVolumeDb = neutral.data();
    view.authoredBusCount = ids.size();
    harken::PreparedProjectGenerationV1 prepared;
    ASSERT_EQ(harken::ProjectGenerationPrepareStatusV1::Ok,
        harken::PrepareProjectGenerationV1(*engine, view, prepared).status);
    ASSERT_EQ(harken::ProjectGenerationCommitStatusV1::Ok,
        harken::CommitPreparedProjectGenerationV1(*engine, prepared).status);
    const auto beforeInstall = engine->mixer.BlocksRendered();
    engine->nullBackend.Pump(1);
    ASSERT_EQ(beforeInstall + 1, engine->mixer.BlocksRendered());
    ASSERT_EQ(VN_OK, Vn_Update());

    const auto playAndCapture = [&](VnBusId busId, float expectedDb) {
        VnPlayDesc play = harkentest::MakePlayDesc(kSound);
        play.busId = busId;
        play.volumeDb = -6.0f;
        VnVoiceHandle voice = 0;
        EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
        EXPECT_NE(0u, voice);
        const auto beforeSound = engine->mixer.BlocksRendered();
        engine->nullBackend.Pump(1);
        EXPECT_EQ(beforeSound + 1, engine->mixer.BlocksRendered());
        const float expected = 0.25f * std::pow(10.0f, expectedDb / 20.0f);
        EXPECT_NEAR(expected, engine->nullBackend.LastBlock()[0], 1e-5f);
        EXPECT_NEAR(expected, engine->nullBackend.LastBlock()[1], 1e-5f);
        EXPECT_EQ(VN_OK, Vn_Stop(voice, 0));
        EXPECT_EQ(VN_OK, Vn_Update());
        engine->nullBackend.Pump(1);
    };
    playAndCapture(childId, -21.0f);
    playAndCapture(siblingId, -9.0f);
}

TEST(BusAdmissionStart, VolumeV2RetainsApiAndBindingAcrossAuthoredUpdateAndRejectsDrift) {
    AdmissionSession fixture;
    auto* engine = fixture.session.Engine();
    ASSERT_NE(nullptr, engine);
    constexpr VnVariableId kVariable = harkentest::ObjectId(740);
    constexpr VnBindingId kBinding = harkentest::ObjectId(741);
    auto definition = harkentest::MakeVariableDefinition(
        kVariable, harken::VariableKindV1::Smooth, 0.0f, 1.0f, 0.0f);
    fixture.image.world.variables = &definition;
    fixture.image.world.variableCount = 1;
    fixture.image.world.identity.capabilityMask |= harken::kWorldCapabilityVariableDefinitions;
    fixture.image.world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV5(fixture.image.world);
    const std::array<VnBusId, 1> ids{VN_BUS_MASTER};
    const std::array<std::uint8_t, 1> alive{1};
    const std::array<std::uint32_t, 1> generations{1};
    const std::array<std::uint32_t, 1> parents{0};
    std::vector<std::uint32_t> sends(engine->config.busSendSlotsPerBus,
                                      harken::kEmptySendTarget);
    std::vector<float> sendLevels(sends.size(), 0.0f);
    std::vector<std::uint8_t> sendLevelApply(sends.size(), 0);
    std::array<std::uint32_t, 1> feeder{}, emittedFeeders{};
    std::array<std::uint8_t, 1> emitted{};
    harken::ProspectiveTopologyView topology{};
    topology.busCount = 1;
    topology.busIds = ids.data();
    topology.generations = generations.data();
    topology.parents = parents.data();
    topology.alive = alive.data();
    topology.sendTargets = sends.data();
    topology.sendLevels = sendLevels.data();
    topology.sendLevelApply = sendLevelApply.data();
    topology.feederCount = feeder.data();
    topology.emittedFeeders = emittedFeeders.data();
    topology.emitted = emitted.data();
    std::array<float, 1> volume{0.0f};
    std::array<float, 1> makeUp{0.0f};
    std::array<float, 1> output{-6.0f};
    auto view = MakeInstallView(fixture.image);
    view.publicationVintage = 3;
    view.prospective = &topology;
    view.authoredBusIds = ids.data();
    view.authoredBusVolumeDb = volume.data();
    view.authoredBusMakeUpGainDb = makeUp.data();
    view.authoredBusOutputVolumeDb = output.data();
    view.authoredBusCount = 1;
    harken::PreparedProjectGenerationV1 first;
    ASSERT_EQ(harken::ProjectGenerationPrepareStatusV1::Ok,
        harken::PrepareProjectGenerationV1(*engine, view, first).status);
    ASSERT_EQ(harken::ProjectGenerationCommitStatusV1::Ok,
        harken::CommitPreparedProjectGenerationV1(*engine, first).status);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());

    VnBindingDesc binding{};
    binding.structSize = sizeof(binding);
    binding.variableId = kVariable;
    binding.targetScopeKind = VN_TARGET_SCOPE_BUS;
    binding.targetId = harken::kBusTargetBusVolume;
    binding.curve.structSize = sizeof(VnCurveDesc);
    binding.curve.pointCount = 2;
    binding.curve.points[0] = {0.0f, 0.0f};
    binding.curve.points[1] = {1.0f, -6.0f};
    binding.curve.handles[0] = {0.0f, 0.0f, 1.0f, -6.0f};
    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(attach);
    attach.scopeKind = VN_TARGET_SCOPE_BUS;
    attach.objectId = VN_BUS_MASTER;
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(kBinding, &binding));
    ASSERT_EQ(VN_OK, Vn_AttachBinding(kBinding, &attach));
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1.0f));
    harkentest::AdvanceVariableWrite(engine);
    ASSERT_FLOAT_EQ(-6.0f, engine->busTargets.SlotValue(
        0, harken::kBusTargetBusVolume, harken::BusTargetStore::kSlotVariableIndex));
    ASSERT_EQ(VN_OK, Vn_SetBusOutputVolume(VN_BUS_MASTER, -3.0f, 0));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_TRUE(engine->bindings.HasAttachment(kBinding, VN_TARGET_SCOPE_BUS, VN_BUS_MASTER));

    VnPlayDesc play = harkentest::MakePlayDesc(kSound);
    play.busId = VN_BUS_MASTER;
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    const auto captureFresh = [&](float expectedDb) {
        const auto before = engine->mixer.BlocksRendered();
        EXPECT_EQ(VN_OK, Vn_Update());
        engine->nullBackend.Pump(1);
        EXPECT_EQ(before + 1, engine->mixer.BlocksRendered());
        const float expected = 0.25f * std::pow(10.0f, expectedDb / 20.0f);
        EXPECT_NEAR(expected, engine->nullBackend.LastBlock()[0], 1e-5f);
        EXPECT_NEAR(expected, engine->nullBackend.LastBlock()[1], 1e-5f);
    };
    captureFresh(-15.0f);

    volume[0] = -3.0f;
    output[0] = -9.0f;
    ++view.token.authoringRevision;
    ++view.token.submissionSerial;
    view.ownerBaseRevision = 1;
    view.ownerRevision = 2;
    harken::PreparedProjectGenerationV1 stale;
    ASSERT_EQ(harken::ProjectGenerationPrepareStatusV1::Ok,
        harken::PrepareProjectGenerationV1(*engine, view, stale).status);
    const auto revisionBefore = engine->acceptedBusTopologyRevision;
    ASSERT_EQ(VN_OK, Vn_SetBusOutputVolume(VN_BUS_MASTER, -4.0f, 0));
    EXPECT_EQ(harken::ProjectGenerationCommitStatusV1::Backpressure,
        harken::CommitPreparedProjectGenerationV1(*engine, stale).status);
    EXPECT_EQ(revisionBefore, engine->acceptedBusTopologyRevision);
    EXPECT_EQ(1u, LiveOwnerRevision(*engine, view.ownerId));
    EXPECT_FLOAT_EQ(0.0f, engine->busTargets.SlotValue(
        0, harken::kBusTargetBusVolume, harken::BusTargetStore::kSlotAuthoredIndex));
    EXPECT_FLOAT_EQ(-6.0f, engine->busTargets.SlotValue(
        0, harken::kBusTargetBusOutputVolume, harken::BusTargetStore::kSlotAuthoredIndex));
    harken::CancelPreparedProjectGenerationV1(*engine, std::move(stale));
    captureFresh(-16.0f);

    harken::PreparedProjectGenerationV1 staleInput;
    ASSERT_EQ(harken::ProjectGenerationPrepareStatusV1::Ok,
        harken::PrepareProjectGenerationV1(*engine, view, staleInput).status);
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 0.5f));
    harkentest::AdvanceVariableWrite(engine);
    EXPECT_EQ(harken::ProjectGenerationCommitStatusV1::InvalidPrepared,
        harken::CommitPreparedProjectGenerationV1(*engine, staleInput).status);
    EXPECT_EQ(1u, LiveOwnerRevision(*engine, view.ownerId));
    EXPECT_EQ(revisionBefore, engine->acceptedBusTopologyRevision);
    EXPECT_FLOAT_EQ(0.0f, engine->busTargets.SlotValue(
        0, harken::kBusTargetBusVolume, harken::BusTargetStore::kSlotAuthoredIndex));
    const auto* oldBinding = engine->bindings.Find(kBinding);
    ASSERT_NE(nullptr, oldBinding);
    const float oldDb = -10.0f + oldBinding->Evaluate(0.5f);
    const auto beforeRejectedBlock = engine->mixer.BlocksRendered();
    engine->nullBackend.Pump(1);
    ASSERT_EQ(beforeRejectedBlock + 1, engine->mixer.BlocksRendered());
    EXPECT_NEAR(0.25f * std::pow(10.0f, oldDb / 20.0f),
        engine->nullBackend.LastBlock()[0], 1e-5f);
    harken::CancelPreparedProjectGenerationV1(*engine, std::move(staleInput));
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1.0f));
    harkentest::AdvanceVariableWrite(engine);
    captureFresh(-16.0f);

    harken::PreparedProjectGenerationV1 retry;
    ASSERT_EQ(harken::ProjectGenerationPrepareStatusV1::Ok,
        harken::PrepareProjectGenerationV1(*engine, view, retry).status);
    ASSERT_EQ(harken::ProjectGenerationCommitStatusV1::Ok,
        harken::CommitPreparedProjectGenerationV1(*engine, retry).status);
    EXPECT_EQ(2u, LiveOwnerRevision(*engine, view.ownerId));
    ASSERT_TRUE(engine->bindings.HasAttachment(kBinding, VN_TARGET_SCOPE_BUS, VN_BUS_MASTER));
    EXPECT_FLOAT_EQ(-3.0f, engine->busTargets.SlotValue(
        0, harken::kBusTargetBusVolume, harken::BusTargetStore::kSlotAuthoredIndex));
    EXPECT_FLOAT_EQ(-9.0f, engine->busTargets.SlotValue(
        0, harken::kBusTargetBusOutputVolume, harken::BusTargetStore::kSlotAuthoredIndex));
    EXPECT_FLOAT_EQ(-6.0f, engine->busTargets.SlotValue(
        0, harken::kBusTargetBusVolume, harken::BusTargetStore::kSlotVariableIndex));
    captureFresh(-22.0f);
    definition.defaultValue = 0.5f;
    fixture.image.world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV5(fixture.image.world);
    view.compiledWorld = fixture.image.world;
    ++view.token.authoringRevision;
    ++view.token.submissionSerial;
    view.ownerBaseRevision = 2;
    view.ownerRevision = 3;
    harken::PreparedProjectGenerationV1 changedDefault;
    ASSERT_EQ(harken::ProjectGenerationPrepareStatusV1::Ok,
        harken::PrepareProjectGenerationV1(*engine, view, changedDefault).status);
    ASSERT_EQ(harken::ProjectGenerationCommitStatusV1::Ok,
        harken::CommitPreparedProjectGenerationV1(*engine, changedDefault).status);
    const auto* changedBinding = engine->bindings.Find(kBinding);
    ASSERT_NE(nullptr, changedBinding);
    const float defaultDb = -16.0f + changedBinding->Evaluate(0.5f);
    EXPECT_LT(defaultDb, -16.0f);
    EXPECT_GT(defaultDb, -22.0f);
    const auto beforeDefaultBlock = engine->mixer.BlocksRendered();
    engine->nullBackend.Pump(1);
    ASSERT_EQ(beforeDefaultBlock + 1, engine->mixer.BlocksRendered());
    EXPECT_NEAR(0.25f * std::pow(10.0f, defaultDb / 20.0f),
        engine->nullBackend.LastBlock()[0], 1e-5f);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    EXPECT_NEAR(0.25f * std::pow(10.0f, defaultDb / 20.0f),
        engine->nullBackend.LastBlock()[0], 1e-5f);
    fixture.image.world.variables = nullptr;
    fixture.image.world.variableCount = 0;
    fixture.image.world.identity.capabilityMask &=
        ~harken::kWorldCapabilityVariableDefinitions;
    fixture.image.world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV5(fixture.image.world);
    view.compiledWorld = fixture.image.world;
    ++view.token.authoringRevision;
    ++view.token.submissionSerial;
    view.ownerBaseRevision = 3;
    view.ownerRevision = 4;
    harken::PreparedProjectGenerationV1 withoutVariable;
    ASSERT_EQ(harken::ProjectGenerationPrepareStatusV1::Ok,
        harken::PrepareProjectGenerationV1(*engine, view, withoutVariable).status);
    ASSERT_EQ(harken::ProjectGenerationCommitStatusV1::Ok,
        harken::CommitPreparedProjectGenerationV1(*engine, withoutVariable).status);
    const auto beforeRemovedBlock = engine->mixer.BlocksRendered();
    engine->nullBackend.Pump(1);
    ASSERT_EQ(beforeRemovedBlock + 1, engine->mixer.BlocksRendered());
    EXPECT_NEAR(0.25f * std::pow(10.0f, -16.0f / 20.0f),
        engine->nullBackend.LastBlock()[0], 1e-5f);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    EXPECT_NEAR(0.25f * std::pow(10.0f, -16.0f / 20.0f),
        engine->nullBackend.LastBlock()[0], 1e-5f);
    ASSERT_EQ(VN_OK, Vn_SetBusOutputVolume(VN_BUS_MASTER, 0.0f, 0));
    ASSERT_EQ(VN_OK, Vn_DetachBinding(kBinding, &attach));
    captureFresh(-12.0f);
}

TEST(BusAdmissionStart, PairedCommitInstallsLogicMasterAndChildBuses) {
    AdmissionSession fixture;
    harken::Engine* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    constexpr VnBusId kChild = harkentest::ObjectId(40);
    constexpr VnBusId kOther = harkentest::ObjectId(41);
    std::vector<VnBusId> busIds{kChild, kOther, VN_BUS_MASTER};
    std::vector<std::uint8_t> alive{1, 1, 1};
    std::vector<std::uint32_t> generations{1, 1, 1};
    std::vector<std::uint32_t> parents{2u, 2u, 0u};
    const std::uint32_t sendSlots = engine->config.busSendSlotsPerBus;
    std::vector<std::uint32_t> sendTargets(
        static_cast<std::size_t>(busIds.size()) * sendSlots,
        harken::kEmptySendTarget);
    sendTargets[0] = 2;  // child -> projection Master index 2
    std::vector<float> sendLevels(
        static_cast<std::size_t>(busIds.size()) * sendSlots, 0.0f);
    std::vector<std::uint8_t> sendLevelApply(
        static_cast<std::size_t>(busIds.size()) * sendSlots, 0);
    sendLevels[0] = 0.5f;
    sendLevelApply[0] = 1;
    std::vector<std::uint32_t> feeder(busIds.size(), 0);
    std::vector<std::uint32_t> emittedFeeders(busIds.size(), 0);
    std::vector<std::uint8_t> emitted(busIds.size(), 0);
    harken::ProspectiveTopologyView prospective{};
    prospective.busCount = static_cast<std::uint32_t>(busIds.size());
    prospective.busIds = busIds.data();
    prospective.generations = generations.data();
    prospective.parents = parents.data();
    prospective.alive = alive.data();
    prospective.sendTargets = sendTargets.data();
    prospective.sendLevels = sendLevels.data();
    prospective.sendLevelApply = sendLevelApply.data();
    prospective.feederCount = feeder.data();
    prospective.emittedFeeders = emittedFeeders.data();
    prospective.emitted = emitted.data();

    harken::ProjectGenerationInstallViewV1 view = MakeInstallView(fixture.image);
    view.prospective = &prospective;
    harken::PreparedProjectGenerationV1 prepared;
    ASSERT_EQ(harken::PrepareProjectGenerationV1(*engine, view, prepared).status,
              harken::ProjectGenerationPrepareStatusV1::Ok);
    ASSERT_EQ(harken::CommitPreparedProjectGenerationV1(*engine, prepared).status,
              harken::ProjectGenerationCommitStatusV1::Ok);

    EXPECT_EQ(engine->buses.FindIndex(VN_BUS_MASTER), 0u);
    EXPECT_TRUE(engine->buses.FindRef(VN_BUS_MASTER).Valid());
    EXPECT_EQ(engine->buses.ParentOf(0), 0u);
    const std::uint32_t child = engine->buses.FindIndex(kChild);
    const std::uint32_t other = engine->buses.FindIndex(kOther);
    ASSERT_NE(child, harken::kInvalidBusIndex);
    ASSERT_NE(other, harken::kInvalidBusIndex);
    EXPECT_NE(child, 0u);
    EXPECT_NE(other, 0u);
    EXPECT_NE(child, other);
    EXPECT_TRUE(engine->buses.FindRef(kChild).Valid());
    EXPECT_TRUE(engine->buses.FindRef(kOther).Valid());
    EXPECT_EQ(engine->buses.ParentOf(child), 0u);
    EXPECT_EQ(engine->buses.ParentOf(other), 0u);
    bool sawChildToMaster = false;
    for (std::uint32_t s = 0; s < sendSlots; ++s) {
        const std::uint32_t target =
            engine->busSendAdj[static_cast<std::size_t>(child) * sendSlots + s];
        if (target == 0) {
            sawChildToMaster = true;
        }
        EXPECT_NE(target, 2u);
    }
    EXPECT_TRUE(sawChildToMaster);
    bool childSlotHasPreparedLevel = false;
    bool projectionIndexKeepsLevel = false;
    for (std::uint32_t i = 0; i < engine->topologyPlans.Capacity(); ++i) {
        const auto plan = engine->topologyPlans.View(i);
        if (!plan.valid || plan.preparedSendLevels == nullptr ||
            plan.slotsPerBus != sendSlots) {
            continue;
        }
        for (std::uint32_t s = 0; s < sendSlots; ++s) {
            if (plan.preparedSendLevels[static_cast<std::size_t>(child) *
                                            sendSlots +
                                        s] == 0.5f) {
                childSlotHasPreparedLevel = true;
            }
            if (child != 0 && plan.preparedSendLevels[s] == 0.5f) {
                projectionIndexKeepsLevel = true;
            }
        }
    }
    EXPECT_TRUE(childSlotHasPreparedLevel);
    EXPECT_FALSE(projectionIndexKeepsLevel);
}

TEST(BusAdmissionStart, PrepareRejectsDuplicateAuthoredBusIds) {
    AdmissionSession fixture;
    harken::Engine* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    constexpr VnBusId kChild = harkentest::ObjectId(40);
    std::vector<VnBusId> busIds{VN_BUS_MASTER, kChild, kChild};
    std::vector<std::uint8_t> alive{1, 1, 1};
    std::vector<std::uint32_t> generations{1, 1, 1};
    std::vector<std::uint32_t> parents{0, 0, 0};
    const std::uint32_t sendSlots = engine->config.busSendSlotsPerBus;
    std::vector<std::uint32_t> sendTargets(
        static_cast<std::size_t>(busIds.size()) * sendSlots,
        harken::kEmptySendTarget);
    std::vector<std::uint32_t> feeder(busIds.size(), 0);
    std::vector<std::uint32_t> emittedFeeders(busIds.size(), 0);
    std::vector<std::uint8_t> emitted(busIds.size(), 0);
    harken::ProspectiveTopologyView prospective{};
    prospective.busCount = static_cast<std::uint32_t>(busIds.size());
    prospective.busIds = busIds.data();
    prospective.generations = generations.data();
    prospective.parents = parents.data();
    prospective.alive = alive.data();
    prospective.sendTargets = sendTargets.data();
    prospective.feederCount = feeder.data();
    prospective.emittedFeeders = emittedFeeders.data();
    prospective.emitted = emitted.data();
    harken::ProjectGenerationInstallViewV1 view = MakeInstallView(fixture.image);
    view.prospective = &prospective;
    harken::PreparedProjectGenerationV1 prepared;
    EXPECT_EQ(harken::PrepareProjectGenerationV1(*engine, view, prepared).status,
              harken::ProjectGenerationPrepareStatusV1::Capacity);
    EXPECT_FALSE(prepared.Valid());
}

TEST(BusAdmissionStart, PrepareRejectsOutOfRangeParentIndex) {
    AdmissionSession fixture;
    harken::Engine* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    constexpr VnBusId kChild = harkentest::ObjectId(40);
    std::vector<VnBusId> busIds{VN_BUS_MASTER, kChild};
    std::vector<std::uint8_t> alive{1, 1};
    std::vector<std::uint32_t> generations{1, 1};
    std::vector<std::uint32_t> parents{0, 9};
    const std::uint32_t sendSlots = engine->config.busSendSlotsPerBus;
    std::vector<std::uint32_t> sendTargets(
        static_cast<std::size_t>(busIds.size()) * sendSlots,
        harken::kEmptySendTarget);
    std::vector<std::uint32_t> feeder(busIds.size(), 0);
    std::vector<std::uint32_t> emittedFeeders(busIds.size(), 0);
    std::vector<std::uint8_t> emitted(busIds.size(), 0);
    harken::ProspectiveTopologyView prospective{};
    prospective.busCount = static_cast<std::uint32_t>(busIds.size());
    prospective.busIds = busIds.data();
    prospective.generations = generations.data();
    prospective.parents = parents.data();
    prospective.alive = alive.data();
    prospective.sendTargets = sendTargets.data();
    prospective.feederCount = feeder.data();
    prospective.emittedFeeders = emittedFeeders.data();
    prospective.emitted = emitted.data();
    harken::ProjectGenerationInstallViewV1 view = MakeInstallView(fixture.image);
    view.prospective = &prospective;
    harken::PreparedProjectGenerationV1 prepared;
    EXPECT_EQ(harken::PrepareProjectGenerationV1(*engine, view, prepared).status,
              harken::ProjectGenerationPrepareStatusV1::Capacity);
    EXPECT_FALSE(prepared.Valid());
}

TEST(BusAdmissionStart, PrepareRejectsUnsetNonMasterParent) {
    AdmissionSession fixture;
    harken::Engine* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    constexpr VnBusId kChild = harkentest::ObjectId(40);
    std::vector<VnBusId> busIds{VN_BUS_MASTER, kChild};
    std::vector<std::uint8_t> alive{1, 1};
    std::vector<std::uint32_t> generations{1, 1};
    std::vector<std::uint32_t> parents{0, 0xFFFFFFFFu};
    const std::uint32_t sendSlots = engine->config.busSendSlotsPerBus;
    std::vector<std::uint32_t> sendTargets(
        static_cast<std::size_t>(busIds.size()) * sendSlots,
        harken::kEmptySendTarget);
    std::vector<std::uint32_t> feeder(busIds.size(), 0);
    std::vector<std::uint32_t> emittedFeeders(busIds.size(), 0);
    std::vector<std::uint8_t> emitted(busIds.size(), 0);
    harken::ProspectiveTopologyView prospective{};
    prospective.busCount = static_cast<std::uint32_t>(busIds.size());
    prospective.busIds = busIds.data();
    prospective.generations = generations.data();
    prospective.parents = parents.data();
    prospective.alive = alive.data();
    prospective.sendTargets = sendTargets.data();
    prospective.feederCount = feeder.data();
    prospective.emittedFeeders = emittedFeeders.data();
    prospective.emitted = emitted.data();
    harken::ProjectGenerationInstallViewV1 view = MakeInstallView(fixture.image);
    view.prospective = &prospective;
    harken::PreparedProjectGenerationV1 prepared;
    EXPECT_EQ(harken::PrepareProjectGenerationV1(*engine, view, prepared).status,
              harken::ProjectGenerationPrepareStatusV1::Capacity);
    EXPECT_FALSE(prepared.Valid());
}

TEST(BusAdmissionStart, PrepareRejectsParentCycle) {
    AdmissionSession fixture;
    harken::Engine* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    constexpr VnBusId kChildA = harkentest::ObjectId(40);
    constexpr VnBusId kChildB = harkentest::ObjectId(41);
    std::vector<VnBusId> busIds{VN_BUS_MASTER, kChildA, kChildB};
    std::vector<std::uint8_t> alive{1, 1, 1};
    std::vector<std::uint32_t> generations{1, 1, 1};
    std::vector<std::uint32_t> parents{0, 2, 1};
    const std::uint32_t sendSlots = engine->config.busSendSlotsPerBus;
    std::vector<std::uint32_t> sendTargets(
        static_cast<std::size_t>(busIds.size()) * sendSlots,
        harken::kEmptySendTarget);
    std::vector<std::uint32_t> feeder(busIds.size(), 0);
    std::vector<std::uint32_t> emittedFeeders(busIds.size(), 0);
    std::vector<std::uint8_t> emitted(busIds.size(), 0);
    harken::ProspectiveTopologyView prospective{};
    prospective.busCount = static_cast<std::uint32_t>(busIds.size());
    prospective.busIds = busIds.data();
    prospective.generations = generations.data();
    prospective.parents = parents.data();
    prospective.alive = alive.data();
    prospective.sendTargets = sendTargets.data();
    prospective.feederCount = feeder.data();
    prospective.emittedFeeders = emittedFeeders.data();
    prospective.emitted = emitted.data();
    harken::ProjectGenerationInstallViewV1 view = MakeInstallView(fixture.image);
    view.prospective = &prospective;
    harken::PreparedProjectGenerationV1 prepared;
    EXPECT_EQ(harken::PrepareProjectGenerationV1(*engine, view, prepared).status,
              harken::ProjectGenerationPrepareStatusV1::Capacity);
    EXPECT_FALSE(prepared.Valid());
    harken::PreparedProjectGenerationV1 reuse;
    std::vector<std::uint32_t> acyclicParents{0, 0, 0};
    prospective.parents = acyclicParents.data();
    EXPECT_EQ(harken::PrepareProjectGenerationV1(*engine, view, reuse).status,
              harken::ProjectGenerationPrepareStatusV1::Ok);
    EXPECT_TRUE(reuse.Valid());
    harken::CancelPreparedProjectGenerationV1(*engine, std::move(reuse));
}

TEST(BusAdmissionStart, PrepareRejectsSendCycle) {
    AdmissionSession fixture;
    harken::Engine* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    constexpr VnBusId kChildA = harkentest::ObjectId(40);
    constexpr VnBusId kChildB = harkentest::ObjectId(41);
    std::vector<VnBusId> busIds{VN_BUS_MASTER, kChildA, kChildB};
    std::vector<std::uint8_t> alive{1, 1, 1};
    std::vector<std::uint32_t> generations{1, 1, 1};
    std::vector<std::uint32_t> parents{0, 0, 0};
    const std::uint32_t sendSlots = engine->config.busSendSlotsPerBus;
    std::vector<std::uint32_t> sendTargets(
        static_cast<std::size_t>(busIds.size()) * sendSlots,
        harken::kEmptySendTarget);
    sendTargets[static_cast<std::size_t>(1) * sendSlots] = 2;
    sendTargets[static_cast<std::size_t>(2) * sendSlots] = 1;
    std::vector<std::uint32_t> feeder(busIds.size(), 0);
    std::vector<std::uint32_t> emittedFeeders(busIds.size(), 0);
    std::vector<std::uint8_t> emitted(busIds.size(), 0);
    harken::ProspectiveTopologyView prospective{};
    prospective.busCount = static_cast<std::uint32_t>(busIds.size());
    prospective.busIds = busIds.data();
    prospective.generations = generations.data();
    prospective.parents = parents.data();
    prospective.alive = alive.data();
    prospective.sendTargets = sendTargets.data();
    prospective.feederCount = feeder.data();
    prospective.emittedFeeders = emittedFeeders.data();
    prospective.emitted = emitted.data();
    harken::ProjectGenerationInstallViewV1 view = MakeInstallView(fixture.image);
    view.prospective = &prospective;
    harken::PreparedProjectGenerationV1 prepared;
    EXPECT_EQ(harken::PrepareProjectGenerationV1(*engine, view, prepared).status,
              harken::ProjectGenerationPrepareStatusV1::Capacity);
    EXPECT_FALSE(prepared.Valid());
}

TEST(BusAdmissionStart, PairedCommitAcceptsMasterOnlyProspective) {
    AdmissionSession fixture;
    harken::Engine* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    std::vector<VnBusId> busIds{VN_BUS_MASTER};
    std::vector<std::uint8_t> alive{1};
    std::vector<std::uint32_t> generations{1};
    std::vector<std::uint32_t> parents{0};
    const std::uint32_t sendSlots = engine->config.busSendSlotsPerBus;
    std::vector<std::uint32_t> sendTargets(
        static_cast<std::size_t>(busIds.size()) * sendSlots,
        harken::kEmptySendTarget);
    std::vector<std::uint32_t> feeder(busIds.size(), 0);
    std::vector<std::uint32_t> emittedFeeders(busIds.size(), 0);
    std::vector<std::uint8_t> emitted(busIds.size(), 0);
    harken::ProspectiveTopologyView prospective{};
    prospective.busCount = static_cast<std::uint32_t>(busIds.size());
    prospective.busIds = busIds.data();
    prospective.generations = generations.data();
    prospective.parents = parents.data();
    prospective.alive = alive.data();
    prospective.sendTargets = sendTargets.data();
    prospective.feederCount = feeder.data();
    prospective.emittedFeeders = emittedFeeders.data();
    prospective.emitted = emitted.data();
    harken::ProjectGenerationInstallViewV1 view = MakeInstallView(fixture.image);
    view.prospective = &prospective;
    harken::PreparedProjectGenerationV1 prepared;
    ASSERT_EQ(harken::PrepareProjectGenerationV1(*engine, view, prepared).status,
              harken::ProjectGenerationPrepareStatusV1::Ok);
    ASSERT_EQ(harken::CommitPreparedProjectGenerationV1(*engine, prepared).status,
              harken::ProjectGenerationCommitStatusV1::Ok);
    EXPECT_EQ(engine->buses.FindIndex(VN_BUS_MASTER), 0u);
    EXPECT_EQ(engine->buses.ParentOf(0), 0u);
}

TEST(BusAdmissionStart, PairedCommitReparentsRetainedChildUnderNewParentSlot) {
    AdmissionSession fixture;
    harken::Engine* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    constexpr VnBusId kChild = harkentest::ObjectId(40);
    constexpr VnBusId kParent = harkentest::ObjectId(41);
    std::vector<VnBusId> firstIds{VN_BUS_MASTER, kChild};
    std::vector<std::uint8_t> firstAlive{1, 1};
    std::vector<std::uint32_t> firstGenerations{1, 1};
    std::vector<std::uint32_t> firstParents{0, 0};
    const std::uint32_t sendSlots = engine->config.busSendSlotsPerBus;
    std::vector<std::uint32_t> firstSends(
        static_cast<std::size_t>(firstIds.size()) * sendSlots,
        harken::kEmptySendTarget);
    std::vector<std::uint32_t> firstFeeder(firstIds.size(), 0);
    std::vector<std::uint32_t> firstEmittedFeeders(firstIds.size(), 0);
    std::vector<std::uint8_t> firstEmitted(firstIds.size(), 0);
    harken::ProspectiveTopologyView first{};
    first.busCount = static_cast<std::uint32_t>(firstIds.size());
    first.busIds = firstIds.data();
    first.generations = firstGenerations.data();
    first.parents = firstParents.data();
    first.alive = firstAlive.data();
    first.sendTargets = firstSends.data();
    first.feederCount = firstFeeder.data();
    first.emittedFeeders = firstEmittedFeeders.data();
    first.emitted = firstEmitted.data();
    harken::ProjectGenerationInstallViewV1 firstView = MakeInstallView(fixture.image);
    firstView.prospective = &first;
    harken::PreparedProjectGenerationV1 firstPrepared;
    ASSERT_EQ(harken::PrepareProjectGenerationV1(*engine, firstView, firstPrepared)
                  .status,
              harken::ProjectGenerationPrepareStatusV1::Ok);
    ASSERT_EQ(harken::CommitPreparedProjectGenerationV1(*engine, firstPrepared)
                  .status,
              harken::ProjectGenerationCommitStatusV1::Ok);
    const std::uint32_t childSlot = engine->buses.FindIndex(kChild);
    ASSERT_NE(childSlot, harken::kInvalidBusIndex);
    EXPECT_EQ(engine->buses.ParentOf(childSlot), 0u);

    std::vector<VnBusId> secondIds{VN_BUS_MASTER, kChild, kParent};
    std::vector<std::uint8_t> secondAlive{1, 1, 1};
    std::vector<std::uint32_t> secondGenerations{1, 1, 1};
    std::vector<std::uint32_t> secondParents{0, 2, 0};
    std::vector<std::uint32_t> secondSends(
        static_cast<std::size_t>(secondIds.size()) * sendSlots,
        harken::kEmptySendTarget);
    std::vector<std::uint32_t> secondFeeder(secondIds.size(), 0);
    std::vector<std::uint32_t> secondEmittedFeeders(secondIds.size(), 0);
    std::vector<std::uint8_t> secondEmitted(secondIds.size(), 0);
    harken::ProspectiveTopologyView second{};
    second.busCount = static_cast<std::uint32_t>(secondIds.size());
    second.busIds = secondIds.data();
    second.generations = secondGenerations.data();
    second.parents = secondParents.data();
    second.alive = secondAlive.data();
    second.sendTargets = secondSends.data();
    second.feederCount = secondFeeder.data();
    second.emittedFeeders = secondEmittedFeeders.data();
    second.emitted = secondEmitted.data();
    harken::ProjectGenerationInstallViewV1 secondView = MakeInstallView(fixture.image);
    secondView.token.submissionSerial = 5;
    secondView.ownerBaseRevision = LiveOwnerRevision(*engine, secondView.ownerId);
    secondView.ownerRevision = secondView.ownerBaseRevision + 1u;
    secondView.prospective = &second;
    harken::PreparedProjectGenerationV1 secondPrepared;
    ASSERT_EQ(harken::PrepareProjectGenerationV1(*engine, secondView, secondPrepared)
                  .status,
              harken::ProjectGenerationPrepareStatusV1::Ok);
    ASSERT_EQ(harken::CommitPreparedProjectGenerationV1(*engine, secondPrepared)
                  .status,
              harken::ProjectGenerationCommitStatusV1::Ok);
    EXPECT_EQ(engine->buses.FindIndex(kChild), childSlot);
    const std::uint32_t parentSlot = engine->buses.FindIndex(kParent);
    ASSERT_NE(parentSlot, harken::kInvalidBusIndex);
    EXPECT_NE(parentSlot, childSlot);
    EXPECT_EQ(engine->buses.ParentOf(childSlot), parentSlot);
}

TEST(BusAdmissionStart, PairedCommitReusesDistinctDeadSlotsForNewBuses) {
    AdmissionSession fixture;
    harken::Engine* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    ASSERT_EQ(Vn_CreateBus(harkentest::ObjectId(40), VN_BUS_MASTER), VN_OK);
    ASSERT_EQ(Vn_CreateBus(harkentest::ObjectId(41), VN_BUS_MASTER), VN_OK);
    engine->nullBackend.Pump(2);
    ASSERT_EQ(Vn_DestroyBus(harkentest::ObjectId(40)), VN_OK);
    ASSERT_EQ(Vn_DestroyBus(harkentest::ObjectId(41)), VN_OK);
    engine->nullBackend.Pump(2);
    constexpr VnBusId kA = harkentest::ObjectId(50);
    constexpr VnBusId kB = harkentest::ObjectId(51);
    constexpr VnBusId kC = harkentest::ObjectId(52);
    std::vector<VnBusId> busIds{VN_BUS_MASTER, kA, kB, kC};
    std::vector<std::uint8_t> alive{1, 1, 1, 1};
    std::vector<std::uint32_t> generations{1, 1, 1, 1};
    std::vector<std::uint32_t> parents{0, 0, 0, 0};
    const std::uint32_t sendSlots = engine->config.busSendSlotsPerBus;
    std::vector<std::uint32_t> sendTargets(
        static_cast<std::size_t>(busIds.size()) * sendSlots,
        harken::kEmptySendTarget);
    std::vector<std::uint32_t> feeder(busIds.size(), 0);
    std::vector<std::uint32_t> emittedFeeders(busIds.size(), 0);
    std::vector<std::uint8_t> emitted(busIds.size(), 0);
    harken::ProspectiveTopologyView prospective{};
    prospective.busCount = static_cast<std::uint32_t>(busIds.size());
    prospective.busIds = busIds.data();
    prospective.generations = generations.data();
    prospective.parents = parents.data();
    prospective.alive = alive.data();
    prospective.sendTargets = sendTargets.data();
    prospective.feederCount = feeder.data();
    prospective.emittedFeeders = emittedFeeders.data();
    prospective.emitted = emitted.data();
    harken::ProjectGenerationInstallViewV1 view = MakeInstallView(fixture.image);
    view.prospective = &prospective;
    harken::PreparedProjectGenerationV1 prepared;
    ASSERT_EQ(harken::PrepareProjectGenerationV1(*engine, view, prepared).status,
              harken::ProjectGenerationPrepareStatusV1::Ok);
    ASSERT_EQ(harken::CommitPreparedProjectGenerationV1(*engine, prepared).status,
              harken::ProjectGenerationCommitStatusV1::Ok);
    const std::uint32_t a = engine->buses.FindIndex(kA);
    const std::uint32_t b = engine->buses.FindIndex(kB);
    const std::uint32_t c = engine->buses.FindIndex(kC);
    ASSERT_NE(a, harken::kInvalidBusIndex);
    ASSERT_NE(b, harken::kInvalidBusIndex);
    ASSERT_NE(c, harken::kInvalidBusIndex);
    EXPECT_EQ(engine->buses.FindIndex(VN_BUS_MASTER), 0u);
    EXPECT_NE(a, b);
    EXPECT_NE(a, c);
    EXPECT_NE(b, c);
}

TEST(BusAdmissionStart, PreviewRemovalRejectsCallerBindingOnDeadSlot) {
    AdmissionSession fixture;
    harken::Engine* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    constexpr VnBusId kChild = harkentest::ObjectId(40);
    constexpr VnVariableId kVolumeVariable = harkentest::ObjectId(730);
    constexpr VnBindingId kVolumeBinding = harkentest::ObjectId(731);
    const harken::VariableDefinitionV1 definition =
        harkentest::MakeVariableDefinition(kVolumeVariable,
                                         harken::VariableKindV1::Smooth,
                                         0.0f, 1.0f, 0.0f);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallDefinitionsWorld(engine, &definition, 1).status);
    std::vector<VnBusId> firstIds{VN_BUS_MASTER, kChild};
    std::vector<std::uint8_t> firstAlive{1, 1};
    std::vector<std::uint32_t> firstGenerations{1, 1};
    std::vector<std::uint32_t> firstParents{0, 0};
    const std::uint32_t sendSlots = engine->config.busSendSlotsPerBus;
    std::vector<std::uint32_t> firstSends(
        static_cast<std::size_t>(firstIds.size()) * sendSlots,
        harken::kEmptySendTarget);
    std::vector<std::uint32_t> firstFeeder(firstIds.size(), 0);
    std::vector<std::uint32_t> firstEmittedFeeders(firstIds.size(), 0);
    std::vector<std::uint8_t> firstEmitted(firstIds.size(), 0);
    harken::ProspectiveTopologyView first{};
    first.busCount = static_cast<std::uint32_t>(firstIds.size());
    first.busIds = firstIds.data();
    first.generations = firstGenerations.data();
    first.parents = firstParents.data();
    first.alive = firstAlive.data();
    first.sendTargets = firstSends.data();
    first.feederCount = firstFeeder.data();
    first.emittedFeeders = firstEmittedFeeders.data();
    first.emitted = firstEmitted.data();
    harken::ProjectGenerationInstallViewV1 firstView = MakeInstallView(fixture.image);
    firstView.prospective = &first;
    harken::PreparedProjectGenerationV1 firstPrepared;
    ASSERT_EQ(harken::PrepareProjectGenerationV1(*engine, firstView, firstPrepared).status,
              harken::ProjectGenerationPrepareStatusV1::Ok);
    ASSERT_EQ(harken::CommitPreparedProjectGenerationV1(*engine, firstPrepared).status,
              harken::ProjectGenerationCommitStatusV1::Ok);
    const std::uint32_t childSlot = engine->buses.FindIndex(kChild);
    ASSERT_NE(childSlot, harken::kInvalidBusIndex);
    VnBindingDesc binding{};
    binding.structSize = sizeof(VnBindingDesc);
    binding.variableId = kVolumeVariable;
    binding.targetScopeKind = VN_TARGET_SCOPE_BUS;
    binding.targetId = harken::kBusTargetBusVolume;
    binding.curve.structSize = sizeof(VnCurveDesc);
    binding.curve.pointCount = 2;
    binding.curve.points[0] = {0.0f, 0.0f};
    binding.curve.points[1] = {1.0f, -6.0f};
    binding.curve.handles[0] = {0.0f, 0.0f, 1.0f, -6.0f};
    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(VnBindingAttachDesc);
    attach.scopeKind = VN_TARGET_SCOPE_BUS;
    attach.objectId = kChild;
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(kVolumeBinding, &binding));
    ASSERT_EQ(VN_OK, Vn_AttachBinding(kVolumeBinding, &attach));
    ASSERT_TRUE(engine->bindings.HasAttachment(
        kVolumeBinding, VN_TARGET_SCOPE_BUS, kChild));
    EXPECT_FALSE(harken::CanResetNeutralBusPropertiesV1(*engine, childSlot));

    std::vector<VnBusId> secondIds{VN_BUS_MASTER};
    std::vector<std::uint8_t> secondAlive{1};
    std::vector<std::uint32_t> secondGenerations{1};
    std::vector<std::uint32_t> secondParents{0};
    std::vector<std::uint32_t> secondSends(sendSlots, harken::kEmptySendTarget);
    std::vector<std::uint32_t> secondFeeder(1, 0);
    std::vector<std::uint32_t> secondEmittedFeeders(1, 0);
    std::vector<std::uint8_t> secondEmitted(1, 0);
    harken::ProspectiveTopologyView second{};
    second.busCount = 1;
    second.busIds = secondIds.data();
    second.generations = secondGenerations.data();
    second.parents = secondParents.data();
    second.alive = secondAlive.data();
    second.sendTargets = secondSends.data();
    second.feederCount = secondFeeder.data();
    second.emittedFeeders = secondEmittedFeeders.data();
    second.emitted = secondEmitted.data();
    harken::ProjectGenerationInstallViewV1 secondView = MakeInstallView(fixture.image);
    secondView.token.submissionSerial = 5;
    secondView.ownerBaseRevision = LiveOwnerRevision(*engine, secondView.ownerId);
    secondView.ownerRevision = secondView.ownerBaseRevision + 1u;
    secondView.prospective = &second;
    harken::PreparedProjectGenerationV1 secondPrepared;
    EXPECT_EQ(harken::PrepareProjectGenerationV1(*engine, secondView, secondPrepared).status,
              harken::ProjectGenerationPrepareStatusV1::Backpressure);
    EXPECT_FALSE(secondPrepared.Valid());
    EXPECT_TRUE(engine->bindings.HasAttachment(
        kVolumeBinding, VN_TARGET_SCOPE_BUS, kChild));
    EXPECT_EQ(engine->buses.FindIndex(kChild), childSlot);
}

TEST(BusAdmissionStart, PreviewRemovalMigratesDeadSlotsToRetainedAncestor) {
    AdmissionSession fixture;
    harken::Engine* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    constexpr VnBusId kChild = harkentest::ObjectId(40);
    constexpr VnBusId kGrand = harkentest::ObjectId(41);
    std::vector<VnBusId> firstIds{VN_BUS_MASTER, kChild, kGrand};
    std::vector<std::uint8_t> firstAlive{1, 1, 1};
    std::vector<std::uint32_t> firstGenerations{1, 1, 1};
    std::vector<std::uint32_t> firstParents{0, 0, 1};
    const std::uint32_t sendSlots = engine->config.busSendSlotsPerBus;
    std::vector<std::uint32_t> firstSends(
        static_cast<std::size_t>(firstIds.size()) * sendSlots,
        harken::kEmptySendTarget);
    std::vector<std::uint32_t> firstFeeder(firstIds.size(), 0);
    std::vector<std::uint32_t> firstEmittedFeeders(firstIds.size(), 0);
    std::vector<std::uint8_t> firstEmitted(firstIds.size(), 0);
    harken::ProspectiveTopologyView first{};
    first.busCount = static_cast<std::uint32_t>(firstIds.size());
    first.busIds = firstIds.data();
    first.generations = firstGenerations.data();
    first.parents = firstParents.data();
    first.alive = firstAlive.data();
    first.sendTargets = firstSends.data();
    first.feederCount = firstFeeder.data();
    first.emittedFeeders = firstEmittedFeeders.data();
    first.emitted = firstEmitted.data();
    harken::ProjectGenerationInstallViewV1 firstView = MakeInstallView(fixture.image);
    firstView.prospective = &first;
    harken::PreparedProjectGenerationV1 firstPrepared;
    ASSERT_EQ(harken::PrepareProjectGenerationV1(*engine, firstView, firstPrepared).status,
              harken::ProjectGenerationPrepareStatusV1::Ok);
    ASSERT_EQ(harken::CommitPreparedProjectGenerationV1(*engine, firstPrepared).status,
              harken::ProjectGenerationCommitStatusV1::Ok);
    const std::uint32_t childSlot = engine->buses.FindIndex(kChild);
    const std::uint32_t grandSlot = engine->buses.FindIndex(kGrand);
    ASSERT_NE(childSlot, harken::kInvalidBusIndex);
    ASSERT_NE(grandSlot, harken::kInvalidBusIndex);
    EXPECT_EQ(engine->buses.ParentOf(grandSlot), childSlot);
    EXPECT_EQ(engine->buses.ParentOf(childSlot), 0u);

    std::vector<VnBusId> secondIds{VN_BUS_MASTER};
    std::vector<std::uint8_t> secondAlive{1};
    std::vector<std::uint32_t> secondGenerations{1};
    std::vector<std::uint32_t> secondParents{0};
    std::vector<std::uint32_t> secondSends(sendSlots, harken::kEmptySendTarget);
    std::vector<std::uint32_t> secondFeeder(1, 0);
    std::vector<std::uint32_t> secondEmittedFeeders(1, 0);
    std::vector<std::uint8_t> secondEmitted(1, 0);
    harken::ProspectiveTopologyView second{};
    second.busCount = 1;
    second.busIds = secondIds.data();
    second.generations = secondGenerations.data();
    second.parents = secondParents.data();
    second.alive = secondAlive.data();
    second.sendTargets = secondSends.data();
    second.feederCount = secondFeeder.data();
    second.emittedFeeders = secondEmittedFeeders.data();
    second.emitted = secondEmitted.data();
    harken::ProjectGenerationInstallViewV1 secondView = MakeInstallView(fixture.image);
    secondView.token.submissionSerial = 5;
    secondView.ownerBaseRevision = LiveOwnerRevision(*engine, secondView.ownerId);
    secondView.ownerRevision = secondView.ownerBaseRevision + 1u;
    secondView.prospective = &second;
    harken::PreparedProjectGenerationV1 secondPrepared;
    ASSERT_EQ(harken::PrepareProjectGenerationV1(*engine, secondView, secondPrepared).status,
              harken::ProjectGenerationPrepareStatusV1::Ok);
    const auto plan = engine->preparedTopologyPlans.View(secondPrepared.PreparedPlanIndex());
    ASSERT_TRUE(plan.valid);
    ASSERT_NE(plan.alive, nullptr);
    ASSERT_NE(plan.migrationTargets, nullptr);
    ASSERT_GT(plan.slotCount, grandSlot);
    EXPECT_EQ(0u, plan.alive[childSlot]);
    EXPECT_EQ(0u, plan.alive[grandSlot]);
    EXPECT_NE(0u, plan.alive[0]);
    EXPECT_EQ(0u, plan.migrationTargets[childSlot]);
    EXPECT_EQ(0u, plan.migrationTargets[grandSlot]);
    EXPECT_EQ(0u, plan.migrationTargets[0]);
    harken::CancelPreparedProjectGenerationV1(*engine, std::move(secondPrepared));
}

TEST(BusAdmissionStart, PreviewCommitMigratesPublishedIdentityAndFactsOffDeadSlots) {
    AdmissionSession fixture;
    harken::Engine* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    constexpr VnBusId kChild = harkentest::ObjectId(40);
    constexpr VnBusId kGrand = harkentest::ObjectId(41);
    std::vector<VnBusId> firstIds{VN_BUS_MASTER, kChild, kGrand};
    std::vector<std::uint8_t> firstAlive{1, 1, 1};
    std::vector<std::uint32_t> firstGenerations{1, 1, 1};
    std::vector<std::uint32_t> firstParents{0, 0, 1};
    const std::uint32_t sendSlots = engine->config.busSendSlotsPerBus;
    std::vector<std::uint32_t> firstSends(
        static_cast<std::size_t>(firstIds.size()) * sendSlots,
        harken::kEmptySendTarget);
    std::vector<std::uint32_t> firstFeeder(firstIds.size(), 0);
    std::vector<std::uint32_t> firstEmittedFeeders(firstIds.size(), 0);
    std::vector<std::uint8_t> firstEmitted(firstIds.size(), 0);
    harken::ProspectiveTopologyView first{};
    first.busCount = static_cast<std::uint32_t>(firstIds.size());
    first.busIds = firstIds.data();
    first.generations = firstGenerations.data();
    first.parents = firstParents.data();
    first.alive = firstAlive.data();
    first.sendTargets = firstSends.data();
    first.feederCount = firstFeeder.data();
    first.emittedFeeders = firstEmittedFeeders.data();
    first.emitted = firstEmitted.data();
    harken::ProjectGenerationInstallViewV1 firstView = MakeInstallView(fixture.image);
    firstView.prospective = &first;
    harken::PreparedProjectGenerationV1 firstPrepared;
    ASSERT_EQ(harken::PrepareProjectGenerationV1(*engine, firstView, firstPrepared).status,
              harken::ProjectGenerationPrepareStatusV1::Ok);
    ASSERT_EQ(harken::CommitPreparedProjectGenerationV1(*engine, firstPrepared).status,
              harken::ProjectGenerationCommitStatusV1::Ok);
    const std::uint32_t childSlot = engine->buses.FindIndex(kChild);
    const std::uint32_t grandSlot = engine->buses.FindIndex(kGrand);
    ASSERT_NE(childSlot, harken::kInvalidBusIndex);
    ASSERT_NE(grandSlot, harken::kInvalidBusIndex);
    const auto childGeneration = engine->buses.GenerationAt(childSlot);
    const auto grandGeneration = engine->buses.GenerationAt(grandSlot);
    ASSERT_TRUE(engine->voices.PrepareSlotWorkingSet(0, 1));
    ASSERT_NE(0u, engine->voicePlayFacts.size());
    ASSERT_TRUE(engine->pendingStarts.Emplace(0));
    ASSERT_TRUE(engine->parkRecords.Emplace(0));
    ASSERT_NE(0u, engine->timelineVoiceInstallIdentities.size());
    auto& facts = engine->voicePlayFacts[0];
    facts.valid = true;
    facts.busIndex = grandSlot;
    facts.busGeneration = grandGeneration;
    auto& pending = engine->pendingStarts[0];
    pending.active = true;
    pending.busIndex = grandSlot;
    pending.busGeneration = grandGeneration;
    pending.voiceSends[0] = {true, childSlot, -5.0f, childGeneration};
    auto& parked = engine->parkRecords[0];
    parked.active = true;
    parked.facts.busIndex = grandSlot;
    parked.facts.busGeneration = grandGeneration;
    parked.voiceSends[0] = {true, childSlot, -7.0f, childGeneration};
    auto& identity = engine->timelineVoiceInstallIdentities[0];
    identity.renderHandle = 1;
    identity.busIndex = grandSlot;
    identity.busGeneration = grandGeneration;
    identity.voiceSends[0] = {true, childSlot, -9.0f, childGeneration};

    std::vector<VnBusId> secondIds{VN_BUS_MASTER};
    std::vector<std::uint8_t> secondAlive{1};
    std::vector<std::uint32_t> secondGenerations{1};
    std::vector<std::uint32_t> secondParents{0};
    std::vector<std::uint32_t> secondSends(sendSlots, harken::kEmptySendTarget);
    std::vector<std::uint32_t> secondFeeder(1, 0);
    std::vector<std::uint32_t> secondEmittedFeeders(1, 0);
    std::vector<std::uint8_t> secondEmitted(1, 0);
    harken::ProspectiveTopologyView second{};
    second.busCount = 1;
    second.busIds = secondIds.data();
    second.generations = secondGenerations.data();
    second.parents = secondParents.data();
    second.alive = secondAlive.data();
    second.sendTargets = secondSends.data();
    second.feederCount = secondFeeder.data();
    second.emittedFeeders = secondEmittedFeeders.data();
    second.emitted = secondEmitted.data();
    harken::ProjectGenerationInstallViewV1 secondView = MakeInstallView(fixture.image);
    secondView.token.submissionSerial = 6;
    secondView.ownerBaseRevision = LiveOwnerRevision(*engine, secondView.ownerId);
    secondView.ownerRevision = secondView.ownerBaseRevision + 1u;
    secondView.prospective = &second;
    harken::PreparedProjectGenerationV1 secondPrepared;
    ASSERT_EQ(harken::PrepareProjectGenerationV1(*engine, secondView, secondPrepared).status,
              harken::ProjectGenerationPrepareStatusV1::Ok);
    ASSERT_EQ(harken::CommitPreparedProjectGenerationV1(*engine, secondPrepared).status,
              harken::ProjectGenerationCommitStatusV1::Ok);
    EXPECT_EQ(0u, facts.busIndex);
    EXPECT_EQ(engine->buses.GenerationAt(0), facts.busGeneration);
    EXPECT_EQ(0u, pending.busIndex);
    EXPECT_FALSE(pending.voiceSends[0].active);
    EXPECT_EQ(0u, parked.facts.busIndex);
    EXPECT_FALSE(parked.voiceSends[0].active);
    EXPECT_EQ(0u, identity.busIndex);
    EXPECT_EQ(engine->buses.GenerationAt(0), identity.busGeneration);
    EXPECT_FALSE(identity.voiceSends[0].active);
    facts = {};
    pending = {};
    parked = {};
    identity = {};
}

struct LiveBusPublication {
    std::array<VnBusId, 1> ids{VN_BUS_MASTER};
    std::array<std::uint8_t, 1> alive{1}, emitted{};
    std::array<std::uint32_t, 1> generations{}, parents{}, feeders{}, emittedFeeders{};
    std::array<float, 1> volume{}, neutral{};
    std::vector<std::uint32_t> sends;
    std::vector<float> levels;
    std::vector<std::uint8_t> levelApply;
    harken::ProspectiveTopologyView topology{};
    harken::ProjectGenerationInstallViewV1 view{};

    LiveBusPublication(harken::Engine& engine, const AdmissionWorldImage& image,
                       std::uint32_t vintage)
        : sends(engine.config.busSendSlotsPerBus, harken::kEmptySendTarget),
          levels(sends.size()), levelApply(sends.size()), view(MakeInstallView(image)) {
        generations[0] = engine.buses.GenerationAt(0);
        topology.busCount = 1;
        topology.busIds = ids.data();
        topology.generations = generations.data();
        topology.parents = parents.data();
        topology.alive = alive.data();
        topology.sendTargets = sends.data();
        topology.sendLevels = levels.data();
        topology.sendLevelApply = levelApply.data();
        topology.feederCount = feeders.data();
        topology.emittedFeeders = emittedFeeders.data();
        topology.emitted = emitted.data();
        view.publicationVintage = vintage;
        view.prospective = &topology;
        view.authoredBusIds = ids.data();
        view.authoredBusVolumeDb = volume.data();
        view.authoredBusMakeUpGainDb = neutral.data();
        view.authoredBusOutputVolumeDb = neutral.data();
        view.authoredBusCount = 1;
    }

    void Next(float db) {
        view.ownerBaseRevision = view.ownerRevision;
        ++view.ownerRevision;
        ++view.token.authoringRevision;
        ++view.token.submissionSerial;
        volume[0] = db;
    }
};

AdmissionWorldImage ChangedAdmissionWorld(const AdmissionWorldImage& prior) {
    auto changed = prior;
    changed.action.actionStableId = harkentest::ObjectId(92);
    changed.action.programSemanticFingerprint =
        harken::ComputeActionProgramFingerprintV1(changed.action);
    changed.BindWorld();
    changed.world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV5(changed.world);
    return changed;
}

void CommitAndConsumeLiveBus(harken::Engine& engine, LiveBusPublication& publication) {
    harken::PreparedProjectGenerationV1 prepared;
    ASSERT_EQ(harken::ProjectGenerationPrepareStatusV1::Ok,
        harken::PrepareProjectGenerationV1(engine, publication.view, prepared).status);
    ASSERT_EQ(harken::ProjectGenerationCommitStatusV1::Ok,
        harken::CommitPreparedProjectGenerationV1(engine, prepared).status);
    const auto block = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    ASSERT_GT(engine.mixer.BlocksRendered(), block);
    ASSERT_EQ(VN_OK, Vn_Update());
}

void StartLiveBusConsumer(harken::Engine& engine, const AdmissionWorldImage& image,
                          float expected = 0.25f) {
    harken::PlayableStartRequest request{};
    request.root = image.Root();
    const auto started = harken::StartPlayable(engine, request);
    ASSERT_EQ(harken::PlayableStartStatus::Accepted, started.status);
    ASSERT_NE(0u, started.instance);
    ASSERT_TRUE(harken::HasCurrentWorldPublicationConsumers(engine));
    bool heard = false;
    for (unsigned attempt = 0; attempt < 16 && !heard; ++attempt) {
        ASSERT_EQ(VN_OK, Vn_Update());
        const auto block = engine.mixer.BlocksRendered();
        engine.nullBackend.Pump(1);
        ASSERT_GT(engine.mixer.BlocksRendered(), block);
        heard = std::abs(engine.nullBackend.LastBlock()[0]) > 0.001f;
    }
    ASSERT_TRUE(heard);
    EXPECT_NEAR(expected, engine.nullBackend.LastBlock()[0], 1e-5f);
}

TEST(BusAdmissionStart, LiveBusV3AndV4RetainActualConsumerAndValidatedWorld) {
    for (const auto vintage : {3u, 4u}) {
        SCOPED_TRACE(vintage);
        AdmissionSession fixture;
        auto& engine = *fixture.session.Engine();
        LiveBusPublication publication(engine, fixture.image, vintage);
        CommitAndConsumeLiveBus(engine, publication);
        ASSERT_FALSE(HasFatalFailure());
        StartLiveBusConsumer(engine, fixture.image);
        ASSERT_FALSE(HasFatalFailure());
        const auto generation = engine.variables.CurrentWorldGeneration();
        const auto worldFingerprint = engine.variables.CurrentWorldInstallView()
            .identity.runtimeSemanticFingerprint;
        publication.Next(-6.0f);
        CommitAndConsumeLiveBus(engine, publication);
        ASSERT_FALSE(HasFatalFailure());
        EXPECT_EQ(generation, engine.variables.CurrentWorldGeneration());
        EXPECT_EQ(worldFingerprint, engine.variables.CurrentWorldInstallView()
            .identity.runtimeSemanticFingerprint);
        EXPECT_TRUE(harken::HasCurrentWorldPublicationConsumers(engine));
        const float expected = 0.25f * std::pow(10.0f, -6.0f / 20.0f);
        for (std::uint32_t i = 0; i < engine.config.blockFrames * 2; ++i)
            EXPECT_NEAR(expected, engine.nullBackend.LastBlock()[i], 1e-5f) << i;
        EXPECT_EQ(2u, LiveOwnerRevision(engine, publication.view.ownerId));
    }
}

TEST(BusAdmissionStart, LiveConsumerStillRejectsChangedWorldAndLegacyButValidatesInvalidWorld) {
    for (const auto vintage : {1u, 3u, 4u}) {
        SCOPED_TRACE(vintage);
        AdmissionSession fixture;
        auto& engine = *fixture.session.Engine();
        StartLiveBusConsumer(engine, fixture.image);
        ASSERT_FALSE(HasFatalFailure());
        LiveBusPublication publication(engine, fixture.image, vintage);
        if (vintage <= 2) {
            publication.view.prospective = nullptr;
            publication.view.authoredBusCount = 0;
        }
        auto changed = ChangedAdmissionWorld(fixture.image);
        publication.view.compiledWorld = changed.world;
        const auto head = engine.commands.PublishedHead();
        const auto livePlans = engine.topologyPlans.InUse();
        harken::PreparedProjectGenerationV1 prepared;
        EXPECT_EQ(harken::ProjectGenerationPrepareStatusV1::Backpressure,
            harken::PrepareProjectGenerationV1(engine, publication.view, prepared).status);
        EXPECT_FALSE(prepared.Valid());
        EXPECT_EQ(0u, engine.preparedTopologyPlans.InUse());
        EXPECT_EQ(head, engine.commands.PublishedHead());
        EXPECT_EQ(livePlans, engine.topologyPlans.InUse());
        if (vintage >= 3) {
            publication.view.compiledWorld = fixture.image.world;
            publication.view.compiledWorld.identity.runtimeSemanticFingerprint ^= 1;
            EXPECT_EQ(harken::ProjectGenerationPrepareStatusV1::InvalidWorld,
                harken::PrepareProjectGenerationV1(engine, publication.view, prepared).status);
            EXPECT_EQ(0u, engine.preparedTopologyPlans.InUse());
            EXPECT_EQ(head, engine.commands.PublishedHead());
        } else {
            publication.view.compiledWorld = fixture.image.world;
            EXPECT_EQ(harken::ProjectGenerationPrepareStatusV1::Backpressure,
                harken::PrepareProjectGenerationV1(engine, publication.view, prepared).status);
        }
    }
}

TEST(BusAdmissionStart, V3AndV4StillAllowValidatedReplacementWithoutConsumers) {
    for (const auto vintage : {3u, 4u}) {
        AdmissionSession fixture;
        auto& engine = *fixture.session.Engine();
        auto changed = ChangedAdmissionWorld(fixture.image);
        LiveBusPublication publication(engine, changed, vintage);
        const auto generation = engine.variables.CurrentWorldGeneration();
        CommitAndConsumeLiveBus(engine, publication);
        ASSERT_FALSE(HasFatalFailure());
        EXPECT_EQ(generation + 1, engine.variables.CurrentWorldGeneration());
        EXPECT_EQ(changed.world.identity.runtimeSemanticFingerprint,
            engine.variables.CurrentWorldInstallView().identity.runtimeSemanticFingerprint);
    }
}

enum class LiveBusProtection { Slice, TransportDrain, WorldDrain, DirectAction };

void IntroduceLiveBusProtection(harken::Engine& engine,
    const AdmissionWorldImage& image, LiveBusProtection protection) {
    if (protection == LiveBusProtection::DirectAction) {
        VnPostActionDesc desc{};
        desc.structSize = sizeof(desc);
        desc.actionId = kAction;
        VnActionFiringHandle firing = 0;
        ASSERT_EQ(VN_OK, Vn_PostAction(&desc, &firing));
        ASSERT_NE(0u, firing);
        ASSERT_TRUE(harken::HasPredecessorDirectWorldPublication(engine));
        return;
    }
    harken::VariableSliceRef ref{};
    ASSERT_TRUE(engine.variableSlices.Reserve(engine.variables.CurrentWorldGeneration(),
        engine.variables.CurrentTransportEpoch(), 901, 10000, 901, 1, &ref));
    harken::VariableSliceTarget target{};
    target.locator.scope = harken::VariableTargetScope::Bus;
    target.locator.objectGeneration = engine.buses.AudioGenerationAt(0);
    target.locator.targetId = harken::kBusTargetBusVolume;
    target.command.opcode = harken::kOpcodeSetBusVolume;
    target.command.payload.setBusVolume.busIndex = 0;
    target.command.payload.setBusVolume.busGeneration = target.locator.objectGeneration;
    ASSERT_TRUE(engine.variableSlices.AddTarget(ref, target));
    ASSERT_TRUE(engine.variableSlices.Publish(ref));
    ASSERT_EQ(harken::VariableSliceState::PageCommitted, engine.variableSlices.State(ref));
    ASSERT_TRUE(engine.variables.HasUnreclaimedLiveWorldRows());
    if (protection == LiveBusProtection::TransportDrain) {
        ASSERT_FALSE(engine.variables.ResetTransport(0));
        ASSERT_TRUE(engine.variables.DrainingTransport());
    } else if (protection == LiveBusProtection::WorldDrain) {
        auto changed = ChangedAdmissionWorld(image);
        ASSERT_EQ(harken::WorldInstallStatus::Backpressure,
            engine.variables.Install(changed.world).status);
        ASSERT_TRUE(engine.variables.DrainingWorld());
    }
}

TEST(BusAdmissionStart, LiveBusProtectedStatesRejectPrepareAndConditionsAddedAfterPrepare) {
    for (const auto vintage : {3u, 4u}) {
        for (const auto protection : {LiveBusProtection::Slice, LiveBusProtection::TransportDrain,
                 LiveBusProtection::WorldDrain, LiveBusProtection::DirectAction}) {
            for (const bool afterPrepare : {false, true}) {
                SCOPED_TRACE(vintage);
                SCOPED_TRACE(static_cast<int>(protection));
                SCOPED_TRACE(afterPrepare);
                AdmissionSession fixture;
                auto& engine = *fixture.session.Engine();
                if (protection == LiveBusProtection::DirectAction) {
                    SetAbsoluteTime(&fixture.image.playStep.delay, 1000000000);
                    fixture.image.action.directFeatureMask = 1;
                    fixture.image.action.transitiveFeatureMask = 1;
                    fixture.image.action.programSemanticFingerprint =
                        harken::ComputeActionProgramFingerprintV1(fixture.image.action);
                    fixture.image.BindWorld();
                    fixture.image.world.identity.runtimeSemanticFingerprint =
                        harken::ComputeCurrentWorldFingerprintV5(fixture.image.world);
                    ASSERT_EQ(harken::WorldInstallStatus::Ok,
                        harken::InstallContainerWorldV1(engine, fixture.image.world).status);
                }
                LiveBusPublication publication(engine, fixture.image, vintage);
                harken::PreparedProjectGenerationV1 prepared;
                if (afterPrepare) {
                    ASSERT_EQ(harken::ProjectGenerationPrepareStatusV1::Ok,
                        harken::PrepareProjectGenerationV1(engine, publication.view, prepared).status);
                }
                IntroduceLiveBusProtection(engine, fixture.image, protection);
                ASSERT_FALSE(HasFatalFailure());
                const auto head = engine.commands.PublishedHead();
                const auto free = engine.commands.FreeSlots();
                const auto live = engine.topologyPlans.InUse();
                const auto generation = engine.variables.CurrentWorldGeneration();
                if (afterPrepare) {
                    EXPECT_EQ(harken::ProjectGenerationCommitStatusV1::Backpressure,
                        harken::CommitPreparedProjectGenerationV1(engine, prepared).status);
                    EXPECT_TRUE(prepared.Valid());
                } else {
                    EXPECT_EQ(harken::ProjectGenerationPrepareStatusV1::Backpressure,
                        harken::PrepareProjectGenerationV1(engine, publication.view, prepared).status);
                    EXPECT_FALSE(prepared.Valid());
                }
                EXPECT_EQ(head, engine.commands.PublishedHead());
                EXPECT_EQ(free, engine.commands.FreeSlots());
                EXPECT_EQ(live, engine.topologyPlans.InUse());
                EXPECT_EQ(generation, engine.variables.CurrentWorldGeneration());
                EXPECT_EQ(0u, ValidAckCount(engine));
                EXPECT_EQ(0u, LiveOwnerRevision(engine, publication.view.ownerId));
            }
        }
    }
}

TEST(BusAdmissionStart, ChangedWorldConsumerIntroducedAfterPrepareStillBlocksCommit) {
    AdmissionSession fixture;
    auto& engine = *fixture.session.Engine();
    auto changed = ChangedAdmissionWorld(fixture.image);
    LiveBusPublication publication(engine, changed, 4);
    harken::PreparedProjectGenerationV1 prepared;
    ASSERT_EQ(harken::ProjectGenerationPrepareStatusV1::Ok,
        harken::PrepareProjectGenerationV1(engine, publication.view, prepared).status);
    StartLiveBusConsumer(engine, fixture.image);
    ASSERT_FALSE(HasFatalFailure());
    const auto head = engine.commands.PublishedHead();
    EXPECT_EQ(harken::ProjectGenerationCommitStatusV1::Backpressure,
        harken::CommitPreparedProjectGenerationV1(engine, prepared).status);
    EXPECT_EQ(head, engine.commands.PublishedHead());
    EXPECT_EQ(0u, ValidAckCount(engine));
    EXPECT_TRUE(prepared.Valid());
}

TEST(BusAdmissionStart, RecycledValidAckSurvivesFinalPreflightRefusalAndRetryPublishesOnce) {
    AdmissionSession fixture;
    auto& engine = *fixture.session.Engine();
    LiveBusPublication publication(engine, fixture.image, 4);
    CommitAndConsumeLiveBus(engine, publication);
    ASSERT_FALSE(HasFatalFailure());
    publication.Next(-3.0f);
    CommitAndConsumeLiveBus(engine, publication);
    ASSERT_FALSE(HasFatalFailure());
    engine.topologyPlans.DrainRetire();
    const auto reused = engine.topologyPlans.Acquire();
    ASSERT_NE(harken::kInvalidTopologyPlanIndex, reused);
    ASSERT_EQ(1u, engine.projectGenerationInstallAckSideTable[reused].valid);
    engine.topologyPlans.Recycle(reused);
    StartLiveBusConsumer(engine, fixture.image, 0.25f * std::pow(10.0f, -3.0f / 20.0f));
    ASSERT_FALSE(HasFatalFailure());
    publication.Next(-6.0f);
    harken::PreparedProjectGenerationV1 prepared;
    ASSERT_EQ(harken::ProjectGenerationPrepareStatusV1::Ok,
        harken::PrepareProjectGenerationV1(engine, publication.view, prepared).status);
    const auto token = engine.lastPairedProjectGenerationToken;
    const auto busFingerprint = engine.lastPairedPublishedBusFingerprint;
    const auto profileFingerprint = engine.lastPairedCompileTargetProfileFingerprint;
    const auto acks = engine.projectGenerationInstallAckSideTable;
    const auto owners = engine.busTopologyOwners;
    const auto holders = engine.previewMasterHolders;
    const auto admissions = engine.currentWorldBusAdmission;
    const auto publicationVintage = engine.lastPairedPublicationVintage;
    const auto revision = engine.acceptedBusTopologyRevision;
    const auto generation = engine.variables.CurrentWorldGeneration();
    const auto head = engine.commands.PublishedHead();
    const auto free = engine.commands.FreeSlots();
    const auto live = engine.topologyPlans.InUse();
    const auto busCount = engine.buses.BusCount();
    const auto active = engine.buses.ActiveBusCount();
    const auto masterGeneration = engine.buses.GenerationAt(0);
    const auto masterAlive = engine.buses.AliveAt(0);
    const auto masterParent = engine.buses.ParentOf(0);
    const auto volume = engine.busTargets.Reduce(0, harken::kBusTargetBusVolume);
    harken::detail::ArmLogicAllocFaultForTests(1, harken::detail::LogicAllocSite::ProjectFinalPreflight);
    const auto result = harken::CommitPreparedProjectGenerationV1(engine, prepared);
    EXPECT_EQ(0u, harken::detail::g_logicAllocFault.remaining);
    harken::detail::DisarmLogicAllocFaultForTests();
    EXPECT_EQ(harken::ProjectGenerationCommitStatusV1::Backpressure, result.status);
    ASSERT_TRUE(prepared.Valid());
    EXPECT_EQ(token.projectSessionId, engine.lastPairedProjectGenerationToken.projectSessionId);
    EXPECT_EQ(token.authoringRevision, engine.lastPairedProjectGenerationToken.authoringRevision);
    EXPECT_EQ(token.runtimeEpoch, engine.lastPairedProjectGenerationToken.runtimeEpoch);
    EXPECT_EQ(token.submissionSerial, engine.lastPairedProjectGenerationToken.submissionSerial);
    EXPECT_EQ(busFingerprint, engine.lastPairedPublishedBusFingerprint);
    EXPECT_EQ(profileFingerprint, engine.lastPairedCompileTargetProfileFingerprint);
    EXPECT_EQ(publicationVintage, engine.lastPairedPublicationVintage);
    ASSERT_EQ(acks.size(), engine.projectGenerationInstallAckSideTable.size());
    for (std::size_t i = 0; i < acks.size(); ++i) {
        const auto& before = acks[i];
        const auto& after = engine.projectGenerationInstallAckSideTable[i];
        EXPECT_EQ(before.valid, after.valid) << i;
        EXPECT_EQ(before.zero, after.zero) << i;
        EXPECT_EQ(before.slotGeneration, after.slotGeneration) << i;
        EXPECT_EQ(before.planSequence, after.planSequence) << i;
        EXPECT_EQ(before.planFingerprint, after.planFingerprint) << i;
        EXPECT_EQ(before.claimSetFingerprint, after.claimSetFingerprint) << i;
    }
    for (std::size_t i = 0; i < owners.size(); ++i) {
        const auto& before = owners[i];
        const auto& after = engine.busTopologyOwners[i];
        EXPECT_EQ(before.live, after.live);
        EXPECT_EQ(before.ownerId, after.ownerId);
        EXPECT_EQ(before.currentRevision, after.currentRevision);
        EXPECT_EQ(before.predecessorRevision, after.predecessorRevision);
        EXPECT_EQ(before.digestLow, after.digestLow);
        EXPECT_EQ(before.digestHigh, after.digestHigh);
        EXPECT_EQ(holders[i].occupied, engine.previewMasterHolders[i].occupied);
        EXPECT_EQ(holders[i].ownerId, engine.previewMasterHolders[i].ownerId);
        EXPECT_EQ(holders[i].revision, engine.previewMasterHolders[i].revision);
        EXPECT_EQ(holders[i].fingerprint, engine.previewMasterHolders[i].fingerprint);
    }
    ASSERT_EQ(admissions.size(), engine.currentWorldBusAdmission.size());
    for (std::size_t i = 0; i < admissions.size(); ++i) {
        const auto& before = admissions[i];
        const auto& after = engine.currentWorldBusAdmission[i];
        EXPECT_EQ(before.generation, after.generation);
        EXPECT_EQ(before.occupied, after.occupied);
        EXPECT_EQ(before.state, after.state);
        EXPECT_EQ(before.key.tag, after.key.tag);
        EXPECT_EQ(before.key.zero, after.key.zero);
        for (std::size_t field = 0; field < 5; ++field)
            EXPECT_EQ(before.key.fields[field], after.key.fields[field]);
    }
    EXPECT_EQ(revision, engine.acceptedBusTopologyRevision);
    EXPECT_EQ(generation, engine.variables.CurrentWorldGeneration());
    EXPECT_EQ(head, engine.commands.PublishedHead());
    EXPECT_EQ(free, engine.commands.FreeSlots());
    EXPECT_EQ(live, engine.topologyPlans.InUse());
    EXPECT_EQ(busCount, engine.buses.BusCount());
    EXPECT_EQ(active, engine.buses.ActiveBusCount());
    EXPECT_EQ(VN_BUS_MASTER, engine.buses.IdAt(0));
    EXPECT_EQ(masterGeneration, engine.buses.GenerationAt(0));
    EXPECT_EQ(masterAlive, engine.buses.AliveAt(0));
    EXPECT_EQ(masterParent, engine.buses.ParentOf(0));
    EXPECT_FLOAT_EQ(volume, engine.busTargets.Reduce(0, harken::kBusTargetBusVolume));
    ASSERT_EQ(harken::ProjectGenerationCommitStatusV1::Ok,
        harken::CommitPreparedProjectGenerationV1(engine, prepared).status);
    EXPECT_EQ(head + 1, engine.commands.PublishedHead());
    EXPECT_EQ(revision + 1, engine.acceptedBusTopologyRevision);
    EXPECT_EQ(publication.view.ownerRevision, LiveOwnerRevision(engine, publication.view.ownerId));
    const auto& ack = engine.projectGenerationInstallAckSideTable[reused];
    EXPECT_EQ(1u, ack.valid);
    EXPECT_EQ(0u, ack.zero);
    EXPECT_EQ(reused + 1u, ack.slotGeneration);
    EXPECT_NE(acks[reused].planSequence, ack.planSequence);
    EXPECT_EQ(publication.view.token.authoringRevision, ack.planFingerprint);
    EXPECT_EQ(publication.view.token.submissionSerial, ack.claimSetFingerprint);
    const auto block = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    ASSERT_GT(engine.mixer.BlocksRendered(), block);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_TRUE(harken::HasCurrentWorldPublicationConsumers(engine));
    EXPECT_EQ(harken::ProjectGenerationCommitStatusV1::InvalidPrepared,
        harken::CommitPreparedProjectGenerationV1(engine, prepared).status);
    EXPECT_EQ(head + 1, engine.commands.PublishedHead());
}

}  // namespace


TEST(BusAdmissionStart, ProjectGenerationGrowthProjectsVariable257) {
    AdmissionSession fixture;
    auto& e = *fixture.session.Engine();
    std::vector<harken::VariableDefinitionV1> definitions;
    for (unsigned i = 0; i < 257; ++i)
        definitions.push_back(harkentest::MakeVariableDefinition(harkentest::ObjectId(10000+i),
            harken::VariableKindV1::Smooth, 0, 8, 2));
    auto view = MakeInstallView(fixture.image);
    view.compiledWorld.variables = definitions.data();
    view.compiledWorld.variableCount = definitions.size();
    view.compiledWorld.capacity.maxVariableDefinitions = definitions.size();
    view.compiledWorld.identity.capabilityMask |= harken::kWorldCapabilityVariableDefinitions;
    view.compiledWorld.identity.runtimeSemanticFingerprint = harken::ComputeCurrentWorldFingerprintV4(view.compiledWorld);
    harken::PreparedProjectGenerationV1 prepared;
    for (unsigned nth : {1u, 2u}) {
        harken::detail::ArmLogicAllocFaultForTests(nth, harken::detail::LogicAllocSite::VariableValueStorage);
        const auto result = harken::PrepareProjectGenerationV1(e, view, prepared);
        harken::detail::DisarmLogicAllocFaultForTests();
        EXPECT_EQ(harken::ProjectGenerationPrepareStatusV1::Capacity, result.status);
        EXPECT_FALSE(prepared.Valid());
        EXPECT_EQ(0u, e.variables.DefinitionCount());
    }
    ASSERT_EQ(harken::ProjectGenerationPrepareStatusV1::Ok, harken::PrepareProjectGenerationV1(e, view, prepared).status);
    ASSERT_EQ(harken::ProjectGenerationCommitStatusV1::Ok, harken::CommitPreparedProjectGenerationV1(e, prepared).status);
    ASSERT_EQ(VN_OK, Vn_SetValue(definitions.back().variableId, 7));
    harkentest::AdvanceVariableWrite(&e);
    float value = 0;
    ASSERT_TRUE(e.params.ScopedVariableValue(nullptr, 0, definitions.back().variableId, &value));
    EXPECT_FLOAT_EQ(7, value);
}

harken::ProjectGenerationInstallViewV1 VariableReplacementView(
    const harken::VariableDefinitionV1* definitions, std::uint32_t count) {
    harken::ProjectGenerationInstallViewV1 view{};
    view.token = {11, 2, 3, 4};
    view.ownerId = 77;
    view.ownerRevision = 1;
    view.compiledWorld.variables = definitions;
    view.compiledWorld.variableCount = count;
    view.compiledWorld.identity.capabilityMask = count != 0
        ? harken::kWorldCapabilityVariableDefinitions : 0;
    view.compiledWorld.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(view.compiledWorld);
    return view;
}

TEST(BusAdmissionStart, PairedWorldReplacementMigratesEmitterValuesByDefinition) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    std::array<harken::VariableDefinitionV1, 3> definitions{};
    for (std::uint32_t i = 0; i < definitions.size(); ++i)
        definitions[i] = harkentest::MakeVariableDefinition(harkentest::ObjectId(7 + i),
            harken::VariableKindV1::Smooth, -1, 1, 0);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
        harkentest::InstallDefinitionsWorld(&e, definitions.data(), definitions.size()).status);
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    for (const auto& definition : definitions) {
        ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, definition.variableId, 0.75f));
        ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(&e));
    }
    for (unsigned i = 0; i < 8; ++i) {
        e.nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    const auto priorWorld = e.variables.CurrentWorldGeneration();
    auto next = std::array{definitions[0], definitions[2]};
    next[0].rangeMin = -0.1f;
    next[0].rangeMax = 0.1f;
    const auto view = VariableReplacementView(next.data(), next.size());
    const auto expectOld = [&] {
        EXPECT_EQ(priorWorld, e.variables.CurrentWorldGeneration());
        for (const auto& definition : definitions) {
            float value = 0;
            EXPECT_TRUE(e.variables.CurrentValue(emitter, definition.variableId, &value));
            EXPECT_FLOAT_EQ(0.75f, value);
            EXPECT_TRUE(e.emitters.LocalValue(emitter, definition.variableId, &value));
            EXPECT_FLOAT_EQ(0.75f, value);
        }
    };
    harken::PreparedProjectGenerationV1 cancelled;
    ASSERT_EQ(harken::ProjectGenerationPrepareStatusV1::Ok,
        harken::PrepareProjectGenerationV1(e, view, cancelled).status);
    expectOld();
    harken::CancelPreparedProjectGenerationV1(e, std::move(cancelled));
    expectOld();
    harken::PreparedProjectGenerationV1 prepared;
    ASSERT_EQ(harken::ProjectGenerationPrepareStatusV1::Ok,
        harken::PrepareProjectGenerationV1(e, view, prepared).status);
    harken::detail::ArmLogicAllocFaultForTests(1,
        harken::detail::LogicAllocSite::ProjectEmitterProjection);
    const auto refused = harken::CommitPreparedProjectGenerationV1(e, prepared);
    harken::detail::DisarmLogicAllocFaultForTests();
    EXPECT_EQ(harken::ProjectGenerationCommitStatusV1::Backpressure, refused.status);
    expectOld();
    ASSERT_EQ(harken::ProjectGenerationCommitStatusV1::Ok,
        harken::CommitPreparedProjectGenerationV1(e, prepared).status);
    EXPECT_EQ(priorWorld + 1, e.variables.CurrentWorldGeneration());
    float value = 0;
    EXPECT_FALSE(e.variables.CurrentValue(emitter, definitions[1].variableId, &value));
    EXPECT_FALSE(e.emitters.LocalValue(emitter, definitions[1].variableId, &value));
    for (std::size_t i = 0; i < next.size(); ++i) {
        VnVariableQueryDescV1 query{};
        query.structSize = sizeof(query);
        query.variableId = next[i].variableId;
        query.emitter = emitter;
        VnVariableQueryResultV1 out{};
        out.structSize = sizeof(out);
        ASSERT_EQ(VN_OK, Vn_QueryVariable(&query, &out));
        EXPECT_FLOAT_EQ(i == 0 ? 0 : 0.75f, out.currentValue);
        EXPECT_EQ(i == 0 ? 0u : 1u, out.hasEmitterOverride);
        EXPECT_FLOAT_EQ(next[i].rangeMax, out.rangeMax);
        EXPECT_EQ(i != 0, e.emitters.LocalValue(emitter, next[i].variableId, &value));
        ASSERT_TRUE(e.params.ScopedVariableValue(&e.emitters, emitter, next[i].variableId, &value));
        EXPECT_FLOAT_EQ(out.currentValue, value);
    }
}

TEST(BusAdmissionStart, PairedWorldReplacementTerminatesUnprojectedWritesOnce) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    auto definition = harkentest::MakeVariableDefinition(harkentest::ObjectId(7),
        harken::VariableKindV1::Smooth, -1, 1, 0);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
        harkentest::InstallDefinitionsWorld(&e, &definition, 1).status);
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    ASSERT_EQ(VN_OK, Vn_SetValue(definition.variableId, 0.25f));
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, definition.variableId, 0.75f));
    ASSERT_EQ(2u, e.variables.PendingCount());
    const auto priorWorld = e.variables.CurrentWorldGeneration();
    definition.defaultValue = -0.25f;
    const auto view = VariableReplacementView(&definition, 1);
    harken::PreparedProjectGenerationV1 prepared;
    ASSERT_EQ(harken::ProjectGenerationPrepareStatusV1::Ok,
        harken::PrepareProjectGenerationV1(e, view, prepared).status);
    EXPECT_EQ(2u, e.variables.PendingCount());
    ASSERT_EQ(harken::ProjectGenerationCommitStatusV1::Ok,
        harken::CommitPreparedProjectGenerationV1(e, prepared).status);
    EXPECT_EQ(0u, e.variables.PendingCount());
    EXPECT_FALSE(e.variables.HasBlockingWrite(definition.variableId));
    EXPECT_EQ(2u, e.variables.TerminationCount());
    unsigned seen = 0;
    harken::VariableOutcomeRecord outcome{};
    while (e.variables.PopOutcome(&outcome)) {
        EXPECT_EQ(harken::VariableOutcomeKind::WorldReplacement, outcome.outcome);
        EXPECT_EQ(priorWorld, outcome.world);
        ++seen;
    }
    EXPECT_EQ(2u, seen);
    for (unsigned i = 0; i < 4; ++i) {
        e.nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    float value = 0;
    ASSERT_TRUE(e.variables.CurrentValue(emitter, definition.variableId, &value));
    EXPECT_FLOAT_EQ(-0.25f, value);
    EXPECT_FALSE(e.variables.PopOutcome(&outcome));
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, definition.variableId, 0.5f));
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(&e));
    ASSERT_TRUE(e.variables.CurrentValue(emitter, definition.variableId, &value));
    EXPECT_FLOAT_EQ(0.5f, value);
}

TEST(BusAdmissionStart, PairedWorldReplacementDoesNotRestoreRecycledEmitterOverrides) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    auto definition = harkentest::MakeVariableDefinition(harkentest::ObjectId(7),
        harken::VariableKindV1::Smooth, -1, 1, 0);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
        harkentest::InstallDefinitionsWorld(&e, &definition, 1).status);
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, definition.variableId, 0.75f));
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(&e));
    for (unsigned i = 0; i < 8; ++i) {
        e.nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    definition.defaultValue = -0.25f;
    harken::PreparedProjectGenerationV1 prepared;
    ASSERT_EQ(harken::ProjectGenerationPrepareStatusV1::Ok,
        harken::PrepareProjectGenerationV1(e, VariableReplacementView(&definition, 1), prepared).status);
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(emitter));
    VnEmitterHandle replacement = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&replacement));
    EXPECT_EQ(static_cast<std::uint32_t>(emitter), static_cast<std::uint32_t>(replacement));
    EXPECT_NE(emitter, replacement);
    ASSERT_EQ(harken::ProjectGenerationCommitStatusV1::Ok,
        harken::CommitPreparedProjectGenerationV1(e, prepared).status);
    float value = 0;
    EXPECT_FALSE(e.emitters.LocalValue(replacement, definition.variableId, &value));
    ASSERT_TRUE(e.variables.CurrentValue(replacement, definition.variableId, &value));
    EXPECT_FLOAT_EQ(-0.25f, value);
}

TEST(BusAdmissionStart, WorldReplacementRefusesReservedWritesBeforeAndAfterPrepare) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    auto definition = harkentest::MakeVariableDefinition(harkentest::ObjectId(7),
        harken::VariableKindV1::Smooth, -1, 1, 0);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
        harkentest::InstallDefinitionsWorld(&e, &definition, 1).status);
    const auto priorWorld = e.variables.CurrentWorldGeneration();
    definition.defaultValue = -0.25f;
    const auto view = VariableReplacementView(&definition, 1);
    const auto observation = harken::ObserveVariableCallAt(priorWorld,
        e.variables.CurrentTransportEpoch(), e.config.blockFrames, 0, 0);
    harken::PreparedVariableWriteToken write;
    ASSERT_EQ(VN_OK, e.variables.PrepareGlobalWrite(
        definition.variableId, 0.75f, observation, 0, &write).result);
    harken::PreparedProjectGenerationV1 refused;
    EXPECT_EQ(harken::ProjectGenerationPrepareStatusV1::Backpressure,
        harken::PrepareProjectGenerationV1(e, view, refused).status);
    harken::CancelPreparedProjectGenerationV1(e, std::move(refused));
    EXPECT_EQ(priorWorld, e.variables.CurrentWorldGeneration());
    EXPECT_EQ(harken::WorldInstallStatus::Backpressure,
        harken::InstallContainerWorldV1(e, view.compiledWorld).status);
    EXPECT_EQ(priorWorld, e.variables.CurrentWorldGeneration());
    e.variables.CancelPreparedWrite(&write);
    harken::PreparedProjectGenerationV1 prepared;
    ASSERT_EQ(harken::ProjectGenerationPrepareStatusV1::Ok,
        harken::PrepareProjectGenerationV1(e, view, prepared).status);
    ASSERT_EQ(VN_OK, e.variables.PrepareGlobalWrite(
        definition.variableId, 0.75f, observation, 0, &write).result);
    EXPECT_NE(harken::ProjectGenerationCommitStatusV1::Ok,
        harken::CommitPreparedProjectGenerationV1(e, prepared).status);
    EXPECT_EQ(priorWorld, e.variables.CurrentWorldGeneration());
    e.variables.CancelPreparedWrite(&write);
    ASSERT_EQ(harken::ProjectGenerationCommitStatusV1::Ok,
        harken::CommitPreparedProjectGenerationV1(e, prepared).status);
    EXPECT_EQ(priorWorld + 1, e.variables.CurrentWorldGeneration());
}

TEST(BusAdmissionStart, PairedIdempotentWorldPreservesPendingWritesAndEmitterValues) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    auto definition = harkentest::MakeVariableDefinition(harkentest::ObjectId(7),
        harken::VariableKindV1::Smooth, -1, 1, 0);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
        harkentest::InstallDefinitionsWorld(&e, &definition, 1).status);
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, definition.variableId, 0.75f));
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(&e));
    for (unsigned i = 0; i < 8; ++i) {
        e.nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    const auto priorWorld = e.variables.CurrentWorldGeneration();
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, definition.variableId, 0.5f));
    harken::PreparedProjectGenerationV1 prepared;
    ASSERT_EQ(harken::ProjectGenerationPrepareStatusV1::Ok,
        harken::PrepareProjectGenerationV1(e, VariableReplacementView(&definition, 1), prepared).status);
    ASSERT_EQ(harken::ProjectGenerationCommitStatusV1::Ok,
        harken::CommitPreparedProjectGenerationV1(e, prepared).status);
    EXPECT_EQ(priorWorld, e.variables.CurrentWorldGeneration());
    EXPECT_EQ(1u, e.variables.PendingCount());
    EXPECT_EQ(0u, e.variables.TerminationCount());
    float value = 0;
    ASSERT_TRUE(e.variables.CurrentValue(emitter, definition.variableId, &value));
    EXPECT_FLOAT_EQ(0.75f, value);
    ASSERT_TRUE(e.emitters.LocalValue(emitter, definition.variableId, &value));
    EXPECT_FLOAT_EQ(0.75f, value);
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(&e));
    ASSERT_TRUE(e.variables.CurrentValue(emitter, definition.variableId, &value));
    EXPECT_FLOAT_EQ(0.5f, value);
}

TEST(BusAdmissionStart, PairedWorldReplacementRejectsGlobalAppliedAfterPrepare) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& e = *session.Engine();
    std::array<harken::VariableDefinitionV1, 2> definitions{};
    for (std::uint32_t i = 0; i < definitions.size(); ++i)
        definitions[i] = harkentest::MakeVariableDefinition(harkentest::ObjectId(7 + i),
            harken::VariableKindV1::Smooth, -1, 1, 0);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
        harkentest::InstallDefinitionsWorld(&e, definitions.data(), definitions.size()).status);
    const auto priorWorld = e.variables.CurrentWorldGeneration();
    ASSERT_EQ(VN_OK, Vn_SetValue(definitions[0].variableId, 0.75f));
    definitions[1].defaultValue = -0.25f;
    const auto view = VariableReplacementView(definitions.data(), definitions.size());
    harken::PreparedProjectGenerationV1 stale;
    ASSERT_EQ(harken::ProjectGenerationPrepareStatusV1::Ok,
        harken::PrepareProjectGenerationV1(e, view, stale).status);
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(&e));
    for (unsigned i = 0; i < 8; ++i) {
        e.nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    EXPECT_EQ(harken::ProjectGenerationCommitStatusV1::InvalidPrepared,
        harken::CommitPreparedProjectGenerationV1(e, stale).status);
    EXPECT_EQ(priorWorld, e.variables.CurrentWorldGeneration());
    float value = 0;
    ASSERT_TRUE(e.variables.CurrentValue(definitions[0].variableId, &value));
    EXPECT_FLOAT_EQ(0.75f, value);
    harken::CancelPreparedProjectGenerationV1(e, std::move(stale));
    harken::PreparedProjectGenerationV1 retry;
    ASSERT_EQ(harken::ProjectGenerationPrepareStatusV1::Ok,
        harken::PrepareProjectGenerationV1(e, view, retry).status);
    ASSERT_EQ(harken::ProjectGenerationCommitStatusV1::Ok,
        harken::CommitPreparedProjectGenerationV1(e, retry).status);
    EXPECT_EQ(priorWorld + 1, e.variables.CurrentWorldGeneration());
    ASSERT_TRUE(e.variables.CurrentValue(definitions[0].variableId, &value));
    EXPECT_FLOAT_EQ(0.75f, value);
    ASSERT_TRUE(e.params.ScopedVariableValue(nullptr, 0, definitions[0].variableId, &value));
    EXPECT_FLOAT_EQ(0.75f, value);
}
