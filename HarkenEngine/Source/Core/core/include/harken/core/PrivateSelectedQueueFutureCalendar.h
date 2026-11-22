#ifndef HARKEN_CORE_PRIVATE_SELECTED_QUEUE_FUTURE_CALENDAR_H
#define HARKEN_CORE_PRIVATE_SELECTED_QUEUE_FUTURE_CALENDAR_H
#include "harken/core/PrivateSelectedQueueFutureSpan.h"

namespace harken {
// Scheduling input remains scheduling input. The original Bank/Source holder must
// outlive this calendar; PinPrivate consumes only actual Core/native reader rights.
class PrivateSelectedQueueFutureCalendar {
public:
    PrivateSelectedQueueFutureCalendar() = default;
    PrivateSelectedQueueFutureCalendar(const PrivateSelectedQueueFutureCalendar&) = delete;
    PrivateSelectedQueueFutureCalendar& operator=(const PrivateSelectedQueueFutureCalendar&) = delete;
    ~PrivateSelectedQueueFutureCalendar() { assert(lane_ == nullptr); }

    bool Prepare(PrivateSelectedQueueFutureSpanPool& spans,
        const PrivateSelectedQueueFutureSpanPool::Owner& owner,
        PrivateSelectedQueueFutureDomain& domain,
        const ImmediateLaneEvent* events, std::uint32_t eventCount,
        const ImmediateLaneMemberManifest* member, TimelineArmTable& arms) noexcept {
        if (lane_ != nullptr || !spans.Matches(owner) || !domain.Valid() ||
            (eventCount != 0 && events == nullptr)) return false;
        const auto* native = spans.CalendarRow(owner);
        if (native == nullptr || domain.calendar_ != nullptr) return false;
        const auto& row = *native;
        if (row.phase != PrivateSelectedQueueFutureSpanPool::Phase::FrozenUnbound ||
            row.bound.load(std::memory_order_acquire) || row.nativeGate.load(std::memory_order_acquire))
            return false;
        if (!spans.MatchesCalendarDomain(row, domain)) return false;
        if (row.value.newMemberCount > 1u || (row.value.newMemberCount != 0 && member == nullptr)) return false;
        if (row.value.newMemberCount != 0 &&
            (!(member->group == row.backing.groups[1]) ||
             member->arm.slot != row.value.newMember.slot ||
             member->arm.generation != row.value.newMember.generation ||
             member->arm.renderSlot != row.value.newMember.renderSlot ||
             member->arm.renderGeneration != row.value.newMember.renderGeneration ||
             !member->future ||
             member->eventOrdinal >= eventCount || member->eventSpanCount == 0 ||
             member->eventSpanCount > eventCount - member->eventOrdinal)) return false;
        if (!owner.Borrow(&span_)) return false;
        const ImmediateLaneDemand demand{eventCount, 2, row.value.newMemberCount, 1};
        if (!domain.ReserveCalendar(demand, &handle_, &original_)) {
            span_.Release(); return false;
        }
        lane_ = domain.lane_; domain_ = &domain; spans_ = &spans;
        domain.calendar_ = this;
        bool prepared = true;
        for (std::size_t i = 0; i < 2; ++i) {
            prepared = prepared && domain.BindGroup(row.backing.groups[i], handle_.slot, handle_.generation) &&
                lane_->DeclareGroup(handle_, {row.backing.groups[i], row.backing.relativeFrames[i]});
        }
        if (prepared && row.value.newMemberCount != 0)
            prepared = lane_->DeclareMember(handle_, *member);
        prepared = prepared && lane_->Prepare(handle_, events, eventCount);
        // No pin/physical mutation has occurred, and the new unpublished handle
        // has never escaped this factory on failure.
        if (!prepared) {
            const bool released = Abort(0); assert(released); (void)released;
            return false;
        }
        (void)arms; // Actual pins are issued only in PinPrivate after native binding.
        return true;
    }

    bool BindOriginalFirstS(const PrivateOriginalQueueGroupClockProof& actualClock,
        const TimelineArmTable& arms, const DecisionTable& decisions) noexcept {
        if (lane_ == nullptr || placed_ || !span_.Valid()) return false;
        std::uint64_t anchor = 0;
        if (!spans_->CaptureCalendarFirstS(span_, actualClock, arms, decisions, &anchor)) return false;
        if (!layoutPlaced_) {
            if (!lane_->TryPlaceExactPrivate(handle_, anchor)) return false;
            layoutPlaced_ = true;
        }
        if (!domain_->Rebase(anchor)) return false;
        placed_ = true;
        return true;
    }

    bool BindOriginalFirstS(
        const QueueInstanceTable::OriginalConditionalLaneClock& actualClock) noexcept {
        if (lane_ == nullptr || placed_ || !span_.Valid()) return false;
        std::uint64_t anchor = 0;
        if (!spans_->CaptureCalendarFirstS(span_, actualClock, &anchor)) return false;
        if (!layoutPlaced_) {
            if (!lane_->TryPlaceExactPrivate(handle_, anchor)) return false;
            layoutPlaced_ = true;
        }
        if (!domain_->Rebase(anchor)) return false;
        placed_ = true;
        return true;
    }

    bool PinPrivate(TimelineArmTable& arms, DecisionTable& decisions,
        std::uint32_t maxGroupsPerQuantum) noexcept {
        if (lane_ == nullptr || !placed_ || !span_.Valid()) return false;
        const auto* row = spans_->CalendarRow(span_);
        if (row == nullptr || row->closed.load(std::memory_order_acquire) ||
            row->nativeGate.load(std::memory_order_acquire)) return false;
        auto* pool = spans_->CalendarPool(*row);
        pinned_ = pool != nullptr && lane_->PinArms(handle_, arms) &&
            lane_->PinGroups(handle_, *pool, decisions, maxGroupsPerQuantum);
        return pinned_;
    }

    bool PublishSelected() noexcept {
        if (lane_ == nullptr || !placed_ || !pinned_ || published_ ||
            !spans_->HasSelectedCalendar(span_, false)) return false;
        const auto* row = spans_->CalendarRow(span_);
        if (row == nullptr || !row->decisions->CanPublishFrontierActivation(row->backing.coordinator))
            return false;
        // All rows and pins were acquired while private; the final Lane store
        // exposes this exact retained Queue successor to the native reader.
        if (!domain_->PublishGroups() ||
            !row->decisions->PublishFrontierActivation(row->backing.coordinator) ||
            !lane_->Publish(handle_)) return false;
        published_ = true;
        return true;
    }

    bool RetireSelected(std::uint64_t completed) noexcept {
        if (lane_ == nullptr || !published_ || !spans_->HasSelectedCalendar(span_, true) ||
            !lane_->Cancel(handle_) || !lane_->Retire(handle_, completed, &original_)) return false;
        span_.Release();
        domain_->calendar_ = nullptr;
        lane_ = nullptr; domain_ = nullptr; spans_ = nullptr; handle_ = {};
        placed_ = false; layoutPlaced_ = false; pinned_ = false; published_ = false;
        return true;
    }

    ImmediateLaneHandle HandleObservation() const noexcept { return handle_; }
    bool BorrowOriginalAttempt(OriginalImmediateLaneAttemptLifetimePin* out) const noexcept {
        return original_.Borrow(out);
    }

    bool Abort(std::uint64_t completed) noexcept {
        if (lane_ == nullptr) return true;
        if (!lane_->OwnsNeverCommitted(handle_, domain_->marker_) ||
            !lane_->Cancel(handle_) || !lane_->Retire(handle_, completed, &original_)) return false;
        // Native groups, Frontier/Decision/Source owners remain with the original
        // producer and still require their own exact native disposition.
        span_.Release();
        domain_->calendar_ = nullptr;
        lane_ = nullptr; domain_ = nullptr; spans_ = nullptr; handle_ = {}; placed_ = false; layoutPlaced_ = false;
        pinned_ = false; published_ = false;
        return true;
    }
private:
    friend struct Engine;
    void ReturnOwnStorageAfterQuiescence() noexcept {
        if (lane_ == nullptr) return;
        // Engine has joined callback and workers. Native rows remain owned by
        // their tables until those tables are destroyed; no audio fence is minted.
        original_.Return();
        span_.Release();
        domain_->calendar_ = nullptr;
        lane_ = nullptr; domain_ = nullptr; spans_ = nullptr; handle_ = {};
        placed_ = layoutPlaced_ = pinned_ = published_ = false;
    }
    ImmediateActionLane* lane_ = nullptr;
    PrivateSelectedQueueFutureDomain* domain_ = nullptr;
    PrivateSelectedQueueFutureSpanPool* spans_ = nullptr;
    ImmediateLaneHandle handle_{};
    OriginalImmediateLaneAttemptLifetimePin original_{};
    PrivateSelectedQueueFutureSpanPool::Retention span_{};
    bool placed_ = false;
    bool layoutPlaced_ = false;
    bool pinned_ = false;
    bool published_ = false;
};
} // namespace harken
#endif
