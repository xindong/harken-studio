#include "harken/core/CompletionProgress.h"

#include <bit>
#include <limits>

namespace harken {
namespace {

using ProgressWords = std::array<std::uint64_t, 20>;

std::uint64_t PackF64(double value) noexcept {
    return std::bit_cast<std::uint64_t>(value);
}

double UnpackF64(std::uint64_t bits) noexcept {
    return std::bit_cast<double>(bits);
}

ProgressWords Pack(const CompletionProgressSnapshot& snapshot) noexcept {
    return {
        static_cast<std::uint64_t>(snapshot.occurrence.slot) |
            (static_cast<std::uint64_t>(snapshot.occurrence.generation) << 32),
        snapshot.renderGeneration,
        snapshot.logicalOutputFramesProduced,
        snapshot.resamplerSourceFrame,
        snapshot.sourceFeedCursor,
        static_cast<std::uint64_t>(snapshot.completedLoopPasses) |
            (static_cast<std::uint64_t>(snapshot.loopsRemaining) << 32),
        static_cast<std::uint64_t>(snapshot.logicalSourceExhausted) |
            (static_cast<std::uint64_t>(snapshot.progressExhausted) << 8) |
            (static_cast<std::uint64_t>(snapshot.loopInfinite) << 16) |
            (static_cast<std::uint64_t>(snapshot.hasExitMarker) << 24) |
            (static_cast<std::uint64_t>(snapshot.rampRemaining) << 32),
        snapshot.playbackFactsRevision,
        snapshot.completedThroughBlockSequence,
        snapshot.progressRevision,
        PackF64(snapshot.resamplerSourceFraction),
        PackF64(snapshot.currentRatio),
        PackF64(snapshot.targetRatio),
        PackF64(snapshot.rampStep),
        snapshot.loopStartFrame,
        snapshot.loopEndFrame,
        snapshot.nextLoopEndOutputFrame,
        snapshot.nextExitMarkerOutputFrame,
    };
}

CompletionProgressSnapshot Unpack(const ProgressWords& words) noexcept {
    CompletionProgressSnapshot snapshot{};
    snapshot.occurrence.slot = static_cast<std::uint32_t>(words[0]);
    snapshot.occurrence.generation =
        static_cast<std::uint32_t>(words[0] >> 32);
    snapshot.renderGeneration = static_cast<std::uint32_t>(words[1]);
    snapshot.logicalOutputFramesProduced = words[2];
    snapshot.resamplerSourceFrame = words[3];
    snapshot.sourceFeedCursor = words[4];
    snapshot.completedLoopPasses = static_cast<std::uint32_t>(words[5]);
    snapshot.loopsRemaining = static_cast<std::uint32_t>(words[5] >> 32);
    snapshot.logicalSourceExhausted = static_cast<std::uint8_t>(words[6]);
    snapshot.progressExhausted = static_cast<std::uint8_t>(words[6] >> 8);
    snapshot.loopInfinite = static_cast<std::uint8_t>(words[6] >> 16);
    snapshot.hasExitMarker = static_cast<std::uint8_t>(words[6] >> 24);
    snapshot.rampRemaining = static_cast<std::uint32_t>(words[6] >> 32);
    snapshot.playbackFactsRevision = words[7];
    snapshot.completedThroughBlockSequence = words[8];
    snapshot.progressRevision = words[9];
    snapshot.resamplerSourceFraction = UnpackF64(words[10]);
    snapshot.currentRatio = UnpackF64(words[11]);
    snapshot.targetRatio = UnpackF64(words[12]);
    snapshot.rampStep = UnpackF64(words[13]);
    snapshot.loopStartFrame = words[14];
    snapshot.loopEndFrame = words[15];
    snapshot.nextLoopEndOutputFrame = words[16];
    snapshot.nextExitMarkerOutputFrame = words[17];
    return snapshot;
}

template <class AtomicWords>
ProgressWords LoadWords(const AtomicWords& source) noexcept {
    ProgressWords words{};
    for (std::size_t i = 0; i < words.size(); ++i) {
        words[i] = source[i].load(std::memory_order_seq_cst);
    }
    return words;
}

template <class AtomicWords>
void StoreWords(AtomicWords& destination,
                const ProgressWords& words) noexcept {
    for (std::size_t i = 0; i < words.size(); ++i) {
        destination[i].store(words[i], std::memory_order_seq_cst);
    }
}

bool SameIdentity(const CompletionProgressSnapshot& snapshot,
                  const CompletionProgressBlockUpdate& update) noexcept {
    return snapshot.occurrence.slot == update.occurrence.slot &&
           snapshot.occurrence.generation == update.occurrence.generation &&
           snapshot.renderGeneration == update.renderGeneration;
}

} // namespace

bool InitializeCompletionProgress(
    CompletionProgressRow& row,
    const CompletionProgressSnapshot& snapshot) noexcept {
    constexpr std::uint64_t max =
        (std::numeric_limits<std::uint64_t>::max)();
    if (snapshot.occurrence.generation == 0 ||
        snapshot.renderGeneration == 0 || snapshot.loopInfinite > 1u ||
        snapshot.hasExitMarker > 1u) {
        return false;
    }
    const std::uint64_t sequence =
        row.seqlock_.load(std::memory_order_seq_cst);
    if ((sequence & 1u) != 0 || sequence > max - 2) return false;
    row.seqlock_.store(sequence + 1, std::memory_order_seq_cst);
    StoreWords(row.payloadWords_, Pack(snapshot));
    row.seqlock_.store(sequence + 2, std::memory_order_seq_cst);
    return true;
}

CompletionProgressPublishStatus PublishCompletionProgressBlock(
    CompletionProgressRow& row,
    const CompletionProgressBlockUpdate& update) noexcept {
    constexpr std::uint64_t max =
        (std::numeric_limits<std::uint64_t>::max)();
    const std::uint64_t sequence =
        row.seqlock_.load(std::memory_order_seq_cst);
    if ((sequence & 1u) != 0) {
        return CompletionProgressPublishStatus::InvalidState;
    }
    if (sequence > max - 2) {
        return CompletionProgressPublishStatus::SeqlockExhausted;
    }
    CompletionProgressSnapshot next = Unpack(LoadWords(row.payloadWords_));
    if (!SameIdentity(next, update)) {
        return CompletionProgressPublishStatus::InvalidState;
    }
    if (next.progressExhausted != 0) {
        return CompletionProgressPublishStatus::CounterExhausted;
    }
    next.resamplerSourceFrame = update.resamplerSourceFrame;
    next.resamplerSourceFraction = update.resamplerSourceFraction;
    next.currentRatio = update.currentRatio;
    next.targetRatio = update.targetRatio;
    next.rampStep = update.rampStep;
    next.rampRemaining = update.rampRemaining;
    next.sourceFeedCursor = update.sourceFeedCursor;
    next.loopStartFrame = update.loopStartFrame;
    next.loopEndFrame = update.loopEndFrame;
    next.completedLoopPasses = update.completedLoopPasses;
    next.loopsRemaining = update.loopsRemaining;
    next.nextLoopEndOutputFrame = update.nextLoopEndOutputFrame;
    next.nextExitMarkerOutputFrame = update.nextExitMarkerOutputFrame;
    next.logicalSourceExhausted = update.logicalSourceExhausted;
    next.loopInfinite = update.loopInfinite;
    next.hasExitMarker = update.hasExitMarker;
    next.playbackFactsRevision = update.playbackFactsRevision;
    next.completedThroughBlockSequence =
        update.completedThroughBlockSequence;

    CompletionProgressPublishStatus status =
        CompletionProgressPublishStatus::Ok;
    const bool logicalCounterExhausted =
        update.sourceOutputFramesThisBlock >
        max - next.logicalOutputFramesProduced;
    const bool revisionExhausted = next.progressRevision == max;
    if (!logicalCounterExhausted) {
        next.logicalOutputFramesProduced +=
            update.sourceOutputFramesThisBlock;
    }
    if (logicalCounterExhausted || revisionExhausted) {
        next.progressExhausted = 1;
        status = CompletionProgressPublishStatus::CounterExhausted;
        if (!revisionExhausted) ++next.progressRevision;
    } else {
        ++next.progressRevision;
    }

    row.seqlock_.store(sequence + 1, std::memory_order_seq_cst);
    StoreWords(row.payloadWords_, Pack(next));
    row.seqlock_.store(sequence + 2, std::memory_order_seq_cst);
    return status;
}

CompletionProgressPublishStatus CatchUpCompletionProgressFirstRender(
    CompletionProgressRow& row,
    OccurrenceToken occurrence,
    std::uint32_t renderGeneration,
    std::uint64_t playbackFactsRevision,
    std::uint64_t completedThroughBlockSequence) noexcept {
    constexpr std::uint64_t max =
        (std::numeric_limits<std::uint64_t>::max)();
    const std::uint64_t sequence =
        row.seqlock_.load(std::memory_order_seq_cst);
    if ((sequence & 1u) != 0) {
        return CompletionProgressPublishStatus::InvalidState;
    }
    if (sequence > max - 2) {
        return CompletionProgressPublishStatus::SeqlockExhausted;
    }
    CompletionProgressSnapshot next = Unpack(LoadWords(row.payloadWords_));
    if (next.occurrence.slot != occurrence.slot ||
        next.occurrence.generation != occurrence.generation ||
        next.renderGeneration != renderGeneration ||
        next.playbackFactsRevision != playbackFactsRevision ||
        next.progressExhausted != 0 || next.progressRevision != 0 ||
        completedThroughBlockSequence <
            next.completedThroughBlockSequence) {
        return CompletionProgressPublishStatus::InvalidState;
    }
    const std::uint64_t skippedBlocks =
        completedThroughBlockSequence -
        next.completedThroughBlockSequence;
    if (skippedBlocks > max - next.progressRevision) {
        return CompletionProgressPublishStatus::CounterExhausted;
    }
    if (skippedBlocks == 0) {
        return CompletionProgressPublishStatus::Ok;
    }
    next.completedThroughBlockSequence = completedThroughBlockSequence;
    next.progressRevision += skippedBlocks;
    row.seqlock_.store(sequence + 1, std::memory_order_seq_cst);
    StoreWords(row.payloadWords_, Pack(next));
    row.seqlock_.store(sequence + 2, std::memory_order_seq_cst);
    return CompletionProgressPublishStatus::Ok;
}

bool CaptureCompletionProgress(
    const CompletionProgressRow& row,
    CompletionProgressSnapshot* out) noexcept {
    if (out == nullptr) return false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        const std::uint64_t before =
            row.seqlock_.load(std::memory_order_seq_cst);
        if (before == 0 || (before & 1u) != 0) continue;
        const ProgressWords words = LoadWords(row.payloadWords_);
        const std::uint64_t after =
            row.seqlock_.load(std::memory_order_seq_cst);
        if (before == after) {
            *out = Unpack(words);
            return true;
        }
    }
    return false;
}

bool CaptureCompletionProgressExact(
    const CompletionProgressRow& row,
    OccurrenceToken occurrence,
    std::uint32_t renderGeneration,
    CompletionProgressSnapshot* out) noexcept {
    if (out == nullptr) return false;
    CompletionProgressSnapshot captured{};
    if (!CaptureCompletionProgress(row, &captured) ||
        captured.occurrence.slot != occurrence.slot ||
        captured.occurrence.generation != occurrence.generation ||
        captured.renderGeneration != renderGeneration) {
        return false;
    }
    *out = captured;
    return true;
}

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

} // namespace harken
