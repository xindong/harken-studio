#ifndef HARKEN_CORE_SPARSE_ROW_STORAGE_H
#define HARKEN_CORE_SPARSE_ROW_STORAGE_H

#include "harken/core/StableRowStorage.h"

#include <algorithm>

namespace harken {

// One control owner prepares pages before publishing any row on them.
// Pages remain charged and address-stable until quiescent destruction.
template <class T>
class SparseRowStorage {
public:
    SparseRowStorage() = default;
    SparseRowStorage(const SparseRowStorage&) = delete;
    SparseRowStorage& operator=(const SparseRowStorage&) = delete;
    ~SparseRowStorage() {
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
        } catch (...) { return false; }
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

} // namespace harken

#endif // HARKEN_CORE_SPARSE_ROW_STORAGE_H
