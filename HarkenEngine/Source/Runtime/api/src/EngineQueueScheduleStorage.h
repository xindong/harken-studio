#ifndef HARKEN_API_ENGINE_QUEUE_SCHEDULE_STORAGE_H
#define HARKEN_API_ENGINE_QUEUE_SCHEDULE_STORAGE_H

#include "harken/core/LogicAllocCheckpoint.h"
#include "harken/core/AllocGuard.h"
#include "harken/core/SourceTerminalRecord.h"
#include "harken/core/TimelinePlanner.h"
#include "harken/core/TimelineProgram.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <utility>
#include <vector>

namespace harken {

struct EnginePlayableMemberToken {
    std::uint32_t slot = UINT32_MAX;
    std::uint64_t generation = 0;
    bool Valid() const noexcept { return slot != UINT32_MAX && generation != 0; }
    bool operator==(const EnginePlayableMemberToken&) const noexcept = default;
};

struct QueueScheduleShape {
    std::uint32_t programs = 0;
    std::uint32_t events = 0;

    bool Covers(QueueScheduleShape other) const noexcept {
        return programs >= other.programs && events >= other.events;
    }
    static QueueScheduleShape Maximum(QueueScheduleShape a, QueueScheduleShape b) noexcept {
        return {std::max(a.programs, b.programs), std::max(a.events, b.events)};
    }
};

struct EngineQueueScheduleColumns {
    std::vector<TimelineEventPayload> payloads;
    std::vector<EventTemplate> events;
    std::vector<TimelineProgramView> runtimes;
    std::vector<ProgramView> schedules;
    std::vector<VoiceReleaseObserver> releaseObservers;
    std::vector<EnginePlayableMemberToken> members;

    EngineQueueScheduleColumns() noexcept = default;
    ~EngineQueueScheduleColumns() { assert(!borrowed_); }
    // Copies are detached snapshots, never another loan of the original buffers.
    EngineQueueScheduleColumns(const EngineQueueScheduleColumns& other)
        : payloads(other.payloads), events(other.events), runtimes(other.runtimes),
          schedules(other.schedules), releaseObservers(other.releaseObservers), members(other.members) {}
    EngineQueueScheduleColumns& operator=(const EngineQueueScheduleColumns& other) {
        if (this != &other) {
            if (borrowed_) std::terminate();
            EngineQueueScheduleColumns copy(other);
            Swap(copy);
        }
        return *this;
    }
    EngineQueueScheduleColumns(EngineQueueScheduleColumns&&) = delete;
    EngineQueueScheduleColumns& operator=(EngineQueueScheduleColumns&&) = delete;

    void Swap(EngineQueueScheduleColumns& other) noexcept {
        payloads.swap(other.payloads);
        events.swap(other.events);
        runtimes.swap(other.runtimes);
        schedules.swap(other.schedules);
        releaseObservers.swap(other.releaseObservers);
        members.swap(other.members);
    }
    QueueScheduleShape Shape() const noexcept {
        const auto programs = std::min({runtimes.size(), schedules.size(), releaseObservers.size(), members.size()});
        const auto eventCount = std::min(payloads.size(), events.size());
        return {static_cast<std::uint32_t>(std::min<std::size_t>(programs, UINT32_MAX)),
            static_cast<std::uint32_t>(std::min<std::size_t>(eventCount, UINT32_MAX))};
    }
    std::size_t StorageBytes() const noexcept {
        return payloads.capacity() * sizeof(TimelineEventPayload) +
            events.capacity() * sizeof(EventTemplate) +
            runtimes.capacity() * sizeof(TimelineProgramView) +
            schedules.capacity() * sizeof(ProgramView) +
            releaseObservers.capacity() * sizeof(VoiceReleaseObserver) +
            members.capacity() * sizeof(EnginePlayableMemberToken);
    }
private:
    friend class EngineQueueScheduleStorage;
    bool borrowed_ = false;
};

// Single logic producer. Borrowers keep their original addresses until a loan returns.
class EngineQueueScheduleStorage {
    struct Slot {
        EngineQueueScheduleColumns columns;
        EngineQueueScheduleColumns* borrower = nullptr;
        std::unique_ptr<Slot> next;
        Slot* freeNext = nullptr;

        const EngineQueueScheduleColumns& Columns() const noexcept {
            return borrower != nullptr ? *borrower : columns;
        }
    };

public:
    class Preparation;

    class PreparedUse {
    public:
        PreparedUse() noexcept = default;
        ~PreparedUse() { Release(); }
        PreparedUse(const PreparedUse&) = delete;
        PreparedUse& operator=(const PreparedUse&) = delete;
        PreparedUse(PreparedUse&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)) {}
        PreparedUse& operator=(PreparedUse&& other) noexcept {
            if (this != &other) {
                Release();
                owner_ = std::exchange(other.owner_, nullptr);
            }
            return *this;
        }
        bool Valid() const noexcept { return owner_ != nullptr; }
        void Release() noexcept {
            if (owner_ != nullptr) {
                assert(owner_->preparedUsers_ != 0);
                --owner_->preparedUsers_;
                owner_ = nullptr;
            }
        }
    private:
        friend class EngineQueueScheduleStorage;
        friend class Preparation;
        explicit PreparedUse(EngineQueueScheduleStorage* owner) noexcept : owner_(owner) {
            if (owner_ != nullptr) {
                if (owner_->preparedUsers_ == UINT32_MAX) owner_ = nullptr;
                else ++owner_->preparedUsers_;
            }
        }
        EngineQueueScheduleStorage* owner_ = nullptr;
    };

    class Preparation {
    public:
        Preparation() noexcept = default;
        ~Preparation() { Release(); }
        Preparation(const Preparation&) = delete;
        Preparation& operator=(const Preparation&) = delete;
        Preparation(Preparation&& other) noexcept { Take(other); }
        Preparation& operator=(Preparation&& other) noexcept {
            if (this != &other) { Release(); Take(other); }
            return *this;
        }
        bool Valid() const noexcept { return owner_ != nullptr; }
        PreparedUse Use() const noexcept { return PreparedUse(owner_); }
        void Release() noexcept {
            if (owner_ == nullptr) return;
            if (previous_ != nullptr) previous_->next_ = next_;
            else owner_->preparations_ = next_;
            if (next_ != nullptr) next_->previous_ = previous_;
            owner_->reserved_ -= count_;
            owner_ = nullptr;
            previous_ = next_ = nullptr;
            shape_ = {};
        }
    private:
        friend class EngineQueueScheduleStorage;
        void Take(Preparation& other) noexcept {
            owner_ = std::exchange(other.owner_, nullptr);
            previous_ = std::exchange(other.previous_, nullptr);
            next_ = std::exchange(other.next_, nullptr);
            shape_ = std::exchange(other.shape_, {});
            count_ = std::exchange(other.count_, 0u);
            if (owner_ == nullptr) return;
            if (previous_ != nullptr) previous_->next_ = this;
            else owner_->preparations_ = this;
            if (next_ != nullptr) next_->previous_ = this;
        }
        EngineQueueScheduleStorage* owner_ = nullptr;
        Preparation* previous_ = nullptr;
        Preparation* next_ = nullptr;
        QueueScheduleShape shape_{};
        std::uint32_t count_ = 0;
    };

    class Loan {
    public:
        Loan() noexcept = default;
        ~Loan() { Reset(); }
        Loan(const Loan&) = delete;
        Loan& operator=(const Loan&) = delete;
        Loan(Loan&& other) noexcept { Take(other); }
        Loan& operator=(Loan&& other) noexcept {
            if (this != &other) { Reset(); Take(other); }
            return *this;
        }
        bool Valid() const noexcept { return owner_ != nullptr; }
        // The native bank's actual reader, owner and fence gates precede this call.
        void Reset() noexcept {
            if (owner_ == nullptr) return;
            assert(slot_->borrower == borrower_ && borrower_->borrowed_ && owner_->loans_ != 0);
            slot_->columns.Swap(*borrower_);
            borrower_->borrowed_ = false;
            slot_->borrower = nullptr;
            slot_->freeNext = owner_->free_;
            owner_->free_ = slot_;
            --owner_->loans_;
            owner_ = nullptr;
            slot_ = nullptr;
            borrower_ = nullptr;
        }
    private:
        friend class EngineQueueScheduleStorage;
        void Take(Loan& other) noexcept {
            owner_ = std::exchange(other.owner_, nullptr);
            slot_ = std::exchange(other.slot_, nullptr);
            borrower_ = std::exchange(other.borrower_, nullptr);
        }
        EngineQueueScheduleStorage* owner_ = nullptr;
        Slot* slot_ = nullptr;
        EngineQueueScheduleColumns* borrower_ = nullptr;
    };

    EngineQueueScheduleStorage() noexcept = default;
    ~EngineQueueScheduleStorage() {
        assert(preparations_ == nullptr && preparedUsers_ == 0 && loans_ == 0);
        if (memguard::IsAudioTagged() && slots_ != nullptr) std::terminate();
        DestroySlots(slots_);
    }
    EngineQueueScheduleStorage(const EngineQueueScheduleStorage&) = delete;
    EngineQueueScheduleStorage& operator=(const EngineQueueScheduleStorage&) = delete;

    bool Prepare(QueueScheduleShape shape, std::uint32_t count, Preparation* out) noexcept {
        return Prepare(shape, count, out, []() noexcept { return true; });
    }
    template<class AllowAllocation>
    bool Prepare(QueueScheduleShape shape, std::uint32_t count, Preparation* out, AllowAllocation&& allow) noexcept {
        if (out == nullptr || out->Valid() || preparedUsers_ != 0 ||
            count > UINT32_MAX - reserved_ || memguard::IsAudioTagged()) return false;
        RefreshEnvelope();
        const auto target = QueueScheduleShape::Maximum(envelope_, shape);
        const auto needed = reserved_ + count;
        std::unique_ptr<Slot> additions;
        for (auto supplied = QualifiedFree(target, needed); supplied < needed; ++supplied) {
            auto slot = AllocateSlot(target, allow);
            if (slot == nullptr) {
                DestroySlots(additions);
                return false;
            }
            slot->next = std::move(additions);
            additions = std::move(slot);
        }
        // Only a complete global supply may publish a larger retained envelope.
        while (additions != nullptr) {
            auto next = std::move(additions->next);
            AddSlot(std::move(additions));
            additions = std::move(next);
        }
        out->owner_ = this;
        out->shape_ = shape;
        out->count_ = count;
        reserved_ = needed;
        out->next_ = preparations_;
        if (preparations_ != nullptr) preparations_->previous_ = out;
        preparations_ = out;
        envelope_ = target;
        CollectUnused();
        return true;
    }

    bool Acquire(EngineQueueScheduleColumns& columns, QueueScheduleShape actualShape,
        Loan* out) noexcept {
        return Acquire(columns, actualShape, out, []() noexcept { return true; });
    }
    template<class AllowAllocation>
    bool Acquire(EngineQueueScheduleColumns& columns, QueueScheduleShape actualShape,
        Loan* out, AllowAllocation&& allow) noexcept {
        if (out == nullptr || out->Valid() || loans_ == UINT32_MAX ||
            columns.borrowed_ || columns.StorageBytes() != 0 || memguard::IsAudioTagged()) return false;
        if (preparedUsers_ == 0) RefreshEnvelope();
        const auto target = QueueScheduleShape::Maximum(envelope_, actualShape);
        Slot** available = &free_;
        while (*available != nullptr && !(*available)->columns.Shape().Covers(target))
            available = &(*available)->freeNext;
        // An ordinary acquisition may not use the last rows promised to pending callers.
        const bool protect = preparedUsers_ == 0 && reserved_ != 0 &&
            QualifiedFree(envelope_, static_cast<std::uint64_t>(reserved_) + 1u) <= reserved_;
        if (*available == nullptr || protect) {
            if (preparedUsers_ != 0) return false;
            if (!protect && free_ != nullptr) {
                EngineQueueScheduleColumns replacement;
                if (!AllocateColumns(replacement, target, allow)) return false;
                free_->columns.Swap(replacement);
            } else {
                auto slot = AllocateSlot(target, allow);
                if (slot == nullptr) return false;
                AddSlot(std::move(slot));
            }
            available = &free_;
        }
        auto* slot = *available;
        *available = slot->freeNext;
        slot->freeNext = nullptr;
        slot->columns.Swap(columns);
        columns.borrowed_ = true;
        slot->borrower = &columns;
        ++loans_;
        out->owner_ = this;
        out->slot_ = slot;
        out->borrower_ = &columns;
        return true;
    }

    // Retired borrowers are retained; only free groups can be reclaimed here.
    bool CollectUnused() noexcept {
        if (preparedUsers_ != 0 || memguard::IsAudioTagged()) return false;
        RefreshEnvelope();
        auto keep = reserved_;
        free_ = nullptr;
        auto* link = &slots_;
        while (*link != nullptr) {
            auto* slot = link->get();
            if (slot->borrower != nullptr) {
                link = &slot->next;
            } else if (keep != 0 && slot->columns.Shape().Covers(envelope_)) {
                --keep;
                slot->freeNext = free_;
                free_ = slot;
                link = &slot->next;
            } else {
                auto removed = std::move(*link);
                *link = std::move(removed->next);
            }
        }
        return true;
    }

    // Dynamic slot metadata and actual capacities, including all outstanding loans.
    std::size_t StorageBytes() const noexcept {
        std::size_t bytes = 0;
        for (auto* slot = slots_.get(); slot != nullptr; slot = slot->next.get())
            bytes += sizeof(Slot) + slot->Columns().StorageBytes();
        return bytes;
    }

private:
    static void DestroySlots(std::unique_ptr<Slot>& slots) noexcept {
        while (slots != nullptr) {
            auto removed = std::move(slots);
            slots = std::move(removed->next);
        }
    }
    void AddSlot(std::unique_ptr<Slot> slot) noexcept {
        slot->freeNext = free_;
        free_ = slot.get();
        slot->next = std::move(slots_);
        slots_ = std::move(slot);
    }
    void RefreshEnvelope() noexcept {
        assert(preparedUsers_ == 0);
        envelope_ = {};
        for (auto* preparation = preparations_; preparation != nullptr; preparation = preparation->next_)
            envelope_ = QueueScheduleShape::Maximum(envelope_, preparation->shape_);
    }
    std::uint32_t QualifiedFree(QueueScheduleShape shape, std::uint64_t limit) const noexcept {
        std::uint32_t count = 0;
        for (auto* slot = free_; slot != nullptr && count < limit; slot = slot->freeNext)
            if (slot->columns.Shape().Covers(shape)) ++count;
        return count;
    }
    template<class AllowAllocation>
    static bool AllocateColumns(EngineQueueScheduleColumns& columns, QueueScheduleShape shape,
        AllowAllocation& allow) noexcept {
        try {
            const auto resize = [&](auto& column, std::uint32_t count) {
                if (count == 0) return true;
                if (!allow()) return false;
                column.resize(count);
                return true;
            };
            return resize(columns.payloads, shape.events) && resize(columns.events, shape.events) &&
                resize(columns.runtimes, shape.programs) && resize(columns.schedules, shape.programs) &&
                resize(columns.releaseObservers, shape.programs) && resize(columns.members, shape.programs);
        } catch (...) { harken::detail::RecordCurrentLogicAllocFailure(harken::detail::LogicAllocSite::EngineStorage); return false; }
    }
    template<class AllowAllocation>
    static std::unique_ptr<Slot> AllocateSlot(QueueScheduleShape shape, AllowAllocation& allow) noexcept {
        try {
            if (!allow()) return nullptr;
            auto slot = std::make_unique<Slot>();
            if (!AllocateColumns(slot->columns, shape, allow)) return nullptr;
            return slot;
        } catch (...) { harken::detail::RecordCurrentLogicAllocFailure(harken::detail::LogicAllocSite::EngineStorage); return nullptr; }
    }

    std::unique_ptr<Slot> slots_;
    Slot* free_ = nullptr;
    Preparation* preparations_ = nullptr;
    QueueScheduleShape envelope_{};
    std::uint32_t reserved_ = 0;
    std::uint32_t loans_ = 0;
    std::uint32_t preparedUsers_ = 0;
};

} // namespace harken

#endif
