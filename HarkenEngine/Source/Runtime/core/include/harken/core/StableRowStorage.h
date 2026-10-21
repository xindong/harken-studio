#ifndef HARKEN_CORE_STABLE_ROW_STORAGE_H
#define HARKEN_CORE_STABLE_ROW_STORAGE_H

#include "harken/core/AllocGuard.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace harken {
namespace detail {

class StablePageReservation {
public:
    StablePageReservation() = default;
    ~StablePageReservation();
    StablePageReservation(const StablePageReservation&) = delete;
    StablePageReservation& operator=(const StablePageReservation&) = delete;

    bool Reserve(std::size_t bytes, std::size_t alignment, bool sparse = false) noexcept;
    bool CommitThrough(std::size_t bytes) noexcept;
    bool CommitRange(std::size_t offset, std::size_t bytes) noexcept;
    void Swap(StablePageReservation& other) noexcept;
    void* Address() const noexcept { return address_; }
    std::size_t ReservedBytes() const noexcept { return reservedBytes_; }
    std::size_t CommittedBytes() const noexcept { return committedBytes_; }
    std::size_t DirectoryBytes() const noexcept { return committedPages_.capacity() * sizeof(std::uint64_t); }
    static std::size_t SystemPageBytes() noexcept;
    void SetCommitPageLimitForTests(std::size_t pages) noexcept { remainingPagesForTests_ = pages; }
    static void FailNextReservationForTests() noexcept;

private:
    bool CommitPage(std::size_t offset) noexcept;
    void* address_ = nullptr;
    std::size_t reservedBytes_ = 0;
    std::size_t committedBytes_ = 0;
    std::size_t pageBytes_ = 0;
    std::size_t remainingPagesForTests_ = SIZE_MAX;
    std::vector<std::uint64_t> committedPages_;
};

} // namespace detail

struct StableStorageGrowthState {
    std::uint64_t heldRows = 0;
    std::uint32_t preparedUsers = 0;
};

class StableStoragePreparedUse {
public:
    StableStoragePreparedUse() = default;
    explicit StableStoragePreparedUse(StableStorageGrowthState* state) noexcept : state_(state) {
        if (state_ != nullptr) ++state_->preparedUsers;
    }
    ~StableStoragePreparedUse() { if (state_ != nullptr) --state_->preparedUsers; }
    StableStoragePreparedUse(const StableStoragePreparedUse&) = delete;
    StableStoragePreparedUse& operator=(const StableStoragePreparedUse&) = delete;
    StableStoragePreparedUse(StableStoragePreparedUse&& other) noexcept
        : state_(std::exchange(other.state_, nullptr)) {}
private:
    StableStorageGrowthState* state_ = nullptr;
};

// A physical backing hold does not reserve an issuer identity or pin an old reader.
class StableStoragePreparation {
public:
    StableStoragePreparation() = default;
    ~StableStoragePreparation() { Release(); }
    StableStoragePreparation(const StableStoragePreparation&) = delete;
    StableStoragePreparation& operator=(const StableStoragePreparation&) = delete;
    StableStoragePreparation(StableStoragePreparation&& other) noexcept { *this = std::move(other); }
    StableStoragePreparation& operator=(StableStoragePreparation&& other) noexcept {
        if (this != &other) {
            Release();
            state_ = std::exchange(other.state_, nullptr);
            count_ = std::exchange(other.count_, 0u);
        }
        return *this;
    }
    bool Valid() const noexcept { return state_ != nullptr; }
    StableStoragePreparedUse Use() const noexcept { return StableStoragePreparedUse(state_); }
    void Release() noexcept {
        if (state_ != nullptr) {
            assert(!memguard::IsAudioTagged() && state_->heldRows >= count_);
            state_->heldRows -= count_;
            state_ = nullptr;
            count_ = 0;
        }
    }
private:
    template<class T> friend class StableRowStorage;
    friend class ImmediateActionLane;
    StableStorageGrowthState* state_ = nullptr;
    std::uint32_t count_ = 0;
};

// One serialized control owner grows a stable prefix. Readers never grow it.
// Issuer generations and retirement rights remain in the caller's real rows.
template <class T>
class StableRowStorage {
public:
    StableRowStorage() = default;
    ~StableRowStorage() {
        assert(growth_.heldRows == 0 && growth_.preparedUsers == 0);
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
        return ConstructThrough(count, [](std::uint32_t) noexcept { return true; });
    }

    template<class PrepareDependencies>
    bool ConstructThrough(std::uint32_t count, PrepareDependencies&& prepare) noexcept {
        if (count > capacity_) return false;
        if (growth_.preparedUsers != 0) return count <= ConstructedCount();
        const auto protectedCount = static_cast<std::uint32_t>(std::min<std::uint64_t>(
            capacity_, static_cast<std::uint64_t>(count) + growth_.heldRows));
        if (memguard::IsAudioTagged()) return protectedCount <= ConstructedCount();
        return ConstructUnheldThrough(protectedCount, std::forward<PrepareDependencies>(prepare));
    }

    template<class... Args>
    bool EmplaceBack(Args&&... args) noexcept {
        const auto slot = ConstructedCount();
        if (memguard::IsAudioTagged() || growth_.preparedUsers != 0 || growth_.heldRows != 0 ||
            slot == capacity_ || !reservation_.CommitThrough((static_cast<std::size_t>(slot) + 1u) * sizeof(T)))
            return false;
        try { std::construct_at(AddressOf(slot), std::forward<Args>(args)...); }
        catch (...) { return false; }
        constructed_.store(slot + 1u, std::memory_order_release);
        return true;
    }

    bool PrepareBacking(std::uint32_t count, std::uint32_t occupiedPrefix,
                        StableStoragePreparation* out) noexcept {
        return PrepareBacking(count, occupiedPrefix, out,
            [](std::uint32_t) noexcept { return true; });
    }

    template<class PrepareDependencies>
    bool PrepareBacking(std::uint32_t count, std::uint32_t occupiedPrefix,
                        StableStoragePreparation* out, PrepareDependencies&& prepare) noexcept {
        if (out == nullptr || out->Valid() || count > capacity_ || occupiedPrefix > ConstructedCount() ||
            memguard::IsAudioTagged() ||
            growth_.preparedUsers != 0 || growth_.heldRows > UINT64_MAX - count) return false;
        const auto required = static_cast<std::uint32_t>(std::min<std::uint64_t>(
            capacity_, occupiedPrefix + growth_.heldRows + count));
        if (!ConstructUnheldThrough(required, std::forward<PrepareDependencies>(prepare))) return false;
        growth_.heldRows += count;
        out->state_ = &growth_;
        out->count_ = count;
        return true;
    }

private:
    template<class PrepareDependencies>
    bool ConstructUnheldThrough(std::uint32_t count, PrepareDependencies&& prepare) noexcept {
        const auto previous = ConstructedCount();
        if (count > capacity_ || memguard::IsAudioTagged() || !prepare(count)) return false;
        if (count <= previous) return true;
        if (!reservation_.CommitThrough(static_cast<std::size_t>(count) * sizeof(T)))
            return false;
        for (auto index = previous; index < count; ++index)
            std::construct_at(AddressOf(index));
        constructed_.store(count, std::memory_order_release);
        return true;
    }

public:
    std::uint32_t Capacity() const noexcept { return capacity_; }
    bool PreparedUseActive() const noexcept { return growth_.preparedUsers != 0; }
    bool HasPreparedBacking() const noexcept {
        return growth_.heldRows != 0 || growth_.preparedUsers != 0;
    }
    std::uint32_t ConstructedCount() const noexcept {
        return constructed_.load(std::memory_order_acquire);
    }
    // Range iteration visits only constructed rows, never the logical capacity.
    std::uint32_t size() const noexcept { return ConstructedCount(); }
    T* data() noexcept { return static_cast<T*>(reservation_.Address()); }
    const T* data() const noexcept { return static_cast<const T*>(reservation_.Address()); }
    T* begin() noexcept { return data(); }
    const T* begin() const noexcept { return data(); }
    T* end() noexcept { return size() == 0 ? data() : data() + size(); }
    const T* end() const noexcept { return size() == 0 ? data() : data() + size(); }
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
        assert(growth_.heldRows == 0 && other.growth_.heldRows == 0 &&
               growth_.preparedUsers == 0 && other.growth_.preparedUsers == 0);
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
    StableStorageGrowthState growth_;
};

} // namespace harken

#endif // HARKEN_CORE_STABLE_ROW_STORAGE_H
