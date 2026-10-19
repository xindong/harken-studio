#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"
#include "Widgets/Common/ProportionalTreeWidget.h"
#include "Widgets/Common/TableItemDelegate.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFontDatabase>
#include <QHeaderView>
#include <QImage>
#include <QMouseEvent>
#include <QScopeGuard>
#include <QTimer>

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

TEST_F(ProportionalTreeWidgetTest, PinnedTrailingColumnsKeepOrderWhileOtherColumnsMove)
{
    VNX::ProportionalTreeWidget table({200, 200, 200, 200, 200});
    table.setHeaderLabels({"A", "B", "C", "D", "E"});
    table.setPinnedTrailingColumns({1, 3});
    auto* header = table.header();
    header->setSectionsMovable(true);
    header->setFirstSectionMovable(true);
    table.resize(900, 200);
    table.show();
    QCoreApplication::processEvents();
    const auto order = [&] {
        QList<int> result;
        for (int i = 0; i < header->count(); ++i) result.append(header->logicalIndex(i));
        return result;
    };
    EXPECT_EQ(order(), (QList<int>{0, 2, 4, 1, 3}));
    header->moveSection(header->visualIndex(1), 0);
    EXPECT_EQ(order(), (QList<int>{0, 2, 4, 1, 3}));
    header->moveSection(header->visualIndex(2), header->count() - 1);
    EXPECT_EQ(order(), (QList<int>{0, 4, 2, 1, 3}));
    const auto drag = [&](const QPoint& from, const QPoint& to) {
        auto* viewport = header->viewport();
        QMouseEvent press(QEvent::MouseButtonPress, from, viewport->mapToGlobal(from),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &press);
        QMouseEvent move(QEvent::MouseMove, to, viewport->mapToGlobal(to),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, to, viewport->mapToGlobal(to),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &release);
        QEventLoop loop;
        QTimer::singleShot(200, &loop, &QEventLoop::quit);
        loop.exec();
    };
    for (int column : {1, 3}) {
        drag(QPoint(header->sectionViewportPosition(column) + header->sectionSize(column) / 2,
                    header->height() / 2), QPoint(10, header->height() / 2));
        EXPECT_EQ(order(), (QList<int>{0, 4, 2, 1, 3}));
    }
    drag(QPoint(header->sectionViewportPosition(4) + header->sectionSize(4) / 2,
                header->height() / 2), QPoint(header->viewport()->width() - 10, header->height() / 2));
    EXPECT_EQ(order(), (QList<int>{0, 2, 4, 1, 3}));
    const int width = header->sectionSize(1);
    const QPoint edge(header->sectionViewportPosition(1) + width - 1, header->height() / 2);
    drag(edge, edge + QPoint(30, 0));
    EXPECT_GT(header->sectionSize(1), width);
    expectFilled(table);
    table.setColumnsHidden({4}, true);
    EXPECT_EQ(order(), (QList<int>{0, 2, 4, 1, 3}));
    table.setPinnedTrailingColumns({});
    header->moveSection(header->visualIndex(3), 0);
    EXPECT_EQ(header->logicalIndex(0), 3);
}

TEST_F(ProportionalTreeWidgetTest, ColumnDecorationsPreserveGroupsAndSelectionAcrossLayoutChanges)
{
    VNX::ProportionalTreeWidget table({400, 300, 300});
    table.setHeaderLabels({"A", "B", "C"});
    auto* group = table.createGroup("Group");
    auto* child = new QTreeWidgetItem(group, {"Child"});
    table.show();
    for (int width : {640, 920, 360}) {
        child->setSelected(false);
        table.setAccentedColumns({});
        table.setColumnSeparatorBefore(-1);
        table.resize(width, 240);
        table.setColumnsHidden({2}, width == 360);
        if (width == 920) table.header()->moveSection(table.header()->visualIndex(1), 2);
        QCoreApplication::processEvents();
        const QImage plain = table.grab().toImage();
        table.setAccentedColumns({1});
        table.setColumnSeparatorBefore(1);
        QCoreApplication::processEvents();
        const int x = table.viewport()->x() + table.header()->sectionViewportPosition(1) - 1;
        const QImage image = table.grab().toImage();
        const QRect groupRect = table.visualItemRect(group).translated(table.viewport()->pos());
        EXPECT_EQ(image.copy(groupRect), plain.copy(groupRect));
        for (int y = table.header()->y(); y <= table.viewport()->geometry().bottom(); ++y) {
            if (y >= groupRect.top() && y <= groupRect.bottom()) continue;
            ASSERT_EQ(image.pixelColor(x, y), VNX::ThemeManager::instance().colors().borderPrimary)
                << "width " << width << ", row " << y;
        }
        const int normalX = table.viewport()->x() + table.header()->sectionViewportPosition(0)
            + table.columnWidth(0) - 8;
        const int accentedX = x + table.columnWidth(1) - 8;
        const int headerY = table.header()->geometry().center().y();
        const int rowY = table.viewport()->y() + table.visualItemRect(child).bottom() - 5;
        const int emptyY = table.viewport()->geometry().bottom() - 4;
        EXPECT_NE(image.pixelColor(normalX, headerY), image.pixelColor(accentedX, headerY));
        EXPECT_NE(image.pixelColor(normalX, rowY), image.pixelColor(accentedX, rowY));
        EXPECT_EQ(image.pixelColor(accentedX, emptyY), image.pixelColor(accentedX, rowY));
        EXPECT_EQ(image.pixelColor(normalX, emptyY), image.pixelColor(normalX, rowY));
        child->setSelected(true);
        QCoreApplication::processEvents();
        const QImage selected = table.grab().toImage();
        EXPECT_EQ(selected.pixelColor(normalX, rowY), selected.pixelColor(accentedX, rowY));
        auto* hit = table.childAt(x, table.header()->geometry().center().y());
        ASSERT_NE(hit, nullptr);
        EXPECT_TRUE(table.header() == hit || table.header()->isAncestorOf(hit));
    }
    const int accentedX = table.viewport()->x() + table.header()->sectionViewportPosition(1) + 8;
    const int emptyY = table.viewport()->geometry().bottom() - 4;
    const QColor background = table.grab().toImage().pixelColor(accentedX, emptyY);
    table.clear();
    table.setColumnSeparatorBefore(-1);
    QCoreApplication::processEvents();
    const QImage empty = table.grab().toImage();
    EXPECT_EQ(empty.pixelColor(accentedX, table.viewport()->y() + 4), background);
    EXPECT_EQ(empty.pixelColor(accentedX, emptyY), background);
}

TEST_F(ProportionalTreeWidgetTest, RowProgressPreservesGroupsAndTrailingCellsAcrossLayoutAndSelection)
{
    VNX::ProportionalTreeWidget table({300, 250, 250, 200});
    table.setHeaderLabels({"A", "B", "C", "Status"});
    table.setPinnedTrailingColumns({3});
    table.setAccentedColumns({3});
    table.setColumnSeparatorBefore(3);
    auto* group = table.createGroup("Group");
    auto* child = new QTreeWidgetItem(group, {"Task", "Value"});
    child->setCheckState(0, Qt::Checked);
    table.show();
    for (int width : {700, 1000, 380}) {
        table.resize(width, 240);
        table.setColumnsHidden({1}, width == 380);
        if (width == 1000) table.header()->moveSection(table.header()->visualIndex(2), 0);
        for (bool selected : {false, true}) {
            SCOPED_TRACE(::testing::Message() << "width " << width << ", selected " << selected);
            child->setSelected(selected);
            child->setData(0, VNX::TableRowProgressRole, {});
            table.setRowProgressEndColumn(-1);
            QCoreApplication::processEvents();
            const QImage baseline = table.viewport()->grab().toImage();
            table.setRowProgressEndColumn(3);
            QCoreApplication::processEvents();
            EXPECT_EQ(table.viewport()->grab().toImage(), baseline);
            const QRect row = table.visualItemRect(child);
            const QRect groupRect = table.visualItemRect(group);
            const int boundary = table.header()->sectionViewportPosition(3);
            const int y = row.top() + 2;
            const int inside = boundary / 4;
            const int outside = boundary * 3 / 4;
            child->setData(0, VNX::TableRowProgressRole, 50);
            child->setData(0, VNX::TableRowProgressStateRole,
                static_cast<int>(VNX::TableRowProgressState::Active));
            table.viewport()->update();
            QCoreApplication::processEvents();
            const QImage progress = table.viewport()->grab().toImage();
            EXPECT_NE(progress.pixelColor(inside, y), baseline.pixelColor(inside, y));
            EXPECT_EQ(progress.pixelColor(outside, y), baseline.pixelColor(outside, y));
            EXPECT_EQ(progress.copy(groupRect), baseline.copy(groupRect));
            const QRect trailing(boundary, row.top(), table.viewport()->width() - boundary, row.height());
            EXPECT_EQ(progress.copy(trailing), baseline.copy(trailing));
            EXPECT_EQ(progress.copy(0, row.bottom(), boundary, 1), baseline.copy(0, row.bottom(), boundary, 1));
            if (selected)
                EXPECT_EQ(progress.pixelColor(1, y), VNX::ThemeManager::instance().colors().accent);
            child->setData(0, VNX::TableRowProgressStateRole,
                static_cast<int>(VNX::TableRowProgressState::Failed));
            table.viewport()->update();
            QCoreApplication::processEvents();
            const QImage failed = table.viewport()->grab().toImage();
            EXPECT_NE(failed.pixelColor(inside, y), progress.pixelColor(inside, y));
            EXPECT_EQ(failed.pixelColor(outside, y), baseline.pixelColor(outside, y));
            child->setData(0, VNX::TableRowProgressRole, 100);
            child->setData(0, VNX::TableRowProgressStateRole,
                static_cast<int>(VNX::TableRowProgressState::Complete));
            table.viewport()->update();
            QCoreApplication::processEvents();
            const QImage complete = table.viewport()->grab().toImage();
            EXPECT_NE(complete.pixelColor(outside, y), baseline.pixelColor(outside, y));
            EXPECT_EQ(complete.copy(trailing), baseline.copy(trailing));
            child->setData(0, VNX::TableRowProgressRole, 0);
            table.viewport()->update();
            QCoreApplication::processEvents();
            EXPECT_EQ(table.viewport()->grab().toImage(), baseline);
        }
    }
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
