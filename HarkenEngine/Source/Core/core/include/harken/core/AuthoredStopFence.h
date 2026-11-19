#ifndef HARKEN_CORE_AUTHORED_STOP_FENCE_H
#define HARKEN_CORE_AUTHORED_STOP_FENCE_H

#include "harken/core/DecisionTable.h"
#include "harken/core/AuthoredStopOperationIdentity.h"
#include "harken/core/TimelineProgram.h"

#include <atomic>
#include <cassert>
#include <utility>

namespace harken {

enum class AuthoredStopCancelDisposition : std::uint8_t {
    Cancelled, AlreadyCancelled, DeferredApplying
};

// Storage must outlive every exact program reference and its final audio block.
class AuthoredStopOperation {
public:
    explicit AuthoredStopOperation(AuthoredStopOperationIdentity identity) noexcept
        : identity_(identity) {}

    const AuthoredStopOperationIdentity& Identity() const noexcept { return identity_; }
    bool Active(const AuthoredStopOperationIdentity& identity) const noexcept {
        return identity.Valid() && identity_ == identity &&
            (access_.load(std::memory_order_acquire) & kCancelled) == 0;
    }
    AuthoredStopCancelDisposition Cancel() noexcept {
        std::uint32_t expected = 0;
        if (access_.compare_exchange_strong(expected, kCancelled,
                std::memory_order_acq_rel, std::memory_order_acquire))
            return AuthoredStopCancelDisposition::Cancelled;
        return (expected & kCancelled) != 0 ? AuthoredStopCancelDisposition::AlreadyCancelled :
            AuthoredStopCancelDisposition::DeferredApplying;
    }
    std::uint64_t AppliedAudioSequence() const noexcept {
        return appliedAudioSequence_.load(std::memory_order_acquire);
    }
    void PublishApplied(const AuthoredStopOperationIdentity& identity,
                        std::uint64_t sequence) const noexcept {
        if (!Active(identity) || sequence == 0) return;
        std::uint64_t expected = 0;
        (void)appliedAudioSequence_.compare_exchange_strong(
            expected, sequence, std::memory_order_release, std::memory_order_relaxed);
    }

private:
    friend struct AuthoredStopFenceDescriptor;
    friend class TimelineArmTable;
    friend class AuthoredStopFenceLaneReference;
    bool TryAcquireApply(const AuthoredStopOperationIdentity& identity) const noexcept {
        if (!identity.Valid() || identity != identity_) return false;
        auto access = access_.load(std::memory_order_acquire);
        do {
            if ((access & kCancelled) != 0 || access == kCancelled - 1u) return false;
        } while (!access_.compare_exchange_weak(access, access + 1u,
            std::memory_order_acq_rel, std::memory_order_acquire));
        return true;
    }
    void ReleaseApply() const noexcept { access_.fetch_sub(1u, std::memory_order_release); }
    static constexpr std::uint32_t kCancelled = 1u << 31;
    const AuthoredStopOperationIdentity identity_;
    mutable std::atomic<std::uint32_t> access_{0};
    mutable std::atomic<std::uint64_t> appliedAudioSequence_{0};
};

enum class AuthoredStopFenceAccess : std::uint8_t { Armed, Applying, Disarmed, Done, Prepared };

// The original program storage outlives this future reader and all apply proofs.
class AuthoredStopFenceLaneReference {
public:
    AuthoredStopFenceLaneReference() noexcept = default;
    AuthoredStopFenceLaneReference(const AuthoredStopFenceLaneReference&) = delete;
    AuthoredStopFenceLaneReference& operator=(const AuthoredStopFenceLaneReference&) = delete;
    AuthoredStopFenceLaneReference(AuthoredStopFenceLaneReference&& other) noexcept
        : descriptor_(std::exchange(other.descriptor_, nullptr)),
          payloads_(std::exchange(other.payloads_, nullptr)),
          lastReadSequence_(std::exchange(other.lastReadSequence_, 0)) {}
    AuthoredStopFenceLaneReference& operator=(AuthoredStopFenceLaneReference&& other) noexcept {
        if (this != &other) {
            Reset(); descriptor_ = std::exchange(other.descriptor_, nullptr);
            payloads_ = std::exchange(other.payloads_, nullptr);
            lastReadSequence_ = std::exchange(other.lastReadSequence_, 0);
        }
        return *this;
    }
    ~AuthoredStopFenceLaneReference() { Reset(); }
    bool Valid() const noexcept { return descriptor_ != nullptr; }
    const AuthoredStopFenceDescriptor* Descriptor() const noexcept { return descriptor_; }
    bool Matches(const TimelineTransactionProgramRef& reference) const noexcept;
    void Reset() noexcept;
private:
    friend struct AuthoredStopFenceDescriptor;
    friend class ImmediateActionLane;
    const AuthoredStopFenceDescriptor* descriptor_ = nullptr;
    const TimelineEventPayload* payloads_ = nullptr;
    mutable std::uint64_t lastReadSequence_ = 0;
};

class AuthoredStopFenceLaneProof {
public:
    AuthoredStopFenceLaneProof() noexcept = default;
    AuthoredStopFenceLaneProof(const AuthoredStopFenceLaneProof&) = delete;
    AuthoredStopFenceLaneProof& operator=(const AuthoredStopFenceLaneProof&) = delete;
    AuthoredStopFenceLaneProof(AuthoredStopFenceLaneProof&& other) noexcept
        : descriptor_(std::exchange(other.descriptor_, nullptr)) {}
    AuthoredStopFenceLaneProof& operator=(AuthoredStopFenceLaneProof&& other) noexcept {
        if (this != &other) {
            Reset();
            descriptor_ = std::exchange(other.descriptor_, nullptr);
        }
        return *this;
    }
    ~AuthoredStopFenceLaneProof() { Reset(); }
    bool Valid() const noexcept { return descriptor_ != nullptr; }
    void Reset() noexcept;

private:
    friend struct AuthoredStopFenceDescriptor;
    const AuthoredStopFenceDescriptor* descriptor_ = nullptr;
};

struct AuthoredStopFenceDescriptor {
    // Logic owns the pin; disarm or observe Done before releasing it or destroying storage.
    AuthoredStopFenceDescriptor(const AuthoredStopOperation* op,
        AuthoredStopOperationIdentity id, DecisionToken target,
        const TimelineProgramView* view, ProgramGeneration generation,
        DecisionFuturePin pin = {}) noexcept
        : operation(op), identity(id), decision(target), program(view),
          programGeneration(generation), decisionTable_(pin.Owner()),
          decisionPinned_(pin.Valid() && pin.Token().slot == target.slot &&
              pin.Token().generation == target.generation), pin_(std::move(pin)) {}

    const AuthoredStopOperation* const operation;
    const AuthoredStopOperationIdentity identity;
    const DecisionToken decision;
    const TimelineProgramView* const program;
    const ProgramGeneration programGeneration;

    bool Active() const noexcept {
        return operation != nullptr && operation->Active(identity) &&
            access_.load(std::memory_order_acquire) == AuthoredStopFenceAccess::Armed &&
            ((decision.Valid() && decisionPinned_) ||
                (decision.slot == UINT32_MAX && decision.generation == 0));
    }

    bool UsesDecisionTable(const DecisionTable* table) const noexcept {
        return !decision.Valid() || (decisionPinned_ && decisionTable_ == table);
    }
    bool AcquireLaneReference(const TimelineTransactionProgramRef& reference,
                             AuthoredStopFenceLaneReference* out) const noexcept {
        if (out == nullptr || out->Valid() || !CanonicalLaneProgram(reference) ||
            !Active() || !operation->TryAcquireApply(identity)) return false;
        if (!Active()) { operation->ReleaseApply(); return false; }
        out->descriptor_ = this;
        out->payloads_ = program->payloads;
        return true;
    }
    bool ProveLaneApply(const TimelineTransactionProgramRef& reference,
                        AuthoredStopFenceLaneProof* out) const noexcept {
        if (out == nullptr || out->Valid() || !CanonicalLaneProgram(reference) ||
            !Active() || !operation->TryAcquireApply(identity)) return false;
        auto expected = AuthoredStopFenceAccess::Armed;
        if (!access_.compare_exchange_strong(expected, AuthoredStopFenceAccess::Prepared,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            operation->ReleaseApply();
            return false;
        }
        out->descriptor_ = this;
        return true;
    }
    // The actual lane claim must precede this no-fail ownership transfer.
    void ApplyLaneProof(AuthoredStopFenceLaneProof* proof) const noexcept {
        assert(proof != nullptr && proof->descriptor_ == this);
        assert(access_.load(std::memory_order_acquire) == AuthoredStopFenceAccess::Prepared);
        access_.store(AuthoredStopFenceAccess::Applying, std::memory_order_release);
        proof->descriptor_ = nullptr;
    }
    bool TryBeginApply() const noexcept {
        if (!Active() || !operation->TryAcquireApply(identity)) return false;
        auto expected = AuthoredStopFenceAccess::Armed;
        if (access_.compare_exchange_strong(expected, AuthoredStopFenceAccess::Applying,
                std::memory_order_acq_rel, std::memory_order_acquire)) return true;
        operation->ReleaseApply();
        return false;
    }
    void FinishApply() const noexcept {
        const auto* applyingOperation = operation;
        auto expected = AuthoredStopFenceAccess::Applying;
        if (access_.compare_exchange_strong(expected, AuthoredStopFenceAccess::Done,
                std::memory_order_release, std::memory_order_relaxed)) applyingOperation->ReleaseApply();
    }
    AuthoredStopFenceAccess Access() const noexcept {
        return access_.load(std::memory_order_acquire);
    }
    bool DisarmAndReleaseDecisionPin() noexcept {
        auto state = access_.load(std::memory_order_acquire);
        if (state == AuthoredStopFenceAccess::Armed) {
            if (access_.compare_exchange_strong(state, AuthoredStopFenceAccess::Disarmed,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                state = AuthoredStopFenceAccess::Disarmed;
            }
        }
        return state != AuthoredStopFenceAccess::Applying &&
            state != AuthoredStopFenceAccess::Prepared && pin_.Release();
    }

private:
    friend class AuthoredStopFenceLaneReference;
    friend class AuthoredStopFenceLaneProof;
    bool CanonicalLaneProgram(const TimelineTransactionProgramRef& reference) const noexcept {
        if (program == nullptr || programGeneration == 0 || reference.program != program ||
            reference.programGeneration != programGeneration ||
            program->generation != programGeneration ||
            program->role != TimelineProgramRole::AuthoredStopFence ||
            program->authoredStopFence != this || program->payloads == nullptr ||
            program->payloadCount != 1 || decision.slot != UINT32_MAX ||
            decision.generation != 0) return false;
        const auto& payload = program->payloads[0];
        return payload.kind == TimelineEventKind::Stop &&
            payload.reserved == kTimelineEventAuthoredRootStop &&
            payload.authoredStopOperation == identity &&
            payload.envelopeEndFrame == identity.stopFrame;
    }
    void AbortLaneProof() const noexcept {
        auto expected = AuthoredStopFenceAccess::Prepared;
        const bool released = access_.compare_exchange_strong(expected,
            AuthoredStopFenceAccess::Armed, std::memory_order_release,
            std::memory_order_relaxed);
        assert(released);
        if (released) operation->ReleaseApply();
    }
    const DecisionTable* const decisionTable_;
    const bool decisionPinned_;
    DecisionFuturePin pin_;
    mutable std::atomic<AuthoredStopFenceAccess> access_{AuthoredStopFenceAccess::Armed};
};

inline void AuthoredStopFenceLaneReference::Reset() noexcept {
    if (const auto* descriptor = std::exchange(descriptor_, nullptr))
        descriptor->operation->ReleaseApply();
    lastReadSequence_ = 0;
    payloads_ = nullptr;
}

inline bool AuthoredStopFenceLaneReference::Matches(
    const TimelineTransactionProgramRef& reference) const noexcept {
    return descriptor_ != nullptr && descriptor_->CanonicalLaneProgram(reference) &&
        reference.program->payloads == payloads_;
}

inline void AuthoredStopFenceLaneProof::Reset() noexcept {
    if (const auto* descriptor = std::exchange(descriptor_, nullptr))
        descriptor->AbortLaneProof();
}

} // namespace harken

#endif // HARKEN_CORE_AUTHORED_STOP_FENCE_H
