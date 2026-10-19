#ifndef HARKEN_CORE_STABLE_ROW_STORAGE_H
#define HARKEN_CORE_STABLE_ROW_STORAGE_H

#include "harken/core/AllocGuard.h"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>

namespace harken {
namespace detail {

class StablePageReservation {
public:
    StablePageReservation() = default;
    ~StablePageReservation();
    StablePageReservation(const StablePageReservation&) = delete;
    StablePageReservation& operator=(const StablePageReservation&) = delete;

    bool Reserve(std::size_t bytes, std::size_t alignment) noexcept;
    bool CommitThrough(std::size_t bytes) noexcept;
    void Swap(StablePageReservation& other) noexcept;
    void* Address() const noexcept { return address_; }
    std::size_t ReservedBytes() const noexcept { return reservedBytes_; }
    std::size_t CommittedBytes() const noexcept { return committedBytes_; }
    void SetCommitPageLimitForTests(std::size_t pages) noexcept { remainingPagesForTests_ = pages; }
    static void FailNextReservationForTests() noexcept;

private:
    void* address_ = nullptr;
    std::size_t reservedBytes_ = 0;
    std::size_t committedBytes_ = 0;
    std::size_t pageBytes_ = 0;
    std::size_t remainingPagesForTests_ = SIZE_MAX;
};

} // namespace detail

// One serialized control owner grows a stable prefix. Readers never grow it.
// Issuer generations and retirement rights remain in the caller's real rows.
template <class T>
class StableRowStorage {
public:
    StableRowStorage() = default;
    ~StableRowStorage() {
        if (memguard::IsAudioTagged() && ConstructedCount() != 0) std::terminate();
        for (auto count = ConstructedCount(); count != 0; --count)
            std::destroy_at(AddressOf(count - 1));
    }
    StableRowStorage(const StableRowStorage&) = delete;
    StableRowStorage& operator=(const StableRowStorage&) = delete;

    // Called on an empty, private candidate; swap only at quiescence.
    bool Reserve(std::uint32_t capacity) noexcept {
        static_assert(std::is_nothrow_default_constructible_v<T>);
        static_assert(std::is_nothrow_destructible_v<T>);
        if (memguard::IsAudioTagged() || capacity_ != 0 ||
            capacity > static_cast<std::size_t>(PTRDIFF_MAX) / sizeof(T))
            return false;
        if (!reservation_.Reserve(static_cast<std::size_t>(capacity) * sizeof(T),
                                  alignof(T)))
            return false;
        capacity_ = capacity;
        return true;
    }

    bool ConstructThrough(std::uint32_t count) noexcept {
        const auto previous = ConstructedCount();
        if (count <= previous) return true;
        if (count > capacity_ || memguard::IsAudioTagged() ||
            !reservation_.CommitThrough(static_cast<std::size_t>(count) * sizeof(T)))
            return false;
        for (auto index = previous; index < count; ++index)
            std::construct_at(AddressOf(index));
        constructed_.store(count, std::memory_order_release);
        return true;
    }

    std::uint32_t Capacity() const noexcept { return capacity_; }
    std::uint32_t ConstructedCount() const noexcept {
        return constructed_.load(std::memory_order_acquire);
    }
    std::size_t RequestedBytes() const noexcept {
        return static_cast<std::size_t>(ConstructedCount()) * sizeof(T);
    }
    std::size_t ReservedBytes() const noexcept { return reservation_.ReservedBytes(); }
    std::size_t CommittedBytes() const noexcept { return reservation_.CommittedBytes(); }

    T& operator[](std::uint32_t slot) noexcept {
        assert(slot < ConstructedCount());
        return *std::launder(AddressOf(slot));
    }
    const T& operator[](std::uint32_t slot) const noexcept {
        assert(slot < ConstructedCount());
        return *std::launder(AddressOf(slot));
    }
    std::uint32_t IndexOf(const T& row) const noexcept {
        const auto offset = reinterpret_cast<std::uintptr_t>(&row) -
                            reinterpret_cast<std::uintptr_t>(reservation_.Address());
        assert(offset % sizeof(T) == 0 && offset / sizeof(T) < ConstructedCount());
        return static_cast<std::uint32_t>(offset / sizeof(T));
    }

    void Swap(StableRowStorage& other) noexcept {
        if (memguard::IsAudioTagged()) std::terminate();
        reservation_.Swap(other.reservation_);
        std::swap(capacity_, other.capacity_);
        const auto previous = constructed_.load(std::memory_order_relaxed);
        constructed_.store(other.constructed_.load(std::memory_order_relaxed),
                           std::memory_order_relaxed);
        other.constructed_.store(previous, std::memory_order_relaxed);
    }
    void SetCommitPageLimitForTests(std::size_t pages) noexcept {
        reservation_.SetCommitPageLimitForTests(pages);
    }

private:
    T* AddressOf(std::uint32_t slot) const noexcept {
        return reinterpret_cast<T*>(static_cast<std::byte*>(reservation_.Address()) +
                                    static_cast<std::size_t>(slot) * sizeof(T));
    }

    detail::StablePageReservation reservation_;
    std::uint32_t capacity_ = 0;
    std::atomic<std::uint32_t> constructed_{0};
};

} // namespace harken

#endif // HARKEN_CORE_STABLE_ROW_STORAGE_H
