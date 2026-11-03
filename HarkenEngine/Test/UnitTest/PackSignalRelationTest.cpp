#include "Common/ObjectIdTestSupport.h"
#include "harken/core/PackComposition.h"
#include "harken/core/detail/CurrentWorldValidation.h"
#include <gtest/gtest.h>
#include <array>
#include <span>

namespace {
using namespace harken;

PackComposition::CandidateOwner Candidate(std::uint64_t id,
    const ContainerWorldInstallViewV1& input, std::uint64_t dependency = 0) {
    auto world = input;
    world.sourceBodyVintage = kCurrentWorldBodyVintageV20;
    world.identity.capabilityMask = detail::ComputeCurrentWorldLocalCapabilityMaskV1(world);
    world.identity.runtimeSemanticFingerprint = ComputeCurrentWorldFingerprintV7(world);
    EXPECT_EQ(detail::ValidateCurrentWorldPartitionLocalV1(world), WorldInstallStatus::Ok);
    PackComposition::CandidateOwner owner;
    owner.packStableId = id;
    owner.contentHash = id;
    owner.handle = (UINT64_C(1) << 32) | id;
    owner.slotIndex = static_cast<std::uint32_t>(id);
    owner.contentRevision = owner.partitionRevision = owner.resourceRevision = 1;
    owner.hasPartition = true;
    owner.partitionBodyVintage = kCurrentWorldBodyVintageV20;
    EXPECT_TRUE(EncodeCurrentWorldBodyV20(world, owner.partitionBody));
    owner.definitionBytes = owner.partitionBody.size();
    if (dependency) {
        PackDependencyEdge edge;
        edge.prerequisitePackStableId = dependency;
        edge.required01 = 1;
        edge.expectedPrerequisiteContentRevision = 1;
        owner.dependencyEdges.push_back(edge);
    }
    return owner;
}

PlayablePropertyProgramViewV1 SoundProperty() {
    PlayablePropertyProgramViewV1 p;
    p.stableId = harkentest::ObjectId(100);
    p.contentPropertyKind = playableproperties::ContentPropertyKindV1::Sound;
    p.contentRevision = 1;
    p.routeStableId = VN_BUS_MASTER;
    p.compatibilityFingerprint = 1;
    p.payload.presentMask = kResolvedSectionGroupPropertyMaskV1;
    p.payload.values[3].bits = 1;
    p.payload.values[6].objectId = VN_BUS_MASTER;
    p.payload.values[11].bits = p.payload.values[16].bits = 0x3f800000;
    p.programSemanticFingerprint = ComputePlayablePropertyFingerprintV1(p);
    return p;
}

TEST(PackSignalRelation, SourceMustBelongToDeclaredClosureAndMatchDefinition) {
    auto property = SoundProperty();
    ContainerWorldInstallViewV1 provider;
    provider.playableProperties = &property;
    provider.playablePropertyCount = 1;
    SignalReceiverV1 receiver;
    receiver.target.stableId = VN_BUS_MASTER;
    SignalRelationV1 relation;
    relation.stableId = harkentest::ObjectId(200);
    relation.source = {SignalEndpointKindV1::Sound, 0, property.stableId,
                       property.programSemanticFingerprint, 0};
    relation.target = receiver.target;
    ContainerWorldInstallViewV1 consumer;
    consumer.signalRelations = &relation;
    consumer.signalRelationCount = 1;
    consumer.signalReceivers = &receiver;
    consumer.signalReceiverCount = 1;
    PackComposition composition(4);
    ASSERT_EQ(composition.Commit({Candidate(1, provider)}, nullptr, 0, 8, 0), VN_OK);
    const auto previous = composition.Published();
    const auto generation = composition.CompositeGeneration();
    PackComposition::PreparedCommit prepared;
    EXPECT_EQ(composition.PrepareCommit({Candidate(1, provider), Candidate(2, consumer)},
        nullptr, 0, 8, 0, &prepared), VN_BAD_PACK);
    // Until the render consumer is wired, a valid declared closure reaches the
    // capability gate. It must never silently install an ignored relation.
    EXPECT_EQ(composition.PrepareCommit({Candidate(1, provider), Candidate(2, consumer, 1)},
        nullptr, 0, 8, 0, &prepared), VN_CAPABILITY_UNAVAILABLE);
    ++relation.source.expectedPropertyFingerprint;
    EXPECT_EQ(composition.PrepareCommit({Candidate(1, provider), Candidate(2, consumer, 1)},
        nullptr, 0, 8, 0, &prepared), VN_BAD_PACK);
    EXPECT_EQ(composition.Published(), previous);
    EXPECT_EQ(composition.CompositeGeneration(), generation);
}

TEST(PackSignalRelation, ReceiverConfigurationAlsoRequiresDeclaredClosure) {
    SignalReceiverV1 receiver;
    receiver.target.stableId = VN_BUS_MASTER;
    ContainerWorldInstallViewV1 provider;
    provider.signalReceivers = &receiver;
    provider.signalReceiverCount = 1;
    SignalRelationV1 relation;
    relation.stableId = harkentest::ObjectId(200);
    relation.source.stableId = harkentest::ObjectId(101);
    relation.target = receiver.target;
    ContainerWorldInstallViewV1 consumer;
    consumer.signalRelations = &relation;
    consumer.signalRelationCount = 1;
    PackComposition composition(4);
    PackComposition::PreparedCommit prepared;
    EXPECT_EQ(composition.PrepareCommit({Candidate(1, provider), Candidate(2, consumer)},
        nullptr, 0, 8, 0, &prepared), VN_BAD_PACK);
    EXPECT_EQ(composition.PrepareCommit({Candidate(1, provider), Candidate(2, consumer, 1)},
        nullptr, 0, 8, 0, &prepared), VN_CAPABILITY_UNAVAILABLE);
    consumer.signalReceivers = &receiver;
    consumer.signalReceiverCount = 1;
    EXPECT_EQ(composition.PrepareCommit({Candidate(1, provider), Candidate(2, consumer, 1)},
        nullptr, 0, 8, 0, &prepared), VN_BAD_PACK);
}
} // namespace
