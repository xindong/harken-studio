#include "harken/core/AudioSliceLease.h"

#include <cassert>

namespace harken {
namespace {

std::uint32_t ReasonBit(AudioSliceInvalidateReason reason) noexcept {
    switch (reason) {
    case AudioSliceInvalidateReason::Cancel:
        return AudioSliceReasonCancel;
    case AudioSliceInvalidateReason::Supersede:
        return AudioSliceReasonSupersede;
    case AudioSliceInvalidateReason::Deadline:
        return AudioSliceReasonDeadline;
    case AudioSliceInvalidateReason::Identity:
        return AudioSliceReasonIdentity;
    case AudioSliceInvalidateReason::WorldReplacement:
        return AudioSliceReasonWorldReplacement;
    case AudioSliceInvalidateReason::TransportReset:
        return AudioSliceReasonTransportReset;
    }
    return 0;
}

AudioSliceInvalidateReason SelectReason(std::uint32_t bits) noexcept {
    if ((bits & AudioSliceReasonTransportReset) != 0) {
        return AudioSliceInvalidateReason::TransportReset;
    }
    if ((bits & AudioSliceReasonWorldReplacement) != 0) {
        return AudioSliceInvalidateReason::WorldReplacement;
    }
    if ((bits & AudioSliceReasonIdentity) != 0) {
        return AudioSliceInvalidateReason::Identity;
    }
    if ((bits & AudioSliceReasonDeadline) != 0) {
        return AudioSliceInvalidateReason::Deadline;
    }
    if ((bits & AudioSliceReasonSupersede) != 0) {
        return AudioSliceInvalidateReason::Supersede;
    }
    return AudioSliceInvalidateReason::Cancel;
}

} // namespace

bool AudioSliceLease::HoldPrivateTransfer() noexcept {
    if (pendingReasonBits_.load(std::memory_order_acquire) != 0 ||
        invalidateAfterSlice_.load(std::memory_order_acquire) != 0) return false;
    auto expected = AudioSliceState::Ready;
    return state_.compare_exchange_strong(expected, AudioSliceState::PrivateTransferReserved,
        std::memory_order_acq_rel, std::memory_order_acquire);
}

bool AudioSliceLease::HoldPublication() noexcept {
    if (pendingReasonBits_.load(std::memory_order_acquire) != 0 ||
        invalidateAfterSlice_.load(std::memory_order_acquire) != 0) return false;
    auto expected = AudioSliceState::Ready;
    return state_.compare_exchange_strong(expected, AudioSliceState::PublicationReserved,
        std::memory_order_acq_rel, std::memory_order_acquire);
}

void AudioSliceLease::ReleasePublication() noexcept {
    assert(state_.load(std::memory_order_acquire) == AudioSliceState::PublicationReserved);
    assert(pendingReasonBits_.load(std::memory_order_acquire) == 0 &&
        invalidateAfterSlice_.load(std::memory_order_acquire) == 0);
    state_.store(AudioSliceState::Ready, std::memory_order_release);
}

void AudioSliceLease::ReleasePrivateTransfer() noexcept {
    assert(state_.load(std::memory_order_acquire) == AudioSliceState::PrivateTransferReserved);
    assert(pendingReasonBits_.load(std::memory_order_acquire) == 0 &&
        invalidateAfterSlice_.load(std::memory_order_acquire) == 0);
    state_.store(AudioSliceState::Ready, std::memory_order_release);
}

void AudioSliceLease::ConsumePrivateTransferCancellation() noexcept {
    assert(state_.load(std::memory_order_acquire) == AudioSliceState::PrivateTransferReserved);
    assert(pendingReasonBits_.load(std::memory_order_acquire) == 0);
    assert(invalidateAfterSlice_.load(std::memory_order_acquire) == 0);
    pendingReasonBits_.store(AudioSliceReasonCancel, std::memory_order_relaxed);
    state_.store(AudioSliceState::SliceInvalidated, std::memory_order_release);
}

bool AudioSliceLease::TryClaim() noexcept {
    AudioSliceState expected = AudioSliceState::Ready;
    return state_.compare_exchange_strong(
        expected, AudioSliceState::SliceClaimed, std::memory_order_acq_rel,
        std::memory_order_acquire);
}

bool AudioSliceLease::MarkApplied() noexcept {
    AudioSliceState state = state_.load(std::memory_order_acquire);
    for (;;) {
        if (state == AudioSliceState::SliceClaimed) {
            if (state_.compare_exchange_weak(
                    state, AudioSliceState::SliceApplied,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return true;
            }
            continue;
        }
        if (state == AudioSliceState::SliceClaimedWithInvalidation) {
            invalidateAfterSlice_.store(1, std::memory_order_release);
            if (state_.compare_exchange_weak(
                    state, AudioSliceState::SliceApplied,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return true;
            }
            continue;
        }
        return false;
    }
}

bool AudioSliceLease::AbortClaimedBeforeApply(
    AudioSliceInvalidateReason reason) noexcept {
    const std::uint32_t bit = ReasonBit(reason);
    if (bit == 0) return false;
    auto state = state_.load(std::memory_order_acquire);
    if (state != AudioSliceState::SliceClaimed &&
        state != AudioSliceState::SliceClaimedWithInvalidation) return false;
    pendingReasonBits_.fetch_or(bit, std::memory_order_acq_rel);
    for (;;) {
        if (state != AudioSliceState::SliceClaimed &&
            state != AudioSliceState::SliceClaimedWithInvalidation)
            return false;
        if (state_.compare_exchange_weak(
                state, AudioSliceState::SliceInvalidated,
                std::memory_order_acq_rel, std::memory_order_acquire))
            return true;
    }
}

AudioSliceInvalidateDisposition AudioSliceLease::Invalidate(
    AudioSliceInvalidateReason reason) noexcept {
    const std::uint32_t bit = ReasonBit(reason);
    if (bit == 0) {
        return AudioSliceInvalidateDisposition::InvalidReason;
    }
    // Only the single producer can hold a never-published private lease.
    if (state_.load(std::memory_order_acquire) == AudioSliceState::PrivateTransferReserved)
        return AudioSliceInvalidateDisposition::PrivateTransferHeld;
    if (state_.load(std::memory_order_acquire) == AudioSliceState::PublicationReserved)
        return AudioSliceInvalidateDisposition::PublicationHeld;
    pendingReasonBits_.fetch_or(bit, std::memory_order_acq_rel);
    for (;;) {
        AudioSliceState state = state_.load(std::memory_order_acquire);
        if (state == AudioSliceState::PrivateTransferReserved)
            return AudioSliceInvalidateDisposition::PrivateTransferHeld;
        if (state == AudioSliceState::PublicationReserved)
            return AudioSliceInvalidateDisposition::PublicationHeld;
        if (state == AudioSliceState::Ready) {
            if (state_.compare_exchange_weak(
                    state, AudioSliceState::SliceInvalidated,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return AudioSliceInvalidateDisposition::InvalidatedReady;
            }
            continue;
        }
        if (state == AudioSliceState::SliceInvalidated) {
            return AudioSliceInvalidateDisposition::AlreadyInvalidated;
        }
        if (state == AudioSliceState::SliceApplied) {
            return AudioSliceInvalidateDisposition::AppliedNeedsLifecycle;
        }
        if (state == AudioSliceState::SliceClaimedWithInvalidation) {
            return AudioSliceInvalidateDisposition::DeferredAfterClaim;
        }
        if (state_.compare_exchange_weak(
                state, AudioSliceState::SliceClaimedWithInvalidation,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return AudioSliceInvalidateDisposition::DeferredAfterClaim;
        }
    }
}

bool AudioSliceLease::ConsumePostSliceInvalidation(
    AudioSliceInvalidateReason* outReason) noexcept {
    if (outReason == nullptr ||
        state_.load(std::memory_order_acquire) !=
            AudioSliceState::SliceApplied ||
        invalidateAfterSlice_.exchange(0, std::memory_order_acq_rel) == 0) {
        return false;
    }
    const std::uint32_t bits =
        pendingReasonBits_.load(std::memory_order_acquire);
    if (bits == 0) {
        return false;
    }
    *outReason = SelectReason(bits);
    return true;
}

AudioSliceLeaseSnapshot AudioSliceLease::Capture() const noexcept {
    AudioSliceLeaseSnapshot snapshot{};
    const AudioSliceState state = state_.load(std::memory_order_acquire);
    snapshot.state =
        state == AudioSliceState::SliceClaimedWithInvalidation
            ? AudioSliceState::SliceClaimed
            : state;
    snapshot.invalidateAfterSlice =
        state == AudioSliceState::SliceClaimedWithInvalidation ||
        invalidateAfterSlice_.load(std::memory_order_acquire) != 0;
    snapshot.pendingReasonBits =
        pendingReasonBits_.load(std::memory_order_acquire);
    return snapshot;
}

bool AudioSliceLease::PrepareReplacement() noexcept {
    AudioSliceState expected = AudioSliceState::Ready;
    return state_.compare_exchange_strong(
        expected, AudioSliceState::ReplacementReserved,
        std::memory_order_acq_rel, std::memory_order_acquire);
}

bool AudioSliceLease::CommitPreparedReplacement() noexcept {
    AudioSliceState expected = AudioSliceState::ReplacementReserved;
    return state_.compare_exchange_strong(
        expected, AudioSliceState::Ready, std::memory_order_acq_rel,
        std::memory_order_acquire);
}

bool AudioSliceLease::CancelPreparedReplacement() noexcept {
    return CommitPreparedReplacement();
}

bool AudioSliceLease::ResetForReuse() noexcept {
    const AudioSliceState current = state_.load(std::memory_order_acquire);
    if (current != AudioSliceState::SliceApplied &&
        current != AudioSliceState::SliceInvalidated) {
        return false;
    }
    pendingReasonBits_.store(0, std::memory_order_relaxed);
    invalidateAfterSlice_.store(0, std::memory_order_relaxed);
    state_.store(AudioSliceState::Ready, std::memory_order_release);
    return true;
}

AudioSliceApplyStatus ApplyAudioSliceWhole(
    AudioSliceLease& lease, const AudioSliceOperation* operations,
    std::uint16_t operationCount) noexcept {
    if (operations == nullptr || operationCount == 0) {
        return AudioSliceApplyStatus::Invalid;
    }
    for (std::uint16_t index = 0; index < operationCount; ++index) {
        if (operations[index].validate == nullptr ||
            operations[index].apply == nullptr ||
            !operations[index].validate(operations[index].context)) {
            return AudioSliceApplyStatus::ValidationFailed;
        }
    }
    if (!lease.TryClaim()) {
        return AudioSliceApplyStatus::SliceUnavailable;
    }
    for (std::uint16_t index = 0; index < operationCount; ++index) {
        if (operations[index].afterClaim != nullptr) {
            operations[index].afterClaim(operations[index].context);
        }
    }
    for (std::uint16_t index = 0; index < operationCount; ++index) {
        operations[index].apply(operations[index].context);
    }
    if (!lease.MarkApplied()) {
        return AudioSliceApplyStatus::Invalid;
    }
    return lease.Capture().invalidateAfterSlice
               ? AudioSliceApplyStatus::AppliedWithDeferredInvalidation
               : AudioSliceApplyStatus::Applied;
}

} // namespace harken
