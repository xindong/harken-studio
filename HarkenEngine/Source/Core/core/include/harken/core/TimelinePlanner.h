// The timeline planner: the one place scheduled transactions are published from.
//
// Action, game and control code only submit intent; the planner is the sole
// producer that turns an intent into a ScheduledTransaction filed into the
// horizon pool. Concentrating publication here is what lets the pool be written
// for a single writer with no lock.
//
// What the planner decides, and when. Every occurrence has a decision frame fixed
// at compile/plan time -- decisionFrame = entryAnchorFrame - decisionLead -- not
// the moment an Update happens to reach it. A play whose whole span fits the
// writable horizon is filed immediately; a play beyond the horizon is held as a
// deferred intent, occupying no page or playable slot, and is exchanged for a real
// transaction once the coordinator claims it. The coordinator owns the shared
// Timeline/Action materialization budget and routes Playable claims here.
//
// Lateness is the pool's contract, wired here. A request whose earliest frame is
// before the publish frontier takes the caller's PrePublishLatePolicy. Three of
// the four the pool decides itself (reschedule to the earliest safe frame, drop,
// refuse); the fourth, NextEquivalentBoundary, needs the tempo/meter map the pool
// does not have, so the planner resolves it through the marker index -- the next bar,
// beat or marker after the frontier -- and refiles there, or applies the caller's
// missing-boundary policy when the marker grid offers no candidate.
//
// Determinism. Given the same timestamped inputs, and provided the planner meets
// its own deadline, the schedule it produces does not depend on how many quanta
// pass between calls: decision frames are precomputed, materialization is a fixed
// per-quantum step, and randomness is keyed (see KeyedRng.h), so an Update cadence
// of one, two or four quanta yields the same filed schedule.
//
// Logic thread, single producer, no lock. Storage is sized once at Reset; the
// steady path allocates nothing. Not implemented here (documented seams): variable
// snapshot evaluation at the decision frame, occurrence-bound transactions, and
// the prepare/armed resource state machine. The compiled SegmentProgram that feeds
// PostPlay is produced by a later segment; here it is the narrow ProgramView.
#ifndef HARKEN_CORE_TIMELINE_PLANNER_H
#define HARKEN_CORE_TIMELINE_PLANNER_H

#include "harken/core/ClockKernel.h"
#include "harken/core/MarkerIndex.h"
#include "harken/core/DeferredIntentPool.h"
#include "harken/core/LogicCommitCoordinator.h"
#include "harken/core/PlayableInstanceTable.h"
#include "harken/core/ScheduledTransactionPool.h"
#include "harken/core/TimeCoordinates.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace harken {

// One event of a compiled program, positioned relative to the segment's entry. The
// planner rebases it to an absolute frame at file time (entryAnchorFrame + offset,
// clamped at zero for a weak-up that would precede the session start).
struct EventTemplate {
    std::int64_t frameOffsetFromEntry = 0;
    LogicCommitSequence logicCommitSequence = 0;
    std::uint32_t eventIndex = 0;
    std::uint32_t transactionOrdinal = 0;
    std::uint32_t subeventPhase = 0;
    std::uint64_t payload = 0;
    std::uint32_t executionOrdinal = 0;
    bool authoredFadeOutCandidate = false;
    TimelineArmToken armToken{};
    FrontierEventBinding frontier{};
};

// entryAnchor + offset as a nonnegative absolute frame. A weak-up offset can be
// negative; the result clamps at zero, because a frame before the session start
// cannot be scheduled (the pool then treats it by the window/late rules). Also
// saturates rather than wrapping on the (unreachable in practice) positive
// overflow.
//
// Shared rather than private to the planner because rebasing an EventTemplate is
// not only the planner's business: anything that consumes a compiled program has
// to land on the same absolute frame the planner files, and a second copy of
// this arithmetic would be a second chance to get the negative magnitude or the
// saturation wrong. Two details make it worth stating rather than inlining at
// each site: the magnitude of a negative offset is taken through the
// complement, so INT64_MIN does not overflow the way a unary minus would, and
// the positive sum is checked for wrap instead of assumed not to.
inline std::uint64_t AnchoredFrame(std::uint64_t entryAnchor,
                                   std::int64_t offset) {
    if (offset >= 0) {
        const std::uint64_t sum = entryAnchor + static_cast<std::uint64_t>(offset);
        return sum < entryAnchor ? UINT64_MAX : sum;
    }
    const std::uint64_t magnitude =
        (~static_cast<std::uint64_t>(offset)) + 1u;
    return entryAnchor >= magnitude ? entryAnchor - magnitude : 0;
}

// The narrow view of a compiled program the planner files from. In the shipping
// build the SegmentProgram compiler produces this; here a test builds it by hand.
// Non-owning: the events array, and the view itself when a play is deferred, must
// outlive the planner's use of them (the compiled program store owns them), just
// as a ClockKernel borrows its tempo segments.
struct ProgramView {
    PlayableInstanceHandle ownerInstance = 0;
    std::uint64_t ownerInstanceScheduleEpoch = 0;
    TransactionId transactionId = 0;
    std::uint64_t entryAnchorFrame = 0; // absolute frame the entry (tick 0) lands on
    std::int64_t decisionLeadFrames = 0; // decisionFrame = entryAnchorFrame - this
    const EventTemplate* events = nullptr;
    std::size_t eventCount = 0;
    // For NextEquivalentBoundary resolution only. clock and markerIndex are the
    // segment's own; boundaryType and boundaryFromPosition say which grid line to
    // align to and from where (in the segment's native unit). Null clock/markerIndex
    // means the segment offers no boundary grid.
    SegmentTimeType timeType = SegmentTimeType::Musical;
    const ClockKernel* clock = nullptr;
    const MarkerIndex* markerIndex = nullptr;
    BoundaryType boundaryType = BoundaryType::Bar;
    TimelineTransactionProgramRef runtimeProgram{};
    TransactionOccurrenceRef occurrence{};
    TimelineArmToken armToken{};
    bool rollingGroups = false;
    ImmediateEntryAttemptRef immediateEntry{};
};

enum class TimelineDeferredMaterializeStatus : std::uint8_t {
    Consumed = 0,
    Retry = 1,
    NotOwned = 2,
    RetainedOwned = 3,
};

// When the pool refuses NextEquivalentBoundary and the marker grid has no next
// boundary, the play takes one of these -- the planner does not invent a frame.
enum class MissingBoundaryPolicy : std::uint32_t {
    Drop = 0,
    Refuse = 1
};

// The timestamped input of one PostPlay. currentFrame is the earliest safely
// publishable frame ("now"), not a wall clock read. logicCommitSequence is the
// ordering key assigned by the logic commit coordinator for this submission.
struct PostInput {
    std::uint64_t currentFrame = 0;
    TransportEpoch transportEpoch = kInvalidTransportEpoch;
    LogicCommitSequence logicCommitSequence = 0;
    PrePublishLatePolicy latePolicy = PrePublishLatePolicy::ExecuteAtEarliestSafeFrame;
    MissingBoundaryPolicy missingBoundaryPolicy = MissingBoundaryPolicy::Refuse;
};

enum class PostStatus : std::uint32_t {
    Reserved = 0,     // filed into the horizon at its requested frames
    Rescheduled = 1,  // filed after a late shift or a boundary realignment
    Deferred = 2,     // beyond the horizon; held as a deferred intent
    Dropped = 3,      // late + Drop, or missing boundary + Drop
    Refused = 4       // refused; see refusal
};

struct PostResult {
    PostStatus status = PostStatus::Refused;
    ReserveRefusal refusal = ReserveRefusal::None; // when a pool Reserve refused
    TransactionId transactionId = 0;
    std::uint64_t firstFrame = 0; // filed span, when Reserved/Rescheduled
    std::uint64_t lastFrame = 0;
    CommittedTransactionRef committed{};
    // The planner was asked to decide at or after the decision frame; it must not
    // read state from later than the decision frame. No variable service exists
    // yet, so this only flags the condition for the caller.
    bool lateDecision = false;
};

struct PreparedDeferredPost {
    DeferredIntentToken intent{};
    std::uint32_t slot = UINT32_MAX;

    bool Valid() const { return intent.Valid() && slot != UINT32_MAX; }
};

class TimelinePlanner {
public:
    struct Config {
        std::uint32_t maxRollingOccurrences = 0;
    };

    TimelinePlanner() = default;

    // Binds the planner to its pool, deferred-intent pool and instance table (all
    // borrowed, must outlive it) and sizes its own fixed storage. The deferred
    // pool must be empty because its payloads index planner-owned side storage;
    // callers reusing a pool must drain it first.
    bool Reset(const Config& config, ScheduledTransactionPool* pool,
               DeferredIntentPool* deferredPool,
               const PlayableInstanceTable* instances,
               const OccurrenceStatePool* occurrences = nullptr,
               TimelineArmTable* armTable = nullptr);
    bool ResetForTransport() noexcept;

    bool Valid() const { return valid_; }

    // Files one play. If its whole span fits the writable horizon it is reserved
    // now (taking latePolicy if it is late); otherwise it is held as a deferred
    // intent and reserved later after coordinator routing. The program view must outlive
    // the planner when the result is Deferred.
    PostResult PostPlay(const ProgramView& program, const PostInput& input);
    PostResult PrepareDeferredPlay(const ProgramView& program,
                                   const PostInput& input,
                                   PreparedDeferredPost* out);
    bool CommitPreparedDeferredPlay(PreparedDeferredPost* prepared);
    bool AbandonPreparedDeferredPlay(PreparedDeferredPost* prepared);

    // Reclaims one page only after the audio consumer has crossed it.
    bool ReclaimConsumedQuantum();

    void DropStaleDeferred();
    std::uint32_t ClaimDeferredDue(
        std::uint64_t windowEndFrame, DeferredIntentClaim* out,
        std::uint32_t capacity, std::uint32_t* staleDropped = nullptr,
        std::uint32_t* dueCount = nullptr,
        std::uint64_t currentQuantum = UINT64_MAX);
    TimelineDeferredMaterializeStatus MaterializeDeferredClaim(
        const DeferredIntentClaim& claim, TransportEpoch transportEpoch,
        std::uint64_t cutoffFrame);
    std::uint64_t DeferredBlockerBefore(std::uint64_t cutoffFrame) const;

    // Freezes the safe continuous prefix after every obligation below it has been
    // closed. This is the only planner publication entry after Reset.
    void PublishThrough(std::uint64_t cutoffFrame);

    // Legacy publication retained for non-production planner clients. It reclaims
    // the consumed page and publishes the configured lead. A zero-lead client retains
    // the one-page bootstrap runway so
    // its consumer always has one ready quantum. Callers must advance the consumer
    // frontier only after consuming the stamped absolute quantum.
    void AdvanceQuantum(TransportEpoch transportEpoch);

    // Materializations re-deferred after a transient scheduled-pool refusal.
    // Each attempt is retried no earlier than the next quantum.
    std::uint64_t MaterializationsRetried() const {
        return materializationsRetried_;
    }
    // Materializations that reached filing but were terminally refused by the
    // pool or retained policy. Explicit drops and transient retries are not
    // failures.
    std::uint64_t MaterializationReserveFailures() const {
        return materializationReserveFailures_;
    }
    std::uint64_t TrimmedFadeOutGroups() const {
        return trimmedFadeOutGroups_;
    }
    bool HasLiveProgramGeneration(ProgramGeneration generation) const;
    bool HasProgramReference(TimelineTransactionProgramRef program) const noexcept;
    bool RollingAbortPending(TimelineArmToken armToken) const;
    bool AbortRollingOccurrence(TimelineArmToken armToken);
    bool CompleteRollingAbort(TimelineArmToken armToken);
    std::uint32_t LiveRollingCountForTests() const;
    bool SetRollingDeferredGenerationForTests(TimelineArmToken armToken,
                                              std::uint64_t generation);

private:
    enum class RollingGroup : std::uint8_t {
        Start = 0,
        FadeOut = 1,
        Stop = 2,
        Done = 3,
    };
    enum class RollingState : std::uint8_t {
        Free = 0,
        AwaitingWindow = 1,
        Publishing = 2,
        Aborted = 4,
    };
    struct RollingCursor {
        RollingState state = RollingState::Free;
        const ProgramView* program = nullptr;
        PostInput input{};
        ProgramGeneration programGeneration = 0;
        TransactionOccurrenceRef occurrence{};
        RollingGroup next = RollingGroup::Start;
        std::uint64_t startFrame = 0;
        std::uint64_t fadeOutStartFrame = 0;
        std::uint64_t fadeOutEndFrame = 0;
        std::uint64_t stopFrame = 0;
        std::uint32_t fadeOutEnvelopeIndex = 0;
        TimelineArmToken armToken{};
        DeferredIntentToken deferred{};
        bool authoredFadeOutPublished = false;
    };

    // Builds the transaction from a program at a (possibly shifted) entry anchor
    // and reserves it under a late policy. Returns the pool's result mapped to a
    // PostResult, resolving NextEquivalentBoundary through the marker index. A finite
    // publicationFrontier evaluates policy against a prefix being prepared.
    PostResult FileProgram(const ProgramView& program, std::uint64_t entryAnchor,
                           const PostInput& input, bool countAsReschedule,
                           bool trimBeforeFrontier = false,
                           std::uint64_t publicationFrontier = UINT64_MAX);
    // Resolves NextEquivalentBoundary through the marker grid and refiles on the
    // boundary, or applies the missing-boundary policy when there is no candidate.
    PostResult ResolveBoundary(const ProgramView& program, const PostInput& input,
                               std::uint64_t publicationFrontier);
    // Reserve once, without boundary resolution.
    ReserveResult ReserveAt(const ProgramView& program, std::uint64_t entryAnchor,
                            TransportEpoch epoch, PrePublishLatePolicy policy,
                            std::uint64_t* outFirst, std::uint64_t* outLast,
                            bool trimBeforeFrontier = false,
                            std::uint64_t publicationFrontier = UINT64_MAX);
    void ReleaseDeferredSlot(std::uint64_t payload);
    void ReleaseStaleDeferredClaims();
    std::uint64_t MaterializeDue(TransportEpoch transportEpoch,
                                std::uint64_t cutoffFrame,
                                std::uint64_t windowEndFrame);
    PostResult PostRolling(const ProgramView& program, const PostInput& input);
    PostResult FileRollingGroup(RollingCursor& cursor, RollingGroup group,
                                bool materialization,
                                bool* trimmedFadeOut = nullptr,
                                std::uint64_t publicationFrontier = UINT64_MAX);
    bool AdvanceRollingCursor(RollingCursor& cursor,
                              DeferredIntentToken claimToken,
                              PostStatus status);
    Config config_{};
    ScheduledTransactionPool* pool_ = nullptr;
    DeferredIntentPool* deferredPool_ = nullptr;
    const PlayableInstanceTable* instances_ = nullptr;
    const OccurrenceStatePool* occurrences_ = nullptr;
    TimelineArmTable* armTable_ = nullptr;
    bool valid_ = false;

    // Non-owning retention of the program behind each deferred intent, indexed by
    // the intent's payload. The compiled program store owns the views; the planner
    // only remembers which one a deferred slot refers to.
    std::vector<const ProgramView*> deferredProgram_;
    std::vector<PostInput> deferredInput_;
    std::vector<std::uint8_t> deferredSlotUsed_;
    std::uint32_t committedDeferredProgramCount_ = 0;
    std::vector<RollingCursor> rolling_;

    // Reserve-time scratch for a transaction's events, sized once at Reset.
    std::vector<ScheduledEventInput> scratchEvents_;
    EventTemplate rollingEventScratch_{};
    std::vector<DeferredIntentClaim> staleClaimScratch_;

    std::uint64_t materializationsRetried_ = 0;
    std::uint64_t materializationReserveFailures_ = 0;
    std::uint64_t trimmedFadeOutGroups_ = 0;
};

} // namespace harken

#endif // HARKEN_CORE_TIMELINE_PLANNER_H
