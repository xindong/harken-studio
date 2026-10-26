#include "Widgets/Common/TableWidget.h"
#include "Widgets/Common/ProportionalTreeWidget.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QVBoxLayout>

TEST(TableWidgetTest, ManagedItemViewIsDestroyedWithItsTable)
{
    QPointer<VNX::ProportionalTreeWidget> owned;
    {
        VNX::TableWidget table;
        owned = table.createItemView(QStringLiteral("ManagedItems"),
            {"Name", "Value"}, {600, 400});
        ASSERT_FALSE(owned.isNull());
    }
    EXPECT_TRUE(owned.isNull());
}

TEST(TableWidgetTest, RowRemovalDestroysAssociatedDetails)
{
    VNX::TableWidget table;
    table.setColumns(QStringLiteral("Details"), {
        {QStringLiteral("Number"), QStringLiteral("#"), 0},
        {QStringLiteral("Name"), QStringLiteral("Name"), 700},
        {QStringLiteral("Weight"), QStringLiteral("Weight"), 300}
    });
    auto* row = new QWidget(table.rowsWidget());
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(0);
    const QList<QWidget*> cells{
        table.createRowNumber(row, 0), new QLabel(QStringLiteral("Name"), row),
        new QLabel(QStringLiteral("Weight"), row)};
    for (auto* cell : cells) rowLayout->addWidget(cell);
    auto* details = new QWidget(table.rowsWidget());
    new QVBoxLayout(details);
    table.registerRow(row, cells, details);
    table.rowsLayout()->addWidget(row);
    table.rowsLayout()->addWidget(details);
    const QPointer<QWidget> detailGuard(details);
    ASSERT_FALSE(detailGuard.isNull());
    delete row;
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_TRUE(detailGuard.isNull());
}
