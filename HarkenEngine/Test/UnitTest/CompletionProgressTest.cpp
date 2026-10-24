#include "harken/core/CompletionProgress.h"
#include "harken/core/OccurrenceStatePool.h"
#include "harken/core/CompositeCompletionOwnerPool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <thread>

namespace harken::detail {

struct CompletionProgressTestAccess {
    static void SeedSeqlock(CompletionProgressRow& row,
                            std::uint64_t sequence) {
        row.seqlock_.store(sequence, std::memory_order_relaxed);
    }

    static std::uint64_t Seqlock(const CompletionProgressRow& row) {
        return row.seqlock_.load(std::memory_order_relaxed);
    }
};

} // namespace harken::detail

TEST(CompletionProgress, CounterExhaustionPublishesTerminalSnapshotWithoutWrap) {
    harken::CompletionProgressRow row{};
    harken::CompletionProgressSnapshot initial{};
    initial.occurrence = {3, 7};
    initial.renderGeneration = 11;
    initial.logicalOutputFramesProduced =
        (std::numeric_limits<std::uint64_t>::max)();
    initial.progressRevision = 19;
    ASSERT_TRUE(harken::InitializeCompletionProgress(row, initial));

    harken::CompletionProgressBlockUpdate update{};
    update.occurrence = initial.occurrence;
    update.renderGeneration = initial.renderGeneration;
    update.sourceOutputFramesThisBlock = 1;
    update.resamplerSourceFrame = 101;
    update.sourceFeedCursor = 202;
    update.completedThroughBlockSequence = 23;

    EXPECT_EQ(harken::CompletionProgressPublishStatus::CounterExhausted,
              harken::PublishCompletionProgressBlock(row, update));

    harken::CompletionProgressSnapshot captured{};
    ASSERT_TRUE(harken::CaptureCompletionProgress(row, &captured));
    EXPECT_EQ((std::numeric_limits<std::uint64_t>::max)(),
              captured.logicalOutputFramesProduced);
    EXPECT_EQ(20u, captured.progressRevision);
    EXPECT_EQ(1u, captured.progressExhausted);
    EXPECT_EQ(23u, captured.completedThroughBlockSequence);
    EXPECT_EQ(101u, captured.resamplerSourceFrame);
    EXPECT_EQ(202u, captured.sourceFeedCursor);
}

TEST(CompletionProgress, PacksResamplerPhaseRatioLoopAndNextBoundaries) {
    harken::CompletionProgressRow row{};
    harken::CompletionProgressSnapshot initial{};
    initial.occurrence = {3, 7};
    initial.renderGeneration = 11;
    initial.resamplerSourceFrame = 40;
    initial.resamplerSourceFraction = 0.25;
    initial.currentRatio = 1.5;
    initial.targetRatio = 2.0;
    initial.rampStep = 0.125;
    initial.rampRemaining = 8;
    initial.sourceFeedCursor = 41;
    initial.loopStartFrame = 10;
    initial.loopEndFrame = 90;
    initial.completedLoopPasses = 2;
    initial.loopsRemaining = 4;
    initial.nextLoopEndOutputFrame = 120;
    initial.nextExitMarkerOutputFrame = 70;
    initial.loopInfinite = 0;
    initial.hasExitMarker = 1;
    ASSERT_TRUE(harken::InitializeCompletionProgress(row, initial));

    harken::CompletionProgressSnapshot captured{};
    ASSERT_TRUE(harken::CaptureCompletionProgress(row, &captured));
    EXPECT_EQ(40u, captured.resamplerSourceFrame);
    EXPECT_DOUBLE_EQ(0.25, captured.resamplerSourceFraction);
    EXPECT_DOUBLE_EQ(1.5, captured.currentRatio);
    EXPECT_DOUBLE_EQ(2.0, captured.targetRatio);
    EXPECT_DOUBLE_EQ(0.125, captured.rampStep);
    EXPECT_EQ(8u, captured.rampRemaining);
    EXPECT_EQ(41u, captured.sourceFeedCursor);
    EXPECT_EQ(10u, captured.loopStartFrame);
    EXPECT_EQ(90u, captured.loopEndFrame);
    EXPECT_EQ(2u, captured.completedLoopPasses);
    EXPECT_EQ(4u, captured.loopsRemaining);
    EXPECT_EQ(120u, captured.nextLoopEndOutputFrame);
    EXPECT_EQ(70u, captured.nextExitMarkerOutputFrame);
    EXPECT_EQ(0u, captured.loopInfinite);
    EXPECT_EQ(1u, captured.hasExitMarker);
}

TEST(CompletionProgress, UninitializedRowHasNoReadableAuthority) {
    const harken::CompletionProgressRow row{};
    harken::CompletionProgressSnapshot captured{};
    captured.logicalOutputFramesProduced = 71;
    EXPECT_FALSE(harken::CaptureCompletionProgress(row, &captured));
    EXPECT_EQ(71u, captured.logicalOutputFramesProduced);
}

TEST(CompletionProgress, RevisionAndSeqlockExhaustionNeverMasqueradeAsSuccess) {
    constexpr std::uint64_t max =
        (std::numeric_limits<std::uint64_t>::max)();
    harken::CompletionProgressRow revisionRow{};
    harken::CompletionProgressSnapshot initial{};
    initial.occurrence = {2, 5};
    initial.renderGeneration = 9;
    initial.logicalOutputFramesProduced = 10;
    initial.progressRevision = max;
    ASSERT_TRUE(harken::InitializeCompletionProgress(revisionRow, initial));
    harken::CompletionProgressBlockUpdate update{};
    update.occurrence = initial.occurrence;
    update.renderGeneration = initial.renderGeneration;
    update.sourceOutputFramesThisBlock = 4;
    update.completedThroughBlockSequence = 41;

    EXPECT_EQ(harken::CompletionProgressPublishStatus::CounterExhausted,
              harken::PublishCompletionProgressBlock(revisionRow, update));
    harken::CompletionProgressSnapshot captured{};
    ASSERT_TRUE(harken::CaptureCompletionProgress(revisionRow, &captured));
    EXPECT_EQ(14u, captured.logicalOutputFramesProduced);
    EXPECT_EQ(max, captured.progressRevision);
    EXPECT_EQ(1u, captured.progressExhausted);
    EXPECT_EQ(41u, captured.completedThroughBlockSequence);

    harken::CompletionProgressRow seqlockRow{};
    initial.logicalOutputFramesProduced = 33;
    initial.progressRevision = 7;
    initial.progressExhausted = 0;
    ASSERT_TRUE(harken::InitializeCompletionProgress(seqlockRow, initial));
    harken::detail::CompletionProgressTestAccess::SeedSeqlock(
        seqlockRow, max - 1);
    EXPECT_EQ(harken::CompletionProgressPublishStatus::SeqlockExhausted,
              harken::PublishCompletionProgressBlock(seqlockRow, update));
    captured = {};
    ASSERT_TRUE(harken::CaptureCompletionProgress(seqlockRow, &captured));
    EXPECT_EQ(33u, captured.logicalOutputFramesProduced);
    EXPECT_EQ(7u, captured.progressRevision);
    EXPECT_EQ(max - 1,
              harken::detail::CompletionProgressTestAccess::Seqlock(
                  seqlockRow));
}

TEST(CompletionProgress, ExactCaptureRejectsStaleIdentityWithoutOutputMutation) {
    harken::CompletionProgressRow row{};
    harken::CompletionProgressSnapshot initial{};
    initial.occurrence = {4, 8};
    initial.renderGeneration = 12;
    initial.logicalOutputFramesProduced = 27;
    ASSERT_TRUE(harken::InitializeCompletionProgress(row, initial));

    harken::CompletionProgressSnapshot captured{};
    captured.logicalOutputFramesProduced = 99;
    EXPECT_FALSE(harken::CaptureCompletionProgressExact(
        row, {4, 9}, 12, &captured));
    EXPECT_EQ(99u, captured.logicalOutputFramesProduced);
    EXPECT_FALSE(harken::CaptureCompletionProgressExact(
        row, {4, 8}, 13, &captured));
    EXPECT_EQ(99u, captured.logicalOutputFramesProduced);
    ASSERT_TRUE(harken::CaptureCompletionProgressExact(
        row, {4, 8}, 12, &captured));
    EXPECT_EQ(27u, captured.logicalOutputFramesProduced);
}

TEST(CompletionProgress, FirstRenderCatchUpOnlyFillsTheInitialZeroOutputGap) {
    harken::CompletionProgressRow row{};
    harken::CompletionProgressSnapshot initial{};
    initial.occurrence = {5, 9};
    initial.renderGeneration = 13;
    initial.playbackFactsRevision = 17;
    initial.completedThroughBlockSequence = 3;
    ASSERT_TRUE(harken::InitializeCompletionProgress(row, initial));

    ASSERT_EQ(harken::CompletionProgressPublishStatus::Ok,
              harken::CatchUpCompletionProgressFirstRender(
                  row, initial.occurrence, initial.renderGeneration,
                  initial.playbackFactsRevision, 6));
    harken::CompletionProgressSnapshot captured{};
    ASSERT_TRUE(harken::CaptureCompletionProgress(row, &captured));
    EXPECT_EQ(0u, captured.logicalOutputFramesProduced);
    EXPECT_EQ(6u, captured.completedThroughBlockSequence);
    EXPECT_EQ(3u, captured.progressRevision);

    harken::CompletionProgressBlockUpdate update{};
    update.occurrence = initial.occurrence;
    update.renderGeneration = initial.renderGeneration;
    update.sourceOutputFramesThisBlock = 4;
    update.playbackFactsRevision = initial.playbackFactsRevision;
    update.completedThroughBlockSequence = 7;
    ASSERT_EQ(harken::CompletionProgressPublishStatus::Ok,
              harken::PublishCompletionProgressBlock(row, update));
    ASSERT_EQ(harken::CompletionProgressPublishStatus::InvalidState,
              harken::CatchUpCompletionProgressFirstRender(
                  row, initial.occurrence, initial.renderGeneration,
                  initial.playbackFactsRevision, 9));
    ASSERT_TRUE(harken::CaptureCompletionProgress(row, &captured));
    EXPECT_EQ(4u, captured.logicalOutputFramesProduced);
    EXPECT_EQ(7u, captured.completedThroughBlockSequence);
    EXPECT_EQ(4u, captured.progressRevision);
}

TEST(CompletionProgress, ConcurrentPublicationNeverTearsSnapshot) {
    constexpr std::uint64_t publications = 100000;
    harken::CompletionProgressRow row{};
    harken::CompletionProgressSnapshot initial{};
    initial.occurrence = {6, 10};
    initial.renderGeneration = 14;
    initial.loopsRemaining = 1;
    ASSERT_TRUE(harken::InitializeCompletionProgress(row, initial));

    std::atomic<bool> start{false};
    std::atomic<bool> done{false};
    std::atomic<bool> writerFailed{false};
    std::thread writer([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (std::uint64_t revision = 1;
             revision <= publications; ++revision) {
            harken::CompletionProgressBlockUpdate update{};
            update.occurrence = initial.occurrence;
            update.renderGeneration = initial.renderGeneration;
            update.sourceOutputFramesThisBlock = 1;
            update.resamplerSourceFrame = revision * 3;
            update.sourceFeedCursor = revision * 5;
            update.completedLoopPasses =
                static_cast<std::uint32_t>(revision);
            update.loopsRemaining =
                static_cast<std::uint32_t>(revision + 1);
            update.playbackFactsRevision = revision * 7;
            update.completedThroughBlockSequence = revision;
            if (harken::PublishCompletionProgressBlock(row, update) !=
                harken::CompletionProgressPublishStatus::Ok) {
                writerFailed.store(true, std::memory_order_release);
                break;
            }
        }
        done.store(true, std::memory_order_release);
    });

    start.store(true, std::memory_order_release);
    std::uint64_t captures = 0;
    std::uint64_t tears = 0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    bool timedOut = false;
    while (!done.load(std::memory_order_acquire)) {
        harken::CompletionProgressSnapshot captured{};
        if (harken::CaptureCompletionProgressExact(
                row, initial.occurrence, initial.renderGeneration,
                &captured)) {
            ++captures;
            const std::uint64_t revision = captured.progressRevision;
            if (captured.logicalOutputFramesProduced != revision ||
                captured.resamplerSourceFrame != revision * 3 ||
                captured.sourceFeedCursor != revision * 5 ||
                captured.completedLoopPasses != revision ||
                captured.loopsRemaining != revision + 1 ||
                captured.playbackFactsRevision != revision * 7 ||
                captured.completedThroughBlockSequence != revision) {
                ++tears;
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            timedOut = true;
            break;
        }
    }
    writer.join();

    ASSERT_FALSE(timedOut);
    ASSERT_FALSE(writerFailed.load(std::memory_order_acquire));
    ASSERT_GT(captures, 0u);
    EXPECT_EQ(0u, tears);
    harken::CompletionProgressSnapshot final{};
    ASSERT_TRUE(harken::CaptureCompletionProgressExact(
        row, initial.occurrence, initial.renderGeneration, &final));
    EXPECT_EQ(publications, final.progressRevision);
    EXPECT_EQ(publications, final.logicalOutputFramesProduced);
}


TEST(CompletionProgress, BusyPublicationAfterOwnerClearRetainsFenceAndEventuallyReleases) {
    harken::OccurrenceStatePool pool(1);
    harken::OccurrenceToken occurrence{};
    ASSERT_TRUE(pool.Acquire({}, &occurrence));
    ASSERT_TRUE(pool.BindCompletionProgress(occurrence, 13, 11));
    const auto owner = harken::CompletionOwnerRef::Single({5, 7}, 11);
    ASSERT_TRUE(pool.TryPinCompletionOwner(occurrence, owner));
    ASSERT_TRUE(pool.Advance(occurrence, harken::OccurrenceStage::AwaitingEventFence));
    auto& progress = *pool.CompletionProgressOf(occurrence);
    const auto sequence = harken::detail::CompletionProgressTestAccess::Seqlock(progress);
    harken::detail::CompletionProgressTestAccess::SeedSeqlock(progress, sequence + 1);
    EXPECT_TRUE(pool.ClearCompletionOwnerExact(occurrence, owner));
    harken::CompletionOwnerRef captured{};
    EXPECT_FALSE(pool.CaptureCompletionOwner(occurrence, &captured));
    EXPECT_FALSE(pool.CompletionOwnerReleaseFencePassed(occurrence, UINT64_MAX));
    EXPECT_FALSE(pool.ReleaseAfterFence(occurrence, UINT64_MAX));
    EXPECT_FALSE(pool.UnbindCompletionProgress(occurrence, 13));
    harken::detail::CompletionProgressTestAccess::SeedSeqlock(progress, sequence);
    EXPECT_FALSE(pool.TryPinCompletionOwner(occurrence, owner));
    harken::CompletionProgressBlockUpdate update{};
    update.occurrence = occurrence;
    update.renderGeneration = 13;
    update.playbackFactsRevision = 11;
    update.completedThroughBlockSequence = 8;
    ASSERT_EQ(harken::CompletionProgressPublishStatus::Ok,
              harken::PublishCompletionProgressBlock(progress, update));
    EXPECT_TRUE(pool.UnbindCompletionProgress(occurrence, 13));
    EXPECT_FALSE(pool.BindCompletionProgress(occurrence, 13, 11));
    EXPECT_FALSE(pool.CompletionOwnerReleaseFencePassed(occurrence, 8));
    EXPECT_TRUE(pool.CompletionOwnerReleaseFencePassed(occurrence, 9));
    EXPECT_FALSE(pool.ReleaseAfterFence(occurrence, 8));
    EXPECT_TRUE(pool.ReleaseAfterFence(occurrence, 9));
    EXPECT_EQ(0u, pool.OccupiedCount());
}


TEST(CompletionProgress, DeferredOwnerFenceRejectsChangedPlaybackFacts) {
    harken::OccurrenceStatePool pool(1);
    harken::OccurrenceToken occurrence{};
    ASSERT_TRUE(pool.Acquire({}, &occurrence));
    ASSERT_TRUE(pool.BindCompletionProgress(occurrence, 13, 11));
    const auto owner = harken::CompletionOwnerRef::Single({5, 7}, 11);
    ASSERT_TRUE(pool.TryPinCompletionOwner(occurrence, owner));
    ASSERT_TRUE(pool.Advance(occurrence, harken::OccurrenceStage::AwaitingEventFence));
    auto& progress = *pool.CompletionProgressOf(occurrence);
    const auto sequence = harken::detail::CompletionProgressTestAccess::Seqlock(progress);
    harken::detail::CompletionProgressTestAccess::SeedSeqlock(progress, sequence + 1);
    ASSERT_TRUE(pool.ClearCompletionOwnerExact(occurrence, owner));
    harken::detail::CompletionProgressTestAccess::SeedSeqlock(progress, sequence);
    harken::CompletionProgressBlockUpdate update{};
    update.occurrence = occurrence;
    update.renderGeneration = 13;
    update.playbackFactsRevision = 99;
    update.completedThroughBlockSequence = 8;
    ASSERT_EQ(harken::CompletionProgressPublishStatus::Ok,
              harken::PublishCompletionProgressBlock(progress, update));
    EXPECT_FALSE(pool.CompletionOwnerReleaseFencePassed(occurrence, UINT64_MAX));
    EXPECT_FALSE(pool.ReleaseAfterFence(occurrence, UINT64_MAX));
    EXPECT_FALSE(pool.TryPinCompletionOwner(occurrence, owner));
    EXPECT_FALSE(pool.UnbindCompletionProgress(occurrence, 13));
    EXPECT_EQ(1u, pool.OccupiedCount());
}

TEST(CompletionProgress, CompositeBusyLeafDefersReclamationWithoutQuarantine) {
    harken::OccurrenceStatePool occurrences(3);
    harken::CompositeCompletionOwnerPool pool(1, 2);
    harken::OccurrenceToken composed{};
    ASSERT_TRUE(occurrences.Acquire({}, &composed));
    std::array<harken::KnownScheduleLeafV1, 2> leaves{};
    for (std::uint32_t i = 0; i < leaves.size(); ++i) {
        auto& leaf = leaves[i];
        ASSERT_TRUE(occurrences.Acquire({}, &leaf.occurrence));
        leaf.stableLeafOrdinal = i + 1;
        leaf.authoredOffset.denominator = 1;
        leaf.renderGeneration = 13 + i;
        leaf.absoluteLeafStartFrame = 0;
        leaf.predicate.occurrence = leaf.occurrence;
        leaf.predicate.renderGeneration = leaf.renderGeneration;
        leaf.predicate.absoluteLogicalStartFrame = 0;
        leaf.predicate.absoluteLogicalCompleteFrame = 100;
        leaf.predicate.totalLogicalOutputFrames = 100;
        leaf.predicate.playbackFactsRevision = 11 + i;
        leaf.predicate.program.kind = harken::PlayableKindV1::Sound;
        leaf.predicate.program.stableId = {1, 101 + i};
        leaf.predicate.program.world = 13;
        leaf.predicate.program.runtimeSemanticFingerprint = 17;
        ASSERT_TRUE(occurrences.BindCompletionProgress(leaf.occurrence, leaf.renderGeneration,
                                                       leaf.predicate.playbackFactsRevision));
    }
    harken::CompositeCompletionOwnerToken token{};
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok,
              pool.Acquire(composed, 47, leaves.data(), leaves.size(), occurrences, &token));
    auto& progress = *occurrences.CompletionProgressOf(leaves[1].occurrence);
    const auto sequence = harken::detail::CompletionProgressTestAccess::Seqlock(progress);
    harken::detail::CompletionProgressTestAccess::SeedSeqlock(progress, sequence + 1);
    EXPECT_FALSE(pool.Release(token, occurrences, UINT64_MAX));
    EXPECT_EQ(1u, pool.OccupiedCount());
    EXPECT_EQ(0u, pool.QuarantinedCount());
    harken::detail::CompletionProgressTestAccess::SeedSeqlock(progress, sequence);
    harken::CompletionProgressBlockUpdate update{};
    update.occurrence = leaves[1].occurrence;
    update.renderGeneration = leaves[1].renderGeneration;
    update.playbackFactsRevision = leaves[1].predicate.playbackFactsRevision;
    update.completedThroughBlockSequence = 8;
    ASSERT_EQ(harken::CompletionProgressPublishStatus::Ok,
              harken::PublishCompletionProgressBlock(progress, update));
    EXPECT_FALSE(pool.Release(token, occurrences, 8));
    EXPECT_TRUE(pool.Release(token, occurrences, 9));
    EXPECT_EQ(0u, pool.OccupiedCount());
    EXPECT_EQ(0u, pool.QuarantinedCount());
}
