#ifndef HARKEN_CORE_COMPOSITE_COMPLETION_OWNER_POOL_H
#define HARKEN_CORE_COMPOSITE_COMPLETION_OWNER_POOL_H

#include "harken/core/CompletionOwner.h"
#include "harken/core/CurrentWorld.h"
#include "harken/core/KnownCompletion.h"
#include "harken/core/OccurrenceStatePool.h"

#include "harken/core/StableRowStorage.h"

#include <atomic>
#include <array>
#include <cstdint>
#include <vector>

namespace harken {

namespace detail {
struct CompositeCompletionOwnerPoolTestAccess;
}

struct KnownScheduleLeafV1 {
    std::uint32_t stableLeafOrdinal = 0;
    AuthoredTimeRationalV1 authoredOffset{};
    OccurrenceToken occurrence{};
    std::uint32_t renderGeneration = 0;
    std::uint64_t absoluteLeafStartFrame = 0;
    KnownCompletionPredicateV1 predicate{};
};

enum class CompositeCompletionAcquireStatus : std::uint8_t {
    Ok = 0,
    Invalid = 1,
    Capacity = 2,
    LeafOwned = 3,
};

struct CompositeCompletionOwnerInfo {
    OccurrenceToken composedOccurrence{};
    std::uint64_t aggregateRevision = 0;
    std::uint32_t leafCount = 0;
    std::uint64_t absoluteLogicalCompleteFrame = 0;
};

class CompositeCompletionOwnerPool {
public:
    // Issued by the same successful PrepareAcquire, never by token lookup.
    // Serialized logic-producer storage retention only: no leaf-progress,
    // audio-reader, Current mutation, completion or publication right.
    class OriginalLifetimePin {
    public:
        OriginalLifetimePin() noexcept = default;
        OriginalLifetimePin(const OriginalLifetimePin&) = delete;
        OriginalLifetimePin& operator=(const OriginalLifetimePin&) = delete;
        OriginalLifetimePin(OriginalLifetimePin&& other) noexcept;
        OriginalLifetimePin& operator=(OriginalLifetimePin&& other) noexcept;
        ~OriginalLifetimePin() { Release(); }
        bool Valid() const noexcept;
        bool Borrow(OriginalLifetimePin* out) const noexcept;
        void Release() noexcept;
    private:
        friend class CompositeCompletionOwnerPool;
        CompositeCompletionOwnerPool* owner_ = nullptr;
        CompositeCompletionOwnerToken token_{};
        std::uint64_t marker_ = 0;
        bool baseline_ = false;
    };

    class PrivateReanchorProof {
    public:
        bool Valid() const noexcept { return owner_ != nullptr; }
    private:
        friend class CompositeCompletionOwnerPool;
        CompositeCompletionOwnerPool* owner_ = nullptr;
        void* row_ = nullptr;
        std::uint32_t slot_ = UINT32_MAX;
        const KnownScheduleLeafV1* leaves_ = nullptr;
        std::uint32_t leafCount_ = 0;
        std::uint64_t originalCompleteFrame_ = 0;
        std::uint64_t completeFrame_ = 0;
    };

    CompositeCompletionOwnerPool(std::uint32_t capacity,
                                 std::uint32_t maxLeaves);
    ~CompositeCompletionOwnerPool();
    using OriginalBaselineResolver = OriginalLifetimePin* (*)(void*,
        CompositeCompletionOwnerPool&, CompositeCompletionOwnerToken) noexcept;
    bool RegisterOriginalLifetimeBaselineResolver(void* context, OriginalBaselineResolver resolver) noexcept;

    CompositeCompletionAcquireStatus Acquire(
        OccurrenceToken composedOccurrence,
        std::uint64_t aggregateRevision,
        const KnownScheduleLeafV1* leaves,
        std::uint32_t leafCount,
        OccurrenceStatePool& occurrences,
        CompositeCompletionOwnerToken* outToken) noexcept;
    CompositeCompletionAcquireStatus PrepareAcquire(
        OccurrenceToken composedOccurrence,
        std::uint64_t aggregateRevision,
        const KnownScheduleLeafV1* leaves,
        std::uint32_t leafCount,
        OccurrenceStatePool& occurrences,
        CompositeCompletionOwnerToken* outToken,
        OriginalLifetimePin* original = nullptr) noexcept;
    bool IsOriginalLifetimePinFor(const OriginalLifetimePin& original,
                                 CompositeCompletionOwnerToken token) const noexcept;
    bool IsOriginalLifetimePinned(const OriginalLifetimePin& original) const noexcept;
    // Copies the actual retained episode; retired storage is not an active owner.
    // Absolute geometry may still be reanchored by the real private factory.
    bool CaptureOriginal(const OriginalLifetimePin& original,
                         CompositeCompletionOwnerInfo* out) const noexcept;
    bool CaptureOriginalLeaf(const OriginalLifetimePin& original,
                             std::uint32_t index, KnownScheduleLeafV1* out) const noexcept;
    // Available only after this same real CommitPrepared has bound the leaf.
    // A prepared row, a copied leaf/token, or retired metadata cannot issue it.
    bool BorrowOriginalLeafCompletion(const OriginalLifetimePin& original,
        std::uint32_t index, OccurrenceStatePool::OriginalCompositeCompletionOwnerPin* out) noexcept;
    // The actual factory baseline stages all native leaf gates before publication.
    bool PrepareOriginalLeafCompletionCommits(const OriginalLifetimePin& original,
        OccurrenceStatePool& occurrences) noexcept;
    bool BorrowOriginalPreparedLeafCompletion(const OriginalLifetimePin& original,
        std::uint32_t index, OccurrenceStatePool::OriginalCompositeCompletionOwnerPin* out) noexcept;
    // Resolves an already issued owner-held proof; it cannot create one by token.
    OccurrenceStatePool::PreparedCompositeCompletionCommitProof* ResolveOriginalPreparedLeafCommit(
        OccurrenceStatePool& occurrences, OccurrenceToken token) noexcept;
    void ReleaseOriginalPreparedLeafCommitProofsForQuiescentOwnerTeardown() noexcept;
    CompositeCompletionAcquireStatus CommitPrepared(
        CompositeCompletionOwnerToken token,
        OccurrenceStatePool& occurrences) noexcept;
    bool CancelPrepared(CompositeCompletionOwnerToken token) noexcept;
    bool CapturePreparedLeaf(CompositeCompletionOwnerToken token,
                             std::uint32_t index,
                             KnownScheduleLeafV1* out) const noexcept;
    // Producer observation of the actual Reserved row; no pin or proof is issued.
    bool CapturePrepared(CompositeCompletionOwnerToken token,
                         CompositeCompletionOwnerInfo* out) const noexcept;

    bool Capture(CompositeCompletionOwnerToken token,
                 CompositeCompletionOwnerInfo* out) const noexcept;
    bool CaptureLeaf(CompositeCompletionOwnerToken token,
                     std::uint32_t index,
                     KnownScheduleLeafV1* out) const noexcept;
    // The caller proves that no audio path can read the composed owner until
    // Apply completes and keeps candidate storage stable across both calls.
    bool ProvePrivateReanchorBeforePublication(
        CompositeCompletionOwnerToken token,
        const KnownScheduleLeafV1* candidates,
        std::uint32_t leafCount,
        OccurrenceStatePool& occurrences,
        PrivateReanchorProof* out) noexcept;
    // Uniform reanchors retain the checked shift, without a candidate leaf array.
    // The same private-owner and serialized producer requirements apply.
    bool ProvePrivateUniformReanchorBeforePublication(
        CompositeCompletionOwnerToken token,
        std::uint64_t oldEntryFrame,
        std::uint64_t newEntryFrame,
        OccurrenceStatePool& occurrences,
        PrivateReanchorProof* out) noexcept;
    void ApplyPrivateReanchorBeforePublication(
        const PrivateReanchorProof& proof) noexcept;
    bool RollbackBeforePublication(
        CompositeCompletionOwnerToken token,
        OccurrenceStatePool& occurrences) noexcept;
    bool Release(CompositeCompletionOwnerToken token,
                 OccurrenceStatePool& occurrences,
                 std::uint64_t observedCompletionBlockSequence) noexcept;

    std::uint32_t Capacity() const noexcept { return capacity_; }
    std::uint32_t ConstructedRowCount() const noexcept { return rows_.ConstructedCount(); }
    std::uint64_t StorageBytes() const noexcept;
    using BackingPreparation = std::array<StableStoragePreparation, 4>;
    bool PrepareBacking(std::uint32_t rows, BackingPreparation* out) noexcept;
    std::uint64_t CommittedBytes() const noexcept;
    std::uint64_t ReservedBytes() const noexcept;
    void SetRowCommitPageLimitForTests(std::size_t pages) noexcept {
        rows_.SetCommitPageLimitForTests(pages);
    }
    std::uint32_t FreeCount() const noexcept;
    std::uint32_t OccupiedCount() const noexcept { return occupied_; }
    std::uint32_t HighWater() const noexcept { return highWater_; }
    std::uint64_t RefusalCount() const noexcept { return refusalCount_; }
    std::uint32_t QuarantinedCount() const noexcept {
        return quarantinedCount_;
    }

private:
    friend struct detail::CompositeCompletionOwnerPoolTestAccess;
    enum class State : std::uint32_t {
        Free = 0,
        Reserved = 1,
        Pinned = 2,
        Releasing = 3,
        Quarantined = 4,
        Retired = 5,
    };

    struct Row {
        std::atomic<State> state{State::Free};
        std::uint32_t generation = 1;
        OccurrenceToken composedOccurrence{};
        std::uint64_t aggregateRevision = 0;
        std::uint32_t leafCount = 0;
        std::uint64_t absoluteLogicalCompleteFrame = 0;
        std::uint64_t originalMarker = 0;
        std::uint32_t originalPinCount = 0;
        OccurrenceStatePool* originalOccurrenceReceiver = nullptr;
        bool originalLeafCommitsPrepared = false;
    };

    const Row* Resolve(CompositeCompletionOwnerToken token) const noexcept;
    bool ProvePrivateReanchor(
        CompositeCompletionOwnerToken token,
        const KnownScheduleLeafV1* candidates,
        std::uint32_t leafCount,
        std::uint64_t oldEntryFrame,
        std::uint64_t newEntryFrame,
        OccurrenceStatePool& occurrences,
        PrivateReanchorProof* out) noexcept;
    bool PrepareSlotStorage(std::uint32_t slot) noexcept;
    Row* ResolveForRelease(CompositeCompletionOwnerToken token) noexcept;
    void Quarantine(Row& row) noexcept;
    void RetireRow(Row& row, bool occupied) noexcept;
    void RecycleRow(Row& row) noexcept;
    bool MatchesOriginalLifetime(const OriginalLifetimePin& original) const noexcept;
    bool BorrowOriginalLifetime(const OriginalLifetimePin& original,
                                OriginalLifetimePin* out) noexcept;
    void ReleaseOriginalLifetime(OriginalLifetimePin& original) noexcept;
    bool ResolveOriginalBaselineForNativeEnd(CompositeCompletionOwnerToken token,
        OriginalLifetimePin** out) noexcept;
    KnownScheduleLeafV1* LeafStorage(std::uint32_t slot) noexcept;
    const KnownScheduleLeafV1* LeafStorage(
        std::uint32_t slot) const noexcept;

    std::uint32_t capacity_ = 0;
    std::uint32_t maxLeaves_ = 0;
    StableRowStorage<Row> rows_;
    StableRowStorage<KnownScheduleLeafV1> leaves_;
    StableRowStorage<OccurrenceStatePool::OriginalCompositeCompletionOwnerPin> leafOriginals_;
    StableRowStorage<OccurrenceStatePool::PreparedCompositeCompletionCommitProof> leafCommitProofs_;
    std::uint32_t occupied_ = 0;
    std::uint32_t highWater_ = 0;
    std::uint64_t refusalCount_ = 0;
    std::uint32_t quarantinedCount_ = 0;
    std::uint64_t nextOriginalMarker_ = 1;
    bool originalMarkerExhausted_ = false;
    void* originalBaselineContext_ = nullptr;
    OriginalBaselineResolver originalBaselineResolver_ = nullptr;
};

} // namespace harken

#endif // HARKEN_CORE_COMPOSITE_COMPLETION_OWNER_POOL_H
