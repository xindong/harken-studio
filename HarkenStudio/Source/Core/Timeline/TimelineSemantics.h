#pragma once

#include "TimelineTypes.h"

#include <cstdint>
#include <vector>

namespace VNX {

struct TimelineSpan
{
    int64_t start = 0;
    int64_t end = 0;

    bool operator==(const TimelineSpan&) const = default;
};

struct TimelineSemanticsResult
{
    TimelineValidation validation;
    SegmentTimeSpec time;
    TimelineSpan content;
    TimelineSpan nominal;
    int64_t pickup = 0;
    int64_t tail = 0;

    bool known() const { return validation.ok(); }
    explicit operator bool() const { return known(); }

    static TimelineSemanticsResult unknown(TimelineError error,
                                           const QString& message);
};

enum class LoopPreviewKind : uint8_t { Off = 0, Finite = 1, Infinite = 2 };

struct QueuePreviewItem
{
    QUuid itemGuid;
    TimelineValidation validation;
    TimelineSpan contentNanoseconds;
    TimelineSpan nominalNanoseconds;
    int64_t anchorNanoseconds = 0;
    int64_t pickupNanoseconds = 0;
    int64_t tailNanoseconds = 0;
    uint32_t loopCount = 1;
    LoopPreviewKind loopKind = LoopPreviewKind::Off;
    bool entryAnchor = false;
    bool stopCompletesInfiniteBody = false;

    bool known() const { return validation.ok(); }
    explicit operator bool() const { return known(); }
};

struct QueuePreviewResult
{
    TimelineValidation validation;
    std::vector<QueuePreviewItem> items;
    int64_t endNanoseconds = 0;
    bool spliceByMarker = false;

    bool valid() const { return validation.ok(); }
    explicit operator bool() const { return valid(); }
};

using TimelineNaturalDurationResolver =
    std::function<std::optional<int64_t>(const ProjectItem&)>;

namespace TimelineSemantics {

TimelineSemanticsResult contentBounds(const ClipLedger& clips,
                                      const SegmentTimeSpec& time = {});
std::vector<TimelineSpan> overlapRegions(const ClipLedger& clips);
TimelineSemanticsResult trackNominalSpan(const ProjectItem& track,
                                        const TimelineItemResolver& resolve,
                                        const SegmentTimeSpec& time = {});
TimelineSemanticsResult segmentNominalSpan(const ProjectItem& segment,
                                        const TimelineItemResolver& resolve);
TimelineSemanticsResult nominalSpanOf(const ProjectItem& item,
                                     const TimelineItemResolver& resolve,
                                     const TimelineNaturalDurationResolver& naturalDuration = {});
QueuePreviewResult queuePreview(const ProjectItem& queue,
                                const TimelineItemResolver& resolve,
                                bool spliceByMarker,
                                const TimelineNaturalDurationResolver& naturalDuration = {});
QString formatConciseNumber(double value);
QString formatReadableCoordinate(int64_t coordinate, const SegmentTimeSpec& time);
QString formatNominalSummary(const TimelineSemanticsResult& semantics);
QString formatQueueLoopBadge(const QueuePreviewItem& item);
QString formatQueueItemTooltip(const QString& name, const QueuePreviewItem& item);
TimelineValidation validateProjectTimelineIdentities(
    const std::vector<const ProjectItem*>& roots);

bool hasMusicalClock(const ProjectItem& item);
bool hasAnyMarker(const ProjectItem& item);
bool hasMarkerKind(const ProjectItem& item, MarkerKind kind);
int64_t coordinateUnitFor(const SegmentTimeSpec& spec);
int64_t snapGridFor(const SegmentTimeSpec& spec);

}
}
