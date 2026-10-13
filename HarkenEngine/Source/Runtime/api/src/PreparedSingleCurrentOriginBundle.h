// Internal authentic Single preparation adapter. Production issuance remains
// disabled until stable native factory baselines and cleanup callbacks connect.
#ifndef HARKEN_API_PREPARED_SINGLE_CURRENT_ORIGIN_BUNDLE_H
#define HARKEN_API_PREPARED_SINGLE_CURRENT_ORIGIN_BUNDLE_H

#include "harken/core/DecisionTable.h"
#include "harken/core/PlayableInstanceTable.h"
#include "harken/core/OccurrenceStatePool.h"
#include "harken/core/VoiceLedger.h"
#include "harken/render/OriginalSourceSubmission.h"
#include <utility>

namespace harken {

struct Engine;

// All receivers outlive the bundle and its loans. No PCM reader, timing
// mutation, Queue selection, general Decision retarget or Reset right is issued.
class PreparedSingleCurrentOriginBundle {
public:
    using SourcePin = OriginalSourceSubmissionRegistry::OriginalSourceLifetimePin;
    using BodyPin = VoicePool::OriginalSourceBodyLifetimePin;
    using OccurrencePin = OccurrenceStatePool::OriginalSingleCompletionOwnerPin;
    using LedgerPin = VoiceLedger::OriginalCompletionMutationOwner;
    using OccurrenceProof = OccurrenceStatePool::PreparedSingleCompletionCommitProof;
    using LedgerProof = VoiceLedger::PreparedSingleCompletionCommitProof;
    PreparedSingleCurrentOriginBundle() noexcept = default;
    PreparedSingleCurrentOriginBundle(const PreparedSingleCurrentOriginBundle&) = delete;
    PreparedSingleCurrentOriginBundle& operator=(const PreparedSingleCurrentOriginBundle&) = delete;
    PreparedSingleCurrentOriginBundle(PreparedSingleCurrentOriginBundle&&) noexcept = default;
    PreparedSingleCurrentOriginBundle& operator=(PreparedSingleCurrentOriginBundle&&) noexcept = default;
    ~PreparedSingleCurrentOriginBundle() = default;

    // All fallible native borrows complete before the caller may release Head.
    static bool Prepare(OriginalSourceSubmissionRegistry& sources, VoicePool& voices,
        TimelineArmTable& arms, DecisionTable& decisions, OccurrenceStatePool& occurrences,
        VoiceLedger& ledger, PlayableInstanceTable& instances,
        const PlayableInstanceTable::PreparedParentLinksProof& playablePreparation,
        const SourcePin& source, const BodyPin& body,
        const OriginalDecisionLifetimePin& decision, const PreparedDecisionReservation& decisionPreparation,
        const OccurrencePin& occurrence, const OccurrenceProof& occurrencePreparation,
        const LedgerPin& logicalVoice, const LedgerProof& ledgerPreparation,
        const TimelineArmTable::PreparedCommitProof& armPreparation,
        const VoicePool::PreparedCommitProof& voicePreparation, const VoicePool::PreparedCommitInput& physical,
        const CommandQueue::PreparedBatchProof& batch, TimelineArmToken arm, VnVoiceHandle sourceIdentity,
        std::uint64_t scheduleEpoch, PreparedSingleCurrentOriginBundle* out) noexcept {
        if (out == nullptr || out->Valid() || !ledger.MatchesPreparedSingleCompletionCommit(ledgerPreparation)) return false;
        const auto& logical = ledgerPreparation.OriginalInput();
        const auto& mutation = logical.completionMutation;
        const auto arranging = occurrencePreparation.OriginalArrangingInstance();
        if (!instances.MatchesPreparedOriginalAncestry(playablePreparation, arranging, logical.parent.internalToken) ||
            !voices.IsOriginalSourceBodyLifetimePinFor(body, arm, sourceIdentity) ||
            !decisions.MatchesOriginalPreparedDecision(decision, decisionPreparation) ||
            !decisions.IsOriginalDecisionLifetimePinFor(decision, mutation.owner.singleDecision) ||
            !occurrences.IsOriginalSingleCompletionOwnerPinFor(occurrence, mutation.occurrence) ||
            !occurrences.MatchesPreparedSingleCompletionCommit(occurrencePreparation, mutation.occurrence,
                mutation.owner, physical.completionProgress, physical.completionRenderGeneration,
                scheduleEpoch, arranging) ||
            !ledger.IsOriginalCompletionMutationOwnerFor(logicalVoice, logical.identity, logical.parent,
                logical.renderHandle, mutation) || armPreparation.Owner() != &arms ||
            logical.renderHandle != physical.renderHandle || physical.completionOccurrence.slot != mutation.occurrence.slot ||
            physical.completionOccurrence.generation != mutation.occurrence.generation ||
            physical.playbackFactsRevision != mutation.owner.factsRevision ||
            !sources.MatchesPreparedOriginalSourcePair(source, body, armPreparation,
                voicePreparation, physical, batch, arm, sourceIdentity)) return false;
        PreparedSingleCurrentOriginBundle prepared;
        if (!source.Borrow(&prepared.source_) || !body.Borrow(&prepared.body_) ||
            !decision.Borrow(&prepared.decision_) || !occurrence.Borrow(&prepared.occurrence_) ||
            !logicalVoice.Borrow(&prepared.ledger_)) return false;
        prepared.sources_ = &sources;
        prepared.voices_ = &voices;
        prepared.arms_ = &arms;
        prepared.decisions_ = &decisions;
        prepared.occurrences_ = &occurrences;
        prepared.logicalVoices_ = &ledger;
        prepared.logical_ = logical;
        prepared.instances_ = &instances;
        prepared.arrangingInstance_ = arranging;
        prepared.arm_ = arm;
        prepared.sourceIdentity_ = sourceIdentity;
        prepared.originalPhysicalRelease_ = physical.originalRelease;
        prepared.progress_ = physical.completionProgress;
        prepared.scheduleEpoch_ = scheduleEpoch;
        *out = std::move(prepared);
        return true;
    }

    static bool PrepareWithOriginalPlayableLifetimes(OriginalSourceSubmissionRegistry& sources, VoicePool& voices,
        TimelineArmTable& arms, DecisionTable& decisions, OccurrenceStatePool& occurrences,
        VoiceLedger& ledger, PlayableInstanceTable& instances,
        const PlayableInstanceTable::PreparedParentLinksProof& playablePreparation,
        const PlayableInstanceTable::OriginalLifetimePin& rootOriginal,
        const PlayableInstanceTable::OriginalLifetimePin& arrangingOriginal,
        const SourcePin& source, const BodyPin& body,
        const OriginalDecisionLifetimePin& decision, const PreparedDecisionReservation& decisionPreparation,
        const OccurrencePin& occurrence, const OccurrenceProof& occurrencePreparation,
        const LedgerPin& logicalVoice, const LedgerProof& ledgerPreparation,
        const TimelineArmTable::PreparedCommitProof& armPreparation,
        const VoicePool::PreparedCommitProof& voicePreparation, const VoicePool::PreparedCommitInput& physical,
        const CommandQueue::PreparedBatchProof& batch, TimelineArmToken arm, VnVoiceHandle sourceIdentity,
        std::uint64_t scheduleEpoch, PreparedSingleCurrentOriginBundle* out) noexcept {
        if (out == nullptr || out->sources_ != nullptr || !ledger.MatchesPreparedSingleCompletionCommit(ledgerPreparation)) return false;
        const auto root = ledgerPreparation.OriginalInput().parent.internalToken;
        const auto arranging = occurrencePreparation.OriginalArrangingInstance();
        if (!instances.IsOriginalLifetimePinFor(rootOriginal, root) ||
            !instances.IsOriginalLifetimePinFor(arrangingOriginal, arranging) ||
            !instances.IsOriginalLifetimeCurrent(rootOriginal) || !instances.IsOriginalLifetimeCurrent(arrangingOriginal)) return false;
        PreparedSingleCurrentOriginBundle prepared;
        if (!Prepare(sources, voices, arms, decisions, occurrences, ledger, instances, playablePreparation,
                source, body, decision, decisionPreparation, occurrence, occurrencePreparation,
                logicalVoice, ledgerPreparation, armPreparation, voicePreparation, physical, batch,
                arm, sourceIdentity, scheduleEpoch, &prepared) || !rootOriginal.Borrow(&prepared.rootOriginal_) ||
            (root != arranging && !arrangingOriginal.Borrow(&prepared.arrangingOriginal_))) return false;
        prepared.playableRetention_ = true;
        *out = std::move(prepared);
        return true;
    }
    // Authentic immutable association views. They never issue a new cap or
    // upgrade Prepared installation into Started, Current mutation or Ready.
    DecisionToken OriginalCurrentSingleDecision() const noexcept {
        return Valid() ? logical_.completionMutation.owner.singleDecision : DecisionToken{};
    }
    OccurrenceToken OriginalCurrentOccurrence() const noexcept {
        return Valid() ? logical_.completionMutation.occurrence : OccurrenceToken{};
    }
    CompletionOwnerRef OriginalCurrentCompletionOwner() const noexcept {
        return Valid() ? logical_.completionMutation.owner : CompletionOwnerRef{};
    }
    TimelineArmToken OriginalCurrentSourceArm() const noexcept { return Valid() ? arm_ : TimelineArmToken{}; }
    VnVoiceHandle OriginalCurrentSourceIdentity() const noexcept { return Valid() ? sourceIdentity_ : 0; }
    const SourcePin* OriginalSourceLifetime() const noexcept { return Valid() ? &source_ : nullptr; }
    const BodyPin* OriginalSourceBodyLifetime() const noexcept { return Valid() ? &body_ : nullptr; }
    const OriginalDecisionLifetimePin* OriginalDecisionLifetime() const noexcept { return Valid() ? &decision_ : nullptr; }
    const OccurrencePin* OriginalOccurrenceLifetime() const noexcept { return Valid() ? &occurrence_ : nullptr; }
    const LedgerPin* OriginalLedgerLifetime() const noexcept { return Valid() ? &ledger_ : nullptr; }
    const PlayableInstanceTable::OriginalLifetimePin* OriginalRootLifetime() const noexcept {
        return HasOriginalPlayableRetention() ? &rootOriginal_ : nullptr;
    }
    const PlayableInstanceTable::OriginalLifetimePin* OriginalArrangingLifetime() const noexcept {
        if (!HasOriginalPlayableRetention()) return nullptr;
        return arrangingInstance_ == logical_.parent.internalToken ? &rootOriginal_ : &arrangingOriginal_;
    }
    // This is the actual observer captured and authenticated by the original
    // paired Source/Voice factory. Disposed command storage is not audio release.
    bool OriginalPhysicalReleaseObserved() const noexcept {
        return (componentsClosed_ ? HasOriginalPlayableRetention() : Valid()) && voices_ != nullptr &&
            originalPhysicalRelease_.Valid() && voices_->ReleaseObserved(originalPhysicalRelease_);
    }
    bool HasOriginalPlayableRetention() const noexcept {
        return playableRetention_ && instances_ != nullptr &&
            instances_->IsOriginalLifetimePinFor(rootOriginal_, logical_.parent.internalToken) &&
            (arrangingInstance_ == logical_.parent.internalToken ||
             instances_->IsOriginalLifetimePinFor(arrangingOriginal_, arrangingInstance_));
    }
    bool MatchesOriginalPlayable(PlayableInstanceTable& instances, PlayableInstanceHandle handle) const noexcept {
        return instances_ == &instances && HasOriginalPlayableRetention() &&
            (handle == logical_.parent.internalToken || handle == arrangingInstance_);
    }

    bool Valid() const noexcept {
        return (!playableRetention_ || HasOriginalPlayableRetention()) && sources_ != nullptr && voices_ != nullptr && arms_ != nullptr && decisions_ != nullptr &&
            occurrences_ != nullptr && logicalVoices_ != nullptr && instances_ != nullptr && arrangingInstance_ != 0 &&
            source_.Valid() && body_.Valid() && decision_.Valid() && occurrence_.Valid() && ledger_.Valid() &&
            sources_->IsOriginalSourceLifetimePinFor(source_, arm_) &&
            voices_->IsOriginalSourceBodyLifetimePinFor(body_, arm_, sourceIdentity_) &&
            decisions_->IsOriginalDecisionLifetimePinFor(decision_, logical_.completionMutation.owner.singleDecision) &&
            occurrences_->IsOriginalSingleCompletionOwnerPinFor(occurrence_, logical_.completionMutation.occurrence) &&
            logicalVoices_->IsOriginalCompletionMutationOwnerFor(ledger_, logical_.identity, logical_.parent,
                logical_.renderHandle, logical_.completionMutation);
    }
    bool Borrow(PreparedSingleCurrentOriginBundle* out) const noexcept {
        if (out == nullptr || out->Valid() || out == this || !Valid()) return false;
        PreparedSingleCurrentOriginBundle loan;
        if (!source_.Borrow(&loan.source_) || !body_.Borrow(&loan.body_) ||
            !decision_.Borrow(&loan.decision_) || !occurrence_.Borrow(&loan.occurrence_) ||
            !ledger_.Borrow(&loan.ledger_)) return false;
        if (playableRetention_ && (!rootOriginal_.Borrow(&loan.rootOriginal_) ||
            (arrangingInstance_ != logical_.parent.internalToken &&
             !arrangingOriginal_.Borrow(&loan.arrangingOriginal_)))) return false;
        loan.playableRetention_ = playableRetention_;
        loan.sources_ = sources_;
        loan.voices_ = voices_;
        loan.arms_ = arms_;
        loan.decisions_ = decisions_;
        loan.occurrences_ = occurrences_;
        loan.logicalVoices_ = logicalVoices_;
        loan.logical_ = logical_;
        loan.instances_ = instances_;
        loan.arrangingInstance_ = arrangingInstance_;
        loan.arm_ = arm_;
        loan.sourceIdentity_ = sourceIdentity_;
        loan.originalPhysicalRelease_ = originalPhysicalRelease_;
        loan.progress_ = progress_;
        loan.scheduleEpoch_ = scheduleEpoch_;
        *out = std::move(loan);
        return true;
    }
    void Release() noexcept {
        ledger_.Release();
        occurrence_.Release();
        decision_.Release();
        body_.Release();
        source_.Release();
        arrangingOriginal_.Release();
        rootOriginal_.Release();
        playableRetention_ = false;
        componentsClosed_ = false;
        sources_ = nullptr; voices_ = nullptr; arms_ = nullptr; decisions_ = nullptr;
        occurrences_ = nullptr; logicalVoices_ = nullptr; logical_ = {};
        instances_ = nullptr; arrangingInstance_ = 0;
        arm_ = {}; sourceIdentity_ = 0; originalPhysicalRelease_ = {};
        progress_ = nullptr; scheduleEpoch_ = 0;
    }
    bool MatchesPrepared(const PlayableInstanceTable::PreparedParentLinksProof& playablePreparation,
        const PreparedDecisionReservation& decisionPreparation,
        const OccurrenceProof& occurrencePreparation, const LedgerProof& ledgerPreparation,
        const TimelineArmTable::PreparedCommitProof& armPreparation,
        const VoicePool::PreparedCommitProof& voicePreparation,
        const VoicePool::PreparedCommitInput& physical, const CommandQueue::PreparedBatchProof& batch) const noexcept {
        if (!Valid() || !logicalVoices_->MatchesPreparedSingleCompletionCommit(ledgerPreparation)) return false;
        const auto& logical = ledgerPreparation.OriginalInput();
        return occurrencePreparation.OriginalArrangingInstance() == arrangingInstance_ &&
            instances_->MatchesPreparedOriginalAncestry(playablePreparation, arrangingInstance_, logical_.parent.internalToken) &&
            logical.identity.slot == logical_.identity.slot && logical.identity.generation == logical_.identity.generation &&
            logical.parent.previewHandle == logical_.parent.previewHandle && logical.parent.internalToken == logical_.parent.internalToken &&
            logical.renderHandle == logical_.renderHandle && logical.priority == logical_.priority &&
            SameCompletionMutationBinding(logical.completionMutation, logical_.completionMutation) &&
            physical.renderHandle == logical_.renderHandle && physical.completionProgress == progress_ &&
            physical.completionOccurrence.slot == logical_.completionMutation.occurrence.slot &&
            physical.completionOccurrence.generation == logical_.completionMutation.occurrence.generation &&
            physical.playbackFactsRevision == logical_.completionMutation.owner.factsRevision &&
            decisions_->MatchesOriginalPreparedDecision(decision_, decisionPreparation) &&
            occurrences_->MatchesPreparedSingleCompletionCommit(occurrencePreparation, logical_.completionMutation.occurrence,
                logical_.completionMutation.owner, progress_, physical.completionRenderGeneration,
                scheduleEpoch_, arrangingInstance_) &&
            sources_->MatchesPreparedOriginalSourcePair(source_, body_, armPreparation,
                voicePreparation, physical, batch, arm_, sourceIdentity_);
    }
    // This observes component installation, not whole Queue/Reset admission.
    // A gated future Source can be installed while its real Start is still zero.
    bool ObserveInstalled() const noexcept {
        return Valid() && (!playableRetention_ ||
            (instances_->IsOriginalLifetimeCurrent(rootOriginal_) &&
             (arrangingInstance_ == logical_.parent.internalToken ||
              instances_->IsOriginalLifetimeCurrent(arrangingOriginal_)))) && !decisions_->IsCancelled(logical_.completionMutation.owner.singleDecision) &&
            sources_->IsOriginalInstalledSourceCurrent(source_, sourceIdentity_) &&
            voices_->IsOriginalSourceBodyCurrent(body_) &&
            logicalVoices_->IsCurrentOriginalCompletionMutationOwner(ledger_) &&
            occurrences_->IsOriginalSingleCompletionOwnerCurrent(occurrence_, logical_.completionMutation.owner,
                progress_, arm_.renderGeneration, scheduleEpoch_);
    }
    bool CaptureStartedSequence(std::uint64_t* out) const noexcept {
        if (out == nullptr || !ObserveInstalled()) return false;
        TimelineArmObservation original{};
        if (!arms_->Observe(arm_, &original) || original.armedCompletionSequence == 0 ||
            original.startedBlockSequence == 0) return false;
        *out = original.startedBlockSequence;
        return true;
    }
private:
    friend struct Engine;
    // Engine calls this only for its actual installed factory association.
    // Real physical release returns component storage loans; native Playable
    // end preflights still own the original root/arranging loan disposition.
    bool CloseOriginalComponentsAfterPhysicalRelease(VoicePool& receiver,
        TimelineArmToken originalArm) noexcept {
        if (voices_ != &receiver || !HasOriginalPlayableRetention() ||
            originalArm.slot != arm_.slot || originalArm.generation != arm_.generation ||
            originalArm.renderSlot != arm_.renderSlot || originalArm.renderGeneration != arm_.renderGeneration ||
            !originalPhysicalRelease_.Valid() || !receiver.ReleaseObserved(originalPhysicalRelease_)) return false;
        if (componentsClosed_) return true;
        if (!Valid()) return false;
        ledger_.Release();
        occurrence_.Release();
        decision_.Release();
        body_.Release();
        source_.Release();
        componentsClosed_ = true;
        return true;
    }
    OriginalSourceSubmissionRegistry* sources_ = nullptr;
    VoicePool* voices_ = nullptr;
    TimelineArmTable* arms_ = nullptr;
    DecisionTable* decisions_ = nullptr;
    OccurrenceStatePool* occurrences_ = nullptr;
    VoiceLedger* logicalVoices_ = nullptr;
    // The component-only overload retains historical association only. The
    // Engine overload also borrows actual original root/arranging storage.
    PlayableInstanceTable* instances_ = nullptr;
    PlayableInstanceHandle arrangingInstance_ = 0;
    VoiceLedger::PreparedCommitInput logical_{};
    TimelineArmToken arm_{};
    VnVoiceHandle sourceIdentity_ = 0;
    VoiceReleaseObserver originalPhysicalRelease_{};
    CompletionProgressRow* progress_ = nullptr;
    std::uint64_t scheduleEpoch_ = 0;
    SourcePin source_;
    BodyPin body_;
    OriginalDecisionLifetimePin decision_;
    OccurrencePin occurrence_;
    LedgerPin ledger_;
    PlayableInstanceTable::OriginalLifetimePin rootOriginal_;
    PlayableInstanceTable::OriginalLifetimePin arrangingOriginal_;
    bool playableRetention_ = false;
    bool componentsClosed_ = false;
};

} // namespace harken
#endif // HARKEN_API_PREPARED_SINGLE_CURRENT_ORIGIN_BUNDLE_H
