#include "ProjectItemSaveState.h"

#include "ProjectSerializer.h"
#include "ProjectTypes.h"

namespace VNX {
namespace {

QJsonObject authoringState(const ProjectItem& item, const ProjectSerializer& serializer)
{
    auto state = serializer.projectItemToJson(item, false);
    if (state.isEmpty()) return {};
    state.remove(QStringLiteral("expanded"));
    state.remove(QStringLiteral("objectPath"));
    state.remove(QStringLiteral("nameBindingRevision"));
    QJsonArray children;
    for (const auto& child : item.children)
        children.append(child->guid.toString(QUuid::WithoutBraces));
    state.insert(QStringLiteral("children"), children);
    return state;
}

}

bool ProjectItemSaveState::markSaved(const ProjectData& project)
{
    ProjectSerializer serializer;
    if (!serializer.prepareReferenceWrite(project)) return false;
    QHash<QUuid, QJsonObject> next;
    const auto visit = [&](const auto& self, const ProjectItem& item) -> bool {
        auto state = authoringState(item, serializer);
        if (state.isEmpty()) return false;
        next.insert(item.guid, std::move(state));
        for (const auto& child : item.children)
            if (!self(self, *child)) return false;
        return true;
    };
    for (const auto* binding : project.allFileBindings())
        if (!visit(visit, *binding->rootItem)) return false;
    saved_ = std::move(next);
    return true;
}

QList<ProjectItem*> ProjectItemSaveState::refresh(ProjectData& project) const
{
    QList<ProjectItem*> changed;
    ProjectSerializer serializer;
    if (!serializer.prepareReferenceWrite(project)) return changed;
    const auto visit = [&](const auto& self, ProjectItem& item) -> void {
        const auto state = authoringState(item, serializer);
        if (!state.isEmpty()) {
            const auto saved = saved_.constFind(item.guid);
            const bool dirty = saved == saved_.cend() || *saved != state;
            if (item.dirtyBit != dirty) {
                item.dirtyBit = dirty;
                changed.append(&item);
            }
        }
        for (const auto& child : item.children)
            self(self, *child);
    };
    for (const auto* binding : project.allFileBindings())
        visit(visit, *binding->rootItem);
    return changed;
}

}
