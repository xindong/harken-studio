#ifndef HARKEN_CORE_SPARSE_ROW_STORAGE_H
#define HARKEN_CORE_SPARSE_ROW_STORAGE_H

#include "harken/core/LogicAllocCheckpoint.h"
#include "harken/core/StableRowStorage.h"

#include <algorithm>
#include <array>
#include <bit>

namespace harken {

// One control owner prepares pages before publishing any row on them.
// Pages remain charged and address-stable until quiescent destruction.
template <class T>
class SparseRowSegment {
public:
    SparseRowSegment() = default;
    SparseRowSegment(const SparseRowSegment&) = delete;
    SparseRowSegment& operator=(const SparseRowSegment&) = delete;
    ~SparseRowSegment() {
        if (memguard::IsAudioTagged() && capacity_ != 0) std::terminate();
        for (std::uint32_t page = 0; page < pageCount_; ++page) {
            if (!PageConstructed(page)) continue;
            const auto first = static_cast<std::uint64_t>(page) * rowsPerPage_;
            const auto count = std::min<std::uint64_t>(rowsPerPage_, capacity_ - first);
            for (std::uint32_t index = 0; index < count; ++index)
                std::destroy_at(AddressOf(static_cast<std::uint32_t>(first + index)));
        }
    }

    bool Reserve(std::uint32_t capacity) noexcept {
        static_assert(std::is_nothrow_default_constructible_v<T>);
        static_assert(std::is_nothrow_destructible_v<T>);
        if (memguard::IsAudioTagged() || capacity_ != 0) return false;
        const auto pageBytes = detail::StablePageReservation::SystemPageBytes();
        if (pageBytes < sizeof(T) || pageBytes % alignof(T) != 0) return false;
        const auto rowsPerPage = pageBytes / sizeof(T);
        const auto pages = capacity / rowsPerPage + (capacity % rowsPerPage != 0);
        if (pages > static_cast<std::size_t>(PTRDIFF_MAX) / pageBytes) return false;
        const auto words = pages / 64u + (pages % 64u != 0);
        std::unique_ptr<std::atomic<std::uint64_t>[]> prepared;
        try {
            prepared = std::make_unique<std::atomic<std::uint64_t>[]>(words);
        } catch (...) { harken::detail::RecordCurrentLogicAllocFailure(harken::detail::LogicAllocSite::EngineStorage); return false; }
        for (std::size_t word = 0; word < words; ++word)
            prepared[word].store(0, std::memory_order_relaxed);
        if (!reservation_.Reserve(pages * pageBytes, alignof(T), true)) return false;
        prepared_ = std::move(prepared);
        directoryWords_ = words;
        rowsPerPage_ = static_cast<std::uint32_t>(rowsPerPage);
        pageBytes_ = pageBytes;
        pageCount_ = static_cast<std::uint32_t>(pages);
        capacity_ = capacity;
        return true;
    }

    bool ConstructPageFor(std::uint32_t slot) noexcept {
        if (slot >= capacity_) return false;
        const auto page = slot / rowsPerPage_;
        if (PageConstructed(page)) return true;
        if (memguard::IsAudioTagged() ||
            !reservation_.CommitRange(static_cast<std::size_t>(page) * pageBytes_, pageBytes_))
            return false;
        const auto first = static_cast<std::uint64_t>(page) * rowsPerPage_;
        const auto count = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(rowsPerPage_, capacity_ - first));
        for (std::uint32_t index = 0; index < count; ++index)
            std::construct_at(AddressOf(static_cast<std::uint32_t>(first + index)));
        constructed_.fetch_add(count, std::memory_order_relaxed);
        prepared_[page / 64u].fetch_or(std::uint64_t{1} << (page % 64u), std::memory_order_release);
        return true;
    }

    bool ConstructThrough(std::uint32_t count) noexcept {
        if (count > capacity_) return false;
        while (prefix_ < count) {
            if (!ConstructPageFor(prefix_)) return false;
            const auto next = (std::uint64_t{prefix_ / rowsPerPage_} + 1u) * rowsPerPage_;
            prefix_ = static_cast<std::uint32_t>(std::min<std::uint64_t>(capacity_, next));
        }
        return true;
    }

    bool IsConstructed(std::uint32_t slot) const noexcept {
        return slot < capacity_ && PageConstructed(slot / rowsPerPage_);
    }
    T& operator[](std::uint32_t slot) noexcept {
        assert(IsConstructed(slot));
        return *std::launder(AddressOf(slot));
    }
    const T& operator[](std::uint32_t slot) const noexcept {
        assert(IsConstructed(slot));
        return *std::launder(AddressOf(slot));
    }
    std::uint32_t Capacity() const noexcept { return capacity_; }
    std::size_t RequestedBytes() const noexcept {
        return static_cast<std::size_t>(constructed_.load(std::memory_order_acquire)) * sizeof(T);
    }
    std::size_t DirectoryBytes() const noexcept {
        return directoryWords_ * sizeof(std::atomic<std::uint64_t>) + reservation_.DirectoryBytes();
    }
    std::size_t CommittedBytes() const noexcept { return reservation_.CommittedBytes(); }
    std::size_t ReservedBytes() const noexcept { return reservation_.ReservedBytes(); }
    void SetCommitPageLimitForTests(std::size_t pages) noexcept {
        reservation_.SetCommitPageLimitForTests(pages);
    }

private:
    bool PageConstructed(std::uint32_t page) const noexcept {
        return (prepared_[page / 64u].load(std::memory_order_acquire) &
            (std::uint64_t{1} << (page % 64u))) != 0;
    }
    T* AddressOf(std::uint32_t slot) const noexcept {
        const auto offset = static_cast<std::size_t>(slot / rowsPerPage_) * pageBytes_ +
            static_cast<std::size_t>(slot % rowsPerPage_) * sizeof(T);
        return reinterpret_cast<T*>(static_cast<std::byte*>(reservation_.Address()) + offset);
    }

    detail::StablePageReservation reservation_;
    std::unique_ptr<std::atomic<std::uint64_t>[]> prepared_;
    std::size_t directoryWords_ = 0;
    std::size_t pageBytes_ = 0;
    std::uint32_t rowsPerPage_ = 0;
    std::uint32_t pageCount_ = 0;
    std::uint32_t capacity_ = 0;
    std::uint32_t prefix_ = 0;
    std::atomic<std::uint32_t> constructed_{0};
};

// A segment is published once and survives until both users are quiescent.
// Each segment retains the sparse page map, including its acquire/release publication.
template<class T>
class SparseRowStorage {
public:
    bool ConstructPageFor(std::uint32_t slot) noexcept {
        if (slot == UINT32_MAX || memguard::IsAudioTagged()) return IsConstructed(slot);
        const auto index = SegmentFor(slot);
        auto* segment = EnsureSegment(index);
        if (!segment) return false;
        const auto before = segment->CommittedBytes();
        segment->SetCommitPageLimitForTests(remainingPages_);
        const bool ok = segment->ConstructPageFor(slot - SegmentBegin(index));
        Account(*segment, before);
        return ok;
    }

    bool ConstructThrough(std::uint32_t count) noexcept {
        if (memguard::IsAudioTagged()) return false;
        while (prefix_ < count) {
            const auto index = SegmentFor(prefix_);
            auto* segment = EnsureSegment(index);
            if (!segment) return false;
            const auto begin = SegmentBegin(index);
            const auto end = std::min(count - begin, segment->Capacity());
            const auto before = segment->CommittedBytes();
            segment->SetCommitPageLimitForTests(remainingPages_);
            const bool ok = segment->ConstructThrough(end);
            Account(*segment, before);
            if (!ok) return false;
            prefix_ = begin + end;
        }
        return true;
    }

    bool IsConstructed(std::uint32_t slot) const noexcept {
        if (slot == UINT32_MAX) return false;
        const auto index = SegmentFor(slot);
        const auto* segment = published_[index].load(std::memory_order_acquire);
        return segment && segment->IsConstructed(slot - SegmentBegin(index));
    }

    T& operator[](std::uint32_t slot) noexcept {
        const auto index = SegmentFor(slot);
        return (*published_[index].load(std::memory_order_acquire))[slot - SegmentBegin(index)];
    }
    const T& operator[](std::uint32_t slot) const noexcept {
        return const_cast<SparseRowStorage&>(*this)[slot];
    }
    std::size_t RequestedBytes() const noexcept { return Sum(&Segment::RequestedBytes); }
    std::size_t DirectoryBytes() const noexcept { return Sum(&Segment::DirectoryBytes); }
    std::size_t CommittedBytes() const noexcept { return Sum(&Segment::CommittedBytes); }
    std::size_t ReservedBytes() const noexcept { return Sum(&Segment::ReservedBytes); }
    void SetCommitPageLimitForTests(std::size_t pages) noexcept { remainingPages_ = pages; }

private:
    using Segment = SparseRowSegment<T>;
    static constexpr unsigned baseShift = std::countr_zero(std::bit_floor(
        static_cast<unsigned>(std::max<std::size_t>(1, 4096 / sizeof(T)))));
    static unsigned SegmentFor(std::uint32_t slot) noexcept {
        return std::bit_width((std::uint64_t{slot} >> baseShift) + 1u) - 1u;
    }
    static std::uint32_t SegmentBegin(unsigned index) noexcept {
        return static_cast<std::uint32_t>(((UINT64_C(1) << index) - 1u) << baseShift);
    }
    Segment* EnsureSegment(unsigned index) noexcept {
        if (owners_[index]) return owners_[index].get();
        try {
            auto segment = std::make_unique<Segment>();
            const auto count = std::min<std::uint64_t>(UINT32_MAX - SegmentBegin(index),
                UINT64_C(1) << (index + baseShift));
            if (!segment->Reserve(static_cast<std::uint32_t>(count))) return nullptr;
            owners_[index] = std::move(segment);
            published_[index].store(owners_[index].get(), std::memory_order_release);
            return owners_[index].get();
        } catch (...) {
            detail::RecordCurrentLogicAllocFailure(detail::LogicAllocSite::EngineStorage);
            return nullptr;
        }
    }
    void Account(const Segment& segment, std::size_t before) noexcept {
        if (remainingPages_ != SIZE_MAX)
            remainingPages_ -= (segment.CommittedBytes() - before) / detail::StablePageReservation::SystemPageBytes();
    }
    std::size_t Sum(std::size_t (Segment::*metric)() const noexcept) const noexcept {
        std::size_t total = 0;
        for (const auto& entry : published_) {
            const auto* segment = entry.load(std::memory_order_acquire);
            if (segment) total += (segment->*metric)();
        }
        return total;
    }
    std::array<std::unique_ptr<Segment>, 32> owners_{};
    std::array<std::atomic<Segment*>, 32> published_{};
    std::uint32_t prefix_ = 0;
    std::size_t remainingPages_ = SIZE_MAX;
};

} // namespace harken

#endif // HARKEN_CORE_SPARSE_ROW_STORAGE_H
