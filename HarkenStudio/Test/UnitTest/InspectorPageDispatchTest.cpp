#include "Widgets/Inspector/InspectorPanel_p.h"
#include "Core/ItemTypeCatalog.h"
#include "Core/ProjectTypes.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QString>

using VNX::InspectorPanelPrivate;
using VNX::ProjectItem;

namespace {

struct DispatchRow {
    ProjectItem::Type type;
    InspectorPanelPrivate::Page page;
};

} // namespace

TEST(InspectorPageDispatch, PageForItemTypeGoldenTable)
{
    const DispatchRow rows[] = {
        { ProjectItem::Type::None,      InspectorPanelPrivate::PageEmpty },
        { ProjectItem::Type::Sound,       InspectorPanelPrivate::PageSound },
        { ProjectItem::Type::Container,   InspectorPanelPrivate::PageContainer },
        { ProjectItem::Type::Track,       InspectorPanelPrivate::PageTrack },
        { ProjectItem::Type::Piece,       InspectorPanelPrivate::PagePiece },
        { ProjectItem::Type::Layer,       InspectorPanelPrivate::PageLayer },
        { ProjectItem::Type::Bus,         InspectorPanelPrivate::PageBus },
        { ProjectItem::Type::Action,      InspectorPanelPrivate::PageAction },
        { ProjectItem::Type::Variable,    InspectorPanelPrivate::PageVariable },
        { ProjectItem::Type::Attenuation, InspectorPanelPrivate::PageAttenuation },
    };
    for (const DispatchRow& row : rows)
        EXPECT_EQ(VNX::pageForItemType(row.type), row.page)
            << VNX::ItemTypeCatalog::canonicalName(row.type).toStdString();

    EXPECT_EQ(VNX::pageForItemType(static_cast<ProjectItem::Type>(255)),
              InspectorPanelPrivate::PageEmpty);
}

TEST(InspectorPageDispatch, PageEnumKeepsLegacyOrderAndAppendsNewPages)
{
    EXPECT_EQ(static_cast<int>(InspectorPanelPrivate::PageEmpty), 0);
    EXPECT_EQ(static_cast<int>(InspectorPanelPrivate::PageAction), 1);
    EXPECT_EQ(static_cast<int>(InspectorPanelPrivate::PageAttenuation), 2);
    EXPECT_EQ(static_cast<int>(InspectorPanelPrivate::PageSound), 3);
    EXPECT_EQ(static_cast<int>(InspectorPanelPrivate::PageLayer), 4);
    EXPECT_EQ(static_cast<int>(InspectorPanelPrivate::PageBus), 5);
    EXPECT_EQ(static_cast<int>(InspectorPanelPrivate::PageContainer), 6);
    EXPECT_EQ(static_cast<int>(InspectorPanelPrivate::PageTrack), 7);
    EXPECT_EQ(static_cast<int>(InspectorPanelPrivate::PagePiece), 8);
    EXPECT_EQ(static_cast<int>(InspectorPanelPrivate::PageVariable), 9);
    EXPECT_EQ(static_cast<int>(InspectorPanelPrivate::PageCount), 10);
}

TEST(InspectorPageDispatch, InspectorTitleWordFaceComesFromCatalog)
{
    EXPECT_EQ(VNX::ItemTypeCatalog::displayName(ProjectItem::Type::Layer),
              QStringLiteral("Layer"));

    const QString sourceDir = QStringLiteral(HARKEN_STUDIO_SOURCE_DIR)
        + QStringLiteral("/Source");
    QDirIterator it(sourceDir, {QStringLiteral("*.cpp"), QStringLiteral("*.h")},
                    QDir::Files, QDirIterator::Subdirectories);
    int scanned = 0;
    while (it.hasNext()) {
        QFile f(it.next());
        ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString text = QString::fromUtf8(f.readAll());
        EXPECT_FALSE(text.contains(QStringLiteral("Audio Layer")))
            << it.filePath().toStdString();
        ++scanned;
    }
    EXPECT_GT(scanned, 0);
}
