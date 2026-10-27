#include "Common/ObjectIdTestSupport.h"
#include "harken/core/KnownCompletion.h"

#include <gtest/gtest.h>

namespace {

harken::KnownCompletionFactsV1 KnownFacts() {
    harken::KnownCompletionFactsV1 facts{};
    facts.occurrence = {3, 7};
    facts.renderGeneration = 11;
    facts.program.kind = harken::PlayableKindV1::Sound;
    facts.program.stableId = harkentest::ObjectId(101);
    facts.program.world = 13;
    facts.program.runtimeSemanticFingerprint = 17;
    facts.residentExactMedia = true;
    facts.pcmFrameCount = 1000;
    facts.sourceStartFrame = 100;
    facts.loopStartFrame = 200;
    facts.loopEndFrame = 600;
    facts.loopCount = 1;
    facts.sourceRate = 48000;
    facts.mixRate = 48000;
    facts.pitchRatio = 1.0f;
    facts.pitchFixed = true;
    facts.serialRender = true;
    facts.endMutationPossible = false;
    facts.absoluteLogicalStartFrame = 50;
    facts.progressBaselineOutputFrames = 9;
    facts.progressBaselineRevision = 16;
    facts.playbackFactsRevision = 21;
    return facts;
}

harken::KnownCompletionPredicateV1 BoundForProof(
    const harken::KnownCompletionPredicateV1& candidate) {
    harken::KnownCompletionPredicateV1 bound{};
    EXPECT_TRUE(harken::BindKnownCompletionPredicate(
        candidate, candidate.program,
        harken::CompletionOwnerRef::Single(
            {5, 7}, candidate.playbackFactsRevision),
        &bound));
    return bound;
}

TEST(KnownCompletion, MaterializesCheckedHalfOpenSingleAndFiniteLoopFormula) {
    harken::KnownCompletionPredicateV1 predicate{};
    harken::KnownCompletionFactsV1 facts = KnownFacts();

    ASSERT_EQ(harken::CompletionPredictability::KnownFinite,
              harken::MaterializeKnownCompletion(facts, &predicate));
    EXPECT_EQ(900u, predicate.totalLogicalOutputFrames);
    EXPECT_EQ(950u, predicate.absoluteLogicalCompleteFrame);
    EXPECT_EQ(facts.occurrence.slot, predicate.occurrence.slot);
    EXPECT_EQ(facts.occurrence.generation, predicate.occurrence.generation);
    EXPECT_EQ(facts.renderGeneration, predicate.renderGeneration);
    EXPECT_EQ(facts.program.stableId, predicate.program.stableId);
    EXPECT_EQ(facts.program.world, predicate.program.world);
    EXPECT_EQ(facts.program.runtimeSemanticFingerprint,
              predicate.program.runtimeSemanticFingerprint);
    EXPECT_EQ(facts.progressBaselineOutputFrames,
              predicate.progressBaselineOutputFrames);
    EXPECT_EQ(facts.progressBaselineRevision,
              predicate.progressBaselineRevision);
    EXPECT_EQ(facts.playbackFactsRevision,
              predicate.playbackFactsRevision);
    EXPECT_EQ(harken::CompletionOwnerKind::None, predicate.owner.kind);

    facts.loopCount = 3;
    ASSERT_EQ(harken::CompletionPredictability::KnownFinite,
              harken::MaterializeKnownCompletion(facts, &predicate));
    EXPECT_EQ(1700u, predicate.totalLogicalOutputFrames);
    EXPECT_EQ(1750u, predicate.absoluteLogicalCompleteFrame);
}

TEST(KnownCompletion, AuthoredDurationUsesEarlierNaturalOrTimelineEnd) {
    harken::KnownCompletionFactsV1 facts = KnownFacts();
    harken::KnownCompletionPredicateV1 predicate{};

    facts.authoredDurationFrames = 120;
    ASSERT_EQ(harken::CompletionPredictability::KnownFinite,
              harken::MaterializeKnownCompletion(facts, &predicate));
    EXPECT_EQ(120u, predicate.totalLogicalOutputFrames);
    EXPECT_EQ(170u, predicate.absoluteLogicalCompleteFrame);

    facts.authoredDurationFrames = 900;
    ASSERT_EQ(harken::CompletionPredictability::KnownFinite,
              harken::MaterializeKnownCompletion(facts, &predicate));
    EXPECT_EQ(900u, predicate.totalLogicalOutputFrames);
    EXPECT_EQ(950u, predicate.absoluteLogicalCompleteFrame);

    facts.authoredDurationFrames = 1200;
    ASSERT_EQ(harken::CompletionPredictability::KnownFinite,
              harken::MaterializeKnownCompletion(facts, &predicate));
    EXPECT_EQ(900u, predicate.totalLogicalOutputFrames);
    EXPECT_EQ(950u, predicate.absoluteLogicalCompleteFrame);

    facts.loopCount = 3;
    facts.authoredDurationFrames = 120;
    ASSERT_EQ(harken::CompletionPredictability::KnownFinite,
              harken::MaterializeKnownCompletion(facts, &predicate));
    EXPECT_EQ(120u, predicate.totalLogicalOutputFrames);
    EXPECT_EQ(170u, predicate.absoluteLogicalCompleteFrame);
}

TEST(KnownCompletion, ResampledDurationUsesRenderBoundaryAndOutputDeadline) {
    auto facts = KnownFacts();
    facts.sourceRate = 44100;
    facts.mixRate = 48000;
    facts.pcmFrameCount = 441;
    facts.sourceStartFrame = 147;
    facts.loopCount = 1;
    harken::KnownCompletionPredicateV1 predicate{};
    ASSERT_EQ(harken::CompletionPredictability::KnownFinite,
        harken::MaterializeKnownCompletion(facts, &predicate));
    EXPECT_EQ(321u, predicate.totalLogicalOutputFrames);
    facts.authoredDurationFrames = 300;
    ASSERT_EQ(harken::CompletionPredictability::KnownFinite,
        harken::MaterializeKnownCompletion(facts, &predicate));
    EXPECT_EQ(300u, predicate.totalLogicalOutputFrames);
    EXPECT_EQ(facts.absoluteLogicalStartFrame + 300u, predicate.absoluteLogicalCompleteFrame);
    facts.pcmFrameCount = 1ull << 63;
    facts.sourceStartFrame = 0;
    facts.sourceRate = 24000;
    facts.authoredDurationFrames = 1;
    ASSERT_EQ(harken::CompletionPredictability::KnownFinite,
        harken::MaterializeKnownCompletion(facts, &predicate));
    EXPECT_EQ(1u, predicate.totalLogicalOutputFrames);
}

TEST(KnownCompletion, BindsCandidateOnlyToExactOwnerWithoutMutationOnFailure) {
    harken::KnownCompletionPredicateV1 candidate{};
    const harken::KnownCompletionFactsV1 facts = KnownFacts();
    ASSERT_EQ(harken::CompletionPredictability::KnownFinite,
              harken::MaterializeKnownCompletion(facts, &candidate));
    const harken::KnownCompletionPredicateV1 sentinel{
        {99, 98}, 97, 96, 95, 94, 93, 92, 91};
    harken::KnownCompletionPredicateV1 out = sentinel;

    EXPECT_FALSE(harken::BindKnownCompletionPredicate(
        candidate, candidate.program,
        harken::CompletionOwnerRef::Single({5, 7}, 20), &out));
    EXPECT_EQ(sentinel.occurrence.slot, out.occurrence.slot);
    const harken::CompletionOwnerRef owner =
        harken::CompletionOwnerRef::Single(
            {5, 7}, facts.playbackFactsRevision);
    harken::PlayableProgramKey mismatched = candidate.program;
    ++mismatched.runtimeSemanticFingerprint;
    EXPECT_FALSE(harken::BindKnownCompletionPredicate(
        candidate, mismatched, owner, &out));
    EXPECT_EQ(sentinel.occurrence.slot, out.occurrence.slot);
    ASSERT_TRUE(harken::BindKnownCompletionPredicate(
        candidate, candidate.program, owner, &out));
    EXPECT_TRUE(harken::IsBoundKnownCompletionPredicate(out));
    EXPECT_TRUE(harken::SameCompletionOwner(owner, out.owner));
    EXPECT_EQ(harken::CompletionOwnerKind::None, candidate.owner.kind);
}

TEST(KnownCompletion, ClassifiesSupportedFallbacksAndMalformedFactsExactly) {
    const harken::KnownCompletionPredicateV1 sentinel{
        {99, 98}, 97, 96, 95, 94, 93, 92, 91};
    const auto classify = [&](harken::KnownCompletionFactsV1 facts) {
        harken::KnownCompletionPredicateV1 out = sentinel;
        const harken::CompletionPredictability result =
            harken::MaterializeKnownCompletion(facts, &out);
        if (result != harken::CompletionPredictability::KnownFinite) {
            EXPECT_EQ(sentinel.occurrence.slot, out.occurrence.slot);
            EXPECT_EQ(sentinel.absoluteLogicalCompleteFrame,
                      out.absoluteLogicalCompleteFrame);
        }
        return result;
    };

    harken::KnownCompletionFactsV1 facts = KnownFacts();
    facts.residentExactMedia = false;
    EXPECT_EQ(harken::CompletionPredictability::SignalOnly, classify(facts));
    facts = KnownFacts();
    facts.sourceRate = 44100;
    EXPECT_EQ(harken::CompletionPredictability::KnownFinite, classify(facts));
    facts = KnownFacts();
    facts.pitchRatio = 0.5f;
    EXPECT_EQ(harken::CompletionPredictability::KnownFinite, classify(facts));
    facts = KnownFacts();
    facts.pitchFixed = false;
    EXPECT_EQ(harken::CompletionPredictability::SignalOnly, classify(facts));
    facts = KnownFacts();
    facts.serialRender = false;
    EXPECT_EQ(harken::CompletionPredictability::SignalOnly, classify(facts));
    facts = KnownFacts();
    facts.endMutationPossible = true;
    EXPECT_EQ(harken::CompletionPredictability::SignalOnly, classify(facts));
    facts = KnownFacts();
    facts.loopCount = 0;
    EXPECT_EQ(harken::CompletionPredictability::SignalOnly, classify(facts));

    facts = KnownFacts();
    facts.occurrence.generation = 0;
    EXPECT_EQ(harken::CompletionPredictability::Invalid, classify(facts));
    facts = KnownFacts();
    facts.sourceRate = 0;
    EXPECT_EQ(harken::CompletionPredictability::Invalid, classify(facts));
    facts = KnownFacts();
    facts.sourceStartFrame = facts.pcmFrameCount;
    EXPECT_EQ(harken::CompletionPredictability::Invalid, classify(facts));
    facts = KnownFacts();
    facts.loopCount = 2;
    facts.loopStartFrame = facts.loopEndFrame;
    EXPECT_EQ(harken::CompletionPredictability::Invalid, classify(facts));
}

TEST(KnownCompletion,
     StreamSourceRemainsSignalOnlyAndProducesNoBindableCandidate) {
    const harken::KnownCompletionPredicateV1 sentinel{
        {99, 98}, 97, 96, 95, 94, 93, 92, 91};
    harken::KnownCompletionPredicateV1 out = sentinel;
    harken::KnownCompletionFactsV1 streamFacts = KnownFacts();
    streamFacts.residentExactMedia = false;

    EXPECT_EQ(harken::CompletionPredictability::SignalOnly,
              harken::MaterializeKnownCompletion(streamFacts, &out));
    EXPECT_EQ(sentinel.occurrence.slot, out.occurrence.slot);
    EXPECT_FALSE(harken::IsBoundKnownCompletionPredicate(out));
}

TEST(KnownCompletion, CheckedOverflowIsInvalidAndDoesNotWriteOutput) {
    const harken::KnownCompletionPredicateV1 sentinel{
        {99, 98}, 97, 96, 95, 94, 93, 92, 91};
    harken::KnownCompletionPredicateV1 out = sentinel;
    harken::KnownCompletionFactsV1 facts = KnownFacts();
    facts.absoluteLogicalStartFrame = UINT64_MAX - 10u;
    EXPECT_EQ(harken::CompletionPredictability::Invalid,
              harken::MaterializeKnownCompletion(facts, &out));
    EXPECT_EQ(sentinel.absoluteLogicalCompleteFrame,
              out.absoluteLogicalCompleteFrame);

    facts = KnownFacts();
    facts.pcmFrameCount = UINT64_MAX;
    facts.sourceStartFrame = 0;
    facts.loopStartFrame = 0;
    facts.loopEndFrame = UINT64_MAX - 1u;
    facts.loopCount = UINT32_MAX;
    EXPECT_EQ(harken::CompletionPredictability::Invalid,
              harken::MaterializeKnownCompletion(facts, &out));
    EXPECT_EQ(sentinel.totalLogicalOutputFrames,
              out.totalLogicalOutputFrames);
}

TEST(KnownCompletion, QuantumEvaluatorRejectsUnboundCandidateWithoutMutation) {
    harken::KnownCompletionFactsV1 facts = KnownFacts();
    facts.pcmFrameCount = 400;
    facts.sourceStartFrame = 100;
    facts.absoluteLogicalStartFrame = 100;
    harken::KnownCompletionPredicateV1 candidate{};
    ASSERT_EQ(harken::CompletionPredictability::KnownFinite,
              harken::MaterializeKnownCompletion(facts, &candidate));
    ASSERT_FALSE(harken::IsBoundKnownCompletionPredicate(candidate));
    harken::CompletionProgressSnapshot snapshot{};
    snapshot.occurrence = candidate.occurrence;
    snapshot.renderGeneration = candidate.renderGeneration;
    snapshot.logicalOutputFramesProduced =
        candidate.progressBaselineOutputFrames + 156u;
    snapshot.playbackFactsRevision = candidate.playbackFactsRevision;
    snapshot.completedThroughBlockSequence = 1;
    snapshot.progressRevision = 17;
    const harken::KnownCompletionQuantumProof sentinel{91, 92, 93, 94};
    harken::KnownCompletionQuantumProof out = sentinel;

    EXPECT_EQ(harken::CompletionProofStatus::Invalid,
              harken::EvaluateKnownCompletionForQuantum(
                  candidate, snapshot, 256, 512, 1, 17, true, &out));
    EXPECT_EQ(sentinel.activationOffset, out.activationOffset);
}

TEST(KnownCompletion, EvaluatesExactHalfOpenQuantumBoundaries) {
    harken::KnownCompletionFactsV1 facts = KnownFacts();
    facts.pcmFrameCount = 400;
    facts.sourceStartFrame = 100;
    facts.absoluteLogicalStartFrame = 100;
    facts.progressBaselineOutputFrames = 10;
    harken::KnownCompletionPredicateV1 predicate{};
    ASSERT_EQ(harken::CompletionPredictability::KnownFinite,
              harken::MaterializeKnownCompletion(facts, &predicate));
    predicate = BoundForProof(predicate);
    ASSERT_EQ(400u, predicate.absoluteLogicalCompleteFrame);

    harken::CompletionProgressSnapshot snapshot{};
    snapshot.occurrence = facts.occurrence;
    snapshot.renderGeneration = facts.renderGeneration;
    snapshot.logicalOutputFramesProduced = 166;
    snapshot.playbackFactsRevision = facts.playbackFactsRevision;
    snapshot.completedThroughBlockSequence = 1;
    snapshot.progressRevision = facts.progressBaselineRevision + 1u;
    harken::KnownCompletionQuantumProof proof{};
    ASSERT_EQ(harken::CompletionProofStatus::CompletesInQuantum,
              harken::EvaluateKnownCompletionForQuantum(
                  predicate, snapshot, 256, 512, 1,
                  facts.progressBaselineRevision + 1u, true, &proof));
    EXPECT_EQ(156u, proof.actualProducedBeforeQuantum);
    EXPECT_EQ(144u, proof.remainingLogicalFrames);
    EXPECT_EQ(144u, proof.expectedProducedInQuantumBeforeCompletion);
    EXPECT_EQ(144u, proof.activationOffset);

    snapshot.logicalOutputFramesProduced = 165;
    const harken::KnownCompletionQuantumProof sentinel{91, 92, 93, 94};
    proof = sentinel;
    EXPECT_EQ(harken::CompletionProofStatus::ProgressLost,
              harken::EvaluateKnownCompletionForQuantum(
                  predicate, snapshot, 256, 512, 1,
                  facts.progressBaselineRevision + 1u, true, &proof));
    EXPECT_EQ(sentinel.activationOffset, proof.activationOffset);

    facts.absoluteLogicalStartFrame = 0;
    facts.pcmFrameCount = 512;
    facts.sourceStartFrame = 0;
    ASSERT_EQ(harken::CompletionPredictability::KnownFinite,
              harken::MaterializeKnownCompletion(facts, &predicate));
    predicate = BoundForProof(predicate);
    snapshot.logicalOutputFramesProduced = 266;
    EXPECT_EQ(harken::CompletionProofStatus::CompletesAfterQuantum,
              harken::EvaluateKnownCompletionForQuantum(
                  predicate, snapshot, 256, 512, 1,
                  facts.progressBaselineRevision + 1u, true, &proof));
}

TEST(KnownCompletion, ProofRejectsEveryStaleOrExcessSnapshotWithoutMutation) {
    harken::KnownCompletionFactsV1 facts = KnownFacts();
    facts.pcmFrameCount = 400;
    facts.sourceStartFrame = 100;
    facts.absoluteLogicalStartFrame = 100;
    harken::KnownCompletionPredicateV1 predicate{};
    ASSERT_EQ(harken::CompletionPredictability::KnownFinite,
              harken::MaterializeKnownCompletion(facts, &predicate));
    predicate = BoundForProof(predicate);
    harken::CompletionProgressSnapshot valid{};
    valid.occurrence = facts.occurrence;
    valid.renderGeneration = facts.renderGeneration;
    valid.logicalOutputFramesProduced =
        facts.progressBaselineOutputFrames + 156u;
    valid.playbackFactsRevision = facts.playbackFactsRevision;
    valid.completedThroughBlockSequence = 1;
    valid.progressRevision = 17;
    constexpr std::uint64_t expectedRevision = 17;
    const harken::KnownCompletionQuantumProof sentinel{91, 92, 93, 94};
    const auto rejected = [&](harken::CompletionProgressSnapshot snapshot) {
        harken::KnownCompletionQuantumProof out = sentinel;
        EXPECT_EQ(harken::CompletionProofStatus::ProgressLost,
                  harken::EvaluateKnownCompletionForQuantum(
                      predicate, snapshot, 256, 512, 1,
                      expectedRevision, true, &out));
        EXPECT_EQ(sentinel.activationOffset, out.activationOffset);
    };

    harken::CompletionProgressSnapshot stale = valid;
    ++stale.occurrence.generation;
    rejected(stale);
    stale = valid;
    ++stale.renderGeneration;
    rejected(stale);
    stale = valid;
    ++stale.playbackFactsRevision;
    rejected(stale);
    stale = valid;
    ++stale.completedThroughBlockSequence;
    rejected(stale);
    stale = valid;
    --stale.progressRevision;
    rejected(stale);
    stale = valid;
    stale.progressExhausted = 1;
    rejected(stale);
    stale = valid;
    ++stale.logicalOutputFramesProduced;
    rejected(stale);
}

TEST(KnownCompletion,
     ProofRejectsBaselineRevisionBeyondExpectedRevisionWithoutMutation) {
    harken::KnownCompletionFactsV1 facts = KnownFacts();
    facts.pcmFrameCount = 400;
    facts.sourceStartFrame = 100;
    facts.absoluteLogicalStartFrame = 100;
    harken::KnownCompletionPredicateV1 predicate{};
    ASSERT_EQ(harken::CompletionPredictability::KnownFinite,
              harken::MaterializeKnownCompletion(facts, &predicate));
    predicate = BoundForProof(predicate);
    predicate.progressBaselineRevision = 18;
    harken::CompletionProgressSnapshot snapshot{};
    snapshot.occurrence = predicate.occurrence;
    snapshot.renderGeneration = predicate.renderGeneration;
    snapshot.logicalOutputFramesProduced =
        predicate.progressBaselineOutputFrames + 156u;
    snapshot.playbackFactsRevision = predicate.playbackFactsRevision;
    snapshot.completedThroughBlockSequence = 1;
    snapshot.progressRevision = 17;
    const harken::KnownCompletionQuantumProof sentinel{91, 92, 93, 94};
    harken::KnownCompletionQuantumProof out = sentinel;

    EXPECT_EQ(harken::CompletionProofStatus::ProgressLost,
              harken::EvaluateKnownCompletionForQuantum(
                  predicate, snapshot, 256, 512, 1, 17, true, &out));
    EXPECT_EQ(sentinel.activationOffset, out.activationOffset);
}

TEST(KnownCompletion, ProofHonorsStartCompletionAndOneFrameBoundaries) {
    const auto materialize = [](std::uint64_t start,
                                std::uint64_t total) {
        harken::KnownCompletionFactsV1 facts = KnownFacts();
        facts.absoluteLogicalStartFrame = start;
        facts.pcmFrameCount = facts.sourceStartFrame + total;
        facts.progressBaselineCompletedThroughBlockSequence = 8;
        harken::KnownCompletionPredicateV1 predicate{};
        EXPECT_EQ(harken::CompletionPredictability::KnownFinite,
                  harken::MaterializeKnownCompletion(facts, &predicate));
        return BoundForProof(predicate);
    };
    const auto snapshot = [](const harken::KnownCompletionPredicateV1& predicate,
                             std::uint64_t actual) {
        harken::CompletionProgressSnapshot value{};
        value.occurrence = predicate.occurrence;
        value.renderGeneration = predicate.renderGeneration;
        value.logicalOutputFramesProduced =
            predicate.progressBaselineOutputFrames + actual;
        value.playbackFactsRevision = predicate.playbackFactsRevision;
        value.completedThroughBlockSequence = 9;
        value.progressRevision = 17;
        return value;
    };
    harken::KnownCompletionQuantumProof proof{};

    harken::KnownCompletionPredicateV1 predicate = materialize(300, 20);
    EXPECT_EQ(harken::CompletionProofStatus::CompletesInQuantum,
              harken::EvaluateKnownCompletionForQuantum(
                  predicate, snapshot(predicate, 0), 256, 512, 9, 17,
                  true, &proof));
    EXPECT_EQ(64u, proof.activationOffset);
    EXPECT_EQ(20u, proof.expectedProducedInQuantumBeforeCompletion);

    predicate = materialize(200, 56);
    EXPECT_EQ(harken::CompletionProofStatus::CompletesInQuantum,
              harken::EvaluateKnownCompletionForQuantum(
                  predicate, snapshot(predicate, 56), 256, 512, 9, 17,
                  true, &proof));
    EXPECT_EQ(0u, proof.activationOffset);

    predicate = materialize(256, 256);
    EXPECT_EQ(harken::CompletionProofStatus::CompletesAfterQuantum,
              harken::EvaluateKnownCompletionForQuantum(
                  predicate, snapshot(predicate, 0), 256, 512, 9, 17,
                  true, &proof));

    predicate = materialize(511, 1);
    EXPECT_EQ(harken::CompletionProofStatus::CompletesAfterQuantum,
              harken::EvaluateKnownCompletionForQuantum(
                  predicate, snapshot(predicate, 0), 256, 512, 9, 17,
                  true, &proof));
    EXPECT_EQ(harken::CompletionProofStatus::CompletesInQuantum,
              harken::EvaluateKnownCompletionForQuantum(
                  predicate, snapshot(predicate, 1), 512, 768, 9, 17,
                  true, &proof));
    EXPECT_EQ(0u, proof.activationOffset);
    EXPECT_EQ(harken::CompletionProofStatus::ProgressLost,
              harken::EvaluateKnownCompletionForQuantum(
                  predicate, snapshot(predicate, 0), 512, 768, 9, 17,
                  true, &proof));

    predicate = materialize(100, 20);
    EXPECT_EQ(harken::CompletionProofStatus::CompletedBeforeQuantum,
              harken::EvaluateKnownCompletionForQuantum(
                  predicate, snapshot(predicate, 20), 256, 512, 9, 17,
                  true, &proof));
}

TEST(KnownCompletion, CurrentQuantumAdmissionIsExplicitAndCannotBeInferredFromStaticFacts) {
    harken::KnownCompletionFactsV1 facts = KnownFacts();
    harken::KnownCompletionPredicateV1 predicate{};
    ASSERT_EQ(harken::CompletionPredictability::KnownFinite,
              harken::MaterializeKnownCompletion(facts, &predicate));
    predicate = BoundForProof(predicate);

    harken::CompletionProgressSnapshot snapshot{};
    snapshot.occurrence = predicate.occurrence;
    snapshot.renderGeneration = predicate.renderGeneration;
    snapshot.logicalOutputFramesProduced =
        predicate.progressBaselineOutputFrames;
    snapshot.playbackFactsRevision = predicate.playbackFactsRevision;
    snapshot.completedThroughBlockSequence = 9;
    snapshot.progressRevision = 17;
    const harken::KnownCompletionQuantumProof sentinel{91, 92, 93, 94};
    harken::KnownCompletionQuantumProof out = sentinel;

    EXPECT_EQ(harken::CompletionProofStatus::CurrentQuantumNotAdmitted,
              harken::EvaluateKnownCompletionForQuantum(
                  predicate, snapshot, 0, 256, 9, 17, false, &out));
    EXPECT_EQ(sentinel.activationOffset, out.activationOffset);
}

TEST(KnownCompletion, ForgedPredicateIsInvalidWithoutOutputMutation) {
    harken::KnownCompletionFactsV1 facts = KnownFacts();
    harken::KnownCompletionPredicateV1 predicate{};
    ASSERT_EQ(harken::CompletionPredictability::KnownFinite,
              harken::MaterializeKnownCompletion(facts, &predicate));
    predicate = BoundForProof(predicate);
    ++predicate.absoluteLogicalCompleteFrame;
    harken::CompletionProgressSnapshot snapshot{};
    snapshot.occurrence = predicate.occurrence;
    snapshot.renderGeneration = predicate.renderGeneration;
    snapshot.playbackFactsRevision = predicate.playbackFactsRevision;
    const harken::KnownCompletionQuantumProof sentinel{91, 92, 93, 94};
    harken::KnownCompletionQuantumProof out = sentinel;
    EXPECT_EQ(harken::CompletionProofStatus::Invalid,
              harken::EvaluateKnownCompletionForQuantum(
                  predicate, snapshot, 0, 256, 1, 1, true, &out));
    EXPECT_EQ(sentinel.activationOffset, out.activationOffset);
}

TEST(KnownCompletion, ProofRequiresOneProgressRevisionPerCompletedBlock) {
    harken::KnownCompletionFactsV1 facts = KnownFacts();
    facts.pcmFrameCount = 400;
    facts.sourceStartFrame = 100;
    facts.absoluteLogicalStartFrame = 100;
    facts.progressBaselineOutputFrames = 10;
    facts.progressBaselineRevision = 4;
    facts.progressBaselineCompletedThroughBlockSequence = 10;
    harken::KnownCompletionPredicateV1 candidate{};
    ASSERT_EQ(harken::CompletionPredictability::KnownFinite,
              harken::MaterializeKnownCompletion(facts, &candidate));
    const auto predicate = BoundForProof(candidate);
    harken::CompletionProgressSnapshot snapshot{};
    snapshot.occurrence = predicate.occurrence;
    snapshot.renderGeneration = predicate.renderGeneration;
    snapshot.logicalOutputFramesProduced = 166;
    snapshot.playbackFactsRevision = predicate.playbackFactsRevision;
    snapshot.completedThroughBlockSequence = 13;
    snapshot.progressRevision = 6;
    harken::KnownCompletionQuantumProof proof{91, 92, 93, 94};

    EXPECT_EQ(harken::CompletionProofStatus::ProgressLost,
              harken::EvaluateKnownCompletionForQuantum(
                  predicate, snapshot, 256, 512, 13, 6, true, &proof));
    EXPECT_EQ(94u, proof.activationOffset);
    snapshot.progressRevision = 7;
    EXPECT_EQ(harken::CompletionProofStatus::CompletesInQuantum,
              harken::EvaluateKnownCompletionForQuantum(
                  predicate, snapshot, 256, 512, 13, 7, true, &proof));
}

} // namespace
