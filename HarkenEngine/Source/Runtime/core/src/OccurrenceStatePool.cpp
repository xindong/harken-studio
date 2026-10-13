#include "harken/core/OccurrenceStatePool.h"

#include <limits>
#include <cassert>
#include <utility>

namespace harken {
namespace {

// A maximum generation is exhausted rather than wrapped back to 1. Returning 0
// lets the release path mark the slot permanently unavailable.
std::uint32_t NextGeneration(std::uint32_t generation) {
    if (generation == (std::numeric_limits<std::uint32_t>::max)()) {
        return 0;
    }
    return generation + 1;
}

} // namespace

OccurrenceStatePool::OccurrenceStatePool(std::uint32_t capacity)
    : m_entries(capacity) {}

OccurrenceStatePool::~OccurrenceStatePool() {
    for (const auto& entry : m_entries)
        assert(entry.originalStoragePinCount == 0 && entry.originalSinglePinCount == 0 &&
               !entry.originalSinglePreparedCommitHeld);
}

OccurrenceStatePool::OriginalStoragePin::OriginalStoragePin(OriginalStoragePin&& other) noexcept {
    *this = std::move(other);
}
OccurrenceStatePool::OriginalStoragePin& OccurrenceStatePool::OriginalStoragePin::operator=(
    OriginalStoragePin&& other) noexcept {
    if (this != &other) {
        Release();
        owner_ = std::exchange(other.owner_, nullptr);
        entry_ = std::exchange(other.entry_, nullptr);
        token_ = std::exchange(other.token_, {});
        episode_ = std::exchange(other.episode_, 0);
        baseline_ = std::exchange(other.baseline_, false);
    }
    return *this;
}
bool OccurrenceStatePool::OriginalStoragePin::Valid() const noexcept {
    return owner_ != nullptr && owner_->MatchesOriginalStoragePin(*this);
}
bool OccurrenceStatePool::OriginalStoragePin::NativeEnded() const noexcept {
    return Valid() && entry_->originalCompositeStorageRetired;
}
bool OccurrenceStatePool::OriginalStoragePin::CaptureOriginalKey(OccurrenceKey* out) const noexcept {
    if (out == nullptr || !Valid()) return false;
    *out = entry_->key;
    return true;
}
bool OccurrenceStatePool::OriginalStoragePin::Borrow(OriginalStoragePin* out) const noexcept {
    if (out == nullptr || out->owner_ != nullptr || !Valid() ||
        entry_->originalStoragePinCount == UINT32_MAX) return false;
    ++entry_->originalStoragePinCount;
    out->owner_ = owner_;
    out->entry_ = entry_;
    out->token_ = token_;
    out->episode_ = episode_;
    out->baseline_ = false;
    return true;
}
void OccurrenceStatePool::OriginalStoragePin::Release() noexcept {
    if (owner_ == nullptr) return;
    assert(Valid());
    if (!Valid()) return;
    auto* pool = owner_;
    auto* entry = entry_;
    if (--entry->originalStoragePinCount == 0) {
        if (entry->originalCompositeStorageRetired && entry->originalSinglePinCount == 0 &&
            !entry->originalSinglePreparedCommitHeld) pool->RecycleOriginalStorage(*entry);
        else entry->originalStorageEpisode = 0;
    }
    owner_ = nullptr;
    entry_ = nullptr;
    token_ = {};
    episode_ = 0;
    baseline_ = false;
}
bool OccurrenceStatePool::MatchesOriginalStoragePin(const OriginalStoragePin& original) const noexcept {
    return original.owner_ == this && original.entry_ != nullptr && original.token_.generation != 0 &&
        original.token_.slot < m_entries.size() && original.entry_ == &m_entries[original.token_.slot] &&
        original.entry_->generation == original.token_.generation && original.episode_ != 0 &&
        original.entry_->originalStorageEpisode == original.episode_ &&
        original.entry_->originalStoragePinCount != 0;
}
bool OccurrenceStatePool::IsOriginalStoragePinFor(const OriginalStoragePin& original,
    OccurrenceToken token) const noexcept {
    return MatchesOriginalStoragePin(original) && original.token_.slot == token.slot &&
        original.token_.generation == token.generation;
}
bool OccurrenceStatePool::RegisterOriginalStorageBaselineResolver(void* context,
    OriginalStorageBaselineResolver resolver) noexcept {
    if (context == nullptr || resolver == nullptr || m_originalStorageBaselineResolver != nullptr) return false;
    for (const auto& entry : m_entries)
        if (entry.originalStoragePinCount != 0) return false;
    m_originalStorageBaselineContext = context;
    m_originalStorageBaselineResolver = resolver;
    return true;
}
OccurrenceStatePool::OriginalStoragePin* OccurrenceStatePool::ResolveOriginalStorageBaseline(
    OccurrenceToken token, OriginalStoragePin* explicitBaseline) noexcept {
    if (explicitBaseline != nullptr || m_originalStorageBaselineResolver == nullptr) return explicitBaseline;
    // Return an already issued native object, never a token-derived grant.
    return m_originalStorageBaselineResolver(m_originalStorageBaselineContext, *this, token);
}
bool OccurrenceStatePool::OriginalStorageBaselineMatches(const OriginalStoragePin* original,
    OccurrenceToken token) const noexcept {
    return original == nullptr || (original->baseline_ && IsOriginalStoragePinFor(*original, token));
}

void OccurrenceStatePool::ClearOriginalSingleMetadata(Entry& entry) noexcept {
    assert(entry.originalSinglePinCount == 0 && !entry.originalSinglePreparedCommitHeld);
    entry.originalSingleMarker = 0;
    entry.originalSingleOwner = {};
    entry.originalSingleRenderGeneration = 0;
    entry.originalSingleScheduleEpoch = 0;
}

OccurrenceStatePool::PreparedSingleCompletionCommitProof::PreparedSingleCompletionCommitProof(
    PreparedSingleCompletionCommitProof&& other) noexcept { *this = std::move(other); }
OccurrenceStatePool::PreparedSingleCompletionCommitProof&
OccurrenceStatePool::PreparedSingleCompletionCommitProof::operator=(
    PreparedSingleCompletionCommitProof&& other) noexcept {
    if (this != &other) {
        Release();
        owner_ = std::exchange(other.owner_, nullptr);
        entry_ = std::exchange(other.entry_, nullptr);
        token_ = std::exchange(other.token_, {});
        marker_ = std::exchange(other.marker_, 0);
        completionOwner_ = std::exchange(other.completionOwner_, {});
        progress_ = std::exchange(other.progress_, nullptr);
        renderGeneration_ = std::exchange(other.renderGeneration_, 0);
        scheduleEpoch_ = std::exchange(other.scheduleEpoch_, 0);
    }
    return *this;
}
void OccurrenceStatePool::PreparedSingleCompletionCommitProof::Release() noexcept {
    if (entry_ == nullptr) return;
    assert(owner_ != nullptr && owner_->FindMut(token_) == entry_ &&
        entry_->originalSinglePreparedCommitHeld);
    entry_->originalSinglePreparedCommitHeld = false;
    if (entry_->originalSinglePinCount == 0) ClearOriginalSingleMetadata(*entry_);
    owner_ = nullptr;
    entry_ = nullptr;
    token_ = {};
    marker_ = 0;
    completionOwner_ = {};
    progress_ = nullptr;
    renderGeneration_ = 0;
    scheduleEpoch_ = 0;
}

PlayableInstanceHandle OccurrenceStatePool::PreparedSingleCompletionCommitProof::OriginalArrangingInstance() const noexcept {
    return owner_ != nullptr && owner_->MatchesPreparedSingleProof(*this)
        ? entry_->key.arrangingInstance : 0;
}

bool OccurrenceStatePool::MatchesPreparedSingleProof(
    const PreparedSingleCompletionCommitProof& proof) const noexcept {
    if (proof.owner_ != this || proof.entry_ == nullptr || proof.marker_ == 0 ||
        Find(proof.token_) != proof.entry_) return false;
    const auto& entry = *proof.entry_;
    CompletionOwnerRef actualOwner{};
    CompletionProgressSnapshot progress{};
    return entry.originalSinglePreparedCommitHeld && entry.preparedReservation &&
        entry.stage == OccurrenceStage::Preparing && entry.completionProgressBound &&
        !entry.completionOwnerReleaseFenceArmed && !entry.completionOwnerReleaseTerminal &&
        entry.originalSingleMarker == proof.marker_ &&
        proof.progress_ == &entry.completionProgress && proof.renderGeneration_ != 0 &&
        proof.renderGeneration_ == entry.originalSingleRenderGeneration &&
        proof.scheduleEpoch_ != 0 && proof.scheduleEpoch_ == entry.scheduleEpoch &&
        proof.scheduleEpoch_ == entry.originalSingleScheduleEpoch &&
        SameCompletionOwner(proof.completionOwner_, entry.originalSingleOwner) &&
        entry.completionOwner.Capture(&actualOwner) &&
        SameCompletionOwner(actualOwner, proof.completionOwner_) &&
        CaptureCompletionProgressExact(entry.completionProgress, proof.token_,
            proof.renderGeneration_, &progress) &&
        progress.playbackFactsRevision == proof.completionOwner_.factsRevision;
}
bool OccurrenceStatePool::ProvePreparedSingleCompletionCommit(
    const OriginalSingleCompletionOwnerPin& original, const CompletionOwnerRef& owner,
    const CompletionProgressRow* originalProgress, std::uint32_t renderGeneration,
    std::uint64_t scheduleEpoch, PreparedSingleCompletionCommitProof* out) noexcept {
    return ProvePreparedCompletionCommitInternal(original, owner, originalProgress,
        renderGeneration, scheduleEpoch, out, false);
}
bool OccurrenceStatePool::ProvePreparedCompositeCompletionCommit(
    const OriginalCompositeCompletionOwnerPin& original, const CompletionOwnerRef& owner,
    const CompletionProgressRow* progress, std::uint32_t renderGeneration,
    std::uint64_t scheduleEpoch, PreparedCompositeCompletionCommitProof* out) noexcept {
    return out != nullptr && ProvePreparedCompletionCommitInternal(original.original_, owner,
        progress, renderGeneration, scheduleEpoch, &out->proof_, true);
}
bool OccurrenceStatePool::ProvePreparedCompletionCommitInternal(
    const OriginalSingleCompletionOwnerPin& original, const CompletionOwnerRef& owner,
    const CompletionProgressRow* originalProgress, std::uint32_t renderGeneration,
    std::uint64_t scheduleEpoch, PreparedSingleCompletionCommitProof* out, bool compositeFactory) noexcept {
    if (out == nullptr || out->Valid() || !MatchesOriginalSingleCompletionOwnerPin(original)) return false;
    const bool kindMatches = compositeFactory
        ? owner.kind == CompletionOwnerKind::Composite && owner.composite.Valid() && !owner.singleDecision.Valid()
        : owner.kind == CompletionOwnerKind::SingleQueue && owner.singleDecision.Valid() && !owner.composite.Valid();
    auto& entry = m_entries[original.token_.slot];
    CompletionOwnerRef actualOwner{};
    CompletionProgressSnapshot progress{};
    if (entry.originalSinglePreparedCommitHeld || !entry.preparedReservation ||
        entry.stage != OccurrenceStage::Preparing || !entry.completionProgressBound ||
        entry.completionOwnerReleaseFenceArmed || entry.completionOwnerReleaseTerminal ||
        !kindMatches || originalProgress != &entry.completionProgress ||
        renderGeneration == 0 || renderGeneration != entry.originalSingleRenderGeneration ||
        scheduleEpoch == 0 || scheduleEpoch != entry.scheduleEpoch ||
        scheduleEpoch != entry.originalSingleScheduleEpoch ||
        !SameCompletionOwner(owner, entry.originalSingleOwner) ||
        !entry.completionOwner.Capture(&actualOwner) || !SameCompletionOwner(actualOwner, owner) ||
        !CaptureCompletionProgressExact(entry.completionProgress, original.token_, renderGeneration, &progress) ||
        progress.playbackFactsRevision != owner.factsRevision) return false;
    entry.originalSinglePreparedCommitHeld = true;
    out->owner_ = this;
    out->entry_ = &entry;
    out->token_ = original.token_;
    out->marker_ = original.marker_;
    out->completionOwner_ = owner;
    out->progress_ = originalProgress;
    out->renderGeneration_ = renderGeneration;
    out->scheduleEpoch_ = scheduleEpoch;
    return true;
}
bool OccurrenceStatePool::MatchesPreparedSingleCompletionCommit(
    const PreparedSingleCompletionCommitProof& proof, OccurrenceToken occurrence,
    const CompletionOwnerRef& owner, const CompletionProgressRow* originalProgress,
    std::uint32_t renderGeneration, std::uint64_t scheduleEpoch,
    PlayableInstanceHandle arrangingInstance) const noexcept {
    return proof.completionOwner_.kind == CompletionOwnerKind::SingleQueue &&
        MatchesPreparedSingleProof(proof) && proof.token_.slot == occurrence.slot &&
        proof.token_.generation == occurrence.generation &&
        SameCompletionOwner(proof.completionOwner_, owner) && proof.progress_ == originalProgress &&
        proof.renderGeneration_ == renderGeneration && proof.scheduleEpoch_ == scheduleEpoch &&
        proof.entry_->key.arrangingInstance == arrangingInstance;
}
void OccurrenceStatePool::ConsumePreparedSingleCompletionCommit(
    PreparedSingleCompletionCommitProof&& proof) noexcept {
    assert(proof.completionOwner_.kind == CompletionOwnerKind::SingleQueue);
    ConsumePreparedCompletionCommitInternal(std::move(proof));
}
void OccurrenceStatePool::ConsumePreparedCompositeCompletionCommit(
    PreparedCompositeCompletionCommitProof&& proof) noexcept {
    assert(proof.proof_.completionOwner_.kind == CompletionOwnerKind::Composite);
    ConsumePreparedCompletionCommitInternal(std::move(proof.proof_));
}
void OccurrenceStatePool::ConsumePreparedCompletionCommitInternal(
    PreparedSingleCompletionCommitProof&& proof) noexcept {
    assert(proof.owner_ != nullptr && proof.owner_->MatchesPreparedSingleProof(proof));
    auto& entry = *proof.entry_;
    auto* owner = proof.owner_;
    // The exclusive native gate makes the already proven commit non-fallible.
    entry.preparedReservation = false;
    if (owner->m_occupiedCount > owner->m_highWater) owner->m_highWater = owner->m_occupiedCount;
    proof.Release();
}

OccurrenceStatePool::OriginalSingleCompletionOwnerPin::OriginalSingleCompletionOwnerPin(
    OriginalSingleCompletionOwnerPin&& other) noexcept { *this = std::move(other); }

OccurrenceStatePool::OriginalSingleCompletionOwnerPin&
OccurrenceStatePool::OriginalSingleCompletionOwnerPin::operator=(
    OriginalSingleCompletionOwnerPin&& other) noexcept {
    if (this != &other) {
        Release();
        owner_ = other.owner_;
        token_ = other.token_;
        marker_ = other.marker_;
        other.owner_ = nullptr;
        other.token_ = {};
        other.marker_ = 0;
    }
    return *this;
}

OccurrenceStatePool::OriginalSingleCompletionOwnerPin::~OriginalSingleCompletionOwnerPin() {
    Release();
}

bool OccurrenceStatePool::OriginalSingleCompletionOwnerPin::Borrow(
    OriginalSingleCompletionOwnerPin* out) const noexcept {
    return owner_ != nullptr && owner_->BorrowOriginalSingleCompletionOwnerPin(*this, out);
}

void OccurrenceStatePool::OriginalSingleCompletionOwnerPin::Release() noexcept {
    if (owner_ == nullptr) return;
    owner_->ReleaseOriginalSingleCompletionOwnerPin(*this);
}

bool OccurrenceStatePool::MatchesOriginalSingleCompletionOwnerPin(
    const OriginalSingleCompletionOwnerPin& original) const noexcept {
    if (original.owner_ != this || original.marker_ == 0) return false;
    if (original.token_.generation == 0 || original.token_.slot >= m_entries.size()) return false;
    const auto& entry = m_entries[original.token_.slot];
    return !entry.quarantined && entry.generation == original.token_.generation &&
        (entry.stage != OccurrenceStage::Free || entry.originalCompositeStorageRetired) &&
        entry.originalSingleMarker == original.marker_ && entry.originalSinglePinCount != 0;
}

bool OccurrenceStatePool::IsOriginalSingleCompletionOwnerPinFor(
    const OriginalSingleCompletionOwnerPin& original,
    const OccurrenceToken& token) const noexcept {
    return MatchesOriginalSingleCompletionOwnerPin(original) &&
        original.token_.slot == token.slot && original.token_.generation == token.generation;
}

bool OccurrenceStatePool::IsOriginalSingleCompletionOwnerCurrent(
    const OriginalSingleCompletionOwnerPin& original,
    const CompletionOwnerRef& owner, const CompletionProgressRow* originalProgress,
    std::uint32_t renderGeneration, std::uint64_t scheduleEpoch) const noexcept {
    if (!MatchesOriginalSingleCompletionOwnerPin(original)) return false;
    const auto& entry = m_entries[original.token_.slot];
    CompletionOwnerRef actualOwner{};
    CompletionProgressSnapshot progress{};
    return !entry.originalCompositeStorageRetired && !entry.preparedReservation &&
        entry.completionProgressBound && !entry.completionOwnerReleaseFenceArmed &&
        !entry.completionOwnerReleaseTerminal && scheduleEpoch != 0 &&
        entry.scheduleEpoch == scheduleEpoch && entry.originalSingleScheduleEpoch == scheduleEpoch &&
        originalProgress == &entry.completionProgress &&
        renderGeneration == entry.originalSingleRenderGeneration &&
        SameCompletionOwner(owner, entry.originalSingleOwner) &&
        entry.completionOwner.Capture(&actualOwner) && SameCompletionOwner(actualOwner, owner) &&
        CaptureCompletionProgressExact(entry.completionProgress, original.token_, renderGeneration, &progress) &&
        progress.playbackFactsRevision == entry.originalSingleOwner.factsRevision;
}

bool OccurrenceStatePool::CanWithdrawOriginalSingleCompletionForRebind(
    const OriginalSingleCompletionOwnerPin& original, const OccurrenceToken& token,
    const CompletionOwnerRef& owner, const CompletionProgressRow* originalProgress,
    std::uint32_t renderGeneration, std::uint64_t scheduleEpoch) const noexcept {
    return OriginalSingleRetirementReady(token, &original) &&
        IsOriginalSingleCompletionOwnerCurrent(original, owner, originalProgress,
            renderGeneration, scheduleEpoch);
}

bool OccurrenceStatePool::ReturnWithdrawnOriginalSingleCompletionForRebind(
    OriginalSingleCompletionOwnerPin& original, const OccurrenceToken& token,
    const CompletionOwnerRef& owner, const CompletionProgressRow* originalProgress,
    std::uint32_t renderGeneration, std::uint64_t scheduleEpoch) noexcept {
    if (!OriginalSingleRetirementReady(token, &original)) return false;
    const auto* entry = Find(token);
    if (entry == nullptr || entry->preparedReservation || entry->completionProgressBound ||
        entry->completionOwnerReleaseFenceArmed || entry->completionOwnerReleaseTerminal ||
        !entry->completionOwner.IsUnpinnedForRelease() || originalProgress != &entry->completionProgress ||
        renderGeneration == 0 || entry->originalSingleRenderGeneration != renderGeneration ||
        scheduleEpoch == 0 || entry->scheduleEpoch != scheduleEpoch ||
        entry->originalSingleScheduleEpoch != scheduleEpoch ||
        !SameCompletionOwner(entry->originalSingleOwner, owner)) return false;
    original.Release();
    return true;
}

bool OccurrenceStatePool::BorrowOriginalSingleCompletionOwnerPin(
    const OriginalSingleCompletionOwnerPin& original,
    OriginalSingleCompletionOwnerPin* out) noexcept {
    if (out == nullptr || out->Valid() || !MatchesOriginalSingleCompletionOwnerPin(original)) return false;
    auto& entry = m_entries[original.token_.slot];
    if (entry.originalSinglePinCount == UINT32_MAX) return false;
    ++entry.originalSinglePinCount;
    out->owner_ = this;
    out->token_ = original.token_;
    out->marker_ = original.marker_;
    return true;
}

void OccurrenceStatePool::ReleaseOriginalSingleCompletionOwnerPin(
    OriginalSingleCompletionOwnerPin& original) noexcept {
    const bool matches = MatchesOriginalSingleCompletionOwnerPin(original);
    assert(matches);
    if (!matches) return;
    auto& entry = m_entries[original.token_.slot];
    if (--entry.originalSinglePinCount == 0 && !entry.originalSinglePreparedCommitHeld) {
        if (entry.originalCompositeStorageRetired && entry.originalStoragePinCount == 0)
            RecycleOriginalStorage(entry);
        else ClearOriginalSingleMetadata(entry);
    }
    original.owner_ = nullptr;
    original.token_ = {};
    original.marker_ = 0;
}

void OccurrenceStatePool::RetireOriginalStorage(Entry& entry, bool afterFence) noexcept {
    assert(!entry.originalCompositeStorageRetired &&
        (entry.originalStoragePinCount != 0 || entry.originalSinglePinCount != 0) &&
        (entry.originalSinglePinCount == 0 || entry.originalSingleOwner.kind == CompletionOwnerKind::Composite) &&
        !entry.originalSinglePreparedCommitHeld && entry.completionOwner.IsUnpinnedForRelease());
    // This is reached only after the real native cancellation or strict fence.
    // Retained progress/identity is no longer an active occurrence or Current.
    entry.originalCompositeStorageRetired = true;
    entry.originalCompositeStorageAdvanceGeneration = afterFence;
    entry.stage = OccurrenceStage::Free;
    ++m_retainedStorageCount;
    --m_occupiedCount;
    if (afterFence) --m_awaitingFenceCount;
}

void OccurrenceStatePool::RecycleOriginalStorage(Entry& entry) noexcept {
    assert(entry.originalCompositeStorageRetired && entry.originalStoragePinCount == 0 &&
        entry.originalSinglePinCount == 0 &&
        !entry.originalSinglePreparedCommitHeld && entry.stage == OccurrenceStage::Free);
    if (entry.originalCompositeStorageAdvanceGeneration) {
        const auto nextGeneration = NextGeneration(entry.generation);
        if (nextGeneration == 0 || entry.completionOwner.Exhausted()) {
            entry.quarantined = true;
            ++m_quarantinedCount;
        } else entry.generation = nextGeneration;
    }
    entry.originalCompositeStorageRetired = false;
    entry.originalCompositeStorageAdvanceGeneration = false;
    entry.originalStorageEpisode = 0;
    --m_retainedStorageCount;
    entry.key = {};
    entry.scheduleEpoch = 0;
    entry.preparedReservation = false;
    entry.completionProgressBound = false;
    entry.completionOwnerReleaseFenceArmed = false;
    entry.completionOwnerReleaseTerminal = false;
    entry.completionOwnerReleaseAfterBlock = 0;
    ClearOriginalSingleMetadata(entry);
}

bool OccurrenceStatePool::RegisterOriginalSingleOwnerResolvers(void* context,
    OriginalSingleBaselineResolver baseline, OriginalSinglePreparedCommitResolver preparedCommit) noexcept {
    if (context == nullptr || baseline == nullptr || preparedCommit == nullptr ||
        m_originalSingleBaselineResolver != nullptr || m_originalSinglePreparedCommitResolver != nullptr) return false;
    for (const auto& entry : m_entries)
        if (entry.originalSinglePinCount != 0 || entry.originalSinglePreparedCommitHeld) return false;
    m_originalSingleResolverContext = context;
    m_originalSingleBaselineResolver = baseline;
    m_originalSinglePreparedCommitResolver = preparedCommit;
    return true;
}
OccurrenceStatePool::OriginalSingleCompletionOwnerPin* OccurrenceStatePool::ResolveOriginalSingleBaseline(
    const OccurrenceToken& token, OriginalSingleCompletionOwnerPin* explicitBaseline) noexcept {
    if (explicitBaseline != nullptr || m_originalSingleBaselineResolver == nullptr) return explicitBaseline;
    auto* original = m_originalSingleBaselineResolver(m_originalSingleResolverContext, *this, token);
    return original != nullptr && IsOriginalSingleCompletionOwnerPinFor(*original, token) ? original : nullptr;
}
bool OccurrenceStatePool::RegisterOriginalCompositePreparedCommitResolver(void* context,
    OriginalCompositePreparedCommitResolver resolver) noexcept {
    if (context == nullptr || resolver == nullptr || m_originalCompositeCommitResolver != nullptr) return false;
    for (const auto& entry : m_entries)
        if (entry.originalSinglePinCount != 0 || entry.originalSinglePreparedCommitHeld) return false;
    m_originalCompositeCommitContext = context;
    m_originalCompositeCommitResolver = resolver;
    return true;
}
bool OccurrenceStatePool::IsPreparedCompositeCompletionCommitFor(
    const PreparedCompositeCompletionCommitProof& proof, OccurrenceToken occurrence) const noexcept {
    return proof.proof_.completionOwner_.kind == CompletionOwnerKind::Composite &&
        proof.proof_.token_.slot == occurrence.slot && proof.proof_.token_.generation == occurrence.generation &&
        MatchesPreparedSingleProof(proof.proof_);
}
bool OccurrenceStatePool::IsPreparedSingleCompletionCommitFor(
    const PreparedSingleCompletionCommitProof& proof, const OccurrenceToken& occurrence) const noexcept {
    return proof.completionOwner_.kind == CompletionOwnerKind::SingleQueue &&
        MatchesPreparedSingleProof(proof) && proof.token_.slot == occurrence.slot &&
        proof.token_.generation == occurrence.generation;
}

bool OccurrenceStatePool::OriginalSingleRetirementReady(const OccurrenceToken& token,
    const OriginalSingleCompletionOwnerPin* originalBaseline) const noexcept {
    const auto* entry = Find(token);
    if (entry == nullptr || entry->originalSinglePreparedCommitHeld) return false;
    return originalBaseline != nullptr
        ? IsOriginalSingleCompletionOwnerPinFor(*originalBaseline, token) && entry->originalSinglePinCount == 1u
        : entry->originalSinglePinCount == 0;
}

const OccurrenceStatePool::Entry* OccurrenceStatePool::Find(
    const OccurrenceToken& token) const {
    if (token.generation == 0 || token.slot >= m_entries.size()) {
        return nullptr;
    }
    const Entry& entry = m_entries[token.slot];
    if (entry.stage == OccurrenceStage::Free || entry.quarantined ||
        entry.generation != token.generation) {
        return nullptr;
    }
    return &entry;
}

OccurrenceStatePool::Entry* OccurrenceStatePool::FindMut(
    const OccurrenceToken& token) {
    if (Find(token) == nullptr) {
        return nullptr;
    }
    return &m_entries[token.slot];
}

bool OccurrenceStatePool::Acquire(const OccurrenceKey& key,
                                  OccurrenceToken* outToken) {
    return AcquireInternal(key, outToken, false);
}

bool OccurrenceStatePool::PrepareAcquire(const OccurrenceKey& key,
                                         OccurrenceToken* outToken, OriginalStoragePin* original) {
    return AcquireInternal(key, outToken, true, original);
}

bool OccurrenceStatePool::AcquireInternal(const OccurrenceKey& key,
                                          OccurrenceToken* outToken,
                                          bool prepared, OriginalStoragePin* original) {
    if (outToken == nullptr || (original != nullptr && original->owner_ != nullptr)) {
        return false;
    }
    for (std::uint32_t slot = 0; slot < m_entries.size(); ++slot) {
        Entry& entry = m_entries[slot];
        if (entry.stage != OccurrenceStage::Free || entry.quarantined ||
            entry.originalCompositeStorageRetired || entry.originalSinglePinCount != 0 ||
            entry.originalStoragePinCount != 0) {
            continue;
        }
        const IssueResult issue =
            TryIssue(&m_nextScheduleEpoch, &m_scheduleEpochExhausted);
        if (issue.status != IssueStatus::Ok) {
            return false;
        }
        entry.stage = OccurrenceStage::Preparing;
        entry.key = key;
        entry.scheduleEpoch = issue.value;
        entry.preparedReservation = prepared;
        entry.originalSinglePreparedCommitHeld = false;
        entry.completionProgressBound = false;
        entry.completionOwnerReleaseFenceArmed = false;
        entry.completionOwnerReleaseTerminal = false;
        entry.completionOwnerReleaseAfterBlock = 0;
        ++m_occupiedCount;
        if (!prepared && m_occupiedCount > m_highWater) {
            m_highWater = m_occupiedCount;
        }
        if (original != nullptr) {
            assert(entry.originalStorageEpisode == 0);
            entry.originalStorageEpisode = issue.value;
            entry.originalStoragePinCount = 1;
            original->owner_ = this;
            original->entry_ = &entry;
            original->token_ = {slot, entry.generation};
            original->episode_ = issue.value;
            original->baseline_ = true;
        }
        outToken->slot = slot;
        outToken->generation = entry.generation;
        return true;
    }
    return false;
}

bool OccurrenceStatePool::CommitPreparedAcquire(
    const OccurrenceToken& token) {
    Entry* entry = FindMut(token);
    if (entry == nullptr || !entry->preparedReservation || entry->stage != OccurrenceStage::Preparing) return false;
    if (entry->originalSinglePreparedCommitHeld) {
        if (entry->originalSingleOwner.kind == CompletionOwnerKind::Composite) {
            auto* actual = m_originalCompositeCommitResolver != nullptr
                ? m_originalCompositeCommitResolver(m_originalCompositeCommitContext, *this, token) : nullptr;
            if (actual == nullptr || !IsPreparedCompositeCompletionCommitFor(*actual, token)) return false;
            ConsumePreparedCompositeCompletionCommit(std::move(*actual));
            return true;
        }
        auto* actual = m_originalSinglePreparedCommitResolver != nullptr
            ? m_originalSinglePreparedCommitResolver(m_originalSingleResolverContext, *this, token) : nullptr;
        if (actual == nullptr || !IsPreparedSingleCompletionCommitFor(*actual, token)) return false;
        // The genuine same-row held proof owns the exclusive native short gate.
        // This consumes it without publishing audio or relaxing another owner.
        ConsumePreparedSingleCompletionCommit(std::move(*actual));
        return true;
    }
    entry->preparedReservation = false;
    if (m_occupiedCount > m_highWater) {
        m_highWater = m_occupiedCount;
    }
    return true;
}

bool OccurrenceStatePool::CancelPreparedAcquire(
    const OccurrenceToken& token,
    OriginalSingleCompletionOwnerPin* originalBaseline, OriginalStoragePin* storageBaseline) {
    storageBaseline = ResolveOriginalStorageBaseline(token, storageBaseline);
    if (!OriginalStorageBaselineMatches(storageBaseline, token)) return false;
    originalBaseline = ResolveOriginalSingleBaseline(token, originalBaseline);
    Entry* entry = FindMut(token);
    const bool retainComposite = entry != nullptr && originalBaseline == nullptr &&
        !entry->originalSinglePreparedCommitHeld && entry->originalSinglePinCount != 0 &&
        entry->originalSingleOwner.kind == CompletionOwnerKind::Composite;
    if (entry == nullptr || (!retainComposite && !OriginalSingleRetirementReady(token, originalBaseline)) ||
        !entry->preparedReservation ||
        entry->stage != OccurrenceStage::Preparing ||
        entry->completionProgressBound ||
        !entry->completionOwner.IsUnpinnedForRelease()) {
        return false;
    }
    if (storageBaseline != nullptr) storageBaseline->Release();
    if (retainComposite) {
        RetireOriginalStorage(*entry, false);
        return true;
    }
    if (originalBaseline != nullptr) originalBaseline->Release();
    if (entry->originalStoragePinCount != 0) {
        RetireOriginalStorage(*entry, false);
        return true;
    }
    entry->stage = OccurrenceStage::Free;
    entry->key = {};
    entry->scheduleEpoch = 0;
    entry->preparedReservation = false;
    entry->completionOwnerReleaseFenceArmed = false;
    entry->completionOwnerReleaseTerminal = false;
    entry->completionOwnerReleaseAfterBlock = 0;
    --m_occupiedCount;
    return true;
}

OccurrenceStage OccurrenceStatePool::StageOf(
    const OccurrenceToken& token) const {
    const Entry* entry = Find(token);
    return entry != nullptr ? entry->stage : OccurrenceStage::Free;
}

const OccurrenceKey* OccurrenceStatePool::KeyOf(
    const OccurrenceToken& token) const {
    const Entry* entry = Find(token);
    return entry != nullptr ? &entry->key : nullptr;
}

std::uint64_t OccurrenceStatePool::ScheduleEpochOf(
    const OccurrenceToken& token) const {
    const Entry* entry = Find(token);
    return entry != nullptr ? entry->scheduleEpoch : 0;
}

bool OccurrenceStatePool::BindCompletionProgress(
    const OccurrenceToken& token, std::uint32_t renderGeneration,
    std::uint64_t playbackFactsRevision,
    std::uint64_t completedThroughBlockSequence) {
    Entry* entry = FindMut(token);
    if (entry == nullptr || entry->originalSinglePreparedCommitHeld || entry->completionProgressBound ||
        entry->originalSinglePinCount != 0 || renderGeneration == 0) {
        return false;
    }
    CompletionProgressSnapshot snapshot{};
    snapshot.occurrence = token;
    snapshot.renderGeneration = renderGeneration;
    snapshot.playbackFactsRevision = playbackFactsRevision;
    snapshot.completedThroughBlockSequence =
        completedThroughBlockSequence;
    if (!InitializeCompletionProgress(entry->completionProgress, snapshot)) {
        return false;
    }
    entry->completionProgressBound = true;
    return true;
}

CompletionProgressRow* OccurrenceStatePool::CompletionProgressOf(
    const OccurrenceToken& token) {
    Entry* entry = FindMut(token);
    return entry != nullptr && entry->completionProgressBound
               ? &entry->completionProgress
               : nullptr;
}

const CompletionProgressRow* OccurrenceStatePool::CompletionProgressOf(
    const OccurrenceToken& token) const {
    const Entry* entry = Find(token);
    return entry != nullptr && entry->completionProgressBound
               ? &entry->completionProgress
               : nullptr;
}

bool OccurrenceStatePool::UnbindCompletionProgress(
    const OccurrenceToken& token, std::uint32_t renderGeneration) {
    Entry* entry = FindMut(token);
    if (entry == nullptr || entry->originalSinglePreparedCommitHeld || !entry->completionProgressBound) {
        return false;
    }
    CompletionProgressSnapshot snapshot{};
    if (!CaptureCompletionProgressExact(entry->completionProgress, token,
                                        renderGeneration, &snapshot)) {
        return false;
    }
    entry->completionProgressBound = false;
    return true;
}

bool OccurrenceStatePool::TryPinCompletionOwner(
    const OccurrenceToken& token, const CompletionOwnerRef& owner,
    OriginalSingleCompletionOwnerPin* original) {
    return TryPinCompletionOwnerInternal(token, owner, original, false);
}

bool OccurrenceStatePool::TryPinOriginalCompositeCompletionOwner(
    const OccurrenceToken& token, const CompletionOwnerRef& owner,
    OriginalCompositeCompletionOwnerPin* original) {
    return original != nullptr && TryPinCompletionOwnerInternal(token, owner, &original->original_, true);
}

bool OccurrenceStatePool::IsOriginalCompositeCompletionOwnerPinFor(
    const OriginalCompositeCompletionOwnerPin& original, OccurrenceToken token) const noexcept {
    if (!IsOriginalSingleCompletionOwnerPinFor(original.original_, token)) return false;
    return m_entries[token.slot].originalSingleOwner.kind == CompletionOwnerKind::Composite;
}

bool OccurrenceStatePool::IsOriginalCompositeCompletionOwnerCurrent(
    const OriginalCompositeCompletionOwnerPin& original, const CompletionOwnerRef& owner,
    const CompletionProgressRow* progress, std::uint32_t renderGeneration,
    std::uint64_t scheduleEpoch) const noexcept {
    return owner.kind == CompletionOwnerKind::Composite &&
        IsOriginalSingleCompletionOwnerCurrent(original.original_, owner, progress, renderGeneration, scheduleEpoch);
}

bool OccurrenceStatePool::TryPinCompletionOwnerInternal(
    const OccurrenceToken& token, const CompletionOwnerRef& owner,
    OriginalSingleCompletionOwnerPin* original, bool compositeFactory) {
    Entry* entry = FindMut(token);
    if (entry == nullptr || entry->originalSinglePreparedCommitHeld || !entry->completionProgressBound ||
        entry->completionOwnerReleaseFenceArmed) {
        return false;
    }
    CompletionProgressSnapshot progress{};
    if (!CaptureCompletionProgress(entry->completionProgress, &progress) ||
        progress.occurrence.slot != token.slot ||
        progress.occurrence.generation != token.generation ||
        progress.playbackFactsRevision != owner.factsRevision) {
        return false;
    }
    const bool originalKindMatches = compositeFactory
        ? owner.kind == CompletionOwnerKind::Composite && owner.composite.Valid() && !owner.singleDecision.Valid()
        : owner.kind == CompletionOwnerKind::SingleQueue && owner.singleDecision.Valid() && !owner.composite.Valid();
    if (original != nullptr && (original->Valid() || !originalKindMatches || entry->originalSinglePinCount != 0 ||
        entry->originalSingleMarker != 0 || m_originalSingleMarkerExhausted ||
        m_nextOriginalSingleMarker == 0)) return false;
    if (entry->originalSinglePinCount != 0 &&
        !SameCompletionOwner(owner, entry->originalSingleOwner)) return false;
    if (!entry->completionOwner.TryPin(owner)) return false;
    if (original != nullptr) {
        // Logic-thread preflight above makes marker issue non-fallible here.
        const auto marker = TryIssue(&m_nextOriginalSingleMarker, &m_originalSingleMarkerExhausted);
        assert(marker.status == IssueStatus::Ok);
        entry->originalSingleMarker = marker.value;
        entry->originalSinglePinCount = 1;
        entry->originalSingleOwner = owner;
        entry->originalSingleRenderGeneration = progress.renderGeneration;
        entry->originalSingleScheduleEpoch = entry->scheduleEpoch;
        original->owner_ = this;
        original->token_ = token;
        original->marker_ = marker.value;
    }
    return true;
}

bool OccurrenceStatePool::CaptureCompletionOwner(
    const OccurrenceToken& token, CompletionOwnerRef* out) const {
    const Entry* entry = Find(token);
    return entry != nullptr && entry->completionOwner.Capture(out);
}

bool OccurrenceStatePool::ReplaceCompletionOwnerExact(
    const OccurrenceToken& token,
    const CompletionOwnerRef& expected,
    const CompletionOwnerRef& replacement) {
    Entry* entry = FindMut(token);
    if (entry == nullptr || entry->originalSinglePreparedCommitHeld || !entry->completionProgressBound ||
        entry->completionOwnerReleaseFenceArmed ||
        entry->completionOwnerReleaseTerminal ||
        expected.factsRevision != replacement.factsRevision ||
        (entry->originalSinglePinCount != 0 &&
         !SameCompletionOwner(expected, replacement))) {
        return false;
    }
    CompletionProgressSnapshot progress{};
    return CaptureCompletionProgress(entry->completionProgress, &progress) &&
           progress.occurrence.slot == token.slot &&
           progress.occurrence.generation == token.generation &&
           progress.playbackFactsRevision == replacement.factsRevision &&
           entry->completionOwner.ReplaceExact(expected, replacement);
}

bool OccurrenceStatePool::ClearCompletionOwnerExact(
    const OccurrenceToken& token, const CompletionOwnerRef& owner) {
    Entry* entry = FindMut(token);
    if (entry == nullptr || entry->originalSinglePreparedCommitHeld || !entry->completionProgressBound ||
        entry->completionOwnerReleaseFenceArmed ||
        entry->completionOwnerReleaseTerminal) {
        return false;
    }
    if (!entry->completionOwner.ClearExact(owner)) {
        return false;
    }
    if (m_afterCompletionOwnerClearHookForTest != nullptr) {
        m_afterCompletionOwnerClearHookForTest(
            m_afterCompletionOwnerClearHookContextForTest);
    }
    CompletionProgressSnapshot progress{};
    if (!CaptureCompletionProgress(entry->completionProgress, &progress) ||
        progress.occurrence.slot != token.slot ||
        progress.occurrence.generation != token.generation ||
        progress.playbackFactsRevision != owner.factsRevision) {
        entry->completionOwnerReleaseTerminal = true;
        return false;
    }
    entry->completionOwnerReleaseAfterBlock =
        progress.completedThroughBlockSequence;
    entry->completionOwnerReleaseFenceArmed = true;
    return true;
}

bool OccurrenceStatePool::RollbackCompletionOwnerPinExact(
    const OccurrenceToken& token, const CompletionOwnerRef& owner) {
    Entry* entry = FindMut(token);
    return entry != nullptr && !entry->originalSinglePreparedCommitHeld &&
           !entry->completionOwnerReleaseFenceArmed && entry->completionOwner.ClearExact(owner);
}

bool OccurrenceStatePool::CompletionOwnerReleaseFencePassed(
    const OccurrenceToken& token,
    std::uint64_t observedCompletionBlockSequence) const {
    const Entry* entry = Find(token);
    return entry != nullptr && entry->completionOwnerReleaseFenceArmed &&
           !entry->completionOwnerReleaseTerminal &&
           entry->completionOwner.IsUnpinnedForRelease() &&
           observedCompletionBlockSequence >
               entry->completionOwnerReleaseAfterBlock;
}

bool OccurrenceStatePool::Advance(const OccurrenceToken& token,
                                  OccurrenceStage to) {
    Entry* entry = FindMut(token);
    if (entry == nullptr || entry->originalSinglePreparedCommitHeld) {
        return false;
    }
    // Strictly forward only. Free is not a destination: the slot returns there
    // through the fence release, which is also where identity ends.
    if (to == OccurrenceStage::Free ||
        static_cast<std::uint8_t>(to) <=
            static_cast<std::uint8_t>(entry->stage)) {
        return false;
    }
    entry->stage = to;
    if (to == OccurrenceStage::AwaitingEventFence) {
        ++m_awaitingFenceCount;
    }
    return true;
}

IssueResult OccurrenceStatePool::BumpScheduleEpochChecked(
    const OccurrenceToken& token) {
    Entry* entry = FindMut(token);
    if (entry == nullptr || entry->originalSinglePreparedCommitHeld) {
        return {};
    }
    const IssueResult issue =
        TryIssue(&m_nextScheduleEpoch, &m_scheduleEpochExhausted);
    if (issue.status != IssueStatus::Ok) {
        return issue;
    }
    entry->scheduleEpoch = issue.value;
    return issue;
}

bool OccurrenceStatePool::ValidateTransaction(
    const OccurrenceToken& token, std::uint64_t occurrenceScheduleEpoch,
    std::uint64_t arrangingInstanceScheduleEpoch,
    const PlayableInstanceTable& instances) const {
    const Entry* entry = Find(token);
    if (entry == nullptr || entry->scheduleEpoch != occurrenceScheduleEpoch) {
        return false;
    }
    // The owner check is not optional and not separable: an occurrence whose
    // arranging instance has ended or been cancelled has no standing, however
    // current its own epoch looks.
    return instances.ValidateStamp(entry->key.arrangingInstance,
                                   arrangingInstanceScheduleEpoch);
}

bool OccurrenceStatePool::ReleaseAfterFence(
    const OccurrenceToken& token,
    std::uint64_t observedCompletionBlockSequence,
    OriginalSingleCompletionOwnerPin* originalBaseline, OriginalStoragePin* storageBaseline) {
    storageBaseline = ResolveOriginalStorageBaseline(token, storageBaseline);
    if (!OriginalStorageBaselineMatches(storageBaseline, token)) return false;
    originalBaseline = ResolveOriginalSingleBaseline(token, originalBaseline);
    Entry* entry = FindMut(token);
    const bool retainComposite = entry != nullptr && originalBaseline == nullptr &&
        !entry->originalSinglePreparedCommitHeld && entry->originalSinglePinCount != 0 &&
        entry->originalSingleOwner.kind == CompletionOwnerKind::Composite;
    if (entry == nullptr || (!retainComposite && !OriginalSingleRetirementReady(token, originalBaseline)) ||
        entry->stage != OccurrenceStage::AwaitingEventFence ||
        !entry->completionOwner.IsUnpinnedForRelease() ||
        entry->completionOwnerReleaseTerminal ||
        (entry->completionOwnerReleaseFenceArmed &&
         observedCompletionBlockSequence <=
             entry->completionOwnerReleaseAfterBlock)) {
        return false;
    }
    if (storageBaseline != nullptr) storageBaseline->Release();
    if (retainComposite) {
        RetireOriginalStorage(*entry, true);
        return true;
    }
    if (originalBaseline != nullptr) originalBaseline->Release();
    if (entry->originalStoragePinCount != 0) {
        RetireOriginalStorage(*entry, true);
        return true;
    }
    const std::uint32_t nextGeneration =
        NextGeneration(entry->generation);
    if (nextGeneration == 0 || entry->completionOwner.Exhausted()) {
        entry->quarantined = true;
        ++m_quarantinedCount;
    } else {
        entry->generation = nextGeneration;
    }
    entry->stage = OccurrenceStage::Free;
    entry->key = OccurrenceKey{};
    entry->scheduleEpoch = 0;
    entry->preparedReservation = false;
    entry->completionProgressBound = false;
    entry->completionOwnerReleaseFenceArmed = false;
    entry->completionOwnerReleaseTerminal = false;
    entry->completionOwnerReleaseAfterBlock = 0;
    --m_awaitingFenceCount;
    --m_occupiedCount;
    return true;
}

std::uint32_t OccurrenceStatePool::FreeCount() const {
    std::uint32_t free = 0;
    for (const Entry& entry : m_entries) {
        if (entry.stage == OccurrenceStage::Free && !entry.quarantined &&
            !entry.originalCompositeStorageRetired && entry.originalSinglePinCount == 0 &&
            entry.originalStoragePinCount == 0) {
            ++free;
        }
    }
    return free;
}

} // namespace harken
