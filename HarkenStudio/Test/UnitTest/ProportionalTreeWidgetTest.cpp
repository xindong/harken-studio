#include "Core/QtControlSkin.h"
#include "Widgets/Common/ProportionalTreeWidget.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QFontDatabase>
#include <QHeaderView>
#include <QImage>
#include <QMouseEvent>
#include <QScopeGuard>

namespace {

class ProportionalTreeWidgetTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        previousStyle_ = qApp->styleSheet();
        qApp->setStyleSheet(VNX::QtControlSkin::current().applicationStyleSheet());
    }

    void TearDown() override { qApp->setStyleSheet(previousStyle_); }

    static void expectFilled(const VNX::ProportionalTreeWidget& table)
    {
        int total = 0;
        for (int column = 0; column < table.columnCount(); ++column) {
            if (table.isColumnHidden(column)) continue;
            EXPECT_GE(table.columnWidth(column), table.header()->minimumSectionSize());
            total += table.columnWidth(column);
        }
        EXPECT_NEAR(total, table.viewport()->width(), 2);
    }

    QString previousStyle_;
};

TEST_F(ProportionalTreeWidgetTest, DraggedWidthsRemainProportionalAfterResizeAndRowRefresh)
{
    VNX::ProportionalTreeWidget table({500, 500});
    table.setHeaderLabels({"Range", "Sample rate"});
    new QTreeWidgetItem(&table, {"64 B", "22050 Hz"});
    table.resize(640, 200);
    table.show();
    QCoreApplication::processEvents();
    const int before = table.columnWidth(0);
    auto* header = table.header();
    const QPoint edge(header->sectionViewportPosition(0) + before - 1, header->height() / 2);
    auto* viewport = header->viewport();
    const QPoint target = edge + QPoint(80, 0);
    QMouseEvent press(QEvent::MouseButtonPress, edge, viewport->mapToGlobal(edge), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(viewport, &press);
    QMouseEvent move(QEvent::MouseMove, target, viewport->mapToGlobal(target), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(viewport, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, target, viewport->mapToGlobal(target), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(viewport, &release);
    QCoreApplication::processEvents();
    EXPECT_GT(table.columnWidth(0), before + 60);
    expectFilled(table);
    const double ratio = double(table.columnWidth(0)) / table.viewport()->width();
    for (int width : {1000, 320, 640}) {
        table.resize(width, 200);
        QCoreApplication::processEvents();
        expectFilled(table);
        EXPECT_NEAR(table.columnWidth(0), ratio * table.viewport()->width(), 2);
    }
    table.clear();
    new QTreeWidgetItem(&table, {"12 MB", "44100 Hz"});
    table.setEnabled(false);
    table.setEnabled(true);
    QCoreApplication::processEvents();
    EXPECT_NEAR(table.columnWidth(0), ratio * table.viewport()->width(), 2);
}

TEST_F(ProportionalTreeWidgetTest, DragStopsBeforeOtherVisibleColumnsLeaveTheViewport)
{
    VNX::ProportionalTreeWidget table({600, 250, 150});
    table.resize(900, 200);
    table.show();
    QCoreApplication::processEvents();
    table.header()->resizeSection(0, 5000);
    QCoreApplication::processEvents();
    expectFilled(table);
    EXPECT_LE(table.columnWidth(0), table.viewport()->width() - 2 * table.header()->minimumSectionSize());
    table.header()->resizeSection(0, 60);
    QCoreApplication::processEvents();
    EXPECT_NEAR(table.columnWidth(0), 60, 2);
    expectFilled(table);
}

TEST_F(ProportionalTreeWidgetTest, ContinuousHeaderDragLeavesUnrelatedColumnsStationary)
{
    VNX::ProportionalTreeWidget table({280, 70, 90, 150, 80, 80, 120, 60, 80});
    table.resize(1123, 200);
    table.show();
    QCoreApplication::processEvents();
    auto* header = table.header();
    auto* viewport = header->viewport();
    QList<int> initial;
    for (int i = 0; i < table.columnCount(); ++i) initial.append(table.columnWidth(i));
    const QPoint edge(header->sectionViewportPosition(3) + initial[3] - 1, header->height() / 2);
    QMouseEvent press(QEvent::MouseButtonPress, edge, viewport->mapToGlobal(edge),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(viewport, &press);
    for (int step = 0; step <= 120; ++step) {
        const int offset = step <= 40 ? step : step <= 80 ? 80 - step : step - 80;
        const QPoint point = edge + QPoint(offset, 0);
        QMouseEvent move(QEvent::MouseMove, point, viewport->mapToGlobal(point),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &move);
        QCoreApplication::processEvents();
        for (int i = 0; i < initial.size(); ++i) {
            const int expected = initial[i] + (i == 3 ? offset : i == 4 ? -offset : 0);
            ASSERT_EQ(table.columnWidth(i), expected) << "step " << step << ", column " << i;
        }
        EXPECT_EQ(header->sectionViewportPosition(3) + table.columnWidth(3) - 1, point.x());
        expectFilled(table);
    }
    const QPoint target = edge + QPoint(40, 0);
    QMouseEvent release(QEvent::MouseButtonRelease, target, viewport->mapToGlobal(target),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(viewport, &release);
}

TEST_F(ProportionalTreeWidgetTest, HiddenAndReorderedColumnsKeepTheirOwnShares)
{
    VNX::ProportionalTreeWidget table({400, 250, 150, 200});
    table.resize(1000, 200);
    table.show();
    QCoreApplication::processEvents();
    const QList<int> initial{table.columnWidth(0), table.columnWidth(1), table.columnWidth(2), table.columnWidth(3)};
    table.setColumnsHidden({1, 3}, true);
    QCoreApplication::processEvents();
    expectFilled(table);
    EXPECT_NEAR(double(table.columnWidth(0)) / table.columnWidth(2), 400.0 / 150, 0.02);
    table.setColumnsHidden({1, 3}, false);
    QCoreApplication::processEvents();
    for (int column = 0; column < 4; ++column) EXPECT_NEAR(table.columnWidth(column), initial[column], 2);
    table.header()->setSectionsMovable(true);
    table.header()->moveSection(1, 3);
    QCoreApplication::processEvents();
    expectFilled(table);
    const int firstWidth = table.columnWidth(0);
    table.header()->resizeSection(2, table.columnWidth(2) + 50);
    QCoreApplication::processEvents();
    EXPECT_NEAR(table.columnWidth(0), firstWidth, 2);
    expectFilled(table);
    const double ratio = double(table.columnWidth(2)) / table.viewport()->width();
    table.resize(700, 200);
    QCoreApplication::processEvents();
    EXPECT_NEAR(table.columnWidth(2), ratio * table.viewport()->width(), 2);
    expectFilled(table);
}

TEST_F(ProportionalTreeWidgetTest, SharedHeaderActuallyRendersMixedCaseLabels)
{
#ifdef Q_OS_WIN
    const int fontId = QFontDatabase::addApplicationFont(QDir(qEnvironmentVariable("WINDIR")).filePath("Fonts/segoeui.ttf"));
    ASSERT_GE(fontId, 0);
    const auto removeFont = qScopeGuard([fontId]() { QFontDatabase::removeApplicationFont(fontId); });
#endif
    VNX::ProportionalTreeWidget table({500, 500});
#ifdef Q_OS_WIN
    table.setFont(QFont(QFontDatabase::applicationFontFamilies(fontId).first()));
#endif
    table.setHeaderLabels({"Range", "Sample rate"});
    table.resize(500, 200);
    table.show();
    QCoreApplication::processEvents();
    auto* header = table.header();
    const auto render = [header]() {
        QImage image(header->size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        header->render(&image);
        return image;
    };
    const auto mixedCase = render();
    table.setHeaderLabels({"RANGE", "SAMPLE RATE"});
    QCoreApplication::processEvents();
    const auto upperCase = render();
    EXPECT_FALSE(mixedCase == upperCase);
    EXPECT_TRUE(table.styleSheet().isEmpty());
    EXPECT_TRUE(header->styleSheet().isEmpty());
}

}
