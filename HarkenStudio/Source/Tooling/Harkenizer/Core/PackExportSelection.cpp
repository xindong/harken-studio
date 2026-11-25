#include "Tooling/Harkenizer/Core/PackExportSelection.h"

#include "ProjectLanguageSemantics.h"
#include "ProjectTypes.h"
#include "ProjectUserData.h"

#include <QDebug>
#include <QFileInfo>
#include <QSet>
#include <algorithm>
#include <set>

namespace harken::studio {
namespace {

bool readSelection(const QJsonObject& root, PackExportSelection& selection)
{
    if (!root.value(QStringLiteral("selectedPlatformIds")).isArray()
        || !root.value(QStringLiteral("actionTargeting")).isArray()) return false;
    PackExportSelection loaded;
    loaded.currentPlatformId = root.value(QStringLiteral("currentPlatformId")).toString();
    for (const auto& value : root.value(QStringLiteral("selectedPlatformIds")).toArray()) {
        if (!value.isString()) return false;
        loaded.selectedPlatformIds.append(value.toString());
    }
    for (const auto& value : root.value(QStringLiteral("actionTargeting")).toArray()) {
        const auto row = value.toObject();
        if (!row.value(QStringLiteral("allPlatforms")).isBool()
            || !row.value(QStringLiteral("platformIds")).isArray()
            || (row.contains(QStringLiteral("language")) && !row.value(QStringLiteral("language")).isString()))
            return false;
        for (const auto& platform : row.value(QStringLiteral("platformIds")).toArray())
            if (!platform.isString()) return false;
        auto entry = ActionPackTargeting::fromJson(row);
        if (harken::studio::types::IsNull(entry.stableId)) return false;
        loaded.actionTargeting.push_back(std::move(entry));
    }
    selection = std::move(loaded);
    return true;
}

void reconcile(const ProjectData& project, PackExportSelection& selection)
{
    const auto platforms = project.platformIds();
    if (!platforms.contains(selection.currentPlatformId))
        selection.currentPlatformId = platforms.value(0);
    selection.selectedPlatformIds.removeDuplicates();
    selection.selectedPlatformIds.removeIf([&](const auto& id) {
        return !platforms.contains(id);
    });
    std::set<harken::studio::types::ObjectId> actions;
    const auto visit = [&](const auto& self, const ProjectItem* item) -> void {
        if (!item) return;
        if (item->type == ProjectItem::Type::Action) actions.insert(item->runtimeId());
        for (const auto& child : item->children) self(self, child.get());
    };
    for (const auto* binding : project.allFileBindings()) visit(visit, binding->rootItem.get());
    const auto languages = ProjectLanguageSemantics::buildLanguages(project);
    std::erase_if(selection.actionTargeting, [&](const auto& entry) {
        return !actions.contains(entry.stableId)
            || (entry.language && !entry.language->isEmpty() && !languages.contains(*entry.language));
    });
    for (auto& entry : selection.actionTargeting) {
        entry.platformIds.removeDuplicates();
        entry.platformIds.removeIf([&](const auto& id) { return !platforms.contains(id); });
    }
}

}

bool PackExportSelection::includesActionPack(harken::studio::types::ObjectId id,
                                             const QString& platform,
                                             const QString& language) const
{
    const ActionPackTargeting* inherited = nullptr;
    for (const auto& entry : actionTargeting) {
        if (entry.stableId != id) continue;
        if (entry.language == language) return entry.includesPlatform(platform);
        if (!entry.language) inherited = &entry;
    }
    return !inherited || inherited->includesPlatform(platform);
}

PackExportSelectionStore& PackExportSelectionStore::instance()
{
    static PackExportSelectionStore store;
    return store;
}

void PackExportSelectionStore::openProject(const ProjectData& project)
{
    closeProject();
    projectRoot_ = QFileInfo(project.projectPath).absolutePath();
    selection_.selectedPlatformIds = project.platformIds();
    selection_.currentPlatformId = project.platformIds().value(0);
    ProjectUserData::load(projectRoot_, QLatin1String(ProjectUserData::kPackExportSelection),
        [&](const QJsonObject& data) {
            if (!readSelection(data, selection_)) return false;
            reconcile(project, selection_);
            return true;
        }, QStringLiteral("PackExportSelection/%1").arg(project.projectId));
}

void PackExportSelectionStore::relocateProject(const ProjectData& project)
{
    projectRoot_ = QFileInfo(project.projectPath).absolutePath();
    save();
}

void PackExportSelectionStore::closeProject()
{
    projectRoot_.clear();
    selection_ = {};
}

const PackExportSelection& PackExportSelectionStore::selection() const
{
    return selection_;
}

void PackExportSelectionStore::setSelection(const ProjectData& project,
                                            PackExportSelection selection)
{
    reconcile(project, selection);
    if (selection_ == selection) return;
    selection_ = std::move(selection);
    save();
}

void PackExportSelectionStore::save() const
{
    if (projectRoot_.isEmpty()) return;
    QJsonArray targeting;
    for (const auto& row : selection_.actionTargeting) targeting.append(row.toJson());
    if (!ProjectUserData::write(projectRoot_, QLatin1String(ProjectUserData::kPackExportSelection), {
            {QStringLiteral("currentPlatformId"), selection_.currentPlatformId},
            {QStringLiteral("selectedPlatformIds"), QJsonArray::fromStringList(selection_.selectedPlatformIds)},
            {QStringLiteral("actionTargeting"), targeting}}))
        qWarning() << "Failed to save pack export selection:" << projectRoot_;
}

}
