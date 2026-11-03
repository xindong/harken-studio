#include "harken/render/SignalRenderPlan.h"
#include "harken/render/SignalWorldPlan.h"

#include <gtest/gtest.h>
#include <array>

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
