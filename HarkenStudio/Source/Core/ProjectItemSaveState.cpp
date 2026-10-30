#include "ProjectItemSaveState.h"

#include "ProjectSerializer.h"
#include "ProjectTypes.h"

namespace VNX {
namespace {

QJsonObject persistentState(const ProjectItem& item, QJsonObject state)
{
    if (state.isEmpty()) return {};
    QJsonArray children;
    for (const auto& child : item.children)
        children.append(child->guid.toString(QUuid::WithoutBraces));
    state.insert(QStringLiteral("children"), children);
    return state;
}

}

ProjectItemSaveState::ProjectItemSaveState() = default;
ProjectItemSaveState::~ProjectItemSaveState() = default;

bool ProjectItemSaveState::rebuildIndex(const ProjectData& project)
{
    auto serializer = std::make_unique<ProjectSerializer>();
    if (!serializer->prepareReferenceWrite(project)) return false;
    QHash<QUuid, ProjectItem*> index;
    QList<ProjectItem*> items;
    const auto visit = [&](const auto& self, ProjectItem& item) -> void {
        index.insert(item.guid, &item);
        items.append(&item);
        for (const auto& child : item.children)
            self(self, *child);
    };
    for (const auto* binding : project.allFileBindings())
        visit(visit, *binding->rootItem);
    serializer_ = std::move(serializer);
    index_ = std::move(index);
    items_ = std::move(items);
    return true;
}

bool ProjectItemSaveState::ensurePrepared(const ProjectData& project)
{
    return serializer_ || rebuildIndex(project);
}

ProjectSerializer& ProjectItemSaveState::serializer()
{
    return *serializer_;
}

ProjectItem* ProjectItemSaveState::findItem(const QUuid& guid) const
{
    return index_.value(guid, nullptr);
}

bool ProjectItemSaveState::markSaved(const ProjectData& project)
{
    ProjectSerializer serializer;
    if (!serializer.prepareReferenceWrite(project)) return false;
    return markSaved(project, serializer, {});
}

bool ProjectItemSaveState::markSaved(
    const ProjectData& project, const ProjectSerializer& serializer,
    const QHash<QUuid, QJsonObject>& serializedItems)
{
    QHash<QUuid, QJsonObject> next;
    const auto visit = [&](const auto& self, const ProjectItem& item) -> bool {
        const auto serialized = serializedItems.constFind(item.guid);
        auto state = persistentState(item, serialized == serializedItems.cend()
            ? serializer.projectItemToJson(item, false) : *serialized);
        if (state.isEmpty()) return false;
        next.insert(item.guid, std::move(state));
        for (const auto& child : item.children)
            if (!self(self, *child)) return false;
        return true;
    };
    for (const auto* binding : project.allFileBindings())
        if (!visit(visit, *binding->rootItem)) return false;
    if (!rebuildIndex(project)) return false;
    saved_ = std::move(next);
    pending_.clear();
    candidates_.clear();
    requiresFullRefresh_ = false;
    requiresFullSave_ = false;
    return true;
}

bool ProjectItemSaveState::acceptSaved(
    const ProjectData& project, const QSet<QUuid>& accepted,
    const ProjectSerializer& serializer,
    const QHash<QUuid, QJsonObject>& serializedItems)
{
    QHash<QUuid, QJsonObject> states;
    if (!prepareAccepted(project, accepted, serializer, serializedItems, &states)) return false;
    publishAccepted(states);
    return true;
}

bool ProjectItemSaveState::prepareAccepted(
    const ProjectData& project, const QSet<QUuid>& accepted,
    const ProjectSerializer& serializer,
    const QHash<QUuid, QJsonObject>& serializedItems,
    QHash<QUuid, QJsonObject>* states)
{
    if (!states || !ensurePrepared(project)) return false;
    QHash<QUuid, QJsonObject> next;
    for (const auto& guid : accepted) {
        const auto* item = findItem(guid);
        if (!item) return false;
        const auto serialized = serializedItems.constFind(guid);
        auto state = persistentState(*item, serialized == serializedItems.cend()
            ? serializer.projectItemToJson(*item, false) : *serialized);
        if (state.isEmpty()) return false;
        next.insert(guid, std::move(state));
    }
    *states = std::move(next);
    return true;
}

void ProjectItemSaveState::publishAccepted(const QHash<QUuid, QJsonObject>& states)
{
    for (auto it = states.cbegin(); it != states.cend(); ++it) {
        saved_.insert(it.key(), it.value());
        pending_.remove(it.key());
        candidates_.remove(it.key());
    }
}

void ProjectItemSaveState::recordChange(ProjectItem* item)
{
    if (!item) return;
    if (findItem(item->guid) != item) invalidate();
    recordPreview(item->guid);
}

void ProjectItemSaveState::recordPreview(const QUuid& guid)
{
    pending_.insert(guid);
    candidates_.insert(guid);
    ++revision_;
}

void ProjectItemSaveState::invalidate()
{
    serializer_.reset();
    index_.clear();
    items_.clear();
    requiresFullRefresh_ = true;
    requiresFullSave_ = true;
    ++revision_;
}

QList<ProjectItem*> ProjectItemSaveState::refreshItems(const QList<ProjectItem*>& items)
{
    QList<ProjectItem*> changed;
    bool complete = true;
    for (auto* item : items) {
        const auto state = persistentState(*item, serializer_->projectItemToJson(*item, false));
        if (state.isEmpty()) {
            complete = false;
            candidates_.insert(item->guid);
            continue;
        }
        const auto saved = saved_.constFind(item->guid);
        if (saved == saved_.cend() || *saved != state)
            candidates_.insert(item->guid);
        else
            candidates_.remove(item->guid);
        const bool dirty = saved == saved_.cend()
            || *saved != state;
        if (item->dirtyBit != dirty) {
            item->dirtyBit = dirty;
            changed.append(item);
        }
        pending_.remove(item->guid);
    }
    requiresFullRefresh_ = !complete;
    if (!complete) requiresFullSave_ = true;
    return changed;
}

QList<ProjectItem*> ProjectItemSaveState::refresh(
    ProjectData& project, const QSet<QUuid>* authoringWriteSet)
{
    if (!rebuildIndex(project)) {
        invalidate();
        return {};
    }
    if (!authoringWriteSet || requiresFullRefresh_) {
        const auto changed = refreshItems(items_);
        if (!requiresFullRefresh_) pending_.clear();
        return changed;
    }
    auto pending = *authoringWriteSet | pending_;
    QList<ProjectItem*> candidates;
    for (auto* item : items_)
        if (pending.remove(item->guid) || item->dirtyBit)
            candidates.append(item);
    const auto changed = refreshItems(pending.isEmpty() ? candidates : items_);
    if (!requiresFullRefresh_) pending_.clear();
    return changed;
}

QList<ProjectItem*> ProjectItemSaveState::refreshPending(ProjectData& project)
{
    if (!ensurePrepared(project)) {
        invalidate();
        return {};
    }
    if (requiresFullRefresh_) {
        const auto changed = refreshItems(items_);
        if (!requiresFullRefresh_) pending_.clear();
        return changed;
    }
    QList<ProjectItem*> items;
    items.reserve(pending_.size());
    for (const auto& guid : pending_) {
        auto* item = findItem(guid);
        if (!item) {
            invalidate();
            return refresh(project);
        }
        items.append(item);
    }
    return refreshItems(items);
}

}
