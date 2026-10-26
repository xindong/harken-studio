#ifndef HARKEN_RENDER_ORIGINAL_SOURCE_SUBMISSION_H
#define HARKEN_RENDER_ORIGINAL_SOURCE_SUBMISSION_H

#include "harken/core/CommandQueue.h"
#include "harken/core/CheckedIssuer.h"
#include "harken/core/LogicAllocCheckpoint.h"
#include "harken/core/StableSlotStorage.h"
#include "harken/core/TimelineProgram.h"
#include "harken/render/VoicePool.h"

#include <array>
#include <cstring>
#include <memory>
#include <span>
#include <utility>

namespace harken {

class MixerLoop;
struct Engine;
namespace detail { struct OriginalSourceMutationActorTestAccess; }

// Actual preparation, immutable queue publication and complete disposition
// share one stable record. Engine quiescence precedes registry destruction.
class OriginalSourceSubmissionRegistry {
    friend class VoicePool;
    friend struct Engine;
public:
    static constexpr std::uint32_t kMaxCommands = 3u + kTimelineMaxVoiceSends +
        kTimelineMaxVoiceEffects + 2u + kMaxVoiceBindingInstanceSlots;
    enum class Phase : std::uint8_t {
        Free, Preparing, QueuePrepared, Published, ConsumerOwned, CleanupPending,
        Disposed, EngineDestroyed
    };
    enum class Disposition : std::uint8_t { Apply, Cancel, Invalid };
    class Record;
    using OriginalSourceLifetimePin = harken::OriginalSourceLifetimePin;
    class Record {
        friend class OriginalSourceSubmissionRegistry;
        friend class MixerLoop;
        friend struct detail::OriginalSourceMutationActorTestAccess;
    public:
        Phase State() const noexcept { return phase_.load(std::memory_order_acquire); }
        TimelineArmToken Token() const noexcept { return token_; }
    private:
        std::atomic<Phase> phase_{Phase::Free};
        TimelineArmToken token_{};
        std::atomic<std::uint64_t> originalLifetime_{0};
        std::uint64_t originalMarker_ = 0;
        VnVoiceHandle originalSourceIdentity_ = 0;
        std::atomic<bool> originalInstallationCompleted_{false};
        // This word describes this original Source episode only. No link or
        // selector exists until a later genuine full Current/Catalog factory.
        std::atomic<std::uint64_t> originWord_{0};
        std::atomic<bool> originInstallationStanding_{false};
        std::atomic<bool> originCallbackHeld_{false};
        OriginalSourceMutationActor rangeActor_;
        OriginalCatalogStageBaselineLoan catalogOrigin_;
        TimelineArmTable::OriginalArmRowOwner arm_;
        VoicePool::OriginalVoiceOwner voice_;
        CommandQueue::OriginalBatchRange range_;
        // InitialSource lends this fixed prefix even when complete backing is prepared later.
        std::array<CommandRecord, 3> bodyPrefix_{};
        using CommandTail = std::array<CommandRecord, kMaxCommands - 3>;
        std::unique_ptr<CommandTail> bodyTail_;
        bool retainCommandBacking_ = true;
        CommandRecord& BodyAt(std::uint32_t index) noexcept {
            assert(index < kMaxCommands);
            if (index < bodyPrefix_.size()) return bodyPrefix_[index];
            assert(bodyTail_ != nullptr);
            return (*bodyTail_)[index - bodyPrefix_.size()];
        }
        const CommandRecord& BodyAt(std::uint32_t index) const noexcept {
            assert(index < kMaxCommands);
            if (index < bodyPrefix_.size()) return bodyPrefix_[index];
            assert(bodyTail_ != nullptr);
            return (*bodyTail_)[index - bodyPrefix_.size()];
        }
        std::uint32_t count_ = 0;
        std::array<EffectInstanceRef, kTimelineMaxVoiceEffects> incoming_{};
        std::array<bool, kTimelineMaxVoiceEffects> incomingDone_{};
        std::uint32_t incomingCount_ = 0;
        TimelineArmTable::SourceCommandRowProof rowProof_;
        TimelineArmTable::PublicationAfterInstallation installPublication_;
        TimelineArmTable::GroupedPublicationAfterInstallation groupInstallPublication_;
        VoicePool::SourceCommandInstallProof installProof_;
        std::uint64_t completion_ = 0;
        std::uint32_t progress_ = 0;
        Disposition disposition_ = Disposition::Apply;
        bool protocolReported_ = false;
        bool wasPublished_ = false;
        bool unpublishedVoiceDispositionComplete_ = false;
        std::uint64_t nativeSourceEpisode_ = 0;
        TimelineArmTable::OriginalUnpublishedSourceDisposition unpublishedDisposition_;
    };

    OriginalSourceSubmissionRegistry(TimelineArmTable& arms, VoicePool& voices,
        CommandQueue& commands, EffectPoolBank& effects)
        : arms_(arms), voices_(voices), commands_(commands), effects_(effects),
          capacity_(arms.Capacity()), incarnation_(NextIncarnation()) {
        if (!rows_.Reserve(capacity_)) throw std::bad_alloc{};
    }
    ~OriginalSourceSubmissionRegistry() {
        for (auto& row : rows_) {
            assert((row.originalLifetime_.load(std::memory_order_relaxed) & kPinMask) == 0);
            assert(OriginCount(row.originWord_.load(std::memory_order_relaxed)) == 0);
            assert(!row.rangeActor_.Valid());
            assert(!row.catalogOrigin_.Valid());
        }
    }
    OriginalSourceSubmissionRegistry(const OriginalSourceSubmissionRegistry&) = delete;
    OriginalSourceSubmissionRegistry& operator=(const OriginalSourceSubmissionRegistry&) = delete;

    std::size_t StorageBytes() const noexcept {
        return rows_.RequestedBytes() + rows_.DirectoryBytes() +
            allocatedCommandTails_.load(std::memory_order_relaxed) * sizeof(Record::CommandTail);
    }
    bool PrepareSlotBacking(std::uint32_t slot) noexcept {
        if (!rows_.Emplace(slot) || !EnsureCommandBacking(rows_[slot])) return false;
        rows_[slot].retainCommandBacking_ = true;
        return true;
    }
    void SetCommitPageLimitForTests(std::size_t pages) noexcept { rows_.SetCommitPageLimitForTests(pages); }

    bool CapturePreparedSource(const CommandRecord& header,
        const TimelineArmTable::PreparedCommitProof& armProof,
        const VoicePool::PreparedCommitProof& voiceProof,
        std::span<const EffectPoolBank::PreparedCommitProof> effectProofs,
        OriginalSourceLifetimePin* original = nullptr,
        VoicePool::OriginalSourceBodyLifetimePin* originalBody = nullptr) noexcept {
        return CapturePreparedSourceImpl(header, armProof, voiceProof, effectProofs,
            original, originalBody, false);
    }

    bool CapturePreparedFinalSource(const CommandRecord& header,
        const TimelineArmTable::PreparedCommitProof& armProof,
        const VoicePool::PreparedCommitProof& voiceProof,
        std::span<const EffectPoolBank::PreparedCommitProof> effectProofs,
        OriginalSourceLifetimePin* original = nullptr,
        VoicePool::OriginalSourceBodyLifetimePin* originalBody = nullptr) noexcept {
        return CapturePreparedSourceImpl(header, armProof, voiceProof, effectProofs,
            original, originalBody, true);
    }

private:
    bool PrepareRecordBacking(std::uint32_t slot) noexcept {
        if (rows_.IsConstructed(slot)) return true;
        if (!rows_.Emplace(slot)) return false;
        rows_[slot].retainCommandBacking_ = false;
        return true;
    }
    bool EnsureCommandBacking(Record& row) noexcept {
        if (row.bodyTail_ != nullptr) return true;
        if (memguard::IsAudioTagged()) return false;
        try {
            detail::LogicAllocCheckpoint(detail::LogicAllocSite::SourceCommandBacking);
            row.bodyTail_ = std::make_unique<Record::CommandTail>();
        } catch (const std::bad_alloc&) { return false; }
        allocatedCommandTails_.fetch_add(1u, std::memory_order_relaxed);
        return true;
    }

    bool CapturePreparedSourceImpl(const CommandRecord& header,
        const TimelineArmTable::PreparedCommitProof& armProof,
        const VoicePool::PreparedCommitProof& voiceProof,
        std::span<const EffectPoolBank::PreparedCommitProof> effectProofs,
        OriginalSourceLifetimePin* original,
        VoicePool::OriginalSourceBodyLifetimePin* originalBody,
        bool publishOnInstallation) noexcept {
        const auto token = armProof.OriginalPreparedToken();
        const auto render = voiceProof.OriginalPreparedHandle();
        if (armProof.Owner() != &arms_ || !token.Valid() || token.slot >= capacity_ ||
            voiceProof.Owner() != &voices_ ||
            render == 0 || render != RenderOf(token) || !HeaderMatches(header, token) ||
            effectProofs.size() > kTimelineMaxVoiceEffects) return false;
        if (publishOnInstallation && !arms_.FinalSourcePreparationEligible(armProof)) return false;
        if (original != nullptr && (original->Valid() || originalMarkerExhausted_ ||
            nextOriginalMarker_ == 0)) return false;
        // Both outputs belong to this actual Arm/Voice joint preparation.
        // No installation loan can be minted later from Find or a numeric handle.
        if (originalBody != nullptr && (original == nullptr ||
            header.payload.play.sourceIdentity == 0 ||
            !voices_.CanIssueOriginalSourceBody(voiceProof, originalBody))) return false;
        std::array<EffectInstanceRef, kTimelineMaxVoiceEffects> incoming{};
        for (std::size_t i = 0; i < effectProofs.size(); ++i) {
            if (!effects_.CaptureOriginalPreparedEffect(effectProofs[i], &incoming[i])) return false;
            for (std::size_t j = 0; j < i; ++j)
                if (SameEffect(incoming[i], incoming[j])) return false;
        }
        if (!rows_.Emplace(token.slot)) return false;
        auto& row = rows_[token.slot];
        const auto phase = row.State();
        if (phase == Phase::Preparing) {
            // Retrying preparation cannot manufacture another original issuer.
            if (original != nullptr || publishOnInstallation || row.installPublication_.Valid()) return false;
            if (!SameToken(row.token_, token) || !row.arm_.Valid() || !row.voice_.Valid() ||
                !voices_.MatchesOriginalPreparedVoice(row.voice_, voiceProof) ||
                row.incomingCount_ != effectProofs.size()) return false;
            for (std::size_t i = 0; i < effectProofs.size(); ++i)
                if (!SameEffect(row.incoming_[i], incoming[i])) return false;
            return arms_.BindOriginalSourceSubmission(armProof, row.arm_, &row);
        }
        if (phase != Phase::Free && phase != Phase::Disposed) return false;
        if (nativeSourceEpisodeExhausted_ || nextNativeSourceEpisode_ == 0) return false;
        const auto lifetime = row.originalLifetime_.load(std::memory_order_acquire);
        if ((lifetime & kPinMask) != 0 ||
            (phase == Phase::Disposed && (lifetime & kOwnersReleased) == 0)) return false;
        if (!EnsureCommandBacking(row)) return false;
        if (!row.range_.ResetAfterDisposition()) return false;
        row.token_ = token;
        row.originalLifetime_.store(0, std::memory_order_relaxed);
        row.originalMarker_ = 0;
        row.nativeSourceEpisode_ = 0;
        row.unpublishedDisposition_.Reset();
        row.installPublication_.Reset();
        row.groupInstallPublication_.Reset();
        row.originalSourceIdentity_ = header.payload.play.sourceIdentity;
        row.originalInstallationCompleted_.store(false, std::memory_order_relaxed);
        assert(!row.rangeActor_.Valid() && !row.originCallbackHeld_.load(std::memory_order_relaxed));
        assert(!row.catalogOrigin_.Valid());
        row.originWord_.store(0, std::memory_order_relaxed);
        row.originInstallationStanding_.store(false, std::memory_order_relaxed);
        row.count_ = 0;
        row.incomingCount_ = static_cast<std::uint32_t>(effectProofs.size());
        row.incoming_ = incoming;
        row.incomingDone_.fill(false);
        row.progress_ = 0;
        row.completion_ = 0;
        row.protocolReported_ = false;
        row.wasPublished_ = false;
        row.unpublishedVoiceDispositionComplete_ = false;
        row.phase_.store(Phase::Preparing, std::memory_order_relaxed);
        if (!arms_.IssueOriginalArmRowOwner(armProof, &row.arm_) ||
            !voices_.IssueOriginalVoiceOwner(voiceProof, &row.voice_) ||
            row.voice_.RenderHandle() != render ||
            !arms_.BindOriginalSourceSubmission(armProof, row.arm_, &row)) return false;
        const auto sourceEpisode = TryIssue(&nextNativeSourceEpisode_, &nativeSourceEpisodeExhausted_);
        assert(sourceEpisode.status == IssueStatus::Ok);
        row.nativeSourceEpisode_ = sourceEpisode.value;
        if (!arms_.BindOriginalSourceEpisode(armProof, row.arm_, &row, row.nativeSourceEpisode_)) return false;
        if (publishOnInstallation &&
            !arms_.IssuePublicationAfterInstallation(armProof, row.arm_, &row, &row.installPublication_))
            return false;
        if (original != nullptr) {
            const auto issued = TryIssue(&nextOriginalMarker_, &originalMarkerExhausted_);
            assert(issued.status == IssueStatus::Ok);
            row.originalMarker_ = issued.value;
            if (originalBody != nullptr) {
                // Register without an active actor or an extra standing loan.
                // The real frozen range and installation loan supply standing.
                row.originInstallationStanding_.store(true, std::memory_order_relaxed);
                row.originWord_.store(OriginPack(OriginMode::Normal, 0, 1),
                    std::memory_order_release);
            }
            row.originalLifetime_.store(originalBody != nullptr ? 2u : 1u, std::memory_order_release);
            original->owner_ = this;
            original->row_ = &row;
            original->token_ = token;
            original->marker_ = issued.value;
            if (originalBody != nullptr) {
                OriginalSourceInstallationLoan installation;
                installation.owner_ = this;
                installation.token_ = token;
                installation.marker_ = issued.value;
                voices_.IssueOriginalSourceBody(voiceProof, token,
                    row.originalSourceIdentity_, std::move(installation), originalBody);
            }
        }
        return true;
    }

public:

    // Every source child is validated before the irreversible outer commit.
    bool BindPreparedChild(const CommandQueue::PreparedBatchProof& batch,
        std::uint32_t offset, std::uint32_t count) noexcept {
        CommandRecord header{};
        if (!commands_.ReadPreparedAt(batch, offset, &header)) return false;
        const auto token = TokenOf(header);
        if (!rows_.IsConstructed(token.slot) || count == 0 || count > kMaxCommands ||
            !HeaderMatches(header, token) || header.payload.play.batch.batchCommandCount != count)
            return false;
        auto& row = rows_[token.slot];
        if (row.State() == Phase::QueuePrepared &&
            row.range_.State() == CommandQueue::OriginalRangeState::UnpublishedCancelled) {
            if (!row.range_.ResetAfterDisposition()) return false;
            row.phase_.store(Phase::Preparing, std::memory_order_release);
        }
        if (row.State() != Phase::Preparing || !SameToken(row.token_, token) ||
            !row.arm_.Valid() || !row.voice_.Valid() ||
            voices_.OriginalVoiceReleased(row.voice_)) return false;
        std::array<bool, kTimelineMaxVoiceEffects> seen{};
        for (std::uint32_t i = 0; i < count; ++i) {
            CommandRecord command{};
            if (!commands_.ReadPreparedAt(batch, offset + i, &command) ||
                (i != 0 && !MetadataAllowed(command))) return false;
            if (command.opcode == kOpcodeSetVoiceEffect) {
                const auto& effect = command.payload.setVoiceEffect;
                const EffectInstanceRef incoming{effect.effectType, effect.poolIndex, effect.poolGeneration};
                std::uint32_t owned = UINT32_MAX;
                for (std::uint32_t j = 0; j < row.incomingCount_; ++j)
                    if (SameEffect(incoming, row.incoming_[j])) owned = j;
                if (owned == UINT32_MAX || seen[owned]) return false;
                seen[owned] = true;
            }
            row.BodyAt(i) = command;
        }
        for (std::uint32_t i = 0; i < row.incomingCount_; ++i)
            if (!seen[i]) return false;
        const bool issued = batch.InitialSource()
            ? offset == 0 && count == 3 && commands_.IssueInitialSourceRange(batch,
                {row.nativeSourceEpisode_, token.slot, 1}, row.bodyPrefix_.data(), &row, &row.range_)
            : commands_.IssueOriginalBatchRange(batch, offset, count, &row, &row.range_);
        if (!issued)
            return false;
        row.count_ = count;
        row.phase_.store(Phase::QueuePrepared, std::memory_order_release);
        return true;
    }

    bool IsOriginalSourceLifetimePinFor(const OriginalSourceLifetimePin& original,
        TimelineArmToken token) const noexcept {
        return MatchesOriginal(original) && SameToken(original.token_, token);
    }

    // This prospective association is proved from actual held factories and
    // this native frozen unpublished range. Metadata alone cannot issue it.
    bool MatchesPreparedOriginalSourcePair(const OriginalSourceLifetimePin& original,
        const VoicePool::OriginalSourceBodyLifetimePin& originalBody,
        const TimelineArmTable::PreparedCommitProof& armPreparation,
        const VoicePool::PreparedCommitProof& voicePreparation,
        const VoicePool::PreparedCommitInput& physical,
        const CommandQueue::PreparedBatchProof& batch, TimelineArmToken arm,
        VnVoiceHandle sourceIdentity) const noexcept {
        const OriginalSourceInstallationLoan* pending = nullptr;
        return voices_.MatchesPreparedOriginalSourceBody(originalBody, voicePreparation, physical, &pending) &&
            MatchesPreparedInitialGroupSourcePair(original, originalBody, armPreparation, voicePreparation,
                physical, batch, arm, sourceIdentity);
    }

    bool MatchesPreparedInitialGroupSourcePair(const OriginalSourceLifetimePin& original,
        const VoicePool::OriginalSourceBodyLifetimePin& originalBody,
        const TimelineArmTable::PreparedCommitProof& armPreparation,
        const VoicePool::PreparedCommitProof& voicePreparation,
        const VoicePool::PreparedCommitInput& physical,
        const CommandQueue::PreparedBatchProof& batch, TimelineArmToken arm,
        VnVoiceHandle sourceIdentity) const noexcept {
        if (!MatchesOriginal(original) || !SameToken(original.token_, arm) ||
            armPreparation.Owner() != &arms_ || !armPreparation.Valid() ||
            !SameToken(armPreparation.OriginalPreparedToken(), arm) ||
            physical.renderHandle != RenderOf(arm) || sourceIdentity == 0) return false;
        const auto& row = *static_cast<const Record*>(original.row_);
        const auto* actualArm = arms_.Resolve(arm);
        const OriginalSourceInstallationLoan* pending = nullptr;
        return row.State() == Phase::QueuePrepared && !row.wasPublished_ &&
            row.arm_.Valid() && row.arm_.Owner() == &arms_ && SameToken(row.arm_.Token(), arm) &&
            row.voice_.Valid() && voices_.MatchesOriginalPreparedVoice(row.voice_, voicePreparation) &&
            actualArm != nullptr && actualArm->state.load(std::memory_order_acquire) == ArmState::Prepared &&
            actualArm->originalSourceSubmission == &row &&
            commands_.PreparedBatchContains(batch, row.range_) && row.count_ != 0 &&
            HeaderMatches(row.BodyAt(0), arm) && row.originalSourceIdentity_ == sourceIdentity &&
            row.BodyAt(0).payload.play.sourceIdentity == sourceIdentity &&
            voices_.IsOriginalSourceBodyLifetimePinFor(originalBody, arm, sourceIdentity) &&
            voices_.MatchesPreparedOriginalSourceStorage(originalBody, voicePreparation, physical, &pending) &&
            pending != nullptr && pending->owner_ == this && pending->marker_ == original.marker_ &&
            MatchesInstallationForSource(*pending, &row, arm, sourceIdentity);
    }

    bool MatchesPrivateCommittedInitialGroupSourcePair(const OriginalSourceLifetimePin& original,
        const VoicePool::OriginalSourceBodyLifetimePin& body, const CommandQueue::PreparedBatchProof& batch,
        TimelineArmToken arm, VnVoiceHandle sourceIdentity) const noexcept {
        const auto* native = arms_.Resolve(arm);
        return MatchesUnpublishedCommittedInitialGroupSourceStorage(original, body, batch, arm, sourceIdentity) &&
            native != nullptr && !native->startBound;
    }

    bool MatchesEntryBoundInitialGroupSourcePair(const OriginalSourceLifetimePin& original,
        const VoicePool::OriginalSourceBodyLifetimePin& body, const CommandQueue::PreparedBatchProof& batch,
        TimelineArmToken arm, VnVoiceHandle sourceIdentity, std::uint64_t startFrame,
        const FrontierGroupBinding& binding, const AudioSliceLease& lease) const noexcept {
        const auto* native = arms_.Resolve(arm);
        return MatchesUnpublishedCommittedInitialGroupSourceStorage(original, body, batch, arm, sourceIdentity) &&
            native != nullptr && native->startBound && native->startFrame == startFrame &&
            native->armDeadline == startFrame && native->frontierAdmission == &binding &&
            native->frontierAdmissionLease == &lease;
    }

    bool CanReturnUnpublishedOriginalSourcePairStorage(const OriginalSourceLifetimePin& original,
        const VoicePool::OriginalSourceBodyLifetimePin& body) const noexcept {
        if (!MatchesOriginal(original)) return false;
        const auto& row = *static_cast<const Record*>(original.row_);
        const OriginalSourceInstallationLoan* installation = nullptr;
        // Only the holder and the body's installation association may remain.
        if (row.wasPublished_ || !CanWithdrawUnpublished(row.token_) ||
            !row.voice_.Valid() || !voices_.OriginalVoiceReleased(row.voice_) ||
            (row.originalLifetime_.load(std::memory_order_acquire) & kPinMask) != 2u ||
            !voices_.MatchesOriginalSourceRetirementPair(row.voice_, body) ||
            !voices_.IsOriginalSourceBodyLifetimePinFor(body, row.token_, row.originalSourceIdentity_) ||
            !voices_.CanReturnUnpublishedOriginalSourceBodyStorage(body, &installation) ||
            installation == nullptr) return false;
        // A real factory prefix can be cancelled before its commands are frozen.
        if (row.range_.State() == CommandQueue::OriginalRangeState::Empty &&
            row.range_.owner_ == nullptr && row.range_.client_ == nullptr &&
            row.range_.origin_ == 0 && row.range_.head_ == 0 && row.range_.count_ == 0 && row.count_ == 0)
            return MatchesInstallationLoan(*installation, &row, row.token_);
        return row.range_.State() == CommandQueue::OriginalRangeState::UnpublishedCancelled &&
            MatchesInstallationForSource(*installation, &row, row.token_, row.originalSourceIdentity_);
    }

    bool CanReturnPublishedOriginalSourcePairStorage(const OriginalSourceLifetimePin& original,
        const VoicePool::OriginalSourceBodyLifetimePin& body, VoiceReleaseObserver release) const noexcept {
        if (!MatchesOriginal(original)) return false;
        const auto& row = *static_cast<const Record*>(original.row_);
        const OriginalSourceInstallationLoan* installation = nullptr;
        return row.wasPublished_ && row.State() == Phase::Disposed && row.nativeSourceEpisode_ != 0 &&
            row.arm_.Valid() && row.arm_.Owner() == &arms_ && SameToken(row.arm_.Token(), row.token_) &&
            row.voice_.Valid() && row.voice_.RenderHandle() == RenderOf(row.token_) &&
            row.range_.State() == CommandQueue::OriginalRangeState::Completed &&
            row.range_.owner_ == &commands_ && row.range_.client_ == &row && row.range_.origin_ != 0 &&
            row.count_ != 0 && row.range_.FactCount() == row.count_ &&
            row.range_.IsSource() &&
            voices_.IsOriginalSourceBodyLifetimePinFor(body, row.token_, row.originalSourceIdentity_) &&
            voices_.CanReturnPublishedOriginalSourceBodyStorage(row.voice_, body, release, &installation) &&
            installation != nullptr &&
            MatchesInstallationForSource(*installation, &row, row.token_, row.originalSourceIdentity_);
    }

    // This checks this Source component's real installation, not Decision,
    // Occurrence, ledger ownership, Queue selection or Reset admission.
    bool IsOriginalInstalledSourceCurrent(const OriginalSourceLifetimePin& original,
        VnVoiceHandle sourceIdentity) const noexcept {
        if (!MatchesOriginal(original)) return false;
        const auto& row = *static_cast<const Record*>(original.row_);
        return row.originalInstallationCompleted_.load(std::memory_order_acquire) &&
            row.State() != Phase::EngineDestroyed && sourceIdentity != 0 &&
            row.originalSourceIdentity_ == sourceIdentity && row.arm_.Valid() &&
            row.voice_.Valid() && !voices_.OriginalVoiceReleased(row.voice_) &&
            voices_.Resolve(row.voice_.RenderHandle()) != nullptr;
    }

    bool IsOriginalSourceInstallationPending(const OriginalSourceLifetimePin& original,
        const VoicePool::OriginalSourceBodyLifetimePin& body, VnVoiceHandle sourceIdentity) const noexcept {
        if (!MatchesOriginal(original)) return false;
        const auto& row = *static_cast<const Record*>(original.row_);
        const auto phase = row.State();
        return row.wasPublished_ && (phase == Phase::Published || phase == Phase::ConsumerOwned) &&
            !row.originalInstallationCompleted_.load(std::memory_order_acquire) &&
            row.range_.State() == CommandQueue::OriginalRangeState::Published &&
            sourceIdentity != 0 && row.originalSourceIdentity_ == sourceIdentity &&
            row.arm_.Valid() && row.arm_.Owner() == &arms_ && SameToken(row.arm_.Token(), row.token_) &&
            row.voice_.Valid() && !voices_.OriginalVoiceReleased(row.voice_) &&
            voices_.Resolve(row.voice_.RenderHandle()) != nullptr &&
            voices_.MatchesOriginalSourceRetirementPair(row.voice_, body) &&
            voices_.IsOriginalSourceBodyLifetimePinFor(body, row.token_, sourceIdentity);
    }

    bool HasOriginalVoice(TimelineArmToken token) const noexcept {
        const auto* row = Find(token);
        return row != nullptr && row->voice_.Valid();
    }

    bool OriginalVoiceStillCurrent(TimelineArmToken token) const noexcept {
        const auto* row = Find(token);
        return row != nullptr && row->voice_.Valid() && !voices_.OriginalVoiceReleased(row->voice_);
    }

    enum class InstalledSourceRetirementRequest : std::uint8_t {
        Requested, AlreadyReleased, Deferred, Refused
    };
    class PendingInstalledSourceRetirementReceipt {
    public:
        PendingInstalledSourceRetirementReceipt() noexcept = default;
        PendingInstalledSourceRetirementReceipt(const PendingInstalledSourceRetirementReceipt&) = delete;
        PendingInstalledSourceRetirementReceipt& operator=(const PendingInstalledSourceRetirementReceipt&) = delete;
        PendingInstalledSourceRetirementReceipt(PendingInstalledSourceRetirementReceipt&& other) noexcept {
            *this = std::move(other);
        }
        PendingInstalledSourceRetirementReceipt& operator=(PendingInstalledSourceRetirementReceipt&& other) noexcept {
            if (this != &other) {
                Release();
                source_ = std::move(other.source_);
                body_ = std::move(other.body_);
                owner_ = std::exchange(other.owner_, nullptr);
                registryIncarnation_ = std::exchange(other.registryIncarnation_, 0);
                armIncarnation_ = std::exchange(other.armIncarnation_, 0);
                episode_ = std::exchange(other.episode_, 0);
                marker_ = std::exchange(other.marker_, 0);
                identity_ = std::exchange(other.identity_, 0);
                token_ = std::exchange(other.token_, TimelineArmToken{});
                release_ = std::exchange(other.release_, VoiceReleaseObserver{});
                terminal_ = std::exchange(other.terminal_, SourceTerminalToken{});
                terminalReturned_ = std::exchange(other.terminalReturned_, false);
            }
            return *this;
        }
        bool Valid() const noexcept { return (source_.Valid() && body_.Valid()) || owner_ != nullptr; }
        bool StorageReturned() const noexcept { return owner_ != nullptr; }
        void Release() noexcept {
            body_.Release(); source_.Release(); owner_ = nullptr;
            registryIncarnation_ = armIncarnation_ = episode_ = marker_ = identity_ = 0;
            token_ = {};
            release_ = {};
            terminal_ = {};
            terminalReturned_ = false;
        }
    private:
        friend class OriginalSourceSubmissionRegistry;
        OriginalSourceLifetimePin source_;
        VoicePool::OriginalSourceBodyLifetimePin body_;
        const OriginalSourceSubmissionRegistry* owner_ = nullptr;
        std::uint64_t registryIncarnation_ = 0;
        std::uint64_t armIncarnation_ = 0;
        std::uint64_t episode_ = 0;
        std::uint64_t marker_ = 0;
        VnVoiceHandle identity_ = 0;
        TimelineArmToken token_{};
        VoiceReleaseObserver release_{};
        SourceTerminalToken terminal_{};
        bool terminalReturned_ = false;
    };
private:
    friend struct Engine;
    bool PrepareEntryBoundGroupPublication(const OriginalSourceLifetimePin& source,
        const VoicePool::OriginalSourceBodyLifetimePin& body, const CommandQueue::PreparedBatchProof& batch,
        ArmGroupTable& groups, ArmGroupToken group, std::uint64_t frame,
        const FrontierGroupBinding& binding, AudioSliceLease& lease) noexcept {
        if (!MatchesOriginal(source) ||
            !MatchesEntryBoundInitialGroupSourcePair(source, body, batch, source.token_,
                static_cast<const Record*>(source.row_)->originalSourceIdentity_, frame, binding, lease)) return false;
        auto& row = *static_cast<Record*>(source.row_);
        if (row.installPublication_.Valid()) return false;
        return arms_.IssueGroupedPublicationAfterInstallation(row.arm_, &row, groups, group,
            binding, lease, &row.groupInstallPublication_);
    }
    bool PrepareQueueBoundGroupPublication(const OriginalSourceLifetimePin& source,
        const VoicePool::OriginalSourceBodyLifetimePin& body, const CommandQueue::PreparedBatchProof& batch,
        ArmGroupTable& groups, ArmGroupToken group, std::uint64_t frame,
        const FrontierGroupBinding& binding, AudioSliceLease& lease,
        const ScheduledTransactionPool& pool, const DecisionTable& decisions,
        const PrivatePendingQueueFrontierBinding& queue, const TimelineArmReaderPin& reader,
        const OriginalQueueFutureGroupRegistration& original, const OriginalQueueFutureGroupRegistration& outer,
        const ImmediateActionLane& lane, const PrivateImmediateLanePlacementProof& placement,
        std::uint32_t eventOrdinal) noexcept {
        if (!MatchesOriginal(source) ||
            !MatchesEntryBoundInitialGroupSourcePair(source, body, batch, source.token_,
                static_cast<const Record*>(source.row_)->originalSourceIdentity_, frame, binding, lease) ||
            !arms_.PrivatePendingQueueFrontierStartReady(reader, source.token_, binding, lease, pool, decisions,
                queue, original, outer, lane, placement, eventOrdinal)) return false;
        auto& row = *static_cast<Record*>(source.row_);
        if (row.installPublication_.Valid()) return false;
        return arms_.IssueQueueGroupedPublicationAfterInstallation(row.arm_, &row, groups, group,
            binding, lease, pool, decisions, queue, &row.groupInstallPublication_);
    }
    bool MatchesUnpublishedCommittedInitialGroupSourceStorage(const OriginalSourceLifetimePin& original,
        const VoicePool::OriginalSourceBodyLifetimePin& body, const CommandQueue::PreparedBatchProof& batch,
        TimelineArmToken arm, VnVoiceHandle sourceIdentity) const noexcept {
        if (!MatchesOriginal(original) || !SameToken(original.token_, arm) || sourceIdentity == 0) return false;
        const auto& row = *static_cast<const Record*>(original.row_);
        const auto* native = arms_.Resolve(arm);
        return row.State() == Phase::QueuePrepared && !row.wasPublished_ &&
            !row.originalInstallationCompleted_.load(std::memory_order_acquire) &&
            row.arm_.Valid() && row.arm_.Owner() == &arms_ && SameToken(row.arm_.Token(), arm) &&
            row.voice_.Valid() && row.voice_.RenderHandle() == RenderOf(arm) &&
            !voices_.OriginalVoiceReleased(row.voice_) && voices_.Resolve(row.voice_.RenderHandle()) != nullptr &&
            voices_.MatchesOriginalSourceRetirementPair(row.voice_, body) &&
            voices_.IsOriginalSourceBodyLifetimePinFor(body, arm, sourceIdentity) &&
            native != nullptr && native->state.load(std::memory_order_acquire) == ArmState::PendingApply &&
            native->pendingImmediateStart && native->originalSourceSubmission == &row &&
            native->startedBlockSequence.load(std::memory_order_acquire) == 0 &&
            native->tombstoneAudioSequence.load(std::memory_order_acquire) == 0 &&
            native->logicalCompletionSequence.load(std::memory_order_acquire) == 0 &&
            commands_.PreparedBatchContains(batch, row.range_) && row.count_ != 0 &&
            HeaderMatches(row.BodyAt(0), arm) && row.originalSourceIdentity_ == sourceIdentity &&
            row.BodyAt(0).payload.play.sourceIdentity == sourceIdentity;
    }
    // Only the original Engine producer may replace its unpublished children.
    InstalledSourceRetirementRequest RequestPendingInstalledSourceRetirement(
        const OriginalSourceLifetimePin& source,
        const VoicePool::OriginalSourceBodyLifetimePin& body) noexcept {
        using Result = InstalledSourceRetirementRequest;
        if (!MatchesOriginal(source)) return Result::Refused;
        auto& row = *static_cast<Record*>(source.row_);
        if (!row.voice_.Valid() || !row.arm_.Valid() ||
            !voices_.MatchesOriginalSourceRetirementPair(row.voice_, body) ||
            !voices_.IsOriginalSourceBodyLifetimePinFor(body, row.token_, row.originalSourceIdentity_))
            return Result::Refused;
        if (voices_.OriginalVoiceReleased(row.voice_)) return Result::AlreadyReleased;
        if (row.State() != Phase::Disposed || !row.wasPublished_ ||
            !row.originalInstallationCompleted_.load(std::memory_order_acquire)) return Result::Refused;
        const auto status = arms_.RequestOriginalPendingSourceRetirement(
            row.arm_, &row, row.nativeSourceEpisode_);
        if (status == TimelineArmTable::SourceCommandRowStatus::DeferredReservation) return Result::Deferred;
        if (status != TimelineArmTable::SourceCommandRowStatus::Ready) return Result::Refused;
        // The real Arm is cancelled before the audio request becomes visible.
        voices_.RequestOriginalSourceBodyRetirement(body);
        return Result::Requested;
    }

    InstalledSourceRetirementRequest RequestPendingInstalledSourceRetirementWithReceipt(
        const OriginalSourceLifetimePin& source,
        const VoicePool::OriginalSourceBodyLifetimePin& body,
        PendingInstalledSourceRetirementReceipt* out) noexcept {
        using Result = InstalledSourceRetirementRequest;
        if (out == nullptr || out->Valid() ||
            !MatchesOriginal(source)) return Result::Refused;
        const auto& row = *static_cast<const Record*>(source.row_);
        if (!IsOriginalInstalledSourceCurrent(source, row.originalSourceIdentity_)) return Result::Refused;
        PendingInstalledSourceRetirementReceipt prepared;
        if (!source.Borrow(&prepared.source_) || !body.Borrow(&prepared.body_)) return Result::Refused;
        if (!voices_.CaptureOriginalSourceRetirementIdentity(row.voice_, body,
                &prepared.release_, &prepared.terminal_)) return Result::Refused;
        const auto result = RequestPendingInstalledSourceRetirement(source, body);
        // AlreadyReleased alone does not prove an unplayed pending episode.
        if (result != Result::Requested) return result == Result::AlreadyReleased ? Result::Refused : result;
        *out = std::move(prepared);
        return Result::Requested;
    }

    bool MatchesPendingInstalledSourceRetirementReceipt(
        const PendingInstalledSourceRetirementReceipt& receipt, TimelineArmToken token) const noexcept {
        if (receipt.owner_ != nullptr)
            return receipt.owner_ == this && incarnation_ != 0 && receipt.registryIncarnation_ == incarnation_ &&
                receipt.armIncarnation_ == arms_.Incarnation() && receipt.episode_ != 0 &&
                receipt.marker_ != 0 && receipt.identity_ != 0 && SameToken(receipt.token_, token) &&
                !receipt.source_.Valid() && !receipt.body_.Valid();
        if (!MatchesOriginal(receipt.source_) || !SameToken(receipt.source_.token_, token)) return false;
        const auto& row = *static_cast<const Record*>(receipt.source_.row_);
        return row.arm_.Owner() == &arms_ && SameToken(row.arm_.Token(), token) &&
            row.voice_.Valid() && voices_.MatchesOriginalSourceRetirementPair(row.voice_, receipt.body_) &&
            voices_.IsOriginalSourceBodyLifetimePinFor(receipt.body_, token, row.originalSourceIdentity_);
    }

    bool PendingInstalledSourceRetirementSettled(
        const PendingInstalledSourceRetirementReceipt& receipt, TimelineArmToken token) const noexcept {
        if (!MatchesPendingInstalledSourceRetirementReceipt(receipt, token)) return false;
        if (receipt.owner_ != nullptr) return true;
        const auto& source = *static_cast<const Record*>(receipt.source_.row_);
        // Receipt loans retain storage; physical release never waits for them.
        return voices_.OriginalVoiceReleased(source.voice_) &&
            arms_.OriginalPendingSourceRetirementSettled(source.arm_, &source, source.nativeSourceEpisode_);
    }

    bool ReturnPendingInstalledSourceRetirementStorage(
        PendingInstalledSourceRetirementReceipt& receipt, TimelineArmToken token) noexcept {
        if (!PendingInstalledSourceRetirementSettled(receipt, token) || incarnation_ == 0) return false;
        if (receipt.owner_ != nullptr) return true;
        const auto& source = *static_cast<const Record*>(receipt.source_.row_);
        // Seal the actual terminal episode while its native owners still exist.
        // Later matches must never consult a reused Source, Body or Arm row.
        receipt.owner_ = this;
        receipt.registryIncarnation_ = incarnation_;
        receipt.armIncarnation_ = arms_.Incarnation();
        receipt.episode_ = source.nativeSourceEpisode_;
        receipt.marker_ = source.originalMarker_;
        receipt.identity_ = source.originalSourceIdentity_;
        receipt.token_ = source.token_;
        receipt.body_.Release();
        receipt.source_.Release();
        return true;
    }

    bool MatchesPendingInstalledSourceRetirementIdentity(
        const PendingInstalledSourceRetirementReceipt& receipt, TimelineArmToken arm,
        VnVoiceHandle identity, VoiceReleaseObserver release, SourceTerminalToken terminal) const noexcept {
        if (!PendingInstalledSourceRetirementSettled(receipt, arm) || identity == 0 ||
            !release.Valid() || !terminal.Valid() || receipt.release_.slot != release.slot ||
            receipt.release_.targetSequence != release.targetSequence || receipt.terminal_.slot != terminal.slot ||
            receipt.terminal_.generation != terminal.generation) return false;
        return receipt.owner_ != nullptr ? receipt.identity_ == identity
            : static_cast<const Record*>(receipt.source_.row_)->originalSourceIdentity_ == identity;
    }

    bool CapturePendingInstalledSourceRetirementTerminal(
        const PendingInstalledSourceRetirementReceipt& receipt, TimelineArmToken arm,
        VnVoiceHandle identity, VoiceReleaseObserver release, SourceTerminalToken* out) const noexcept {
        if (out == nullptr || !MatchesPendingInstalledSourceRetirementIdentity(
                receipt, arm, identity, release, receipt.terminal_)) return false;
        *out = receipt.terminal_;
        return true;
    }

    void ObservePendingInstalledSourceTerminalReturn(PendingInstalledSourceRetirementReceipt& receipt,
        TimelineArmToken arm, SourceTerminalToken terminal, const SourceTerminalRecord& source) noexcept {
        if (!MatchesPendingInstalledSourceRetirementReceipt(receipt, arm) ||
            receipt.terminal_.slot != terminal.slot || receipt.terminal_.generation != terminal.generation ||
            source.render != RenderOf(arm) || source.release.slot != receipt.release_.slot ||
            source.release.targetSequence != receipt.release_.targetSequence || source.occupied ||
            !source.releaseSeen || !voices_.ReleaseObserved(source.release)) return;
        receipt.terminalReturned_ = true;
    }

    bool PendingInstalledSourceTerminalReturned(const PendingInstalledSourceRetirementReceipt& receipt,
        TimelineArmToken arm) const noexcept {
        return PendingInstalledSourceRetirementSettled(receipt, arm) && receipt.terminalReturned_;
    }

public:
    bool DisposeUnpublishedVoice(TimelineArmToken token) noexcept {
        auto* row = Find(token);
        if (row == nullptr || !CanWithdrawUnpublished(token)) return false;
        if (row->unpublishedVoiceDispositionComplete_) return true;
        if (!row->voice_.Valid()) return false;
        if (voices_.OriginalVoiceReleased(row->voice_) ||
            voices_.CancelOriginalPreparedVoice(row->voice_)) {
            row->unpublishedVoiceDispositionComplete_ = true;
            return true;
        }
        const auto result = voices_.TryDisposeOriginalTimelineVoice(row->voice_);
        row->unpublishedVoiceDispositionComplete_ = result == VoicePool::OriginalVoiceDisposal::Released ||
            result == VoicePool::OriginalVoiceDisposal::AlreadyReleased;
        return row->unpublishedVoiceDispositionComplete_;
    }

    bool WithdrawUnpublished(TimelineArmToken token) noexcept {
        auto* row = Find(token);
        if (row == nullptr) return true;
        const auto phase = row->State();
        if (phase == Phase::Free || phase == Phase::Disposed) return true;
        const auto range = row->range_.State();
        if ((phase != Phase::Preparing && phase != Phase::QueuePrepared) ||
            (range != CommandQueue::OriginalRangeState::Empty &&
             range != CommandQueue::OriginalRangeState::UnpublishedCancelled) ||
            (row->voice_.Valid() && !voices_.OriginalVoiceReleased(row->voice_))) return false;
        if (row->voice_.Valid()) row->unpublishedVoiceDispositionComplete_ = true;
        // Missing/Disposed rows and incomplete factory prefixes cannot mint an
        // output. The genuine unbound branch and the actual cancelled range
        // both require the original physical Voice to have really returned.
        const bool genuineUnbound = range == CommandQueue::OriginalRangeState::Empty &&
            row->range_.owner_ == nullptr && row->range_.client_ == nullptr &&
            row->range_.origin_ == 0 && row->range_.head_ == 0 && row->range_.count_ == 0 && row->count_ == 0;
        const bool genuineCancelled = range == CommandQueue::OriginalRangeState::UnpublishedCancelled &&
            row->range_.owner_ == &commands_ && row->range_.client_ == row && row->range_.origin_ != 0 &&
            row->range_.count_ != 0 && row->range_.FactCount() == row->count_ &&
            row->range_.IsSource();
        if (!row->wasPublished_ && row->nativeSourceEpisode_ != 0 && row->originalSourceIdentity_ != 0 &&
            row->arm_.Valid() && row->voice_.Valid() && row->unpublishedVoiceDispositionComplete_ &&
            voices_.OriginalVoiceReleased(row->voice_) && (genuineUnbound || genuineCancelled)) {
            const bool issued = arms_.IssueOriginalUnpublishedSourceDisposition(row->arm_, row,
                row->nativeSourceEpisode_, row->originalMarker_, row->originalSourceIdentity_,
                &row->unpublishedDisposition_);
            assert(issued);
            if (issued) {
                auto& disposition = row->unpublishedDisposition_;
                disposition.commandQueue_ = &commands_; disposition.originalRange_ = &row->range_;
                disposition.rangeOrigin_ = row->range_.origin_; disposition.rangeHead_ = row->range_.head_;
                disposition.rangeCount_ = row->range_.count_; disposition.unboundOriginal_ = genuineUnbound;
            }
        }
        row->phase_.store(Phase::Disposed, std::memory_order_release);
        CompleteOriginalDisposition(*row);
        return true;
    }

    const TimelineArmTable::OriginalUnpublishedSourceDisposition*
    OriginalUnpublishedDisposition(TimelineArmToken token) const noexcept {
        const auto* row = Find(token);
        return row != nullptr && row->State() == Phase::Disposed &&
            row->unpublishedDisposition_.Valid() ? &row->unpublishedDisposition_ : nullptr;
    }

    bool CanWithdrawUnpublished(TimelineArmToken token) const noexcept {
        const auto* row = Find(token);
        if (row != nullptr && row->wasPublished_) return false;
        if (row == nullptr || row->State() == Phase::Disposed || row->State() == Phase::Free)
            return true;
        const auto state = row->range_.State();
        return (row->State() == Phase::Preparing || row->State() == Phase::QueuePrepared) &&
            (state == CommandQueue::OriginalRangeState::Empty ||
             state == CommandQueue::OriginalRangeState::UnpublishedCancelled);
    }

    bool CancelBoundPreparation(TimelineArmToken token,
        CommandQueue::PreparedBatchProof& batch) noexcept {
        auto* row = Find(token);
        if (row == nullptr) return true;
        if (row->range_.State() == CommandQueue::OriginalRangeState::Prepared) {
            if (!commands_.PreparedBatchContains(batch, row->range_)) return false;
            batch.Release();
        }
        return CanWithdrawUnpublished(token);
    }

    void PublishPreparedChild(const CommandRecord& header) noexcept {
        auto* row = Find(TokenOf(header));
        assert(row != nullptr && row->State() == Phase::QueuePrepared &&
            row->range_.State() == CommandQueue::OriginalRangeState::Prepared);
        // Only this native range proves no consumer borrows the complete tail.
        if (!memguard::IsAudioTagged() && !row->retainCommandBacking_ &&
            !arms_.rows_.HasPreparedBacking() &&
            row->range_.Kind() == CommandQueue::OriginalRangeKind::InitialSource &&
            row->count_ == 3 && row->bodyTail_ != nullptr) {
            row->bodyTail_.reset();
            allocatedCommandTails_.fetch_sub(1u, std::memory_order_relaxed);
        }
        row->wasPublished_ = true;
        row->phase_.store(Phase::Published, std::memory_order_release);
    }

    Record* Find(TimelineArmToken token) noexcept {
        return rows_.IsConstructed(token.slot) && SameToken(rows_[token.slot].token_, token)
            ? &rows_[token.slot] : nullptr;
    }
    const Record* Find(TimelineArmToken token) const noexcept {
        return rows_.IsConstructed(token.slot) && SameToken(rows_[token.slot].token_, token)
            ? &rows_[token.slot] : nullptr;
    }

    // Called only after callback admission is revoked and all workers joined.
    // EngineDestroyed withdraws lifetime loans; it does not invent a release receipt.
    void ShutdownAfterQuiescence() noexcept {
        pending_ = nullptr;
        commands_.DiscardPublishedAfterQuiescence();
        for (auto& row : rows_) {
            // The actual worker/callback join precedes this final return.
            // Even a Disposed range may have retained a deferred actor.
            row.rangeActor_.Release();
            TryCloseOrigin(row);
            if (row.State() == Phase::Free || row.State() == Phase::Disposed ||
                row.State() == Phase::EngineDestroyed) continue;
            row.installProof_.Release();
            row.rowProof_.Release();
            if (row.voice_.Valid() && !voices_.OriginalVoiceReleased(row.voice_)) {
                if (!voices_.CancelPreparedVoice(row.voice_.RenderHandle()))
                    (void)voices_.TryDisposeOriginalTimelineVoice(row.voice_);
            }
            for (std::uint32_t effect = 0; effect < row.incomingCount_; ++effect) {
                if (row.incomingDone_[effect]) continue;
                if (!effects_.CancelPrepared(row.incoming_[effect]))
                    effects_.Recycle(row.incoming_[effect]);
                row.incomingDone_[effect] = true;
            }
            if (row.range_.State() == CommandQueue::OriginalRangeState::Published) {
                const bool complete = commands_.CompleteOriginalRange(row.range_);
                assert(complete);
                (void)complete;
            }
            // No consumer or producer can read the queue after engine join.
            // A queued range is consumed by the explicit shutdown queue tail.
            row.phase_.store(Phase::EngineDestroyed, std::memory_order_release);
            CompleteOriginalDisposition(row);
        }
    }

    static TimelineArmToken TokenOf(const CommandRecord& header) noexcept {
        const auto packed = header.payload.play.batch.armToken;
        const auto render = header.payload.play.handle;
        return {static_cast<std::uint32_t>(packed), static_cast<std::uint32_t>(packed >> 32),
            static_cast<std::uint32_t>(render), static_cast<std::uint32_t>(render >> 32)};
    }
    static VnVoiceHandle RenderOf(TimelineArmToken token) noexcept {
        return (static_cast<std::uint64_t>(token.renderGeneration) << 32) | token.renderSlot;
    }
    static bool SameToken(TimelineArmToken a, TimelineArmToken b) noexcept {
        return a.slot == b.slot && a.generation == b.generation &&
            a.renderSlot == b.renderSlot && a.renderGeneration == b.renderGeneration;
    }
    static bool SameEffect(EffectInstanceRef a, EffectInstanceRef b) noexcept {
        return a.type == b.type && a.poolIndex == b.poolIndex && a.generation == b.generation;
    }
    static bool MetadataAllowed(const CommandRecord& command) noexcept {
        if (command.flags != 0 &&
            !(command.opcode == kOpcodeSetVoiceVolume && command.flags == kCommandFlagVariableWeightSegment) &&
            !(command.opcode == kOpcodeSetVoiceEffectParam && command.flags == kCommandFlagPreparedCompressor))
            return false;
        switch (command.opcode) {
        case kOpcodeSetVoiceVolume: case kOpcodeSetVoicePitch: case kOpcodeSetVoiceFilter:
        case kOpcodeSetVoicePosition: case kOpcodeSetVoicePan: case kOpcodeSetVoiceSpatialScalar:
        case kOpcodeSetVoiceLowShelf: case kOpcodeSetVoiceEffectBypass: case kOpcodeSetVoiceEffect:
        case kOpcodeClearVoiceEffect: case kOpcodeSetVoiceEffectParam: case kOpcodeSetVoiceSend:
        case kOpcodeClearVoiceSend: case kOpcodeSetVoiceSendLevel: return true;
        default: return false;
        }
    }

private:
    bool RetireRequestedInstalledSource(const OriginalSourceInstallationLoan& installation,
        std::uint64_t blockIndex) noexcept {
        if (!rows_.IsConstructed(installation.token_.slot)) return false;
        auto& row = rows_[installation.token_.slot];
        if (!MatchesInstallationForSource(installation, &row, row.token_, row.originalSourceIdentity_) ||
            row.State() != Phase::Disposed || !row.voice_.Valid() || !row.arm_.Valid()) return false;
        if (arms_.RequestOriginalPendingSourceRetirement(row.arm_, &row, row.nativeSourceEpisode_) !=
            TimelineArmTable::SourceCommandRowStatus::Ready) return false;
        TimelineArmTable::SourceCommandRowProof cancellation;
        if (arms_.ProveOriginalSourceCancellation(row.arm_, &row, &cancellation) !=
            TimelineArmTable::SourceCommandRowStatus::Ready) return false;
        if (!voices_.CancelTimelineBindingFromAudio(row.token_.renderSlot, row.token_.renderGeneration))
            return false;
        TimelineArmTable::TombstoneOriginalSourceCommand(cancellation, blockIndex);
        return true;
    }
    friend struct Engine;
    friend class MixerLoop;
    friend class OriginalSourceInstallationLoan;
    friend class harken::OriginalSourceLifetimePin;
    friend class OriginalSourceMutationActor;
    friend struct detail::OriginalSourceMutationActorTestAccess;
    enum class OriginMode : std::uint8_t {
        Disabled = 0, Normal = 1, ExhaustedNormal = 3, Closed = 4,
        Redirect = 5, ExhaustedRedirect = 7
    };
    static constexpr std::uint64_t kOriginAbortBit = 2;
    static constexpr unsigned kOriginNativeAttempts = 8;
    static bool IsNormalOrigin(OriginMode mode) noexcept {
        return mode == OriginMode::Normal || mode == OriginMode::Redirect;
    }
    static bool IsExhaustedOrigin(OriginMode mode) noexcept {
        return mode == OriginMode::ExhaustedNormal || mode == OriginMode::ExhaustedRedirect;
    }
    static bool IsLiveOrigin(OriginMode mode) noexcept {
        return IsNormalOrigin(mode) || IsExhaustedOrigin(mode);
    }
    static constexpr std::uint64_t kOriginCountOne = UINT64_C(1) << 3;
    static constexpr std::uint64_t kOriginCountMask = UINT64_C(255) << 3;
    static constexpr std::uint64_t kOriginEpochMax = UINT64_MAX >> 11;
    static constexpr std::uint64_t kNativeActorReserve = 2;
    static OriginMode OriginState(std::uint64_t word) noexcept {
        return static_cast<OriginMode>(word & 7u);
    }
    static std::uint64_t OriginCount(std::uint64_t word) noexcept { return (word >> 3) & 255u; }
    static std::uint64_t OriginEpoch(std::uint64_t word) noexcept { return word >> 11; }
    static std::uint64_t OriginPack(OriginMode mode, std::uint64_t count,
        std::uint64_t epoch) noexcept {
        return (epoch << 11) | (count << 3) | static_cast<std::uint64_t>(mode);
    }
    // Only an actual native standing or already counted actor reaches this
    // operation. Its receiver loan prevents episode reuse; the active range,
    // installation standing, or real count prevents Closed during this RMW.
    static void ExhaustOrigin(Record& row) noexcept {
        const auto before = row.originWord_.fetch_or(kOriginAbortBit, std::memory_order_acq_rel);
        assert(IsLiveOrigin(OriginState(before)));
        if (OriginState(before) == OriginMode::Redirect ||
            OriginState(before) == OriginMode::ExhaustedRedirect) {
            const bool aborted = row.catalogOrigin_.AbortNativeOldCandidate();
            assert(aborted);
            (void)aborted;
        }
    }
    static void TryCloseOrigin(Record& row) noexcept {
        if (row.originInstallationStanding_.load(std::memory_order_acquire) ||
            (row.originalLifetime_.load(std::memory_order_acquire) & kDispositionComplete) == 0)
            return;
        auto word = row.originWord_.load(std::memory_order_acquire);
        if (IsExhaustedOrigin(OriginState(word)) &&
            (row.originalLifetime_.load(std::memory_order_acquire) & kPinMask) != 0) return;
        if (!IsLiveOrigin(OriginState(word)) || OriginCount(word) != 0) return;
        if (!row.originWord_.compare_exchange_strong(word,
            OriginPack(OriginMode::Closed, 0, OriginEpoch(word)),
            std::memory_order_acq_rel, std::memory_order_acquire)) return;
        // Only the CAS winner from an actual published Redirect owns this
        // alias. Normal close must not touch a prospective producer install or
        // a detached alias that the serialized producer is still localizing.
        if (OriginState(word) == OriginMode::Redirect ||
            OriginState(word) == OriginMode::ExhaustedRedirect) {
            OriginalCatalogStageBaselineLoan old(std::move(row.catalogOrigin_));
            old.Release();
        }
    }
    // Component primitive only. This private Engine port has no caller in this
    // slice. The genuine Engine provisional-family issuer must certify Queue,
    // Conditional, C0/Lane, Source/Current and Stage scope before reaching it.
    bool TryAttachOriginalCatalogOrigin(const OriginalSourceLifetimePin& original,
        const VoicePool::OriginalSourceBodyLifetimePin& originalBody,
        const OriginalCatalogStageBaselineLoan& actualStage,
        std::uint64_t expectedEpoch) noexcept {
        if (!MatchesOriginal(original) || !actualStage.Valid() || expectedEpoch == 0 ||
            expectedEpoch >= kOriginEpochMax) return false;
        auto& row = *static_cast<Record*>(original.row_);
        auto expected = OriginPack(OriginMode::Normal, 0, expectedEpoch);
        // A Normal observation precedes every plain link touch. Audio closing
        // from Normal cannot touch the producer's prospective/detached alias.
        if (row.originWord_.load(std::memory_order_acquire) != expected) return false;
        if (row.catalogOrigin_.Valid() || row.State() == Phase::Free ||
            row.State() == Phase::EngineDestroyed ||
            !row.originInstallationStanding_.load(std::memory_order_acquire) ||
            !voices_.IsOriginalSourceBodyLifetimePinFor(originalBody, row.token_,
                row.originalSourceIdentity_)) return false;
        OriginalCatalogStageBaselineLoan link;
        if (!actualStage.Borrow(&link)) return false;
        row.catalogOrigin_ = std::move(link);
        if (!row.originWord_.compare_exchange_strong(expected,
            OriginPack(OriginMode::Redirect, 0, expectedEpoch),
            std::memory_order_acq_rel, std::memory_order_acquire)) {
            OriginalCatalogStageBaselineLoan failed(std::move(row.catalogOrigin_));
            failed.Release();
            return false;
        }
        return true;
    }
    static void ReturnOriginalOriginRead(Record& row) noexcept {
        const auto previous = row.originWord_.fetch_sub(kOriginCountOne, std::memory_order_acq_rel);
        assert(OriginCount(previous) != 0 &&
            (OriginState(previous) == OriginMode::Redirect ||
             OriginState(previous) == OriginMode::ExhaustedRedirect));
        (void)previous;
        // The caller's real original Source cap still protects the Record.
        TryCloseOrigin(row);
    }
    bool TryDetachAbortedOriginalCatalogOrigin(const OriginalSourceLifetimePin& original) noexcept {
        if (!MatchesOriginal(original)) return false;
        auto& row = *static_cast<Record*>(original.row_);
        auto expected = row.originWord_.load(std::memory_order_acquire);
        if (OriginState(expected) != OriginMode::Redirect || OriginCount(expected) != 0)
            return false;
        const auto borrowed = expected + kOriginCountOne;
        if (!row.originWord_.compare_exchange_strong(expected, borrowed,
            std::memory_order_acq_rel, std::memory_order_acquire)) return false;
        // This exact count belongs to this real original-factory read. Native
        // Old actors may still enter count2; Close cannot move the alias here.
        if (!row.catalogOrigin_.AbortedForNativeDetach()) {
            ReturnOriginalOriginRead(row);
            return false;
        }
        expected = borrowed;
        if (!row.originWord_.compare_exchange_strong(expected,
            OriginPack(OriginMode::Normal, 0, OriginEpoch(borrowed)),
            std::memory_order_acq_rel, std::memory_order_acquire)) {
            ReturnOriginalOriginRead(row);
            return false;
        }
        // The successful same-word transition returns only our own count.
        // Normal close does not touch this now producer-owned alias.
        OriginalCatalogStageBaselineLoan old(std::move(row.catalogOrigin_));
        old.Release();
        TryCloseOrigin(row);
        return true;
    }
    static void RetainRedirectForActualActor(Record& row,
        OriginalSourceMutationActor& actor) noexcept {
        const auto mode = OriginState(row.originWord_.load(std::memory_order_acquire));
        if (mode != OriginMode::Redirect && mode != OriginMode::ExhaustedRedirect) return;
        if (!row.catalogOrigin_.BorrowNativeOldSibling(&actor.catalogSibling_)) {
            // Real Source lifetime plus origin standing/count protects this
            // alias. Stage structural exhaustion aborts New; Old still writes.
            const bool aborted = row.catalogOrigin_.AbortNativeOldCandidate();
            assert(aborted);
            (void)aborted;
            actor.catalogFallback_ = true;
        }
    }
    bool MatchesActor(const OriginalSourceMutationActor& actor) const noexcept {
        if ((actor.borrowedMode_ != static_cast<std::uint8_t>(OriginMode::Normal) &&
                actor.borrowedMode_ != static_cast<std::uint8_t>(OriginMode::ExhaustedNormal)) ||
            !MatchesOriginal(actor.lifetime_)) return false;
        const auto& row = *static_cast<const Record*>(actor.lifetime_.row_);
        const auto word = row.originWord_.load(std::memory_order_acquire);
        return actor.borrowedMode_ == static_cast<std::uint8_t>(OriginMode::ExhaustedNormal)
            ? IsExhaustedOrigin(OriginState(word))
            : IsLiveOrigin(OriginState(word)) && OriginCount(word) != 0;
    }
    bool AddOriginActivity(Record& row, bool native) noexcept {
        auto word = row.originWord_.load(std::memory_order_acquire);
        for (unsigned attempt = 0; attempt < kOriginNativeAttempts; ++attempt) {
            if (!IsLiveOrigin(OriginState(word))) return false;
            const auto limit = native ? 255u : 255u - kNativeActorReserve;
            if (OriginCount(word) >= limit) { ExhaustOrigin(row); return false; }
            if (row.originWord_.compare_exchange_strong(word, word + kOriginCountOne,
                std::memory_order_acq_rel, std::memory_order_acquire)) return true;
        }
        ExhaustOrigin(row);
        return false;
    }
    void ReturnActor(OriginalSourceMutationActor& actor) noexcept {
        assert(MatchesActor(actor));
        auto& row = *static_cast<Record*>(actor.lifetime_.row_);
        // Clear a Record-owned actor before its last native loan can publish
        // OwnersReleased. Producer reuse may start immediately at that publish.
        OriginalSourceLifetimePin lifetime(std::move(actor.lifetime_));
        OriginalCatalogOldSiblingLoan sibling(std::move(actor.catalogSibling_));
        actor.catalogFallback_ = false;
        // The real origin count remains held until the Catalog parent returns.
        sibling.Release();
        if (actor.borrowedMode_ == static_cast<std::uint8_t>(OriginMode::Normal)) {
            const auto previous = row.originWord_.fetch_sub(kOriginCountOne, std::memory_order_acq_rel);
            assert(OriginCount(previous) != 0 && IsLiveOrigin(OriginState(previous)));
        }
        if (actor.callbackStanding_)
            row.originCallbackHeld_.store(false, std::memory_order_release);
        actor.borrowedMode_ = 0;
        actor.wrote_ = false;
        actor.callbackStanding_ = false;
        TryCloseOrigin(row);
        // Last lifetime return can return the actual native Arm/Voice owners.
        // No record access follows that return.
        lifetime.Release();
    }
    bool BorrowActor(const OriginalSourceMutationActor& original,
        OriginalSourceMutationActor* out) noexcept {
        if (out == nullptr || out->Valid() || !MatchesActor(original)) return false;
        OriginalSourceLifetimePin lifetime;
        if (!BorrowOriginal(original.lifetime_, &lifetime)) return false;
        auto& row = *static_cast<Record*>(lifetime.row_);
        if (!AddOriginActivity(row, false)) return false;
        out->lifetime_ = std::move(lifetime);
        out->borrowedMode_ = static_cast<std::uint8_t>(OriginMode::Normal);
        out->wrote_ = false;
        out->callbackStanding_ = false;
        RetainRedirectForActualActor(row, *out);
        return true;
    }
    bool ActorBeforeFirstWrite(OriginalSourceMutationActor& actor) noexcept {
        if (!MatchesActor(actor)) return false;
        if (actor.wrote_) return true;
        auto& row = *static_cast<Record*>(actor.lifetime_.row_);
        if (actor.catalogSibling_.Valid()) {
            if (!actor.catalogSibling_.BeforeFirstNativeOldWrite()) return false;
        } else if (actor.catalogFallback_) {
            const bool aborted = row.catalogOrigin_.AbortNativeOldCandidate();
            assert(aborted);
            (void)aborted;
        }
        auto word = row.originWord_.load(std::memory_order_acquire);
        for (unsigned attempt = 0; attempt < kOriginNativeAttempts; ++attempt) {
            const auto mode = OriginState(word);
            if (IsExhaustedOrigin(mode)) { actor.wrote_ = true; return true; }
            if (!IsNormalOrigin(mode) || OriginCount(word) == 0) return false;
            const auto epoch = OriginEpoch(word);
            if (epoch == kOriginEpochMax) break;
            if (row.originWord_.compare_exchange_strong(word,
                OriginPack(mode, OriginCount(word), epoch + 1u),
                std::memory_order_acq_rel, std::memory_order_acquire)) {
                actor.wrote_ = true;
                return true;
            }
        }
        ExhaustOrigin(row);
        actor.wrote_ = true;
        return true;
    }
    bool IssueFrontRangeActor(Record& row) noexcept {
        const auto mode = OriginState(row.originWord_.load(std::memory_order_acquire));
        if (mode == OriginMode::Disabled) return true;
        if (row.rangeActor_.Valid()) return MatchesActor(row.rangeActor_);
        if (!commands_.IsOriginalSourceFront(row.range_) ||
            row.range_.Owner() != &commands_ || row.range_.Client() != &row ||
            !row.range_.IsSource() ||
            row.State() != Phase::Published || row.originalMarker_ == 0 ||
            !IsLiveOrigin(mode)) return false;
        auto lifetime = row.originalLifetime_.load(std::memory_order_acquire);
        for (;;) {
            // The exact original range can still stand when all external
            // Source loans and the cancelled installation loan have returned.
            if ((lifetime & (kDispositionComplete | kOwnersReleased)) != 0) return false;
            if ((lifetime & kPinMask) == kPinMask) { ExhaustOrigin(row); return false; }
            if (row.originalLifetime_.compare_exchange_weak(lifetime, lifetime + 1u,
                std::memory_order_acq_rel, std::memory_order_acquire)) break;
        }
        OriginalSourceLifetimePin pin;
        pin.owner_ = this; pin.row_ = &row; pin.token_ = row.token_; pin.marker_ = row.originalMarker_;
        const bool activity = AddOriginActivity(row, true);
        // Exhaustion revokes every future snapshot/admission possibility. It
        // does not revoke the actual already accepted native range operation.
        // Its authentic Source loan still protects the exhausted episode.
        row.rangeActor_.lifetime_ = std::move(pin);
        row.rangeActor_.borrowedMode_ = static_cast<std::uint8_t>(activity
            ? OriginMode::Normal : OriginMode::ExhaustedNormal);
        RetainRedirectForActualActor(row, row.rangeActor_);
        return true;
    }
    bool BorrowInstallationMutation(const OriginalSourceInstallationLoan& installation,
        OriginalSourceMutationActor* out) noexcept {
        const auto token = installation.token_;
        if (out == nullptr || out->Valid() || !rows_.IsConstructed(token.slot) ||
            !MatchesInstallationLoan(installation, &rows_[token.slot], token)) return false;
        auto& row = rows_[token.slot];
        const auto mode = OriginState(row.originWord_.load(std::memory_order_acquire));
        if (!IsLiveOrigin(mode) ||
            !row.originInstallationStanding_.load(std::memory_order_acquire)) return false;
        bool held = false;
        if (!row.originCallbackHeld_.compare_exchange_strong(held, true,
            std::memory_order_acq_rel, std::memory_order_acquire)) return false;
        OriginalSourceLifetimePin standing;
        standing.owner_ = this; standing.row_ = &row;
        standing.token_ = token; standing.marker_ = installation.marker_;
        // This local view is the existing installation loan, not a new loan.
        // Only the authentic private installation factory can form it.
        OriginalSourceLifetimePin lifetime;
        const bool borrowed = BorrowOriginal(standing, &lifetime, true);
        standing.owner_ = nullptr; standing.row_ = nullptr; standing.token_ = {}; standing.marker_ = 0;
        if (!borrowed) {
            row.originCallbackHeld_.store(false, std::memory_order_release);
            ExhaustOrigin(row);
            return false;
        }
        const bool activity = AddOriginActivity(row, true);
        out->lifetime_ = std::move(lifetime);
        out->borrowedMode_ = static_cast<std::uint8_t>(activity
            ? OriginMode::Normal : OriginMode::ExhaustedNormal);
        out->callbackStanding_ = true;
        out->wrote_ = false;
        RetainRedirectForActualActor(row, *out);
        return true;
    }
    static constexpr std::uint64_t kPinMask = UINT32_MAX;
    static constexpr std::uint64_t kDispositionComplete = UINT64_C(1) << 63;
    static constexpr std::uint64_t kOwnersReleased = UINT64_C(1) << 62;

    bool MatchesInstallationLoan(const OriginalSourceInstallationLoan& installation,
        const void* actualSubmission, TimelineArmToken token) const noexcept {
        if (installation.owner_ != this || !rows_.IsConstructed(token.slot) ||
            installation.marker_ == 0 || !SameToken(installation.token_, token)) return false;
        const auto& row = rows_[token.slot];
        return actualSubmission == &row && SameToken(row.token_, token) &&
            row.originalMarker_ == installation.marker_ &&
            (row.originalLifetime_.load(std::memory_order_acquire) & kPinMask) != 0;
    }
    bool MatchesInstallationForSource(const OriginalSourceInstallationLoan& installation,
        const void* actualSubmission, TimelineArmToken token, VnVoiceHandle sourceIdentity) const noexcept {
        if (!MatchesInstallationLoan(installation, actualSubmission, token)) return false;
        const auto& row = rows_[token.slot];
        // Actual frozen commands must agree with the early true factory identity.
        // Release remains a separate family-only operation for unbound prefixes.
        return sourceIdentity != 0 && row.originalSourceIdentity_ == sourceIdentity &&
            row.count_ != 0 && HeaderMatches(row.BodyAt(0), token) &&
            row.BodyAt(0).payload.play.sourceIdentity == sourceIdentity;
    }
    void ReleaseInstallationLoan(OriginalSourceInstallationLoan& installation) noexcept {
        const auto token = installation.token_;
        const bool matches = rows_.IsConstructed(token.slot) &&
            MatchesInstallationLoan(installation, &rows_[token.slot], token);
        assert(matches);
        if (!matches) return;
        auto& row = rows_[token.slot];
        // The Slot factory still owns its exact-once return claim. Clear its
        // installation capability before any Source reuse publication.
        installation.owner_ = nullptr;
        installation.token_ = {};
        installation.marker_ = 0;
        row.originInstallationStanding_.store(false, std::memory_order_release);
        TryCloseOrigin(row);
        const auto previous = row.originalLifetime_.fetch_sub(1u, std::memory_order_acq_rel);
        assert((previous & kPinMask) != 0 && (previous & kOwnersReleased) == 0);
        TryCloseOrigin(row);
        if ((previous & kPinMask) == 1u && (previous & kDispositionComplete) != 0)
            ReleaseOriginalOwners(row);
    }
    bool MatchesOriginal(const OriginalSourceLifetimePin& original) const noexcept {
        if (original.owner_ != this || original.row_ == nullptr ||
            !rows_.IsConstructed(original.token_.slot) || original.row_ != &rows_[original.token_.slot] ||
            original.marker_ == 0) return false;
        const auto& row = rows_[original.token_.slot];
        return row.originalMarker_ == original.marker_ && SameToken(row.token_, original.token_) &&
            (row.originalLifetime_.load(std::memory_order_acquire) & kPinMask) != 0;
    }
    bool BorrowOriginal(const OriginalSourceLifetimePin& original,
        OriginalSourceLifetimePin* out, bool native = false) noexcept {
        if (out == nullptr || out->Valid() || !MatchesOriginal(original)) return false;
        auto& row = *static_cast<Record*>(original.row_);
        auto lifetime = row.originalLifetime_.load(std::memory_order_acquire);
        for (;;) {
            const auto count = lifetime & kPinMask;
            const auto mode = OriginState(row.originWord_.load(std::memory_order_acquire));
            const auto limit = !native && IsLiveOrigin(mode) ? kPinMask - kNativeActorReserve : kPinMask;
            if (count == 0 || count >= limit || (lifetime & kOwnersReleased) != 0) return false;
            if (row.originalLifetime_.compare_exchange_weak(lifetime, lifetime + 1u,
                std::memory_order_acq_rel, std::memory_order_acquire)) break;
        }
        out->owner_ = this;
        out->row_ = &row;
        out->token_ = original.token_;
        out->marker_ = original.marker_;
        return true;
    }
    void ReleaseOriginal(OriginalSourceLifetimePin& original) noexcept {
        const bool matches = MatchesOriginal(original);
        assert(matches);
        if (!matches) return;
        auto& row = *static_cast<Record*>(original.row_);
        original.owner_ = nullptr;
        original.row_ = nullptr;
        original.token_ = {};
        original.marker_ = 0;
        const auto previous = row.originalLifetime_.fetch_sub(1u, std::memory_order_acq_rel);
        assert((previous & kPinMask) != 0 && (previous & kOwnersReleased) == 0);
        TryCloseOrigin(row);
        if ((previous & kPinMask) == 1u && (previous & kDispositionComplete) != 0)
            ReleaseOriginalOwners(row);
    }
    static void ReleaseOriginalOwners(Record& row) noexcept {
        row.voice_.Release();
        row.arm_.Release();
        // New native preparation may reuse the row only after both returns.
        row.originalLifetime_.fetch_or(kOwnersReleased, std::memory_order_release);
    }
    static void CompleteOriginalDisposition(Record& row) noexcept {
        row.installPublication_.Reset();
        row.groupInstallPublication_.Reset();
        const auto previous = row.originalLifetime_.fetch_or(kDispositionComplete,
            std::memory_order_acq_rel);
        TryCloseOrigin(row);
        if ((previous & (kDispositionComplete | kPinMask)) == 0) ReleaseOriginalOwners(row);
    }
    Record* BorrowFront() noexcept {
        auto* range = commands_.OriginalRangeAtFront();
        return BorrowRange(range);
    }
    Record* BorrowInitialFront(const CommandQueue::InitialSourceEntry& entry) noexcept {
        auto* row = BorrowRange(entry.range);
        return row != nullptr && row->range_.Kind() == CommandQueue::OriginalRangeKind::InitialSource &&
            entry.source == CommandQueue::InitialSourceIdentity{row->nativeSourceEpisode_, row->token_.slot, 1} &&
            entry.facts == row->bodyPrefix_.data() && row->count_ == 3 ? row : nullptr;
    }
    Record* BorrowRange(CommandQueue::OriginalBatchRange* range) noexcept {
        if (range == nullptr) return nullptr;
        const auto client = reinterpret_cast<std::uintptr_t>(range->Client());
        const auto begin = reinterpret_cast<std::uintptr_t>(rows_.BackingAddress());
        if (client < begin || client - begin >= sizeof(Record) * capacity_ ||
            (client - begin) % sizeof(Record) != 0) return nullptr;
        const auto slot = static_cast<std::uint32_t>((client - begin) / sizeof(Record));
        if (!rows_.IsConstructed(slot)) return nullptr;
        auto& row = rows_[slot];
        return &row.range_ == range && row.State() == Phase::Published ? &row : nullptr;
    }
    static bool HeaderMatches(const CommandRecord& header, TimelineArmToken token) noexcept {
        const auto& batch = header.payload.play.batch;
        return header.opcode == kOpcodePlay && header.flags == 0 &&
            batch.batchVersion == kPlayLedBatchVersion && batch.batchKind == PlayLedBatchKind::TimelineArm &&
            batch.reserved == 0 && batch.batchCommandCount != 0 && batch.batchCommandCount <= kMaxCommands &&
            SameToken(TokenOf(header), token);
    }
    static std::uint64_t NextIncarnation() noexcept {
        static std::atomic<std::uint64_t> next{1};
        auto current = next.load(std::memory_order_relaxed);
        while (current != 0) {
            const auto successor = current == UINT64_MAX ? 0 : current + 1;
            if (next.compare_exchange_weak(current, successor,
                    std::memory_order_relaxed, std::memory_order_relaxed)) return current;
        }
        return 0;
    }
    std::uint64_t nextNativeSourceEpisode_ = 1;
    bool nativeSourceEpisodeExhausted_ = false;
    TimelineArmTable& arms_;
    VoicePool& voices_;
    CommandQueue& commands_;
    EffectPoolBank& effects_;
    const std::uint32_t capacity_;
    StableSlotStorage<Record> rows_;
    std::atomic<std::size_t> allocatedCommandTails_{0};
    const std::uint64_t incarnation_;
    Record* pending_ = nullptr;
    std::uint64_t nextOriginalMarker_ = 1;
    bool originalMarkerExhausted_ = false;
};

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

} // namespace harken
#endif
