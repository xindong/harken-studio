#ifndef HARKEN_CORE_TIMELINE_RUNTIME_INSTALL_H
#define HARKEN_CORE_TIMELINE_RUNTIME_INSTALL_H

#include "harken/core/TimelinePlanner.h"
#include "harken/core/TimelineProgram.h"

#include <cstddef>
#include <cstdint>

namespace harken {

enum class TimelineOccurrenceInstallKind : std::uint8_t {
    Audible = 0,
    LogicalOnly = 1,
};

struct TimelineOccurrenceInstallView {
    const ProgramView* schedule = nullptr;
    TimelineVoiceInstallDesc voice{};
    TimelineOccurrenceInstallKind kind =
        TimelineOccurrenceInstallKind::Audible;
};

struct TimelineProgramInstallView {
    const TimelineProgramView* program = nullptr;
    const TimelineOccurrenceInstallView* occurrences = nullptr;
    std::size_t occurrenceCount = 0;
};

enum class TimelineRuntimeInstallStatus : std::uint32_t {
    Installed = 0,
    NotInitialized = 1,
    InvalidProgram = 2,
    ProgramCapacity = 3,
    VoiceCapacity = 4,
    CommandCapacity = 5,
    RuntimeRecreateRequired = 6,
};

struct TimelineRuntimeInstallResult {
    TimelineRuntimeInstallStatus status =
        TimelineRuntimeInstallStatus::InvalidProgram;
    ProgramGeneration generation = 0;
    std::uint32_t occurrenceCount = 0;
};

TimelineRuntimeInstallResult InstallTimelineProgram(
    const TimelineProgramInstallView& program) noexcept;

} // namespace harken

#endif // HARKEN_CORE_TIMELINE_RUNTIME_INSTALL_H
