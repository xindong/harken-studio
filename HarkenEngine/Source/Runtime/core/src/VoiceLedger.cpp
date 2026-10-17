#include "harken/core/VoiceLedger.h"

#include <cassert>
#include <stdexcept>
#include <utility>

namespace harken {

struct VoiceLedger::MonitorSelectionStorage {
    struct Slot {
        MonitorLifecycleFacts facts{};
        std::size_t references = 0;
        std::uint32_t next = UINT32_MAX;
        std::uint32_t generation = 1;
        std::uint32_t ledgerSlot = UINT32_MAX;
        std::uint32_t ledgerGeneration = 0;
    };
    static std::size_t CheckedCapacity(std::size_t capacity) {
        if (capacity > UINT32_MAX) throw std::length_error("monitor selection capacity overflow");
        return capacity;
    }
    explicit MonitorSelectionStorage(std::size_t capacity) : slots(CheckedCapacity(capacity)) {
        for (std::size_t i = 0; i < slots.size(); ++i)
            slots[i].next = i + 1 < slots.size() ? static_cast<std::uint32_t>(i + 1) : UINT32_MAX;
        free = slots.empty() ? UINT32_MAX : 0;
    }
    Slot* Resolve(std::uint32_t slot, std::uint32_t generation) noexcept {
        if (slot >= slots.size()) return nullptr;
        auto& value = slots[slot];
        return value.references != 0 && value.generation == generation ? &value : nullptr;
    }
    void Release(std::uint32_t index) noexcept {
        auto& slot = slots[index];
        slot.references = 0;
        // Exhausted diagnostic tokens are quarantined, never wrapped or reused.
        if (slot.generation == UINT32_MAX) return;
        ++slot.generation;
        slot.next = free;
        free = index;
    }
    std::vector<Slot> slots;
    std::uint32_t free = UINT32_MAX;
};

VoiceLedger::MonitorSelection::~MonitorSelection() { Reset(); }

VoiceLedger::MonitorSelection::MonitorSelection(const MonitorSelection& other) noexcept {
    *this = other;
}

VoiceLedger::MonitorSelection& VoiceLedger::MonitorSelection::operator=(const MonitorSelection& other) noexcept {
    if (this == &other) return *this;
    Reset();
    if (other.storage_ != nullptr) {
        if (auto* slot = other.storage_->Resolve(other.slot_, other.generation_)) {
            assert(slot->references != SIZE_MAX);
            if (slot->references != SIZE_MAX) {
                ++slot->references;
                storage_ = other.storage_;
                slot_ = other.slot_;
                generation_ = other.generation_;
            }
        }
    }
    return *this;
}

VoiceLedger::MonitorSelection::MonitorSelection(MonitorSelection&& other) noexcept {
    *this = std::move(other);
}

VoiceLedger::MonitorSelection& VoiceLedger::MonitorSelection::operator=(MonitorSelection&& other) noexcept {
    if (this == &other) return *this;
    Reset();
    storage_ = std::move(other.storage_);
    slot_ = std::exchange(other.slot_, UINT32_MAX);
    generation_ = std::exchange(other.generation_, 0);
    return *this;
}

void VoiceLedger::MonitorSelection::Reset() noexcept {
    if (storage_ != nullptr) {
        if (auto* slot = storage_->Resolve(slot_, generation_)) {
            if (--slot->references == 0) storage_->Release(slot_);
        }
    }
    storage_.reset();
    slot_ = UINT32_MAX;
    generation_ = 0;
}

VnVoiceHandle VoiceLedger::MonitorSelection::RenderActivation() const noexcept {
    const auto* slot = storage_ == nullptr ? nullptr : storage_->Resolve(slot_, generation_);
    return slot == nullptr ? 0 : slot->facts.renderActivation;
}

void VoiceLedger::MonitorSelection::SetCause(const MonitorControlCause& cause) noexcept {
    auto* slot = storage_ == nullptr ? nullptr : storage_->Resolve(slot_, generation_);
    if (slot == nullptr) return;
    const auto& previous = slot->facts.selectionCause;
    if (previous.request.key == cause.request.key && previous.request.origin == cause.request.origin &&
        previous.request.operation == cause.request.operation && previous.actionFiring == cause.actionFiring &&
        previous.parentPlayable == cause.parentPlayable && previous.executionOrdinal == cause.executionOrdinal) return;
    // Diverging private candidates must not rewrite another owner's saved cause.
    if (slot->references > 1) {
        if (storage_->free == UINT32_MAX) {
            ObserveMonitorSelectionUnavailable();
            Reset();
            return;
        }
        const auto index = storage_->free;
        auto& replacement = storage_->slots[index];
        storage_->free = replacement.next;
        replacement.facts = slot->facts;
        replacement.ledgerSlot = slot->ledgerSlot;
        replacement.ledgerGeneration = slot->ledgerGeneration;
        replacement.references = 1;
        --slot->references;
        slot_ = index;
        generation_ = replacement.generation;
        slot = &replacement;
    }
    slot->facts.selectionCause = cause;
}

MonitorControlCause VoiceLedger::MonitorSelection::Cause() const noexcept {
    const auto* slot = storage_ == nullptr ? nullptr : storage_->Resolve(slot_, generation_);
    return slot == nullptr ? MonitorControlCause{} : slot->facts.selectionCause;
}

bool VoiceLedger::MonitorSelection::CaptureFacts(MonitorLifecycleFacts* out,
    std::uint32_t* generation) const noexcept {
    if (out == nullptr) return false;
    *out = {};
    if (generation != nullptr) *generation = 0;
    const auto* slot = storage_ == nullptr ? nullptr : storage_->Resolve(slot_, generation_);
    if (slot == nullptr) return false;
    *out = slot->facts;
    if (generation != nullptr) *generation = slot->ledgerGeneration;
    return true;
}

std::size_t VoiceLedger::MonitorSelectionCapacity() const noexcept { return m_monitorSelections->slots.size(); }
std::size_t VoiceLedger::MonitorSelectionStorageBytes() const noexcept {
    return m_monitorSelections->slots.size() * sizeof(MonitorSelectionStorage::Slot);
}

VoiceLedger::VoiceLedger(std::uint32_t capacity,
                         std::uint32_t renderCapacity,
                         std::size_t monitorSelectionCapacity)
    : m_entries(capacity == 0 ? 1 : capacity),
      m_monitorControlReceipts(m_entries.size()),
      m_renderOwners(renderCapacity == 0
                         ? (capacity == 0 ? 1 : capacity)
                         : renderCapacity),
      m_monitorSelections(std::make_shared<MonitorSelectionStorage>(monitorSelectionCapacity == 0
          ? 2 * m_entries.size() : monitorSelectionCapacity)) {}


VoiceLedger::~VoiceLedger() {
    // The owning Ledger/Engine must outlive every external loan. Destruction
    // does not fabricate a VoiceEnd or a physical completion fence.
    for (const auto& entry : m_entries) assert(entry.originalCompletion.externalCount == 0);
}

bool VoiceLedger::OriginalCompletionMutationOwner::Valid() const noexcept {
    return table_ != nullptr && table_->MatchesOriginalCompletionMutationOwner(*this);
}

bool VoiceLedger::OriginalCompletionMutationOwner::Borrow(
    OriginalCompletionMutationOwner* out) const noexcept {
    return table_ != nullptr && table_->BorrowOriginalCompletionMutationOwner(*this, out);
}

void VoiceLedger::OriginalCompletionMutationOwner::Release() noexcept {
    if (table_ != nullptr) table_->ReleaseOriginalCompletionMutationOwner(*this);
    table_ = nullptr;
    entry_ = nullptr;
    slot_ = UINT32_MAX;
    generation_ = 0;
    marker_ = 0;
}

bool VoiceLedger::IssueOriginalCompletionMutationOwner(
    const PreparedCommitProof& proof, OriginalCompletionMutationOwner* out) noexcept {
    return IssueOriginalCompletionMutationOwnerInternal(proof, out, CompletionOwnerKind::SingleQueue);
}

bool VoiceLedger::IssueOriginalCompositeCompletionMutationOwner(
    const PreparedCommitProof& proof, OriginalCompositeCompletionMutationOwner* out) noexcept {
    return out != nullptr && IssueOriginalCompletionMutationOwnerInternal(
        proof, &out->original_, CompletionOwnerKind::Composite);
}

bool VoiceLedger::IsOriginalCompositeCompletionMutationOwnerFor(
    const OriginalCompositeCompletionMutationOwner& original, PreparedPlayableIdentity identity,
    const PlayableParentRef& parent, VnVoiceHandle render, const CompletionMutationBinding& mutation) const noexcept {
    return mutation.owner.kind == CompletionOwnerKind::Composite &&
        IsOriginalCompletionMutationOwnerFor(original.original_, identity, parent, render, mutation);
}

bool VoiceLedger::HasOriginalCompositeCompletionNativeEnd(
    const OriginalCompositeCompletionMutationOwner& original) const noexcept {
    return MatchesOriginalCompletionMutationOwner(original.original_) &&
        original.original_.entry_->originalCompletion.fullEnd;
}

bool VoiceLedger::IsCurrentOriginalCompositeCompletionMutationOwner(
    const OriginalCompositeCompletionMutationOwner& original) const noexcept {
    return IsCurrentOriginalCompletionMutationOwner(original.original_);
}

bool VoiceLedger::IssueOriginalCompletionMutationOwnerInternal(
    const PreparedCommitProof& proof, OriginalCompletionMutationOwner* out, CompletionOwnerKind kind) noexcept {
    if (out == nullptr || out->table_ != nullptr || proof.table_ != this ||
        proof.slot_ >= m_entries.size() || proof.entry_ != &m_entries[proof.slot_] ||
        proof.renderHandle_ == 0 || static_cast<std::uint32_t>(proof.renderHandle_ >> 32) == 0 ||
        static_cast<std::uint32_t>(proof.renderHandle_) >= m_renderOwners.size() ||
        proof.reverse_ != &m_renderOwners[static_cast<std::uint32_t>(proof.renderHandle_)] ||
        m_lastOriginalCompletionMarker == UINT64_MAX) return false;
    auto& entry = *proof.entry_;
    auto& original = entry.originalCompletion;
    if (entry.state != EntryState::PreparedCommitHeld || !entry.preparedReservation ||
        entry.renderHandle != 0 || entry.visibility != PublicVisibility::PlayableOwnedInternal ||
        entry.generation == 0 || entry.generation > kMaxGeneration ||
        entry.publicHandle != MakeHandle(proof.slot_, entry.generation) ||
        entry.parent.internalToken == 0 || proof.reverse_->heldLedgerSlot != proof.slot_ ||
        !IsValidCompletionMutationBinding(entry.completionMutation) ||
        entry.completionMutation.owner.kind != kind ||
        original.marker != 0 || original.externalCount != 0) return false;
    original.generation = entry.generation;
    original.parent = entry.parent;
    original.render = proof.renderHandle_;
    original.mutation = entry.completionMutation;
    original.marker = ++m_lastOriginalCompletionMarker;
    original.externalCount = 1;
    original.nativeAssociation = true;
    original.committed = false;
    original.fullEnd = false;
    out->table_ = this;
    out->entry_ = &entry;
    out->slot_ = proof.slot_;
    out->generation_ = original.generation;
    out->marker_ = original.marker;
    return true;
}

bool VoiceLedger::MatchesOriginalCompletionMutationOwner(
    const OriginalCompletionMutationOwner& cap) const noexcept {
    if (cap.table_ != this || cap.slot_ >= m_entries.size() ||
        cap.entry_ != &m_entries[cap.slot_] || cap.marker_ == 0 || cap.generation_ == 0) return false;
    const auto& original = cap.entry_->originalCompletion;
    return original.marker == cap.marker_ && original.generation == cap.generation_ &&
        original.externalCount != 0;
}

bool VoiceLedger::IsOriginalCompletionMutationOwnerFor(
    const OriginalCompletionMutationOwner& cap, PreparedPlayableIdentity identity,
    const PlayableParentRef& parent, VnVoiceHandle render,
    const CompletionMutationBinding& mutation) const noexcept {
    if (!MatchesOriginalCompletionMutationOwner(cap)) return false;
    const auto& original = cap.entry_->originalCompletion;
    return cap.slot_ == identity.slot && original.generation == identity.generation &&
        original.parent.previewHandle == parent.previewHandle &&
        original.parent.internalToken == parent.internalToken && original.render == render &&
        SameCompletionMutationBinding(original.mutation, mutation);
}

bool VoiceLedger::IsCurrentOriginalCompletionMutationOwner(
    const OriginalCompletionMutationOwner& cap) const noexcept {
    if (!MatchesOriginalCompletionMutationOwner(cap)) return false;
    const auto& entry = *cap.entry_;
    const auto& original = entry.originalCompletion;
    return original.nativeAssociation && original.committed && !original.fullEnd &&
        entry.state == EntryState::Live && !entry.preparedReservation &&
        entry.generation == original.generation && entry.renderHandle == original.render &&
        entry.visibility == PublicVisibility::PlayableOwnedInternal &&
        entry.parent.previewHandle == original.parent.previewHandle &&
        entry.parent.internalToken == original.parent.internalToken &&
        SameCompletionMutationBinding(entry.completionMutation, original.mutation) &&
        EntryForRender(original.render) == &entry;
}

bool VoiceLedger::BorrowOriginalCompletionMutationOwner(
    const OriginalCompletionMutationOwner& cap, OriginalCompletionMutationOwner* out) noexcept {
    if (out == nullptr || out->table_ != nullptr || !MatchesOriginalCompletionMutationOwner(cap)) return false;
    auto& original = cap.entry_->originalCompletion;
    if (original.externalCount == UINT32_MAX) return false;
    ++original.externalCount;
    out->table_ = this;
    out->entry_ = cap.entry_;
    out->slot_ = cap.slot_;
    out->generation_ = cap.generation_;
    out->marker_ = cap.marker_;
    return true;
}

void VoiceLedger::ReleaseOriginalCompletionMutationOwner(
    OriginalCompletionMutationOwner& cap) noexcept {
    const bool matches = MatchesOriginalCompletionMutationOwner(cap);
    assert(matches);
    if (!matches) return;
    --cap.entry_->originalCompletion.externalCount;
    ReclaimOriginalCompletionEpisode(*cap.entry_);
}

void VoiceLedger::ReclaimOriginalCompletionEpisode(Entry& entry) noexcept {
    auto& original = entry.originalCompletion;
    if (original.marker != 0 && original.fullEnd && original.externalCount == 0)
        original = {};
}

void VoiceLedger::EndOriginalCompletionEpisode(Entry& entry) noexcept {
    auto& original = entry.originalCompletion;
    if (original.marker == 0) return;
    original.nativeAssociation = false;
    original.fullEnd = true;
    ReclaimOriginalCompletionEpisode(entry);
}

void VoiceLedger::RevokeOriginalCompletionAssociation(Entry& entry) noexcept {
    entry.originalCompletion.nativeAssociation = false;
}

void VoiceLedger::ConsumeOriginalCompletionAssociation(Entry& entry, VnVoiceHandle render) noexcept {
    auto& original = entry.originalCompletion;
    if (original.marker == 0 || !original.nativeAssociation) return;
    const bool exact = !original.fullEnd && entry.generation == original.generation &&
        original.render == render && entry.parent.previewHandle == original.parent.previewHandle &&
        entry.parent.internalToken == original.parent.internalToken &&
        SameCompletionMutationBinding(entry.completionMutation, original.mutation);
    original.nativeAssociation = exact;
    if (exact) original.committed = true;
}

bool VoiceLedger::Acquire(VnVoiceHandle* outPublicHandle,
                          std::uint32_t* outLedgerSlot,
                          const MonitorRequestContext& request, const MonitorEmitterOrigin& emitterOrigin, VnObjectId media) {
    for (std::uint32_t i = 0; i < m_entries.size(); ++i) {
        Entry& entry = m_entries[i];
        if (!ReusableEntry(entry)) {
            continue;
        }
        // Reuse of an Ended slot needs no extra generation bump: reap already
        // advanced it, so the value here is the fresh generation for this
        // acquisition. Steal metadata from a prior tenant is cleared so a
        // reused entry never inherits a stale victim flag.
        entry.state = EntryState::Live;
        entry.playbackRequest = request;
        entry.emitterOrigin = emitterOrigin;
        entry.monitorContent = {};
        entry.monitorContent.content.stableId = media;
        entry.monitorContent.kind = media == VnObjectId{} ? VN_MONITOR_CONTENT_UNKNOWN : VN_MONITOR_CONTENT_MEDIA;
        entry.controlCause = {};
        entry.selectionCause = {};
        entry.monitorSelectionAccepted = false;
        entry.monitorCancellationAccepted = false;
        entry.stolen = false;
        entry.terminating = false;
        entry.preparedReservation = false;
        entry.renderHandle = 0;
        entry.visibility = PublicVisibility::DirectVoice;
        entry.parent = {};
        entry.completionMutation = {};
        entry.monitorObserveCount = 0;
        entry.publicHandle = MakeHandle(i, entry.generation);
        *outPublicHandle = entry.publicHandle;
        *outLedgerSlot = i;
        return true;
    }
    return false; // every entry is live: a defined degradation (see header)
}

bool VoiceLedger::CaptureMonitorIdentity(std::uint32_t ledgerSlot, MonitorIdentity* out) const noexcept {
    if (out == nullptr) return false;
    *out = {};
    if (ledgerSlot >= m_entries.size()) return false;
    const auto& entry = m_entries[ledgerSlot];
    if (entry.state != EntryState::Live && entry.state != EntryState::PendingStart &&
        entry.state != EntryState::Parked && entry.state != EntryState::PreparedCommitHeld) return false;
    out->ledgerSlot = ledgerSlot;
    out->generation = entry.generation;
    out->logicalHandle = entry.publicHandle;
    out->renderHandle = entry.renderHandle;
    out->parent = entry.parent;
    out->content = entry.monitorContent;
    out->playbackRequest = entry.playbackRequest;
    out->emitterOrigin = entry.emitterOrigin;
    MonitorLifecycleFacts facts{};
    CaptureMonitorFacts(entry, facts);
    out->controlCause = facts.controlCause;
    out->selectionCause = entry.selectionCause;
    out->controlObservationIncomplete = facts.controlObservationIncomplete;
    out->prepared = entry.preparedReservation;
    return true;
}

VnVoiceHandle VoiceLedger::CaptureMonitorIdentityAt(std::uint32_t ledgerSlot,
    MonitorLifecycleFacts* out) const noexcept {
    if (out == nullptr) return 0;
    *out = {};
    if (ledgerSlot >= m_entries.size()) return 0;
    const auto& entry = m_entries[ledgerSlot];
    if (entry.preparedReservation || (entry.state != EntryState::Live &&
        entry.state != EntryState::PendingStart && entry.state != EntryState::Parked &&
        entry.state != EntryState::PreparedCommitHeld)) return 0;
    CaptureMonitorFacts(entry, *out, false, false);
    out->reasonKnown = false;
    return entry.publicHandle;
}

void VoiceLedger::RecordMonitorContent(std::uint32_t ledgerSlot, const MonitorContent& content) noexcept {
    if (ledgerSlot >= m_entries.size()) return;
    auto& entry = m_entries[ledgerSlot];
    if (entry.state != EntryState::Live && entry.state != EntryState::PendingStart &&
        entry.state != EntryState::PreparedCommitHeld) return;
    entry.monitorContent = content;
}

void VoiceLedger::RecordMonitorSelection(VnVoiceHandle render, const MonitorControlCause& cause) noexcept {
    const auto owner = OwnerForRender(render);
    if (!owner.live) return;
    MonitorSelection selection{};
    if (!CaptureMonitorSelection(owner.ledgerSlot, render, &selection)) return;
    PublishMonitorSelection(selection, &cause);
}

bool VoiceLedger::CaptureMonitorSelection(std::uint32_t ledgerSlot, VnVoiceHandle render,
    MonitorSelection* out) const noexcept {
    if (out == nullptr) return false;
    *out = {};
    if (ledgerSlot >= m_entries.size() || render == 0) return false;
    const auto& entry = m_entries[ledgerSlot];
    if (entry.state != EntryState::Live && entry.state != EntryState::PendingStart &&
        entry.state != EntryState::PreparedCommitHeld) return false;
    if (entry.renderHandle != 0 && entry.renderHandle != render) return false;
    if (entry.monitorSelectionAccepted) return true;
    auto& storage = *m_monitorSelections;
    if (storage.free == UINT32_MAX) {
        ObserveMonitorSelectionUnavailable();
        return false;
    }
    const auto index = storage.free;
    auto& slot = storage.slots[index];
    storage.free = slot.next;
    slot.references = 1;
    slot.ledgerSlot = ledgerSlot;
    slot.ledgerGeneration = entry.generation;
    slot.facts = {};
    CaptureMonitorFacts(entry, slot.facts);
    slot.facts.renderActivation = render;
    slot.facts.reasonKnown = false;
    out->storage_ = m_monitorSelections;
    out->slot_ = index;
    out->generation_ = slot.generation;
    return true;
}

void VoiceLedger::PublishMonitorSelection(MonitorSelection& selection,
    const MonitorControlCause* cause) noexcept {
    if (selection.storage_ != m_monitorSelections) return;
    auto* slot = m_monitorSelections->Resolve(selection.slot_, selection.generation_);
    if (slot == nullptr) return;
    auto facts = slot->facts;
    if (cause != nullptr) facts.selectionCause = *cause;
    const auto ledgerSlot = slot->ledgerSlot;
    const auto generation = slot->ledgerGeneration;
    // Publishing retires every alias; stale aliases cannot publish after slot reuse.
    m_monitorSelections->Release(selection.slot_);
    if (facts.logicalVoice == 0 || generation == 0) return;
    if (ledgerSlot < m_entries.size()) {
        auto& entry = m_entries[ledgerSlot];
        if (entry.generation == generation && entry.publicHandle == facts.logicalVoice &&
            entry.renderHandle == facts.renderActivation) {
            if (entry.monitorSelectionAccepted) return;
            entry.selectionCause = facts.selectionCause;
            entry.monitorSelectionAccepted = true;
        }
    }
    ObserveMonitorLifecycle(MonitorLifecyclePhase::SelectionCommitted, facts.logicalVoice,
        generation, facts.contentWorld, facts.contentObject, facts.parentPlayable,
        facts.relatedAction, 0, facts.contentKind, 0, facts);
}

void VoiceLedger::RecordMonitorCancellation(VnVoiceHandle render, const MonitorControlCause& cause) noexcept {
    const auto owner = OwnerForRender(render);
    if (!owner.live) return;
    auto& entry = m_entries[owner.ledgerSlot];
    if (entry.monitorCancellationAccepted || entry.stolen || entry.terminating) return;
    entry.controlCause = cause;
    entry.monitorCancellationAccepted = true;
}

MonitorVoiceControlBinding VoiceLedger::ArmMonitorControl(std::uint32_t ledgerSlot,
    VnVoiceHandle render) noexcept {
    if (ledgerSlot >= m_entries.size() || render == 0) return {};
    auto& entry = m_entries[ledgerSlot];
    if (entry.state != EntryState::Live && entry.state != EntryState::PreparedCommitHeld &&
        entry.state != EntryState::PendingStart && entry.state != EntryState::Parked) return {};
    InvalidateMonitorControl(entry);
    entry.monitorControlAttempted = true;
    if (m_monitorControlTicket == MonitorVoiceControlReceipt::kMaxTicket ||
        !m_monitorControlReceipts[ledgerSlot].TryArm(++m_monitorControlTicket)) {
        ++m_monitorControlUnavailable;
        return {};
    }
    entry.monitorControl = {&m_monitorControlReceipts[ledgerSlot], m_monitorControlTicket,
        render, ledgerSlot, entry.generation};
    return entry.monitorControl;
}

void VoiceLedger::InvalidateMonitorControl(Entry& entry) noexcept {
    if (entry.monitorControl.Valid()) entry.monitorControl.receipt->Invalidate(entry.monitorControl.ticket);
    entry.monitorControl = {};
    entry.monitorControlAttempted = false;
}

void VoiceLedger::CaptureMonitorFacts(const Entry& entry, MonitorLifecycleFacts& facts,
    bool terminal, bool readAudioReceipt) const noexcept {
    const auto& content = entry.monitorContent;
    facts.publishLegacy = entry.visibility == PublicVisibility::DirectVoice;
    facts.logicalVoice = entry.publicHandle;
    facts.renderActivation = entry.renderHandle;
    facts.parentPlayable = entry.parent.internalToken;
    facts.root = content.root;
    facts.relatedAction = content.action;
    facts.serviceLeaf = content.leaf;
    facts.resourceOwner = content.resourceOwner;
    facts.ownerEpoch = content.ownerEpoch;
    facts.logicCommitSequence = content.logicCommit;
    facts.occurrenceIdentity = content.occurrence;
    facts.projectId = content.content.projectId;
    facts.contentObject = content.content.stableId;
    facts.contentWorld = content.content.worldGeneration;
    facts.contentRevision = content.content.contentRevision;
    facts.programSemanticFingerprint = content.content.programSemanticFingerprint;
    facts.contentKind = content.kind;
    facts.identityObservationIncomplete = content.incomplete;
    facts.playbackRequest = entry.playbackRequest;
    facts.emitterOrigin = entry.emitterOrigin;
    facts.selectionCause = entry.selectionCause;
    facts.controlCause = entry.controlCause;
    facts.controlObservationIncomplete = entry.monitorControlAttempted && !entry.monitorControl.Valid();
    if (!readAudioReceipt) return;
    MonitorVoiceControlFact observed{};
    const auto& binding = entry.monitorControl;
    if (!binding.Valid() || binding.ledgerGeneration != entry.generation ||
        binding.render != entry.renderHandle || !binding.receipt->TryRead(binding, &observed)) {
        if (terminal) facts.controlObservationIncomplete = true;
        return;
    }
    if (observed.sourceKind == SourceTerminalKind::Stop) {
        if (observed.timelineControl) facts.controlCause = observed.cause;
    } else if (observed.sourceKind != SourceTerminalKind::Cancelled) facts.controlCause = {};
}

void VoiceLedger::ObserveVoice(MonitorLifecyclePhase phase,
                               std::uint32_t ledgerSlot, std::uint32_t reason,
                               const MonitorLifecycleFacts& facts) noexcept {
    if (ledgerSlot >= m_entries.size()) {
        return;
    }
    Entry& entry = m_entries[ledgerSlot];
    if (entry.state == EntryState::PreparedCommitHeld ||
        entry.preparedReservation || entry.publicHandle == 0) {
        return;
    }
    const std::uint64_t generation = entry.generation;
    if (generation == 0) {
        return;
    }
    ++entry.monitorObserveCount;
    auto observation = facts;
    CaptureMonitorFacts(entry, observation);
    ObserveMonitorLifecycle(phase, entry.publicHandle, generation, 0, {}, 0, 0,
                            reason, 0, entry.monitorObserveCount, observation);
}

bool VoiceLedger::AcquirePlayableOwned(const PlayableParentRef& parent,
                                       std::uint32_t* outLedgerSlot,
                                       const MonitorRequestContext& request, const MonitorEmitterOrigin& emitterOrigin) {
    if (parent.internalToken == 0 || outLedgerSlot == nullptr) {
        return false;
    }
    for (std::uint32_t i = 0; i < m_entries.size(); ++i) {
        Entry& entry = m_entries[i];
        if (!ReusableEntry(entry)) {
            continue;
        }
        entry.state = EntryState::Live;
        entry.playbackRequest = request;
        entry.emitterOrigin = emitterOrigin;
        entry.controlCause = {};
        entry.selectionCause = {};
        entry.monitorContent = {};
        entry.monitorSelectionAccepted = false;
        entry.monitorCancellationAccepted = false;
        entry.stolen = false;
        entry.terminating = false;
        entry.priority = 128;
        entry.startOrder = 0;
        entry.renderHandle = 0;
        entry.publicHandle = MakeHandle(i, entry.generation);
        entry.visibility = PublicVisibility::PlayableOwnedInternal;
        entry.parent = parent;
        entry.completionMutation = {};
        entry.preparedReservation = false;
        entry.monitorObserveCount = 0;
        *outLedgerSlot = i;
        return true;
    }
    return false;
}

bool VoiceLedger::PreparePlayableOwned(const PlayableParentRef& parent,
                                       std::uint32_t* outLedgerSlot,
                                       PreparedPlayableIdentity* outIdentity,
                                       const MonitorRequestContext& request, const MonitorEmitterOrigin& emitterOrigin) {
    if (parent.internalToken == 0 || outLedgerSlot == nullptr) {
        return false;
    }
    for (std::uint32_t i = 0; i < m_entries.size(); ++i) {
        Entry& entry = m_entries[i];
        if (!ReusableEntry(entry)) {
            continue;
        }
        entry.state = EntryState::Live;
        entry.playbackRequest = request;
        entry.emitterOrigin = emitterOrigin;
        entry.controlCause = {};
        entry.selectionCause = {};
        entry.monitorContent = {};
        entry.monitorSelectionAccepted = false;
        entry.monitorCancellationAccepted = false;
        entry.stolen = false;
        entry.terminating = false;
        entry.preparedReservation = true;
        entry.priority = 128;
        entry.startOrder = 0;
        entry.renderHandle = 0;
        entry.publicHandle = MakeHandle(i, entry.generation);
        entry.visibility = PublicVisibility::PlayableOwnedInternal;
        entry.parent = parent;
        entry.completionMutation = {};
        entry.monitorObserveCount = 0;
        *outLedgerSlot = i;
        if (outIdentity != nullptr) *outIdentity = {i, entry.generation};
        return true;
    }
    return false;
}

bool VoiceLedger::CommitPreparedPlayableOwned(
    std::uint32_t ledgerSlot) {
    if (ledgerSlot >= m_entries.size()) {
        return false;
    }
    Entry& entry = m_entries[ledgerSlot];
    if (entry.state != EntryState::Live ||
        !entry.preparedReservation || entry.renderHandle != 0 ||
        entry.visibility != PublicVisibility::PlayableOwnedInternal) {
        return false;
    }
    entry.preparedReservation = false;
    return true;
}

bool VoiceLedger::CancelPreparedPlayableOwned(
    std::uint32_t ledgerSlot) {
    if (ledgerSlot >= m_entries.size()) {
        return false;
    }
    Entry& entry = m_entries[ledgerSlot];
    if (entry.state != EntryState::Live ||
        !entry.preparedReservation || entry.renderHandle != 0 ||
        entry.visibility != PublicVisibility::PlayableOwnedInternal) {
        return false;
    }
    entry.stolen = false;
    entry.terminating = false;
    entry.preparedReservation = false;
    entry.priority = 128;
    entry.startOrder = 0;
    entry.publicHandle = 0;
    entry.visibility = PublicVisibility::DirectVoice;
    entry.parent = {};
    entry.completionMutation = {};
    AdvanceGeneration(entry, EntryState::Free);
    return true;
}

VoiceLedger::PreparedCommitProof::PreparedCommitProof(
    PreparedCommitProof&& other) noexcept
    : table_(std::exchange(other.table_, nullptr)),
      entry_(std::exchange(other.entry_, nullptr)),
      reverse_(std::exchange(other.reverse_, nullptr)),
      slot_(std::exchange(other.slot_, UINT32_MAX)),
      renderHandle_(other.renderHandle_), priority_(other.priority_) {}

VoiceLedger::PreparedCommitProof& VoiceLedger::PreparedCommitProof::operator=(
    PreparedCommitProof&& other) noexcept {
    if (this != &other) {
        Release();
        table_ = std::exchange(other.table_, nullptr);
        entry_ = std::exchange(other.entry_, nullptr);
        reverse_ = std::exchange(other.reverse_, nullptr);
        slot_ = std::exchange(other.slot_, UINT32_MAX);
        renderHandle_ = other.renderHandle_;
        priority_ = other.priority_;
    }
    return *this;
}

VoiceLedger::PreparedCommitProof::~PreparedCommitProof() { Release(); }

void VoiceLedger::PreparedCommitProof::Release() noexcept {
    if (entry_ == nullptr) return;
    assert(table_ != nullptr && reverse_ != nullptr &&
        entry_->state == EntryState::PreparedCommitHeld &&
        reverse_->heldLedgerSlot == slot_);
    reverse_->heldLedgerSlot = UINT32_MAX;
    entry_->state = EntryState::Live;
    entry_ = nullptr;
    reverse_ = nullptr;
    table_ = nullptr;
    slot_ = UINT32_MAX;
}

bool VoiceLedger::BindPreparedSingleCompletionMutation(PreparedCommitInput* original,
    const CompletionMutationBinding& binding) noexcept {
    return BindPreparedCompletionMutationInternal(original, binding, CompletionOwnerKind::SingleQueue);
}
bool VoiceLedger::BindPreparedCompositeCompletionMutation(PreparedCommitInput* original,
    const CompletionMutationBinding& binding) noexcept {
    return BindPreparedCompletionMutationInternal(original, binding, CompletionOwnerKind::Composite);
}
bool VoiceLedger::BindPreparedCompletionMutationInternal(PreparedCommitInput* original,
    const CompletionMutationBinding& binding, CompletionOwnerKind kind) noexcept {
    if (original == nullptr || original->identity.slot >= m_entries.size() ||
        original->identity.generation == 0 || original->parent.internalToken == 0 ||
        original->renderHandle == 0 || static_cast<std::uint32_t>(original->renderHandle >> 32) == 0 ||
        static_cast<std::uint32_t>(original->renderHandle) >= m_renderOwners.size() ||
        !IsCanonicalUnboundCompletionMutation(original->completionMutation) ||
        !IsValidCompletionMutationBinding(binding) || binding.owner.kind != kind)
        return false;
    const auto& entry = m_entries[original->identity.slot];
    if (entry.state != EntryState::Live || !entry.preparedReservation || entry.renderHandle != 0 ||
        entry.visibility != PublicVisibility::PlayableOwnedInternal ||
        entry.generation != original->identity.generation ||
        entry.publicHandle != MakeHandle(original->identity.slot, original->identity.generation) ||
        entry.parent.previewHandle != original->parent.previewHandle ||
        entry.parent.internalToken != original->parent.internalToken ||
        !IsCanonicalUnboundCompletionMutation(entry.completionMutation) ||
        m_renderOwners[static_cast<std::uint32_t>(original->renderHandle)].heldLedgerSlot != UINT32_MAX ||
        OwnerForRender(original->renderHandle).live) return false;
    if (!BindCompletionMutation(original->identity.slot, binding)) return false;
    original->completionMutation = binding;
    return true;
}
bool VoiceLedger::ProvePreparedSingleCompletionCommit(const PreparedCommitInput& original,
    PreparedSingleCompletionCommitProof* out) noexcept {
    if (out == nullptr || out->Valid() || !IsValidCompletionMutationBinding(original.completionMutation) ||
        original.completionMutation.owner.kind != CompletionOwnerKind::SingleQueue) return false;
    if (!ProvePreparedPlayableCommit(original, &out->native_)) return false;
    out->original_ = original;
    return true;
}
bool VoiceLedger::MatchesPreparedSingleCompletionCommit(
    const PreparedSingleCompletionCommitProof& proof) const noexcept {
    const auto& native = proof.native_;
    const auto& original = proof.original_;
    if (native.table_ != this || native.slot_ >= m_entries.size() ||
        native.entry_ != &m_entries[native.slot_] || !native.Valid() ||
        native.renderHandle_ == 0 || static_cast<std::uint32_t>(native.renderHandle_) >= m_renderOwners.size() ||
        native.reverse_ != &m_renderOwners[static_cast<std::uint32_t>(native.renderHandle_)] ||
        original.identity.slot != native.slot_ || original.identity.generation == 0 ||
        original.renderHandle != native.renderHandle_ || original.priority != native.priority_) return false;
    const auto& entry = *native.entry_;
    return entry.state == EntryState::PreparedCommitHeld && entry.preparedReservation && entry.renderHandle == 0 &&
        entry.generation == original.identity.generation &&
        entry.publicHandle == MakeHandle(original.identity.slot, original.identity.generation) &&
        entry.visibility == PublicVisibility::PlayableOwnedInternal && original.parent.internalToken != 0 &&
        entry.parent.previewHandle == original.parent.previewHandle &&
        entry.parent.internalToken == original.parent.internalToken &&
        native.reverse_->heldLedgerSlot == native.slot_ &&
        IsValidCompletionMutationBinding(original.completionMutation) &&
        original.completionMutation.owner.kind == CompletionOwnerKind::SingleQueue &&
        SameCompletionMutationBinding(entry.completionMutation, original.completionMutation);
}
void VoiceLedger::ConsumePreparedSingleCompletionCommit(PreparedSingleCompletionCommitProof&& proof) noexcept {
    assert(proof.native_.table_ != nullptr && proof.native_.table_->MatchesPreparedSingleCompletionCommit(proof));
    ConsumePreparedPlayableCommit(std::move(proof.native_));
    proof.original_ = {};
}
bool VoiceLedger::IssueOriginalCompletionMutationOwner(const PreparedSingleCompletionCommitProof& proof,
    OriginalCompletionMutationOwner* out) noexcept {
    return MatchesPreparedSingleCompletionCommit(proof) && IssueOriginalCompletionMutationOwner(proof.native_, out);
}

bool VoiceLedger::ProvePreparedPlayableCommit(
    const PreparedCommitInput& expected, PreparedCommitProof* out) noexcept {
    if (out == nullptr || out->Valid() ||
        expected.identity.slot >= m_entries.size() ||
        expected.identity.generation == 0 || expected.parent.internalToken == 0 ||
        expected.renderHandle == 0 ||
        static_cast<std::uint32_t>(expected.renderHandle >> 32) == 0 ||
        static_cast<std::uint32_t>(expected.renderHandle) >= m_renderOwners.size() ||
        (!IsCanonicalUnboundCompletionMutation(expected.completionMutation) &&
         !IsValidCompletionMutationBinding(expected.completionMutation))) return false;
    Entry& entry = m_entries[expected.identity.slot];
    auto& reverse = m_renderOwners[static_cast<std::uint32_t>(expected.renderHandle)];
    if (entry.state != EntryState::Live || !entry.preparedReservation ||
        entry.renderHandle != 0 ||
        entry.visibility != PublicVisibility::PlayableOwnedInternal ||
        entry.generation != expected.identity.generation ||
        entry.publicHandle != MakeHandle(expected.identity.slot, expected.identity.generation) ||
        entry.parent.previewHandle != expected.parent.previewHandle ||
        entry.parent.internalToken != expected.parent.internalToken ||
        !SameCompletionMutationBinding(entry.completionMutation, expected.completionMutation) ||
        reverse.heldLedgerSlot != UINT32_MAX ||
        OwnerForRender(expected.renderHandle).live) return false;
    entry.state = EntryState::PreparedCommitHeld;
    reverse.heldLedgerSlot = expected.identity.slot;
    out->table_ = this;
    out->entry_ = &entry;
    out->reverse_ = &reverse;
    out->slot_ = expected.identity.slot;
    out->renderHandle_ = expected.renderHandle;
    out->priority_ = expected.priority;
    return true;
}

bool VoiceLedger::MatchesPreparedPlayableCommit(const PreparedCommitProof& proof,
    const PreparedCommitInput& original) const noexcept {
    if (proof.table_ != this || proof.slot_ >= m_entries.size() ||
        proof.entry_ != &m_entries[proof.slot_] || !proof.Valid() ||
        proof.renderHandle_ == 0 || static_cast<std::uint32_t>(proof.renderHandle_ >> 32) == 0 ||
        static_cast<std::uint32_t>(proof.renderHandle_) >= m_renderOwners.size() ||
        proof.reverse_ != &m_renderOwners[static_cast<std::uint32_t>(proof.renderHandle_)] ||
        original.identity.slot != proof.slot_ || original.identity.generation == 0 ||
        original.renderHandle != proof.renderHandle_ || original.priority != proof.priority_) return false;
    const auto& entry = *proof.entry_;
    return entry.state == EntryState::PreparedCommitHeld && entry.preparedReservation && entry.renderHandle == 0 &&
        entry.visibility == PublicVisibility::PlayableOwnedInternal && original.parent.internalToken != 0 &&
        entry.generation == original.identity.generation &&
        entry.publicHandle == MakeHandle(proof.slot_, original.identity.generation) &&
        entry.parent.previewHandle == original.parent.previewHandle &&
        entry.parent.internalToken == original.parent.internalToken &&
        proof.reverse_->heldLedgerSlot == proof.slot_ &&
        (IsCanonicalUnboundCompletionMutation(original.completionMutation) ||
         IsValidCompletionMutationBinding(original.completionMutation)) &&
        SameCompletionMutationBinding(entry.completionMutation, original.completionMutation);
}

void VoiceLedger::ConsumePreparedPlayableCommit(PreparedCommitProof&& proof) noexcept {
    assert(proof.Valid() && proof.table_ != nullptr && proof.reverse_ != nullptr &&
        proof.entry_->state == EntryState::PreparedCommitHeld &&
        proof.entry_->preparedReservation && proof.reverse_->heldLedgerSlot == proof.slot_);
    auto& entry = *proof.entry_;
    ConsumeOriginalCompletionAssociation(entry, proof.renderHandle_);
    entry.preparedReservation = false;
    entry.renderHandle = proof.renderHandle_;
    entry.priority = proof.priority_;
    entry.startOrder = proof.table_->m_nextStartOrder++;
    proof.reverse_->ledgerSlot = proof.slot_;
    proof.reverse_->renderGeneration = static_cast<std::uint32_t>(proof.renderHandle_ >> 32);
    proof.reverse_->heldLedgerSlot = UINT32_MAX;
    entry.state = EntryState::Live;
    proof.table_->ObserveVoice(MonitorLifecyclePhase::VoiceLive, proof.slot_,
        VN_ACTION_TERMINAL_CAPABILITY_UNAVAILABLE);
    proof.entry_ = nullptr;
    proof.reverse_ = nullptr;
    proof.table_ = nullptr;
    proof.slot_ = UINT32_MAX;
}

bool VoiceLedger::HasFreeEntry() const noexcept {
    for (const Entry& entry : m_entries) {
        if (ReusableEntry(entry)) {
            return true;
        }
    }
    return false;
}

std::uint32_t VoiceLedger::AvailableEntryCount() const noexcept {
    std::uint32_t count = 0;
    for (const Entry& entry : m_entries) {
        if (ReusableEntry(entry)) {
            ++count;
        }
    }
    return count;
}

void VoiceLedger::BindRender(std::uint32_t ledgerSlot,
                             VnVoiceHandle renderHandle,
                             std::uint32_t priority) {
    if (ledgerSlot >= m_entries.size()) {
        return;
    }
    Entry& entry = m_entries[ledgerSlot];
    if (entry.state == EntryState::PreparedCommitHeld || !CanBindRender(renderHandle)) return;
    if (entry.originalCompletion.marker != 0 &&
        renderHandle != entry.originalCompletion.render)
        RevokeOriginalCompletionAssociation(entry);
    if (entry.renderHandle != 0) {
        ClearReverse(ledgerSlot, entry.renderHandle);
    }
    entry.renderHandle = renderHandle;
    entry.priority = priority;
    entry.startOrder = m_nextStartOrder++;
    BindReverse(ledgerSlot, renderHandle);
    ObserveVoice(MonitorLifecyclePhase::VoiceLive, ledgerSlot,
                 VN_ACTION_TERMINAL_CAPABILITY_UNAVAILABLE);
}

bool VoiceLedger::AcquirePending(VnVoiceHandle* outPublicHandle,
                                 std::uint32_t* outLedgerSlot,
                                 std::uint32_t priority,
                                 const MonitorRequestContext& request, const MonitorEmitterOrigin& emitterOrigin, VnObjectId media) {
    for (std::uint32_t i = 0; i < m_entries.size(); ++i) {
        Entry& entry = m_entries[i];
        if (!ReusableEntry(entry)) {
            continue;
        }
        entry.state = EntryState::PendingStart;
        entry.playbackRequest = request;
        entry.emitterOrigin = emitterOrigin;
        entry.monitorContent = {};
        entry.monitorContent.content.stableId = media;
        entry.monitorContent.kind = media == VnObjectId{} ? VN_MONITOR_CONTENT_UNKNOWN : VN_MONITOR_CONTENT_MEDIA;
        entry.controlCause = {};
        entry.selectionCause = {};
        entry.monitorSelectionAccepted = false;
        entry.monitorCancellationAccepted = false;
        entry.stolen = false;
        entry.terminating = false;
        entry.preparedReservation = false;
        entry.priority = priority;
        entry.startOrder = 0; // assigned when the render slot is acquired
        entry.renderHandle = 0;
        entry.publicHandle = MakeHandle(i, entry.generation);
        entry.visibility = PublicVisibility::DirectVoice;
        entry.parent = {};
        entry.completionMutation = {};
        entry.monitorObserveCount = 0;
        *outPublicHandle = entry.publicHandle;
        *outLedgerSlot = i;
        ObserveVoice(MonitorLifecyclePhase::VoicePending, i,
                     VN_ACTION_TERMINAL_CAPABILITY_UNAVAILABLE);
        return true;
    }
    return false;
}

void VoiceLedger::StartPending(std::uint32_t ledgerSlot,
                               VnVoiceHandle renderHandle) {
    if (ledgerSlot >= m_entries.size()) {
        return;
    }
    Entry& entry = m_entries[ledgerSlot];
    if (entry.state != EntryState::PendingStart || !CanBindRender(renderHandle)) {
        return;
    }
    entry.state = EntryState::Live;
    entry.renderHandle = renderHandle;
    entry.startOrder = m_nextStartOrder++;
    BindReverse(ledgerSlot, renderHandle);
    ObserveVoice(MonitorLifecyclePhase::VoiceLive, ledgerSlot,
                 VN_ACTION_TERMINAL_CAPABILITY_UNAVAILABLE);
}

void VoiceLedger::ParkPending(std::uint32_t ledgerSlot) {
    if (ledgerSlot >= m_entries.size()) {
        return;
    }
    Entry& entry = m_entries[ledgerSlot];
    if (entry.state != EntryState::PendingStart) {
        return;
    }
    // Straight to Parked with no render binding, generation/public handle kept:
    // the deferred start never compiled a render voice, so there is no internal
    // stop to consume — unlike Park() of a live voice.
    entry.state = EntryState::Parked;
    entry.renderHandle = 0;
    entry.stolen = false;
    entry.terminating = false;
    entry.preparedReservation = false;
    entry.preparedReservation = false;
}

void VoiceLedger::CancelPending(std::uint32_t ledgerSlot, const MonitorControlCause& cause) {
    if (ledgerSlot >= m_entries.size()) {
        return;
    }
    Entry& entry = m_entries[ledgerSlot];
    if (entry.state != EntryState::PendingStart) {
        return;
    }
    MonitorLifecycleFacts facts{};
    CaptureMonitorFacts(entry, facts);
    facts.controlCause = cause;
    entry.renderHandle = 0;
    entry.stolen = false;
    entry.terminating = false;
    entry.parent = {};
    entry.completionMutation = {};
    const std::uint64_t endedHandle = entry.publicHandle;
    const std::uint64_t endedGeneration = entry.generation;
    const bool publish = endedHandle != 0;
    AdvanceGeneration(entry, EntryState::Ended);
    if (publish) {
        ObserveMonitorLifecycle(MonitorLifecyclePhase::VoiceEnd, endedHandle,
                                endedGeneration, 0, {}, 0, 0,
                                VN_ACTION_TERMINAL_STOP_REQUESTED, 0, 0, facts);
    }
}

void VoiceLedger::MarkStolen(VnVoiceHandle renderHandle, const MonitorControlCause& cause) {
    if (renderHandle == 0) {
        return;
    }
    for (Entry& entry : m_entries) {
        if (entry.state == EntryState::Live &&
            entry.renderHandle == renderHandle) {
            if (!entry.stolen && !entry.terminating && !entry.monitorCancellationAccepted) entry.controlCause = cause;
            entry.stolen = true;
            return;
        }
    }
}

bool VoiceLedger::IsStolen(VnVoiceHandle renderHandle) const {
    if (renderHandle == 0) {
        return false;
    }
    for (const Entry& entry : m_entries) {
        if (entry.state == EntryState::Live &&
            entry.renderHandle == renderHandle) {
            return entry.stolen;
        }
    }
    return false;
}

void VoiceLedger::MarkTerminating(VnVoiceHandle renderHandle, const MonitorControlCause& cause,
                                  bool recordControlCause) {
    if (renderHandle == 0) {
        return;
    }
    for (Entry& entry : m_entries) {
        if (entry.state == EntryState::Live &&
            entry.renderHandle == renderHandle) {
            if (recordControlCause && !entry.stolen && !entry.terminating && !entry.monitorCancellationAccepted) entry.controlCause = cause;
            entry.terminating = true;
            return;
        }
    }
}

bool VoiceLedger::IsTerminating(VnVoiceHandle renderHandle) const {
    if (renderHandle == 0) {
        return false;
    }
    for (const Entry& entry : m_entries) {
        if (entry.state == EntryState::Live &&
            entry.renderHandle == renderHandle) {
            return entry.terminating;
        }
    }
    return false;
}

std::uint32_t VoiceLedger::PendingStartCount() const {
    std::uint32_t count = 0;
    for (const Entry& entry : m_entries) {
        if (entry.state == EntryState::PendingStart) {
            ++count;
        }
    }
    return count;
}

VoiceLedger::Resolved VoiceLedger::Classify(VnVoiceHandle publicHandle) const {
    Resolved resolved;
    const Entry* entry = EntryFor(publicHandle);
    if (entry == nullptr ||
        entry->visibility != PublicVisibility::DirectVoice) {
        return resolved; // Stale
    }
    if (entry->state == EntryState::Live) {
        resolved.kind = HandleKind::Live;
        resolved.renderHandle = entry->renderHandle;
        resolved.ledgerSlot = static_cast<std::uint32_t>(publicHandle);
    } else if (entry->state == EntryState::PendingStart) {
        resolved.kind = HandleKind::Pending;
        resolved.ledgerSlot = static_cast<std::uint32_t>(publicHandle);
    } else if (entry->state == EntryState::Parked) {
        resolved.kind = HandleKind::Parked;
        resolved.ledgerSlot = static_cast<std::uint32_t>(publicHandle);
    }
    return resolved;
}

VoiceLedger::RenderOwner VoiceLedger::OwnerForRender(
    VnVoiceHandle renderHandle) const {
    RenderOwner owner{};
    const Entry* entry = EntryForRender(renderHandle);
    if (entry == nullptr && renderHandle != 0) {
        for (const Entry& candidate : m_entries) {
            if (candidate.state == EntryState::Live &&
                candidate.renderHandle == renderHandle) {
                entry = &candidate;
                break;
            }
        }
    }
    if (entry == nullptr) {
        return owner;
    }
    owner.live = true;
    owner.visibility = entry->visibility;
    owner.ledgerSlot = static_cast<std::uint32_t>(entry - m_entries.data());
    owner.parent = entry->parent;
    return owner;
}

bool VoiceLedger::BindCompletionMutation(
    std::uint32_t ledgerSlot,
    const CompletionMutationBinding& binding) noexcept {
    if (ledgerSlot >= m_entries.size() ||
        !IsValidCompletionMutationBinding(binding)) {
        return false;
    }
    Entry& entry = m_entries[ledgerSlot];
    if ((entry.state != EntryState::Live &&
         entry.state != EntryState::PendingStart &&
         entry.state != EntryState::Parked) ||
        !IsCanonicalUnboundCompletionMutation(entry.completionMutation) ||
        (entry.originalCompletion.marker != 0 &&
         !SameCompletionMutationBinding(entry.originalCompletion.mutation, binding))) {
        return false;
    }
    entry.completionMutation = binding;
    return true;
}

bool VoiceLedger::CaptureCompletionMutation(
    VnVoiceHandle publicHandle,
    CompletionMutationBinding* out) const noexcept {
    if (out == nullptr) {
        return false;
    }
    const Entry* entry = EntryFor(publicHandle);
    if (entry == nullptr ||
        entry->visibility != PublicVisibility::DirectVoice ||
        (entry->state != EntryState::Live &&
         entry->state != EntryState::PendingStart &&
         entry->state != EntryState::Parked)) {
        return false;
    }
    *out = entry->completionMutation;
    return true;
}

bool VoiceLedger::CaptureCompletionMutationForRender(
    VnVoiceHandle renderHandle,
    CompletionMutationBinding* out) const noexcept {
    if (out == nullptr) {
        return false;
    }
    const Entry* entry = EntryForRender(renderHandle);
    if (entry == nullptr) {
        return false;
    }
    *out = entry->completionMutation;
    return true;
}

bool VoiceLedger::CaptureCompletionMutationForSlot(
    std::uint32_t ledgerSlot,
    CompletionMutationBinding* out) const noexcept {
    if (out == nullptr || ledgerSlot >= m_entries.size()) {
        return false;
    }
    const Entry& entry = m_entries[ledgerSlot];
    if (entry.state != EntryState::Live && entry.state != EntryState::PreparedCommitHeld &&
        entry.state != EntryState::PendingStart &&
        entry.state != EntryState::Parked) {
        return false;
    }
    *out = entry.completionMutation;
    return true;
}

bool VoiceLedger::ReplaceCompletionMutationExact(
    std::uint32_t ledgerSlot,
    const CompletionMutationBinding& expected,
    const CompletionMutationBinding& replacement) noexcept {
    if (ledgerSlot >= m_entries.size() ||
        !IsValidCompletionMutationBinding(expected) ||
        !IsValidCompletionMutationBinding(replacement)) {
        return false;
    }
    Entry& entry = m_entries[ledgerSlot];
    if ((entry.state != EntryState::Live &&
         entry.state != EntryState::PendingStart &&
         entry.state != EntryState::Parked) ||
        !SameCompletionMutationBinding(entry.completionMutation,
                                       expected) ||
        (entry.originalCompletion.marker != 0 &&
         !SameCompletionMutationBinding(entry.originalCompletion.mutation, replacement))) {
        return false;
    }
    entry.completionMutation = replacement;
    return true;
}

bool VoiceLedger::ClearCompletionMutationExact(
    std::uint32_t ledgerSlot,
    const CompletionMutationBinding& binding) noexcept {
    if (ledgerSlot >= m_entries.size() ||
        !IsValidCompletionMutationBinding(binding)) {
        return false;
    }
    Entry& entry = m_entries[ledgerSlot];
    if (entry.state == EntryState::PreparedCommitHeld) return false;
    if (!SameCompletionMutationBinding(entry.completionMutation, binding)) {
        return false;
    }
    RevokeOriginalCompletionAssociation(entry);
    entry.completionMutation = {};
    return true;
}

bool VoiceLedger::Park(VnVoiceHandle renderHandle,
                       std::uint32_t* outLedgerSlot) {
    if (outLedgerSlot == nullptr) {
        return false;
    }
    Entry* entry = EntryForRender(renderHandle);
    if (entry == nullptr) {
        return false;
    }
    const std::uint32_t ledgerSlot =
        static_cast<std::uint32_t>(entry - m_entries.data());
    ClearReverse(ledgerSlot, renderHandle);
    RevokeOriginalCompletionAssociation(*entry);
    MonitorLifecycleFacts facts{};
    CaptureMonitorFacts(*entry, facts);
    entry->controlCause = facts.controlCause;
    InvalidateMonitorControl(*entry);
    entry->state = EntryState::Parked;
    entry->renderHandle = 0;
    entry->stolen = false;
    entry->terminating = false;
    *outLedgerSlot = ledgerSlot;
    return true;
}

void VoiceLedger::ReturnPark(std::uint32_t ledgerSlot,
                             VnVoiceHandle renderHandle) {
    if (ledgerSlot >= m_entries.size()) {
        return;
    }
    Entry& entry = m_entries[ledgerSlot];
    if (entry.state != EntryState::Parked || !CanBindRender(renderHandle)) {
        return;
    }
    entry.state = EntryState::Live;
    entry.renderHandle = renderHandle;
    entry.terminating = false;
    entry.startOrder = m_nextStartOrder++;
    BindReverse(ledgerSlot, renderHandle);
    MonitorLifecycleFacts facts{};
    facts.notifyType = VN_NOTIFY_VOICE_RETURNED;
    ObserveVoice(MonitorLifecyclePhase::VoiceLive, ledgerSlot,
                 VN_ACTION_TERMINAL_CAPABILITY_UNAVAILABLE, facts);
}

void VoiceLedger::EndParked(std::uint32_t ledgerSlot, const MonitorControlCause& cause) {
    if (ledgerSlot >= m_entries.size()) {
        return;
    }
    Entry& entry = m_entries[ledgerSlot];
    if (entry.state != EntryState::Parked) {
        return;
    }
    MonitorLifecycleFacts facts{};
    CaptureMonitorFacts(entry, facts);
    facts.controlCause = cause;
    entry.renderHandle = 0;
    entry.terminating = false;
    entry.parent = {};
    entry.completionMutation = {};
    const std::uint64_t endedHandle = entry.publicHandle;
    const std::uint64_t endedGeneration = entry.generation;
    const bool publish = endedHandle != 0;
    AdvanceGeneration(entry, EntryState::Ended);
    if (publish) {
        facts.notifyType = VN_NOTIFY_VOICE_ENDED;
        ObserveMonitorLifecycle(MonitorLifecyclePhase::VoiceEnd, endedHandle,
                                endedGeneration, 0, {}, 0, 0,
                                VN_ACTION_TERMINAL_NATURAL, 0, 0, facts);
    }
}

std::uint32_t VoiceLedger::ParkedCount() const {
    std::uint32_t count = 0;
    for (const Entry& entry : m_entries) {
        if (entry.state == EntryState::Parked) {
            ++count;
        }
    }
    return count;
}

VnVoiceHandle VoiceLedger::PublicHandleForSlot(std::uint32_t ledgerSlot) const {
    if (ledgerSlot >= m_entries.size()) {
        return 0;
    }
    const Entry& entry = m_entries[ledgerSlot];
    return entry.visibility == PublicVisibility::DirectVoice
               ? entry.publicHandle
               : 0;
}

void VoiceLedger::Release(std::uint32_t ledgerSlot) {
    if (ledgerSlot >= m_entries.size()) {
        return;
    }
    Entry& entry = m_entries[ledgerSlot];
    if (entry.state == EntryState::PreparedCommitHeld) return;
    if (!entry.preparedReservation && entry.state == EntryState::Live && entry.renderHandle != 0) {
        MonitorLifecycleFacts facts{};
        CaptureMonitorFacts(entry, facts);
        facts.publishLegacy = false;
        facts.reasonKnown = false;
        ObserveMonitorLifecycle(MonitorLifecyclePhase::VoiceEnd, entry.publicHandle,
            entry.generation, 0, {}, entry.parent.internalToken, 0, 0, 0, 0, facts);
    }
    if (entry.renderHandle != 0) {
        ClearReverse(ledgerSlot, entry.renderHandle);
    }
    entry.renderHandle = 0;
    entry.stolen = false;
    entry.terminating = false;
    entry.parent = {};
    entry.completionMutation = {};
    AdvanceGeneration(entry, EntryState::Free);
}

VnVoiceHandle VoiceLedger::ToRender(VnVoiceHandle publicHandle) const {
    const Entry* entry = EntryFor(publicHandle);
    if (entry == nullptr || entry->state != EntryState::Live ||
        entry->visibility != PublicVisibility::DirectVoice) {
        return 0;
    }
    return entry->renderHandle;
}

VnVoiceHandle VoiceLedger::ToPublic(VnVoiceHandle renderHandle) const {
    const Entry* entry = EntryForRender(renderHandle);
    if (entry == nullptr && renderHandle != 0) {
        for (const Entry& candidate : m_entries) {
            if (candidate.state == EntryState::Live &&
                candidate.renderHandle == renderHandle) {
                entry = &candidate;
                break;
            }
        }
    }
    if (entry == nullptr ||
        entry->visibility != PublicVisibility::DirectVoice) {
        return 0;
    }
    return entry->publicHandle;
}

void VoiceLedger::Reap(VnVoiceHandle renderHandle) {
    Entry* entry = EntryForRender(renderHandle);
    if (entry == nullptr && renderHandle != 0) {
        for (Entry& candidate : m_entries) {
            if (candidate.state == EntryState::Live &&
                candidate.renderHandle == renderHandle) {
                entry = &candidate;
                break;
            }
        }
    }
    if (entry == nullptr) {
        return;
    }
    const std::uint32_t ledgerSlot =
        static_cast<std::uint32_t>(entry - m_entries.data());
    ClearReverse(ledgerSlot, renderHandle);
    const bool stolen = entry->stolen;
    const std::uint64_t endedHandle = entry->publicHandle;
    const std::uint64_t endedGeneration = entry->generation;
    const bool publish = endedHandle != 0;
    MonitorLifecycleFacts facts{};
    CaptureMonitorFacts(*entry, facts, true);
    entry->renderHandle = 0;
    entry->stolen = false;
    entry->terminating = false;
    entry->parent = {};
    entry->completionMutation = {};
    AdvanceGeneration(*entry, EntryState::Ended);
    if (publish) {
        facts.notifyType =
            stolen ? VN_NOTIFY_VOICE_STOLEN : VN_NOTIFY_VOICE_ENDED;
        ObserveMonitorLifecycle(
            MonitorLifecyclePhase::VoiceEnd, endedHandle, endedGeneration, 0, {},
            0, 0,
            stolen ? VN_ACTION_TERMINAL_CAPACITY : VN_ACTION_TERMINAL_NATURAL,
            0, 0, facts);
    }
}

const VoiceLedger::Entry*
VoiceLedger::EntryFor(VnVoiceHandle publicHandle) const {
    if (publicHandle == 0) {
        return nullptr;
    }
    const std::uint32_t slot = static_cast<std::uint32_t>(publicHandle);
    const std::uint32_t generation =
        static_cast<std::uint32_t>(publicHandle >> 32);
    if (slot >= m_entries.size()) {
        return nullptr;
    }
    const Entry& entry = m_entries[slot];
    if (entry.generation != generation) {
        return nullptr;
    }
    return &entry;
}

const VoiceLedger::Entry*
VoiceLedger::EntryForRender(VnVoiceHandle renderHandle) const noexcept {
    if (renderHandle == 0) {
        return nullptr;
    }
    const std::uint32_t renderSlot =
        static_cast<std::uint32_t>(renderHandle);
    const std::uint32_t renderGeneration =
        static_cast<std::uint32_t>(renderHandle >> 32);
    if (renderSlot >= m_renderOwners.size() || renderGeneration == 0) {
        return nullptr;
    }
    const ReverseRenderEntry& reverse = m_renderOwners[renderSlot];
    if (reverse.renderGeneration != renderGeneration ||
        reverse.ledgerSlot >= m_entries.size()) {
        return nullptr;
    }
    const Entry& entry = m_entries[reverse.ledgerSlot];
    if (entry.state != EntryState::Live ||
        entry.renderHandle != renderHandle) {
        return nullptr;
    }
    return &entry;
}

VoiceLedger::Entry*
VoiceLedger::EntryForRender(VnVoiceHandle renderHandle) noexcept {
    return const_cast<Entry*>(
        static_cast<const VoiceLedger*>(this)->EntryForRender(renderHandle));
}

void VoiceLedger::BindReverse(std::uint32_t ledgerSlot,
                              VnVoiceHandle renderHandle) noexcept {
    if (renderHandle == 0 || !CanBindRender(renderHandle)) {
        return;
    }
    const std::uint32_t renderSlot =
        static_cast<std::uint32_t>(renderHandle);
    const std::uint32_t renderGeneration =
        static_cast<std::uint32_t>(renderHandle >> 32);
    if (renderSlot >= m_renderOwners.size() || renderGeneration == 0) {
        return;
    }
    m_renderOwners[renderSlot] = {ledgerSlot, renderGeneration};
}

void VoiceLedger::ClearReverse(std::uint32_t ledgerSlot,
                               VnVoiceHandle renderHandle) noexcept {
    if (renderHandle == 0) {
        return;
    }
    const std::uint32_t renderSlot =
        static_cast<std::uint32_t>(renderHandle);
    const std::uint32_t renderGeneration =
        static_cast<std::uint32_t>(renderHandle >> 32);
    if (renderSlot >= m_renderOwners.size()) {
        return;
    }
    ReverseRenderEntry& reverse = m_renderOwners[renderSlot];
    if (reverse.ledgerSlot == ledgerSlot &&
        reverse.renderGeneration == renderGeneration) {
        reverse.ledgerSlot = UINT32_MAX;
        reverse.renderGeneration = 0;
    }
}

void VoiceLedger::AdvanceGeneration(Entry& entry,
                                    EntryState reusableState) noexcept {
    InvalidateMonitorControl(entry);
    EndOriginalCompletionEpisode(entry);
    entry.playbackRequest = {};
    entry.emitterOrigin = {};
    entry.controlCause = {};
    entry.selectionCause = {};
    entry.monitorSelectionAccepted = false;
    entry.monitorContent = {};
    entry.monitorCancellationAccepted = false;
    entry.parent = {};
    entry.completionMutation = {};
    entry.preparedReservation = false;
    if (entry.generation >= kMaxGeneration) {
        entry.state = EntryState::Quarantined;
        entry.publicHandle = 0;
        entry.visibility = PublicVisibility::DirectVoice;
        ++m_quarantinedCount;
        return;
    }
    ++entry.generation;
    entry.state = reusableState;
    entry.publicHandle = 0;
    entry.visibility = PublicVisibility::DirectVoice;
    entry.monitorObserveCount = 0;
}

bool VoiceLedger::CanBindRender(VnVoiceHandle renderHandle) const noexcept {
    const auto slot = static_cast<std::uint32_t>(renderHandle);
    return renderHandle == 0 || slot >= m_renderOwners.size() ||
        m_renderOwners[slot].heldLedgerSlot == UINT32_MAX;
}

bool VoiceLedger::SetGenerationForTests(std::uint32_t slot,
                                        std::uint32_t generation) noexcept {
    if (slot >= m_entries.size() || generation == 0 ||
        generation > kMaxGeneration) {
        return false;
    }
    Entry& entry = m_entries[slot];
    if (!ReusableEntry(entry)) {
        return false;
    }
    entry.generation = generation;
    entry.publicHandle = 0;
    return true;
}

} // namespace harken
