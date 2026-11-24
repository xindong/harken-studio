#include "TimelineSemantics.h"

#include "TimelineCoordinates.h"
#include "../Components/VSegment.h"
#include "../Components/VTrack.h"
#include "../ItemTypeCatalog.h"
#include "../ProjectTypes.h"

#include <QHash>
#include <QObject>
#include <QSet>

#include <algorithm>
#include <limits>
#include <vector>

namespace harken::studio {
namespace {

bool checkedDistance(int64_t high, int64_t low, int64_t* out)
{
    if (!out || high < low) return false;
    if (low < 0 && high > std::numeric_limits<int64_t>::max() + low)
        return false;
    *out = high - low;
    return true;
}

TimelineValidation validateSpan(const TimelineSpan& span,
                                TimelineError orderError,
                                const QString& orderMessage)
{
    if (span.start > span.end)
        return TimelineValidation::failure(orderError, orderMessage);
    int64_t duration = 0;
    if (!checkedDistance(span.end, span.start, &duration))
        return TimelineValidation::failure(
            TimelineError::CoordinateOverflow,
            QStringLiteral("Timeline span duration overflows"));
    return TimelineValidation::success();
}

TimelineValidation inspectTrack(const ProjectItem& track,
                                const TimelineItemResolver& resolve,
                                std::vector<const ProjectItem*>* segmentTargets)
{
    if (track.type != ProjectItem::Type::Track)
        return TimelineValidation::failure(
            TimelineError::InvalidTargetType,
            QStringLiteral("Segment children must be Track items"));
    const VTrack* timeline = track.comp<VTrack>();
    if (!timeline)
        return TimelineValidation::failure(
            TimelineError::MissingTimeline,
            QStringLiteral("Track has no timeline component"));
    const TimelineValidation structure = timeline->clips.validateStructure();
    if (!structure) return structure;
    for (const Clip& clip : timeline->clips.items) {
        const ProjectItem* target = resolve ? resolve(clip.targetGuid) : nullptr;
        if (!target)
            return TimelineValidation::failure(
                TimelineError::MissingTarget,
                QStringLiteral("Clip target does not exist"));
        if (!ItemTypeCatalog::isTimelineClipTarget(target->type))
            return TimelineValidation::failure(
                TimelineError::InvalidTargetType,
                QStringLiteral("Clip target must be Sound, Container, or Segment"));
        if (segmentTargets && target->type == ProjectItem::Type::Segment)
            segmentTargets->push_back(target);
    }
    return TimelineValidation::success();
}

TimelineValidation inspectSegment(const ProjectItem& segment,
                                const TimelineItemResolver& resolve,
                                std::vector<const ProjectItem*>* segmentTargets)
{
    if (segment.type != ProjectItem::Type::Segment)
        return TimelineValidation::failure(
            TimelineError::InvalidTargetType,
            QStringLiteral("Segment semantics require a Segment item"));
    const VSegment* timeline = segment.comp<VSegment>();
    if (!timeline)
        return TimelineValidation::failure(
            TimelineError::MissingTimeline,
            QStringLiteral("Segment has no timeline component"));
    for (const auto& child : segment.children) {
        if (!child)
            return TimelineValidation::failure(
                TimelineError::InvalidTargetType,
                QStringLiteral("Segment contains an invalid child"));
        const TimelineValidation trackValidity = inspectTrack(
            *child, resolve, segmentTargets);
        if (!trackValidity) return trackValidity;
    }
    return TimelineValidation::success();
}

TimelineValidation validateSegmentGraph(
    const std::vector<const ProjectItem*>& roots,
    const TimelineItemResolver& resolve)
{
    struct Frame
    {
        const ProjectItem* segment = nullptr;
        std::vector<const ProjectItem*> targets;
        size_t next = 0;
    };

    QHash<const ProjectItem*, uint8_t> state;
    std::vector<Frame> stack;
    for (const ProjectItem* root : roots) {
        if (!root) continue;
        const uint8_t rootState = state.value(root, 0);
        if (rootState == 2) continue;
        if (rootState == 1)
            return TimelineValidation::failure(
                TimelineError::Cycle,
                QStringLiteral("Segment reference cycle detected"));
        Frame frame;
        frame.segment = root;
        const TimelineValidation validity = inspectSegment(
            *root, resolve, &frame.targets);
        if (!validity) return validity;
        state.insert(root, 1);
        stack.push_back(std::move(frame));
        while (!stack.empty()) {
            Frame& current = stack.back();
            if (current.next == current.targets.size()) {
                state.insert(current.segment, 2);
                stack.pop_back();
                continue;
            }
            const ProjectItem* target = current.targets[current.next++];
            const uint8_t targetState = state.value(target, 0);
            if (targetState == 1)
                return TimelineValidation::failure(
                    TimelineError::Cycle,
                    QStringLiteral("Segment reference cycle detected"));
            if (targetState == 2) continue;
            Frame child;
            child.segment = target;
            const TimelineValidation validity = inspectSegment(
                *target, resolve, &child.targets);
            if (!validity) return validity;
            state.insert(target, 1);
            stack.push_back(std::move(child));
        }
    }
    return TimelineValidation::success();
}

TimelineSemanticsResult resolveTrack(const ProjectItem& track,
                                     const TimelineItemResolver& resolve)
{
    std::vector<const ProjectItem*> segmentTargets;
    const TimelineValidation validity = inspectTrack(
        track, resolve, &segmentTargets);
    if (!validity)
        return TimelineSemanticsResult::unknown(validity.error, validity.message);
    const TimelineValidation graphValidity = validateSegmentGraph(
        segmentTargets, resolve);
    if (!graphValidity)
        return TimelineSemanticsResult::unknown(graphValidity.error,
                                                graphValidity.message);
    const VTrack* timeline = track.comp<VTrack>();
    TimelineSemanticsResult result = TimelineSemantics::contentBounds(
        timeline->clips);
    if (!result) return result;
    result.nominal = {0, std::max<int64_t>(0, result.content.end)};
    return result;
}

TimelineSemanticsResult resolveSegment(const ProjectItem& segment,
                                     const TimelineItemResolver& resolve)
{
    const TimelineValidation graphValidity = validateSegmentGraph({&segment}, resolve);
    if (!graphValidity)
        return TimelineSemanticsResult::unknown(graphValidity.error,
                                                graphValidity.message);
    TimelineSpan content;
    bool hasContent = false;
    for (const auto& child : segment.children) {
        const VTrack* trackTimeline = child->comp<VTrack>();
        const TimelineSemanticsResult track = TimelineSemantics::contentBounds(
            trackTimeline->clips);
        if (!track) return track;
        if (trackTimeline->clips.empty()) continue;
        if (!hasContent) {
            content = track.content;
            hasContent = true;
        } else {
            content.start = std::min(content.start, track.content.start);
            content.end = std::max(content.end, track.content.end);
        }
    }
    if (!hasContent) content = {0, 0};
    const TimelineValidation contentValidity = validateSpan(
        content, TimelineError::CoordinateOverflow,
        QStringLiteral("Content span is inverted"));
    if (!contentValidity)
        return TimelineSemanticsResult::unknown(contentValidity.error,
                                                contentValidity.message);

    TimelineSemanticsResult result;
    result.validation = TimelineValidation::success();
    result.content = content;
    result.nominal = content;
    return result;
}

}

TimelineSemanticsResult TimelineSemanticsResult::unknown(TimelineError error,
                                                         const QString& message)
{
    TimelineSemanticsResult result;
    result.validation = TimelineValidation::failure(error, message);
    return result;
}

namespace TimelineSemantics {

TimelineSemanticsResult contentBounds(const ClipLedger& clips)
{
    const TimelineValidation validity = clips.validateStructure();
    if (!validity)
        return TimelineSemanticsResult::unknown(validity.error, validity.message);

    TimelineSemanticsResult result;
    result.validation = TimelineValidation::success();
    if (clips.empty()) return result;
    result.content.start = clips.items.front().start;
    result.content.end = clips.items.front().start + clips.items.front().duration;
    for (const Clip& clip : clips.items) {
        result.content.start = std::min(result.content.start, clip.start);
        result.content.end = std::max(result.content.end, clip.start + clip.duration);
    }
    const TimelineValidation spanValidity = validateSpan(
        result.content, TimelineError::CoordinateOverflow,
        QStringLiteral("Content span is inverted"));
    if (!spanValidity)
        return TimelineSemanticsResult::unknown(spanValidity.error,
                                                spanValidity.message);
    result.nominal = result.content;
    return result;
}

std::vector<TimelineSpan> overlapRegions(const ClipLedger& clips)
{
    if (!clips.validateStructure()) return {};

    struct Event
    {
        int64_t position = 0;
        int delta = 0;
    };

    std::vector<Event> events;
    events.reserve(clips.items.size() * 2);
    for (const Clip& clip : clips.items) {
        events.push_back({clip.start, 1});
        events.push_back({clip.start + clip.duration, -1});
    }
    std::sort(events.begin(), events.end(), [](const Event& left, const Event& right) {
        return left.position < right.position;
    });

    std::vector<TimelineSpan> result;
    int coverage = 0;
    size_t next = 0;
    while (next < events.size()) {
        const int64_t position = events[next].position;
        int delta = 0;
        while (next < events.size() && events[next].position == position) {
            delta += events[next].delta;
            ++next;
        }
        coverage += delta;
        if (next == events.size()) break;
        const int64_t end = events[next].position;
        if (coverage < 2 || position == end) continue;
        if (!result.empty() && result.back().end == position)
            result.back().end = end;
        else
            result.push_back({position, end});
    }
    return result;
}

TimelineSemanticsResult trackNominalSpan(const ProjectItem& track,
                                        const TimelineItemResolver& resolve)
{
    return resolveTrack(track, resolve);
}

TimelineSemanticsResult segmentNominalSpan(const ProjectItem& segment,
                                        const TimelineItemResolver& resolve)
{
    return resolveSegment(segment, resolve);
}

QString formatConciseNumber(double value)
{
    QString text = QString::number(value, 'f', 3);
    while (text.contains(QLatin1Char('.')) && text.endsWith(QLatin1Char('0')))
        text.chop(1);
    if (text.endsWith(QLatin1Char('.')))
        text.chop(1);
    return text;
}

QString formatReadableCoordinate(int64_t coordinate)
{
    return QObject::tr("%1 s").arg(formatConciseNumber(TimelineCoordinates::toSeconds(coordinate)));
}

QString formatNominalSummary(const TimelineSemanticsResult& semantics)
{
    if (!semantics)
        return QObject::tr("Nominal unknown · %1").arg(semantics.validation.message);
    return QObject::tr("Nominal %1 — %2")
        .arg(formatReadableCoordinate(semantics.nominal.start),
             formatReadableCoordinate(semantics.nominal.end));
}

TimelineValidation validateProjectTimelineIdentities(
    const std::vector<const ProjectItem*>& roots)
{
    QSet<const ProjectItem*> visited;
    QSet<QUuid> identities;
    std::vector<const ProjectItem*> stack(roots.begin(), roots.end());
    while (!stack.empty()) {
        const ProjectItem* item = stack.back();
        stack.pop_back();
        if (!item || visited.contains(item)) continue;
        visited.insert(item);
        if (item->guid.isNull())
            return TimelineValidation::failure(
                TimelineError::EmptyGuid,
                QStringLiteral("Project item GUID must not be empty"));
        if (identities.contains(item->guid))
            return TimelineValidation::failure(
                TimelineError::DuplicateGuid,
                QStringLiteral("Timeline entity GUIDs must be project-wide unique"));
        identities.insert(item->guid);
        if (const VTrack* track = item->comp<VTrack>()) {
            const TimelineValidation validity = track->clips.validateStructure();
            if (!validity) return validity;
            for (const Clip& clip : track->clips.items) {
                if (identities.contains(clip.clipGuid))
                    return TimelineValidation::failure(
                        TimelineError::DuplicateGuid,
                        QStringLiteral("Timeline entity GUIDs must be project-wide unique"));
                identities.insert(clip.clipGuid);
            }
        }
        for (const auto& child : item->children) stack.push_back(child.get());
    }
    return TimelineValidation::success();
}

}
}
