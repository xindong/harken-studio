#include "TimelineEditing.h"

#include "TimelineCoordinates.h"
#include "../AuthoringPolicy.h"
#include "../Components/VPiece.h"
#include "../Components/VSelection.h"
#include "../Components/VMixNode.h"
#include "../Components/VTrack.h"
#include "../ProjectManager.h"

#include <QHash>
#include <QSet>

#include <algorithm>
#include <limits>

namespace VNX {

uint32_t itemShortID(const ProjectItem* item);

namespace TimelineEditing {
namespace {

ActionResult failure(const QString& code, const QString& message)
{
    return {false, {}, code, message};
}

QJsonObject propertyResult(const ProjectItem* item,
                           const QString& property,
                           const QJsonValue& value)
{
    return {
        {QStringLiteral("name"), item->name},
        {QStringLiteral("object_id"), static_cast<qint64>(itemShortID(item))},
        {QStringLiteral("authoring_handle"), static_cast<qint64>(item->objectID)},
        {QStringLiteral("property"), property},
        {QStringLiteral("value"), value}
    };
}

std::vector<ProjectItem*> projectRoots(ProjectData* project)
{
    std::vector<ProjectItem*> roots;
    if (!project) return roots;
    for (ProjectFileBinding* binding : project->allFileBindings())
        if (binding && binding->rootItem)
            roots.push_back(binding->rootItem.get());
    return roots;
}

TimelineValidation validateEntityIdentities(ProjectData* project,
                                            const ProjectItem* replacedOwner,
                                            const ClipLedger* replacementClips,
                                            const MarkerLedger* replacementMarkers)
{
    QSet<QUuid> identities;
    std::vector<ProjectItem*> stack = projectRoots(project);
    while (!stack.empty()) {
        ProjectItem* item = stack.back();
        stack.pop_back();
        if (!item) continue;
        auto insert = [&identities](const QUuid& guid) -> TimelineValidation {
            if (guid.isNull())
                return TimelineValidation::failure(
                    TimelineError::EmptyGuid,
                    QStringLiteral("Timeline entity GUID must not be empty"));
            if (identities.contains(guid))
                return TimelineValidation::failure(
                    TimelineError::DuplicateGuid,
                    QStringLiteral("Timeline entity GUIDs must be project-wide unique"));
            identities.insert(guid);
            return TimelineValidation::success();
        };
        if (const TimelineValidation validity = insert(item->guid); !validity)
            return validity;
        if (const auto* mix = item->comp<VMixNode>()) {
            for (const auto& binding : mix->bindings.entries())
                if (const auto validity = insert(binding.guid); !validity) return validity;
            for (const auto& effect : mix->effects.entries())
                if (const auto validity = insert(effect.guid); !validity) return validity;
        }
        if (const auto* timeline = item->comp<VTrack>()) {
            const ClipLedger& ledger = item == replacedOwner && replacementClips
                ? *replacementClips : timeline->clips;
            for (const Clip& clip : ledger.items)
                if (const TimelineValidation validity = insert(clip.clipGuid); !validity)
                    return validity;
        }
        if (const auto* timeline = item->comp<VPiece>()) {
            const MarkerLedger& ledger = item == replacedOwner && replacementMarkers
                ? *replacementMarkers : timeline->markers;
            for (const Marker& marker : ledger.items)
                if (const TimelineValidation validity = insert(marker.markerGuid); !validity)
                    return validity;
        }
        for (auto& child : item->children) stack.push_back(child.get());
    }
    return TimelineValidation::success();
}

void applyClips(const QUuid& trackGuid,
                const ClipLedger& clips,
                const QObject* originator = nullptr)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* track = manager.findItemByGuid(trackGuid);
    auto* timeline = track ? track->comp<VTrack>() : nullptr;
    if (!timeline) return;
    timeline->clips = clips;
    track->markDirty();
    emit manager.itemPropertyChanged(track, QStringLiteral("clips"), originator);
}

void applyMarkers(const QUuid& pieceGuid,
               const MarkerLedger& markers,
               const QObject* originator = nullptr)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* piece = manager.findItemByGuid(pieceGuid);
    auto* timeline = piece ? piece->comp<VPiece>() : nullptr;
    if (!timeline) return;
    timeline->markers = markers;
    piece->markDirty();
    emit manager.itemPropertyChanged(piece, QStringLiteral("markers"), originator);
}

int clipMergeId(const QUuid& clipGuid)
{
    const uint value = qHash(clipGuid) & 0x3fffffffU;
    return static_cast<int>(value == 0 ? 1 : value);
}

struct PieceTimeSnapshot
{
    PieceTimeSpec pieceTime;
    MarkerLedger markers;
    QHash<QUuid, ClipLedger> tracks;
};

PieceTimeSnapshot capturePieceTime(ProjectItem* piece)
{
    PieceTimeSnapshot snapshot;
    const auto* timeline = piece->comp<VPiece>();
    snapshot.pieceTime = timeline->pieceTime;
    snapshot.markers = timeline->markers;
    for (const auto& child : piece->children)
        if (const auto* track = child->comp<VTrack>())
            snapshot.tracks.insert(child->guid, track->clips);
    return snapshot;
}

bool pieceReaches(ProjectManager* projectManager,
                  const ProjectItem* piece,
                  const ProjectItem* target,
                  const ProjectItem* replacedTrack,
                  const ClipLedger& replacement)
{
    if (!piece || !target || piece->type != ProjectItem::Type::Piece)
        return false;
    QSet<QUuid> visited;
    std::vector<const ProjectItem*> stack{piece};
    while (!stack.empty()) {
        const ProjectItem* current = stack.back();
        stack.pop_back();
        if (current == target)
            return true;
        if (!current || visited.contains(current->guid))
            continue;
        visited.insert(current->guid);
        for (const auto& child : current->children) {
            if (!child || child->type != ProjectItem::Type::Track)
                continue;
            const auto* timeline = child->comp<VTrack>();
            if (!timeline)
                continue;
            const ClipLedger& clips = child.get() == replacedTrack
                ? replacement : timeline->clips;
            for (const Clip& clip : clips.items) {
                const ProjectItem* referenced = projectManager->findItemByGuid(clip.targetGuid);
                if (referenced && referenced->type == ProjectItem::Type::Piece)
                    stack.push_back(referenced);
            }
        }
    }
    return false;
}

void applyPieceTime(const QUuid& pieceGuid,
                    const PieceTimeSnapshot& snapshot,
                    bool coordinatesChanged,
                    const QObject* originator = nullptr)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* piece = manager.findItemByGuid(pieceGuid);
    auto* timeline = piece ? piece->comp<VPiece>() : nullptr;
    if (!timeline) return;
    timeline->pieceTime = snapshot.pieceTime;
    timeline->markers = snapshot.markers;
    piece->markDirty();
    emit manager.itemPropertyChanged(piece, QStringLiteral("pieceTime"), originator);
    if (coordinatesChanged)
        emit manager.itemPropertyChanged(piece, QStringLiteral("markers"), originator);
    if (coordinatesChanged)
        for (auto it = snapshot.tracks.cbegin(); it != snapshot.tracks.cend(); ++it)
            applyClips(it.key(), it.value(), originator);
}

}

PieceTimeSpec timeSpecForTrackParent(const ProjectItem* parent)
{
    if (parent && parent->type == ProjectItem::Type::Piece)
        if (const auto* timeline = parent->comp<VPiece>())
            return timeline->pieceTime;
    return {};
}

std::optional<ClipLedger> convertClipLedger(const ClipLedger& source,
                                            const PieceTimeSpec& sourceTime,
                                            const PieceTimeSpec& destinationTime,
                                            QString* error)
{
    const TimelineValidation sourceValidity = source.validateStructure();
    if (!sourceValidity) {
        if (error) *error = sourceValidity.message;
        return std::nullopt;
    }
    if (sourceTime == destinationTime) return source;
    ClipLedger converted = source;
    for (Clip& clip : converted.items) {
        if (clip.start > std::numeric_limits<int64_t>::max() - clip.duration) {
            if (error) *error = QStringLiteral("Clip end overflows the timeline coordinate");
            return std::nullopt;
        }
        const CoordinateResult start = TimelineCoordinates::convert(
            clip.start, sourceTime, destinationTime);
        const CoordinateResult end = TimelineCoordinates::convert(
            clip.start + clip.duration, sourceTime, destinationTime);
        if (!start || !end) {
            if (error) *error = !start ? start.validation.message : end.validation.message;
            return std::nullopt;
        }
        if (end.value <= start.value
            || static_cast<uint64_t>(end.value) - static_cast<uint64_t>(start.value)
                > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            if (error) *error = QStringLiteral("Converted clip duration must be positive");
            return std::nullopt;
        }
        clip.start = start.value;
        clip.duration = end.value - start.value;
    }
    const TimelineValidation validity = converted.validateStructure();
    if (!validity) {
        if (error) *error = validity.message;
        return std::nullopt;
    }
    return converted;
}

TimelineValidation validateClipPlacement(ProjectManager* projectManager,
                                         ProjectItem* track,
                                         const ClipLedger& clips)
{
    if (!projectManager || !track || track->type != ProjectItem::Type::Track)
        return TimelineValidation::failure(
            TimelineError::MissingTimeline,
            QStringLiteral("Clip placement requires a Track timeline"));
    const TimelineValidation targets = clips.validateTargets(
        *track, [projectManager](const QUuid& guid) {
            return projectManager->findItemByGuid(guid);
        });
    if (!targets)
        return targets;
    ProjectItem* hostPiece = track->parent && track->parent->type == ProjectItem::Type::Piece
        ? track->parent : nullptr;
    if (!hostPiece)
        return TimelineValidation::success();
    for (const Clip& clip : clips.items) {
        ProjectItem* target = projectManager->findItemByGuid(clip.targetGuid);
        if (!target || target->type != ProjectItem::Type::Piece)
            continue;
        if (pieceReaches(projectManager, target, hostPiece, track, clips))
            return TimelineValidation::failure(
                TimelineError::Cycle,
                QStringLiteral("Clip placement would create a Piece reference cycle"));
    }
    return TimelineValidation::success();
}

ActionResult replaceClips(ProjectManager* projectManager,
                          ProjectItem* track,
                          const ClipLedger& clips,
                          const QObject* originator,
                          const QString& undoText)
{
    auto* timeline = track ? track->comp<VTrack>() : nullptr;
    if (!projectManager || !timeline)
        return failure(QStringLiteral("INVALID_TIMELINE_OWNER"),
                       QStringLiteral("clips requires a Track timeline"));
    ProjectData* project = projectManager->currentProject();
    if (!project)
        return failure(QStringLiteral("NO_PROJECT_OPEN"),
                       QStringLiteral("No project is currently open"));
    const auto resolve = [projectManager](const QUuid& guid) {
        return projectManager->findItemByGuid(guid);
    };
    const auto authoring = AuthoringPolicy::validateTimeline(*track, resolve);
    if (!authoring)
        return failure(QStringLiteral("AUTHORING_RESTRICTED"), authoring.message);
    for (const Clip& clip : clips.items) {
        if (const ProjectItem* target = resolve(clip.targetGuid)) {
            const auto targetAuthoring = AuthoringPolicy::validateNewSubtree(
                *target, nullptr, resolve);
            if (!targetAuthoring)
                return failure(QStringLiteral("AUTHORING_RESTRICTED"), targetAuthoring.message);
        }
    }
    if (clips.occurrenceOrdinalHighWater
        < timeline->clips.occurrenceOrdinalHighWater)
        return failure(QStringLiteral("INVALID_ARGS"),
                       QStringLiteral("Clip occurrence high-water must not decrease"));
    const TimelineValidation placement = validateClipPlacement(
        projectManager, track, clips);
    if (!placement)
        return failure(QStringLiteral("INVALID_ARGS"), placement.message);
    const TimelineValidation identities = validateEntityIdentities(
        project, track, &clips, nullptr);
    if (!identities)
        return failure(QStringLiteral("INVALID_ARGS"), identities.message);

    ClipLedger normalized = clips;
    const ClipLedger oldClips = timeline->clips;
    ActionResult result;
    result.success = true;
    result.data = propertyResult(
        track, QStringLiteral("clips"), normalized.toJsonObject());
    if (oldClips == normalized) return result;

    const QUuid trackGuid = track->guid;
    applyClips(trackGuid, normalized, originator);
    result.undoFn = [trackGuid, oldClips] { applyClips(trackGuid, oldClips); };
    result.redoFn = [trackGuid, normalized] { applyClips(trackGuid, normalized); };
    result.undoText = undoText.isEmpty() ? QStringLiteral("Set clips") : undoText;
    result.authoringWriteSet = QSet<QUuid>{trackGuid};
    return result;
}

ActionResult addClip(ProjectManager* projectManager,
                     ProjectItem* track,
                     Clip clip,
                     const QObject* originator)
{
    auto* timeline = track ? track->comp<VTrack>() : nullptr;
    if (!timeline)
        return failure(QStringLiteral("INVALID_TIMELINE_OWNER"),
                       QStringLiteral("Clip creation requires a Track timeline"));
    if (clip.clipGuid.isNull()) clip.clipGuid = QUuid::createUuid();
    ClipLedger clips = timeline->clips;
    if (!clips.mintOccurrenceOrdinal(&clip))
        return failure(QStringLiteral("OCCURRENCE_ORDINAL_OVERFLOW"),
                       QStringLiteral("Track occurrence ordinal space is exhausted"));
    clips.items.push_back(std::move(clip));
    return replaceClips(projectManager, track, clips, originator,
                        QStringLiteral("Add clip"));
}

ActionResult removeClip(ProjectManager* projectManager,
                        ProjectItem* track,
                        const QUuid& clipGuid,
                        const QObject* originator)
{
    auto* timeline = track ? track->comp<VTrack>() : nullptr;
    if (!timeline)
        return failure(QStringLiteral("INVALID_TIMELINE_OWNER"),
                       QStringLiteral("Clip deletion requires a Track timeline"));
    ClipLedger clips = timeline->clips;
    const auto it = std::find_if(clips.items.begin(), clips.items.end(),
                                 [&clipGuid](const Clip& clip) {
                                     return clip.clipGuid == clipGuid;
                                 });
    if (it == clips.items.end())
        return failure(QStringLiteral("CLIP_NOT_FOUND"),
                       QStringLiteral("Clip does not exist"));
    clips.items.erase(it);
    return replaceClips(projectManager, track, clips, originator,
                        QStringLiteral("Delete clip"));
}

ActionResult moveClip(ProjectManager* projectManager,
                      ProjectItem* track,
                      const QUuid& clipGuid,
                      int64_t start,
                      const QObject* originator)
{
    auto* timeline = track ? track->comp<VTrack>() : nullptr;
    if (!timeline)
        return failure(QStringLiteral("INVALID_TIMELINE_OWNER"),
                       QStringLiteral("Clip movement requires a Track timeline"));
    ClipLedger clips = timeline->clips;
    const auto it = std::find_if(clips.items.begin(), clips.items.end(),
                                 [&clipGuid](const Clip& clip) {
                                     return clip.clipGuid == clipGuid;
                                 });
    if (it == clips.items.end())
        return failure(QStringLiteral("CLIP_NOT_FOUND"),
                       QStringLiteral("Clip does not exist"));
    it->start = start;
    ActionResult result = replaceClips(projectManager, track, clips, originator,
                                       QStringLiteral("Move clip"));
    if (result.success) result.mergeId = clipMergeId(clipGuid);
    return result;
}

ActionResult resizeClip(ProjectManager* projectManager,
                        ProjectItem* track,
                        const QUuid& clipGuid,
                        int64_t start,
                        int64_t duration,
                        const QObject* originator)
{
    auto* timeline = track ? track->comp<VTrack>() : nullptr;
    if (!timeline)
        return failure(QStringLiteral("INVALID_TIMELINE_OWNER"),
                       QStringLiteral("Clip resize requires a Track timeline"));
    ClipLedger clips = timeline->clips;
    const auto it = std::find_if(clips.items.begin(), clips.items.end(),
                                 [&clipGuid](const Clip& clip) {
                                     return clip.clipGuid == clipGuid;
                                 });
    if (it == clips.items.end())
        return failure(QStringLiteral("CLIP_NOT_FOUND"),
                       QStringLiteral("Clip does not exist"));
    it->start = start;
    it->duration = duration;
    ActionResult result = replaceClips(projectManager, track, clips, originator,
                                       QStringLiteral("Resize clip"));
    if (result.success) result.mergeId = clipMergeId(clipGuid);
    return result;
}

ActionResult moveClipAcrossTracks(ProjectManager* projectManager,
                                  ProjectItem* sourceTrack,
                                  ProjectItem* destinationTrack,
                                  const QUuid& clipGuid,
                                  int64_t start,
                                  const QObject* originator)
{
    auto* sourceTimeline = sourceTrack ? sourceTrack->comp<VTrack>() : nullptr;
    auto* destinationTimeline = destinationTrack
        ? destinationTrack->comp<VTrack>() : nullptr;
    if (!projectManager || !sourceTimeline || !destinationTimeline
        || sourceTrack == destinationTrack)
        return failure(QStringLiteral("INVALID_TIMELINE_OWNER"),
                       QStringLiteral("Cross-track movement requires two distinct Tracks"));
    const auto resolve = [projectManager](const QUuid& guid) {
        return projectManager->findItemByGuid(guid);
    };
    for (const ProjectItem* track : {sourceTrack, destinationTrack}) {
        const auto authoring = AuthoringPolicy::validateNewSubtree(
            *track, track->parent, resolve);
        if (!authoring)
            return failure(QStringLiteral("AUTHORING_RESTRICTED"), authoring.message);
    }
    ClipLedger source = sourceTimeline->clips;
    ClipLedger destination = destinationTimeline->clips;
    const auto it = std::find_if(source.items.begin(), source.items.end(),
                                 [&clipGuid](const Clip& clip) {
                                     return clip.clipGuid == clipGuid;
                                 });
    if (it == source.items.end())
        return failure(QStringLiteral("CLIP_NOT_FOUND"),
                       QStringLiteral("Clip does not exist"));
    Clip moved = *it;
    moved.start = start;
    source.items.erase(it);
    if (!destination.mintOccurrenceOrdinal(&moved))
        return failure(QStringLiteral("OCCURRENCE_ORDINAL_OVERFLOW"),
                       QStringLiteral("Track occurrence ordinal space is exhausted"));
    destination.items.push_back(moved);
    const TimelineValidation sourceValidity = validateClipPlacement(
        projectManager, sourceTrack, source);
    const TimelineValidation destinationValidity = validateClipPlacement(
        projectManager, destinationTrack, destination);
    if (!sourceValidity || !destinationValidity)
        return failure(QStringLiteral("INVALID_ARGS"),
                       !sourceValidity ? sourceValidity.message : destinationValidity.message);

    const ClipLedger oldSource = sourceTimeline->clips;
    const ClipLedger oldDestination = destinationTimeline->clips;
    const QUuid sourceGuid = sourceTrack->guid;
    const QUuid destinationGuid = destinationTrack->guid;
    applyClips(sourceGuid, source, originator);
    applyClips(destinationGuid, destination, originator);

    ActionResult result;
    result.success = true;
    result.data = {
        {QStringLiteral("clipGuid"), clipGuid.toString(QUuid::WithoutBraces)},
        {QStringLiteral("source"), sourceGuid.toString(QUuid::WithoutBraces)},
        {QStringLiteral("destination"), destinationGuid.toString(QUuid::WithoutBraces)}
    };
    result.undoFn = [sourceGuid, destinationGuid, oldSource, oldDestination] {
        applyClips(sourceGuid, oldSource);
        applyClips(destinationGuid, oldDestination);
    };
    result.redoFn = [sourceGuid, destinationGuid, source, destination] {
        applyClips(sourceGuid, source);
        applyClips(destinationGuid, destination);
    };
    result.undoText = QStringLiteral("Move clip across tracks");
    result.mergeId = clipMergeId(clipGuid);
    result.authoringWriteSet = QSet<QUuid>{sourceGuid, destinationGuid};
    return result;
}

ActionResult replaceMarkers(ProjectManager* projectManager,
                         ProjectItem* piece,
                         const MarkerLedger& markers,
                         const QObject* originator,
                         const QString& undoText)
{
    auto* timeline = piece ? piece->comp<VPiece>() : nullptr;
    if (!projectManager || !timeline)
        return failure(QStringLiteral("INVALID_TIMELINE_OWNER"),
                       QStringLiteral("markers requires a Piece timeline"));
    ProjectData* project = projectManager ? projectManager->currentProject() : nullptr;
    if (!project)
        return failure(QStringLiteral("NO_PROJECT_OPEN"),
                       QStringLiteral("No project is currently open"));
    const auto authoring = AuthoringPolicy::validateTimeline(*piece,
        [projectManager](const QUuid& guid) { return projectManager->findItemByGuid(guid); });
    if (!authoring)
        return failure(QStringLiteral("AUTHORING_RESTRICTED"), authoring.message);
    const TimelineValidation validity = markers.validate();
    if (!validity)
        return failure(QStringLiteral("INVALID_ARGS"), validity.message);
    const TimelineValidation identities = validateEntityIdentities(
        project, piece, nullptr, &markers);
    if (!identities)
        return failure(QStringLiteral("INVALID_ARGS"), identities.message);

    MarkerLedger normalized = markers;
    const MarkerLedger oldMarkers = timeline->markers;
    ActionResult result;
    result.success = true;
    result.data = propertyResult(piece, QStringLiteral("markers"), normalized.toJson());
    if (oldMarkers == normalized) return result;
    const QUuid pieceGuid = piece->guid;
    applyMarkers(pieceGuid, normalized, originator);
    result.undoFn = [pieceGuid, oldMarkers] { applyMarkers(pieceGuid, oldMarkers); };
    result.redoFn = [pieceGuid, normalized] { applyMarkers(pieceGuid, normalized); };
    result.undoText = undoText.isEmpty() ? QStringLiteral("Set markers") : undoText;
    result.authoringWriteSet = QSet<QUuid>{pieceGuid};
    return result;
}

ActionResult addMarker(ProjectManager* projectManager,
                    ProjectItem* piece,
                    Marker marker,
                    const QObject* originator)
{
    auto* timeline = piece ? piece->comp<VPiece>() : nullptr;
    if (!timeline)
        return failure(QStringLiteral("INVALID_TIMELINE_OWNER"),
                       QStringLiteral("Marker creation requires a Piece timeline"));
    if (marker.markerGuid.isNull()) marker.markerGuid = QUuid::createUuid();
    MarkerLedger markers = timeline->markers;
    markers.items.push_back(std::move(marker));
    return replaceMarkers(projectManager, piece, markers, originator,
                       QStringLiteral("Add marker"));
}

ActionResult updateMarker(ProjectManager* projectManager,
                       ProjectItem* piece,
                       const Marker& marker,
                       const QObject* originator)
{
    auto* timeline = piece ? piece->comp<VPiece>() : nullptr;
    if (!timeline)
        return failure(QStringLiteral("INVALID_TIMELINE_OWNER"),
                       QStringLiteral("Marker update requires a Piece timeline"));
    MarkerLedger markers = timeline->markers;
    const auto it = std::find_if(markers.items.begin(), markers.items.end(),
                                 [&marker](const Marker& current) {
                                     return current.markerGuid == marker.markerGuid;
                                 });
    if (it == markers.items.end())
        return failure(QStringLiteral("MARKER_NOT_FOUND"),
                       QStringLiteral("Marker does not exist"));
    *it = marker;
    return replaceMarkers(projectManager, piece, markers, originator,
                       QStringLiteral("Edit marker"));
}

ActionResult removeMarker(ProjectManager* projectManager,
                       ProjectItem* piece,
                       const QUuid& markerGuid,
                       const QObject* originator)
{
    auto* timeline = piece ? piece->comp<VPiece>() : nullptr;
    if (!timeline)
        return failure(QStringLiteral("INVALID_TIMELINE_OWNER"),
                       QStringLiteral("Marker deletion requires a Piece timeline"));
    MarkerLedger markers = timeline->markers;
    const auto it = std::find_if(markers.items.begin(), markers.items.end(),
                                 [&markerGuid](const Marker& marker) {
                                     return marker.markerGuid == markerGuid;
                                 });
    if (it == markers.items.end())
        return failure(QStringLiteral("MARKER_NOT_FOUND"),
                       QStringLiteral("Marker does not exist"));
    markers.items.erase(it);
    return replaceMarkers(projectManager, piece, markers, originator,
                       QStringLiteral("Delete marker"));
}

ActionResult replacePieceTime(ProjectManager* projectManager,
                              ProjectItem* piece,
                              const PieceTimeSpec& pieceTime,
                              const QObject* originator,
                              const QString& undoText)
{
    auto* timeline = piece ? piece->comp<VPiece>() : nullptr;
    if (!projectManager || !timeline)
        return failure(QStringLiteral("INVALID_TIMELINE_OWNER"),
                       QStringLiteral("pieceTime requires a Piece timeline"));
    const PieceTimeSpec* previousTime = &timeline->pieceTime;
    for (const PieceTimeSpec* time : {previousTime, &pieceTime}) {
        const auto authoring = AuthoringPolicy::validatePieceTime(*time);
        if (!authoring)
            return failure(QStringLiteral("AUTHORING_RESTRICTED"), authoring.message);
    }
    const TimelineValidation validity = TimelineCoordinates::validate(pieceTime);
    if (!validity)
        return failure(QStringLiteral("INVALID_ARGS"), validity.message);

    const PieceTimeSnapshot oldSnapshot = capturePieceTime(piece);
    PieceTimeSnapshot newSnapshot = oldSnapshot;
    newSnapshot.pieceTime = pieceTime;
    const bool coordinatesChanged = oldSnapshot.pieceTime.domain != pieceTime.domain;
    if (coordinatesChanged) {
        for (Marker& marker : newSnapshot.markers.items) {
            const CoordinateResult converted = TimelineCoordinates::convert(
                marker.position, oldSnapshot.pieceTime, pieceTime);
            if (!converted)
                return failure(QStringLiteral("INVALID_ARGS"), converted.validation.message);
            marker.position = converted.value;
        }
        for (auto it = newSnapshot.tracks.begin(); it != newSnapshot.tracks.end(); ++it) {
            QString error;
            auto converted = convertClipLedger(
                it.value(), oldSnapshot.pieceTime, pieceTime, &error);
            if (!converted)
                return failure(QStringLiteral("INVALID_ARGS"), error);
            it.value() = std::move(*converted);
        }
        const TimelineValidation markerValidity = newSnapshot.markers.validate();
        if (!markerValidity)
            return failure(QStringLiteral("INVALID_ARGS"), markerValidity.message);
    }

    ActionResult result;
    result.success = true;
    result.data = propertyResult(piece, QStringLiteral("pieceTime"), pieceTime.toJson());
    if (oldSnapshot.pieceTime == pieceTime) return result;
    const QUuid pieceGuid = piece->guid;
    applyPieceTime(pieceGuid, newSnapshot, coordinatesChanged, originator);
    result.undoFn = [pieceGuid, oldSnapshot, coordinatesChanged] {
        applyPieceTime(pieceGuid, oldSnapshot, coordinatesChanged);
    };
    result.redoFn = [pieceGuid, newSnapshot, coordinatesChanged] {
        applyPieceTime(pieceGuid, newSnapshot, coordinatesChanged);
    };
    result.undoText = undoText.isEmpty()
        ? QStringLiteral("Set piece time") : undoText;
    result.authoringWriteSet = QSet<QUuid>{pieceGuid};
    if (coordinatesChanged)
        for (auto it = newSnapshot.tracks.cbegin(); it != newSnapshot.tracks.cend(); ++it)
            result.authoringWriteSet->insert(it.key());
    return result;
}

ActionResult setSpliceByMarker(ProjectManager* projectManager,
                            ProjectItem* container,
                            bool spliceByMarker,
                            const QObject* originator,
                            const QString& undoText)
{
    auto* selection = container ? container->comp<VSelection>() : nullptr;
    if (!projectManager || !selection || container->type != ProjectItem::Type::Container)
        return failure(QStringLiteral("INVALID_TIMELINE_OWNER"),
                       QStringLiteral("spliceByMarker requires a Container selection"));
    if (!selection->isQueue())
        return failure(QStringLiteral("INVALID_TIMELINE_OWNER"),
                       QStringLiteral("spliceByMarker requires a Queue Container"));
    const bool oldValue = selection->spliceByMarker;
    ActionResult result;
    result.success = true;
    result.data = propertyResult(container, QStringLiteral("spliceByMarker"), spliceByMarker);
    if (oldValue == spliceByMarker) return result;

    const QUuid containerGuid = container->guid;
    auto apply = [](const QUuid& guid, bool value, const QObject* source = nullptr) {
        auto& manager = ProjectManager::instance();
        ProjectItem* item = manager.findItemByGuid(guid);
        auto* component = item ? item->comp<VSelection>() : nullptr;
        if (!component) return;
        component->spliceByMarker = value;
        item->markDirty();
        emit manager.itemPropertyChanged(item, QStringLiteral("spliceByMarker"), source);
    };
    apply(containerGuid, spliceByMarker, originator);
    result.undoFn = [apply, containerGuid, oldValue] { apply(containerGuid, oldValue); };
    result.redoFn = [apply, containerGuid, spliceByMarker] {
        apply(containerGuid, spliceByMarker);
    };
    result.undoText = undoText.isEmpty()
        ? QStringLiteral("Set splice by marker") : undoText;
    result.authoringWriteSet = QSet<QUuid>{containerGuid};
    return result;
}

}

}
