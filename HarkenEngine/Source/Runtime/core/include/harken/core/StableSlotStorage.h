#ifndef HARKEN_CORE_STABLE_SLOT_STORAGE_H
#define HARKEN_CORE_STABLE_SLOT_STORAGE_H

#include "harken/core/StableRowStorage.h"

#include <bit>
#include <iterator>

namespace harken {

// Sparse slots keep their original addresses, including when a high slot is used first.
template<class T>
class StableSlotStorage {
public:
    class PreparedUse {
    public:
        explicit PreparedUse(StableSlotStorage* owner = nullptr) noexcept : owner_(owner) {
            if (owner_ != nullptr) ++owner_->preparedUsers_;
        }
        ~PreparedUse() { if (owner_ != nullptr) --owner_->preparedUsers_; }
        PreparedUse(const PreparedUse&) = delete;
        PreparedUse& operator=(const PreparedUse&) = delete;
        PreparedUse(PreparedUse&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)) {}
    private:
        StableSlotStorage* owner_;
    };

    class Preparation {
    public:
        Preparation() = default;
        ~Preparation() { Release(); }
        Preparation(const Preparation&) = delete;
        Preparation& operator=(const Preparation&) = delete;
        Preparation(Preparation&& other) noexcept { *this = std::move(other); }
        Preparation& operator=(Preparation&& other) noexcept {
            if (this != &other) {
                Release();
                owner_ = std::exchange(other.owner_, nullptr);
                low_ = std::exchange(other.low_, 0u);
                high_ = std::exchange(other.high_, 0u);
            }
            return *this;
        }
        bool Valid() const noexcept { return owner_ != nullptr; }
        PreparedUse Use() const noexcept { return PreparedUse(owner_); }
        void Release() noexcept {
            if (owner_ == nullptr) return;
            assert(!memguard::IsAudioTagged() && owner_->heldLow_ >= low_ && owner_->heldHigh_ >= high_);
            owner_->heldLow_ -= low_;
            owner_->heldHigh_ -= high_;
            owner_ = nullptr;
            low_ = high_ = 0;
        }
    private:
        friend class StableSlotStorage;
        StableSlotStorage* owner_ = nullptr;
        std::uint32_t low_ = 0;
        std::uint32_t high_ = 0;
    };

    StableSlotStorage() = default;
    StableSlotStorage(const StableSlotStorage&) = delete;
    StableSlotStorage& operator=(const StableSlotStorage&) = delete;
    ~StableSlotStorage() {
        assert(heldLow_ == 0 && heldHigh_ == 0 && preparedUsers_ == 0);
        if (memguard::IsAudioTagged() && capacity_ != 0) std::terminate();
        for (std::uint32_t slot = capacity_; slot != 0; --slot)
            if (IsConstructed(slot - 1u)) std::destroy_at(AddressOf(slot - 1u));
    }

    bool Reserve(std::uint32_t capacity) noexcept {
        static_assert(std::is_nothrow_destructible_v<T>);
        if (memguard::IsAudioTagged() || capacity_ != 0 ||
            capacity > static_cast<std::size_t>(PTRDIFF_MAX) / sizeof(T)) return false;
        const auto words = capacity / 64u + (capacity % 64u != 0);
        std::unique_ptr<std::atomic<std::uint64_t>[]> directory;
        try { directory = std::make_unique<std::atomic<std::uint64_t>[]>(words); }
        catch (...) { return false; }
        for (std::uint32_t word = 0; word < words; ++word)
            directory[word].store(0, std::memory_order_relaxed);
        if (!reservation_.Reserve(static_cast<std::size_t>(capacity) * sizeof(T), alignof(T), true))
            return false;
        directory_ = std::move(directory);
        directoryWords_ = words;
        capacity_ = capacity;
        return true;
    }

    template<class... Args>
    bool Emplace(std::uint32_t slot, Args&&... args) noexcept {
        if (slot >= capacity_) return false;
        if (IsConstructed(slot)) return true;
        if (memguard::IsAudioTagged() || preparedUsers_ != 0 ||
            !reservation_.CommitRange(static_cast<std::size_t>(slot) * sizeof(T), sizeof(T))) return false;
        try { std::construct_at(AddressOf(slot), std::forward<Args>(args)...); }
        catch (...) { return false; }
        constructed_.fetch_add(1, std::memory_order_relaxed);
        directory_[slot / 64u].fetch_or(std::uint64_t{1} << (slot % 64u), std::memory_order_release);
        return true;
    }

    template<class Available, class Construct>
    bool PrepareBacking(std::uint32_t low, std::uint32_t high, Available&& available,
        Construct&& construct, Preparation* out) noexcept {
        if (out == nullptr || out->Valid() || memguard::IsAudioTagged() || preparedUsers_ != 0 ||
            heldLow_ > UINT64_MAX - low || heldHigh_ > UINT64_MAX - high) return false;
        if (!EnsureAvailable(heldLow_ + low, heldHigh_ + high, UINT32_MAX, available, construct)) return false;
        heldLow_ += low;
        heldHigh_ += high;
        out->owner_ = this;
        out->low_ = low;
        out->high_ = high;
        return true;
    }

    // Preserve outstanding physical allowances before the caller issues a real identity.
    template<class Available, class Construct>
    bool ProtectBackingForAcquire(std::uint32_t slot, Available&& available, Construct&& construct) noexcept {
        if (!IsConstructed(slot)) return false;
        return preparedUsers_ != 0 || EnsureAvailable(heldLow_, heldHigh_, slot, available, construct);
    }

    bool PreparedUseActive() const noexcept { return preparedUsers_ != 0; }
    bool IsConstructed(std::uint32_t slot) const noexcept {
        return slot < capacity_ && (directory_[slot / 64u].load(std::memory_order_acquire) &
            (std::uint64_t{1} << (slot % 64u))) != 0;
    }
    std::uint32_t size() const noexcept { return capacity_; }
    std::uint32_t ConstructedCount() const noexcept { return constructed_.load(std::memory_order_acquire); }
    std::size_t RequestedBytes() const noexcept { return static_cast<std::size_t>(ConstructedCount()) * sizeof(T); }
    std::size_t DirectoryBytes() const noexcept {
        return directoryWords_ * sizeof(std::atomic<std::uint64_t>) + reservation_.DirectoryBytes();
    }
    std::size_t CommittedBytes() const noexcept { return reservation_.CommittedBytes(); }
    std::size_t ReservedBytes() const noexcept { return reservation_.ReservedBytes(); }
    const void* BackingAddress() const noexcept { return reservation_.Address(); }
    void SetCommitPageLimitForTests(std::size_t pages) noexcept { reservation_.SetCommitPageLimitForTests(pages); }

    std::uint32_t IndexOf(const T& row) const noexcept {
        const auto base = reinterpret_cast<std::uintptr_t>(reservation_.Address());
        const auto address = reinterpret_cast<std::uintptr_t>(&row);
        if (address < base || (address - base) % sizeof(T) != 0) return UINT32_MAX;
        const auto index = (address - base) / sizeof(T);
        return index < capacity_ && IsConstructed(static_cast<std::uint32_t>(index))
            ? static_cast<std::uint32_t>(index) : UINT32_MAX;
    }
    T& operator[](std::uint32_t slot) noexcept {
        assert(IsConstructed(slot));
        return *std::launder(AddressOf(slot));
    }
    const T& operator[](std::uint32_t slot) const noexcept {
        assert(IsConstructed(slot));
        return *std::launder(AddressOf(slot));
    }

    template<bool Const>
    class Iterator {
        using Owner = std::conditional_t<Const, const StableSlotStorage, StableSlotStorage>;
    public:
        using value_type = T;
        using reference = std::conditional_t<Const, const T&, T&>;
        using pointer = std::conditional_t<Const, const T*, T*>;
        using difference_type = std::ptrdiff_t;
        using iterator_category = std::forward_iterator_tag;
        Iterator() = default;
        Iterator(Owner* owner, std::uint32_t slot) noexcept : owner_(owner), slot_(slot) { Skip(); }
        reference operator*() const noexcept { return (*owner_)[slot_]; }
        pointer operator->() const noexcept { return &**this; }
        Iterator& operator++() noexcept { ++slot_; Skip(); return *this; }
        Iterator operator++(int) noexcept { auto old = *this; ++*this; return old; }
        bool operator==(const Iterator& other) const noexcept { return owner_ == other.owner_ && slot_ == other.slot_; }
    private:
        void Skip() noexcept {
            while (slot_ < owner_->capacity_) {
                const auto word = slot_ / 64u;
                const auto bits = owner_->directory_[word].load(std::memory_order_acquire) &
                    (UINT64_MAX << (slot_ % 64u));
                if (bits != 0) {
                    slot_ = word * 64u + static_cast<std::uint32_t>(std::countr_zero(bits));
                    return;
                }
                if (word + 1u == owner_->directoryWords_) break;
                slot_ = (word + 1u) * 64u;
            }
            slot_ = owner_->capacity_;
        }
        Owner* owner_ = nullptr;
        std::uint32_t slot_ = 0;
    };
    Iterator<false> begin() noexcept { return {this, 0}; }
    Iterator<false> end() noexcept { return {this, capacity_}; }
    Iterator<true> begin() const noexcept { return {this, 0}; }
    Iterator<true> end() const noexcept { return {this, capacity_}; }

private:
    template<class Available, class Construct>
    bool EnsureAvailable(std::uint64_t low, std::uint64_t high, std::uint32_t excluded,
        Available& available, Construct& construct) noexcept {
        const auto ensure = [&](std::uint64_t count, bool reverse) noexcept {
            for (std::uint32_t offset = 0; offset < capacity_ && count != 0; ++offset) {
                const auto slot = reverse ? capacity_ - 1u - offset : offset;
                if (slot == excluded || (IsConstructed(slot) && !available((*this)[slot]))) continue;
                if (!IsConstructed(slot) && !construct(slot)) return false;
                --count;
            }
            // Physical backing does not grant more logical capacity than the original allocator.
            return true;
        };
        return ensure(low, false) && ensure(high, true);
    }
    T* AddressOf(std::uint32_t slot) const noexcept {
        return reinterpret_cast<T*>(static_cast<std::byte*>(reservation_.Address()) +
            static_cast<std::size_t>(slot) * sizeof(T));
    }

    detail::StablePageReservation reservation_;
    std::unique_ptr<std::atomic<std::uint64_t>[]> directory_;
    std::size_t directoryWords_ = 0;
    std::uint32_t capacity_ = 0;
    std::atomic<std::uint32_t> constructed_{0};
    std::uint64_t heldLow_ = 0;
    std::uint64_t heldHigh_ = 0;
    std::uint32_t preparedUsers_ = 0;
};

} // namespace harken

#endif // HARKEN_CORE_STABLE_SLOT_STORAGE_H
