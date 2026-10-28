#include "TimelineSemantics.h"

#include "TimelineCoordinates.h"
#include "../Components/VAudio.h"
#include "../Components/VSegment.h"
#include "../Components/VSelection.h"
#include "../Components/VTrack.h"
#include "../ItemTypeCatalog.h"
#include "../ProjectTypes.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <limits>
#include <vector>

namespace VNX {
namespace {

bool checkedDistance(int64_t high, int64_t low, int64_t* out)
{
    if (!out || high < low) return false;
    if (low < 0 && high > std::numeric_limits<int64_t>::max() + low)
        return false;
    *out = high - low;
    return true;
}

bool checkedAdd(int64_t left, int64_t right, int64_t* out)
{
    if (!out) return false;
    if (right > 0 && left > std::numeric_limits<int64_t>::max() - right)
        return false;
    if (right < 0 && left < std::numeric_limits<int64_t>::min() - right)
        return false;
    *out = left + right;
    return true;
}

bool checkedSubtract(int64_t left, int64_t right, int64_t* out)
{
    if (right == std::numeric_limits<int64_t>::min()) {
        if (left >= 0) return false;
        *out = left - right;
        return true;
    }
    return checkedAdd(left, -right, out);
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
    const TimelineValidation timeValidity = TimelineCoordinates::validate(
        timeline->segmentTime);
    if (!timeValidity) return timeValidity;
    const TimelineValidation markerValidity = timeline->markers.validate();
    if (!markerValidity) return markerValidity;
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
                                     const TimelineItemResolver& resolve,
                                     const SegmentTimeSpec& time)
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
        timeline->clips, time);
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
    const VSegment* timeline = segment.comp<VSegment>();
    TimelineSpan content;
    bool hasContent = false;
    for (const auto& child : segment.children) {
        const VTrack* trackTimeline = child->comp<VTrack>();
        const TimelineSemanticsResult track = TimelineSemantics::contentBounds(
            trackTimeline->clips, timeline->segmentTime);
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

    const Marker* entry = timeline->markers.find(MarkerKind::Entry);
    const Marker* exit = timeline->markers.find(MarkerKind::Exit);
    TimelineSemanticsResult result;
    result.time = timeline->segmentTime;
    result.content = content;
    result.nominal = {
        entry ? entry->position : content.start,
        exit ? exit->position : content.end
    };
    const TimelineValidation nominalValidity = validateSpan(
        result.nominal, TimelineError::MarkerOrder,
        QStringLiteral("Nominal start must not exceed nominal end"));
    if (!nominalValidity)
        return TimelineSemanticsResult::unknown(nominalValidity.error,
                                                nominalValidity.message);
    result.validation = TimelineValidation::success();
    if (content.start < result.nominal.start
        && !checkedDistance(result.nominal.start, content.start, &result.pickup))
        return TimelineSemanticsResult::unknown(
            TimelineError::CoordinateOverflow,
            QStringLiteral("Pickup duration overflows"));
    if (content.end > result.nominal.end
        && !checkedDistance(content.end, result.nominal.end, &result.tail))
        return TimelineSemanticsResult::unknown(
            TimelineError::CoordinateOverflow,
            QStringLiteral("Tail duration overflows"));
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

TimelineSemanticsResult contentBounds(const ClipLedger& clips,
                                      const SegmentTimeSpec& time)
{
    const TimelineValidation timeValidity = TimelineCoordinates::validate(time);
    if (!timeValidity)
        return TimelineSemanticsResult::unknown(timeValidity.error,
                                                timeValidity.message);
    const TimelineValidation validity = clips.validateStructure();
    if (!validity)
        return TimelineSemanticsResult::unknown(validity.error, validity.message);

    TimelineSemanticsResult result;
    result.validation = TimelineValidation::success();
    result.time = time;
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
                                        const TimelineItemResolver& resolve,
                                        const SegmentTimeSpec& time)
{
    return resolveTrack(track, resolve, time);
}

TimelineSemanticsResult segmentNominalSpan(const ProjectItem& segment,
                                        const TimelineItemResolver& resolve)
{
    return resolveSegment(segment, resolve);
}

TimelineSemanticsResult nominalSpanOf(const ProjectItem& item,
                                     const TimelineItemResolver& resolve,
                                     const TimelineNaturalDurationResolver& naturalDuration)
{
    if (item.type == ProjectItem::Type::Track)
        return resolveTrack(item, resolve, {});
    if (item.type == ProjectItem::Type::Segment)
        return resolveSegment(item, resolve);
    if (item.type == ProjectItem::Type::Sound && naturalDuration) {
        const std::optional<int64_t> duration = naturalDuration(item);
        if (!duration)
            return TimelineSemanticsResult::unknown(
                TimelineError::MissingTimeline,
                QStringLiteral("Sound natural duration is unavailable"));
        if (*duration > 0) {
            TimelineSemanticsResult result;
            result.validation = TimelineValidation::success();
            result.content = {0, *duration};
            result.nominal = result.content;
            return result;
        }
        return TimelineSemanticsResult::unknown(
            TimelineError::NonPositiveDuration,
            QStringLiteral("Sound natural duration must be positive"));
    }
    return TimelineSemanticsResult::unknown(
        TimelineError::InvalidTargetType,
        QStringLiteral("Nominal span is unavailable for this object"));
}

QueuePreviewResult queuePreview(const ProjectItem& queue,
                                const TimelineItemResolver& resolve,
                                bool spliceByMarker,
                                const TimelineNaturalDurationResolver& naturalDuration)
{
    QueuePreviewResult result;
    result.spliceByMarker = spliceByMarker;
    const VSelection* selection = queue.comp<VSelection>();
    if (queue.type != ProjectItem::Type::Container || !selection
        || !selection->isQueue()) {
        result.validation = TimelineValidation::failure(
            TimelineError::InvalidTargetType,
            QStringLiteral("Queue preview requires a Queue Container"));
        return result;
    }

    result.validation = TimelineValidation::success();
    int64_t cursor = 0;
    QString blockingReason;
    result.items.reserve(queue.children.size());
    for (const auto& child : queue.children) {
        QueuePreviewItem item;
        if (!child) {
            item.validation = TimelineValidation::failure(
                TimelineError::MissingTarget,
                QStringLiteral("Queue item does not exist"));
            blockingReason = item.validation.message;
            result.items.push_back(std::move(item));
            continue;
        }
        item.itemGuid = child->guid;
        if (const auto* audio = child->comp<VAudio>()) {
            item.loopCount = audio->loopCount;
            item.loopKind = audio->loopCount == 0 ? LoopPreviewKind::Infinite
                : audio->loopCount == 1 ? LoopPreviewKind::Off
                                        : LoopPreviewKind::Finite;
            item.stopCompletesInfiniteBody = audio->loopCount == 0;
        }
        if (!blockingReason.isEmpty()) {
            item.validation = TimelineValidation::failure(
                TimelineError::MissingTarget,
                QStringLiteral("Cannot position after unknown Queue item: %1")
                    .arg(blockingReason));
            result.items.push_back(std::move(item));
            continue;
        }
        TimelineSemanticsResult semantics = nominalSpanOf(
            *child, resolve, naturalDuration);
        if (!semantics) {
            item.validation = semantics.validation;
            blockingReason = item.validation.message;
            result.items.push_back(std::move(item));
            continue;
        }

        const CoordinateResult contentStart = TimelineCoordinates::toNanoseconds(
            semantics.content.start, semantics.time);
        const CoordinateResult contentEnd = TimelineCoordinates::toNanoseconds(
            semantics.content.end, semantics.time);
        const CoordinateResult nominalStart = TimelineCoordinates::toNanoseconds(
            semantics.nominal.start, semantics.time);
        const CoordinateResult nominalEnd = TimelineCoordinates::toNanoseconds(
            semantics.nominal.end, semantics.time);
        const CoordinateResult* coordinates[] = {
            &contentStart, &contentEnd, &nominalStart, &nominalEnd
        };
        for (const CoordinateResult* coordinate : coordinates) {
            if (!*coordinate) {
                item.validation = coordinate->validation;
                break;
            }
        }
        if (!item.validation.ok()) {
            blockingReason = item.validation.message;
            result.items.push_back(std::move(item));
            continue;
        }

        int64_t nominalDuration = 0;
        int64_t contentDuration = 0;
        int64_t projectedNominalEnd = 0;
        int64_t contentStartOffset = 0;
        int64_t contentEndOffset = 0;
        int64_t projectedContentStart = 0;
        int64_t projectedContentEnd = 0;
        if (!checkedDistance(nominalEnd.value, nominalStart.value, &nominalDuration)
            || !checkedDistance(contentEnd.value, contentStart.value, &contentDuration)
            || !checkedSubtract(contentStart.value, nominalStart.value,
                                &contentStartOffset)
            || !checkedSubtract(contentEnd.value, nominalStart.value,
                                &contentEndOffset)) {
            item.validation = TimelineValidation::failure(
                TimelineError::CoordinateOverflow,
                QStringLiteral("Queue preview projection overflows"));
            blockingReason = item.validation.message;
            result.items.push_back(std::move(item));
            continue;
        }

        if (spliceByMarker) {
            if (!checkedAdd(cursor, nominalDuration, &projectedNominalEnd)
                || !checkedAdd(cursor, contentStartOffset, &projectedContentStart)
                || !checkedAdd(cursor, contentEndOffset, &projectedContentEnd)) {
                item.validation = TimelineValidation::failure(
                    TimelineError::CoordinateOverflow,
                    QStringLiteral("Queue marker-splice projection overflows"));
            }
            item.anchorNanoseconds = cursor;
        } else {
            int64_t nominalStartOffset = 0;
            int64_t nominalEndOffset = 0;
            if (!checkedSubtract(nominalStart.value, contentStart.value,
                                &nominalStartOffset)
                || !checkedSubtract(nominalEnd.value, contentStart.value,
                                    &nominalEndOffset)
                || !checkedAdd(cursor, nominalStartOffset,
                               &item.anchorNanoseconds)
                || !checkedAdd(cursor, nominalEndOffset, &projectedNominalEnd)
                || !checkedAdd(cursor, contentDuration, &projectedContentEnd)) {
                item.validation = TimelineValidation::failure(
                    TimelineError::CoordinateOverflow,
                    QStringLiteral("Queue butt projection overflows"));
            }
            projectedContentStart = cursor;
        }
        if (!item.validation.ok()) {
            blockingReason = item.validation.message;
            result.items.push_back(std::move(item));
            continue;
        }

        item.validation = TimelineValidation::success();
        item.nominalNanoseconds = {item.anchorNanoseconds, projectedNominalEnd};
        item.contentNanoseconds = {projectedContentStart, projectedContentEnd};
        if (projectedContentStart < item.nominalNanoseconds.start)
            if (!checkedDistance(item.nominalNanoseconds.start, projectedContentStart,
                                 &item.pickupNanoseconds))
                item.validation = TimelineValidation::failure(
                    TimelineError::CoordinateOverflow,
                    QStringLiteral("Queue pickup duration overflows"));
        if (projectedContentEnd > item.nominalNanoseconds.end)
            if (!checkedDistance(projectedContentEnd, item.nominalNanoseconds.end,
                                 &item.tailNanoseconds))
                item.validation = TimelineValidation::failure(
                    TimelineError::CoordinateOverflow,
                    QStringLiteral("Queue tail duration overflows"));
        if (!item.validation) {
            blockingReason = item.validation.message;
            result.items.push_back(std::move(item));
            continue;
        }
        const VSegment* segmentTimeline = child->comp<VSegment>();
        item.entryAnchor = segmentTimeline
            && segmentTimeline->markers.find(MarkerKind::Entry) != nullptr;
        cursor = spliceByMarker ? projectedNominalEnd : projectedContentEnd;
        result.items.push_back(std::move(item));
    }
    result.endNanoseconds = cursor;
    return result;
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

QString formatReadableCoordinate(int64_t coordinate, const SegmentTimeSpec& time)
{
    if (time.domain == TimelineDomain::Musical) {
        const auto beats = TimelineCoordinates::ticksToBeats(coordinate);
        return beats ? QObject::tr("%1 beats").arg(formatConciseNumber(*beats))
                     : QObject::tr("Unknown");
    }
    const auto seconds = TimelineCoordinates::toSeconds(coordinate, time);
    return seconds ? QObject::tr("%1 s").arg(formatConciseNumber(*seconds))
                   : QObject::tr("Unknown");
}

QString formatNominalSummary(const TimelineSemanticsResult& semantics)
{
    if (!semantics)
        return QObject::tr("Nominal unknown · %1").arg(semantics.validation.message);
    return QObject::tr("Nominal %1 — %2 · Pickup %3 · Tail %4")
        .arg(formatReadableCoordinate(semantics.nominal.start, semantics.time),
             formatReadableCoordinate(semantics.nominal.end, semantics.time),
             formatReadableCoordinate(semantics.pickup, semantics.time),
             formatReadableCoordinate(semantics.tail, semantics.time));
}

QString formatQueueLoopBadge(const QueuePreviewItem& item)
{
    if (item.loopKind == LoopPreviewKind::Finite)
        return QObject::tr("×%1").arg(item.loopCount);
    if (item.loopKind == LoopPreviewKind::Infinite)
        return QStringLiteral("∞");
    return {};
}

QString formatQueueItemTooltip(const QString& name, const QueuePreviewItem& item)
{
    QStringList lines;
    lines << name;
    if (!item) {
        lines << QObject::tr("unknown — %1").arg(item.validation.message);
        lines << QObject::tr("excluded from time calculation");
        const QString badge = formatQueueLoopBadge(item);
        if (!badge.isEmpty())
            lines << badge;
        if (item.loopKind == LoopPreviewKind::Infinite)
            lines << QObject::tr("Stop completes the loop body and advances the parent Queue");
        return lines.join(QLatin1Char('\n'));
    }
    const SegmentTimeSpec absolute;
    lines << QObject::tr("%1 anchor %2 s")
                 .arg(item.entryAnchor ? QObject::tr("entry")
                                       : QObject::tr("nominal-start fallback"),
                      formatConciseNumber(
                          static_cast<double>(item.anchorNanoseconds)
                          / static_cast<double>(TimelineCoordinates::kNanosecondsPerSecond)));
    lines << QObject::tr("nominal [%1, %2] s")
                 .arg(formatReadableCoordinate(item.nominalNanoseconds.start, absolute),
                      formatReadableCoordinate(item.nominalNanoseconds.end, absolute));
    lines << QObject::tr("content [%1, %2] s")
                 .arg(formatReadableCoordinate(item.contentNanoseconds.start, absolute),
                      formatReadableCoordinate(item.contentNanoseconds.end, absolute));
    lines << QObject::tr("pickup %1 · tail %2")
                 .arg(formatReadableCoordinate(item.pickupNanoseconds, absolute),
                      formatReadableCoordinate(item.tailNanoseconds, absolute));
    const QString badge = formatQueueLoopBadge(item);
    if (!badge.isEmpty())
        lines << badge;
    if (item.loopKind == LoopPreviewKind::Infinite)
        lines << QObject::tr("Stop completes the loop body and advances the parent Queue");
    return lines.join(QLatin1Char('\n'));
}

bool hasMusicalClock(const ProjectItem& item)
{
    const VSegment* timeline = item.comp<VSegment>();
    return timeline && timeline->segmentTime.domain == TimelineDomain::Musical;
}

bool hasAnyMarker(const ProjectItem& item)
{
    const VSegment* timeline = item.comp<VSegment>();
    return timeline && !timeline->markers.empty();
}

int64_t coordinateUnitFor(const SegmentTimeSpec& spec)
{
    if (spec.domain != TimelineDomain::Musical)
        return TimelineCoordinates::kNanosecondsPerSecond;
    const int32_t denominator = spec.meterMap.empty()
        ? 4 : spec.meterMap.front().denominator;
    return (TimelineCoordinates::kPpq * 4) / denominator;
}

int64_t snapGridFor(const SegmentTimeSpec& spec)
{
    if (spec.domain == TimelineDomain::Musical)
        return std::max<int64_t>(1, coordinateUnitFor(spec));
    return TimelineCoordinates::kNanosecondsPerSecond;
}

bool hasMarkerKind(const ProjectItem& item, MarkerKind kind)
{
    const VSegment* timeline = item.comp<VSegment>();
    return timeline && timeline->markers.find(kind) != nullptr;
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
        if (const VSegment* segment = item->comp<VSegment>()) {
            const TimelineValidation validity = segment->markers.validate();
            if (!validity) return validity;
            for (const Marker& marker : segment->markers.items) {
                if (identities.contains(marker.markerGuid))
                    return TimelineValidation::failure(
                        TimelineError::DuplicateGuid,
                        QStringLiteral("Timeline entity GUIDs must be project-wide unique"));
                identities.insert(marker.markerGuid);
            }
        }
        for (const auto& child : item->children) stack.push_back(child.get());
    }
    return TimelineValidation::success();
}

}
}
