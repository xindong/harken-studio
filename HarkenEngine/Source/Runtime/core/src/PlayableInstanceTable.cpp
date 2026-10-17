#include "harken/core/PlayableInstanceTable.h"

#include <algorithm>
#include <utility>
#include <cassert>
#include <limits>
#include <stdexcept>

namespace harken {
namespace {

constexpr std::uint32_t kMaximumTaggedGeneration = 0x7FFFFFFFu;

// A slot is reusable when it has never been used or when its instance has
// finished retiring. Every other stage still owns the slot.
bool Reusable(PlayableLifecycle lifecycle) {
    return lifecycle == PlayableLifecycle::Free ||
           lifecycle == PlayableLifecycle::Retired;
}

// A maximum generation is exhausted rather than wrapped back to 1. Returning 0
// lets the retirement path mark the slot permanently unavailable.
std::uint32_t NextGeneration(std::uint32_t generation) {
    if (generation >= kMaximumTaggedGeneration) {
        return 0;
    }
    return generation + 1;
}

bool SameMetadata(const PlayableRootMetadata& lhs, const PlayableRootMetadata& rhs) noexcept {
    return lhs.projectId == rhs.projectId && lhs.definitionKind == rhs.definitionKind &&
        lhs.definitionStableId == rhs.definitionStableId &&
        lhs.programSemanticFingerprint == rhs.programSemanticFingerprint &&
        lhs.invocationEmitter == rhs.invocationEmitter && lhs.actionFiring == rhs.actionFiring &&
        lhs.actionExecutionOrdinal == rhs.actionExecutionOrdinal &&
        lhs.worldGeneration == rhs.worldGeneration;
}

} // namespace

bool PreparedPlayableInstance::Valid() const noexcept {
    return owner_ != nullptr && owner_->OwnsPrepared(*this);
}

PreparedPlayableInstance& PreparedPlayableInstance::operator=(
    const PreparedPlayableInstance& other) noexcept {
    if (this != &other && (owner_ == nullptr ||
            (!owner_->PreparedCommitHeld(*this) && !owner_->ParentLinksHeld(handle_)))) {
        owner_ = other.owner_;
        handle_ = other.handle_;
        parent_ = other.parent_;
        scheduleEpoch_ = other.scheduleEpoch_;
    }
    return *this;
}

PreparedPlayableInstance& PreparedPlayableInstance::operator=(
    PreparedPlayableInstance&& other) noexcept {
    return *this = static_cast<const PreparedPlayableInstance&>(other);
}

PlayableInstanceTable::PreparedCommitProof::PreparedCommitProof(PreparedCommitProof&& other) noexcept {
    *this = std::move(other);
}

PlayableInstanceTable::PreparedCommitProof&
PlayableInstanceTable::PreparedCommitProof::operator=(PreparedCommitProof&& other) noexcept {
    if (this == &other) return *this;
    Release();
    owner_ = other.owner_;
    entry_ = other.entry_;
    original_ = other.original_;
    handle_ = other.handle_;
    other.owner_ = nullptr;
    other.entry_ = nullptr;
    other.original_ = nullptr;
    other.handle_ = 0;
    return *this;
}

void PlayableInstanceTable::PreparedCommitProof::Release() noexcept {
    if (entry_ != nullptr) {
        assert(entry_->prepared && entry_->preparationCommitHeld && entry_->preparingCommitOwned);
        entry_->preparationCommitHeld = false;
    }
    owner_ = nullptr;
    entry_ = nullptr;
    original_ = nullptr;
    handle_ = 0;
}

PlayableInstanceTable::OriginalLifetimePin::OriginalLifetimePin(OriginalLifetimePin&& other) noexcept {
    *this = std::move(other);
}
PlayableInstanceTable::OriginalLifetimePin& PlayableInstanceTable::OriginalLifetimePin::operator=(
    OriginalLifetimePin&& other) noexcept {
    if (this != &other) {
        Release();
        owner_ = std::exchange(other.owner_, nullptr);
        entry_ = std::exchange(other.entry_, nullptr);
        handle_ = std::exchange(other.handle_, 0);
        marker_ = std::exchange(other.marker_, 0);
    }
    return *this;
}
bool PlayableInstanceTable::OriginalLifetimePin::Valid() const noexcept {
    return owner_ != nullptr && owner_->MatchesOriginalLifetime(*this);
}
bool PlayableInstanceTable::OriginalLifetimePin::Borrow(OriginalLifetimePin* out) const noexcept {
    return owner_ != nullptr && owner_->BorrowOriginalLifetime(*this, out);
}
void PlayableInstanceTable::OriginalLifetimePin::Release() noexcept {
    if (owner_ != nullptr) owner_->ReturnOriginalLifetime(*this);
    owner_ = nullptr; entry_ = nullptr; handle_ = 0; marker_ = 0;
}
PlayableInstanceHandle PlayableInstanceTable::OriginalLifetimePin::OriginalParent() const noexcept {
    return Valid() ? entry_->originalLifetimeInput.parent : 0;
}
std::uint64_t PlayableInstanceTable::OriginalLifetimePin::OriginalScheduleEpoch() const noexcept {
    return Valid() ? entry_->originalLifetimeInput.epoch : 0;
}
bool PlayableInstanceTable::MatchesOriginalLifetime(const OriginalLifetimePin& original) const noexcept {
    return original.owner_ == this && original.handle_ != 0 && original.marker_ != 0 &&
        SlotOf(original.handle_) < m_entries.size() &&
        original.entry_ == &m_entries[SlotOf(original.handle_)] &&
        original.entry_->generation == GenerationOf(original.handle_) && !original.entry_->quarantined &&
        original.entry_->originalLifetimeCount != 0 &&
        original.entry_->originalLifetimeMarker == original.marker_ &&
        original.entry_->originalLifetimeInput.handle == original.handle_;
}
bool PlayableInstanceTable::IsOriginalLifetimePinFor(const OriginalLifetimePin& original,
    PlayableInstanceHandle handle) const noexcept {
    return MatchesOriginalLifetime(original) && original.handle_ == handle;
}
bool PlayableInstanceTable::IsOriginalLifetimeCurrent(const OriginalLifetimePin& original) const noexcept {
    if (!MatchesOriginalLifetime(original)) return false;
    const auto& entry = *original.entry_;
    const auto& input = entry.originalLifetimeInput;
    return !entry.originalLifetimeEnding && (entry.prepared || !Reusable(entry.lifecycle)) && entry.kind == input.kind &&
        entry.parent == input.parent && entry.scheduleEpoch == input.epoch;
}
bool PlayableInstanceTable::BorrowOriginalLifetime(const OriginalLifetimePin& original,
    OriginalLifetimePin* out) noexcept {
    if (out == nullptr || out == &original || out->owner_ != nullptr ||
        !MatchesOriginalLifetime(original) || original.entry_->originalLifetimeCount == UINT32_MAX) return false;
    ++original.entry_->originalLifetimeCount;
    out->owner_ = this; out->entry_ = original.entry_;
    out->handle_ = original.handle_; out->marker_ = original.marker_;
    return true;
}
void PlayableInstanceTable::ReturnOriginalLifetime(OriginalLifetimePin& original) noexcept {
    const bool matches = MatchesOriginalLifetime(original);
    assert(matches);
    if (!matches) return;
    auto& entry = *original.entry_;
    --entry.originalLifetimeCount;
    if (entry.originalLifetimeCount == 0) {
        entry.originalLifetimeMarker = 0;
        entry.originalLifetimeInput = {};
        entry.originalDisposition = Entry::OriginalDisposition::None;
        entry.originalDispositionPrepared = {};
        entry.originalLifetimeEnding = false;
    }
}
bool PlayableInstanceTable::OriginalLifetimeRetirementReady(const Entry& entry,
    const OriginalLifetimePin* originalBaseline) const noexcept {
    if (entry.originalLifetimeCount == 0)
        return originalBaseline == nullptr || originalBaseline->owner_ == nullptr;
    return entry.originalLifetimeCount == 1 && originalBaseline != nullptr &&
        MatchesOriginalLifetime(*originalBaseline) && originalBaseline->entry_ == &entry;
}
bool PlayableInstanceTable::IssueOriginalPreparedLifetime(const PreparedCommitProof& preparation,
    OriginalLifetimePin* out) noexcept {
    if (out == nullptr || out->owner_ != nullptr || preparation.owner_ != this ||
        preparation.original_ == nullptr || !preparation.Valid() ||
        preparation.original_->owner_ != this || !OwnsPrepared(*preparation.original_) ||
        preparation.handle_ != preparation.original_->Handle() || SlotOf(preparation.handle_) >= m_entries.size() ||
        preparation.entry_ != &m_entries[SlotOf(preparation.handle_)]) return false;
    auto& entry = *preparation.entry_;
    const auto& input = entry.preparationInput;
    if (!entry.preparationCommitHeld || !entry.preparingCommitOwned ||
        entry.originalLifetimeCount != 0 || entry.originalLifetimeMarker != 0 ||
        input.handle != preparation.handle_ || input.parent != preparation.original_->Parent() ||
        input.epoch != preparation.original_->ScheduleEpoch() || entry.parent != input.parent ||
        entry.scheduleEpoch != input.epoch || entry.kind != input.kind || entry.lifecycle != input.lifecycle ||
        !SameMetadata(entry.rootMetadata, input.metadata) || entry.sourceRetirementHold != input.sourceHold) return false;
    const auto issue = TryIssue(&m_nextOriginalLifetimeMarker, &m_originalLifetimeMarkerExhausted);
    if (issue.status != IssueStatus::Ok) return false;
    entry.originalLifetimeInput = input;
    entry.originalLifetimeMarker = issue.value;
    entry.originalLifetimeCount = 1;
    out->owner_ = this; out->entry_ = &entry; out->handle_ = input.handle; out->marker_ = issue.value;
    return true;
}
bool PlayableInstanceTable::IssueOriginalPreparedLifetime(const PreparedParentLinksProof& preparation,
    const PreparedPlayableInstance& exactOriginal, OriginalLifetimePin* out) noexcept {
    if (preparation.owner_ != this || !preparation.Valid()) return false;
    const auto issue = [this, &exactOriginal, out](const PreparedParentLinksProof::Node& node) noexcept {
        return node.request.original == &exactOriginal && node.request.activation != nullptr &&
            node.entry == node.request.activation->entry_ && node.handle == exactOriginal.Handle() &&
            node.parent == exactOriginal.Parent() && node.epoch == exactOriginal.ScheduleEpoch() &&
            IssueOriginalPreparedLifetime(*node.request.activation, out);
    };
    for (std::size_t index = 0; index < preparation.nodeCount_; ++index)
        if (preparation.nodes_[index].request.original == &exactOriginal) return issue(preparation.nodes_[index]);
    for (std::size_t index = 0; index < preparation.anchorCount_; ++index)
        if (preparation.anchors_[index].request.original == &exactOriginal) return issue(preparation.anchors_[index]);
    return false;
}

bool PlayableInstanceTable::RegisterOriginalLifetimeBaselineResolver(void* context,
    OriginalLifetimeBaselineResolver resolver, OriginalLifetimeDispositionFn disposition) noexcept {
    if (context == nullptr || resolver == nullptr || m_originalLifetimeBaselineResolver != nullptr) return false;
    for (const auto& entry : m_entries) if (entry.originalLifetimeCount != 0) return false;
    m_originalLifetimeBaselineContext = context;
    m_originalLifetimeBaselineResolver = resolver;
    m_originalLifetimeDisposition = disposition;
    return true;
}
PlayableInstanceTable::OriginalLifetimePin* PlayableInstanceTable::FindOriginalLifetimeBaseline(
    Entry& entry, PlayableInstanceHandle handle, OriginalLifetimePin* supplied) noexcept {
    auto* actual = supplied;
    if (actual == nullptr && entry.originalLifetimeCount != 0 && m_originalLifetimeBaselineResolver != nullptr)
        actual = m_originalLifetimeBaselineResolver(m_originalLifetimeBaselineContext, *this, handle);
    return actual != nullptr && MatchesOriginalLifetime(*actual) && actual->entry_ == &entry ? actual : nullptr;
}
bool PlayableInstanceTable::RequestOriginalLifetimeDisposition(Entry& entry, PlayableInstanceHandle handle,
    Entry::OriginalDisposition requested, OriginalLifetimePin* supplied,
    const PreparedPlayableInstance* prepared) noexcept {
    if (entry.originalLifetimeCount == 0 || FindOriginalLifetimeBaseline(entry, handle, supplied) == nullptr)
        return true;
    if (entry.originalDisposition != Entry::OriginalDisposition::None && entry.originalDisposition != requested)
        return false;
    entry.originalDisposition = requested;
    if (prepared != nullptr) entry.originalDispositionPrepared = *prepared;
    return true;
}
PlayableInstanceTable::OriginalLifetimePin* PlayableInstanceTable::ResolveOriginalLifetimeBaseline(
    Entry& entry, PlayableInstanceHandle handle, OriginalLifetimePin* supplied) noexcept {
    if (entry.originalLifetimeCount == 0) return supplied;
    auto* actual = FindOriginalLifetimeBaseline(entry, handle, supplied);
    if (actual == nullptr) return nullptr;
    // This is reached only after every other native end preflight. The actual
    // request can wait for children/Source while Current and own loans continue.
    entry.originalLifetimeEnding = true;
    if (m_originalLifetimeDisposition != nullptr && m_originalLifetimeBaselineResolver != nullptr &&
        actual == m_originalLifetimeBaselineResolver(m_originalLifetimeBaselineContext, *this, handle))
        m_originalLifetimeDisposition(m_originalLifetimeBaselineContext, *this, handle);
    return actual;
}
PlayableInstanceTable::OriginalLifetimeRetirementHold::OriginalLifetimeRetirementHold(
    OriginalLifetimeRetirementHold&& other) noexcept { *this = std::move(other); }
PlayableInstanceTable::OriginalLifetimeRetirementHold&
PlayableInstanceTable::OriginalLifetimeRetirementHold::operator=(OriginalLifetimeRetirementHold&& other) noexcept {
    if (this != &other) {
        Release();
        owner_ = std::exchange(other.owner_, nullptr);
        loan_ = std::move(other.loan_);
    }
    return *this;
}
bool PlayableInstanceTable::OriginalLifetimeRetirementHold::Valid() const noexcept {
    return owner_ != nullptr && owner_->IsOriginalLifetimeRetirementHoldFor(*this, loan_.OriginalHandle());
}
void PlayableInstanceTable::OriginalLifetimeRetirementHold::Release() noexcept {
    if (owner_ == nullptr) return;
    const bool matches = owner_->IsOriginalLifetimeRetirementHoldFor(*this, loan_.OriginalHandle());
    assert(matches);
    if (!matches) return;
    owner_->ReturnOriginalLifetimeRetirementHold(*this);
}
void PlayableInstanceTable::ReturnOriginalLifetimeRetirementHold(
    OriginalLifetimeRetirementHold& original) noexcept {
    const bool matches = IsOriginalLifetimeRetirementHoldFor(original, original.loan_.OriginalHandle());
    assert(matches);
    if (!matches) return;
    original.loan_.entry_->originalLifetimeRetirementHeld = false;
    original.loan_.Release();
    original.owner_ = nullptr;
}
bool PlayableInstanceTable::IsOriginalLifetimeRetirementHoldFor(
    const OriginalLifetimeRetirementHold& original, PlayableInstanceHandle handle) const noexcept {
    return original.owner_ == this && MatchesOriginalLifetime(original.loan_) &&
        original.loan_.handle_ == handle && original.loan_.entry_->originalLifetimeEnding &&
        original.loan_.entry_->originalLifetimeRetirementHeld &&
        original.loan_.entry_->originalDisposition == Entry::OriginalDisposition::Retired;
}
bool PlayableInstanceTable::BeginOriginalLifetimeRetirement(OriginalLifetimePin& originalBaseline,
    OriginalLifetimeRetirementHold* out) noexcept {
    if (out == nullptr || out->owner_ != nullptr || out->loan_.owner_ != nullptr ||
        !MatchesOriginalLifetime(originalBaseline)) return false;
    auto& entry = *originalBaseline.entry_;
    const auto handle = originalBaseline.handle_;
    if (FindOriginalLifetimeBaseline(entry, handle, &originalBaseline) != &originalBaseline ||
        (m_originalLifetimeBaselineResolver != nullptr &&
         m_originalLifetimeBaselineResolver(m_originalLifetimeBaselineContext, *this, handle) != &originalBaseline) ||
        entry.prepared || entry.preparationCommitHeld || entry.privateLinksHeld ||
        entry.originalLifetimeEnding || entry.originalLifetimeRetirementHeld ||
        entry.lifecycle < PlayableLifecycle::LogicalComplete || entry.lifecycle >= PlayableLifecycle::Retired ||
        entry.firstChild != 0 || entry.sourceRetirementHold ||
        (entry.parent != 0 && !CanUnlinkChild(handle, entry.parent)) ||
        (entry.originalDisposition != Entry::OriginalDisposition::None &&
         entry.originalDisposition != Entry::OriginalDisposition::Retired)) return false;
    OriginalLifetimePin hold;
    if (!originalBaseline.Borrow(&hold)) return false;
    // No fallible operation follows: the actual row remains live and reserved.
    entry.originalDisposition = Entry::OriginalDisposition::Retired;
    entry.originalLifetimeRetirementHeld = true;
    out->loan_ = std::move(hold);
    out->owner_ = this;
    (void)ResolveOriginalLifetimeBaseline(entry, handle, &originalBaseline);
    return true;
}
bool PlayableInstanceTable::HasRequestedOriginalLifetimeDisposition(const OriginalLifetimePin& original) const noexcept {
    return MatchesOriginalLifetime(original) && original.entry_->originalDisposition != Entry::OriginalDisposition::None;
}
bool PlayableInstanceTable::TryFinishOriginalLifetimeDisposition(OriginalLifetimePin& originalBaseline) noexcept {
    if (!MatchesOriginalLifetime(originalBaseline)) return false;
    auto& entry = *originalBaseline.entry_;
    if (entry.originalLifetimeRetirementHeld) return false;
    if (entry.originalDisposition == Entry::OriginalDisposition::PreparedCancel) {
        auto actual = entry.originalDispositionPrepared;
        return CancelPreparedImpl(&actual, &originalBaseline, true);
    }
    return entry.originalDisposition == Entry::OriginalDisposition::Retired &&
        AdvanceLifecycle(originalBaseline.handle_, PlayableLifecycle::Retired, &originalBaseline);
}
bool PlayableInstanceTable::IssueOriginalPreparedLifetimeBaselines(
    const PreparedParentLinksProof& preparation, std::span<OriginalLifetimePin> stableBaselines) noexcept {
    if (preparation.owner_ != this || !preparation.Valid() || stableBaselines.size() != m_entries.size()) return false;
    const auto count = preparation.nodeCount_ + preparation.anchorCount_;
    const auto nodeAt = [&preparation](std::size_t index) -> const PreparedParentLinksProof::Node& {
        return index < preparation.nodeCount_ ? preparation.nodes_[index]
            : preparation.anchors_[index - preparation.nodeCount_];
    };
    for (std::size_t index = 0; index < count; ++index) {
        const auto& node = nodeAt(index);
        if (node.request.original == nullptr || node.request.activation == nullptr || node.handle == 0 ||
            SlotOf(node.handle) >= stableBaselines.size() || stableBaselines[SlotOf(node.handle)].owner_ != nullptr)
            return false;
        for (std::size_t previous = 0; previous < index; ++previous)
            if (nodeAt(previous).handle == node.handle) return false;
    }
    std::size_t issued = 0;
    for (; issued < count; ++issued) {
        const auto& node = nodeAt(issued);
        if (!IssueOriginalPreparedLifetime(preparation, *node.request.original,
                &stableBaselines[SlotOf(node.handle)])) break;
    }
    if (issued == count) return true;
    while (issued != 0) stableBaselines[SlotOf(nodeAt(--issued).handle)].Release();
    return false;
}

bool PlayableInstanceTable::PreparedCommitHeld(const PreparedPlayableInstance& original) const noexcept {
    return OwnsPrepared(original) && m_entries[SlotOf(original.Handle())].preparationCommitHeld;
}

PlayableInstanceTable::PreparedParentLinksProof::PreparedParentLinksProof(std::size_t capacity) {
    if (capacity > UINT32_MAX || capacity > (std::numeric_limits<std::size_t>::max() - 1u) / 3u)
        throw std::length_error("parent link proof capacity overflow");
    nodes_.resize(capacity);
    anchors_.resize(capacity + 1u);
    held_.resize(3u * capacity + 1u);
    groups_.resize(capacity);
}

PlayableInstanceTable::PreparedParentLinksProof::PreparedParentLinksProof(
    PreparedParentLinksProof&& other) noexcept {
    *this = std::move(other);
}

PlayableInstanceTable::PreparedParentLinksProof&
PlayableInstanceTable::PreparedParentLinksProof::operator=(PreparedParentLinksProof&& other) noexcept {
    if (this == &other) return *this;
    Release();
    owner_ = other.owner_;
    nodes_ = std::move(other.nodes_);
    anchors_ = std::move(other.anchors_);
    held_ = std::move(other.held_);
    groups_ = std::move(other.groups_);
    nodeCount_ = other.nodeCount_;
    anchorCount_ = other.anchorCount_;
    heldCount_ = other.heldCount_;
    groupCount_ = other.groupCount_;
    other.owner_ = nullptr;
    other.nodeCount_ = other.anchorCount_ = other.heldCount_ = other.groupCount_ = 0;
    return *this;
}

bool PlayableInstanceTable::PreparedParentLinksProof::Valid() const noexcept {
    if (owner_ == nullptr || heldCount_ == 0) return false;
    for (std::size_t index = 0; index < heldCount_; ++index) {
        const auto& held = held_[index];
        if (held.entry == nullptr || !held.entry->privateLinksHeld ||
            held.entry->generation != GenerationOf(held.handle) || held.entry->quarantined ||
            held.entry->scheduleEpoch != held.epoch || held.entry->parent != held.parent ||
            held.entry->firstChild != held.head || held.entry->nextSibling != held.next) return false;
    }
    return true;
}

bool PlayableInstanceTable::PreparedParentLinksProof::MatchesOriginalScope(
    const PlayableInstanceTable* expectedOwner, std::span<const ParentLinkRequest> requests,
    std::span<const ParentLinkRequest> anchors) const noexcept {
    return owner_ != nullptr && owner_ == expectedOwner &&
        owner_->ValidatePreparedParentLinksScope(*this, requests, anchors);
}

void PlayableInstanceTable::PreparedParentLinksProof::Release() noexcept {
    if (owner_ != nullptr) {
        for (std::size_t index = 0; index < heldCount_; ++index) {
            assert(held_[index].entry != nullptr && held_[index].entry->privateLinksHeld);
            held_[index].entry->privateLinksHeld = false;
        }
    }
    owner_ = nullptr;
    nodeCount_ = anchorCount_ = heldCount_ = groupCount_ = 0;
}

bool PlayableInstanceTable::ParentLinksHeld(PlayableInstanceHandle handle) const noexcept {
    const auto* entry = FindAny(handle);
    return entry != nullptr && entry->privateLinksHeld;
}

bool PlayableInstanceTable::ValidatePreparedParentLinksScope(const PreparedParentLinksProof& proof,
    std::span<const ParentLinkRequest> requests, std::span<const ParentLinkRequest> anchors) const noexcept {
    if (proof.owner_ != this || !proof.Valid() || requests.size() != proof.nodeCount_ ||
        anchors.size() != proof.anchorCount_) return false;
    const auto matches = [this](const auto& node, const auto& request) noexcept {
        return request.original != nullptr && request.original == node.request.original &&
            request.activation == node.request.activation && request.original->owner_ == this &&
            request.original->Handle() == node.handle && request.original->Parent() == node.parent &&
            request.original->ScheduleEpoch() == node.epoch;
    };
    for (std::size_t index = 0; index < requests.size(); ++index)
        if (!matches(proof.nodes_[index], requests[index])) return false;
    for (std::size_t index = 0; index < anchors.size(); ++index)
        if (!matches(proof.anchors_[index], anchors[index])) return false;
    return true;
}

bool PlayableInstanceTable::ProvePreparedParentLinks(std::span<const ParentLinkRequest> requests,
    std::span<const ParentLinkRequest> anchors, PreparedParentLinksProof* out) noexcept {
    if (out == nullptr || out->owner_ != nullptr) return false;
    out->nodeCount_ = out->anchorCount_ = out->heldCount_ = out->groupCount_ = 0;
    const auto fail = [out]() noexcept {
        out->nodeCount_ = out->anchorCount_ = out->heldCount_ = out->groupCount_ = 0;
        return false;
    };
    if ((requests.empty() && anchors.size() != 1u) || requests.size() > out->nodes_.size() ||
        anchors.size() > out->anchors_.size()) return fail();
    const auto captureOriginal = [this](const ParentLinkRequest& request,
        PreparedParentLinksProof::Node& node) noexcept {
        if (request.original == nullptr || request.activation == nullptr ||
            request.original->owner_ != this || request.activation->owner_ != this ||
            request.activation->original_ != request.original || !OwnsPrepared(*request.original)) return false;
        auto& entry = m_entries[SlotOf(request.original->Handle())];
        const auto& input = entry.preparationInput;
        if (request.activation->entry_ != &entry || request.activation->handle_ != request.original->Handle() ||
            !entry.preparationCommitHeld || !entry.preparingCommitOwned || entry.privateLinksHeld ||
            input.handle != request.original->Handle() || input.parent != request.original->Parent() ||
            input.epoch != request.original->ScheduleEpoch() || entry.parent != input.parent ||
            entry.scheduleEpoch != input.epoch || entry.firstChild != 0 || entry.nextSibling != 0) return false;
        node = {request, &entry, input.handle, input.parent, input.epoch, input.parentEpoch, UINT32_MAX};
        return true;
    };
    for (const auto& request : requests) {
        auto& node = out->nodes_[out->nodeCount_];
        if (!captureOriginal(request, node)) return fail();
        for (std::size_t prior = 0; prior < out->nodeCount_; ++prior)
            if (out->nodes_[prior].handle == node.handle) return fail();
        ++out->nodeCount_;
    }
    for (const auto& anchor : anchors) {
        auto& node = out->anchors_[out->anchorCount_];
        if (!captureOriginal(anchor, node)) return fail();
        for (std::size_t prior = 0; prior < out->anchorCount_; ++prior)
            if (out->anchors_[prior].handle == node.handle) return fail();
        bool used = requests.empty();
        for (std::size_t index = 0; index < out->nodeCount_; ++index) {
            const auto& child = out->nodes_[index];
            used = used || child.parent == node.handle;
            if (child.handle == node.handle && child.request.original != node.request.original) return fail();
        }
        if (!used) return fail();
        ++out->anchorCount_;
    }
    const auto hold = [this, out](Entry* entry, PlayableInstanceHandle handle) noexcept {
        if (entry == nullptr || entry->privateLinksHeld || entry->cancellationPrepared ||
            entry->retirementObserver.actionFiring != 0 || entry->delayedCancellationEpoch != 0) return false;
        for (std::size_t index = 0; index < out->heldCount_; ++index)
            if (out->held_[index].entry == entry) return out->held_[index].handle == handle;
        if (out->heldCount_ >= out->held_.size()) return false;
        out->held_[out->heldCount_++] = {entry, handle, entry->scheduleEpoch,
            entry->parent, entry->firstChild, entry->nextSibling};
        return true;
    };
    for (std::size_t index = 0; index < out->nodeCount_; ++index) {
        auto& node = out->nodes_[index];
        if (!hold(node.entry, node.handle)) return fail();
        if (node.parent == 0) {
            if (node.parentEpoch != 0) return fail();
            continue;
        }
        const auto* foundParent = FindAny(node.parent);
        if (foundParent == nullptr || foundParent->scheduleEpoch != node.parentEpoch) return fail();
        auto* parent = &m_entries[SlotOf(node.parent)];
        if (parent->prepared) {
            bool anchored = false;
            for (std::size_t other = 0; other < out->nodeCount_; ++other)
                anchored = anchored || out->nodes_[other].entry == parent;
            for (std::size_t other = 0; other < out->anchorCount_; ++other)
                anchored = anchored || out->anchors_[other].entry == parent;
            if (!anchored) return fail();
        } else if (parent->lifecycle != PlayableLifecycle::Active) return fail();
        if (!hold(parent, node.parent)) return fail();
        std::size_t group = 0;
        while (group < out->groupCount_ && out->groups_[group].parent != parent) ++group;
        if (group == out->groupCount_) {
            if (group >= out->groups_.size()) return fail();
            Entry* tail = nullptr;
            auto cursor = parent->firstChild;
            std::size_t visited = 0;
            while (cursor != 0) {
                if (visited++ >= m_entries.size()) return fail();
                const auto* existing = FindAny(cursor);
                if (existing == nullptr || existing->prepared || existing->parent != node.parent) return fail();
                for (std::size_t other = 0; other < out->nodeCount_; ++other)
                    if (out->nodes_[other].handle == cursor) return fail();
                tail = &m_entries[SlotOf(cursor)];
                cursor = existing->nextSibling;
            }
            if (tail != nullptr && !hold(tail, MakeHandle(
                    static_cast<std::uint32_t>(tail - m_entries.data()), tail->generation))) return fail();
            out->groups_[group] = {parent, tail, static_cast<std::uint32_t>(index), static_cast<std::uint32_t>(index)};
            ++out->groupCount_;
        } else {
            out->nodes_[out->groups_[group].last].next = static_cast<std::uint32_t>(index);
            out->groups_[group].last = static_cast<std::uint32_t>(index);
        }
    }
    for (std::size_t index = 0; index < out->anchorCount_; ++index)
        if (!hold(out->anchors_[index].entry, out->anchors_[index].handle)) return fail();
    for (std::size_t index = 0; index < out->heldCount_; ++index) out->held_[index].entry->privateLinksHeld = true;
    out->owner_ = this;
    return true;
}

bool PlayableInstanceTable::MatchesPreparedOriginalAncestry(const PreparedParentLinksProof& proof,
    PlayableInstanceHandle descendant, PlayableInstanceHandle ancestor) const noexcept {
    if (proof.owner_ != this || !proof.Valid() || descendant == 0 || ancestor == 0) return false;
    const auto matchesFactory = [this](const PreparedParentLinksProof::Node& node) noexcept {
        const auto* original = node.request.original;
        const auto* activation = node.request.activation;
        if (original == nullptr || activation == nullptr || original->owner_ != this ||
            activation->owner_ != this || SlotOf(node.handle) >= m_entries.size() ||
            node.entry != &m_entries[SlotOf(node.handle)] || activation->entry_ != node.entry ||
            activation->original_ != original || activation->handle_ != node.handle ||
            !OwnsPrepared(*original) || original->Handle() != node.handle ||
            original->Parent() != node.parent || original->ScheduleEpoch() != node.epoch) return false;
        const auto& entry = *node.entry;
        const auto& input = entry.preparationInput;
        return entry.privateLinksHeld && entry.prepared && entry.preparationCommitHeld &&
            entry.preparingCommitOwned && input.handle == node.handle && input.parent == node.parent &&
            input.epoch == node.epoch && input.parentEpoch == node.parentEpoch &&
            entry.parent == node.parent && entry.scheduleEpoch == node.epoch;
    };
    bool originalDescendant = false;
    for (std::size_t index = 0; index < proof.nodeCount_; ++index) {
        if (!matchesFactory(proof.nodes_[index])) return false;
        originalDescendant = originalDescendant || proof.nodes_[index].handle == descendant;
    }
    for (std::size_t index = 0; index < proof.anchorCount_; ++index) {
        if (!matchesFactory(proof.anchors_[index])) return false;
        originalDescendant = originalDescendant || proof.anchors_[index].handle == descendant;
    }
    if (!originalDescendant) return false;
    // Every edge is the protected original native edge captured by Prove.
    // Active ancestors are permitted only when that factory held them too.
    auto cursor = descendant;
    for (std::size_t visited = 0; visited < proof.heldCount_; ++visited) {
        const PreparedParentLinksProof::HeldEntry* actual = nullptr;
        for (std::size_t index = 0; index < proof.heldCount_; ++index) {
            const auto& held = proof.held_[index];
            if (held.handle == cursor) { actual = &held; break; }
        }
        if (actual == nullptr || SlotOf(cursor) >= m_entries.size() ||
            actual->entry != &m_entries[SlotOf(cursor)]) return false;
        if (cursor == ancestor) return true;
        cursor = actual->parent;
        if (cursor == 0) return false;
    }
    return false;
}

bool PlayableInstanceTable::CaptureOriginalPreparedNodeKind(const PreparedPlayableInstance& prepared,
    const OriginalLifetimePin& original, PlayableKind* kind) const noexcept {
    if (kind == nullptr || !OwnsPrepared(prepared) || !PreparedCommitHeld(prepared) ||
        !IsOriginalLifetimePinFor(original, prepared.Handle()) || !IsOriginalLifetimeCurrent(original) ||
        original.OriginalParent() != prepared.Parent() ||
        original.OriginalScheduleEpoch() != prepared.ScheduleEpoch()) return false;
    const auto* entry = FindAny(prepared.Handle());
    if (entry == nullptr || entry->preparationInput.kind != entry->kind) return false;
    *kind = entry->kind;
    return true;
}

bool PlayableInstanceTable::MatchesPreparedOriginalNodeCensus(const PreparedParentLinksProof& proof,
    std::span<const PreparedNodeCensusEntry> nodes) const noexcept {
    if (proof.owner_ != this || !proof.Valid() || nodes.empty()) return false;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const auto* original = nodes[index].original;
        if (original == nullptr || !OwnsPrepared(*original) ||
            !MatchesPreparedOriginalAncestry(proof, original->Handle(), original->Handle())) return false;
        const auto* entry = FindAny(original->Handle());
        if (entry == nullptr || entry->kind != nodes[index].expectedKind ||
            entry->preparationInput.kind != nodes[index].expectedKind) return false;
        for (std::size_t prior = 0; prior < index; ++prior)
            if (nodes[prior].original == original || nodes[prior].original->Handle() == original->Handle()) return false;
        bool present = false;
        for (std::size_t actual = 0; actual < proof.nodeCount_; ++actual)
            present |= proof.nodes_[actual].request.original == original;
        for (std::size_t actual = 0; actual < proof.anchorCount_; ++actual)
            present |= proof.anchors_[actual].request.original == original;
        if (!present) return false;
    }
    const auto included = [&](const PreparedParentLinksProof::Node& actual) noexcept {
        return std::count_if(nodes.begin(), nodes.end(), [&](const auto& supplied) {
            return supplied.original == actual.request.original;
        }) == 1;
    };
    for (std::size_t index = 0; index < proof.nodeCount_; ++index)
        if (!included(proof.nodes_[index])) return false;
    for (std::size_t index = 0; index < proof.anchorCount_; ++index)
        if (!included(proof.anchors_[index])) return false;
    return true;
}

void PlayableInstanceTable::ConsumePreparedParentLinks(PreparedParentLinksProof&& proof) noexcept {
    assert(proof.Valid());
    for (std::size_t index = 0; index < proof.nodeCount_; ++index) {
        auto& node = proof.nodes_[index];
        assert(!node.entry->prepared && node.entry->lifecycle == PlayableLifecycle::Active);
        node.entry->nextSibling = node.next == UINT32_MAX ? 0 : proof.nodes_[node.next].handle;
    }
    for (std::size_t index = 0; index < proof.anchorCount_; ++index)
        assert(!proof.anchors_[index].entry->prepared &&
            proof.anchors_[index].entry->lifecycle == PlayableLifecycle::Active);
    for (std::size_t index = 0; index < proof.groupCount_; ++index) {
        auto& group = proof.groups_[index];
        if (group.tail == nullptr) group.parent->firstChild = proof.nodes_[group.first].handle;
        else group.tail->nextSibling = proof.nodes_[group.first].handle;
    }
    proof.Release();
}

bool PlayableInstanceTable::ProvePreparedCommit(PreparedPlayableInstance& original,
    PreparedCommitProof* out) noexcept {
    if (out == nullptr || out->Valid()) return false;
    auto* entry = FindPrepared(original);
    if (entry == nullptr || !entry->preparingCommitOwned) return false;
    const auto& input = entry->preparationInput;
    if (input.handle != original.Handle() || input.parent != original.Parent() ||
        input.epoch != original.ScheduleEpoch() || entry->kind != input.kind ||
        entry->lifecycle != input.lifecycle || !Reusable(entry->lifecycle) ||
        entry->parent != input.parent || entry->scheduleEpoch != input.epoch ||
        !SameMetadata(entry->rootMetadata, input.metadata) ||
        entry->sourceRetirementHold != input.sourceHold ||
        (entry->parent != 0 && FindAny(entry->parent) == nullptr) ||
        entry->firstChild != 0 || entry->nextSibling != 0 ||
        entry->terminalOutcome.kind != PlayableTerminalKind::None ||
        entry->terminalOutcome.resultCode != 0 || entry->terminalOutcome.reasonCode != 0 ||
        entry->terminalOutcome.naturalCompletionOrdinal != 0 || entry->cancellationPrepared ||
        entry->preparedObserver.actionFiring != 0 ||
        entry->preparedObserver.actionExecutionOrdinal != UINT32_MAX ||
        entry->retirementObserver.actionFiring != 0 ||
        entry->retirementObserver.actionExecutionOrdinal != UINT32_MAX ||
        entry->delayedCancellationEpoch != 0) return false;
    entry->preparationCommitHeld = true;
    out->owner_ = this;
    out->entry_ = entry;
    out->original_ = &original;
    out->handle_ = original.Handle();
    return true;
}

void PlayableInstanceTable::ConsumePreparedCommit(PreparedCommitProof&& proof) noexcept {
    assert(proof.Valid() && proof.owner_ != nullptr && proof.original_ != nullptr);
    if (!proof.Valid()) return;
    assert(proof.entry_->prepared && proof.entry_->preparationCommitHeld &&
        proof.entry_->preparingCommitOwned);
    proof.entry_->preparationCommitHeld = false;
    proof.owner_->ActivatePreparedEntry(*proof.entry_, proof.handle_);
    proof.owner_ = nullptr;
    proof.entry_ = nullptr;
    proof.original_ = nullptr;
    proof.handle_ = 0;
}

bool PreparedPlayableCancellation::Valid() const noexcept {
    return owner_ != nullptr && owner_->IsCancellationPrepared(*this);
}

DelayedPlayableCancellation::~DelayedPlayableCancellation() {
    if (owner_ != nullptr) (void)owner_->CancelDelayedCancellation(this);
}

DelayedPlayableCancellation::DelayedPlayableCancellation(
    DelayedPlayableCancellation&& other) noexcept {
    *this = std::move(other);
}

DelayedPlayableCancellation& DelayedPlayableCancellation::operator=(
    DelayedPlayableCancellation&& other) noexcept {
    if (this == &other) return *this;
    if (owner_ != nullptr) (void)owner_->CancelDelayedCancellation(this);
    owner_ = other.owner_;
    handle_ = other.handle_;
    sourceEpoch_ = other.sourceEpoch_;
    issuedEpoch_ = other.issuedEpoch_;
    observer_ = other.observer_;
    accepted_ = other.accepted_;
    other.Clear();
    return *this;
}

void DelayedPlayableCancellation::Clear() noexcept {
    owner_ = nullptr;
    handle_ = 0;
    sourceEpoch_ = 0;
    issuedEpoch_ = 0;
    observer_ = {};
    accepted_ = false;
}

bool DelayedPlayableCancellation::Valid() const noexcept {
    return owner_ != nullptr && owner_->OwnsDelayedCancellation(*this);
}

PlayableInstanceTable::PlayableInstanceTable(std::uint32_t capacity)
    : m_entries(capacity) {}

PlayableInstanceTable::~PlayableInstanceTable() {
    for (const auto& entry : m_entries) assert(entry.originalLifetimeCount == 0);
}

const PlayableInstanceTable::Entry* PlayableInstanceTable::Find(
    PlayableInstanceHandle handle) const {
    if (handle == 0) {
        return nullptr;
    }
    const std::uint32_t slot = SlotOf(handle);
    if (slot >= m_entries.size()) {
        return nullptr;
    }
    const Entry& entry = m_entries[slot];
    if (Reusable(entry.lifecycle) || entry.quarantined || entry.prepared ||
        entry.generation != GenerationOf(handle)) {
        return nullptr;
    }
    return &entry;
}

PlayableInstanceTable::Entry* PlayableInstanceTable::FindMut(
    PlayableInstanceHandle handle) {
    if (Find(handle) == nullptr) {
        return nullptr;
    }
    return &m_entries[SlotOf(handle)];
}

const PlayableInstanceTable::Entry* PlayableInstanceTable::FindAny(
    PlayableInstanceHandle handle) const noexcept {
    if (handle == 0) {
        return nullptr;
    }
    const std::uint32_t slot = SlotOf(handle);
    if (slot >= m_entries.size()) {
        return nullptr;
    }
    const Entry& entry = m_entries[slot];
    if (entry.quarantined || entry.generation != GenerationOf(handle) ||
        (!entry.prepared && Reusable(entry.lifecycle))) {
        return nullptr;
    }
    return &entry;
}

bool PlayableInstanceTable::LinkChild(PlayableInstanceHandle child,
                                      PlayableInstanceHandle parent) noexcept {
    if (child == 0 || parent == 0 || child == parent) {
        return false;
    }
    Entry* childEntry = FindAnyMut(child);
    // A prepared parent is a valid transaction-local anchor.  The child edge
    // becomes externally visible only when the corresponding entries commit.
    Entry* parentEntry = FindAnyMut(parent);
    if (childEntry == nullptr || parentEntry == nullptr ||
        childEntry->privateLinksHeld || parentEntry->privateLinksHeld ||
        childEntry->parent != parent || childEntry->nextSibling != 0) {
        return false;
    }
    if (parentEntry->firstChild == 0) {
        parentEntry->firstChild = child;
        return true;
    }
    PlayableInstanceHandle cursor = parentEntry->firstChild;
    std::size_t visited = 0;
    while (cursor != 0) {
        if (cursor == child || visited++ >= m_entries.size()) return false;
        Entry* sibling = FindAnyMut(cursor);
        if (sibling == nullptr || sibling->parent != parent) {
            return false;
        }
        if (sibling->nextSibling == 0) {
            if (sibling->privateLinksHeld) return false;
            sibling->nextSibling = child;
            return true;
        }
        cursor = sibling->nextSibling;
    }
    return false;
}

bool PlayableInstanceTable::UnlinkChild(PlayableInstanceHandle child,
                                        PlayableInstanceHandle parent) noexcept {
    if (child == 0 || parent == 0) {
        return false;
    }
    Entry* parentEntry = FindAnyMut(parent);
    if (parentEntry == nullptr || parentEntry->privateLinksHeld || ParentLinksHeld(child)) {
        return false;
    }
    PlayableInstanceHandle previous = 0;
    PlayableInstanceHandle cursor = parentEntry->firstChild;
    std::size_t visited = 0;
    while (cursor != 0) {
        if (visited++ >= m_entries.size()) return false;
        Entry* current = FindAnyMut(cursor);
        if (current == nullptr || current->parent != parent) {
            return false;
        }
        if (cursor == child) {
            if (current->privateLinksHeld) return false;
            const PlayableInstanceHandle next = current->nextSibling;
            if (previous == 0) {
                parentEntry->firstChild = next;
            } else {
                Entry* previousEntry = FindAnyMut(previous);
                if (previousEntry == nullptr || previousEntry->privateLinksHeld) {
                    return false;
                }
                previousEntry->nextSibling = next;
            }
            current->nextSibling = 0;
            return true;
        }
        previous = cursor;
        cursor = current->nextSibling;
    }
    return false;
}

PlayableInstanceTable::Entry* PlayableInstanceTable::FindAnyMut(
    PlayableInstanceHandle handle) noexcept {
    const auto* entry = FindAny(handle);
    return entry == nullptr || entry->preparationCommitHeld
        ? nullptr : &m_entries[SlotOf(handle)];
}

PlayableInstanceHandle PlayableInstanceTable::Acquire(
    PlayableKind kind, PlayableInstanceHandle parent,
    const MonitorRequestContext& request, const MonitorEmitterOrigin& emitterOrigin) {
    // A named parent must still be live. Accepting a stale one would root the
    // new instance, quietly detaching it from a tree that has already gone.
    if (parent != 0 && (Find(parent) == nullptr || ParentLinksHeld(parent))) {
        return 0;
    }
    for (std::uint32_t slot = 0; slot < m_entries.size(); ++slot) {
        Entry& entry = m_entries[slot];
        if (!Reusable(entry.lifecycle) || entry.quarantined ||
            entry.prepared || entry.privateLinksHeld || entry.originalLifetimeCount != 0 ||
            entry.completedOriginalCancellation.owner_ != nullptr) {
            continue;
        }
        const IssueResult issue =
            TryIssue(&m_nextScheduleEpoch, &m_scheduleEpochExhausted);
        if (issue.status != IssueStatus::Ok) {
            return 0;
        }
        entry.kind = kind;
        entry.lifecycle = PlayableLifecycle::Active;
        entry.preparationCommitHeld = false;
        entry.preparingCommitOwned = false;
        entry.privateLinksHeld = false;
        entry.preparationInput = {};
        entry.parent = parent;
        entry.firstChild = 0;
        entry.nextSibling = 0;
        entry.scheduleEpoch = issue.value;
        entry.rootMetadata = {};
        entry.contentIdentity = {};
        entry.monitorPreparedCandidateIdentity = {};
        entry.monitorPreparedCandidateObserved = false;
        entry.playbackRequest = parent != 0 ? FindAny(parent)->playbackRequest : request;
        entry.emitterOrigin = parent != 0 ? FindAny(parent)->emitterOrigin : emitterOrigin;
        entry.controlCause = {};
        entry.controlCauseAccepted = false;
        entry.terminalOutcome = {};
        entry.cancellationPrepared = false;
        entry.preparedObserver = {};
        entry.retirementObserver = {};
        entry.delayedCancellationEpoch = 0;
        ++m_liveCount;
        if (m_liveCount > m_highWater) {
            m_highWater = m_liveCount;
        }
        const PlayableInstanceHandle handle =
            MakeHandle(slot, entry.generation);
        if (parent != 0 && !LinkChild(handle, parent)) {
            entry.lifecycle = PlayableLifecycle::Free;
            entry.parent = 0;
            entry.firstChild = 0;
            entry.nextSibling = 0;
            entry.scheduleEpoch = 0;
            entry.terminalOutcome = {};
            --m_liveCount;
            return 0;
        }
        MonitorLifecycleFacts facts{};
        FillMonitorFacts(entry, handle, facts);
        ObserveMonitorLifecycle(
            MonitorLifecyclePhase::PlayableStart, handle, entry.generation, 0,
            entry.contentIdentity.stableId, handle, 0,
            0, static_cast<std::uint8_t>(entry.kind), 0, facts);
        return handle;
    }
    return 0;
}

bool PlayableInstanceTable::PrepareAcquire(
    PlayableKind kind, PlayableInstanceHandle parent,
    PreparedPlayableInstance* out, const MonitorRequestContext& request, const MonitorEmitterOrigin& emitterOrigin) noexcept {
    if (out == nullptr || out->Valid() ||
        (out->owner_ != nullptr && out->owner_->ParentLinksHeld(out->Handle())) ||
        (parent != 0 && (FindAnyMut(parent) == nullptr || ParentLinksHeld(parent)))) {
        return false;
    }
    *out = {};
    for (std::uint32_t slot = 0; slot < m_entries.size(); ++slot) {
        Entry& entry = m_entries[slot];
        if (!Reusable(entry.lifecycle) || entry.quarantined ||
            entry.prepared || entry.privateLinksHeld || entry.originalLifetimeCount != 0 ||
            entry.completedOriginalCancellation.owner_ != nullptr) {
            continue;
        }
        const IssueResult issue =
            TryIssue(&m_nextScheduleEpoch, &m_scheduleEpochExhausted);
        if (issue.status != IssueStatus::Ok) {
            return false;
        }
        entry.prepared = true;
        entry.preparationCommitHeld = false;
        entry.preparingCommitOwned = true;
        entry.privateLinksHeld = false;
        entry.kind = kind;
        entry.parent = parent;
        entry.firstChild = 0;
        entry.nextSibling = 0;
        entry.scheduleEpoch = issue.value;
        entry.rootMetadata = {};
        entry.contentIdentity = {};
        entry.monitorPreparedCandidateIdentity = {};
        entry.monitorPreparedCandidateObserved = false;
        entry.playbackRequest = parent != 0 ? FindAny(parent)->playbackRequest : request;
        entry.emitterOrigin = parent != 0 ? FindAny(parent)->emitterOrigin : emitterOrigin;
        entry.controlCause = {};
        entry.controlCauseAccepted = false;
        entry.terminalOutcome = {};
        entry.cancellationPrepared = false;
        entry.preparedObserver = {};
        entry.retirementObserver = {};
        entry.delayedCancellationEpoch = 0;
        entry.preparationInput = {};
        entry.preparationInput.handle = MakeHandle(slot, entry.generation);
        entry.preparationInput.kind = kind;
        entry.preparationInput.lifecycle = entry.lifecycle;
        entry.preparationInput.parent = parent;
        entry.preparationInput.parentEpoch = parent == 0 ? 0 : FindAny(parent)->scheduleEpoch;
        entry.preparationInput.epoch = entry.scheduleEpoch;
        entry.preparationInput.metadata = entry.rootMetadata;
        entry.preparationInput.sourceHold = entry.sourceRetirementHold;
        out->owner_ = this;
        out->handle_ = MakeHandle(slot, entry.generation);
        out->parent_ = parent;
        out->scheduleEpoch_ = entry.scheduleEpoch;
        return true;
    }
    return false;
}

bool PlayableInstanceTable::SetPreparedRootMetadata(
    const PreparedPlayableInstance& prepared,
    const PlayableRootMetadata& metadata) noexcept {
    Entry* entry = FindPrepared(prepared);
    if (entry == nullptr) {
        return false;
    }
    entry->rootMetadata = metadata;
    const auto revision = entry->contentIdentity.stableId == metadata.definitionStableId &&
        entry->contentIdentity.worldGeneration == metadata.worldGeneration
            ? entry->contentIdentity.contentRevision : 0;
    entry->contentIdentity = {metadata.projectId, metadata.definitionStableId,
        metadata.worldGeneration, metadata.programSemanticFingerprint, revision};
    if (entry->preparingCommitOwned) entry->preparationInput.metadata = metadata;
    return true;
}

bool PlayableInstanceTable::SetPreparedContentIdentity(
    const PreparedPlayableInstance& prepared,
    const MonitorContentIdentity& identity) noexcept {
    Entry* entry = FindPrepared(prepared);
    if (entry == nullptr) return false;
    entry->contentIdentity = identity;
    const auto& previous = entry->monitorPreparedCandidateIdentity;
    if (entry->monitorPreparedCandidateObserved && previous.projectId == identity.projectId &&
        previous.stableId == identity.stableId && previous.worldGeneration == identity.worldGeneration &&
        previous.programSemanticFingerprint == identity.programSemanticFingerprint &&
        previous.contentRevision == identity.contentRevision) return true;
    // Root metadata can prefill content; deduplicate against the last observation.
    entry->monitorPreparedCandidateIdentity = identity;
    entry->monitorPreparedCandidateObserved = true;
    MonitorLifecycleFacts facts{};
    FillMonitorFacts(*entry, prepared.Handle(), facts);
    facts.publishLegacy = false;
    facts.reasonKnown = false;
    ObserveMonitorLifecycle(MonitorLifecyclePhase::PreparedCandidate, prepared.Handle(),
        entry->generation, identity.worldGeneration, identity.stableId, prepared.Handle(),
        facts.relatedAction, 0, static_cast<std::uint8_t>(entry->kind), 0, facts);
    return true;
}

bool PlayableInstanceTable::CaptureContentIdentity(
    PlayableInstanceHandle handle, MonitorContentIdentity* out) const noexcept {
    if (out == nullptr) return false;
    *out = {};
    const Entry* entry = FindAny(handle);
    if (entry == nullptr) return false;
    *out = entry->contentIdentity;
    return true;
}

bool PlayableInstanceTable::CaptureEmitterOrigin(
    PlayableInstanceHandle handle, MonitorEmitterOrigin* out) const noexcept {
    if (out == nullptr) return false;
    *out = {};
    const auto* entry = FindAny(handle);
    if (entry == nullptr) return false;
    *out = entry->emitterOrigin;
    return true;
}

bool PlayableInstanceTable::CapturePlaybackRequest(
    PlayableInstanceHandle handle, MonitorRequestContext* out) const noexcept {
    if (out == nullptr) return false;
    *out = {};
    const auto* entry = FindAny(handle);
    if (entry == nullptr) return false;
    *out = entry->playbackRequest;
    return true;
}

bool PlayableInstanceTable::CaptureControlCause(
    PlayableInstanceHandle handle, MonitorControlCause* out) const noexcept {
    if (out == nullptr) return false;
    *out = {};
    const auto* entry = FindAny(handle);
    if (entry == nullptr || !entry->controlCauseAccepted) return false;
    *out = entry->controlCause;
    return true;
}

void PlayableInstanceTable::RecordControlCause(
    PlayableInstanceHandle handle, const MonitorControlCause& cause) noexcept {
    const auto* original = FindAny(handle);
    if (original == nullptr || original->controlCauseAccepted ||
        original->terminalOutcome.kind != PlayableTerminalKind::None) return;
    // Observation is independent of the held preparation input and its proofs.
    auto& entry = m_entries[SlotOf(handle)];
    entry.controlCause = cause;
    entry.controlCauseAccepted = true;
}

PlayableInstanceTable::Entry* PlayableInstanceTable::FindPrepared(
    const PreparedPlayableInstance& prepared) noexcept {
    return OwnsPrepared(prepared) && !m_entries[SlotOf(prepared.handle_)].preparationCommitHeld &&
            !m_entries[SlotOf(prepared.handle_)].privateLinksHeld
        ? &m_entries[SlotOf(prepared.handle_)] : nullptr;
}

bool PlayableInstanceTable::CommitPrepared(
    PreparedPlayableInstance* prepared, bool publishParentLink) noexcept {
    if (prepared == nullptr) {
        return false;
    }
    Entry* entry = FindPrepared(*prepared);
    if (entry == nullptr) {
        return false;
    }
    if (publishParentLink && entry->parent != 0 &&
        !LinkChild(prepared->handle_, entry->parent)) {
        return false;
    }
    ActivatePreparedEntry(*entry, prepared->handle_);
    return true;
}

void PlayableInstanceTable::ActivatePreparedEntry(Entry& actual,
    PlayableInstanceHandle handle) noexcept {
    auto* entry = &actual;
    entry->prepared = false;
    entry->preparingCommitOwned = false;
    entry->lifecycle = PlayableLifecycle::Active;
    ++m_liveCount;
    if (m_liveCount > m_highWater) {
        m_highWater = m_liveCount;
    }
    const std::uint32_t generation = GenerationOf(handle);
    const PlayableKind kind = entry->kind;
    const VnObjectId stableId = entry->contentIdentity.stableId;
    MonitorLifecycleFacts facts{};
    FillMonitorFacts(*entry, handle, facts);
    ObserveMonitorLifecycle(MonitorLifecyclePhase::PlayableStart, handle,
                            generation, entry->contentIdentity.worldGeneration,
                            stableId, handle, entry->rootMetadata.actionFiring, 0,
                            static_cast<std::uint8_t>(kind), 0, facts);
}

bool PlayableInstanceTable::PublishPreparedLink(
    PlayableInstanceHandle handle) noexcept {
    Entry* entry = FindMut(handle);
    if (entry == nullptr || entry->lifecycle != PlayableLifecycle::Active ||
        entry->privateLinksHeld ||
        entry->parent == 0 || entry->nextSibling != 0) {
        return entry != nullptr && !entry->privateLinksHeld && entry->parent == 0;
    }
    return LinkChild(handle, entry->parent);
}

bool PlayableInstanceTable::CancelPrepared(
    PreparedPlayableInstance* prepared, OriginalLifetimePin* originalBaseline) noexcept {
    return CancelPreparedImpl(prepared, originalBaseline, false);
}

bool PlayableInstanceTable::CancelPreparedImpl(
    PreparedPlayableInstance* prepared, OriginalLifetimePin* originalBaseline,
    bool retainNativeReceipt) noexcept {
    if (prepared == nullptr) {
        return false;
    }
    if (prepared->owner_ == this && prepared->handle_ != 0 && SlotOf(prepared->handle_) < m_entries.size()) {
        auto& completed = m_entries[SlotOf(prepared->handle_)].completedOriginalCancellation;
        if (completed.owner_ == this && completed.handle_ == prepared->handle_ &&
            completed.parent_ == prepared->parent_ && completed.scheduleEpoch_ == prepared->scheduleEpoch_) {
            // Only the exact retained original alias receives this completion.
            // The slot cannot be reused until that native owner acknowledges it.
            completed = {};
            *prepared = {};
            return true;
        }
    }
    Entry* entry = FindPrepared(*prepared);
    if (entry == nullptr || !RequestOriginalLifetimeDisposition(*entry, prepared->Handle(),
            Entry::OriginalDisposition::PreparedCancel, originalBaseline, prepared)) return false;
    if (entry->sourceRetirementHold) return false;
    originalBaseline = ResolveOriginalLifetimeBaseline(*entry, prepared->Handle(), originalBaseline);
    if (!OriginalLifetimeRetirementReady(*entry, originalBaseline)) return false;
    if (retainNativeReceipt && entry->originalLifetimeCount != 0) entry->completedOriginalCancellation = *prepared;
    if (originalBaseline != nullptr) originalBaseline->Release();
    if (entry->cancellationPrepared && entry->delayedCancellationEpoch == 0) {
        assert(m_preparedCancellationCount != 0);
        --m_preparedCancellationCount;
    }
    const auto nextGeneration = NextGeneration(entry->generation);
    if (nextGeneration == 0) {
        entry->quarantined = true;
        ++m_quarantinedCount;
    } else {
        entry->generation = nextGeneration;
    }
    entry->prepared = false;
    entry->preparingCommitOwned = false;
    entry->preparationInput = {};
    entry->parent = 0;
    entry->firstChild = 0;
    entry->nextSibling = 0;
    entry->scheduleEpoch = 0;
    entry->rootMetadata = {};
    entry->contentIdentity = {};
    entry->monitorPreparedCandidateIdentity = {};
    entry->monitorPreparedCandidateObserved = false;
    entry->playbackRequest = {};
    entry->emitterOrigin = {};
    entry->controlCause = {};
    entry->controlCauseAccepted = false;
    entry->terminalOutcome = {};
    entry->cancellationPrepared = false;
    entry->preparedObserver = {};
    entry->retirementObserver = {};
    entry->delayedCancellationEpoch = 0;
    *prepared = {};
    return true;
}

bool PlayableInstanceTable::WithdrawUnpublishedParentLink(
    const PreparedPlayableInstance& child, PlayableInstanceHandle parent,
    std::uint64_t expectedParentEpoch) noexcept {
    if (child.owner_ != this || child.handle_ == 0 || child.scheduleEpoch_ == 0 ||
        parent == 0 || expectedParentEpoch == 0) return false;
    const auto* entry = FindMut(child.handle_);
    const auto* parentEntry = FindMut(parent);
    if (entry == nullptr || parentEntry == nullptr ||
        entry->privateLinksHeld || parentEntry->privateLinksHeld ||
        entry->scheduleEpoch != child.scheduleEpoch_ || entry->parent != parent ||
        entry->lifecycle != PlayableLifecycle::Active ||
        parentEntry->scheduleEpoch != expectedParentEpoch) return false;
    std::size_t visited = 0;
    for (auto sibling = parentEntry->firstChild; sibling != 0;) {
        if (visited++ >= m_entries.size()) return false;
        if (sibling == child.handle_) return UnlinkChild(child.handle_, parent);
        const auto* siblingEntry = FindAny(sibling);
        if (siblingEntry == nullptr) return false;
        sibling = siblingEntry->nextSibling;
    }
    // A retained private child can already be withdrawn by a prior cleanup pass.
    return entry->nextSibling == 0;
}

bool PlayableInstanceTable::RollbackCommitted(
    PlayableInstanceHandle handle) noexcept {
    Entry* entry = FindMut(handle);
    if (entry == nullptr || entry->lifecycle != PlayableLifecycle::Active ||
        entry->privateLinksHeld || entry->originalLifetimeEnding ||
        entry->firstChild != 0 || entry->sourceRetirementHold) {
        return false;
    }
    const PlayableInstanceHandle parent = entry->parent;
    if (parent != 0) {
        // A shared tree executor may commit the instance state before it
        // publishes parent links. In that window the child is active but is
        // intentionally absent from the parent's intrusive list.
        bool linked = false;
        const Entry* parentEntry = FindAny(parent);
        std::size_t visited = 0;
        for (auto child = parentEntry != nullptr ? parentEntry->firstChild : 0;
             child != 0;) {
            if (visited++ >= m_entries.size()) return false;
            if (child == handle) {
                linked = true;
                break;
            }
            const Entry* childEntry = FindAny(child);
            child = childEntry != nullptr ? childEntry->nextSibling : 0;
        }
        if (linked && !UnlinkChild(handle, parent)) {
            return false;
        }
    }
    // A successful native rollback supersedes a not-yet-ending Retired request.
    // Failed Source/child/unlink preflights leave that authentic request intact.
    entry->originalDisposition = Entry::OriginalDisposition::None;
    entry->originalDispositionPrepared = {};
    entry->prepared = true;
    entry->lifecycle = PlayableLifecycle::Free;
    if (entry->delayedCancellationEpoch != 0) {
        entry->cancellationPrepared = false;
        entry->preparedObserver = {};
        entry->retirementObserver = {};
        entry->delayedCancellationEpoch = 0;
    }
    entry->firstChild = 0;
    entry->nextSibling = 0;
    --m_liveCount;
    return true;
}

void PlayableInstanceTable::FillMonitorFacts(const Entry& entry,
    PlayableInstanceHandle handle, MonitorLifecycleFacts& out) const noexcept {
    out.playbackRequest = entry.playbackRequest;
    out.emitterOrigin = entry.emitterOrigin;
    out.controlCause = entry.controlCause;
    out.projectId = entry.contentIdentity.projectId;
    out.programSemanticFingerprint = entry.contentIdentity.programSemanticFingerprint;
    out.contentRevision = entry.contentIdentity.contentRevision;
    out.contentObject = entry.contentIdentity.stableId;
    out.contentWorld = entry.contentIdentity.worldGeneration;
    out.contentKind = static_cast<std::uint8_t>(entry.kind);
    out.parentPlayable = entry.parent;
    out.ownerEpoch = entry.scheduleEpoch;
    const Entry* ancestor = &entry;
    auto cursor = handle;
    for (std::size_t depth = 0; depth < m_entries.size(); ++depth) {
        if (ancestor->rootMetadata.actionFiring != 0 && out.relatedAction == 0) {
            out.relatedAction = ancestor->rootMetadata.actionFiring;
            out.selectionCause.actionFiring = ancestor->rootMetadata.actionFiring;
            out.selectionCause.executionOrdinal = ancestor->rootMetadata.actionExecutionOrdinal;
        }
        if (ancestor->parent == 0) {
            out.root = cursor;
            return;
        }
        cursor = ancestor->parent;
        ancestor = FindAny(cursor);
        if (ancestor == nullptr) break;
    }
    out.identityObservationIncomplete = true;
}

PlayableInstanceHandle PlayableInstanceTable::CaptureMonitorIdentityAt(
    std::uint32_t slot, MonitorLifecycleFacts* out) const noexcept {
    if (out == nullptr) return 0;
    *out = {};
    if (slot >= m_entries.size()) return 0;
    const auto& entry = m_entries[slot];
    const auto handle = MakeHandle(slot, entry.generation);
    if (Find(handle) == nullptr) return 0;
    FillMonitorFacts(entry, handle, *out);
    out->reasonKnown = false;
    return handle;
}

bool PlayableInstanceTable::CaptureMonitorFacts(PlayableInstanceHandle handle,
    MonitorLifecycleFacts* out) const noexcept {
    if (out == nullptr) return false;
    *out = {};
    const auto* entry = FindAny(handle);
    if (entry == nullptr) return false;
    FillMonitorFacts(*entry, handle, *out);
    return true;
}

bool PlayableInstanceTable::CaptureRootMetadata(
    PlayableInstanceHandle handle, PlayableRootMetadata* out) const noexcept {
    if (out == nullptr) {
        return false;
    }
    *out = {};
    const Entry* entry = FindAny(handle);
    if (entry == nullptr) {
        return false;
    }
    *out = entry->rootMetadata;
    return true;
}

bool PlayableInstanceTable::PrepareCancellation(
    PlayableInstanceHandle handle, std::uint64_t observerActionFiring,
    std::uint32_t observerExecutionOrdinal,
    PreparedPlayableCancellation* out) noexcept {
    if (out == nullptr || observerActionFiring == 0 ||
        observerExecutionOrdinal == UINT32_MAX || m_scheduleEpochExhausted ||
        m_nextScheduleEpoch == 0 ||
        m_preparedCancellationCount > UINT64_MAX - m_nextScheduleEpoch) {
        return false;
    }
    *out = {};
    Entry* entry = FindAnyMut(handle);
    if (entry == nullptr || entry->rootMetadata.actionFiring == 0 ||
        entry->privateLinksHeld ||
        entry->cancellationPrepared ||
        entry->retirementObserver.actionFiring != 0) {
        return false;
    }
    entry->cancellationPrepared = true;
    entry->preparedObserver = {observerActionFiring,
                               observerExecutionOrdinal};
    ++m_preparedCancellationCount;
    out->owner_ = this;
    out->handle_ = handle;
    out->observer_ = entry->preparedObserver;
    return true;
}

bool PlayableInstanceTable::IsCancellationPrepared(
    const PreparedPlayableCancellation& prepared) const noexcept {
    if (prepared.owner_ != this) {
        return false;
    }
    const Entry* entry = FindAny(prepared.handle_);
    return entry != nullptr && entry->cancellationPrepared &&
           entry->delayedCancellationEpoch == 0 &&
           entry->preparedObserver.actionFiring ==
               prepared.observer_.actionFiring &&
           entry->preparedObserver.actionExecutionOrdinal ==
               prepared.observer_.actionExecutionOrdinal;
}

bool PlayableInstanceTable::CommitPreparedCancellation(
    PreparedPlayableCancellation* prepared, const MonitorControlCause* cause) noexcept {
    if (prepared == nullptr || !IsCancellationPrepared(*prepared)) {
        return false;
    }
    Entry* entry = FindAnyMut(prepared->handle_);
    if (entry == nullptr || entry->privateLinksHeld) {
        return false;
    }
    const IssueResult issue =
        TryIssue(&m_nextScheduleEpoch, &m_scheduleEpochExhausted);
    if (issue.status != IssueStatus::Ok) {
        return false;
    }
    if (cause != nullptr) RecordControlCause(prepared->handle_, *cause);
    entry->scheduleEpoch = issue.value;
    entry->cancellationPrepared = false;
    entry->retirementObserver = entry->preparedObserver;
    entry->preparedObserver = {};
    --m_preparedCancellationCount;
    *prepared = {};
    return true;
}

bool PlayableInstanceTable::CancelPreparedCancellation(
    PreparedPlayableCancellation* prepared) noexcept {
    if (prepared == nullptr || !IsCancellationPrepared(*prepared)) {
        return false;
    }
    Entry* entry = FindAnyMut(prepared->handle_);
    if (entry == nullptr || entry->privateLinksHeld) {
        return false;
    }
    entry->cancellationPrepared = false;
    entry->preparedObserver = {};
    --m_preparedCancellationCount;
    *prepared = {};
    return true;
}

bool PlayableInstanceTable::PrepareDelayedCancellation(
    PlayableInstanceHandle handle, std::uint64_t observerActionFiring,
    std::uint32_t observerExecutionOrdinal,
    DelayedPlayableCancellation* out) noexcept {
    if (out == nullptr || out->owner_ != nullptr || observerActionFiring == 0 ||
        observerExecutionOrdinal == UINT32_MAX || m_scheduleEpochExhausted ||
        m_nextScheduleEpoch == 0 ||
        m_preparedCancellationCount > UINT64_MAX - m_nextScheduleEpoch) {
        return false;
    }
    Entry* entry = FindAnyMut(handle);
    if (entry == nullptr || entry->rootMetadata.actionFiring == 0 ||
        entry->privateLinksHeld ||
        entry->cancellationPrepared || entry->retirementObserver.actionFiring != 0 ||
        entry->delayedCancellationEpoch != 0) {
        return false;
    }
    // Existing Immediate reservations retain their unissued epoch capacity.
    const IssueResult issue = TryIssue(&m_nextScheduleEpoch, &m_scheduleEpochExhausted);
    if (issue.status != IssueStatus::Ok) return false;
    entry->cancellationPrepared = true;
    entry->preparedObserver = {observerActionFiring, observerExecutionOrdinal};
    entry->delayedCancellationEpoch = issue.value;
    out->owner_ = this;
    out->handle_ = handle;
    out->sourceEpoch_ = entry->scheduleEpoch;
    out->issuedEpoch_ = issue.value;
    out->observer_ = entry->preparedObserver;
    return true;
}

bool PlayableInstanceTable::OwnsDelayedCancellation(
    const DelayedPlayableCancellation& cancellation,
    bool requireSourceEpoch) const noexcept {
    if (cancellation.owner_ != this || cancellation.issuedEpoch_ == 0) return false;
    const Entry* entry = FindAny(cancellation.handle_);
    if (entry == nullptr || entry->delayedCancellationEpoch != cancellation.issuedEpoch_ ||
        entry->cancellationPrepared == cancellation.accepted_ ||
        (requireSourceEpoch && entry->scheduleEpoch != cancellation.sourceEpoch_)) {
        return false;
    }
    const auto& observer = cancellation.accepted_
        ? entry->retirementObserver : entry->preparedObserver;
    return observer.actionFiring == cancellation.observer_.actionFiring &&
           observer.actionExecutionOrdinal == cancellation.observer_.actionExecutionOrdinal;
}

bool PlayableInstanceTable::CommitPreparedDelayedCancellation(
    DelayedPlayableCancellation* cancellation) noexcept {
    if (cancellation == nullptr || cancellation->accepted_ ||
        !OwnsDelayedCancellation(*cancellation)) return false;
    Entry* entry = FindAnyMut(cancellation->handle_);
    if (entry == nullptr || entry->privateLinksHeld) return false;
    entry->cancellationPrepared = false;
    entry->retirementObserver = entry->preparedObserver;
    entry->preparedObserver = {};
    cancellation->accepted_ = true;
    return true;
}

bool PlayableInstanceTable::ActivateDelayedCancellation(
    DelayedPlayableCancellation* cancellation, const MonitorControlCause* cause) noexcept {
    if (cancellation == nullptr || !cancellation->accepted_ ||
        !OwnsDelayedCancellation(*cancellation)) return false;
    Entry* entry = FindMut(cancellation->handle_);
    if (entry == nullptr || entry->privateLinksHeld) return false;
    if (cause != nullptr) RecordControlCause(cancellation->handle_, *cause);
    entry->scheduleEpoch = cancellation->issuedEpoch_;
    entry->delayedCancellationEpoch = 0;
    MonitorLifecycleFacts facts{};
    FillMonitorFacts(*entry, cancellation->handle_, facts);
    ObserveMonitorLifecycle(
        MonitorLifecyclePhase::PlayableCancel, cancellation->handle_, entry->generation,
        entry->contentIdentity.worldGeneration, entry->contentIdentity.stableId,
        cancellation->handle_, entry->rootMetadata.actionFiring, 0,
        static_cast<std::uint8_t>(entry->kind), 0, facts);
    cancellation->Clear();
    return true;
}

bool PlayableInstanceTable::CancelDelayedCancellation(
    DelayedPlayableCancellation* cancellation) noexcept {
    if (cancellation == nullptr || cancellation->owner_ != this) return false;
    const bool owned = OwnsDelayedCancellation(*cancellation, false);
    if (owned) {
        Entry* entry = FindAnyMut(cancellation->handle_);
        if (entry == nullptr || entry->privateLinksHeld) return false;
        if (cancellation->accepted_) {
            entry->retirementObserver = {};
        } else {
            entry->cancellationPrepared = false;
            entry->preparedObserver = {};
        }
        entry->delayedCancellationEpoch = 0;
    }
    cancellation->Clear();
    return owned;
}

bool PlayableInstanceTable::HasCancellationClaim(
    PlayableInstanceHandle handle) const noexcept {
    const Entry* entry = FindAny(handle);
    return entry != nullptr &&
           (entry->cancellationPrepared ||
            entry->retirementObserver.actionFiring != 0);
}

bool PlayableInstanceTable::CaptureRetirementObserver(
    PlayableInstanceHandle handle,
    PlayableRetirementObserver* out) const noexcept {
    if (out == nullptr) {
        return false;
    }
    *out = {};
    const Entry* entry = Find(handle);
    if (entry == nullptr || entry->retirementObserver.actionFiring == 0) {
        return false;
    }
    *out = entry->retirementObserver;
    return true;
}

bool PlayableInstanceTable::DetachRetirementObserver(
    PlayableInstanceHandle handle,
    const PlayableRetirementObserver& expected) noexcept {
    Entry* entry = FindMut(handle);
    if (entry == nullptr || expected.actionFiring == 0 ||
        entry->privateLinksHeld ||
        entry->retirementObserver.actionFiring != expected.actionFiring ||
        entry->retirementObserver.actionExecutionOrdinal !=
            expected.actionExecutionOrdinal) {
        return false;
    }
    entry->retirementObserver = {};
    entry->delayedCancellationEpoch = 0;
    return true;
}

bool PlayableInstanceTable::TakeRetirementObserver(
    PlayableInstanceHandle handle,
    PlayableRetirementObserver* out) noexcept {
    if (out == nullptr) {
        return false;
    }
    *out = {};
    Entry* entry = FindMut(handle);
    if (entry == nullptr || entry->privateLinksHeld || entry->retirementObserver.actionFiring == 0) {
        return false;
    }
    *out = entry->retirementObserver;
    entry->retirementObserver = {};
    entry->delayedCancellationEpoch = 0;
    return true;
}

PlayableInstanceView PlayableInstanceTable::Classify(
    PlayableInstanceHandle handle) const {
    PlayableInstanceView view;
    const Entry* entry = Find(handle);
    if (entry == nullptr) {
        return view;
    }
    view.live = true;
    view.kind = entry->kind;
    view.lifecycle = entry->lifecycle;
    view.parent = entry->parent;
    view.firstChild = entry->firstChild;
    view.nextSibling = entry->nextSibling;
    view.scheduleEpoch = entry->scheduleEpoch;
    view.terminalOutcome = entry->terminalOutcome;
    return view;
}

PlayableInstanceHandle PlayableInstanceTable::FirstChild(
    PlayableInstanceHandle handle) const noexcept {
    const Entry* entry = Find(handle);
    return entry == nullptr ? 0 : entry->firstChild;
}

PlayableInstanceHandle PlayableInstanceTable::NextSibling(
    PlayableInstanceHandle handle) const noexcept {
    const Entry* entry = Find(handle);
    return entry == nullptr ? 0 : entry->nextSibling;
}

bool PlayableInstanceTable::SetTerminalOutcome(
    PlayableInstanceHandle handle, PlayableTerminalOutcome outcome) noexcept {
    if (outcome.kind == PlayableTerminalKind::None) {
        return false;
    }
    Entry* entry = FindMut(handle);
    if (entry == nullptr || entry->terminalOutcome.kind !=
                                PlayableTerminalKind::None) {
        return false;
    }
    if (entry->privateLinksHeld) return false;
    entry->terminalOutcome = outcome;
    return true;
}

bool PlayableInstanceTable::AcquireSourceRetirementHold(
    PlayableInstanceHandle handle, std::uint64_t acceptedEpoch) noexcept {
    Entry* entry = FindAnyMut(handle);
    if (entry == nullptr || entry->kind != PlayableKind::Sound ||
        acceptedEpoch == 0 || entry->scheduleEpoch != acceptedEpoch ||
        entry->sourceRetirementHold) return false;
    entry->sourceRetirementHold = true;
    if (entry->prepared && entry->preparingCommitOwned)
        entry->preparationInput.sourceHold = true;
    return true;
}

std::uint64_t PlayableInstanceTable::SourceRetirementEpoch(
    PlayableInstanceHandle handle) const noexcept {
    const Entry* entry = FindAny(handle);
    return entry != nullptr && entry->kind == PlayableKind::Sound
        ? entry->scheduleEpoch : 0;
}

std::uint32_t PlayableInstanceTable::SourceRetirementSlot(
    PlayableInstanceHandle handle) const noexcept {
    const Entry* entry = FindAny(handle);
    return entry != nullptr && entry->sourceRetirementHold ? SlotOf(handle) : UINT32_MAX;
}

bool PlayableInstanceTable::ReleaseSourceRetirementHold(
    PlayableInstanceHandle handle) noexcept {
    Entry* entry = FindAnyMut(handle);
    if (entry == nullptr || !entry->sourceRetirementHold) return false;
    entry->sourceRetirementHold = false;
    if (entry->prepared && entry->preparingCommitOwned)
        entry->preparationInput.sourceHold = false;
    return true;
}

bool PlayableInstanceTable::HasSourceRetirementHold(
    PlayableInstanceHandle handle) const noexcept {
    const Entry* entry = FindAny(handle);
    return entry != nullptr && entry->sourceRetirementHold;
}

bool PlayableInstanceTable::CanUnlinkChild(PlayableInstanceHandle child,
    PlayableInstanceHandle parent) const noexcept {
    if (child == 0 || parent == 0) return false;
    const auto* parentEntry = FindAny(parent);
    if (parentEntry == nullptr || parentEntry->preparationCommitHeld || parentEntry->privateLinksHeld ||
        ParentLinksHeld(child)) return false;
    const Entry* previous = nullptr;
    auto cursor = parentEntry->firstChild;
    std::size_t visited = 0;
    while (cursor != 0) {
        if (visited++ >= m_entries.size()) return false;
        const auto* current = FindAny(cursor);
        if (current == nullptr || current->preparationCommitHeld || current->parent != parent) return false;
        if (cursor == child)
            return !current->privateLinksHeld && (previous == nullptr ||
                (!previous->privateLinksHeld && !previous->preparationCommitHeld));
        previous = current;
        cursor = current->nextSibling;
    }
    return false;
}

bool PlayableInstanceTable::AdvanceLifecycle(PlayableInstanceHandle handle,
                                             PlayableLifecycle to, OriginalLifetimePin* originalBaseline) {
    Entry* entry = FindMut(handle);
    if (entry == nullptr || entry->privateLinksHeld) {
        return false;
    }
    // Strictly forward only. Free is not a destination: a slot leaves the live
    // set through Retired, which is also where identity ends.
    if (to == PlayableLifecycle::Free ||
        static_cast<std::uint8_t>(to) <=
            static_cast<std::uint8_t>(entry->lifecycle)) {
        return false;
    }
    const PlayableInstanceHandle parent = entry->parent;
    if (to == PlayableLifecycle::Retired) {
        if (entry->originalLifetimeRetirementHeld) return false;
        if (!RequestOriginalLifetimeDisposition(*entry, handle, Entry::OriginalDisposition::Retired, originalBaseline))
            return false;
        if (entry->firstChild != 0 || entry->sourceRetirementHold ||
            (parent != 0 && !CanUnlinkChild(handle, parent))) return false;
        originalBaseline = ResolveOriginalLifetimeBaseline(*entry, handle, originalBaseline);
        if (!OriginalLifetimeRetirementReady(*entry, originalBaseline)) return false;
    }
    if (to == PlayableLifecycle::Retired && parent != 0 &&
        !UnlinkChild(handle, parent)) {
        return false;
    }
    if (to == PlayableLifecycle::Retired && originalBaseline != nullptr) originalBaseline->Release();
    const PlayableKind kind = entry->kind;
    const std::uint32_t generation = entry->generation;
    const VnObjectId stableId = entry->contentIdentity.stableId;
    entry->lifecycle = to;
    MonitorLifecyclePhase phase = MonitorLifecyclePhase::PlayableStart;
    if (to == PlayableLifecycle::LogicalComplete) {
        phase = MonitorLifecyclePhase::PlayableLogicalComplete;
    } else if (to == PlayableLifecycle::AudibleRetiring) {
        phase = MonitorLifecyclePhase::PlayableAudibleRetiring;
    } else if (to == PlayableLifecycle::Retired) {
        phase = MonitorLifecyclePhase::PlayableRetired;
    }
    MonitorLifecycleFacts facts{};
    FillMonitorFacts(*entry, handle, facts);
    ObserveMonitorLifecycle(phase, handle, generation, entry->contentIdentity.worldGeneration,
                            stableId, handle, entry->rootMetadata.actionFiring, 0,
                            static_cast<std::uint8_t>(kind), 0, facts);
    if (to == PlayableLifecycle::Retired) {
        // One generation move per lifetime, taken here: handles to this
        // instance are stale from now on and the slot is reusable unless the
        // generation is exhausted, with no second step that could be forgotten.
        const std::uint32_t nextGeneration =
            NextGeneration(entry->generation);
        if (nextGeneration == 0) {
            entry->quarantined = true;
            ++m_quarantinedCount;
        } else {
            entry->generation = nextGeneration;
        }
        entry->parent = 0;
        entry->firstChild = 0;
        entry->nextSibling = 0;
        entry->scheduleEpoch = 0;
        entry->rootMetadata = {};
        entry->contentIdentity = {};
        entry->monitorPreparedCandidateIdentity = {};
        entry->monitorPreparedCandidateObserved = false;
        entry->playbackRequest = {};
        entry->emitterOrigin = {};
        entry->controlCause = {};
        entry->controlCauseAccepted = false;
        entry->preparingCommitOwned = false;
        entry->privateLinksHeld = false;
        entry->preparationInput = {};
        entry->terminalOutcome = {};
        entry->cancellationPrepared = false;
        entry->preparedObserver = {};
        entry->retirementObserver = {};
        entry->delayedCancellationEpoch = 0;
        --m_liveCount;
    }
    return true;
}

std::uint32_t PlayableInstanceTable::FreeCount() const {
    std::uint32_t free = 0;
    for (const Entry& entry : m_entries) {
        if (!entry.quarantined && !entry.prepared && entry.originalLifetimeCount == 0 &&
            entry.completedOriginalCancellation.owner_ == nullptr &&
            Reusable(entry.lifecycle)) {
            ++free;
        }
    }
    return free;
}

IssueResult PlayableInstanceTable::BumpScheduleEpochChecked(
    PlayableInstanceHandle handle, const MonitorControlCause* cause) {
    Entry* entry = FindMut(handle);
    if (entry == nullptr || entry->privateLinksHeld) {
        return {};
    }
    const IssueResult issue =
        TryIssue(&m_nextScheduleEpoch, &m_scheduleEpochExhausted);
    if (issue.status != IssueStatus::Ok) {
        return issue;
    }
    if (cause != nullptr) RecordControlCause(handle, *cause);
    entry->scheduleEpoch = issue.value;
    MonitorLifecycleFacts facts{};
    FillMonitorFacts(*entry, handle, facts);
    ObserveMonitorLifecycle(
        MonitorLifecyclePhase::PlayableCancel, handle, entry->generation,
        entry->contentIdentity.worldGeneration, entry->contentIdentity.stableId,
        handle, entry->rootMetadata.actionFiring, 0,
        static_cast<std::uint8_t>(entry->kind), 0, facts);
    return issue;
}

bool PlayableInstanceTable::CanBumpScheduleEpoch(
    PlayableInstanceHandle handle) const {
    return Find(handle) != nullptr && !ParentLinksHeld(handle) && !m_scheduleEpochExhausted &&
           m_nextScheduleEpoch != 0;
}

bool PlayableInstanceTable::CanBumpScheduleEpochs(
    std::uint32_t count) const {
    if (count == 0) {
        return true;
    }
    return !m_scheduleEpochExhausted && m_nextScheduleEpoch != 0 &&
           static_cast<std::uint64_t>(count - 1u) <=
               UINT64_MAX - m_nextScheduleEpoch;
}

bool PlayableInstanceTable::ValidateStamp(PlayableInstanceHandle handle,
                                         std::uint64_t scheduleEpoch) const {
    const Entry* entry = Find(handle);
    return entry != nullptr && entry->scheduleEpoch == scheduleEpoch;
}

bool PlayableInstanceTable::IsPrepared(
    PlayableInstanceHandle handle) const noexcept {
    if (handle == 0) {
        return false;
    }
    const std::uint32_t slot = SlotOf(handle);
    return slot < m_entries.size() && m_entries[slot].prepared &&
           !m_entries[slot].quarantined &&
           m_entries[slot].generation == GenerationOf(handle);
}

bool PlayableInstanceTable::OwnsPrepared(
    const PreparedPlayableInstance& prepared) const noexcept {
    if (prepared.owner_ != this || prepared.handle_ == 0 ||
        prepared.scheduleEpoch_ == 0) {
        return false;
    }
    const std::uint32_t slot = SlotOf(prepared.handle_);
    const std::uint32_t generation = GenerationOf(prepared.handle_);
    if (slot >= m_entries.size() || generation == 0) {
        return false;
    }
    const Entry& entry = m_entries[slot];
    return entry.prepared && !entry.quarantined &&
           entry.generation == generation &&
           entry.scheduleEpoch == prepared.scheduleEpoch_;
}

} // namespace harken
