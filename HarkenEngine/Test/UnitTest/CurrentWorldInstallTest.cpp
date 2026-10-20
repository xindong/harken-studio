#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include "Engine.h"
#include "harken/core/CurrentWorld.h"
#include "harken/core/CurrentWorldArtifact.h"
#include "harken/core/CurrentWorldCodec.h"
#include "harken/media/PackFormat.h"
#include "harken/param/ParamTargetTable.h"
#include "harken/render/generated/harken_effect_registry.g.h"

#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <vector>

namespace {

static_assert(static_cast<std::uint32_t>(harken::QueueSpliceV1::MarkerAligned) == 1);
static_assert(static_cast<std::uint32_t>(
                  harken::SignalOnlyPolicyV1::StartNextAtEarliestSafeNoOverlap) ==
              0);
static_assert(static_cast<std::uint32_t>(
                  harken::SignalOnlyPolicyV1::StopQueue) == 1);
static_assert(static_cast<std::uint32_t>(
                  harken::AuthoredCompletionClassV1::FiniteMediaCandidate) == 0);
static_assert(static_cast<std::uint32_t>(
                  harken::AuthoredCompletionClassV1::BoundedTrackPieceCandidate) == 1);
static_assert(static_cast<std::uint32_t>(
                  harken::AuthoredCompletionClassV1::SignalOnly) == 2);

harken::PlayablePropertyPayloadViewV1 CanonicalPayload(
    std::uint64_t presentMask = 0) {
    harken::PlayablePropertyPayloadViewV1 payload{};
    payload.presentMask = presentMask;
    payload.values[3].bits = 1;
    payload.values[11].bits = std::bit_cast<std::uint32_t>(1.0f);
    payload.values[16].bits = std::bit_cast<std::uint32_t>(1.0f);
    return payload;
}

harken::PlayablePropertyPayloadViewV1 ResolvedPayload(
    std::uint64_t presentMask = 0) {
    auto payload = CanonicalPayload(presentMask);
    payload.presentMask |= harken::kResolvedSectionGroupPropertyMaskV1;
    payload.values[6].objectId = VN_BUS_MASTER;
    return payload;
}

struct RunnableWorld {
    harken::PlayablePropertyProgramViewV1 properties[2]{};
    harken::PlayableOccurrencePropertyViewV1 occurrence{};
    harken::ContainerChildProgramViewV2 child{};
    harken::ContainerProgramViewV3 container{};
    harken::ContainerChildProgramViewV1 historicalChild{};
    harken::ContainerProgramViewV2 historicalContainer{};
    harken::AuthoredCompletionLeafV1 historicalLeaf{};
    harken::ContainerWorldInstallViewV1 world{};

    RunnableWorld() {
        properties[0].contentPropertyKind =
            harken::playableproperties::ContentPropertyKindV1::Sound;
        properties[0].stableId = harkentest::ObjectId(10);
        properties[0].authoredStableOrder = 0;
        properties[0].contentRevision = 101;
        properties[0].routeStableId = harkentest::ObjectId(201);
        properties[0].compatibilityFingerprint = 301;
        properties[0].payload = ResolvedPayload(1);
        properties[0].programSemanticFingerprint =
            harken::ComputePlayablePropertyFingerprintV1(properties[0]);

        properties[1].contentPropertyKind =
            harken::playableproperties::ContentPropertyKindV1::Container;
        properties[1].stableId = harkentest::ObjectId(20);
        properties[1].authoredStableOrder = 1;
        properties[1].contentRevision = 102;
        properties[1].routeStableId = harkentest::ObjectId(202);
        properties[1].compatibilityFingerprint = 302;
        properties[1].payload = ResolvedPayload();
        properties[1].programSemanticFingerprint =
            harken::ComputePlayablePropertyFingerprintV1(properties[1]);

        child.playable.kind = harken::PlayableKindV1::Sound;
        child.playable.stableId = properties[0].stableId;
        child.playable.expectedProgramSemanticFingerprint =
            properties[0].programSemanticFingerprint;
        child.playable.contentRevision = properties[0].contentRevision;
        child.playable.routeStableId = properties[0].routeStableId;
        child.playable.compatibilityFingerprint =
            properties[0].compatibilityFingerprint;
        child.frequencyWeight = 1;

        occurrence.parentKind =
            harken::playableproperties::ContentPropertyKindV1::Container;
        occurrence.parentStableId = properties[1].stableId;
        occurrence.parentProgramSemanticFingerprint =
            properties[1].programSemanticFingerprint;
        occurrence.childPlayable = child.playable;
        occurrence.payload = CanonicalPayload(1);
        occurrence.occurrenceSemanticFingerprint =
            harken::ComputePlayableOccurrenceFingerprintV1(occurrence);

        container.containerStableId = properties[1].stableId;
        container.programSemanticFingerprint =
            properties[1].programSemanticFingerprint;
        container.childCount = 1;
        container.children = &child;
        world.playableProperties = properties;
        world.playablePropertyCount = 2;
        world.playableOccurrences = &occurrence;
        world.playableOccurrenceCount = 1;
        world.containers = &container;
        world.containerCount = 1;
        world.identity.capabilityMask =
            harken::kWorldCapabilityPlayableProperties |
            harken::kWorldCapabilityRandomDraw;
        RefreshWorldFingerprint();
    }

    void RefreshWorldFingerprint() {
        world.identity.runtimeSemanticFingerprint =
            harken::ComputeCurrentWorldFingerprintV4(world);
    }

    void RefreshSemanticRows() {
        properties[0].programSemanticFingerprint =
            harken::ComputePlayablePropertyFingerprintV1(properties[0]);
        properties[1].programSemanticFingerprint =
            harken::ComputePlayablePropertyFingerprintV1(properties[1]);
        child.playable.expectedProgramSemanticFingerprint =
            properties[0].programSemanticFingerprint;
        child.playable.contentRevision = properties[0].contentRevision;
        occurrence.parentProgramSemanticFingerprint =
            properties[1].programSemanticFingerprint;
        occurrence.childPlayable = child.playable;
        occurrence.occurrenceSemanticFingerprint =
            harken::ComputePlayableOccurrenceFingerprintV1(occurrence);
        container.programSemanticFingerprint =
            properties[1].programSemanticFingerprint;
        RefreshWorldFingerprint();
    }
};

TEST(CurrentWorldInstall, LargeActionUsesDeclaredCountsAndPreservesAcceptedDecodeOnRefusal) {
    RunnableWorld source;
    constexpr std::uint32_t stepCount = 10000;
    std::vector<harken::ActionStepProgramViewV1> steps(stepCount);
    for (std::uint32_t index = 0; index < stepCount; ++index) {
        auto& step = steps[index];
        step.opcode = harken::ActionOpcodeV1::Stop;
        step.authoredEventIndex = index;
        step.target.stableId = source.properties[0].stableId;
        step.target.expectedProgramSemanticFingerprint =
            source.properties[0].programSemanticFingerprint;
        step.delay.domain = harken::AuthoredTimeDomainV1::AbsoluteNanoseconds;
        step.delay.denominator = 1;
        step.fade = step.delay;
        step.cooldown = step.delay;
    }
    harken::ActionProgramViewV1 action;
    action.actionStableId = harkentest::ObjectId(30);
    action.steps = steps.data();
    action.stepCount = stepCount;
    action.expandedStepOccurrenceCount = stepCount;
    action.expandedFiringCount = 1;
    action.programSemanticFingerprint = harken::ComputeActionProgramFingerprintV1(action);
    source.world.actions = &action;
    source.world.actionCount = 1;
    source.world.identity.capabilityMask |= harken::kWorldCapabilityActionDefinitions;
    source.world.capacity.maxActionStepsPerDefinition = stepCount;
    source.world.capacity.maxActionStepsTotal = stepCount;
    source.world.capacity.maxActionTriggerDepth = 1;
    source.world.capacity.maxActionExpandedWork = stepCount;
    source.RefreshWorldFingerprint();
    ASSERT_EQ(harken::WorldInstallStatus::Ok, harken::ValidateCurrentWorldV1(source.world));

    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(harken::EncodeCurrentWorldShippingBody(source.world, bytes));
    harken::CurrentWorldBodyImage decoded;
    ASSERT_TRUE(decoded.Decode(bytes.data(), bytes.size(),
        harken::kCurrentWorldShippingBodyVintage, harken::CurrentWorldDecodeMode::StrictArtifact));
    const auto* accepted = decoded.ProductionView();
    ASSERT_NE(nullptr, accepted);
    ASSERT_EQ(harken::WorldInstallStatus::Ok, harken::ValidateCurrentWorldV1(*accepted));
    ASSERT_EQ(1u, accepted->actionCount);
    ASSERT_EQ(stepCount, accepted->actions[0].stepCount);
    for (std::uint32_t index = 0; index < stepCount; ++index) {
        EXPECT_EQ(index, accepted->actions[0].steps[index].authoredEventIndex);
        EXPECT_EQ(harken::ActionOpcodeV1::Stop, accepted->actions[0].steps[index].opcode);
    }

    EXPECT_FALSE(decoded.Decode(bytes.data(), bytes.size() - 1,
        harken::kCurrentWorldShippingBodyVintage, harken::CurrentWorldDecodeMode::StrictArtifact));
    EXPECT_EQ(accepted, decoded.ProductionView());
    auto excessiveCount = bytes;
    const auto countOffset = bytes.size() -
        static_cast<std::size_t>(stepCount) * harken::kCurrentWorldActionStepWireBytes -
        harken::kCurrentWorldActionHeaderWireBytes + 24;
    for (std::uint32_t byte = 0; byte < 4; ++byte) excessiveCount[countOffset + byte] = 0xff;
    EXPECT_FALSE(decoded.Decode(excessiveCount.data(), excessiveCount.size(),
        harken::kCurrentWorldShippingBodyVintage, harken::CurrentWorldDecodeMode::StrictArtifact));
    EXPECT_EQ(accepted, decoded.ProductionView());

    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
        harken::InstallContainerWorldV1(*engine, source.world).status);
    const auto generation = engine->variables.CurrentWorldGeneration();
    EXPECT_EQ(stepCount, engine->variables.ActionProgramAt(0).stepCount);
    for (auto* limit : {&source.world.capacity.maxActionStepsPerDefinition,
                       &source.world.capacity.maxActionStepsTotal,
                       &source.world.capacity.maxActionExpandedWork}) {
        *limit = stepCount - 1;
        source.RefreshWorldFingerprint();
        EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
            harken::InstallContainerWorldV1(*engine, source.world).status);
        EXPECT_EQ(generation, engine->variables.CurrentWorldGeneration());
        EXPECT_EQ(stepCount - 1,
            engine->variables.ActionProgramAt(0).steps[stepCount - 1].authoredEventIndex);
        *limit = stepCount;
    }
    source.RefreshWorldFingerprint();
    EXPECT_EQ(harken::WorldInstallStatus::Ok,
        harken::InstallContainerWorldV1(*engine, source.world).status);
}

TEST(CurrentWorldInstall, DefinitionFingerprintMatchesPinnedCanonicalBytes) {
    RunnableWorld image;
    image.properties[0].payload = CanonicalPayload(1);
    EXPECT_EQ(UINT64_C(0x6af80ae1bd894fa5),
              harken::ComputePlayablePropertyFingerprintV1(
                  image.properties[0]));
}

TEST(CurrentWorldInstall, WorldFingerprintMatchesPinnedCanonicalBytes) {
    const harken::ContainerWorldInstallViewV1 empty{};
    EXPECT_EQ(UINT64_C(0x8b272f6b42796eba),
              harken::ComputeCurrentWorldFingerprintV2(harkentest::AsHistoricalPrefix(empty)));
}

TEST(CurrentWorldInstall, RequiresCompleteDefinitionGroupsAndRejectsOccurrenceGroups) {
    RunnableWorld image;
    ASSERT_TRUE(harken::ValidateCurrentWorldSectionGroupsV1(image.world));
    for (std::uint32_t slot = 0; slot < image.properties[0].payload.values.size(); ++slot) {
        const auto bit = UINT64_C(1) << slot;
        if ((harken::kResolvedSectionGroupPropertyMaskV1 & bit) == 0) continue;
        image.properties[0].payload.presentMask &= ~bit;
        EXPECT_EQ(harken::ValidateCurrentWorldSectionGroupsV1(image.world), slot == 9);
        if (slot == 9) {
            image.properties[0].payload.values[slot].bits = std::bit_cast<std::uint32_t>(1.0f);
            EXPECT_FALSE(harken::ValidateCurrentWorldSectionGroupsV1(image.world));
            image.properties[0].payload.values[slot].bits = 0;
        }
        image.properties[0].payload.presentMask |= bit;
        image.occurrence.payload.presentMask |= bit;
        EXPECT_FALSE(harken::ValidateCurrentWorldSectionGroupsV1(image.world));
        image.occurrence.payload.presentMask &= ~bit;
    }
    EXPECT_TRUE(harken::ValidateCurrentWorldSectionGroupsV1(image.world));
}

TEST(CurrentWorldInstall, GroupSnapshotsValidateRangesAndRoutingAsAWhole) {
    auto payload = ResolvedPayload();
    const auto accepts = [&](std::uint32_t slot, float value) {
        auto candidate = payload;
        candidate.values[slot].bits = std::bit_cast<std::uint32_t>(value);
        return harken::ValidateResolvedSectionGroupPayloadV1(candidate);
    };
    EXPECT_TRUE(accepts(5, -96.0f));
    EXPECT_TRUE(accepts(5, 24.0f));
    EXPECT_FALSE(accepts(5, 24.01f));
    EXPECT_FALSE(accepts(5, -96.01f));
    EXPECT_TRUE(accepts(8, -1.0f));
    EXPECT_TRUE(accepts(8, 1.0f));
    EXPECT_FALSE(accepts(8, 1.01f));
    EXPECT_TRUE(accepts(9, 100.0f));
    EXPECT_FALSE(accepts(9, -0.01f));
    EXPECT_FALSE(accepts(9, 100.01f));
    EXPECT_TRUE(accepts(11, 0.01f));
    EXPECT_TRUE(accepts(11, 100.0f));
    EXPECT_FALSE(accepts(11, 0.0f));
    EXPECT_FALSE(accepts(11, 100.01f));
    EXPECT_TRUE(accepts(16, 0.0f));
    EXPECT_TRUE(accepts(16, 1.0f));
    EXPECT_FALSE(accepts(16, -0.01f));
    EXPECT_FALSE(accepts(16, 1.01f));
    payload.values[6].objectId = {};
    EXPECT_FALSE(harken::ValidateResolvedSectionGroupPayloadV1(payload));
    payload.values[6].objectId = VN_BUS_MASTER;
    harken::SendV1 sends[] = {{0, 0, harkentest::ObjectId(2), -6.0f, 0}, {1, 0, harkentest::ObjectId(3), -3.0f, 0}};
    payload.sends = sends;
    payload.sendCount = 2;
    EXPECT_TRUE(harken::ValidateResolvedSectionGroupPayloadV1(payload));
    sends[1].targetBusId = sends[0].targetBusId;
    EXPECT_FALSE(harken::ValidateResolvedSectionGroupPayloadV1(payload));
    sends[1].targetBusId = VN_BUS_MASTER;
    EXPECT_FALSE(harken::ValidateResolvedSectionGroupPayloadV1(payload));
    sends[1].targetBusId = harkentest::ObjectId(3);
    sends[1].sendLevel = 24.01f;
    EXPECT_FALSE(harken::ValidateResolvedSectionGroupPayloadV1(payload));
}

TEST(CurrentWorldInstall, StructuralContributionsExcludeGroupsStreamingAndEffects) {
    RunnableWorld image;
    harken::StructuralAncestorRefV1 ancestor{};
    ancestor.payload = CanonicalPayload();
    image.properties[0].structuralAncestorCount = 1;
    image.properties[0].structuralAncestors = &ancestor;
    EXPECT_TRUE(harken::ValidateCurrentWorldSectionGroupsV1(image.world));
    const auto localMask = harken::kResolvedSectionGroupPropertyMaskV1 |
        (UINT64_C(1) << 4) | (UINT64_C(1) << 15);
    for (std::uint32_t slot = 0; slot < ancestor.payload.values.size(); ++slot) {
        const auto bit = UINT64_C(1) << slot;
        if ((localMask & bit) == 0) continue;
        ancestor.payload.presentMask = bit;
        EXPECT_FALSE(harken::ValidateCurrentWorldSectionGroupsV1(image.world));
    }
    ancestor.payload.presentMask = 1;
    EXPECT_TRUE(harken::ValidateCurrentWorldSectionGroupsV1(image.world));
}

TEST(CurrentWorldInstall, LargeSendSnapshotsRejectDistantDuplicatesWithoutReordering) {
    auto payload = ResolvedPayload();
    std::vector<harken::SendV1> sends(1024);
    for (std::uint32_t index = 0; index < sends.size(); ++index) {
        sends[index].stableSendOrdinal = index;
        sends[index].targetBusId = harkentest::ObjectId(sends.size() - index + 1);
    }
    payload.sendCount = sends.size();
    payload.sends = sends.data();
    ASSERT_TRUE(harken::ValidateResolvedSectionGroupPayloadV1(payload));
    EXPECT_EQ(harkentest::ObjectId(1025u), sends.front().targetBusId);
    EXPECT_EQ(harkentest::ObjectId(2u), sends.back().targetBusId);
    sends.back().targetBusId = sends.front().targetBusId;
    EXPECT_FALSE(harken::ValidateResolvedSectionGroupPayloadV1(payload));
    EXPECT_EQ(harkentest::ObjectId(1025u), sends.front().targetBusId);
    EXPECT_EQ(harkentest::ObjectId(1025u), sends.back().targetBusId);
}

TEST(CurrentWorldInstall, HistoricalDecodeCannotBypassTheShippingInstallBoundary) {
    RunnableWorld image;
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto installed = harken::InstallContainerWorldV1(*session.Engine(), image.world);
    ASSERT_EQ(harken::WorldInstallStatus::Ok, installed.status);
    std::vector<std::uint8_t> body;
    ASSERT_TRUE(harken::EncodeCurrentWorldBodyV8(
        harkentest::AsHistoricalPrefix(image.world), body));
    harken::CurrentWorldBodyImage decoded;
    EXPECT_FALSE(decoded.Decode(body.data(), body.size(), harken::kCurrentWorldBodyVintageV8,
                               harken::CurrentWorldDecodeMode::StrictArtifact));
    EXPECT_FALSE(decoded.Decode(body.data(), body.size(), harken::kCurrentWorldBodyVintageV8,
                               harken::CurrentWorldDecodeMode::ReplayRawAttempt));
    EXPECT_EQ(nullptr, decoded.ReplayView());
    EXPECT_EQ(nullptr, decoded.ProductionView());
    auto retired = image.world;
    retired.sourceBodyVintage = harken::kCurrentWorldBodyVintageV8;
    EXPECT_EQ(harken::WorldInstallStatus::InvalidFormat,
              harken::InstallContainerWorldV1(*session.Engine(), retired).status);
    EXPECT_EQ(installed.resultGeneration, session.Engine()->variables.CurrentWorldGeneration());
    EXPECT_FALSE(harken::EncodeCurrentWorldShippingBody(retired, body));
}

TEST(CurrentWorldInstall, CurrentRawAttemptsPreserveRejectedGroupsWithoutInstalling) {
    RunnableWorld image;
    image.properties[0].payload.presentMask &= ~(UINT64_C(1) << 5);
    image.RefreshSemanticRows();
    std::vector<std::uint8_t> body;
    EXPECT_FALSE(harken::EncodeCurrentWorldShippingBody(image.world, body));
    ASSERT_TRUE(harken::EncodeCurrentWorldShippingBodyRawAttempt(image.world, body));
    harken::CurrentWorldBodyImage decoded;
    EXPECT_FALSE(decoded.Decode(body.data(), body.size(), harken::kCurrentWorldShippingBodyVintage,
                               harken::CurrentWorldDecodeMode::StrictArtifact));
    EXPECT_FALSE(decoded.Decode(body.data(), body.size(), harken::kCurrentWorldShippingBodyVintage,
                               harken::CurrentWorldDecodeMode::ReplayRawAttempt));
    ASSERT_TRUE(decoded.Decode(body.data(), body.size(), harken::kCurrentWorldShippingBodyVintage,
                               harken::CurrentWorldDecodeMode::ReplayCurrentAttempt));
    ASSERT_NE(nullptr, decoded.ProductionView());
    EXPECT_EQ(nullptr, decoded.ReplayView());
    EXPECT_EQ(image.properties[0].payload.presentMask,
              decoded.ProductionView()->playableProperties[0].payload.presentMask);
    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
              harken::ValidateCurrentWorldV1(*decoded.ProductionView()));
}

TEST(CurrentWorldInstall, DirectWorldEnvelopeRequiresCurrentShippingVintage) {
    RunnableWorld image;
    std::vector<std::uint8_t> artifact;
    ASSERT_TRUE(harken::EncodeCurrentWorldArtifactV1(image.world, artifact));
    ASSERT_EQ(18u, harken::kCurrentWorldShippingBodyVintage);
    harken::CurrentWorldArtifactHeaderV1 header;
    ASSERT_EQ(harken::CurrentWorldLoadStatusV1::Ok,
              harken::InspectCurrentWorldArtifactHeaderV1(
                  artifact.data(), artifact.size(), artifact.size(), header));
    for (const std::uint32_t retired :
         {0u, 2u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u, 16u, 17u}) {
        harken::StoreLE32(artifact.data() + 20, retired);
        EXPECT_EQ(harken::CurrentWorldLoadStatusV1::UnsupportedBody,
                  harken::InspectCurrentWorldArtifactHeaderV1(
                      artifact.data(), artifact.size(), artifact.size(), header));
    }
}

TEST(CurrentWorldInstall, StopControlReferenceMayBeAbsentButMustBeWellFormed) {
    harken::ActionStepProgramViewV1 step{};
    step.opcode = harken::ActionOpcodeV1::Stop;
    step.target.kind = harken::ActionTargetKindV1::Sound;
    step.target.stableId = harkentest::ObjectId(10);
    step.target.expectedProgramSemanticFingerprint = 123;
    step.delay = {harken::AuthoredTimeDomainV1::AbsoluteNanoseconds, 0, 1};
    step.fade = step.delay;
    step.cooldown = step.delay;
    harken::ActionProgramViewV1 action{};
    action.actionStableId = harkentest::ObjectId(99);
    action.steps = &step;
    action.stepCount = 1;
    action.expandedStepOccurrenceCount = 1;
    action.expandedFiringCount = 1;
    harken::ContainerWorldInstallViewV1 world{};
    world.actions = &action;
    world.actionCount = 1;
    world.identity.capabilityMask = harken::kWorldCapabilityActionDefinitions;
    const auto validate = [&] {
        action.programSemanticFingerprint = harken::ComputeActionProgramFingerprintV1(action);
        world.identity.runtimeSemanticFingerprint = harken::ComputeCurrentWorldFingerprintV5(world);
        return harken::ValidateCurrentWorldV1(world);
    };
    ASSERT_EQ(harken::WorldInstallStatus::Ok, validate());
    const auto valid = step;
    harken::VariableDefinitionV1 variable{};
    variable.variableId = harkentest::ObjectId(98);
    variable.kind = harken::VariableKindV1::Smooth;
    variable.rangeMax = 1.0f;
    world.variables = &variable;
    world.variableCount = 1;
    world.identity.capabilityMask |= harken::kWorldCapabilityVariableDefinitions;
    ASSERT_EQ(harken::WorldInstallStatus::Ok, validate());
    for (const auto id : {action.actionStableId, variable.variableId}) {
        step = valid;
        step.target.stableId = id;
        EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld, validate());
        step = valid;
        step.quantizeTiming = harken::ActionQuantizeTimingV1::NextBeat;
        step.quantizeReference = step.target;
        step.quantizeReference.stableId = id;
        action.directFeatureMask = action.transitiveFeatureMask = 2;
        EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld, validate());
        action.directFeatureMask = action.transitiveFeatureMask = 0;
    }
    step = valid;
    step.target.expectedProgramSemanticFingerprint = 0;
    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld, validate());
    step = valid;
    step.target.stableId = {};
    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld, validate());
    step = valid;
    step.target.kind = harken::ActionTargetKindV1::Action;
    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld, validate());
    step = valid;
    step.target.reserved = 1;
    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld, validate());
    step = valid;
    step.opcode = harken::ActionOpcodeV1::Play;
    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld, validate());
    step = valid;
    step.quantizeTiming = harken::ActionQuantizeTimingV1::NextBeat;
    step.quantizeReference = step.target;
    step.quantizeReference.stableId = harkentest::ObjectId(11);
    action.directFeatureMask = action.transitiveFeatureMask = 2;
    EXPECT_EQ(harken::WorldInstallStatus::Ok, validate());
    step.quantizeReference.expectedProgramSemanticFingerprint = 0;
    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld, validate());
    step = valid;
    action.directFeatureMask = action.transitiveFeatureMask = 0;
    RunnableWorld known;
    world = known.world;
    world.actions = &action;
    world.actionCount = 1;
    world.identity.capabilityMask |= harken::kWorldCapabilityActionDefinitions;
    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld, validate());
    step.target.expectedProgramSemanticFingerprint = known.properties[0].programSemanticFingerprint;
    ASSERT_EQ(harken::WorldInstallStatus::Ok, validate());
    step.target.kind = harken::ActionTargetKindV1::Container;
    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld, validate());
}

TEST(CurrentWorldInstall, PreviousShippingBodyCannotUseCurrentDecodeModes) {
    RunnableWorld image;
    std::vector<std::uint8_t> body;
    ASSERT_TRUE(harken::EncodeCurrentWorldBodyV9(image.world, body));
    for (const auto mode : {harken::CurrentWorldDecodeMode::StrictArtifact,
                           harken::CurrentWorldDecodeMode::ReplayRawAttempt,
                           harken::CurrentWorldDecodeMode::ReplayCurrentAttempt}) {
        harken::CurrentWorldBodyImage decoded;
        EXPECT_FALSE(decoded.Decode(body.data(), body.size(), harken::kCurrentWorldBodyVintageV9, mode));
        EXPECT_EQ(nullptr, decoded.ProductionView());
        EXPECT_EQ(nullptr, decoded.ReplayView());
    }
}

TEST(CurrentWorldInstall, ShippingBodyRoundTripsSpatializationMix) {
    RunnableWorld image;
    image.properties[0].payload.values[16].bits =
        std::bit_cast<std::uint32_t>(0.5f);
    image.RefreshSemanticRows();
    std::vector<std::uint8_t> body;
    ASSERT_TRUE(harken::EncodeCurrentWorldShippingBody(image.world, body));
    harken::CurrentWorldBodyImage decoded;
    ASSERT_TRUE(decoded.Decode(body.data(), body.size(),
                               harken::kCurrentWorldShippingBodyVintage,
                               harken::CurrentWorldDecodeMode::StrictArtifact));
    ASSERT_NE(nullptr, decoded.ProductionView());
    EXPECT_FLOAT_EQ(
        0.5f,
        std::bit_cast<float>(static_cast<std::uint32_t>(
            decoded.ProductionView()->playableProperties[0]
                .payload.values[16].bits)));

    harken::CurrentWorldBodyImage retired;
    EXPECT_FALSE(retired.Decode(body.data(), body.size(),
                                harken::kCurrentWorldBodyVintageV12,
                                harken::CurrentWorldDecodeMode::StrictArtifact));
}

TEST(CurrentWorldInstall, SpatializationMixChangesSemanticFingerprints) {
    RunnableWorld image;
    const auto definition =
        harken::ComputePlayablePropertyFingerprintV1(image.properties[0]);
    const auto occurrence =
        harken::ComputePlayableOccurrenceFingerprintV1(image.occurrence);
    const auto world = harken::ComputeCurrentWorldFingerprintV4(image.world);

    image.properties[0].payload.values[16].bits =
        std::bit_cast<std::uint32_t>(0.5f);
    EXPECT_NE(definition,
              harken::ComputePlayablePropertyFingerprintV1(image.properties[0]));

    image.occurrence.payload.presentMask |= UINT64_C(1) << 16;
    image.occurrence.payload.values[16].bits =
        std::bit_cast<std::uint32_t>(0.5f);
    EXPECT_NE(occurrence,
              harken::ComputePlayableOccurrenceFingerprintV1(image.occurrence));

    image.RefreshSemanticRows();
    EXPECT_NE(world, harken::ComputeCurrentWorldFingerprintV4(image.world));
}

TEST(CurrentWorldInstall, OccurrenceFingerprintMatchesPinnedCanonicalBytes) {
    harken::PlayableOccurrencePropertyViewV1 row{};
    row.parentKind =
        harken::playableproperties::ContentPropertyKindV1::Container;
    row.stableOccurrenceOrdinal = 7;
    row.parentStableId = harkentest::ObjectId(20);
    row.parentProgramSemanticFingerprint = 0x22;
    row.childPlayable.kind = harken::PlayableKindV1::Sound;
    row.childPlayable.stableId = harkentest::ObjectId(10);
    row.childPlayable.expectedProgramSemanticFingerprint = 0x11;
    row.childPlayable.contentRevision = 101;
    row.childPlayable.routeStableId = harkentest::ObjectId(201);
    row.childPlayable.compatibilityFingerprint = 301;
    row.payload = CanonicalPayload();
    EXPECT_EQ(UINT64_C(0x5abf571225350f18),
              harken::ComputePlayableOccurrenceFingerprintV1(row));
}

TEST(CurrentWorldInstall,
     ActionValidationRejectsHardCapChainBeforeRecursiveDescent) {
    RunnableWorld image;
    constexpr std::uint32_t actionCount =
        harken::kHardActionDefinitionCapacity;
    std::vector<harken::ActionStepProgramViewV1> steps(actionCount);
    std::vector<harken::ActionProgramViewV1> actions(actionCount);
    for (std::uint32_t i = actionCount; i-- > 0;) {
        auto& action = actions[i];
        auto& step = steps[i];
        action.actionStableId = harkentest::ObjectId(1000u + i);
        action.steps = &step;
        action.stepCount = 1;
        action.expandedStepOccurrenceCount = actionCount - i;
        action.expandedFiringCount = actionCount - i;
        if (i + 1u == actionCount) {
            step.opcode = harken::ActionOpcodeV1::Play;
            step.target.kind = harken::ActionTargetKindV1::Sound;
            step.target.stableId = image.properties[0].stableId;
            step.target.expectedProgramSemanticFingerprint =
                image.properties[0].programSemanticFingerprint;
        } else {
            step.opcode = harken::ActionOpcodeV1::PostAction;
            step.target.kind = harken::ActionTargetKindV1::Action;
            step.target.stableId = actions[i + 1u].actionStableId;
            step.target.expectedProgramSemanticFingerprint =
                actions[i + 1u].programSemanticFingerprint;
        }
        action.programSemanticFingerprint =
            harken::ComputeActionProgramFingerprintV1(action);
    }
    image.world.actions = actions.data();
    image.world.actionCount = actionCount;
    image.world.capacity.maxActionDefinitions = actionCount;
    image.world.capacity.maxActionStepsPerDefinition = 1;
    image.world.capacity.maxActionStepsTotal = actionCount;
    image.world.capacity.maxActionTriggerDepth = 1;
    image.world.capacity.maxActionExpandedWork = harken::kMaxActionSteps;
    image.world.identity.capabilityMask |=
        harken::kWorldCapabilityActionDefinitions;
    image.RefreshWorldFingerprint();

    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
              harken::ValidateCurrentWorldV1(image.world));
}

TEST(CurrentWorldInstall,
     ConfiguredActionDefinitionCapacityAcceptsExactAndRejectsExcessWithoutMutation) {
    constexpr std::uint32_t configuredCapacity = 64;
    constexpr std::uint32_t artifactCapacity = configuredCapacity + 1;
    RunnableWorld image;
    std::vector<harken::ActionStepProgramViewV1> steps(artifactCapacity);
    std::vector<harken::ActionProgramViewV1> actions(artifactCapacity);
    for (std::uint32_t i = 0; i < artifactCapacity; ++i) {
        auto& step = steps[i];
        step.opcode = harken::ActionOpcodeV1::Play;
        step.target.kind = harken::ActionTargetKindV1::Sound;
        step.target.stableId = image.properties[0].stableId;
        step.target.expectedProgramSemanticFingerprint =
            image.properties[0].programSemanticFingerprint;
        step.delay = {harken::AuthoredTimeDomainV1::AbsoluteNanoseconds, 0, 1};
        step.fade = {harken::AuthoredTimeDomainV1::AbsoluteNanoseconds, 0, 1};
        step.cooldown = {
            harken::AuthoredTimeDomainV1::AbsoluteNanoseconds, 0, 1};

        auto& action = actions[i];
        action.actionStableId = harkentest::ObjectId(1000u + i);
        action.stepCount = 1;
        action.expandedStepOccurrenceCount = 1;
        action.expandedFiringCount = 1;
    }
    for (std::uint32_t i = 0; i < artifactCapacity; ++i) {
        actions[i].steps = &steps[i];
        actions[i].programSemanticFingerprint =
            harken::ComputeActionProgramFingerprintV1(actions[i]);
    }
    image.world.actions = actions.data();
    image.world.actionCount = configuredCapacity;
    image.world.capacity.maxActionDefinitions = artifactCapacity;
    image.world.capacity.maxActionStepsPerDefinition = 1;
    image.world.capacity.maxActionStepsTotal = artifactCapacity;
    image.world.identity.capabilityMask |=
        harken::kWorldCapabilityActionDefinitions;
    image.RefreshWorldFingerprint();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harken::ValidateCurrentWorldV1(image.world));

    VnConfig config = harkentest::MakeNullConfig();
    config.maxActions = configuredCapacity;
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    const harken::WorldInstallResult accepted =
        harken::InstallContainerWorldV1(*session.Engine(), image.world);
    ASSERT_EQ(harken::WorldInstallStatus::Ok, accepted.status);
    ASSERT_EQ(configuredCapacity,
              session.Engine()->variables.ActionProgramCount());
    const std::uint64_t installedFingerprint =
        session.Engine()->variables.ActionProgramAt(configuredCapacity - 1)
            .programSemanticFingerprint;

    image.world.actionCount = artifactCapacity;
    image.RefreshWorldFingerprint();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harken::ValidateCurrentWorldV1(image.world));
    const harken::WorldInstallResult rejected =
        harken::InstallContainerWorldV1(*session.Engine(), image.world);
    EXPECT_EQ(harken::WorldInstallStatus::Capacity, rejected.status);
    EXPECT_EQ(accepted.resultGeneration, rejected.priorGeneration);
    EXPECT_EQ(0u, rejected.resultGeneration);
    EXPECT_EQ(accepted.resultGeneration,
              session.Engine()->variables.CurrentWorldGeneration());
    ASSERT_EQ(configuredCapacity,
              session.Engine()->variables.ActionProgramCount());
    EXPECT_EQ(installedFingerprint,
              session.Engine()->variables.ActionProgramAt(
                  configuredCapacity - 1).programSemanticFingerprint);
}

TEST(CurrentWorldInstall, TypedRowsAreValidatedDeepOwnedAndIdempotent) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RunnableWorld image;

    const harken::WorldInstallResult first =
        harken::InstallContainerWorldV1(*engine, image.world);
    ASSERT_EQ(harken::WorldInstallStatus::Ok, first.status);
    EXPECT_FALSE(first.idempotent);
    ASSERT_EQ(2u, engine->variables.PlayablePropertyProgramCount());
    ASSERT_EQ(1u, engine->variables.PlayableOccurrencePropertyCount());
    ASSERT_EQ(1u, engine->variables.ContainerProgramCount());
    EXPECT_EQ(engine->config.maxPlayableInstances,
              engine->playableInstances.Capacity());
    EXPECT_EQ(0u, engine->selectionMemory.RowCount());
    const harken::PlayableWorldViewV1 current =
        engine->variables.CurrentPlayableWorld();
    EXPECT_EQ(first.resultGeneration, current.generation);
    ASSERT_EQ(2u, current.propertyCount);
    ASSERT_EQ(1u, current.occurrenceCount);
    ASSERT_EQ(1u, current.containerCount);
    harken::PlayableRefV1 root{};
    root.kind = harken::PlayableKindV1::Container;
    root.stableId = image.properties[1].stableId;
    root.expectedProgramSemanticFingerprint =
        image.properties[1].programSemanticFingerprint;
    root.contentRevision = image.properties[1].contentRevision;
    root.routeStableId = image.properties[1].routeStableId;
    root.compatibilityFingerprint =
        image.properties[1].compatibilityFingerprint;
    const std::uint32_t selected = image.child.stableChildOrdinal;
    ASSERT_EQ(harken::ResolvedPlayablePropertyCutStatusV1::Ok,
              engine->playablePropertyCutScratch.Resolve(
                  current, root, &selected, 1));
    EXPECT_EQ(image.properties[0].stableId,
              engine->playablePropertyCutScratch.View().leaf.stableId);

    const harken::WorldInstallResult repeated =
        harken::InstallContainerWorldV1(*engine, image.world);
    ASSERT_EQ(harken::WorldInstallStatus::Ok, repeated.status);
    EXPECT_TRUE(repeated.idempotent);
    EXPECT_EQ(first.resultGeneration, repeated.resultGeneration);

    image.properties[0].payload.values[0].bits =
        std::bit_cast<std::uint32_t>(-12.0f);
    image.child.frequencyWeight = 99;
    EXPECT_EQ(0u, engine->variables.PlayablePropertyProgramAt(0)
                      .payload.values[0].bits);
    EXPECT_EQ(1u, engine->variables.ContainerProgramAt(0)
                      .children[0].frequencyWeight);
}

TEST(CurrentWorldInstall, InvalidFingerprintAndCapabilityHaveZeroMutation) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RunnableWorld image;
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harken::InstallContainerWorldV1(*engine, image.world).status);
    const std::uint64_t generation = engine->variables.CurrentWorldGeneration();

    image.world.identity.runtimeSemanticFingerprint ^= 1;
    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
              harken::InstallContainerWorldV1(*engine, image.world).status);
    EXPECT_EQ(generation, engine->variables.CurrentWorldGeneration());

    image.RefreshWorldFingerprint();
    image.world.identity.capabilityMask &=
        ~harken::kWorldCapabilityRandomDraw;
    image.RefreshWorldFingerprint();
    EXPECT_EQ(harken::WorldInstallStatus::InvalidFormat,
              harken::InstallContainerWorldV1(*engine, image.world).status);
    EXPECT_EQ(generation, engine->variables.CurrentWorldGeneration());
}

TEST(CurrentWorldInstall, LoopCountSpendsAFullU32WhileNeighbourSlotsStayRanged) {
    RunnableWorld canonical;
    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              harken::ValidateCurrentWorldV1(canonical.world));

    RunnableWorld absentSlotIsNotZero;
    absentSlotIsNotZero.properties[0].payload.values[3].bits = 0;
    absentSlotIsNotZero.RefreshSemanticRows();
    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
              harken::ValidateCurrentWorldV1(absentSlotIsNotZero.world));

    RunnableWorld wideLoopCount;
    wideLoopCount.properties[0].payload.presentMask |= UINT64_C(0x8);
    wideLoopCount.properties[0].payload.values[3].bits = 1000;
    wideLoopCount.RefreshSemanticRows();
    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              harken::ValidateCurrentWorldV1(wideLoopCount.world));

    RunnableWorld outOfRangeStreaming;
    outOfRangeStreaming.properties[0].payload.presentMask |= UINT64_C(0x10);
    outOfRangeStreaming.properties[0].payload.values[4].bits = 2;
    outOfRangeStreaming.RefreshSemanticRows();
    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
              harken::ValidateCurrentWorldV1(outOfRangeStreaming.world));

    RunnableWorld outOfRangeIs3D;
    outOfRangeIs3D.properties[0].payload.presentMask |= UINT64_C(0x400);
    outOfRangeIs3D.properties[0].payload.values[10].bits = 2;
    outOfRangeIs3D.RefreshSemanticRows();
    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
              harken::ValidateCurrentWorldV1(outOfRangeIs3D.world));

    harken::PlayablePropertyProgramViewV1 lowByte = canonical.properties[0];
    lowByte.payload.presentMask |= UINT64_C(0x8);
    lowByte.payload.values[3].bits = 1;
    harken::PlayablePropertyProgramViewV1 sameLowByte = lowByte;
    sameLowByte.payload.values[3].bits = 257;
    EXPECT_NE(harken::ComputePlayablePropertyFingerprintV1(lowByte),
              harken::ComputePlayablePropertyFingerprintV1(sameLowByte));
}

TEST(CurrentWorldInstall, CapacityRefusalPrecedesMutation) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    RunnableWorld image;
    image.world.capacity.maxPlayablePropertyPrograms = 1;
    image.RefreshWorldFingerprint();
    EXPECT_EQ(harken::WorldInstallStatus::Capacity,
              harken::InstallContainerWorldV1(*session.Engine(), image.world)
                  .status);
    EXPECT_EQ(0u, session.Engine()->variables.CurrentWorldGeneration());
}

TEST(CurrentWorldInstall, RetiredPropertiesAreRejected) {
    for (std::uint8_t slot : {12, 14}) {
        RunnableWorld retiredPresence;
        retiredPresence.properties[0].payload.presentMask |= UINT64_C(1) << slot;
        retiredPresence.RefreshSemanticRows();
        EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
                  harken::ValidateCurrentWorldV1(retiredPresence.world));

        RunnableWorld retiredValue;
        retiredValue.properties[0].payload.values[slot].bits = 9;
        retiredValue.RefreshSemanticRows();
        EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
                  harken::ValidateCurrentWorldV1(retiredValue.world));
    }
}

TEST(CurrentWorldInstall, HostileEnumsOrdersAndMissingContentAreRefused) {
    RunnableWorld image;
    image.container.memoryScope = static_cast<harken::MemoryScopeV1>(99);
    image.RefreshWorldFingerprint();
    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
              harken::ValidateCurrentWorldV1(image.world));

    RunnableWorld missingContent;
    missingContent.properties[0].contentRevision = 0;
    missingContent.RefreshSemanticRows();
    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
              harken::ValidateCurrentWorldV1(missingContent.world));

    RunnableWorld duplicateOrder;
    duplicateOrder.properties[1].authoredStableOrder =
        duplicateOrder.properties[0].authoredStableOrder;
    duplicateOrder.RefreshSemanticRows();
    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
              harken::ValidateCurrentWorldV1(duplicateOrder.world));
}

TEST(CurrentWorldInstall, EffectCyclesAreRefused) {
    RunnableWorld cyclic;
    const std::uint32_t paramCount = harken::FindEffectRow(2)->paramCount;
    std::vector<harken::EffectParamV1> firstParams(paramCount);
    std::vector<harken::EffectParamV1> secondParams(paramCount);
    for (std::uint32_t i = 0; i < paramCount; ++i) {
        firstParams[i].paramId = i;
        secondParams[i].paramId = i;
    }
    harken::EffectNodeV1 nodes[2] = {
        {1, 2, paramCount, firstParams.data()},
        {2, 2, paramCount, secondParams.data()},
    };
    const harken::EffectEdgeV1 edges[2] = {{1, 2}, {2, 1}};
    cyclic.properties[0].payload.presentMask |= UINT64_C(1) << 15;
    cyclic.properties[0].payload.effectNodeCount = 2;
    cyclic.properties[0].payload.effectNodes = nodes;
    cyclic.properties[0].payload.effectEdgeCount = 2;
    cyclic.properties[0].payload.effectEdges = edges;
    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
              harken::ValidateCurrentWorldV1(cyclic.world));


}

TEST(CurrentWorldInstall, NestedContainerCyclesAreRefused) {
    harken::PlayablePropertyProgramViewV1 properties[2]{};
    harken::ContainerChildProgramViewV2 children[2]{};
    harken::PlayableOccurrencePropertyViewV1 occurrences[2]{};
    harken::ContainerProgramViewV3 containers[2]{};
    for (std::uint32_t i = 0; i < 2; ++i) {
        properties[i].contentPropertyKind =
            harken::playableproperties::ContentPropertyKindV1::Container;
        properties[i].authoredStableOrder = i;
        properties[i].stableId = harkentest::ObjectId(10 + i * 10);
        properties[i].contentRevision = 100 + i;
        properties[i].payload = ResolvedPayload();
        properties[i].programSemanticFingerprint =
            harken::ComputePlayablePropertyFingerprintV1(properties[i]);
    }
    for (std::uint32_t i = 0; i < 2; ++i) {
        const std::uint32_t target = 1 - i;
        children[i].playable.kind = harken::PlayableKindV1::Container;
        children[i].playable.stableId = properties[target].stableId;
        children[i].playable.expectedProgramSemanticFingerprint =
            properties[target].programSemanticFingerprint;
        children[i].playable.contentRevision =
            properties[target].contentRevision;
        children[i].frequencyWeight = 1;

        occurrences[i].parentKind =
            harken::playableproperties::ContentPropertyKindV1::Container;
        occurrences[i].parentStableId = properties[i].stableId;
        occurrences[i].parentProgramSemanticFingerprint =
            properties[i].programSemanticFingerprint;
        occurrences[i].childPlayable = children[i].playable;
        occurrences[i].payload = CanonicalPayload();
        occurrences[i].occurrenceSemanticFingerprint =
            harken::ComputePlayableOccurrenceFingerprintV1(occurrences[i]);

        containers[i].containerStableId = properties[i].stableId;
        containers[i].programSemanticFingerprint =
            properties[i].programSemanticFingerprint;
        containers[i].authoredStableOrder = i;
        containers[i].childCount = 1;
        containers[i].children = &children[i];
    }
    harken::ContainerWorldInstallViewV1 world{};
    world.playableProperties = properties;
    world.playablePropertyCount = 2;
    world.playableOccurrences = occurrences;
    world.playableOccurrenceCount = 2;
    world.containers = containers;
    world.containerCount = 2;
    world.identity.capabilityMask =
        harken::kWorldCapabilityPlayableProperties |
        harken::kWorldCapabilityRandomDraw;
    world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(world);
    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
              harken::ValidateCurrentWorldV1(world));
    world.capacity.maxContainerDepth = 1;
    world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(world);
    EXPECT_EQ(harken::WorldInstallStatus::Capacity,
              harken::ValidateCurrentWorldV1(world));
}

TEST(CurrentWorldInstall, DefaultProfileAndHardCeilingsRemainDistinct) {
    const harken::WorldCapacitySummaryV1 defaults{};
    EXPECT_EQ(8u, defaults.maxCurvePoints);
    EXPECT_LT(defaults.maxVariableDefinitions,
              harken::kHardVariableDefinitionCapacity);
    EXPECT_LT(defaults.maxContainerPrograms,
              harken::kHardContainerProgramCapacity);
    EXPECT_LT(defaults.maxPlayablePropertyPrograms,
              harken::kHardPlayablePropertyProgramCapacity);

    RunnableWorld image;
    image.world.capacity.maxCurvePoints = 9;
    image.RefreshWorldFingerprint();
    EXPECT_EQ(harken::WorldInstallStatus::Capacity,
              harken::ValidateCurrentWorldV1(image.world));
}

TEST(CurrentWorldInstall, RawCanonicalVariableDefinitionsRejectNegativeZero) {
    const float negativeZero =
        std::bit_cast<float>(UINT32_C(0x80000000));
    const auto validate = [&](const auto& mutate) {
        RunnableWorld image;
        harken::VariableDefinitionV1 variable{};
        variable.variableId = harkentest::ObjectId(1);
        variable.kind = harken::VariableKindV1::Smooth;
        variable.rangeMin = -1.0f;
        variable.rangeMax = 1.0f;
        variable.defaultValue = 0.0f;
        image.world.variables = &variable;
        image.world.variableCount = 1;
        image.world.identity.capabilityMask |=
            harken::kWorldCapabilityVariableDefinitions;
        mutate(variable);
        image.RefreshSemanticRows();
        return harken::ValidateCurrentWorldV1(image.world);
    };

    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              validate([](auto&) {}));
    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
              validate([&](auto& variable) {
                  variable.rangeMin = negativeZero;
              }));
    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
              validate([&](auto& variable) {
                  variable.rangeMax = negativeZero;
              }));
    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
              validate([&](auto& variable) {
                  variable.defaultValue = negativeZero;
              }));

}

TEST(CurrentWorldInstall, TrackPlacementIsDeepOwnedAndPieceKeepsOnlyTrackReference) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    harken::PlayablePropertyProgramViewV1 properties[3]{};
    properties[0].contentPropertyKind =
        harken::playableproperties::ContentPropertyKindV1::Sound;
    properties[0].stableId = harkentest::ObjectId(10);
    properties[0].authoredStableOrder = 0;
    properties[0].contentRevision = 101;
    properties[0].payload = ResolvedPayload();
    properties[0].programSemanticFingerprint =
        harken::ComputePlayablePropertyFingerprintV1(properties[0]);
    properties[1].contentPropertyKind =
        harken::playableproperties::ContentPropertyKindV1::Track;
    properties[1].stableId = harkentest::ObjectId(20);
    properties[1].authoredStableOrder = 1;
    properties[1].contentRevision = 102;
    properties[1].payload = ResolvedPayload();
    properties[1].programSemanticFingerprint =
        harken::ComputePlayablePropertyFingerprintV1(properties[1]);
    properties[2].contentPropertyKind =
        harken::playableproperties::ContentPropertyKindV1::Piece;
    properties[2].stableId = harkentest::ObjectId(30);
    properties[2].authoredStableOrder = 2;
    properties[2].contentRevision = 103;
    properties[2].payload = ResolvedPayload();
    properties[2].programSemanticFingerprint =
        harken::ComputePlayablePropertyFingerprintV1(properties[2]);

    harken::ScheduleClipPlacementV1 placement{};
    placement.stableOccurrenceOrdinal = 7;
    placement.content.kind = harken::PlayableKindV1::Sound;
    placement.content.stableId = properties[0].stableId;
    placement.content.expectedProgramSemanticFingerprint =
        properties[0].programSemanticFingerprint;
    placement.content.contentRevision = properties[0].contentRevision;
    placement.authoredStart = 500000000;
    placement.clipId = harkentest::ObjectId(91);
    placement.clipGuidLow = 92;

    harken::PlayableOccurrencePropertyViewV1 occurrence{};
    occurrence.parentKind =
        harken::playableproperties::ContentPropertyKindV1::Track;
    occurrence.stableOccurrenceOrdinal = placement.stableOccurrenceOrdinal;
    occurrence.parentStableId = properties[1].stableId;
    occurrence.parentProgramSemanticFingerprint =
        properties[1].programSemanticFingerprint;
    occurrence.childPlayable = placement.content;
    occurrence.payload = CanonicalPayload();
    occurrence.occurrenceSemanticFingerprint =
        harken::ComputePlayableOccurrenceFingerprintV1(occurrence);

    harken::TrackScheduleProgramViewV1 track{};
    track.rootDefinition.kind = harken::PlayableKindV1::Track;
    track.rootDefinition.stableId = properties[1].stableId;
    track.rootDefinition.expectedProgramSemanticFingerprint =
        properties[1].programSemanticFingerprint;
    track.rootDefinition.contentRevision = properties[1].contentRevision;
    track.placementCount = 1;
    track.placements = &placement;
    track.scheduleSemanticFingerprint =
        harken::ComputeTrackScheduleFingerprintV1(track);

    harken::ScheduleTrackRefV1 trackRef{};
    trackRef.trackDefinition = track.rootDefinition;
    trackRef.trackScheduleSemanticFingerprint =
        track.scheduleSemanticFingerprint;
    harken::PieceScheduleProgramViewV1 piece{};
    piece.rootDefinition.kind = harken::PlayableKindV1::Piece;
    piece.rootDefinition.stableId = properties[2].stableId;
    piece.rootDefinition.expectedProgramSemanticFingerprint =
        properties[2].programSemanticFingerprint;
    piece.rootDefinition.contentRevision = properties[2].contentRevision;
    piece.trackRefCount = 1;
    piece.trackRefs = &trackRef;
    harken::ScheduleMarkerV1 marker{harkentest::ObjectId(93), 1, 0, 250000000};
    piece.markerCount = 1;
    piece.markers = &marker;
    piece.scheduleSemanticFingerprint =
        harken::ComputePieceScheduleFingerprintV1(piece);

    harken::ContainerWorldInstallViewV1 world{};
    world.playableProperties = properties;
    world.playablePropertyCount = 3;
    world.playableOccurrences = &occurrence;
    world.playableOccurrenceCount = 1;
    world.trackSchedules = &track;
    world.trackScheduleCount = 1;
    world.pieceSchedules = &piece;
    world.pieceScheduleCount = 1;
    world.identity.capabilityMask = harken::kWorldCapabilityPlayableProperties;
    world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(world);

    const auto expectCapacityStatus =
        [&](auto mutateCapacity, harken::WorldInstallStatus expected) {
            const auto saved = world.capacity;
            mutateCapacity(world.capacity);
            world.identity.runtimeSemanticFingerprint =
                harken::ComputeCurrentWorldFingerprintV4(world);
            EXPECT_EQ(expected, harken::ValidateCurrentWorldV1(world));
            world.capacity = saved;
        };
    expectCapacityStatus(
        [](auto& capacity) { capacity.maxTrackSchedulePrograms = 0; },
        harken::WorldInstallStatus::Capacity);
    expectCapacityStatus(
        [](auto& capacity) { capacity.maxPieceSchedulePrograms = 0; },
        harken::WorldInstallStatus::Capacity);
    expectCapacityStatus(
        [](auto& capacity) { capacity.maxScheduleTrackRefsPerPiece = 0; },
        harken::WorldInstallStatus::Capacity);
    expectCapacityStatus(
        [](auto& capacity) { capacity.maxScheduleTrackRefsTotal = 0; },
        harken::WorldInstallStatus::Capacity);
    expectCapacityStatus(
        [](auto& capacity) { capacity.maxSchedulePlacementsPerTrack = 0; },
        harken::WorldInstallStatus::Capacity);
    expectCapacityStatus(
        [](auto& capacity) { capacity.maxSchedulePlacementsTotal = 0; },
        harken::WorldInstallStatus::Capacity);
    expectCapacityStatus(
        [](auto& capacity) { capacity.maxScheduleMarkersPerPiece = 0; },
        harken::WorldInstallStatus::Capacity);
    expectCapacityStatus(
        [](auto& capacity) { capacity.maxScheduleMarkersTotal = 0; },
        harken::WorldInstallStatus::Capacity);
    expectCapacityStatus(
        [](auto& capacity) { capacity.maxTrackPieceScheduleDepth = 1; },
        harken::WorldInstallStatus::Capacity);

    expectCapacityStatus(
        [](auto& capacity) {
            capacity.maxTrackSchedulePrograms =
                harken::kHardTrackScheduleProgramCapacity + 1;
        },
        harken::WorldInstallStatus::InvalidWorld);
    expectCapacityStatus(
        [](auto& capacity) {
            capacity.maxPieceSchedulePrograms =
                harken::kHardPieceScheduleProgramCapacity + 1;
        },
        harken::WorldInstallStatus::InvalidWorld);
    expectCapacityStatus(
        [](auto& capacity) {
            capacity.maxScheduleTrackRefsPerPiece = 1025;
        },
        harken::WorldInstallStatus::InvalidWorld);
    expectCapacityStatus(
        [](auto& capacity) {
            capacity.maxScheduleTrackRefsTotal =
                harken::kHardScheduleTrackRefCapacity + 1;
        },
        harken::WorldInstallStatus::InvalidWorld);
    expectCapacityStatus(
        [](auto& capacity) {
            capacity.maxSchedulePlacementsPerTrack = 65537;
        },
        harken::WorldInstallStatus::InvalidWorld);
    expectCapacityStatus(
        [](auto& capacity) {
            capacity.maxSchedulePlacementsTotal =
                harken::kHardSchedulePlacementCapacity + 1;
        },
        harken::WorldInstallStatus::InvalidWorld);
    expectCapacityStatus(
        [](auto& capacity) { capacity.maxScheduleMarkersPerPiece = 65537; },
        harken::WorldInstallStatus::InvalidWorld);
    expectCapacityStatus(
        [](auto& capacity) {
            capacity.maxScheduleMarkersTotal =
                harken::kHardScheduleMarkerCapacity + 1;
        },
        harken::WorldInstallStatus::InvalidWorld);
    expectCapacityStatus(
        [](auto& capacity) { capacity.maxTrackPieceScheduleDepth = 65; },
        harken::WorldInstallStatus::InvalidWorld);
    world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(world);

    const harken::WorldInstallResult installed =
        harken::InstallContainerWorldV1(*session.Engine(), world);
    ASSERT_EQ(harken::WorldInstallStatus::Ok, installed.status);
    const harken::WorldInstallResult repeated =
        harken::InstallContainerWorldV1(*session.Engine(), world);
    EXPECT_EQ(harken::WorldInstallStatus::Ok, repeated.status);
    EXPECT_TRUE(repeated.idempotent);
    EXPECT_EQ(installed.resultGeneration, repeated.resultGeneration);

    placement.authoredStart = 999;
    trackRef.trackScheduleSemanticFingerprint = 0;
    const auto current = session.Engine()->variables.CurrentWorldInstallView();
    ASSERT_EQ(1u, current.trackScheduleCount);
    ASSERT_EQ(1u, current.pieceScheduleCount);
    ASSERT_EQ(1u, current.trackSchedules[0].placementCount);
    EXPECT_EQ(500000000, current.trackSchedules[0].placements[0].authoredStart);
    ASSERT_EQ(1u, current.pieceSchedules[0].trackRefCount);
    EXPECT_EQ(track.scheduleSemanticFingerprint,
              current.pieceSchedules[0]
                  .trackRefs[0]
                  .trackScheduleSemanticFingerprint);

    harken::ScheduleClipPlacementV1 placements[2] = {
        current.trackSchedules[0].placements[0],
        current.trackSchedules[0].placements[0],
    };
    placements[1].stablePlacementOrdinal = 1;
    placements[1].stableOccurrenceOrdinal = 8;
    placements[1].clipId = harkentest::ObjectId(90);
    placements[1].clipGuidLow = 93;
    harken::PlayableOccurrencePropertyViewV1 occurrences[2] = {
        occurrence, occurrence,
    };
    occurrences[1].stableOccurrenceOrdinal = 8;
    occurrences[1].occurrenceSemanticFingerprint =
        harken::ComputePlayableOccurrenceFingerprintV1(occurrences[1]);
    track.placementCount = 2;
    track.placements = placements;
    track.scheduleSemanticFingerprint =
        harken::ComputeTrackScheduleFingerprintV1(track);
    trackRef.trackScheduleSemanticFingerprint =
        track.scheduleSemanticFingerprint;
    piece.scheduleSemanticFingerprint =
        harken::ComputePieceScheduleFingerprintV1(piece);
    world.playableOccurrences = occurrences;
    world.playableOccurrenceCount = 2;
    const auto expectRejectedWithoutMutation = [&](auto mutate, auto restore) {
        mutate();
        track.scheduleSemanticFingerprint =
            harken::ComputeTrackScheduleFingerprintV1(track);
        trackRef.trackScheduleSemanticFingerprint =
            track.scheduleSemanticFingerprint;
        piece.scheduleSemanticFingerprint =
            harken::ComputePieceScheduleFingerprintV1(piece);
        world.identity.runtimeSemanticFingerprint =
            harken::ComputeCurrentWorldFingerprintV4(world);
        const auto generation =
            session.Engine()->variables.CurrentWorldGeneration();
        EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
                  harken::InstallContainerWorldV1(*session.Engine(), world)
                      .status);
        EXPECT_EQ(generation,
                  session.Engine()->variables.CurrentWorldGeneration());
        restore();
    };
    expectRejectedWithoutMutation(
        [&] { placements[1].stableOccurrenceOrdinal = 7; },
        [&] { placements[1].stableOccurrenceOrdinal = 8; });
    expectRejectedWithoutMutation(
        [&] { track.placementCount = 1; },
        [&] { track.placementCount = 2; });
    expectRejectedWithoutMutation(
        [&] { placements[1].authoredStart = placements[0].authoredStart - 1; },
        [&] { placements[1].authoredStart = placements[0].authoredStart; });
    expectRejectedWithoutMutation(
        [&] { placements[1].clipId = placements[0].clipId; },
        [&] { placements[1].clipId = harkentest::ObjectId(90); });
    expectRejectedWithoutMutation(
        [&] {
            placements[1].clipGuidHigh = placements[0].clipGuidHigh;
            placements[1].clipGuidLow = placements[0].clipGuidLow;
        },
        [&] {
            placements[1].clipGuidHigh = 0;
            placements[1].clipGuidLow = 93;
        });
    expectRejectedWithoutMutation(
        [&] {
            placements[1].content = track.rootDefinition;
            occurrences[1].childPlayable = placements[1].content;
            occurrences[1].occurrenceSemanticFingerprint =
                harken::ComputePlayableOccurrenceFingerprintV1(occurrences[1]);
        },
        [&] {
            placements[1].content = placements[0].content;
            occurrences[1].childPlayable = placements[1].content;
            occurrences[1].occurrenceSemanticFingerprint =
                harken::ComputePlayableOccurrenceFingerprintV1(occurrences[1]);
        });
    track.scheduleSemanticFingerprint =
        harken::ComputeTrackScheduleFingerprintV1(track);
    trackRef.trackScheduleSemanticFingerprint =
        track.scheduleSemanticFingerprint;
    piece.scheduleSemanticFingerprint =
        harken::ComputePieceScheduleFingerprintV1(piece);
    world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(world);
    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              harken::InstallContainerWorldV1(*session.Engine(), world).status);

    harken::PlayablePropertyProgramViewV1 ownedProperties[4] = {
        properties[0], properties[1], properties[2], properties[2],
    };
    ownedProperties[3].stableId = harkentest::ObjectId(40);
    ownedProperties[3].authoredStableOrder = 3;
    ownedProperties[3].contentRevision = 104;
    ownedProperties[3].programSemanticFingerprint =
        harken::ComputePlayablePropertyFingerprintV1(ownedProperties[3]);
    harken::PieceScheduleProgramViewV1 pieces[2] = {piece, piece};
    pieces[1].rootDefinition.stableId = ownedProperties[3].stableId;
    pieces[1].rootDefinition.expectedProgramSemanticFingerprint =
        ownedProperties[3].programSemanticFingerprint;
    pieces[1].rootDefinition.contentRevision =
        ownedProperties[3].contentRevision;
    pieces[1].scheduleSemanticFingerprint =
        harken::ComputePieceScheduleFingerprintV1(pieces[1]);
    world.playableProperties = ownedProperties;
    world.playablePropertyCount = 4;
    world.pieceSchedules = pieces;
    world.pieceScheduleCount = 2;
    world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(world);
    const std::uint64_t generation =
        session.Engine()->variables.CurrentWorldGeneration();
    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
              harken::InstallContainerWorldV1(*session.Engine(), world).status);
    EXPECT_EQ(generation,
              session.Engine()->variables.CurrentWorldGeneration());
}

TEST(CurrentWorldInstall, NonQueueChildRejectsScheduleCompletionSource) {
    RunnableWorld image;
    image.child.timingHint = harken::MakeScheduleReferenceTimingHint(
        harken::PlayableKindV1::Sound, image.child.playable.stableId, 1);
    image.world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(image.world);

    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
              harken::ValidateCurrentWorldV1(image.world));
}

TEST(CurrentWorldInstall, MarkerAlignedQueueRejectsSignalOnlyCompletion) {
    RunnableWorld image;
    image.container.mode = harken::ContainerModeV1::SequenceQueue;
    image.container.memoryScope = harken::MemoryScopeV1::Emitter;
    image.container.queueSplice = harken::QueueSpliceV1::MarkerAligned;
    image.child.frequencyWeight = 0;
    image.child.queueStopPolicy = harken::QueueStopPolicyV1::AdvanceQueue;
    image.world.identity.capabilityMask =
        harken::kWorldCapabilityPlayableProperties |
        harken::kWorldCapabilitySequenceQueue;
    image.RefreshWorldFingerprint();

    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
              harken::ValidateCurrentWorldV1(image.world));
}

TEST(CurrentWorldInstall, ClipIdentityIsUniqueAcrossTracksWithoutMutation) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    harken::PlayablePropertyProgramViewV1 properties[3]{};
    properties[0].contentPropertyKind =
        harken::playableproperties::ContentPropertyKindV1::Sound;
    properties[0].stableId = harkentest::ObjectId(10);
    properties[0].contentRevision = 100;
    properties[0].payload = ResolvedPayload();
    properties[0].programSemanticFingerprint =
        harken::ComputePlayablePropertyFingerprintV1(properties[0]);
    for (std::uint32_t i = 1; i < 3; ++i) {
        properties[i].contentPropertyKind =
            harken::playableproperties::ContentPropertyKindV1::Track;
        properties[i].stableId = harkentest::ObjectId(19 + i);
        properties[i].authoredStableOrder = i;
        properties[i].contentRevision = 100 + i;
        properties[i].payload = ResolvedPayload();
        properties[i].programSemanticFingerprint =
            harken::ComputePlayablePropertyFingerprintV1(properties[i]);
    }
    harken::ScheduleClipPlacementV1 placements[2]{};
    harken::PlayableOccurrencePropertyViewV1 occurrences[2]{};
    harken::TrackScheduleProgramViewV1 tracks[2]{};
    for (std::uint32_t i = 0; i < 2; ++i) {
        placements[i].stableOccurrenceOrdinal = 7;
        placements[i].content.kind = harken::PlayableKindV1::Sound;
        placements[i].content.stableId = properties[0].stableId;
        placements[i].content.expectedProgramSemanticFingerprint =
            properties[0].programSemanticFingerprint;
        placements[i].content.contentRevision =
            properties[0].contentRevision;
        placements[i].clipId = harkentest::ObjectId(50 + i);
        placements[i].clipGuidLow = 60 + i;
        occurrences[i].parentKind =
            harken::playableproperties::ContentPropertyKindV1::Track;
        occurrences[i].stableOccurrenceOrdinal = 7;
        occurrences[i].parentStableId = properties[i + 1].stableId;
        occurrences[i].parentProgramSemanticFingerprint =
            properties[i + 1].programSemanticFingerprint;
        occurrences[i].childPlayable = placements[i].content;
        occurrences[i].payload = CanonicalPayload();
        occurrences[i].occurrenceSemanticFingerprint =
            harken::ComputePlayableOccurrenceFingerprintV1(occurrences[i]);
        tracks[i].rootDefinition.kind = harken::PlayableKindV1::Track;
        tracks[i].rootDefinition.stableId = properties[i + 1].stableId;
        tracks[i].rootDefinition.expectedProgramSemanticFingerprint =
            properties[i + 1].programSemanticFingerprint;
        tracks[i].rootDefinition.contentRevision =
            properties[i + 1].contentRevision;
        tracks[i].placementCount = 1;
        tracks[i].placements = &placements[i];
        tracks[i].scheduleSemanticFingerprint =
            harken::ComputeTrackScheduleFingerprintV1(tracks[i]);
    }
    harken::ContainerWorldInstallViewV1 world{};
    world.playableProperties = properties;
    world.playablePropertyCount = 3;
    world.playableOccurrences = occurrences;
    world.playableOccurrenceCount = 2;
    world.trackSchedules = tracks;
    world.trackScheduleCount = 2;
    world.identity.capabilityMask = harken::kWorldCapabilityPlayableProperties;
    world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(world);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harken::InstallContainerWorldV1(*session.Engine(), world).status);
    const auto generation =
        session.Engine()->variables.CurrentWorldGeneration();
    const auto expectRejected = [&] {
        tracks[1].scheduleSemanticFingerprint =
            harken::ComputeTrackScheduleFingerprintV1(tracks[1]);
        world.identity.runtimeSemanticFingerprint =
            harken::ComputeCurrentWorldFingerprintV4(world);
        EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
                  harken::InstallContainerWorldV1(*session.Engine(), world)
                      .status);
        EXPECT_EQ(generation,
                  session.Engine()->variables.CurrentWorldGeneration());
    };
    placements[1].clipId = placements[0].clipId;
    expectRejected();
    placements[1].clipId = harkentest::ObjectId(51);
    placements[1].clipGuidLow = placements[0].clipGuidLow;
    expectRejected();
}

TEST(CurrentWorldInstall, ScheduleCycleIsInvalidWithoutAContainerRoot) {
    harken::PlayablePropertyProgramViewV1 properties[2]{};
    properties[0].contentPropertyKind =
        harken::playableproperties::ContentPropertyKindV1::Track;
    properties[0].stableId = harkentest::ObjectId(20);
    properties[0].contentRevision = 102;
    properties[0].payload = ResolvedPayload();
    properties[0].programSemanticFingerprint =
        harken::ComputePlayablePropertyFingerprintV1(properties[0]);
    properties[1].contentPropertyKind =
        harken::playableproperties::ContentPropertyKindV1::Piece;
    properties[1].authoredStableOrder = 1;
    properties[1].stableId = harkentest::ObjectId(30);
    properties[1].contentRevision = 103;
    properties[1].payload = ResolvedPayload();
    properties[1].programSemanticFingerprint =
        harken::ComputePlayablePropertyFingerprintV1(properties[1]);

    harken::ScheduleClipPlacementV1 placement{};
    placement.stableOccurrenceOrdinal = 7;
    placement.content.kind = harken::PlayableKindV1::Piece;
    placement.content.stableId = properties[1].stableId;
    placement.content.expectedProgramSemanticFingerprint =
        properties[1].programSemanticFingerprint;
    placement.content.contentRevision = properties[1].contentRevision;
    placement.clipId = harkentest::ObjectId(91);
    placement.clipGuidLow = 92;
    harken::PlayableOccurrencePropertyViewV1 occurrence{};
    occurrence.parentKind =
        harken::playableproperties::ContentPropertyKindV1::Track;
    occurrence.stableOccurrenceOrdinal = placement.stableOccurrenceOrdinal;
    occurrence.parentStableId = properties[0].stableId;
    occurrence.parentProgramSemanticFingerprint =
        properties[0].programSemanticFingerprint;
    occurrence.childPlayable = placement.content;
    occurrence.payload = CanonicalPayload();
    occurrence.occurrenceSemanticFingerprint =
        harken::ComputePlayableOccurrenceFingerprintV1(occurrence);

    harken::TrackScheduleProgramViewV1 track{};
    track.rootDefinition.kind = harken::PlayableKindV1::Track;
    track.rootDefinition.stableId = properties[0].stableId;
    track.rootDefinition.expectedProgramSemanticFingerprint =
        properties[0].programSemanticFingerprint;
    track.rootDefinition.contentRevision = properties[0].contentRevision;
    track.placementCount = 1;
    track.placements = &placement;
    track.scheduleSemanticFingerprint =
        harken::ComputeTrackScheduleFingerprintV1(track);
    harken::ScheduleTrackRefV1 trackRef{};
    trackRef.trackDefinition = track.rootDefinition;
    trackRef.trackScheduleSemanticFingerprint =
        track.scheduleSemanticFingerprint;
    harken::PieceScheduleProgramViewV1 piece{};
    piece.rootDefinition.kind = harken::PlayableKindV1::Piece;
    piece.rootDefinition.stableId = properties[1].stableId;
    piece.rootDefinition.expectedProgramSemanticFingerprint =
        properties[1].programSemanticFingerprint;
    piece.rootDefinition.contentRevision = properties[1].contentRevision;
    piece.trackRefCount = 1;
    piece.trackRefs = &trackRef;
    piece.scheduleSemanticFingerprint =
        harken::ComputePieceScheduleFingerprintV1(piece);

    harken::ContainerWorldInstallViewV1 world{};
    world.playableProperties = properties;
    world.playablePropertyCount = 2;
    world.playableOccurrences = &occurrence;
    world.playableOccurrenceCount = 1;
    world.trackSchedules = &track;
    world.trackScheduleCount = 1;
    world.pieceSchedules = &piece;
    world.pieceScheduleCount = 1;
    world.identity.capabilityMask = harken::kWorldCapabilityPlayableProperties;
    world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(world);

    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
              harken::ValidateCurrentWorldV1(world));
}

TEST(CurrentWorldInstall, PieceMarkerIdsAreUniqueBeyondCanonicalPairOrder) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    harken::PlayablePropertyProgramViewV1 property{};
    property.contentPropertyKind =
        harken::playableproperties::ContentPropertyKindV1::Piece;
    property.stableId = harkentest::ObjectId(30);
    property.contentRevision = 103;
    property.payload = ResolvedPayload();
    property.programSemanticFingerprint =
        harken::ComputePlayablePropertyFingerprintV1(property);
    harken::ScheduleMarkerV1 markers[2] = {{harkentest::ObjectId(41), 7, 0, 100}, {}};
    harken::PieceScheduleProgramViewV1 piece{};
    piece.rootDefinition.kind = harken::PlayableKindV1::Piece;
    piece.rootDefinition.stableId = property.stableId;
    piece.rootDefinition.expectedProgramSemanticFingerprint =
        property.programSemanticFingerprint;
    piece.rootDefinition.contentRevision = property.contentRevision;
    piece.markerCount = 1;
    piece.markers = markers;
    piece.scheduleSemanticFingerprint =
        harken::ComputePieceScheduleFingerprintV1(piece);
    harken::ContainerWorldInstallViewV1 world{};
    world.playableProperties = &property;
    world.playablePropertyCount = 1;
    world.pieceSchedules = &piece;
    world.pieceScheduleCount = 1;
    world.identity.capabilityMask = harken::kWorldCapabilityPlayableProperties;
    world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(world);

    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harken::InstallContainerWorldV1(*session.Engine(), world).status);
    const auto generation =
        session.Engine()->variables.CurrentWorldGeneration();
    const auto expectRejected = [&] {
        piece.markerCount = 2;
        piece.scheduleSemanticFingerprint =
            harken::ComputePieceScheduleFingerprintV1(piece);
        world.identity.runtimeSemanticFingerprint =
            harken::ComputeCurrentWorldFingerprintV4(world);
        EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
                  harken::InstallContainerWorldV1(*session.Engine(), world)
                      .status);
        EXPECT_EQ(generation,
                  session.Engine()->variables.CurrentWorldGeneration());
    };

    markers[1] = {harkentest::ObjectId(42), 7, 1, 200};
    expectRejected();
    markers[1] = {harkentest::ObjectId(41), 8, 1, 200};
    expectRejected();
}

TEST(CurrentWorldInstall, VintageTwoReplayKeepsBothQueueBitsUnavailable) {
    for (const auto [mode, capability] : {
             std::pair{harken::ContainerModeV1::RandomQueue,
                       harken::kWorldCapabilityRandomQueue},
             std::pair{harken::ContainerModeV1::SequenceQueue,
                       harken::kWorldCapabilitySequenceQueue}}) {
        RunnableWorld image;
        image.properties[0].contentPropertyKind =
            harken::playableproperties::ContentPropertyKindV1::Track;
        image.properties[0].programSemanticFingerprint =
            harken::ComputePlayablePropertyFingerprintV1(image.properties[0]);
        image.child.playable.kind = harken::PlayableKindV1::Track;
        image.child.playable.expectedProgramSemanticFingerprint =
            image.properties[0].programSemanticFingerprint;
        image.occurrence.childPlayable = image.child.playable;
        image.occurrence.occurrenceSemanticFingerprint =
            harken::ComputePlayableOccurrenceFingerprintV1(image.occurrence);
        std::swap(image.properties[0], image.properties[1]);
        harken::AuthoredCompletionLeafV1 boundedLeaf{};
        boundedLeaf.stableLeafOrdinal = 1;
        boundedLeaf.content = image.child.playable;
        boundedLeaf.startOffset = {
            harken::AuthoredTimeDomainV1::AbsoluteNanoseconds, 0, 1};
        boundedLeaf.duration = {
            harken::AuthoredTimeDomainV1::AbsoluteNanoseconds, 1, 1};
        harkentest::CopyLeafTimingToHint(image.child, boundedLeaf);
        image.container.mode = mode;
        image.child.frequencyWeight = 0;
        image.child.orderWeight =
            mode == harken::ContainerModeV1::RandomQueue ? 1u : 0u;
        image.world.identity.capabilityMask =
            harken::kWorldCapabilityPlayableProperties | capability;
        image.historicalChild = {};
        image.historicalChild.playable = image.child.playable;
        image.historicalChild.stableChildOrdinal = image.child.stableChildOrdinal;
        image.historicalChild.authoredStableOrder = image.child.authoredStableOrder;
        image.historicalChild.frequencyWeight = image.child.frequencyWeight;
        image.historicalChild.orderWeight = image.child.orderWeight;
        image.historicalChild.queueStopPolicy = image.child.queueStopPolicy;
        image.historicalLeaf = boundedLeaf;
        image.historicalChild.completion.source =
            harken::CompletionSourceV1::InlineTemplate;
        image.historicalChild.completion.payload.inlineTemplate.completionClass =
            harken::AuthoredCompletionClassV1::BoundedTrackPieceCandidate;
        image.historicalChild.completion.payload.inlineTemplate.leafCount = 1;
        image.historicalChild.completion.payload.inlineTemplate.leaves =
            &image.historicalLeaf;
        image.historicalContainer = {};
        image.historicalContainer.containerStableId =
            image.container.containerStableId;
        image.historicalContainer.programSemanticFingerprint =
            image.container.programSemanticFingerprint;
        image.historicalContainer.mode = image.container.mode;
        image.historicalContainer.randomDiscipline =
            image.container.randomDiscipline;
        image.historicalContainer.memoryScope = image.container.memoryScope;
        image.historicalContainer.childCount = 1;
        image.historicalContainer.children = &image.historicalChild;
        auto historical = harkentest::AsHistoricalPrefix(image.world);
        historical.containers = &image.historicalContainer;
        historical.containerCount = 1;
        historical.identity.runtimeSemanticFingerprint =
            harken::ComputeCurrentWorldFingerprintV2(historical);
        image.world.identity.runtimeSemanticFingerprint =
            historical.identity.runtimeSemanticFingerprint;
        EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
                  harken::ValidateCurrentWorldV1(image.world));
        EXPECT_EQ(harken::WorldInstallStatus::CapabilityUnavailable,
                  harken::ValidateCurrentWorldV2Replay(historical));
        harkentest::EngineSession session(harkentest::MakeNullConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        EXPECT_EQ(harken::WorldInstallStatus::CapabilityUnavailable,
                  harken::InstallContainerWorldV2Replay(
                      *session.Engine(), historical).status);
        EXPECT_EQ(0u,
                  session.Engine()->variables.CurrentWorldGeneration());
    }
}

TEST(CurrentWorldInstall, VintageThreeInstallsBothQueueCapabilitiesExactly) {
    for (const auto [mode, capability] : {
             std::pair{harken::ContainerModeV1::RandomQueue,
                       harken::kWorldCapabilityRandomQueue},
             std::pair{harken::ContainerModeV1::SequenceQueue,
                       harken::kWorldCapabilitySequenceQueue}}) {
        RunnableWorld image;
        harken::AuthoredCompletionLeafV1 leaf{};
        leaf.stableLeafOrdinal = 1;
        leaf.content = image.child.playable;
        leaf.startOffset = {
            harken::AuthoredTimeDomainV1::SourceFrames, 0, 1};
        leaf.duration = {
            harken::AuthoredTimeDomainV1::NaturalMediaEnd, 0, 1};
        harkentest::CopyLeafTimingToHint(image.child, leaf);
        image.container.mode = mode;
        image.container.memoryScope = harken::MemoryScopeV1::Emitter;
        image.child.frequencyWeight = 0;
        image.child.orderWeight =
            mode == harken::ContainerModeV1::RandomQueue ? 1u : 0u;
        image.world.identity.capabilityMask =
            harken::kWorldCapabilityPlayableProperties | capability;
        image.RefreshWorldFingerprint();

        EXPECT_EQ(harken::WorldInstallStatus::Ok,
                  harken::ValidateCurrentWorldV1(image.world));
        harkentest::EngineSession session(harkentest::MakeNullConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        const auto installed =
            harken::InstallContainerWorldV1(*session.Engine(), image.world);
        EXPECT_EQ(harken::WorldInstallStatus::Ok, installed.status);
        EXPECT_NE(0u, installed.resultGeneration);
    }
}

TEST(CurrentWorldInstall,
     VariableControlGridShapeIsCanonicalAndInstalls) {
    RunnableWorld image;
    harken::VariableDefinitionV1 variable{};
    variable.variableId = harkentest::ObjectId(30);
    variable.kind = harken::VariableKindV1::Step;
    variable.rangeMin = 0.0f;
    variable.rangeMax = 10.0f;
    variable.defaultValue = 1.0f;

    harken::GridAxisV1 axis{};
    axis.stableAxisOrdinal = 0;
    axis.authoredColumnOrder = 0;
    axis.variableId = variable.variableId;

    harken::DiscreteGridCellV1 cell{};
    cell.kind = harken::DiscreteCellKindV1::Exact;
    cell.exactValue = 7;

    harken::DiscreteGridRowViewV1 row{};
    row.stableRowOrdinal = 0;
    row.authoredStableOrder = 0;
    row.childStableOrdinal = image.child.stableChildOrdinal;
    row.cellCount = 1;
    row.cells = &cell;

    image.child.frequencyWeight = 0;
    image.container.mode = harken::ContainerModeV1::VariableControl;
    image.container.discreteAxisCount = 1;
    image.container.discreteAxes = &axis;
    image.container.discreteRowCount = 1;
    image.container.discreteRows = &row;
    image.world.variables = &variable;
    image.world.variableCount = 1;
    image.world.identity.capabilityMask =
        harken::kWorldCapabilityVariableDefinitions |
        harken::kWorldCapabilityPlayableProperties |
        harken::kWorldCapabilityVariableControl;
    image.RefreshWorldFingerprint();

    const auto originalFingerprint =
        image.world.identity.runtimeSemanticFingerprint;
    ASSERT_NE(0u, originalFingerprint);
    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              harken::ValidateCurrentWorldV1(image.world));

    axis.variableId = harkentest::ObjectId(31);
    image.RefreshWorldFingerprint();
    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
              harken::ValidateCurrentWorldV1(image.world));

    axis.variableId = variable.variableId;
    axis.authoredColumnOrder = 1;
    image.RefreshWorldFingerprint();
    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
              harken::ValidateCurrentWorldV1(image.world));

    axis.authoredColumnOrder = 0;
    cell.exactValue = 8;
    image.RefreshWorldFingerprint();
    EXPECT_NE(originalFingerprint,
              image.world.identity.runtimeSemanticFingerprint);
    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              harken::ValidateCurrentWorldV1(image.world));

    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto installed =
        harken::InstallContainerWorldV1(*session.Engine(), image.world);
    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              installed.status);
    EXPECT_NE(0u, installed.resultGeneration);
    EXPECT_EQ(installed.resultGeneration,
              session.Engine()->variables.CurrentWorldGeneration());
}

TEST(CurrentWorldInstall,
     BlendEnvelopeAcceptsRepeatedChildRowsWithSharedCapacity) {
    RunnableWorld image;
    harken::VariableDefinitionV1 variable{};
    variable.variableId = harkentest::ObjectId(30);
    variable.kind = harken::VariableKindV1::Smooth;
    variable.rangeMin = 0.0f;
    variable.rangeMax = 10.0f;
    variable.defaultValue = 1.0f;
    harken::GridAxisV1 axis{0, 0, variable.variableId};
    harken::ContinuousGridCellV1 cells[2]{};
    cells[0].kind = harken::ContinuousCellKindV1::Interval;
    cells[0].lower = 0.0f;
    cells[0].upper = 4.0f;
    cells[0].lowerInclusive = 1;
    cells[0].upperInclusive = 1;
    cells[1] = cells[0];
    cells[1].lower = 6.0f;
    cells[1].upper = 10.0f;
    harken::ContinuousGridRowViewV1 rows[2]{};
    for (std::uint32_t i = 0; i < 2; ++i) {
        rows[i].stableRowOrdinal = i;
        rows[i].authoredStableOrder = i;
        rows[i].childStableOrdinal = image.child.stableChildOrdinal;
        rows[i].cellCount = 1;
        rows[i].cells = &cells[i];
    }
    image.child.frequencyWeight = 0;
    image.container.mode = harken::ContainerModeV1::VariableControl;
    image.container.continuousAxisCount = 1;
    image.container.continuousAxes = &axis;
    image.container.continuousRowCount = 2;
    image.container.continuousRows = rows;
    image.container.evaluationPeriodFrames = 64;
    image.container.envelopeFrames = 32;
    image.container.maxActiveRows = 1;
    image.container.maxLiveRows = 2;
    image.world.variables = &variable;
    image.world.variableCount = 1;
    image.world.identity.capabilityMask =
        harken::kWorldCapabilityVariableDefinitions |
        harken::kWorldCapabilityPlayableProperties |
        harken::kWorldCapabilityVariableControl;
    image.RefreshWorldFingerprint();

    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              harken::ValidateCurrentWorldV1(image.world));
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              harken::InstallContainerWorldV1(*session.Engine(), image.world)
                  .status);
    EXPECT_NE(0u, session.Engine()->variables.CurrentWorldGeneration());
}

TEST(CurrentWorldInstall,
     BlendRepeatedChildRowsMayOverlapAndShareMemberCapacity) {
    RunnableWorld image;
    harken::VariableDefinitionV1 variable{};
    variable.variableId = harkentest::ObjectId(30);
    variable.kind = harken::VariableKindV1::Smooth;
    variable.rangeMin = 0.0f;
    variable.rangeMax = 10.0f;
    variable.defaultValue = 1.0f;
    harken::GridAxisV1 axis{0, 0, variable.variableId};
    harken::ContinuousGridCellV1 cells[2]{};
    for (auto& cell : cells) {
        cell.kind = harken::ContinuousCellKindV1::Interval;
        cell.lowerInclusive = 1;
        cell.upperInclusive = 1;
    }
    cells[0].lower = 0.0f;
    cells[0].upper = 4.0f;
    cells[1].lower = 6.0f;
    cells[1].upper = 10.0f;
    harken::ContinuousGridRowViewV1 rows[2]{};
    for (std::uint32_t i = 0; i < 2; ++i) {
        rows[i].stableRowOrdinal = i;
        rows[i].authoredStableOrder = i;
        rows[i].childStableOrdinal = image.child.stableChildOrdinal;
        rows[i].cellCount = 1;
        rows[i].cells = &cells[i];
    }
    image.child.frequencyWeight = 0;
    image.container.mode = harken::ContainerModeV1::VariableControl;
    image.container.continuousAxisCount = 1;
    image.container.continuousAxes = &axis;
    image.container.continuousRowCount = 2;
    image.container.continuousRows = rows;
    image.container.evaluationPeriodFrames = 64;
    image.container.maxActiveRows = 1;
    image.container.maxLiveRows = 1;
    image.world.variables = &variable;
    image.world.variableCount = 1;
    image.world.identity.capabilityMask =
        harken::kWorldCapabilityVariableDefinitions |
        harken::kWorldCapabilityPlayableProperties |
        harken::kWorldCapabilityVariableControl;
    image.RefreshWorldFingerprint();

    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              harken::ValidateCurrentWorldV1(image.world));
    cells[1].lower = cells[0].upper;
    cells[0].upperInclusive = 0;
    cells[1].lowerInclusive = 0;
    image.RefreshWorldFingerprint();
    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              harken::ValidateCurrentWorldV1(image.world));
}

TEST(CurrentWorldInstall,
     VariableControlContinuousCurveReferencesAreLocalActiveAndUnique) {
    RunnableWorld image;
    harken::VariableDefinitionV1 variable{};
    variable.variableId = harkentest::ObjectId(30);
    variable.kind = harken::VariableKindV1::Smooth;
    variable.rangeMin = 0.0f;
    variable.rangeMax = 10.0f;
    variable.defaultValue = 1.0f;
    harken::GridAxisV1 axis{0, 0, variable.variableId};
    harken::ContinuousGridCellV1 cell{};
    cell.kind = harken::ContinuousCellKindV1::Interval;
    cell.lower = 0.0f;
    cell.upper = 10.0f;
    cell.lowerInclusive = 1;
    cell.upperInclusive = 1;
    cell.leftCurveLocalId = 42;
    harken::ContinuousGridRowViewV1 row{};
    row.childStableOrdinal = image.child.stableChildOrdinal;
    row.cellCount = 1;
    row.cells = &cell;
    harken::CurveV1 curves[2]{};
    for (std::uint32_t i = 0; i < 2; ++i) {
        curves[i].curveLocalId = 42 + i;
        curves[i].curve.pointCount = 2;
        curves[i].curve.points[0] = {0.0f, 0.0f};
        curves[i].curve.points[1] = {10.0f, 1.0f};
    }
    image.child.frequencyWeight = 0;
    image.container.mode = harken::ContainerModeV1::VariableControl;
    image.container.continuousAxisCount = 1;
    image.container.continuousAxes = &axis;
    image.container.continuousRowCount = 1;
    image.container.continuousRows = &row;
    image.container.evaluationPeriodFrames = 64;
    image.container.maxActiveRows = 1;
    image.container.maxLiveRows = 1;
    image.world.variables = &variable;
    image.world.variableCount = 1;
    image.world.identity.capabilityMask =
        harken::kWorldCapabilityVariableDefinitions |
        harken::kWorldCapabilityPlayableProperties |
        harken::kWorldCapabilityVariableControl;

    image.RefreshWorldFingerprint();
    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
              harken::ValidateCurrentWorldV1(image.world));

    image.container.curveCount = 1;
    image.container.curves = curves;
    image.RefreshWorldFingerprint();
    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              harken::ValidateCurrentWorldV1(image.world));

    curves[0].curve.pointCount = 1;
    image.RefreshWorldFingerprint();
    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
              harken::ValidateCurrentWorldV1(image.world));
    curves[0].curve.pointCount = 2;

    cell.rightCurveLocalId = curves[0].curveLocalId;
    image.RefreshWorldFingerprint();
    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
              harken::ValidateCurrentWorldV1(image.world));

    cell.rightCurveLocalId = UINT32_MAX;
    image.container.curveCount = 2;
    image.RefreshWorldFingerprint();
    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
              harken::ValidateCurrentWorldV1(image.world));
}

TEST(CurrentWorldInstall,
     TransitionAndContinuousCurvesShareOneValidatedContainerTable) {
    RunnableWorld image;
    harken::CurveV1 curves[2]{};
    for (std::uint32_t i = 0; i < 2; ++i) {
        curves[i].curveLocalId = 42 + i;
        curves[i].curve.pointCount = 2;
        curves[i].curve.points[0] = {0.0f, 0.0f};
        curves[i].curve.points[1] = {10.0f, 1.0f};
    }
    harken::TransitionRuleViewV1 transitionRule{};
    transitionRule.sourceAny = 1;
    transitionRule.targetAny = 1;
    transitionRule.sourceFade.active = 1;
    transitionRule.sourceFade.durationFrames = 64;
    transitionRule.sourceFade.curveLocalId = curves[0].curveLocalId;
    transitionRule.bridgeFadeIn.curveLocalId = UINT32_MAX;
    transitionRule.bridgeFadeOut.curveLocalId = UINT32_MAX;
    transitionRule.targetFade.curveLocalId = UINT32_MAX;
    harken::TransitionProgramViewV1 transition{};
    transition.ownerContainerStableId = image.container.containerStableId;
    transition.ruleCount = 1;
    transition.rules = &transitionRule;
    harken::AuthoredCompletionLeafV1 completionLeaf{};
    completionLeaf.stableLeafOrdinal = 1;
    completionLeaf.content = image.child.playable;
    completionLeaf.startOffset = {
        harken::AuthoredTimeDomainV1::SourceFrames, 0, 1};
    completionLeaf.duration = {
        harken::AuthoredTimeDomainV1::NaturalMediaEnd, 0, 1};
    harkentest::CopyLeafTimingToHint(image.child, completionLeaf);
    image.container.mode = harken::ContainerModeV1::SequenceQueue;
    image.container.memoryScope = harken::MemoryScopeV1::Emitter;
    image.container.transitionProgramIndex = 0;
    image.container.curveCount = 1;
    image.container.curves = curves;
    image.child.frequencyWeight = 0;
    image.world.transitions = &transition;
    image.world.transitionCount = 1;
    image.world.identity.capabilityMask =
        harken::kWorldCapabilityPlayableProperties |
        harken::kWorldCapabilitySequenceQueue |
        harken::kWorldCapabilityTransition;
    const auto expect = [&](harken::WorldInstallStatus status,
                            const char* caseName) {
        image.RefreshWorldFingerprint();
        EXPECT_EQ(status, harken::ValidateCurrentWorldV1(image.world))
            << caseName;
    };

    ASSERT_EQ(harken::TransitionProgramClassV1::Basic,
              harken::ClassifyTransitionProgramV1(transition));
    const harken::TransitionFadeV1 sourceFade = transitionRule.sourceFade;
    transitionRule.sourceFade = {};
    image.container.curveCount = 0;
    image.container.curves = nullptr;
    expect(harken::WorldInstallStatus::Ok, "canonical transition baseline");
    transitionRule.sourceFade = sourceFade;
    image.container.curveCount = 1;
    image.container.curves = curves;
    expect(harken::WorldInstallStatus::Ok, "transition curve reference");

    transitionRule.sourceFade.curveLocalId = UINT32_MAX;
    image.container.curveCount = 0;
    image.container.curves = nullptr;
    expect(harken::WorldInstallStatus::InvalidWorld,
           "active transition fade requires a curve");
    transitionRule.sourceFade.curveLocalId = curves[0].curveLocalId;
    image.container.curveCount = 1;
    image.container.curves = curves;

    transitionRule.sourceFade.curveLocalId = 99;
    expect(harken::WorldInstallStatus::InvalidWorld, "missing curve reference");
    transitionRule.sourceFade.curveLocalId = curves[0].curveLocalId;

    transitionRule.targetFade.active = 1;
    transitionRule.targetFade.durationFrames = 64;
    transitionRule.targetFade.curveLocalId = curves[0].curveLocalId;
    expect(harken::WorldInstallStatus::InvalidWorld, "duplicate curve reference");
    transitionRule.targetFade = {};
    transitionRule.targetFade.curveLocalId = UINT32_MAX;

    image.container.curveCount = 2;
    expect(harken::WorldInstallStatus::InvalidWorld, "unused curve");
    image.container.curveCount = 1;

    curves[0].curve.pointCount = 1;
    expect(harken::WorldInstallStatus::InvalidWorld, "one-point curve");
    curves[0].curve.pointCount = 2;

    harken::VariableDefinitionV1 variable{};
    variable.variableId = harkentest::ObjectId(30);
    variable.kind = harken::VariableKindV1::Smooth;
    variable.rangeMin = 0.0f;
    variable.rangeMax = 10.0f;
    variable.defaultValue = 1.0f;
    harken::GridAxisV1 axis{0, 0, variable.variableId};
    harken::GridAxisV1 discreteAxis{0, 0, variable.variableId};
    harken::DiscreteGridCellV1 discreteCell{
        harken::DiscreteCellKindV1::Exact, 1};
    harken::DiscreteGridRowViewV1 discreteRow{};
    discreteRow.childStableOrdinal = image.child.stableChildOrdinal;
    discreteRow.cellCount = 1;
    discreteRow.cells = &discreteCell;
    harken::ContinuousGridCellV1 cell{};
    cell.kind = harken::ContinuousCellKindV1::Interval;
    cell.lower = 0.0f;
    cell.upper = 10.0f;
    cell.lowerInclusive = 1;
    cell.upperInclusive = 1;
    cell.leftCurveLocalId = curves[1].curveLocalId;
    harken::ContinuousGridRowViewV1 row{};
    row.childStableOrdinal = image.child.stableChildOrdinal;
    row.cellCount = 1;
    row.cells = &cell;
    image.child.timingHint = {};
    image.container.mode = harken::ContainerModeV1::VariableControl;
    image.container.curveCount = 2;
    image.container.discreteAxisCount = 1;
    image.container.discreteAxes = &discreteAxis;
    image.container.discreteRowCount = 1;
    image.container.discreteRows = &discreteRow;
    image.container.discreteTiming = 1;
    image.container.continuousAxisCount = 1;
    image.container.continuousAxes = &axis;
    image.container.continuousRowCount = 1;
    image.container.continuousRows = &row;
    image.container.evaluationPeriodFrames = 64;
    image.container.maxActiveRows = 1;
    image.container.maxLiveRows = 1;
    image.world.variables = &variable;
    image.world.variableCount = 1;
    image.world.identity.capabilityMask =
        harken::kWorldCapabilityVariableDefinitions |
        harken::kWorldCapabilityPlayableProperties |
        harken::kWorldCapabilityVariableControl |
        harken::kWorldCapabilityTransition;
    expect(harken::WorldInstallStatus::Ok,
           "transition and continuous curves share the table");
}

TEST(CurrentWorldInstall,
     VariableControlInactiveTimingCapsAndTransitionFieldsAreCanonical) {
    RunnableWorld image;
    harken::VariableDefinitionV1 variable{};
    variable.variableId = harkentest::ObjectId(30);
    variable.kind = harken::VariableKindV1::Step;
    variable.rangeMin = 0.0f;
    variable.rangeMax = 10.0f;
    variable.defaultValue = 1.0f;
    harken::GridAxisV1 axis{0, 0, variable.variableId};
    harken::DiscreteGridCellV1 cell{harken::DiscreteCellKindV1::Exact, 1};
    harken::DiscreteGridRowViewV1 row{};
    row.childStableOrdinal = image.child.stableChildOrdinal;
    row.cellCount = 1;
    row.cells = &cell;
    image.child.frequencyWeight = 0;
    image.container.mode = harken::ContainerModeV1::VariableControl;
    image.container.discreteAxisCount = 1;
    image.container.discreteAxes = &axis;
    image.container.discreteRowCount = 1;
    image.container.discreteRows = &row;
    image.world.variables = &variable;
    image.world.variableCount = 1;
    image.world.identity.capabilityMask =
        harken::kWorldCapabilityVariableDefinitions |
        harken::kWorldCapabilityPlayableProperties |
        harken::kWorldCapabilityVariableControl;
    const auto expect = [&](harken::WorldInstallStatus status) {
        image.RefreshWorldFingerprint();
        EXPECT_EQ(status, harken::ValidateCurrentWorldV1(image.world));
    };

    expect(harken::WorldInstallStatus::Ok);
    image.container.evaluationPeriodFrames = 1;
    expect(harken::WorldInstallStatus::InvalidWorld);
    image.container.evaluationPeriodFrames = 0;
    image.container.discreteTiming = 2;
    expect(harken::WorldInstallStatus::InvalidWorld);
    image.container.discreteTiming = 0;
    image.container.continuousNormalization = 1;
    expect(harken::WorldInstallStatus::InvalidWorld);
    image.container.continuousNormalization = 0;
    image.container.envelopeFrames = 1;
    expect(harken::WorldInstallStatus::InvalidWorld);
    image.container.envelopeFrames = 0;
    image.container.maxActiveRows = 1;
    expect(harken::WorldInstallStatus::InvalidWorld);
    image.container.maxActiveRows = 0;
    image.container.queueCycleCount = 1;
    expect(harken::WorldInstallStatus::InvalidWorld);
    image.container.queueCycleCount = 0;
    image.container.transitionProgramIndex = 0;
    expect(harken::WorldInstallStatus::InvalidWorld);

    harken::TransitionRuleViewV1 transitionRule{};
    transitionRule.sourceAny = 1;
    transitionRule.targetAny = 1;
    transitionRule.bridgeFadeIn.curveLocalId = UINT32_MAX;
    transitionRule.bridgeFadeOut.curveLocalId = UINT32_MAX;
    harken::TransitionProgramViewV1 transition{};
    transition.ownerContainerStableId = image.container.containerStableId;
    transition.ruleCount = 1;
    transition.rules = &transitionRule;
    image.container.discreteTiming = 1;
    image.container.evaluationPeriodFrames = 64;
    image.world.transitions = &transition;
    image.world.transitionCount = 1;
    image.world.identity.capabilityMask |= harken::kWorldCapabilityTransition;
    expect(harken::WorldInstallStatus::Ok);
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto installed =
        harken::InstallContainerWorldV1(*session.Engine(), image.world);
    EXPECT_EQ(harken::WorldInstallStatus::Ok, installed.status);
    EXPECT_NE(0u, installed.resultGeneration);
    EXPECT_EQ(installed.resultGeneration,
              session.Engine()->variables.CurrentWorldGeneration());
}

TEST(CurrentWorldInstall,
     TypedImmediateTransitionIsValidatedAndAffectsWorldFingerprint) {
    RunnableWorld image;
    image.container.mode = harken::ContainerModeV1::SequenceQueue;
    image.child.frequencyWeight = 0;
    image.container.transitionProgramIndex = 0;
    image.world.identity.capabilityMask =
        harken::kWorldCapabilityPlayableProperties |
        harken::kWorldCapabilitySequenceQueue |
        harken::kWorldCapabilityTransition;

    harken::CurveV1 curves[2]{};
    for (std::uint32_t i = 0; i < 2; ++i) {
        curves[i].curveLocalId = i + 1u;
        curves[i].curve.pointCount = 2;
        curves[i].curve.points[0] = {0.0f, 0.0f};
        curves[i].curve.points[1] = {1.0f, 1.0f};
        curves[i].curve.handles[0] = {
            1.0f / 3.0f, 1.0f / 3.0f,
            2.0f / 3.0f, 2.0f / 3.0f};
    }
    image.container.curveCount = 2;
    image.container.curves = curves;

    harken::TransitionRuleViewV1 rule{};
    rule.stableRuleOrdinal = 7;
    rule.sourceAny = 1;
    rule.targetAny = 1;
    rule.sourceExit = harken::TransitionSourceExitV1::Immediate;
    rule.targetEntry = harken::TransitionTargetEntryV1::FromEntryMarker;
    rule.sourceFade.active = 1;
    rule.sourceFade.durationFrames = 480;
    rule.sourceFade.curveLocalId = curves[0].curveLocalId;
    rule.bridgeFadeIn.curveLocalId = UINT32_MAX;
    rule.bridgeFadeOut.curveLocalId = UINT32_MAX;
    rule.targetFade.active = 1;
    rule.targetFade.durationFrames = 960;
    rule.targetFade.curveLocalId = curves[1].curveLocalId;
    rule.authoredStableOrder = 11;

    harken::TransitionProgramViewV1 transition{};
    transition.ownerContainerStableId = image.container.containerStableId;
    transition.ruleCount = 1;
    transition.rules = &rule;

    image.world.transitions = &transition;
    image.world.transitionCount = 1;
    image.world.identity.capabilityMask |= harken::kWorldCapabilityTransition;
    image.RefreshWorldFingerprint();
    const std::uint64_t firstFingerprint =
        image.world.identity.runtimeSemanticFingerprint;

    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              harken::ValidateCurrentWorldV1(image.world));

    rule.targetFade.durationFrames = 961;
    image.RefreshWorldFingerprint();
    EXPECT_NE(firstFingerprint,
              image.world.identity.runtimeSemanticFingerprint);
    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              harken::ValidateCurrentWorldV1(image.world));
}

TEST(CurrentWorldInstall,
     TransitionRuleEndpointsMustBeDirectChildrenOfTheirOwner) {
    RunnableWorld image;
    harken::PlayablePropertyProgramViewV1 properties[3] = {
        image.properties[0], {}, image.properties[1]};
    properties[1].contentPropertyKind =
        harken::playableproperties::ContentPropertyKindV1::Sound;
    properties[1].stableId = harkentest::ObjectId(11);
    properties[1].authoredStableOrder = 2;
    properties[1].contentRevision = 111;
    properties[1].routeStableId = harkentest::ObjectId(211);
    properties[1].compatibilityFingerprint = 311;
    properties[1].payload = ResolvedPayload();
    properties[1].programSemanticFingerprint =
        harken::ComputePlayablePropertyFingerprintV1(properties[1]);

    harken::TransitionRuleViewV1 rules[2]{};
    rules[0].sourceAny = 1;
    rules[0].targetAny = 1;
    rules[1].stableRuleOrdinal = 1;
    rules[1].source = {
        harken::PlayableKindV1::Sound, 0, properties[1].stableId,
        properties[1].programSemanticFingerprint,
        properties[1].contentRevision, properties[1].routeStableId,
        properties[1].compatibilityFingerprint};
    rules[1].targetAny = 1;
    rules[1].authoredStableOrder = 1;

    harken::TransitionProgramViewV1 transition{};
    transition.ownerContainerStableId = image.container.containerStableId;
    transition.ruleCount = 2;
    transition.rules = rules;

    image.container.mode = harken::ContainerModeV1::SequenceQueue;
    image.container.transitionProgramIndex = 0;
    image.world.playableProperties = properties;
    image.world.playablePropertyCount = 3;
    image.world.transitions = &transition;
    image.world.transitionCount = 1;
    image.world.identity.capabilityMask =
        harken::kWorldCapabilityPlayableProperties |
        harken::kWorldCapabilitySequenceQueue |
        harken::kWorldCapabilityTransition;
    image.RefreshWorldFingerprint();

    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
              harken::ValidateCurrentWorldV1(image.world));
}

TEST(CurrentWorldInstall, TransitionProgramsMustBeReferencedByTheirOwner) {
    RunnableWorld image;
    harken::TransitionRuleViewV1 rule{};
    rule.sourceAny = 1;
    rule.targetAny = 1;
    harken::TransitionProgramViewV1 transition{};
    transition.ownerContainerStableId = image.container.containerStableId;
    transition.ruleCount = 1;
    transition.rules = &rule;

    image.world.transitions = &transition;
    image.world.transitionCount = 1;
    image.world.identity.capabilityMask |= harken::kWorldCapabilityTransition;
    image.RefreshWorldFingerprint();

    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
              harken::ValidateCurrentWorldV1(image.world));
}

TEST(CurrentWorldInstall,
     TransitionCapabilityClassificationRejectsAdvancedAndMalformedRows) {
    harken::TransitionRuleViewV1 rule{};
    rule.sourceAny = 1;
    rule.targetAny = 1;
    rule.bridgeFadeIn.curveLocalId = UINT32_MAX;
    rule.bridgeFadeOut.curveLocalId = UINT32_MAX;
    harken::TransitionProgramViewV1 transition{};
    transition.ownerContainerStableId = harkentest::ObjectId(20);
    transition.ruleCount = 1;
    transition.rules = &rule;

    EXPECT_EQ(harken::TransitionProgramClassV1::Basic,
              harken::ClassifyTransitionProgramV1(transition));

    rule.sourceExit = harken::TransitionSourceExitV1::NextBeat;
    rule.requiredClockFingerprint = 0x1234;
    EXPECT_EQ(harken::TransitionProgramClassV1::Advanced,
              harken::ClassifyTransitionProgramV1(transition));

    rule.sourceExit = harken::TransitionSourceExitV1::Immediate;
    rule.requiredClockFingerprint = 0;
    transition.maxContainerDescent = 1;
    EXPECT_EQ(harken::TransitionProgramClassV1::Advanced,
              harken::ClassifyTransitionProgramV1(transition));

    transition.maxContainerDescent = 0;
    transition.maxOwnerAscent = 1;
    EXPECT_EQ(harken::TransitionProgramClassV1::Advanced,
              harken::ClassifyTransitionProgramV1(transition));

    harken::StableAuthoredPathStepV1 path{};
    path.playable.kind = harken::PlayableKindV1::Container;
    path.playable.stableId = harkentest::ObjectId(20);
    path.playable.expectedProgramSemanticFingerprint = 21;
    path.playable.contentRevision = 22;
    transition.maxOwnerAscent = 0;
    transition.downwardPathStepCount = 1;
    transition.downwardPathSteps = &path;
    EXPECT_EQ(harken::TransitionProgramClassV1::Advanced,
              harken::ClassifyTransitionProgramV1(transition));

    transition.downwardPathStepCount = 0;
    transition.downwardPathSteps = nullptr;
    transition.watchFallback = 1;
    EXPECT_EQ(harken::TransitionProgramClassV1::Invalid,
              harken::ClassifyTransitionProgramV1(transition));

    transition.watchFallback = 0;
    rule.sourceFade.durationFrames = 1;
    EXPECT_EQ(harken::TransitionProgramClassV1::Invalid,
              harken::ClassifyTransitionProgramV1(transition));
}

TEST(CurrentWorldInstall,
     CanonicalAdvancedTransitionIsCapabilityUnavailableWithoutMutation) {
    RunnableWorld image;
    image.container.mode = harken::ContainerModeV1::SequenceQueue;
    image.child.frequencyWeight = 0;
    image.container.transitionProgramIndex = 0;
    image.world.identity.capabilityMask =
        harken::kWorldCapabilityPlayableProperties |
        harken::kWorldCapabilitySequenceQueue |
        harken::kWorldCapabilityTransition;

    harken::TransitionRuleViewV1 rule{};
    rule.sourceAny = 1;
    rule.targetAny = 1;
    rule.sourceExit = harken::TransitionSourceExitV1::NextBeat;
    rule.bridgeFadeIn.curveLocalId = UINT32_MAX;
    rule.bridgeFadeOut.curveLocalId = UINT32_MAX;
    rule.requiredClockFingerprint = 0x1234;

    harken::TransitionProgramViewV1 transition{};
    transition.ownerContainerStableId = image.container.containerStableId;
    transition.ruleCount = 1;
    transition.rules = &rule;
    image.world.transitions = &transition;
    image.world.transitionCount = 1;
    image.RefreshWorldFingerprint();

    EXPECT_EQ(harken::WorldInstallStatus::CapabilityUnavailable,
              harken::ValidateCurrentWorldV1(image.world));

    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto installed =
        harken::InstallContainerWorldV1(*session.Engine(), image.world);
    EXPECT_EQ(harken::WorldInstallStatus::CapabilityUnavailable,
              installed.status);
    EXPECT_EQ(harken::kNoWorldGeneration,
              session.Engine()->variables.CurrentWorldGeneration());
}

TEST(CurrentWorldInstall,
     BasicTransitionAcceptsCompositeTrackForQueueAndDiscreteVariableControl) {
    RunnableWorld image;

    harken::PlayablePropertyProgramViewV1 properties[3] = {
        image.properties[0], image.properties[1], {}};
    properties[2].contentPropertyKind =
        harken::playableproperties::ContentPropertyKindV1::Track;
    properties[2].stableId = harkentest::ObjectId(30);
    properties[2].authoredStableOrder = 2;
    properties[2].contentRevision = 103;
    properties[2].routeStableId = harkentest::ObjectId(203);
    properties[2].compatibilityFingerprint = 303;
    properties[2].payload = ResolvedPayload();
    properties[2].programSemanticFingerprint =
        harken::ComputePlayablePropertyFingerprintV1(properties[2]);

    image.child.playable.kind = harken::PlayableKindV1::Track;
    image.child.playable.stableId = properties[2].stableId;
    image.child.playable.expectedProgramSemanticFingerprint =
        properties[2].programSemanticFingerprint;
    image.child.playable.contentRevision = properties[2].contentRevision;
    image.child.playable.routeStableId = properties[2].routeStableId;
    image.child.playable.compatibilityFingerprint =
        properties[2].compatibilityFingerprint;
    image.child.frequencyWeight = 0;

    harken::ScheduleClipPlacementV1 placements[2]{};
    harken::PlayableOccurrencePropertyViewV1 occurrences[3]{};
    occurrences[0] = image.occurrence;
    occurrences[0].childPlayable = image.child.playable;
    occurrences[0].occurrenceSemanticFingerprint =
        harken::ComputePlayableOccurrenceFingerprintV1(occurrences[0]);
    for (std::uint32_t index = 0; index < 2; ++index) {
        placements[index].stablePlacementOrdinal = index;
        placements[index].stableOccurrenceOrdinal = index + 1;
        placements[index].content = properties[0].contentPropertyKind ==
                harken::playableproperties::ContentPropertyKindV1::Sound
            ? harken::PlayableRefV1{
                  harken::PlayableKindV1::Sound, 0, properties[0].stableId,
                  properties[0].programSemanticFingerprint,
                  properties[0].contentRevision, properties[0].routeStableId,
                  properties[0].compatibilityFingerprint}
            : harken::PlayableRefV1{};
        placements[index].authoredStart = index * 1000000;
        placements[index].clipId = harkentest::ObjectId(100 + index);
        placements[index].clipGuidHigh = 200 + index;
        placements[index].clipGuidLow = 300 + index;

        occurrences[index + 1].parentKind =
            harken::playableproperties::ContentPropertyKindV1::Track;
        occurrences[index + 1].stableOccurrenceOrdinal = index + 1;
        occurrences[index + 1].parentStableId = properties[2].stableId;
        occurrences[index + 1].parentProgramSemanticFingerprint =
            properties[2].programSemanticFingerprint;
        occurrences[index + 1].childPlayable = placements[index].content;
        occurrences[index + 1].payload = CanonicalPayload();
        occurrences[index + 1].occurrenceSemanticFingerprint =
            harken::ComputePlayableOccurrenceFingerprintV1(
                occurrences[index + 1]);
    }

    harken::TrackScheduleProgramViewV1 track{};
    track.rootDefinition = image.child.playable;
    track.placementCount = 2;
    track.placements = placements;
    track.scheduleSemanticFingerprint =
        harken::ComputeTrackScheduleFingerprintV1(track);
    image.child.timingHint = harken::MakeScheduleReferenceTimingHint(
        harken::PlayableKindV1::Track, properties[2].stableId,
        track.scheduleSemanticFingerprint);

    harken::TransitionRuleViewV1 rule{};
    rule.sourceAny = 1;
    rule.targetAny = 1;
    rule.sourceFade.curveLocalId = UINT32_MAX;
    rule.bridgeFadeIn.curveLocalId = UINT32_MAX;
    rule.bridgeFadeOut.curveLocalId = UINT32_MAX;
    rule.targetFade.curveLocalId = UINT32_MAX;
    harken::TransitionProgramViewV1 transition{};
    transition.ownerContainerStableId = image.container.containerStableId;
    transition.ruleCount = 1;
    transition.rules = &rule;

    image.container.mode = harken::ContainerModeV1::SequenceQueue;
    image.container.transitionProgramIndex = 0;
    image.world.playableProperties = properties;
    image.world.playablePropertyCount = 3;
    image.world.playableOccurrences = occurrences;
    image.world.playableOccurrenceCount = 3;
    image.world.trackSchedules = &track;
    image.world.trackScheduleCount = 1;
    image.world.transitions = &transition;
    image.world.transitionCount = 1;
    image.world.identity.capabilityMask =
        harken::kWorldCapabilityPlayableProperties |
        harken::kWorldCapabilitySequenceQueue |
        harken::kWorldCapabilityTransition;
    image.RefreshWorldFingerprint();

    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              harken::ValidateCurrentWorldV1(image.world));

    harken::VariableDefinitionV1 variable{};
    variable.variableId = harkentest::ObjectId(41);
    variable.kind = harken::VariableKindV1::Step;
    variable.rangeMin = 0.0f;
    variable.rangeMax = 8.0f;
    variable.defaultValue = 3.0f;
    harken::GridAxisV1 axis{0, 0, variable.variableId};
    harken::DiscreteGridCellV1 cell{};
    cell.kind = harken::DiscreteCellKindV1::Exact;
    cell.exactValue = 3;
    harken::DiscreteGridRowViewV1 row{};
    row.childStableOrdinal = image.child.stableChildOrdinal;
    row.cellCount = 1;
    row.cells = &cell;

    image.child.timingHint = {};
    image.container.mode = harken::ContainerModeV1::VariableControl;
    image.container.discreteTiming = 1;
    image.container.discreteAxisCount = 1;
    image.container.discreteAxes = &axis;
    image.container.discreteRowCount = 1;
    image.container.discreteRows = &row;
    image.container.evaluationPeriodFrames = 64;
    image.world.variables = &variable;
    image.world.variableCount = 1;
    image.world.identity.capabilityMask =
        harken::kWorldCapabilityVariableDefinitions |
        harken::kWorldCapabilityPlayableProperties |
        harken::kWorldCapabilityVariableControl |
        harken::kWorldCapabilityTransition;
    image.RefreshWorldFingerprint();

    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              harken::ValidateCurrentWorldV1(image.world));
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              harken::InstallContainerWorldV1(*session.Engine(), image.world)
                  .status);

    harken::ContinuousGridCellV1 continuousCell{};
    continuousCell.kind = harken::ContinuousCellKindV1::Interval;
    continuousCell.lower = variable.rangeMin;
    continuousCell.upper = variable.rangeMax;
    continuousCell.lowerInclusive = 1;
    continuousCell.upperInclusive = 1;
    harken::ContinuousGridRowViewV1 continuousRow{};
    continuousRow.childStableOrdinal = image.child.stableChildOrdinal;
    continuousRow.cellCount = 1;
    continuousRow.cells = &continuousCell;
    image.container.continuousAxisCount = 1;
    image.container.continuousAxes = &axis;
    image.container.continuousRowCount = 1;
    image.container.continuousRows = &continuousRow;
    image.container.maxActiveRows = 1;
    image.container.maxLiveRows = 1;
    image.RefreshWorldFingerprint();
    EXPECT_EQ(harken::WorldInstallStatus::CapabilityUnavailable,
              harken::ValidateCurrentWorldV1(image.world));
}

TEST(CurrentWorldInstall,
     VintageThreeOpaqueTransitionRemainsCapabilityUnavailableWithoutMutation) {
    RunnableWorld image;
    image.container.mode = harken::ContainerModeV1::SequenceQueue;
    image.child.frequencyWeight = 0;
    image.container.transitionProgramIndex = 0;
    image.world.identity.capabilityMask =
        harken::kWorldCapabilityPlayableProperties |
        harken::kWorldCapabilitySequenceQueue |
        harken::kWorldCapabilityTransition;

    harken::TransitionProgramViewV1 opaqueTransition{};
    image.world.transitions = &opaqueTransition;
    image.world.transitionCount = 1;
    image.historicalChild = {};
    image.historicalChild.playable = image.child.playable;
    image.historicalChild.stableChildOrdinal = image.child.stableChildOrdinal;
    image.historicalChild.authoredStableOrder = image.child.authoredStableOrder;
    image.historicalChild.frequencyWeight = image.child.frequencyWeight;
    image.historicalChild.orderWeight = image.child.orderWeight;
    image.historicalChild.queueStopPolicy = image.child.queueStopPolicy;
    image.historicalContainer = {};
    image.historicalContainer.containerStableId =
        image.container.containerStableId;
    image.historicalContainer.programSemanticFingerprint =
        image.container.programSemanticFingerprint;
    image.historicalContainer.mode = image.container.mode;
    image.historicalContainer.randomDiscipline =
        image.container.randomDiscipline;
    image.historicalContainer.memoryScope = image.container.memoryScope;
    image.historicalContainer.childCount = 1;
    image.historicalContainer.children = &image.historicalChild;
    image.historicalContainer.transitionProgramIndex = 0;
    auto historical = harkentest::AsHistoricalPrefix(image.world);
    historical.containers = &image.historicalContainer;
    historical.containerCount = 1;
    historical.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV3Replay(historical);
    image.world.identity.runtimeSemanticFingerprint =
        historical.identity.runtimeSemanticFingerprint;

    EXPECT_EQ(harken::WorldInstallStatus::CapabilityUnavailable,
              harken::ValidateCurrentWorldV3Replay(historical));

    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto installed = harken::InstallContainerWorldV3Replay(
        *session.Engine(), historical);
    EXPECT_EQ(harken::WorldInstallStatus::CapabilityUnavailable,
              installed.status);
    EXPECT_EQ(harken::kNoWorldGeneration,
              session.Engine()->variables.CurrentWorldGeneration());
}

TEST(CurrentWorldInstall, TransitionRuleSelectionUsesExactSpecificity) {
    harken::PlayableRefV1 source{};
    source.stableId = harkentest::ObjectId(10);
    source.expectedProgramSemanticFingerprint = 11;
    source.contentRevision = 12;
    harken::PlayableRefV1 target{};
    target.stableId = harkentest::ObjectId(20);
    target.expectedProgramSemanticFingerprint = 21;
    target.contentRevision = 22;
    harken::TransitionRuleViewV1 rules[4]{};
    for (std::uint32_t i = 0; i < 4; ++i) {
        rules[i].stableRuleOrdinal = i;
        rules[i].authoredStableOrder = 10 + i;
        rules[i].bridgeFadeIn.curveLocalId = UINT32_MAX;
        rules[i].bridgeFadeOut.curveLocalId = UINT32_MAX;
    }
    rules[0].sourceAny = 1;
    rules[0].targetAny = 1;
    rules[1].sourceAny = 1;
    rules[1].target = target;
    rules[2].source = source;
    rules[2].targetAny = 1;
    rules[3].source = source;
    rules[3].target = target;
    harken::TransitionProgramViewV1 transition{};
    transition.ownerContainerStableId = harkentest::ObjectId(1);
    transition.ruleCount = 4;
    transition.rules = rules;

    ASSERT_EQ(harken::TransitionProgramClassV1::Basic,
              harken::ClassifyTransitionProgramV1(transition));
    ASSERT_EQ(&rules[3],
              harken::ResolveTransitionRuleV1(transition, source, target));
    harken::PlayableRefV1 otherSource = source;
    otherSource.stableId = harkentest::ObjectId(11);
    EXPECT_EQ(&rules[1],
              harken::ResolveTransitionRuleV1(
                  transition, otherSource, target));
    harken::PlayableRefV1 otherTarget = target;
    otherTarget.stableId = harkentest::ObjectId(21);
    EXPECT_EQ(&rules[2],
              harken::ResolveTransitionRuleV1(
                  transition, source, otherTarget));
    EXPECT_EQ(&rules[0],
              harken::ResolveTransitionRuleV1(
                  transition, otherSource, otherTarget));
}

TEST(CurrentWorldInstall,
     VariableControlContinuousTimingNormalizationAndCapsAreBounded) {
    RunnableWorld image;
    harken::VariableDefinitionV1 variable{};
    variable.variableId = harkentest::ObjectId(30);
    variable.kind = harken::VariableKindV1::Smooth;
    variable.rangeMin = 0.0f;
    variable.rangeMax = 10.0f;
    variable.defaultValue = 1.0f;
    harken::GridAxisV1 axis{0, 0, variable.variableId};
    harken::ContinuousGridCellV1 cell{};
    cell.kind = harken::ContinuousCellKindV1::Interval;
    cell.lower = 0.0f;
    cell.upper = 10.0f;
    cell.lowerInclusive = 1;
    cell.upperInclusive = 1;
    harken::ContinuousGridRowViewV1 row{};
    row.childStableOrdinal = image.child.stableChildOrdinal;
    row.cellCount = 1;
    row.cells = &cell;
    image.child.frequencyWeight = 0;
    image.container.mode = harken::ContainerModeV1::VariableControl;
    image.container.continuousAxisCount = 1;
    image.container.continuousAxes = &axis;
    image.container.continuousRowCount = 1;
    image.container.continuousRows = &row;
    image.container.evaluationPeriodFrames = 64;
    image.container.maxActiveRows = 1;
    image.container.maxLiveRows = 1;
    image.world.variables = &variable;
    image.world.variableCount = 1;
    image.world.identity.capabilityMask =
        harken::kWorldCapabilityVariableDefinitions |
        harken::kWorldCapabilityPlayableProperties |
        harken::kWorldCapabilityVariableControl;
    const auto expect = [&](harken::WorldInstallStatus status) {
        image.RefreshWorldFingerprint();
        EXPECT_EQ(status, harken::ValidateCurrentWorldV1(image.world));
    };

    expect(harken::WorldInstallStatus::Ok);
    image.container.continuousNormalization = 2;
    expect(harken::WorldInstallStatus::InvalidWorld);
    image.container.continuousNormalization = 0;
    image.container.evaluationPeriodFrames = 0;
    expect(harken::WorldInstallStatus::InvalidWorld);
    image.container.evaluationPeriodFrames = 64;
    image.container.maxActiveRows = 0;
    expect(harken::WorldInstallStatus::InvalidWorld);
    image.container.maxActiveRows = 2;
    image.container.maxLiveRows = 1;
    expect(harken::WorldInstallStatus::InvalidWorld);
    image.container.maxActiveRows =
        image.world.capacity.maxContinuousActiveRows + 1;
    image.container.maxLiveRows = image.container.maxActiveRows;
    expect(harken::WorldInstallStatus::Capacity);
    image.container.maxActiveRows = 1;
    image.container.maxLiveRows =
        image.world.capacity.maxContinuousLiveRows + 1;
    expect(harken::WorldInstallStatus::Capacity);
    image.container.maxLiveRows = 1;
    image.world.capacity.maxContinuousCandidates = 0;
    expect(harken::WorldInstallStatus::Capacity);
}

TEST(CurrentWorldInstall,
     VariableControlContinuousEnvelopeRejectsOneFrameWithoutMutation) {
    RunnableWorld image;
    harken::VariableDefinitionV1 variable{};
    variable.variableId = harkentest::ObjectId(30);
    variable.kind = harken::VariableKindV1::Smooth;
    variable.rangeMin = 0.0f;
    variable.rangeMax = 10.0f;
    variable.defaultValue = 1.0f;
    harken::GridAxisV1 axis{0, 0, variable.variableId};
    harken::ContinuousGridCellV1 cell{};
    cell.kind = harken::ContinuousCellKindV1::Interval;
    cell.lower = 0.0f;
    cell.upper = 10.0f;
    cell.lowerInclusive = 1;
    cell.upperInclusive = 1;
    harken::ContinuousGridRowViewV1 row{};
    row.childStableOrdinal = image.child.stableChildOrdinal;
    row.cellCount = 1;
    row.cells = &cell;
    image.child.frequencyWeight = 0;
    image.container.mode = harken::ContainerModeV1::VariableControl;
    image.container.continuousAxisCount = 1;
    image.container.continuousAxes = &axis;
    image.container.continuousRowCount = 1;
    image.container.continuousRows = &row;
    image.container.evaluationPeriodFrames = 64;
    image.container.maxActiveRows = 1;
    image.container.maxLiveRows = 1;
    image.world.variables = &variable;
    image.world.variableCount = 1;
    image.world.identity.capabilityMask =
        harken::kWorldCapabilityVariableDefinitions |
        harken::kWorldCapabilityPlayableProperties |
        harken::kWorldCapabilityVariableControl;

    image.container.envelopeFrames = 0;
    image.RefreshWorldFingerprint();
    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              harken::ValidateCurrentWorldV1(image.world));

    image.container.envelopeFrames = 2;
    image.RefreshWorldFingerprint();
    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              harken::ValidateCurrentWorldV1(image.world));

    image.container.envelopeFrames = 1;
    image.RefreshWorldFingerprint();
    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
              harken::ValidateCurrentWorldV1(image.world));
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
              harken::InstallContainerWorldV1(*session.Engine(), image.world)
                  .status);
    EXPECT_EQ(0u, session.Engine()->variables.CurrentWorldGeneration());
}

TEST(CurrentWorldInstall,
     VariableControlContinuousWeightCurvesRejectNegativeYWithoutMutation) {
    RunnableWorld image;
    harken::VariableDefinitionV1 variable{};
    variable.variableId = harkentest::ObjectId(30);
    variable.kind = harken::VariableKindV1::Smooth;
    variable.rangeMin = 0.0f;
    variable.rangeMax = 10.0f;
    variable.defaultValue = 1.0f;
    harken::GridAxisV1 axis{0, 0, variable.variableId};
    harken::ContinuousGridCellV1 cell{};
    cell.kind = harken::ContinuousCellKindV1::Interval;
    cell.lower = 0.0f;
    cell.upper = 10.0f;
    cell.lowerInclusive = 1;
    cell.upperInclusive = 1;
    cell.leftCurveLocalId = 42;
    harken::ContinuousGridRowViewV1 row{};
    row.childStableOrdinal = image.child.stableChildOrdinal;
    row.cellCount = 1;
    row.cells = &cell;
    harken::CurveV1 curve{};
    curve.curveLocalId = 42;
    curve.curve.pointCount = 2;
    curve.curve.points[0] = {0.0f, 0.0f};
    curve.curve.points[1] = {10.0f, 1.0f};
    curve.curve.handles[0] = {0.0f, 0.0f, 10.0f, 1.0f};
    image.child.frequencyWeight = 0;
    image.container.mode = harken::ContainerModeV1::VariableControl;
    image.container.curveCount = 1;
    image.container.curves = &curve;
    image.container.continuousAxisCount = 1;
    image.container.continuousAxes = &axis;
    image.container.continuousRowCount = 1;
    image.container.continuousRows = &row;
    image.container.evaluationPeriodFrames = 64;
    image.container.maxActiveRows = 1;
    image.container.maxLiveRows = 1;
    image.world.variables = &variable;
    image.world.variableCount = 1;
    image.world.identity.capabilityMask =
        harken::kWorldCapabilityVariableDefinitions |
        harken::kWorldCapabilityPlayableProperties |
        harken::kWorldCapabilityVariableControl;
    const auto expectInvalid = [&] {
        image.RefreshWorldFingerprint();
        EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
                  harken::ValidateCurrentWorldV1(image.world));
    };

    image.RefreshWorldFingerprint();
    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              harken::ValidateCurrentWorldV1(image.world));
    curve.curve.points[1].y = -0.25f;
    expectInvalid();
    curve.curve.points[1].y = 1.0f;
    curve.curve.handles[0].h1y = -0.25f;
    expectInvalid();
    curve.curve.handles[0].h1y = 0.0f;
    curve.curve.handles[0].h2y = -0.25f;
    expectInvalid();

    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
              harken::InstallContainerWorldV1(*session.Engine(), image.world)
                  .status);
    EXPECT_EQ(0u, session.Engine()->variables.CurrentWorldGeneration());
}

TEST(CurrentWorldInstall,
     VariableControlDiscreteAxisRejectsUnroundableVariableRangeWithoutMutation) {
    RunnableWorld image;
    harken::VariableDefinitionV1 variable{};
    variable.variableId = harkentest::ObjectId(30);
    variable.kind = harken::VariableKindV1::Smooth;
    variable.rangeMin = 0.0f;
    variable.rangeMax = 10.0f;
    variable.defaultValue = 1.0f;
    harken::GridAxisV1 axis{0, 0, variable.variableId};
    harken::DiscreteGridCellV1 cell{};
    cell.kind = harken::DiscreteCellKindV1::Exact;
    cell.exactValue = 1;
    harken::DiscreteGridRowViewV1 row{};
    row.childStableOrdinal = image.child.stableChildOrdinal;
    row.cellCount = 1;
    row.cells = &cell;
    image.child.frequencyWeight = 0;
    image.container.mode = harken::ContainerModeV1::VariableControl;
    image.container.discreteAxisCount = 1;
    image.container.discreteAxes = &axis;
    image.container.discreteRowCount = 1;
    image.container.discreteRows = &row;
    image.world.variables = &variable;
    image.world.variableCount = 1;
    image.world.identity.capabilityMask =
        harken::kWorldCapabilityVariableDefinitions |
        harken::kWorldCapabilityPlayableProperties |
        harken::kWorldCapabilityVariableControl;

    image.RefreshWorldFingerprint();
    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              harken::ValidateCurrentWorldV1(image.world));

    variable.rangeMin = -1.0e30f;
    variable.rangeMax = 1.0e30f;
    variable.defaultValue = 0.0f;
    image.RefreshWorldFingerprint();
    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
              harken::ValidateCurrentWorldV1(image.world));
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
              harken::InstallContainerWorldV1(*session.Engine(), image.world)
                  .status);
    EXPECT_EQ(0u, session.Engine()->variables.CurrentWorldGeneration());
}

} // namespace
