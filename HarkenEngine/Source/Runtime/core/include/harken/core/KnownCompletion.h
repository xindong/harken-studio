#ifndef HARKEN_CORE_KNOWN_COMPLETION_H
#define HARKEN_CORE_KNOWN_COMPLETION_H

#include "harken/core/CompletionOwner.h"
#include "harken/core/CompletionProgress.h"
#include "harken/core/PlayableProgramKey.h"

#include <cstdint>

namespace harken {

enum class CompletionPredictability : std::uint8_t {
    KnownFinite = 0,
    SignalOnly = 1,
    Invalid = 2,
};

struct KnownCompletionFactsV1 {
    OccurrenceToken occurrence{};
    std::uint32_t renderGeneration = 0;
    bool residentExactMedia = false;
    std::uint64_t pcmFrameCount = 0;
    std::uint64_t sourceStartFrame = 0;
    std::uint64_t loopStartFrame = 0;
    std::uint64_t loopEndFrame = 0;
    std::uint32_t loopCount = 0;
    std::uint32_t sourceRate = 0;
    std::uint32_t mixRate = 0;
    float pitchRatio = 1.0f;
    bool pitchFixed = false;
    bool serialRender = false;
    bool endMutationPossible = false;
    std::uint64_t authoredDurationFrames = UINT64_MAX;
    std::uint64_t absoluteLogicalStartFrame = 0;
    std::uint64_t progressBaselineOutputFrames = 0;
    std::uint64_t progressBaselineRevision = 0;
    std::uint64_t progressBaselineCompletedThroughBlockSequence = 0;
    std::uint64_t playbackFactsRevision = 0;
    PlayableProgramKey program{};
};

struct KnownCompletionPredicateV1 {
    OccurrenceToken occurrence{};
    std::uint32_t renderGeneration = 0;
    std::uint64_t absoluteLogicalStartFrame = 0;
    std::uint64_t absoluteLogicalCompleteFrame = 0;
    std::uint64_t totalLogicalOutputFrames = 0;
    std::uint64_t progressBaselineOutputFrames = 0;
    std::uint64_t progressBaselineRevision = 0;
    std::uint64_t playbackFactsRevision = 0;
    std::uint64_t progressBaselineCompletedThroughBlockSequence = 0;
    PlayableProgramKey program{};
    CompletionOwnerRef owner{};
};

enum class CompletionProofStatus : std::uint8_t {
    CompletesInQuantum = 0,
    CompletesAfterQuantum = 1,
    ProgressLost = 2,
    Invalid = 3,
    CompletedBeforeQuantum = 4,
    CurrentQuantumNotAdmitted = 5,
};

struct KnownCompletionQuantumProof {
    std::uint64_t actualProducedBeforeQuantum = 0;
    std::uint64_t remainingLogicalFrames = 0;
    std::uint64_t expectedProducedInQuantumBeforeCompletion = 0;
    std::uint32_t activationOffset = 0;
};

CompletionPredictability MaterializeKnownCompletion(
    const KnownCompletionFactsV1& facts,
    KnownCompletionPredicateV1* out) noexcept;

bool BindKnownCompletionPredicate(
    const KnownCompletionPredicateV1& candidate,
    const PlayableProgramKey& expectedProgram,
    const CompletionOwnerRef& owner,
    KnownCompletionPredicateV1* out) noexcept;

bool IsBoundKnownCompletionPredicate(
    const KnownCompletionPredicateV1& predicate) noexcept;

bool ReanchorKnownCompletionBeforePublication(
    const KnownCompletionPredicateV1& original,
    std::uint64_t absoluteLogicalStartFrame,
    KnownCompletionPredicateV1* out) noexcept;

CompletionProofStatus EvaluateKnownCompletionForQuantum(
    const KnownCompletionPredicateV1& predicate,
    const CompletionProgressSnapshot& snapshot,
    std::uint64_t quantumStartFrame,
    std::uint64_t quantumEndFrame,
    std::uint64_t expectedCompletedThroughBlockSequence,
    std::uint64_t expectedProgressRevision,
    bool currentQuantumScratchAdmitted,
    KnownCompletionQuantumProof* out) noexcept;

} // namespace harken

#endif // HARKEN_CORE_KNOWN_COMPLETION_H
