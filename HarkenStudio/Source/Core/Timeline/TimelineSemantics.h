#pragma once

#include "TimelineTypes.h"

#include <cstdint>
#include <vector>

namespace harken::studio {

struct TimelineSpan
{
    int64_t start = 0;
    int64_t end = 0;

    bool operator==(const TimelineSpan&) const = default;
};

struct TimelineSemanticsResult
{
    TimelineValidation validation;
    TimelineSpan content;
    TimelineSpan nominal;

    bool known() const { return validation.ok(); }
    explicit operator bool() const { return known(); }

    static TimelineSemanticsResult unknown(TimelineError error,
                                           const QString& message);
};

namespace TimelineSemantics {

TimelineSemanticsResult contentBounds(const ClipLedger& clips);
std::vector<TimelineSpan> overlapRegions(const ClipLedger& clips);
TimelineSemanticsResult trackNominalSpan(const ProjectItem& track,
                                        const TimelineItemResolver& resolve);
TimelineSemanticsResult segmentNominalSpan(const ProjectItem& segment,
                                        const TimelineItemResolver& resolve);
QString formatConciseNumber(double value);
QString formatReadableCoordinate(int64_t coordinate);
QString formatNominalSummary(const TimelineSemanticsResult& semantics);
TimelineValidation validateProjectTimelineIdentities(
    const std::vector<const ProjectItem*>& roots);

}
}
