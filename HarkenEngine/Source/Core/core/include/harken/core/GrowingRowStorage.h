#ifndef HARKEN_CORE_GROWING_ROW_STORAGE_H
#define HARKEN_CORE_GROWING_ROW_STORAGE_H

#include "harken/core/StableRowStorage.h"

#include <array>
#include <bit>
#include <iterator>

namespace harken {

// Serialized control-side growth adds stable segments without relocating rows.
// The directory covers the index representation, not a preallocated row quota.
template<class T>
class GrowingRowStorage {
public:
    GrowingRowStorage() = default;
    GrowingRowStorage(const GrowingRowStorage&) = delete;
    GrowingRowStorage& operator=(const GrowingRowStorage&) = delete;
    ~GrowingRowStorage() {
        assert(growth_.heldRows == 0 && growth_.preparedUsers == 0);
    }

    template<class PrepareDependencies>
    bool ConstructThrough(std::uint32_t count, PrepareDependencies&& prepare) noexcept {
        if (growth_.preparedUsers != 0) return count <= constructed_;
        const auto required = static_cast<std::uint64_t>(count) + growth_.heldRows;
        if (required > UINT32_MAX) return false;
        if (memguard::IsAudioTagged()) return required <= constructed_;
        return ConstructUnheldThrough(static_cast<std::uint32_t>(required),
            std::forward<PrepareDependencies>(prepare));
    }

    bool ConstructThrough(std::uint32_t count) noexcept {
        return ConstructThrough(count, [](std::uint32_t) noexcept { return true; });
    }

    template<class PrepareDependencies>
    bool PrepareBacking(std::uint32_t count, std::uint32_t occupiedPrefix,
        StableStoragePreparation* out, PrepareDependencies&& prepare) noexcept {
        if (out == nullptr || out->Valid() || occupiedPrefix > constructed_ ||
            memguard::IsAudioTagged() || growth_.preparedUsers != 0 ||
            growth_.heldRows > UINT32_MAX - static_cast<std::uint64_t>(occupiedPrefix) ||
            count > UINT32_MAX - occupiedPrefix - growth_.heldRows) return false;
        const auto required = static_cast<std::uint32_t>(occupiedPrefix + growth_.heldRows + count);
        if (!ConstructUnheldThrough(required, std::forward<PrepareDependencies>(prepare))) return false;
        growth_.heldRows += count;
        out->state_ = &growth_;
        out->count_ = count;
        return true;
    }

    bool PrepareBacking(std::uint32_t count, std::uint32_t occupiedPrefix,
        StableStoragePreparation* out) noexcept {
        return PrepareBacking(count, occupiedPrefix, out,
            [](std::uint32_t) noexcept { return true; });
    }

    bool PreparedUseActive() const noexcept { return growth_.preparedUsers != 0; }
    bool HasPreparedBacking() const noexcept { return growth_.heldRows != 0 || PreparedUseActive(); }
    std::uint64_t HeldRows() const noexcept { return growth_.heldRows; }
    std::uint32_t Capacity() const noexcept { return capacity_; }
    std::uint32_t ConstructedCount() const noexcept { return constructed_; }
    std::uint32_t size() const noexcept { return constructed_; }

    T& operator[](std::uint32_t slot) noexcept {
        assert(slot < constructed_);
        const auto segment = SegmentFor(slot);
        return (*segments_[segment])[slot - SegmentBegin(segment)];
    }
    const T& operator[](std::uint32_t slot) const noexcept {
        assert(slot < constructed_);
        const auto segment = SegmentFor(slot);
        return (*segments_[segment])[slot - SegmentBegin(segment)];
    }

    template<bool Const>
    class Iterator {
        using Owner = std::conditional_t<Const, const GrowingRowStorage, GrowingRowStorage>;
    public:
        using value_type = T;
        using reference = std::conditional_t<Const, const T&, T&>;
        using pointer = std::conditional_t<Const, const T*, T*>;
        using difference_type = std::ptrdiff_t;
        using iterator_category = std::forward_iterator_tag;
        Iterator() = default;
        Iterator(Owner* owner, std::uint32_t slot) noexcept : owner_(owner), slot_(slot) {}
        reference operator*() const noexcept { return (*owner_)[slot_]; }
        pointer operator->() const noexcept { return &**this; }
        Iterator& operator++() noexcept { ++slot_; return *this; }
        Iterator operator++(int) noexcept { auto old = *this; ++*this; return old; }
        bool operator==(const Iterator& other) const noexcept {
            return owner_ == other.owner_ && slot_ == other.slot_;
        }
    private:
        Owner* owner_ = nullptr;
        std::uint32_t slot_ = 0;
    };
    Iterator<false> begin() noexcept { return {this, 0}; }
    Iterator<false> end() noexcept { return {this, constructed_}; }
    Iterator<true> begin() const noexcept { return {this, 0}; }
    Iterator<true> end() const noexcept { return {this, constructed_}; }

    std::size_t RequestedBytes() const noexcept {
        std::size_t bytes = 0;
        for (const auto& segment : segments_)
            if (segment) bytes += sizeof(*segment) + segment->RequestedBytes();
        return bytes;
    }
    std::size_t ReservedBytes() const noexcept {
        std::size_t bytes = 0;
        for (const auto& segment : segments_) if (segment) bytes += segment->ReservedBytes();
        return bytes;
    }
    std::size_t CommittedBytes() const noexcept {
        std::size_t bytes = 0;
        for (const auto& segment : segments_) if (segment) bytes += segment->CommittedBytes();
        return bytes;
    }
    void SetCommitPageLimitForTests(std::size_t pages) noexcept { remainingPagesForTests_ = pages; }

    void Swap(GrowingRowStorage& other) noexcept {
        if (memguard::IsAudioTagged()) std::terminate();
        assert(!HasPreparedBacking() && !other.HasPreparedBacking());
        segments_.swap(other.segments_);
        std::swap(baseRows_, other.baseRows_);
        std::swap(pageBytes_, other.pageBytes_);
        std::swap(capacity_, other.capacity_);
        std::swap(constructed_, other.constructed_);
        std::swap(remainingPagesForTests_, other.remainingPagesForTests_);
    }

private:
    std::uint32_t SegmentFor(std::uint32_t slot) const noexcept {
        return static_cast<std::uint32_t>(std::bit_width(
            static_cast<std::uint64_t>(slot / baseRows_) + 1u) - 1u);
    }
    std::uint32_t SegmentBegin(std::uint32_t segment) const noexcept {
        return static_cast<std::uint32_t>(static_cast<std::uint64_t>(baseRows_) *
            ((UINT64_C(1) << segment) - 1u));
    }

    template<class PrepareDependencies>
    bool ConstructUnheldThrough(std::uint32_t count, PrepareDependencies&& prepare) noexcept {
        if (count <= constructed_) return true;
        if (memguard::IsAudioTagged() || growth_.preparedUsers != 0 || !prepare(count)) return false;
        if (baseRows_ == 0) {
            pageBytes_ = detail::StablePageReservation::SystemPageBytes();
            if (pageBytes_ == 0) return false;
            baseRows_ = std::bit_floor(static_cast<std::uint32_t>(
                std::max<std::size_t>(1u, std::min<std::size_t>(UINT32_MAX, pageBytes_ / sizeof(T)))));
        }
        auto cursor = constructed_;
        while (cursor < count) {
            const auto index = SegmentFor(cursor);
            const auto first = SegmentBegin(index);
            const auto segmentCapacity = static_cast<std::uint32_t>(std::min<std::uint64_t>(
                static_cast<std::uint64_t>(baseRows_) << index,
                static_cast<std::uint64_t>(UINT32_MAX) - first));
            if (!segments_[index]) {
                try {
                    auto candidate = std::make_unique<StableRowStorage<T>>();
                    if (!candidate->Reserve(segmentCapacity)) return false;
                    segments_[index] = std::move(candidate);
                } catch (...) { return false; }
                capacity_ = first + segmentCapacity;
            }
            auto& segment = *segments_[index];
            segment.SetCommitPageLimitForTests(remainingPagesForTests_);
            const auto previousBytes = segment.CommittedBytes();
            const auto localCount = std::min(count - first, segmentCapacity);
            const bool ready = segment.ConstructThrough(localCount);
            if (remainingPagesForTests_ != SIZE_MAX)
                remainingPagesForTests_ -= (segment.CommittedBytes() - previousBytes) / pageBytes_;
            if (!ready) return false;
            cursor = first + localCount;
        }
        // Dependencies and every requested segment are ready before exposing the prefix.
        constructed_ = count;
        return true;
    }

    std::array<std::unique_ptr<StableRowStorage<T>>, std::numeric_limits<std::uint32_t>::digits> segments_{};
    StableStorageGrowthState growth_;
    std::uint32_t baseRows_ = 0;
    std::uint32_t capacity_ = 0;
    std::uint32_t constructed_ = 0;
    std::size_t pageBytes_ = 0;
    std::size_t remainingPagesForTests_ = SIZE_MAX;
};

} // namespace harken

#endif
