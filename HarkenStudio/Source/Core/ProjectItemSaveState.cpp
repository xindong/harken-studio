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
    pendingPreviews_.clear();
    requiresFullRefresh_ = false;
    return true;
}

void ProjectItemSaveState::recordPreview(const QUuid& guid)
{
    pendingPreviews_.insert(guid);
}

void ProjectItemSaveState::invalidate()
{
    requiresFullRefresh_ = true;
}

QList<ProjectItem*> ProjectItemSaveState::refresh(
    ProjectData& project, const QSet<QUuid>* authoringWriteSet)
{
    QList<ProjectItem*> changed;
    ProjectSerializer serializer;
    if (!serializer.prepareReferenceWrite(project)) {
        requiresFullRefresh_ = true;
        return changed;
    }
    const bool scoped = authoringWriteSet && !requiresFullRefresh_;
    QSet<QUuid> pending = scoped ? *authoringWriteSet | pendingPreviews_ : QSet<QUuid>{};
    QList<ProjectItem*> items;
    QList<ProjectItem*> candidates;
    const auto visit = [&](const auto& self, ProjectItem& item) -> void {
        items.append(&item);
        if (scoped && (pending.remove(item.guid) || item.dirtyBit))
            candidates.append(&item);
        for (const auto& child : item.children)
            self(self, *child);
    };
    for (const auto* binding : project.allFileBindings())
        visit(visit, *binding->rootItem);
    bool complete = true;
    for (auto* item : scoped && pending.isEmpty() ? candidates : items) {
        const auto state = authoringState(*item, serializer);
        if (!state.isEmpty()) {
            const auto saved = saved_.constFind(item->guid);
            const bool dirty = saved == saved_.cend() || *saved != state;
            if (item->dirtyBit != dirty) {
                item->dirtyBit = dirty;
                changed.append(item);
            }
        } else {
            complete = false;
        }
    }
    if (complete) pendingPreviews_.clear();
    requiresFullRefresh_ = !complete;
    return changed;
}

}
