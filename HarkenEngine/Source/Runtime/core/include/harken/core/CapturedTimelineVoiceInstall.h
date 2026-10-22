#ifndef HARKEN_CORE_CAPTURED_TIMELINE_VOICE_INSTALL_H
#define HARKEN_CORE_CAPTURED_TIMELINE_VOICE_INSTALL_H

#include "harken/core/TimelineVoiceInstallInput.h"
#include "harken/core/AllocGuard.h"

#include <cassert>
#include <cstddef>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

namespace harken {

class CapturedTimelineVoiceInstallBuilder;

// The serialized control owner keeps this allocation through its native readers.
class CapturedTimelineVoiceInstall final {
public:
    CapturedTimelineVoiceInstall() noexcept = default;
    ~CapturedTimelineVoiceInstall() { Release(); }

    CapturedTimelineVoiceInstall(CapturedTimelineVoiceInstall&& other) noexcept
        : allocation_(std::exchange(other.allocation_, nullptr)) {}

    CapturedTimelineVoiceInstall& operator=(CapturedTimelineVoiceInstall&& other) noexcept {
        if (this != &other) {
            Release();
            allocation_ = std::exchange(other.allocation_, nullptr);
        }
        return *this;
    }

    CapturedTimelineVoiceInstall(const CapturedTimelineVoiceInstall&) = delete;
    CapturedTimelineVoiceInstall& operator=(const CapturedTimelineVoiceInstall&) = delete;

    bool HasStorage() const noexcept { return allocation_ != nullptr; }
    bool Valid() const noexcept { return HasStorage() && allocation_->populated; }

    const TimelineVoiceInstallFacts& Facts() const noexcept {
        static const TimelineVoiceInstallFacts empty{};
        return Valid() ? allocation_->facts : empty;
    }

    bool ClearForReuse() noexcept {
        assert(!memguard::IsAudioTagged());
        if (ReferenceCount() > 1u) return false;
        if (HasStorage()) allocation_->populated = false;
        return true;
    }

    CapturedTimelineVoiceInstall Share() const noexcept {
        assert(!memguard::IsAudioTagged());
        CapturedTimelineVoiceInstall result;
        if (!Valid()) return result;
        assert(allocation_->references < (std::numeric_limits<std::size_t>::max)());
        ++allocation_->references;
        result.allocation_ = allocation_;
        return result;
    }

    std::size_t ReferenceCount() const noexcept {
        return HasStorage() ? allocation_->references : 0u;
    }

    bool SharesStorage(const CapturedTimelineVoiceInstall& other) const noexcept {
        return allocation_ != nullptr && allocation_ == other.allocation_;
    }

    TimelineVoiceInstallInput Input() const & noexcept {
        if (!Valid()) return {};
        return {&allocation_->facts,
            Column<TimelineVoiceBindingInstall>(0), Column<PlaybackPathRow>(1),
            Column<TimelineVoiceSendInstall>(2), Column<TimelineVoiceEffectInstall>(3)};
    }

    TimelineVoiceInstallInput Input() const && = delete;

    std::size_t AllocationBytes() const noexcept {
        return HasStorage() ? allocation_->allocationBytes : 0;
    }

    std::size_t AllocationBytesForAccounting(std::uint64_t epoch) const noexcept {
        assert(!memguard::IsAudioTagged() && epoch != 0);
        if (!HasStorage() || allocation_->accountedEpoch == epoch) return 0;
        allocation_->accountedEpoch = epoch;
        return allocation_->allocationBytes;
    }

    static constexpr std::size_t AllocationHeaderBytes() noexcept { return sizeof(Header); }

private:
    struct ColumnRange {
        std::uint32_t offset = 0;
        std::uint32_t count = 0;
    };

    struct Header {
        TimelineVoiceInstallFacts facts{};
        ColumnRange columns[4]{};
        std::size_t allocationBytes = 0;
        std::size_t references = 1;
        std::uint64_t accountedEpoch = 0;
        bool populated = false;
    };

    static_assert(std::is_trivially_destructible_v<Header>);
    static_assert(alignof(Header) <= alignof(std::max_align_t));
    static_assert(std::is_trivially_destructible_v<TimelineVoiceBindingInstall>);
    static_assert(std::is_trivially_destructible_v<PlaybackPathRow>);
    static_assert(std::is_trivially_destructible_v<TimelineVoiceSendInstall>);
    static_assert(std::is_trivially_destructible_v<TimelineVoiceEffectInstall>);

    template<class T>
    std::span<const T> Column(std::size_t index) const noexcept {
        const auto& range = allocation_->columns[index];
        if (range.count == 0) return {};
        const auto* base = reinterpret_cast<const std::byte*>(allocation_);
        return {reinterpret_cast<const T*>(base + range.offset), range.count};
    }

    void Release() noexcept {
        if (allocation_ == nullptr) return;
        assert(!memguard::IsAudioTagged());
        auto* allocation = std::exchange(allocation_, nullptr);
        assert(allocation->references != 0);
        if (--allocation->references == 0) ::operator delete(allocation);
    }

    friend class CapturedTimelineVoiceInstallBuilder;
    Header* allocation_ = nullptr;
};

} // namespace harken

#endif // HARKEN_CORE_CAPTURED_TIMELINE_VOICE_INSTALL_H
