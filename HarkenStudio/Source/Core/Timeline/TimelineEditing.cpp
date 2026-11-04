#include "TimelineEditing.h"

#include "TimelineCoordinates.h"
#include "../AuthoringPolicy.h"
#include "../Components/VSegment.h"
#include "../Components/VSelection.h"
#include "../Components/VMixNode.h"
#include "../Components/VTrack.h"
#include "../ProjectManager.h"
#include "../PhysicalFolderOperations.h"
#include "../UndoCommands.h"

#include <QHash>
#include <QSet>
#include <QSignalBlocker>

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
            QSet<QUuid> effectGuids;
            auto validity = TimelineValidation::success();
            const auto claimEffects = [&](const EffectChainLedger& effects, FieldGroupScope) {
                QSet<QUuid> chainGuids;
                for (const auto& effect : effects.entries()) {
                    if (!validity) return;
                    if (chainGuids.contains(effect.guid)) {
                        validity = TimelineValidation::failure(TimelineError::DuplicateGuid,
                            QStringLiteral("Effect slot GUIDs must be unique within a chain"));
                        return;
                    }
                    chainGuids.insert(effect.guid);
                    if (effectGuids.contains(effect.guid)) continue;
                    validity = insert(effect.guid);
                    effectGuids.insert(effect.guid);
                }
            };
            claimEffects(mix->effects, {});
            for (const auto& [platform, effects] : mix->effectPlatforms)
                claimEffects(effects, {});
            VisitAuthoredEffectChains(item->fieldGroups, claimEffects);
            if (!validity) return validity;
        }
        if (const auto* timeline = item->comp<VTrack>()) {
            const ClipLedger& ledger = item == replacedOwner && replacementClips
                ? *replacementClips : timeline->clips;
            for (const Clip& clip : ledger.items)
                if (const TimelineValidation validity = insert(clip.clipGuid); !validity)
                    return validity;
        }
        if (const auto* timeline = item->comp<VSegment>()) {
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

void applyMarkers(const QUuid& segmentGuid,
               const MarkerLedger& markers,
               const QObject* originator = nullptr)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* segment = manager.findItemByGuid(segmentGuid);
    auto* timeline = segment ? segment->comp<VSegment>() : nullptr;
    if (!timeline) return;
    timeline->markers = markers;
    segment->markDirty();
    emit manager.itemPropertyChanged(segment, QStringLiteral("markers"), originator);
}

int clipMergeId(const QUuid& clipGuid)
{
    const uint value = qHash(clipGuid) & 0x3fffffffU;
    return static_cast<int>(value == 0 ? 1 : value);
}

struct SegmentTimeSnapshot
{
    SegmentTimeSpec segmentTime;
    MarkerLedger markers;
    QHash<QUuid, ClipLedger> tracks;
};

SegmentTimeSnapshot captureSegmentTime(ProjectItem* segment)
{
    SegmentTimeSnapshot snapshot;
    const auto* timeline = segment->comp<VSegment>();
    snapshot.segmentTime = timeline->segmentTime;
    snapshot.markers = timeline->markers;
    for (const auto& child : segment->children)
        if (const auto* track = child->comp<VTrack>())
            snapshot.tracks.insert(child->guid, track->clips);
    return snapshot;
}

bool segmentReaches(ProjectManager* projectManager,
                  const ProjectItem* segment,
                  const ProjectItem* target,
                  const ProjectItem* replacedTrack,
                  const ClipLedger& replacement)
{
    if (!segment || !target || segment->type != ProjectItem::Type::Segment)
        return false;
    QSet<QUuid> visited;
    std::vector<const ProjectItem*> stack{segment};
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
                if (referenced && referenced->type == ProjectItem::Type::Segment)
                    stack.push_back(referenced);
            }
        }
    }
    return false;
}

void applySegmentTime(const QUuid& segmentGuid,
                    const SegmentTimeSnapshot& snapshot,
                    bool coordinatesChanged,
                    const QObject* originator = nullptr)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* segment = manager.findItemByGuid(segmentGuid);
    auto* timeline = segment ? segment->comp<VSegment>() : nullptr;
    if (!timeline) return;
    timeline->segmentTime = snapshot.segmentTime;
    timeline->markers = snapshot.markers;
    segment->markDirty();
    emit manager.itemPropertyChanged(segment, QStringLiteral("segmentTime"), originator);
    if (coordinatesChanged)
        emit manager.itemPropertyChanged(segment, QStringLiteral("markers"), originator);
    if (coordinatesChanged)
        for (auto it = snapshot.tracks.cbegin(); it != snapshot.tracks.cend(); ++it)
            applyClips(it.key(), it.value(), originator);
}

}

SegmentTimeSpec timeSpecForTrackParent(const ProjectItem* parent)
{
    if (parent && parent->type == ProjectItem::Type::Segment)
        if (const auto* timeline = parent->comp<VSegment>())
            return timeline->segmentTime;
    return {};
}

std::optional<ClipLedger> convertClipLedger(const ClipLedger& source,
                                            const SegmentTimeSpec& sourceTime,
                                            const SegmentTimeSpec& destinationTime,
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
    ProjectItem* hostSegment = track->parent && track->parent->type == ProjectItem::Type::Segment
        ? track->parent : nullptr;
    if (!hostSegment)
        return TimelineValidation::success();
    for (const Clip& clip : clips.items) {
        ProjectItem* target = projectManager->findItemByGuid(clip.targetGuid);
        if (!target || target->type != ProjectItem::Type::Segment)
            continue;
        if (segmentReaches(projectManager, target, hostSegment, track, clips))
            return TimelineValidation::failure(
                TimelineError::Cycle,
                QStringLiteral("Clip placement would create a Segment reference cycle"));
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

namespace {

struct ClipMoveTrackState {
    ProjectItem* track = nullptr;
    ClipLedger before;
    ClipLedger after;
};

struct ClipMoveTargetState {
    ProjectItem* target = nullptr;
    ProjectItem* beforeParent = nullptr;
    ProjectItem* afterParent = nullptr;
    bool cloned = false;
};

class ClipMoveCommand final : public TransactionalUndoCommand
{
public:
    ClipMoveCommand(ProjectData* project, std::vector<ClipMoveTrackState> tracks,
                    std::vector<ClipMoveTargetState> targets,
                    std::unique_ptr<TransactionalUndoCommand> hierarchy)
        : TransactionalUndoCommand(QStringLiteral("Move clips"), nullptr, false)
        , project_(project), tracks_(std::move(tracks)), targets_(std::move(targets))
        , hierarchy_(std::move(hierarchy))
    {}

private:
    bool apply(bool redo) override
    {
        auto& manager = ProjectManager::instance();
        if (blocked_ || manager.currentProject() != project_)
            return false;
        for (const auto& state : tracks_) {
            const auto* track = manager.findItemByGuid(state.track->guid);
            if (track != state.track || !track->comp<VTrack>()
                || track->comp<VTrack>()->clips != (redo ? state.before : state.after)) {
                emit manager.errorOccurred(QStringLiteral("The tracks changed after this clip move was prepared."));
                return false;
            }
        }

        QString error;
        {
            const QSignalBlocker blocker(&manager);
            const auto bindings = project_->documentBindings;
            QHash<ProjectItem*, bool> dirty;
            auto pending = projectRoots(project_);
            while (!pending.empty()) {
                auto* item = pending.back();
                pending.pop_back();
                dirty.insert(item, item->dirtyBit);
                for (const auto& child : item->children)
                    pending.push_back(child.get());
            }
            const auto install = [&](bool after) {
                for (const auto& state : tracks_)
                    applyClips(state.track->guid, after ? state.after : state.before);
            };
            const bool hierarchyApplied = !hierarchy_ || hierarchy_->tryReplay(redo);
            if (!hierarchyApplied) {
                error = QStringLiteral("The clip objects could not be moved to their destination tracks.");
            } else {
                install(redo);
                for (const auto& state : tracks_) {
                    const auto validity = validateClipPlacement(&manager, state.track,
                        state.track->comp<VTrack>()->clips);
                    if (!validity) {
                        error = validity.message;
                        break;
                    }
                }
                if (error.isEmpty()) {
                    const auto identities = validateEntityIdentities(project_, nullptr, nullptr, nullptr);
                    if (!identities) error = identities.message;
                }
            }
            if (!error.isEmpty()) {
                if (hierarchyApplied && hierarchy_ && !hierarchy_->tryReplay(!redo))
                    blocked_ = true;
                install(!redo);
                project_->documentBindings = bindings;
                for (auto it = dirty.cbegin(); it != dirty.cend(); ++it)
                    it.key()->dirtyBit = it.value();
            }
        }
        if (!error.isEmpty()) {
            emit manager.errorOccurred(error);
            return false;
        }
        for (const auto& target : targets_) {
            if (target.cloned) {
                if (redo) emit manager.itemCreated(target.target);
                else emit manager.itemDeleted(target.target);
            } else {
                emit manager.itemMoved(target.target,
                    redo ? target.beforeParent : target.afterParent,
                    redo ? target.afterParent : target.beforeParent);
            }
        }
        for (const auto& state : tracks_)
            emit manager.itemPropertyChanged(state.track, QStringLiteral("clips"));
        return true;
    }

    ProjectData* project_ = nullptr;
    std::vector<ClipMoveTrackState> tracks_;
    std::vector<ClipMoveTargetState> targets_;
    std::unique_ptr<TransactionalUndoCommand> hierarchy_;
    bool blocked_ = false;
};

}

std::unique_ptr<TransactionalUndoCommand> makeClipMoveCommand(
    ProjectManager* projectManager, const std::vector<ClipMove>& moves, QString* error)
{
    if (error) error->clear();
    const auto fail = [error](const QString& message) -> std::unique_ptr<TransactionalUndoCommand> {
        if (error) *error = message;
        return {};
    };
    auto* project = projectManager ? projectManager->currentProject() : nullptr;
    if (!project)
        return fail(QStringLiteral("No project is currently open."));
    std::vector<ClipMoveTrackState> tracks;
    QHash<QUuid, size_t> trackIndices;
    QSet<QUuid> requestedClips;
    const auto includeTrack = [&](ProjectItem* track) {
        if (trackIndices.contains(track->guid)) return;
        trackIndices.insert(track->guid, tracks.size());
        const auto& clips = track->comp<VTrack>()->clips;
        tracks.push_back({track, clips, clips});
    };
    const auto resolve = [projectManager](const QUuid& guid) {
        return projectManager->findItemByGuid(guid);
    };
    for (const auto& move : moves) {
        auto* source = resolve(move.sourceTrackGuid);
        auto* destination = resolve(move.destinationTrackGuid);
        if (!source || !destination || !source->comp<VTrack>() || !destination->comp<VTrack>())
            return fail(QStringLiteral("The clip or destination track is unavailable."));
        if (requestedClips.contains(move.clipGuid))
            return fail(QStringLiteral("A clip cannot be moved more than once in the same operation."));
        requestedClips.insert(move.clipGuid);
        includeTrack(source);
        includeTrack(destination);
    }
    for (const auto& state : tracks) {
        const auto authoring = AuthoringPolicy::validateNewSubtree(
            *state.track, state.track->parent, resolve);
        if (!authoring) return fail(authoring.message);
        const auto validity = validateClipPlacement(projectManager, state.track, state.before);
        if (!validity) return fail(validity.message);
    }

    struct PreparedMove {
        const ClipMove* move;
        Clip clip;
    };
    std::vector<PreparedMove> prepared;
    prepared.reserve(moves.size());
    for (const auto& move : moves) {
        const auto& source = tracks[trackIndices.value(move.sourceTrackGuid)];
        const auto found = std::find_if(source.before.items.begin(), source.before.items.end(),
            [&](const Clip& clip) { return clip.clipGuid == move.clipGuid; });
        if (found == source.before.items.end())
            return fail(QStringLiteral("The clip does not exist in its source track."));
        prepared.push_back({&move, *found});
    }
    std::sort(prepared.begin(), prepared.end(), [](const auto& left, const auto& right) {
        if (left.move->sourceTrackGuid != right.move->sourceTrackGuid)
            return left.move->sourceTrackGuid < right.move->sourceTrackGuid;
        return left.clip.stableOccurrenceOrdinal < right.clip.stableOccurrenceOrdinal;
    });

    QHash<QUuid, QSet<QUuid>> targetDestinations;
    for (const auto& entry : prepared) {
        const auto& move = *entry.move;
        auto& source = tracks[trackIndices.value(move.sourceTrackGuid)];
        auto& destination = tracks[trackIndices.value(move.destinationTrackGuid)];
        Clip moved = entry.clip;
        moved.start = move.start;
        auto existing = std::find_if(source.after.items.begin(), source.after.items.end(),
            [&](const Clip& clip) { return clip.clipGuid == move.clipGuid; });
        if (move.sourceTrackGuid == move.destinationTrackGuid) {
            existing->start = move.start;
        } else {
            source.after.items.erase(existing);
            if (!destination.after.mintOccurrenceOrdinal(&moved))
                return fail(QStringLiteral("Track occurrence ordinal space is exhausted."));
            destination.after.items.push_back(moved);
            targetDestinations[moved.targetGuid].insert(move.destinationTrackGuid);
        }
    }

    std::vector<ClipMoveTargetState> targets;
    std::vector<std::unique_ptr<TransactionalUndoCommand>> commands;
    std::vector<PhysicalItemMove> physicalMoves;
    QHash<QUuid, QSet<QString>> reservedNames;
    for (auto targetIt = targetDestinations.cbegin(); targetIt != targetDestinations.cend(); ++targetIt) {
        auto* target = resolve(targetIt.key());
        const auto& source = tracks[trackIndices.value(target->parent->guid)];
        const bool remains = std::any_of(source.after.items.begin(), source.after.items.end(),
            [&](const Clip& clip) { return clip.targetGuid == targetIt.key(); });
        for (const auto& destinationGuid : targetIt.value()) {
            auto* destination = resolve(destinationGuid);
            QString reason;
            if (remains || targetIt.value().size() > 1) {
                ProjectItem* clone = nullptr;
                auto command = projectManager->makeClipTargetCloneCommand(
                    target, destination, &clone, &reason);
                if (!command) return fail(reason);
                auto& names = reservedNames[destinationGuid];
                clone->name = projectManager->suggestUniqueName(target->type, target->name,
                    nullptr, names, destination);
                names.insert(clone->name);
                auto& clips = tracks[trackIndices.value(destinationGuid)].after;
                for (auto& clip : clips.items)
                    if (clip.targetGuid == targetIt.key()) clip.targetGuid = clone->guid;
                targets.push_back({clone, nullptr, destination, true});
                commands.push_back(std::move(command));
            } else {
                if (!projectManager->validateMoveConstraints(target, destination, &reason))
                    return fail(reason);
                targets.push_back({target, target->parent, destination, false});
                commands.push_back(std::make_unique<MoveItemCommand>(target, destination));
                physicalMoves.push_back({target, project->objectFolder(destination)});
            }
        }
    }
    for (const auto& state : tracks) {
        const auto validity = state.after.validateStructure();
        if (!validity) return fail(validity.message);
    }
    if (std::all_of(tracks.begin(), tracks.end(), [](const auto& state) {
            return state.before == state.after;
        })) return {};
    bool valid = false;
    auto hierarchy = makePhysicalMoveCommand(physicalMoves, {}, valid,
        makeMoveCommand(std::move(commands), QStringLiteral("Move clip objects")));
    if (!valid)
        return fail(QStringLiteral("The destination object folders are unavailable."));
    return std::make_unique<ClipMoveCommand>(project, std::move(tracks), std::move(targets),
        std::move(hierarchy));
}

ActionResult replaceMarkers(ProjectManager* projectManager,
                         ProjectItem* segment,
                         const MarkerLedger& markers,
                         const QObject* originator,
                         const QString& undoText)
{
    auto* timeline = segment ? segment->comp<VSegment>() : nullptr;
    if (!projectManager || !timeline)
        return failure(QStringLiteral("INVALID_TIMELINE_OWNER"),
                       QStringLiteral("markers requires a Segment timeline"));
    ProjectData* project = projectManager ? projectManager->currentProject() : nullptr;
    if (!project)
        return failure(QStringLiteral("NO_PROJECT_OPEN"),
                       QStringLiteral("No project is currently open"));
    const auto authoring = AuthoringPolicy::validateTimeline(*segment,
        [projectManager](const QUuid& guid) { return projectManager->findItemByGuid(guid); });
    if (!authoring)
        return failure(QStringLiteral("AUTHORING_RESTRICTED"), authoring.message);
    const TimelineValidation validity = markers.validate();
    if (!validity)
        return failure(QStringLiteral("INVALID_ARGS"), validity.message);
    const TimelineValidation identities = validateEntityIdentities(
        project, segment, nullptr, &markers);
    if (!identities)
        return failure(QStringLiteral("INVALID_ARGS"), identities.message);

    MarkerLedger normalized = markers;
    const MarkerLedger oldMarkers = timeline->markers;
    ActionResult result;
    result.success = true;
    result.data = propertyResult(segment, QStringLiteral("markers"), normalized.toJson());
    if (oldMarkers == normalized) return result;
    const QUuid segmentGuid = segment->guid;
    applyMarkers(segmentGuid, normalized, originator);
    result.undoFn = [segmentGuid, oldMarkers] { applyMarkers(segmentGuid, oldMarkers); };
    result.redoFn = [segmentGuid, normalized] { applyMarkers(segmentGuid, normalized); };
    result.undoText = undoText.isEmpty() ? QStringLiteral("Set markers") : undoText;
    result.authoringWriteSet = QSet<QUuid>{segmentGuid};
    return result;
}

ActionResult addMarker(ProjectManager* projectManager,
                    ProjectItem* segment,
                    Marker marker,
                    const QObject* originator)
{
    auto* timeline = segment ? segment->comp<VSegment>() : nullptr;
    if (!timeline)
        return failure(QStringLiteral("INVALID_TIMELINE_OWNER"),
                       QStringLiteral("Marker creation requires a Segment timeline"));
    if (marker.markerGuid.isNull()) marker.markerGuid = QUuid::createUuid();
    MarkerLedger markers = timeline->markers;
    markers.items.push_back(std::move(marker));
    return replaceMarkers(projectManager, segment, markers, originator,
                       QStringLiteral("Add marker"));
}

ActionResult updateMarker(ProjectManager* projectManager,
                       ProjectItem* segment,
                       const Marker& marker,
                       const QObject* originator)
{
    auto* timeline = segment ? segment->comp<VSegment>() : nullptr;
    if (!timeline)
        return failure(QStringLiteral("INVALID_TIMELINE_OWNER"),
                       QStringLiteral("Marker update requires a Segment timeline"));
    MarkerLedger markers = timeline->markers;
    const auto it = std::find_if(markers.items.begin(), markers.items.end(),
                                 [&marker](const Marker& current) {
                                     return current.markerGuid == marker.markerGuid;
                                 });
    if (it == markers.items.end())
        return failure(QStringLiteral("MARKER_NOT_FOUND"),
                       QStringLiteral("Marker does not exist"));
    *it = marker;
    return replaceMarkers(projectManager, segment, markers, originator,
                       QStringLiteral("Edit marker"));
}

ActionResult removeMarker(ProjectManager* projectManager,
                       ProjectItem* segment,
                       const QUuid& markerGuid,
                       const QObject* originator)
{
    auto* timeline = segment ? segment->comp<VSegment>() : nullptr;
    if (!timeline)
        return failure(QStringLiteral("INVALID_TIMELINE_OWNER"),
                       QStringLiteral("Marker deletion requires a Segment timeline"));
    MarkerLedger markers = timeline->markers;
    const auto it = std::find_if(markers.items.begin(), markers.items.end(),
                                 [&markerGuid](const Marker& marker) {
                                     return marker.markerGuid == markerGuid;
                                 });
    if (it == markers.items.end())
        return failure(QStringLiteral("MARKER_NOT_FOUND"),
                       QStringLiteral("Marker does not exist"));
    markers.items.erase(it);
    return replaceMarkers(projectManager, segment, markers, originator,
                       QStringLiteral("Delete marker"));
}

ActionResult replaceSegmentTime(ProjectManager* projectManager,
                              ProjectItem* segment,
                              const SegmentTimeSpec& segmentTime,
                              const QObject* originator,
                              const QString& undoText)
{
    auto* timeline = segment ? segment->comp<VSegment>() : nullptr;
    if (!projectManager || !timeline)
        return failure(QStringLiteral("INVALID_TIMELINE_OWNER"),
                       QStringLiteral("segmentTime requires a Segment timeline"));
    const SegmentTimeSpec* previousTime = &timeline->segmentTime;
    for (const SegmentTimeSpec* time : {previousTime, &segmentTime}) {
        const auto authoring = AuthoringPolicy::validateSegmentTime(*time);
        if (!authoring)
            return failure(QStringLiteral("AUTHORING_RESTRICTED"), authoring.message);
    }
    const TimelineValidation validity = TimelineCoordinates::validate(segmentTime);
    if (!validity)
        return failure(QStringLiteral("INVALID_ARGS"), validity.message);

    const SegmentTimeSnapshot oldSnapshot = captureSegmentTime(segment);
    SegmentTimeSnapshot newSnapshot = oldSnapshot;
    newSnapshot.segmentTime = segmentTime;
    const bool coordinatesChanged = oldSnapshot.segmentTime.domain != segmentTime.domain;
    if (coordinatesChanged) {
        for (Marker& marker : newSnapshot.markers.items) {
            const CoordinateResult converted = TimelineCoordinates::convert(
                marker.position, oldSnapshot.segmentTime, segmentTime);
            if (!converted)
                return failure(QStringLiteral("INVALID_ARGS"), converted.validation.message);
            marker.position = converted.value;
        }
        for (auto it = newSnapshot.tracks.begin(); it != newSnapshot.tracks.end(); ++it) {
            QString error;
            auto converted = convertClipLedger(
                it.value(), oldSnapshot.segmentTime, segmentTime, &error);
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
    result.data = propertyResult(segment, QStringLiteral("segmentTime"), segmentTime.toJson());
    if (oldSnapshot.segmentTime == segmentTime) return result;
    const QUuid segmentGuid = segment->guid;
    applySegmentTime(segmentGuid, newSnapshot, coordinatesChanged, originator);
    result.undoFn = [segmentGuid, oldSnapshot, coordinatesChanged] {
        applySegmentTime(segmentGuid, oldSnapshot, coordinatesChanged);
    };
    result.redoFn = [segmentGuid, newSnapshot, coordinatesChanged] {
        applySegmentTime(segmentGuid, newSnapshot, coordinatesChanged);
    };
    result.undoText = undoText.isEmpty()
        ? QStringLiteral("Set segment time") : undoText;
    result.authoringWriteSet = QSet<QUuid>{segmentGuid};
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
