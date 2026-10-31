#include "Tooling/Harkenizer/Core/CurrentWorldProjectAdapter.h"
#include "Tooling/Harkenizer/Core/CanonicalRuntimeProjectAdapterPrivate.h"
#include "RuntimeBridge/CurrentWorldSyncBridgeInternal.h"

#include "ActionStepUtils.h"
#include "ContainerBehaviorSemantics.h"
#include "PlayablePropertyAuthoringV1.h"
#include "ProjectTypes.h"
#include "ProjectLanguageSemantics.h"
#include "RuntimeCapabilityMatrix.h"
#include "SoundSourceEditing.h"
#include "Components/VAudio.h"
#include "Components/VAction.h"
#include "Components/VMixNode.h"
#include "Components/VSegment.h"
#include "Components/VSelection.h"
#include "Components/VSpatial.h"
#include "Components/VTrack.h"
#include "Components/VVariable.h"

#include <harken_studio_build/BindingTargets.h>
#include <harken_studio/AudioValues.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace VNX {
namespace {

using BridgeRequest = HarkenEditor::Bridge::CurrentWorldSyncRequest;
using ContentKind = harkenprojectschema::ContentKind;
using ProjectIndex =
    HarkenEditor::Bridge::CanonicalRuntimeProjectSourceIndexV1;

void setError(QString* error, const QString& value)
{
    if (error != nullptr)
        *error = value;
}

void setStatus(CurrentWorldBuildStatus* status,
               CurrentWorldBuildStatus value)
{
    if (status != nullptr)
        *status = value;
}

harkenprojectschema::AuthoringGuid authoringGuid(const QUuid& guid)
{
    const QByteArray bytes = guid.toRfc4122();
    if (bytes.size() != 16)
        return {};
    const auto* data = reinterpret_cast<const uchar*>(bytes.constData());
    return {qFromBigEndian<quint64>(data), qFromBigEndian<quint64>(data + 8)};
}

std::optional<ContentKind> contentKind(ProjectItem::Type type)
{
    switch (type) {
        case ProjectItem::Type::Layer: return ContentKind::Layer;
        case ProjectItem::Type::Sound: return ContentKind::Sound;
        case ProjectItem::Type::Container: return ContentKind::Container;
        case ProjectItem::Type::Track: return ContentKind::Track;
        case ProjectItem::Type::Segment: return ContentKind::Segment;
        default: return std::nullopt;
    }
}

harkenplayableproperties::ContentPropertyKindV1 runtimePropertyKind(
    ContentKind kind)
{
    return static_cast<harkenplayableproperties::ContentPropertyKindV1>(
        static_cast<uint32_t>(kind));
}

bool variableControlHasPlayableGrid(const ProjectItem& item, PlatformScope scope)
{
    const auto* selection = item.comp<VSelection>();
    if (selection == nullptr)
        return false;
    if (selection->mode == SelectionMode::Match)
        return !selection->matchGrid(scope).axes.empty() && !selection->activeRows(scope).empty();
    if (selection->mode == SelectionMode::Blend)
        return !selection->activeRows(scope).empty();
    return true;
}

QString soundLanguage(const ProjectItem& item, const ProjectData& project,
                      const CurrentWorldBuildOptions* options, const QString& languageFallback)
{
    const auto* sound = item.comp<VSound>();
    if (!sound || !sound->distinguishLanguage)
        return {};
    const QString language = options && options->language
        ? *options->language : languageFallback;
    return ProjectLanguageSemantics::buildLanguages(project).contains(language)
        ? language : QString();
}

std::unordered_set<ProjectItem*> runtimeDefinitions(
    const ProjectIndex& index, const ProjectData& project, PlatformScope scope,
    const CurrentWorldBuildOptions* options, const QString& languageFallback,
    std::unordered_set<ProjectItem*>* languageUnavailable = nullptr)
{
    std::unordered_set<ProjectItem*> allowed;
    if (options && options->includedDefinitionIds) {
        for (ProjectItem* item : index.definitions) {
            if (options->includedDefinitionIds->count(item->runtimeId()) == 0)
                continue;
            for (ProjectItem* ancestor = item; ancestor;) {
                allowed.insert(ancestor);
                const auto parent = index.parentDefinition.find(ancestor);
                ancestor = parent == index.parentDefinition.end()
                    ? nullptr : parent->second;
            }
        }
    } else {
        allowed.insert(index.definitions.begin(), index.definitions.end());
    }
    std::unordered_set<ProjectItem*> included;
    std::unordered_set<ProjectItem*> mediaUnavailable;
    for (ProjectItem* item : index.definitions) {
        if (allowed.find(item) == allowed.end())
            continue;
        const auto* sound = item->type == ProjectItem::Type::Sound
            ? item->comp<VSound>() : nullptr;
        if (item->type == ProjectItem::Type::Sound
            && options && options->soundContentRevisions) {
            const auto revision = options->soundContentRevisions->find(item->runtimeId());
            if (revision == options->soundContentRevisions->end() || revision->second == 0) {
                mediaUnavailable.insert(item);
                continue;
            }
        }
        if (!(options && options->includedDefinitionIds && !options->actionRoot
              && !options->definitionRoot)
            && sound && sound->distinguishLanguage
            && !SoundSourceEditing::activeSource(
                *item, scope.value_or(ProjectPlatformId{}),
                soundLanguage(*item, project, options, languageFallback))) {
            if (languageUnavailable) {
                for (ProjectItem* ancestor = item; ancestor;) {
                    languageUnavailable->insert(ancestor);
                    const auto parent = index.parentDefinition.find(ancestor);
                    ancestor = parent == index.parentDefinition.end()
                        ? nullptr : parent->second;
                }
            }
            continue;
        }
        if (item->type != ProjectItem::Type::Container)
            included.insert(item);
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (ProjectItem* item : index.definitions) {
            if (item->type != ProjectItem::Type::Container
                || allowed.find(item) == allowed.end()
                || included.find(item) != included.end()
                || ((!options || !options->includedDefinitionIds
                     || options->includedDefinitionIds->count(item->runtimeId()) != 0)
                    && !variableControlHasPlayableGrid(*item, scope)))
                continue;
            bool hasRuntimeChild = false;
            for (const auto& occurrence :
                 item->comp<VSelection>()->activeRows(scope)) {
                const auto child = index.byGuid.find(
                    authoringGuid(occurrence.childGuid));
                if (child != index.byGuid.end()
                    && included.find(child->second) != included.end()) {
                    hasRuntimeChild = true;
                    break;
                }
            }
            if (hasRuntimeChild)
                changed = included.insert(item).second || changed;
        }
    }
    changed = languageUnavailable && !languageUnavailable->empty();
    while (changed) {
        changed = false;
        for (ProjectItem* item : index.definitions) {
            if (item->type != ProjectItem::Type::Container
                || included.find(item) != included.end()
                || languageUnavailable->find(item) != languageUnavailable->end())
                continue;
            for (const auto& occurrence : item->comp<VSelection>()->activeRows(scope)) {
                const auto child = index.byGuid.find(authoringGuid(occurrence.childGuid));
                if (child != index.byGuid.end()
                    && languageUnavailable->find(child->second) != languageUnavailable->end()) {
                    changed = languageUnavailable->insert(item).second || changed;
                    break;
                }
            }
        }
    }
    changed = !mediaUnavailable.empty();
    while (changed) {
        changed = false;
        const auto unavailableTarget = [&](const QUuid& guid) {
            const auto target = index.byQtGuid.find(guid);
            return target != index.byQtGuid.end()
                && mediaUnavailable.count(target->second) != 0;
        };
        for (ProjectItem* item : index.definitions) {
            if (mediaUnavailable.count(item) != 0)
                continue;
            bool unavailable = false;
            if (item->type == ProjectItem::Type::Container) {
                const auto members = item->comp<VSelection>()->activeRows(scope);
                unavailable = std::any_of(members.begin(), members.end(),
                    [&](const auto& row) { return unavailableTarget(row.childGuid); });
            } else if (const auto* timeline = item->comp<VTrack>()) {
                unavailable = std::any_of(timeline->clips.items.begin(), timeline->clips.items.end(),
                    [&](const auto& clip) { return unavailableTarget(clip.targetGuid); });
            } else if (item->type == ProjectItem::Type::Segment) {
                for (const auto& [child, parent] : index.parentDefinition)
                    unavailable = unavailable || (parent == item && mediaUnavailable.count(child) != 0);
            }
            if (unavailable) {
                mediaUnavailable.insert(item);
                included.erase(item);
                changed = true;
            }
        }
    }
    return included;
}

std::optional<harkenstudiobuild::ActionOpcodeV1> actionOpcode(VActionStep::Type type)
{
    switch (type) {
        case VActionStep::Type::Play: return harkenstudiobuild::ActionOpcodeV1::Play;
        case VActionStep::Type::Stop: return harkenstudiobuild::ActionOpcodeV1::Stop;
        case VActionStep::Type::PostAction:
            return harkenstudiobuild::ActionOpcodeV1::PostAction;
        case VActionStep::Type::SetVariable:
            return harkenstudiobuild::ActionOpcodeV1::SetVariable;
        case VActionStep::Type::Reset: return harkenstudiobuild::ActionOpcodeV1::Reset;
    }
    return std::nullopt;
}

std::optional<harkenstudiobuild::ActionTargetKindV1> actionTargetKind(ProjectItem::Type type)
{
    switch (type) {
        case ProjectItem::Type::Sound:
            return harkenstudiobuild::ActionTargetKindV1::Sound;
        case ProjectItem::Type::Container:
            return harkenstudiobuild::ActionTargetKindV1::Container;
        case ProjectItem::Type::Track:
            return harkenstudiobuild::ActionTargetKindV1::Track;
        case ProjectItem::Type::Segment:
            return harkenstudiobuild::ActionTargetKindV1::Segment;
        case ProjectItem::Type::Action:
            return harkenstudiobuild::ActionTargetKindV1::Action;
        case ProjectItem::Type::Variable:
            return harkenstudiobuild::ActionTargetKindV1::Variable;
        case ProjectItem::Type::Bus:
            return harkenstudiobuild::ActionTargetKindV1::Bus;
        default:
            return std::nullopt;
    }
}

std::optional<harkenstudiobuild::ActionQuantizeTimingV1> actionQuantizeTiming(
    QuantizeTiming timing)
{
    switch (timing) {
        case QuantizeTiming::Immediate:
            return harkenstudiobuild::ActionQuantizeTimingV1::Immediate;
        case QuantizeTiming::NextBeat:
            return harkenstudiobuild::ActionQuantizeTimingV1::NextBeat;
        case QuantizeTiming::NextBar:
            return harkenstudiobuild::ActionQuantizeTimingV1::NextBar;
        case QuantizeTiming::NextMarker:
            return harkenstudiobuild::ActionQuantizeTimingV1::NextMarker;
        case QuantizeTiming::NaturalEnd:
            return harkenstudiobuild::ActionQuantizeTimingV1::NaturalEnd;
        case QuantizeTiming::CurrentLoopEnd:
            return harkenstudiobuild::ActionQuantizeTimingV1::CurrentLoopEnd;
        case QuantizeTiming::EntryMarker:
            return std::nullopt;
        case QuantizeTiming::ExitMarker:
            return harkenstudiobuild::ActionQuantizeTimingV1::ExitMarker;
    }
    return std::nullopt;
}

std::optional<harkenstudiobuild::ActionIfSilentV1> actionIfSilent(QuantizeIfSilent value)
{
    switch (value) {
        case QuantizeIfSilent::Immediate:
            return harkenstudiobuild::ActionIfSilentV1::Immediate;
        case QuantizeIfSilent::Drop:
            return harkenstudiobuild::ActionIfSilentV1::Drop;
    }
    return std::nullopt;
}

std::optional<harkenstudiobuild::ActionScopeV1> actionResetScope(ResetScope scope)
{
    switch (scope) {
        case ResetScope::Emitter: return harkenstudiobuild::ActionScopeV1::Emitter;
        case ResetScope::Global: return harkenstudiobuild::ActionScopeV1::Global;
    }
    return std::nullopt;
}

std::optional<harkenstudiobuild::ActionScopeV1> variableWriteScope(VariableScope scope)
{
    switch (scope) {
        case VariableScope::Global: return harkenstudiobuild::ActionScopeV1::Global;
        case VariableScope::Emitter: return harkenstudiobuild::ActionScopeV1::Emitter;
    }
    return std::nullopt;
}

bool isPlayableActionReference(ProjectItem::Type type)
{
    return type == ProjectItem::Type::Sound
        || type == ProjectItem::Type::Container
        || type == ProjectItem::Type::Track
        || type == ProjectItem::Type::Segment;
}

bool buildDefinitionNode(ProjectItem& item, const ProjectIndex& index,
                         const std::unordered_set<ProjectItem*>& included,
                         const FieldGroupEffectiveSnapshot& groups,
                         int64_t editorOrder, PlatformScope scope,
                         BridgeRequest* request, QString* error)
{
    const auto kind = contentKind(item.type);
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(item);
    if (!kind || !locator || request == nullptr || !request->identityIndex
        || harkenstudio::IsNull(item.runtimeId())) {
        setError(error, QStringLiteral("Playable definition identity is invalid"));
        return false;
    }
    harkenprojectschema::InputNode node;
    node.kind = *kind;
    node.guid = authoringGuid(item.guid);
    node.editorOrder = editorOrder;
    if (!HarkenEditor::Bridge::copyStudioDefinitionPayload(
            item, scope, index, groups, *request->identityIndex, &node, error)) {
        return false;
    }

    std::vector<ProjectItem*> ownedChildren;
    const auto collectChildren = [&](ProjectItem* owner, const auto& self) -> void {
        const auto children = index.children.find(owner);
        if (children == index.children.end()) return;
        for (ProjectItem* child : children->second) {
            if (included.find(child) != included.end())
                ownedChildren.push_back(child);
            else
                self(child, self);
        }
    };
    collectChildren(&item, collectChildren);
    std::sort(ownedChildren.begin(), ownedChildren.end(), [](const auto* a, const auto* b) {
        if (a->runtimeId() != b->runtimeId())
            return a->runtimeId() < b->runtimeId();
        return a->guid.toString() < b->guid.toString();
    });
    for (ProjectItem* child : ownedChildren) {
        harkenprojectschema::InputOwnershipEdge edge;
        edge.childGuid = authoringGuid(child->guid);
        node.ownedChildren.push_back(std::move(edge));
    }
    if (*kind == ContentKind::Container) {
        for (const auto& row : item.comp<VSelection>()->activeRows(scope)) {
            const auto occurrence =
                PlayablePropertyAuthoringV1::containerChildLocator(
                    item, row.stableOccurrenceOrdinal, scope);
            const auto childEntry = index.byGuid.find(
                authoringGuid(row.childGuid));
            ProjectItem* child = occurrence && childEntry != index.byGuid.end()
                ? childEntry->second : nullptr;
            if (!occurrence || !child) {
                setError(error,
                         QStringLiteral("Container occurrence identity is invalid"));
                return false;
            }
            if (included.find(child) == included.end())
                continue;
            harkenprojectschema::InputContainerChildOccurrence authored;
            authored.childGuid = authoringGuid(row.childGuid);
            authored.stableOccurrenceOrdinal = row.stableOccurrenceOrdinal;
            if (!HarkenEditor::Bridge::copyStudioOccurrencePayload(
                    item, *occurrence, scope, &authored, error)) {
                setError(error,
                         QStringLiteral("Container occurrence payload is invalid"));
                return false;
            }
            node.containerChildOccurrences.push_back(std::move(authored));
        }
    }

    if (*kind == ContentKind::Track) {
        const auto* timeline = item.comp<VTrack>();
        if (timeline == nullptr) {
            setError(error, QStringLiteral("Track timeline is unavailable"));
            return false;
        }
        for (const Clip& source : timeline->clips.items) {
            const auto target = index.byGuid.find(authoringGuid(source.targetGuid));
            if (target != index.byGuid.end()
                && included.find(target->second) == included.end())
                continue;
            const auto occurrence =
                PlayablePropertyAuthoringV1::trackClipLocator(item,
                                                               source.clipGuid);
            if (!occurrence) {
                setError(error, QStringLiteral("Track occurrence identity is invalid"));
                return false;
            }
            harkenprojectschema::InputClip clip;
            clip.guid = authoringGuid(source.clipGuid);
            clip.targetGuid = authoringGuid(source.targetGuid);
            clip.stableOccurrenceOrdinal = source.stableOccurrenceOrdinal;
            clip.time = source.start;
            clip.duration = source.duration;
            clip.semanticOrderKey = source.stableOccurrenceOrdinal;
            if (!HarkenEditor::Bridge::copyStudioOccurrencePayload(
                    item, *occurrence, scope, &clip, error)) {
                setError(error, QStringLiteral("Track occurrence payload is invalid"));
                return false;
            }
            node.clips.push_back(std::move(clip));
        }
    }

    if (*kind == ContentKind::Segment) {
        const auto* timeline = item.comp<VSegment>();
        if (timeline == nullptr) {
            setError(error, QStringLiteral("Segment timeline is unavailable"));
            return false;
        }
        harkenprojectschema::InputSegmentTimeSpec time;
        time.kind = timeline->segmentTime.domain == TimelineDomain::Musical
            ? harkenprojectschema::SegmentTimeKind::Musical
            : harkenprojectschema::SegmentTimeKind::Absolute;
        for (const TempoPoint& source : timeline->segmentTime.tempoMap)
            time.tempoMap.push_back({source.tick,
                                     source.microsecondsPerQuarter});
        for (const MeterPoint& source : timeline->segmentTime.meterMap) {
            if (source.numerator < 0 || source.numerator > UINT16_MAX
                || source.denominator < 0 || source.denominator > UINT16_MAX) {
                setError(error, QStringLiteral("Segment meter is invalid"));
                return false;
            }
            time.meterMap.push_back(
                {source.tick, static_cast<uint16_t>(source.numerator),
                 static_cast<uint16_t>(source.denominator)});
        }
        node.timeSpec = std::move(time);
        node.startPoint = harkenprojectschema::InputStartPoint{};
        uint32_t markerId = 1;
        for (const Marker& source : timeline->markers.items) {
            if (source.kind == MarkerKind::Entry)
                continue;
            harkenprojectschema::InputMarker marker;
            marker.guid = authoringGuid(source.markerGuid);
            marker.markerId = markerId++;
            marker.kind = source.kind == MarkerKind::Exit
                ? harkenprojectschema::MarkerKind::Exit
                : harkenprojectschema::MarkerKind::Custom;
            marker.position = source.position;
            node.markers.push_back(std::move(marker));
        }
    }

    request->project.nodes.push_back(std::move(node));
    return true;
}

std::optional<harkenstudiobuild::PlayableKindV1> transitionPlayableKind(
    ProjectItem::Type type)
{
    switch (type) {
        case ProjectItem::Type::Sound: return harkenstudiobuild::PlayableKindV1::Sound;
        case ProjectItem::Type::Container:
            return harkenstudiobuild::PlayableKindV1::Container;
        case ProjectItem::Type::Track: return harkenstudiobuild::PlayableKindV1::Track;
        case ProjectItem::Type::Segment: return harkenstudiobuild::PlayableKindV1::Segment;
        default: return std::nullopt;
    }
}

bool accumulateBoundedTransitionSoundLeaves(
    const ProjectItem& item, const ProjectIndex& index,
    std::unordered_set<const ProjectItem*>* active,
    std::uint32_t* leafCount)
{
    const std::uint32_t leafCapacity =
        harkenstudiobuild::WorldCapacitySummaryV1{}.maxKnownScheduleLeaves;
    if (active == nullptr || leafCount == nullptr
        || leafCapacity == 0 || *leafCount > leafCapacity)
        return false;
    if (item.type == ProjectItem::Type::Sound) {
        if (*leafCount == leafCapacity)
            return false;
        ++*leafCount;
        return true;
    }
    if (item.type == ProjectItem::Type::Container
        || !active->insert(&item).second)
        return false;

    bool valid = true;
    if (item.type == ProjectItem::Type::Track) {
        const auto* timeline = item.comp<VTrack>();
        if (timeline == nullptr) {
            valid = false;
        } else {
            for (const Clip& clip : timeline->clips.items) {
                const auto target = index.byGuid.find(
                    authoringGuid(clip.targetGuid));
                if (target == index.byGuid.end() || target->second == nullptr
                    || !accumulateBoundedTransitionSoundLeaves(
                        *target->second, index, active, leafCount)) {
                    valid = false;
                    break;
                }
            }
        }
    } else if (item.type == ProjectItem::Type::Segment) {
        const auto children = index.children.find(
            const_cast<ProjectItem*>(&item));
        if (children == index.children.end()) {
            valid = false;
        } else {
            for (const ProjectItem* child : children->second) {
                if (child == nullptr
                    || !accumulateBoundedTransitionSoundLeaves(
                        *child, index, active, leafCount)) {
                    valid = false;
                    break;
                }
            }
        }
    } else {
        valid = false;
    }
    active->erase(&item);
    return valid;
}

bool transitionEndpointHasBoundedSounds(const ProjectItem& item,
                                        const ProjectIndex& index)
{
    std::unordered_set<const ProjectItem*> active;
    std::uint32_t leafCount = 0;
    const std::uint32_t leafCapacity =
        harkenstudiobuild::WorldCapacitySummaryV1{}.maxKnownScheduleLeaves;
    return accumulateBoundedTransitionSoundLeaves(
               item, index, &active, &leafCount)
        && leafCount != 0
        && leafCount <= leafCapacity;
}

bool appendTransitionFade(
    const TransitionFadeDef& source, std::uint32_t sampleRate,
    std::uint32_t* nextCurveId,
    std::vector<harkenstudiobuild::CurveV1>* curves, harkenstudiobuild::TransitionFadeV1* output)
{
    if (nextCurveId == nullptr || curves == nullptr || output == nullptr ||
        sampleRate == 0 ||
        *nextCurveId == 0 || *nextCurveId == UINT32_MAX ||
        !std::isfinite(source.durationSeconds) ||
        source.durationSeconds < 0.0f || source.curve.size() < 2 ||
        source.curve.size() > kCurveMaxPoints ||
        source.handles.size() + 1 != source.curve.size()) {
        return false;
    }
    const double frameCount = std::ceil(
        static_cast<double>(source.durationSeconds) *
        static_cast<double>(sampleRate));
    if (frameCount > static_cast<double>(UINT32_MAX)) return false;

    std::array<harkenstudiobuild::CurvePoint, harkenstudiobuild::kMaxCurvePoints> points{};
    std::array<harkenstudiobuild::CurveHandlePair, harkenstudiobuild::kMaxCurvePoints - 1> handles{};
    for (qsizetype i = 0; i < source.curve.size(); ++i) {
        points[static_cast<std::size_t>(i)] = {
            source.curve[i].x, source.curve[i].value};
    }
    for (qsizetype i = 0; i < source.handles.size(); ++i) {
        handles[static_cast<std::size_t>(i)] = {
            source.handles[i].h1x, source.handles[i].h1y,
            source.handles[i].h2x, source.handles[i].h2y};
    }
    harkenstudiobuild::CurveV1 curve;
    curve.curveLocalId = *nextCurveId;
    if (!curve.curve.SetPoints(
            points.data(), handles.data(),
            static_cast<std::uint32_t>(source.curve.size()))) {
        return false;
    }
    output->active = 1;
    output->durationFrames = static_cast<std::uint32_t>(frameCount);
    output->curveLocalId = curve.curveLocalId;
    curves->push_back(curve);
    ++*nextCurveId;
    return true;
}

QString transitionExitTimingName(TransitionExitTiming timing)
{
    switch (timing) {
        case TransitionExitTiming::Immediate:
            return QStringLiteral("Immediate");
        case TransitionExitTiming::NaturalEnd:
            return QStringLiteral("NaturalEnd");
        case TransitionExitTiming::NextBeat:
            return QStringLiteral("NextBeat");
        case TransitionExitTiming::NextBar:
            return QStringLiteral("NextBar");
        case TransitionExitTiming::NextMarker:
            return QStringLiteral("NextMarker");
        case TransitionExitTiming::ExitMarker:
            return QStringLiteral("ExitMarker");
    }
    return {};
}

bool appendBasicTransitionProgram(
    ProjectItem& item, const ProjectIndex& index,
    const std::unordered_set<ProjectItem*>& included,
    const VSelection& selection, PlatformScope scope, std::uint32_t sampleRate,
    std::uint32_t* nextCurveId,
    harkencurrentworldcompiler::ContainerAuthoringV1* container,
    BridgeRequest* request, QString* error,
    CurrentWorldBuildStatus* status)
{
    if (!selection.isQueue()
        && (selection.mode != SelectionMode::Match
            || selection.variableResponse() != VariableResponse::Live))
        return true;
    auto absoluteDefault = canonicalTransitionTable();
    absoluteDefault.rules.front().exitTiming = TransitionExitTiming::Immediate;
    if (selection.transitions == canonicalTransitionTable()
        || selection.transitions == absoluteDefault) return true;
    const TransitionValidation validation = selection.transitions.validate();
    if (!validation) {
        setError(error, validation.message);
        return false;
    }
    if (TransitionSemantics::transitionMountState(item, scope) !=
        TransitionMountState::Editable) {
        setError(error,
                 QStringLiteral("Transition is unavailable for this Container mode"));
        return false;
    }
    if (selection.transitions.rules.size() > UINT32_MAX ||
        request == nullptr || container == nullptr || nextCurveId == nullptr ||
        request->transitions.size() >= UINT32_MAX) {
        setError(error, QStringLiteral("Transition authoring exceeds capacity"));
        return false;
    }

    for (const auto& member : selection.activeRows(scope)) {
        const auto found = index.byQtGuid.find(member.childGuid);
        const ProjectItem* child = found != index.byQtGuid.end() ? found->second : nullptr;
        if (child == nullptr
            || included.find(const_cast<ProjectItem*>(child)) ==
                included.end()) {
            continue;
        }
        if (selection.isQueue()
            && !transitionEndpointHasBoundedSounds(*child, index)) {
            setStatus(status,
                      CurrentWorldBuildStatus::CapabilityUnavailable);
            setError(error, QStringLiteral(
                "Transition V1 requires each Queue child to resolve to bounded Sound voices"));
            return false;
        }
        if (selection.mode == SelectionMode::Match
            && !transitionEndpointHasBoundedSounds(*child, index)) {
            setStatus(status,
                      CurrentWorldBuildStatus::CapabilityUnavailable);
            setError(error, QStringLiteral(
                "Match transitions require bounded Sound, Track or Segment children"));
            return false;
        }
    }

    const auto endpoint = [&](const QUuid& guid,
                              harkenstudiobuild::PlayableRefV1* output) {
        const auto children = index.children.find(&item);
        if (children == index.children.end() || output == nullptr) return false;
        const auto found = std::find_if(
            children->second.begin(), children->second.end(),
            [&](const ProjectItem* child) {
                return child != nullptr && child->guid == guid;
            });
        if (found == children->second.end() || *found == nullptr ||
            harkenstudio::IsNull((*found)->runtimeId())) {
            return false;
        }
        const auto kind = transitionPlayableKind((*found)->type);
        if (!kind) return false;
        output->kind = *kind;
        output->stableId = (*found)->runtimeId();
        return true;
    };

    harkencurrentworldcompiler::TransitionProgramAuthoringV1 program;
    program.ownerContainerStableId = item.runtimeId();
    bool haveDefault = false;
    for (std::size_t i = 0; i < selection.transitions.rules.size(); ++i) {
        const TransitionRuleDef& source = selection.transitions.rules[i];
        const auto excludedEndpoint = [&](const std::optional<QUuid>& guid) {
            if (!guid)
                return false;
            const auto target = index.byGuid.find(authoringGuid(*guid));
            return target != index.byGuid.end()
                && included.find(target->second) == included.end();
        };
        if (excludedEndpoint(source.source) || excludedEndpoint(source.target))
            continue;
        if (source.exitTiming != TransitionExitTiming::Immediate &&
            source.exitTiming != TransitionExitTiming::NaturalEnd) {
            setStatus(status,
                      CurrentWorldBuildStatus::CapabilityUnavailable);
            setError(error, QStringLiteral(
                "Current World Transition supports Immediate and Natural End exits; %1 is unavailable")
                .arg(transitionExitTimingName(source.exitTiming)));
            return false;
        }
        if (source.bridge) {
            setStatus(status,
                      CurrentWorldBuildStatus::CapabilityUnavailable);
            setError(error,
                     QStringLiteral("Current World Transition does not support a Bridge"));
            return false;
        }
        if (source.entryMode != TransitionEntryMode::EntryMarker) {
            setStatus(status,
                      CurrentWorldBuildStatus::CapabilityUnavailable);
            setError(error,
                     QStringLiteral(
                         "Current World Transition does not support SamePosition target entry"));
            return false;
        }

        harkenstudiobuild::TransitionRuleViewV1 rule;
        rule.stableRuleOrdinal = static_cast<std::uint32_t>(i);
        rule.authoredStableOrder = static_cast<std::uint32_t>(i);
        rule.sourceAny = source.source ? 0u : 1u;
        rule.targetAny = source.target ? 0u : 1u;
        if ((source.source && !endpoint(*source.source, &rule.source)) ||
            (source.target && !endpoint(*source.target, &rule.target))) {
            setError(error,
                     QStringLiteral("Transition endpoint is not a direct runtime child"));
            return false;
        }
        rule.sourceExit = static_cast<harkenstudiobuild::TransitionSourceExitV1>(
            static_cast<std::uint32_t>(source.exitTiming));
        rule.targetEntry = harkenstudiobuild::TransitionTargetEntryV1::FromEntryMarker;
        if (!appendTransitionFade(source.sourceFadeOut, sampleRate, nextCurveId,
                                  &container->curves, &rule.sourceFade) ||
            !appendTransitionFade(source.targetFadeIn, sampleRate, nextCurveId,
                                  &container->curves, &rule.targetFade)) {
            setError(error, QStringLiteral("Transition fade is invalid"));
            return false;
        }
        if (source.isDefault()) {
            program.defaultRuleIndex = static_cast<std::uint32_t>(program.rules.size());
            haveDefault = true;
        }
        program.rules.push_back(rule);
    }
    if (!haveDefault) {
        setError(error, QStringLiteral("Transition table has no default rule"));
        return false;
    }
    container->transitionProgramIndex =
        static_cast<std::uint32_t>(request->transitions.size());
    request->transitions.push_back(std::move(program));
    return true;
}

bool buildContainerAuthoring(ProjectItem& item, const ProjectIndex& index,
                             const std::unordered_set<ProjectItem*>& included,
                             uint32_t authoredOrder, std::uint32_t sampleRate,
                             PlatformScope scope, BridgeRequest* request,
                             QString* error,
                             CurrentWorldBuildStatus* status, bool execute)
{
    VSelection inheritedSelection;
    inheritedSelection.mode = SelectionMode::Sequence;
    const auto* selection = execute ? item.comp<VSelection>() : &inheritedSelection;
    if (selection == nullptr || request == nullptr) {
        setError(error, QStringLiteral("Container selection is unavailable"));
        return false;
    }
    const auto matches = selection->mode == SelectionMode::Match
        ? selection->matchGrid(scope) : DiscreteGridDef{};
    const auto mixing = selection->mode == SelectionMode::Blend
        ? selection->mixing(scope) : MixingDef{};
    const auto activeRows = item.comp<VSelection>()->activeRows(scope);
    harkencurrentworldcompiler::ContainerAuthoringV1 row;
    row.containerStableId = item.runtimeId();
    row.authoredStableOrder = authoredOrder;
    row.memoryScope = selection->behavior.memoryScope ==
            SelectionMemoryScope::Global
        ? harkenstudiobuild::MemoryScopeV1::Global
        : harkenstudiobuild::MemoryScopeV1::Emitter;
    if (selection->behavior.avoidRepeat < 0) {
        setError(error, QStringLiteral("Container avoid-repeat is invalid"));
        return false;
    }
    row.avoidLastN = static_cast<uint32_t>(ContainerBehaviorSemantics::effectiveAvoidRepeat(*selection, scope));
    switch (selection->mode) {
        case SelectionMode::Random:
            row.mode = selection->isQueue() ? harkenstudiobuild::ContainerModeV1::RandomQueue
                                           : harkenstudiobuild::ContainerModeV1::Random;
            row.randomDiscipline = selection->isQueue() || selection->behavior.discipline ==
                    RandomDiscipline::Shuffle
                ? harkenstudiobuild::RandomDisciplineV1::Rotation
                : harkenstudiobuild::RandomDisciplineV1::Draw;
            break;
        case SelectionMode::Sequence:
            row.mode = selection->isQueue() ? harkenstudiobuild::ContainerModeV1::SequenceQueue
                                           : harkenstudiobuild::ContainerModeV1::Sequence;
            row.avoidLastN = 0;
            break;
        case SelectionMode::Match:
        case SelectionMode::Blend:
            row.mode = harkenstudiobuild::ContainerModeV1::VariableControl;
            row.avoidLastN = 0;
            break;
    }
    const bool queueMode = row.mode == harkenstudiobuild::ContainerModeV1::RandomQueue
        || row.mode == harkenstudiobuild::ContainerModeV1::SequenceQueue;
    const bool variableControl =
        row.mode == harkenstudiobuild::ContainerModeV1::VariableControl;
    std::uint32_t nextCurveId = 1;
    if (variableControl) {
        row.memoryScope = harkenstudiobuild::MemoryScopeV1::Emitter;
    }
    if (queueMode) {
        const auto locator = PlayablePropertyAuthoringV1::definitionLocator(item);
        const auto state = locator
            ? PlayablePropertyAuthoringV1::readProperty(item, *locator, 4, scope)
            : std::nullopt;
        if (!state) {
            setError(error, QStringLiteral("Queue loop policy is unsupported"));
            return false;
        }
        const auto loopCount = static_cast<std::uint32_t>(state->value.toInteger());
        row.memoryScope = harkenstudiobuild::MemoryScopeV1::Emitter;
        if (loopCount == 0) {
            row.queueCycle = harkenstudiobuild::QueueCycleV1::Loop;
            row.queueCycleCount = 0;
        } else if (loopCount == 1) {
            row.queueCycle = harkenstudiobuild::QueueCycleV1::Once;
            row.queueCycleCount = 0;
        } else {
            row.queueCycle = harkenstudiobuild::QueueCycleV1::Loop;
            row.queueCycleCount = loopCount;
        }
        row.queueSplice = selection->spliceByMarker
            ? harkenstudiobuild::QueueSpliceV1::MarkerAligned
            : harkenstudiobuild::QueueSpliceV1::Butt;
        row.resumePosition = selection->behavior.resumeFromLast;
    }
    uint32_t order = 0;
    for (const auto& occurrenceRow : activeRows) {
        const auto childEntry = index.byGuid.find(
            authoringGuid(occurrenceRow.childGuid));
        ProjectItem* child = childEntry != index.byGuid.end()
            ? childEntry->second : nullptr;
        if (!child || included.find(child) == included.end())
            continue;
        const auto occurrence =
            PlayablePropertyAuthoringV1::containerChildLocator(
                item, occurrenceRow.stableOccurrenceOrdinal, scope);
        if (!occurrence) {
                setError(error,
                         QStringLiteral("Container child occurrence is unavailable"));
                return false;
        }
        harkencurrentworldcompiler::ContainerChildAuthoringV1 authored;
        authored.stableOccurrenceOrdinal = occurrence->stableOccurrenceOrdinal;
        authored.authoredStableOrder = order++;
        if (queueMode && child->type == ProjectItem::Type::Sound) {
            const auto childLocator =
                PlayablePropertyAuthoringV1::definitionLocator(*child);
            const auto streamingState = childLocator
                ? PlayablePropertyAuthoringV1::readProperty(
                      *child, *childLocator, 5, scope)
                : std::nullopt;
            const auto loopState = childLocator
                ? PlayablePropertyAuthoringV1::readProperty(
                      *child, *childLocator, 4, scope)
                : std::nullopt;
            if (streamingState && loopState
                && streamingState->value.toInteger() == 0
                && loopState->value.toInteger() != 0) {
                authored.completion.completionClass =
                    harkenstudiobuild::AuthoredCompletionClassV1::FiniteMediaCandidate;
                harkencurrentworldcompiler::CompletionLeafAuthoringV1 leaf;
                leaf.value.stableLeafOrdinal = 1;
                leaf.value.content.kind = harkenstudiobuild::PlayableKindV1::Sound;
                leaf.value.content.stableId = child->runtimeId();
                leaf.value.startOffset = {
                    harkenstudiobuild::AuthoredTimeDomainV1::SourceFrames, 0, 1};
                leaf.value.duration = {
                    harkenstudiobuild::AuthoredTimeDomainV1::NaturalMediaEnd, 0, 1};
                authored.completion.leaves.push_back(std::move(leaf));
            }
        }
        if (row.mode == harkenstudiobuild::ContainerModeV1::Random
                && row.randomDiscipline == harkenstudiobuild::RandomDisciplineV1::Draw) {
                const int authoredWeight =
                    ContainerBehaviorSemantics::effectiveChildWeight(
                        *selection, occurrenceRow.stableOccurrenceOrdinal, scope);
                if (authoredWeight <
                        ContainerBehaviorSemantics::kMinDrawChildWeight
                    || authoredWeight >
                        ContainerBehaviorSemantics::kMaxChildWeight) {
                    setError(error,
                             QStringLiteral("Container child weight is invalid"));
                    return false;
                }
                const uint32_t weight = static_cast<uint32_t>(authoredWeight);
                authored.frequencyWeight = weight;
        } else if (row.mode == harkenstudiobuild::ContainerModeV1::Random
                       || row.mode == harkenstudiobuild::ContainerModeV1::RandomQueue) {
                const int authoredWeight = std::clamp(
                    ContainerBehaviorSemantics::effectiveChildWeight(
                        *selection, occurrenceRow.stableOccurrenceOrdinal, scope),
                    ContainerBehaviorSemantics::kMinOrderChildWeight,
                    ContainerBehaviorSemantics::kMaxChildWeight);
                authored.orderWeight = static_cast<uint32_t>(authoredWeight);
        }
        row.children.push_back(std::move(authored));
    }
    if (variableControl) {
        const auto childOrdinal = [&](const QUuid& target,
                                      uint32_t* out) {
            const auto found = std::find_if(activeRows.begin(), activeRows.end(),
                [&](const auto& member) { return member.childGuid == target; });
            if (found == activeRows.end() || out == nullptr)
                return false;
            *out = found->stableOccurrenceOrdinal;
            return true;
        };
        const auto appendAxes = [&](const std::vector<QUuid>& axes,
                                    std::vector<harkenstudiobuild::GridAxisV1>* out) {
            if (out == nullptr || axes.size() > 32)
                return false;
            for (uint32_t axisIndex = 0;
                 axisIndex < axes.size(); ++axisIndex) {
                const auto found =
                    index.byGuid.find(authoringGuid(axes[axisIndex]));
                if (found == index.byGuid.end() || found->second == nullptr
                    || found->second->type != ProjectItem::Type::Variable
                    || harkenstudio::IsNull(found->second->runtimeId()))
                    return false;
                out->push_back(
                    {axisIndex, axisIndex,
                     found->second->runtimeId()});
            }
            return true;
        };
        std::vector<QUuid> mixingVariables;
        for (const auto& mixingRow : mixing.rows)
            if (!mixingRow.variable.isNull()) mixingVariables.push_back(mixingRow.variable);
        if (!appendAxes(matches.axes,
                        &row.discreteAxes)
            || !appendAxes(mixingVariables,
                           &row.continuousAxes)) {
            setError(error,
                     QStringLiteral("Selection axis is invalid"));
            return false;
        }
        row.discreteTiming =
            selection->mode == SelectionMode::Match && selection->variableResponse() == VariableResponse::Live
                ? 1u : 0u;
        if (selection->mode == SelectionMode::Blend)
            row.continuousTiming = selection->variableResponse() == VariableResponse::Live ? 1u : 0u;
        for (uint32_t rowIndex = 0;
             rowIndex < matches.rows.size(); ++rowIndex) {
            const auto& source = matches.rows[rowIndex];
            const auto target = index.byGuid.find(authoringGuid(source.target));
            if (target != index.byGuid.end()
                && included.find(target->second) == included.end())
                continue;
            harkencurrentworldcompiler::DiscreteGridRowAuthoringV1 authored;
            authored.stableRowOrdinal = *source.stableRuleOrdinal;
            authored.authoredStableOrder = rowIndex;
            if (!childOrdinal(source.target,
                              &authored.childStableOrdinal)
                || source.cells.size() != row.discreteAxes.size()) {
                setError(error,
                         QStringLiteral("Match row is invalid"));
                return false;
            }
            for (size_t axis = 0; axis < source.cells.size(); ++axis) {
                const auto& sourceCell = source.cells[axis];
                harkenstudiobuild::DiscreteGridCellV1 cell;
                if (sourceCell) {
                    const auto found = index.byGuid.find(authoringGuid(matches.axes[axis]));
                    const auto* variable = found != index.byGuid.end() && found->second
                        ? found->second->comp<VVariable>() : nullptr;
                    const auto value = GridSemantics::discreteCellValue(*sourceCell, variable);
                    if (!value || !variable || *value < variable->rangeMin || *value > variable->rangeMax) {
                        setError(error, QStringLiteral("Grid '%1' row %2 axis %3 references a missing or out-of-range Label")
                            .arg(item.name).arg(rowIndex + 1).arg(axis + 1));
                        return false;
                    }
                    cell.kind = harkenstudiobuild::DiscreteCellKindV1::Exact;
                    cell.exactValue = *value;
                }
                authored.cells.push_back(cell);
            }
            row.discreteRows.push_back(std::move(authored));
        }

        QHash<QUuid, size_t> mixingMembers;
        size_t mixingAxisIndex = 0;
        for (size_t rowIndex = 0; rowIndex < mixing.rows.size(); ++rowIndex) {
            const auto& mixingRow = mixing.rows[rowIndex];
            QSet<QUuid> rowMembers;
            for (const auto& child : mixingRow.children) {
                if (rowMembers.contains(child.target)) {
                    setError(error, QStringLiteral("Mixing row contains a duplicate child"));
                    return false;
                }
                rowMembers.insert(child.target);
                const auto target = index.byGuid.find(authoringGuid(child.target));
                if (target != index.byGuid.end() && included.find(target->second) == included.end()) continue;
                if (!mixingMembers.contains(child.target)) {
                    harkencurrentworldcompiler::ContinuousGridRowAuthoringV1 authored;
                    if (!childOrdinal(child.target, &authored.childStableOrdinal)) return false;
                    authored.stableRowOrdinal = authored.childStableOrdinal;
                    authored.authoredStableOrder = static_cast<uint32_t>(row.continuousRows.size());
                    authored.cells.resize(row.continuousAxes.size());
                    mixingMembers.insert(child.target, row.continuousRows.size());
                    row.continuousRows.push_back(std::move(authored));
                }
                if (mixingRow.variable.isNull()) continue;
                if (child.points.size() < 2 || child.points.size() > kCurveMaxPoints
                    || child.handles.size() + 1 != child.points.size()) {
                    setError(error, QStringLiteral("Mixing curve is invalid"));
                    return false;
                }
                std::array<harkenstudiobuild::CurvePoint, harkenstudiobuild::kMaxCurvePoints> points{};
                std::array<harkenstudiobuild::CurveHandlePair, harkenstudiobuild::kMaxCurvePoints - 1> handles{};
                for (int i = 0; i < child.points.size(); ++i)
                    points[i] = {child.points[i].x, child.points[i].value};
                for (int i = 0; i < child.handles.size(); ++i) {
                    const auto& handle = child.handles[i];
                    handles[i] = {handle.h1x, handle.h1y, handle.h2x, handle.h2y};
                }
                harkenstudiobuild::CurveV1 curve;
                curve.curveLocalId = nextCurveId++;
                if (!curve.curve.SetPoints(points.data(), handles.data(), static_cast<uint32_t>(child.points.size()))) {
                    setError(error, QStringLiteral("Mixing curve cannot be compiled"));
                    return false;
                }
                auto& cell = row.continuousRows[mixingMembers.value(child.target)].cells[mixingAxisIndex];
                cell.kind = harkenstudiobuild::ContinuousCellKindV1::Interval;
                cell.lower = child.points.front().x;
                cell.upper = child.points.back().x;
                cell.lowerInclusive = 1;
                cell.upperInclusive = 1;
                cell.leftCurveLocalId = curve.curveLocalId;
                row.curves.push_back(std::move(curve));
            }
            if (!mixingRow.variable.isNull()) ++mixingAxisIndex;
        }
        if (row.discreteTiming != 0 || (row.continuousTiming != 0 && !row.continuousAxes.empty())) {
            row.evaluationPeriodFrames = 256;
        }
        if (!row.continuousRows.empty()) {
            row.continuousNormalization = 0;
            row.envelopeFrames = 480;
            QSet<uint32_t> mixedChildren;
            for (const auto& member : row.continuousRows)
                mixedChildren.insert(member.childStableOrdinal);
            row.maxActiveRows = static_cast<uint32_t>(mixedChildren.size());
            row.maxLiveRows = std::max(32u, row.maxActiveRows);
        }
    }
    if (!appendBasicTransitionProgram(item, index, included, *selection, scope, sampleRate,
                                      &nextCurveId, &row, request, error,
                                      status)) {
        return false;
    }
    request->containers.push_back(std::move(row));
    return true;
}

enum class ActionTargetResolution { Resolved, Skipped, Invalid };

ActionTargetResolution resolveActionTarget(
    const VActionStep& source, const ProjectItem& owner,
    const ProjectIndex& index,
    const std::unordered_set<ProjectItem*>& included,
    const std::unordered_set<ProjectItem*>& languageUnavailable,
    const EditorUtils::ActionTargetLookup& lookup,
    harkencurrentworldcompiler::ActionObjectRefAuthoringV1* target,
    QString* error, const CurrentWorldBuildOptions* options)
{
    const auto resolved = EditorUtils::resolveActionStepTarget(
        source.type, source.targetObjectID, source.targetName, lookup,
        owner.objectID);
    if (resolved.state != EditorUtils::ActionTargetState::Resolved) {
        if (options && options->skipEmptyActionSteps
            && resolved.state == EditorUtils::ActionTargetState::Empty)
            return ActionTargetResolution::Skipped;
        setError(error, QStringLiteral("Action step target is unresolved"));
        return ActionTargetResolution::Invalid;
    }
    const auto item = index.byObjectId.find(resolved.objectID);
    if (item == index.byObjectId.end() || item->second == nullptr
        || harkenstudio::IsNull(item->second->runtimeId())) {
        setError(error, QStringLiteral("Action step target is invalid"));
        return ActionTargetResolution::Invalid;
    }
    if (options && options->includedActionIds
        && item->second->type == ProjectItem::Type::Action
        && options->includedActionIds->count(item->second->runtimeId()) == 0) {
        return ActionTargetResolution::Skipped;
    }
    if (isPlayableActionReference(item->second->type)
        && included.find(item->second) == included.end()) {
        if (languageUnavailable.find(item->second) != languageUnavailable.end())
            return ActionTargetResolution::Skipped;
        setError(error, QStringLiteral("Action step target is not playable"));
        return ActionTargetResolution::Invalid;
    }
    const auto kind = actionTargetKind(item->second->type);
    if (!kind) {
        setError(error, QStringLiteral("Action step target kind is invalid"));
        return ActionTargetResolution::Invalid;
    }
    target->kind = *kind;
    target->stableId = item->second->runtimeId();
    return ActionTargetResolution::Resolved;
}

ProjectItem* findActionQuantizeReference(
    const VActionStep& source, const ProjectIndex& index,
    const EditorUtils::ActionTargetLookup& lookup)
{
    ProjectItem* item = nullptr;
    if (source.quantize.referenceObjectID != 0) {
        const auto exact = index.byObjectId.find(
            source.quantize.referenceObjectID);
        if (exact != index.byObjectId.end())
            item = exact->second;
    } else if (!source.quantize.referenceName.isEmpty()) {
        const auto resolved = EditorUtils::resolveActionStepTarget(
            VActionStep::Type::Play, 0, source.quantize.referenceName,
            lookup);
        if (resolved.state == EditorUtils::ActionTargetState::Resolved) {
            const auto rebound = index.byObjectId.find(resolved.objectID);
            if (rebound != index.byObjectId.end())
                item = rebound->second;
        }
    }
    return item;
}

ActionTargetResolution resolveActionQuantizeReference(
    const VActionStep& source, const ProjectIndex& index,
    const std::unordered_set<ProjectItem*>& included,
    const std::unordered_set<ProjectItem*>& languageUnavailable,
    const EditorUtils::ActionTargetLookup& lookup,
    harkencurrentworldcompiler::ActionObjectRefAuthoringV1* reference,
    QString* error)
{
    if (source.quantize.timing == QuantizeTiming::Immediate
        || (source.type == VActionStep::Type::Stop
            && source.quantize.referenceObjectID == 0
            && source.quantize.referenceName.isEmpty())) {
        return ActionTargetResolution::Resolved;
    }

    ProjectItem* item = findActionQuantizeReference(source, index, lookup);
    if (item && included.find(item) == included.end()
        && languageUnavailable.find(item) != languageUnavailable.end())
        return ActionTargetResolution::Skipped;
    if (item == nullptr || !isPlayableActionReference(item->type)
        || harkenstudio::IsNull(item->runtimeId())
        || included.find(item) == included.end()) {
        setError(error,
                 QStringLiteral("Action quantize reference is unresolved"));
        return ActionTargetResolution::Invalid;
    }
    reference->kind = *actionTargetKind(item->type);
    reference->stableId = item->runtimeId();
    return ActionTargetResolution::Resolved;
}

struct ActionBuildClosure
{
    std::set<harkenstudio::ObjectId> actions;
    std::set<harkenstudio::ObjectId> definitions;
    std::set<harkenstudio::ObjectId> variables;
};

ActionBuildClosure actionBuildClosure(
    const ProjectItem& root, const ProjectIndex& index,
    const EditorUtils::ActionTargetLookup& lookup, PlatformScope scope)
{
    ActionBuildClosure closure;
    std::unordered_set<const ProjectItem*> visited;
    std::unordered_set<const ProjectItem*> expanded;
    const auto byGuid = [&](const QUuid& guid) {
        const auto found = index.byQtGuid.find(guid);
        return found == index.byQtGuid.end() ? nullptr : found->second;
    };
    const auto visit = [&](const ProjectItem* item, bool descendants,
                           const auto& self) -> void {
        if (!item) return;
        if (descendants && contentKind(item->type))
            closure.definitions.insert(item->runtimeId());
        if (visited.insert(item).second) {
            if (item->type == ProjectItem::Type::Action) {
                closure.actions.insert(item->runtimeId());
                if (const auto* action = item->comp<VAction>()) {
                    for (const VActionStep& step : action->steps) {
                        const auto target = EditorUtils::resolveActionStepTarget(
                            step.type, step.targetObjectID, step.targetName,
                            lookup, item->objectID);
                        const auto found = index.byObjectId.find(target.objectID);
                        if (target.state == EditorUtils::ActionTargetState::Resolved
                            && found != index.byObjectId.end())
                            self(found->second, true, self);
                        if ((step.type == VActionStep::Type::Play
                             || step.type == VActionStep::Type::Stop
                             || step.type == VActionStep::Type::PostAction)
                            && step.quantize.timing != QuantizeTiming::Immediate)
                            self(findActionQuantizeReference(step, index, lookup), true, self);
                    }
                }
            } else if (item->type == ProjectItem::Type::Variable) {
                closure.variables.insert(item->runtimeId());
            } else if (contentKind(item->type)) {
                self(item->parent, false, self);
                if (const auto* mix = item->comp<VMixNode>())
                    for (const auto& binding : mix->bindings.entries())
                        self(byGuid(binding.variable), true, self);
            } else if (item->type == ProjectItem::Type::None) {
                self(item->parent, false, self);
            }
        }
        if (!descendants || !contentKind(item->type)
            || !expanded.insert(item).second)
            return;
        const auto children = index.children.find(const_cast<ProjectItem*>(item));
        if (item->type != ProjectItem::Type::Container && children != index.children.end())
            for (const auto* child : children->second)
                self(child, true, self);
        if (const auto* selection = item->comp<VSelection>())
            for (const auto& occurrence : selection->activeRows(scope))
                self(byGuid(occurrence.childGuid), true, self);
        if (const auto* track = item->comp<VTrack>())
            for (const auto& clip : track->clips.items)
                self(byGuid(clip.targetGuid), true, self);
        if (const auto* selection = item->comp<VSelection>()) {
            if (selection->mode == SelectionMode::Match) {
                for (const auto& axis : selection->matchGrid(scope).axes)
                    self(byGuid(axis), true, self);
            }
            if (selection->mode == SelectionMode::Blend) {
                for (const auto& mixingRow : selection->mixing(scope).rows)
                    if (!mixingRow.variable.isNull()) self(byGuid(mixingRow.variable), true, self);
            }
            if (selection->isQueue() || (selection->mode == SelectionMode::Match
                    && selection->variableResponse() == VariableResponse::Live))
                for (const auto& rule : selection->transitions.rules)
                    if (rule.bridge)
                        self(byGuid(*rule.bridge), true, self);
        }
    };
    visit(&root, true, visit);
    return closure;
}

bool appendActionPrograms(
    const ProjectIndex& index,
    const std::unordered_set<ProjectItem*>& included,
    const std::unordered_set<ProjectItem*>& languageUnavailable,
    const EditorUtils::ActionTargetLookup& lookup,
    BridgeRequest* request, QString* error,
    const CurrentWorldBuildOptions* options, CurrentWorldBuildStatus* status)
{
    std::unordered_set<harkenstudio::ObjectId> unavailableActions;
    for (ProjectItem* item : index.actions) {
        if (item && options && options->includedActionIds
            && options->includedActionIds->count(item->runtimeId()) == 0)
            continue;
        const auto* source = item != nullptr ? item->comp<VAction>() : nullptr;
        if (source == nullptr || harkenstudio::IsNull(item->runtimeId())) {
            setError(error, QStringLiteral("Action definition is invalid"));
            return false;
        }
        harkencurrentworldcompiler::ActionProgramAuthoringV1 action;
        action.actionStableId = item->runtimeId();
        action.steps.reserve(source->steps.size());
        for (std::size_t stepIndex = 0; stepIndex < source->steps.size();
             ++stepIndex) {
            const VActionStep& sourceStep = source->steps[stepIndex];
            const auto opcode = actionOpcode(sourceStep.type);
            if (!opcode || stepIndex > UINT32_MAX) {
                setError(error, QStringLiteral("Action step is invalid"));
                return false;
            }
            harkencurrentworldcompiler::ActionStepAuthoringV1 step;
            step.opcode = *opcode;
            step.delayMilliseconds = sourceStep.delay;
            step.fadeMilliseconds = sourceStep.fadeTime;
            step.value = sourceStep.value;
            const ActionTargetResolution targetResolution = resolveActionTarget(
                sourceStep, *item, index, included, languageUnavailable, lookup,
                &step.target, error, options);
            if (targetResolution == ActionTargetResolution::Skipped)
                continue;
            if (targetResolution == ActionTargetResolution::Invalid) {
                const QString reason = error && !error->isEmpty()
                    ? *error : QStringLiteral("target is invalid");
                const QString target = !sourceStep.targetName.isEmpty()
                    ? sourceStep.targetName
                    : QString::number(sourceStep.targetObjectID);
                setError(error, QStringLiteral(
                    "Action '%1' step %2 (%3 target '%4'): %5")
                    .arg(item->name).arg(stepIndex)
                    .arg(sourceStep.typeString(), target, reason));
                if (options && options->isolateUnavailable) {
                    unavailableActions.insert(action.actionStableId);
                    break;
                }
                return false;
            }

            const bool hasQuantize = sourceStep.type == VActionStep::Type::Play
                || sourceStep.type == VActionStep::Type::Stop
                || sourceStep.type == VActionStep::Type::PostAction;
            if (hasQuantize && sourceStep.quantize.timing != QuantizeTiming::Immediate) {
                const QString target = !sourceStep.targetName.isEmpty()
                    ? sourceStep.targetName
                    : QString::number(sourceStep.targetObjectID);
                if (sourceStep.quantize.timing == QuantizeTiming::EntryMarker
                    || (sourceStep.type == VActionStep::Type::Play
                        && sourceStep.quantize.timing == QuantizeTiming::ExitMarker)) {
                    setStatus(status, CurrentWorldBuildStatus::CapabilityUnavailable);
                    setError(error, QStringLiteral(
                        "Action '%1' step %2 (%3 target '%4'): %5 requires an updated Engine marker scheduling contract")
                        .arg(item->name).arg(stepIndex)
                        .arg(sourceStep.typeString(), target)
                        .arg(sourceStep.quantize.timing == QuantizeTiming::EntryMarker
                                 ? QStringLiteral("Entry Marker") : QStringLiteral("Exit Marker")));
                    return false;
                }
                const auto timing = actionQuantizeTiming(
                    sourceStep.quantize.timing);
                const auto ifSilent = actionIfSilent(
                    sourceStep.quantize.ifSilent);
                if (!timing || !ifSilent) {
                    setError(error, QStringLiteral("Action timing is invalid"));
                    return false;
                }
                if (!harkenstudiobuild::ActionQuantizeTimingSupportedV1(
                        *opcode, *timing)) {
                    setStatus(status, CurrentWorldBuildStatus::CapabilityUnavailable);
                    setError(error, QStringLiteral(
                        "Action '%1' step %2 (%3 target '%4', timing %5): timing is not supported for this opcode")
                        .arg(item->name).arg(stepIndex)
                        .arg(sourceStep.typeString(), target)
                        .arg(static_cast<std::uint32_t>(*timing)));
                    return false;
                }
                step.quantizeTiming = *timing;
                step.ifSilent = *ifSilent;
                const auto referenceResolution = resolveActionQuantizeReference(
                    sourceStep, index, included, languageUnavailable, lookup,
                    &step.quantizeReference, error);
                if (referenceResolution == ActionTargetResolution::Skipped)
                    continue;
                if (referenceResolution == ActionTargetResolution::Invalid) {
                    const QString reason = error && !error->isEmpty()
                        ? *error : QStringLiteral("quantize reference is invalid");
                    const QString reference = !sourceStep.quantize.referenceName.isEmpty()
                        ? sourceStep.quantize.referenceName
                        : QString::number(sourceStep.quantize.referenceObjectID);
                    setError(error, QStringLiteral(
                        "Action '%1' step %2 (%3 quantize reference '%4'): %5")
                        .arg(item->name).arg(stepIndex)
                        .arg(sourceStep.typeString(), reference, reason));
                    if (options && options->isolateUnavailable) {
                        unavailableActions.insert(action.actionStableId);
                        break;
                    }
                    return false;
                }
            }
            if (sourceStep.type == VActionStep::Type::SetVariable) {
                const auto scope = variableWriteScope(sourceStep.scope);
                if (!scope) {
                    setError(error, QStringLiteral("Action variable scope is invalid"));
                    return false;
                }
                step.scope = *scope;
            }
            if (sourceStep.type == VActionStep::Type::Reset) {
                const auto scope = actionResetScope(sourceStep.resetScope);
                if (!scope) {
                    setError(error,
                             QStringLiteral("Action reset scope is invalid"));
                    return false;
                }
                step.resetScope = *scope;
            }
            step.authoredEventIndex =
                static_cast<std::uint32_t>(action.steps.size());
            action.steps.push_back(step);
        }
        if (unavailableActions.count(action.actionStableId) != 0
            || (action.steps.empty()
                && ((options && options->isolateUnavailable)
                    || (!source->steps.empty() && !languageUnavailable.empty())))) {
            unavailableActions.insert(action.actionStableId);
            continue;
        }
        request->actions.push_back(std::move(action));
    }
    bool changed = !unavailableActions.empty();
    while (changed) {
        changed = false;
        for (auto& action : request->actions) {
            if (options && options->isolateUnavailable) {
                if (std::any_of(action.steps.begin(), action.steps.end(), [&](const auto& step) {
                        return step.opcode == harkenstudiobuild::ActionOpcodeV1::PostAction
                            && unavailableActions.count(step.target.stableId) != 0;
                    }))
                    changed = unavailableActions.insert(action.actionStableId).second || changed;
                continue;
            }
            const auto removed = std::erase_if(action.steps, [&](const auto& step) {
                return step.opcode == harkenstudiobuild::ActionOpcodeV1::PostAction
                    && unavailableActions.count(step.target.stableId) != 0;
            });
            if (removed != 0 && action.steps.empty())
                changed = unavailableActions.insert(action.actionStableId).second || changed;
            for (std::size_t index = 0; index < action.steps.size(); ++index)
                action.steps[index].authoredEventIndex = static_cast<std::uint32_t>(index);
        }
    }
    std::erase_if(request->actions, [&](const auto& action) {
        return unavailableActions.count(action.actionStableId) != 0;
    });
    std::sort(request->actions.begin(), request->actions.end(),
              [](const auto& a, const auto& b) {
                  return a.actionStableId < b.actionStableId;
              });
    return true;
}

} // namespace

HarkenEditor::Bridge::CurrentWorldSyncRequestPtr
buildCurrentWorldSyncRequestForContext(ProjectData& project, PlatformScope scope,
                             QString* error,
                             CurrentWorldBuildStatus* status,
                             const CurrentWorldBuildOptions* options,
                             std::uint64_t projectGeneration,
                             const QString& languageFallback)
{
    setStatus(status, CurrentWorldBuildStatus::InvalidAuthoring);
    if (scope && *scope == ProjectPlatformId{}) {
        setError(error, QStringLiteral("Platform ID must not be null"));
        return {};
    }
    if (scope && !ProjectPlatformSemantics::find(project.platforms, *scope)) {
        setError(error, QStringLiteral("Platform ID does not belong to this project"));
        return {};
    }
    if (project.sampleRate <= 0) {
        setError(error, QStringLiteral("Project sample rate is invalid"));
        return {};
    }
    const auto sourceIndex = options && options->canonicalContext
        ? options->canonicalContext->sourceIndex
        : options && options->sourceIndex ? options->sourceIndex
            : HarkenEditor::Bridge::canonicalRuntimeProjectSourceIndex(project);
    std::optional<HarkenEditor::Bridge::CanonicalRuntimeProjectContextV1> ownedCanonical;
    const auto& canonical = options && options->canonicalContext
        ? *options->canonicalContext
        : ownedCanonical.emplace(HarkenEditor::Bridge::canonicalRuntimeProjectContext(
            project, projectGeneration, std::nullopt, scope, sourceIndex));
    if (!canonical.ok()) {
        if (!canonical.compileTargetProfile)
            setStatus(status, CurrentWorldBuildStatus::CapabilityUnavailable);
        setError(error, canonical.diagnostics.empty()
                            ? QStringLiteral("Canonical Runtime project adaptation failed")
                            : canonical.diagnostics.front().message);
        return {};
    }
    const auto sampleRate = canonical.compileTargetProfile->sampleRate;
    auto request = std::make_shared<BridgeRequest>();
    request->project.projectId = runtimeObjectId(QUuid(project.projectId));
    const ProjectIndex& index = *canonical.sourceIndex;
    std::optional<EditorUtils::ActionTargetLookup> ownedActionLookup;
    const auto& actionLookup = options && options->actionLookup
        ? *options->actionLookup
        : ownedActionLookup.emplace(EditorUtils::makeProjectTargetLookup(project));
    ActionBuildClosure closure;
    CurrentWorldBuildOptions scopedOptions;
    const ProjectItem* closureRoot = options
        ? (options->actionRoot ? options->actionRoot : options->definitionRoot)
        : nullptr;
    if (closureRoot) {
        const auto root = index.byObjectId.find(closureRoot->objectID);
        if (root == index.byObjectId.end() || root->second != closureRoot
            || (options->actionRoot
                && closureRoot->type != ProjectItem::Type::Action)) {
            setError(error, QStringLiteral("Current World build root is invalid"));
            return {};
        }
        closure = actionBuildClosure(*closureRoot, index, actionLookup, scope);
        scopedOptions = *options;
        scopedOptions.includedActionIds = &closure.actions;
        scopedOptions.includedDefinitionIds = &closure.definitions;
        options = &scopedOptions;
    }
    request->identityIndex = canonical.identityIndex;
    request->project.identityIndex = request->identityIndex;
    request->compileTargetProfile = canonical.compileTargetProfile;
    request->project.compileTargetProfile = request->compileTargetProfile;
    request->busResult = canonical.busResult;
    std::optional<FieldGroupEffectiveSnapshot> ownedGroups;
    const auto& groups = options && options->fieldGroups
        ? *options->fieldGroups
        : ownedGroups.emplace(buildFieldGroupEffectiveSnapshot(
            project, toFieldGroupScope(scope), *sourceIndex));
    if (!groups.ok()) {
        setError(error, groups.diagnostics.front().message);
        return {};
    }
    std::unordered_set<ProjectItem*> languageUnavailable;
    const auto included = runtimeDefinitions(
        index, project, scope, options, languageFallback, &languageUnavailable);

    int64_t editorOrder = 0;
    uint32_t containerOrder = 0;
    for (ProjectItem* item : index.definitions) {
        if (included.find(item) == included.end())
            continue;
        if (options && options->validatePackResources) {
            RuntimeCapabilityContext resourceContext;
            resourceContext.deferCurrentWorldCompilation = true;
            for (const auto feature : {RuntimeCapabilityFeature::TypedBinding,
                                       RuntimeCapabilityFeature::TypedEffect}) {
                const auto capability = RuntimeCapabilityMatrix::evaluateForContext(
                    *item, project, RuntimeCapabilityOperation::PackBuild,
                    feature, std::nullopt, resourceContext);
                if (capability.state != RuntimeCapabilityState::Ready) {
                    setStatus(status, capability.state == RuntimeCapabilityState::UnavailableEngineContract
                        ? CurrentWorldBuildStatus::CapabilityUnavailable : CurrentWorldBuildStatus::InvalidAuthoring);
                    setError(error, capability.diagnostic);
                    return {};
                }
            }
        }
        if (!buildDefinitionNode(*item, index, included, groups, editorOrder++,
                                 scope, request.get(), error))
            return {};
        const auto kind = contentKind(item->type);
        if (*kind != ContentKind::Layer) {
            harkencurrentworldcompiler::DefinitionMaterializationV1 materialized;
            materialized.kind = runtimePropertyKind(*kind);
            materialized.stableId = item->runtimeId();
            materialized.contentRevision = *kind == ContentKind::Sound
                ? 0 : 1;
            request->materializations.push_back(materialized);
            if (*kind == ContentKind::Sound) {
                request->soundSources.push_back(
                    {item->runtimeId(),
                     SoundSourceEditing::activeSourceId(
                         *item, scope.value_or(ProjectPlatformId{}),
                         soundLanguage(*item, project, options, languageFallback))});
            }
        }
        if (*kind == ContentKind::Container
            && !buildContainerAuthoring(*item, index, included,
                                         containerOrder++, sampleRate, scope,
                                         request.get(), error, status,
                                         !options || !options->includedDefinitionIds
                                             || options->includedDefinitionIds->count(
                                                 item->runtimeId()) != 0))
            return {};
        auto parentIt = index.parentDefinition.find(item);
        ProjectItem* parent = parentIt == index.parentDefinition.end()
            ? nullptr : parentIt->second;
        while (parent != nullptr && included.find(parent) == included.end()) {
            parentIt = index.parentDefinition.find(parent);
            parent = parentIt == index.parentDefinition.end()
                ? nullptr : parentIt->second;
        }
        if (parent == nullptr)
            request->project.roots.push_back(authoringGuid(item->guid));
    }

    std::unordered_set<ProjectItem*> visited;
    bool variablesValid = true;
    const auto collectVariables = [&](ProjectItem* root, const auto& self) -> void {
        if (root == nullptr || !visited.insert(root).second)
            return;
        if (root->type == ProjectItem::Type::Variable
            && (!closureRoot
                || closure.variables.count(root->runtimeId()) != 0)) {
            const auto* source = root->comp<VVariable>();
            if (source == nullptr || harkenstudio::IsNull(root->runtimeId())) {
                setError(error, QStringLiteral("Variable definition is invalid"));
                variablesValid = false;
                return;
            }
            harkenstudiobuild::VariableDefinitionV1 variable;
            variable.variableId = root->runtimeId();
            variable.kind = source->kind == VariableKind::Step
                ? harkenstudiobuild::VariableKindV1::Step : harkenstudiobuild::VariableKindV1::Smooth;
            variable.rangeMin = source->rangeMin;
            variable.rangeMax = source->rangeMax;
            variable.defaultValue = source->defaultValue;
            request->variables.push_back(variable);
        }
        for (const auto& child : root->children)
            self(child.get(), self);
    };
    if (project.root)
        collectVariables(project.root.get(), collectVariables);
    for (ProjectFileBinding* binding : project.allFileBindings()) {
        if (binding != nullptr && binding->rootItem)
            collectVariables(binding->rootItem.get(), collectVariables);
    }
    if (!variablesValid)
        return {};
    std::sort(request->variables.begin(), request->variables.end(),
              [](const auto& a, const auto& b) {
                  return a.variableId < b.variableId;
              });
    if (!appendActionPrograms(index, included, languageUnavailable, actionLookup,
                              request.get(), error, options, status)) {
        return {};
    }
    setStatus(status, CurrentWorldBuildStatus::Ready);
    if (options && options->soundContentRevisions)
        request->soundContentRevisions = *options->soundContentRevisions;
    if (error)
        error->clear();
    return request;
}

bool isCurrentWorldPlayableForContext(ProjectData& project, const ProjectItem& item,
                                     PlatformScope scope, const QString& languageFallback)
{
    const auto canonical =
        HarkenEditor::Bridge::canonicalRuntimeProjectContext(project, 0, std::nullopt, scope);
    if (!canonical.ok())
        return false;
    const auto included = runtimeDefinitions(
        *canonical.sourceIndex, project,
        scope, nullptr, languageFallback);
    return included.find(const_cast<ProjectItem*>(&item)) != included.end();
}

bool readCurrentWorldScalarUpdate(
    ProjectItem& item, PlatformScope scope, std::uint8_t propertyId,
    HarkenEditor::Bridge::CurrentWorldScalarUpdate* output)
{
    using Property = harkenplayableproperties::PlayablePropertyIdV1;
    const auto property = static_cast<Property>(propertyId);
    if (!output || item.type == ProjectItem::Type::Bus
        || (property != Property::Volume && property != Property::Pitch
            && property != Property::MakeUpGain)
        || !PlayablePropertyAuthoringV1::isSlotApplicable(item.type, propertyId))
        return false;
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(item);
    const auto state = locator
        ? PlayablePropertyAuthoringV1::readProperty(item, *locator, propertyId, scope)
        : std::nullopt;
    if (!state || harkenstudio::IsNull(item.runtimeId()))
        return false;
    *output = {authoringGuid(item.guid), item.runtimeId(), propertyId,
               state->present,
               state->present ? static_cast<float>(state->value.toDouble()) : 0.0f};
    return true;
}

bool affectsCurrentWorld(ProjectItemType itemType,
                         CurrentWorldMutationKind mutationKind,
                         QStringView propertyName)
{
    const bool contributesDefinition =
        itemType == ProjectItemType::Layer
        || itemType == ProjectItemType::Sound
        || itemType == ProjectItemType::Container
        || itemType == ProjectItemType::Track
        || itemType == ProjectItemType::Segment
        || itemType == ProjectItemType::Variable
        || itemType == ProjectItemType::Action
        || itemType == ProjectItemType::Bus;
    if (!contributesDefinition)
        return false;
    if (mutationKind == CurrentWorldMutationKind::Structure)
        return true;
    const QString name = propertyName.toString();
    if (PlayablePropertyAuthoringV1::fieldByName(name) != nullptr
        || name == QStringLiteral("fieldGroups")
        || name == QStringLiteral("bindings")
        || name.startsWith(QStringLiteral("bindings."))
        || name == QStringLiteral("effects")) {
        return true;
    }
    switch (itemType) {
        case ProjectItemType::Sound:
            return name == QStringLiteral("sources")
                || name == QStringLiteral("distinguishLanguage");
        case ProjectItemType::Container:
            return name == QStringLiteral("mode")
                || name == QStringLiteral("playType")
                || name == QStringLiteral("variableResponse")
                || name == QStringLiteral("behavior")
                || name == QStringLiteral("childOrder")
                || name == QStringLiteral("selections")
                || name == QStringLiteral("matches")
                || name == QStringLiteral("blends")
                || name == QStringLiteral("transitions");
        case ProjectItemType::Track:
            return name == QStringLiteral("clips");
        case ProjectItemType::Segment:
            return name == QStringLiteral("segmentTime")
                || name == QStringLiteral("markers");
        case ProjectItemType::Variable:
            return name == QStringLiteral("kind")
                || name == QStringLiteral("rangeMin")
                || name == QStringLiteral("rangeMax")
                || name == QStringLiteral("defaultValue");
        case ProjectItemType::Action:
            return name == QStringLiteral("steps");
        default:
            return false;
    }
}

} // namespace VNX
