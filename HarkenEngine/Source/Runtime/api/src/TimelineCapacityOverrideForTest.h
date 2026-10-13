#pragma once

#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)

#include <cstdint>

namespace harken::detail {

struct TimelineCapacityOverrideForTest {
    std::uint32_t events = 0;
    std::uint32_t groups = 0;
    std::uint32_t frameBatches = 0;
    std::uint32_t originalConditionalRows = 0;
};

enum class TimelineCapacityOverrideReadForTest : std::uint8_t {
    None,
    CurrentThread,
    OtherThread,
};

TimelineCapacityOverrideReadForTest ReadTimelineCapacityOverrideForTest(
    TimelineCapacityOverrideForTest* out) noexcept;

class ScopedTimelineCapacityOverrideForTest final {
public:
    ScopedTimelineCapacityOverrideForTest(std::uint32_t events,
        std::uint32_t groups, std::uint32_t frameBatches,
        std::uint32_t originalConditionalRows = 0) noexcept;
    ~ScopedTimelineCapacityOverrideForTest() noexcept;
    ScopedTimelineCapacityOverrideForTest(
        const ScopedTimelineCapacityOverrideForTest&) = delete;
    ScopedTimelineCapacityOverrideForTest& operator=(
        const ScopedTimelineCapacityOverrideForTest&) = delete;
    ScopedTimelineCapacityOverrideForTest(
        ScopedTimelineCapacityOverrideForTest&&) = delete;
    ScopedTimelineCapacityOverrideForTest& operator=(
        ScopedTimelineCapacityOverrideForTest&&) = delete;

    bool Valid() const noexcept { return generation_ != 0; }

private:
    std::uint64_t generation_ = 0;
};

} // namespace harken::detail

#endif
