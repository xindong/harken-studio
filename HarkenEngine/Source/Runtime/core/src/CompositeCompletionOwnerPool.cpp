#include "harken/core/CompositeCompletionOwnerPool.h"
#include "harken/core/CheckedIssuer.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <utility>

namespace harken {
namespace {

bool SameToken(OccurrenceToken a, OccurrenceToken b) noexcept {
    return a.slot == b.slot && a.generation == b.generation;
}

bool ValidLeaf(const KnownScheduleLeafV1& leaf,
               OccurrenceStatePool& occurrences) noexcept {
    if (leaf.stableLeafOrdinal == 0 ||
        leaf.authoredOffset.denominator == 0 ||
        leaf.occurrence.generation == 0 || leaf.renderGeneration == 0 ||
        !SameToken(leaf.occurrence, leaf.predicate.occurrence) ||
        leaf.renderGeneration != leaf.predicate.renderGeneration ||
        leaf.absoluteLeafStartFrame !=
            leaf.predicate.absoluteLogicalStartFrame ||
        leaf.predicate.playbackFactsRevision == 0 ||
        leaf.predicate.totalLogicalOutputFrames == 0 ||
        leaf.predicate.absoluteLogicalCompleteFrame <
            leaf.predicate.absoluteLogicalStartFrame ||
        leaf.predicate.absoluteLogicalCompleteFrame -
                leaf.predicate.absoluteLogicalStartFrame !=
            leaf.predicate.totalLogicalOutputFrames) {
        return false;
    }
    const CompletionProgressRow* row =
        occurrences.CompletionProgressOf(leaf.occurrence);
    CompletionProgressSnapshot progress{};
    return row != nullptr &&
           CaptureCompletionProgressExact(*row, leaf.occurrence,
                                          leaf.renderGeneration,
                                          &progress) &&
           progress.playbackFactsRevision ==
               leaf.predicate.playbackFactsRevision;
}

} // namespace

CompositeCompletionOwnerPool::CompositeCompletionOwnerPool(
    std::uint32_t capacity, std::uint32_t maxLeaves) {
    constexpr std::uint32_t kHardMaxCapacity = 16384;
    constexpr std::uint32_t kHardMaxLeaves = 64;
    const std::size_t max =
        (std::numeric_limits<std::size_t>::max)();
    if (capacity > kHardMaxCapacity || maxLeaves == 0 ||
        maxLeaves > kHardMaxLeaves ||
        capacity > max / maxLeaves) {
        return;
    }
    capacity_ = capacity;
    maxLeaves_ = maxLeaves;
    rows_ = std::vector<Row>(capacity);
    leaves_ = std::vector<KnownScheduleLeafV1>(
        static_cast<std::size_t>(capacity) * maxLeaves);
    leafOriginals_ = std::vector<OccurrenceStatePool::OriginalCompositeCompletionOwnerPin>(
        static_cast<std::size_t>(capacity) * maxLeaves);
    leafCommitProofs_ = std::vector<OccurrenceStatePool::PreparedCompositeCompletionCommitProof>(
        static_cast<std::size_t>(capacity) * maxLeaves);
}

CompositeCompletionOwnerPool::~CompositeCompletionOwnerPool() {
    for (const Row& row : rows_) assert(row.originalPinCount == 0);
}

CompositeCompletionOwnerPool::OriginalLifetimePin::OriginalLifetimePin(
    OriginalLifetimePin&& other) noexcept { *this = std::move(other); }

CompositeCompletionOwnerPool::OriginalLifetimePin&
CompositeCompletionOwnerPool::OriginalLifetimePin::operator=(OriginalLifetimePin&& other) noexcept {
    if (this != &other) {
        Release();
        owner_ = std::exchange(other.owner_, nullptr);
        token_ = std::exchange(other.token_, CompositeCompletionOwnerToken{});
        marker_ = std::exchange(other.marker_, 0);
        baseline_ = std::exchange(other.baseline_, false);
    }
    return *this;
}

bool CompositeCompletionOwnerPool::OriginalLifetimePin::Valid() const noexcept {
    return owner_ != nullptr && owner_->MatchesOriginalLifetime(*this);
}

bool CompositeCompletionOwnerPool::OriginalLifetimePin::Borrow(OriginalLifetimePin* out) const noexcept {
    return owner_ != nullptr && owner_->BorrowOriginalLifetime(*this, out);
}

void CompositeCompletionOwnerPool::OriginalLifetimePin::Release() noexcept {
    if (owner_ != nullptr) owner_->ReleaseOriginalLifetime(*this);
}

bool CompositeCompletionOwnerPool::MatchesOriginalLifetime(const OriginalLifetimePin& original) const noexcept {
    if (original.owner_ != this || original.marker_ == 0 || !original.token_.Valid() ||
        original.token_.slot >= capacity_) return false;
    const auto& row = rows_[original.token_.slot];
    return row.generation == original.token_.generation && row.originalMarker == original.marker_ &&
        row.originalPinCount != 0 && row.state.load(std::memory_order_acquire) != State::Free;
}

bool CompositeCompletionOwnerPool::IsOriginalLifetimePinFor(const OriginalLifetimePin& original,
    CompositeCompletionOwnerToken token) const noexcept {
    return MatchesOriginalLifetime(original) && original.token_.slot == token.slot &&
        original.token_.generation == token.generation;
}

bool CompositeCompletionOwnerPool::IsOriginalLifetimePinned(const OriginalLifetimePin& original) const noexcept {
    return MatchesOriginalLifetime(original) &&
        rows_[original.token_.slot].state.load(std::memory_order_acquire) == State::Pinned;
}

bool CompositeCompletionOwnerPool::CaptureOriginal(const OriginalLifetimePin& original,
    CompositeCompletionOwnerInfo* out) const noexcept {
    if (out == nullptr || !MatchesOriginalLifetime(original)) return false;
    const auto& row = rows_[original.token_.slot];
    *out = {row.composedOccurrence, row.aggregateRevision, row.leafCount, row.absoluteLogicalCompleteFrame};
    return true;
}

bool CompositeCompletionOwnerPool::CaptureOriginalLeaf(const OriginalLifetimePin& original,
    std::uint32_t index, KnownScheduleLeafV1* out) const noexcept {
    if (out == nullptr || !MatchesOriginalLifetime(original) ||
        index >= rows_[original.token_.slot].leafCount) return false;
    *out = LeafStorage(original.token_.slot)[index];
    return true;
}

bool CompositeCompletionOwnerPool::BorrowOriginalLeafCompletion(const OriginalLifetimePin& original,
    std::uint32_t index, OccurrenceStatePool::OriginalCompositeCompletionOwnerPin* out) noexcept {
    if (!IsOriginalLifetimePinned(original) || index >= rows_[original.token_.slot].leafCount) return false;
    return leafOriginals_[static_cast<std::size_t>(original.token_.slot) * maxLeaves_ + index].Borrow(out);
}

bool CompositeCompletionOwnerPool::PrepareOriginalLeafCompletionCommits(
    const OriginalLifetimePin& original, OccurrenceStatePool& occurrences) noexcept {
    if (!original.baseline_ || !MatchesOriginalLifetime(original)) return false;
    auto& row = rows_[original.token_.slot];
    if (row.state.load(std::memory_order_acquire) != State::Reserved ||
        row.originalOccurrenceReceiver != &occurrences || row.originalLeafCommitsPrepared) return false;
    const auto* aggregate = occurrences.Find(row.composedOccurrence);
    if (aggregate == nullptr || !aggregate->preparedReservation || aggregate->stage != OccurrenceStage::Preparing)
        return false;
    const auto base = static_cast<std::size_t>(original.token_.slot) * maxLeaves_;
    const auto* leaves = LeafStorage(original.token_.slot);
    for (std::uint32_t index = 0; index < row.leafCount; ++index) {
        const auto& leaf = leaves[index];
        const auto* entry = occurrences.Find(leaf.occurrence);
        CompletionOwnerRef existing{};
        if (!ValidLeaf(leaf, occurrences) || entry == nullptr || !entry->preparedReservation ||
            entry->stage != OccurrenceStage::Preparing || entry->originalSinglePreparedCommitHeld ||
            leafOriginals_[base + index].Valid() || leafCommitProofs_[base + index].Valid() ||
            occurrences.CaptureCompletionOwner(leaf.occurrence, &existing)) return false;
    }
    for (std::uint32_t index = 0; index < row.leafCount; ++index) {
        const auto& leaf = leaves[index];
        auto& native = leafOriginals_[base + index];
        if (occurrences.TryPinOriginalCompositeCompletionOwner(leaf.occurrence, leaf.predicate.owner, &native) &&
            occurrences.ProvePreparedCompositeCompletionCommit(native, leaf.predicate.owner,
                occurrences.CompletionProgressOf(leaf.occurrence), leaf.renderGeneration,
                occurrences.ScheduleEpochOf(leaf.occurrence), &leafCommitProofs_[base + index])) continue;
        for (std::uint32_t undo = index + 1u; undo > 0; --undo) {
            const auto offset = undo - 1u;
            leafCommitProofs_[base + offset].Release();
            auto& issued = leafOriginals_[base + offset];
            if (!issued.Valid()) continue;
            const bool rolled = occurrences.RollbackCompletionOwnerPinExact(
                leaves[offset].occurrence, leaves[offset].predicate.owner);
            assert(rolled);
            if (!rolled) return false;
            issued.Release();
        }
        return false;
    }
    row.originalLeafCommitsPrepared = true;
    return true;
}

bool CompositeCompletionOwnerPool::BorrowOriginalPreparedLeafCompletion(
    const OriginalLifetimePin& original, std::uint32_t index,
    OccurrenceStatePool::OriginalCompositeCompletionOwnerPin* out) noexcept {
    if (!MatchesOriginalLifetime(original)) return false;
    const auto& row = rows_[original.token_.slot];
    if (row.state.load(std::memory_order_acquire) != State::Reserved ||
        !row.originalLeafCommitsPrepared || row.originalOccurrenceReceiver == nullptr || index >= row.leafCount) return false;
    const auto offset = static_cast<std::size_t>(original.token_.slot) * maxLeaves_ + index;
    return row.originalOccurrenceReceiver->IsPreparedCompositeCompletionCommitFor(
        leafCommitProofs_[offset], LeafStorage(original.token_.slot)[index].occurrence) &&
        leafOriginals_[offset].Borrow(out);
}

OccurrenceStatePool::PreparedCompositeCompletionCommitProof*
CompositeCompletionOwnerPool::ResolveOriginalPreparedLeafCommit(
    OccurrenceStatePool& occurrences, OccurrenceToken token) noexcept {
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        const auto& row = rows_[slot];
        if (!row.originalLeafCommitsPrepared || row.originalOccurrenceReceiver != &occurrences ||
            row.state.load(std::memory_order_acquire) != State::Reserved) continue;
        for (std::uint32_t index = 0; index < row.leafCount; ++index) {
            if (!SameToken(LeafStorage(slot)[index].occurrence, token)) continue;
            auto& proof = leafCommitProofs_[static_cast<std::size_t>(slot) * maxLeaves_ + index];
            if (occurrences.IsPreparedCompositeCompletionCommitFor(proof, token)) return &proof;
        }
    }
    return nullptr;
}

void CompositeCompletionOwnerPool::ReleaseOriginalPreparedLeafCommitProofsForQuiescentOwnerTeardown() noexcept {
    for (auto& proof : leafCommitProofs_) proof.Release();
}

bool CompositeCompletionOwnerPool::BorrowOriginalLifetime(const OriginalLifetimePin& original,
    OriginalLifetimePin* out) noexcept {
    if (out == nullptr || out->owner_ != nullptr || !MatchesOriginalLifetime(original)) return false;
    auto& row = rows_[original.token_.slot];
    if (row.originalPinCount == UINT32_MAX) return false;
    ++row.originalPinCount;
    out->owner_ = this;
    out->token_ = original.token_;
    out->marker_ = original.marker_;
    out->baseline_ = false;
    return true;
}

void CompositeCompletionOwnerPool::ReleaseOriginalLifetime(OriginalLifetimePin& original) noexcept {
    const bool matches = MatchesOriginalLifetime(original);
    assert(matches);
    if (!matches) return;
    auto& row = rows_[original.token_.slot];
    original.owner_ = nullptr;
    original.token_ = {};
    original.marker_ = 0;
    original.baseline_ = false;
    --row.originalPinCount;
    if (row.originalPinCount == 0 && row.state.load(std::memory_order_acquire) == State::Retired)
        RecycleRow(row);
}

bool CompositeCompletionOwnerPool::RegisterOriginalLifetimeBaselineResolver(
    void* context, OriginalBaselineResolver resolver) noexcept {
    if (context == nullptr || resolver == nullptr || originalBaselineResolver_ != nullptr) return false;
    for (const auto& row : rows_)
        if (row.originalPinCount != 0 || row.state.load(std::memory_order_acquire) != State::Free) return false;
    originalBaselineContext_ = context;
    originalBaselineResolver_ = resolver;
    return true;
}

bool CompositeCompletionOwnerPool::ResolveOriginalBaselineForNativeEnd(
    CompositeCompletionOwnerToken token, OriginalLifetimePin** out) noexcept {
    *out = nullptr;
    if (originalBaselineResolver_ == nullptr) return true;
    auto* actual = originalBaselineResolver_(originalBaselineContext_, *this, token);
    if (actual == nullptr || actual->owner_ == nullptr) return true;
    if (!actual->baseline_ || !IsOriginalLifetimePinFor(*actual, token)) return false;
    *out = actual;
    return true;
}

void CompositeCompletionOwnerPool::RetireRow(Row& row, bool occupied) noexcept {
    const auto slot = static_cast<std::size_t>(&row - rows_.data());
    for (std::uint32_t index = 0; index < row.leafCount; ++index) {
        leafCommitProofs_[slot * maxLeaves_ + index].Release();
        leafOriginals_[slot * maxLeaves_ + index].Release();
    }
    if (occupied) --occupied_;
    row.state.store(State::Retired, std::memory_order_release);
    if (row.originalPinCount == 0) RecycleRow(row);
}

void CompositeCompletionOwnerPool::RecycleRow(Row& row) noexcept {
    assert(row.originalPinCount == 0 && row.state.load(std::memory_order_acquire) == State::Retired);
    row.composedOccurrence = {};
    row.aggregateRevision = 0;
    row.leafCount = 0;
    row.absoluteLogicalCompleteFrame = 0;
    row.originalMarker = 0;
    row.originalOccurrenceReceiver = nullptr;
    row.originalLeafCommitsPrepared = false;
    if (row.generation == UINT32_MAX) {
        row.state.store(State::Quarantined, std::memory_order_release);
        ++quarantinedCount_;
    } else {
        ++row.generation;
        row.state.store(State::Free, std::memory_order_release);
    }
}

KnownScheduleLeafV1* CompositeCompletionOwnerPool::LeafStorage(
    std::uint32_t slot) noexcept {
    return leaves_.data() + static_cast<std::size_t>(slot) * maxLeaves_;
}

const KnownScheduleLeafV1* CompositeCompletionOwnerPool::LeafStorage(
    std::uint32_t slot) const noexcept {
    return leaves_.data() + static_cast<std::size_t>(slot) * maxLeaves_;
}

std::uint32_t CompositeCompletionOwnerPool::FreeCount() const noexcept {
    std::uint32_t count = 0;
    for (const Row& row : rows_) {
        if (row.state.load(std::memory_order_acquire) == State::Free) {
            ++count;
        }
    }
    return count;
}

CompositeCompletionAcquireStatus CompositeCompletionOwnerPool::Acquire(
    OccurrenceToken composedOccurrence,
    std::uint64_t aggregateRevision,
    const KnownScheduleLeafV1* leaves,
    std::uint32_t leafCount,
    OccurrenceStatePool& occurrences,
    CompositeCompletionOwnerToken* outToken) noexcept {
    CompositeCompletionOwnerToken prepared{};
    const CompositeCompletionAcquireStatus prepare = PrepareAcquire(
        composedOccurrence, aggregateRevision, leaves, leafCount,
        occurrences, &prepared);
    if (prepare != CompositeCompletionAcquireStatus::Ok) return prepare;
    const CompositeCompletionAcquireStatus commit =
        CommitPrepared(prepared, occurrences);
    if (commit != CompositeCompletionAcquireStatus::Ok) {
        (void)CancelPrepared(prepared);
        return commit;
    }
    *outToken = prepared;
    return CompositeCompletionAcquireStatus::Ok;
}

CompositeCompletionAcquireStatus
CompositeCompletionOwnerPool::PrepareAcquire(
    OccurrenceToken composedOccurrence,
    std::uint64_t aggregateRevision,
    const KnownScheduleLeafV1* leaves,
    std::uint32_t leafCount,
    OccurrenceStatePool& occurrences,
    CompositeCompletionOwnerToken* outToken,
    OriginalLifetimePin* original) noexcept {
    if (outToken == nullptr || leaves == nullptr || leafCount == 0 ||
        leafCount > maxLeaves_ || aggregateRevision == 0 ||
        composedOccurrence.generation == 0 ||
        occurrences.StageOf(composedOccurrence) == OccurrenceStage::Free ||
        (original != nullptr && (original->owner_ != nullptr || originalMarkerExhausted_ ||
                                nextOriginalMarker_ == 0))) {
        ++refusalCount_;
        return CompositeCompletionAcquireStatus::Invalid;
    }
    std::uint32_t slot = capacity_;
    for (std::uint32_t i = 0; i < capacity_; ++i) {
        if (rows_[i].state.load(std::memory_order_acquire) == State::Free) {
            slot = i;
            break;
        }
    }
    if (slot == capacity_) {
        ++refusalCount_;
        return CompositeCompletionAcquireStatus::Capacity;
    }
    Row& row = rows_[slot];
    row.state.store(State::Reserved, std::memory_order_relaxed);
    KnownScheduleLeafV1* ordered = LeafStorage(slot);
    for (std::uint32_t i = 0; i < leafCount; ++i) {
        std::uint32_t insert = i;
        while (insert > 0 &&
               ordered[insert - 1].stableLeafOrdinal >
                   leaves[i].stableLeafOrdinal) {
            ordered[insert] = ordered[insert - 1];
            --insert;
        }
        ordered[insert] = leaves[i];
    }
    for (std::uint32_t i = 0; i < leafCount; ++i) {
        if ((i > 0 && ordered[i - 1].stableLeafOrdinal ==
                          ordered[i].stableLeafOrdinal) ||
            !ValidLeaf(ordered[i], occurrences)) {
            row.state.store(State::Free, std::memory_order_release);
            ++refusalCount_;
            return CompositeCompletionAcquireStatus::Invalid;
        }
        for (std::uint32_t prior = 0; prior < i; ++prior) {
            if (SameToken(ordered[prior].occurrence,
                          ordered[i].occurrence)) {
                row.state.store(State::Free, std::memory_order_release);
                ++refusalCount_;
                return CompositeCompletionAcquireStatus::Invalid;
            }
        }
        CompletionOwnerRef existing{};
        if (occurrences.CaptureCompletionOwner(ordered[i].occurrence,
                                               &existing)) {
            row.state.store(State::Free, std::memory_order_release);
            ++refusalCount_;
            return CompositeCompletionAcquireStatus::LeafOwned;
        }
    }

    const CompositeCompletionOwnerToken token{slot, row.generation};
    std::uint64_t complete = 0;
    for (std::uint32_t index = 0; index < leafCount; ++index) {
        const CompletionOwnerRef owner = CompletionOwnerRef::Composite(
            token, ordered[index].predicate.playbackFactsRevision);
        KnownCompletionPredicateV1 bound{};
        if (!BindKnownCompletionPredicate(ordered[index].predicate,
                                          ordered[index].predicate.program,
                                          owner, &bound)) {
            row.state.store(State::Free, std::memory_order_release);
            ++refusalCount_;
            return CompositeCompletionAcquireStatus::Invalid;
        }
        ordered[index].predicate = bound;
        if (ordered[index].predicate.absoluteLogicalCompleteFrame > complete) {
            complete =
                ordered[index].predicate.absoluteLogicalCompleteFrame;
        }
    }
    row.originalOccurrenceReceiver = &occurrences;
    row.originalLeafCommitsPrepared = false;
    row.composedOccurrence = composedOccurrence;
    row.aggregateRevision = aggregateRevision;
    row.leafCount = leafCount;
    row.absoluteLogicalCompleteFrame = complete;
    if (original != nullptr) {
        const auto marker = TryIssue(&nextOriginalMarker_, &originalMarkerExhausted_);
        assert(marker.status == IssueStatus::Ok && row.originalPinCount == 0 && row.originalMarker == 0);
        row.originalMarker = marker.value;
        row.originalPinCount = 1;
        original->owner_ = this;
        original->token_ = token;
        original->marker_ = marker.value;
        original->baseline_ = true;
    }
    row.state.store(State::Reserved, std::memory_order_release);
    *outToken = token;
    return CompositeCompletionAcquireStatus::Ok;
}

CompositeCompletionAcquireStatus
CompositeCompletionOwnerPool::CommitPrepared(
    CompositeCompletionOwnerToken token,
    OccurrenceStatePool& occurrences) noexcept {
    if (!token.Valid() || token.slot >= capacity_) {
        ++refusalCount_;
        return CompositeCompletionAcquireStatus::Invalid;
    }
    Row& row = rows_[token.slot];
    if (row.generation != token.generation ||
        row.state.load(std::memory_order_acquire) != State::Reserved) {
        ++refusalCount_;
        return CompositeCompletionAcquireStatus::Invalid;
    }
    if (row.originalOccurrenceReceiver != &occurrences) {
        ++refusalCount_;
        return CompositeCompletionAcquireStatus::Invalid;
    }
    KnownScheduleLeafV1* leaves = LeafStorage(token.slot);
    if (row.originalLeafCommitsPrepared) {
        const auto* aggregate = occurrences.Find(row.composedOccurrence);
        if (aggregate == nullptr || aggregate->preparedReservation) {
            ++refusalCount_;
            return CompositeCompletionAcquireStatus::Invalid;
        }
        const auto base = static_cast<std::size_t>(token.slot) * maxLeaves_;
        for (std::uint32_t index = 0; index < row.leafCount; ++index) {
            const auto& leaf = leaves[index];
            const auto* entry = occurrences.Find(leaf.occurrence);
            if (!ValidLeaf(leaf, occurrences) || entry == nullptr || entry->preparedReservation ||
                entry->originalSinglePreparedCommitHeld || leafCommitProofs_[base + index].Valid() ||
                !occurrences.IsOriginalCompositeCompletionOwnerCurrent(leafOriginals_[base + index],
                    leaf.predicate.owner, occurrences.CompletionProgressOf(leaf.occurrence),
                    leaf.renderGeneration, occurrences.ScheduleEpochOf(leaf.occurrence))) {
                ++refusalCount_;
                return CompositeCompletionAcquireStatus::Invalid;
            }
        }
        // All genuine O gates were consumed by native O commits in owner order.
        row.state.store(State::Pinned, std::memory_order_release);
        ++occupied_;
        if (occupied_ > highWater_) highWater_ = occupied_;
        return CompositeCompletionAcquireStatus::Ok;
    }
    for (std::uint32_t index = 0; index < row.leafCount; ++index) {
        CompletionOwnerRef existing{};
        if (!ValidLeaf(leaves[index], occurrences)) {
            ++refusalCount_;
            return CompositeCompletionAcquireStatus::Invalid;
        }
        if (occurrences.CaptureCompletionOwner(
                leaves[index].occurrence, &existing)) {
            ++refusalCount_;
            return CompositeCompletionAcquireStatus::LeafOwned;
        }
    }
    std::uint32_t pinned = 0;
    auto* originals = leafOriginals_.data() + static_cast<std::size_t>(token.slot) * maxLeaves_;
    const bool retainOriginalLeaves = row.originalPinCount != 0;
    for (; pinned < row.leafCount; ++pinned) {
        const CompletionOwnerRef owner = CompletionOwnerRef::Composite(
            token, leaves[pinned].predicate.playbackFactsRevision);
        const bool bound = retainOriginalLeaves
            ? occurrences.TryPinOriginalCompositeCompletionOwner(leaves[pinned].occurrence, owner, &originals[pinned])
            : occurrences.TryPinCompletionOwner(leaves[pinned].occurrence, owner);
        if (!bound) {
            while (pinned > 0) {
                --pinned;
                const CompletionOwnerRef rollback =
                    CompletionOwnerRef::Composite(
                        token,
                        leaves[pinned].predicate.playbackFactsRevision);
                (void)occurrences.RollbackCompletionOwnerPinExact(
                    leaves[pinned].occurrence, rollback);
                originals[pinned].Release();
            }
            ++refusalCount_;
            return CompositeCompletionAcquireStatus::LeafOwned;
        }
    }
    row.state.store(State::Pinned, std::memory_order_release);
    ++occupied_;
    if (occupied_ > highWater_) highWater_ = occupied_;
    return CompositeCompletionAcquireStatus::Ok;
}

bool CompositeCompletionOwnerPool::CancelPrepared(
    CompositeCompletionOwnerToken token) noexcept {
    if (!token.Valid() || token.slot >= capacity_) return false;
    Row& row = rows_[token.slot];
    if (row.generation != token.generation ||
        row.state.load(std::memory_order_acquire) != State::Reserved) {
        return false;
    }
    OriginalLifetimePin* actualBaseline = nullptr;
    if (!ResolveOriginalBaselineForNativeEnd(token, &actualBaseline)) return false;
    if (row.originalLeafCommitsPrepared) {
        auto* occurrences = row.originalOccurrenceReceiver;
        if (occurrences == nullptr) return false;
        const auto base = static_cast<std::size_t>(token.slot) * maxLeaves_;
        for (std::uint32_t index = 0; index < row.leafCount; ++index) {
            const auto& leaf = LeafStorage(token.slot)[index];
            CompletionOwnerRef actual{};
            const auto* entry = occurrences->Find(leaf.occurrence);
            if (entry == nullptr ||
                (entry->originalSinglePreparedCommitHeld &&
                 !occurrences->IsPreparedCompositeCompletionCommitFor(leafCommitProofs_[base + index], leaf.occurrence)) ||
                !occurrences->CaptureCompletionOwner(leaf.occurrence, &actual) ||
                !SameCompletionOwner(actual, leaf.predicate.owner)) return false;
        }
        for (std::uint32_t index = row.leafCount; index > 0; --index) {
            const auto offset = index - 1u;
            const auto& leaf = LeafStorage(token.slot)[offset];
            leafCommitProofs_[base + offset].Release();
            const bool rolled = occurrences->RollbackCompletionOwnerPinExact(leaf.occurrence, leaf.predicate.owner);
            assert(rolled);
            if (!rolled) return false;
        }
    }
    if (actualBaseline != nullptr) actualBaseline->Release();
    RetireRow(row, false);
    return true;
}

bool CompositeCompletionOwnerPool::CapturePreparedLeaf(
    CompositeCompletionOwnerToken token, std::uint32_t index,
    KnownScheduleLeafV1* out) const noexcept {
    if (out == nullptr || !token.Valid() || token.slot >= capacity_) {
        return false;
    }
    const Row& row = rows_[token.slot];
    if (row.generation != token.generation ||
        row.state.load(std::memory_order_acquire) != State::Reserved ||
        index >= row.leafCount) {
        return false;
    }
    *out = LeafStorage(token.slot)[index];
    return true;
}

bool CompositeCompletionOwnerPool::CapturePrepared(
    CompositeCompletionOwnerToken token,
    CompositeCompletionOwnerInfo* out) const noexcept {
    if (out == nullptr || !token.Valid() || token.slot >= capacity_) return false;
    const Row& row = rows_[token.slot];
    if (row.generation != token.generation ||
        row.state.load(std::memory_order_acquire) != State::Reserved) return false;
    CompositeCompletionOwnerInfo info{};
    info.composedOccurrence = row.composedOccurrence;
    info.aggregateRevision = row.aggregateRevision;
    info.leafCount = row.leafCount;
    info.absoluteLogicalCompleteFrame = row.absoluteLogicalCompleteFrame;
    *out = info;
    return true;
}

const CompositeCompletionOwnerPool::Row*
CompositeCompletionOwnerPool::Resolve(
    CompositeCompletionOwnerToken token) const noexcept {
    if (!token.Valid() || token.slot >= capacity_) return nullptr;
    const Row& row = rows_[token.slot];
    return row.generation == token.generation &&
                   row.state.load(std::memory_order_acquire) == State::Pinned
               ? &row
               : nullptr;
}

bool CompositeCompletionOwnerPool::Capture(
    CompositeCompletionOwnerToken token,
    CompositeCompletionOwnerInfo* out) const noexcept {
    if (out == nullptr) return false;
    const Row* row = Resolve(token);
    if (row == nullptr) return false;
    CompositeCompletionOwnerInfo info{};
    info.composedOccurrence = row->composedOccurrence;
    info.aggregateRevision = row->aggregateRevision;
    info.leafCount = row->leafCount;
    info.absoluteLogicalCompleteFrame = row->absoluteLogicalCompleteFrame;
    *out = info;
    return true;
}

bool CompositeCompletionOwnerPool::CaptureLeaf(
    CompositeCompletionOwnerToken token, std::uint32_t index,
    KnownScheduleLeafV1* out) const noexcept {
    if (out == nullptr) return false;
    const Row* row = Resolve(token);
    if (row == nullptr || index >= row->leafCount) return false;
    *out = LeafStorage(token.slot)[index];
    return true;
}

bool CompositeCompletionOwnerPool::ProvePrivateReanchorBeforePublication(
    CompositeCompletionOwnerToken token,
    const KnownScheduleLeafV1* candidates,
    std::uint32_t leafCount,
    OccurrenceStatePool& occurrences,
    PrivateReanchorProof* out) noexcept {
    if (out == nullptr || out->Valid() || candidates == nullptr ||
        !token.Valid() || token.slot >= capacity_) return false;
    Row* row = &rows_[token.slot];
    if (row->generation != token.generation ||
        row->state.load(std::memory_order_acquire) != State::Pinned ||
        row->originalOccurrenceReceiver != &occurrences ||
        leafCount == 0 || leafCount != row->leafCount) return false;
    const KnownScheduleLeafV1* original = LeafStorage(token.slot);
    std::uint64_t complete = 0;
    for (std::uint32_t index = 0; index < leafCount; ++index) {
        const auto& before = original[index];
        const auto& after = candidates[index];
        const auto& oldPredicate = before.predicate;
        const auto& newPredicate = after.predicate;
        const CompletionProgressRow* progressRow =
            occurrences.CompletionProgressOf(before.occurrence);
        CompletionProgressSnapshot progress{};
        CompletionOwnerRef owner{};
        if (after.stableLeafOrdinal != before.stableLeafOrdinal ||
            after.authoredOffset.domain != before.authoredOffset.domain ||
            after.authoredOffset.numerator != before.authoredOffset.numerator ||
            after.authoredOffset.denominator != before.authoredOffset.denominator ||
            !SameToken(after.occurrence, before.occurrence) ||
            after.renderGeneration != before.renderGeneration ||
            after.absoluteLeafStartFrame !=
                newPredicate.absoluteLogicalStartFrame ||
            !SameToken(newPredicate.occurrence, oldPredicate.occurrence) ||
            newPredicate.renderGeneration != oldPredicate.renderGeneration ||
            newPredicate.totalLogicalOutputFrames !=
                oldPredicate.totalLogicalOutputFrames ||
            newPredicate.progressBaselineOutputFrames !=
                oldPredicate.progressBaselineOutputFrames ||
            newPredicate.progressBaselineRevision !=
                oldPredicate.progressBaselineRevision ||
            newPredicate.progressBaselineCompletedThroughBlockSequence !=
                oldPredicate.progressBaselineCompletedThroughBlockSequence ||
            newPredicate.playbackFactsRevision !=
                oldPredicate.playbackFactsRevision ||
            !SamePlayableProgramKey(newPredicate.program,
                                    oldPredicate.program) ||
            !SameCompletionOwner(newPredicate.owner,
                                 oldPredicate.owner) ||
            !IsBoundKnownCompletionPredicate(newPredicate) ||
            newPredicate.totalLogicalOutputFrames == 0 ||
            newPredicate.totalLogicalOutputFrames >
                UINT64_MAX - newPredicate.absoluteLogicalStartFrame ||
            newPredicate.absoluteLogicalCompleteFrame !=
                newPredicate.absoluteLogicalStartFrame +
                    newPredicate.totalLogicalOutputFrames ||
            progressRow == nullptr ||
            !CaptureCompletionProgressExact(*progressRow,
                before.occurrence, before.renderGeneration, &progress) ||
            progress.playbackFactsRevision !=
                oldPredicate.playbackFactsRevision ||
            progress.logicalOutputFramesProduced !=
                oldPredicate.progressBaselineOutputFrames ||
            progress.completedThroughBlockSequence <
                oldPredicate.progressBaselineCompletedThroughBlockSequence ||
            progress.completedThroughBlockSequence -
                oldPredicate.progressBaselineCompletedThroughBlockSequence >
                UINT64_MAX - oldPredicate.progressBaselineRevision ||
            progress.progressRevision !=
                oldPredicate.progressBaselineRevision +
                    (progress.completedThroughBlockSequence -
                     oldPredicate.progressBaselineCompletedThroughBlockSequence) ||
            !occurrences.CaptureCompletionOwner(before.occurrence, &owner) ||
            !SameCompletionOwner(owner, oldPredicate.owner)) return false;
        complete = std::max(complete,
            newPredicate.absoluteLogicalCompleteFrame);
    }
    out->owner_ = this;
    out->row_ = row;
    out->slot_ = token.slot;
    out->leaves_ = candidates;
    out->leafCount_ = leafCount;
    out->completeFrame_ = complete;
    return true;
}

void CompositeCompletionOwnerPool::ApplyPrivateReanchorBeforePublication(
    const PrivateReanchorProof& proof) noexcept {
    assert(proof.owner_ == this && proof.row_ == &rows_[proof.slot_]);
    auto* row = static_cast<Row*>(proof.row_);
    std::copy_n(proof.leaves_, proof.leafCount_, LeafStorage(proof.slot_));
    row->absoluteLogicalCompleteFrame = proof.completeFrame_;
}

bool CompositeCompletionOwnerPool::RollbackBeforePublication(
    CompositeCompletionOwnerToken token,
    OccurrenceStatePool& occurrences) noexcept {
    Row* row = ResolveForRelease(token);
    if (row == nullptr || row->originalOccurrenceReceiver != &occurrences ||
        row->state.load(std::memory_order_acquire) != State::Pinned) {
        return false;
    }
    OriginalLifetimePin* actualBaseline = nullptr;
    if (!ResolveOriginalBaselineForNativeEnd(token, &actualBaseline)) return false;
    KnownScheduleLeafV1* leaves = LeafStorage(token.slot);
    for (std::uint32_t index = 0; index < row->leafCount; ++index) {
        CompletionOwnerRef captured{};
        const CompletionOwnerRef expected = CompletionOwnerRef::Composite(
            token, leaves[index].predicate.playbackFactsRevision);
        if (!occurrences.CaptureCompletionOwner(
                leaves[index].occurrence, &captured) ||
            !SameCompletionOwner(expected, captured)) {
            return false;
        }
    }
    for (std::uint32_t index = row->leafCount; index > 0; --index) {
        const KnownScheduleLeafV1& leaf = leaves[index - 1u];
        const CompletionOwnerRef expected = CompletionOwnerRef::Composite(
            token, leaf.predicate.playbackFactsRevision);
        if (!occurrences.RollbackCompletionOwnerPinExact(
                leaf.occurrence, expected)) {
            Quarantine(*row);
            return false;
        }
    }
    if (actualBaseline != nullptr) actualBaseline->Release();
    RetireRow(*row, true);
    return true;
}

CompositeCompletionOwnerPool::Row*
CompositeCompletionOwnerPool::ResolveForRelease(
    CompositeCompletionOwnerToken token) noexcept {
    if (!token.Valid() || token.slot >= capacity_) return nullptr;
    Row& row = rows_[token.slot];
    const State state = row.state.load(std::memory_order_acquire);
    return row.generation == token.generation &&
                   (state == State::Pinned || state == State::Releasing)
               ? &row
               : nullptr;
}

void CompositeCompletionOwnerPool::Quarantine(Row& row) noexcept {
    row.state.store(State::Quarantined, std::memory_order_release);
    --occupied_;
    ++quarantinedCount_;
}

bool CompositeCompletionOwnerPool::Release(
    CompositeCompletionOwnerToken token,
    OccurrenceStatePool& occurrences,
    std::uint64_t observedCompletionBlockSequence) noexcept {
    Row* row = ResolveForRelease(token);
    if (row == nullptr || row->originalOccurrenceReceiver != &occurrences) return false;
    OriginalLifetimePin* actualBaseline = nullptr;
    if (!ResolveOriginalBaselineForNativeEnd(token, &actualBaseline)) return false;
    KnownScheduleLeafV1* leaves = LeafStorage(token.slot);
    State state = row->state.load(std::memory_order_acquire);
    if (state == State::Pinned) {
        for (std::uint32_t i = 0; i < row->leafCount; ++i) {
            CompletionOwnerRef captured{};
            const CompletionOwnerRef expected =
                CompletionOwnerRef::Composite(
                    token, leaves[i].predicate.playbackFactsRevision);
            if (!occurrences.CaptureCompletionOwner(
                    leaves[i].occurrence, &captured) ||
                !SameCompletionOwner(expected, captured)) {
                return false;
            }
        }
        for (std::uint32_t i = row->leafCount; i > 0; --i) {
            const KnownScheduleLeafV1& leaf = leaves[i - 1];
            const CompletionOwnerRef expected =
                CompletionOwnerRef::Composite(
                    token, leaf.predicate.playbackFactsRevision);
            if (!occurrences.ClearCompletionOwnerExact(
                    leaf.occurrence, expected)) {
                Quarantine(*row);
                return false;
            }
        }
        row->state.store(State::Releasing, std::memory_order_release);
    }
    for (std::uint32_t i = 0; i < row->leafCount; ++i) {
        if (!occurrences.CompletionOwnerReleaseFencePassed(
                leaves[i].occurrence,
                observedCompletionBlockSequence)) {
            return false;
        }
    }
    if (actualBaseline != nullptr) actualBaseline->Release();
    RetireRow(*row, true);
    return true;
}

static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

} // namespace harken
