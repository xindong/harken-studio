#ifndef HARKEN_API_ENGINE_QUEUE_SCHEDULE_STORAGE_H
#define HARKEN_API_ENGINE_QUEUE_SCHEDULE_STORAGE_H

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

    EngineQueueScheduleColumns() noexcept = default;
    ~EngineQueueScheduleColumns() { assert(!borrowed_); }
    // Copies are detached snapshots, never another loan of the original buffers.
    EngineQueueScheduleColumns(const EngineQueueScheduleColumns& other)
        : payloads(other.payloads), events(other.events), runtimes(other.runtimes),
          schedules(other.schedules), releaseObservers(other.releaseObservers) {}
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
    }
    QueueScheduleShape Shape() const noexcept {
        const auto programs = std::min({runtimes.size(), schedules.size(), releaseObservers.size()});
        const auto eventCount = std::min(payloads.size(), events.size());
        return {static_cast<std::uint32_t>(std::min<std::size_t>(programs, UINT32_MAX)),
            static_cast<std::uint32_t>(std::min<std::size_t>(eventCount, UINT32_MAX))};
    }
    std::size_t StorageBytes() const noexcept {
        return payloads.capacity() * sizeof(TimelineEventPayload) +
            events.capacity() * sizeof(EventTemplate) +
            runtimes.capacity() * sizeof(TimelineProgramView) +
            schedules.capacity() * sizeof(ProgramView) +
            releaseObservers.capacity() * sizeof(VoiceReleaseObserver);
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
            if (owner_ == nullptr) return;
            if (previous_ != nullptr) previous_->next_ = this;
            else owner_->preparations_ = this;
            if (next_ != nullptr) next_->previous_ = this;
        }
        EngineQueueScheduleStorage* owner_ = nullptr;
        Preparation* previous_ = nullptr;
        Preparation* next_ = nullptr;
        QueueScheduleShape shape_{};
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

    bool Configure(std::uint32_t logicalCapacity) noexcept {
        if (configured_ || memguard::IsAudioTagged()) return false;
        logicalCapacity_ = logicalCapacity;
        configured_ = true;
        return true;
    }

    bool Prepare(QueueScheduleShape shape, Preparation* out) noexcept {
        return Prepare(shape, out, []() noexcept { return true; });
    }
    template<class AllowAllocation>
    bool Prepare(QueueScheduleShape shape, Preparation* out, AllowAllocation&& allow) noexcept {
        if (!configured_ || out == nullptr || out->Valid() || preparedUsers_ != 0 ||
            memguard::IsAudioTagged()) return false;
        RefreshEnvelope();
        const bool alreadyPrepared = hasEnvelope_ && envelope_.Covers(shape);
        const auto target = QueueScheduleShape::Maximum(envelope_, shape);
        std::unique_ptr<Slot> additions;
        // Existing owners already guarantee the whole global supply for this shape.
        const auto supplied = alreadyPrepared ? logicalCapacity_ : QualifiedGroups(target);
        for (auto count = supplied; count < logicalCapacity_; ++count) {
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
        out->next_ = preparations_;
        if (preparations_ != nullptr) preparations_->previous_ = out;
        preparations_ = out;
        envelope_ = target;
        hasEnvelope_ = true;
        if (!alreadyPrepared) CollectUnused();
        return true;
    }

    bool Acquire(EngineQueueScheduleColumns& columns, QueueScheduleShape actualShape,
        Loan* out) noexcept {
        return Acquire(columns, actualShape, out, []() noexcept { return true; });
    }
    template<class AllowAllocation>
    bool Acquire(EngineQueueScheduleColumns& columns, QueueScheduleShape actualShape,
        Loan* out, AllowAllocation&& allow) noexcept {
        if (!configured_ || out == nullptr || out->Valid() || loans_ >= logicalCapacity_ ||
            columns.borrowed_ || columns.StorageBytes() != 0 || memguard::IsAudioTagged()) return false;
        if (preparedUsers_ == 0) RefreshEnvelope();
        const auto target = QueueScheduleShape::Maximum(envelope_, actualShape);
        Slot** available = &free_;
        while (*available != nullptr && !(*available)->columns.Shape().Covers(target))
            available = &(*available)->freeNext;
        if (*available == nullptr) {
            if (preparedUsers_ != 0) return false;
            if (free_ != nullptr) {
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
        std::uint32_t qualifiedLoans = 0;
        for (auto* slot = slots_.get(); slot != nullptr; slot = slot->next.get())
            if (slot->borrower != nullptr && slot->Columns().Shape().Covers(envelope_))
                ++qualifiedLoans;
        auto keep = hasEnvelope_ ? logicalCapacity_ - qualifiedLoans : 0u;
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
        assert(!hasEnvelope_ || keep == 0);
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
        hasEnvelope_ = preparations_ != nullptr;
        for (auto* preparation = preparations_; preparation != nullptr; preparation = preparation->next_)
            envelope_ = QueueScheduleShape::Maximum(envelope_, preparation->shape_);
    }
    std::uint32_t QualifiedGroups(QueueScheduleShape shape) const noexcept {
        std::uint32_t count = 0;
        for (auto* slot = slots_.get(); slot != nullptr && count < logicalCapacity_; slot = slot->next.get())
            if (slot->Columns().Shape().Covers(shape)) ++count;
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
                resize(columns.releaseObservers, shape.programs);
        } catch (...) { return false; }
    }
    template<class AllowAllocation>
    static std::unique_ptr<Slot> AllocateSlot(QueueScheduleShape shape, AllowAllocation& allow) noexcept {
        try {
            if (!allow()) return nullptr;
            auto slot = std::make_unique<Slot>();
            if (!AllocateColumns(slot->columns, shape, allow)) return nullptr;
            return slot;
        } catch (...) { return nullptr; }
    }

    std::unique_ptr<Slot> slots_;
    Slot* free_ = nullptr;
    Preparation* preparations_ = nullptr;
    QueueScheduleShape envelope_{};
    std::uint32_t logicalCapacity_ = 0;
    std::uint32_t loans_ = 0;
    std::uint32_t preparedUsers_ = 0;
    bool configured_ = false;
    bool hasEnvelope_ = false;
};

} // namespace harken

#endif
