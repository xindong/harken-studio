#pragma once

#include "Core/ProjectTypes.h"
#include "Core/ItemTypeCatalog.h"

#include <QHash>

namespace VNX {

class ProjectObjectLabels
{
public:
    explicit ProjectObjectLabels(const ProjectData& project)
    {
        const auto visit = [&](auto&& self, const ProjectItem* item, const QString& parentPath) -> void {
            if (!item) return;
            const QString path = parentPath + QLatin1Char('/') + item->name;
            paths_.insert(item, path);
            const QString nameKey = item->name.normalized(QString::NormalizationForm_C);
            const QString pathKey = path.normalized(QString::NormalizationForm_C);
            ++names_[nameKey];
            ++foldedNames_[nameKey.toCaseFolded()];
            ++pathCounts_[pathKey];
            ++foldedPathCounts_[pathKey.toCaseFolded()];
            for (const auto& child : item->children) self(self, child.get(), path);
        };
        for (const auto* binding : project.allFileBindings()) {
            if (!binding || !binding->rootItem) continue;
            QString parentPath = binding->parentFolder;
            const QString folderPath = project.itemFolderPath(binding->rootItem.get());
            if (!folderPath.isEmpty())
                parentPath += QLatin1Char('/') + folderPath;
            visit(visit, binding->rootItem.get(), parentPath);
        }
        const auto visitFolder = [&](auto&& self, const PhysicalFolder* folder, const QString& parentPath) -> void {
            if (!folder) return;
            const QString path = parentPath + QLatin1Char('/') + folder->name;
            folderPaths_.insert(folder, path);
            const QString nameKey = folder->name.normalized(QString::NormalizationForm_C);
            const QString pathKey = path.normalized(QString::NormalizationForm_C);
            ++names_[nameKey];
            ++foldedNames_[nameKey.toCaseFolded()];
            ++pathCounts_[pathKey];
            ++foldedPathCounts_[pathKey.toCaseFolded()];
            for (const auto& child : folder->children) self(self, child.get(), path);
        };
        for (const auto& binding : project.folders)
            visitFolder(visitFolder, binding.rootFolder.get(), binding.parentFolder);
    }

    QString path(const ProjectItem& item) const
    {
        return paths_.value(&item);
    }

    QString label(const ProjectItem& item) const
    {
        const bool insensitive = item.type == ProjectItem::Type::Attenuation;
        const QString nameKey = item.name.normalized(QString::NormalizationForm_C);
        if ((insensitive ? foldedNames_.value(nameKey.toCaseFolded()) : names_.value(nameKey)) < 2)
            return item.name;
        const QString path = paths_.value(&item);
        if (path.isEmpty()) return item.name;
        const QString pathKey = path.normalized(QString::NormalizationForm_C);
        if ((insensitive ? foldedPathCounts_.value(pathKey.toCaseFolded()) : pathCounts_.value(pathKey)) < 2)
            return path;
        return path + QStringLiteral(" [%1]").arg(item.guid.toString(QUuid::WithoutBraces));
    }

    QString label(const PhysicalFolder& folder) const
    {
        const QString nameKey = folder.name.normalized(QString::NormalizationForm_C).toCaseFolded();
        return foldedNames_.value(nameKey) < 2 ? folder.name : folderPaths_.value(&folder, folder.name);
    }

private:
    QHash<const ProjectItem*, QString> paths_;
    QHash<const PhysicalFolder*, QString> folderPaths_;
    QHash<QString, int> names_;
    QHash<QString, int> foldedNames_;
    QHash<QString, int> pathCounts_;
    QHash<QString, int> foldedPathCounts_;
};

}
