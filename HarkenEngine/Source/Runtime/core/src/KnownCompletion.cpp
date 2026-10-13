#include "harken/core/KnownCompletion.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace harken {
namespace {

bool Add(std::uint64_t a, std::uint64_t b,
         std::uint64_t* out) noexcept {
    const std::uint64_t max =
        (std::numeric_limits<std::uint64_t>::max)();
    if (out == nullptr || b > max - a) {
        return false;
    }
    *out = a + b;
    return true;
}

bool Multiply(std::uint64_t a, std::uint64_t b,
              std::uint64_t* out) noexcept {
    const std::uint64_t max =
        (std::numeric_limits<std::uint64_t>::max)();
    if (out == nullptr || (a != 0 && b > max / a)) {
        return false;
    }
    *out = a * b;
    return true;
}

} // namespace

CompletionPredictability MaterializeKnownCompletion(
    const KnownCompletionFactsV1& facts,
    KnownCompletionPredicateV1* out) noexcept {
    if (out == nullptr || facts.occurrence.generation == 0 ||
        facts.renderGeneration == 0 || facts.sourceRate == 0 ||
        facts.mixRate == 0 || !std::isfinite(facts.pitchRatio) ||
        facts.pitchRatio <= 0.0f || facts.playbackFactsRevision == 0 ||
        !ValidPlayableProgramKey(facts.program)) {
        return CompletionPredictability::Invalid;
    }
    if (!facts.residentExactMedia || facts.sourceRate != facts.mixRate ||
        facts.pitchRatio != 1.0f || !facts.pitchFixed ||
        !facts.serialRender || facts.endMutationPossible ||
        facts.loopCount == 0) {
        return CompletionPredictability::SignalOnly;
    }
    if (facts.pcmFrameCount == 0 ||
        facts.sourceStartFrame > facts.pcmFrameCount) {
        return CompletionPredictability::Invalid;
    }

    std::uint64_t total = 0;
    if (facts.loopCount == 1) {
        total = facts.pcmFrameCount - facts.sourceStartFrame;
    } else {
        if (facts.loopStartFrame >= facts.loopEndFrame ||
            facts.loopEndFrame > facts.pcmFrameCount ||
            facts.sourceStartFrame >= facts.loopEndFrame) {
            return CompletionPredictability::Invalid;
        }
        const std::uint64_t first =
            facts.loopEndFrame - facts.sourceStartFrame;
        const std::uint64_t loopLength =
            facts.loopEndFrame - facts.loopStartFrame;
        const std::uint64_t last =
            facts.pcmFrameCount - facts.loopStartFrame;
        std::uint64_t middle = 0;
        std::uint64_t partial = 0;
        if (!Multiply(facts.loopCount - 2u, loopLength, &middle) ||
            !Add(first, middle, &partial) ||
            !Add(partial, last, &total)) {
            return CompletionPredictability::Invalid;
        }
    }
    if (facts.authoredDurationFrames != UINT64_MAX) {
        total = std::min(total, facts.authoredDurationFrames);
    }
    if (total == 0) {
        return CompletionPredictability::Invalid;
    }
    std::uint64_t complete = 0;
    if (!Add(facts.absoluteLogicalStartFrame, total, &complete)) {
        return CompletionPredictability::Invalid;
    }

    KnownCompletionPredicateV1 materialized{};
    materialized.occurrence = facts.occurrence;
    materialized.renderGeneration = facts.renderGeneration;
    materialized.absoluteLogicalStartFrame =
        facts.absoluteLogicalStartFrame;
    materialized.absoluteLogicalCompleteFrame = complete;
    materialized.totalLogicalOutputFrames = total;
    materialized.progressBaselineOutputFrames =
        facts.progressBaselineOutputFrames;
    materialized.progressBaselineRevision =
        facts.progressBaselineRevision;
    materialized.playbackFactsRevision = facts.playbackFactsRevision;
    materialized.progressBaselineCompletedThroughBlockSequence =
        facts.progressBaselineCompletedThroughBlockSequence;
    materialized.program = facts.program;
    *out = materialized;
    return CompletionPredictability::KnownFinite;
}

bool BindKnownCompletionPredicate(
    const KnownCompletionPredicateV1& candidate,
    const PlayableProgramKey& expectedProgram,
    const CompletionOwnerRef& owner,
    KnownCompletionPredicateV1* out) noexcept {
    if (out == nullptr || candidate.owner.kind != CompletionOwnerKind::None ||
        candidate.occurrence.generation == 0 ||
        candidate.renderGeneration == 0 ||
        candidate.totalLogicalOutputFrames == 0 ||
        candidate.absoluteLogicalCompleteFrame <
            candidate.absoluteLogicalStartFrame ||
        candidate.absoluteLogicalCompleteFrame -
                candidate.absoluteLogicalStartFrame !=
            candidate.totalLogicalOutputFrames ||
        candidate.playbackFactsRevision == 0 ||
        !ValidPlayableProgramKey(candidate.program) ||
        !SamePlayableProgramKey(candidate.program, expectedProgram) ||
        owner.factsRevision != candidate.playbackFactsRevision ||
        (owner.kind == CompletionOwnerKind::SingleQueue &&
         (!owner.singleDecision.Valid() || owner.composite.Valid())) ||
        (owner.kind == CompletionOwnerKind::Composite &&
         (!owner.composite.Valid() || owner.singleDecision.Valid())) ||
        (owner.kind != CompletionOwnerKind::SingleQueue &&
         owner.kind != CompletionOwnerKind::Composite)) {
        return false;
    }
    KnownCompletionPredicateV1 bound = candidate;
    bound.owner = owner;
    *out = bound;
    return true;
}

bool IsBoundKnownCompletionPredicate(
    const KnownCompletionPredicateV1& predicate) noexcept {
    if (predicate.owner.factsRevision != predicate.playbackFactsRevision) {
        return false;
    }
    return (predicate.owner.kind == CompletionOwnerKind::SingleQueue &&
            predicate.owner.singleDecision.Valid() &&
            !predicate.owner.composite.Valid()) ||
           (predicate.owner.kind == CompletionOwnerKind::Composite &&
            predicate.owner.composite.Valid() &&
            !predicate.owner.singleDecision.Valid());
}

bool ReanchorKnownCompletionBeforePublication(
    const KnownCompletionPredicateV1& original,
    std::uint64_t absoluteLogicalStartFrame,
    KnownCompletionPredicateV1* out) noexcept {
    if (out == nullptr || !IsBoundKnownCompletionPredicate(original) ||
        original.occurrence.generation == 0 ||
        original.renderGeneration == 0 ||
        original.totalLogicalOutputFrames == 0 ||
        original.absoluteLogicalCompleteFrame <
            original.absoluteLogicalStartFrame ||
        original.absoluteLogicalCompleteFrame -
                original.absoluteLogicalStartFrame !=
            original.totalLogicalOutputFrames ||
        original.totalLogicalOutputFrames >
            UINT64_MAX - absoluteLogicalStartFrame) return false;
    KnownCompletionPredicateV1 candidate = original;
    candidate.absoluteLogicalStartFrame = absoluteLogicalStartFrame;
    candidate.absoluteLogicalCompleteFrame =
        absoluteLogicalStartFrame + original.totalLogicalOutputFrames;
    *out = candidate;
    return true;
}

CompletionProofStatus EvaluateKnownCompletionForQuantum(
    const KnownCompletionPredicateV1& predicate,
    const CompletionProgressSnapshot& snapshot,
    std::uint64_t quantumStartFrame,
    std::uint64_t quantumEndFrame,
    std::uint64_t expectedCompletedThroughBlockSequence,
    std::uint64_t expectedProgressRevision,
    bool currentQuantumScratchAdmitted,
    KnownCompletionQuantumProof* out) noexcept {
    if (out == nullptr || quantumEndFrame <= quantumStartFrame ||
        !IsBoundKnownCompletionPredicate(predicate) ||
        predicate.occurrence.generation == 0 ||
        predicate.renderGeneration == 0 ||
        predicate.playbackFactsRevision == 0 ||
        predicate.totalLogicalOutputFrames == 0 ||
        !ValidPlayableProgramKey(predicate.program) ||
        predicate.absoluteLogicalCompleteFrame <
            predicate.absoluteLogicalStartFrame ||
        predicate.absoluteLogicalCompleteFrame -
                predicate.absoluteLogicalStartFrame !=
            predicate.totalLogicalOutputFrames) {
        return CompletionProofStatus::Invalid;
    }
    if (!currentQuantumScratchAdmitted) {
        return CompletionProofStatus::CurrentQuantumNotAdmitted;
    }
    if (snapshot.occurrence.slot != predicate.occurrence.slot ||
        snapshot.occurrence.generation != predicate.occurrence.generation ||
        snapshot.renderGeneration != predicate.renderGeneration ||
        snapshot.playbackFactsRevision !=
            predicate.playbackFactsRevision ||
        snapshot.progressExhausted != 0 ||
        snapshot.completedThroughBlockSequence !=
            expectedCompletedThroughBlockSequence ||
        snapshot.progressRevision != expectedProgressRevision ||
        expectedProgressRevision < predicate.progressBaselineRevision ||
        expectedCompletedThroughBlockSequence <
            predicate.progressBaselineCompletedThroughBlockSequence ||
        expectedCompletedThroughBlockSequence -
                predicate.progressBaselineCompletedThroughBlockSequence >
            UINT64_MAX - predicate.progressBaselineRevision ||
        predicate.progressBaselineRevision +
                (expectedCompletedThroughBlockSequence -
                 predicate.progressBaselineCompletedThroughBlockSequence) !=
            expectedProgressRevision ||
        snapshot.logicalOutputFramesProduced <
            predicate.progressBaselineOutputFrames) {
        return CompletionProofStatus::ProgressLost;
    }
    const std::uint64_t actual =
        snapshot.logicalOutputFramesProduced -
        predicate.progressBaselineOutputFrames;
    const std::uint64_t beforeStart =
        quantumStartFrame > predicate.absoluteLogicalStartFrame
            ? quantumStartFrame - predicate.absoluteLogicalStartFrame
            : 0;
    const std::uint64_t expectedBefore =
        beforeStart < predicate.totalLogicalOutputFrames
            ? beforeStart
            : predicate.totalLogicalOutputFrames;
    if (actual != expectedBefore ||
        actual > predicate.totalLogicalOutputFrames) {
        return CompletionProofStatus::ProgressLost;
    }
    const std::uint64_t completion =
        predicate.absoluteLogicalCompleteFrame;
    if (completion < quantumStartFrame) {
        return actual == predicate.totalLogicalOutputFrames
                   ? CompletionProofStatus::CompletedBeforeQuantum
                   : CompletionProofStatus::ProgressLost;
    }
    if (completion >= quantumEndFrame) {
        return CompletionProofStatus::CompletesAfterQuantum;
    }
    const std::uint64_t prefixStart =
        predicate.absoluteLogicalStartFrame > quantumStartFrame
            ? predicate.absoluteLogicalStartFrame
            : quantumStartFrame;
    const std::uint64_t expectedInQuantum =
        completion > prefixStart ? completion - prefixStart : 0;
    const std::uint64_t remaining =
        predicate.totalLogicalOutputFrames - actual;
    if (remaining != expectedInQuantum ||
        completion - quantumStartFrame > UINT32_MAX) {
        return CompletionProofStatus::ProgressLost;
    }
    KnownCompletionQuantumProof proof{};
    proof.actualProducedBeforeQuantum = actual;
    proof.remainingLogicalFrames = remaining;
    proof.expectedProducedInQuantumBeforeCompletion = expectedInQuantum;
    proof.activationOffset =
        static_cast<std::uint32_t>(completion - quantumStartFrame);
    *out = proof;
    return CompletionProofStatus::CompletesInQuantum;
}

} // namespace harken
