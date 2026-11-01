// The playable-instance table: one generational identity per live instance of
// a playable content object, plus the parent edge that makes an instance tree
// out of them.
//
// A handle is (32-bit slot, 32-bit generation) packed into 64 bits, with zero
// never valid. Generation exhaustion permanently quarantines a slot instead
// of wrapping it back to an old identity: a caller may hold a handle after the
// instance behind it is gone, and reuse must make that handle read as stale
// rather than as somebody else's instance.
//
// Clips are deliberately absent. A clip compiles to a template plus lineage
// and never becomes a public instance; its live state lives in the separate
// fixed occurrence pool, whose slots carry their own non-public token. That
// split is what lets one transaction check the owning instance and a single
// occurrence independently.
//
// Retirement is three-staged because the two completion points differ. An
// instance reaches LogicalComplete when it will produce nothing further,
// AudibleRetiring while tails and child instances are still sounding, and
// Retired only once nothing is left to wait for. Collapsing the middle stage
// would either recycle a slot that is still audible or close the logical side
// while it is still expected to schedule.
//
// Live identity dies when the advance into Retired bumps the
// generation, so every handle to that instance goes stale there. The slot
// becomes reusable in the same step unless that generation is exhausted, in
// which case it is quarantined permanently. Acquire therefore never needs to
// bump — one generation move per lifetime, at the point the instance ends.
// Owned cancellation of an unpublished preparation also ends that identity
// and advances its generation, so raw handles cannot mutate a reused slot.
//
// Cancellation works through scheduleEpoch rather than by hunting down
// outstanding work: bumping the epoch makes every transaction stamped with
// the old value fail its check on arrival. The epoch is never 0, so an
// unstamped record cannot be mistaken for a current one.
//
// Logic thread only. Storage keeps its original address and prepares before
// acquisition; lifecycle advance and lookup never grow storage.
#ifndef HARKEN_CORE_PLAYABLE_INSTANCE_TABLE_H
#define HARKEN_CORE_PLAYABLE_INSTANCE_TABLE_H
#include "harken/core/StableRowStorage.h"

#include "harken/core/CheckedIssuer.h"
#include "harken/core/MonitorIdentity.h"
#include "harken/core/MonitorRequest.h"
#include "harken/core/MonitorLifecycleSink.h"

#include <cstdint>
#include <span>
#include <vector>

namespace harken {

namespace detail {
struct PlayableInstanceTableTestAccess;
struct PlayableCancellationTestAccess;
struct PlayableInstancePreparationTestAccess;
struct PlayableInstanceDeadlineTestAccess;
}

// Packed (slot, generation). 0 is never a live instance.
using PlayableInstanceHandle = std::uint64_t;

// Which content type an instance is an instance of. Sound, Container, Track
// and Segment are the four directly playable types; the table keeps the kind so
// callers can reject a handle of the wrong type without a second lookup.
enum class PlayableKind : std::uint8_t {
    Sound = 1,
    Container = 2,
    Track = 3,
    Segment = 4
};

// Lifecycle stages, in the only order they may be traversed. Free and
// non-quarantined Retired are reusable by acquire; they differ only in whether
// the slot has ever been used, which keeps "retired" readable as a fact rather
// than erased.
enum class PlayableLifecycle : std::uint8_t {
    Free = 0,
    Active = 1,
    LogicalComplete = 2,
    AudibleRetiring = 3,
    Retired = 4
};

// A terminal outcome is immutable once published. The result and reason
// fields remain numeric here so the core table does not depend on an API-layer
// result-code header.
enum class PlayableTerminalKind : std::uint8_t {
    None = 0,
    NaturalCompletion = 1,
    RequestedStop = 2,
    ParentCancellation = 3,
    ReplacementReset = 4,
    Failure = 5,
};

struct PlayableTerminalOutcome {
    PlayableTerminalKind kind = PlayableTerminalKind::None;
    std::int32_t resultCode = 0;
    std::uint32_t reasonCode = 0;
    std::uint64_t naturalCompletionOrdinal = 0;
};

// What a handle currently names. A retired instance reports live == false: its
// generation has already moved, so the handle names nobody.
struct PlayableInstanceView {
    bool live = false; // false for a stale handle; other fields are then zeroed
    PlayableKind kind = PlayableKind::Sound;
    PlayableLifecycle lifecycle = PlayableLifecycle::Free;
    PlayableInstanceHandle parent = 0; // 0 for a root instance
    PlayableInstanceHandle firstChild = 0;
    PlayableInstanceHandle nextSibling = 0;
    std::uint64_t scheduleEpoch = 0;   // never 0 while live
    PlayableTerminalOutcome terminalOutcome{};
};

struct PlayableRootMetadata {
    VnObjectId projectId{};
    PlayableKind definitionKind = PlayableKind::Sound;
    VnObjectId definitionStableId{};
    std::uint64_t programSemanticFingerprint = 0;
    std::uint64_t invocationEmitter = 0;
    std::uint64_t actionFiring = 0;
    std::uint32_t actionExecutionOrdinal = UINT32_MAX;
    std::uint64_t worldGeneration = 0;
};

struct PlayableRetirementObserver {
    std::uint64_t actionFiring = 0;
    std::uint32_t actionExecutionOrdinal = UINT32_MAX;
};

class PlayableInstanceTable;
class PreparedPlayableTreeStep;

class PreparedPlayableInstance {
public:
    PreparedPlayableInstance() = default;
    PreparedPlayableInstance(const PreparedPlayableInstance&) = default;
    PreparedPlayableInstance(PreparedPlayableInstance&&) noexcept = default;
    PreparedPlayableInstance& operator=(const PreparedPlayableInstance& other) noexcept;
    PreparedPlayableInstance& operator=(PreparedPlayableInstance&& other) noexcept;
    bool Valid() const noexcept;
    PlayableInstanceHandle Handle() const noexcept { return handle_; }
    PlayableInstanceHandle Parent() const noexcept { return parent_; }
    std::uint64_t ScheduleEpoch() const noexcept { return scheduleEpoch_; }

private:
    friend class PlayableInstanceTable;
    friend class PreparedPlayableTreeStep;
    PlayableInstanceTable* owner_ = nullptr;
    PlayableInstanceHandle handle_ = 0;
    PlayableInstanceHandle parent_ = 0;
    std::uint64_t scheduleEpoch_ = 0;
};

class PreparedPlayableCancellation {
public:
    bool Valid() const noexcept;
    PlayableInstanceHandle Handle() const noexcept { return handle_; }

private:
    friend class PlayableInstanceTable;
    PlayableInstanceTable* owner_ = nullptr;
    PlayableInstanceHandle handle_ = 0;
    PlayableRetirementObserver observer_{};
};

class DelayedPlayableCancellation {
public:
    DelayedPlayableCancellation() = default;
    ~DelayedPlayableCancellation();
    DelayedPlayableCancellation(const DelayedPlayableCancellation&) = delete;
    DelayedPlayableCancellation& operator=(const DelayedPlayableCancellation&) = delete;
    DelayedPlayableCancellation(DelayedPlayableCancellation&& other) noexcept;
    DelayedPlayableCancellation& operator=(DelayedPlayableCancellation&& other) noexcept;
    bool Valid() const noexcept;
    PlayableInstanceHandle Handle() const noexcept { return handle_; }
    std::uint64_t SourceEpoch() const noexcept { return sourceEpoch_; }

private:
    friend class PlayableInstanceTable;
    void Clear() noexcept;
    PlayableInstanceTable* owner_ = nullptr;
    PlayableInstanceHandle handle_ = 0;
    std::uint64_t sourceEpoch_ = 0;
    std::uint64_t issuedEpoch_ = 0;
    PlayableRetirementObserver observer_{};
    bool accepted_ = false;
};

class PlayableInstanceTable {
    struct Entry;
public:
    // Capacity is the number of concurrently live instances. Reservation
    // fixes every row's address before any original lifetime can name it.
    explicit PlayableInstanceTable(std::uint32_t capacity);
    bool PrepareStorage() noexcept {
        return m_entries.ConstructThrough(m_entries.Capacity());
    }
    // Prepare the reusable rows needed by one transaction without changing capacity.
    bool PrepareStorageForAcquire(std::uint32_t count) noexcept;
    std::size_t StorageBytes() const noexcept { return m_entries.RequestedBytes(); }
    std::size_t StorageCommittedBytes() const noexcept { return m_entries.CommittedBytes(); }
    std::size_t StorageReservedBytes() const noexcept { return m_entries.ReservedBytes(); }
    void SetCommitPageLimitForTests(std::size_t pages) noexcept {
        m_entries.SetCommitPageLimitForTests(pages);
    }
    ~PlayableInstanceTable();
    PlayableInstanceTable(const PlayableInstanceTable&) = delete;
    PlayableInstanceTable& operator=(const PlayableInstanceTable&) = delete;
    PlayableInstanceTable(PlayableInstanceTable&&) = delete;
    PlayableInstanceTable& operator=(PlayableInstanceTable&&) = delete;

    class PreparedCommitProof {
    public:
        PreparedCommitProof() = default;
        ~PreparedCommitProof() { Release(); }
        PreparedCommitProof(const PreparedCommitProof&) = delete;
        PreparedCommitProof& operator=(const PreparedCommitProof&) = delete;
        PreparedCommitProof(PreparedCommitProof&& other) noexcept;
        PreparedCommitProof& operator=(PreparedCommitProof&& other) noexcept;
        bool Valid() const noexcept { return entry_ != nullptr; }
        bool AssociatedWith(const PreparedPlayableInstance& original) const noexcept {
            return Valid() && original_ == &original;
        }
        void Release() noexcept;
    private:
        friend class PlayableInstanceTable;
        PlayableInstanceTable* owner_ = nullptr;
        Entry* entry_ = nullptr;
        PreparedPlayableInstance* original_ = nullptr;
        PlayableInstanceHandle handle_ = 0;
    };
    bool PreparedCommitHeld(const PreparedPlayableInstance& original) const noexcept;
    bool ProvePreparedCommit(PreparedPlayableInstance& original,
        PreparedCommitProof* out) noexcept;
    static void ConsumePreparedCommit(PreparedCommitProof&& proof) noexcept;

    // Logic-thread structural storage loan issued only by a held native factory.
    // It never freezes normal epochs, terminal facts or Source progress.
    class OriginalLifetimePin {
    public:
        OriginalLifetimePin() noexcept = default;
        ~OriginalLifetimePin() { Release(); }
        OriginalLifetimePin(const OriginalLifetimePin&) = delete;
        OriginalLifetimePin& operator=(const OriginalLifetimePin&) = delete;
        OriginalLifetimePin(OriginalLifetimePin&& other) noexcept;
        OriginalLifetimePin& operator=(OriginalLifetimePin&& other) noexcept;
        bool Valid() const noexcept;
        bool Borrow(OriginalLifetimePin* out) const noexcept;
        void Release() noexcept;
        PlayableInstanceHandle OriginalHandle() const noexcept { return handle_; }
        PlayableInstanceHandle OriginalParent() const noexcept;
        std::uint64_t OriginalScheduleEpoch() const noexcept;
    private:
        friend class PlayableInstanceTable;
        PlayableInstanceTable* owner_ = nullptr;
        Entry* entry_ = nullptr;
        PlayableInstanceHandle handle_ = 0;
        std::uint64_t marker_ = 0;
    };
    bool IssueOriginalPreparedLifetime(const PreparedCommitProof& preparation,
        OriginalLifetimePin* out) noexcept;
    bool IsOriginalLifetimePinFor(const OriginalLifetimePin& original,
        PlayableInstanceHandle handle) const noexcept;
    bool IsOriginalLifetimeCurrent(const OriginalLifetimePin& original) const noexcept;
    bool CaptureOriginalPreparedNodeKind(const PreparedPlayableInstance& prepared,
        const OriginalLifetimePin& original, PlayableKind* kind) const noexcept;
    // A genuine native end request can return its own structural associations
    // before Arm/Occurrence tails finish. This hold retains the actual Core row;
    // only its Return permits the existing final native Retired operation.
    class OriginalLifetimeRetirementHold {
    public:
        OriginalLifetimeRetirementHold() noexcept = default;
        OriginalLifetimeRetirementHold(const OriginalLifetimeRetirementHold&) = delete;
        OriginalLifetimeRetirementHold& operator=(const OriginalLifetimeRetirementHold&) = delete;
        OriginalLifetimeRetirementHold(OriginalLifetimeRetirementHold&& other) noexcept;
        OriginalLifetimeRetirementHold& operator=(OriginalLifetimeRetirementHold&& other) noexcept;
        ~OriginalLifetimeRetirementHold() { Release(); }
        bool Valid() const noexcept;
        PlayableInstanceHandle OriginalHandle() const noexcept { return loan_.OriginalHandle(); }
        void Release() noexcept;
    private:
        friend class PlayableInstanceTable;
        PlayableInstanceTable* owner_ = nullptr;
        OriginalLifetimePin loan_;
    };
    bool BeginOriginalLifetimeRetirement(OriginalLifetimePin& originalBaseline,
        OriginalLifetimeRetirementHold* out) noexcept;
    bool IsOriginalLifetimeRetirementHoldFor(const OriginalLifetimeRetirementHold& original,
        PlayableInstanceHandle handle) const noexcept;
    bool TryFinishOriginalLifetimeDisposition(OriginalLifetimePin& originalBaseline) noexcept;
    bool HasRequestedOriginalLifetimeDisposition(const OriginalLifetimePin& original) const noexcept;
    bool HasRequestedOriginalLifetimeDispositions() const noexcept {
        return m_originalDispositionCount != 0;
    }
    using OriginalLifetimeBaselineResolver = OriginalLifetimePin* (*)(
        void*, PlayableInstanceTable&, PlayableInstanceHandle) noexcept;
    using OriginalLifetimeDispositionFn = void (*)(
        void*, PlayableInstanceTable&, PlayableInstanceHandle) noexcept;
    bool RegisterOriginalLifetimeBaselineResolver(void* context,
        OriginalLifetimeBaselineResolver resolver,
        OriginalLifetimeDispositionFn disposition = nullptr) noexcept;

    struct ParentLinkRequest {
        PreparedPlayableInstance* original = nullptr;
        PreparedCommitProof* activation = nullptr;
    };
    class PreparedParentLinksProof {
    public:
        PreparedParentLinksProof() = default;
        explicit PreparedParentLinksProof(std::size_t capacity);
        bool PrepareStorage(std::size_t capacity) noexcept;
        // Dynamic vector backing only; the owning object accounts for inline storage.
        std::size_t StorageBytes() const noexcept {
            return nodes_.capacity() * sizeof(Node) +
                anchors_.capacity() * sizeof(Node) +
                held_.capacity() * sizeof(HeldEntry) +
                groups_.capacity() * sizeof(Group);
        }
        ~PreparedParentLinksProof() { Release(); }
        PreparedParentLinksProof(const PreparedParentLinksProof&) = delete;
        PreparedParentLinksProof& operator=(const PreparedParentLinksProof&) = delete;
        PreparedParentLinksProof(PreparedParentLinksProof&& other) noexcept;
        PreparedParentLinksProof& operator=(PreparedParentLinksProof&& other) noexcept;
        bool Valid() const noexcept;
        bool MatchesOriginalScope(const PlayableInstanceTable* expectedOwner,
            std::span<const ParentLinkRequest> requests,
            std::span<const ParentLinkRequest> anchors = {}) const noexcept;
        void Release() noexcept;
    private:
        friend class PlayableInstanceTable;
        struct Node {
            ParentLinkRequest request{};
            Entry* entry = nullptr;
            PlayableInstanceHandle handle = 0;
            PlayableInstanceHandle parent = 0;
            std::uint64_t epoch = 0;
            std::uint64_t parentEpoch = 0;
            std::uint32_t next = UINT32_MAX;
        };
        struct HeldEntry {
            Entry* entry = nullptr;
            PlayableInstanceHandle handle = 0;
            std::uint64_t epoch = 0;
            PlayableInstanceHandle parent = 0;
            PlayableInstanceHandle head = 0;
            PlayableInstanceHandle next = 0;
        };
        struct Group {
            Entry* parent = nullptr;
            Entry* tail = nullptr;
            std::uint32_t first = UINT32_MAX;
            std::uint32_t last = UINT32_MAX;
        };
        PlayableInstanceTable* owner_ = nullptr;
        std::vector<Node> nodes_;
        std::vector<Node> anchors_;
        std::vector<HeldEntry> held_;
        std::vector<Group> groups_;
        // Zero means no backing; explicit capacity zero still permits a root anchor.
        std::size_t anchorCapacity_ = 0;
        // Both counts are bounded by the constructor's uint32 capacity.
        std::uint32_t nodeCount_ = 0;
        std::uint32_t groupCount_ = 0;
        std::size_t anchorCount_ = 0;
        std::size_t heldCount_ = 0;
    };
    bool ParentLinksHeld(PlayableInstanceHandle handle) const noexcept;
    bool ProvePreparedParentLinks(std::span<const ParentLinkRequest> requests,
        std::span<const ParentLinkRequest> anchors, PreparedParentLinksProof* out) noexcept;
    // Authenticates ancestry only from this genuine held preparation graph.
    // No numeric handle lookup can issue a parent relationship through it.
    bool MatchesPreparedOriginalAncestry(const PreparedParentLinksProof& proof,
        PlayableInstanceHandle descendant, PlayableInstanceHandle ancestor) const noexcept;
    struct PreparedNodeCensusEntry {
        const PreparedPlayableInstance* original = nullptr;
        PlayableKind expectedKind = PlayableKind::Sound;
    };
    bool MatchesPreparedOriginalNodeCensus(const PreparedParentLinksProof& proof,
        std::span<const PreparedNodeCensusEntry> nodes) const noexcept;
    bool IssueOriginalPreparedLifetime(const PreparedParentLinksProof& preparation,
        const PreparedPlayableInstance& exactOriginal, OriginalLifetimePin* out) noexcept;
    // Stable output slots are selected only from the actual held native nodes.
    bool IssueOriginalPreparedLifetimeBaselines(const PreparedParentLinksProof& preparation,
        std::span<OriginalLifetimePin> stableBaselines) noexcept;
    static void ConsumePreparedParentLinks(PreparedParentLinksProof&& proof) noexcept;

    // Logical capacity reserved at construction. FreeCount reports the
    // currently reusable subset after generation exhaustion is accounted for.
    std::uint32_t Capacity() const {
        return m_entries.Capacity();
    }
    std::uint32_t ConstructedEntryCount() const noexcept { return m_entries.ConstructedCount(); }

    // Reserves a non-quarantined slot for a new instance and returns its
    // handle, or 0 when no reusable slot remains. parent is the arranging or
    // triggering instance, or 0 for a root. A parent handle that is already
    // stale is refused rather than silently rooted, because an instance
    // orphaned that way would outlive the tree it belonged to.
    PlayableInstanceHandle Acquire(PlayableKind kind,
                                   PlayableInstanceHandle parent,
                                   const MonitorRequestContext& request = {},
                     const MonitorEmitterOrigin& emitterOrigin = {});
    bool PrepareAcquire(PlayableKind kind, PlayableInstanceHandle parent,
                        PreparedPlayableInstance* out,
                        const MonitorRequestContext& request = {},
                     const MonitorEmitterOrigin& emitterOrigin = {}) noexcept;
    bool PrepareAcquireFromPreparedStorage(PlayableKind kind, PlayableInstanceHandle parent,
                        PreparedPlayableInstance* out,
                        const MonitorRequestContext& request = {},
                        const MonitorEmitterOrigin& emitterOrigin = {}) noexcept;
    bool SetPreparedRootMetadata(
        const PreparedPlayableInstance& prepared,
        const PlayableRootMetadata& metadata) noexcept;
    bool SetPreparedContentIdentity(const PreparedPlayableInstance& prepared,
                                    const MonitorContentIdentity& identity) noexcept;
    bool SetPreparedProvenance(const PreparedPlayableInstance& prepared,
                                const MonitorPlayableProvenance& provenance) noexcept;
    bool CaptureContentIdentity(PlayableInstanceHandle handle,
                                MonitorContentIdentity* out) const noexcept;
    bool CaptureEmitterOrigin(PlayableInstanceHandle handle,
                              MonitorEmitterOrigin* out) const noexcept;
    bool CapturePlaybackRequest(PlayableInstanceHandle handle,
                                MonitorRequestContext* out) const noexcept;
    bool CaptureControlCause(PlayableInstanceHandle handle,
                             MonitorControlCause* out) const noexcept;
    void RecordControlCause(PlayableInstanceHandle handle,
                            const MonitorControlCause& cause) noexcept;
    template <typename Visitor>
    void VisitPublishedSubtree(PlayableInstanceHandle root, Visitor&& visitor) const noexcept {
        auto current = root;
        for (std::size_t visited = 0; current != 0 && visited < m_entries.size(); ++visited) {
            const auto* entry = FindAny(current);
            if (entry == nullptr) return;
            visitor(current);
            if (entry->firstChild != 0) {
                current = entry->firstChild;
                continue;
            }
            for (std::size_t depth = 0; depth < m_entries.size(); ++depth) {
                if (current == root) return;
                entry = FindAny(current);
                if (entry == nullptr) return;
                if (entry->nextSibling != 0) {
                    current = entry->nextSibling;
                    break;
                }
                current = entry->parent;
            }
        }
    }
    bool CommitPrepared(PreparedPlayableInstance* prepared,
                        bool publishParentLink = true) noexcept;
    bool PublishPreparedLink(PlayableInstanceHandle handle) noexcept;
    bool WithdrawUnpublishedParentLink(const PreparedPlayableInstance& child,
        PlayableInstanceHandle parent, std::uint64_t expectedParentEpoch) noexcept;
    bool RollbackCommitted(PlayableInstanceHandle handle) noexcept;
    bool CancelPrepared(PreparedPlayableInstance* prepared,
        OriginalLifetimePin* originalBaseline = nullptr) noexcept;
    bool AcquireSourceRetirementHold(PlayableInstanceHandle handle,
                                      std::uint64_t acceptedEpoch) noexcept;
    bool ReleaseSourceRetirementHold(PlayableInstanceHandle handle) noexcept;
    bool HasSourceRetirementHold(PlayableInstanceHandle handle) const noexcept;
    std::uint64_t SourceRetirementEpoch(PlayableInstanceHandle handle) const noexcept;
    std::uint32_t SourceRetirementSlot(PlayableInstanceHandle handle) const noexcept;
    // Copy only committed identity values; prepared and retired slots are absent.
    PlayableInstanceHandle CaptureMonitorIdentityAt(std::uint32_t slot,
        MonitorLifecycleFacts* out) const noexcept;
    bool CaptureMonitorFacts(PlayableInstanceHandle handle,
                             MonitorLifecycleFacts* out) const noexcept;
    bool NeedsStartedObservation(PlayableInstanceHandle handle) const noexcept;
    bool HasPendingStartedObservations() const noexcept { return m_pendingStartedObservationCount != 0; }
    bool HasPendingPlaybackEndObservations() const noexcept { return m_pendingPlaybackEndObservationCount != 0; }
    void ObserveStarted(PlayableInstanceHandle handle, std::uint64_t audioBlock,
                        std::uint64_t frame) noexcept;
    void RecordPlaybackEnd(PlayableInstanceHandle handle, std::uint64_t audioBlock,
        std::uint64_t frame, const PlayableTerminalOutcome& outcome,
        const MonitorControlCause& cause) noexcept;
    PlayableInstanceHandle CapturePlaybackEndAt(std::uint32_t slot,
        std::uint64_t* audioBlock, std::uint64_t* frame) const noexcept;
    void ObserveFinished(PlayableInstanceHandle handle) noexcept;
    bool CaptureRootMetadata(PlayableInstanceHandle handle,
                             PlayableRootMetadata* out) const noexcept;

    bool PrepareCancellation(PlayableInstanceHandle handle,
                             std::uint64_t observerActionFiring,
                             std::uint32_t observerExecutionOrdinal,
                             PreparedPlayableCancellation* out) noexcept;
    bool CommitPreparedCancellation(
        PreparedPlayableCancellation* prepared, const MonitorControlCause* cause = nullptr) noexcept;
    bool CancelPreparedCancellation(
        PreparedPlayableCancellation* prepared) noexcept;
    bool PrepareDelayedCancellation(
        PlayableInstanceHandle handle, std::uint64_t observerActionFiring,
        std::uint32_t observerExecutionOrdinal,
        DelayedPlayableCancellation* out) noexcept;
    bool CommitPreparedDelayedCancellation(
        DelayedPlayableCancellation* cancellation) noexcept;
    bool ActivateDelayedCancellation(
        DelayedPlayableCancellation* cancellation, const MonitorControlCause* cause = nullptr) noexcept;
    bool CancelDelayedCancellation(
        DelayedPlayableCancellation* cancellation) noexcept;
    bool HasCancellationClaim(PlayableInstanceHandle handle) const noexcept;
    bool CaptureRetirementObserver(
        PlayableInstanceHandle handle,
        PlayableRetirementObserver* out) const noexcept;
    bool DetachRetirementObserver(
        PlayableInstanceHandle handle,
        const PlayableRetirementObserver& expected) noexcept;
    bool TakeRetirementObserver(
        PlayableInstanceHandle handle,
        PlayableRetirementObserver* out) noexcept;

    // How a handle resolves right now. A stale handle yields live == false.
    PlayableInstanceView Classify(PlayableInstanceHandle handle) const;

    // Child links are intrusive and owned by this table. A stale handle or a
    // non-child edge returns zero; callers never need a second tree owner.
    PlayableInstanceHandle FirstChild(
        PlayableInstanceHandle handle) const noexcept;
    PlayableInstanceHandle NextSibling(
        PlayableInstanceHandle handle) const noexcept;

    // Publishes one terminal outcome. Repeated publication, or publication of
    // the empty outcome, is rejected so parent policy can trust the record.
    bool SetTerminalOutcome(PlayableInstanceHandle handle,
                            PlayableTerminalOutcome outcome) noexcept;

    // Advances one instance to a later stage. Advancing to the stage it is
    // already in, or to an earlier one, is refused and changes nothing: the
    // order is the contract, and letting a caller walk backwards would make
    // "no longer audible" a reversible claim. Advancing to Retired also ends
    // the instance's identity and frees its slot.
    bool AdvanceLifecycle(PlayableInstanceHandle handle,
                          PlayableLifecycle to, OriginalLifetimePin* originalBaseline = nullptr);

    // Bumps the instance's schedule epoch and returns the new value, or 0 for
    // a stale handle. Every transaction stamped with the previous epoch now
    // fails validation, which is how a whole-instance cancel takes effect
    // without visiting the outstanding records one by one.
    IssueResult BumpScheduleEpochChecked(PlayableInstanceHandle handle, const MonitorControlCause* cause = nullptr);
    bool CanBumpScheduleEpoch(PlayableInstanceHandle handle) const;
    bool CanBumpScheduleEpochs(std::uint32_t count) const;
    std::uint64_t BumpScheduleEpoch(PlayableInstanceHandle handle) {
        return BumpScheduleEpochChecked(handle).value;
    }
    bool ScheduleEpochExhausted() const { return m_scheduleEpochExhausted; }

    // Whether a stamped record still belongs to the instance it names. False
    // for a stale handle and for a superseded epoch, so one check covers both
    // "the instance is gone" and "the instance cancelled this".
    bool ValidateStamp(PlayableInstanceHandle handle,
                       std::uint64_t scheduleEpoch) const;
    bool IsPrepared(PlayableInstanceHandle handle) const noexcept;
    bool OwnsPrepared(
        const PreparedPlayableInstance& prepared) const noexcept;

    // Live instances, and the most that were ever live at once.
    std::uint32_t LiveCount() const { return m_liveCount; }
    std::uint32_t HighWater() const { return m_highWater; }

    // Slots that can still accept a new instance, excluding exhausted slots.
    std::uint32_t FreeCount() const;
    std::uint32_t QuarantinedCount() const { return m_quarantinedCount; }

private:
    friend class PreparedPlayableCancellation;
    friend class DelayedPlayableCancellation;
    friend class PreparedPlayableTreeStep;
    friend struct detail::PlayableInstanceTableTestAccess;
    friend struct detail::PlayableCancellationTestAccess;
    friend struct detail::PlayableInstancePreparationTestAccess;
    friend struct detail::PlayableInstanceDeadlineTestAccess;

    struct Entry {
        std::uint32_t generation = 1; // zero is invalid; max is terminal
        bool quarantined = false;
        bool prepared = false;
        bool preparationCommitHeld = false;
        bool preparingCommitOwned = false;
        bool privateLinksHeld = false;
        struct PreparationInput {
            PlayableInstanceHandle handle = 0;
            PlayableKind kind = PlayableKind::Sound;
            PlayableLifecycle lifecycle = PlayableLifecycle::Free;
            PlayableInstanceHandle parent = 0;
            std::uint64_t parentEpoch = 0;
            std::uint64_t epoch = 0;
            PlayableRootMetadata metadata{};
            bool sourceHold = false;
        } preparationInput{};
        PreparationInput originalLifetimeInput{};
        std::uint64_t originalLifetimeMarker = 0;
        std::uint32_t originalLifetimeCount = 0;
        enum class OriginalDisposition : std::uint8_t { None, PreparedCancel, Retired };
        OriginalDisposition originalDisposition = OriginalDisposition::None;
        PreparedPlayableInstance originalDispositionPrepared{};
        bool originalLifetimeEnding = false;
        bool originalLifetimeRetirementHeld = false;
        // An actual original cancellation, never a reusable-slot observation.
        PreparedPlayableInstance completedOriginalCancellation{};
        bool sourceRetirementHold = false;
        PlayableKind kind = PlayableKind::Sound;
        PlayableLifecycle lifecycle = PlayableLifecycle::Free;
        PlayableInstanceHandle parent = 0;
        PlayableInstanceHandle firstChild = 0;
        PlayableInstanceHandle nextSibling = 0;
        std::uint64_t scheduleEpoch = 0;
        PlayableRootMetadata rootMetadata{};
        MonitorContentIdentity contentIdentity{};
        MonitorPlayableProvenance playableProvenance{};
        MonitorContentIdentity monitorPreparedCandidateIdentity{};
        bool monitorPreparedCandidateObserved = false;
        bool monitorStartedObserved = false;
        bool monitorFinishedObserved = false;
        struct PlaybackEnd {
            std::uint64_t audioBlock = 0;
            std::uint64_t frame = 0;
            PlayableTerminalOutcome outcome{};
            MonitorControlCause cause{};
        } monitorPlaybackEnd{};
        MonitorRequestContext playbackRequest{};
        MonitorEmitterOrigin emitterOrigin{};
        MonitorControlCause controlCause{};
        bool controlCauseAccepted = false;
        PlayableTerminalOutcome terminalOutcome{};
        bool cancellationPrepared = false;
        PlayableRetirementObserver preparedObserver{};
        PlayableRetirementObserver retirementObserver{};
        std::uint64_t delayedCancellationEpoch = 0;
    };

    void FillMonitorFacts(const Entry& entry, PlayableInstanceHandle handle,
                          MonitorLifecycleFacts& out) const noexcept;
    static PlayableInstanceHandle MakeHandle(std::uint32_t slot,
                                             std::uint32_t generation) {
        return (static_cast<std::uint64_t>(generation) << 32) | slot;
    }
    static std::uint32_t SlotOf(PlayableInstanceHandle handle) {
        return static_cast<std::uint32_t>(handle & 0xFFFFFFFFu);
    }
    static std::uint32_t GenerationOf(PlayableInstanceHandle handle) {
        return static_cast<std::uint32_t>(handle >> 32);
    }

    // Resolves a handle to its entry, or nullptr when the slot is out of
    // range, reusable, or holds a different generation.
    const Entry* Find(PlayableInstanceHandle handle) const;
    Entry* FindMut(PlayableInstanceHandle handle);
    const Entry* FindAny(PlayableInstanceHandle handle) const noexcept;
    Entry* FindAnyMut(PlayableInstanceHandle handle) noexcept;
    Entry* FindPrepared(const PreparedPlayableInstance& prepared) noexcept;
    void ActivatePreparedEntry(Entry& entry, PlayableInstanceHandle handle) noexcept;
    void RemovePendingStartedObservation(const Entry& entry) noexcept;
    void RemovePendingPlaybackEndObservation(const Entry& entry) noexcept;
    bool IsCancellationPrepared(
        const PreparedPlayableCancellation& prepared) const noexcept;
    bool OwnsDelayedCancellation(
        const DelayedPlayableCancellation& cancellation,
        bool requireSourceEpoch = true) const noexcept;
    bool LinkChild(PlayableInstanceHandle child,
                   PlayableInstanceHandle parent) noexcept;
    bool UnlinkChild(PlayableInstanceHandle child,
                     PlayableInstanceHandle parent) noexcept;
    bool ValidatePreparedParentLinksScope(const PreparedParentLinksProof& proof,
        std::span<const ParentLinkRequest> requests,
        std::span<const ParentLinkRequest> anchors) const noexcept;

    bool CancelPreparedImpl(PreparedPlayableInstance* prepared,
        OriginalLifetimePin* originalBaseline, bool retainNativeReceipt) noexcept;
    static bool AvailableForAcquire(const Entry& entry) noexcept;
    bool PrepareAcquireInternal(PlayableKind kind, PlayableInstanceHandle parent,
        PreparedPlayableInstance* out, const MonitorRequestContext& request,
        const MonitorEmitterOrigin& emitterOrigin, bool prepareStorage) noexcept;
    bool MatchesOriginalLifetime(const OriginalLifetimePin& original) const noexcept;
    OriginalLifetimePin* FindOriginalLifetimeBaseline(Entry& entry,
        PlayableInstanceHandle handle, OriginalLifetimePin* supplied) noexcept;
    bool RequestOriginalLifetimeDisposition(Entry& entry, PlayableInstanceHandle handle,
        Entry::OriginalDisposition requested, OriginalLifetimePin* supplied,
        const PreparedPlayableInstance* prepared = nullptr) noexcept;
    OriginalLifetimePin* ResolveOriginalLifetimeBaseline(Entry& entry,
        PlayableInstanceHandle handle, OriginalLifetimePin* supplied) noexcept;
    bool CanUnlinkChild(PlayableInstanceHandle child, PlayableInstanceHandle parent) const noexcept;
    bool OriginalLifetimeRetirementReady(const Entry& entry,
        const OriginalLifetimePin* originalBaseline) const noexcept;
    bool BorrowOriginalLifetime(const OriginalLifetimePin& original, OriginalLifetimePin* out) noexcept;
    void ReturnOriginalLifetime(OriginalLifetimePin& original) noexcept;
    void ReturnOriginalLifetimeRetirementHold(OriginalLifetimeRetirementHold& original) noexcept;
    StableRowStorage<Entry> m_entries;
    std::uint32_t m_originalDispositionCount = 0;
    std::uint64_t m_nextOriginalLifetimeMarker = 1;
    bool m_originalLifetimeMarkerExhausted = false;
    void* m_originalLifetimeBaselineContext = nullptr;
    OriginalLifetimeBaselineResolver m_originalLifetimeBaselineResolver = nullptr;
    OriginalLifetimeDispositionFn m_originalLifetimeDisposition = nullptr;
    std::uint64_t m_nextScheduleEpoch = 1;
    bool m_scheduleEpochExhausted = false;
    std::uint32_t m_liveCount = 0;
    std::uint32_t m_pendingStartedObservationCount = 0;
    std::uint32_t m_pendingPlaybackEndObservationCount = 0;
    std::uint32_t m_highWater = 0;
    std::uint32_t m_quarantinedCount = 0;
    std::uint64_t m_preparedCancellationCount = 0;
};

} // namespace harken

#endif // HARKEN_CORE_PLAYABLE_INSTANCE_TABLE_H
