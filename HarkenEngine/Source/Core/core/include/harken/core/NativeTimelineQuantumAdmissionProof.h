#ifndef HARKEN_CORE_NATIVE_TIMELINE_QUANTUM_ADMISSION_PROOF_H
#define HARKEN_CORE_NATIVE_TIMELINE_QUANTUM_ADMISSION_PROOF_H
#include <atomic>
#include <cassert>
#include <cstdint>
#include <utility>
namespace harken {
class TimelineArmTable;
class VoicePool;
class BlockArena;
class ImmediateLaneTimelineFinalizer;
// Fixed renderer-owned state. Its owning finalizer must outlive every proof.
// Epoch invalidation does not return a foreign proof or block Old rendering.
class NativeTimelineQuantumAdmissionState {
public:
    NativeTimelineQuantumAdmissionState() = default;
    NativeTimelineQuantumAdmissionState(const NativeTimelineQuantumAdmissionState&) = delete;
    NativeTimelineQuantumAdmissionState& operator=(const NativeTimelineQuantumAdmissionState&) = delete;
private:
    friend class ImmediateLaneTimelineFinalizer;
    friend class NativeTimelineQuantumAdmissionProof;
    void Invalidate() noexcept {
        const auto value = epoch_.load(std::memory_order_relaxed);
        if (value != 0) epoch_.store(value == UINT64_MAX ? 0 : value + 1u, std::memory_order_release);
    }
    std::atomic<std::uint64_t> epoch_{1};
    mutable std::atomic<std::uint32_t> readers_{0};
};
// No numeric, boolean, observation or capacity DTO can construct a proof.
// The only root issuer is actual successful native scratch admission.
class NativeTimelineQuantumAdmissionProof {
public:
    NativeTimelineQuantumAdmissionProof() = default;
    NativeTimelineQuantumAdmissionProof(const NativeTimelineQuantumAdmissionProof&) = delete;
    NativeTimelineQuantumAdmissionProof& operator=(const NativeTimelineQuantumAdmissionProof&) = delete;
    NativeTimelineQuantumAdmissionProof(NativeTimelineQuantumAdmissionProof&& other) noexcept { Take(other); }
    NativeTimelineQuantumAdmissionProof& operator=(NativeTimelineQuantumAdmissionProof&& other) noexcept {
        if (this != &other) { Release(); Take(other); }
        return *this;
    }
    ~NativeTimelineQuantumAdmissionProof() { Release(); }
    bool Valid() const noexcept {
        return state_ != nullptr && epoch_ != 0 && state_->epoch_.load(std::memory_order_acquire) == epoch_;
    }
    // Comparisons only. The consuming Engine must use its genuine fixed
    // Arena/Voice/Arm domain; these addresses cannot mint another proof.
    bool MatchesNativeDomain(const VoicePool* voices, const TimelineArmTable* arms,
                             const BlockArena* arena) const noexcept {
        return Valid() && voices != nullptr && arms != nullptr && arena != nullptr &&
            voices == voices_ && arms == arms_ && arena == arena_;
    }
    bool Matches(const TimelineArmTable* arms, std::uint64_t start,
                 std::uint64_t end, std::uint64_t completedSequence) const noexcept {
        return Valid() && arms != nullptr && arms == arms_ && blockFrames_ != 0 &&
            quantum_ == completedSequence && quantum_ <= UINT64_MAX / blockFrames_ &&
            start == quantum_ * blockFrames_ && start <= UINT64_MAX - blockFrames_ &&
            end == start + blockFrames_;
    }
    bool Borrow(NativeTimelineQuantumAdmissionProof* out) const noexcept {
        if (!Valid() || out == nullptr || out->state_ != nullptr) return false;
        auto count = state_->readers_.load(std::memory_order_relaxed);
        do {
            if (count == UINT32_MAX) return false;
        } while (!state_->readers_.compare_exchange_weak(count, count + 1u,
            std::memory_order_acq_rel, std::memory_order_relaxed));
        out->state_ = state_; out->epoch_ = epoch_; out->arms_ = arms_;
        out->quantum_ = quantum_; out->blockFrames_ = blockFrames_;
        out->voices_ = voices_; out->arena_ = arena_;
        return true;
    }
    void Release() noexcept {
        const auto* actual = std::exchange(state_, nullptr);
        epoch_ = quantum_ = 0; arms_ = nullptr; blockFrames_ = 0;
        voices_ = nullptr; arena_ = nullptr;
        if (actual != nullptr) {
            const auto count = actual->readers_.fetch_sub(1, std::memory_order_acq_rel);
            assert(count != 0); (void)count;
        }
    }
private:
    friend class ImmediateLaneTimelineFinalizer;
    void Take(NativeTimelineQuantumAdmissionProof& other) noexcept {
        state_ = std::exchange(other.state_, nullptr);
        epoch_ = std::exchange(other.epoch_, 0);
        arms_ = std::exchange(other.arms_, nullptr);
        quantum_ = std::exchange(other.quantum_, 0);
        blockFrames_ = std::exchange(other.blockFrames_, 0);
        voices_ = std::exchange(other.voices_, nullptr);
        arena_ = std::exchange(other.arena_, nullptr);
    }
    const NativeTimelineQuantumAdmissionState* state_ = nullptr;
    const TimelineArmTable* arms_ = nullptr;
    const VoicePool* voices_ = nullptr;
    const BlockArena* arena_ = nullptr;
    std::uint64_t epoch_ = 0, quantum_ = 0;
    std::uint32_t blockFrames_ = 0;
};
} // namespace harken
#endif
