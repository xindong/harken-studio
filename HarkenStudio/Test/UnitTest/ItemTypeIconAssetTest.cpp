#include "Core/ItemTypeCatalog.h"
#include "Core/ThemeAssetCatalog.h"

#include <gtest/gtest.h>

#include <QSet>
#include <QString>

namespace Catalog = VNX::ItemTypeCatalog;

TEST(ItemTypeIconAssets, CatalogEntriesHaveUniqueResolvableIdentities)
{
    QSet<QString> logicalIds;

    for (const auto& entry : VNX::themeAssetCatalog()) {
        EXPECT_FALSE(entry.logicalId.isEmpty());
        EXPECT_FALSE(logicalIds.contains(entry.logicalId))
            << entry.logicalId.toStdString();
        logicalIds.insert(entry.logicalId);
    }

    for (VNX::ProjectItem::Type type : Catalog::allTypes()) {
        const QString logicalId = QString::fromLatin1(
            Catalog::iconSpecOf(type).logicalId);
        EXPECT_NE(VNX::findThemeAssetCatalogEntry(logicalId), nullptr)
            << logicalId.toStdString();
    }
}

