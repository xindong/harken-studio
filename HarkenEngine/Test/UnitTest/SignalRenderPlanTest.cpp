#include "harken/render/SignalRenderPlan.h"
#include "harken/render/SignalWorldPlan.h"

#include <gtest/gtest.h>
#include <array>
#include <limits>
#include <thread>

namespace {
using namespace harken;
constexpr std::uint32_t kFrames = 64;
constexpr SignalBlockFormat kFormat{1, kFrames, 48000, VN_CHANNEL_LAYOUT_MONO};
SignalBlockIdentity Block(std::uint64_t epoch) { return {1, 2, 3, epoch, epoch * kFrames, kFrames}; }
struct Member {
    std::array<float, kFrames> pcm{};
    float* plane = pcm.data();
    OutputGain out;
    std::array<std::uint32_t, 3> nodes{};
    SignalMemberImage Image(std::uint32_t count) { return {&plane, &out, {nodes.data(), count}, 0, kFrames}; }
};

TEST(SignalRenderPlan, SignedCancellationUsesOneSharedDefinitionReceiver) {
    const std::array nodes{SignalRenderNodeInput{SignalEndpointKindV1::Sound},
                          SignalRenderNodeInput{SignalEndpointKindV1::Sound},
                          SignalRenderNodeInput{SignalEndpointKindV1::Sound, true}};
    const std::array connections{SignalConnectionInput{0, 2, 1}, SignalConnectionInput{1, 2, 1}};
    const std::array paths{SignalFeedbackEdge{0, 2, 1, true}, SignalFeedbackEdge{1, 2, 1, true}};
    auto plan = SignalRenderPlan::Prepare(kFormat, nodes, connections, paths);
    ASSERT_NE(plan, nullptr);
    Member a, b, targetA, targetB;
    a.nodes[0] = 0; b.nodes[0] = 1;
    targetA.nodes = {2, 2, 2}; targetB.nodes[0] = 2;
    targetB.out.Set(.25f);
    const std::array members{a.Image(1), b.Image(1), targetA.Image(3), targetB.Image(1)};
    for (std::uint32_t epoch = 0; epoch < 8; ++epoch) {
        a.pcm.fill(2); b.pcm.fill(-2);
        targetA.pcm.fill(.25f); targetB.pcm.fill(.75f);
        ASSERT_TRUE(plan->BeginBlock(Block(epoch)));
        ASSERT_TRUE(plan->ProcessContent(members));
        ASSERT_TRUE(plan->CommitBlock());
        for (std::uint32_t n = 0; n < kFrames; ++n) {
            EXPECT_FLOAT_EQ(targetA.pcm[n], .25f);
            EXPECT_FLOAT_EQ(targetB.pcm[n], .75f);
        }
    }
}

TEST(SignalRenderPlan, NonFiniteMemberInputIsRejectedBeforeAnyMemberChanges) {
    constexpr SignalBlockFormat format{2, kFrames, 48000, VN_CHANNEL_LAYOUT_STEREO};
    const std::array nodes{SignalRenderNodeInput{SignalEndpointKindV1::Sound},
                          SignalRenderNodeInput{SignalEndpointKindV1::Sound, true}};
    const std::array connections{SignalConnectionInput{0, 1, 1}};
    const std::array paths{SignalFeedbackEdge{0, 1, 1, true}};
    auto plan = SignalRenderPlan::Prepare(format, nodes, connections, paths);
    ASSERT_NE(plan, nullptr);
    std::array<float, kFrames> sourceLeft{}, sourceRight{}, targetLeft{}, targetRight{};
    float* sourcePlanes[]{sourceLeft.data(), sourceRight.data()};
    float* targetPlanes[]{targetLeft.data(), targetRight.data()};
    OutputGain sourceOut, targetOut;
    const std::array<std::uint32_t, 1> sourceNode{0}, targetNode{1};
    // Put the valid receiver first so rejecting a later member cannot partially
    // consume detector time or change already validated output.
    std::array members{
        SignalMemberImage{targetPlanes, &targetOut, targetNode, 0, kFrames},
        SignalMemberImage{sourcePlanes, &sourceOut, sourceNode, 0, kFrames}};
    for (std::uint32_t epoch = 0; epoch < 6; ++epoch) {
        // A proven silent lane cannot suppress validation of other lanes or gains.
        members[1].silentChannels = epoch % 2;
        sourceLeft.fill(members[1].silentChannels != 0 ? 0.0f : .1f);
        sourceRight.fill(.1f);
        targetLeft.fill(1); targetRight.fill(1);
        ASSERT_TRUE(plan->BeginBlock(Block(epoch)));
        if (epoch != 0) {
            const auto beforeLeft = targetLeft, beforeRight = targetRight;
            if (epoch <= 3) {
                sourceRight.back() = epoch == 1 ? std::numeric_limits<float>::infinity()
                    : epoch == 2 ? -std::numeric_limits<float>::infinity()
                                 : std::numeric_limits<float>::quiet_NaN();
            } else {
                sourceOut.Set(std::numeric_limits<float>::quiet_NaN(), epoch == 4 ? 0 : kFrames * 2);
            }
            EXPECT_FALSE(plan->ProcessContent(members));
            EXPECT_EQ(targetLeft, beforeLeft);
            EXPECT_EQ(targetRight, beforeRight);
            sourceRight.back() = .1f;
            sourceOut.Set(1);
        }
        ASSERT_TRUE(plan->ProcessContent(members));
        ASSERT_TRUE(plan->CommitBlock());
        if (epoch != 0) {
            EXPECT_LT(targetLeft.back(), 1);
            EXPECT_EQ(targetLeft, targetRight);
        }
    }
}

TEST(SignalRenderPlan, SharedSoundAndContainerApplyInOrderWithFrozenChildTap) {
    const std::array nodes{SignalRenderNodeInput{SignalEndpointKindV1::Sound},
                          SignalRenderNodeInput{SignalEndpointKindV1::Sound, true},
                          SignalRenderNodeInput{SignalEndpointKindV1::Container, true},
                          SignalRenderNodeInput{SignalEndpointKindV1::Sound, true}};
    const std::array connections{SignalConnectionInput{0, 1, .05f},
                                SignalConnectionInput{0, 2, .025f}, SignalConnectionInput{1, 3, .1f}};
    const std::array paths{SignalFeedbackEdge{0, 1, .05, true},
                          SignalFeedbackEdge{0, 2, .025, true}, SignalFeedbackEdge{1, 3, .1, true},
                          SignalFeedbackEdge{1, 2, 1, false}};
    auto plan = SignalRenderPlan::Prepare(kFormat, nodes, connections, paths);
    ASSERT_NE(plan, nullptr);
    Member source, a, b, observer;
    source.nodes[0] = 0; a.nodes = {1, 2, 2}; b.nodes = {1, 2, 1}; observer.nodes[0] = 3;
    b.out.Set(.5f);
    const std::array members{source.Image(1), a.Image(3), b.Image(3), observer.Image(1)};
    DuckingPreparedParameters parameters;
    ASSERT_TRUE(PrepareDuckingParameters(48000, kDuckingDefaultAttackMs, kDuckingDefaultReleaseMs, parameters));
    DuckingReceiver sound, container, downstream;
    sound.SetParameters(parameters); container.SetParameters(parameters); downstream.SetParameters(parameters);
    std::array<float, kFrames> main{}, side{}, frozenChild{};
    std::array<double, kFrames> sg{}, cg{}, dg{};
    const float* m = main.data(); const float* sc = side.data();
    for (std::uint32_t epoch = 0; epoch < 16; ++epoch) {
        source.pcm.fill(1); a.pcm.fill(.4f); b.pcm.fill(.6f); observer.pcm.fill(1);
        main.fill(1); side.fill(epoch == 0 ? 0.0f : .05f);
        ASSERT_TRUE(sound.Process(&m, &sc, 1, kFrames, nullptr, nullptr, kDuckingBaselineSlope, sg.data()));
        for (std::uint32_t n = 0; n < kFrames; ++n) {
            const float ga = static_cast<float>(static_cast<double>(.4f) * sg[n]);
            const float gb = static_cast<float>(static_cast<double>(.6f) * sg[n]);
            main[n] = static_cast<float>(static_cast<double>(ga) + static_cast<double>(gb) * .5);
        }
        side.fill(epoch == 0 ? 0.0f : .025f);
        ASSERT_TRUE(container.Process(&m, &sc, 1, kFrames, nullptr, nullptr, kDuckingBaselineSlope, cg.data()));
        side = frozenChild;
        for (auto& sample : side) sample *= .1f;
        frozenChild = main;
        main.fill(1);
        ASSERT_TRUE(downstream.Process(&m, &sc, 1, kFrames, nullptr, nullptr, kDuckingBaselineSlope, dg.data()));
        ASSERT_TRUE(plan->BeginBlock(Block(epoch)));
        ASSERT_TRUE(plan->ProcessContent(members));
        ASSERT_TRUE(plan->CommitBlock());
        for (std::uint32_t n = 0; n < kFrames; ++n) {
            EXPECT_FLOAT_EQ(a.pcm[n], static_cast<float>(static_cast<double>(static_cast<float>(.4 * sg[n])) * cg[n]));
            EXPECT_NEAR(b.pcm[n] / a.pcm[n], 1.5f, 2e-6f);
            EXPECT_NEAR(observer.pcm[n], dg[n], 2e-7);
        }
    }
}

TEST(SignalRenderPlan, MutualDefinitionReceivingStaysSymmetricAndResetDropsOldPcm) {
    const std::array nodes{SignalRenderNodeInput{SignalEndpointKindV1::Sound, true},
                          SignalRenderNodeInput{SignalEndpointKindV1::Sound, true}};
    const std::array connections{SignalConnectionInput{0, 1, 1}, SignalConnectionInput{1, 0, 1}};
    const std::array paths{SignalFeedbackEdge{0, 1, 1, true}, SignalFeedbackEdge{1, 0, 1, true}};
    auto plan = SignalRenderPlan::Prepare(kFormat, nodes, connections, paths);
    ASSERT_NE(plan, nullptr);
    Member a, b; a.nodes[0] = 0; b.nodes[0] = 1;
    const std::array members{a.Image(1), b.Image(1)};
    for (std::uint32_t epoch = 0; epoch < 1000; ++epoch) {
        a.pcm.fill(1); b.pcm.fill(1.000001f);
        ASSERT_TRUE(plan->BeginBlock(Block(epoch)));
        ASSERT_TRUE(plan->ProcessContent(members));
        ASSERT_TRUE(plan->CommitBlock());
    }
    const auto c = 1.0 / (1.0 / kDuckingBaselineSlope + 1);
    EXPECT_NEAR(a.pcm.back(), 1.0 / (1.0 + c), 2e-5);
    EXPECT_NEAR(b.pcm.back(), a.pcm.back(), 3e-5);
    auto reset = Block(1000); ++reset.transport;
    a.pcm.fill(1); b.pcm.fill(1);
    ASSERT_TRUE(plan->BeginBlock(reset));
    ASSERT_TRUE(plan->ProcessContent(members));
    ASSERT_TRUE(plan->CommitBlock());
    EXPECT_FLOAT_EQ(a.pcm.front(), 1);
    EXPECT_FLOAT_EQ(b.pcm.front(), 1);
}

TEST(SignalRenderPlan, SourceCaptureUsesActualMemberOutputRoundingBeforeSignedSum) {
    const std::array nodes{SignalRenderNodeInput{SignalEndpointKindV1::Container},
                          SignalRenderNodeInput{SignalEndpointKindV1::Sound, true}};
    const std::array connections{SignalConnectionInput{0, 1, 1}};
    const std::array paths{SignalFeedbackEdge{0, 1, 1, true}};
    auto plan = SignalRenderPlan::Prepare(kFormat, nodes, connections, paths);
    ASSERT_NE(plan, nullptr);
    Member a, b, target; a.nodes[0] = b.nodes[0] = 0; target.nodes[0] = 1;
    a.out.Set(1.0f + 0x1p-23f);
    const std::array members{a.Image(1), b.Image(1), target.Image(1)};
    for (std::uint32_t epoch = 0; epoch < 16; ++epoch) {
        a.pcm.fill(16777218.0f); b.pcm.fill(-16777220.0f); target.pcm.fill(1e-6f);
        ASSERT_TRUE(plan->BeginBlock(Block(epoch)));
        ASSERT_TRUE(plan->ProcessContent(members));
        ASSERT_TRUE(plan->CommitBlock());
        for (const auto sample : target.pcm) EXPECT_FLOAT_EQ(sample, 1e-6f);
    }
}

TEST(SignalRenderPlan, TopologyProjectionKeepsRetiredBusEndpointConnectedToSilence) {
    std::array nodes{SignalRenderNodeInput{}, SignalRenderNodeInput{}, SignalRenderNodeInput{}};
    for (unsigned i = 0; i < nodes.size(); ++i) {
        nodes[i].busIndex = i;
        nodes[i].busId = nodes[i].stableId = {0, i + 1u};
        nodes[i].busGeneration = 1;
    }
    nodes[2].receiver = true;
    const std::array relations{SignalConnectionInput{1, 2, .5f, {0, 10}}};
    auto full = SignalRenderPlan::Prepare(kFormat, nodes, relations, {});
    ASSERT_NE(nullptr, full);
    full->EnableActualRouting(3);
    ASSERT_TRUE(full->ConfigurePublication(1, 100, nullptr, 0));
    const std::array selected{0u, 1u, 2u};
    std::array current{nodes[0], nodes[1], nodes[2]};
    current[2].receiver = false;
    current[1].busId = current[1].stableId = {0, 20};
    current[1].busGeneration = 2;
    auto projected = full->PrepareProjection(selected, 3, current);
    ASSERT_NE(nullptr, projected);
    EXPECT_EQ(4u, projected->NodeCount());
    EXPECT_EQ(UINT32_MAX, projected->NodeDefinition(1).busIndex);
    EXPECT_EQ(nodes[1].busId, projected->NodeDefinition(1).busId);
    EXPECT_EQ(1u, projected->NodeDefinition(1).busGeneration);
    EXPECT_EQ(3u, projected->NodeForBus(1, current[1].busId, 2));
    EXPECT_EQ(UINT32_MAX, projected->NodeForBus(1, nodes[1].busId, 1));
    EXPECT_EQ(1u, projected->Relations()[0].source);
    ASSERT_TRUE(projected->ConfigurePublication(2, 100, full.get(), 0));
    ASSERT_TRUE(projected->AdoptContinuityFrom(*full));
    // Both old and new Bus nodes can be absent from physical processing. The
    // retained connection is still committed as fresh zero input each block.
    for (std::uint64_t epoch = 0; epoch < 4; ++epoch) {
        auto block = Block(epoch);
        block.world = projected->WorldIdentity();
        ASSERT_TRUE(projected->BeginBlock(block));
        ASSERT_TRUE(projected->ProcessContent({}));
        ASSERT_TRUE(projected->CommitBlock());
    }
}

} // namespace

TEST(SignalRenderPlan, FormalDefinitionProofUsesCarrierOwnershipAndBusIdentity) {
    const VnObjectId sound{0, 1}, parent{0, 2}, bus{0, 3};
    const ProgramSourceIdentityV1 source{5, 10, 20, 30, kCurrentWorldBodyVintageV20};
    StructuralAncestorRefV1 ancestor;
    ancestor.contentPropertyKind = playableproperties::ContentPropertyKindV1::Container;
    ancestor.stableId = parent;
    ancestor.programSemanticFingerprint = 22;
    PlayablePropertyProgramViewV1 property;
    property.stableId = sound;
    property.programSemanticFingerprint = 11;
    property.structuralAncestorCount = 1;
    property.structuralAncestors = &ancestor;
    const std::array properties{SignalPropertyInput{&property, source}};
    SignalRelationV1 relation;
    relation.stableId = {0, 4};
    relation.source = {SignalEndpointKindV1::Container, 0, parent, 22, 33};
    relation.target = {SignalEndpointKindV1::Bus, 0, bus};
    SignalReceiverV1 receiver;
    receiver.target = relation.target;
    const std::array buses{SignalBusInput{bus, 7}};
    auto plan = PrepareSignalWorldPlan(kFormat, {&relation, 1}, {&receiver, 1}, properties, buses);
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->NodeForBus(0, bus, 7), 0u);
    EXPECT_EQ(plan->NodeForBus(0, bus, 8), UINT32_MAX);
    EXPECT_EQ(plan->NodeForBus(0, {0, 99}, 7), UINT32_MAX);
    PlaybackPathRow row;
    row.stableId = parent;
    row.rowKind = ResolvedPlayablePathRowKindV1::Definition;
    row.contentPropertyKind = playableproperties::ContentPropertyKindV1::Container;
    row.propertyFingerprint = 22;
    row.source = source;
    std::array<std::uint32_t, 1> matched{};
    EXPECT_EQ(plan->MatchMember({&row, 1}, matched), 1u);
    ++row.source.leaseIdentity;
    ++row.source.worldGeneration;
    EXPECT_EQ(plan->MatchMember({&row, 1}, matched), 1u);
    ++row.source.ownerRevision;
    EXPECT_EQ(plan->MatchMember({&row, 1}, matched), 0u);
    relation.target.stableId = {0, 99};
    receiver.target = relation.target;
    EXPECT_EQ(PrepareSignalWorldPlan(kFormat, {&relation, 1}, {&receiver, 1}, properties, buses), nullptr);
}

TEST(SignalRenderPlan, QueuedParameterPublicationPreservesPcmEnvelopeAndRunningSendRamp) {
    std::array nodes{SignalRenderNodeInput{SignalEndpointKindV1::Sound},
                     SignalRenderNodeInput{SignalEndpointKindV1::Sound, true}};
    nodes[0].stableId = {0, 1}; nodes[1].stableId = {0, 2};
    std::array connections{SignalConnectionInput{0, 1, .025f, {0, 3}}};
    const std::array paths{SignalFeedbackEdge{0, 1, 1, true}};
    auto old = SignalRenderPlan::Prepare(kFormat, nodes, connections, paths);
    auto reference = SignalRenderPlan::Prepare(kFormat, nodes, connections, paths);
    ASSERT_TRUE(old->ConfigurePublication(10, 100, nullptr, 256));
    connections[0].level = .1f;
    auto next = SignalRenderPlan::Prepare(kFormat, nodes, connections, paths);
    auto queued = SignalRenderPlan::Prepare(kFormat, nodes, connections, paths);
    ASSERT_TRUE(next->ConfigurePublication(11, 100, old.get(), 256));
    ASSERT_TRUE(queued->ConfigurePublication(12, 100, next.get(), 256));
    Member source, target, refSource, refTarget;
    source.nodes[0] = refSource.nodes[0] = 0;
    target.nodes[0] = refTarget.nodes[0] = 1;
    const std::array members{source.Image(1), target.Image(1)};
    const std::array refMembers{refSource.Image(1), refTarget.Image(1)};
    auto* current = old.get();
    for (std::uint64_t epoch = 0; epoch < 12; ++epoch) {
        if (epoch == 4) {
            ASSERT_TRUE(next->AdoptContinuityFrom(*old));
            ASSERT_TRUE(reference->SetConnectionLevel(0, .1f, 256));
            current = next.get();
        }
        if (epoch == 5) {
            ASSERT_TRUE(queued->AdoptContinuityFrom(*next));
            EXPECT_FALSE(queued->AdoptContinuityFrom(*next));
            current = queued.get();
        }
        source.pcm.fill(.5f + static_cast<float>(epoch) * .02f);
        refSource.pcm = source.pcm;
        target.pcm.fill(1); refTarget.pcm.fill(1);
        auto block = Block(epoch); block.world = current->WorldIdentity();
        ASSERT_TRUE(current->BeginBlock(block));
        ASSERT_TRUE(current->ProcessContent(members));
        ASSERT_TRUE(current->CommitBlock());
        ASSERT_TRUE(reference->BeginBlock(Block(epoch)));
        ASSERT_TRUE(reference->ProcessContent(refMembers));
        ASSERT_TRUE(reference->CommitBlock());
        EXPECT_EQ(target.pcm, refTarget.pcm);
    }
    auto differentWorld = SignalRenderPlan::Prepare(kFormat, nodes, connections, paths);
    ASSERT_TRUE(differentWorld->ConfigurePublication(13, 101, queued.get(), 256));
    EXPECT_FALSE(differentWorld->AdoptContinuityFrom(*queued));
    EXPECT_EQ(differentWorld->WorldIdentity(), 13u);
    connections[0].stableId = {0, 4};
    auto differentRelation = SignalRenderPlan::Prepare(kFormat, nodes, connections, paths);
    ASSERT_TRUE(differentRelation->ConfigurePublication(14, 100, queued.get(), 256));
    EXPECT_TRUE(differentRelation->AdoptContinuityFrom(*queued));
    // Changing relation identity invalidates its PCM, but a surviving target
    // must release from its current gain rather than snap back to unity.
    source.pcm.fill(1); target.pcm.fill(1);
    auto block = Block(12); block.world = differentRelation->WorldIdentity();
    ASSERT_TRUE(differentRelation->BeginBlock(block));
    ASSERT_TRUE(differentRelation->ProcessContent(members));
    ASSERT_TRUE(differentRelation->CommitBlock());
    EXPECT_LT(target.pcm.front(), .9f);
    EXPECT_GT(target.pcm.back(), target.pcm.front());
}

TEST(SignalRenderPlan, ActivityProjectionReordersAndRemovesNodesWithoutRestartingSignalHistory) {
    for (bool ramp : {false, true}) {
    SCOPED_TRACE(ramp);
    const float updatedLevel = ramp ? .1f : .025f;
    std::array nodes{SignalRenderNodeInput{SignalEndpointKindV1::Sound},
                     SignalRenderNodeInput{SignalEndpointKindV1::Sound, true}};
    nodes[0].stableId = {0, 1}; nodes[1].stableId = {0, 2};
    const std::array connections{SignalConnectionInput{0, 1, .025f, {0, 3}}};
    const std::array paths{SignalFeedbackEdge{0, 1, 1, true}};
    auto old = SignalRenderPlan::Prepare(kFormat, nodes, connections, paths);
    auto reference = SignalRenderPlan::Prepare(kFormat, nodes, connections, paths);
    ASSERT_NE(nullptr, old); ASSERT_NE(nullptr, reference);
    ASSERT_TRUE(old->ConfigurePublication(10, 100, nullptr, 256));

    std::array expanded{nodes[1], nodes[0], nodes[1]};
    expanded[0].stableId = {0, 4};
    const std::array expandedConnections{
        SignalConnectionInput{1, 0, updatedLevel, {0, 5}},
        SignalConnectionInput{1, 2, updatedLevel, {0, 3}}};
    const std::array expandedPaths{SignalFeedbackEdge{1, 0, 1, true},
                                   SignalFeedbackEdge{1, 2, 1, true}};
    auto next = SignalRenderPlan::Prepare(kFormat, expanded, expandedConnections, expandedPaths);
    ASSERT_NE(nullptr, next);
    ASSERT_TRUE(next->ConfigurePublication(11, 100, old.get(), 256));
    const std::array compact{nodes[1], nodes[0]};
    const std::array compactConnections{SignalConnectionInput{1, 0, updatedLevel, {0, 3}}};
    const std::array compactPaths{SignalFeedbackEdge{1, 0, 1, true}};
    auto queued = SignalRenderPlan::Prepare(kFormat, compact, compactConnections, compactPaths);
    ASSERT_NE(nullptr, queued);
    ASSERT_TRUE(queued->ConfigurePublication(12, 100, next.get(), 256));

    Member source, target, added, refSource, refTarget;
    refSource.nodes[0] = 0; refTarget.nodes[0] = 1;
    auto* current = old.get();
    for (std::uint64_t epoch = 0; epoch < 14; ++epoch) {
        if (epoch == 4) {
            ASSERT_TRUE(next->AdoptContinuityFrom(*old));
            ASSERT_TRUE(reference->SetConnectionLevel(0, updatedLevel, 256));
            current = next.get();
        }
        if (epoch == 5) {
            // This candidate was prepared before next adopted its predecessor.
            ASSERT_TRUE(queued->AdoptContinuityFrom(*next));
            current = queued.get();
        }
        source.nodes[0] = epoch < 4 ? 0 : 1;
        target.nodes[0] = epoch < 4 ? 1 : epoch == 4 ? 2 : 0;
        added.nodes[0] = 0;
        source.pcm.fill(.5f + static_cast<float>(epoch) * .02f);
        refSource.pcm = source.pcm;
        target.pcm.fill(1); refTarget.pcm.fill(1); added.pcm.fill(1);
        std::vector<SignalMemberImage> members{target.Image(1)};
        std::vector<SignalMemberImage> refMembers{refTarget.Image(1)};
        // Commit must publish fresh silence when a continuing source is absent.
        if (epoch != 7) {
            members.push_back(source.Image(1));
            refMembers.push_back(refSource.Image(1));
        }
        if (epoch == 4) members.push_back(added.Image(1));
        auto block = Block(epoch); block.world = current->WorldIdentity();
        ASSERT_TRUE(current->BeginBlock(block));
        ASSERT_TRUE(current->ProcessContent(members));
        ASSERT_TRUE(current->CommitBlock());
        ASSERT_TRUE(reference->BeginBlock(Block(epoch)));
        ASSERT_TRUE(reference->ProcessContent(refMembers));
        ASSERT_TRUE(reference->CommitBlock());
        EXPECT_EQ(target.pcm, refTarget.pcm) << epoch;
        if (epoch == 4) for (float sample : added.pcm) EXPECT_FLOAT_EQ(1, sample);
    }
    }
}

TEST(SignalRenderPlan, WakeSelectsAcceptedOrRejectedSleepAndThenOneSuccessor) {
    for (bool acceptSleep : {false, true}) for (bool sameBlock : {false, true}) {
        SCOPED_TRACE(acceptSleep);
        SCOPED_TRACE(sameBlock);
        std::array nodes{SignalRenderNodeInput{SignalEndpointKindV1::Sound},
                         SignalRenderNodeInput{SignalEndpointKindV1::Sound, true},
                         SignalRenderNodeInput{SignalEndpointKindV1::Sound, true}};
        for (unsigned i = 0; i != nodes.size(); ++i) nodes[i].stableId = {0, i + 1u};
        const std::array relations{SignalConnectionInput{0, 1, .025f, {0, 4}}};
        const std::array paths{SignalFeedbackEdge{0, 1, .025f, true}};
        auto active = SignalRenderPlan::Prepare(kFormat, nodes, relations, paths);
        auto reference = SignalRenderPlan::Prepare(kFormat, nodes, relations, paths);
        ASSERT_NE(nullptr, active); ASSERT_NE(nullptr, reference);
        ASSERT_TRUE(active->ConfigurePublication(10, 100, nullptr, 256));
        const std::array compact{nodes[0], nodes[1]};
        auto sleep = SignalRenderPlan::Prepare(kFormat, compact, relations, paths);
        ASSERT_NE(nullptr, sleep);
        ASSERT_TRUE(sleep->ConfigurePublication(11, 100, active.get(), 256));
        const std::array reordered{nodes[2], nodes[1], nodes[0]};
        const std::array wakeRelations{SignalConnectionInput{2, 1, .025f, {0, 4}}};
        const std::array wakePaths{SignalFeedbackEdge{2, 1, .025f, true}};
        auto wake = SignalRenderPlan::Prepare(kFormat, reordered, wakeRelations, wakePaths);
        ASSERT_NE(nullptr, wake);
        ASSERT_TRUE(wake->ConfigurePublication(12, 100, sleep.get(), 256, active.get()));
        auto successor = SignalRenderPlan::Prepare(kFormat, nodes, relations, paths);
        ASSERT_NE(nullptr, successor);
        ASSERT_TRUE(successor->ConfigurePublication(13, 100, wake.get(), 256));
        auto stranger = SignalRenderPlan::Prepare(kFormat, nodes, relations, paths);
        ASSERT_TRUE(stranger->ConfigurePublication(99, 100, nullptr, 256));
        EXPECT_FALSE(wake->AdoptContinuityFrom(*stranger));
        DuckingDormantState fixed;
        fixed.channels = 1; fixed.sampleRate = kFormat.sampleRate;
        fixed.gain = std::nextafter(1.0, 0.0);
        ASSERT_TRUE(wake->PrepareDormantReceiver(0, fixed));
        Member source, target, idle, refSource, refTarget, refIdle;
        refSource.nodes[0] = 0; refTarget.nodes[0] = 1; refIdle.nodes[0] = 2;
        auto* current = active.get();
        const unsigned wakeAt = sameBlock ? 4u : 6u;
        for (unsigned epoch = 0; epoch != 12; ++epoch) {
            if (epoch == 2) {
                ASSERT_TRUE(active->SetConnectionLevel(0, .1f, 512));
                ASSERT_TRUE(reference->SetConnectionLevel(0, .1f, 512));
            }
            if (epoch == 4 && acceptSleep) {
                ASSERT_TRUE(sleep->AdoptContinuityFrom(*current));
                current = sleep.get();
            }
            if (epoch == wakeAt) {
                ASSERT_TRUE(wake->AdoptContinuityFrom(*current));
                EXPECT_FALSE(wake->AdoptContinuityFrom(*current));
                current = wake.get();
            }
            if (epoch == wakeAt + 1) {
                ASSERT_TRUE(successor->AdoptContinuityFrom(*current));
                current = successor.get();
            }
            source.nodes[0] = current == wake.get() ? 2 : 0;
            target.nodes[0] = 1;
            idle.nodes[0] = current == wake.get() ? 0 : 2;
            source.pcm.fill(.4f + epoch * .03f); refSource.pcm = source.pcm;
            target.pcm.fill(.8f); refTarget.pcm = target.pcm;
            idle.pcm.fill(0); refIdle.pcm.fill(0);
            std::vector<SignalMemberImage> members{source.Image(1), target.Image(1)};
            if (current != sleep.get()) members.push_back(idle.Image(1));
            const std::array refMembers{refSource.Image(1), refTarget.Image(1), refIdle.Image(1)};
            auto block = Block(epoch); block.world = current->WorldIdentity();
            ASSERT_TRUE(current->BeginBlock(block));
            ASSERT_TRUE(current->ProcessContent(members));
            ASSERT_TRUE(current->CommitBlock());
            ASSERT_TRUE(reference->BeginBlock(Block(epoch)));
            ASSERT_TRUE(reference->ProcessContent(refMembers));
            ASSERT_TRUE(reference->CommitBlock());
            EXPECT_EQ(target.pcm, refTarget.pcm) << epoch;
            if (epoch == wakeAt) {
                DuckingDormantState saved;
                ASSERT_TRUE(current->CaptureDormantReceiver(0, false, saved));
                // Accepted sleep keeps the seeded residual. Rejected sleep
                // must use the actual live receiver, not overwrite it with cold data.
                EXPECT_EQ(std::bit_cast<std::uint64_t>(acceptSleep ? fixed.gain : 1.0),
                          std::bit_cast<std::uint64_t>(saved.gain));
            }
        }
    }
}

TEST(SignalRenderPlan, ActivityProjectionPreservesAuthoredRecoveryAndAllCarrierProofs) {
    std::array nodes{SignalRenderNodeInput{SignalEndpointKindV1::Sound},
                     SignalRenderNodeInput{SignalEndpointKindV1::Container, true, 37, 777},
                     SignalRenderNodeInput{SignalEndpointKindV1::Bus}};
    for (unsigned i = 0; i < nodes.size(); ++i) nodes[i].stableId = {0, i + 1u};
    nodes[2].busIndex = 31;
    const std::array relations{SignalConnectionInput{0, 1, .4f, {0, 4}}};
    const std::array paths{SignalFeedbackEdge{0, 1, .4f, true}};
    std::array<SignalMemberProof, 3> proofs{};
    proofs[0].stableId = nodes[0].stableId; proofs[0].propertyFingerprint = 11; proofs[0].node = 0;
    proofs[1].stableId = nodes[1].stableId; proofs[1].propertyFingerprint = 22; proofs[1].node = 1;
    proofs[2] = proofs[1]; proofs[2].source.ownerStableId = 10; proofs[2].source.ownerRevision = 2;
    auto full = SignalRenderPlan::Prepare(kFormat, nodes, relations, paths, proofs);
    ASSERT_NE(nullptr, full);
    const std::array selected{1u, 0u};
    auto projection = full->PrepareProjection(selected, 32);
    ASSERT_NE(nullptr, projection);
    EXPECT_EQ(2u, projection->NodeCount());
    EXPECT_EQ(3u, projection->Membership().size());
    EXPECT_FLOAT_EQ(37, projection->NodeDefinition(0).attackMs);
    EXPECT_FLOAT_EQ(777, projection->NodeDefinition(0).releaseMs);
    EXPECT_EQ(1u, projection->Relations()[0].source);
    EXPECT_EQ(0u, projection->Relations()[0].target);
    Member source, target, refSource, refTarget;
    source.nodes[0] = 1; target.nodes[0] = 0;
    refSource.nodes[0] = 0; refTarget.nodes[0] = 1;
    const std::array members{source.Image(1), target.Image(1)};
    const std::array reference{refSource.Image(1), refTarget.Image(1)};
    for (unsigned epoch = 0; epoch < 40; ++epoch) {
        source.pcm.fill(epoch < 12 ? .7f : 0); refSource.pcm = source.pcm;
        target.pcm.fill(.5f); refTarget.pcm = target.pcm;
        ASSERT_TRUE(projection->BeginBlock(Block(epoch)));
        ASSERT_TRUE(projection->ProcessContent(members));
        ASSERT_TRUE(projection->CommitBlock());
        ASSERT_TRUE(full->BeginBlock(Block(epoch)));
        ASSERT_TRUE(full->ProcessContent(reference));
        ASSERT_TRUE(full->CommitBlock());
        EXPECT_EQ(target.pcm, refTarget.pcm) << epoch;
    }
    const std::array severed{1u};
    EXPECT_EQ(nullptr, full->PrepareProjection(severed, 32));
    const std::array duplicate{0u, 0u};
    EXPECT_EQ(nullptr, full->PrepareProjection(duplicate, 32));
    const std::array highBus{2u};
    EXPECT_EQ(nullptr, full->PrepareProjection(highBus, 31));
}


TEST(SignalRenderPlan, ColdProjectionPreservesReceiverConnectionAndActualResetHistory) {
    for (bool accepted : {false, true}) for (bool sameBlock : {false, true})
    for (bool reset : {false, true}) {
        SCOPED_TRACE(accepted);
        SCOPED_TRACE(sameBlock);
        SCOPED_TRACE(reset);
        std::array nodes{SignalRenderNodeInput{SignalEndpointKindV1::Sound},
                         SignalRenderNodeInput{SignalEndpointKindV1::Sound, true},
                         SignalRenderNodeInput{SignalEndpointKindV1::Sound}};
        for (unsigned i = 0; i < nodes.size(); ++i) nodes[i].stableId = {0, i + 1u};
        std::array relations{SignalConnectionInput{0, 1, .025f, {0, 4}}};
        const std::array paths{SignalFeedbackEdge{0, 1, .025f, true}};
        const std::array keeper{nodes[2]};
        auto boot = SignalRenderPlan::Prepare(kFormat, keeper, {}, {});
        ASSERT_TRUE(boot->ConfigurePublication(1, 100, nullptr, 256));
        auto first = Block(0); first.world = boot->WorldIdentity();
        ASSERT_TRUE(boot->BeginBlock(first));
        ASSERT_TRUE(boot->ProcessContent({})); ASSERT_TRUE(boot->CommitBlock());
        auto active = SignalRenderPlan::Prepare(kFormat, nodes, relations, paths);
        auto reference = SignalRenderPlan::Prepare(kFormat, nodes, relations, paths);
        ASSERT_TRUE(active->ConfigurePublication(2, 100, boot.get(), 256));
        ASSERT_TRUE(reference->ConfigurePublication(2, 100, boot.get(), 256));
        DuckingDormantState fixed;
        fixed.channels = 1; fixed.sampleRate = kFormat.sampleRate;
        fixed.gain = std::nextafter(1.0, 0.0);
        ASSERT_TRUE(active->PrepareDormantReceiver(1, fixed));
        ASSERT_TRUE(reference->PrepareDormantReceiver(1, fixed));
        ASSERT_TRUE(active->AdoptContinuityFrom(*boot));
        ASSERT_TRUE(reference->AdoptContinuityFrom(*boot));
        auto sleep = SignalRenderPlan::Prepare(kFormat, keeper, {}, {});
        ASSERT_TRUE(sleep->ConfigurePublication(3, 100, active.get(), 256));
        auto cold = sleep->PrepareDormantState(*active);
        ASSERT_NE(nullptr, cold);
        relations[0].level = .1f;
        auto wake = SignalRenderPlan::Prepare(kFormat, nodes, relations, paths);
        ASSERT_TRUE(wake->ConfigurePublication(4, 100, sleep.get(), 256, active.get()));
        ASSERT_TRUE(wake->PrepareDormantContinuity(*sleep, cold));
        // An accepted branch cannot restore an unfilled ticket. Refusal leaves
        // the candidate usable with its real active predecessor later.
        EXPECT_FALSE(wake->AdoptContinuityFrom(*sleep));
        Member source, target, refSource, refTarget;
        source.nodes[0] = refSource.nodes[0] = 0;
        target.nodes[0] = refTarget.nodes[0] = 1;
        const std::array members{source.Image(1), target.Image(1)};
        const std::array refMembers{refSource.Image(1), refTarget.Image(1)};
        auto* current = active.get();
        const unsigned wakeAt = sameBlock ? 3u : 5u;
        for (unsigned epoch = 1; epoch < 14; ++epoch) {
            if (epoch == 3 && accepted) {
                ASSERT_TRUE(sleep->CaptureDormantState(*active, *cold));
                ASSERT_TRUE(sleep->AdoptContinuityFrom(*active));
                current = sleep.get();
            }
            if (epoch == wakeAt) {
                if (reset) { current->Invalidate(); reference->Invalidate(); }
                ASSERT_TRUE(reference->SetConnectionLevel(0, .1f, 256));
                // The wake lease owns the immutable payload even if the
                // control side has already consumed the sleep acknowledgement.
                cold.reset();
                ASSERT_TRUE(wake->AdoptContinuityFrom(*current));
                current = wake.get();
                if (!reset) {
                    DuckingDormantState restored;
                    ASSERT_TRUE(current->CaptureDormantReceiver(1, false, restored));
                    EXPECT_EQ(std::bit_cast<std::uint64_t>(restored.gain),
                              std::bit_cast<std::uint64_t>(fixed.gain));
                }
            }
            source.pcm.fill(epoch >= wakeAt ? .5f : 0);
            target.pcm.fill(epoch >= wakeAt ? .75f : 0);
            refSource.pcm = source.pcm; refTarget.pcm = target.pcm;
            auto block = Block(epoch); block.world = current->WorldIdentity();
            ASSERT_TRUE(current->BeginBlock(block));
            ASSERT_TRUE(current->ProcessContent(current == sleep.get()
                ? std::span<const SignalMemberImage>{} : std::span<const SignalMemberImage>{members}));
            ASSERT_TRUE(current->CommitBlock());
            ASSERT_TRUE(reference->BeginBlock(block));
            ASSERT_TRUE(reference->ProcessContent(refMembers));
            ASSERT_TRUE(reference->CommitBlock());
            if (current != sleep.get()) EXPECT_EQ(target.pcm, refTarget.pcm) << epoch;
            if (epoch == 2) {
                DuckingDormantState proof;
                ASSERT_TRUE(current->CaptureDormantReceiver(1, false, proof));
                EXPECT_EQ(std::bit_cast<std::uint64_t>(proof.gain), std::bit_cast<std::uint64_t>(fixed.gain));
            }
        }
    }
}

TEST(SignalRenderPlan, ColdCaptureRefusesRealInputSourceHistoryAndConnectionRamp) {
    const std::array nodes{SignalRenderNodeInput{SignalEndpointKindV1::Sound, false, 8, 80,
                              UINT32_MAX, {}, 0, {0, 1}},
                          SignalRenderNodeInput{SignalEndpointKindV1::Sound, true, 8, 80,
                              UINT32_MAX, {}, 0, {0, 2}}};
    const std::array relations{SignalConnectionInput{0, 1, 0, {0, 3}}};
    auto active = SignalRenderPlan::Prepare(kFormat, nodes, relations, {});
    ASSERT_TRUE(active->ConfigurePublication(1, 100, nullptr, 256));
    auto sleep = SignalRenderPlan::Prepare(kFormat, {}, {}, {});
    ASSERT_TRUE(sleep->ConfigurePublication(2, 100, active.get(), 256));
    auto cold = sleep->PrepareDormantState(*active);
    ASSERT_NE(nullptr, cold);
    Member source, target;
    source.nodes[0] = 0; target.nodes[0] = 1;
    const std::array members{source.Image(1), target.Image(1)};
    source.pcm.fill(1); target.pcm.fill(0);
    ASSERT_TRUE(active->BeginBlock(Block(0)));
    ASSERT_TRUE(active->ProcessContent(members)); ASSERT_TRUE(active->CommitBlock());
    // A zero connection level hides the source in the detector, but its
    // preceding PCM must still prevent removal of the producer history.
    EXPECT_FALSE(sleep->CaptureDormantState(*active, *cold));
    source.pcm.fill(0);
    ASSERT_TRUE(active->BeginBlock(Block(1)));
    ASSERT_TRUE(active->ProcessContent(members)); ASSERT_TRUE(active->CommitBlock());
    ASSERT_TRUE(active->SetConnectionLevel(0, .5f, 256));
    EXPECT_FALSE(sleep->CaptureDormantState(*active, *cold));
    ASSERT_TRUE(active->SetConnectionLevel(0, 0, 0));
    ASSERT_TRUE(sleep->CaptureDormantState(*active, *cold));
    EXPECT_FALSE(sleep->CaptureDormantState(*active, *cold));
}

TEST(SignalRenderPlan, ActivityObservationCopiesOnePinnedBlockAndKeepsUnrelatedHold) {
    std::array<SignalRenderNodeInput, 4> nodes{};
    for (unsigned i = 0; i < nodes.size(); ++i) {
        nodes[i].kind = SignalEndpointKindV1::Sound;
        nodes[i].stableId = {0, i + 1u};
        nodes[i].receiver = (i & 1) != 0;
    }
    const std::array relations{SignalConnectionInput{0, 1, 1, {0, 10}},
        SignalConnectionInput{2, 3, 1, {0, 11}}};
    auto plan = SignalRenderPlan::Prepare(kFormat, nodes, relations, {});
    ASSERT_TRUE(plan->ConfigurePublication(1, 100, nullptr, 256));
    std::atomic<bool> done{false}, valid{true};
    std::thread audio([&] {
        for (unsigned epoch = 0; epoch != 1000; ++epoch)
            if (!plan->BeginBlock(Block(epoch)) || !plan->ProcessContent({}) || !plan->CommitBlock())
                valid.store(false);
        done.store(true);
    });
    SignalRenderPlan::ActivityObservation observed;
    unsigned captures = 0;
    bool coherent = true;
    do {
        if (plan->CaptureActivityObservation(observed)) {
            ++captures;
            for (const auto& node : observed.nodes)
                coherent &= node.ready && node.quietFrames == (observed.blockSequence - 1) * kFrames;
        }
    } while (!done.load());
    audio.join();
    EXPECT_TRUE(valid.load());
    EXPECT_TRUE(coherent);
    ASSERT_GT(captures, 0u);
    ASSERT_TRUE(plan->SetConnectionLevel(0, .5f, 0));
    ASSERT_TRUE(plan->BeginBlock(Block(1000)));
    ASSERT_TRUE(plan->ProcessContent({}));
    ASSERT_TRUE(plan->CommitBlock());
    ASSERT_TRUE(plan->CaptureActivityObservation(observed));
    EXPECT_EQ(observed.blockSequence, 1001u);
    EXPECT_EQ(observed.nodes[0].quietFrames, 0u);
    EXPECT_EQ(observed.nodes[1].quietFrames, 0u);
    EXPECT_EQ(observed.nodes[2].quietFrames, 1000u * kFrames);
    EXPECT_EQ(observed.nodes[3].quietFrames, 1000u * kFrames);
}

TEST(SignalRenderPlan, ColdRegistryPrunesOnlyRestoredComponents) {
    std::array<SignalRenderNodeInput, 4> nodes{};
    for (unsigned i = 0; i < nodes.size(); ++i) {
        nodes[i].kind = SignalEndpointKindV1::Sound;
        nodes[i].stableId = {0, i + 1u};
        nodes[i].receiver = (i & 1) != 0;
    }
    const std::array relations{SignalConnectionInput{0, 1, .1f, {0, 10}},
        SignalConnectionInput{2, 3, .2f, {0, 11}}};
    auto active = SignalRenderPlan::Prepare(kFormat, nodes, relations, {});
    ASSERT_TRUE(active->ConfigurePublication(1, 100, nullptr, 256));
    auto sleep = SignalRenderPlan::Prepare(kFormat, {}, {}, {});
    ASSERT_TRUE(sleep->ConfigurePublication(2, 100, active.get(), 256));
    auto cold = sleep->PrepareDormantState(*active);
    ASSERT_NE(cold, nullptr);
    std::shared_ptr<const SignalRenderPlan::DormantState> remaining;
    EXPECT_FALSE(sleep->PruneDormantState(cold, remaining));
    ASSERT_TRUE(active->BeginBlock(Block(0)));
    ASSERT_TRUE(active->ProcessContent({}));
    ASSERT_TRUE(active->CommitBlock());
    ASSERT_TRUE(sleep->CaptureDormantState(*active, *cold));
    ASSERT_TRUE(sleep->AdoptContinuityFrom(*active));
    auto partial = SignalRenderPlan::Prepare(kFormat, {nodes.data(), 2}, {relations.data(), 1}, {});
    ASSERT_TRUE(partial->ConfigurePublication(3, 100, sleep.get(), 256));
    ASSERT_TRUE(partial->PrepareDormantContinuity(*sleep, cold));
    ASSERT_TRUE(partial->PruneDormantState(cold, remaining));
    ASSERT_NE(remaining, nullptr);
    EXPECT_NE(remaining.get(), cold.get());
    ASSERT_TRUE(partial->AdoptContinuityFrom(*sleep));
    auto wake = SignalRenderPlan::Prepare(kFormat, nodes, relations, {});
    ASSERT_TRUE(wake->ConfigurePublication(4, 100, partial.get(), 256));
    ASSERT_TRUE(wake->PrepareDormantContinuity(*partial, remaining));
    cold.reset();
    ASSERT_TRUE(wake->AdoptContinuityFrom(*partial));
    ASSERT_TRUE(wake->PruneDormantState(remaining, remaining));
    EXPECT_EQ(remaining, nullptr);
}
