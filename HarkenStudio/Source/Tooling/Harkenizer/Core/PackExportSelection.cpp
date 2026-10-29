#include "Tooling/Harkenizer/Core/PackExportSelection.h"

#include "AssetPipeline/PathUtils.h"
#include "ProjectLanguageSemantics.h"
#include "ProjectTypes.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <algorithm>
#include <set>

namespace VNX {
namespace {

void reconcile(const ProjectData& project, PackExportSelection& selection)
{
    const auto platforms = project.platformIds();
    if (!platforms.contains(selection.currentPlatformId))
        selection.currentPlatformId = platforms.value(0);
    selection.selectedPlatformIds.removeDuplicates();
    selection.selectedPlatformIds.removeIf([&](const auto& id) {
        return !platforms.contains(id);
    });
    std::set<harkenstudio::ObjectId> actions;
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

bool PackExportSelection::includesActionPack(harkenstudio::ObjectId id,
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
    cachePath_ = QDir(PathUtils::getDotCacheDir(QFileInfo(project.projectPath).absolutePath()))
        .filePath(QStringLiteral("pack-export-selection.json"));
    selection_.selectedPlatformIds = project.platformIds();
    selection_.currentPlatformId = project.platformIds().value(0);
    QFile file(cachePath_);
    if (!file.open(QIODevice::ReadOnly)) return;
    const auto document = QJsonDocument::fromJson(file.readAll());
    const auto root = document.object();
    if (!document.isObject() || root.value(QStringLiteral("version")).toInt() != 1
        || !root.value(QStringLiteral("selectedPlatformIds")).isArray()
        || !root.value(QStringLiteral("actionTargeting")).isArray()) return;
    PackExportSelection loaded;
    loaded.currentPlatformId = root.value(QStringLiteral("currentPlatformId")).toString();
    for (const auto& value : root.value(QStringLiteral("selectedPlatformIds")).toArray()) {
        if (!value.isString()) return;
        loaded.selectedPlatformIds.append(value.toString());
    }
    for (const auto& value : root.value(QStringLiteral("actionTargeting")).toArray()) {
        const auto row = value.toObject();
        if (!value.isObject() || !row.value(QStringLiteral("allPlatforms")).isBool()
            || !row.value(QStringLiteral("platformIds")).isArray()
            || (row.contains(QStringLiteral("language"))
                && !row.value(QStringLiteral("language")).isString())) return;
        for (const auto& platform : row.value(QStringLiteral("platformIds")).toArray())
            if (!platform.isString()) return;
        auto entry = ActionPackTargeting::fromJson(row);
        if (harkenstudio::IsNull(entry.stableId)) return;
        loaded.actionTargeting.push_back(std::move(entry));
    }
    reconcile(project, loaded);
    selection_ = std::move(loaded);
}

void PackExportSelectionStore::closeProject()
{
    cachePath_.clear();
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
    if (cachePath_.isEmpty()) return;
    QJsonArray targeting;
    for (const auto& row : selection_.actionTargeting) targeting.append(row.toJson());
    const QJsonObject root{{QStringLiteral("version"), 1},
        {QStringLiteral("currentPlatformId"), selection_.currentPlatformId},
        {QStringLiteral("selectedPlatformIds"), QJsonArray::fromStringList(selection_.selectedPlatformIds)},
        {QStringLiteral("actionTargeting"), targeting}};
    const auto bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
    QSaveFile file(cachePath_);
    if (!QDir().mkpath(QFileInfo(cachePath_).absolutePath()) || !file.open(QIODevice::WriteOnly)
        || file.write(bytes) != bytes.size() || !file.commit())
        qWarning() << "Failed to save pack export selection cache:" << cachePath_;
}

}
