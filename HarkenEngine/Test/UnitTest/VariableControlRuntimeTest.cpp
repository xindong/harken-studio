#include "Common/ObjectIdTestSupport.h"
#include "harken/core/VariableControlRuntime.h"

#include "Engine.h"
#include "Common/TestUtil.h"

#include <gtest/gtest.h>

#include <barrier>
#include <thread>
#include <vector>
#include <limits>
#include <utility>

namespace harken::detail {

struct VariableControlInstanceTableTestAccess {
    static bool IssueInitial(VariableControlInstanceTable& table,
        const PreparedVariableControlInstance& original, VariableControlSide side,
        VariableControlInstanceTable::OriginalInitialSideReceipt* out) {
        return table.IssueOriginalInitialSideReceipt(original, side, out);
    }
    static VariableControlInstanceStatus BeginInitial(VariableControlInstanceTable& table,
        VariableControlInstanceTable::OriginalInitialSideReceipt& original, DecisionToken replacement,
        VariableControlInstanceTable::InitialSideReplacement* out) {
        return table.BeginInitialSideReplacement(original, replacement, out);
    }
    static bool MatchesInitial(const VariableControlInstanceTable& table,
        const VariableControlInstanceTable::OriginalInitialSideReceipt& original,
        VariableControlInstanceToken instance, VariableControlSide side,
        DecisionToken decision, BankVersionToken bank) {
        return table.MatchesOriginalInitialSideReceipt(original, instance, side, decision, bank);
    }
    static bool FinishInitial(VariableControlInstanceTable& table,
        VariableControlInstanceTable::InitialSideReplacement& replacement, bool commit) {
        return table.FinishInitialSideReplacement(replacement, commit);
    }
    static bool SeedFreeGeneration(VariableControlInstanceTable& table, std::uint32_t slot,
                                  std::uint32_t generation) {
        if (slot >= table.capacity_ || generation == 0 || !table.entries_.ConstructThrough(slot + 1u) ||
            table.entries_[slot].state.load() != VariableControlInstanceTable::SlotState::Free) return false;
        table.entries_[slot].generation.store(generation);
        return true;
    }
    static bool ChangePreparingRoot(VariableControlInstanceTable& table,
                                   VariableControlInstanceToken token) {
        auto* entry = table.ResolvePreparing(token);
        if (entry == nullptr || entry->state.load() != VariableControlInstanceTable::SlotState::Initializing)
            return false;
        entry->rootInstance.fetch_add(1);
        return true;
    }
    static bool ChangeHeldBank(VariableControlInstanceTable& table,
        VariableControlInstanceToken token, bool restore) {
        auto* entry = table.Resolve(token);
        if (entry == nullptr || entry->discrete.initialReplacement.load() == nullptr) return false;
        if (restore) entry->discrete.newestObserved.fetch_sub(1);
        else entry->discrete.newestObserved.fetch_add(1);
        return true;
    }
    static bool SeedPreparingSideGeneration(VariableControlInstanceTable& table,
        VariableControlInstanceToken token, std::uint64_t generation) {
        auto* entry = table.ResolvePreparing(token);
        if (entry == nullptr || entry->state.load() != VariableControlInstanceTable::SlotState::Initializing)
            return false;
        entry->discrete.generation.store(generation);
        return true;
    }
    static bool SeedSideGeneration(VariableControlInstanceTable& table,
                                   VariableControlInstanceToken instance,
                                   VariableControlSide side,
                                   std::uint64_t generation) {
        VariableControlInstanceTable::Entry* entry = table.Resolve(instance);
        if (entry == nullptr || generation == 0) return false;
        VariableControlInstanceTable::SideRow& target =
            side == VariableControlSide::Discrete
                ? entry->discrete
                : entry->continuous;
        target.generation.store(generation, std::memory_order_relaxed);
        return true;
    }
};

} // namespace harken::detail

namespace {

struct FrontierSideHarness {
    void Prepare(harken::VariableControlSide side,
        harken::VariableControlInstanceTable::OriginalInitialSideReceipt* initial = nullptr) {
        selected = side;
        harken::ScheduledTransactionPool::Config config{};
        config.quantumFrames = 64;
        config.scheduleHorizonFrames = 256;
        config.publishLeadQuanta = 1;
        config.maxScheduledTransactions = config.maxIngressTransactionsPerQuantum = 4;
        config.maxTransactionSpanFrames = 256;
        config.maxFrameBatchesPerQuantum = config.maxEventsPerQuantum = config.maxTransactionEvents = 8;
        config.maxCurrentSlicesPerQuantum = config.maxPlan03GroupHeadersPerPage = config.maxArmsPerGroup = 2;
        ASSERT_TRUE(pool.Reset(config, 0, 1));
        if (initial != nullptr) {
            harken::PreparedVariableControlInstance original;
            ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
                instances.PrepareAcquire({1, 1, harkentest::ObjectId(9), 7}, &original));
            ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
                instances.PrepareSide(original, side, decision, bank));
            ASSERT_TRUE(harken::detail::VariableControlInstanceTableTestAccess::IssueInitial(
                instances, original, side, initial));
            ASSERT_TRUE(instances.CommitPreparedAcquire(&original, &instance));
        } else {
            ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
                instances.Acquire({1, 1, harkentest::ObjectId(9), 7}, &instance));
            ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
                instances.PrepareSide(instance, side, decision, bank));
        }
        harken::Plan03GroupSliceInput input{};
        input.decision = decision;
        ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed, pool.ReservePlan03GroupSlice(input, arms));
        ASSERT_EQ(1u, pool.CaptureLivePlan03Groups(&group, 1));
        harken::FrontierGroupBinding binding{};
        binding.role = harken::FrontierGroupRole::Future;
        binding.entry = {{0, 1}, 1, {1, 1, 0, 0, 1, 1, 0, 1}};
        binding.operation.kind = harken::FrontierOperationKind::Dependent;
        binding.operation.owner = binding.entry.owner;
        binding.operation.order.logicCommitSequence = 1;
        binding.operation.group = group.identity;
        binding.operation.world = binding.operation.transport = 1;
        binding.variableSide = {instance.slot, instance.generation, static_cast<std::uint8_t>(side),
            2, bank.slot, bank.generation};
        ASSERT_TRUE(pool.BindPlan03GroupFrontier(group.identity, binding));
        ASSERT_TRUE(pool.ResolvePlan03GroupSliceProducer(group.identity, &group));
        ASSERT_TRUE(instances.BindFrontierSide(instance, *group.frontier, pool));
        pool.Publish(64, nullptr, nullptr, &arms);
        ASSERT_TRUE(pool.NotePlan03GroupReadRt(group.identity, 1));
    }
    static bool Validate(void*) noexcept { return true; }
    static void AfterClaim(void* opaque) noexcept {
        auto& h = *static_cast<FrontierSideHarness*>(opaque);
        EXPECT_FALSE(h.instances.CancelSide(h.instance, h.selected, h.decision));
        harken::DecisionToken superseded{};
        EXPECT_EQ(harken::VariableControlInstanceStatus::Stale,
            h.instances.PrepareNewerSide(h.instance, h.selected, {6, 7}, {h.bank.slot, 4}, &superseded));
        EXPECT_FALSE(h.instances.Release(h.instance));
    }
    static void Apply(void* opaque) noexcept {
        auto& h = *static_cast<FrontierSideHarness*>(opaque);
        h.instances.ApplyFrontierSide(h.proof);
    }
    harken::ScheduledTransactionPool pool;
    harken::TimelineArmTable arms{1};
    harken::VariableControlInstanceTable instances{1};
    harken::VariableControlInstanceToken instance{};
    harken::VariableControlSide selected{};
    harken::DecisionToken decision{3, 4};
    harken::BankVersionToken bank{2, 3};
    harken::Plan03PageGroupView group{};
    harken::VariableControlInstanceTable::FrontierSideProof proof;
};

TEST(VariableControlInstance, FrontierActualSideClaimDefersCancelAndRetainsOriginalBank) {
    for (const auto side : {harken::VariableControlSide::Discrete, harken::VariableControlSide::Continuous}) {
        FrontierSideHarness h;
        h.Prepare(side);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        ASSERT_TRUE(h.instances.ProveFrontierSide(*h.group.frontier, *h.group.lease, &h.proof));
        const harken::AudioSliceOperation operation{&h, &h.Validate, &h.AfterClaim, &h.Apply};
        EXPECT_EQ(harken::AudioSliceApplyStatus::AppliedWithDeferredInvalidation,
            harken::ApplyAudioSliceWhole(*h.group.lease, &operation, 1));
        h.proof.Release();
        harken::VariableControlInstanceSnapshot snapshot{};
        ASSERT_TRUE(h.instances.Capture(h.instance, &snapshot));
        const auto& actual = side == harken::VariableControlSide::Discrete ? snapshot.discrete : snapshot.continuous;
        EXPECT_EQ(harken::VariableControlSideState::Committed, actual.state);
        EXPECT_EQ(h.bank.generation, actual.lastApplied.generation);
        EXPECT_FALSE(h.instances.Release(h.instance));
        EXPECT_FALSE(h.pool.DetachPlan03GroupFrontier(h.group.identity, 1));
        ASSERT_TRUE(h.pool.DetachPlan03GroupFrontier(h.group.identity, 2));
        EXPECT_TRUE(h.instances.Release(h.instance));
    }
}

TEST(VariableControlInstance, FrontierCancelBeforeClaimRejectsWholeOperationWithoutSideCommit) {
    FrontierSideHarness h;
    h.Prepare(harken::VariableControlSide::Discrete);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
    auto wrong = *h.group.frontier;
    ++wrong.variableSide.bankGeneration;
    EXPECT_FALSE(h.instances.ProveFrontierSide(wrong, *h.group.lease, &h.proof));
    wrong = *h.group.frontier;
    wrong.variableSide.side = static_cast<std::uint8_t>(
        harken::VariableControlSide::Continuous);
    EXPECT_FALSE(h.instances.ProveFrontierSide(wrong, *h.group.lease, &h.proof));
    wrong = *h.group.frontier;
    ++wrong.variableSide.generation;
    EXPECT_FALSE(h.instances.ProveFrontierSide(wrong, *h.group.lease, &h.proof));
    wrong = *h.group.frontier;
    ++wrong.operation.owner.serviceGeneration;
    EXPECT_FALSE(h.instances.ProveFrontierSide(wrong, *h.group.lease, &h.proof));
    ASSERT_TRUE(h.instances.ProveFrontierSide(*h.group.frontier, *h.group.lease, &h.proof));
    EXPECT_FALSE(h.instances.CancelSide(h.instance, h.selected, {3, 5}));
    EXPECT_EQ(harken::AudioSliceState::Ready, h.group.lease->Capture().state);
    EXPECT_FALSE(h.instances.CancelSide(h.instance, h.selected, h.decision));
    const harken::AudioSliceOperation operation{&h, &h.Validate, nullptr, &h.Apply};
    EXPECT_EQ(harken::AudioSliceApplyStatus::SliceUnavailable,
        harken::ApplyAudioSliceWhole(*h.group.lease, &operation, 1));
    h.proof.Release();
    harken::VariableControlInstanceSnapshot snapshot{};
    ASSERT_TRUE(h.instances.Capture(h.instance, &snapshot));
    EXPECT_EQ(harken::VariableControlSideState::Pending, snapshot.discrete.state);
    EXPECT_FALSE(snapshot.discrete.lastApplied.Valid());
    EXPECT_TRUE(h.instances.CancelSide(h.instance, h.selected, h.decision));
}

TEST(VariableControlWeightSegment,
     ResolvesExactEndpointsAndRetargetsFromAnalyticValue) {
    harken::WeightSegmentV1 current{};
    current.startFrame = 100;
    current.endFrame = 104;
    current.startWeight = 0.0f;
    current.endWeight = 1.0f;
    current.segmentGeneration = 9;
    current.decision = {3, 5};
    current.bank = {7, 11};

    float weight = 71.0f;
    ASSERT_TRUE(harken::ResolveWeightSegment(current, 100, &weight));
    EXPECT_FLOAT_EQ(0.0f, weight);
    ASSERT_TRUE(harken::ResolveWeightSegment(current, 101, &weight));
    EXPECT_FLOAT_EQ(1.0f / 3.0f, weight);
    ASSERT_TRUE(harken::ResolveWeightSegment(current, 102, &weight));
    EXPECT_FLOAT_EQ(2.0f / 3.0f, weight);
    ASSERT_TRUE(harken::ResolveWeightSegment(current, 103, &weight));
    EXPECT_FLOAT_EQ(1.0f, weight);
    ASSERT_TRUE(harken::ResolveWeightSegment(current, 104, &weight));
    EXPECT_FLOAT_EQ(1.0f, weight);

    harken::WeightSegmentV1 retargeted{};
    ASSERT_EQ(harken::WeightSegmentStatus::Ok,
              harken::RetargetWeightSegment(
                  &current, 102, 5, 0.25f,
                  {13, 17}, {19, 23}, &retargeted));
    EXPECT_EQ(102u, retargeted.startFrame);
    EXPECT_EQ(107u, retargeted.endFrame);
    EXPECT_FLOAT_EQ(2.0f / 3.0f, retargeted.startWeight);
    EXPECT_FLOAT_EQ(0.25f, retargeted.endWeight);
    EXPECT_EQ(10u, retargeted.segmentGeneration);
    EXPECT_EQ(13u, retargeted.decision.slot);
    EXPECT_EQ(17u, retargeted.decision.generation);
    EXPECT_EQ(19u, retargeted.bank.slot);
    EXPECT_EQ(23u, retargeted.bank.generation);
    ASSERT_TRUE(harken::ResolveWeightSegment(retargeted, 102, &weight));
    EXPECT_FLOAT_EQ(2.0f / 3.0f, weight);
    ASSERT_TRUE(harken::ResolveWeightSegment(retargeted, 106, &weight));
    EXPECT_FLOAT_EQ(0.25f, weight);

    current.segmentGeneration = UINT32_MAX;
    harken::WeightSegmentV1 untouched{};
    untouched.startFrame = 991;
    EXPECT_EQ(harken::WeightSegmentStatus::IdentityExhausted,
              harken::RetargetWeightSegment(
                  &current, 103, 5, 0.75f,
                  {13, 17}, {19, 23}, &untouched));
    EXPECT_EQ(991u, untouched.startFrame);
    EXPECT_EQ(harken::WeightSegmentStatus::Invalid,
              harken::RetargetWeightSegment(
                  nullptr, 103, 1, 0.75f,
                  {13, 17}, {19, 23}, &untouched));
    EXPECT_EQ(991u, untouched.startFrame);
}

TEST(VariableControlCadence,
     ResolvesAbsoluteFrameGridWithoutUpdateCountOrOverflow) {
    std::uint64_t frame = 999;
    ASSERT_TRUE(harken::ResolveVariableControlCadenceFrame(
        100, 64, 99, &frame));
    EXPECT_EQ(100u, frame);
    ASSERT_TRUE(harken::ResolveVariableControlCadenceFrame(
        100, 64, 100, &frame));
    EXPECT_EQ(100u, frame);
    ASSERT_TRUE(harken::ResolveVariableControlCadenceFrame(
        100, 64, 101, &frame));
    EXPECT_EQ(164u, frame);
    ASSERT_TRUE(harken::ResolveVariableControlCadenceFrame(
        100, 64, 228, &frame));
    EXPECT_EQ(228u, frame);
    ASSERT_TRUE(harken::ResolveVariableControlCadenceFrame(
        100, 64, 229, &frame));
    EXPECT_EQ(292u, frame);

    frame = 991;
    EXPECT_FALSE(harken::ResolveVariableControlCadenceFrame(
        UINT64_MAX - 3u, 8, UINT64_MAX, &frame));
    EXPECT_EQ(991u, frame);
    EXPECT_FALSE(harken::ResolveVariableControlCadenceFrame(
        100, 0, 100, &frame));
    EXPECT_EQ(991u, frame);
}

TEST(VariableControlWeightSegment,
     VoiceRenderAppliesAbsoluteSegmentAndRejectsStaleGeneration) {
    VnConfig config = harkentest::MakeNullConfig(1, 64, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_NE(nullptr, engine);

    std::vector<float> samples(1024, 1.0f);
    const VnPCMDesc pcm = harkentest::MonoDesc(samples, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(7001), &pcm));
    VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(7001), 0.0f);
    VnVoiceHandle publicHandle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &publicHandle));
    harkentest::Capture warm;
    harkentest::PumpAndCapture(engine, 1, warm);
    const VnVoiceHandle render =
        harkentest::RenderHandle(engine, publicHandle);
    ASSERT_NE(0u, render);

    harken::WeightSegmentV1 segment{};
    segment.startFrame = engine->mixer.BlocksRendered() * 64u;
    segment.endFrame = segment.startFrame + 4u;
    segment.startWeight = 0.0f;
    segment.endWeight = 1.0f;
    segment.segmentGeneration = 2;
    segment.decision = {3, 5};
    segment.bank = {7, 11};
    ASSERT_TRUE(engine->voices.ApplyVariableWeightSegment(
        render, segment));

    harken::WeightSegmentV1 stale = segment;
    stale.segmentGeneration = 1;
    stale.startWeight = 1.0f;
    stale.endWeight = 0.0f;
    EXPECT_FALSE(engine->voices.ApplyVariableWeightSegment(
        render, stale));

    harkentest::Capture out;
    harkentest::PumpAndCapture(engine, 1, out);
    ASSERT_EQ(64u, out.left.size());
    EXPECT_FLOAT_EQ(0.0f, out.left[0]);
    EXPECT_NEAR(1.0f / 3.0f, out.left[1], 1e-6f);
    EXPECT_NEAR(2.0f / 3.0f, out.left[2], 1e-6f);
    EXPECT_FLOAT_EQ(1.0f, out.left[3]);
    EXPECT_FLOAT_EQ(1.0f, out.left[63]);
}

TEST(VariableControlWeightSegment,
     NestedOwnersMultiplyRampsAndRemoveOnlyExactController) {
    VnConfig config = harkentest::MakeNullConfig(1, 64, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    std::vector<float> samples(1024, 1.0f);
    const auto pcm = harkentest::MonoDesc(samples, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(7002), &pcm));
    auto play = harkentest::MakePlayDesc(harkentest::ObjectId(7002), 0.0f);
    VnVoiceHandle publicHandle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &publicHandle));
    harkentest::Capture warm;
    harkentest::PumpAndCapture(engine, 1, warm);
    const auto render = harkentest::RenderHandle(engine, publicHandle);
    ASSERT_NE(0u, render);
    const harken::VariableWeightControlIdentity outer{101, 7, 1};
    const harken::VariableWeightControlIdentity inner{102, 9, 1};
    harken::WeightSegmentV1 a{};
    a.startFrame = engine->mixer.BlocksRendered() * 64u;
    a.endFrame = a.startFrame + 4u;
    a.startWeight = 0.0f;
    a.endWeight = 1.0f;
    a.segmentGeneration = 1;
    a.decision = {3, 5};
    a.bank = {7, 11};
    auto b = a;
    b.startWeight = 1.0f;
    b.endWeight = 0.0f;
    b.decision = {4, 6};
    b.bank = {8, 12};
    ASSERT_TRUE(engine->voices.ApplyOwnedVariableWeightSegment(render, outer, a));
    ASSERT_TRUE(engine->voices.ApplyOwnedVariableWeightSegment(render, inner, b));
    EXPECT_FALSE(engine->voices.ApplyOwnedVariableWeightSegment(render, outer, b));
    harkentest::Capture mixed;
    harkentest::PumpAndCapture(engine, 1, mixed);
    ASSERT_EQ(64u, mixed.left.size());
    EXPECT_FLOAT_EQ(0.0f, mixed.left[0]);
    EXPECT_NEAR(2.0f / 9.0f, mixed.left[1], 1e-6f);
    EXPECT_NEAR(2.0f / 9.0f, mixed.left[2], 1e-6f);
    EXPECT_FLOAT_EQ(0.0f, mixed.left[3]);
    EXPECT_FALSE(engine->voices.RemoveOwnedVariableWeightSegment(render, {inner.instance, 10, inner.side}, 1));
    EXPECT_FALSE(engine->voices.RemoveOwnedVariableWeightSegment(render, inner, 2));
    ASSERT_TRUE(engine->voices.RemoveOwnedVariableWeightSegment(render, inner, 1));
    EXPECT_FALSE(engine->voices.ApplyOwnedVariableWeightSegment(render, inner, b));
    a.startFrame += 64;
    a.endFrame += 64;
    a.startWeight = a.endWeight = 0.5f;
    ++a.segmentGeneration;
    ASSERT_TRUE(engine->voices.ApplyOwnedVariableWeightSegment(render, outer, a));
    harkentest::Capture retained;
    harkentest::PumpAndCapture(engine, 1, retained);
    ASSERT_EQ(64u, retained.left.size());
    for (float sample : retained.left) EXPECT_NEAR(0.5f, sample, 1e-6f);
}

TEST(VariableControlWeightSegment,
     VolumeOpcodeCarriesSegmentAcrossTheAudioBoundary) {
    VnConfig config = harkentest::MakeNullConfig(1, 64, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_NE(nullptr, engine);

    std::vector<float> samples(1024, 1.0f);
    const VnPCMDesc pcm = harkentest::MonoDesc(samples, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(7002), &pcm));
    VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(7002), 0.0f);
    VnVoiceHandle publicHandle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &publicHandle));
    harkentest::Capture warm;
    harkentest::PumpAndCapture(engine, 1, warm);
    const VnVoiceHandle render =
        harkentest::RenderHandle(engine, publicHandle);
    ASSERT_NE(0u, render);

    harken::CommandRecord command{};
    command.opcode = harken::kOpcodeSetVoiceVolume;
    command.flags = harken::kCommandFlagVariableWeightSegment;
    command.payload.setVoiceVariableWeight.handle = render;
    command.payload.setVoiceVariableWeight.startFrame =
        engine->mixer.BlocksRendered() * 64u;
    command.payload.setVoiceVariableWeight.endFrame =
        command.payload.setVoiceVariableWeight.startFrame + 4u;
    command.payload.setVoiceVariableWeight.startWeight = 1.0f;
    command.payload.setVoiceVariableWeight.endWeight = 0.0f;
    command.payload.setVoiceVariableWeight.segmentGeneration = 3;
    command.payload.setVoiceVariableWeight.decisionSlot = 5;
    command.payload.setVoiceVariableWeight.decisionGeneration = 7;
    command.payload.setVoiceVariableWeight.bankSlot = 11;
    command.payload.setVoiceVariableWeight.bankGeneration = 13;
    ASSERT_TRUE(engine->commands.TryPush(command));

    harkentest::Capture out;
    harkentest::PumpAndCapture(engine, 1, out);
    ASSERT_EQ(64u, out.left.size());
    EXPECT_FLOAT_EQ(1.0f, out.left[0]);
    EXPECT_NEAR(2.0f / 3.0f, out.left[1], 1e-6f);
    EXPECT_NEAR(1.0f / 3.0f, out.left[2], 1e-6f);
    EXPECT_FLOAT_EQ(0.0f, out.left[3]);
    EXPECT_FLOAT_EQ(0.0f, out.left[63]);
}

TEST(VariableControlWeightSegment,
     TimelineControlArmAcknowledgesWithoutRestartingTheLiveVoice) {
    VnConfig config = harkentest::MakeNullConfig(1, 64, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_NE(nullptr, engine);

    std::vector<float> samples(1024, 1.0f);
    const VnPCMDesc pcm = harkentest::MonoDesc(samples, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(7003), &pcm));
    VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(7003), 0.0f);
    VnVoiceHandle publicHandle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &publicHandle));
    harkentest::Capture warm;
    harkentest::PumpAndCapture(engine, 1, warm);
    const VnVoiceHandle render =
        harkentest::RenderHandle(engine, publicHandle);
    ASSERT_NE(0u, render);
    const harken::VoiceRenderContext* before = engine->voices.Resolve(render);
    ASSERT_NE(nullptr, before);
    const std::uint64_t sourceCursorBefore = before->srcCursor;

    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = 31;
    target.subjectId = harkentest::ObjectId(7003);
    harken::TimelineArmToken arm{};
    const harken::DecisionToken decision{3, 5};
    const std::uint64_t startFrame =
        engine->mixer.BlocksRendered() * 64u + 64u;
    ASSERT_TRUE(engine->timelineArms.ReserveGrouped(
        static_cast<std::uint32_t>(render),
        static_cast<std::uint32_t>(render >> 32), startFrame,
        startFrame, 7, target, harken::ParentAudioGroupRef{decision},
        0, &arm));

    harken::CommandRecord command{};
    command.opcode = harken::kOpcodePlay;
    command.payload.play.batch.batchVersion = harken::kPlayLedBatchVersion;
    command.payload.play.batch.batchKind =
        harken::PlayLedBatchKind::TimelineControlArm;
    command.payload.play.batch.batchCommandCount = 1;
    command.payload.play.batch.armToken =
        (static_cast<std::uint64_t>(arm.generation) << 32) | arm.slot;
    command.payload.play.handle = render;
    command.payload.play.sourceIdentity = render;
    ASSERT_TRUE(engine->commands.TryPush(command));

    harkentest::Capture out;
    harkentest::PumpAndCapture(engine, 1, out);
    harken::TimelineArmObservation observed{};
    ASSERT_TRUE(engine->timelineArms.Observe(arm, &observed));
    EXPECT_EQ(harken::ArmState::Armed, observed.state);
    const harken::VoiceRenderContext* after = engine->voices.Resolve(render);
    ASSERT_NE(nullptr, after);
    EXPECT_GT(after->srcCursor, sourceCursorBefore);
    EXPECT_EQ(1u, engine->voices.ActiveCount());
}

TEST(VariableControlInstance, PreparedAcquireStaysHidden) {
    harken::VariableControlInstanceTable table(1);
    harken::VariableControlInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = 11;
    create.containerStableId = harkentest::ObjectId(13);
    create.programSemanticFingerprint = 17;
    harken::PreparedVariableControlInstance prepared;
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.PrepareAcquire(create, &prepared));
    const harken::VariableControlInstanceToken token = prepared.Token();
    ASSERT_TRUE(token.Valid());
    EXPECT_EQ(0u, table.LiveCount());
    EXPECT_EQ(0u, table.HighWater());
    harken::VariableControlInstanceSnapshot snapshot{};
    EXPECT_FALSE(table.Capture(token, &snapshot));
}

TEST(VariableControlInstance,
     CancelPreparedAcquireAdvancesIdentityAndRejectsEscapedOldToken) {
    harken::VariableControlInstanceTable table(1);
    harken::VariableControlInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = 11;
    create.containerStableId = harkentest::ObjectId(13);
    create.programSemanticFingerprint = 17;
    harken::PreparedVariableControlInstance first;
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.PrepareAcquire(create, &first));
    const harken::VariableControlInstanceToken firstToken = first.Token();
    ASSERT_TRUE(table.CancelPreparedAcquire(&first));
    EXPECT_FALSE(first.Valid());
    EXPECT_EQ(0u, table.LiveCount());
    EXPECT_EQ(0u, table.HighWater());

    harken::PreparedVariableControlInstance second;
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.PrepareAcquire(create, &second));
    EXPECT_EQ(firstToken.slot, second.Token().slot);
    EXPECT_LT(firstToken.generation, second.Token().generation);
    EXPECT_EQ(harken::VariableControlInstanceStatus::Stale,
        table.PrepareSide(firstToken, harken::VariableControlSide::Discrete, {1, 1}, {1, 1}));
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
        table.PrepareSide(second, harken::VariableControlSide::Discrete, {1, 1}, {1, 1}));
    EXPECT_TRUE(table.CancelPreparedAcquire(&second));
}

void PrepareOriginalVcSides(harken::VariableControlInstanceTable& table,
    harken::PreparedVariableControlInstance& original) {
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
        table.PrepareAcquire({7, 11, harkentest::ObjectId(13), 17}, &original));
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
        table.PrepareSide(original, harken::VariableControlSide::Discrete, {2, 3}, {4, 5}));
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
        table.PrepareSide(original, harken::VariableControlSide::Continuous, {6, 7}, {4, 5}));
}

TEST(VariableControlInstance, InitialSideReplacementKeepsBankAndOtherSideAndRejectsOldProof) {
    using Access = harken::detail::VariableControlInstanceTableTestAccess;
    harken::VariableControlInstanceTable table(1);
    harken::PreparedVariableControlInstance prepared;
    ASSERT_NO_FATAL_FAILURE(PrepareOriginalVcSides(table, prepared));
    harken::VariableControlInstanceTable::OriginalInitialSideReceipt original;
    ASSERT_TRUE(Access::IssueInitial(table, prepared, harken::VariableControlSide::Discrete, &original));
    harken::VariableControlInstanceToken token{};
    ASSERT_TRUE(table.CommitPreparedAcquire(&prepared, &token));
    harken::VariableControlActivationProof oldProof{};
    ASSERT_TRUE(table.ProveActivation(token, {2, 3}, &oldProof));
    harken::VariableControlInstanceTable::InitialSideReplacement replacement;
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
        Access::BeginInitial(table, original, {8, 9}, &replacement));
    EXPECT_FALSE(table.Release(token));
    EXPECT_FALSE(table.ActivateProven(oldProof));
    EXPECT_FALSE(table.CancelSide(token, harken::VariableControlSide::Discrete, {2, 3}));
    harken::DecisionToken superseded{};
    EXPECT_EQ(harken::VariableControlInstanceStatus::Stale,
        table.PrepareNewerSide(token, harken::VariableControlSide::Discrete, {10, 11}, {4, 6}, &superseded));
    harken::VariableControlInstanceTable::OriginalInitialSideReceipt denied(std::move(original));
    EXPECT_FALSE(denied.Valid());
    EXPECT_TRUE(original.Valid());
    ASSERT_TRUE(Access::FinishInitial(table, replacement, true));
    EXPECT_FALSE(replacement.Valid());
    EXPECT_FALSE(table.ActivateProven(oldProof));
    harken::VariableControlActivationProof fresh{};
    ASSERT_TRUE(table.ProveActivation(token, {8, 9}, &fresh));
    EXPECT_GT(fresh.sideGeneration, oldProof.sideGeneration);
    EXPECT_EQ(4u, fresh.bank.slot);
    EXPECT_EQ(5u, fresh.bank.generation);
    harken::VariableControlInstanceSnapshot snapshot{};
    ASSERT_TRUE(table.Capture(token, &snapshot));
    EXPECT_EQ(11u, snapshot.rootInstance);
    EXPECT_EQ(6u, snapshot.continuous.decision.slot);
    EXPECT_EQ(7u, snapshot.continuous.decision.generation);
    EXPECT_EQ(5u, snapshot.continuous.newestPending.generation);
    EXPECT_TRUE(table.ActivateProven(fresh));
    EXPECT_TRUE(table.ActivateSide(token, harken::VariableControlSide::Continuous, {6, 7}, {4, 5}));
    EXPECT_TRUE(table.Release(token));
}

TEST(VariableControlInstance, InitialSideReplacementCancelRestoresOnlyOriginalSideWithoutProofRevival) {
    using Access = harken::detail::VariableControlInstanceTableTestAccess;
    harken::VariableControlInstanceTable table(1);
    harken::PreparedVariableControlInstance prepared;
    ASSERT_NO_FATAL_FAILURE(PrepareOriginalVcSides(table, prepared));
    harken::VariableControlInstanceTable::OriginalInitialSideReceipt original;
    ASSERT_TRUE(Access::IssueInitial(table, prepared, harken::VariableControlSide::Discrete, &original));
    harken::VariableControlInstanceToken token{};
    ASSERT_TRUE(table.CommitPreparedAcquire(&prepared, &token));
    harken::VariableControlActivationProof oldProof{};
    ASSERT_TRUE(table.ProveActivation(token, {2, 3}, &oldProof));
    {
        harken::VariableControlInstanceTable::InitialSideReplacement replacement;
        ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
            Access::BeginInitial(table, original, {8, 9}, &replacement));
        ASSERT_TRUE(table.ActivateSide(token, harken::VariableControlSide::Continuous, {6, 7}, {4, 5}));
    }
    EXPECT_FALSE(table.ActivateProven(oldProof));
    harken::VariableControlInstanceSnapshot snapshot{};
    ASSERT_TRUE(table.Capture(token, &snapshot));
    EXPECT_EQ(harken::VariableControlSideState::Pending, snapshot.discrete.state);
    EXPECT_EQ(2u, snapshot.discrete.decision.slot);
    EXPECT_EQ(3u, snapshot.discrete.decision.generation);
    EXPECT_FALSE(snapshot.discrete.lastApplied.Valid());
    EXPECT_EQ(harken::VariableControlSideState::Committed, snapshot.continuous.state);
    EXPECT_EQ(5u, snapshot.continuous.lastApplied.generation);
    harken::VariableControlInstanceTable::InitialSideReplacement retry;
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok, Access::BeginInitial(table, original, {}, &retry));
    ASSERT_TRUE(Access::FinishInitial(table, retry, true));
    ASSERT_TRUE(table.Capture(token, &snapshot));
    EXPECT_EQ(harken::VariableControlSideState::Committed, snapshot.discrete.state);
    EXPECT_FALSE(snapshot.discrete.decision.Valid());
    EXPECT_FALSE(snapshot.discrete.newestPending.Valid());
    EXPECT_EQ(5u, snapshot.discrete.lastApplied.generation);
    EXPECT_TRUE(table.Release(token));
}

TEST(VariableControlInstance, InitialSideReplacementChangesSilentToVoicedAndRestoresSilentOnCancel) {
    using Access = harken::detail::VariableControlInstanceTableTestAccess;
    harken::VariableControlInstanceTable table(1);
    harken::PreparedVariableControlInstance prepared;
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
        table.PrepareAcquire({7, 11, harkentest::ObjectId(13), 17}, &prepared));
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
        table.CommitSilentSide(prepared, harken::VariableControlSide::Discrete, {4, 5}));
    harken::VariableControlInstanceTable::OriginalInitialSideReceipt original;
    ASSERT_TRUE(Access::IssueInitial(table, prepared, harken::VariableControlSide::Discrete, &original));
    EXPECT_TRUE(Access::MatchesInitial(table, original, prepared.Token(),
        harken::VariableControlSide::Discrete, {}, {4, 5}));
    for (const auto invalid : {harken::DecisionToken{0, 0}, {8, 0}, {UINT32_MAX, 1}}) {
        EXPECT_FALSE(Access::MatchesInitial(table, original, prepared.Token(),
            harken::VariableControlSide::Discrete, invalid, {4, 5}));
    }
    EXPECT_FALSE(Access::MatchesInitial(table, original, prepared.Token(),
        harken::VariableControlSide::Continuous, {}, {4, 5}));
    EXPECT_FALSE(Access::MatchesInitial(table, original, prepared.Token(),
        harken::VariableControlSide::Discrete, {}, {4, 6}));
    harken::VariableControlInstanceToken token{};
    ASSERT_TRUE(table.CommitPreparedAcquire(&prepared, &token));
    {
        harken::VariableControlInstanceTable::InitialSideReplacement replacement;
        ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
            Access::BeginInitial(table, original, {8, 9}, &replacement));
    }
    harken::VariableControlInstanceSnapshot snapshot{};
    ASSERT_TRUE(table.Capture(token, &snapshot));
    EXPECT_EQ(harken::VariableControlSideState::Committed, snapshot.discrete.state);
    EXPECT_FALSE(snapshot.discrete.decision.Valid());
    EXPECT_EQ(UINT32_MAX, snapshot.discrete.decision.slot);
    EXPECT_EQ(0u, snapshot.discrete.decision.generation);
    EXPECT_EQ(UINT32_MAX, snapshot.discrete.newestPending.slot);
    EXPECT_EQ(0u, snapshot.discrete.newestPending.generation);
    EXPECT_EQ(5u, snapshot.discrete.lastApplied.generation);
    harken::VariableControlInstanceTable::InitialSideReplacement replacement;
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
        Access::BeginInitial(table, original, {8, 9}, &replacement));
    ASSERT_TRUE(Access::FinishInitial(table, replacement, true));
    ASSERT_TRUE(table.Capture(token, &snapshot));
    EXPECT_EQ(harken::VariableControlSideState::Pending, snapshot.discrete.state);
    EXPECT_FALSE(snapshot.discrete.lastApplied.Valid());
    EXPECT_EQ(UINT32_MAX, snapshot.discrete.lastApplied.slot);
    EXPECT_EQ(0u, snapshot.discrete.lastApplied.generation);
    EXPECT_TRUE(table.ActivateSide(token, harken::VariableControlSide::Discrete, {8, 9}, {4, 5}));
    EXPECT_TRUE(table.Release(token));
}

TEST(VariableControlInstance, InitialSideReplacementRefusesForeignStaleAndAlreadyActivatedReceipts) {
    using Access = harken::detail::VariableControlInstanceTableTestAccess;
    harken::VariableControlInstanceTable table(1), foreign(1);
    harken::PreparedVariableControlInstance prepared;
    ASSERT_NO_FATAL_FAILURE(PrepareOriginalVcSides(table, prepared));
    harken::VariableControlInstanceTable::OriginalInitialSideReceipt original;
    ASSERT_TRUE(Access::IssueInitial(table, prepared, harken::VariableControlSide::Discrete, &original));
    harken::VariableControlInstanceToken token{};
    ASSERT_TRUE(table.CommitPreparedAcquire(&prepared, &token));
    harken::VariableControlInstanceTable::InitialSideReplacement replacement;
    EXPECT_EQ(harken::VariableControlInstanceStatus::Stale,
        Access::BeginInitial(foreign, original, {8, 9}, &replacement));
    EXPECT_EQ(harken::VariableControlInstanceStatus::Invalid,
        Access::BeginInitial(table, original, {8, 0}, &replacement));
    EXPECT_EQ(harken::VariableControlInstanceStatus::Invalid,
        Access::BeginInitial(table, original, {6, 7}, &replacement));
    ASSERT_TRUE(table.ActivateSide(token, harken::VariableControlSide::Discrete, {2, 3}, {4, 5}));
    EXPECT_EQ(harken::VariableControlInstanceStatus::Stale,
        Access::BeginInitial(table, original, {8, 9}, &replacement));
    EXPECT_FALSE(replacement.Valid());
    ASSERT_TRUE(table.ActivateSide(token, harken::VariableControlSide::Continuous, {6, 7}, {4, 5}));
    ASSERT_TRUE(table.Release(token));
    ASSERT_NO_FATAL_FAILURE(PrepareOriginalVcSides(table, prepared));
    ASSERT_TRUE(table.CommitPreparedAcquire(&prepared, &token));
    EXPECT_EQ(harken::VariableControlInstanceStatus::Stale,
        Access::BeginInitial(table, original, {8, 9}, &replacement));
    EXPECT_TRUE(table.CancelSide(token, harken::VariableControlSide::Discrete, {2, 3}));
    EXPECT_TRUE(table.CancelSide(token, harken::VariableControlSide::Continuous, {6, 7}));
    EXPECT_TRUE(table.Release(token));
}

TEST(VariableControlInstance, InitialSideReceiptRefusesTokenOnlyPreparationAndChangedFactoryMetadata) {
    using Access = harken::detail::VariableControlInstanceTableTestAccess;
    harken::VariableControlInstanceTable table(1);
    harken::PreparedVariableControlInstance prepared;
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
        table.PrepareAcquire({7, 11, harkentest::ObjectId(13), 17}, &prepared));
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
        table.PrepareSide(prepared.Token(), harken::VariableControlSide::Discrete, {2, 3}, {4, 5}));
    harken::VariableControlInstanceTable::OriginalInitialSideReceipt original;
    EXPECT_FALSE(Access::IssueInitial(table, prepared, harken::VariableControlSide::Discrete, &original));
    ASSERT_TRUE(table.CancelPreparedAcquire(&prepared));
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
        table.PrepareAcquire({7, 11, harkentest::ObjectId(13), 17}, &prepared));
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
        table.CommitSilentSide(prepared.Token(), harken::VariableControlSide::Discrete, {4, 5}));
    EXPECT_FALSE(Access::IssueInitial(table, prepared, harken::VariableControlSide::Discrete, &original));
    ASSERT_TRUE(table.CancelPreparedAcquire(&prepared));
    ASSERT_NO_FATAL_FAILURE(PrepareOriginalVcSides(table, prepared));
    ASSERT_TRUE(Access::ChangePreparingRoot(table, prepared.Token()));
    EXPECT_FALSE(Access::IssueInitial(table, prepared, harken::VariableControlSide::Discrete, &original));
    EXPECT_FALSE(original.Valid());
    EXPECT_TRUE(table.CancelPreparedAcquire(&prepared));
}

TEST(VariableControlInstance, InitialSideReplacementRefusesBoundFrontierAndRealGroupReader) {
    using Access = harken::detail::VariableControlInstanceTableTestAccess;
    FrontierSideHarness harness;
    harken::VariableControlInstanceTable::OriginalInitialSideReceipt original;
    ASSERT_NO_FATAL_FAILURE(harness.Prepare(harken::VariableControlSide::Discrete, &original));
    harken::VariableControlInstanceTable::InitialSideReplacement replacement;
    EXPECT_EQ(harken::VariableControlInstanceStatus::Stale,
        Access::BeginInitial(harness.instances, original, {8, 9}, &replacement));
    EXPECT_FALSE(replacement.Valid());
    harken::VariableControlInstanceSnapshot snapshot{};
    ASSERT_TRUE(harness.instances.Capture(harness.instance, &snapshot));
    EXPECT_EQ(harken::VariableControlSideState::Pending, snapshot.discrete.state);
    EXPECT_EQ(harness.decision.slot, snapshot.discrete.decision.slot);
}

TEST(VariableControlInstance, InitialSideReplacementCancelRejectsForeignSemanticChange) {
    using Access = harken::detail::VariableControlInstanceTableTestAccess;
    harken::VariableControlInstanceTable table(1);
    harken::PreparedVariableControlInstance prepared;
    ASSERT_NO_FATAL_FAILURE(PrepareOriginalVcSides(table, prepared));
    harken::VariableControlInstanceTable::OriginalInitialSideReceipt original;
    ASSERT_TRUE(Access::IssueInitial(table, prepared, harken::VariableControlSide::Discrete, &original));
    harken::VariableControlInstanceToken token{};
    ASSERT_TRUE(table.CommitPreparedAcquire(&prepared, &token));
    harken::VariableControlInstanceTable::InitialSideReplacement replacement;
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
        Access::BeginInitial(table, original, {8, 9}, &replacement));
    ASSERT_TRUE(Access::ChangeHeldBank(table, token, false));
    EXPECT_FALSE(Access::FinishInitial(table, replacement, false));
    EXPECT_TRUE(replacement.Valid());
    harken::VariableControlInstanceSnapshot snapshot{};
    ASSERT_TRUE(table.Capture(token, &snapshot));
    EXPECT_EQ(5u, snapshot.discrete.newestObserved.slot);
    ASSERT_TRUE(Access::ChangeHeldBank(table, token, true));
    ASSERT_TRUE(Access::FinishInitial(table, replacement, false));
    EXPECT_TRUE(table.CancelSide(token, harken::VariableControlSide::Discrete, {2, 3}));
    EXPECT_TRUE(table.CancelSide(token, harken::VariableControlSide::Continuous, {6, 7}));
    EXPECT_TRUE(table.Release(token));
}

TEST(VariableControlInstance, InitialSideReplacementExhaustionPreservesAcceptedSilentSide) {
    using Access = harken::detail::VariableControlInstanceTableTestAccess;
    harken::VariableControlInstanceTable table(1);
    harken::PreparedVariableControlInstance prepared;
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
        table.PrepareAcquire({7, 11, harkentest::ObjectId(13), 17}, &prepared));
    ASSERT_TRUE(Access::SeedPreparingSideGeneration(table, prepared.Token(), UINT64_MAX));
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
        table.CommitSilentSide(prepared, harken::VariableControlSide::Discrete, {4, 5}));
    harken::VariableControlInstanceTable::OriginalInitialSideReceipt original;
    ASSERT_TRUE(Access::IssueInitial(table, prepared, harken::VariableControlSide::Discrete, &original));
    harken::VariableControlInstanceToken token{};
    ASSERT_TRUE(table.CommitPreparedAcquire(&prepared, &token));
    harken::VariableControlInstanceTable::InitialSideReplacement replacement;
    EXPECT_EQ(harken::VariableControlInstanceStatus::Capacity,
        Access::BeginInitial(table, original, {8, 9}, &replacement));
    EXPECT_FALSE(replacement.Valid());
    harken::VariableControlInstanceSnapshot snapshot{};
    ASSERT_TRUE(table.Capture(token, &snapshot));
    EXPECT_EQ(11u, snapshot.rootInstance);
    EXPECT_EQ(harken::VariableControlSideState::Committed, snapshot.discrete.state);
    EXPECT_EQ(5u, snapshot.discrete.lastApplied.generation);
    EXPECT_TRUE(table.Release(token));
}

TEST(VariableControlInstance, PreparedCommitHoldBlocksMutatorsAndPreservesStableOriginalCap) {
    harken::VariableControlInstanceTable table(2);
    harken::PreparedVariableControlInstance original, other;
    ASSERT_NO_FATAL_FAILURE(PrepareOriginalVcSides(table, original));
    ASSERT_NO_FATAL_FAILURE(PrepareOriginalVcSides(table, other));
    const auto token = original.Token();
    const auto otherToken = other.Token();
    harken::VariableControlInstanceTable::PreparedCommitProof proof;
    ASSERT_TRUE(table.ProvePreparedCommit(original, &proof));
    ASSERT_TRUE(table.PreparedCommitHeld(token));
    harken::VariableControlInstanceToken untouched{19, 23};
    EXPECT_FALSE(table.CommitPreparedAcquire(&original, &untouched));
    EXPECT_EQ(19u, untouched.slot);
    EXPECT_EQ(23u, untouched.generation);
    EXPECT_FALSE(table.CancelPreparedAcquire(&original));
    EXPECT_EQ(harken::VariableControlInstanceStatus::Stale,
        table.PrepareSide(token, harken::VariableControlSide::Discrete, {8, 9}, {4, 6}));
    EXPECT_EQ(harken::VariableControlInstanceStatus::Stale,
        table.CommitSilentSide(token, harken::VariableControlSide::Continuous, {4, 6}));
    EXPECT_FALSE(table.ActivateSide(token, harken::VariableControlSide::Discrete, {2, 3}, {4, 5}));
    EXPECT_FALSE(table.CancelSide(token, harken::VariableControlSide::Discrete, {2, 3}));
    EXPECT_FALSE(table.Release(token));
    harken::PreparedVariableControlInstance denied(std::move(original));
    EXPECT_FALSE(denied.Valid());
    other = std::move(original);
    original = std::move(other);
    EXPECT_EQ(token.slot, original.Token().slot);
    EXPECT_EQ(otherToken.slot, other.Token().slot);
    EXPECT_TRUE(proof.Associated(&table, &original));
    EXPECT_EQ(0u, table.LiveCount());
    EXPECT_EQ(0u, table.HighWater());
    proof.Release();
    ASSERT_TRUE(table.ProvePreparedCommit(original, &proof));
    proof.Release();
    ASSERT_TRUE(table.CancelPreparedAcquire(&original));
    ASSERT_TRUE(table.CancelPreparedAcquire(&other));
}

TEST(VariableControlInstance, PreparedCommitUniqueConsumeRetainsRealPendingActivationAndRelease) {
    harken::VariableControlInstanceTable table(2);
    harken::PreparedVariableControlInstance first, second;
    ASSERT_NO_FATAL_FAILURE(PrepareOriginalVcSides(table, first));
    ASSERT_NO_FATAL_FAILURE(PrepareOriginalVcSides(table, second));
    const auto secondToken = second.Token();
    harken::VariableControlInstanceTable::PreparedCommitProof firstProof, secondProof;
    ASSERT_TRUE(table.ProvePreparedCommit(first, &firstProof));
    ASSERT_TRUE(table.ProvePreparedCommit(second, &secondProof));
    firstProof = std::move(secondProof);
    EXPECT_FALSE(secondProof.Valid());
    ASSERT_TRUE(table.CancelPreparedAcquire(&first));
    EXPECT_FALSE(table.CancelPreparedAcquire(&second));
    harken::VariableControlInstanceTable::ConsumePreparedAcquire(std::move(firstProof));
    EXPECT_FALSE(firstProof.Valid());
    EXPECT_FALSE(second.Valid());
    EXPECT_EQ(1u, table.LiveCount());
    EXPECT_EQ(1u, table.HighWater());
    harken::VariableControlInstanceSnapshot snapshot{};
    ASSERT_TRUE(table.Capture(secondToken, &snapshot));
    EXPECT_EQ(11u, snapshot.rootInstance);
    EXPECT_EQ(harken::VariableControlSideState::Pending, snapshot.discrete.state);
    EXPECT_EQ(harken::VariableControlSideState::Pending, snapshot.continuous.state);
    EXPECT_EQ(5u, snapshot.discrete.newestPending.generation);
    EXPECT_FALSE(snapshot.discrete.lastApplied.Valid());
    EXPECT_FALSE(table.Release(secondToken));
    EXPECT_FALSE(table.ActivateSide(secondToken, harken::VariableControlSide::Discrete, {2, 4}, {4, 5}));
    ASSERT_TRUE(table.ActivateSide(secondToken, harken::VariableControlSide::Discrete, {2, 3}, {4, 5}));
    ASSERT_TRUE(table.ActivateSide(secondToken, harken::VariableControlSide::Continuous, {6, 7}, {4, 5}));
    ASSERT_TRUE(table.Release(secondToken));
    EXPECT_EQ(0u, table.LiveCount());
}

TEST(VariableControlInstance, PreparedCommitRejectsUnissuedSideReceiptsAndChangedOriginalMetadata) {
    harken::VariableControlInstanceTable table(1);
    harken::PreparedVariableControlInstance original;
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
        table.PrepareAcquire({7, 11, harkentest::ObjectId(13), 17}, &original));
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
        table.PrepareSide(original.Token(), harken::VariableControlSide::Discrete, {2, 3}, {4, 5}));
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
        table.PrepareSide(original.Token(), harken::VariableControlSide::Continuous, {6, 7}, {4, 5}));
    harken::VariableControlInstanceTable::PreparedCommitProof proof;
    EXPECT_FALSE(table.ProvePreparedCommit(original, &proof));
    EXPECT_FALSE(proof.Valid());
    ASSERT_TRUE(table.CancelPreparedAcquire(&original));
    ASSERT_NO_FATAL_FAILURE(PrepareOriginalVcSides(table, original));
    ASSERT_TRUE(harken::detail::VariableControlInstanceTableTestAccess::ChangePreparingRoot(table, original.Token()));
    EXPECT_FALSE(table.ProvePreparedCommit(original, &proof));
    EXPECT_FALSE(proof.Valid());
    ASSERT_TRUE(table.CancelPreparedAcquire(&original));
    EXPECT_EQ(0u, table.LiveCount());
}

TEST(VariableControlInstance, PreparedCommitMaximumAbortAndConsumedDisposalNeverWrap) {
    for (const bool consume : {false, true}) {
        harken::VariableControlInstanceTable table(1);
        ASSERT_TRUE(harken::detail::VariableControlInstanceTableTestAccess::SeedFreeGeneration(
            table, 0, std::numeric_limits<std::uint32_t>::max()));
        harken::PreparedVariableControlInstance original;
        ASSERT_NO_FATAL_FAILURE(PrepareOriginalVcSides(table, original));
        const auto token = original.Token();
        ASSERT_EQ(std::numeric_limits<std::uint32_t>::max(), token.generation);
        harken::VariableControlInstanceTable::PreparedCommitProof proof;
        ASSERT_TRUE(table.ProvePreparedCommit(original, &proof));
        if (consume) {
            harken::VariableControlInstanceTable::ConsumePreparedAcquire(std::move(proof));
            ASSERT_TRUE(table.CancelSide(token, harken::VariableControlSide::Discrete, {2, 3}));
            ASSERT_TRUE(table.CancelSide(token, harken::VariableControlSide::Continuous, {6, 7}));
            ASSERT_TRUE(table.Release(token));
        } else {
            proof.Release();
            ASSERT_TRUE(table.CancelPreparedAcquire(&original));
        }
        EXPECT_FALSE(original.Valid());
        EXPECT_EQ(0u, table.LiveCount());
        EXPECT_EQ(harken::VariableControlInstanceStatus::Capacity,
            table.PrepareAcquire({7, 11, harkentest::ObjectId(13), 17}, &original));
        EXPECT_FALSE(table.PreparedCommitHeld(token));
    }
}

TEST(VariableControlInstance, CommitPreparedAcquirePublishesLiveInstance) {
    harken::VariableControlInstanceTable table(1);
    harken::VariableControlInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = 11;
    create.containerStableId = harkentest::ObjectId(13);
    create.programSemanticFingerprint = 17;
    harken::PreparedVariableControlInstance prepared;
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.PrepareAcquire(create, &prepared));
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.PrepareSide(
                  prepared.Token(), harken::VariableControlSide::Discrete,
                  {1, 1}, {1, 1}));
    harken::VariableControlInstanceToken committed{};
    ASSERT_TRUE(table.CommitPreparedAcquire(&prepared, &committed));
    EXPECT_FALSE(prepared.Valid());
    EXPECT_EQ(1u, table.LiveCount());
    EXPECT_EQ(1u, table.HighWater());
    harken::VariableControlInstanceSnapshot snapshot{};
    ASSERT_TRUE(table.Capture(committed, &snapshot));
    EXPECT_EQ(create.world, snapshot.world);
    EXPECT_EQ(create.rootInstance, snapshot.rootInstance);
    EXPECT_EQ(create.containerStableId, snapshot.containerStableId);
    EXPECT_EQ(create.programSemanticFingerprint,
              snapshot.programSemanticFingerprint);
    EXPECT_EQ(harken::VariableControlSideState::Pending,
              snapshot.discrete.state);
    EXPECT_TRUE(table.CancelSide(
        committed, harken::VariableControlSide::Discrete, {1, 1}));
    EXPECT_TRUE(table.Release(committed));
    EXPECT_EQ(0u, table.LiveCount());
}

TEST(VariableControlInstance, DualSidesOwnIndependentPendingCuts) {
    harken::VariableControlInstanceTable table(1);
    harken::VariableControlInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = UINT64_C(1) << 32;
    create.containerStableId = harkentest::ObjectId(2001);
    create.programSemanticFingerprint = 3001;

    harken::VariableControlInstanceToken instance{};
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.Acquire(create, &instance));
    ASSERT_TRUE(instance.Valid());

    const harken::DecisionToken discreteDecision{3, 9};
    const harken::BankVersionToken discreteBank{5, 11};
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.PrepareSide(instance,
                                harken::VariableControlSide::Discrete,
                                discreteDecision, discreteBank));

    harken::VariableControlInstanceSnapshot snapshot{};
    ASSERT_TRUE(table.Capture(instance, &snapshot));
    EXPECT_EQ(harken::VariableControlSideState::Pending,
              snapshot.discrete.state);
    EXPECT_EQ(discreteDecision.slot, snapshot.discrete.decision.slot);
    EXPECT_EQ(discreteDecision.generation,
              snapshot.discrete.decision.generation);
    EXPECT_EQ(discreteBank.slot, snapshot.discrete.newestPending.slot);
    EXPECT_EQ(discreteBank.generation,
              snapshot.discrete.newestPending.generation);
    EXPECT_EQ(harken::VariableControlSideState::Empty,
              snapshot.continuous.state);

    const harken::VariableControlInstanceSnapshot before = snapshot;
    EXPECT_EQ(harken::VariableControlInstanceStatus::Invalid,
              table.PrepareSide(instance,
                                harken::VariableControlSide::Continuous,
                                {}, {8, 13}));
    ASSERT_TRUE(table.Capture(instance, &snapshot));
    EXPECT_EQ(before.discrete.decision.slot,
              snapshot.discrete.decision.slot);
    EXPECT_EQ(before.discrete.newestPending.generation,
              snapshot.discrete.newestPending.generation);
    EXPECT_EQ(harken::VariableControlSideState::Empty,
              snapshot.continuous.state);

    const harken::DecisionToken continuousDecision{4, 10};
    const harken::BankVersionToken continuousBank{8, 13};
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.PrepareSide(instance,
                                harken::VariableControlSide::Continuous,
                                continuousDecision, continuousBank));
    ASSERT_TRUE(table.Capture(instance, &snapshot));
    EXPECT_EQ(create.world, snapshot.world);
    EXPECT_EQ(create.rootInstance, snapshot.rootInstance);
    EXPECT_EQ(harken::VariableControlSideState::Pending,
              snapshot.discrete.state);
    EXPECT_EQ(harken::VariableControlSideState::Pending,
              snapshot.continuous.state);
    EXPECT_EQ(continuousDecision.generation,
              snapshot.continuous.decision.generation);
    EXPECT_EQ(continuousBank.generation,
              snapshot.continuous.newestPending.generation);

    harken::VariableControlInstanceToken untouched{77, 88};
    EXPECT_EQ(harken::VariableControlInstanceStatus::Capacity,
              table.Acquire(create, &untouched));
    EXPECT_EQ(77u, untouched.slot);
    EXPECT_EQ(88u, untouched.generation);
}

TEST(VariableControlInstance, ReleaseInvalidatesOldIdentityBeforeSlotReuse) {
    harken::VariableControlInstanceTable table(1);
    harken::VariableControlInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = UINT64_C(1) << 32;
    create.containerStableId = harkentest::ObjectId(2001);
    create.programSemanticFingerprint = 3001;

    harken::VariableControlInstanceToken first{};
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.Acquire(create, &first));
    ASSERT_TRUE(table.Release(first));

    harken::VariableControlInstanceSnapshot untouched{};
    untouched.world = 99;
    EXPECT_FALSE(table.Capture(first, &untouched));
    EXPECT_EQ(99u, untouched.world);
    EXPECT_EQ(harken::VariableControlInstanceStatus::Stale,
              table.PrepareSide(first,
                                harken::VariableControlSide::Discrete,
                                {1, 1}, {2, 1}));

    harken::VariableControlInstanceToken second{};
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.Acquire(create, &second));
    EXPECT_EQ(first.slot, second.slot);
    EXPECT_NE(first.generation, second.generation);
    EXPECT_FALSE(table.Release(first));
    EXPECT_TRUE(table.Release(second));
}

TEST(VariableControlInstance,
     AudioActivationRequiresExactSideDecisionAndBank) {
    harken::VariableControlInstanceTable table(1);
    harken::VariableControlInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = UINT64_C(1) << 32;
    create.containerStableId = harkentest::ObjectId(2001);
    create.programSemanticFingerprint = 3001;

    harken::VariableControlInstanceToken instance{};
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.Acquire(create, &instance));
    const harken::DecisionToken discreteDecision{3, 9};
    const harken::DecisionToken continuousDecision{4, 10};
    const harken::BankVersionToken discreteBank{5, 11};
    const harken::BankVersionToken continuousBank{5, 12};
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.PrepareSide(instance,
                                harken::VariableControlSide::Discrete,
                                discreteDecision, discreteBank));
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.PrepareSide(instance,
                                harken::VariableControlSide::Continuous,
                                continuousDecision, continuousBank));

    EXPECT_FALSE(table.ActivateSide(
        instance, harken::VariableControlSide::Discrete,
        {discreteDecision.slot, discreteDecision.generation + 1u},
        discreteBank));
    EXPECT_FALSE(table.ActivateSide(
        instance, harken::VariableControlSide::Discrete,
        discreteDecision,
        {discreteBank.slot, discreteBank.generation + 1u}));

    harken::VariableControlInstanceSnapshot snapshot{};
    ASSERT_TRUE(table.Capture(instance, &snapshot));
    EXPECT_EQ(harken::VariableControlSideState::Pending,
              snapshot.discrete.state);
    EXPECT_EQ(harken::VariableControlSideState::Pending,
              snapshot.continuous.state);

    ASSERT_TRUE(table.ActivateSide(
        instance, harken::VariableControlSide::Discrete,
        discreteDecision, discreteBank));
    ASSERT_TRUE(table.Capture(instance, &snapshot));
    EXPECT_EQ(harken::VariableControlSideState::Committed,
              snapshot.discrete.state);
    EXPECT_EQ(discreteBank.slot, snapshot.discrete.lastApplied.slot);
    EXPECT_EQ(discreteBank.generation,
              snapshot.discrete.lastApplied.generation);
    EXPECT_EQ(harken::VariableControlSideState::Pending,
              snapshot.continuous.state);
    EXPECT_FALSE(table.ActivateSide(
        instance, harken::VariableControlSide::Discrete,
        discreteDecision, discreteBank));
}

TEST(VariableControlInstance,
     FailedActivationCannotPublishAppliedBank) {
    harken::VariableControlInstanceTable table(1);
    harken::VariableControlInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = UINT64_C(1) << 32;
    create.containerStableId = harkentest::ObjectId(2001);
    create.programSemanticFingerprint = 3001;

    harken::VariableControlInstanceToken instance{};
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.Acquire(create, &instance));
    const harken::DecisionToken decision{3, 9};
    const harken::BankVersionToken bank{5, 11};
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.PrepareSide(instance,
                                harken::VariableControlSide::Discrete,
                                decision, bank));
    ASSERT_TRUE(table.CancelSide(
        instance, harken::VariableControlSide::Discrete, decision));

    EXPECT_FALSE(table.ActivateSide(
        instance, harken::VariableControlSide::Discrete, decision, bank));

    harken::VariableControlInstanceSnapshot snapshot{};
    ASSERT_TRUE(table.Capture(instance, &snapshot));
    EXPECT_EQ(harken::VariableControlSideState::Terminal,
              snapshot.discrete.state);
    EXPECT_FALSE(snapshot.discrete.lastApplied.Valid());
}

TEST(VariableControlInstance,
     InitialSidePublicationGenerationExhaustionQuarantinesWithoutPayload) {
    harken::VariableControlInstanceTable table(1);
    harken::VariableControlInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = UINT64_C(1) << 32;
    create.containerStableId = harkentest::ObjectId(2001);
    create.programSemanticFingerprint = 3001;
    harken::VariableControlInstanceToken instance{};
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.Acquire(create, &instance));
    ASSERT_TRUE(
        harken::detail::VariableControlInstanceTableTestAccess::
            SeedSideGeneration(table, instance,
                               harken::VariableControlSide::Discrete,
                               UINT64_MAX));

    EXPECT_EQ(harken::VariableControlInstanceStatus::Capacity,
              table.PrepareSide(instance,
                                harken::VariableControlSide::Discrete,
                                {3, 9}, {5, 11}));
    harken::VariableControlInstanceSnapshot exhausted{};
    ASSERT_TRUE(table.Capture(instance, &exhausted));
    EXPECT_EQ(harken::VariableControlSideState::Quarantined,
              exhausted.discrete.state);
    EXPECT_FALSE(exhausted.discrete.decision.Valid());
    EXPECT_FALSE(exhausted.discrete.newestObserved.Valid());
    EXPECT_FALSE(exhausted.discrete.newestPending.Valid());
    EXPECT_FALSE(exhausted.discrete.lastApplied.Valid());
    EXPECT_EQ(harken::VariableControlInstanceStatus::Capacity,
              table.PrepareSide(instance,
                                harken::VariableControlSide::Discrete,
                                {3, 10}, {5, 12}));
}

TEST(VariableControlInstance,
     NewerSidePublicationGenerationExhaustionInvalidatesOldProofForever) {
    harken::VariableControlInstanceTable table(1);
    harken::VariableControlInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = UINT64_C(1) << 32;
    create.containerStableId = harkentest::ObjectId(2001);
    create.programSemanticFingerprint = 3001;
    harken::VariableControlInstanceToken instance{};
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.Acquire(create, &instance));
    const harken::DecisionToken oldDecision{3, 9};
    const harken::BankVersionToken oldBank{5, 11};
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.PrepareSide(instance,
                                harken::VariableControlSide::Continuous,
                                oldDecision, oldBank));
    harken::VariableControlActivationProof oldProof{};
    ASSERT_TRUE(table.ProveActivation(instance, oldDecision, &oldProof));
    harken::VariableControlInstanceSnapshot before{};
    ASSERT_TRUE(table.Capture(instance, &before));
    ASSERT_TRUE(
        harken::detail::VariableControlInstanceTableTestAccess::
            SeedSideGeneration(table, instance,
                               harken::VariableControlSide::Continuous,
                               UINT64_MAX));

    harken::DecisionToken superseded{71, 73};
    EXPECT_EQ(harken::VariableControlInstanceStatus::Capacity,
              table.PrepareNewerSide(
                  instance, harken::VariableControlSide::Continuous,
                  {7, 15}, {5, 12}, &superseded));
    EXPECT_EQ(71u, superseded.slot);
    EXPECT_EQ(73u, superseded.generation);
    harken::VariableControlInstanceSnapshot exhausted{};
    ASSERT_TRUE(table.Capture(instance, &exhausted));
    EXPECT_EQ(harken::VariableControlSideState::Quarantined,
              exhausted.continuous.state);
    EXPECT_EQ(before.continuous.decision.slot,
              exhausted.continuous.decision.slot);
    EXPECT_EQ(before.continuous.decision.generation,
              exhausted.continuous.decision.generation);
    EXPECT_EQ(before.continuous.newestObserved.slot,
              exhausted.continuous.newestObserved.slot);
    EXPECT_EQ(before.continuous.newestObserved.generation,
              exhausted.continuous.newestObserved.generation);
    EXPECT_EQ(before.continuous.newestPending.slot,
              exhausted.continuous.newestPending.slot);
    EXPECT_EQ(before.continuous.newestPending.generation,
              exhausted.continuous.newestPending.generation);
    EXPECT_EQ(before.continuous.lastApplied.slot,
              exhausted.continuous.lastApplied.slot);
    EXPECT_EQ(before.continuous.lastApplied.generation,
              exhausted.continuous.lastApplied.generation);
    EXPECT_FALSE(table.ActivateProven(oldProof));
    EXPECT_EQ(harken::VariableControlInstanceStatus::Capacity,
              table.PrepareNewerSide(
                  instance, harken::VariableControlSide::Continuous,
                  {7, 16}, {5, 13}, &superseded));
    EXPECT_FALSE(table.ActivateProven(oldProof));
}

TEST(VariableControlInstance,
     NewerBankSupersedesOnlyItsSideAndOldActivationBecomesNoOp) {
    harken::VariableControlInstanceTable table(1);
    harken::VariableControlInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = UINT64_C(1) << 32;
    create.containerStableId = harkentest::ObjectId(2001);
    create.programSemanticFingerprint = 3001;
    harken::VariableControlInstanceToken instance{};
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.Acquire(create, &instance));

    const harken::DecisionToken oldDecision{3, 9};
    const harken::DecisionToken siblingDecision{4, 10};
    const harken::BankVersionToken oldBank{5, 11};
    const harken::BankVersionToken siblingBank{6, 13};
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.PrepareSide(instance,
                                harken::VariableControlSide::Continuous,
                                oldDecision, oldBank));
    ASSERT_TRUE(table.ActivateSide(
        instance, harken::VariableControlSide::Continuous,
        oldDecision, oldBank));
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.PrepareSide(instance,
                                harken::VariableControlSide::Discrete,
                                siblingDecision, siblingBank));

    harken::DecisionToken superseded{71, 73};
    const harken::DecisionToken newDecision{7, 15};
    const harken::BankVersionToken newBank{5, 12};
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.PrepareNewerSide(
                  instance, harken::VariableControlSide::Continuous,
                  newDecision, newBank, &superseded));
    EXPECT_EQ(oldDecision.slot, superseded.slot);
    EXPECT_EQ(oldDecision.generation, superseded.generation);
    EXPECT_FALSE(table.ActivateSide(
        instance, harken::VariableControlSide::Continuous,
        oldDecision, oldBank));

    harken::VariableControlInstanceSnapshot snapshot{};
    ASSERT_TRUE(table.Capture(instance, &snapshot));
    EXPECT_EQ(harken::VariableControlSideState::Pending,
              snapshot.continuous.state);
    EXPECT_EQ(oldBank.generation,
              snapshot.continuous.lastApplied.generation);
    EXPECT_EQ(newBank.generation,
              snapshot.continuous.newestObserved.generation);
    EXPECT_EQ(newBank.generation,
              snapshot.continuous.newestPending.generation);
    EXPECT_EQ(siblingDecision.generation,
              snapshot.discrete.decision.generation);

    superseded = {71, 73};
    EXPECT_EQ(harken::VariableControlInstanceStatus::Stale,
              table.PrepareNewerSide(
                  instance, harken::VariableControlSide::Continuous,
                  {8, 16}, oldBank, &superseded));
    EXPECT_EQ(71u, superseded.slot);
    EXPECT_EQ(73u, superseded.generation);
    ASSERT_TRUE(table.ActivateSide(
        instance, harken::VariableControlSide::Continuous,
        newDecision, newBank));
    ASSERT_TRUE(table.Capture(instance, &snapshot));
    EXPECT_EQ(newBank.generation,
              snapshot.continuous.lastApplied.generation);
    EXPECT_EQ(harken::VariableControlSideState::Pending,
              snapshot.discrete.state);
}

TEST(VariableControlInstance,
     ConcurrentNewerBanksCannotPublishAnOlderPendingGeneration) {
    constexpr std::uint32_t kIterations = 512;
    harken::VariableControlInstanceTable table(kIterations);
    harken::VariableControlInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = UINT64_C(1) << 32;
    create.containerStableId = harkentest::ObjectId(2001);
    create.programSemanticFingerprint = 3001;
    std::vector<harken::VariableControlInstanceToken> instances(kIterations);
    for (auto& instance : instances) {
        ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
                  table.Acquire(create, &instance));
        ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
                  table.PrepareSide(
                      instance, harken::VariableControlSide::Discrete,
                      {3, 9}, {5, 11}));
    }

    std::barrier start(3);
    std::barrier finish(3);
    auto publish = [&](std::uint32_t decisionGeneration,
                       std::uint32_t bankGeneration) {
        for (const auto instance : instances) {
            start.arrive_and_wait();
            harken::DecisionToken superseded{};
            (void)table.PrepareNewerSide(
                instance, harken::VariableControlSide::Discrete,
                {7, decisionGeneration}, {5, bankGeneration},
                &superseded);
            finish.arrive_and_wait();
        }
    };
    std::thread older(publish, 15, 12);
    std::thread newer(publish, 16, 13);
    for (std::uint32_t iteration = 0; iteration < kIterations;
         ++iteration) {
        start.arrive_and_wait();
        finish.arrive_and_wait();
    }
    older.join();
    newer.join();

    std::uint32_t observedOlder = 0;
    std::uint32_t pendingOlder = 0;
    for (const auto instance : instances) {
        harken::VariableControlInstanceSnapshot snapshot{};
        ASSERT_TRUE(table.Capture(instance, &snapshot));
        observedOlder +=
            static_cast<std::uint32_t>(
                snapshot.discrete.newestObserved.generation != 13u);
        pendingOlder +=
            static_cast<std::uint32_t>(
                snapshot.discrete.newestPending.generation != 13u);
    }
    EXPECT_EQ(0u, observedOlder);
    EXPECT_EQ(0u, pendingOlder);
}

TEST(VariableControlInstance,
     ValidatedOldDecisionCannotActivateSupersedingPendingSide) {
    harken::VariableControlInstanceTable table(1);
    harken::VariableControlInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = UINT64_C(1) << 32;
    create.containerStableId = harkentest::ObjectId(2001);
    create.programSemanticFingerprint = 3001;
    harken::VariableControlInstanceToken instance{};
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.Acquire(create, &instance));

    const harken::DecisionToken oldDecision{3, 9};
    const harken::BankVersionToken oldBank{5, 11};
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.PrepareSide(instance,
                                harken::VariableControlSide::Discrete,
                                oldDecision, oldBank));
    ASSERT_TRUE(table.CanActivateDecision(instance, oldDecision));

    const harken::DecisionToken newDecision{7, 15};
    const harken::BankVersionToken newBank{5, 12};
    harken::DecisionToken superseded{};
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.PrepareNewerSide(
                  instance, harken::VariableControlSide::Discrete,
                  newDecision, newBank, &superseded));
    EXPECT_FALSE(table.ActivateDecision(instance, oldDecision));
    ASSERT_TRUE(table.ActivateDecision(instance, newDecision));

    harken::VariableControlInstanceSnapshot snapshot{};
    ASSERT_TRUE(table.Capture(instance, &snapshot));
    EXPECT_EQ(harken::VariableControlSideState::Committed,
              snapshot.discrete.state);
    EXPECT_EQ(newDecision.generation,
              snapshot.discrete.decision.generation);
    EXPECT_EQ(newBank.generation,
              snapshot.discrete.lastApplied.generation);
}

TEST(VariableControlInstance,
     ExactPreparedCutCanRestoreItsPriorCommittedIdentity) {
    harken::VariableControlInstanceTable table(1);
    harken::VariableControlInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = UINT64_C(1) << 32;
    create.containerStableId = harkentest::ObjectId(2001);
    create.programSemanticFingerprint = 3001;
    harken::VariableControlInstanceToken instance{};
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.Acquire(create, &instance));

    const harken::DecisionToken priorDecision{3, 9};
    const harken::BankVersionToken appliedBank{5, 11};
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.PrepareSide(instance,
                                harken::VariableControlSide::Discrete,
                                priorDecision, appliedBank));
    ASSERT_TRUE(table.ActivateSide(
        instance, harken::VariableControlSide::Discrete,
        priorDecision, appliedBank));

    const harken::DecisionToken preparedDecision{7, 15};
    harken::DecisionToken superseded{};
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.PrepareNewerSide(
                  instance, harken::VariableControlSide::Discrete,
                  preparedDecision, {5, 12}, &superseded));

    harken::VariableControlInstanceSnapshot before{};
    ASSERT_TRUE(table.Capture(instance, &before));
    EXPECT_FALSE(table.CancelPreparedSideAndRestoreCommitted(
        instance, harken::VariableControlSide::Discrete,
        {preparedDecision.slot, preparedDecision.generation + 1u},
        priorDecision));
    harken::VariableControlInstanceSnapshot afterMismatch{};
    ASSERT_TRUE(table.Capture(instance, &afterMismatch));
    EXPECT_EQ(before.discrete.state, afterMismatch.discrete.state);
    EXPECT_EQ(before.discrete.decision.generation,
              afterMismatch.discrete.decision.generation);
    EXPECT_EQ(before.discrete.newestObserved.generation,
              afterMismatch.discrete.newestObserved.generation);
    EXPECT_EQ(before.discrete.newestPending.generation,
              afterMismatch.discrete.newestPending.generation);
    EXPECT_EQ(before.discrete.lastApplied.generation,
              afterMismatch.discrete.lastApplied.generation);

    ASSERT_TRUE(table.CancelPreparedSideAndRestoreCommitted(
        instance, harken::VariableControlSide::Discrete,
        preparedDecision, priorDecision));
    harken::VariableControlInstanceSnapshot restored{};
    ASSERT_TRUE(table.Capture(instance, &restored));
    EXPECT_EQ(harken::VariableControlSideState::Committed,
              restored.discrete.state);
    EXPECT_EQ(priorDecision.generation,
              restored.discrete.decision.generation);
    EXPECT_EQ(appliedBank.generation,
              restored.discrete.newestObserved.generation);
    EXPECT_FALSE(restored.discrete.newestPending.Valid());
    EXPECT_EQ(appliedBank.generation,
              restored.discrete.lastApplied.generation);
}

TEST(VariableControlInstance,
     StaleActivationProofCannotCrossPreparedSideAba) {
    harken::VariableControlInstanceTable table(1);
    harken::VariableControlInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = UINT64_C(1) << 32;
    create.containerStableId = harkentest::ObjectId(2001);
    create.programSemanticFingerprint = 3001;
    harken::VariableControlInstanceToken instance{};
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.Acquire(create, &instance));

    const harken::DecisionToken priorDecision{3, 9};
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.PrepareSide(instance,
                                harken::VariableControlSide::Discrete,
                                priorDecision, {5, 10}));
    ASSERT_TRUE(table.ActivateDecision(instance, priorDecision));

    const harken::DecisionToken preparedDecision{7, 15};
    harken::DecisionToken superseded{};
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.PrepareNewerSide(
                  instance, harken::VariableControlSide::Discrete,
                  preparedDecision, {5, 11}, &superseded));
    harken::VariableControlActivationProof staleProof{};
    ASSERT_TRUE(table.ProveActivation(
        instance, preparedDecision, &staleProof));
    ASSERT_TRUE(table.CancelPreparedSideAndRestoreCommitted(
        instance, harken::VariableControlSide::Discrete,
        preparedDecision, priorDecision));
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.PrepareNewerSide(
                  instance, harken::VariableControlSide::Discrete,
                  preparedDecision, {5, 12}, &superseded));

    EXPECT_FALSE(table.ActivateProven(staleProof));
    harken::VariableControlInstanceSnapshot pending{};
    ASSERT_TRUE(table.Capture(instance, &pending));
    EXPECT_EQ(harken::VariableControlSideState::Pending,
              pending.discrete.state);
    EXPECT_EQ(12u, pending.discrete.newestPending.generation);
    EXPECT_EQ(10u, pending.discrete.lastApplied.generation);

    harken::VariableControlActivationProof freshProof{};
    ASSERT_TRUE(table.ProveActivation(
        instance, preparedDecision, &freshProof));
    ASSERT_TRUE(table.ActivateProven(freshProof));
    harken::VariableControlInstanceSnapshot committed{};
    ASSERT_TRUE(table.Capture(instance, &committed));
    EXPECT_EQ(12u, committed.discrete.lastApplied.generation);
}

TEST(VariableControlInstance,
     CancellingFirstWinnerRestoresCommittedSilentSide) {
    harken::VariableControlInstanceTable table(1);
    harken::VariableControlInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = UINT64_C(1) << 32;
    create.containerStableId = harkentest::ObjectId(2001);
    create.programSemanticFingerprint = 3001;
    harken::VariableControlInstanceToken instance{};
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.Acquire(create, &instance));
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.CommitSilentSide(
                  instance, harken::VariableControlSide::Discrete,
                  {5, 10}));

    const harken::DecisionToken preparedDecision{7, 15};
    harken::DecisionToken superseded{};
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.PrepareNewerSide(
                  instance, harken::VariableControlSide::Discrete,
                  preparedDecision, {5, 11}, &superseded));
    ASSERT_TRUE(table.CancelPreparedSideAndRestoreCommitted(
        instance, harken::VariableControlSide::Discrete,
        preparedDecision, {}));

    harken::VariableControlInstanceSnapshot restored{};
    ASSERT_TRUE(table.Capture(instance, &restored));
    EXPECT_EQ(harken::VariableControlSideState::Committed,
              restored.discrete.state);
    EXPECT_FALSE(restored.discrete.decision.Valid());
    EXPECT_EQ(10u, restored.discrete.newestObserved.generation);
    EXPECT_FALSE(restored.discrete.newestPending.Valid());
    EXPECT_EQ(10u, restored.discrete.lastApplied.generation);
}

TEST(VariableControlInstance,
     SilentSideOwnsExactBankAndFirstWinnerMintsDecision) {
    harken::VariableControlInstanceTable table(1);
    harken::VariableControlInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = UINT64_C(1) << 32;
    create.containerStableId = harkentest::ObjectId(2001);
    create.programSemanticFingerprint = 3001;
    harken::VariableControlInstanceToken instance{};
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.Acquire(create, &instance));

    const harken::BankVersionToken silentBank{5, 11};
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.CommitSilentSide(
                  instance, harken::VariableControlSide::Discrete,
                  silentBank));
    harken::VariableControlInstanceSnapshot snapshot{};
    ASSERT_TRUE(table.Capture(instance, &snapshot));
    EXPECT_EQ(harken::VariableControlSideState::Committed,
              snapshot.discrete.state);
    EXPECT_FALSE(snapshot.discrete.decision.Valid());
    EXPECT_EQ(silentBank.generation,
              snapshot.discrete.lastApplied.generation);
    EXPECT_EQ(silentBank.generation,
              snapshot.discrete.newestObserved.generation);
    EXPECT_FALSE(snapshot.discrete.newestPending.Valid());

    harken::DecisionToken untouched{71, 73};
    EXPECT_EQ(harken::VariableControlInstanceStatus::Stale,
              table.PrepareNewerSide(
                  instance, harken::VariableControlSide::Discrete,
                  {3, 9}, silentBank, &untouched));
    EXPECT_EQ(71u, untouched.slot);
    EXPECT_EQ(73u, untouched.generation);

    const harken::DecisionToken winnerDecision{3, 9};
    const harken::BankVersionToken winnerBank{5, 12};
    harken::DecisionToken superseded{71, 73};
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.PrepareNewerSide(
                  instance, harken::VariableControlSide::Discrete,
                  winnerDecision, winnerBank, &superseded));
    EXPECT_FALSE(superseded.Valid());
    ASSERT_TRUE(table.ActivateSide(
        instance, harken::VariableControlSide::Discrete,
        winnerDecision, winnerBank));
    ASSERT_TRUE(table.Capture(instance, &snapshot));
    EXPECT_EQ(winnerDecision.generation,
              snapshot.discrete.decision.generation);
    EXPECT_EQ(winnerBank.generation,
              snapshot.discrete.lastApplied.generation);
}

TEST(VariableControlInstance,
     NoTopologyChangeAdvancesOnlyExactCommittedSideBank) {
    harken::VariableControlInstanceTable table(1);
    harken::VariableControlInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = UINT64_C(1) << 32;
    create.containerStableId = harkentest::ObjectId(2001);
    create.programSemanticFingerprint = 3001;
    harken::VariableControlInstanceToken instance{};
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.Acquire(create, &instance));

    const harken::BankVersionToken first{5, 11};
    const harken::BankVersionToken second{5, 12};
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.CommitSilentSide(
                  instance, harken::VariableControlSide::Discrete, first));
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.AdvanceCommittedSideBank(
                  instance, harken::VariableControlSide::Discrete,
                  first, second));

    harken::VariableControlInstanceSnapshot snapshot{};
    ASSERT_TRUE(table.Capture(instance, &snapshot));
    EXPECT_EQ(harken::VariableControlSideState::Committed,
              snapshot.discrete.state);
    EXPECT_FALSE(snapshot.discrete.decision.Valid());
    EXPECT_EQ(second.generation,
              snapshot.discrete.newestObserved.generation);
    EXPECT_EQ(second.generation,
              snapshot.discrete.lastApplied.generation);
    EXPECT_FALSE(snapshot.discrete.newestPending.Valid());
    const harken::VariableControlInstanceSnapshot before = snapshot;

    EXPECT_EQ(harken::VariableControlInstanceStatus::Stale,
              table.AdvanceCommittedSideBank(
                  instance, harken::VariableControlSide::Discrete,
                  first, {5, 13}));
    ASSERT_TRUE(table.Capture(instance, &snapshot));
    EXPECT_EQ(before.discrete.lastApplied.generation,
              snapshot.discrete.lastApplied.generation);
    EXPECT_EQ(before.discrete.newestObserved.generation,
              snapshot.discrete.newestObserved.generation);
}

TEST(VariableControlBank, CapturesOneExactObservationForEveryRequestedAxis) {
    VnConfig config = harkentest::MakeNullConfig(2, 64, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_NE(nullptr, engine);

    const harken::VariableDefinitionV1 definitions[2] = {
        {harkentest::ObjectId(41), harken::VariableKindV1::Step, 0.0f, 8.0f, 3.0f},
        {harkentest::ObjectId(42), harken::VariableKindV1::Smooth, -1.0f, 1.0f, 0.25f}};
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallDefinitionsWorld(
                  engine, definitions, 2, UINT64_C(0x564347524944))
                  .status);

    const VnVariableId axes[2] = {harkentest::ObjectId(41), harkentest::ObjectId(42)};
    float values[2] = {99.0f, 99.0f};
    harken::BankVersionToken token{};
    ASSERT_TRUE(engine->variables.CaptureCurrentBank(
        0, axes, 2, values, &token));
    EXPECT_TRUE(token.Valid());
    EXPECT_FLOAT_EQ(3.0f, values[0]);
    EXPECT_FLOAT_EQ(0.25f, values[1]);

    const harken::BankVersionToken initial = token;
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(41), 4.0f));
    for (std::uint32_t turn = 0;
         turn < 16 && engine->variables.PendingCount() != 0; ++turn) {
        ASSERT_EQ(VN_OK, Vn_Update());
        engine->nullBackend.Pump(1);
    }
    ASSERT_EQ(0u, engine->variables.PendingCount());
    ASSERT_TRUE(engine->variables.CaptureCurrentBank(
        0, axes, 2, values, &token));
    EXPECT_FLOAT_EQ(4.0f, values[0]);
    EXPECT_FLOAT_EQ(0.25f, values[1]);
    EXPECT_EQ(initial.slot, token.slot);
    EXPECT_NE(initial.generation, token.generation);

    const VnVariableId badAxes[2] = {harkentest::ObjectId(41), harkentest::ObjectId(999)};
    values[0] = 71.0f;
    values[1] = 72.0f;
    const harken::BankVersionToken before = token;
    EXPECT_FALSE(engine->variables.CaptureCurrentBank(
        0, badAxes, 2, values, &token));
    EXPECT_FLOAT_EQ(71.0f, values[0]);
    EXPECT_FLOAT_EQ(72.0f, values[1]);
    EXPECT_EQ(before.slot, token.slot);
    EXPECT_EQ(before.generation, token.generation);
}

TEST(VariableControlBank,
     LiveEmitterFallbackGlobalWriteAdvancesExactScopeBank) {
    VnConfig config = harkentest::MakeNullConfig(2, 64, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_NE(nullptr, engine);

    const harken::VariableDefinitionV1 definition = {
        harkentest::ObjectId(41), harken::VariableKindV1::Step, 0.0f, 8.0f, 3.0f};
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallDefinitionsWorld(
                  engine, &definition, 1,
                  UINT64_C(0x5643454D474C4F42)).status);
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    const VnVariableId axis = harkentest::ObjectId(41);
    float value = 0.0f;
    harken::BankVersionToken initial{};
    ASSERT_TRUE(engine->variables.CaptureCurrentBank(
        emitter, &axis, 1, &value, &initial));
    ASSERT_FLOAT_EQ(3.0f, value);

    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(41), 7.0f));
    for (std::uint32_t turn = 0;
         turn < 16 && engine->variables.PendingCount() != 0; ++turn) {
        ASSERT_EQ(VN_OK, Vn_Update());
        engine->nullBackend.Pump(1);
    }
    ASSERT_EQ(0u, engine->variables.PendingCount());
    harken::BankVersionToken after{};
    ASSERT_TRUE(engine->variables.CaptureCurrentBank(
        emitter, &axis, 1, &value, &after));
    EXPECT_FLOAT_EQ(7.0f, value);
    EXPECT_EQ(initial.slot, after.slot);
    EXPECT_GT(after.generation, initial.generation);
}

} // namespace
