#include "Common/ObjectIdTestSupport.h"
#include "UnitTest/CanonicalPackSetActivate.h"
#include "SignalPublication.h"
#include "harken/core/PackComposition.h"
#include "harken/core/detail/CurrentWorldValidation.h"
#include <gtest/gtest.h>
#include <array>
#include <span>

namespace {
using namespace harken;

PackComposition::CandidateOwner Candidate(std::uint64_t id,
    const ContainerWorldInstallViewV1& input, std::uint64_t dependency = 0,
    std::uint32_t vintage = kCurrentWorldBodyVintageV20) {
    auto world = input;
    world.sourceBodyVintage = vintage;
    world.identity.capabilityMask = detail::ComputeCurrentWorldLocalCapabilityMaskV1(world);
    world.identity.runtimeSemanticFingerprint = vintage == 19
        ? ComputeCurrentWorldFingerprintV6(world) : ComputeCurrentWorldFingerprintV7(world);
    EXPECT_EQ(detail::ValidateCurrentWorldPartitionLocalV1(world), WorldInstallStatus::Ok);
    PackComposition::CandidateOwner owner;
    owner.packStableId = id;
    owner.contentHash = id;
    owner.handle = (UINT64_C(1) << 32) | id;
    owner.slotIndex = static_cast<std::uint32_t>(id);
    owner.contentRevision = owner.partitionRevision = owner.resourceRevision = 1;
    owner.hasPartition = true;
    owner.partitionBodyVintage = vintage;
    EXPECT_TRUE(vintage == 19 ? EncodeCurrentWorldBodyV19(world, owner.partitionBody)
        : EncodeCurrentWorldBodyV20(world, owner.partitionBody));
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
    for (const auto providerVintage : {19u, 20u}) {
    SCOPED_TRACE(providerVintage);
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
    ASSERT_EQ(composition.Commit({Candidate(1, provider, 0, providerVintage)}, nullptr, 0, 8, 0), VN_OK);
    const auto previous = composition.Published();
    const auto generation = composition.CompositeGeneration();
    PackComposition::PreparedCommit prepared;
    EXPECT_EQ(composition.PrepareCommit({Candidate(1, provider, 0, providerVintage), Candidate(2, consumer)},
        nullptr, 0, 8, 0, &prepared), VN_BAD_PACK);
    // Valid declared closure preserves the formal aggregate for publication.
    EXPECT_EQ(composition.PrepareCommit({Candidate(1, provider, 0, providerVintage), Candidate(2, consumer, 1)},
        nullptr, 0, 8, 0, &prepared), VN_OK);
    ++relation.source.expectedPropertyFingerprint;
    EXPECT_EQ(composition.PrepareCommit({Candidate(1, provider, 0, providerVintage), Candidate(2, consumer, 1)},
        nullptr, 0, 8, 0, &prepared), VN_BAD_PACK);
    EXPECT_EQ(composition.Published(), previous);
    EXPECT_EQ(composition.CompositeGeneration(), generation);
    }
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
        nullptr, 0, 8, 0, &prepared), VN_OK);
    consumer.signalReceivers = &receiver;
    consumer.signalReceiverCount = 1;
    EXPECT_EQ(composition.PrepareCommit({Candidate(1, provider), Candidate(2, consumer, 1)},
        nullptr, 0, 8, 0, &prepared), VN_BAD_PACK);
}
} // namespace

namespace {
struct SignalPackProvider {
    std::vector<std::uint8_t> bytes;
    static VnResult Read(void* context, std::uint64_t, const char*, std::uint32_t,
                         std::uint64_t, VnPackByteSpan* span) {
        auto& self = *static_cast<SignalPackProvider*>(context);
        *span = {sizeof(*span), 0, self.bytes.data(), self.bytes.size()};
        return VN_OK;
    }
    static void Release(void*, std::uint64_t, VnPackByteSpan) {}
};

std::vector<std::uint8_t> SignalContentPack(std::uint64_t revision, float sendDb) {
    using namespace harkentest::packfoundation;
    const auto bus = Bus2RuntimeBusFixture();
    SignalRelationV1 relation;
    relation.stableId = harkentest::ObjectId(1000);
    relation.source.stableId = VN_BUS_MASTER;
    relation.target.stableId = VN_OBJECT_ID(0, 2);
    relation.sendGainDb = sendDb;
    SignalReceiverV1 receiver;
    receiver.target = relation.target;
    ContainerWorldInstallViewV1 world;
    world.sourceBodyVintage = kCurrentWorldBodyVintageV20;
    world.capacity.maxActionDefinitions = 64;
    world.signalRelations = &relation; world.signalRelationCount = 1;
    world.signalReceivers = &receiver; world.signalReceiverCount = 1;
    world.identity.capabilityMask = kWorldCapabilitySignalRelations;
    world.identity.runtimeSemanticFingerprint = ComputeCurrentWorldFingerprintV7(world);
    std::vector<std::uint8_t> body;
    EXPECT_TRUE(EncodeCurrentWorldBodyV20(world, body));
    PackCanonicalPartitionHeader header;
    header.headerBytes = kCanonicalDefinitionPartitionHeaderV2Bytes;
    header.bodyVintage = kCurrentWorldBodyVintageV20;
    header.bodyBytes = body.size();
    header.bodyChecksum = Fnv1a64(body.data(), body.size());
    header.packStableId = kResidentPackStableId;
    header.partitionRevision = revision;
    ApplyCanonicalBusTriple(&header, bus.triple);
    std::vector<std::uint8_t> partition(header.headerBytes + body.size());
    SerializePackCanonicalPartitionHeaderV2(header, partition.data());
    header.headerChecksum = Fnv1a64(partition.data(), 80);
    SerializePackCanonicalPartitionHeaderV2(header, partition.data());
    std::copy(body.begin(), body.end(), partition.begin() + header.headerBytes);
    ImageBuilder builder(kPackKindCanonicalContent);
    builder.AddEntry(kPackEntryKindPackRequirementBlock, kResidentPackStableId,
        RequirementPayload(kResidentPackStableId, revision, revision, 0, body.size(), bus.triple));
    builder.AddEntry(kPackEntryKindCanonicalDefinitionPartition, kResidentPackStableId, std::move(partition));
    builder.AddEntry(kPackEntryKindPackDependencyEdge, VnObjectId{},
        DependencyPayload(kInitPackStableId, 1, bus.triple), kPackCanonicalRequirementVintage);
    return builder.Build();
}

TEST(PackSignalRelation, FullSetAndContentOnlyPublicationUseOneCommandAndClearOnUnload) {
    using namespace harkentest::packfoundation;
    auto config = harkentest::MakeNullConfig(2, 64, 48000);
    config.maxBuses = 4;
    config.maxTriggerDepth = 2;
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    auto& engine = *session.Engine();
    SignalPackProvider init{harkentest::BuildMasterBus2InitBytes()}, content{SignalContentPack(1, 0)};
    std::array<VnCanonicalPackSetMemberV1, 2> members{};
    for (auto& member : members) {
        member.structSize = sizeof(member); member.provide = SignalPackProvider::Read;
        member.flags = VN_CANONICAL_PACK_SET_MEMBER_PROVIDER;
        member.release = SignalPackProvider::Release;
    }
    members[0].packStableId = kInitPackStableId;
    members[0].packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    members[0].providerUserData = &init;
    members[1].packStableId = kResidentPackStableId;
    members[1].packKind = VN_CANONICAL_PACK_SET_MEMBER_CONTENT;
    members[1].providerUserData = &content;
    std::array<VnCanonicalPackSetMemberResultV1, 2> rows{};
    for (auto& row : rows) row.structSize = sizeof(row);
    VnCanonicalPackSetActivationReportV1 report{};
    auto free = engine.commands.FreeSlots();
    ASSERT_EQ(harkentest::ActivateCanonicalMembersV1(members.data(), 2, rows.data(), report), VN_OK);
    ASSERT_NE(engine.acceptedSignalPlan, nullptr);
    EXPECT_EQ(engine.commands.FreeSlots(), free - 1);
    engine.nullBackend.Pump(1);
    EXPECT_EQ(engine.buses.InstalledSignals(), engine.acceptedSignalPlan.get());
    const auto identity = engine.acceptedSignalPlan->WorldIdentity();
    for (unsigned i = 0; i < 2; ++i) members[i].expectedHandle = rows[i].handle;
    content.bytes = SignalContentPack(2, -6);
    free = engine.commands.FreeSlots();
    ASSERT_EQ(harkentest::ActivateCanonicalMembersV1(members.data(), 2, rows.data(), report,
        report.initHandle, 1), VN_OK);
    EXPECT_EQ(engine.commands.FreeSlots(), free - 1);
    EXPECT_EQ(engine.acceptedSignalPlan->WorldIdentity(), identity);
    engine.nullBackend.Pump(1);
    EXPECT_EQ(engine.buses.InstalledSignals(), engine.acceptedSignalPlan.get());
    VnCanonicalPackSetUnloadDescV1 unload{};
    unload.structSize = sizeof(unload);
    unload.expectedInitHandle = report.initHandle;
    unload.baseSetRevision = 2; unload.unloadRevision = 3;
    free = engine.commands.FreeSlots();
    ASSERT_EQ(Vn_UnloadCanonicalPackSetV1(&unload), VN_OK);
    EXPECT_EQ(engine.commands.FreeSlots(), free - 1);
    EXPECT_EQ(engine.acceptedSignalPlan, nullptr);
    engine.nullBackend.Pump(1);
    EXPECT_EQ(engine.buses.InstalledSignals(), nullptr);
}

TEST(PackSignalRelation, RetainedPreparedAndLiveLeasesKeepExactProofUntilLastRelease) {
    auto config = harkentest::MakeNullConfig(2, 64, 48000);
    config.maxTriggerDepth = 2;
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    auto& engine = *session.Engine();
    auto& composition = engine.packComposition;
    auto property = SoundProperty();
    SignalRelationV1 relation;
    relation.stableId = harkentest::ObjectId(200);
    relation.source = {SignalEndpointKindV1::Sound, 0, property.stableId, property.programSemanticFingerprint, 0};
    relation.target.stableId = VN_BUS_MASTER;
    SignalReceiverV1 receiver; receiver.target = relation.target;
    ContainerWorldInstallViewV1 world;
    world.playableProperties = &property; world.playablePropertyCount = 1;
    world.signalRelations = &relation; world.signalRelationCount = 1;
    world.signalReceivers = &receiver; world.signalReceiverCount = 1;
    ASSERT_EQ(composition.Commit({Candidate(1, world)}, nullptr, 0, 8, 0), VN_OK);
    PackIdentityLookup pending;
    ASSERT_EQ(composition.PrepareLookup(property.stableId, &pending), VN_OK);
    const auto live = composition.Lookup(property.stableId);
    ASSERT_EQ(live.result, VN_OK);
    auto successor = Candidate(1, world);
    successor.partitionRevision = 2;
    ASSERT_EQ(composition.Commit({successor}, nullptr, 0, 8, 0), VN_OK);
    PreparedSignalPublication signals;
    ASSERT_EQ(PreparePackSignals(engine, composition.Published(), nullptr, signals), WorldInstallStatus::Ok);
    PrivateSignalTopology topology(engine);
    ASSERT_TRUE(topology.Prepare(signals));
    CommitSignals(engine, signals); topology.Commit();
    engine.nullBackend.Pump(1);
    PlaybackPathRow row;
    row.rowKind = ResolvedPlayablePathRowKindV1::Definition;
    row.contentPropertyKind = playableproperties::ContentPropertyKindV1::Sound;
    row.stableId = property.stableId;
    row.propertyFingerprint = property.programSemanticFingerprint;
    row.source = {live.lease.compositeGeneration, live.lease.packStableId,
        live.lease.partitionRevision, live.lease.leaseToken, kCurrentWorldBodyVintageV20};
    std::array<std::uint32_t, 1> matched{};
    ASSERT_EQ(engine.acceptedSignalPlan->MatchMember({&row, 1}, matched), 1u);
    composition.AbandonPreparedLease(pending.lease);
    RefreshRetainedPackSignals(engine);
    ASSERT_EQ(engine.acceptedSignalPlan->MatchMember({&row, 1}, matched), 1u);
    ASSERT_EQ(composition.ReleaseLease(live.lease), VN_OK);
    RefreshRetainedPackSignals(engine);
    EXPECT_EQ(engine.acceptedSignalPlan->MatchMember({&row, 1}, matched), 0u);
    row.source.ownerRevision = 2;
    EXPECT_EQ(engine.acceptedSignalPlan->MatchMember({&row, 1}, matched), 1u);
    engine.nullBackend.Pump(1);
    EXPECT_EQ(engine.buses.InstalledSignals(), engine.acceptedSignalPlan.get());
}
} // namespace
