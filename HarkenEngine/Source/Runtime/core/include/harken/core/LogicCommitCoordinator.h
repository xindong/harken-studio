// The logic commit coordinator: the one place a commit order comes from, and
// the bundle that lets several services publish under a single order stamp
// without a planner ever seeing half of it.
//
// There is exactly one sequence. Variables, Actions, selection resets and
// anything else that mutates logic state take their stamp from here rather than
// keeping a counter each, because per-service counters cannot be compared: two
// changes stamped 4 by different services say nothing about which happened
// first, and a replay that cannot order them cannot reproduce the session.
//
// A bundle exists because a single authored step often touches several services
// at once, and a planner that observed one of them already changed and another
// not yet would make a decision against a state that never legally existed.
// Reservation is therefore all-or-nothing: every participant is asked for room
// first, and if any refuses, the ones that already agreed are released in
// reverse order and nothing is published. Only a fully reserved bundle commits,
// and it commits every participant under the same sequence.
//
// Reserving and committing are separate steps on purpose. Reservation is the
// part that can fail; commit is the part that must not. Once room is held, the
// publish has no failure path left to unwind, which is what makes the atomicity
// claim more than a hope.
//
// Logic thread only. The step list is fixed at construction; nothing here
// allocates while reserving or committing.
#ifndef HARKEN_CORE_LOGIC_COMMIT_COORDINATOR_H
#define HARKEN_CORE_LOGIC_COMMIT_COORDINATOR_H

#include <cstdint>
#include <cassert>
#include <utility>
#include <vector>

namespace harken {

// Monotonic logic commit order. 0 means "never stamped" and is never issued, so
// an unstamped record cannot sort ahead of a real one.
using LogicCommitSequence = std::uint64_t;

struct LogicCommitTicket {
    LogicCommitSequence sequence = 0;

    bool Valid() const noexcept { return sequence != 0; }
};

class LogicCommitCoordinator {
public:
    // An observation of one genuine reservation, never a right to consume it.
    class PreparedIssueIdentity {
    public:
        bool HasReservation() const noexcept { return owner_ != nullptr; }
        bool SameReservation(const PreparedIssueIdentity& other) const noexcept {
            return owner_ != nullptr && owner_ == other.owner_ && marker_ == other.marker_ &&
                sequence_ == other.sequence_;
        }
    private:
        friend class LogicCommitCoordinator;
        const LogicCommitCoordinator* owner_ = nullptr;
        std::uint64_t marker_ = 0;
        LogicCommitSequence sequence_ = 0;
    };

    class ConsumedIssue {
    public:
        ConsumedIssue() = default;
        ConsumedIssue(const ConsumedIssue&) = delete;
        ConsumedIssue& operator=(const ConsumedIssue&) = delete;
        ConsumedIssue(ConsumedIssue&& other) noexcept { *this = std::move(other); }
        ConsumedIssue& operator=(ConsumedIssue&& other) noexcept {
            identity_ = std::exchange(other.identity_, PreparedIssueIdentity{});
            sequence_ = std::exchange(other.sequence_, 0);
            return *this;
        }
        LogicCommitSequence Sequence() const noexcept { return sequence_; }
        bool MatchesOriginalReservation(const PreparedIssueIdentity& identity) const noexcept {
            return sequence_ != 0 && identity_.SameReservation(identity);
        }
    private:
        friend class LogicCommitCoordinator;
        PreparedIssueIdentity identity_{};
        LogicCommitSequence sequence_ = 0;
    };

    enum class IssueStatus : std::uint8_t {
        Issued,
        RuntimeRecreateRequired,
        Busy,
    };

    // The coordinator must outlive its genuine producer reservation.
    class PreparedIssue {
    public:
        PreparedIssue() = default;
        PreparedIssue(const PreparedIssue&) = delete;
        PreparedIssue& operator=(const PreparedIssue&) = delete;
        PreparedIssue(PreparedIssue&& other) noexcept { *this = std::move(other); }
        PreparedIssue& operator=(PreparedIssue&& other) noexcept {
            if (this != &other) {
                Release();
                owner_ = std::exchange(other.owner_, nullptr);
                marker_ = std::exchange(other.marker_, 0);
                sequence_ = std::exchange(other.sequence_, 0);
            }
            return *this;
        }
        ~PreparedIssue() { Release(); }
        bool Valid() const noexcept {
            return owner_ != nullptr && owner_->MatchesPreparedIssue(*this);
        }
        LogicCommitSequence Sequence() const noexcept {
            return Valid() ? sequence_ : 0;
        }
        void Release() noexcept {
            if (owner_ != nullptr) owner_->CancelPreparedIssue(*this);
            owner_ = nullptr;
            marker_ = 0;
            sequence_ = 0;
        }
    private:
        friend class LogicCommitCoordinator;
        friend class OriginalConditionalReplacementBatch;
        LogicCommitCoordinator* owner_ = nullptr;
        std::uint64_t marker_ = 0;
        LogicCommitSequence sequence_ = 0;
    };

    LogicCommitCoordinator() = default;
    LogicCommitCoordinator(const LogicCommitCoordinator&) = delete;
    LogicCommitCoordinator& operator=(const LogicCommitCoordinator&) = delete;
    LogicCommitCoordinator(LogicCommitCoordinator&&) = delete;
    LogicCommitCoordinator& operator=(LogicCommitCoordinator&&) = delete;
    ~LogicCommitCoordinator() { assert(m_liveReservation == 0); }

    // Reserves the actual next sequence without publishing Current. The token
    // excludes every other issuer until it is consumed or genuinely returned.
    IssueStatus PrepareIssue(PreparedIssue* out) noexcept {
        if (out == nullptr || out->owner_ != nullptr || m_liveReservation != 0)
            return IssueStatus::Busy;
        if (m_exhausted || m_current == UINT64_MAX || m_nextReservation == UINT64_MAX) {
            m_exhausted = true;
            return IssueStatus::RuntimeRecreateRequired;
        }
        m_liveReservation = ++m_nextReservation;
        out->owner_ = this;
        out->marker_ = m_liveReservation;
        out->sequence_ = m_current + 1;
        return IssueStatus::Issued;
    }

    // Checks this actual receiver's unique held issuer; no number or lookup
    // can manufacture a prepared issue or upgrade a foreign reservation.
    bool IsOriginalPreparedIssueFor(const PreparedIssue& proof) const noexcept {
        return MatchesPreparedIssue(proof);
    }

    bool CapturePreparedIssueIdentity(const PreparedIssue& proof,
        PreparedIssueIdentity* out) const noexcept {
        if (out == nullptr || !MatchesPreparedIssue(proof)) return false;
        out->owner_ = this;
        out->marker_ = proof.marker_;
        out->sequence_ = proof.sequence_;
        return true;
    }

    bool IsPreparedIssueActive(const PreparedIssueIdentity& identity) const noexcept {
        return identity.owner_ == this && identity.marker_ != 0 &&
            identity.marker_ == m_liveReservation && m_current != UINT64_MAX &&
            identity.sequence_ == m_current + 1u;
    }

    // Only the original issuer token can enter this no-fail ownership transfer.
    // No capacity, generation or sequence issuance remains after preparation.
    ConsumedIssue ConsumePreparedIssue(PreparedIssue&& proof) noexcept {
        assert(MatchesPreparedIssue(proof));
        ConsumedIssue consumed;
        if (!CapturePreparedIssueIdentity(proof, &consumed.identity_)) return consumed;
        consumed.sequence_ = proof.sequence_;
        m_current = proof.sequence_;
        m_liveReservation = 0;
        proof.owner_ = nullptr;
        proof.marker_ = 0;
        proof.sequence_ = 0;
        return consumed;
    }

    // The last sequence issued, or 0 before the first.
    LogicCommitSequence Current() const { return m_current; }

    IssueStatus TryIssue(LogicCommitSequence* out) noexcept {
        if (m_liveReservation != 0) return IssueStatus::Busy;
        if (out == nullptr || m_exhausted) {
            return IssueStatus::RuntimeRecreateRequired;
        }
        if (m_current == UINT64_MAX) {
            m_exhausted = true;
            return IssueStatus::RuntimeRecreateRequired;
        }
        ++m_current;
        *out = m_current;
        return IssueStatus::Issued;
    }

    // Issues the next sequence. Legacy construction helpers use this only after
    // their own bounded reservation has succeeded.
    LogicCommitSequence Issue() {
        LogicCommitSequence issued = 0;
        (void)TryIssue(&issued);
        return issued;
    }

    std::uint64_t IssuedCount() const { return m_current; }
    bool Exhausted() const noexcept { return m_exhausted; }
    void SetCurrentForTests(LogicCommitSequence current) noexcept {
        assert(m_liveReservation == 0);
        if (m_liveReservation != 0) return;
        m_current = current;
        m_exhausted = false;
    }

private:
    bool MatchesPreparedIssue(const PreparedIssue& proof) const noexcept {
        return proof.owner_ == this && proof.marker_ != 0 &&
            proof.marker_ == m_liveReservation && m_current != UINT64_MAX &&
            proof.sequence_ == m_current + 1;
    }
    void CancelPreparedIssue(const PreparedIssue& proof) noexcept {
        assert(MatchesPreparedIssue(proof));
        if (MatchesPreparedIssue(proof)) m_liveReservation = 0;
    }
    LogicCommitSequence m_current = 0;
    bool m_exhausted = false;
    std::uint64_t m_nextReservation = 0;
    std::uint64_t m_liveReservation = 0;
};

// One service participating in a bundle. Every callback must be present; an
// incomplete participant answers "no room" rather than silently leaking or
// skipping an owner operation.
//
// reserve/release must be a matched pair over the same count: the bundle relies
// on release undoing exactly what reserve took, and cannot verify that itself.
struct CommitParticipant {
    void* ctx = nullptr;
    // Holds room for count items. False when the service cannot.
    bool (*reserve)(void* ctx, std::uint32_t count) = nullptr;
    // Returns room a reservation took, used when a later participant refuses.
    void (*release)(void* ctx, std::uint32_t count) = nullptr;
    // Publishes into the room already held. Called only after every
    // participant reserved, so it has no failure path.
    void (*commit)(void* ctx, std::uint32_t count,
                   LogicCommitSequence sequence) = nullptr;
};

enum class BundleState : std::uint32_t {
    // Steps may still be added.
    Open = 0,
    // Every participant holds its room; commit cannot fail from here.
    Reserved = 1,
    // Published under one sequence.
    Committed = 2,
    // Reservation refused or the bundle was abandoned; nothing is held.
    Abandoned = 3
};

class ActionCommitBundle {
public:
    // maxSteps bounds one bundle's participant count. Allocated here so
    // reserving cannot fail for lack of memory, only for lack of room.
    explicit ActionCommitBundle(std::uint32_t maxSteps);

    BundleState State() const { return m_state; }
    std::uint32_t StepCount() const {
        return static_cast<std::uint32_t>(m_steps.size());
    }
    std::uint32_t MaxSteps() const { return m_maxSteps; }
    LogicCommitSequence Sequence() const { return m_sequence; }

    // Adds one participant and the room it needs. Refused once the bundle has
    // left Open, and refused past maxSteps, so a bundle cannot grow after its
    // room was already proven sufficient.
    bool AddStep(const CommitParticipant& participant, std::uint32_t count);

    // Asks every participant for room, in step order. On the first refusal,
    // releases what was already held in reverse order, moves to Abandoned and
    // returns false — so a refused bundle leaves every service exactly as it
    // was. On success the bundle is Reserved.
    bool Reserve();

    // Publishes every participant under one freshly issued sequence. Refused
    // unless the bundle is Reserved: committing without holding room is the
    // half-state this class exists to make impossible.
    bool Commit(LogicCommitCoordinator& coordinator);

    // Publishes every participant under an already reserved sequence. Deferred
    // Action materialization uses this so it never TryIssue a second stamp.
    bool CommitReserved(LogicCommitSequence sequence);

    bool Reset();

    // Gives back a held reservation without publishing. A no-op unless the
    // bundle is Reserved, so abandoning twice cannot double-release.
    void Abandon();

private:
    struct Step {
        CommitParticipant participant;
        std::uint32_t count = 0;
    };

    // Releases the first `count` steps in reverse order.
    void ReleasePrefix(std::uint32_t count);

    std::vector<Step> m_steps;
    std::uint32_t m_maxSteps = 0;
    BundleState m_state = BundleState::Open;
    LogicCommitSequence m_sequence = 0;
};

} // namespace harken

#endif // HARKEN_CORE_LOGIC_COMMIT_COORDINATOR_H
