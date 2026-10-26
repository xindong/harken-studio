#ifndef HARKEN_CORE_MONITOR_VOICE_CONTROL_H
#define HARKEN_CORE_MONITOR_VOICE_CONTROL_H

#include "harken/core/MonitorRequest.h"
#include "harken/core/SourceTerminalRecord.h"

#include <atomic>
#include <cstdint>
#include <type_traits>

namespace harken {

class MonitorVoiceControlReceipt;

struct MonitorVoiceControlBinding {
    MonitorVoiceControlReceipt* receipt = nullptr;
    std::uint64_t ticket = 0;
    std::uint64_t render = 0;
    std::uint32_t ledgerSlot = UINT32_MAX;
    std::uint32_t ledgerGeneration = 0;

    bool Valid() const noexcept { return receipt != nullptr && ticket != 0; }
};

struct MonitorVoiceControlFact {
    MonitorControlCause cause{};
    std::uint64_t render = 0;
    std::uint64_t frame = 0;
    std::uint32_t ledgerSlot = UINT32_MAX;
    std::uint32_t ledgerGeneration = 0;
    SourceTerminalKind sourceKind = SourceTerminalKind::None;
    bool timelineControl = false;
};

struct MonitorVoicePlaybackEnd {
    std::uint64_t audioBlock = 0;
    std::uint64_t frame = 0;
    SourceTerminalKind kind = SourceTerminalKind::None;
    MonitorControlCause cause{};
    bool timelineControl = false;
};

namespace detail { struct MonitorVoiceControlTestAccess; }

// A fixed ledger-capacity mailbox. Logic never writes the ordinary payload.
// Arm/read/invalidate share the sole logic owner; only the audio coordinator
// publishes. Neither side waits for the other or retains a playback resource.
class MonitorVoiceControlReceipt {
public:
    static constexpr std::uint64_t kMaxTicket = UINT64_MAX >> 2;

    bool TryArm(std::uint64_t ticket) noexcept {
        if (ticket == 0 || ticket > kMaxTicket) return false;
        const auto arm = [ticket](std::atomic<std::uint64_t>& control) {
            auto previous = control.load(std::memory_order_acquire);
            if (ticket <= (previous >> 2) || PhaseOf(previous) == Phase::Writing) return false;
            return control.compare_exchange_strong(previous, State(ticket, Phase::Armed),
                std::memory_order_acq_rel, std::memory_order_acquire);
        };
        if (!arm(control_)) return false;
        if (arm(endControl_)) return true;
        Invalidate(ticket);
        return false;
    }

    void Invalidate(std::uint64_t ticket) noexcept {
        const auto invalidate = [ticket](std::atomic<std::uint64_t>& control) {
            auto previous = control.load(std::memory_order_acquire);
            if (ticket == 0 || (previous >> 2) != ticket || PhaseOf(previous) == Phase::Writing) return;
            (void)control.compare_exchange_strong(previous, State(ticket, Phase::Idle),
                std::memory_order_acq_rel, std::memory_order_acquire);
        };
        invalidate(control_);
        invalidate(endControl_);
    }

    bool TryPublish(const MonitorVoiceControlBinding& binding,
        SourceTerminalKind kind, std::uint64_t frame,
        const MonitorControlCause& cause, bool timelineControl) noexcept {
        if (binding.receipt != this || !binding.Valid() || kind == SourceTerminalKind::None) return false;
        auto expected = State(binding.ticket, Phase::Armed);
        if (!control_.compare_exchange_strong(expected, State(binding.ticket, Phase::Writing),
                std::memory_order_acq_rel, std::memory_order_acquire)) return false;
        payload_ = {cause, binding.render, frame, binding.ledgerSlot,
            binding.ledgerGeneration, kind, timelineControl};
        control_.store(State(binding.ticket, Phase::Complete), std::memory_order_release);
        return true;
    }

    bool TryRead(const MonitorVoiceControlBinding& binding,
        MonitorVoiceControlFact* out) const noexcept {
        if (out == nullptr || binding.receipt != this || !binding.Valid() ||
            control_.load(std::memory_order_acquire) != State(binding.ticket, Phase::Complete)) return false;
        const auto copy = payload_;
        if (copy.ledgerSlot != binding.ledgerSlot || copy.ledgerGeneration != binding.ledgerGeneration ||
            copy.render != binding.render) return false;
        *out = copy;
        return true;
    }

    bool TryPublishEnd(const MonitorVoiceControlBinding& binding,
        std::uint64_t audioBlock, std::uint64_t frame, SourceTerminalKind kind,
        const MonitorControlCause& cause, bool timelineControl) noexcept {
        if (binding.receipt != this || !binding.Valid() || audioBlock == 0 ||
            kind == SourceTerminalKind::None || kind == SourceTerminalKind::Parked) return false;
        auto expected = State(binding.ticket, Phase::Armed);
        if (!endControl_.compare_exchange_strong(expected, State(binding.ticket, Phase::Writing),
                std::memory_order_acq_rel, std::memory_order_acquire)) return false;
        endPayload_ = {audioBlock, frame, kind, cause, timelineControl};
        endBinding_ = binding;
        endControl_.store(State(binding.ticket, Phase::Complete), std::memory_order_release);
        return true;
    }

    bool TryReadEnd(const MonitorVoiceControlBinding& binding,
        MonitorVoicePlaybackEnd* out) const noexcept {
        if (out == nullptr || binding.receipt != this || !binding.Valid() ||
            endControl_.load(std::memory_order_acquire) != State(binding.ticket, Phase::Complete)) return false;
        if (endBinding_.ledgerSlot != binding.ledgerSlot ||
            endBinding_.ledgerGeneration != binding.ledgerGeneration || endBinding_.render != binding.render) return false;
        *out = endPayload_;
        return true;
    }

private:
    friend struct detail::MonitorVoiceControlTestAccess;
    enum class Phase : std::uint64_t { Idle, Armed, Writing, Complete };
    static constexpr std::uint64_t State(std::uint64_t ticket, Phase phase) noexcept {
        return (ticket << 2) | static_cast<std::uint64_t>(phase);
    }
    static constexpr Phase PhaseOf(std::uint64_t state) noexcept {
        return static_cast<Phase>(state & 3u);
    }
    std::atomic<std::uint64_t> control_{0};
    MonitorVoiceControlFact payload_{};
    std::atomic<std::uint64_t> endControl_{0};
    MonitorVoicePlaybackEnd endPayload_{};
    MonitorVoiceControlBinding endBinding_{};
};

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::is_trivially_copyable_v<MonitorVoiceControlFact>);

} // namespace harken

#endif
