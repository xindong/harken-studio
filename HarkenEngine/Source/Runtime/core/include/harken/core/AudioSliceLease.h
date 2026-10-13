#ifndef HARKEN_CORE_AUDIO_SLICE_LEASE_H
#define HARKEN_CORE_AUDIO_SLICE_LEASE_H

#include <atomic>
#include <cstdint>

namespace harken {

enum class AudioSliceState : std::uint8_t {
    Ready = 0,
    SliceClaimed = 1,
    SliceApplied = 2,
    SliceInvalidated = 3,
    SliceClaimedWithInvalidation = 4,
    ReplacementReserved = 5,
    PrivateTransferReserved = 6,
};

enum class AudioSliceInvalidateReason : std::uint8_t {
    Cancel = 0,
    Supersede = 1,
    Deadline = 2,
    Identity = 3,
    WorldReplacement = 4,
    TransportReset = 5,
};

inline constexpr std::uint32_t AudioSliceReasonCancel = 1u << 0;
inline constexpr std::uint32_t AudioSliceReasonSupersede = 1u << 1;
inline constexpr std::uint32_t AudioSliceReasonDeadline = 1u << 2;
inline constexpr std::uint32_t AudioSliceReasonIdentity = 1u << 3;
inline constexpr std::uint32_t AudioSliceReasonWorldReplacement = 1u << 4;
inline constexpr std::uint32_t AudioSliceReasonTransportReset = 1u << 5;

enum class AudioSliceInvalidateDisposition : std::uint8_t {
    InvalidatedReady = 0,
    DeferredAfterClaim = 1,
    AppliedNeedsLifecycle = 2,
    AlreadyInvalidated = 3,
    InvalidReason = 4,
    PrivateTransferHeld = 5,
};

struct AudioSliceLeaseSnapshot {
    AudioSliceState state = AudioSliceState::Ready;
    bool invalidateAfterSlice = false;
    std::uint32_t pendingReasonBits = 0;
};

struct AudioSliceOperation {
    void* context = nullptr;
    bool (*validate)(void* context) noexcept = nullptr;
    void (*afterClaim)(void* context) noexcept = nullptr;
    void (*apply)(void* context) noexcept = nullptr;
};

enum class AudioSliceApplyStatus : std::uint8_t {
    Applied = 0,
    AppliedWithDeferredInvalidation = 1,
    ValidationFailed = 2,
    SliceUnavailable = 3,
    Invalid = 4,
};

class AudioSliceLease {
public:
    bool TryClaim() noexcept;
    bool MarkApplied() noexcept;
    // The claiming audio owner may abandon a slice before exposing any work.
    // A claimed slice cannot return to Ready because cancellation may race it.
    bool AbortClaimedBeforeApply(AudioSliceInvalidateReason reason) noexcept;
    AudioSliceInvalidateDisposition Invalidate(
        AudioSliceInvalidateReason reason) noexcept;
    bool ConsumePostSliceInvalidation(
        AudioSliceInvalidateReason* outReason) noexcept;
    AudioSliceLeaseSnapshot Capture() const noexcept;
    bool PrepareReplacement() noexcept;
    bool CommitPreparedReplacement() noexcept;
    bool CancelPreparedReplacement() noexcept;
    bool ResetForReuse() noexcept;

private:
    friend class ScheduledTransactionPool;
    bool HoldPrivateTransfer() noexcept;
    void ReleasePrivateTransfer() noexcept;
    void ConsumePrivateTransferCancellation() noexcept;
    std::atomic<AudioSliceState> state_{AudioSliceState::Ready};
    std::atomic<std::uint8_t> invalidateAfterSlice_{0};
    std::atomic<std::uint32_t> pendingReasonBits_{0};
};

AudioSliceApplyStatus ApplyAudioSliceWhole(
    AudioSliceLease& lease, const AudioSliceOperation* operations,
    std::uint16_t operationCount) noexcept;

static_assert(std::atomic<AudioSliceState>::is_always_lock_free);
static_assert(std::atomic<std::uint8_t>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

} // namespace harken

#endif // HARKEN_CORE_AUDIO_SLICE_LEASE_H
