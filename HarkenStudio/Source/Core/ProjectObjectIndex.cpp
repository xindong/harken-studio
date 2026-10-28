#include "ProjectObjectIndex.h"

#include "ProjectTypes.h"
#include "Components/VMixNode.h"
#include "Components/VSegment.h"
#include "Components/VTrack.h"

#include <QSet>

#include <functional>

namespace VNX {

ProjectObjectIndex ProjectObjectIndex::fromProject(const ProjectData* project,
                                                   const ProjectItem* excludedSubtree)
{
    ProjectObjectIndex index;
    if (!project) return index;
    index.projectGuid_ = QUuid(project->projectId);
    if (index.projectGuid_.isNull()) return index;
    QHash<QUuid, const ProjectItem*> items;
    QSet<const ProjectItem*> visited;
    QSet<const ProjectItem*> excluded;
    std::function<void(const ProjectItem*)> exclude = [&](const ProjectItem* item) {
        if (!item || excluded.contains(item)) return;
        excluded.insert(item);
        for (const auto& child : item->children) exclude(child.get());
    };
    exclude(excludedSubtree);
    QSet<harkenstudio::ObjectId> ambiguous;
    auto add = [&](harkenstudio::ObjectId id, const ProjectItem& owner,
                   ProjectObjectLocation::Kind kind, const QString& name) {
        if (harkenstudio::IsNull(id) || id == harkenstudio::kMasterBusObjectId
            || runtimeObjectId(owner.guid) == harkenstudio::kMasterBusObjectId
            || ambiguous.contains(id)) return;
        if (index.objects_.contains(id)) {
            index.objects_.remove(id);
            ambiguous.insert(id);
            return;
        }
        auto businessKind = ProjectObjectLocation::BusinessKind::None;
        if (kind == ProjectObjectLocation::Kind::Item) {
            switch (owner.type) {
            case ProjectItem::Type::Action: businessKind = ProjectObjectLocation::BusinessKind::Action; break;
            case ProjectItem::Type::Variable: businessKind = ProjectObjectLocation::BusinessKind::Variable; break;
            case ProjectItem::Type::Sound:
            case ProjectItem::Type::Container:
            case ProjectItem::Type::Track:
            case ProjectItem::Type::Segment: businessKind = ProjectObjectLocation::BusinessKind::Playable; break;
            default: break;
            }
        }
        index.objects_.insert(id, {kind, owner.guid, owner.objectID, name, businessKind});
    };
    std::function<void(const ProjectItem*)> collect = [&](const ProjectItem* item) {
        if (!item || visited.contains(item) || excluded.contains(item)) return;
        visited.insert(item);
        items.insert(item->guid, item);
        add(item->runtimeId(), *item, ProjectObjectLocation::Kind::Item, item->name);
        for (const auto& child : item->children) collect(child.get());
    };
    collect(project->root.get());
    for (const auto* binding : project->allFileBindings())
        if (binding) collect(binding->rootItem.get());
    for (const auto* item : visited) {
        if (const auto* mix = item->comp<VMixNode>()) {
            for (const auto& binding : mix->bindings.entries()) {
                if (!binding.typed) continue;
                add(binding.runtimeId(), *item, ProjectObjectLocation::Kind::Binding,
                    item->name + QStringLiteral(" / ") + BindingSemantics::addressKey(binding.address));
            }
            for (const auto& effect : mix->effects.entries()) {
                if (!effect.typed) continue;
                add(effect.runtimeId(), *item, ProjectObjectLocation::Kind::Effect,
                    item->name + QStringLiteral(" / ") + EffectChainSemantics::typeName(effect.type));
            }
        }
        if (const auto* timeline = item->comp<VTrack>()) {
            for (const auto& clip : timeline->clips.items) {
                const auto* target = items.value(clip.targetGuid);
                add(clip.runtimeId(), *item, ProjectObjectLocation::Kind::Clip,
                    item->name + QStringLiteral(" / ") +
                    (target ? target->name : clip.targetGuid.toString(QUuid::WithoutBraces)));
            }
        }
        if (const auto* timeline = item->comp<VSegment>()) {
            for (const auto& marker : timeline->markers.items)
                add(marker.runtimeId(), *item, ProjectObjectLocation::Kind::Marker,
                    item->name + QStringLiteral(" / ") + marker.name);
        }
    }
    for (auto it = index.objects_.begin(); it != index.objects_.end();) {
        if (ambiguous.contains(runtimeObjectId(it->ownerGuid)))
            it = index.objects_.erase(it);
        else
            ++it;
    }
    return index;
}

}
