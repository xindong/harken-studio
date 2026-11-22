#include "Core/ItemTypeCatalog.h"
#include "Core/ThemeAssetCatalog.h"

#include <gtest/gtest.h>

#include <QSet>
#include <QString>

namespace Catalog = harken::studio::ItemTypeCatalog;

TEST(ItemTypeIconAssets, CatalogEntriesHaveUniqueResolvableIdentities)
{
    QSet<QString> logicalIds;

    for (const auto& entry : harken::studio::themeAssetCatalog()) {
        EXPECT_FALSE(entry.logicalId.isEmpty());
        EXPECT_FALSE(logicalIds.contains(entry.logicalId))
            << entry.logicalId.toStdString();
        logicalIds.insert(entry.logicalId);
    }

    for (harken::studio::ProjectItem::Type type : Catalog::allTypes()) {
        const QString logicalId = QString::fromLatin1(
            Catalog::iconSpecOf(type).logicalId);
        EXPECT_NE(harken::studio::findThemeAssetCatalogEntry(logicalId), nullptr)
            << logicalId.toStdString();
    }
}

