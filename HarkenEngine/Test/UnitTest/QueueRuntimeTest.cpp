#include "harken/core/PrivateSelectedQueueFutureSpan.h"
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"
#include "harken/core/QueueRuntime.h"
#include "harken/core/ArmGroupTable.h"
#include "harken/core/KnownCompletion.h"
#include "harken/core/DecisionTable.h"
#include "harken/core/LogicCommitCoordinator.h"
#include "harken/core/OccurrenceStatePool.h"
#include "harken/core/ScheduledTransactionPool.h"
#include "harken/render/VoicePool.h"
#include "harken/render/VoiceTimelineSink.h"
#include "Engine.h"

#include <gtest/gtest.h>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace harken {
struct EngineQueueProgramBankTestAccess {
    static void SetGenerationToLastReservable(EngineQueueProgramBank& bank) {
        ASSERT_FALSE(bank.occupied);
        ASSERT_FALSE(bank.HasOriginalLifetimeOwners());
        bank.generation_ = UINT64_MAX - 1u;
    }
    static void SetLifetimeMarkerToLastIssuable(EngineQueueProgramBank& bank) {
        ASSERT_EQ(0u, bank.originalLifetimePinCount_);
        bank.lastLifetimePinMarker_ = UINT64_MAX - 1u;
    }
};
struct QueueInstanceTableTestAccess {

    // Observation and negative prerequisites only. No helper constructs a cap.
    static std::uint32_t OriginalConditionalIndex(
        const QueueInstanceTable& table, QueueInstanceToken token) {
        return table.rows_[token.slot].Pending().originalConditionalRow;
    }
    static std::vector<std::uint64_t> OriginalConditionalRecord(
        const QueueInstanceTable& table, std::uint32_t index) {
        const auto& row = table.originalConditionalRows_[index];
        return {static_cast<std::uint64_t>(row.state), row.generation, row.marker,
            row.loans, row.queue.slot, row.queue.generation, row.version.version,
            row.world, row.rootInstance, row.programSemanticFingerprint,
            row.decision.slot, row.decision.generation, row.current.slot,
            row.current.generation, row.successor.slot, row.successor.generation,
            row.currentState.version, row.next.version, row.first.version,
            row.stepCount, row.resetIntent, row.consumesReset,
            row.logicalBoundary, row.logicalFloor, row.logicalCompletionFrame};
    }
    static void SetOriginalConditionalFactoryMarkerMax(QueueInstanceTable& table) {
        table.nextOriginalConditionalFactoryMarker_ = UINT64_MAX;
    }
    static void SetOriginalConditionalLoanCount(
        QueueInstanceTable& table, std::uint32_t index, std::uint32_t count) {
        ASSERT_NE(0u, count);
        ASSERT_NE(0u, table.originalConditionalRows_[index].loans);
        table.originalConditionalRows_[index].loans = count;
    }
    static void SetFreeOriginalConditionalGenerationMax(
        QueueInstanceTable& table, std::uint32_t index) {
        ASSERT_TRUE(table.PrepareConditionalSlot(index));
        auto& row = table.originalConditionalRows_[index];
        ASSERT_EQ(QueueInstanceTable::OriginalConditionalState::Free, row.state);
        ASSERT_EQ(0u, row.loans);
        ASSERT_FALSE(row.parent.Valid());
        row.generation = UINT32_MAX;
    }
    static std::pair<std::uint64_t, std::uint32_t> OriginalLifetime(
        const QueueInstanceTable& table, QueueInstanceToken token) {
        const auto& row = table.rows_[token.slot];
        return {row.originalLifetimeMarker, row.originalLifetimePinCount};
    }
    static void SetOriginalLifetimeMarkerToLastIssuable(QueueInstanceTable& table) {
        table.nextOriginalLifetimeMarker_ = UINT64_MAX - 1u;
    }
    static void SetOriginalLifetimeCount(QueueInstanceTable& table,
        QueueInstanceToken token, std::uint32_t count) {
        ASSERT_NE(0u, table.rows_[token.slot].originalLifetimePinCount);
        ASSERT_NE(0u, count);
        table.rows_[token.slot].originalLifetimePinCount = count;
    }
    static void SetFreeNativeGenerationToMax(QueueInstanceTable& table) {
        ASSERT_TRUE(table.PrepareInstanceSlot(0));
        ASSERT_EQ(QueueInstanceTable::SlotState::Free, table.rows_[0].slotState.load());
        table.rows_[0].generation = UINT32_MAX;
    }
    static void SetCatalogScopeMarkerLimit(QueueInstanceTable& table) {
        table.nextCatalogResetScopeMarker_ = UINT64_MAX;
    }
    static std::pair<KnownCompletionPredicateV1, std::uint64_t>
    PendingTiming(const QueueInstanceTable& table, QueueInstanceToken token) {
        const auto& row = table.rows_[token.slot];
        return {row.Pending().pendingCompletion, row.Pending().pendingLogicalCompletionFrame};
    }
    static void SetSignalLimits(QueueInstanceTable& table, QueueInstanceToken token,
                                std::uint64_t mirrorSequence, std::uint64_t attempt) {
        table.rows_[token.slot].mirrorSequence.store(mirrorSequence);
        table.rows_[token.slot].signalAttemptCounter = attempt;
    }
    static std::vector<std::uint64_t> MirrorAndReset(const QueueInstanceTable& table, QueueInstanceToken token) {
        const auto& row = table.rows_[token.slot];
        std::vector<std::uint64_t> values{
            row.mirrorSequence.load(), row.mirrorVersion.load(), row.mirrorCursor.load(),
            row.mirrorCurrentChildOrdinal.load(), row.mirrorCycleOrdinal.load(),
            row.mirrorSelectionOrdinal.load(), row.mirrorRandomInitialRecentCount.load(),
            row.resetEpoch, row.resetReserved.load(), row.resetPending.load(), row.resetExecuteFrame};
        for (const auto& recent : row.mirrorRandomInitialRecent) values.push_back(recent.load());
        const auto append = [&](const EffectiveQueueState& state) {
            values.insert(values.end(), {state.version, state.cursor, state.currentChildOrdinal,
                state.cycleOrdinal, state.selectionOrdinal, state.randomInitialRecentCount});
            for (const auto recent : state.randomInitialRecent) values.push_back(recent);
        };
        append(row.resetInitial);
        values.push_back(row.resetNextVersion);
        return values;
    }
    static std::vector<std::uint64_t> Snapshot(const QueueInstanceTable& table, QueueInstanceToken token) {
        auto values = MirrorAndReset(table, token);
        const auto& row = table.rows_[token.slot];
        values.insert(values.end(), {static_cast<std::uint64_t>(row.slotState.load()), row.generation,
            row.Pending().pendingVersion.load(), row.Pending().pendingNext.version, row.Pending().pendingDecision.slot,
            row.Pending().pendingDecision.generation, static_cast<std::uint64_t>(row.Pending().pendingActivation.load()),
            row.conditionalReserved.load(), row.conditionalReplacementReserved.load(),
            row.Pending().pendingConsumesReset.load(), static_cast<std::uint64_t>(row.failurePhase.load()),
            static_cast<std::uint64_t>(row.failureReason), row.failureFrame});
        return values;
    }
    static bool PendingCompletionIdentityEmpty(
        const QueueInstanceTable& table, QueueInstanceToken token) {
        const auto& row = table.rows_[token.slot];
        return !row.Pending().pendingCompletionIsLogicalBoundary &&
            !row.Pending().pendingCompletionHasLogicalFloor &&
            row.Pending().pendingLogicalCompletionFrame == 0 &&
            !row.Pending().pendingRetiredSource.sourceArm.Valid() &&
            row.Pending().pendingRetiredSource.sourceRenderHandle == 0 &&
            row.Pending().pendingRetiredSource.logicalCompletionSequence == 0 &&
            row.Pending().pendingRetiredSource.tombstoneAudioSequence == 0;
    }
    static void SetRejectionState(QueueInstanceTable& table, QueueInstanceToken token, unsigned index, bool enabled) {
        auto& row = table.rows_[token.slot];
        switch (index) {
            case 0: row.Pending().pendingActivation.store(enabled ? ConditionalQueueActivation::Activated : ConditionalQueueActivation::Waiting); break;
            case 1: row.Pending().pendingActivation.store(enabled ? ConditionalQueueActivation::InvalidatedIdentity : ConditionalQueueActivation::Waiting); break;
            case 2: row.conditionalReserved.store(enabled); break;
            case 3: row.conditionalReplacementReserved.store(enabled); break;
            case 4: row.Pending().pendingNext.version = enabled ? 99 : row.Pending().pendingVersion.load(); break;
            case 5: row.slotState.store(enabled ? QueueInstanceTable::SlotState::Live : QueueInstanceTable::SlotState::Initializing); break;
        }
    }
    static void SeedResetState(QueueInstanceTable& table, QueueInstanceToken token) {
        auto& row = table.rows_[token.slot];
        row.resetPending.store(true);
        row.resetReserved.store(true);
        row.resetExecuteFrame = 4096;
        row.resetNextVersion = 19;
    }
};
} // namespace harken

namespace {

harken::KnownCompletionPredicateV1 BoundPredicate(
    harken::DecisionToken decision,
    harken::OccurrenceToken occurrence = {29, 31},
    std::uint32_t renderGeneration = 43) {
    harken::KnownCompletionFactsV1 facts{};
    facts.occurrence = occurrence;
    facts.renderGeneration = renderGeneration;
    facts.residentExactMedia = true;
    facts.pcmFrameCount = 300;
    facts.loopEndFrame = 300;
    facts.loopCount = 1;
    facts.sourceRate = 48000;
    facts.mixRate = 48000;
    facts.pitchFixed = true;
    facts.serialRender = true;
    facts.absoluteLogicalStartFrame = 0;
    facts.playbackFactsRevision = 1;
    facts.program.kind = harken::PlayableKindV1::Sound;
    facts.program.stableId = harkentest::ObjectId(47);
    facts.program.world = 7;
    facts.program.runtimeSemanticFingerprint = 53;
    harken::KnownCompletionPredicateV1 candidate{};
    EXPECT_EQ(harken::CompletionPredictability::KnownFinite,
              harken::MaterializeKnownCompletion(facts, &candidate));
    harken::KnownCompletionPredicateV1 bound{};
    EXPECT_TRUE(harken::BindKnownCompletionPredicate(
        candidate, facts.program,
        harken::CompletionOwnerRef::Single(decision, 1), &bound));
    return bound;
}

harken::QueueInstanceToken AcquireLogicalBoundaryQueue(
    harken::QueueInstanceTable& table,
    std::uint64_t rootInstance = 11) {
    harken::QueueInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = rootInstance;
    create.containerStableId = harkentest::ObjectId(13);
    create.programSemanticFingerprint = 17;
    create.childCount = 2;
    create.initial.version = 1;
    create.initial.currentChildOrdinal = 3;
    harken::QueueInstanceToken queue{};
    EXPECT_TRUE(table.Acquire(create, &queue));
    return queue;
}

harken::ConditionalQueuePrepareRequest LogicalBoundaryRequest(
    std::uint64_t frame = 300) {
    harken::ConditionalQueuePrepareRequest request{};
    request.expectedCurrentVersion = 1;
    request.rootDecision = {19, 23};
    request.expectedCurrentOccurrence = {29, 31};
    request.successorOccurrence = {37, 41};
    request.completionIsLogicalBoundary = true;
    request.logicalCompletionFrame = frame;
    request.next.version = 2;
    request.next.currentChildOrdinal = 5;
    return request;
}

harken::QueueInstanceCreateRequest OriginalQueueCreateRequest() {
    harken::QueueInstanceCreateRequest request{};
    request.world = 7;
    request.rootInstance = 11;
    request.containerStableId = harkentest::ObjectId(13);
    request.programSemanticFingerprint = 17;
    request.childCount = 2;
    request.initial.version = 1;
    request.initial.currentChildOrdinal = 3;
    return request;
}

TEST(QueueRuntime, FollowingConditionalRequiresActualPredecessorSourceAndSettlesInOrder) {
    harken::QueueInstanceTable queues(1, 2);
    harkentest::EngineSession session(harkentest::MakeNullConfig(8, 64, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    auto& arms = engine.timelineArms;
    std::vector<float> pcm(300, 0.25f);
    const auto pcmDesc = harkentest::MonoDesc(pcm, 48000);
    const auto sound = harkentest::ObjectId(47);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &pcmDesc));
    harken::QueueInstanceTable::OriginalLifetimePin parent;
    harken::QueueInstanceToken queue{};
    ASSERT_TRUE(queues.Acquire(OriginalQueueCreateRequest(), &queue, &parent));
    auto first = LogicalBoundaryRequest();
    ASSERT_TRUE(engine.timelineOccurrences.Acquire({11, sound, 0, 5}, &first.successorOccurrence));
    harken::QueueInstanceTable::OriginalConditionalFutureReservation firstOriginal;
    harken::PreparedQueueConditionalV1 firstPrepared;
    harken::ConditionalQueueVersionToken firstVersion{};
    ASSERT_TRUE(queues.PrepareConditional(queue, first, &firstPrepared, parent, &firstOriginal));
    ASSERT_TRUE(queues.CommitPreparedConditional(&firstPrepared, &firstVersion));

    const auto media = engine.registry.Lookup(sound);
    VnVoiceHandle render = 0;
    ASSERT_EQ(VN_OK, engine.voices.Play(media, 0.0f, 1.0, 0, false, 0, 0, 0,
        {}, 0, 300, 1, harken::ResampleProfile::LegacyCompatibility, &render,
        1, UINT32_MAX, 0, true));
    const auto renderGeneration = static_cast<std::uint32_t>(render >> 32);
    ASSERT_TRUE(engine.timelineOccurrences.BindCompletionProgress(first.successorOccurrence,
        renderGeneration, 1, 0));
    auto* progress = engine.timelineOccurrences.CompletionProgressOf(first.successorOccurrence);
    ASSERT_TRUE(engine.voices.BindCompletionProgress(render, progress, first.successorOccurrence, 1));
    harken::VoicePool::PreparedCommitInput physical{};
    physical.renderHandle = render;
    ASSERT_TRUE(engine.voices.CaptureReleaseObserver(render, &physical.originalRelease));
    physical.completionProgress = progress;
    physical.completionOccurrence = first.successorOccurrence;
    physical.completionRenderGeneration = renderGeneration;
    physical.playbackFactsRevision = 1;
    ASSERT_TRUE(engine.voices.CapturePreparedRoutingV1(render, &physical.routing));
    harken::VoicePool::PreparedCommitProof voiceProof;
    ASSERT_TRUE(engine.voices.ProvePreparedCommit(physical, &voiceProof));
    harken::TimelineArmTable::PreparedCommitInput logical{};
    logical.startFrame = logical.armDeadline = 300;
    logical.programGeneration = 11;
    logical.target.programSemanticFingerprint = 17;
    logical.target.subjectId = sound;
    logical.parentGroup = {first.rootDecision};
    logical.memberIndex = 0;
    harken::TimelineArmToken sourceArm{};
    ASSERT_TRUE(arms.PrepareGrouped(static_cast<std::uint32_t>(render), renderGeneration,
        300, 300, 11, logical.target, logical.parentGroup, 0, &sourceArm));
    logical.arm = sourceArm;
    harken::TimelineArmTable::PreparedCommitProof armProof;
    ASSERT_TRUE(arms.ProvePreparedCommit(logical, &armProof));
    harken::CommandRecord command{};
    command.opcode = harken::kOpcodePlay;
    command.payload.play.handle = command.payload.play.sourceIdentity = render;
    command.payload.play.batch.batchVersion = harken::kPlayLedBatchVersion;
    command.payload.play.batch.batchKind = harken::PlayLedBatchKind::TimelineArm;
    command.payload.play.batch.batchCommandCount = 1;
    command.payload.play.batch.armToken =
        (static_cast<std::uint64_t>(sourceArm.generation) << 32) | sourceArm.slot;
    ASSERT_TRUE(engine.originalSourceSubmissions.CapturePreparedSource(command, armProof, voiceProof, {}));
    harken::CommandQueue::BatchReservation reservation{};
    ASSERT_TRUE(engine.commands.ReserveBatch(1, &reservation));
    harken::CommandQueue::PreparedBatchProof batch;
    ASSERT_TRUE(engine.commands.ProvePreparedBatch(&reservation, &command, 1, &batch));
    ASSERT_TRUE(engine.originalSourceSubmissions.BindPreparedChild(batch, 0, 1));
    harken::VoicePool::ConsumePreparedCommit(std::move(voiceProof));
    harken::TimelineArmTable::ConsumePreparedCommit(std::move(armProof));
    engine.originalSourceSubmissions.PublishPreparedChild(command);
    (void)engine.commands.ConsumePreparedBatch(std::move(batch));
    harken::QueueInstanceTable::ContinuationSourceBinding sourceBinding;
    ASSERT_TRUE(engine.originalSourceSubmissions.ProveQueueContinuationSource(sourceArm,
        engine.timelineOccurrences, first.successorOccurrence, &sourceBinding));
    for (const auto renderSlot : {sourceArm.renderSlot, sourceArm.renderSlot + 1u}) {
        harken::TimelineArmToken wrong{};
        ASSERT_TRUE(arms.ReserveGrouped(renderSlot, renderGeneration, 300, 300, 11,
            logical.target, logical.parentGroup, 1, &wrong));
        harken::QueueInstanceTable::ContinuationSourceBinding wrongBinding;
        EXPECT_FALSE(engine.originalSourceSubmissions.ProveQueueContinuationSource(wrong,
            engine.timelineOccurrences, first.successorOccurrence, &wrongBinding));
        EXPECT_FALSE(wrongBinding.Valid());
    }
    auto second = LogicalBoundaryRequest(0);
    second.expectedCurrentVersion = 2;
    second.rootDecision = {20, 23};
    second.expectedCurrentOccurrence = first.successorOccurrence;
    second.successorOccurrence = {39, 41};
    second.completionIsLogicalBoundary = false;
    second.completion = BoundPredicate(second.rootDecision, second.expectedCurrentOccurrence, renderGeneration);
    second.completionOwner = second.completion.owner;
    second.next.version = 3;
    second.next.currentChildOrdinal = 3;
    harken::QueueInstanceTable::OriginalConditionalFutureReservation secondOriginal;
    harken::PreparedQueueConditionalV1 secondPrepared;
    harken::ConditionalQueueVersionToken secondVersion{};
    ASSERT_TRUE(queues.PrepareFollowingConditional(queue, second, firstOriginal, sourceBinding, &secondPrepared, parent, &secondOriginal));
    ASSERT_TRUE(queues.CommitPreparedConditional(&secondPrepared, &secondVersion));
    EXPECT_FALSE(queues.ActivateConditional(queue, secondVersion, second.rootDecision,
        second.expectedCurrentOccurrence));
    ASSERT_TRUE(queues.ActivateConditional(queue, firstVersion, first.rootDecision,
        first.expectedCurrentOccurrence));
    EXPECT_FALSE(queues.ActivateConditional(queue, secondVersion, second.rootDecision,
        second.expectedCurrentOccurrence));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine.nullBackend.Pump(2);
    ASSERT_TRUE(arms.Publish(sourceArm));
    arms.ApplyClaimedAudioSlice(sourceArm);
    ASSERT_TRUE(arms.Start(sourceArm, 2));
    ASSERT_TRUE(queues.ActivateConditional(queue, secondVersion, second.rootDecision,
        second.expectedCurrentOccurrence));
    harken::EffectiveQueueState effective{};
    ASSERT_TRUE(queues.ResolveEffective(queue, &effective));
    EXPECT_EQ(3u, effective.version);
    EXPECT_FALSE(queues.SettleActivated(queue, secondVersion));
    ASSERT_TRUE(queues.SettleActivated(queue, firstVersion));
    EXPECT_FALSE(queues.SettleActivated(queue, secondVersion, 1));
    ASSERT_TRUE(queues.SettleActivated(queue, secondVersion, 2));
    harken::QueueInstanceSnapshot snapshot{};
    ASSERT_TRUE(queues.Capture(queue, &snapshot));
    EXPECT_EQ(3u, snapshot.logicMirror.version);
    EXPECT_EQ(0u, snapshot.pendingVersion);
    secondOriginal.Release();
    firstOriginal.Release();
    parent.Release();
    EXPECT_TRUE(queues.Release(queue));
}

TEST(QueueRuntime, UnusedTablesReserveIdentitySpaceWithoutConstructingRows) {
    harken::QueueInstanceTable queues(harken::kMaxQueueInstancesHard,
        harken::kMaxQueueInstancesHard);
    harken::QueueCheckpointTable checkpoints(harken::kMaxQueueCheckpointRowsHard);
    harken::PrivateSelectedQueueFutureSpanPool selected(harken::kMaxQueueInstancesHard);
    EXPECT_TRUE(queues.Valid());
    EXPECT_TRUE(checkpoints.Valid());
    EXPECT_EQ(0u, queues.ConstructedRowCount());
    EXPECT_EQ(0u, queues.ConstructedConditionalRowCount());
    EXPECT_EQ(0u, checkpoints.ConstructedRowCount());
    EXPECT_EQ(0u, selected.ConstructedRowCount());
    EXPECT_EQ(0u, queues.RowCommittedBytes());
    EXPECT_EQ(0u, queues.ConditionalRowCommittedBytes());
    EXPECT_EQ(0u, checkpoints.RowCommittedBytes());
    EXPECT_EQ(0u, selected.RowCommittedBytes());
    harken::QueueInstanceSnapshot snapshot;
    EXPECT_FALSE(queues.Capture({harken::kMaxQueueInstancesHard - 1u, 1}, &snapshot));
    queues.ApplyDue(19);
    checkpoints.ClearAll();
    EXPECT_EQ(0u, queues.LiveCount());
    EXPECT_EQ(0u, queues.QuarantinedCount());
    EXPECT_EQ(0u, checkpoints.LiveCount());
}

TEST(QueueRuntime, PreparedRowsSurviveCompetingAcquireAndRetainedConditionalReturn) {
    harken::QueueInstanceTable queues(4, 4);
    harken::QueueInstanceTable::BackingPreparation backing;
    ASSERT_TRUE(queues.PrepareBacking(2, 2, &backing));
    harken::QueueInstanceToken competitor;
    ASSERT_TRUE(queues.Acquire(OriginalQueueCreateRequest(), &competitor));
    EXPECT_EQ(3u, queues.ConstructedRowCount());
    const auto rowBytes = queues.RowStorageBytes();
    const auto conditionalBytes = queues.ConditionalRowStorageBytes();
    queues.SetRowCommitPageLimitForTests(0);
    queues.SetConditionalRowCommitPageLimitForTests(0);
    std::array<harken::QueueInstanceToken, 2> tokens;
    std::array<harken::QueueInstanceTable::OriginalLifetimePin, 2> parents;
    std::array<harken::QueueInstanceTable::OriginalConditionalFutureReservation, 2> originals;
    std::array<harken::PreparedQueueConditionalV1, 2> prepared;
    std::array<bool, 2> acquired{}, conditional{};
    harken::QueueInstanceToken refused;
    bool extra = false;
    {
        harken::QueueInstanceTable::BackingUse use(&backing);
        harken::memguard::AudioThreadScope audio;
        for (std::uint32_t index = 0; index < tokens.size(); ++index) {
            acquired[index] = queues.Acquire(OriginalQueueCreateRequest(), &tokens[index], &parents[index]);
            conditional[index] = queues.PrepareConditional(tokens[index], LogicalBoundaryRequest(),
                &prepared[index], parents[index], &originals[index]);
        }
        extra = queues.Acquire(OriginalQueueCreateRequest(), &refused);
    }
    EXPECT_FALSE(extra);
    EXPECT_EQ(rowBytes, queues.RowStorageBytes());
    EXPECT_EQ(conditionalBytes, queues.ConditionalRowStorageBytes());
    for (std::uint32_t index = 0; index < tokens.size(); ++index) {
        ASSERT_TRUE(acquired[index]);
        ASSERT_TRUE(conditional[index]);
        ASSERT_TRUE(queues.CancelPreparedConditional(&prepared[index]));
        EXPECT_TRUE(originals[index].Valid());
        EXPECT_FALSE(queues.Release(tokens[index], &parents[index]));
    }
    backing = {};
    originals[0].Release();
    ASSERT_TRUE(queues.Release(tokens[0], &parents[0]));
    EXPECT_TRUE(originals[1].Valid());
    originals[1].Release();
    ASSERT_TRUE(queues.Release(tokens[1], &parents[1]));
    ASSERT_TRUE(queues.Release(competitor));
}

TEST(QueueRuntime, FailedBackingPreparationRetainsOnlyConstructedPrefixAndCanRetry) {
    harken::QueueInstanceTable queues(16, 4);
    queues.SetRowCommitPageLimitForTests(1);
    harken::QueueInstanceTable::BackingPreparation refused;
    EXPECT_FALSE(queues.PrepareBacking(16, 4, &refused));
    EXPECT_FALSE(refused.instances.Valid());
    EXPECT_FALSE(refused.conditionals.Valid());
    EXPECT_GT(queues.ConstructedRowCount(), 0u);
    EXPECT_LT(queues.ConstructedRowCount(), 16u);
    EXPECT_EQ(0u, queues.ConstructedConditionalRowCount());
    EXPECT_EQ(0u, queues.LiveCount());
    queues.SetRowCommitPageLimitForTests(SIZE_MAX);
    ASSERT_TRUE(queues.PrepareBacking(2, 2, &refused));
    harken::QueueInstanceToken token;
    {
        harken::QueueInstanceTable::BackingUse use(&refused);
        ASSERT_TRUE(queues.Acquire(OriginalQueueCreateRequest(), &token));
    }
    EXPECT_TRUE(queues.Release(token));
}

TEST(QueueRuntime, AudioRefusesMissingBackingBeforeInvokingDependencyPreparation) {
    harken::QueueInstanceTable queues(2);
    std::uint32_t dependencyCalls = 0;
    ASSERT_TRUE(queues.AttachRowStoragePreparation(&dependencyCalls,
        [](void* context, std::uint32_t) noexcept {
            ++*static_cast<std::uint32_t*>(context);
            return true;
        }));
    harken::QueueInstanceToken token;
    bool acquired = true;
    {
        harken::memguard::AudioThreadScope audio;
        acquired = queues.Acquire(OriginalQueueCreateRequest(), &token);
    }
    EXPECT_FALSE(acquired);
    EXPECT_EQ(0u, dependencyCalls);
    EXPECT_EQ(0u, queues.ConstructedRowCount());
    harken::QueueInstanceTable::BackingPreparation backing;
    ASSERT_TRUE(queues.PrepareBacking(1, 0, &backing));
    EXPECT_EQ(1u, dependencyCalls);
    {
        harken::QueueInstanceTable::BackingUse use(&backing);
        harken::memguard::AudioThreadScope audio;
        acquired = queues.Acquire(OriginalQueueCreateRequest(), &token);
    }
    ASSERT_TRUE(acquired);
    EXPECT_EQ(1u, dependencyCalls);
    EXPECT_TRUE(queues.Release(token));
}

TEST(QueueRuntime, OriginalInitializingLoansRetainPreparedRowUntilEveryReturn) {
    using Pin = harken::QueueInstanceTable::OriginalLifetimePin;
    static_assert(!std::is_copy_constructible_v<Pin>);
    static_assert(!std::is_copy_assignable_v<Pin>);
    static_assert(std::is_nothrow_move_constructible_v<Pin>);
    harken::QueueInstanceTable table(1);
    harken::PreparedQueueInstanceV1 prepared;
    Pin original;
    ASSERT_TRUE(table.PrepareAcquire(OriginalQueueCreateRequest(), &prepared, &original));
    const auto token = prepared.Token();
    const auto before = harken::QueueInstanceTableTestAccess::Snapshot(table, token);
    ASSERT_TRUE(original.Valid());
    ASSERT_TRUE(table.IsOriginalLifetimePinFor(original, token));
    Pin first;
    Pin second;
    ASSERT_TRUE(original.Borrow(&first));
    ASSERT_TRUE(first.Borrow(&second));
    EXPECT_EQ(3u, harken::QueueInstanceTableTestAccess::OriginalLifetime(table, token).second);
    EXPECT_FALSE(table.CancelPreparedAcquire(&prepared));
    EXPECT_TRUE(prepared.Valid());
    EXPECT_EQ(before, harken::QueueInstanceTableTestAccess::Snapshot(table, token));
    original.Release();
    original.Release();
    EXPECT_TRUE(first.Valid());
    EXPECT_FALSE(table.CancelPreparedAcquire(&prepared));
    first.Release();
    EXPECT_TRUE(second.Valid());
    EXPECT_FALSE(table.CancelPreparedAcquire(&prepared));
    second.Release();
    EXPECT_EQ(0u, harken::QueueInstanceTableTestAccess::OriginalLifetime(table, token).first);
    EXPECT_TRUE(table.CancelPreparedAcquire(&prepared));
    EXPECT_FALSE(prepared.Valid());
}

TEST(QueueRuntime, OriginalLoansAndScopeAllowActualCommitAndReturnIndependently) {
    harken::QueueInstanceTable table(1);
    const auto create = OriginalQueueCreateRequest();
    harken::PreparedQueueInstanceV1 prepared;
    harken::QueueInstanceTable::OriginalLifetimePin original;
    ASSERT_TRUE(table.PrepareAcquire(create, &prepared, &original));
    harken::QueueInstanceTable::OriginalLifetimePin loan;
    ASSERT_TRUE(original.Borrow(&loan));
    harken::QueueResetTargetV1 target{};
    target.world = create.world;
    target.containerStableId = create.containerStableId;
    target.programSemanticFingerprint = create.programSemanticFingerprint;
    harken::QueueInstanceTable::PreparedCatalogResetScope scope;
    ASSERT_EQ(harken::QueueResetStatusV1::Ok, table.PrepareCatalogResetScope(target, &scope));
    EXPECT_FALSE(table.CancelPreparedAcquire(&prepared));
    harken::QueueInstanceToken token{};
    ASSERT_TRUE(table.CommitPreparedAcquire(&prepared, &token));
    EXPECT_TRUE(original.Valid());
    EXPECT_TRUE(loan.Valid());
    EXPECT_FALSE(table.Release(token));
    scope.Release();
    EXPECT_EQ(2u, harken::QueueInstanceTableTestAccess::OriginalLifetime(table, token).second);
    EXPECT_FALSE(table.Release(token));
    original.Release();
    EXPECT_FALSE(table.Release(token));
    loan.Release();
    EXPECT_TRUE(table.Release(token));
}

TEST(QueueRuntime, OriginalLoansAllowOldConditionalProgressAndRepeatedResetScopes) {
    harken::QueueInstanceTable table(1);
    const auto create = OriginalQueueCreateRequest();
    harken::QueueInstanceTable::OriginalLifetimePin original;
    harken::QueueInstanceToken token{};
    ASSERT_TRUE(table.Acquire(create, &token, &original));
    const auto originalIdentity = harken::QueueInstanceTableTestAccess::OriginalLifetime(table, token);
    const auto request = LogicalBoundaryRequest();
    harken::ConditionalQueueVersionToken conditional{};
    ASSERT_TRUE(table.PrepareConditional(token, request, &conditional));
    ASSERT_TRUE(table.ActivateConditional(token, conditional, request.rootDecision,
        request.expectedCurrentOccurrence));
    ASSERT_TRUE(table.SettleActivated(token, conditional));
    harken::QueueResetTargetV1 target{};
    target.world = create.world;
    target.containerStableId = create.containerStableId;
    target.programSemanticFingerprint = create.programSemanticFingerprint;
    for (std::uint64_t round = 0; round < 2; ++round) {
        harken::QueueInstanceTable::PreparedCatalogResetScope scope;
        ASSERT_EQ(harken::QueueResetStatusV1::Ok, table.PrepareCatalogResetScope(target, &scope));
        EXPECT_EQ(1u, scope.MatchedCount());
        harken::PreparedQueueResetSetV1 reset;
        ASSERT_EQ(harken::QueueResetStatusV1::Ok, table.PrepareReset(target, &reset));
        ASSERT_EQ(harken::QueueResetStatusV1::Ok, table.CommitPreparedReset(&reset, 61u + round));
        table.ApplyDue(round);
        harken::QueueInstanceSnapshot pending{};
        ASSERT_TRUE(table.Capture(token, &pending));
        ASSERT_TRUE(pending.resetPending);
        harken::EffectiveQueueState current{};
        harken::EffectiveQueueState resetNext{};
        bool hasReset = false;
        ASSERT_TRUE(table.ResolveNextSelection(token, &current, &resetNext, &hasReset));
        ASSERT_TRUE(hasReset);
        EXPECT_EQ(3u + round, resetNext.version);
        auto consume = LogicalBoundaryRequest();
        consume.expectedCurrentVersion = current.version;
        consume.next = resetNext;
        harken::ConditionalQueueVersionToken consumed{};
        ASSERT_TRUE(table.PrepareConditional(token, consume, &consumed));
        ASSERT_TRUE(table.ActivateConditional(token, consumed, consume.rootDecision,
            consume.expectedCurrentOccurrence));
        ASSERT_TRUE(table.SettleActivated(token, consumed));
        harken::QueueInstanceSnapshot applied{};
        ASSERT_TRUE(table.Capture(token, &applied));
        EXPECT_FALSE(applied.resetPending);
        EXPECT_EQ(resetNext.version, applied.logicMirror.version);
        EXPECT_EQ(resetNext.currentChildOrdinal, applied.logicMirror.currentChildOrdinal);
        EXPECT_EQ(resetNext.cursor, applied.logicMirror.cursor);
        EXPECT_EQ(resetNext.cycleOrdinal, applied.logicMirror.cycleOrdinal);
        scope.Release();
        EXPECT_EQ(originalIdentity, harken::QueueInstanceTableTestAccess::OriginalLifetime(table, token));
        EXPECT_TRUE(original.Valid());
    }
    harken::QueueInstanceSnapshot snapshot{};
    ASSERT_TRUE(table.Capture(token, &snapshot));
    EXPECT_FALSE(snapshot.resetPending);
    EXPECT_FALSE(table.Release(token));
    original.Release();
    EXPECT_TRUE(table.Release(token));
}

TEST(QueueRuntime, OriginalForeignReceiverAndNonemptyOutputsPreserveOwners) {
    using Pin = harken::QueueInstanceTable::OriginalLifetimePin;
    harken::QueueInstanceTable first(2);
    harken::QueueInstanceTable second(1);
    Pin firstPin;
    Pin secondPin;
    harken::QueueInstanceToken firstToken{};
    harken::QueueInstanceToken secondToken{};
    const auto create = OriginalQueueCreateRequest();
    ASSERT_TRUE(first.Acquire(create, &firstToken, &firstPin));
    ASSERT_TRUE(second.Acquire(create, &secondToken, &secondPin));
    ASSERT_EQ(firstToken.slot, secondToken.slot);
    ASSERT_EQ(firstToken.generation, secondToken.generation);
    EXPECT_FALSE(first.IsOriginalLifetimePinFor(secondPin, firstToken));
    EXPECT_FALSE(second.IsOriginalLifetimePinFor(firstPin, secondToken));
    EXPECT_FALSE(first.IsOriginalLifetimePinFor(firstPin, {firstToken.slot, firstToken.generation + 1u}));
    EXPECT_FALSE(firstPin.Borrow(&firstPin));
    EXPECT_FALSE(firstPin.Borrow(&secondPin));
    EXPECT_FALSE(firstPin.Borrow(nullptr));
    harken::PreparedQueueInstanceV1 refused;
    EXPECT_FALSE(first.PrepareAcquire(create, &refused, &secondPin));
    EXPECT_FALSE(refused.Valid());
    harken::QueueInstanceToken unchanged{27, 31};
    EXPECT_FALSE(first.Acquire(create, &unchanged, &firstPin));
    EXPECT_EQ(27u, unchanged.slot);
    EXPECT_EQ(31u, unchanged.generation);
    EXPECT_EQ(1u, first.LiveCount());
    EXPECT_EQ(1u, harken::QueueInstanceTableTestAccess::OriginalLifetime(first, firstToken).second);
    EXPECT_EQ(1u, harken::QueueInstanceTableTestAccess::OriginalLifetime(second, secondToken).second);
    firstPin.Release();
    secondPin.Release();
    EXPECT_TRUE(first.Release(firstToken));
    EXPECT_TRUE(second.Release(secondToken));
}

TEST(QueueRuntime, OriginalMoveOverwriteAndDestructorsReturnExactPriorReceivers) {
    using Pin = harken::QueueInstanceTable::OriginalLifetimePin;
    harken::QueueInstanceTable first(1);
    harken::QueueInstanceTable second(1);
    harken::QueueInstanceToken firstToken{};
    harken::QueueInstanceToken secondToken{};
    Pin independent;
    {
        Pin firstPin;
        Pin secondPin;
        ASSERT_TRUE(first.Acquire(OriginalQueueCreateRequest(), &firstToken, &firstPin));
        ASSERT_TRUE(second.Acquire(OriginalQueueCreateRequest(), &secondToken, &secondPin));
        ASSERT_TRUE(firstPin.Borrow(&independent));
        firstPin = std::move(secondPin);
        EXPECT_FALSE(secondPin.Valid());
        EXPECT_TRUE(second.IsOriginalLifetimePinFor(firstPin, secondToken));
        EXPECT_EQ(1u, harken::QueueInstanceTableTestAccess::OriginalLifetime(first, firstToken).second);
        EXPECT_FALSE(first.Release(firstToken));
        Pin moved(std::move(firstPin));
        EXPECT_FALSE(firstPin.Valid());
        EXPECT_TRUE(moved.Valid());
    }
    EXPECT_TRUE(second.Release(secondToken));
    EXPECT_FALSE(first.Release(firstToken));
    independent.Release();
    EXPECT_TRUE(first.Release(firstToken));
}

TEST(QueueRuntime, OriginalCancelledNativeTokenReissueGetsNewFactoryEpisode) {
    harken::QueueInstanceTable table(1);
    harken::PreparedQueueInstanceV1 prepared;
    harken::QueueInstanceToken firstToken{};
    std::uint64_t firstMarker = 0;
    {
        harken::QueueInstanceTable::OriginalLifetimePin pin;
        ASSERT_TRUE(table.PrepareAcquire(OriginalQueueCreateRequest(), &prepared, &pin));
        firstToken = prepared.Token();
        firstMarker = harken::QueueInstanceTableTestAccess::OriginalLifetime(table, firstToken).first;
        EXPECT_FALSE(table.CancelPreparedAcquire(&prepared));
    }
    ASSERT_TRUE(table.CancelPreparedAcquire(&prepared));
    harken::QueueInstanceTable::OriginalLifetimePin nextPin;
    ASSERT_TRUE(table.PrepareAcquire(OriginalQueueCreateRequest(), &prepared, &nextPin));
    const auto nextToken = prepared.Token();
    EXPECT_EQ(firstToken.slot, nextToken.slot);
    EXPECT_EQ(firstToken.generation, nextToken.generation);
    EXPECT_NE(firstMarker, harken::QueueInstanceTableTestAccess::OriginalLifetime(table, nextToken).first);
    nextPin.Release();
    EXPECT_FALSE(nextPin.Borrow(&nextPin));
    EXPECT_TRUE(table.CancelPreparedAcquire(&prepared));
}

TEST(QueueRuntime, OriginalLastFactoryMarkerAndCountLimitKeepGenuineLoansReturnable) {
    using Pin = harken::QueueInstanceTable::OriginalLifetimePin;
    harken::QueueInstanceTable table(1);
    harken::QueueInstanceTableTestAccess::SetOriginalLifetimeMarkerToLastIssuable(table);
    Pin original;
    harken::QueueInstanceToken token{};
    ASSERT_TRUE(table.Acquire(OriginalQueueCreateRequest(), &token, &original));
    EXPECT_EQ(UINT64_MAX, harken::QueueInstanceTableTestAccess::OriginalLifetime(table, token).first);
    harken::QueueInstanceTableTestAccess::SetOriginalLifetimeCount(table, token, UINT32_MAX);
    Pin refused;
    EXPECT_FALSE(original.Borrow(&refused));
    EXPECT_FALSE(refused.Valid());
    EXPECT_TRUE(original.Valid());
    EXPECT_EQ(UINT32_MAX, harken::QueueInstanceTableTestAccess::OriginalLifetime(table, token).second);
    harken::QueueInstanceTableTestAccess::SetOriginalLifetimeCount(table, token, 1);
    ASSERT_TRUE(original.Borrow(&refused));
    original.Release();
    EXPECT_TRUE(refused.Valid());
    refused.Release();
    ASSERT_TRUE(table.Release(token));
    harken::QueueInstanceToken unchanged{27, 31};
    EXPECT_FALSE(table.Acquire(OriginalQueueCreateRequest(), &unchanged, &original));
    EXPECT_EQ(27u, unchanged.slot);
    EXPECT_EQ(31u, unchanged.generation);
    harken::PreparedQueueInstanceV1 prepared;
    EXPECT_FALSE(table.PrepareAcquire(OriginalQueueCreateRequest(), &prepared, &original));
    EXPECT_FALSE(prepared.Valid());
    EXPECT_EQ(0u, table.LiveCount());
    EXPECT_TRUE(table.Acquire(OriginalQueueCreateRequest(), &token));
    EXPECT_TRUE(table.Release(token));
}

TEST(QueueRuntime, OriginalPinsPreserveLastNativeGenerationUntilActualQuarantine) {
    harken::QueueInstanceTable table(1);
    harken::QueueInstanceTableTestAccess::SetFreeNativeGenerationToMax(table);
    harken::QueueInstanceTable::OriginalLifetimePin original;
    harken::QueueInstanceToken token{};
    ASSERT_TRUE(table.Acquire(OriginalQueueCreateRequest(), &token, &original));
    EXPECT_EQ(UINT32_MAX, token.generation);
    const auto before = harken::QueueInstanceTableTestAccess::Snapshot(table, token);
    EXPECT_FALSE(table.Release(token));
    EXPECT_EQ(0u, table.QuarantinedCount());
    EXPECT_EQ(before, harken::QueueInstanceTableTestAccess::Snapshot(table, token));
    EXPECT_TRUE(original.Valid());
    original.Release();
    EXPECT_FALSE(table.Release(token));
    EXPECT_EQ(1u, table.QuarantinedCount());
    harken::QueueInstanceToken next{};
    EXPECT_FALSE(table.Acquire(OriginalQueueCreateRequest(), &next));
}

TEST(QueueRuntime, LogicalBoundaryConditionalProvesExactTimelineFrame) {
    harken::OccurrenceStatePool occurrences(1);
    harken::QueueInstanceTable table(1);
    const auto queue = AcquireLogicalBoundaryQueue(table);
    auto request = LogicalBoundaryRequest();
    harken::ConditionalQueueVersionToken conditional{};
    ASSERT_TRUE(table.PrepareConditional(queue, request, &conditional));

    harken::QueueConditionalProof proof{};
    EXPECT_EQ(harken::QueueConditionalProofStatus::Waiting,
              table.ProveConditionalForQuantum(
                  queue, conditional, occurrences, 128, 256, 1, true,
                  &proof));
    ASSERT_EQ(harken::QueueConditionalProofStatus::ReadyToActivate,
              table.ProveConditionalForQuantum(
                  queue, conditional, occurrences, 256, 384, 1, true,
                  &proof));
    EXPECT_EQ(harken::QueueConditionalCompletionMode::LogicalBoundary,
              proof.completionMode);
    EXPECT_EQ(44u, proof.completion.activationOffset);
    ASSERT_TRUE(table.ActivateProvenConditional(proof));
    ASSERT_TRUE(table.SettleActivated(queue, conditional));
    harken::EffectiveQueueState effective{};
    ASSERT_TRUE(table.ResolveEffective(queue, &effective));
    EXPECT_EQ(2u, effective.version);
    EXPECT_EQ(5u, effective.currentChildOrdinal);
    EXPECT_TRUE(table.Release(queue));
}

TEST(QueueProgramBank, OriginalStorageLoansKeepActualDataAndCancelAliasUntilReturn) {
    using Bank = harken::EngineQueueProgramBank;
    static_assert(!std::is_copy_constructible_v<Bank>);
    static_assert(!std::is_move_constructible_v<Bank>);
    static_assert(!std::is_constructible_v<Bank, const harken::EngineQueueProgramBankBody&>);
    static_assert(!std::is_copy_constructible_v<Bank::OriginalReservation>);
    static_assert(!std::is_copy_constructible_v<Bank::OriginalLifetimePin>);
    harken::EngineQueueScheduleStorage storage;
    ASSERT_TRUE(storage.Configure(2));
    std::vector<Bank> banks(2);
    auto& bank = banks[0];
    Bank::OriginalReservation original;
    ASSERT_TRUE(bank.TryReserve(&original));
    ASSERT_TRUE(storage.Acquire(bank, {1, 1}, &bank.scheduleStorageLoan));
    ASSERT_TRUE(original.Valid());
    const auto generation = original.Generation();
    bank.programCount = 1; bank.eventCount = 1;
    bank.payloads[0].kind = harken::TimelineEventKind::Start;
    bank.groups[0] = {3, 5};
    const auto body = std::make_unique<harken::EngineQueueProgramBankBody>(bank);
    auto* alias = &bank;
    EXPECT_FALSE(bank.TryCancelAlias(alias));
    EXPECT_EQ(&bank, alias);
    Bank::OriginalLifetimePin pin, borrowed;
    ASSERT_TRUE(bank.IssueLifetimePin(original, &pin));
    ASSERT_TRUE(pin.Borrow(&borrowed));
    original.Release();
    EXPECT_FALSE(original.Valid());
    EXPECT_TRUE(pin.Valid());
    EXPECT_TRUE(borrowed.Valid());
    EXPECT_EQ(generation, pin.Generation());
    EXPECT_FALSE(bank.TryCancelAlias(alias));
    pin.Release();
    EXPECT_FALSE(bank.TryResetStorage());
    EXPECT_TRUE(bank.scheduleStorageLoan.Valid());
    EXPECT_EQ(body->programCount, bank.programCount);
    EXPECT_EQ(body->eventCount, bank.eventCount);
    EXPECT_EQ(body->payloads[0].kind, bank.payloads[0].kind);
    EXPECT_EQ(body->groups[0].slot, bank.groups[0].slot);
    EXPECT_EQ(body->groups[0].generation, bank.groups[0].generation);
    borrowed.Release();
    EXPECT_TRUE(bank.TryCancelAlias(alias));
    EXPECT_FALSE(bank.scheduleStorageLoan.Valid());
    EXPECT_EQ(nullptr, alias);
    EXPECT_FALSE(bank.occupied);
    EXPECT_EQ(generation, bank.Generation());
    Bank::OriginalReservation next;
    ASSERT_TRUE(bank.TryReserve(&next));
    EXPECT_NE(generation, next.Generation());
    EXPECT_FALSE(bank.IssueLifetimePin(original, &pin));
    next.Release();
    EXPECT_TRUE(bank.TryResetStorage());
}

TEST(QueueProgramBank, ForeignReceiverAndOccupiedOutputsPreserveOriginalOwners) {
    using Bank = harken::EngineQueueProgramBank;
    std::vector<Bank> banks(2);
    Bank::OriginalReservation first, second;
    ASSERT_TRUE(banks[0].TryReserve(&first));
    ASSERT_TRUE(banks[1].TryReserve(&second));
    Bank::OriginalLifetimePin pin, empty;
    ASSERT_TRUE(banks[0].IssueLifetimePin(first, &pin));
    EXPECT_FALSE(banks[1].IssueLifetimePin(first, &empty));
    EXPECT_FALSE(empty.Valid());
    EXPECT_FALSE(banks[1].IssueLifetimePin(second, &pin));
    EXPECT_FALSE(pin.Borrow(&pin));
    EXPECT_TRUE(first.Valid()); EXPECT_TRUE(second.Valid()); EXPECT_TRUE(pin.Valid());
    auto* foreign = &banks[1];
    EXPECT_FALSE(banks[0].TryCancelAlias(foreign));
    EXPECT_EQ(&banks[1], foreign);
    const auto firstGeneration = banks[0].Generation();
    first.Release(); pin.Release();
    ASSERT_TRUE(banks[0].TryResetStorage());
    EXPECT_FALSE(banks[0].TryReserve(&second));
    EXPECT_FALSE(banks[0].occupied);
    EXPECT_EQ(firstGeneration, banks[0].Generation());
    second.Release();
    EXPECT_TRUE(banks[1].TryResetStorage());
}

TEST(QueueProgramBank, MoveOverwriteReturnsOnlyEachGenuinePreviousReceiver) {
    using Bank = harken::EngineQueueProgramBank;
    std::vector<Bank> banks(2);
    Bank::OriginalReservation first, second;
    Bank::OriginalLifetimePin firstPin, secondPin;
    ASSERT_TRUE(banks[0].TryReserve(&first));
    ASSERT_TRUE(banks[1].TryReserve(&second));
    ASSERT_TRUE(banks[0].IssueLifetimePin(first, &firstPin));
    ASSERT_TRUE(banks[1].IssueLifetimePin(second, &secondPin));
    first = std::move(second);
    EXPECT_TRUE(first.Valid()); EXPECT_FALSE(second.Valid());
    EXPECT_FALSE(banks[0].TryResetStorage());
    firstPin = std::move(secondPin);
    EXPECT_TRUE(firstPin.Valid()); EXPECT_FALSE(secondPin.Valid());
    EXPECT_TRUE(banks[0].TryResetStorage());
    EXPECT_FALSE(banks[1].TryResetStorage());
    first.Release();
    EXPECT_TRUE(firstPin.Valid());
    firstPin.Release();
    EXPECT_TRUE(banks[1].TryResetStorage());
}

TEST(QueueProgramBank, NonemptyDestructorsReturnReservationAndIndependentLoan) {
    using Bank = harken::EngineQueueProgramBank;
    auto bank = std::make_unique<Bank>();
    Bank::OriginalLifetimePin retained;
    {
        Bank::OriginalReservation original;
        ASSERT_TRUE(bank->TryReserve(&original));
        Bank::OriginalLifetimePin local;
        ASSERT_TRUE(bank->IssueLifetimePin(original, &local));
        ASSERT_TRUE(local.Borrow(&retained));
    }
    EXPECT_TRUE(retained.Valid());
    EXPECT_FALSE(bank->TryResetStorage());
    retained.Release(); retained.Release();
    EXPECT_TRUE(bank->TryResetStorage());
    {
        Bank::OriginalReservation original;
        ASSERT_TRUE(bank->TryReserve(&original));
    }
    EXPECT_TRUE(bank->TryResetStorage());
}

TEST(QueueProgramBank, LastActualGenerationReturnsWithoutWrappingOrReissuing) {
    using Bank = harken::EngineQueueProgramBank;
    auto bank = std::make_unique<Bank>();
    harken::EngineQueueProgramBankTestAccess::SetGenerationToLastReservable(*bank);
    Bank::OriginalReservation original;
    ASSERT_TRUE(bank->TryReserve(&original));
    ASSERT_EQ(UINT64_MAX, original.Generation());
    Bank::OriginalLifetimePin pin;
    ASSERT_TRUE(bank->IssueLifetimePin(original, &pin));
    original.Release();
    EXPECT_FALSE(bank->TryResetStorage());
    pin.Release();
    ASSERT_TRUE(bank->TryResetStorage());
    EXPECT_FALSE(bank->TryReserve(&original));
    EXPECT_FALSE(bank->TryReserve());
    EXPECT_FALSE(original.Valid());
    EXPECT_FALSE(bank->occupied);
    EXPECT_EQ(UINT64_MAX, bank->Generation());
}

TEST(QueueProgramBank, LastAuthenticLoanMarkerKeepsIssuedLoanAndRefusesAnother) {
    using Bank = harken::EngineQueueProgramBank;
    auto bank = std::make_unique<Bank>();
    Bank::OriginalReservation original;
    ASSERT_TRUE(bank->TryReserve(&original));
    harken::EngineQueueProgramBankTestAccess::SetLifetimeMarkerToLastIssuable(*bank);
    Bank::OriginalLifetimePin last, refused;
    ASSERT_TRUE(bank->IssueLifetimePin(original, &last));
    EXPECT_FALSE(last.Borrow(&refused));
    EXPECT_FALSE(bank->IssueLifetimePin(original, &refused));
    EXPECT_FALSE(refused.Valid());
    EXPECT_TRUE(last.Valid());
    EXPECT_TRUE(original.Valid());
    original.Release();
    EXPECT_FALSE(bank->TryResetStorage());
    last.Release();
    EXPECT_TRUE(bank->TryResetStorage());
}

TEST(QueueRuntime, CatalogScopePinsAllActualMatchedRowsWithoutChangingOldState) {
    harken::QueueInstanceTable table(3);
    const auto first = AcquireLogicalBoundaryQueue(table, 11);
    const auto second = AcquireLogicalBoundaryQueue(table, 12);
    harken::QueueInstanceCreateRequest other{};
    other.world = 7; other.rootInstance = 15;
    other.containerStableId = harkentest::ObjectId(99);
    other.programSemanticFingerprint = 17; other.childCount = 2;
    other.initial.version = 1; other.initial.currentChildOrdinal = 3;
    harken::QueueInstanceToken unmatched{};
    ASSERT_TRUE(table.Acquire(other, &unmatched));
    harken::QueueResetTargetV1 target{};
    target.world = 7; target.containerStableId = harkentest::ObjectId(13);
    target.programSemanticFingerprint = 17;
    const auto beforeFirst = harken::QueueInstanceTableTestAccess::Snapshot(table, first);
    const auto beforeSecond = harken::QueueInstanceTableTestAccess::Snapshot(table, second);
    harken::QueueInstanceTable::PreparedCatalogResetScope original;
    ASSERT_EQ(harken::QueueResetStatusV1::Ok, table.PrepareCatalogResetScope(target, &original));
    ASSERT_TRUE(original.Valid());
    ASSERT_EQ(2u, original.MatchedCount());
    for (std::uint32_t index = 0; index < 2; ++index) {
        harken::QueueInstanceToken token{};
        harken::QueueInstanceSnapshot snapshot{};
        ASSERT_TRUE(table.CaptureCatalogResetScopeQueue(original, index, &token, &snapshot));
        EXPECT_EQ(index == 0 ? first.slot : second.slot, token.slot);
        EXPECT_EQ(index == 0 ? first.generation : second.generation, token.generation);
        EXPECT_EQ(index == 0 ? 11u : 12u, snapshot.rootInstance);
    }
    EXPECT_EQ(beforeFirst, harken::QueueInstanceTableTestAccess::Snapshot(table, first));
    EXPECT_EQ(beforeSecond, harken::QueueInstanceTableTestAccess::Snapshot(table, second));
    EXPECT_FALSE(table.Release(first));
    EXPECT_FALSE(table.Release(second));
    EXPECT_TRUE(table.Release(unmatched));
    original.Release();
    original.Release();
    EXPECT_TRUE(table.Release(first));
    EXPECT_TRUE(table.Release(second));
    const auto reused = AcquireLogicalBoundaryQueue(table, 21);
    EXPECT_EQ(first.slot, reused.slot);
    EXPECT_NE(first.generation, reused.generation);
}

TEST(QueueRuntime, CatalogScopeRetainsActualInitializingRowAndAllowsOriginalCommit) {
    harken::QueueInstanceTable table(1);
    harken::QueueInstanceCreateRequest create{};
    create.world = 7; create.rootInstance = 11;
    create.containerStableId = harkentest::ObjectId(13);
    create.programSemanticFingerprint = 17; create.childCount = 2;
    create.initial.version = 1; create.initial.currentChildOrdinal = 3;
    harken::PreparedQueueInstanceV1 acquire{};
    ASSERT_TRUE(table.PrepareAcquire(create, &acquire));
    const auto original = acquire.Token();
    const auto before = harken::QueueInstanceTableTestAccess::Snapshot(table, original);
    harken::QueueResetTargetV1 target{};
    target.world = create.world; target.containerStableId = create.containerStableId;
    target.programSemanticFingerprint = create.programSemanticFingerprint;
    harken::QueueInstanceTable::PreparedCatalogResetScope scope;
    ASSERT_EQ(harken::QueueResetStatusV1::Ok, table.PrepareCatalogResetScope(target, &scope));
    ASSERT_EQ(1u, scope.MatchedCount());
    harken::QueueInstanceToken captured{};
    harken::QueueInstanceSnapshot snapshot{};
    ASSERT_TRUE(table.CaptureCatalogResetScopeQueue(scope, 0, &captured, &snapshot));
    EXPECT_EQ(original.slot, captured.slot);
    EXPECT_EQ(original.generation, captured.generation);
    EXPECT_EQ(create.rootInstance, snapshot.rootInstance);
    EXPECT_FALSE(table.Capture(original, &snapshot));
    EXPECT_FALSE(table.CancelPreparedAcquire(&acquire));
    EXPECT_TRUE(acquire.Valid());
    EXPECT_EQ(before, harken::QueueInstanceTableTestAccess::Snapshot(table, original));
    harken::QueueInstanceToken committed{};
    ASSERT_TRUE(table.CommitPreparedAcquire(&acquire, &committed));
    EXPECT_FALSE(acquire.Valid());
    EXPECT_EQ(original.slot, committed.slot);
    EXPECT_EQ(original.generation, committed.generation);
    ASSERT_TRUE(table.Capture(committed, &snapshot));
    EXPECT_FALSE(table.Release(committed));
    scope.Release();
    EXPECT_TRUE(table.Release(committed));
}

TEST(QueueRuntime, CatalogScopeDestructorReturnsUncommittedOriginalRowLifetime) {
    harken::QueueInstanceTable table(1);
    harken::QueueInstanceCreateRequest create{};
    create.world = 7; create.rootInstance = 11;
    create.containerStableId = harkentest::ObjectId(13);
    create.programSemanticFingerprint = 17; create.childCount = 2;
    create.initial.version = 1; create.initial.currentChildOrdinal = 3;
    harken::PreparedQueueInstanceV1 acquire{};
    ASSERT_TRUE(table.PrepareAcquire(create, &acquire));
    {
        harken::QueueResetTargetV1 target{};
        target.world = create.world; target.containerStableId = create.containerStableId;
        target.programSemanticFingerprint = create.programSemanticFingerprint;
        harken::QueueInstanceTable::PreparedCatalogResetScope scope;
        ASSERT_EQ(harken::QueueResetStatusV1::Ok, table.PrepareCatalogResetScope(target, &scope));
        ASSERT_EQ(1u, scope.MatchedCount());
        EXPECT_FALSE(table.CancelPreparedAcquire(&acquire));
    }
    EXPECT_TRUE(table.CancelPreparedAcquire(&acquire));
    EXPECT_FALSE(acquire.Valid());
    harken::PreparedQueueInstanceV1 next{};
    ASSERT_TRUE(table.PrepareAcquire(create, &next));
    EXPECT_TRUE(table.CancelPreparedAcquire(&next));
}

TEST(QueueRuntime, CatalogScopeExtensionKeepsUnionAndRefusesForeignReceiver) {
    harken::QueueInstanceTable firstTable(2);
    harken::QueueInstanceTable secondTable(1);
    const auto first = AcquireLogicalBoundaryQueue(firstTable);
    harken::QueueInstanceCreateRequest other{};
    other.world = 7; other.rootInstance = 15;
    other.containerStableId = harkentest::ObjectId(99);
    other.programSemanticFingerprint = 17; other.childCount = 2;
    other.initial.version = 1; other.initial.currentChildOrdinal = 3;
    harken::QueueInstanceToken second{};
    ASSERT_TRUE(firstTable.Acquire(other, &second));
    harken::QueueResetTargetV1 target{};
    target.world = 7; target.containerStableId = harkentest::ObjectId(13);
    target.programSemanticFingerprint = 17;
    harken::QueueInstanceTable::PreparedCatalogResetScope scope;
    ASSERT_EQ(harken::QueueResetStatusV1::Ok, firstTable.PrepareCatalogResetScope(target, &scope));
    EXPECT_EQ(1u, scope.MatchedCount());
    EXPECT_EQ(harken::QueueResetStatusV1::Ok, firstTable.ExtendCatalogResetScope(scope, target));
    EXPECT_EQ(1u, scope.MatchedCount());
    target.containerStableId = other.containerStableId;
    EXPECT_EQ(harken::QueueResetStatusV1::InvalidInput, secondTable.ExtendCatalogResetScope(scope, target));
    ASSERT_EQ(harken::QueueResetStatusV1::Ok, firstTable.ExtendCatalogResetScope(scope, target));
    EXPECT_EQ(2u, scope.MatchedCount());
    auto moved = std::move(scope);
    EXPECT_FALSE(scope.Valid());
    EXPECT_TRUE(moved.Valid());
    EXPECT_FALSE(firstTable.Release(first));
    EXPECT_FALSE(firstTable.Release(second));
    harken::QueueInstanceTable::PreparedCatalogResetScope refused;
    EXPECT_EQ(harken::QueueResetStatusV1::Backpressure,
              firstTable.PrepareCatalogResetScope(target, &refused));
    EXPECT_FALSE(refused.Valid());
    moved.Release();
    EXPECT_TRUE(firstTable.Release(first));
    EXPECT_TRUE(firstTable.Release(second));
}

TEST(QueueRuntime, CatalogScopeOverwriteReturnsOnlyItsGenuinePreviousTable) {
    harken::QueueInstanceTable firstTable(1);
    harken::QueueInstanceTable secondTable(1);
    const auto first = AcquireLogicalBoundaryQueue(firstTable);
    const auto second = AcquireLogicalBoundaryQueue(secondTable);
    harken::QueueResetTargetV1 target{};
    target.world = 7; target.containerStableId = harkentest::ObjectId(13);
    target.programSemanticFingerprint = 17;
    harken::QueueInstanceTable::PreparedCatalogResetScope firstScope;
    harken::QueueInstanceTable::PreparedCatalogResetScope secondScope;
    ASSERT_EQ(harken::QueueResetStatusV1::Ok, firstTable.PrepareCatalogResetScope(target, &firstScope));
    ASSERT_EQ(harken::QueueResetStatusV1::Ok, secondTable.PrepareCatalogResetScope(target, &secondScope));
    firstScope = std::move(secondScope);
    EXPECT_FALSE(secondScope.Valid());
    EXPECT_TRUE(firstScope.Valid());
    EXPECT_TRUE(firstTable.Release(first));
    EXPECT_FALSE(secondTable.Release(second));
    firstScope.Release();
    EXPECT_TRUE(secondTable.Release(second));
}

TEST(QueueRuntime, CatalogScopeAllowsActualOldConditionalAdvanceAndDoesNotRevertOnAbort) {
    harken::QueueInstanceTable table(1);
    const auto queue = AcquireLogicalBoundaryQueue(table);
    auto request = LogicalBoundaryRequest();
    harken::ConditionalQueueVersionToken conditional{};
    ASSERT_TRUE(table.PrepareConditional(queue, request, &conditional));
    harken::QueueResetTargetV1 target{};
    target.world = 7; target.containerStableId = harkentest::ObjectId(13);
    target.programSemanticFingerprint = 17;
    const auto before = harken::QueueInstanceTableTestAccess::Snapshot(table, queue);
    harken::QueueInstanceTable::PreparedCatalogResetScope scope;
    ASSERT_EQ(harken::QueueResetStatusV1::Ok, table.PrepareCatalogResetScope(target, &scope));
    EXPECT_EQ(before, harken::QueueInstanceTableTestAccess::Snapshot(table, queue));
    ASSERT_TRUE(table.ActivateConditional(queue, conditional, request.rootDecision,
        request.expectedCurrentOccurrence));
    ASSERT_TRUE(table.SettleActivated(queue, conditional));
    harken::QueueInstanceToken captured{};
    harken::QueueInstanceSnapshot after{};
    ASSERT_TRUE(table.CaptureCatalogResetScopeQueue(scope, 0, &captured, &after));
    EXPECT_EQ(queue.slot, captured.slot);
    EXPECT_EQ(2u, after.logicMirror.version);
    EXPECT_EQ(5u, after.logicMirror.currentChildOrdinal);
    const auto advanced = harken::QueueInstanceTableTestAccess::Snapshot(table, queue);
    scope.Release();
    EXPECT_EQ(advanced, harken::QueueInstanceTableTestAccess::Snapshot(table, queue));
    EXPECT_TRUE(table.Release(queue));
}

TEST(QueueRuntime, CatalogScopeEmptyInvalidAndMarkerExhaustionDoNotTouchActualRows) {
    harken::QueueInstanceTable table(1);
    const auto queue = AcquireLogicalBoundaryQueue(table);
    const auto before = harken::QueueInstanceTableTestAccess::Snapshot(table, queue);
    harken::QueueResetTargetV1 target{};
    target.world = 7; target.containerStableId = harkentest::ObjectId(99);
    target.programSemanticFingerprint = 17;
    harken::QueueInstanceTable::PreparedCatalogResetScope empty;
    ASSERT_EQ(harken::QueueResetStatusV1::Ok, table.PrepareCatalogResetScope(target, &empty));
    EXPECT_TRUE(empty.Valid());
    EXPECT_EQ(0u, empty.MatchedCount());
    empty.Release();
    target.world = 0;
    EXPECT_EQ(harken::QueueResetStatusV1::InvalidInput, table.PrepareCatalogResetScope(target, &empty));
    EXPECT_FALSE(empty.Valid());
    target.world = 7;
    harken::QueueInstanceTableTestAccess::SetCatalogScopeMarkerLimit(table);
    EXPECT_EQ(harken::QueueResetStatusV1::RuntimeRecreateRequired,
              table.PrepareCatalogResetScope(target, &empty));
    EXPECT_FALSE(empty.Valid());
    EXPECT_EQ(before, harken::QueueInstanceTableTestAccess::Snapshot(table, queue));
    EXPECT_TRUE(table.Release(queue));
}

TEST(QueueRuntime, PrivateConditionalTimingKeepsVersionAndOwnerWhileMovingBoundary) {
    harken::QueueInstanceTable table(1);
    const auto queue = AcquireLogicalBoundaryQueue(table);
    auto request = LogicalBoundaryRequest(300);
    harken::ConditionalQueueVersionToken version{};
    ASSERT_TRUE(table.PrepareConditional(queue, request, &version));
    harken::QueueInstanceTable::PrivateConditionalTimingProof proof{};
    EXPECT_FALSE(table.ProvePrivateConditionalTimingBeforePublication(
        queue, version, {19, 24}, request.expectedCurrentOccurrence,
        0, 500, &proof));
    const auto before = harken::QueueInstanceTableTestAccess::PendingTiming(
        table, queue);
    EXPECT_EQ(300u, before.second);
    ASSERT_TRUE(table.ProvePrivateConditionalTimingBeforePublication(
        queue, version, request.rootDecision,
        request.expectedCurrentOccurrence, 0, 500, &proof));
    table.ApplyPrivateConditionalTimingBeforePublication(proof);
    const auto after = harken::QueueInstanceTableTestAccess::PendingTiming(
        table, queue);
    EXPECT_EQ(500u, after.second);
    EXPECT_EQ(harken::CompletionOwnerKind::None,
              after.first.owner.kind);
    harken::OccurrenceStatePool occurrences(1);
    harken::QueueConditionalProof completion{};
    EXPECT_EQ(harken::QueueConditionalProofStatus::Waiting,
        table.ProveConditionalForQuantum(queue, version, occurrences,
            256, 384, 1, true, &completion));
    EXPECT_EQ(harken::QueueConditionalProofStatus::ReadyToActivate,
        table.ProveConditionalForQuantum(queue, version, occurrences,
            384, 512, 1, true, &completion));
    EXPECT_EQ(116u, completion.completion.activationOffset);
}

static void CheckCompositeCompletionLaterFailure(bool ownerChanged) {
    harken::OccurrenceStatePool occurrences(4);
    harken::OccurrenceToken current{}, successor{};
    std::array<harken::OccurrenceToken, 2> members{};
    ASSERT_TRUE(occurrences.Acquire({}, &current));
    ASSERT_TRUE(occurrences.Acquire({}, &successor));
    harken::CompositeCompletionOwnerPool composites(1, 2);
    const harken::DecisionToken decision{19, 23};
    std::array<harken::KnownScheduleLeafV1, 2> leaves{};
    for (std::uint32_t index = 0; index < members.size(); ++index) {
        ASSERT_TRUE(occurrences.Acquire({}, &members[index]));
        ASSERT_TRUE(occurrences.BindCompletionProgress(members[index], 43 + index, 1));
        auto& leaf = leaves[index];
        leaf.occurrence = members[index];
        leaf.renderGeneration = 43 + index;
        leaf.stableLeafOrdinal = index + 1u;
        leaf.authoredOffset.denominator = 1;
        leaf.predicate = BoundPredicate(decision, leaf.occurrence, leaf.renderGeneration);
        leaf.predicate.owner = {};
    }
    harken::CompositeCompletionOwnerToken owner{};
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok,
        composites.Acquire(current, 1, leaves.data(), leaves.size(), occurrences, &owner));
    harken::KnownScheduleLeafV1 later{};
    ASSERT_TRUE(composites.CaptureLeaf(owner, 1, &later));
    harken::QueueInstanceTable queues(1);
    const auto queue = AcquireLogicalBoundaryQueue(queues);
    auto request = LogicalBoundaryRequest(0);
    request.completionIsLogicalBoundary = false;
    request.expectedCurrentOccurrence = current;
    request.successorOccurrence = successor;
    request.completionOwner = harken::CompletionOwnerRef::Composite(owner, 1);
    harken::ConditionalQueueVersionToken version{};
    ASSERT_TRUE(queues.PrepareConditional(queue, request, &version));
    harken::QueueConditionalProof proof{};
    EXPECT_EQ(harken::QueueConditionalProofStatus::Waiting,
        queues.ProveConditionalForQuantum(queue, version, occurrences, composites,
            0, 64, 0, false, &proof));
    if (ownerChanged) {
        ASSERT_TRUE(occurrences.ClearCompletionOwnerExact(later.occurrence, later.predicate.owner));
    } else {
        harken::CompletionProgressSnapshot changed{};
        changed.occurrence = later.occurrence;
        changed.renderGeneration = later.renderGeneration + 1u;
        changed.playbackFactsRevision = 1;
        ASSERT_TRUE(harken::InitializeCompletionProgress(*occurrences.CompletionProgressOf(later.occurrence), changed));
    }
    EXPECT_EQ(ownerChanged ? harken::QueueConditionalProofStatus::FailedIdentity
                           : harken::QueueConditionalProofStatus::FailedProgress,
        queues.ProveConditionalForQuantum(queue, version, occurrences, composites,
            0, 64, 0, false, &proof));
    EXPECT_FALSE(proof.version.Valid());
}

TEST(QueueRuntime, CompositeCompletionChecksLaterIdentityFailureWhileEarlierLeafWaits) {
    CheckCompositeCompletionLaterFailure(true);
}

TEST(QueueRuntime, CompositeCompletionChecksLaterProgressFailureWhileEarlierLeafWaits) {
    CheckCompositeCompletionLaterFailure(false);
}

TEST(QueueRuntime, PrivateConditionalTimingPreservesSingleCompletionBaseline) {
    harken::QueueInstanceTable table(1);
    const auto queue = AcquireLogicalBoundaryQueue(table);
    auto request = LogicalBoundaryRequest(0);
    request.completionIsLogicalBoundary = false;
    request.completion = BoundPredicate(request.rootDecision,
                                        request.expectedCurrentOccurrence);
    request.completionOwner = request.completion.owner;
    harken::ConditionalQueueVersionToken version{};
    ASSERT_TRUE(table.PrepareConditional(queue, request, &version));
    harken::QueueInstanceTable::PrivateConditionalTimingProof proof{};
    ASSERT_TRUE(table.ProvePrivateConditionalTimingBeforePublication(
        queue, version, request.rootDecision,
        request.expectedCurrentOccurrence, 512, 0, &proof));
    table.ApplyPrivateConditionalTimingBeforePublication(proof);
    const auto after = harken::QueueInstanceTableTestAccess::PendingTiming(
        table, queue);
    EXPECT_EQ(512u, after.first.absoluteLogicalStartFrame);
    EXPECT_EQ(812u, after.first.absoluteLogicalCompleteFrame);
    EXPECT_EQ(request.completion.progressBaselineRevision,
              after.first.progressBaselineRevision);
    EXPECT_EQ(request.completion.progressBaselineCompletedThroughBlockSequence,
              after.first.progressBaselineCompletedThroughBlockSequence);
    EXPECT_TRUE(harken::SameCompletionOwner(
        request.completion.owner, after.first.owner));
}

TEST(QueueRuntime,
     PhysicalCompletionWaitsForLaterLogicalFloorInExactQuantum) {
    harken::OccurrenceStatePool occurrences(1);
    harken::OccurrenceToken occurrence{};
    ASSERT_TRUE(occurrences.Acquire(
        {11, harkentest::ObjectId(47), 0, 3}, &occurrence));
    ASSERT_TRUE(occurrences.BindCompletionProgress(occurrence, 43, 1));
    const harken::DecisionToken decision{19, 23};
    ASSERT_TRUE(occurrences.TryPinCompletionOwner(
        occurrence, harken::CompletionOwnerRef::Single(decision, 1)));
    harken::CompletionProgressBlockUpdate update{};
    update.occurrence = occurrence;
    update.renderGeneration = 43;
    update.sourceOutputFramesThisBlock = 256;
    update.playbackFactsRevision = 1;
    update.completedThroughBlockSequence = 1;
    ASSERT_EQ(harken::CompletionProgressPublishStatus::Ok,
              harken::PublishCompletionProgressBlock(
                  *occurrences.CompletionProgressOf(occurrence), update));

    harken::QueueInstanceTable table(1);
    const auto queue = AcquireLogicalBoundaryQueue(table);
    harken::ConditionalQueuePrepareRequest request{};
    request.expectedCurrentVersion = 1;
    request.rootDecision = decision;
    request.expectedCurrentOccurrence = occurrence;
    request.successorOccurrence = {37, 41};
    request.completion = BoundPredicate(decision, occurrence);
    request.completionHasLogicalFloor = true;
    request.logicalCompletionFrame = 600;
    request.next.version = 2;
    request.next.currentChildOrdinal = 5;
    harken::ConditionalQueueVersionToken conditional{};
    ASSERT_TRUE(table.PrepareConditional(queue, request, &conditional));

    harken::QueueConditionalProof proof{};
    EXPECT_EQ(harken::QueueConditionalProofStatus::Waiting,
              table.ProveConditionalForQuantum(
                  queue, conditional, occurrences, 256, 512, 1, true,
                  &proof));
    update.sourceOutputFramesThisBlock = 44;
    update.completedThroughBlockSequence = 2;
    ASSERT_EQ(harken::CompletionProgressPublishStatus::Ok,
              harken::PublishCompletionProgressBlock(
                  *occurrences.CompletionProgressOf(occurrence), update));
    ASSERT_EQ(harken::QueueConditionalProofStatus::ReadyToActivate,
              table.ProveConditionalForQuantum(
                  queue, conditional, occurrences, 512, 768, 2, true,
                  &proof));
    EXPECT_EQ(harken::QueueConditionalCompletionMode::InQuantum,
              proof.completionMode);
    EXPECT_EQ(88u, proof.completion.activationOffset);
    ASSERT_TRUE(table.ActivateProvenConditional(proof));
    ASSERT_TRUE(table.SettleActivated(queue, conditional));
    harken::EffectiveQueueState effective{};
    ASSERT_TRUE(table.ResolveEffective(queue, &effective));
    EXPECT_EQ(2u, effective.version);
    EXPECT_EQ(5u, effective.currentChildOrdinal);
    EXPECT_TRUE(table.Release(queue));
}

TEST(QueueRuntime,
     RetiredPhysicalCompletionWaitsForLogicalFloorAndKeepsProof) {
    harken::OccurrenceStatePool occurrences(1);
    harken::OccurrenceToken occurrence{};
    ASSERT_TRUE(occurrences.Acquire(
        {11, harkentest::ObjectId(47), 0, 3}, &occurrence));
    ASSERT_TRUE(occurrences.BindCompletionProgress(occurrence, 43, 1));
    const harken::DecisionToken decision{19, 23};
    ASSERT_TRUE(occurrences.TryPinCompletionOwner(
        occurrence, harken::CompletionOwnerRef::Single(decision, 1)));
    harken::CompletionProgressBlockUpdate update{};
    update.occurrence = occurrence;
    update.renderGeneration = 43;
    update.sourceOutputFramesThisBlock = 300;
    update.playbackFactsRevision = 1;
    update.completedThroughBlockSequence = 1;
    ASSERT_EQ(harken::CompletionProgressPublishStatus::Ok,
              harken::PublishCompletionProgressBlock(
                  *occurrences.CompletionProgressOf(occurrence), update));

    harken::QueueInstanceTable table(1);
    const auto queue = AcquireLogicalBoundaryQueue(table);
    harken::ConditionalQueuePrepareRequest request{};
    request.expectedCurrentVersion = 1;
    request.rootDecision = decision;
    request.expectedCurrentOccurrence = occurrence;
    request.successorOccurrence = {37, 41};
    request.completion = BoundPredicate(decision, occurrence);
    request.completionHasLogicalFloor = true;
    request.logicalCompletionFrame = 600;
    request.next.version = 2;
    request.next.currentChildOrdinal = 5;
    request.retiredSource.sourceArm = {7, 11, 9, 43};
    request.retiredSource.sourceRenderHandle =
        (UINT64_C(43) << 32) | 9u;
    request.retiredSource.logicalCompletionSequence = 1;
    request.retiredSource.tombstoneAudioSequence = 1;
    harken::ConditionalQueueVersionToken conditional{};
    ASSERT_TRUE(table.PrepareConditional(queue, request, &conditional));

    harken::QueueConditionalProof proof{};
    EXPECT_EQ(harken::QueueConditionalProofStatus::Waiting,
              table.ProveConditionalForQuantum(
                  queue, conditional, occurrences, 320, 576, 1, true,
                  &proof));
    ASSERT_EQ(
        harken::QueueConditionalProofStatus::ReadyToActivateRetiredSource,
        table.ProveConditionalForQuantum(
            queue, conditional, occurrences, 576, 640, 1, true, &proof));
    EXPECT_EQ(harken::QueueConditionalCompletionMode::RetiredSource,
              proof.completionMode);
    EXPECT_EQ(24u, proof.completion.activationOffset);
    EXPECT_EQ(request.retiredSource.sourceRenderHandle,
              proof.retiredSourceRenderHandle);
    EXPECT_EQ(request.retiredSource.logicalCompletionSequence,
              proof.retiredSourceLogicalCompletionSequence);
    EXPECT_EQ(request.retiredSource.tombstoneAudioSequence,
              proof.retiredSourceTombstoneAudioSequence);
    ASSERT_TRUE(table.ActivateProvenConditional(proof));
    ASSERT_TRUE(table.SettleActivated(queue, conditional));
    EXPECT_TRUE(harken::QueueInstanceTableTestAccess::
                    PendingCompletionIdentityEmpty(table, queue));
    EXPECT_TRUE(table.Release(queue));
}

TEST(QueueRuntime, RetiredPhysicalFloorAbortClearsPendingIdentity) {
    harken::QueueInstanceTable table(1);
    const auto queue = AcquireLogicalBoundaryQueue(table);
    harken::ConditionalQueuePrepareRequest request{};
    request.expectedCurrentVersion = 1;
    request.rootDecision = {19, 23};
    request.expectedCurrentOccurrence = {29, 31};
    request.successorOccurrence = {37, 41};
    request.completion = BoundPredicate(request.rootDecision);
    request.completionHasLogicalFloor = true;
    request.logicalCompletionFrame = 600;
    request.next.version = 2;
    request.next.currentChildOrdinal = 5;
    request.retiredSource.sourceArm = {7, 11, 9, 43};
    request.retiredSource.sourceRenderHandle =
        (UINT64_C(43) << 32) | 9u;
    request.retiredSource.logicalCompletionSequence = 1;
    request.retiredSource.tombstoneAudioSequence = 1;
    harken::ConditionalQueueVersionToken conditional{};
    ASSERT_TRUE(table.PrepareConditional(queue, request, &conditional));
    ASSERT_TRUE(table.RetireConditionalForAbort(
        queue, conditional, request.rootDecision));
    EXPECT_TRUE(harken::QueueInstanceTableTestAccess::
                    PendingCompletionIdentityEmpty(table, queue));
    auto next = LogicalBoundaryRequest(700);
    next.rootDecision = {43, 47};
    harken::ConditionalQueueVersionToken nextVersion{};
    ASSERT_TRUE(table.PrepareConditional(queue, next, &nextVersion));
    ASSERT_TRUE(table.RetireConditionalForAbort(
        queue, nextVersion, next.rootDecision));
    EXPECT_TRUE(table.Release(queue));
}

TEST(QueueRuntime, LogicalBoundaryConditionalRejectsMixedCompletionIdentity) {
    harken::QueueInstanceTable table(1);
    const auto queue = AcquireLogicalBoundaryQueue(table);
    auto request = LogicalBoundaryRequest();
    request.completion = BoundPredicate(request.rootDecision);
    harken::ConditionalQueueVersionToken conditional{};
    EXPECT_FALSE(table.PrepareConditional(queue, request, &conditional));

    request.completion = {};
    request.completionHasLogicalFloor = true;
    EXPECT_FALSE(table.PrepareConditional(queue, request, &conditional));

    request.completionHasLogicalFloor = false;
    request.retiredSource.sourceArm = {7, 11, 9, 43};
    request.retiredSource.sourceRenderHandle =
        (UINT64_C(43) << 32) | 9u;
    request.retiredSource.logicalCompletionSequence = 1;
    request.retiredSource.tombstoneAudioSequence = 1;
    EXPECT_FALSE(table.PrepareConditional(queue, request, &conditional));
    EXPECT_TRUE(table.Release(queue));
}

TEST(QueueRuntime, LogicalBoundaryConditionalMissedQuantumFailsDeadline) {
    harken::OccurrenceStatePool occurrences(1);
    harken::QueueInstanceTable table(1);
    const auto queue = AcquireLogicalBoundaryQueue(table);
    auto request = LogicalBoundaryRequest(255);
    harken::ConditionalQueueVersionToken conditional{};
    ASSERT_TRUE(table.PrepareConditional(queue, request, &conditional));

    EXPECT_EQ(harken::QueueConditionalProofStatus::MissedDeadline,
              table.EvaluateConditionalForQuantum(
                  queue, conditional, occurrences, 256, 384, 1, true));
    harken::QueueInstanceSnapshot snapshot{};
    ASSERT_TRUE(table.Capture(queue, &snapshot));
    EXPECT_EQ(harken::ConditionalQueueActivation::InvalidatedDeadline,
              snapshot.pendingActivation);
    ASSERT_TRUE(table.RetireConditionalForAbort(
        queue, conditional, request.rootDecision));
    EXPECT_TRUE(table.Release(queue));
}

TEST(QueueRuntime,
     PreparedConditionalStaysInvisibleAndCancelRestoresLiveRow) {
    static_assert(!std::is_copy_constructible_v<
                  harken::PreparedQueueConditionalV1>);
    static_assert(!std::is_copy_assignable_v<
                  harken::PreparedQueueConditionalV1>);
    static_assert(std::is_move_constructible_v<
                  harken::PreparedQueueConditionalV1>);

    harken::QueueInstanceTable table(1);
    harken::QueueInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = 11;
    create.containerStableId = harkentest::ObjectId(13);
    create.programSemanticFingerprint = 17;
    create.childCount = 2;
    create.initial.version = 1;
    create.initial.cursor = 0;
    create.initial.currentChildOrdinal = 3;
    harken::QueueInstanceToken queue{};
    ASSERT_TRUE(table.Acquire(create, &queue));

    harken::ConditionalQueuePrepareRequest request{};
    request.expectedCurrentVersion = 1;
    request.rootDecision = {19, 23};
    request.expectedCurrentOccurrence = {29, 31};
    request.successorOccurrence = {37, 41};
    request.completion = BoundPredicate(request.rootDecision);
    request.next.version = 2;
    request.next.cursor = 1;
    request.next.currentChildOrdinal = 5;
    harken::PreparedQueueConditionalV1 prepared{};
    ASSERT_TRUE(table.PrepareConditional(queue, request, &prepared));
    ASSERT_TRUE(prepared.Valid());
    EXPECT_EQ(2u, prepared.Version().version);
    harken::QueueInstanceSnapshot before{};
    ASSERT_TRUE(table.Capture(queue, &before));
    EXPECT_EQ(0u, before.pendingVersion);
    EXPECT_FALSE(table.Release(queue));
    harken::ConditionalQueueVersionToken refused{};
    EXPECT_FALSE(table.PrepareConditional(queue, request, &refused));

    harken::PreparedQueueConditionalV1 moved(std::move(prepared));
    EXPECT_FALSE(prepared.Valid());
    ASSERT_TRUE(table.CancelPreparedConditional(&moved));
    EXPECT_FALSE(moved.Valid());
    EXPECT_FALSE(table.CancelPreparedConditional(&moved));
    harken::QueueInstanceSnapshot cancelled{};
    ASSERT_TRUE(table.Capture(queue, &cancelled));
    EXPECT_EQ(0u, cancelled.pendingVersion);

    harken::ConditionalQueueVersionToken committed{};
    ASSERT_TRUE(table.PrepareConditional(queue, request, &committed));
    ASSERT_TRUE(table.RetireConditionalForAbort(
        queue, committed, request.rootDecision));
    EXPECT_TRUE(table.Release(queue));
}

TEST(QueueRuntime,
     PreparedConditionalOwnsInitializingQueueRowUntilCommitOrCancel) {
    harken::QueueInstanceTable table(1);
    harken::QueueInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = 11;
    create.containerStableId = harkentest::ObjectId(13);
    create.programSemanticFingerprint = 17;
    create.childCount = 2;
    create.initial.version = 1;
    create.initial.cursor = 0;
    create.initial.currentChildOrdinal = 3;
    harken::PreparedQueueInstanceV1 instance{};
    ASSERT_TRUE(table.PrepareAcquire(create, &instance));

    harken::ConditionalQueuePrepareRequest request{};
    request.expectedCurrentVersion = 1;
    request.rootDecision = {19, 23};
    request.expectedCurrentOccurrence = {29, 31};
    request.successorOccurrence = {37, 41};
    request.completion = BoundPredicate(request.rootDecision);
    request.next.version = 2;
    request.next.cursor = 1;
    request.next.currentChildOrdinal = 5;
    harken::PreparedQueueConditionalV1 conditional{};
    ASSERT_TRUE(table.PrepareConditional(
        instance.Token(), request, &conditional));
    EXPECT_FALSE(table.CancelPreparedAcquire(&instance));

    harken::ConditionalQueueVersionToken version{};
    ASSERT_TRUE(table.CommitPreparedConditional(&conditional, &version));
    EXPECT_FALSE(table.CommitPreparedConditional(&conditional, &version));
    harken::QueueInstanceToken queue{};
    ASSERT_TRUE(table.CommitPreparedAcquire(&instance, &queue));
    harken::QueueInstanceSnapshot committed{};
    ASSERT_TRUE(table.Capture(queue, &committed));
    EXPECT_EQ(2u, committed.pendingVersion);
    ASSERT_TRUE(table.RetireConditionalForAbort(
        queue, version, request.rootDecision));
    ASSERT_TRUE(table.Release(queue));

    create.rootInstance = 12;
    harken::PreparedQueueInstanceV1 cancelledInstance{};
    ASSERT_TRUE(table.PrepareAcquire(create, &cancelledInstance));
    harken::PreparedQueueConditionalV1 cancelledConditional{};
    ASSERT_TRUE(table.PrepareConditional(
        cancelledInstance.Token(), request, &cancelledConditional));
    ASSERT_TRUE(table.CancelPreparedConditional(&cancelledConditional));
    ASSERT_TRUE(table.CancelPreparedAcquire(&cancelledInstance));
    EXPECT_EQ(0u, table.LiveCount());
}

TEST(QueueRuntime, InitializingConditionalRollbackRequiresExactOriginalOwnerAndReceipt) {
    harken::QueueInstanceTable table(1);
    harken::QueueInstanceTable foreign(1);
    harken::QueueInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = 11;
    create.containerStableId = harkentest::ObjectId(13);
    create.programSemanticFingerprint = 17;
    create.childCount = 2;
    create.initial.version = 1;
    create.initial.currentChildOrdinal = 3;
    create.initial.selectionOrdinal = 5;
    create.resetInitial = create.initial;
    create.resetEpoch = 23;
    harken::PreparedQueueInstanceV1 instance{};
    ASSERT_TRUE(table.PrepareAcquire(create, &instance));
    const auto queue = instance.Token();
    harken::ConditionalQueuePrepareRequest request{};
    request.expectedCurrentVersion = 1;
    request.rootDecision = {19, 23};
    request.expectedCurrentOccurrence = {29, 31};
    request.successorOccurrence = {37, 41};
    request.completion = BoundPredicate(request.rootDecision);
    request.next.version = 2;
    request.next.cursor = 1;
    request.next.currentChildOrdinal = 5;
    harken::PreparedQueueConditionalV1 conditional{};
    ASSERT_TRUE(table.PrepareConditional(queue, request, &conditional));
    harken::ConditionalQueueVersionToken version{};
    ASSERT_TRUE(table.CommitPreparedConditional(&conditional, &version));
    EXPECT_FALSE(table.CancelPreparedAcquire(&instance));
    EXPECT_FALSE(table.RetireConditionalForAbort(queue, version, request.rootDecision));
    using Access = harken::QueueInstanceTableTestAccess;
    const auto rejected = [&](const auto& operation) {
        const auto before = Access::Snapshot(table, queue);
        EXPECT_FALSE(operation());
        EXPECT_EQ(before, Access::Snapshot(table, queue));
        EXPECT_TRUE(instance.Valid());
        EXPECT_EQ(queue.slot, instance.Token().slot);
        EXPECT_EQ(queue.generation, instance.Token().generation);
    };
    rejected([&] { return foreign.RollbackInitializingConditional(instance, queue, version, request.rootDecision); });
    rejected([&] { return table.RollbackInitializingConditional(instance, {1, queue.generation}, version, request.rootDecision); });
    rejected([&] { return table.RollbackInitializingConditional(instance, {queue.slot, queue.generation + 1u}, version, request.rootDecision); });
    rejected([&] { return table.RollbackInitializingConditional(instance, queue, {version.version + 1u}, request.rootDecision); });
    rejected([&] { return table.RollbackInitializingConditional(instance, queue, version, {request.rootDecision.slot + 1u, request.rootDecision.generation}); });
    rejected([&] { return table.RollbackInitializingConditional(instance, queue, {}, request.rootDecision); });
    rejected([&] { return table.RollbackInitializingConditional(instance, queue, version, {}); });
    for (unsigned state = 0; state < 6; ++state) {
        Access::SetRejectionState(table, queue, state, true);
        rejected([&] { return table.RollbackInitializingConditional(instance, queue, version, request.rootDecision); });
        Access::SetRejectionState(table, queue, state, false);
    }
    harken::PreparedQueueInstanceV1 moved(std::move(instance));
    const auto beforeMoved = Access::Snapshot(table, queue);
    EXPECT_FALSE(table.RollbackInitializingConditional(instance, queue, version, request.rootDecision));
    EXPECT_EQ(beforeMoved, Access::Snapshot(table, queue));
    instance = std::move(moved);
    Access::SeedResetState(table, queue);
    const auto mirrorAndReset = Access::MirrorAndReset(table, queue);
    ASSERT_TRUE(table.RollbackInitializingConditional(instance, queue, version, request.rootDecision));
    EXPECT_EQ(mirrorAndReset, Access::MirrorAndReset(table, queue));
    EXPECT_TRUE(instance.Valid());
    rejected([&] { return table.RollbackInitializingConditional(instance, queue, version, request.rootDecision); });
    ASSERT_TRUE(table.CancelPreparedAcquire(&instance));
    harken::PreparedQueueInstanceV1 reacquired{};
    ASSERT_TRUE(table.PrepareAcquire(create, &reacquired));
    harken::QueueInstanceToken releasedQueue{};
    ASSERT_TRUE(table.CommitPreparedAcquire(&reacquired, &releasedQueue));
    ASSERT_TRUE(table.Release(releasedQueue));
    ASSERT_TRUE(table.PrepareAcquire(create, &reacquired));
    const auto reused = reacquired.Token();
    ASSERT_NE(releasedQueue.generation, reused.generation);
    ASSERT_TRUE(table.PrepareConditional(reused, request, &conditional));
    ASSERT_TRUE(table.CommitPreparedConditional(&conditional, &version));
    const auto beforeStale = Access::Snapshot(table, reused);
    EXPECT_FALSE(table.RollbackInitializingConditional(reacquired, releasedQueue, version, request.rootDecision));
    EXPECT_FALSE(table.RollbackInitializingConditional(instance, reused, version, request.rootDecision));
    EXPECT_EQ(beforeStale, Access::Snapshot(table, reused));
    harken::QueueInstanceToken live{};
    ASSERT_TRUE(table.CommitPreparedAcquire(&reacquired, &live));
    EXPECT_FALSE(table.RollbackInitializingConditional(reacquired, live, version, request.rootDecision));
    ASSERT_TRUE(table.RetireConditionalForAbort(live, version, request.rootDecision));
    ASSERT_TRUE(table.Release(live));
    ASSERT_TRUE(table.PrepareAcquire(create, &reacquired));
    EXPECT_TRUE(table.CancelPreparedAcquire(&reacquired));
}

TEST(QueueRuntime, ActivatedSuccessorIsEffectiveBeforeLogicSettlement) {
    harken::QueueInstanceTable table(1);
    harken::QueueInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = 11;
    create.containerStableId = harkentest::ObjectId(13);
    create.programSemanticFingerprint = 17;
    create.childCount = 2;
    create.initial.cursor = 0;
    create.initial.currentChildOrdinal = 3;
    create.initial.version = 1;

    harken::QueueInstanceToken queue{};
    ASSERT_TRUE(table.Acquire(create, &queue));

    harken::ConditionalQueuePrepareRequest prepare{};
    prepare.expectedCurrentVersion = 1;
    prepare.rootDecision = {19, 23};
    prepare.expectedCurrentOccurrence = {29, 31};
    prepare.successorOccurrence = {37, 41};
    prepare.completion = BoundPredicate(prepare.rootDecision);
    prepare.next.cursor = 1;
    prepare.next.currentChildOrdinal = 5;
    prepare.next.version = 2;
    harken::ConditionalQueueVersionToken conditional{};
    ASSERT_TRUE(table.PrepareConditional(queue, prepare, &conditional));

    harken::EffectiveQueueState state{};
    ASSERT_TRUE(table.ResolveEffective(queue, &state));
    EXPECT_EQ(1u, state.version);
    EXPECT_EQ(0u, state.cursor);
    EXPECT_EQ(3u, state.currentChildOrdinal);

    ASSERT_TRUE(table.ActivateConditional(
        queue, conditional, prepare.rootDecision,
        prepare.expectedCurrentOccurrence));
    ASSERT_TRUE(table.ResolveEffective(queue, &state));
    EXPECT_EQ(2u, state.version);
    EXPECT_EQ(1u, state.cursor);
    EXPECT_EQ(5u, state.currentChildOrdinal);

    ASSERT_TRUE(table.SettleActivated(queue, conditional));
    harken::QueueInstanceSnapshot settled{};
    ASSERT_TRUE(table.Capture(queue, &settled));
    EXPECT_EQ(harken::ConditionalQueueActivation::Waiting,
              settled.pendingActivation);
    EXPECT_EQ(2u, settled.logicMirror.version);
    ASSERT_TRUE(table.ResolveEffective(queue, &state));
    EXPECT_EQ(2u, state.version);
    EXPECT_EQ(1u, state.cursor);
    EXPECT_EQ(5u, state.currentChildOrdinal);
}

TEST(QueueRuntime, ConditionalProjectionMatchesSerialFrontierAndOwnsAllStates) {
    harken::ContainerChildProgramViewV2 children[4]{};
    for (std::uint32_t index = 0; index < 4; ++index) children[index].stableChildOrdinal = 3 + index * 2;
    harken::ContainerProgramViewV3 program{};
    program.mode = harken::ContainerModeV1::SequenceQueue;
    program.children = children;
    program.childCount = 4;
    program.queueCycle = harken::QueueCycleV1::Once;
    harken::QueueInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = 11;
    create.containerStableId = harkentest::ObjectId(13);
    create.programSemanticFingerprint = 17;
    create.childCount = 4;
    create.initial.version = 1;
    create.initial.currentChildOrdinal = 3;
    create.initial.selectionOrdinal = 9;
    harken::QueueInstanceTable table(1);
    harken::QueueInstanceToken queue{};
    ASSERT_TRUE(table.Acquire(create, &queue));
    std::array<harken::EffectiveQueueState, 3> chain{};
    auto serial = create.initial;
    for (auto& step : chain) {
        ASSERT_EQ(harken::QueueSequenceStepStatus::Successor,
                  harken::ResolveSequenceQueueStep(program, serial, &step));
        serial = step;
    }
    harken::ConditionalQueuePrepareRequest request{};
    request.expectedCurrentVersion = create.initial.version;
    request.rootDecision = {19, 23};
    request.expectedCurrentOccurrence = {29, 31};
    request.successorOccurrence = {37, 41};
    request.completion = BoundPredicate(request.rootDecision);
    request.next = serial;
    request.projectedStates = chain.data();
    request.projectedStateCount = static_cast<std::uint32_t>(chain.size());
    harken::PreparedQueueConditionalV1 prepared{};
    chain[1].version += 1;
    EXPECT_FALSE(table.PrepareConditional(queue, request, &prepared));
    chain[1].version -= 1;
    chain[1].selectionOrdinal = UINT64_MAX;
    EXPECT_FALSE(table.PrepareConditional(queue, request, &prepared));
    chain[1].selectionOrdinal = chain[0].selectionOrdinal + 1;
    ASSERT_TRUE(table.PrepareConditional(queue, request, &prepared));
    chain = {};
    harken::EffectiveQueueState effective{};
    ASSERT_TRUE(table.ResolveEffective(queue, &effective));
    EXPECT_EQ(create.initial.version, effective.version);
    harken::ConditionalQueueVersionToken version{};
    ASSERT_TRUE(table.CommitPreparedConditional(&prepared, &version));
    EXPECT_EQ(serial.version, version.version);
    EXPECT_FALSE(table.ActivateConditional(queue, {2}, request.rootDecision, request.expectedCurrentOccurrence));
    EXPECT_FALSE(table.ActivateConditional(queue, version, {19, 24}, request.expectedCurrentOccurrence));
    ASSERT_TRUE(table.ActivateConditional(queue, version, request.rootDecision, request.expectedCurrentOccurrence));
    ASSERT_TRUE(table.ResolveEffective(queue, &effective));
    EXPECT_EQ(serial.version, effective.version);
    EXPECT_EQ(serial.cursor, effective.cursor);
    EXPECT_EQ(serial.currentChildOrdinal, effective.currentChildOrdinal);
    EXPECT_EQ(serial.selectionOrdinal, effective.selectionOrdinal);
    EXPECT_EQ(serial.randomInitialRecent, effective.randomInitialRecent);
    ASSERT_TRUE(table.SettleActivated(queue, version));
    EXPECT_FALSE(table.SettleActivated(queue, version));
    ASSERT_TRUE(table.Release(queue));
    EXPECT_EQ(0u, table.LiveCount());
}

TEST(QueueRuntime, ReplacementProjectionValidatesFirstResetAndPublishesFinalVersion) {
    harken::QueueInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = 11;
    create.containerStableId = harkentest::ObjectId(13);
    create.programSemanticFingerprint = 17;
    create.childCount = 3;
    create.initial.version = 7;
    create.initial.cursor = 2;
    create.initial.currentChildOrdinal = 7;
    create.initial.selectionOrdinal = 11;
    create.resetInitial.version = 1;
    create.resetInitial.currentChildOrdinal = 3;
    create.resetInitial.selectionOrdinal = 1;
    harken::QueueInstanceTable table(1);
    harken::QueueInstanceToken queue{};
    ASSERT_TRUE(table.Acquire(create, &queue));
    harken::ConditionalQueuePrepareRequest old{};
    old.expectedCurrentVersion = 7;
    old.rootDecision = {19, 23};
    old.expectedCurrentOccurrence = {29, 31};
    old.successorOccurrence = {37, 41};
    old.completion = BoundPredicate(old.rootDecision);
    old.next = create.resetInitial;
    old.next.version = 8;
    harken::ConditionalQueueVersionToken oldVersion{};
    ASSERT_TRUE(table.PrepareConditional(queue, old, &oldVersion));
    auto replacement = old;
    replacement.rootDecision = {43, 47};
    replacement.completion = BoundPredicate(replacement.rootDecision);
    std::array<harken::EffectiveQueueState, 2> chain{old.next, old.next};
    chain[1].version = 9;
    chain[1].cursor = 1;
    chain[1].currentChildOrdinal = 5;
    chain[1].selectionOrdinal = 2;
    replacement.next = chain[1];
    replacement.projectedStates = chain.data();
    replacement.projectedStateCount = 2;
    replacement.completionHasLogicalFloor = true;
    replacement.logicalCompletionFrame = 600;
    replacement.retiredSource.sourceArm = {7, 11, 9, 43};
    replacement.retiredSource.sourceRenderHandle =
        (UINT64_C(43) << 32) | 9u;
    replacement.retiredSource.logicalCompletionSequence = 1;
    replacement.retiredSource.tombstoneAudioSequence = 1;
    harken::PreparedQueueConditionalReplacementV1 prepared{};
    chain[0].currentChildOrdinal = 9;
    EXPECT_EQ(harken::QueueResetStatusV1::InvalidInput,
        table.PrepareConditionalReplacement(queue, oldVersion, old.rootDecision, replacement, &prepared));
    chain[0].currentChildOrdinal = 3;
    ASSERT_EQ(harken::QueueResetStatusV1::Ok,
        table.PrepareConditionalReplacement(queue, oldVersion, old.rootDecision, replacement, &prepared));
    ASSERT_EQ(harken::QueueResetStatusV1::Ok, table.CancelPreparedConditionalReplacement(&prepared));
    ASSERT_EQ(harken::QueueResetStatusV1::Ok,
        table.PrepareConditionalReplacement(queue, oldVersion, old.rootDecision, replacement, &prepared));
    chain = {};
    harken::QueueResetTargetV1 target{};
    target.world = 7;
    target.containerStableId = harkentest::ObjectId(13);
    target.programSemanticFingerprint = 17;
    harken::PreparedQueueResetSetV1 reset{};
    ASSERT_EQ(harken::QueueResetStatusV1::Ok, table.PrepareReset(target, &reset));
    ASSERT_EQ(harken::QueueResetStatusV1::Ok, table.CommitPreparedConditionalReplacement(&prepared, 61));
    ASSERT_EQ(harken::QueueResetStatusV1::Ok, table.CommitPreparedReset(&reset, 61));
    EXPECT_FALSE(table.ActivateConditional(queue, oldVersion, old.rootDecision, old.expectedCurrentOccurrence));
    EXPECT_FALSE(table.ActivateConditional(queue, {9}, old.rootDecision, old.expectedCurrentOccurrence));
    ASSERT_TRUE(table.ActivateConditional(queue, {9}, replacement.rootDecision, replacement.expectedCurrentOccurrence));
    ASSERT_TRUE(table.SettleActivated(queue, {9}));
    EXPECT_TRUE(harken::QueueInstanceTableTestAccess::
                    PendingCompletionIdentityEmpty(table, queue));
    harken::QueueInstanceSnapshot result{};
    ASSERT_TRUE(table.Capture(queue, &result));
    EXPECT_EQ(9u, result.logicMirror.version);
    EXPECT_EQ(5u, result.logicMirror.currentChildOrdinal);
    EXPECT_FALSE(result.resetPending);
    ASSERT_TRUE(table.Release(queue));
}

TEST(QueueRuntime, LogicalBoundaryReplacementSurvivesCommittedReset) {
    harken::QueueInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = 11;
    create.containerStableId = harkentest::ObjectId(13);
    create.programSemanticFingerprint = 17;
    create.childCount = 2;
    create.initial.version = 7;
    create.initial.currentChildOrdinal = 5;
    create.initial.selectionOrdinal = 11;
    create.resetInitial.version = 1;
    create.resetInitial.currentChildOrdinal = 3;
    create.resetInitial.selectionOrdinal = 1;
    harken::QueueInstanceTable table(1);
    harken::QueueInstanceToken queue{};
    ASSERT_TRUE(table.Acquire(create, &queue));

    harken::ConditionalQueuePrepareRequest old{};
    old.expectedCurrentVersion = create.initial.version;
    old.rootDecision = {19, 23};
    old.expectedCurrentOccurrence = {29, 31};
    old.successorOccurrence = {37, 41};
    old.completion = BoundPredicate(old.rootDecision);
    old.next = create.resetInitial;
    old.next.version = 8;
    harken::ConditionalQueueVersionToken oldVersion{};
    ASSERT_TRUE(table.PrepareConditional(queue, old, &oldVersion));

    auto replacement = LogicalBoundaryRequest(300);
    replacement.expectedCurrentVersion = create.initial.version;
    replacement.rootDecision = {43, 47};
    replacement.expectedCurrentOccurrence = old.expectedCurrentOccurrence;
    replacement.successorOccurrence = old.successorOccurrence;
    replacement.next = old.next;
    replacement.projectedStates = &replacement.next;
    replacement.projectedStateCount = 1;
    harken::PreparedQueueConditionalReplacementV1 prepared{};
    ASSERT_EQ(harken::QueueResetStatusV1::Ok,
              table.PrepareConditionalReplacement(
                  queue, oldVersion, old.rootDecision, replacement, &prepared));
    harken::QueueResetTargetV1 target{};
    target.world = create.world;
    target.containerStableId = create.containerStableId;
    target.programSemanticFingerprint = create.programSemanticFingerprint;
    harken::PreparedQueueResetSetV1 reset{};
    ASSERT_EQ(harken::QueueResetStatusV1::Ok,
              table.PrepareReset(target, &reset));
    ASSERT_EQ(harken::QueueResetStatusV1::Ok,
              table.CommitPreparedConditionalReplacement(&prepared, 61));
    ASSERT_EQ(harken::QueueResetStatusV1::Ok,
              table.CommitPreparedReset(&reset, 61));

    harken::OccurrenceStatePool occurrences(1);
    harken::QueueConditionalProof proof{};
    ASSERT_EQ(harken::QueueConditionalProofStatus::ReadyToActivate,
              table.ProveConditionalForQuantum(
                  queue, {8}, occurrences, 256, 384, 1, true, &proof));
    EXPECT_EQ(harken::QueueConditionalCompletionMode::LogicalBoundary,
              proof.completionMode);
    EXPECT_EQ(44u, proof.completion.activationOffset);
    ASSERT_TRUE(table.ActivateProvenConditional(proof));
    ASSERT_TRUE(table.SettleActivated(queue, {8}));
    harken::EffectiveQueueState effective{};
    ASSERT_TRUE(table.ResolveEffective(queue, &effective));
    EXPECT_EQ(8u, effective.version);
    EXPECT_EQ(create.resetInitial.currentChildOrdinal,
              effective.currentChildOrdinal);
    EXPECT_TRUE(table.Release(queue));
}

TEST(QueueRuntime, LogicalBoundaryAbortClearsPendingIdentity) {
    harken::OccurrenceStatePool occurrences(1);
    harken::QueueInstanceTable table(1);
    const auto queue = AcquireLogicalBoundaryQueue(table);
    auto first = LogicalBoundaryRequest(300);
    harken::ConditionalQueueVersionToken firstVersion{};
    ASSERT_TRUE(table.PrepareConditional(queue, first, &firstVersion));
    ASSERT_TRUE(table.RetireConditionalForAbort(
        queue, firstVersion, first.rootDecision));

    auto second = LogicalBoundaryRequest(500);
    second.rootDecision = {43, 47};
    second.expectedCurrentOccurrence = {53, 59};
    second.successorOccurrence = {61, 67};
    harken::ConditionalQueueVersionToken secondVersion{};
    ASSERT_TRUE(table.PrepareConditional(queue, second, &secondVersion));
    harken::QueueConditionalProof proof{};
    ASSERT_EQ(harken::QueueConditionalProofStatus::ReadyToActivate,
              table.ProveConditionalForQuantum(
                  queue, secondVersion, occurrences, 448, 512, 1, true,
                  &proof));
    EXPECT_EQ(52u, proof.completion.activationOffset);
    EXPECT_EQ(second.rootDecision.slot, proof.decision.slot);
    EXPECT_EQ(second.rootDecision.generation,
              proof.decision.generation);
    ASSERT_TRUE(table.RetireConditionalForAbort(
        queue, secondVersion, second.rootDecision));
    EXPECT_TRUE(table.Release(queue));
}

TEST(QueueRuntime, AbortRetiresWaitingConditionalWithoutAdvancing) {
    harken::QueueInstanceTable table(1);
    harken::QueueInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = 11;
    create.containerStableId = harkentest::ObjectId(13);
    create.programSemanticFingerprint = 17;
    create.childCount = 2;
    create.initial.version = 1;
    create.initial.cursor = 0;
    create.initial.currentChildOrdinal = 3;
    harken::QueueInstanceToken queue{};
    ASSERT_TRUE(table.Acquire(create, &queue));

    harken::ConditionalQueuePrepareRequest prepare{};
    prepare.expectedCurrentVersion = 1;
    prepare.rootDecision = {19, 23};
    prepare.expectedCurrentOccurrence = {29, 31};
    prepare.successorOccurrence = {37, 41};
    prepare.completion = BoundPredicate(prepare.rootDecision);
    prepare.next.version = 2;
    prepare.next.cursor = 1;
    prepare.next.currentChildOrdinal = 5;
    harken::ConditionalQueueVersionToken conditional{};
    ASSERT_TRUE(table.PrepareConditional(queue, prepare, &conditional));

    ASSERT_TRUE(table.RetireConditionalForAbort(
        queue, conditional, prepare.rootDecision));
    harken::EffectiveQueueState effective{};
    ASSERT_TRUE(table.ResolveEffective(queue, &effective));
    EXPECT_EQ(1u, effective.version);
    EXPECT_EQ(3u, effective.currentChildOrdinal);
    EXPECT_TRUE(table.Release(queue));
}

TEST(QueueRuntime, SignalCompletionAdvancesExactVersionWithoutConditional) {
    harken::QueueInstanceTable table(1);
    harken::QueueInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = 11;
    create.containerStableId = harkentest::ObjectId(13);
    create.programSemanticFingerprint = 17;
    create.childCount = 2;
    create.initial.version = 1;
    create.initial.cursor = 0;
    create.initial.currentChildOrdinal = 3;
    harken::QueueInstanceToken queue{};
    ASSERT_TRUE(table.Acquire(create, &queue));

    harken::EffectiveQueueState next{};
    next.version = 2;
    next.cursor = 1;
    next.currentChildOrdinal = 5;
    ASSERT_TRUE(table.AdvanceAfterSignalCompletion(queue, 1, next));

    harken::EffectiveQueueState effective{};
    ASSERT_TRUE(table.ResolveEffective(queue, &effective));
    EXPECT_EQ(2u, effective.version);
    EXPECT_EQ(1u, effective.cursor);
    EXPECT_EQ(5u, effective.currentChildOrdinal);
    EXPECT_FALSE(table.AdvanceAfterSignalCompletion(queue, 1, next));

    next.version = 3;
    next.cursor = 0;
    next.currentChildOrdinal = 3;
    ASSERT_TRUE(table.AdvanceAfterSignalCompletion(queue, 2, next));
    ASSERT_TRUE(table.ResolveEffective(queue, &effective));
    EXPECT_EQ(3u, effective.version);
    EXPECT_EQ(0u, effective.cursor);
    EXPECT_EQ(3u, effective.currentChildOrdinal);
}

struct FutureQueueResetFixture {
    harken::QueueInstanceTable table{1};
    harken::QueueInstanceToken queue{};
    harken::QueueResetTargetV1 target{};

    FutureQueueResetFixture() {
        harken::QueueInstanceCreateRequest create{};
        create.world = 7;
        create.rootInstance = 11;
        create.containerStableId = harkentest::ObjectId(13);
        create.programSemanticFingerprint = 17;
        create.childCount = 2;
        create.initial.version = 1;
        create.initial.currentChildOrdinal = 3;
        create.initial.selectionOrdinal = 1;
        create.resetInitial = create.initial;
        EXPECT_TRUE(table.Acquire(create, &queue));
        target.world = create.world;
        target.containerStableId = create.containerStableId;
        target.programSemanticFingerprint = create.programSemanticFingerprint;
    }

    void AcceptReset128(const harken::MonitorControlCause& cause = {}) {
        harken::PreparedQueueResetSetV1 reset;
        ASSERT_EQ(harken::QueueResetStatusV1::Ok, table.PrepareReset(target, &reset, 128));
        ASSERT_EQ(harken::QueueResetStatusV1::Ok,
                  table.CommitPreparedReset(&reset, 31, 128, cause));
    }
};

TEST(QueueRuntime, FutureResetProjectsFromActualCurrentAfterEarlierAdvance) {
    FutureQueueResetFixture fixture;
    fixture.AcceptReset128();
    ASSERT_FALSE(HasFatalFailure());
    harken::EffectiveQueueState advanced{};
    advanced.version = 2;
    advanced.cursor = 1;
    advanced.currentChildOrdinal = 5;
    advanced.selectionOrdinal = 1;
    ASSERT_TRUE(fixture.table.AdvanceAfterSignalCompletion(fixture.queue, 1, advanced));
    harken::EffectiveQueueState current{};
    harken::EffectiveQueueState projected{};
    bool reset = true;
    fixture.table.ApplyDue(127);
    ASSERT_TRUE(fixture.table.ResolveNextSelection(fixture.queue, &current, &projected, &reset));
    EXPECT_FALSE(reset);
    EXPECT_EQ(2u, current.version);
    fixture.table.ApplyDue(128);
    ASSERT_TRUE(fixture.table.ResolveNextSelection(fixture.queue, &current, &projected, &reset));
    EXPECT_TRUE(reset);
    EXPECT_EQ(3u, projected.version);
    EXPECT_EQ(3u, projected.currentChildOrdinal);
    ASSERT_TRUE(fixture.table.AdvanceAfterSignalCompletion(fixture.queue, 2, projected));
    ASSERT_TRUE(fixture.table.ResolveNextSelection(fixture.queue, &current, &projected, &reset));
    EXPECT_FALSE(reset);
    EXPECT_EQ(3u, current.version);
}

TEST(QueueRuntime, CancelLaterResetPreparationPreservesAcceptedFutureIntent) {
    FutureQueueResetFixture fixture;
    fixture.AcceptReset128();
    ASSERT_FALSE(HasFatalFailure());
    const auto accepted = harken::QueueInstanceTableTestAccess::MirrorAndReset(
        fixture.table, fixture.queue);
    harken::PreparedQueueResetSetV1 later;
    const auto result = fixture.table.PrepareReset(fixture.target, &later);
    if (result == harken::QueueResetStatusV1::Ok) {
        EXPECT_EQ(harken::QueueResetStatusV1::Ok,
                  fixture.table.CancelPreparedReset(&later));
    } else {
        EXPECT_EQ(harken::QueueResetStatusV1::Backpressure, result);
    }
    EXPECT_EQ(accepted, harken::QueueInstanceTableTestAccess::MirrorAndReset(
        fixture.table, fixture.queue));
    fixture.table.ApplyDue(128);
    harken::EffectiveQueueState current{};
    harken::EffectiveQueueState projected{};
    bool reset = false;
    ASSERT_TRUE(fixture.table.ResolveNextSelection(fixture.queue, &current, &projected, &reset));
    EXPECT_TRUE(reset);
    EXPECT_EQ(2u, projected.version);
    EXPECT_EQ(3u, projected.currentChildOrdinal);
}

TEST(QueueRuntime, PreparedSignalKeepsMirrorAndResetUntilExactAcceptedOutcome) {
    FutureQueueResetFixture fixture;
    fixture.table.ApplyDue(64);
    harken::MonitorControlCause resetCause{};
    resetCause.request.key = {7, 13};
    resetCause.actionFiring = 23;
    resetCause.executionOrdinal = 2;
    fixture.AcceptReset128(resetCause);
    harken::MonitorQueueResetObservation observation{};
    ASSERT_TRUE(fixture.table.CaptureMonitorReset(fixture.queue, &observation));
    EXPECT_EQ(harken::MonitorQueueResetObservation::Stage::Intent, observation.stage);
    EXPECT_EQ(resetCause.request.key, observation.cause.request.key);
    ASSERT_FALSE(HasFatalFailure());
    const auto before = harken::QueueInstanceTableTestAccess::MirrorAndReset(
        fixture.table, fixture.queue);
    harken::EffectiveQueueState current{};
    harken::EffectiveQueueState next{};
    std::uint64_t resetIntent = 0;
    const harken::QueueExecutionOrderV1 order{192, 41, 0};
    ASSERT_TRUE(fixture.table.ResolveNextSelection(fixture.queue, order,
        &current, &next, &resetIntent));
    ASSERT_NE(0u, resetIntent);
    harken::PreparedQueueSignalAdvanceV1 prepared;
    ASSERT_EQ(harken::QueueSignalStatusV1::Ok,
        fixture.table.PrepareSignalAdvance(fixture.queue, current, next, order, resetIntent, &prepared));
    EXPECT_EQ(before, harken::QueueInstanceTableTestAccess::MirrorAndReset(fixture.table, fixture.queue));
    EXPECT_FALSE(fixture.table.AdvanceAfterSignalCompletion(fixture.queue, current.version, next));
    EXPECT_FALSE(fixture.table.Release(fixture.queue));
    harken::PreparedQueueResetSetV1 reset;
    EXPECT_EQ(harken::QueueResetStatusV1::Backpressure,
        fixture.table.PrepareReset(fixture.target, &reset));
    ASSERT_TRUE(fixture.table.CommitPreparedSignalAdvance(&prepared));
    EXPECT_EQ(before, harken::QueueInstanceTableTestAccess::MirrorAndReset(fixture.table, fixture.queue));
    harken::AcceptedQueueSignalAdvanceV1 accepted;
    ASSERT_TRUE(fixture.table.PublishPreparedSignalAdvance(&prepared, &accepted));
    EXPECT_FALSE(prepared.Valid());
    EXPECT_EQ(before, harken::QueueInstanceTableTestAccess::MirrorAndReset(fixture.table, fixture.queue));
    fixture.table.ApplyDue(128);
    ASSERT_TRUE(fixture.table.SettleSignalAdvance(accepted, harken::QueueSignalOutcomeV1::InFlight));
    ASSERT_TRUE(fixture.table.CaptureMonitorReset(fixture.queue, &observation));
    EXPECT_EQ(harken::MonitorQueueResetObservation::Stage::Intent, observation.stage);
    harken::EffectiveQueueState effective{};
    ASSERT_TRUE(fixture.table.ResolveEffective(fixture.queue, &effective));
    EXPECT_EQ(current.version, effective.version);
    auto stale = accepted;
    ++stale.attempt;
    EXPECT_FALSE(fixture.table.SettleSignalAdvance(stale, harken::QueueSignalOutcomeV1::Applied));
    ASSERT_TRUE(fixture.table.SettleSignalAdvance(accepted, harken::QueueSignalOutcomeV1::Applied));
    EXPECT_FALSE(fixture.table.SettleSignalAdvance(accepted, harken::QueueSignalOutcomeV1::Applied));
    ASSERT_TRUE(fixture.table.ResolveEffective(fixture.queue, &effective));
    EXPECT_EQ(next.version, effective.version);
    EXPECT_EQ(next.currentChildOrdinal, effective.currentChildOrdinal);
    harken::QueueInstanceSnapshot snapshot{};
    ASSERT_TRUE(fixture.table.Capture(fixture.queue, &snapshot));
    EXPECT_FALSE(snapshot.resetPending);
    EXPECT_EQ(0u, snapshot.pendingVersion);
    ASSERT_TRUE(fixture.table.CaptureMonitorReset(fixture.queue, &observation));
    EXPECT_EQ(harken::MonitorQueueResetObservation::Stage::Applied, observation.stage);
    EXPECT_EQ(resetCause.request.key, observation.cause.request.key);
    EXPECT_EQ(resetCause.actionFiring, observation.cause.actionFiring);
    EXPECT_EQ(resetCause.executionOrdinal, observation.cause.executionOrdinal);
    EXPECT_EQ(current.version, observation.previousVersion);
    EXPECT_EQ(next.version, observation.nextVersion);
    EXPECT_EQ(resetIntent, observation.resetIntent);
    EXPECT_TRUE(fixture.table.Release(fixture.queue));
    EXPECT_FALSE(fixture.table.CaptureMonitorReset(fixture.queue, &observation));
}

TEST(QueueRuntime, DuplicateDueResetAndStaleSelectionCannotReplaceOriginalCause) {
    FutureQueueResetFixture fixture;
    harken::MonitorControlCause original{}, later{};
    original.request.key = {9, 17};
    later.request.key = {9, 21};
    fixture.AcceptReset128(original);
    fixture.table.ApplyDue(128);
    harken::MonitorQueueResetObservation before{}, after{};
    ASSERT_TRUE(fixture.table.CaptureMonitorReset(fixture.queue, &before));
    harken::PreparedQueueResetSetV1 duplicate;
    ASSERT_EQ(harken::QueueResetStatusV1::Ok, fixture.table.PrepareReset(fixture.target, &duplicate, 128));
    ASSERT_EQ(harken::QueueResetStatusV1::Ok, fixture.table.CommitPreparedReset(&duplicate, 37, 128, later));
    fixture.table.RecordMonitorResetSelection(fixture.queue, {128, 37, 0}, later);
    auto stale = fixture.queue;
    ++stale.generation;
    fixture.table.RecordMonitorResetSelection(stale, {128, 31, 0}, later);
    ASSERT_TRUE(fixture.table.CaptureMonitorReset(fixture.queue, &after));
    EXPECT_EQ(before.resetIntent, after.resetIntent);
    EXPECT_EQ(original.request.key, after.cause.request.key);
    EXPECT_EQ(harken::MonitorQueueResetObservation::Stage::Intent, after.stage);
    EXPECT_FALSE(fixture.table.CaptureMonitorReset(stale, &after));
}

TEST(QueueRuntime, CancelPreparedAndNegativeAcceptedSignalPreserveOriginalResetIdentity) {
    FutureQueueResetFixture fixture;
    fixture.AcceptReset128();
    ASSERT_FALSE(HasFatalFailure());
    harken::EffectiveQueueState current{};
    harken::EffectiveQueueState next{};
    std::uint64_t originalIntent = 0;
    const harken::QueueExecutionOrderV1 order{192, 41, 0};
    ASSERT_TRUE(fixture.table.ResolveNextSelection(fixture.queue, order, &current, &next, &originalIntent));
    const auto before = harken::QueueInstanceTableTestAccess::MirrorAndReset(fixture.table, fixture.queue);
    for (bool publish : {false, true}) {
        harken::PreparedQueueSignalAdvanceV1 prepared;
        ASSERT_EQ(harken::QueueSignalStatusV1::Ok, fixture.table.PrepareSignalAdvance(
            fixture.queue, current, next, order, originalIntent, &prepared));
        ASSERT_TRUE(fixture.table.CommitPreparedSignalAdvance(&prepared));
        if (publish) {
            harken::AcceptedQueueSignalAdvanceV1 accepted;
            ASSERT_TRUE(fixture.table.PublishPreparedSignalAdvance(&prepared, &accepted));
            ASSERT_TRUE(fixture.table.SettleSignalAdvance(accepted,
                harken::QueueSignalOutcomeV1::DefinitivelyNotApplied));
        } else {
            ASSERT_TRUE(fixture.table.CancelPreparedSignalAdvance(&prepared));
        }
        EXPECT_EQ(before, harken::QueueInstanceTableTestAccess::MirrorAndReset(fixture.table, fixture.queue));
        std::uint64_t remainingIntent = 0;
        ASSERT_TRUE(fixture.table.ResolveNextSelection(fixture.queue, order,
            &current, &next, &remainingIntent));
        EXPECT_EQ(originalIntent, remainingIntent);
    }
}

TEST(QueueRuntime, EarlierSignalSettlementAfterResetDueDoesNotConsumeFutureReset) {
    FutureQueueResetFixture fixture;
    fixture.AcceptReset128();
    ASSERT_FALSE(HasFatalFailure());
    const harken::QueueExecutionOrderV1 beforeReset{64, 41, 0};
    harken::EffectiveQueueState current{};
    harken::EffectiveQueueState unused{};
    std::uint64_t resetIntent = 0;
    ASSERT_TRUE(fixture.table.ResolveNextSelection(fixture.queue, beforeReset,
        &current, &unused, &resetIntent));
    EXPECT_EQ(0u, resetIntent);
    auto next = current;
    ++next.version;
    ++next.selectionOrdinal;
    next.cursor = 1;
    next.currentChildOrdinal = 5;
    harken::PreparedQueueSignalAdvanceV1 prepared;
    ASSERT_EQ(harken::QueueSignalStatusV1::Ok, fixture.table.PrepareSignalAdvance(
        fixture.queue, current, next, beforeReset, resetIntent, &prepared));
    ASSERT_TRUE(fixture.table.CommitPreparedSignalAdvance(&prepared));
    harken::AcceptedQueueSignalAdvanceV1 accepted;
    ASSERT_TRUE(fixture.table.PublishPreparedSignalAdvance(&prepared, &accepted));
    fixture.table.ApplyDue(128);
    ASSERT_TRUE(fixture.table.SettleSignalAdvance(accepted, harken::QueueSignalOutcomeV1::Applied));
    ASSERT_TRUE(fixture.table.ResolveNextSelection(fixture.queue, {192, 42, 0},
        &current, &next, &resetIntent));
    EXPECT_NE(0u, resetIntent);
    EXPECT_EQ(3u, next.version);
    EXPECT_EQ(3u, next.currentChildOrdinal);
}

TEST(QueueRuntime, ResetSelectionOrderIsLexicographicAndDistinctFutureIntentsRefuse) {
    FutureQueueResetFixture fixture;
    harken::PreparedQueueResetSetV1 reset;
    ASSERT_EQ(harken::QueueResetStatusV1::Ok, fixture.table.PrepareReset(fixture.target, &reset, 128, 4));
    ASSERT_EQ(harken::QueueResetStatusV1::Ok, fixture.table.CommitPreparedReset(&reset, 31));
    const harken::QueueExecutionOrderV1 selections[] = {
        {127, 40, 9}, {128, 30, 9}, {128, 31, 3}, {128, 31, 4},
        {128, 31, 5}, {128, 32, 0}, {129, 1, 0}};
    for (std::size_t i = 0; i < std::size(selections); ++i) {
        harken::EffectiveQueueState current{}, next{};
        std::uint64_t intent = 0;
        ASSERT_TRUE(fixture.table.ResolveNextSelection(fixture.queue, selections[i], &current, &next, &intent));
        EXPECT_EQ(i >= 4, intent != 0) << i;
    }
    for (auto now : {64u, 128u}) {
        fixture.table.ApplyDue(now);
        const auto before = harken::QueueInstanceTableTestAccess::MirrorAndReset(fixture.table, fixture.queue);
        for (auto frame : {96u, 192u}) {
            if (frame <= now) continue;
            harken::PreparedQueueResetSetV1 conflicting;
            EXPECT_EQ(harken::QueueResetStatusV1::Backpressure,
                fixture.table.PrepareReset(fixture.target, &conflicting, frame));
            EXPECT_EQ(before, harken::QueueInstanceTableTestAccess::MirrorAndReset(fixture.table, fixture.queue));
        }
    }
}

TEST(QueueRuntime, SignalMirrorAndAttemptExhaustionRefuseBeforeOwnership) {
    FutureQueueResetFixture fixture;
    harken::EffectiveQueueState current{};
    ASSERT_TRUE(fixture.table.ResolveEffective(fixture.queue, &current));
    auto next = current;
    ++next.version;
    next.currentChildOrdinal = 5;
    next.cursor = 1;
    harken::QueueInstanceTableTestAccess::SetSignalLimits(fixture.table, fixture.queue, UINT64_MAX - 3u, 0);
    harken::PreparedQueueSignalAdvanceV1 prepared;
    ASSERT_EQ(harken::QueueSignalStatusV1::Ok, fixture.table.PrepareSignalAdvance(
        fixture.queue, current, next, {64, 31, 0}, 0, &prepared));
    ASSERT_TRUE(fixture.table.CommitPreparedSignalAdvance(&prepared));
    harken::AcceptedQueueSignalAdvanceV1 accepted;
    ASSERT_TRUE(fixture.table.PublishPreparedSignalAdvance(&prepared, &accepted));
    ASSERT_TRUE(fixture.table.SettleSignalAdvance(accepted, harken::QueueSignalOutcomeV1::Applied));
    current = next;
    ++next.version;
    const auto before = harken::QueueInstanceTableTestAccess::MirrorAndReset(fixture.table, fixture.queue);
    EXPECT_EQ(harken::QueueSignalStatusV1::RuntimeRecreateRequired, fixture.table.PrepareSignalAdvance(
        fixture.queue, current, next, {128, 32, 0}, 0, &prepared));
    EXPECT_FALSE(prepared.Valid());
    EXPECT_EQ(before, harken::QueueInstanceTableTestAccess::MirrorAndReset(fixture.table, fixture.queue));
    harken::QueueInstanceTableTestAccess::SetSignalLimits(fixture.table, fixture.queue, 2, UINT64_MAX);
    EXPECT_EQ(harken::QueueSignalStatusV1::RuntimeRecreateRequired, fixture.table.PrepareSignalAdvance(
        fixture.queue, current, next, {128, 32, 0}, 0, &prepared));
    EXPECT_TRUE(fixture.table.Release(fixture.queue));
}

TEST(QueueRuntime, ReleasedInstanceInvalidatesOldTokenBeforeReuse) {
    harken::QueueInstanceTable table(1);
    harken::QueueInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = 11;
    create.containerStableId = harkentest::ObjectId(13);
    create.programSemanticFingerprint = 17;
    create.childCount = 2;
    create.initial.version = 1;
    create.initial.currentChildOrdinal = 3;
    harken::QueueInstanceToken first{};
    ASSERT_TRUE(table.Acquire(create, &first));
    ASSERT_TRUE(table.Release(first));
    harken::QueueInstanceSnapshot stale{};
    EXPECT_FALSE(table.Capture(first, &stale));

    harken::QueueInstanceToken reused{};
    ASSERT_TRUE(table.Acquire(create, &reused));
    EXPECT_EQ(first.slot, reused.slot);
    EXPECT_EQ(first.generation + 1u, reused.generation);
    EXPECT_FALSE(table.Release(first));
    EXPECT_TRUE(table.Release(reused));
}

TEST(QueueRuntime, PreparedInstanceStaysHiddenUntilCommitOrCancel) {
    harken::QueueInstanceTable table(1);
    harken::QueueInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = 11;
    create.containerStableId = harkentest::ObjectId(13);
    create.programSemanticFingerprint = 17;
    create.childCount = 2;
    create.emitterSlot = 5;
    create.emitterGeneration = 7;
    create.initial.version = 1;
    create.initial.cursor = 0;
    create.initial.currentChildOrdinal = 3;
    create.initial.selectionOrdinal = 1;

    harken::PreparedQueueInstanceV1 prepared;
    ASSERT_TRUE(table.PrepareAcquire(create, &prepared));
    const harken::QueueInstanceToken cancelled = prepared.Token();
    ASSERT_TRUE(cancelled.Valid());
    EXPECT_EQ(0u, table.LiveCount());
    harken::QueueInstanceSnapshot snapshot{};
    EXPECT_FALSE(table.Capture(cancelled, &snapshot));
    harken::QueueInstanceToken unavailable{};
    EXPECT_FALSE(table.Acquire(create, &unavailable));
    ASSERT_TRUE(table.CancelPreparedAcquire(&prepared));
    EXPECT_FALSE(prepared.Valid());
    EXPECT_EQ(0u, table.LiveCount());

    ASSERT_TRUE(table.PrepareAcquire(create, &prepared));
    harken::QueueInstanceToken committed{};
    ASSERT_TRUE(table.CommitPreparedAcquire(&prepared, &committed));
    EXPECT_FALSE(prepared.Valid());
    EXPECT_EQ(1u, table.LiveCount());
    ASSERT_TRUE(table.Capture(committed, &snapshot));
    EXPECT_EQ(create.world, snapshot.world);
    EXPECT_EQ(create.rootInstance, snapshot.rootInstance);
    EXPECT_EQ(create.containerStableId, snapshot.containerStableId);
    EXPECT_EQ(create.programSemanticFingerprint,
              snapshot.programSemanticFingerprint);
    EXPECT_EQ(create.initial.version, snapshot.logicMirror.version);
    EXPECT_EQ(create.initial.currentChildOrdinal,
              snapshot.logicMirror.currentChildOrdinal);
    EXPECT_TRUE(table.Release(committed));
}

TEST(QueueRuntime,
     CommittedOwnerRetainsQueueTokenUntilBlockedReleaseCanRetry) {
    harken::QueueInstanceTable table(1);
    harken::QueueInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = 11;
    create.containerStableId = harkentest::ObjectId(13);
    create.programSemanticFingerprint = 17;
    create.childCount = 2;
    create.initial.version = 1;
    create.initial.currentChildOrdinal = 3;

    harken::PreparedQueueInstanceV1 prepared{};
    ASSERT_TRUE(table.PrepareAcquire(create, &prepared));
    harken::QueueInstanceToken committed{};
    ASSERT_TRUE(table.CommitPreparedAcquire(&prepared, &committed));

    harken::ConditionalQueuePrepareRequest conditional{};
    conditional.expectedCurrentVersion = 1;
    conditional.rootDecision = {19, 23};
    conditional.expectedCurrentOccurrence = {29, 31};
    conditional.successorOccurrence = {37, 41};
    conditional.completion = BoundPredicate(conditional.rootDecision);
    conditional.next.version = 2;
    conditional.next.cursor = 1;
    conditional.next.currentChildOrdinal = 5;
    harken::ConditionalQueueVersionToken version{};
    ASSERT_TRUE(table.PrepareConditional(
        committed, conditional, &version));

    harken::EngineActionPlayScratch::PreparedQueueInstanceOwnerContext owner{
        &table, &prepared, &committed, nullptr, true};
    EXPECT_FALSE(harken::detail::CancelPreparedQueueInstanceOwnerForTests(
        &owner));
    EXPECT_TRUE(owner.committed);
    EXPECT_TRUE(committed.Valid());
    harken::QueueInstanceSnapshot snapshot{};
    ASSERT_TRUE(table.Capture(committed, &snapshot));
    EXPECT_EQ(version.version, snapshot.pendingVersion);

    ASSERT_TRUE(table.RetireConditionalForAbort(
        committed, version, conditional.rootDecision));
    EXPECT_TRUE(harken::detail::CancelPreparedQueueInstanceOwnerForTests(
        &owner));
    EXPECT_FALSE(owner.committed);
    EXPECT_FALSE(committed.Valid());
    EXPECT_EQ(0u, table.LiveCount());
}

TEST(QueueRuntime, PreparedCheckpointBackingSurvivesCompetitionAndAudioRefusesGrowth) {
    harken::QueueCheckpointTable checkpoints(4);
    harken::QueueCheckpointKey key{};
    key.world = 7;
    key.containerStableId = harkentest::ObjectId(13);
    key.programSemanticFingerprint = 17;
    key.resetEpoch = 19;
    harken::QueueCheckpointImage image{};
    image.effective.version = 2;
    image.replayCurrent = true;
    bool stored = true;
    {
        harken::memguard::AudioThreadScope audio;
        stored = checkpoints.Store(key, image);
    }
    EXPECT_FALSE(stored);
    EXPECT_EQ(0u, checkpoints.ConstructedRowCount());
    harken::QueueCheckpointTable::BackingPreparation backing;
    ASSERT_TRUE(checkpoints.PrepareBacking(2, &backing));
    ASSERT_TRUE(checkpoints.Store(key, image));
    EXPECT_EQ(3u, checkpoints.ConstructedRowCount());
    checkpoints.SetRowCommitPageLimitForTests(0);
    const auto bytes = checkpoints.RowStorageBytes();
    std::array<bool, 3> results{};
    {
        auto use = backing.Use();
        harken::memguard::AudioThreadScope audio;
        for (std::uint32_t index = 0; index < results.size(); ++index) {
            ++key.resetEpoch;
            results[index] = checkpoints.Store(key, image);
        }
    }
    EXPECT_TRUE(results[0]);
    EXPECT_TRUE(results[1]);
    EXPECT_FALSE(results[2]);
    EXPECT_EQ(bytes, checkpoints.RowStorageBytes());
    EXPECT_EQ(3u, checkpoints.LiveCount());
    backing = {};
    checkpoints.ClearAll();
}

TEST(QueueRuntime, CheckpointStoresOnlyExplicitEffectiveStateAndClearsAtEnd) {
    harken::QueueCheckpointTable checkpoints(1);
    harken::QueueCheckpointKey key{};
    key.world = 7;
    key.containerStableId = harkentest::ObjectId(13);
    key.programSemanticFingerprint = 17;
    key.resetEpoch = 19;
    harken::QueueCheckpointImage image{};
    image.effective.version = 2;
    image.effective.cursor = 1;
    image.effective.currentChildOrdinal = 5;
    image.effective.cycleOrdinal = 3;
    image.effective.selectionOrdinal = 11;
    image.replayCurrent = true;
    ASSERT_TRUE(checkpoints.Store(key, image));

    harken::QueueCheckpointImage captured{};
    ASSERT_TRUE(checkpoints.Load(key, &captured));
    EXPECT_EQ(image.effective.version, captured.effective.version);
    EXPECT_EQ(image.effective.cursor, captured.effective.cursor);
    EXPECT_EQ(image.effective.currentChildOrdinal,
              captured.effective.currentChildOrdinal);
    EXPECT_EQ(image.effective.cycleOrdinal,
              captured.effective.cycleOrdinal);
    EXPECT_EQ(image.effective.selectionOrdinal,
              captured.effective.selectionOrdinal);
    EXPECT_TRUE(captured.replayCurrent);

    harken::QueueCheckpointKey other = key;
    other.resetEpoch = 20;
    captured.effective.version = 71;
    EXPECT_FALSE(checkpoints.Load(other, &captured));
    EXPECT_EQ(71u, captured.effective.version);
    EXPECT_FALSE(checkpoints.Store(other, image));
    EXPECT_TRUE(checkpoints.Clear(key));
    EXPECT_FALSE(checkpoints.Load(key, &captured));
    EXPECT_EQ(0u, checkpoints.LiveCount());
}

TEST(QueueRuntime, PreparedCheckpointResetStaysHiddenUntilCommitOrCancel) {
    harken::QueueCheckpointTable checkpoints(1);
    harken::QueueCheckpointKey key{};
    key.world = 7;
    key.containerStableId = harkentest::ObjectId(13);
    key.programSemanticFingerprint = 17;
    key.resetEpoch = 19;
    key.emitterSlot = 5;
    key.emitterGeneration = 7;
    harken::QueueCheckpointImage image{};
    image.effective.version = 2;
    image.effective.cursor = 1;
    image.effective.currentChildOrdinal = 5;
    image.replayCurrent = true;
    ASSERT_TRUE(checkpoints.Store(key, image));

    harken::PreparedQueueCheckpointResetV1 prepared;
    ASSERT_EQ(harken::QueueResetStatusV1::Ok,
              checkpoints.PrepareReset(key, &prepared));
    EXPECT_FALSE(prepared.Idempotent());
    harken::QueueCheckpointImage captured{};
    EXPECT_TRUE(checkpoints.Load(key, &captured));
    EXPECT_FALSE(checkpoints.Store(key, image));
    ASSERT_EQ(harken::QueueResetStatusV1::Ok,
              checkpoints.CancelPreparedReset(&prepared));
    EXPECT_TRUE(checkpoints.Load(key, &captured));

    ASSERT_EQ(harken::QueueResetStatusV1::Ok,
              checkpoints.PrepareReset(key, &prepared));
    ASSERT_EQ(harken::QueueResetStatusV1::Ok,
              checkpoints.CommitPreparedReset(&prepared, 23));
    EXPECT_FALSE(checkpoints.Load(key, &captured));
    EXPECT_EQ(0u, checkpoints.LiveCount());

    ASSERT_EQ(harken::QueueResetStatusV1::Ok,
              checkpoints.PrepareReset(key, &prepared));
    EXPECT_TRUE(prepared.Idempotent());
    EXPECT_EQ(harken::QueueResetStatusV1::Ok,
              checkpoints.CommitPreparedReset(&prepared, 24));
}

TEST(QueueRuntime,
     PreparedInstanceResetOverridesOnlyTheNextSelectionForExactEmitter) {
    harken::QueueInstanceTable table(3);
    harken::QueueInstanceCreateRequest create{};
    create.world = 7;
    create.containerStableId = harkentest::ObjectId(13);
    create.programSemanticFingerprint = 17;
    create.childCount = 2;
    create.emitterSlot = 5;
    create.emitterGeneration = 7;
    create.initial.version = 1;
    create.initial.cursor = 0;
    create.initial.currentChildOrdinal = 3;
    create.initial.selectionOrdinal = 1;
    create.resetInitial = create.initial;

    create.rootInstance = 11;
    harken::QueueInstanceToken first{};
    ASSERT_TRUE(table.Acquire(create, &first));
    create.rootInstance = 12;
    harken::QueueInstanceToken second{};
    ASSERT_TRUE(table.Acquire(create, &second));
    create.rootInstance = 13;
    create.emitterSlot = 6;
    harken::QueueInstanceToken otherEmitter{};
    ASSERT_TRUE(table.Acquire(create, &otherEmitter));

    harken::EffectiveQueueState advanced{};
    advanced.version = 2;
    advanced.cursor = 1;
    advanced.currentChildOrdinal = 5;
    advanced.selectionOrdinal = 1;
    ASSERT_TRUE(table.AdvanceAfterSignalCompletion(first, 1, advanced));
    ASSERT_TRUE(table.AdvanceAfterSignalCompletion(second, 1, advanced));
    ASSERT_TRUE(
        table.AdvanceAfterSignalCompletion(otherEmitter, 1, advanced));

    harken::QueueResetTargetV1 target{};
    target.world = 7;
    target.containerStableId = harkentest::ObjectId(13);
    target.programSemanticFingerprint = 17;
    target.emitterSlot = 5;
    target.emitterGeneration = 7;
    harken::PreparedQueueResetSetV1 prepared;
    ASSERT_EQ(harken::QueueResetStatusV1::Ok,
              table.PrepareReset(target, &prepared));
    EXPECT_EQ(2u, prepared.MatchedCount());
    harken::EffectiveQueueState current{};
    harken::EffectiveQueueState next{};
    bool reset = true;
    ASSERT_TRUE(table.ResolveNextSelection(first, &current, &next, &reset));
    EXPECT_FALSE(reset);
    EXPECT_EQ(2u, current.version);
    ASSERT_EQ(harken::QueueResetStatusV1::Ok,
              table.CancelPreparedReset(&prepared));

    ASSERT_EQ(harken::QueueResetStatusV1::Ok,
              table.PrepareReset(target, &prepared));
    ASSERT_EQ(harken::QueueResetStatusV1::Ok,
              table.CommitPreparedReset(&prepared, 31));
    ASSERT_TRUE(table.ResolveNextSelection(first, &current, &next, &reset));
    EXPECT_TRUE(reset);
    EXPECT_EQ(2u, current.version);
    EXPECT_EQ(5u, current.currentChildOrdinal);
    EXPECT_EQ(3u, next.version);
    EXPECT_EQ(0u, next.cursor);
    EXPECT_EQ(3u, next.currentChildOrdinal);
    EXPECT_EQ(1u, next.selectionOrdinal);
    ASSERT_TRUE(table.AdvanceAfterSignalCompletion(first, 2, next));
    harken::QueueInstanceSnapshot settled{};
    ASSERT_TRUE(table.Capture(first, &settled));
    EXPECT_FALSE(settled.resetPending);
    EXPECT_EQ(3u, settled.logicMirror.version);

    ASSERT_TRUE(table.ResolveNextSelection(
        otherEmitter, &current, &next, &reset));
    EXPECT_FALSE(reset);
    EXPECT_EQ(2u, current.version);
}

TEST(QueueRuntime, PreparedInstanceResetRefusesAnOwnedSuccessor) {
    harken::QueueInstanceTable table(1);
    harken::QueueInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = 11;
    create.containerStableId = harkentest::ObjectId(13);
    create.programSemanticFingerprint = 17;
    create.childCount = 2;
    create.emitterSlot = 5;
    create.emitterGeneration = 7;
    create.initial.version = 1;
    create.initial.cursor = 0;
    create.initial.currentChildOrdinal = 3;
    create.initial.selectionOrdinal = 1;
    create.resetInitial = create.initial;
    harken::QueueInstanceToken queue{};
    ASSERT_TRUE(table.Acquire(create, &queue));

    harken::ConditionalQueuePrepareRequest conditional{};
    conditional.expectedCurrentVersion = 1;
    conditional.rootDecision = {19, 23};
    conditional.expectedCurrentOccurrence = {29, 31};
    conditional.successorOccurrence = {37, 41};
    conditional.completion = BoundPredicate(conditional.rootDecision);
    conditional.next.version = 2;
    conditional.next.cursor = 1;
    conditional.next.currentChildOrdinal = 5;
    conditional.next.selectionOrdinal = 1;
    harken::ConditionalQueueVersionToken version{};
    ASSERT_TRUE(table.PrepareConditional(queue, conditional, &version));

    harken::QueueResetTargetV1 target{};
    target.world = 7;
    target.containerStableId = harkentest::ObjectId(13);
    target.programSemanticFingerprint = 17;
    target.emitterSlot = 5;
    target.emitterGeneration = 7;
    harken::PreparedQueueResetSetV1 prepared;
    EXPECT_EQ(harken::QueueResetStatusV1::Backpressure,
              table.PrepareReset(target, &prepared));
    EXPECT_FALSE(prepared.Valid());
    harken::QueueInstanceSnapshot snapshot{};
    ASSERT_TRUE(table.Capture(queue, &snapshot));
    EXPECT_EQ(version.version, snapshot.pendingVersion);
    EXPECT_FALSE(snapshot.resetPending);
}

TEST(QueueRuntime,
     PreparedConditionalReplacementKeepsTheOldSuccessorUntilCommit) {
    harken::QueueInstanceTable table(2);
    harken::QueueInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = 11;
    create.containerStableId = harkentest::ObjectId(13);
    create.programSemanticFingerprint = 17;
    create.childCount = 2;
    create.emitterSlot = 5;
    create.emitterGeneration = 7;
    create.initial.version = 1;
    create.initial.cursor = 1;
    create.initial.currentChildOrdinal = 5;
    create.initial.selectionOrdinal = 9;
    create.resetInitial = create.initial;
    create.resetInitial.cursor = 0;
    create.resetInitial.currentChildOrdinal = 3;
    create.resetInitial.selectionOrdinal = 1;

    harken::QueueInstanceToken cancelledQueue{};
    ASSERT_TRUE(table.Acquire(create, &cancelledQueue));
    harken::ConditionalQueuePrepareRequest oldRequest{};
    oldRequest.expectedCurrentVersion = 1;
    oldRequest.rootDecision = {19, 23};
    oldRequest.expectedCurrentOccurrence = {29, 31};
    oldRequest.successorOccurrence = {37, 41};
    oldRequest.completion = BoundPredicate(oldRequest.rootDecision);
    oldRequest.next.version = 2;
    oldRequest.next.cursor = 0;
    oldRequest.next.currentChildOrdinal = 3;
    oldRequest.next.selectionOrdinal = 10;
    harken::ConditionalQueueVersionToken oldVersion{};
    ASSERT_TRUE(table.PrepareConditional(
        cancelledQueue, oldRequest, &oldVersion));

    harken::ConditionalQueuePrepareRequest replacement = oldRequest;
    replacement.rootDecision = {43, 47};
    replacement.successorOccurrence = {53, 59};
    replacement.completion = BoundPredicate(replacement.rootDecision);
    replacement.next = create.resetInitial;
    replacement.next.version = 2;
    harken::PreparedQueueConditionalReplacementV1 cancelled;
    ASSERT_EQ(harken::QueueResetStatusV1::Ok,
              table.PrepareConditionalReplacement(
                  cancelledQueue, oldVersion, oldRequest.rootDecision,
                  replacement, &cancelled));
    EXPECT_FALSE(table.ActivateConditional(
        cancelledQueue, oldVersion, oldRequest.rootDecision,
        oldRequest.expectedCurrentOccurrence));
    ASSERT_EQ(harken::QueueResetStatusV1::Ok,
              table.CancelPreparedConditionalReplacement(&cancelled));
    EXPECT_TRUE(table.ActivateConditional(
        cancelledQueue, oldVersion, oldRequest.rootDecision,
        oldRequest.expectedCurrentOccurrence));
    ASSERT_TRUE(table.SettleActivated(cancelledQueue, oldVersion));
    ASSERT_TRUE(table.Release(cancelledQueue));

    create.rootInstance = 12;
    harken::QueueInstanceToken committedQueue{};
    ASSERT_TRUE(table.Acquire(create, &committedQueue));
    ASSERT_TRUE(table.PrepareConditional(
        committedQueue, oldRequest, &oldVersion));
    harken::PreparedQueueConditionalReplacementV1 committed;
    ASSERT_EQ(harken::QueueResetStatusV1::Ok,
              table.PrepareConditionalReplacement(
                  committedQueue, oldVersion, oldRequest.rootDecision,
                  replacement, &committed));
    harken::QueueResetTargetV1 target{};
    target.world = create.world;
    target.containerStableId = create.containerStableId;
    target.programSemanticFingerprint =
        create.programSemanticFingerprint;
    target.emitterSlot = create.emitterSlot;
    target.emitterGeneration = create.emitterGeneration;
    harken::PreparedQueueResetSetV1 reset;
    ASSERT_EQ(harken::QueueResetStatusV1::Ok,
              table.PrepareReset(target, &reset));
    ASSERT_EQ(harken::QueueResetStatusV1::Ok,
              table.CommitPreparedConditionalReplacement(&committed, 61));
    ASSERT_EQ(harken::QueueResetStatusV1::Ok,
              table.CommitPreparedReset(&reset, 61));
    EXPECT_FALSE(table.ActivateConditional(
        committedQueue, oldVersion, oldRequest.rootDecision,
        oldRequest.expectedCurrentOccurrence));
    EXPECT_TRUE(table.ActivateConditional(
        committedQueue, oldVersion, replacement.rootDecision,
        replacement.expectedCurrentOccurrence));
    ASSERT_TRUE(table.SettleActivated(committedQueue, oldVersion));
    harken::QueueInstanceSnapshot snapshot{};
    ASSERT_TRUE(table.Capture(committedQueue, &snapshot));
    EXPECT_EQ(2u, snapshot.logicMirror.version);
    EXPECT_EQ(0u, snapshot.logicMirror.cursor);
    EXPECT_EQ(3u, snapshot.logicMirror.currentChildOrdinal);
    EXPECT_EQ(1u, snapshot.logicMirror.selectionOrdinal);
    EXPECT_FALSE(snapshot.resetPending);
}

TEST(QueueRuntime, InvalidCheckpointCapacityFailsClosed) {
    EXPECT_FALSE(harken::QueueCheckpointTable(0).Valid());
    EXPECT_FALSE(harken::QueueCheckpointTable(
                     harken::kMaxQueueCheckpointRowsHard + 1u)
                     .Valid());
}

TEST(QueueRuntime, SequenceOnceAdvancesInCompiledOrderAndStopsAtEnd) {
    harken::ContainerChildProgramViewV2 children[2]{};
    children[0].stableChildOrdinal = 3;
    children[1].stableChildOrdinal = 5;
    harken::ContainerProgramViewV3 program{};
    program.containerStableId = harkentest::ObjectId(13);
    program.programSemanticFingerprint = 17;
    program.mode = harken::ContainerModeV1::SequenceQueue;
    program.queueCycle = harken::QueueCycleV1::Once;
    program.childCount = 2;
    program.children = children;

    harken::EffectiveQueueState current{};
    current.version = 1;
    current.cursor = 0;
    current.currentChildOrdinal = 3;
    current.selectionOrdinal = 9;
    harken::EffectiveQueueState next{};
    ASSERT_EQ(harken::QueueSequenceStepStatus::Successor,
              harken::ResolveSequenceQueueStep(program, current, &next));
    EXPECT_EQ(2u, next.version);
    EXPECT_EQ(1u, next.cursor);
    EXPECT_EQ(5u, next.currentChildOrdinal);
    EXPECT_EQ(0u, next.cycleOrdinal);
    EXPECT_EQ(10u, next.selectionOrdinal);

    current = next;
    next.version = 99;
    EXPECT_EQ(harken::QueueSequenceStepStatus::NaturalEnd,
              harken::ResolveSequenceQueueStep(program, current, &next));
    EXPECT_EQ(99u, next.version);

    current.version = 7;
    current.cursor = 0;
    current.currentChildOrdinal = 3;
    current.selectionOrdinal = UINT64_MAX;
    EXPECT_EQ(harken::QueueSequenceStepStatus::Invalid,
              harken::ResolveSequenceQueueStep(program, current, &next));
}

TEST(QueueRuntime, SequenceFiniteLoopCountWrapsThenStops) {
    harken::ContainerChildProgramViewV2 children[2]{};
    children[0].stableChildOrdinal = 3;
    children[1].stableChildOrdinal = 5;
    harken::ContainerProgramViewV3 program{};
    program.containerStableId = harkentest::ObjectId(13);
    program.programSemanticFingerprint = 17;
    program.mode = harken::ContainerModeV1::SequenceQueue;
    program.queueCycle = harken::QueueCycleV1::Loop;
    program.queueCycleCount = 2;
    program.childCount = 2;
    program.children = children;

    harken::EffectiveQueueState current{};
    current.version = 1;
    current.cursor = 1;
    current.currentChildOrdinal = 5;
    current.selectionOrdinal = 10;
    current.cycleOrdinal = 0;
    harken::EffectiveQueueState next{};
    ASSERT_EQ(harken::QueueSequenceStepStatus::Successor,
              harken::ResolveSequenceQueueStep(program, current, &next));
    EXPECT_EQ(0u, next.cursor);
    EXPECT_EQ(3u, next.currentChildOrdinal);
    EXPECT_EQ(1u, next.cycleOrdinal);

    current = next;
    current.cursor = 1;
    current.currentChildOrdinal = 5;
    EXPECT_EQ(harken::QueueSequenceStepStatus::NaturalEnd,
              harken::ResolveSequenceQueueStep(program, current, &next));
}

TEST(QueueRuntime, RandomPermutationCarriesOneSeedAcrossWeightedCycle) {
    std::array<harken::ContainerChildProgramViewV2, 3> children{};
    children[0].stableChildOrdinal = 3;
    children[0].authoredStableOrder = 0;
    children[0].orderWeight = 1;
    children[1].stableChildOrdinal = 5;
    children[1].authoredStableOrder = 1;
    children[1].orderWeight = 2;
    children[2].stableChildOrdinal = 7;
    children[2].authoredStableOrder = 2;
    children[2].orderWeight = 3;
    harken::ContainerProgramViewV3 program{};
    program.containerStableId = harkentest::ObjectId(13);
    program.programSemanticFingerprint = 17;
    program.mode = harken::ContainerModeV1::RandomQueue;
    program.memoryScope = harken::MemoryScopeV1::Emitter;
    program.avoidLastN = 1;
    program.queueCycle = harken::QueueCycleV1::Loop;
    program.childCount = children.size();
    program.children = children.data();

    harken::RandomQueueSeedContext seed{};
    seed.worldSemanticFingerprint = 19;
    seed.rootStableId = harkentest::ObjectId(23);
    seed.callerSeed = 29;
    seed.acceptedPostOrdinal = 31;
    seed.emitterSlot = 37;
    seed.emitterGeneration = 41;
    seed.resetEpoch = 1;
    harken::EffectiveQueueState current{};
    current.version = 1;
    current.cycleOrdinal = 0;
    current.selectionOrdinal = 1;
    std::array<harken::SelectionChildV1, 3> selectionChildren{};
    std::array<std::uint32_t, 3> remaining{};
    std::array<std::uint32_t, 3> permutation{};
    ASSERT_EQ(harken::SelectionStatusV1::Ok,
              harken::BuildRandomQueuePermutationV1(
                  program, seed, current, selectionChildren.data(),
                  remaining.data(), permutation.data(),
                  permutation.size()));
    current.currentChildOrdinal = permutation[0];

    const auto firstCycle = permutation;
    EXPECT_EQ((std::array<std::uint32_t, 3>{7, 3, 5}), firstCycle);
    harken::EffectiveQueueState next{};
    ASSERT_EQ(harken::QueueSequenceStepStatus::Successor,
              harken::ResolveRandomQueueStep(
                  program, seed, current, selectionChildren.data(),
                  remaining.data(), permutation.data(),
                  permutation.size(), &next));
    EXPECT_EQ(1u, next.selectionOrdinal);
    EXPECT_EQ(0u, next.cycleOrdinal);
    EXPECT_EQ(firstCycle[1], next.currentChildOrdinal);
    current = next;
    ASSERT_EQ(harken::QueueSequenceStepStatus::Successor,
              harken::ResolveRandomQueueStep(
                  program, seed, current, selectionChildren.data(),
                  remaining.data(), permutation.data(),
                  permutation.size(), &next));
    EXPECT_EQ(firstCycle[2], next.currentChildOrdinal);
    current = next;
    ASSERT_EQ(harken::QueueSequenceStepStatus::Successor,
              harken::ResolveRandomQueueStep(
                  program, seed, current, selectionChildren.data(),
                  remaining.data(), permutation.data(),
                  permutation.size(), &next));
    EXPECT_EQ(2u, next.selectionOrdinal);
    EXPECT_EQ(1u, next.cycleOrdinal);
    EXPECT_EQ(7u, next.currentChildOrdinal);
    EXPECT_NE(firstCycle[2], next.currentChildOrdinal);
    std::array<std::uint32_t, 3> secondCycle{};
    ASSERT_EQ(harken::SelectionStatusV1::Ok,
              harken::BuildRandomQueuePermutationV1(
                  program, seed, next, selectionChildren.data(),
                  remaining.data(), secondCycle.data(), secondCycle.size()));
    EXPECT_EQ((std::array<std::uint32_t, 3>{7, 5, 3}), secondCycle);

    std::array<std::uint32_t, 3> replay{};
    harken::EffectiveQueueState replayState{};
    replayState.version = 1;
    replayState.selectionOrdinal = 1;
    ASSERT_EQ(harken::SelectionStatusV1::Ok,
              harken::BuildRandomQueuePermutationV1(
                  program, seed, replayState, selectionChildren.data(),
                  remaining.data(), replay.data(), replay.size()));
    EXPECT_EQ(firstCycle, replay);
}

TEST(QueueRuntime, RandomAvoidWindowIsPartOfAtomicEffectiveState) {
    harken::QueueInstanceTable table(1);
    harken::QueueInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = 11;
    create.containerStableId = harkentest::ObjectId(13);
    create.programSemanticFingerprint = 17;
    create.childCount = 3;
    create.initial.version = 1;
    create.initial.currentChildOrdinal = 3;
    create.initial.selectionOrdinal = 2;
    create.initial.randomInitialRecentCount = 2;
    create.initial.randomInitialRecent[0] = 5;
    create.initial.randomInitialRecent[1] = 7;
    harken::QueueInstanceToken queue{};
    ASSERT_TRUE(table.Acquire(create, &queue));

    harken::EffectiveQueueState effective{};
    ASSERT_TRUE(table.ResolveEffective(queue, &effective));
    EXPECT_EQ(2u, effective.randomInitialRecentCount);
    EXPECT_EQ(5u, effective.randomInitialRecent[0]);
    EXPECT_EQ(7u, effective.randomInitialRecent[1]);

    harken::EffectiveQueueState next = effective;
    next.version = 2;
    next.currentChildOrdinal = 5;
    next.randomInitialRecent[0] = 7;
    next.randomInitialRecent[1] = 3;
    ASSERT_TRUE(table.AdvanceAfterSignalCompletion(queue, 1, next));
    ASSERT_TRUE(table.ResolveEffective(queue, &effective));
    EXPECT_EQ(2u, effective.randomInitialRecentCount);
    EXPECT_EQ(7u, effective.randomInitialRecent[0]);
    EXPECT_EQ(3u, effective.randomInitialRecent[1]);
}

TEST(QueueRuntime, RandomQueueRequiresExactEmitterAuthorityAndNeverWraps) {
    std::array<harken::ContainerChildProgramViewV2, 2> children{};
    children[0].stableChildOrdinal = 3;
    children[0].orderWeight = 1;
    children[1].stableChildOrdinal = 5;
    children[1].authoredStableOrder = 1;
    children[1].orderWeight = 2;
    harken::ContainerProgramViewV3 program{};
    program.containerStableId = harkentest::ObjectId(13);
    program.programSemanticFingerprint = 17;
    program.mode = harken::ContainerModeV1::RandomQueue;
    program.memoryScope = harken::MemoryScopeV1::Global;
    program.queueCycle = harken::QueueCycleV1::Loop;
    program.childCount = children.size();
    program.children = children.data();
    harken::RandomQueueSeedContext seed{};
    seed.worldSemanticFingerprint = 19;
    seed.rootStableId = harkentest::ObjectId(23);
    seed.acceptedPostOrdinal = 29;
    seed.emitterSlot = 31;
    seed.emitterGeneration = 37;
    seed.resetEpoch = 1;
    harken::EffectiveQueueState state{};
    state.version = 1;
    state.selectionOrdinal = 1;
    std::array<harken::SelectionChildV1, 2> selectionChildren{};
    std::array<std::uint32_t, 2> remaining{};
    std::array<std::uint32_t, 2> permutation = {41, 43};
    seed.emitterSlot = UINT32_MAX;
    seed.emitterGeneration = 0;

    EXPECT_EQ(harken::SelectionStatusV1::InvalidInput,
              harken::BuildRandomQueuePermutationV1(
                  program, seed, state, selectionChildren.data(),
                  remaining.data(), permutation.data(), permutation.size()));
    EXPECT_EQ((std::array<std::uint32_t, 2>{41, 43}), permutation);

    program.memoryScope = harken::MemoryScopeV1::Emitter;
    seed.emitterSlot = 31;
    seed.emitterGeneration = 37;
    ASSERT_EQ(harken::SelectionStatusV1::Ok,
              harken::BuildRandomQueuePermutationV1(
                  program, seed, state, selectionChildren.data(),
                  remaining.data(), permutation.data(), permutation.size()));
    state.cursor = 1;
    state.currentChildOrdinal = permutation[1];
    state.selectionOrdinal = UINT64_MAX;
    harken::EffectiveQueueState unchanged{};
    unchanged.version = 47;
    EXPECT_EQ(harken::QueueSequenceStepStatus::Invalid,
              harken::ResolveRandomQueueStep(
                  program, seed, state, selectionChildren.data(),
                  remaining.data(), permutation.data(), permutation.size(),
                  &unchanged));
    EXPECT_EQ(47u, unchanged.version);

    seed.resetEpoch = 0;
    permutation = {53, 59};
    EXPECT_EQ(harken::SelectionStatusV1::InvalidInput,
              harken::BuildRandomQueuePermutationV1(
                  program, seed, state, selectionChildren.data(),
                  remaining.data(), permutation.data(), permutation.size()));
    EXPECT_EQ((std::array<std::uint32_t, 2>{53, 59}), permutation);
}

TEST(QueueRuntime, FailureKeepsLastEffectiveStateAndRecordsOneReason) {
    harken::QueueInstanceTable table(1);
    harken::QueueInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = 11;
    create.containerStableId = harkentest::ObjectId(13);
    create.programSemanticFingerprint = 17;
    create.childCount = 2;
    create.initial.cursor = 0;
    create.initial.currentChildOrdinal = 3;
    create.initial.version = 1;
    harken::QueueInstanceToken queue{};
    ASSERT_TRUE(table.Acquire(create, &queue));

    harken::ConditionalQueuePrepareRequest prepare{};
    prepare.expectedCurrentVersion = 1;
    prepare.rootDecision = {19, 23};
    prepare.expectedCurrentOccurrence = {29, 31};
    prepare.successorOccurrence = {37, 41};
    prepare.completion = BoundPredicate(prepare.rootDecision);
    prepare.next.cursor = 1;
    prepare.next.currentChildOrdinal = 5;
    prepare.next.version = 2;
    harken::ConditionalQueueVersionToken conditional{};
    ASSERT_TRUE(table.PrepareConditional(queue, prepare, &conditional));

    EXPECT_EQ(harken::QueueFailureDisposition::NewlyFailed,
              table.FailContainer(
                  queue, conditional,
                  harken::ConditionalQueueActivation::InvalidatedIdentity,
                  harken::QueueFailureReason::OccurrenceLost, 101));
    EXPECT_EQ(harken::QueueFailureDisposition::AlreadyFailed,
              table.FailContainer(
                  queue, conditional,
                  harken::ConditionalQueueActivation::InvalidatedDeadline,
                  harken::QueueFailureReason::Deadline, 103));

    harken::QueueInstanceSnapshot snapshot{};
    ASSERT_TRUE(table.Capture(queue, &snapshot));
    EXPECT_EQ(harken::QueueFailurePhase::FailingSourceActive,
              snapshot.failurePhase);
    EXPECT_EQ(harken::QueueFailureReason::OccurrenceLost,
              snapshot.failureReason);
    EXPECT_EQ(101u, snapshot.failureFrame);
    EXPECT_EQ(harken::ConditionalQueueActivation::InvalidatedIdentity,
              snapshot.pendingActivation);
    harken::EffectiveQueueState state{};
    ASSERT_TRUE(table.ResolveEffective(queue, &state));
    EXPECT_EQ(1u, state.version);
    EXPECT_EQ(3u, state.currentChildOrdinal);
}

TEST(QueueRuntime, TargetPreparationFailureUsesSameSingleTerminalBeforeVersion) {
    harken::QueueInstanceTable table(1);
    harken::QueueInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = 11;
    create.containerStableId = harkentest::ObjectId(13);
    create.programSemanticFingerprint = 17;
    create.childCount = 2;
    create.initial.version = 1;
    create.initial.currentChildOrdinal = 3;
    harken::QueueInstanceToken queue{};
    ASSERT_TRUE(table.Acquire(create, &queue));

    EXPECT_EQ(harken::QueueFailureDisposition::NewlyFailed,
              table.FailContainerBeforeConditional(
                  queue,
                  harken::QueueFailureReason::TargetPreparationFailed,
                  101));
    EXPECT_EQ(harken::QueueFailureDisposition::AlreadyFailed,
              table.FailContainerBeforeConditional(
                  queue,
                  harken::QueueFailureReason::TargetPreparationFailed,
                  103));
    harken::QueueInstanceSnapshot snapshot{};
    ASSERT_TRUE(table.Capture(queue, &snapshot));
    EXPECT_EQ(harken::QueueFailurePhase::FailingSourceActive,
              snapshot.failurePhase);
    EXPECT_EQ(harken::QueueFailureReason::TargetPreparationFailed,
              snapshot.failureReason);
    EXPECT_EQ(101u, snapshot.failureFrame);
}

TEST(QueueRuntime, ExactProgressProofActivatesPreparedSuccessor) {
    harken::PlayableInstanceTable instances(1);
    const auto root = instances.Acquire(harken::PlayableKind::Container, 0);
    ASSERT_NE(0u, root);
    harken::OccurrenceStatePool occurrences(1);
    harken::OccurrenceToken occurrence{};
    ASSERT_TRUE(occurrences.Acquire({root, harkentest::ObjectId(47), 0, 3}, &occurrence));
    ASSERT_TRUE(occurrences.BindCompletionProgress(occurrence, 43, 1));

    const harken::DecisionToken decision{19, 23};
    const auto owner = harken::CompletionOwnerRef::Single(decision, 1);
    ASSERT_TRUE(occurrences.TryPinCompletionOwner(occurrence, owner));
    harken::CompletionProgressBlockUpdate update{};
    update.occurrence = occurrence;
    update.renderGeneration = 43;
    update.sourceOutputFramesThisBlock = 256;
    update.playbackFactsRevision = 1;
    update.completedThroughBlockSequence = 1;
    ASSERT_EQ(harken::CompletionProgressPublishStatus::Ok,
              harken::PublishCompletionProgressBlock(
                  *occurrences.CompletionProgressOf(occurrence), update));

    harken::QueueInstanceTable table(1);
    harken::QueueInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = root;
    create.containerStableId = harkentest::ObjectId(13);
    create.programSemanticFingerprint = 17;
    create.childCount = 2;
    create.initial.version = 1;
    create.initial.currentChildOrdinal = 3;
    harken::QueueInstanceToken queue{};
    ASSERT_TRUE(table.Acquire(create, &queue));

    harken::ConditionalQueuePrepareRequest prepare{};
    prepare.expectedCurrentVersion = 1;
    prepare.rootDecision = decision;
    prepare.expectedCurrentOccurrence = occurrence;
    prepare.successorOccurrence = {37, 41};
    prepare.completion = BoundPredicate(decision, occurrence);
    prepare.next.version = 2;
    prepare.next.cursor = 1;
    prepare.next.currentChildOrdinal = 5;
    harken::ConditionalQueueVersionToken conditional{};
    ASSERT_TRUE(table.PrepareConditional(queue, prepare, &conditional));

    EXPECT_EQ(harken::QueueConditionalProofStatus::ReadyToActivate,
              table.EvaluateConditionalForQuantum(
                  queue, conditional, occurrences, 256, 512, 1, true));
    harken::EffectiveQueueState effective{};
    ASSERT_TRUE(table.ResolveEffective(queue, &effective));
    EXPECT_EQ(2u, effective.version);
    EXPECT_EQ(5u, effective.currentChildOrdinal);
}

TEST(QueueRuntime, ProgressLossFailsOnceWithoutAdvancingEffectiveState) {
    harken::OccurrenceStatePool occurrences(1);
    harken::OccurrenceToken occurrence{};
    ASSERT_TRUE(occurrences.Acquire({11, harkentest::ObjectId(47), 0, 3}, &occurrence));
    ASSERT_TRUE(occurrences.BindCompletionProgress(occurrence, 43, 1));
    const harken::DecisionToken decision{19, 23};
    ASSERT_TRUE(occurrences.TryPinCompletionOwner(
        occurrence, harken::CompletionOwnerRef::Single(decision, 1)));
    harken::CompletionProgressBlockUpdate update{};
    update.occurrence = occurrence;
    update.renderGeneration = 43;
    update.sourceOutputFramesThisBlock = 128;
    update.playbackFactsRevision = 1;
    update.completedThroughBlockSequence = 1;
    ASSERT_EQ(harken::CompletionProgressPublishStatus::Ok,
              harken::PublishCompletionProgressBlock(
                  *occurrences.CompletionProgressOf(occurrence), update));

    harken::QueueInstanceTable table(1);
    harken::QueueInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = 11;
    create.containerStableId = harkentest::ObjectId(13);
    create.programSemanticFingerprint = 17;
    create.childCount = 2;
    create.initial.version = 1;
    create.initial.currentChildOrdinal = 3;
    harken::QueueInstanceToken queue{};
    ASSERT_TRUE(table.Acquire(create, &queue));
    harken::ConditionalQueuePrepareRequest prepare{};
    prepare.expectedCurrentVersion = 1;
    prepare.rootDecision = decision;
    prepare.expectedCurrentOccurrence = occurrence;
    prepare.successorOccurrence = {37, 41};
    prepare.completion = BoundPredicate(decision, occurrence);
    prepare.next.version = 2;
    prepare.next.currentChildOrdinal = 5;
    harken::ConditionalQueueVersionToken conditional{};
    ASSERT_TRUE(table.PrepareConditional(queue, prepare, &conditional));

    EXPECT_EQ(harken::QueueConditionalProofStatus::FailedProgress,
              table.EvaluateConditionalForQuantum(
                  queue, conditional, occurrences, 256, 512, 1, true));
    EXPECT_EQ(harken::QueueConditionalProofStatus::Stale,
              table.EvaluateConditionalForQuantum(
                  queue, conditional, occurrences, 256, 512, 1, true));
    harken::QueueInstanceSnapshot snapshot{};
    ASSERT_TRUE(table.Capture(queue, &snapshot));
    EXPECT_EQ(harken::QueueFailureReason::RenderProgressLost,
              snapshot.failureReason);
    EXPECT_EQ(harken::ConditionalQueueActivation::InvalidatedProgress,
              snapshot.pendingActivation);
    harken::EffectiveQueueState effective{};
    ASSERT_TRUE(table.ResolveEffective(queue, &effective));
    EXPECT_EQ(1u, effective.version);
    EXPECT_EQ(3u, effective.currentChildOrdinal);
}

TEST(QueueRuntime, MissedDeadlineFailsOnceWithoutAdvancingEffectiveState) {
    harken::OccurrenceStatePool occurrences(1);
    harken::OccurrenceToken occurrence{};
    ASSERT_TRUE(occurrences.Acquire({11, harkentest::ObjectId(47), 0, 3}, &occurrence));
    ASSERT_TRUE(occurrences.BindCompletionProgress(occurrence, 43, 1));
    const harken::DecisionToken decision{19, 23};
    ASSERT_TRUE(occurrences.TryPinCompletionOwner(
        occurrence, harken::CompletionOwnerRef::Single(decision, 1)));
    harken::CompletionProgressBlockUpdate update{};
    update.occurrence = occurrence;
    update.renderGeneration = 43;
    update.sourceOutputFramesThisBlock = 300;
    update.playbackFactsRevision = 1;
    update.completedThroughBlockSequence = 1;
    ASSERT_EQ(harken::CompletionProgressPublishStatus::Ok,
              harken::PublishCompletionProgressBlock(
                  *occurrences.CompletionProgressOf(occurrence), update));

    harken::QueueInstanceTable table(1);
    harken::QueueInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = 11;
    create.containerStableId = harkentest::ObjectId(13);
    create.programSemanticFingerprint = 17;
    create.childCount = 2;
    create.initial.version = 1;
    create.initial.currentChildOrdinal = 3;
    harken::QueueInstanceToken queue{};
    ASSERT_TRUE(table.Acquire(create, &queue));
    harken::ConditionalQueuePrepareRequest prepare{};
    prepare.expectedCurrentVersion = 1;
    prepare.rootDecision = decision;
    prepare.expectedCurrentOccurrence = occurrence;
    prepare.successorOccurrence = {37, 41};
    prepare.completion = BoundPredicate(decision, occurrence);
    prepare.next.version = 2;
    prepare.next.currentChildOrdinal = 5;
    harken::ConditionalQueueVersionToken conditional{};
    ASSERT_TRUE(table.PrepareConditional(queue, prepare, &conditional));

    EXPECT_EQ(harken::QueueConditionalProofStatus::MissedDeadline,
              table.EvaluateConditionalForQuantum(
                  queue, conditional, occurrences, 512, 768, 1, true));
    EXPECT_EQ(harken::QueueConditionalProofStatus::Stale,
              table.EvaluateConditionalForQuantum(
                  queue, conditional, occurrences, 512, 768, 1, true));
    harken::QueueInstanceSnapshot snapshot{};
    ASSERT_TRUE(table.Capture(queue, &snapshot));
    EXPECT_EQ(harken::QueueFailureReason::Deadline,
              snapshot.failureReason);
    EXPECT_EQ(harken::ConditionalQueueActivation::InvalidatedDeadline,
              snapshot.pendingActivation);
    harken::EffectiveQueueState effective{};
    ASSERT_TRUE(table.ResolveEffective(queue, &effective));
    EXPECT_EQ(1u, effective.version);
    EXPECT_EQ(3u, effective.currentChildOrdinal);
}

TEST(QueueRuntime,
     RetiredSourceCompletionProvesExplicitActivationOutcome) {
    harken::OccurrenceStatePool occurrences(2);
    harken::OccurrenceToken current{};
    harken::OccurrenceToken successor{};
    ASSERT_TRUE(occurrences.Acquire({11, harkentest::ObjectId(47), 0, 3}, &current));
    ASSERT_TRUE(occurrences.Acquire({11, harkentest::ObjectId(59), 0, 5}, &successor));
    ASSERT_TRUE(occurrences.BindCompletionProgress(current, 43, 1));
    const harken::DecisionToken decision{19, 23};
    ASSERT_TRUE(occurrences.TryPinCompletionOwner(
        current, harken::CompletionOwnerRef::Single(decision, 1)));
    harken::CompletionProgressBlockUpdate update{};
    update.occurrence = current;
    update.renderGeneration = 43;
    update.sourceOutputFramesThisBlock = 300;
    update.playbackFactsRevision = 1;
    update.completedThroughBlockSequence = 1;
    ASSERT_EQ(harken::CompletionProgressPublishStatus::Ok,
              harken::PublishCompletionProgressBlock(
                  *occurrences.CompletionProgressOf(current), update));

    harken::QueueInstanceTable table(1);
    harken::QueueInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = 11;
    create.containerStableId = harkentest::ObjectId(13);
    create.programSemanticFingerprint = 17;
    create.childCount = 2;
    create.initial.version = 1;
    create.initial.currentChildOrdinal = 3;
    harken::QueueInstanceToken queue{};
    ASSERT_TRUE(table.Acquire(create, &queue));

    harken::ConditionalQueuePrepareRequest prepare{};
    prepare.expectedCurrentVersion = 1;
    prepare.rootDecision = decision;
    prepare.expectedCurrentOccurrence = current;
    prepare.successorOccurrence = successor;
    prepare.completion = BoundPredicate(decision, current);
    prepare.next.version = 2;
    prepare.next.currentChildOrdinal = 5;
    prepare.retiredSource.sourceArm = {7, 11, 9, 43};
    prepare.retiredSource.sourceRenderHandle =
        (UINT64_C(43) << 32) | 9u;
    prepare.retiredSource.logicalCompletionSequence = 1;
    prepare.retiredSource.tombstoneAudioSequence = 1;
    harken::ConditionalQueueVersionToken conditional{};
    ASSERT_TRUE(table.PrepareConditional(queue, prepare, &conditional));

    harken::QueueConditionalProof proof{};
    EXPECT_EQ(
        harken::QueueConditionalProofStatus::ReadyToActivateRetiredSource,
        table.ProveConditionalForQuantum(
            queue, conditional, occurrences, 512, 768, 1, true, &proof));
    EXPECT_EQ(harken::QueueConditionalCompletionMode::RetiredSource,
              proof.completionMode);
    EXPECT_EQ(successor.slot, proof.successorOccurrence.slot);
    EXPECT_EQ(successor.generation, proof.successorOccurrence.generation);
    EXPECT_EQ(prepare.retiredSource.sourceArm.slot,
              proof.retiredSourceArm.slot);
    EXPECT_EQ(prepare.retiredSource.sourceRenderHandle,
              proof.retiredSourceRenderHandle);
    EXPECT_EQ(prepare.retiredSource.logicalCompletionSequence,
              proof.retiredSourceLogicalCompletionSequence);
    EXPECT_EQ(prepare.retiredSource.tombstoneAudioSequence,
              proof.retiredSourceTombstoneAudioSequence);
    EXPECT_TRUE(table.ActivateProvenConditional(proof));
    harken::EffectiveQueueState effective{};
    ASSERT_TRUE(table.ResolveEffective(queue, &effective));
    EXPECT_EQ(2u, effective.version);
    EXPECT_EQ(5u, effective.currentChildOrdinal);
}

TEST(QueueRuntime,
     RetiredSignalSourceRequiresFencedOccurrenceAndProvesActivation) {
    harken::OccurrenceStatePool occurrences(2);
    harken::OccurrenceToken current{};
    harken::OccurrenceToken successor{};
    ASSERT_TRUE(occurrences.Acquire(
        {11, harkentest::ObjectId(47), 0, 3}, &current));
    ASSERT_TRUE(occurrences.Acquire(
        {11, harkentest::ObjectId(59), 0, 5}, &successor));
    const harken::DecisionToken decision{19, 23};

    harken::QueueInstanceTable table(1);
    harken::QueueInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = 11;
    create.containerStableId = harkentest::ObjectId(13);
    create.programSemanticFingerprint = 17;
    create.childCount = 2;
    create.initial.version = 1;
    create.initial.currentChildOrdinal = 3;
    harken::QueueInstanceToken queue{};
    ASSERT_TRUE(table.Acquire(create, &queue));

    harken::ConditionalQueuePrepareRequest prepare{};
    prepare.expectedCurrentVersion = 1;
    prepare.rootDecision = decision;
    prepare.expectedCurrentOccurrence = current;
    prepare.successorOccurrence = successor;
    prepare.next.version = 2;
    prepare.next.currentChildOrdinal = 5;
    prepare.retiredSource.sourceArm = {7, 11, 9, 43};
    prepare.retiredSource.sourceRenderHandle =
        (UINT64_C(43) << 32) | 9u;
    prepare.retiredSource.logicalCompletionSequence = 1;
    prepare.retiredSource.tombstoneAudioSequence = 1;
    harken::ConditionalQueueVersionToken conditional{};
    ASSERT_TRUE(table.PrepareConditional(queue, prepare, &conditional));

    harken::QueueConditionalProof proof{};
    EXPECT_EQ(harken::QueueConditionalProofStatus::FailedIdentity,
              table.ProveConditionalForQuantum(
                  queue, conditional, occurrences, 512, 768, 1, true,
                  &proof));
    ASSERT_TRUE(occurrences.Advance(
        current, harken::OccurrenceStage::Active));
    ASSERT_TRUE(occurrences.Advance(
        current, harken::OccurrenceStage::LogicalDone));
    ASSERT_TRUE(occurrences.Advance(
        current, harken::OccurrenceStage::AwaitingEventFence));
    EXPECT_EQ(
        harken::QueueConditionalProofStatus::ReadyToActivateRetiredSource,
        table.ProveConditionalForQuantum(
            queue, conditional, occurrences, 512, 768, 1, true, &proof));
    EXPECT_EQ(harken::QueueConditionalCompletionMode::RetiredSource,
              proof.completionMode);
    ASSERT_TRUE(table.ActivateProvenConditional(proof));
    ASSERT_TRUE(table.SettleActivated(queue, conditional));
    harken::EffectiveQueueState effective{};
    ASSERT_TRUE(table.ResolveEffective(queue, &effective));
    EXPECT_EQ(2u, effective.version);
    EXPECT_EQ(5u, effective.currentChildOrdinal);
}

TEST(QueueRuntime, RetiredSourceOwnerMismatchStillFailsIdentity) {
    harken::OccurrenceStatePool occurrences(2);
    harken::OccurrenceToken current{};
    harken::OccurrenceToken successor{};
    ASSERT_TRUE(occurrences.Acquire({11, harkentest::ObjectId(47), 0, 3}, &current));
    ASSERT_TRUE(occurrences.Acquire({11, harkentest::ObjectId(59), 0, 5}, &successor));
    ASSERT_TRUE(occurrences.BindCompletionProgress(current, 43, 1));
    const harken::DecisionToken decision{19, 23};
    const harken::DecisionToken differentOwner{17, 21};
    ASSERT_TRUE(occurrences.TryPinCompletionOwner(
        current, harken::CompletionOwnerRef::Single(differentOwner, 1)));
    harken::CompletionProgressBlockUpdate update{};
    update.occurrence = current;
    update.renderGeneration = 43;
    update.sourceOutputFramesThisBlock = 300;
    update.playbackFactsRevision = 1;
    update.completedThroughBlockSequence = 1;
    ASSERT_EQ(harken::CompletionProgressPublishStatus::Ok,
              harken::PublishCompletionProgressBlock(
                  *occurrences.CompletionProgressOf(current), update));

    harken::QueueInstanceTable table(1);
    harken::QueueInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = 11;
    create.containerStableId = harkentest::ObjectId(13);
    create.programSemanticFingerprint = 17;
    create.childCount = 2;
    create.initial.version = 1;
    create.initial.currentChildOrdinal = 3;
    harken::QueueInstanceToken queue{};
    ASSERT_TRUE(table.Acquire(create, &queue));
    harken::ConditionalQueuePrepareRequest prepare{};
    prepare.expectedCurrentVersion = 1;
    prepare.rootDecision = decision;
    prepare.expectedCurrentOccurrence = current;
    prepare.successorOccurrence = successor;
    prepare.completion = BoundPredicate(decision, current);
    prepare.next.version = 2;
    prepare.next.currentChildOrdinal = 5;
    prepare.retiredSource.sourceArm = {7, 11, 9, 43};
    prepare.retiredSource.sourceRenderHandle =
        (UINT64_C(43) << 32) | 9u;
    prepare.retiredSource.logicalCompletionSequence = 1;
    prepare.retiredSource.tombstoneAudioSequence = 1;
    harken::ConditionalQueueVersionToken conditional{};
    ASSERT_TRUE(table.PrepareConditional(queue, prepare, &conditional));

    EXPECT_EQ(harken::QueueConditionalProofStatus::FailedIdentity,
              table.EvaluateConditionalForQuantum(
                  queue, conditional, occurrences, 512, 768, 1, true));
    harken::QueueInstanceSnapshot snapshot{};
    ASSERT_TRUE(table.Capture(queue, &snapshot));
    EXPECT_EQ(harken::ConditionalQueueActivation::InvalidatedIdentity,
              snapshot.pendingActivation);
    harken::EffectiveQueueState effective{};
    ASSERT_TRUE(table.ResolveEffective(queue, &effective));
    EXPECT_EQ(1u, effective.version);
    EXPECT_EQ(3u, effective.currentChildOrdinal);
}

TEST(QueueRuntime, RetiredSourceProgressMismatchStillFailsProgress) {
    harken::OccurrenceStatePool occurrences(2);
    harken::OccurrenceToken current{};
    harken::OccurrenceToken successor{};
    ASSERT_TRUE(occurrences.Acquire({11, harkentest::ObjectId(47), 0, 3}, &current));
    ASSERT_TRUE(occurrences.Acquire({11, harkentest::ObjectId(59), 0, 5}, &successor));
    ASSERT_TRUE(occurrences.BindCompletionProgress(current, 43, 1));
    const harken::DecisionToken decision{19, 23};
    ASSERT_TRUE(occurrences.TryPinCompletionOwner(
        current, harken::CompletionOwnerRef::Single(decision, 1)));
    harken::CompletionProgressBlockUpdate update{};
    update.occurrence = current;
    update.renderGeneration = 43;
    update.sourceOutputFramesThisBlock = 299;
    update.playbackFactsRevision = 1;
    update.completedThroughBlockSequence = 1;
    ASSERT_EQ(harken::CompletionProgressPublishStatus::Ok,
              harken::PublishCompletionProgressBlock(
                  *occurrences.CompletionProgressOf(current), update));

    harken::QueueInstanceTable table(1);
    harken::QueueInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = 11;
    create.containerStableId = harkentest::ObjectId(13);
    create.programSemanticFingerprint = 17;
    create.childCount = 2;
    create.initial.version = 1;
    create.initial.currentChildOrdinal = 3;
    harken::QueueInstanceToken queue{};
    ASSERT_TRUE(table.Acquire(create, &queue));
    harken::ConditionalQueuePrepareRequest prepare{};
    prepare.expectedCurrentVersion = 1;
    prepare.rootDecision = decision;
    prepare.expectedCurrentOccurrence = current;
    prepare.successorOccurrence = successor;
    prepare.completion = BoundPredicate(decision, current);
    prepare.next.version = 2;
    prepare.next.currentChildOrdinal = 5;
    prepare.retiredSource.sourceArm = {7, 11, 9, 43};
    prepare.retiredSource.sourceRenderHandle =
        (UINT64_C(43) << 32) | 9u;
    prepare.retiredSource.logicalCompletionSequence = 1;
    prepare.retiredSource.tombstoneAudioSequence = 1;
    harken::ConditionalQueueVersionToken conditional{};
    ASSERT_TRUE(table.PrepareConditional(queue, prepare, &conditional));

    EXPECT_EQ(harken::QueueConditionalProofStatus::FailedProgress,
              table.EvaluateConditionalForQuantum(
                  queue, conditional, occurrences, 512, 768, 1, true));
    harken::QueueInstanceSnapshot snapshot{};
    ASSERT_TRUE(table.Capture(queue, &snapshot));
    EXPECT_EQ(harken::ConditionalQueueActivation::InvalidatedProgress,
              snapshot.pendingActivation);
    harken::EffectiveQueueState effective{};
    ASSERT_TRUE(table.ResolveEffective(queue, &effective));
    EXPECT_EQ(1u, effective.version);
    EXPECT_EQ(3u, effective.currentChildOrdinal);
}

void ExpectRetiredSourceAudioSlice(std::uint32_t memberCount, bool invalidateLast = false) {
    harken::EngineConfig config;
    harken::VoiceCapacityModel capacity;
    ASSERT_TRUE(harken::VoiceCapacityModel::TryCreate(3, 0, &capacity));
    config.voiceCapacity = capacity;
    harken::ResampleTables tables;
    tables.Init();
    harken::MultirateResampleTables multirateTables;
    multirateTables.Init();
    harken::BiquadTrigTable biquadTrig;
    harken::EffectPoolBank effects(config);
    harken::DynamicRouteSystem routes(config);
    harken::TimelineArmTable arms(3);
    harken::VoicePool voices(config, &tables, &multirateTables, &biquadTrig,
                            effects, routes, &arms);

    float sample = 0.0f;
    harken::PCMView pcm{};
    pcm.channels[0] = &sample;
    pcm.channelCount = 1;
    pcm.stride = 1;
    pcm.frames = 1;
    pcm.sampleRate = 48000;
    VnVoiceHandle sourceRender = 0;
    VnVoiceHandle successorRender = 0;
    ASSERT_EQ(VN_OK, voices.Play(
                         pcm, 0.0f, 1.0, 0, false, 0.0f, 0.0f, 0.0f,
                         {}, 0, 1, 1,
                         harken::ResampleProfile::LegacyCompatibility,
                         &sourceRender));
    ASSERT_EQ(VN_OK, voices.Play(
                         pcm, 0.0f, 1.0, 0, false, 0.0f, 0.0f, 0.0f,
                         {}, 0, 1, 1,
                         harken::ResampleProfile::LegacyCompatibility,
                         &successorRender));

    harken::PlayableInstanceTable instances(1);
    const auto root = instances.Acquire(harken::PlayableKind::Container, 0);
    harken::OccurrenceStatePool occurrences(2);
    harken::OccurrenceToken current{};
    harken::OccurrenceToken successor{};
    ASSERT_TRUE(occurrences.Acquire({root, harkentest::ObjectId(47), 0, 3}, &current));
    ASSERT_TRUE(occurrences.Acquire({root, harkentest::ObjectId(59), 0, 5}, &successor));
    ASSERT_TRUE(occurrences.BindCompletionProgress(
        current, static_cast<std::uint32_t>(sourceRender >> 32), 1));
    harken::CompletionProgressBlockUpdate update{};
    update.occurrence = current;
    update.renderGeneration = static_cast<std::uint32_t>(sourceRender >> 32);
    update.sourceOutputFramesThisBlock = 300;
    update.playbackFactsRevision = 1;
    update.completedThroughBlockSequence = 1;
    ASSERT_EQ(harken::CompletionProgressPublishStatus::Ok,
              harken::PublishCompletionProgressBlock(
                  *occurrences.CompletionProgressOf(current), update));

    harken::DecisionTable decisions(1, 2);
    harken::DecisionReserveRequest decisionRequest{};
    decisionRequest.ticket.sequence = 1;
    decisionRequest.world = 7;
    decisionRequest.transport = 3;
    decisionRequest.expectedContinuationGeneration = 1;
    decisionRequest.sliceCount = 1;
    harken::DecisionToken decision{};
    ASSERT_TRUE(decisions.Reserve(decisionRequest, &decision));
    ASSERT_TRUE(occurrences.TryPinCompletionOwner(
        current, harken::CompletionOwnerRef::Single(decision, 1)));

    harken::QueueInstanceTable queues(1);
    harken::QueueInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = root;
    create.containerStableId = harkentest::ObjectId(13);
    create.programSemanticFingerprint = 17;
    create.childCount = 2;
    create.initial.version = 1;
    create.initial.currentChildOrdinal = 3;
    harken::QueueInstanceToken queue{};
    ASSERT_TRUE(queues.Acquire(create, &queue));

    ASSERT_TRUE(occurrences.Advance(
        current, harken::OccurrenceStage::Active));
    ASSERT_TRUE(occurrences.Advance(
        current, harken::OccurrenceStage::LogicalDone));
    ASSERT_TRUE(occurrences.Advance(
        current, harken::OccurrenceStage::AwaitingEventFence));

    harken::TimelineSemanticTarget sourceTarget{};
    sourceTarget.programSemanticFingerprint = 17;
    sourceTarget.subjectId = harkentest::ObjectId(47);
    harken::TimelineArmToken sourceArm{};
    ASSERT_TRUE(arms.Reserve(
        static_cast<std::uint32_t>(sourceRender),
        static_cast<std::uint32_t>(sourceRender >> 32), 0, 0, root,
        sourceTarget, &sourceArm));
    ASSERT_TRUE(arms.AcknowledgeApplied(sourceArm, 1));
    ASSERT_TRUE(arms.Publish(sourceArm));
    ASSERT_TRUE(arms.Start(sourceArm, 1));
    ASSERT_TRUE(arms.AcknowledgeLogicalComplete(sourceArm, 1));
    ASSERT_TRUE(arms.Tombstone(sourceArm, 1));
    ASSERT_TRUE(voices.CancelTimelineBinding(
        static_cast<std::uint32_t>(sourceRender),
        static_cast<std::uint32_t>(sourceRender >> 32)));

    harken::ConditionalQueuePrepareRequest prepare{};
    prepare.expectedCurrentVersion = 1;
    prepare.rootDecision = decision;
    prepare.expectedCurrentOccurrence = current;
    prepare.successorOccurrence = successor;
    prepare.completion = BoundPredicate(
        decision, current,
        static_cast<std::uint32_t>(sourceRender >> 32));
    prepare.next.version = 2;
    prepare.next.cursor = 1;
    prepare.next.currentChildOrdinal = 5;
    prepare.retiredSource.sourceArm = sourceArm;
    prepare.retiredSource.sourceRenderHandle = sourceRender;
    prepare.retiredSource.logicalCompletionSequence = 1;
    prepare.retiredSource.tombstoneAudioSequence = 1;
    harken::ConditionalQueueVersionToken conditional{};
    ASSERT_TRUE(queues.PrepareConditional(queue, prepare, &conditional));

    harken::TimelineSemanticTarget successorTarget{};
    successorTarget.programSemanticFingerprint = 17;
    successorTarget.subjectId = harkentest::ObjectId(59);
    harken::TimelineArmToken successorArm{};
    ASSERT_TRUE(arms.ReserveGrouped(
        static_cast<std::uint32_t>(successorRender),
        static_cast<std::uint32_t>(successorRender >> 32), 512, 512, root,
        successorTarget, {decision}, 0, &successorArm));
    voices.ApplyTimelineGatedPlay(
        successorRender, successorRender, 0,
        (static_cast<std::uint64_t>(successorArm.generation) << 32) |
            successorArm.slot,
        0);
    ASSERT_TRUE(arms.AcknowledgeApplied(successorArm, 1));
    ASSERT_TRUE(arms.Publish(successorArm));

    harken::TimelineArmToken members[2]{successorArm, {}};
    if (memberCount == 2) {
        VnVoiceHandle additional = 0;
        ASSERT_EQ(VN_OK, voices.Play(pcm, 0.0f, 1.0, 0, false, 0.0f, 0.0f, 0.0f,
            {}, 0, 1, 1, harken::ResampleProfile::LegacyCompatibility, &additional));
        ASSERT_TRUE(arms.ReserveGrouped(static_cast<std::uint32_t>(additional),
            static_cast<std::uint32_t>(additional >> 32), 512, 512, root,
            successorTarget, {decision}, 1, &members[1]));
        voices.ApplyTimelineGatedPlay(additional, additional, 0,
            (static_cast<std::uint64_t>(members[1].generation) << 32) | members[1].slot, 0);
        ASSERT_TRUE(arms.AcknowledgeApplied(members[1], 1));
        ASSERT_TRUE(arms.Publish(members[1]));
    }
    harken::ArmGroupTable armGroups(1, 2);
    harken::ArmGroupToken armGroup{};
    ASSERT_TRUE(armGroups.Reserve(
        decision, members, memberCount, &armGroup));
    for (std::uint32_t index = 0; index < memberCount; ++index)
        ASSERT_TRUE(armGroups.Acknowledge(armGroup, members[index], 1));
    harken::DecisionParticipantRef participant{};
    participant.kind = harken::DecisionParticipantKind::QueueInstance;
    participant.slot = queue.slot;
    participant.generation = queue.generation;
    harken::DecisionCommitPayload decisionPayload{};
    decisionPayload.participants = &participant;
    decisionPayload.participantCount = 1;
    decisionPayload.pages = {1, 1};
    decisionPayload.armGroup = armGroup;
    ASSERT_TRUE(decisions.Commit(decision, decisionPayload));

    harken::ScheduledTransactionPool::Config poolConfig{};
    poolConfig.quantumFrames = 256;
    poolConfig.scheduleHorizonFrames = 1024;
    poolConfig.publishLeadQuanta = 1;
    poolConfig.maxScheduledTransactions = 4;
    poolConfig.maxTransactionSpanFrames = 1024;
    poolConfig.maxIngressTransactionsPerQuantum = 4;
    poolConfig.maxFrameBatchesPerQuantum = 4;
    poolConfig.maxEventsPerQuantum = 8;
    poolConfig.maxTransactionEvents = 8;
    poolConfig.maxCurrentSlicesPerQuantum = 4;
    poolConfig.maxPlan03GroupHeadersPerPage = 1;
    poolConfig.maxArmsPerGroup = 2;
    harken::ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(poolConfig, 0, 3));
    harken::Plan03GroupSliceInput group{};
    group.executeFrame = 512;
    group.decision = decision;
    group.members = members;
    group.memberCount = memberCount;
    ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
              pool.ReservePlan03GroupSlice(group, arms));
    pool.Publish(768, nullptr, nullptr, &arms);

    harken::VoiceTimelineSink sink(voices, arms);
    ASSERT_TRUE(sink.AttachPlan03ScratchAuthority(decisions, pool));
    ASSERT_TRUE(sink.AttachQueueAuthority(queues, armGroups, occurrences));
    sink.SetArenaAdmission(UINT32_MAX, 256);
    ASSERT_TRUE(sink.AdmitQuantum(2));
    if (invalidateLast) {
        const auto last = members[memberCount - 1u];
        ASSERT_TRUE(voices.CancelTimelineBinding(last.renderSlot, last.renderGeneration));
    }
    ASSERT_EQ(!invalidateLast, sink.ApplyPlan03Groups(2));
    if (memberCount == 0 || invalidateLast) {
        harken::QueueInstanceSnapshot rejected{};
        ASSERT_TRUE(queues.Capture(queue, &rejected));
        EXPECT_NE(harken::ConditionalQueueActivation::Activated, rejected.pendingActivation);
        harken::EffectiveQueueState effective{};
        ASSERT_TRUE(queues.ResolveEffective(queue, &effective));
        EXPECT_EQ(1u, effective.version);
        EXPECT_EQ(3u, effective.currentChildOrdinal);
        for (std::uint32_t index = 0; index < memberCount; ++index) {
            harken::TimelineArmObservation observation{};
            ASSERT_TRUE(arms.Observe(members[index], &observation));
            EXPECT_EQ(0u, observation.startedBlockSequence);
        }
        return;
    }

    harken::DecisionCommittedSnapshot committed{};
    ASSERT_TRUE(decisions.CaptureCommitted(decision, &committed));
    EXPECT_EQ(harken::AudioGroupState::Admitted, committed.audioState);
    harken::QueueInstanceSnapshot snapshot{};
    ASSERT_TRUE(queues.Capture(queue, &snapshot));
    EXPECT_EQ(harken::ConditionalQueueActivation::Activated,
              snapshot.pendingActivation);
    harken::ArmState successorState = harken::ArmState::Free;
    ASSERT_TRUE(arms.State(successorArm, &successorState));
    EXPECT_EQ(harken::ArmState::Published, successorState);
    harken::TimelineEventPayload startPayload{};
    startPayload.kind = harken::TimelineEventKind::Start;
    harken::TimelineSinkEvent startEvent{};
    startEvent.payload = &startPayload;
    ASSERT_TRUE(sink.BeginQuantum(2));
    ASSERT_TRUE(sink.ApplyTransaction(
        successorArm.renderSlot, successorArm.renderGeneration, 2,
        &startEvent, 1, successorArm));
    ASSERT_TRUE(arms.State(successorArm, &successorState));
    EXPECT_EQ(harken::ArmState::Started, successorState);
    for (std::uint32_t index = 1; index < memberCount; ++index) {
        ASSERT_TRUE(sink.ApplyTransaction(members[index].renderSlot, members[index].renderGeneration, 2,
            &startEvent, 1, members[index]));
        ASSERT_TRUE(arms.State(members[index], &successorState));
        EXPECT_EQ(harken::ArmState::Started, successorState);
    }
    EXPECT_TRUE(queues.SettleActivated(queue, conditional));
    ASSERT_TRUE(occurrences.ClearCompletionOwnerExact(
        current, harken::CompletionOwnerRef::Single(decision, 1)));
    EXPECT_TRUE(occurrences.CompletionOwnerReleaseFencePassed(current, 2));
    harken::QueueConditionalProof stale{};
    EXPECT_EQ(harken::QueueConditionalProofStatus::Stale,
              queues.ProveConditionalForQuantum(
                  queue, conditional, occurrences, 512, 768, 2, true,
                  &stale));
}

TEST(QueueRuntime, RetiredSourceAudioSliceRequiresRetirementAndStartsSuccessor) {
    ExpectRetiredSourceAudioSlice(1);
}
TEST(QueueRuntime, RetiredSourceAudioSliceStartsEveryCompositeSuccessor) {
    ExpectRetiredSourceAudioSlice(2);
}
TEST(QueueRuntime, RetiredSourceAudioSliceRejectsInvalidSecondSuccessor) {
    ExpectRetiredSourceAudioSlice(2, true);
}
TEST(QueueRuntime, RetiredSourceAudioSliceRejectsEmptySuccessorGroup) {
    ExpectRetiredSourceAudioSlice(0);
}

TEST(QueueRuntime, ExistingAudioSliceLeaseActivatesWholeQueueGroup) {
    harken::EngineConfig config;
    harken::VoiceCapacityModel capacity;
    ASSERT_TRUE(harken::VoiceCapacityModel::TryCreate(1, 0, &capacity));
    config.voiceCapacity = capacity;
    harken::ResampleTables tables;
    tables.Init();
    harken::MultirateResampleTables multirateTables;
    multirateTables.Init();
    harken::BiquadTrigTable biquadTrig;
    harken::EffectPoolBank effects(config);
    harken::DynamicRouteSystem routes(config);
    harken::TimelineArmTable arms(1);
    harken::VoicePool voices(config, &tables, &multirateTables, &biquadTrig,
                            effects, routes, &arms);

    float sample = 0.0f;
    harken::PCMView pcm{};
    pcm.channels[0] = &sample;
    pcm.channelCount = 1;
    pcm.stride = 1;
    pcm.frames = 1;
    pcm.sampleRate = 48000;
    VnVoiceHandle render = 0;
    ASSERT_EQ(VN_OK, voices.Play(
                         pcm, 0.0f, 1.0, 0, false, 0.0f, 0.0f, 0.0f,
                         {}, 0, 1, 1,
                         harken::ResampleProfile::LegacyCompatibility,
                         &render));

    harken::PlayableInstanceTable instances(1);
    const auto root = instances.Acquire(harken::PlayableKind::Container, 0);
    harken::OccurrenceStatePool occurrences(2);
    harken::OccurrenceToken current{};
    harken::OccurrenceToken successor{};
    ASSERT_TRUE(occurrences.Acquire({root, harkentest::ObjectId(47), 0, 3}, &current));
    ASSERT_TRUE(occurrences.Acquire({root, harkentest::ObjectId(59), 0, 5}, &successor));
    ASSERT_TRUE(occurrences.BindCompletionProgress(
        current, static_cast<std::uint32_t>(render >> 32), 1));
    harken::CompletionProgressBlockUpdate update{};
    update.occurrence = current;
    update.renderGeneration = static_cast<std::uint32_t>(render >> 32);
    update.sourceOutputFramesThisBlock = 256;
    update.playbackFactsRevision = 1;
    update.completedThroughBlockSequence = 1;
    ASSERT_EQ(harken::CompletionProgressPublishStatus::Ok,
              harken::PublishCompletionProgressBlock(
                  *occurrences.CompletionProgressOf(current), update));

    harken::DecisionTable decisions(1, 2);
    harken::DecisionReserveRequest decisionRequest{};
    decisionRequest.ticket.sequence = 1;
    decisionRequest.world = 7;
    decisionRequest.transport = 3;
    decisionRequest.expectedContinuationGeneration = 1;
    decisionRequest.sliceCount = 1;
    harken::DecisionToken decision{};
    ASSERT_TRUE(decisions.Reserve(decisionRequest, &decision));
    ASSERT_TRUE(occurrences.TryPinCompletionOwner(
        current, harken::CompletionOwnerRef::Single(decision, 1)));

    harken::QueueInstanceTable queues(1);
    harken::QueueInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = root;
    create.containerStableId = harkentest::ObjectId(13);
    create.programSemanticFingerprint = 17;
    create.childCount = 2;
    create.initial.version = 1;
    create.initial.currentChildOrdinal = 3;
    harken::QueueInstanceToken queue{};
    ASSERT_TRUE(queues.Acquire(create, &queue));
    harken::ConditionalQueuePrepareRequest prepare{};
    prepare.expectedCurrentVersion = 1;
    prepare.rootDecision = decision;
    prepare.expectedCurrentOccurrence = current;
    prepare.successorOccurrence = successor;
    prepare.completion = BoundPredicate(
        decision, current, static_cast<std::uint32_t>(render >> 32));
    prepare.next.version = 2;
    prepare.next.cursor = 1;
    prepare.next.currentChildOrdinal = 5;
    harken::ConditionalQueueVersionToken conditional{};
    ASSERT_TRUE(queues.PrepareConditional(queue, prepare, &conditional));

    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = 17;
    target.subjectId = harkentest::ObjectId(59);
    harken::TimelineArmToken arm{};
    ASSERT_TRUE(arms.ReserveGrouped(
        static_cast<std::uint32_t>(render),
        static_cast<std::uint32_t>(render >> 32), 300, 256, 1, target,
        {decision}, 0, &arm));
    voices.ApplyTimelineGatedPlay(
        render, 300, 0,
        (static_cast<std::uint64_t>(arm.generation) << 32) | arm.slot, 0);
    ASSERT_TRUE(arms.AcknowledgeApplied(arm, 1));
    ASSERT_TRUE(arms.Publish(arm));

    harken::ArmGroupTable armGroups(1, 1);
    harken::ArmGroupToken armGroup{};
    ASSERT_TRUE(armGroups.Reserve(decision, &arm, 1, &armGroup));

    harken::DecisionParticipantRef participant{};
    participant.kind = harken::DecisionParticipantKind::QueueInstance;
    participant.slot = queue.slot;
    participant.generation = queue.generation;
    harken::DecisionCommitPayload decisionPayload{};
    decisionPayload.participants = &participant;
    decisionPayload.participantCount = 1;
    decisionPayload.pages = {1, 1};
    decisionPayload.armGroup = armGroup;
    ASSERT_TRUE(decisions.Commit(decision, decisionPayload));

    harken::ScheduledTransactionPool::Config poolConfig{};
    poolConfig.quantumFrames = 256;
    poolConfig.scheduleHorizonFrames = 1024;
    poolConfig.publishLeadQuanta = 1;
    poolConfig.maxScheduledTransactions = 4;
    poolConfig.maxTransactionSpanFrames = 1024;
    poolConfig.maxIngressTransactionsPerQuantum = 4;
    poolConfig.maxFrameBatchesPerQuantum = 4;
    poolConfig.maxEventsPerQuantum = 8;
    poolConfig.maxTransactionEvents = 8;
    poolConfig.maxCurrentSlicesPerQuantum = 4;
    poolConfig.maxPlan03GroupHeadersPerPage = 1;
    poolConfig.maxArmsPerGroup = 1;
    harken::ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(poolConfig, 0, 3));
    harken::Plan03GroupSliceInput group{};
    group.executeFrame = 300;
    group.decision = decision;
    group.members = &arm;
    group.memberCount = 1;
    ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
              pool.ReservePlan03GroupSlice(group, arms));
    pool.Publish(512, nullptr, nullptr, &arms);

    harken::VoiceTimelineSink sink(voices, arms);
    ASSERT_TRUE(sink.AttachPlan03ScratchAuthority(decisions, pool));
    ASSERT_TRUE(sink.AttachQueueAuthority(queues, armGroups, occurrences));
    sink.SetArenaAdmission(UINT32_MAX, 256);
    ASSERT_TRUE(sink.AdmitQuantum(1));
    EXPECT_FALSE(sink.ApplyPlan03Groups(1));
    harken::EffectiveQueueState effective{};
    ASSERT_TRUE(queues.ResolveEffective(queue, &effective));
    EXPECT_EQ(1u, effective.version);
    harken::Plan03PageGroupView page{};
    ASSERT_EQ(1u, pool.CapturePlan03PageGroupsRt(1, &page, 1));
    EXPECT_EQ(harken::AudioSliceState::Ready,
              page.lease->Capture().state);

    ASSERT_TRUE(armGroups.Acknowledge(armGroup, arm, 1));
    ASSERT_TRUE(sink.ApplyPlan03Groups(1));

    harken::DecisionCommittedSnapshot committed{};
    ASSERT_TRUE(decisions.CaptureCommitted(decision, &committed));
    EXPECT_EQ(harken::AudioGroupState::Admitted, committed.audioState);
    ASSERT_TRUE(queues.ResolveEffective(queue, &effective));
    EXPECT_EQ(2u, effective.version);
    EXPECT_EQ(harken::AudioSliceState::SliceApplied,
              page.lease->Capture().state);
}

TEST(QueueRuntime, MemberlessAudioSliceActivatesQueueCondition) {
    harken::EngineConfig config;
    harken::VoiceCapacityModel capacity;
    ASSERT_TRUE(harken::VoiceCapacityModel::TryCreate(1, 0, &capacity));
    config.voiceCapacity = capacity;
    harken::ResampleTables tables;
    tables.Init();
    harken::MultirateResampleTables multirateTables;
    multirateTables.Init();
    harken::BiquadTrigTable biquadTrig;
    harken::EffectPoolBank effects(config);
    harken::DynamicRouteSystem routes(config);
    harken::TimelineArmTable arms(1);
    harken::VoicePool voices(config, &tables, &multirateTables, &biquadTrig,
                            effects, routes, &arms);

    harken::PlayableInstanceTable instances(1);
    const auto root = instances.Acquire(harken::PlayableKind::Container, 0);
    harken::OccurrenceStatePool occurrences(1);
    harken::OccurrenceToken current{};
    ASSERT_TRUE(occurrences.Acquire({root, harkentest::ObjectId(47), 0, 3}, &current));
    ASSERT_TRUE(occurrences.BindCompletionProgress(current, 1, 1));
    harken::CompletionProgressBlockUpdate update{};
    update.occurrence = current;
    update.renderGeneration = 1;
    update.sourceOutputFramesThisBlock = 256;
    update.playbackFactsRevision = 1;
    update.completedThroughBlockSequence = 1;
    ASSERT_EQ(harken::CompletionProgressPublishStatus::Ok,
              harken::PublishCompletionProgressBlock(
                  *occurrences.CompletionProgressOf(current), update));

    harken::DecisionTable decisions(1, 2);
    harken::DecisionReserveRequest decisionRequest{};
    decisionRequest.ticket.sequence = 1;
    decisionRequest.world = 7;
    decisionRequest.transport = 3;
    decisionRequest.expectedContinuationGeneration = 1;
    decisionRequest.sliceCount = 1;
    harken::DecisionToken decision{};
    ASSERT_TRUE(decisions.Reserve(decisionRequest, &decision));
    ASSERT_TRUE(occurrences.TryPinCompletionOwner(
        current, harken::CompletionOwnerRef::Single(decision, 1)));

    harken::QueueInstanceTable queues(1);
    harken::QueueInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = root;
    create.containerStableId = harkentest::ObjectId(13);
    create.programSemanticFingerprint = 17;
    create.childCount = 2;
    create.initial.version = 1;
    create.initial.currentChildOrdinal = 3;
    harken::QueueInstanceToken queue{};
    ASSERT_TRUE(queues.Acquire(create, &queue));
    harken::ConditionalQueuePrepareRequest prepare{};
    prepare.expectedCurrentVersion = 1;
    prepare.rootDecision = decision;
    prepare.expectedCurrentOccurrence = current;
    prepare.successorOccurrence = current;
    prepare.completion = BoundPredicate(decision, current, 1);
    prepare.next.version = 2;
    prepare.next.cursor = 1;
    prepare.next.currentChildOrdinal = 5;
    harken::ConditionalQueueVersionToken conditional{};
    ASSERT_TRUE(queues.PrepareConditional(queue, prepare, &conditional));

    harken::ArmGroupTable armGroups(1, 1);
    harken::ArmGroupToken armGroup{};
    ASSERT_TRUE(armGroups.Reserve(decision, nullptr, 0, &armGroup));

    harken::DecisionParticipantRef participant{};
    participant.kind = harken::DecisionParticipantKind::QueueInstance;
    participant.slot = queue.slot;
    participant.generation = queue.generation;
    harken::DecisionCommitPayload decisionPayload{};
    decisionPayload.participants = &participant;
    decisionPayload.participantCount = 1;
    decisionPayload.pages = {1, 1};
    decisionPayload.armGroup = armGroup;
    ASSERT_TRUE(decisions.Commit(decision, decisionPayload));

    harken::ScheduledTransactionPool::Config poolConfig{};
    poolConfig.quantumFrames = 256;
    poolConfig.scheduleHorizonFrames = 1024;
    poolConfig.publishLeadQuanta = 1;
    poolConfig.maxScheduledTransactions = 4;
    poolConfig.maxTransactionSpanFrames = 1024;
    poolConfig.maxIngressTransactionsPerQuantum = 4;
    poolConfig.maxFrameBatchesPerQuantum = 4;
    poolConfig.maxEventsPerQuantum = 8;
    poolConfig.maxTransactionEvents = 8;
    poolConfig.maxCurrentSlicesPerQuantum = 4;
    poolConfig.maxPlan03GroupHeadersPerPage = 1;
    poolConfig.maxArmsPerGroup = 1;
    harken::ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(poolConfig, 0, 3));
    harken::Plan03GroupSliceInput group{};
    group.executeFrame = 300;
    group.decision = decision;
    ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
              pool.ReservePlan03GroupSlice(group, arms));
    pool.Publish(512, nullptr, nullptr, &arms);

    harken::VoiceTimelineSink sink(voices, arms);
    ASSERT_TRUE(sink.AttachPlan03ScratchAuthority(decisions, pool));
    ASSERT_TRUE(sink.AttachQueueAuthority(queues, armGroups, occurrences));
    sink.SetArenaAdmission(UINT32_MAX, 256);
    ASSERT_TRUE(sink.AdmitQuantum(1));
    ASSERT_TRUE(sink.ApplyPlan03Groups(1));

    harken::EffectiveQueueState effective{};
    ASSERT_TRUE(queues.ResolveEffective(queue, &effective));
    EXPECT_EQ(2u, effective.version);
    EXPECT_EQ(1u, effective.cursor);
    EXPECT_EQ(5u, effective.currentChildOrdinal);
    harken::DecisionCommittedSnapshot committed{};
    ASSERT_TRUE(decisions.CaptureCommitted(decision, &committed));
    EXPECT_EQ(harken::AudioGroupState::Admitted, committed.audioState);
    harken::Plan03PageGroupView page{};
    ASSERT_EQ(1u, pool.CapturePlan03PageGroupsRt(1, &page, 1));
    EXPECT_EQ(0u, page.memberCount);
    EXPECT_EQ(nullptr, page.members);
    EXPECT_EQ(harken::AudioSliceState::SliceApplied,
              page.lease->Capture().state);
}

} // namespace


TEST(QueueRuntime, NativeBaselineAcquireCancellationPreservesEveryExternalLoan) {
    harken::QueueInstanceTable table(1), foreign(1);
    harken::PreparedQueueInstanceV1 prepared, otherPrepared;
    harken::QueueInstanceTable::OriginalLifetimePin original, loan, other;
    ASSERT_TRUE(table.PrepareAcquire(OriginalQueueCreateRequest(), &prepared, &original));
    ASSERT_TRUE(foreign.PrepareAcquire(OriginalQueueCreateRequest(), &otherPrepared, &other));
    const auto token = prepared.Token();
    const auto before = harken::QueueInstanceTableTestAccess::Snapshot(table, token);
    ASSERT_TRUE(original.Borrow(&loan));
    EXPECT_FALSE(table.CancelPreparedAcquire(&prepared, &original));
    EXPECT_TRUE(original.Valid());
    EXPECT_TRUE(loan.Valid());
    EXPECT_EQ(before, harken::QueueInstanceTableTestAccess::Snapshot(table, token));
    loan.Release();
    EXPECT_FALSE(table.CancelPreparedAcquire(&prepared, &other));
    EXPECT_TRUE(original.Valid());
    EXPECT_TRUE(other.Valid());
    ASSERT_TRUE(table.CancelPreparedAcquire(&prepared, &original));
    EXPECT_FALSE(original.Valid());
    EXPECT_FALSE(prepared.Valid());
    ASSERT_TRUE(foreign.CancelPreparedAcquire(&otherPrepared, &other));
    EXPECT_FALSE(other.Valid());
}

TEST(QueueRuntime, NativeBaselineLiveRetirementConsumesOnlyItsExactOriginal) {
    harken::QueueInstanceTable table(1);
    harken::PreparedQueueInstanceV1 prepared;
    harken::QueueInstanceTable::OriginalLifetimePin original, loan;
    ASSERT_TRUE(table.PrepareAcquire(OriginalQueueCreateRequest(), &prepared, &original));
    harken::QueueInstanceToken token{};
    ASSERT_TRUE(table.CommitPreparedAcquire(&prepared, &token));
    ASSERT_TRUE(original.Borrow(&loan));
    EXPECT_FALSE(table.Release(token, &original));
    EXPECT_TRUE(original.Valid());
    harken::QueueInstanceSnapshot actual{};
    EXPECT_TRUE(table.Capture(token, &actual));
    loan.Release();
    EXPECT_FALSE(table.Release({token.slot, token.generation + 1u}, &original));
    EXPECT_TRUE(original.Valid());
    ASSERT_TRUE(table.Release(token, &original));
    EXPECT_FALSE(original.Valid());
    EXPECT_FALSE(table.Capture(token, &actual));
    harken::PreparedQueueInstanceV1 next;
    harken::QueueInstanceTable::OriginalLifetimePin nextOriginal;
    ASSERT_TRUE(table.PrepareAcquire(OriginalQueueCreateRequest(), &next, &nextOriginal));
    EXPECT_NE(token.generation, next.Token().generation);
    ASSERT_TRUE(table.CancelPreparedAcquire(&next, &nextOriginal));
}

TEST(QueueRuntime, NativeBankBaselineResetPreservesForeignAndBorrowedOwners) {
    using Bank = harken::EngineQueueProgramBank;
    Bank bank, foreign;
    Bank::OriginalReservation original, other;
    Bank::OriginalLifetimePin baseline, loan, otherBaseline;
    ASSERT_TRUE(bank.TryReserve(&original));
    ASSERT_TRUE(bank.IssueLifetimePin(original, &baseline));
    ASSERT_TRUE(foreign.TryReserve(&other));
    ASSERT_TRUE(foreign.IssueLifetimePin(other, &otherBaseline));
    ASSERT_TRUE(baseline.Borrow(&loan));
    const auto generation = bank.Generation();
    EXPECT_TRUE(bank.HasOriginalOwnersOtherThan(original, baseline));
    EXPECT_FALSE(bank.TryResetOriginalStorage(original, baseline));
    EXPECT_TRUE(original.Valid());
    EXPECT_TRUE(baseline.Valid());
    EXPECT_TRUE(loan.Valid());
    loan.Release();
    EXPECT_FALSE(bank.TryResetOriginalStorage(other, otherBaseline));
    EXPECT_TRUE(other.Valid());
    EXPECT_TRUE(otherBaseline.Valid());
    ASSERT_TRUE(bank.TryResetOriginalStorage(original, baseline));
    EXPECT_FALSE(original.Valid());
    EXPECT_FALSE(baseline.Valid());
    EXPECT_FALSE(bank.occupied);
    ASSERT_TRUE(bank.TryReserve(&original));
    EXPECT_GT(bank.Generation(), generation);
    ASSERT_TRUE(bank.IssueLifetimePin(original, &baseline));
    ASSERT_TRUE(bank.TryResetOriginalStorage(original, baseline));
    ASSERT_TRUE(foreign.TryResetOriginalStorage(other, otherBaseline));
}

namespace {

void ExpectNativeQueueOwnerLedgerCancellation(bool commitQueue) {
    using Context = harken::EngineActionPlayScratch::PreparedQueueInstanceOwnerContext;
    using Access = harken::QueueInstanceTableTestAccess;
    const auto input = harkentest::MakeNullConfig(1, 64, 48000);
    harken::EngineConfig config{};
    ASSERT_EQ(VN_OK, harken::ResolveConfig(&input, &config));
    auto actualStorage = std::make_unique<harken::Engine>(config);
    auto foreignStorage = std::make_unique<harken::Engine>(config);
    auto& actual = *actualStorage;
    auto& foreign = *foreignStorage;
    harken::PreparedQueueInstanceV1 prepared, foreignPrepared;
    ASSERT_TRUE(actual.PrepareOriginalQueueAcquire(OriginalQueueCreateRequest(), &prepared));
    ASSERT_TRUE(foreign.PrepareOriginalQueueAcquire(OriginalQueueCreateRequest(), &foreignPrepared));
    const auto token = prepared.Token();
    const auto foreignToken = foreignPrepared.Token();
    ASSERT_EQ(token.slot, foreignToken.slot);
    ASSERT_EQ(token.generation, foreignToken.generation);
    auto& original = actual.originalQueueLifetimeHolders[token.slot];
    auto& foreignOriginal = foreign.originalQueueLifetimeHolders[foreignToken.slot];
    ASSERT_TRUE(actual.queueInstances.IsOriginalLifetimePinFor(original, token));
    ASSERT_TRUE(foreign.queueInstances.IsOriginalLifetimePinFor(foreignOriginal, foreignToken));
    harken::QueueInstanceToken output{}, foreignOutput{};
    Context owner{&actual.queueInstances, &prepared, &output, nullptr, false};
    owner.engine = &actual;
    if (commitQueue) {
        owner.committed = actual.queueInstances.CommitPreparedAcquire(&prepared, &output);
        ASSERT_TRUE(owner.committed);
        ASSERT_TRUE(foreign.queueInstances.CommitPreparedAcquire(&foreignPrepared, &foreignOutput));
        ASSERT_EQ(token.slot, output.slot);
        ASSERT_EQ(token.generation, output.generation);
    }
    harken::QueueInstanceTable::OriginalLifetimePin loan;
    ASSERT_TRUE(original.Borrow(&loan));
    const auto actualBefore = Access::Snapshot(actual.queueInstances, token);
    const auto foreignBefore = Access::Snapshot(foreign.queueInstances, foreignToken);
    const auto actualLifetimeBefore = Access::OriginalLifetime(actual.queueInstances, token);
    const auto foreignLifetimeBefore = Access::OriginalLifetime(foreign.queueInstances, foreignToken);
    EXPECT_EQ(2u, actualLifetimeBefore.second);
    EXPECT_EQ(1u, foreignLifetimeBefore.second);

    owner.engine = &foreign;
    EXPECT_FALSE(harken::detail::CancelPreparedQueueInstanceOwnerForTests(&owner));
    EXPECT_EQ(actualBefore, Access::Snapshot(actual.queueInstances, token));
    EXPECT_EQ(foreignBefore, Access::Snapshot(foreign.queueInstances, foreignToken));
    EXPECT_EQ(actualLifetimeBefore, Access::OriginalLifetime(actual.queueInstances, token));
    EXPECT_EQ(foreignLifetimeBefore, Access::OriginalLifetime(foreign.queueInstances, foreignToken));
    EXPECT_TRUE(original.Valid());
    EXPECT_TRUE(foreignOriginal.Valid());
    EXPECT_TRUE(loan.Valid());
    EXPECT_EQ(commitQueue, owner.committed);
    EXPECT_EQ(!commitQueue, prepared.Valid());
    if (commitQueue) {
        EXPECT_EQ(token.slot, output.slot);
        EXPECT_EQ(token.generation, output.generation);
    } else {
        EXPECT_FALSE(output.Valid());
        EXPECT_EQ(token.slot, prepared.Token().slot);
        EXPECT_EQ(token.generation, prepared.Token().generation);
    }
    owner.engine = &actual;

    harken::EngineFrontierOwnerLedger ledger;
    harken::EngineFrontierOwnerLedger::Limits limits{};
    limits[static_cast<std::size_t>(harken::FrontierOwnerKind::Mode)] = 1;
    ledger.Initialize(limits);
    ASSERT_TRUE(ledger.Begin(41));
    ASSERT_TRUE(ledger.Add(41, {harken::FrontierOwnerKind::Mode, 0, 41}, &owner,
        harken::FrontierOwnerCommitPhase::Mode, harken::FrontierOwnerCancelPhase::Mode,
        [](void* opaque) noexcept {
            const auto& context = *static_cast<Context*>(opaque);
            if (!context.committed) return context.prepared->Valid();
            harken::QueueInstanceSnapshot snapshot{};
            return context.table->Capture(*context.output, &snapshot);
        },
        [](void*, std::uint64_t) noexcept { return false; },
        [](void* opaque) noexcept {
            return harken::detail::CancelPreparedQueueInstanceOwnerForTests(
                static_cast<Context*>(opaque));
        }));
    EXPECT_FALSE(ledger.Cancel(41));
    EXPECT_EQ(1u, ledger.UnresolvedCount());
    EXPECT_EQ(actualBefore, Access::Snapshot(actual.queueInstances, token));
    EXPECT_EQ(actualLifetimeBefore, Access::OriginalLifetime(actual.queueInstances, token));
    EXPECT_EQ(commitQueue, owner.committed);
    EXPECT_EQ(!commitQueue, prepared.Valid());
    EXPECT_TRUE(original.Valid());
    EXPECT_TRUE(loan.Valid());
    if (commitQueue) {
        EXPECT_EQ(token.slot, output.slot);
        EXPECT_EQ(token.generation, output.generation);
        EXPECT_EQ(1u, actual.queueInstances.LiveCount());
    } else {
        EXPECT_FALSE(output.Valid());
        EXPECT_EQ(token.slot, prepared.Token().slot);
        EXPECT_EQ(token.generation, prepared.Token().generation);
    }
    loan.Release();
    EXPECT_FALSE(loan.Valid());
    EXPECT_EQ(actualLifetimeBefore.first,
              Access::OriginalLifetime(actual.queueInstances, token).first);
    EXPECT_EQ(1u, Access::OriginalLifetime(actual.queueInstances, token).second);
    ASSERT_TRUE(ledger.Cancel(41));
    EXPECT_TRUE(ledger.Empty());
    EXPECT_FALSE(original.Valid());
    EXPECT_FALSE(prepared.Valid());
    EXPECT_FALSE(owner.committed);
    EXPECT_FALSE(output.Valid());
    EXPECT_EQ(0u, actual.queueInstances.LiveCount());
    harken::QueueInstanceSnapshot released{};
    EXPECT_FALSE(actual.queueInstances.Capture(token, &released));
    EXPECT_EQ(0u, Access::OriginalLifetime(actual.queueInstances, token).second);
    EXPECT_EQ(foreignBefore, Access::Snapshot(foreign.queueInstances, foreignToken));
    EXPECT_EQ(foreignLifetimeBefore, Access::OriginalLifetime(foreign.queueInstances, foreignToken));
    EXPECT_TRUE(foreignOriginal.Valid());
    if (commitQueue) {
        ASSERT_TRUE(foreign.ReleaseOriginalQueue(foreignOutput));
    } else {
        ASSERT_TRUE(foreign.CancelOriginalQueueAcquire(&foreignPrepared));
    }
    EXPECT_FALSE(foreignOriginal.Valid());
    harken::PreparedQueueInstanceV1 next;
    ASSERT_TRUE(actual.PrepareOriginalQueueAcquire(OriginalQueueCreateRequest(), &next));
    ASSERT_TRUE(next.Valid());
    ASSERT_TRUE(actual.CancelOriginalQueueAcquire(&next));
    EXPECT_FALSE(next.Valid());
}

} // namespace

TEST(QueueRuntime, OriginalInitializingOwnerLedgerCallbackReturnsOnlyItsNativeBaseline) {
    ExpectNativeQueueOwnerLedgerCancellation(false);
}

TEST(QueueRuntime, OriginalCommittedOwnerLedgerCallbackReturnsOnlyItsNativeBaseline) {
    ExpectNativeQueueOwnerLedgerCancellation(true);
}


namespace {

// These tests prove native conditional storage and acquired Queue-parent
// lifetime. Logical-boundary identities come from real native factories.
// They do not prove a physical Current Source or grant any Reset cutover right.
struct OriginalConditionalNativeIdentities {
    harken::LogicCommitCoordinator coordinator;
    harken::DecisionTable decisions{8, 1};
    harken::OccurrenceStatePool occurrences{16};
    std::vector<harken::DecisionToken> decisionTokens;
    std::vector<harken::OccurrenceToken> occurrenceTokens;

    bool Acquire(harken::ConditionalQueuePrepareRequest* out) {
        if (out == nullptr) return false;
        harken::DecisionReserveRequest reserve{};
        reserve.ticket.sequence = coordinator.Issue();
        reserve.world = 7;
        reserve.transport = 1;
        reserve.expectedContinuationGeneration = 1;
        reserve.sliceCount = 1;
        harken::DecisionToken decision{};
        if (!decisions.Reserve(reserve, &decision)) return false;
        decisionTokens.push_back(decision);
        harken::OccurrenceToken current{}, successor{};
        if (!occurrences.Acquire({11, harkentest::ObjectId(47), 0,
                static_cast<std::uint32_t>(occurrenceTokens.size())}, &current)) return false;
        occurrenceTokens.push_back(current);
        if (!occurrences.Acquire({11, harkentest::ObjectId(53), 0,
                static_cast<std::uint32_t>(occurrenceTokens.size())}, &successor)) return false;
        occurrenceTokens.push_back(successor);
        *out = LogicalBoundaryRequest();
        out->rootDecision = decision;
        out->expectedCurrentOccurrence = current;
        out->successorOccurrence = successor;
        return true;
    }

    void ReleaseAll() {
        for (const auto occurrence : occurrenceTokens) {
            EXPECT_TRUE(occurrences.Advance(occurrence, harken::OccurrenceStage::AwaitingEventFence));
            EXPECT_TRUE(occurrences.ReleaseAfterFence(occurrence));
        }
        occurrenceTokens.clear();
        for (const auto decision : decisionTokens) {
            EXPECT_TRUE(decisions.Cancel(decision));
            EXPECT_TRUE(decisions.ReclaimCancelled(decision));
        }
        decisionTokens.clear();
        EXPECT_EQ(0u, occurrences.OccupiedCount());
        EXPECT_EQ(0u, decisions.LiveCount());
    }
};

TEST(QueueRuntime, OriginalConditionalFactoryCommitRetainsExactFamilyWhileOldProgressCompletes) {
    using Original = harken::QueueInstanceTable::OriginalConditionalFutureReservation;
    using Access = harken::QueueInstanceTableTestAccess;
    static_assert(!std::is_copy_constructible_v<Original>);
    static_assert(!std::is_copy_assignable_v<Original>);
    static_assert(std::is_nothrow_move_constructible_v<Original>);
    harken::QueueInstanceTable table(1, 2), foreign(1, 2);
    OriginalConditionalNativeIdentities identities;
    harken::QueueInstanceTable::OriginalLifetimePin parent;
    harken::QueueInstanceToken queue{};
    ASSERT_TRUE(table.Acquire(OriginalQueueCreateRequest(), &queue, &parent));
    harken::ConditionalQueuePrepareRequest request{};
    ASSERT_TRUE(identities.Acquire(&request));
    harken::PreparedQueueConditionalV1 prepared;
    Original original, loan;
    ASSERT_TRUE(table.PrepareConditional(queue, request, &prepared, parent, &original));
    const auto index = Access::OriginalConditionalIndex(table, queue);
    ASSERT_TRUE(original.Valid());
    EXPECT_FALSE(table.IsCurrentOriginalConditionalFutureReservation(original));
    ASSERT_TRUE(original.Borrow(&loan));
    Original moved(std::move(loan));
    EXPECT_FALSE(loan.Valid());
    EXPECT_FALSE(original.Borrow(&moved));
    EXPECT_FALSE(original.Borrow(&original));
    EXPECT_FALSE(original.Borrow(nullptr));
    const auto before = Access::Snapshot(table, queue);
    const auto originalBefore = Access::OriginalConditionalRecord(table, index);
    harken::ConditionalQueueVersionToken version{71};
    EXPECT_FALSE(foreign.CommitPreparedConditional(&prepared, &version));
    EXPECT_EQ(71u, version.version);
    EXPECT_EQ(before, Access::Snapshot(table, queue));
    EXPECT_EQ(originalBefore, Access::OriginalConditionalRecord(table, index));
    EXPECT_TRUE(prepared.Valid());
    ASSERT_TRUE(table.CommitPreparedConditional(&prepared, &version));
    EXPECT_FALSE(prepared.Valid());
    EXPECT_TRUE(table.IsCurrentOriginalConditionalFutureReservation(original));
    EXPECT_TRUE(table.IsOriginalConditionalFutureReservationFor(original, parent,
        queue, version, request.rootDecision, request.expectedCurrentOccurrence));
    EXPECT_FALSE(foreign.IsOriginalConditionalFutureReservationFor(original, parent,
        queue, version, request.rootDecision, request.expectedCurrentOccurrence));
    EXPECT_FALSE(table.IsOriginalConditionalFutureReservationFor(original, parent,
        {queue.slot, queue.generation + 1u}, version, request.rootDecision,
        request.expectedCurrentOccurrence));
    EXPECT_FALSE(table.IsOriginalConditionalFutureReservationFor(original, parent,
        queue, {version.version + 1u}, request.rootDecision, request.expectedCurrentOccurrence));
    EXPECT_FALSE(table.IsOriginalConditionalFutureReservationFor(original, parent,
        queue, version, {request.rootDecision.slot, request.rootDecision.generation + 1u},
        request.expectedCurrentOccurrence));
    EXPECT_FALSE(table.IsOriginalConditionalFutureReservationFor(original, parent,
        queue, version, request.rootDecision,
        {request.expectedCurrentOccurrence.slot, request.expectedCurrentOccurrence.generation + 1u}));
    ASSERT_TRUE(table.ActivateConditional(queue, version, request.rootDecision,
        request.expectedCurrentOccurrence));
    ASSERT_TRUE(table.SettleActivated(queue, version));
    EXPECT_TRUE(original.Valid());
    EXPECT_TRUE(moved.Valid());
    EXPECT_FALSE(table.IsCurrentOriginalConditionalFutureReservation(original));
    EXPECT_EQ(2u, Access::OriginalLifetime(table, queue).second);
    harken::ConditionalQueuePrepareRequest descendant{};
    ASSERT_TRUE(identities.Acquire(&descendant));
    descendant.expectedCurrentVersion = version.version;
    descendant.next.version = version.version + 1u;
    Original next;
    ASSERT_TRUE(table.PrepareConditional(queue, descendant, &prepared, parent, &next));
    harken::ConditionalQueueVersionToken nextVersion{};
    ASSERT_TRUE(table.CommitPreparedConditional(&prepared, &nextVersion));
    EXPECT_FALSE(table.IsOriginalConditionalFutureReservationFor(original, parent,
        queue, nextVersion, descendant.rootDecision, descendant.expectedCurrentOccurrence));
    EXPECT_TRUE(table.IsCurrentOriginalConditionalFutureReservation(next));
    ASSERT_TRUE(table.RetireConditionalForAbort(queue, nextVersion, descendant.rootDecision));
    EXPECT_EQ(3u, Access::OriginalLifetime(table, queue).second);
    EXPECT_FALSE(table.Release(queue, &parent));
    original.Release();
    EXPECT_TRUE(moved.Valid());
    EXPECT_FALSE(table.Release(queue, &parent));
    moved.Release();
    next.Release();
    EXPECT_EQ(1u, Access::OriginalLifetime(table, queue).second);
    ASSERT_TRUE(table.Release(queue, &parent));
    EXPECT_FALSE(parent.Valid());
    identities.ReleaseAll();
}

TEST(QueueRuntime, OriginalConditionalInitializingCancelAndRollbackReturnOnlyNativeAssociation) {
    using Original = harken::QueueInstanceTable::OriginalConditionalFutureReservation;
    harken::QueueInstanceTable table(1, 2), foreign(1, 2);
    OriginalConditionalNativeIdentities identities;
    harken::QueueInstanceTable::OriginalLifetimePin parent;
    harken::PreparedQueueInstanceV1 instance;
    ASSERT_TRUE(table.PrepareAcquire(OriginalQueueCreateRequest(), &instance, &parent));
    const auto queue = instance.Token();
    harken::ConditionalQueuePrepareRequest request{};
    ASSERT_TRUE(identities.Acquire(&request));
    harken::PreparedQueueConditionalV1 prepared;
    Original original, loan;
    ASSERT_TRUE(table.PrepareConditional(queue, request, &prepared, parent, &original));
    ASSERT_TRUE(original.Borrow(&loan));
    EXPECT_FALSE(foreign.CancelPreparedConditional(&prepared));
    ASSERT_TRUE(table.CancelPreparedConditional(&prepared));
    EXPECT_TRUE(original.Valid());
    EXPECT_TRUE(loan.Valid());
    EXPECT_FALSE(table.CancelPreparedAcquire(&instance, &parent));
    original.Release();
    EXPECT_FALSE(table.CancelPreparedAcquire(&instance, &parent));
    loan.Release();
    Original committed;
    ASSERT_TRUE(table.PrepareConditional(queue, request, &prepared, parent, &committed));
    harken::PreparedQueueConditionalV1 moved(std::move(prepared));
    EXPECT_FALSE(prepared.Valid());
    harken::ConditionalQueueVersionToken version{};
    ASSERT_TRUE(table.CommitPreparedConditional(&moved, &version));
    EXPECT_FALSE(table.RollbackInitializingConditional(instance, queue,
        {version.version + 1u}, request.rootDecision));
    EXPECT_TRUE(table.IsCurrentOriginalConditionalFutureReservation(committed));
    ASSERT_TRUE(table.RollbackInitializingConditional(instance, queue, version, request.rootDecision));
    EXPECT_TRUE(committed.Valid());
    EXPECT_FALSE(table.IsCurrentOriginalConditionalFutureReservation(committed));
    EXPECT_FALSE(table.CancelPreparedAcquire(&instance, &parent));
    committed.Release();
    ASSERT_TRUE(table.CancelPreparedAcquire(&instance, &parent));
    EXPECT_FALSE(parent.Valid());
    identities.ReleaseAll();
}

TEST(QueueRuntime, OriginalConditionalCapacityForeignAndNonemptyRefusalPreserveBothOutputs) {
    using Original = harken::QueueInstanceTable::OriginalConditionalFutureReservation;
    using Access = harken::QueueInstanceTableTestAccess;
    harken::QueueInstanceTable table(1, 1), foreign(1, 1), ordinary(1);
    OriginalConditionalNativeIdentities identities;
    harken::QueueInstanceTable::OriginalLifetimePin parent, foreignParent, ordinaryParent;
    harken::QueueInstanceToken queue{}, foreignQueue{}, ordinaryQueue{};
    ASSERT_TRUE(table.Acquire(OriginalQueueCreateRequest(), &queue, &parent));
    ASSERT_TRUE(foreign.Acquire(OriginalQueueCreateRequest(), &foreignQueue, &foreignParent));
    ASSERT_TRUE(ordinary.Acquire(OriginalQueueCreateRequest(), &ordinaryQueue, &ordinaryParent));
    ASSERT_EQ(queue.slot, foreignQueue.slot);
    ASSERT_EQ(queue.generation, foreignQueue.generation);
    harken::ConditionalQueuePrepareRequest request{};
    ASSERT_TRUE(identities.Acquire(&request));
    harken::PreparedQueueConditionalV1 prepared, refused;
    Original original, output;
    ASSERT_TRUE(table.PrepareConditional(queue, request, &prepared, parent, &original));
    const auto index = Access::OriginalConditionalIndex(table, queue);
    ASSERT_TRUE(table.CancelPreparedConditional(&prepared));
    const auto before = Access::Snapshot(table, queue);
    const auto recordBefore = Access::OriginalConditionalRecord(table, index);
    const auto reject = [&](const auto& operation) {
        EXPECT_FALSE(operation());
        EXPECT_FALSE(refused.Valid());
        EXPECT_FALSE(output.Valid());
        EXPECT_EQ(before, Access::Snapshot(table, queue));
        EXPECT_EQ(recordBefore, Access::OriginalConditionalRecord(table, index));
    };
    reject([&] { return table.PrepareConditional(queue, request, &refused, parent, &output); });
    reject([&] { return table.PrepareConditional(queue, request, &refused, foreignParent, &output); });
    reject([&] { return table.PrepareConditional({queue.slot, queue.generation + 1u}, request,
        &refused, parent, &output); });
    reject([&] { return table.PrepareConditional(queue, request, &refused, parent, nullptr); });
    EXPECT_FALSE(table.PrepareConditional(queue, request, &refused, parent, &original));
    EXPECT_EQ(recordBefore, Access::OriginalConditionalRecord(table, index));
    EXPECT_FALSE(ordinary.PrepareConditional(ordinaryQueue, request, &refused, ordinaryParent, &output));
    EXPECT_FALSE(refused.Valid());
    EXPECT_FALSE(output.Valid());
    harken::ConditionalQueueVersionToken version{};
    ASSERT_TRUE(table.PrepareConditional(queue, request, &version));
    ASSERT_TRUE(table.ActivateConditional(queue, version, request.rootDecision, request.expectedCurrentOccurrence));
    ASSERT_TRUE(table.SettleActivated(queue, version));
    EXPECT_EQ(recordBefore, Access::OriginalConditionalRecord(table, index));
    EXPECT_TRUE(original.Valid());
    original.Release();
    auto next = request;
    next.expectedCurrentVersion = version.version;
    next.next.version = version.version + 1u;
    ASSERT_TRUE(table.PrepareConditional(queue, next, &refused, parent, &output));
    EXPECT_NE(recordBefore[1], Access::OriginalConditionalRecord(table, index)[1]);
    EXPECT_NE(recordBefore[2], Access::OriginalConditionalRecord(table, index)[2]);
    ASSERT_TRUE(table.CancelPreparedConditional(&refused));
    output.Release();
    ASSERT_TRUE(table.Release(queue, &parent));
    ASSERT_TRUE(foreign.Release(foreignQueue, &foreignParent));
    ASSERT_TRUE(ordinary.Release(ordinaryQueue, &ordinaryParent));
    identities.ReleaseAll();
}

TEST(QueueRuntime, OriginalConditionalActualResetReplacementRetainsOldRecordAndConsumesNewIntent) {
    using Original = harken::QueueInstanceTable::OriginalConditionalFutureReservation;
    using Access = harken::QueueInstanceTableTestAccess;
    harken::QueueInstanceTable table(1, 1);
    OriginalConditionalNativeIdentities identities;
    auto create = OriginalQueueCreateRequest();
    create.initial.version = 7;
    create.initial.currentChildOrdinal = 5;
    create.initial.selectionOrdinal = 11;
    create.resetInitial.version = 1;
    create.resetInitial.currentChildOrdinal = 3;
    create.resetInitial.selectionOrdinal = 1;
    harken::QueueInstanceTable::OriginalLifetimePin parent;
    harken::QueueInstanceToken queue{};
    ASSERT_TRUE(table.Acquire(create, &queue, &parent));
    harken::ConditionalQueuePrepareRequest old{};
    ASSERT_TRUE(identities.Acquire(&old));
    old.expectedCurrentVersion = create.initial.version;
    old.next = create.resetInitial;
    old.next.version = 8;
    harken::PreparedQueueConditionalV1 conditional;
    Original original, loan;
    ASSERT_TRUE(table.PrepareConditional(queue, old, &conditional, parent, &original));
    ASSERT_TRUE(original.Borrow(&loan));
    const auto index = Access::OriginalConditionalIndex(table, queue);
    harken::ConditionalQueueVersionToken oldVersion{};
    ASSERT_TRUE(table.CommitPreparedConditional(&conditional, &oldVersion));
    const auto originalBefore = Access::OriginalConditionalRecord(table, index);
    auto replacement = old;
    harken::ConditionalQueuePrepareRequest replacementIdentity{};
    ASSERT_TRUE(identities.Acquire(&replacementIdentity));
    replacement.rootDecision = replacementIdentity.rootDecision;
    replacement.projectedStates = &replacement.next;
    replacement.projectedStateCount = 1;
    harken::PreparedQueueConditionalReplacementV1 prepared;
    ASSERT_EQ(harken::QueueResetStatusV1::Ok, table.PrepareConditionalReplacement(
        queue, oldVersion, old.rootDecision, replacement, &prepared));
    harken::QueueResetTargetV1 target{};
    target.world = create.world;
    target.containerStableId = create.containerStableId;
    target.programSemanticFingerprint = create.programSemanticFingerprint;
    harken::PreparedQueueResetSetV1 reset;
    ASSERT_EQ(harken::QueueResetStatusV1::Ok, table.PrepareReset(target, &reset));
    ASSERT_EQ(harken::QueueResetStatusV1::Ok, table.CommitPreparedConditionalReplacement(&prepared, 61));
    ASSERT_EQ(harken::QueueResetStatusV1::Ok, table.CommitPreparedReset(&reset, 61));
    EXPECT_TRUE(original.Valid());
    EXPECT_FALSE(table.IsCurrentOriginalConditionalFutureReservation(original));
    auto originalAfter = Access::OriginalConditionalRecord(table, index);
    auto expectedRetired = originalBefore;
    expectedRetired[0] = 3u;
    EXPECT_EQ(expectedRetired, originalAfter);
    EXPECT_TRUE(table.IsOriginalConditionalFutureReservationFor(original, parent,
        queue, oldVersion, old.rootDecision, old.expectedCurrentOccurrence));
    EXPECT_FALSE(table.IsOriginalConditionalFutureReservationFor(original, parent,
        queue, oldVersion, replacement.rootDecision, old.expectedCurrentOccurrence));
    ASSERT_TRUE(table.ActivateConditional(queue, oldVersion, replacement.rootDecision,
        replacement.expectedCurrentOccurrence));
    ASSERT_TRUE(table.SettleActivated(queue, oldVersion));
    harken::QueueInstanceSnapshot actual{};
    ASSERT_TRUE(table.Capture(queue, &actual));
    EXPECT_FALSE(actual.resetPending);
    EXPECT_EQ(8u, actual.logicMirror.version);
    EXPECT_EQ(create.resetInitial.currentChildOrdinal, actual.logicMirror.currentChildOrdinal);
    EXPECT_EQ(expectedRetired, Access::OriginalConditionalRecord(table, index));
    original.Release();
    EXPECT_FALSE(table.Release(queue, &parent));
    loan.Release();
    ASSERT_TRUE(table.Release(queue, &parent));
    identities.ReleaseAll();
}

TEST(QueueRuntime, OriginalConditionalMoveOverwriteDestructionReturnPriorExactReceiver) {
    using Original = harken::QueueInstanceTable::OriginalConditionalFutureReservation;
    using Access = harken::QueueInstanceTableTestAccess;
    harken::QueueInstanceTable first(1, 1), second(1, 1);
    OriginalConditionalNativeIdentities identities;
    harken::QueueInstanceTable::OriginalLifetimePin firstParent, secondParent;
    harken::QueueInstanceToken firstQueue{}, secondQueue{};
    ASSERT_TRUE(first.Acquire(OriginalQueueCreateRequest(), &firstQueue, &firstParent));
    ASSERT_TRUE(second.Acquire(OriginalQueueCreateRequest(), &secondQueue, &secondParent));
    harken::ConditionalQueuePrepareRequest request{};
    ASSERT_TRUE(identities.Acquire(&request));
    harken::PreparedQueueConditionalV1 firstPrepared, secondPrepared;
    Original retained;
    {
        Original firstOriginal, secondOriginal;
        ASSERT_TRUE(first.PrepareConditional(firstQueue, request, &firstPrepared, firstParent, &firstOriginal));
        ASSERT_TRUE(second.PrepareConditional(secondQueue, request, &secondPrepared, secondParent, &secondOriginal));
        ASSERT_TRUE(firstOriginal.Borrow(&retained));
        ASSERT_TRUE(first.CancelPreparedConditional(&firstPrepared));
        ASSERT_TRUE(second.CancelPreparedConditional(&secondPrepared));
        firstOriginal = std::move(secondOriginal);
        EXPECT_FALSE(secondOriginal.Valid());
        EXPECT_TRUE(second.IsOriginalConditionalFutureReservationFor(firstOriginal, secondParent,
            secondQueue, {request.next.version}, request.rootDecision, request.expectedCurrentOccurrence));
        EXPECT_EQ(2u, Access::OriginalLifetime(first, firstQueue).second);
        Original moved(std::move(firstOriginal));
        EXPECT_FALSE(firstOriginal.Valid());
        EXPECT_TRUE(moved.Valid());
    }
    EXPECT_EQ(1u, Access::OriginalLifetime(second, secondQueue).second);
    ASSERT_TRUE(second.Release(secondQueue, &secondParent));
    EXPECT_FALSE(first.Release(firstQueue, &firstParent));
    retained.Release();
    ASSERT_TRUE(first.Release(firstQueue, &firstParent));
    identities.ReleaseAll();
}

TEST(QueueRuntime, OriginalConditionalReturningAllLoansPreservesNativePrepareAndCommittedExit) {
    using Original = harken::QueueInstanceTable::OriginalConditionalFutureReservation;
    using Access = harken::QueueInstanceTableTestAccess;
    harken::QueueInstanceTable table(1, 1);
    OriginalConditionalNativeIdentities identities;
    harken::QueueInstanceTable::OriginalLifetimePin parent;
    harken::QueueInstanceToken queue{};
    ASSERT_TRUE(table.Acquire(OriginalQueueCreateRequest(), &queue, &parent));
    harken::ConditionalQueuePrepareRequest request{};
    ASSERT_TRUE(identities.Acquire(&request));
    harken::PreparedQueueConditionalV1 prepared;
    Original original;
    ASSERT_TRUE(table.PrepareConditional(queue, request, &prepared, parent, &original));
    original.Release();
    EXPECT_FALSE(original.Valid());
    EXPECT_EQ(2u, Access::OriginalLifetime(table, queue).second);
    EXPECT_FALSE(table.Release(queue, &parent));
    // The true prepared object still owns the original native association.
    harken::ConditionalQueueVersionToken version{};
    ASSERT_TRUE(table.CommitPreparedConditional(&prepared, &version));
    EXPECT_EQ(2u, Access::OriginalLifetime(table, queue).second);
    ASSERT_TRUE(table.ActivateConditional(queue, version, request.rootDecision,
        request.expectedCurrentOccurrence));
    ASSERT_TRUE(table.SettleActivated(queue, version));
    EXPECT_EQ(1u, Access::OriginalLifetime(table, queue).second);
    auto next = request;
    next.expectedCurrentVersion = version.version;
    next.next.version = version.version + 1u;
    ASSERT_TRUE(table.PrepareConditional(queue, next, &prepared, parent, &original));
    original.Release();
    EXPECT_EQ(2u, Access::OriginalLifetime(table, queue).second);
    ASSERT_TRUE(table.CancelPreparedConditional(&prepared));
    EXPECT_EQ(1u, Access::OriginalLifetime(table, queue).second);
    ASSERT_TRUE(table.Release(queue, &parent));
    identities.ReleaseAll();
}

TEST(QueueRuntime, OriginalConditionalNativeLimitsRefuseNewLoansWhileActualOldProgressFinishes) {
    using Original = harken::QueueInstanceTable::OriginalConditionalFutureReservation;
    using Access = harken::QueueInstanceTableTestAccess;
    harken::QueueInstanceTable table(1, 1);
    OriginalConditionalNativeIdentities identities;
    harken::QueueInstanceTable::OriginalLifetimePin parent;
    harken::QueueInstanceToken queue{};
    ASSERT_TRUE(table.Acquire(OriginalQueueCreateRequest(), &queue, &parent));
    harken::ConditionalQueuePrepareRequest request{};
    ASSERT_TRUE(identities.Acquire(&request));
    harken::PreparedQueueConditionalV1 prepared;
    Original original, output;
    Access::SetOriginalLifetimeCount(table, queue, UINT32_MAX);
    const auto parentBefore = Access::Snapshot(table, queue);
    EXPECT_FALSE(table.PrepareConditional(queue, request, &prepared, parent, &output));
    EXPECT_FALSE(prepared.Valid());
    EXPECT_FALSE(output.Valid());
    EXPECT_EQ(parentBefore, Access::Snapshot(table, queue));
    Access::SetOriginalLifetimeCount(table, queue, 1u);
    Access::SetFreeOriginalConditionalGenerationMax(table, 0);
    ASSERT_TRUE(table.PrepareConditional(queue, request, &prepared, parent, &original));
    const auto index = Access::OriginalConditionalIndex(table, queue);
    harken::ConditionalQueueVersionToken version{};
    ASSERT_TRUE(table.CommitPreparedConditional(&prepared, &version));
    Access::SetOriginalConditionalLoanCount(table, index, UINT32_MAX);
    const auto countBefore = Access::OriginalConditionalRecord(table, index);
    EXPECT_FALSE(original.Borrow(&output));
    EXPECT_FALSE(output.Valid());
    EXPECT_EQ(countBefore, Access::OriginalConditionalRecord(table, index));
    ASSERT_TRUE(table.ActivateConditional(queue, version, request.rootDecision, request.expectedCurrentOccurrence));
    ASSERT_TRUE(table.SettleActivated(queue, version));
    EXPECT_TRUE(original.Valid());
    Access::SetOriginalConditionalLoanCount(table, index, 1u);
    original.Release();
    auto next = request;
    next.expectedCurrentVersion = version.version;
    next.next.version = version.version + 1u;
    const auto before = Access::Snapshot(table, queue);
    EXPECT_FALSE(table.PrepareConditional(queue, next, &prepared, parent, &output));
    EXPECT_FALSE(prepared.Valid());
    EXPECT_FALSE(output.Valid());
    EXPECT_EQ(before, Access::Snapshot(table, queue));
    ASSERT_TRUE(table.PrepareConditional(queue, next, &version));
    ASSERT_TRUE(table.ActivateConditional(queue, version, next.rootDecision, next.expectedCurrentOccurrence));
    ASSERT_TRUE(table.SettleActivated(queue, version));
    ASSERT_TRUE(table.Release(queue, &parent));
    // Isolate marker exhaustion from the genuinely quarantined record above.
    harken::QueueInstanceTable markerTable(1, 1);
    harken::QueueInstanceTable::OriginalLifetimePin markerParent;
    harken::QueueInstanceToken markerQueue{};
    ASSERT_TRUE(markerTable.Acquire(OriginalQueueCreateRequest(), &markerQueue, &markerParent));
    Access::SetOriginalConditionalFactoryMarkerMax(markerTable);
    const auto markerBefore = Access::Snapshot(markerTable, markerQueue);
    EXPECT_FALSE(markerTable.PrepareConditional(markerQueue, request, &prepared, markerParent, &output));
    EXPECT_FALSE(prepared.Valid());
    EXPECT_FALSE(output.Valid());
    EXPECT_EQ(markerBefore, Access::Snapshot(markerTable, markerQueue));
    ASSERT_TRUE(markerTable.PrepareConditional(markerQueue, request, &version));
    ASSERT_TRUE(markerTable.ActivateConditional(markerQueue, version, request.rootDecision,
        request.expectedCurrentOccurrence));
    ASSERT_TRUE(markerTable.SettleActivated(markerQueue, version));
    ASSERT_TRUE(markerTable.Release(markerQueue, &markerParent));
    identities.ReleaseAll();
}

} // namespace


namespace {

using OriginalUnboundClock = harken::QueueInstanceTable::OriginalConditionalLaneClock;
using OriginalUnboundClockState = harken::QueueInstanceTable::OriginalConditionalLaneClockState;

// These genuine logical-boundary/empty-group factories grant only storage life.
// No physical Current, native Source ACK, absolute firstS or Ready is fabricated.
struct GenuineUnboundConditionalClockHarness {
    explicit GenuineUnboundConditionalClockHarness(std::uint32_t capacity = 1) : table(capacity, 2) {}
    harken::QueueInstanceTable table;
    OriginalConditionalNativeIdentities identities;
    harken::ScheduledTransactionPool pool;
    harken::TimelineArmTable arms{1};
    harken::QueueInstanceCreateRequest create = OriginalQueueCreateRequest();
    harken::QueueInstanceTable::OriginalLifetimePin parent;
    harken::PreparedQueueInstanceV1 instance;
    harken::QueueInstanceToken queue{};
    harken::ConditionalQueuePrepareRequest request{};
    harken::PreparedQueueConditionalV1 prepared;
    harken::QueueInstanceTable::OriginalConditionalFutureReservation original;
    harken::ConditionalQueueVersionToken version{};
    harken::DecisionToken initialDecision{};
    std::array<harken::Plan03GroupSliceInput, 3> inputs{};
    std::array<harken::Plan03GroupSliceIdentity, 3> raw{};
    std::array<harken::OriginalQueueFutureGroupRegistration, 3> groups{};
    std::array<std::uint64_t, 3> roots{17, 17, 19};
    bool initializing = false;

    void ReserveGroup(std::uint32_t index) {
        raw[index] = {};
        harken::PreparedPrivateImmediateLaneGroup issued;
        ASSERT_TRUE(pool.ReserveImmediateLaneGroup(roots[index], 100, inputs[index], arms,
                                                   &raw[index], &issued));
        harken::PrivateImmediateLaneGroupTransferProof proof;
        ASSERT_TRUE(pool.ProvePrivateImmediateLaneGroupTransfer(issued, roots[index], 100,
            inputs[index], arms, &proof));
        ASSERT_TRUE(pool.ConsumePrivateImmediateLaneGroupTransfer(std::move(proof), &groups[index]));
        ASSERT_TRUE(groups[index].MatchesOriginalScope(roots[index], 100, inputs[index], arms));
    }

    void Prepare(bool initialize = false, bool resetFixture = false, bool lastGeneration = false) {
        initializing = initialize;
        if (resetFixture) {
            create.initial.version = 7;
            create.initial.currentChildOrdinal = 5;
            create.initial.selectionOrdinal = 11;
            create.resetInitial.version = 1;
            create.resetInitial.currentChildOrdinal = 3;
            create.resetInitial.selectionOrdinal = 1;
        }
        if (lastGeneration)
            harken::QueueInstanceTableTestAccess::SetFreeOriginalConditionalGenerationMax(table, 0);
        if (initializing) {
            ASSERT_TRUE(table.PrepareAcquire(create, &instance, &parent));
            queue = instance.Token();
        } else ASSERT_TRUE(table.Acquire(create, &queue, &parent));
        ASSERT_TRUE(identities.Acquire(&request));
        if (resetFixture) {
            request.expectedCurrentVersion = 7;
            request.next = create.resetInitial;
            request.next.version = 8;
        }
        ASSERT_TRUE(table.PrepareConditional(queue, request, &prepared, parent, &original));
        harken::DecisionReserveRequest initial{};
        initial.ticket.sequence = identities.coordinator.Issue();
        initial.world = 7;
        initial.transport = 1;
        initial.expectedContinuationGeneration = 1;
        initial.sliceCount = 1;
        ASSERT_TRUE(identities.decisions.Reserve(initial, &initialDecision));
        identities.decisionTokens.push_back(initialDecision);
        harken::ScheduledTransactionPool::Config config{};
        config.quantumFrames = 100;
        config.scheduleHorizonFrames = 400;
        config.publishLeadQuanta = 1;
        config.maxScheduledTransactions = 4;
        config.maxTransactionSpanFrames = 400;
        config.maxIngressTransactionsPerQuantum = 4;
        config.maxFrameBatchesPerQuantum = 4;
        config.maxEventsPerQuantum = 8;
        config.maxTransactionEvents = 8;
        config.maxCurrentSlicesPerQuantum = 4;
        config.maxPlan03GroupHeadersPerPage = 1;
        config.maxArmsPerGroup = 1;
        config.maxImmediateLaneGroupRows = 3;
        config.maxImmediateLaneOwnerGates = 1;
        config.maxImmediateLaneDecisionGates = 8;
        ASSERT_TRUE(pool.Reset(config, 0, 1));
        inputs[0].executeFrame = 117;
        inputs[0].decision = initialDecision;
        inputs[1].executeFrame = 164;
        inputs[1].decision = request.rootDecision;
        inputs[2] = inputs[0];
        ASSERT_NO_FATAL_FAILURE(ReserveGroup(0));
        ASSERT_NO_FATAL_FAILURE(ReserveGroup(1));
    }

    bool Issue(OriginalUnboundClock* out) {
        return table.PrepareOriginalConditionalLaneClock(original, parent, pool,
            groups[0], groups[1], out);
    }

    void Commit() { ASSERT_TRUE(table.CommitPreparedConditional(&prepared, &version)); }

    void CleanupGroups() {
        for (std::uint32_t i = 0; i < groups.size(); ++i)
            if (groups[i].Valid()) ASSERT_TRUE(pool.CancelPrivateImmediateLaneGroup(
                roots[i], raw[i], &groups[i]));
        ASSERT_TRUE(pool.ResetForEpoch(0, 2));
    }

    void CleanupQueue() {
        original.Release();
        if (initializing) ASSERT_TRUE(table.CancelPreparedAcquire(&instance, &parent));
        else ASSERT_TRUE(table.Release(queue, &parent));
        identities.ReleaseAll();
    }
};

TEST(QueueRuntime, OriginalConditionalUnboundClockRealPreparedIssuerBorrowMoveAndLastReturn) {
    static_assert(!std::is_copy_constructible_v<OriginalUnboundClock>);
    static_assert(!std::is_copy_assignable_v<OriginalUnboundClock>);
    static_assert(!std::is_constructible_v<OriginalUnboundClock, harken::ConditionalQueueVersionToken>);
    GenuineUnboundConditionalClockHarness state;
    ASSERT_NO_FATAL_FAILURE(state.Prepare(true));
    OriginalUnboundClock clock, loan;
    ASSERT_TRUE(state.Issue(&clock));
    ASSERT_TRUE(clock.Valid());
    OriginalUnboundClockState actual = OriginalUnboundClockState::Retired;
    ASSERT_TRUE(clock.CaptureState(&actual));
    EXPECT_EQ(OriginalUnboundClockState::Unbound, actual);
    EXPECT_TRUE(state.table.IsOriginalConditionalLaneClockFor(clock, state.original, state.parent));
    ASSERT_TRUE(clock.Borrow(&loan));
    OriginalUnboundClock moved(std::move(loan));
    EXPECT_FALSE(loan.Valid());
    EXPECT_FALSE(clock.Borrow(&moved));
    EXPECT_FALSE(clock.Borrow(&clock));
    EXPECT_FALSE(clock.Borrow(nullptr));
    ASSERT_TRUE(state.table.CancelPreparedConditional(&state.prepared));
    ASSERT_TRUE(moved.CaptureState(&actual));
    EXPECT_EQ(OriginalUnboundClockState::Retired, actual);
    EXPECT_FALSE(state.table.IsCurrentOriginalConditionalFutureReservation(state.original));
    state.original.Release();
    clock.Release();
    EXPECT_TRUE(moved.Valid());
    EXPECT_FALSE(state.table.CancelPreparedAcquire(&state.instance, &state.parent));
    EXPECT_FALSE(state.pool.CancelPrivateImmediateLaneGroup(17, state.raw[0], &state.groups[0]));
    EXPECT_FALSE(state.pool.CancelPrivateImmediateLaneGroup(17, state.raw[1], &state.groups[1]));
    moved.Release();
    EXPECT_FALSE(moved.CaptureState(&actual));
    EXPECT_EQ(OriginalUnboundClockState::Retired, actual);
    ASSERT_NO_FATAL_FAILURE(state.CleanupGroups());
    ASSERT_NO_FATAL_FAILURE(state.CleanupQueue());
}

TEST(QueueRuntime, OriginalConditionalUnboundClockCommittedOldProgressKeepsNativeRelativeGeometry) {
    GenuineUnboundConditionalClockHarness state;
    ASSERT_NO_FATAL_FAILURE(state.Prepare());
    ASSERT_NO_FATAL_FAILURE(state.Commit());
    OriginalUnboundClock clock, loan;
    ASSERT_TRUE(state.Issue(&clock));
    ASSERT_TRUE(clock.Borrow(&loan));
    harken::QueueInstanceTable::OriginalConditionalFutureReservation originalLoan;
    ASSERT_TRUE(state.original.Borrow(&originalLoan));
    std::array<std::uint64_t, 2> relative{71, 73};
    ASSERT_TRUE(state.pool.CaptureOriginalQueueGroupRelativeFrames(state.groups[0], state.groups[1], &relative));
    EXPECT_EQ((std::array<std::uint64_t, 2>{17, 64}), relative);
    ASSERT_TRUE(state.table.ActivateConditional(state.queue, state.version, state.request.rootDecision,
        state.request.expectedCurrentOccurrence));
    ASSERT_TRUE(state.table.SettleActivated(state.queue, state.version));
    OriginalUnboundClockState actual = OriginalUnboundClockState::Unbound;
    ASSERT_TRUE(clock.CaptureState(&actual));
    EXPECT_EQ(OriginalUnboundClockState::Retired, actual);
    EXPECT_TRUE(state.table.IsOriginalConditionalLaneClockFor(clock, originalLoan, state.parent));
    EXPECT_TRUE(state.groups[0].MatchesOriginalScope(17, 100, state.inputs[0], state.arms));
    EXPECT_TRUE(state.groups[1].MatchesOriginalScope(17, 100, state.inputs[1], state.arms));
    state.original.Release();
    clock.Release();
    loan.Release();
    EXPECT_TRUE(originalLoan.Valid());
    EXPECT_FALSE(state.pool.CancelPrivateImmediateLaneGroup(17, state.raw[1], &state.groups[1]));
    EXPECT_FALSE(state.table.Release(state.queue, &state.parent));
    originalLoan.Release();
    ASSERT_NO_FATAL_FAILURE(state.CleanupGroups());
    ASSERT_NO_FATAL_FAILURE(state.CleanupQueue());
}

TEST(QueueRuntime, OriginalConditionalUnboundClockInitializingRollbackAndFinalGenerationReturn) {
    for (const bool lastGeneration : {false, true}) {
        GenuineUnboundConditionalClockHarness state;
        ASSERT_NO_FATAL_FAILURE(state.Prepare(true, false, lastGeneration));
        const auto record = harken::QueueInstanceTableTestAccess::OriginalConditionalIndex(state.table, state.queue);
        const auto before = harken::QueueInstanceTableTestAccess::OriginalConditionalRecord(state.table, record);
        OriginalUnboundClock clock, loan;
        ASSERT_TRUE(state.Issue(&clock));
        ASSERT_TRUE(clock.Borrow(&loan));
        ASSERT_NO_FATAL_FAILURE(state.Commit());
        ASSERT_TRUE(state.table.RollbackInitializingConditional(state.instance, state.queue,
            state.version, state.request.rootDecision));
        OriginalUnboundClockState actual = OriginalUnboundClockState::Unbound;
        ASSERT_TRUE(loan.CaptureState(&actual));
        EXPECT_EQ(OriginalUnboundClockState::Retired, actual);
        state.original.Release();
        clock.Release();
        EXPECT_FALSE(state.table.CancelPreparedAcquire(&state.instance, &state.parent));
        loan.Release();
        const auto after = harken::QueueInstanceTableTestAccess::OriginalConditionalRecord(state.table, record);
        EXPECT_EQ(0u, after[2]);
        EXPECT_EQ(0u, after[3]);
        if (lastGeneration) {
            EXPECT_EQ(UINT32_MAX, after[1]);
            EXPECT_NE(before[0], after[0]);
        } else EXPECT_EQ(before[1] + 1u, after[1]);
        ASSERT_NO_FATAL_FAILURE(state.CleanupGroups());
        ASSERT_NO_FATAL_FAILURE(state.CleanupQueue());
    }
}

TEST(QueueRuntime, OriginalConditionalUnboundClockRefusesForeignFamilyDecisionParentAndReissue) {
    GenuineUnboundConditionalClockHarness first, second;
    ASSERT_NO_FATAL_FAILURE(first.Prepare());
    ASSERT_NO_FATAL_FAILURE(second.Prepare());
    ASSERT_NO_FATAL_FAILURE(first.ReserveGroup(2));
    OriginalUnboundClock output;
    EXPECT_FALSE(first.table.PrepareOriginalConditionalLaneClock(first.original, second.parent,
        first.pool, first.groups[0], first.groups[1], &output));
    EXPECT_FALSE(second.table.PrepareOriginalConditionalLaneClock(first.original, first.parent,
        first.pool, first.groups[0], first.groups[1], &output));
    EXPECT_FALSE(first.table.PrepareOriginalConditionalLaneClock(first.original, first.parent,
        first.pool, second.groups[0], second.groups[1], &output));
    EXPECT_FALSE(first.table.PrepareOriginalConditionalLaneClock(first.original, first.parent,
        first.pool, first.groups[1], first.groups[0], &output));
    EXPECT_FALSE(first.table.PrepareOriginalConditionalLaneClock(first.original, first.parent,
        first.pool, first.groups[2], first.groups[1], &output));
    EXPECT_FALSE(first.table.PrepareOriginalConditionalLaneClock(first.original, first.parent,
        first.pool, first.groups[1], first.groups[1], &output));
    EXPECT_FALSE(first.Issue(nullptr));
    std::array<std::uint64_t, 2> unchanged{79, 83};
    EXPECT_FALSE(first.pool.CaptureOriginalQueueGroupRelativeFrames(first.groups[2], first.groups[1], &unchanged));
    EXPECT_EQ((std::array<std::uint64_t, 2>{79, 83}), unchanged);
    OriginalUnboundClockState actual = OriginalUnboundClockState::Retired;
    EXPECT_FALSE(output.CaptureState(&actual));
    EXPECT_EQ(OriginalUnboundClockState::Retired, actual);
    ASSERT_TRUE(first.Issue(&output));
    EXPECT_FALSE(second.Issue(&output));
    EXPECT_TRUE(first.table.IsOriginalConditionalLaneClockFor(output, first.original, first.parent));
    output.Release();
    // A native clock association remains present while the original episode lives.
    // Dropping an external handle cannot remint the association from a lookup.
    EXPECT_FALSE(first.Issue(&output));
    ASSERT_TRUE(first.table.CancelPreparedConditional(&first.prepared));
    EXPECT_FALSE(first.Issue(&output));
    first.original.Release();
    ASSERT_NO_FATAL_FAILURE(first.CleanupGroups());
    ASSERT_NO_FATAL_FAILURE(first.CleanupQueue());
    ASSERT_TRUE(second.table.CancelPreparedConditional(&second.prepared));
    ASSERT_NO_FATAL_FAILURE(second.CleanupQueue());
    ASSERT_NO_FATAL_FAILURE(second.CleanupGroups());
}

TEST(QueueRuntime, OriginalConditionalUnboundClockNativeCountLimitsReturnRealPrefixesAndOldEnd) {
    using Access = harken::QueueInstanceTableTestAccess;
    GenuineUnboundConditionalClockHarness state;
    ASSERT_NO_FATAL_FAILURE(state.Prepare());
    const auto record = Access::OriginalConditionalIndex(state.table, state.queue);
    Access::SetOriginalConditionalLoanCount(state.table, record, UINT32_MAX);
    OriginalUnboundClock output;
    EXPECT_FALSE(state.Issue(&output));
    EXPECT_FALSE(output.Valid());
    // Real disposal proves both provisional group Borrow prefixes were returned.
    ASSERT_TRUE(state.pool.CancelPrivateImmediateLaneGroup(17, state.raw[0], &state.groups[0]));
    ASSERT_TRUE(state.pool.CancelPrivateImmediateLaneGroup(17, state.raw[1], &state.groups[1]));
    Access::SetOriginalConditionalLoanCount(state.table, record, 1u);
    ASSERT_NO_FATAL_FAILURE(state.ReserveGroup(0));
    ASSERT_NO_FATAL_FAILURE(state.ReserveGroup(1));
    ASSERT_TRUE(state.Issue(&output));
    ASSERT_NO_FATAL_FAILURE(state.Commit());
    Access::SetOriginalConditionalLoanCount(state.table, record, UINT32_MAX);
    OriginalUnboundClock refused;
    EXPECT_FALSE(output.Borrow(&refused));
    EXPECT_FALSE(refused.Valid());
    ASSERT_TRUE(state.table.RetireConditionalForAbort(state.queue, state.version, state.request.rootDecision));
    OriginalUnboundClockState actual = OriginalUnboundClockState::Unbound;
    ASSERT_TRUE(output.CaptureState(&actual));
    EXPECT_EQ(OriginalUnboundClockState::Retired, actual);
    Access::SetOriginalConditionalLoanCount(state.table, record, 2u);
    state.original.Release();
    output.Release();
    ASSERT_NO_FATAL_FAILURE(state.CleanupGroups());
    ASSERT_NO_FATAL_FAILURE(state.CleanupQueue());
}

TEST(QueueRuntime, OriginalConditionalUnboundClockActualResetReplacementRetainsOriginalFamily) {
    GenuineUnboundConditionalClockHarness state;
    ASSERT_NO_FATAL_FAILURE(state.Prepare(false, true));
    ASSERT_NO_FATAL_FAILURE(state.Commit());
    OriginalUnboundClock clock, loan;
    ASSERT_TRUE(state.Issue(&clock));
    ASSERT_TRUE(clock.Borrow(&loan));
    auto replacement = state.request;
    harken::ConditionalQueuePrepareRequest newIdentity{};
    ASSERT_TRUE(state.identities.Acquire(&newIdentity));
    replacement.rootDecision = newIdentity.rootDecision;
    replacement.projectedStates = &replacement.next;
    replacement.projectedStateCount = 1;
    harken::PreparedQueueConditionalReplacementV1 replacing;
    ASSERT_EQ(harken::QueueResetStatusV1::Ok, state.table.PrepareConditionalReplacement(
        state.queue, state.version, state.request.rootDecision, replacement, &replacing));
    harken::QueueResetTargetV1 target{};
    target.world = state.create.world;
    target.containerStableId = state.create.containerStableId;
    target.programSemanticFingerprint = state.create.programSemanticFingerprint;
    harken::PreparedQueueResetSetV1 reset;
    ASSERT_EQ(harken::QueueResetStatusV1::Ok, state.table.PrepareReset(target, &reset));
    ASSERT_EQ(harken::QueueResetStatusV1::Ok, state.table.CommitPreparedConditionalReplacement(&replacing, 61));
    ASSERT_EQ(harken::QueueResetStatusV1::Ok, state.table.CommitPreparedReset(&reset, 61));
    OriginalUnboundClockState actual = OriginalUnboundClockState::Unbound;
    ASSERT_TRUE(loan.CaptureState(&actual));
    EXPECT_EQ(OriginalUnboundClockState::Retired, actual);
    EXPECT_TRUE(state.table.IsOriginalConditionalLaneClockFor(loan, state.original, state.parent));
    EXPECT_FALSE(state.table.IsCurrentOriginalConditionalFutureReservation(state.original));
    std::array<std::uint64_t, 2> relative{};
    ASSERT_TRUE(state.pool.CaptureOriginalQueueGroupRelativeFrames(state.groups[0], state.groups[1], &relative));
    EXPECT_EQ((std::array<std::uint64_t, 2>{17, 64}), relative);
    ASSERT_TRUE(state.table.ActivateConditional(state.queue, state.version, replacement.rootDecision,
        replacement.expectedCurrentOccurrence));
    ASSERT_TRUE(state.table.SettleActivated(state.queue, state.version));
    state.original.Release();
    clock.Release();
    EXPECT_FALSE(state.table.Release(state.queue, &state.parent));
    loan.Release();
    ASSERT_NO_FATAL_FAILURE(state.CleanupGroups());
    ASSERT_NO_FATAL_FAILURE(state.CleanupQueue());
}

TEST(QueueRuntime, NativeResetScopeRequiresRealCurrentClockAndRemainderCannotSkipOrdinaryPendingRows) {
    GenuineUnboundConditionalClockHarness state(3);
    ASSERT_NO_FATAL_FAILURE(state.Prepare());
    ASSERT_NO_FATAL_FAILURE(state.Commit());
    harken::QueueResetTargetV1 target{};
    target.world = state.create.world;
    target.containerStableId = state.create.containerStableId;
    target.programSemanticFingerprint = state.create.programSemanticFingerprint;
    harken::QueueInstanceTable::PreparedCatalogResetScope scope;
    ASSERT_EQ(harken::QueueResetStatusV1::Ok,
        state.table.PrepareNativeConditionalCatalogResetScope(target, &scope));
    EXPECT_EQ(0u, scope.MatchedCount());
    scope.Release();
    OriginalUnboundClock clock;
    ASSERT_TRUE(state.Issue(&clock));
    harken::QueueInstanceToken ordinary{};
    ASSERT_TRUE(state.table.Acquire(state.create, &ordinary));
    harken::ConditionalQueueVersionToken ordinaryVersion{};
    ASSERT_TRUE(state.table.PrepareConditional(ordinary, state.request, &ordinaryVersion));
    ASSERT_EQ(harken::QueueResetStatusV1::Ok,
        state.table.PrepareNativeConditionalCatalogResetScope(target, &scope));
    EXPECT_EQ(1u, scope.MatchedCount());
    harken::QueueInstanceTable::PreparedCatalogResetScope moved(std::move(scope));
    scope = std::move(moved);
    ASSERT_EQ(harken::QueueResetStatusV1::Ok, state.table.ExtendCatalogResetScope(scope, target));
    EXPECT_EQ(1u, scope.MatchedCount());
    harken::PreparedQueueResetSetV1 remainder;
    EXPECT_EQ(harken::QueueResetStatusV1::Backpressure,
        state.table.PrepareResetRemainder(target, scope, &remainder));
    EXPECT_FALSE(remainder.Valid());
    scope.Release();
    ASSERT_EQ(harken::QueueResetStatusV1::Ok, state.table.PrepareCatalogResetScope(target, &scope));
    EXPECT_EQ(2u, scope.MatchedCount());
    EXPECT_EQ(harken::QueueResetStatusV1::InvalidInput,
        state.table.PrepareResetRemainder(target, scope, &remainder));
    scope.Release();
    ASSERT_TRUE(state.table.ActivateConditional(ordinary, ordinaryVersion,
        state.request.rootDecision, state.request.expectedCurrentOccurrence));
    ASSERT_TRUE(state.table.SettleActivated(ordinary, ordinaryVersion));
    ASSERT_TRUE(state.table.Release(ordinary));
    ASSERT_TRUE(state.table.ActivateConditional(state.queue, state.version,
        state.request.rootDecision, state.request.expectedCurrentOccurrence));
    ASSERT_TRUE(state.table.SettleActivated(state.queue, state.version));
    clock.Release();
    state.original.Release();
    ASSERT_NO_FATAL_FAILURE(state.CleanupGroups());
    ASSERT_NO_FATAL_FAILURE(state.CleanupQueue());
}

TEST(QueueRuntime, NativeResetRemainderRollbackReturnsOnlyItsRealReservations) {
    GenuineUnboundConditionalClockHarness state(2);
    ASSERT_NO_FATAL_FAILURE(state.Prepare());
    ASSERT_NO_FATAL_FAILURE(state.Commit());
    OriginalUnboundClock clock;
    ASSERT_TRUE(state.Issue(&clock));
    harken::QueueInstanceToken ordinary{};
    ASSERT_TRUE(state.table.Acquire(state.create, &ordinary));
    harken::QueueResetTargetV1 target{};
    target.world = state.create.world;
    target.containerStableId = state.create.containerStableId;
    target.programSemanticFingerprint = state.create.programSemanticFingerprint;
    harken::QueueInstanceTable::PreparedCatalogResetScope scope;
    ASSERT_EQ(harken::QueueResetStatusV1::Ok,
        state.table.PrepareNativeConditionalCatalogResetScope(target, &scope));
    harken::PreparedQueueResetSetV1 remainder;
    ASSERT_EQ(harken::QueueResetStatusV1::Ok,
        state.table.PrepareResetRemainder(target, scope, &remainder));
    EXPECT_EQ(1u, remainder.MatchedCount());
    EXPECT_EQ(harken::QueueResetStatusV1::InvalidInput,
        state.table.CommitPreparedReset(&remainder, state.identities.coordinator.Issue()));
    ASSERT_EQ(harken::QueueResetStatusV1::Ok, state.table.CancelPreparedReset(&remainder));
    harken::QueueInstanceSnapshot snapshot{};
    ASSERT_TRUE(state.table.Capture(ordinary, &snapshot));
    EXPECT_FALSE(snapshot.resetPending);
    EXPECT_TRUE(state.table.IsCurrentOriginalConditionalFutureReservation(state.original));
    ASSERT_EQ(harken::QueueResetStatusV1::Ok,
        state.table.PrepareResetRemainder(target, scope, &remainder));
    scope.Release();
    EXPECT_EQ(harken::QueueResetStatusV1::InvalidInput,
        state.table.CommitPreparedReset(&remainder, state.identities.coordinator.Issue()));
    ASSERT_EQ(harken::QueueResetStatusV1::Ok, state.table.CancelPreparedReset(&remainder));
    ASSERT_TRUE(state.table.Release(ordinary));
    ASSERT_TRUE(state.table.ActivateConditional(state.queue, state.version,
        state.request.rootDecision, state.request.expectedCurrentOccurrence));
    ASSERT_TRUE(state.table.SettleActivated(state.queue, state.version));
    clock.Release();
    state.original.Release();
    ASSERT_NO_FATAL_FAILURE(state.CleanupGroups());
    ASSERT_NO_FATAL_FAILURE(state.CleanupQueue());
}


TEST(QueueRuntime, OriginalConditionalUnboundClockMoveOverwriteAndDestructionReturnActualReceiver) {
    GenuineUnboundConditionalClockHarness first, second;
    ASSERT_NO_FATAL_FAILURE(first.Prepare());
    ASSERT_NO_FATAL_FAILURE(second.Prepare());
    OriginalUnboundClock firstClock, secondClock, secondLoan;
    ASSERT_TRUE(first.Issue(&firstClock));
    ASSERT_TRUE(second.Issue(&secondClock));
    ASSERT_TRUE(secondClock.Borrow(&secondLoan));
    ASSERT_TRUE(first.table.CancelPreparedConditional(&first.prepared));
    ASSERT_TRUE(second.table.CancelPreparedConditional(&second.prepared));
    firstClock = std::move(secondClock);
    EXPECT_FALSE(secondClock.Valid());
    EXPECT_TRUE(firstClock.Valid());
    EXPECT_FALSE(first.table.IsOriginalConditionalLaneClockFor(firstClock, first.original, first.parent));
    EXPECT_TRUE(second.table.IsOriginalConditionalLaneClockFor(firstClock, second.original, second.parent));
    // The overwritten native loan returned only to its actual first receiver.
    first.original.Release();
    ASSERT_NO_FATAL_FAILURE(first.CleanupGroups());
    ASSERT_NO_FATAL_FAILURE(first.CleanupQueue());
    second.original.Release();
    firstClock.Release();
    EXPECT_TRUE(secondLoan.Valid());
    EXPECT_FALSE(second.table.Release(second.queue, &second.parent));
    EXPECT_FALSE(second.pool.CancelPrivateImmediateLaneGroup(17, second.raw[0], &second.groups[0]));
    {
        OriginalUnboundClock last(std::move(secondLoan));
        EXPECT_FALSE(secondLoan.Valid());
        EXPECT_TRUE(last.Valid());
        OriginalUnboundClockState actual = OriginalUnboundClockState::Unbound;
        ASSERT_TRUE(last.CaptureState(&actual));
        EXPECT_EQ(OriginalUnboundClockState::Retired, actual);
    }
    ASSERT_NO_FATAL_FAILURE(second.CleanupGroups());
    ASSERT_NO_FATAL_FAILURE(second.CleanupQueue());
}

} // namespace

#include "harken/core/PrivateCatalogStage.h"

#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
namespace harken {
// Negative prerequisites only; no successful origin is issued by this helper.
struct PrivateCatalogStagePoolTestAccess {
    static std::uint64_t ExchangeWord(PrivateCatalogStagePool& pool,
            const PrivateCatalogStagePool::Owner& owner, std::uint64_t word) {
        assert(owner.Valid() && owner.pool_ == &pool);
        return pool.rows_[owner.index_].word.exchange(word);
    }
    static std::uint64_t CaptureWord(PrivateCatalogStagePool& pool,
            const PrivateCatalogStagePool::Owner& owner) {
        assert(owner.Valid() && owner.pool_ == &pool);
        return pool.rows_[owner.index_].word.load();
    }
    static std::uint64_t ParentWord(PrivateCatalogStagePool& pool,
            const PrivateCatalogStagePool::Owner& owner) {
        assert(owner.Valid() && owner.pool_ == &pool);
        return pool.rows_[owner.index_].nativeParentWord.load();
    }
    static std::uint64_t ExchangeParentWord(PrivateCatalogStagePool& pool,
            const PrivateCatalogStagePool::Owner& owner, std::uint64_t word) {
        assert(owner.Valid() && owner.pool_ == &pool);
        return pool.rows_[owner.index_].nativeParentWord.exchange(word);
    }
    static std::uint32_t ExchangeStructural(PrivateCatalogStagePool& pool,
            const PrivateCatalogStagePool::Owner& owner, std::uint32_t count) {
        assert(owner.Valid() && owner.pool_ == &pool);
        return pool.rows_[owner.index_].structural.exchange(count);
    }
    static std::pair<std::uint64_t, std::uint64_t> Episode(PrivateCatalogStagePool& pool,
            const PrivateCatalogStagePool::Owner& owner) {
        assert(owner.Valid() && owner.pool_ == &pool);
        return {owner.generation_, owner.marker_};
    }
    static void ExhaustMarker(PrivateCatalogStagePool& pool) { pool.nextMarker_ = UINT64_MAX; }
    static void LastFreeGeneration(PrivateCatalogStagePool& pool) {
        assert(pool.rows_[0].word.load() == 0 && pool.rows_[0].structural.load() == 0);
        pool.rows_[0].generation.store(UINT64_MAX - 1);
    }
};
} // namespace harken
#endif

namespace {
using CatalogStage = harken::PrivateCatalogStagePool;

struct GenuineCatalogStageHarness {
    harken::QueueInstanceTable queues{1};
    harken::TimelineArmTable arms{1};
    harken::LogicCommitCoordinator coordinator;
    harken::DecisionTable decisions{2, 2};
    harken::ScheduledTransactionPool groups;
    std::array<harken::OriginalQueueFutureGroupRegistration, 2> originals;
    std::array<harken::Plan03GroupSliceIdentity, 2> identities{};
    harken::DecisionToken decision{};
    harken::QueueInstanceToken queue{};
    harken::PreparedQueueInstanceV1 initialAcquire;
    CatalogStage stage{1};
    harken::QueueInstanceTable::PreparedCatalogResetScope scope;
    harken::LogicCommitCoordinator::PreparedIssue sequence;

    void Prepare(bool initializing = false) {
        harken::QueueInstanceCreateRequest create{};
        create.world = 7; create.rootInstance = 17;
        create.containerStableId = harkentest::ObjectId(13);
        create.programSemanticFingerprint = 19; create.childCount = 2;
        create.initial.version = 1; create.initial.currentChildOrdinal = 3;
        if (initializing) {
            ASSERT_TRUE(queues.PrepareAcquire(create, &initialAcquire));
            queue = initialAcquire.Token();
        } else ASSERT_TRUE(queues.Acquire(create, &queue));
        harken::QueueResetTargetV1 target{};
        target.world = create.world; target.containerStableId = create.containerStableId;
        target.programSemanticFingerprint = create.programSemanticFingerprint;
        ASSERT_EQ(harken::QueueResetStatusV1::Ok, queues.PrepareCatalogResetScope(target, &scope));
        ASSERT_EQ(1u, scope.MatchedCount());
        harken::DecisionReserveRequest request{};
        request.ticket.sequence = coordinator.Issue();
        request.world = 7; request.transport = 1;
        request.expectedContinuationGeneration = 1; request.sliceCount = 2;
        ASSERT_TRUE(decisions.Reserve(request, &decision));
        harken::ScheduledTransactionPool::Config config{};
        config.quantumFrames = 100; config.scheduleHorizonFrames = 400;
        config.publishLeadQuanta = 1; config.maxScheduledTransactions = 4;
        config.maxTransactionSpanFrames = 400; config.maxIngressTransactionsPerQuantum = 4;
        config.maxFrameBatchesPerQuantum = 4; config.maxEventsPerQuantum = 8;
        config.maxTransactionEvents = 8; config.maxCurrentSlicesPerQuantum = 4;
        config.maxPlan03GroupHeadersPerPage = 1; config.maxArmsPerGroup = 1;
        config.maxImmediateLaneGroupRows = 2;
        config.maxImmediateLaneOwnerGates = 1; config.maxImmediateLaneDecisionGates = 2;
        ASSERT_TRUE(groups.Reset(config, 0, 1));
        for (std::uint32_t i = 0; i < originals.size(); ++i) {
            harken::Plan03GroupSliceInput input{};
            input.decision = decision; input.sliceIndex = i; input.executeFrame = 100 + 100 * i;
            harken::PreparedPrivateImmediateLaneGroup original;
            ASSERT_TRUE(groups.ReserveImmediateLaneGroup(17, 100, input, arms, &identities[i], &original));
            harken::PrivateImmediateLaneGroupTransferProof proof;
            ASSERT_TRUE(groups.ProvePrivateImmediateLaneGroupTransfer(original, 17, 100, input, arms, &proof));
            ASSERT_TRUE(groups.ConsumePrivateImmediateLaneGroupTransfer(std::move(proof), &originals[i]));
        }
        ASSERT_TRUE(groups.IsOriginalQueueFutureGroupFamily(originals[0], originals[1]));
        ASSERT_EQ(harken::LogicCommitCoordinator::IssueStatus::Issued, coordinator.PrepareIssue(&sequence));
    }
    CatalogStage::Result Issue(CatalogStage::Owner* out) {
        return stage.Reserve(queues, &scope, &sequence, groups, originals[0], originals[1], out);
    }
    void Cleanup() {
        scope.Release(); sequence.Release();
        stage.ReclaimClosed();
        for (std::uint32_t i = 0; i < originals.size(); ++i)
            ASSERT_TRUE(groups.CancelPrivateImmediateLaneGroup(17, identities[i], &originals[i]));
        ASSERT_TRUE(groups.ResetForEpoch(0, 2));
        ASSERT_TRUE(decisions.Cancel(decision));
        ASSERT_TRUE(decisions.ReclaimCancelled(decision));
        if (initialAcquire.Valid()) ASSERT_TRUE(queues.CancelPreparedAcquire(&initialAcquire));
        else ASSERT_TRUE(queues.Release(queue));
    }
};

TEST(QueueRuntime, PrivateCatalogStageGenuineOriginsReserveAndNormalNoLinkLoans) {
    static_assert(!std::is_copy_constructible_v<CatalogStage::Owner>);
    static_assert(!std::is_constructible_v<CatalogStage::Owner, harken::QueueInstanceToken>);
    GenuineCatalogStageHarness actual;
    ASSERT_NO_FATAL_FAILURE(actual.Prepare());
    CatalogStage::Owner owner;
    ASSERT_EQ(CatalogStage::Result::Ok, actual.Issue(&owner));
    EXPECT_FALSE(actual.scope.Valid()); EXPECT_FALSE(actual.sequence.Valid());
    EXPECT_EQ(2u, actual.stage.ReservedSequence(owner));
    EXPECT_EQ(1u, actual.coordinator.Current());
    harken::LogicCommitSequence next = 0;
    EXPECT_EQ(harken::LogicCommitCoordinator::IssueStatus::Busy, actual.coordinator.TryIssue(&next));
    CatalogStage::Loan storage;
    CatalogStage::Read read;
    ASSERT_TRUE(actual.stage.Borrow(owner, &storage));
    ASSERT_EQ(CatalogStage::Result::Ok, actual.stage.BorrowRead(storage, &read));
    owner.Release();
    EXPECT_EQ(harken::LogicCommitCoordinator::IssueStatus::Issued, actual.coordinator.TryIssue(&next));
    EXPECT_EQ(2u, next);
    EXPECT_EQ(0u, actual.stage.ReclaimClosed());
    EXPECT_FALSE(actual.queues.Release(actual.queue));
    EXPECT_FALSE(actual.groups.CancelPrivateImmediateLaneGroup(17, actual.identities[0], &actual.originals[0]));
    storage.Release();
    EXPECT_EQ(0u, actual.stage.ReclaimClosed());
    read.Release();
    EXPECT_EQ(1u, actual.stage.ReclaimClosed());
    ASSERT_NO_FATAL_FAILURE(actual.Cleanup());
}

TEST(QueueRuntime, PrivateCatalogStageSnapshotExcludesExistingWriterButOldContinues) {
    GenuineCatalogStageHarness actual;
    ASSERT_NO_FATAL_FAILURE(actual.Prepare(true));
    CatalogStage::Owner owner;
    ASSERT_EQ(CatalogStage::Result::Ok, actual.Issue(&owner));
    CatalogStage::Mutation existing;
    ASSERT_EQ(CatalogStage::Result::Ok, actual.stage.BorrowMutation(owner, &existing));
    CatalogStage::Snapshot snapshot;
    EXPECT_EQ(CatalogStage::Result::Busy, actual.stage.BeginSnapshot(owner, &snapshot));
    existing.Release();
    ASSERT_EQ(CatalogStage::Result::Ok, actual.stage.BeginSnapshot(owner, &snapshot));
    CatalogStage::Mutation later;
    ASSERT_EQ(CatalogStage::Result::Ok, actual.stage.BorrowMutation(owner, &later));
    // A real native Old write remains legal while the snapshot holds count1.
    harken::QueueInstanceToken committed{};
    ASSERT_TRUE(actual.queues.CommitPreparedAcquire(&actual.initialAcquire, &committed));
    EXPECT_EQ(actual.queue.slot, committed.slot);
    EXPECT_EQ(actual.queue.generation, committed.generation);
    harken::QueueInstanceSnapshot publishedOld{};
    ASSERT_TRUE(actual.queues.Capture(committed, &publishedOld));
    EXPECT_EQ(17u, publishedOld.rootInstance);
    CatalogStage::CompareProof compare;
    EXPECT_FALSE(actual.stage.FinishSnapshot(&snapshot, &compare));
    EXPECT_TRUE(snapshot.Valid());
    later.Release();
    EXPECT_FALSE(actual.stage.FinishSnapshot(&snapshot, &compare));
    snapshot.Release();
    ASSERT_EQ(CatalogStage::Result::Ok, actual.stage.BeginSnapshot(owner, &snapshot));
    ASSERT_TRUE(actual.stage.FinishSnapshot(&snapshot, &compare));
    CatalogStage::QuiescentComparison receipt;
    ASSERT_TRUE(actual.stage.TryCompare(&compare, &receipt));
    EXPECT_FALSE(compare.Valid());
    ASSERT_EQ(CatalogStage::Result::Ok, actual.stage.BorrowMutation(owner, &later));
    EXPECT_TRUE(receipt.Valid()); // Historical retention, never a New seal.
    later.Release(); receipt.Release(); owner.Release();
    ASSERT_NO_FATAL_FAILURE(actual.Cleanup());
}

TEST(QueueRuntime, PrivateCatalogStageCompareRejectsChangedEpochAndPreservesProof) {
    GenuineCatalogStageHarness actual;
    ASSERT_NO_FATAL_FAILURE(actual.Prepare());
    CatalogStage::Owner owner;
    ASSERT_EQ(CatalogStage::Result::Ok, actual.Issue(&owner));
    CatalogStage::Snapshot snapshot;
    CatalogStage::CompareProof compare;
    ASSERT_EQ(CatalogStage::Result::Ok, actual.stage.BeginSnapshot(owner, &snapshot));
    ASSERT_TRUE(actual.stage.FinishSnapshot(&snapshot, &compare));
    CatalogStage::Read read;
    CatalogStage::Mutation writer;
    ASSERT_EQ(CatalogStage::Result::Ok, actual.stage.BorrowRead(owner, &read));
    ASSERT_TRUE(actual.stage.Upgrade(&read, &writer));
    EXPECT_FALSE(read.Valid());
    writer.Release();
    CatalogStage::QuiescentComparison receipt;
    EXPECT_FALSE(actual.stage.TryCompare(&compare, &receipt));
    EXPECT_TRUE(compare.Valid()); EXPECT_FALSE(receipt.Valid());
    ASSERT_TRUE(actual.stage.Abort(owner));
    EXPECT_EQ(1u, actual.coordinator.Current());
    owner.Release();
    EXPECT_EQ(0u, actual.stage.ReclaimClosed());
    compare.Release();
    EXPECT_EQ(1u, actual.stage.ReclaimClosed());
    ASSERT_NO_FATAL_FAILURE(actual.Cleanup());
}

TEST(QueueRuntime, PrivateCatalogStageForeignReceiverAndOccupiedOutputKeepRealInputs) {
    GenuineCatalogStageHarness first, second;
    ASSERT_NO_FATAL_FAILURE(first.Prepare());
    ASSERT_NO_FATAL_FAILURE(second.Prepare());
    CatalogStage::Owner owner;
    EXPECT_EQ(CatalogStage::Result::Invalid, first.stage.Reserve(second.queues, &first.scope,
        &first.sequence, first.groups, first.originals[0], first.originals[1], &owner));
    EXPECT_EQ(CatalogStage::Result::Invalid, first.stage.Reserve(first.queues, &first.scope,
        &first.sequence, second.groups, first.originals[0], first.originals[1], &owner));
    ASSERT_TRUE(first.scope.Valid()); ASSERT_TRUE(first.sequence.Valid());
    ASSERT_EQ(CatalogStage::Result::Ok, first.Issue(&owner));
    EXPECT_EQ(CatalogStage::Result::Invalid, second.Issue(&owner));
    EXPECT_TRUE(second.scope.Valid()); EXPECT_TRUE(second.sequence.Valid());
    CatalogStage::Loan loan;
    EXPECT_FALSE(second.stage.Borrow(owner, &loan));
    ASSERT_TRUE(first.stage.Borrow(owner, &loan));
    EXPECT_FALSE(first.stage.Borrow(owner, &loan));
    EXPECT_FALSE(first.stage.Borrow(owner, static_cast<CatalogStage::Loan*>(nullptr)));
    loan.Release(); owner.Release();
    ASSERT_NO_FATAL_FAILURE(first.Cleanup());
    ASSERT_NO_FATAL_FAILURE(second.Cleanup());
}

TEST(QueueRuntime, PrivateCatalogStageMovesOverwriteAndDestructionReturnRealOrigins) {
    GenuineCatalogStageHarness first, second;
    ASSERT_NO_FATAL_FAILURE(first.Prepare());
    ASSERT_NO_FATAL_FAILURE(second.Prepare());
    CatalogStage::Owner a, b;
    ASSERT_EQ(CatalogStage::Result::Ok, first.Issue(&a));
    ASSERT_EQ(CatalogStage::Result::Ok, second.Issue(&b));
    CatalogStage::Loan held;
    ASSERT_TRUE(first.stage.Borrow(a, &held));
    a = std::move(b);
    EXPECT_FALSE(b.Valid()); EXPECT_TRUE(a.Valid());
    EXPECT_EQ(0u, first.stage.ReclaimClosed());
    held.Release(); EXPECT_EQ(1u, first.stage.ReclaimClosed());
    {
        CatalogStage::Owner final(std::move(a));
        EXPECT_FALSE(a.Valid()); EXPECT_TRUE(final.Valid());
    }
    EXPECT_EQ(1u, second.stage.ReclaimClosed());
    ASSERT_NO_FATAL_FAILURE(first.Cleanup());
    ASSERT_NO_FATAL_FAILURE(second.Cleanup());
}

#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
TEST(QueueRuntime, PrivateCatalogStageCountMaximumRefusesWithoutLosingActualOrigin) {
    GenuineCatalogStageHarness actual;
    ASSERT_NO_FATAL_FAILURE(actual.Prepare());
    CatalogStage::Owner owner;
    ASSERT_EQ(CatalogStage::Result::Ok, actual.Issue(&owner));
    const auto before = harken::PrivateCatalogStagePoolTestAccess::CaptureWord(actual.stage, owner);
    harken::PrivateCatalogStagePoolTestAccess::ExchangeWord(actual.stage, owner,
        before | CatalogStage::kCountMask);
    CatalogStage::Read refused;
    EXPECT_EQ(CatalogStage::Result::Busy, actual.stage.BorrowRead(owner, &refused));
    EXPECT_FALSE(refused.Valid());
    harken::PrivateCatalogStagePoolTestAccess::ExchangeWord(actual.stage, owner, before);
    ASSERT_EQ(CatalogStage::Result::Ok, actual.stage.BorrowRead(owner, &refused));
    refused.Release(); owner.Release();
    ASSERT_NO_FATAL_FAILURE(actual.Cleanup());
}

TEST(QueueRuntime, PrivateCatalogStageEpochMaximumAbortsCandidateButAllowsOldMutation) {
    GenuineCatalogStageHarness actual;
    ASSERT_NO_FATAL_FAILURE(actual.Prepare());
    CatalogStage::Owner owner;
    ASSERT_EQ(CatalogStage::Result::Ok, actual.Issue(&owner));
    const auto before = harken::PrivateCatalogStagePoolTestAccess::CaptureWord(actual.stage, owner);
    harken::PrivateCatalogStagePoolTestAccess::ExchangeWord(actual.stage, owner,
        (CatalogStage::kEpochMax << CatalogStage::kEpochShift) | (before & CatalogStage::kStateMask));
    CatalogStage::Mutation write;
    ASSERT_EQ(CatalogStage::Result::Ok, actual.stage.BorrowMutation(owner, &write));
    CatalogStage::State state = CatalogStage::State::Free;
    ASSERT_TRUE(actual.stage.CaptureState(owner, &state));
    EXPECT_EQ(CatalogStage::State::AbortedOld, state);
    write.Release();
    ASSERT_EQ(CatalogStage::Result::Ok, actual.stage.BorrowMutation(owner, &write));
    write.Release();
    CatalogStage::Snapshot refused;
    EXPECT_EQ(CatalogStage::Result::Busy, actual.stage.BeginSnapshot(owner, &refused));
    ASSERT_TRUE(actual.stage.Abort(owner));
    EXPECT_EQ(1u, actual.coordinator.Current());
    owner.Release();
    ASSERT_NO_FATAL_FAILURE(actual.Cleanup());
}

TEST(QueueRuntime, PrivateCatalogStageMarkerMaximumKeepsUnconsumedFactoryInputs) {
    GenuineCatalogStageHarness actual;
    ASSERT_NO_FATAL_FAILURE(actual.Prepare());
    harken::PrivateCatalogStagePoolTestAccess::ExhaustMarker(actual.stage);
    CatalogStage::Owner refused;
    EXPECT_EQ(CatalogStage::Result::Exhausted, actual.Issue(&refused));
    EXPECT_TRUE(actual.scope.Valid()); EXPECT_TRUE(actual.sequence.Valid());
    EXPECT_FALSE(refused.Valid());
    ASSERT_NO_FATAL_FAILURE(actual.Cleanup());
}

TEST(QueueRuntime, PrivateCatalogStageLastActualGenerationQuarantinesWithoutReuse) {
    GenuineCatalogStageHarness actual;
    ASSERT_NO_FATAL_FAILURE(actual.Prepare());
    harken::PrivateCatalogStagePoolTestAccess::LastFreeGeneration(actual.stage);
    CatalogStage::Owner last;
    ASSERT_EQ(CatalogStage::Result::Ok, actual.Issue(&last));
    EXPECT_TRUE(last.Valid());
    last.Release();
    ASSERT_EQ(1u, actual.stage.ReclaimClosed());
    harken::QueueResetTargetV1 target{};
    target.world = 7; target.containerStableId = harkentest::ObjectId(13);
    target.programSemanticFingerprint = 19;
    ASSERT_EQ(harken::QueueResetStatusV1::Ok, actual.queues.PrepareCatalogResetScope(target, &actual.scope));
    ASSERT_EQ(harken::LogicCommitCoordinator::IssueStatus::Issued,
        actual.coordinator.PrepareIssue(&actual.sequence));
    CatalogStage::Owner refused;
    EXPECT_EQ(CatalogStage::Result::Busy, actual.Issue(&refused));
    EXPECT_FALSE(refused.Valid());
    EXPECT_TRUE(actual.scope.Valid()); EXPECT_TRUE(actual.sequence.Valid());
    ASSERT_NO_FATAL_FAILURE(actual.Cleanup());
}
#endif
} // namespace

namespace harken::detail {
struct PrivateSelectedQueueFutureSpanTestAccess {
    static void MarkerLimit(PrivateSelectedQueueFutureSpanPool& pool, bool exhausted) {
        pool.nextMarker_ = exhausted ? UINT64_MAX : 0;
    }
    static void LoanLimit(PrivateSelectedQueueFutureSpanPool& pool,
        std::uint32_t slot, std::uint32_t count) {
        pool.rows_[slot].loans = count;
    }
    static void GenerationLimit(PrivateSelectedQueueFutureSpanPool& pool) {
        ASSERT_TRUE(pool.rows_.Emplace(0));
        pool.rows_[0].generation = UINT32_MAX;
    }
};
}

namespace {
using SelectedFuturePool = harken::PrivateSelectedQueueFutureSpanPool;

// These are real private Core factories. No Source install, Lane publication,
// firstS, physical Current or shared Catalog selection is asserted here.
struct GenuineHiddenSelectedFuture {
    enum class SuccessorForm { PreparedControl, RetainedCurrent, PreparedSource };
    harken::QueueInstanceTable queues{1, 2};
    OriginalConditionalNativeIdentities old;
    harken::ScheduledTransactionPool pool;
    harken::TimelineArmTable arms{2};
    harken::QueueInstanceCreateRequest create = OriginalQueueCreateRequest();
    harken::QueueInstanceTable::OriginalLifetimePin parent;
    harken::PreparedQueueInstanceV1 instance;
    harken::QueueInstanceToken queue{};
    harken::ConditionalQueuePrepareRequest request{};
    harken::PreparedQueueConditionalV1 conditional;
    harken::QueueInstanceTable::OriginalConditionalFutureReservation original;
    harken::QueueInstanceTable::OriginalConditionalLaneClock clock;
    harken::QueueInstanceTable::PreparedCatalogResetScope scope;
    harken::ConditionalQueueVersionToken version{};
    std::array<harken::OriginalQueueFutureGroupRegistration, 4> groups;
    std::array<harken::Plan03GroupSliceIdentity, 4> identities{};
    std::array<harken::Plan03GroupSliceInput, 4> inputs{};
    harken::DecisionToken initialDecision{};
    harken::LogicCommitCoordinator::PreparedIssue sequence;
    harken::OriginalDecisionLifetimePin currentDecisionOriginal;
    harken::OccurrenceStatePool::OriginalSingleCompletionOwnerPin currentOriginal;
    harken::CompletionOwnerRef currentOwner{};
    harken::PreparedDecisionReservation decision;
    harken::OriginalDecisionLifetimePin decisionOriginal;
    harken::DecisionTable::PreparedTicketCommitProof ticket;
    harken::PreparedDecisionReservation coordinator;
    harken::OriginalDecisionLifetimePin coordinatorOriginal;
    harken::DecisionTable::PreparedTicketCommitProof coordinatorTicket;
    harken::OccurrenceToken successor{};
    harken::OccurrenceStatePool::OriginalSingleCompletionOwnerPin successorOriginal;
    harken::OccurrenceStatePool::PreparedSingleCompletionCommitProof successorProof;
    harken::CompletionOwnerRef successorOwner{};
    harken::CompletionProgressRow* successorProgress = nullptr;
    harken::TimelineArmToken newArm{};
    bool initializing = true;
    SuccessorForm successorForm = SuccessorForm::PreparedControl;
    harken::PreparedDecisionReservation sourceDecision;
    harken::OriginalDecisionLifetimePin sourceDecisionOriginal;
    harken::DecisionTable::PreparedTicketCommitProof sourceTicket;

    void Prepare(bool withMember = true, bool initialize = true,
        SuccessorForm form = SuccessorForm::PreparedControl) {
        initializing = initialize;
        successorForm = form;
        create.resetInitial = create.initial;
        if (initialize) {
            ASSERT_TRUE(queues.PrepareAcquire(create, &instance, &parent));
            queue = instance.Token();
        } else ASSERT_TRUE(queues.Acquire(create, &queue, &parent));
        harken::DecisionReserveRequest originalDecision{};
        originalDecision.ticket.sequence = old.coordinator.Issue();
        originalDecision.world = 7;
        originalDecision.transport = 1;
        originalDecision.expectedContinuationGeneration = 1;
        originalDecision.sliceCount = 1;
        harken::DecisionToken currentDecision;
        ASSERT_TRUE(old.decisions.Reserve(originalDecision, &currentDecision, &currentDecisionOriginal));
        old.decisionTokens.push_back(currentDecision);
        harken::OccurrenceToken current, oldSuccessor;
        ASSERT_TRUE(old.occurrences.Acquire({11, harkentest::ObjectId(47), 0, 0}, &current));
        ASSERT_TRUE(old.occurrences.Acquire({11, harkentest::ObjectId(53), 0, 1}, &oldSuccessor));
        old.occurrenceTokens.push_back(current);
        old.occurrenceTokens.push_back(oldSuccessor);
        request = LogicalBoundaryRequest();
        request.rootDecision = currentDecision;
        request.expectedCurrentOccurrence = current;
        request.successorOccurrence = oldSuccessor;
        const auto currentEpoch = old.occurrences.ScheduleEpochOf(current);
        currentOwner = harken::CompletionOwnerRef::Single(currentDecision, currentEpoch);
        ASSERT_TRUE(old.occurrences.BindCompletionProgress(current, 13, currentEpoch));
        ASSERT_TRUE(old.occurrences.TryPinCompletionOwner(current, currentOwner, &currentOriginal));
        request.completionIsLogicalBoundary = false;
        request.logicalCompletionFrame = 0;
        harken::KnownCompletionFactsV1 facts{};
        facts.occurrence = current;
        facts.renderGeneration = 13;
        facts.residentExactMedia = true;
        facts.pcmFrameCount = 64;
        facts.loopEndFrame = 64;
        facts.loopCount = 1;
        facts.sourceRate = 48000;
        facts.mixRate = 48000;
        facts.pitchFixed = true;
        facts.serialRender = true;
        facts.absoluteLogicalStartFrame = 100;
        facts.playbackFactsRevision = currentEpoch;
        facts.program.kind = harken::PlayableKindV1::Sound;
        facts.program.stableId = harkentest::ObjectId(47);
        facts.program.world = 7;
        facts.program.runtimeSemanticFingerprint = 53;
        harken::KnownCompletionPredicateV1 candidate;
        ASSERT_EQ(harken::CompletionPredictability::KnownFinite,
            harken::MaterializeKnownCompletion(facts, &candidate));
        ASSERT_TRUE(harken::BindKnownCompletionPredicate(candidate, facts.program,
            currentOwner, &request.completion));
        request.completionOwner = currentOwner;
        ASSERT_TRUE(queues.PrepareConditional(queue, request, &conditional, parent, &original));
        harken::DecisionReserveRequest initial{};
        initial.ticket.sequence = old.coordinator.Issue();
        initial.world = 7;
        initial.transport = 1;
        initial.expectedContinuationGeneration = 1;
        initial.sliceCount = 1;
        ASSERT_TRUE(old.decisions.Reserve(initial, &initialDecision));
        old.decisionTokens.push_back(initialDecision);
        harken::ScheduledTransactionPool::Config config{};
        config.quantumFrames = 100;
        config.scheduleHorizonFrames = 400;
        config.publishLeadQuanta = 1;
        config.maxScheduledTransactions = 4;
        config.maxTransactionSpanFrames = 400;
        config.maxIngressTransactionsPerQuantum = 4;
        config.maxFrameBatchesPerQuantum = 4;
        config.maxEventsPerQuantum = 8;
        config.maxTransactionEvents = 8;
        config.maxCurrentSlicesPerQuantum = 4;
        config.maxPlan03GroupHeadersPerPage = 1;
        config.maxArmsPerGroup = 1;
        config.maxImmediateLaneGroupRows = 4;
        config.maxImmediateLaneOwnerGates = 1;
        config.maxImmediateLaneDecisionGates = old.decisions.Capacity();
        ASSERT_TRUE(pool.Reset(config, 0, 1));
        inputs[0].decision = initialDecision;
        inputs[0].executeFrame = 117;
        inputs[1].decision = request.rootDecision;
        inputs[1].executeFrame = 164;
        ASSERT_NO_FATAL_FAILURE(ReserveGroup(0));
        ASSERT_NO_FATAL_FAILURE(ReserveGroup(1));
        ASSERT_TRUE(queues.PrepareOriginalConditionalLaneClock(original, parent,
            pool, groups[0], groups[1], &clock));
        harken::QueueResetTargetV1 target{};
        target.world = create.world;
        target.containerStableId = create.containerStableId;
        target.programSemanticFingerprint = create.programSemanticFingerprint;
        ASSERT_EQ(harken::QueueResetStatusV1::Ok, queues.PrepareCatalogResetScope(target, &scope));
        ASSERT_EQ(1u, scope.MatchedCount());
        ASSERT_EQ(harken::LogicCommitCoordinator::IssueStatus::Issued,
            old.coordinator.PrepareIssue(&sequence));
        harken::DecisionReserveRequest upcoming{};
        upcoming.world = 7;
        upcoming.transport = 1;
        upcoming.expectedContinuationGeneration = 1;
        upcoming.sliceCount = 1;
        ASSERT_TRUE(old.decisions.PrepareReservation(upcoming, &decision, &decisionOriginal));
        ASSERT_TRUE(old.decisions.ProvePreparedReservationCommit(decision,
            upcoming, sequence.Sequence(), &ticket));
        ASSERT_TRUE(old.decisions.PrepareReservation(upcoming, &coordinator, &coordinatorOriginal));
        ASSERT_TRUE(old.decisions.ProvePreparedReservationCommit(coordinator,
            upcoming, sequence.Sequence(), &coordinatorTicket));
        if (form == SuccessorForm::PreparedSource) {
            ASSERT_TRUE(old.decisions.PrepareReservation(upcoming, &sourceDecision, &sourceDecisionOriginal));
            ASSERT_TRUE(old.decisions.ProvePreparedReservationCommit(sourceDecision,
                upcoming, sequence.Sequence(), &sourceTicket));
        }
        if (form != SuccessorForm::RetainedCurrent) {
            ASSERT_TRUE(old.occurrences.PrepareAcquire({11, harkentest::ObjectId(67), 0, 9}, &successor));
            const auto epoch = old.occurrences.ScheduleEpochOf(successor);
            successorOwner = harken::CompletionOwnerRef::Single(
                form == SuccessorForm::PreparedSource ? sourceDecision.Token() : decision.Token(), epoch);
            ASSERT_TRUE(old.occurrences.BindCompletionProgress(successor, 17, epoch));
            ASSERT_TRUE(old.occurrences.TryPinCompletionOwner(successor, successorOwner, &successorOriginal));
            successorProgress = old.occurrences.CompletionProgressOf(successor);
            ASSERT_NE(nullptr, successorProgress);
            ASSERT_TRUE(old.occurrences.ProvePreparedSingleCompletionCommit(successorOriginal,
                successorOwner, successorProgress, 17, epoch, &successorProof));
        }
        for (std::uint32_t i = 2; i < 4; ++i) {
            inputs[i].decision = i == 2 ? coordinator.Token() : decision.Token();
            inputs[i].sliceIndex = 0;
            inputs[i].executeFrame = 164;
        }
        if (withMember) {
            ASSERT_TRUE(arms.PreparePendingImmediateGrouped(1, 17, 7, {},
                harken::ParentAudioGroupRef{decision.Token()}, 0, &newArm));
            inputs[3].members = &newArm;
            inputs[3].memberCount = 1;
            inputs[3].sourceControlCount = form == SuccessorForm::PreparedSource ? 0 : 1;
        }
        ASSERT_NO_FATAL_FAILURE(ReserveGroup(2));
        ASSERT_NO_FATAL_FAILURE(ReserveGroup(3));
    }
    void ReserveGroup(std::uint32_t index) {
        const auto root = index < 2 ? 17u : 29u;
        harken::PreparedPrivateImmediateLaneGroup prepared;
        ASSERT_TRUE(pool.ReserveImmediateLaneGroup(root, 100, inputs[index], arms,
            &identities[index], &prepared));
        harken::PrivateImmediateLaneGroupTransferProof transfer;
        ASSERT_TRUE(pool.ProvePrivateImmediateLaneGroupTransfer(prepared, root, 100,
            inputs[index], arms, &transfer));
        ASSERT_TRUE(pool.ConsumePrivateImmediateLaneGroupTransfer(std::move(transfer), &groups[index]));
    }
    bool Issue(SelectedFuturePool& spans, SelectedFuturePool::Owner* out) {
        if (successorForm == SuccessorForm::RetainedCurrent)
            return spans.PrepareRetainedCurrent(queues, scope, parent, original, clock, sequence,
                currentDecisionOriginal, currentOriginal, old.decisions, decision, decisionOriginal,
                ticket, coordinator, coordinatorOriginal, coordinatorTicket, old.occurrences,
                pool, groups[2], groups[3], out);
        const SelectedFuturePool::SourceCompletionFactory source{
            sourceDecision, sourceDecisionOriginal, sourceTicket};
        return spans.Prepare(queues, scope, parent, original, clock, sequence, currentDecisionOriginal, currentOriginal,
            old.decisions, decision, decisionOriginal, ticket, coordinator, coordinatorOriginal, coordinatorTicket, old.occurrences,
            successorOriginal, successorProof, pool, groups[2], groups[3], out, nullptr,
            successorForm == SuccessorForm::PreparedSource ? &source : nullptr);
    }
    void EndOld() {
        if (conditional.Valid()) ASSERT_TRUE(queues.CancelPreparedConditional(&conditional));
        else if (queues.IsCurrentOriginalConditionalFutureReservation(original)) {
            if (initializing)
                ASSERT_TRUE(queues.RollbackInitializingConditional(instance, queue, version, request.rootDecision));
            else ASSERT_TRUE(queues.RetireConditionalForAbort(queue, version, request.rootDecision));
        }
        clock.Release();
        original.Release();
    }
    void Cleanup() {
        ASSERT_NO_FATAL_FAILURE(EndOld());
        successorProof.Release();
        sourceTicket.Release();
        ticket.Release();
        coordinatorTicket.Release();
        sequence.Release();
        for (std::uint32_t i = 0; i < groups.size(); ++i)
            if (groups[i].Valid()) ASSERT_TRUE(pool.CancelPrivateImmediateLaneGroup(
                i < 2 ? 17u : 29u, identities[i], &groups[i]));
        if (newArm.Valid()) ASSERT_TRUE(arms.CancelPrepared(newArm));
        ASSERT_TRUE(pool.ResetForEpoch(0, 2));
        if (successorForm != SuccessorForm::RetainedCurrent) {
            ASSERT_TRUE(old.occurrences.RollbackCompletionOwnerPinExact(successor, successorOwner));
            ASSERT_TRUE(old.occurrences.UnbindCompletionProgress(successor, 17));
            ASSERT_TRUE(old.occurrences.CancelPreparedAcquire(successor, &successorOriginal));
        }
        if (sourceDecision.Valid())
            ASSERT_TRUE(old.decisions.CancelPreparedReservation(&sourceDecision, &sourceDecisionOriginal));
        ASSERT_TRUE(old.decisions.CancelPreparedReservation(&decision, &decisionOriginal));
        ASSERT_TRUE(old.decisions.CancelPreparedReservation(&coordinator, &coordinatorOriginal));
        scope.Release();
        const auto* currentProgress = old.occurrences.CompletionProgressOf(request.expectedCurrentOccurrence);
        ASSERT_NE(nullptr, currentProgress);
        harken::CompletionProgressSnapshot unpublishedProgress{};
        ASSERT_TRUE(harken::CaptureCompletionProgressExact(*currentProgress,
            request.expectedCurrentOccurrence, 13, &unpublishedProgress));
        ASSERT_EQ(0u, unpublishedProgress.logicalOutputFramesProduced);
        ASSERT_EQ(0u, unpublishedProgress.completedThroughBlockSequence);
        // This logic-only fixture never publishes a source or enters an audio callback.
        ASSERT_TRUE(old.occurrences.RollbackCompletionOwnerPinExact(request.expectedCurrentOccurrence, currentOwner));
        ASSERT_TRUE(old.occurrences.UnbindCompletionProgress(request.expectedCurrentOccurrence, 13));
        currentOriginal.Release();
        currentDecisionOriginal.Release();
        if (initializing) ASSERT_TRUE(queues.CancelPreparedAcquire(&instance, &parent));
        else ASSERT_TRUE(queues.Release(queue, &parent));
        old.ReleaseAll();
        EXPECT_EQ(0u, arms.LiveCount());
    }
};

TEST(QueueRuntime, PreparedSelectedFutureBackingRetainsExactNativeReaderUntilReturn) {
    GenuineHiddenSelectedFuture state;
    ASSERT_NO_FATAL_FAILURE(state.Prepare());
    SelectedFuturePool spans(1);
    spans.SetRowCommitPageLimitForTests(0);
    SelectedFuturePool::Owner owner;
    EXPECT_FALSE(state.Issue(spans, &owner));
    EXPECT_FALSE(owner.Valid());
    EXPECT_EQ(0u, spans.ConstructedRowCount());
    spans.SetRowCommitPageLimitForTests(SIZE_MAX);
    SelectedFuturePool::BackingPreparation backing;
    ASSERT_TRUE(spans.PrepareBacking(1, &backing));
    spans.SetRowCommitPageLimitForTests(0);
    const auto bytes = spans.RowStorageBytes();
    {
        auto use = backing.Use();
        ASSERT_TRUE(state.Issue(spans, &owner));
    }
    ASSERT_TRUE(spans.FreezeUnbound(owner));
    SelectedFuturePool::NativeConditionalRead reader;
    ASSERT_TRUE(owner.BorrowNativeConditional(&reader));
    SelectedFuturePool::ConditionalBacking original, captured;
    ASSERT_TRUE(reader.Capture(&original));
    owner.Release();
    backing = {};
    ASSERT_TRUE(reader.Valid());
    ASSERT_TRUE(reader.Capture(&captured));
    EXPECT_EQ(original.version.version, captured.version.version);
    EXPECT_EQ(original.decision.generation, captured.decision.generation);
    EXPECT_FALSE(state.Issue(spans, &owner));
    EXPECT_EQ(bytes, spans.RowStorageBytes());
    ASSERT_TRUE(reader.Release());
    ASSERT_TRUE(state.Issue(spans, &owner));
    owner.Release();
    ASSERT_NO_FATAL_FAILURE(state.Cleanup());
}

TEST(QueueRuntime, HiddenSelectedFutureActualPreparedUnboundFactoryFreezesBeforeAnyAck) {
    static_assert(!std::is_copy_constructible_v<SelectedFuturePool::Owner>);
    static_assert(!std::is_constructible_v<SelectedFuturePool::Owner, harken::QueueInstanceToken>);
    GenuineHiddenSelectedFuture state;
    ASSERT_NO_FATAL_FAILURE(state.Prepare());
    SelectedFuturePool spans(1);
    SelectedFuturePool::Owner owner;
    ASSERT_TRUE(state.Issue(spans, &owner));
    SelectedFuturePool::Observation snapshot;
    ASSERT_TRUE(owner.Capture(&snapshot));
    EXPECT_EQ(state.request.rootDecision.slot, snapshot.currentDecision.slot);
    EXPECT_EQ(state.decision.Token().slot, snapshot.newDecision.slot);
    EXPECT_NE(snapshot.currentDecision.slot, snapshot.newDecision.slot);
    EXPECT_EQ(state.request.expectedCurrentOccurrence.slot, snapshot.current.slot);
    EXPECT_EQ(state.successor.slot, snapshot.newSuccessor.slot);
    EXPECT_EQ(2u, snapshot.resetFirst.version);
    EXPECT_EQ(state.create.resetInitial.currentChildOrdinal, snapshot.resetFirst.currentChildOrdinal);
    EXPECT_EQ((std::array<std::uint64_t, 2>{64, 64}), snapshot.relativeFrames);
    EXPECT_EQ(1u, snapshot.newMemberCount);
    EXPECT_EQ(state.newArm.renderGeneration, snapshot.newMember.renderGeneration);
    harken::TimelineArmObservation arm;
    ASSERT_TRUE(state.arms.Observe(state.newArm, &arm));
    EXPECT_EQ(harken::ArmState::Prepared, arm.state);
    EXPECT_EQ(0u, arm.armedCompletionSequence);
    EXPECT_EQ(0u, arm.startedBlockSequence);
    ASSERT_TRUE(spans.FreezeUnbound(owner));
    EXPECT_FALSE(spans.FreezeUnbound(owner));
    EXPECT_EQ(SelectedFuturePool::Phase::FrozenUnbound, owner.State());
    SelectedFuturePool::NativeConditionalRead native;
    ASSERT_TRUE(owner.BorrowNativeConditional(&native));
    SelectedFuturePool::ConditionalBacking body;
    ASSERT_TRUE(native.Capture(&body));
    EXPECT_EQ(state.request.rootDecision.slot, body.currentDecision.slot);
    EXPECT_EQ(state.decision.Token().slot, body.decision.slot);
    EXPECT_EQ(state.coordinator.Token().slot, body.coordinator.slot);
    EXPECT_NE(body.coordinator.slot, body.decision.slot);
    EXPECT_TRUE(harken::SameCompletionOwner(body.completionOwner, state.currentOwner));
    EXPECT_EQ(state.request.rootDecision.slot, body.completionOwner.singleDecision.slot);
    EXPECT_EQ(snapshot.resetFirst.version, body.version.version);
    EXPECT_EQ(state.create.resetInitial.currentChildOrdinal, body.next.currentChildOrdinal);
    EXPECT_EQ(snapshot.newGroups, body.groups);
    harken::FrontierGroupBinding notBound;
    harken::AudioSliceLease noSlice;
    SelectedFuturePool::NativeConditionalProof proof;
    harken::NativeTimelineQuantumAdmissionProof notAdmitted;
    EXPECT_EQ(harken::QueueConditionalProofStatus::Waiting,
        spans.ProveNativeConditional(native, notBound, noSlice, state.old.occurrences, 100, 200, 0, notAdmitted, &proof));
    EXPECT_FALSE(proof.Valid());
    EXPECT_TRUE(native.Release());
    EXPECT_TRUE(spans.IsFrozenFor(owner, state.queues, state.original, state.pool,
        state.groups[2], state.groups[3]));
    EXPECT_FALSE(spans.IsFrozenFor(owner, state.queues, state.original, state.pool,
        state.groups[0], state.groups[1]));
    EXPECT_TRUE(harken::SameCompletionOwner(state.currentOwner, snapshot.currentOwner));
    EXPECT_EQ(state.request.completion.totalLogicalOutputFrames,
        snapshot.currentCompletion.totalLogicalOutputFrames);
    ASSERT_TRUE(state.queues.CommitPreparedConditional(&state.conditional, &state.version));
    EXPECT_TRUE(state.queues.IsCurrentOriginalConditionalFutureReservation(state.original));
    EXPECT_TRUE(state.groups[0].MatchesOriginalScope(17, 100, state.inputs[0], state.arms));
    EXPECT_TRUE(state.groups[1].MatchesOriginalScope(17, 100, state.inputs[1], state.arms));
    // Previous real issues existed, but this candidate has not advanced Current.
    EXPECT_LT(state.old.coordinator.Current(), snapshot.reservedSequence);
    owner.Release();
    ASSERT_NO_FATAL_FAILURE(state.Cleanup());
}

TEST(QueueRuntime, HiddenSelectedFutureRetainsCurrentWithoutPreparingAnotherOccurrence) {
    GenuineHiddenSelectedFuture state;
    ASSERT_NO_FATAL_FAILURE(state.Prepare(false, true,
        GenuineHiddenSelectedFuture::SuccessorForm::RetainedCurrent));
    EXPECT_FALSE(state.successorOriginal.Valid());
    EXPECT_FALSE(state.successorProof.Valid());
    SelectedFuturePool spans(1);
    SelectedFuturePool::Owner owner;
    SelectedFuturePool::Retention history;
    ASSERT_TRUE(state.Issue(spans, &owner));
    ASSERT_TRUE(spans.FreezeUnbound(owner));
    ASSERT_TRUE(owner.Borrow(&history));
    SelectedFuturePool::Observation actual{};
    ASSERT_TRUE(owner.Capture(&actual));
    EXPECT_EQ(state.request.expectedCurrentOccurrence.slot, actual.current.slot);
    EXPECT_EQ(actual.current.slot, actual.newSuccessor.slot);
    EXPECT_EQ(actual.current.generation, actual.newSuccessor.generation);
    EXPECT_TRUE(harken::SameCompletionOwner(state.currentOwner, actual.newSuccessorOwner));
    EXPECT_EQ(0u, actual.newMemberCount);
    EXPECT_NE(actual.currentDecision.slot, actual.newDecision.slot);
    harken::CompletionOwnerRef ownerBefore{}, ownerAfter{};
    ASSERT_TRUE(state.old.occurrences.CaptureCompletionOwner(actual.current, &ownerBefore));
    owner.Release();
    EXPECT_TRUE(history.Valid());
    EXPECT_FALSE(state.pool.CancelPrivateImmediateLaneGroup(29, state.identities[3], &state.groups[3]));
    ASSERT_TRUE(state.old.occurrences.CaptureCompletionOwner(actual.current, &ownerAfter));
    EXPECT_TRUE(harken::SameCompletionOwner(ownerBefore, ownerAfter));
    history.Release();
    ASSERT_NO_FATAL_FAILURE(state.Cleanup());
}

TEST(QueueRuntime, HiddenSelectedFutureRetainedCurrentRefusesARealNewMember) {
    GenuineHiddenSelectedFuture state;
    ASSERT_NO_FATAL_FAILURE(state.Prepare(true, true,
        GenuineHiddenSelectedFuture::SuccessorForm::RetainedCurrent));
    SelectedFuturePool spans(1);
    SelectedFuturePool::Owner owner;
    EXPECT_FALSE(state.Issue(spans, &owner));
    EXPECT_FALSE(owner.Valid());
    ASSERT_NO_FATAL_FAILURE(state.Cleanup());
}

TEST(QueueRuntime, HiddenSelectedFutureSourceRetainsItsDistinctCompletionDecision) {
    GenuineHiddenSelectedFuture state;
    ASSERT_NO_FATAL_FAILURE(state.Prepare(true, true,
        GenuineHiddenSelectedFuture::SuccessorForm::PreparedSource));
    SelectedFuturePool spans(1);
    SelectedFuturePool::Owner owner;
    ASSERT_TRUE(state.Issue(spans, &owner));
    ASSERT_TRUE(spans.FreezeUnbound(owner));
    SelectedFuturePool::Observation actual{};
    ASSERT_TRUE(owner.Capture(&actual));
    EXPECT_EQ(state.decision.Token().slot, actual.newDecision.slot);
    EXPECT_EQ(state.sourceDecision.Token().slot, actual.newSuccessorOwner.singleDecision.slot);
    EXPECT_EQ(state.sourceDecision.Token().generation, actual.newSuccessorOwner.singleDecision.generation);
    EXPECT_NE(actual.newDecision.slot, actual.newSuccessorOwner.singleDecision.slot);
    EXPECT_EQ(state.successor.slot, actual.newSuccessor.slot);
    EXPECT_EQ(1u, actual.newMemberCount);
    EXPECT_EQ(state.newArm.renderGeneration, actual.newMember.renderGeneration);
    state.sourceTicket.Release();
    EXPECT_FALSE(state.old.decisions.CancelPreparedReservation(
        &state.sourceDecision, &state.sourceDecisionOriginal));
    owner.Release();
    ASSERT_NO_FATAL_FAILURE(state.Cleanup());
}

TEST(QueueRuntime, HiddenSelectedFutureSourceRefusesMissingOrUnrelatedCompletionTicket) {
    GenuineHiddenSelectedFuture state;
    ASSERT_NO_FATAL_FAILURE(state.Prepare(true, true,
        GenuineHiddenSelectedFuture::SuccessorForm::PreparedSource));
    SelectedFuturePool spans(1);
    SelectedFuturePool::Owner owner;
    const SelectedFuturePool::SourceCompletionFactory unrelated{
        state.sourceDecision, state.sourceDecisionOriginal, state.ticket};
    EXPECT_FALSE(spans.Prepare(state.queues, state.scope, state.parent, state.original, state.clock,
        state.sequence, state.currentDecisionOriginal, state.currentOriginal, state.old.decisions,
        state.decision, state.decisionOriginal, state.ticket, state.coordinator,
        state.coordinatorOriginal, state.coordinatorTicket, state.old.occurrences,
        state.successorOriginal, state.successorProof, state.pool, state.groups[2], state.groups[3],
        &owner, nullptr, &unrelated));
    state.sourceTicket.Release();
    EXPECT_FALSE(state.Issue(spans, &owner));
    EXPECT_FALSE(owner.Valid());
    ASSERT_TRUE(state.old.decisions.ProvePreparedReservationCommit(state.sourceDecision,
        state.sourceDecision.OriginalRequest(), state.sequence.Sequence(), &state.sourceTicket));
    ASSERT_TRUE(state.Issue(spans, &owner));
    owner.Release();
    ASSERT_NO_FATAL_FAILURE(state.Cleanup());
}

TEST(QueueRuntime, HiddenSelectedFutureOldSettleContinuesWhileFrozenHistoryAndForeignLoanSurvive) {
    GenuineHiddenSelectedFuture state;
    ASSERT_NO_FATAL_FAILURE(state.Prepare(false, false));
    SelectedFuturePool spans(1);
    SelectedFuturePool::Owner owner;
    SelectedFuturePool::Retention loan;
    ASSERT_TRUE(state.Issue(spans, &owner));
    ASSERT_TRUE(spans.FreezeUnbound(owner));
    ASSERT_TRUE(owner.Borrow(&loan));
    SelectedFuturePool::Observation before, after;
    ASSERT_TRUE(owner.Capture(&before));
    ASSERT_TRUE(state.queues.CommitPreparedConditional(&state.conditional, &state.version));
    ASSERT_TRUE(state.queues.ActivateConditional(state.queue, state.version, state.request.rootDecision,
        state.request.expectedCurrentOccurrence));
    ASSERT_TRUE(state.queues.SettleActivated(state.queue, state.version));
    ASSERT_TRUE(loan.Capture(&after));
    EXPECT_EQ(before.oldVersion.version, after.oldVersion.version);
    EXPECT_EQ(before.currentDecision.slot, after.currentDecision.slot);
    EXPECT_EQ(before.current.slot, after.current.slot);
    EXPECT_EQ(before.newGroups, after.newGroups);
    ASSERT_NO_FATAL_FAILURE(state.EndOld());
    owner.Release();
    EXPECT_TRUE(loan.Valid());
    EXPECT_FALSE(state.pool.CancelPrivateImmediateLaneGroup(29, state.identities[3], &state.groups[3]));
    state.scope.Release();
    EXPECT_FALSE(state.queues.Release(state.queue, &state.parent));
    loan.Release();
    ASSERT_NO_FATAL_FAILURE(state.Cleanup());
}

TEST(QueueRuntime, HiddenSelectedFutureRefusesForeignFactoriesAndOccupiedOutputBeforeLoans) {
    GenuineHiddenSelectedFuture first, second;
    ASSERT_NO_FATAL_FAILURE(first.Prepare());
    ASSERT_NO_FATAL_FAILURE(second.Prepare());
    SelectedFuturePool spans(1);
    SelectedFuturePool::Owner owner;
    EXPECT_FALSE(spans.Prepare(first.queues, second.scope, first.parent, first.original, first.clock,
        first.sequence, first.currentDecisionOriginal, first.currentOriginal, first.old.decisions, first.decision, first.decisionOriginal, first.ticket, first.coordinator, first.coordinatorOriginal, first.coordinatorTicket,
        first.old.occurrences, first.successorOriginal, first.successorProof,
        first.pool, first.groups[2], first.groups[3], &owner));
    EXPECT_FALSE(spans.Prepare(first.queues, first.scope, first.parent, first.original, first.clock,
        first.sequence, first.currentDecisionOriginal, first.currentOriginal, first.old.decisions, first.decision, first.decisionOriginal, first.ticket, first.coordinator, first.coordinatorOriginal, first.coordinatorTicket,
        first.old.occurrences, second.successorOriginal, second.successorProof,
        first.pool, first.groups[2], first.groups[3], &owner));
    EXPECT_FALSE(spans.Prepare(first.queues, first.scope, first.parent, first.original, first.clock,
        first.sequence, first.currentDecisionOriginal, first.currentOriginal, first.old.decisions, first.decision, first.decisionOriginal, first.ticket, first.coordinator, first.coordinatorOriginal, first.coordinatorTicket,
        first.old.occurrences, first.successorOriginal, first.successorProof,
        first.pool, second.groups[2], second.groups[3], &owner));
    EXPECT_FALSE(first.Issue(spans, nullptr));
    ASSERT_TRUE(first.Issue(spans, &owner));
    EXPECT_FALSE(second.Issue(spans, &owner));
    owner.Release();
    ASSERT_NO_FATAL_FAILURE(first.Cleanup());
    ASSERT_NO_FATAL_FAILURE(second.Cleanup());
}

TEST(QueueRuntime, HiddenSelectedFutureExactEpisodeOneIssuerAbortAllowsRetryWithHistoricalLoan) {
    GenuineHiddenSelectedFuture state;
    ASSERT_NO_FATAL_FAILURE(state.Prepare(false));
    SelectedFuturePool first(1), second(1);
    SelectedFuturePool::Owner owner, retry;
    SelectedFuturePool::Retention history;
    ASSERT_TRUE(state.Issue(first, &owner));
    ASSERT_TRUE(owner.Borrow(&history));
    EXPECT_FALSE(state.Issue(second, &retry));
    owner.Release();
    ASSERT_TRUE(state.Issue(second, &retry));
    ASSERT_TRUE(second.FreezeUnbound(retry));
    EXPECT_FALSE(first.FreezeUnbound(owner));
    retry.Release();
    EXPECT_TRUE(history.Valid());
    history.Release();
    ASSERT_NO_FATAL_FAILURE(state.Cleanup());
}

TEST(QueueRuntime, HiddenSelectedFutureMoveOverwriteAndNonemptyDestructorReturnOriginalReceivers) {
    GenuineHiddenSelectedFuture first, second;
    ASSERT_NO_FATAL_FAILURE(first.Prepare());
    ASSERT_NO_FATAL_FAILURE(second.Prepare());
    SelectedFuturePool spans(2);
    SelectedFuturePool::Owner one, two;
    ASSERT_TRUE(first.Issue(spans, &one));
    ASSERT_TRUE(second.Issue(spans, &two));
    ASSERT_TRUE(spans.FreezeUnbound(two));
    one = std::move(two);
    EXPECT_FALSE(two.Valid());
    ASSERT_NO_FATAL_FAILURE(first.Cleanup());
    {
        SelectedFuturePool::Owner last(std::move(one));
        EXPECT_TRUE(last.Valid());
        EXPECT_FALSE(one.Valid());
    }
    ASSERT_NO_FATAL_FAILURE(second.Cleanup());
}

TEST(QueueRuntime, HiddenSelectedFutureNativeCountMarkerAndGenerationLimitsNeverMintFromValues) {
    using Access = harken::detail::PrivateSelectedQueueFutureSpanTestAccess;
    GenuineHiddenSelectedFuture state;
    ASSERT_NO_FATAL_FAILURE(state.Prepare());
    SelectedFuturePool spans(1);
    SelectedFuturePool::Owner owner;
    Access::MarkerLimit(spans, true);
    EXPECT_FALSE(state.Issue(spans, &owner));
    Access::MarkerLimit(spans, false);
    Access::GenerationLimit(spans);
    ASSERT_TRUE(state.Issue(spans, &owner));
    Access::LoanLimit(spans, 0, UINT32_MAX);
    SelectedFuturePool::Retention refused;
    EXPECT_FALSE(owner.Borrow(&refused));
    EXPECT_FALSE(refused.Valid());
    Access::LoanLimit(spans, 0, 1);
    owner.Release();
    EXPECT_FALSE(state.Issue(spans, &owner));
    ASSERT_NO_FATAL_FAILURE(state.Cleanup());
}
} // namespace

#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
namespace {
// Word observations are taken after each real factory action on the same Owner.
std::uint32_t NativeStageCount(std::uint64_t word) {
    return static_cast<std::uint32_t>((word & CatalogStage::kCountMask) >> CatalogStage::kCountShift);
}
std::uint64_t NativeStageEpoch(std::uint64_t word) { return word >> CatalogStage::kEpochShift; }
using NativeStageAccess = harken::PrivateCatalogStagePoolTestAccess;

TEST(QueueRuntime, PrivateCatalogNativeOldSiblingActualWriteInvalidatesActiveSnapshot) {
    static_assert(!std::is_copy_constructible_v<CatalogStage::OriginBaseline>);
    static_assert(!std::is_copy_constructible_v<CatalogStage::OriginalOldSibling>);
    static_assert(!std::is_constructible_v<CatalogStage::OriginalOldSibling, bool>);
    GenuineCatalogStageHarness actual;
    ASSERT_NO_FATAL_FAILURE(actual.Prepare(true));
    CatalogStage::Owner owner;
    ASSERT_EQ(CatalogStage::Result::Ok, actual.Issue(&owner));
    CatalogStage::OriginBaseline baseline;
    ASSERT_TRUE(actual.stage.BorrowOriginBaseline(owner, &baseline));
    CatalogStage::Snapshot snapshot;
    ASSERT_EQ(CatalogStage::Result::Ok, actual.stage.BeginSnapshot(owner, &snapshot));
    const auto before = NativeStageAccess::CaptureWord(actual.stage, owner);
    CatalogStage::OriginalOldSibling actor;
    ASSERT_EQ(CatalogStage::Result::Ok, actual.stage.BorrowOriginalOldSibling(baseline, &actor));
    const auto borrowed = NativeStageAccess::CaptureWord(actual.stage, owner);
    EXPECT_EQ(2u, NativeStageCount(borrowed));
    EXPECT_EQ(NativeStageEpoch(before), NativeStageEpoch(borrowed));
    ASSERT_TRUE(actual.stage.BeforeOriginalOldSiblingWrite(actor));
    const auto written = NativeStageAccess::CaptureWord(actual.stage, owner);
    EXPECT_GT(NativeStageEpoch(written), NativeStageEpoch(before));
    // The genuine original Queue publisher remains legal during preparation.
    // The kernel activity itself does not issue this separate Queue permission.
    harken::QueueInstanceToken committed{};
    ASSERT_TRUE(actual.queues.CommitPreparedAcquire(&actual.initialAcquire, &committed));
    EXPECT_EQ(actual.queue.slot, committed.slot);
    EXPECT_EQ(actual.queue.generation, committed.generation);
    harken::QueueInstanceSnapshot observedOld{};
    ASSERT_TRUE(actual.queues.Capture(committed, &observedOld));
    EXPECT_EQ(17u, observedOld.rootInstance);
    ASSERT_TRUE(actual.stage.BeforeOriginalOldSiblingWrite(actor));
    EXPECT_EQ(written, NativeStageAccess::CaptureWord(actual.stage, owner));
    CatalogStage::CompareProof refused;
    EXPECT_FALSE(actual.stage.FinishSnapshot(&snapshot, &refused));
    EXPECT_TRUE(snapshot.Valid());
    EXPECT_FALSE(refused.Valid());
    actor.Release(); snapshot.Release(); baseline.Release(); owner.Release();
    ASSERT_NO_FATAL_FAILURE(actual.Cleanup());
}

TEST(QueueRuntime, PrivateCatalogNativeOldActual255LoansAbortCandidateAndRetainOriginalScope) {
    GenuineCatalogStageHarness actual;
    ASSERT_NO_FATAL_FAILURE(actual.Prepare(true));
    CatalogStage::Owner owner;
    ASSERT_EQ(CatalogStage::Result::Ok, actual.Issue(&owner));
    CatalogStage::OriginBaseline baseline;
    ASSERT_TRUE(actual.stage.BorrowOriginBaseline(owner, &baseline));
    std::array<CatalogStage::Read, 255> accepted;
    for (auto& read : accepted)
        ASSERT_EQ(CatalogStage::Result::Ok, actual.stage.BorrowRead(owner, &read));
    const auto full = NativeStageAccess::CaptureWord(actual.stage, owner);
    ASSERT_EQ(255u, NativeStageCount(full));
    CatalogStage::OriginalOldSibling native;
    ASSERT_EQ(CatalogStage::Result::Ok, actual.stage.BorrowOriginalOldSibling(baseline, &native));
    const auto overflow = NativeStageAccess::CaptureWord(actual.stage, owner);
    EXPECT_EQ(255u, NativeStageCount(overflow));
    EXPECT_EQ(NativeStageEpoch(full), NativeStageEpoch(overflow));
    CatalogStage::State state = CatalogStage::State::Free;
    ASSERT_TRUE(actual.stage.CaptureState(owner, &state));
    EXPECT_EQ(CatalogStage::State::AbortedOld, state);
    ASSERT_TRUE(actual.stage.BeforeOriginalOldSiblingWrite(native));
    harken::QueueInstanceToken committed{};
    ASSERT_TRUE(actual.queues.CommitPreparedAcquire(&actual.initialAcquire, &committed));
    for (auto& read : accepted) read.Release();
    EXPECT_EQ(0u, NativeStageCount(NativeStageAccess::CaptureWord(actual.stage, owner)));
    EXPECT_TRUE(native.Valid());
    owner.Release(); baseline.Release();
    EXPECT_EQ(0u, actual.stage.ReclaimClosed());
    EXPECT_FALSE(actual.groups.CancelPrivateImmediateLaneGroup(17, actual.identities[0], &actual.originals[0]));
    EXPECT_TRUE(actual.originals[0].Valid());
    native.Release();
    ASSERT_EQ(1u, actual.stage.ReclaimClosed());
    ASSERT_NO_FATAL_FAILURE(actual.Cleanup());
}

TEST(QueueRuntime, PrivateCatalogNativeOld512NestedSiblingsKeepOneContinuousActualParent) {
    GenuineCatalogStageHarness actual;
    ASSERT_NO_FATAL_FAILURE(actual.Prepare());
    CatalogStage::Owner owner;
    ASSERT_EQ(CatalogStage::Result::Ok, actual.Issue(&owner));
    CatalogStage::OriginBaseline baseline;
    ASSERT_TRUE(actual.stage.BorrowOriginBaseline(owner, &baseline));
    std::array<CatalogStage::OriginalOldSibling, 512> actors;
    for (auto& actor : actors)
        ASSERT_EQ(CatalogStage::Result::Ok, actual.stage.BorrowOriginalOldSibling(baseline, &actor));
    const auto borrowed = NativeStageAccess::CaptureWord(actual.stage, owner);
    EXPECT_EQ(1u, NativeStageCount(borrowed));
    const auto parent = NativeStageAccess::ParentWord(actual.stage, owner);
    EXPECT_EQ(512u, static_cast<std::uint32_t>(parent) >> 2);
    ASSERT_TRUE(actual.stage.BeforeOriginalOldSiblingWrite(actors[0]));
    const auto firstWrite = NativeStageAccess::CaptureWord(actual.stage, owner);
    EXPECT_GT(NativeStageEpoch(firstWrite), NativeStageEpoch(borrowed));
    for (auto& actor : actors) ASSERT_TRUE(actual.stage.BeforeOriginalOldSiblingWrite(actor));
    EXPECT_EQ(firstWrite, NativeStageAccess::CaptureWord(actual.stage, owner));
    for (std::size_t i = 0; i + 1 < actors.size(); ++i) actors[i].Release();
    EXPECT_EQ(1u, NativeStageCount(NativeStageAccess::CaptureWord(actual.stage, owner)));
    CatalogStage::OriginalOldSibling nextCallback;
    ASSERT_EQ(CatalogStage::Result::Ok, actual.stage.BorrowOriginalOldSibling(baseline, &nextCallback));
    EXPECT_EQ(parent >> 32, NativeStageAccess::ParentWord(actual.stage, owner) >> 32);
    actors.back().Release();
    ASSERT_TRUE(actual.stage.BeforeOriginalOldSiblingWrite(nextCallback));
    EXPECT_EQ(firstWrite, NativeStageAccess::CaptureWord(actual.stage, owner));
    CatalogStage::Snapshot excluded;
    EXPECT_EQ(CatalogStage::Result::Busy, actual.stage.BeginSnapshot(owner, &excluded));
    nextCallback.Release();
    EXPECT_EQ(0u, NativeStageCount(NativeStageAccess::CaptureWord(actual.stage, owner)));
    ASSERT_EQ(CatalogStage::Result::Ok, actual.stage.BeginSnapshot(owner, &excluded));
    excluded.Release(); baseline.Release(); owner.Release();
    ASSERT_NO_FATAL_FAILURE(actual.Cleanup());
}

TEST(QueueRuntime, PrivateCatalogNativeOldForeignOccupiedMoveAndDestructorReturnTrueLoans) {
    GenuineCatalogStageHarness actual, foreign;
    ASSERT_NO_FATAL_FAILURE(actual.Prepare());
    ASSERT_NO_FATAL_FAILURE(foreign.Prepare());
    CatalogStage::Owner owner, other;
    ASSERT_EQ(CatalogStage::Result::Ok, actual.Issue(&owner));
    ASSERT_EQ(CatalogStage::Result::Ok, foreign.Issue(&other));
    CatalogStage::OriginBaseline baseline, otherBaseline;
    ASSERT_TRUE(actual.stage.BorrowOriginBaseline(owner, &baseline));
    ASSERT_TRUE(foreign.stage.BorrowOriginBaseline(other, &otherBaseline));
    CatalogStage::OriginalOldSibling refused;
    EXPECT_EQ(CatalogStage::Result::Invalid, actual.stage.BorrowOriginalOldSibling(otherBaseline, &refused));
    EXPECT_FALSE(refused.Valid());
    CatalogStage::OriginBaseline borrowedBaseline;
    EXPECT_FALSE(otherBaseline.Borrow(&baseline));
    ASSERT_TRUE(baseline.Borrow(&borrowedBaseline));
    CatalogStage::OriginBaseline movedBaseline(std::move(borrowedBaseline));
    EXPECT_FALSE(borrowedBaseline.Valid());
    otherBaseline = std::move(movedBaseline);
    EXPECT_FALSE(movedBaseline.Valid());
    EXPECT_TRUE(otherBaseline.Valid());
    EXPECT_EQ(CatalogStage::Result::Invalid, foreign.stage.BorrowOriginalOldSibling(otherBaseline, &refused));
    EXPECT_FALSE(refused.Valid());
    {
        CatalogStage::OriginalOldSibling first, second;
        ASSERT_EQ(CatalogStage::Result::Ok, actual.stage.BorrowOriginalOldSibling(baseline, &first));
        ASSERT_EQ(CatalogStage::Result::Ok, actual.stage.BorrowOriginalOldSibling(baseline, &second));
        const auto before = NativeStageAccess::ParentWord(actual.stage, owner);
        EXPECT_EQ(CatalogStage::Result::Invalid, actual.stage.BorrowOriginalOldSibling(baseline, &first));
        EXPECT_EQ(before, NativeStageAccess::ParentWord(actual.stage, owner));
        EXPECT_FALSE(foreign.stage.BeforeOriginalOldSiblingWrite(first));
        CatalogStage::OriginalOldSibling moved(std::move(first));
        EXPECT_FALSE(first.Valid());
        second = std::move(moved);
        EXPECT_FALSE(moved.Valid());
        EXPECT_TRUE(second.Valid());
        EXPECT_EQ(1u, static_cast<std::uint32_t>(NativeStageAccess::ParentWord(actual.stage, owner)) >> 2);
        EXPECT_FALSE(actual.groups.CancelPrivateImmediateLaneGroup(17, actual.identities[1], &actual.originals[1]));
    }
    EXPECT_EQ(0u, NativeStageCount(NativeStageAccess::CaptureWord(actual.stage, owner)));
    otherBaseline.Release(); other.Release(); baseline.Release(); owner.Release();
    ASSERT_NO_FATAL_FAILURE(foreign.Cleanup());
    ASSERT_NO_FATAL_FAILURE(actual.Cleanup());
}

TEST(QueueRuntime, PrivateCatalogNativeOldEpochMaximumKeepsActualOldPublisherAndNeverWraps) {
    GenuineCatalogStageHarness actual;
    ASSERT_NO_FATAL_FAILURE(actual.Prepare(true));
    CatalogStage::Owner owner;
    ASSERT_EQ(CatalogStage::Result::Ok, actual.Issue(&owner));
    CatalogStage::OriginBaseline baseline;
    ASSERT_TRUE(actual.stage.BorrowOriginBaseline(owner, &baseline));
    const auto before = NativeStageAccess::CaptureWord(actual.stage, owner);
    NativeStageAccess::ExchangeWord(actual.stage, owner,
        (CatalogStage::kEpochMax << CatalogStage::kEpochShift) | (before & CatalogStage::kStateMask));
    CatalogStage::OriginalOldSibling actor;
    ASSERT_EQ(CatalogStage::Result::Ok, actual.stage.BorrowOriginalOldSibling(baseline, &actor));
    ASSERT_TRUE(actual.stage.BeforeOriginalOldSiblingWrite(actor));
    const auto written = NativeStageAccess::CaptureWord(actual.stage, owner);
    EXPECT_EQ(CatalogStage::kEpochMax, NativeStageEpoch(written));
    CatalogStage::State state = CatalogStage::State::Free;
    ASSERT_TRUE(actual.stage.CaptureState(owner, &state));
    EXPECT_EQ(CatalogStage::State::AbortedOld, state);
    harken::QueueInstanceToken committed{};
    ASSERT_TRUE(actual.queues.CommitPreparedAcquire(&actual.initialAcquire, &committed));
    EXPECT_EQ(actual.queue.slot, committed.slot);
    CatalogStage::Snapshot refused;
    EXPECT_EQ(CatalogStage::Result::Busy, actual.stage.BeginSnapshot(owner, &refused));
    actor.Release(); baseline.Release(); owner.Release();
    ASSERT_NO_FATAL_FAILURE(actual.Cleanup());
}

TEST(QueueRuntime, PrivateCatalogNativeOldParentSerialMaximumUsesAuthenticRetainedFallback) {
    GenuineCatalogStageHarness actual;
    ASSERT_NO_FATAL_FAILURE(actual.Prepare(true));
    CatalogStage::Owner owner;
    ASSERT_EQ(CatalogStage::Result::Ok, actual.Issue(&owner));
    CatalogStage::OriginBaseline baseline;
    ASSERT_TRUE(actual.stage.BorrowOriginBaseline(owner, &baseline));
    // A negative exhausted counter prerequisite never constructs a cap.
    NativeStageAccess::ExchangeParentWord(actual.stage, owner, static_cast<std::uint64_t>(UINT32_MAX) << 32);
    CatalogStage::OriginalOldSibling actor;
    ASSERT_EQ(CatalogStage::Result::Ok, actual.stage.BorrowOriginalOldSibling(baseline, &actor));
    EXPECT_TRUE(actor.Valid());
    ASSERT_TRUE(actual.stage.BeforeOriginalOldSiblingWrite(actor));
    EXPECT_EQ(UINT32_MAX, NativeStageAccess::ParentWord(actual.stage, owner) >> 32);
    EXPECT_EQ(1u, NativeStageCount(NativeStageAccess::CaptureWord(actual.stage, owner)));
    CatalogStage::State state = CatalogStage::State::Free;
    ASSERT_TRUE(actual.stage.CaptureState(owner, &state));
    EXPECT_EQ(CatalogStage::State::AbortedOld, state);
    harken::QueueInstanceToken committed{};
    ASSERT_TRUE(actual.queues.CommitPreparedAcquire(&actual.initialAcquire, &committed));
    actor.Release();
    EXPECT_EQ(0u, NativeStageCount(NativeStageAccess::CaptureWord(actual.stage, owner)));
    baseline.Release(); owner.Release();
    ASSERT_NO_FATAL_FAILURE(actual.Cleanup());
}

TEST(QueueRuntime, PrivateCatalogNativeOldBusyParentAndStructuralLimitNeverMintOrBlockQueuePublisher) {
    GenuineCatalogStageHarness actual;
    ASSERT_NO_FATAL_FAILURE(actual.Prepare(true));
    CatalogStage::Owner owner;
    ASSERT_EQ(CatalogStage::Result::Ok, actual.Issue(&owner));
    CatalogStage::OriginBaseline baseline;
    ASSERT_TRUE(actual.stage.BorrowOriginBaseline(owner, &baseline));
    // Negative busy preparation only, not a successful native parent issuer.
    const auto idle = NativeStageAccess::ExchangeParentWord(actual.stage, owner, 1u);
    CatalogStage::OriginalOldSibling fallback;
    ASSERT_EQ(CatalogStage::Result::Ok, actual.stage.BorrowOriginalOldSibling(baseline, &fallback));
    ASSERT_TRUE(actual.stage.BeforeOriginalOldSiblingWrite(fallback));
    EXPECT_EQ(1u, NativeStageCount(NativeStageAccess::CaptureWord(actual.stage, owner)));
    fallback.Release();
    NativeStageAccess::ExchangeParentWord(actual.stage, owner, idle);
    const auto realStructural = NativeStageAccess::ExchangeStructural(actual.stage, owner, UINT32_MAX);
    CatalogStage::OriginalOldSibling refused;
    EXPECT_EQ(CatalogStage::Result::Exhausted, actual.stage.BorrowOriginalOldSibling(baseline, &refused));
    EXPECT_FALSE(refused.Valid());
    NativeStageAccess::ExchangeStructural(actual.stage, owner, realStructural);
    CatalogStage::State state = CatalogStage::State::Free;
    ASSERT_TRUE(actual.stage.CaptureState(owner, &state));
    EXPECT_EQ(CatalogStage::State::AbortedOld, state);
    // Native Queue authority is independent; refusal issued no activity rights.
    harken::QueueInstanceToken committed{};
    ASSERT_TRUE(actual.queues.CommitPreparedAcquire(&actual.initialAcquire, &committed));
    EXPECT_EQ(actual.queue.generation, committed.generation);
    baseline.Release(); owner.Release();
    ASSERT_NO_FATAL_FAILURE(actual.Cleanup());
}

TEST(QueueRuntime, PrivateCatalogNativeOldLastReturnPreservesHistoricalFamilyThenReissuesFullEpisode) {
    GenuineCatalogStageHarness actual;
    ASSERT_NO_FATAL_FAILURE(actual.Prepare());
    CatalogStage::Owner owner;
    ASSERT_EQ(CatalogStage::Result::Ok, actual.Issue(&owner));
    const auto originalEpisode = NativeStageAccess::Episode(actual.stage, owner);
    CatalogStage::OriginBaseline baseline;
    CatalogStage::Loan historical;
    ASSERT_TRUE(actual.stage.BorrowOriginBaseline(owner, &baseline));
    ASSERT_TRUE(actual.stage.Borrow(owner, &historical));
    CatalogStage::OriginalOldSibling actor;
    ASSERT_EQ(CatalogStage::Result::Ok, actual.stage.BorrowOriginalOldSibling(baseline, &actor));
    ASSERT_TRUE(actual.stage.BeforeOriginalOldSiblingWrite(actor));
    owner.Release(); baseline.Release(); actor.Release();
    EXPECT_FALSE(actor.Valid());
    EXPECT_TRUE(historical.Valid());
    EXPECT_EQ(0u, actual.stage.ReclaimClosed());
    EXPECT_FALSE(actual.groups.CancelPrivateImmediateLaneGroup(17, actual.identities[0], &actual.originals[0]));
    historical.Release();
    ASSERT_EQ(1u, actual.stage.ReclaimClosed());
    harken::QueueResetTargetV1 target{};
    target.world = 7; target.containerStableId = harkentest::ObjectId(13);
    target.programSemanticFingerprint = 19;
    ASSERT_EQ(harken::QueueResetStatusV1::Ok, actual.queues.PrepareCatalogResetScope(target, &actual.scope));
    ASSERT_EQ(harken::LogicCommitCoordinator::IssueStatus::Issued, actual.coordinator.PrepareIssue(&actual.sequence));
    ASSERT_EQ(CatalogStage::Result::Ok, actual.Issue(&owner));
    const auto nextEpisode = NativeStageAccess::Episode(actual.stage, owner);
    EXPECT_GT(nextEpisode.first, originalEpisode.first);
    EXPECT_GT(nextEpisode.second, originalEpisode.second);
    ASSERT_TRUE(actual.stage.BorrowOriginBaseline(owner, &baseline));
    ASSERT_EQ(CatalogStage::Result::Ok, actual.stage.BorrowOriginalOldSibling(baseline, &actor));
    ASSERT_TRUE(actor.Valid());
    ASSERT_TRUE(actual.stage.BeforeOriginalOldSiblingWrite(actor));
    actor.Release(); baseline.Release(); owner.Release();
    ASSERT_NO_FATAL_FAILURE(actual.Cleanup());
}
} // namespace
#endif
