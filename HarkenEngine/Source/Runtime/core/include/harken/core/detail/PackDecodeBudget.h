#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace harken::detail {

struct PackDecodeCapacity final : std::bad_alloc {};

struct PackDecodeBudgetState {
    std::uint64_t limit;
    std::uint64_t live = 0;
    explicit PackDecodeBudgetState(std::uint64_t value) noexcept : limit(value) {}
};
using PackDecodeBudget = std::shared_ptr<PackDecodeBudgetState>;

inline std::uint64_t DecodeBytes(std::uint64_t count, std::uint64_t elementBytes) {
    if (elementBytes != 0 && count > UINT64_MAX / elementBytes) throw PackDecodeCapacity{};
    return count * elementBytes;
}

class PackDecodeCharge {
public:
    PackDecodeCharge() = default;
    explicit PackDecodeCharge(PackDecodeBudget budget) noexcept : budget_(std::move(budget)) {}
    PackDecodeCharge(const PackDecodeCharge& other) : budget_(other.budget_) { Add(other.bytes_); }
    PackDecodeCharge(PackDecodeCharge&& other) noexcept
        : budget_(std::move(other.budget_)), bytes_(std::exchange(other.bytes_, 0)) {}
    PackDecodeCharge& operator=(const PackDecodeCharge& other) {
        if (this != &other) { PackDecodeCharge copy(other); *this = std::move(copy); }
        return *this;
    }
    PackDecodeCharge& operator=(PackDecodeCharge&& other) noexcept {
        if (this != &other) {
            Reset();
            budget_ = std::move(other.budget_);
            bytes_ = std::exchange(other.bytes_, 0);
        }
        return *this;
    }
    ~PackDecodeCharge() { Reset(); }
    const PackDecodeBudget& Budget() const noexcept { return budget_; }
    std::uint64_t Bytes() const noexcept { return bytes_; }
    void Add(std::uint64_t bytes) {
        if (bytes > UINT64_MAX - bytes_ ||
            (budget_ && (budget_->live > budget_->limit || bytes > budget_->limit - budget_->live)))
            throw PackDecodeCapacity{};
        if (budget_) budget_->live += bytes;
        bytes_ += bytes;
    }
    template<class T> void AddRows(std::uint64_t count) { Add(DecodeBytes(count, sizeof(T))); }
    void Release(std::uint64_t bytes) noexcept {
        if (bytes > bytes_) std::terminate();
        if (budget_) budget_->live -= bytes;
        bytes_ -= bytes;
    }
    void Merge(PackDecodeCharge&& other) noexcept {
        if (budget_ != other.budget_ || other.bytes_ > UINT64_MAX - bytes_) std::terminate();
        bytes_ += std::exchange(other.bytes_, 0);
    }
    void Reset() noexcept { Release(bytes_); }
    void Detach() noexcept {
        if (budget_) budget_->live -= bytes_;
        budget_.reset();
    }

private:
    PackDecodeBudget budget_;
    std::uint64_t bytes_ = 0;
};

template<class T>
void DecodeReserve(std::vector<T>& values, std::size_t count, PackDecodeCharge& owner) {
    if (count <= values.capacity()) return;
    PackDecodeCharge next(owner.Budget());
    next.AddRows<T>(count);
    values.reserve(count);
    owner.Merge(std::move(next));
}

template<class T>
void DecodeResize(std::vector<T>& values, std::size_t count, PackDecodeCharge& owner) {
    DecodeReserve(values, count, owner);
    values.resize(count);
}

template<class T, class U>
void DecodeAppend(std::vector<T>& values, U&& value, PackDecodeCharge& owner,
                  std::size_t& requestedSlots) {
    if (values.size() == values.max_size()) throw PackDecodeCapacity{};
    if (values.size() >= requestedSlots) {
        const auto next = requestedSlots <= values.max_size() / 2
            ? std::max<std::size_t>(1, requestedSlots * 2) : values.max_size();
        if (next <= values.capacity()) {
            owner.AddRows<T>(next - requestedSlots);
        } else {
            PackDecodeCharge replacement(owner.Budget());
            replacement.AddRows<T>(next);
            values.reserve(next);
            owner.Release(DecodeBytes(requestedSlots, sizeof(T)));
            owner.Merge(std::move(replacement));
        }
        requestedSlots = next;
    }
    values.push_back(std::forward<U>(value));
}

} // namespace harken::detail
