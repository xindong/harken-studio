#include "AuthoringPolicy.h"

#include "ActionStepUtils.h"
#include "Components/VAction.h"
#include "Components/VSegment.h"
#include "Components/VSelection.h"
#include "Components/VTrack.h"
#include "ProjectTypes.h"

#include <QSet>

#include <algorithm>

namespace VNX::AuthoringPolicy {
namespace {

TimelineValidation validateDependencies(const ProjectItem& item,
                                        const ProjectItem* destinationParent,
                                        const TimelineItemResolver& resolve,
                                        const ObjectResolver& resolveObject,
                                        bool newAuthoring)
{
    QSet<const ProjectItem*> visited;
    std::vector<const ProjectItem*> pending{&item};
    while (!pending.empty()) {
        const ProjectItem* current = pending.back();
        pending.pop_back();
        if (!current || visited.contains(current)) continue;
        visited.insert(current);
        if (const auto* segment = current->comp<VSegment>()) {
            const auto validity = validateSegmentTime(segment->segmentTime);
            if (!validity) return validity;
        }
        if (const auto* track = current->comp<VTrack>()) {
            const ProjectItem* parent = current == &item
                ? destinationParent : current->parent;
            const ProjectItem* sourceParent = current->parent;
            for (const ProjectItem* context : {sourceParent, parent}) {
                const auto* segment = context ? context->comp<VSegment>() : nullptr;
                if (segment) {
                    const auto validity = validateSegmentTime(segment->segmentTime);
                    if (!validity) return validity;
                }
            }
            if (resolve)
                for (const Clip& clip : track->clips.items)
                    pending.push_back(resolve(clip.targetGuid));
        }
        if (const auto* selection = current->comp<VSelection>()) {
            if (newAuthoring) {
                const auto validity = validateTransitions(selection->transitions);
                if (!validity) return validity;
            }
            if (resolve)
                for (const TransitionRuleDef& rule : selection->transitions.rules)
                    for (const auto* reference : {&rule.source, &rule.target, &rule.bridge})
                        if (*reference) pending.push_back(resolve(**reference));
        }
        if (const auto* action = current->comp<VAction>()) {
            const auto reference = [&](uint32_t objectID, const QString& name,
                                        VActionStep::Type stepType,
                                        const QString& field) -> TimelineValidation {
                const ProjectItem* target = objectID != 0 && resolveObject
                    ? resolveObject(objectID) : nullptr;
                if (newAuthoring && (objectID != 0 || !name.isEmpty())
                    && (!target || !EditorUtils::canActionStepReferenceObject(
                        stepType, target->type, target->objectID, current->objectID)))
                    return TimelineValidation::failure(
                        TimelineError::AuthoringRestricted,
                        QStringLiteral("New Action %1 must resolve to a compatible object identity; select the referenced object before authoring it.")
                            .arg(field));
                pending.push_back(target);
                return TimelineValidation::success();
            };
            for (const auto& step : action->steps) {
                const auto target = reference(step.targetObjectID, step.targetName,
                                              step.type, QStringLiteral("target"));
                if (!target) return target;
                const auto* catalogEntry =
                    EditorUtils::actionStepCatalogEntry(step.type);
                if (catalogEntry && catalogEntry->supportsQuantize) {
                    const auto timing = reference(step.quantize.referenceObjectID,
                        step.quantize.referenceName, VActionStep::Type::Play,
                        QStringLiteral("timing reference"));
                    if (!timing) return timing;
                }
            }
        }
        for (const auto& child : current->children)
            pending.push_back(child.get());
    }
    return TimelineValidation::success();
}

}

bool supportsObjectColorEditing()
{
    return false;
}

bool supportsMusicalTime()
{
    return false;
}

QString musicalTimeRestriction()
{
    return QStringLiteral("This version supports absolute-time authoring only; musical time, tempo maps and meter maps are read-only.");
}

const std::vector<TransitionExitTiming>& allowedTransitionExitTimings()
{
    static const std::vector<TransitionExitTiming> timings{
        TransitionExitTiming::Immediate,
        TransitionExitTiming::NaturalEnd,
        TransitionExitTiming::NextMarker,
        TransitionExitTiming::ExitMarker};
    return timings;
}

bool allowsTransitionExitTiming(TransitionExitTiming timing)
{
    const auto& timings = allowedTransitionExitTimings();
    return std::find(timings.begin(), timings.end(), timing) != timings.end();
}

TransitionRuleDef defaultTransitionRule()
{
    auto rule = canonicalTransitionRule();
    rule.exitTiming = TransitionExitTiming::Immediate;
    return rule;
}

TransitionTableDef defaultTransitionTable()
{
    return {{defaultTransitionRule()}};
}

TimelineValidation validateSegmentTime(const SegmentTimeSpec& time)
{
    if (time.domain != TimelineDomain::Absolute
        || !time.tempoMap.empty() || !time.meterMap.empty())
        return TimelineValidation::failure(
            TimelineError::AuthoringRestricted, musicalTimeRestriction());
    return TimelineValidation::success();
}

TimelineValidation validateSegmentTime(const QJsonObject& time)
{
    if (time.value(QStringLiteral("domain")).toString() == QStringLiteral("Musical")
        || !time.value(QStringLiteral("tempoMap")).toArray().isEmpty()
        || !time.value(QStringLiteral("meterMap")).toArray().isEmpty())
        return TimelineValidation::failure(
            TimelineError::AuthoringRestricted, musicalTimeRestriction());
    return TimelineValidation::success();
}

TimelineValidation validateTemplate(const QJsonObject& templateData)
{
    std::vector<QJsonObject> pending;
    for (const auto* section : {"content", "variables", "actions"})
        for (const auto& value : templateData.value(QLatin1String(section)).toArray())
            pending.push_back(value.toObject());
    while (!pending.empty()) {
        const QJsonObject item = pending.back();
        pending.pop_back();
        const auto components = item.value(QStringLiteral("components")).toObject();
        const auto segment = components.value(QStringLiteral("segmenttimeline")).toObject();
        const auto time = validateSegmentTime(segment.value(QStringLiteral("segmentTime")).toObject());
        if (!time) return time;
        const auto selection = components.value(QStringLiteral("selection")).toObject();
        if (item.value(QStringLiteral("type")).toString() == QStringLiteral("Container")
            && !selection.contains(QStringLiteral("transitions")))
            return validateTransitions(canonicalTransitionTable());
        if (selection.contains(QStringLiteral("transitions"))) {
            const auto rules = selection.value(QStringLiteral("transitions")).toObject()
                .value(QStringLiteral("rules")).toArray();
            for (const auto& value : rules) {
                const auto timing = value.toObject().value(QStringLiteral("exitTiming")).toString();
                if (timing == QStringLiteral("NextBeat") || timing == QStringLiteral("NextBar"))
                    return validateTransitions(canonicalTransitionTable());
            }
        }
        for (const auto& child : item.value(QStringLiteral("children")).toArray())
            pending.push_back(child.toObject());
    }
    return TimelineValidation::success();
}

TimelineValidation validateTimeline(const ProjectItem& item,
                                    const TimelineItemResolver& resolve,
                                    const ObjectResolver& resolveObject)
{
    return validateDependencies(item, item.parent, resolve, resolveObject, false);
}

TimelineValidation validateNewSubtree(const ProjectItem& item,
                                      const ProjectItem* destinationParent,
                                      const TimelineItemResolver& resolve,
                                      const ObjectResolver& resolveObject)
{
    if (destinationParent
        && (destinationParent->type == ProjectItem::Type::Track
            || destinationParent->type == ProjectItem::Type::Segment)) {
        const auto context = validateTimeline(*destinationParent, resolve, resolveObject);
        if (!context) return context;
    }
    return validateDependencies(item, destinationParent, resolve, resolveObject, true);
}

TimelineValidation validateTransitions(const TransitionTableDef& replacement,
                                       const TransitionTableDef* previous)
{
    for (size_t index = 0; index < replacement.rules.size(); ++index) {
        const auto& rule = replacement.rules[index];
        if (allowsTransitionExitTiming(rule.exitTiming)) continue;
        if (previous && std::find(previous->rules.begin(), previous->rules.end(), rule)
                != previous->rules.end()) continue;
        return TimelineValidation::failure(
            TimelineError::AuthoringRestricted,
            QStringLiteral("This version does not support authoring NextBeat or NextBar transitions."),
            static_cast<int>(index));
    }
    return TimelineValidation::success();
}

TimelineValidation validateTransitionActivation(const ProjectItem& container,
                                                const VSelection& replacement,
                                                PlatformScope scope)
{
    const auto* previous = container.comp<VSelection>();
    if (!previous || TransitionSemantics::transitionMountState(container, scope)
            == TransitionMountState::Editable)
        return TimelineValidation::success();
    ProjectItem candidate(container.name, container.type);
    auto* selection = candidate.compOrCreate<VSelection>();
    *selection = replacement;
    if (TransitionSemantics::transitionMountState(candidate, scope) != TransitionMountState::Editable)
        return TimelineValidation::success();
    return validateTransitions(previous->transitions);
}

}
