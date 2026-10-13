// The occurrence-state pool: live state for each active placement of a clip,
// held in a fixed pool that is deliberately not the playable-instance table.
//
// A clip is never a public instance. It compiles to an event template plus
// lineage, and what exists at runtime is an occurrence: one arranging instance
// playing one clip on one loop iteration. Keeping occurrences out of the
// instance table is what stops a public handle from appearing for something
// callers were never meant to address individually.
//
// The token is the internal counterpart of an instance handle: a (slot,
// generation) pair that is a struct rather than a packed integer, so it cannot
// be passed off as a public handle by accident. Generation 0 is never valid;
// exhausting the maximum generation permanently quarantines the slot.
//
// Two independent cancellations have to be expressible, which is why there are
// two epochs to check. Cancelling one occurrence bumps that occurrence's
// schedule epoch; cancelling the whole arranging instance bumps the instance's
// epoch as well. A transaction therefore validates both in one call — checking
// only the occurrence would let work survive its host, and checking only the
// host would let a cancelled occurrence keep running alongside its siblings.
//
// The stage cycle is Free, Preparing, Active, LogicalDone,
// AwaitingEventFence, and back to Free. A record waiting on the event fence
// still owns its slot: it is not free, so acquire cannot hand it out and a
// capacity reading cannot count it as headroom. The slot advances its
// generation on the way back to Free, which is the single moment an
// occurrence's identity ends; at maximum generation it instead becomes
// permanently quarantined.
//
// Logic thread only. Storage is sized once at construction; acquire, advance,
// validate, and release allocate nothing.
#ifndef HARKEN_CORE_OCCURRENCE_STATE_POOL_H
#define HARKEN_CORE_OCCURRENCE_STATE_POOL_H

#include <harken/ObjectId.h>
#include "harken/core/CheckedIssuer.h"
#include "harken/core/CompletionProgress.h"
#include "harken/core/CompletionOwner.h"
#include "harken/core/OccurrenceToken.h"
#include "harken/core/PlayableInstanceTable.h"

#include <cstdint>
#include <vector>

namespace harken {

namespace detail {
struct OccurrenceStatePoolTestAccess;
struct CompletionOwnerTestAccess;
struct PreparedCompositeLeafIssuerLimitTestAccess;
}

// Identity of one occurrence in authored terms. The arranging instance is the
// Track's host: the Piece instance for a Track inside a Piece, the Track
// instance when the Track was triggered directly. Both depths use this one
// shape, so neither is a special case.
struct OccurrenceKey {
    PlayableInstanceHandle arrangingInstance = 0;
    VnObjectId clipId{};
    std::uint32_t loopIteration = 0;
    std::uint32_t occurrenceOrdinal = 0;
};

// Internal pool identity. A struct rather than a packed id, so it cannot be
// mistaken for — or handed out as — a public instance handle.
// Stages, in the only order they may be traversed. Free is reached again only
// through the event fence.
enum class OccurrenceStage : std::uint8_t {
    Free = 0,
    Preparing = 1,
    Active = 2,
    LogicalDone = 3,
    AwaitingEventFence = 4
};

class OccurrenceStatePool {
    struct Entry;
public:
    // Storage only, issued with the actual native prepared acquisition.
    // It grants no completion owner, SourceDone, or Current mutation.
    class OriginalStoragePin {
    public:
        OriginalStoragePin() noexcept = default;
        OriginalStoragePin(const OriginalStoragePin&) = delete;
        OriginalStoragePin& operator=(const OriginalStoragePin&) = delete;
        OriginalStoragePin(OriginalStoragePin&& other) noexcept;
        OriginalStoragePin& operator=(OriginalStoragePin&& other) noexcept;
        ~OriginalStoragePin() { Release(); }
        bool Valid() const noexcept;
        bool NativeEnded() const noexcept;
        OccurrenceToken OriginalToken() const noexcept { return token_; }
        bool CaptureOriginalKey(OccurrenceKey* out) const noexcept;
        bool Borrow(OriginalStoragePin* out) const noexcept;
        void Release() noexcept;
    private:
        friend class OccurrenceStatePool;
        OccurrenceStatePool* owner_ = nullptr;
        Entry* entry_ = nullptr;
        OccurrenceToken token_{};
        std::uint64_t episode_ = 0;
        bool baseline_ = false;
    };
    using OriginalStorageBaselineResolver = OriginalStoragePin* (*)(
        void*, OccurrenceStatePool&, const OccurrenceToken&) noexcept;
    bool RegisterOriginalStorageBaselineResolver(void* context,
        OriginalStorageBaselineResolver resolver) noexcept;
    bool IsOriginalStoragePinFor(const OriginalStoragePin& original,
        OccurrenceToken token) const noexcept;

    // Issued only by this pool's successful original Single owner bind.
    // The pool outlives all original storage loans.
    class OriginalSingleCompletionOwnerPin {
    public:
        OriginalSingleCompletionOwnerPin() noexcept = default;
        OriginalSingleCompletionOwnerPin(const OriginalSingleCompletionOwnerPin&) = delete;
        OriginalSingleCompletionOwnerPin& operator=(const OriginalSingleCompletionOwnerPin&) = delete;
        OriginalSingleCompletionOwnerPin(OriginalSingleCompletionOwnerPin&& other) noexcept;
        OriginalSingleCompletionOwnerPin& operator=(OriginalSingleCompletionOwnerPin&& other) noexcept;
        ~OriginalSingleCompletionOwnerPin();
        bool Valid() const noexcept { return owner_ != nullptr; }
        bool Borrow(OriginalSingleCompletionOwnerPin* out) const noexcept;
        void Release() noexcept;
    private:
        friend class OccurrenceStatePool;
        OccurrenceStatePool* owner_ = nullptr;
        OccurrenceToken token_{};
        std::uint64_t marker_ = 0;
    };
    // Only the actual Composite factory can issue this leaf-owner output.
    // Its storage retention shares the native occurrence counter, while the
    // wrapper keeps it out of the Single preparation/retarget interfaces.
    class OriginalCompositeCompletionOwnerPin {
    public:
        OriginalCompositeCompletionOwnerPin() noexcept = default;
        OriginalCompositeCompletionOwnerPin(const OriginalCompositeCompletionOwnerPin&) = delete;
        OriginalCompositeCompletionOwnerPin& operator=(const OriginalCompositeCompletionOwnerPin&) = delete;
        OriginalCompositeCompletionOwnerPin(OriginalCompositeCompletionOwnerPin&&) noexcept = default;
        OriginalCompositeCompletionOwnerPin& operator=(OriginalCompositeCompletionOwnerPin&&) noexcept = default;
        bool Valid() const noexcept { return original_.Valid(); }
        bool Borrow(OriginalCompositeCompletionOwnerPin* out) const noexcept {
            return out != nullptr && original_.Borrow(&out->original_);
        }
        void Release() noexcept { original_.Release(); }
    private:
        friend class OccurrenceStatePool;
        OriginalSingleCompletionOwnerPin original_;
    };
    bool IsOriginalCompositeCompletionOwnerPinFor(
        const OriginalCompositeCompletionOwnerPin& original, OccurrenceToken token) const noexcept;
    bool IsOriginalCompositeCompletionOwnerCurrent(
        const OriginalCompositeCompletionOwnerPin& original, const CompletionOwnerRef& owner,
        const CompletionProgressRow* progress, std::uint32_t renderGeneration,
        std::uint64_t scheduleEpoch) const noexcept;
    // Capacity covers every record that owns a slot at once: preparing,
    // active, logically done, and waiting on the fence. Allocated here and
    // never grown.
    // A short logic-thread exclusion on this actual prepared Single row.
    // It is separate from structural loans and never publishes audio or a fence.
    class PreparedSingleCompletionCommitProof {
    public:
        PreparedSingleCompletionCommitProof() noexcept = default;
        PreparedSingleCompletionCommitProof(const PreparedSingleCompletionCommitProof&) = delete;
        PreparedSingleCompletionCommitProof& operator=(const PreparedSingleCompletionCommitProof&) = delete;
        PreparedSingleCompletionCommitProof(PreparedSingleCompletionCommitProof&& other) noexcept;
        PreparedSingleCompletionCommitProof& operator=(PreparedSingleCompletionCommitProof&& other) noexcept;
        ~PreparedSingleCompletionCommitProof() { Release(); }
        bool Valid() const noexcept { return entry_ != nullptr; }
        PlayableInstanceHandle OriginalArrangingInstance() const noexcept;
        void Release() noexcept;
    private:
        friend class OccurrenceStatePool;
        friend class PrivateSelectedQueueFutureSpanPool;
        OccurrenceStatePool* owner_ = nullptr;
        Entry* entry_ = nullptr;
        OccurrenceToken token_{};
        std::uint64_t marker_ = 0;
        CompletionOwnerRef completionOwner_{};
        const CompletionProgressRow* progress_ = nullptr;
        std::uint32_t renderGeneration_ = 0;
        std::uint64_t scheduleEpoch_ = 0;
    };
    // Only the actual native Composite factory can prepare this typed leaf gate.
    class PreparedCompositeCompletionCommitProof {
    public:
        PreparedCompositeCompletionCommitProof() noexcept = default;
        PreparedCompositeCompletionCommitProof(const PreparedCompositeCompletionCommitProof&) = delete;
        PreparedCompositeCompletionCommitProof& operator=(const PreparedCompositeCompletionCommitProof&) = delete;
        PreparedCompositeCompletionCommitProof(PreparedCompositeCompletionCommitProof&&) noexcept = default;
        PreparedCompositeCompletionCommitProof& operator=(PreparedCompositeCompletionCommitProof&&) noexcept = default;
        bool Valid() const noexcept { return proof_.Valid(); }
        void Release() noexcept { proof_.Release(); }
    private:
        friend class OccurrenceStatePool;
        PreparedSingleCompletionCommitProof proof_;
    };
    using OriginalCompositePreparedCommitResolver = PreparedCompositeCompletionCommitProof* (*)(
        void*, OccurrenceStatePool&, const OccurrenceToken&) noexcept;
    bool RegisterOriginalCompositePreparedCommitResolver(void* context,
        OriginalCompositePreparedCommitResolver resolver) noexcept;
    bool IsPreparedCompositeCompletionCommitFor(const PreparedCompositeCompletionCommitProof& proof,
        OccurrenceToken occurrence) const noexcept;
    static void ConsumePreparedCompositeCompletionCommit(PreparedCompositeCompletionCommitProof&& proof) noexcept;

    bool ProvePreparedSingleCompletionCommit(const OriginalSingleCompletionOwnerPin& original,
        const CompletionOwnerRef& owner, const CompletionProgressRow* originalProgress,
        std::uint32_t renderGeneration, std::uint64_t scheduleEpoch,
        PreparedSingleCompletionCommitProof* out) noexcept;
    bool MatchesPreparedSingleCompletionCommit(const PreparedSingleCompletionCommitProof& proof,
        OccurrenceToken occurrence, const CompletionOwnerRef& owner,
        const CompletionProgressRow* originalProgress, std::uint32_t renderGeneration,
        std::uint64_t scheduleEpoch, PlayableInstanceHandle arrangingInstance) const noexcept;
    static void ConsumePreparedSingleCompletionCommit(PreparedSingleCompletionCommitProof&& proof) noexcept;

    explicit OccurrenceStatePool(std::uint32_t capacity);
    ~OccurrenceStatePool();
    OccurrenceStatePool(const OccurrenceStatePool&) = delete;
    OccurrenceStatePool& operator=(const OccurrenceStatePool&) = delete;
    OccurrenceStatePool(OccurrenceStatePool&&) = delete;
    OccurrenceStatePool& operator=(OccurrenceStatePool&&) = delete;

    // Physical storage reserved at construction. FreeCount reports the
    // currently reusable subset after generation exhaustion is accounted for.
    std::uint32_t Capacity() const {
        return static_cast<std::uint32_t>(m_entries.size());
    }

    // Takes a free slot for a new occurrence and writes its token. False when
    // the pool is full — a defined refusal. The occurrence enters Preparing
    // with a fresh, nonzero schedule epoch.
    bool Acquire(const OccurrenceKey& key, OccurrenceToken* outToken);
    bool PrepareAcquire(const OccurrenceKey& key,
                        OccurrenceToken* outToken, OriginalStoragePin* original = nullptr);
    using OriginalSingleBaselineResolver = OriginalSingleCompletionOwnerPin* (*)(
        void*, OccurrenceStatePool&, const OccurrenceToken&) noexcept;
    using OriginalSinglePreparedCommitResolver = PreparedSingleCompletionCommitProof* (*)(
        void*, OccurrenceStatePool&, const OccurrenceToken&) noexcept;
    // Only actual owner-held issued objects may be resolved. General held
    // CommitPreparedAcquire remains refused when no native proof is returned.
    bool RegisterOriginalSingleOwnerResolvers(void* context,
        OriginalSingleBaselineResolver baseline,
        OriginalSinglePreparedCommitResolver preparedCommit) noexcept;
    bool IsPreparedSingleCompletionCommitFor(const PreparedSingleCompletionCommitProof& proof,
        const OccurrenceToken& occurrence) const noexcept;
    bool CommitPreparedAcquire(const OccurrenceToken& token);
    bool CancelPreparedAcquire(const OccurrenceToken& token,
        OriginalSingleCompletionOwnerPin* originalBaseline = nullptr,
        OriginalStoragePin* storageBaseline = nullptr);

    // The stage a token names, or Free when the token is stale. A stale token
    // and a genuinely free slot read alike on purpose: neither names an
    // occurrence a caller may act on.
    OccurrenceStage StageOf(const OccurrenceToken& token) const;

    // The authored identity behind a token, or nullptr when stale. Callers
    // need it to attribute an event without keeping a parallel copy of the key.
    const OccurrenceKey* KeyOf(const OccurrenceToken& token) const;

    // The occurrence's current schedule epoch, or 0 when the token is stale.
    std::uint64_t ScheduleEpochOf(const OccurrenceToken& token) const;

    bool BindCompletionProgress(const OccurrenceToken& token,
                                std::uint32_t renderGeneration,
                                std::uint64_t playbackFactsRevision,
                                std::uint64_t completedThroughBlockSequence = 0);
    CompletionProgressRow* CompletionProgressOf(
        const OccurrenceToken& token);
    const CompletionProgressRow* CompletionProgressOf(
        const OccurrenceToken& token) const;
    bool UnbindCompletionProgress(const OccurrenceToken& token,
                                  std::uint32_t renderGeneration);
    bool TryPinCompletionOwner(const OccurrenceToken& token,
                               const CompletionOwnerRef& owner,
                               OriginalSingleCompletionOwnerPin* original = nullptr);
    bool IsOriginalSingleCompletionOwnerPinFor(
        const OriginalSingleCompletionOwnerPin& original,
        const OccurrenceToken& token) const noexcept;
    bool IsOriginalSingleCompletionOwnerCurrent(
        const OriginalSingleCompletionOwnerPin& original,
        const CompletionOwnerRef& owner,
        const CompletionProgressRow* originalProgress,
        std::uint32_t renderGeneration,
        std::uint64_t scheduleEpoch) const noexcept;
    // Only the authentic original episode can end its own storage retention.
    // A foreign loan keeps the old progress and owner intact before withdrawal.
    bool CanWithdrawOriginalSingleCompletionForRebind(
        const OriginalSingleCompletionOwnerPin& original, const OccurrenceToken& token,
        const CompletionOwnerRef& owner, const CompletionProgressRow* originalProgress,
        std::uint32_t renderGeneration, std::uint64_t scheduleEpoch) const noexcept;
    bool ReturnWithdrawnOriginalSingleCompletionForRebind(
        OriginalSingleCompletionOwnerPin& original, const OccurrenceToken& token,
        const CompletionOwnerRef& owner, const CompletionProgressRow* originalProgress,
        std::uint32_t renderGeneration, std::uint64_t scheduleEpoch) noexcept;
    bool CaptureCompletionOwner(const OccurrenceToken& token,
                                CompletionOwnerRef* out) const;
    bool ReplaceCompletionOwnerExact(
        const OccurrenceToken& token,
        const CompletionOwnerRef& expected,
        const CompletionOwnerRef& replacement);
    bool ClearCompletionOwnerExact(const OccurrenceToken& token,
                                   const CompletionOwnerRef& owner);
    bool RollbackCompletionOwnerPinExact(const OccurrenceToken& token,
                                         const CompletionOwnerRef& owner);
    bool CompletionOwnerReleaseFencePassed(
        const OccurrenceToken& token,
        std::uint64_t observedCompletionBlockSequence) const;

    // Advances one occurrence to a later stage. Refused for a stale token, for
    // Free as a destination, and for any move that is not strictly forward.
    bool Advance(const OccurrenceToken& token, OccurrenceStage to);

    // Cancels one occurrence without touching its siblings: the epoch moves,
    // so records stamped with the old value fail validation on arrival.
    // Returns the new epoch, or 0 for a stale token.
    IssueResult BumpScheduleEpochChecked(const OccurrenceToken& token);
    std::uint64_t BumpScheduleEpoch(const OccurrenceToken& token) {
        return BumpScheduleEpochChecked(token).value;
    }
    bool ScheduleEpochExhausted() const { return m_scheduleEpochExhausted; }

    // Whether a stamped transaction may still be applied. Every condition is
    // checked here so no caller can accidentally check one and not the other:
    // the token resolves, the occurrence has not been cancelled since, and the
    // arranging instance named by the key is itself still live and uncancelled.
    bool ValidateTransaction(const OccurrenceToken& token,
                             std::uint64_t occurrenceScheduleEpoch,
                             std::uint64_t arrangingInstanceScheduleEpoch,
                             const PlayableInstanceTable& instances) const;

    // Returns a fence-waiting slot to the pool and advances its generation, so
    // every token to that occurrence is stale from here on. A slot at maximum
    // generation is quarantined instead of wrapping to generation one.
    // Refused before the record reaches AwaitingEventFence: releasing earlier
    // would reuse a slot whose events have not yet been consumed or
    // acknowledged. Genuine Composite structural loans retain only the ended
    // storage: active tokens stop resolving, and last Return performs generation
    // advance/reuse. Single and null-output retirement contracts are unchanged.
    bool ReleaseAfterFence(
        const OccurrenceToken& token,
        std::uint64_t observedCompletionBlockSequence = 0,
        OriginalSingleCompletionOwnerPin* originalBaseline = nullptr,
        OriginalStoragePin* storageBaseline = nullptr);

    // Records owning a slot, records specifically waiting on the fence, and
    // the most that ever owned a slot at once. The fence count is reported
    // separately because it is the part of occupancy that cannot be reclaimed
    // by cancelling anything.
    std::uint32_t OccupiedCount() const { return m_occupiedCount; }
    std::uint32_t AwaitingFenceCount() const { return m_awaitingFenceCount; }
    std::uint32_t HighWater() const { return m_highWater; }
    std::uint32_t FreeCount() const;
    std::uint32_t QuarantinedCount() const { return m_quarantinedCount; }
    std::uint32_t RetainedStorageCount() const { return m_retainedStorageCount; }

private:
    friend class CompositeCompletionOwnerPool;
    bool ProvePreparedCompositeCompletionCommit(const OriginalCompositeCompletionOwnerPin& original,
        const CompletionOwnerRef& owner, const CompletionProgressRow* progress,
        std::uint32_t renderGeneration, std::uint64_t scheduleEpoch,
        PreparedCompositeCompletionCommitProof* out) noexcept;
    bool ProvePreparedCompletionCommitInternal(const OriginalSingleCompletionOwnerPin& original,
        const CompletionOwnerRef& owner, const CompletionProgressRow* progress,
        std::uint32_t renderGeneration, std::uint64_t scheduleEpoch,
        PreparedSingleCompletionCommitProof* out, bool compositeFactory) noexcept;
    static void ConsumePreparedCompletionCommitInternal(PreparedSingleCompletionCommitProof&& proof) noexcept;
    void* m_originalCompositeCommitContext = nullptr;
    OriginalCompositePreparedCommitResolver m_originalCompositeCommitResolver = nullptr;
    bool TryPinOriginalCompositeCompletionOwner(const OccurrenceToken& token,
        const CompletionOwnerRef& owner, OriginalCompositeCompletionOwnerPin* original);
    bool TryPinCompletionOwnerInternal(const OccurrenceToken& token,
        const CompletionOwnerRef& owner, OriginalSingleCompletionOwnerPin* original,
        bool compositeFactory);
    friend class PrivateSelectedQueueFutureSpanPool;
    friend struct detail::OccurrenceStatePoolTestAccess;
    friend struct detail::CompletionOwnerTestAccess;
    friend struct detail::PreparedCompositeLeafIssuerLimitTestAccess;

    struct Entry {
        std::uint32_t generation = 1; // zero is invalid; max is terminal
        bool quarantined = false;
        OccurrenceStage stage = OccurrenceStage::Free;
        OccurrenceKey key{};
        std::uint64_t scheduleEpoch = 0;
        bool preparedReservation = false;
        bool originalSinglePreparedCommitHeld = false;
        // Only an actual native end can retire separately retained storage.
        bool originalCompositeStorageRetired = false;
        // Immutable acquisition episode, independent of the mutable schedule epoch.
        std::uint64_t originalStorageEpisode = 0;
        std::uint32_t originalStoragePinCount = 0;
        bool originalCompositeStorageAdvanceGeneration = false;
        CompletionProgressRow completionProgress{};
        bool completionProgressBound = false;
        CompletionOwnerBackref completionOwner{};
        // Immutable issuer metadata while an original structural loan exists.
        std::uint64_t originalSingleMarker = 0;
        std::uint32_t originalSinglePinCount = 0;
        CompletionOwnerRef originalSingleOwner{};
        std::uint32_t originalSingleRenderGeneration = 0;
        std::uint64_t originalSingleScheduleEpoch = 0;
        bool completionOwnerReleaseFenceArmed = false;
        bool completionOwnerReleaseTerminal = false;
        std::uint64_t completionOwnerReleaseAfterBlock = 0;
    };

    // Resolves a token to its entry, or nullptr when the slot is out of range,
    // free, or holds a different generation.
    const Entry* Find(const OccurrenceToken& token) const;
    Entry* FindMut(const OccurrenceToken& token);
    bool AcquireInternal(const OccurrenceKey& key,
                         OccurrenceToken* outToken, bool prepared,
                         OriginalStoragePin* original = nullptr);
    bool MatchesOriginalStoragePin(const OriginalStoragePin& original) const noexcept;
    OriginalStoragePin* ResolveOriginalStorageBaseline(OccurrenceToken token,
        OriginalStoragePin* explicitBaseline) noexcept;
    void* m_originalStorageBaselineContext = nullptr;
    OriginalStorageBaselineResolver m_originalStorageBaselineResolver = nullptr;
    bool OriginalStorageBaselineMatches(const OriginalStoragePin* original,
        OccurrenceToken token) const noexcept;

    bool MatchesPreparedSingleProof(const PreparedSingleCompletionCommitProof& proof) const noexcept;
    static void ClearOriginalSingleMetadata(Entry& entry) noexcept;
    void RetireOriginalStorage(Entry& entry, bool afterFence) noexcept;
    void RecycleOriginalStorage(Entry& entry) noexcept;
    bool MatchesOriginalSingleCompletionOwnerPin(
        const OriginalSingleCompletionOwnerPin& original) const noexcept;
    bool BorrowOriginalSingleCompletionOwnerPin(
        const OriginalSingleCompletionOwnerPin& original,
        OriginalSingleCompletionOwnerPin* out) noexcept;
    void ReleaseOriginalSingleCompletionOwnerPin(
        OriginalSingleCompletionOwnerPin& original) noexcept;
    OriginalSingleCompletionOwnerPin* ResolveOriginalSingleBaseline(const OccurrenceToken& token,
        OriginalSingleCompletionOwnerPin* explicitBaseline) noexcept;
    void* m_originalSingleResolverContext = nullptr;
    OriginalSingleBaselineResolver m_originalSingleBaselineResolver = nullptr;
    OriginalSinglePreparedCommitResolver m_originalSinglePreparedCommitResolver = nullptr;
    bool OriginalSingleRetirementReady(const OccurrenceToken& token,
        const OriginalSingleCompletionOwnerPin* originalBaseline) const noexcept;
    std::uint64_t m_nextOriginalSingleMarker = 1;
    bool m_originalSingleMarkerExhausted = false;
    std::vector<Entry> m_entries;
    std::uint64_t m_nextScheduleEpoch = 1;
    bool m_scheduleEpochExhausted = false;
    std::uint32_t m_occupiedCount = 0;
    std::uint32_t m_awaitingFenceCount = 0;
    std::uint32_t m_highWater = 0;
    std::uint32_t m_quarantinedCount = 0;
    std::uint32_t m_retainedStorageCount = 0;
    void (*m_afterCompletionOwnerClearHookForTest)(void*) = nullptr;
    void* m_afterCompletionOwnerClearHookContextForTest = nullptr;
};

} // namespace harken

#endif // HARKEN_CORE_OCCURRENCE_STATE_POOL_H
