#ifndef HARKEN_CORE_CAPTURED_TIMELINE_VOICE_INSTALL_H
#define HARKEN_CORE_CAPTURED_TIMELINE_VOICE_INSTALL_H

#include "harken/core/TimelineVoiceInstallInput.h"
#include "harken/core/AllocGuard.h"

#include <cassert>
#include <cstddef>
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

    bool Valid() const noexcept { return allocation_ != nullptr; }

    TimelineVoiceInstallInput Input() const & noexcept {
        if (!Valid()) return {};
        return {&allocation_->facts,
            Column<TimelineVoiceBindingInstall>(0), Column<PlaybackPathRow>(1),
            Column<TimelineVoiceSendInstall>(2), Column<TimelineVoiceEffectInstall>(3)};
    }

    TimelineVoiceInstallInput Input() const && = delete;

    std::size_t AllocationBytes() const noexcept {
        if (!Valid()) return 0;
        const auto& last = allocation_->columns[3];
        return last.offset + static_cast<std::size_t>(last.count) *
            sizeof(TimelineVoiceEffectInstall);
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
        ::operator delete(allocation_);
        allocation_ = nullptr;
    }

    friend class CapturedTimelineVoiceInstallBuilder;
    Header* allocation_ = nullptr;
};

} // namespace harken

#endif // HARKEN_CORE_CAPTURED_TIMELINE_VOICE_INSTALL_H
