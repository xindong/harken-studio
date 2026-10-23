#ifndef HARKEN_ENGINE_NATIVE_VOICE_SIDE_H
#define HARKEN_ENGINE_NATIVE_VOICE_SIDE_H

#include "harken/core/CapturedTimelineVoiceInstall.h"

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

namespace harken {

// Owns memory only. Native rights and safe disposal remain with the caller.
template<class Start, class Context>
class EngineNativeVoiceSide {
public:
    struct CapturedInput {
        CapturedTimelineVoiceInstall voice;
        std::array<CapturedTimelineVoiceInstall*, 2> stagedReaders{};

        void Stage(CapturedTimelineVoiceInstall& target) noexcept {
            for (auto& reader : stagedReaders) {
                if (reader == &target || reader == nullptr) {
                    target = voice.Share();
                    reader = &target;
                    return;
                }
            }
            assert(false);
        }

        bool CanClear() const noexcept {
            std::size_t expected = 1u;
            for (const auto* reader : stagedReaders)
                if (reader != nullptr && reader->SharesStorage(voice)) ++expected;
            return voice.ReferenceCount() <= expected;
        }

        void ClearAfterSafeReset() noexcept {
            for (auto*& reader : stagedReaders) {
                if (reader != nullptr && reader->SharesStorage(voice)) *reader = {};
                reader = nullptr;
            }
            const bool cleared = voice.ClearForReuse();
            assert(cleared);
            (void)cleared;
        }
    };

    EngineNativeVoiceSide() noexcept = default;
    EngineNativeVoiceSide(const EngineNativeVoiceSide&) = delete;
    EngineNativeVoiceSide& operator=(const EngineNativeVoiceSide&) = delete;
    EngineNativeVoiceSide(EngineNativeVoiceSide&& other) noexcept { Swap(other); }
    EngineNativeVoiceSide& operator=(EngineNativeVoiceSide&& other) noexcept {
        if (this != &other) {
            EngineNativeVoiceSide incoming(std::move(other));
            Swap(incoming);
        }
        return *this;
    }

    template<class AllowAllocation>
    bool Construct(std::uint32_t capacity, std::uint32_t shape,
        AllowAllocation&& allowAllocation) noexcept {
        static_assert(std::is_nothrow_default_constructible_v<Start>);
        static_assert(std::is_nothrow_default_constructible_v<Context>);
        if (!Empty() || shape > capacity ||
            capacity > std::numeric_limits<std::size_t>::max() / sizeof(Start) ||
            capacity > std::numeric_limits<std::size_t>::max() / sizeof(Context)) return false;
        if (capacity == 0) return true;
        if (!allowAllocation()) return false;
        std::unique_ptr<Start[]> starts(new (std::nothrow) Start[capacity]{});
        if (starts == nullptr || !allowAllocation()) return false;
        std::unique_ptr<Context[]> contexts(new (std::nothrow) Context[capacity]{});
        if (contexts == nullptr) return false;
        starts_ = std::move(starts);
        contexts_ = std::move(contexts);
        capacity_ = capacity;
        shape_ = shape;
        return true;
    }

    bool Activate(std::uint32_t shape) noexcept {
        if (shape_ != 0 || shape > capacity_) return false;
        shape_ = shape;
        return true;
    }

    template<class AllowAllocation>
    bool ConstructCaptures(AllowAllocation&& allowAllocation) noexcept {
        if (captures_ != nullptr || capacity_ == 0) return true;
        if (capacity_ > std::numeric_limits<std::size_t>::max() /
                sizeof(CapturedInput) || !allowAllocation()) return false;
        captures_.reset(new (std::nothrow) CapturedInput[capacity_]{});
        return captures_ != nullptr;
    }

    bool CaptureReadersReturned() const noexcept {
        for (const auto& capture : Captures()) if (!capture.CanClear()) return false;
        return true;
    }

    void ClearAfterSafeReset() noexcept {
        for (std::uint32_t i = 0; i < capacity_; ++i) {
            std::destroy_at(&contexts_[i]);
            std::construct_at(&contexts_[i]);
            std::destroy_at(&starts_[i]);
            std::construct_at(&starts_[i]);
            if (captures_ != nullptr) captures_[i].ClearAfterSafeReset();
        }
        shape_ = 0;
    }

    bool Empty() const noexcept {
        return starts_ == nullptr && contexts_ == nullptr && captures_ == nullptr &&
            capacity_ == 0 && shape_ == 0;
    }
    bool empty() const noexcept { return capacity_ == 0; }
    std::uint32_t size() const noexcept { return capacity_; }
    std::uint32_t Shape() const noexcept { return shape_; }
    std::size_t RequestedBytes() const noexcept {
        auto bytes = static_cast<std::size_t>(capacity_) * (sizeof(Start) + sizeof(Context));
        if (captures_ != nullptr) bytes += static_cast<std::size_t>(capacity_) * sizeof(CapturedInput);
        return bytes;
    }
    Start* data() noexcept { return starts_.get(); }
    const Start* data() const noexcept { return starts_.get(); }
    Start* begin() noexcept { return data(); }
    const Start* begin() const noexcept { return data(); }
    Start* end() noexcept { return capacity_ == 0 ? data() : data() + capacity_; }
    const Start* end() const noexcept { return capacity_ == 0 ? data() : data() + capacity_; }
    Start& operator[](std::size_t index) noexcept { return starts_[index]; }
    const Start& operator[](std::size_t index) const noexcept { return starts_[index]; }
    std::span<Context> Owners() noexcept { return {contexts_.get(), capacity_}; }
    std::span<const Context> Owners() const noexcept { return {contexts_.get(), capacity_}; }
    std::span<CapturedInput> Captures() noexcept {
        return {captures_.get(), captures_ == nullptr ? 0u : capacity_};
    }
    std::span<const CapturedInput> Captures() const noexcept {
        return {captures_.get(), captures_ == nullptr ? 0u : capacity_};
    }

private:
    void Swap(EngineNativeVoiceSide& other) noexcept {
        starts_.swap(other.starts_);
        contexts_.swap(other.contexts_);
        captures_.swap(other.captures_);
        std::swap(capacity_, other.capacity_);
        std::swap(shape_, other.shape_);
    }
    std::unique_ptr<Start[]> starts_;
    std::unique_ptr<Context[]> contexts_;
    std::unique_ptr<CapturedInput[]> captures_;
    std::uint32_t capacity_ = 0;
    std::uint32_t shape_ = 0;
};

} // namespace harken

#endif
