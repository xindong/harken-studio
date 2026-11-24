#include "AuthoringPolicy.h"

#include "ActionStepUtils.h"
#include "Components/VAction.h"
#include "Components/VTrack.h"
#include "ProjectTypes.h"

#include <QSet>

#include <algorithm>

namespace harken::studio::AuthoringPolicy {
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
        if (const auto* track = current->comp<VTrack>()) {
            if (resolve)
                for (const Clip& clip : track->clips.items)
                    pending.push_back(resolve(clip.targetGuid));
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

}
