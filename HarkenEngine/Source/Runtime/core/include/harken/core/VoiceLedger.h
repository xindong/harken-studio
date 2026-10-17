// The Logical Voice ledger is the identity layer between the public
// VnVoiceHandle a caller holds and the internal Render Voice slot it may
// occupy. A public handle is a generational ledger handle (32-bit ledger
// slot + 31-bit generation, 0 never valid, exhausted generations quarantine
// the entry); the
// render-slot handle it maps to is engine-internal and never leaves this
// boundary — commands still carry render-slot handles, so the audio thread
// is untouched.
//
// Decoupling the two identities lets a Logical Voice's public handle outlive
// its Render Voice. Audibility parking releases that Render Voice while the
// resulting Virtual Voice retains its identity and timeline. With no such state
// active the ledger is a pure translation layer — every live voice maps
// one-to-one to a render slot, so observable behavior is unchanged.
//
// Entry states: Free (available), Live (mapped to a Render Voice handle),
// PendingStart (a deferred Logical Voice awaiting a Render Voice), Parked (a
// Virtual Voice whose identity and timeline remain alive without a Render Voice),
// and Ended (the voice finished and the entry awaits reuse).
//
// Logic thread only: the ledger is consulted and mutated on the pumping
// thread (the externally synchronized C API and Vn_Update). It never crosses
// to the audio thread. Storage is allocated once at construction; acquire,
// reap, and translation touch no heap.
#ifndef HARKEN_CORE_VOICE_LEDGER_H
#define HARKEN_CORE_VOICE_LEDGER_H

#include <harken/Harken.h>

#include "harken/core/CompletionMutationBinding.h"
#include "harken/core/MonitorLifecycleSink.h"
#include "harken/core/MonitorVoiceControl.h"
#include "harken/core/PlayableInstanceTable.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace harken {

#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
namespace detail { struct VoiceLedgerOriginalCompletionTestAccess; struct PreparedSingleBundleBorrowLimitTestAccess; struct CompositeLedgerIssuerLimitTestAccess; }
#endif

class VoiceLedger {
    struct Entry;
    struct ReverseRenderEntry;
    struct MonitorSelectionStorage;
public:
    static constexpr std::uint32_t kMaxGeneration = 0x7fffffffu;

    enum class PublicVisibility : std::uint8_t {
        DirectVoice = 0,
        PlayableOwnedInternal = 1,
    };

    struct PlayableParentRef {
        std::uint64_t previewHandle = 0;
        PlayableInstanceHandle internalToken = 0;
    };

    struct PreparedPlayableIdentity {
        std::uint32_t slot = UINT32_MAX;
        std::uint32_t generation = 0;
    };

    struct PreparedCommitInput {
        PreparedPlayableIdentity identity{};
        PlayableParentRef parent{};
        VnVoiceHandle renderHandle = 0;
        std::uint32_t priority = 128;
        CompletionMutationBinding completionMutation{};
    };

    // Logic-thread exclusion on the actual prepared entry and reverse row.
    // The table must outlive the proof; abandonment preserves cancellation.
    class PreparedCommitProof {
    public:
        PreparedCommitProof() = default;
        PreparedCommitProof(const PreparedCommitProof&) = delete;
        PreparedCommitProof& operator=(const PreparedCommitProof&) = delete;
        PreparedCommitProof(PreparedCommitProof&& other) noexcept;
        PreparedCommitProof& operator=(PreparedCommitProof&& other) noexcept;
        ~PreparedCommitProof();
        bool Valid() const noexcept { return entry_ != nullptr; }
        void Release() noexcept;
    private:
        friend class VoiceLedger;
        VoiceLedger* table_ = nullptr;
        Entry* entry_ = nullptr;
        ReverseRenderEntry* reverse_ = nullptr;
        std::uint32_t slot_ = UINT32_MAX;
        VnVoiceHandle renderHandle_ = 0;
        std::uint32_t priority_ = 128;
    };

    // Only a genuinely bound native Single preparation can issue this proof.
    // No token, Current snapshot or generic unbound VC input can construct it.
    class PreparedSingleCompletionCommitProof {
    public:
        PreparedSingleCompletionCommitProof() noexcept = default;
        PreparedSingleCompletionCommitProof(const PreparedSingleCompletionCommitProof&) = delete;
        PreparedSingleCompletionCommitProof& operator=(const PreparedSingleCompletionCommitProof&) = delete;
        PreparedSingleCompletionCommitProof(PreparedSingleCompletionCommitProof&&) noexcept = default;
        PreparedSingleCompletionCommitProof& operator=(PreparedSingleCompletionCommitProof&&) noexcept = default;
        bool Valid() const noexcept { return native_.Valid(); }
        void Release() noexcept { native_.Release(); original_ = {}; }
        const PreparedCommitInput& OriginalInput() const noexcept { return original_; }
    private:
        friend class VoiceLedger;
        PreparedCommitProof native_;
        PreparedCommitInput original_{};
    };
    // Updates the preparation input only after this actual native bind succeeds.
    bool BindPreparedSingleCompletionMutation(PreparedCommitInput* original,
        const CompletionMutationBinding& binding) noexcept;
    bool ProvePreparedSingleCompletionCommit(const PreparedCommitInput& original,
        PreparedSingleCompletionCommitProof* out) noexcept;
    bool MatchesPreparedSingleCompletionCommit(
        const PreparedSingleCompletionCommitProof& proof) const noexcept;
    static void ConsumePreparedSingleCompletionCommit(PreparedSingleCompletionCommitProof&& proof) noexcept;

    // Logic-producer only. This retains this Ledger's immutable native episode,
    // not a physical Voice, Occurrence, Decision, Current or mutation right.
    class OriginalCompletionMutationOwner {
    public:
        OriginalCompletionMutationOwner() noexcept = default;
        OriginalCompletionMutationOwner(const OriginalCompletionMutationOwner&) = delete;
        OriginalCompletionMutationOwner& operator=(const OriginalCompletionMutationOwner&) = delete;
        OriginalCompletionMutationOwner(OriginalCompletionMutationOwner&& other) noexcept {
            *this = std::move(other);
        }
        OriginalCompletionMutationOwner& operator=(OriginalCompletionMutationOwner&& other) noexcept {
            if (this != &other) {
                Release();
                table_ = std::exchange(other.table_, nullptr);
                entry_ = std::exchange(other.entry_, nullptr);
                slot_ = std::exchange(other.slot_, UINT32_MAX);
                generation_ = std::exchange(other.generation_, 0);
                marker_ = std::exchange(other.marker_, 0);
            }
            return *this;
        }
        ~OriginalCompletionMutationOwner() { Release(); }
        bool Valid() const noexcept;
        bool Borrow(OriginalCompletionMutationOwner* out) const noexcept;
        void Release() noexcept;
        PreparedPlayableIdentity OriginalIdentity() const noexcept {
            return Valid() ? PreparedPlayableIdentity{slot_, generation_} : PreparedPlayableIdentity{};
        }
    private:
        friend class VoiceLedger;
        VoiceLedger* table_ = nullptr;
        Entry* entry_ = nullptr;
        std::uint32_t slot_ = UINT32_MAX;
        std::uint32_t generation_ = 0;
        std::uint64_t marker_ = 0;
    };

    // A distinct original Composite Ledger episode, issued only by this same
    // actual bound held preparation. It grants no Single/Source/Current rights.
    class OriginalCompositeCompletionMutationOwner {
    public:
        OriginalCompositeCompletionMutationOwner() noexcept = default;
        OriginalCompositeCompletionMutationOwner(const OriginalCompositeCompletionMutationOwner&) = delete;
        OriginalCompositeCompletionMutationOwner& operator=(const OriginalCompositeCompletionMutationOwner&) = delete;
        OriginalCompositeCompletionMutationOwner(OriginalCompositeCompletionMutationOwner&&) noexcept = default;
        OriginalCompositeCompletionMutationOwner& operator=(OriginalCompositeCompletionMutationOwner&&) noexcept = default;
        bool Valid() const noexcept { return original_.Valid(); }
        bool Borrow(OriginalCompositeCompletionMutationOwner* out) const noexcept {
            return out != nullptr && original_.Borrow(&out->original_);
        }
        void Release() noexcept { original_.Release(); }
        PreparedPlayableIdentity OriginalIdentity() const noexcept { return original_.OriginalIdentity(); }
    private:
        friend class VoiceLedger;
        OriginalCompletionMutationOwner original_;
    };
    bool IssueOriginalCompositeCompletionMutationOwner(const PreparedCommitProof& proof,
        OriginalCompositeCompletionMutationOwner* out) noexcept;
    bool IsOriginalCompositeCompletionMutationOwnerFor(const OriginalCompositeCompletionMutationOwner& original,
        PreparedPlayableIdentity identity, const PlayableParentRef& parent,
        VnVoiceHandle render, const CompletionMutationBinding& mutation) const noexcept;
    bool IsCurrentOriginalCompositeCompletionMutationOwner(
        const OriginalCompositeCompletionMutationOwner& original) const noexcept;
    bool HasOriginalCompositeCompletionNativeEnd(
        const OriginalCompositeCompletionMutationOwner& original) const noexcept;

    bool IssueOriginalCompletionMutationOwner(const PreparedSingleCompletionCommitProof& proof,
        OriginalCompletionMutationOwner* out) noexcept;
    bool IssueOriginalCompletionMutationOwner(const PreparedCommitProof& proof,
        OriginalCompletionMutationOwner* out) noexcept;
    bool IsOriginalCompletionMutationOwnerFor(const OriginalCompletionMutationOwner& original,
        PreparedPlayableIdentity identity, const PlayableParentRef& parent,
        VnVoiceHandle render, const CompletionMutationBinding& mutation) const noexcept;
    // An observation of actual association only; audio may claim afterwards.
    bool IsCurrentOriginalCompletionMutationOwner(
        const OriginalCompletionMutationOwner& original) const noexcept;

    struct RenderOwner {
        bool live = false;
        PublicVisibility visibility = PublicVisibility::DirectVoice;
        std::uint32_t ledgerSlot = 0;
        PlayableParentRef parent{};
    };

    // Capacity is the resolved Logical Voice capacity L = R + V. Live,
    // PendingStart, and Parked all consume one entry; ended-but-unreaped Live
    // entries temporarily retain theirs. Allocated once here.
    explicit VoiceLedger(std::uint32_t capacity,
                         std::uint32_t renderCapacity = 0,
                         std::size_t monitorSelectionCapacity = 0);
    ~VoiceLedger();
    VoiceLedger(const VoiceLedger&) = delete;
    VoiceLedger& operator=(const VoiceLedger&) = delete;
    VoiceLedger(VoiceLedger&&) = delete;
    VoiceLedger& operator=(VoiceLedger&&) = delete;

    // Reserves a free entry and forms its public handle. Returns false when
    // every entry is occupied by a live voice — a defined degradation, not a
    // crash. A mint can exhaust Logical capacity when pending, parked, or
    // ended-but-unreaped identities retain entries. On success the entry is
    // Live but not yet bound to a Render Voice; call BindRender once the Render
    // Voice slot is claimed, or Release if the start then fails so a refused
    // raw voice start stays side-effect-free.
    bool Acquire(VnVoiceHandle* outPublicHandle, std::uint32_t* outLedgerSlot,
                 const MonitorRequestContext& request = {},
                     const MonitorEmitterOrigin& emitterOrigin = {}, VnObjectId media = {});
    void ObserveVoice(MonitorLifecyclePhase phase, std::uint32_t ledgerSlot,
                      std::uint32_t reason = 0,
                      const MonitorLifecycleFacts& facts = {}) noexcept;
    bool AcquirePlayableOwned(const PlayableParentRef& parent,
                              std::uint32_t* outLedgerSlot,
                              const MonitorRequestContext& request = {},
                     const MonitorEmitterOrigin& emitterOrigin = {});
    bool PreparePlayableOwned(const PlayableParentRef& parent,
                              std::uint32_t* outLedgerSlot,
                              PreparedPlayableIdentity* outIdentity = nullptr,
                              const MonitorRequestContext& request = {},
                     const MonitorEmitterOrigin& emitterOrigin = {});
    bool BindPreparedCompositeCompletionMutation(PreparedCommitInput* original,
        const CompletionMutationBinding& binding) noexcept;
    bool MatchesPreparedPlayableCommit(const PreparedCommitProof& proof,
        const PreparedCommitInput& original) const noexcept;
    bool ProvePreparedPlayableCommit(const PreparedCommitInput& expected,
                                     PreparedCommitProof* out) noexcept;
    static void ConsumePreparedPlayableCommit(PreparedCommitProof&& proof) noexcept;
    bool CommitPreparedPlayableOwned(std::uint32_t ledgerSlot);
    bool CancelPreparedPlayableOwned(std::uint32_t ledgerSlot);
    bool HasFreeEntry() const noexcept;
    std::uint32_t AvailableEntryCount() const noexcept;

    // Binds the internal Render Voice handle to a reserved entry and records
    // the voice's steal priority. Assigns the entry a monotonic start order
    // (used by the oldest-first steal policy — a later bind is a younger
    // voice).
    void BindRender(std::uint32_t ledgerSlot, VnVoiceHandle renderHandle,
                    std::uint32_t priority);

    // How a public handle currently resolves.
    enum class HandleKind : std::uint8_t { Stale, Live, Pending, Parked };
    struct Resolved {
        HandleKind kind = HandleKind::Stale;
        VnVoiceHandle renderHandle = 0; // valid only when kind == Live
        std::uint32_t ledgerSlot = 0; // valid when Live, Pending, or Parked
    };
    // Resolves a public handle to its current kind plus the render handle
    // (Live) or ledger slot (Live/Pending/Parked). A stale handle yields Stale
    // with zeroed fields.
    Resolved Classify(VnVoiceHandle publicHandle) const;
    RenderOwner OwnerForRender(VnVoiceHandle renderHandle) const;

    struct MonitorContent {
        MonitorContentIdentity content{};
        MonitorOccurrenceIdentity occurrence{};
        std::uint64_t root = 0;
        std::uint64_t action = 0;
        std::uint64_t leaf = 0;
        std::uint64_t resourceOwner = 0;
        std::uint64_t ownerEpoch = 0;
        std::uint64_t logicCommit = 0;
        std::uint8_t kind = 0;
        bool incomplete = false;
    };
    void RecordMonitorContent(std::uint32_t ledgerSlot, const MonitorContent& content) noexcept;

    struct MonitorIdentity {
        std::uint32_t ledgerSlot = UINT32_MAX;
        std::uint32_t generation = 0;
        VnVoiceHandle logicalHandle = 0;
        VnVoiceHandle renderHandle = 0;
        PlayableParentRef parent{};
        MonitorContent content{};
        MonitorRequestContext playbackRequest{};
        MonitorEmitterOrigin emitterOrigin{};
        MonitorControlCause controlCause{};
        MonitorControlCause selectionCause{};
        bool controlObservationIncomplete = false;
        bool prepared = false;
    };
    bool CaptureMonitorIdentity(std::uint32_t ledgerSlot, MonitorIdentity* out) const noexcept;
    // State snapshots contain logic-owned values, never newly arriving audio receipts.
    VnVoiceHandle CaptureMonitorIdentityAt(std::uint32_t ledgerSlot,
        MonitorLifecycleFacts* out) const noexcept;
    // Logic-thread diagnostic aliases retain frozen values, never playback resources.
    // The shared storage can outlive the Ledger during Engine member destruction.
    class MonitorSelection {
    public:
        MonitorSelection() noexcept = default;
        ~MonitorSelection();
        MonitorSelection(const MonitorSelection& other) noexcept;
        MonitorSelection& operator=(const MonitorSelection& other) noexcept;
        MonitorSelection(MonitorSelection&& other) noexcept;
        MonitorSelection& operator=(MonitorSelection&& other) noexcept;
        VnVoiceHandle RenderActivation() const noexcept;
        MonitorControlCause Cause() const noexcept;
        void SetCause(const MonitorControlCause& cause) noexcept;
        bool CaptureFacts(MonitorLifecycleFacts* out, std::uint32_t* generation = nullptr) const noexcept;
    private:
        friend class VoiceLedger;
        void Reset() noexcept;
        std::shared_ptr<MonitorSelectionStorage> storage_;
        std::uint32_t slot_ = UINT32_MAX;
        std::uint32_t generation_ = 0;
    };
    bool CaptureMonitorSelection(std::uint32_t ledgerSlot, VnVoiceHandle render,
        MonitorSelection* out) const noexcept;
    void PublishMonitorSelection(MonitorSelection& selection,
        const MonitorControlCause* cause = nullptr) noexcept;
    void RecordMonitorSelection(VnVoiceHandle render, const MonitorControlCause& cause) noexcept;
    std::size_t MonitorSelectionCapacity() const noexcept;
    std::size_t MonitorSelectionStorageBytes() const noexcept;
    void RecordMonitorCancellation(VnVoiceHandle render, const MonitorControlCause& cause) noexcept;
    MonitorVoiceControlBinding ArmMonitorControl(std::uint32_t ledgerSlot,
        VnVoiceHandle render) noexcept;
    std::uint64_t MonitorControlUnavailableCount() const noexcept { return m_monitorControlUnavailable; }

    bool BindCompletionMutation(
        std::uint32_t ledgerSlot,
        const CompletionMutationBinding& binding) noexcept;
    bool CaptureCompletionMutation(
        VnVoiceHandle publicHandle,
        CompletionMutationBinding* out) const noexcept;
    bool CaptureCompletionMutationForRender(
        VnVoiceHandle renderHandle,
        CompletionMutationBinding* out) const noexcept;
    bool CaptureCompletionMutationForSlot(
        std::uint32_t ledgerSlot,
        CompletionMutationBinding* out) const noexcept;
    bool ReplaceCompletionMutationExact(
        std::uint32_t ledgerSlot,
        const CompletionMutationBinding& expected,
        const CompletionMutationBinding& replacement) noexcept;
    bool ClearCompletionMutationExact(
        std::uint32_t ledgerSlot,
        const CompletionMutationBinding& binding) noexcept;

    // Reserves a free entry for a Logical Voice awaiting a Render Voice.
    // PendingStart consumes Logical capacity but is not a Virtual Voice. It
    // owns no Render Voice until StartPending binds one. Returns false when
    // every entry is occupied. priority drives later steal decisions.
    bool AcquirePending(VnVoiceHandle* outPublicHandle,
                        std::uint32_t* outLedgerSlot, std::uint32_t priority,
                        const MonitorRequestContext& request = {},
                     const MonitorEmitterOrigin& emitterOrigin = {}, VnObjectId media = {});

    // Transitions a PendingStart entry to Live on the given render slot,
    // assigning its start order. No-op if the slot is not PendingStart.
    void StartPending(std::uint32_t ledgerSlot, VnVoiceHandle renderHandle);

    // Transitions a PendingStart entry directly to Parked without ever claiming
    // a render slot: a deferred start that arrives inaudible parks without
    // rendering. The entry keeps its generation and public handle (the caller's
    // handle stays valid) and holds no render binding, exactly like a Park() of
    // a live voice; the caller builds the park/replay record at this slot. No-op
    // if the slot is not PendingStart.
    void ParkPending(std::uint32_t ledgerSlot);

    // Ends a PendingStart entry without ever having claimed a render slot
    // (a cancelled deferred start): the entry advances to Ended so its public
    // handle is now stale and the slot is reusable.
    void CancelPending(std::uint32_t ledgerSlot, const MonitorControlCause& cause = {});

    // Marks the Live entry on this render slot as a steal victim, so its
    // terminal notification is translated to VN_NOTIFY_VOICE_STOLEN. No-op if
    // no Live entry owns the render handle.
    void MarkStolen(VnVoiceHandle renderHandle, const MonitorControlCause& cause = {});

    // Whether the Live entry on this render handle was marked a steal victim.
    bool IsStolen(VnVoiceHandle renderHandle) const;

    // Marks a Live Render Voice as already committed to an ordinary stop.
    // Terminating voices remain Live until the audio side releases them, but
    // are protected from steal selection so their terminal reason cannot be
    // relabeled STOLEN. Logic thread only.
    void MarkTerminating(VnVoiceHandle renderHandle, const MonitorControlCause& cause = {},
                         bool recordControlCause = true);
    bool IsTerminating(VnVoiceHandle renderHandle) const;

    // Current number of PendingStart entries (a live gauge, like the parked
    // count).
    std::uint32_t PendingStartCount() const;

    // Parks the Live entry on this Render Voice: it becomes a Virtual Voice,
    // releases its render binding (renderHandle -> 0), and keeps its generation
    // and public handle so the caller's handle stays valid across the park. Writes
    // the ledger slot (where the caller keeps the park/replay record) and
    // returns true; false when no Live entry owns the render handle. The
    // internal stop the caller issues on the freed Render Voice slot then ends
    // that Render Voice; its terminal notification finds no Live owner and is
    // consumed (the game sees only VN_NOTIFY_VOICE_PARKED, emitted by the
    // caller).
    bool Park(VnVoiceHandle renderHandle, std::uint32_t* outLedgerSlot);

    // Returns a Parked entry to Live on a freshly claimed Render Voice, keeping
    // its generation/public handle and assigning a new start order. No-op if
    // the slot is not Parked.
    void ReturnPark(std::uint32_t ledgerSlot, VnVoiceHandle renderHandle);

    // Ends a Virtual Voice (a stop reaching a Parked entry, or a virtual
    // end/one-shot completion): it advances to Ended and bumps its generation,
    // so the public handle is now stale and the slot reusable. No render
    // involvement — a parked voice holds no render slot.
    void EndParked(std::uint32_t ledgerSlot, const MonitorControlCause& cause = {});

    // Current number of Parked entries (a live gauge).
    std::uint32_t ParkedCount() const;

    // The public handle of the entry at a ledger slot, or 0 for an out-of-range
    // slot. For callers that already hold a ledger slot (a park record) and need
    // the public handle to carry in a notification.
    VnVoiceHandle PublicHandleForSlot(std::uint32_t ledgerSlot) const;

    // Visits every Parked entry with its ledger slot and public handle — the
    // per-tick park/return/virtual-time scan.
    template <typename Fn>
    void ForEachParked(Fn&& fn) const {
        for (std::uint32_t i = 0; i < m_entries.size(); ++i) {
            const Entry& entry = m_entries[i];
            if (entry.state == EntryState::Parked) {
                fn(i, entry.publicHandle);
            }
        }
    }

    // Visits every Live entry with its render handle, steal priority, start
    // order, and whether it is already a steal victim — the steal-candidate
    // scan. A voice already marked stolen is committed to a prior steal (its
    // slot is promised), so the caller skips it. Logic thread only.
    template <typename Fn>
    void ForEachLive(Fn&& fn) const {
        for (const Entry& entry : m_entries) {
            if (entry.state == EntryState::Live && entry.renderHandle != 0 &&
                entry.visibility == PublicVisibility::DirectVoice) {
                fn(entry.renderHandle, entry.priority, entry.startOrder,
                   entry.stolen);
            }
        }
    }

    // Visits every PendingStart entry with its ledger slot and public handle.
    template <typename Fn>
    void ForEachPending(Fn&& fn) const {
        for (std::uint32_t i = 0; i < m_entries.size(); ++i) {
            const Entry& entry = m_entries[i];
            if (entry.state == EntryState::PendingStart) {
                fn(i, entry.publicHandle);
            }
        }
    }

    template <typename Fn>
    void ForEachPlayableOwnedLive(Fn&& fn) const {
        for (const Entry& entry : m_entries) {
            if (entry.state == EntryState::Live && entry.renderHandle != 0 &&
                entry.visibility ==
                    PublicVisibility::PlayableOwnedInternal) {
                fn(entry.renderHandle, entry.parent);
            }
        }
    }

    // Returns a reserved entry to Free without delivering a notification
    // (used when a play fails after acquire): the generation advances, so the
    // discarded public handle is already stale.
    void Release(std::uint32_t ledgerSlot);

    // Resolves a public handle to the render-slot handle of its Live entry,
    // or 0 when the public handle is stale (unknown ledger slot, generation
    // mismatch, or a non-Live entry). The returned render handle may itself
    // be stale if the voice ended but its end notification has not drained
    // yet — the caller re-checks render liveness, so the observable result is
    // a benign VN_STALE_HANDLE either way. No counting here: the render-slot
    // liveness check the caller runs owns the stale-handle counter, exactly
    // as it did before the ledger existed.
    VnVoiceHandle ToRender(VnVoiceHandle publicHandle) const;

    // Finds the public handle of the Live entry whose render-slot handle
    // matches exactly (slot and generation), or 0 when none owns it. Used to
    // translate audio-thread notifications into public identity. The exact
    // match disambiguates a render slot already reused by a newer voice from
    // the older entry still awaiting its end notification.
    VnVoiceHandle ToPublic(VnVoiceHandle renderHandle) const;

    // Ends the Live entry that owns this render-slot handle: it becomes Ended
    // and its generation advances, so the public handle is now stale and the
    // slot is reusable. Called after an end notification is translated and
    // delivered.
    void Reap(VnVoiceHandle renderHandle);

    // Reclaims Live entries whose bound render slot is no longer a live voice.
    // Identity reclamation must not depend on notification delivery:
    // notifications are advisory and may be dropped by the reverse queue's
    // drop-oldest policy, but the render slot still frees, so its ledger entry
    // would otherwise stay Live forever and eventually exhaust the ledger.
    // Runs on the logic thread after the notification drain, so an entry
    // whose end notification WAS delivered is already Ended and skipped here;
    // only orphaned Live entries are swept. The advisory notification is
    // still not synthesized, but DirectVoice State follows the ledger reclaim
    // with VoiceEnd so the current view does not keep a ghost upsert.
    // O(capacity), no allocation.
    // isRenderLive answers whether a render handle still names a live voice.
    template <typename LivePred>
    void SweepEnded(LivePred&& isRenderLive) {
        for (std::uint32_t i = 0; i < m_entries.size(); ++i) {
            Entry& entry = m_entries[i];
            if (entry.state != EntryState::Live || entry.renderHandle == 0) {
                continue; // Free/Ended, or a reserved entry mid-play
            }
            if (isRenderLive(entry.renderHandle)) {
                continue; // still rendering
            }
            ClearReverse(i, entry.renderHandle);
            const bool stolen = entry.stolen;
            const std::uint64_t endedHandle = entry.publicHandle;
            const std::uint64_t endedGeneration = entry.generation;
            const bool publish = endedHandle != 0;
            MonitorLifecycleFacts facts{};
            CaptureMonitorFacts(entry, facts, true);
            entry.renderHandle = 0;
            entry.stolen = false;
            entry.terminating = false;
            entry.parent = {};
            entry.completionMutation = {};
            AdvanceGeneration(entry, EntryState::Ended);
            if (publish) {
                ObserveMonitorLifecycle(
                    MonitorLifecyclePhase::VoiceEnd, endedHandle, endedGeneration,
                    0, {}, 0, 0,
                    stolen ? VN_ACTION_TERMINAL_CAPACITY
                           : VN_ACTION_TERMINAL_NATURAL,
                    0, 0, facts);
            }
        }
    }

    // Fills a correlation array indexed by render slot: out[renderSlot] is the
    // public handle whose Live entry currently occupies that render slot
    // (verified live through isRenderLive), or 0. isRenderLive answers whether
    // a render handle still names a live render slot; taking it as a predicate
    // keeps the ledger free of a dependency on the render module.
    template <typename LivePred>
    void FillPublicHandles(std::uint64_t* out, std::uint32_t count,
                           LivePred&& isRenderLive) const {
        for (std::uint32_t i = 0; i < count; ++i) {
            out[i] = 0;
        }
        for (const Entry& entry : m_entries) {
            if (entry.state != EntryState::Live || entry.renderHandle == 0 ||
                entry.visibility != PublicVisibility::DirectVoice) {
                continue;
            }
            // An ended-but-unreaped entry no longer owns its render slot (a
            // newer voice may already hold it), so the live predicate selects
            // the true current owner and its render generation disambiguates.
            if (!isRenderLive(entry.renderHandle)) {
                continue;
            }
            const std::uint32_t renderSlot =
                static_cast<std::uint32_t>(entry.renderHandle);
            if (renderSlot < count) {
                out[renderSlot] = entry.publicHandle;
            }
        }
    }

    std::uint32_t Capacity() const {
        return static_cast<std::uint32_t>(m_entries.size());
    }
    std::uint32_t QuarantinedCount() const { return m_quarantinedCount; }
    bool SetGenerationForTests(std::uint32_t slot,
                               std::uint32_t generation) noexcept;

private:
#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
    friend struct detail::VoiceLedgerOriginalCompletionTestAccess;
    friend struct detail::CompositeLedgerIssuerLimitTestAccess;
    friend struct detail::PreparedSingleBundleBorrowLimitTestAccess;
#endif
    // Free -> Live on acquire; Live -> Ended on reap; Live -> Parked on park and
    // Parked -> Live on return; PendingStart -> Live on start; any of Live /
    // PendingStart / Parked -> Ended when the voice finishes or is stopped.
    // Ended and Free are both reusable by acquire.
    enum class EntryState : std::uint8_t {
        Free = 0,
        Live = 1,
        Ended = 2,
        PendingStart = 3, // a Live-without-render staging record (steal defer)
        Parked = 4,       // a slot-less parked record (audibility parking)
        Quarantined = 5,
        PreparedCommitHeld = 6,
    };

    struct OriginalCompletionEpisode {
        std::uint32_t generation = 0;
        std::uint32_t externalCount = 0;
        std::uint64_t marker = 0;
        PlayableParentRef parent{};
        VnVoiceHandle render = 0;
        CompletionMutationBinding mutation{};
        bool nativeAssociation = false;
        bool committed = false;
        bool fullEnd = false;
    };

    struct Entry {
        MonitorRequestContext playbackRequest{};
        MonitorEmitterOrigin emitterOrigin{};
        MonitorControlCause controlCause{};
        MonitorControlCause selectionCause{};
        MonitorContent monitorContent{};
        bool monitorSelectionAccepted = false;
        bool monitorCancellationAccepted = false;
        MonitorVoiceControlBinding monitorControl{};
        bool monitorControlAttempted = false;
        OriginalCompletionEpisode originalCompletion{};
        std::uint32_t generation = 1;
        EntryState state = EntryState::Free;
        bool stolen = false; // Live victim: terminal notification -> STOLEN
        bool terminating = false; // ordinary stop is already in flight
        bool preparedReservation = false;
        std::uint32_t priority = 128;   // steal priority (higher wins)
        std::uint64_t startOrder = 0;   // monotonic start order (oldest steal)
        VnVoiceHandle renderHandle = 0; // internal render-slot handle when Live
        VnVoiceHandle publicHandle = 0; // cached ledger slot + generation
        PublicVisibility visibility = PublicVisibility::DirectVoice;
        PlayableParentRef parent{};
        CompletionMutationBinding completionMutation{};
        std::uint64_t monitorObserveCount = 0;
    };

    struct ReverseRenderEntry {
        std::uint32_t ledgerSlot = UINT32_MAX;
        std::uint32_t renderGeneration = 0;
        std::uint32_t heldLedgerSlot = UINT32_MAX;
    };

    static VnVoiceHandle MakeHandle(std::uint32_t slot,
                                    std::uint32_t generation) {
        return (static_cast<std::uint64_t>(generation) << 32) | slot;
    }

    static bool ReusableEntry(const Entry& entry) noexcept {
        return (entry.state == EntryState::Free || entry.state == EntryState::Ended) &&
            entry.originalCompletion.marker == 0;
    }
    bool BindPreparedCompletionMutationInternal(PreparedCommitInput* original,
        const CompletionMutationBinding& binding, CompletionOwnerKind kind) noexcept;
    bool IssueOriginalCompletionMutationOwnerInternal(const PreparedCommitProof& proof,
        OriginalCompletionMutationOwner* out, CompletionOwnerKind kind) noexcept;
    bool MatchesOriginalCompletionMutationOwner(
        const OriginalCompletionMutationOwner& original) const noexcept;
    bool BorrowOriginalCompletionMutationOwner(const OriginalCompletionMutationOwner& original,
        OriginalCompletionMutationOwner* out) noexcept;
    void ReleaseOriginalCompletionMutationOwner(OriginalCompletionMutationOwner& original) noexcept;
    static void ReclaimOriginalCompletionEpisode(Entry& entry) noexcept;
    static void EndOriginalCompletionEpisode(Entry& entry) noexcept;
    static void RevokeOriginalCompletionAssociation(Entry& entry) noexcept;
    static void ConsumeOriginalCompletionAssociation(Entry& entry,
        VnVoiceHandle render) noexcept;

    const Entry* EntryFor(VnVoiceHandle publicHandle) const;
    const Entry* EntryForRender(VnVoiceHandle renderHandle) const noexcept;
    Entry* EntryForRender(VnVoiceHandle renderHandle) noexcept;
    bool CanBindRender(VnVoiceHandle renderHandle) const noexcept;
    void BindReverse(std::uint32_t ledgerSlot,
                     VnVoiceHandle renderHandle) noexcept;
    void ClearReverse(std::uint32_t ledgerSlot,
                      VnVoiceHandle renderHandle) noexcept;
    void AdvanceGeneration(Entry& entry, EntryState reusableState) noexcept;
    void CaptureMonitorFacts(const Entry& entry, MonitorLifecycleFacts& facts,
        bool terminal = false, bool readAudioReceipt = true) const noexcept;
    static void InvalidateMonitorControl(Entry& entry) noexcept;

    std::vector<Entry> m_entries;
    std::vector<MonitorVoiceControlReceipt> m_monitorControlReceipts;
    std::uint64_t m_monitorControlTicket = 0;
    std::uint64_t m_monitorControlUnavailable = 0;
    std::vector<ReverseRenderEntry> m_renderOwners;
    std::shared_ptr<MonitorSelectionStorage> m_monitorSelections;
    std::uint64_t m_lastOriginalCompletionMarker = 0;
    std::uint64_t m_nextStartOrder = 1; // monotonic; 0 means "never started"
    std::uint32_t m_quarantinedCount = 0;
};

} // namespace harken

#endif // HARKEN_CORE_VOICE_LEDGER_H
