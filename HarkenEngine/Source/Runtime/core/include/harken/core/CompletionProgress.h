#ifndef HARKEN_CORE_COMPLETION_PROGRESS_H
#define HARKEN_CORE_COMPLETION_PROGRESS_H

#include "harken/core/OccurrenceToken.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace harken {

struct CompletionProgressSnapshot {
    OccurrenceToken occurrence{};
    std::uint32_t renderGeneration = 0;
    std::uint64_t logicalOutputFramesProduced = 0;
    std::uint64_t resamplerSourceFrame = 0;
    double resamplerSourceFraction = 0.0;
    double currentRatio = 1.0;
    double targetRatio = 1.0;
    double rampStep = 0.0;
    std::uint32_t rampRemaining = 0;
    std::uint64_t sourceFeedCursor = 0;
    std::uint64_t loopStartFrame = 0;
    std::uint64_t loopEndFrame = 0;
    std::uint32_t completedLoopPasses = 0;
    std::uint32_t loopsRemaining = 0;
    std::uint64_t nextLoopEndOutputFrame = UINT64_MAX;
    std::uint64_t nextExitMarkerOutputFrame = UINT64_MAX;
    std::uint8_t logicalSourceExhausted = 0;
    std::uint8_t progressExhausted = 0;
    std::uint8_t loopInfinite = 0;
    std::uint8_t hasExitMarker = 0;
    std::uint64_t playbackFactsRevision = 0;
    std::uint64_t completedThroughBlockSequence = 0;
    std::uint64_t progressRevision = 0;
};

struct CompletionProgressBlockUpdate;
enum class CompletionProgressPublishStatus : std::uint8_t;

namespace detail {
struct CompletionProgressTestAccess;
}

class CompletionProgressRow {
public:
    CompletionProgressRow() = default;

private:
    friend struct detail::CompletionProgressTestAccess;
    friend bool InitializeCompletionProgress(
        CompletionProgressRow&, const CompletionProgressSnapshot&) noexcept;
    friend CompletionProgressPublishStatus PublishCompletionProgressBlock(
        CompletionProgressRow&,
        const CompletionProgressBlockUpdate&) noexcept;
    friend CompletionProgressPublishStatus
    CatchUpCompletionProgressFirstRender(
        CompletionProgressRow&, OccurrenceToken, std::uint32_t,
        std::uint64_t, std::uint64_t) noexcept;
    friend bool CaptureCompletionProgress(
        const CompletionProgressRow&,
        CompletionProgressSnapshot*) noexcept;

    std::atomic<std::uint64_t> seqlock_{0};
    std::array<std::atomic<std::uint64_t>, 20> payloadWords_{};
};

struct CompletionProgressBlockUpdate {
    OccurrenceToken occurrence{};
    std::uint32_t renderGeneration = 0;
    std::uint32_t sourceOutputFramesThisBlock = 0;
    std::uint64_t resamplerSourceFrame = 0;
    double resamplerSourceFraction = 0.0;
    double currentRatio = 1.0;
    double targetRatio = 1.0;
    double rampStep = 0.0;
    std::uint32_t rampRemaining = 0;
    std::uint64_t sourceFeedCursor = 0;
    std::uint64_t loopStartFrame = 0;
    std::uint64_t loopEndFrame = 0;
    std::uint32_t completedLoopPasses = 0;
    std::uint32_t loopsRemaining = 0;
    std::uint64_t nextLoopEndOutputFrame = UINT64_MAX;
    std::uint64_t nextExitMarkerOutputFrame = UINT64_MAX;
    std::uint8_t logicalSourceExhausted = 0;
    std::uint8_t loopInfinite = 0;
    std::uint8_t hasExitMarker = 0;
    std::uint64_t playbackFactsRevision = 0;
    std::uint64_t completedThroughBlockSequence = 0;
};

enum class CompletionProgressPublishStatus : std::uint8_t {
    Ok = 0,
    CounterExhausted = 1,
    SeqlockExhausted = 2,
    InvalidState = 3,
};

bool InitializeCompletionProgress(
    CompletionProgressRow& row,
    const CompletionProgressSnapshot& snapshot) noexcept;

CompletionProgressPublishStatus PublishCompletionProgressBlock(
    CompletionProgressRow& row,
    const CompletionProgressBlockUpdate& update) noexcept;

CompletionProgressPublishStatus CatchUpCompletionProgressFirstRender(
    CompletionProgressRow& row,
    OccurrenceToken occurrence,
    std::uint32_t renderGeneration,
    std::uint64_t playbackFactsRevision,
    std::uint64_t completedThroughBlockSequence) noexcept;

bool CaptureCompletionProgress(
    const CompletionProgressRow& row,
    CompletionProgressSnapshot* out) noexcept;

bool CaptureCompletionProgressExact(
    const CompletionProgressRow& row,
    OccurrenceToken occurrence,
    std::uint32_t renderGeneration,
    CompletionProgressSnapshot* out) noexcept;

} // namespace harken

#endif // HARKEN_CORE_COMPLETION_PROGRESS_H
