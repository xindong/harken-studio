
#include "RefDomainAdapter.h"

#include "SelectorWindow.h"
#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/AuthoringPolicy.h"
#include "Core/AssetPipeline/PathUtils.h"
#include "Core/ItemTypeIcons.h"
#include "Core/ProjectManager.h"
#include "Core/ThemeAssetCatalog.h"
#include "Core/ThemeManager.h"
#include "Widgets/Shared/AudioAssetLabels.h"
#include "Widgets/Shared/TreeRowStyle.h"
#include "Widgets/Shared/TypeFilter.h"
#include "Widgets/Shared/UiHiddenBucket.h"

#include <QHash>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QtGlobal>

#include <algorithm>
#include <functional>

namespace VNX {
namespace {

class ProjectObjectAdapter final : public IRefDomainAdapter
{
public:
    bool exists(uint32_t id) const override
    {
        return ProjectManager::instance().findItemByObjectID(id) != nullptr;
    }

    QString displayName(uint32_t id) const override
    {
        ProjectItem* item = ProjectManager::instance().findItemByObjectID(id);
        return item ? item->name : QString();
    }

    QString tooltip(uint32_t id) const override
    {
        ProjectItem* item = ProjectManager::instance().findItemByObjectID(id);
        if (!item)
            return {};
        const auto* project = ProjectManager::instance().currentProject();
        return project ? project->itemPath(item) : item->name;
    }

    QIcon icon(uint32_t id, const QColor& tint) const override
    {
        ProjectItem* item = ProjectManager::instance().findItemByObjectID(id);
        if (!item)
            return {};
        return tint.isValid() ? iconForItem(*item, tint) : iconForItem(*item);
    }

    bool matchesFilter(uint32_t id, const RefFilter& filter) const override
    {
        ProjectItem* item = ProjectManager::instance().findItemByObjectID(id);
        if (!item)
            return false;
        return TypeFilter::matches(item->type, filter.single, filter.multi)
            && (filter.directChildParentGuid.isNull()
                || (item->parent && item->parent->guid == filter.directChildParentGuid))
            && AuthoringPolicy::validateNewSubtree(*item, nullptr, [](const QUuid& guid) {
                return ProjectManager::instance().findItemByGuid(guid);
            }, [](uint32_t objectID) {
                return ProjectManager::instance().findItemByObjectID(objectID);
            });
    }

    QList<QMetaObject::Connection> connectChanged(
        QObject* ctx, std::function<void(uint32_t)> onChanged) const override
    {
        auto* pm = &ProjectManager::instance();
        return {
            QObject::connect(
                pm, &ProjectManager::itemPropertyChanged, ctx,
                [cb = onChanged](ProjectItem* item, const QString& property, const QObject*) {
                    if (item && (property == QStringLiteral("mode")
                                 || property == QStringLiteral("playType")
                                 || property == QStringLiteral("kind")))
                        cb(item->objectID);
                }),
            QObject::connect(
                pm, &ProjectManager::itemRenamed, ctx,
                [cb = onChanged](ProjectItem*, const QString&) { cb(0); }),
            QObject::connect(pm, &ProjectManager::itemCreated, ctx,
                [cb = onChanged](ProjectItem*) { cb(0); }),
            QObject::connect(pm, &ProjectManager::itemDeleted, ctx,
                [cb = onChanged](ProjectItem*) { cb(0); }, Qt::QueuedConnection),
            QObject::connect(pm, &ProjectManager::itemMoved, ctx,
                [cb = std::move(onChanged)](ProjectItem*, ProjectItem*, ProjectItem*) { cb(0); }),
        };
    }

    void populate(QTreeWidget* tree, const SelectorRequest& request) const override
    {
        auto* project = ProjectManager::instance().currentProject();
        if (!project)
            return;

        std::function<QTreeWidgetItem*(ProjectItem*)> build =
            [&](ProjectItem* item) -> QTreeWidgetItem* {
            std::vector<QTreeWidgetItem*> childRows;
            if (request.directChildParentGuid.isNull() || item->guid == request.directChildParentGuid) {
                for (const auto& child : item->children)
                    if (auto* row = build(child.get()))
                        childRows.push_back(row);
            }

            const bool matchesFilter =
                TypeFilter::matches(item->type, request.filterType, request.filterTypes)
                && (request.directChildParentGuid.isNull()
                    || (item->parent && item->parent->guid == request.directChildParentGuid));
            if (!matchesFilter && childRows.empty())
                return nullptr;

            const TimelineValidation authoring = request.onToggled
                ? TimelineValidation::success()
                : AuthoringPolicy::validateNewSubtree(*item, nullptr, [](const QUuid& guid) {
                    return ProjectManager::instance().findItemByGuid(guid);
                }, [](uint32_t objectID) {
                    return ProjectManager::instance().findItemByObjectID(objectID);
                });
            const bool selectable = matchesFilter && authoring
                && (!request.isSelectable || request.isSelectable(item->objectID));

            auto* row = new QTreeWidgetItem;
            row->setText(0, item->name);
            row->setToolTip(0, tooltip(item->objectID));
            if (!authoring)
                row->setToolTip(0, row->toolTip(0) + QLatin1Char('\n') + authoring.message);
            row->setIcon(0, iconForItem(*item));
            row->setData(0, kRoleObjectID, item->objectID);
            row->setData(0, kRoleSelectable, selectable);
            row->setData(0, kRoleSearchPath, project->itemPath(item));
            if (!selectable) {
                row->setFlags(row->flags() & ~Qt::ItemIsSelectable);
                if (childRows.empty())
                    row->setFlags(row->flags() & ~Qt::ItemIsEnabled);
            }
            for (auto* childRow : childRows)
                row->addChild(childRow);
            return row;
        };

        if (!request.directChildParentGuid.isNull()) {
            if (auto* parent = ProjectManager::instance().findItemByGuid(request.directChildParentGuid))
                if (auto* row = build(parent))
                    tree->addTopLevelItem(row);
            return;
        }

        QHash<QString, QTreeWidgetItem*> buckets;
        auto bucketFor = [tree, &buckets](const QString& folder) {
            if (QTreeWidgetItem* existing = buckets.value(folder))
                return existing;
            QTreeWidgetItem* bucket = TreeRowStyle::makeBucketRow(folder);
            bucket->setFlags(bucket->flags() & ~Qt::ItemIsSelectable);
            tree->addTopLevelItem(bucket);
            buckets[folder] = bucket;
            return bucket;
        };

        const auto bindings = project->allFileBindings();
        auto addBindingsFor = [&](const QString& folder) {
            for (auto* binding : bindings) {
                if (!binding || !binding->rootItem || binding->parentFolder != folder)
                    continue;
                if (auto* row = build(binding->rootItem.get()))
                    bucketFor(folder)->addChild(row);
            }
        };

        if (!request.bucketFolders.isEmpty()) {
            for (const QString& folder : request.bucketFolders) {
                if (isUiHiddenBucket(folder))
                    continue;
                bucketFor(folder);
                addBindingsFor(folder);
            }
            return;
        }

        const QStringList standard = ProjectManager::getStandardDirectories();
        for (const QString& cat : standard) {
            if (isUiHiddenBucket(cat))
                continue;
            addBindingsFor(cat);
        }
        for (auto* binding : bindings) {
            if (!binding || standard.contains(binding->parentFolder)
                || isUiHiddenBucket(binding->parentFolder) || buckets.contains(binding->parentFolder))
                continue;
            addBindingsFor(binding->parentFolder);
        }
    }
};

class AudioAssetAdapter final : public IRefDomainAdapter
{
public:
    bool exists(uint32_t id) const override
    {
        return AssetRegistry::instance().contains(id);
    }

    QString displayName(uint32_t id) const override
    {
        const AssetInfo* asset = AssetRegistry::instance().getAsset(id);
        return asset ? assetDisplayName(*asset) : QString();
    }

    QString tooltip(uint32_t id) const override
    {
        const AssetInfo* asset = AssetRegistry::instance().getAsset(id);
        return asset ? assetTooltip(*asset) : QString();
    }

    QIcon icon(uint32_t, const QColor& tint) const override
    {
        return ThemeManager::instance().resolveIcon(
            QString::fromLatin1(ThemeAssetIds::AudioSource),
            ThemeAssetVariant{QSize(), 1.0, 0, tint}).icon;
    }

    bool matchesFilter(uint32_t, const RefFilter&) const override { return true; }

    QList<QMetaObject::Connection> connectChanged(
        QObject* ctx, std::function<void(uint32_t)> onChanged) const override
    {
        auto* registry = &AssetRegistry::instance();
        auto invoke = [cb = std::move(onChanged)](uint32_t id) { cb(id); };
        return {
            QObject::connect(registry, &AssetRegistry::assetModified, ctx, invoke),
            QObject::connect(registry, &AssetRegistry::assetUnregistered, ctx, invoke),
        };
    }

    void populate(QTreeWidget* tree, const SelectorRequest& request) const override
    {
        QList<const AssetInfo*> assets = AssetRegistry::instance().getAllAssets();
        std::sort(assets.begin(), assets.end(), [](const AssetInfo* a, const AssetInfo* b) {
            return PathUtils::normalizeToUnix(a->relativePath)
                .compare(PathUtils::normalizeToUnix(b->relativePath), Qt::CaseInsensitive) < 0;
        });

        QHash<QString, QTreeWidgetItem*> folders;
        auto folderFor = [tree, &folders](const QStringList& segments) -> QTreeWidgetItem* {
            QTreeWidgetItem* parent = nullptr;
            QString key;
            for (const QString& segment : segments) {
                if (!key.isEmpty())
                    key += QLatin1Char('/');
                key += segment;
                if (QTreeWidgetItem* existing = folders.value(key)) {
                    parent = existing;
                    continue;
                }
                QTreeWidgetItem* folder = TreeRowStyle::makeFolderRow(segment);
                folder->setFlags(folder->flags() & ~Qt::ItemIsSelectable);
                if (parent)
                    parent->addChild(folder);
                else
                    tree->addTopLevelItem(folder);
                folders.insert(key, folder);
                parent = folder;
            }
            return parent;
        };

        for (const AssetInfo* asset : assets) {
            if (!asset)
                continue;
            const uint32_t id = asset->sourceID;
            const bool selectable = !request.isSelectable || request.isSelectable(id);
            auto* row = new QTreeWidgetItem;
            row->setText(0, assetDisplayName(*asset));
            row->setIcon(0, icon(id, QColor()));
            row->setData(0, kRoleObjectID, id);
            row->setData(0, kRoleSelectable, selectable);
            if (!selectable)
                row->setFlags(row->flags() & ~(Qt::ItemIsSelectable | Qt::ItemIsEnabled));
            if (QTreeWidgetItem* folder = folderFor(assetFolderSegments(*asset)))
                folder->addChild(row);
            else
                tree->addTopLevelItem(row);
        }
    }
};

}

const IRefDomainAdapter& refDomainAdapter(RefDomain domain)
{
    static const ProjectObjectAdapter kProjectObject;
    static const AudioAssetAdapter kAudioAsset;
    switch (domain) {
    case RefDomain::ProjectObject:
        return kProjectObject;
    case RefDomain::AudioAsset:
        return kAudioAsset;
    }
    Q_UNREACHABLE_RETURN(kProjectObject);
}

}
