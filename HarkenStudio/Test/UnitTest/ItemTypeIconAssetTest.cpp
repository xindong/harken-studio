#include "Core/ItemTypeCatalog.h"
#include "Core/ThemeAssetCatalog.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QSet>
#include <QString>

namespace Catalog = VNX::ItemTypeCatalog;

TEST(ItemTypeIconAssets, CatalogEntriesHaveBuiltinResources)
{
    const QString studioDir = QStringLiteral(HARKEN_STUDIO_SOURCE_DIR);

    QFile qrc(studioDir + QStringLiteral("/resources.qrc"));
    ASSERT_TRUE(qrc.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString qrcText = QString::fromUtf8(qrc.readAll());
    QSet<QString> logicalIds;

    for (const auto& entry : VNX::themeAssetCatalog()) {
        EXPECT_FALSE(entry.logicalId.isEmpty());
        EXPECT_FALSE(logicalIds.contains(entry.logicalId))
            << entry.logicalId.toStdString();
        logicalIds.insert(entry.logicalId);
        if (entry.fallback != VNX::ThemeBuiltinAssetFallback::File)
            continue;
        EXPECT_FALSE(entry.builtinPath.isEmpty());
        const QString fileName = entry.builtinPath.section(QLatin1Char('/'), -1);
        EXPECT_TRUE(qrcText.contains(fileName)) << fileName.toStdString();
    }

    for (VNX::ProjectItem::Type type : Catalog::allTypes()) {
        const QString logicalId = QString::fromLatin1(
            Catalog::iconSpecOf(type).logicalId);
        EXPECT_NE(VNX::findThemeAssetCatalogEntry(logicalId), nullptr)
            << logicalId.toStdString();
    }
}

