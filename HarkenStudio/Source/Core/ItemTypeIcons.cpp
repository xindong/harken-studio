
#include "ItemTypeIcons.h"

#include "ItemTypeCatalog.h"
#include "ThemeAssetCatalog.h"
#include "ThemeManager.h"

namespace VNX {

QIcon iconForItemType(ProjectItem::Type type)
{
    return ThemeManager::instance()
        .resolveIcon(QString::fromLatin1(
            ItemTypeCatalog::iconSpecOf(type).logicalId))
        .icon;
}

QIcon iconForItemType(ProjectItem::Type type, const QColor& tint)
{
    return ThemeManager::instance()
        .resolveIcon(QString::fromLatin1(
            ItemTypeCatalog::iconSpecOf(type).logicalId),
            ThemeAssetVariant{QSize(), 1.0, 0, tint})
        .icon;
}

namespace {

QIcon resolveItemIcon(const char* logicalId, const QColor& tint = {})
{
    ThemeAssetVariant variant;
    variant.tint = tint;
    return ThemeManager::instance()
        .resolveIcon(QString::fromLatin1(logicalId), variant)
        .icon;
}

}

QIcon iconForItem(const ProjectItem& item)
{
    return resolveItemIcon(iconLogicalIdForItem(item));
}

QIcon iconForItem(const ProjectItem& item, const QColor& tint)
{
    return resolveItemIcon(iconLogicalIdForItem(item), tint);
}

}
